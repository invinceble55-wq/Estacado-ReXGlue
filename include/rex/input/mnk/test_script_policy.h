#pragma once

#include <array>
#include <charconv>
#include <cstdint>
#include <deque>
#include <string>
#include <string_view>
#include <vector>

namespace rex::input::mnk::test_script_policy {
// Opt-in developer test input for isolated automated runs (cvar
// input_test_script = path of a command file; empty by default and never set
// by packaged presets). The driver tails the file while the title runs and
// feeds the commands into the same state as the physical keyboard and mouse:
// bound key names and relative mouse counts, so the title sees exactly what a
// player's devices would produce, independent of window focus. It never
// synthesizes system input. Local input wins: real input anywhere on the
// machine yields the script (keys released, queue dropped) until an explicit
// `arm` line is appended.
//
// Commands, one per line ('#' starts a comment):
//   wait <ms>               nothing for ms
//   down <key> / up <key>   press / release a key by bind name (Space, W, LMB...)
//   tap <key> <ms>          press, hold ms, release
//   mouse <dx> <dy> [<ms>]  relative counts, spread evenly over ms (0: at once)
//   mark <text>             marker line in the log
//   arm                     re-enable after a yield (ignored otherwise)
//   press <key>             a key press delivered to the game window itself
//                           (host keys such as the settings overlay key)
//   click <x> <y>           a left click at window client pixel x, y
//   stick <L|R> <x> <y> [<jitter>]
//                           hold a thumbstick deflection in XInput units
//                           (-32767..32767, up = +y; `0 0` releases). A
//                           jitter adds a different offset of at most that
//                           many units at every guest poll, like a physical
//                           stick that never reports the same value twice.
//                           Scripted and physical axes merge by the larger
//                           magnitude, like two pads on one user.
//   pad <hex>               hold raw XInput buttons (wButtons bits, e.g.
//                           0030 = Back + Start; `0` releases) on a virtual
//                           controller that the host treats like a physical
//                           one: the settings overlay's chord and navigation
//                           and the runtime's native controller path.
enum class Op : uint8_t {
  kWait, kDown, kUp, kTap, kMouse, kMark, kArm, kPress, kClick, kStick, kPad
};

inline const char* OpName(Op op) {
  switch (op) {
    case Op::kWait: return "wait";
    case Op::kDown: return "down";
    case Op::kUp: return "up";
    case Op::kTap: return "tap";
    case Op::kMouse: return "mouse";
    case Op::kMark: return "mark";
    case Op::kArm: return "arm";
    case Op::kPress: return "press";
    case Op::kClick: return "click";
    case Op::kStick: return "stick";
    case Op::kPad: return "pad";
  }
  return "?";
}

struct Command {
  Op op = Op::kWait;
  uint16_t key = 0;
  int32_t dx = 0;
  int32_t dy = 0;
  uint32_t ms = 0;
  std::string text;
  uint32_t line = 0;
};

// Key name -> key index below 256, 0 when unknown.
using KeyParser = uint16_t (*)(std::string_view);

inline constexpr uint32_t kMaxDurationMs = 600000;
inline constexpr int32_t kMaxCounts = 1000000;
inline constexpr uint64_t kMaxCatchUpMs = 50;
inline constexpr int32_t kMaxStick = 32767;
inline constexpr uint32_t kMaxStickJitter = 4096;

namespace detail {
inline std::string_view Trim(std::string_view text) {
  while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) text.remove_prefix(1);
  while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r'))
    text.remove_suffix(1);
  return text;
}

inline std::vector<std::string_view> Split(std::string_view text) {
  std::vector<std::string_view> words;
  size_t index = 0;
  while (index < text.size()) {
    while (index < text.size() && (text[index] == ' ' || text[index] == '\t')) ++index;
    const size_t start = index;
    while (index < text.size() && text[index] != ' ' && text[index] != '\t') ++index;
    if (index > start) words.push_back(text.substr(start, index - start));
  }
  return words;
}

template <typename T>
bool Number(std::string_view text, T& value, T minimum, T maximum) {
  T parsed{};
  const auto result = std::from_chars(text.data(), text.data() + text.size(), parsed);
  if (result.ec != std::errc() || result.ptr != text.data() + text.size()) return false;
  if (parsed < minimum || parsed > maximum) return false;
  value = parsed;
  return true;
}
}  // namespace detail

enum class ParseResult { kCommand, kEmpty, kError };

