/**
 * @file        ui/d3d12/openxr_screen.cpp
 * @brief       See openxr_screen.h (VR, docs/VR_PLAN.md).
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#include <rex/ui/d3d12/openxr_screen.h>

#if REX_HAVE_OPENXR

#include <rex/cvar.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include <d3d12.h>
#include <dxgi1_4.h>
#include <windows.h>
#include <wrl/client.h>

#define XR_USE_GRAPHICS_API_D3D12
#define XR_USE_PLATFORM_WIN32
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

REXCVAR_DEFINE_BOOL(display_vr, false, "Display",
                    "Experimental VR: stereo gameplay with head tracking in an OpenXR headset "
                    "(menus on a screen 2 m in front of the player)")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_DOUBLE(display_vr_screen_width_m, 2.6, "Display",
                      "VR screen width in metres (2 m in front of the player)")
    .range(0.5, 10.0)
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_STRING(display_vr_simulate, "off", "Display",
                      "VR test without a headset: render the two eyes and show them on the "
                      "monitor side by side (sbs) or as a red-cyan anaglyph")
    .allowed({"off", "sbs", "anaglyph"})
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_DOUBLE(display_vr_simulate_ipd_mm, 64.0, "Display",
                      "VR simulation: distance between the eyes in millimetres (a headset "
                      "reports its own)")
    .range(40.0, 120.0)
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(display_vr_simulate_motion, false, "Display",
                    "VR simulation: the head turns slowly left and right (+-20 degrees) and "
                    "the right hand aims in a slow circle, to check tracking on the monitor")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(display_vr_simulate_hand, false, "Display",
                    "VR simulation: only the right hand aims in a slow circle (the head stays "
                    "still)")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_DOUBLE(display_vr_hud_distance_m, 1.5, "Display",
                      "VR: distance of the HUD plane in front of the player, metres")
    .range(0.5, 5.0);
REXCVAR_DEFINE_DOUBLE(display_vr_hud_width_m, 1.6, "Display", "VR: HUD plane width, metres")
    .range(0.3, 5.0);
REXCVAR_DEFINE_BOOL(display_vr_hand_aim, true, "Display",
                    "VR: aim with the right controller (the camera follows the hand; the head "
                    "looks around freely)");
REXCVAR_DEFINE_STRING(display_vr_turn, "snap", "Display",
                      "VR: turning with the right stick, snap or smooth")
    .allowed({"snap", "smooth"});
REXCVAR_DEFINE_DOUBLE(display_vr_snap_turn_degrees, 30.0, "Display", "VR: snap turn angle")
    .range(10.0, 90.0);
REXCVAR_DEFINE_DOUBLE(display_vr_smooth_turn_degrees_per_second, 120.0, "Display",
                      "VR: smooth turn speed")
    .range(30.0, 360.0);
REXCVAR_DEFINE_DOUBLE(display_vr_comfort_vignette, 0.6, "Display",
                      "VR: darkens the edges of the view while turning smoothly or moving "
                      "(0 = off, 1 = strongest)")
    .range(0.0, 1.0);
REXCVAR_DEFINE_BOOL(display_vr_standing, false, "Display",
                    "VR: standing play (the floor is the reference; off = seated, the head "
                    "position at the start or after recentring is the reference)")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

namespace rex::ui::d3d12::openxr_screen {
namespace {

using Microsoft::WRL::ComPtr;

struct Api {
  PFN_xrGetInstanceProcAddr GetInstanceProcAddr = nullptr;
  PFN_xrEnumerateInstanceExtensionProperties EnumerateInstanceExtensionProperties = nullptr;
  PFN_xrCreateInstance CreateInstance = nullptr;
  PFN_xrDestroyInstance DestroyInstance = nullptr;
  PFN_xrGetSystem GetSystem = nullptr;
  PFN_xrGetD3D12GraphicsRequirementsKHR GetD3D12GraphicsRequirementsKHR = nullptr;
  PFN_xrCreateSession CreateSession = nullptr;
  PFN_xrDestroySession DestroySession = nullptr;
  PFN_xrBeginSession BeginSession = nullptr;
  PFN_xrEndSession EndSession = nullptr;
  PFN_xrCreateReferenceSpace CreateReferenceSpace = nullptr;
  PFN_xrDestroySpace DestroySpace = nullptr;
  PFN_xrEnumerateSwapchainFormats EnumerateSwapchainFormats = nullptr;
  PFN_xrCreateSwapchain CreateSwapchain = nullptr;
  PFN_xrDestroySwapchain DestroySwapchain = nullptr;
  PFN_xrEnumerateSwapchainImages EnumerateSwapchainImages = nullptr;
  PFN_xrAcquireSwapchainImage AcquireSwapchainImage = nullptr;
  PFN_xrWaitSwapchainImage WaitSwapchainImage = nullptr;
  PFN_xrReleaseSwapchainImage ReleaseSwapchainImage = nullptr;
  PFN_xrPollEvent PollEvent = nullptr;
  PFN_xrWaitFrame WaitFrame = nullptr;
  PFN_xrBeginFrame BeginFrame = nullptr;
  PFN_xrEndFrame EndFrame = nullptr;
  PFN_xrLocateViews LocateViews = nullptr;
  PFN_xrLocateSpace LocateSpace = nullptr;
  PFN_xrStringToPath StringToPath = nullptr;
  PFN_xrCreateActionSet CreateActionSet = nullptr;
  PFN_xrCreateAction CreateAction = nullptr;
  PFN_xrSuggestInteractionProfileBindings SuggestInteractionProfileBindings = nullptr;
  PFN_xrAttachSessionActionSets AttachSessionActionSets = nullptr;
  PFN_xrSyncActions SyncActions = nullptr;
  PFN_xrGetActionStateFloat GetActionStateFloat = nullptr;
  PFN_xrGetActionStateBoolean GetActionStateBoolean = nullptr;
  PFN_xrGetActionStateVector2f GetActionStateVector2f = nullptr;
  PFN_xrCreateActionSpace CreateActionSpace = nullptr;
};

enum class State { kOff, kFailed, kIdle, kRunning };

// An image the command processor writes (eye or HUD) and the presenter reads.
struct Slot {
  ComPtr<ID3D12Resource> texture;
  uint32_t tag = 0;
  ID3D12Fence* fence = nullptr;
  uint64_t fence_value = 0;
  uint64_t serial = 0;  // publication order; 0 = never written
  std::chrono::steady_clock::time_point time{};
};
constexpr uint32_t kSlots = 3;

struct Swapchain {
  XrSwapchain handle = XR_NULL_HANDLE;
  uint32_t width = 0, height = 0;
  std::vector<XrSwapchainImageD3D12KHR> images;
  bool acquired = false;
};

struct Context {
  State state = State::kOff;
  HMODULE loader = nullptr;
  Api api;
  XrInstance instance = XR_NULL_HANDLE;
  XrSystemId system = XR_NULL_SYSTEM_ID;
  XrSession session = XR_NULL_HANDLE;
  XrSpace space = XR_NULL_HANDLE;
  int64_t format = 0;       // the flat screen (B8G8R8A8 family, like the back buffer)
  int64_t eye_format = 0;   // eyes and HUD (R8G8B8A8_UNORM_SRGB)
  Swapchain screen;
  Swapchain eyes[2];
  Swapchain hud;
  XrSessionState session_state = XR_SESSION_STATE_UNKNOWN;
  bool session_running = false;
  bool frame_begun = false;
  XrTime display_time = 0;
  bool should_render = false;
  uint64_t frames = 0;
  // Actions (hands).
  XrActionSet action_set = XR_NULL_HANDLE;
  XrAction aim = XR_NULL_HANDLE, trigger = XR_NULL_HANDLE, grip = XR_NULL_HANDLE,
           stick = XR_NULL_HANDLE, stick_click = XR_NULL_HANDLE, primary = XR_NULL_HANDLE,
           secondary = XR_NULL_HANDLE, menu = XR_NULL_HANDLE;
  XrPath hand_paths[2] = {XR_NULL_PATH, XR_NULL_PATH};
  XrSpace hand_spaces[2] = {XR_NULL_HANDLE, XR_NULL_HANDLE};
  bool actions = false;
  // This frame's layers (stereo: which eye slots were copied).
  bool stereo_this_frame = false;
  uint32_t eye_tags[2] = {};
  bool hud_this_frame = false;
};
Context g;

// Shared with the game's threads.
std::mutex g_mutex;
std::atomic<bool> g_stereo_active{false};
HeadView g_head;
std::array<HeadView, 64> g_head_history;
Controllers g_controllers;
uint32_t g_next_head_id = 1;
Slot g_eye_slots[2][kSlots];
Slot g_hud_slots[kSlots];
uint32_t g_eye_pending[2] = {kSlots, kSlots};  // slot handed out, not yet published
uint32_t g_hud_pending = kSlots;
uint64_t g_serial = 0;
bool g_simulate = false;
uint32_t g_simulate_mode = 0;  // 4 anaglyph, 8 side by side
bool g_simulate_checked = false;
std::chrono::steady_clock::time_point g_start = std::chrono::steady_clock::now();
// Look input and comfort (UpdateLookInput).
std::chrono::steady_clock::time_point g_last_input{};
bool g_aim_known = false;
double g_aim_yaw = 0.0, g_aim_pitch = 0.0;
bool g_snap_held = false;
double g_look_yaw = 0.0, g_look_pitch = 0.0;
float g_vignette = 0.0f;
// The laser pointer on the flat screen.
float g_screen_aspect = 0.5625f;
bool g_pointer_valid = false, g_pointer_pressed = false;
float g_pointer_u = 0.0f, g_pointer_v = 0.0f;
std::atomic<bool> g_overlay_visible{false};
bool g_screen_this_frame = false;

void Log(const char* text) {
  std::fprintf(stderr, "REX_OPENXR %s\n", text);
  std::fflush(stderr);
}
void LogResult(const char* what, XrResult result) {
  std::fprintf(stderr, "REX_OPENXR %s result=%d\n", what, int(result));
  std::fflush(stderr);
}

bool Simulating() {
  if (!g_simulate_checked) {
    g_simulate_checked = true;
    const std::string mode = REXCVAR_GET(display_vr_simulate);
    g_simulate = mode == "sbs" || mode == "anaglyph";
    g_simulate_mode = mode == "anaglyph" ? 4u : 8u;
    if (g_simulate) {
      std::fprintf(stderr, "REX_OPENXR simulate=%s ipd_mm=%.1f motion=%d\n", mode.c_str(),
                   REXCVAR_GET(display_vr_simulate_ipd_mm),
                   REXCVAR_GET(display_vr_simulate_motion) ? 1 : 0);
      std::fflush(stderr);
    }
  }
  return g_simulate;
}

void QuatFromYawPitch(float yaw, float pitch, float out[4]) {
  // Yaw about +y, then pitch about the yawed +x (OpenXR axes).
  const float cy = std::cos(yaw * 0.5f), sy = std::sin(yaw * 0.5f);
  const float cp = std::cos(pitch * 0.5f), sp = std::sin(pitch * 0.5f);
  // q = q_yaw * q_pitch
  out[0] = cy * sp;
  out[1] = sy * cp;
  out[2] = -sy * sp;
  out[3] = cy * cp;
}

void Rotate(const float q[4], const float v[3], float out[3]) {
  // v' = q v q*
  const float x = q[0], y = q[1], z = q[2], w = q[3];
  const float tx = 2.0f * (y * v[2] - z * v[1]);
  const float ty = 2.0f * (z * v[0] - x * v[2]);
  const float tz = 2.0f * (x * v[1] - y * v[0]);
  out[0] = v[0] + w * tx + (y * tz - z * ty);
  out[1] = v[1] + w * ty + (z * tx - x * tz);
  out[2] = v[2] + w * tz + (x * ty - y * tx);
}

// Hand aim, turning and the comfort vignette from the latest controllers
// (presenter thread, once per tracking update).
void UpdateLookInput(const Controllers& controllers, HeadView& head) {
  const auto now = std::chrono::steady_clock::now();
  const double dt =
      g_last_input.time_since_epoch().count()
          ? std::min(0.1, std::chrono::duration<double>(now - g_last_input).count())
          : 0.0;
  g_last_input = now;
  const Hand& right = controllers.hand[1];
  const Hand& left = controllers.hand[0];
  double look_yaw = 0.0, look_pitch = 0.0;
  float turning = 0.0f;
  if (REXCVAR_GET(display_vr_hand_aim) && right.valid) {
    // Forward = -z of the aim pose.
    const float minus_z[3] = {0.0f, 0.0f, -1.0f};
    float forward[3];
    Rotate(right.orientation, minus_z, forward);
    const double yaw = std::atan2(double(forward[0]), -double(forward[2]));  // right +
    const double pitch = std::asin(std::clamp(double(forward[1]), -1.0, 1.0));  // up +
    if (g_aim_known) {
      double d_yaw = yaw - g_aim_yaw;
      if (d_yaw > 3.14159265358979) d_yaw -= 2.0 * 3.14159265358979;
      if (d_yaw < -3.14159265358979) d_yaw += 2.0 * 3.14159265358979;
      look_yaw += d_yaw;
      look_pitch += pitch - g_aim_pitch;
    }
    g_aim_known = true;
    g_aim_yaw = yaw;
    g_aim_pitch = pitch;
    // The aim as a rotation (yaw about +y, then pitch), OpenXR axes; a yaw
    // to the right is a negative rotation about +y.
    QuatFromYawPitch(float(-yaw), float(pitch), head.aim);
    head.aim_valid = true;
  } else {
    g_aim_known = false;
  }
  // Turning with the right stick (the body turns: look input).
  const float stick_x = right.valid ? right.stick[0] : 0.0f;
  const bool power_select = left.valid && left.grip > 0.5f;
  if (!power_select) {
    if (REXCVAR_GET(display_vr_turn) == "smooth") {
      if (std::fabs(stick_x) > 0.2f) {
        look_yaw += double(stick_x) * REXCVAR_GET(display_vr_smooth_turn_degrees_per_second) *
                    3.14159265358979 / 180.0 * dt;
        turning = std::fabs(stick_x);
      }
    } else {
      if (std::fabs(stick_x) > 0.7f && !g_snap_held) {
        look_yaw += (stick_x > 0.0f ? 1.0 : -1.0) * REXCVAR_GET(display_vr_snap_turn_degrees) *
                    3.14159265358979 / 180.0;
        g_snap_held = true;
      } else if (std::fabs(stick_x) < 0.3f) {
        g_snap_held = false;
      }
    }
  }
  const float moving = left.valid ? std::min(1.0f, std::hypot(left.stick[0], left.stick[1])) : 0.0f;
  std::lock_guard lock(g_mutex);
  g_look_yaw += look_yaw;
  g_look_pitch += look_pitch;
  g_vignette = float(REXCVAR_GET(display_vr_comfort_vignette)) * std::max(turning, moving * 0.5f);
  // The laser pointer on the screen quad (z = -2 m, centred, width from the
  // setting): the right hand's ray.
  g_pointer_valid = false;
  if (right.valid && g_screen_aspect > 0.0f) {
    const float minus_z[3] = {0.0f, 0.0f, -1.0f};
    float direction[3];
    Rotate(right.orientation, minus_z, direction);
    if (direction[2] < -1e-3f) {
      const float t = (-2.0f - right.position[2]) / direction[2];
      const float x = right.position[0] + direction[0] * t;
      const float y = right.position[1] + direction[1] * t;
      const float width = float(REXCVAR_GET(display_vr_screen_width_m));
      const float height = width * g_screen_aspect;
      g_pointer_u = x / width + 0.5f;
      g_pointer_v = 0.5f - y / height;
      g_pointer_pressed = right.trigger > 0.5f;
      g_pointer_valid = g_pointer_u >= 0.0f && g_pointer_u <= 1.0f && g_pointer_v >= 0.0f &&
                        g_pointer_v <= 1.0f;
    }
  }
}

void PublishHead(HeadView view) {
  std::lock_guard lock(g_mutex);
  view.id = g_next_head_id;
  g_next_head_id = (g_next_head_id % kFrameTagIdMask) + 1;
  g_head = view;
  g_head_history[view.id % g_head_history.size()] = view;
  g_stereo_active.store(true, std::memory_order_release);
}

// The simulation's head and hand (no headset).
void SimulateTracking() {
  const double t =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - g_start).count();
  const bool motion = REXCVAR_GET(display_vr_simulate_motion);
  HeadView view;
  const float yaw = motion ? float(0.349 * std::sin(t * 2.0 * 3.14159265 / 8.0)) : 0.0f;
  QuatFromYawPitch(yaw, 0.0f, view.orientation);
  const float half = float(REXCVAR_GET(display_vr_simulate_ipd_mm)) * 0.0005f;
  view.eye_offset[0][0] = -half;
  view.eye_offset[1][0] = half;
  // 0: keep the game's own field of view on the monitor.
  view.tan_half_fov_x = 0.0f;
  view.tan_half_fov_y = 0.0f;
  Controllers controllers;
  if (motion || REXCVAR_GET(display_vr_simulate_hand)) {
    Hand& right = controllers.hand[1];
    right.valid = true;
    const float hand_yaw = float(0.26 * std::cos(t * 2.0 * 3.14159265 / 6.0));
    const float hand_pitch = float(0.17 * std::sin(t * 2.0 * 3.14159265 / 6.0));
    QuatFromYawPitch(hand_yaw, hand_pitch, right.orientation);
    right.position[0] = 0.2f;
    right.position[1] = -0.3f;
    right.position[2] = -0.4f;
  }
  UpdateLookInput(controllers, view);
  PublishHead(view);
  std::lock_guard lock(g_mutex);
  g_controllers = controllers;
}

template <typename T>
bool Load(T& out, const char* name) {
  if (!g.api.GetInstanceProcAddr) return false;
  PFN_xrVoidFunction function = nullptr;
  if (XR_FAILED(g.api.GetInstanceProcAddr(g.instance, name, &function)) || !function) {
    std::fprintf(stderr, "REX_OPENXR missing_function=%s\n", name);
    return false;
  }
  out = reinterpret_cast<T>(function);
  return true;
}

void Fail(const char* why) {
  Log(why);
  Shutdown();
  g.state = State::kFailed;
}

bool LoadLoader() {
  // Only from the executable's folder (like the upscaler runtimes).
  wchar_t path[MAX_PATH]{};
  const DWORD length = GetModuleFileNameW(nullptr, path, MAX_PATH);
  if (!length || length >= MAX_PATH) return false;
  std::wstring folder(path, length);
  folder.resize(folder.find_last_of(L"\\/") + 1);
  g.loader = LoadLibraryExW((folder + L"openxr_loader.dll").c_str(), nullptr,
                            LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
  if (!g.loader) return false;
  g.api.GetInstanceProcAddr = reinterpret_cast<PFN_xrGetInstanceProcAddr>(
      GetProcAddress(g.loader, "xrGetInstanceProcAddr"));
  return g.api.GetInstanceProcAddr != nullptr;
}

XrPath Path(const char* text) {
  XrPath path = XR_NULL_PATH;
  g.api.StringToPath(g.instance, text, &path);
  return path;
}

// Touch controllers (Quest 3 Touch Plus report the Oculus Touch profile).
bool CreateActions() {
  XrActionSetCreateInfo set_info{XR_TYPE_ACTION_SET_CREATE_INFO};
  std::strcpy(set_info.actionSetName, "gameplay");
  std::strcpy(set_info.localizedActionSetName, "Gameplay");
  if (XR_FAILED(g.api.CreateActionSet(g.instance, &set_info, &g.action_set))) return false;
  g.hand_paths[0] = Path("/user/hand/left");
  g.hand_paths[1] = Path("/user/hand/right");
  auto action = [](XrAction& out, const char* name, XrActionType type) {
    XrActionCreateInfo info{XR_TYPE_ACTION_CREATE_INFO};
    std::strcpy(info.actionName, name);
    std::strcpy(info.localizedActionName, name);
    info.actionType = type;
    info.countSubactionPaths = 2;
    info.subactionPaths = g.hand_paths;
    return XR_SUCCEEDED(g.api.CreateAction(g.action_set, &info, &out));
  };
  if (!action(g.aim, "aim", XR_ACTION_TYPE_POSE_INPUT) ||
      !action(g.trigger, "trigger", XR_ACTION_TYPE_FLOAT_INPUT) ||
      !action(g.grip, "grip", XR_ACTION_TYPE_FLOAT_INPUT) ||
      !action(g.stick, "stick", XR_ACTION_TYPE_VECTOR2F_INPUT) ||
      !action(g.stick_click, "stick_click", XR_ACTION_TYPE_BOOLEAN_INPUT) ||
      !action(g.primary, "primary", XR_ACTION_TYPE_BOOLEAN_INPUT) ||
      !action(g.secondary, "secondary", XR_ACTION_TYPE_BOOLEAN_INPUT) ||
      !action(g.menu, "menu", XR_ACTION_TYPE_BOOLEAN_INPUT)) {
    return false;
  }
  const std::pair<XrAction, const char*> bindings[] = {
      {g.aim, "/user/hand/left/input/aim/pose"},
      {g.aim, "/user/hand/right/input/aim/pose"},
      {g.trigger, "/user/hand/left/input/trigger/value"},
      {g.trigger, "/user/hand/right/input/trigger/value"},
      {g.grip, "/user/hand/left/input/squeeze/value"},
      {g.grip, "/user/hand/right/input/squeeze/value"},
      {g.stick, "/user/hand/left/input/thumbstick"},
      {g.stick, "/user/hand/right/input/thumbstick"},
      {g.stick_click, "/user/hand/left/input/thumbstick/click"},
      {g.stick_click, "/user/hand/right/input/thumbstick/click"},
      {g.primary, "/user/hand/left/input/x/click"},
      {g.primary, "/user/hand/right/input/a/click"},
      {g.secondary, "/user/hand/left/input/y/click"},
      {g.secondary, "/user/hand/right/input/b/click"},
      {g.menu, "/user/hand/left/input/menu/click"},
  };
  std::vector<XrActionSuggestedBinding> suggested;
  for (const auto& [bound, path] : bindings) suggested.push_back({bound, Path(path)});
  XrInteractionProfileSuggestedBinding profile{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
  profile.interactionProfile = Path("/interaction_profiles/oculus/touch_controller");
  profile.countSuggestedBindings = uint32_t(suggested.size());
  profile.suggestedBindings = suggested.data();
  if (XR_FAILED(g.api.SuggestInteractionProfileBindings(g.instance, &profile))) return false;
  for (uint32_t hand = 0; hand < 2; ++hand) {
    XrActionSpaceCreateInfo space{XR_TYPE_ACTION_SPACE_CREATE_INFO};
    space.action = g.aim;
    space.subactionPath = g.hand_paths[hand];
    space.poseInActionSpace.orientation.w = 1.0f;
    if (XR_FAILED(g.api.CreateActionSpace(g.session, &space, &g.hand_spaces[hand]))) return false;
  }
  XrSessionActionSetsAttachInfo attach{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
  attach.countActionSets = 1;
  attach.actionSets = &g.action_set;
  return XR_SUCCEEDED(g.api.AttachSessionActionSets(g.session, &attach));
}

bool Initialize(ID3D12Device* device, ID3D12CommandQueue* queue) {
  if (!LoadLoader()) {
    Fail("no openxr_loader.dll next to the executable; VR off");
    return false;
  }
  if (!Load(g.api.EnumerateInstanceExtensionProperties, "xrEnumerateInstanceExtensionProperties") ||
      !Load(g.api.CreateInstance, "xrCreateInstance")) {
    Fail("loader without the global functions; VR off");
    return false;
  }
  uint32_t count = 0;
  g.api.EnumerateInstanceExtensionProperties(nullptr, 0, &count, nullptr);
  std::vector<XrExtensionProperties> extensions(count, {XR_TYPE_EXTENSION_PROPERTIES});
  g.api.EnumerateInstanceExtensionProperties(nullptr, count, &count, extensions.data());
  bool d3d12 = false;
  for (const auto& extension : extensions) {
    d3d12 |= !std::strcmp(extension.extensionName, XR_KHR_D3D12_ENABLE_EXTENSION_NAME);
  }
  if (!d3d12) {
    Fail("the OpenXR runtime has no D3D12 support (XR_KHR_D3D12_enable); VR off");
    return false;
  }
  const char* enabled[] = {XR_KHR_D3D12_ENABLE_EXTENSION_NAME};
  XrInstanceCreateInfo create{XR_TYPE_INSTANCE_CREATE_INFO};
  std::strncpy(create.applicationInfo.applicationName, "Estacado", XR_MAX_APPLICATION_NAME_SIZE - 1);
  create.applicationInfo.applicationVersion = 1;
  std::strncpy(create.applicationInfo.engineName, "ReXGlue", XR_MAX_ENGINE_NAME_SIZE - 1);
  create.applicationInfo.apiVersion = XR_API_VERSION_1_0;
  create.enabledExtensionCount = 1;
  create.enabledExtensionNames = enabled;
  XrResult result = g.api.CreateInstance(&create, &g.instance);
  if (XR_FAILED(result)) {
    LogResult("create_instance", result);
    Fail("no OpenXR runtime; VR off");
    return false;
  }
  if (!Load(g.api.DestroyInstance, "xrDestroyInstance") || !Load(g.api.GetSystem, "xrGetSystem") ||
      !Load(g.api.GetD3D12GraphicsRequirementsKHR, "xrGetD3D12GraphicsRequirementsKHR") ||
      !Load(g.api.CreateSession, "xrCreateSession") ||
      !Load(g.api.DestroySession, "xrDestroySession") ||
      !Load(g.api.BeginSession, "xrBeginSession") || !Load(g.api.EndSession, "xrEndSession") ||
      !Load(g.api.CreateReferenceSpace, "xrCreateReferenceSpace") ||
      !Load(g.api.DestroySpace, "xrDestroySpace") ||
      !Load(g.api.EnumerateSwapchainFormats, "xrEnumerateSwapchainFormats") ||
      !Load(g.api.CreateSwapchain, "xrCreateSwapchain") ||
      !Load(g.api.DestroySwapchain, "xrDestroySwapchain") ||
      !Load(g.api.EnumerateSwapchainImages, "xrEnumerateSwapchainImages") ||
      !Load(g.api.AcquireSwapchainImage, "xrAcquireSwapchainImage") ||
      !Load(g.api.WaitSwapchainImage, "xrWaitSwapchainImage") ||
      !Load(g.api.ReleaseSwapchainImage, "xrReleaseSwapchainImage") ||
      !Load(g.api.PollEvent, "xrPollEvent") || !Load(g.api.WaitFrame, "xrWaitFrame") ||
      !Load(g.api.BeginFrame, "xrBeginFrame") || !Load(g.api.EndFrame, "xrEndFrame") ||
      !Load(g.api.LocateViews, "xrLocateViews") || !Load(g.api.LocateSpace, "xrLocateSpace") ||
      !Load(g.api.StringToPath, "xrStringToPath") ||
      !Load(g.api.CreateActionSet, "xrCreateActionSet") ||
      !Load(g.api.CreateAction, "xrCreateAction") ||
      !Load(g.api.SuggestInteractionProfileBindings, "xrSuggestInteractionProfileBindings") ||
      !Load(g.api.AttachSessionActionSets, "xrAttachSessionActionSets") ||
      !Load(g.api.SyncActions, "xrSyncActions") ||
      !Load(g.api.GetActionStateFloat, "xrGetActionStateFloat") ||
      !Load(g.api.GetActionStateBoolean, "xrGetActionStateBoolean") ||
      !Load(g.api.GetActionStateVector2f, "xrGetActionStateVector2f") ||
      !Load(g.api.CreateActionSpace, "xrCreateActionSpace")) {
    Fail("runtime without a needed function; VR off");
    return false;
  }
  XrSystemGetInfo system_info{XR_TYPE_SYSTEM_GET_INFO};
  system_info.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
  result = g.api.GetSystem(g.instance, &system_info, &g.system);
  if (XR_FAILED(result)) {
    LogResult("get_system", result);
    Fail("no headset connected (start Quest Link first); VR off");
    return false;
  }
  XrGraphicsRequirementsD3D12KHR requirements{XR_TYPE_GRAPHICS_REQUIREMENTS_D3D12_KHR};
  g.api.GetD3D12GraphicsRequirementsKHR(g.instance, g.system, &requirements);
  const LUID luid = device->GetAdapterLuid();
  if (std::memcmp(&luid, &requirements.adapterLuid, sizeof(LUID))) {
    Fail("the headset is on another graphics adapter than the game; VR off");
    return false;
  }
  XrGraphicsBindingD3D12KHR binding{XR_TYPE_GRAPHICS_BINDING_D3D12_KHR};
  binding.device = device;
  binding.queue = queue;
  XrSessionCreateInfo session_info{XR_TYPE_SESSION_CREATE_INFO};
  session_info.next = &binding;
  session_info.systemId = g.system;
  result = g.api.CreateSession(g.instance, &session_info, &g.session);
  if (XR_FAILED(result)) {
    LogResult("create_session", result);
    Fail("session refused; VR off");
    return false;
  }
  XrReferenceSpaceCreateInfo space_info{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
  space_info.referenceSpaceType = REXCVAR_GET(display_vr_standing)
                                      ? XR_REFERENCE_SPACE_TYPE_STAGE
                                      : XR_REFERENCE_SPACE_TYPE_LOCAL;
  space_info.poseInReferenceSpace.orientation.w = 1.0f;
  result = g.api.CreateReferenceSpace(g.session, &space_info, &g.space);
  if (XR_FAILED(result) && REXCVAR_GET(display_vr_standing)) {
    Log("no standing (stage) space; seated");
    space_info.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    result = g.api.CreateReferenceSpace(g.session, &space_info, &g.space);
  }
  if (XR_FAILED(result)) {
    LogResult("create_space", result);
    Fail("no local space; VR off");
    return false;
  }
  // The back buffer is B8G8R8A8_UNORM holding sRGB-encoded values: an sRGB
  // view of the same bytes shows them as on the monitor. The eye and HUD
  // images are R8G8B8A8 holding sRGB-encoded values likewise.
  g.api.EnumerateSwapchainFormats(g.session, 0, &count, nullptr);
  std::vector<int64_t> formats(count);
  g.api.EnumerateSwapchainFormats(g.session, count, &count, formats.data());
  for (const int64_t wanted : {int64_t(DXGI_FORMAT_B8G8R8A8_UNORM_SRGB), int64_t(DXGI_FORMAT_B8G8R8A8_UNORM)}) {
    for (const int64_t format : formats) {
      if (format == wanted && !g.format) g.format = format;
    }
  }
  for (const int64_t format : formats) {
    if (format == int64_t(DXGI_FORMAT_R8G8B8A8_UNORM_SRGB)) g.eye_format = format;
  }
  if (!g.format) {
    Fail("the runtime offers no B8G8R8A8 swapchain format; VR off");
    return false;
  }
  if (!g.eye_format) Log("no R8G8B8A8 sRGB swapchain format: stereo off, screen only");
  g.actions = CreateActions();
  if (!g.actions) Log("controllers unavailable (no action bindings); head tracking only");
  g.state = State::kIdle;
  std::fprintf(stderr, "REX_OPENXR ready format=%lld eye_format=%lld actions=%d\n",
               static_cast<long long>(g.format), static_cast<long long>(g.eye_format),
               g.actions ? 1 : 0);
  std::fflush(stderr);
  return true;
}

bool EnsureSwapchain(Swapchain& chain, int64_t format, uint32_t width, uint32_t height) {
  if (chain.handle && chain.width == width && chain.height == height) return true;
  if (chain.handle) {
    g.api.DestroySwapchain(chain.handle);
    chain.handle = XR_NULL_HANDLE;
  }
  XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
  info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
  info.format = format;
  info.sampleCount = 1;
  info.width = width;
  info.height = height;
  info.faceCount = 1;
  info.arraySize = 1;
  info.mipCount = 1;
  const XrResult result = g.api.CreateSwapchain(g.session, &info, &chain.handle);
  if (XR_FAILED(result)) {
    LogResult("create_swapchain", result);
    return false;
  }
  uint32_t count = 0;
  g.api.EnumerateSwapchainImages(chain.handle, 0, &count, nullptr);
  chain.images.assign(count, {XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR});
  g.api.EnumerateSwapchainImages(chain.handle, count, &count,
                                 reinterpret_cast<XrSwapchainImageBaseHeader*>(chain.images.data()));
  chain.width = width;
  chain.height = height;
  std::fprintf(stderr, "REX_OPENXR swapchain %ux%u format=%lld images=%u\n", width, height,
               static_cast<long long>(format), count);
  std::fflush(stderr);
  return true;
}

ID3D12Resource* Acquire(Swapchain& chain) {
  uint32_t index = 0;
  XrSwapchainImageAcquireInfo acquire{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
  if (XR_FAILED(g.api.AcquireSwapchainImage(chain.handle, &acquire, &index))) return nullptr;
  chain.acquired = true;
  XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
  wait.timeout = XR_INFINITE_DURATION;
  if (XR_FAILED(g.api.WaitSwapchainImage(chain.handle, &wait)) || index >= chain.images.size()) {
    return nullptr;
  }
  return chain.images[index].texture;
}

void Release(Swapchain& chain) {
  if (!chain.acquired) return;
  XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
  g.api.ReleaseSwapchainImage(chain.handle, &release);
  chain.acquired = false;
}

void PollEvents() {
  XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER};
  while (g.api.PollEvent(g.instance, &event) == XR_SUCCESS) {
    if (event.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
      const auto& changed = reinterpret_cast<const XrEventDataSessionStateChanged&>(event);
      g.session_state = changed.state;
      std::fprintf(stderr, "REX_OPENXR session_state=%d\n", int(changed.state));
      std::fflush(stderr);
      if (changed.state == XR_SESSION_STATE_READY && !g.session_running) {
        XrSessionBeginInfo begin{XR_TYPE_SESSION_BEGIN_INFO};
        begin.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
        g.session_running = XR_SUCCEEDED(g.api.BeginSession(g.session, &begin));
      } else if (changed.state == XR_SESSION_STATE_STOPPING && g.session_running) {
        g.api.EndSession(g.session);
        g.session_running = false;
        g_stereo_active.store(false, std::memory_order_release);
      } else if (changed.state == XR_SESSION_STATE_EXITING ||
                 changed.state == XR_SESSION_STATE_LOSS_PENDING) {
        // The headset or Link went away: the game continues flat.
        g.session_running = false;
        Fail("session ended by the runtime; VR off until the next start");
        return;
      }
    }
    event = {XR_TYPE_EVENT_DATA_BUFFER};
  }
}

HeadView g_located_head;
bool g_located = false;

// Head (from the two eye views) and hands at the frame's display time.
void LocateTracking() {
  g_located = false;
  XrViewLocateInfo locate{XR_TYPE_VIEW_LOCATE_INFO};
  locate.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
  locate.displayTime = g.display_time;
  locate.space = g.space;
  XrViewState view_state{XR_TYPE_VIEW_STATE};
  XrView views[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
  uint32_t count = 0;
  if (XR_SUCCEEDED(g.api.LocateViews(g.session, &locate, &view_state, 2, &count, views)) &&
      count == 2 && (view_state.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT) &&
      (view_state.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT)) {
    HeadView head;
    const XrQuaternionf& q = views[0].pose.orientation;
    head.orientation[0] = q.x;
    head.orientation[1] = q.y;
    head.orientation[2] = q.z;
    head.orientation[3] = q.w;
    for (int axis = 0; axis < 3; ++axis) {
      const float a = (&views[0].pose.position.x)[axis];
      const float b = (&views[1].pose.position.x)[axis];
      head.position[axis] = 0.5f * (a + b);
    }
    const float inverse[4] = {-q.x, -q.y, -q.z, q.w};
    for (int eye = 0; eye < 2; ++eye) {
      float world[3];
      for (int axis = 0; axis < 3; ++axis) {
        world[axis] = (&views[eye].pose.position.x)[axis] - head.position[axis];
      }
      Rotate(inverse, world, head.eye_offset[eye]);
    }
    // One symmetric field of view covering both eyes' (the compositor
    // reprojects to each eye's real one).
    float tan_x = 0.0f, tan_y = 0.0f;
    for (const XrView& view : views) {
      tan_x = std::max({tan_x, std::tan(std::fabs(view.fov.angleLeft)),
                        std::tan(std::fabs(view.fov.angleRight))});
      tan_y = std::max({tan_y, std::tan(std::fabs(view.fov.angleUp)),
                        std::tan(std::fabs(view.fov.angleDown))});
    }
    head.tan_half_fov_x = tan_x;
    head.tan_half_fov_y = tan_y;
    g_located_head = head;
    g_located = true;
  }
  if (!g.actions) {
    if (g_located) {
      Controllers none;
      UpdateLookInput(none, g_located_head);
      PublishHead(g_located_head);
    }
    return;
  }
  XrActiveActionSet active{g.action_set, XR_NULL_PATH};
  XrActionsSyncInfo sync{XR_TYPE_ACTIONS_SYNC_INFO};
  sync.countActiveActionSets = 1;
  sync.activeActionSets = &active;
  if (XR_FAILED(g.api.SyncActions(g.session, &sync))) return;
  Controllers controllers;
  for (uint32_t index = 0; index < 2; ++index) {
    Hand& hand = controllers.hand[index];
    XrSpaceLocation location{XR_TYPE_SPACE_LOCATION};
    if (XR_SUCCEEDED(g.api.LocateSpace(g.hand_spaces[index], g.space, g.display_time, &location)) &&
        (location.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)) {
      hand.valid = true;
      hand.orientation[0] = location.pose.orientation.x;
      hand.orientation[1] = location.pose.orientation.y;
      hand.orientation[2] = location.pose.orientation.z;
      hand.orientation[3] = location.pose.orientation.w;
      hand.position[0] = location.pose.position.x;
      hand.position[1] = location.pose.position.y;
      hand.position[2] = location.pose.position.z;
    }
    XrActionStateGetInfo get{XR_TYPE_ACTION_STATE_GET_INFO};
    get.subactionPath = g.hand_paths[index];
    XrActionStateFloat value{XR_TYPE_ACTION_STATE_FLOAT};
    get.action = g.trigger;
    if (XR_SUCCEEDED(g.api.GetActionStateFloat(g.session, &get, &value))) hand.trigger = value.currentState;
    get.action = g.grip;
    value = {XR_TYPE_ACTION_STATE_FLOAT};
    if (XR_SUCCEEDED(g.api.GetActionStateFloat(g.session, &get, &value))) hand.grip = value.currentState;
    XrActionStateVector2f stick{XR_TYPE_ACTION_STATE_VECTOR2F};
    get.action = g.stick;
    if (XR_SUCCEEDED(g.api.GetActionStateVector2f(g.session, &get, &stick))) {
      hand.stick[0] = stick.currentState.x;
      hand.stick[1] = stick.currentState.y;
    }
    const XrAction buttons[4] = {g.primary, g.secondary, g.stick_click, g.menu};
    for (uint32_t bit = 0; bit < 4; ++bit) {
      XrActionStateBoolean pressed{XR_TYPE_ACTION_STATE_BOOLEAN};
      get.action = buttons[bit];
      if (XR_SUCCEEDED(g.api.GetActionStateBoolean(g.session, &get, &pressed)) &&
          pressed.isActive && pressed.currentState) {
        hand.buttons |= 1u << bit;
      }
    }
  }
  if (g_located) {
    UpdateLookInput(controllers, g_located_head);
    PublishHead(g_located_head);
  }
  std::lock_guard lock(g_mutex);
  g_controllers = controllers;
}

// The newest published slot whose writing list has completed on the GPU.
Slot* Newest(Slot* slots) {
  Slot* best = nullptr;
  for (uint32_t i = 0; i < kSlots; ++i) {
    Slot& slot = slots[i];
    if (!slot.serial || !slot.fence || slot.fence->GetCompletedValue() < slot.fence_value) continue;
    if (!best || slot.serial > best->serial) best = &slot;
  }
  return best;
}

ID3D12Resource* SlotTarget(ID3D12Device* device, Slot* slots, uint32_t& pending,
                           uint32_t width, uint32_t height) {
  // The slot after the newest published one (never the one being read: the
  // presenter reads the newest completed slot).
  uint32_t newest = 0;
  uint64_t newest_serial = 0;
  for (uint32_t i = 0; i < kSlots; ++i) {
    if (slots[i].serial > newest_serial) {
      newest_serial = slots[i].serial;
      newest = i;
    }
  }
  const uint32_t index = newest_serial ? (newest + 1) % kSlots : 0;
  Slot& slot = slots[index];
  if (slot.texture) {
    const D3D12_RESOURCE_DESC desc = slot.texture->GetDesc();
    if (desc.Width != width || desc.Height != height) {
      // Rare (resolution change): the old texture may still be read.
      slot.texture.Reset();
      slot.serial = 0;
    }
  }
  if (!slot.texture) {
    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width;
    desc.Height = height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_TYPELESS;
    desc.SampleDesc.Count = 1;
    // Simultaneous access: written by the command processor's list, read by
    // the presenter's, no explicit transitions (both decay to common).
    desc.Flags =
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS | D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS;
    D3D12_HEAP_PROPERTIES heap = {};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                               D3D12_RESOURCE_STATE_COMMON, nullptr,
                                               IID_PPV_ARGS(&slot.texture)))) {
      return nullptr;
    }
  }
  pending = index;
  return slot.texture.Get();
}

// Copies a slot into an acquired swapchain image (render target state).
void CopyToImage(ID3D12GraphicsCommandList* list, ID3D12Resource* source, ID3D12Resource* image) {
  D3D12_RESOURCE_BARRIER barrier = {};
  barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  barrier.Transition.pResource = image;
  barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
  barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
  list->ResourceBarrier(1, &barrier);
  list->CopyResource(image, source);
  std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
  list->ResourceBarrier(1, &barrier);
}

}  // namespace

bool Wanted() { return REXCVAR_GET(display_vr) || Simulating(); }

void TakeLook(double& yaw, double& pitch) {
  std::lock_guard lock(g_mutex);
  yaw = g_look_yaw;
  pitch = g_look_pitch;
  g_look_yaw = g_look_pitch = 0.0;
}

float VignetteStrength() {
  std::lock_guard lock(g_mutex);
  return g_vignette;
}

bool Pointer(float& u, float& v, bool& pressed) {
  std::lock_guard lock(g_mutex);
  if (!g_pointer_valid || !g_screen_this_frame) return false;
  u = g_pointer_u;
  v = g_pointer_v;
  pressed = g_pointer_pressed;
  return true;
}

void SetOverlayVisible(bool visible) { g_overlay_visible.store(visible, std::memory_order_relaxed); }

bool StereoActive() { return g_stereo_active.load(std::memory_order_acquire); }

bool LatestHeadView(HeadView& out) {
  if (!StereoActive()) return false;
  std::lock_guard lock(g_mutex);
  out = g_head;
  return out.id != 0;
}

bool LatestControllers(Controllers& out) {
  if (!StereoActive()) return false;
  std::lock_guard lock(g_mutex);
  out = g_controllers;
  return true;
}

ID3D12Resource* EyeTarget(ID3D12Device* device, uint32_t tag, uint32_t width, uint32_t height) {
  if (!(tag & kFrameTagValid) || !StereoActive() || !width || !height) return nullptr;
  const uint32_t eye = (tag & kFrameTagRightEye) ? 1 : 0;
  std::lock_guard lock(g_mutex);
  return SlotTarget(device, g_eye_slots[eye], g_eye_pending[eye], width, height);
}

ID3D12Resource* HudTarget(ID3D12Device* device, uint32_t width, uint32_t height) {
  if (!StereoActive() || !width || !height) return nullptr;
  std::lock_guard lock(g_mutex);
  return SlotTarget(device, g_hud_slots, g_hud_pending, width, height);
}

void Published(uint32_t tag, ID3D12Fence* fence, uint64_t fence_value) {
  const uint32_t eye = (tag & kFrameTagRightEye) ? 1 : 0;
  const auto now = std::chrono::steady_clock::now();
  std::lock_guard lock(g_mutex);
  auto publish = [&](Slot* slots, uint32_t& pending) {
    if (pending >= kSlots) return;
    Slot& slot = slots[pending];
    slot.tag = tag;
    slot.fence = fence;
    slot.fence_value = fence_value;
    slot.serial = ++g_serial;
    slot.time = now;
    pending = kSlots;
  };
  publish(g_eye_slots[eye], g_eye_pending[eye]);
  publish(g_hud_slots, g_hud_pending);
}

bool PreviewSources(ID3D12Resource*& left, ID3D12Resource*& right, ID3D12Resource*& hud,
                    uint32_t& mode) {
  if (!Simulating()) return false;
  std::lock_guard lock(g_mutex);
  // The newest published (the preview runs in the command processor's own
  // list after the copies, so completion is not needed).
  auto newest = [](Slot* slots) -> Slot* {
    Slot* best = nullptr;
    for (uint32_t i = 0; i < kSlots; ++i) {
      if (slots[i].serial && (!best || slots[i].serial > best->serial)) best = &slots[i];
    }
    return best;
  };
  Slot* l = newest(g_eye_slots[0]);
  Slot* r = newest(g_eye_slots[1]);
  Slot* h = newest(g_hud_slots);
  if (!l || !r || !h) return false;
  left = l->texture.Get();
  right = r->texture.Get();
  hud = h->texture.Get();
  mode = g_simulate_mode;
  return true;
}

ID3D12Resource* BeginFrame(ID3D12Device* device, ID3D12CommandQueue* queue, uint32_t width,
                           uint32_t height, ID3D12GraphicsCommandList* list) {
  if (Simulating()) {
    SimulateTracking();
    return nullptr;
  }
  if (g.state == State::kFailed) return nullptr;
  if (g.state == State::kOff) {
    if (!REXCVAR_GET(display_vr)) return nullptr;
    if (!Initialize(device, queue)) return nullptr;
  }
  PollEvents();
  if (g.state == State::kFailed || !g.session_running) return nullptr;
  XrFrameWaitInfo wait_info{XR_TYPE_FRAME_WAIT_INFO};
  XrFrameState frame_state{XR_TYPE_FRAME_STATE};
  if (XR_FAILED(g.api.WaitFrame(g.session, &wait_info, &frame_state))) return nullptr;
  XrFrameBeginInfo begin_info{XR_TYPE_FRAME_BEGIN_INFO};
  if (XR_FAILED(g.api.BeginFrame(g.session, &begin_info))) return nullptr;
  g.frame_begun = true;
  g.display_time = frame_state.predictedDisplayTime;
  g.should_render = frame_state.shouldRender == XR_TRUE;
  g.stereo_this_frame = false;
  g.hud_this_frame = false;
  LocateTracking();
  if (!g.should_render) return nullptr;

  // Stereo when both eyes have an image from the last quarter second (a
  // frame with a scene); otherwise the flat screen.
  if (g.eye_format) {
    Slot* eyes[2];
    Slot* hud;
    {
      std::lock_guard lock(g_mutex);
      eyes[0] = Newest(g_eye_slots[0]);
      eyes[1] = Newest(g_eye_slots[1]);
      hud = Newest(g_hud_slots);
    }
    const auto now = std::chrono::steady_clock::now();
    const auto fresh = [&](const Slot* slot) {
      return slot && now - slot->time < std::chrono::milliseconds(250);
    };
    if (fresh(eyes[0]) && fresh(eyes[1])) {
      bool ok = true;
      for (int eye = 0; eye < 2 && ok; ++eye) {
        const D3D12_RESOURCE_DESC desc = eyes[eye]->texture->GetDesc();
        ok = EnsureSwapchain(g.eyes[eye], g.eye_format, uint32_t(desc.Width), desc.Height);
        ID3D12Resource* image = ok ? Acquire(g.eyes[eye]) : nullptr;
        if (!image) {
          ok = false;
          break;
        }
        CopyToImage(list, eyes[eye]->texture.Get(), image);
        g.eye_tags[eye] = eyes[eye]->tag;
      }
      if (ok && hud) {
        const D3D12_RESOURCE_DESC desc = hud->texture->GetDesc();
        if (EnsureSwapchain(g.hud, g.eye_format, uint32_t(desc.Width), desc.Height)) {
          if (ID3D12Resource* image = Acquire(g.hud)) {
            CopyToImage(list, hud->texture.Get(), image);
            g.hud_this_frame = true;
          }
        }
      }
      g.stereo_this_frame = ok;
      // An open overlay (F1) is drawn on the presented image only: show
      // that image as the screen too.
      if (ok && !g_overlay_visible.load(std::memory_order_relaxed)) {
        std::lock_guard lock(g_mutex);
        g_screen_this_frame = false;
        return nullptr;
      }
    }
  }
  {
    std::lock_guard lock(g_mutex);
    g_screen_this_frame = width && height;
    if (width) g_screen_aspect = float(height) / float(width);
  }
  if (!width || !height || !EnsureSwapchain(g.screen, g.format, width, height)) return nullptr;
  return Acquire(g.screen);
}

void EndFrame(bool copied) {
  if (!g.frame_begun) return;
  Release(g.screen);
  Release(g.eyes[0]);
  Release(g.eyes[1]);
  Release(g.hud);
  std::vector<const XrCompositionLayerBaseHeader*> layers;
  XrCompositionLayerProjectionView views[2] = {{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW},
                                               {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}};
  XrCompositionLayerProjection projection{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
  XrCompositionLayerQuad hud{XR_TYPE_COMPOSITION_LAYER_QUAD};
  XrCompositionLayerQuad quad{XR_TYPE_COMPOSITION_LAYER_QUAD};
  if (g.should_render && g.stereo_this_frame) {
    for (int eye = 0; eye < 2; ++eye) {
      // The pose the game rendered this eye's image with.
      HeadView head;
      {
        std::lock_guard lock(g_mutex);
        head = g_head_history[(g.eye_tags[eye] & kFrameTagIdMask) % g_head_history.size()];
      }
      float offset[3];
      Rotate(head.orientation, head.eye_offset[eye], offset);
      XrCompositionLayerProjectionView& view = views[eye];
      view.pose.orientation = {head.orientation[0], head.orientation[1], head.orientation[2],
                               head.orientation[3]};
      view.pose.position = {head.position[0] + offset[0], head.position[1] + offset[1],
                            head.position[2] + offset[2]};
      // The image is the game's 16:9 frame: the vertical field follows from
      // the horizontal one the game rendered with.
      const float tan_x = head.tan_half_fov_x;
      const float tan_y = tan_x * float(g.eyes[eye].height) / float(std::max(1u, g.eyes[eye].width));
      view.fov = {-std::atan(tan_x), std::atan(tan_x), std::atan(tan_y), -std::atan(tan_y)};
      view.subImage.swapchain = g.eyes[eye].handle;
      view.subImage.imageRect.extent = {int32_t(g.eyes[eye].width), int32_t(g.eyes[eye].height)};
    }
    projection.space = g.space;
    projection.viewCount = 2;
    projection.views = views;
    layers.push_back(reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projection));
    if (g.hud_this_frame) {
      hud.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
      hud.space = g.space;
      hud.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
      hud.subImage.swapchain = g.hud.handle;
      hud.subImage.imageRect.extent = {int32_t(g.hud.width), int32_t(g.hud.height)};
      // In front of the player; with hand aim along the aim, so the HUD's
      // crosshair lies on the hand's ray (the title aims there).
      const float distance = float(REXCVAR_GET(display_vr_hud_distance_m));
      HeadView latest;
      {
        std::lock_guard lock(g_mutex);
        latest = g_head;
      }
      hud.pose.orientation.w = 1.0f;
      hud.pose.position = {0.0f, -0.05f, -distance};
      if (latest.aim_valid) {
        const float minus_z[3] = {0.0f, 0.0f, -distance};
        float along[3];
        Rotate(latest.aim, minus_z, along);
        hud.pose.orientation = {latest.aim[0], latest.aim[1], latest.aim[2], latest.aim[3]};
        hud.pose.position = {latest.position[0] + along[0], latest.position[1] + along[1],
                             latest.position[2] + along[2]};
      }
      const float hud_width = float(REXCVAR_GET(display_vr_hud_width_m));
      hud.size = {hud_width, hud_width * float(g.hud.height) / float(std::max(1u, g.hud.width))};
      layers.push_back(reinterpret_cast<const XrCompositionLayerBaseHeader*>(&hud));
    }
  }
  if (g.should_render && copied) {
    quad.space = g.space;
    quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
    quad.subImage.swapchain = g.screen.handle;
    quad.subImage.imageRect.extent = {int32_t(g.screen.width), int32_t(g.screen.height)};
    quad.pose.orientation.w = 1.0f;
    quad.pose.position = {0.0f, 0.0f, -2.0f};
    const float screen_width = float(REXCVAR_GET(display_vr_screen_width_m));
    quad.size = {screen_width,
                 g.screen.width ? screen_width * float(g.screen.height) / float(g.screen.width) : 1.0f};
    layers.push_back(reinterpret_cast<const XrCompositionLayerBaseHeader*>(&quad));
  }
  XrFrameEndInfo end{XR_TYPE_FRAME_END_INFO};
  end.displayTime = g.display_time;
  end.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
  end.layerCount = uint32_t(layers.size());
  end.layers = layers.data();
  const XrResult result = g.api.EndFrame(g.session, &end);
  g.frame_begun = false;
  if (XR_FAILED(result)) LogResult("end_frame", result);
  if (++g.frames == 1 || g.frames % 3600 == 0) {
    std::fprintf(stderr, "REX_OPENXR frames=%llu layers=%u stereo=%d hud=%d\n",
                 static_cast<unsigned long long>(g.frames), end.layerCount,
                 g.stereo_this_frame ? 1 : 0, g.hud_this_frame ? 1 : 0);
    std::fflush(stderr);
  }
}

void Shutdown() {
  if (g.frame_begun) EndFrame(false);
  for (Swapchain* chain : {&g.screen, &g.eyes[0], &g.eyes[1], &g.hud}) {
    if (chain->handle) g.api.DestroySwapchain(chain->handle);
    *chain = {};
  }
  for (XrSpace& space : g.hand_spaces) {
    if (space && g.api.DestroySpace) g.api.DestroySpace(space);
    space = XR_NULL_HANDLE;
  }
  if (g.space) g.api.DestroySpace(g.space);
  if (g.session) {
    if (g.session_running) g.api.EndSession(g.session);
    g.api.DestroySession(g.session);
  }
  if (g.instance && g.api.DestroyInstance) g.api.DestroyInstance(g.instance);
  g.space = XR_NULL_HANDLE;
  g.session = XR_NULL_HANDLE;
  g.instance = XR_NULL_HANDLE;
  g.action_set = XR_NULL_HANDLE;
  g.actions = false;
  g.session_running = false;
  g.format = 0;
  g.eye_format = 0;
  g.state = State::kOff;
  g_stereo_active.store(false, std::memory_order_release);
  // The loader stays loaded (the runtime may still hold threads).
}

}  // namespace rex::ui::d3d12::openxr_screen

#else  // !REX_HAVE_OPENXR: builds without the experiment (player builds).

namespace rex::ui::d3d12::openxr_screen {
bool Wanted() { return false; }
void TakeLook(double& yaw, double& pitch) { yaw = pitch = 0.0; }
float VignetteStrength() { return 0.0f; }
bool Pointer(float&, float&, bool&) { return false; }
void SetOverlayVisible(bool) {}
bool StereoActive() { return false; }
bool LatestHeadView(HeadView&) { return false; }
bool LatestControllers(Controllers&) { return false; }
ID3D12Resource* EyeTarget(ID3D12Device*, uint32_t, uint32_t, uint32_t) { return nullptr; }
ID3D12Resource* HudTarget(ID3D12Device*, uint32_t, uint32_t) { return nullptr; }
void Published(uint32_t, ID3D12Fence*, uint64_t) {}
bool PreviewSources(ID3D12Resource*&, ID3D12Resource*&, ID3D12Resource*&, uint32_t&) {
  return false;
}
ID3D12Resource* BeginFrame(ID3D12Device*, ID3D12CommandQueue*, uint32_t, uint32_t,
                           ID3D12GraphicsCommandList*) {
  return nullptr;
}
void EndFrame(bool) {}
void Shutdown() {}
}  // namespace rex::ui::d3d12::openxr_screen

#endif  // REX_HAVE_OPENXR
