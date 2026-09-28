// Temporal anti-aliasing for The Darkness (V397; graphics.temporal_aa, default
// off). See rex/graphics/temporal_aa_policy.h for the camera, reprojection,
// cut and jitter rules and docs/NEXT_FEATURES_PLAN.md section 1 for the
// evidence (street frame anatomy, camera constants, depth by key).
//
// Per frame: the depth-writing scene draws vote for the camera (their c0..c7),
// the first depth resolve gives the resolved scene depth, the scene draws are
// rasterized with a sub-pixel viewport jitter (only in frames that follow a
// resolved frame, so menus never shake), and right before the title's final
// composite samples its scene colour, a resolve replaces that scene colour:
// the built-in compute TAA (reprojected history, neighbourhood clamp), or an
// upscaler on the same inputs (NVIDIA DLSS in DLAA mode, AMD FSR 3.1 native
// AA, Intel XeSS native AA) with camera motion vectors computed from the
// resolved depth. The SDK DLLs are loaded only when their method is chosen;
// any failure falls back to the built-in resolve.

#include <rex/cvar.h>
#include <rex/graphics/d3d12/command_processor.h>
#include <rex/graphics/temporal_aa_policy.h>
#include <rex/ui/d3d12/d3d12_util.h>
#include <rex/ui/d3d12/streamline_bridge.h>
#include <rex/ui/frame_latency.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>

#if REX_HAVE_DLSS
#include <nvsdk_ngx_helpers.h>
#endif
#if REX_HAVE_FSR
#include <ffx_api/dx12/ffx_api_dx12.h>
#include <ffx_api/ffx_api.h>
#include <ffx_api/ffx_upscale.h>
#endif
#if REX_HAVE_XESS
#include <xess/xess.h>
#include <xess/xess_d3d12.h>
#endif

// Player setting ([graphics] temporal_aa in the PC config).
REXCVAR_DEFINE_STRING(graphics_temporal_aa, "off", "Graphics",
                      "Temporal anti-aliasing: off, taa (built-in), dlss (NVIDIA DLAA), fsr "
                      "(AMD FSR 3.1 native AA) or xess (Intel XeSS native AA); the built-in TAA "
                      "is used when the chosen upscaler is unavailable")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
