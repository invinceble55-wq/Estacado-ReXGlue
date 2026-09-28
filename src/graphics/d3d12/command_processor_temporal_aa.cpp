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
#include <rex/ui/d3d12/openxr_screen.h>
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

REXCVAR_DEFINE_STRING(graphics_upscale_mode, "off", "Graphics",
                      "Temporal upscaling of the finished frame with the temporal_aa upscaler "
                      "(dlss, fsr or xess): off, native (anti-aliasing only: DLAA), quality "
                      "(1.5x), balanced (1.72x), performance (2x) or ultra_performance (3x) of "
                      "the internal resolution")
    .allowed({"off", "native", "quality", "balanced", "performance", "ultra_performance"})
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_DOUBLE(gpu_vr_units_per_metre, 39.37, "GPU/Experimental",
                      "VR: the title's world units per metre (eye distance and head movement; inches, measured from a door in the first apartment)")
    .range(1.0, 10000.0);
REXCVAR_DEFINE_BOOL(gpu_vr_log_camera, false, "GPU/Experimental",
                    "VR test: log the title camera's direction every 64 frames");
REXCVAR_DEFINE_INT32(gpu_temporal_upscale_debug, 0, "GPU/Experimental",
                     "Temporal upscaling test mode: 1 = show the HUD mask in red")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace rex::graphics::d3d12 {
double TemporalUpscaleRatio(const std::string& mode);

namespace shaders {
#include "../shaders/bytecode/d3d12_5_1/temporal_aa_resolve_cs.h"
#include "../shaders/bytecode/d3d12_5_1/temporal_upscale_composite_cs.h"
#include "../shaders/bytecode/d3d12_5_1/temporal_upscale_prepare_cs.h"
#include "../shaders/bytecode/d3d12_5_1/temporal_upscale_vr_hud_cs.h"
#include "../shaders/bytecode/d3d12_5_1/temporal_upscale_vr_preview_cs.h"
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
  uint32_t out_width;
  uint32_t out_height;
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
  // Output size (temporal upscaling: larger than the render size; native AA:
  // equal) and whether the input colour is display-ready (LDR) rather than
  // the scene's linear HDR.
  uint32_t out_width = 0;
  uint32_t out_height = 0;
  bool ldr = false;
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
  // VR (experimental): only the HUD-less frame at the final composite, for
  // the eye images (no jitter, no upscaler; TemporalVrAtSwap).
  if (ui::d3d12::openxr_screen::Wanted()) {
    temporal_aa_enabled_ = true;
    temporal_aa_vr_only_ = true;
    temporal_aa_frame_ = std::make_unique<TemporalAaFrame>();
    temporal_upscale_ = std::make_unique<TemporalUpscale>();
    temporal_upscale_->ratio = 0.0;
    std::fprintf(stderr, "REX_TEMPORAL_AA enabled=1 method=vr_capture (temporal AA and "
                         "upscaling off in VR)\n");
    std::fflush(stderr);
    return;
  }
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
  // Temporal upscaling needs one of the SDK upscalers; without it the frame
  // stays as without the setting (never the scene TAA).
  const double upscale_ratio = TemporalUpscaleRatio(REXCVAR_GET(graphics_upscale_mode));
  if (upscale_ratio >= 1.0) {
    if (!upscaler) {
      std::fprintf(stderr, "REX_TEMPORAL_UPSCALE unavailable method=%s mode=%s applied=off\n",
                   method.c_str(), REXCVAR_GET(graphics_upscale_mode).c_str());
      std::fflush(stderr);
      temporal_aa_enabled_ = false;
      temporal_aa_frame_.reset();
      return;
    }
    temporal_upscale_ = std::make_unique<TemporalUpscale>();
    temporal_upscale_->ratio = upscale_ratio;
    std::fprintf(stderr, "REX_TEMPORAL_UPSCALE enabled=1 mode=%s ratio=%.3f\n",
                 REXCVAR_GET(graphics_upscale_mode).c_str(), upscale_ratio);
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

bool D3D12CommandProcessor::TemporalAaUpscalerEnsure(uint32_t width, uint32_t height,
                                                      uint32_t out_width, uint32_t out_height,
                                                      bool ldr) {
  TemporalAaUpscaler& upscaler = *temporal_aa_upscaler_;
  if (!out_width || !out_height) {
    out_width = width;
    out_height = height;
  }
  if (upscaler.width == width && upscaler.height == height && upscaler.out_width == out_width &&
      upscaler.out_height == out_height && upscaler.ldr == ldr && upscaler.motion) {
    return true;
  }
  // Output / render size: the SDKs' quality modes (1.5 Quality, ~1.72
  // Balanced, 2 Performance, 3 Ultra Performance, 1 native AA).
  const double ratio = double(out_height) / double(height);
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
  upscaler.out_width = upscaler.out_height = 0;
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
      create.Feature.InTargetWidth = out_width;
      create.Feature.InTargetHeight = out_height;
      create.Feature.InPerfQualityValue =
          ratio < 1.05   ? NVSDK_NGX_PerfQuality_Value_DLAA
          : ratio < 1.6  ? NVSDK_NGX_PerfQuality_Value_MaxQuality
          : ratio < 1.85 ? NVSDK_NGX_PerfQuality_Value_Balanced
          : ratio < 2.4  ? NVSDK_NGX_PerfQuality_Value_MaxPerf
                         : NVSDK_NGX_PerfQuality_Value_UltraPerformance;
      // Linear HDR colour (native AA) or the display-ready frame (temporal
      // upscaling), camera motion vectors at the render resolution without
      // jitter, reversed depth, no engine exposure value.
      create.InFeatureCreateFlags =
          (ldr ? 0 : NVSDK_NGX_DLSS_Feature_Flags_IsHDR) | NVSDK_NGX_DLSS_Feature_Flags_MVLowRes |
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
      create.flags = (ldr ? 0u : uint32_t(FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE)) |
                     FFX_UPSCALE_ENABLE_DEPTH_INVERTED | FFX_UPSCALE_ENABLE_AUTO_EXPOSURE;
      create.maxRenderSize = {width, height};
      create.maxUpscaleSize = {out_width, out_height};
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
      init.outputResolution.x = out_width;
      init.outputResolution.y = out_height;
      init.qualitySetting = ratio < 1.05   ? XESS_QUALITY_SETTING_AA
                            : ratio < 1.6  ? XESS_QUALITY_SETTING_QUALITY
                            : ratio < 1.85 ? XESS_QUALITY_SETTING_BALANCED
                            : ratio < 2.4  ? XESS_QUALITY_SETTING_PERFORMANCE
                                           : XESS_QUALITY_SETTING_ULTRA_PERFORMANCE;
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
  upscaler.out_width = out_width;
  upscaler.out_height = out_height;
  upscaler.ldr = ldr;
  std::fprintf(stderr, "REX_TEMPORAL_AA %s_ready size=%ux%u output=%ux%u ratio=%.2f mode=%s detail=%d\n",
               UpscalerName(upscaler.kind), width, height, out_width, out_height, ratio,
               ratio < 1.05 ? "native_aa" : "upscale", detail);
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
                                   call.out_width, call.out_height, true);
      dispatch.jitterOffset.x = call.jitter_x;
      dispatch.jitterOffset.y = call.jitter_y;
      dispatch.motionVectorScale.x = 1.0f;
      dispatch.motionVectorScale.y = 1.0f;
      dispatch.renderSize = {call.width, call.height};
      dispatch.upscaleSize = {call.out_width, call.out_height};
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
      !temporal_aa_motion_only_ && !temporal_aa_vr_only_ && frame.armed && depth_tested &&
      frame.scene_target_valid &&
      regs[XE_GPU_REG_RB_SURFACE_INFO] == frame.scene_surface_info &&
      (regs[XE_GPU_REG_RB_COLOR_INFO] & 0xFFF) == frame.scene_color_edram_base;
  // VR stereo: the same scene draws get the eye's camera (TemporalVrNextFrame).
  temporal_vr_draw_ = temporal_aa_vr_only_ && temporal_vr_stereo_ && frame.armed && depth_tested &&
                      frame.scene_target_valid &&
                      regs[XE_GPU_REG_RB_SURFACE_INFO] == frame.scene_surface_info &&
                      (regs[XE_GPU_REG_RB_COLOR_INFO] & 0xFFF) == frame.scene_color_edram_base;
  if (temporal_vr_draw_) ++temporal_vr_draws_;
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
  // per scene tile): its source render target is the scene target. Only the
  // first one of the frame: with the Darkness out (and in the title menu) a
  // post-process on another surface copies back into the same texture after
  // the scene, and learning that surface jittered its depth-tested passes,
  // the colour grading among them (red/green casts alternating per frame,
  // 2026-10-12, owner's report).
  const uint32_t dest = regs[XE_GPU_REG_RB_COPY_DEST_BASE];
  if (control.copy_src_select < 4 && frame.scene_color_bytes && !frame.next_target_valid &&
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
  if (temporal_aa_vr_only_) TemporalVrNextFrame();
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
  if (temporal_aa_vr_only_) {
    // VR: the title's projection for the next frames' eye transform, and
    // the frame right after this composite is the eyes' image.
    if (camera.valid) {
      const auto& m = camera.m;
      const double forward = m[12] * m[12] + m[13] * m[13] + m[14] * m[14];
      temporal_vr_projection_[0] = std::sqrt(m[0] * m[0] + m[1] * m[1] + m[2] * m[2]);
      temporal_vr_projection_[1] = std::sqrt(m[4] * m[4] + m[5] * m[5] + m[6] * m[6]);
      temporal_vr_projection_[2] =
          forward > 0.0 ? (m[8] * m[12] + m[9] * m[13] + m[10] * m[14]) / forward : 0.0;
      temporal_vr_projection_[3] = m[11];
      temporal_vr_projection_valid_ = true;
      const double length = std::sqrt(forward);
      for (int i = 0; i < 3; ++i) temporal_vr_forward_[i] = length > 0.0 ? m[12 + i] / length : 0.0;
    }
    TemporalUpscale& upscale = *temporal_upscale_;
    upscale.width = color.width;
    upscale.height = color.height;
    upscale.capture_pending = true;
    upscale.frame_valid = false;
    // The scene target (TemporalAaCopy learns it from the colour resolves
    // into this texture) for the next frame's scene draws.
    frame.scene_color_base = color.guest_base;
    frame.scene_color_bytes = ((depth_width + 31) & ~31u) * ((depth_height + 31) & ~31u) * 8;
    return true;
  }
  if (temporal_aa_motion_only_) return TemporalAaFrameGenerationInputs(color, depth, camera);
  if (temporal_upscale_) {
    // Temporal upscaling: the scene stays jittered; motion, depth and the
    // HUD-less frame go to the swap (TemporalUpscaleAtSwap). After a failure
    // the frame is drawn as without the setting (no jitter).
    if (temporal_upscale_->failed) {
      temporal_aa_enabled_ = false;
      temporal_aa_jitter_draw_ = false;
      return false;
    }
    if (!TemporalUpscaleAtComposite(color, depth, camera)) return false;
    frame.scene_color_base = color.guest_base;
    frame.scene_color_bytes = ((depth_width + 31) & ~31u) * ((depth_height + 31) & ~31u) * 8;
    return true;
  }

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
    call.out_width = color.width;
    call.out_height = color.height;
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
  // Temporal upscaling renders the scene jittered (zero for Frame Generation).
  constants.jitter[0] = temporal_aa_jitter_[0];
  constants.jitter[1] = temporal_aa_jitter_[1];

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

// ---------------------------------------------------------------------------
// Temporal upscaling of the finished frame (#3, #23; command_processor.h).

namespace {

struct TemporalUpscaleConstants {
  uint32_t in_size[2];
  uint32_t out_size[2];
  float in_size_inv[2];
  float out_size_inv[2];
  float hud_threshold;
  uint32_t flags;
  float padding0[2];
  float padding1[12];
};
static_assert(sizeof(TemporalUpscaleConstants) == 24 * sizeof(uint32_t),
              "the temporal AA root signature's 24 root constants");

}  // namespace

double TemporalUpscaleRatio(const std::string& mode) {
  if (mode == "native") return 1.0;
  if (mode == "quality") return 1.5;
  if (mode == "balanced") return 1.0 / 0.58;
  if (mode == "performance") return 2.0;
  if (mode == "ultra_performance") return 3.0;
  return 0.0;
}

bool D3D12CommandProcessor::TemporalUpscaleAtComposite(
    const D3D12TextureCache::TemporalAaTexture& color,
    const D3D12TextureCache::TemporalAaTexture& depth, const temporal_aa::Camera& camera) {
  TemporalUpscale& upscale = *temporal_upscale_;
  if (!TemporalAaFrameGenerationInputs(color, depth, camera)) return false;
  const FrameGenerationPending& pending = frame_generation_pending_;
  upscale.slot = pending.slot;
  upscale.width = color.width;
  upscale.height = color.height;
  upscale.reset = !pending.history;
  upscale.jitter[0] = temporal_aa_jitter_[0];
  upscale.jitter[1] = temporal_aa_jitter_[1];
  const double y_scale = std::sqrt(camera.m[4] * camera.m[4] + camera.m[5] * camera.m[5] +
                                   camera.m[6] * camera.m[6]);
  upscale.fov_y = y_scale > 0.0 ? float(2.0 * std::atan(1.0 / y_scale)) : 1.0f;
  upscale.capture_pending = true;
  upscale.frame_valid = false;
  return true;
}

void D3D12CommandProcessor::TemporalUpscaleCaptureHudless() {
  TemporalUpscale& upscale = *temporal_upscale_;
  upscale.capture_pending = false;
  DXGI_FORMAT resource_format, view_format;
  uint32_t target_width = 0, target_height = 0;
  if (!render_target_cache_->BoundColorTarget0Format(resource_format, view_format, target_width,
                                                      target_height) ||
      target_width < upscale.width || target_height < upscale.height) {
    if (++upscale.skipped <= 8) {
      std::fprintf(stderr,
                   "REX_TEMPORAL_UPSCALE hudless_unavailable target=%ux%u frame=%ux%u\n",
                   target_width, target_height, upscale.width, upscale.height);
      std::fflush(stderr);
    }
    return;
  }
  if (upscale.hudless) {
    const D3D12_RESOURCE_DESC desc = upscale.hudless->GetDesc();
    if (desc.Width != upscale.width || desc.Height != upscale.height ||
        desc.Format != resource_format) {
      upscale.hudless->AddRef();
      resources_for_deletion_.emplace_back(submission_current_, upscale.hudless.Get());
      upscale.hudless.Reset();
    }
  }
  const ui::d3d12::D3D12Provider& provider = GetD3D12Provider();
  if (!upscale.hudless) {
    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = upscale.width;
    desc.Height = upscale.height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = resource_format;
    desc.SampleDesc.Count = 1;
    if (FAILED(provider.GetDevice()->CreateCommittedResource(
            &ui::d3d12::util::kHeapPropertiesDefault, provider.GetHeapFlagCreateNotZeroed(), &desc,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr,
            IID_PPV_ARGS(&upscale.hudless)))) {
      return;
    }
  }
  PushTransitionBarrier(upscale.hudless.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                        D3D12_RESOURCE_STATE_COPY_DEST);
  const bool copied =
      render_target_cache_->CopyBoundColorTarget0(upscale.hudless.Get(), upscale.width,
                                                  upscale.height);
  PushTransitionBarrier(upscale.hudless.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  upscale.hudless_view_format = view_format;
  upscale.frame_valid = copied;
  upscale.textures_submission = submission_current_;
}

bool D3D12CommandProcessor::TemporalUpscaleAtSwap(ID3D12Resource*& swap_texture,
                                                  D3D12_SHADER_RESOURCE_VIEW_DESC& swap_texture_srv,
                                                  uint32_t& width, uint32_t& height) {
  if (temporal_upscale_ && temporal_aa_vr_only_) {
    return TemporalVrAtSwap(swap_texture, swap_texture_srv, width, height);
  }
  if (!temporal_upscale_ || !temporal_aa_upscaler_) return false;
  TemporalUpscale& upscale = *temporal_upscale_;
  if (!upscale.frame_valid) return false;
  upscale.frame_valid = false;
  TemporalAaUpscaler& upscaler = *temporal_aa_upscaler_;
  if (width != upscale.width || height != upscale.height) return false;
  const uint32_t out_width = upscale.ratio < 1.01
                                 ? width
                                 : (uint32_t(std::lround(double(width) * upscale.ratio)) + 1) & ~1u;
  const uint32_t out_height =
      upscale.ratio < 1.01 ? height
                           : (uint32_t(std::lround(double(height) * upscale.ratio)) + 1) & ~1u;
  if (upscaler.failed || !TemporalAaUpscalerEnsure(width, height, out_width, out_height, true)) {
    if (!upscale.failed) {
      upscale.failed = true;
      std::fprintf(stderr,
                   "REX_TEMPORAL_UPSCALE failed method=%s render=%ux%u output=%ux%u applied=off\n",
                   UpscalerName(upscaler.kind), width, height, out_width, out_height);
      std::fflush(stderr);
    }
    return false;
  }

  const ui::d3d12::D3D12Provider& provider = GetD3D12Provider();
  ID3D12Device* device = provider.GetDevice();
  // The finished frame's format when a typed UAV can write it.
  DXGI_FORMAT output_format = swap_texture_srv.Format;
  if (output_format != DXGI_FORMAT_R8G8B8A8_UNORM && output_format != DXGI_FORMAT_R10G10B10A2_UNORM &&
      output_format != DXGI_FORMAT_R16G16B16A16_UNORM &&
      output_format != DXGI_FORMAT_R16G16B16A16_FLOAT) {
    output_format = DXGI_FORMAT_R8G8B8A8_UNORM;
  }
  auto ensure = [&](Microsoft::WRL::ComPtr<ID3D12Resource>& texture, uint32_t texture_width,
                    uint32_t texture_height, DXGI_FORMAT format) {
    if (texture) {
      const D3D12_RESOURCE_DESC desc = texture->GetDesc();
      if (desc.Width == texture_width && desc.Height == texture_height && desc.Format == format) {
        return true;
      }
      texture->AddRef();
      resources_for_deletion_.emplace_back(submission_current_, texture.Get());
      texture.Reset();
    }
    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = texture_width;
    desc.Height = texture_height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    return SUCCEEDED(device->CreateCommittedResource(
        &ui::d3d12::util::kHeapPropertiesDefault, provider.GetHeapFlagCreateNotZeroed(), &desc,
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr, IID_PPV_ARGS(&texture)));
  };
  if (!ensure(upscale.color, width, height, DXGI_FORMAT_R16G16B16A16_FLOAT) ||
      !ensure(upscale.upscaled, out_width, out_height, DXGI_FORMAT_R16G16B16A16_FLOAT) ||
      !ensure(upscale.output, out_width, out_height, output_format)) {
    return false;
  }
  if (!temporal_upscale_prepare_pipeline_) {
    temporal_upscale_prepare_pipeline_ = ui::d3d12::util::CreateComputePipeline(
        device, shaders::temporal_upscale_prepare_cs, sizeof(shaders::temporal_upscale_prepare_cs),
        temporal_aa_root_signature_.Get());
    temporal_upscale_composite_pipeline_ = ui::d3d12::util::CreateComputePipeline(
        device, shaders::temporal_upscale_composite_cs,
        sizeof(shaders::temporal_upscale_composite_cs), temporal_aa_root_signature_.Get());
  }
  if (!temporal_aa_root_signature_ || !temporal_upscale_prepare_pipeline_ ||
      !temporal_upscale_composite_pipeline_) {
    return false;
  }

  constexpr uint32_t kTables = uint32_t(TemporalAaRootParameter::kCount) - 1;
  ui::d3d12::util::DescriptorCpuGpuHandlePair descriptors[kTables];
  if (!RequestOneUseSingleViewDescriptors(kTables, descriptors)) return false;
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
  write_srv(upscale.hudless.Get(), upscale.hudless_view_format, descriptors[0].first);
  device->CreateShaderResourceView(swap_texture, &swap_texture_srv, descriptors[1].first);
  write_srv(upscale.upscaled.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT, descriptors[2].first);
  write_uav(upscale.color.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT, descriptors[3].first);
  write_uav(upscale.output.Get(), output_format, descriptors[4].first);
  write_uav(upscale.color.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT, descriptors[5].first);

  TemporalUpscaleConstants constants = {};
  constants.in_size[0] = width;
  constants.in_size[1] = height;
  constants.out_size[0] = out_width;
  constants.out_size[1] = out_height;
  constants.in_size_inv[0] = 1.0f / float(width);
  constants.in_size_inv[1] = 1.0f / float(height);
  constants.out_size_inv[0] = 1.0f / float(out_width);
  constants.out_size_inv[1] = 1.0f / float(out_height);
  constants.hud_threshold = 3.0f / 255.0f;
  constants.flags = REXCVAR_GET(gpu_temporal_upscale_debug) == 1 ? 1u : 0u;
  auto dispatch = [&](ID3D12PipelineState* pipeline, uint32_t groups_x, uint32_t groups_y) {
    deferred_command_list_.D3DSetComputeRootSignature(temporal_aa_root_signature_.Get());
    deferred_command_list_.D3DSetComputeRoot32BitConstants(
        UINT(TemporalAaRootParameter::kConstants), sizeof(constants) / sizeof(uint32_t),
        &constants, 0);
    for (uint32_t i = 0; i < kTables; ++i) {
      deferred_command_list_.D3DSetComputeRootDescriptorTable(
          UINT(TemporalAaRootParameter::kColor) + i, descriptors[i].second);
    }
    SetExternalPipeline(pipeline);
    SubmitBarriers();
    deferred_command_list_.D3DDispatch(groups_x, groups_y, 1);
  };

  // The HUD-less frame as the upscaler's input colour.
  PushTransitionBarrier(upscale.color.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  dispatch(temporal_upscale_prepare_pipeline_.Get(), (width + 7) / 8, (height + 7) / 8);
  PushTransitionBarrier(upscale.color.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  PushTransitionBarrier(upscale.upscaled.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  SubmitBarriers();

  const auto now = std::chrono::steady_clock::now();
  float frame_time_ms = 1000.0f / 60.0f;
  if (upscale.last.time_since_epoch().count()) {
    frame_time_ms = std::chrono::duration<float, std::milli>(now - upscale.last).count();
    frame_time_ms = std::fmin(std::fmax(frame_time_ms, 0.5f), 100.0f);
  }
  upscale.last = now;
  const FrameGenerationInputSlot& slot = frame_generation_inputs_[upscale.slot];
  UpscalerCall call = {};
  call.upscaler = &upscaler;
  call.color = upscale.color.Get();
  call.depth = slot.depth.Get();
  call.motion = slot.motion.Get();
  call.output = upscale.upscaled.Get();
  call.width = width;
  call.height = height;
  call.out_width = out_width;
  call.out_height = out_height;
  call.jitter_x = upscale.jitter[0];
  call.jitter_y = upscale.jitter[1];
  call.frame_time_ms = frame_time_ms;
  call.fov_y = upscale.fov_y;
  call.reset = upscale.reset ? 1 : 0;
  deferred_command_list_.ExternalCall(&D3D12CommandProcessor::TemporalAaUpscalerEvaluate, &call,
                                      sizeof(call));
  InvalidateCommandListStateAfterExternalCall();
  ++upscaler.evaluations;
  PushTransitionBarrier(upscale.upscaled.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

  // The HUD from the finished frame over the upscaled scene.
  PushTransitionBarrier(upscale.output.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  dispatch(temporal_upscale_composite_pipeline_.Get(), (out_width + 7) / 8, (out_height + 7) / 8);
  PushTransitionBarrier(upscale.output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  SubmitBarriers();
  upscale.textures_submission = submission_current_;

  swap_texture = upscale.output.Get();
  swap_texture_srv = {};
  swap_texture_srv.Format = output_format;
  swap_texture_srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
  swap_texture_srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  swap_texture_srv.Texture2D.MipLevels = 1;
  width = out_width;
  height = out_height;
  if ((++upscale.frames & 0x3FF) == 1) {
    std::fprintf(stderr,
                 "REX_TEMPORAL_UPSCALE frames=%llu method=%s render=%ux%u output=%ux%u "
                 "ratio=%.2f reset=%u jitter=%.3f,%.3f\n",
                 static_cast<unsigned long long>(upscale.frames), UpscalerName(upscaler.kind),
                 call.width, call.height, out_width, out_height, upscale.ratio,
                 upscale.reset ? 1u : 0u, upscale.jitter[0], upscale.jitter[1]);
    std::fflush(stderr);
  }
  return true;
}

// VR (experimental; openxr_screen.h): the HUD-less frame into the tagged
// eye's image, the HUD into the HUD image; with display_vr_simulate the
// monitor shows the two eyes (side by side or anaglyph) instead of the frame.
bool D3D12CommandProcessor::TemporalVrAtSwap(ID3D12Resource*& swap_texture,
                                             D3D12_SHADER_RESOURCE_VIEW_DESC& swap_texture_srv,
                                             uint32_t& width, uint32_t& height) {
  TemporalUpscale& upscale = *temporal_upscale_;
  // The eye and head view this frame was drawn with (TemporalVrNextFrame).
  const uint32_t tag = temporal_vr_finished_tag_;
  if (!upscale.frame_valid) return false;
  upscale.frame_valid = false;
  if (width != upscale.width || height != upscale.height || !upscale.hudless) return false;
  const ui::d3d12::D3D12Provider& provider = GetD3D12Provider();
  ID3D12Device* device = provider.GetDevice();
  ID3D12Resource* eye = ui::d3d12::openxr_screen::EyeTarget(device, tag, width, height);
  ID3D12Resource* hud = eye ? ui::d3d12::openxr_screen::HudTarget(device, width, height) : nullptr;
  if (!eye || !hud) return false;
  if (!temporal_upscale_prepare_pipeline_) {
    temporal_upscale_prepare_pipeline_ = ui::d3d12::util::CreateComputePipeline(
        device, shaders::temporal_upscale_prepare_cs, sizeof(shaders::temporal_upscale_prepare_cs),
        temporal_aa_root_signature_.Get());
  }
  if (!temporal_vr_hud_pipeline_) {
    temporal_vr_hud_pipeline_ = ui::d3d12::util::CreateComputePipeline(
        device, shaders::temporal_upscale_vr_hud_cs, sizeof(shaders::temporal_upscale_vr_hud_cs),
        temporal_aa_root_signature_.Get());
    temporal_vr_preview_pipeline_ = ui::d3d12::util::CreateComputePipeline(
        device, shaders::temporal_upscale_vr_preview_cs,
        sizeof(shaders::temporal_upscale_vr_preview_cs), temporal_aa_root_signature_.Get());
  }
  if (!temporal_aa_root_signature_ || !temporal_upscale_prepare_pipeline_ ||
      !temporal_vr_hud_pipeline_ || !temporal_vr_preview_pipeline_) {
    return false;
  }
  constexpr uint32_t kTables = uint32_t(TemporalAaRootParameter::kCount) - 1;
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
  TemporalUpscaleConstants constants = {};
  auto dispatch = [&](ID3D12PipelineState* pipeline,
                      const ui::d3d12::util::DescriptorCpuGpuHandlePair* descriptors,
                      uint32_t groups_x, uint32_t groups_y) {
    deferred_command_list_.D3DSetComputeRootSignature(temporal_aa_root_signature_.Get());
    deferred_command_list_.D3DSetComputeRoot32BitConstants(
        UINT(TemporalAaRootParameter::kConstants), sizeof(constants) / sizeof(uint32_t),
        &constants, 0);
    for (uint32_t i = 0; i < kTables; ++i) {
      deferred_command_list_.D3DSetComputeRootDescriptorTable(
          UINT(TemporalAaRootParameter::kColor) + i, descriptors[i].second);
    }
    SetExternalPipeline(pipeline);
    SubmitBarriers();
    deferred_command_list_.D3DDispatch(groups_x, groups_y, 1);
  };

  ui::d3d12::util::DescriptorCpuGpuHandlePair descriptors[kTables];
  if (!RequestOneUseSingleViewDescriptors(kTables, descriptors)) return false;
  write_srv(upscale.hudless.Get(), upscale.hudless_view_format, descriptors[0].first);
  device->CreateShaderResourceView(swap_texture, &swap_texture_srv, descriptors[1].first);
  write_srv(upscale.hudless.Get(), upscale.hudless_view_format, descriptors[2].first);
  write_uav(eye, DXGI_FORMAT_R8G8B8A8_UNORM, descriptors[3].first);
  write_uav(hud, DXGI_FORMAT_R8G8B8A8_UNORM, descriptors[4].first);
  write_uav(eye, DXGI_FORMAT_R8G8B8A8_UNORM, descriptors[5].first);
  constants.in_size[0] = constants.out_size[0] = width;
  constants.in_size[1] = constants.out_size[1] = height;
  constants.in_size_inv[0] = constants.out_size_inv[0] = 1.0f / float(width);
  constants.in_size_inv[1] = constants.out_size_inv[1] = 1.0f / float(height);
  constants.hud_threshold = 3.0f / 255.0f;
  // Comfort vignette (prepare, flag 16, strength in padding0.x).
  const float vignette = ui::d3d12::openxr_screen::VignetteStrength();
  if (vignette > 0.001f) {
    constants.flags = 16u;
    constants.padding0[0] = vignette;
  }
  // Simultaneous-access images: from common to written to readable; they
  // decay to common when this submission ends.
  PushTransitionBarrier(eye, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  PushTransitionBarrier(hud, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  dispatch(temporal_upscale_prepare_pipeline_.Get(), descriptors, (width + 7) / 8,
           (height + 7) / 8);
  dispatch(temporal_vr_hud_pipeline_.Get(), descriptors, (width + 7) / 8, (height + 7) / 8);
  PushTransitionBarrier(eye, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  PushTransitionBarrier(hud, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  SubmitBarriers();
  ui::d3d12::openxr_screen::Published(tag, submission_fence_, submission_current_);
  ++upscale.frames;
  if ((upscale.frames & 0x3FF) == 1) {
    std::fprintf(stderr,
                 "REX_VR_FRAMES frames=%llu size=%ux%u tag=%08X scene_draws=%llu "
                 "camera_uploads=%llu projection=%.3f,%.3f,%.4f,%.3f\n",
                 static_cast<unsigned long long>(upscale.frames), width, height, tag,
                 static_cast<unsigned long long>(temporal_vr_draws_),
                 static_cast<unsigned long long>(temporal_vr_uploads_),
                 temporal_vr_projection_[0], temporal_vr_projection_[1],
                 temporal_vr_projection_[2], temporal_vr_projection_[3]);
    std::fflush(stderr);
  }
  // Developer check of hand aim: the title camera's forward every 64 frames.
  if (REXCVAR_GET(gpu_vr_log_camera) && (upscale.frames & 63) == 0) {
    std::fprintf(stderr, "REX_VR_CAMERA frame=%llu forward=%.4f,%.4f,%.4f\n",
                 static_cast<unsigned long long>(upscale.frames), temporal_vr_forward_[0],
                 temporal_vr_forward_[1], temporal_vr_forward_[2]);
    std::fflush(stderr);
  }

  // The monitor preview of a simulation.
  ID3D12Resource *left = nullptr, *right = nullptr, *hud_layer = nullptr;
  uint32_t mode = 0;
  if (!ui::d3d12::openxr_screen::PreviewSources(left, right, hud_layer, mode)) return false;
  const uint32_t out_width = mode == 8 ? width * 2 : width;
  const uint32_t out_height = height;
  if (upscale.output) {
    const D3D12_RESOURCE_DESC desc = upscale.output->GetDesc();
    if (desc.Width != out_width || desc.Height != out_height) {
      upscale.output->AddRef();
      resources_for_deletion_.emplace_back(submission_current_, upscale.output.Get());
      upscale.output.Reset();
    }
  }
  if (!upscale.output) {
    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = out_width;
    desc.Height = out_height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    if (FAILED(device->CreateCommittedResource(
            &ui::d3d12::util::kHeapPropertiesDefault, provider.GetHeapFlagCreateNotZeroed(), &desc,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr,
            IID_PPV_ARGS(&upscale.output)))) {
      return false;
    }
  }
  ui::d3d12::util::DescriptorCpuGpuHandlePair preview[kTables];
  if (!RequestOneUseSingleViewDescriptors(kTables, preview)) return false;
  write_srv(left, DXGI_FORMAT_R8G8B8A8_UNORM, preview[0].first);
  write_srv(right, DXGI_FORMAT_R8G8B8A8_UNORM, preview[1].first);
  write_srv(hud_layer, DXGI_FORMAT_R8G8B8A8_UNORM, preview[2].first);
  write_uav(upscale.output.Get(), DXGI_FORMAT_R8G8B8A8_UNORM, preview[3].first);
  write_uav(upscale.output.Get(), DXGI_FORMAT_R8G8B8A8_UNORM, preview[4].first);
  write_uav(upscale.output.Get(), DXGI_FORMAT_R8G8B8A8_UNORM, preview[5].first);
  constants.out_size[0] = out_width;
  constants.out_size[1] = out_height;
  constants.out_size_inv[0] = 1.0f / float(out_width);
  constants.out_size_inv[1] = 1.0f / float(out_height);
  constants.flags = mode;
  constants.padding0[0] = 0.0f;
  PushTransitionBarrier(upscale.output.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  dispatch(temporal_vr_preview_pipeline_.Get(), preview, (out_width + 7) / 8,
           (out_height + 7) / 8);
  PushTransitionBarrier(upscale.output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  SubmitBarriers();
  swap_texture = upscale.output.Get();
  swap_texture_srv = {};
  swap_texture_srv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  swap_texture_srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
  swap_texture_srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  swap_texture_srv.Texture2D.MipLevels = 1;
  width = out_width;
  height = out_height;
  return true;
}

// VR stereo (experimental): the next frame's eye and head view. The scene
// draws' camera rows (c0..c3, clip = M * (p - C, 1)) are rewritten at upload
// to M' = T * M with T = P_eye * V * P^-1: P the title's projection (from the
// last voted world camera), V the eye relative to the title's camera (head
// rotation, head position, half the eye distance), P_eye the headset's
// projection. Host constants only: guest memory and the title's culling keep
// its own camera.
void D3D12CommandProcessor::TemporalVrNextFrame() {
  namespace xr = ui::d3d12::openxr_screen;
  // The swap's frame (TemporalVrAtSwap runs after this, in IssueSwap).
  temporal_vr_finished_tag_ = temporal_vr_stereo_ ? temporal_vr_tag_ : 0u;
  temporal_vr_stereo_ = false;
  xr::HeadView head;
  if (!temporal_vr_projection_valid_ || !xr::LatestHeadView(head)) return;
  temporal_vr_eye_ ^= 1u;
  const uint32_t eye = temporal_vr_eye_;
  temporal_vr_tag_ = xr::kFrameTagValid | (eye ? xr::kFrameTagRightEye : 0u) |
                     (head.id & xr::kFrameTagIdMask);
  const double sx = temporal_vr_projection_[0], sy = temporal_vr_projection_[1];
  const double a = temporal_vr_projection_[2], b = temporal_vr_projection_[3];
  if (!(std::fabs(sx) > 1e-6 && std::fabs(sy) > 1e-6 && std::fabs(b) > 1e-9)) return;
  // OpenXR axes (x right, y up, z back) to the title's camera axes (x right,
  // y up, z forward): S = diag(1, 1, -1); R_cam = S R S.
  // With hand aim the title's camera follows the aim: the head relative to
  // it is conj(aim) * head.
  double hq[4] = {head.orientation[0], head.orientation[1], head.orientation[2],
                  head.orientation[3]};
  if (head.aim_valid) {
    const double ax = -head.aim[0], ay = -head.aim[1], az = -head.aim[2], aw = head.aim[3];
    const double bx = hq[0], by = hq[1], bz = hq[2], bw = hq[3];
    hq[0] = aw * bx + ax * bw + ay * bz - az * by;
    hq[1] = aw * by - ax * bz + ay * bw + az * bx;
    hq[2] = aw * bz + ax * by - ay * bx + az * bw;
    hq[3] = aw * bw - ax * bx - ay * by - az * bz;
  }
  const double qx = hq[0], qy = hq[1], qz = hq[2], qw = hq[3];
  double r[3][3] = {
      {1 - 2 * (qy * qy + qz * qz), 2 * (qx * qy - qz * qw), 2 * (qx * qz + qy * qw)},
      {2 * (qx * qy + qz * qw), 1 - 2 * (qx * qx + qz * qz), 2 * (qy * qz - qx * qw)},
      {2 * (qx * qz - qy * qw), 2 * (qy * qz + qx * qw), 1 - 2 * (qx * qx + qy * qy)}};
  const double s[3] = {1.0, 1.0, -1.0};
  for (int i = 0; i < 3; ++i) {
    for (int j = 0; j < 3; ++j) r[i][j] *= s[i] * s[j];
  }
  // The eye in the camera frame (title units).
  const double units = REXCVAR_GET(gpu_vr_units_per_metre);
  double eye_head[3], e[3];
  for (int i = 0; i < 3; ++i) eye_head[i] = head.eye_offset[eye][i] * s[i];
  for (int i = 0; i < 3; ++i) {
    e[i] = (r[i][0] * eye_head[0] + r[i][1] * eye_head[1] + r[i][2] * eye_head[2] +
            head.position[i] * s[i]) *
           units;
  }
  // V = [R^T, -R^T e; 0 0 0 1] on homogeneous view coordinates.
  double v[4][4] = {};
  for (int i = 0; i < 3; ++i) {
    for (int j = 0; j < 3; ++j) v[i][j] = r[j][i];
    v[i][3] = -(r[0][i] * e[0] + r[1][i] * e[1] + r[2][i] * e[2]);
  }
  v[3][3] = 1.0;
  const double p_inverse[4][4] = {{1.0 / sx, 0, 0, 0},
                                  {0, 1.0 / sy, 0, 0},
                                  {0, 0, 0, 1.0},
                                  {0, 0, 1.0 / b, -a / b}};
  double eye_sx = sx, eye_sy = sy;
  if (head.tan_half_fov_x > 0.0f) {
    eye_sx = 1.0 / double(head.tan_half_fov_x);
    eye_sy = eye_sx * sy / sx;
  }
  const double p_eye[4][4] = {{eye_sx, 0, 0, 0}, {0, eye_sy, 0, 0}, {0, 0, a, b}, {0, 0, 1, 0}};
  double vp[4][4] = {};
  for (int i = 0; i < 4; ++i) {
    for (int j = 0; j < 4; ++j) {
      for (int k = 0; k < 4; ++k) vp[i][j] += v[i][k] * p_inverse[k][j];
    }
  }
  for (int i = 0; i < 4; ++i) {
    for (int j = 0; j < 4; ++j) {
      double sum = 0.0;
      for (int k = 0; k < 4; ++k) sum += p_eye[i][k] * vp[k][j];
      temporal_vr_clip_[i * 4 + j] = sum;
    }
  }
  temporal_vr_stereo_ = true;
}

// The upload of a scene draw's camera rows (see TemporalVrNextFrame).
void D3D12CommandProcessor::TemporalVrRewriteCamera(float* rows) const {
  // c0..c3 (the shader may not read c4..c7, so only the rows are known).
  float c[16];
  std::memcpy(c, rows, sizeof(c));
  if (c[3] != 0.0f || c[7] != 0.0f || c[15] != 0.0f) return;
  float out[16];
  for (int i = 0; i < 4; ++i) {
    for (int j = 0; j < 4; ++j) {
      double sum = 0.0;
      for (int k = 0; k < 4; ++k) sum += temporal_vr_clip_[i * 4 + k] * double(c[k * 4 + j]);
      out[i * 4 + j] = float(sum);
    }
  }
  std::memcpy(rows, out, sizeof(out));
}

}  // namespace rex::graphics::d3d12
