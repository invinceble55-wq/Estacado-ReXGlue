/**
 * @file        ui/d3d12/streamline_bridge.cpp
 * @brief       Optional NVIDIA Streamline (Reflex / PC Latency markers)
 *
 * @license     BSD 3-Clause License
 */

#include <rex/ui/d3d12/streamline_bridge.h>

#if REX_HAVE_STREAMLINE

#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <mutex>
#include <string>
#include <type_traits>
#include <vector>

#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <SDL3/SDL_system.h>

// sl_pcl.h aliases std::to_underlying (a function) with `using` when
// __cplusplus is exactly C++23, which does not compile; its own fallback
// template is used instead (the headers' other checks are >=, and every
// standard header they include is already included above).
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wbuiltin-macro-redefined"
#pragma push_macro("__cplusplus")
#undef __cplusplus
#define __cplusplus 202002L
#include <sl.h>
#include <sl_dlss_g.h>
#include <sl_pcl.h>
#include <sl_reflex.h>
#pragma pop_macro("__cplusplus")
#pragma clang diagnostic pop

#include <rex/cvar.h>

REXCVAR_DEFINE_INT32(streamline_reflex, 0, "UI/D3D12",
                     "NVIDIA Reflex through NVIDIA Streamline (its signed DLLs beside the "
                     "executable): 0 off (Streamline not loaded), 1 on, 2 on + boost. Loading is "
                     "decided at start; later changes switch the mode.")
    .range(0, 2);
REXCVAR_DEFINE_BOOL(streamline_always_load, false, "UI/D3D12",
                    "Load NVIDIA Streamline even with streamline_reflex 0 (developer latency "
                    "measurements: PC Latency reports with Reflex off)");
REXCVAR_DEFINE_BOOL(streamline_frame_generation_only_generated, false, "UI/D3D12",
                    "Frame Generation test: show only the generated frames (NVIDIA's debug "
                    "flag, for checking them in captures)");
REXCVAR_DEFINE_BOOL(streamline_frame_generation_placeholders, false, "UI/D3D12",
                    "Frame Generation test: placeholder inputs (no motion, far depth) for "
                    "frames without tracked inputs instead of turning it off");
REXCVAR_DEFINE_INT32(streamline_frame_generation, 0, "UI/D3D12",
                     "NVIDIA DLSS Frame Generation through Streamline (development test with "
                     "placeholder inputs): frames generated per rendered frame, 0 off, 1 (2x) to "
                     "3 (4x). Decided at start; Reflex is on with it.")
    .range(0, 3);

namespace rex::ui::d3d12::streamline {

// NVIDIA's check from sl_security.h (streamline_signature.cpp).
bool VerifyStreamlineSignature(const wchar_t* path);

namespace {

// The project id of the DLSS (NGX) path as well.
constexpr char kProjectId[] = "6d2c8f4e-3b7a-4e1d-9a5f-2c0e7b1d4a93";

struct Api {
  HMODULE module = nullptr;
  PFun_slInit* init = nullptr;
  PFun_slShutdown* shutdown = nullptr;
  PFun_slSetD3DDevice* set_d3d_device = nullptr;
  PFun_slUpgradeInterface* upgrade_interface = nullptr;
  PFun_slGetNativeInterface* get_native_interface = nullptr;
  PFun_slGetFeatureFunction* get_feature_function = nullptr;
  PFun_slGetNewFrameToken* get_new_frame_token = nullptr;
  PFun_slIsFeatureSupported* is_feature_supported = nullptr;
  PFun_slReflexSleep* reflex_sleep = nullptr;
  PFun_slReflexSetOptions* reflex_set_options = nullptr;
  PFun_slReflexGetState* reflex_get_state = nullptr;
  PFun_slPCLSetMarker* pcl_set_marker = nullptr;
  PFun_slPCLGetState* pcl_get_state = nullptr;
  PFun_slSetTagForFrame* set_tag_for_frame = nullptr;
  PFun_slSetConstants* set_constants = nullptr;
  PFun_slDLSSGSetOptions* dlssg_set_options = nullptr;
  PFun_slDLSSGGetState* dlssg_get_state = nullptr;
};

Api api;
// slInit succeeded and Shutdown has not run.
std::atomic<bool> active{false};
// The feature functions are known; the per-frame calls work.
std::atomic<bool> markers_ready{false};
std::atomic<int32_t> calls_in_flight{0};
bool initialized_once = false;
std::mutex setup_mutex;
// Kept for Streamline, which may hold the pointers.
std::wstring plugin_directory;
std::wstring log_directory;
const wchar_t* plugin_paths[1] = {};

IDXGIFactory2* proxy_factory = nullptr;
ID3D12Device* proxy_device = nullptr;
ID3D12CommandQueue* proxy_queue = nullptr;

// Frame tokens come from a ring of six that markers of other threads
// advance, so taking a token and using it are done under one lock.
std::mutex marker_mutex;
// PC Latency ping: a window message sent while a latency tool listens.
UINT stats_message = 0;
std::atomic<bool> ping_pending{false};
// Producer thread (mode changes under setup_mutex).
std::atomic<int32_t> applied_mode{-1};
uint32_t ready_attempts = 0;
std::atomic<uint32_t> sleeps{0};

// Frame Generation (development test). Requested at start, supported by the
// adapter, and its functions found.
uint32_t frame_generation_frames = 0;
bool frame_generation_supported = false;
std::atomic<bool> frame_generation_ready{false};
ID3D12Device* native_device_for_inputs = nullptr;
// Presenter thread: placeholder depth (R32_FLOAT, 0 = far with reversed
// depth) and motion vectors (R16G16_FLOAT, zero) at the swap chain size.
Microsoft::WRL::ComPtr<ID3D12Resource> placeholder_depth;
Microsoft::WRL::ComPtr<ID3D12Resource> placeholder_motion;
uint32_t placeholder_width = 0;
uint32_t placeholder_height = 0;
bool frame_generation_options_set = false;
bool frame_generation_on = false;
bool frame_generation_reset = true;
uint32_t frame_generation_presents = 0;
// Consecutive presents of tracked gameplay frames (Frame Generation turns on
// after a few, off at once without them).
uint32_t frame_generation_gameplay_presents = 0;

// Frame inputs from the command processor by frame id (frames are presented
// in order within a few frames of their swap).
struct FrameInputs {
  uint32_t frame = 0;
  Microsoft::WRL::ComPtr<ID3D12Resource> depth;
  Microsoft::WRL::ComPtr<ID3D12Resource> motion;
  uint32_t width = 0;
  uint32_t height = 0;
  FrameGenerationCamera camera{};
};
std::mutex frame_inputs_mutex;
std::array<FrameInputs, 8> frame_inputs;
uint32_t frames_with_inputs = 0;
uint32_t frames_without_inputs = 0;
// Frames DLSS-G presents per rendered frame while on (0 when off), for the
// overlay's honest displayed frame rate.
std::atomic<uint32_t> frame_generation_presented_per_frame{0};

// Counts a per-frame call so Shutdown can wait for it to leave Streamline.
class CallScope {
 public:
  CallScope() {
    calls_in_flight.fetch_add(1, std::memory_order_acq_rel);
    entered_ = markers_ready.load(std::memory_order_acquire);
    if (!entered_) calls_in_flight.fetch_sub(1, std::memory_order_acq_rel);
  }
  ~CallScope() {
    if (entered_) calls_in_flight.fetch_sub(1, std::memory_order_acq_rel);
  }
  explicit operator bool() const { return entered_; }

