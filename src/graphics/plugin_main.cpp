/**
 * @file        graphics/plugin_main.cpp
 * @brief       rexgpu-xenos plugin entry points
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <algorithm>
#include <array>
#include <atomic>
#include <rex/graphics/cp_interrupt_timing.h>
#include <rex/graphics/cp_interrupt_wakeup.h>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <rex/logging.h>
#include <rex/cvar.h>
REXCVAR_DECLARE(bool, embedded_interrupt_wait_wakeup);
REXCVAR_DECLARE(uint32_t, display_frame_limit);
REXCVAR_DECLARE(std::string, display_present_mode);
REXCVAR_DECLARE(std::string, input_bind_a);
REXCVAR_DECLARE(std::string, input_bind_b);
REXCVAR_DECLARE(std::string, input_bind_x);
REXCVAR_DECLARE(std::string, input_bind_y);
REXCVAR_DECLARE(std::string, input_bind_left_trigger);
REXCVAR_DECLARE(std::string, input_bind_right_trigger);
REXCVAR_DECLARE(std::string, input_bind_left_shoulder);
REXCVAR_DECLARE(std::string, input_bind_right_shoulder);
REXCVAR_DECLARE(std::string, input_bind_lstick_up);
REXCVAR_DECLARE(std::string, input_bind_lstick_down);
REXCVAR_DECLARE(std::string, input_bind_lstick_left);
REXCVAR_DECLARE(std::string, input_bind_lstick_right);
REXCVAR_DECLARE(std::string, input_bind_lstick_press);
REXCVAR_DECLARE(std::string, input_bind_rstick_press);
REXCVAR_DECLARE(std::string, input_bind_dpad_up);
REXCVAR_DECLARE(std::string, input_bind_dpad_down);
REXCVAR_DECLARE(std::string, input_bind_dpad_left);
REXCVAR_DECLARE(std::string, input_bind_dpad_right);
REXCVAR_DECLARE(std::string, input_bind_back);
REXCVAR_DECLARE(std::string, input_bind_start);
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dxgi1_6.h>
#include <intrin.h>
#endif
#include <rex/graphics/pipeline/texture/texture_pack.h>
#include <rex/graphics/pipeline/texture/prompt_icons.h>
#include <rex/ui/settings_detection.h>
#include <rex/ui/guest_frame_limiter.h>
#include <rex/ui/frame_rate_policy.h>
#include <rex/audio/xma/decoder.h>
#include <rex/crypto/sha256.h>
#include <rex/graphics/embedded_debug_overlay_policy.h>
#include <rex/graphics/embedded_config.h>
#include <rex/graphics/embedded_screenshot_policy.h>
#include <rex/system/gpu_plugin.h>
#include <rex/system/xmemory.h>
#include <rex/graphics/graphics_system.h>
#include <rex/graphics/pipeline/shader/replacement_pack.h>
#include <rex/input/input.h>
#include <rex/input/mnk/mnk_input_driver.h>
#include <rex/ui/flags.h>
#include <rex/ui/imgui_drawer.h>
#include <rex/ui/keybinds.h>
#include <rex/ui/overlay/debug_overlay.h>
#include <rex/ui/overlay/host_settings_overlay.h>
#include <rex/ui/presenter.h>
#include <rex/ui/settings_schema.h>
#include <rex/ui/sdl_virtual_key.h>
#include <rex/ui/window_sdl.h>
#include <rex/ui/window.h>
#include <rex/ui/window_mode_policy.h>
#include <rex/ui/windowed_app_context_sdl.h>

REXCVAR_DEFINE_INT32(pc_config_version, 1, "PC",
                     "The Darkness PC configuration schema version")
    .range(1, 1)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

// V376: the player's view, horizontal degrees on a 16:9 screen (the title's
// 70 degrees at 4:3 = 86). camera_gameplay_fov is the pre-V376 key: it edited
// a viewport the title overwrites, so it never changed the view; still read
// from old configurations, but ignored.
REXCVAR_DEFINE_DOUBLE(camera_field_of_view, 86.0, "Camera",
                      "Gameplay field of view: horizontal degrees on a 16:9 screen "
                      "(86 = the original view)")
    .range(60.0, 120.0)
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_DOUBLE(camera_gameplay_fov, 95.0, "Camera",
                      "Pre-V376 field of view key; ignored (it never changed the view)")
    .range(60.0, 120.0)
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_BOOL(diagnostics_camera_state, false, "Diagnostics",
                    "Write a bounded title-camera setter classification log")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_STRING(display_frame_rate, "original", "Display",
                      "Frame rate: original (title pacing), 60, half_refresh, refresh, "
                      "custom (display.frame_limit), uncapped or a whole-number cap (20-1000)")
    .validator([](std::string_view value) { return rex::ui::IsFrameRateValue(value); });

// Menus, the title screen and loading (reported by the host through
// rex_gpu_embedded_set_menu_state): "reduced" paces them at about 60 frames
// per second at most (a whole divisor of the refresh with VSync), "full" uses
// the gameplay frame rate. The title's front end renders a full 3D backdrop
// that costs more GPU time per frame than gameplay (V327: 8.9 ms at internal
// 2x), so uncapped menus mostly produce heat and fan noise.
REXCVAR_DEFINE_STRING(display_menu_frame_rate, "reduced", "Display",
                      "Menu frame rate: reduced (about 60 FPS at most) or full (as gameplay)")
    .allowed({"reduced", "full"});

REXCVAR_DEFINE_BOOL(dev_frame_mode_hotkeys, false, "Advanced/Developer",
                    "Test launches only: F10 cycles frame-rate and present modes live")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
// In-game settings overlay (host schema, see rex_gpu_embedded_settings_*).
// Only function keys the host does not reserve (F3 debug overlay, F9 phase
// markers, F10 developer modes, F12 screenshots, Alt+F4).
REXCVAR_DEFINE_STRING(input_overlay_key, "F1", "Input",
                      "Key that opens the in-game settings overlay (F1, F2, F4-F8, F11)")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_BOOL(graphics_motion_blur, true, "Graphics",
                    "Enable the title-native final-composite motion blur")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_STRING(
    graphics_motion_blur_off_shader_pack, "", "Graphics",
    "Motion blur off: a shader replacement pack file to use instead of the built-in "
    "patch of the title's own composite shaders (empty or the former default "
    "runtime_data/TheDarkness.motion_blur_off.xsrp = built-in)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace {
// Motion blur off, built in: the title's final composite XREngine_Final5 in
// its eight motion-blur permutations (flag bit 0; bits 1-3 exposure, RGB map,
// glow). Each takes its blur offset from one MAD whose two constant operands
// are repointed to c252 - its selected components are zero in all eight - so
// the offset is (0, 0) while bindings, constants, control flow, exposure,
// grading and glow stay the title's. The patch is made from the player's own
// shader microcode when it loads; the same bytes as the former
// TheDarkness.motion_blur_off.xsrp pack, which is no longer needed.
constexpr uint64_t kMotionBlurComposites[] = {
    UINT64_C(0x5466CF9942F35964), UINT64_C(0xA167D4EAF622F7C2), UINT64_C(0xED6B74C3B52BE18F),
    UINT64_C(0x41AA202FFE632222), UINT64_C(0x147A61D164BBEBB9), UINT64_C(0x7C6AF8B69563DB69),
    UINT64_C(0xF7B11AA01AB700F3), UINT64_C(0xA59B41D0BD79484B)};
// The built-in patch makes exactly the microcode of the former pack, so it
// keeps that pack's identity (its SHA-256): shader caches stay valid.
constexpr char kMotionBlurPatchIdentity[] =
    "f2f6fe7b991c0aaabb1d3e0369045e7cd3d23798c5ed37a8817dcf06daad5cc7";
// Configurations and presets name the former pack; it means the built-in
// patch now.
constexpr char kLegacyMotionBlurPack[] = "runtime_data/TheDarkness.motion_blur_off.xsrp";

uint32_t GuestWord(uint32_t stored) { return _byteswap_ulong(stored); }

bool PatchMotionBlurOff(std::vector<uint32_t>& ucode) {
  // ALU instruction: 3 dwords; word 0 = destination (the motion coordinate),
  // word 2 = operand selects with both constant operands >= 0xFD.
  constexpr uint32_t kDestination = 0xC80C0000u;
  constexpr uint32_t kSourceMask = 0xFFFF0000u;
  constexpr uint32_t kSourceShape = 0x8B000000u;
  size_t found = 0;
  size_t count = 0;
  for (size_t i = 0; i + 3 <= ucode.size(); i += 3) {
    const uint32_t word0 = GuestWord(ucode[i]);
    const uint32_t word2 = GuestWord(ucode[i + 2]);
    if (word0 == kDestination && (word2 & kSourceMask) == kSourceShape &&
        ((word2 >> 8) & 0xFFu) >= 0xFDu && (word2 & 0xFFu) >= 0xFDu) {
      found = i;
      ++count;
    }
  }
  if (count != 1) return false;
  const uint32_t patched = (GuestWord(ucode[found + 2]) & kSourceMask) | 0xFCFCu;
  if (patched != 0x8B00FCFCu) return false;
  ucode[found + 2] = GuestWord(patched);
  return true;
}
}  // namespace

#if REX_HAS_D3D12
#include <rex/graphics/embedded_diagnostics.h>
#include <rex/graphics/d3d12/graphics_system.h>
#include <rex/graphics/d3d12/pipeline_cache.h>
#include <rex/graphics/d3d12/command_processor.h>
#include <rex/ui/overlay/shader_prewarm_indicator.h>
#endif
#if REX_HAS_VULKAN
#include <rex/graphics/vulkan/graphics_system.h>
#endif


// Keyboard button prompts (prompt_icons.h): the key bound to a controller
// button, from this plugin's input cvars (the overlay changes them live).
static std::string PromptBindingValue(std::string_view button) {
  if (button == "a") return REXCVAR_GET(input_bind_a);
  if (button == "b") return REXCVAR_GET(input_bind_b);
  if (button == "x") return REXCVAR_GET(input_bind_x);
  if (button == "y") return REXCVAR_GET(input_bind_y);
  if (button == "left_trigger") return REXCVAR_GET(input_bind_left_trigger);
  if (button == "right_trigger") return REXCVAR_GET(input_bind_right_trigger);
  if (button == "left_shoulder") return REXCVAR_GET(input_bind_left_shoulder);
  if (button == "right_shoulder") return REXCVAR_GET(input_bind_right_shoulder);
  if (button == "lstick_up") return REXCVAR_GET(input_bind_lstick_up);
  if (button == "lstick_down") return REXCVAR_GET(input_bind_lstick_down);
  if (button == "lstick_left") return REXCVAR_GET(input_bind_lstick_left);
  if (button == "lstick_right") return REXCVAR_GET(input_bind_lstick_right);
  if (button == "lstick_press") return REXCVAR_GET(input_bind_lstick_press);
  if (button == "rstick_press") return REXCVAR_GET(input_bind_rstick_press);
  if (button == "dpad_up") return REXCVAR_GET(input_bind_dpad_up);
  if (button == "dpad_down") return REXCVAR_GET(input_bind_dpad_down);
  if (button == "dpad_left") return REXCVAR_GET(input_bind_dpad_left);
  if (button == "dpad_right") return REXCVAR_GET(input_bind_dpad_right);
  if (button == "back") return REXCVAR_GET(input_bind_back);
  if (button == "start") return REXCVAR_GET(input_bind_start);
  return {};
}

extern "C" REX_GPU_PLUGIN_EXPORT uint32_t rex_gpu_abi_version(void) {
  return rex::system::kGpuPluginAbiVersion;
}

extern "C" REX_GPU_PLUGIN_EXPORT rex::system::IGraphicsSystem* rex_gpu_create(
    uint32_t abi_version, const rex::system::GpuCreateInfo* info) {
  if (abi_version != rex::system::kGpuPluginAbiVersion) {
    REXLOG_ERROR("rexgpu-xenos: host requested ABI {}, plugin is ABI {}", abi_version,
                 rex::system::kGpuPluginAbiVersion);
    return nullptr;
  }
  if (!info || info->struct_size < sizeof(rex::system::GpuCreateInfo)) {
    REXLOG_ERROR("rexgpu-xenos: invalid GpuCreateInfo");
    return nullptr;
  }

  std::string_view backend = info->backend ? info->backend : "any";
#if REX_HAS_D3D12
  if (backend == "any" || backend == "d3d12") {
    return new rex::graphics::d3d12::D3D12GraphicsSystem();
  }
#endif
#if REX_HAS_VULKAN
  if (backend == "any" || backend == "vulkan") {
    return new rex::graphics::vulkan::VulkanGraphicsSystem();
  }
#endif
  REXLOG_ERROR("rexgpu-xenos: requested backend '{}' is not compiled into this plugin", backend);
  return nullptr;
}

#if defined(_WIN32)
double QueryWindowRefreshHz(void* window);
uint64_t QueryCardVideoMemoryBytes();
bool QueryNvidiaRtxCard();
#endif

namespace {
constexpr bool kEmbeddedOutputProbeEnabled = false;

using rex::graphics::ResolveEmbeddedShaderReplacementPackPath;

struct EmbeddedGpu;

class EmbeddedScreenshotListener final : public rex::ui::WindowInputListener {
 public:
  explicit EmbeddedScreenshotListener(EmbeddedGpu& embedded)
      : embedded_(embedded) {}

  void OnKeyDown(rex::ui::KeyEvent& event) override;

 private:
  EmbeddedGpu& embedded_;
};

class EmbeddedDebugOverlayListener final
    : public rex::ui::WindowInputListener {
 public:
  EmbeddedDebugOverlayListener(rex::ui::Window* window,
                               rex::graphics::GraphicsSystem* graphics)
      : window_(window), graphics_(graphics) {}
  ~EmbeddedDebugOverlayListener() override { Reset(); }

  void OnKeyDown(rex::ui::KeyEvent& event) override;
  void Reset();

 private:
  bool EnsureDrawer();

  rex::ui::Window* window_ = nullptr;
  rex::graphics::GraphicsSystem* graphics_ = nullptr;
  std::unique_ptr<rex::ui::ImmediateDrawer> immediate_drawer_;
  std::unique_ptr<rex::ui::ImGuiDrawer> imgui_drawer_;
  std::unique_ptr<rex::ui::DebugOverlayDialog> dialog_;
};

// In-game settings overlay: the host's settings schema rendered with the
// shared settings panel on its own ImGui drawer. While it is open the game
// gets no keyboard/mouse input (EmbeddedOverlayInputBlocker below the drawer,
// MnK inactive so the cursor is free) and the host neutralizes the pad.
class EmbeddedSettingsOverlayListener final : public rex::ui::WindowInputListener {
 public:
  EmbeddedSettingsOverlayListener(EmbeddedGpu& embedded, rex::ui::Window* window,
                                  rex::graphics::GraphicsSystem* graphics);
  ~EmbeddedSettingsOverlayListener() override { Reset(); }

  void OnKeyDown(rex::ui::KeyEvent& event) override;
  void Reset();
  // The controller chord (EmbeddedGamepadOverlayPoller), on the UI thread.
  void Toggle() {
    if (dialog_) {
      Close();
    } else {
      Open();
    }
  }

 private:
  bool EnsureDrawer();
  void Open();
  void Close();

  EmbeddedGpu& embedded_;
  rex::ui::Window* window_ = nullptr;
  rex::graphics::GraphicsSystem* graphics_ = nullptr;
  rex::ui::VirtualKey key_ = rex::ui::VirtualKey::kF1;
  std::string key_name_ = "F1";
  std::unique_ptr<rex::ui::ImmediateDrawer> immediate_drawer_;
  std::unique_ptr<rex::ui::ImGuiDrawer> imgui_drawer_;
  rex::ui::HostSettingsOverlayDialog* dialog_ = nullptr;  // see its header
  ImFont* font_ = nullptr;
};

class EmbeddedOverlayInputBlocker final : public rex::ui::WindowInputListener {
 public:
  explicit EmbeddedOverlayInputBlocker(EmbeddedGpu& embedded) : embedded_(embedded) {}
  // Key-up and mouse-up pass so nothing held stays down in the game.
  void OnKeyDown(rex::ui::KeyEvent& event) override { Block(event); }
  void OnKeyChar(rex::ui::KeyEvent& event) override { Block(event); }
  void OnMouseDown(rex::ui::MouseEvent& event) override { Block(event); }
  void OnMouseMove(rex::ui::MouseEvent& event) override { Block(event); }
  void OnMouseWheel(rex::ui::MouseEvent& event) override { Block(event); }

 private:
  template <typename Event>
  void Block(Event& event);
  EmbeddedGpu& embedded_;
};

// Test launches only (dev_frame_mode_hotkeys): F10 cycles live frame-rate
// modes so one isolated arrival can cover every mode. Each switch is logged
// with its QPC tick for phase splitting. Never enabled in player configs.
struct DevFrameMode {
  const char* name;
  uint32_t limit;       // guest production cap, 0 = none
  int32_t present;      // rex::ui::d3d12::HostPresentMode value
  uint32_t interval;    // vsync interval when present == vsync
};
constexpr DevFrameMode kDevFrameModes[] = {
    {"uncapped_immediate", 0, 1, 0}, {"vsync_refresh", 0, 0, 1},
    {"limit120_immediate", 120, 1, 0}, {"limit90_immediate", 90, 1, 0},
    {"vsync_half_refresh", 0, 0, 2}, {"limit60_immediate", 60, 1, 0},
    {"limit45_immediate", 45, 1, 0}, {"limit120_vsync", 120, 0, 1},
    {"limit144_immediate", 144, 1, 0},
};

class EmbeddedDevFrameModeListener final : public rex::ui::WindowInputListener {
 public:
  explicit EmbeddedDevFrameModeListener(EmbeddedGpu& embedded) : embedded_(embedded) {}
  void OnKeyDown(rex::ui::KeyEvent& event) override;

 private:
  EmbeddedGpu& embedded_;
  size_t index_ = SIZE_MAX;
};

struct EmbeddedGpu {
  EmbeddedGpu() : screenshot_listener(*this) {}

  rex::memory::Memory memory;
  std::unique_ptr<rex::graphics::GraphicsSystem> graphics;
  // The host's payload guards (source tracking), chained by the plugin's own
  // host-write callbacks (EmbeddedHostWriteBegin/End).
  rex::memory::HostWriteCallbacks runtime_host_writes{};
  // Set once the GPU is set up, cleared first on destroy: host writes made
  // inside the plugin invalidate the GPU copy of their pages through it.
  std::atomic<rex::graphics::GraphicsSystem*> host_write_invalidation{nullptr};
  std::unique_ptr<rex::audio::XmaDecoder> xma;
  std::unique_ptr<rex::input::mnk::MnkInputDriver> keyboard_mouse;
  std::thread presentation_thread;
  std::mutex presentation_mutex;
  std::condition_variable presentation_condition;
  rex::ui::SDLWindowedAppContext* presentation_context = nullptr;
  bool presentation_initialized = false;
  bool presentation_succeeded = false;
  std::filesystem::path screenshot_root;
  EmbeddedScreenshotListener screenshot_listener;
  std::thread screenshot_thread;
  std::mutex screenshot_mutex;
  std::condition_variable screenshot_condition;
  bool screenshot_stop = true;
  uint32_t screenshot_pending = 0;
  uint64_t screenshot_ordinal = 0;
  std::thread output_probe_thread;
  std::atomic<bool> output_probe_stop = false;
  // Guest frame-production pacing (rex_gpu_embedded_frame_boundary).
  std::atomic<void*> native_window{nullptr};
  std::atomic<int32_t> dev_frame_limit_override{-1};
  std::mutex frame_limiter_mutex;
  rex::ui::GuestFrameDeadline frame_deadline;
  rex::ui::GuestFrameSpinMargin frame_spin{0, 0, 0};
  uint64_t qpc_frequency = 0;
  uint64_t frame_boundaries = 0;
  uint64_t frame_waits = 0;
  uint64_t frame_background_capped = 0;
  bool frame_limiter_announced = false;
#if defined(_WIN32)
  HANDLE frame_timer = nullptr;
#endif
  // Developer pacing statistics (dev_frame_mode_hotkeys only): intervals
  // between consecutive production releases in 0.1 ms bins (0..51.1 ms).
  std::array<uint32_t, 512> pacing_hist{};
  uint64_t pacing_last_release = 0;
  uint64_t pacing_frames = 0;
  uint64_t pacing_sum_ticks = 0;
  uint64_t pacing_max_ticks = 0;
  uint32_t pacing_target = 0;
  std::atomic<bool> pacing_report_requested{false};
  std::string pacing_mode_name = "configured";
  // Player-facing frame-rate policy (display.frame_rate), re-resolved every
  // two seconds against the refresh rate of the window's current display.
  rex::ui::FrameRatePolicy frame_policy{};
  uint64_t frame_policy_tick = 0;
  double frame_policy_refresh_hz = -1.0;
  std::string frame_policy_key;
  // Host-reported menu state (rex_gpu_embedded_set_menu_state) and whether
  // the resolved policy is the menu one.
  std::atomic<bool> menu_active{false};
  bool frame_policy_menu = false;
  uint64_t menu_transitions = 0;
  // In-game settings overlay (host schema, rex_gpu_embedded_settings_*).
  rex::ui::HostSettingsState settings_state;
  std::atomic<bool> settings_overlay_open{false};
  // The controller as the overlay sees it (EmbeddedGamepadOverlayPoller):
  // XInput buttons, and the left stick packed as (x << 16) | y.
  std::atomic<uint32_t> overlay_pad_buttons{0};
  std::atomic<uint32_t> overlay_pad_stick{0};
  // V380: the player closed the window (close button, Alt+F4). The runtime
  // polls this and stops the title cleanly; a watchdog hard-exits if that
  // never completes, so a closed game never keeps running without a window.
  std::atomic<bool> close_requested{false};
};

template <typename Event>
void EmbeddedOverlayInputBlocker::Block(Event& event) {
  if (embedded_.settings_overlay_open.load(std::memory_order_acquire)) {
    event.set_handled(true);
  }
}

// Allowed overlay keys: function keys the host does not reserve.
rex::ui::VirtualKey ResolveOverlayKey(std::string& name) {
  using rex::ui::VirtualKey;
  const VirtualKey key = rex::ui::ParseVirtualKey(name);
  switch (key) {
    case VirtualKey::kF1: case VirtualKey::kF2: case VirtualKey::kF4:
    case VirtualKey::kF5: case VirtualKey::kF6: case VirtualKey::kF7:
    case VirtualKey::kF8: case VirtualKey::kF11:
      return key;
    default:
      std::fprintf(stderr, "REX_SETTINGS_OVERLAY key=%s rejected fallback=F1\n", name.c_str());
      name = "F1";
      return VirtualKey::kF1;
  }
}

// Developer test input: the script's host events (press <key>,
// click <x> <y> in physical client pixels) handed to the game window on its UI
// thread exactly like SDL input, independent of window focus.
void DeliverHostTestEvent(rex::ui::WindowSDL& window,
                          const rex::input::mnk::test_script_policy::Command& command) {
  using Op = rex::input::mnk::test_script_policy::Op;
  if (command.op == Op::kPress) {
    SDL_Scancode scancode = SDL_SCANCODE_UNKNOWN;
    for (int code = 0; code < SDL_SCANCODE_COUNT; ++code) {
      if (uint16_t(rex::ui::TranslateSDLScancode(SDL_Scancode(code))) == command.key) {
        scancode = SDL_Scancode(code);
        break;
      }
    }
    if (scancode == SDL_SCANCODE_UNKNOWN) return;
    SDL_Event event{};
    event.type = SDL_EVENT_KEY_DOWN;
    event.key.scancode = scancode;
    event.key.down = true;
    window.HandleKeyEvent(event);
    event.type = SDL_EVENT_KEY_UP;
    event.key.down = false;
    window.HandleKeyEvent(event);
  } else if (command.op == Op::kClick) {
    const float density = window.PixelDensity();
    const float x = float(command.dx) / density;
    const float y = float(command.dy) / density;
    SDL_Event motion{};
    motion.type = SDL_EVENT_MOUSE_MOTION;
    motion.motion.x = x;
    motion.motion.y = y;
    window.HandleMouseEvent(motion);
    SDL_Event button{};
    button.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
    button.button.button = SDL_BUTTON_LEFT;
    button.button.down = true;
    button.button.x = x;
    button.button.y = y;
    window.HandleMouseEvent(button);
    button.type = SDL_EVENT_MOUSE_BUTTON_UP;
    button.button.down = false;
    window.HandleMouseEvent(button);
  }
}

EmbeddedSettingsOverlayListener::EmbeddedSettingsOverlayListener(
    EmbeddedGpu& embedded, rex::ui::Window* window, rex::graphics::GraphicsSystem* graphics)
    : embedded_(embedded), window_(window), graphics_(graphics) {
  key_name_ = REXCVAR_GET(input_overlay_key);
  key_ = ResolveOverlayKey(key_name_);
  std::fprintf(stderr, "REX_SETTINGS_OVERLAY key=%s\n", key_name_.c_str());
  std::fflush(stderr);
}

bool EmbeddedSettingsOverlayListener::EnsureDrawer() {
  if (imgui_drawer_) return true;
  if (!window_ || !graphics_ || !graphics_->provider() || !graphics_->presenter()) return false;
  immediate_drawer_ = graphics_->provider()->CreateImmediateDrawer();
  if (!immediate_drawer_) return false;
  immediate_drawer_->SetPresenter(graphics_->presenter());
  imgui_drawer_ = std::make_unique<rex::ui::ImGuiDrawer>(
      window_, 64, rex::ui::HostSettingsFontSetup(&font_));
  imgui_drawer_->SetPresenterAndImmediateDrawer(graphics_->presenter(),
                                                immediate_drawer_.get());
  return true;
}

void EmbeddedSettingsOverlayListener::Open() {
  if (dialog_ || !EnsureDrawer()) return;
#if defined(_WIN32)
  {
    // The frame-rate list follows the display the game window is on.
    const double refresh =
        QueryWindowRefreshHz(embedded_.native_window.load(std::memory_order_acquire));
    std::lock_guard<std::mutex> lock(embedded_.settings_state.mutex);
    embedded_.settings_state.refresh_hz = refresh;
  }
  {
    // HD texture packs: what is installed now (names and sizes only).
    const auto pack =
        rex::ui::settings::ScanTexturePackFolder(rex::graphics::texture_pack::PackFolder());
    const uint64_t video_memory = QueryCardVideoMemoryBytes();
    std::lock_guard<std::mutex> lock(embedded_.settings_state.mutex);
    embedded_.settings_state.texture_pack = pack;
    embedded_.settings_state.video_memory_bytes = video_memory;
    embedded_.settings_state.texture_pack_where = rex::graphics::texture_pack::PackFolderHint();
    ++embedded_.settings_state.texture_pack_generation;
  }
  {
    // Vendor upscalers: the runtime DLL in the game folder and the SDK in
    // this build.
    std::vector<wchar_t> module(1024);
    const DWORD length = GetModuleFileNameW(nullptr, module.data(), DWORD(module.size()));
    rex::ui::settings::UpscalerRuntimes upscalers;
    if (length && length < module.size()) {
      upscalers = rex::ui::settings::ScanUpscalerRuntimes(
          std::filesystem::path(module.data(), module.data() + length).parent_path());
    }
#if !REX_HAVE_DLSS
    upscalers.dlss = false;
#endif
#if !REX_HAVE_FSR
    upscalers.fsr = false;
#endif
#if !REX_HAVE_XESS
    upscalers.xess = false;
#endif
    if (upscalers.dlss) upscalers.dlss_card = QueryNvidiaRtxCard();
    std::lock_guard<std::mutex> lock(embedded_.settings_state.mutex);
    embedded_.settings_state.upscalers = upscalers;
    embedded_.settings_state.upscalers_known = true;
  }
#endif
  dialog_ = new rex::ui::HostSettingsOverlayDialog(
      imgui_drawer_.get(), embedded_.settings_state, font_, key_name_,
      [this](const rex::ui::settings::Setting& setting, const std::string& value) {
        // Live settings owned by this plugin are cvars named like the key.
        std::string name = setting.key;
        std::replace(name.begin(), name.end(), '.', '_');
        if (!rex::cvar::GetFlagInfo(name)) return;
        rex::cvar::SetFlagByName(name, value);
        // The presenter keeps its own copy of the present_* values.
        if (name.rfind("present_", 0) == 0 && graphics_ && graphics_->presenter()) {
          graphics_->presenter()->SetGuestOutputPaintConfigFromUIThread(
              rex::ui::Presenter::GuestOutputPaintConfigFromCVars());
        }
      },
      [this]() {
        // Resume: the dialog deletes itself right after this.
        dialog_ = nullptr;
        embedded_.settings_overlay_open.store(false, std::memory_order_release);
        std::fprintf(stderr, "REX_SETTINGS_OVERLAY open=0 via=resume\n");
        std::fflush(stderr);
      },
      [this]() {
        // Quit game: the close request of Alt+F4, so the runtime's close
        // listener stops the title (RequestClose would destroy the window
        // without asking it).
        std::fprintf(stderr, "REX_SETTINGS_OVERLAY quit=1\n");
        std::fflush(stderr);
        static_cast<rex::ui::WindowSDL*>(window_)->PostCloseRequest();
      },
      [this]() {
        const uint32_t stick = embedded_.overlay_pad_stick.load(std::memory_order_acquire);
        rex::ui::OverlayGamepadState pad;
        pad.buttons = uint16_t(embedded_.overlay_pad_buttons.load(std::memory_order_acquire));
        pad.thumb_lx = int16_t(uint16_t(stick >> 16));
        pad.thumb_ly = int16_t(uint16_t(stick & 0xFFFF));
        return pad;
      });
  embedded_.settings_overlay_open.store(true, std::memory_order_release);
  std::fprintf(stderr, "REX_SETTINGS_OVERLAY open=1\n");
  std::fflush(stderr);
}

void EmbeddedSettingsOverlayListener::Close() {
  if (!dialog_) return;
  delete dialog_;  // not drawing: key events and drawing share the UI thread
  dialog_ = nullptr;
  embedded_.settings_overlay_open.store(false, std::memory_order_release);
  std::fprintf(stderr, "REX_SETTINGS_OVERLAY open=0\n");
  std::fflush(stderr);
}

void EmbeddedSettingsOverlayListener::OnKeyDown(rex::ui::KeyEvent& event) {
  const bool toggle = event.virtual_key() == key_ && !event.is_alt_pressed();
  // While a key editor captures, Escape is a binding (Start's default).
  const bool escape = dialog_ && event.virtual_key() == rex::ui::VirtualKey::kEscape &&
                      !dialog_->CapturingKey();
  if (!toggle && !escape) return;
  event.set_handled(true);
  if (event.prev_state()) return;  // fresh press only
  if (dialog_) {
    Close();
  } else if (toggle) {
    Open();
  }
}

void EmbeddedSettingsOverlayListener::Reset() {
  Close();
  if (imgui_drawer_) {
    imgui_drawer_->SetPresenterAndImmediateDrawer(nullptr, nullptr);
    imgui_drawer_.reset();
  }
  if (immediate_drawer_) {
    immediate_drawer_->SetPresenter(nullptr);
    immediate_drawer_.reset();
  }
}

void QueueEmbeddedScreenshot(EmbeddedGpu& embedded) {
  bool queued = false;
  {
    std::lock_guard<std::mutex> lock(embedded.screenshot_mutex);
    if (!embedded.screenshot_stop &&
        embedded.screenshot_pending <
            rex::graphics::embedded_screenshot_policy::kMaximumPendingCaptures) {
      ++embedded.screenshot_pending;
      queued = true;
    }
  }
  if (queued) {
    embedded.screenshot_condition.notify_one();
  } else {
    std::fprintf(stderr,
                 "REX_EMBEDDED_SCREENSHOT result=0 reason=unavailable_or_queue_full\n");
    std::fflush(stderr);
  }
}

void EmbeddedScreenshotListener::OnKeyDown(rex::ui::KeyEvent& event) {
  if (event.virtual_key() == rex::ui::VirtualKey::kF9) {
    // Play-testing snapshot (#14): a screenshot plus the next whole guest
    // frame's draws and resolves (diag_<time>_frame.txt beside it). Reads GPU
    // registers only; idle until pressed.
    event.set_handled(true);
    if (event.prev_state()) return;
    static uint32_t snapshots = 0;
    std::time_t now = std::time(nullptr);
    std::tm local{};
#if defined(_WIN32)
    localtime_s(&local, &now);
#else
    localtime_r(&now, &local);
#endif
    char name[64];
    std::strftime(name, sizeof(name), "diag_%Y%m%d_%H%M%S", &local);
    const std::filesystem::path path =
        embedded_.screenshot_root / (std::string(name) + "_" + std::to_string(++snapshots) +
                                     "_frame.txt");
    const std::string text = path.u8string();  // UTF-8 (C++17)
#if REX_HAS_D3D12
    rex::graphics::d3d12::RequestEmbeddedFrameDump(text.c_str());
    rex::graphics::d3d12::RequestEmbeddedGameplayCapture();
#endif
    QueueEmbeddedScreenshot(embedded_);
    std::fprintf(stderr, "REX_DIAG_SNAPSHOT key=F9 index=%u frame_dump=%s\n", snapshots,
                 text.c_str());
    std::fflush(stderr);
    return;
  }
  if (event.virtual_key() != rex::ui::VirtualKey::kF12) {
    return;
  }
  // F12 belongs to the host capture service and must never become a title
  // input binding. Repeats remain handled but only a fresh physical edge is
  // allowed to enqueue a capture.
  event.set_handled(true);
  if (rex::graphics::embedded_screenshot_policy::IsCaptureRequest(
          event.virtual_key(), event.prev_state())) {
#if REX_HAS_D3D12
    rex::graphics::d3d12::RequestEmbeddedGameplayCapture();
#endif
    QueueEmbeddedScreenshot(embedded_);
  }
}

bool EmbeddedDebugOverlayListener::EnsureDrawer() {
  if (imgui_drawer_) {
    return true;
  }
  if (!window_ || !graphics_ || !graphics_->provider() ||
      !graphics_->presenter()) {
    return false;
  }
  immediate_drawer_ = graphics_->provider()->CreateImmediateDrawer();
  if (!immediate_drawer_) {
    return false;
  }
  immediate_drawer_->SetPresenter(graphics_->presenter());
  imgui_drawer_ = std::make_unique<rex::ui::ImGuiDrawer>(window_, 64);
  imgui_drawer_->SetPresenterAndImmediateDrawer(graphics_->presenter(),
                                                immediate_drawer_.get());
  return true;
}

void EmbeddedDevFrameModeListener::OnKeyDown(rex::ui::KeyEvent& event) {
  if (event.virtual_key() != rex::ui::VirtualKey::kF10 || event.prev_state()) {
    return;
  }
  event.set_handled(true);
  index_ = index_ == SIZE_MAX ? 0 : (index_ + 1) % std::size(kDevFrameModes);
  const DevFrameMode& mode = kDevFrameModes[index_];
  embedded_.pacing_report_requested.store(true, std::memory_order_release);
  embedded_.dev_frame_limit_override.store(int32_t(mode.limit), std::memory_order_release);
  rex::ui::SetHostPresentOverride(mode.present, mode.interval);
  uint64_t qpc = 0;
#if defined(_WIN32)
  LARGE_INTEGER now;
  QueryPerformanceCounter(&now);
  qpc = uint64_t(now.QuadPart);
#endif
  std::fprintf(stderr,
               "REX_DEV_FRAME_MODE index=%zu name=%s limit=%u present=%d interval=%u qpc=%llu\n",
               index_, mode.name, mode.limit, mode.present, mode.interval,
               static_cast<unsigned long long>(qpc));
  std::fflush(stderr);
}

void EmbeddedDebugOverlayListener::OnKeyDown(rex::ui::KeyEvent& event) {
  if (event.virtual_key() != rex::ui::VirtualKey::kF3) {
    return;
  }
  // F3 is host-only even while held. Only a fresh physical edge toggles.
  event.set_handled(true);
  if (!rex::graphics::embedded_debug_overlay_policy::IsToggleRequest(
          event.virtual_key(), event.prev_state())) {
    return;
  }
  if (dialog_) {
    dialog_.reset();
    return;
  }
  if (!EnsureDrawer()) {
    return;
  }
  dialog_ = std::make_unique<rex::ui::DebugOverlayDialog>(
      imgui_drawer_.get(), [graphics = graphics_]() {
        rex::ui::FrameStats result{};
        const auto* command_processor = graphics->command_processor();
        if (!command_processor) {
          return result;
        }
        const auto stats = command_processor->guest_frame_stats();
        result.frame_count = stats.frame_count;
        result.frame_time_ms = double(stats.frame_time_us) / 1000.0;
        result.fps = stats.frame_time_us
                         ? 1000000.0 / double(stats.frame_time_us)
                         : 0.0;
        return result;
      });
}

void EmbeddedDebugOverlayListener::Reset() {
  dialog_.reset();
  if (imgui_drawer_) {
    imgui_drawer_->SetPresenterAndImmediateDrawer(nullptr, nullptr);
    imgui_drawer_.reset();
  }
  if (immediate_drawer_) {
    immediate_drawer_->SetPresenter(nullptr);
    immediate_drawer_.reset();
  }
}

FILE* OpenExclusiveScreenshotFile(const std::filesystem::path& path) {
#if defined(_WIN32)
  FILE* file = nullptr;
  return _wfopen_s(&file, path.c_str(), L"wbx") == 0 ? file : nullptr;
#else
  return std::fopen(path.c_str(), "wbx");
#endif
}

bool WriteEmbeddedOutputBmp(const rex::ui::RawImage& image,
                            const std::filesystem::path& path) {
  if (!image.width || !image.height ||
      image.stride < size_t(image.width) * sizeof(uint32_t) ||
      image.data.size() < image.stride * image.height) {
    return false;
  }

  const uint32_t row_bytes = image.width * 3;
  const uint32_t padded_row_bytes = (row_bytes + 3) & ~uint32_t(3);
  const uint32_t pixel_bytes = padded_row_bytes * image.height;
  const uint32_t file_bytes = 54 + pixel_bytes;
  uint8_t header[54]{};
  const auto write_u16 = [&header](size_t offset, uint16_t value) {
    header[offset] = uint8_t(value);
    header[offset + 1] = uint8_t(value >> 8);
  };
  const auto write_u32 = [&header](size_t offset, uint32_t value) {
    header[offset] = uint8_t(value);
    header[offset + 1] = uint8_t(value >> 8);
    header[offset + 2] = uint8_t(value >> 16);
    header[offset + 3] = uint8_t(value >> 24);
  };
  header[0] = 'B';
  header[1] = 'M';
  write_u32(2, file_bytes);
  write_u32(10, 54);
  write_u32(14, 40);
  write_u32(18, image.width);
  write_u32(22, image.height);
  write_u16(26, 1);
  write_u16(28, 24);
  write_u32(34, pixel_bytes);

  FILE* file = OpenExclusiveScreenshotFile(path);
  if (!file) {
    return false;
  }
  bool succeeded = std::fwrite(header, 1, sizeof(header), file) == sizeof(header);
  std::vector<uint8_t> row(padded_row_bytes);
  for (uint32_t y = 0; succeeded && y < image.height; ++y) {
    const uint8_t* source = image.data.data() + size_t(image.height - 1 - y) * image.stride;
    for (uint32_t x = 0; x < image.width; ++x) {
      row[x * 3] = source[x * 4 + 2];
      row[x * 3 + 1] = source[x * 4 + 1];
      row[x * 3 + 2] = source[x * 4];
    }
    succeeded = std::fwrite(row.data(), 1, row.size(), file) == row.size();
  }
  succeeded = std::fclose(file) == 0 && succeeded;
  if (!succeeded) {
    std::error_code remove_error;
    std::filesystem::remove(path, remove_error);
  }
  return succeeded;
}

std::unique_ptr<rex::graphics::GraphicsSystem> CreateEmbeddedGraphicsSystem(
    std::string_view backend) {
#if REX_HAS_D3D12
  if (backend == "any" || backend == "d3d12") {
    return std::make_unique<rex::graphics::d3d12::D3D12GraphicsSystem>();
  }
#endif
#if REX_HAS_VULKAN
  if (backend == "any" || backend == "vulkan") {
    return std::make_unique<rex::graphics::vulkan::VulkanGraphicsSystem>();
  }
#endif
  return nullptr;
}

// Veto the window close and let the runtime stop the title (it destroys the
// window during its shutdown). Before V380 the window closed and the title
// kept running without one, holding the single-instance lock.
class EmbeddedCloseRequestListener final : public rex::ui::WindowListener {
 public:
  explicit EmbeddedCloseRequestListener(EmbeddedGpu& embedded) : embedded_(embedded) {}
  bool OnCloseRequested(rex::ui::UIEvent&) override {
    if (!embedded_.close_requested.exchange(true, std::memory_order_acq_rel)) {
      std::fprintf(stderr, "REX_WINDOW_CLOSE_REQUESTED\n");
      std::fflush(stderr);
      std::thread([] {
        std::this_thread::sleep_for(std::chrono::seconds(10));
        std::fprintf(stderr, "REX_WINDOW_CLOSE_WATCHDOG hard_exit=1\n");
        std::fflush(stderr);
        std::_Exit(0);
      }).detach();
    }
    return false;
  }

 private:
  EmbeddedGpu& embedded_;
};

#if REX_HAS_D3D12
// 0.9.1 (#5): while the D3D12 shader prewarm creates pipelines (when an
// area's shaders first appear in memory, mostly while it loads), a small
// progress bar in the top-left corner (rex::ui::ShaderPrewarmIndicatorDialog).
// A poller posts show/hide to the UI thread; while the prewarm is idle the
// drawer holds no dialog (no cost).
class EmbeddedShaderPrewarmIndicator {
 public:
  EmbeddedShaderPrewarmIndicator(rex::ui::SDLWindowedAppContext& context, rex::ui::Window* window,
                                 rex::graphics::GraphicsSystem* graphics)
      : context_(context), window_(window), graphics_(graphics) {
    poller_ = std::thread([this]() { Poll(); });
  }
  ~EmbeddedShaderPrewarmIndicator() { Shutdown(); }

  // UI thread, before the window goes.
  void Shutdown() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      stop_ = true;
    }
    condition_.notify_all();
    if (poller_.joinable()) {
      poller_.join();
      context_.ExecutePendingFunctionsFromUIThread();
    }
    dialog_.reset();
    if (imgui_drawer_) {
      imgui_drawer_->SetPresenterAndImmediateDrawer(nullptr, nullptr);
      imgui_drawer_.reset();
    }
    if (immediate_drawer_) {
      immediate_drawer_->SetPresenter(nullptr);
      immediate_drawer_.reset();
    }
  }

 private:
  void Poll() {
    bool shown = false;
    auto last_pending = std::chrono::steady_clock::now();
    std::unique_lock<std::mutex> lock(mutex_);
    while (!condition_.wait_for(lock, std::chrono::milliseconds(250), [this]() { return stop_; })) {
      const auto status = rex::graphics::d3d12::GetShaderPrewarmStatus();
      const auto now = std::chrono::steady_clock::now();
      if (status.handled < status.queued) last_pending = now;
      // Shown while pipelines are pending and one second after.
      const bool show = now - last_pending < std::chrono::seconds(1) && status.queued;
      if (show != shown) {
        shown = show;
        context_.CallInUIThreadDeferred([this, show]() { Show(show); });
      }
    }
  }
  void Show(bool show) {
    if (!show) {
      dialog_.reset();
      return;
    }
    if (dialog_ || !EnsureDrawer()) return;
    dialog_ = std::make_unique<rex::ui::ShaderPrewarmIndicatorDialog>(
        imgui_drawer_.get(), []() {
          const auto status = rex::graphics::d3d12::GetShaderPrewarmStatus();
          return rex::ui::ShaderPrewarmProgress{status.queued, status.handled};
        });
  }
  bool EnsureDrawer() {
    if (imgui_drawer_) return true;
    if (!window_ || !graphics_ || !graphics_->provider() || !graphics_->presenter()) return false;
    immediate_drawer_ = graphics_->provider()->CreateImmediateDrawer();
    if (!immediate_drawer_) return false;
    immediate_drawer_->SetPresenter(graphics_->presenter());
    imgui_drawer_ = std::make_unique<rex::ui::ImGuiDrawer>(window_, 48);
    imgui_drawer_->SetPresenterAndImmediateDrawer(graphics_->presenter(), immediate_drawer_.get());
    return true;
  }

  rex::ui::SDLWindowedAppContext& context_;
  rex::ui::Window* window_ = nullptr;
  rex::graphics::GraphicsSystem* graphics_ = nullptr;
  std::unique_ptr<rex::ui::ImmediateDrawer> immediate_drawer_;
  std::unique_ptr<rex::ui::ImGuiDrawer> imgui_drawer_;
  std::unique_ptr<rex::ui::ShaderPrewarmIndicatorDialog> dialog_;
  std::thread poller_;
  std::mutex mutex_;
  std::condition_variable condition_;
  bool stop_ = false;
};
#endif

// Opens and closes the settings overlay from a controller: Back + Start
// together (View + Menu on an Xbox pad, Create + Options on a DualSense
// through Steam Input). Polls XInput itself (all slots, ~120 Hz), so it works
// whatever the title does with the pad (the runtime hides the chord from the
// title, runtime_overlay_chord.h), and publishes the pad for the overlay's
// navigation. From djanice1980's fork (#7); the developer test script's
// virtual controller (`pad`) counts like a physical one.
class EmbeddedGamepadOverlayPoller {
 public:
  EmbeddedGamepadOverlayPoller(EmbeddedGpu& embedded, rex::ui::SDLWindowedAppContext& app_context,
                               EmbeddedSettingsOverlayListener& listener)
      : embedded_(embedded), app_context_(app_context), listener_(listener) {
#if defined(_WIN32)
    if (const HMODULE xinput = LoadLibraryW(L"xinput1_4.dll")) {
      get_state_ = reinterpret_cast<GetStateFn>(GetProcAddress(xinput, "XInputGetState"));
    }
#endif
    thread_ = std::thread([this] { Run(); });
  }
  ~EmbeddedGamepadOverlayPoller() { Stop(); }

  // Before the UI thread's listeners go away: nothing is posted afterwards,
  // and a toggle already posted does nothing.
  void Stop() {
    accepting_->store(false, std::memory_order_release);
    stop_.store(true, std::memory_order_release);
    if (thread_.joinable()) thread_.join();
  }

 private:
  struct PadState {
    uint32_t packet;
    uint16_t buttons;
    uint8_t left_trigger, right_trigger;
    int16_t lx, ly, rx, ry;
  };
  using GetStateFn = unsigned long(__stdcall*)(unsigned long, PadState*);
  static constexpr uint16_t kBack = 0x0020, kStart = 0x0010;

  void Run() {
    uint16_t previous = 0;
    unsigned idle_polls[4] = {};
    while (!stop_.load(std::memory_order_acquire)) {
      uint16_t buttons = 0;
      int16_t lx = 0, ly = 0;
      for (unsigned slot = 0; get_state_ && slot < 4; ++slot) {
        // An empty slot is re-probed about once a second (XInput is slow to
        // report missing controllers).
        if (idle_polls[slot] && ++idle_polls[slot] < 120) continue;
        PadState state{};
        if (get_state_(slot, &state) != 0) {
          idle_polls[slot] = 1;
          continue;
        }
        idle_polls[slot] = 0;
        buttons |= state.buttons;
        if (std::abs(int(state.lx)) > std::abs(int(lx))) lx = state.lx;
        if (std::abs(int(state.ly)) > std::abs(int(ly))) ly = state.ly;
      }
      if (embedded_.keyboard_mouse) buttons |= embedded_.keyboard_mouse->TestPadButtons();
      embedded_.overlay_pad_buttons.store(buttons, std::memory_order_release);
      embedded_.overlay_pad_stick.store((uint32_t(uint16_t(lx)) << 16) | uint16_t(ly),
                                        std::memory_order_release);
      const bool chord = (buttons & (kBack | kStart)) == (kBack | kStart);
      const bool was_chord = (previous & (kBack | kStart)) == (kBack | kStart);
      if (chord && !was_chord) {
        std::fprintf(stderr, "REX_SETTINGS_OVERLAY toggle=gamepad\n");
        std::fflush(stderr);
        app_context_.CallInUIThreadDeferred(
            [accepting = accepting_, listener = &listener_]() {
              if (accepting->load(std::memory_order_acquire)) listener->Toggle();
            });
      }
      previous = buttons;
      std::this_thread::sleep_for(std::chrono::milliseconds(8));
    }
  }

  EmbeddedGpu& embedded_;
  rex::ui::SDLWindowedAppContext& app_context_;
  EmbeddedSettingsOverlayListener& listener_;
  GetStateFn get_state_ = nullptr;
  std::shared_ptr<std::atomic<bool>> accepting_ = std::make_shared<std::atomic<bool>>(true);
  std::atomic<bool> stop_{false};
  std::thread thread_;
};

bool StartEmbeddedPresentation(EmbeddedGpu& embedded, const char* backend) {
  embedded.presentation_thread = std::thread([&embedded, backend]() {
    rex::ui::SDLWindowedAppContext app_context;
    std::unique_ptr<rex::ui::Window> window;
    bool screenshot_listener_attached = false;
    std::unique_ptr<EmbeddedDebugOverlayListener> debug_overlay_listener;
    std::unique_ptr<EmbeddedDevFrameModeListener> dev_frame_mode_listener;
    std::unique_ptr<EmbeddedSettingsOverlayListener> settings_overlay_listener;
    std::unique_ptr<EmbeddedGamepadOverlayPoller> gamepad_overlay_poller;
    std::unique_ptr<EmbeddedOverlayInputBlocker> overlay_input_blocker;
#if REX_HAS_D3D12
    std::unique_ptr<EmbeddedShaderPrewarmIndicator> prewarm_indicator;
#endif
    EmbeddedCloseRequestListener close_request_listener(embedded);
    rex::X_STATUS status = static_cast<rex::X_STATUS>(0xC0000001L);

    bool initialized = app_context.Initialize();
    if (initialized) {
      status = embedded.graphics->SetupPresentation(&app_context);
      initialized = XSUCCEEDED(status);
    }
    if (initialized) {
      window = rex::ui::Window::Create(app_context, "The Darkness Recompiled", 1280, 720);
      // The embedded title path owns the actual game window. Apply the same
      // typed startup policy as ReXApp before Open so WindowSDL can choose
      // decorated-window or borderless-desktop creation deterministically.
      // Previously only the detached ReXApp path consumed this CVar, making
      // packaged `fullscreen = true` ineffective for The Darkness.
      if (window && rex::ui::window_mode_policy::ShouldStartBorderless(
                        REXCVAR_GET(window_mode), REXCVAR_GET(fullscreen))) {
        window->SetFullscreen(true);
      }
      initialized = window && window->Open() &&
                    window->phase() == rex::ui::Window::Phase::kOpen;
    }
    if (initialized) {
      if (embedded.keyboard_mouse) {
        embedded.keyboard_mouse->OnWindowAvailable(window.get());
      }
      if (!embedded.screenshot_root.empty()) {
        window->AddInputListener(
            &embedded.screenshot_listener,
            std::numeric_limits<size_t>::max());
        screenshot_listener_attached = true;
      }
      debug_overlay_listener =
          std::make_unique<EmbeddedDebugOverlayListener>(window.get(),
                                                         embedded.graphics.get());
      window->AddInputListener(debug_overlay_listener.get(),
                               std::numeric_limits<size_t>::max() - 1);
      if (REXCVAR_GET(dev_frame_mode_hotkeys)) {
        dev_frame_mode_listener = std::make_unique<EmbeddedDevFrameModeListener>(embedded);
        window->AddInputListener(dev_frame_mode_listener.get(),
                                 std::numeric_limits<size_t>::max() - 2);
      }
      // Settings overlay toggle above the title bridge; the blocker sits
      // between the overlay's ImGui drawer (z 64) and the MnK driver (z 0).
      settings_overlay_listener = std::make_unique<EmbeddedSettingsOverlayListener>(
          embedded, window.get(), embedded.graphics.get());
      window->AddInputListener(settings_overlay_listener.get(),
                               std::numeric_limits<size_t>::max() - 3);
      gamepad_overlay_poller = std::make_unique<EmbeddedGamepadOverlayPoller>(
          embedded, app_context, *settings_overlay_listener);
      overlay_input_blocker = std::make_unique<EmbeddedOverlayInputBlocker>(embedded);
      window->AddInputListener(overlay_input_blocker.get(), 32);
#if REX_HAS_D3D12
      prewarm_indicator = std::make_unique<EmbeddedShaderPrewarmIndicator>(
          app_context, window.get(), embedded.graphics.get());
#endif
      window->AddListener(&close_request_listener);
      if (embedded.keyboard_mouse) {
        auto* sdl_window = static_cast<rex::ui::WindowSDL*>(window.get());
        embedded.keyboard_mouse->set_host_event_callback(
            [&app_context, sdl_window](
                const rex::input::mnk::test_script_policy::Command& command) {
              app_context.CallInUIThreadDeferred(
                  [sdl_window, command]() { DeliverHostTestEvent(*sdl_window, command); });
            });
      }
      window->SetPresenter(embedded.graphics->presenter());
      initialized = window->GetNativeWindowHandle() != nullptr;
      embedded.native_window.store(window->GetNativeWindowHandle(), std::memory_order_release);
    }

    {
      std::lock_guard<std::mutex> lock(embedded.presentation_mutex);
      embedded.presentation_context = initialized ? &app_context : nullptr;
      embedded.presentation_succeeded = initialized;
      embedded.presentation_initialized = true;
    }
    embedded.presentation_condition.notify_one();

    if (initialized) {
      std::fprintf(stderr,
                   "REX_EMBEDDED_PRESENTATION_READY backend=%s mode=%s size=%ux%u "
                   "requested=%ux%u\n",
                   backend ? backend : "any",
                   window->IsFullscreen() ? "borderless" : "windowed",
                   window->GetActualPhysicalWidth(), window->GetActualPhysicalHeight(),
                   window->GetDesiredLogicalWidth(), window->GetDesiredLogicalHeight());
      std::fflush(stderr);
      app_context.RunMainMessageLoop();
    } else {
      std::fprintf(stderr,
                   "REX_EMBEDDED_PRESENTATION_FAILURE status=0x%08X mode=windowed\n",
                   uint32_t(status));
      std::fflush(stderr);
    }

    embedded.native_window.store(nullptr, std::memory_order_release);
#if REX_HAS_D3D12
    if (prewarm_indicator) {
      prewarm_indicator->Shutdown();
      prewarm_indicator.reset();
    }
#endif
    if (gamepad_overlay_poller) {
      gamepad_overlay_poller->Stop();
      gamepad_overlay_poller.reset();
    }
    if (window) {
      if (dev_frame_mode_listener) {
        window->RemoveInputListener(dev_frame_mode_listener.get());
        dev_frame_mode_listener.reset();
      }
      if (embedded.keyboard_mouse) {
        embedded.keyboard_mouse->set_host_event_callback(nullptr);
        app_context.ExecutePendingFunctionsFromUIThread();
      }
      if (overlay_input_blocker) {
        window->RemoveInputListener(overlay_input_blocker.get());
        overlay_input_blocker.reset();
      }
      if (settings_overlay_listener) {
        window->RemoveInputListener(settings_overlay_listener.get());
        settings_overlay_listener->Reset();
        settings_overlay_listener.reset();
      }
      embedded.settings_overlay_open.store(false, std::memory_order_release);
      if (debug_overlay_listener) {
        window->RemoveInputListener(debug_overlay_listener.get());
        debug_overlay_listener->Reset();
        debug_overlay_listener.reset();
      }
      if (screenshot_listener_attached) {
        window->RemoveInputListener(&embedded.screenshot_listener);
      }
      window->SetPresenter(nullptr);
      window->RemoveListener(&close_request_listener);
      if (window->phase() == rex::ui::Window::Phase::kOpen) {
        window->RequestClose();
      }
      window.reset();
    }
    // This thread's app context is about to go (see ReleaseAppContext).
    if (embedded.graphics) embedded.graphics->ReleaseAppContext();
    {
      std::lock_guard<std::mutex> lock(embedded.presentation_mutex);
      embedded.presentation_context = nullptr;
    }
  });

  std::unique_lock<std::mutex> lock(embedded.presentation_mutex);
  embedded.presentation_condition.wait(
      lock, [&embedded]() { return embedded.presentation_initialized; });
  return embedded.presentation_succeeded;
}

void StopEmbeddedPresentation(EmbeddedGpu& embedded) {
  {
    std::lock_guard<std::mutex> lock(embedded.presentation_mutex);
    if (embedded.presentation_context) {
      embedded.presentation_context->RequestDeferredQuit();
    }
  }
  if (embedded.presentation_thread.joinable()) {
    embedded.presentation_thread.join();
  }
}

std::filesystem::path BuildEmbeddedScreenshotPath(
    const std::filesystem::path& root, uint64_t ordinal) {
  const auto now = std::chrono::system_clock::now();
  const std::time_t seconds = std::chrono::system_clock::to_time_t(now);
  std::tm utc{};
#if defined(_WIN32)
  gmtime_s(&utc, &seconds);
#else
  gmtime_r(&seconds, &utc);
#endif
  const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(
                                now.time_since_epoch()) %
                            1000;
  char filename[96];
  std::snprintf(filename, sizeof(filename),
                "TheDarkness_%04d%02d%02d_%02d%02d%02d_%03lld_%06llu.bmp",
                utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday,
                utc.tm_hour, utc.tm_min, utc.tm_sec,
                static_cast<long long>(milliseconds.count()),
                static_cast<unsigned long long>(ordinal));
  return root / filename;
}

void StartEmbeddedScreenshotService(EmbeddedGpu& embedded) {
  if (embedded.screenshot_root.empty()) {
    return;
  }
  {
    std::lock_guard<std::mutex> lock(embedded.screenshot_mutex);
    embedded.screenshot_pending = 0;
    embedded.screenshot_stop = false;
  }
  embedded.screenshot_thread = std::thread([&embedded]() {
    while (true) {
      uint64_t ordinal = 0;
      {
        std::unique_lock<std::mutex> lock(embedded.screenshot_mutex);
        embedded.screenshot_condition.wait(lock, [&embedded]() {
          return embedded.screenshot_stop || embedded.screenshot_pending;
        });
        if (embedded.screenshot_stop) {
          break;
        }
        --embedded.screenshot_pending;
        ordinal = ++embedded.screenshot_ordinal;
      }

      std::error_code directory_error;
      std::filesystem::create_directories(embedded.screenshot_root,
                                          directory_error);
      if (directory_error ||
          !std::filesystem::is_directory(embedded.screenshot_root)) {
        std::fprintf(stderr,
                     "REX_EMBEDDED_SCREENSHOT result=0 reason=directory path=%s\n",
                     rex::path_to_utf8(embedded.screenshot_root).c_str());
        std::fflush(stderr);
        continue;
      }

      rex::ui::RawImage image;
      rex::ui::Presenter* presenter = embedded.graphics->presenter();
      if (!presenter || !presenter->CaptureGuestOutput(image)) {
        std::fprintf(stderr,
                     "REX_EMBEDDED_SCREENSHOT result=0 reason=readback\n");
        std::fflush(stderr);
        continue;
      }

      const std::filesystem::path capture_path =
          BuildEmbeddedScreenshotPath(embedded.screenshot_root, ordinal);
      const bool wrote_capture = WriteEmbeddedOutputBmp(image, capture_path);
      std::fprintf(stderr,
                   "REX_EMBEDDED_SCREENSHOT result=%u size=%ux%u path=%s\n",
                   wrote_capture ? 1u : 0u, image.width, image.height,
                   rex::path_to_utf8(capture_path).c_str());
      std::fflush(stderr);
    }
  });
}

void StopEmbeddedScreenshotService(EmbeddedGpu& embedded) {
  {
    std::lock_guard<std::mutex> lock(embedded.screenshot_mutex);
    embedded.screenshot_stop = true;
    embedded.screenshot_pending = 0;
  }
  embedded.screenshot_condition.notify_all();
  if (embedded.screenshot_thread.joinable()) {
    embedded.screenshot_thread.join();
  }
}

void StartEmbeddedOutputProbe(EmbeddedGpu& embedded) {
  embedded.output_probe_stop.store(false, std::memory_order_release);
  embedded.output_probe_thread = std::thread([&embedded]() {
    // A full title boot can take longer than ten minutes while detailed runtime
    // tracing is enabled. Keep the readback probe alive long enough to cover
    // the first interactive state instead of losing output evidence immediately
    // before the menu-era command stream begins.
    constexpr uint32_t kMaximumCaptures = 3600;
    constexpr uint32_t kMaximumColoredCaptureFiles = 64;
    // One sample per second keeps the probe bounded to sixty minutes and avoids
    // making synchronous GPU readback a material part of title/input timing.
    // Unique colored frames are still retained across the title-to-menu
    // transition, while unchanged frames produce only sparse diagnostics.
    constexpr uint32_t kCaptureIntervalTenths = 10;
    uint32_t capture_ordinal = 0;
    uint32_t captured_colored_hashes[kMaximumColoredCaptureFiles]{};
    uint32_t captured_colored_hash_count = 0;
    uint32_t previous_output_hash = 0;
    bool has_previous_output_hash = false;
    while (!embedded.output_probe_stop.load(std::memory_order_acquire) &&
           capture_ordinal < kMaximumCaptures) {
      rex::ui::RawImage image;
      rex::ui::Presenter* presenter = embedded.graphics->presenter();
      if (presenter && presenter->CaptureGuestOutput(image) && image.width && image.height &&
          image.data.size() >= size_t(image.width) * image.height * sizeof(uint32_t)) {
        uint64_t colored_pixels = 0;
        uint32_t first_colored_pixel = UINT32_MAX;
        uint32_t hash = 2166136261u;
        const auto* pixels = reinterpret_cast<const uint32_t*>(image.data.data());
        const size_t pixel_count = size_t(image.width) * image.height;
        for (size_t i = 0; i < pixel_count; ++i) {
          const uint32_t pixel = pixels[i];
          hash = (hash ^ pixel) * 16777619u;
          if (pixel & 0x00FFFFFFu) {
            if (!colored_pixels) {
              first_colored_pixel = uint32_t(i);
            }
            ++colored_pixels;
          }
        }
        ++capture_ordinal;
        const bool output_changed = !has_previous_output_hash || hash != previous_output_hash;
        const bool trace_capture =
            capture_ordinal <= 8 || output_changed || !(capture_ordinal & 127);
        previous_output_hash = hash;
        has_previous_output_hash = true;
        if (trace_capture) {
          std::fprintf(stderr,
                       "REX_EMBEDDED_OUTPUT_CAPTURE ordinal=%u size=%ux%u colored_pixels=%llu "
                       "first_colored=0x%08X fnv1a=0x%08X changed=%u\n",
                       capture_ordinal, image.width, image.height,
                       static_cast<unsigned long long>(colored_pixels), first_colored_pixel, hash,
                       output_changed ? 1u : 0u);
        }
        bool colored_hash_seen = false;
        for (uint32_t i = 0; i < captured_colored_hash_count; ++i) {
          if (captured_colored_hashes[i] == hash) {
            colored_hash_seen = true;
            break;
          }
        }
        // Preserve stable guest states (prompt/menu/error screens) instead of
        // spending the bounded evidence budget on every unique movie frame.
        const bool stable_colored_output = colored_pixels && !output_changed;
        if (stable_colored_output && !colored_hash_seen &&
            captured_colored_hash_count < kMaximumColoredCaptureFiles) {
          const long long timestamp = static_cast<long long>(
              std::chrono::system_clock::now().time_since_epoch().count());
          char capture_path[160];
          std::snprintf(capture_path, sizeof(capture_path),
                        "rex_guest_output_colored_%u_%08X_%lld.bmp", capture_ordinal, hash,
                        timestamp);
          const bool wrote_capture = WriteEmbeddedOutputBmp(image, capture_path);
          std::fprintf(stderr, "REX_EMBEDDED_OUTPUT_CAPTURE_FILE result=%u path=%s\n",
                       wrote_capture ? 1u : 0u, capture_path);
          if (wrote_capture) {
            captured_colored_hashes[captured_colored_hash_count++] = hash;
          }
        }
        if (trace_capture || (stable_colored_output && !colored_hash_seen)) {
          std::fflush(stderr);
        }
        for (uint32_t tenth = 0;
             tenth < kCaptureIntervalTenths &&
             !embedded.output_probe_stop.load(std::memory_order_acquire);
             ++tenth) {
          std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        continue;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
  });
}

void StopEmbeddedOutputProbe(EmbeddedGpu& embedded) {
  embedded.output_probe_stop.store(true, std::memory_order_release);
  if (embedded.output_probe_thread.joinable()) {
    embedded.output_probe_thread.join();
  }
}

// Host writes of CPU-side data made inside the plugin (XMA decoder output and
// contexts, command-processor register, fence, query and read-pointer writes)
// never pass through the host's guest-store tracker. Once written, their pages
// are marked for the command processor, which invalidates the GPU copy before
// the pages are next requested (and every frame), exactly as for a published
// guest store, so that copy stays coherent without the frame-end page reset
// (clear_memory_page_state). GPU results mirrored into guest memory use
// GuardGpuMirrorWrite and only get the host's payload guard.
struct EmbeddedHostWrite {
  void* runtime_token;
  uint32_t physical;
  uint32_t bytes;
};
struct EmbeddedHostWriteStack {
  std::array<EmbeddedHostWrite, 64> slots;
  size_t depth = 0;
};
EmbeddedHostWriteStack& CurrentEmbeddedHostWrites() {
  thread_local EmbeddedHostWriteStack stack;
  return stack;
}

#if REX_GPU_DIAGNOSTICS
// Measurement builds: page invalidation sources (REX_INVALIDATION_SOURCES).
struct DiagInvalidationSources {
  std::atomic<uint64_t> host_small{0};  // <= 8 bytes: registers, fences.
  std::atomic<uint64_t> host_large{0};
  std::atomic<uint64_t> host_bytes{0};
  std::atomic<uint64_t> drain_ranges{0};
  std::atomic<uint64_t> drain_bytes{0};
  uint64_t frames = 0;
};
DiagInvalidationSources& DiagInvalidations() {
  static DiagInvalidationSources sources;
  return sources;
}
#endif

void* EmbeddedHostWriteBegin(void* context, uint32_t physical, uint32_t bytes) noexcept {
  auto& embedded = *static_cast<EmbeddedGpu*>(context);
  EmbeddedHostWriteStack& stack = CurrentEmbeddedHostWrites();
  if (stack.depth == stack.slots.size()) std::terminate();
  EmbeddedHostWrite& slot = stack.slots[stack.depth++];
  const rex::memory::HostWriteCallbacks& runtime = embedded.runtime_host_writes;
  slot.runtime_token = runtime.begin ? runtime.begin(runtime.context, physical, bytes) : nullptr;
  slot.physical = physical;
  slot.bytes = bytes;
  return &slot;
}

void EmbeddedHostWriteEnd(void* context, void* token) noexcept {
  auto& embedded = *static_cast<EmbeddedGpu*>(context);
  EmbeddedHostWriteStack& stack = CurrentEmbeddedHostWrites();
  if (!stack.depth || token != &stack.slots[stack.depth - 1]) std::terminate();
  const EmbeddedHostWrite slot = stack.slots[--stack.depth];
  const rex::memory::HostWriteCallbacks& runtime = embedded.runtime_host_writes;
  if (runtime.end) runtime.end(runtime.context, slot.runtime_token);
  if (rex::graphics::GraphicsSystem* graphics =
          embedded.host_write_invalidation.load(std::memory_order_acquire)) {
    graphics->NotifyHostWrite(slot.physical, slot.bytes);
#if REX_GPU_DIAGNOSTICS
    auto& sources = DiagInvalidations();
    (slot.bytes <= 8 ? sources.host_small : sources.host_large)
        .fetch_add(1, std::memory_order_relaxed);
    sources.host_bytes.fetch_add(slot.bytes, std::memory_order_relaxed);
#endif
  }
}
}  // namespace

extern "C" REX_GPU_PLUGIN_EXPORT void* rex_gpu_embedded_create(
    uint32_t abi_version, const rex::system::EmbeddedGpuCreateInfo* info) {
  if (abi_version != rex::system::kEmbeddedGpuAbiVersion || !info ||
      info->struct_size < sizeof(rex::system::EmbeddedGpuCreateInfo) ||
      !info->virtual_membase || !info->physical_membase ||
      !info->asset_root_utf8 || !info->asset_root_utf8[0] ||
      !info->config_path_utf8 || !info->config_path_utf8[0] ||
      info->config_present > 1 ||
      (!info->config_contents_utf8 && info->config_contents_size) ||
      (!info->config_present && info->config_contents_size)) {
    REXLOG_ERROR("rexgpu-xenos: invalid embedded GPU create request");
    return nullptr;
  }

#if REX_GPU_DIAGNOSTICS && defined(_WIN32)
  // Measurement builds: REX_PIX_GPU_CAPTURER=1 loads PIX's GPU capturer from
  // the newest installed PIX before any Direct3D 12 device exists, so
  // `pixtool attach <pid> take-capture` can capture frames
  // (scripts/pix-capture.ps1; PIX install: docs/TOOLS_INSTALLED.md).
  if (const char* pix = std::getenv("REX_PIX_GPU_CAPTURER"); pix && pix[0] == '1') {
    std::filesystem::path newest;
    std::error_code error;
    for (const auto& entry :
         std::filesystem::directory_iterator(L"C:\\Program Files\\Microsoft PIX", error)) {
      if (entry.is_directory(error) && std::filesystem::exists(
              entry.path() / L"WinPixGpuCapturer.dll", error) &&
          (newest.empty() || entry.path().filename() > newest.filename())) {
        newest = entry.path();
      }
    }
    HMODULE capturer = newest.empty()
                           ? nullptr
                           : LoadLibraryW((newest / L"WinPixGpuCapturer.dll").c_str());
    std::fprintf(stderr, "REX_PIX_GPU_CAPTURER loaded=%u path=%s\n", capturer ? 1u : 0u,
                 newest.empty() ? "(no PIX install)" : newest.u8string().c_str());
    std::fflush(stderr);
    // pixtool only attaches to processes PIX launched itself, so captures are
    // programmatic: REX_PIX_CAPTURE_REQUEST=<file>; when that file appears
    // (its text = the .wpix path to write), the next frame is captured by
    // the capturer's CaptureNextFrame (pix3's PIXGpuCaptureNextFrames) and
    // the request file is removed (scripts/pix-capture.ps1).
    using CaptureNextFrameFn = HRESULT(WINAPI*)(PCWSTR, UINT32);
    const auto capture_next_frame = capturer ? reinterpret_cast<CaptureNextFrameFn>(
                                                   GetProcAddress(capturer, "CaptureNextFrame"))
                                             : nullptr;
    const char* request = std::getenv("REX_PIX_CAPTURE_REQUEST");
    if (capture_next_frame && request && *request) {
      std::thread([capture_next_frame, request_path = std::filesystem::u8path(request)]() {
        for (;;) {
          std::this_thread::sleep_for(std::chrono::milliseconds(200));
          std::error_code error;
          if (!std::filesystem::exists(request_path, error)) continue;
          std::ifstream file(request_path, std::ios::binary);
          std::string target((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
          file.close();
          while (!target.empty() && (target.back() == '\n' || target.back() == '\r' ||
                                     target.back() == ' ')) {
            target.pop_back();
          }
          std::filesystem::remove(request_path, error);
          if (target.empty()) continue;
          const HRESULT result =
              capture_next_frame(std::filesystem::u8path(target).wstring().c_str(), 1);
          std::fprintf(stderr, "REX_PIX_CAPTURE requested=%s result=0x%08X\n", target.c_str(),
                       static_cast<unsigned>(result));
          std::fflush(stderr);
        }
      }).detach();
    }
  }
  // Measurement builds: REX_RENDERDOC_DLL=<renderdoc.dll> loads RenderDoc's
  // in-application API before any Direct3D 12 device exists (its captures
  // replay without Windows Developer Mode, unlike PIX's). Captures are
  // requested like PIX's: REX_RENDERDOC_CAPTURE_REQUEST=<file>, whose text is
  // the capture path template; RenderDoc's own overlay is hidden so window
  // snapshots stay clean (docs/TOOLS_INSTALLED.md).
  if (const char* renderdoc_dll = std::getenv("REX_RENDERDOC_DLL"); renderdoc_dll && *renderdoc_dll) {
    HMODULE renderdoc = LoadLibraryW(std::filesystem::u8path(renderdoc_dll).wstring().c_str());
    using GetApiFn = int(__cdecl*)(int version, void** api);
    const auto get_api = renderdoc ? reinterpret_cast<GetApiFn>(
                                         GetProcAddress(renderdoc, "RENDERDOC_GetAPI"))
                                   : nullptr;
    // RENDERDOC_API_1_1_2 function table, by slot (renderdoc_app.h).
    void** api = nullptr;
    const bool api_ok = get_api && get_api(10102, reinterpret_cast<void**>(&api)) == 1 && api;
    std::fprintf(stderr, "REX_RENDERDOC loaded=%u api=%u path=%s\n", renderdoc ? 1u : 0u,
                 api_ok ? 1u : 0u, renderdoc_dll);
    std::fflush(stderr);
    const char* request = std::getenv("REX_RENDERDOC_CAPTURE_REQUEST");
    if (api_ok) {
      using MaskOverlayBitsFn = void(__cdecl*)(uint32_t, uint32_t);
      reinterpret_cast<MaskOverlayBitsFn>(api[8])(0, 0);
      // Whole guest frames: the command processor starts and ends captures
      // at guest swaps (RenderDoc's own boundary is a host present).
      rex::graphics::d3d12::SetRenderDocApi(api);
    }
    if (api_ok && request && *request) {
      std::thread([api, request_path = std::filesystem::u8path(request)]() {
        using SetTemplateFn = void(__cdecl*)(const char*);
        using GetNumCapturesFn = uint32_t(__cdecl*)();
        using GetCaptureFn = uint32_t(__cdecl*)(uint32_t, char*, uint32_t*, uint64_t*);
        uint32_t reported = 0;
        for (;;) {
          std::this_thread::sleep_for(std::chrono::milliseconds(200));
          const uint32_t captures = reinterpret_cast<GetNumCapturesFn>(api[13])();
          for (; reported < captures; ++reported) {
            uint32_t length = 0;
            reinterpret_cast<GetCaptureFn>(api[14])(reported, nullptr, &length, nullptr);
            std::string name(length, '\0');
            reinterpret_cast<GetCaptureFn>(api[14])(reported, name.data(), &length, nullptr);
            while (!name.empty() && name.back() == '\0') name.pop_back();
            std::fprintf(stderr, "REX_RENDERDOC_CAPTURE written=%s\n", name.c_str());
            std::fflush(stderr);
          }
          std::error_code error;
          if (!std::filesystem::exists(request_path, error)) continue;
          std::ifstream file(request_path, std::ios::binary);
          std::string target((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
          file.close();
          while (!target.empty() && (target.back() == '\n' || target.back() == '\r' ||
                                     target.back() == ' ')) {
            target.pop_back();
          }
          std::filesystem::remove(request_path, error);
          // "<path template>[|<guest frames>]", 2 frames by default.
          uint32_t frames = 2;
          if (const size_t bar = target.rfind('|'); bar != std::string::npos) {
            frames = std::clamp<uint32_t>(uint32_t(std::strtoul(target.c_str() + bar + 1, nullptr, 10)), 1, 8);
            target.resize(bar);
          }
          if (target.empty()) continue;
          reinterpret_cast<SetTemplateFn>(api[11])(target.c_str());
          rex::graphics::d3d12::RequestRenderDocGuestFrames(frames);
          std::fprintf(stderr, "REX_RENDERDOC_CAPTURE requested=%s guest_frames=%u\n", target.c_str(),
                       frames);
          std::fflush(stderr);
        }
      }).detach();
    }
  }
#endif

  // Embedded hosts do not run the normal ReXGlue application bootstrap. Load
  // the host-selected shared PC configuration here, then apply environment
  // overrides so automated validation can still override individual values.
  // All of this happens before any graphics object consumes a setting.
  if (!rex::graphics::LoadEmbeddedPcConfig(
          info->config_present != 0,
          std::string_view(info->config_contents_utf8 ? info->config_contents_utf8 : "",
                           static_cast<size_t>(info->config_contents_size)),
          std::filesystem::u8path(info->config_path_utf8))) {
    REXLOG_ERROR("rexgpu-xenos: invalid owned startup configuration");
    return nullptr;
  }
  std::fprintf(stderr, "REX_PC_CONFIG_SNAPSHOT present=%u bytes=%llu source=%s\n",
               info->config_present,
               static_cast<unsigned long long>(info->config_contents_size),
               info->config_path_utf8);
  rex::graphics::prompt_icons::SetBindingProvider(&PromptBindingValue);

  // One bounded post-override snapshot makes packaged settings provenance
  // observable without enabling per-frame diagnostics. Keep this after the
  // environment layer and before any graphics/input object consumes a value.
  const auto effective_setting = [](const char* name) {
    return rex::cvar::GetFlagByName(name);
  };
  std::fprintf(
      stderr,
      "REX_PC_SETTINGS_EFFECTIVE window_mode=%s output_resolution=%s "
      "window_width=%s window_height=%s monitor=%s resolution_scale=%s "
      "draw_resolution_scale_threshold=%s native_grid_rules=%s "
      "swap_post_effect=%s anisotropic_override=%s motion_blur=%s "
      "letterbox=%s overscan_cutoff=%s safe_area_x=%s safe_area_y=%s "
      "present_mode=%s max_frame_latency=%s frame_limit=%s frame_rate=%s vsync_interval=%s gameplay_fov=%s "
      "keyboard_mouse=%s keyboard_mouse_user=%s mouse_sensitivity=%s "
      "mouse_acceleration=%s mouse_smoothing=%s mouse_invert_y=%s mouse_look=%s\n",
      effective_setting("window_mode").c_str(),
      effective_setting("output_resolution").c_str(),
      effective_setting("window_width").c_str(),
      effective_setting("window_height").c_str(),
      effective_setting("monitor").c_str(),
      effective_setting("resolution_scale").c_str(),
      effective_setting("draw_resolution_scale_threshold").c_str(),
      effective_setting("draw_resolution_scale_native_grid_rules").c_str(),
      effective_setting("swap_post_effect").c_str(),
      effective_setting("anisotropic_override").c_str(),
      effective_setting("graphics_motion_blur").c_str(),
      effective_setting("present_letterbox").c_str(),
      effective_setting("present_allow_overscan_cutoff").c_str(),
      effective_setting("present_safe_area_x").c_str(),
      effective_setting("present_safe_area_y").c_str(),
      effective_setting("display_present_mode").c_str(),
      effective_setting("display_max_frame_latency").c_str(),
      effective_setting("display_frame_limit").c_str(),
      effective_setting("display_frame_rate").c_str(),
      effective_setting("display_vsync_interval").c_str(),
      effective_setting("camera_field_of_view").c_str(),
      effective_setting("input_keyboard_mouse").c_str(),
      effective_setting("input_keyboard_mouse_user_index").c_str(),
      effective_setting("input_mouse_sensitivity").c_str(),
      effective_setting("input_mouse_acceleration").c_str(),
      effective_setting("input_mouse_smoothing").c_str(),
      effective_setting("input_mouse_invert_y").c_str(),
      effective_setting("input_mouse_look").c_str());
  std::fflush(stderr);

  std::string replacement_pack_identity = "original";
  const std::string motion_blur_pack = REXCVAR_GET(graphics_motion_blur_off_shader_pack);
  if (!REXCVAR_GET(graphics_motion_blur) &&
      (motion_blur_pack.empty() || motion_blur_pack == kLegacyMotionBlurPack)) {
    for (const uint64_t hash : kMotionBlurComposites) {
      rex::graphics::ConfigureShaderPatch(rex::graphics::xenos::ShaderType::kPixel, hash,
                                          &PatchMotionBlurOff);
    }
    replacement_pack_identity = kMotionBlurPatchIdentity;
    std::fprintf(stderr, "REX_MOTION_BLUR_PACK result=1 mappings=%zu path=builtin\n",
                 rex::graphics::ConfiguredShaderPatchCount());
    std::fflush(stderr);
  } else if (!REXCVAR_GET(graphics_motion_blur)) {
    const std::filesystem::path replacement_pack =
        ResolveEmbeddedShaderReplacementPackPath(
            std::filesystem::u8path(
                REXCVAR_GET(graphics_motion_blur_off_shader_pack)),
            info->config_path_utf8, info->asset_root_utf8);
    std::string replacement_error;
    if (!rex::graphics::ConfigureShaderReplacementPack(replacement_pack,
                                                        &replacement_error)) {
      REXLOG_ERROR("rexgpu-xenos: motion blur Off pack '{}' is invalid: {}",
                   rex::path_to_utf8(replacement_pack), replacement_error);
      std::fprintf(stderr,
                   "REX_MOTION_BLUR_PACK result=0 path=%s reason=%s\n",
                   rex::path_to_utf8(replacement_pack).c_str(),
                   replacement_error.c_str());
      std::fflush(stderr);
      return nullptr;
    }
    replacement_pack_identity = rex::crypto::sha256_file(replacement_pack);
    if (replacement_pack_identity.empty()) {
      REXLOG_ERROR("rexgpu-xenos: unable to hash motion blur Off pack '{}'",
                   rex::path_to_utf8(replacement_pack));
      std::fprintf(stderr,
                   "REX_MOTION_BLUR_PACK result=0 path=%s reason=hash_failed\n",
                   rex::path_to_utf8(replacement_pack).c_str());
      std::fflush(stderr);
      return nullptr;
    }
    REXLOG_INFO("rexgpu-xenos: motion blur Off uses {} title-native shader mappings from {}",
                rex::graphics::ConfiguredShaderReplacementCount(),
                rex::path_to_utf8(replacement_pack));
    std::fprintf(stderr,
                 "REX_MOTION_BLUR_PACK result=1 mappings=%zu path=%s\n",
                 rex::graphics::ConfiguredShaderReplacementCount(),
                 rex::path_to_utf8(replacement_pack).c_str());
    std::fflush(stderr);
  }
  rex::cvar::FinalizeInit();

  auto embedded = std::make_unique<EmbeddedGpu>();
  if (info->screenshot_root_utf8 && info->screenshot_root_utf8[0]) {
    embedded->screenshot_root =
        std::filesystem::u8path(info->screenshot_root_utf8).lexically_normal();
    if (!embedded->screenshot_root.is_absolute()) {
      REXLOG_ERROR("rexgpu-xenos: embedded screenshot root must be absolute");
      return nullptr;
    }
  }
  embedded->keyboard_mouse =
      std::make_unique<rex::input::mnk::MnkInputDriver>(nullptr, 0);
  // The settings overlay owns keyboard and mouse while it is open (neutral
  // pad state, released capture) - read on the game thread.
  embedded->keyboard_mouse->set_is_active_callback([raw = embedded.get()]() {
    return !raw->settings_overlay_open.load(std::memory_order_acquire);
  });
  if (XFAILED(embedded->keyboard_mouse->Setup())) {
    REXLOG_ERROR("rexgpu-xenos: keyboard/mouse input setup failed");
    return nullptr;
  }
  embedded->runtime_host_writes = info->host_writes;
  if (!embedded->runtime_host_writes.Valid() ||
      !embedded->memory.InitializeExternal(
          info->virtual_membase, info->physical_membase,
          {embedded.get(), EmbeddedHostWriteBegin, EmbeddedHostWriteEnd}, info->host_writes)) {
    REXLOG_ERROR("rexgpu-xenos: unable to adopt host guest memory");
    return nullptr;
  }
  embedded->graphics =
      CreateEmbeddedGraphicsSystem(info->backend ? info->backend : "any");
  if (!embedded->graphics) {
    REXLOG_ERROR("rexgpu-xenos: requested embedded backend is unavailable");
    return nullptr;
  }

  // ReXGlue's presenter and window own their normal UI-thread lifecycle. The
  // embedded host shares the same D3D12 provider with the command processor;
  // no framebuffer copy or parallel renderer is introduced here.
  if (!StartEmbeddedPresentation(*embedded, info->backend)) {
    StopEmbeddedPresentation(*embedded);
    return nullptr;
  }

  const auto interrupt_callback = info->interrupt_callback;
  void* const interrupt_context = info->interrupt_context;
  rex::X_STATUS status = embedded->graphics->SetupEmbeddedGuestGpu(
      &embedded->memory, info->refresh_rate_hz,
      [interrupt_callback, interrupt_context](uint32_t callback, uint32_t source, uint32_t cpu,
                                              uint32_t callback_data) {
        if (interrupt_callback) {
          interrupt_callback(interrupt_context, callback, source, cpu, callback_data);
        }
      });
  if (XFAILED(status)) {
    REXLOG_ERROR("rexgpu-xenos: embedded GPU setup failed with status 0x{:08X}",
                 uint32_t(status));
    StopEmbeddedPresentation(*embedded);
    embedded->graphics->Shutdown();
    return nullptr;
  }
  embedded->graphics->command_processor()->SetOwnedCameraPacketCallbacks(info->camera_packets);
  embedded->host_write_invalidation.store(embedded->graphics.get(), std::memory_order_release);
  if (info->cache_root_utf8 && info->cache_root_utf8[0] && info->title_id) {
    const std::filesystem::path host_cache_root =
        std::filesystem::u8path(info->cache_root_utf8);
    const std::string backend = info->backend ? info->backend : "any";
    std::string effective_identity =
        "ReXGlue-embedded-graphics-cache-identity-v1";
    effective_identity.push_back('\0');
    effective_identity += host_cache_root.filename().u8string();
    effective_identity.push_back('\0');
    effective_identity += backend;
    effective_identity.push_back('\0');
    effective_identity += replacement_pack_identity;
    const std::string effective_leaf =
        rex::crypto::sha256(effective_identity);
    const std::filesystem::path cache_root =
        host_cache_root.parent_path() / effective_leaf;
    embedded->graphics->InitializeShaderStorage(
        cache_root, info->title_id, info->shader_storage_blocking != 0);
    REXLOG_INFO(
        "rexgpu-xenos: persistent shader storage initialized root={} title={:08X} blocking={}",
        rex::path_to_utf8(cache_root), info->title_id,
        info->shader_storage_blocking != 0);
    std::fprintf(
        stderr,
        "REX_GRAPHICS_CACHE_EFFECTIVE root=%s host_leaf=%s backend=%s "
        "replacement=%s replacement_sha256=%s\n",
        rex::path_to_utf8(cache_root).c_str(),
        host_cache_root.filename().u8string().c_str(), backend.c_str(),
        replacement_pack_identity == "original" ? "original" : "shader_pack",
        replacement_pack_identity == "original"
            ? "none"
            : replacement_pack_identity.c_str());
    std::fflush(stderr);
  }
  if (kEmbeddedOutputProbeEnabled) {
    StartEmbeddedOutputProbe(*embedded);
  }
  StartEmbeddedScreenshotService(*embedded);
  return embedded.release();
}

extern "C" REX_GPU_PLUGIN_EXPORT void rex_gpu_embedded_destroy(void* handle) {
  auto embedded = std::unique_ptr<EmbeddedGpu>(static_cast<EmbeddedGpu*>(handle));
  if (embedded) {
    // XMA and command-processor threads may still write; stop invalidating
    // before either shuts down (the XMA join waits for an in-flight call).
    embedded->host_write_invalidation.store(nullptr, std::memory_order_release);
  }
  if (embedded && embedded->xma) {
    embedded->xma->Shutdown();
    embedded->xma.reset();
  }
  if (embedded && embedded->graphics) {
    StopEmbeddedScreenshotService(*embedded);
    StopEmbeddedOutputProbe(*embedded);
    StopEmbeddedPresentation(*embedded);
    embedded->graphics->Shutdown();
  }
}

extern "C" REX_GPU_PLUGIN_EXPORT uint32_t rex_gpu_embedded_read_mmio(void* handle,
                                                                      uint32_t address) {
  auto* embedded = static_cast<EmbeddedGpu*>(handle);
  return embedded->graphics->ReadRegisterEmbedded(address);
}

extern "C" REX_GPU_PLUGIN_EXPORT void rex_gpu_embedded_write_mmio(
    void* handle, uint32_t address, uint32_t value) {
  auto* embedded = static_cast<EmbeddedGpu*>(handle);
  embedded->graphics->WriteRegisterEmbedded(address, value);
}

extern "C" REX_GPU_PLUGIN_EXPORT void rex_gpu_embedded_set_interrupt(
    void* handle, uint32_t callback, uint32_t callback_data) {
  auto* embedded = static_cast<EmbeddedGpu*>(handle);
  embedded->graphics->SetInterruptCallback(callback, callback_data);
}

extern "C" REX_GPU_PLUGIN_EXPORT void rex_gpu_embedded_initialize_ring(
    void* handle, uint32_t physical_address, uint32_t size_log2) {
  auto* embedded = static_cast<EmbeddedGpu*>(handle);
  embedded->graphics->InitializeRingBuffer(physical_address, size_log2);
}

extern "C" REX_GPU_PLUGIN_EXPORT void rex_gpu_embedded_enable_read_pointer_writeback(
    void* handle, uint32_t physical_address, uint32_t block_size_log2) {
  auto* embedded = static_cast<EmbeddedGpu*>(handle);
  embedded->graphics->EnableReadPointerWriteBack(physical_address, block_size_log2);
}

extern "C" REX_GPU_PLUGIN_EXPORT void rex_gpu_embedded_notify_physical_write(
    void* handle, uint32_t physical_address, uint32_t length) {
  auto* embedded = static_cast<EmbeddedGpu*>(handle);
  embedded->graphics->NotifyPhysicalMemoryWrite(physical_address, length);
#if REX_GPU_DIAGNOSTICS
  DiagInvalidations().drain_ranges.fetch_add(1, std::memory_order_relaxed);
  DiagInvalidations().drain_bytes.fetch_add(length, std::memory_order_relaxed);
#endif
}

namespace {
// Uncapped safety: a minimized window shows nothing, so bound production.
constexpr uint32_t kMinimizedFrameLimit = 30;

void WaitGuestFrameDeadline(EmbeddedGpu& embedded, uint32_t frames_per_second) {
#if defined(_WIN32)
  if (!embedded.qpc_frequency) {
    LARGE_INTEGER frequency;
    QueryPerformanceFrequency(&frequency);
    embedded.qpc_frequency = uint64_t(frequency.QuadPart);
    // Spin tail: 50 us .. 1.5 ms, starting at 0.4 ms, adapted to timer lateness.
    embedded.frame_spin = rex::ui::GuestFrameSpinMargin(
        embedded.qpc_frequency / 20000, embedded.qpc_frequency * 15 / 10000,
        embedded.qpc_frequency * 4 / 10000);
    embedded.frame_timer = CreateWaitableTimerExW(
        nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
  }
  const uint64_t frequency = embedded.qpc_frequency;
  LARGE_INTEGER now;
  QueryPerformanceCounter(&now);
  const uint64_t period = frames_per_second ? frequency / frames_per_second : 0;
  const uint64_t deadline = embedded.frame_deadline.Begin(uint64_t(now.QuadPart), period);
  if (deadline <= uint64_t(now.QuadPart)) return;
  ++embedded.frame_waits;
  for (;;) {
    QueryPerformanceCounter(&now);
    const uint64_t tick = uint64_t(now.QuadPart);
    if (tick >= deadline) break;
    const uint64_t remaining = deadline - tick;
    const uint64_t margin = embedded.frame_spin.margin();
    if (remaining > margin) {
      const uint64_t sleep_ticks = remaining - margin;
      if (embedded.frame_timer) {
        LARGE_INTEGER due;
        due.QuadPart = -std::max<int64_t>(int64_t(sleep_ticks * 10000000 / frequency), 1);
        if (SetWaitableTimer(embedded.frame_timer, &due, 0, nullptr, nullptr, FALSE) &&
            WaitForSingleObject(embedded.frame_timer, INFINITE) == WAIT_OBJECT_0) {
          QueryPerformanceCounter(&now);
          const uint64_t expected = tick + sleep_ticks;
          const uint64_t woke = uint64_t(now.QuadPart);
          embedded.frame_spin.Observe(woke > expected ? woke - expected : 0, frequency / 20000);
          continue;
        }
      } else if (sleep_ticks > frequency / 500) {
        Sleep(1);  // no high-resolution timer (pre-1803): coarse sleep, then spin
        continue;
      }
    }
    _mm_pause();
  }
#else
  (void)embedded;
  (void)frames_per_second;
#endif
}
}  // namespace

void ReportGuestFramePacing(EmbeddedGpu& embedded) {
  if (!embedded.pacing_frames || !embedded.qpc_frequency) return;
  const auto percentile = [&](double fraction) -> double {
    const uint64_t need = uint64_t(fraction * double(embedded.pacing_frames) + 0.5);
    uint64_t run = 0;
    for (size_t bin = 0; bin < embedded.pacing_hist.size(); ++bin) {
      run += embedded.pacing_hist[bin];
      if (run >= std::max<uint64_t>(need, 1)) return (double(bin) + 0.5) * 0.1;
    }
    return double(embedded.pacing_hist.size()) * 0.1;
  };
  const double frequency = double(embedded.qpc_frequency);
  const double target_ms = embedded.pacing_target ? 1000.0 / embedded.pacing_target : 0.0;
  const double mean_ms = double(embedded.pacing_sum_ticks) * 1000.0 / frequency /
                         double(embedded.pacing_frames);
  const double limit_ms = 1.5 * (target_ms ? target_ms : mean_ms);
  uint64_t over = 0;
  for (size_t bin = 0; bin < embedded.pacing_hist.size(); ++bin) {
    if ((double(bin) + 0.5) * 0.1 > limit_ms) over += embedded.pacing_hist[bin];
  }
  LARGE_INTEGER now;
  QueryPerformanceCounter(&now);
  std::fprintf(stderr,
               "REX_GUEST_FRAME_PACING target=%u frames=%llu mean_ms=%.3f p50_ms=%.2f "
               "p99_ms=%.2f max_ms=%.3f over_1_5x=%llu grid_resets=%llu spin_margin_us=%llu "
               "qpc=%lld\n",
               embedded.pacing_target, static_cast<unsigned long long>(embedded.pacing_frames),
               mean_ms, percentile(0.50), percentile(0.99),
               double(embedded.pacing_max_ticks) * 1000.0 / frequency,
               static_cast<unsigned long long>(over),
               static_cast<unsigned long long>(embedded.frame_deadline.grid_resets()),
               static_cast<unsigned long long>(embedded.frame_spin.margin() * 1000000 /
                                               embedded.qpc_frequency),
               static_cast<long long>(now.QuadPart));
  std::fflush(stderr);
  embedded.pacing_hist.fill(0);
  embedded.pacing_frames = 0;
  embedded.pacing_sum_ticks = 0;
  embedded.pacing_max_ticks = 0;
}

#if defined(_WIN32)
// Current refresh rate of the display that shows the game window (integer Hz
// as reported by GDI, e.g. 59 for 59.94; 0 when unknown).
double QueryWindowRefreshHz(void* window) {
  if (!window) return 0.0;
  HMONITOR monitor = MonitorFromWindow(static_cast<HWND>(window), MONITOR_DEFAULTTONEAREST);
  MONITORINFOEXW info{};
  info.cbSize = sizeof(info);
  if (!monitor || !GetMonitorInfoW(monitor, &info)) return 0.0;
  DEVMODEW mode{};
  mode.dmSize = sizeof(mode);
  if (!EnumDisplaySettingsW(info.szDevice, ENUM_CURRENT_SETTINGS, &mode)) return 0.0;
  return mode.dmDisplayFrequency > 1 ? double(mode.dmDisplayFrequency) : 0.0;
}

// Dedicated memory of the high-performance graphics card (0 when unknown).
uint64_t QueryCardVideoMemoryBytes() {
  uint64_t bytes = 0;
  IDXGIFactory6* factory = nullptr;
  if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory6), reinterpret_cast<void**>(&factory)))) {
    return 0;
  }
  IDXGIAdapter1* adapter = nullptr;
  if (SUCCEEDED(factory->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                                                    __uuidof(IDXGIAdapter1),
                                                    reinterpret_cast<void**>(&adapter)))) {
    DXGI_ADAPTER_DESC1 desc{};
    if (SUCCEEDED(adapter->GetDesc1(&desc)) && !(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
      bytes = uint64_t(desc.DedicatedVideoMemory);
    }
    adapter->Release();
  }
  factory->Release();
  return bytes;
}

// NVIDIA DLSS needs an NVIDIA RTX card: the high-performance adapter is an
// NVIDIA one (vendor 0x10DE) with RTX in its name. True when DXGI can't tell.
bool QueryNvidiaRtxCard() {
  IDXGIFactory6* factory = nullptr;
  if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory6), reinterpret_cast<void**>(&factory)))) {
    return true;
  }
  bool rtx = false;
  IDXGIAdapter1* adapter = nullptr;
  if (SUCCEEDED(factory->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                                                    __uuidof(IDXGIAdapter1),
                                                    reinterpret_cast<void**>(&adapter)))) {
    DXGI_ADAPTER_DESC1 desc{};
    rtx = SUCCEEDED(adapter->GetDesc1(&desc)) && desc.VendorId == 0x10DE &&
          !(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) && std::wcsstr(desc.Description, L"RTX");
    adapter->Release();
  }
  factory->Release();
  return rtx;
}
#endif

void RefreshFramePolicy(EmbeddedGpu& embedded, uint64_t tick) {
#if defined(_WIN32)
  // A menu transition re-resolves at once; otherwise every two seconds.
  const bool menu = embedded.menu_active.load(std::memory_order_acquire) &&
                    REXCVAR_GET(display_menu_frame_rate) == "reduced";
  if (menu == embedded.frame_policy_menu && embedded.frame_policy_tick &&
      embedded.qpc_frequency && tick - embedded.frame_policy_tick < 2 * embedded.qpc_frequency) {
    return;
  }
  embedded.frame_policy_tick = tick ? tick : 1;
  const double refresh = QueryWindowRefreshHz(
      embedded.native_window.load(std::memory_order_acquire));
  const std::string& mode_text = REXCVAR_GET(display_frame_rate);
  rex::ui::FrameRateMode mode = rex::ui::FrameRateMode::kOriginal;
  uint32_t custom = REXCVAR_GET(display_frame_limit);
  rex::ui::ParseFrameRate(mode_text, mode, custom);
  const std::string& present_text = REXCVAR_GET(display_present_mode);
  const bool present_vsync = present_text != "immediate" && present_text != "vrr";
  rex::ui::FrameRatePolicy policy =
      rex::ui::ResolveFrameRatePolicy(mode, custom, present_vsync, refresh);
  // The title's own pacing (original) already runs menus at its console rate.
  if (menu && mode != rex::ui::FrameRateMode::kOriginal) {
    policy = rex::ui::MenuFrameRatePolicy(policy, present_vsync, refresh);
  }
  char key[176];
  std::snprintf(key, sizeof(key), "%s|%s|%u|%.1f|%u", mode_text.c_str(), present_text.c_str(),
                custom, refresh, menu ? 1u : 0u);
  if (menu != embedded.frame_policy_menu) {
    embedded.frame_policy_menu = menu;
    ++embedded.menu_transitions;
  }
  if (embedded.frame_policy_key == key) return;
  embedded.frame_policy_key = key;
  embedded.frame_policy = policy;
  embedded.frame_policy_refresh_hz = refresh;
  // The developer switch (dev_frame_mode_hotkeys) owns the present override
  // while it is active; otherwise apply the resolved vsync interval.
  if (embedded.dev_frame_limit_override.load(std::memory_order_acquire) < 0) {
    rex::ui::SetHostPresentOverride(-1, policy.display_paced ? policy.vsync_interval : 0);
  }
  std::fprintf(stderr,
               "REX_FRAME_RATE_POLICY mode=%s present=%s refresh_hz=%.1f target_fps=%.2f "
               "limit_fps=%u vsync_interval=%u display_paced=%u uneven_without_vrr=%u "
               "menu=%u menu_transitions=%llu\n",
               mode_text.c_str(), present_text.c_str(), refresh, policy.target_fps,
               policy.limit_fps, policy.vsync_interval, policy.display_paced ? 1u : 0u,
               policy.uneven_without_vrr ? 1u : 0u, menu ? 1u : 0u,
               static_cast<unsigned long long>(embedded.menu_transitions));
  std::fflush(stderr);
#else
  (void)embedded;
  (void)tick;
#endif
}

// Called by the embedded runtime at the title's guest frame-production
// boundary (the producer thread, once per built frame). Paces production to
// display_frame_limit (or the developer override); returns immediately when
// uncapped. Never touches guest state, clocks, VBlank or GPU work.
#if defined(REXGLUE_PGO_GENERATE)
extern "C" int __llvm_profile_write_file(void);
extern "C" void __llvm_profile_reset_counters(void);
namespace {
// Profile-guided training builds only (never a player build): merge the
// counters into LLVM_PROFILE_FILE (a %m pattern) every 10 s and restart them,
// so a run closed by the test harness keeps its profile.
void MaybeWritePgoProfile() {
  static std::atomic<uint64_t> next_write_ms{0};
  const uint64_t now = GetTickCount64();
  uint64_t next = next_write_ms.load(std::memory_order_relaxed);
  if (next && now < next) return;
  if (!next_write_ms.compare_exchange_strong(next, now + 10000)) return;
  if (!next) return;  // The first call only arms the timer.
  if (__llvm_profile_write_file() == 0) {
    __llvm_profile_reset_counters();
  }
}
}  // namespace
#endif

extern "C" REX_GPU_PLUGIN_EXPORT void rex_gpu_embedded_frame_boundary(void* handle) {
  auto* embedded = static_cast<EmbeddedGpu*>(handle);
  if (!embedded) return;
#if defined(REXGLUE_PGO_GENERATE)
  MaybeWritePgoProfile();
#endif
#if REX_GPU_DIAGNOSTICS
  if (auto& sources = DiagInvalidations(); ++sources.frames == 600) {
    const double frames = double(sources.frames);
    std::fprintf(stderr,
                 "REX_INVALIDATION_SOURCES frames=%llu host_small_per_frame=%.1f "
                 "host_large_per_frame=%.1f host_kb_per_frame=%.1f drain_ranges_per_frame=%.1f "
                 "drain_kb_per_frame=%.1f\n",
                 static_cast<unsigned long long>(sources.frames),
                 double(sources.host_small.exchange(0, std::memory_order_relaxed)) / frames,
                 double(sources.host_large.exchange(0, std::memory_order_relaxed)) / frames,
                 double(sources.host_bytes.exchange(0, std::memory_order_relaxed)) / 1024.0 / frames,
                 double(sources.drain_ranges.exchange(0, std::memory_order_relaxed)) / frames,
                 double(sources.drain_bytes.exchange(0, std::memory_order_relaxed)) / 1024.0 / frames);
    sources.frames = 0;
  }
#endif
  std::unique_lock<std::mutex> lock(embedded->frame_limiter_mutex, std::try_to_lock);
  if (!lock.owns_lock()) return;
  if (!embedded->frame_limiter_announced) {
    embedded->frame_limiter_announced = true;
    rex::ui::SetGuestFrameLimiterActive(true);
    std::fprintf(stderr, "REX_GUEST_FRAME_LIMITER active=1 frame_rate=%s custom_limit=%u\n",
                 REXCVAR_GET(display_frame_rate).c_str(), REXCVAR_GET(display_frame_limit));
    std::fflush(stderr);
  }
#if defined(_WIN32)
  {
    LARGE_INTEGER policy_now;
    QueryPerformanceCounter(&policy_now);
    RefreshFramePolicy(*embedded, uint64_t(policy_now.QuadPart));
  }
#endif
  const int32_t dev_limit = embedded->dev_frame_limit_override.load(std::memory_order_acquire);
  uint32_t target = dev_limit >= 0 ? uint32_t(dev_limit) : embedded->frame_policy.limit_fps;
#if defined(_WIN32)
  void* window = embedded->native_window.load(std::memory_order_acquire);
  if (window && IsIconic(static_cast<HWND>(window)) &&
      (!target || target > kMinimizedFrameLimit)) {
    target = kMinimizedFrameLimit;
    ++embedded->frame_background_capped;
  }
#endif
  WaitGuestFrameDeadline(*embedded, target);
#if defined(_WIN32)
  if (REXCVAR_GET(dev_frame_mode_hotkeys)) {
    LARGE_INTEGER released;
    QueryPerformanceCounter(&released);
    const uint64_t tick = uint64_t(released.QuadPart);
    const uint64_t frequency = embedded->qpc_frequency;
    if (embedded->pacing_report_requested.exchange(false, std::memory_order_acq_rel) ||
        embedded->pacing_target != target) {
      // Report the finished mode, then start a new one.
      ReportGuestFramePacing(*embedded);
      embedded->pacing_target = target;
      embedded->pacing_last_release = 0;
    }
    if (embedded->pacing_last_release && frequency) {
      const uint64_t interval = tick - embedded->pacing_last_release;
      const uint64_t bin = interval * 10000 / frequency;  // 0.1 ms bins
      ++embedded->pacing_hist[std::min<uint64_t>(bin, embedded->pacing_hist.size() - 1)];
      ++embedded->pacing_frames;
      embedded->pacing_sum_ticks += interval;
      embedded->pacing_max_ticks = std::max(embedded->pacing_max_ticks, interval);
    }
    embedded->pacing_last_release = tick;
  }
#endif
  if ((++embedded->frame_boundaries & 0x3FFF) == 0) {
    std::fprintf(stderr,
                 "REX_GUEST_FRAME_LIMITER boundaries=%llu waits=%llu target=%u grid_resets=%llu "
                 "background_capped=%llu spin_margin_us=%llu\n",
                 static_cast<unsigned long long>(embedded->frame_boundaries),
                 static_cast<unsigned long long>(embedded->frame_waits), target,
                 static_cast<unsigned long long>(embedded->frame_deadline.grid_resets()),
                 static_cast<unsigned long long>(embedded->frame_background_capped),
                 static_cast<unsigned long long>(
                     embedded->qpc_frequency
                         ? embedded->frame_spin.margin() * 1000000 / embedded->qpc_frequency
                         : 0));
    std::fflush(stderr);
  }
}

// The host's view of the title: nonzero while the front end, a loading screen
// or a modal menu is up (display_menu_frame_rate paces those). Read state only.
extern "C" REX_GPU_PLUGIN_EXPORT void rex_gpu_embedded_set_menu_state(void* handle,
                                                                       uint32_t in_menu) {
  auto* embedded = static_cast<EmbeddedGpu*>(handle);
  if (!embedded) return;
  embedded->menu_active.store(in_menu != 0, std::memory_order_release);
}

// GPU busy time of the most recent frames in microseconds, oldest first (the
// command processor's GPU frame meter); returns the number copied.
extern "C" REX_GPU_PLUGIN_EXPORT uint32_t rex_gpu_embedded_get_gpu_frame_busy_us(
    void* handle, uint32_t* out, uint32_t capacity, uint64_t* total_frames) {
  auto* embedded = static_cast<EmbeddedGpu*>(handle);
  if (total_frames) *total_frames = 0;
  if (!embedded || !embedded->graphics) return 0;
  const auto* command_processor = embedded->graphics->command_processor();
  if (!command_processor) return 0;
  return uint32_t(command_processor->GetRecentGpuFrameBusyUs(out, capacity, total_frames));
}

extern "C" REX_GPU_PLUGIN_EXPORT void rex_gpu_embedded_interrupt_completed(void* handle) {
  if (handle && REXCVAR_GET(embedded_interrupt_wait_wakeup))
    rex::graphics::cp_interrupt_wakeup.Notify();
}
extern "C" REX_GPU_PLUGIN_EXPORT uint64_t rex_gpu_embedded_interrupt_timing_token(void* handle) {
  return handle ? rex::graphics::cp_interrupt_timing.Token() : 0;
}
extern "C" REX_GPU_PLUGIN_EXPORT void rex_gpu_embedded_interrupt_timing_record(
    void* handle, uint64_t token, uint32_t source, uint32_t address,
    uint32_t before, uint32_t after, uint64_t enqueue, uint64_t dispatch,
    uint64_t returned) {
  if (!handle) return;
  rex::graphics::cp_interrupt_timing.Add(token,
      {1, source, address, before, after, enqueue, dispatch, returned});
}

// Keyboard button prompts: the host reports the device of the latest player
// input (1 = controller, 2 = keyboard/mouse) when it changes.
extern "C" REX_GPU_PLUGIN_EXPORT void rex_gpu_embedded_note_input_device(void* handle,
                                                                       uint32_t device) {
  if (!handle || device > 2) return;
  rex::graphics::prompt_icons::NoteInputDevice(rex::graphics::prompt_icons::Device(device));
}

// Keyboard button prompts for the host's prompt text: returns bit 0 = keyboard
// prompts wanted now, bits 8-31 = the labels generation; writes the labels
// as "button=label" lines (UTF-8, text spelling: arrows as words) when the
// buffer is given and large enough (else an empty string).
extern "C" REX_GPU_PLUGIN_EXPORT uint32_t rex_gpu_embedded_get_prompt_labels(void* handle,
                                                                           char* buffer,
                                                                           uint32_t size) {
  if (buffer && size) buffer[0] = '\0';
  if (!handle) return 0;
  namespace prompt_icons = rex::graphics::prompt_icons;
  const uint32_t state = (prompt_icons::KeyboardPromptsWanted() ? 1u : 0u) |
                         (prompt_icons::LabelsGeneration() << 8);
  if (buffer && size) {
    std::string text;
    for (const prompt_icons::ButtonLabel& label : prompt_icons::CurrentLabels()) {
      text += std::string(label.button) + "=" +
              prompt_icons::TextKeyLabel(PromptBindingValue(label.button)) + "\n";
    }
    if (text.size() < size) {
      std::memcpy(buffer, text.c_str(), text.size() + 1);
    }
  }
  return state;
}

extern "C" REX_GPU_PLUGIN_EXPORT void rex_gpu_embedded_note_input_transition(
    void* handle, int64_t host_performance_counter,
    int64_t host_performance_frequency, uint32_t packet_number,
    uint32_t buttons) {
  if (!handle) return;
#if REX_HAS_D3D12
  rex::graphics::d3d12::NoteEmbeddedInputTransition(
      host_performance_counter, host_performance_frequency, packet_number,
      buttons);
#else
  (void)host_performance_counter;
  (void)host_performance_frequency;
  (void)packet_number;
  (void)buttons;
#endif
}

extern "C" REX_GPU_PLUGIN_EXPORT uint32_t rex_gpu_embedded_get_manual_capture(
    void* handle, uint64_t* generation) {
  if (!generation) return 0;
  *generation = 0;
  if (!handle) return 0;
#if REX_HAS_D3D12
  return rex::graphics::d3d12::GetEmbeddedGameplayCaptureRequest(*generation)
             ? 1u : 0u;
#else
  return 0;
#endif
}

extern "C" REX_GPU_PLUGIN_EXPORT uint32_t rex_gpu_embedded_get_pc_settings(
    void* handle, rex::system::EmbeddedGpuPcSettings* settings) {
  if (!handle || !settings ||
      settings->struct_size < sizeof(rex::system::EmbeddedGpuPcSettings)) {
    return 0;
  }
  auto* embedded = static_cast<EmbeddedGpu*>(handle);
  settings->gameplay_fov_degrees =
      static_cast<float>(REXCVAR_GET(camera_field_of_view));
  settings->trace_camera_state = REXCVAR_GET(diagnostics_camera_state) ? 1u : 0u;
  settings->keyboard_mouse_enabled =
      embedded->keyboard_mouse && embedded->keyboard_mouse->IsEnabled() ? 1u : 0u;
  settings->keyboard_mouse_user_index = embedded->keyboard_mouse
      ? embedded->keyboard_mouse->UserIndex()
      : 0u;
  return 1;
}

extern "C" REX_GPU_PLUGIN_EXPORT uint32_t
rex_gpu_embedded_poll_keyboard_mouse(
    void* handle, uint32_t user_index,
    rex::system::EmbeddedHostInputState* output) {
  auto* embedded = static_cast<EmbeddedGpu*>(handle);
  if (!embedded || !embedded->keyboard_mouse || !output ||
      output->struct_size < sizeof(rex::system::EmbeddedHostInputState)) {
    return 0;
  }
  rex::input::X_INPUT_STATE state{};
  if (embedded->keyboard_mouse->GetState(user_index, &state) != 0) {
    return 0;
  }
  output->packet_number = static_cast<uint32_t>(state.packet_number);
  output->buttons = static_cast<uint16_t>(state.gamepad.buttons);
  output->left_trigger = state.gamepad.left_trigger;
  output->right_trigger = state.gamepad.right_trigger;
  output->thumb_lx = static_cast<int16_t>(state.gamepad.thumb_lx);
  output->thumb_ly = static_cast<int16_t>(state.gamepad.thumb_ly);
  output->thumb_rx = static_cast<int16_t>(state.gamepad.thumb_rx);
  output->thumb_ry = static_cast<int16_t>(state.gamepad.thumb_ry);
  return 1;
}

// Developer test input: the script's virtual controller buttons (`pad`) for
// the host's native controller path; 0 without a driving script.
extern "C" REX_GPU_PLUGIN_EXPORT uint32_t rex_gpu_embedded_test_pad_buttons(void* handle) {
  auto* embedded = static_cast<EmbeddedGpu*>(handle);
  if (!embedded || !embedded->keyboard_mouse) return 0;
  return embedded->keyboard_mouse->TestPadButtons();
}

extern "C" REX_GPU_PLUGIN_EXPORT uint32_t
rex_gpu_embedded_consume_mouse_look(void* handle, uint32_t user_index,
                                    rex::system::EmbeddedMouseLook* output) {
  auto* embedded = static_cast<EmbeddedGpu*>(handle);
  if (!embedded || !embedded->keyboard_mouse || !output ||
      output->struct_size < sizeof(rex::system::EmbeddedMouseLook)) {
    return 0;
  }
  int32_t dx = 0;
  int32_t dy = 0;
  output->active = embedded->keyboard_mouse->ConsumeMouseLook(user_index, dx, dy) ? 1u : 0u;
  output->dx = dx;
  output->dy = dy;
  output->sensitivity =
      static_cast<float>(rex::input::mnk::MnkInputDriver::MouseSensitivity());
  output->invert_y = rex::input::mnk::MnkInputDriver::MouseInvertY() ? 1u : 0u;
  return 1;
}

// In-game settings overlay (host side: runtime settings service).
// configure: the host's settings schema, saved values and defaults as TOML
// (rex/ui/settings_schema.h); flags bit 0 = the host saves edits.
extern "C" REX_GPU_PLUGIN_EXPORT uint32_t rex_gpu_embedded_settings_configure(
    void* handle, const char* schema_toml, const char* saved_toml, const char* defaults_toml,
    uint32_t flags) {
  auto* embedded = static_cast<EmbeddedGpu*>(handle);
  if (!embedded || !schema_toml || !saved_toml || !defaults_toml) return 0;
  std::string error;
  auto schema = rex::ui::settings::ParseSchema(schema_toml, &error);
  auto saved = rex::ui::settings::ParseValues(saved_toml, &error);
  auto defaults = rex::ui::settings::ParseValues(defaults_toml, &error);
  if (!schema || !saved || !defaults) {
    std::fprintf(stderr, "REX_SETTINGS_OVERLAY configure=0 error=%s\n", error.c_str());
    std::fflush(stderr);
    return 0;
  }
  {
    auto& state = embedded->settings_state;
    std::lock_guard<std::mutex> lock(state.mutex);
    state.schema = std::move(*schema);
    state.saved = std::move(*saved);
    state.defaults = std::move(*defaults);
    state.persistence = (flags & 1u) != 0;
    state.configured = true;
    ++state.generation;
  }
  std::fprintf(stderr, "REX_SETTINGS_OVERLAY configure=1 settings=%zu persistence=%u\n",
               embedded->settings_state.schema.settings.size(), flags & 1u);
  std::fflush(stderr);
  return 1;
}

// poll: edits made in the overlay since the last poll, as TOML key = "value".
// Returns the text length (0 = none). When capacity is too small nothing is
// consumed and the required size is returned.
extern "C" REX_GPU_PLUGIN_EXPORT uint32_t rex_gpu_embedded_settings_poll(
    void* handle, char* buffer, uint32_t capacity) {
  auto* embedded = static_cast<EmbeddedGpu*>(handle);
  if (!embedded) return 0;
  auto& state = embedded->settings_state;
  std::lock_guard<std::mutex> lock(state.mutex);
  if (state.pending.empty()) return 0;
  const std::string text = rex::ui::settings::SerializeValues(state.pending);
  const uint32_t length = uint32_t(text.size());
  if (!buffer || capacity <= length) return length + 1;
  std::memcpy(buffer, text.c_str(), text.size() + 1);
  state.pending.clear();
  return length;
}

// saved: the host's result for the last edits (new saved values, status).
extern "C" REX_GPU_PLUGIN_EXPORT uint32_t rex_gpu_embedded_settings_saved(
    void* handle, const char* saved_toml, const char* status) {
  auto* embedded = static_cast<EmbeddedGpu*>(handle);
  if (!embedded || !saved_toml) return 0;
  auto saved = rex::ui::settings::ParseValues(saved_toml, nullptr);
  auto& state = embedded->settings_state;
  std::lock_guard<std::mutex> lock(state.mutex);
  if (saved) state.saved = std::move(*saved);
  state.status = status ? status : "";
  ++state.generation;
  return saved ? 1 : 0;
}

// V380: 1 once the player asked to close the game window.
extern "C" REX_GPU_PLUGIN_EXPORT uint32_t rex_gpu_embedded_close_requested(void* handle) {
  auto* embedded = static_cast<EmbeddedGpu*>(handle);
  return embedded && embedded->close_requested.load(std::memory_order_acquire) ? 1u : 0u;
}

extern "C" REX_GPU_PLUGIN_EXPORT uint32_t rex_gpu_embedded_settings_overlay_open(void* handle) {
  auto* embedded = static_cast<EmbeddedGpu*>(handle);
  return embedded && embedded->settings_overlay_open.load(std::memory_order_acquire) ? 1u : 0u;
}

extern "C" REX_GPU_PLUGIN_EXPORT uint32_t rex_gpu_embedded_xma_setup(
    void* handle, uint32_t context_array_guest_address) {
  auto* embedded = static_cast<EmbeddedGpu*>(handle);
  if (!embedded || embedded->xma || !context_array_guest_address) return 0;
  auto decoder = std::make_unique<rex::audio::XmaDecoder>(&embedded->memory);
  const rex::X_STATUS status = decoder->SetupExternal(context_array_guest_address);
  if (XFAILED(status)) {
    std::fprintf(stderr,
                 "REX_EMBEDDED_XMA_SETUP_FAILURE status=0x%08X context_array=0x%08X\n",
                 uint32_t(status), context_array_guest_address);
    std::fflush(stderr);
    return 0;
  }
  embedded->xma = std::move(decoder);
  std::fprintf(stderr,
               "REX_EMBEDDED_XMA_READY context_array=0x%08X physical=0x%08X contexts=320\n",
               context_array_guest_address, embedded->xma->context_array_ptr());
  std::fflush(stderr);
  return 1;
}

extern "C" REX_GPU_PLUGIN_EXPORT void rex_gpu_embedded_xma_shutdown(void* handle) {
  auto* embedded = static_cast<EmbeddedGpu*>(handle);
  if (embedded && embedded->xma) {
    embedded->xma->Shutdown();
    embedded->xma.reset();
  }
}

extern "C" REX_GPU_PLUGIN_EXPORT uint32_t rex_gpu_embedded_xma_allocate(void* handle) {
  auto* embedded = static_cast<EmbeddedGpu*>(handle);
  return embedded && embedded->xma ? embedded->xma->AllocateContext() : 0;
}

extern "C" REX_GPU_PLUGIN_EXPORT void rex_gpu_embedded_xma_release(
    void* handle, uint32_t context_guest_address) {
  auto* embedded = static_cast<EmbeddedGpu*>(handle);
  if (embedded && embedded->xma && context_guest_address) {
    embedded->xma->ReleaseContext(context_guest_address);
  }
}

extern "C" REX_GPU_PLUGIN_EXPORT uint32_t rex_gpu_embedded_xma_read_mmio(
    void* handle, uint32_t address) {
  auto* embedded = static_cast<EmbeddedGpu*>(handle);
  return embedded && embedded->xma ? embedded->xma->ReadRegister(address) : 0;
}

extern "C" REX_GPU_PLUGIN_EXPORT void rex_gpu_embedded_xma_write_mmio(
    void* handle, uint32_t address, uint32_t value) {
  auto* embedded = static_cast<EmbeddedGpu*>(handle);
  if (embedded && embedded->xma) embedded->xma->WriteRegister(address, value);
}
