/**
 * @file        ui/frame_latency.cpp
 * @brief       Guest frame identity, the low-latency frame-queue limit and
 *              the input-to-present measurement
 *
 * @license     BSD 3-Clause License
 */

#include <rex/ui/frame_latency.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <mutex>

#include <rex/cvar.h>
#if REX_HAS_D3D12
#include <rex/ui/d3d12/streamline_bridge.h>
#endif

REXCVAR_DEFINE_INT32(display_low_latency_queue, 0, "Display",
                     "Low latency: the title starts a frame only once the frame this many frames "
                     "before it has been presented (0 off, 1 to 3; at most 50 ms per wait)")
    .range(0, 3);
REXCVAR_DEFINE_INT32(display_low_latency_lead_us, 0, "Display",
                     "Low latency: end the frame-queue wait this many microseconds before the "
                     "awaited frame's expected present (0 = wait for the present itself)")
    .range(0, 20000);
// The player setting (display.low_latency): on keeps at most two frames
// between the title and the display, the wait ending half a frame before the
// awaited present so the frame rate holds (measured V489, VSync 144 Hz:
// input to present 24.9 -> 16.8 ms at 144 FPS; at 60 FPS the queue never
// fills, so nothing changes there). The two developer switches above
// override it when set; DLSS Frame Generation keeps its own pacing.
REXCVAR_DEFINE_STRING(display_low_latency, "on", "Display",
                      "Low latency: on keeps at most two frames between the game and the display "
                      "(the wait ends half a frame early); off lets the frame queue fill")
    .allowed({"on", "off"});
REXCVAR_DEFINE_BOOL(frame_latency_report, false, "Display",
                    "Developer: input-to-present and frame pairing lines on stderr every 1200 "
                    "frames");
#if REX_HAS_D3D12
REXCVAR_DECLARE(std::string, display_present_mode);
#endif

namespace rex::ui::frame_latency {
namespace {

using Clock = std::chrono::steady_clock;
constexpr uint32_t kReportFrames = 1200;
constexpr auto kQueueWaitLimit = std::chrono::milliseconds(50);

// PC Latency markers (sl::PCLMarker values; NVIDIA Streamline when active).
enum class Marker : uint32_t {
  kSimulationStart = 0,
  kSimulationEnd = 1,
  kRenderSubmitStart = 2,
  kRenderSubmitEnd = 3,
  kPresentStart = 4,
  kPresentEnd = 5,
  kLatencyPing = 8,
  kControllerInputSample = 13,
};

void Mark(Marker marker, uint32_t frame) {
#if REX_HAS_D3D12
  d3d12::streamline::SetMarker(d3d12::streamline::Marker(uint32_t(marker)), frame);
#else
  (void)marker;
  (void)frame;
#endif
}

bool StreamlineActive() {
#if REX_HAS_D3D12
  return d3d12::streamline::IsActive();
#else
  return false;
#endif
}

bool FrameGenerationActive() {
#if REX_HAS_D3D12
  return d3d12::streamline::FrameGenerationActive();
#else
  return false;
#endif
}

// Whether the display paces the presents (VSync). With immediate or VRR
// presentation nothing waits on the display, so the setting's limit would
// only cost frame rate (measured V490, uncapped, VSync off: 200.7 -> 168.0
// FPS for 0.4 ms less input to present).
bool DisplayPacesPresents() {
#if REX_HAS_D3D12
  return REXCVAR_GET(display_present_mode) == "vsync";
#else
  return true;
#endif
}

// Whether the frame-queue limit comes from the player setting rather than
// the developer switches (which win when set) or Frame Generation's pacing.
bool SettingDrivesQueue() {
  return REXCVAR_GET(display_low_latency_queue) <= 0 &&
         REXCVAR_GET(display_low_latency_lead_us) <= 0 && !FrameGenerationActive() &&
         REXCVAR_GET(display_low_latency) == "on" && DisplayPacesPresents();
}

// The frame-queue limit in effect (0 off).
int32_t EffectiveQueue() {
  return SettingDrivesQueue() ? 2 : REXCVAR_GET(display_low_latency_queue);
}

int64_t NowUs() {
  return std::chrono::duration_cast<std::chrono::microseconds>(Clock::now().time_since_epoch())
      .count();
}

// Frame ids in order, oldest first (0 = a swap without a tracked frame).
template <uint32_t kCapacity>
class FrameQueue {
 public:
  // Appends; when full, drops the oldest and returns it (otherwise 0).
  uint32_t Push(uint32_t frame) {
    const uint32_t dropped = count_ == kCapacity ? Pop() : 0;
    frames_[(first_ + count_++) % kCapacity] = frame;
    return dropped;
  }
  // The oldest frame, 0 when empty.
  uint32_t Pop() {
    if (!count_) return 0;
    const uint32_t frame = frames_[first_];
    first_ = (first_ + 1) % kCapacity;
    --count_;
    return frame;
  }
  uint32_t size() const { return count_; }