// Test overrides (scripts): enable with a method regardless of the setting.
REXCVAR_DEFINE_BOOL(gpu_temporal_aa, false, "GPU/Experimental",
                    "Temporal anti-aliasing of the scene (jittered scene draws, a history "
                    "resolve before the final composite)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_STRING(gpu_temporal_aa_method, "taa", "GPU/Experimental",
                      "Temporal AA resolve with gpu_temporal_aa: taa, dlss, fsr or xess")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(gpu_dlss_preset, 0, "GPU/Experimental",
                     "DLSS render preset hint: 0 = NVIDIA default, 10 = J, 11 = K, 12 = L, 13 = M")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_DOUBLE(gpu_temporal_aa_history_weight, 0.9, "GPU/Experimental",
                      "Temporal AA: weight of the reprojected history (0 = current frame only)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(gpu_temporal_aa_debug, 0, "GPU/Experimental",
                     "Temporal AA test mode: 1 = resolve without writing the scene colour back, "
                     "2 = upscalers with negated jitter offsets")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(gpu_temporal_aa_upscaler_linear, false, "GPU/Experimental",
                    "Upscalers get linear light (the title's square-root encoded scene colour "
                    "squared) instead of the scene colour as stored")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace rex::graphics::d3d12 {

namespace shaders {
#include "../shaders/bytecode/d3d12_5_1/temporal_aa_resolve_cs.h"
}  // namespace shaders

namespace {
// The title's final composite: VS 4FA9486610B42A92 with PS A59B41D0BD79484B,
// or 22FC55CE134777AC when the motion-blur-off shader pack replaces it.
constexpr uint64_t kFinalCompositeVertexShader = UINT64_C(0x4FA9486610B42A92);
constexpr uint64_t kFinalCompositePixelShaders[] = {UINT64_C(0xA59B41D0BD79484B),
                                                    UINT64_C(0x22FC55CE134777AC)};
constexpr uint32_t kSceneColorGuestFormat = 26;  // k_16_16_16_16 (x16 exponent)

struct TemporalAaConstants {
  float reproject[16];
  uint32_t size[2];
  float size_inv[2];
  float jitter[2];
  float history_weight;
  uint32_t flags;
};
static_assert(sizeof(TemporalAaConstants) % 4 == 0);
constexpr uint32_t kTemporalAaFlagHistory = 1;
// Write the upscaler inputs (motion vectors to u0, linear colour to u2).
constexpr uint32_t kTemporalAaFlagUpscalerInputs = 2;
// Convert the upscaler output (t2) back into the scene colour encoding (u1).
constexpr uint32_t kTemporalAaFlagUpscalerOutput = 4;
// Upscaler colour in linear light (the scene colour squared) instead of the
// title's square-root encoding.
constexpr uint32_t kTemporalAaFlagLinearUpscalerColor = 8;
// With kTemporalAaFlagUpscalerInputs: motion vectors only (Frame Generation).
constexpr uint32_t kTemporalAaFlagMotionOnly = 16;

// The street camera (phase 1 captures): reversed depth, near ~1.8, far
// ~22,500 world units; FSR uses the planes only for its depth heuristics.
constexpr float kCameraNear = 1.8f;
constexpr float kCameraFar = 22500.0f;

enum class UpscalerKind : uint32_t { kDlss, kFsr, kXess };

const char* UpscalerName(UpscalerKind kind) {
  switch (kind) {
    case UpscalerKind::kDlss:
      return "dlss";
    case UpscalerKind::kFsr:
      return "fsr";
    case UpscalerKind::kXess:
      return "xess";
  }
  return "?";
}

// One upscaler evaluation, recorded into the deferred command stream and run
// on the thread that executes it.
struct UpscalerCall {
  void* upscaler;
  ID3D12Resource* color;
  ID3D12Resource* depth;
  ID3D12Resource* motion;
  ID3D12Resource* output;
  uint32_t width;
  uint32_t height;
  float jitter_x;
  float jitter_y;
  float frame_time_ms;
  float fov_y;
  int32_t reset;
};

std::filesystem::path ExecutableDirectory() {
  wchar_t module_path[MAX_PATH];
  const DWORD length = GetModuleFileNameW(nullptr, module_path, MAX_PATH);
  if (!length || length >= MAX_PATH) return {};
  return std::filesystem::path(module_path).parent_path();
}

// SDK DLLs load only from the game's own folder (never the search path).
HMODULE LoadSdkLibrary(const wchar_t* name) {
  const std::filesystem::path directory = ExecutableDirectory();
  if (directory.empty()) return nullptr;
  const std::filesystem::path path = directory / name;
  return LoadLibraryExW(path.c_str(), nullptr,
                        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
}

#if REX_HAVE_DLSS
// NGX project identifier of this build (custom engine).
constexpr char kDlssProjectId[] = "6d2c8f4e-3b7a-4e1d-9a5f-2c0e7b1d4a93";
#endif

#if REX_HAVE_FSR
FfxApiResource FfxTexture(ID3D12Resource* resource, uint32_t format, uint32_t width,
                          uint32_t height, bool unordered_access) {
  FfxApiResource out = {};
  out.resource = resource;
  out.description.type = FFX_API_RESOURCE_TYPE_TEXTURE2D;
  out.description.format = format;
  out.description.width = width;
  out.description.height = height;
  out.description.depth = 1;
  out.description.mipCount = 1;
  out.description.flags = FFX_API_RESOURCE_FLAGS_NONE;
  out.description.usage =
      unordered_access ? FFX_API_RESOURCE_USAGE_UAV : FFX_API_RESOURCE_USAGE_READ_ONLY;
  out.state = unordered_access ? FFX_API_RESOURCE_STATE_UNORDERED_ACCESS
                               : FFX_API_RESOURCE_STATE_COMPUTE_READ;
  return out;
}

void FfxMessage(uint32_t type, const wchar_t* message) {
  if (type != FFX_API_MESSAGE_TYPE_ERROR || !message) return;
  std::fprintf(stderr, "REX_TEMPORAL_AA fsr_message %ls\n", message);
  std::fflush(stderr);
}
#endif  // REX_HAVE_FSR
}  // namespace

struct D3D12CommandProcessor::TemporalAaUpscaler {
  UpscalerKind kind = UpscalerKind::kDlss;
  uint32_t width = 0;
  uint32_t height = 0;
  // Typed copy of the resolved depth (read state NON_PIXEL_SHADER_RESOURCE)
  // and the camera motion vectors. The linear input colour and the output are
  // the built-in resolve's two R16G16B16A16_FLOAT history textures.
  Microsoft::WRL::ComPtr<ID3D12Resource> depth;
  Microsoft::WRL::ComPtr<ID3D12Resource> motion;
  // Set by the executing thread when an evaluation fails: the built-in
  // resolve takes over from the next frame.
  std::atomic<bool> failed{false};
  bool fallback = false;
  uint64_t evaluations = 0;
  std::chrono::steady_clock::time_point last_resolve{};
#if REX_HAVE_DLSS
  bool ngx_initialized = false;
  NVSDK_NGX_Parameter* ngx_parameters = nullptr;
  NVSDK_NGX_Handle* ngx_feature = nullptr;
#endif
#if REX_HAVE_FSR
  HMODULE ffx_module = nullptr;
  PfnFfxCreateContext ffx_create_context = nullptr;
  PfnFfxDestroyContext ffx_destroy_context = nullptr;
  PfnFfxDispatch ffx_dispatch = nullptr;
  ffxContext ffx_context = nullptr;
#endif
#if REX_HAVE_XESS
  HMODULE xess_module = nullptr;
  decltype(&xessD3D12CreateContext) xess_create_context = nullptr;
  decltype(&xessD3D12Init) xess_init = nullptr;
  decltype(&xessD3D12Execute) xess_execute = nullptr;
  decltype(&xessDestroyContext) xess_destroy_context = nullptr;
  xess_context_handle_t xess_context = nullptr;
#endif
};

void D3D12CommandProcessor::TemporalAaInitialize() {
  const std::string setting = REXCVAR_GET(graphics_temporal_aa);
  const bool from_setting = setting == "taa" || setting == "dlss" || setting == "fsr" ||
                            setting == "xess";
  temporal_aa_enabled_ = from_setting || REXCVAR_GET(gpu_temporal_aa);
  temporal_aa_jitter_draw_ = false;
  temporal_aa_jitter_[0] = temporal_aa_jitter_[1] = 0.0f;
  // NVIDIA DLSS Frame Generation (development test) without temporal AA: the
  // same frame tracking for its inputs only.
  temporal_aa_motion_only_ =
      !temporal_aa_enabled_ && ui::d3d12::streamline::FrameGenerationActive();
  if (temporal_aa_motion_only_) {
    temporal_aa_enabled_ = true;
    temporal_aa_frame_ = std::make_unique<TemporalAaFrame>();
    std::fprintf(stderr, "REX_TEMPORAL_AA enabled=1 method=motion_only (frame generation)\n");
    std::fflush(stderr);
    return;
  }
  if (!temporal_aa_enabled_) return;
  temporal_aa_frame_ = std::make_unique<TemporalAaFrame>();
  const std::string method = from_setting ? setting : REXCVAR_GET(gpu_temporal_aa_method);
  bool upscaler = false;
  if (method == "dlss") {
    upscaler = TemporalAaUpscalerInitialize(uint32_t(UpscalerKind::kDlss));
  } else if (method == "fsr") {
    upscaler = TemporalAaUpscalerInitialize(uint32_t(UpscalerKind::kFsr));
  } else if (method == "xess") {
    upscaler = TemporalAaUpscalerInitialize(uint32_t(UpscalerKind::kXess));
  }
  std::fprintf(stderr,
               "REX_TEMPORAL_AA enabled=1 method=%s requested=%s history_weight=%.2f "
               "jitter_phases=%u debug=%d\n",
               upscaler ? UpscalerName(temporal_aa_upscaler_->kind) : "taa", method.c_str(),
               REXCVAR_GET(gpu_temporal_aa_history_weight), temporal_aa::kJitterPhases,
               REXCVAR_GET(gpu_temporal_aa_debug));
  std::fflush(stderr);
}

bool D3D12CommandProcessor::TemporalAaUpscalerInitialize(uint32_t kind_value) {
  const UpscalerKind kind = UpscalerKind(kind_value);
  auto upscaler = std::make_unique<TemporalAaUpscaler>();
  upscaler->kind = kind;
  switch (kind) {
    case UpscalerKind::kDlss: {
#if REX_HAVE_DLSS
      const ui::d3d12::D3D12Provider& provider = GetD3D12Provider();
      // NGX writes its logs next to the game's own (logs\ beside the game).
      std::filesystem::path data_path = ExecutableDirectory();
      std::error_code error;
      data_path = data_path.empty() ? std::filesystem::temp_directory_path(error)
                                    : data_path / L"logs";
      std::filesystem::create_directories(data_path, error);
      NVSDK_NGX_Result result = NVSDK_NGX_D3D12_Init_with_ProjectID(
          kDlssProjectId, NVSDK_NGX_ENGINE_TYPE_CUSTOM, "1.0", data_path.c_str(),
          provider.GetDevice());
      if (NVSDK_NGX_FAILED(result)) {
        std::fprintf(stderr, "REX_TEMPORAL_AA dlss_unavailable init=0x%08X\n",
                     static_cast<unsigned>(result));
        return false;
      }
      upscaler->ngx_initialized = true;
      int available = 0;
      int needs_driver = 0;
      NVSDK_NGX_Parameter* capabilities = nullptr;
      result = NVSDK_NGX_D3D12_GetCapabilityParameters(&capabilities);
      if (NVSDK_NGX_SUCCEED(result) && capabilities) {
        NVSDK_NGX_Parameter_GetI(capabilities, NVSDK_NGX_Parameter_SuperSampling_Available,
                                 &available);
        NVSDK_NGX_Parameter_GetI(capabilities,
                                 NVSDK_NGX_Parameter_SuperSampling_NeedsUpdatedDriver,
                                 &needs_driver);
        NVSDK_NGX_D3D12_DestroyParameters(capabilities);
      }
      if (!available ||
          NVSDK_NGX_FAILED(NVSDK_NGX_D3D12_AllocateParameters(&upscaler->ngx_parameters))) {
        std::fprintf(stderr, "REX_TEMPORAL_AA dlss_unavailable available=%d needs_driver=%d\n",
                     available, needs_driver);
        temporal_aa_upscaler_ = upscaler.release();
        TemporalAaShutdown();
        return false;
      }
      break;
#else
      std::fprintf(stderr, "REX_TEMPORAL_AA dlss_unavailable build=no_sdk\n");
      return false;
#endif
    }
    case UpscalerKind::kFsr: {
#if REX_HAVE_FSR
      upscaler->ffx_module = LoadSdkLibrary(L"amd_fidelityfx_dx12.dll");
      if (upscaler->ffx_module) {
        upscaler->ffx_create_context = reinterpret_cast<PfnFfxCreateContext>(
            GetProcAddress(upscaler->ffx_module, "ffxCreateContext"));
        upscaler->ffx_destroy_context = reinterpret_cast<PfnFfxDestroyContext>(
            GetProcAddress(upscaler->ffx_module, "ffxDestroyContext"));
        upscaler->ffx_dispatch =
            reinterpret_cast<PfnFfxDispatch>(GetProcAddress(upscaler->ffx_module, "ffxDispatch"));
      }
      if (!upscaler->ffx_create_context || !upscaler->ffx_destroy_context ||
          !upscaler->ffx_dispatch) {
        std::fprintf(stderr, "REX_TEMPORAL_AA fsr_unavailable dll=%d\n",
                     upscaler->ffx_module ? 1 : 0);
        if (upscaler->ffx_module) FreeLibrary(upscaler->ffx_module);
        return false;
      }
      break;
#else
      std::fprintf(stderr, "REX_TEMPORAL_AA fsr_unavailable build=no_sdk\n");
      return false;
#endif
    }
    case UpscalerKind::kXess: {
#if REX_HAVE_XESS
      upscaler->xess_module = LoadSdkLibrary(L"libxess.dll");
      if (upscaler->xess_module) {
        upscaler->xess_create_context = reinterpret_cast<decltype(&xessD3D12CreateContext)>(
            GetProcAddress(upscaler->xess_module, "xessD3D12CreateContext"));
        upscaler->xess_init = reinterpret_cast<decltype(&xessD3D12Init)>(
            GetProcAddress(upscaler->xess_module, "xessD3D12Init"));
        upscaler->xess_execute = reinterpret_cast<decltype(&xessD3D12Execute)>(
            GetProcAddress(upscaler->xess_module, "xessD3D12Execute"));
        upscaler->xess_destroy_context = reinterpret_cast<decltype(&xessDestroyContext)>(
            GetProcAddress(upscaler->xess_module, "xessDestroyContext"));
      }
      if (!upscaler->xess_create_context || !upscaler->xess_init || !upscaler->xess_execute ||
          !upscaler->xess_destroy_context) {
        std::fprintf(stderr, "REX_TEMPORAL_AA xess_unavailable dll=%d\n",
                     upscaler->xess_module ? 1 : 0);
        if (upscaler->xess_module) FreeLibrary(upscaler->xess_module);
        return false;
      }
      break;
#else
      std::fprintf(stderr, "REX_TEMPORAL_AA xess_unavailable build=no_sdk\n");
      return false;
#endif
    }
  }
  temporal_aa_upscaler_ = upscaler.release();
  return true;
}

bool D3D12CommandProcessor::TemporalAaUpscalerEnsure(uint32_t width, uint32_t height) {
  TemporalAaUpscaler& upscaler = *temporal_aa_upscaler_;
  if (upscaler.width == width && upscaler.height == height && upscaler.motion) return true;
  const ui::d3d12::D3D12Provider& provider = GetD3D12Provider();
  ID3D12Device* device = provider.GetDevice();
  if (upscaler.motion) {
    // The resolution scale is init-only; a size change is rare. Nothing may
    // still use the old feature/context or textures when they are released.
    AwaitAllQueueOperationsCompletion();
  }
#if REX_HAVE_DLSS
  if (upscaler.ngx_feature) {
    NVSDK_NGX_D3D12_ReleaseFeature(upscaler.ngx_feature);
    upscaler.ngx_feature = nullptr;
  }
#endif
#if REX_HAVE_FSR
  if (upscaler.ffx_context) {
    upscaler.ffx_destroy_context(&upscaler.ffx_context, nullptr);
    upscaler.ffx_context = nullptr;
  }
#endif
#if REX_HAVE_XESS
  if (upscaler.xess_context) {
    upscaler.xess_destroy_context(upscaler.xess_context);
    upscaler.xess_context = nullptr;
  }
#endif
  upscaler.width = upscaler.height = 0;
  upscaler.depth.Reset();
  upscaler.motion.Reset();
  auto create_texture = [&](DXGI_FORMAT format, bool unordered_access,
                            Microsoft::WRL::ComPtr<ID3D12Resource>& texture) {
    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width;
    desc.Height = height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags = unordered_access ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS
                                  : D3D12_RESOURCE_FLAG_NONE;
    return SUCCEEDED(device->CreateCommittedResource(
        &ui::d3d12::util::kHeapPropertiesDefault, provider.GetHeapFlagCreateNotZeroed(), &desc,
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr,
        IID_PPV_ARGS(texture.ReleaseAndGetAddressOf())));
  };
  if (!create_texture(DXGI_FORMAT_R32_FLOAT, false, upscaler.depth) ||
      !create_texture(DXGI_FORMAT_R16G16_FLOAT, true, upscaler.motion)) {
    upscaler.failed = true;
    return false;
  }

  bool created = false;
  int32_t detail = 0;
  switch (upscaler.kind) {
    case UpscalerKind::kDlss: {
#if REX_HAVE_DLSS
      // Feature creation records work into a command list: a one-off list on
      // the direct queue, waited for here.
      Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator;
      Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> list;
      Microsoft::WRL::ComPtr<ID3D12Fence> fence;
      if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                IID_PPV_ARGS(&allocator))) ||
          FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(),
                                           nullptr, IID_PPV_ARGS(&list))) ||
          FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)))) {
        break;
      }
      detail = REXCVAR_GET(gpu_dlss_preset);
      if (detail > 0) {
        NVSDK_NGX_Parameter_SetUI(upscaler.ngx_parameters,
                                  NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA,
                                  static_cast<unsigned int>(detail));
      }
      NVSDK_NGX_DLSS_Create_Params create = {};
      create.Feature.InWidth = width;
      create.Feature.InHeight = height;
      create.Feature.InTargetWidth = width;
      create.Feature.InTargetHeight = height;
      create.Feature.InPerfQualityValue = NVSDK_NGX_PerfQuality_Value_DLAA;
      // Linear HDR colour, camera motion vectors at the render resolution
      // without jitter, reversed depth, no engine exposure value.
      create.InFeatureCreateFlags =
          NVSDK_NGX_DLSS_Feature_Flags_IsHDR | NVSDK_NGX_DLSS_Feature_Flags_MVLowRes |
          NVSDK_NGX_DLSS_Feature_Flags_DepthInverted | NVSDK_NGX_DLSS_Feature_Flags_AutoExposure;
      const NVSDK_NGX_Result result = NGX_D3D12_CREATE_DLSS_EXT(
          list.Get(), 1, 1, &upscaler.ngx_feature, upscaler.ngx_parameters, &create);
      list->Close();
      if (NVSDK_NGX_FAILED(result) || !upscaler.ngx_feature) {
        upscaler.ngx_feature = nullptr;
        detail = int32_t(result);
        break;
      }
      ID3D12CommandList* lists[] = {list.Get()};
      provider.GetDirectQueue()->ExecuteCommandLists(1, lists);
      provider.GetDirectQueue()->Signal(fence.Get(), 1);
      if (fence->GetCompletedValue() < 1) {
        HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (event) {
          fence->SetEventOnCompletion(1, event);
          WaitForSingleObject(event, INFINITE);
          CloseHandle(event);
        }
      }
      created = true;
