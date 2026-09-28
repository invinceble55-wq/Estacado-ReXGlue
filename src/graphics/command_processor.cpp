#include <rex/graphics/pc_constant_writer.h>
/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <algorithm>
#include <cstdarg>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <intrin.h>
#endif

#include <fmt/format.h>

#include <rex/cvar.h>
#include <rex/dbg.h>
#include <rex/perf/counter.h>
#include <rex/chrono/clock.h>
#include <rex/graphics/command_processor.h>
#include <rex/graphics/cp_interrupt_timing.h>
#include <rex/graphics/cp_interrupt_wakeup.h>
#include <rex/graphics/flags.h>
#include <rex/graphics/graphics_system.h>
#include <rex/graphics/pipeline/texture/info.h>
#include <rex/graphics/sampler_info.h>
#include <rex/graphics/xenos.h>
#include <rex/logging.h>
#include <rex/math.h>
#include <rex/memory.h>
#include <rex/memory/ring_buffer.h>
#include <rex/stream.h>
#include <rex/system/kernel_state.h>
#include <rex/system/user_module.h>

REXCVAR_DEFINE_BOOL(vsync, true, "GPU", "Enable vertical sync");
REXCVAR_DEFINE_BOOL(embedded_cp_cadence_diagnostics, false, "GPU/Diagnostics",
                   "Bounded inter-swap command availability, PM4 wait and marker timing; no timing changes");
REXCVAR_DEFINE_BOOL(embedded_swap_interval_diagnostics, false, "GPU/Diagnostics",
                   "Bounded distribution of real completed guest-swap intervals; no presentation changes");
REXCVAR_DEFINE_INT32(embedded_swap_long_frame_us, 0, "GPU/Diagnostics",
                     "With embedded_swap_interval_diagnostics: swap intervals at or above this many "
                     "microseconds form the long-frame group and are recorded with the frame before "
                     "them (0 = two 60 Hz refreshes, no per-frame records)")
    .range(0, 1000000);
// Default ON since V285: the accepted V283 player configuration always ran
// with it (launch flag -InterruptWakeup); without it the CP polls
// WAIT_REG_MEM with Sleep and the street runs at about half the frame rate.
REXCVAR_DEFINE_BOOL(embedded_interrupt_wait_wakeup, true, "GPU/Experiments",
                   "Advisory callback completion wakes embedded memory polling; comparisons and timeout remain authoritative");

// Default OFF since V349: every CPU write reaches the page state (guest stores
// through the host's tracker, plugin host writes through
// SharedMemory::MarkHostWrite), so the reset only re-uploaded unchanged pages:
// ~15 MB and ~600 copies per frame at the street, 8-12% of the frame rate.
// The coherency audit (measurement builds) found no missed write with it off.
REXCVAR_DEFINE_BOOL(clear_memory_page_state, false, "GPU",
                    "Reset CPU-uploaded page state at every frame end, re-uploading the working "
                    "set (a coherency fallback; costs GPU and command-processor time).")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(occlusion_query_enable, true, "GPU", "Enable host occlusion query handling")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_STRING(readback_resolve, "none", "GPU",
                      "Controls CPU readback of render-to-texture resolve results.\n"
                      " none: Disable readback (default)\n"
                      " fast: Read previous frame (delayed, copy every frame)\n"
                      " some: Read previous frame (delayed, copy on cache miss)\n"
                      " full: Immediate sync readback (accurate but stalls)")
    .allowed({"none", "fast", "some", "full"})
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(readback_resolve_half_pixel_offset, false, "GPU",
                    "When draw resolution scaling is active, sample from the center of each "
                    "scaled block during resolve readback downscale")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(readback_memexport, true, "GPU",
                    "Enable CPU readback of shader memexport writes for guest memory "
                    "coherency (can reduce correctness issues, but may add GPU/CPU sync cost)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(readback_memexport_fast, true, "GPU",
                    "Use fast double-buffered memexport readback when possible, with "
                    "automatic fallback to full synchronous readback")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_INT32(query_occlusion_fake_sample_count, 1000, "GPU",
                     "Fake sample count for occlusion queries")
    .range(1, 100000)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