 private:
  std::array<uint32_t, kCapacity> frames_{};
  uint32_t first_ = 0;
  uint32_t count_ = 0;
};

// Tracking starts when anything needs it and then stays on, so the queues
// below stay paired across the threads.
std::atomic<bool> tracking{false};
std::atomic<uint32_t> next_frame{0};
// The frame the producer is simulating (from its start to the next frame
// start or its guest swap, whichever comes first).
std::atomic<uint32_t> open_simulation{0};
// The title builds a frame on its producer thread and its render thread swaps
// the frames in the same order, up to a few frames later (both may be one
// thread): `built` holds built frames awaiting their guest swap (a few at
// most; more means one was never swapped), `swapped` the guest swaps awaiting
// the command processor's swap.
std::mutex frame_mutex;
FrameQueue<4> built;
FrameQueue<32> swapped;
std::atomic<uint32_t> last_built{0};
// Producer thread.
uint32_t built_frame = 0;
uint32_t sampled_frame = 0;
uint32_t frames_until_report = kReportFrames;
// Command processor thread.
uint32_t render_frame = 0;
bool render_open = false;
// Presenter.
std::atomic<uint32_t> last_present{0};
std::atomic<bool> in_menu{false};
// The newest frame whose present returned (the queue limit waits on it).
std::mutex present_mutex;
std::condition_variable present_condition;
uint32_t presented_frame = 0;
// When the newest present returned, and the average interval between
// consecutive presents (for the lead of the queue wait).
int64_t presented_time_us = 0;
double present_interval_us = 0.0;

// Input to present per frame: when the title read its input (frame id and
// time per slot), and a histogram of the time to the end of the frame's
// present in 0.5 ms bins (the last bin collects 128 ms and more).
struct InputSampleSlot {
  std::atomic<uint32_t> frame{0};
  std::atomic<int64_t> time_us{0};
};
std::array<InputSampleSlot, 64> input_samples;
std::mutex latency_mutex;
std::array<uint32_t, 257> input_to_present{};
uint64_t input_to_present_sum_us = 0;
uint32_t input_to_present_count = 0;
uint64_t input_to_present_max_us = 0;

// Since the previous report.
struct Counters {
  std::atomic<uint64_t> frames_started{0};
  std::atomic<uint64_t> frames_built{0};
  std::atomic<uint64_t> guest_swaps{0};
  // Guest swaps without a built frame before them (menus, loading screens).
  std::atomic<uint64_t> untracked_swaps{0};
  // Built frames dropped from the queue without a guest swap.
  std::atomic<uint64_t> dropped_frames{0};
  std::atomic<uint64_t> cp_swaps{0};
  // Command processor swaps that found the swap queue empty.
  std::atomic<uint64_t> cp_unpaired{0};
  std::atomic<uint64_t> presents{0};
  // Presents of an image whose frame was already presented (repaints).
  std::atomic<uint64_t> repeat_presents{0};
  // Frames started beyond the presented one, summed per present.
  std::atomic<uint64_t> present_lag_sum{0};
  std::atomic<uint64_t> queue_waits{0};
  std::atomic<uint64_t> queue_wait_us{0};
  std::atomic<uint64_t> queue_timeouts{0};
};
Counters counters;

// Low latency: the frame `queue` frames before this one has been presented
// (or a newer one), unless it was never built (menus, loading) or the wait
// reaches its limit.
void WaitForPresent(uint32_t frame, uint32_t queue) {
  if (frame <= queue) return;
  const uint32_t target = frame - queue;
  if (last_built.load(std::memory_order_acquire) < target) return;
  std::unique_lock<std::mutex> lock(present_mutex);
  if (presented_frame >= target) return;
  const int64_t start = NowUs();
  // With a lead, the wait may end that long before the awaited frame's
  // expected present (the present before it plus the average interval): the
  // developer switch, or half the interval with the player setting.
  const int64_t lead_us = SettingDrivesQueue() ? int64_t(present_interval_us * 0.5)
                                               : int64_t(REXCVAR_GET(display_low_latency_lead_us));
  auto deadline = Clock::now() + kQueueWaitLimit;
  auto lead_reached = [&] {
    if (lead_us <= 0 || presented_frame + 1 < target || present_interval_us <= 0.0) return false;
    return NowUs() >= presented_time_us + int64_t(present_interval_us) - lead_us;
  };
  bool presented = presented_frame >= target || lead_reached();
  while (!presented && Clock::now() < deadline) {
    auto wake = deadline;
    if (lead_us > 0 && presented_frame + 1 >= target && present_interval_us > 0.0) {
      const int64_t lead_time_us =
          presented_time_us + int64_t(present_interval_us) - lead_us - NowUs();
      wake = std::min(wake, Clock::now() + std::chrono::microseconds(std::max<int64_t>(0, lead_time_us)));
    }
    present_condition.wait_until(lock, wake);
    presented = presented_frame >= target || lead_reached();
  }
  counters.queue_waits.fetch_add(1, std::memory_order_relaxed);
  counters.queue_wait_us.fetch_add(uint64_t(NowUs() - start), std::memory_order_relaxed);
  if (!presented) counters.queue_timeouts.fetch_add(1, std::memory_order_relaxed);
}

void Report() {
#if REX_HAS_D3D12
  d3d12::streamline::LogReport();
#endif
  if (!REXCVAR_GET(frame_latency_report) && !StreamlineActive()) return;
  const uint64_t presents = counters.presents.exchange(0, std::memory_order_relaxed);
  const uint64_t lag_sum = counters.present_lag_sum.exchange(0, std::memory_order_relaxed);
  const uint64_t waits = counters.queue_waits.exchange(0, std::memory_order_relaxed);
  const uint64_t wait_us = counters.queue_wait_us.exchange(0, std::memory_order_relaxed);
  std::fprintf(stderr,
               "REX_FRAME_PAIRING frames_started=%llu frames_built=%llu guest_swaps=%llu "
               "untracked_swaps=%llu dropped_frames=%llu cp_swaps=%llu cp_unpaired=%llu "
               "presents=%llu repeat_presents=%llu present_lag_frames=%.2f\n",
               static_cast<unsigned long long>(counters.frames_started.exchange(0)),
               static_cast<unsigned long long>(counters.frames_built.exchange(0)),
               static_cast<unsigned long long>(counters.guest_swaps.exchange(0)),
               static_cast<unsigned long long>(counters.untracked_swaps.exchange(0)),
               static_cast<unsigned long long>(counters.dropped_frames.exchange(0)),
               static_cast<unsigned long long>(counters.cp_swaps.exchange(0)),
               static_cast<unsigned long long>(counters.cp_unpaired.exchange(0)),
               static_cast<unsigned long long>(presents),
               static_cast<unsigned long long>(counters.repeat_presents.exchange(0)),
               presents ? double(lag_sum) / double(presents) : 0.0);
  std::lock_guard<std::mutex> lock(latency_mutex);
  auto percentile = [](double p) {
    const uint64_t target = uint64_t(double(input_to_present_count) * p + 0.5);
    uint64_t seen = 0;
    for (size_t i = 0; i < input_to_present.size(); ++i) {
      seen += input_to_present[i];
      if (seen >= target && seen) return (double(i) + 0.5) * 0.5;
    }
    return 128.0;
  };
  std::fprintf(stderr,
               "REX_FRAME_LATENCY queue=%d lead_us=%d frames=%u input_to_present_ms avg=%.2f "
               "p50=%.2f "
               "p90=%.2f p99=%.2f max=%.2f queue_waits=%llu queue_wait_ms_avg=%.2f "
               "queue_timeouts=%llu\n",
               EffectiveQueue(),
               SettingDrivesQueue() ? -1 : REXCVAR_GET(display_low_latency_lead_us),
               input_to_present_count,
               input_to_present_count
                   ? double(input_to_present_sum_us) / 1000.0 / input_to_present_count
                   : 0.0,
               percentile(0.5), percentile(0.9), percentile(0.99),
               double(input_to_present_max_us) / 1000.0, static_cast<unsigned long long>(waits),
               waits ? double(wait_us) / 1000.0 / double(waits) : 0.0,
               static_cast<unsigned long long>(counters.queue_timeouts.exchange(0)));
  std::fflush(stderr);
  input_to_present.fill(0);
  input_to_present_sum_us = 0;
  input_to_present_count = 0;
  input_to_present_max_us = 0;
}

}  // namespace

void FrameStart() {
  const int32_t queue = EffectiveQueue();
  if (!tracking.load(std::memory_order_acquire)) {
    if (queue <= 0 && !REXCVAR_GET(frame_latency_report) && !StreamlineActive()) return;
    tracking.store(true, std::memory_order_release);
  }
  counters.frames_started.fetch_add(1, std::memory_order_relaxed);
  // The previous frame's simulation ends here unless its guest swap came
  // first (a frame start without a built frame, e.g. in menus, ends here too).
  if (const uint32_t open = open_simulation.exchange(0, std::memory_order_acq_rel)) {
    Mark(Marker::kSimulationEnd, open);
  }
  const uint32_t frame = next_frame.fetch_add(1, std::memory_order_relaxed) + 1;
  if (queue > 0) WaitForPresent(frame, uint32_t(queue));
#if REX_HAS_D3D12
  d3d12::streamline::ReflexSleep(frame);
  if (d3d12::streamline::TakeLatencyPing()) Mark(Marker::kLatencyPing, frame);
#endif
  Mark(Marker::kSimulationStart, frame);
  open_simulation.store(frame, std::memory_order_release);
  if (!--frames_until_report) {
    frames_until_report = kReportFrames;
    Report();
  }
}

void InputSample() {
  if (!tracking.load(std::memory_order_acquire)) return;
  const uint32_t frame = open_simulation.load(std::memory_order_acquire);
  // The first input read of the frame.
  if (!frame || frame == sampled_frame) return;
  sampled_frame = frame;
  InputSampleSlot& slot = input_samples[frame % input_samples.size()];
  slot.time_us.store(NowUs(), std::memory_order_relaxed);
  slot.frame.store(frame, std::memory_order_release);
  Mark(Marker::kControllerInputSample, frame);
}

void FrameBuilt() {
  if (!tracking.load(std::memory_order_acquire)) return;
  const uint32_t frame = open_simulation.load(std::memory_order_acquire);
  // Once per frame: built frames and guest swaps pair one to one, in order.
  if (!frame || frame == built_frame) return;
  built_frame = frame;
  last_built.store(frame, std::memory_order_release);
  counters.frames_built.fetch_add(1, std::memory_order_relaxed);
  std::lock_guard<std::mutex> lock(frame_mutex);
  if (built.Push(frame)) counters.dropped_frames.fetch_add(1, std::memory_order_relaxed);
}

void GuestSwap() {
  if (!tracking.load(std::memory_order_acquire)) return;
  uint32_t frame;
  {
    std::lock_guard<std::mutex> lock(frame_mutex);
    frame = built.Pop();
    swapped.Push(frame);
  }
  counters.guest_swaps.fetch_add(1, std::memory_order_relaxed);
  if (!frame) {
    counters.untracked_swaps.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  // The producer may still be in this frame (when it is the render thread).
  uint32_t open = frame;
  if (open_simulation.compare_exchange_strong(open, 0, std::memory_order_acq_rel)) {
    Mark(Marker::kSimulationEnd, frame);
  }
  // From the guest's complete frame to its last host submission.
  Mark(Marker::kRenderSubmitStart, frame);
}

void BeginRenderSubmit() {
  render_frame = 0;
  render_open = false;
  if (!tracking.load(std::memory_order_acquire)) return;
  {
    std::lock_guard<std::mutex> lock(frame_mutex);
    if (swapped.size()) {
      render_frame = swapped.Pop();
    } else {
      counters.cp_unpaired.fetch_add(1, std::memory_order_relaxed);
    }
  }
  counters.cp_swaps.fetch_add(1, std::memory_order_relaxed);
  render_open = render_frame != 0;
}

void EndRenderSubmit() {
  if (!render_open) return;
  render_open = false;
  Mark(Marker::kRenderSubmitEnd, render_frame);
}

uint32_t RenderSubmitFrame() { return render_frame; }

uint32_t BeginPresent(uint32_t frame) {
  if (!frame) return 0;
  uint32_t last = last_present.load(std::memory_order_relaxed);
  do {
    if (frame <= last) {
      counters.repeat_presents.fetch_add(1, std::memory_order_relaxed);
      return 0;
    }
  } while (!last_present.compare_exchange_weak(last, frame, std::memory_order_relaxed));
  counters.presents.fetch_add(1, std::memory_order_relaxed);
  counters.present_lag_sum.fetch_add(next_frame.load(std::memory_order_relaxed) - frame,
                                     std::memory_order_relaxed);
  Mark(Marker::kPresentStart, frame);
  return frame;
}

void SetMenuState(bool menu) { in_menu.store(menu, std::memory_order_relaxed); }

bool InMenu() { return in_menu.load(std::memory_order_relaxed); }

void EndPresent(uint32_t frame) {
  if (!frame) return;
  Mark(Marker::kPresentEnd, frame);
  {
    std::lock_guard<std::mutex> lock(present_mutex);
    const int64_t now = NowUs();
    if (frame > presented_frame) {
      if (presented_time_us) {
        // Consecutive frames only, without long gaps (menus, loading).
        const double interval = double(now - presented_time_us) / double(frame - presented_frame);
        if (interval < 100000.0) {
          present_interval_us = present_interval_us > 0.0
                                    ? present_interval_us * 0.9 + interval * 0.1
                                    : interval;
        }
      }
      presented_frame = frame;
      presented_time_us = now;
    }
  }
  present_condition.notify_all();
  InputSampleSlot& slot = input_samples[frame % input_samples.size()];
  if (slot.frame.load(std::memory_order_acquire) != frame) return;
  const uint64_t elapsed_us =
      uint64_t(std::max<int64_t>(0, NowUs() - slot.time_us.load(std::memory_order_relaxed)));
  std::lock_guard<std::mutex> lock(latency_mutex);
  ++input_to_present[std::min<uint64_t>(elapsed_us / 500, input_to_present.size() - 1)];
  input_to_present_sum_us += elapsed_us;
  ++input_to_present_count;
  input_to_present_max_us = std::max(input_to_present_max_us, elapsed_us);
}

}  // namespace rex::ui::frame_latency