#endif
    } break;
    case UpscalerKind::kFsr: {
#if REX_HAVE_FSR
      ffxCreateBackendDX12Desc backend = {};
      backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
      backend.device = device;
      ffxCreateContextDescUpscale create = {};
      create.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
      create.header.pNext = &backend.header;
      create.flags = FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE | FFX_UPSCALE_ENABLE_DEPTH_INVERTED |
                     FFX_UPSCALE_ENABLE_AUTO_EXPOSURE;
      create.maxRenderSize = {width, height};
      create.maxUpscaleSize = {width, height};
      create.fpMessage = FfxMessage;
      const ffxReturnCode_t result =
          upscaler.ffx_create_context(&upscaler.ffx_context, &create.header, nullptr);
      if (result != FFX_API_RETURN_OK) {
        upscaler.ffx_context = nullptr;
        detail = int32_t(result);
        break;
      }
      created = true;
#endif
    } break;
    case UpscalerKind::kXess: {
#if REX_HAVE_XESS
      xess_result_t result = upscaler.xess_create_context(device, &upscaler.xess_context);
      if (result != XESS_RESULT_SUCCESS) {
        upscaler.xess_context = nullptr;
        detail = int32_t(result);
        break;
      }
      xess_d3d12_init_params_t init = {};
      init.outputResolution.x = width;
      init.outputResolution.y = height;
      init.qualitySetting = XESS_QUALITY_SETTING_AA;
      init.initFlags = XESS_INIT_FLAG_INVERTED_DEPTH | XESS_INIT_FLAG_ENABLE_AUTOEXPOSURE;
      init.creationNodeMask = 1;
      init.visibleNodeMask = 1;
      result = upscaler.xess_init(upscaler.xess_context, &init);
      if (result != XESS_RESULT_SUCCESS) {
        upscaler.xess_destroy_context(upscaler.xess_context);
        upscaler.xess_context = nullptr;
        detail = int32_t(result);
        break;
      }
      created = true;
#endif
    } break;
  }
  if (!created) {
    std::fprintf(stderr, "REX_TEMPORAL_AA %s_create_failed result=0x%08X size=%ux%u\n",
                 UpscalerName(upscaler.kind), static_cast<unsigned>(detail), width, height);
    std::fflush(stderr);
    upscaler.failed = true;
    return false;
  }
  upscaler.width = width;
  upscaler.height = height;
  std::fprintf(stderr, "REX_TEMPORAL_AA %s_ready size=%ux%u mode=native_aa detail=%d\n",
               UpscalerName(upscaler.kind), width, height, detail);
  std::fflush(stderr);
  return true;
}