 private:
  bool entered_;
};

const char* ModeName(int32_t mode) {
  return mode >= 2 ? "on_boost" : mode == 1 ? "on" : "off";
}

void LogMessage(sl::LogType type, const char* message) {
  // Warnings and errors always; information only at the start.
  static std::atomic<uint32_t> info_lines{0};
  if (!message ||
      (type == sl::LogType::eInfo && info_lines.fetch_add(1, std::memory_order_relaxed) >= 200)) {
    return;
  }
  size_t length = std::strlen(message);
  while (length && (message[length - 1] == '\n' || message[length - 1] == '\r')) --length;
  const char* kind = type == sl::LogType::eError  ? "error"
                     : type == sl::LogType::eWarn ? "warn"
                                                  : "info";
  std::fprintf(stderr, "REX_STREAMLINE_LOG %s %.*s\n", kind, int(length), message);
  std::fflush(stderr);
}

bool Fail(const char* stage, int code) {
  std::fprintf(stderr, "REX_STREAMLINE init=failed stage=%s code=%d\n", stage, code);
  std::fflush(stderr);
  return false;
}

template <typename Function>
bool LoadExport(Function*& function, const char* name) {
  function = reinterpret_cast<Function*>(GetProcAddress(api.module, name));
  return function != nullptr;
}

template <typename Function>
bool LoadFeatureFunction(Function*& function, sl::Feature feature, const char* name) {
  void* address = nullptr;
  if (api.get_feature_function(feature, name, address) != sl::Result::eOk || !address) {
    return false;
  }
  function = reinterpret_cast<Function*>(address);
  return true;
}

std::wstring ExecutableDirectory() {
  std::wstring path(32768, L'\0');
  const DWORD length = GetModuleFileNameW(nullptr, path.data(), DWORD(path.size()));
  if (!length || length >= path.size()) return {};
  path.resize(length);
  const size_t separator = path.find_last_of(L"\\/");
  return separator == std::wstring::npos ? std::wstring() : path.substr(0, separator);
}

bool SDLCALL WindowsMessageHook(void*, MSG* message) {
  if (stats_message && message && message->message == stats_message) {
    ping_pending.store(true, std::memory_order_relaxed);
  }
  return true;
}

// Under setup_mutex.
void ApplyReflexMode(int32_t mode) {
  // DLSS Frame Generation needs Reflex low latency on.
  if (frame_generation_frames && mode < 1) mode = 1;
  sl::ReflexOptions options;
  options.mode = mode >= 2   ? sl::ReflexMode::eLowLatencyWithBoost
                 : mode == 1 ? sl::ReflexMode::eLowLatency
                             : sl::ReflexMode::eOff;
  const sl::Result result = api.reflex_set_options(options);
  applied_mode.store(mode, std::memory_order_relaxed);
  static sl::ReflexState state;
  state = sl::ReflexState();
  const bool have_state = api.reflex_get_state(state) == sl::Result::eOk;
  std::fprintf(stderr, "REX_STREAMLINE_REFLEX mode=%s result=%d low_latency_available=%d\n",
               ModeName(mode), int(result), have_state && state.lowLatencyAvailable ? 1 : 0);
  std::fflush(stderr);
}

// Under setup_mutex: the feature functions exist once the device is set.
bool TryReadyLocked() {
  if (markers_ready.load(std::memory_order_acquire)) return true;
  if (!active.load(std::memory_order_acquire)) return false;
  if (!LoadFeatureFunction(api.reflex_sleep, sl::kFeatureReflex, "slReflexSleep") ||
      !LoadFeatureFunction(api.reflex_set_options, sl::kFeatureReflex, "slReflexSetOptions") ||
      !LoadFeatureFunction(api.reflex_get_state, sl::kFeatureReflex, "slReflexGetState") ||
      !LoadFeatureFunction(api.pcl_set_marker, sl::kFeaturePCL, "slPCLSetMarker") ||
      !LoadFeatureFunction(api.pcl_get_state, sl::kFeaturePCL, "slPCLGetState")) {
    return false;
  }
  sl::PCLState pcl_state;
  if (api.pcl_get_state(pcl_state) == sl::Result::eOk && pcl_state.statsWindowMessage) {
    stats_message = pcl_state.statsWindowMessage;
    SDL_SetWindowsMessageHook(WindowsMessageHook, nullptr);
  }
  if (frame_generation_frames && frame_generation_supported &&
      LoadFeatureFunction(api.dlssg_set_options, sl::kFeatureDLSS_G, "slDLSSGSetOptions") &&
      LoadFeatureFunction(api.dlssg_get_state, sl::kFeatureDLSS_G, "slDLSSGGetState") &&
      api.set_tag_for_frame && api.set_constants) {
    frame_generation_ready.store(true, std::memory_order_release);
  }
  std::fprintf(stderr, "REX_STREAMLINE_FRAMEGEN requested=%u supported=%d ready=%d\n",
               frame_generation_frames, frame_generation_supported ? 1 : 0,
               frame_generation_ready.load(std::memory_order_relaxed) ? 1 : 0);
  ApplyReflexMode(REXCVAR_GET(streamline_reflex));
  markers_ready.store(true, std::memory_order_release);
  std::fprintf(stderr, "REX_STREAMLINE markers=ready pcl_ping_message=%u\n", stats_message);
  std::fflush(stderr);
  return true;
}

}  // namespace

bool InitializeBeforeDevice() {
  if (active.load(std::memory_order_acquire)) return true;
  frame_generation_frames = uint32_t(std::clamp(REXCVAR_GET(streamline_frame_generation), 0, 3));
  if (REXCVAR_GET(streamline_reflex) <= 0 && !REXCVAR_GET(streamline_always_load) &&
      !frame_generation_frames) {
    return false;
  }
  if (initialized_once) return Fail("already_shut_down", 0);
  initialized_once = true;
  plugin_directory = ExecutableDirectory();
  const std::wstring interposer = plugin_directory + L"\\sl.interposer.dll";
  if (plugin_directory.empty() ||
      GetFileAttributesW(interposer.c_str()) == INVALID_FILE_ATTRIBUTES) {
    return Fail("missing_interposer", 0);
  }
  if (!VerifyStreamlineSignature(interposer.c_str())) return Fail("signature", 0);
  api.module = LoadLibraryExW(interposer.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
  if (!api.module) return Fail("load", int(GetLastError()));
  if (!LoadExport(api.init, "slInit") || !LoadExport(api.shutdown, "slShutdown") ||
      !LoadExport(api.set_d3d_device, "slSetD3DDevice") ||
      !LoadExport(api.upgrade_interface, "slUpgradeInterface") ||
      !LoadExport(api.get_native_interface, "slGetNativeInterface") ||
      !LoadExport(api.get_feature_function, "slGetFeatureFunction") ||
      !LoadExport(api.get_new_frame_token, "slGetNewFrameToken") ||
      !LoadExport(api.is_feature_supported, "slIsFeatureSupported")) {
    return Fail("exports", 0);
  }
  LoadExport(api.set_tag_for_frame, "slSetTagForFrame");
  LoadExport(api.set_constants, "slSetConstants");

  log_directory = plugin_directory + L"\\logs";
  const DWORD log_attributes = GetFileAttributesW(log_directory.c_str());
  const bool log_to_file = log_attributes != INVALID_FILE_ATTRIBUTES &&
                           (log_attributes & FILE_ATTRIBUTE_DIRECTORY);
  plugin_paths[0] = plugin_directory.c_str();
  static const sl::Feature kFeatures[] = {sl::kFeatureReflex, sl::kFeaturePCL,
                                          sl::kFeatureDLSS_G};
  sl::Preferences preferences;
  preferences.showConsole = false;
  preferences.logLevel = sl::LogLevel::eDefault;
  preferences.pathsToPlugins = plugin_paths;
  preferences.numPathsToPlugins = 1;
  preferences.pathToLogsAndData = log_to_file ? log_directory.c_str() : nullptr;
  preferences.logMessageCallback = LogMessage;
  // Manual hooking: only the swap chain, its factory and its queue go through
  // Streamline's proxies. No over-the-air downloads or downloaded plugins.
  // Tags carry their frame (slSetTagForFrame).
  preferences.flags = sl::PreferenceFlags::eDisableCLStateTracking |
                      sl::PreferenceFlags::eUseManualHooking |
                      sl::PreferenceFlags::eUseFrameBasedResourceTagging;
  preferences.featuresToLoad = kFeatures;
  // Frame Generation only when requested.
  preferences.numFeaturesToLoad = uint32_t(std::size(kFeatures)) - (frame_generation_frames ? 0 : 1);
  preferences.engine = sl::EngineType::eCustom;
  preferences.engineVersion = "1.0";
  preferences.projectId = kProjectId;
  preferences.renderAPI = sl::RenderAPI::eD3D12;
  const sl::Result result = api.init(preferences, sl::kSDKVersion);
  if (result != sl::Result::eOk) return Fail("slInit", int(result));
  active.store(true, std::memory_order_release);
  std::fprintf(stderr, "REX_STREAMLINE init=ok sdk=%d.%d.%d reflex=%s log_file=%d\n",
               SL_VERSION_MAJOR, SL_VERSION_MINOR, SL_VERSION_PATCH,
               ModeName(REXCVAR_GET(streamline_reflex)), log_to_file ? 1 : 0);
  std::fflush(stderr);
  return true;
}

bool IsActive() { return active.load(std::memory_order_acquire); }

void OnDeviceCreated(ID3D12Device* native_device, uint64_t adapter_luid) {
  if (!active.load(std::memory_order_acquire) || !native_device) return;
  const sl::Result result = api.set_d3d_device(native_device);
  LUID luid;
  luid.LowPart = DWORD(adapter_luid);
  luid.HighPart = LONG(adapter_luid >> 32);
  sl::AdapterInfo adapter;
  adapter.deviceLUID = reinterpret_cast<uint8_t*>(&luid);
  adapter.deviceLUIDSizeInBytes = uint32_t(sizeof(luid));
  const sl::Result reflex = api.is_feature_supported(sl::kFeatureReflex, adapter);
  const sl::Result pcl = api.is_feature_supported(sl::kFeaturePCL, adapter);
  const sl::Result dlssg = frame_generation_frames
                               ? api.is_feature_supported(sl::kFeatureDLSS_G, adapter)
                               : sl::Result::eErrorFeatureMissing;
  frame_generation_supported = dlssg == sl::Result::eOk;
  native_device_for_inputs = native_device;
  // Results: 0 = supported, otherwise sl::Result.
  std::fprintf(stderr, "REX_STREAMLINE device result=%d reflex=%d pcl=%d dlssg=%d\n",
               int(result), int(reflex), int(pcl), int(dlssg));
  std::fflush(stderr);
  if (result == sl::Result::eOk) {
    std::lock_guard<std::mutex> lock(setup_mutex);
    TryReadyLocked();
  }
}

bool CreateDirectQueue(ID3D12Device* native_device, const D3D12_COMMAND_QUEUE_DESC& desc,
                       ID3D12CommandQueue** native_queue_out) {
  *native_queue_out = nullptr;
  if (!active.load(std::memory_order_acquire) || !native_device) return false;
  if (!proxy_device) {
    void* device = native_device;
    if (api.upgrade_interface(&device) != sl::Result::eOk || device == native_device) {
      std::fprintf(stderr, "REX_STREAMLINE queue=failed stage=device_proxy\n");
      std::fflush(stderr);
      return false;
    }
    proxy_device = static_cast<ID3D12Device*>(device);
  }
  ID3D12CommandQueue* queue = nullptr;
  if (FAILED(proxy_device->CreateCommandQueue(&desc, IID_PPV_ARGS(&queue)))) return false;
  void* native = nullptr;
  if (api.get_native_interface(queue, &native) != sl::Result::eOk || !native) {
    queue->Release();
    std::fprintf(stderr, "REX_STREAMLINE queue=failed stage=native_queue\n");
    std::fflush(stderr);
    return false;
  }
  if (proxy_queue) proxy_queue->Release();
  proxy_queue = queue;
  *native_queue_out = static_cast<ID3D12CommandQueue*>(native);
  return true;
}

void UpgradeFactory(IDXGIFactory2* native_factory) {
  if (!active.load(std::memory_order_acquire) || !native_factory || proxy_factory) return;
  void* factory = native_factory;
  if (api.upgrade_interface(&factory) != sl::Result::eOk || factory == native_factory) {
    std::fprintf(stderr, "REX_STREAMLINE factory=failed\n");
    std::fflush(stderr);
    return;
  }
  IUnknown* proxy = static_cast<IUnknown*>(factory);
  IDXGIFactory2* proxy_factory_2 = nullptr;
  if (SUCCEEDED(proxy->QueryInterface(IID_PPV_ARGS(&proxy_factory_2)))) {
    proxy_factory = proxy_factory_2;
  }
  proxy->Release();
}

IDXGIFactory2* SwapChainFactory() { return proxy_factory; }

ID3D12CommandQueue* SwapChainQueue() { return proxy_queue; }

void Shutdown() {
  if (!active.exchange(false, std::memory_order_acq_rel)) return;
  markers_ready.store(false, std::memory_order_release);
  frame_generation_ready.store(false, std::memory_order_release);
  // Let per-frame calls already inside Streamline (a Reflex sleep) return.
  for (int i = 0; i < 200 && calls_in_flight.load(std::memory_order_acquire) > 0; ++i) {
    Sleep(1);
  }
  if (stats_message) SDL_SetWindowsMessageHook(nullptr, nullptr);
  // The proxies hold their native interfaces; release them first.
  if (proxy_queue) {
    proxy_queue->Release();
    proxy_queue = nullptr;
  }
  if (proxy_device) {
    proxy_device->Release();
    proxy_device = nullptr;
  }
  if (proxy_factory) {
    proxy_factory->Release();
    proxy_factory = nullptr;
  }
  placeholder_depth.Reset();
  placeholder_motion.Reset();
  const sl::Result result = api.shutdown();
  std::fprintf(stderr, "REX_STREAMLINE shutdown result=%d frames=%u\n", int(result),
               sleeps.load(std::memory_order_relaxed));
  std::fflush(stderr);
}

void ReflexSleep(uint32_t frame) {
  if (!markers_ready.load(std::memory_order_acquire)) {
    // Not ready at device creation: retry now and then.
    if (!active.load(std::memory_order_acquire) || (ready_attempts++ & 0xFF) != 0) return;
    std::lock_guard<std::mutex> lock(setup_mutex);
    if (!TryReadyLocked()) return;
  }
  CallScope scope;
  if (!scope || !frame) return;
  const int32_t mode = REXCVAR_GET(streamline_reflex);
  if (mode != applied_mode.load(std::memory_order_relaxed)) {
    std::lock_guard<std::mutex> lock(setup_mutex);
    ApplyReflexMode(mode);
  }
  sl::FrameToken* token = nullptr;
  {
    std::lock_guard<std::mutex> lock(marker_mutex);
    if (api.get_new_frame_token(token, &frame) != sl::Result::eOk) token = nullptr;
  }
  // The sleep does not read the token, so other threads' markers may run.
  if (token) api.reflex_sleep(*token);
  sleeps.fetch_add(1, std::memory_order_relaxed);
}

void SetMarker(Marker marker, uint32_t frame) {
  if (!frame) return;
  CallScope scope;
  if (!scope) return;
  std::lock_guard<std::mutex> lock(marker_mutex);
  sl::FrameToken* token = nullptr;
  if (api.get_new_frame_token(token, &frame) == sl::Result::eOk && token) {
    api.pcl_set_marker(sl::PCLMarker(uint32_t(marker)), *token);
  }
}

bool TakeLatencyPing() { return ping_pending.exchange(false, std::memory_order_relaxed); }

bool FrameGenerationActive() { return frame_generation_ready.load(std::memory_order_acquire); }

uint32_t FrameGenerationPresentedPerFrame() {
  return frame_generation_presented_per_frame.load(std::memory_order_relaxed);
}

namespace {

bool EnsurePlaceholders(uint32_t width, uint32_t height) {
  if (placeholder_depth && placeholder_width == width && placeholder_height == height) {
    return true;
  }
  placeholder_depth.Reset();
  placeholder_motion.Reset();
  if (!native_device_for_inputs || !width || !height) return false;
  D3D12_HEAP_PROPERTIES heap = {};
  heap.Type = D3D12_HEAP_TYPE_DEFAULT;
  D3D12_RESOURCE_DESC desc = {};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  desc.Width = width;
  desc.Height = height;
  desc.DepthOrArraySize = 1;
  desc.MipLevels = 1;
  desc.SampleDesc.Count = 1;
  desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
  // Committed resources start zeroed: depth 0 (far, reversed), no motion.
  desc.Format = DXGI_FORMAT_R32_FLOAT;
  if (FAILED(native_device_for_inputs->CreateCommittedResource(
          &heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COMMON, nullptr,
          IID_PPV_ARGS(&placeholder_depth)))) {
    return false;
  }
  desc.Format = DXGI_FORMAT_R16G16_FLOAT;
  if (FAILED(native_device_for_inputs->CreateCommittedResource(
          &heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COMMON, nullptr,
          IID_PPV_ARGS(&placeholder_motion)))) {
    placeholder_depth.Reset();
    return false;
  }
  placeholder_width = width;
  placeholder_height = height;
  frame_generation_options_set = false;
  frame_generation_reset = true;
  return true;
}

sl::float4x4 Identity() {
  sl::float4x4 m;
  for (uint32_t i = 0; i < 4; ++i) {
    m.setRow(i, sl::float4(i == 0 ? 1.0f : 0.0f, i == 1 ? 1.0f : 0.0f, i == 2 ? 1.0f : 0.0f,
                           i == 3 ? 1.0f : 0.0f));
  }
  return m;
}

// Streamline takes row vectors (v * M): the transpose of a row-major matrix
// for column vectors.
sl::float4x4 Transposed(const double* m) {
  sl::float4x4 out;
  for (uint32_t r = 0; r < 4; ++r) {
    out.setRow(r, sl::float4(float(m[0 * 4 + r]), float(m[1 * 4 + r]), float(m[2 * 4 + r]),
                             float(m[3 * 4 + r])));
  }
  return out;
}

bool Inverted(const double* a, double* out) {
  double inv[16];
  inv[0] = a[5] * a[10] * a[15] - a[5] * a[11] * a[14] - a[9] * a[6] * a[15] +
           a[9] * a[7] * a[14] + a[13] * a[6] * a[11] - a[13] * a[7] * a[10];
  inv[4] = -a[4] * a[10] * a[15] + a[4] * a[11] * a[14] + a[8] * a[6] * a[15] -
           a[8] * a[7] * a[14] - a[12] * a[6] * a[11] + a[12] * a[7] * a[10];
  inv[8] = a[4] * a[9] * a[15] - a[4] * a[11] * a[13] - a[8] * a[5] * a[15] +
           a[8] * a[7] * a[13] + a[12] * a[5] * a[11] - a[12] * a[7] * a[9];
  inv[12] = -a[4] * a[9] * a[14] + a[4] * a[10] * a[13] + a[8] * a[5] * a[14] -
            a[8] * a[6] * a[13] - a[12] * a[5] * a[10] + a[12] * a[6] * a[9];
  inv[1] = -a[1] * a[10] * a[15] + a[1] * a[11] * a[14] + a[9] * a[2] * a[15] -
           a[9] * a[3] * a[14] - a[13] * a[2] * a[11] + a[13] * a[3] * a[10];
  inv[5] = a[0] * a[10] * a[15] - a[0] * a[11] * a[14] - a[8] * a[2] * a[15] +
           a[8] * a[3] * a[14] + a[12] * a[2] * a[11] - a[12] * a[3] * a[10];
  inv[9] = -a[0] * a[9] * a[15] + a[0] * a[11] * a[13] + a[8] * a[1] * a[15] -
           a[8] * a[3] * a[13] - a[12] * a[1] * a[11] + a[12] * a[3] * a[9];
  inv[13] = a[0] * a[9] * a[14] - a[0] * a[10] * a[13] - a[8] * a[1] * a[14] +
            a[8] * a[2] * a[13] + a[12] * a[1] * a[10] - a[12] * a[2] * a[9];
  inv[2] = a[1] * a[6] * a[15] - a[1] * a[7] * a[14] - a[5] * a[2] * a[15] +
           a[5] * a[3] * a[14] + a[13] * a[2] * a[7] - a[13] * a[3] * a[6];
  inv[6] = -a[0] * a[6] * a[15] + a[0] * a[7] * a[14] + a[4] * a[2] * a[15] -
           a[4] * a[3] * a[14] - a[12] * a[2] * a[7] + a[12] * a[3] * a[6];
  inv[10] = a[0] * a[5] * a[15] - a[0] * a[7] * a[13] - a[4] * a[1] * a[15] +
            a[4] * a[3] * a[13] + a[12] * a[1] * a[7] - a[12] * a[3] * a[5];
  inv[14] = -a[0] * a[5] * a[14] + a[0] * a[6] * a[13] + a[4] * a[1] * a[14] -
            a[4] * a[2] * a[13] - a[12] * a[1] * a[6] + a[12] * a[2] * a[5];
  inv[3] = -a[1] * a[6] * a[11] + a[1] * a[7] * a[10] + a[5] * a[2] * a[11] -
           a[5] * a[3] * a[10] - a[9] * a[2] * a[7] + a[9] * a[3] * a[6];
  inv[7] = a[0] * a[6] * a[11] - a[0] * a[7] * a[10] - a[4] * a[2] * a[11] +
           a[4] * a[3] * a[10] + a[8] * a[2] * a[7] - a[8] * a[3] * a[6];
  inv[11] = -a[0] * a[5] * a[11] + a[0] * a[7] * a[9] + a[4] * a[1] * a[11] -
            a[4] * a[3] * a[9] - a[8] * a[1] * a[7] + a[8] * a[3] * a[5];
  inv[15] = a[0] * a[5] * a[10] - a[0] * a[6] * a[9] - a[4] * a[1] * a[10] +
            a[4] * a[2] * a[9] + a[8] * a[1] * a[6] - a[8] * a[2] * a[5];
  const double det = a[0] * inv[0] + a[1] * inv[4] + a[2] * inv[8] + a[3] * inv[12];
  if (!(std::fabs(det) > 1e-30)) return false;
  for (int i = 0; i < 16; ++i) out[i] = inv[i] / det;
  return true;
}

sl::float3 Direction(const float* row) {
  const double length =
      std::sqrt(double(row[0]) * row[0] + double(row[1]) * row[1] + double(row[2]) * row[2]);
  if (!(length > 0.0)) return sl::float3(0.0f, 0.0f, 0.0f);
  return sl::float3(float(row[0] / length), float(row[1] / length), float(row[2] / length));
}

// The frame's constants from its camera (inputs width x height).
sl::Constants CameraConstants(const FrameGenerationCamera& camera, uint32_t width,
                              uint32_t height) {
  sl::Constants constants;
  double view_to_clip[16], clip_to_view[16], to_previous[16], from_previous[16];
  for (int i = 0; i < 16; ++i) {
    view_to_clip[i] = camera.view_to_clip[i];
    to_previous[i] = camera.clip_to_previous_clip[i];
  }
  constants.cameraViewToClip = Transposed(view_to_clip);
  constants.clipToCameraView =
      Inverted(view_to_clip, clip_to_view) ? Transposed(clip_to_view) : Identity();
  constants.clipToLensClip = Identity();
  constants.clipToPrevClip = Transposed(to_previous);
  constants.prevClipToClip =
      Inverted(to_previous, from_previous) ? Transposed(from_previous) : Identity();
  constants.jitterOffset = sl::float2(0.0f, 0.0f);
  constants.mvecScale = sl::float2(1.0f / float(width), 1.0f / float(height));
  constants.cameraPinholeOffset = sl::float2(0.0f, 0.0f);
  constants.cameraPos = sl::float3(camera.position[0], camera.position[1], camera.position[2]);
  // Rows of the view-projection: x (right), y (up), w (forward).
  constants.cameraRight = Direction(&camera.view_to_clip[0]);
  constants.cameraUp = Direction(&camera.view_to_clip[4]);
  constants.cameraFwd = Direction(&camera.view_to_clip[12]);
  constants.cameraNear = camera.near_plane;
  constants.cameraFar = camera.far_plane;
  constants.cameraFOV = camera.fov_y;
  constants.cameraAspectRatio = float(width) / float(height);
  constants.depthInverted = sl::Boolean::eTrue;
  constants.cameraMotionIncluded = sl::Boolean::eTrue;
  constants.motionVectors3D = sl::Boolean::eFalse;
  constants.reset = camera.reset ? sl::Boolean::eTrue : sl::Boolean::eFalse;
  return constants;
}

}  // namespace

void FrameGenerationSetInputs(uint32_t frame, ID3D12Resource* depth, ID3D12Resource* motion,
                              uint32_t width, uint32_t height,
                              const FrameGenerationCamera* camera) {
  if (!frame || !frame_generation_ready.load(std::memory_order_acquire)) return;
  std::lock_guard<std::mutex> lock(frame_inputs_mutex);
  FrameInputs& inputs = frame_inputs[frame % frame_inputs.size()];
  inputs.frame = frame;
  inputs.depth = depth;
  inputs.motion = motion;
  inputs.width = width;
  inputs.height = height;
  inputs.camera = camera ? *camera : FrameGenerationCamera{};
}

void FrameGenerationBeforePresent(uint32_t guest_frame, uint32_t frame, bool gameplay,
                                  uint32_t width, uint32_t height) {
  if (!frame_generation_ready.load(std::memory_order_acquire)) return;
  CallScope scope;
  if (!scope || !EnsurePlaceholders(width, height)) return;
  const sl::ViewportHandle viewport(0u);
  // The image's tracked inputs (none for menus, loading and cut-scene video).
  FrameInputs inputs;
  bool have_inputs = false;
  if (guest_frame) {
    std::lock_guard<std::mutex> lock(frame_inputs_mutex);
    const FrameInputs& stored = frame_inputs[guest_frame % frame_inputs.size()];
    if (stored.frame == guest_frame && stored.depth && stored.motion) {
      inputs = stored;
      have_inputs = true;
    }
  }
  const bool placeholders = REXCVAR_GET(streamline_frame_generation_placeholders);
  if (!have_inputs && !placeholders) gameplay = false;
  // On only for tracked gameplay frames: off at once for loading screens,
  // menus and untracked swaps, on again after a few gameplay presents (with
  // a history reset). Repaints of a presented frame keep the mode.
  if (!guest_frame || !gameplay) {
    frame_generation_gameplay_presents = 0;
  } else if (frame) {
    ++frame_generation_gameplay_presents;
  }
  const bool want_on = frame_generation_on ? frame_generation_gameplay_presents != 0
                                           : frame_generation_gameplay_presents >= 3;
  if (!frame_generation_options_set || want_on != frame_generation_on) {
    sl::DLSSGOptions options;
    options.mode = want_on ? sl::DLSSGMode::eOn : sl::DLSSGMode::eOff;
    options.numFramesToGenerate = frame_generation_frames;
    options.flags = sl::DLSSGFlags::eRetainResourcesWhenOff;
    if (REXCVAR_GET(streamline_frame_generation_only_generated)) {
      options.flags = options.flags | sl::DLSSGFlags::eShowOnlyInterpolatedFrame;
    }
    options.mvecDepthWidth = have_inputs ? inputs.width : width;
    options.mvecDepthHeight = have_inputs ? inputs.height : height;
    options.colorWidth = width;
    options.colorHeight = height;
    const sl::Result result = api.dlssg_set_options(viewport, options);
    frame_generation_options_set = true;
    frame_generation_on = want_on;
    frame_generation_reset = true;
    if (!want_on) frame_generation_presented_per_frame.store(0, std::memory_order_relaxed);
    std::fprintf(stderr,
                 "REX_STREAMLINE_FRAMEGEN options result=%d mode=%s frames=%u size=%ux%u "
                 "frame=%u\n",
                 int(result), want_on ? "on" : "off", frame_generation_frames, width, height,
                 guest_frame);
    std::fflush(stderr);
  }
  if (!frame_generation_on || !frame) return;
  if (have_inputs) {
    ++frames_with_inputs;
  } else {
    ++frames_without_inputs;
  }
  const uint32_t input_width = have_inputs ? inputs.width : width;
  const uint32_t input_height = have_inputs ? inputs.height : height;
  sl::Resource depth(sl::ResourceType::eTex2d,
                     have_inputs ? inputs.depth.Get() : placeholder_depth.Get(),
                     uint32_t(have_inputs ? D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE
                                          : D3D12_RESOURCE_STATE_COMMON));
  sl::Resource motion(sl::ResourceType::eTex2d,
                      have_inputs ? inputs.motion.Get() : placeholder_motion.Get(),
                      uint32_t(have_inputs ? D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE
                                           : D3D12_RESOURCE_STATE_COMMON));
  const sl::Extent extent{0, 0, input_width, input_height};
  const sl::ResourceTag tags[] = {
      sl::ResourceTag(&depth, sl::kBufferTypeDepth, sl::ResourceLifecycle::eValidUntilPresent,
                      &extent),
      sl::ResourceTag(&motion, sl::kBufferTypeMotionVectors,
                      sl::ResourceLifecycle::eValidUntilPresent, &extent),
  };
  sl::Constants constants;
  if (have_inputs) {
    constants = CameraConstants(inputs.camera, input_width, input_height);
    if (frame_generation_reset) constants.reset = sl::Boolean::eTrue;
    frame_generation_reset = false;
  } else {
  constants.cameraViewToClip = Identity();
  constants.clipToCameraView = Identity();
  constants.clipToLensClip = Identity();
  constants.clipToPrevClip = Identity();
  constants.prevClipToClip = Identity();
  constants.jitterOffset = sl::float2(0.0f, 0.0f);
  constants.mvecScale = sl::float2(1.0f / float(width), 1.0f / float(height));
  constants.cameraPinholeOffset = sl::float2(0.0f, 0.0f);
  constants.cameraPos = sl::float3(0.0f, 0.0f, 0.0f);
  constants.cameraUp = sl::float3(0.0f, 1.0f, 0.0f);
  constants.cameraRight = sl::float3(1.0f, 0.0f, 0.0f);
  constants.cameraFwd = sl::float3(0.0f, 0.0f, 1.0f);
  constants.cameraNear = 1.8f;
  constants.cameraFar = 22500.0f;
  constants.cameraFOV = 1.5f;
  constants.cameraAspectRatio = float(width) / float(height);
  constants.depthInverted = sl::Boolean::eTrue;
  constants.cameraMotionIncluded = sl::Boolean::eTrue;
  constants.motionVectors3D = sl::Boolean::eFalse;
  constants.reset = frame_generation_reset ? sl::Boolean::eTrue : sl::Boolean::eFalse;
  frame_generation_reset = false;
  }
  {
    std::lock_guard<std::mutex> lock(marker_mutex);
    sl::FrameToken* token = nullptr;
    if (api.get_new_frame_token(token, &frame) == sl::Result::eOk && token) {
      api.set_tag_for_frame(*token, viewport, tags, uint32_t(std::size(tags)), nullptr);
      api.set_constants(constants, *token, viewport);
    }
  }
  if ((++frame_generation_presents % 60) == 1) {
    sl::DLSSGState state;
    const sl::Result result = api.dlssg_get_state(viewport, state, nullptr);
    frame_generation_presented_per_frame.store(
        result == sl::Result::eOk && state.status == sl::DLSSGStatus::eOk
            ? state.numFramesActuallyPresented
            : 0,
        std::memory_order_relaxed);
  }
  if ((frame_generation_presents % 600) == 1) {
    sl::DLSSGState state;
    const sl::Result result = api.dlssg_get_state(viewport, state, nullptr);
    std::fprintf(stderr,
                 "REX_STREAMLINE_FRAMEGEN state result=%d status=0x%X presented_per_frame=%u "
                 "max_generated=%u vsync_supported=%d vram_mb=%.0f presents=%u inputs=%ux%u "
                 "with_inputs=%u without_inputs=%u\n",
                 int(result), uint32_t(state.status), state.numFramesActuallyPresented,
                 state.numFramesToGenerateMax, state.bIsVsyncSupportAvailable == sl::Boolean::eTrue,
                 double(state.estimatedVRAMUsageInBytes) / (1024.0 * 1024.0),
                 frame_generation_presents, input_width, input_height, frames_with_inputs,
                 frames_without_inputs);
    std::fflush(stderr);
  }
}

// Averages of NVIDIA's latest frame reports with every stage, and the newest
// three reports' stage times from their simulation start (us).
void LogReport() {
  CallScope scope;
  if (!scope) return;
  static sl::ReflexState state;
  state = sl::ReflexState();
  if (api.reflex_get_state(state) != sl::Result::eOk) return;
  uint64_t frames = 0, last_frame = 0;
  uint64_t simulation = 0, submit = 0, present = 0, queue = 0, gpu = 0, total = 0;
  uint64_t gpu_frame = 0, gpu_active = 0;
  if (state.latencyReportAvailable) {
    for (const sl::ReflexReport& report : state.frameReport) {
      if (!report.frameID || !report.simStartTime || report.simEndTime < report.simStartTime ||
          report.renderSubmitEndTime < report.renderSubmitStartTime ||
          report.presentEndTime < report.presentStartTime ||
          report.gpuRenderEndTime < report.gpuRenderStartTime ||
          report.gpuRenderEndTime < report.simStartTime) {
        continue;
      }
      ++frames;
      last_frame = std::max<uint64_t>(last_frame, report.frameID);
      simulation += report.simEndTime - report.simStartTime;
      submit += report.renderSubmitEndTime - report.renderSubmitStartTime;
      present += report.presentEndTime - report.presentStartTime;
      if (report.osRenderQueueEndTime >= report.osRenderQueueStartTime) {
        queue += report.osRenderQueueEndTime - report.osRenderQueueStartTime;
      }
      gpu += report.gpuRenderEndTime - report.gpuRenderStartTime;
      total += report.gpuRenderEndTime - report.simStartTime;
      gpu_frame += report.gpuFrameTimeUs;
      gpu_active += report.gpuActiveRenderTimeUs;
    }
  }
  const double n = frames ? double(frames) : 1.0;
  std::fprintf(stderr,
               "REX_STREAMLINE_LATENCY mode=%s low_latency_available=%d report=%d frames=%llu "
               "last_frame=%llu sim_us=%.0f submit_us=%.0f present_us=%.0f queue_us=%.0f "
               "gpu_us=%.0f gpu_frame_us=%.0f gpu_active_us=%.0f sim_start_to_gpu_end_us=%.0f\n",
               ModeName(applied_mode.load(std::memory_order_relaxed)),
               state.lowLatencyAvailable ? 1 : 0, state.latencyReportAvailable ? 1 : 0,
               static_cast<unsigned long long>(frames), static_cast<unsigned long long>(last_frame),
               simulation / n, submit / n, present / n, queue / n, gpu / n, gpu_frame / n,
               gpu_active / n, total / n);
  const sl::ReflexReport* newest[3] = {};
  for (const sl::ReflexReport& report : state.frameReport) {
    if (!report.frameID || !report.simStartTime) continue;
    for (int i = 0; i < 3; ++i) {
      if (!newest[i] || report.frameID > newest[i]->frameID) {
        for (int j = 2; j > i; --j) newest[j] = newest[j - 1];
        newest[i] = &report;
        break;
      }
    }
  }
  auto since = [](uint64_t time, uint64_t start) {
    return time ? static_cast<long long>(time) - static_cast<long long>(start) : -1ll;
  };
  for (const sl::ReflexReport* report : newest) {
    if (!report) continue;
    const uint64_t s = report->simStartTime;
    std::fprintf(stderr,
                 "REX_STREAMLINE_FRAME id=%llu sim_end=%lld submit=%lld..%lld present=%lld..%lld "
                 "driver=%lld..%lld os_queue=%lld..%lld gpu=%lld..%lld\n",
                 static_cast<unsigned long long>(report->frameID), since(report->simEndTime, s),
                 since(report->renderSubmitStartTime, s), since(report->renderSubmitEndTime, s),
                 since(report->presentStartTime, s), since(report->presentEndTime, s),
                 since(report->driverStartTime, s), since(report->driverEndTime, s),
                 since(report->osRenderQueueStartTime, s), since(report->osRenderQueueEndTime, s),
                 since(report->gpuRenderStartTime, s), since(report->gpuRenderEndTime, s));
  }
  std::fflush(stderr);
}

}  // namespace rex::ui::d3d12::streamline

#else  // !REX_HAVE_STREAMLINE

namespace rex::ui::d3d12::streamline {

bool InitializeBeforeDevice() { return false; }
bool IsActive() { return false; }
void OnDeviceCreated(ID3D12Device*, uint64_t) {}
bool CreateDirectQueue(ID3D12Device*, const D3D12_COMMAND_QUEUE_DESC&,
                       ID3D12CommandQueue** native_queue_out) {
  *native_queue_out = nullptr;
  return false;
}
void UpgradeFactory(IDXGIFactory2*) {}
IDXGIFactory2* SwapChainFactory() { return nullptr; }
ID3D12CommandQueue* SwapChainQueue() { return nullptr; }
void Shutdown() {}
void ReflexSleep(uint32_t) {}
void SetMarker(Marker, uint32_t) {}
bool TakeLatencyPing() { return false; }
void LogReport() {}
bool FrameGenerationActive() { return false; }
uint32_t FrameGenerationPresentedPerFrame() { return 0; }
void FrameGenerationSetInputs(uint32_t, ID3D12Resource*, ID3D12Resource*, uint32_t, uint32_t,
                              const FrameGenerationCamera*) {}
void FrameGenerationBeforePresent(uint32_t, uint32_t, bool, uint32_t, uint32_t) {}

}  // namespace rex::ui::d3d12::streamline

#endif  // REX_HAVE_STREAMLINE