inline ParseResult ParseLine(std::string_view line, KeyParser parse_key, Command& out,
                             std::string& error) {
  const size_t comment = line.find('#');
  if (comment != std::string_view::npos) line = line.substr(0, comment);
  line = detail::Trim(line);
  if (line.empty()) return ParseResult::kEmpty;
  const auto words = detail::Split(line);
  const std::string_view verb = words[0];
  Command command;
  const auto key = [&](size_t index) {
    if (index >= words.size() || !parse_key) return false;
    command.key = parse_key(words[index]);
    return command.key != 0 && command.key < 256;
  };
  const auto ms = [&](size_t index) {
    return index < words.size() &&
           detail::Number<uint32_t>(words[index], command.ms, 0, kMaxDurationMs);
  };
  bool valid = false;
  if (verb == "wait") {
    command.op = Op::kWait;
    valid = words.size() == 2 && ms(1);
  } else if (verb == "down" || verb == "up") {
    command.op = verb == "down" ? Op::kDown : Op::kUp;
    valid = words.size() == 2 && key(1);
  } else if (verb == "tap") {
    command.op = Op::kTap;
    valid = words.size() == 3 && key(1) && ms(2);
  } else if (verb == "mouse") {
    command.op = Op::kMouse;
    valid = (words.size() == 3 || words.size() == 4) &&
            detail::Number<int32_t>(words[1], command.dx, -kMaxCounts, kMaxCounts) &&
            detail::Number<int32_t>(words[2], command.dy, -kMaxCounts, kMaxCounts) &&
            (words.size() == 3 || ms(3));
  } else if (verb == "mark") {
    command.op = Op::kMark;
    command.text = std::string(detail::Trim(line.substr(verb.size())));
    valid = true;
  } else if (verb == "arm") {
    command.op = Op::kArm;
    valid = words.size() == 1;
  } else if (verb == "press") {
    command.op = Op::kPress;
    valid = words.size() == 2 && key(1);
  } else if (verb == "click") {
    command.op = Op::kClick;
    valid = words.size() == 3 &&
            detail::Number<int32_t>(words[1], command.dx, 0, 16384) &&
            detail::Number<int32_t>(words[2], command.dy, 0, 16384);
  } else if (verb == "stick") {
    // key = 0 left / 1 right, dx/dy = deflection, ms = jitter.
    command.op = Op::kStick;
    valid = (words.size() == 4 || words.size() == 5) &&
            (words[1] == "L" || words[1] == "R") &&
            detail::Number<int32_t>(words[2], command.dx, -kMaxStick, kMaxStick) &&
            detail::Number<int32_t>(words[3], command.dy, -kMaxStick, kMaxStick) &&
            (words.size() == 4 ||
             detail::Number<uint32_t>(words[4], command.ms, 0, kMaxStickJitter));
    command.key = words.size() > 1 && words[1] == "R" ? 1 : 0;
  } else if (verb == "pad") {
    // dx = the held wButtons bits.
    command.op = Op::kPad;
    uint32_t buttons = 0;
    const std::string_view hex = words.size() == 2 ? words[1] : std::string_view();
    const auto result = std::from_chars(hex.data(), hex.data() + hex.size(), buttons, 16);
    valid = !hex.empty() && result.ec == std::errc() && result.ptr == hex.data() + hex.size() &&
            buttons <= 0xFFFFu;
    command.dx = int32_t(buttons);
  }
  if (!valid) {
    error = "invalid test input command: " + std::string(line);
    return ParseResult::kError;
  }
  out = std::move(command);
  return ParseResult::kCommand;
}

class Runner {
 public:
  using Keys = std::array<bool, 256>;
  // Scripted thumbsticks: left x, left y, right x, right y.
  using Sticks = std::array<int16_t, 4>;

  // Appends newly read file bytes; complete lines are parsed and queued.
  // Invalid lines are reported and skipped. While yielded, everything before
  // an `arm` line is dropped.
  void Append(std::string_view bytes, KeyParser parse_key, std::vector<std::string>& errors) {
    if (!bom_checked_) {
      partial_.append(bytes);
      if (partial_.size() < 3 && std::string_view("\xEF\xBB\xBF").substr(0, partial_.size()) ==
                                     std::string_view(partial_)) {
        return;
      }
      if (partial_.compare(0, 3, "\xEF\xBB\xBF") == 0) partial_.erase(0, 3);
      bom_checked_ = true;
    } else {
      partial_.append(bytes);
    }
    size_t start = 0;
    for (;;) {
      const size_t end = partial_.find('\n', start);
      if (end == std::string::npos) break;
      const std::string_view text(partial_.data() + start, end - start);
      Command command;
      std::string error;
      const ParseResult result = ParseLine(text, parse_key, command, error);
      command.line = next_line_++;
      if (result == ParseResult::kError) {
        errors.push_back("line " + std::to_string(command.line) + ": " + error);
      } else if (result == ParseResult::kCommand) {
        if (yielded_) {
          if (command.op == Op::kArm) {
            yielded_ = false;
            idle_ = true;
          }
        } else if (command.op != Op::kArm) {
          queue_.push_back(std::move(command));
        }
      }
      start = end + 1;
    }
    partial_.erase(0, start);
  }

  // Advances script time to now_ms at one guest poll: key flags change in
  // `keys`, mouse counts are added to dx/dy, and on_start(command) runs as
  // each command starts. Commands chain back to back; after a stall (no polls
  // during loading) they resume at most kMaxCatchUpMs late instead of
  // bursting, and a tap is always released at a later poll than its press so
  // the title sees it.
  template <typename OnStart>
  void Advance(uint64_t now_ms, Keys& keys, int32_t& dx, int32_t& dy, OnStart&& on_start) {
    if (yielded_) return;
    ++poll_;
    AdvanceInner(now_ms, keys, dx, dy, on_start);
    UpdateSticks();
  }