void D3D12CommandProcessor::TemporalAaUpscalerEvaluate(const void* payload,
                                                        ID3D12GraphicsCommandList* command_list) {
  UpscalerCall call;
  std::memcpy(&call, payload, sizeof(call));
  TemporalAaUpscaler& upscaler = *static_cast<TemporalAaUpscaler*>(call.upscaler);
  int64_t result = 0;
  bool ok = false;
  switch (upscaler.kind) {
    case UpscalerKind::kDlss: {
#if REX_HAVE_DLSS
      NVSDK_NGX_D3D12_DLSS_Eval_Params eval = {};
      eval.Feature.pInColor = call.color;
      eval.Feature.pInOutput = call.output;
      eval.pInDepth = call.depth;
      eval.pInMotionVectors = call.motion;
      eval.InJitterOffsetX = call.jitter_x;
      eval.InJitterOffsetY = call.jitter_y;
      eval.InRenderSubrectDimensions.Width = call.width;
      eval.InRenderSubrectDimensions.Height = call.height;
      eval.InReset = call.reset;
      eval.InMVScaleX = 1.0f;
      eval.InMVScaleY = 1.0f;
      const NVSDK_NGX_Result ngx_result = NGX_D3D12_EVALUATE_DLSS_EXT(
          command_list, upscaler.ngx_feature, upscaler.ngx_parameters, &eval);
      result = ngx_result;
      ok = NVSDK_NGX_SUCCEED(ngx_result);
#endif
    } break;
    case UpscalerKind::kFsr: {
#if REX_HAVE_FSR
      ffxDispatchDescUpscale dispatch = {};
      dispatch.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
      dispatch.commandList = command_list;
      dispatch.color = FfxTexture(call.color, FFX_API_SURFACE_FORMAT_R16G16B16A16_FLOAT,
                                  call.width, call.height, false);
      dispatch.depth =
          FfxTexture(call.depth, FFX_API_SURFACE_FORMAT_R32_FLOAT, call.width, call.height, false);
      dispatch.motionVectors = FfxTexture(call.motion, FFX_API_SURFACE_FORMAT_R16G16_FLOAT,
                                          call.width, call.height, false);
      dispatch.output = FfxTexture(call.output, FFX_API_SURFACE_FORMAT_R16G16B16A16_FLOAT,
                                   call.width, call.height, true);
      dispatch.jitterOffset.x = call.jitter_x;
      dispatch.jitterOffset.y = call.jitter_y;
      dispatch.motionVectorScale.x = 1.0f;
      dispatch.motionVectorScale.y = 1.0f;
      dispatch.renderSize = {call.width, call.height};
      dispatch.upscaleSize = {call.width, call.height};
      dispatch.enableSharpening = false;
      dispatch.sharpness = 0.0f;
      dispatch.frameTimeDelta = call.frame_time_ms;
      dispatch.preExposure = 1.0f;
      dispatch.reset = call.reset != 0;
      dispatch.cameraNear = kCameraFar;  // reversed depth: near maps to 1
      dispatch.cameraFar = kCameraNear;
      dispatch.cameraFovAngleVertical = call.fov_y;
      dispatch.viewSpaceToMetersFactor = 0.0254f;
      dispatch.flags = 0;
      const ffxReturnCode_t ffx_result =
          upscaler.ffx_dispatch(&upscaler.ffx_context, &dispatch.header);
      result = ffx_result;
      ok = ffx_result == FFX_API_RETURN_OK;
#endif
    } break;
    case UpscalerKind::kXess: {
#if REX_HAVE_XESS
      xess_d3d12_execute_params_t execute = {};
      execute.pColorTexture = call.color;
      execute.pVelocityTexture = call.motion;
      execute.pDepthTexture = call.depth;
      execute.pOutputTexture = call.output;
      execute.jitterOffsetX = call.jitter_x;
      execute.jitterOffsetY = call.jitter_y;
      execute.exposureScale = 1.0f;
      execute.resetHistory = call.reset ? 1u : 0u;
      execute.inputWidth = call.width;
      execute.inputHeight = call.height;
      const xess_result_t xess_result =
          upscaler.xess_execute(upscaler.xess_context, command_list, &execute);
      result = xess_result;
      ok = xess_result == XESS_RESULT_SUCCESS;
#endif
    } break;
  }
  if (!ok && !upscaler.failed.exchange(true)) {
    std::fprintf(stderr, "REX_TEMPORAL_AA %s_evaluate_failed result=0x%08llX\n",
                 UpscalerName(upscaler.kind), static_cast<unsigned long long>(result));
    std::fflush(stderr);
  }
}

void D3D12CommandProcessor::TemporalAaShutdown() {
  if (!temporal_aa_upscaler_) return;
  TemporalAaUpscaler& upscaler = *temporal_aa_upscaler_;
#if REX_HAVE_DLSS
  if (upscaler.ngx_feature) NVSDK_NGX_D3D12_ReleaseFeature(upscaler.ngx_feature);
  if (upscaler.ngx_parameters) NVSDK_NGX_D3D12_DestroyParameters(upscaler.ngx_parameters);
  if (upscaler.ngx_initialized) NVSDK_NGX_D3D12_Shutdown1(GetD3D12Provider().GetDevice());
#endif
#if REX_HAVE_FSR
  if (upscaler.ffx_context) upscaler.ffx_destroy_context(&upscaler.ffx_context, nullptr);
  if (upscaler.ffx_module) FreeLibrary(upscaler.ffx_module);
#endif
#if REX_HAVE_XESS
  if (upscaler.xess_context) upscaler.xess_destroy_context(upscaler.xess_context);
  if (upscaler.xess_module) FreeLibrary(upscaler.xess_module);
#endif
  delete temporal_aa_upscaler_;
  temporal_aa_upscaler_ = nullptr;
}

void D3D12CommandProcessor::InvalidateCommandListStateAfterExternalCall() {
  // The same state a new submission starts without.
  ff_viewport_update_needed_ = true;
  ff_scissor_update_needed_ = true;
  ff_blend_factor_update_needed_ = true;
  ff_stencil_ref_update_needed_ = true;
  current_guest_pipeline_ = nullptr;
  current_external_pipeline_ = nullptr;
  current_graphics_root_signature_ = nullptr;
  current_graphics_root_up_to_date_ = 0;
  if (bindless_resources_used_) {
    deferred_command_list_.SetDescriptorHeaps(view_bindless_heap_, sampler_bindless_heap_current_);
  } else {
    view_bindful_heap_current_ = nullptr;
    sampler_bindful_heap_current_ = nullptr;
  }
  primitive_topology_ = D3D_PRIMITIVE_TOPOLOGY_UNDEFINED;
  render_target_cache_->InvalidateCommandListRenderTargets();
}