// Off (default): shaders are translated when a draw first needs them and new
// pipelines are created by the creation threads while the rest of the
// submission is recorded; the submission waits for them, so no draw is ever
// skipped (a short hitch the first time a pipeline is needed).
REXCVAR_DEFINE_BOOL(async_shader_compilation, false, "GPU",
                    "Also translate shaders in background threads and skip draws until "
                    "their pipelines are ready. This reduces stutter but draws frames with "
                    "missing passes (brief visual artifacts) while pipelines are prepared.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

// Lean indirect-buffer loop (see CommandProcessor::ExecuteIndirectBufferLean).
// Read per indirect buffer, so it can be switched live for A/B checks.
REXCVAR_DEFINE_BOOL(gpu_lean_indirect_buffers, false, "GPU",
                    "Execute indirect buffers without per-packet diagnostic bookkeeping");

namespace rex::graphics {

using namespace rex::graphics::xenos;

namespace {

void TraceEmbeddedCommandProcessor(const char* format, ...) {
  std::FILE* file = std::fopen("logs/m6c_rex_wait_trace.internal.log", "a");
  if (!file) {
    return;
  }
  va_list args;
  va_start(args, format);
  std::vfprintf(file, format, args);
  va_end(args);
  std::fputc('\n', file);
  std::fclose(file);
}

// Long frames captured in one swap-interval window, each with the frame
// before it, written through a single open of the trace. Tick fields are
// host ticks converted to microseconds; busy_us is the CP thread's cycle
// time (-1 when unavailable). remaining_us = interval - idle - wait - swap.
void TraceSwapLongFrames(const SwapIntervalDiagnostic& intervals, uint64_t frequency,
                         uint64_t tsc_hz) {
  std::FILE* file = std::fopen("logs/m6c_rex_wait_trace.internal.log", "a");
  if (!file) {
    return;
  }
  const auto us = [frequency](uint64_t ticks) -> unsigned long long {
    return frequency ? static_cast<unsigned long long>(ticks * 1000000 / frequency) : 0;
  };
  const auto busy = [tsc_hz](const SwapIntervalDiagnostic::FrameRecord& r) -> long long {
    return r.thread_cycles_valid && tsc_hz
               ? static_cast<long long>(double(r.thread_cycles) * 1e6 / double(tsc_hz))
               : -1;
  };
  const auto remaining = [&us](const SwapIntervalDiagnostic::FrameRecord& r) {
    const unsigned long long accounted =
        us(r.idle_ticks) + us(r.wait_ticks) + us(r.issue_swap_ticks);
    return r.interval_us > accounted ? r.interval_us - accounted : 0ull;
  };
  for (uint32_t i = 0; i < intervals.long_frame_count; ++i) {
    const auto& p = intervals.long_frames[i].previous;
    const auto& f = intervals.long_frames[i].frame;
    std::fprintf(
        file,
        "REX_SWAP_LONG_FRAME version=2 swap=%llu tick=%llu interval_us=%llu idle_us=%llu "
        "wait_us=%llu waits=%llu issue_swap_us=%llu fence_wait_us=%llu full_syncs=%llu "
        "remaining_us=%llu busy_us=%lld draws=%llu type0_words=%llu upload_bytes=%llu "
        "upload_ranges=%llu texture_creates=%llu texture_create_us=%llu prev_swap=%llu "
        "prev_interval_us=%llu prev_idle_us=%llu "
        "prev_wait_us=%llu prev_issue_swap_us=%llu prev_fence_wait_us=%llu "
        "prev_remaining_us=%llu prev_busy_us=%lld prev_draws=%llu prev_upload_bytes=%llu\n",
        static_cast<unsigned long long>(f.swap), static_cast<unsigned long long>(f.end_tick),
        static_cast<unsigned long long>(f.interval_us), us(f.idle_ticks), us(f.wait_ticks),
        static_cast<unsigned long long>(f.waits), us(f.issue_swap_ticks),
        us(f.fence_wait_ticks), static_cast<unsigned long long>(f.full_syncs), remaining(f),
        busy(f), static_cast<unsigned long long>(f.draws),
        static_cast<unsigned long long>(f.type0_words),
        static_cast<unsigned long long>(f.upload_bytes),
        static_cast<unsigned long long>(f.upload_ranges),
        static_cast<unsigned long long>(f.texture_creations), us(f.texture_create_ticks),
        static_cast<unsigned long long>(p.swap), static_cast<unsigned long long>(p.interval_us),
        us(p.idle_ticks), us(p.wait_ticks), us(p.issue_swap_ticks), us(p.fence_wait_ticks),
        remaining(p), busy(p), static_cast<unsigned long long>(p.draws),
        static_cast<unsigned long long>(p.upload_bytes));
  }
  std::fclose(file);
}

ReadbackResolveMode ParseReadbackResolveMode(std::string_view value) {
  if (value == "fast") {
    return ReadbackResolveMode::kFast;
  }
  if (value == "some") {
    return ReadbackResolveMode::kSome;
  }
  if (value == "full") {
    return ReadbackResolveMode::kFull;
  }
  return ReadbackResolveMode::kDisabled;
}

// rex::cvar::HasNonDefaultValue takes the registry mutex, builds a std::string
// key on the heap and does a hash lookup, while the readback settings are read
// per resolve and per memexport draw. Its answer depends only on the flag's
// value, so it is cached on the calling (command processor) thread and asked
// again whenever the value the caller passes changes and at least once per
// guest frame: a live change of the setting still applies within a frame.
// thread_local keeps the command processor's object layout unchanged.
struct NonDefaultValueCache {
  const char* name = nullptr;
  uint64_t frame = UINT64_MAX;
  uint32_t key = 0;
  bool non_default = false;

  bool Get(const char* flag_name, uint64_t current_frame, uint32_t current_key) {
    if (flag_name != name || current_frame != frame || current_key != key) {
      non_default = rex::cvar::HasNonDefaultValue(flag_name);
      name = flag_name;
      frame = current_frame;
      key = current_key;
    }
    return non_default;
  }
};
thread_local NonDefaultValueCache readback_resolve_non_default;
thread_local NonDefaultValueCache readback_memexport_legacy_non_default;

}  // namespace

CommandProcessor::CommandProcessor(GraphicsSystem* graphics_system,
                                   system::KernelState* kernel_state)
    : memory_(graphics_system->memory()),
      kernel_state_(kernel_state),
      graphics_system_(graphics_system),
      register_file_(graphics_system_->register_file()),
      trace_writer_(graphics_system->memory()->physical_membase()),
      worker_running_(true),
      write_ptr_index_event_(rex::thread::Event::CreateAutoResetEvent(false)),
      write_ptr_index_(0) {
  assert_not_null(write_ptr_index_event_);
}

CommandProcessor::~CommandProcessor() = default;

bool CommandProcessor::Initialize() {
  // Initialize the gamma ramps to their default (linear) values - taken from
  // what games set when starting with the sRGB (return value 1)
  // VdGetCurrentDisplayGamma.
  for (uint32_t i = 0; i < 256; ++i) {
    uint32_t value = i * 0x3FF / 0xFF;
    reg::DC_LUT_30_COLOR& gamma_ramp_entry = gamma_ramp_256_entry_table_[i];
    gamma_ramp_entry.color_10_blue = value;
    gamma_ramp_entry.color_10_green = value;
    gamma_ramp_entry.color_10_red = value;
  }
  for (uint32_t i = 0; i < 128; ++i) {
    reg::DC_LUT_PWL_DATA gamma_ramp_entry = {};
    gamma_ramp_entry.base = (i * 0xFFFF / 0x7F) & ~UINT32_C(0x3F);
    gamma_ramp_entry.delta = i < 0x7F ? 0x200 : 0;
    for (uint32_t j = 0; j < 3; ++j) {
      gamma_ramp_pwl_rgb_[i][j] = gamma_ramp_entry;
    }
  }

  worker_running_ = true;
  if (kernel_state_) {
    worker_thread_ = system::object_ref<system::XHostThread>(
        new system::XHostThread(kernel_state_, 128 * 1024, 0, [this]() {
          WorkerThreadMain();
          return 0;
        }));
    worker_thread_->set_name("GPU Commands");
    worker_thread_->Create();
  } else {
    embedded_worker_thread_ = std::thread([this]() {
      embedded_worker_thread_id_ = std::this_thread::get_id();
      WorkerThreadMain();
    });
  }

  return true;
}

void CommandProcessor::Shutdown() {
  EndTracing();

  worker_running_ = false;
  write_ptr_index_event_->Set();
  if (worker_thread_) {
    worker_thread_->Wait(0, 0, 0, nullptr);
    worker_thread_.reset();
  }
  if (embedded_worker_thread_.joinable()) {
    embedded_worker_thread_.join();
  }
  embedded_worker_thread_id_ = {};
}

void CommandProcessor::InitializeShaderStorage(const std::filesystem::path& cache_root,
                                               uint32_t title_id, bool blocking) {}

void CommandProcessor::RequestFrameTrace(const std::filesystem::path& root_path) {
  if (trace_state_ == TraceState::kStreaming) {
    REXGPU_ERROR("Streaming trace; cannot also trace frame.");
    return;
  }
  if (trace_state_ == TraceState::kSingleFrame) {
    REXGPU_ERROR("Frame trace already pending; ignoring.");
    return;
  }
  trace_state_ = TraceState::kSingleFrame;
  trace_frame_path_ = root_path;
}

void CommandProcessor::BeginTracing(const std::filesystem::path& root_path) {
  if (trace_state_ == TraceState::kStreaming) {
    REXGPU_ERROR("Streaming already active; ignoring request.");
    return;
  }
  if (trace_state_ == TraceState::kSingleFrame) {
    REXGPU_ERROR("Frame trace pending; ignoring streaming request.");
    return;
  }
  // Streaming starts on the next primary buffer execute.
  trace_state_ = TraceState::kStreaming;
  trace_stream_path_ = root_path;
}

void CommandProcessor::EndTracing() {
  if (!trace_writer_.is_open()) {
    return;
  }
  assert_true(trace_state_ == TraceState::kStreaming);
  trace_state_ = TraceState::kDisabled;
  trace_writer_.Close();
}

void CommandProcessor::RestoreRegisters(uint32_t first_register, const uint32_t* register_values,
                                        uint32_t register_count, bool execute_callbacks) {
  if (first_register > RegisterFile::kRegisterCount ||
      RegisterFile::kRegisterCount - first_register < register_count) {
    REXGPU_WARN(
        "CommandProcessor::RestoreRegisters out of bounds (0x{:X} registers "
        "starting with 0x{:X}, while a total of 0x{:X} registers are stored)",
        register_count, first_register, RegisterFile::kRegisterCount);
    if (first_register > RegisterFile::kRegisterCount) {
      return;
    }
    register_count =
        std::min(uint32_t(RegisterFile::kRegisterCount) - first_register, register_count);
  }
  if (kGpuDiagnostics) command_execution_.Invalidate();
  InvalidateRegisterProvenance();
  if (execute_callbacks) {
    for (uint32_t i = 0; i < register_count; ++i) {
      WriteRegister(first_register + i, register_values[i]);
    }
  } else {
    std::memcpy(register_file_->values + first_register, register_values,
                sizeof(uint32_t) * register_count);
  }
}

void CommandProcessor::RestoreGammaRamp(const reg::DC_LUT_30_COLOR* new_gamma_ramp_256_entry_table,
                                        const reg::DC_LUT_PWL_DATA* new_gamma_ramp_pwl_rgb,
                                        uint32_t new_gamma_ramp_rw_component) {
  std::memcpy(gamma_ramp_256_entry_table_, new_gamma_ramp_256_entry_table,
              sizeof(reg::DC_LUT_30_COLOR) * 256);
  std::memcpy(gamma_ramp_pwl_rgb_, new_gamma_ramp_pwl_rgb, sizeof(reg::DC_LUT_PWL_DATA) * 3 * 128);
  gamma_ramp_rw_component_ = new_gamma_ramp_rw_component;
  OnGammaRamp256EntryTableValueWritten();
  OnGammaRampPWLValueWritten();
}

void CommandProcessor::CallInThread(std::function<void()> fn) {
  const bool is_worker = worker_thread_
                             ? system::XThread::IsInThread(worker_thread_.get())
                             : std::this_thread::get_id() == embedded_worker_thread_id_;
  if (pending_fns_.empty() && is_worker) {
    fn();
  } else {
    pending_fns_.push(std::move(fn));
  }
}

void CommandProcessor::ClearCaches() {}

void CommandProcessor::InvalidateGpuMemory() {}

ReadbackResolveMode CommandProcessor::GetReadbackResolveMode(
    bool legacy_readback_resolve_enabled) const {
  ReadbackResolveMode shared_mode = ParseReadbackResolveMode(REXCVAR_GET(readback_resolve));
  bool shared_mode_overrides_legacy =
      shared_mode != ReadbackResolveMode::kDisabled ||
      readback_resolve_non_default.Get("readback_resolve",
                                       guest_frame_count_.load(std::memory_order_relaxed),
                                       uint32_t(shared_mode));
  if (shared_mode_overrides_legacy) {
    return shared_mode;
  }
  return legacy_readback_resolve_enabled ? ReadbackResolveMode::kFast
                                         : ReadbackResolveMode::kDisabled;
}

bool CommandProcessor::IsReadbackMemexportEnabled(bool legacy_backend_flag) const {
  // Backends pass the current value of the flag named by
  // legacy_readback_memexport_cvar_name_, so a change refreshes the cache.
  if (legacy_readback_memexport_cvar_name_ &&
      readback_memexport_legacy_non_default.Get(
          legacy_readback_memexport_cvar_name_,
          guest_frame_count_.load(std::memory_order_relaxed), legacy_backend_flag)) {
    return legacy_backend_flag;
  }
  return REXCVAR_GET(readback_memexport);
}

void CommandProcessor::SetDesiredSwapPostEffect(SwapPostEffect swap_post_effect) {
  if (swap_post_effect_desired_ == swap_post_effect) {
    return;
  }
  swap_post_effect_desired_ = swap_post_effect;
  CallInThread([this, swap_post_effect]() { swap_post_effect_actual_ = swap_post_effect; });
}

void CommandProcessor::WorkerThreadMain() {
  if (!SetupContext()) {
    rex::FatalError("Unable to setup command processor internal state");
    return;
  }
  // Publish backend cache construction to embedded CPU-side memory
  // notifications. Initialize() intentionally returns as soon as the worker
  // is started, before SetupContext has completed on this thread.
  worker_context_ready_.store(true, std::memory_order_release);

  while (worker_running_) {
    while (!pending_fns_.empty()) {
      auto fn = std::move(pending_fns_.front());
      pending_fns_.pop();
      fn();
    }

    uint32_t write_ptr_index = write_ptr_index_.load();
    if (write_ptr_index == 0xBAADF00D || read_ptr_index_ == write_ptr_index) {
      SCOPE_profile_cpu_i("gpu", "rex::graphics::CommandProcessor::Stall");
      // We've run out of commands to execute.
      const bool interval_idle_active = kGpuDiagnostics && swap_intervals_.active;
      const uint64_t observed_idle_begin =
          ((kGpuDiagnostics && cp_cadence_.active) || interval_idle_active)
          ? rex::chrono::Clock::QueryHostTickCount() : 0;
      // We spin here waiting for new ones, as the overhead of waiting on our
      // event is too high.
      PrepareForWait();
      uint32_t loop_count = 0;
      do {
        // If we spin around too much, revert to a "low-power" state.
        if (loop_count > 500) {
          const int wait_time_ms = 5;
          rex::thread::Wait(write_ptr_index_event_.get(), true,
                            std::chrono::milliseconds(wait_time_ms));
        }

        rex::thread::MaybeYield();
        loop_count++;
        write_ptr_index = write_ptr_index_.load();
      } while (worker_running_ && pending_fns_.empty() &&
               (write_ptr_index == 0xBAADF00D || read_ptr_index_ == write_ptr_index));
      ReturnFromWait();
      if ((kGpuDiagnostics && cp_cadence_.active) || interval_idle_active) {
        const uint64_t idle_ticks =
            rex::chrono::Clock::QueryHostTickCount() - observed_idle_begin;
        if (cp_cadence_.active) cp_cadence_.idle_ticks += idle_ticks;
        if (interval_idle_active) swap_intervals_.AddIdle(idle_ticks);
      }
      if (!worker_running_ || !pending_fns_.empty()) {
        continue;
      }
    }
    assert_true(read_ptr_index_ != write_ptr_index);

    // Execute. Note that we handle wraparound transparently.
    read_ptr_index_ = ExecutePrimaryBuffer(read_ptr_index_, write_ptr_index);

    // TODO(benvanik): use reader->Read_update_freq_ and only issue after moving
    //     that many indices.
    if (read_ptr_writeback_ptr_) {
      {
        auto host_write = memory_->GuardPhysicalWrite(read_ptr_writeback_ptr_, 4);
        memory::store_and_swap<uint32_t>(memory_->TranslatePhysical(read_ptr_writeback_ptr_), read_ptr_index_);
      }
    }

    // FIXME: We're supposed to process the WAIT_UNTIL register at this point,
    // but no games seem to actually use it.
  }

  worker_context_ready_.store(false, std::memory_order_release);
  ShutdownContext();
}

void CommandProcessor::Pause() {
  if (paused_) {
    return;
  }
  paused_ = true;

  thread::Fence fence;
  CallInThread([&fence]() {
    fence.Signal();
    thread::Thread::GetCurrentThread()->Suspend();
  });

  fence.Wait();
}

void CommandProcessor::Resume() {
  if (!paused_) {
    return;
  }
  paused_ = false;

  worker_thread_->thread()->Resume();
}

bool CommandProcessor::Save(::rex::stream::ByteStream* stream) {
  assert_true(paused_);

  stream->Write<uint32_t>(primary_buffer_ptr_);
  stream->Write<uint32_t>(primary_buffer_size_);
  stream->Write<uint32_t>(read_ptr_index_);
  stream->Write<uint32_t>(read_ptr_update_freq_);
  stream->Write<uint32_t>(read_ptr_writeback_ptr_);
  stream->Write<uint32_t>(write_ptr_index_.load());

  return true;
}

bool CommandProcessor::Restore(::rex::stream::ByteStream* stream) {
  assert_true(paused_);
  if (track_ring_publications_) {
    std::lock_guard lock(ring_publication_mutex_);
    // Restored pending commands have no newly observed CPU publication.
    ring_publications_.Disable();
  }
  if (kGpuDiagnostics) command_execution_.Invalidate();
  InvalidateRegisterProvenance();

  primary_buffer_ptr_ = stream->Read<uint32_t>();
  primary_buffer_size_ = stream->Read<uint32_t>();
  read_ptr_index_ = stream->Read<uint32_t>();
  read_ptr_update_freq_ = stream->Read<uint32_t>();
  read_ptr_writeback_ptr_ = stream->Read<uint32_t>();
  write_ptr_index_.store(stream->Read<uint32_t>());

  return true;
}

bool CommandProcessor::SetupContext() {
  return true;
}

void CommandProcessor::MarkRegisterWriteHandler(uint32_t first, uint32_t last) {
  for (uint32_t i = first; i <= last && i < RegisterFile::kRegisterCount; ++i) {
    register_write_handler_[i] = 1;
  }
}

void CommandProcessor::EnableRegisterWriteFastPath() {
  // Registers with side effects in CommandProcessor::WriteRegister itself.
  MarkRegisterWriteHandler(XE_GPU_REG_SCRATCH_REG0, XE_GPU_REG_SCRATCH_REG7);
  MarkRegisterWriteHandler(XE_GPU_REG_COHER_STATUS_HOST, XE_GPU_REG_COHER_STATUS_HOST);
  MarkRegisterWriteHandler(XE_GPU_REG_DC_LUT_RW_INDEX, XE_GPU_REG_DC_LUT_RW_INDEX);
  MarkRegisterWriteHandler(XE_GPU_REG_DC_LUT_SEQ_COLOR, XE_GPU_REG_DC_LUT_SEQ_COLOR);
  MarkRegisterWriteHandler(XE_GPU_REG_DC_LUT_PWL_DATA, XE_GPU_REG_DC_LUT_PWL_DATA);
  MarkRegisterWriteHandler(XE_GPU_REG_DC_LUT_30_COLOR, XE_GPU_REG_DC_LUT_30_COLOR);
  register_write_fast_path_ = true;
}

void CommandProcessor::ReportEmbeddedNanConstantSource(
    uint32_t packet, uintptr_t packet_host_address, uintptr_t data_host_address,
    uint32_t base_index, uint32_t count, uint32_t write_one_reg, uint32_t target_index,
    uint32_t reg_data) {
  static uint32_t embedded_nan_constant_source_count = 0;
  if (embedded_nan_constant_source_count >= 64) {
    return;
  }
  ++embedded_nan_constant_source_count;
  const uintptr_t physical_base = reinterpret_cast<uintptr_t>(memory_->physical_membase());
  const uintptr_t physical_end = physical_base + 0x20000000u;
  const uint32_t packet_physical =
      packet_host_address >= physical_base && packet_host_address < physical_end
          ? uint32_t(packet_host_address - physical_base)
          : UINT32_MAX;
  const uint32_t data_physical =
      data_host_address >= physical_base && data_host_address < physical_end
          ? uint32_t(data_host_address - physical_base)
          : UINT32_MAX;
  std::fprintf(stderr,
               "REX_EMBEDDED_NAN_CONSTANT_SOURCE ordinal=%u packet=type0 "
               "header=0x%08X packet_physical=0x%08X data_physical=0x%08X "
               "base=0x%04X count=%u write_one=%u target=0x%04X value=0x%08X\n",
               embedded_nan_constant_source_count, packet, packet_physical,
               data_physical, base_index, count, write_one_reg, target_index, reg_data);
  std::fflush(stderr);
}

void CommandProcessor::ShutdownContext() {}

void CommandProcessor::InitializeRingBuffer(uint32_t ptr, uint32_t size_log2) {
  if (kGpuDiagnostics) command_execution_.Invalidate();
  InvalidateRegisterProvenance();
  read_ptr_index_ = 0;
  primary_buffer_ptr_ = ptr;
  primary_buffer_size_ = uint32_t(1) << (size_log2 + 3);
  if (track_ring_publications_) {
    std::lock_guard lock(ring_publication_mutex_);
    ring_publications_.Reset(primary_buffer_size_ / sizeof(uint32_t));
  }
}

void CommandProcessor::EnableReadPointerWriteBack(uint32_t ptr, uint32_t block_size_log2) {
  // CP_RB_RPTR_ADDR Ring Buffer Read Pointer Address 0x70C
  // ptr = RB_RPTR_ADDR, pointer to write back the address to.
  read_ptr_writeback_ptr_ = ptr;
  // CP_RB_CNTL Ring Buffer Control 0x704
  // block_size = RB_BLKSZ, log2 of number of quadwords read between updates of
  //              the read pointer.
  read_ptr_update_freq_ = uint32_t(1) << block_size_log2 >> 2;
}

void CommandProcessor::UpdateWritePointer(uint32_t value) {
  if (track_ring_publications_) {
    std::lock_guard lock(ring_publication_mutex_);
    ring_publications_.Publish(value);
    // Serialize the real publication with its metadata, including callers on
    // different CPU threads. The existing event remains advisory.
    write_ptr_index_ = value;
  } else {
    write_ptr_index_ = value;
  }
  write_ptr_index_event_->Set();
}

uint32_t CommandProcessor::ReadRegisterValue(uint32_t index) const {
  if (index < RegisterFile::kRegisterCount) {
    return register_file_->values[index];
  }
  auto it = extended_register_values_.find(index);
  return it != extended_register_values_.end() ? it->second : 0;
}

void CommandProcessor::WriteRegister(uint32_t index, uint32_t value) {
  RegisterFile& regs = *register_file_;
  if (index >= RegisterFile::kRegisterCount) {
    auto [it, inserted] = extended_register_values_.insert_or_assign(index, value);
    (void)it;
    if (inserted) {
      REXGPU_WARN(
          "CommandProcessor::WriteRegister index out of bounds: {} (stored as extended register)",
          index);
    }
    return;
  }

  // Volatile for the WAIT_REG_MEM loop.
  const_cast<volatile uint32_t&>(regs.values[index]) = value;
#ifndef NDEBUG
  // Debug-only: the register table lookup runs for every register write
  // (hundreds of thousands per frame) just to feed this debug message.
  if (!regs.GetRegisterInfo(index)) {
    REXGPU_DEBUG("GPU: Write to unknown register ({:04X} = {:08X})", index, value);
  }
#endif

  // Scratch register writeback.
  if (index >= XE_GPU_REG_SCRATCH_REG0 && index <= XE_GPU_REG_SCRATCH_REG7) {
    uint32_t scratch_reg = index - XE_GPU_REG_SCRATCH_REG0;
    if ((1 << scratch_reg) & regs.values[XE_GPU_REG_SCRATCH_UMSK]) {
      // Enabled - write to address.
      uint32_t scratch_addr = regs.values[XE_GPU_REG_SCRATCH_ADDR];
      uint32_t mem_addr = scratch_addr + (scratch_reg * 4);
      SettleGuestVisibleWork();
      {
        auto host_write = memory_->GuardPhysicalWrite(mem_addr, 4);
        memory::store_and_swap<uint32_t>(memory_->TranslatePhysical(mem_addr), value);
      }
    }
  } else {
    switch (index) {
      // If this is a COHER register, set the dirty flag.
      // This will block the command processor the next time it WAIT_REG_MEMs
      // and allow us to synchronize the memory.
      case XE_GPU_REG_COHER_STATUS_HOST: {
        const_cast<volatile uint32_t&>(regs.values[index]) |= UINT32_C(0x80000000);
      } break;

      case XE_GPU_REG_DC_LUT_RW_INDEX: {
        // Reset the sequential read / write component index (see the M56
        // DC_LUT_SEQ_COLOR documentation).
        gamma_ramp_rw_component_ = 0;
      } break;

      case XE_GPU_REG_DC_LUT_SEQ_COLOR: {
        // Should be in the 256-entry table writing mode.
        assert_zero(regs[XE_GPU_REG_DC_LUT_RW_MODE] & 0b1);
        auto gamma_ramp_rw_index = regs.Get<reg::DC_LUT_RW_INDEX>();
        // DC_LUT_SEQ_COLOR is in the red, green, blue order, but the write
        // enable mask is blue, green, red.
        bool write_gamma_ramp_component = (regs[XE_GPU_REG_DC_LUT_WRITE_EN_MASK] &
                                           (UINT32_C(1) << (2 - gamma_ramp_rw_component_))) != 0;
        if (write_gamma_ramp_component) {
          reg::DC_LUT_30_COLOR& gamma_ramp_entry =
              gamma_ramp_256_entry_table_[gamma_ramp_rw_index.rw_index];
          // Bits 0:5 are hardwired to zero.
          uint32_t gamma_ramp_seq_color = regs.Get<reg::DC_LUT_SEQ_COLOR>().seq_color >> 6;
          switch (gamma_ramp_rw_component_) {
            case 0:
              gamma_ramp_entry.color_10_red = gamma_ramp_seq_color;
              break;
            case 1:
              gamma_ramp_entry.color_10_green = gamma_ramp_seq_color;
              break;
            case 2:
              gamma_ramp_entry.color_10_blue = gamma_ramp_seq_color;
              break;
          }
        }
        if (++gamma_ramp_rw_component_ >= 3) {
          gamma_ramp_rw_component_ = 0;
          reg::DC_LUT_RW_INDEX new_gamma_ramp_rw_index = gamma_ramp_rw_index;
          ++new_gamma_ramp_rw_index.rw_index;
          WriteRegister(XE_GPU_REG_DC_LUT_RW_INDEX,
                        rex::memory::Reinterpret<uint32_t>(new_gamma_ramp_rw_index));
        }
        if (write_gamma_ramp_component) {
          OnGammaRamp256EntryTableValueWritten();
        }
      } break;

      case XE_GPU_REG_DC_LUT_PWL_DATA: {
        // Should be in the PWL writing mode.
        assert_not_zero(regs[XE_GPU_REG_DC_LUT_RW_MODE] & 0b1);
        auto gamma_ramp_rw_index = regs.Get<reg::DC_LUT_RW_INDEX>();
        // Bit 7 of the index is ignored for PWL.
        uint32_t gamma_ramp_rw_index_pwl = gamma_ramp_rw_index.rw_index & 0x7F;
        // DC_LUT_PWL_DATA is likely in the red, green, blue order because
        // DC_LUT_SEQ_COLOR is, but the write enable mask is blue, green, red.
        bool write_gamma_ramp_component = (regs[XE_GPU_REG_DC_LUT_WRITE_EN_MASK] &
                                           (UINT32_C(1) << (2 - gamma_ramp_rw_component_))) != 0;
        if (write_gamma_ramp_component) {
          reg::DC_LUT_PWL_DATA& gamma_ramp_entry =
              gamma_ramp_pwl_rgb_[gamma_ramp_rw_index_pwl][gamma_ramp_rw_component_];
          auto gamma_ramp_value = regs.Get<reg::DC_LUT_PWL_DATA>();
          // Bits 0:5 are hardwired to zero.
          gamma_ramp_entry.base = gamma_ramp_value.base & ~UINT32_C(0x3F);
          gamma_ramp_entry.delta = gamma_ramp_value.delta & ~UINT32_C(0x3F);
        }
        if (++gamma_ramp_rw_component_ >= 3) {
          gamma_ramp_rw_component_ = 0;
          reg::DC_LUT_RW_INDEX new_gamma_ramp_rw_index = gamma_ramp_rw_index;
          // TODO(Triang3l): Should this increase beyond 7 bits for PWL?
          // Direct3D 9 explicitly sets rw_index to 0x80 after writing the last
          // PWL entry. However, the DC_LUT_RW_INDEX documentation says that for
          // PWL, the bit 7 is ignored.
          new_gamma_ramp_rw_index.rw_index = (gamma_ramp_rw_index.rw_index & ~UINT32_C(0x7F)) |
                                             ((gamma_ramp_rw_index_pwl + 1) & 0x7F);
          WriteRegister(XE_GPU_REG_DC_LUT_RW_INDEX,
                        rex::memory::Reinterpret<uint32_t>(new_gamma_ramp_rw_index));
        }
        if (write_gamma_ramp_component) {
          OnGammaRampPWLValueWritten();
        }
      } break;

      case XE_GPU_REG_DC_LUT_30_COLOR: {
        // Should be in the 256-entry table writing mode.
        assert_zero(regs[XE_GPU_REG_DC_LUT_RW_MODE] & 0b1);
        auto gamma_ramp_rw_index = regs.Get<reg::DC_LUT_RW_INDEX>();
        uint32_t gamma_ramp_write_enable_mask = regs[XE_GPU_REG_DC_LUT_WRITE_EN_MASK] & 0b111;
        if (gamma_ramp_write_enable_mask) {
          reg::DC_LUT_30_COLOR& gamma_ramp_entry =
              gamma_ramp_256_entry_table_[gamma_ramp_rw_index.rw_index];
          auto gamma_ramp_value = regs.Get<reg::DC_LUT_30_COLOR>();
          if (gamma_ramp_write_enable_mask & 0b001) {
            gamma_ramp_entry.color_10_blue = gamma_ramp_value.color_10_blue;
          }
          if (gamma_ramp_write_enable_mask & 0b010) {
            gamma_ramp_entry.color_10_green = gamma_ramp_value.color_10_green;
          }
          if (gamma_ramp_write_enable_mask & 0b100) {
            gamma_ramp_entry.color_10_red = gamma_ramp_value.color_10_red;
          }
        }
        // TODO(Triang3l): Should this reset the component write index? If this
        // increase is assumed to behave like a full DC_LUT_RW_INDEX write, it
        // probably should. Currently this also calls WriteRegister for
        // DC_LUT_RW_INDEX, which resets gamma_ramp_rw_component_ as well.
        gamma_ramp_rw_component_ = 0;
        reg::DC_LUT_RW_INDEX new_gamma_ramp_rw_index = gamma_ramp_rw_index;
        ++new_gamma_ramp_rw_index.rw_index;
        WriteRegister(XE_GPU_REG_DC_LUT_RW_INDEX,
                      rex::memory::Reinterpret<uint32_t>(new_gamma_ramp_rw_index));
        if (gamma_ramp_write_enable_mask) {
          OnGammaRamp256EntryTableValueWritten();
        }
      } break;
    }
  }
}

void CommandProcessor::WriteRegistersFromMem(uint32_t start_index, uint32_t* base,
                                             uint32_t num_registers) {
  for (uint32_t i = 0; i < num_registers; ++i) {
    uint32_t data = memory::load_and_swap<uint32_t>(base + i);
    WriteRegister(start_index + i, data);
  }
}

void CommandProcessor::WriteRegisterRangeFromRing(memory::RingBuffer* ring, uint32_t base,
                                                  uint32_t num_registers) {
  if (!num_registers) {
    return;
  }
  memory::RingBuffer::ReadRange range = ring->BeginRead(size_t(num_registers) * sizeof(uint32_t));
  if (range.first_length != 0) {
    uint32_t first_count = uint32_t(range.first_length / sizeof(uint32_t));
    WriteRegistersFromMem(base, reinterpret_cast<uint32_t*>(const_cast<uint8_t*>(range.first)),
                          first_count);
    base += first_count;
  }
  if (range.second_length != 0) {
    WriteRegistersFromMem(base, reinterpret_cast<uint32_t*>(const_cast<uint8_t*>(range.second)),
                          uint32_t(range.second_length / sizeof(uint32_t)));
  }
  ring->EndRead(range);
}

void CommandProcessor::WriteALURangeFromRing(memory::RingBuffer* ring, uint32_t base,
                                             uint32_t num_registers) {
  WriteRegisterRangeFromRing(ring, base + 0x4000, num_registers);
}

void CommandProcessor::WriteFetchRangeFromRing(memory::RingBuffer* ring, uint32_t base,
                                               uint32_t num_registers) {
  WriteRegisterRangeFromRing(ring, base + 0x4800, num_registers);
}

void CommandProcessor::WriteBoolRangeFromRing(memory::RingBuffer* ring, uint32_t base,
                                              uint32_t num_registers) {
  WriteRegisterRangeFromRing(ring, base + 0x4900, num_registers);
}

void CommandProcessor::WriteLoopRangeFromRing(memory::RingBuffer* ring, uint32_t base,
                                              uint32_t num_registers) {
  WriteRegisterRangeFromRing(ring, base + 0x4908, num_registers);
}

void CommandProcessor::WriteREGISTERSRangeFromRing(memory::RingBuffer* ring, uint32_t base,
                                                   uint32_t num_registers) {
  WriteRegisterRangeFromRing(ring, base + 0x2000, num_registers);
}

void CommandProcessor::WriteALURangeFromMem(uint32_t start_index, uint32_t* base,
                                            uint32_t num_registers) {
  WriteRegistersFromMem(start_index + 0x4000, base, num_registers);
}

void CommandProcessor::WriteFetchRangeFromMem(uint32_t start_index, uint32_t* base,
                                              uint32_t num_registers) {
  WriteRegistersFromMem(start_index + 0x4800, base, num_registers);
}

void CommandProcessor::WriteBoolRangeFromMem(uint32_t start_index, uint32_t* base,
                                             uint32_t num_registers) {
  WriteRegistersFromMem(start_index + 0x4900, base, num_registers);
}

void CommandProcessor::WriteLoopRangeFromMem(uint32_t start_index, uint32_t* base,
                                             uint32_t num_registers) {
  WriteRegistersFromMem(start_index + 0x4908, base, num_registers);
}

void CommandProcessor::WriteREGISTERSRangeFromMem(uint32_t start_index, uint32_t* base,
                                                  uint32_t num_registers) {
  WriteRegistersFromMem(start_index + 0x2000, base, num_registers);
}

void CommandProcessor::MakeCoherent() {
  SCOPE_profile_cpu_f("gpu");

  // Status host often has 0x01000000 or 0x03000000.
  // This is likely toggling VC (vertex cache) or TC (texture cache).
  // Or, it also has a direction in here maybe - there is probably
  // some way to check for dest coherency (what all the COHER_DEST_BASE_*
  // registers are for).
  // Best docs I've found on this are here:
  // https://web.archive.org/web/20160711162346/https://amd-dev.wpengine.netdna-cdn.com/wordpress/media/2013/10/R6xx_R7xx_3D.pdf
  // https://cgit.freedesktop.org/xorg/driver/xf86-video-radeonhd/tree/src/r6xx_accel.c?id=3f8b6eccd9dba116cc4801e7f80ce21a879c67d2#n454

  // Volatile because this may be called from the WAIT_REG_MEM loop.
  volatile uint32_t* regs_volatile = register_file_->values;
  auto status_host = rex::memory::Reinterpret<reg::COHER_STATUS_HOST>(
      uint32_t(regs_volatile[XE_GPU_REG_COHER_STATUS_HOST]));
  uint32_t base_host = regs_volatile[XE_GPU_REG_COHER_BASE_HOST];
  uint32_t size_host = regs_volatile[XE_GPU_REG_COHER_SIZE_HOST];

  if (!status_host.status) {
    return;
  }

  const char* action = "N/A";
  if (status_host.vc_action_ena && status_host.tc_action_ena) {
    action = "VC | TC";
  } else if (status_host.tc_action_ena) {
    action = "TC";
  } else if (status_host.vc_action_ena) {
    action = "VC";
  }

  // TODO(benvanik): notify resource cache of base->size and type.
  REXGPU_TRACE("Make {:08X} -> {:08X} ({}b) coherent, action = {}", base_host,
               base_host + size_host, size_host, action);

  // Mark coherent.
  regs_volatile[XE_GPU_REG_COHER_STATUS_HOST] = 0;
}

void CommandProcessor::PrepareForWait() {
  trace_writer_.Flush();
}

void CommandProcessor::ReturnFromWait() {}

uint32_t CommandProcessor::ExecutePrimaryBuffer(uint32_t read_index, uint32_t write_index) {
#if REX_GPU_DIAGNOSTICS
  pc_command_execution::State::Scope execution_scope(command_execution_);
#endif
  SCOPE_profile_cpu_f("gpu");

  // If we have a pending trace stream open it now. That way we ensure we get
  // all commands.
  if (!trace_writer_.is_open() && trace_state_ == TraceState::kStreaming) {
    uint32_t title_id = kernel_state_ && kernel_state_->GetExecutableModule()
                            ? kernel_state_->GetExecutableModule()->title_id()
                            : 0;
    auto file_name = fmt::format("{:08X}_stream.xtr", title_id);
    auto path = trace_stream_path_ / file_name;
    trace_writer_.Open(path, title_id);
    InitializeTrace();
  }

  // Adjust pointer base.
  uint32_t start_ptr = primary_buffer_ptr_ + read_index * sizeof(uint32_t);
  start_ptr = (primary_buffer_ptr_ & ~0x1FFFFFFF) | (start_ptr & 0x1FFFFFFF);
  uint32_t end_ptr = primary_buffer_ptr_ + write_index * sizeof(uint32_t);
  end_ptr = (primary_buffer_ptr_ & ~0x1FFFFFFF) | (end_ptr & 0x1FFFFFFF);

  trace_writer_.WritePrimaryBufferStart(start_ptr, write_index - read_index);

  // Execute commands!
  memory::RingBuffer reader(memory_->TranslatePhysical(primary_buffer_ptr_), primary_buffer_size_);
#if REX_GPU_DIAGNOSTICS
  // Ring publication tracking (measurement builds) matches root packets.
  struct PrimaryReaderScope {
    memory::RingBuffer*& slot;
    memory::RingBuffer* saved;
    ~PrimaryReaderScope() { slot = saved; }
  } primary_reader_scope{executing_primary_reader_, executing_primary_reader_};
  executing_primary_reader_ = &reader;
#endif
  reader.set_read_offset(read_index * sizeof(uint32_t));
  reader.set_write_offset(write_index * sizeof(uint32_t));
  do {
    if (!ExecutePacket(&reader)) {
      // This probably should be fatal - but we're going to continue anyways.
      REXGPU_ERROR("**** PRIMARY RINGBUFFER: Failed to execute packet.");
      assert_always();
      break;
    }
  } while (reader.read_count());

  OnPrimaryBufferEnd();

  trace_writer_.WritePrimaryBufferEnd();

  return write_index;
}

bool CommandProcessor::CanUseLeanIndirectBuffers() const {
  return REXCVAR_GET(gpu_lean_indirect_buffers) && !track_ring_publications_ &&
         !(kGpuDiagnostics && cp_cadence_.active) && !trace_writer_.is_open();
}

void CommandProcessor::ExecuteIndirectBufferLean(uint32_t ptr, uint32_t count) {
  auto* words = memory_->TranslatePhysical<uint32_t*>(ptr);
  if (!words) return;
  RegisterFile& regs = *register_file_;
  uint32_t pos = 0;
  while (pos < count) {
    const uint32_t packet = rex::byte_swap(words[pos]);
    if (packet == 0) {
      ++pos;
      continue;
    }
    const uint32_t packet_type = packet >> 30;
    if (packet_type == 0) {
      const uint32_t register_count = ((packet >> 16) & 0x3FFF) + 1;
      if (register_count > count - pos - 1) {
        REXGPU_ERROR("ExecutePacketType0 overflow (read count {:08X}, packet count {:08X})",
                     (count - pos - 1) * sizeof(uint32_t), register_count * sizeof(uint32_t));
        assert_always();
        return;
      }
      if (kGpuDiagnostics) swap_intervals_.AddType0(register_count);
      const uint32_t base_index = packet & 0x7FFF;
      const bool write_one_reg = (packet >> 15) & 0x1;
      uint32_t* data = words + pos + 1;
      const uint64_t last_index = uint64_t(base_index) + register_count - 1;
      // The same bulk writer the ring path uses for these ranges
      // (WriteRegisterRangeFromRing is WriteRegistersFromMem per span).
      const bool float_range = register_count >= 4 &&
                               base_index >= XE_GPU_REG_SHADER_CONSTANT_000_X &&
                               last_index <= XE_GPU_REG_SHADER_CONSTANT_511_W;
      const bool binding_range =
          register_write_fast_path_ &&
          ((base_index >= XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 &&
            last_index <= XE_GPU_REG_SHADER_CONSTANT_FETCH_31_5) ||
           (base_index >= XE_GPU_REG_SHADER_CONSTANT_BOOL_000_031 &&
            last_index <= XE_GPU_REG_SHADER_CONSTANT_LOOP_31));
      if (!write_one_reg && (float_range || binding_range)) {
        WriteRegistersFromMem(base_index, data, register_count);
      } else {
        for (uint32_t m = 0; m < register_count; ++m) {
          const uint32_t reg_data = rex::byte_swap(data[m]);
          const uint32_t target_index = write_one_reg ? base_index : base_index + m;
          if (register_write_fast_path_ && target_index < RegisterFile::kRegisterCount &&
              !register_write_handler_[target_index]) {
            // Volatile for the WAIT_REG_MEM loop, as on the ring path.
            const_cast<volatile uint32_t&>(regs.values[target_index]) = reg_data;
            continue;
          }
          if (kGpuDiagnostics && !kernel_state_ &&
              target_index >= XE_GPU_REG_SHADER_CONSTANT_000_X &&
              target_index < XE_GPU_REG_SHADER_CONSTANT_000_X + 11 * 4 &&
              (reg_data & 0x7F800000u) == 0x7F800000u && (reg_data & 0x007FFFFFu)) {
            ReportEmbeddedNanConstantSource(packet, reinterpret_cast<uintptr_t>(words + pos),
                                            reinterpret_cast<uintptr_t>(data + m), base_index,
                                            register_count, write_one_reg, target_index, reg_data);
          }
          WriteRegisterFromPacket(target_index, reg_data, UINT32_MAX, UINT32_MAX, nullptr);
        }
      }
      pos += 1 + register_count;
      continue;
    }
    // Types 1-3 keep their handlers; positions are computed here because a
    // reader that ends exactly at the buffer end wraps its offset to 0.
    uint32_t packet_words = 1;
    if (packet_type == 1) {
      packet_words = 3;
    } else if (packet_type == 3) {
      packet_words = 1 + ((packet >> 16) & 0x3FFF) + 1;
    }
    if (packet_words > count - pos) {
      REXGPU_ERROR("**** INDIRECT RINGBUFFER: packet overflows the buffer.");
      assert_always();
      return;
    }
    memory::RingBuffer reader(reinterpret_cast<uint8_t*>(words), size_t(count) * sizeof(uint32_t));
    reader.set_write_offset(size_t(count) * sizeof(uint32_t));
    reader.set_read_offset(size_t(pos) * sizeof(uint32_t));
    bool executed;
    if (packet_type == 3 &&
        !(kGpuDiagnostics && swap_intervals_.active &&
          ((packet >> 8) & 0x7F) == PM4_EVENT_WRITE_ZPD)) {
      reader.AdvanceRead(sizeof(uint32_t));
      executed = ExecutePacketType3(&reader, packet);
    } else {
      // Type 1/2 and a timed ZPD event take the ordinary dispatcher.
      executed = ExecutePacket(&reader);
    }
    if (!executed) {
      REXGPU_ERROR("**** INDIRECT RINGBUFFER: Failed to execute packet.");
      assert_always();
      return;
    }
    pos += packet_words;
  }
}

void CommandProcessor::ExecuteIndirectBuffer(uint32_t ptr, uint32_t count) {
  if (CanUseLeanIndirectBuffers()) {
    ExecuteIndirectBufferLean(ptr, count);
    return;
  }
#if REX_GPU_DIAGNOSTICS
  pc_command_execution::State::Scope execution_scope(command_execution_);
#endif
  SCOPE_profile_cpu_f("gpu");

  trace_writer_.WriteIndirectBufferStart(ptr, count * sizeof(uint32_t));

  // Execute commands!
  memory::RingBuffer reader(memory_->TranslatePhysical(ptr), count * sizeof(uint32_t));
  reader.set_write_offset(count * sizeof(uint32_t));
  do {
    if (!ExecutePacket(&reader)) {
      // Return up a level if we encounter a bad packet.
      REXGPU_ERROR("**** INDIRECT RINGBUFFER: Failed to execute packet.");
      assert_always();
      break;
    }
  } while (reader.read_count());

  trace_writer_.WriteIndirectBufferEnd();
}

void CommandProcessor::ExecutePacket(uint32_t ptr, uint32_t count) {
#if REX_GPU_DIAGNOSTICS
  pc_command_execution::State::Scope execution_scope(command_execution_);
#endif
  // Execute commands!
  memory::RingBuffer reader(memory_->TranslatePhysical(ptr), count * sizeof(uint32_t));
  reader.set_write_offset(count * sizeof(uint32_t));
  do {
    if (!ExecutePacket(&reader)) {
      REXGPU_ERROR("**** ExecutePacket: Failed to execute packet.");
      assert_always();
      break;
    }
  } while (reader.read_count());
}

bool CommandProcessor::ExecutePacket(memory::RingBuffer* reader) {
  if (kGpuDiagnostics) command_execution_.BeginPacket();
  const uint32_t packet_read_index = reader->read_offset() / sizeof(uint32_t);
  const uint32_t available_words = reader->read_count() / sizeof(uint32_t);
  const uint32_t packet = reader->ReadAndSwap<uint32_t>();
  if (track_ring_publications_ && reader == executing_primary_reader_) {
    std::lock_guard lock(ring_publication_mutex_);
    const uint32_t words = pc_ring_publication::PacketWords(packet);
    if (words > available_words) ring_publications_.Disable();
    command_execution_.SetRootPublication(
        ring_publications_.Consume(packet_read_index, words));
  }
  const uint32_t packet_type = packet >> 30;
  if (packet == 0) {
    trace_writer_.WritePacketStart(uint32_t(reader->read_ptr() - 4), 1);
    trace_writer_.WritePacketEnd();
    return true;
  }

  if (packet == 0xCDCDCDCD) {
    REXGPU_WARN("GPU packet is CDCDCDCD - probably read uninitialized memory!");
  }

  // Ordinary packets (no sparse CP window, no interval ZPD timing) dispatch
  // without constructing the timing scope below. Player builds always do:
  // the scope and its large stack frame compile out.
  if (!kGpuDiagnostics ||
      (!cp_cadence_.active &&
       !(swap_intervals_.active && packet_type == 3 &&
         ((packet >> 8) & 0x7F) == PM4_EVENT_WRITE_ZPD))) {
    switch (packet_type) {
      case 0x00:
        return ExecutePacketType0(reader, packet);
      case 0x01:
        return ExecutePacketType1(reader, packet);
      case 0x02:
        return ExecutePacketType2(reader, packet);
      case 0x03:
        return ExecutePacketType3(reader, packet);
      default:
        assert_unhandled_case(packet_type);
        return false;
    }
  }

  // No packet timing on ordinary launches. The selected sparse window measures
  // only handlers not already attributed as WAIT, draw, swap or nested indirect
  // work. The scope covers early returns without logging per packet.
  uint32_t packet_work_category = 4;
  uint32_t packet_work_opcode = 128;
  if (cp_cadence_.active) {
    if (packet_type < 3) {
      packet_work_category = packet_type;
    } else {
      const uint32_t opcode = (packet >> 8) & 0x7F;
      if (opcode != PM4_WAIT_REG_MEM && opcode != PM4_XE_SWAP &&
          opcode != PM4_INDIRECT_BUFFER && opcode != PM4_INDIRECT_BUFFER_PFD &&
          opcode != PM4_DRAW_INDX && opcode != PM4_DRAW_INDX_2) {
        packet_work_category = 3;
        packet_work_opcode = opcode;
      }
    }
  }
  const bool interval_zpd = swap_intervals_.active && packet_type == 3 &&
      ((packet >> 8) & 0x7F) == PM4_EVENT_WRITE_ZPD;
  struct SparsePacketWorkScope {
    CpCadenceDiagnostic* diagnostic;
    SwapIntervalDiagnostic* interval_diagnostic;
    uint32_t category;
    uint32_t opcode;
    uint64_t begin_tick;
    ~SparsePacketWorkScope() {
      if (diagnostic || interval_diagnostic) {
        const uint64_t ticks = rex::chrono::Clock::QueryHostTickCount() - begin_tick;
        if (diagnostic) diagnostic->PacketWorkTime(category, ticks, opcode);
        if (interval_diagnostic) interval_diagnostic->AddZpd(ticks);
      }
    }
  } sparse_packet_work_scope{
      packet_work_category < 4 ? &cp_cadence_ : nullptr,
      interval_zpd ? &swap_intervals_ : nullptr,
      packet_work_category,
      packet_work_opcode,
      (packet_work_category < 4 || interval_zpd)
          ? rex::chrono::Clock::QueryHostTickCount() : 0};

  switch (packet_type) {
    case 0x00:
      return ExecutePacketType0(reader, packet);
    case 0x01:
      return ExecutePacketType1(reader, packet);
    case 0x02:
      return ExecutePacketType2(reader, packet);
    case 0x03:
      return ExecutePacketType3(reader, packet);
    default:
      assert_unhandled_case(packet_type);
      return false;
  }
}

bool CommandProcessor::ExecutePacketType0(memory::RingBuffer* reader, uint32_t packet) {
  // Type-0 packet.
  // Write count registers in sequence to the registers starting at
  // (base_index << 2).

  uint32_t count = ((packet >> 16) & 0x3FFF) + 1;
  if (reader->read_count() < count * sizeof(uint32_t)) {
    REXGPU_ERROR("ExecutePacketType0 overflow (read count {:08X}, packet count {:08X})",
                 reader->read_count(), count * sizeof(uint32_t));
    return false;
  }
  if (kGpuDiagnostics) swap_intervals_.AddType0(count);

  const uintptr_t packet_host_address = reader->read_ptr() - 4;
  trace_writer_.WritePacketStart(uint32_t(packet_host_address), 1 + count);

  uint32_t base_index = (packet & 0x7FFF);
  uint32_t write_one_reg = (packet >> 15) & 0x1;
  // The D3D12 bulk writer already preserves shader-constant binding
  // invalidation and handles wrapped ring reads. Use it only for sequential
  // float constants when no per-word provenance is required. Repeated-register
  // packets and all other register ranges retain the ordinary write path.
  if (!track_ring_publications_ && !write_one_reg && count >= 4 &&
      base_index >= XE_GPU_REG_SHADER_CONSTANT_000_X &&
      uint64_t(base_index) + count - 1 <= XE_GPU_REG_SHADER_CONSTANT_511_W) {
    if (kGpuDiagnostics && cp_cadence_.active) {
      ++cp_cadence_.type0_bulk_packets;
      cp_cadence_.type0_bulk_words += count;
    }
    WriteRegisterRangeFromRing(reader, base_index, count);
    trace_writer_.WritePacketEnd();
    return true;
  }
  if (!track_ring_publications_ && register_write_fast_path_) {
    const uint64_t last_index = uint64_t(base_index) + count - 1;
    if (!write_one_reg &&
        ((base_index >= XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 &&
          last_index <= XE_GPU_REG_SHADER_CONSTANT_FETCH_31_5) ||
         (base_index >= XE_GPU_REG_SHADER_CONSTANT_BOOL_000_031 &&
          last_index <= XE_GPU_REG_SHADER_CONSTANT_LOOP_31))) {
      // The backend's range writer applies the same binding, texture and
      // vertex-buffer invalidation as the per-register path, once per range.
      WriteRegisterRangeFromRing(reader, base_index, count);
      trace_writer_.WritePacketEnd();
      return true;
    }
    RegisterFile& regs = *register_file_;
    for (uint32_t m = 0; m < count; ++m) {
      const uintptr_t data_host_address = reader->read_ptr();
      const uint32_t reg_data = reader->ReadAndSwap<uint32_t>();
      const uint32_t target_index = write_one_reg ? base_index : base_index + m;
      if (target_index < RegisterFile::kRegisterCount && !register_write_handler_[target_index]) {
        // No side effects anywhere: the store WriteRegister would make.
        // Volatile for the WAIT_REG_MEM loop.
        const_cast<volatile uint32_t&>(regs.values[target_index]) = reg_data;
        continue;
      }
      if (kGpuDiagnostics && !kernel_state_ && target_index >= XE_GPU_REG_SHADER_CONSTANT_000_X &&
          target_index < XE_GPU_REG_SHADER_CONSTANT_000_X + 11 * 4 &&
          (reg_data & 0x7F800000u) == 0x7F800000u && (reg_data & 0x007FFFFFu)) {
        ReportEmbeddedNanConstantSource(packet, packet_host_address, data_host_address,
                                        base_index, count, write_one_reg, target_index,
                                        reg_data);
      }
      WriteRegisterFromPacket(target_index, reg_data, UINT32_MAX, UINT32_MAX, nullptr);
    }
    trace_writer_.WritePacketEnd();
    return true;
  }
  const bool track_provenance = track_ring_publications_;
  std::unique_ptr<uint32_t[]> nested_owned_words;
  uint32_t* owned_words = nullptr;
  bool release_reusable_owned_words = false;
  if (track_provenance) {
    if (!type0_owned_words_in_use_) {
      if (!type0_owned_words_) {
        type0_owned_words_ = std::make_unique<uint32_t[]>(1024);
      }
      type0_owned_words_in_use_ = true;
      release_reusable_owned_words = true;
      owned_words = type0_owned_words_.get();
    } else {
      // Preserve the original stack copy's isolation if a tracked packet ever
      // re-enters this handler before its outer words have been consumed.
      nested_owned_words = std::make_unique<uint32_t[]>(1024);
      owned_words = nested_owned_words.get();
    }
  }
  struct ReusableOwnedWordsRelease {
    bool* in_use;
    ~ReusableOwnedWordsRelease() { if (in_use) *in_use = false; }
  } reusable_owned_words_release{
      release_reusable_owned_words ? &type0_owned_words_in_use_ : nullptr};
  pc_owned_camera_packet::Source owned_source;
  const uintptr_t source_base = track_provenance
      ? reinterpret_cast<uintptr_t>(memory_->physical_membase()) : 0;
  const uint32_t packet_physical = track_provenance
      ? pc_constant_writer::PhysicalWord(packet_host_address, source_base, 0x20000000u)
      : UINT32_MAX;
  // Reject wrapped readers: a contiguous physical range alone does not prove
  // that the reader's next payload words occupy that range. The host callback
  // copies payload AND provenance under one memory guard, then releases it.
  const bool owned = track_provenance && pc_owned_camera_packet::CopyContiguous(owned_camera_packet_callbacks_,
      packet_physical, packet, count, reader->read_offset(), reader->capacity(),
      owned_words, &owned_source);
  for (uint32_t m = 0; m < count; m++) {
    const uintptr_t data_host_address = reader->read_ptr();
    uint32_t reg_data;
    if (owned) {
      reg_data = owned_words[m];
      reader->AdvanceRead(sizeof(uint32_t));
    } else {
      reg_data = reader->ReadAndSwap<uint32_t>();
    }
    uint32_t target_index = write_one_reg ? base_index : base_index + m;
    if (kGpuDiagnostics && !kernel_state_ && target_index >= XE_GPU_REG_SHADER_CONSTANT_000_X &&
        target_index < XE_GPU_REG_SHADER_CONSTANT_000_X + 11 * 4 &&
        (reg_data & 0x7F800000u) == 0x7F800000u && (reg_data & 0x007FFFFFu)) {
      ReportEmbeddedNanConstantSource(packet, packet_host_address, data_host_address, base_index,
                                      count, write_one_reg, target_index, reg_data);
    }
    WriteRegisterFromPacket(target_index, reg_data,
        packet_physical,
        track_provenance
            ? pc_constant_writer::PhysicalWord(data_host_address, source_base, 0x20000000u)
            : UINT32_MAX,
        owned ? &owned_source : nullptr);
  }

  trace_writer_.WritePacketEnd();
  return true;
}

bool CommandProcessor::ExecutePacketType1(memory::RingBuffer* reader, uint32_t packet) {
  // Type-1 packet.
  // Contains two registers of data. Type-0 should be more common.
  trace_writer_.WritePacketStart(uint32_t(reader->read_ptr() - 4), 3);
  uint32_t reg_index_1 = packet & 0x7FF;
  uint32_t reg_index_2 = (packet >> 11) & 0x7FF;
  uint32_t reg_data_1 = reader->ReadAndSwap<uint32_t>();
  uint32_t reg_data_2 = reader->ReadAndSwap<uint32_t>();
  WriteRegister(reg_index_1, reg_data_1);
  WriteRegister(reg_index_2, reg_data_2);
  trace_writer_.WritePacketEnd();
  return true;
}

bool CommandProcessor::ExecutePacketType2(memory::RingBuffer* reader, uint32_t packet) {
  // Type-2 packet.
  // No-op. Do nothing.
  trace_writer_.WritePacketStart(uint32_t(reader->read_ptr() - 4), 1);
  trace_writer_.WritePacketEnd();
  return true;
}

bool CommandProcessor::ExecutePacketType3(memory::RingBuffer* reader, uint32_t packet) {
  // Type-3 packet.
  uint32_t opcode = (packet >> 8) & 0x7F;
  uint32_t count = ((packet >> 16) & 0x3FFF) + 1;
  auto data_start_offset = reader->read_offset();

  if (reader->read_count() < count * sizeof(uint32_t)) {
    REXGPU_ERROR("ExecutePacketType3 overflow (read count {:08X}, packet count {:08X})",
                 reader->read_count(), count * sizeof(uint32_t));
    return false;
  }

  // To handle nesting behavior when tracing we special case indirect buffers.
  if (opcode == PM4_INDIRECT_BUFFER) {
    trace_writer_.WritePacketStart(uint32_t(reader->read_ptr() - 4), 2);
  } else {
    trace_writer_.WritePacketStart(uint32_t(reader->read_ptr() - 4), 1 + count);
  }

  // & 1 == predicate - when set, we do bin check to see if we should execute
  // the packet. Only type 3 packets are affected.
  // We also skip predicated swaps, as they are never valid (probably?).
  if (packet & 1) {
    bool any_pass = (bin_select_ & bin_mask_) != 0;
    if (!any_pass || opcode == PM4_XE_SWAP) {
      OnPredicatedPacketSkipped(opcode);
      reader->AdvanceRead(count * sizeof(uint32_t));
      trace_writer_.WritePacketEnd();
      return true;
    }
  }

  bool result = false;
  switch (opcode) {
    case PM4_ME_INIT:
      result = ExecutePacketType3_ME_INIT(reader, packet, count);
      break;
    case PM4_NOP:
      result = ExecutePacketType3_NOP(reader, packet, count);
      break;
    case PM4_INTERRUPT:
      result = ExecutePacketType3_INTERRUPT(reader, packet, count);
      break;
    case PM4_XE_SWAP:
      result = ExecutePacketType3_XE_SWAP(reader, packet, count);
      break;
    case PM4_INDIRECT_BUFFER:
    case PM4_INDIRECT_BUFFER_PFD:
      result = ExecutePacketType3_INDIRECT_BUFFER(reader, packet, count);
      break;
    case PM4_WAIT_REG_MEM:
      result = ExecutePacketType3_WAIT_REG_MEM(reader, packet, count);
      break;
    case PM4_REG_RMW:
      result = ExecutePacketType3_REG_RMW(reader, packet, count);
      break;
    case PM4_REG_TO_MEM:
      result = ExecutePacketType3_REG_TO_MEM(reader, packet, count);
      break;
    case PM4_MEM_WRITE:
      result = ExecutePacketType3_MEM_WRITE(reader, packet, count);
      break;
    case PM4_COND_WRITE:
      result = ExecutePacketType3_COND_WRITE(reader, packet, count);
      break;
    case PM4_EVENT_WRITE:
      result = ExecutePacketType3_EVENT_WRITE(reader, packet, count);
      break;
    case PM4_EVENT_WRITE_SHD:
      result = ExecutePacketType3_EVENT_WRITE_SHD(reader, packet, count);
      break;
    case PM4_EVENT_WRITE_EXT:
      result = ExecutePacketType3_EVENT_WRITE_EXT(reader, packet, count);
      break;
    case PM4_EVENT_WRITE_ZPD:
      result = ExecutePacketType3_EVENT_WRITE_ZPD(reader, packet, count);
      break;
    case PM4_DRAW_INDX:
      result = ExecutePacketType3_DRAW_INDX(reader, packet, count);
      break;
    case PM4_DRAW_INDX_2:
      result = ExecutePacketType3_DRAW_INDX_2(reader, packet, count);
      break;
    case PM4_SET_CONSTANT:
      result = ExecutePacketType3_SET_CONSTANT(reader, packet, count);
      break;
    case PM4_SET_CONSTANT2:
      result = ExecutePacketType3_SET_CONSTANT2(reader, packet, count);
      break;
    case PM4_LOAD_ALU_CONSTANT:
      result = ExecutePacketType3_LOAD_ALU_CONSTANT(reader, packet, count);
      break;
    case PM4_SET_SHADER_CONSTANTS:
      result = ExecutePacketType3_SET_SHADER_CONSTANTS(reader, packet, count);
      break;
    case PM4_IM_LOAD:
      result = ExecutePacketType3_IM_LOAD(reader, packet, count);
      break;
    case PM4_IM_LOAD_IMMEDIATE:
      result = ExecutePacketType3_IM_LOAD_IMMEDIATE(reader, packet, count);
      break;
    case PM4_INVALIDATE_STATE:
      result = ExecutePacketType3_INVALIDATE_STATE(reader, packet, count);
      break;
    case PM4_VIZ_QUERY:
      result = ExecutePacketType3_VIZ_QUERY(reader, packet, count);
      break;

    case PM4_SET_BIN_MASK_LO: {
      uint32_t value = reader->ReadAndSwap<uint32_t>();
      bin_mask_ = (bin_mask_ & 0xFFFFFFFF00000000ull) | value;
      result = true;
    } break;
    case PM4_SET_BIN_MASK_HI: {
      uint32_t value = reader->ReadAndSwap<uint32_t>();
      bin_mask_ = (bin_mask_ & 0xFFFFFFFFull) | (static_cast<uint64_t>(value) << 32);
      result = true;
    } break;
    case PM4_SET_BIN_SELECT_LO: {
      uint32_t value = reader->ReadAndSwap<uint32_t>();
      bin_select_ = (bin_select_ & 0xFFFFFFFF00000000ull) | value;
      result = true;
    } break;
    case PM4_SET_BIN_SELECT_HI: {
      uint32_t value = reader->ReadAndSwap<uint32_t>();
      bin_select_ = (bin_select_ & 0xFFFFFFFFull) | (static_cast<uint64_t>(value) << 32);
      result = true;
    } break;
    case PM4_SET_BIN_MASK: {
      assert_true(count == 2);
      uint64_t val_hi = reader->ReadAndSwap<uint32_t>();
      uint64_t val_lo = reader->ReadAndSwap<uint32_t>();
      bin_mask_ = (val_hi << 32) | val_lo;
      result = true;
    } break;
    case PM4_SET_BIN_SELECT: {
      assert_true(count == 2);
      uint64_t val_hi = reader->ReadAndSwap<uint32_t>();
      uint64_t val_lo = reader->ReadAndSwap<uint32_t>();
      bin_select_ = (val_hi << 32) | val_lo;
      result = true;
    } break;
    case PM4_CONTEXT_UPDATE: {
      assert_true(count == 1);
      uint32_t value = reader->ReadAndSwap<uint32_t>();
      REXGPU_INFO("GPU context update = {:08X}", value);
      assert_true(value == 0);
      result = true;
      break;
    }
    case PM4_WAIT_FOR_IDLE: {
      // This opcode is used by 5454084E while going / being ingame.
      assert_true(count == 1);
      uint32_t value = reader->ReadAndSwap<uint32_t>();
      REXGPU_INFO("GPU wait for idle = {:08X}", value);
      result = true;
      break;
    }

    default:
      REXGPU_INFO("Unimplemented GPU OPCODE: 0x{:02X}\t\tCOUNT: {}\n", opcode, count);
      assert_always();
      reader->AdvanceRead(count * sizeof(uint32_t));
      break;
  }

  trace_writer_.WritePacketEnd();
  if (opcode == PM4_XE_SWAP) {
    // End the trace writer frame.
    if (trace_writer_.is_open()) {
      trace_writer_.WriteEvent(EventCommand::Type::kSwap);
      trace_writer_.Flush();
      if (trace_state_ == TraceState::kSingleFrame) {
        trace_state_ = TraceState::kDisabled;
        trace_writer_.Close();
      }
    } else if (trace_state_ == TraceState::kSingleFrame) {
      // New trace request - we only start tracing at the beginning of a frame.
      uint32_t title_id = kernel_state_ && kernel_state_->GetExecutableModule()
                              ? kernel_state_->GetExecutableModule()->title_id()
                              : 0;
      auto file_name = fmt::format("{:08X}_{}.xtr", title_id, counter_ - 1);
      auto path = trace_frame_path_ / file_name;
      trace_writer_.Open(path, title_id);
      InitializeTrace();
    }
  }

  assert_true(reader->read_offset() ==
              (data_start_offset + (count * sizeof(uint32_t))) % reader->capacity());
  if (!result && !kernel_state_) {
    // The embedded host doesn't initialize ReXGlue's logging frontend. Keep
    // packet rejection visible at the C ABI boundary instead of allowing the
    // primary-ring read pointer to make a failed packet look successful.
    std::fprintf(stderr,
                 "REX_EMBEDDED_PACKET_FAILURE opcode=0x%02X count=%u "
                 "data_offset=0x%08X\n",
                 opcode, count, data_start_offset);
    std::fflush(stderr);
  }
  return result;
}

bool CommandProcessor::ExecutePacketType3_ME_INIT(memory::RingBuffer* reader, uint32_t packet,
                                                  uint32_t count) {
  // initialize CP's micro-engine
  me_bin_.clear();
  for (uint32_t i = 0; i < count; i++) {
    me_bin_.push_back(reader->ReadAndSwap<uint32_t>());
  }

  return true;
}

bool CommandProcessor::ExecutePacketType3_NOP(memory::RingBuffer* reader, uint32_t packet,
                                              uint32_t count) {
  // skip N 32-bit words to get to the next packet
  // No-op, ignore some data.
  reader->AdvanceRead(count * sizeof(uint32_t));
  return true;
}

bool CommandProcessor::ExecutePacketType3_INTERRUPT(memory::RingBuffer* reader, uint32_t packet,
                                                    uint32_t count) {
  SCOPE_profile_cpu_f("gpu");

  // generate interrupt from the command stream
  uint32_t cpu_mask = reader->ReadAndSwap<uint32_t>();
  SettleGuestVisibleWork();
  static uint64_t embedded_interrupt_ordinal = 0;
  const uint64_t interrupt_ordinal = ++embedded_interrupt_ordinal;
  if (kGpuDiagnostics && (interrupt_ordinal <= 16 || !(interrupt_ordinal & 4095))) {
    std::fprintf(stderr,
                 "REX_PM4_INTERRUPT ordinal=%llu cpu_mask=0x%08X count=%u\n",
                 static_cast<unsigned long long>(interrupt_ordinal), cpu_mask, count);
    std::fflush(stderr);
    TraceEmbeddedCommandProcessor(
        "REX_PM4_INTERRUPT ordinal=%llu cpu_mask=0x%08X count=%u",
        static_cast<unsigned long long>(interrupt_ordinal), cpu_mask, count);
  }
  for (int n = 0; n < 6; n++) {
    if (cpu_mask & (1 << n)) {
      if (graphics_system_) {
        graphics_system_->DispatchInterruptCallback(1, n);
      }
    }
  }
  return true;
}

void CommandProcessor::LogSwapIntervalWindow(uint64_t frequency, bool closed_by_marker) {
  std::string bins;
  bins.reserve(390);
  for (uint32_t i = 0; i <= SwapIntervalDiagnostic::kOverflowBin; ++i) {
    if (i) bins += ',';
    bins += std::to_string(swap_intervals_.histogram_ms[i]);
  }
  uint64_t tsc_hz = 0;
#if defined(_WIN32)
  // Thread cycle times advance at the invariant TSC rate; calibrate it against
  // the host tick over the whole observed lifetime.
  const uint64_t now_tick = rex::chrono::Clock::QueryHostTickCount();
  const uint64_t now_tsc = __rdtsc();
  if (swap_diag_start_tick_ && now_tick > swap_diag_start_tick_ &&
      now_tsc > swap_diag_start_tsc_) {
    tsc_hz = uint64_t(double(now_tsc - swap_diag_start_tsc_) * double(frequency) /
                      double(now_tick - swap_diag_start_tick_));
  }
#endif
  TraceEmbeddedCommandProcessor(
      "REX_SWAP_INTERVAL_WINDOW first_swap=%llu last_swap=%llu count=%llu "
      "first_tick=%llu last_tick=%llu frequency=%llu sum_us=%llu "
      "min_us=%llu max_us=%llu over_16667_us=%llu over_33334_us=%llu "
      "max_doubled_run=%llu marker_closed=%u tsc_hz=%llu histogram_ms=%s",
      static_cast<unsigned long long>(swap_intervals_.first_swap),
      static_cast<unsigned long long>(swap_intervals_.last_swap),
      static_cast<unsigned long long>(swap_intervals_.count),
      static_cast<unsigned long long>(swap_intervals_.first_tick),
      static_cast<unsigned long long>(swap_intervals_.last_tick),
      static_cast<unsigned long long>(frequency),
      static_cast<unsigned long long>(swap_intervals_.sum_us),
      static_cast<unsigned long long>(swap_intervals_.min_us),
      static_cast<unsigned long long>(swap_intervals_.max_us),
      static_cast<unsigned long long>(swap_intervals_.over_16667_us),
      static_cast<unsigned long long>(swap_intervals_.over_33334_us),
      static_cast<unsigned long long>(swap_intervals_.max_doubled_run),
      closed_by_marker ? 1u : 0u, static_cast<unsigned long long>(tsc_hz),
      bins.c_str());
  if (const std::string backend = SwapIntervalBackendStats(); !backend.empty()) {
    TraceEmbeddedCommandProcessor("%s last_swap=%llu", backend.c_str(),
                                  static_cast<unsigned long long>(swap_intervals_.last_swap));
  }
  for (uint32_t group_index = 0; group_index < 2; ++group_index) {
    const auto& group = group_index ? swap_intervals_.doubled : swap_intervals_.one_refresh;
    TraceEmbeddedCommandProcessor(
        "REX_SWAP_WORK_GROUP version=5 last_swap=%llu group=%u count=%llu "
        "elapsed_ticks=%llu idle_ticks=%llu wait_ticks=%llu "
        "issue_swap_ticks=%llu remaining_ticks=%llu waits=%llu "
        "accounting_invalid=%llu thread_cpu_100ns=%llu "
        "thread_cpu_samples=%llu thread_cycles=%llu thread_cycle_samples=%llu "
        "draws=%llu type0_packets=%llu "
        "type0_words=%llu zpd_calls=%llu zpd_ticks=%llu "
        "sampled_draws=%llu draw_pre_texture_ticks=%llu "
        "draw_pre_binding_ticks=%llu draw_post_binding_ticks=%llu "
        "draw_binding_ticks=%llu frequency=%llu",
        static_cast<unsigned long long>(swap_intervals_.last_swap), group_index,
        static_cast<unsigned long long>(group.count),
        static_cast<unsigned long long>(group.elapsed_ticks),
        static_cast<unsigned long long>(group.idle_ticks),
        static_cast<unsigned long long>(group.wait_ticks),
        static_cast<unsigned long long>(group.issue_swap_ticks),
        static_cast<unsigned long long>(group.remaining_ticks),
        static_cast<unsigned long long>(group.waits),
        static_cast<unsigned long long>(group.accounting_invalid),
        static_cast<unsigned long long>(group.thread_cpu_100ns),
        static_cast<unsigned long long>(group.thread_cpu_samples),
        static_cast<unsigned long long>(group.thread_cycles),
        static_cast<unsigned long long>(group.thread_cycle_samples),
        static_cast<unsigned long long>(group.draws),
        static_cast<unsigned long long>(group.type0_packets),
        static_cast<unsigned long long>(group.type0_words),
        static_cast<unsigned long long>(group.zpd_calls),
        static_cast<unsigned long long>(group.zpd_ticks),
        static_cast<unsigned long long>(group.sampled_draws),
        static_cast<unsigned long long>(group.sampled_draw_ticks[0]),
        static_cast<unsigned long long>(group.sampled_draw_ticks[1]),
        static_cast<unsigned long long>(group.sampled_draw_ticks[2]),
        static_cast<unsigned long long>(group.sampled_binding_ticks),
        static_cast<unsigned long long>(frequency));
    // Seven mutually exclusive portions of the sampled pre-texture draw
    // path. Counts may differ because early returns stop at their last
    // reached boundary; ticks are raw host ticks, not rounded microseconds.
    TraceEmbeddedCommandProcessor(
        "REX_SWAP_DRAW_PRE_TEXTURE version=1 last_swap=%llu group=%u "
        "pre_primitive=%llu,%llu primitive=%llu,%llu "
        "pre_target=%llu,%llu target=%llu,%llu "
        "pre_pipeline=%llu,%llu pipeline=%llu,%llu "
        "texture=%llu,%llu frequency=%llu",
        static_cast<unsigned long long>(swap_intervals_.last_swap), group_index,
        static_cast<unsigned long long>(group.sampled_pre_texture_part_counts[0]),
        static_cast<unsigned long long>(group.sampled_pre_texture_part_ticks[0]),
        static_cast<unsigned long long>(group.sampled_pre_texture_part_counts[1]),
        static_cast<unsigned long long>(group.sampled_pre_texture_part_ticks[1]),
        static_cast<unsigned long long>(group.sampled_pre_texture_part_counts[2]),
        static_cast<unsigned long long>(group.sampled_pre_texture_part_ticks[2]),
        static_cast<unsigned long long>(group.sampled_pre_texture_part_counts[3]),
        static_cast<unsigned long long>(group.sampled_pre_texture_part_ticks[3]),
        static_cast<unsigned long long>(group.sampled_pre_texture_part_counts[4]),
        static_cast<unsigned long long>(group.sampled_pre_texture_part_ticks[4]),
        static_cast<unsigned long long>(group.sampled_pre_texture_part_counts[5]),
        static_cast<unsigned long long>(group.sampled_pre_texture_part_ticks[5]),
        static_cast<unsigned long long>(group.sampled_pre_texture_part_counts[6]),
        static_cast<unsigned long long>(group.sampled_pre_texture_part_ticks[6]),
        static_cast<unsigned long long>(frequency));
    TraceEmbeddedCommandProcessor(
        "REX_SWAP_WORK_EXTRA version=1 last_swap=%llu group=%u upload_bytes=%llu "
        "upload_ranges=%llu max_frame_upload_bytes=%llu "
        "render_target_update_reuses=%llu",
        static_cast<unsigned long long>(swap_intervals_.last_swap), group_index,
        static_cast<unsigned long long>(group.upload_bytes),
        static_cast<unsigned long long>(group.upload_ranges),
        static_cast<unsigned long long>(group.max_frame_upload_bytes),
        static_cast<unsigned long long>(group.render_target_update_reuses));
  }
  const auto& zpd = swap_intervals_.zpd;
  TraceEmbeddedCommandProcessor(
      "REX_SWAP_ZPD version=1 last_swap=%llu ended=%llu published=%llu "
      "max_depth=%llu lag0=%llu lag1=%llu lag2=%llu lag3p=%llu "
      "slot_awaits=%llu write_awaits=%llu index_awaits=%llu "
      "blocking_awaits=%llu pending=%llu",
      static_cast<unsigned long long>(swap_intervals_.last_swap),
      static_cast<unsigned long long>(zpd.ended),
      static_cast<unsigned long long>(zpd.published),
      static_cast<unsigned long long>(zpd.max_depth),
      static_cast<unsigned long long>(zpd.lag_swaps[0]),
      static_cast<unsigned long long>(zpd.lag_swaps[1]),
      static_cast<unsigned long long>(zpd.lag_swaps[2]),
      static_cast<unsigned long long>(zpd.lag_swaps[3]),
      static_cast<unsigned long long>(zpd.slot_awaits),
      static_cast<unsigned long long>(zpd.write_awaits),
      static_cast<unsigned long long>(zpd.index_awaits),
      static_cast<unsigned long long>(zpd.blocking_awaits),
      static_cast<unsigned long long>(swap_intervals_.zpd_pending_now));
  TraceEmbeddedCommandProcessor(
      "REX_SWAP_WORK_FENCE version=2 last_swap=%llu long_threshold_us=%llu "
      "fence_wait_ticks=%llu,%llu full_syncs=%llu,%llu texture_creates=%llu,%llu "
      "texture_create_ticks=%llu,%llu long_frames=%u long_frames_dropped=%llu",
      static_cast<unsigned long long>(swap_intervals_.last_swap),
      static_cast<unsigned long long>(swap_intervals_.long_interval_us),
      static_cast<unsigned long long>(swap_intervals_.one_refresh.fence_wait_ticks),
      static_cast<unsigned long long>(swap_intervals_.doubled.fence_wait_ticks),
      static_cast<unsigned long long>(swap_intervals_.one_refresh.full_syncs),
      static_cast<unsigned long long>(swap_intervals_.doubled.full_syncs),
      static_cast<unsigned long long>(swap_intervals_.one_refresh.texture_creations),
      static_cast<unsigned long long>(swap_intervals_.doubled.texture_creations),
      static_cast<unsigned long long>(swap_intervals_.one_refresh.texture_create_ticks),
      static_cast<unsigned long long>(swap_intervals_.doubled.texture_create_ticks),
      swap_intervals_.long_frame_count,
      static_cast<unsigned long long>(swap_intervals_.long_frames_dropped));
  if (swap_intervals_.long_frame_count) {
    TraceSwapLongFrames(swap_intervals_, frequency, tsc_hz);
  }
  swap_intervals_.ResetWindow();
}

bool CommandProcessor::ExecutePacketType3_XE_SWAP(memory::RingBuffer* reader, uint32_t packet,
                                                  uint32_t count) {
  SCOPE_profile_cpu_f("gpu");
  SettleGuestVisibleWork();

  {
    const bool interval_enabled = kGpuDiagnostics &&
        !kernel_state_ && REXCVAR_GET(embedded_swap_interval_diagnostics);
    uint64_t thread_cpu_100ns = 0;
    bool thread_cpu_valid = false;
    uint64_t thread_cycles = 0;
    bool thread_cycles_valid = false;
#if defined(_WIN32)
    if (interval_enabled) {
      FILETIME created{}, exited{}, kernel{}, user{};
      if (::GetThreadTimes(::GetCurrentThread(), &created, &exited, &kernel, &user)) {
        const auto ticks = [](const FILETIME& value) {
          return (uint64_t(value.dwHighDateTime) << 32) | value.dwLowDateTime;
        };
        thread_cpu_100ns = ticks(kernel) + ticks(user);
        thread_cpu_valid = true;
      }
      ULONG64 cycles = 0;
      if (::QueryThreadCycleTime(::GetCurrentThread(), &cycles)) {
        thread_cycles = cycles;
        thread_cycles_valid = true;
      }
    }
#endif
    const uint64_t now = rex::chrono::Clock::QueryHostTickCount();
    const uint64_t frequency = rex::chrono::Clock::QueryHostTickFrequency();
#if defined(_WIN32)
    if (interval_enabled) {
      if (!swap_diag_start_tick_) {
        swap_diag_start_tick_ = now;
        swap_diag_start_tsc_ = __rdtsc();
      }
      // Isolated diagnostic sessions only: F9 marks a phase boundary (for
      // example stationary -> camera turn -> walk) and closes the current
      // window so each phase is aggregated separately.
      const bool marker_down = (::GetAsyncKeyState(VK_F9) & 0x8000) != 0;
      if (marker_down && !swap_phase_marker_down_) {
        ++swap_phase_markers_;
        TraceEmbeddedCommandProcessor(
            "REX_SWAP_PHASE_MARKER index=%u swap=%llu tick=%llu frequency=%llu",
            swap_phase_markers_,
            static_cast<unsigned long long>(
                guest_frame_count_.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(now),
            static_cast<unsigned long long>(frequency));
        if (swap_intervals_.active && swap_intervals_.count && frequency) {
          LogSwapIntervalWindow(frequency, true);
        }
      }
      swap_phase_marker_down_ = marker_down;
    }
#endif
    if (interval_enabled) {
      const int32_t long_frame_us = REXCVAR_GET(embedded_swap_long_frame_us);
      if (long_frame_us > 0 && !swap_long_frame_storage_) {
        swap_long_frame_storage_ = std::make_unique<SwapIntervalDiagnostic::LongFrame[]>(
            SwapIntervalDiagnostic::kLongFrameCapacity);
        swap_intervals_.long_frames = swap_long_frame_storage_.get();
        swap_intervals_.long_frame_capacity = SwapIntervalDiagnostic::kLongFrameCapacity;
      }
      swap_intervals_.capture_long_frames = long_frame_us > 0;
      swap_intervals_.long_interval_us = long_frame_us > 0
                                             ? uint64_t(long_frame_us)
                                             : SwapIntervalDiagnostic::kDoubledIntervalUs;
    }
    if (guest_frame_last_tick_) {
      const uint64_t elapsed_ticks = now - guest_frame_last_tick_;
      const uint64_t frame_time_us =
          frequency ? elapsed_ticks * 1000000 / frequency : 0;
      guest_frame_time_us_.store(frame_time_us, std::memory_order_relaxed);
      if (kGpuDiagnostics && frequency &&
          swap_intervals_.Observe(interval_enabled,
                                  guest_frame_count_.load(std::memory_order_relaxed) + 1,
                                  guest_frame_last_tick_, now, frame_time_us,
                                  thread_cpu_100ns, thread_cpu_valid,
                                  thread_cycles, thread_cycles_valid)) {
        LogSwapIntervalWindow(frequency, false);
      }
#ifdef REXGLUE_ENABLE_PERF_COUNTERS
      const int64_t dt_us = static_cast<int64_t>(frame_time_us);
      PROFILE_FRAME_TIME_US(dt_us);
      PROFILE_FPS(elapsed_ticks ? frequency / elapsed_ticks : 0);
#endif
    }
    if (interval_enabled && !swap_intervals_.active) {
      swap_intervals_.SetThreadCpuBaseline(thread_cpu_100ns, thread_cpu_valid);
      swap_intervals_.SetThreadCycleBaseline(thread_cycles, thread_cycles_valid);
    }
    guest_frame_last_tick_ = now;
    const uint64_t current_swap = guest_frame_count_.fetch_add(1, std::memory_order_relaxed) + 1;
    if (kGpuDiagnostics) {
      swap_intervals_.active = interval_enabled &&
          current_swap < 1 + SwapIntervalDiagnostic::kWindowSize *
                             SwapIntervalDiagnostic::kMaxWindows;
    }
  }
  rex::perf::Profiler::Flip();

  // Xenia-specific VdSwap hook.
  // VdSwap will post this to tell us we need to swap the screen/fire an
  // interrupt.
  // 63 words here, but only the first has any data.
  uint32_t magic = reader->ReadAndSwap<memory::fourcc_t>();
  assert_true(magic == kSwapSignature);

  // TODO(benvanik): only swap frontbuffer ptr.
  uint32_t frontbuffer_ptr = reader->ReadAndSwap<uint32_t>();
  uint32_t frontbuffer_width = reader->ReadAndSwap<uint32_t>();
  uint32_t frontbuffer_height = reader->ReadAndSwap<uint32_t>();
  reader->AdvanceRead((count - 4) * sizeof(uint32_t));

  const bool cadence_enabled = kGpuDiagnostics &&
      !kernel_state_ && REXCVAR_GET(embedded_cp_cadence_diagnostics);
  // counter_ is also advanced by VBlank and is not a swap ordinal.
  const uint64_t cadence_swap_ordinal =
      guest_frame_count_.load(std::memory_order_relaxed);
  if (cadence_enabled && cadence_swap_ordinal == 1) {
    TraceEmbeddedCommandProcessor(
        "REX_CP_CADENCE_BEGIN version=1 max_intervals=128 selector=real_XE_SWAP "
        "guest_timing_changed=0");
  }
  const bool interval_swap_active = kGpuDiagnostics && swap_intervals_.active;
  const uint64_t observed_swap_begin =
      ((kGpuDiagnostics && cp_cadence_.active) || interval_swap_active)
      ? rex::chrono::Clock::QueryHostTickCount() : 0;
  IssueSwap(frontbuffer_ptr, frontbuffer_width, frontbuffer_height);
  const uint64_t observed_swap_end =
      ((kGpuDiagnostics && cp_cadence_.active) || interval_swap_active)
      ? rex::chrono::Clock::QueryHostTickCount() : 0;
  if (interval_swap_active) {
    swap_intervals_.AddIssueSwap(observed_swap_end - observed_swap_begin);
  }
  if (kGpuDiagnostics && cp_cadence_.active) {
    const uint64_t end = observed_swap_end;
    const auto interrupt_batch = cp_interrupt_timing.Finish();
    const uint64_t frequency = rex::chrono::Clock::QueryHostTickFrequency();
    // Keep raw host ticks to avoid rounding and permit independent accounting.
    // 'other' includes draws/resources, dispatch and host scheduling, NOT GPU time.
    const uint64_t interval = observed_swap_begin - cp_cadence_.begin_tick;
    const uint64_t accounted = cp_cadence_.idle_ticks + cp_cadence_.wait_ticks;
    uint64_t submitted = 0, completed = 0;
    const bool submission_valid = QueryCadenceSubmission(submitted, completed);
    TraceEmbeddedCommandProcessor(
        "REX_CP_CADENCE completed_swap=%llu frequency=%llu begin_tick=%llu "
        "end_tick=%llu interval_ticks=%llu idle_ticks=%llu wait_ticks=%llu "
        "other_ticks=%llu accounting_valid=%u issue_swap_ticks=%llu "
        "waits=%llu max_wait_ticks=%llu wait_info=%08X wait_address=%08X "
        "wait_ref=%08X wait_mask=%08X wait_poll=%08X markers=%llu "
        "last_marker_tick=%llu last_marker_address=%08X last_marker_value=%08X "
        "host_submission_valid=%u host_submitted=%llu host_completed=%llu",
        static_cast<unsigned long long>(cadence_swap_ordinal),
        static_cast<unsigned long long>(frequency),
        static_cast<unsigned long long>(cp_cadence_.begin_tick),
        static_cast<unsigned long long>(end),
        static_cast<unsigned long long>(interval),
        static_cast<unsigned long long>(cp_cadence_.idle_ticks),
        static_cast<unsigned long long>(cp_cadence_.wait_ticks),
        static_cast<unsigned long long>(interval >= accounted ? interval - accounted : 0),
        interval >= accounted ? 1u : 0u,
        static_cast<unsigned long long>(end - observed_swap_begin),
        static_cast<unsigned long long>(cp_cadence_.waits),
        static_cast<unsigned long long>(cp_cadence_.max_wait_ticks),
        cp_cadence_.max_wait_info, cp_cadence_.max_wait_address,
        cp_cadence_.max_wait_ref, cp_cadence_.max_wait_mask,
        cp_cadence_.max_wait_poll,
        static_cast<unsigned long long>(cp_cadence_.markers),
        static_cast<unsigned long long>(cp_cadence_.marker_tick),
        cp_cadence_.marker_address, cp_cadence_.marker_value,
        submission_valid ? 1u : 0u,
        static_cast<unsigned long long>(submitted),
        static_cast<unsigned long long>(completed));
    for (uint32_t i = 0; i < cp_cadence_.wait_group_count; ++i) {
      const auto& g = cp_cadence_.wait_groups[i];
      TraceEmbeddedCommandProcessor(
          "REX_CP_WAIT_GROUP completed_swap=%llu group=%u info=%08X address=%08X "
          "ref=%08X mask=%08X poll=%08X count=%llu ticks=%llu sleeps=%llu sleep_ticks=%llu",
          static_cast<unsigned long long>(cadence_swap_ordinal), i,
          g.info, g.address, g.ref, g.mask, g.poll,
          static_cast<unsigned long long>(g.count),
          static_cast<unsigned long long>(g.ticks),
          static_cast<unsigned long long>(g.sleeps),
          static_cast<unsigned long long>(g.sleep_ticks));
    }
    for (uint32_t i = 0; i < 3; ++i) {
      const auto& s = cp_cadence_.draw_stages[i];
      TraceEmbeddedCommandProcessor(
          "REX_CP_DRAW_STAGE completed_swap=%llu stage=%u calls=%llu us=%llu max_us=%llu",
          static_cast<unsigned long long>(cadence_swap_ordinal), i,
          static_cast<unsigned long long>(s.calls),
          static_cast<unsigned long long>(s.us),
          static_cast<unsigned long long>(s.max_us));
    }
    TraceEmbeddedCommandProcessor(
        "REX_CP_DRAW_WORK completed_swap=%llu calls=%llu ticks=%llu max_ticks=%llu",
        static_cast<unsigned long long>(cadence_swap_ordinal),
        static_cast<unsigned long long>(cp_cadence_.draw_work.calls),
        static_cast<unsigned long long>(cp_cadence_.draw_work.ticks),
        static_cast<unsigned long long>(cp_cadence_.draw_work.max_ticks));
    for (uint32_t i = 0; i < 4; ++i) {
      const auto& work = cp_cadence_.packet_work[i];
      TraceEmbeddedCommandProcessor(
          "REX_CP_PACKET_WORK completed_swap=%llu category=%u calls=%llu ticks=%llu max_ticks=%llu",
          static_cast<unsigned long long>(cadence_swap_ordinal), i,
          static_cast<unsigned long long>(work.calls),
          static_cast<unsigned long long>(work.ticks),
          static_cast<unsigned long long>(work.max_ticks));
    }
    TraceEmbeddedCommandProcessor(
        "REX_CP_TYPE0_BULK completed_swap=%llu packets=%llu words=%llu",
        static_cast<unsigned long long>(cadence_swap_ordinal),
        static_cast<unsigned long long>(cp_cadence_.type0_bulk_packets),
        static_cast<unsigned long long>(cp_cadence_.type0_bulk_words));
    for (uint32_t opcode = 0; opcode < 128; ++opcode) {
      const auto& work = cp_cadence_.type3_opcode_work[opcode];
      if (!work.calls) continue;
      TraceEmbeddedCommandProcessor(
          "REX_CP_TYPE3_OPCODE_WORK completed_swap=%llu opcode=%02X calls=%llu ticks=%llu max_ticks=%llu",
          static_cast<unsigned long long>(cadence_swap_ordinal), opcode,
          static_cast<unsigned long long>(work.calls),
          static_cast<unsigned long long>(work.ticks),
          static_cast<unsigned long long>(work.max_ticks));
    }
    for (uint32_t stage = 0; stage < 5; ++stage) {
      const auto& work = cp_cadence_.occlusion_work[stage];
      TraceEmbeddedCommandProcessor(
          "REX_CP_ZPD_WORK completed_swap=%llu stage=%u calls=%llu ticks=%llu max_ticks=%llu",
          static_cast<unsigned long long>(cadence_swap_ordinal), stage,
          static_cast<unsigned long long>(work.calls),
          static_cast<unsigned long long>(work.ticks),
          static_cast<unsigned long long>(work.max_ticks));
    }
    TraceEmbeddedCommandProcessor(
        "REX_CP_ZPD_GPU completed_swap=%llu calls=%llu gpu_ticks=%llu max_gpu_ticks=%llu frequency=%llu",
        static_cast<unsigned long long>(cadence_swap_ordinal),
        static_cast<unsigned long long>(cp_cadence_.occlusion_gpu_work.calls),
        static_cast<unsigned long long>(cp_cadence_.occlusion_gpu_work.ticks),
        static_cast<unsigned long long>(cp_cadence_.occlusion_gpu_work.max_ticks),
        static_cast<unsigned long long>(cp_cadence_.occlusion_gpu_frequency));
    TraceEmbeddedCommandProcessor(
        "REX_CP_ZPD_QUEUE completed_swap=%llu calls=%llu submit_to_begin_ms=%.4f max_submit_to_begin_ms=%.4f end_to_retire_ms=%.4f max_end_to_retire_ms=%.4f calibration_ms=%.4f",
        static_cast<unsigned long long>(cadence_swap_ordinal),
        static_cast<unsigned long long>(cp_cadence_.occlusion_gpu_queue_calls),
        cp_cadence_.occlusion_submit_to_begin_ms,
        cp_cadence_.occlusion_submit_to_begin_max_ms,
        cp_cadence_.occlusion_end_to_retire_ms,
        cp_cadence_.occlusion_end_to_retire_max_ms,
        cp_cadence_.occlusion_calibration_ms);
    TraceEmbeddedCommandProcessor(
        "REX_CP_WAIT_OVERFLOW completed_swap=%llu count=%llu ticks=%llu sleeps=%llu sleep_ticks=%llu",
        static_cast<unsigned long long>(cadence_swap_ordinal),
        static_cast<unsigned long long>(cp_cadence_.overflow_waits),
        static_cast<unsigned long long>(cp_cadence_.overflow_ticks),
        static_cast<unsigned long long>(cp_cadence_.overflow_sleeps),
        static_cast<unsigned long long>(cp_cadence_.overflow_sleep_ticks));
    for (uint32_t i = 0; i < interrupt_batch.count; ++i) {
      const auto& r = interrupt_batch.records[i];
      TraceEmbeddedCommandProcessor(
          "REX_CP_INTERRUPT_TIMING completed_swap=%llu kind=%u source=%u address=%08X "
          "before=%08X after=%08X enqueue_tick=%llu dispatch_tick=%llu return_tick=%llu",
          static_cast<unsigned long long>(cadence_swap_ordinal), r.kind, r.source,
          r.address, r.before, r.after, static_cast<unsigned long long>(r.enqueue),
          static_cast<unsigned long long>(r.dispatch), static_cast<unsigned long long>(r.returned));
    }
    TraceEmbeddedCommandProcessor("REX_CP_INTERRUPT_TIMING_END completed_swap=%llu count=%u overflow=%u",
        static_cast<unsigned long long>(cadence_swap_ordinal), interrupt_batch.count, interrupt_batch.overflow);
  }
  const uint64_t next_ordinal = cadence_swap_ordinal;
  const bool next_sample = cadence_enabled &&
      CpCadenceDiagnostic::ShouldSample(next_ordinal);
  if (kGpuDiagnostics) {
    cp_cadence_.Begin(cadence_enabled, next_ordinal, next_sample
        ? rex::chrono::Clock::QueryHostTickCount() : 0);
  }
  if (next_sample) cp_interrupt_timing.Begin(next_ordinal);

  ++counter_;
  return true;
}

bool CommandProcessor::ExecutePacketType3_INDIRECT_BUFFER(memory::RingBuffer* reader,
                                                          uint32_t packet, uint32_t count) {
  // indirect buffer dispatch
  uint32_t list_ptr = CpuToGpu(reader->ReadAndSwap<uint32_t>());
  uint32_t list_length = reader->ReadAndSwap<uint32_t>();
  assert_zero(list_length & ~0xFFFFF);
  list_length &= 0xFFFFF;
  ExecuteIndirectBuffer(GpuToCpu(list_ptr), list_length);
  return true;
}

bool CommandProcessor::ExecutePacketType3_WAIT_REG_MEM(memory::RingBuffer* reader, uint32_t packet,
                                                       uint32_t count) {
  SCOPE_profile_cpu_f("gpu");

  // wait until a register or memory location is a specific value

  uint32_t wait_info = reader->ReadAndSwap<uint32_t>();
  uint32_t poll_reg_addr = reader->ReadAndSwap<uint32_t>();
  uint32_t ref = reader->ReadAndSwap<uint32_t>();
  uint32_t mask = reader->ReadAndSwap<uint32_t>();
  uint32_t wait = reader->ReadAndSwap<uint32_t>();

  bool is_memory = (wait_info & 0x10) != 0;
  const bool interval_wait_active = kGpuDiagnostics && swap_intervals_.active;
  const uint64_t observed_wait_begin =
      ((kGpuDiagnostics && cp_cadence_.active) || interval_wait_active)
      ? rex::chrono::Clock::QueryHostTickCount() : 0;
  static uint64_t wait_sequence = 0;
  const uint64_t sequence = ++wait_sequence;
  const bool trace_wait = kGpuDiagnostics && sequence <= 64;
  if (trace_wait) {
    std::fprintf(stderr,
                 "REX_WAIT_REG_MEM_BEGIN sequence=%" PRIu64
                 " info=0x%08X address=0x%08X ref=0x%08X mask=0x%08X wait=0x%08X "
                 "memory=%u endian=%u\n",
                 sequence, wait_info, poll_reg_addr, ref, mask, wait, is_memory ? 1u : 0u,
                 is_memory ? (poll_reg_addr & 3u) : 0u);
    std::fflush(stderr);
    TraceEmbeddedCommandProcessor(
        "REX_WAIT_REG_MEM_BEGIN sequence=%" PRIu64
        " info=0x%08X address=0x%08X ref=0x%08X mask=0x%08X wait=0x%08X memory=%u "
        "endian=%u",
        sequence, wait_info, poll_reg_addr, ref, mask, wait, is_memory ? 1u : 0u,
        is_memory ? (poll_reg_addr & 3u) : 0u);
  }

  bool matched = false;
  bool first_observation = true;
  uint64_t cadence_sleeps = 0, cadence_sleep_ticks = 0;
  const bool interrupt_wakeup = !kernel_state_ && is_memory &&
      REXCVAR_GET(embedded_interrupt_wait_wakeup);
  do {
    // Register before observing memory: callback completion cannot be lost
    // between a failed comparison and entering the host wait.
    const uint64_t wake_generation = interrupt_wakeup ? cp_interrupt_wakeup.Snapshot() : 0;
    uint32_t value = 0;
    uint32_t raw_value = 0;
    if (is_memory) {
      raw_value =
          *reinterpret_cast<uint32_t*>(memory_->TranslatePhysical(poll_reg_addr & ~uint32_t(0x3)));
      value = raw_value;
      trace_writer_.WriteMemoryRead(CpuToGpu(poll_reg_addr & ~uint32_t(0x3)), sizeof(uint32_t));
      value = xenos::GpuSwap(value, static_cast<xenos::Endian>(poll_reg_addr & 0x3));
    } else {
      value = ReadRegisterValue(poll_reg_addr);
      if (poll_reg_addr == XE_GPU_REG_COHER_STATUS_HOST) {
        MakeCoherent();
        value = ReadRegisterValue(poll_reg_addr);
      }
    }
    switch (wait_info & 0x7) {
      case 0x0:  // Never.
        matched = false;
        break;
      case 0x1:  // Less than reference.
        matched = (value & mask) < ref;
        break;
      case 0x2:  // Less than or equal to reference.
        matched = (value & mask) <= ref;
        break;
      case 0x3:  // Equal to reference.
        matched = (value & mask) == ref;
        break;
      case 0x4:  // Not equal to reference.
        matched = (value & mask) != ref;
        break;
      case 0x5:  // Greater than or equal to reference.
        matched = (value & mask) >= ref;
        break;
      case 0x6:  // Greater than reference.
        matched = (value & mask) > ref;
        break;
      case 0x7:  // Always
        matched = true;
        break;
    }
    if (trace_wait && (first_observation || matched)) {
      std::fprintf(stderr,
                   "REX_WAIT_REG_MEM_OBSERVE sequence=%" PRIu64
                   " raw=0x%08X value=0x%08X matched=%u\n",
                   sequence, raw_value, value, matched ? 1u : 0u);
      std::fflush(stderr);
      TraceEmbeddedCommandProcessor(
          "REX_WAIT_REG_MEM_OBSERVE sequence=%" PRIu64
          " raw=0x%08X value=0x%08X matched=%u",
          sequence, raw_value, value, matched ? 1u : 0u);
    }
    first_observation = false;
    if (!matched) {
      // Wait.
      if (wait >= 0x100) {
        PrepareForWait();
        if (!REXCVAR_GET(vsync)) {
          // User wants it fast and dangerous.
          rex::thread::MaybeYield();
        } else {
          // Nested in WAIT duration: measure only in an already-selected
          // window. Never alter the wait policy or log inside the polling loop.
          const uint64_t sleep_begin = (kGpuDiagnostics && cp_cadence_.active)
              ? rex::chrono::Clock::QueryHostTickCount() : 0;
          if (interrupt_wakeup) {
            cp_interrupt_wakeup.Wait(wake_generation, std::chrono::milliseconds(wait / 0x100));
          } else {
            rex::thread::Sleep(std::chrono::milliseconds(wait / 0x100));
          }
          if (kGpuDiagnostics && cp_cadence_.active) {
            ++cadence_sleeps;
            cadence_sleep_ticks += rex::chrono::Clock::QueryHostTickCount() - sleep_begin;
          }
        }
        rex::thread::SyncMemory();
        ReturnFromWait();

        if (!worker_running_) {
          // Short-circuited exit.
          return false;
        }
      } else {
        rex::thread::MaybeYield();
      }
    }
  } while (!matched);
  if ((kGpuDiagnostics && cp_cadence_.active) || interval_wait_active) {
    const uint64_t observed_tick = rex::chrono::Clock::QueryHostTickCount();
    const uint64_t wait_ticks = observed_tick - observed_wait_begin;
    if (cp_cadence_.active) {
      cp_cadence_.Wait(wait_ticks, wait_info, poll_reg_addr, ref, mask, wait,
                       cadence_sleeps, cadence_sleep_ticks);
      if (is_memory) cp_interrupt_timing.Add(cp_interrupt_timing.Token(),
          {2, wait_info, poll_reg_addr & ~uint32_t(3), ref, mask,
           observed_wait_begin, 0, observed_tick});
    }
    if (interval_wait_active) swap_intervals_.AddWait(wait_ticks);
  }

  return true;
}

bool CommandProcessor::ExecutePacketType3_REG_RMW(memory::RingBuffer* reader, uint32_t packet,
                                                  uint32_t count) {
  // register read/modify/write
  // ? (used during shader upload and edram setup)
  uint32_t rmw_info = reader->ReadAndSwap<uint32_t>();
  uint32_t and_mask = reader->ReadAndSwap<uint32_t>();
  uint32_t or_mask = reader->ReadAndSwap<uint32_t>();
  uint32_t value = register_file_->values[rmw_info & 0x1FFF];
  if ((rmw_info >> 31) & 0x1) {
    // & reg
    value &= register_file_->values[and_mask & 0x1FFF];
  } else {
    // & imm
    value &= and_mask;
  }
  if ((rmw_info >> 30) & 0x1) {
    // | reg
    value |= register_file_->values[or_mask & 0x1FFF];
  } else {
    // | imm
    value |= or_mask;
  }
  WriteRegister(rmw_info & 0x1FFF, value);
  return true;
}

bool CommandProcessor::ExecutePacketType3_REG_TO_MEM(memory::RingBuffer* reader, uint32_t packet,
                                                     uint32_t count) {
  // Copy Register to Memory (?)
  // Count is 2, assuming a Register Addr and a Memory Addr.

  uint32_t reg_addr = reader->ReadAndSwap<uint32_t>();
  uint32_t mem_addr = reader->ReadAndSwap<uint32_t>();

  uint32_t reg_val = ReadRegisterValue(reg_addr);

  auto endianness = static_cast<xenos::Endian>(mem_addr & 0x3);
  mem_addr &= ~0x3;
  reg_val = GpuSwap(reg_val, endianness);
  PrepareForPacketMemoryWrite(mem_addr, 4);
  {
    auto host_write = memory_->GuardPhysicalWrite(mem_addr, 4);
    memory::store(memory_->TranslatePhysical(mem_addr), reg_val);
  }
  trace_writer_.WriteMemoryWrite(CpuToGpu(mem_addr), 4);

  return true;
}

bool CommandProcessor::ExecutePacketType3_MEM_WRITE(memory::RingBuffer* reader, uint32_t packet,
                                                    uint32_t count) {
  uint32_t write_addr = reader->ReadAndSwap<uint32_t>();
  if (count > 1) {
    PrepareForPacketMemoryWrite(write_addr & ~0x3, (count - 1) * sizeof(uint32_t));
  }
  for (uint32_t i = 0; i < count - 1; i++) {
    uint32_t write_data = reader->ReadAndSwap<uint32_t>();

    auto endianness = static_cast<xenos::Endian>(write_addr & 0x3);
    auto addr = write_addr & ~0x3;
    write_data = GpuSwap(write_data, endianness);
    {
      auto host_write = memory_->GuardPhysicalWrite(addr, 4);
      memory::store(memory_->TranslatePhysical(addr), write_data);
    }
    trace_writer_.WriteMemoryWrite(CpuToGpu(addr), 4);
    write_addr += 4;
  }

  return true;
}

bool CommandProcessor::ExecutePacketType3_COND_WRITE(memory::RingBuffer* reader, uint32_t packet,
                                                     uint32_t count) {
  // conditional write to memory or register
  uint32_t wait_info = reader->ReadAndSwap<uint32_t>();
  uint32_t poll_reg_addr = reader->ReadAndSwap<uint32_t>();
  uint32_t ref = reader->ReadAndSwap<uint32_t>();
  uint32_t mask = reader->ReadAndSwap<uint32_t>();
  uint32_t write_reg_addr = reader->ReadAndSwap<uint32_t>();
  uint32_t write_data = reader->ReadAndSwap<uint32_t>();
  uint32_t value;
  if (wait_info & 0x10) {
    // Memory.
    auto endianness = static_cast<xenos::Endian>(poll_reg_addr & 0x3);
    poll_reg_addr &= ~0x3;
    trace_writer_.WriteMemoryRead(CpuToGpu(poll_reg_addr), 4);
    value = memory::load<uint32_t>(memory_->TranslatePhysical(poll_reg_addr));
    value = GpuSwap(value, endianness);
  } else {
    // Register.
    value = ReadRegisterValue(poll_reg_addr);
  }
  bool matched = false;
  switch (wait_info & 0x7) {
    case 0x0:  // Never.
      matched = false;
      break;
    case 0x1:  // Less than reference.
      matched = (value & mask) < ref;
      break;
    case 0x2:  // Less than or equal to reference.
      matched = (value & mask) <= ref;
      break;
    case 0x3:  // Equal to reference.
      matched = (value & mask) == ref;
      break;
    case 0x4:  // Not equal to reference.
      matched = (value & mask) != ref;
      break;
    case 0x5:  // Greater than or equal to reference.
      matched = (value & mask) >= ref;
      break;
    case 0x6:  // Greater than reference.
      matched = (value & mask) > ref;
      break;
    case 0x7:  // Always
      matched = true;
      break;
  }
  if (matched) {
    // Write.
    if (wait_info & 0x100) {
      // Memory.
      auto endianness = static_cast<xenos::Endian>(write_reg_addr & 0x3);
      write_reg_addr &= ~0x3;
      write_data = GpuSwap(write_data, endianness);
      PrepareForPacketMemoryWrite(write_reg_addr, 4);
      {
        auto host_write = memory_->GuardPhysicalWrite(write_reg_addr, 4);
        memory::store(memory_->TranslatePhysical(write_reg_addr), write_data);
      }
      trace_writer_.WriteMemoryWrite(CpuToGpu(write_reg_addr), 4);
    } else {
      // Register.
      WriteRegister(write_reg_addr, write_data);
    }
  }
  return true;
}

bool CommandProcessor::ExecutePacketType3_EVENT_WRITE(memory::RingBuffer* reader, uint32_t packet,
                                                      uint32_t count) {
  // generate an event that creates a write to memory when completed
  uint32_t initiator = reader->ReadAndSwap<uint32_t>();
  // Writeback initiator.
  WriteRegister(XE_GPU_REG_VGT_EVENT_INITIATOR, initiator & 0x3F);
  if (count == 1) {
    // Just an event flag? Where does this write?
  } else {
    // Write to an address.
    assert_always();
    reader->AdvanceRead((count - 1) * sizeof(uint32_t));
  }
  return true;
}

bool CommandProcessor::ExecutePacketType3_EVENT_WRITE_SHD(memory::RingBuffer* reader,
                                                          uint32_t packet, uint32_t count) {
  // generate a VS|PS_done event
  uint32_t initiator = reader->ReadAndSwap<uint32_t>();
  uint32_t address = reader->ReadAndSwap<uint32_t>();
  uint32_t value = reader->ReadAndSwap<uint32_t>();

  // Writeback initiator.
  WriteRegister(XE_GPU_REG_VGT_EVENT_INITIATOR, initiator & 0x3F);
  uint32_t data_value;
  if ((initiator >> 31) & 0x1) {
    // Write counter (GPU vblank counter?).
    data_value = counter_;
  } else {
    // Write value.
    data_value = value;
  }
  auto endianness = static_cast<xenos::Endian>(address & 0x3);
  address &= ~0x3;
  data_value = GpuSwap(data_value, endianness);
  PrepareForPacketMemoryWrite(address, 4);
  {
    auto host_write = memory_->GuardPhysicalWrite(address, 4);
    memory::store(memory_->TranslatePhysical(address), data_value);
  }
  if (kGpuDiagnostics && cp_cadence_.active) {
    ++cp_cadence_.markers;
    cp_cadence_.marker_tick = rex::chrono::Clock::QueryHostTickCount();
    cp_cadence_.marker_address = address;
    cp_cadence_.marker_value = GpuSwap(data_value, endianness);
  }
  trace_writer_.WriteMemoryWrite(CpuToGpu(address), 4);
  return true;
}

bool CommandProcessor::ExecutePacketType3_EVENT_WRITE_EXT(memory::RingBuffer* reader,
                                                          uint32_t packet, uint32_t count) {
  // generate a screen extent event
  uint32_t initiator = reader->ReadAndSwap<uint32_t>();
  uint32_t address = reader->ReadAndSwap<uint32_t>();
  // Writeback initiator.
  WriteRegister(XE_GPU_REG_VGT_EVENT_INITIATOR, initiator & 0x3F);
  auto endianness = static_cast<xenos::Endian>(address & 0x3);
  address &= ~0x3;

  // Let us hope we can fake this.
  // This callback tells the driver the xy coordinates affected by a previous
  // drawcall.
  // https://www.google.com/patents/US20060055701
  uint16_t extents[] = {
      0 >> 3,                                    // min x
      xenos::kTexture2DCubeMaxWidthHeight >> 3,  // max x
      0 >> 3,                                    // min y
      xenos::kTexture2DCubeMaxWidthHeight >> 3,  // max y
      0,                                         // min z
      1,                                         // max z
  };
  assert_true(endianness == xenos::Endian::k8in16);
  PrepareForPacketMemoryWrite(address, sizeof(extents));
  {
    auto host_write = memory_->GuardPhysicalWrite(address, sizeof(extents));
    memory::copy_and_swap_16_unaligned(memory_->TranslatePhysical(address), extents,
        rex::countof(extents));
  }
  trace_writer_.WriteMemoryWrite(CpuToGpu(address), sizeof(extents));
  return true;
}

bool CommandProcessor::ExecutePacketType3_EVENT_WRITE_ZPD(memory::RingBuffer* reader,
                                                          uint32_t packet, uint32_t count) {
  // Set by D3D as BE but struct ABI is LE
  const uint32_t kQueryFinished = rex::byte_swap(0xFFFFFEED);
  assert_true(count == 1);
  uint32_t initiator = reader->ReadAndSwap<uint32_t>();
  // Writeback initiator.
  WriteRegister(XE_GPU_REG_VGT_EVENT_INITIATOR, initiator & 0x3F);

  // Occlusion queries:
  // This command is send on query begin and end.
  // As a workaround report some fixed amount of passed samples.
  auto fake_sample_count = REXCVAR_GET(query_occlusion_fake_sample_count);
  if (fake_sample_count >= 0) {
    auto* pSampleCounts = memory_->TranslatePhysical<xe_gpu_depth_sample_counts*>(
        register_file_->values[XE_GPU_REG_RB_SAMPLE_COUNT_ADDR]);
    if (!pSampleCounts) {
      return true;
    }
    SettleGuestVisibleWork();
    auto host_write = memory_->GuardPhysicalWrite(
        register_file_->values[XE_GPU_REG_RB_SAMPLE_COUNT_ADDR], sizeof(xe_gpu_depth_sample_counts));
    // 0xFFFFFEED is written to this two locations by D3D only on D3DISSUE_END
    // and used to detect a finished query.
    bool is_end_via_z_pass =
        pSampleCounts->ZPass_A == kQueryFinished && pSampleCounts->ZPass_B == kQueryFinished;
    // Older versions of D3D also checks for ZFail (4D5307D5).
    bool is_end_via_z_fail =
        pSampleCounts->ZFail_A == kQueryFinished && pSampleCounts->ZFail_B == kQueryFinished;
    std::memset(pSampleCounts, 0, sizeof(xe_gpu_depth_sample_counts));
    if (is_end_via_z_pass || is_end_via_z_fail) {
      pSampleCounts->ZPass_A = fake_sample_count;
      pSampleCounts->Total_A = fake_sample_count;
    }
  }

  return true;
}

bool CommandProcessor::ExecutePacketType3Draw(memory::RingBuffer* reader, uint32_t packet,
                                              const char* opcode_name, uint32_t viz_query_condition,
                                              uint32_t count_remaining) {
  // if viz_query_condition != 0, this is a conditional draw based on viz query.
  // This ID matches the one issued in PM4_VIZ_QUERY
  // uint32_t viz_id = viz_query_condition & 0x3F;
  // when true, render conditionally based on query result
  // uint32_t viz_use = viz_query_condition & 0x100;

  assert_not_zero(count_remaining);
  if (!count_remaining) {
    REXGPU_ERROR("{}: Packet too small, can't read VGT_DRAW_INITIATOR", opcode_name);
    return false;
  }
  reg::VGT_DRAW_INITIATOR vgt_draw_initiator;
  vgt_draw_initiator.value = reader->ReadAndSwap<uint32_t>();
  --count_remaining;
  WriteRegister(XE_GPU_REG_VGT_DRAW_INITIATOR, vgt_draw_initiator.value);

  bool draw_succeeded = true;
  // TODO(Triang3l): Remove IndexBufferInfo and replace handling of all this
  // with PrimitiveProcessor when the old Vulkan renderer is removed.
  bool is_indexed = false;
  IndexBufferInfo index_buffer_info;
  switch (vgt_draw_initiator.source_select) {
    case xenos::SourceSelect::kDMA: {
      // Indexed draw.
      is_indexed = true;

      // Two separate bounds checks so if there's only one missing register
      // value out of two, one uint32_t will be skipped in the command buffer,
      // not two.
      assert_not_zero(count_remaining);
      if (!count_remaining) {
        REXGPU_ERROR("{}: Packet too small, can't read VGT_DMA_BASE", opcode_name);
        return false;
      }
      uint32_t vgt_dma_base = reader->ReadAndSwap<uint32_t>();
      --count_remaining;
      WriteRegister(XE_GPU_REG_VGT_DMA_BASE, vgt_dma_base);
      reg::VGT_DMA_SIZE vgt_dma_size;
      assert_not_zero(count_remaining);
      if (!count_remaining) {
        REXGPU_ERROR("{}: Packet too small, can't read VGT_DMA_SIZE", opcode_name);
        return false;
      }
      vgt_dma_size.value = reader->ReadAndSwap<uint32_t>();
      --count_remaining;
      WriteRegister(XE_GPU_REG_VGT_DMA_SIZE, vgt_dma_size.value);

      uint32_t index_size_bytes = vgt_draw_initiator.index_size == xenos::IndexFormat::kInt16
                                      ? sizeof(uint16_t)
                                      : sizeof(uint32_t);
      // The base address must already be word-aligned according to the R6xx
      // documentation, but for safety.
      index_buffer_info.guest_base = vgt_dma_base & ~(index_size_bytes - 1);
      index_buffer_info.endianness = vgt_dma_size.swap_mode;
      index_buffer_info.format = vgt_draw_initiator.index_size;
      index_buffer_info.length = vgt_dma_size.num_words * index_size_bytes;
      index_buffer_info.count = vgt_draw_initiator.num_indices;
    } break;
    case xenos::SourceSelect::kImmediate: {
      // TODO(Triang3l): VGT_IMMED_DATA.
      REXGPU_ERROR(
          "{}: Using immediate vertex indices, which are not supported yet. "
          "Report the game to Xenia developers!",
          opcode_name, uint32_t(vgt_draw_initiator.source_select));
      draw_succeeded = false;
      assert_always();
    } break;
    case xenos::SourceSelect::kAutoIndex: {
      // Auto draw.
      index_buffer_info.guest_base = 0;
      index_buffer_info.length = 0;
    } break;
    default: {
      // Invalid source selection.
      draw_succeeded = false;
      assert_unhandled_case(vgt_draw_initiator.source_select);
    } break;
  }

  // Skip to the next command, for example, if there are immediate indexes that
  // we don't support yet.
  reader->AdvanceRead(count_remaining * sizeof(uint32_t));

  if (draw_succeeded) {
    auto viz_query = register_file_->Get<reg::PA_SC_VIZ_QUERY>();
    if (!(viz_query.viz_query_ena && viz_query.kill_pix_post_hi_z)) {
      // TODO(Triang3l): Don't drop the draw call completely if the vertex
      // shader has memexport.
      // TODO(Triang3l || JoelLinn): Handle this properly in the render
      // backends.

      bool major_mode_explicit =
          xenos::IsMajorModeExplicit(vgt_draw_initiator.major_mode, vgt_draw_initiator.prim_type);
      if (kGpuDiagnostics) swap_intervals_.AddDraw();
      draw_succeeded = IssueDraw(vgt_draw_initiator.prim_type, vgt_draw_initiator.num_indices,
                                 is_indexed ? &index_buffer_info : nullptr, major_mode_explicit);
      // Per-draw ordinal log: measurement builds. Player builds still report
      // D3D12 draw failures with their stage (REX_EMBEDDED_DRAW_FAILURE).
      if (kGpuDiagnostics && !kernel_state_) {
        static uint64_t embedded_draw_ordinal = 0;
        static uint64_t embedded_draw_failure_ordinal = 0;
        const uint64_t ordinal = ++embedded_draw_ordinal;
        const uint64_t failure_ordinal =
            draw_succeeded ? 0 : ++embedded_draw_failure_ordinal;
        if (failure_ordinal &&
            (failure_ordinal <= 64 || !(failure_ordinal & 1023))) {
          const auto rb_modecontrol = register_file_->Get<reg::RB_MODECONTROL>();
          std::fprintf(
              stderr,
              "REX_EMBEDDED_DRAW ordinal=%llu result=%u indices=%u primitive=%u source=%u "
              "indexed=%u edram_mode=%u\n",
              static_cast<unsigned long long>(ordinal), draw_succeeded ? 1u : 0u,
              vgt_draw_initiator.num_indices, uint32_t(vgt_draw_initiator.prim_type),
              uint32_t(vgt_draw_initiator.source_select), is_indexed ? 1u : 0u,
              uint32_t(rb_modecontrol.edram_mode));
          std::fflush(stderr);
        }
      }
      if (!draw_succeeded) {
        auto vgt_output_path_cntl = register_file_->Get<reg::VGT_OUTPUT_PATH_CNTL>();
        auto vgt_hos_cntl = register_file_->Get<reg::VGT_HOS_CNTL>();
        auto rb_modecontrol = register_file_->Get<reg::RB_MODECONTROL>();
        REXGPU_ERROR(
            "{}({}, {}, {}): Failed in backend "
            "(major_mode={}, explicit_major={}, path_select={}, tess_mode={}, edram_mode={})",
            opcode_name, static_cast<uint32_t>(vgt_draw_initiator.num_indices),
            uint32_t(vgt_draw_initiator.prim_type), uint32_t(vgt_draw_initiator.source_select),
            uint32_t(vgt_draw_initiator.major_mode), uint32_t(major_mode_explicit),
            uint32_t(vgt_output_path_cntl.path_select), uint32_t(vgt_hos_cntl.tess_mode),
            uint32_t(rb_modecontrol.edram_mode));
      }
    }
  }

  // If read the packed correctly, but merely couldn't execute it (because of,
  // for instance, features not supported by the host), don't terminate command
  // buffer processing as that would leave rendering in a way more inconsistent
  // state than just a single dropped draw command.
  return true;
}

bool CommandProcessor::ExecutePacketType3_DRAW_INDX(memory::RingBuffer* reader, uint32_t packet,
                                                    uint32_t count) {
  // "initiate fetch of index buffer and draw"
  // Generally used by Xbox 360 Direct3D 9 for kDMA and kAutoIndex sources.
  // With a viz query token as the first one.
  uint32_t count_remaining = count;
  assert_not_zero(count_remaining);
  if (!count_remaining) {
    REXGPU_ERROR("PM4_DRAW_INDX: Packet too small, can't read the viz query token");
    return false;
  }
  uint32_t viz_query_condition = reader->ReadAndSwap<uint32_t>();
  --count_remaining;
  return ExecutePacketType3Draw(reader, packet, "PM4_DRAW_INDX", viz_query_condition,
                                count_remaining);
}

bool CommandProcessor::ExecutePacketType3_DRAW_INDX_2(memory::RingBuffer* reader, uint32_t packet,
                                                      uint32_t count) {
  // "draw using supplied indices in packet"
  // Generally used by Xbox 360 Direct3D 9 for kAutoIndex source.
  // No viz query token.
  return ExecutePacketType3Draw(reader, packet, "PM4_DRAW_INDX_2", 0, count);
}

bool CommandProcessor::ExecutePacketType3_SET_CONSTANT(memory::RingBuffer* reader, uint32_t packet,
                                                       uint32_t count) {
  // load constant into chip and to memory
  // PM4_REG(reg) ((0x4 << 16) | (GSL_HAL_SUBBLOCK_OFFSET(reg)))
  //                                     reg - 0x2000
  uint32_t offset_type = reader->ReadAndSwap<uint32_t>();
  uint32_t index = offset_type & 0x7FF;
  uint32_t type = (offset_type >> 16) & 0xFF;
  uint32_t count_registers = count - 1;
  switch (type) {
    case 0:  // ALU
      WriteALURangeFromRing(reader, index, count_registers);
      break;
    case 1:  // FETCH
      WriteFetchRangeFromRing(reader, index, count_registers);
      break;
    case 2:  // BOOL
      WriteBoolRangeFromRing(reader, index, count_registers);
      break;
    case 3:  // LOOP
      WriteLoopRangeFromRing(reader, index, count_registers);
      break;
    case 4:  // REGISTERS
      WriteREGISTERSRangeFromRing(reader, index, count_registers);
      break;
    default:
      assert_always();
      reader->AdvanceRead((count - 1) * sizeof(uint32_t));
      return true;
  }
  return true;
}

bool CommandProcessor::ExecutePacketType3_SET_CONSTANT2(memory::RingBuffer* reader, uint32_t packet,
                                                        uint32_t count) {
  uint32_t offset_type = reader->ReadAndSwap<uint32_t>();
  uint32_t index = offset_type & 0xFFFF;
  WriteRegisterRangeFromRing(reader, index, count - 1);
  return true;
}

bool CommandProcessor::ExecutePacketType3_LOAD_ALU_CONSTANT(memory::RingBuffer* reader,
                                                            uint32_t packet, uint32_t count) {
  // load constants from memory
  uint32_t address = reader->ReadAndSwap<uint32_t>();
  address &= 0x3FFFFFFF;
  uint32_t offset_type = reader->ReadAndSwap<uint32_t>();
  uint32_t index = offset_type & 0x7FF;
  uint32_t size_dwords = reader->ReadAndSwap<uint32_t>();
  size_dwords &= 0xFFF;
  uint32_t type = (offset_type >> 16) & 0xFF;
  uint32_t* xlat_address = memory_->TranslatePhysical<uint32_t*>(address);
  switch (type) {
    case 0:  // ALU
      trace_writer_.WriteMemoryRead(CpuToGpu(address), size_dwords * 4);
      WriteALURangeFromMem(index, xlat_address, size_dwords);
      break;
    case 1:  // FETCH
      trace_writer_.WriteMemoryRead(CpuToGpu(address), size_dwords * 4);
      WriteFetchRangeFromMem(index, xlat_address, size_dwords);
      break;
    case 2:  // BOOL
      trace_writer_.WriteMemoryRead(CpuToGpu(address), size_dwords * 4);
      WriteBoolRangeFromMem(index, xlat_address, size_dwords);
      break;
    case 3:  // LOOP
      trace_writer_.WriteMemoryRead(CpuToGpu(address), size_dwords * 4);
      WriteLoopRangeFromMem(index, xlat_address, size_dwords);
      break;
    case 4:  // REGISTERS
      trace_writer_.WriteMemoryRead(CpuToGpu(address), size_dwords * 4);
      WriteREGISTERSRangeFromMem(index, xlat_address, size_dwords);
      break;
    default:
      assert_always();
      return true;
  }
  return true;
}

bool CommandProcessor::ExecutePacketType3_SET_SHADER_CONSTANTS(memory::RingBuffer* reader,
                                                               uint32_t packet, uint32_t count) {
  uint32_t offset_type = reader->ReadAndSwap<uint32_t>();
  uint32_t index = offset_type & 0xFFFF;
  WriteRegisterRangeFromRing(reader, index, count - 1);
  return true;
}

bool CommandProcessor::ExecutePacketType3_IM_LOAD(memory::RingBuffer* reader, uint32_t packet,
                                                  uint32_t count) {
  SCOPE_profile_cpu_f("gpu");

  // load sequencer instruction memory (pointer-based)
  uint32_t addr_type = reader->ReadAndSwap<uint32_t>();
  auto shader_type = static_cast<xenos::ShaderType>(addr_type & 0x3);
  uint32_t addr = addr_type & ~0x3;
  uint32_t start_size = reader->ReadAndSwap<uint32_t>();
  uint32_t start = start_size >> 16;
  uint32_t size_dwords = start_size & 0xFFFF;  // dwords
  assert_true(start == 0);

  trace_writer_.WriteMemoryRead(CpuToGpu(addr), size_dwords * 4);
  auto shader =
      LoadShader(shader_type, addr, memory_->TranslatePhysical<uint32_t*>(addr), size_dwords);
  switch (shader_type) {
    case xenos::ShaderType::kVertex:
      active_vertex_shader_ = shader;
      break;
    case xenos::ShaderType::kPixel:
      active_pixel_shader_ = shader;
      break;
    default:
      assert_unhandled_case(shader_type);
      return false;
  }
  return true;
}

bool CommandProcessor::ExecutePacketType3_IM_LOAD_IMMEDIATE(memory::RingBuffer* reader,
                                                            uint32_t packet, uint32_t count) {
  SCOPE_profile_cpu_f("gpu");

  // load sequencer instruction memory (code embedded in packet)
  uint32_t dword0 = reader->ReadAndSwap<uint32_t>();
  uint32_t dword1 = reader->ReadAndSwap<uint32_t>();
  auto shader_type = static_cast<xenos::ShaderType>(dword0);
  uint32_t start_size = dword1;
  uint32_t start = start_size >> 16;
  uint32_t size_dwords = start_size & 0xFFFF;  // dwords
  assert_true(start == 0);
  assert_true(reader->read_count() >= size_dwords * 4);
  assert_true(count - 2 >= size_dwords);
  auto shader = LoadShader(shader_type, uint32_t(reader->read_ptr()),
                           reinterpret_cast<uint32_t*>(reader->read_ptr()), size_dwords);
  switch (shader_type) {
    case xenos::ShaderType::kVertex:
      active_vertex_shader_ = shader;
      break;
    case xenos::ShaderType::kPixel:
      active_pixel_shader_ = shader;
      break;
    default:
      assert_unhandled_case(shader_type);
      return false;
  }
  reader->AdvanceRead(size_dwords * sizeof(uint32_t));
  return true;
}

bool CommandProcessor::ExecutePacketType3_INVALIDATE_STATE(memory::RingBuffer* reader,
                                                           uint32_t packet, uint32_t count) {
  // selective invalidation of state pointers
  /*uint32_t mask =*/reader->ReadAndSwap<uint32_t>();
  // driver_->InvalidateState(mask);
  return true;
}

bool CommandProcessor::ExecutePacketType3_VIZ_QUERY(memory::RingBuffer* reader, uint32_t packet,
                                                    uint32_t count) {
  // begin/end initiator for viz query extent processing
  // https://www.google.com/patents/US20050195186
  assert_true(count == 1);

  uint32_t dword0 = reader->ReadAndSwap<uint32_t>();

  uint32_t id = dword0 & 0x3F;
  uint32_t end = dword0 & 0x100;
  if (!end) {
    // begin a new viz query @ id
    // On hardware this clears the internal state of the scan converter (which
    // is different to the register)
    WriteRegister(XE_GPU_REG_VGT_EVENT_INITIATOR, VIZQUERY_START);
    REXGPU_INFO("Begin viz query ID {:02X}", id);
  } else {
    // end the viz query
    WriteRegister(XE_GPU_REG_VGT_EVENT_INITIATOR, VIZQUERY_END);
    REXGPU_INFO("End viz query ID {:02X}", id);
    // The scan converter writes the internal result back to the register here.
    // We just fake it and say it was visible in case it is read back.
    if (id < 32) {
      register_file_->values[XE_GPU_REG_PA_SC_VIZ_QUERY_STATUS_0] |= uint32_t(1) << id;
    } else {
      register_file_->values[XE_GPU_REG_PA_SC_VIZ_QUERY_STATUS_1] |= uint32_t(1) << (id - 32);
    }
  }

  return true;
}

void CommandProcessor::InitializeTrace() {
  // Write the initial register values, to be loaded directly into the
  // RegisterFile since all registers, including those that may have side
  // effects on setting, will be saved.
  trace_writer_.WriteRegisters(0, register_file_->values, RegisterFile::kRegisterCount, false);

  trace_writer_.WriteGammaRamp(gamma_ramp_256_entry_table(), gamma_ramp_pwl_rgb(),
                               gamma_ramp_rw_component_);
}

}  // namespace rex::graphics