  // Local input took over: release every scripted key and drop the queue.
  void YieldToLocalInput(Keys& keys) {
    keys.fill(false);
    queue_.clear();
    active_ = false;
    yielded_ = true;
    stick_base_.fill(0);
    stick_jitter_.fill(0);
    sticks_.fill(0);
    pad_buttons_ = 0;
  }

  bool yielded() const { return yielded_; }
  // A held scripted stick or pad keeps the script driving (local input still
  // yields).
  bool idle() const {
    return !active_ && queue_.empty() && !stick_base_[0] && !stick_base_[1] &&
           !stick_base_[2] && !stick_base_[3] && !pad_buttons_;
  }
  // Deflections for the current poll (jitter already applied).
  const Sticks& sticks() const { return sticks_; }
  // The virtual controller's held buttons (`pad`).
  uint16_t pad_buttons() const { return pad_buttons_; }

 private:
  template <typename OnStart>
  void AdvanceInner(uint64_t now_ms, Keys& keys, int32_t& dx, int32_t& dy, OnStart& on_start) {
    for (;;) {
      if (!active_) {
        if (queue_.empty()) {
          idle_ = true;
          return;
        }
        current_ = std::move(queue_.front());
        queue_.pop_front();
        active_ = true;
        const uint64_t earliest = now_ms > kMaxCatchUpMs ? now_ms - kMaxCatchUpMs : 0;
        start_ms_ = idle_ ? now_ms : (cursor_ms_ > earliest ? cursor_ms_ : earliest);
        idle_ = false;
        sent_dx_ = 0;
        sent_dy_ = 0;
        on_start(current_);
        switch (current_.op) {
          case Op::kDown:
          case Op::kTap:
            keys[current_.key] = true;
            pressed_poll_ = poll_;
            break;
          case Op::kUp:
            keys[current_.key] = false;
            break;
          case Op::kStick:
            stick_base_[current_.key * 2] = current_.dx;
            stick_base_[current_.key * 2 + 1] = current_.dy;
            stick_jitter_[current_.key] = current_.ms;
            break;
          case Op::kPad:
            pad_buttons_ = uint16_t(current_.dx);
            break;
          default:
            break;
        }
      }
      const bool timed = current_.op == Op::kWait || current_.op == Op::kTap ||
                         current_.op == Op::kMouse;
      const uint64_t end_ms = start_ms_ + (timed ? current_.ms : 0);
      if (current_.op == Op::kMouse) {
        const uint64_t elapsed = now_ms > start_ms_ ? now_ms - start_ms_ : 0;
        const bool done = current_.ms == 0 || elapsed >= current_.ms;
        const int32_t target_x =
            done ? current_.dx
                 : int32_t(int64_t(current_.dx) * int64_t(elapsed) / int64_t(current_.ms));
        const int32_t target_y =
            done ? current_.dy
                 : int32_t(int64_t(current_.dy) * int64_t(elapsed) / int64_t(current_.ms));
        dx += target_x - sent_dx_;
        dy += target_y - sent_dy_;
        sent_dx_ = target_x;
        sent_dy_ = target_y;
      }
      if (now_ms < end_ms) return;
      if (current_.op == Op::kTap) {
        if (pressed_poll_ == poll_) return;  // let one poll see the press
        keys[current_.key] = false;
      }
      cursor_ms_ = end_ms;
      active_ = false;
    }
  }

  void UpdateSticks() {
    for (size_t axis = 0; axis < 4; ++axis) {
      int32_t value = stick_base_[axis];
      const size_t side = axis / 2;
      const uint32_t jitter = stick_jitter_[side];
      if (jitter && (stick_base_[side * 2] || stick_base_[side * 2 + 1])) {
        // Deterministic LCG: runs stay reproducible.
        noise_ = noise_ * 1664525u + 1013904223u;
        value += int32_t((noise_ >> 8) % (2 * jitter + 1)) - int32_t(jitter);
      }
      sticks_[axis] = int16_t(value < -kMaxStick ? -kMaxStick : value > kMaxStick ? kMaxStick : value);
    }
  }

  std::string partial_;
  bool bom_checked_ = false;
  std::deque<Command> queue_;
  Command current_;
  bool active_ = false;
  bool idle_ = true;
  bool yielded_ = false;
  uint64_t start_ms_ = 0;
  uint64_t cursor_ms_ = 0;
  uint64_t poll_ = 0;
  uint64_t pressed_poll_ = 0;
  int32_t sent_dx_ = 0;
  int32_t sent_dy_ = 0;
  uint32_t next_line_ = 1;
  std::array<int32_t, 4> stick_base_{};
  std::array<uint32_t, 2> stick_jitter_{};
  Sticks sticks_{};
  uint16_t pad_buttons_ = 0;
  uint32_t noise_ = 0x9E3779B9u;
};
}  // namespace rex::input::mnk::test_script_policy