void D3D12CommandProcessor::TemporalAaDraw(uint64_t vertex_shader_hash,
                                           uint64_t pixel_shader_hash,
                                           reg::RB_DEPTHCONTROL normalized_depth_control) {
  TemporalAaFrame& frame = *temporal_aa_frame_;
  const RegisterFile& regs = *register_file_;
  const float x_scale = regs.Get<float>(XE_GPU_REG_PA_CL_VPORT_XSCALE);
  const float y_scale = regs.Get<float>(XE_GPU_REG_PA_CL_VPORT_YSCALE);
  const reg::PA_CL_VTE_CNTL vte = regs.Get<reg::PA_CL_VTE_CNTL>();
  // Geometry through the viewport transform (screen-space quads - clears,
  // light rectangles - are pre-transformed).
  const bool transformed = vte.vport_x_scale_ena && vte.vport_x_offset_ena &&
                           vte.vport_y_scale_ena && vte.vport_y_offset_ena;
  const bool depth_tested = normalized_depth_control.z_enable && transformed;
  // Jitter every depth-tested draw to the scene target: depth pre-pass,
  // motion, lighting (equal-depth passes must match the pre-pass), stencil
  // volumes, sky, transparents. Nothing else: V397-V399 also jittered three
  // depth-tested post passes on the 1x surface at the end of the frame (a
  // 352x18 texture resolved to 0x1F191000 that grades the composite), which
  // shifted the grading by a sub-texel and tinted the shadows magenta/green.
  temporal_aa_jitter_draw_ =
      !temporal_aa_motion_only_ && frame.armed && depth_tested && frame.scene_target_valid &&
      regs[XE_GPU_REG_RB_SURFACE_INFO] == frame.scene_surface_info &&
      (regs[XE_GPU_REG_RB_COLOR_INFO] & 0xFFF) == frame.scene_color_edram_base;
  // Camera vote: depth-writing full-frame geometry (world and objects).
  if (depth_tested && normalized_depth_control.z_write_enable && x_scale > 256.0f &&
      (y_scale < -128.0f || y_scale > 128.0f) && !frame.vote.Full()) {
    frame.vote.Add(&regs.values[XE_GPU_REG_SHADER_CONSTANT_000_X]);
  }
  if (vertex_shader_hash != kFinalCompositeVertexShader || frame.resolved) return;
  for (uint64_t hash : kFinalCompositePixelShaders) {
    if (pixel_shader_hash == hash) {
      frame.resolved = TemporalAaResolve();
      break;
    }
  }
}

void D3D12CommandProcessor::TemporalAaCopy() {
  TemporalAaFrame& frame = *temporal_aa_frame_;
  const RegisterFile& regs = *register_file_;
  const reg::RB_COPY_CONTROL control = regs.Get<reg::RB_COPY_CONTROL>();
  if (control.copy_src_select == 4) {
    if (frame.depth_base) return;  // the first depth resolve
    frame.depth_base = regs[XE_GPU_REG_RB_COPY_DEST_BASE];
    frame.depth_pitch = regs[XE_GPU_REG_RB_COPY_DEST_PITCH];
    frame.depth_info = regs[XE_GPU_REG_RB_COPY_DEST_INFO];
    return;
  }
  // A colour resolve into the scene colour the last composite sampled (one
  // per scene tile): its source render target is the scene target.
  const uint32_t dest = regs[XE_GPU_REG_RB_COPY_DEST_BASE];
  if (control.copy_src_select < 4 && frame.scene_color_bytes &&
      dest - frame.scene_color_base < frame.scene_color_bytes) {
    frame.next_surface_info = regs[XE_GPU_REG_RB_SURFACE_INFO];
    frame.next_color_edram_base =
        regs[reg::RB_COLOR_INFO::rt_register_indices[control.copy_src_select]] & 0xFFF;
    frame.next_target_valid = true;
  }
}

void D3D12CommandProcessor::TemporalAaEndFrame() {
  TemporalAaFrame& frame = *temporal_aa_frame_;
  if (temporal_aa_motion_only_) {
    // This swap's frame (called after the swap took its frame id): its Frame
    // Generation inputs, or none (menus, loading: Frame Generation stays off).
    FrameGenerationPending& pending = frame_generation_pending_;
    const uint32_t latency_frame = ui::frame_latency::RenderSubmitFrame();
    if (pending.valid && frame.resolved) {
      const FrameGenerationInputSlot& slot = frame_generation_inputs_[pending.slot];
      ui::d3d12::streamline::FrameGenerationCamera camera = {};
      for (uint32_t i = 0; i < 16; ++i) {
        camera.view_to_clip[i] = float(pending.camera.m[i]);
        camera.clip_to_previous_clip[i] =
            pending.history ? float(pending.reproject[i]) : (i % 5 == 0 ? 1.0f : 0.0f);
      }
      for (uint32_t i = 0; i < 3; ++i) camera.position[i] = float(pending.camera.position[i]);
      const double y_scale = std::sqrt(pending.camera.m[4] * pending.camera.m[4] +
                                       pending.camera.m[5] * pending.camera.m[5] +
                                       pending.camera.m[6] * pending.camera.m[6]);
      camera.fov_y = y_scale > 0.0 ? float(2.0 * std::atan(1.0 / y_scale)) : 1.0f;
      camera.near_plane = kCameraNear;
      camera.far_plane = kCameraFar;
      camera.reset = !pending.history;
      ui::d3d12::streamline::FrameGenerationSetInputs(latency_frame, slot.depth.Get(),
                                                      slot.motion.Get(), pending.width,
                                                      pending.height, &camera);
    } else {
      ui::d3d12::streamline::FrameGenerationSetInputs(latency_frame, nullptr, nullptr, 0, 0,
                                                      nullptr);
    }
    pending.valid = false;
  }
  if (!frame.resolved) {
    // No composite this frame (menus, loading): no history, no jitter.
    frame.history_valid = false;
    frame.previous_camera = {};
  }
  frame.armed = frame.resolved;
  frame.resolved = false;
  frame.vote.Reset();
  frame.depth_base = frame.depth_pitch = frame.depth_info = 0;
  frame.scene_target_valid = frame.next_target_valid;
  frame.scene_surface_info = frame.next_surface_info;
  frame.scene_color_edram_base = frame.next_color_edram_base;
  frame.next_target_valid = false;
  ++frame.frame;
  double x = 0.0, y = 0.0;
  if (frame.armed && !temporal_aa_motion_only_) temporal_aa::Jitter(uint32_t(frame.frame), x, y);
  // Host pixels: the viewport is at the draw resolution scale.
  temporal_aa_jitter_[0] = float(x);
  temporal_aa_jitter_[1] = float(y);
}

