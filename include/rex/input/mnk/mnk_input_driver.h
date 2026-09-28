/**
 * @file        rex/input/mnk/mnk_input_driver.h
 * @brief       Keyboard/mouse input driver - maps MnK to Xbox 360 controller.
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#pragma once

#include <rex/input/input_driver.h>
#include <rex/input/mnk/mnk_state_policy.h>
#include <rex/input/mnk/mouse_motion_policy.h>
#include <rex/input/mnk/mouse_wheel_policy.h>
#include <rex/input/mnk/test_script_policy.h>
#include <rex/input/mnk/test_tap_policy.h>
#include <rex/ui/window.h>
#include <rex/ui/window_listener.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <queue>
#include <string>

namespace rex::input::mnk {

class MnkInputDriver final : public InputDriver,
                             public rex::ui::WindowInputListener,
                             public rex::ui::WindowListener {
 public:
  explicit MnkInputDriver(rex::ui::Window* window, size_t window_z_order);
  ~MnkInputDriver() override;

  X_STATUS Setup() override;

  X_RESULT GetCapabilities(uint32_t user_index, uint32_t flags,
                           X_INPUT_CAPABILITIES* out_caps) override;
  X_RESULT GetState(uint32_t user_index, X_INPUT_STATE* out_state) override;
  X_RESULT SetState(uint32_t user_index, X_INPUT_VIBRATION* vibration) override;
  X_RESULT GetKeystroke(uint32_t user_index, uint32_t flags,
                        X_INPUT_KEYSTROKE* out_keystroke) override;

  void OnWindowAvailable(rex::ui::Window* window) override;

  // WindowInputListener
  void OnKeyDown(rex::ui::KeyEvent& e) override;
  void OnKeyUp(rex::ui::KeyEvent& e) override;
  void OnMouseDown(rex::ui::MouseEvent& e) override;
  void OnMouseUp(rex::ui::MouseEvent& e) override;
  void OnMouseMove(rex::ui::MouseEvent& e) override;
  void OnMouseWheel(rex::ui::MouseEvent& e) override;

  // WindowListener
  void OnClosing(rex::ui::UIEvent& e) override;
  void OnLostFocus(rex::ui::UISetupEvent& e) override;
  void OnGotFocus(rex::ui::UISetupEvent& e) override;

  uint32_t UserIndex() const;
  bool IsEnabled() const;

  // Native mouse look (input_mouse_look = "native"): mouse motion is not
  // translated to the right stick; the host takes the raw relative counts
  // accumulated since its previous call and feeds them to the title's own
  // look command. Returns false (and zero counts) while keyboard/mouse is
  // disabled, unfocused, inactive, not captured or in stick mode.
  static bool MouseLookNative();
  static double MouseSensitivity();
  static bool MouseInvertY();
  bool ConsumeMouseLook(uint32_t user_index, int32_t& dx, int32_t& dy);

  // Developer test input: the window owner delivers the script's host
  // events (press <key>, click <x> <y>) to its window on the UI thread.
  using HostEventCallback = std::function<void(const test_script_policy::Command&)>;
  void set_host_event_callback(HostEventCallback callback) {
    std::lock_guard lock(state_mutex_);
    host_event_callback_ = std::move(callback);
  }
  // Developer test input: the script's virtual controller buttons (`pad`),
  // 0 without a driving script. Any thread.
  uint16_t TestPadButtons() const { return test_pad_buttons_.load(std::memory_order_acquire); }

 private:
  void CenterCursor();
  void UpdateMouseCapture();
  void SetKeyState(uint16_t vk, bool down);
  void EnqueueKeystroke(uint16_t vk_pad, bool down);
  // Developer test input (input_test_script; empty = off, see
  // test_script_policy.h): tailed and advanced at guest polls under
  // state_mutex_. Driving = enabled and not yielded to local input.
  void PumpTestScriptLocked();
  bool TestScriptDrivingLocked() const;

  rex::ui::Window* attached_window_ = nullptr;

  std::mutex state_mutex_;
  bool key_down_[256] = {};
  test_tap_policy::BoundedTaps test_taps_{};
  uint32_t test_tap_log_count_ = 0;
  std::string test_script_path_;
  uint64_t test_script_offset_ = 0;
  uint64_t test_script_next_read_ms_ = 0;
  test_script_policy::Runner test_script_{};
  test_script_policy::Runner::Keys script_keys_{};
  // Scripted counts stay apart from the physical accumulators, which focus
  // and capture changes reset.
  int32_t script_mouse_dx_ = 0;
  int32_t script_mouse_dy_ = 0;
  uint32_t test_script_log_count_ = 0;
  HostEventCallback host_event_callback_;
  std::atomic<uint16_t> test_pad_buttons_{0};

  // Mouse delta tracking
  int32_t mouse_dx_ = 0;
  int32_t mouse_dy_ = 0;
  mouse_motion_policy::TwoSampleSmoother mouse_smoothing_x_{};
  mouse_motion_policy::TwoSampleSmoother mouse_smoothing_y_{};
  // Stick bridge: the deflection computed at a poll, held for every poll in
  // the same frame (see GetState).
  mouse_motion_policy::RightStick mouse_stick_{};
  uint64_t mouse_stick_ms_ = 0;
  bool mouse_stick_held_ = false;
  uint64_t test_script_polls_ = 0;
  uint64_t test_script_poll_start_ms_ = 0;
  mouse_wheel_policy::PulseQueue mouse_wheel_pulses_{};
  int32_t prev_mouse_x_ = 0;
  int32_t prev_mouse_y_ = 0;
  bool mouse_captured_ = false;
  bool relative_mouse_mode_ = false;
  // Cursor visibility to restore on capture release - the window owner may run
  // an auto-hide policy that capture must not permanently override.
  rex::ui::Window::CursorVisibility precapture_cursor_visibility_ =
      rex::ui::Window::CursorVisibility::kVisible;
  // False until the attached window's actual state is adopted. The embedded
  // title attaches after Window::Open and therefore doesn't receive the
  // initial focus event through WindowListener.
  bool has_focus_ = false;

  // Keystroke queue
  std::queue<X_INPUT_KEYSTROKE> keystroke_queue_;

  // Packet number incremented on state change
  uint32_t packet_number_ = 0;
  bool has_published_state_ = false;
  state_policy::ControllerState published_state_{};
};

}  // namespace rex::input::mnk
