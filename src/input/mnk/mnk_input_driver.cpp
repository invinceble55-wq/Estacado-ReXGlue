/**
 * @file        input/mnk/mnk_input_driver.cpp
 * @brief       Keyboard/mouse input driver implementation.
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#include <rex/input/mnk/mnk_input_driver.h>

#include <rex/cvar.h>
#include <rex/input/input.h>
#include <rex/input/mnk/mouse_button_policy.h>
#include <rex/input/mnk/mouse_motion_policy.h>
#include <rex/logging.h>
#include <rex/ui/keybinds.h>
#include <rex/ui/virtual_key.h>
#include <rex/ui/window.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cmath>
#include <cstdlib>
#include <cstring>

#if REX_PLATFORM_WIN32
#include <Windows.h>
#endif

REXCVAR_DEFINE_BOOL(input_keyboard_mouse, false, "Input",
                    "Enable physical keyboard/mouse input")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_INT32(input_keyboard_mouse_user_index, 0, "Input",
                     "Controller slot (0-3) for keyboard/mouse")
    .range(0, 3)
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_DOUBLE(input_mouse_sensitivity, 1.0, "Input",
                      "Relative mouse sensitivity")
    .range(0.01, 10.0);
REXCVAR_DEFINE_DOUBLE(input_mouse_acceleration, 0.0, "Input",
                      "Bounded per-event relative mouse acceleration")
    .range(0.0, 1.0);
REXCVAR_DEFINE_DOUBLE(input_mouse_smoothing, 0.0, "Input",
                      "Finite two-sample relative mouse smoothing")
    .range(0.0, 1.0);
REXCVAR_DEFINE_BOOL(input_mouse_invert_y, false, "Input",
                    "Invert the optional relative-mouse look Y axis");
REXCVAR_DEFINE_STRING(input_mouse_look, "native", "Input",
                      "Mouse look path: native (relative counts fed to the title's own look "
                      "command by the host, 1:1 without stick dead zone or acceleration) or "
                      "stick (right-stick bridge)")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

// The Darkness: A = use, Y = jump; use on E and jump on Space like most PC
// games (Estacado 0.9.1; 0.9.0 had A on Space and Y on E).
REXCVAR_DEFINE_STRING(input_bind_a, "E", "Input/Keybinds/Controller", "A button");
REXCVAR_DEFINE_STRING(input_bind_b, "Shift", "Input/Keybinds/Controller", "B button");
REXCVAR_DEFINE_STRING(input_bind_x, "R", "Input/Keybinds/Controller", "X button");
REXCVAR_DEFINE_STRING(input_bind_y, "Space", "Input/Keybinds/Controller", "Y button");
REXCVAR_DEFINE_STRING(input_bind_left_trigger, "RMB", "Input/Keybinds/Controller", "Left trigger");
REXCVAR_DEFINE_STRING(input_bind_right_trigger, "LMB", "Input/Keybinds/Controller", "Right trigger");
REXCVAR_DEFINE_STRING(input_bind_left_shoulder, "Q", "Input/Keybinds/Controller", "Left shoulder");
REXCVAR_DEFINE_STRING(input_bind_right_shoulder, "F", "Input/Keybinds/Controller", "Right shoulder");
REXCVAR_DEFINE_STRING(input_bind_lstick_up, "W", "Input/Keybinds/Controller", "Left stick up");
REXCVAR_DEFINE_STRING(input_bind_lstick_down, "S", "Input/Keybinds/Controller", "Left stick down");
REXCVAR_DEFINE_STRING(input_bind_lstick_left, "A", "Input/Keybinds/Controller", "Left stick left");
REXCVAR_DEFINE_STRING(input_bind_lstick_right, "D", "Input/Keybinds/Controller", "Left stick right");
REXCVAR_DEFINE_STRING(input_bind_lstick_press, "C", "Input/Keybinds/Controller", "Left stick press");
REXCVAR_DEFINE_STRING(input_bind_rstick_press, "MMB", "Input/Keybinds/Controller",
                      "Right stick press");
REXCVAR_DEFINE_STRING(input_bind_dpad_up, "Up", "Input/Keybinds/Controller", "D-pad up");
REXCVAR_DEFINE_STRING(input_bind_dpad_down, "Down", "Input/Keybinds/Controller", "D-pad down");
REXCVAR_DEFINE_STRING(input_bind_dpad_left, "Left", "Input/Keybinds/Controller", "D-pad left");
REXCVAR_DEFINE_STRING(input_bind_dpad_right, "Right", "Input/Keybinds/Controller", "D-pad right");
REXCVAR_DEFINE_STRING(input_bind_back, "Tab", "Input/Keybinds/Controller", "Back button");
REXCVAR_DEFINE_STRING(input_bind_start, "Escape", "Input/Keybinds/Controller", "Start button");
REXCVAR_DEFINE_UINT32(input_test_tap_hold_ms, 0, "Input/Developer",
                     "Opt-in bounded host key-tap retention for offline tests")
    .range(0, 250)
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(input_test_camera_keys, false, "Input/Developer",
                    "Opt-in I/J/K/L controller-look keys for offline tests")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_STRING(input_test_script, "", "Input/Developer",
                      "Opt-in test input command file for isolated automated runs "
                      "(bound keys and mouse counts; yields to local input; empty: off)")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_STRING(input_bind_guide, "", "Input/Keybinds/Controller", "Guide button");

namespace rex::input::mnk {

using rex::ui::VirtualKey;

static uint64_t TestTapClockMs() {
  return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count());
}

static uint16_t ParseTestScriptKey(std::string_view name) {
  const VirtualKey vk = rex::ui::ParseVirtualKey(name);
  const auto index = static_cast<uint16_t>(vk);
  return vk == VirtualKey::kNone || index >= 256 ? 0 : index;
}

static int64_t TestScriptQpc() {
#if REX_PLATFORM_WIN32
  LARGE_INTEGER counter;
  QueryPerformanceCounter(&counter);
  return counter.QuadPart;
#else
  return 0;
#endif
}

// Stick bridge hold: polls closer than this share one deflection.
static constexpr uint64_t kMouseStickHoldMs = 4;
// Local input anywhere on the machine this recently yields a running script.
static constexpr uint32_t kTestScriptYieldIdleMs = 1500;
static constexpr uint32_t kTestScriptReadIntervalMs = 25;
static constexpr size_t kTestScriptReadChunk = 64 * 1024;

MnkInputDriver::MnkInputDriver(rex::ui::Window* window, size_t window_z_order)
    : InputDriver(window, window_z_order) {}

MnkInputDriver::~MnkInputDriver() {
  // Detach handled by OnClosing; if window outlives the driver, clean up here.
  std::lock_guard lock(state_mutex_);
  if (attached_window_) {
    if (relative_mouse_mode_) {
      attached_window_->SetRelativeMouseMode(false);
      relative_mouse_mode_ = false;
    }
    attached_window_->RemoveInputListener(this);
    attached_window_->RemoveListener(this);
    attached_window_ = nullptr;
  }
}

X_STATUS MnkInputDriver::Setup() {
  REXLOG_INFO("MnK input driver initialized");
  if (REXCVAR_GET(input_test_camera_keys)) {
    std::fprintf(stderr, "REX_INPUT_TEST_CAMERA source=window_keydown keys=I,J,K,L hold_ms=%u enabled=%u\n",
                 REXCVAR_GET(input_test_tap_hold_ms),
                 REXCVAR_GET(input_test_tap_hold_ms) != 0 ? 1u : 0u);
  }
  if (IsEnabled() && !REXCVAR_GET(input_test_script).empty()) {
    std::lock_guard lock(state_mutex_);
    test_script_path_ = REXCVAR_GET(input_test_script);
    std::fprintf(stderr,
                 "REX_INPUT_TEST_SCRIPT enabled=1 path=%s yield_idle_ms=%u source=file_commands\n",
                 test_script_path_.c_str(), kTestScriptYieldIdleMs);
    std::fflush(stderr);
  }
  return X_STATUS_SUCCESS;
}

bool MnkInputDriver::TestScriptDrivingLocked() const {
  return !test_script_path_.empty() && !test_script_.yielded();
}

void MnkInputDriver::PumpTestScriptLocked() {
  if (test_script_path_.empty()) {
    return;
  }
  const uint64_t now_ms = TestTapClockMs();
  // Poll rate record (test runs only): how often the title reads the pad.
  if (!test_script_polls_++) {
    test_script_poll_start_ms_ = now_ms;
  } else if (test_script_polls_ % 4096 == 0 && now_ms > test_script_poll_start_ms_) {
    std::fprintf(stderr, "REX_INPUT_TEST_SCRIPT_POLLS polls=%llu per_s=%.1f\n",
                 static_cast<unsigned long long>(test_script_polls_),
                 double(test_script_polls_) * 1000.0 / double(now_ms - test_script_poll_start_ms_));
    std::fflush(stderr);
  }
  if (now_ms >= test_script_next_read_ms_) {
    test_script_next_read_ms_ = now_ms + kTestScriptReadIntervalMs;
#if REX_PLATFORM_WIN32
    // Shared open: the test harness appends while the title runs.
    std::FILE* file = _fsopen(test_script_path_.c_str(), "rb", _SH_DENYNO);
#else
    std::FILE* file = std::fopen(test_script_path_.c_str(), "rb");
#endif
    if (file) {
      std::string bytes(kTestScriptReadChunk, '\0');
      size_t read = 0;
#if REX_PLATFORM_WIN32
      if (_fseeki64(file, static_cast<int64_t>(test_script_offset_), SEEK_SET) == 0) {
#else
      if (std::fseek(file, static_cast<long>(test_script_offset_), SEEK_SET) == 0) {
#endif
        read = std::fread(bytes.data(), 1, bytes.size(), file);
      }
      std::fclose(file);
      if (read) {
        test_script_offset_ += read;
        std::vector<std::string> errors;
        test_script_.Append(std::string_view(bytes.data(), read), &ParseTestScriptKey, errors);
        for (const std::string& error : errors) {
          std::fprintf(stderr, "REX_INPUT_TEST_SCRIPT_ERROR %s\n", error.c_str());
        }
        if (!errors.empty()) std::fflush(stderr);
      }
    }
  }
#if REX_PLATFORM_WIN32
  if (!test_script_.yielded() && !test_script_.idle()) {
    LASTINPUTINFO info{};
    info.cbSize = sizeof(info);
    if (GetLastInputInfo(&info)) {
      const DWORD idle_ms = GetTickCount() - info.dwTime;
      if (idle_ms < kTestScriptYieldIdleMs) {
        test_script_.YieldToLocalInput(script_keys_);
        script_mouse_dx_ = 0;
        script_mouse_dy_ = 0;
        std::fprintf(stderr,
                     "REX_INPUT_TEST_SCRIPT yielded=1 reason=local_input idle_ms=%lu "
                     "qpc=%lld (append 'arm' to resume)\n",
                     static_cast<unsigned long>(idle_ms),
                     static_cast<long long>(TestScriptQpc()));
        std::fflush(stderr);
      }
    }
  }
#endif
  test_script_.Advance(
      now_ms, script_keys_, script_mouse_dx_, script_mouse_dy_,
      [this](const test_script_policy::Command& command) {
        // Host keys and clicks (the overlay key, overlay navigation) are
        // delivered to this game's own window by its owner on the UI thread;
        // never system input.
        if ((command.op == test_script_policy::Op::kPress ||
             command.op == test_script_policy::Op::kClick) &&
            host_event_callback_) {
          host_event_callback_(command);
        }
        if (test_script_log_count_ >= 100000) return;
        ++test_script_log_count_;
        std::fprintf(stderr,
                     "REX_INPUT_TEST_SCRIPT_CMD line=%u op=%s key=%u dx=%d dy=%d ms=%u "
                     "qpc=%lld text=%s\n",
                     command.line, test_script_policy::OpName(command.op), command.key,
                     command.dx, command.dy, command.ms,
                     static_cast<long long>(TestScriptQpc()), command.text.c_str());
        std::fflush(stderr);
      });
}

void MnkInputDriver::OnWindowAvailable(rex::ui::Window* window) {
  std::lock_guard lock(state_mutex_);
  if (window) {
    attached_window_ = window;
    // Embedded presentation attaches this listener after Window::Open, so the
    // initial OnGotFocus notification has already happened. Adopt the real
    // window state here instead of assuming a newly attached window is focused.
    has_focus_ = window->HasFocus();
    window->AddInputListener(this, window_z_order());
    window->AddListener(this);
  }
}

void MnkInputDriver::OnClosing(rex::ui::UIEvent&) {
  std::lock_guard lock(state_mutex_);
  test_taps_.Reset();
  std::memset(key_down_, 0, sizeof(key_down_));
  has_focus_ = false;
  if (attached_window_) {
    if (mouse_captured_) {
      if (relative_mouse_mode_) {
        attached_window_->SetRelativeMouseMode(false);
        relative_mouse_mode_ = false;
      }
      mouse_captured_ = false;
      attached_window_->SetCursorVisibility(precapture_cursor_visibility_);
      attached_window_->ReleaseMouse();
    }
    attached_window_->RemoveInputListener(this);
    attached_window_->RemoveListener(this);
    attached_window_ = nullptr;
  }
}

uint32_t MnkInputDriver::UserIndex() const {
  return static_cast<uint32_t>(REXCVAR_GET(input_keyboard_mouse_user_index));
}

bool MnkInputDriver::IsEnabled() const {
  return REXCVAR_GET(input_keyboard_mouse);
}

bool MnkInputDriver::MouseLookNative() {
  return REXCVAR_GET(input_mouse_look) == "native";
}

double MnkInputDriver::MouseSensitivity() {
  return REXCVAR_GET(input_mouse_sensitivity);
}

bool MnkInputDriver::MouseInvertY() {
  return REXCVAR_GET(input_mouse_invert_y);
}

bool MnkInputDriver::ConsumeMouseLook(uint32_t user_index, int32_t& dx, int32_t& dy) {
  dx = 0;
  dy = 0;
  if (!IsEnabled() || user_index != UserIndex() || !MouseLookNative()) {
    return false;
  }
  std::lock_guard lock(state_mutex_);
  UpdateMouseCapture();
  const bool physical = is_active() && has_focus_ && mouse_captured_;
  if (!physical) {
    mouse_dx_ = 0;
    mouse_dy_ = 0;
  }
  // A driving test script supplies counts without focus or capture.
  if (!is_active() || (!physical && !TestScriptDrivingLocked())) {
    script_mouse_dx_ = 0;
    script_mouse_dy_ = 0;
    return false;
  }
  dx = mouse_motion_policy::AccumulateEventDelta(mouse_dx_, script_mouse_dx_, 0.0);
  dy = mouse_motion_policy::AccumulateEventDelta(mouse_dy_, script_mouse_dy_, 0.0);
  mouse_dx_ = 0;
  mouse_dy_ = 0;
  script_mouse_dx_ = 0;
  script_mouse_dy_ = 0;
  return true;
}

static bool IsBindPressed(
    const bool* key_down, const std::string& cvar_val,
    mouse_wheel_policy::Direction wheel_direction,
    const test_tap_policy::BoundedTaps* test_taps, uint64_t test_now_ms) {
  if (cvar_val == "WheelUp") {
    return wheel_direction == mouse_wheel_policy::Direction::kUp;
  }
  if (cvar_val == "WheelDown") {
    return wheel_direction == mouse_wheel_policy::Direction::kDown;
  }
  VirtualKey vk = rex::ui::ParseVirtualKey(cvar_val);
  if (vk == VirtualKey::kNone)
    return false;
  uint16_t idx = static_cast<uint16_t>(vk);
  return idx < 256 && (key_down[idx] || (test_taps && test_taps->Active(idx, test_now_ms)));
}

X_RESULT MnkInputDriver::GetCapabilities(uint32_t user_index, uint32_t flags,
                                         X_INPUT_CAPABILITIES* out_caps) {
  if (!IsEnabled() || user_index != UserIndex()) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }
  if (out_caps) {
    std::memset(out_caps, 0, sizeof(*out_caps));
    out_caps->type = 0x01;
    out_caps->sub_type = 0x01;
    out_caps->flags = 0;
    out_caps->gamepad.buttons = 0xFFFF;
    out_caps->gamepad.left_trigger = 0xFF;
    out_caps->gamepad.right_trigger = 0xFF;
    out_caps->gamepad.thumb_lx = static_cast<int16_t>(0x7FFF);
    out_caps->gamepad.thumb_ly = static_cast<int16_t>(0x7FFF);
    out_caps->gamepad.thumb_rx = static_cast<int16_t>(0x7FFF);
    out_caps->gamepad.thumb_ry = static_cast<int16_t>(0x7FFF);
    out_caps->vibration.left_motor_speed = 0xFFFF;
    out_caps->vibration.right_motor_speed = 0xFFFF;
  }
  return X_ERROR_SUCCESS;
}

X_RESULT MnkInputDriver::GetState(uint32_t user_index, X_INPUT_STATE* out_state) {
  if (!IsEnabled() || user_index != UserIndex()) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }

  std::lock_guard lock(state_mutex_);
  UpdateMouseCapture();
  PumpTestScriptLocked();
  const bool scripted = TestScriptDrivingLocked();

  if (!is_active() || (!has_focus_ && !scripted)) {
    test_taps_.Reset();
    mouse_smoothing_x_.Reset();
    mouse_smoothing_y_.Reset();
    if (has_published_state_) {
      state_policy::Publish({}, published_state_, has_published_state_,
                            packet_number_);
    }
    if (out_state) {
      std::memset(out_state, 0, sizeof(*out_state));
      out_state->packet_number = packet_number_;
    }
    return X_ERROR_SUCCESS;
  }

  uint16_t buttons = 0;
  const bool test_taps_enabled = REXCVAR_GET(input_test_tap_hold_ms) != 0;
  const uint64_t test_now_ms = test_taps_enabled ? TestTapClockMs() : 0;
  const auto wheel_direction = mouse_wheel_pulses_.Pop();
  // Scripted keys act exactly like physical ones (test runs only).
  bool merged_keys[256];
  const bool* keys = key_down_;
  if (!test_script_path_.empty()) {
    for (size_t index = 0; index < 256; ++index) {
      merged_keys[index] = key_down_[index] || script_keys_[index];
    }
    keys = merged_keys;
  }
  const auto bind_pressed = [&](const std::string& binding) {
    return IsBindPressed(keys, binding, wheel_direction,
                         test_taps_enabled ? &test_taps_ : nullptr, test_now_ms);
  };
  if (bind_pressed(REXCVAR_GET(input_bind_a)))
    buttons |= X_INPUT_GAMEPAD_A;
  if (bind_pressed(REXCVAR_GET(input_bind_b)))
    buttons |= X_INPUT_GAMEPAD_B;
  if (bind_pressed(REXCVAR_GET(input_bind_x)))
    buttons |= X_INPUT_GAMEPAD_X;
  if (bind_pressed(REXCVAR_GET(input_bind_y)))
    buttons |= X_INPUT_GAMEPAD_Y;
  if (bind_pressed(REXCVAR_GET(input_bind_left_shoulder)))
    buttons |= X_INPUT_GAMEPAD_LEFT_SHOULDER;
  if (bind_pressed(REXCVAR_GET(input_bind_right_shoulder)))
    buttons |= X_INPUT_GAMEPAD_RIGHT_SHOULDER;
  if (bind_pressed(REXCVAR_GET(input_bind_lstick_press)))
    buttons |= X_INPUT_GAMEPAD_LEFT_THUMB;
  if (bind_pressed(REXCVAR_GET(input_bind_rstick_press)))
    buttons |= X_INPUT_GAMEPAD_RIGHT_THUMB;
  if (bind_pressed(REXCVAR_GET(input_bind_back)))
    buttons |= X_INPUT_GAMEPAD_BACK;
  if (bind_pressed(REXCVAR_GET(input_bind_start)))
    buttons |= X_INPUT_GAMEPAD_START;
  if (bind_pressed(REXCVAR_GET(input_bind_guide)))
    buttons |= X_INPUT_GAMEPAD_GUIDE;
  if (bind_pressed(REXCVAR_GET(input_bind_dpad_up)))
    buttons |= X_INPUT_GAMEPAD_DPAD_UP;
  if (bind_pressed(REXCVAR_GET(input_bind_dpad_down)))
    buttons |= X_INPUT_GAMEPAD_DPAD_DOWN;
  if (bind_pressed(REXCVAR_GET(input_bind_dpad_left)))
    buttons |= X_INPUT_GAMEPAD_DPAD_LEFT;
  if (bind_pressed(REXCVAR_GET(input_bind_dpad_right)))
    buttons |= X_INPUT_GAMEPAD_DPAD_RIGHT;

  uint8_t lt = bind_pressed(REXCVAR_GET(input_bind_left_trigger)) ? 0xFF : 0;
  uint8_t rt = bind_pressed(REXCVAR_GET(input_bind_right_trigger)) ? 0xFF : 0;

  int32_t lx = 0;
  int32_t ly = 0;
  if (bind_pressed(REXCVAR_GET(input_bind_lstick_left)))
    lx -= INT16_MAX;
  if (bind_pressed(REXCVAR_GET(input_bind_lstick_right)))
    lx += INT16_MAX;
  if (bind_pressed(REXCVAR_GET(input_bind_lstick_up)))
    ly += INT16_MAX;
  if (bind_pressed(REXCVAR_GET(input_bind_lstick_down)))
    ly -= INT16_MAX;

  // Native mouse look leaves the counts for ConsumeMouseLook; the right stick
  // then carries no mouse motion.
  mouse_motion_policy::RightStick mouse_stick{};
  if (!MouseLookNative()) {
    // The title reads the pad from several methods per frame; the counts
    // become a deflection once and every poll within the hold window sees
    // the same deflection instead of the first poll draining it.
    const uint64_t stick_now_ms = TestTapClockMs();
    if (!mouse_stick_held_ || stick_now_ms - mouse_stick_ms_ >= kMouseStickHoldMs) {
      if (!has_focus_) {
        mouse_dx_ = 0;  // only scripted counts drive an unfocused window
        mouse_dy_ = 0;
      }
      mouse_dx_ = mouse_motion_policy::AccumulateEventDelta(mouse_dx_, script_mouse_dx_, 0.0);
      mouse_dy_ = mouse_motion_policy::AccumulateEventDelta(mouse_dy_, script_mouse_dy_, 0.0);
      script_mouse_dx_ = 0;
      script_mouse_dy_ = 0;
      const double mouse_smoothing = REXCVAR_GET(input_mouse_smoothing);
      const int32_t smoothed_mouse_dx =
          mouse_smoothing_x_.Apply(mouse_dx_, mouse_smoothing);
      const int32_t smoothed_mouse_dy =
          mouse_smoothing_y_.Apply(mouse_dy_, mouse_smoothing);
      mouse_stick_ = mouse_motion_policy::Translate(
          smoothed_mouse_dx, smoothed_mouse_dy,
          REXCVAR_GET(input_mouse_sensitivity),
          REXCVAR_GET(input_mouse_invert_y));
      mouse_stick_ms_ = stick_now_ms;
      mouse_stick_held_ = true;
      mouse_dx_ = 0;
      mouse_dy_ = 0;
    }
    mouse_stick = mouse_stick_;
  }

  auto clamp16 = [](int32_t v) -> int16_t {
    return static_cast<int16_t>(std::clamp(v, (int32_t)INT16_MIN, (int32_t)INT16_MAX));
  };

  // Scripted thumbsticks (test runs only) merge like a second pad: the larger
  // magnitude wins per axis.
  const auto merge_axis = [](int32_t a, int32_t b) { return std::abs(a) >= std::abs(b) ? a : b; };
  int32_t script_rx = 0;
  int32_t script_ry = 0;
  if (!test_script_path_.empty() && scripted) {
    const auto& sticks = test_script_.sticks();
    lx = merge_axis(lx, sticks[0]);
    ly = merge_axis(ly, sticks[1]);
    script_rx = sticks[2];
    script_ry = sticks[3];
  }
  const int16_t state_lx = clamp16(lx);
  const int16_t state_ly = clamp16(ly);
  int16_t state_rx = clamp16(merge_axis(mouse_stick.x, script_rx));
  int16_t state_ry = clamp16(merge_axis(mouse_stick.y, script_ry));
  if (test_taps_enabled && REXCVAR_GET(input_test_camera_keys)) {
    state_rx = test_tap_policy::CameraAxis(true, state_rx,
        bind_pressed("J"), bind_pressed("L"));
    state_ry = test_tap_policy::CameraAxis(true, state_ry,
        bind_pressed("K"), bind_pressed("I"));
  }
  state_policy::Publish(
      {buttons, lt, rt, state_lx, state_ly, state_rx, state_ry},
      published_state_, has_published_state_, packet_number_);

  if (out_state) {
    out_state->packet_number = packet_number_;
    out_state->gamepad.buttons = buttons;
    out_state->gamepad.left_trigger = lt;
    out_state->gamepad.right_trigger = rt;
    out_state->gamepad.thumb_lx = state_lx;
    out_state->gamepad.thumb_ly = state_ly;
    out_state->gamepad.thumb_rx = state_rx;
    out_state->gamepad.thumb_ry = state_ry;
  }
  return X_ERROR_SUCCESS;
}

X_RESULT MnkInputDriver::SetState(uint32_t user_index, X_INPUT_VIBRATION* vibration) {
  if (!IsEnabled() || user_index != UserIndex()) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }
  return X_ERROR_SUCCESS;
}

X_RESULT MnkInputDriver::GetKeystroke(uint32_t user_index, uint32_t flags,
                                      X_INPUT_KEYSTROKE* out_keystroke) {
  if (!IsEnabled() || user_index != UserIndex()) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }
  std::lock_guard lock(state_mutex_);
  if (keystroke_queue_.empty()) {
    return X_ERROR_EMPTY;
  }
  if (out_keystroke) {
    *out_keystroke = keystroke_queue_.front();
  }
  keystroke_queue_.pop();
  return X_ERROR_SUCCESS;
}

void MnkInputDriver::EnqueueKeystroke(uint16_t vk_pad, bool down) {
  X_INPUT_KEYSTROKE ks = {};
  ks.virtual_key = vk_pad;
  ks.unicode = 0;
  ks.flags = down ? X_INPUT_KEYSTROKE_KEYDOWN : X_INPUT_KEYSTROKE_KEYUP;
  ks.user_index = static_cast<uint8_t>(UserIndex());
  ks.hid_code = 0;
  keystroke_queue_.push(ks);
}

void MnkInputDriver::CenterCursor() {
  if (!attached_window_)
    return;
  int32_t cx = static_cast<int32_t>(attached_window_->GetActualLogicalWidth() / 2);
  int32_t cy = static_cast<int32_t>(attached_window_->GetActualLogicalHeight() / 2);
  prev_mouse_x_ = cx;
  prev_mouse_y_ = cy;
#if REX_PLATFORM_WIN32
  HWND hwnd = static_cast<HWND>(attached_window_->GetNativeWindowHandle());
  if (hwnd) {
    POINT pt = {static_cast<LONG>(cx), static_cast<LONG>(cy)};
    ClientToScreen(hwnd, &pt);
    SetCursorPos(pt.x, pt.y);
  }
#endif
}

void MnkInputDriver::UpdateMouseCapture() {
  if (!attached_window_)
    return;

  bool should_capture = IsEnabled() && has_focus_ && is_active();

  if (should_capture && !mouse_captured_) {
    mouse_captured_ = true;
    precapture_cursor_visibility_ = attached_window_->GetCursorVisibility();
    attached_window_->SetCursorVisibility(rex::ui::Window::CursorVisibility::kHidden);
    attached_window_->CaptureMouse();
    relative_mouse_mode_ = attached_window_->SetRelativeMouseMode(true);
    // Reset deltas to avoid a spike on capture start
    mouse_dx_ = 0;
    mouse_dy_ = 0;
    mouse_smoothing_x_.Reset();
    mouse_smoothing_y_.Reset();
  } else if (!should_capture && mouse_captured_) {
    if (relative_mouse_mode_) {
      attached_window_->SetRelativeMouseMode(false);
      relative_mouse_mode_ = false;
    }
    mouse_captured_ = false;
    mouse_smoothing_x_.Reset();
    mouse_smoothing_y_.Reset();
    attached_window_->SetCursorVisibility(precapture_cursor_visibility_);
    attached_window_->ReleaseMouse();
  }

  // Re-center cursor each frame while captured to prevent edge clamping
  if (mouse_captured_ && !relative_mouse_mode_) {
    CenterCursor();
  }
}

void MnkInputDriver::SetKeyState(uint16_t vk, bool down) {
  if (vk < 256) {
    key_down_[vk] = down;
  }
}

void MnkInputDriver::OnKeyDown(rex::ui::KeyEvent& e) {
  if (!IsEnabled())
    return;
  std::lock_guard lock(state_mutex_);
  if (!has_focus_ || !is_active()) return;
  uint16_t vk = static_cast<uint16_t>(e.virtual_key());
  const uint32_t hold_ms = REXCVAR_GET(input_test_tap_hold_ms);
  if (hold_ms && vk < 256 && !key_down_[vk]) {
    test_taps_.KeyDown(vk, hold_ms, TestTapClockMs());
    if (test_tap_log_count_ < 64) {
      ++test_tap_log_count_;
      std::fprintf(stderr, "REX_INPUT_TEST_TAP source=window_keydown key=%u hold_ms=%u focus=1\n",
                   vk, vk == 0x1B ? 0 : hold_ms);
    }
  }
  SetKeyState(vk, true);
}

void MnkInputDriver::OnKeyUp(rex::ui::KeyEvent& e) {
  if (!IsEnabled())
    return;
  std::lock_guard lock(state_mutex_);
  uint16_t vk = static_cast<uint16_t>(e.virtual_key());
  SetKeyState(vk, false);
}

void MnkInputDriver::OnMouseDown(rex::ui::MouseEvent& e) {
  if (!IsEnabled() || !has_focus_)
    return;
  std::lock_guard lock(state_mutex_);
  const VirtualKey key = mouse_button_policy::ToVirtualKey(e.button());
  if (key != VirtualKey::kNone) {
    SetKeyState(static_cast<uint16_t>(key), true);
  }
}

void MnkInputDriver::OnMouseUp(rex::ui::MouseEvent& e) {
  if (!IsEnabled())
    return;
  std::lock_guard lock(state_mutex_);
  const VirtualKey key = mouse_button_policy::ToVirtualKey(e.button());
  if (key != VirtualKey::kNone) {
    SetKeyState(static_cast<uint16_t>(key), false);
  }
}

void MnkInputDriver::OnMouseMove(rex::ui::MouseEvent& e) {
  if (!IsEnabled() || !has_focus_)
    return;
  std::lock_guard lock(state_mutex_);
  // Native mouse look takes raw counts; acceleration belongs to the stick bridge.
  const double acceleration =
      MouseLookNative() ? 0.0 : REXCVAR_GET(input_mouse_acceleration);
  if (relative_mouse_mode_) {
    mouse_dx_ = mouse_motion_policy::AccumulateEventDelta(
        mouse_dx_, e.delta_x(), acceleration);
    mouse_dy_ = mouse_motion_policy::AccumulateEventDelta(
        mouse_dy_, e.delta_y(), acceleration);
  } else {
    int32_t x = e.x();
    int32_t y = e.y();
    mouse_dx_ = mouse_motion_policy::AccumulateEventDelta(
        mouse_dx_, x - prev_mouse_x_, acceleration);
    mouse_dy_ = mouse_motion_policy::AccumulateEventDelta(
        mouse_dy_, y - prev_mouse_y_, acceleration);
    prev_mouse_x_ = x;
    prev_mouse_y_ = y;
  }
}

void MnkInputDriver::OnMouseWheel(rex::ui::MouseEvent& e) {
  if (!IsEnabled() || !has_focus_)
    return;
  std::lock_guard lock(state_mutex_);
  mouse_wheel_pulses_.PushScrollY(e.scroll_y());
}

void MnkInputDriver::OnLostFocus(rex::ui::UISetupEvent&) {
  std::lock_guard lock(state_mutex_);
  has_focus_ = false;
  test_taps_.Reset();
  std::memset(key_down_, 0, sizeof(key_down_));
  mouse_dx_ = 0;
  mouse_dy_ = 0;
  mouse_smoothing_x_.Reset();
  mouse_smoothing_y_.Reset();
  mouse_wheel_pulses_.Clear();
  if (has_published_state_) {
    state_policy::Publish({}, published_state_, has_published_state_,
                          packet_number_);
  }
  if (mouse_captured_ && attached_window_) {
    if (relative_mouse_mode_) {
      attached_window_->SetRelativeMouseMode(false);
      relative_mouse_mode_ = false;
    }
    mouse_captured_ = false;
    attached_window_->SetCursorVisibility(precapture_cursor_visibility_);
    attached_window_->ReleaseMouse();
  }
}

void MnkInputDriver::OnGotFocus(rex::ui::UISetupEvent&) {
  std::lock_guard lock(state_mutex_);
  has_focus_ = true;
}

}  // namespace rex::input::mnk