bool D3D12CommandProcessor::TemporalAaEnsureResources(uint32_t width, uint32_t height) {
  const ui::d3d12::D3D12Provider& provider = GetD3D12Provider();
  ID3D12Device* device = provider.GetDevice();
  constexpr uint32_t kTables = uint32_t(TemporalAaRootParameter::kCount) - 1;
  if (!temporal_aa_root_signature_) {
    D3D12_ROOT_PARAMETER parameters[UINT(TemporalAaRootParameter::kCount)];
    D3D12_ROOT_PARAMETER& constants = parameters[UINT(TemporalAaRootParameter::kConstants)];
    constants.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    constants.Constants.ShaderRegister = 0;
    constants.Constants.RegisterSpace = 0;
    constants.Constants.Num32BitValues = sizeof(TemporalAaConstants) / sizeof(uint32_t);
    constants.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    // t0 colour, t1 depth, t2 history; u0 history/motion, u1 scene colour
    // out, u2 upscaler colour.
    D3D12_DESCRIPTOR_RANGE ranges[kTables];
    for (uint32_t i = 0; i < kTables; ++i) {
      D3D12_DESCRIPTOR_RANGE& range = ranges[i];
      range.RangeType = i < 3 ? D3D12_DESCRIPTOR_RANGE_TYPE_SRV : D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
      range.NumDescriptors = 1;
      range.BaseShaderRegister = i < 3 ? i : i - 3;
      range.RegisterSpace = 0;
      range.OffsetInDescriptorsFromTableStart = 0;
      D3D12_ROOT_PARAMETER& parameter =
          parameters[UINT(TemporalAaRootParameter::kColor) + i];
      parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
      parameter.DescriptorTable.NumDescriptorRanges = 1;
      parameter.DescriptorTable.pDescriptorRanges = &range;
      parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    }
    D3D12_STATIC_SAMPLER_DESC sampler = {};
    sampler.Filter = D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.MaxAnisotropy = 1;
    sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
    sampler.BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_BLACK;
    sampler.ShaderRegister = 0;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_ROOT_SIGNATURE_DESC desc;
    desc.NumParameters = UINT(TemporalAaRootParameter::kCount);
    desc.pParameters = parameters;
    desc.NumStaticSamplers = 1;
    desc.pStaticSamplers = &sampler;
    desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;
    *(temporal_aa_root_signature_.ReleaseAndGetAddressOf()) =
        ui::d3d12::util::CreateRootSignature(provider, desc);
    if (!temporal_aa_root_signature_) return false;
    *(temporal_aa_pipeline_.ReleaseAndGetAddressOf()) = ui::d3d12::util::CreateComputePipeline(
        device, shaders::temporal_aa_resolve_cs, sizeof(shaders::temporal_aa_resolve_cs),
        temporal_aa_root_signature_.Get());
    if (!temporal_aa_pipeline_) return false;
  }
  if (temporal_aa_output_) {
    const D3D12_RESOURCE_DESC desc = temporal_aa_output_->GetDesc();
    if (desc.Width != width || desc.Height != height) {
      for (Microsoft::WRL::ComPtr<ID3D12Resource>* texture :
           {std::addressof(temporal_aa_history_[0]), std::addressof(temporal_aa_history_[1]),
            std::addressof(temporal_aa_output_)}) {
        if (*texture && submission_completed_ < temporal_aa_textures_submission_) {
          (*texture)->AddRef();
          resources_for_deletion_.emplace_back(temporal_aa_textures_submission_, texture->Get());
        }
        texture->Reset();
      }
      temporal_aa_frame_->history_valid = false;
    }
  }
  if (!temporal_aa_output_) {
    auto create = [&](DXGI_FORMAT format, Microsoft::WRL::ComPtr<ID3D12Resource>& texture) {
      D3D12_RESOURCE_DESC desc = {};
      desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
      desc.Width = width;
      desc.Height = height;
      desc.DepthOrArraySize = 1;
      desc.MipLevels = 1;
      desc.Format = format;
      desc.SampleDesc.Count = 1;
      desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
      desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
      return SUCCEEDED(device->CreateCommittedResource(
          &ui::d3d12::util::kHeapPropertiesDefault, provider.GetHeapFlagCreateNotZeroed(),
          &desc, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr,
          IID_PPV_ARGS(texture.ReleaseAndGetAddressOf())));
    };
    if (!create(DXGI_FORMAT_R16G16B16A16_FLOAT, temporal_aa_history_[0]) ||
        !create(DXGI_FORMAT_R16G16B16A16_FLOAT, temporal_aa_history_[1]) ||
        !create(DXGI_FORMAT_R16G16B16A16_UNORM, temporal_aa_output_)) {
      temporal_aa_history_[0].Reset();
      temporal_aa_history_[1].Reset();
      temporal_aa_output_.Reset();
      return false;
    }
    temporal_aa_frame_->history_valid = false;
  }
  return true;
}

bool D3D12CommandProcessor::TemporalAaResolve() {
  TemporalAaFrame& frame = *temporal_aa_frame_;
  const temporal_aa::Camera camera = frame.vote.Result();
  D3D12TextureCache::TemporalAaTexture color, depth;
  if (!camera.valid || !frame.depth_base ||
      !texture_cache_->GetTemporalAaBoundTexture(0, color) ||
      color.guest_format != kSceneColorGuestFormat) {
    return false;
  }
  const uint32_t depth_width = frame.depth_pitch & 0x3FFF;
  const uint32_t depth_height = (frame.depth_pitch >> 16) & 0x3FFF;
  if (!texture_cache_->RequestTemporalAaDepth(frame.depth_base, depth_width, depth_height,
                                              xenos::Endian(frame.depth_info & 3), depth) ||
      depth.width != color.width || depth.height != color.height ||
      !TemporalAaEnsureResources(color.width, color.height)) {
    return false;
  }
  if (temporal_aa_motion_only_) return TemporalAaFrameGenerationInputs(color, depth, camera);

  // An upscaler replaces the blend: the same pass writes its inputs. When it
  // fails, the built-in resolve takes over with a fresh history (the history
  // textures held the upscaler's input and output).
  TemporalAaUpscaler* upscaler = temporal_aa_upscaler_;
  if (upscaler && (upscaler->failed || (depth.format != DXGI_FORMAT_R32_FLOAT &&
                                        depth.format != DXGI_FORMAT_R32_TYPELESS) ||
                   !TemporalAaUpscalerEnsure(color.width, color.height))) {
    if (!upscaler->fallback) {
      upscaler->fallback = true;
      frame.history_valid = false;
      std::fprintf(stderr, "REX_TEMPORAL_AA %s_fallback method=taa depth_format=%u\n",
                   UpscalerName(upscaler->kind), unsigned(depth.format));
      std::fflush(stderr);
    }
    upscaler = nullptr;
  }

  TemporalAaConstants constants = {};
  std::array<double, 16> reproject;
  const bool history = frame.history_valid && !temporal_aa::IsCut(camera, frame.previous_camera) &&
                       temporal_aa::ReprojectionMatrix(camera, frame.previous_camera, reproject);
  for (uint32_t i = 0; i < 16; ++i) {
    constants.reproject[i] = history ? float(reproject[i]) : (i % 5 == 0 ? 1.0f : 0.0f);
  }
  constants.size[0] = color.width;
  constants.size[1] = color.height;
  constants.size_inv[0] = 1.0f / float(color.width);
  constants.size_inv[1] = 1.0f / float(color.height);
  constants.jitter[0] = temporal_aa_jitter_[0];
  constants.jitter[1] = temporal_aa_jitter_[1];
  constants.history_weight = float(REXCVAR_GET(gpu_temporal_aa_history_weight));
  constants.flags = history ? kTemporalAaFlagHistory : 0u;
  const uint32_t linear_flag =
      REXCVAR_GET(gpu_temporal_aa_upscaler_linear) ? kTemporalAaFlagLinearUpscalerColor : 0u;
  if (upscaler) constants.flags |= kTemporalAaFlagUpscalerInputs | linear_flag;

  constexpr uint32_t kTables = uint32_t(TemporalAaRootParameter::kCount) - 1;
  ui::d3d12::util::DescriptorCpuGpuHandlePair descriptors[kTables];
  if (!RequestOneUseSingleViewDescriptors(kTables, descriptors)) return false;
  ID3D12Device* device = GetD3D12Provider().GetDevice();
  auto write_srv = [&](ID3D12Resource* resource, DXGI_FORMAT format,
                       D3D12_CPU_DESCRIPTOR_HANDLE handle) {
    D3D12_SHADER_RESOURCE_VIEW_DESC desc = {};
    desc.Format = format;
    desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    desc.Texture2D.MipLevels = 1;
    device->CreateShaderResourceView(resource, &desc, handle);
  };
  auto write_uav = [&](ID3D12Resource* resource, DXGI_FORMAT format,
                       D3D12_CPU_DESCRIPTOR_HANDLE handle) {
    D3D12_UNORDERED_ACCESS_VIEW_DESC desc = {};
    desc.Format = format;
    desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    device->CreateUnorderedAccessView(resource, nullptr, &desc, handle);
  };
  // Built-in: t2 = previous history, u0 = next history. Upscaler: u0 = motion
  // vectors, u2 = its linear input colour (history 0), then its output
  // (history 1) is read back through t2 by the conversion pass.
  ID3D12Resource* history_in = temporal_aa_history_[frame.history_index].Get();
  ID3D12Resource* history_out = temporal_aa_history_[frame.history_index ^ 1].Get();
  ID3D12Resource* upscaler_color = temporal_aa_history_[0].Get();
  ID3D12Resource* upscaler_output = temporal_aa_history_[1].Get();
  ID3D12Resource* target_u0 = upscaler ? upscaler->motion.Get() : history_out;
  write_srv(color.resource, DXGI_FORMAT_R16G16B16A16_UNORM, descriptors[0].first);
  write_srv(depth.resource, DXGI_FORMAT_R32_FLOAT, descriptors[1].first);
  write_srv(upscaler ? upscaler_output : history_in, DXGI_FORMAT_R16G16B16A16_FLOAT,
            descriptors[2].first);
  write_uav(target_u0, upscaler ? DXGI_FORMAT_R16G16_FLOAT : DXGI_FORMAT_R16G16B16A16_FLOAT,
            descriptors[3].first);
  write_uav(temporal_aa_output_.Get(), DXGI_FORMAT_R16G16B16A16_UNORM, descriptors[4].first);
  write_uav(upscaler_color, DXGI_FORMAT_R16G16B16A16_FLOAT, descriptors[5].first);

  auto bind_and_dispatch = [&](uint32_t flags) {
    constants.flags = flags;
    deferred_command_list_.D3DSetComputeRootSignature(temporal_aa_root_signature_.Get());
    deferred_command_list_.D3DSetComputeRoot32BitConstants(
        UINT(TemporalAaRootParameter::kConstants), sizeof(constants) / sizeof(uint32_t),
        &constants, 0);
    for (uint32_t i = 0; i < kTables; ++i) {
      deferred_command_list_.D3DSetComputeRootDescriptorTable(
          UINT(TemporalAaRootParameter::kColor) + i, descriptors[i].second);
    }
    SetExternalPipeline(temporal_aa_pipeline_.Get());
    SubmitBarriers();
    deferred_command_list_.D3DDispatch((color.width + 7) / 8, (color.height + 7) / 8, 1);
  };

  PushTransitionBarrier(target_u0, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  PushTransitionBarrier(temporal_aa_output_.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  if (upscaler) {
    PushTransitionBarrier(upscaler_color, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                          D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  }
  bind_and_dispatch(constants.flags);
  PushTransitionBarrier(target_u0, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

  if (upscaler) {
    // Inputs in the read state the SDKs expect (a typed depth copy), the
    // output in UAV state, then the SDK's own work on the command list.
    PushTransitionBarrier(upscaler_color, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                          D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    PushTransitionBarrier(upscaler_output, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                          D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    texture_cache_->TransitionTemporalAaTexture(depth.handle, D3D12_RESOURCE_STATE_COPY_SOURCE);
    PushTransitionBarrier(upscaler->depth.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                          D3D12_RESOURCE_STATE_COPY_DEST);
    SubmitBarriers();
    deferred_command_list_.D3DCopyResource(upscaler->depth.Get(), depth.resource);
    PushTransitionBarrier(upscaler->depth.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                          D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    texture_cache_->TransitionTemporalAaTexture(
        depth.handle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
                          D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    SubmitBarriers();
    const auto now = std::chrono::steady_clock::now();
    float frame_time_ms = 1000.0f / 60.0f;
    if (upscaler->last_resolve.time_since_epoch().count()) {
      frame_time_ms = std::chrono::duration<float, std::milli>(now - upscaler->last_resolve).count();
      frame_time_ms = std::fmin(std::fmax(frame_time_ms, 0.5f), 100.0f);
    }
    upscaler->last_resolve = now;
    const float jitter_sign = REXCVAR_GET(gpu_temporal_aa_debug) == 2 ? -1.0f : 1.0f;
    UpscalerCall call = {};
    call.upscaler = upscaler;
    call.color = upscaler_color;
    call.depth = upscaler->depth.Get();
    call.motion = upscaler->motion.Get();
    call.output = upscaler_output;
    call.width = color.width;
    call.height = color.height;
    call.jitter_x = jitter_sign * temporal_aa_jitter_[0];
    call.jitter_y = jitter_sign * temporal_aa_jitter_[1];
    call.frame_time_ms = frame_time_ms;
    // Vertical field of view from the camera's y scale (row c1).
    const double y_scale = std::sqrt(camera.m[4] * camera.m[4] + camera.m[5] * camera.m[5] +
                                     camera.m[6] * camera.m[6]);
    call.fov_y = y_scale > 0.0 ? float(2.0 * std::atan(1.0 / y_scale)) : 1.0f;
    call.reset = history ? 0 : 1;
    deferred_command_list_.ExternalCall(&D3D12CommandProcessor::TemporalAaUpscalerEvaluate,
                                        &call, sizeof(call));
    InvalidateCommandListStateAfterExternalCall();
    ++upscaler->evaluations;
    // Its output back into the title's encoding.
    PushTransitionBarrier(upscaler_output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                          D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    bind_and_dispatch((history ? kTemporalAaFlagHistory : 0u) | kTemporalAaFlagUpscalerOutput |
                      linear_flag);
  }

  // The result replaces the scene colour the final composite samples.
  if (REXCVAR_GET(gpu_temporal_aa_debug) == 1) {
    PushTransitionBarrier(temporal_aa_output_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                          D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  } else {
    PushTransitionBarrier(temporal_aa_output_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                          D3D12_RESOURCE_STATE_COPY_SOURCE);
    texture_cache_->TransitionTemporalAaTexture(color.handle, D3D12_RESOURCE_STATE_COPY_DEST);
    SubmitBarriers();
    deferred_command_list_.D3DCopyResource(color.resource, temporal_aa_output_.Get());
    PushTransitionBarrier(temporal_aa_output_.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
                          D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    texture_cache_->TransitionTemporalAaTexture(
        color.handle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
                          D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
  }
  temporal_aa_textures_submission_ = submission_current_;

  // Guest range of the scene colour (k_16_16_16_16, 32x32 tiles) for finding
  // the scene target in the next frame's resolves.
  frame.scene_color_base = color.guest_base;
  frame.scene_color_bytes = ((depth_width + 31) & ~31u) * ((depth_height + 31) & ~31u) * 8;
  frame.previous_camera = camera;
  frame.history_valid = true;
  // The built-in resolve alternates its history; an upscaler keeps its own.
  if (!upscaler) frame.history_index ^= 1;
  ++frame.resolves;
  if ((frame.resolves & 0x3FF) == 1 && frame.reports < 64) {
    ++frame.reports;
    std::fprintf(stderr,
                 "REX_TEMPORAL_AA resolves=%llu size=%ux%u history=%u jitter=%.3f,%.3f "
                 "camera=%.1f,%.1f,%.1f votes=%u target=%u:%08X/%03X method=%s evaluations=%llu\n",
                 static_cast<unsigned long long>(frame.resolves), color.width, color.height,
                 history ? 1u : 0u, temporal_aa_jitter_[0], temporal_aa_jitter_[1],
                 camera.position[0], camera.position[1], camera.position[2], frame.vote.draws,
                 frame.scene_target_valid ? 1u : 0u, frame.scene_surface_info,
                 frame.scene_color_edram_base, upscaler ? UpscalerName(upscaler->kind) : "taa",
                 static_cast<unsigned long long>(upscaler ? upscaler->evaluations : 0));
    std::fflush(stderr);
  }
  return true;
}

bool D3D12CommandProcessor::TemporalAaFrameGenerationEnsure(uint32_t width, uint32_t height) {
  if (frame_generation_inputs_[0].depth && frame_generation_input_width_ == width &&
      frame_generation_input_height_ == height) {
    return true;
  }
  // Older slots may still be read by submitted work: deleted after it.
  auto release = [this](Microsoft::WRL::ComPtr<ID3D12Resource>& resource) {
    if (!resource) return;
    resource->AddRef();
    resources_for_deletion_.emplace_back(submission_current_, resource.Get());
    resource.Reset();
  };
  for (FrameGenerationInputSlot& slot : frame_generation_inputs_) {
    release(slot.depth);
    release(slot.motion);
  }
  ID3D12Device* device = GetD3D12Provider().GetDevice();
  D3D12_HEAP_PROPERTIES heap = {};
  heap.Type = D3D12_HEAP_TYPE_DEFAULT;
  D3D12_RESOURCE_DESC desc = {};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  desc.Width = width;
  desc.Height = height;
  desc.DepthOrArraySize = 1;
  desc.MipLevels = 1;
  desc.SampleDesc.Count = 1;
  for (FrameGenerationInputSlot& slot : frame_generation_inputs_) {
    desc.Format = DXGI_FORMAT_R32_FLOAT;
    desc.Flags = D3D12_RESOURCE_FLAG_NONE;
    if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                               D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                               nullptr, IID_PPV_ARGS(&slot.depth)))) {
      return false;
    }
    desc.Format = DXGI_FORMAT_R16G16_FLOAT;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                               D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                               nullptr, IID_PPV_ARGS(&slot.motion)))) {
      return false;
    }
  }
  frame_generation_input_width_ = width;
  frame_generation_input_height_ = height;
  std::fprintf(stderr, "REX_TEMPORAL_AA frame_generation_inputs size=%ux%u slots=%u\n", width,
               height, kFrameGenerationInputSlots);
  std::fflush(stderr);
  return true;
}

// Frame Generation inputs (motion-only tracking): the frame's camera motion
// vectors (the resolve's upscaler-input pass without colour) and a typed copy
// of its scene depth into the next ring slot; the scene colour is untouched.
bool D3D12CommandProcessor::TemporalAaFrameGenerationInputs(
    const D3D12TextureCache::TemporalAaTexture& color,
    const D3D12TextureCache::TemporalAaTexture& depth, const temporal_aa::Camera& camera) {
  TemporalAaFrame& frame = *temporal_aa_frame_;
  if ((depth.format != DXGI_FORMAT_R32_FLOAT && depth.format != DXGI_FORMAT_R32_TYPELESS) ||
      !TemporalAaFrameGenerationEnsure(color.width, color.height)) {
    return false;
  }
  FrameGenerationPending& pending = frame_generation_pending_;
  pending.history = frame.history_valid && !temporal_aa::IsCut(camera, frame.previous_camera) &&
                    temporal_aa::ReprojectionMatrix(camera, frame.previous_camera, pending.reproject);
  TemporalAaConstants constants = {};
  for (uint32_t i = 0; i < 16; ++i) {
    constants.reproject[i] =
        pending.history ? float(pending.reproject[i]) : (i % 5 == 0 ? 1.0f : 0.0f);
  }
  constants.size[0] = color.width;
  constants.size[1] = color.height;
  constants.size_inv[0] = 1.0f / float(color.width);
  constants.size_inv[1] = 1.0f / float(color.height);
  constants.flags = (pending.history ? kTemporalAaFlagHistory : 0u) |
                    kTemporalAaFlagUpscalerInputs | kTemporalAaFlagMotionOnly;

  frame_generation_input_slot_ = (frame_generation_input_slot_ + 1) % kFrameGenerationInputSlots;
  const FrameGenerationInputSlot& slot = frame_generation_inputs_[frame_generation_input_slot_];
  constexpr uint32_t kTables = uint32_t(TemporalAaRootParameter::kCount) - 1;
  ui::d3d12::util::DescriptorCpuGpuHandlePair descriptors[kTables];
  if (!RequestOneUseSingleViewDescriptors(kTables, descriptors)) return false;
  ID3D12Device* device = GetD3D12Provider().GetDevice();
  auto write_srv = [&](ID3D12Resource* resource, DXGI_FORMAT format,
                       D3D12_CPU_DESCRIPTOR_HANDLE handle) {
    D3D12_SHADER_RESOURCE_VIEW_DESC desc = {};
    desc.Format = format;
    desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    desc.Texture2D.MipLevels = 1;
    device->CreateShaderResourceView(resource, &desc, handle);
  };
  auto write_uav = [&](ID3D12Resource* resource, DXGI_FORMAT format,
                       D3D12_CPU_DESCRIPTOR_HANDLE handle) {
    D3D12_UNORDERED_ACCESS_VIEW_DESC desc = {};
    desc.Format = format;
    desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    device->CreateUnorderedAccessView(resource, nullptr, &desc, handle);
  };
  // Only t1 (depth) and u0 (motion) are used; the other views stay valid.
  write_srv(color.resource, DXGI_FORMAT_R16G16B16A16_UNORM, descriptors[0].first);
  write_srv(depth.resource, DXGI_FORMAT_R32_FLOAT, descriptors[1].first);
  write_srv(temporal_aa_history_[0].Get(), DXGI_FORMAT_R16G16B16A16_FLOAT, descriptors[2].first);
  write_uav(slot.motion.Get(), DXGI_FORMAT_R16G16_FLOAT, descriptors[3].first);
  write_uav(temporal_aa_output_.Get(), DXGI_FORMAT_R16G16B16A16_UNORM, descriptors[4].first);
  write_uav(temporal_aa_history_[1].Get(), DXGI_FORMAT_R16G16B16A16_FLOAT, descriptors[5].first);
  PushTransitionBarrier(slot.motion.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  deferred_command_list_.D3DSetComputeRootSignature(temporal_aa_root_signature_.Get());
  deferred_command_list_.D3DSetComputeRoot32BitConstants(
      UINT(TemporalAaRootParameter::kConstants), sizeof(constants) / sizeof(uint32_t), &constants,
      0);
  for (uint32_t i = 0; i < kTables; ++i) {
    deferred_command_list_.D3DSetComputeRootDescriptorTable(
        UINT(TemporalAaRootParameter::kColor) + i, descriptors[i].second);
  }
  SetExternalPipeline(temporal_aa_pipeline_.Get());
  SubmitBarriers();
  deferred_command_list_.D3DDispatch((color.width + 7) / 8, (color.height + 7) / 8, 1);
  PushTransitionBarrier(slot.motion.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  // The typed depth copy.
  texture_cache_->TransitionTemporalAaTexture(depth.handle, D3D12_RESOURCE_STATE_COPY_SOURCE);
  PushTransitionBarrier(slot.depth.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                        D3D12_RESOURCE_STATE_COPY_DEST);
  SubmitBarriers();
  deferred_command_list_.D3DCopyResource(slot.depth.Get(), depth.resource);
  PushTransitionBarrier(slot.depth.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  texture_cache_->TransitionTemporalAaTexture(
      depth.handle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
                        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
  SubmitBarriers();

  pending.valid = true;
  pending.slot = frame_generation_input_slot_;
  pending.width = color.width;
  pending.height = color.height;
  pending.camera = camera;
  frame.previous_camera = camera;
  frame.history_valid = true;
  ++frame.resolves;
  if ((frame.resolves & 0x3FF) == 1 && frame.reports < 64) {
    ++frame.reports;
    std::fprintf(stderr,
                 "REX_TEMPORAL_AA resolves=%llu size=%ux%u history=%u camera=%.1f,%.1f,%.1f "
                 "votes=%u method=motion_only\n",
                 static_cast<unsigned long long>(frame.resolves), color.width, color.height,
                 pending.history ? 1u : 0u, camera.position[0], camera.position[1],
                 camera.position[2], frame.vote.draws);
    std::fflush(stderr);
  }
  return true;
}

}  // namespace rex::graphics::d3d12
