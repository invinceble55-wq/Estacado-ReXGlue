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
#include <unordered_map>
#include <unordered_set>
#include <mutex>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <rex/assert.h>
#include <rex/chrono/clock.h>
#include <rex/cvar.h>
#include <rex/dbg.h>
#include <rex/perf/counter.h>
#include <rex/graphics/d3d12/command_processor.h>
#include <rex/graphics/embedded_diagnostics.h>
#include <rex/graphics/embedded_manual_capture_policy.h>
#include <rex/graphics/embedded_camera_draw_capture_policy.h>
#include <rex/graphics/embedded_camera_history_capture_policy.h>
#include <rex/graphics/embedded_camera_geometry_capture_policy.h>
#include <rex/graphics/embedded_scene_resolve_capture_policy.h>
#include <rex/graphics/embedded_scene_transfer_capture_policy.h>
#include <rex/graphics/embedded_resolve_boundary_capture_policy.h>
#include <rex/graphics/d3d12/graphics_system.h>
#include <rex/graphics/d3d12/shader.h>
#include <rex/graphics/flags.h>
#include <rex/graphics/pipeline/shader/interpreter.h>
#include <rex/graphics/registers.h>
#include <rex/graphics/util/draw.h>
#include <rex/graphics/xenos.h>
#include <rex/graphics/xenos_zpd_report.h>
#include <rex/kernel/xboxkrnl/video.h>
#include <rex/logging.h>
#include <rex/memory/utils.h>
#include <rex/ui/d3d12/d3d12_presenter.h>
#include <rex/ui/d3d12/d3d12_util.h>

REXCVAR_DEFINE_BOOL(d3d12_bindless, true, "GPU/D3D12", "Use bindless resources where available")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_STRING(
    draw_resolution_scale_native_grid_rules, "", "GPU/D3D12",
    "Native-grid pass annotations: VS:PS:fetch:width:height:format[:filter], semicolon-separated")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_BOOL(d3d12_readback_memexport, false, "GPU/D3D12",
                    "Read data written by memory export in shaders on the CPU")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(d3d12_readback_resolve, false, "GPU/D3D12",
                    "Read render-to-texture results on the CPU")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DECLARE(bool, native_resolve_region_tracking);

REXCVAR_DEFINE_BOOL(d3d12_submit_on_primary_buffer_end, true, "GPU/D3D12",
                    "Submit command list when PM4 primary buffer ends")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_UINT32(d3d12_submit_after_draws, 0, "GPU/D3D12",
                      "Experimental: submit completed non-query draw batches at this draw limit (0 disables, maximum 4096)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(d3d12_async_submission, true, "GPU/D3D12",
                    "Replay and execute closed submissions on a worker thread")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(d3d12_deferred_presenter_completion, true, "GPU/D3D12",
                    "With asynchronous submission, run the presenter's post-refresh steps "
                    "(fence signal, mailbox publication, immediate paint) on the submission "
                    "worker after the frame's command lists instead of draining the worker at "
                    "every swap (read at every swap)");
REXCVAR_DEFINE_UINT32(d3d12_async_submit_after_draws, 512, "GPU/D3D12",
                      "With asynchronous submission, submit completed non-query draw batches at "
                      "this draw limit when d3d12_submit_after_draws is 0 (0 disables, maximum 4096)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_UINT32(d3d12_zpd_submit_after_draws, 0, "GPU/D3D12",
                      "Experimental: submit an active native-scale ZPD query segment after this many draws (0 disables)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_UINT32(d3d12_zpd_max_publish_lag_frames, 2, "GPU/D3D12",
                      "Guest frames an occlusion report may stay unpublished after its END before "
                      "the command processor waits for its host submission at a swap (0: publish "
                      "only when the host GPU happens to be done)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(embedded_zpd_gpu_timing, false, "GPU/Diagnostics",
                   "Sample native D3D12 timestamp duration of ZPD query segments in sparse CP intervals")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(d3d12_gpu_timing, false, "GPU/Diagnostics",
                   "Diagnostic: GPU timestamps where the category of recorded work changes (draws, "
                   "render-target transfers, resolves, texture loads, uploads, barriers, swap); "
                   "per-category GPU time logged every half second as REX_GPU_TIMING")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_UINT32(d3d12_gpu_timing_transfer_log_interval, 0, "GPU/Diagnostics",
                      "With d3d12_gpu_timing: log every render-target transfer of one frame in "
                      "this many (REX_GPU_TRANSFER, 0 disables)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(d3d12_gpu_timing_passes, false, "GPU/Diagnostics",
                    "With d3d12_gpu_timing: attribute draw time to render-target sets and log "
                    "the costliest every half second (REX_GPU_PASS)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(d3d12_gpu_frame_meter, true, "GPU",
                    "GPU busy time of each frame (two timestamps per submission) for automatic "
                    "settings")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

// Bounded embedded-title diagnostics. A zero start ordinal keeps this entirely
// disabled. The runtime validation launcher opts into a handful of late frames
// so intro/menu execution is not burdened by per-draw logging.
REXCVAR_DEFINE_UINT32(embedded_gameplay_capture_start_swap, 0, "GPU/Diagnostics",
                      "First embedded swap ordinal whose draw state is captured")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_UINT32(embedded_gameplay_capture_interval_swaps, 600,
                      "GPU/Diagnostics",
                      "Swap interval between bounded embedded gameplay captures")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_UINT32(embedded_gameplay_capture_count, 6, "GPU/Diagnostics",
                      "Maximum number of bounded embedded gameplay captures")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(
    embedded_manual_gameplay_capture, false, "GPU/Diagnostics",
    "Capture one complete GPU frame after a physical F12 screenshot request")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(
    embedded_camera_draw_capture, false, "GPU/Diagnostics",
    "Capture actual bound vertex constants and viewport registers for one selected frame")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(
    embedded_camera_geometry_capture, false, "GPU/Diagnostics",
    "Capture complete bounded GPU geometry for selected packed-depth and clear draws")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(
    embedded_camera_depth_clear_capture, false, "GPU/Diagnostics",
    "Capture two selected clear/alias depth sample pairs without splitting submission")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(
    embedded_scaled_texture_readback_capture, false, "GPU/Diagnostics",
    "Capture bounded unique scaled shader resources in a selected diagnostic frame")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_UINT32(
    embedded_resolve_source_capture_min_draw_packets, 500, "GPU/Diagnostics",
    "Minimum draw packet count for the one-shot pre-EDRAM resolve source capture")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_UINT32(
    embedded_resolve_source_capture_min_pixel_draws, 250, "GPU/Diagnostics",
    "Minimum pixel draw count for the one-shot pre-EDRAM resolve source capture")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_UINT32(
    embedded_resolve_source_capture_min_prior_resolves, 8, "GPU/Diagnostics",
    "Minimum prior resolve count for the one-shot pre-EDRAM resolve source capture")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_UINT32(
    embedded_resolve_source_capture_destination, 0, "GPU/Diagnostics",
    "Optional exact guest resolve destination for one-shot pre-EDRAM source capture")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(
    embedded_resolve_source_capture_scene_color, false, "GPU/Diagnostics",
    "Capture the first embedded title scene-color resolve source by register signature")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(
    embedded_resolve_boundary_capture_first_scene_feedback, false,
    "GPU/Diagnostics",
    "Capture every boundary of the first embedded title scene-feedback resolve")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(
    embedded_resolve_source_capture_first_full_frame, false,
    "GPU/Diagnostics",
    "Capture the first embedded title full-frame resolve source by register signature")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(
    embedded_scene_seed_draw_capture, false, "GPU/Diagnostics",
    "Capture the embedded title scene-seed input and its first two host draw outputs")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(
    embedded_mixed_scale_transition_capture, false, "GPU/Diagnostics",
    "Capture the embedded title native-4x to scaled-2x EDRAM ownership transition")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(
    embedded_scene_chain_capture, false, "GPU/Diagnostics",
    "Capture the first six embedded scaled HDR scene-target draw outputs")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(
    embedded_scene_chain_late_summary, false, "GPU/Diagnostics",
    "Summarize embedded HDR scene-target draw outputs 7 through 32 without dumping them")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(
    embedded_scene_chain_checkpoint_capture, false, "GPU/Diagnostics",
    "Capture embedded HDR scene-target outputs after draws 8, 16, 24 and 32")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_UINT32(embedded_target_writer_capture_count, 0, "GPU/Diagnostics",
                      "Maximum sparse target-writer after-images in one selected frame (0 disables, max 8)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(embedded_target_writer_texture_source, false, "GPU/Diagnostics",
                   "Capture exact resident unscaled texture bytes with selected writer pairs (8 copies, 8MiB each)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(embedded_temporal_depth_resolve_capture, false, "GPU/Diagnostics",
                   "Copy up to four native depth-resolve ranges in one selected frame; not a complete temporal surface")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(embedded_camera_scene_capture, false, "GPU/Diagnostics",
                   "Copy two native color/motion epochs each with resolve chronology and bound PS constants in one selected camera frame")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(embedded_camera_history_capture, false, "GPU/Diagnostics",
                   "Bounded motion-pass constants and geometry bindings for two adjacent manually selected frames")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_UINT32(embedded_camera_history_capture_frames, 2, "GPU/Diagnostics",
                     "Manually selected history metadata: 2 frames, or opt-in 6 for producer/consumer correspondence")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_UINT32(embedded_target_writer_surface, 0, "GPU/Diagnostics",
                      "Exact RB_SURFACE_INFO for target-writer diagnostics")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_UINT32(embedded_target_writer_color, 0, "GPU/Diagnostics",
                      "Exact RB_COLOR_INFO for target-writer diagnostics")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_UINT32(embedded_target_writer_scissor_br, 0, "GPU/Diagnostics",
                      "Exact first-tile window scissor BR; zero TL and window offset required")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_UINT32(embedded_target_writer_min_draw, 0, "GPU/Diagnostics",
                      "Earliest submitted draw eligible for target-writer diagnostics")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_UINT32(embedded_target_writer_first, 1, "GPU/Diagnostics",
                      "First matching writer ordinal to capture after drawing")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_UINT32(embedded_target_writer_stride, 256, "GPU/Diagnostics",
                      "Matching-writer interval between sparse after-images")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_UINT64(embedded_target_writer_pixel_shader, 0, "GPU/Diagnostics",
                     "Optional exact pixel-shader hash for bounded target checkpoints")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_UINT32(embedded_target_writer_auto_start_swap, 0, "GPU/Diagnostics",
                     "Arm one partial diagnostic frame on the first exact writer match on/after this swap (0 disables)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(embedded_target_writer_capture_pairs, false, "GPU/Diagnostics",
                   "Capture before/after pairs plus shader inputs; exact PS required, max four pairs")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(
    embedded_simple_title_composition_capture, false, "GPU/Diagnostics",
    "Capture the two inputs and output of the simple title composition draw")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(
    embedded_color_map_chain_capture, false, "GPU/Diagnostics",
    "Capture three color-map producers and the final HDR compositor once")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(
    embedded_scene_depth_snapshot_capture, false, "GPU/Diagnostics",
    "Capture packed depth/stencil immediately before the first corrupting scaled scene draw")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(
    embedded_scene_depth_pair_trace, false, "GPU/Diagnostics",
    "Trace the matching depth-prepass and shaded-draw position inputs for the embedded scene")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(
    embedded_scene_depth_phase_trace, false, "GPU/Diagnostics",
    "Summarize depth/stencil around the exact embedded-scene mask-producing draw phases")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(
    embedded_scene_vertex_output_trace, false, "GPU/Diagnostics",
    "Interpret a bounded set of matching embedded-scene vertices and trace their guest raster inputs")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(
    d3d12_embedded_scene_host_vertex_output_diagnostic, false,
    "GPU/D3D12",
    "Capture actual host SV_Position output for the matching scaled embedded-scene draws")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(
    embedded_scene_shaded_texture_capture, false, "GPU/Diagnostics",
    "Capture the four texture resources feeding the first scaled embedded scene shaded draw")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_UINT32(embedded_hitch_trace_threshold_ms, 75,
                       "GPU/Diagnostics",
                       "Minimum embedded swap interval logged for hitch attribution")
    .range(34, 1000);
REXCVAR_DEFINE_BOOL(embedded_hitch_diagnostics, false, "GPU/Diagnostics",
                    "Collect embedded per-draw hitch attribution timing")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace rex::graphics::d3d12 {

constexpr bool kEmbeddedGraphicsDiagnosticsEnabled = false;

struct SparseZpdWorkScope {
  CpCadenceDiagnostic* diagnostic;
  uint32_t stage;
  uint64_t begin_tick;
  SparseZpdWorkScope(CpCadenceDiagnostic& cadence, uint32_t stage)
      : diagnostic((kGpuDiagnostics && cadence.active) ? &cadence : nullptr),
        stage(stage),
        begin_tick(diagnostic ? rex::chrono::Clock::QueryHostTickCount() : 0) {}
  ~SparseZpdWorkScope() {
    if (diagnostic) {
      diagnostic->OcclusionWorkTime(
          stage, rex::chrono::Clock::QueryHostTickCount() - begin_tick);
    }
  }
};

// Captures the guest vertex-shader position export without altering the draw.
// This is used only by the opt-in internal-scale diagnostic below. It retains
// no guest data after the draw and performs no host GPU work.
class EmbeddedPositionExportSink final
    : public ShaderInterpreter::ExportSink {
 public:
  void Export(ucode::ExportRegister export_register, const float* value,
              uint32_t value_mask) override {
    if (export_register == ucode::ExportRegister::kVSPosition) {
      for (uint32_t component = 0; component < 4; ++component) {
        if (value_mask & (UINT32_C(1) << component)) {
          position_[component] = value[component];
          position_mask_ |= UINT32_C(1) << component;
        }
      }
    } else if (export_register ==
                   ucode::ExportRegister::kVSPointSizeEdgeFlagKillVertex &&
               (value_mask & 0b0100)) {
      vertex_kill_ = rex::memory::Reinterpret<uint32_t>(value[2]);
      vertex_kill_valid_ = true;
    }
  }

  void Reset() {
    position_ = {};
    position_mask_ = 0;
    vertex_kill_ = 0;
    vertex_kill_valid_ = false;
  }

  const std::array<float, 4>& position() const { return position_; }
  uint32_t position_mask() const { return position_mask_; }
  bool killed() const {
    return vertex_kill_valid_ &&
           (vertex_kill_ & ~(UINT32_C(1) << 31)) != 0;
  }

 private:
  std::array<float, 4> position_{};
  uint32_t position_mask_ = 0;
  uint32_t vertex_kill_ = 0;
  bool vertex_kill_valid_ = false;
};

static void TraceEmbeddedSceneVertexOutputs(
    const RegisterFile& regs, const memory::Memory& memory,
    const Shader& vertex_shader, const char* pair_kind,
    uint32_t draw_scale_x, uint32_t draw_scale_y) {
  constexpr uint32_t kMaximumIndexOrdinals = 128;
  constexpr uint32_t kMaximumUniqueVertices = 24;
  constexpr uint32_t kMaximumFullHashIndexOrdinals = 16384;
  constexpr uint32_t kMaximumFullHashUniqueVertices = 8192;

  if (!ShaderInterpreter::CanInterpretShader(vertex_shader)) {
    std::fprintf(stderr,
                 "REX_EMBEDDED_SCENE_VERTEX_OUTPUT kind=%s result=unsupported_shader\n",
                 pair_kind);
    std::fflush(stderr);
    return;
  }

  const auto draw_initiator = regs.Get<reg::VGT_DRAW_INITIATOR>();
  if (draw_initiator.source_select != xenos::SourceSelect::kDMA &&
      draw_initiator.source_select != xenos::SourceSelect::kAutoIndex) {
    std::fprintf(stderr,
                 "REX_EMBEDDED_SCENE_VERTEX_OUTPUT kind=%s result=unsupported_index_source source=%u\n",
                 pair_kind, uint32_t(draw_initiator.source_select));
    std::fflush(stderr);
    return;
  }

  const auto dma_size = regs.Get<reg::VGT_DMA_SIZE>();
  const auto sc_mode = regs.Get<reg::PA_SU_SC_MODE_CNTL>();
  const auto clip_control = regs.Get<reg::PA_CL_CLIP_CNTL>();
  const auto vte = regs.Get<reg::PA_CL_VTE_CNTL>();
  const auto vertex_control = regs.Get<reg::PA_SU_VTX_CNTL>();
  const auto window_offset = regs.Get<reg::PA_SC_WINDOW_OFFSET>();
  const uint32_t reset_index =
      regs.Get<reg::VGT_MULTI_PRIM_IB_RESET_INDX>().reset_indx;
  const uint32_t index_offset = regs.Get<reg::VGT_INDX_OFFSET>().indx_offset;
  const uint32_t min_index = regs.Get<reg::VGT_MIN_VTX_INDX>().min_indx;
  const uint32_t max_index = regs.Get<reg::VGT_MAX_VTX_INDX>().max_indx;

  xenos::Endian index_endian = dma_size.swap_mode;
  const uint8_t* index_data = nullptr;
  uint32_t index_element_size = 0;
  uint32_t index_buffer_base = 0;
  uint32_t available_index_count = uint32_t(draw_initiator.num_indices);
  if (draw_initiator.source_select == xenos::SourceSelect::kDMA) {
    index_element_size =
        draw_initiator.index_size == xenos::IndexFormat::kInt16 ? 2u : 4u;
    if (index_element_size == 2) {
      if (index_endian == xenos::Endian::k8in32) {
        index_endian = xenos::Endian::k8in16;
      } else if (index_endian == xenos::Endian::k16in32) {
        index_endian = xenos::Endian::kNone;
      }
    }
    index_buffer_base =
        regs[XE_GPU_REG_VGT_DMA_BASE] & ~(index_element_size - 1);
    const uint32_t readable_indices =
        std::min(uint32_t(draw_initiator.num_indices),
                 uint32_t(dma_size.num_words));
    available_index_count = readable_indices;
    const uint64_t readable_bytes =
        uint64_t(readable_indices) * index_element_size;
    if (index_buffer_base >= SharedMemory::kBufferSize ||
        readable_bytes > SharedMemory::kBufferSize - index_buffer_base) {
      std::fprintf(stderr,
                   "REX_EMBEDDED_SCENE_VERTEX_OUTPUT kind=%s result=index_range_invalid base=0x%08X bytes=%llu\n",
                   pair_kind, index_buffer_base,
                   static_cast<unsigned long long>(readable_bytes));
      std::fflush(stderr);
      return;
    }
    index_data = memory.TranslatePhysical<const uint8_t*>(index_buffer_base);
    if (!index_data) {
      std::fprintf(stderr,
                   "REX_EMBEDDED_SCENE_VERTEX_OUTPUT kind=%s result=index_unmapped base=0x%08X\n",
                   pair_kind, index_buffer_base);
      std::fflush(stderr);
      return;
    }
  }

  const float viewport_scale[3] = {
      vte.vport_x_scale_ena
          ? regs.Get<float>(XE_GPU_REG_PA_CL_VPORT_XSCALE)
          : 1.0f,
      vte.vport_y_scale_ena
          ? regs.Get<float>(XE_GPU_REG_PA_CL_VPORT_YSCALE)
          : 1.0f,
      vte.vport_z_scale_ena
          ? regs.Get<float>(XE_GPU_REG_PA_CL_VPORT_ZSCALE)
          : 1.0f,
  };
  float viewport_offset[3] = {
      vte.vport_x_offset_ena
          ? regs.Get<float>(XE_GPU_REG_PA_CL_VPORT_XOFFSET)
          : 0.0f,
      vte.vport_y_offset_ena
          ? regs.Get<float>(XE_GPU_REG_PA_CL_VPORT_YOFFSET)
          : 0.0f,
      vte.vport_z_offset_ena
          ? regs.Get<float>(XE_GPU_REG_PA_CL_VPORT_ZOFFSET)
          : 0.0f,
  };
  if (sc_mode.vtx_window_offset_enable) {
    viewport_offset[0] += float(window_offset.window_x_offset);
    viewport_offset[1] += float(window_offset.window_y_offset);
  }
  if (REXCVAR_GET(half_pixel_offset) &&
      vertex_control.pix_center == xenos::PixelCenter::kD3DZero) {
    viewport_offset[0] += 0.5f;
    viewport_offset[1] += 0.5f;
  }

  ShaderInterpreter interpreter(regs, memory);
  interpreter.SetShader(vertex_shader);
  EmbeddedPositionExportSink sink;
  interpreter.SetExportSink(&sink);
  std::array<uint32_t, kMaximumUniqueVertices> captured_indices{};
  uint32_t captured_count = 0;
  const uint32_t ordinal_count =
      std::min(available_index_count, kMaximumIndexOrdinals);

  const auto decode_vertex_index = [&](uint32_t ordinal,
                                       uint32_t& vertex_index_out) {
    uint32_t vertex_index;
    if (draw_initiator.source_select == xenos::SourceSelect::kDMA) {
      if (ordinal >= available_index_count) {
        return false;
      }
      if (index_element_size == 2) {
        vertex_index = xenos::GpuSwap(
            rex::memory::load<uint16_t>(index_data + ordinal * 2),
            index_endian);
      } else {
        vertex_index =
            xenos::GpuSwap(
                rex::memory::load<uint32_t>(index_data + ordinal * 4),
                index_endian) &
            xenos::kVertexIndexMask;
      }
      if (sc_mode.multi_prim_ib_ena && vertex_index == reset_index) {
        return false;
      }
    } else {
      vertex_index = ordinal;
    }
    vertex_index_out = std::min(
        max_index,
        std::max(min_index, (vertex_index + index_offset) &
                                xenos::kVertexIndexMask));
    return true;
  };

  std::fprintf(
      stderr,
      "REX_EMBEDDED_SCENE_VERTEX_OUTPUT_BEGIN kind=%s indices=%u scan_limit=%u capture_limit=%u index_base=0x%08X index_size=%u index_endian=%u index_offset=%u min=%u max=%u vte=0x%08X vertex_control=0x%08X window=0x%08X viewport=%g,%g,%g,%g,%g,%g scale=%ux%u\n",
      pair_kind, uint32_t(draw_initiator.num_indices), ordinal_count,
      kMaximumUniqueVertices, index_buffer_base, index_element_size,
      uint32_t(index_endian), index_offset, min_index, max_index, vte.value,
      vertex_control.value, window_offset.value, viewport_scale[0],
      viewport_offset[0], viewport_scale[1], viewport_offset[1],
      viewport_scale[2], viewport_offset[2], draw_scale_x, draw_scale_y);

  for (uint32_t ordinal = 0;
       ordinal < ordinal_count && captured_count < kMaximumUniqueVertices;
       ++ordinal) {
    uint32_t vertex_index;
    if (!decode_vertex_index(ordinal, vertex_index)) {
      continue;
    }

    bool duplicate = false;
    for (uint32_t i = 0; i < captured_count; ++i) {
      if (captured_indices[i] == vertex_index) {
        duplicate = true;
        break;
      }
    }
    if (duplicate) {
      continue;
    }
    captured_indices[captured_count++] = vertex_index;

    sink.Reset();
    std::fill(interpreter.temp_registers(),
              interpreter.temp_registers() +
                  xenos::kMaxShaderTempRegisters * 4,
              0.0f);
    interpreter.temp_registers()[0] = float(vertex_index);
    interpreter.Execute();

    const auto& position = sink.position();
    float ndc[3] = {position[0], position[1], position[2]};
    const bool have_w = (sink.position_mask() & 0b1000) != 0;
    if (!vte.vtx_xy_fmt && have_w) {
      ndc[0] /= position[3];
      ndc[1] /= position[3];
    }
    if (!vte.vtx_z_fmt && have_w) {
      ndc[2] /= position[3];
    }
    const float guest_screen[3] = {
        ndc[0] * viewport_scale[0] + viewport_offset[0],
        ndc[1] * viewport_scale[1] + viewport_offset[1],
        ndc[2] * viewport_scale[2] + viewport_offset[2],
    };
    const float subpixel_x = guest_screen[0] * 16.0f;
    const float subpixel_y = guest_screen[1] * 16.0f;
    const bool finite_subpixel =
        std::isfinite(subpixel_x) && std::isfinite(subpixel_y);
    const int32_t floor_subpixel_x =
        finite_subpixel ? int32_t(std::floor(subpixel_x)) : 0;
    const int32_t floor_subpixel_y =
        finite_subpixel ? int32_t(std::floor(subpixel_y)) : 0;
    const int32_t round_subpixel_x =
        finite_subpixel ? int32_t(std::round(subpixel_x)) : 0;
    const int32_t round_subpixel_y =
        finite_subpixel ? int32_t(std::round(subpixel_y)) : 0;
    std::fprintf(
        stderr,
        "REX_EMBEDDED_SCENE_VERTEX_OUTPUT kind=%s ordinal=%u vertex=%u mask=0x%X killed=%u clip=% .9g,% .9g,% .9g,% .9g ndc=% .9g,% .9g,% .9g guest_screen=% .9g,% .9g,% .9g subpixel16=% .9g,% .9g finite=%u floor16=%d,%d round16=%d,%d scaled_screen=% .9g,% .9g\n",
        pair_kind, ordinal, vertex_index, sink.position_mask(),
        sink.killed() ? 1u : 0u, position[0], position[1], position[2],
        position[3], ndc[0], ndc[1], ndc[2], guest_screen[0],
        guest_screen[1], guest_screen[2], subpixel_x, subpixel_y,
        finite_subpixel ? 1u : 0u, floor_subpixel_x, floor_subpixel_y,
        round_subpixel_x, round_subpixel_y,
        guest_screen[0] * float(draw_scale_x),
        guest_screen[1] * float(draw_scale_y));
  }
  std::fprintf(stderr,
               "REX_EMBEDDED_SCENE_VERTEX_OUTPUT_END kind=%s captured=%u\n",
               pair_kind, captured_count);

  // Hash the exact interpreter output for every unique vertex referenced by
  // this draw (within a deliberately bounded diagnostic budget). Sorting the
  // indices makes the result independent of primitive/index ordering while
  // retaining the index-to-output association in every output hash.
  const uint32_t full_hash_ordinal_count =
      std::min(available_index_count, kMaximumFullHashIndexOrdinals);
  std::vector<uint32_t> full_hash_indices;
  full_hash_indices.reserve(full_hash_ordinal_count);
  for (uint32_t ordinal = 0; ordinal < full_hash_ordinal_count; ++ordinal) {
    uint32_t vertex_index;
    if (decode_vertex_index(ordinal, vertex_index)) {
      full_hash_indices.push_back(vertex_index);
    }
  }
  std::sort(full_hash_indices.begin(), full_hash_indices.end());
  full_hash_indices.erase(
      std::unique(full_hash_indices.begin(), full_hash_indices.end()),
      full_hash_indices.end());
  const bool full_hash_truncated =
      available_index_count > kMaximumFullHashIndexOrdinals ||
      full_hash_indices.size() > kMaximumFullHashUniqueVertices;
  if (full_hash_indices.size() > kMaximumFullHashUniqueVertices) {
    full_hash_indices.resize(kMaximumFullHashUniqueVertices);
  }

  constexpr uint64_t kFnvOffsetBasis = UINT64_C(1469598103934665603);
  constexpr uint64_t kFnvPrime = UINT64_C(1099511628211);
  const auto hash_word = [](uint64_t hash, uint32_t word) {
    for (uint32_t byte = 0; byte < 4; ++byte) {
      hash ^= uint8_t(word >> (byte * 8));
      hash *= kFnvPrime;
    }
    return hash;
  };
  uint64_t index_hash = kFnvOffsetBasis;
  uint64_t clip_hash = kFnvOffsetBasis;
  uint64_t screen_hash = kFnvOffsetBasis;
  uint64_t depth_hash = kFnvOffsetBasis;
  uint32_t killed_count = 0;
  uint32_t nonfinite_count = 0;
  uint32_t outside_clip_count = 0;
  for (uint32_t vertex_index : full_hash_indices) {
    sink.Reset();
    std::fill(interpreter.temp_registers(),
              interpreter.temp_registers() +
                  xenos::kMaxShaderTempRegisters * 4,
              0.0f);
    interpreter.temp_registers()[0] = float(vertex_index);
    interpreter.Execute();

    const auto& position = sink.position();
    float ndc[3] = {position[0], position[1], position[2]};
    const bool have_w = (sink.position_mask() & 0b1000) != 0;
    if (!vte.vtx_xy_fmt && have_w) {
      ndc[0] /= position[3];
      ndc[1] /= position[3];
    }
    if (!vte.vtx_z_fmt && have_w) {
      ndc[2] /= position[3];
    }
    const float guest_screen[3] = {
        ndc[0] * viewport_scale[0] + viewport_offset[0],
        ndc[1] * viewport_scale[1] + viewport_offset[1],
        ndc[2] * viewport_scale[2] + viewport_offset[2],
    };

    index_hash = hash_word(index_hash, vertex_index);
    clip_hash = hash_word(clip_hash, vertex_index);
    clip_hash = hash_word(clip_hash, sink.position_mask());
    clip_hash = hash_word(clip_hash, sink.killed() ? 1u : 0u);
    for (float component : position) {
      clip_hash = hash_word(
          clip_hash, rex::memory::Reinterpret<uint32_t>(component));
    }
    screen_hash = hash_word(screen_hash, vertex_index);
    for (float component : guest_screen) {
      screen_hash = hash_word(
          screen_hash, rex::memory::Reinterpret<uint32_t>(component));
    }
    depth_hash = hash_word(depth_hash, vertex_index);
    depth_hash = hash_word(
        depth_hash, rex::memory::Reinterpret<uint32_t>(guest_screen[2]));

    if (sink.killed()) {
      ++killed_count;
    }
    const bool finite_position =
        std::isfinite(position[0]) && std::isfinite(position[1]) &&
        std::isfinite(position[2]) && std::isfinite(position[3]) &&
        std::isfinite(guest_screen[0]) &&
        std::isfinite(guest_screen[1]) &&
        std::isfinite(guest_screen[2]);
    if (!finite_position) {
      ++nonfinite_count;
    } else if (!clip_control.clip_disable &&
               sink.position_mask() == 0b1111) {
      const float clip_z_min =
          clip_control.dx_clip_space_def ? 0.0f : -position[3];
      if (position[3] <= 0.0f || position[0] < -position[3] ||
          position[0] > position[3] || position[1] < -position[3] ||
          position[1] > position[3] || position[2] < clip_z_min ||
          position[2] > position[3]) {
        ++outside_clip_count;
      }
    }
  }
  std::fprintf(
      stderr,
      "REX_EMBEDDED_SCENE_VERTEX_OUTPUT_FULL kind=%s indices_scanned=%u unique=%u truncated=%u index_hash=0x%016llX clip_hash=0x%016llX screen_hash=0x%016llX depth_hash=0x%016llX killed=%u nonfinite=%u outside=%u\n",
      pair_kind, full_hash_ordinal_count,
      uint32_t(full_hash_indices.size()), full_hash_truncated ? 1u : 0u,
      static_cast<unsigned long long>(index_hash),
      static_cast<unsigned long long>(clip_hash),
      static_cast<unsigned long long>(screen_hash),
      static_cast<unsigned long long>(depth_hash), killed_count,
      nonfinite_count, outside_clip_count);
  std::fflush(stderr);
}

// Low-volume counters for locating the final rendered-frame frontier in an
// embedded title. These intentionally don't retain per-draw data. IssueSwap
// emits only bounded summaries before resetting the per-frame state.
struct EmbeddedFrameFrontier {
  struct ResolveDestination {
    uint32_t address = 0;
    uint32_t count = 0;
    uint64_t bytes = 0;
  };

  struct DrawState {
    struct TextureState {
      uint32_t valid = 0;
      uint32_t guest_base = 0;
      uint32_t guest_size = 0;
      uint32_t guest_width = 0;
      uint32_t guest_height = 0;
      uint32_t guest_depth_or_array_size = 0;
      uint32_t guest_format = 0;
      uint32_t guest_dimension = 0;
      uint32_t guest_tiled = 0;
      uint32_t scaled_resolve = 0;
      uint32_t outdated_mask = 0;
      uint32_t descriptor_index = UINT32_MAX;
      uint32_t descriptor_index_signed = UINT32_MAX;
      uint64_t resource_identity = 0;
      uint64_t resource_width = 0;
      uint32_t resource_height = 0;
      uint32_t resource_depth_or_array_size = 0;
      uint32_t resource_mip_levels = 0;
      uint32_t resource_format = 0;
    };

    uint64_t signature = 0;
    uint64_t occurrences = 0;
    uint64_t first_draw_ordinal = 0;
    uint64_t last_draw_ordinal = 0;
    uint64_t vertex_shader = 0;
    uint64_t pixel_shader = 0;
    uint64_t bound_render_target_formats = 0;
    uint64_t texture_format_mask = 0;
    uint32_t primitive_type = 0;
    uint32_t index_count = 0;
    uint32_t min_index_count = 0;
    uint32_t max_index_count = 0;
    uint32_t guest_draw_vertex_count = 0;
    uint32_t host_draw_vertex_count = 0;
    uint32_t index_buffer_type = 0;
    uint32_t guest_index_base = 0;
    uint32_t host_index_format = 0;
    uint32_t used_texture_mask = 0;
    uint32_t blend_candidate_mask = 0;
    uint32_t texture_fetch_count = 0;
    uint32_t texture_fetch_indices[8]{};
    uint32_t texture_fetch_words[8][6]{};
    TextureState texture_states[8]{};
    uint64_t first_texture_resource_hash = 0;
    uint64_t last_texture_resource_hash = 0;
    uint32_t texture_resource_changes = 0;
    uint32_t vertex_fetch_count = 0;
    uint32_t vertex_fetch_indices[8]{};
    uint32_t vertex_fetch_words[8][2]{};
    uint64_t first_vertex_resource_hash = 0;
    uint64_t last_vertex_resource_hash = 0;
    uint32_t vertex_resource_changes = 0;
    uint32_t depth_texture_mask = 0;
    uint32_t first_depth_texture_base = 0;
    uint32_t last_depth_texture_base = 0;
    uint32_t depth_texture_base_changes = 0;
    uint32_t first_depth_texture_format = 0;
    uint32_t bound_render_target_bits = 0;
    uint32_t surface_info = 0;
    uint32_t color_info[4]{};
    uint32_t color_mask = 0;
    uint32_t normalized_color_mask = 0;
    uint32_t color_control = 0;
    uint32_t blend_control[4]{};
    uint32_t depth_info = 0;
    uint32_t depth_control = 0;
    uint32_t normalized_depth_control = 0;
    uint32_t stencil_ref_mask = 0;
    uint32_t stencil_ref_mask_back = 0;
    uint32_t mode_control = 0;
    uint32_t raster_control = 0;
    uint32_t screen_scissor_tl = 0;
    uint32_t screen_scissor_br = 0;
    uint32_t window_offset = 0;
    uint32_t window_scissor_tl = 0;
    uint32_t window_scissor_br = 0;
    uint32_t draw_scale_x = 1;
    uint32_t draw_scale_y = 1;
    uint32_t scaled_texture_mask = 0;
    uint32_t pixel_shader_flags = 0;
  };

  struct ResolveState {
    uint32_t control = 0;
    uint32_t destination_base = 0;
    uint32_t destination_info = 0;
    uint32_t destination_pitch = 0;
    uint32_t surface_info = 0;
    uint32_t source_color_info = 0;
    uint32_t depth_info = 0;
    uint32_t written_address = 0;
    uint32_t written_length = 0;
    uint32_t written_scaled = 0;
  };

  uint64_t draw_packets = 0;
  uint64_t submitted_draws = 0;
  uint64_t pixel_shader_draws = 0;
  uint64_t memexport_draws = 0;
  uint64_t submitted_vertices = 0;
  uint64_t render_target_update_calls = 0;
  uint64_t render_target_update_us = 0;
  uint64_t render_target_update_max_us = 0;
  uint64_t pipeline_configure_calls = 0;
  uint64_t pipeline_configure_us = 0;
  uint64_t pipeline_configure_max_us = 0;
  uint64_t pipeline_placeholder_draws = 0;
  uint64_t pipeline_current_reuses = 0;
  uint64_t pipeline_cache_hits = 0;
  uint64_t pipeline_cache_misses = 0;
  uint64_t pipeline_async_queued = 0;
  uint64_t pipeline_sync_created = 0;
  uint64_t pipeline_async_completed = 0;
  uint64_t pipeline_async_failed = 0;
  uint64_t pipeline_queue_depth = 0;
  uint64_t pipeline_threads_busy = 0;
  uint64_t texture_request_calls = 0;
  uint64_t texture_request_us = 0;
  uint64_t texture_request_max_us = 0;
  uint64_t issue_swap_us = 0;
  uint64_t host_submission_submitted = 0;
  uint64_t host_submission_completed = 0;
  uint64_t host_submission_backlog = 0;
  uint64_t resolve_attempts = 0;
  uint64_t successful_resolves = 0;
  uint64_t resolved_bytes = 0;
  uint64_t state_hash = UINT64_C(1469598103934665603);
  uint32_t last_resolve_destination = 0;
  std::array<ResolveDestination, 32> resolve_destinations{};
  uint32_t resolve_destination_count = 0;
  std::array<DrawState, 256> draw_states{};
  uint32_t draw_state_count = 0;
  uint32_t dropped_draw_states = 0;
  std::array<ResolveState, 96> resolve_states{};
  uint32_t resolve_state_count = 0;
  uint32_t dropped_resolve_states = 0;
  uint64_t zpd_begins = 0;
  uint64_t zpd_ends = 0;
  uint64_t zpd_segments_opened = 0;
  uint64_t zpd_segments_closed = 0;
  uint64_t zpd_segments_retired = 0;
  uint64_t zpd_raw_samples = 0;
  uint64_t zpd_normalized_samples = 0;
  uint32_t zpd_last_slot = 0;
  uint32_t zpd_last_begin_value = 0;
  uint32_t zpd_last_end_value = 0;

  void Mix(uint64_t value) {
    state_hash ^= value;
    state_hash *= UINT64_C(1099511628211);
  }

  void RecordResolve(uint32_t address, uint32_t bytes) {
    uint32_t index = 0;
    while (index < resolve_destination_count &&
           resolve_destinations[index].address != address) {
      ++index;
    }
    if (index == resolve_destination_count &&
        resolve_destination_count < resolve_destinations.size()) {
      resolve_destinations[index].address = address;
      ++resolve_destination_count;
    }
    if (index < resolve_destination_count) {
      ++resolve_destinations[index].count;
      resolve_destinations[index].bytes += bytes;
    }
  }

  bool RecordDrawState(const DrawState& state) {
    for (uint32_t i = 0; i < draw_state_count; ++i) {
      if (draw_states[i].signature == state.signature) {
        bool resources_changed = false;
        ++draw_states[i].occurrences;
        draw_states[i].last_draw_ordinal = state.last_draw_ordinal;
        draw_states[i].min_index_count =
            std::min(draw_states[i].min_index_count, state.index_count);
        draw_states[i].max_index_count =
            std::max(draw_states[i].max_index_count, state.index_count);
        if (draw_states[i].last_depth_texture_base !=
            state.first_depth_texture_base) {
          draw_states[i].last_depth_texture_base =
              state.first_depth_texture_base;
          ++draw_states[i].depth_texture_base_changes;
        }
        if (draw_states[i].last_texture_resource_hash !=
            state.first_texture_resource_hash) {
          draw_states[i].last_texture_resource_hash =
              state.first_texture_resource_hash;
          ++draw_states[i].texture_resource_changes;
          resources_changed = true;
        }
        if (draw_states[i].last_vertex_resource_hash !=
            state.first_vertex_resource_hash) {
          draw_states[i].last_vertex_resource_hash =
              state.first_vertex_resource_hash;
          ++draw_states[i].vertex_resource_changes;
          resources_changed = true;
        }
        return resources_changed;
      }
    }
    if (draw_state_count == draw_states.size()) {
      ++dropped_draw_states;
      return false;
    }
    draw_states[draw_state_count] = state;
    draw_states[draw_state_count].occurrences = 1;
    draw_states[draw_state_count].min_index_count = state.index_count;
    draw_states[draw_state_count].max_index_count = state.index_count;
    draw_states[draw_state_count].last_depth_texture_base =
        state.first_depth_texture_base;
    draw_states[draw_state_count].last_texture_resource_hash =
        state.first_texture_resource_hash;
    draw_states[draw_state_count].last_vertex_resource_hash =
        state.first_vertex_resource_hash;
    ++draw_state_count;
    return true;
  }

  void RecordResolveState(const ResolveState& state) {
    if (resolve_state_count == resolve_states.size()) {
      ++dropped_resolve_states;
      return;
    }
    resolve_states[resolve_state_count++] = state;
  }

  void Reset() { *this = EmbeddedFrameFrontier{}; }
};

EmbeddedFrameFrontier embedded_frame_frontier;
uint64_t embedded_completed_swap_ordinal = 0;
// GPU-thread-owned one-shot diagnostic scope. This never changes guest state.
// Earlier draws in this frame were not collected, so explicitly label the scope.
uint64_t embedded_writer_match_frame = 0;
uint64_t embedded_writer_match_first_draw = 0;
std::atomic<uint64_t> embedded_manual_capture_request_generation{0};
embedded_manual_capture_policy::State embedded_manual_capture_state;

struct EmbeddedCameraDraw {
  uint64_t draw = 0;
  uint64_t vertex_shader = 0;
  uint64_t pixel_shader = 0;
  uint64_t constant_buffer = 0;
  uint32_t surface = 0;
  uint32_t depth = 0;
  uint32_t depth_control = 0;
  uint32_t window_offset = 0;
  uint32_t scissor_tl = 0;
  uint32_t scissor_br = 0;
  uint32_t viewport[6]{}; // X scale/offset, Y scale/offset, Z scale/offset
  uint32_t vte = 0;
  uint32_t clip = 0;
  uint32_t raster = 0, vertex_control = 0;
  uint32_t host_viewport[6]{}, host_ndc[6]{};
  int32_t host_scissor[4]{};
  uint32_t scale_x = 1;
  uint32_t scale_y = 1;
  embedded_camera_draw_capture_policy::Constants constants;
  uint64_t pixel_constant_buffer = 0;
  embedded_camera_draw_capture_policy::Constants pixel_constants;
  uint32_t target_color[4]{}, target_blend[4]{};
  uint32_t target_mask = 0, target_control = 0, target_bits = 0;
  uint32_t textures = 0, primitive = 0, vertices = 0;
};
embedded_camera_draw_capture_policy::Budget embedded_camera_draw_budget;
embedded_camera_geometry_capture_policy::DrawBudget embedded_camera_geometry_budget;
embedded_camera_depth_clear_policy::Budget embedded_camera_depth_clear_budget;
embedded_scene_resolve_capture_policy::State embedded_scene_resolve_budget;
embedded_scene_transfer_capture_policy::FrameBudget embedded_scene_update_budget;
// Allocated only during the selected diagnostic frame; owned copies survive
// upload reuse. This is separate from the frequently reset frame frontier.
std::vector<EmbeddedCameraDraw> embedded_camera_draws;

struct EmbeddedHistoryDraw {
  EmbeddedCameraDraw camera;
  uint64_t submission = 0;
  uint32_t index_kind = 0, index_address = 0, index_format = 0, index_endian = 0;
  uint32_t index_offset = 0, min_index = 0, max_index = 0, reset_enable = 0, reset_index = 0;
  uint32_t bindings = 0;
  bool memexport = false, bindings_complete = false;
  struct Vertex { uint32_t fetch = 0, word0 = 0, word1 = 0, stride = 0; };
  std::array<Vertex, 8> vertices{};
};
embedded_camera_history_capture_policy::Budget embedded_camera_history_budget;
std::vector<EmbeddedHistoryDraw> embedded_camera_history_draws[
    embedded_camera_history_capture_policy::Budget::kMaximumFrames];

void FinishEmbeddedCameraHistory(uint64_t frame, bool refreshed, uint32_t frontbuffer,
                                 uint64_t submission, uint64_t completed) {
  auto& budget = embedded_camera_history_budget;
  const int index = budget.Finish(frame, embedded_frame_frontier.submitted_draws);
  if (index == -1) return;
  if (index == -2) {
    std::fprintf(stderr, "REX_CAMERA_HISTORY_ABORT generation=%llu first_frame=%llu actual_frame=%llu reason=lost_adjacent_boundary\n",
        static_cast<unsigned long long>(budget.generation), static_cast<unsigned long long>(budget.first_frame),
        static_cast<unsigned long long>(frame));
    for (auto& draws : embedded_camera_history_draws) draws.clear();
    return;
  }
  auto& draws = embedded_camera_history_draws[index];
  char path[128];
  std::snprintf(path, sizeof(path), "rex_camera_history_generation_%llu_frame_%llu.log",
      static_cast<unsigned long long>(budget.generation), static_cast<unsigned long long>(frame));
  FILE* file = std::fopen(path, "wb");
  bool written = false;
  if (file) {
    std::fprintf(file,
        "CAMERA_HISTORY version=%u generation=%llu first_frame=%llu frame=%llu index=%u frames=%u "
        "refreshed=%u frontbuffer=%08X observed=%llu selected=%u dropped=%u failures=%u aborted=%u "
        "submission=%llu completed=%llu scope=adjacent_bound_inputs_not_object_identity_or_pixel_motion\n",
        budget.frame_count == 2 ? 1u : 2u,
        static_cast<unsigned long long>(budget.generation), static_cast<unsigned long long>(budget.first_frame),
        static_cast<unsigned long long>(frame), uint32_t(index), budget.frame_count, refreshed ? 1u : 0u, frontbuffer,
        static_cast<unsigned long long>(budget.observed[index]), budget.captured[index], budget.dropped[index],
        budget.failures[index], budget.aborted ? 1u : 0u,
        static_cast<unsigned long long>(submission), static_cast<unsigned long long>(completed));
    for (const auto& history : draws) {
      const auto& draw = history.camera;
      std::fprintf(file,
          "HISTORY_DRAW draw=%llu vs=%016llX ps=%016llX submission=%llu surface=%08X depth=%08X "
          "depth_control=%08X offset=%08X scissor=%08X,%08X scale=%ux%u mask=%08X bound_bits=%X "
          "primitive=%u vertices=%u index_kind=%u index_address=%08X index_format=%u index_endian=%u "
          "index_offset=%u min_index=%u max_index=%u reset_enable=%u reset_index=%u "
          "bindings=%u bindings_complete=%u memexport=%u\n",
          static_cast<unsigned long long>(draw.draw), static_cast<unsigned long long>(draw.vertex_shader),
          static_cast<unsigned long long>(draw.pixel_shader), static_cast<unsigned long long>(history.submission),
          draw.surface, draw.depth, draw.depth_control, draw.window_offset, draw.scissor_tl, draw.scissor_br,
          draw.scale_x, draw.scale_y, draw.target_mask, draw.target_bits, draw.primitive, draw.vertices,
          history.index_kind, history.index_address, history.index_format, history.index_endian,
          history.index_offset, history.min_index, history.max_index, history.reset_enable, history.reset_index,
          history.bindings, history.bindings_complete ? 1u : 0u, history.memexport ? 1u : 0u);
      for (uint32_t slot = 0; slot < std::min<uint32_t>(history.bindings, uint32_t(history.vertices.size())); ++slot) {
        const auto& vertex = history.vertices[slot];
        std::fprintf(file, "HISTORY_VERTEX draw=%llu slot=%u fetch=%u words=%08X,%08X stride=%u\n",
            static_cast<unsigned long long>(draw.draw), slot, vertex.fetch, vertex.word0, vertex.word1, vertex.stride);
      }
      for (uint32_t stage = 0; stage < 2; ++stage) {
        const auto& constants = stage ? draw.pixel_constants : draw.constants;
        std::fprintf(file, "HISTORY_CONSTANTS draw=%llu stage=%s cbuffer=%016llX valid=%u count=%u map=%016llX,%016llX,%016llX,%016llX words=",
            static_cast<unsigned long long>(draw.draw), stage ? "pixel" : "vertex",
            static_cast<unsigned long long>(stage ? draw.pixel_constant_buffer : draw.constant_buffer),
            constants.valid ? 1u : 0u, constants.count,
            static_cast<unsigned long long>(constants.map[0]), static_cast<unsigned long long>(constants.map[1]),
            static_cast<unsigned long long>(constants.map[2]), static_cast<unsigned long long>(constants.map[3]));
        for (uint32_t i = 0; i < constants.count * 4; ++i)
          std::fprintf(file, "%s%08X", i ? "," : "", constants.words[i]);
        std::fputc('\n', file);
      }
      std::fprintf(file, "HISTORY_RASTER draw=%llu viewport=%08X,%08X,%08X,%08X,%08X,%08X "
          "vte=%08X clip=%08X raster=%08X vertex_control=%08X host_scissor=%d,%d,%d,%d "
          "host_viewport=%08X,%08X,%08X,%08X,%08X,%08X host_ndc=%08X,%08X,%08X,%08X,%08X,%08X\n",
          static_cast<unsigned long long>(draw.draw), draw.viewport[0], draw.viewport[1], draw.viewport[2],
          draw.viewport[3], draw.viewport[4], draw.viewport[5], draw.vte, draw.clip, draw.raster, draw.vertex_control,
          draw.host_scissor[0], draw.host_scissor[1], draw.host_scissor[2], draw.host_scissor[3],
          draw.host_viewport[0], draw.host_viewport[1], draw.host_viewport[2], draw.host_viewport[3], draw.host_viewport[4], draw.host_viewport[5],
          draw.host_ndc[0], draw.host_ndc[1], draw.host_ndc[2], draw.host_ndc[3], draw.host_ndc[4], draw.host_ndc[5]);
    }
    std::fprintf(file, "HISTORY_END generation=%llu frame=%llu records=%u\n",
        static_cast<unsigned long long>(budget.generation), static_cast<unsigned long long>(frame), uint32_t(draws.size()));
    written = std::ferror(file) == 0;
    if (std::fclose(file) != 0) written = false;
  }
  std::fprintf(stderr, "REX_CAMERA_HISTORY_FRAME generation=%llu frame=%llu index=%u selected=%u dropped=%u "
      "failures=%u aborted=%u written=%u path=%s scope=bound_inputs_not_history_acceptance\n",
      static_cast<unsigned long long>(budget.generation), static_cast<unsigned long long>(frame), uint32_t(index),
      budget.captured[index], budget.dropped[index], budget.failures[index], budget.aborted ? 1u : 0u,
      written ? 1u : 0u, path);
  std::fflush(stderr);
  draws.clear();
}

struct EmbeddedInputTransition {
  std::atomic_flag write_lock = ATOMIC_FLAG_INIT;
  std::atomic<uint64_t> sequence{0};
  std::atomic<int64_t> host_performance_counter{0};
  std::atomic<int64_t> host_performance_frequency{0};
  std::atomic<uint32_t> packet_number{0};
  std::atomic<uint32_t> buttons{0};
};

EmbeddedInputTransition embedded_input_transition;
uint64_t embedded_input_swap_sequence = 0;
int64_t embedded_input_swap_counter = 0;
int64_t embedded_input_swap_frequency = 0;
uint32_t embedded_input_swap_packet = 0;
uint32_t embedded_input_swap_buttons = 0;
uint32_t embedded_input_swap_step = 0;
uint32_t embedded_input_swap_remaining = 0;

void NoteEmbeddedInputTransition(int64_t host_performance_counter,
                                 int64_t host_performance_frequency,
                                 uint32_t packet_number,
                                 uint32_t buttons) noexcept {
  if (host_performance_frequency <= 0 ||
      embedded_input_transition.write_lock.test_and_set(
          std::memory_order_acquire)) {
    return;
  }
  embedded_input_transition.host_performance_counter.store(
      host_performance_counter, std::memory_order_relaxed);
  embedded_input_transition.host_performance_frequency.store(
      host_performance_frequency, std::memory_order_relaxed);
  embedded_input_transition.packet_number.store(packet_number,
                                                 std::memory_order_relaxed);
  embedded_input_transition.buttons.store(buttons, std::memory_order_relaxed);
  embedded_input_transition.sequence.fetch_add(1, std::memory_order_release);
  embedded_input_transition.write_lock.clear(std::memory_order_release);
}

void RequestEmbeddedGameplayCapture() noexcept {
  const uint64_t generation =
      embedded_manual_capture_request_generation.fetch_add(
          1, std::memory_order_release) +
      1;
  std::fprintf(stderr,
               "REX_EMBEDDED_MANUAL_CAPTURE_REQUEST generation=%llu enabled=%u\n",
               static_cast<unsigned long long>(generation),
               REXCVAR_GET(embedded_manual_gameplay_capture) ? 1u : 0u);
  std::fflush(stderr);
}

bool GetEmbeddedGameplayCaptureRequest(uint64_t& generation) noexcept {
  generation = 0;
  if (!REXCVAR_GET(embedded_manual_gameplay_capture)) return false;
  generation = embedded_manual_capture_request_generation.load(
      std::memory_order_acquire);
  return true;
}

// Play-testing snapshot (#14): see embedded_diagnostics.h. The request comes
// from the UI thread; the file is opened, written and closed on the command
// processor thread only (at guest swaps, draws and resolves).
std::mutex embedded_frame_dump_request_mutex;
std::string embedded_frame_dump_request_path;  // embedded_frame_dump_request_mutex
std::atomic<bool> embedded_frame_dump_requested{false};
std::FILE* embedded_frame_dump_file = nullptr;  // command processor thread
std::string embedded_frame_dump_path;           // command processor thread
uint32_t embedded_frame_dump_draws = 0;
uint32_t embedded_frame_dump_copies = 0;

void RequestEmbeddedFrameDump(const char* path) noexcept {
  if (!path || !*path) return;
  try {
    std::lock_guard<std::mutex> lock(embedded_frame_dump_request_mutex);
    embedded_frame_dump_request_path = path;
  } catch (...) {
    return;
  }
  embedded_frame_dump_requested.store(true, std::memory_order_release);
}

// At each guest swap: finishes a frame being written, then starts a requested
// one (the frame after this swap).
void EmbeddedFrameDumpAtSwap(uint64_t frame, uint32_t scale_x, uint32_t scale_y) {
  if (embedded_frame_dump_file) {
    std::fprintf(embedded_frame_dump_file, "END frame=%llu draws=%u copies=%u\n",
                 static_cast<unsigned long long>(frame), embedded_frame_dump_draws,
                 embedded_frame_dump_copies);
    std::fclose(embedded_frame_dump_file);
    embedded_frame_dump_file = nullptr;
    std::fprintf(stderr, "REX_DIAG_FRAME_DUMP written=%s draws=%u copies=%u frame=%llu\n",
                 embedded_frame_dump_path.c_str(), embedded_frame_dump_draws,
                 embedded_frame_dump_copies, static_cast<unsigned long long>(frame));
    std::fflush(stderr);
  }
  if (!embedded_frame_dump_requested.exchange(false, std::memory_order_acq_rel)) return;
  {
    std::lock_guard<std::mutex> lock(embedded_frame_dump_request_mutex);
    embedded_frame_dump_path = embedded_frame_dump_request_path;
  }
#if defined(_WIN32)
  const std::filesystem::path path = std::filesystem::u8path(embedded_frame_dump_path);
  embedded_frame_dump_file = _wfopen(path.c_str(), L"w");
#else
  embedded_frame_dump_file = std::fopen(embedded_frame_dump_path.c_str(), "w");
#endif
  embedded_frame_dump_draws = 0;
  embedded_frame_dump_copies = 0;
  if (!embedded_frame_dump_file) {
    std::fprintf(stderr, "REX_DIAG_FRAME_DUMP failed=%s\n", embedded_frame_dump_path.c_str());
    std::fflush(stderr);
    return;
  }
  std::fprintf(embedded_frame_dump_file,
               "FRAME_DUMP version=1 after_frame=%llu draw_scale=%ux%u\n"
               "# D <draw> bins=select/mask vs ps prim verts surf c0..c3 depth mask colorctl "
               "blend0..3 depthctl stencil stencil_bf modectl sumode woffset scissor_tl "
               "scissor_br t<fetch>=6 dwords\n"
               "# P = a packet the predicated-tiling bin check skipped\n"
               "# C <copy> copyctl dest_base dest_pitch dest_info surf c0 depth woffset\n",
               static_cast<unsigned long long>(frame), scale_x, scale_y);
}

void EmbeddedFrameDumpDraw(const RegisterFile& regs, const DxbcShader& vertex_shader,
                           const DxbcShader* pixel_shader, uint32_t primitive_type,
                           uint32_t vertices, uint64_t bin_select, uint64_t bin_mask) {
  std::FILE* file = embedded_frame_dump_file;
  std::fprintf(
      file,
      "D %u bins=%llX/%llX vs=%016llX ps=%016llX prim=%u verts=%u surf=%08X "
      "c=%08X,%08X,%08X,%08X "
      "depth=%08X mask=%08X colorctl=%08X blend=%08X,%08X,%08X,%08X depthctl=%08X "
      "stencil=%08X,%08X modectl=%08X sumode=%08X woff=%08X sc=%08X,%08X",
      embedded_frame_dump_draws++, static_cast<unsigned long long>(bin_select),
      static_cast<unsigned long long>(bin_mask),
      static_cast<unsigned long long>(vertex_shader.ucode_data_hash()),
      static_cast<unsigned long long>(pixel_shader ? pixel_shader->ucode_data_hash() : 0),
      primitive_type, vertices, regs[XE_GPU_REG_RB_SURFACE_INFO], regs[XE_GPU_REG_RB_COLOR_INFO],
      regs[XE_GPU_REG_RB_COLOR1_INFO], regs[XE_GPU_REG_RB_COLOR2_INFO],
      regs[XE_GPU_REG_RB_COLOR3_INFO], regs[XE_GPU_REG_RB_DEPTH_INFO],
      regs[XE_GPU_REG_RB_COLOR_MASK], regs[XE_GPU_REG_RB_COLORCONTROL],
      regs[XE_GPU_REG_RB_BLENDCONTROL0], regs[XE_GPU_REG_RB_BLENDCONTROL1],
      regs[XE_GPU_REG_RB_BLENDCONTROL2], regs[XE_GPU_REG_RB_BLENDCONTROL3],
      regs[XE_GPU_REG_RB_DEPTHCONTROL], regs[XE_GPU_REG_RB_STENCILREFMASK],
      regs[XE_GPU_REG_RB_STENCILREFMASK_BF], regs[XE_GPU_REG_RB_MODECONTROL],
      regs[XE_GPU_REG_PA_SU_SC_MODE_CNTL], regs[XE_GPU_REG_PA_SC_WINDOW_OFFSET],
      regs[XE_GPU_REG_PA_SC_WINDOW_SCISSOR_TL], regs[XE_GPU_REG_PA_SC_WINDOW_SCISSOR_BR]);
  uint32_t seen = 0;
  auto bindings = [&](const DxbcShader& shader) {
    for (const auto& binding : shader.GetTextureBindingsAfterTranslation()) {
      const uint32_t index = binding.fetch_constant;
      if (index >= 32 || (seen & (1u << index))) continue;
      seen |= 1u << index;
      const xenos::xe_gpu_texture_fetch_t fetch = regs.GetTextureFetch(index);
      std::fprintf(file, " t%u=%08X.%08X.%08X.%08X.%08X.%08X", index, fetch.dword_0,
                   fetch.dword_1, fetch.dword_2, fetch.dword_3, fetch.dword_4, fetch.dword_5);
    }
  };
  bindings(vertex_shader);
  if (pixel_shader) bindings(*pixel_shader);
  std::fputc('\n', file);
}

void EmbeddedFrameDumpCopy(const RegisterFile& regs) {
  std::fprintf(embedded_frame_dump_file,
               "C %u copyctl=%08X dest_base=%08X dest_pitch=%08X dest_info=%08X surf=%08X "
               "c0=%08X depth=%08X woff=%08X\n",
               embedded_frame_dump_copies++, regs[XE_GPU_REG_RB_COPY_CONTROL],
               regs[XE_GPU_REG_RB_COPY_DEST_BASE], regs[XE_GPU_REG_RB_COPY_DEST_PITCH],
               regs[XE_GPU_REG_RB_COPY_DEST_INFO], regs[XE_GPU_REG_RB_SURFACE_INFO],
               regs[XE_GPU_REG_RB_COLOR_INFO], regs[XE_GPU_REG_RB_DEPTH_INFO],
               regs[XE_GPU_REG_PA_SC_WINDOW_OFFSET]);
}

void TraceEmbeddedInputToSwap(uint64_t swap_ordinal, bool refresh_succeeded) {
  const uint64_t sequence =
      embedded_input_transition.sequence.load(std::memory_order_acquire);
  if (sequence != embedded_input_swap_sequence) {
    embedded_input_swap_sequence = sequence;
    embedded_input_swap_counter =
        embedded_input_transition.host_performance_counter.load(
            std::memory_order_relaxed);
    embedded_input_swap_frequency =
        embedded_input_transition.host_performance_frequency.load(
            std::memory_order_relaxed);
    embedded_input_swap_packet =
        embedded_input_transition.packet_number.load(std::memory_order_relaxed);
    embedded_input_swap_buttons =
        embedded_input_transition.buttons.load(std::memory_order_relaxed);
    embedded_input_swap_step = 0;
    embedded_input_swap_remaining = sequence ? 8 : 0;
  }
  if (!embedded_input_swap_remaining || embedded_input_swap_frequency <= 0) {
    return;
  }
  LARGE_INTEGER swap_counter{};
  QueryPerformanceCounter(&swap_counter);
  const int64_t delta_ticks =
      swap_counter.QuadPart - embedded_input_swap_counter;
  const int64_t delta_us = delta_ticks >= 0
      ? (delta_ticks * INT64_C(1000000)) / embedded_input_swap_frequency
      : -1;
  std::fprintf(
      stderr,
      "REX_EMBEDDED_INPUT_TO_SWAP sequence=%llu step=%u swap=%llu "
      "refresh=%u input_qpc=%lld swap_qpc=%lld frequency=%lld delta_us=%lld "
      "packet=%u buttons=0x%04X state_hash=0x%016llX draws=%llu resolves=%llu\n",
      static_cast<unsigned long long>(embedded_input_swap_sequence),
      embedded_input_swap_step, static_cast<unsigned long long>(swap_ordinal),
      refresh_succeeded ? 1u : 0u,
      static_cast<long long>(embedded_input_swap_counter),
      static_cast<long long>(swap_counter.QuadPart),
      static_cast<long long>(embedded_input_swap_frequency),
      static_cast<long long>(delta_us), embedded_input_swap_packet,
      embedded_input_swap_buttons,
      static_cast<unsigned long long>(embedded_frame_frontier.state_hash),
      static_cast<unsigned long long>(embedded_frame_frontier.submitted_draws),
      static_cast<unsigned long long>(embedded_frame_frontier.successful_resolves));
  std::fflush(stderr);
  ++embedded_input_swap_step;
  --embedded_input_swap_remaining;
}

// The frame-diagnostic predicates below are constant false in player builds
// (REXGLUE_GPU_DIAGNOSTICS off), which removes every diagnostic they gate.
bool IsEmbeddedGameplayCaptureSwap(uint64_t swap_ordinal) {
  if constexpr (!kGpuDiagnostics) return false;
  if (embedded_writer_match_frame && swap_ordinal == embedded_writer_match_frame) {
    return true;
  }
  const uint64_t start = REXCVAR_GET(embedded_gameplay_capture_start_swap);
  const uint64_t count = REXCVAR_GET(embedded_gameplay_capture_count);
  if (!start || !count || swap_ordinal < start) {
    return false;
  }
  const uint64_t interval =
      std::max(uint64_t(1),
               uint64_t(REXCVAR_GET(embedded_gameplay_capture_interval_swaps)));
  const uint64_t delta = swap_ordinal - start;
  return !(delta % interval) && delta / interval < count;
}

bool IsEmbeddedGameplayCaptureEnabled() {
  if constexpr (!kGpuDiagnostics) return false;
  return (REXCVAR_GET(embedded_gameplay_capture_start_swap) != 0 &&
          REXCVAR_GET(embedded_gameplay_capture_count) != 0) ||
         (REXCVAR_GET(embedded_target_writer_auto_start_swap) != 0 &&
          REXCVAR_GET(embedded_target_writer_capture_count) != 0);
}

bool IsEmbeddedFrameDiagnosticsEnabled() {
  if constexpr (!kGpuDiagnostics) return false;
  return REXCVAR_GET(embedded_hitch_diagnostics) ||
         IsEmbeddedGameplayCaptureEnabled() ||
         (REXCVAR_GET(embedded_manual_gameplay_capture) &&
          embedded_manual_capture_policy::IsPending(
              embedded_manual_capture_request_generation.load(
                  std::memory_order_acquire),
              embedded_manual_capture_state));
}

bool IsCurrentEmbeddedGameplayCaptureFrame() {
  if constexpr (!kGpuDiagnostics) return false;
  return IsEmbeddedGameplayCaptureSwap(embedded_completed_swap_ordinal + 1) ||
         (REXCVAR_GET(embedded_manual_gameplay_capture) &&
          embedded_manual_capture_policy::IsCompleteFrameArmed(
              embedded_manual_capture_state));
}

void WriteEmbeddedGameplayCapture(uint64_t swap_ordinal, bool refresh_succeeded,
                                  uint32_t frontbuffer_ptr,
                                  uint64_t manual_generation = 0) {
  char path[128];
  if (manual_generation) {
    std::snprintf(path, sizeof(path),
                  "rex_gameplay_gpu_state_manual_%llu_swap_%llu.log",
                  static_cast<unsigned long long>(manual_generation),
                  static_cast<unsigned long long>(swap_ordinal));
  } else {
    std::snprintf(path, sizeof(path), "rex_gameplay_gpu_state_%llu.log",
                  static_cast<unsigned long long>(swap_ordinal));
  }
  FILE* file = std::fopen(path, "wb");
  if (!file) {
    std::fprintf(stderr,
                 "REX_EMBEDDED_GAMEPLAY_CAPTURE result=open_failed swap=%llu path=%s\n",
                 static_cast<unsigned long long>(swap_ordinal), path);
    std::fflush(stderr);
    return;
  }
  std::fprintf(
      file,
      "REX_EMBEDDED_GAMEPLAY_FRAME swap=%llu refresh=%u frontbuffer=0x%08X "
      "draw_packets=%llu submitted_draws=%llu pixel_draws=%llu vertices=%llu "
      "resolves=%llu draw_states=%u dropped_draw_states=%u resolve_states=%u "
      "dropped_resolve_states=%u state_hash=0x%016llX\n",
      static_cast<unsigned long long>(swap_ordinal), refresh_succeeded ? 1u : 0u,
      frontbuffer_ptr,
      static_cast<unsigned long long>(embedded_frame_frontier.draw_packets),
      static_cast<unsigned long long>(embedded_frame_frontier.submitted_draws),
      static_cast<unsigned long long>(embedded_frame_frontier.pixel_shader_draws),
      static_cast<unsigned long long>(embedded_frame_frontier.submitted_vertices),
      static_cast<unsigned long long>(embedded_frame_frontier.successful_resolves),
      embedded_frame_frontier.draw_state_count,
      embedded_frame_frontier.dropped_draw_states,
      embedded_frame_frontier.resolve_state_count,
      embedded_frame_frontier.dropped_resolve_states,
      static_cast<unsigned long long>(embedded_frame_frontier.state_hash));
  if (swap_ordinal == embedded_writer_match_frame) {
    std::fprintf(file,
        "CAPTURE_SCOPE kind=exact_writer_partial_frame first_observed_draw=%llu "
        "earlier_draw_states=NOT_CAPTURED frame_counters=WHOLE_FRAME\n",
        static_cast<unsigned long long>(embedded_writer_match_first_draw));
  }
  std::fprintf(
      file,
      "ZPD begins=%llu ends=%llu segments_opened=%llu segments_closed=%llu "
      "segments_retired=%llu raw_samples=%llu normalized_samples=%llu "
      "last_slot=0x%08X last_begin=%u last_end=%u\n",
      static_cast<unsigned long long>(embedded_frame_frontier.zpd_begins),
      static_cast<unsigned long long>(embedded_frame_frontier.zpd_ends),
      static_cast<unsigned long long>(embedded_frame_frontier.zpd_segments_opened),
      static_cast<unsigned long long>(embedded_frame_frontier.zpd_segments_closed),
      static_cast<unsigned long long>(embedded_frame_frontier.zpd_segments_retired),
      static_cast<unsigned long long>(embedded_frame_frontier.zpd_raw_samples),
      static_cast<unsigned long long>(embedded_frame_frontier.zpd_normalized_samples),
      embedded_frame_frontier.zpd_last_slot,
      embedded_frame_frontier.zpd_last_begin_value,
      embedded_frame_frontier.zpd_last_end_value);
  for (uint32_t i = 0; i < embedded_frame_frontier.draw_state_count; ++i) {
    const EmbeddedFrameFrontier::DrawState& state =
        embedded_frame_frontier.draw_states[i];
    std::fprintf(
        file,
        "DRAW state=%u occurrences=%llu first_draw=%llu last_draw=%llu "
        "signature=0x%016llX primitive=%u indices=%u index_range=%u,%u "
        "vs=0x%016llX ps=0x%016llX ps_flags=0x%X used_textures=0x%08X "
        "texture_formats=0x%016llX depth_textures=0x%08X depth_texture_base=0x%08X "
        "depth_texture_last_base=0x%08X depth_texture_base_changes=%u "
        "depth_texture_format=%u bound_bits=0x%X bound_formats=0x%016llX "
        "surface=0x%08X color=%08X,%08X,%08X,%08X color_mask=0x%08X "
        "normalized_color_mask=0x%08X color_control=0x%08X "
        "blend=%08X,%08X,%08X,%08X depth_info=0x%08X depth_control=0x%08X "
        "normalized_depth_control=0x%08X stencil=0x%08X stencil_back=0x%08X "
        "mode=0x%08X raster=0x%08X screen_scissor=%08X,%08X "
        "window_offset=0x%08X window_scissor=%08X,%08X "
        "draw_scale=%ux%u scaled_textures=0x%08X\n",
        i, static_cast<unsigned long long>(state.occurrences),
        static_cast<unsigned long long>(state.first_draw_ordinal),
        static_cast<unsigned long long>(state.last_draw_ordinal),
        static_cast<unsigned long long>(state.signature), state.primitive_type,
        state.index_count, state.min_index_count, state.max_index_count,
        static_cast<unsigned long long>(state.vertex_shader),
        static_cast<unsigned long long>(state.pixel_shader), state.pixel_shader_flags,
        state.used_texture_mask,
        static_cast<unsigned long long>(state.texture_format_mask),
        state.depth_texture_mask, state.first_depth_texture_base,
        state.last_depth_texture_base, state.depth_texture_base_changes,
        state.first_depth_texture_format, state.bound_render_target_bits,
        static_cast<unsigned long long>(state.bound_render_target_formats),
        state.surface_info, state.color_info[0], state.color_info[1],
        state.color_info[2], state.color_info[3], state.color_mask,
        state.normalized_color_mask, state.color_control, state.blend_control[0],
        state.blend_control[1], state.blend_control[2], state.blend_control[3],
        state.depth_info, state.depth_control, state.normalized_depth_control,
        state.stencil_ref_mask, state.stencil_ref_mask_back, state.mode_control,
        state.raster_control, state.screen_scissor_tl, state.screen_scissor_br,
        state.window_offset, state.window_scissor_tl, state.window_scissor_br,
        state.draw_scale_x, state.draw_scale_y, state.scaled_texture_mask);
    std::fprintf(
        file,
        "RESOURCE state=%u guest_vertices=%u host_vertices=%u "
        "index_buffer_type=%u guest_index_base=0x%08X host_index_format=%u "
        "blend_candidate_mask=0x%X texture_fetches=%u "
        "texture_resource_hash_first=0x%016llX "
        "texture_resource_hash_last=0x%016llX texture_resource_changes=%u "
        "vertex_fetches=%u vertex_resource_hash_first=0x%016llX "
        "vertex_resource_hash_last=0x%016llX vertex_resource_changes=%u\n",
        i, state.guest_draw_vertex_count, state.host_draw_vertex_count,
        state.index_buffer_type, state.guest_index_base,
        state.host_index_format, state.blend_candidate_mask,
        state.texture_fetch_count,
        static_cast<unsigned long long>(state.first_texture_resource_hash),
        static_cast<unsigned long long>(state.last_texture_resource_hash),
        state.texture_resource_changes, state.vertex_fetch_count,
        static_cast<unsigned long long>(state.first_vertex_resource_hash),
        static_cast<unsigned long long>(state.last_vertex_resource_hash),
        state.vertex_resource_changes);
    for (uint32_t fetch_index = 0; fetch_index < state.texture_fetch_count;
         ++fetch_index) {
      std::fprintf(
          file,
          "TEXTURE state=%u slot=%u fetch=%u "
          "words=%08X,%08X,%08X,%08X,%08X,%08X valid=%u "
          "guest=0x%08X+0x%X guest_extent=%ux%ux%u guest_format=%u "
          "dimension=%u tiled=%u scaled=%u outdated=0x%X "
          "resource=0x%016llX host_extent=%llux%ux%u mips=%u "
          "host_format=%u descriptor=%u signed_descriptor=%u\n",
          i, fetch_index, state.texture_fetch_indices[fetch_index],
          state.texture_fetch_words[fetch_index][0],
          state.texture_fetch_words[fetch_index][1],
          state.texture_fetch_words[fetch_index][2],
          state.texture_fetch_words[fetch_index][3],
          state.texture_fetch_words[fetch_index][4],
          state.texture_fetch_words[fetch_index][5],
          state.texture_states[fetch_index].valid,
          state.texture_states[fetch_index].guest_base,
          state.texture_states[fetch_index].guest_size,
          state.texture_states[fetch_index].guest_width,
          state.texture_states[fetch_index].guest_height,
          state.texture_states[fetch_index].guest_depth_or_array_size,
          state.texture_states[fetch_index].guest_format,
          state.texture_states[fetch_index].guest_dimension,
          state.texture_states[fetch_index].guest_tiled,
          state.texture_states[fetch_index].scaled_resolve,
          state.texture_states[fetch_index].outdated_mask,
          static_cast<unsigned long long>(
              state.texture_states[fetch_index].resource_identity),
          static_cast<unsigned long long>(
              state.texture_states[fetch_index].resource_width),
          state.texture_states[fetch_index].resource_height,
          state.texture_states[fetch_index].resource_depth_or_array_size,
          state.texture_states[fetch_index].resource_mip_levels,
          state.texture_states[fetch_index].resource_format,
          state.texture_states[fetch_index].descriptor_index,
          state.texture_states[fetch_index].descriptor_index_signed);
    }
    for (uint32_t fetch_index = 0; fetch_index < state.vertex_fetch_count;
         ++fetch_index) {
      std::fprintf(file,
                   "VERTEX state=%u slot=%u fetch=%u words=%08X,%08X\n",
                   i, fetch_index, state.vertex_fetch_indices[fetch_index],
                   state.vertex_fetch_words[fetch_index][0],
                   state.vertex_fetch_words[fetch_index][1]);
    }
  }
  for (uint32_t i = 0; i < embedded_frame_frontier.resolve_state_count; ++i) {
    const EmbeddedFrameFrontier::ResolveState& state =
        embedded_frame_frontier.resolve_states[i];
    std::fprintf(
        file,
        "RESOLVE state=%u control=0x%08X dest_base=0x%08X dest_info=0x%08X "
        "dest_pitch=0x%08X surface=0x%08X source_color=0x%08X depth_info=0x%08X "
        "written=0x%08X bytes=%u written_scaled=%u\n",
        i, state.control, state.destination_base, state.destination_info,
        state.destination_pitch, state.surface_info, state.source_color_info,
        state.depth_info, state.written_address, state.written_length,
        state.written_scaled);
  }
  if (REXCVAR_GET(embedded_camera_draw_capture) &&
      embedded_camera_draw_budget.frame == swap_ordinal) {
    std::fprintf(file,
        "CAMERA_CAPTURE version=1 frame=%llu generation=%llu draws=%u dropped=%llu "
        "source=bound_vertex_float_upload scope=submitted_draw_constants_not_camera_history\n",
        static_cast<unsigned long long>(swap_ordinal),
        static_cast<unsigned long long>(manual_generation),
        uint32_t(embedded_camera_draws.size()),
        static_cast<unsigned long long>(embedded_camera_draw_budget.dropped));
    for (const auto& draw : embedded_camera_draws) {
      std::fprintf(file,
          "CAMERA_DRAW draw=%llu vs=%016llX ps=%016llX cbuffer=%016llX "
          "surface=%08X depth=%08X depth_control=%08X offset=%08X scissor=%08X,%08X "
          "viewport=%08X,%08X,%08X,%08X,%08X,%08X vte=%08X clip=%08X "
          "scale=%ux%u valid=%u count=%u map=%016llX,%016llX,%016llX,%016llX words=",
          static_cast<unsigned long long>(draw.draw),
          static_cast<unsigned long long>(draw.vertex_shader),
          static_cast<unsigned long long>(draw.pixel_shader),
          static_cast<unsigned long long>(draw.constant_buffer),
          draw.surface, draw.depth, draw.depth_control, draw.window_offset,
          draw.scissor_tl, draw.scissor_br, draw.viewport[0], draw.viewport[1],
          draw.viewport[2], draw.viewport[3], draw.viewport[4], draw.viewport[5],
          draw.vte, draw.clip, draw.scale_x, draw.scale_y,
          draw.constants.valid ? 1u : 0u, draw.constants.count,
          static_cast<unsigned long long>(draw.constants.map[0]),
          static_cast<unsigned long long>(draw.constants.map[1]),
          static_cast<unsigned long long>(draw.constants.map[2]),
          static_cast<unsigned long long>(draw.constants.map[3]));
      for (uint32_t i = 0; i < draw.constants.count * 4; ++i) {
        std::fprintf(file, "%s%08X", i ? "," : "", draw.constants.words[i]);
      }
      std::fputc('\n', file);
      std::fprintf(file,
          "CAMERA_RASTER draw=%llu raster=%08X vertex_control=%08X "
          "host_viewport=%08X,%08X,%08X,%08X,%08X,%08X "
          "host_ndc=%08X,%08X,%08X,%08X,%08X,%08X host_scissor=%d,%d,%d,%d\n",
          static_cast<unsigned long long>(draw.draw), draw.raster, draw.vertex_control,
          draw.host_viewport[0], draw.host_viewport[1], draw.host_viewport[2],
          draw.host_viewport[3], draw.host_viewport[4], draw.host_viewport[5],
          draw.host_ndc[0], draw.host_ndc[1], draw.host_ndc[2],
          draw.host_ndc[3], draw.host_ndc[4], draw.host_ndc[5],
          draw.host_scissor[0], draw.host_scissor[1], draw.host_scissor[2], draw.host_scissor[3]);
      if (REXCVAR_GET(embedded_camera_scene_capture)) {
        std::fprintf(file,
            "CAMERA_PIXEL draw=%llu ps=%016llX cbuffer=%016llX valid=%u count=%u "
            "map=%016llX,%016llX,%016llX,%016llX words=",
            static_cast<unsigned long long>(draw.draw), static_cast<unsigned long long>(draw.pixel_shader),
            static_cast<unsigned long long>(draw.pixel_constant_buffer),
            draw.pixel_constants.valid ? 1u : 0u, draw.pixel_constants.count,
            static_cast<unsigned long long>(draw.pixel_constants.map[0]),
            static_cast<unsigned long long>(draw.pixel_constants.map[1]),
            static_cast<unsigned long long>(draw.pixel_constants.map[2]),
            static_cast<unsigned long long>(draw.pixel_constants.map[3]));
        for (uint32_t i = 0; i < draw.pixel_constants.count * 4; ++i)
          std::fprintf(file, "%s%08X", i ? "," : "", draw.pixel_constants.words[i]);
        std::fputc('\n', file);
        std::fprintf(file,
            "CAMERA_TARGET draw=%llu color=%08X,%08X,%08X,%08X mask=%08X "
            "color_control=%08X blend=%08X,%08X,%08X,%08X bound_bits=%X "
            "textures=%08X primitive=%u vertices=%u\n",
            static_cast<unsigned long long>(draw.draw), draw.target_color[0], draw.target_color[1],
            draw.target_color[2], draw.target_color[3], draw.target_mask, draw.target_control,
            draw.target_blend[0], draw.target_blend[1], draw.target_blend[2], draw.target_blend[3],
            draw.target_bits, draw.textures, draw.primitive, draw.vertices);
      }
    }
    if (REXCVAR_GET(embedded_camera_scene_capture)) {
      std::fprintf(file,
          "CAMERA_SCENE_CAPTURE version=1 frame=%llu draws=%u resolve_events=%u "
          "dropped_resolves=%u copies=%u motion_copies=%u color_copies=%u "
          "source=bound_pixel_float_upload scope=scene_motion_epochs_not_temporal_history\n",
          static_cast<unsigned long long>(swap_ordinal), uint32_t(embedded_camera_draws.size()),
          embedded_scene_resolve_budget.events, embedded_scene_resolve_budget.dropped,
          embedded_scene_resolve_budget.copies, embedded_scene_resolve_budget.motion_copies,
          embedded_scene_resolve_budget.color_copies);
      embedded_scene_resolve_budget.closed = true;
      std::fprintf(file,
          "CAMERA_SCENE_UPDATES frame=%llu updates=%u dropped_updates=%u records=%u dropped_records=%u "
          "scope=ownership_and_queued_transfers_not_pixel_validity\n",
          static_cast<unsigned long long>(swap_ordinal), embedded_scene_update_budget.updates,
          embedded_scene_update_budget.dropped_updates, embedded_scene_update_budget.records,
          embedded_scene_update_budget.dropped_records);
      embedded_scene_update_budget.closed = true;
    }
    embedded_camera_draws.clear();
    embedded_camera_draw_budget.closed = true;
    if (REXCVAR_GET(embedded_camera_depth_clear_capture)) {
      std::fprintf(file,
          "CAMERA_DEPTH_CLEAR_CAPTURE frame=%llu pairs=%u attempts=%u missed=%u pending=%llu "
          "scope=clear_and_immediate_alias_samples_not_camera_history\n",
          static_cast<unsigned long long>(swap_ordinal), embedded_camera_depth_clear_budget.pairs,
          embedded_camera_depth_clear_budget.attempts, embedded_camera_depth_clear_budget.missed,
          static_cast<unsigned long long>(embedded_camera_depth_clear_budget.pending_clear));
      embedded_camera_depth_clear_budget.closed = true;
    }
    if (REXCVAR_GET(embedded_camera_geometry_capture)) {
      std::fprintf(file,
          "CAMERA_GEOMETRY_CAPTURE frame=%llu attempts=%u dropped=%u scope=selected_gpu_geometry_not_camera_history\n",
          static_cast<unsigned long long>(swap_ordinal),
          embedded_camera_geometry_budget.attempts, embedded_camera_geometry_budget.dropped);
      embedded_camera_geometry_budget.closed = true;
    }
  }
  const bool write_succeeded = std::fclose(file) == 0;
  std::fprintf(
      stderr,
      "REX_EMBEDDED_GAMEPLAY_CAPTURE result=%s swap=%llu path=%s draw_states=%u "
      "dropped_draw_states=%u resolve_states=%u dropped_resolve_states=%u\n",
      write_succeeded ? "ok" : "write_failed",
      static_cast<unsigned long long>(swap_ordinal), path,
      embedded_frame_frontier.draw_state_count,
      embedded_frame_frontier.dropped_draw_states,
      embedded_frame_frontier.resolve_state_count,
      embedded_frame_frontier.dropped_resolve_states);
  std::fflush(stderr);
}

// One bounded asynchronous capture used to determine whether a complex frame
// is already black in the swap texture or becomes black later in presentation.
// It is observational only and never changes guest memory or register state.
struct EmbeddedSwapReadback {
  Microsoft::WRL::ComPtr<ID3D12Resource> source_resource;
  Microsoft::WRL::ComPtr<ID3D12Resource> output_resource;
  uint64_t submission = 0;
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT source_footprint{};
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT output_footprint{};
  UINT source_rows = 0;
  UINT output_rows = 0;
  UINT64 source_row_size = 0;
  UINT64 output_row_size = 0;
  UINT64 source_size = 0;
  UINT64 output_size = 0;
  uint32_t frontbuffer = 0;
  uint64_t frame_state_hash = 0;
  bool attempted = false;
};

EmbeddedSwapReadback embedded_swap_readback;

// Generated with `xb buildshaders`.
namespace shaders {
#include "../shaders/bytecode/d3d12_5_1/apply_gamma_pwl_cs.h"
#include "../shaders/bytecode/d3d12_5_1/apply_gamma_pwl_fxaa_luma_cs.h"
#include "../shaders/bytecode/d3d12_5_1/apply_gamma_table_cs.h"
#include "../shaders/bytecode/d3d12_5_1/apply_gamma_table_fxaa_luma_cs.h"
#include "../shaders/bytecode/d3d12_5_1/fxaa_cs.h"
#include "../shaders/bytecode/d3d12_5_1/fxaa_extreme_cs.h"
#include "../shaders/bytecode/d3d12_5_1/resolve_downscale_cs.h"
// scripts/build-smaa-shaders.ps1 (FXC) from shaders/smaa.cs.hlsl.
#include "../shaders/bytecode/d3d12_5_1/smaa_blending_weight_cs.h"
#include "../shaders/bytecode/d3d12_5_1/smaa_edge_detection_cs.h"
#include "../shaders/bytecode/d3d12_5_1/smaa_neighborhood_blending_cs.h"
}  // namespace shaders

// SMAA's precomputed lookup tables (MIT, shaders/smaa/LICENSE.txt).
namespace smaa_tables {
#include "../shaders/smaa/AreaTex.h"
#include "../shaders/smaa/SearchTex.h"
}  // namespace smaa_tables

struct D3D12CommandProcessor::GpuTimingState {
  // Timestamps per frame slot (one slot per frame in flight).
  static constexpr uint32_t kCapacity = 32768;
  // Entries kept free for submission ends and starts once marks overflow.
  static constexpr uint32_t kReserve = 64;
  Microsoft::WRL::ComPtr<ID3D12QueryHeap> heap;
  Microsoft::WRL::ComPtr<ID3D12Resource> readback;
  const uint64_t* mapping = nullptr;
  uint64_t frequency = 0;
  int64_t qpc_frequency = 0;
  std::array<std::array<uint8_t, kCapacity>, kQueueFrames> categories;
  std::array<uint32_t, kQueueFrames> slot_count{};
  std::array<uint32_t, kQueueFrames> slot_overflow{};
  std::array<uint64_t, kQueueFrames> slot_frame{};
  uint64_t last_collected_frame = UINT64_MAX - 1;
  uint32_t slot = 0;
  uint32_t count = 0;
  uint32_t resolved = 0;
  uint32_t overflow = 0;
  GpuTimingCategory category = GpuTimingCategory::kOther;
  GpuTimingCategory resume_category = GpuTimingCategory::kOther;
  bool frame_active = false;
  uint64_t previous_last = 0;
  bool have_previous = false;
  GpuTimingWindow window;
  int64_t window_begin_qpc = 0;
  // Recorded-work counters of the frames closed in the current window.
  std::array<uint64_t, size_t(GpuTimingCounter::kCount)> counters{};
  uint64_t recorded_frames = 0;
  uint32_t log_interval = 0;
  bool log_frame = false;
  // d3d12_gpu_timing_passes: the render-target set of each draw interval.
  struct PassInfo {
    uint64_t signature = 0;
    uint32_t keys[1 + xenos::kMaxColorRenderTargets] = {};
    uint64_t vertex_shader = 0;
    uint64_t pixel_shader = 0;
    uint64_t draws = 0;
  };
  struct PassTotal {
    PassInfo info;
    uint64_t ticks = 0;
    uint64_t draws = 0;
    uint64_t intervals = 0;
  };
  bool passes_enabled = false;
  // Per timestamp: index into the slot's passes of the draw interval it closes.
  std::array<std::array<uint16_t, kCapacity>, kQueueFrames> pass_of{};
  std::array<std::vector<PassInfo>, kQueueFrames> slot_passes;  // [0] = no pass
  uint16_t pass = 0;
  uint32_t pass_keys[1 + xenos::kMaxColorRenderTargets] = {};  // render targets bound now
  std::unordered_map<uint64_t, PassTotal> pass_totals;  // current window
};

// d3d12_gpu_frame_meter: a timestamp when each submission of a frame opens
// and when it closes; the sum of the pairs is the frame's GPU busy time (idle
// time between submissions is not counted). Read when the frame slot is
// reused, after its fence completed.
struct D3D12CommandProcessor::GpuFrameMeter {
  // Submissions split every d3d12_submit_after_draws (512) draws: a street
  // frame has ~15, so this keeps room for the heaviest scenes.
  static constexpr uint32_t kPairsPerFrame = 64;
  static constexpr size_t kHistory = 512;
  Microsoft::WRL::ComPtr<ID3D12QueryHeap> heap;
  Microsoft::WRL::ComPtr<ID3D12Resource> readback;
  const uint64_t* mapping = nullptr;
  uint64_t frequency = 0;
  std::array<uint32_t, kQueueFrames> slot_pairs{};
  std::array<bool, kQueueFrames> slot_valid{};
  uint32_t slot = 0;
  uint32_t pairs = 0;
  bool pair_open = false;
  bool frame_active = false;
  bool truncation_logged = false;
  mutable std::mutex mutex;
  std::array<uint32_t, kHistory> history{};
  uint64_t history_count = 0;
};

D3D12CommandProcessor::D3D12CommandProcessor(D3D12GraphicsSystem* graphics_system,
                                             system::KernelState* kernel_state)
    : CommandProcessor(graphics_system, kernel_state), deferred_command_list_(*this) {
  legacy_readback_memexport_cvar_name_ = "d3d12_readback_memexport";
}
D3D12CommandProcessor::~D3D12CommandProcessor() { StopSubmissionThread(); }

void D3D12CommandProcessor::InitializeGpuTiming() {
  gpu_timing_.reset();
  if (!REXCVAR_GET(d3d12_gpu_timing)) {
    return;
  }
#if !REX_GPU_DIAGNOSTICS
  // Player build: the diagnostic is compiled out (REXGLUE_GPU_DIAGNOSTICS).
  std::fprintf(stderr, "REX_GPU_TIMING_POLICY enabled=0 reason=not_compiled_in_player_build\n");
  std::fflush(stderr);
  return;
#endif
  auto state = std::make_unique<GpuTimingState>();
  const ui::d3d12::D3D12Provider& provider = GetD3D12Provider();
  ID3D12Device* device = provider.GetDevice();
  ID3D12CommandQueue* queue = provider.GetDirectQueue();
  LARGE_INTEGER qpc_frequency;
  QueryPerformanceFrequency(&qpc_frequency);
  state->qpc_frequency = qpc_frequency.QuadPart;
  state->log_interval = REXCVAR_GET(d3d12_gpu_timing_transfer_log_interval);
  state->passes_enabled = REXCVAR_GET(d3d12_gpu_timing_passes);
  const uint32_t total = GpuTimingState::kCapacity * kQueueFrames;
  bool ok = device && queue && SUCCEEDED(queue->GetTimestampFrequency(&state->frequency)) &&
            state->frequency;
  if (ok) {
    D3D12_QUERY_HEAP_DESC heap_desc = {};
    heap_desc.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    heap_desc.Count = total;
    ok = SUCCEEDED(device->CreateQueryHeap(&heap_desc, IID_PPV_ARGS(&state->heap)));
  }
  if (ok) {
    D3D12_RESOURCE_DESC buffer_desc;
    ui::d3d12::util::FillBufferResourceDesc(buffer_desc, sizeof(uint64_t) * total,
                                            D3D12_RESOURCE_FLAG_NONE);
    ok = SUCCEEDED(device->CreateCommittedResource(
        &ui::d3d12::util::kHeapPropertiesReadback, provider.GetHeapFlagCreateNotZeroed(),
        &buffer_desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&state->readback)));
  }
  if (ok) {
    D3D12_RANGE read_range = {0, sizeof(uint64_t) * total};
    void* mapping = nullptr;
    ok = SUCCEEDED(state->readback->Map(0, &read_range, &mapping)) && mapping;
    state->mapping = static_cast<const uint64_t*>(mapping);
  }
  std::fprintf(stderr, "REX_GPU_TIMING_POLICY enabled=%u frequency=%llu capacity=%u\n",
               ok ? 1u : 0u, static_cast<unsigned long long>(state->frequency),
               GpuTimingState::kCapacity);
  std::fflush(stderr);
  if (ok) {
    gpu_timing_ = std::move(state);
  }
}

void D3D12CommandProcessor::InitializeGpuFrameMeter() {
  gpu_frame_meter_.reset();
  if (!REXCVAR_GET(d3d12_gpu_frame_meter)) {
    return;
  }
  auto meter = std::make_unique<GpuFrameMeter>();
  const ui::d3d12::D3D12Provider& provider = GetD3D12Provider();
  ID3D12Device* device = provider.GetDevice();
  ID3D12CommandQueue* queue = provider.GetDirectQueue();
  const uint32_t total = GpuFrameMeter::kPairsPerFrame * 2 * kQueueFrames;
  bool ok = device && queue && SUCCEEDED(queue->GetTimestampFrequency(&meter->frequency)) &&
            meter->frequency;
  if (ok) {
    D3D12_QUERY_HEAP_DESC heap_desc = {};
    heap_desc.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    heap_desc.Count = total;
    ok = SUCCEEDED(device->CreateQueryHeap(&heap_desc, IID_PPV_ARGS(&meter->heap)));
  }
  if (ok) {
    D3D12_RESOURCE_DESC buffer_desc;
    ui::d3d12::util::FillBufferResourceDesc(buffer_desc, sizeof(uint64_t) * total,
                                            D3D12_RESOURCE_FLAG_NONE);
    ok = SUCCEEDED(device->CreateCommittedResource(
        &ui::d3d12::util::kHeapPropertiesReadback, provider.GetHeapFlagCreateNotZeroed(),
        &buffer_desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&meter->readback)));
  }
  if (ok) {
    D3D12_RANGE read_range = {0, sizeof(uint64_t) * total};
    void* mapping = nullptr;
    ok = SUCCEEDED(meter->readback->Map(0, &read_range, &mapping)) && mapping;
    meter->mapping = static_cast<const uint64_t*>(mapping);
  }
  if (ok) {
    gpu_frame_meter_ = std::move(meter);
  } else {
    REXGPU_WARN("D3D12CommandProcessor: GPU frame meter unavailable");
  }
}

void D3D12CommandProcessor::GpuFrameMeterBeginSubmission(bool submission_opened,
                                                         bool is_opening_frame) {
  GpuFrameMeter& meter = *gpu_frame_meter_;
  if (is_opening_frame) {
    // The fence wait for this frame slot completed the frame that used it.
    const uint32_t slot = uint32_t(frame_current_ % kQueueFrames);
    if (meter.slot_valid[slot]) {
      const uint64_t* stamps =
          meter.mapping + size_t(slot) * GpuFrameMeter::kPairsPerFrame * 2;
      uint64_t busy = 0;
      for (uint32_t i = 0; i < meter.slot_pairs[slot]; ++i) {
        if (stamps[2 * i + 1] > stamps[2 * i]) {
          busy += stamps[2 * i + 1] - stamps[2 * i];
        }
      }
      const uint64_t busy_us = busy * 1000000 / meter.frequency;
      std::lock_guard<std::mutex> lock(meter.mutex);
      meter.history[meter.history_count % GpuFrameMeter::kHistory] =
          uint32_t(std::min(busy_us, uint64_t(UINT32_MAX)));
      ++meter.history_count;
    }
    meter.slot_valid[slot] = false;
    meter.slot = slot;
    meter.pairs = 0;
    meter.pair_open = false;
    meter.frame_active = true;
  }
  if ((submission_opened || is_opening_frame) && meter.frame_active && !meter.pair_open) {
    if (meter.pairs < GpuFrameMeter::kPairsPerFrame) {
      deferred_command_list_.D3DEndQuery(
          meter.heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP,
          (meter.slot * GpuFrameMeter::kPairsPerFrame + meter.pairs) * 2);
      meter.pair_open = true;
    } else if (!meter.truncation_logged) {
      // The frame's remaining submissions are not measured (undercount).
      meter.truncation_logged = true;
      std::fprintf(stderr, "REX_GPU_FRAME_METER truncated=1 pairs_per_frame=%u\n",
                   GpuFrameMeter::kPairsPerFrame);
      std::fflush(stderr);
    }
  }
}

void D3D12CommandProcessor::GpuFrameMeterEndSubmission() {
  GpuFrameMeter& meter = *gpu_frame_meter_;
  if (!meter.pair_open) {
    return;
  }
  const uint32_t first = (meter.slot * GpuFrameMeter::kPairsPerFrame + meter.pairs) * 2;
  deferred_command_list_.D3DEndQuery(meter.heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, first + 1);
  deferred_command_list_.D3DResolveQueryData(meter.heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, first,
                                             2, meter.readback.Get(),
                                             uint64_t(first) * sizeof(uint64_t));
  meter.pair_open = false;
  ++meter.pairs;
}

void D3D12CommandProcessor::GpuFrameMeterCloseFrame() {
  GpuFrameMeter& meter = *gpu_frame_meter_;
  if (!meter.frame_active) {
    return;
  }
  // The frame's submissions are closed (and their pairs resolved) by now.
  meter.slot_pairs[meter.slot] = meter.pairs;
  meter.slot_valid[meter.slot] = !meter.pair_open;
  meter.frame_active = false;
}

size_t D3D12CommandProcessor::GetRecentGpuFrameBusyUs(uint32_t* out, size_t capacity,
                                                      uint64_t* total_frames_out) const {
  if (total_frames_out) {
    *total_frames_out = 0;
  }
  if (!gpu_frame_meter_) {
    return 0;
  }
  const GpuFrameMeter& meter = *gpu_frame_meter_;
  std::lock_guard<std::mutex> lock(meter.mutex);
  if (total_frames_out) {
    *total_frames_out = meter.history_count;
  }
  if (!out) {
    return 0;
  }
  const size_t count = size_t(std::min<uint64_t>(
      std::min<uint64_t>(capacity, meter.history_count), GpuFrameMeter::kHistory));
  for (size_t i = 0; i < count; ++i) {
    out[i] = meter.history[(meter.history_count - count + i) % GpuFrameMeter::kHistory];
  }
  return count;
}

#if REX_GPU_DIAGNOSTICS
void D3D12CommandProcessor::GpuTimingWrite(bool boundary) {
  GpuTimingState& timing = *gpu_timing_;
  if (timing.count + (boundary ? 0 : GpuTimingState::kReserve) >= GpuTimingState::kCapacity) {
    ++timing.overflow;
    return;
  }
  timing.categories[timing.slot][timing.count] = uint8_t(timing.category);
  timing.pass_of[timing.slot][timing.count] =
      timing.category == GpuTimingCategory::kDraw ? timing.pass : 0;
  deferred_command_list_.D3DEndQuery(timing.heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP,
                                     timing.slot * GpuTimingState::kCapacity + timing.count);
  ++timing.count;
}

void D3D12CommandProcessor::GpuTimingNotePass(const uint32_t* render_target_keys) {
  GpuTimingState& timing = *gpu_timing_;
  if (!timing.passes_enabled) {
    return;
  }
  // The next draw starts a pass with these render targets (see
  // GpuTimingDrawPass, which also splits by shader pair).
  std::memcpy(timing.pass_keys, render_target_keys, sizeof(timing.pass_keys));
}

void D3D12CommandProcessor::GpuTimingDrawPass() {
  GpuTimingState& timing = *gpu_timing_;
  if (!timing.frame_active) {
    return;
  }
  const Shader* vertex_shader = active_vertex_shader();
  const Shader* pixel_shader = active_pixel_shader();
  const uint64_t vertex_hash = vertex_shader ? vertex_shader->ucode_data_hash() : 0;
  const uint64_t pixel_hash = pixel_shader ? pixel_shader->ucode_data_hash() : 0;
  uint64_t signature = 1469598103934665603ull;
  for (uint32_t i = 0; i < 1 + xenos::kMaxColorRenderTargets; ++i) {
    signature = (signature ^ timing.pass_keys[i]) * 1099511628211ull;
  }
  signature = (signature ^ vertex_hash) * 1099511628211ull;
  signature = (signature ^ pixel_hash) * 1099511628211ull;
  std::vector<GpuTimingState::PassInfo>& passes = timing.slot_passes[timing.slot];
  if (passes.empty()) {
    passes.emplace_back();
  }
  if (!timing.pass || passes[timing.pass].signature != signature) {
    // Close the previous pass's draw interval.
    if (timing.category == GpuTimingCategory::kDraw && submission_open_) {
      GpuTimingWrite();
    }
    if (passes.size() >= UINT16_MAX) {
      timing.pass = 0;
      return;
    }
    GpuTimingState::PassInfo info;
    info.signature = signature;
    std::memcpy(info.keys, timing.pass_keys, sizeof(info.keys));
    info.vertex_shader = vertex_hash;
    info.pixel_shader = pixel_hash;
    passes.push_back(info);
    timing.pass = uint16_t(passes.size() - 1);
  }
  ++passes[timing.pass].draws;
}

GpuTimingCategory D3D12CommandProcessor::GpuTimingSwitch(GpuTimingCategory category) {
  GpuTimingState& timing = *gpu_timing_;
  if (category == GpuTimingCategory::kDraw && timing.passes_enabled) {
    // Called once per host draw (GpuTimingMark right before it).
    GpuTimingDrawPass();
  }
  const GpuTimingCategory previous = timing.category;
  if (category != previous) {
    if (submission_open_ && timing.frame_active) {
      GpuTimingWrite();
    }
    timing.category = category;
  }
  return previous;
}

void D3D12CommandProcessor::GpuTimingBeginSubmission(bool submission_opened,
                                                     bool is_opening_frame) {
  GpuTimingState& timing = *gpu_timing_;
  if (is_opening_frame) {
    // The fence wait for this frame slot completed the frame that used it.
    const uint32_t slot = uint32_t(frame_current_ % kQueueFrames);
    if (timing.slot_count[slot]) {
      // Chain the first interval only to the directly preceding frame.
      if (timing.slot_frame[slot] != timing.last_collected_frame + 1) {
        timing.have_previous = false;
      }
      timing.last_collected_frame = timing.slot_frame[slot];
      if (timing.passes_enabled) {
        // Draw intervals by render-target set (entry 0 closes the gap before
        // the frame; later entries are consecutive in GPU time).
        const uint64_t* stamps = timing.mapping + size_t(slot) * GpuTimingState::kCapacity;
        const std::vector<GpuTimingState::PassInfo>& passes = timing.slot_passes[slot];
        const auto total_of = [&](const GpuTimingState::PassInfo& info) -> GpuTimingState::PassTotal& {
          GpuTimingState::PassTotal& total = timing.pass_totals[info.signature];
          if (!total.info.signature) total.info = info;
          return total;
        };
        for (uint32_t i = 1; i < timing.slot_count[slot]; ++i) {
          const uint16_t pass = timing.pass_of[slot][i];
          if (!pass || pass >= passes.size() || stamps[i] < stamps[i - 1]) continue;
          GpuTimingState::PassTotal& total = total_of(passes[pass]);
          total.ticks += stamps[i] - stamps[i - 1];
          ++total.intervals;
        }
        for (size_t pass = 1; pass < passes.size(); ++pass) {
          total_of(passes[pass]).draws += passes[pass].draws;
        }
      }
      timing.window.AddFrame(timing.mapping + size_t(slot) * GpuTimingState::kCapacity,
                             timing.categories[slot].data(), timing.slot_count[slot],
                             timing.slot_overflow[slot], timing.previous_last,
                             timing.have_previous);
      timing.slot_count[slot] = 0;
      LARGE_INTEGER now;
      QueryPerformanceCounter(&now);
      if (!timing.window_begin_qpc) {
        timing.window_begin_qpc = now.QuadPart;
      } else if (now.QuadPart - timing.window_begin_qpc >= timing.qpc_frequency / 2) {
        const GpuTimingWindow& w = timing.window;
        const double to_us = 1000000.0 / double(timing.frequency);
        std::fprintf(stderr,
                     "REX_GPU_TIMING version=1 qpc_begin=%lld qpc_end=%lld frames=%llu "
                     "period_us=%.0f busy_us=%.0f max_period_us=%.0f max_busy_us=%.0f "
                     "marks=%llu overflow=%llu non_monotonic=%llu",
                     static_cast<long long>(timing.window_begin_qpc),
                     static_cast<long long>(now.QuadPart),
                     static_cast<unsigned long long>(w.frames), double(w.period_ticks) * to_us,
                     double(w.busy_ticks) * to_us, double(w.max_period_ticks) * to_us,
                     double(w.max_busy_ticks) * to_us, static_cast<unsigned long long>(w.marks),
                     static_cast<unsigned long long>(w.overflow),
                     static_cast<unsigned long long>(w.non_monotonic));
        for (size_t i = 0; i < size_t(GpuTimingCategory::kCount); ++i) {
          std::fprintf(stderr, " %s_us=%.0f", GpuTimingCategoryName(GpuTimingCategory(i)),
                       double(w.category_ticks[i]) * to_us);
        }
        std::fprintf(stderr, " recorded_frames=%llu",
                     static_cast<unsigned long long>(timing.recorded_frames));
        for (size_t i = 0; i < size_t(GpuTimingCounter::kCount); ++i) {
          std::fprintf(stderr, " %s=%llu", GpuTimingCounterName(GpuTimingCounter(i)),
                       static_cast<unsigned long long>(timing.counters[i]));
        }
        std::fputc('\n', stderr);
        if (timing.passes_enabled && w.frames) {
          std::vector<const GpuTimingState::PassTotal*> ranked;
          ranked.reserve(timing.pass_totals.size());
          uint64_t draw_ticks = 0;
          for (const auto& entry : timing.pass_totals) {
            ranked.push_back(&entry.second);
            draw_ticks += entry.second.ticks;
          }
          std::sort(ranked.begin(), ranked.end(),
                    [](const GpuTimingState::PassTotal* a, const GpuTimingState::PassTotal* b) {
                      return a->ticks > b->ticks;
                    });
          for (size_t rank = 0; rank < ranked.size() && rank < 40; ++rank) {
            const GpuTimingState::PassTotal& total = *ranked[rank];
            const uint32_t* keys = total.info.keys;
            std::fprintf(
                stderr,
                "REX_GPU_PASS rank=%zu us_per_frame=%.1f share_of_draw=%.1f draws_per_frame=%.1f "
                "intervals_per_frame=%.1f depth=%08X color=%08X,%08X,%08X,%08X vs=%016llX "
                "ps=%016llX\n",
                rank + 1, double(total.ticks) * to_us / double(w.frames),
                draw_ticks ? 100.0 * double(total.ticks) / double(draw_ticks) : 0.0,
                double(total.draws) / double(w.frames), double(total.intervals) / double(w.frames),
                keys[0], keys[1], keys[2], keys[3], keys[4],
                static_cast<unsigned long long>(total.info.vertex_shader),
                static_cast<unsigned long long>(total.info.pixel_shader));
          }
          timing.pass_totals.clear();
        }
        std::fflush(stderr);
        timing.window.Reset();
        timing.window_begin_qpc = now.QuadPart;
        timing.counters = {};
        timing.recorded_frames = 0;
      }
    }
    timing.slot_passes[slot].clear();
    timing.pass = 0;
    timing.log_frame = timing.log_interval && frame_current_ % timing.log_interval == 0;
    timing.slot = slot;
    timing.slot_frame[slot] = frame_current_;
    timing.count = 0;
    timing.resolved = 0;
    timing.overflow = 0;
    timing.frame_active = true;
    // The first timestamp closes the time since the previous frame's last.
    timing.category = GpuTimingCategory::kGap;
    GpuTimingWrite(true);
    timing.category = GpuTimingCategory::kOther;
    timing.resume_category = GpuTimingCategory::kOther;
    return;
  }
  if (submission_opened && timing.frame_active) {
    timing.category = GpuTimingCategory::kGap;
    GpuTimingWrite(true);
    timing.category = timing.resume_category;
  }
}

void D3D12CommandProcessor::GpuTimingEndSubmission() {
  GpuTimingState& timing = *gpu_timing_;
  if (!timing.frame_active) {
    return;
  }
  timing.resume_category = timing.category;
  GpuTimingWrite(true);
  timing.category = GpuTimingCategory::kGap;
  if (timing.count > timing.resolved) {
    const uint32_t base = timing.slot * GpuTimingState::kCapacity;
    deferred_command_list_.D3DResolveQueryData(
        timing.heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, base + timing.resolved,
        timing.count - timing.resolved, timing.readback.Get(),
        uint64_t(base + timing.resolved) * sizeof(uint64_t));
    timing.resolved = timing.count;
  }
}

void D3D12CommandProcessor::GpuTimingCloseFrame() {
  GpuTimingState& timing = *gpu_timing_;
  if (!timing.frame_active) {
    return;
  }
  timing.slot_count[timing.slot] = timing.resolved;
  timing.slot_overflow[timing.slot] = timing.overflow;
  timing.frame_active = false;
  timing.log_frame = false;
  ++timing.recorded_frames;
}

void D3D12CommandProcessor::GpuTimingAddCount(GpuTimingCounter counter, uint64_t value) {
  GpuTimingState& timing = *gpu_timing_;
  if (timing.frame_active && size_t(counter) < timing.counters.size()) {
    timing.counters[size_t(counter)] += value;
  }
}

bool D3D12CommandProcessor::GpuTimingLogFrame() const {
  return gpu_timing_->frame_active && gpu_timing_->log_frame;
}

#endif  // REX_GPU_DIAGNOSTICS

void D3D12CommandProcessor::UpdateDebugMarkersEnabled() {
  debug_markers_enabled_ = IsGpuDebugMarkersEnabled();
}

void D3D12CommandProcessor::PushDebugMarker(const char* format, ...) {
  if (!debug_markers_enabled_) {
    return;
  }
  char label[256];
  va_list args;
  va_start(args, format);
  vsnprintf(label, sizeof(label), format, args);
  va_end(args);
  deferred_command_list_.BeginDebugMarker(label);
}

void D3D12CommandProcessor::PopDebugMarker() {
  if (!debug_markers_enabled_) {
    return;
  }
  deferred_command_list_.EndDebugMarker();
}

void D3D12CommandProcessor::InsertDebugMarker(const char* format, ...) {
  if (!debug_markers_enabled_) {
    return;
  }
  char label[256];
  va_list args;
  va_start(args, format);
  vsnprintf(label, sizeof(label), format, args);
  va_end(args);
  deferred_command_list_.InsertDebugMarker(label);
}

void D3D12CommandProcessor::ClearCaches() {
  CommandProcessor::ClearCaches();
  InvalidateAllVertexBufferResidency();
  cache_clear_requested_ = true;
}

void D3D12CommandProcessor::InvalidateGpuMemory() {
  if (shared_memory_) {
    shared_memory_->InvalidateAllPages();
  }
}

void D3D12CommandProcessor::InvalidateAllVertexBufferResidency() {
  vertex_buffers_in_sync_[0] = 0;
  vertex_buffers_in_sync_[1] = 0;
  for (VertexBufferState& state : vertex_buffer_states_) {
    state.address = UINT32_MAX;
    state.size = UINT32_MAX;
  }
}

void D3D12CommandProcessor::InvalidateVertexBufferResidency(uint32_t vfetch_index) {
  if (vfetch_index >= vertex_buffer_states_.size()) {
    return;
  }
  vertex_buffers_in_sync_[vfetch_index >> 6] &= ~(uint64_t(1) << (vfetch_index & 63));
}

void D3D12CommandProcessor::InvalidateVertexBufferResidencyRange(uint32_t first_vfetch,
                                                                 uint32_t last_vfetch) {
  if (first_vfetch > last_vfetch) {
    std::swap(first_vfetch, last_vfetch);
  }
  if (first_vfetch >= vertex_buffer_states_.size()) {
    return;
  }
  last_vfetch = std::min(last_vfetch, uint32_t(vertex_buffer_states_.size() - 1));
  for (uint32_t vfetch_index = first_vfetch; vfetch_index <= last_vfetch; ++vfetch_index) {
    InvalidateVertexBufferResidency(vfetch_index);
  }
}

void D3D12CommandProcessor::InitializeShaderStorage(const std::filesystem::path& cache_root,
                                                    uint32_t title_id, bool blocking) {
  CommandProcessor::InitializeShaderStorage(cache_root, title_id, blocking);
  pipeline_cache_->InitializeShaderStorage(cache_root, title_id, blocking);
}

void D3D12CommandProcessor::RequestFrameTrace(const std::filesystem::path& root_path) {
  // Capture with PIX if attached.
  if (GetD3D12Provider().GetGraphicsAnalysis() != nullptr) {
    pix_capture_requested_.store(true, std::memory_order_relaxed);
    return;
  }
  CommandProcessor::RequestFrameTrace(root_path);
}

void D3D12CommandProcessor::TracePlaybackWroteMemory(uint32_t base_ptr, uint32_t length) {
  // In embedded mode the title's CPU threads may write physical memory while
  // the command processor worker is still constructing these caches. There is
  // nothing to invalidate before a cache exists; once constructed, its initial
  // state treats guest pages and primitive residency as uncached.
  if (shared_memory_) {
    if (kGpuDiagnostics && shared_memory_->TextureLifecycleDiagnosticOverlaps(base_ptr, length)) {
      shared_memory_->RecordTextureLifecycleDiagnosticEvent(
          SharedMemory::TextureLifecycleDiagnosticEventType::kPhysicalWriteNotify, base_ptr,
          length);
    }
    shared_memory_->MemoryInvalidationCallback(base_ptr, length, true);
  }
  if (primitive_processor_) {
    primitive_processor_->MemoryInvalidationCallback(base_ptr, length, true);
  }
}

void D3D12CommandProcessor::MarkHostWrite(uint32_t base_ptr, uint32_t length) {
  // Lock-free; published on this command processor's thread by the shared
  // memory before the pages are requested, and at every frame end.
  if (shared_memory_) {
    shared_memory_->MarkHostWrite(base_ptr, length);
  }
}

void D3D12CommandProcessor::RestoreEdramSnapshot(const void* snapshot) {
  // Starting a new frame because descriptors may be needed.
  if (!BeginSubmission(true)) {
    return;
  }
  render_target_cache_->RestoreEdramSnapshot(snapshot);
}

namespace {
std::atomic<void**> renderdoc_api{nullptr};
std::atomic<uint32_t> renderdoc_requested_frames{0};
// Guest swaps left in the capture in progress (command processor thread).
uint32_t renderdoc_capture_frames_left = 0;
}  // namespace

void SetRenderDocApi(void** api) { renderdoc_api.store(api); }

void RequestRenderDocGuestFrames(uint32_t frames) { renderdoc_requested_frames.store(frames); }

#if REX_GPU_DIAGNOSTICS
namespace {
// Measurement builds: REX_ZPD_STATS every 300 guest frames - the occlusion
// report values the guest reads (per slot), their publication lag in guest
// frames, the host segments' draw scale (native vs scaled samples), results
// dropped because the guest reissued their slot first, and the lag bound's
// waits.
struct ZpdDiagStats {
  uint64_t window_frame = 0;
  uint64_t reports = 0, zero_reports = 0, delta_sum = 0, delta_max = 0;
  uint64_t lag_sum = 0, lag_max = 0, queue_max = 0;
  uint64_t segments_native = 0, segments_scaled = 0;
  uint64_t raw_native = 0, raw_scaled = 0, normalized_scaled = 0;
  uint64_t superseded = 0, bound_waits = 0, bound_wait_ticks = 0;
  static constexpr uint32_t kSlots = 12;
  uint32_t slot_base[kSlots] = {};
  uint64_t slot_reports[kSlots] = {};
  uint64_t slot_delta_sum[kSlots] = {};
  uint32_t slot_count = 0;
};
ZpdDiagStats& ZpdDiag() {
  static ZpdDiagStats stats;
  return stats;
}
void ZpdDiagCountSuperseded(uint32_t count) { ZpdDiag().superseded += count; }
}  // namespace
#endif

bool D3D12CommandProcessor::ExecutePacketType3_EVENT_WRITE_ZPD(memory::RingBuffer* reader,
                                                               uint32_t packet, uint32_t count) {
  if (!REXCVAR_GET(occlusion_query_enable) || !occlusion_query_resources_available_) {
    return CommandProcessor::ExecutePacketType3_EVENT_WRITE_ZPD(reader, packet, count);
  }

  assert_true(count == 1);
  uint32_t initiator = reader->ReadAndSwap<uint32_t>();
  WriteRegister(XE_GPU_REG_VGT_EVENT_INITIATOR, initiator & 0x3F);

  const uint32_t sample_count_address =
      register_file_->values[XE_GPU_REG_RB_SAMPLE_COUNT_ADDR];
  const uint32_t record_base = XenosZPDReport::GetRecordBase(sample_count_address);
  auto* sample_counts = record_base
                            ? memory_->TranslatePhysical<
                                  xenos::xe_gpu_depth_sample_counts*>(record_base)
                            : nullptr;
  if (!record_base || !sample_counts) {
    REXGPU_WARN(
        "D3D12CommandProcessor: Invalid ZPD report address 0x{:08X}",
        sample_count_address);
    DisableHostOcclusionQueries();
    return true;
  }

  const bool is_begin_record = XenosZPDReport::IsBeginRecord(sample_count_address);
  const bool is_end_record = XenosZPDReport::IsEndRecord(sample_count_address);

  if (is_begin_record) {
    // Xenos has one current sample-count address. A new BEGIN implicitly ends
    // the preceding logical report if the guest didn't emit its END first.
    if (logical_occlusion_query_.valid) {
      auto* previous_end = memory_->TranslatePhysical<
          xenos::xe_gpu_depth_sample_counts*>(logical_occlusion_query_.end_record);
      if (!EndGuestOcclusionQuery(logical_occlusion_query_.end_record,
                                  previous_end)) {
        DisableHostOcclusionQueries();
        return true;
      }
    }

    // An unpublished result of a prior lifetime of this slot belongs to a
    // query the guest has reissued (Xenos wrote it when the GPU passed its
    // END, before this BEGIN). Publishing it now would put that older END over
    // the new lifetime's pending sentinel, next to the BEGIN zeroed below, and
    // the guest would read it as the new query's result. Drop it instead (no
    // wait either); d3d12_zpd_max_publish_lag_frames keeps results timely so
    // the guest normally reads them before it reissues.
    const uint32_t superseded_reports = occlusion_reports_.SupersedeSlot(
        XenosZPDReport::GetSlotBase(sample_count_address));
#if REX_GPU_DIAGNOSTICS
    ZpdDiagCountSuperseded(superseded_reports);
#else
    (void)superseded_reports;
#endif
    // Acknowledge the BEGIN and remove a stale sentinel from a prior lifetime.
    // A guest-memory write by the command processor: guarded like every other
    // one, so a GPU copy of its page (the records often share pages with
    // command-buffer vertex data) is invalidated.
    {
      auto host_write = memory_->GuardPhysicalWrite(record_base, sizeof(*sample_counts));
      std::memset(sample_counts, 0, sizeof(*sample_counts));
    }
    // The new lifetime's END stays pending until it is published here. The
    // guest wrote the pending sentinel into the END record when it issued the
    // query, but runs ahead of this command processor: a result of the slot's
    // previous lifetime published after that write (and before this BEGIN)
    // covered it, and the guest then read that older END against this zeroed
    // BEGIN as soon as the new query's fence passed - a huge count. The
    // Darkness's auto-exposure queries (a ring of 5 slots) showed it as
    // over-exposed yellow frames whenever this processor fell a little behind.
    if (auto* end_counts = memory_->TranslatePhysical<xenos::xe_gpu_depth_sample_counts*>(
            XenosZPDReport::GetEndRecordBase(sample_count_address))) {
      auto host_write = memory_->GuardPhysicalWrite(
          XenosZPDReport::GetEndRecordBase(sample_count_address), sizeof(*end_counts));
      XenosZPDReport::WritePendingSentinel(end_counts);
    }
    if (!BeginGuestOcclusionQuery(sample_count_address)) {
      DisableHostOcclusionQueries();
    }
    return true;
  }

  if (is_end_record && logical_occlusion_query_.valid) {
    if (!EndGuestOcclusionQuery(sample_count_address, sample_counts)) {
      DisableHostOcclusionQueries();
    }
    return true;
  }

  // The record layout is authoritative. The pending sentinel is only a hint;
  // using it alone loses QueryBatch-style pairs and same-slot reuse ordering.
  static uint32_t unexpected_zpd_records_logged = 0;
  if (unexpected_zpd_records_logged < 16) {
    REXGPU_WARN(
        "D3D12CommandProcessor: Ignoring {} ZPD record at 0x{:08X} "
        "without a matching logical report (pending={})",
        is_end_record ? "orphaned END" : "non-standard",
        sample_count_address, XenosZPDReport::HasPendingSentinel(sample_counts));
    ++unexpected_zpd_records_logged;
  }
  return true;
}

bool D3D12CommandProcessor::PushTransitionBarrier(ID3D12Resource* resource,
                                                  D3D12_RESOURCE_STATES old_state,
                                                  D3D12_RESOURCE_STATES new_state,
                                                  UINT subresource) {
  if (old_state == new_state) {
    return false;
  }
  D3D12_RESOURCE_BARRIER barrier;
  barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
  barrier.Transition.pResource = resource;
  barrier.Transition.Subresource = subresource;
  barrier.Transition.StateBefore = old_state;
  barrier.Transition.StateAfter = new_state;
  barriers_.push_back(barrier);
  return true;
}

void D3D12CommandProcessor::PushAliasingBarrier(ID3D12Resource* old_resource,
                                                ID3D12Resource* new_resource) {
  D3D12_RESOURCE_BARRIER barrier;
  barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_ALIASING;
  barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
  barrier.Aliasing.pResourceBefore = old_resource;
  barrier.Aliasing.pResourceAfter = new_resource;
  barriers_.push_back(barrier);
}

void D3D12CommandProcessor::PushUAVBarrier(ID3D12Resource* resource) {
  D3D12_RESOURCE_BARRIER barrier;
  barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
  barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
  barrier.UAV.pResource = resource;
  barriers_.push_back(barrier);
}

void D3D12CommandProcessor::SubmitBarriers() {
  UINT barrier_count = UINT(barriers_.size());
  if (barrier_count != 0) {
#if REX_GPU_DIAGNOSTICS
    if (gpu_timing_) {
      const GpuTimingCategory previous = GpuTimingSwitch(GpuTimingCategory::kBarrier);
      deferred_command_list_.D3DResourceBarrier(barrier_count, barriers_.data());
      GpuTimingSwitch(previous);
      barriers_.clear();
      return;
    }
#endif
    deferred_command_list_.D3DResourceBarrier(barrier_count, barriers_.data());
    barriers_.clear();
  }
}

ID3D12RootSignature* D3D12CommandProcessor::GetRootSignature(const DxbcShader* vertex_shader,
                                                             const DxbcShader* pixel_shader,
                                                             bool tessellated) {
  if (bindless_resources_used_) {
    return tessellated ? root_signature_bindless_ds_ : root_signature_bindless_vs_;
  }

  D3D12_SHADER_VISIBILITY vertex_visibility =
      tessellated ? D3D12_SHADER_VISIBILITY_DOMAIN : D3D12_SHADER_VISIBILITY_VERTEX;

  uint32_t texture_count_vertex =
      uint32_t(vertex_shader->GetTextureBindingsAfterTranslation().size());
  uint32_t sampler_count_vertex =
      uint32_t(vertex_shader->GetSamplerBindingsAfterTranslation().size());
  uint32_t texture_count_pixel =
      pixel_shader ? uint32_t(pixel_shader->GetTextureBindingsAfterTranslation().size()) : 0;
  uint32_t sampler_count_pixel =
      pixel_shader ? uint32_t(pixel_shader->GetSamplerBindingsAfterTranslation().size()) : 0;

  // Better put the pixel texture/sampler in the lower bits probably because it
  // changes often.
  uint32_t index = 0;
  uint32_t index_offset = 0;
  index |= texture_count_pixel << index_offset;
  index_offset += D3D12Shader::kMaxTextureBindingIndexBits;
  index |= sampler_count_pixel << index_offset;
  index_offset += D3D12Shader::kMaxSamplerBindingIndexBits;
  index |= texture_count_vertex << index_offset;
  index_offset += D3D12Shader::kMaxTextureBindingIndexBits;
  index |= sampler_count_vertex << index_offset;
  index_offset += D3D12Shader::kMaxSamplerBindingIndexBits;
  index |= uint32_t(vertex_visibility == D3D12_SHADER_VISIBILITY_DOMAIN) << index_offset;
  ++index_offset;
  assert_true(index_offset <= 32);

  // Try an existing root signature.
  auto it = root_signatures_bindful_.find(index);
  if (it != root_signatures_bindful_.end()) {
    return it->second;
  }

  // Create a new one.
  D3D12_ROOT_SIGNATURE_DESC desc;
  D3D12_ROOT_PARAMETER parameters[kRootParameter_Bindful_Count_Max];
  desc.NumParameters = kRootParameter_Bindful_Count_Base;
  desc.pParameters = parameters;
  desc.NumStaticSamplers = 0;
  desc.pStaticSamplers = nullptr;
  desc.Flags =
      REXCVAR_GET(d3d12_embedded_scene_host_vertex_output_diagnostic)
          ? D3D12_ROOT_SIGNATURE_FLAG_ALLOW_STREAM_OUTPUT
          : D3D12_ROOT_SIGNATURE_FLAG_NONE;

  // Base parameters.

  // Fetch constants.
  {
    auto& parameter = parameters[kRootParameter_Bindful_FetchConstants];
    parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    parameter.Descriptor.ShaderRegister =
        uint32_t(DxbcShaderTranslator::CbufferRegister::kFetchConstants);
    parameter.Descriptor.RegisterSpace = 0;
    parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  }

  // Vertex float constants.
  {
    auto& parameter = parameters[kRootParameter_Bindful_FloatConstantsVertex];
    parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    parameter.Descriptor.ShaderRegister =
        uint32_t(DxbcShaderTranslator::CbufferRegister::kFloatConstants);
    parameter.Descriptor.RegisterSpace = 0;
    parameter.ShaderVisibility = vertex_visibility;
  }

  // Pixel float constants.
  {
    auto& parameter = parameters[kRootParameter_Bindful_FloatConstantsPixel];
    parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    parameter.Descriptor.ShaderRegister =
        uint32_t(DxbcShaderTranslator::CbufferRegister::kFloatConstants);
    parameter.Descriptor.RegisterSpace = 0;
    parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
  }

  // System constants.
  {
    auto& parameter = parameters[kRootParameter_Bindful_SystemConstants];
    parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    parameter.Descriptor.ShaderRegister =
        uint32_t(DxbcShaderTranslator::CbufferRegister::kSystemConstants);
    parameter.Descriptor.RegisterSpace = 0;
    parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  }

  // Bool and loop constants.
  {
    auto& parameter = parameters[kRootParameter_Bindful_BoolLoopConstants];
    parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    parameter.Descriptor.ShaderRegister =
        uint32_t(DxbcShaderTranslator::CbufferRegister::kBoolLoopConstants);
    parameter.Descriptor.RegisterSpace = 0;
    parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  }

  // Shared memory and, if ROVs are used, EDRAM.
  D3D12_DESCRIPTOR_RANGE shared_memory_and_edram_ranges[3];
  {
    auto& parameter = parameters[kRootParameter_Bindful_SharedMemoryAndEdram];
    parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameter.DescriptorTable.NumDescriptorRanges = 2;
    parameter.DescriptorTable.pDescriptorRanges = shared_memory_and_edram_ranges;
    parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    shared_memory_and_edram_ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    shared_memory_and_edram_ranges[0].NumDescriptors = 1;
    shared_memory_and_edram_ranges[0].BaseShaderRegister =
        uint32_t(DxbcShaderTranslator::SRVMainRegister::kSharedMemory);
    shared_memory_and_edram_ranges[0].RegisterSpace =
        uint32_t(DxbcShaderTranslator::SRVSpace::kMain);
    shared_memory_and_edram_ranges[0].OffsetInDescriptorsFromTableStart = 0;
    shared_memory_and_edram_ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    shared_memory_and_edram_ranges[1].NumDescriptors = 1;
    shared_memory_and_edram_ranges[1].BaseShaderRegister =
        UINT(DxbcShaderTranslator::UAVRegister::kSharedMemory);
    shared_memory_and_edram_ranges[1].RegisterSpace = 0;
    shared_memory_and_edram_ranges[1].OffsetInDescriptorsFromTableStart = 1;
    if (render_target_cache_->GetPath() == RenderTargetCache::Path::kPixelShaderInterlock) {
      ++parameter.DescriptorTable.NumDescriptorRanges;
      shared_memory_and_edram_ranges[2].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
      shared_memory_and_edram_ranges[2].NumDescriptors = 1;
      shared_memory_and_edram_ranges[2].BaseShaderRegister =
          UINT(DxbcShaderTranslator::UAVRegister::kEdram);
      shared_memory_and_edram_ranges[2].RegisterSpace = 0;
      shared_memory_and_edram_ranges[2].OffsetInDescriptorsFromTableStart = 2;
    }
  }

  // Extra parameters.

  // Pixel textures.
  D3D12_DESCRIPTOR_RANGE range_textures_pixel;
  if (texture_count_pixel > 0) {
    auto& parameter = parameters[desc.NumParameters];
    parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameter.DescriptorTable.NumDescriptorRanges = 1;
    parameter.DescriptorTable.pDescriptorRanges = &range_textures_pixel;
    parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    range_textures_pixel.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    range_textures_pixel.NumDescriptors = texture_count_pixel;
    range_textures_pixel.BaseShaderRegister =
        uint32_t(DxbcShaderTranslator::SRVMainRegister::kBindfulTexturesStart);
    range_textures_pixel.RegisterSpace = uint32_t(DxbcShaderTranslator::SRVSpace::kMain);
    range_textures_pixel.OffsetInDescriptorsFromTableStart = 0;
    ++desc.NumParameters;
  }

  // Pixel samplers.
  D3D12_DESCRIPTOR_RANGE range_samplers_pixel;
  if (sampler_count_pixel > 0) {
    auto& parameter = parameters[desc.NumParameters];
    parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameter.DescriptorTable.NumDescriptorRanges = 1;
    parameter.DescriptorTable.pDescriptorRanges = &range_samplers_pixel;
    parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    range_samplers_pixel.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
    range_samplers_pixel.NumDescriptors = sampler_count_pixel;
    range_samplers_pixel.BaseShaderRegister = 0;
    range_samplers_pixel.RegisterSpace = 0;
    range_samplers_pixel.OffsetInDescriptorsFromTableStart = 0;
    ++desc.NumParameters;
  }

  // Vertex textures.
  D3D12_DESCRIPTOR_RANGE range_textures_vertex;
  if (texture_count_vertex > 0) {
    auto& parameter = parameters[desc.NumParameters];
    parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameter.DescriptorTable.NumDescriptorRanges = 1;
    parameter.DescriptorTable.pDescriptorRanges = &range_textures_vertex;
    parameter.ShaderVisibility = vertex_visibility;
    range_textures_vertex.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    range_textures_vertex.NumDescriptors = texture_count_vertex;
    range_textures_vertex.BaseShaderRegister =
        uint32_t(DxbcShaderTranslator::SRVMainRegister::kBindfulTexturesStart);
    range_textures_vertex.RegisterSpace = uint32_t(DxbcShaderTranslator::SRVSpace::kMain);
    range_textures_vertex.OffsetInDescriptorsFromTableStart = 0;
    ++desc.NumParameters;
  }

  // Vertex samplers.
  D3D12_DESCRIPTOR_RANGE range_samplers_vertex;
  if (sampler_count_vertex > 0) {
    auto& parameter = parameters[desc.NumParameters];
    parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameter.DescriptorTable.NumDescriptorRanges = 1;
    parameter.DescriptorTable.pDescriptorRanges = &range_samplers_vertex;
    parameter.ShaderVisibility = vertex_visibility;
    range_samplers_vertex.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
    range_samplers_vertex.NumDescriptors = sampler_count_vertex;
    range_samplers_vertex.BaseShaderRegister = 0;
    range_samplers_vertex.RegisterSpace = 0;
    range_samplers_vertex.OffsetInDescriptorsFromTableStart = 0;
    ++desc.NumParameters;
  }

  ID3D12RootSignature* root_signature =
      ui::d3d12::util::CreateRootSignature(GetD3D12Provider(), desc);
  if (root_signature == nullptr) {
    REXGPU_ERROR(
        "Failed to create a root signature with {} pixel textures, {} pixel "
        "samplers, {} vertex textures and {} vertex samplers",
        texture_count_pixel, sampler_count_pixel, texture_count_vertex, sampler_count_vertex);
    return nullptr;
  }
  root_signatures_bindful_.emplace(index, root_signature);
  return root_signature;
}

uint32_t D3D12CommandProcessor::GetRootBindfulExtraParameterIndices(
    const DxbcShader* vertex_shader, const DxbcShader* pixel_shader,
    RootBindfulExtraParameterIndices& indices_out) {
  uint32_t index = kRootParameter_Bindful_Count_Base;
  if (pixel_shader && !pixel_shader->GetTextureBindingsAfterTranslation().empty()) {
    indices_out.textures_pixel = index++;
  } else {
    indices_out.textures_pixel = RootBindfulExtraParameterIndices::kUnavailable;
  }
  if (pixel_shader && !pixel_shader->GetSamplerBindingsAfterTranslation().empty()) {
    indices_out.samplers_pixel = index++;
  } else {
    indices_out.samplers_pixel = RootBindfulExtraParameterIndices::kUnavailable;
  }
  if (!vertex_shader->GetTextureBindingsAfterTranslation().empty()) {
    indices_out.textures_vertex = index++;
  } else {
    indices_out.textures_vertex = RootBindfulExtraParameterIndices::kUnavailable;
  }
  if (!vertex_shader->GetSamplerBindingsAfterTranslation().empty()) {
    indices_out.samplers_vertex = index++;
  } else {
    indices_out.samplers_vertex = RootBindfulExtraParameterIndices::kUnavailable;
  }
  return index;
}

uint64_t D3D12CommandProcessor::RequestViewBindfulDescriptors(
    uint64_t previous_heap_index, uint32_t count_for_partial_update, uint32_t count_for_full_update,
    D3D12_CPU_DESCRIPTOR_HANDLE& cpu_handle_out, D3D12_GPU_DESCRIPTOR_HANDLE& gpu_handle_out) {
  assert_false(bindless_resources_used_);
  assert_true(submission_open_);
  uint32_t descriptor_index;
  uint64_t current_heap_index = view_bindful_heap_pool_->Request(
      frame_current_, previous_heap_index, count_for_partial_update, count_for_full_update,
      descriptor_index);
  if (current_heap_index == ui::d3d12::D3D12DescriptorHeapPool::kHeapIndexInvalid) {
    // There was an error.
    return ui::d3d12::D3D12DescriptorHeapPool::kHeapIndexInvalid;
  }
  ID3D12DescriptorHeap* heap = view_bindful_heap_pool_->GetLastRequestHeap();
  if (view_bindful_heap_current_ != heap) {
    view_bindful_heap_current_ = heap;
    deferred_command_list_.SetDescriptorHeaps(view_bindful_heap_current_,
                                              sampler_bindful_heap_current_);
  }
  const ui::d3d12::D3D12Provider& provider = GetD3D12Provider();
  cpu_handle_out = provider.OffsetViewDescriptor(
      view_bindful_heap_pool_->GetLastRequestHeapCPUStart(), descriptor_index);
  gpu_handle_out = provider.OffsetViewDescriptor(
      view_bindful_heap_pool_->GetLastRequestHeapGPUStart(), descriptor_index);
  return current_heap_index;
}

uint32_t D3D12CommandProcessor::RequestPersistentViewBindlessDescriptor() {
  assert_true(bindless_resources_used_);
  if (!view_bindless_heap_free_.empty()) {
    uint32_t descriptor_index = view_bindless_heap_free_.back();
    view_bindless_heap_free_.pop_back();
    return descriptor_index;
  }
  if (view_bindless_heap_allocated_ >= kViewBindlessHeapSize) {
    return UINT32_MAX;
  }
  return view_bindless_heap_allocated_++;
}

void D3D12CommandProcessor::ReleaseViewBindlessDescriptorImmediately(uint32_t descriptor_index) {
  assert_true(bindless_resources_used_);
  view_bindless_heap_free_.push_back(descriptor_index);
}

bool D3D12CommandProcessor::RequestOneUseSingleViewDescriptors(
    uint32_t count, ui::d3d12::util::DescriptorCpuGpuHandlePair* handles_out) {
  assert_true(submission_open_);
  if (!count) {
    return true;
  }
  assert_not_null(handles_out);
  const ui::d3d12::D3D12Provider& provider = GetD3D12Provider();
  if (bindless_resources_used_) {
    // Request separate bindless descriptors that will be freed when this
    // submission is completed by the GPU.
    if (count >
        kViewBindlessHeapSize - view_bindless_heap_allocated_ + view_bindless_heap_free_.size()) {
      return false;
    }
    for (uint32_t i = 0; i < count; ++i) {
      uint32_t descriptor_index;
      if (!view_bindless_heap_free_.empty()) {
        descriptor_index = view_bindless_heap_free_.back();
        view_bindless_heap_free_.pop_back();
      } else {
        descriptor_index = view_bindless_heap_allocated_++;
      }
      view_bindless_one_use_descriptors_.push_back(
          std::make_pair(descriptor_index, submission_current_));
      handles_out[i] = std::make_pair(
          provider.OffsetViewDescriptor(view_bindless_heap_cpu_start_, descriptor_index),
          provider.OffsetViewDescriptor(view_bindless_heap_gpu_start_, descriptor_index));
    }
  } else {
    // Request a range within the current heap for bindful resources path.
    D3D12_CPU_DESCRIPTOR_HANDLE cpu_handle_start;
    D3D12_GPU_DESCRIPTOR_HANDLE gpu_handle_start;
    if (RequestViewBindfulDescriptors(ui::d3d12::D3D12DescriptorHeapPool::kHeapIndexInvalid, count,
                                      count, cpu_handle_start, gpu_handle_start) ==
        ui::d3d12::D3D12DescriptorHeapPool::kHeapIndexInvalid) {
      return false;
    }
    for (uint32_t i = 0; i < count; ++i) {
      handles_out[i] = std::make_pair(provider.OffsetViewDescriptor(cpu_handle_start, i),
                                      provider.OffsetViewDescriptor(gpu_handle_start, i));
    }
  }
  return true;
}

ui::d3d12::util::DescriptorCpuGpuHandlePair D3D12CommandProcessor::GetSystemBindlessViewHandlePair(
    SystemBindlessView view) const {
  assert_true(bindless_resources_used_);
  const ui::d3d12::D3D12Provider& provider = GetD3D12Provider();
  return std::make_pair(
      provider.OffsetViewDescriptor(view_bindless_heap_cpu_start_, uint32_t(view)),
      provider.OffsetViewDescriptor(view_bindless_heap_gpu_start_, uint32_t(view)));
}

ui::d3d12::util::DescriptorCpuGpuHandlePair
D3D12CommandProcessor::GetSharedMemoryUintPow2BindlessSRVHandlePair(
    uint32_t element_size_bytes_pow2) const {
  SystemBindlessView view;
  switch (element_size_bytes_pow2) {
    case 2:
      view = SystemBindlessView::kSharedMemoryR32UintSRV;
      break;
    case 3:
      view = SystemBindlessView::kSharedMemoryR32G32UintSRV;
      break;
    case 4:
      view = SystemBindlessView::kSharedMemoryR32G32B32A32UintSRV;
      break;
    default:
      assert_unhandled_case(element_size_bytes_pow2);
      view = SystemBindlessView::kSharedMemoryR32UintSRV;
  }
  return GetSystemBindlessViewHandlePair(view);
}

ui::d3d12::util::DescriptorCpuGpuHandlePair
D3D12CommandProcessor::GetSharedMemoryUintPow2BindlessUAVHandlePair(
    uint32_t element_size_bytes_pow2) const {
  SystemBindlessView view;
  switch (element_size_bytes_pow2) {
    case 2:
      view = SystemBindlessView::kSharedMemoryR32UintUAV;
      break;
    case 3:
      view = SystemBindlessView::kSharedMemoryR32G32UintUAV;
      break;
    case 4:
      view = SystemBindlessView::kSharedMemoryR32G32B32A32UintUAV;
      break;
    default:
      assert_unhandled_case(element_size_bytes_pow2);
      view = SystemBindlessView::kSharedMemoryR32UintUAV;
  }
  return GetSystemBindlessViewHandlePair(view);
}

ui::d3d12::util::DescriptorCpuGpuHandlePair
D3D12CommandProcessor::GetEdramUintPow2BindlessSRVHandlePair(
    uint32_t element_size_bytes_pow2) const {
  SystemBindlessView view;
  switch (element_size_bytes_pow2) {
    case 2:
      view = SystemBindlessView::kEdramR32UintSRV;
      break;
    case 3:
      view = SystemBindlessView::kEdramR32G32UintSRV;
      break;
    case 4:
      view = SystemBindlessView::kEdramR32G32B32A32UintSRV;
      break;
    default:
      assert_unhandled_case(element_size_bytes_pow2);
      view = SystemBindlessView::kEdramR32UintSRV;
  }
  return GetSystemBindlessViewHandlePair(view);
}

ui::d3d12::util::DescriptorCpuGpuHandlePair
D3D12CommandProcessor::GetEdramUintPow2BindlessUAVHandlePair(
    uint32_t element_size_bytes_pow2) const {
  SystemBindlessView view;
  switch (element_size_bytes_pow2) {
    case 2:
      view = SystemBindlessView::kEdramR32UintUAV;
      break;
    case 3:
      view = SystemBindlessView::kEdramR32G32UintUAV;
      break;
    case 4:
      view = SystemBindlessView::kEdramR32G32B32A32UintUAV;
      break;
    default:
      assert_unhandled_case(element_size_bytes_pow2);
      view = SystemBindlessView::kEdramR32UintUAV;
  }
  return GetSystemBindlessViewHandlePair(view);
}

uint64_t D3D12CommandProcessor::RequestSamplerBindfulDescriptors(
    uint64_t previous_heap_index, uint32_t count_for_partial_update, uint32_t count_for_full_update,
    D3D12_CPU_DESCRIPTOR_HANDLE& cpu_handle_out, D3D12_GPU_DESCRIPTOR_HANDLE& gpu_handle_out) {
  assert_false(bindless_resources_used_);
  assert_true(submission_open_);
  uint32_t descriptor_index;
  uint64_t current_heap_index = sampler_bindful_heap_pool_->Request(
      frame_current_, previous_heap_index, count_for_partial_update, count_for_full_update,
      descriptor_index);
  if (current_heap_index == ui::d3d12::D3D12DescriptorHeapPool::kHeapIndexInvalid) {
    // There was an error.
    return ui::d3d12::D3D12DescriptorHeapPool::kHeapIndexInvalid;
  }
  ID3D12DescriptorHeap* heap = sampler_bindful_heap_pool_->GetLastRequestHeap();
  if (sampler_bindful_heap_current_ != heap) {
    sampler_bindful_heap_current_ = heap;
    deferred_command_list_.SetDescriptorHeaps(view_bindful_heap_current_,
                                              sampler_bindful_heap_current_);
  }
  const ui::d3d12::D3D12Provider& provider = GetD3D12Provider();
  cpu_handle_out = provider.OffsetSamplerDescriptor(
      sampler_bindful_heap_pool_->GetLastRequestHeapCPUStart(), descriptor_index);
  gpu_handle_out = provider.OffsetSamplerDescriptor(
      sampler_bindful_heap_pool_->GetLastRequestHeapGPUStart(), descriptor_index);
  return current_heap_index;
}

ID3D12Resource* D3D12CommandProcessor::RequestScratchGPUBuffer(uint32_t size,
                                                               D3D12_RESOURCE_STATES state) {
  assert_true(submission_open_);
  assert_false(scratch_buffer_used_);
  if (!submission_open_ || scratch_buffer_used_ || size == 0) {
    return nullptr;
  }

  if (size <= scratch_buffer_size_) {
    PushTransitionBarrier(scratch_buffer_, scratch_buffer_state_, state);
    scratch_buffer_state_ = state;
    scratch_buffer_used_ = true;
    return scratch_buffer_;
  }

  size = rex::align(size, kScratchBufferSizeIncrement);

  const ui::d3d12::D3D12Provider& provider = GetD3D12Provider();
  ID3D12Device* device = provider.GetDevice();
  D3D12_RESOURCE_DESC buffer_desc;
  ui::d3d12::util::FillBufferResourceDesc(buffer_desc, size,
                                          D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
  ID3D12Resource* buffer;
  if (FAILED(device->CreateCommittedResource(&ui::d3d12::util::kHeapPropertiesDefault,
                                             provider.GetHeapFlagCreateNotZeroed(), &buffer_desc,
                                             state, nullptr, IID_PPV_ARGS(&buffer)))) {
    REXGPU_ERROR("Failed to create a {} MB scratch GPU buffer", size >> 20);
    return nullptr;
  }
  if (scratch_buffer_ != nullptr) {
    resources_for_deletion_.emplace_back(submission_current_, scratch_buffer_);
  }
  scratch_buffer_ = buffer;
  scratch_buffer_size_ = size;
  scratch_buffer_state_ = state;
  scratch_buffer_used_ = true;
  return scratch_buffer_;
}

void D3D12CommandProcessor::ReleaseScratchGPUBuffer(ID3D12Resource* buffer,
                                                    D3D12_RESOURCE_STATES new_state) {
  assert_true(submission_open_);
  assert_true(scratch_buffer_used_);
  scratch_buffer_used_ = false;
  if (buffer == scratch_buffer_) {
    scratch_buffer_state_ = new_state;
  }
}

void D3D12CommandProcessor::SetExternalPipeline(ID3D12PipelineState* pipeline) {
  if (current_external_pipeline_ != pipeline) {
    current_external_pipeline_ = pipeline;
    current_guest_pipeline_ = nullptr;
    deferred_command_list_.D3DSetPipelineState(pipeline);
  }
}

void D3D12CommandProcessor::SetExternalGraphicsRootSignature(ID3D12RootSignature* root_signature) {
  if (current_graphics_root_signature_ != root_signature) {
    current_graphics_root_signature_ = root_signature;
    deferred_command_list_.D3DSetGraphicsRootSignature(root_signature);
  }
  // Force-invalidate because setting a non-guest root signature.
  current_graphics_root_up_to_date_ = 0;
}

void D3D12CommandProcessor::SetViewport(const D3D12_VIEWPORT& viewport) {
  ff_viewport_update_needed_ |= ff_viewport_.TopLeftX != viewport.TopLeftX;
  ff_viewport_update_needed_ |= ff_viewport_.TopLeftY != viewport.TopLeftY;
  ff_viewport_update_needed_ |= ff_viewport_.Width != viewport.Width;
  ff_viewport_update_needed_ |= ff_viewport_.Height != viewport.Height;
  ff_viewport_update_needed_ |= ff_viewport_.MinDepth != viewport.MinDepth;
  ff_viewport_update_needed_ |= ff_viewport_.MaxDepth != viewport.MaxDepth;
  if (ff_viewport_update_needed_) {
    ff_viewport_ = viewport;
    deferred_command_list_.RSSetViewport(ff_viewport_);
    ff_viewport_update_needed_ = false;
  }
}

void D3D12CommandProcessor::SetScissorRect(const D3D12_RECT& scissor_rect) {
  ff_scissor_update_needed_ |= ff_scissor_.left != scissor_rect.left;
  ff_scissor_update_needed_ |= ff_scissor_.top != scissor_rect.top;
  ff_scissor_update_needed_ |= ff_scissor_.right != scissor_rect.right;
  ff_scissor_update_needed_ |= ff_scissor_.bottom != scissor_rect.bottom;
  if (ff_scissor_update_needed_) {
    ff_scissor_ = scissor_rect;
    deferred_command_list_.RSSetScissorRect(ff_scissor_);
    ff_scissor_update_needed_ = false;
  }
}

void D3D12CommandProcessor::SetStencilReference(uint32_t stencil_ref) {
  ff_stencil_ref_update_needed_ |= ff_stencil_ref_ != stencil_ref;
  if (ff_stencil_ref_update_needed_) {
    ff_stencil_ref_ = stencil_ref;
    deferred_command_list_.D3DOMSetStencilRef(stencil_ref);
    ff_stencil_ref_update_needed_ = false;
  }
}

void D3D12CommandProcessor::SetPrimitiveTopology(D3D12_PRIMITIVE_TOPOLOGY primitive_topology) {
  if (primitive_topology_ != primitive_topology) {
    primitive_topology_ = primitive_topology;
    deferred_command_list_.D3DIASetPrimitiveTopology(primitive_topology);
  }
}

std::string D3D12CommandProcessor::GetWindowTitleText() const {
  std::ostringstream title;
  title << "Direct3D 12";
  if (render_target_cache_) {
    // Rasterizer-ordered views are a feature very rarely used as of 2020 and
    // that faces adoption complications (outside of Direct3D - on Vulkan - at
    // least), but crucial to Xenia - raise awareness of its usage.
    // https://github.com/KhronosGroup/Vulkan-Ecosystem/issues/27#issuecomment-455712319
    // "In Xenia's title bar "D3D12 ROV" can be seen, which was a surprise, as I
    //  wasn't aware that Xenia D3D12 backend was using Raster Order Views
    //  feature" - oscarbg in that issue.
    switch (render_target_cache_->GetPath()) {
      case RenderTargetCache::Path::kHostRenderTargets:
        title << " - RTV/DSV";
        break;
      case RenderTargetCache::Path::kPixelShaderInterlock:
        title << " - ROV";
        break;
      default:
        break;
    }
    uint32_t draw_resolution_scale_x =
        texture_cache_ ? texture_cache_->draw_resolution_scale_x() : 1;
    uint32_t draw_resolution_scale_y =
        texture_cache_ ? texture_cache_->draw_resolution_scale_y() : 1;
    if (draw_resolution_scale_x > 1 || draw_resolution_scale_y > 1) {
      title << ' ' << draw_resolution_scale_x << 'x' << draw_resolution_scale_y;
    }
  }
  return title.str();
}

bool D3D12CommandProcessor::SetupContext() {
  if constexpr (!kGpuDiagnostics) {
    // Player build: the command-processor diagnostics are compiled out. Name
    // each main switch a launch still sets, once, so a measurement script run
    // against a player package can't silently measure nothing.
    for (const char* name :
         {"embedded_hitch_diagnostics", "embedded_cp_cadence_diagnostics",
          "embedded_swap_interval_diagnostics", "embedded_swap_long_frame_us",
          "embedded_gameplay_capture_start_swap", "embedded_target_writer_capture_count",
          "embedded_manual_gameplay_capture", "embedded_camera_draw_capture",
          "embedded_pc_scene_history"}) {
      if (rex::cvar::HasNonDefaultValue(name)) {
        std::fprintf(stderr, "REX_GPU_DIAGNOSTICS_COMPILED_OUT cvar=%s\n", name);
        std::fflush(stderr);
      }
    }
  }
  async_submission_ = REXCVAR_GET(d3d12_async_submission);
  guest_draw_submit_limit_ = REXCVAR_GET(d3d12_submit_after_draws);
  if (!guest_draw_submit_limit_ && async_submission_) {
    // Replay is off the command processor thread, so closing batches early
    // costs it only the handoff, and the swap-time drain stays short.
    guest_draw_submit_limit_ = REXCVAR_GET(d3d12_async_submit_after_draws);
  }
  if (guest_draw_submit_limit_ > 4096) {
    REXGPU_ERROR("Invalid experimental draw submission limit (maximum 4096)");
    return false;
  }
  submission_guest_draws_ = 0;
  guest_draw_submit_count_ = 0;
  if (guest_draw_submit_limit_) {
    std::fprintf(stderr, "REX_DRAW_BATCH_CONFIG limit=%u query_splits=0\n",
                 guest_draw_submit_limit_);
  }
  if (!render_target::native_shader_scale_policy::Parse(
          REXCVAR_GET(draw_resolution_scale_native_grid_rules), native_shader_grid_rules_)) {
    REXGPU_ERROR("Invalid native shader-grid policy; refusing ambiguous scale annotations");
    return false;
  }
  native_shader_grid_logged_mask_ = 0;
  embedded_target_writer_config_ = {
      REXCVAR_GET(embedded_target_writer_surface),
      REXCVAR_GET(embedded_target_writer_color),
      REXCVAR_GET(embedded_target_writer_scissor_br),
      REXCVAR_GET(embedded_target_writer_min_draw),
      REXCVAR_GET(embedded_target_writer_first),
      REXCVAR_GET(embedded_target_writer_stride),
      REXCVAR_GET(embedded_target_writer_capture_count),
      REXCVAR_GET(embedded_target_writer_pixel_shader),
      REXCVAR_GET(embedded_target_writer_capture_pairs),
      REXCVAR_GET(embedded_target_writer_auto_start_swap)};
  embedded_target_writer_state_ = {};
  embedded_depth_resolve_capture_state_ = {};
  embedded_writer_match_frame = 0;
  embedded_writer_match_first_draw = 0;
  if (!embedded_target_writer_capture_policy::IsValid(embedded_target_writer_config_)) {
    REXGPU_ERROR("Invalid bounded target-writer diagnostic configuration");
    return false;
  }
  if (!CommandProcessor::SetupContext()) {
    REXGPU_ERROR("Failed to initialize base command processor context");
    return false;
  }
  // Constant ranges carry binding/texture/vertex-buffer invalidation in
  // D3D12CommandProcessor::WriteRegister; every other register is a plain
  // store there, so type-0 packets may store those directly.
  MarkRegisterWriteHandler(XE_GPU_REG_SHADER_CONSTANT_000_X, XE_GPU_REG_SHADER_CONSTANT_511_W);
  MarkRegisterWriteHandler(XE_GPU_REG_SHADER_CONSTANT_BOOL_000_031,
                           XE_GPU_REG_SHADER_CONSTANT_LOOP_31);
  MarkRegisterWriteHandler(XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0,
                           XE_GPU_REG_SHADER_CONSTANT_FETCH_31_5);
  EnableRegisterWriteFastPath();
  InvalidateAllVertexBufferResidency();
  UpdateDebugMarkersEnabled();

  const ui::d3d12::D3D12Provider& provider = GetD3D12Provider();
  ID3D12Device* device = provider.GetDevice();
  ID3D12CommandQueue* direct_queue = provider.GetDirectQueue();

  fence_completion_event_ = CreateEvent(nullptr, FALSE, FALSE, nullptr);
  if (fence_completion_event_ == nullptr) {
    REXGPU_ERROR("Failed to create the fence completion event");
    return false;
  }
  if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&submission_fence_)))) {
    REXGPU_ERROR("Failed to create the submission fence");
    return false;
  }
  if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE,
                                 IID_PPV_ARGS(&queue_operations_since_submission_fence_)))) {
    REXGPU_ERROR(
        "Failed to create the fence for awaiting queue operations done since "
        "the latest submission");
    return false;
  }

  // Create the command list and one allocator because it's needed for a command
  // list.
  ID3D12CommandAllocator* command_allocator;
  if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                            IID_PPV_ARGS(&command_allocator)))) {
    REXGPU_ERROR("Failed to create a command allocator");
    return false;
  }
  command_allocator_writable_first_ = new CommandAllocator;
  command_allocator_writable_first_->command_allocator = command_allocator;
  command_allocator_writable_first_->last_usage_submission = 0;
  command_allocator_writable_first_->next = nullptr;
  command_allocator_writable_last_ = command_allocator_writable_first_;
  if (FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, command_allocator,
                                       nullptr, IID_PPV_ARGS(&command_list_)))) {
    REXGPU_ERROR("Failed to create the graphics command list");
    return false;
  }
  // Initially in open state, wait until a deferred command list submission.
  command_list_->Close();
  // Optional - added in Creators Update (SDK 10.0.15063.0).
  command_list_->QueryInterface(IID_PPV_ARGS(&command_list_1_));
  if (async_submission_) {
    StartSubmissionThread();
    std::fprintf(stderr, "REX_ASYNC_SUBMISSION enabled=1 submit_after_draws=%u\n",
                 guest_draw_submit_limit_);
  }

  bindless_resources_used_ = REXCVAR_GET(d3d12_bindless) &&
                             provider.GetResourceBindingTier() >= D3D12_RESOURCE_BINDING_TIER_2;

  // Get the draw resolution scale for the render target cache and the texture
  // cache.
  uint32_t draw_resolution_scale_x, draw_resolution_scale_y;
  bool draw_resolution_scale_not_clamped =
      TextureCache::GetConfigDrawResolutionScale(draw_resolution_scale_x, draw_resolution_scale_y);
  if (!D3D12TextureCache::ClampDrawResolutionScaleToMaxSupported(
          draw_resolution_scale_x, draw_resolution_scale_y, provider)) {
    draw_resolution_scale_not_clamped = false;
  }
  if (!draw_resolution_scale_not_clamped) {
    REXGPU_WARN(
        "The requested draw resolution scale is not supported by the device or "
        "the emulator, reducing to {}x{}",
        draw_resolution_scale_x, draw_resolution_scale_y);
  }

  shared_memory_ = std::make_unique<D3D12SharedMemory>(*this, *memory_, trace_writer_);
  if (!shared_memory_->Initialize()) {
    REXGPU_ERROR("Failed to initialize shared memory");
    return false;
  }
  // Pending plugin host writes are published like guest stores: shared
  // memory pages and primitive processor cache (this thread only).
  shared_memory_->SetHostWritePublisher(
      [](void* context, uint32_t start, uint32_t length) {
        static_cast<D3D12CommandProcessor*>(context)->TracePlaybackWroteMemory(start, length);
      },
      this);

  // Initialize the render target cache before configuring binding - need to
  // know if using rasterizer-ordered views for the bindless root signature.
  render_target_cache_ = std::make_unique<D3D12RenderTargetCache>(
      *register_file_, *memory_, trace_writer_, draw_resolution_scale_x, draw_resolution_scale_y,
      *this, bindless_resources_used_);
  if (!render_target_cache_->Initialize()) {
    REXGPU_ERROR("Failed to initialize the render target cache");
    return false;
  }

  // Initialize resource binding.
  constant_buffer_pool_ = std::make_unique<ui::d3d12::D3D12UploadBufferPool>(
      provider, std::max(ui::d3d12::D3D12UploadBufferPool::kDefaultPageSize,
                         sizeof(float) * 4 * D3D12_REQ_CONSTANT_BUFFER_ELEMENT_COUNT));
  if (bindless_resources_used_) {
    D3D12_DESCRIPTOR_HEAP_DESC view_bindless_heap_desc;
    view_bindless_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    view_bindless_heap_desc.NumDescriptors = kViewBindlessHeapSize;
    view_bindless_heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    view_bindless_heap_desc.NodeMask = 0;
    if (FAILED(device->CreateDescriptorHeap(&view_bindless_heap_desc,
                                            IID_PPV_ARGS(&view_bindless_heap_)))) {
      REXGPU_ERROR("Failed to create the bindless CBV/SRV/UAV descriptor heap");
      return false;
    }
    view_bindless_heap_cpu_start_ = view_bindless_heap_->GetCPUDescriptorHandleForHeapStart();
    view_bindless_heap_gpu_start_ = view_bindless_heap_->GetGPUDescriptorHandleForHeapStart();
    view_bindless_heap_allocated_ = uint32_t(SystemBindlessView::kCount);

    D3D12_DESCRIPTOR_HEAP_DESC sampler_bindless_heap_desc;
    sampler_bindless_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
    sampler_bindless_heap_desc.NumDescriptors = kSamplerHeapSize;
    sampler_bindless_heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    sampler_bindless_heap_desc.NodeMask = 0;
    if (FAILED(device->CreateDescriptorHeap(&sampler_bindless_heap_desc,
                                            IID_PPV_ARGS(&sampler_bindless_heap_current_)))) {
      REXGPU_ERROR("Failed to create the bindless sampler descriptor heap");
      return false;
    }
    sampler_bindless_heap_cpu_start_ =
        sampler_bindless_heap_current_->GetCPUDescriptorHandleForHeapStart();
    sampler_bindless_heap_gpu_start_ =
        sampler_bindless_heap_current_->GetGPUDescriptorHandleForHeapStart();
    sampler_bindless_heap_allocated_ = 0;
  } else {
    view_bindful_heap_pool_ = std::make_unique<ui::d3d12::D3D12DescriptorHeapPool>(
        device, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, kViewBindfulHeapSize);
    sampler_bindful_heap_pool_ = std::make_unique<ui::d3d12::D3D12DescriptorHeapPool>(
        device, D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, kSamplerHeapSize);
  }

  if (bindless_resources_used_) {
    // Global bindless resource root signatures.
    // No CBV or UAV descriptor ranges with any descriptors to be allocated
    // dynamically (via RequestPersistentViewBindlessDescriptor or
    // RequestOneUseSingleViewDescriptors) should be here, because they would
    // overlap the unbounded SRV range, which is not allowed on Nvidia Fermi!
    D3D12_ROOT_SIGNATURE_DESC root_signature_bindless_desc;
    D3D12_ROOT_PARAMETER
    root_parameters_bindless[kRootParameter_Bindless_Count];
    root_signature_bindless_desc.NumParameters = kRootParameter_Bindless_Count;
    root_signature_bindless_desc.pParameters = root_parameters_bindless;
    root_signature_bindless_desc.NumStaticSamplers = 0;
    root_signature_bindless_desc.pStaticSamplers = nullptr;
    root_signature_bindless_desc.Flags =
        REXCVAR_GET(d3d12_embedded_scene_host_vertex_output_diagnostic)
            ? D3D12_ROOT_SIGNATURE_FLAG_ALLOW_STREAM_OUTPUT
            : D3D12_ROOT_SIGNATURE_FLAG_NONE;
    // Fetch constants.
    {
      auto& parameter = root_parameters_bindless[kRootParameter_Bindless_FetchConstants];
      parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
      parameter.Descriptor.ShaderRegister =
          uint32_t(DxbcShaderTranslator::CbufferRegister::kFetchConstants);
      parameter.Descriptor.RegisterSpace = 0;
      parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    }
    // Vertex float constants.
    {
      auto& parameter = root_parameters_bindless[kRootParameter_Bindless_FloatConstantsVertex];
      parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
      parameter.Descriptor.ShaderRegister =
          uint32_t(DxbcShaderTranslator::CbufferRegister::kFloatConstants);
      parameter.Descriptor.RegisterSpace = 0;
      parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    }
    // Pixel float constants.
    {
      auto& parameter = root_parameters_bindless[kRootParameter_Bindless_FloatConstantsPixel];
      parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
      parameter.Descriptor.ShaderRegister =
          uint32_t(DxbcShaderTranslator::CbufferRegister::kFloatConstants);
      parameter.Descriptor.RegisterSpace = 0;
      parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    }
    // Pixel shader descriptor indices.
    {
      auto& parameter = root_parameters_bindless[kRootParameter_Bindless_DescriptorIndicesPixel];
      parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
      parameter.Descriptor.ShaderRegister =
          uint32_t(DxbcShaderTranslator::CbufferRegister::kDescriptorIndices);
      parameter.Descriptor.RegisterSpace = 0;
      parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    }
    // Vertex shader descriptor indices.
    {
      auto& parameter = root_parameters_bindless[kRootParameter_Bindless_DescriptorIndicesVertex];
      parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
      parameter.Descriptor.ShaderRegister =
          uint32_t(DxbcShaderTranslator::CbufferRegister::kDescriptorIndices);
      parameter.Descriptor.RegisterSpace = 0;
      parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    }
    // System constants.
    {
      auto& parameter = root_parameters_bindless[kRootParameter_Bindless_SystemConstants];
      parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
      parameter.Descriptor.ShaderRegister =
          uint32_t(DxbcShaderTranslator::CbufferRegister::kSystemConstants);
      parameter.Descriptor.RegisterSpace = 0;
      parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    }
    // Bool and loop constants.
    {
      auto& parameter = root_parameters_bindless[kRootParameter_Bindless_BoolLoopConstants];
      parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
      parameter.Descriptor.ShaderRegister =
          uint32_t(DxbcShaderTranslator::CbufferRegister::kBoolLoopConstants);
      parameter.Descriptor.RegisterSpace = 0;
      parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    }
    // Shared memory SRV and UAV.
    D3D12_DESCRIPTOR_RANGE root_shared_memory_view_ranges[2];
    {
      auto& parameter = root_parameters_bindless[kRootParameter_Bindless_SharedMemory];
      parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
      parameter.DescriptorTable.NumDescriptorRanges =
          uint32_t(rex::countof(root_shared_memory_view_ranges));
      parameter.DescriptorTable.pDescriptorRanges = root_shared_memory_view_ranges;
      parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
      {
        auto& range = root_shared_memory_view_ranges[0];
        range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        range.NumDescriptors = 1;
        range.BaseShaderRegister = UINT(DxbcShaderTranslator::SRVMainRegister::kSharedMemory);
        range.RegisterSpace = UINT(DxbcShaderTranslator::SRVSpace::kMain);
        range.OffsetInDescriptorsFromTableStart = 0;
      }
      {
        auto& range = root_shared_memory_view_ranges[1];
        range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        range.NumDescriptors = 1;
        range.BaseShaderRegister = UINT(DxbcShaderTranslator::UAVRegister::kSharedMemory);
        range.RegisterSpace = 0;
        range.OffsetInDescriptorsFromTableStart = 1;
      }
    }
    // Sampler heap.
    D3D12_DESCRIPTOR_RANGE root_bindless_sampler_range;
    {
      auto& parameter = root_parameters_bindless[kRootParameter_Bindless_SamplerHeap];
      parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
      // Will be appending.
      parameter.DescriptorTable.NumDescriptorRanges = 1;
      parameter.DescriptorTable.pDescriptorRanges = &root_bindless_sampler_range;
      parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
      root_bindless_sampler_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
      root_bindless_sampler_range.NumDescriptors = UINT_MAX;
      root_bindless_sampler_range.BaseShaderRegister = 0;
      root_bindless_sampler_range.RegisterSpace = 0;
      root_bindless_sampler_range.OffsetInDescriptorsFromTableStart = 0;
    }
    // View heap.
    D3D12_DESCRIPTOR_RANGE root_bindless_view_ranges[4];
    {
      auto& parameter = root_parameters_bindless[kRootParameter_Bindless_ViewHeap];
      parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
      // Will be appending.
      parameter.DescriptorTable.NumDescriptorRanges = 0;
      parameter.DescriptorTable.pDescriptorRanges = root_bindless_view_ranges;
      parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
      // EDRAM.
      if (render_target_cache_->GetPath() == RenderTargetCache::Path::kPixelShaderInterlock) {
        assert_true(parameter.DescriptorTable.NumDescriptorRanges <
                    rex::countof(root_bindless_view_ranges));
        auto& range = root_bindless_view_ranges[parameter.DescriptorTable.NumDescriptorRanges++];
        range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        range.NumDescriptors = 1;
        range.BaseShaderRegister = UINT(DxbcShaderTranslator::UAVRegister::kEdram);
        range.RegisterSpace = 0;
        range.OffsetInDescriptorsFromTableStart = UINT(SystemBindlessView::kEdramR32UintUAV);
      }
      // Used UAV and SRV ranges must not overlap on Nvidia Fermi, so textures
      // have OffsetInDescriptorsFromTableStart after all static descriptors of
      // other types.
      // 2D array textures.
      {
        assert_true(parameter.DescriptorTable.NumDescriptorRanges <
                    rex::countof(root_bindless_view_ranges));
        auto& range = root_bindless_view_ranges[parameter.DescriptorTable.NumDescriptorRanges++];
        range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        range.NumDescriptors = UINT_MAX;
        range.BaseShaderRegister = 0;
        range.RegisterSpace = UINT(DxbcShaderTranslator::SRVSpace::kBindlessTextures2DArray);
        range.OffsetInDescriptorsFromTableStart = UINT(SystemBindlessView::kUnboundedSRVsStart);
      }
      // 3D textures.
      {
        assert_true(parameter.DescriptorTable.NumDescriptorRanges <
                    rex::countof(root_bindless_view_ranges));
        auto& range = root_bindless_view_ranges[parameter.DescriptorTable.NumDescriptorRanges++];
        range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        range.NumDescriptors = UINT_MAX;
        range.BaseShaderRegister = 0;
        range.RegisterSpace = UINT(DxbcShaderTranslator::SRVSpace::kBindlessTextures3D);
        range.OffsetInDescriptorsFromTableStart = UINT(SystemBindlessView::kUnboundedSRVsStart);
      }
      // Cube textures.
      {
        assert_true(parameter.DescriptorTable.NumDescriptorRanges <
                    rex::countof(root_bindless_view_ranges));
        auto& range = root_bindless_view_ranges[parameter.DescriptorTable.NumDescriptorRanges++];
        range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        range.NumDescriptors = UINT_MAX;
        range.BaseShaderRegister = 0;
        range.RegisterSpace = UINT(DxbcShaderTranslator::SRVSpace::kBindlessTexturesCube);
        range.OffsetInDescriptorsFromTableStart = UINT(SystemBindlessView::kUnboundedSRVsStart);
      }
    }
    root_signature_bindless_vs_ =
        ui::d3d12::util::CreateRootSignature(provider, root_signature_bindless_desc);
    if (!root_signature_bindless_vs_) {
      REXGPU_ERROR(
          "Failed to create the global root signature for bindless resources, "
          "the version for use without tessellation");
      return false;
    }
    root_parameters_bindless[kRootParameter_Bindless_FloatConstantsVertex].ShaderVisibility =
        D3D12_SHADER_VISIBILITY_DOMAIN;
    root_parameters_bindless[kRootParameter_Bindless_DescriptorIndicesVertex].ShaderVisibility =
        D3D12_SHADER_VISIBILITY_DOMAIN;
    root_signature_bindless_ds_ =
        ui::d3d12::util::CreateRootSignature(provider, root_signature_bindless_desc);
    if (!root_signature_bindless_ds_) {
      REXGPU_ERROR(
          "Failed to create the global root signature for bindless resources, "
          "the version for use with tessellation");
      return false;
    }
  }

  primitive_processor_ = std::make_unique<D3D12PrimitiveProcessor>(
      *register_file_, *memory_, trace_writer_, *shared_memory_, *this);
  if (!primitive_processor_->Initialize()) {
    REXGPU_ERROR("Failed to initialize the geometric primitive processor");
    return false;
  }

  texture_cache_ =
      D3D12TextureCache::Create(*register_file_, *shared_memory_, draw_resolution_scale_x,
                                draw_resolution_scale_y, *this, bindless_resources_used_);
  if (!texture_cache_) {
    REXGPU_ERROR("Failed to initialize the texture cache");
    return false;
  }
#if REX_GPU_DIAGNOSTICS
  // Measurement builds only: a constant false in player builds.
  track_ring_publications_ = !kernel_state_ && texture_cache_->OwnsSceneHistory();
#endif

  pipeline_cache_ = std::make_unique<PipelineCache>(
      *this, *register_file_, *render_target_cache_.get(), bindless_resources_used_);
  if (!pipeline_cache_->Initialize()) {
    REXGPU_ERROR("Failed to initialize the graphics pipeline cache");
    return false;
  }

  D3D12_HEAP_FLAGS heap_flag_create_not_zeroed = provider.GetHeapFlagCreateNotZeroed();

  // Create gamma ramp resources.
  gamma_ramp_256_entry_table_up_to_date_ = false;
  gamma_ramp_pwl_up_to_date_ = false;
  D3D12_RESOURCE_DESC gamma_ramp_buffer_desc;
  ui::d3d12::util::FillBufferResourceDesc(gamma_ramp_buffer_desc, (256 + 128 * 3) * 4,
                                          D3D12_RESOURCE_FLAG_NONE);
  // The first action will be uploading.
  gamma_ramp_buffer_state_ = D3D12_RESOURCE_STATE_COPY_DEST;
  if (FAILED(device->CreateCommittedResource(&ui::d3d12::util::kHeapPropertiesDefault,
                                             heap_flag_create_not_zeroed, &gamma_ramp_buffer_desc,
                                             gamma_ramp_buffer_state_, nullptr,
                                             IID_PPV_ARGS(&gamma_ramp_buffer_)))) {
    REXGPU_ERROR("Failed to create the gamma ramp buffer");
    return false;
  }
  // The upload buffer is frame-buffered.
  gamma_ramp_buffer_desc.Width *= kQueueFrames;
  if (FAILED(device->CreateCommittedResource(&ui::d3d12::util::kHeapPropertiesUpload,
                                             heap_flag_create_not_zeroed, &gamma_ramp_buffer_desc,
                                             D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                             IID_PPV_ARGS(&gamma_ramp_upload_buffer_)))) {
    REXGPU_ERROR("Failed to create the gamma ramp upload buffer");
    return false;
  }
  if (FAILED(gamma_ramp_upload_buffer_->Map(
          0, nullptr, reinterpret_cast<void**>(&gamma_ramp_upload_buffer_mapping_)))) {
    REXGPU_ERROR("Failed to map the gamma ramp upload buffer");
    gamma_ramp_upload_buffer_mapping_ = nullptr;
    return false;
  }

  // Initialize compute pipelines for output with gamma ramp.
  D3D12_ROOT_PARAMETER
  apply_gamma_root_parameters[UINT(ApplyGammaRootParameter::kCount)];
  {
    D3D12_ROOT_PARAMETER& apply_gamma_root_parameter_constants =
        apply_gamma_root_parameters[UINT(ApplyGammaRootParameter::kConstants)];
    apply_gamma_root_parameter_constants.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    apply_gamma_root_parameter_constants.Constants.ShaderRegister = 0;
    apply_gamma_root_parameter_constants.Constants.RegisterSpace = 0;
    apply_gamma_root_parameter_constants.Constants.Num32BitValues =
        sizeof(ApplyGammaConstants) / sizeof(uint32_t);
    apply_gamma_root_parameter_constants.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  }
  D3D12_DESCRIPTOR_RANGE apply_gamma_root_descriptor_range_dest;
  apply_gamma_root_descriptor_range_dest.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
  apply_gamma_root_descriptor_range_dest.NumDescriptors = 1;
  apply_gamma_root_descriptor_range_dest.BaseShaderRegister = 0;
  apply_gamma_root_descriptor_range_dest.RegisterSpace = 0;
  apply_gamma_root_descriptor_range_dest.OffsetInDescriptorsFromTableStart = 0;
  {
    D3D12_ROOT_PARAMETER& apply_gamma_root_parameter_dest =
        apply_gamma_root_parameters[UINT(ApplyGammaRootParameter::kDestination)];
    apply_gamma_root_parameter_dest.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    apply_gamma_root_parameter_dest.DescriptorTable.NumDescriptorRanges = 1;
    apply_gamma_root_parameter_dest.DescriptorTable.pDescriptorRanges =
        &apply_gamma_root_descriptor_range_dest;
    apply_gamma_root_parameter_dest.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  }
  D3D12_DESCRIPTOR_RANGE apply_gamma_root_descriptor_range_source;
  apply_gamma_root_descriptor_range_source.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
  apply_gamma_root_descriptor_range_source.NumDescriptors = 1;
  apply_gamma_root_descriptor_range_source.BaseShaderRegister = 1;
  apply_gamma_root_descriptor_range_source.RegisterSpace = 0;
  apply_gamma_root_descriptor_range_source.OffsetInDescriptorsFromTableStart = 0;
  {
    D3D12_ROOT_PARAMETER& apply_gamma_root_parameter_source =
        apply_gamma_root_parameters[UINT(ApplyGammaRootParameter::kSource)];
    apply_gamma_root_parameter_source.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    apply_gamma_root_parameter_source.DescriptorTable.NumDescriptorRanges = 1;
    apply_gamma_root_parameter_source.DescriptorTable.pDescriptorRanges =
        &apply_gamma_root_descriptor_range_source;
    apply_gamma_root_parameter_source.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  }
  D3D12_DESCRIPTOR_RANGE apply_gamma_root_descriptor_range_ramp;
  apply_gamma_root_descriptor_range_ramp.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
  apply_gamma_root_descriptor_range_ramp.NumDescriptors = 1;
  apply_gamma_root_descriptor_range_ramp.BaseShaderRegister = 0;
  apply_gamma_root_descriptor_range_ramp.RegisterSpace = 0;
  apply_gamma_root_descriptor_range_ramp.OffsetInDescriptorsFromTableStart = 0;
  {
    D3D12_ROOT_PARAMETER& apply_gamma_root_parameter_gamma_ramp =
        apply_gamma_root_parameters[UINT(ApplyGammaRootParameter::kRamp)];
    apply_gamma_root_parameter_gamma_ramp.ParameterType =
        D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    apply_gamma_root_parameter_gamma_ramp.DescriptorTable.NumDescriptorRanges = 1;
    apply_gamma_root_parameter_gamma_ramp.DescriptorTable.pDescriptorRanges =
        &apply_gamma_root_descriptor_range_ramp;
    apply_gamma_root_parameter_gamma_ramp.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  }
  D3D12_ROOT_SIGNATURE_DESC apply_gamma_root_signature_desc;
  apply_gamma_root_signature_desc.NumParameters = UINT(ApplyGammaRootParameter::kCount);
  apply_gamma_root_signature_desc.pParameters = apply_gamma_root_parameters;
  apply_gamma_root_signature_desc.NumStaticSamplers = 0;
  apply_gamma_root_signature_desc.pStaticSamplers = nullptr;
  apply_gamma_root_signature_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;
  *(apply_gamma_root_signature_.ReleaseAndGetAddressOf()) =
      ui::d3d12::util::CreateRootSignature(provider, apply_gamma_root_signature_desc);
  if (!apply_gamma_root_signature_) {
    REXGPU_ERROR("Failed to create the gamma ramp application root signature");
    return false;
  }
  *(apply_gamma_table_pipeline_.ReleaseAndGetAddressOf()) = ui::d3d12::util::CreateComputePipeline(
      device, shaders::apply_gamma_table_cs, sizeof(shaders::apply_gamma_table_cs),
      apply_gamma_root_signature_.Get());
  if (!apply_gamma_table_pipeline_) {
    REXGPU_ERROR(
        "Failed to create the 256-entry table gamma ramp application compute "
        "pipeline");
    return false;
  }
  *(apply_gamma_table_fxaa_luma_pipeline_.ReleaseAndGetAddressOf()) =
      ui::d3d12::util::CreateComputePipeline(device, shaders::apply_gamma_table_fxaa_luma_cs,
                                             sizeof(shaders::apply_gamma_table_fxaa_luma_cs),
                                             apply_gamma_root_signature_.Get());
  if (!apply_gamma_table_fxaa_luma_pipeline_) {
    REXGPU_ERROR(
        "Failed to create the 256-entry table gamma ramp application compute "
        "pipeline with perceptual luma output");
    return false;
  }
  *(apply_gamma_pwl_pipeline_.ReleaseAndGetAddressOf()) = ui::d3d12::util::CreateComputePipeline(
      device, shaders::apply_gamma_pwl_cs, sizeof(shaders::apply_gamma_pwl_cs),
      apply_gamma_root_signature_.Get());
  if (!apply_gamma_pwl_pipeline_) {
    REXGPU_ERROR("Failed to create the PWL gamma ramp application compute pipeline");
    return false;
  }
  *(apply_gamma_pwl_fxaa_luma_pipeline_.ReleaseAndGetAddressOf()) =
      ui::d3d12::util::CreateComputePipeline(device, shaders::apply_gamma_pwl_fxaa_luma_cs,
                                             sizeof(shaders::apply_gamma_pwl_fxaa_luma_cs),
                                             apply_gamma_root_signature_.Get());
  if (!apply_gamma_pwl_fxaa_luma_pipeline_) {
    REXGPU_ERROR(
        "Failed to create the PWL gamma ramp application compute pipeline with "
        "perceptual luma output");
    return false;
  }

  // Initialize compute pipelines for post-processing anti-aliasing.
  D3D12_ROOT_PARAMETER fxaa_root_parameters[UINT(FxaaRootParameter::kCount)];
  {
    D3D12_ROOT_PARAMETER& fxaa_root_parameter_constants =
        fxaa_root_parameters[UINT(ApplyGammaRootParameter::kConstants)];
    fxaa_root_parameter_constants.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    fxaa_root_parameter_constants.Constants.ShaderRegister = 0;
    fxaa_root_parameter_constants.Constants.RegisterSpace = 0;
    fxaa_root_parameter_constants.Constants.Num32BitValues =
        sizeof(FxaaConstants) / sizeof(uint32_t);
    fxaa_root_parameter_constants.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  }
  D3D12_DESCRIPTOR_RANGE fxaa_root_descriptor_range_dest;
  fxaa_root_descriptor_range_dest.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
  fxaa_root_descriptor_range_dest.NumDescriptors = 1;
  fxaa_root_descriptor_range_dest.BaseShaderRegister = 0;
  fxaa_root_descriptor_range_dest.RegisterSpace = 0;
  fxaa_root_descriptor_range_dest.OffsetInDescriptorsFromTableStart = 0;
  {
    D3D12_ROOT_PARAMETER& fxaa_root_parameter_dest =
        fxaa_root_parameters[UINT(FxaaRootParameter::kDestination)];
    fxaa_root_parameter_dest.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    fxaa_root_parameter_dest.DescriptorTable.NumDescriptorRanges = 1;
    fxaa_root_parameter_dest.DescriptorTable.pDescriptorRanges = &fxaa_root_descriptor_range_dest;
    fxaa_root_parameter_dest.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  }
  D3D12_DESCRIPTOR_RANGE fxaa_root_descriptor_range_source;
  fxaa_root_descriptor_range_source.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
  fxaa_root_descriptor_range_source.NumDescriptors = 1;
  fxaa_root_descriptor_range_source.BaseShaderRegister = 0;
  fxaa_root_descriptor_range_source.RegisterSpace = 0;
  fxaa_root_descriptor_range_source.OffsetInDescriptorsFromTableStart = 0;
  {
    D3D12_ROOT_PARAMETER& fxaa_root_parameter_source =
        fxaa_root_parameters[UINT(FxaaRootParameter::kSource)];
    fxaa_root_parameter_source.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    fxaa_root_parameter_source.DescriptorTable.NumDescriptorRanges = 1;
    fxaa_root_parameter_source.DescriptorTable.pDescriptorRanges =
        &fxaa_root_descriptor_range_source;
    fxaa_root_parameter_source.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  }
  D3D12_STATIC_SAMPLER_DESC fxaa_root_sampler;
  fxaa_root_sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
  fxaa_root_sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
  fxaa_root_sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
  fxaa_root_sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
  fxaa_root_sampler.MipLODBias = 0.0f;
  fxaa_root_sampler.MaxAnisotropy = 1;
  fxaa_root_sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
  fxaa_root_sampler.BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_BLACK;
  fxaa_root_sampler.MinLOD = 0.0f;
  fxaa_root_sampler.MaxLOD = 0.0f;
  fxaa_root_sampler.ShaderRegister = 0;
  fxaa_root_sampler.RegisterSpace = 0;
  fxaa_root_sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  D3D12_ROOT_SIGNATURE_DESC fxaa_root_signature_desc;
  fxaa_root_signature_desc.NumParameters = UINT(FxaaRootParameter::kCount);
  fxaa_root_signature_desc.pParameters = fxaa_root_parameters;
  fxaa_root_signature_desc.NumStaticSamplers = 1;
  fxaa_root_signature_desc.pStaticSamplers = &fxaa_root_sampler;
  fxaa_root_signature_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;
  *(fxaa_root_signature_.ReleaseAndGetAddressOf()) =
      ui::d3d12::util::CreateRootSignature(provider, fxaa_root_signature_desc);
  if (!fxaa_root_signature_) {
    REXGPU_ERROR("Failed to create the FXAA root signature");
    return false;
  }
  *(fxaa_pipeline_.ReleaseAndGetAddressOf()) = ui::d3d12::util::CreateComputePipeline(
      device, shaders::fxaa_cs, sizeof(shaders::fxaa_cs), fxaa_root_signature_.Get());
  if (!fxaa_pipeline_) {
    REXGPU_ERROR("Failed to create the FXAA compute pipeline");
    return false;
  }
  *(fxaa_extreme_pipeline_.ReleaseAndGetAddressOf()) = ui::d3d12::util::CreateComputePipeline(
      device, shaders::fxaa_extreme_cs, sizeof(shaders::fxaa_extreme_cs),
      fxaa_root_signature_.Get());
  if (!fxaa_pipeline_) {
    REXGPU_ERROR("Failed to create the extreme-quality FXAA compute pipeline");
    return false;
  }

  // SMAA 1x: constants as FXAA's (size, 1 / size), three single-descriptor
  // source tables, one destination, linear and point clamped samplers.
  {
    D3D12_ROOT_PARAMETER smaa_root_parameters[UINT(SmaaRootParameter::kCount)];
    D3D12_ROOT_PARAMETER& smaa_constants =
        smaa_root_parameters[UINT(SmaaRootParameter::kConstants)];
    smaa_constants.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    smaa_constants.Constants.ShaderRegister = 0;
    smaa_constants.Constants.RegisterSpace = 0;
    smaa_constants.Constants.Num32BitValues = sizeof(FxaaConstants) / sizeof(uint32_t);
    smaa_constants.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_DESCRIPTOR_RANGE smaa_ranges[4];
    for (uint32_t i = 0; i < 4; ++i) {
      D3D12_DESCRIPTOR_RANGE& range = smaa_ranges[i];
      range.RangeType = i < 3 ? D3D12_DESCRIPTOR_RANGE_TYPE_SRV : D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
      range.NumDescriptors = 1;
      range.BaseShaderRegister = i < 3 ? i : 0;
      range.RegisterSpace = 0;
      range.OffsetInDescriptorsFromTableStart = 0;
      D3D12_ROOT_PARAMETER& parameter =
          smaa_root_parameters[UINT(SmaaRootParameter::kSource0) + i];
      parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
      parameter.DescriptorTable.NumDescriptorRanges = 1;
      parameter.DescriptorTable.pDescriptorRanges = &range;
      parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    }
    D3D12_STATIC_SAMPLER_DESC smaa_samplers[2];
    for (uint32_t i = 0; i < 2; ++i) {
      D3D12_STATIC_SAMPLER_DESC& sampler = smaa_samplers[i];
      sampler = fxaa_root_sampler;
      sampler.Filter = i ? D3D12_FILTER_MIN_MAG_MIP_POINT : D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT;
      sampler.ShaderRegister = i;
    }
    D3D12_ROOT_SIGNATURE_DESC smaa_root_signature_desc;
    smaa_root_signature_desc.NumParameters = UINT(SmaaRootParameter::kCount);
    smaa_root_signature_desc.pParameters = smaa_root_parameters;
    smaa_root_signature_desc.NumStaticSamplers = 2;
    smaa_root_signature_desc.pStaticSamplers = smaa_samplers;
    smaa_root_signature_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;
    *(smaa_root_signature_.ReleaseAndGetAddressOf()) =
        ui::d3d12::util::CreateRootSignature(provider, smaa_root_signature_desc);
    if (!smaa_root_signature_) {
      REXGPU_ERROR("Failed to create the SMAA root signature");
      return false;
    }
    *(smaa_edge_detection_pipeline_.ReleaseAndGetAddressOf()) =
        ui::d3d12::util::CreateComputePipeline(device, shaders::smaa_edge_detection_cs,
                                               sizeof(shaders::smaa_edge_detection_cs),
                                               smaa_root_signature_.Get());
    *(smaa_blending_weight_pipeline_.ReleaseAndGetAddressOf()) =
        ui::d3d12::util::CreateComputePipeline(device, shaders::smaa_blending_weight_cs,
                                               sizeof(shaders::smaa_blending_weight_cs),
                                               smaa_root_signature_.Get());
    *(smaa_neighborhood_blending_pipeline_.ReleaseAndGetAddressOf()) =
        ui::d3d12::util::CreateComputePipeline(device, shaders::smaa_neighborhood_blending_cs,
                                               sizeof(shaders::smaa_neighborhood_blending_cs),
                                               smaa_root_signature_.Get());
    if (!smaa_edge_detection_pipeline_ || !smaa_blending_weight_pipeline_ ||
        !smaa_neighborhood_blending_pipeline_) {
      REXGPU_ERROR("Failed to create the SMAA compute pipelines");
      return false;
    }
  }

  // Resolve downscale compute pipeline for scaled readback resolve.
  D3D12_ROOT_PARAMETER
  resolve_downscale_root_parameters[UINT(ResolveDownscaleRootParameter::kCount)];
  {
    D3D12_ROOT_PARAMETER& constants_parameter =
        resolve_downscale_root_parameters[UINT(ResolveDownscaleRootParameter::kConstants)];
    constants_parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    constants_parameter.Constants.ShaderRegister = 0;
    constants_parameter.Constants.RegisterSpace = 0;
    constants_parameter.Constants.Num32BitValues =
        sizeof(ResolveDownscaleConstants) / sizeof(uint32_t);
    constants_parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  }
  D3D12_DESCRIPTOR_RANGE resolve_downscale_source_range;
  resolve_downscale_source_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
  resolve_downscale_source_range.NumDescriptors = 1;
  resolve_downscale_source_range.BaseShaderRegister = 0;
  resolve_downscale_source_range.RegisterSpace = 0;
  resolve_downscale_source_range.OffsetInDescriptorsFromTableStart = 0;
  {
    D3D12_ROOT_PARAMETER& source_parameter =
        resolve_downscale_root_parameters[UINT(ResolveDownscaleRootParameter::kSource)];
    source_parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    source_parameter.DescriptorTable.NumDescriptorRanges = 1;
    source_parameter.DescriptorTable.pDescriptorRanges = &resolve_downscale_source_range;
    source_parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  }
  D3D12_DESCRIPTOR_RANGE resolve_downscale_destination_range;
  resolve_downscale_destination_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
  resolve_downscale_destination_range.NumDescriptors = 1;
  resolve_downscale_destination_range.BaseShaderRegister = 0;
  resolve_downscale_destination_range.RegisterSpace = 0;
  resolve_downscale_destination_range.OffsetInDescriptorsFromTableStart = 0;
  {
    D3D12_ROOT_PARAMETER& destination_parameter =
        resolve_downscale_root_parameters[UINT(ResolveDownscaleRootParameter::kDestination)];
    destination_parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    destination_parameter.DescriptorTable.NumDescriptorRanges = 1;
    destination_parameter.DescriptorTable.pDescriptorRanges = &resolve_downscale_destination_range;
    destination_parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  }
  D3D12_ROOT_SIGNATURE_DESC resolve_downscale_root_signature_desc;
  resolve_downscale_root_signature_desc.NumParameters = UINT(ResolveDownscaleRootParameter::kCount);
  resolve_downscale_root_signature_desc.pParameters = resolve_downscale_root_parameters;
  resolve_downscale_root_signature_desc.NumStaticSamplers = 0;
  resolve_downscale_root_signature_desc.pStaticSamplers = nullptr;
  resolve_downscale_root_signature_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;
  *(resolve_downscale_root_signature_.ReleaseAndGetAddressOf()) =
      ui::d3d12::util::CreateRootSignature(provider, resolve_downscale_root_signature_desc);
  if (resolve_downscale_root_signature_) {
    *(resolve_downscale_pipeline_.ReleaseAndGetAddressOf()) =
        ui::d3d12::util::CreateComputePipeline(device, shaders::resolve_downscale_cs,
                                               sizeof(shaders::resolve_downscale_cs),
                                               resolve_downscale_root_signature_.Get());
  }
  if (!resolve_downscale_root_signature_ || !resolve_downscale_pipeline_) {
    resolve_downscale_pipeline_.Reset();
    resolve_downscale_root_signature_.Reset();
    REXGPU_WARN("Failed to initialize D3D12 resolve-downscale readback pipeline");
  }

  if (bindless_resources_used_) {
    // Create the system bindless descriptors once all resources are
    // initialized.
    // kNullRawSRV.
    ui::d3d12::util::CreateBufferRawSRV(
        device,
        provider.OffsetViewDescriptor(view_bindless_heap_cpu_start_,
                                      uint32_t(SystemBindlessView::kNullRawSRV)),
        nullptr, 0);
    // kNullRawUAV.
    ui::d3d12::util::CreateBufferRawUAV(
        device,
        provider.OffsetViewDescriptor(view_bindless_heap_cpu_start_,
                                      uint32_t(SystemBindlessView::kNullRawUAV)),
        nullptr, 0);
    // kNullTexture2DArray.
    D3D12_SHADER_RESOURCE_VIEW_DESC null_srv_desc;
    null_srv_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    null_srv_desc.Shader4ComponentMapping = D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING(
        D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_0, D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_0,
        D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_0, D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_0);
    null_srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
    null_srv_desc.Texture2DArray.MostDetailedMip = 0;
    null_srv_desc.Texture2DArray.MipLevels = 1;
    null_srv_desc.Texture2DArray.FirstArraySlice = 0;
    null_srv_desc.Texture2DArray.ArraySize = 1;
    null_srv_desc.Texture2DArray.PlaneSlice = 0;
    null_srv_desc.Texture2DArray.ResourceMinLODClamp = 0.0f;
    device->CreateShaderResourceView(
        nullptr, &null_srv_desc,
        provider.OffsetViewDescriptor(view_bindless_heap_cpu_start_,
                                      uint32_t(SystemBindlessView::kNullTexture2DArray)));
    // kNullTexture3D.
    null_srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
    null_srv_desc.Texture3D.MostDetailedMip = 0;
    null_srv_desc.Texture3D.MipLevels = 1;
    null_srv_desc.Texture3D.ResourceMinLODClamp = 0.0f;
    device->CreateShaderResourceView(
        nullptr, &null_srv_desc,
        provider.OffsetViewDescriptor(view_bindless_heap_cpu_start_,
                                      uint32_t(SystemBindlessView::kNullTexture3D)));
    // kNullTextureCube.
    null_srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
    null_srv_desc.TextureCube.MostDetailedMip = 0;
    null_srv_desc.TextureCube.MipLevels = 1;
    null_srv_desc.TextureCube.ResourceMinLODClamp = 0.0f;
    device->CreateShaderResourceView(
        nullptr, &null_srv_desc,
        provider.OffsetViewDescriptor(view_bindless_heap_cpu_start_,
                                      uint32_t(SystemBindlessView::kNullTextureCube)));
    // kSharedMemoryRawSRV.
    shared_memory_->WriteRawSRVDescriptor(provider.OffsetViewDescriptor(
        view_bindless_heap_cpu_start_, uint32_t(SystemBindlessView::kSharedMemoryRawSRV)));
    // kSharedMemoryR32UintSRV.
    shared_memory_->WriteUintPow2SRVDescriptor(
        provider.OffsetViewDescriptor(view_bindless_heap_cpu_start_,
                                      uint32_t(SystemBindlessView::kSharedMemoryR32UintSRV)),
        2);
    // kSharedMemoryR32G32UintSRV.
    shared_memory_->WriteUintPow2SRVDescriptor(
        provider.OffsetViewDescriptor(view_bindless_heap_cpu_start_,
                                      uint32_t(SystemBindlessView::kSharedMemoryR32G32UintSRV)),
        3);
    // kSharedMemoryR32G32B32A32UintSRV.
    shared_memory_->WriteUintPow2SRVDescriptor(
        provider.OffsetViewDescriptor(
            view_bindless_heap_cpu_start_,
            uint32_t(SystemBindlessView::kSharedMemoryR32G32B32A32UintSRV)),
        4);
    // kSharedMemoryRawUAV.
    shared_memory_->WriteRawUAVDescriptor(provider.OffsetViewDescriptor(
        view_bindless_heap_cpu_start_, uint32_t(SystemBindlessView::kSharedMemoryRawUAV)));
    // kSharedMemoryR32UintUAV.
    shared_memory_->WriteUintPow2UAVDescriptor(
        provider.OffsetViewDescriptor(view_bindless_heap_cpu_start_,
                                      uint32_t(SystemBindlessView::kSharedMemoryR32UintUAV)),
        2);
    // kSharedMemoryR32G32UintUAV.
    shared_memory_->WriteUintPow2UAVDescriptor(
        provider.OffsetViewDescriptor(view_bindless_heap_cpu_start_,
                                      uint32_t(SystemBindlessView::kSharedMemoryR32G32UintUAV)),
        3);
    // kSharedMemoryR32G32B32A32UintUAV.
    shared_memory_->WriteUintPow2UAVDescriptor(
        provider.OffsetViewDescriptor(
            view_bindless_heap_cpu_start_,
            uint32_t(SystemBindlessView::kSharedMemoryR32G32B32A32UintUAV)),
        4);
    // kEdramRawSRV.
    render_target_cache_->WriteEdramRawSRVDescriptor(provider.OffsetViewDescriptor(
        view_bindless_heap_cpu_start_, uint32_t(SystemBindlessView::kEdramRawSRV)));
    // kEdramR32UintSRV.
    render_target_cache_->WriteEdramUintPow2SRVDescriptor(
        provider.OffsetViewDescriptor(view_bindless_heap_cpu_start_,
                                      uint32_t(SystemBindlessView::kEdramR32UintSRV)),
        2);
    // kEdramR32G32UintSRV.
    render_target_cache_->WriteEdramUintPow2SRVDescriptor(
        provider.OffsetViewDescriptor(view_bindless_heap_cpu_start_,
                                      uint32_t(SystemBindlessView::kEdramR32G32UintSRV)),
        3);
    // kEdramR32G32B32A32UintSRV.
    render_target_cache_->WriteEdramUintPow2SRVDescriptor(
        provider.OffsetViewDescriptor(view_bindless_heap_cpu_start_,
                                      uint32_t(SystemBindlessView::kEdramR32G32B32A32UintSRV)),
        4);
    // kEdramRawUAV.
    render_target_cache_->WriteEdramRawUAVDescriptor(provider.OffsetViewDescriptor(
        view_bindless_heap_cpu_start_, uint32_t(SystemBindlessView::kEdramRawUAV)));
    // kEdramR32UintUAV.
    render_target_cache_->WriteEdramUintPow2UAVDescriptor(
        provider.OffsetViewDescriptor(view_bindless_heap_cpu_start_,
                                      uint32_t(SystemBindlessView::kEdramR32UintUAV)),
        2);
    // kEdramR32G32UintUAV.
    render_target_cache_->WriteEdramUintPow2UAVDescriptor(
        provider.OffsetViewDescriptor(view_bindless_heap_cpu_start_,
                                      uint32_t(SystemBindlessView::kEdramR32G32UintUAV)),
        3);
    // kEdramR32G32B32A32UintUAV.
    render_target_cache_->WriteEdramUintPow2UAVDescriptor(
        provider.OffsetViewDescriptor(view_bindless_heap_cpu_start_,
                                      uint32_t(SystemBindlessView::kEdramR32G32B32A32UintUAV)),
        4);
    // kGammaRampTableSRV.
    WriteGammaRampSRV(
        false, provider.OffsetViewDescriptor(view_bindless_heap_cpu_start_,
                                             uint32_t(SystemBindlessView::kGammaRampTableSRV)));
    // kGammaRampPWLSRV.
    WriteGammaRampSRV(
        true, provider.OffsetViewDescriptor(view_bindless_heap_cpu_start_,
                                            uint32_t(SystemBindlessView::kGammaRampPWLSRV)));
  }

  pix_capture_requested_.store(false, std::memory_order_relaxed);
  pix_capturing_ = false;
  occlusion_query_resources_available_ = InitializeOcclusionQueryResources();
  InitializeGpuTiming();
  InitializeGpuFrameMeter();
  embedded_scene_host_vertex_output_resources_available_ =
      REXCVAR_GET(d3d12_embedded_scene_host_vertex_output_diagnostic) &&
      InitializeEmbeddedSceneHostVertexOutputResources();

  // Just not to expose uninitialized memory.
  std::memset(&system_constants_, 0, sizeof(system_constants_));

  TemporalAaInitialize();
  return true;
}

void D3D12CommandProcessor::ShutdownContext() {
  AwaitAllQueueOperationsCompletion();
  if (presenter_completion_deferred_) {
    // Run pending presenter completions while the presenter is alive, then
    // return it to completing inline.
    DrainSubmissions();
    if (graphics_system_ && graphics_system_->presenter()) {
      graphics_system_->presenter()->SetGuestOutputCompletionExecutor(nullptr);
    }
    presenter_completion_deferred_ = false;
  }
  StopSubmissionThread();
  TemporalAaShutdown();
  InvalidateAllVertexBufferResidency();
  ShutdownEmbeddedSceneHostVertexOutputResources();
  ShutdownOcclusionQueryResources();
  if (gpu_timing_ && gpu_timing_->readback) {
    gpu_timing_->readback->Unmap(0, nullptr);
  }
  gpu_timing_.reset();

  ui::d3d12::util::ReleaseAndNull(readback_buffer_);
  readback_buffer_size_ = 0;
  for (auto& resolve_readback_pair : readback_buffers_) {
    auto& readback = resolve_readback_pair.second;
    for (uint32_t i = 0; i < 2; ++i) {
      if (readback.buffers[i]) {
        if (readback.mapped_data[i]) {
          readback.buffers[i]->Unmap(0, nullptr);
          readback.mapped_data[i] = nullptr;
        }
        readback.buffers[i]->Release();
        readback.buffers[i] = nullptr;
      }
      readback.sizes[i] = 0;
      readback.submission_written[i] = 0;
      readback.written_size[i] = 0;
    }
  }
  readback_buffers_.clear();
  for (auto& memexport_readback_pair : memexport_readback_buffers_) {
    auto& readback = memexport_readback_pair.second;
    for (uint32_t i = 0; i < 2; ++i) {
      if (readback.buffers[i]) {
        if (readback.mapped_data[i]) {
          readback.buffers[i]->Unmap(0, nullptr);
          readback.mapped_data[i] = nullptr;
        }
        readback.buffers[i]->Release();
        readback.buffers[i] = nullptr;
      }
      readback.sizes[i] = 0;
      readback.submission_written[i] = 0;
      readback.written_size[i] = 0;
    }
  }
  memexport_readback_buffers_.clear();

  ui::d3d12::util::ReleaseAndNull(scratch_buffer_);
  scratch_buffer_size_ = 0;
  resolve_downscale_buffer_size_ = 0;
  resolve_downscale_buffer_.Reset();

  for (const std::pair<uint64_t, ID3D12Resource*>& resource_for_deletion :
       resources_for_deletion_) {
    resource_for_deletion.second->Release();
  }
  resources_for_deletion_.clear();

  fxaa_source_texture_submission_ = 0;
  fxaa_source_texture_.Reset();

  smaa_textures_submission_ = 0;
  smaa_blend_texture_.Reset();
  smaa_edges_texture_.Reset();
  smaa_color_texture_.Reset();
  smaa_lookup_textures_uploaded_ = false;
  smaa_search_texture_.Reset();
  smaa_area_texture_.Reset();
  smaa_neighborhood_blending_pipeline_.Reset();
  smaa_blending_weight_pipeline_.Reset();
  smaa_edge_detection_pipeline_.Reset();
  smaa_root_signature_.Reset();

  fxaa_extreme_pipeline_.Reset();
  fxaa_pipeline_.Reset();
  fxaa_root_signature_.Reset();
  resolve_downscale_pipeline_.Reset();
  resolve_downscale_root_signature_.Reset();

  apply_gamma_pwl_fxaa_luma_pipeline_.Reset();
  apply_gamma_pwl_pipeline_.Reset();
  apply_gamma_table_fxaa_luma_pipeline_.Reset();
  apply_gamma_table_pipeline_.Reset();
  apply_gamma_root_signature_.Reset();

  // Unmapping will be done implicitly by the destruction.
  gamma_ramp_upload_buffer_mapping_ = nullptr;
  gamma_ramp_upload_buffer_.Reset();
  gamma_ramp_buffer_.Reset();

  texture_cache_.reset();

  pipeline_cache_.reset();

  primitive_processor_.reset();

  // Shut down binding - bindless descriptors may be owned by subsystems like
  // the texture cache.

  // Root signatures are used by pipelines, thus freed after the pipelines.
  ui::d3d12::util::ReleaseAndNull(root_signature_bindless_ds_);
  ui::d3d12::util::ReleaseAndNull(root_signature_bindless_vs_);
  for (auto it : root_signatures_bindful_) {
    it.second->Release();
  }
  root_signatures_bindful_.clear();

  if (bindless_resources_used_) {
    texture_cache_bindless_sampler_map_.clear();
    for (const auto& sampler_bindless_heap_overflowed : sampler_bindless_heaps_overflowed_) {
      sampler_bindless_heap_overflowed.first->Release();
    }
    sampler_bindless_heaps_overflowed_.clear();
    sampler_bindless_heap_allocated_ = 0;
    ui::d3d12::util::ReleaseAndNull(sampler_bindless_heap_current_);
    view_bindless_one_use_descriptors_.clear();
    view_bindless_heap_free_.clear();
    ui::d3d12::util::ReleaseAndNull(view_bindless_heap_);
  } else {
    sampler_bindful_heap_pool_.reset();
    view_bindful_heap_pool_.reset();
  }
  constant_buffer_pool_.reset();

  render_target_cache_.reset();

  shared_memory_.reset();

  deferred_command_list_.Reset();
  ui::d3d12::util::ReleaseAndNull(command_list_1_);
  ui::d3d12::util::ReleaseAndNull(command_list_);
  ClearCommandAllocatorCache();

  frame_open_ = false;
  frame_current_ = 1;
  frame_completed_ = 0;
  std::memset(closed_frame_submissions_, 0, sizeof(closed_frame_submissions_));

  // First release the fences since they may reference fence_completion_event_.

  queue_operations_done_since_submission_signal_ = false;
  queue_operations_since_submission_fence_last_ = 0;
  ui::d3d12::util::ReleaseAndNull(queue_operations_since_submission_fence_);

  ui::d3d12::util::ReleaseAndNull(submission_fence_);
  submission_open_ = false;
  submission_current_ = 1;
  submission_completed_ = 0;

  if (fence_completion_event_) {
    CloseHandle(fence_completion_event_);
    fence_completion_event_ = nullptr;
  }

  device_removed_ = false;

  CommandProcessor::ShutdownContext();
}

void D3D12CommandProcessor::WriteRegister(uint32_t index, uint32_t value) {
  CommandProcessor::WriteRegister(index, value);

  if (kGpuDiagnostics && track_ring_publications_ && index >= XE_GPU_REG_SHADER_CONSTANT_000_X &&
      index < XE_GPU_REG_SHADER_CONSTANT_000_X + kEmbeddedTrackedFloatConstantDwords) {
    EmbeddedFloatConstantWrite& write =
        embedded_float_constant_writes_[index - XE_GPU_REG_SHADER_CONSTANT_000_X];
    write.sequence = ++embedded_float_constant_write_sequence_;
    write.value = value;
    write.physical_address = UINT32_MAX;
    write.packet_physical = UINT32_MAX;
    write.bulk = false;
    write.execution = {};
    if (write.camera_source.packet) write.camera_source = {};
  }

  if (index >= XE_GPU_REG_SHADER_CONSTANT_000_X && index <= XE_GPU_REG_SHADER_CONSTANT_511_W) {
    if (frame_open_) {
      uint32_t float_constant_index = (index - XE_GPU_REG_SHADER_CONSTANT_000_X) >> 2;
      if (float_constant_index >= 256) {
        float_constant_index -= 256;
        if (current_float_constant_map_pixel_[float_constant_index >> 6] &
            (1ull << (float_constant_index & 63))) {
          cbuffer_binding_float_pixel_.up_to_date = false;
        }
      } else {
        if (current_float_constant_map_vertex_[float_constant_index >> 6] &
            (1ull << (float_constant_index & 63))) {
          cbuffer_binding_float_vertex_.up_to_date = false;
        }
      }
    }
  } else if (index >= XE_GPU_REG_SHADER_CONSTANT_BOOL_000_031 &&
             index <= XE_GPU_REG_SHADER_CONSTANT_LOOP_31) {
    cbuffer_binding_bool_loop_.up_to_date = false;
  } else if (index >= XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 &&
             index <= XE_GPU_REG_SHADER_CONSTANT_FETCH_31_5) {
    cbuffer_binding_fetch_.up_to_date = false;
    if (texture_cache_ != nullptr) {
      texture_cache_->TextureFetchConstantWritten((index - XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0) /
                                                  6);
    }
    InvalidateVertexBufferResidency((index - XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0) / 2);
  }
}

void D3D12CommandProcessor::WriteRegisterFromPacket(uint32_t index, uint32_t value,
                                                    uint32_t packet, uint32_t data,
                                                    const pc_owned_camera_packet::Source* source) {
  WriteRegister(index, value);
  if (kGpuDiagnostics && track_ring_publications_ && index >= XE_GPU_REG_SHADER_CONSTANT_000_X &&
      index < XE_GPU_REG_SHADER_CONSTANT_000_X + kEmbeddedTrackedFloatConstantDwords) {
    auto& write = embedded_float_constant_writes_[index - XE_GPU_REG_SHADER_CONSTANT_000_X];
    write.packet_physical = packet;
    write.physical_address = data;
    write.execution = command_execution_.Current();
    if (source && source->Covers(index) && write.execution.Valid())
      write.camera_source = *source;
  }
}

void D3D12CommandProcessor::InvalidateRegisterProvenance() {
  if (track_ring_publications_) {
    embedded_float_constant_writes_ = {};
  }
  // Keep write sequence monotonic across restores and ring replacement.
}

void D3D12CommandProcessor::WriteRegistersFromMem(uint32_t start_index, uint32_t* base,
                                                  uint32_t num_registers) {
  if (!num_registers) {
    return;
  }
  uint32_t end_index = start_index + num_registers - 1;

  auto range_has_any_constant_usage = [](const uint64_t* usage_map, uint32_t first_constant,
                                         uint32_t last_constant) -> bool {
    if (first_constant > last_constant) {
      return false;
    }
    uint32_t first_word = first_constant >> 6;
    uint32_t last_word = last_constant >> 6;
    uint32_t first_bit = first_constant & 63;
    uint32_t last_bit = last_constant & 63;
    if (first_word == last_word) {
      uint32_t bit_count = last_bit - first_bit + 1;
      uint64_t mask = bit_count == 64 ? UINT64_MAX : ((UINT64_C(1) << bit_count) - 1) << first_bit;
      return (usage_map[first_word] & mask) != 0;
    }
    if (usage_map[first_word] & (UINT64_MAX << first_bit)) {
      return true;
    }
    for (uint32_t word = first_word + 1; word < last_word; ++word) {
      if (usage_map[word]) {
        return true;
      }
    }
    uint64_t last_mask = last_bit == 63 ? UINT64_MAX : ((UINT64_C(1) << (last_bit + 1)) - 1);
    return (usage_map[last_word] & last_mask) != 0;
  };

  if (start_index >= XE_GPU_REG_SHADER_CONSTANT_000_X &&
      end_index <= XE_GPU_REG_SHADER_CONSTANT_511_W) {
    memory::copy_and_swap(register_file_->values + start_index, base, num_registers);
    if (kGpuDiagnostics && track_ring_publications_) {
      const uint32_t tracked_start = XE_GPU_REG_SHADER_CONSTANT_000_X;
      const uint32_t tracked_end = tracked_start + kEmbeddedTrackedFloatConstantDwords - 1;
      const uint32_t overlap_start = std::max(start_index, tracked_start);
      const uint32_t overlap_end = std::min(end_index, tracked_end);
      if (overlap_start <= overlap_end) {
        const uintptr_t base_address = reinterpret_cast<uintptr_t>(base);
        const uintptr_t physical_base = reinterpret_cast<uintptr_t>(memory_->physical_membase());
        for (uint32_t register_index = overlap_start; register_index <= overlap_end;
             ++register_index) {
          EmbeddedFloatConstantWrite& write =
              embedded_float_constant_writes_[register_index - tracked_start];
          write.sequence = ++embedded_float_constant_write_sequence_;
          write.value = register_file_->values[register_index];
          const uintptr_t source_address =
              base_address + uintptr_t(register_index - start_index) * sizeof(uint32_t);
          write.physical_address = pc_constant_writer::PhysicalWord(
              source_address, physical_base, SharedMemory::kBufferSize);
          write.packet_physical = UINT32_MAX;
          write.bulk = true;
          write.execution = command_execution_.Current();
          if (write.camera_source.packet) write.camera_source = {};
        }
      }
    }
    if (frame_open_) {
      uint32_t first_float_constant = (start_index - XE_GPU_REG_SHADER_CONSTANT_000_X) >> 2;
      uint32_t last_float_constant = (end_index - XE_GPU_REG_SHADER_CONSTANT_000_X) >> 2;
      if (first_float_constant < 256) {
        uint32_t last_vertex_constant = std::min(last_float_constant, 255u);
        if (range_has_any_constant_usage(current_float_constant_map_vertex_, first_float_constant,
                                         last_vertex_constant)) {
          cbuffer_binding_float_vertex_.up_to_date = false;
        }
      }
      if (last_float_constant >= 256) {
        uint32_t first_pixel_constant =
            first_float_constant >= 256 ? first_float_constant - 256 : 0;
        uint32_t last_pixel_constant = last_float_constant - 256;
        if (range_has_any_constant_usage(current_float_constant_map_pixel_, first_pixel_constant,
                                         last_pixel_constant)) {
          cbuffer_binding_float_pixel_.up_to_date = false;
        }
      }
    }
    return;
  }

  if (start_index >= XE_GPU_REG_SHADER_CONSTANT_BOOL_000_031 &&
      end_index <= XE_GPU_REG_SHADER_CONSTANT_LOOP_31) {
    memory::copy_and_swap(register_file_->values + start_index, base, num_registers);
    cbuffer_binding_bool_loop_.up_to_date = false;
    return;
  }

  if (start_index >= XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 &&
      end_index <= XE_GPU_REG_SHADER_CONSTANT_FETCH_31_5) {
    memory::copy_and_swap(register_file_->values + start_index, base, num_registers);
    cbuffer_binding_fetch_.up_to_date = false;
    uint32_t first_fetch_dword = start_index - XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0;
    uint32_t last_fetch_dword = end_index - XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0;
    if (texture_cache_) {
      texture_cache_->TextureFetchConstantsWritten(first_fetch_dword / 6, last_fetch_dword / 6);
    }
    InvalidateVertexBufferResidencyRange(first_fetch_dword / 2, last_fetch_dword / 2);
    return;
  }

  CommandProcessor::WriteRegistersFromMem(start_index, base, num_registers);
}

void D3D12CommandProcessor::OnGammaRamp256EntryTableValueWritten() {
  gamma_ramp_256_entry_table_up_to_date_ = false;
}

void D3D12CommandProcessor::OnGammaRampPWLValueWritten() {
  gamma_ramp_pwl_up_to_date_ = false;
}

bool D3D12CommandProcessor::QueryCadenceSubmission(uint64_t& submitted,
                                                 uint64_t& completed) {
  submitted = submission_current_ ? submission_current_ - 1 : 0;
  completed = submission_fence_ ? submission_fence_->GetCompletedValue()
                                : submission_completed_;
  return submission_fence_ != nullptr && completed != UINT64_MAX;
}

bool D3D12CommandProcessor::EnsureSmaaResources(uint32_t width, uint32_t height) {
  const ui::d3d12::D3D12Provider& provider = GetD3D12Provider();
  ID3D12Device* device = provider.GetDevice();
  auto create_texture = [&](uint32_t texture_width, uint32_t texture_height, DXGI_FORMAT format,
                            bool uav, D3D12_RESOURCE_STATES state,
                            Microsoft::WRL::ComPtr<ID3D12Resource>& texture) {
    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = texture_width;
    desc.Height = texture_height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags = uav ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE;
    return SUCCEEDED(device->CreateCommittedResource(
        &ui::d3d12::util::kHeapPropertiesDefault, provider.GetHeapFlagCreateNotZeroed(), &desc,
        state, nullptr, IID_PPV_ARGS(texture.ReleaseAndGetAddressOf())));
  };

  // The lookup tables, copied once from an upload buffer that lives until the
  // copying submission completes.
  if (!smaa_lookup_textures_uploaded_) {
    if (!create_texture(AREATEX_WIDTH, AREATEX_HEIGHT, DXGI_FORMAT_R8G8_UNORM, false,
                        D3D12_RESOURCE_STATE_COPY_DEST, smaa_area_texture_) ||
        !create_texture(SEARCHTEX_WIDTH, SEARCHTEX_HEIGHT, DXGI_FORMAT_R8_UNORM, false,
                        D3D12_RESOURCE_STATE_COPY_DEST, smaa_search_texture_)) {
      REXGPU_ERROR("Failed to create the SMAA lookup textures");
      smaa_area_texture_.Reset();
      smaa_search_texture_.Reset();
      return false;
    }
    const uint32_t area_pitch =
        rex::align(uint32_t(AREATEX_PITCH), uint32_t(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT));
    const uint32_t search_pitch =
        rex::align(uint32_t(SEARCHTEX_PITCH), uint32_t(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT));
    const uint32_t search_offset = rex::align(area_pitch * uint32_t(AREATEX_HEIGHT),
                                              uint32_t(D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT));
    D3D12_RESOURCE_DESC upload_desc;
    ui::d3d12::util::FillBufferResourceDesc(
        upload_desc, search_offset + search_pitch * uint32_t(SEARCHTEX_HEIGHT),
        D3D12_RESOURCE_FLAG_NONE);
    Microsoft::WRL::ComPtr<ID3D12Resource> upload;
    uint8_t* mapping = nullptr;
    const D3D12_RANGE read_range = {};
    if (FAILED(device->CreateCommittedResource(
            &ui::d3d12::util::kHeapPropertiesUpload, provider.GetHeapFlagCreateNotZeroed(),
            &upload_desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&upload))) ||
        FAILED(upload->Map(0, &read_range, reinterpret_cast<void**>(&mapping)))) {
      REXGPU_ERROR("Failed to create the SMAA lookup texture upload buffer");
      return false;
    }
    for (uint32_t y = 0; y < uint32_t(AREATEX_HEIGHT); ++y) {
      std::memcpy(mapping + y * area_pitch, smaa_tables::areaTexBytes + y * AREATEX_PITCH,
                  AREATEX_PITCH);
    }
    for (uint32_t y = 0; y < uint32_t(SEARCHTEX_HEIGHT); ++y) {
      std::memcpy(mapping + search_offset + y * search_pitch,
                  smaa_tables::searchTexBytes + y * SEARCHTEX_PITCH, SEARCHTEX_PITCH);
    }
    upload->Unmap(0, nullptr);
    D3D12_TEXTURE_COPY_LOCATION source = {};
    source.pResource = upload.Get();
    source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    source.PlacedFootprint.Offset = 0;
    source.PlacedFootprint.Footprint = {DXGI_FORMAT_R8G8_UNORM, UINT(AREATEX_WIDTH),
                                        UINT(AREATEX_HEIGHT), 1, area_pitch};
    D3D12_TEXTURE_COPY_LOCATION dest = {};
    dest.pResource = smaa_area_texture_.Get();
    dest.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dest.SubresourceIndex = 0;
    SubmitBarriers();
    deferred_command_list_.D3DCopyTextureRegion(&dest, 0, 0, 0, &source, nullptr);
    source.PlacedFootprint.Offset = search_offset;
    source.PlacedFootprint.Footprint = {DXGI_FORMAT_R8_UNORM, UINT(SEARCHTEX_WIDTH),
                                        UINT(SEARCHTEX_HEIGHT), 1, search_pitch};
    dest.pResource = smaa_search_texture_.Get();
    deferred_command_list_.D3DCopyTextureRegion(&dest, 0, 0, 0, &source, nullptr);
    PushTransitionBarrier(smaa_area_texture_.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                          D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    PushTransitionBarrier(smaa_search_texture_.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                          D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    upload->AddRef();
    resources_for_deletion_.emplace_back(submission_current_, upload.Get());
    smaa_lookup_textures_uploaded_ = true;
  }

  // Guest-output-sized intermediates, recreated when the output size changes
  // (the old ones are released once no submission uses them).
  if (smaa_color_texture_) {
    const D3D12_RESOURCE_DESC desc = smaa_color_texture_->GetDesc();
    if (desc.Width != width || desc.Height != height) {
      // ComPtr overloads operator&, hence std::addressof.
      for (Microsoft::WRL::ComPtr<ID3D12Resource>* texture :
           {std::addressof(smaa_color_texture_), std::addressof(smaa_edges_texture_),
            std::addressof(smaa_blend_texture_)}) {
        if (*texture && submission_completed_ < smaa_textures_submission_) {
          (*texture)->AddRef();
          resources_for_deletion_.emplace_back(smaa_textures_submission_, texture->Get());
        }
        texture->Reset();
      }
      smaa_textures_submission_ = 0;
    }
  }
  if (!smaa_color_texture_ || !smaa_edges_texture_ || !smaa_blend_texture_) {
    if (!create_texture(width, height, ui::d3d12::D3D12Presenter::kGuestOutputFormat, true,
                        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, smaa_color_texture_) ||
        !create_texture(width, height, DXGI_FORMAT_R8G8_UNORM, true,
                        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, smaa_edges_texture_) ||
        !create_texture(width, height, DXGI_FORMAT_R8G8B8A8_UNORM, true,
                        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, smaa_blend_texture_)) {
      REXGPU_ERROR("Failed to create the SMAA intermediate textures");
      smaa_color_texture_.Reset();
      smaa_edges_texture_.Reset();
      smaa_blend_texture_.Reset();
      return false;
    }
  }
  return true;
}

void D3D12CommandProcessor::IssueSwap(uint32_t frontbuffer_ptr, uint32_t frontbuffer_width,
                                      uint32_t frontbuffer_height) {
  SCOPE_profile_cpu_f("gpu");
  const bool record_embedded_timing =
      kGpuDiagnostics && !kernel_state_ && REXCVAR_GET(embedded_hitch_diagnostics);
  const auto issue_swap_start =
      record_embedded_timing ? std::chrono::steady_clock::now()
                             : std::chrono::steady_clock::time_point{};
  vertex_buffers_in_sync_[0] = 0;
  vertex_buffers_in_sync_[1] = 0;
  if (temporal_aa_enabled_) TemporalAaEndFrame();
  if (embedded_frame_dump_file ||
      embedded_frame_dump_requested.load(std::memory_order_relaxed)) {
    EmbeddedFrameDumpAtSwap(guest_frame_count_.load(std::memory_order_relaxed),
                            render_target_cache_->GetDrawScaleX(),
                            render_target_cache_->GetDrawScaleY());
  }

  // Occlusion results of earlier frames reach guest memory before the guest
  // can reissue their slots (before this frame's own submission closes).
  BoundGuestOcclusionReportLag();

  if (!graphics_system_)
    return;
  ui::Presenter* presenter = graphics_system_->presenter();
  if (!presenter) {
    REXGPU_ERROR("IssueSwap: presenter is null");
    if (!kernel_state_) {
      std::fprintf(stderr,
                   "REX_EMBEDDED_SWAP_FAILURE reason=presenter_null frontbuffer=0x%08X size=%ux%u\n",
                   frontbuffer_ptr, frontbuffer_width, frontbuffer_height);
      std::fflush(stderr);
    }
    return;
  }

  // In case the swap command is the only one in the frame.
  if (!BeginSubmission(true)) {
    REXGPU_ERROR("IssueSwap: BeginSubmission failed");
    if (!kernel_state_) {
      std::fprintf(stderr, "REX_EMBEDDED_SWAP_FAILURE reason=begin_submission\n");
      std::fflush(stderr);
    }
    return;
  }
  GpuTimingScope swap_timing(*this, GpuTimingCategory::kSwap);

  if (kGpuDiagnostics && !kernel_state_ && embedded_swap_readback.source_resource &&
      submission_fence_->GetCompletedValue() >= embedded_swap_readback.submission) {
    struct EmbeddedReadbackStats {
      uint32_t hash = 2166136261u;
      uint64_t zero_bytes = 0;
      uint64_t nonzero_rgb_pixels = 0;
      uint64_t endpoint_low_channels = 0;
      uint64_t endpoint_high_channels = 0;
      uint64_t midrange_channels = 0;
      uint32_t channel_min = UINT32_MAX;
      uint32_t channel_max = 0;
      uint64_t captured_bytes = 0;
      bool mapped = false;
    };
    auto consume_readback = [&](ID3D12Resource* resource,
                                const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& footprint,
                                UINT rows, UINT64 row_size, UINT64 size,
                                const char* capture_path) -> EmbeddedReadbackStats {
      EmbeddedReadbackStats stats;
      if (!resource) {
        return stats;
      }
      D3D12_RANGE read_range = {0, SIZE_T(size)};
      void* mapping = nullptr;
      if (FAILED(resource->Map(0, &read_range, &mapping))) {
        return stats;
      }
      stats.mapped = true;
      const uint8_t* bytes = static_cast<const uint8_t*>(mapping) + footprint.Offset;
      const uint32_t width = footprint.Footprint.Width;
      const size_t bytes_per_pixel = width ? size_t(row_size) / width : 0;
      FILE* capture = std::fopen(capture_path, "wb");
      for (UINT row = 0; row < rows; ++row) {
        const uint8_t* row_bytes = bytes + size_t(row) * footprint.Footprint.RowPitch;
        for (size_t index = 0; index < row_size; ++index) {
          const uint8_t value = row_bytes[index];
          stats.hash = (stats.hash ^ value) * 16777619u;
          stats.zero_bytes += value == 0;
        }
        if (bytes_per_pixel == sizeof(uint32_t) &&
            footprint.Footprint.Format == DXGI_FORMAT_R10G10B10A2_UNORM) {
          for (uint32_t x = 0; x < width; ++x) {
            uint32_t pixel;
            std::memcpy(&pixel, row_bytes + size_t(x) * sizeof(pixel), sizeof(pixel));
            stats.nonzero_rgb_pixels += (pixel & UINT32_C(0x3FFFFFFF)) != 0;
            const uint32_t channels[] = {pixel & 0x3FF, (pixel >> 10) & 0x3FF,
                                         (pixel >> 20) & 0x3FF};
            for (uint32_t channel : channels) {
              stats.channel_min = std::min(stats.channel_min, channel);
              stats.channel_max = std::max(stats.channel_max, channel);
              stats.endpoint_low_channels += channel <= 1;
              stats.endpoint_high_channels += channel >= 1022;
              stats.midrange_channels += channel > 1 && channel < 1022;
            }
          }
        } else if (bytes_per_pixel >= 3) {
          for (uint32_t x = 0; x < width; ++x) {
            const uint8_t* pixel = row_bytes + size_t(x) * bytes_per_pixel;
            stats.nonzero_rgb_pixels += (pixel[0] | pixel[1] | pixel[2]) != 0;
          }
        }
        if (capture) {
          stats.captured_bytes += std::fwrite(row_bytes, 1, size_t(row_size), capture);
        }
      }
      if (capture) {
        std::fclose(capture);
      }
      D3D12_RANGE write_range = {0, 0};
      resource->Unmap(0, &write_range);
      return stats;
    };
    const char* source_capture_path = "rex_level_swap_pre_gamma.bin";
    const char* output_capture_path = "rex_level_swap_post_gamma.bin";
    EmbeddedReadbackStats source_stats = consume_readback(
        embedded_swap_readback.source_resource.Get(), embedded_swap_readback.source_footprint,
        embedded_swap_readback.source_rows, embedded_swap_readback.source_row_size,
        embedded_swap_readback.source_size, source_capture_path);
    EmbeddedReadbackStats output_stats = consume_readback(
        embedded_swap_readback.output_resource.Get(), embedded_swap_readback.output_footprint,
        embedded_swap_readback.output_rows, embedded_swap_readback.output_row_size,
        embedded_swap_readback.output_size, output_capture_path);
    std::fprintf(
        stderr,
        "REX_EMBEDDED_SWAP_PAIR result=%s frontbuffer=0x%08X state_hash=0x%016llX "
        "pre_hash=0x%08X pre_nonzero=%llu pre_zero_bytes=%llu pre_min=%u pre_max=%u "
        "pre_low=%llu pre_high=%llu pre_mid=%llu pre_capture=%s pre_captured=%llu "
        "post_hash=0x%08X post_nonzero=%llu post_zero_bytes=%llu post_min=%u post_max=%u "
        "post_low=%llu post_high=%llu post_mid=%llu post_capture=%s post_captured=%llu\n",
        source_stats.mapped && output_stats.mapped ? "ok" : "map_failed",
        embedded_swap_readback.frontbuffer,
        static_cast<unsigned long long>(embedded_swap_readback.frame_state_hash), source_stats.hash,
        static_cast<unsigned long long>(source_stats.nonzero_rgb_pixels),
        static_cast<unsigned long long>(source_stats.zero_bytes), source_stats.channel_min,
        source_stats.channel_max, static_cast<unsigned long long>(source_stats.endpoint_low_channels),
        static_cast<unsigned long long>(source_stats.endpoint_high_channels),
        static_cast<unsigned long long>(source_stats.midrange_channels), source_capture_path,
        static_cast<unsigned long long>(source_stats.captured_bytes), output_stats.hash,
        static_cast<unsigned long long>(output_stats.nonzero_rgb_pixels),
        static_cast<unsigned long long>(output_stats.zero_bytes), output_stats.channel_min,
        output_stats.channel_max, static_cast<unsigned long long>(output_stats.endpoint_low_channels),
        static_cast<unsigned long long>(output_stats.endpoint_high_channels),
        static_cast<unsigned long long>(output_stats.midrange_channels), output_capture_path,
        static_cast<unsigned long long>(output_stats.captured_bytes));
    std::fflush(stderr);
    embedded_swap_readback.source_resource.Reset();
    embedded_swap_readback.output_resource.Reset();
    embedded_swap_readback.submission = 0;
  }

  // Obtain the actual swap source texture size (resolution-scaled if it's a
  // resolve destination, or not otherwise).
  D3D12_SHADER_RESOURCE_VIEW_DESC swap_texture_srv_desc;
  xenos::TextureFormat frontbuffer_format;
  uint32_t frontbuffer_width_unscaled = 0, frontbuffer_height_unscaled = 0;
  ID3D12Resource* swap_texture_resource =
      texture_cache_->RequestSwapTexture(swap_texture_srv_desc, frontbuffer_format,
                                         &frontbuffer_width_unscaled, &frontbuffer_height_unscaled);
  if (!swap_texture_resource) {
    // Dump texture fetch constant 0 for debugging
    const auto& regs = *register_file_;
    auto fetch = regs.GetTextureFetch(0);
    REXGPU_ERROR(
        "IssueSwap: RequestSwapTexture failed - fetch0: {:08X} {:08X} {:08X} {:08X} {:08X} {:08X}",
        fetch.dword_0, fetch.dword_1, fetch.dword_2, fetch.dword_3, fetch.dword_4, fetch.dword_5);
    if (!kernel_state_) {
      std::fprintf(stderr,
                   "REX_EMBEDDED_SWAP_FAILURE reason=request_swap_texture fetch0=%08X,%08X,%08X,%08X,%08X,%08X\n",
                   fetch.dword_0, fetch.dword_1, fetch.dword_2, fetch.dword_3,
                   fetch.dword_4, fetch.dword_5);
      std::fflush(stderr);
    }
    return;
  }
  D3D12_RESOURCE_DESC swap_texture_desc = swap_texture_resource->GetDesc();
  if (!kernel_state_ && !embedded_swap_readback.attempted &&
      IsCurrentEmbeddedGameplayCaptureFrame() &&
      embedded_frame_frontier.successful_resolves >= 8) {
    embedded_swap_readback.attempted = true;
    ID3D12Device* device = GetD3D12Provider().GetDevice();
    device->GetCopyableFootprints(
        &swap_texture_desc, 0, 1, 0, &embedded_swap_readback.source_footprint,
        &embedded_swap_readback.source_rows, &embedded_swap_readback.source_row_size,
        &embedded_swap_readback.source_size);
    D3D12_RESOURCE_DESC readback_desc;
    ui::d3d12::util::FillBufferResourceDesc(
        readback_desc, embedded_swap_readback.source_size, D3D12_RESOURCE_FLAG_NONE);
    HRESULT create_result = device->CreateCommittedResource(
        &ui::d3d12::util::kHeapPropertiesReadback,
        GetD3D12Provider().GetHeapFlagCreateNotZeroed(), &readback_desc,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
        IID_PPV_ARGS(&embedded_swap_readback.source_resource));
    if (SUCCEEDED(create_result)) {
      PushTransitionBarrier(swap_texture_resource,
                            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                            D3D12_RESOURCE_STATE_COPY_SOURCE);
      SubmitBarriers();
      D3D12_TEXTURE_COPY_LOCATION source_location = {};
      source_location.pResource = swap_texture_resource;
      source_location.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
      source_location.SubresourceIndex = 0;
      D3D12_TEXTURE_COPY_LOCATION dest_location = {};
      dest_location.pResource = embedded_swap_readback.source_resource.Get();
      dest_location.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
      dest_location.PlacedFootprint = embedded_swap_readback.source_footprint;
      deferred_command_list_.D3DCopyTextureRegion(&dest_location, 0, 0, 0,
                                                  &source_location, nullptr);
      PushTransitionBarrier(swap_texture_resource, D3D12_RESOURCE_STATE_COPY_SOURCE,
                            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
      SubmitBarriers();
      embedded_swap_readback.submission = submission_current_;
      embedded_swap_readback.frontbuffer = frontbuffer_ptr;
      embedded_swap_readback.frame_state_hash = embedded_frame_frontier.state_hash;
      const reg::DC_LUT_PWL_DATA* gamma_ramp_pwl = gamma_ramp_pwl_rgb();
      uint32_t gamma_hash = 2166136261u;
      uint32_t base_violations[3] = {};
      uint32_t base_min[3] = {UINT32_MAX, UINT32_MAX, UINT32_MAX};
      uint32_t base_max[3] = {};
      for (uint32_t index = 0; index < 128; ++index) {
        for (uint32_t component = 0; component < 3; ++component) {
          uint32_t raw;
          std::memcpy(&raw, &gamma_ramp_pwl[index * 3 + component], sizeof(raw));
          gamma_hash = (gamma_hash ^ raw) * 16777619u;
          uint32_t base = gamma_ramp_pwl[index * 3 + component].base;
          base_min[component] = std::min(base_min[component], base);
          base_max[component] = std::max(base_max[component], base);
          if (index && base < gamma_ramp_pwl[(index - 1) * 3 + component].base) {
            ++base_violations[component];
          }
        }
      }
      FILE* gamma_capture = std::fopen("rex_level_gamma_pwl.bin", "wb");
      size_t gamma_captured = 0;
      if (gamma_capture) {
        gamma_captured = std::fwrite(gamma_ramp_pwl, sizeof(*gamma_ramp_pwl), 128 * 3,
                                     gamma_capture);
        std::fclose(gamma_capture);
      }
      std::fprintf(
          stderr,
          "REX_EMBEDDED_SWAP_READBACK result=queued frontbuffer=0x%08X "
          "state_hash=0x%016llX submission=%llu draw_packets=%llu resolves=%llu "
          "size=%llux%u format=%u bytes=%llu gamma_hash=0x%08X "
          "gamma_base_min=%u,%u,%u gamma_base_max=%u,%u,%u "
          "gamma_base_violations=%u,%u,%u gamma_entries_captured=%llu\n",
          frontbuffer_ptr,
          static_cast<unsigned long long>(embedded_frame_frontier.state_hash),
          static_cast<unsigned long long>(embedded_swap_readback.submission),
          static_cast<unsigned long long>(embedded_frame_frontier.draw_packets),
          static_cast<unsigned long long>(embedded_frame_frontier.successful_resolves),
          static_cast<unsigned long long>(swap_texture_desc.Width),
          swap_texture_desc.Height, uint32_t(swap_texture_desc.Format),
          static_cast<unsigned long long>(embedded_swap_readback.source_size), gamma_hash,
          base_min[0], base_min[1], base_min[2], base_max[0], base_max[1], base_max[2],
          base_violations[0], base_violations[1], base_violations[2],
          static_cast<unsigned long long>(gamma_captured));
      std::fflush(stderr);
    } else {
      std::fprintf(stderr,
                   "REX_EMBEDDED_SWAP_READBACK result=create_failed hr=0x%08X bytes=%llu\n",
                   uint32_t(create_result),
                   static_cast<unsigned long long>(embedded_swap_readback.source_size));
      std::fflush(stderr);
    }
  }
  // The swap gamma / FXAA pass samples source texels by pixel index, but swap
  // textures may be allocation-padded. Prefer the active frontbuffer region
  // from the swap packet, scaled proportionally to the actual source texture.
  uint32_t source_width_scaled = uint32_t(swap_texture_desc.Width);
  uint32_t source_height_scaled = uint32_t(swap_texture_desc.Height);
  auto get_active_swap_dimension = [](uint32_t packet_unscaled, uint32_t source_unscaled,
                                      uint32_t source_scaled) -> uint32_t {
    if (!source_scaled) {
      return 0;
    }
    uint32_t active_unscaled = packet_unscaled ? packet_unscaled : source_unscaled;
    if (!active_unscaled) {
      return source_scaled;
    }
    if (source_unscaled) {
      active_unscaled = std::min(active_unscaled, source_unscaled);
      uint64_t active_scaled =
          (uint64_t(active_unscaled) * source_scaled + (source_unscaled >> 1)) / source_unscaled;
      return uint32_t(std::clamp<uint64_t>(active_scaled, 1, source_scaled));
    }
    return std::min(active_unscaled, source_scaled);
  };
  uint32_t guest_output_width =
      get_active_swap_dimension(frontbuffer_width, frontbuffer_width_unscaled, source_width_scaled);
  uint32_t guest_output_height = get_active_swap_dimension(
      frontbuffer_height, frontbuffer_height_unscaled, source_height_scaled);
  if (!guest_output_width) {
    guest_output_width = source_width_scaled
                             ? source_width_scaled
                             : (frontbuffer_width ? frontbuffer_width : frontbuffer_width_unscaled);
  }
  if (!guest_output_height) {
    guest_output_height = source_height_scaled ? source_height_scaled
                                               : (frontbuffer_height ? frontbuffer_height
                                                                     : frontbuffer_height_unscaled);
  }
  bool swap_source_scaled = frontbuffer_width_unscaled && frontbuffer_height_unscaled &&
                            (source_width_scaled != frontbuffer_width_unscaled ||
                             source_height_scaled != frontbuffer_height_unscaled);
  if (texture_cache_->IsDrawResolutionScaled() && !swap_source_scaled) {
    static bool draw_scale_swap_unscaled_logged = false;
    if (!draw_scale_swap_unscaled_logged) {
      draw_scale_swap_unscaled_logged = true;
      REXGPU_WARN(
          "D3D12 draw resolution scaling is enabled, but the swap source is "
          "unscaled ({}x{}). This title may be presenting from an unscaled "
          "resolve path.",
          guest_output_width, guest_output_height);
    }
  }

  system::X_VIDEO_MODE video_mode;
  kernel::xboxkrnl::VdQueryVideoMode(&video_mode);
  uint32_t display_width = std::max(uint32_t(1), uint32_t(video_mode.display_width));
  uint32_t display_height = std::max(uint32_t(1), uint32_t(video_mode.display_height));

  // With the async submission worker, hand the presenter's post-refresh steps
  // to the worker (ordered after this frame's command lists) instead of
  // draining it here: the command processor continues with the next frame.
  // The setting is read at every swap so it can be compared in one run.
  const bool deferred_completion = async_submission_ && submission_thread_.joinable() &&
                                   REXCVAR_GET(d3d12_deferred_presenter_completion);
  if (deferred_completion != presenter_completion_deferred_) {
    if (deferred_completion) {
      presenter->SetGuestOutputCompletionExecutor(
          [this](std::function<void()> task) { EnqueueSubmissionTask(std::move(task)); });
    } else {
      // Run the pending completion before returning to inline completion.
      DrainSubmissions();
      presenter->SetGuestOutputCompletionExecutor(nullptr);
    }
    presenter_completion_deferred_ = deferred_completion;
    std::fprintf(stderr, "REX_PRESENTER_COMPLETION deferred=%d\n", deferred_completion ? 1 : 0);
    std::fflush(stderr);
  }

  const bool refresh_succeeded = presenter->RefreshGuestOutput(
      guest_output_width, guest_output_height, display_width, display_height,
      [this, &swap_texture_srv_desc, frontbuffer_format, swap_texture_resource, guest_output_width,
       guest_output_height](ui::Presenter::GuestOutputRefreshContext& context) -> bool {
        const ui::d3d12::D3D12Provider& provider = GetD3D12Provider();
        ID3D12Device* device = provider.GetDevice();

        SwapPostEffect swap_post_effect = GetActualSwapPostEffect();
        bool use_fxaa = swap_post_effect == SwapPostEffect::kFxaa ||
                        swap_post_effect == SwapPostEffect::kFxaaExtreme;
        if (use_fxaa) {
          // Make sure the texture of the correct size is available for FXAA.
          if (fxaa_source_texture_) {
            D3D12_RESOURCE_DESC fxaa_source_texture_desc = fxaa_source_texture_->GetDesc();
            if (fxaa_source_texture_desc.Width != guest_output_width ||
                fxaa_source_texture_desc.Height != guest_output_height) {
              if (submission_completed_ < fxaa_source_texture_submission_) {
                fxaa_source_texture_->AddRef();
                resources_for_deletion_.emplace_back(fxaa_source_texture_submission_,
                                                     fxaa_source_texture_.Get());
              }
              fxaa_source_texture_.Reset();
              fxaa_source_texture_submission_ = 0;
            }
          }
          if (!fxaa_source_texture_) {
            D3D12_RESOURCE_DESC fxaa_source_texture_desc;
            fxaa_source_texture_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            fxaa_source_texture_desc.Alignment = 0;
            fxaa_source_texture_desc.Width = guest_output_width;
            fxaa_source_texture_desc.Height = guest_output_height;
            fxaa_source_texture_desc.DepthOrArraySize = 1;
            fxaa_source_texture_desc.MipLevels = 1;
            fxaa_source_texture_desc.Format = kFxaaSourceTextureFormat;
            fxaa_source_texture_desc.SampleDesc.Count = 1;
            fxaa_source_texture_desc.SampleDesc.Quality = 0;
            fxaa_source_texture_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
            fxaa_source_texture_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
            if (FAILED(device->CreateCommittedResource(
                    &ui::d3d12::util::kHeapPropertiesDefault, provider.GetHeapFlagCreateNotZeroed(),
                    &fxaa_source_texture_desc, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                    nullptr, IID_PPV_ARGS(&fxaa_source_texture_)))) {
              REXGPU_ERROR("Failed to create the FXAA input texture");
              swap_post_effect = SwapPostEffect::kNone;
              use_fxaa = false;
            }
          }
        }
        // SMAA: the gamma pass writes the SMAA color texture; without its
        // resources the frame is presented without post-processing.
        const bool use_smaa = swap_post_effect == SwapPostEffect::kSmaa &&
                              smaa_neighborhood_blending_pipeline_ &&
                              EnsureSmaaResources(guest_output_width, guest_output_height);

        // This is according to D3D::InitializePresentationParameters from a
        // game executable, which initializes the 256-entry table gamma ramp for
        // 8_8_8_8 output and the PWL gamma ramp for 2_10_10_10.
        // TODO(Triang3l): Choose between the table and PWL based on
        // DC_LUTA_CONTROL, support both for all formats (and also different
        // increments for PWL).
        bool use_pwl_gamma_ramp =
            frontbuffer_format == xenos::TextureFormat::k_2_10_10_10 ||
            frontbuffer_format == xenos::TextureFormat::k_2_10_10_10_AS_16_16_16_16;

        context.SetIs8bpc(!use_pwl_gamma_ramp && !use_fxaa && !use_smaa);

        // Upload the new gamma ramp, using the upload buffer for the current
        // frame (will close the frame after this anyway, so can't write
        // multiple times per frame).
        if (!(use_pwl_gamma_ramp ? gamma_ramp_pwl_up_to_date_
                                 : gamma_ramp_256_entry_table_up_to_date_)) {
          uint32_t gamma_ramp_offset_bytes = use_pwl_gamma_ramp ? 256 * 4 : 0;
          uint32_t gamma_ramp_upload_offset_bytes =
              uint32_t(frame_current_ % kQueueFrames) * ((256 + 128 * 3) * 4) +
              gamma_ramp_offset_bytes;
          uint32_t gamma_ramp_size_bytes = (use_pwl_gamma_ramp ? 128 * 3 : 256) * 4;
          if (std::endian::native != std::endian::little && use_pwl_gamma_ramp) {
            // R16G16 is first R16, where the shader expects the base, and
            // second G16, where the delta should be, but gamma_ramp_pwl_rgb()
            // is an array of 32-bit DC_LUT_PWL_DATA registers - swap 16 bits in
            // each 32.
            auto gamma_ramp_pwl_upload_buffer = reinterpret_cast<reg::DC_LUT_PWL_DATA*>(
                gamma_ramp_upload_buffer_mapping_ + gamma_ramp_upload_offset_bytes);
            const reg::DC_LUT_PWL_DATA* gamma_ramp_pwl = gamma_ramp_pwl_rgb();
            for (size_t i = 0; i < 128 * 3; ++i) {
              reg::DC_LUT_PWL_DATA& gamma_ramp_pwl_upload_buffer_entry =
                  gamma_ramp_pwl_upload_buffer[i];
              reg::DC_LUT_PWL_DATA gamma_ramp_pwl_entry = gamma_ramp_pwl[i];
              gamma_ramp_pwl_upload_buffer_entry.base = gamma_ramp_pwl_entry.delta;
              gamma_ramp_pwl_upload_buffer_entry.delta = gamma_ramp_pwl_entry.base;
            }
          } else {
            std::memcpy(gamma_ramp_upload_buffer_mapping_ + gamma_ramp_upload_offset_bytes,
                        use_pwl_gamma_ramp ? static_cast<const void*>(gamma_ramp_pwl_rgb())
                                           : static_cast<const void*>(gamma_ramp_256_entry_table()),
                        gamma_ramp_size_bytes);
          }
          PushTransitionBarrier(gamma_ramp_buffer_.Get(), gamma_ramp_buffer_state_,
                                D3D12_RESOURCE_STATE_COPY_DEST);
          gamma_ramp_buffer_state_ = D3D12_RESOURCE_STATE_COPY_DEST;
          SubmitBarriers();
          deferred_command_list_.D3DCopyBufferRegion(
              gamma_ramp_buffer_.Get(), gamma_ramp_offset_bytes, gamma_ramp_upload_buffer_.Get(),
              gamma_ramp_upload_offset_bytes, gamma_ramp_size_bytes);
          (use_pwl_gamma_ramp ? gamma_ramp_pwl_up_to_date_
                              : gamma_ramp_256_entry_table_up_to_date_) = true;
        }

        // Destination, source, and if bindful, gamma ramp.
        ui::d3d12::util::DescriptorCpuGpuHandlePair apply_gamma_descriptors[3];
        ui::d3d12::util::DescriptorCpuGpuHandlePair apply_gamma_descriptor_gamma_ramp;
        if (!RequestOneUseSingleViewDescriptors(bindless_resources_used_ ? 2 : 3,
                                                apply_gamma_descriptors)) {
          return false;
        }
        // Must not call anything that can change the descriptor heap from now
        // on!
        if (bindless_resources_used_) {
          apply_gamma_descriptor_gamma_ramp = GetSystemBindlessViewHandlePair(
              use_pwl_gamma_ramp ? SystemBindlessView::kGammaRampPWLSRV
                                 : SystemBindlessView::kGammaRampTableSRV);
        } else {
          apply_gamma_descriptor_gamma_ramp = apply_gamma_descriptors[2];
          WriteGammaRampSRV(use_pwl_gamma_ramp, apply_gamma_descriptor_gamma_ramp.first);
        }

        ID3D12Resource* guest_output_resource =
            static_cast<ui::d3d12::D3D12Presenter::D3D12GuestOutputRefreshContext&>(context)
                .resource_uav_capable();

        if (use_fxaa) {
          fxaa_source_texture_submission_ = submission_current_;
        }
        if (use_smaa) {
          smaa_textures_submission_ = submission_current_;
        }

        ID3D12Resource* apply_gamma_dest =
            use_fxaa ? fxaa_source_texture_.Get()
                     : (use_smaa ? smaa_color_texture_.Get() : guest_output_resource);
        D3D12_RESOURCE_STATES apply_gamma_dest_initial_state =
            (use_fxaa || use_smaa) ? D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE
                                   : ui::d3d12::D3D12Presenter::kGuestOutputInternalState;
        static_cast<ui::d3d12::D3D12Presenter::D3D12GuestOutputRefreshContext&>(context)
            .resource_uav_capable();
        PushTransitionBarrier(apply_gamma_dest, apply_gamma_dest_initial_state,
                              D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        // From now on, even in case of failure, apply_gamma_dest must be
        // transitioned back to apply_gamma_dest_initial_state!
        D3D12_UNORDERED_ACCESS_VIEW_DESC apply_gamma_dest_uav_desc;
        apply_gamma_dest_uav_desc.Format =
            use_fxaa ? kFxaaSourceTextureFormat : ui::d3d12::D3D12Presenter::kGuestOutputFormat;
        apply_gamma_dest_uav_desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        apply_gamma_dest_uav_desc.Texture2D.MipSlice = 0;
        apply_gamma_dest_uav_desc.Texture2D.PlaneSlice = 0;
        device->CreateUnorderedAccessView(apply_gamma_dest, nullptr, &apply_gamma_dest_uav_desc,
                                          apply_gamma_descriptors[0].first);

        device->CreateShaderResourceView(swap_texture_resource, &swap_texture_srv_desc,
                                         apply_gamma_descriptors[1].first);

        PushTransitionBarrier(gamma_ramp_buffer_.Get(), gamma_ramp_buffer_state_,
                              D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        gamma_ramp_buffer_state_ = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

        deferred_command_list_.D3DSetComputeRootSignature(apply_gamma_root_signature_.Get());
        ApplyGammaConstants apply_gamma_constants;
        apply_gamma_constants.size[0] = guest_output_width;
        apply_gamma_constants.size[1] = guest_output_height;
        deferred_command_list_.D3DSetComputeRoot32BitConstants(
            UINT(ApplyGammaRootParameter::kConstants),
            sizeof(apply_gamma_constants) / sizeof(uint32_t), &apply_gamma_constants, 0);
        deferred_command_list_.D3DSetComputeRootDescriptorTable(
            UINT(ApplyGammaRootParameter::kDestination), apply_gamma_descriptors[0].second);
        deferred_command_list_.D3DSetComputeRootDescriptorTable(
            UINT(ApplyGammaRootParameter::kSource), apply_gamma_descriptors[1].second);
        deferred_command_list_.D3DSetComputeRootDescriptorTable(
            UINT(ApplyGammaRootParameter::kRamp), apply_gamma_descriptor_gamma_ramp.second);
        ID3D12PipelineState* apply_gamma_pipeline;
        if (use_pwl_gamma_ramp) {
          apply_gamma_pipeline = use_fxaa ? apply_gamma_pwl_fxaa_luma_pipeline_.Get()
                                          : apply_gamma_pwl_pipeline_.Get();
        } else {
          apply_gamma_pipeline = use_fxaa ? apply_gamma_table_fxaa_luma_pipeline_.Get()
                                          : apply_gamma_table_pipeline_.Get();
        }
        SetExternalPipeline(apply_gamma_pipeline);
        SubmitBarriers();
        uint32_t group_count_x = (guest_output_width + 15) / 16;
        uint32_t group_count_y = (guest_output_height + 7) / 8;
        deferred_command_list_.D3DDispatch(group_count_x, group_count_y, 1);

        // Apply FXAA.
        if (use_fxaa) {
          // Destination and source.
          ui::d3d12::util::DescriptorCpuGpuHandlePair fxaa_descriptors[2];
          if (!RequestOneUseSingleViewDescriptors(uint32_t(rex::countof(fxaa_descriptors)),
                                                  fxaa_descriptors)) {
            // Failed to obtain descriptors for FXAA - just copy after gamma
            // ramp application without applying FXAA.
            PushTransitionBarrier(apply_gamma_dest, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                  D3D12_RESOURCE_STATE_COPY_SOURCE);
            PushTransitionBarrier(guest_output_resource,
                                  ui::d3d12::D3D12Presenter::kGuestOutputInternalState,
                                  D3D12_RESOURCE_STATE_COPY_DEST);
            SubmitBarriers();
            deferred_command_list_.D3DCopyResource(guest_output_resource, apply_gamma_dest);
            PushTransitionBarrier(apply_gamma_dest, D3D12_RESOURCE_STATE_COPY_SOURCE,
                                  apply_gamma_dest_initial_state);
            PushTransitionBarrier(guest_output_resource, D3D12_RESOURCE_STATE_COPY_DEST,
                                  ui::d3d12::D3D12Presenter::kGuestOutputInternalState);
            return false;
          } else {
            assert_true(apply_gamma_dest_initial_state ==
                        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            PushTransitionBarrier(apply_gamma_dest, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                  apply_gamma_dest_initial_state);
            PushTransitionBarrier(guest_output_resource,
                                  ui::d3d12::D3D12Presenter::kGuestOutputInternalState,
                                  D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            // From now on, even in case of failure, guest_output_resource must
            // be transitioned back to kGuestOutputInternalState!
            deferred_command_list_.D3DSetComputeRootSignature(fxaa_root_signature_.Get());
            FxaaConstants fxaa_constants;
            fxaa_constants.size[0] = guest_output_width;
            fxaa_constants.size[1] = guest_output_height;
            fxaa_constants.size_inv[0] = 1.0f / float(fxaa_constants.size[0]);
            fxaa_constants.size_inv[1] = 1.0f / float(fxaa_constants.size[1]);
            deferred_command_list_.D3DSetComputeRoot32BitConstants(
                UINT(FxaaRootParameter::kConstants), sizeof(fxaa_constants) / sizeof(uint32_t),
                &fxaa_constants, 0);
            D3D12_UNORDERED_ACCESS_VIEW_DESC fxaa_dest_uav_desc;
            fxaa_dest_uav_desc.Format = ui::d3d12::D3D12Presenter::kGuestOutputFormat;
            fxaa_dest_uav_desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
            fxaa_dest_uav_desc.Texture2D.MipSlice = 0;
            fxaa_dest_uav_desc.Texture2D.PlaneSlice = 0;
            device->CreateUnorderedAccessView(guest_output_resource, nullptr, &fxaa_dest_uav_desc,
                                              fxaa_descriptors[0].first);
            deferred_command_list_.D3DSetComputeRootDescriptorTable(
                UINT(FxaaRootParameter::kDestination), fxaa_descriptors[0].second);
            D3D12_SHADER_RESOURCE_VIEW_DESC fxaa_source_srv_desc;
            fxaa_source_srv_desc.Format = kFxaaSourceTextureFormat;
            fxaa_source_srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            fxaa_source_srv_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            fxaa_source_srv_desc.Texture2D.MostDetailedMip = 0;
            fxaa_source_srv_desc.Texture2D.MipLevels = 1;
            fxaa_source_srv_desc.Texture2D.PlaneSlice = 0;
            fxaa_source_srv_desc.Texture2D.ResourceMinLODClamp = 0.0f;
            device->CreateShaderResourceView(fxaa_source_texture_.Get(), &fxaa_source_srv_desc,
                                             fxaa_descriptors[1].first);
            deferred_command_list_.D3DSetComputeRootDescriptorTable(
                UINT(FxaaRootParameter::kSource), fxaa_descriptors[1].second);
            SetExternalPipeline(swap_post_effect == SwapPostEffect::kFxaaExtreme
                                    ? fxaa_extreme_pipeline_.Get()
                                    : fxaa_pipeline_.Get());
            SubmitBarriers();
            deferred_command_list_.D3DDispatch(group_count_x, group_count_y, 1);
            PushTransitionBarrier(guest_output_resource, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                  ui::d3d12::D3D12Presenter::kGuestOutputInternalState);
          }
        } else if (use_smaa) {
          // Edges (color -> edges), blending weights (edges + lookup tables ->
          // weights), neighborhood blending (color + weights -> guest output).
          ui::d3d12::util::DescriptorCpuGpuHandlePair smaa_descriptors[9];
          PushTransitionBarrier(apply_gamma_dest, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
          if (!RequestOneUseSingleViewDescriptors(uint32_t(rex::countof(smaa_descriptors)),
                                                  smaa_descriptors)) {
            // No descriptors: present the gamma-corrected image as is.
            PushTransitionBarrier(apply_gamma_dest, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                  D3D12_RESOURCE_STATE_COPY_SOURCE);
            PushTransitionBarrier(guest_output_resource,
                                  ui::d3d12::D3D12Presenter::kGuestOutputInternalState,
                                  D3D12_RESOURCE_STATE_COPY_DEST);
            SubmitBarriers();
            deferred_command_list_.D3DCopyResource(guest_output_resource, apply_gamma_dest);
            PushTransitionBarrier(apply_gamma_dest, D3D12_RESOURCE_STATE_COPY_SOURCE,
                                  D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            PushTransitionBarrier(guest_output_resource, D3D12_RESOURCE_STATE_COPY_DEST,
                                  ui::d3d12::D3D12Presenter::kGuestOutputInternalState);
            return false;
          }
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
          constexpr DXGI_FORMAT kColorFormat = ui::d3d12::D3D12Presenter::kGuestOutputFormat;
          write_srv(smaa_color_texture_.Get(), kColorFormat, smaa_descriptors[0].first);
          write_uav(smaa_edges_texture_.Get(), DXGI_FORMAT_R8G8_UNORM, smaa_descriptors[1].first);
          write_srv(smaa_edges_texture_.Get(), DXGI_FORMAT_R8G8_UNORM, smaa_descriptors[2].first);
          write_srv(smaa_area_texture_.Get(), DXGI_FORMAT_R8G8_UNORM, smaa_descriptors[3].first);
          write_srv(smaa_search_texture_.Get(), DXGI_FORMAT_R8_UNORM, smaa_descriptors[4].first);
          write_uav(smaa_blend_texture_.Get(), DXGI_FORMAT_R8G8B8A8_UNORM,
                    smaa_descriptors[5].first);
          write_srv(smaa_color_texture_.Get(), kColorFormat, smaa_descriptors[6].first);
          write_srv(smaa_blend_texture_.Get(), DXGI_FORMAT_R8G8B8A8_UNORM,
                    smaa_descriptors[7].first);
          write_uav(guest_output_resource, kColorFormat, smaa_descriptors[8].first);

          deferred_command_list_.D3DSetComputeRootSignature(smaa_root_signature_.Get());
          FxaaConstants smaa_constants;
          smaa_constants.size[0] = guest_output_width;
          smaa_constants.size[1] = guest_output_height;
          smaa_constants.size_inv[0] = 1.0f / float(smaa_constants.size[0]);
          smaa_constants.size_inv[1] = 1.0f / float(smaa_constants.size[1]);
          deferred_command_list_.D3DSetComputeRoot32BitConstants(
              UINT(SmaaRootParameter::kConstants), sizeof(smaa_constants) / sizeof(uint32_t),
              &smaa_constants, 0);
          // Unused source slots of a pass repeat its first source.
          auto smaa_pass = [&](ID3D12PipelineState* pipeline, uint32_t source_0,
                               uint32_t source_1, uint32_t source_2, uint32_t destination) {
            deferred_command_list_.D3DSetComputeRootDescriptorTable(
                UINT(SmaaRootParameter::kSource0), smaa_descriptors[source_0].second);
            deferred_command_list_.D3DSetComputeRootDescriptorTable(
                UINT(SmaaRootParameter::kSource1), smaa_descriptors[source_1].second);
            deferred_command_list_.D3DSetComputeRootDescriptorTable(
                UINT(SmaaRootParameter::kSource2), smaa_descriptors[source_2].second);
            deferred_command_list_.D3DSetComputeRootDescriptorTable(
                UINT(SmaaRootParameter::kDestination), smaa_descriptors[destination].second);
            SetExternalPipeline(pipeline);
            SubmitBarriers();
            deferred_command_list_.D3DDispatch((guest_output_width + 7) / 8,
                                               (guest_output_height + 7) / 8, 1);
          };
          PushTransitionBarrier(smaa_edges_texture_.Get(),
                                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
          smaa_pass(smaa_edge_detection_pipeline_.Get(), 0, 0, 0, 1);
          PushTransitionBarrier(smaa_edges_texture_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
          PushTransitionBarrier(smaa_blend_texture_.Get(),
                                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
          smaa_pass(smaa_blending_weight_pipeline_.Get(), 2, 3, 4, 5);
          PushTransitionBarrier(smaa_blend_texture_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
          PushTransitionBarrier(guest_output_resource,
                                ui::d3d12::D3D12Presenter::kGuestOutputInternalState,
                                D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
          smaa_pass(smaa_neighborhood_blending_pipeline_.Get(), 6, 7, 6, 8);
          PushTransitionBarrier(guest_output_resource, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                ui::d3d12::D3D12Presenter::kGuestOutputInternalState);
        } else {
          assert_true(apply_gamma_dest_initial_state ==
                      ui::d3d12::D3D12Presenter::kGuestOutputInternalState);
          PushTransitionBarrier(apply_gamma_dest, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                apply_gamma_dest_initial_state);
        }

        // Pair the selected complex gameplay frame's final guest output with
        // the source captured before gamma. This is observational and bounded
        // to the one frame selected above.
        SubmitBarriers();
        if (kGpuDiagnostics && !kernel_state_ && embedded_swap_readback.source_resource &&
            !embedded_swap_readback.output_resource &&
            embedded_swap_readback.submission == submission_current_) {
          D3D12_RESOURCE_DESC output_desc = guest_output_resource->GetDesc();
          device->GetCopyableFootprints(
              &output_desc, 0, 1, 0, &embedded_swap_readback.output_footprint,
              &embedded_swap_readback.output_rows, &embedded_swap_readback.output_row_size,
              &embedded_swap_readback.output_size);
          D3D12_RESOURCE_DESC readback_desc;
          ui::d3d12::util::FillBufferResourceDesc(
              readback_desc, embedded_swap_readback.output_size, D3D12_RESOURCE_FLAG_NONE);
          HRESULT create_result = device->CreateCommittedResource(
              &ui::d3d12::util::kHeapPropertiesReadback,
              provider.GetHeapFlagCreateNotZeroed(), &readback_desc,
              D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
              IID_PPV_ARGS(&embedded_swap_readback.output_resource));
          if (SUCCEEDED(create_result)) {
            PushTransitionBarrier(
                guest_output_resource, ui::d3d12::D3D12Presenter::kGuestOutputInternalState,
                D3D12_RESOURCE_STATE_COPY_SOURCE);
            SubmitBarriers();
            D3D12_TEXTURE_COPY_LOCATION source_location = {};
            source_location.pResource = guest_output_resource;
            source_location.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            source_location.SubresourceIndex = 0;
            D3D12_TEXTURE_COPY_LOCATION dest_location = {};
            dest_location.pResource = embedded_swap_readback.output_resource.Get();
            dest_location.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            dest_location.PlacedFootprint = embedded_swap_readback.output_footprint;
            deferred_command_list_.D3DCopyTextureRegion(&dest_location, 0, 0, 0,
                                                        &source_location, nullptr);
            PushTransitionBarrier(
                guest_output_resource, D3D12_RESOURCE_STATE_COPY_SOURCE,
                ui::d3d12::D3D12Presenter::kGuestOutputInternalState);
            std::fprintf(stderr,
                         "REX_EMBEDDED_SWAP_READBACK result=post_gamma_queued "
                         "submission=%llu size=%llux%u format=%u bytes=%llu\n",
                         static_cast<unsigned long long>(submission_current_),
                         static_cast<unsigned long long>(output_desc.Width), output_desc.Height,
                         uint32_t(output_desc.Format),
                         static_cast<unsigned long long>(embedded_swap_readback.output_size));
          } else {
            std::fprintf(stderr,
                         "REX_EMBEDDED_SWAP_READBACK result=post_gamma_create_failed "
                         "hr=0x%08X bytes=%llu\n",
                         uint32_t(create_result),
                         static_cast<unsigned long long>(embedded_swap_readback.output_size));
          }
          std::fflush(stderr);
        }

        // Need to submit all the commands before giving the image back to the
        // presenter so it can submit its own commands for displaying it to the
        // queue.
        SubmitBarriers();
        EndSubmission(true);
        // The presenter signals its own fence on the queue after this; the
        // frame's commands must already be in the queue before it. When the
        // presenter's completion is deferred it runs as a worker task queued
        // after this submission, so no drain is needed.
        if (!presenter_completion_deferred_) {
          DrainSubmissions();
        }
        return true;
      });

  if (record_embedded_timing) {
    embedded_frame_frontier.issue_swap_us =
        uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
                     std::chrono::steady_clock::now() - issue_swap_start)
                     .count());

    const PipelineCache::TelemetrySnapshot pipeline_telemetry =
        pipeline_cache_->GetTelemetrySnapshot();
    static PipelineCache::TelemetrySnapshot previous_pipeline_telemetry;
    const auto counter_delta = [](uint64_t current, uint64_t previous) {
      return current >= previous ? current - previous : current;
    };
    embedded_frame_frontier.pipeline_current_reuses = counter_delta(
        pipeline_telemetry.current_reuses,
        previous_pipeline_telemetry.current_reuses);
    embedded_frame_frontier.pipeline_cache_hits = counter_delta(
        pipeline_telemetry.cache_hits, previous_pipeline_telemetry.cache_hits);
    embedded_frame_frontier.pipeline_cache_misses = counter_delta(
        pipeline_telemetry.cache_misses,
        previous_pipeline_telemetry.cache_misses);
    embedded_frame_frontier.pipeline_async_queued = counter_delta(
        pipeline_telemetry.async_queued,
        previous_pipeline_telemetry.async_queued);
    embedded_frame_frontier.pipeline_sync_created = counter_delta(
        pipeline_telemetry.sync_created,
        previous_pipeline_telemetry.sync_created);
    embedded_frame_frontier.pipeline_async_completed = counter_delta(
        pipeline_telemetry.async_completed,
        previous_pipeline_telemetry.async_completed);
    embedded_frame_frontier.pipeline_async_failed = counter_delta(
        pipeline_telemetry.async_failed,
        previous_pipeline_telemetry.async_failed);
    embedded_frame_frontier.pipeline_queue_depth =
        pipeline_telemetry.queue_depth;
    embedded_frame_frontier.pipeline_threads_busy =
        pipeline_telemetry.threads_busy;
    previous_pipeline_telemetry = pipeline_telemetry;

    // EndSubmission signals submission_current_ - 1. Sample the fence only in
    // the explicitly enabled diagnostics path so ordinary embedded Release
    // runs don't add a GPU fence query to every swap. This is host D3D12
    // submission pressure, not a claim about guest PM4 queue occupancy.
    embedded_frame_frontier.host_submission_submitted =
        submission_current_ ? submission_current_ - 1 : 0;
    const uint64_t completed = submission_fence_
        ? submission_fence_->GetCompletedValue()
        : submission_completed_;
    embedded_frame_frontier.host_submission_completed = completed;
    embedded_frame_frontier.host_submission_backlog =
        completed <= embedded_frame_frontier.host_submission_submitted
            ? embedded_frame_frontier.host_submission_submitted - completed
            : 0;
  }

  if (!kernel_state_ && IsEmbeddedFrameDiagnosticsEnabled()) {
    static uint64_t embedded_swap_refresh_ordinal = 0;
    static std::array<uint32_t, 64> embedded_frontbuffers{};
    static size_t embedded_frontbuffer_count = 0;
    static std::chrono::steady_clock::time_point embedded_swap_first_time{};
    static std::chrono::steady_clock::time_point embedded_swap_previous_time{};
    const uint64_t ordinal = ++embedded_swap_refresh_ordinal;
    const auto swap_time = std::chrono::steady_clock::now();
    if (ordinal == 1) {
      embedded_swap_first_time = swap_time;
      embedded_swap_previous_time = swap_time;
    }
    const uint64_t swap_delta_us = ordinal > 1
        ? static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
              swap_time - embedded_swap_previous_time).count())
        : 0;
    const uint64_t swap_elapsed_us = ordinal > 1
        ? static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
              swap_time - embedded_swap_first_time).count())
        : 0;
    embedded_swap_previous_time = swap_time;
    const double swap_average_hz = swap_elapsed_us
        ? double(ordinal - 1) * 1000000.0 / double(swap_elapsed_us)
        : 0.0;
    const uint64_t hitch_threshold_us =
        uint64_t(REXCVAR_GET(embedded_hitch_trace_threshold_ms)) * 1000;
    if (REXCVAR_GET(embedded_hitch_diagnostics) && ordinal > 1 &&
        swap_delta_us >= hitch_threshold_us) {
      std::fprintf(
          stderr,
          "REX_EMBEDDED_HITCH_ATTRIBUTION ordinal=%llu swap_delta_us=%llu "
          "issue_swap_us=%llu rt_calls=%llu rt_total_us=%llu rt_max_us=%llu "
          "pipeline_calls=%llu pipeline_total_us=%llu pipeline_max_us=%llu "
          "pipeline_placeholder_draws=%llu pipeline_reuses=%llu "
          "pipeline_hits=%llu pipeline_misses=%llu pipeline_async_queued=%llu "
          "pipeline_sync_created=%llu pipeline_async_completed=%llu "
          "pipeline_async_failed=%llu pipeline_queue_depth=%llu "
          "pipeline_threads_busy=%llu texture_calls=%llu "
          "texture_total_us=%llu texture_max_us=%llu "
          "host_submission_submitted=%llu host_submission_completed=%llu "
          "host_submission_backlog=%llu draws=%llu resolves=%llu\n",
          static_cast<unsigned long long>(ordinal),
          static_cast<unsigned long long>(swap_delta_us),
          static_cast<unsigned long long>(embedded_frame_frontier.issue_swap_us),
          static_cast<unsigned long long>(
              embedded_frame_frontier.render_target_update_calls),
          static_cast<unsigned long long>(
              embedded_frame_frontier.render_target_update_us),
          static_cast<unsigned long long>(
              embedded_frame_frontier.render_target_update_max_us),
          static_cast<unsigned long long>(
              embedded_frame_frontier.pipeline_configure_calls),
          static_cast<unsigned long long>(
              embedded_frame_frontier.pipeline_configure_us),
          static_cast<unsigned long long>(
              embedded_frame_frontier.pipeline_configure_max_us),
          static_cast<unsigned long long>(
              embedded_frame_frontier.pipeline_placeholder_draws),
          static_cast<unsigned long long>(
              embedded_frame_frontier.pipeline_current_reuses),
          static_cast<unsigned long long>(
              embedded_frame_frontier.pipeline_cache_hits),
          static_cast<unsigned long long>(
              embedded_frame_frontier.pipeline_cache_misses),
          static_cast<unsigned long long>(
              embedded_frame_frontier.pipeline_async_queued),
          static_cast<unsigned long long>(
              embedded_frame_frontier.pipeline_sync_created),
          static_cast<unsigned long long>(
              embedded_frame_frontier.pipeline_async_completed),
          static_cast<unsigned long long>(
              embedded_frame_frontier.pipeline_async_failed),
          static_cast<unsigned long long>(
              embedded_frame_frontier.pipeline_queue_depth),
          static_cast<unsigned long long>(
              embedded_frame_frontier.pipeline_threads_busy),
          static_cast<unsigned long long>(
              embedded_frame_frontier.texture_request_calls),
          static_cast<unsigned long long>(
              embedded_frame_frontier.texture_request_us),
          static_cast<unsigned long long>(
              embedded_frame_frontier.texture_request_max_us),
          static_cast<unsigned long long>(
              embedded_frame_frontier.host_submission_submitted),
          static_cast<unsigned long long>(
              embedded_frame_frontier.host_submission_completed),
          static_cast<unsigned long long>(
              embedded_frame_frontier.host_submission_backlog),
          static_cast<unsigned long long>(
              embedded_frame_frontier.submitted_draws),
          static_cast<unsigned long long>(
              embedded_frame_frontier.successful_resolves));
      std::fflush(stderr);
    }
    size_t frontbuffer_index = 0;
    while (frontbuffer_index < embedded_frontbuffer_count &&
           embedded_frontbuffers[frontbuffer_index] != frontbuffer_ptr) {
      ++frontbuffer_index;
    }
    const bool first_frontbuffer = frontbuffer_index == embedded_frontbuffer_count;
    if (first_frontbuffer && embedded_frontbuffer_count < embedded_frontbuffers.size()) {
      embedded_frontbuffers[embedded_frontbuffer_count++] = frontbuffer_ptr;
    }
    const bool trace_frontier = !refresh_succeeded || ordinal <= 32 ||
                                !(ordinal & (ordinal - 1)) || !(ordinal & 255) ||
                                first_frontbuffer;
    uint32_t frontbuffer_resolves = 0;
    uint64_t frontbuffer_resolved_bytes = 0;
    for (uint32_t index = 0; index < embedded_frame_frontier.resolve_destination_count;
         ++index) {
      const auto& destination = embedded_frame_frontier.resolve_destinations[index];
      if (destination.address == frontbuffer_ptr) {
        frontbuffer_resolves += destination.count;
        frontbuffer_resolved_bytes += destination.bytes;
      }
    }
    if (trace_frontier) {
      std::fprintf(
          stderr,
          "REX_EMBEDDED_FRAME_FRONTIER ordinal=%llu result=%u first_frontbuffer=%u "
          "frontbuffer=0x%08X packet=%ux%u source=%ux%u source_format=%u "
          "resource_format=%u resource=%llux%u display=%ux%u draw_packets=%llu "
          "submitted_draws=%llu pixel_draws=%llu memexport_draws=%llu vertices=%llu "
          "state_hash=0x%016llX resolve_attempts=%llu successful_resolves=%llu "
          "resolved_bytes=%llu unique_resolve_destinations=%u frontbuffer_resolves=%u "
          "frontbuffer_resolved_bytes=%llu last_resolve=0x%08X "
          "swap_delta_us=%llu swap_elapsed_us=%llu swap_average_hz=%.3f\n",
          static_cast<unsigned long long>(ordinal), refresh_succeeded ? 1u : 0u,
          first_frontbuffer ? 1u : 0u, frontbuffer_ptr, frontbuffer_width,
          frontbuffer_height, frontbuffer_width_unscaled, frontbuffer_height_unscaled,
          uint32_t(frontbuffer_format), uint32_t(swap_texture_desc.Format),
          static_cast<unsigned long long>(swap_texture_desc.Width),
          uint32_t(swap_texture_desc.Height), display_width, display_height,
          static_cast<unsigned long long>(embedded_frame_frontier.draw_packets),
          static_cast<unsigned long long>(embedded_frame_frontier.submitted_draws),
          static_cast<unsigned long long>(embedded_frame_frontier.pixel_shader_draws),
          static_cast<unsigned long long>(embedded_frame_frontier.memexport_draws),
          static_cast<unsigned long long>(embedded_frame_frontier.submitted_vertices),
          static_cast<unsigned long long>(embedded_frame_frontier.state_hash),
          static_cast<unsigned long long>(embedded_frame_frontier.resolve_attempts),
          static_cast<unsigned long long>(embedded_frame_frontier.successful_resolves),
          static_cast<unsigned long long>(embedded_frame_frontier.resolved_bytes),
          embedded_frame_frontier.resolve_destination_count, frontbuffer_resolves,
          static_cast<unsigned long long>(frontbuffer_resolved_bytes),
          embedded_frame_frontier.last_resolve_destination,
          static_cast<unsigned long long>(swap_delta_us),
          static_cast<unsigned long long>(swap_elapsed_us), swap_average_hz);
      std::fflush(stderr);
    }
    if (IsEmbeddedGameplayCaptureSwap(ordinal)) {
      WriteEmbeddedGameplayCapture(ordinal, refresh_succeeded,
                                   frontbuffer_ptr);
    }
    if (REXCVAR_GET(embedded_manual_gameplay_capture)) {
      const uint64_t requested_generation =
          embedded_manual_capture_request_generation.load(
              std::memory_order_acquire);
      uint64_t capture_generation = 0;
      const embedded_manual_capture_policy::Action manual_action =
          embedded_manual_capture_policy::Advance(
              requested_generation, embedded_manual_capture_state,
              &capture_generation);
      if (manual_action == embedded_manual_capture_policy::Action::kArm) {
        if (REXCVAR_GET(embedded_camera_history_capture) && REXCVAR_GET(embedded_camera_draw_capture) &&
            REXCVAR_GET(embedded_hitch_diagnostics)) {
          if (!embedded_camera_history_budget.Arm(requested_generation, ordinal,
              REXCVAR_GET(embedded_camera_history_capture_frames))) {
            std::fprintf(stderr, "REX_CAMERA_HISTORY_ABORT generation=%llu reason=invalid_or_repeated_arm frames=%u\n",
                static_cast<unsigned long long>(requested_generation),
                REXCVAR_GET(embedded_camera_history_capture_frames));
          }
        }
        std::fprintf(stderr,
                     "REX_EMBEDDED_MANUAL_CAPTURE_ARMED generation=%llu "
                     "after_swap=%llu\n",
                     static_cast<unsigned long long>(requested_generation),
                     static_cast<unsigned long long>(ordinal));
        std::fflush(stderr);
      } else if (manual_action ==
                 embedded_manual_capture_policy::Action::kCapture) {
        WriteEmbeddedGameplayCapture(ordinal, refresh_succeeded,
                                     frontbuffer_ptr, capture_generation);
      }
    }
    TraceEmbeddedInputToSwap(ordinal, refresh_succeeded);
    if (REXCVAR_GET(embedded_camera_history_capture)) {
      FinishEmbeddedCameraHistory(ordinal, refresh_succeeded, frontbuffer_ptr,
                                  GetCurrentSubmission(), GetCompletedSubmission());
    }
    embedded_completed_swap_ordinal = ordinal;
    embedded_frame_frontier.Reset();
  }

  // End the frame even if did not present for any reason (the image refresher
  // was not called), to prevent leaking per-frame resources.
  EndSubmission(true);

  // Measurement builds: RenderDoc captures of whole guest frames (see
  // SetRenderDocApi), bounded by guest swaps with every earlier submission
  // already executed.
  if (kGpuDiagnostics) {
    if (void** api = renderdoc_api.load()) {
      using StartFrameCaptureFn = void(__cdecl*)(void*, void*);
      using EndFrameCaptureFn = uint32_t(__cdecl*)(void*, void*);
      if (renderdoc_capture_frames_left) {
        if (!--renderdoc_capture_frames_left) {
          DrainSubmissions();
          const uint32_t ended = reinterpret_cast<EndFrameCaptureFn>(api[21])(nullptr, nullptr);
          std::fprintf(stderr, "REX_RENDERDOC_GUEST_FRAMES end=%u after_swap=%llu\n", ended,
                       static_cast<unsigned long long>(embedded_completed_swap_ordinal));
          std::fflush(stderr);
        }
      } else if (const uint32_t frames = renderdoc_requested_frames.exchange(0)) {
        DrainSubmissions();
        reinterpret_cast<StartFrameCaptureFn>(api[19])(nullptr, nullptr);
        renderdoc_capture_frames_left = frames;
        std::fprintf(stderr, "REX_RENDERDOC_GUEST_FRAMES start frames=%u after_swap=%llu\n", frames,
                     static_cast<unsigned long long>(embedded_completed_swap_ordinal));
        std::fflush(stderr);
      }
    }
  }
}

void D3D12CommandProcessor::OnPrimaryBufferEnd() {
  if (REXCVAR_GET(d3d12_submit_on_primary_buffer_end) && submission_open_ &&
      CanEndSubmissionImmediately()) {
    EndSubmission(false);
  }
}

Shader* D3D12CommandProcessor::LoadShader(xenos::ShaderType shader_type, uint32_t guest_address,
                                          const uint32_t* host_address, uint32_t dword_count) {
  return pipeline_cache_->LoadShader(shader_type, host_address, dword_count);
}

bool D3D12CommandProcessor::IssueDraw(xenos::PrimitiveType primitive_type, uint32_t index_count,
                                      IndexBufferInfo* index_buffer_info,
                                      bool major_mode_explicit) {
#if XE_GPU_FINE_GRAINED_DRAW_SCOPES
  SCOPE_profile_cpu_f("gpu");
#endif  // XE_GPU_FINE_GRAINED_DRAW_SCOPES

  // A bounded one-in-64 draw sample partitions the high-volume street path.
  // It is active only with the swap interval diagnostic; every early return
  // contributes to its current stage and ordinary launches read no timer.
  struct SampledDrawScope {
    SwapIntervalDiagnostic* diagnostic;
    uint64_t stage_begin;
    uint64_t part_begin = 0;
    uint64_t stage_ticks[3]{};
    uint64_t pre_texture_part_ticks[SwapIntervalDiagnostic::kPreTextureParts]{};
    uint32_t marked_pre_texture_parts = 0;
    uint64_t binding_begin = 0;
    uint64_t binding_ticks = 0;
    uint32_t stage = 0;
    void Advance() {
      if (!diagnostic || stage >= 2) return;
      const uint64_t now = rex::chrono::Clock::QueryHostTickCount();
      stage_ticks[stage++] += now - stage_begin;
      stage_begin = now;
    }
    void MarkPreTexturePart() {
      if (!diagnostic || stage ||
          marked_pre_texture_parts >= SwapIntervalDiagnostic::kPreTextureParts) return;
      const uint64_t now = rex::chrono::Clock::QueryHostTickCount();
      pre_texture_part_ticks[marked_pre_texture_parts++] =
          now - (part_begin ? part_begin : stage_begin);
      part_begin = now;
    }
    void BeginBinding() {
      if (diagnostic) binding_begin = rex::chrono::Clock::QueryHostTickCount();
    }
    void FinishBinding() {
      if (diagnostic) {
        binding_ticks += rex::chrono::Clock::QueryHostTickCount() - binding_begin;
      }
    }
    ~SampledDrawScope() {
      if (!diagnostic) return;
      stage_ticks[stage] += rex::chrono::Clock::QueryHostTickCount() - stage_begin;
      diagnostic->AddSampledDraw(stage_ticks[0], stage_ticks[1], stage_ticks[2],
                                 binding_ticks, pre_texture_part_ticks,
                                 marked_pre_texture_parts);
    }
  };
  const bool sample_draw = kGpuDiagnostics && swap_intervals_.ShouldSampleDraw();
  SampledDrawScope sampled_draw_scope{
      sample_draw ? &swap_intervals_ : nullptr,
      sample_draw ? rex::chrono::Clock::QueryHostTickCount() : 0};

  // One opt-in, sparse CP window at a time. A scope captures every return path
  // without changing command submission or a guest-visible draw decision.
  struct SparseDrawWorkScope {
    CpCadenceDiagnostic* diagnostic;
    uint64_t begin_tick;
    ~SparseDrawWorkScope() {
      if (diagnostic) {
        diagnostic->DrawWorkTime(rex::chrono::Clock::QueryHostTickCount() - begin_tick);
      }
    }
  } sparse_draw_work_scope{
      (kGpuDiagnostics && cp_cadence_.active) ? &cp_cadence_ : nullptr,
      (kGpuDiagnostics && cp_cadence_.active) ? rex::chrono::Clock::QueryHostTickCount() : 0};

  ID3D12Device* device = GetD3D12Provider().GetDevice();
  const RegisterFile& regs = *register_file_;
  const bool record_embedded_timing = kGpuDiagnostics &&
      !kernel_state_ && (REXCVAR_GET(embedded_hitch_diagnostics) || cp_cadence_.active);
  if (!kernel_state_ && IsEmbeddedFrameDiagnosticsEnabled()) {
    ++embedded_frame_frontier.draw_packets;
  }
  auto fail_embedded_draw = [this](const char* stage) -> bool {
    if (!kernel_state_) {
      if (texture_cache_->OwnsSceneHistory())
        texture_cache_->RecordOwnedDrawTransform({}, false);
      if (REXCVAR_GET(embedded_camera_history_capture))
        embedded_camera_history_budget.Failure(embedded_completed_swap_ordinal + 1);
      static uint64_t failure_ordinal = 0;
      std::fprintf(stderr, "REX_EMBEDDED_DRAW_FAILURE ordinal=%llu stage=%s\n",
                   static_cast<unsigned long long>(++failure_ordinal), stage);
      std::fflush(stderr);
    }
    return false;
  };

  xenos::EdramMode edram_mode = regs.Get<reg::RB_MODECONTROL>().edram_mode;
  if (edram_mode == xenos::EdramMode::kCopy) {
    // Special copy handling.
    return IssueCopy();
  }

  bool surface_pitch_is_zero = regs.Get<reg::RB_SURFACE_INFO>().surface_pitch == 0;

  // Vertex shader analysis.
  auto vertex_shader = static_cast<D3D12Shader*>(active_vertex_shader());
  if (!vertex_shader) {
    // Always need a vertex shader.
    return fail_embedded_draw("missing_vertex_shader");
  }
  pipeline_cache_->AnalyzeShaderUcode(*vertex_shader);
  bool memexport_used_vertex = vertex_shader->memexport_eM_written() != 0;

  // Pixel shader analysis.
  bool primitive_polygonal = draw_util::IsPrimitivePolygonal(regs);
  bool is_rasterization_done = draw_util::IsRasterizationPotentiallyDone(regs, primitive_polygonal);
  if (surface_pitch_is_zero && is_rasterization_done) {
    // Doesn't actually draw.
    // Unlikely that zero would even really be legal though.
    return true;
  }
  D3D12Shader* pixel_shader = nullptr;
  if (is_rasterization_done) {
    // See xenos::EdramMode for explanation why the pixel shader is only used
    // when it's kColorDepth here.
    if (edram_mode == xenos::EdramMode::kColorDepth) {
      pixel_shader = static_cast<D3D12Shader*>(active_pixel_shader());
      if (pixel_shader) {
        pipeline_cache_->AnalyzeShaderUcode(*pixel_shader);
        if (!draw_util::IsPixelShaderNeededWithRasterization(*pixel_shader, regs)) {
          pixel_shader = nullptr;
        }
      }
    }
  } else {
    // Disabling pixel shader for this case is also required by the pipeline
    // cache.
    if (!memexport_used_vertex) {
      // This draw has no effect.
      return true;
    }
  }
  bool memexport_used_pixel = pixel_shader && (pixel_shader->memexport_eM_written() != 0);
  bool memexport_used = memexport_used_vertex || memexport_used_pixel;

  if (!BeginSubmission(true)) {
    return fail_embedded_draw("begin_submission");
  }
  // A logical Xenos report may span D3D12 command-list submissions. Each new
  // list opens another real host segment; all in-flight segments retire at the
  // guest END without stalling intermediate draws.
  if (logical_occlusion_query_.valid && !active_occlusion_query_.valid &&
      !BeginGuestOcclusionQuerySegment()) {
    return fail_embedded_draw("resume_occlusion_segment");
  }
  if (sample_draw) sampled_draw_scope.MarkPreTexturePart();

  // Process primitives.
  PrimitiveProcessor::ProcessingResult primitive_processing_result;
  const bool primitive_processing_succeeded =
      primitive_processor_->Process(primitive_processing_result);
  if (sample_draw) sampled_draw_scope.MarkPreTexturePart();
  if (!primitive_processing_succeeded) {
    return fail_embedded_draw("primitive_processing");
  }
  if (!primitive_processing_result.host_draw_vertex_count) {
    // Nothing to draw.
    return true;
  }

  reg::RB_DEPTHCONTROL normalized_depth_control = draw_util::GetNormalizedDepthControl(regs);

  // Shader modifications.
  uint32_t ps_param_gen_pos = UINT32_MAX;
  uint32_t interpolator_mask =
      pixel_shader ? (vertex_shader->writes_interpolators() &
                      pixel_shader->GetInterpolatorInputMask(regs.Get<reg::SQ_PROGRAM_CNTL>(),
                                                             regs.Get<reg::SQ_CONTEXT_MISC>(),
                                                             ps_param_gen_pos))
                   : 0;
  // Set up the render targets - this may perform dispatches and draws.
  uint32_t normalized_color_mask =
      pixel_shader ? draw_util::GetNormalizedColorMask(regs, pixel_shader->writes_color_targets())
                   : 0;
  const auto render_target_update_start =
      record_embedded_timing ? std::chrono::steady_clock::now()
                             : std::chrono::steady_clock::time_point{};
  bool native_shader_grid = false;
  uint32_t native_filter_fetch = 0;
  native_filter_active_ = false;
  std::fill(std::begin(native_filter_fetch_modes_), std::end(native_filter_fetch_modes_),
            NativeFilterFetchMode::kOff);
  if (pixel_shader && render_target_cache_->IsDrawResolutionScaled() &&
      render_target_cache_->GetPath() == RenderTargetCache::Path::kHostRenderTargets) {
    // The first matching rule selects the shader variant and rasterization;
    // later image filter rules of the same draw (other fetches, scaled output)
    // only add their fetch's runtime footprint mode.
    bool primary_matched = false;
    bool candidates_used = false;
    for (uint32_t i = 0; i < native_shader_grid_rules_.count; ++i) {
      const auto& rule = native_shader_grid_rules_.entries[i];
      if (vertex_shader->ucode_data_hash() != rule.vertex_hash ||
          pixel_shader->ucode_data_hash() != rule.pixel_hash) continue;
      if (primary_matched &&
          (!native_filter_active_ || native_shader_grid || !rule.image_filter ||
           !rule.scaled_filter_output ||
           native_filter_fetch_modes_[rule.fetch] != NativeFilterFetchMode::kOff ||
           (rule.native_source && candidates_used))) continue;
      const auto fetch = regs.GetTextureFetch(rule.fetch);
      if (fetch.type != xenos::FetchConstantType::kTexture ||
          !render_target::native_shader_scale_policy::Matches(
              rule, vertex_shader->ucode_data_hash(), pixel_shader->ucode_data_hash(),
              fetch.dimension == xenos::DataDimension::k2DOrStacked && !fetch.stacked,
              fetch.size_2d.width + 1, fetch.size_2d.height + 1,
              uint32_t(fetch.format), uint32_t(regs.Get<reg::RB_SURFACE_INFO>().msaa_samples),
              normalized_color_mask, normalized_depth_control.value)) {
        // A rule's own shader pair drawn with another source (size, format,
        // MSAA, outputs): logged once per combination (#10 diagnosis).
        if (kGpuDiagnostics) {
          const uint64_t miss_key =
              (uint64_t(i) << 56) ^ (uint64_t(fetch.size_2d.width) << 40) ^
              (uint64_t(fetch.size_2d.height) << 24) ^ (uint64_t(fetch.format) << 16) ^
              (uint64_t(regs.Get<reg::RB_SURFACE_INFO>().msaa_samples) << 12) ^
              (uint64_t(normalized_color_mask) << 4) ^ uint64_t(fetch.type);
          static std::mutex miss_mutex;
          static std::unordered_set<uint64_t> misses;
          std::lock_guard<std::mutex> miss_lock(miss_mutex);
          if (misses.size() < 256 && misses.insert(miss_key).second) {
            std::fprintf(stderr,
                         "REX_NATIVE_SHADER_GRID_MISS rule=%u fetch=%u type=%u dim=%u stacked=%u "
                         "size=%ux%u format=%u msaa=%u color_mask=0x%X depth=0x%X "
                         "rule_size=%ux%u rule_format=%u\n",
                         i, rule.fetch, uint32_t(fetch.type), uint32_t(fetch.dimension),
                         uint32_t(fetch.stacked), fetch.size_2d.width + 1,
                         fetch.size_2d.height + 1, uint32_t(fetch.format),
                         uint32_t(regs.Get<reg::RB_SURFACE_INFO>().msaa_samples),
                         normalized_color_mask, normalized_depth_control.value, rule.width,
                         rule.height, rule.format);
            std::fflush(stderr);
          }
        }
        continue;
      }
      if (rule.image_filter) {
        if (!render_target::native_shader_scale_policy::FilterSamplingSupported(
                render_target_cache_->draw_resolution_scale_x(),
                render_target_cache_->draw_resolution_scale_y(),
                fetch.mag_filter == xenos::TextureFilter::kLinear &&
                    fetch.min_filter == xenos::TextureFilter::kLinear &&
                    fetch.aniso_filter == xenos::AnisoFilter::kDisabled,
                fetch.mip_max_level,
                fetch.sign_x == xenos::TextureSign::kUnsigned &&
                    fetch.sign_y == xenos::TextureSign::kUnsigned &&
                    fetch.sign_z == xenos::TextureSign::kUnsigned &&
                    fetch.sign_w == xenos::TextureSign::kUnsigned,
                fetch.clamp_x == xenos::ClampMode::kClampToEdge &&
                    fetch.clamp_y == xenos::ClampMode::kClampToEdge)) continue;
        if (!primary_matched) {
          native_filter_fetch = rule.fetch + 1;
        }
        native_filter_active_ = true;
        float* region = native_filter_fetch_regions_[rule.fetch];
        if (rule.native_source) {
          native_filter_fetch_modes_[rule.fetch] = NativeFilterFetchMode::kSourceNative;
          candidates_used = true;
        } else if (rule.has_region) {
          native_filter_fetch_modes_[rule.fetch] = NativeFilterFetchMode::kRegion;
          region[0] = float(rule.region_left);
          region[1] = float(rule.region_top);
          region[2] = float(rule.region_right);
          region[3] = float(rule.region_bottom);
        } else {
          native_filter_fetch_modes_[rule.fetch] = NativeFilterFetchMode::kUnbounded;
        }
      }
      const uint32_t msaa_log2 = uint32_t(regs.Get<reg::RB_SURFACE_INFO>().msaa_samples);
      if (!primary_matched) {
        native_shader_grid =
            render_target::native_shader_scale_policy::RequiresNativeRasterization(rule, msaa_log2);
      }
      // Logged once per rule and MSAA class (a 4x draw keeps a scaled output).
      const uint64_t logged_bit = uint64_t(1) << (i * 2 + (msaa_log2 > 1 ? 1 : 0));
      if (!(native_shader_grid_logged_mask_ & logged_bit)) {
        native_shader_grid_logged_mask_ |= logged_bit;
        std::fprintf(stderr,
                     "REX_NATIVE_SHADER_GRID rule=%u vs=0x%016llX ps=0x%016llX "
                     "fetch=%u logical=%ux%u format=%u msaa=%ux result=%s image_filter=%u\n",
                     i, static_cast<unsigned long long>(rule.vertex_hash),
                     static_cast<unsigned long long>(rule.pixel_hash), rule.fetch,
                     rule.width, rule.height, rule.format, 1u << msaa_log2,
                     native_shader_grid ? "native_rasterization" : "scaled_filter_output",
                     rule.image_filter ? 1u : 0u);
        std::fflush(stderr);
      }
      primary_matched = true;
      // A data rule (native rasterization of a table) takes no further rules.
      if (!native_filter_active_) break;
    }
  }
  auto scene_update = embedded_scene_update_budget.Begin(
      embedded_completed_swap_ordinal + 1, embedded_frame_frontier.submitted_draws + 1,
      kGpuDiagnostics && !kernel_state_ && REXCVAR_GET(embedded_camera_draw_capture) &&
          REXCVAR_GET(embedded_camera_scene_capture) && IsCurrentEmbeddedGameplayCaptureFrame());
  if (kGpuDiagnostics) {
    render_target_cache_->SetSceneUpdateCapture(scene_update.budget ? &scene_update : nullptr);
  }
  if (sample_draw) sampled_draw_scope.MarkPreTexturePart();
  // Whether the host draw may leave covered samples unwritten (the render
  // target cache then keeps every transfer into what it claims).
  bool draw_may_discard_samples = false;
  if (pixel_shader) {
    auto rb_colorcontrol = regs.Get<reg::RB_COLORCONTROL>();
    draw_may_discard_samples =
        pixel_shader->kills_pixels() || rb_colorcontrol.alpha_to_mask_enable ||
        (rb_colorcontrol.alpha_test_enable &&
         rb_colorcontrol.alpha_func != xenos::CompareFunction::kAlways);
  }
  render_target_cache_->SetDrawMayDiscardSamples(draw_may_discard_samples);
  const bool render_target_update_succeeded = render_target_cache_->Update(
      is_rasterization_done, normalized_depth_control, normalized_color_mask,
      *vertex_shader, native_shader_grid);
  if (sample_draw) sampled_draw_scope.MarkPreTexturePart();
  if (kGpuDiagnostics) render_target_cache_->SetSceneUpdateCapture(nullptr);
  if (kGpuDiagnostics && render_target_update_succeeded &&
      render_target_cache_->LastUpdateReused()) {
    swap_intervals_.AddRenderTargetUpdateReuse();
  }
  if (kGpuDiagnostics && scene_update.budget) {
    std::fprintf(stderr,
        "REX_SCENE_UPDATE_END frame=%llu update=%llu next_draw=%llu succeeded=%u helper_completed=%u "
        "targets=%u owners=%u planned=%u commands=%u depth_stores=%u failures=%u records=%u dropped=%u "
        "submission=%llu scope=ownership_and_queued_transfers_not_gpu_completion\n",
        static_cast<unsigned long long>(scene_update.frame),
        static_cast<unsigned long long>(scene_update.update),
        static_cast<unsigned long long>(scene_update.next_draw), render_target_update_succeeded ? 1u : 0u,
        scene_update.helper_completed ? 1u : 0u, scene_update.targets, scene_update.owners,
        scene_update.planned, scene_update.commands, scene_update.depth_stores, scene_update.failures,
        scene_update.records, scene_update.dropped, static_cast<unsigned long long>(GetCurrentSubmission()));
  }
  if (record_embedded_timing) {
    const uint64_t elapsed_us =
        uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
                     std::chrono::steady_clock::now() -
                     render_target_update_start)
                     .count());
    ++embedded_frame_frontier.render_target_update_calls;
    cp_cadence_.DrawStageTime(0, elapsed_us);
    embedded_frame_frontier.render_target_update_us += elapsed_us;
    embedded_frame_frontier.render_target_update_max_us = std::max(
        embedded_frame_frontier.render_target_update_max_us, elapsed_us);
  }
  if (!render_target_update_succeeded) {
    return fail_embedded_draw("render_target_update");
  }
  if (embedded_frame_dump_file) {
    EmbeddedFrameDumpDraw(regs, *vertex_shader, pixel_shader, uint32_t(primitive_type),
                          primitive_processing_result.host_draw_vertex_count, bin_select_,
                          bin_mask_);
  }

  // Ownership transfers have finished, but no guest pipeline has been bound or
  // draw issued yet. An immediately preceding captured clear may now be viewed
  // through 2x MSAA. Preserve this epoch before geometry can change its samples.
  const bool camera_depth_clear_enabled = kGpuDiagnostics && !kernel_state_ &&
      REXCVAR_GET(embedded_camera_draw_capture) && REXCVAR_GET(embedded_camera_geometry_capture) &&
      REXCVAR_GET(embedded_camera_depth_clear_capture);
  embedded_camera_depth_clear_policy::Draw camera_depth_draw;
  if (camera_depth_clear_enabled) {
    camera_depth_draw = {embedded_completed_swap_ordinal + 1,
        embedded_frame_frontier.submitted_draws + 1, vertex_shader->ucode_data_hash(),
        pixel_shader ? pixel_shader->ucode_data_hash() : 0,
        IsCurrentEmbeddedGameplayCaptureFrame(), memexport_used,
        regs[XE_GPU_REG_RB_SURFACE_INFO], regs[XE_GPU_REG_RB_DEPTH_INFO],
        normalized_depth_control.value, uint32_t(primitive_type),
        primitive_processing_result.host_draw_vertex_count,
        render_target_cache_->GetDrawScaleX(), render_target_cache_->GetDrawScaleY()};
    const uint64_t clear_draw = embedded_camera_depth_clear_budget.AfterAlias(camera_depth_draw);
    if (clear_draw) {
      render_target_cache_->QueueCameraDepthClearReadback(camera_depth_draw.frame,
          camera_depth_draw.ordinal, clear_draw, true);
    }
  }

  // Update selects the authoritative per-draw scale class. In particular, an
  // ownership-compatible stencil-only alias may retain a scaled depth target
  // even when its pitch is below the native threshold. Build the vertex
  // modification only after that decision so shader coordinates agree with
  // the bound render target and the dynamic viewport/scissor state.
  DxbcShaderTranslator::Modification vertex_shader_modification =
      pipeline_cache_->GetCurrentVertexShaderModification(
          *vertex_shader, primitive_processing_result.host_vertex_shader_type,
          interpolator_mask);

  // Depth ownership transfers performed by Update may prove that the bound
  // D24FS8 target no longer has its original host float32 depth. Select the
  // pixel-shader depth modification only after that state is known.
  DxbcShaderTranslator::Modification pixel_shader_modification =
      pixel_shader
          ? pipeline_cache_->GetCurrentPixelShaderModification(
                *pixel_shader, interpolator_mask, ps_param_gen_pos,
                normalized_depth_control)
          : DxbcShaderTranslator::Modification(0);
  pixel_shader_modification.pixel.native_filter_fetch = native_filter_fetch;
  pixel_shader_modification.pixel.native_region_sampling =
      pixel_shader && texture_cache_->IsNativeResolveSamplingEnabled() ? 1u : 0u;

  // Probes 221-224 localize the internal-2x loss to draws that add a stencil
  // 0x80 test to an otherwise healthy depth-EQUAL path. Identify whether the
  // mask itself first diverges while the matching depth prepass is produced.
  // Read back statistics only (no binary dump) around the two prepass pairs
  // and immediately before each corresponding shaded group. This diagnostic
  // is intentionally exclusive with the query-based pair trace because a
  // synchronous target dump must never be recorded inside an open occlusion
  // query. It observes the actual draw sequence and changes no guest state.
  const bool trace_embedded_scene_depth_phases =
      !kernel_state_ && REXCVAR_GET(embedded_scene_depth_phase_trace) &&
      !REXCVAR_GET(embedded_scene_depth_pair_trace) &&
      IsCurrentEmbeddedGameplayCaptureFrame() &&
      regs.Get<reg::RB_SURFACE_INFO>().value == 0x14010500 &&
      regs.Get<reg::RB_DEPTH_INFO>().value == 0x00010000;
  const uint64_t embedded_scene_phase_vertex_shader_hash =
      vertex_shader->ucode_data_hash();
  const uint64_t embedded_scene_phase_pixel_shader_hash =
      pixel_shader ? pixel_shader->ucode_data_hash() : 0;
  const bool embedded_scene_phase_prepass_draw =
      trace_embedded_scene_depth_phases &&
      embedded_scene_phase_vertex_shader_hash ==
          UINT64_C(0x52472DD3CF459B83) &&
      embedded_scene_phase_pixel_shader_hash == 0 &&
      normalized_depth_control.value == 0x18708767;
  const bool embedded_scene_phase_shaded_draw =
      trace_embedded_scene_depth_phases &&
      embedded_scene_phase_vertex_shader_hash ==
          UINT64_C(0x818A2B33A7AB4ECE) &&
      embedded_scene_phase_pixel_shader_hash ==
          UINT64_C(0xBE763931E2AB7D56) &&
      normalized_depth_control.value == 0x18700227;
  const bool embedded_scene_phase_shadow_volume_draw =
      trace_embedded_scene_depth_phases &&
      embedded_scene_phase_vertex_shader_hash ==
          UINT64_C(0x3582185A37667147) &&
      embedded_scene_phase_pixel_shader_hash == 0 &&
      normalized_depth_control.value == 0x1871C793;
  static uint32_t embedded_scene_phase_prepass_count = 0;
  static uint32_t embedded_scene_phase_shaded_count = 0;
  static uint32_t embedded_scene_phase_shadow_volume_count = 0;
  if (embedded_scene_phase_prepass_draw &&
      (embedded_scene_phase_prepass_count == 0 ||
       embedded_scene_phase_prepass_count == 2)) {
    const char* label = embedded_scene_phase_prepass_count == 0
                            ? "before_prepass_pair_1"
                            : "before_prepass_pair_2";
    const bool captured =
        render_target_cache_->CaptureEmbeddedSceneDepthTarget(label, nullptr);
    std::fprintf(stderr,
                 "REX_EMBEDDED_SCENE_DEPTH_PHASE result=%u label=%s "
                 "prepass_seen=%u shaded_seen=%u\n",
                 captured ? 1u : 0u, label,
                 embedded_scene_phase_prepass_count,
                 embedded_scene_phase_shaded_count);
    std::fflush(stderr);
  }
  if (embedded_scene_phase_shaded_draw &&
      (embedded_scene_phase_shaded_count == 0 ||
       embedded_scene_phase_shaded_count == 4)) {
    const char* label = embedded_scene_phase_shaded_count == 0
                            ? "before_shaded_group_1"
                            : "before_shaded_group_2";
    const bool captured =
        render_target_cache_->CaptureEmbeddedSceneDepthTarget(label, nullptr);
    std::fprintf(stderr,
                 "REX_EMBEDDED_SCENE_DEPTH_PHASE result=%u label=%s "
                 "prepass_seen=%u shaded_seen=%u\n",
                 captured ? 1u : 0u, label,
                 embedded_scene_phase_prepass_count,
                 embedded_scene_phase_shaded_count);
    std::fflush(stderr);
  }
  if (embedded_scene_phase_shadow_volume_draw &&
      (embedded_scene_phase_shadow_volume_count == 0 ||
       embedded_scene_phase_shadow_volume_count == 4)) {
    const char* label = embedded_scene_phase_shadow_volume_count == 0
                            ? "before_shadow_volume_group_1"
                            : "before_shadow_volume_group_2";
    const bool captured =
        render_target_cache_->CaptureEmbeddedSceneDepthTarget(label, nullptr);
    std::fprintf(stderr,
                 "REX_EMBEDDED_SCENE_DEPTH_PHASE result=%u label=%s "
                 "shadow_seen=%u prepass_seen=%u shaded_seen=%u\n",
                 captured ? 1u : 0u, label,
                 embedded_scene_phase_shadow_volume_count,
                 embedded_scene_phase_prepass_count,
                 embedded_scene_phase_shaded_count);
    std::fflush(stderr);
  }

  // Probe163 proved that this exact draw consumes a scaled D24FS8 target whose
  // ownership comes from a native 4x-MSAA depth target. Snapshot the packed
  // target after Update has completed all natural ownership/depth work, but
  // before the unconditional color draw. This is one-shot, opt-in and keyed by
  // shader plus complete target/depth state so unrelated title draws are not
  // captured.
  const bool capture_embedded_scene_depth_snapshot =
      !kernel_state_ &&
      REXCVAR_GET(embedded_scene_depth_snapshot_capture) &&
      IsCurrentEmbeddedGameplayCaptureFrame() && pixel_shader &&
      pixel_shader->ucode_data_hash() == UINT64_C(0xBE763931E2AB7D56) &&
      regs.Get<reg::RB_SURFACE_INFO>().value == 0x14010500 &&
      regs.Get<reg::RB_COLOR_INFO>().value == 0x00030300 &&
      regs.Get<reg::RB_DEPTH_INFO>().value == 0x00010000 &&
      normalized_depth_control.value == 0x18700227;
  if (capture_embedded_scene_depth_snapshot) {
    static bool embedded_scene_depth_snapshot_attempted = false;
    if (!embedded_scene_depth_snapshot_attempted) {
      embedded_scene_depth_snapshot_attempted = true;
      constexpr char kSceneDepthSnapshotPath[] =
          "rex_scene_depth_before_first_corrupting_draw.bin";
      const bool captured =
          render_target_cache_->CaptureEmbeddedSceneDepthTarget(
              "scene_depth_before_first_corrupting_draw",
              kSceneDepthSnapshotPath);
      std::fprintf(
          stderr,
          "REX_EMBEDDED_SCENE_DEPTH_SNAPSHOT result=%u path=%s "
          "ps=0x%016llX depth_control=0x%08X\n",
          captured ? 1u : 0u, kSceneDepthSnapshotPath,
          static_cast<unsigned long long>(pixel_shader->ucode_data_hash()),
          normalized_depth_control.value);
      std::fflush(stderr);
    }
  }
  const uint32_t current_draw_scale_area =
      render_target_cache_->GetDrawScaleX() * render_target_cache_->GetDrawScaleY();
  if (!UpdateGuestOcclusionQueryScale(current_draw_scale_area)) {
    return fail_embedded_draw("occlusion_scale_transition");
  }

  // Create the pipeline (for this, need the actually used render target formats
  // from the render target cache), translating the shaders - doing this now to
  // obtain the used textures.
  D3D12Shader::D3D12Translation* vertex_shader_translation =
      static_cast<D3D12Shader::D3D12Translation*>(
          vertex_shader->GetOrCreateTranslation(vertex_shader_modification.value));
  D3D12Shader::D3D12Translation* pixel_shader_translation =
      pixel_shader ? static_cast<D3D12Shader::D3D12Translation*>(
                         pixel_shader->GetOrCreateTranslation(pixel_shader_modification.value))
                   : nullptr;
  uint32_t bound_depth_and_color_render_target_bits;
  uint32_t bound_depth_and_color_render_target_formats[1 + xenos::kMaxColorRenderTargets]{};
  bool host_render_targets_used =
      render_target_cache_->GetPath() == RenderTargetCache::Path::kHostRenderTargets;
  if (host_render_targets_used) {
    bound_depth_and_color_render_target_bits =
        render_target_cache_->GetLastUpdateBoundRenderTargets(
            bound_depth_and_color_render_target_formats);
  } else {
    bound_depth_and_color_render_target_bits = 0;
  }
  void* pipeline_handle;
  ID3D12RootSignature* root_signature;
  const auto pipeline_configure_start =
      record_embedded_timing ? std::chrono::steady_clock::now()
                             : std::chrono::steady_clock::time_point{};
  if (sample_draw) sampled_draw_scope.MarkPreTexturePart();
  const bool pipeline_configure_succeeded = pipeline_cache_->ConfigurePipeline(
      vertex_shader_translation, pixel_shader_translation,
      primitive_processing_result, normalized_depth_control,
      normalized_color_mask, bound_depth_and_color_render_target_bits,
      bound_depth_and_color_render_target_formats, &pipeline_handle,
      &root_signature);
  if (sample_draw) sampled_draw_scope.MarkPreTexturePart();
  if (record_embedded_timing) {
    const uint64_t elapsed_us =
        uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
                     std::chrono::steady_clock::now() -
                     pipeline_configure_start)
                     .count());
    ++embedded_frame_frontier.pipeline_configure_calls;
    cp_cadence_.DrawStageTime(1, elapsed_us);
    embedded_frame_frontier.pipeline_configure_us += elapsed_us;
    embedded_frame_frontier.pipeline_configure_max_us = std::max(
        embedded_frame_frontier.pipeline_configure_max_us, elapsed_us);
  }
  if (!pipeline_configure_succeeded) {
    return fail_embedded_draw("configure_pipeline");
  }
  if (REXCVAR_GET(async_shader_compilation) &&
      pipeline_cache_->GetD3D12PipelineByHandle(pipeline_handle) == nullptr) {
    if (kGpuDiagnostics && !kernel_state_ && texture_cache_->OwnsSceneHistory()) {
      // The empty failure record is large too; keep it off the normal stack.
      const auto invalidate_owned_transform = [this]() __attribute__((noinline)) {
        texture_cache_->RecordOwnedDrawTransform({}, false);
      };
      invalidate_owned_transform();
    }
    if (!kernel_state_ && IsEmbeddedFrameDiagnosticsEnabled()) {
      ++embedded_frame_frontier.pipeline_placeholder_draws;
    }
    return true;
  }

  // Update the textures - this may bind pipelines.
  uint32_t used_texture_mask =
      vertex_shader->GetUsedTextureMaskAfterTranslation() |
      (pixel_shader != nullptr ? pixel_shader->GetUsedTextureMaskAfterTranslation() : 0);
  const auto texture_request_start =
      record_embedded_timing ? std::chrono::steady_clock::now()
                             : std::chrono::steady_clock::time_point{};
  texture_cache_->RequestTextures(used_texture_mask);
  if (temporal_aa_enabled_) {
    TemporalAaDraw(vertex_shader->ucode_data_hash(),
                   pixel_shader ? pixel_shader->ucode_data_hash() : 0, normalized_depth_control);
  }
  if (!kernel_state_ && IsEmbeddedFrameDiagnosticsEnabled()) {
    texture_cache_->CaptureFirstTemporalDepthBinding(
        used_texture_mask, embedded_frame_frontier.submitted_draws + 1);
  }
  if (record_embedded_timing) {
    const uint64_t elapsed_us =
        uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
                     std::chrono::steady_clock::now() -
                     texture_request_start)
                     .count());
    ++embedded_frame_frontier.texture_request_calls;
    cp_cadence_.DrawStageTime(2, elapsed_us);
    embedded_frame_frontier.texture_request_us += elapsed_us;
    embedded_frame_frontier.texture_request_max_us = std::max(
        embedded_frame_frontier.texture_request_max_us, elapsed_us);
  }
  if (sample_draw) sampled_draw_scope.MarkPreTexturePart();
  if (sample_draw) sampled_draw_scope.Advance();

  // Probes271/272 preserve black in assembled scene color, but the 2x RGB-map
  // has no black entry and the pre-gamma output lifts black. Observe the three
  // actual map producers and their final consumer, never substituting data.
  // The same small allocation is rewritten between producers, so explicitly
  // distinguish bounded draw epochs in the generic resource snapshot budget.
  uint32_t embedded_color_map_capture_stage = 0;
  if (!kernel_state_ && REXCVAR_GET(embedded_color_map_chain_capture) &&
      IsCurrentEmbeddedGameplayCaptureFrame() && pixel_shader &&
      regs.Get<reg::RB_SURFACE_INFO>().value == 0x14000500 &&
      regs.Get<reg::RB_COLOR_INFO>().value == 0x000302D0 &&
      normalized_color_mask == 0xF) {
    constexpr uint64_t kColorMapShaders[] = {
        UINT64_C(0xFDC5E32EC6045BE1), UINT64_C(0x37AC93F53126ABB7),
        UINT64_C(0x54D655FC471D594A), UINT64_C(0xA59B41D0BD79484B)};
    static uint32_t color_map_capture_mask = 0;
    for (uint32_t stage = 0; stage < 4; ++stage) {
      if (pixel_shader->ucode_data_hash() == kColorMapShaders[stage] &&
          !(color_map_capture_mask & (1u << stage))) {
        color_map_capture_mask |= 1u << stage;
        embedded_color_map_capture_stage = stage + 1;
        break;
      }
    }
  }
  if (embedded_color_map_capture_stage) {
    const uint64_t draw_ordinal = embedded_frame_frontier.submitted_draws + 1;
    std::fprintf(stderr,
                 "REX_EMBEDDED_COLOR_MAP_DRAW stage=%u draw=%llu "
                 "vs=0x%016llX ps=0x%016llX used=0x%08X scale=%ux%u "
                 "surface=%08X color=%08X depth=%08X blend=%08X\n",
                 embedded_color_map_capture_stage,
                 static_cast<unsigned long long>(draw_ordinal),
                 static_cast<unsigned long long>(vertex_shader->ucode_data_hash()),
                 static_cast<unsigned long long>(pixel_shader->ucode_data_hash()),
                 used_texture_mask, render_target_cache_->GetDrawScaleX(),
                 render_target_cache_->GetDrawScaleY(),
                 regs[XE_GPU_REG_RB_SURFACE_INFO], regs[XE_GPU_REG_RB_COLOR_INFO],
                 normalized_depth_control.value, regs[XE_GPU_REG_RB_BLENDCONTROL0]);
    for (uint32_t stage = 0; stage < 2; ++stage) {
      const Shader& shader = stage ? *pixel_shader : *vertex_shader;
      const auto& map = shader.constant_register_map();
      uint32_t logged = 0;
      for (uint32_t word = 0; word < 4 && logged < 64; ++word) {
        uint64_t bitmap = map.float_bitmap[word];
        uint32_t bit;
        while (logged < 64 && rex::bit_scan_forward(bitmap, &bit)) {
          bitmap &= ~(UINT64_C(1) << bit);
          const uint32_t index = word * 64 + bit;
          const uint32_t address = (stage ? XE_GPU_REG_SHADER_CONSTANT_256_X
                                          : XE_GPU_REG_SHADER_CONSTANT_000_X) + index * 4;
          std::fprintf(stderr,
                       "REX_EMBEDDED_COLOR_MAP_CONSTANT stage=%u shader=%s c=%u "
                       "raw=%08X,%08X,%08X,%08X\n",
                       embedded_color_map_capture_stage, stage ? "ps" : "vs", index,
                       regs[address], regs[address + 1], regs[address + 2], regs[address + 3]);
          ++logged;
        }
      }
    }
    uint32_t slots[32];
    size_t slot_count = 0;
    for (uint32_t slot = 0; slot < 32; ++slot) {
      if (!(used_texture_mask & (1u << slot))) continue;
      slots[slot_count++] = slot;
      const auto fetch = regs.GetTextureFetch(slot);
      std::fprintf(stderr,
                   "REX_EMBEDDED_COLOR_MAP_FETCH stage=%u slot=%u "
                   "raw=%08X,%08X,%08X,%08X,%08X,%08X\n",
                   embedded_color_map_capture_stage, slot, fetch.dword_0, fetch.dword_1,
                   fetch.dword_2, fetch.dword_3, fetch.dword_4, fetch.dword_5);
    }
    texture_cache_->CaptureActiveTextureReadbackDiagnostics(
        slots, slot_count, false, draw_ordinal, true);
    std::fprintf(stderr,
                 "REX_EMBEDDED_COLOR_MAP_VIEWPORT stage=%u "
                 "raw=%08X,%08X,%08X,%08X,%08X,%08X vte=%08X raster=%08X\n",
                 embedded_color_map_capture_stage,
                 regs[XE_GPU_REG_PA_CL_VPORT_XSCALE], regs[XE_GPU_REG_PA_CL_VPORT_XOFFSET],
                 regs[XE_GPU_REG_PA_CL_VPORT_YSCALE], regs[XE_GPU_REG_PA_CL_VPORT_YOFFSET],
                 regs[XE_GPU_REG_PA_CL_VPORT_ZSCALE], regs[XE_GPU_REG_PA_CL_VPORT_ZOFFSET],
                 regs[XE_GPU_REG_PA_CL_VTE_CNTL], regs[XE_GPU_REG_PA_SU_VTX_CNTL]);
    uint32_t vertex_bindings_logged = 0;
    for (const auto& binding : vertex_shader->vertex_bindings()) {
      if (vertex_bindings_logged++ >= 4) break;
      const auto fetch = regs.GetVertexFetch(binding.fetch_constant);
      const uint32_t count = std::min(fetch.size, std::min(
          binding.stride_words <= 16 ? binding.stride_words * 6 : 0, 96u));
      if (!count || fetch.type != xenos::FetchConstantType::kVertex ||
          uint64_t(fetch.address) * 4 + count * 4 > UINT64_C(0x20000000)) continue;
      const uint8_t* source =
          memory_->TranslatePhysical<const uint8_t*>(fetch.address << 2);
      std::fprintf(stderr,
                   "REX_EMBEDDED_COLOR_MAP_VERTEX stage=%u slot=%u stride=%u "
                   "guest=0x%08X words=%u raw=",
                   embedded_color_map_capture_stage, binding.fetch_constant,
                   binding.stride_words, fetch.address << 2, count);
      for (uint32_t i = 0; i < count; ++i) {
        std::fprintf(stderr, "%s%08X", i ? "," : "",
                     xenos::GpuSwap(rex::memory::load<uint32_t>(source + i * 4), fetch.endian));
      }
      std::fputc('\n', stderr);
    }
    std::fflush(stderr);
  }

  // Probe170 proved that the first full-size scene-color resolve faithfully
  // preserves a source that is already uniform at 2x, while the native source
  // contains the title artwork. Capture the four resources at the earliest
  // affected shaded draw, specifically its second variant where slot 4 is a
  // scaled resolve. This is one-shot, opt-in and uses semantic GPU state rather
  // than allocator addresses.
  const xenos::xe_gpu_texture_fetch_t scene_shaded_effect_fetch =
      regs.GetTextureFetch(4);
  const bool capture_embedded_scene_shaded_textures =
      !kernel_state_ &&
      REXCVAR_GET(embedded_scene_shaded_texture_capture) &&
      IsCurrentEmbeddedGameplayCaptureFrame() && pixel_shader &&
      pixel_shader->ucode_data_hash() == UINT64_C(0xBE763931E2AB7D56) &&
      used_texture_mask == 0x00000017 &&
      regs.Get<reg::RB_SURFACE_INFO>().value == 0x14010500 &&
      regs.Get<reg::RB_COLOR_INFO>().value == 0x00030300 &&
      regs.Get<reg::RB_DEPTH_INFO>().value == 0x00010000 &&
      normalized_depth_control.value == 0x18700227 &&
      scene_shaded_effect_fetch.format == xenos::TextureFormat::k_8_8_8_8 &&
      scene_shaded_effect_fetch.endianness == xenos::Endian::k8in32 &&
      scene_shaded_effect_fetch.size_2d.width + 1 == 256 &&
      scene_shaded_effect_fetch.size_2d.height + 1 == 256;
  if (capture_embedded_scene_shaded_textures) {
    static bool embedded_scene_shaded_textures_queued = false;
    if (!embedded_scene_shaded_textures_queued) {
      embedded_scene_shaded_textures_queued = true;
      constexpr uint32_t kSceneShadedTextureSlots[] = {0, 1, 2, 4};
      std::fprintf(
          stderr,
          "REX_EMBEDDED_SCENE_SHADED_TEXTURES result=queued "
          "ps=0x%016llX used=0x%08X scale=%ux%u\n",
          static_cast<unsigned long long>(pixel_shader->ucode_data_hash()),
          used_texture_mask, render_target_cache_->GetDrawScaleX(),
          render_target_cache_->GetDrawScaleY());
      for (uint32_t texture_slot : kSceneShadedTextureSlots) {
        const xenos::xe_gpu_texture_fetch_t fetch =
            regs.GetTextureFetch(texture_slot);
        D3D12TextureCache::ActiveTextureDiagnostic diagnostic;
        const bool valid = texture_cache_->GetActiveTextureDiagnostic(
            texture_slot, diagnostic);
        std::fprintf(
            stderr,
            "REX_EMBEDDED_SCENE_SHADED_TEXTURE slot=%u valid=%u "
            "fetch=%08X,%08X,%08X,%08X,%08X,%08X guest=0x%08X+0x%X "
            "guest_extent=%ux%u guest_format=%u scaled=%u outdated=0x%X "
            "guest_array=%u host_extent=%llux%ux%u host_format=%u "
            "descriptor=%u\n",
            texture_slot, valid ? 1u : 0u, fetch.dword_0, fetch.dword_1,
            fetch.dword_2, fetch.dword_3, fetch.dword_4, fetch.dword_5,
            diagnostic.guest_base, diagnostic.guest_size,
            diagnostic.guest_width, diagnostic.guest_height,
            diagnostic.guest_format, diagnostic.scaled_resolve,
            diagnostic.outdated_mask,
            diagnostic.guest_depth_or_array_size,
            static_cast<unsigned long long>(diagnostic.resource_width),
            diagnostic.resource_height,
            diagnostic.resource_depth_or_array_size,
            diagnostic.resource_format,
            diagnostic.descriptor_index);
      }
      texture_cache_->CaptureActiveTextureReadbackDiagnostics(
          kSceneShadedTextureSlots,
          sizeof(kSceneShadedTextureSlots) /
              sizeof(kSceneShadedTextureSlots[0]));
      std::fflush(stderr);
    }
  }

  // The first full-frame resolve itself is coherent at 2x. The next bounded
  // boundary is the two half-frame draws that seed the HDR scene target from
  // one ordinary (not scaled-resolve) texture. Capture the input once here,
  // after texture residency is established but before the draw, then capture
  // the host target after each draw below. The shader and complete Xenos
  // target signature make this independent of guest allocator addresses.
  const bool capture_embedded_scene_seed_draw =
      !kernel_state_ && REXCVAR_GET(embedded_scene_seed_draw_capture) &&
      IsCurrentEmbeddedGameplayCaptureFrame() && pixel_shader &&
      pixel_shader->ucode_data_hash() == UINT64_C(0xAC56A81E9E6339F7) &&
      used_texture_mask == 0x00000001 &&
      regs.Get<reg::RB_SURFACE_INFO>().value == 0x14010500 &&
      regs.Get<reg::RB_COLOR_INFO>().value == 0x000C0300;
  if (capture_embedded_scene_seed_draw) {
    static bool embedded_scene_seed_input_capture_queued = false;
    if (!embedded_scene_seed_input_capture_queued) {
      embedded_scene_seed_input_capture_queued = true;
      constexpr uint32_t kSceneSeedTextureSlot = 0;
      texture_cache_->CaptureActiveTextureReadbackDiagnostics(
          &kSceneSeedTextureSlot, 1);
      const xenos::xe_gpu_texture_fetch_t seed_fetch =
          regs.GetTextureFetch(kSceneSeedTextureSlot);
      std::fprintf(
          stderr,
          "REX_EMBEDDED_SCENE_SEED_INPUT_CAPTURE result=queued "
          "ps=0x%016llX surface=0x%08X color=0x%08X "
          "fetch=%08X,%08X,%08X,%08X,%08X,%08X\n",
          static_cast<unsigned long long>(pixel_shader->ucode_data_hash()),
          regs[XE_GPU_REG_RB_SURFACE_INFO], regs[XE_GPU_REG_RB_COLOR_INFO],
          seed_fetch.dword_0, seed_fetch.dword_1, seed_fetch.dword_2,
          seed_fetch.dword_3, seed_fetch.dword_4, seed_fetch.dword_5);
      std::fflush(stderr);
    }
  }

  // Probe 158 proved that the scene-seed target is already corrupt before its
  // first blend draw. The immediately preceding ownership chain writes EDRAM
  // base 0x300 through a native 640-pixel 4x-MSAA target and then rebinds the
  // same allocation as a scaled 1280-pixel 2x-MSAA target. Capture both sides
  // of that semantic transition without depending on allocator addresses.
  const bool capture_embedded_mixed_scale_native_draw =
      !kernel_state_ &&
      REXCVAR_GET(embedded_mixed_scale_transition_capture) &&
      IsCurrentEmbeddedGameplayCaptureFrame() && pixel_shader &&
      pixel_shader->ucode_data_hash() == UINT64_C(0x2E372EA28CC404B7) &&
      regs.Get<reg::RB_SURFACE_INFO>().value == 0x0A020280 &&
      regs.Get<reg::RB_COLOR_INFO>().value == 0x00030300;
  const bool capture_embedded_mixed_scale_scaled_draw =
      !kernel_state_ &&
      REXCVAR_GET(embedded_mixed_scale_transition_capture) &&
      IsCurrentEmbeddedGameplayCaptureFrame() && pixel_shader &&
      pixel_shader->ucode_data_hash() == UINT64_C(0x7EF6E3B55D32AEB4) &&
      regs.Get<reg::RB_SURFACE_INFO>().value == 0x14010500 &&
      regs.Get<reg::RB_COLOR_INFO>().value == 0x00030300;
  const bool capture_embedded_scene_chain_draw =
      !kernel_state_ && REXCVAR_GET(embedded_scene_chain_capture) &&
      IsCurrentEmbeddedGameplayCaptureFrame() && pixel_shader &&
      regs.Get<reg::RB_SURFACE_INFO>().value == 0x14010500 &&
      regs.Get<reg::RB_COLOR_INFO>().value == 0x00030300 &&
      normalized_color_mask != 0;
  const bool summarize_embedded_scene_chain_late_draw =
      !kernel_state_ && REXCVAR_GET(embedded_scene_chain_late_summary) &&
      IsCurrentEmbeddedGameplayCaptureFrame() && pixel_shader &&
      regs.Get<reg::RB_SURFACE_INFO>().value == 0x14010500 &&
      regs.Get<reg::RB_COLOR_INFO>().value == 0x00030300 &&
      normalized_color_mask != 0;
  const bool capture_embedded_scene_chain_checkpoint_draw =
      !kernel_state_ &&
      REXCVAR_GET(embedded_scene_chain_checkpoint_capture) &&
      IsCurrentEmbeddedGameplayCaptureFrame() && pixel_shader &&
      regs.Get<reg::RB_SURFACE_INFO>().value == 0x14010500 &&
      regs.Get<reg::RB_COLOR_INFO>().value == 0x00030300 &&
      normalized_color_mask != 0;
  const bool capture_embedded_simple_title_composition_draw =
      !kernel_state_ &&
      REXCVAR_GET(embedded_simple_title_composition_capture) &&
      IsCurrentEmbeddedGameplayCaptureFrame() && pixel_shader &&
      pixel_shader->ucode_data_hash() == UINT64_C(0x5AD41773AF82E9ED) &&
      used_texture_mask == 0x00000003 &&
      regs.Get<reg::RB_SURFACE_INFO>().value == 0x14020500 &&
      regs.Get<reg::RB_COLOR_INFO>().value == 0x00030400 &&
      normalized_color_mask == 0x0000000F;

  // Probe 164 proved that all four scaled parity classes contain the expected
  // stencil reference immediately before the first corrupting scene draw. The
  // remaining exact-depth question is whether the matching prepass and shaded
  // draw receive identical position inputs. Hash the authoritative guest
  // vertex/index bytes and the complete vertex constant bank for the three
  // exact draws in that pair. This is bounded, opt-in and observational; it
  // neither changes pipeline state nor duplicates a draw.
  if (!kernel_state_ &&
      (REXCVAR_GET(embedded_scene_depth_pair_trace) ||
       REXCVAR_GET(embedded_scene_vertex_output_trace)) &&
      IsCurrentEmbeddedGameplayCaptureFrame() &&
      regs.Get<reg::RB_SURFACE_INFO>().value == 0x14010500) {
    const uint64_t vertex_shader_hash = vertex_shader->ucode_data_hash();
    const uint64_t pixel_shader_hash =
        pixel_shader ? pixel_shader->ucode_data_hash() : 0;
    const char* pair_kind = nullptr;
    if (vertex_shader_hash == UINT64_C(0x52472DD3CF459B83) &&
        pixel_shader_hash == 0 &&
        normalized_depth_control.value == 0x18708767) {
      pair_kind = "prepass";
    } else if (vertex_shader_hash == UINT64_C(0x539FB8DE2DD7715A) &&
               pixel_shader_hash == UINT64_C(0x7EF6E3B55D32AEB4) &&
               normalized_depth_control.value == 0x18708722) {
      pair_kind = "equal_control";
    } else if (vertex_shader_hash == UINT64_C(0x818A2B33A7AB4ECE) &&
               pixel_shader_hash == UINT64_C(0xBE763931E2AB7D56) &&
               normalized_depth_control.value == 0x18700227) {
      pair_kind = "equal_shaded";
    }
    static uint32_t embedded_scene_vertex_output_trace_count = 0;
    if (pair_kind && REXCVAR_GET(embedded_scene_vertex_output_trace) &&
        embedded_scene_vertex_output_trace_count < 8) {
      ++embedded_scene_vertex_output_trace_count;
      TraceEmbeddedSceneVertexOutputs(
          regs, *memory_, *vertex_shader, pair_kind,
          render_target_cache_->GetDrawScaleX(),
          render_target_cache_->GetDrawScaleY());
    }

    static uint32_t embedded_scene_depth_pair_trace_count = 0;
    if (pair_kind && REXCVAR_GET(embedded_scene_depth_pair_trace) &&
        embedded_scene_depth_pair_trace_count < 16) {
      const auto hash_bytes = [](const uint8_t* bytes, uint32_t length) {
        uint64_t hash = UINT64_C(1469598103934665603);
        for (uint32_t i = 0; i < length; ++i) {
          hash = (hash ^ bytes[i]) * UINT64_C(1099511628211);
        }
        return hash;
      };
      const uint32_t trace_ordinal = ++embedded_scene_depth_pair_trace_count;

      // Record the effective host-pipeline inputs alongside the guest-memory
      // hashes below. The three draws use different shader translations, so
      // proving equal guest oPos values is not enough to rule out a pipeline
      // modification, attachment, MSAA fallback, or early-depth difference.
      // Derive the host sample description with the same rules used by
      // PipelineCache::CreateD3D12Pipeline, without changing any draw state.
      const auto surface_info = regs.Get<reg::RB_SURFACE_INFO>();
      const uint32_t guest_sample_count =
          uint32_t(1) << uint32_t(surface_info.msaa_samples);
      const bool msaa_2x_supported = render_target_cache_->msaa_2x_supported();
      const bool msaa_2x_fallback =
          host_render_targets_used && guest_sample_count == 2 &&
          !msaa_2x_supported;
      const uint32_t host_sample_count =
          host_render_targets_used
              ? (msaa_2x_fallback ? 4u : guest_sample_count)
              : 1u;
      const uint32_t host_sample_mask =
          msaa_2x_fallback ? 0b1001u : UINT32_MAX;
      const uint32_t programmable_sample_positions_tier = uint32_t(
          GetD3D12Provider().GetProgrammableSamplePositionsTier());
      const uint32_t ps_kills =
          pixel_shader && pixel_shader->kills_pixels() ? 1u : 0u;
      const uint32_t ps_writes_depth =
          pixel_shader && pixel_shader->writes_depth() ? 1u : 0u;
      const uint32_t ps_implicit_early =
          pixel_shader && pixel_shader->implicit_early_z_write_allowed() ? 1u
                                                                          : 0u;
      // The matching prepass and EQUAL-tested draws may still diverge after
      // the vertex shader if their effective rasterizer or viewport state is
      // different. Capture both the raw Xenos polygon-offset registers and
      // the values passed to D3D12. Resolution scaling multiplies the slope
      // bias in PipelineCache::CreateD3D12Pipeline, so record the value on
      // both sides of that multiplication.
      const auto raster_control = regs.Get<reg::PA_SU_SC_MODE_CNTL>();
      const float polygon_offset_front_scale =
          regs.Get<float>(XE_GPU_REG_PA_SU_POLY_OFFSET_FRONT_SCALE);
      const float polygon_offset_front_offset =
          regs.Get<float>(XE_GPU_REG_PA_SU_POLY_OFFSET_FRONT_OFFSET);
      const float polygon_offset_back_scale =
          regs.Get<float>(XE_GPU_REG_PA_SU_POLY_OFFSET_BACK_SCALE);
      const float polygon_offset_back_offset =
          regs.Get<float>(XE_GPU_REG_PA_SU_POLY_OFFSET_BACK_OFFSET);
      float preferred_polygon_offset_scale = 0.0f;
      float preferred_polygon_offset = 0.0f;
      draw_util::GetPreferredFacePolygonOffset(
          regs, primitive_polygonal, preferred_polygon_offset_scale,
          preferred_polygon_offset);
      const int32_t host_depth_bias =
          host_render_targets_used
              ? draw_util::GetD3D10IntegerPolygonOffset(
                    regs.Get<reg::RB_DEPTH_INFO>().depth_format,
                    preferred_polygon_offset)
              : 0;
      const float host_slope_depth_bias_unscaled =
          host_render_targets_used
              ? preferred_polygon_offset_scale *
                    xenos::kPolygonOffsetScaleSubpixelUnit
              : 0.0f;
      const uint32_t draw_scale_x = render_target_cache_->GetDrawScaleX();
      const uint32_t draw_scale_y = render_target_cache_->GetDrawScaleY();
      const float host_slope_depth_bias =
          host_slope_depth_bias_unscaled *
          ((draw_scale_x == 1 && draw_scale_y == 1)
               ? 1.0f
               : float(std::max(draw_scale_x, draw_scale_y)));
      const bool diagnostic_convert_z_to_float24 =
          host_render_targets_used &&
          render_target_cache_
              ->current_draw_depth_float24_convert_in_pixel_shader();
      draw_util::ViewportInfo diagnostic_viewport_info;
      draw_util::GetHostViewportInfo(
          regs, draw_scale_x, draw_scale_y, true,
          D3D12_VIEWPORT_BOUNDS_MAX, D3D12_VIEWPORT_BOUNDS_MAX, false,
          normalized_depth_control, diagnostic_convert_z_to_float24,
          host_render_targets_used, ps_writes_depth != 0,
          diagnostic_viewport_info);
      std::fprintf(
          stderr,
          "REX_EMBEDDED_SCENE_PIPELINE ordinal=%u kind=%s "
          "vs_mod=0x%016llX ps_mod=0x%016llX interpolators=0x%08X "
          "ps_centroid=0x%08X ps_depth_mode=%u ps_kills=%u "
          "ps_writes_depth=%u ps_implicit_early=%u depth_raw=0x%08X "
          "depth_normalized=0x%08X depth_info=0x%08X color_control=0x%08X "
          "alpha_ref=0x%08X surface=0x%08X bound_bits=0x%08X "
          "bound_formats=%08X,%08X,%08X,%08X,%08X "
          "host_rts=%u guest_samples=%u host_samples=%u host_sample_mask=0x%08X "
          "msaa_2x_supported=%u programmable_sample_tier=%u "
          "float24_ps=%u depth_float24_round=%u "
          "primitive_polygonal=%u raster=0x%08X cull=%u,%u face=%u "
          "poly_enable=%u,%u,%u depth_clip=%u "
          "poly_front=%a,%a poly_back=%a,%a poly_preferred=%a,%a "
          "host_depth_bias=%d host_slope_bias_unscaled=%a "
          "host_slope_bias=%a host_viewport=%u,%u,%u,%u,%a,%a "
          "host_ndc=%a,%a,%a,%a,%a,%a scale=%ux%u\n",
          trace_ordinal, pair_kind,
          static_cast<unsigned long long>(vertex_shader_modification.value),
          static_cast<unsigned long long>(pixel_shader_modification.value),
          interpolator_mask, pixel_shader_modification.pixel.interpolators_centroid,
          uint32_t(pixel_shader_modification.pixel.depth_stencil_mode),
          ps_kills, ps_writes_depth, ps_implicit_early,
          regs[XE_GPU_REG_RB_DEPTHCONTROL], normalized_depth_control.value,
          regs[XE_GPU_REG_RB_DEPTH_INFO], regs[XE_GPU_REG_RB_COLORCONTROL],
          regs[XE_GPU_REG_RB_ALPHA_REF], surface_info.value,
          bound_depth_and_color_render_target_bits,
          bound_depth_and_color_render_target_formats[0],
          bound_depth_and_color_render_target_formats[1],
          bound_depth_and_color_render_target_formats[2],
          bound_depth_and_color_render_target_formats[3],
          bound_depth_and_color_render_target_formats[4],
          host_render_targets_used ? 1u : 0u, guest_sample_count,
          host_sample_count, host_sample_mask, msaa_2x_supported ? 1u : 0u,
          programmable_sample_positions_tier,
          render_target_cache_
                  ->current_draw_depth_float24_convert_in_pixel_shader()
              ? 1u
              : 0u,
          render_target_cache_->depth_float24_round() ? 1u : 0u,
          primitive_polygonal ? 1u : 0u, raster_control.value,
          raster_control.cull_front, raster_control.cull_back,
          raster_control.face, raster_control.poly_offset_front_enable,
          raster_control.poly_offset_back_enable,
          raster_control.poly_offset_para_enable,
          regs.Get<reg::PA_CL_CLIP_CNTL>().clip_disable ? 0u : 1u,
          polygon_offset_front_scale, polygon_offset_front_offset,
          polygon_offset_back_scale, polygon_offset_back_offset,
          preferred_polygon_offset_scale, preferred_polygon_offset,
          host_depth_bias, host_slope_depth_bias_unscaled,
          host_slope_depth_bias, diagnostic_viewport_info.xy_offset[0],
          diagnostic_viewport_info.xy_offset[1],
          diagnostic_viewport_info.xy_extent[0],
          diagnostic_viewport_info.xy_extent[1],
          diagnostic_viewport_info.z_min, diagnostic_viewport_info.z_max,
          diagnostic_viewport_info.ndc_scale[0],
          diagnostic_viewport_info.ndc_offset[0],
          diagnostic_viewport_info.ndc_scale[1],
          diagnostic_viewport_info.ndc_offset[1],
          diagnostic_viewport_info.ndc_scale[2],
          diagnostic_viewport_info.ndc_offset[2], draw_scale_x,
          draw_scale_y);

      constexpr uint32_t kVertexConstantDwordCount = 256 * 4;
      const uint32_t* vertex_constants =
          &regs[XE_GPU_REG_SHADER_CONSTANT_000_X];
      const uint64_t vertex_constants_hash = hash_bytes(
          reinterpret_cast<const uint8_t*>(vertex_constants),
          kVertexConstantDwordCount * sizeof(uint32_t));
      const auto hash_vertex_constant_range =
          [&hash_bytes, vertex_constants](uint32_t first, uint32_t count) {
            return hash_bytes(reinterpret_cast<const uint8_t*>(
                                  vertex_constants + first * 4),
                              count * 4 * uint32_t(sizeof(uint32_t)));
          };
      // These are the only constant ranges read by the identical guest oPos
      // instruction sequence shared by the three shaders. c78/c79 and the
      // other low constants feed texture/lighting outputs, not position.
      const uint64_t position_view_hash = hash_vertex_constant_range(0, 4);
      const uint64_t position_adjust_hash = hash_vertex_constant_range(7, 2);
      const uint64_t position_unpack_hash = hash_vertex_constant_range(76, 2);
      const uint64_t position_bones_hash = hash_vertex_constant_range(96, 160);

      const xenos::xe_gpu_vertex_fetch_t vertex_fetch =
          regs.GetVertexFetch(95);
      const uint32_t vertex_base = vertex_fetch.address << 2;
      uint32_t vertex_bytes = 0;
      uint64_t vertex_hash = 0;
      if (vertex_base < SharedMemory::kBufferSize) {
        vertex_bytes = std::min(vertex_fetch.size * uint32_t(sizeof(uint32_t)),
                                SharedMemory::kBufferSize - vertex_base);
        const uint8_t* vertex_data =
            memory_->TranslatePhysical<const uint8_t*>(vertex_base);
        if (vertex_data) {
          vertex_hash = hash_bytes(vertex_data, vertex_bytes);
        }
      }

      uint32_t index_bytes = 0;
      uint64_t index_hash = 0;
      uint64_t position_referenced_bones_hash =
          UINT64_C(1469598103934665603);
      uint32_t position_referenced_bone_count = 0;
      uint32_t position_invalid_bone_count = 0;
      std::array<uint64_t, 4> position_bone_offset_bitmap = {};
      const uint32_t index_base = primitive_processing_result.guest_index_base;
      if ((primitive_processing_result.index_buffer_type ==
               PrimitiveProcessor::ProcessedIndexBufferType::kGuestDMA ||
           primitive_processing_result.index_buffer_type ==
               PrimitiveProcessor::ProcessedIndexBufferType::kHostBuiltinForDMA) &&
          index_base < SharedMemory::kBufferSize) {
        const uint32_t index_size =
            primitive_processing_result.host_index_format ==
                    xenos::IndexFormat::kInt16
                ? 2u
                : 4u;
        index_bytes = std::min(index_count * index_size,
                               SharedMemory::kBufferSize - index_base);
        const uint8_t* index_data =
            memory_->TranslatePhysical<const uint8_t*>(index_base);
        if (index_data) {
          index_hash = hash_bytes(index_data, index_bytes);

          // All three shaders use the same seven-dword skinned-vertex layout:
          // word 5 contains four normalized U8 bone indices, multiplied by
          // c8.w before maxasf selects c[96 + a0] through c[98 + a0]. Hash
          // only the matrices reached by the current indexed draw, in index
          // order. This avoids treating unrelated entries in the dynamically
          // addressable c96-c255 bank as position input.
          constexpr uint32_t kPositionVertexStrideWords = 7;
          constexpr uint32_t kPositionBoneIndexWord = 5;
          float position_bone_index_scale;
          std::memcpy(&position_bone_index_scale, vertex_constants + 8 * 4 + 3,
                      sizeof(position_bone_index_scale));
          const uint32_t index_offset = regs.Get<reg::VGT_INDX_OFFSET>().indx_offset;
          const uint8_t* vertex_data =
              vertex_base < SharedMemory::kBufferSize
                  ? memory_->TranslatePhysical<const uint8_t*>(vertex_base)
                  : nullptr;
          const uint32_t available_vertex_count =
              vertex_bytes / (kPositionVertexStrideWords * uint32_t(sizeof(uint32_t)));
          for (uint32_t index_ordinal = 0;
               vertex_data && index_ordinal < index_count; ++index_ordinal) {
            uint32_t vertex_index;
            if (index_size == 2) {
              vertex_index = xenos::GpuSwap(
                  rex::memory::load<uint16_t>(index_data + index_ordinal * 2),
                  primitive_processing_result.host_shader_index_endian);
            } else {
              vertex_index =
                  xenos::GpuSwap(
                      rex::memory::load<uint32_t>(index_data + index_ordinal * 4),
                      primitive_processing_result.host_shader_index_endian) &
                  xenos::kVertexIndexMask;
            }
            vertex_index += index_offset;
            if (vertex_index >= available_vertex_count) {
              ++position_invalid_bone_count;
              continue;
            }
            const uint32_t raw_bone_indices = rex::memory::load<uint32_t>(
                vertex_data +
                (vertex_index * kPositionVertexStrideWords +
                 kPositionBoneIndexWord) *
                    sizeof(uint32_t));
            const uint32_t bone_indices =
                xenos::GpuSwap(raw_bone_indices, vertex_fetch.endian);
            for (uint32_t component = 0; component < 4; ++component) {
              const uint32_t bone_index_byte =
                  (bone_indices >> (component * 8)) & 0xFF;
              const float scaled_bone_offset =
                  (float(bone_index_byte) * (1.0f / 255.0f)) *
                  position_bone_index_scale;
              const uint32_t bone_offset = uint32_t(std::max(
                  0.0f, std::min(255.0f, std::floor(scaled_bone_offset))));
              position_bone_offset_bitmap[bone_offset >> 6] |=
                  UINT64_C(1) << (bone_offset & 63);
              if (bone_offset > 157) {
                ++position_invalid_bone_count;
                continue;
              }
              const uint8_t* matrix_bytes = reinterpret_cast<const uint8_t*>(
                  vertex_constants + (96 + bone_offset) * 4);
              for (uint32_t matrix_byte = 0;
                   matrix_byte < 3 * 4 * uint32_t(sizeof(uint32_t));
                   ++matrix_byte) {
                position_referenced_bones_hash =
                    (position_referenced_bones_hash ^ matrix_bytes[matrix_byte]) *
                    UINT64_C(1099511628211);
              }
              ++position_referenced_bone_count;
            }
          }
        }
      }

      std::fprintf(
          stderr,
          "REX_EMBEDDED_SCENE_DEPTH_PAIR ordinal=%u kind=%s "
          "vs=0x%016llX ps=0x%016llX indices=%u "
          "constants_hash=0x%016llX position_hashes=0x%016llX,0x%016llX,"
          "0x%016llX,0x%016llX referenced_bones_hash=0x%016llX "
          "referenced_bones=%u invalid_bones=%u bone_offsets=%016llX,%016llX,"
          "%016llX,%016llX vertex_base=0x%08X vertex_words=%u "
          "vertex_bytes=%u vertex_hash=0x%016llX index_base=0x%08X "
          "index_bytes=%u index_hash=0x%016llX program=0x%08X vte=0x%08X "
          "clip=0x%08X raster=0x%08X vtx_cntl=0x%08X window_offset=0x%08X "
          "viewport=%08X,%08X,%08X,%08X,%08X,%08X scale=%ux%u\n",
          trace_ordinal, pair_kind,
          static_cast<unsigned long long>(vertex_shader_hash),
          static_cast<unsigned long long>(pixel_shader_hash), index_count,
          static_cast<unsigned long long>(vertex_constants_hash),
          static_cast<unsigned long long>(position_view_hash),
          static_cast<unsigned long long>(position_adjust_hash),
          static_cast<unsigned long long>(position_unpack_hash),
          static_cast<unsigned long long>(position_bones_hash),
          static_cast<unsigned long long>(position_referenced_bones_hash),
          position_referenced_bone_count, position_invalid_bone_count,
          static_cast<unsigned long long>(position_bone_offset_bitmap[0]),
          static_cast<unsigned long long>(position_bone_offset_bitmap[1]),
          static_cast<unsigned long long>(position_bone_offset_bitmap[2]),
          static_cast<unsigned long long>(position_bone_offset_bitmap[3]), vertex_base,
          vertex_fetch.size, vertex_bytes,
          static_cast<unsigned long long>(vertex_hash), index_base, index_bytes,
          static_cast<unsigned long long>(index_hash),
          regs[XE_GPU_REG_SQ_PROGRAM_CNTL], regs[XE_GPU_REG_PA_CL_VTE_CNTL],
          regs[XE_GPU_REG_PA_CL_CLIP_CNTL], regs[XE_GPU_REG_PA_SU_SC_MODE_CNTL],
          regs[XE_GPU_REG_PA_SU_VTX_CNTL], regs[XE_GPU_REG_PA_SC_WINDOW_OFFSET],
          regs[XE_GPU_REG_PA_CL_VPORT_XSCALE],
          regs[XE_GPU_REG_PA_CL_VPORT_XOFFSET],
          regs[XE_GPU_REG_PA_CL_VPORT_YSCALE],
          regs[XE_GPU_REG_PA_CL_VPORT_YOFFSET],
          regs[XE_GPU_REG_PA_CL_VPORT_ZSCALE],
          regs[XE_GPU_REG_PA_CL_VPORT_ZOFFSET],
          render_target_cache_->GetDrawScaleX(),
          render_target_cache_->GetDrawScaleY());
      std::fflush(stderr);
    }
  }

  // Bind the pipeline after configuring it and doing everything that may bind
  // other pipelines.
  if (current_guest_pipeline_ != pipeline_handle) {
    deferred_command_list_.SetPipelineStateHandle(reinterpret_cast<void*>(pipeline_handle));
    current_guest_pipeline_ = pipeline_handle;
    current_external_pipeline_ = nullptr;
  }

  // Get dynamic rasterizer state.
  uint32_t draw_resolution_scale_x = render_target_cache_->GetDrawScaleX();
  uint32_t draw_resolution_scale_y = render_target_cache_->GetDrawScaleY();

  bool convert_z_to_float24 =
      host_render_targets_used &&
      render_target_cache_->current_draw_depth_float24_convert_in_pixel_shader();
  bool ps_writes_depth = pixel_shader && pixel_shader->writes_depth();

  // Build a cache key from all viewport-affecting state to skip redundant
  // recalculation when the viewport registers haven't changed between draws.
  ViewportCacheKey viewport_key;
  viewport_key.pa_cl_clip_cntl = regs[XE_GPU_REG_PA_CL_CLIP_CNTL];
  viewport_key.pa_cl_vte_cntl = regs[XE_GPU_REG_PA_CL_VTE_CNTL];
  viewport_key.pa_su_sc_mode_cntl = regs[XE_GPU_REG_PA_SU_SC_MODE_CNTL];
  viewport_key.pa_su_vtx_cntl = regs[XE_GPU_REG_PA_SU_VTX_CNTL];
  viewport_key.pa_sc_window_offset = regs[XE_GPU_REG_PA_SC_WINDOW_OFFSET];
  viewport_key.normalized_depth_control = normalized_depth_control.value;
  std::memcpy(viewport_key.vport_regs, &regs[XE_GPU_REG_PA_CL_VPORT_XSCALE],
              sizeof(viewport_key.vport_regs));
  viewport_key.flags = (uint32_t(convert_z_to_float24) << 0) |
                       (uint32_t(host_render_targets_used) << 1) |
                       (uint32_t(ps_writes_depth) << 2) |
                       (draw_resolution_scale_x << 3) |
                       (draw_resolution_scale_y << 6);

  draw_util::ViewportInfo viewport_info;
  if (viewport_cache_valid_ && viewport_key == previous_viewport_key_) {
    viewport_info = previous_viewport_info_;
  } else {
    draw_util::GetHostViewportInfo(regs, draw_resolution_scale_x, draw_resolution_scale_y, true,
                                   D3D12_VIEWPORT_BOUNDS_MAX, D3D12_VIEWPORT_BOUNDS_MAX, false,
                                   normalized_depth_control, convert_z_to_float24,
                                   host_render_targets_used, ps_writes_depth, viewport_info);
    previous_viewport_key_ = viewport_key;
    previous_viewport_info_ = viewport_info;
    viewport_cache_valid_ = true;
  }

  draw_util::Scissor scissor;
  draw_util::GetScissor(regs, scissor);
  scissor.offset[0] *= draw_resolution_scale_x;
  scissor.offset[1] *= draw_resolution_scale_y;
  scissor.extent[0] *= draw_resolution_scale_x;
  scissor.extent[1] *= draw_resolution_scale_y;

  xenos::xe_gpu_texture_fetch_t embedded_prompt_fetch = {};
  bool embedded_prompt_background_draw = false;
  if (kEmbeddedGraphicsDiagnosticsEnabled && !kernel_state_ && pixel_shader &&
      pixel_shader->ucode_data_hash() == UINT64_C(0x207D40E674A7C916) && index_count == 6) {
    embedded_prompt_fetch = regs.GetTextureFetch(0);
    embedded_prompt_background_draw =
        embedded_prompt_fetch.format == xenos::TextureFormat::k_DXT1 &&
        embedded_prompt_fetch.tiled && embedded_prompt_fetch.size_2d.width + 1 == 512 &&
        embedded_prompt_fetch.size_2d.height + 1 == 191 &&
        regs.Get<reg::RB_SURFACE_INFO>().value == 0x14000500 &&
        regs.Get<reg::RB_COLOR_INFO>().value == 0x000302D0 && scissor.offset[0] == 0 &&
        scissor.offset[1] == 0 && scissor.extent[0] == 1280 && scissor.extent[1] == 720;
  }

  if (embedded_prompt_background_draw) {
    static bool prompt_output_merger_logged = false;
    if (!prompt_output_merger_logged) {
      prompt_output_merger_logged = true;
      std::fprintf(
          stderr,
          "REX_EMBEDDED_PROMPT_OUTPUT_STATE colorcontrol=0x%08X blendcontrol0=0x%08X "
          "depthcontrol=0x%08X normalized_depthcontrol=0x%08X color_mask=0x%08X "
          "normalized_color_mask=0x%08X alpha_ref=0x%08X blend_factor=%08X,%08X,%08X,%08X "
          "modecontrol=0x%08X viewport=%u,%u+%ux%u scissor=%u,%u+%ux%u\n",
          regs[XE_GPU_REG_RB_COLORCONTROL], regs[XE_GPU_REG_RB_BLENDCONTROL0],
          regs[XE_GPU_REG_RB_DEPTHCONTROL], normalized_depth_control.value,
          regs[XE_GPU_REG_RB_COLOR_MASK], normalized_color_mask,
          regs[XE_GPU_REG_RB_ALPHA_REF], regs[XE_GPU_REG_RB_BLEND_RED],
          regs[XE_GPU_REG_RB_BLEND_GREEN], regs[XE_GPU_REG_RB_BLEND_BLUE],
          regs[XE_GPU_REG_RB_BLEND_ALPHA], regs[XE_GPU_REG_RB_MODECONTROL],
          viewport_info.xy_offset[0], viewport_info.xy_offset[1], viewport_info.xy_extent[0],
          viewport_info.xy_extent[1], scissor.offset[0], scissor.offset[1], scissor.extent[0],
          scissor.extent[1]);
      std::fflush(stderr);
    }
  }

  // The embedded title host does not initialize ReXGlue's normal logging
  // frontend. Keep this diagnostic bounded and keyed by the guest draw state
  // so a black output can be localized without dumping every frame. This is
  // deliberately observational: none of the captured values affect command
  // execution.
  if (kEmbeddedGraphicsDiagnosticsEnabled && !kernel_state_) {
    const auto point_size = regs.Get<reg::PA_SU_POINT_SIZE>();
    const auto point_minmax = regs.Get<reg::PA_SU_POINT_MINMAX>();
    const auto surface_info = regs.Get<reg::RB_SURFACE_INFO>();
    const auto color_info = regs.Get<reg::RB_COLOR_INFO>();
    const auto color_mask = regs.Get<reg::RB_COLOR_MASK>();
    const auto program_cntl = regs.Get<reg::SQ_PROGRAM_CNTL>();
    const auto context_misc = regs.Get<reg::SQ_CONTEXT_MISC>();
    const uint64_t vertex_shader_hash = vertex_shader->ucode_data_hash();
    const uint64_t pixel_shader_hash = pixel_shader ? pixel_shader->ucode_data_hash() : 0;
    const uint32_t pixel_color_targets = pixel_shader ? pixel_shader->writes_color_targets() : 0;
    const uint32_t pixel_texture_bindings =
        pixel_shader ? uint32_t(pixel_shader->texture_bindings().size()) : 0;

    uint64_t signature = UINT64_C(1469598103934665603);
    const auto mix_signature = [&signature](uint64_t value) {
      signature ^= value;
      signature *= UINT64_C(1099511628211);
    };
    mix_signature(uint32_t(primitive_type));
    mix_signature(index_count);
    mix_signature(uint32_t(primitive_processing_result.index_buffer_type));
    mix_signature(uint32_t(edram_mode));
    mix_signature(vertex_shader_hash);
    mix_signature(pixel_shader_hash);
    mix_signature(point_size.value);
    mix_signature(point_minmax.value);
    mix_signature(surface_info.value);
    mix_signature(color_info.value);
    mix_signature(color_mask.value);
    mix_signature(normalized_color_mask);
    mix_signature(program_cntl.value);
    mix_signature(context_misc.value);
    mix_signature(used_texture_mask);
    mix_signature(uint64_t(viewport_info.xy_offset[0]) << 32 | viewport_info.xy_offset[1]);
    mix_signature(uint64_t(viewport_info.xy_extent[0]) << 32 | viewport_info.xy_extent[1]);
    mix_signature(uint64_t(scissor.offset[0]) << 32 | scissor.offset[1]);
    mix_signature(uint64_t(scissor.extent[0]) << 32 | scissor.extent[1]);

    static uint64_t observed_signatures[128]{};
    static uint32_t observed_signature_count = 0;
    bool signature_seen = false;
    for (uint32_t i = 0; i < observed_signature_count; ++i) {
      if (observed_signatures[i] == signature) {
        signature_seen = true;
        break;
      }
    }
    if (!signature_seen && observed_signature_count < rex::countof(observed_signatures)) {
      const uint32_t state_ordinal = ++observed_signature_count;
      observed_signatures[state_ordinal - 1] = signature;
      uint32_t index_cpu_sample_bytes = 0;
      uint32_t index_cpu_sample_hash = 0;
      if (primitive_processing_result.index_buffer_type ==
              PrimitiveProcessor::ProcessedIndexBufferType::kGuestDMA ||
          primitive_processing_result.index_buffer_type ==
              PrimitiveProcessor::ProcessedIndexBufferType::kHostBuiltinForDMA) {
        const uint32_t index_size =
            primitive_processing_result.host_index_format == xenos::IndexFormat::kInt16 ? 2u : 4u;
        const uint32_t guest_index_base = primitive_processing_result.guest_index_base;
        if (guest_index_base < SharedMemory::kBufferSize) {
          index_cpu_sample_bytes = std::min(
              std::min(primitive_processing_result.guest_draw_vertex_count * index_size,
                       uint32_t(64 * 1024)),
              SharedMemory::kBufferSize - guest_index_base);
          const uint8_t* index_cpu_sample =
              memory_->TranslatePhysical<const uint8_t*>(guest_index_base);
          index_cpu_sample_hash = 2166136261u;
          for (uint32_t byte_index = 0; byte_index < index_cpu_sample_bytes; ++byte_index) {
            index_cpu_sample_hash =
                (index_cpu_sample_hash ^ index_cpu_sample[byte_index]) * 16777619u;
          }
        }
      }
      std::fprintf(
          stderr,
          "REX_EMBEDDED_DRAW_STATE ordinal=%u signature=0x%016llX primitive=%u indices=%u "
          "host_vertices=%u index_type=%u guest_index_base=0x%08X edram_mode=%u raster=%u "
          "vs=0x%016llX ps=0x%016llX "
          "ps_color_targets=0x%X ps_kills=%u ps_textures=%u used_textures=0x%08X "
          "point_size=0x%08X point_minmax=0x%08X surface=0x%08X color0=0x%08X "
          "color_mask=0x%08X normalized_color_mask=0x%08X program=0x%08X context=0x%08X "
          "viewport=%u,%u+%ux%u scissor=%u,%u+%ux%u "
          "index_cpu_sample_bytes=%u index_cpu_sample_hash=0x%08X\n",
          state_ordinal, static_cast<unsigned long long>(signature), uint32_t(primitive_type),
          index_count, primitive_processing_result.host_draw_vertex_count,
          uint32_t(primitive_processing_result.index_buffer_type),
          primitive_processing_result.guest_index_base, uint32_t(edram_mode),
          is_rasterization_done ? 1u : 0u,
          static_cast<unsigned long long>(vertex_shader_hash),
          static_cast<unsigned long long>(pixel_shader_hash), pixel_color_targets,
          pixel_shader && pixel_shader->kills_pixels() ? 1u : 0u, pixel_texture_bindings,
          used_texture_mask, point_size.value, point_minmax.value, surface_info.value,
          color_info.value, color_mask.value, normalized_color_mask, program_cntl.value,
          context_misc.value, viewport_info.xy_offset[0], viewport_info.xy_offset[1],
          viewport_info.xy_extent[0], viewport_info.xy_extent[1], scissor.offset[0],
          scissor.offset[1], scissor.extent[0], scissor.extent[1], index_cpu_sample_bytes,
          index_cpu_sample_hash);
      uint32_t logged_texture_count = 0;
      for (uint32_t texture_index = 0;
           texture_index < 32 && logged_texture_count < 8; ++texture_index) {
        if (!(used_texture_mask & (uint32_t(1) << texture_index))) {
          continue;
        }
        const xenos::xe_gpu_texture_fetch_t fetch = regs.GetTextureFetch(texture_index);
        const uint32_t texture_base = fetch.base_address << 12;
        const uint32_t texture_sample_bytes =
            std::min(uint32_t(64 * 1024), uint32_t(0x20000000u - texture_base));
        const uint8_t* texture_sample =
            memory_->TranslatePhysical<const uint8_t*>(texture_base);
        uint32_t texture_sample_hash = 2166136261u;
        if (texture_sample) {
          for (uint32_t byte_index = 0; byte_index < texture_sample_bytes; ++byte_index) {
            texture_sample_hash =
                (texture_sample_hash ^ texture_sample[byte_index]) * 16777619u;
          }
        } else {
          texture_sample_hash = 0;
        }
        std::fprintf(
            stderr,
            "REX_EMBEDDED_DRAW_TEXTURE state=%u slot=%u words=%08X,%08X,%08X,%08X,%08X,%08X "
            "type=%u base=0x%08X format=%u tiled=%u size=%ux%u dimension=%u "
            "cpu_sample_bytes=%u cpu_sample_hash=0x%08X\n",
            state_ordinal, texture_index, fetch.dword_0, fetch.dword_1, fetch.dword_2,
            fetch.dword_3, fetch.dword_4, fetch.dword_5, uint32_t(fetch.type),
            texture_base, uint32_t(fetch.format), fetch.tiled, fetch.size_2d.width + 1,
            fetch.size_2d.height + 1, uint32_t(fetch.dimension), texture_sample_bytes,
            texture_sample_hash);
        // The Darkness prompt background is the first verified visually
        // incorrect textured draw. Preserve its guest DXT1 source once so the
        // next probe can distinguish bad producer data from host
        // untile/decompression/sampling state without dumping unrelated game
        // textures.
        if (signature == UINT64_C(0x7E52945AE323DB14) && texture_index == 0 &&
            fetch.format == xenos::TextureFormat::k_DXT1 &&
            fetch.size_2d.width + 1 == 512 && fetch.size_2d.height + 1 == 191) {
          static bool prompt_background_source_captured = false;
          if (!prompt_background_source_captured && texture_sample && texture_sample_bytes) {
            prompt_background_source_captured = true;
            char capture_name[128];
            std::snprintf(capture_name, sizeof(capture_name),
                          "rex_prompt_background_dxt1_tiled_%08X_%08X.bin",
                          texture_base, texture_sample_hash);
            FILE* capture = std::fopen(capture_name, "wb");
            const size_t captured_bytes =
                capture ? std::fwrite(texture_sample, 1, texture_sample_bytes, capture) : 0;
            if (capture) std::fclose(capture);
            std::fprintf(stderr,
                         "REX_EMBEDDED_PROMPT_BACKGROUND_SOURCE path=%s base=0x%08X "
                         "bytes=%u written=%llu hash=0x%08X\n",
                         capture_name, texture_base, texture_sample_bytes,
                         static_cast<unsigned long long>(captured_bytes),
                         texture_sample_hash);
          }
        }
        ++logged_texture_count;
      }
      {
        const Shader::ConstantRegisterMap& constant_map =
            vertex_shader->constant_register_map();
        std::fprintf(
            stderr,
            "REX_EMBEDDED_VS_CONSTANT_MAP state=%u hash=0x%016llX float_count=%u "
            "float_dynamic=%u float_bitmap=%016llX,%016llX,%016llX,%016llX "
            "loop_bitmap=%08X bool_bitmap=%08X,%08X,%08X,%08X,%08X,%08X,%08X,%08X\n",
            state_ordinal, static_cast<unsigned long long>(vertex_shader_hash),
            constant_map.float_count, constant_map.float_dynamic_addressing ? 1u : 0u,
            static_cast<unsigned long long>(constant_map.float_bitmap[0]),
            static_cast<unsigned long long>(constant_map.float_bitmap[1]),
            static_cast<unsigned long long>(constant_map.float_bitmap[2]),
            static_cast<unsigned long long>(constant_map.float_bitmap[3]),
            constant_map.loop_bitmap, constant_map.bool_bitmap[0], constant_map.bool_bitmap[1],
            constant_map.bool_bitmap[2], constant_map.bool_bitmap[3], constant_map.bool_bitmap[4],
            constant_map.bool_bitmap[5], constant_map.bool_bitmap[6], constant_map.bool_bitmap[7]);

        uint32_t logged_float_constant_count = 0;
        for (uint32_t bitmap_index = 0;
             bitmap_index < 4 && logged_float_constant_count < 64; ++bitmap_index) {
          uint64_t bitmap = constant_map.float_bitmap[bitmap_index];
          uint32_t bit_index;
          while (logged_float_constant_count < 64 &&
                 rex::bit_scan_forward(bitmap, &bit_index)) {
            bitmap &= ~(uint64_t(1) << bit_index);
            const uint32_t constant_index = (bitmap_index << 6) + bit_index;
            const uint32_t register_index =
                XE_GPU_REG_SHADER_CONSTANT_000_X + (constant_index << 2);
            float values[4];
            std::memcpy(values, &regs[register_index], sizeof(values));
            std::fprintf(
                stderr,
                "REX_EMBEDDED_VS_FLOAT state=%u c%u raw=%08X,%08X,%08X,%08X "
                "value=%.9g,%.9g,%.9g,%.9g\n",
                state_ordinal, constant_index, regs[register_index], regs[register_index + 1],
                regs[register_index + 2], regs[register_index + 3], values[0], values[1],
                values[2], values[3]);
            ++logged_float_constant_count;
          }
        }
        if (logged_float_constant_count < constant_map.float_count) {
          std::fprintf(stderr,
                       "REX_EMBEDDED_VS_FLOAT_TRUNCATED state=%u logged=%u total=%u\n",
                       state_ordinal, logged_float_constant_count, constant_map.float_count);
        }
        for (uint32_t loop_index = 0; loop_index < 32; ++loop_index) {
          if (constant_map.loop_bitmap & (uint32_t(1) << loop_index)) {
            std::fprintf(stderr, "REX_EMBEDDED_VS_LOOP state=%u l%u raw=%08X\n",
                         state_ordinal, loop_index,
                         regs[XE_GPU_REG_SHADER_CONSTANT_LOOP_00 + loop_index]);
          }
        }
        for (uint32_t bool_index = 0; bool_index < 256; ++bool_index) {
          if (constant_map.bool_bitmap[bool_index >> 5] &
              (uint32_t(1) << (bool_index & 31))) {
            const uint32_t packed =
                regs[XE_GPU_REG_SHADER_CONSTANT_BOOL_000_031 + (bool_index >> 5)];
            std::fprintf(stderr, "REX_EMBEDDED_VS_BOOL state=%u b%u value=%u\n",
                         state_ordinal, bool_index, (packed >> (bool_index & 31)) & 1u);
          }
        }

        const std::string& disassembly = vertex_shader->ucode_disassembly();
        constexpr size_t kMaximumDisassemblyBytes = 16384;
        const size_t disassembly_bytes =
            std::min(disassembly.size(), kMaximumDisassemblyBytes);
        std::fprintf(stderr,
                     "REX_EMBEDDED_VS_DISASM_BEGIN state=%u hash=0x%016llX bytes=%llu "
                     "total=%llu\n",
                     state_ordinal, static_cast<unsigned long long>(vertex_shader_hash),
                     static_cast<unsigned long long>(disassembly_bytes),
                     static_cast<unsigned long long>(disassembly.size()));
        if (disassembly_bytes) {
          std::fwrite(disassembly.data(), 1, disassembly_bytes, stderr);
          if (disassembly[disassembly_bytes - 1] != '\n') {
            std::fputc('\n', stderr);
          }
        }
        std::fprintf(stderr,
                     "REX_EMBEDDED_VS_DISASM_END state=%u truncated=%u\n",
                     state_ordinal,
                     disassembly_bytes < disassembly.size() ? 1u : 0u);

        const auto index_offset = regs.Get<reg::VGT_INDX_OFFSET>();
        const auto minimum_index = regs.Get<reg::VGT_MIN_VTX_INDX>();
        const auto maximum_index = regs.Get<reg::VGT_MAX_VTX_INDX>();
        std::fprintf(stderr,
                     "REX_EMBEDDED_VS_INDEX_RANGE state=%u offset=%u min=%u max=%u\n",
                     state_ordinal, index_offset.indx_offset, minimum_index.min_indx,
                     maximum_index.max_indx);
        for (const Shader::VertexBinding& binding : vertex_shader->vertex_bindings()) {
          const xenos::xe_gpu_vertex_fetch_t fetch =
              regs.GetVertexFetch(binding.fetch_constant);
          std::fprintf(stderr,
                       "REX_EMBEDDED_VFETCH state=%u slot=%u binding=%d stride_words=%u "
                       "attributes=%llu dwords=%08X,%08X type=%u address=0x%08X "
                       "size_words=%u endian=%u\n",
                       state_ordinal, binding.fetch_constant, binding.binding_index,
                       binding.stride_words,
                       static_cast<unsigned long long>(binding.attributes.size()),
                       fetch.dword_0, fetch.dword_1, uint32_t(fetch.type),
                       fetch.address << 2, fetch.size, uint32_t(fetch.endian));
          if (!fetch.size || !binding.stride_words) {
            continue;
          }
          const uint32_t words_to_log =
              std::min(fetch.size, std::min(binding.stride_words * 3, uint32_t(64)));
          const uint8_t* fetch_data =
              memory_->TranslatePhysical<const uint8_t*>(fetch.address << 2);
          for (uint32_t word_index = 0; word_index < words_to_log; ++word_index) {
            const uint32_t raw_value =
                rex::memory::load<uint32_t>(fetch_data + word_index * sizeof(uint32_t));
            const uint32_t swapped_value = xenos::GpuSwap(raw_value, fetch.endian);
            float float_value;
            std::memcpy(&float_value, &swapped_value, sizeof(float_value));
            std::fprintf(stderr,
                         "REX_EMBEDDED_VFETCH_WORD state=%u slot=%u vertex=%u word=%u "
                         "raw=%08X swapped=%08X float=%.9g\n",
                         state_ordinal, binding.fetch_constant,
                         word_index / binding.stride_words,
                         word_index % binding.stride_words, raw_value, swapped_value,
                         float_value);
          }
        }
      }
      if (pixel_shader) {
        const Shader::ConstantRegisterMap& constant_map =
            pixel_shader->constant_register_map();
        std::fprintf(
            stderr,
            "REX_EMBEDDED_PS_CONSTANT_MAP state=%u hash=0x%016llX float_count=%u "
            "float_dynamic=%u float_bitmap=%016llX,%016llX,%016llX,%016llX "
            "loop_bitmap=%08X bool_bitmap=%08X,%08X,%08X,%08X,%08X,%08X,%08X,%08X\n",
            state_ordinal, static_cast<unsigned long long>(pixel_shader_hash),
            constant_map.float_count, constant_map.float_dynamic_addressing ? 1u : 0u,
            static_cast<unsigned long long>(constant_map.float_bitmap[0]),
            static_cast<unsigned long long>(constant_map.float_bitmap[1]),
            static_cast<unsigned long long>(constant_map.float_bitmap[2]),
            static_cast<unsigned long long>(constant_map.float_bitmap[3]),
            constant_map.loop_bitmap, constant_map.bool_bitmap[0], constant_map.bool_bitmap[1],
            constant_map.bool_bitmap[2], constant_map.bool_bitmap[3], constant_map.bool_bitmap[4],
            constant_map.bool_bitmap[5], constant_map.bool_bitmap[6], constant_map.bool_bitmap[7]);

        uint32_t logged_float_constant_count = 0;
        for (uint32_t bitmap_index = 0;
             bitmap_index < 4 && logged_float_constant_count < 64; ++bitmap_index) {
          uint64_t bitmap = constant_map.float_bitmap[bitmap_index];
          uint32_t bit_index;
          while (logged_float_constant_count < 64 &&
                 rex::bit_scan_forward(bitmap, &bit_index)) {
            bitmap &= ~(uint64_t(1) << bit_index);
            const uint32_t constant_index = (bitmap_index << 6) + bit_index;
            const uint32_t register_index =
                XE_GPU_REG_SHADER_CONSTANT_256_X + (constant_index << 2);
            float values[4];
            std::memcpy(values, &regs[register_index], sizeof(values));
            std::fprintf(
                stderr,
                "REX_EMBEDDED_PS_FLOAT state=%u c%u raw=%08X,%08X,%08X,%08X "
                "value=%.9g,%.9g,%.9g,%.9g\n",
                state_ordinal, constant_index + 256, regs[register_index],
                regs[register_index + 1], regs[register_index + 2],
                regs[register_index + 3], values[0], values[1], values[2], values[3]);
            ++logged_float_constant_count;
          }
        }
        if (logged_float_constant_count < constant_map.float_count) {
          std::fprintf(stderr,
                       "REX_EMBEDDED_PS_FLOAT_TRUNCATED state=%u logged=%u total=%u\n",
                       state_ordinal, logged_float_constant_count, constant_map.float_count);
        }

        for (uint32_t loop_index = 0; loop_index < 32; ++loop_index) {
          if (constant_map.loop_bitmap & (uint32_t(1) << loop_index)) {
            std::fprintf(stderr, "REX_EMBEDDED_PS_LOOP state=%u l%u raw=%08X\n",
                         state_ordinal, loop_index,
                         regs[XE_GPU_REG_SHADER_CONSTANT_LOOP_00 + loop_index]);
          }
        }
        for (uint32_t bool_index = 0; bool_index < 256; ++bool_index) {
          if (constant_map.bool_bitmap[bool_index >> 5] &
              (uint32_t(1) << (bool_index & 31))) {
            const uint32_t packed =
                regs[XE_GPU_REG_SHADER_CONSTANT_BOOL_000_031 + (bool_index >> 5)];
            std::fprintf(stderr, "REX_EMBEDDED_PS_BOOL state=%u b%u value=%u\n",
                         state_ordinal, bool_index, (packed >> (bool_index & 31)) & 1u);
          }
        }

        const std::string& disassembly = pixel_shader->ucode_disassembly();
        constexpr size_t kMaximumDisassemblyBytes = 16384;
        const size_t disassembly_bytes =
            std::min(disassembly.size(), kMaximumDisassemblyBytes);
        std::fprintf(stderr,
                     "REX_EMBEDDED_PS_DISASM_BEGIN state=%u hash=0x%016llX bytes=%llu "
                     "total=%llu\n",
                     state_ordinal, static_cast<unsigned long long>(pixel_shader_hash),
                     static_cast<unsigned long long>(disassembly_bytes),
                     static_cast<unsigned long long>(disassembly.size()));
        if (disassembly_bytes) {
          std::fwrite(disassembly.data(), 1, disassembly_bytes, stderr);
          if (disassembly[disassembly_bytes - 1] != '\n') {
            std::fputc('\n', stderr);
          }
        }
        std::fprintf(stderr,
                     "REX_EMBEDDED_PS_DISASM_END state=%u truncated=%u\n",
                     state_ordinal,
                     disassembly_bytes < disassembly.size() ? 1u : 0u);
      }
      std::fflush(stderr);
    }
  }

  // Update viewport, scissor, blend factor and stencil reference.
  UpdateFixedFunctionState(viewport_info, scissor, primitive_polygonal, normalized_depth_control);

  // Update system constants before uploading them.
  // TODO(Triang3l): With ROV, pass the disabled render target mask for safety.
  UpdateSystemConstantValues(memexport_used, primitive_polygonal,
                             primitive_processing_result.line_loop_closing_index,
                             primitive_processing_result.host_shader_index_endian, viewport_info,
                             used_texture_mask, normalized_depth_control, normalized_color_mask);

  // The title attraction's simple video/effect frame uses one final two-input
  // composition draw. Probes 253/254 provide an identical guest state at swap
  // 2816 while internal 2x loses full-frame color intensity. Snapshot both
  // shader-visible inputs only once, after residency is established, so the
  // first differing representation can be placed before or after this draw.
  if (capture_embedded_simple_title_composition_draw) {
    static bool embedded_simple_title_inputs_captured = false;
    if (!embedded_simple_title_inputs_captured) {
      embedded_simple_title_inputs_captured = true;
      constexpr uint32_t kSimpleTitleTextureSlots[] = {0, 1};
      std::fprintf(
          stderr,
          "REX_EMBEDDED_SIMPLE_TITLE_INPUTS result=queued "
          "vs=0x%016llX ps=0x%016llX used=0x%08X scaled_mask=0x%08X "
          "scale=%ux%u ps_modification=0x%016llX\n",
          static_cast<unsigned long long>(vertex_shader->ucode_data_hash()),
          static_cast<unsigned long long>(pixel_shader->ucode_data_hash()),
          used_texture_mask, system_constants_.textures_resolution_scaled,
          draw_resolution_scale_x, draw_resolution_scale_y,
          static_cast<unsigned long long>(
              pixel_shader_translation->modification()));
      for (uint32_t texture_slot : kSimpleTitleTextureSlots) {
        const xenos::xe_gpu_texture_fetch_t fetch =
            regs.GetTextureFetch(texture_slot);
        D3D12TextureCache::ActiveTextureDiagnostic diagnostic;
        const bool valid = texture_cache_->GetActiveTextureDiagnostic(
            texture_slot, diagnostic);
        std::fprintf(
            stderr,
            "REX_EMBEDDED_SIMPLE_TITLE_TEXTURE slot=%u valid=%u "
            "fetch=%08X,%08X,%08X,%08X,%08X,%08X "
            "guest=0x%08X+0x%X guest_extent=%ux%ux%u guest_format=%u "
            "scaled=%u outdated=0x%X host_extent=%llux%ux%u "
            "host_format=%u descriptor=%u signed_descriptor=%u\n",
            texture_slot, valid ? 1u : 0u, fetch.dword_0, fetch.dword_1,
            fetch.dword_2, fetch.dword_3, fetch.dword_4, fetch.dword_5,
            diagnostic.guest_base, diagnostic.guest_size,
            diagnostic.guest_width, diagnostic.guest_height,
            diagnostic.guest_depth_or_array_size, diagnostic.guest_format,
            diagnostic.scaled_resolve, diagnostic.outdated_mask,
            static_cast<unsigned long long>(diagnostic.resource_width),
            diagnostic.resource_height,
            diagnostic.resource_depth_or_array_size,
            diagnostic.resource_format, diagnostic.descriptor_index,
            diagnostic.descriptor_index_signed);
      }
      texture_cache_->CaptureActiveTextureReadbackDiagnostics(
          kSimpleTitleTextureSlots,
          sizeof(kSimpleTitleTextureSlots) /
              sizeof(kSimpleTitleTextureSlots[0]));
      std::fflush(stderr);
    }
  }

  // The Darkness final front-end composition shader. Record one exact binding
  // snapshot per process only when a bounded gameplay capture is enabled. The
  // four inputs include scene color and the auxiliary exposure/RGB-map/effect
  // layers, so this localizes resolution-scale corruption without per-draw
  // logging or changing resource state.
  if (!kernel_state_ && IsCurrentEmbeddedGameplayCaptureFrame() && pixel_shader &&
      pixel_shader->ucode_data_hash() == UINT64_C(0xA59B41D0BD79484B)) {
    static bool final_composition_bindings_logged = false;
    if (!final_composition_bindings_logged) {
      final_composition_bindings_logged = true;
      std::fprintf(
          stderr,
          "REX_EMBEDDED_FINAL_COMPOSITION_BINDINGS used=0x%08X "
          "scaled_mask=0x%08X scale=%ux%u ps_modification=0x%016llX\n",
          used_texture_mask, system_constants_.textures_resolution_scaled,
          draw_resolution_scale_x, draw_resolution_scale_y,
          static_cast<unsigned long long>(pixel_shader_translation->modification()));
      constexpr uint32_t kFinalCompositionTextureSlots[] = {0, 1, 2, 4};
      for (uint32_t texture_slot : kFinalCompositionTextureSlots) {
        const xenos::xe_gpu_texture_fetch_t fetch =
            regs.GetTextureFetch(texture_slot);
        D3D12TextureCache::ActiveTextureDiagnostic diagnostic;
        const bool valid = texture_cache_->GetActiveTextureDiagnostic(
            texture_slot, diagnostic);
        std::fprintf(
            stderr,
            "REX_EMBEDDED_FINAL_COMPOSITION_TEXTURE slot=%u valid=%u "
            "fetch=%08X,%08X,%08X,%08X,%08X,%08X guest=0x%08X+0x%X "
            "guest_extent=%ux%ux%u guest_format=%u dimension=%u tiled=%u "
            "scaled=%u outdated=0x%X resource=0x%016llX "
            "host_extent=%llux%ux%u mips=%u host_format=%u "
            "descriptor=%u signed_descriptor=%u\n",
            texture_slot, valid ? 1u : 0u, fetch.dword_0, fetch.dword_1,
            fetch.dword_2, fetch.dword_3, fetch.dword_4, fetch.dword_5,
            diagnostic.guest_base, diagnostic.guest_size,
            diagnostic.guest_width, diagnostic.guest_height,
            diagnostic.guest_depth_or_array_size, diagnostic.guest_format,
            diagnostic.guest_dimension, diagnostic.guest_tiled,
            diagnostic.scaled_resolve, diagnostic.outdated_mask,
            static_cast<unsigned long long>(diagnostic.resource_identity),
            static_cast<unsigned long long>(diagnostic.resource_width),
            diagnostic.resource_height,
            diagnostic.resource_depth_or_array_size,
            diagnostic.resource_mip_levels, diagnostic.resource_format,
            diagnostic.descriptor_index,
            diagnostic.descriptor_index_signed);
      }
      texture_cache_->CaptureActiveTextureReadbackDiagnostics(
          kFinalCompositionTextureSlots,
          sizeof(kFinalCompositionTextureSlots) /
              sizeof(kFinalCompositionTextureSlots[0]));
      std::fflush(stderr);
    }
  }

  // Record the exact translated binding layout and sampler state only when the
  // verified PRESS START background is active. Both signed and unsigned SRVs
  // are intentionally emitted by the generic translator; this trace proves
  // which descriptor slots and signedness controls the host shader receives.
  if (kEmbeddedGraphicsDiagnosticsEnabled && !kernel_state_ && pixel_shader &&
      pixel_shader->ucode_data_hash() == UINT64_C(0x207D40E674A7C916)) {
    const xenos::xe_gpu_texture_fetch_t prompt_fetch = regs.GetTextureFetch(0);
    static bool prompt_host_binding_logged = false;
    if (!prompt_host_binding_logged &&
        prompt_fetch.format == xenos::TextureFormat::k_DXT1 && prompt_fetch.tiled &&
        prompt_fetch.size_2d.width + 1 == 512 && prompt_fetch.size_2d.height + 1 == 191) {
      prompt_host_binding_logged = true;
      const auto& vertex_textures = vertex_shader->GetTextureBindingsAfterTranslation();
      const auto& pixel_textures = pixel_shader->GetTextureBindingsAfterTranslation();
      const auto& vertex_samplers = vertex_shader->GetSamplerBindingsAfterTranslation();
      const auto& pixel_samplers = pixel_shader->GetSamplerBindingsAfterTranslation();
      const uint8_t texture_signs = uint8_t(system_constants_.texture_swizzled_signs[0]);
      std::fprintf(
          stderr,
          "REX_EMBEDDED_PROMPT_HOST_BINDINGS bindless=%u ps_modification=0x%016llX "
          "fetch=%08X,%08X,%08X,%08X,%08X,%08X swizzled_signs=0x%02X "
          "vs_textures=%llu ps_textures=%llu vs_samplers=%llu ps_samplers=%llu\n",
          bindless_resources_used_ ? 1u : 0u,
          static_cast<unsigned long long>(pixel_shader_translation->modification()),
          prompt_fetch.dword_0, prompt_fetch.dword_1, prompt_fetch.dword_2,
          prompt_fetch.dword_3, prompt_fetch.dword_4, prompt_fetch.dword_5,
          texture_signs, static_cast<unsigned long long>(vertex_textures.size()),
          static_cast<unsigned long long>(pixel_textures.size()),
          static_cast<unsigned long long>(vertex_samplers.size()),
          static_cast<unsigned long long>(pixel_samplers.size()));
      const auto log_textures = [](const char* stage,
                                   const std::vector<D3D12Shader::TextureBinding>& bindings) {
        for (size_t i = 0; i < bindings.size(); ++i) {
          const auto& binding = bindings[i];
          std::fprintf(stderr,
                       "REX_EMBEDDED_PROMPT_HOST_TEXTURE stage=%s ordinal=%llu "
                       "cb_index=%u fetch=%u dimension=%u signed=%u\n",
                       stage, static_cast<unsigned long long>(i),
                       binding.bindless_descriptor_index, binding.fetch_constant,
                       uint32_t(binding.dimension), binding.is_signed ? 1u : 0u);
        }
      };
      log_textures("vs", vertex_textures);
      log_textures("ps", pixel_textures);
      const auto log_samplers = [this](
                                    const char* stage,
                                    const std::vector<D3D12Shader::SamplerBinding>& bindings) {
        for (size_t i = 0; i < bindings.size(); ++i) {
          const auto& binding = bindings[i];
          const D3D12TextureCache::SamplerParameters parameters =
              texture_cache_->GetSamplerParameters(binding);
          std::fprintf(
              stderr,
              "REX_EMBEDDED_PROMPT_HOST_SAMPLER stage=%s ordinal=%llu cb_index=%u "
              "fetch=%u value=0x%08X clamp=%u,%u,%u border=%u linear=%u,%u,%u "
              "aniso=%u mip_min=%u base_map=%u\n",
              stage, static_cast<unsigned long long>(i), binding.bindless_descriptor_index,
              binding.fetch_constant, parameters.value, uint32_t(parameters.clamp_x),
              uint32_t(parameters.clamp_y), uint32_t(parameters.clamp_z),
              uint32_t(parameters.border_color), parameters.mag_linear, parameters.min_linear,
              parameters.mip_linear, uint32_t(parameters.aniso_filter),
              parameters.mip_min_level, parameters.mip_base_map);
        }
      };
      log_samplers("vs", vertex_samplers);
      log_samplers("ps", pixel_samplers);
      const std::string& host_disassembly = pixel_shader_translation->host_disassembly();
      constexpr size_t kMaximumHostDisassemblyBytes = 32768;
      const size_t host_disassembly_bytes =
          std::min(host_disassembly.size(), kMaximumHostDisassemblyBytes);
      std::fprintf(stderr,
                   "REX_EMBEDDED_PROMPT_HOST_PS_BEGIN bytes=%llu total=%llu\n",
                   static_cast<unsigned long long>(host_disassembly_bytes),
                   static_cast<unsigned long long>(host_disassembly.size()));
      if (host_disassembly_bytes) {
        std::fwrite(host_disassembly.data(), 1, host_disassembly_bytes, stderr);
        if (host_disassembly[host_disassembly_bytes - 1] != '\n') {
          std::fputc('\n', stderr);
        }
      }
      std::fprintf(stderr, "REX_EMBEDDED_PROMPT_HOST_PS_END truncated=%u\n",
                   host_disassembly_bytes < host_disassembly.size() ? 1u : 0u);
      std::fflush(stderr);
    }
  }

  // Update constant buffers, descriptors and root parameters.
  if (sample_draw) sampled_draw_scope.BeginBinding();
  const bool bindings_updated =
      UpdateBindings(vertex_shader, pixel_shader, root_signature, memexport_used);
  if (sample_draw) sampled_draw_scope.FinishBinding();
  if (!bindings_updated) {
    return fail_embedded_draw("update_bindings");
  }
  if (sample_draw) sampled_draw_scope.Advance();
  // Must not call anything that can change the descriptor heap from now on!

  if (embedded_prompt_background_draw) {
    static bool prompt_vertex_cbuffer_logged = false;
    if (!prompt_vertex_cbuffer_logged) {
      prompt_vertex_cbuffer_logged = true;
      const Shader::ConstantRegisterMap& constant_map = vertex_shader->constant_register_map();
      std::fprintf(stderr,
                   "REX_EMBEDDED_PROMPT_VS_CBUFFER gpu=0x%016llX cpu=%p bytes=%u "
                   "float_count=%u\n",
                   static_cast<unsigned long long>(cbuffer_binding_float_vertex_.address),
                   static_cast<const void*>(embedded_float_vertex_cpu_address_),
                   embedded_float_vertex_cpu_size_, constant_map.float_count);
      uint32_t dense_index = 0;
      for (uint32_t bitmap_index = 0; bitmap_index < 4; ++bitmap_index) {
        uint64_t bitmap = constant_map.float_bitmap[bitmap_index];
        uint32_t bit_index;
        while (rex::bit_scan_forward(bitmap, &bit_index)) {
          bitmap &= ~(UINT64_C(1) << bit_index);
          const uint32_t constant_index = (bitmap_index << 6) + bit_index;
          uint32_t values[4] = {};
          const uint32_t byte_offset = dense_index * 4 * sizeof(uint32_t);
          if (embedded_float_vertex_cpu_address_ &&
              byte_offset + sizeof(values) <= embedded_float_vertex_cpu_size_) {
            std::memcpy(values, embedded_float_vertex_cpu_address_ + byte_offset,
                        sizeof(values));
          }
          std::fprintf(stderr,
                       "REX_EMBEDDED_PROMPT_VS_CBUFFER_CONSTANT dense=%u c%u offset=%u "
                       "raw=%08X,%08X,%08X,%08X\n",
                       dense_index, constant_index, byte_offset, values[0], values[1], values[2],
                       values[3]);
          if (constant_index < 11) {
            for (uint32_t component = 0; component < 4; ++component) {
              const EmbeddedFloatConstantWrite& write =
                  embedded_float_constant_writes_[constant_index * 4 + component];
              std::fprintf(
                  stderr,
                  "REX_EMBEDDED_PROMPT_VS_CONSTANT_WRITER c%u.%u sequence=%llu value=%08X "
                  "source=%s physical=0x%08X\n",
                  constant_index, component, static_cast<unsigned long long>(write.sequence),
                  write.value, write.bulk ? "bulk" : "inline", write.physical_address);
            }
          }
          ++dense_index;
        }
      }
      std::fflush(stderr);
    }
  }

  // Ensure vertex buffers are resident. An unchanged fetch slot skips the
  // request only while its range stays resident; any invalidation since the
  // in-sync bits were set (an in-place CPU rewrite of the buffer, a published
  // host write, a reset) re-checks every slot.
  const uint64_t vertex_residency_epoch = shared_memory_->invalidation_epoch();
  if (vertex_residency_epoch != vertex_buffers_residency_epoch_) {
    vertex_buffers_in_sync_[0] = 0;
    vertex_buffers_in_sync_[1] = 0;
    vertex_buffers_residency_epoch_ = vertex_residency_epoch;
  }
  const Shader::ConstantRegisterMap& constant_map_vertex = vertex_shader->constant_register_map();
  for (uint32_t i = 0; i < rex::countof(constant_map_vertex.vertex_fetch_bitmap); ++i) {
    uint32_t vfetch_bits_remaining = constant_map_vertex.vertex_fetch_bitmap[i];
    uint32_t j;
    while (rex::bit_scan_forward(vfetch_bits_remaining, &j)) {
      vfetch_bits_remaining &= ~(uint32_t(1) << j);
      uint32_t vfetch_index = i * 32 + j;
      uint64_t vfetch_bit = uint64_t(1) << (vfetch_index & 63);
      if (vertex_buffers_in_sync_[vfetch_index >> 6] & vfetch_bit) {
        continue;
      }
      xenos::xe_gpu_vertex_fetch_t vfetch_constant = regs.GetVertexFetch(vfetch_index);
      switch (vfetch_constant.type) {
        case xenos::FetchConstantType::kVertex:
          break;
        case xenos::FetchConstantType::kInvalidVertex:
          if (REXCVAR_GET(gpu_allow_invalid_fetch_constants)) {
            break;
          }
          REXGPU_WARN(
              "Vertex fetch constant {} ({:08X} {:08X}) has \"invalid\" type! "
              "This is incorrect behavior, but you can try bypassing this by "
              "launching Xenia with --gpu_allow_invalid_fetch_constants=true.",
              vfetch_index, vfetch_constant.dword_0, vfetch_constant.dword_1);
          return fail_embedded_draw("invalid_vertex_fetch_type");
        default:
          REXGPU_WARN("Vertex fetch constant {} ({:08X} {:08X}) is completely invalid!",
                      vfetch_index, vfetch_constant.dword_0, vfetch_constant.dword_1);
          return fail_embedded_draw("unknown_vertex_fetch_type");
      }
      VertexBufferState& state = vertex_buffer_states_[vfetch_index];
      const bool vertex_slot_unchanged =
          state.address == vfetch_constant.address && state.size == vfetch_constant.size;
      if (vertex_slot_unchanged &&
          shared_memory_->IsRangeResident(vfetch_constant.address << 2,
                                          vfetch_constant.size << 2)) {
#if REX_GPU_DIAGNOSTICS
        ++vertex_slot_skipped_;
        shared_memory_->CoherencyAuditSkippedRequest(vfetch_constant.address << 2,
                                                     vfetch_constant.size << 2);
#endif
        vertex_buffers_in_sync_[vfetch_index >> 6] |= vfetch_bit;
        continue;
      }
#if REX_GPU_DIAGNOSTICS
      ++(vertex_slot_unchanged ? vertex_slot_revalidated_ : vertex_slot_changed_);
#endif
      if (!shared_memory_->RequestRange(vfetch_constant.address << 2, vfetch_constant.size << 2)) {
        REXGPU_ERROR(
            "Failed to request vertex buffer at 0x{:08X} (size {}) in the "
            "shared memory",
            vfetch_constant.address << 2, vfetch_constant.size << 2);
        return fail_embedded_draw("vertex_buffer_residency");
      }
      state.address = vfetch_constant.address;
      state.size = vfetch_constant.size;
      vertex_buffers_in_sync_[vfetch_index >> 6] |= vfetch_bit;
    }
  }

  // Gather memexport ranges and ensure the heaps for them are resident, and
  // also load the data surrounding the export and to fill the regions that
  // won't be modified by the shaders.
  memexport_ranges_.clear();
  if (memexport_used_vertex) {
    draw_util::AddMemExportRanges(regs, *vertex_shader, memexport_ranges_);
  }
  if (memexport_used_pixel) {
    draw_util::AddMemExportRanges(regs, *pixel_shader, memexport_ranges_);
  }
  for (const draw_util::MemExportRange& memexport_range : memexport_ranges_) {
    if (!shared_memory_->RequestRange(memexport_range.base_address_dwords << 2,
                                      memexport_range.size_bytes)) {
      REXGPU_ERROR(
          "Failed to request memexport stream at 0x{:08X} (size {}) in the "
          "shared memory",
          memexport_range.base_address_dwords << 2, memexport_range.size_bytes);
      return fail_embedded_draw("memexport_residency");
    }
  }
  if (memexport_used && memexport_ranges_.empty()) {
    if (!shared_memory_->RequestRange(0, SharedMemory::kBufferSize)) {
      REXGPU_ERROR(
          "Failed to request full shared memory residency for unresolved "
          "memexport destinations");
      return fail_embedded_draw("unresolved_memexport_residency");
    }
  }

  // Primitive topology.
  D3D_PRIMITIVE_TOPOLOGY primitive_topology;
  if (primitive_processing_result.IsTessellated()) {
    switch (primitive_processing_result.host_primitive_type) {
      // TODO(Triang3l): Support all primitive types.
      case xenos::PrimitiveType::kTriangleList:
        primitive_topology = D3D_PRIMITIVE_TOPOLOGY_3_CONTROL_POINT_PATCHLIST;
        break;
      case xenos::PrimitiveType::kQuadList:
        primitive_topology = D3D_PRIMITIVE_TOPOLOGY_4_CONTROL_POINT_PATCHLIST;
        break;
      case xenos::PrimitiveType::kTrianglePatch:
        primitive_topology =
            (regs.Get<reg::VGT_HOS_CNTL>().tess_mode == xenos::TessellationMode::kAdaptive)
                ? D3D_PRIMITIVE_TOPOLOGY_3_CONTROL_POINT_PATCHLIST
                : D3D_PRIMITIVE_TOPOLOGY_1_CONTROL_POINT_PATCHLIST;
        break;
      case xenos::PrimitiveType::kQuadPatch:
        primitive_topology =
            (regs.Get<reg::VGT_HOS_CNTL>().tess_mode == xenos::TessellationMode::kAdaptive)
                ? D3D_PRIMITIVE_TOPOLOGY_4_CONTROL_POINT_PATCHLIST
                : D3D_PRIMITIVE_TOPOLOGY_1_CONTROL_POINT_PATCHLIST;
        break;
      default:
        REXGPU_ERROR(
            "Host tessellated primitive type {} returned by the primitive "
            "processor is not supported by the Direct3D 12 command processor",
            uint32_t(primitive_processing_result.host_primitive_type));
        assert_unhandled_case(primitive_processing_result.host_primitive_type);
        return fail_embedded_draw("unsupported_tessellated_primitive");
    }
  } else {
    switch (primitive_processing_result.host_primitive_type) {
      case xenos::PrimitiveType::kPointList:
        primitive_topology = D3D_PRIMITIVE_TOPOLOGY_POINTLIST;
        break;
      case xenos::PrimitiveType::kLineList:
        primitive_topology = D3D_PRIMITIVE_TOPOLOGY_LINELIST;
        break;
      case xenos::PrimitiveType::kLineStrip:
        primitive_topology = D3D_PRIMITIVE_TOPOLOGY_LINESTRIP;
        break;
      case xenos::PrimitiveType::kTriangleList:
      case xenos::PrimitiveType::kRectangleList:
        primitive_topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
        break;
      case xenos::PrimitiveType::kTriangleStrip:
        primitive_topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP;
        break;
      case xenos::PrimitiveType::kQuadList:
        primitive_topology = D3D_PRIMITIVE_TOPOLOGY_LINELIST_ADJ;
        break;
      default:
        REXGPU_ERROR(
            "Host primitive type {} returned by the primitive processor is not "
            "supported by the Direct3D 12 command processor",
            uint32_t(primitive_processing_result.host_primitive_type));
        assert_unhandled_case(primitive_processing_result.host_primitive_type);
        return fail_embedded_draw("unsupported_primitive");
    }
  }
  SetPrimitiveTopology(primitive_topology);
  // Must not call anything that may change the primitive topology from now on!

  // Probe281's first 32 scene-chain images were a normal buffer (00030300),
  // not the lit scene-color target (000C0300). Follow an explicitly selected
  // owning target within ONE frame. Ordinals select checkpoints, not meanings.
  embedded_target_writer_capture_policy::Selection target_writer_selection;
  if (kGpuDiagnostics && !kernel_state_ && embedded_target_writer_config_.count) {
    target_writer_selection = embedded_target_writer_state_.Observe(
        embedded_target_writer_config_,
        {embedded_completed_swap_ordinal + 1,
         embedded_frame_frontier.submitted_draws + 1,
         IsCurrentEmbeddedGameplayCaptureFrame(), pixel_shader != nullptr,
         regs[XE_GPU_REG_RB_SURFACE_INFO], regs[XE_GPU_REG_RB_COLOR_INFO],
         normalized_color_mask, regs[XE_GPU_REG_PA_SC_WINDOW_OFFSET],
         regs[XE_GPU_REG_PA_SC_WINDOW_SCISSOR_TL],
         regs[XE_GPU_REG_PA_SC_WINDOW_SCISSOR_BR],
         pixel_shader ? pixel_shader->ucode_data_hash() : 0});
    if (embedded_target_writer_config_.automatic_first_frame &&
        !embedded_writer_match_frame && embedded_target_writer_state_.locked_frame()) {
      embedded_writer_match_frame = embedded_target_writer_state_.locked_frame();
      embedded_writer_match_first_draw = embedded_frame_frontier.submitted_draws + 1;
      std::fprintf(stderr,
          "REX_EMBEDDED_TARGET_WRITER_ARM frame=%llu first_observed_draw=%llu "
          "scope=exact_writer_partial_frame ps=0x%016llX surface=0x%08X color=0x%08X\n",
          static_cast<unsigned long long>(embedded_writer_match_frame),
          static_cast<unsigned long long>(embedded_writer_match_first_draw),
          static_cast<unsigned long long>(pixel_shader->ucode_data_hash()),
          regs[XE_GPU_REG_RB_SURFACE_INFO], regs[XE_GPU_REG_RB_COLOR_INFO]);
      std::fflush(stderr);
    }
  }
  const auto capture_target_writer = [&](bool before) {
    char path[96];
    std::snprintf(path, sizeof(path), "rex_target_writer_%s_%u_fp16.bin",
                  before ? "before" : "after", target_writer_selection.writer);
    const auto capture_start = std::chrono::steady_clock::now();
    // Queue only. A synchronous readback here closes the submission AFTER
    // graphics binding, dropping the selected draw at the next list reset.
    const uint64_t capture_submission = GetCurrentSubmission();
    const bool captured = render_target_cache_->QueueEmbeddedColorTarget(
        before ? "target_writer_before" : "target_writer_after", path,
        UINT64_C(48) * 1024 * 1024);
    const auto capture_us = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - capture_start).count();
    std::fprintf(stderr,
        "REX_EMBEDDED_TARGET_WRITER phase=%s result=%u frame=%llu draw=%llu "
        "writer=%u vs=0x%016llX ps=0x%016llX surface=0x%08X color=0x%08X "
        "mask=0x%X depth=0x%08X blend=0x%08X textures=0x%08X "
        "window_offset=0x%08X scissor=%08X,%08X scale=%ux%u "
        "capture_us=%lld path=%s deferred=1 submission=%llu open=%u\n",
        before ? "before" : "after", captured ? 1u : 0u,
        static_cast<unsigned long long>(embedded_completed_swap_ordinal + 1),
        static_cast<unsigned long long>(embedded_frame_frontier.submitted_draws + 1),
        target_writer_selection.writer,
        static_cast<unsigned long long>(vertex_shader->ucode_data_hash()),
        static_cast<unsigned long long>(pixel_shader ? pixel_shader->ucode_data_hash() : 0),
        regs[XE_GPU_REG_RB_SURFACE_INFO], regs[XE_GPU_REG_RB_COLOR_INFO],
        normalized_color_mask, normalized_depth_control.value,
        regs[XE_GPU_REG_RB_BLENDCONTROL0], used_texture_mask,
        regs[XE_GPU_REG_PA_SC_WINDOW_OFFSET],
        regs[XE_GPU_REG_PA_SC_WINDOW_SCISSOR_TL],
        regs[XE_GPU_REG_PA_SC_WINDOW_SCISSOR_BR],
        render_target_cache_->GetDrawScaleX(), render_target_cache_->GetDrawScaleY(),
        static_cast<long long>(capture_us), path,
        static_cast<unsigned long long>(capture_submission), IsSubmissionOpen() ? 1u : 0u);
    std::fflush(stderr);
  };
  if (target_writer_selection.before && embedded_target_writer_config_.capture_pairs) {
    const uint64_t context_draw = embedded_frame_frontier.submitted_draws + 1;
    std::fprintf(stderr,
        "REX_EMBEDDED_TARGET_INPUT frame=%llu draw=%llu writer=%u "
        "used=0x%08X scaled=0x%08X vs_mod=0x%016llX ps_mod=0x%016llX "
        "stencil=%08X stencil_back=%08X raster=%08X\n",
        static_cast<unsigned long long>(embedded_completed_swap_ordinal + 1),
        static_cast<unsigned long long>(context_draw), target_writer_selection.writer,
        used_texture_mask, system_constants_.textures_resolution_scaled,
        static_cast<unsigned long long>(vertex_shader_translation->modification()),
        static_cast<unsigned long long>(pixel_shader_translation->modification()),
        regs[XE_GPU_REG_RB_STENCILREFMASK], regs[XE_GPU_REG_RB_STENCILREFMASK_BF],
        regs[XE_GPU_REG_PA_SU_SC_MODE_CNTL]);
    std::array<uint32_t, 8> slots{};
    size_t slot_count = 0;
    for (uint32_t slot = 0; slot < 32 && slot_count < slots.size(); ++slot) {
      if (!(used_texture_mask & (uint32_t(1) << slot))) continue;
      slots[slot_count++] = slot;
      const auto fetch = regs.GetTextureFetch(slot);
      D3D12TextureCache::ActiveTextureDiagnostic diagnostic{};
      const bool valid = texture_cache_->GetActiveTextureDiagnostic(slot, diagnostic);
      if (REXCVAR_GET(embedded_target_writer_texture_source) && valid &&
          !diagnostic.scaled_resolve) {
        char source_path[128];
        std::snprintf(source_path, sizeof(source_path),
            "rex_target_writer_%u_texture_source_%u.bin", target_writer_selection.writer, slot);
        const bool queued = shared_memory_->QueueTextureSourceReadback(
            diagnostic.guest_base, diagnostic.guest_size, source_path, context_draw);
        std::fprintf(stderr,
            "REX_EMBEDDED_TARGET_TEXTURE_SOURCE draw=%llu writer=%u slot=%u "
            "address=0x%08X bytes=%u queued=%u path=%s scope=resident_at_draw_not_upload_epoch\n",
            static_cast<unsigned long long>(context_draw), target_writer_selection.writer,
            slot, diagnostic.guest_base, diagnostic.guest_size, queued ? 1u : 0u, source_path);
      }
      std::fprintf(stderr,
          "REX_EMBEDDED_TARGET_TEXTURE draw=%llu writer=%u slot=%u valid=%u "
          "fetch=%08X,%08X,%08X,%08X,%08X,%08X guest=0x%08X "
          "guest_extent=%ux%ux%u guest_format=%u scaled=%u outdated=0x%X "
          "resource=0x%016llX host_extent=%llux%ux%u host_format=%u "
          "descriptor=%u signed_descriptor=%u\n",
          static_cast<unsigned long long>(context_draw), target_writer_selection.writer,
          slot, valid ? 1u : 0u, fetch.dword_0, fetch.dword_1, fetch.dword_2,
          fetch.dword_3, fetch.dword_4, fetch.dword_5, diagnostic.guest_base,
          diagnostic.guest_width, diagnostic.guest_height,
          diagnostic.guest_depth_or_array_size, diagnostic.guest_format,
          diagnostic.scaled_resolve, diagnostic.outdated_mask,
          static_cast<unsigned long long>(diagnostic.resource_identity),
          static_cast<unsigned long long>(diagnostic.resource_width),
          diagnostic.resource_height, diagnostic.resource_depth_or_array_size,
          diagnostic.resource_format, diagnostic.descriptor_index,
          diagnostic.descriptor_index_signed);
    }
    // The same depth allocation is rewritten for successive lights. Include
    // the draw epoch, not merely resource identity, in diagnostic deduplication.
    // The existing 48-subresource / 256-MiB global budget remains authoritative.
    texture_cache_->CaptureActiveTextureReadbackDiagnostics(
        slots.data(), slot_count, false, context_draw, true);

    const auto dump_input = [&](const char* kind, const void* data, size_t bytes) {
      char input_path[128];
      std::snprintf(input_path, sizeof(input_path),
                    "rex_target_writer_%u_%s.bin", target_writer_selection.writer, kind);
      size_t written = 0;
      if (data && bytes && bytes <= 1024 * 1024) {
        if (FILE* file = std::fopen(input_path, "wb")) {
          written = std::fwrite(data, 1, bytes, file);
          if (std::fclose(file) != 0) written = 0;
        }
      }
      std::fprintf(stderr,
          "REX_EMBEDDED_TARGET_INPUT_DUMP draw=%llu kind=%s bytes=%llu "
          "written=%llu result=%u path=%s\n",
          static_cast<unsigned long long>(context_draw), kind,
          static_cast<unsigned long long>(bytes), static_cast<unsigned long long>(written),
          bytes && written == bytes ? 1u : 0u, input_path);
    };
    // Raw host-endian register words, not an approximation of shader inputs.
    dump_input("float_constants", &regs[XE_GPU_REG_SHADER_CONSTANT_000_X], 512 * 16);
    dump_input("loop_constants", &regs[XE_GPU_REG_SHADER_CONSTANT_LOOP_00], 32 * 4);
    dump_input("bool_constants", &regs[XE_GPU_REG_SHADER_CONSTANT_BOOL_000_031], 8 * 4);
    // Capture the GPU-resident index/vertex bytes, not later CPU RAM. The
    // existing exact-pair selection bounds this to one frame/four operations.
    std::fprintf(stderr,
        "REX_EMBEDDED_TARGET_GEOMETRY draw=%llu writer=%u indices=%u index_type=%u "
        "index_base=0x%08X index_format=%u index_endian=%u index_offset=%u "
        "viewport=%u,%u,%u,%u ndc_scale=%.9g,%.9g,%.9g ndc_offset=%.9g,%.9g,%.9g vte=0x%08X\n",
        static_cast<unsigned long long>(context_draw), target_writer_selection.writer,
        index_count, uint32_t(primitive_processing_result.index_buffer_type),
        primitive_processing_result.guest_index_base,
        uint32_t(primitive_processing_result.host_index_format),
        uint32_t(primitive_processing_result.host_shader_index_endian),
        regs.Get<reg::VGT_INDX_OFFSET>().indx_offset,
        viewport_info.xy_offset[0], viewport_info.xy_offset[1],
        viewport_info.xy_extent[0], viewport_info.xy_extent[1],
        viewport_info.ndc_scale[0], viewport_info.ndc_scale[1], viewport_info.ndc_scale[2],
        viewport_info.ndc_offset[0], viewport_info.ndc_offset[1], viewport_info.ndc_offset[2],
        regs.Get<reg::PA_CL_VTE_CNTL>().value);
    if (primitive_processing_result.index_buffer_type ==
        PrimitiveProcessor::ProcessedIndexBufferType::kGuestDMA) {
      char path[128];
      std::snprintf(path, sizeof(path), "rex_target_writer_%u_indices.bin", target_writer_selection.writer);
      shared_memory_->QueueGeometryReadback(primitive_processing_result.guest_index_base,
          uint64_t(index_count) * (primitive_processing_result.host_index_format ==
              xenos::IndexFormat::kInt16 ? 2u : 4u), path, context_draw);
    }
    uint32_t captured_vertex_streams = 0;
    for (const Shader::VertexBinding& binding : vertex_shader->vertex_bindings()) {
      if (captured_vertex_streams++ >= 8) break;
      const auto fetch = regs.GetVertexFetch(binding.fetch_constant);
      char path[128];
      std::snprintf(path, sizeof(path), "rex_target_writer_%u_vertex_%u.bin",
          target_writer_selection.writer, binding.fetch_constant);
      const bool queued = shared_memory_->QueueGeometryReadback(
          fetch.address << 2, uint64_t(fetch.size) * 4, path, context_draw);
      std::fprintf(stderr,
          "REX_EMBEDDED_TARGET_VERTEX draw=%llu writer=%u fetch=%u words=%08X,%08X "
          "stride=%u endian=%u queued=%u path=%s\n",
          static_cast<unsigned long long>(context_draw), target_writer_selection.writer,
          binding.fetch_constant, fetch.dword_0, fetch.dword_1, binding.stride_words,
          uint32_t(fetch.endian), queued ? 1u : 0u, path);
    }
    // All pairs have the exact PS filter; preserve shader binaries once.
    static bool target_writer_shader_dumped = false;
    if (!target_writer_shader_dumped) {
      target_writer_shader_dumped = true;
      dump_input("ps_ucode_le", pixel_shader->ucode_dwords(),
                 pixel_shader->ucode_dword_count() * sizeof(uint32_t));
      dump_input("vs_ucode_le", vertex_shader->ucode_dwords(),
                 vertex_shader->ucode_dword_count() * sizeof(uint32_t));
      dump_input("ps_host", pixel_shader_translation->translated_binary().data(),
                 pixel_shader_translation->translated_binary().size());
      dump_input("vs_host", vertex_shader_translation->translated_binary().data(),
                 vertex_shader_translation->translated_binary().size());
      const auto& disassembly = pixel_shader->ucode_disassembly();
      dump_input("ps_disassembly", disassembly.data(), disassembly.size());
    }
    std::fflush(stderr);
  }
  if (target_writer_selection.before) capture_target_writer(true);

  if (capture_embedded_scene_seed_draw) {
    static bool embedded_scene_seed_pre_draw_captured = false;
    if (!embedded_scene_seed_pre_draw_captured) {
      embedded_scene_seed_pre_draw_captured = true;
      constexpr char kSceneSeedPreDrawPath[] =
          "rex_scene_seed_before_draw_1_fp16.bin";
      const bool captured = render_target_cache_->CaptureEmbeddedColorTarget(
          "scene_seed_before_draw_1", kSceneSeedPreDrawPath);
      std::fprintf(stderr,
                   "REX_EMBEDDED_SCENE_SEED_PRE_CAPTURE result=%u path=%s\n",
                   captured ? 1u : 0u, kSceneSeedPreDrawPath);
      std::fflush(stderr);
    }
  }

  if (capture_embedded_mixed_scale_scaled_draw) {
    static bool embedded_mixed_scale_scaled_pre_draw_captured = false;
    if (!embedded_mixed_scale_scaled_pre_draw_captured) {
      embedded_mixed_scale_scaled_pre_draw_captured = true;
      constexpr char kMixedScaleScaledPreDrawPath[] =
          "rex_mixed_scale_scaled_before_draw_fp16.bin";
      const bool captured = render_target_cache_->CaptureEmbeddedColorTarget(
          "mixed_scale_scaled_before_draw", kMixedScaleScaledPreDrawPath);
      std::fprintf(
          stderr,
          "REX_EMBEDDED_MIXED_SCALE_PRE_CAPTURE result=%u path=%s\n",
          captured ? 1u : 0u, kMixedScaleScaledPreDrawPath);
      std::fflush(stderr);
    }
  }

  // Capture the actual D3D12 vertex-stage SV_Position stream for the matching
  // scaled scene passes. The PSO declares only SV_Position for stream 0 and
  // continues rasterizing the same stream; this binding changes no shader,
  // target, depth/stencil state, resource input, or guest-visible result.
  const char* embedded_scene_host_vertex_output_kind = nullptr;
  if (!kernel_state_ &&
      REXCVAR_GET(d3d12_embedded_scene_host_vertex_output_diagnostic) &&
      embedded_scene_host_vertex_output_resources_available_ &&
      IsCurrentEmbeddedGameplayCaptureFrame() &&
      regs.Get<reg::RB_SURFACE_INFO>().value == 0x14010500 &&
      regs.Get<reg::RB_DEPTH_INFO>().value == 0x00010000 &&
      embedded_scene_host_vertex_output_record_count_ <
          kEmbeddedSceneHostVertexOutputRecordCount) {
    const uint64_t vertex_shader_hash = vertex_shader->ucode_data_hash();
    const uint64_t pixel_shader_hash =
        pixel_shader ? pixel_shader->ucode_data_hash() : 0;
    if (vertex_shader_hash == UINT64_C(0x52472DD3CF459B83) &&
        pixel_shader_hash == 0 &&
        normalized_depth_control.value == 0x18708767) {
      embedded_scene_host_vertex_output_kind = "prepass";
    } else if (vertex_shader_hash == UINT64_C(0x539FB8DE2DD7715A) &&
               pixel_shader_hash == UINT64_C(0x7EF6E3B55D32AEB4) &&
               normalized_depth_control.value == 0x18708722) {
      embedded_scene_host_vertex_output_kind = "equal_control";
    } else if (vertex_shader_hash == UINT64_C(0x818A2B33A7AB4ECE) &&
               pixel_shader_hash == UINT64_C(0xBE763931E2AB7D56) &&
               normalized_depth_control.value == 0x18700227) {
      embedded_scene_host_vertex_output_kind = "equal_shaded";
    }
  }
  const bool capture_embedded_scene_host_vertex_output =
      embedded_scene_host_vertex_output_kind != nullptr;
  if (capture_embedded_scene_host_vertex_output) {
    PushTransitionBarrier(
        embedded_scene_host_vertex_output_buffer_.Get(),
        embedded_scene_host_vertex_output_buffer_state_,
        D3D12_RESOURCE_STATE_COPY_DEST);
    embedded_scene_host_vertex_output_buffer_state_ =
        D3D12_RESOURCE_STATE_COPY_DEST;
    SubmitBarriers();
    deferred_command_list_.D3DCopyBufferRegion(
        embedded_scene_host_vertex_output_buffer_.Get(),
        kEmbeddedSceneHostVertexOutputDataSize,
        embedded_scene_host_vertex_output_zero_upload_.Get(), 0,
        sizeof(uint64_t));
    PushTransitionBarrier(
        embedded_scene_host_vertex_output_buffer_.Get(),
        embedded_scene_host_vertex_output_buffer_state_,
        D3D12_RESOURCE_STATE_STREAM_OUT);
    embedded_scene_host_vertex_output_buffer_state_ =
        D3D12_RESOURCE_STATE_STREAM_OUT;
    SubmitBarriers();
    const D3D12_GPU_VIRTUAL_ADDRESS output_gpu_address =
        embedded_scene_host_vertex_output_buffer_->GetGPUVirtualAddress();
    D3D12_STREAM_OUTPUT_BUFFER_VIEW output_view = {};
    output_view.BufferLocation = output_gpu_address;
    output_view.SizeInBytes = kEmbeddedSceneHostVertexOutputDataSize;
    output_view.BufferFilledSizeLocation =
        output_gpu_address + kEmbeddedSceneHostVertexOutputDataSize;
    deferred_command_list_.D3DSOSetTargets(0, 1, &output_view);
  }

  // Draw. Wrap only the exact verified prompt background in a host occlusion
  // query. This is observational and distinguishes zero raster/depth coverage
  // from a pixel/output-merger issue without changing any guest-visible state.
  // Do the same, when the bounded scene-pair trace is explicitly enabled, for
  // the matching prepass and EQUAL-tested draws. The resulting host sample
  // counts distinguish a raster/depth coverage divergence from downstream
  // color output. Query results are resolved together after the sixteenth
  // record so the diagnostic adds only one queue wait to the captured frame.
  struct EmbeddedSceneOcclusionRecord {
    uint32_t query_index;
    uint32_t ordinal;
    uint32_t scale_area;
    const char* kind;
  };
  static std::array<EmbeddedSceneOcclusionRecord, 16>
      embedded_scene_occlusion_records = {};
  static uint32_t embedded_scene_occlusion_record_count = 0;
  uint32_t embedded_scene_occlusion_query_index = UINT32_MAX;
  const char* embedded_scene_occlusion_kind = nullptr;
  if (!kernel_state_ && REXCVAR_GET(embedded_scene_depth_pair_trace) &&
      IsCurrentEmbeddedGameplayCaptureFrame() &&
      regs.Get<reg::RB_SURFACE_INFO>().value == 0x14010500 &&
      embedded_scene_occlusion_record_count <
          embedded_scene_occlusion_records.size()) {
    const uint64_t vertex_shader_hash = vertex_shader->ucode_data_hash();
    const uint64_t pixel_shader_hash =
        pixel_shader ? pixel_shader->ucode_data_hash() : 0;
    if (vertex_shader_hash == UINT64_C(0x52472DD3CF459B83) &&
        pixel_shader_hash == 0 &&
        normalized_depth_control.value == 0x18708767) {
      embedded_scene_occlusion_kind = "prepass";
    } else if (vertex_shader_hash == UINT64_C(0x539FB8DE2DD7715A) &&
               pixel_shader_hash == UINT64_C(0x7EF6E3B55D32AEB4) &&
               normalized_depth_control.value == 0x18708722) {
      embedded_scene_occlusion_kind = "equal_control";
    } else if (vertex_shader_hash == UINT64_C(0x818A2B33A7AB4ECE) &&
               pixel_shader_hash == UINT64_C(0xBE763931E2AB7D56) &&
               normalized_depth_control.value == 0x18700227) {
      embedded_scene_occlusion_kind = "equal_shaded";
    }
    if (embedded_scene_occlusion_kind) {
      if (occlusion_query_resources_available_ && occlusion_query_heap_ &&
          occlusion_query_readback_ && !logical_occlusion_query_.valid &&
          !active_occlusion_query_.valid && occlusion_reports_.empty() &&
          AcquireOcclusionQueryIndex(embedded_scene_occlusion_query_index)) {
        deferred_command_list_.D3DBeginQuery(
            occlusion_query_heap_.Get(), D3D12_QUERY_TYPE_OCCLUSION,
            embedded_scene_occlusion_query_index);
      } else {
        embedded_scene_occlusion_query_index = UINT32_MAX;
        std::fprintf(
            stderr,
            "REX_EMBEDDED_SCENE_OCCLUSION result=skipped kind=%s "
            "resources=%u guest_query_active=%u pending=%zu\n",
            embedded_scene_occlusion_kind,
            occlusion_query_resources_available_ ? 1u : 0u,
            active_occlusion_query_.valid ? 1u : 0u,
            occlusion_reports_.segments.size());
        std::fflush(stderr);
      }
    }
  }

  // Count fragments reaching the real two-sided shadow-volume stencil
  // operation. The query is closed before the phase readback below, whose
  // existing bounded queue wait also makes the result available. No duplicate
  // draw is issued and no depth, stencil, color, or guest state is changed.
  uint32_t embedded_scene_phase_shadow_query_index = UINT32_MAX;
  if (embedded_scene_phase_shadow_volume_draw) {
    if (occlusion_query_resources_available_ && occlusion_query_heap_ &&
        occlusion_query_readback_ && !logical_occlusion_query_.valid &&
        !active_occlusion_query_.valid && occlusion_reports_.empty() &&
        AcquireOcclusionQueryIndex(
            embedded_scene_phase_shadow_query_index)) {
      deferred_command_list_.D3DBeginQuery(
          occlusion_query_heap_.Get(), D3D12_QUERY_TYPE_OCCLUSION,
          embedded_scene_phase_shadow_query_index);
    } else {
      embedded_scene_phase_shadow_query_index = UINT32_MAX;
      std::fprintf(stderr,
                   "REX_EMBEDDED_SCENE_SHADOW_OCCLUSION result=skipped "
                   "shadow_ordinal=%u resources=%u guest_query_active=%u\n",
                   embedded_scene_phase_shadow_volume_count + 1,
                   occlusion_query_resources_available_ ? 1u : 0u,
                   active_occlusion_query_.valid ? 1u : 0u);
      std::fflush(stderr);
    }
  }

  uint32_t embedded_prompt_occlusion_query_index = UINT32_MAX;
  if (embedded_prompt_background_draw) {
    static bool prompt_occlusion_query_attempted = false;
    if (!prompt_occlusion_query_attempted) {
      prompt_occlusion_query_attempted = true;
      if (occlusion_query_resources_available_ && occlusion_query_heap_ &&
          occlusion_query_readback_ && !logical_occlusion_query_.valid &&
          !active_occlusion_query_.valid && occlusion_reports_.empty() &&
          AcquireOcclusionQueryIndex(embedded_prompt_occlusion_query_index)) {
        deferred_command_list_.D3DBeginQuery(occlusion_query_heap_.Get(),
                                             D3D12_QUERY_TYPE_OCCLUSION,
                                             embedded_prompt_occlusion_query_index);
      } else {
        embedded_prompt_occlusion_query_index = UINT32_MAX;
        std::fprintf(stderr,
                     "REX_EMBEDDED_PROMPT_OCCLUSION result=skipped resources=%u "
                     "guest_query_active=%u\n",
                     occlusion_query_resources_available_ ? 1u : 0u,
                     active_occlusion_query_.valid ? 1u : 0u);
        std::fflush(stderr);
      }
    }
  }

  if (primitive_processing_result.index_buffer_type ==
      PrimitiveProcessor::ProcessedIndexBufferType::kNone) {
    if (memexport_used) {
      shared_memory_->UseForWriting();
    } else {
      shared_memory_->UseForReading();
    }
    SubmitBarriers();
    PROFILE_DRAW_CALL();
    PROFILE_VERTICES(primitive_processing_result.host_draw_vertex_count);
    GpuTimingMark(GpuTimingCategory::kDraw);
    deferred_command_list_.D3DDrawInstanced(primitive_processing_result.host_draw_vertex_count, 1,
                                            0, 0);
  } else {
    D3D12_INDEX_BUFFER_VIEW index_buffer_view;
    index_buffer_view.SizeInBytes = primitive_processing_result.host_draw_vertex_count;
    if (primitive_processing_result.host_index_format == xenos::IndexFormat::kInt16) {
      index_buffer_view.SizeInBytes *= sizeof(uint16_t);
      index_buffer_view.Format = DXGI_FORMAT_R16_UINT;
    } else {
      index_buffer_view.SizeInBytes *= sizeof(uint32_t);
      index_buffer_view.Format = DXGI_FORMAT_R32_UINT;
    }
    ID3D12Resource* scratch_index_buffer = nullptr;
    switch (primitive_processing_result.index_buffer_type) {
      case PrimitiveProcessor::ProcessedIndexBufferType::kGuestDMA: {
        if (memexport_used) {
          // If the shared memory is a UAV, it can't be used as an index buffer
          // (UAV is a read/write state, index buffer is a read-only state).
          // Need to copy the indices to a buffer in the index buffer state.
          scratch_index_buffer = RequestScratchGPUBuffer(index_buffer_view.SizeInBytes,
                                                         D3D12_RESOURCE_STATE_COPY_DEST);
          if (scratch_index_buffer == nullptr) {
            return fail_embedded_draw("scratch_index_buffer");
          }
          shared_memory_->UseAsCopySource();
          SubmitBarriers();
          deferred_command_list_.D3DCopyBufferRegion(
              scratch_index_buffer, 0, shared_memory_->GetBuffer(),
              primitive_processing_result.guest_index_base, index_buffer_view.SizeInBytes);
          PushTransitionBarrier(scratch_index_buffer, D3D12_RESOURCE_STATE_COPY_DEST,
                                D3D12_RESOURCE_STATE_INDEX_BUFFER);
          index_buffer_view.BufferLocation = scratch_index_buffer->GetGPUVirtualAddress();
        } else {
          index_buffer_view.BufferLocation =
              shared_memory_->GetGPUAddress() + primitive_processing_result.guest_index_base;
        }
      } break;
      case PrimitiveProcessor::ProcessedIndexBufferType::kHostConverted:
        index_buffer_view.BufferLocation = primitive_processor_->GetConvertedIndexBufferGpuAddress(
            primitive_processing_result.host_index_buffer_handle);
        break;
      case PrimitiveProcessor::ProcessedIndexBufferType::kHostBuiltinForAuto:
      case PrimitiveProcessor::ProcessedIndexBufferType::kHostBuiltinForDMA:
        index_buffer_view.BufferLocation = primitive_processor_->GetBuiltinIndexBufferGpuAddress(
            primitive_processing_result.host_index_buffer_handle);
        break;
      default:
        assert_unhandled_case(primitive_processing_result.index_buffer_type);
        return fail_embedded_draw("unknown_index_buffer_type");
    }
    deferred_command_list_.D3DIASetIndexBuffer(&index_buffer_view);
    if (memexport_used) {
      shared_memory_->UseForWriting();
    } else {
      shared_memory_->UseForReading();
    }
    SubmitBarriers();
    PROFILE_DRAW_CALL();
    PROFILE_VERTICES(primitive_processing_result.host_draw_vertex_count);
    GpuTimingMark(GpuTimingCategory::kDraw);
    deferred_command_list_.D3DDrawIndexedInstanced(
        primitive_processing_result.host_draw_vertex_count, 1, 0, 0, 0);
    if (scratch_index_buffer != nullptr) {
      ReleaseScratchGPUBuffer(scratch_index_buffer, D3D12_RESOURCE_STATE_INDEX_BUFFER);
    }
  }

  // Count actual guest draws in this submission, not attempts or diagnostic
  // draws. Saturation retains a pending batch when a safe boundary is delayed.
  if (guest_draw_submit_limit_ &&
      submission_guest_draws_ < guest_draw_submit_limit_) {
    ++submission_guest_draws_;
  }

  // Immediately after the actual draw, before optional diagnostic passes can
  // reuse bindings. Copy the upload already bound by UpdateBindings; no GPU
  // readback, wait, synthesized transform, or guest register mutation.
  if (kGpuDiagnostics && !kernel_state_ && pixel_shader && texture_cache_->OwnsSceneHistory() &&
      pixel_shader->ucode_data_hash() == UINT64_C(0x7EF6E3B55D32AEB4)) {
    // Keep the large owned record off the ordinary draw stack when history is off.
    const auto record_owned_transform = [&]() __attribute__((noinline)) {
      pc_draw_transform_history::Draw input;
      input.vertex_shader = vertex_shader->ucode_data_hash();
      const bool valid = input.ReadUpload(embedded_float_vertex_cpu_address_,
          embedded_float_vertex_cpu_size_, constant_map_vertex.float_count,
          constant_map_vertex.float_bitmap) && !memexport_used &&
          draw_resolution_scale_x == 1 && draw_resolution_scale_y == 1;
      uint32_t owned_word = 0;
      for (uint32_t reg : {0u,1u,2u,3u,7u,12u,13u,14u,15u}) {
        for (uint32_t component = 0; component < 4; ++component, ++owned_word) {
          input.writers[owned_word] = valid ? pc_constant_writer::Matching(
              embedded_float_constant_writes_[reg * 4 + component], input.constants[owned_word])
              : pc_constant_writer::Record{};
        }
      }
      for (uint32_t i = 0; i < 6; ++i)
        input.viewport[i] = regs[XE_GPU_REG_PA_CL_VPORT_XSCALE + i];
      input.window_offset = regs[XE_GPU_REG_PA_SC_WINDOW_OFFSET];
      input.scissor_tl = regs[XE_GPU_REG_PA_SC_WINDOW_SCISSOR_TL];
      input.scissor_br = regs[XE_GPU_REG_PA_SC_WINDOW_SCISSOR_BR];
      input.vte = regs[XE_GPU_REG_PA_CL_VTE_CNTL];
      input.clip = regs[XE_GPU_REG_PA_CL_CLIP_CNTL];
      input.surface = regs[XE_GPU_REG_RB_SURFACE_INFO];
      input.depth = regs[XE_GPU_REG_RB_DEPTH_INFO];
      texture_cache_->RecordOwnedDrawTransform(input, valid);
    };
    record_owned_transform();
  }
  if (kGpuDiagnostics && !kernel_state_ && pixel_shader) {
    bool owned_proof = IsCurrentEmbeddedGameplayCaptureFrame();
    if (REXCVAR_GET(embedded_camera_history_capture) &&
        REXCVAR_GET(embedded_camera_history_capture_frames) == 6) {
      owned_proof = embedded_camera_history_budget.BeginOwnedProof(embedded_completed_swap_ordinal + 1);
    }
    texture_cache_->CopyOwnedSceneColor(pixel_shader->ucode_data_hash(), owned_proof);
  }
  const bool capture_camera_draw = kGpuDiagnostics && !kernel_state_ &&
      REXCVAR_GET(embedded_camera_draw_capture) &&
      embedded_camera_draw_budget.Select(IsCurrentEmbeddedGameplayCaptureFrame(),
          embedded_completed_swap_ordinal + 1, embedded_frame_frontier.submitted_draws + 1);
  const bool capture_history_draw = kGpuDiagnostics && !kernel_state_ &&
      REXCVAR_GET(embedded_camera_history_capture) &&
      REXCVAR_GET(embedded_camera_draw_capture) && REXCVAR_GET(embedded_hitch_diagnostics) &&
      embedded_camera_history_budget.Select(embedded_completed_swap_ordinal + 1,
          embedded_frame_frontier.submitted_draws + 1, pixel_shader ? pixel_shader->ucode_data_hash() : 0);
  if (capture_camera_draw || capture_history_draw) {
    // Cold capture copies need their own frame, including when both captures are off.
    const auto capture_draw_inputs = [&]() __attribute__((noinline)) {
      EmbeddedCameraDraw draw;
      draw.draw = embedded_frame_frontier.submitted_draws + 1;
      draw.vertex_shader = vertex_shader->ucode_data_hash();
      draw.pixel_shader = pixel_shader ? pixel_shader->ucode_data_hash() : 0;
      draw.constant_buffer = cbuffer_binding_float_vertex_.address;
      draw.surface = regs[XE_GPU_REG_RB_SURFACE_INFO];
      draw.depth = regs[XE_GPU_REG_RB_DEPTH_INFO];
      draw.depth_control = normalized_depth_control.value;
      draw.window_offset = regs[XE_GPU_REG_PA_SC_WINDOW_OFFSET];
      draw.scissor_tl = regs[XE_GPU_REG_PA_SC_WINDOW_SCISSOR_TL];
      draw.scissor_br = regs[XE_GPU_REG_PA_SC_WINDOW_SCISSOR_BR];
      for (uint32_t i = 0; i < 6; ++i)
        draw.viewport[i] = regs[XE_GPU_REG_PA_CL_VPORT_XSCALE + i];
      draw.vte = regs[XE_GPU_REG_PA_CL_VTE_CNTL];
      draw.clip = regs[XE_GPU_REG_PA_CL_CLIP_CNTL];
      draw.raster = regs[XE_GPU_REG_PA_SU_SC_MODE_CNTL];
      draw.vertex_control = regs[XE_GPU_REG_PA_SU_VTX_CNTL];
      std::memcpy(draw.host_viewport, &ff_viewport_, sizeof(draw.host_viewport));
      std::memcpy(draw.host_scissor, &ff_scissor_, sizeof(draw.host_scissor));
      for (uint32_t i = 0; i < 3; ++i) {
        std::memcpy(&draw.host_ndc[i * 2], &viewport_info.ndc_scale[i], sizeof(uint32_t));
        std::memcpy(&draw.host_ndc[i * 2 + 1], &viewport_info.ndc_offset[i], sizeof(uint32_t));
      }
      draw.scale_x = draw_resolution_scale_x;
      draw.scale_y = draw_resolution_scale_y;
      draw.constants.Capture(embedded_float_vertex_cpu_address_,
          embedded_float_vertex_cpu_size_, constant_map_vertex.float_count,
          constant_map_vertex.float_bitmap);
      if (REXCVAR_GET(embedded_camera_scene_capture) || capture_history_draw) {
        if (pixel_shader) {
          draw.pixel_constant_buffer = cbuffer_binding_float_pixel_.address;
          const auto& pixel_map = pixel_shader->constant_register_map();
          draw.pixel_constants.Capture(embedded_float_pixel_cpu_address_,
              embedded_float_pixel_cpu_size_, pixel_map.float_count, pixel_map.float_bitmap);
        } else {
          const uint64_t empty_map[4]{};
          draw.pixel_constants.Capture(nullptr, 0, 0, empty_map);
        }
        for (uint32_t i = 0; i < 4; ++i) {
          draw.target_color[i] = regs[reg::RB_COLOR_INFO::rt_register_indices[i]];
          draw.target_blend[i] = regs[reg::RB_BLENDCONTROL::rt_register_indices[i]];
        }
        draw.target_mask = normalized_color_mask;
        draw.target_control = regs[XE_GPU_REG_RB_COLORCONTROL];
        draw.target_bits = bound_depth_and_color_render_target_bits;
        draw.textures = used_texture_mask;
        draw.primitive = uint32_t(primitive_type);
        draw.vertices = primitive_processing_result.host_draw_vertex_count;
      }
      if (capture_history_draw) {
        EmbeddedHistoryDraw history;
        history.camera = draw;
        history.submission = GetCurrentSubmission();
        history.memexport = memexport_used;
        history.index_kind = uint32_t(primitive_processing_result.index_buffer_type);
        history.index_address = primitive_processing_result.guest_index_base;
        history.index_format = uint32_t(primitive_processing_result.host_index_format);
        history.index_endian = uint32_t(primitive_processing_result.host_shader_index_endian);
        history.index_offset = regs.Get<reg::VGT_INDX_OFFSET>().indx_offset;
        history.min_index = regs.Get<reg::VGT_MIN_VTX_INDX>().min_indx;
        history.max_index = regs.Get<reg::VGT_MAX_VTX_INDX>().max_indx;
        history.reset_enable = regs.Get<reg::PA_SU_SC_MODE_CNTL>().multi_prim_ib_ena;
        history.reset_index = regs.Get<reg::VGT_MULTI_PRIM_IB_RESET_INDX>().reset_indx;
        history.bindings = uint32_t(vertex_shader->vertex_bindings().size());
        history.bindings_complete = history.bindings <= history.vertices.size();
        uint32_t slot = 0;
        for (const auto& binding : vertex_shader->vertex_bindings()) {
          if (slot == history.vertices.size()) break;
          const auto fetch = regs.GetVertexFetch(binding.fetch_constant);
          history.vertices[slot++] = {binding.fetch_constant, fetch.dword_0, fetch.dword_1, binding.stride_words};
        }
        embedded_camera_history_draws[embedded_camera_history_budget.finished].push_back(std::move(history));
      }
      if (capture_camera_draw) embedded_camera_draws.push_back(std::move(draw));
    };
    capture_draw_inputs();
  }

  // Preserve the GPU bytes resident at this actual submitted draw. Reuse the
  // deferred SharedMemory copy/completion path; no CPU substitute, new draw,
  // command-list close/reset or wait. The separate complete-range budget never
  // upgrades the legacy geometry preview into an apparently complete stream.
  if (kGpuDiagnostics && !kernel_state_ && REXCVAR_GET(embedded_camera_draw_capture) &&
      REXCVAR_GET(embedded_camera_geometry_capture) &&
      embedded_camera_geometry_budget.Select({embedded_completed_swap_ordinal + 1,
          embedded_frame_frontier.submitted_draws + 1, vertex_shader->ucode_data_hash(),
          IsCurrentEmbeddedGameplayCaptureFrame(), memexport_used,
          regs[XE_GPU_REG_RB_SURFACE_INFO], regs[XE_GPU_REG_RB_DEPTH_INFO],
          normalized_depth_control.value, draw_resolution_scale_x, draw_resolution_scale_y})) {
    const uint64_t geometry_frame = embedded_completed_swap_ordinal + 1;
    const uint64_t geometry_draw = embedded_frame_frontier.submitted_draws + 1;
    const auto index_kind = primitive_processing_result.index_buffer_type;
    const bool guest_dma = index_kind == PrimitiveProcessor::ProcessedIndexBufferType::kGuestDMA;
    const bool supported = (guest_dma || index_kind == PrimitiveProcessor::ProcessedIndexBufferType::kNone) &&
        vertex_shader->vertex_bindings().size() == 1;
    const uint32_t geometry_index_count = primitive_processing_result.host_draw_vertex_count;
    const uint32_t index_element_bytes =
        primitive_processing_result.host_index_format == xenos::IndexFormat::kInt16 ? 2u : 4u;
    char index_path[128];
    std::snprintf(index_path, sizeof(index_path), "rex_camera_geometry_frame_%llu_draw_%llu_indices.bin",
        static_cast<unsigned long long>(geometry_frame), static_cast<unsigned long long>(geometry_draw));
    const bool index_queued = supported && guest_dma && shared_memory_->QueueCameraGeometryReadback(
        primitive_processing_result.guest_index_base, uint64_t(geometry_index_count) * index_element_bytes,
        index_path, geometry_draw);
    std::fprintf(stderr,
        "REX_CAMERA_GEOMETRY frame=%llu draw=%llu vs=%016llX supported=%u bindings=%zu "
        "index_kind=%u indices=%u index_address=%08X index_format=%u index_endian=%u index_offset=%u "
        "min_index=%u max_index=%u reset_enable=%u reset_index=%u index_queued=%u index_path=%s "
        "scope=gpu_resident_after_submitted_draw\n",
        static_cast<unsigned long long>(geometry_frame), static_cast<unsigned long long>(geometry_draw),
        static_cast<unsigned long long>(vertex_shader->ucode_data_hash()), supported ? 1u : 0u,
        vertex_shader->vertex_bindings().size(), uint32_t(index_kind), geometry_index_count,
        primitive_processing_result.guest_index_base, uint32_t(primitive_processing_result.host_index_format),
        uint32_t(primitive_processing_result.host_shader_index_endian),
        regs.Get<reg::VGT_INDX_OFFSET>().indx_offset, regs.Get<reg::VGT_MIN_VTX_INDX>().min_indx,
        regs.Get<reg::VGT_MAX_VTX_INDX>().max_indx,
        regs.Get<reg::PA_SU_SC_MODE_CNTL>().multi_prim_ib_ena,
        regs.Get<reg::VGT_MULTI_PRIM_IB_RESET_INDX>().reset_indx,
        index_queued ? 1u : 0u, guest_dma ? index_path : "none");
    if (supported) {
      const auto& binding = vertex_shader->vertex_bindings().front();
      const auto fetch = regs.GetVertexFetch(binding.fetch_constant);
      char vertex_path[128];
      std::snprintf(vertex_path, sizeof(vertex_path), "rex_camera_geometry_frame_%llu_draw_%llu_vertex_%u.bin",
          static_cast<unsigned long long>(geometry_frame), static_cast<unsigned long long>(geometry_draw),
          binding.fetch_constant);
      const bool vertex_queued = shared_memory_->QueueCameraGeometryReadback(
          fetch.address << 2, uint64_t(fetch.size) * 4, vertex_path, geometry_draw);
      std::fprintf(stderr,
          "REX_CAMERA_GEOMETRY_VERTEX frame=%llu draw=%llu fetch=%u words=%08X,%08X "
          "address=%08X bytes=%llu stride=%u endian=%u queued=%u path=%s\n",
          static_cast<unsigned long long>(geometry_frame), static_cast<unsigned long long>(geometry_draw),
          binding.fetch_constant, fetch.dword_0, fetch.dword_1, fetch.address << 2,
          static_cast<unsigned long long>(uint64_t(fetch.size) * 4), binding.stride_words,
          uint32_t(fetch.endian), vertex_queued ? 1u : 0u, vertex_path);
    }
    std::fflush(stderr);
  }

  if (camera_depth_clear_enabled && embedded_camera_depth_clear_budget.AfterClear(camera_depth_draw)) {
    render_target_cache_->QueueCameraDepthClearReadback(camera_depth_draw.frame,
        camera_depth_draw.ordinal, camera_depth_draw.ordinal, false);
  }

  if (embedded_prompt_occlusion_query_index != UINT32_MAX) {
    deferred_command_list_.D3DEndQuery(occlusion_query_heap_.Get(),
                                       D3D12_QUERY_TYPE_OCCLUSION,
                                       embedded_prompt_occlusion_query_index);
    deferred_command_list_.D3DResolveQueryData(
        occlusion_query_heap_.Get(), D3D12_QUERY_TYPE_OCCLUSION,
        embedded_prompt_occlusion_query_index, 1, occlusion_query_readback_.Get(),
        sizeof(uint64_t) * embedded_prompt_occlusion_query_index);
  }

  if (capture_embedded_scene_host_vertex_output) {
    deferred_command_list_.D3DSOSetTargets(0, 0, nullptr);
    PushTransitionBarrier(
        embedded_scene_host_vertex_output_buffer_.Get(),
        embedded_scene_host_vertex_output_buffer_state_,
        D3D12_RESOURCE_STATE_COPY_SOURCE);
    embedded_scene_host_vertex_output_buffer_state_ =
        D3D12_RESOURCE_STATE_COPY_SOURCE;
    SubmitBarriers();
    const uint32_t record_index =
        embedded_scene_host_vertex_output_record_count_;
    deferred_command_list_.D3DCopyBufferRegion(
        embedded_scene_host_vertex_output_readback_.Get(),
        uint64_t(record_index) *
            kEmbeddedSceneHostVertexOutputRecordStride,
        embedded_scene_host_vertex_output_buffer_.Get(), 0,
        kEmbeddedSceneHostVertexOutputRecordStride);
    EmbeddedSceneHostVertexOutputRecord& record =
        embedded_scene_host_vertex_output_records_[record_index];
    record.kind = embedded_scene_host_vertex_output_kind;
    record.vertex_shader_hash = vertex_shader->ucode_data_hash();
    record.expected_vertex_count =
        primitive_processing_result.host_draw_vertex_count;
    ++embedded_scene_host_vertex_output_record_count_;

    if (embedded_scene_host_vertex_output_record_count_ ==
            kEmbeddedSceneHostVertexOutputRecordCount &&
        AwaitAllQueueOperationsCompletion()) {
      for (uint32_t completed_index = 0;
           completed_index < kEmbeddedSceneHostVertexOutputRecordCount;
           ++completed_index) {
        const uint8_t* completed_base =
            embedded_scene_host_vertex_output_readback_mapping_ +
            size_t(completed_index) *
                kEmbeddedSceneHostVertexOutputRecordStride;
        uint64_t filled_size = 0;
        std::memcpy(&filled_size,
                    completed_base +
                        kEmbeddedSceneHostVertexOutputDataSize,
                    sizeof(filled_size));
        const uint32_t captured_size = uint32_t(std::min<uint64_t>(
            filled_size, kEmbeddedSceneHostVertexOutputDataSize));
        uint64_t output_hash = UINT64_C(14695981039346656037);
        for (uint32_t byte_index = 0; byte_index < captured_size;
             ++byte_index) {
          output_hash ^= completed_base[byte_index];
          output_hash *= UINT64_C(1099511628211);
        }
        const EmbeddedSceneHostVertexOutputRecord& completed_record =
            embedded_scene_host_vertex_output_records_[completed_index];
        char capture_name[192];
        std::snprintf(
            capture_name, sizeof(capture_name),
            "rex_scene_host_sv_position_%u_%s_%016llX.bin",
            completed_index + 1, completed_record.kind,
            static_cast<unsigned long long>(completed_record.vertex_shader_hash));
        bool written = false;
        if (FILE* capture = std::fopen(capture_name, "wb")) {
          written = std::fwrite(completed_base, 1, captured_size,
                                capture) == captured_size;
          std::fclose(capture);
        }
        std::fprintf(
            stderr,
            "REX_EMBEDDED_SCENE_HOST_VERTEX_OUTPUT result=%s ordinal=%u kind=%s vs=0x%016llX expected_vertices=%u filled_bytes=%llu captured_bytes=%u captured_vertices=%u overflow=%u partial_vertex=%u hash=0x%016llX path=%s\n",
            written ? "ok" : "write_failed", completed_index + 1,
            completed_record.kind,
            static_cast<unsigned long long>(
                completed_record.vertex_shader_hash),
            completed_record.expected_vertex_count,
            static_cast<unsigned long long>(filled_size), captured_size,
            captured_size / (sizeof(float) * 4),
            filled_size > kEmbeddedSceneHostVertexOutputDataSize ? 1u : 0u,
            captured_size % (sizeof(float) * 4) != 0 ? 1u : 0u,
            static_cast<unsigned long long>(output_hash), capture_name);
      }
      std::fflush(stderr);
    }
  }

  if (embedded_scene_occlusion_query_index != UINT32_MAX) {
    deferred_command_list_.D3DEndQuery(
        occlusion_query_heap_.Get(), D3D12_QUERY_TYPE_OCCLUSION,
        embedded_scene_occlusion_query_index);
    deferred_command_list_.D3DResolveQueryData(
        occlusion_query_heap_.Get(), D3D12_QUERY_TYPE_OCCLUSION,
        embedded_scene_occlusion_query_index, 1,
        occlusion_query_readback_.Get(),
        sizeof(uint64_t) * embedded_scene_occlusion_query_index);
    EmbeddedSceneOcclusionRecord& record =
        embedded_scene_occlusion_records
            [embedded_scene_occlusion_record_count];
    record.query_index = embedded_scene_occlusion_query_index;
    record.ordinal = embedded_scene_occlusion_record_count + 1;
    record.scale_area = std::max(
        1u, render_target_cache_->GetDrawScaleX() *
                render_target_cache_->GetDrawScaleY());
    record.kind = embedded_scene_occlusion_kind;
    ++embedded_scene_occlusion_record_count;
    if (embedded_scene_occlusion_record_count ==
            embedded_scene_occlusion_records.size() &&
        AwaitAllQueueOperationsCompletion()) {
      for (const EmbeddedSceneOcclusionRecord& completed_record :
           embedded_scene_occlusion_records) {
        const uint64_t raw_samples =
            occlusion_query_readback_mapping_
                ? occlusion_query_readback_mapping_[completed_record.query_index]
                : UINT64_MAX;
        const uint64_t normalized_samples =
            occlusion_query_readback_mapping_
                ? NormalizeOcclusionSamples(raw_samples,
                                            completed_record.scale_area)
                : UINT64_MAX;
        std::fprintf(
            stderr,
            "REX_EMBEDDED_SCENE_OCCLUSION result=%s ordinal=%u kind=%s "
            "query=%u raw_samples=%llu scale_area=%u "
            "normalized_samples=%llu\n",
            occlusion_query_readback_mapping_ ? "ok" : "unmapped",
            completed_record.ordinal, completed_record.kind,
            completed_record.query_index,
            static_cast<unsigned long long>(raw_samples),
            completed_record.scale_area,
            static_cast<unsigned long long>(normalized_samples));
      }
      std::fflush(stderr);
    }
  }

  if (embedded_scene_phase_shadow_query_index != UINT32_MAX) {
    deferred_command_list_.D3DEndQuery(
        occlusion_query_heap_.Get(), D3D12_QUERY_TYPE_OCCLUSION,
        embedded_scene_phase_shadow_query_index);
    deferred_command_list_.D3DResolveQueryData(
        occlusion_query_heap_.Get(), D3D12_QUERY_TYPE_OCCLUSION,
        embedded_scene_phase_shadow_query_index, 1,
        occlusion_query_readback_.Get(),
        sizeof(uint64_t) * embedded_scene_phase_shadow_query_index);
  }

  if (embedded_scene_phase_prepass_draw &&
      embedded_scene_phase_prepass_count < 4) {
    ++embedded_scene_phase_prepass_count;
    const char* label = nullptr;
    switch (embedded_scene_phase_prepass_count) {
      case 1:
        label = "after_prepass_1";
        break;
      case 2:
        label = "after_prepass_2";
        break;
      case 3:
        label = "after_prepass_3";
        break;
      case 4:
        label = "after_prepass_4";
        break;
    }
    const bool captured =
        render_target_cache_->CaptureEmbeddedSceneDepthTarget(label, nullptr);
    std::fprintf(stderr,
                 "REX_EMBEDDED_SCENE_DEPTH_PHASE result=%u label=%s "
                 "prepass_seen=%u shaded_seen=%u\n",
                 captured ? 1u : 0u, label,
                 embedded_scene_phase_prepass_count,
                 embedded_scene_phase_shaded_count);
    std::fflush(stderr);
  }
  if (embedded_scene_phase_shaded_draw &&
      embedded_scene_phase_shaded_count < 8) {
    ++embedded_scene_phase_shaded_count;
  }
  if (embedded_scene_phase_shadow_volume_draw &&
      embedded_scene_phase_shadow_volume_count < 8) {
    ++embedded_scene_phase_shadow_volume_count;
    const char* const labels[] = {
        "after_shadow_volume_1", "after_shadow_volume_2",
        "after_shadow_volume_3", "after_shadow_volume_4",
        "after_shadow_volume_5", "after_shadow_volume_6",
        "after_shadow_volume_7", "after_shadow_volume_8"};
    const char* label =
        labels[embedded_scene_phase_shadow_volume_count - 1];
    const bool captured =
        render_target_cache_->CaptureEmbeddedSceneDepthTarget(label, nullptr);
    const uint32_t scale_area = std::max(
        1u, render_target_cache_->GetDrawScaleX() *
                render_target_cache_->GetDrawScaleY());
    const uint64_t raw_samples =
        embedded_scene_phase_shadow_query_index != UINT32_MAX &&
                occlusion_query_readback_mapping_
            ? occlusion_query_readback_mapping_
                  [embedded_scene_phase_shadow_query_index]
            : UINT64_MAX;
    const uint64_t normalized_samples =
        raw_samples != UINT64_MAX
            ? NormalizeOcclusionSamples(raw_samples, scale_area)
            : UINT64_MAX;
    std::fprintf(
        stderr,
        "REX_EMBEDDED_SCENE_DEPTH_PHASE result=%u label=%s "
        "shadow_seen=%u prepass_seen=%u shaded_seen=%u "
        "occlusion_result=%s raw_samples=%llu scale_area=%u "
        "normalized_samples=%llu\n",
        captured ? 1u : 0u, label,
        embedded_scene_phase_shadow_volume_count,
        embedded_scene_phase_prepass_count,
        embedded_scene_phase_shaded_count,
        raw_samples != UINT64_MAX ? "ok" : "unavailable",
        static_cast<unsigned long long>(raw_samples), scale_area,
        static_cast<unsigned long long>(normalized_samples));
    std::fflush(stderr);
  }

  if (target_writer_selection.after) capture_target_writer(false);

  if (embedded_color_map_capture_stage) {
    char output_path[96];
    std::snprintf(output_path, sizeof(output_path),
                  "rex_color_map_stage_%u_after_draw_fp16.bin",
                  embedded_color_map_capture_stage);
    const bool captured = render_target_cache_->CaptureEmbeddedColorTarget(
        "color_map_chain_after_draw", output_path);
    std::fprintf(stderr,
                 "REX_EMBEDDED_COLOR_MAP_OUTPUT stage=%u result=%u path=%s\n",
                 embedded_color_map_capture_stage, captured ? 1u : 0u, output_path);
    std::fflush(stderr);
  }

  if (capture_embedded_scene_seed_draw) {
    static uint32_t embedded_scene_seed_output_capture_count = 0;
    if (embedded_scene_seed_output_capture_count < 2) {
      const uint32_t capture_index =
          ++embedded_scene_seed_output_capture_count;
      const char* const output_path =
          capture_index == 1 ? "rex_scene_seed_after_draw_1_fp16.bin"
                             : "rex_scene_seed_after_draw_2_fp16.bin";
      const bool captured = render_target_cache_->CaptureEmbeddedColorTarget(
          capture_index == 1 ? "scene_seed_after_draw_1"
                             : "scene_seed_after_draw_2",
          output_path);
      std::fprintf(stderr,
                   "REX_EMBEDDED_SCENE_SEED_OUTPUT_CAPTURE ordinal=%u "
                   "result=%u path=%s\n",
                   capture_index, captured ? 1u : 0u, output_path);
      std::fflush(stderr);
    }
  }

  // Pair the selected shader sample with the exact texture-signature draw used
  // by the active-texture diagnostic above. The generic six-draw scene window
  // also contains earlier variants whose texture samples are intentionally
  // zero, so it cannot prove shader-visible equivalence on its own.
  if (capture_embedded_scene_shaded_textures) {
    static bool embedded_scene_shaded_sample_output_captured = false;
    if (!embedded_scene_shaded_sample_output_captured) {
      embedded_scene_shaded_sample_output_captured = true;
      constexpr char kSceneShadedSampleOutputPath[] =
          "rex_scene_shaded_sample_output_fp16.bin";
      const bool captured = render_target_cache_->CaptureEmbeddedColorTarget(
          "scene_shaded_sample_output", kSceneShadedSampleOutputPath);
      std::fprintf(
          stderr,
          "REX_EMBEDDED_SCENE_SHADED_SAMPLE_OUTPUT result=%u "
          "ps=0x%016llX used_textures=0x%08X scale=%ux%u path=%s\n",
          captured ? 1u : 0u,
          static_cast<unsigned long long>(pixel_shader->ucode_data_hash()),
          used_texture_mask, render_target_cache_->GetDrawScaleX(),
          render_target_cache_->GetDrawScaleY(),
          kSceneShadedSampleOutputPath);
      std::fflush(stderr);
    }
  }

  if (capture_embedded_mixed_scale_native_draw) {
    static bool embedded_mixed_scale_native_post_draw_captured = false;
    if (!embedded_mixed_scale_native_post_draw_captured) {
      embedded_mixed_scale_native_post_draw_captured = true;
      constexpr char kMixedScaleNativePostDrawPath[] =
          "rex_mixed_scale_native_after_draw_fp16.bin";
      const bool captured = render_target_cache_->CaptureEmbeddedColorTarget(
          "mixed_scale_native_after_draw", kMixedScaleNativePostDrawPath);
      std::fprintf(
          stderr,
          "REX_EMBEDDED_MIXED_SCALE_POST_CAPTURE class=native result=%u "
          "path=%s\n",
          captured ? 1u : 0u, kMixedScaleNativePostDrawPath);
      std::fflush(stderr);
    }
  }
  if (capture_embedded_mixed_scale_scaled_draw) {
    static bool embedded_mixed_scale_scaled_post_draw_captured = false;
    if (!embedded_mixed_scale_scaled_post_draw_captured) {
      embedded_mixed_scale_scaled_post_draw_captured = true;
      constexpr char kMixedScaleScaledPostDrawPath[] =
          "rex_mixed_scale_scaled_after_draw_fp16.bin";
      const bool captured = render_target_cache_->CaptureEmbeddedColorTarget(
          "mixed_scale_scaled_after_draw", kMixedScaleScaledPostDrawPath);
      std::fprintf(
          stderr,
          "REX_EMBEDDED_MIXED_SCALE_POST_CAPTURE class=scaled result=%u "
          "path=%s\n",
          captured ? 1u : 0u, kMixedScaleScaledPostDrawPath);
      std::fflush(stderr);
    }
  }

  if (capture_embedded_simple_title_composition_draw) {
    static bool embedded_simple_title_output_captured = false;
    if (!embedded_simple_title_output_captured) {
      embedded_simple_title_output_captured = true;
      constexpr char kSimpleTitleOutputPath[] =
          "rex_simple_title_composition_after_draw_fp16.bin";
      const bool captured = render_target_cache_->CaptureEmbeddedColorTarget(
          "simple_title_composition_after_draw", kSimpleTitleOutputPath);
      std::fprintf(
          stderr,
          "REX_EMBEDDED_SIMPLE_TITLE_OUTPUT result=%u scale=%ux%u path=%s\n",
          captured ? 1u : 0u, render_target_cache_->GetDrawScaleX(),
          render_target_cache_->GetDrawScaleY(), kSimpleTitleOutputPath);
      std::fflush(stderr);
    }
  }

  if (capture_embedded_scene_chain_draw) {
    static uint32_t embedded_scene_chain_capture_count = 0;
    if (embedded_scene_chain_capture_count < 6) {
      const uint32_t capture_index = ++embedded_scene_chain_capture_count;
      char output_path[64];
      std::snprintf(output_path, sizeof(output_path),
                    "rex_scene_chain_after_draw_%u_fp16.bin", capture_index);
      const bool captured = render_target_cache_->CaptureEmbeddedColorTarget(
          "scaled_hdr_scene_chain", output_path);
      std::fprintf(
          stderr,
          "REX_EMBEDDED_SCENE_CHAIN_CAPTURE ordinal=%u result=%u "
          "ps=0x%016llX used_textures=0x%08X path=%s\n",
          capture_index, captured ? 1u : 0u,
          static_cast<unsigned long long>(pixel_shader->ucode_data_hash()),
          used_texture_mask, output_path);
      std::fflush(stderr);
    }
  }

  // The first-six capture isolated the beginning of this target's ownership
  // chain, but the static title state diverges later, before its resolve. Read
  // back only summaries for the bounded remaining interval so the first
  // differing writer can be identified without producing up to a gigabyte of
  // redundant FP16 dumps. This is launch-time opt-in and mode zero never calls
  // the synchronous diagnostic path.
  if (summarize_embedded_scene_chain_late_draw) {
    static uint32_t embedded_scene_chain_late_summary_ordinal = 0;
    const uint32_t summary_ordinal =
        ++embedded_scene_chain_late_summary_ordinal;
    if (summary_ordinal > 6 && summary_ordinal <= 32) {
      const bool summarized =
          render_target_cache_->CaptureEmbeddedColorTarget(
              "scaled_hdr_scene_chain_late", nullptr);
      std::fprintf(
          stderr,
          "REX_EMBEDDED_SCENE_CHAIN_LATE_SUMMARY ordinal=%u result=%u "
          "ps=0x%016llX used_textures=0x%08X color_mask=0x%08X "
          "depth_control=0x%08X scale=%ux%u\n",
          summary_ordinal, summarized ? 1u : 0u,
          static_cast<unsigned long long>(pixel_shader->ucode_data_hash()),
          used_texture_mask, normalized_color_mask,
          normalized_depth_control.value,
          render_target_cache_->GetDrawScaleX(),
          render_target_cache_->GetDrawScaleY());
      std::fflush(stderr);
    }
  }

  // Preserve four bounded image checkpoints across the already summarized
  // 32-draw chain so native and scaled runs can compare sparse writer-family
  // boundaries. This is opt-in, observes the original draws, and emits at most
  // four FP16 files.
  if (capture_embedded_scene_chain_checkpoint_draw) {
    static uint32_t embedded_scene_chain_checkpoint_ordinal = 0;
    const uint32_t checkpoint_ordinal =
        ++embedded_scene_chain_checkpoint_ordinal;
    if (checkpoint_ordinal == 8 || checkpoint_ordinal == 16 ||
        checkpoint_ordinal == 24 || checkpoint_ordinal == 32) {
      char output_path[80];
      std::snprintf(
          output_path, sizeof(output_path),
          "rex_scene_chain_checkpoint_after_draw_%u_fp16.bin",
          checkpoint_ordinal);
      const bool captured =
          render_target_cache_->CaptureEmbeddedColorTarget(
              "embedded_scene_chain_checkpoint", output_path);
      std::fprintf(
          stderr,
          "REX_EMBEDDED_SCENE_CHAIN_CHECKPOINT ordinal=%u result=%u "
          "ps=0x%016llX used_textures=0x%08X color_mask=0x%08X "
          "depth_control=0x%08X scale=%ux%u path=%s\n",
          checkpoint_ordinal, captured ? 1u : 0u,
          static_cast<unsigned long long>(pixel_shader->ucode_data_hash()),
          used_texture_mask, normalized_color_mask,
          normalized_depth_control.value,
          render_target_cache_->GetDrawScaleX(),
          render_target_cache_->GetDrawScaleY(), output_path);
      std::fflush(stderr);
    }
  }

  if (!kernel_state_ && IsEmbeddedFrameDiagnosticsEnabled()) {
    ++embedded_frame_frontier.submitted_draws;
    embedded_frame_frontier.submitted_vertices +=
        primitive_processing_result.host_draw_vertex_count;
    embedded_frame_frontier.pixel_shader_draws += pixel_shader ? 1 : 0;
    embedded_frame_frontier.memexport_draws += memexport_used ? 1 : 0;
    embedded_frame_frontier.Mix(vertex_shader->ucode_data_hash());
    embedded_frame_frontier.Mix(pixel_shader ? pixel_shader->ucode_data_hash() : 0);
    embedded_frame_frontier.Mix(regs[XE_GPU_REG_RB_SURFACE_INFO]);
    embedded_frame_frontier.Mix(regs[XE_GPU_REG_RB_COLOR_INFO]);
    embedded_frame_frontier.Mix(regs[XE_GPU_REG_RB_COLOR_MASK]);
    embedded_frame_frontier.Mix(uint32_t(primitive_type));
    embedded_frame_frontier.Mix(primitive_processing_result.host_draw_vertex_count);

    if (IsCurrentEmbeddedGameplayCaptureFrame()) {
      EmbeddedFrameFrontier::DrawState state;
      state.vertex_shader = vertex_shader->ucode_data_hash();
      state.pixel_shader = pixel_shader ? pixel_shader->ucode_data_hash() : 0;
      state.first_draw_ordinal = embedded_frame_frontier.submitted_draws;
      state.last_draw_ordinal = embedded_frame_frontier.submitted_draws;
      state.primitive_type = uint32_t(primitive_type);
      state.index_count = index_count;
      state.guest_draw_vertex_count =
          primitive_processing_result.guest_draw_vertex_count;
      state.host_draw_vertex_count =
          primitive_processing_result.host_draw_vertex_count;
      state.index_buffer_type =
          uint32_t(primitive_processing_result.index_buffer_type);
      state.guest_index_base = primitive_processing_result.guest_index_base;
      state.host_index_format =
          uint32_t(primitive_processing_result.host_index_format);
      state.used_texture_mask = used_texture_mask;
      state.draw_scale_x = draw_resolution_scale_x;
      state.draw_scale_y = draw_resolution_scale_y;
      state.bound_render_target_bits = bound_depth_and_color_render_target_bits;
      for (uint32_t i = 0; i < 1 + xenos::kMaxColorRenderTargets; ++i) {
        state.bound_render_target_formats |=
            uint64_t(bound_depth_and_color_render_target_formats[i] & 0xFF) << (i * 8);
      }
      uint32_t textures_remaining = used_texture_mask;
      uint32_t texture_index;
      state.first_texture_resource_hash = UINT64_C(1469598103934665603);
      const auto mix_texture_resource = [&state](uint64_t value) {
        state.first_texture_resource_hash ^= value;
        state.first_texture_resource_hash *= UINT64_C(1099511628211);
      };
      while (rex::bit_scan_forward(textures_remaining, &texture_index)) {
        textures_remaining &= ~(uint32_t(1) << texture_index);
        const xenos::xe_gpu_texture_fetch_t fetch = regs.GetTextureFetch(texture_index);
        mix_texture_resource(texture_index);
        mix_texture_resource(fetch.dword_0);
        mix_texture_resource(fetch.dword_1);
        mix_texture_resource(fetch.dword_2);
        mix_texture_resource(fetch.dword_3);
        mix_texture_resource(fetch.dword_4);
        mix_texture_resource(fetch.dword_5);
        if (state.texture_fetch_count < rex::countof(state.texture_fetch_indices)) {
          const uint32_t fetch_slot = state.texture_fetch_count++;
          state.texture_fetch_indices[fetch_slot] = texture_index;
          state.texture_fetch_words[fetch_slot][0] = fetch.dword_0;
          state.texture_fetch_words[fetch_slot][1] = fetch.dword_1;
          state.texture_fetch_words[fetch_slot][2] = fetch.dword_2;
          state.texture_fetch_words[fetch_slot][3] = fetch.dword_3;
          state.texture_fetch_words[fetch_slot][4] = fetch.dword_4;
          state.texture_fetch_words[fetch_slot][5] = fetch.dword_5;
          D3D12TextureCache::ActiveTextureDiagnostic diagnostic;
          EmbeddedFrameFrontier::DrawState::TextureState& texture_state =
              state.texture_states[fetch_slot];
          texture_state.valid = texture_cache_->GetActiveTextureDiagnostic(
                                    texture_index, diagnostic)
                                    ? 1u
                                    : 0u;
          if (texture_state.valid) {
            texture_state.guest_base = diagnostic.guest_base;
            texture_state.guest_size = diagnostic.guest_size;
            texture_state.guest_width = diagnostic.guest_width;
            texture_state.guest_height = diagnostic.guest_height;
            texture_state.guest_depth_or_array_size =
                diagnostic.guest_depth_or_array_size;
            texture_state.guest_format = diagnostic.guest_format;
            texture_state.guest_dimension = diagnostic.guest_dimension;
            texture_state.guest_tiled = diagnostic.guest_tiled;
            texture_state.scaled_resolve = diagnostic.scaled_resolve;
            texture_state.outdated_mask = diagnostic.outdated_mask;
            texture_state.descriptor_index = diagnostic.descriptor_index;
            texture_state.descriptor_index_signed =
                diagnostic.descriptor_index_signed;
            texture_state.resource_identity = diagnostic.resource_identity;
            texture_state.resource_width = diagnostic.resource_width;
            texture_state.resource_height = diagnostic.resource_height;
            texture_state.resource_depth_or_array_size =
                diagnostic.resource_depth_or_array_size;
            texture_state.resource_mip_levels =
                diagnostic.resource_mip_levels;
            texture_state.resource_format = diagnostic.resource_format;
          }
        }
        const uint32_t format = uint32_t(fetch.format);
        state.texture_format_mask |= uint64_t(1) << (format & 63);
        if (fetch.format == xenos::TextureFormat::k_24_8 ||
            fetch.format == xenos::TextureFormat::k_24_8_FLOAT) {
          state.depth_texture_mask |= uint32_t(1) << texture_index;
          if (!state.first_depth_texture_base) {
            state.first_depth_texture_base = fetch.base_address << 12;
            state.first_depth_texture_format = format;
          }
        }
        if (texture_cache_->IsActiveTextureResolutionScaled(texture_index)) {
          state.scaled_texture_mask |= uint32_t(1) << texture_index;
        }
      }
      state.surface_info = regs[XE_GPU_REG_RB_SURFACE_INFO];
      for (uint32_t i = 0; i < xenos::kMaxColorRenderTargets; ++i) {
        state.color_info[i] = regs[reg::RB_COLOR_INFO::rt_register_indices[i]];
        state.blend_control[i] = regs[reg::RB_BLENDCONTROL::rt_register_indices[i]];
        if (((normalized_color_mask >> (i * 4)) & 0xF) &&
            (state.blend_control[i] & 0x1FFF1FFF) != 0x00010001) {
          state.blend_candidate_mask |= uint32_t(1) << i;
        }
      }
      state.first_vertex_resource_hash = UINT64_C(1469598103934665603);
      const auto mix_vertex_resource = [&state](uint64_t value) {
        state.first_vertex_resource_hash ^= value;
        state.first_vertex_resource_hash *= UINT64_C(1099511628211);
      };
      mix_vertex_resource(state.index_buffer_type);
      mix_vertex_resource(state.guest_index_base);
      mix_vertex_resource(state.host_index_format);
      for (uint32_t fetch_word = 0;
           fetch_word < rex::countof(constant_map_vertex.vertex_fetch_bitmap);
           ++fetch_word) {
        uint32_t fetch_bits =
            constant_map_vertex.vertex_fetch_bitmap[fetch_word];
        uint32_t fetch_bit;
        while (rex::bit_scan_forward(fetch_bits, &fetch_bit)) {
          fetch_bits &= ~(uint32_t(1) << fetch_bit);
          const uint32_t fetch_index = fetch_word * 32 + fetch_bit;
          const xenos::xe_gpu_vertex_fetch_t fetch =
              regs.GetVertexFetch(fetch_index);
          mix_vertex_resource(fetch_index);
          mix_vertex_resource(fetch.dword_0);
          mix_vertex_resource(fetch.dword_1);
          if (state.vertex_fetch_count <
              rex::countof(state.vertex_fetch_indices)) {
            const uint32_t fetch_slot = state.vertex_fetch_count++;
            state.vertex_fetch_indices[fetch_slot] = fetch_index;
            state.vertex_fetch_words[fetch_slot][0] = fetch.dword_0;
            state.vertex_fetch_words[fetch_slot][1] = fetch.dword_1;
          }
        }
      }
      state.color_mask = regs[XE_GPU_REG_RB_COLOR_MASK];
      state.normalized_color_mask = normalized_color_mask;
      state.color_control = regs[XE_GPU_REG_RB_COLORCONTROL];
      state.depth_info = regs[XE_GPU_REG_RB_DEPTH_INFO];
      state.depth_control = regs[XE_GPU_REG_RB_DEPTHCONTROL];
      state.normalized_depth_control = normalized_depth_control.value;
      state.stencil_ref_mask = regs[XE_GPU_REG_RB_STENCILREFMASK];
      state.stencil_ref_mask_back = regs[XE_GPU_REG_RB_STENCILREFMASK_BF];
      state.mode_control = regs[XE_GPU_REG_RB_MODECONTROL];
      state.raster_control = regs[XE_GPU_REG_PA_SU_SC_MODE_CNTL];
      state.screen_scissor_tl = regs[XE_GPU_REG_PA_SC_SCREEN_SCISSOR_TL];
      state.screen_scissor_br = regs[XE_GPU_REG_PA_SC_SCREEN_SCISSOR_BR];
      state.window_offset = regs[XE_GPU_REG_PA_SC_WINDOW_OFFSET];
      state.window_scissor_tl = regs[XE_GPU_REG_PA_SC_WINDOW_SCISSOR_TL];
      state.window_scissor_br = regs[XE_GPU_REG_PA_SC_WINDOW_SCISSOR_BR];
      state.pixel_shader_flags =
          (pixel_shader && pixel_shader->writes_depth() ? 1u : 0u) |
          (pixel_shader && pixel_shader->kills_pixels() ? 2u : 0u);

      uint64_t signature = UINT64_C(1469598103934665603);
      const auto mix = [&signature](uint64_t value) {
        signature ^= value;
        signature *= UINT64_C(1099511628211);
      };
      mix(state.vertex_shader);
      mix(state.pixel_shader);
      mix(state.bound_render_target_formats);
      mix(state.texture_format_mask);
      mix(state.primitive_type);
      mix(state.used_texture_mask);
      mix(state.depth_texture_mask);
      mix(state.bound_render_target_bits);
      mix(state.surface_info);
      for (uint32_t i = 0; i < xenos::kMaxColorRenderTargets; ++i) {
        mix(state.color_info[i]);
        mix(state.blend_control[i]);
      }
      mix(state.color_mask);
      mix(state.normalized_color_mask);
      mix(state.color_control);
      mix(state.depth_info);
      mix(state.depth_control);
      mix(state.normalized_depth_control);
      mix(state.stencil_ref_mask);
      mix(state.stencil_ref_mask_back);
      mix(state.mode_control);
      mix(state.raster_control);
      mix(state.draw_scale_x);
      mix(state.draw_scale_y);
      mix(state.scaled_texture_mask);
      mix(state.pixel_shader_flags);
      state.signature = signature;
      const bool capture_new_resource_set =
          embedded_frame_frontier.RecordDrawState(state);
      if (capture_new_resource_set &&
          REXCVAR_GET(embedded_scaled_texture_readback_capture) &&
          state.texture_fetch_count) {
        texture_cache_->CaptureActiveTextureReadbackDiagnostics(
            state.texture_fetch_indices, state.texture_fetch_count, true,
            state.first_draw_ordinal);
      }
    }
  }

  if (memexport_used) {
    // Make sure this memexporting draw is ordered with other work using shared
    // memory as a UAV.
    // TODO(Triang3l): Find some PM4 command that can be used for indication of
    // when memexports should be awaited?
    shared_memory_->MarkUAVWritesCommitNeeded();
    // Invalidate textures in memexported memory and watch for changes.
    if (!memexport_ranges_.empty()) {
      for (const draw_util::MemExportRange& memexport_range : memexport_ranges_) {
        shared_memory_->RangeWrittenByGpu(memexport_range.base_address_dwords << 2,
                                          memexport_range.size_bytes);
      }
    } else {
      // Stream constants can be invalid or dynamic, so exact destinations may
      // be unknown. Keep invalidation conservative in this case.
      shared_memory_->RangeWrittenByGpu(0, SharedMemory::kBufferSize);
    }
    if (IsReadbackMemexportEnabled(REXCVAR_GET(d3d12_readback_memexport)) &&
        !memexport_ranges_.empty()) {
      uint32_t memexport_total_size = 0;
      for (const draw_util::MemExportRange& memexport_range : memexport_ranges_) {
        memexport_total_size += memexport_range.size_bytes;
      }
      if (memexport_total_size != 0) {
        if (REXCVAR_GET(readback_memexport_fast)) {
          IssueDraw_MemexportReadbackFastPath(memexport_total_size);
        } else {
          IssueDraw_MemexportReadbackFullPath(memexport_total_size);
        }
      }
    }
  }

  if (embedded_prompt_background_draw) {
    static bool prompt_rtv_readback_requested = false;
    if (!prompt_rtv_readback_requested) {
      prompt_rtv_readback_requested = true;
      const bool readback_ok = render_target_cache_->CaptureEmbeddedColorTarget(
          "press_start_background", "rex_prompt_color_target_fp16.bin");
      if (embedded_prompt_occlusion_query_index != UINT32_MAX) {
        if (!readback_ok) {
          AwaitAllQueueOperationsCompletion();
        }
        const uint64_t samples =
            occlusion_query_readback_mapping_
                ? occlusion_query_readback_mapping_[embedded_prompt_occlusion_query_index]
                : UINT64_MAX;
        std::fprintf(stderr,
                     "REX_EMBEDDED_PROMPT_OCCLUSION result=%s query=%u samples=%llu\n",
                     occlusion_query_readback_mapping_ ? "ok" : "unmapped",
                     embedded_prompt_occlusion_query_index,
                     static_cast<unsigned long long>(samples));
        std::fflush(stderr);
      }
    }
  }

  // A real ZPD report at guest END must still await and combine every host
  // segment. Submit a native-scale segment early only in the opt-in experiment
  // so the GPU can overlap its work with later guest command processing.
  // Native scale is required here: splitting scaled segments would change the
  // current per-segment sample normalization by extra rounding.
  if (zpd_submit_after_draws_ && active_occlusion_query_.valid &&
      active_occlusion_query_.scale_area == 1) {
    if (active_occlusion_query_.draws < zpd_submit_after_draws_) {
      ++active_occlusion_query_.draws;
    }
    zpd_early_max_segment_draws_ =
        std::max(zpd_early_max_segment_draws_, active_occlusion_query_.draws);
    if (active_occlusion_query_.draws >= zpd_submit_after_draws_ &&
        !scratch_buffer_used_ && CanEndSubmissionImmediately()) {
      // EndSubmission closes and resolves this segment, retaining the logical
      // query. IssueDraw opens the next real segment before its next draw.
      if (EndSubmission(false) && !zpd_early_submission_logged_) {
        zpd_early_submission_logged_ = true;
        std::fprintf(stderr,
                     "REX_ZPD_EARLY_SUBMISSION active=1 after_draws=%u\n",
                     zpd_submit_after_draws_);
        std::fflush(stderr);
      }
    }
  }
  // Let preceding raster work reach the GPU while the CP prepares later work.
  // This opt-in experiment never splits a logical query, waits for a pipeline,
  // changes a guest report, or closes the frame. EndSubmission retains the
  // existing barriers, allocator lifetime, queue signal and cache invalidation.
  if (guest_draw_submit_limit_ &&
      submission_guest_draws_ >= guest_draw_submit_limit_ &&
      submission_open_ && !logical_occlusion_query_.valid &&
      !active_occlusion_query_.valid && !scratch_buffer_used_ &&
      CanEndSubmissionImmediately()) {
    const uint32_t draws = submission_guest_draws_;
    if (EndSubmission(false)) {
      const uint64_t count = ++guest_draw_submit_count_;
      // Activation evidence for the experiment, at most 65 small records.
      if (count == 1 || (count <= 65536 && !(count % 1024))) {
        std::fprintf(stderr,
                     "REX_DRAW_BATCH_SUBMISSION count=%llu limit=%u draws=%u submission=%llu\n",
                     static_cast<unsigned long long>(count), guest_draw_submit_limit_,
                     draws, static_cast<unsigned long long>(submission_current_ - 1));
      }
    }
  }
  return true;
}

bool D3D12CommandProcessor::IssueDraw_MemexportReadbackFullPath(uint32_t total_size) {
  if (!total_size || memexport_ranges_.empty()) {
    return true;
  }

  ID3D12Resource* readback_buffer = RequestReadbackBuffer(total_size);
  if (!readback_buffer) {
    return true;
  }

  shared_memory_->UseAsCopySource();
  SubmitBarriers();
  ID3D12Resource* shared_memory_buffer = shared_memory_->GetBuffer();
  uint32_t readback_buffer_offset = 0;
  for (const draw_util::MemExportRange& memexport_range : memexport_ranges_) {
    deferred_command_list_.D3DCopyBufferRegion(
        readback_buffer, readback_buffer_offset, shared_memory_buffer,
        memexport_range.base_address_dwords << 2, memexport_range.size_bytes);
    readback_buffer_offset += memexport_range.size_bytes;
  }

  if (!AwaitAllQueueOperationsCompletion()) {
    return true;
  }

  D3D12_RANGE readback_range = {};
  readback_range.Begin = 0;
  readback_range.End = total_size;
  void* readback_mapping = nullptr;
  if (FAILED(readback_buffer->Map(0, &readback_range, &readback_mapping))) {
    return true;
  }

  const uint8_t* readback_bytes = reinterpret_cast<const uint8_t*>(readback_mapping);
  SettleUploadCopies();
  for (const draw_util::MemExportRange& memexport_range : memexport_ranges_) {
    // GPU results mirrored into guest memory: not a CPU write.
    auto host_write = memory_->GuardGpuMirrorWrite(
        memexport_range.base_address_dwords << 2, memexport_range.size_bytes);
    std::memcpy(memory_->TranslatePhysical(memexport_range.base_address_dwords << 2),
                readback_bytes, memexport_range.size_bytes);
    readback_bytes += memexport_range.size_bytes;
  }

  D3D12_RANGE readback_write_range = {};
  readback_buffer->Unmap(0, &readback_write_range);
  return true;
}

bool D3D12CommandProcessor::IssueDraw_MemexportReadbackFastPath(uint32_t total_size) {
  if (!total_size || memexport_ranges_.empty()) {
    return true;
  }

  const uint64_t readback_key =
      MakeMemexportReadbackKey(memexport_ranges_.front().base_address_dwords, total_size);
  ReadbackBuffer& readback = memexport_readback_buffers_[readback_key];
  readback.last_used_frame = frame_current_;

  auto ensure_readback_slot = [&](uint32_t index, uint32_t size) -> bool {
    if (readback.buffers[index] && readback.mapped_data[index] && size <= readback.sizes[index]) {
      return true;
    }

    const ui::d3d12::D3D12Provider& provider = GetD3D12Provider();
    ID3D12Device* device = provider.GetDevice();
    D3D12_RESOURCE_DESC buffer_desc;
    ui::d3d12::util::FillBufferResourceDesc(buffer_desc, size, D3D12_RESOURCE_FLAG_NONE);
    ID3D12Resource* buffer = nullptr;
    if (FAILED(device->CreateCommittedResource(
            &ui::d3d12::util::kHeapPropertiesReadback, provider.GetHeapFlagCreateNotZeroed(),
            &buffer_desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&buffer)))) {
      return false;
    }

    D3D12_RANGE read_range = {0, size};
    void* mapped_data = nullptr;
    if (FAILED(buffer->Map(0, &read_range, &mapped_data))) {
      buffer->Release();
      return false;
    }

    if (readback.buffers[index]) {
      if (!AwaitAllQueueOperationsCompletion()) {
        buffer->Unmap(0, nullptr);
        buffer->Release();
        return false;
      }
      if (readback.mapped_data[index]) {
        readback.buffers[index]->Unmap(0, nullptr);
      }
      readback.buffers[index]->Release();
    }

    readback.buffers[index] = buffer;
    readback.mapped_data[index] = mapped_data;
    readback.sizes[index] = size;
    readback.submission_written[index] = 0;
    readback.written_size[index] = 0;
    return true;
  };

  const uint32_t write_index = readback.current_index;
  const uint32_t read_index = 1 - write_index;
  const uint32_t readback_size = AlignReadbackBufferSize(total_size);
  if (!ensure_readback_slot(write_index, readback_size)) {
    return IssueDraw_MemexportReadbackFullPath(total_size);
  }

  shared_memory_->UseAsCopySource();
  SubmitBarriers();
  ID3D12Resource* shared_memory_buffer = shared_memory_->GetBuffer();
  uint32_t readback_offset = 0;
  for (const draw_util::MemExportRange& memexport_range : memexport_ranges_) {
    deferred_command_list_.D3DCopyBufferRegion(
        readback.buffers[write_index], readback_offset, shared_memory_buffer,
        memexport_range.base_address_dwords << 2, memexport_range.size_bytes);
    readback_offset += memexport_range.size_bytes;
  }
  readback.submission_written[write_index] = submission_current_;
  readback.written_size[write_index] = total_size;

  CheckSubmissionFence(0);
  bool previous_slot_ready = readback.buffers[read_index] && readback.mapped_data[read_index] &&
                             total_size <= readback.sizes[read_index] &&
                             total_size <= readback.written_size[read_index] &&
                             readback.submission_written[read_index] &&
                             readback.submission_written[read_index] <= submission_completed_;
  if (!previous_slot_ready) {
    IssueDraw_MemexportReadbackFullPath(total_size);
    readback.current_index = read_index;
    return true;
  }

  const uint8_t* readback_bytes = static_cast<const uint8_t*>(readback.mapped_data[read_index]);
  SettleUploadCopies();
  for (const draw_util::MemExportRange& memexport_range : memexport_ranges_) {
    // GPU results mirrored into guest memory: not a CPU write.
    auto host_write = memory_->GuardGpuMirrorWrite(
        memexport_range.base_address_dwords << 2, memexport_range.size_bytes);
    std::memcpy(memory_->TranslatePhysical(memexport_range.base_address_dwords << 2),
                readback_bytes, memexport_range.size_bytes);
    readback_bytes += memexport_range.size_bytes;
  }
  readback.current_index = read_index;
  return true;
}

void D3D12CommandProcessor::InitializeTrace() {
  CommandProcessor::InitializeTrace();

  if (!BeginSubmission(false)) {
    return;
  }
  bool render_target_cache_submitted = render_target_cache_->InitializeTraceSubmitDownloads();
  bool shared_memory_submitted = shared_memory_->InitializeTraceSubmitDownloads();
  if (!render_target_cache_submitted && !shared_memory_submitted) {
    return;
  }
  AwaitAllQueueOperationsCompletion();
  if (render_target_cache_submitted) {
    render_target_cache_->InitializeTraceCompleteDownloads();
  }
  if (shared_memory_submitted) {
    shared_memory_->InitializeTraceCompleteDownloads();
  }
}

void D3D12CommandProcessor::OnPredicatedPacketSkipped(uint32_t opcode) {
  if (!embedded_frame_dump_file) return;
  std::fprintf(embedded_frame_dump_file, "P skipped opcode=%02X bin_select=%016llX bin_mask=%016llX\n",
               opcode, static_cast<unsigned long long>(bin_select_),
               static_cast<unsigned long long>(bin_mask_));
}

bool D3D12CommandProcessor::IssueCopy() {
#if XE_GPU_FINE_GRAINED_DRAW_SCOPES
  SCOPE_profile_cpu_f("gpu");
#endif  // XE_GPU_FINE_GRAINED_DRAW_SCOPES
  if (!BeginSubmission(true)) {
    return false;
  }
  if (embedded_frame_dump_file) EmbeddedFrameDumpCopy(*register_file_);
  if (temporal_aa_enabled_) TemporalAaCopy();
  ReadbackResolveMode readback_mode = GetReadbackResolveMode(REXCVAR_GET(d3d12_readback_resolve));
  if (readback_mode == ReadbackResolveMode::kDisabled) {
    const char* embedded_resolve_source_capture_label = nullptr;
    const char* embedded_resolve_source_capture_path = nullptr;
    const char* embedded_resolve_edram_capture_path = nullptr;
    const char* embedded_resolve_scaled_capture_path = nullptr;
    bool arm_scene_color_texture_readback = false;
    if (!kernel_state_ && IsEmbeddedGameplayCaptureEnabled()) {
      // Pair the existing complex-frame swap readback with the authoritative
      // host render target immediately before it is packed into Xenos EDRAM.
      // This is intentionally one-shot and selects the same class of frame as
      // the swap capture so it cannot become per-frame synchronous tracing.
      static bool embedded_level_host_rtv_capture_attempted = false;
      const RegisterFile& capture_regs = *register_file_;
      const reg::RB_COPY_CONTROL capture_control =
          capture_regs.Get<reg::RB_COPY_CONTROL>();
      const reg::RB_COPY_DEST_INFO capture_dest_info =
          capture_regs.Get<reg::RB_COPY_DEST_INFO>();
      const reg::RB_COPY_DEST_PITCH capture_dest_pitch =
          capture_regs.Get<reg::RB_COPY_DEST_PITCH>();
      const reg::RB_SURFACE_INFO capture_surface_info =
          capture_regs.Get<reg::RB_SURFACE_INFO>();
      bool capture_source_is_hdr = false;
      uint32_t capture_source_color_info = 0;
      if (capture_control.copy_src_select < xenos::kMaxColorRenderTargets) {
        const reg::RB_COLOR_INFO source_color_info =
            capture_regs.Get<reg::RB_COLOR_INFO>(
                reg::RB_COLOR_INFO::rt_register_indices[
                    capture_control.copy_src_select]);
        capture_source_color_info = source_color_info.value;
        capture_source_is_hdr =
            source_color_info.color_format ==
            xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT;
      }
      const bool capture_complex_final_resolve =
          embedded_frame_frontier.draw_packets >=
              REXCVAR_GET(embedded_resolve_source_capture_min_draw_packets) &&
          embedded_frame_frontier.pixel_shader_draws >=
              REXCVAR_GET(embedded_resolve_source_capture_min_pixel_draws) &&
          embedded_frame_frontier.successful_resolves >=
              REXCVAR_GET(embedded_resolve_source_capture_min_prior_resolves) &&
          capture_control.copy_command == xenos::CopyCommand::kConvert &&
          capture_source_is_hdr &&
          capture_dest_info.copy_dest_format ==
              xenos::ColorFormat::k_2_10_10_10 &&
          capture_surface_info.surface_pitch == 1280 &&
          capture_dest_pitch.copy_dest_pitch == 1280 &&
          capture_dest_pitch.copy_dest_height == 720;
      const bool capture_schedule_allows =
          IsCurrentEmbeddedGameplayCaptureFrame();
      const uint32_t capture_target_destination =
          REXCVAR_GET(embedded_resolve_source_capture_destination);
      const uint32_t capture_current_destination =
          capture_regs[XE_GPU_REG_RB_COPY_DEST_BASE];
      const bool capture_targeted_resolve =
          capture_target_destination &&
          capture_current_destination == capture_target_destination;
      const bool capture_scene_color_resolve =
          REXCVAR_GET(embedded_resolve_source_capture_scene_color) &&
          embedded_frame_frontier.successful_resolves >= 8 &&
          capture_control.value == 0x00100040 &&
          capture_source_color_info == 0x000C0300 &&
          capture_dest_info.value == 0x003C0D01 &&
          capture_dest_pitch.value == 0x02D00500 &&
          capture_surface_info.value == 0x14010500;
      const bool capture_first_scene_feedback_boundary =
          REXCVAR_GET(
              embedded_resolve_boundary_capture_first_scene_feedback) &&
          embedded_resolve_boundary_capture_policy::
              IsFirstSceneFeedbackResolve(
                  {capture_control.value, capture_source_color_info,
                   capture_dest_info.value, capture_dest_pitch.value,
                   capture_surface_info.value});
      // The guest allocator moves this destination between otherwise
      // identical runs, so identify the first full-frame color resolve by the
      // complete Xenos register signature rather than by a guest address.
      // This is the earliest full-size color feedback boundary immediately
      // preceding the depth and scene-color resolves in the captured title
      // frame. The one-shot gate below prevents later identical resolves from
      // adding recurring readback overhead.
      const bool capture_first_full_frame_resolve =
          REXCVAR_GET(embedded_resolve_source_capture_first_full_frame) &&
          embedded_frame_frontier.successful_resolves >= 8 &&
          capture_control.value == 0x00100140 &&
          capture_source_color_info == 0x00030300 &&
          capture_dest_info.value == 0x01000302 &&
          capture_dest_pitch.value == 0x02D00500 &&
          capture_surface_info.value == 0x14010500;
      if (!embedded_level_host_rtv_capture_attempted &&
          capture_schedule_allows &&
          (capture_targeted_resolve || capture_scene_color_resolve ||
           capture_first_full_frame_resolve ||
           capture_first_scene_feedback_boundary ||
           (!capture_target_destination &&
            !REXCVAR_GET(embedded_resolve_source_capture_scene_color) &&
            !REXCVAR_GET(
                embedded_resolve_boundary_capture_first_scene_feedback) &&
            !REXCVAR_GET(
                embedded_resolve_source_capture_first_full_frame) &&
            capture_complex_final_resolve))) {
        embedded_level_host_rtv_capture_attempted = true;
        embedded_resolve_source_capture_label =
            capture_first_scene_feedback_boundary
            ? "first_scene_feedback_resolve_boundary"
            : capture_first_full_frame_resolve
            ? "first_full_frame_pre_edram_exact_resolve_source"
            : (capture_targeted_resolve || capture_scene_color_resolve)
            ? "targeted_pre_edram_exact_resolve_source"
            : "complex_frame_pre_edram_exact_resolve_source";
        embedded_resolve_source_capture_path =
            capture_first_scene_feedback_boundary
            ? "rex_scene_feedback_resolve_source_fp16.bin"
            : capture_first_full_frame_resolve
            ? "rex_first_full_resolve_source_fp16.bin"
            : (capture_targeted_resolve || capture_scene_color_resolve)
            ? "rex_targeted_resolve_source_fp16.bin"
            : "rex_level_host_rtv_fp16.bin";
        if (capture_first_scene_feedback_boundary) {
          embedded_resolve_edram_capture_path =
              "rex_scene_feedback_edram_after_dump.bin";
          embedded_resolve_scaled_capture_path =
              "rex_scene_feedback_scaled_after_copy.bin";
        }
        arm_scene_color_texture_readback =
            capture_scene_color_resolve || capture_first_full_frame_resolve ||
            capture_first_scene_feedback_boundary;
        std::fprintf(
            stderr,
            "REX_EMBEDDED_PRE_EDRAM_CAPTURE result=requested draw_packets=%llu "
            "pixel_draws=%llu prior_resolves=%llu surface=0x%08X "
            "source_color=0x%08X control=0x%08X dest_info=0x%08X "
            "dest_pitch=0x%08X target=0x%08X current=0x%08X "
            "boundary=%u\n",
            static_cast<unsigned long long>(
                embedded_frame_frontier.draw_packets),
            static_cast<unsigned long long>(
                embedded_frame_frontier.pixel_shader_draws),
            static_cast<unsigned long long>(
                embedded_frame_frontier.successful_resolves),
            capture_surface_info.value, capture_source_color_info,
            capture_control.value, capture_dest_info.value,
            capture_dest_pitch.value, capture_target_destination,
            capture_current_destination,
            capture_first_scene_feedback_boundary ? 1u : 0u);
        std::fflush(stderr);
      }
    }
    uint32_t written_address = 0;
    uint32_t written_length = 0;
    bool written_scaled = false;
    const embedded_scene_resolve_capture_policy::Context scene_context{
        embedded_completed_swap_ordinal + 1, embedded_frame_frontier.resolve_attempts + 1,
        embedded_frame_frontier.submitted_draws};
    const bool scene_metadata = kGpuDiagnostics && !kernel_state_ &&
        REXCVAR_GET(embedded_camera_scene_capture) &&
        REXCVAR_GET(embedded_camera_draw_capture) &&
        embedded_scene_resolve_budget.Observe(scene_context, IsCurrentEmbeddedGameplayCaptureFrame());
    const bool resolved = render_target_cache_->Resolve(
        *memory_, *shared_memory_, *texture_cache_, written_address,
        written_length, embedded_resolve_source_capture_label,
        embedded_resolve_source_capture_path, &written_scaled,
        embedded_resolve_edram_capture_path,
        embedded_resolve_scaled_capture_path, scene_metadata ? &scene_context : nullptr);
    if (resolved && written_length && arm_scene_color_texture_readback) {
      texture_cache_->ArmTextureReadbackDiagnostic(written_address,
                                                   written_length);
    }
    if (scene_metadata) {
      const RegisterFile& regs = *register_file_;
      const auto control = regs.Get<reg::RB_COPY_CONTROL>();
      uint32_t color = 0;
      if (control.copy_src_select < xenos::kMaxColorRenderTargets)
        color = regs[reg::RB_COLOR_INFO::rt_register_indices[control.copy_src_select]];
      const auto selected = embedded_scene_resolve_budget.SelectCopy({
          scene_context, scene_metadata, resolved, written_scaled,
          render_target_cache_->GetDrawScaleX(), render_target_cache_->GetDrawScaleY(),
          control.value, regs[XE_GPU_REG_RB_SURFACE_INFO], color, regs[XE_GPU_REG_RB_DEPTH_INFO],
          regs[XE_GPU_REG_RB_COPY_DEST_INFO], regs[XE_GPU_REG_RB_COPY_DEST_PITCH],
          regs[XE_GPU_REG_RB_COPY_DEST_BASE], written_address, written_length});
      char path[144] = "none";
      bool queued = false;
      const uint64_t submission = GetCurrentSubmission();
      if (selected.copy) {
        std::snprintf(path, sizeof(path), "rex_scene_resolve_%llu_frame_%llu_kind_%u.bin",
            static_cast<unsigned long long>(scene_context.ordinal),
            static_cast<unsigned long long>(scene_context.frame), uint32_t(selected.kind));
        queued = shared_memory_->QueueSceneResolveReadback(
            written_address, written_length, path, scene_context.last_draw);
      }
      // Every selected resolve has chronological identity, even without a copy
      // or after a failed resolve. Completion and layout are separate evidence.
      std::fprintf(stderr,
          "REX_SCENE_RESOLVE frame=%llu resolve=%llu last_draw=%llu succeeded=%u "
          "capture=%u kind=%u queued=%u submission=%llu address=%08X bytes=%u scaled=%u "
          "control=%08X surface=%08X color=%08X depth=%08X dest_info=%08X dest_pitch=%08X dest_base=%08X "
          "window_scissor=%08X,%08X window_offset=%08X path=%s\n",
          static_cast<unsigned long long>(scene_context.frame),
          static_cast<unsigned long long>(scene_context.ordinal),
          static_cast<unsigned long long>(scene_context.last_draw), resolved ? 1u : 0u,
          selected.copy, uint32_t(selected.kind), queued ? 1u : 0u,
          static_cast<unsigned long long>(submission), written_address, written_length, written_scaled ? 1u : 0u,
          control.value, regs[XE_GPU_REG_RB_SURFACE_INFO], color, regs[XE_GPU_REG_RB_DEPTH_INFO],
          regs[XE_GPU_REG_RB_COPY_DEST_INFO], regs[XE_GPU_REG_RB_COPY_DEST_PITCH], regs[XE_GPU_REG_RB_COPY_DEST_BASE],
          regs[XE_GPU_REG_PA_SC_WINDOW_SCISSOR_TL], regs[XE_GPU_REG_PA_SC_WINDOW_SCISSOR_BR],
          regs[XE_GPU_REG_PA_SC_WINDOW_OFFSET], path);
      std::fflush(stderr);
    }
    if (!kernel_state_ && REXCVAR_GET(embedded_temporal_depth_resolve_capture)) {
      const RegisterFile& regs = *register_file_;
      const uint64_t frame = embedded_completed_swap_ordinal + 1;
      const uint64_t resolve_ordinal = embedded_frame_frontier.resolve_attempts + 1;
      const uint32_t capture = embedded_depth_resolve_capture_state_.Select({
          frame, resolve_ordinal, IsCurrentEmbeddedGameplayCaptureFrame(),
          resolved, written_scaled, render_target_cache_->GetDrawScaleX(),
          render_target_cache_->GetDrawScaleY(), regs[XE_GPU_REG_RB_COPY_CONTROL],
          regs[XE_GPU_REG_RB_SURFACE_INFO], regs[XE_GPU_REG_RB_DEPTH_INFO],
          regs[XE_GPU_REG_RB_COPY_DEST_INFO], regs[XE_GPU_REG_RB_COPY_DEST_PITCH],
          regs[XE_GPU_REG_RB_COPY_DEST_BASE], written_address, written_length});
      if (capture) {
        char path[128];
        std::snprintf(path, sizeof(path), "rex_temporal_depth_resolve_%u_frame_%llu.bin",
                      capture, static_cast<unsigned long long>(frame));
        // The real resolve has already queued its UAV writes. The existing
        // exact-range copy commits those writes, preserves buffer state and
        // retains its resource until the actual submission fence completes.
        // Never close/reset the command list or copy CPU guest memory here.
        const uint64_t last_draw = embedded_frame_frontier.submitted_draws;
        const bool queued = shared_memory_->QueueTextureSourceReadback(
            written_address, written_length, path, last_draw);
        std::fprintf(stderr,
            "REX_TEMPORAL_DEPTH_RESOLVE frame=%llu resolve=%llu capture=%u "
            "last_draw=%llu queued=%u address=0x%08X bytes=%u "
            "control=0x%08X surface=0x%08X depth=0x%08X "
            "dest_info=0x%08X dest_pitch=0x%08X dest_base=0x%08X "
            "window_scissor=%08X,%08X window_offset=%08X path=%s "
            "scope=post_resolve_packed_range_not_complete_camera_depth\n",
            static_cast<unsigned long long>(frame),
            static_cast<unsigned long long>(resolve_ordinal), capture,
            static_cast<unsigned long long>(last_draw), queued ? 1u : 0u,
            written_address, written_length, regs[XE_GPU_REG_RB_COPY_CONTROL],
            regs[XE_GPU_REG_RB_SURFACE_INFO], regs[XE_GPU_REG_RB_DEPTH_INFO],
            regs[XE_GPU_REG_RB_COPY_DEST_INFO], regs[XE_GPU_REG_RB_COPY_DEST_PITCH],
            regs[XE_GPU_REG_RB_COPY_DEST_BASE], regs[XE_GPU_REG_PA_SC_WINDOW_SCISSOR_TL],
            regs[XE_GPU_REG_PA_SC_WINDOW_SCISSOR_BR], regs[XE_GPU_REG_PA_SC_WINDOW_OFFSET], path);
        std::fflush(stderr);
      }
    }
    if (!kernel_state_ && IsEmbeddedFrameDiagnosticsEnabled()) {
      ++embedded_frame_frontier.resolve_attempts;
      if (resolved) {
        ++embedded_frame_frontier.successful_resolves;
        embedded_frame_frontier.resolved_bytes += written_length;
        embedded_frame_frontier.last_resolve_destination =
            (*register_file_)[XE_GPU_REG_RB_COPY_DEST_BASE];
        embedded_frame_frontier.RecordResolve(
            embedded_frame_frontier.last_resolve_destination, written_length);
      }
      static uint64_t embedded_resolve_ordinal = 0;
      static uint64_t embedded_resolve_failure_ordinal = 0;
      static std::array<uint32_t, 64> embedded_resolve_destinations{};
      static size_t embedded_resolve_destination_count = 0;
      const uint64_t ordinal = ++embedded_resolve_ordinal;
      const uint64_t failure_ordinal =
          resolved ? 0 : ++embedded_resolve_failure_ordinal;
      const RegisterFile& regs = *register_file_;
      const uint32_t destination_base = regs[XE_GPU_REG_RB_COPY_DEST_BASE];
      if (IsCurrentEmbeddedGameplayCaptureFrame()) {
        EmbeddedFrameFrontier::ResolveState state;
        const reg::RB_COPY_CONTROL control =
            regs.Get<reg::RB_COPY_CONTROL>();
        state.control = control.value;
        state.destination_base = destination_base;
        state.destination_info = regs[XE_GPU_REG_RB_COPY_DEST_INFO];
        state.destination_pitch = regs[XE_GPU_REG_RB_COPY_DEST_PITCH];
        state.surface_info = regs[XE_GPU_REG_RB_SURFACE_INFO];
        if (control.copy_src_select < xenos::kMaxColorRenderTargets) {
          state.source_color_info =
              regs[reg::RB_COLOR_INFO::rt_register_indices[
                  control.copy_src_select]];
        }
        state.depth_info = regs[XE_GPU_REG_RB_DEPTH_INFO];
        state.written_address = resolved ? written_address : 0;
        state.written_length = resolved ? written_length : 0;
        state.written_scaled = resolved && written_length && written_scaled;
        embedded_frame_frontier.RecordResolveState(state);
      }
      bool first_destination = false;
      if (resolved && written_length) {
        size_t destination_index = 0;
        while (destination_index < embedded_resolve_destination_count &&
               embedded_resolve_destinations[destination_index] != destination_base) {
          ++destination_index;
        }
        if (destination_index == embedded_resolve_destination_count &&
            embedded_resolve_destination_count < embedded_resolve_destinations.size()) {
          embedded_resolve_destinations[embedded_resolve_destination_count++] = destination_base;
          first_destination = true;
        }
      }
      const bool trace_success =
          resolved && written_length &&
          (ordinal <= 64 || !(ordinal & (ordinal - 1)) || first_destination);
      const bool trace_failure =
          failure_ordinal && (failure_ordinal <= 64 || !(failure_ordinal & 511));
      if (trace_success || trace_failure) {
        const reg::RB_COPY_CONTROL control = regs.Get<reg::RB_COPY_CONTROL>();
        const reg::RB_COPY_DEST_INFO dest_info = regs.Get<reg::RB_COPY_DEST_INFO>();
        const reg::RB_COPY_DEST_PITCH dest_pitch = regs.Get<reg::RB_COPY_DEST_PITCH>();
        std::fprintf(
            stderr,
            "REX_EMBEDDED_EDRAM_RESOLVE ordinal=%llu result=%u first_dest=%u "
            "written=0x%08X bytes=%u "
            "dest_base=0x%08X pitch=%u height=%u format=%u command=%u source=%u "
            "clear_color=%u clear_depth=%u window_scissor=%08X,%08X window_offset=%08X\n",
            static_cast<unsigned long long>(ordinal), resolved ? 1u : 0u,
            first_destination ? 1u : 0u, written_address, written_length, destination_base,
            dest_pitch.copy_dest_pitch,
            dest_pitch.copy_dest_height, uint32_t(dest_info.copy_dest_format),
            uint32_t(control.copy_command), control.copy_src_select, control.color_clear_enable,
            control.depth_clear_enable, regs[XE_GPU_REG_PA_SC_WINDOW_SCISSOR_TL],
            regs[XE_GPU_REG_PA_SC_WINDOW_SCISSOR_BR], regs[XE_GPU_REG_PA_SC_WINDOW_OFFSET]);
        std::fflush(stderr);
      }
    }
    return resolved;
  }
  return IssueCopy_ReadbackResolvePath();
}

bool D3D12CommandProcessor::IssueCopy_ReadbackResolvePath() {
  uint32_t written_address, written_length;
  bool is_scaled = false;
  if (!render_target_cache_->Resolve(*memory_, *shared_memory_, *texture_cache_, written_address,
                                     written_length, nullptr, nullptr,
                                     &is_scaled)) {
    return false;
  }

  if (!written_length) {
    return true;
  }

  if (!memory_->TranslatePhysical(written_address)) {
    return true;
  }

  uint64_t resolve_key = MakeReadbackResolveKey(written_address, written_length);
  ReadbackBuffer& rb = readback_buffers_[resolve_key];
  rb.last_used_frame = frame_current_;

  uint32_t write_index = rb.current_index;
  uint32_t size = AlignReadbackBufferSize(written_length);

  if (size > rb.sizes[write_index]) {
    const ui::d3d12::D3D12Provider& provider = GetD3D12Provider();
    ID3D12Device* device = provider.GetDevice();
    D3D12_RESOURCE_DESC buffer_desc;
    ui::d3d12::util::FillBufferResourceDesc(buffer_desc, size, D3D12_RESOURCE_FLAG_NONE);
    ID3D12Resource* buffer = nullptr;
    if (FAILED(device->CreateCommittedResource(
            &ui::d3d12::util::kHeapPropertiesReadback, provider.GetHeapFlagCreateNotZeroed(),
            &buffer_desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&buffer)))) {
      REXGPU_ERROR("Failed to create a {} MB readback buffer", size >> 20);
      return true;
    }
    if (rb.buffers[write_index]) {
      if (rb.mapped_data[write_index]) {
        rb.buffers[write_index]->Unmap(0, nullptr);
        rb.mapped_data[write_index] = nullptr;
      }
      rb.buffers[write_index]->Release();
    }
    rb.buffers[write_index] = buffer;
    rb.sizes[write_index] = size;
    D3D12_RANGE read_range = {0, size};
    if (FAILED(buffer->Map(0, &read_range, &rb.mapped_data[write_index]))) {
      REXGPU_ERROR("Failed to persistently map resolve readback buffer");
      rb.mapped_data[write_index] = nullptr;
    }
  }

  if (!rb.buffers[write_index]) {
    return true;
  }

  if (is_scaled) {
    if (!resolve_downscale_pipeline_ || !resolve_downscale_root_signature_) {
      return true;
    }

    reg::RB_COPY_DEST_INFO copy_dest_info = register_file_->Get<reg::RB_COPY_DEST_INFO>();
    const FormatInfo* format_info = FormatInfo::Get(uint32_t(copy_dest_info.copy_dest_format));
    uint32_t bits_per_pixel = format_info->bits_per_pixel;
    if (bits_per_pixel != 8 && bits_per_pixel != 16 && bits_per_pixel != 32 &&
        bits_per_pixel != 64) {
      return true;
    }

    uint32_t pixel_size_log2;
    if (!rex::bit_scan_forward(bits_per_pixel >> 3, &pixel_size_log2)) {
      return true;
    }
    uint32_t tile_size_1x = 32 * 32 * (uint32_t(1) << pixel_size_log2);
    uint32_t tile_count = written_length / tile_size_1x;
    if (!tile_count) {
      return true;
    }

    uint32_t scaled_length = uint32_t(texture_cache_->GetCurrentScaledResolveRangeLengthScaled());
    uint64_t scaled_address = texture_cache_->GetCurrentScaledResolveRangeStartScaled();
    if (!scaled_length) {
      return true;
    }

    uint32_t downscale_buffer_size = AlignReadbackBufferSize(written_length);
    if (downscale_buffer_size > resolve_downscale_buffer_size_) {
      const ui::d3d12::D3D12Provider& provider = GetD3D12Provider();
      ID3D12Device* device = provider.GetDevice();
      D3D12_RESOURCE_DESC buffer_desc;
      ui::d3d12::util::FillBufferResourceDesc(buffer_desc, downscale_buffer_size,
                                              D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
      ID3D12Resource* buffer = nullptr;
      if (FAILED(device->CreateCommittedResource(
              &ui::d3d12::util::kHeapPropertiesDefault, provider.GetHeapFlagCreateNotZeroed(),
              &buffer_desc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
              IID_PPV_ARGS(&buffer)))) {
        REXGPU_ERROR("Failed to create a {} MB resolve downscale buffer",
                     downscale_buffer_size >> 20);
        return true;
      }
      if (resolve_downscale_buffer_) {
        resources_for_deletion_.emplace_back(GetCurrentSubmission(),
                                             resolve_downscale_buffer_.Detach());
      }
      resolve_downscale_buffer_.Attach(buffer);
      resolve_downscale_buffer_size_ = downscale_buffer_size;
    }

    if (!resolve_downscale_buffer_) {
      return true;
    }

    ID3D12Resource* scaled_resolve_buffer = texture_cache_->GetCurrentScaledResolveBufferResource();
    size_t scaled_resolve_buffer_index = texture_cache_->GetCurrentScaledResolveBufferIndexPublic();
    if (!scaled_resolve_buffer) {
      return true;
    }
    uint64_t scaled_buffer_base = uint64_t(scaled_resolve_buffer_index) << 30;
    if (scaled_address < scaled_buffer_base) {
      return true;
    }
    uint64_t source_offset = scaled_address - scaled_buffer_base;

    ui::d3d12::util::DescriptorCpuGpuHandlePair downscale_descriptors[2];
    if (!RequestOneUseSingleViewDescriptors(2, downscale_descriptors)) {
      return true;
    }

    const ui::d3d12::D3D12Provider& provider = GetD3D12Provider();
    ID3D12Device* device = provider.GetDevice();
    uint32_t aligned_scaled_length =
        rex::align(scaled_length, uint32_t(D3D12_RAW_UAV_SRV_BYTE_ALIGNMENT));
    ui::d3d12::util::CreateBufferRawSRV(device, downscale_descriptors[0].first,
                                        scaled_resolve_buffer, aligned_scaled_length,
                                        source_offset);
    uint32_t aligned_written_length =
        rex::align(written_length, uint32_t(D3D12_RAW_UAV_SRV_BYTE_ALIGNMENT));
    ui::d3d12::util::CreateBufferRawUAV(device, downscale_descriptors[1].first,
                                        resolve_downscale_buffer_.Get(), aligned_written_length, 0);

    PushUAVBarrier(scaled_resolve_buffer);
    texture_cache_->TransitionCurrentScaledResolveRange(
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    SubmitBarriers();

    SetExternalPipeline(resolve_downscale_pipeline_.Get());
    deferred_command_list_.D3DSetComputeRootSignature(resolve_downscale_root_signature_.Get());
    ResolveDownscaleConstants constants;
    constants.scale_x = texture_cache_->draw_resolution_scale_x();
    constants.scale_y = texture_cache_->draw_resolution_scale_y();
    constants.pixel_size_log2 = pixel_size_log2;
    constants.tile_count = tile_count;
    constants.half_pixel_offset = (REXCVAR_GET(readback_resolve_half_pixel_offset) &&
                                   (constants.scale_x > 1 || constants.scale_y > 1))
                                      ? 1u
                                      : 0u;
    deferred_command_list_.D3DSetComputeRoot32BitConstants(
        UINT(ResolveDownscaleRootParameter::kConstants), sizeof(constants) / sizeof(uint32_t),
        &constants, 0);
    deferred_command_list_.D3DSetComputeRootDescriptorTable(
        UINT(ResolveDownscaleRootParameter::kSource), downscale_descriptors[0].second);
    deferred_command_list_.D3DSetComputeRootDescriptorTable(
        UINT(ResolveDownscaleRootParameter::kDestination), downscale_descriptors[1].second);
    deferred_command_list_.D3DDispatch(tile_count, 1, 1);

    PushUAVBarrier(resolve_downscale_buffer_.Get());
    PushTransitionBarrier(resolve_downscale_buffer_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                          D3D12_RESOURCE_STATE_COPY_SOURCE);
    SubmitBarriers();
    deferred_command_list_.D3DCopyBufferRegion(rb.buffers[write_index], 0,
                                               resolve_downscale_buffer_.Get(), 0, written_length);
    PushTransitionBarrier(resolve_downscale_buffer_.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
                          D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    texture_cache_->TransitionCurrentScaledResolveRange(D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    SubmitBarriers();
  } else {
    shared_memory_->UseAsCopySource();
    SubmitBarriers();
    ID3D12Resource* shared_memory_buffer = shared_memory_->GetBuffer();
    deferred_command_list_.D3DCopyBufferRegion(rb.buffers[write_index], 0, shared_memory_buffer,
                                               written_address, written_length);
  }

  ReadbackResolveMode readback_mode = GetReadbackResolveMode(REXCVAR_GET(d3d12_readback_resolve));
  bool use_delayed_sync =
      readback_mode == ReadbackResolveMode::kFast || readback_mode == ReadbackResolveMode::kSome;
  uint32_t read_index = write_index;
  if (use_delayed_sync) {
    read_index = 1 - write_index;
  } else if (!AwaitAllQueueOperationsCompletion()) {
    return true;
  }

  bool is_cache_miss = false;
  if (use_delayed_sync && (!rb.buffers[read_index] || written_length > rb.sizes[read_index] ||
                           !rb.mapped_data[read_index])) {
    is_cache_miss = true;
    read_index = write_index;
    if (!AwaitAllQueueOperationsCompletion()) {
      return true;
    }
  }

  bool should_copy = (readback_mode == ReadbackResolveMode::kSome) ? is_cache_miss : true;
  if (should_copy && rb.buffers[read_index] && written_length <= rb.sizes[read_index] &&
      rb.mapped_data[read_index]) {
    uint8_t* destination = memory_->TranslatePhysical(written_address);
    if (destination) {
      SettleUploadCopies();
      // GPU results mirrored into guest memory: not a CPU write.
      auto host_write = memory_->GuardGpuMirrorWrite(written_address, written_length);
      std::memcpy(destination, static_cast<uint8_t*>(rb.mapped_data[read_index]), written_length);
    }
  }

  rb.current_index = 1 - rb.current_index;
  return true;
}

void D3D12CommandProcessor::CheckSubmissionFence(uint64_t await_submission) {
  const bool zpd_fence_detail =
      kGpuDiagnostics && zpd_fence_check_active_ && cp_cadence_.active;
  // Swap-interval diagnostics: time blocked on fences and full GPU syncs.
  const bool interval_fence = kGpuDiagnostics && swap_intervals_.active;
  const bool timed_fence = zpd_fence_detail || interval_fence;
  uint64_t zpd_event_wait_ticks = 0;
  if (await_submission >= submission_current_) {
    if (kGpuDiagnostics) swap_intervals_.AddFullSync();
    if (submission_open_) {
      EndSubmission(false);
    }
    // Ending an open submission should result in queue operations done directly
    // (like UpdateTileMappings) to be tracked within the scope of that
    // submission, but just in case of a failure, or queue operations being done
    // outside of a submission, await explicitly.
    if (queue_operations_done_since_submission_signal_) {
      UINT64 fence_value = ++queue_operations_since_submission_fence_last_;
      ID3D12CommandQueue* direct_queue = GetD3D12Provider().GetDirectQueue();
      if (SUCCEEDED(direct_queue->Signal(queue_operations_since_submission_fence_, fence_value)) &&
          SUCCEEDED(queue_operations_since_submission_fence_->SetEventOnCompletion(
              fence_value, fence_completion_event_))) {
        PROFILE_CMD_BUFFER_STALL();
        const uint64_t wait_begin =
            timed_fence ? rex::chrono::Clock::QueryHostTickCount() : 0;
        WaitForSingleObject(fence_completion_event_, INFINITE);
        if (timed_fence) {
          const uint64_t waited = rex::chrono::Clock::QueryHostTickCount() - wait_begin;
          if (zpd_fence_detail) zpd_event_wait_ticks += waited;
          if (interval_fence) swap_intervals_.AddFenceWait(waited);
        }
        queue_operations_done_since_submission_signal_ = false;
      } else {
        REXGPU_ERROR(
            "Failed to await an out-of-submission queue operation completion "
            "Direct3D 12 fence");
      }
    }
    // A submission won't be ended if it hasn't been started, or if ending
    // has failed - clamp the index.
    await_submission = submission_current_ - 1;
  }

  uint64_t submission_completed_before = submission_completed_;
  submission_completed_ = submission_fence_->GetCompletedValue();
  if (submission_completed_ < await_submission) {
    if (SUCCEEDED(
            submission_fence_->SetEventOnCompletion(await_submission, fence_completion_event_))) {
      PROFILE_CMD_BUFFER_STALL();
      const uint64_t wait_begin =
          timed_fence ? rex::chrono::Clock::QueryHostTickCount() : 0;
      WaitForSingleObject(fence_completion_event_, INFINITE);
      if (timed_fence) {
        const uint64_t waited = rex::chrono::Clock::QueryHostTickCount() - wait_begin;
        if (zpd_fence_detail) zpd_event_wait_ticks += waited;
        if (interval_fence) swap_intervals_.AddFenceWait(waited);
      }
      submission_completed_ = submission_fence_->GetCompletedValue();
    }
  }
  if (submission_completed_ < await_submission) {
    REXGPU_ERROR("Failed to await a submission completion Direct3D 12 fence");
  }
  if (zpd_fence_detail) {
    cp_cadence_.OcclusionWorkTime(3, zpd_event_wait_ticks);
  }
  if (submission_completed_ <= submission_completed_before) {
    // Not updated - no need to reclaim or download things.
    return;
  }

  const uint64_t zpd_reclaim_begin =
      zpd_fence_detail ? rex::chrono::Clock::QueryHostTickCount() : 0;

  // Reclaim command allocators.
  while (command_allocator_submitted_first_) {
    if (command_allocator_submitted_first_->last_usage_submission > submission_completed_) {
      break;
    }
    if (command_allocator_writable_last_) {
      command_allocator_writable_last_->next = command_allocator_submitted_first_;
    } else {
      command_allocator_writable_first_ = command_allocator_submitted_first_;
    }
    command_allocator_writable_last_ = command_allocator_submitted_first_;
    command_allocator_submitted_first_ = command_allocator_submitted_first_->next;
    command_allocator_writable_last_->next = nullptr;
  }
  if (!command_allocator_submitted_first_) {
    command_allocator_submitted_last_ = nullptr;
  }

  // Release single-use bindless descriptors.
  while (!view_bindless_one_use_descriptors_.empty()) {
    if (view_bindless_one_use_descriptors_.front().second > submission_completed_) {
      break;
    }
    ReleaseViewBindlessDescriptorImmediately(view_bindless_one_use_descriptors_.front().first);
    view_bindless_one_use_descriptors_.pop_front();
  }

  // Delete transient resources marked for deletion.
  while (!resources_for_deletion_.empty()) {
    if (resources_for_deletion_.front().first > submission_completed_) {
      break;
    }
    resources_for_deletion_.front().second->Release();
    resources_for_deletion_.pop_front();
  }

  shared_memory_->CompletedSubmissionUpdated();

  render_target_cache_->CompletedSubmissionUpdated();

  primitive_processor_->CompletedSubmissionUpdated();

  texture_cache_->CompletedSubmissionUpdated(submission_completed_);
  if (zpd_fence_detail) {
    cp_cadence_.OcclusionWorkTime(
        4, rex::chrono::Clock::QueryHostTickCount() - zpd_reclaim_begin);
  }
}

void D3D12CommandProcessor::LogDeviceRemovalDiagnostics(ID3D12Device* device, HRESULT reason) {
  const char* reason_str = "Unknown";
  switch (reason) {
    case DXGI_ERROR_DEVICE_HUNG:
      reason_str = "DEVICE_HUNG (TDR - GPU command took too long)";
      break;
    case DXGI_ERROR_DEVICE_REMOVED:
      reason_str = "DEVICE_REMOVED (driver internal error or hot-unplug)";
      break;
    case DXGI_ERROR_DEVICE_RESET:
      reason_str = "DEVICE_RESET (bad GPU command)";
      break;
    case DXGI_ERROR_DRIVER_INTERNAL_ERROR:
      reason_str = "DRIVER_INTERNAL_ERROR";
      break;
    case DXGI_ERROR_INVALID_CALL:
      reason_str = "INVALID_CALL";
      break;
  }
  REXGPU_ERROR("D3D12 device removed: HRESULT 0x{:08X} - {}", static_cast<unsigned>(reason),
               reason_str);

  Microsoft::WRL::ComPtr<ID3D12DeviceRemovedExtendedData> dred;
  if (FAILED(device->QueryInterface(IID_PPV_ARGS(&dred)))) {
    return;
  }

  D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT breadcrumbs = {};
  if (SUCCEEDED(dred->GetAutoBreadcrumbsOutput(&breadcrumbs))) {
    for (const D3D12_AUTO_BREADCRUMB_NODE* node = breadcrumbs.pHeadAutoBreadcrumbNode; node;
         node = node->pNext) {
      if (!node->pLastBreadcrumbValue || !node->pCommandHistory ||
          *node->pLastBreadcrumbValue == 0) {
        continue;
      }
      REXGPU_ERROR("DRED breadcrumb: completed {} of {} ops", *node->pLastBreadcrumbValue,
                   node->BreadcrumbCount);
      uint32_t last = std::min(*node->pLastBreadcrumbValue, node->BreadcrumbCount);
      uint32_t start = last > 3 ? last - 3 : 0;
      uint32_t end = std::min(last + 1, node->BreadcrumbCount);
      for (uint32_t i = start; i < end; i++) {
        REXGPU_ERROR("  [{}] op type {}{}", i, static_cast<int>(node->pCommandHistory[i]),
                     i == last ? " <-- FAULT" : "");
      }
    }
  }

  D3D12_DRED_PAGE_FAULT_OUTPUT page_fault = {};
  if (SUCCEEDED(dred->GetPageFaultAllocationOutput(&page_fault)) && page_fault.PageFaultVA != 0) {
    REXGPU_ERROR("DRED page fault at VA 0x{:016X}", page_fault.PageFaultVA);
  }
}

bool D3D12CommandProcessor::BeginSubmission(bool is_guest_command) {
#if XE_GPU_FINE_GRAINED_DRAW_SCOPES
  SCOPE_profile_cpu_f("gpu");
#endif  // XE_GPU_FINE_GRAINED_DRAW_SCOPES

  if (device_removed_) {
    return false;
  }

  bool is_opening_frame = is_guest_command && !frame_open_;
  if (submission_open_ && !is_opening_frame) {
    return true;
  }

  // Check if the device is still available.
  ID3D12Device* device = GetD3D12Provider().GetDevice();
  HRESULT device_removed_reason = device->GetDeviceRemovedReason();
  if (FAILED(device_removed_reason)) {
    device_removed_ = true;
    LogDeviceRemovalDiagnostics(device, device_removed_reason);
    if (graphics_system_) {
      graphics_system_->OnHostGpuLossFromAnyThread(device_removed_reason !=
                                                   DXGI_ERROR_DEVICE_REMOVED);
    }
    return false;
  }

  // Check the fence - needed for all kinds of submissions (to reclaim transient
  // resources early) and specifically for frames (not to queue too many), and
  // await the availability of the current frame.
  CheckSubmissionFence(is_opening_frame ? closed_frame_submissions_[frame_current_ % kQueueFrames]
                                        : 0);
  // TODO(Triang3l): If failed to await (completed submission < awaited frame
  // submission), do something like dropping the draw command that wanted to
  // open the frame.
  if (is_opening_frame) {
    // Update the completed frame index, also obtaining the actual completed
    // frame number (since the CPU may be actually less than 3 frames behind)
    // before reclaiming resources tracked with the frame number.
    frame_completed_ = std::max(frame_current_, uint64_t(kQueueFrames)) - kQueueFrames;
    for (uint64_t frame = frame_completed_ + 1; frame < frame_current_; ++frame) {
      if (closed_frame_submissions_[frame % kQueueFrames] > submission_completed_) {
        break;
      }
      frame_completed_ = frame;
    }
  }

  const bool submission_opened = !submission_open_;
  if (!submission_open_) {
    submission_open_ = true;

    // Start a new deferred command list - will submit it to the real one in the
    // end of the submission (when async pipeline creation requests are
    // fulfilled).
    deferred_command_list_.Reset();

    // Reset cached state of the command list.
    ff_viewport_update_needed_ = true;
    ff_scissor_update_needed_ = true;
    ff_blend_factor_update_needed_ = true;
    ff_stencil_ref_update_needed_ = true;
    viewport_cache_valid_ = false;
    current_guest_pipeline_ = nullptr;
    current_external_pipeline_ = nullptr;
    current_graphics_root_signature_ = nullptr;
    current_graphics_root_up_to_date_ = 0;
    if (bindless_resources_used_) {
      deferred_command_list_.SetDescriptorHeaps(view_bindless_heap_,
                                                sampler_bindless_heap_current_);
    } else {
      view_bindful_heap_current_ = nullptr;
      sampler_bindful_heap_current_ = nullptr;
    }
    primitive_topology_ = D3D_PRIMITIVE_TOPOLOGY_UNDEFINED;

    render_target_cache_->BeginSubmission();

    primitive_processor_->BeginSubmission();

    texture_cache_->BeginSubmission(submission_current_);
  }

  if (is_opening_frame) {
    frame_open_ = true;

    // Reset bindings that depend on the data stored in the pools.
    std::memset(current_float_constant_map_vertex_, 0, sizeof(current_float_constant_map_vertex_));
    std::memset(current_float_constant_map_pixel_, 0, sizeof(current_float_constant_map_pixel_));
    cbuffer_binding_system_.up_to_date = false;
    cbuffer_binding_float_vertex_.up_to_date = false;
    cbuffer_binding_float_pixel_.up_to_date = false;
    cbuffer_binding_bool_loop_.up_to_date = false;
    cbuffer_binding_fetch_.up_to_date = false;
    current_shared_memory_binding_is_uav_.reset();
    if (bindless_resources_used_) {
      cbuffer_binding_descriptor_indices_vertex_.up_to_date = false;
      cbuffer_binding_descriptor_indices_pixel_.up_to_date = false;
    } else {
      draw_view_bindful_heap_index_ = ui::d3d12::D3D12DescriptorHeapPool::kHeapIndexInvalid;
      draw_sampler_bindful_heap_index_ = ui::d3d12::D3D12DescriptorHeapPool::kHeapIndexInvalid;
      bindful_textures_written_vertex_ = false;
      bindful_textures_written_pixel_ = false;
      bindful_samplers_written_vertex_ = false;
      bindful_samplers_written_pixel_ = false;
    }

    // Reclaim pool pages - no need to do this every small submission since some
    // may be reused.
    constant_buffer_pool_->Reclaim(frame_completed_);
    if (!bindless_resources_used_) {
      view_bindful_heap_pool_->Reclaim(frame_completed_);
      sampler_bindful_heap_pool_->Reclaim(frame_completed_);
    }
    EvictOldReadbackBuffers(readback_buffers_);
    EvictOldReadbackBuffers(memexport_readback_buffers_);

    pix_capturing_ = pix_capture_requested_.exchange(false, std::memory_order_relaxed);
    if (pix_capturing_) {
      IDXGraphicsAnalysis* graphics_analysis = GetD3D12Provider().GetGraphicsAnalysis();
      if (graphics_analysis != nullptr) {
        graphics_analysis->BeginCapture();
      }
    }

    primitive_processor_->BeginFrame();

    texture_cache_->BeginFrame();
  }

#if REX_GPU_DIAGNOSTICS
  if (gpu_timing_) {
    GpuTimingBeginSubmission(submission_opened, is_opening_frame);
  }
#endif
  if (gpu_frame_meter_) {
    GpuFrameMeterBeginSubmission(submission_opened, is_opening_frame);
  }

  return true;
}

bool D3D12CommandProcessor::EndSubmission(bool is_swap) {
  const ui::d3d12::D3D12Provider& provider = GetD3D12Provider();

  // Make sure there is a command allocator to write commands to.
  if (submission_open_ && !command_allocator_writable_first_) {
    ID3D12CommandAllocator* command_allocator;
    if (FAILED(provider.GetDevice()->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                            IID_PPV_ARGS(&command_allocator)))) {
      REXGPU_ERROR("Failed to create a command allocator");
      // Try to submit later. Completely dropping the submission is not
      // permitted because resources would be left in an undefined state.
      return false;
    }
    command_allocator_writable_first_ = new CommandAllocator;
    command_allocator_writable_first_->command_allocator = command_allocator;
    command_allocator_writable_first_->last_usage_submission = 0;
    command_allocator_writable_first_->next = nullptr;
    command_allocator_writable_last_ = command_allocator_writable_first_;
  }

  bool is_closing_frame = is_swap && frame_open_;

  if (is_closing_frame) {
    texture_cache_->EndFrame();

    primitive_processor_->EndFrame();
  }

  if (submission_open_) {
    assert_false(scratch_buffer_used_);

    if (active_occlusion_query_.valid && occlusion_query_heap_ &&
        !CloseGuestOcclusionQuerySegment()) {
      REXGPU_ERROR(
          "D3D12CommandProcessor: Failed to preserve an active ZPD query "
          "segment while closing submission {}",
          submission_current_);
    }

    pipeline_cache_->EndSubmission();

    // Submit barriers now because resources with the queued barriers may be
    // destroyed between frames.
    SubmitBarriers();

#if REX_GPU_DIAGNOSTICS
    if (gpu_timing_) {
      GpuTimingEndSubmission();
    }
#endif
    if (gpu_frame_meter_) {
      GpuFrameMeterEndSubmission();
    }

    ID3D12CommandQueue* direct_queue = provider.GetDirectQueue();

    // Submit the deferred command list.
    // Only one deferred command list must be executed in the same
    // ExecuteCommandLists - the boundaries of ExecuteCommandLists are a full
    // UAV and aliasing barrier, and subsystems of the emulator assume it
    // happens between Xenia submissions.
    ID3D12CommandAllocator* command_allocator =
        command_allocator_writable_first_->command_allocator;
    const bool sampled_zpd_submission =
        kGpuDiagnostics && zpd_gpu_timestamp_pending_ &&
        zpd_gpu_timestamp_submission_ == submission_current_ &&
        cp_cadence_.active;
    // The sampled ZPD timing diagnostic measures this thread's submission
    // return, so it keeps the inline path (after earlier jobs, in order).
    const bool submit_async = async_submission_ && !sampled_zpd_submission;
    if (submit_async) {
      SubmissionJob job;
      job.allocator = command_allocator;
      job.fence_value = submission_current_;
      {
        std::lock_guard<std::mutex> lock(submission_mutex_);
        if (!submission_free_streams_.empty()) {
          job.stream = std::move(submission_free_streams_.back());
          submission_free_streams_.pop_back();
        }
      }
      job.stream.clear();
      deferred_command_list_.SwapStream(job.stream);
      job.upload_copy_ticket = shared_memory_->QueuedUploadCopies();
      {
        std::lock_guard<std::mutex> lock(submission_mutex_);
        submission_jobs_.push_back(std::move(job));
      }
      submission_work_cv_.notify_one();
    } else {
      DrainSubmissions();
      SettleUploadCopies();
      command_allocator->Reset();
      command_list_->Reset(command_allocator, nullptr);
      deferred_command_list_.Execute(command_list_, command_list_1_);
      command_list_->Close();
      ID3D12CommandList* execute_command_lists[] = {command_list_};
      direct_queue->ExecuteCommandLists(1, execute_command_lists);
    }
    if (sampled_zpd_submission) {
      LARGE_INTEGER submit_return;
      QueryPerformanceCounter(&submit_return);
      zpd_gpu_submit_return_qpc_ = submit_return.QuadPart;
    }
    command_allocator_writable_first_->last_usage_submission = submission_current_;
    if (command_allocator_submitted_last_) {
      command_allocator_submitted_last_->next = command_allocator_writable_first_;
    } else {
      command_allocator_submitted_first_ = command_allocator_writable_first_;
    }
    command_allocator_submitted_last_ = command_allocator_writable_first_;
    command_allocator_writable_first_ = command_allocator_writable_first_->next;
    command_allocator_submitted_last_->next = nullptr;
    if (!command_allocator_writable_first_) {
      command_allocator_writable_last_ = nullptr;
    }

    if (!submit_async) {
      direct_queue->Signal(submission_fence_, submission_current_);
    }
    ++submission_current_;
    if (sampled_zpd_submission && !zpd_gpu_calibration_valid_ &&
        zpd_gpu_qpc_frequency_) {
      // Once per sparse CP window, after the real submission and fence signal.
      // Calibration correlates the existing GPU timestamp with QPC; it does
      // not alter the submitted query or its completion predicate.
      LARGE_INTEGER calibration_begin, calibration_end;
      QueryPerformanceCounter(&calibration_begin);
      zpd_gpu_calibration_valid_ = SUCCEEDED(direct_queue->GetClockCalibration(
          &zpd_gpu_calibration_gpu_tick_, &zpd_gpu_calibration_cpu_tick_));
      QueryPerformanceCounter(&calibration_end);
      cp_cadence_.occlusion_calibration_ms +=
          double(calibration_end.QuadPart - calibration_begin.QuadPart) *
          1000.0 / double(zpd_gpu_qpc_frequency_);
    }

    submission_open_ = false;
    submission_guest_draws_ = 0;

    // Queue operations done directly (like UpdateTileMappings) will be awaited
    // alongside the last submission if needed.
    queue_operations_done_since_submission_signal_ = false;
  }

  if (is_closing_frame) {
#if REX_GPU_DIAGNOSTICS
    if (gpu_timing_) {
      GpuTimingCloseFrame();
    }
#endif
    if (gpu_frame_meter_) {
      GpuFrameMeterCloseFrame();
    }
    if (shared_memory_) {
      // Host writes that no request has published yet (for example audio
      // output or query results next to GPU data): once per frame.
      shared_memory_->PublishHostWrites();
    }
#if REX_GPU_DIAGNOSTICS
    if (shared_memory_) {
      shared_memory_->CoherencyAuditFrameEnd();
    }
    if (++vertex_slot_frames_ == 600) {
      const double frames = double(vertex_slot_frames_);
      std::fprintf(stderr,
                   "REX_VERTEX_SLOT_RESIDENCY frames=%llu skipped_per_frame=%.1f "
                   "revalidated_per_frame=%.2f changed_per_frame=%.1f revalidated=%llu\n",
                   static_cast<unsigned long long>(vertex_slot_frames_),
                   double(vertex_slot_skipped_) / frames,
                   double(vertex_slot_revalidated_) / frames,
                   double(vertex_slot_changed_) / frames,
                   static_cast<unsigned long long>(vertex_slot_revalidated_));
      vertex_slot_frames_ = 0;
      vertex_slot_skipped_ = 0;
      vertex_slot_revalidated_ = 0;
      vertex_slot_changed_ = 0;
    }
#endif
    if (REXCVAR_GET(clear_memory_page_state) && shared_memory_) {
      shared_memory_->SetSystemPageBlocksValidWithGpuDataWritten();
    }
    // Close the capture after submitting.
    if (pix_capturing_) {
      DrainSubmissions();
      IDXGraphicsAnalysis* graphics_analysis = provider.GetGraphicsAnalysis();
      if (graphics_analysis != nullptr) {
        graphics_analysis->EndCapture();
      }
      pix_capturing_ = false;
    }
    frame_open_ = false;
    // Submission already closed now, so minus 1.
    closed_frame_submissions_[(frame_current_++) % kQueueFrames] = submission_current_ - 1;

    if (cache_clear_requested_ && AwaitAllQueueOperationsCompletion()) {
      cache_clear_requested_ = false;

      ClearCommandAllocatorCache();

      ui::d3d12::util::ReleaseAndNull(scratch_buffer_);
      scratch_buffer_size_ = 0;

      if (bindless_resources_used_) {
        texture_cache_bindless_sampler_map_.clear();
        for (const auto& sampler_bindless_heap_overflowed : sampler_bindless_heaps_overflowed_) {
          sampler_bindless_heap_overflowed.first->Release();
        }
        sampler_bindless_heaps_overflowed_.clear();
        sampler_bindless_heap_allocated_ = 0;
      } else {
        sampler_bindful_heap_pool_->ClearCache();
        view_bindful_heap_pool_->ClearCache();
      }
      constant_buffer_pool_->ClearCache();

      texture_cache_->ClearCache();

      // Not clearing the root signatures as they're referenced by pipelines,
      // which are not destroyed.

      primitive_processor_->ClearCache();

      render_target_cache_->ClearCache();

      shared_memory_->ClearCache();
    }
  }

  return true;
}

void D3D12CommandProcessor::StartSubmissionThread() {
  if (submission_thread_.joinable()) {
    return;
  }
  submission_thread_stop_ = false;
  submission_thread_ = std::thread([this]() {
    SetThreadDescription(GetCurrentThread(), L"D3D12 Submission");
    SubmissionThreadMain();
  });
}

void D3D12CommandProcessor::StopSubmissionThread() {
  if (!submission_thread_.joinable()) {
    return;
  }
  DrainSubmissions();
  {
    std::lock_guard<std::mutex> lock(submission_mutex_);
    submission_thread_stop_ = true;
  }
  submission_work_cv_.notify_one();
  submission_thread_.join();
  submission_free_streams_.clear();
}

void D3D12CommandProcessor::SubmissionThreadMain() {
  ID3D12CommandQueue* direct_queue = GetD3D12Provider().GetDirectQueue();
  std::unique_lock<std::mutex> lock(submission_mutex_);
  while (true) {
    submission_work_cv_.wait(
        lock, [this]() { return submission_thread_stop_ || !submission_jobs_.empty(); });
    if (submission_jobs_.empty()) {
      break;
    }
    SubmissionJob job = std::move(submission_jobs_.front());
    submission_jobs_.pop_front();
    submission_worker_busy_ = true;
    lock.unlock();
    if (!job.allocator) {
      // Ordered task: everything queued before it has been executed and
      // signaled on the direct queue.
      if (job.task) {
        job.task();
      }
      lock.lock();
      submission_worker_busy_ = false;
      if (submission_jobs_.empty()) {
        submission_idle_cv_.notify_all();
      }
      continue;
    }
    // The allocator was last used by a completed submission (the command
    // processor reclaims allocators only after their fence value is reached).
    job.allocator->Reset();
    command_list_->Reset(job.allocator, nullptr);
    deferred_command_list_.ExecuteStream(job.stream.data(), job.stream.size(), command_list_,
                                         command_list_1_);
    command_list_->Close();
    // Copy stage: the upload pages this list copies from are written.
    shared_memory_->WaitForUploadCopies(job.upload_copy_ticket);
    ID3D12CommandList* execute_command_lists[] = {command_list_};
    direct_queue->ExecuteCommandLists(1, execute_command_lists);
    direct_queue->Signal(submission_fence_, job.fence_value);
    job.stream.clear();
    lock.lock();
    submission_free_streams_.push_back(std::move(job.stream));
    submission_worker_busy_ = false;
    if (submission_jobs_.empty()) {
      submission_idle_cv_.notify_all();
    }
  }
}

void D3D12CommandProcessor::DrainSubmissions() {
  if (!submission_thread_.joinable()) {
    return;
  }
  const uint64_t wait_begin = (kGpuDiagnostics && swap_intervals_.active)
                                  ? rex::chrono::Clock::QueryHostTickCount()
                                  : 0;
  {
    std::unique_lock<std::mutex> lock(submission_mutex_);
    submission_idle_cv_.wait(
        lock, [this]() { return submission_jobs_.empty() && !submission_worker_busy_; });
  }
  if (wait_begin) {
    swap_intervals_.AddFenceWait(rex::chrono::Clock::QueryHostTickCount() - wait_begin);
  }
}

void D3D12CommandProcessor::EnqueueSubmissionTask(std::function<void()> task) {
  if (!submission_thread_.joinable()) {
    task();
    return;
  }
  SubmissionJob job;
  job.task = std::move(task);
  {
    std::lock_guard<std::mutex> lock(submission_mutex_);
    submission_jobs_.push_back(std::move(job));
  }
  submission_work_cv_.notify_one();
}

bool D3D12CommandProcessor::CanEndSubmissionImmediately() const {
  return !submission_open_ || !pipeline_cache_->IsCreatingPipelines();
}

void D3D12CommandProcessor::ClearCommandAllocatorCache() {
  while (command_allocator_submitted_first_) {
    auto next = command_allocator_submitted_first_->next;
    command_allocator_submitted_first_->command_allocator->Release();
    delete command_allocator_submitted_first_;
    command_allocator_submitted_first_ = next;
  }
  command_allocator_submitted_last_ = nullptr;
  while (command_allocator_writable_first_) {
    auto next = command_allocator_writable_first_->next;
    command_allocator_writable_first_->command_allocator->Release();
    delete command_allocator_writable_first_;
    command_allocator_writable_first_ = next;
  }
  command_allocator_writable_last_ = nullptr;
}

void D3D12CommandProcessor::UpdateFixedFunctionState(
    const draw_util::ViewportInfo& viewport_info, const draw_util::Scissor& scissor,
    bool primitive_polygonal, reg::RB_DEPTHCONTROL normalized_depth_control) {
#if XE_GPU_FINE_GRAINED_DRAW_SCOPES
  SCOPE_profile_cpu_f("gpu");
#endif  // XE_GPU_FINE_GRAINED_DRAW_SCOPES

  // Viewport.
  D3D12_VIEWPORT viewport;
  viewport.TopLeftX = float(viewport_info.xy_offset[0]);
  viewport.TopLeftY = float(viewport_info.xy_offset[1]);
  viewport.Width = float(viewport_info.xy_extent[0]);
  viewport.Height = float(viewport_info.xy_extent[1]);
  viewport.MinDepth = viewport_info.z_min;
  viewport.MaxDepth = viewport_info.z_max;
  if (temporal_aa_jitter_draw_) {
    // Temporal AA: sub-pixel offset of the scene draws (host pixels).
    viewport.TopLeftX += temporal_aa_jitter_[0];
    viewport.TopLeftY += temporal_aa_jitter_[1];
  }
  SetViewport(viewport);

  // Scissor.
  D3D12_RECT scissor_rect;
  scissor_rect.left = LONG(scissor.offset[0]);
  scissor_rect.top = LONG(scissor.offset[1]);
  scissor_rect.right = LONG(scissor.offset[0] + scissor.extent[0]);
  scissor_rect.bottom = LONG(scissor.offset[1] + scissor.extent[1]);
  SetScissorRect(scissor_rect);

  if (render_target_cache_->GetPath() == RenderTargetCache::Path::kHostRenderTargets) {
    const RegisterFile& regs = *register_file_;

    // Blend factor.
    float blend_factor[] = {
        regs.Get<float>(XE_GPU_REG_RB_BLEND_RED),
        regs.Get<float>(XE_GPU_REG_RB_BLEND_GREEN),
        regs.Get<float>(XE_GPU_REG_RB_BLEND_BLUE),
        regs.Get<float>(XE_GPU_REG_RB_BLEND_ALPHA),
    };
    // std::memcmp instead of != so in case of NaN, every draw won't be
    // invalidating it.
    ff_blend_factor_update_needed_ |=
        std::memcmp(ff_blend_factor_, blend_factor, sizeof(float) * 4) != 0;
    if (ff_blend_factor_update_needed_) {
      std::memcpy(ff_blend_factor_, blend_factor, sizeof(float) * 4);
      deferred_command_list_.D3DOMSetBlendFactor(ff_blend_factor_);
      ff_blend_factor_update_needed_ = false;
    }

    // Stencil reference value. Per-face reference not supported by Direct3D 12,
    // choose the back face one only if drawing only back faces.
    Register stencil_ref_mask_reg;
    auto pa_su_sc_mode_cntl = regs.Get<reg::PA_SU_SC_MODE_CNTL>();
    if (primitive_polygonal && normalized_depth_control.backface_enable &&
        pa_su_sc_mode_cntl.cull_front && !pa_su_sc_mode_cntl.cull_back) {
      stencil_ref_mask_reg = XE_GPU_REG_RB_STENCILREFMASK_BF;
    } else {
      stencil_ref_mask_reg = XE_GPU_REG_RB_STENCILREFMASK;
    }
    uint32_t stencil_ref = regs.Get<reg::RB_STENCILREFMASK>(stencil_ref_mask_reg).stencilref;
    ff_stencil_ref_update_needed_ |= ff_stencil_ref_ != stencil_ref;
    if (ff_stencil_ref_update_needed_) {
      ff_stencil_ref_ = stencil_ref;
      deferred_command_list_.D3DOMSetStencilRef(ff_stencil_ref_);
      ff_stencil_ref_update_needed_ = false;
    }
  }
}

void D3D12CommandProcessor::UpdateSystemConstantValues(
    bool shared_memory_is_uav, bool primitive_polygonal, uint32_t line_loop_closing_index,
    xenos::Endian index_endian, const draw_util::ViewportInfo& viewport_info,
    uint32_t used_texture_mask, reg::RB_DEPTHCONTROL normalized_depth_control,
    uint32_t normalized_color_mask) {
#if XE_GPU_FINE_GRAINED_DRAW_SCOPES
  SCOPE_profile_cpu_f("gpu");
#endif  // XE_GPU_FINE_GRAINED_DRAW_SCOPES

  const RegisterFile& regs = *register_file_;
  auto pa_cl_clip_cntl = regs.Get<reg::PA_CL_CLIP_CNTL>();
  auto pa_cl_vte_cntl = regs.Get<reg::PA_CL_VTE_CNTL>();
  auto pa_su_sc_mode_cntl = regs.Get<reg::PA_SU_SC_MODE_CNTL>();
  auto rb_alpha_ref = regs.Get<float>(XE_GPU_REG_RB_ALPHA_REF);
  auto rb_colorcontrol = regs.Get<reg::RB_COLORCONTROL>();
  auto rb_depth_info = regs.Get<reg::RB_DEPTH_INFO>();
  auto rb_stencilrefmask = regs.Get<reg::RB_STENCILREFMASK>();
  auto rb_stencilrefmask_bf = regs.Get<reg::RB_STENCILREFMASK>(XE_GPU_REG_RB_STENCILREFMASK_BF);
  auto rb_surface_info = regs.Get<reg::RB_SURFACE_INFO>();
  auto sq_context_misc = regs.Get<reg::SQ_CONTEXT_MISC>();
  auto sq_program_cntl = regs.Get<reg::SQ_PROGRAM_CNTL>();
  auto vgt_draw_initiator = regs.Get<reg::VGT_DRAW_INITIATOR>();
  uint32_t vgt_indx_offset = regs.Get<reg::VGT_INDX_OFFSET>().indx_offset;
  uint32_t vgt_max_vtx_indx = regs.Get<reg::VGT_MAX_VTX_INDX>().max_indx;
  uint32_t vgt_min_vtx_indx = regs.Get<reg::VGT_MIN_VTX_INDX>().min_indx;

  bool edram_rov_used =
      render_target_cache_->GetPath() == RenderTargetCache::Path::kPixelShaderInterlock;
  uint32_t draw_resolution_scale_x = render_target_cache_->GetDrawScaleX();
  uint32_t draw_resolution_scale_y = render_target_cache_->GetDrawScaleY();

  // Get the color info register values for each render target. Also, for ROV,
  // exclude components that don't exist in the format from the write mask.
  // Don't exclude fully overlapping render targets, however - two render
  // targets with the same base address are used in the lighting pass of
  // 4D5307E6, for example, with the needed one picked with dynamic control
  // flow.
  reg::RB_COLOR_INFO color_infos[4];
  float rt_clamp[4][4];
  // Two UINT32_MAX if no components actually existing in the RT are written.
  uint32_t rt_keep_masks[4][2];
  for (uint32_t i = 0; i < 4; ++i) {
    auto color_info = regs.Get<reg::RB_COLOR_INFO>(reg::RB_COLOR_INFO::rt_register_indices[i]);
    color_infos[i] = color_info;
    if (edram_rov_used) {
      RenderTargetCache::GetPSIColorFormatInfo(
          color_info.color_format, (normalized_color_mask >> (i * 4)) & 0b1111, rt_clamp[i][0],
          rt_clamp[i][1], rt_clamp[i][2], rt_clamp[i][3], rt_keep_masks[i][0], rt_keep_masks[i][1]);
    }
  }

  // Disable depth and stencil if it aliases a color render target (for
  // instance, during the XBLA logo in 58410954, though depth writing is already
  // disabled there).
  bool depth_stencil_enabled =
      normalized_depth_control.stencil_enable || normalized_depth_control.z_enable;
  if (edram_rov_used && depth_stencil_enabled) {
    for (uint32_t i = 0; i < 4; ++i) {
      if (rb_depth_info.depth_base == color_infos[i].color_base &&
          (rt_keep_masks[i][0] != UINT32_MAX || rt_keep_masks[i][1] != UINT32_MAX)) {
        depth_stencil_enabled = false;
        break;
      }
    }
  }

  bool dirty = false;

  // Flags.
  uint32_t flags = 0;
  // Whether shared memory is an SRV or a UAV. Because a resource can't be in a
  // read-write (UAV) and a read-only (SRV, IBV) state at once, if any shader in
  // the pipeline uses memexport, the shared memory buffer must be a UAV.
  if (shared_memory_is_uav) {
    flags |= DxbcShaderTranslator::kSysFlag_SharedMemoryIsUAV;
  }
  // W0 division control.
  // http://www.x.org/docs/AMD/old/evergreen_3D_registers_v2.pdf
  // 8: VTX_XY_FMT = true: the incoming XY have already been multiplied by 1/W0.
  //               = false: multiply the X, Y coordinates by 1/W0.
  // 9: VTX_Z_FMT = true: the incoming Z has already been multiplied by 1/W0.
  //              = false: multiply the Z coordinate by 1/W0.
  // 10: VTX_W0_FMT = true: the incoming W0 is not 1/W0. Perform the reciprocal
  //                        to get 1/W0.
  if (pa_cl_vte_cntl.vtx_xy_fmt) {
    flags |= DxbcShaderTranslator::kSysFlag_XYDividedByW;
  }
  if (pa_cl_vte_cntl.vtx_z_fmt) {
    flags |= DxbcShaderTranslator::kSysFlag_ZDividedByW;
  }
  if (pa_cl_vte_cntl.vtx_w0_fmt) {
    flags |= DxbcShaderTranslator::kSysFlag_WNotReciprocal;
  }
  // Whether the primitive is polygonal and SV_IsFrontFace matters.
  if (primitive_polygonal) {
    flags |= DxbcShaderTranslator::kSysFlag_PrimitivePolygonal;
  }
  // Primitive type.
  if (draw_util::IsPrimitiveLine(regs)) {
    flags |= DxbcShaderTranslator::kSysFlag_PrimitiveLine;
  }
  // Depth format.
  if (rb_depth_info.depth_format == xenos::DepthRenderTargetFormat::kD24FS8) {
    flags |= DxbcShaderTranslator::kSysFlag_DepthFloat24;
  }
  // Alpha test.
  xenos::CompareFunction alpha_test_function = rb_colorcontrol.alpha_test_enable
                                                   ? rb_colorcontrol.alpha_func
                                                   : xenos::CompareFunction::kAlways;
  flags |= uint32_t(alpha_test_function) << DxbcShaderTranslator::kSysFlag_AlphaPassIfLess_Shift;
  // Gamma writing.
  if (!render_target_cache_->gamma_render_target_as_unorm16()) {
    for (uint32_t i = 0; i < 4; ++i) {
      if (color_infos[i].color_format == xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA) {
        flags |= DxbcShaderTranslator::kSysFlag_ConvertColor0ToGamma << i;
      }
    }
  }
  if (edram_rov_used && depth_stencil_enabled) {
    flags |= DxbcShaderTranslator::kSysFlag_ROVDepthStencil;
    if (normalized_depth_control.z_enable) {
      flags |= uint32_t(normalized_depth_control.zfunc)
               << DxbcShaderTranslator::kSysFlag_ROVDepthPassIfLess_Shift;
      if (normalized_depth_control.z_write_enable) {
        flags |= DxbcShaderTranslator::kSysFlag_ROVDepthWrite;
      }
    } else {
      // In case stencil is used without depth testing - always pass, and
      // don't modify the stored depth.
      flags |= DxbcShaderTranslator::kSysFlag_ROVDepthPassIfLess |
               DxbcShaderTranslator::kSysFlag_ROVDepthPassIfEqual |
               DxbcShaderTranslator::kSysFlag_ROVDepthPassIfGreater;
    }
    if (normalized_depth_control.stencil_enable) {
      flags |= DxbcShaderTranslator::kSysFlag_ROVStencilTest;
    }
    // Hint - if not applicable to the shader, will not have effect.
    if (alpha_test_function == xenos::CompareFunction::kAlways &&
        !rb_colorcontrol.alpha_to_mask_enable) {
      flags |= DxbcShaderTranslator::kSysFlag_ROVDepthStencilEarlyWrite;
    }
  }
  dirty |= system_constants_.flags != flags;
  system_constants_.flags = flags;

  // Tessellation factor range, plus 1.0 according to the images in
  // https://www.slideshare.net/blackdevilvikas/next-generation-graphics-programming-on-xbox-360
  float tessellation_factor_min = regs.Get<float>(XE_GPU_REG_VGT_HOS_MIN_TESS_LEVEL) + 1.0f;
  float tessellation_factor_max = regs.Get<float>(XE_GPU_REG_VGT_HOS_MAX_TESS_LEVEL) + 1.0f;
  dirty |= system_constants_.tessellation_factor_range_min != tessellation_factor_min;
  system_constants_.tessellation_factor_range_min = tessellation_factor_min;
  dirty |= system_constants_.tessellation_factor_range_max != tessellation_factor_max;
  system_constants_.tessellation_factor_range_max = tessellation_factor_max;

  // Line loop closing index (or 0 when drawing other primitives or using an
  // index buffer).
  dirty |= system_constants_.line_loop_closing_index != line_loop_closing_index;
  system_constants_.line_loop_closing_index = line_loop_closing_index;

  // Index or tessellation edge factor buffer endianness.
  dirty |= system_constants_.vertex_index_endian != index_endian;
  system_constants_.vertex_index_endian = index_endian;

  // Vertex index offset.
  dirty |= system_constants_.vertex_index_offset != vgt_indx_offset;
  system_constants_.vertex_index_offset = vgt_indx_offset;

  // Vertex index range.
  dirty |= system_constants_.vertex_index_min != vgt_min_vtx_indx;
  dirty |= system_constants_.vertex_index_max != vgt_max_vtx_indx;
  system_constants_.vertex_index_min = vgt_min_vtx_indx;
  system_constants_.vertex_index_max = vgt_max_vtx_indx;

  // User clip planes (UCP_ENA_#), when not CLIP_DISABLE.
  // The shader knows only the total count - tightly packing the user clip
  // planes that are actually used.
  if (!pa_cl_clip_cntl.clip_disable) {
    float* user_clip_plane_write_ptr = system_constants_.user_clip_planes[0];
    uint32_t user_clip_planes_remaining = pa_cl_clip_cntl.ucp_ena;
    uint32_t user_clip_plane_index;
    while (rex::bit_scan_forward(user_clip_planes_remaining, &user_clip_plane_index)) {
      user_clip_planes_remaining &= ~(UINT32_C(1) << user_clip_plane_index);
      const void* user_clip_plane_regs =
          &regs[XE_GPU_REG_PA_CL_UCP_0_X + user_clip_plane_index * 4];
      if (std::memcmp(user_clip_plane_write_ptr, user_clip_plane_regs, 4 * sizeof(float))) {
        dirty = true;
        std::memcpy(user_clip_plane_write_ptr, user_clip_plane_regs, 4 * sizeof(float));
      }
      user_clip_plane_write_ptr += 4;
    }
  }

  // Conversion to Direct3D 12 normalized device coordinates.
  for (uint32_t i = 0; i < 3; ++i) {
    dirty |= system_constants_.ndc_scale[i] != viewport_info.ndc_scale[i];
    dirty |= system_constants_.ndc_offset[i] != viewport_info.ndc_offset[i];
    system_constants_.ndc_scale[i] = viewport_info.ndc_scale[i];
    system_constants_.ndc_offset[i] = viewport_info.ndc_offset[i];
  }

  // Point size.
  if (vgt_draw_initiator.prim_type == xenos::PrimitiveType::kPointList) {
    auto pa_su_point_minmax = regs.Get<reg::PA_SU_POINT_MINMAX>();
    auto pa_su_point_size = regs.Get<reg::PA_SU_POINT_SIZE>();
    float point_vertex_diameter_min = float(pa_su_point_minmax.min_size) * (2.0f / 16.0f);
    float point_vertex_diameter_max = float(pa_su_point_minmax.max_size) * (2.0f / 16.0f);
    float point_constant_diameter_x = float(pa_su_point_size.width) * (2.0f / 16.0f);
    float point_constant_diameter_y = float(pa_su_point_size.height) * (2.0f / 16.0f);
    dirty |= system_constants_.point_vertex_diameter_min != point_vertex_diameter_min;
    dirty |= system_constants_.point_vertex_diameter_max != point_vertex_diameter_max;
    dirty |= system_constants_.point_constant_diameter[0] != point_constant_diameter_x;
    dirty |= system_constants_.point_constant_diameter[1] != point_constant_diameter_y;
    system_constants_.point_vertex_diameter_min = point_vertex_diameter_min;
    system_constants_.point_vertex_diameter_max = point_vertex_diameter_max;
    system_constants_.point_constant_diameter[0] = point_constant_diameter_x;
    system_constants_.point_constant_diameter[1] = point_constant_diameter_y;
    // 2 because 1 in the NDC is half of the viewport's axis, 0.5 for diameter
    // to radius conversion to avoid multiplying the per-vertex diameter by an
    // additional constant in the shader.
    float point_screen_diameter_to_ndc_radius_x =
        (/* 0.5f * 2.0f * */ float(draw_resolution_scale_x)) /
        std::max(viewport_info.xy_extent[0], uint32_t(1));
    float point_screen_diameter_to_ndc_radius_y =
        (/* 0.5f * 2.0f * */ float(draw_resolution_scale_y)) /
        std::max(viewport_info.xy_extent[1], uint32_t(1));
    dirty |= system_constants_.point_screen_diameter_to_ndc_radius[0] !=
             point_screen_diameter_to_ndc_radius_x;
    dirty |= system_constants_.point_screen_diameter_to_ndc_radius[1] !=
             point_screen_diameter_to_ndc_radius_y;
    system_constants_.point_screen_diameter_to_ndc_radius[0] =
        point_screen_diameter_to_ndc_radius_x;
    system_constants_.point_screen_diameter_to_ndc_radius[1] =
        point_screen_diameter_to_ndc_radius_y;
  }

  // Texture signedness / gamma.
  uint32_t textures_resolution_scaled = 0;
  uint32_t textures_remaining = used_texture_mask;
  uint32_t texture_index;
  // GetActiveNativeResolveRegion yields an empty region unless draws are
  // resolution-scaled with region tracking; then the constants are zero.
  const bool native_regions_possible =
      texture_cache_->IsDrawResolutionScaled() && REXCVAR_GET(native_resolve_region_tracking);
  while (rex::bit_scan_forward(textures_remaining, &texture_index)) {
    textures_remaining &= ~(uint32_t(1) << texture_index);
    uint32_t& texture_signs_uint = system_constants_.texture_swizzled_signs[texture_index >> 2];
    uint32_t texture_signs_shift = (texture_index & 3) * 8;
    uint8_t texture_signs = texture_cache_->GetActiveTextureSwizzledSigns(texture_index);
    uint32_t texture_signs_shifted = uint32_t(texture_signs) << texture_signs_shift;
    uint32_t texture_signs_mask = uint32_t(0b11111111) << texture_signs_shift;
    dirty |= (texture_signs_uint & texture_signs_mask) != texture_signs_shifted;
    texture_signs_uint = (texture_signs_uint & ~texture_signs_mask) | texture_signs_shifted;
    textures_resolution_scaled |=
        uint32_t(texture_cache_->IsActiveTextureResolutionScaled(texture_index)) << texture_index;
    native_resolve::Rect native_region{};
    if (native_regions_possible) {
      texture_cache_->GetActiveNativeResolveRegion(texture_index, native_region, true);
    }
    float region_constants[4] = {float(native_region.left), float(native_region.top),
        float(native_region.right), float(native_region.bottom)};
    // A filter variant reconstructs every 2D fetch unless its region x is -1;
    // only the matched rules' fetches are enabled (see NativeFilterFetchMode).
    if (native_filter_active_) {
      region_constants[1] = region_constants[2] = region_constants[3] = 0.0f;
      switch (native_filter_fetch_modes_[texture_index]) {
        case NativeFilterFetchMode::kOff:
          region_constants[0] = -1.0f;
          break;
        case NativeFilterFetchMode::kUnbounded:
          region_constants[0] = 0.0f;
          break;
        case NativeFilterFetchMode::kRegion:
          std::memcpy(region_constants, native_filter_fetch_regions_[texture_index],
                      sizeof(region_constants));
          break;
        case NativeFilterFetchMode::kSourceNative: {
          // Mode -2: the shader picks the tracked native rectangle containing
          // each sample; -1: no tracked native content, it samples normally.
          native_resolve::Rect tracked[4];
          const size_t tracked_count =
              texture_cache_->GetActiveNativeResolveRegions(texture_index, tracked, 4);
          float candidates[4][4] = {};
          for (size_t i = 0; i < tracked_count; ++i) {
            candidates[i][0] = float(tracked[i].left);
            candidates[i][1] = float(tracked[i].top);
            candidates[i][2] = float(tracked[i].right);
            candidates[i][3] = float(tracked[i].bottom);
          }
          if (std::memcmp(system_constants_.native_filter_candidate_regions, candidates,
                          sizeof(candidates))) {
            std::memcpy(system_constants_.native_filter_candidate_regions, candidates,
                        sizeof(candidates));
            dirty = true;
          }
          region_constants[0] = tracked_count ? -2.0f : -1.0f;
          break;
        }
      }
    }
    if (std::memcmp(system_constants_.native_texture_regions[texture_index], region_constants,
                    sizeof(region_constants))) {
      std::memcpy(system_constants_.native_texture_regions[texture_index], region_constants,
                   sizeof(region_constants));
      dirty = true;
    }
  }
  dirty |= system_constants_.textures_resolution_scaled != textures_resolution_scaled;
  system_constants_.textures_resolution_scaled = textures_resolution_scaled;

  // Log2 of sample count, for alpha to mask and with ROV, for EDRAM address
  // calculation with MSAA.
  uint32_t sample_count_log2_x = rb_surface_info.msaa_samples >= xenos::MsaaSamples::k4X ? 1 : 0;
  uint32_t sample_count_log2_y = rb_surface_info.msaa_samples >= xenos::MsaaSamples::k2X ? 1 : 0;
  dirty |= system_constants_.sample_count_log2[0] != sample_count_log2_x;
  dirty |= system_constants_.sample_count_log2[1] != sample_count_log2_y;
  system_constants_.sample_count_log2[0] = sample_count_log2_x;
  system_constants_.sample_count_log2[1] = sample_count_log2_y;

  // Alpha test and alpha to coverage.
  dirty |= system_constants_.alpha_test_reference != rb_alpha_ref;
  system_constants_.alpha_test_reference = rb_alpha_ref;
  uint32_t alpha_to_mask =
      rb_colorcontrol.alpha_to_mask_enable ? (rb_colorcontrol.value >> 24) | (1 << 8) : 0;
  dirty |= system_constants_.alpha_to_mask != alpha_to_mask;
  system_constants_.alpha_to_mask = alpha_to_mask;

  uint32_t edram_tile_dwords_scaled = xenos::kEdramTileWidthSamples *
                                      xenos::kEdramTileHeightSamples *
                                      (draw_resolution_scale_x * draw_resolution_scale_y);

  // EDRAM pitch for ROV writing.
  if (edram_rov_used) {
    // Align, then multiply by 32bpp tile size in dwords.
    uint32_t edram_32bpp_tile_pitch_dwords_scaled =
        ((rb_surface_info.surface_pitch *
          (rb_surface_info.msaa_samples >= xenos::MsaaSamples::k4X ? 2 : 1)) +
         (xenos::kEdramTileWidthSamples - 1)) /
        xenos::kEdramTileWidthSamples * edram_tile_dwords_scaled;
    dirty |= system_constants_.edram_32bpp_tile_pitch_dwords_scaled !=
             edram_32bpp_tile_pitch_dwords_scaled;
    system_constants_.edram_32bpp_tile_pitch_dwords_scaled = edram_32bpp_tile_pitch_dwords_scaled;
  }

  // Color exponent bias and ROV render target writing.
  for (uint32_t i = 0; i < 4; ++i) {
    reg::RB_COLOR_INFO color_info = color_infos[i];
    // Exponent bias is in bits 20:25 of RB_COLOR_INFO.
    int32_t color_exp_bias = color_info.color_exp_bias;
    if (color_info.color_format == xenos::ColorRenderTargetFormat::k_16_16 ||
        color_info.color_format == xenos::ColorRenderTargetFormat::k_16_16_16_16) {
      if (render_target_cache_->GetPath() == RenderTargetCache::Path::kHostRenderTargets &&
          !render_target_cache_->IsFixed16TruncatedToMinus1To1()) {
        // Remap from -32...32 to -1...1 by dividing the output values by 32,
        // losing blending correctness, but getting the full range.
        color_exp_bias -= 5;
      }
    }
    auto color_exp_bias_scale =
        rex::memory::Reinterpret<float>(int32_t(0x3F800000 + (color_exp_bias << 23)));
    dirty |= system_constants_.color_exp_bias[i] != color_exp_bias_scale;
    system_constants_.color_exp_bias[i] = color_exp_bias_scale;
    if (edram_rov_used) {
      dirty |= system_constants_.edram_rt_keep_mask[i][0] != rt_keep_masks[i][0];
      system_constants_.edram_rt_keep_mask[i][0] = rt_keep_masks[i][0];
      dirty |= system_constants_.edram_rt_keep_mask[i][1] != rt_keep_masks[i][1];
      system_constants_.edram_rt_keep_mask[i][1] = rt_keep_masks[i][1];
      if (rt_keep_masks[i][0] != UINT32_MAX || rt_keep_masks[i][1] != UINT32_MAX) {
        uint32_t rt_base_dwords_scaled = color_info.color_base * edram_tile_dwords_scaled;
        dirty |= system_constants_.edram_rt_base_dwords_scaled[i] != rt_base_dwords_scaled;
        system_constants_.edram_rt_base_dwords_scaled[i] = rt_base_dwords_scaled;
        uint32_t format_flags = RenderTargetCache::AddPSIColorFormatFlags(color_info.color_format);
        dirty |= system_constants_.edram_rt_format_flags[i] != format_flags;
        system_constants_.edram_rt_format_flags[i] = format_flags;
        // Can't do float comparisons here because NaNs would result in always
        // setting the dirty flag.
        dirty |=
            std::memcmp(system_constants_.edram_rt_clamp[i], rt_clamp[i], 4 * sizeof(float)) != 0;
        std::memcpy(system_constants_.edram_rt_clamp[i], rt_clamp[i], 4 * sizeof(float));
        uint32_t blend_factors_ops =
            regs[reg::RB_BLENDCONTROL::rt_register_indices[i]] & 0x1FFF1FFF;
        dirty |= system_constants_.edram_rt_blend_factors_ops[i] != blend_factors_ops;
        system_constants_.edram_rt_blend_factors_ops[i] = blend_factors_ops;
      }
    }
  }

  if (edram_rov_used) {
    uint32_t depth_base_dwords_scaled = rb_depth_info.depth_base * edram_tile_dwords_scaled;
    dirty |= system_constants_.edram_depth_base_dwords_scaled != depth_base_dwords_scaled;
    system_constants_.edram_depth_base_dwords_scaled = depth_base_dwords_scaled;

    // For non-polygons, front polygon offset is used, and it's enabled if
    // POLY_OFFSET_PARA_ENABLED is set, for polygons, separate front and back
    // are used.
    float poly_offset_front_scale = 0.0f, poly_offset_front_offset = 0.0f;
    float poly_offset_back_scale = 0.0f, poly_offset_back_offset = 0.0f;
    if (primitive_polygonal) {
      if (pa_su_sc_mode_cntl.poly_offset_front_enable) {
        poly_offset_front_scale = regs.Get<float>(XE_GPU_REG_PA_SU_POLY_OFFSET_FRONT_SCALE);
        poly_offset_front_offset = regs.Get<float>(XE_GPU_REG_PA_SU_POLY_OFFSET_FRONT_OFFSET);
      }
      if (pa_su_sc_mode_cntl.poly_offset_back_enable) {
        poly_offset_back_scale = regs.Get<float>(XE_GPU_REG_PA_SU_POLY_OFFSET_BACK_SCALE);
        poly_offset_back_offset = regs.Get<float>(XE_GPU_REG_PA_SU_POLY_OFFSET_BACK_OFFSET);
      }
    } else {
      if (pa_su_sc_mode_cntl.poly_offset_para_enable) {
        poly_offset_front_scale = regs.Get<float>(XE_GPU_REG_PA_SU_POLY_OFFSET_FRONT_SCALE);
        poly_offset_front_offset = regs.Get<float>(XE_GPU_REG_PA_SU_POLY_OFFSET_FRONT_OFFSET);
        poly_offset_back_scale = poly_offset_front_scale;
        poly_offset_back_offset = poly_offset_front_offset;
      }
    }
    // With non-square resolution scaling, make sure the worst-case impact is
    // reverted (slope only along the scaled axis), thus max. More bias is
    // better than less bias, because less bias means Z fighting with the
    // background is more likely.
    float poly_offset_scale_factor = xenos::kPolygonOffsetScaleSubpixelUnit *
                                     std::max(draw_resolution_scale_x, draw_resolution_scale_y);
    poly_offset_front_scale *= poly_offset_scale_factor;
    poly_offset_back_scale *= poly_offset_scale_factor;
    dirty |= system_constants_.edram_poly_offset_front_scale != poly_offset_front_scale;
    system_constants_.edram_poly_offset_front_scale = poly_offset_front_scale;
    dirty |= system_constants_.edram_poly_offset_front_offset != poly_offset_front_offset;
    system_constants_.edram_poly_offset_front_offset = poly_offset_front_offset;
    dirty |= system_constants_.edram_poly_offset_back_scale != poly_offset_back_scale;
    system_constants_.edram_poly_offset_back_scale = poly_offset_back_scale;
    dirty |= system_constants_.edram_poly_offset_back_offset != poly_offset_back_offset;
    system_constants_.edram_poly_offset_back_offset = poly_offset_back_offset;

    if (depth_stencil_enabled && normalized_depth_control.stencil_enable) {
      dirty |= system_constants_.edram_stencil_front_reference != rb_stencilrefmask.stencilref;
      system_constants_.edram_stencil_front_reference = rb_stencilrefmask.stencilref;
      dirty |= system_constants_.edram_stencil_front_read_mask != rb_stencilrefmask.stencilmask;
      system_constants_.edram_stencil_front_read_mask = rb_stencilrefmask.stencilmask;
      dirty |=
          system_constants_.edram_stencil_front_write_mask != rb_stencilrefmask.stencilwritemask;
      system_constants_.edram_stencil_front_write_mask = rb_stencilrefmask.stencilwritemask;
      uint32_t stencil_func_ops = (normalized_depth_control.value >> 8) & ((1 << 12) - 1);
      dirty |= system_constants_.edram_stencil_front_func_ops != stencil_func_ops;
      system_constants_.edram_stencil_front_func_ops = stencil_func_ops;

      if (primitive_polygonal && normalized_depth_control.backface_enable) {
        dirty |= system_constants_.edram_stencil_back_reference != rb_stencilrefmask_bf.stencilref;
        system_constants_.edram_stencil_back_reference = rb_stencilrefmask_bf.stencilref;
        dirty |= system_constants_.edram_stencil_back_read_mask != rb_stencilrefmask_bf.stencilmask;
        system_constants_.edram_stencil_back_read_mask = rb_stencilrefmask_bf.stencilmask;
        dirty |= system_constants_.edram_stencil_back_write_mask !=
                 rb_stencilrefmask_bf.stencilwritemask;
        system_constants_.edram_stencil_back_write_mask = rb_stencilrefmask_bf.stencilwritemask;
        uint32_t stencil_func_ops_bf = (normalized_depth_control.value >> 20) & ((1 << 12) - 1);
        dirty |= system_constants_.edram_stencil_back_func_ops != stencil_func_ops_bf;
        system_constants_.edram_stencil_back_func_ops = stencil_func_ops_bf;
      } else {
        dirty |= std::memcmp(system_constants_.edram_stencil_back,
                             system_constants_.edram_stencil_front, 4 * sizeof(uint32_t)) != 0;
        std::memcpy(system_constants_.edram_stencil_back, system_constants_.edram_stencil_front,
                    4 * sizeof(uint32_t));
      }
    }

    dirty |= system_constants_.edram_blend_constant[0] != regs.Get<float>(XE_GPU_REG_RB_BLEND_RED);
    system_constants_.edram_blend_constant[0] = regs.Get<float>(XE_GPU_REG_RB_BLEND_RED);
    dirty |=
        system_constants_.edram_blend_constant[1] != regs.Get<float>(XE_GPU_REG_RB_BLEND_GREEN);
    system_constants_.edram_blend_constant[1] = regs.Get<float>(XE_GPU_REG_RB_BLEND_GREEN);
    dirty |= system_constants_.edram_blend_constant[2] != regs.Get<float>(XE_GPU_REG_RB_BLEND_BLUE);
    system_constants_.edram_blend_constant[2] = regs.Get<float>(XE_GPU_REG_RB_BLEND_BLUE);
    dirty |=
        system_constants_.edram_blend_constant[3] != regs.Get<float>(XE_GPU_REG_RB_BLEND_ALPHA);
    system_constants_.edram_blend_constant[3] = regs.Get<float>(XE_GPU_REG_RB_BLEND_ALPHA);
  }

  cbuffer_binding_system_.up_to_date &= !dirty;
}

bool D3D12CommandProcessor::UpdateBindings(const D3D12Shader* vertex_shader,
                                           const D3D12Shader* pixel_shader,
                                           ID3D12RootSignature* root_signature,
                                           bool shared_memory_is_uav) {
  const ui::d3d12::D3D12Provider& provider = GetD3D12Provider();
  ID3D12Device* device = provider.GetDevice();
  const RegisterFile& regs = *register_file_;

#if XE_GPU_FINE_GRAINED_DRAW_SCOPES
  SCOPE_profile_cpu_f("gpu");
#endif  // XE_GPU_FINE_GRAINED_DRAW_SCOPES

  // Set the new root signature.
  if (current_graphics_root_signature_ != root_signature) {
    current_graphics_root_signature_ = root_signature;
    if (!bindless_resources_used_) {
      GetRootBindfulExtraParameterIndices(vertex_shader, pixel_shader,
                                          current_graphics_root_bindful_extras_);
    }
    // Changing the root signature invalidates all bindings.
    current_graphics_root_up_to_date_ = 0;
    deferred_command_list_.D3DSetGraphicsRootSignature(root_signature);
  }

  // Select the root parameter indices depending on the used binding model.
  uint32_t root_parameter_fetch_constants = bindless_resources_used_
                                                ? kRootParameter_Bindless_FetchConstants
                                                : kRootParameter_Bindful_FetchConstants;
  uint32_t root_parameter_float_constants_vertex =
      bindless_resources_used_ ? kRootParameter_Bindless_FloatConstantsVertex
                               : kRootParameter_Bindful_FloatConstantsVertex;
  uint32_t root_parameter_float_constants_pixel = bindless_resources_used_
                                                      ? kRootParameter_Bindless_FloatConstantsPixel
                                                      : kRootParameter_Bindful_FloatConstantsPixel;
  uint32_t root_parameter_system_constants = bindless_resources_used_
                                                 ? kRootParameter_Bindless_SystemConstants
                                                 : kRootParameter_Bindful_SystemConstants;
  uint32_t root_parameter_bool_loop_constants = bindless_resources_used_
                                                    ? kRootParameter_Bindless_BoolLoopConstants
                                                    : kRootParameter_Bindful_BoolLoopConstants;
  uint32_t root_parameter_shared_memory_and_bindful_edram =
      bindless_resources_used_ ? kRootParameter_Bindless_SharedMemory
                               : kRootParameter_Bindful_SharedMemoryAndEdram;

  //
  // Update root constant buffers that are common for bindful and bindless.
  //

  // These are the constant base addresses/ranges for shaders.
  // We have these hardcoded right now cause nothing seems to differ on the Xbox
  // 360 (however, OpenGL ES on Adreno 200 on Android has different ranges).
  assert_true(regs[XE_GPU_REG_SQ_VS_CONST] == 0x000FF000 ||
              regs[XE_GPU_REG_SQ_VS_CONST] == 0x00000000);
  assert_true(regs[XE_GPU_REG_SQ_PS_CONST] == 0x000FF100 ||
              regs[XE_GPU_REG_SQ_PS_CONST] == 0x00000000);
  // Check if the float constant layout is still the same and get the counts.
  const Shader::ConstantRegisterMap& float_constant_map_vertex =
      vertex_shader->constant_register_map();
  uint32_t float_constant_count_vertex = float_constant_map_vertex.float_count;
  for (uint32_t i = 0; i < 4; ++i) {
    if (current_float_constant_map_vertex_[i] != float_constant_map_vertex.float_bitmap[i]) {
      current_float_constant_map_vertex_[i] = float_constant_map_vertex.float_bitmap[i];
      // If no float constants at all, we can reuse any buffer for them, so not
      // invalidating.
      if (float_constant_count_vertex) {
        cbuffer_binding_float_vertex_.up_to_date = false;
      }
    }
  }
  uint32_t float_constant_count_pixel = 0;
  if (pixel_shader != nullptr) {
    const Shader::ConstantRegisterMap& float_constant_map_pixel =
        pixel_shader->constant_register_map();
    float_constant_count_pixel = float_constant_map_pixel.float_count;
    for (uint32_t i = 0; i < 4; ++i) {
      if (current_float_constant_map_pixel_[i] != float_constant_map_pixel.float_bitmap[i]) {
        current_float_constant_map_pixel_[i] = float_constant_map_pixel.float_bitmap[i];
        if (float_constant_count_pixel) {
          cbuffer_binding_float_pixel_.up_to_date = false;
        }
      }
    }
  } else {
    std::memset(current_float_constant_map_pixel_, 0, sizeof(current_float_constant_map_pixel_));
  }

  // Write the constant buffer data.
  if (!cbuffer_binding_system_.up_to_date) {
    uint8_t* system_constants = constant_buffer_pool_->Request(
        frame_current_, sizeof(system_constants_), D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT,
        nullptr, nullptr, &cbuffer_binding_system_.address);
    if (system_constants == nullptr) {
      return false;
    }
    std::memcpy(system_constants, &system_constants_, sizeof(system_constants_));
    cbuffer_binding_system_.up_to_date = true;
    current_graphics_root_up_to_date_ &= ~(1u << root_parameter_system_constants);
  }
  if (!cbuffer_binding_float_vertex_.up_to_date) {
    // Even if the shader doesn't need any float constants, a valid binding must
    // still be provided, so if the first draw in the frame with the current
    // root signature doesn't have float constants at all, still allocate an
    // empty buffer.
    uint8_t* float_constants = constant_buffer_pool_->Request(
        frame_current_, sizeof(float) * 4 * std::max(float_constant_count_vertex, uint32_t(1)),
        D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT, nullptr, nullptr,
        &cbuffer_binding_float_vertex_.address);
    if (float_constants == nullptr) {
      return false;
    }
    embedded_float_vertex_cpu_address_ = float_constants;
    embedded_float_vertex_cpu_size_ =
        sizeof(float) * 4 * std::max(float_constant_count_vertex, uint32_t(1));
    for (uint32_t i = 0; i < 4; ++i) {
      uint64_t float_constant_map_entry = float_constant_map_vertex.float_bitmap[i];
      uint32_t float_constant_index;
      while (rex::bit_scan_forward(float_constant_map_entry, &float_constant_index)) {
        float_constant_map_entry &= ~(1ull << float_constant_index);
        std::memcpy(
            float_constants,
            &regs[XE_GPU_REG_SHADER_CONSTANT_000_X + (i << 8) + (float_constant_index << 2)],
            4 * sizeof(float));
        float_constants += 4 * sizeof(float);
      }
    }
    cbuffer_binding_float_vertex_.up_to_date = true;
    current_graphics_root_up_to_date_ &= ~(1u << root_parameter_float_constants_vertex);
  }
  if (!cbuffer_binding_float_pixel_.up_to_date) {
    uint8_t* float_constants = constant_buffer_pool_->Request(
        frame_current_, sizeof(float) * 4 * std::max(float_constant_count_pixel, uint32_t(1)),
        D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT, nullptr, nullptr,
        &cbuffer_binding_float_pixel_.address);
    if (float_constants == nullptr) {
      return false;
    }
    embedded_float_pixel_cpu_address_ = float_constants;
    embedded_float_pixel_cpu_size_ =
        sizeof(float) * 4 * std::max(float_constant_count_pixel, uint32_t(1));
    if (pixel_shader != nullptr) {
      const Shader::ConstantRegisterMap& float_constant_map_pixel =
          pixel_shader->constant_register_map();
      for (uint32_t i = 0; i < 4; ++i) {
        uint64_t float_constant_map_entry = float_constant_map_pixel.float_bitmap[i];
        uint32_t float_constant_index;
        while (rex::bit_scan_forward(float_constant_map_entry, &float_constant_index)) {
          float_constant_map_entry &= ~(1ull << float_constant_index);
          std::memcpy(
              float_constants,
              &regs[XE_GPU_REG_SHADER_CONSTANT_256_X + (i << 8) + (float_constant_index << 2)],
              4 * sizeof(float));
          float_constants += 4 * sizeof(float);
        }
      }
    }
    cbuffer_binding_float_pixel_.up_to_date = true;
    current_graphics_root_up_to_date_ &= ~(1u << root_parameter_float_constants_pixel);
  }
  if (!cbuffer_binding_bool_loop_.up_to_date) {
    constexpr uint32_t kBoolLoopConstantsSize = (8 + 32) * sizeof(uint32_t);
    uint8_t* bool_loop_constants = constant_buffer_pool_->Request(
        frame_current_, kBoolLoopConstantsSize, D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT,
        nullptr, nullptr, &cbuffer_binding_bool_loop_.address);
    if (bool_loop_constants == nullptr) {
      return false;
    }
    std::memcpy(bool_loop_constants, &regs[XE_GPU_REG_SHADER_CONSTANT_BOOL_000_031],
                kBoolLoopConstantsSize);
    cbuffer_binding_bool_loop_.up_to_date = true;
    current_graphics_root_up_to_date_ &= ~(1u << root_parameter_bool_loop_constants);
  }
  if (!cbuffer_binding_fetch_.up_to_date) {
    constexpr uint32_t kFetchConstantsSize = 32 * 6 * sizeof(uint32_t);
    uint8_t* fetch_constants = constant_buffer_pool_->Request(
        frame_current_, kFetchConstantsSize, D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT,
        nullptr, nullptr, &cbuffer_binding_fetch_.address);
    if (fetch_constants == nullptr) {
      return false;
    }
    std::memcpy(fetch_constants, &regs[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0], kFetchConstantsSize);
    cbuffer_binding_fetch_.up_to_date = true;
    current_graphics_root_up_to_date_ &= ~(1u << root_parameter_fetch_constants);
  }

  //
  // Update descriptors.
  //

  if (!current_shared_memory_binding_is_uav_.has_value() ||
      current_shared_memory_binding_is_uav_.value() != shared_memory_is_uav) {
    current_shared_memory_binding_is_uav_ = shared_memory_is_uav;
    current_graphics_root_up_to_date_ &= ~(1u << root_parameter_shared_memory_and_bindful_edram);
  }

  // Get textures and samplers used by the vertex shader, check if the last used
  // samplers are compatible and update them.
  size_t texture_layout_uid_vertex = vertex_shader->GetTextureBindingLayoutUserUID();
  size_t sampler_layout_uid_vertex = vertex_shader->GetSamplerBindingLayoutUserUID();
  const std::vector<D3D12Shader::TextureBinding>& textures_vertex =
      vertex_shader->GetTextureBindingsAfterTranslation();
  const std::vector<D3D12Shader::SamplerBinding>& samplers_vertex =
      vertex_shader->GetSamplerBindingsAfterTranslation();
  size_t texture_count_vertex = textures_vertex.size();
  size_t sampler_count_vertex = samplers_vertex.size();
  if (sampler_count_vertex) {
    if (current_sampler_layout_uid_vertex_ != sampler_layout_uid_vertex) {
      current_sampler_layout_uid_vertex_ = sampler_layout_uid_vertex;
      cbuffer_binding_descriptor_indices_vertex_.up_to_date = false;
      bindful_samplers_written_vertex_ = false;
    }
    current_samplers_vertex_.resize(
        std::max(current_samplers_vertex_.size(), sampler_count_vertex));
    for (size_t i = 0; i < sampler_count_vertex; ++i) {
      D3D12TextureCache::SamplerParameters parameters =
          texture_cache_->GetSamplerParameters(samplers_vertex[i]);
      if (current_samplers_vertex_[i] != parameters) {
        cbuffer_binding_descriptor_indices_vertex_.up_to_date = false;
        bindful_samplers_written_vertex_ = false;
        current_samplers_vertex_[i] = parameters;
      }
    }
  }

  // Get textures and samplers used by the pixel shader, check if the last used
  // samplers are compatible and update them.
  size_t texture_layout_uid_pixel, sampler_layout_uid_pixel;
  const std::vector<D3D12Shader::TextureBinding>* textures_pixel;
  const std::vector<D3D12Shader::SamplerBinding>* samplers_pixel;
  size_t texture_count_pixel, sampler_count_pixel;
  if (pixel_shader != nullptr) {
    texture_layout_uid_pixel = pixel_shader->GetTextureBindingLayoutUserUID();
    sampler_layout_uid_pixel = pixel_shader->GetSamplerBindingLayoutUserUID();
    textures_pixel = &pixel_shader->GetTextureBindingsAfterTranslation();
    texture_count_pixel = textures_pixel->size();
    samplers_pixel = &pixel_shader->GetSamplerBindingsAfterTranslation();
    sampler_count_pixel = samplers_pixel->size();
    if (sampler_count_pixel) {
      if (current_sampler_layout_uid_pixel_ != sampler_layout_uid_pixel) {
        current_sampler_layout_uid_pixel_ = sampler_layout_uid_pixel;
        cbuffer_binding_descriptor_indices_pixel_.up_to_date = false;
        bindful_samplers_written_pixel_ = false;
      }
      current_samplers_pixel_.resize(
          std::max(current_samplers_pixel_.size(), size_t(sampler_count_pixel)));
      for (uint32_t i = 0; i < sampler_count_pixel; ++i) {
        D3D12TextureCache::SamplerParameters parameters =
            texture_cache_->GetSamplerParameters((*samplers_pixel)[i]);
        if (current_samplers_pixel_[i] != parameters) {
          current_samplers_pixel_[i] = parameters;
          cbuffer_binding_descriptor_indices_pixel_.up_to_date = false;
          bindful_samplers_written_pixel_ = false;
        }
      }
    }
  } else {
    texture_layout_uid_pixel = PipelineCache::kLayoutUIDEmpty;
    sampler_layout_uid_pixel = PipelineCache::kLayoutUIDEmpty;
    textures_pixel = nullptr;
    texture_count_pixel = 0;
    samplers_pixel = nullptr;
    sampler_count_pixel = 0;
  }

  assert_true(sampler_count_vertex + sampler_count_pixel <= kSamplerHeapSize);

  if (bindless_resources_used_) {
    //
    // Bindless descriptors path.
    //

    // Check if need to write new descriptor indices.
    // Samplers have already been checked.
    if (texture_count_vertex && cbuffer_binding_descriptor_indices_vertex_.up_to_date &&
        (current_texture_layout_uid_vertex_ != texture_layout_uid_vertex ||
         !texture_cache_->AreActiveTextureSRVKeysUpToDate(current_texture_srv_keys_vertex_.data(),
                                                          textures_vertex.data(),
                                                          texture_count_vertex))) {
      cbuffer_binding_descriptor_indices_vertex_.up_to_date = false;
    }
    if (texture_count_pixel && cbuffer_binding_descriptor_indices_pixel_.up_to_date &&
        (current_texture_layout_uid_pixel_ != texture_layout_uid_pixel ||
         !texture_cache_->AreActiveTextureSRVKeysUpToDate(current_texture_srv_keys_pixel_.data(),
                                                          textures_pixel->data(),
                                                          texture_count_pixel))) {
      cbuffer_binding_descriptor_indices_pixel_.up_to_date = false;
    }

    // Get sampler descriptor indices, write new samplers, and handle sampler
    // heap overflow if it happens.
    if ((sampler_count_vertex && !cbuffer_binding_descriptor_indices_vertex_.up_to_date) ||
        (sampler_count_pixel && !cbuffer_binding_descriptor_indices_pixel_.up_to_date)) {
      for (uint32_t i = 0; i < 2; ++i) {
        if (i) {
          // Overflow happened - invalidate sampler bindings because their
          // descriptor indices can't be used anymore (and even if heap creation
          // fails, because current_sampler_bindless_indices_#_ are in an
          // undefined state now) and switch to a new sampler heap.
          cbuffer_binding_descriptor_indices_vertex_.up_to_date = false;
          cbuffer_binding_descriptor_indices_pixel_.up_to_date = false;
          ID3D12DescriptorHeap* sampler_heap_new;
          if (!sampler_bindless_heaps_overflowed_.empty() &&
              sampler_bindless_heaps_overflowed_.front().second <= submission_completed_) {
            sampler_heap_new = sampler_bindless_heaps_overflowed_.front().first;
            sampler_bindless_heaps_overflowed_.pop_front();
          } else {
            D3D12_DESCRIPTOR_HEAP_DESC sampler_heap_new_desc;
            sampler_heap_new_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
            sampler_heap_new_desc.NumDescriptors = kSamplerHeapSize;
            sampler_heap_new_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
            sampler_heap_new_desc.NodeMask = 0;
            if (FAILED(device->CreateDescriptorHeap(&sampler_heap_new_desc,
                                                    IID_PPV_ARGS(&sampler_heap_new)))) {
              REXGPU_ERROR(
                  "Failed to create a new bindless sampler descriptor heap "
                  "after an overflow of the previous one");
              return false;
            }
          }
          // Only change the heap if a new heap was created successfully, not to
          // leave the values in an undefined state in case CreateDescriptorHeap
          // has failed.
          sampler_bindless_heaps_overflowed_.push_back(
              std::make_pair(sampler_bindless_heap_current_, submission_current_));
          sampler_bindless_heap_current_ = sampler_heap_new;
          sampler_bindless_heap_cpu_start_ =
              sampler_bindless_heap_current_->GetCPUDescriptorHandleForHeapStart();
          sampler_bindless_heap_gpu_start_ =
              sampler_bindless_heap_current_->GetGPUDescriptorHandleForHeapStart();
          sampler_bindless_heap_allocated_ = 0;
          // The only thing the heap is used for now is texture cache samplers -
          // invalidate all of them.
          texture_cache_bindless_sampler_map_.clear();
          deferred_command_list_.SetDescriptorHeaps(view_bindless_heap_,
                                                    sampler_bindless_heap_current_);
          current_graphics_root_up_to_date_ &= ~(1u << kRootParameter_Bindless_SamplerHeap);
        }
        bool samplers_overflowed = false;
        if (sampler_count_vertex && !cbuffer_binding_descriptor_indices_vertex_.up_to_date) {
          current_sampler_bindless_indices_vertex_.resize(std::max(
              current_sampler_bindless_indices_vertex_.size(), size_t(sampler_count_vertex)));
          for (uint32_t j = 0; j < sampler_count_vertex; ++j) {
            D3D12TextureCache::SamplerParameters sampler_parameters = current_samplers_vertex_[j];
            uint32_t sampler_index;
            auto it = texture_cache_bindless_sampler_map_.find(sampler_parameters.value);
            if (it != texture_cache_bindless_sampler_map_.end()) {
              sampler_index = it->second;
            } else {
              if (sampler_bindless_heap_allocated_ >= kSamplerHeapSize) {
                samplers_overflowed = true;
                break;
              }
              sampler_index = sampler_bindless_heap_allocated_++;
              texture_cache_->WriteSampler(sampler_parameters,
                                           provider.OffsetSamplerDescriptor(
                                               sampler_bindless_heap_cpu_start_, sampler_index));
              texture_cache_bindless_sampler_map_.emplace(sampler_parameters.value, sampler_index);
            }
            current_sampler_bindless_indices_vertex_[j] = sampler_index;
          }
        }
        if (samplers_overflowed) {
          continue;
        }
        if (sampler_count_pixel && !cbuffer_binding_descriptor_indices_pixel_.up_to_date) {
          current_sampler_bindless_indices_pixel_.resize(std::max(
              current_sampler_bindless_indices_pixel_.size(), size_t(sampler_count_pixel)));
          for (uint32_t j = 0; j < sampler_count_pixel; ++j) {
            D3D12TextureCache::SamplerParameters sampler_parameters = current_samplers_pixel_[j];
            uint32_t sampler_index;
            auto it = texture_cache_bindless_sampler_map_.find(sampler_parameters.value);
            if (it != texture_cache_bindless_sampler_map_.end()) {
              sampler_index = it->second;
            } else {
              if (sampler_bindless_heap_allocated_ >= kSamplerHeapSize) {
                samplers_overflowed = true;
                break;
              }
              sampler_index = sampler_bindless_heap_allocated_++;
              texture_cache_->WriteSampler(sampler_parameters,
                                           provider.OffsetSamplerDescriptor(
                                               sampler_bindless_heap_cpu_start_, sampler_index));
              texture_cache_bindless_sampler_map_.emplace(sampler_parameters.value, sampler_index);
            }
            current_sampler_bindless_indices_pixel_[j] = sampler_index;
          }
        }
        if (!samplers_overflowed) {
          break;
        }
      }
    }

    if (!cbuffer_binding_descriptor_indices_vertex_.up_to_date) {
      uint32_t* descriptor_indices = reinterpret_cast<uint32_t*>(constant_buffer_pool_->Request(
          frame_current_,
          std::max(texture_count_vertex + sampler_count_vertex, size_t(1)) * sizeof(uint32_t),
          D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT, nullptr, nullptr,
          &cbuffer_binding_descriptor_indices_vertex_.address));
      if (!descriptor_indices) {
        return false;
      }
      for (size_t i = 0; i < texture_count_vertex; ++i) {
        const D3D12Shader::TextureBinding& texture = textures_vertex[i];
        descriptor_indices[texture.bindless_descriptor_index] =
            texture_cache_->GetActiveTextureBindlessSRVIndex(texture) -
            uint32_t(SystemBindlessView::kUnboundedSRVsStart);
      }
      current_texture_layout_uid_vertex_ = texture_layout_uid_vertex;
      if (texture_count_vertex) {
        current_texture_srv_keys_vertex_.resize(
            std::max(current_texture_srv_keys_vertex_.size(), size_t(texture_count_vertex)));
        texture_cache_->WriteActiveTextureSRVKeys(current_texture_srv_keys_vertex_.data(),
                                                  textures_vertex.data(), texture_count_vertex);
      }
      // Current samplers have already been updated.
      for (size_t i = 0; i < sampler_count_vertex; ++i) {
        descriptor_indices[samplers_vertex[i].bindless_descriptor_index] =
            current_sampler_bindless_indices_vertex_[i];
      }
      cbuffer_binding_descriptor_indices_vertex_.up_to_date = true;
      current_graphics_root_up_to_date_ &= ~(1u << kRootParameter_Bindless_DescriptorIndicesVertex);
    }

    if (!cbuffer_binding_descriptor_indices_pixel_.up_to_date) {
      uint32_t* descriptor_indices = reinterpret_cast<uint32_t*>(constant_buffer_pool_->Request(
          frame_current_,
          std::max(texture_count_pixel + sampler_count_pixel, size_t(1)) * sizeof(uint32_t),
          D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT, nullptr, nullptr,
          &cbuffer_binding_descriptor_indices_pixel_.address));
      if (!descriptor_indices) {
        return false;
      }
      for (size_t i = 0; i < texture_count_pixel; ++i) {
        const D3D12Shader::TextureBinding& texture = (*textures_pixel)[i];
        descriptor_indices[texture.bindless_descriptor_index] =
            texture_cache_->GetActiveTextureBindlessSRVIndex(texture) -
            uint32_t(SystemBindlessView::kUnboundedSRVsStart);
      }
      current_texture_layout_uid_pixel_ = texture_layout_uid_pixel;
      if (texture_count_pixel) {
        current_texture_srv_keys_pixel_.resize(
            std::max(current_texture_srv_keys_pixel_.size(), size_t(texture_count_pixel)));
        texture_cache_->WriteActiveTextureSRVKeys(current_texture_srv_keys_pixel_.data(),
                                                  textures_pixel->data(), texture_count_pixel);
      }
      // Current samplers have already been updated.
      for (size_t i = 0; i < sampler_count_pixel; ++i) {
        descriptor_indices[(*samplers_pixel)[i].bindless_descriptor_index] =
            current_sampler_bindless_indices_pixel_[i];
      }
      // Capture the exact CPU values consumed by the prompt shader's
      // descriptor-index cbuffer. Bindless indices are one-based here, so the
      // unused zero slot plus all three live slots are recorded explicitly.
      static bool prompt_descriptor_indices_logged = false;
      if (kEmbeddedGraphicsDiagnosticsEnabled && !prompt_descriptor_indices_logged &&
          !kernel_state_ && pixel_shader &&
          pixel_shader->ucode_data_hash() == UINT64_C(0x207D40E674A7C916)) {
        const xenos::xe_gpu_texture_fetch_t prompt_fetch = regs.GetTextureFetch(0);
        if (prompt_fetch.format == xenos::TextureFormat::k_DXT1 && prompt_fetch.tiled &&
            prompt_fetch.size_2d.width + 1 == 512 && prompt_fetch.size_2d.height + 1 == 191) {
          prompt_descriptor_indices_logged = true;
          const size_t descriptor_count = texture_count_pixel + sampler_count_pixel;
          const size_t descriptor_words = rex::align(descriptor_count + 1, size_t(4));
          std::fprintf(stderr,
                       "REX_EMBEDDED_PROMPT_DESCRIPTOR_INDICES address=0x%016llX "
                       "count=%llu words=%llu values=",
                       static_cast<unsigned long long>(
                           cbuffer_binding_descriptor_indices_pixel_.address),
                       static_cast<unsigned long long>(descriptor_count),
                       static_cast<unsigned long long>(descriptor_words));
          for (size_t i = 0; i < descriptor_words; ++i) {
            std::fprintf(stderr, "%s%08X", i ? "," : "", descriptor_indices[i]);
          }
          std::fputc('\n', stderr);
          std::fflush(stderr);
        }
      }
      cbuffer_binding_descriptor_indices_pixel_.up_to_date = true;
      current_graphics_root_up_to_date_ &= ~(1u << kRootParameter_Bindless_DescriptorIndicesPixel);
    }
  } else {
    //
    // Bindful descriptors path.
    //

    // See what descriptors need to be updated.
    // Samplers have already been checked.
    bool write_textures_vertex =
        texture_count_vertex && (!bindful_textures_written_vertex_ ||
                                 current_texture_layout_uid_vertex_ != texture_layout_uid_vertex ||
                                 !texture_cache_->AreActiveTextureSRVKeysUpToDate(
                                     current_texture_srv_keys_vertex_.data(),
                                     textures_vertex.data(), texture_count_vertex));
    bool write_textures_pixel =
        texture_count_pixel &&
        (!bindful_textures_written_pixel_ ||
         current_texture_layout_uid_pixel_ != texture_layout_uid_pixel ||
         !texture_cache_->AreActiveTextureSRVKeysUpToDate(
             current_texture_srv_keys_pixel_.data(), textures_pixel->data(), texture_count_pixel));
    bool write_samplers_vertex = sampler_count_vertex && !bindful_samplers_written_vertex_;
    bool write_samplers_pixel = sampler_count_pixel && !bindful_samplers_written_pixel_;
    bool edram_rov_used =
        render_target_cache_->GetPath() == RenderTargetCache::Path::kPixelShaderInterlock;

    // Allocate the descriptors.
    size_t view_count_partial_update = 0;
    if (write_textures_vertex) {
      view_count_partial_update += texture_count_vertex;
    }
    if (write_textures_pixel) {
      view_count_partial_update += texture_count_pixel;
    }
    // Shared memory SRV and null UAV + null SRV and shared memory UAV +
    // textures.
    size_t view_count_full_update = 4 + texture_count_vertex + texture_count_pixel;
    if (edram_rov_used) {
      // + EDRAM UAV in two tables (with the shared memory SRV and with the
      // shared memory UAV).
      view_count_full_update += 2;
    }
    D3D12_CPU_DESCRIPTOR_HANDLE view_cpu_handle;
    D3D12_GPU_DESCRIPTOR_HANDLE view_gpu_handle;
    uint32_t descriptor_size_view = provider.GetViewDescriptorSize();
    uint64_t view_heap_index = RequestViewBindfulDescriptors(
        draw_view_bindful_heap_index_, uint32_t(view_count_partial_update),
        uint32_t(view_count_full_update), view_cpu_handle, view_gpu_handle);
    if (view_heap_index == ui::d3d12::D3D12DescriptorHeapPool::kHeapIndexInvalid) {
      REXGPU_ERROR("Failed to allocate view descriptors");
      return false;
    }
    size_t sampler_count_partial_update = 0;
    if (write_samplers_vertex) {
      sampler_count_partial_update += sampler_count_vertex;
    }
    if (write_samplers_pixel) {
      sampler_count_partial_update += sampler_count_pixel;
    }
    D3D12_CPU_DESCRIPTOR_HANDLE sampler_cpu_handle = {};
    D3D12_GPU_DESCRIPTOR_HANDLE sampler_gpu_handle = {};
    uint32_t descriptor_size_sampler = provider.GetSamplerDescriptorSize();
    uint64_t sampler_heap_index = ui::d3d12::D3D12DescriptorHeapPool::kHeapIndexInvalid;
    if (sampler_count_vertex != 0 || sampler_count_pixel != 0) {
      sampler_heap_index = RequestSamplerBindfulDescriptors(
          draw_sampler_bindful_heap_index_, uint32_t(sampler_count_partial_update),
          uint32_t(sampler_count_vertex + sampler_count_pixel), sampler_cpu_handle,
          sampler_gpu_handle);
      if (sampler_heap_index == ui::d3d12::D3D12DescriptorHeapPool::kHeapIndexInvalid) {
        REXGPU_ERROR("Failed to allocate sampler descriptors");
        return false;
      }
    }
    if (draw_view_bindful_heap_index_ != view_heap_index) {
      // Need to update all view descriptors.
      write_textures_vertex = texture_count_vertex != 0;
      write_textures_pixel = texture_count_pixel != 0;
      bindful_textures_written_vertex_ = false;
      bindful_textures_written_pixel_ = false;
      // If updating fully, write the shared memory SRV and UAV descriptors and,
      // if needed, the EDRAM descriptor.
      // SRV + null UAV + EDRAM.
      gpu_handle_shared_memory_srv_and_edram_ = view_gpu_handle;
      shared_memory_->WriteRawSRVDescriptor(view_cpu_handle);
      view_cpu_handle.ptr += descriptor_size_view;
      view_gpu_handle.ptr += descriptor_size_view;
      ui::d3d12::util::CreateBufferRawUAV(device, view_cpu_handle, nullptr, 0);
      view_cpu_handle.ptr += descriptor_size_view;
      view_gpu_handle.ptr += descriptor_size_view;
      if (edram_rov_used) {
        render_target_cache_->WriteEdramUintPow2UAVDescriptor(view_cpu_handle, 2);
        view_cpu_handle.ptr += descriptor_size_view;
        view_gpu_handle.ptr += descriptor_size_view;
      }
      // Null SRV + UAV + EDRAM.
      gpu_handle_shared_memory_uav_and_edram_ = view_gpu_handle;
      ui::d3d12::util::CreateBufferRawSRV(device, view_cpu_handle, nullptr, 0);
      view_cpu_handle.ptr += descriptor_size_view;
      view_gpu_handle.ptr += descriptor_size_view;
      shared_memory_->WriteRawUAVDescriptor(view_cpu_handle);
      view_cpu_handle.ptr += descriptor_size_view;
      view_gpu_handle.ptr += descriptor_size_view;
      if (edram_rov_used) {
        render_target_cache_->WriteEdramUintPow2UAVDescriptor(view_cpu_handle, 2);
        view_cpu_handle.ptr += descriptor_size_view;
        view_gpu_handle.ptr += descriptor_size_view;
      }
      current_graphics_root_up_to_date_ &= ~(1u << kRootParameter_Bindful_SharedMemoryAndEdram);
    }
    if (sampler_heap_index != ui::d3d12::D3D12DescriptorHeapPool::kHeapIndexInvalid &&
        draw_sampler_bindful_heap_index_ != sampler_heap_index) {
      write_samplers_vertex = sampler_count_vertex != 0;
      write_samplers_pixel = sampler_count_pixel != 0;
      bindful_samplers_written_vertex_ = false;
      bindful_samplers_written_pixel_ = false;
    }

    // Write the descriptors.
    if (write_textures_vertex) {
      assert_true(current_graphics_root_bindful_extras_.textures_vertex !=
                  RootBindfulExtraParameterIndices::kUnavailable);
      gpu_handle_textures_vertex_ = view_gpu_handle;
      for (size_t i = 0; i < texture_count_vertex; ++i) {
        texture_cache_->WriteActiveTextureBindfulSRV(textures_vertex[i], view_cpu_handle);
        view_cpu_handle.ptr += descriptor_size_view;
        view_gpu_handle.ptr += descriptor_size_view;
      }
      current_texture_layout_uid_vertex_ = texture_layout_uid_vertex;
      current_texture_srv_keys_vertex_.resize(
          std::max(current_texture_srv_keys_vertex_.size(), size_t(texture_count_vertex)));
      texture_cache_->WriteActiveTextureSRVKeys(current_texture_srv_keys_vertex_.data(),
                                                textures_vertex.data(), texture_count_vertex);
      bindful_textures_written_vertex_ = true;
      current_graphics_root_up_to_date_ &=
          ~(1u << current_graphics_root_bindful_extras_.textures_vertex);
    }
    if (write_textures_pixel) {
      assert_true(current_graphics_root_bindful_extras_.textures_pixel !=
                  RootBindfulExtraParameterIndices::kUnavailable);
      gpu_handle_textures_pixel_ = view_gpu_handle;
      for (size_t i = 0; i < texture_count_pixel; ++i) {
        texture_cache_->WriteActiveTextureBindfulSRV((*textures_pixel)[i], view_cpu_handle);
        view_cpu_handle.ptr += descriptor_size_view;
        view_gpu_handle.ptr += descriptor_size_view;
      }
      current_texture_layout_uid_pixel_ = texture_layout_uid_pixel;
      current_texture_srv_keys_pixel_.resize(
          std::max(current_texture_srv_keys_pixel_.size(), size_t(texture_count_pixel)));
      texture_cache_->WriteActiveTextureSRVKeys(current_texture_srv_keys_pixel_.data(),
                                                textures_pixel->data(), texture_count_pixel);
      bindful_textures_written_pixel_ = true;
      current_graphics_root_up_to_date_ &=
          ~(1u << current_graphics_root_bindful_extras_.textures_pixel);
    }
    if (write_samplers_vertex) {
      assert_true(current_graphics_root_bindful_extras_.samplers_vertex !=
                  RootBindfulExtraParameterIndices::kUnavailable);
      gpu_handle_samplers_vertex_ = sampler_gpu_handle;
      for (size_t i = 0; i < sampler_count_vertex; ++i) {
        texture_cache_->WriteSampler(current_samplers_vertex_[i], sampler_cpu_handle);
        sampler_cpu_handle.ptr += descriptor_size_sampler;
        sampler_gpu_handle.ptr += descriptor_size_sampler;
      }
      // Current samplers have already been updated.
      bindful_samplers_written_vertex_ = true;
      current_graphics_root_up_to_date_ &=
          ~(1u << current_graphics_root_bindful_extras_.samplers_vertex);
    }
    if (write_samplers_pixel) {
      assert_true(current_graphics_root_bindful_extras_.samplers_pixel !=
                  RootBindfulExtraParameterIndices::kUnavailable);
      gpu_handle_samplers_pixel_ = sampler_gpu_handle;
      for (size_t i = 0; i < sampler_count_pixel; ++i) {
        texture_cache_->WriteSampler(current_samplers_pixel_[i], sampler_cpu_handle);
        sampler_cpu_handle.ptr += descriptor_size_sampler;
        sampler_gpu_handle.ptr += descriptor_size_sampler;
      }
      // Current samplers have already been updated.
      bindful_samplers_written_pixel_ = true;
      current_graphics_root_up_to_date_ &=
          ~(1u << current_graphics_root_bindful_extras_.samplers_pixel);
    }

    // Wrote new descriptors on the current page.
    draw_view_bindful_heap_index_ = view_heap_index;
    if (sampler_heap_index != ui::d3d12::D3D12DescriptorHeapPool::kHeapIndexInvalid) {
      draw_sampler_bindful_heap_index_ = sampler_heap_index;
    }
  }

  // Update the root parameters.
  if (!(current_graphics_root_up_to_date_ & (1u << root_parameter_fetch_constants))) {
    deferred_command_list_.D3DSetGraphicsRootConstantBufferView(root_parameter_fetch_constants,
                                                                cbuffer_binding_fetch_.address);
    current_graphics_root_up_to_date_ |= 1u << root_parameter_fetch_constants;
  }
  if (!(current_graphics_root_up_to_date_ & (1u << root_parameter_float_constants_vertex))) {
    deferred_command_list_.D3DSetGraphicsRootConstantBufferView(
        root_parameter_float_constants_vertex, cbuffer_binding_float_vertex_.address);
    current_graphics_root_up_to_date_ |= 1u << root_parameter_float_constants_vertex;
  }
  if (!(current_graphics_root_up_to_date_ & (1u << root_parameter_float_constants_pixel))) {
    deferred_command_list_.D3DSetGraphicsRootConstantBufferView(
        root_parameter_float_constants_pixel, cbuffer_binding_float_pixel_.address);
    current_graphics_root_up_to_date_ |= 1u << root_parameter_float_constants_pixel;
  }
  if (!(current_graphics_root_up_to_date_ & (1u << root_parameter_system_constants))) {
    deferred_command_list_.D3DSetGraphicsRootConstantBufferView(root_parameter_system_constants,
                                                                cbuffer_binding_system_.address);
    current_graphics_root_up_to_date_ |= 1u << root_parameter_system_constants;
  }
  if (!(current_graphics_root_up_to_date_ & (1u << root_parameter_bool_loop_constants))) {
    deferred_command_list_.D3DSetGraphicsRootConstantBufferView(root_parameter_bool_loop_constants,
                                                                cbuffer_binding_bool_loop_.address);
    current_graphics_root_up_to_date_ |= 1u << root_parameter_bool_loop_constants;
  }
  if (!(current_graphics_root_up_to_date_ &
        (1u << root_parameter_shared_memory_and_bindful_edram))) {
    assert_true(current_shared_memory_binding_is_uav_.has_value());
    D3D12_GPU_DESCRIPTOR_HANDLE gpu_handle_shared_memory_and_bindful_edram;
    if (bindless_resources_used_) {
      gpu_handle_shared_memory_and_bindful_edram = provider.OffsetViewDescriptor(
          view_bindless_heap_gpu_start_,
          uint32_t(current_shared_memory_binding_is_uav_.value()
                       ? SystemBindlessView ::kNullRawSRVAndSharedMemoryRawUAVStart
                       : SystemBindlessView ::kSharedMemoryRawSRVAndNullRawUAVStart));
    } else {
      gpu_handle_shared_memory_and_bindful_edram = current_shared_memory_binding_is_uav_.value()
                                                       ? gpu_handle_shared_memory_uav_and_edram_
                                                       : gpu_handle_shared_memory_srv_and_edram_;
    }
    deferred_command_list_.D3DSetGraphicsRootDescriptorTable(
        root_parameter_shared_memory_and_bindful_edram, gpu_handle_shared_memory_and_bindful_edram);
    current_graphics_root_up_to_date_ |= 1u << root_parameter_shared_memory_and_bindful_edram;
  }
  if (bindless_resources_used_) {
    if (!(current_graphics_root_up_to_date_ &
          (1u << kRootParameter_Bindless_DescriptorIndicesPixel))) {
      deferred_command_list_.D3DSetGraphicsRootConstantBufferView(
          kRootParameter_Bindless_DescriptorIndicesPixel,
          cbuffer_binding_descriptor_indices_pixel_.address);
      current_graphics_root_up_to_date_ |= 1u << kRootParameter_Bindless_DescriptorIndicesPixel;
    }
    if (!(current_graphics_root_up_to_date_ &
          (1u << kRootParameter_Bindless_DescriptorIndicesVertex))) {
      deferred_command_list_.D3DSetGraphicsRootConstantBufferView(
          kRootParameter_Bindless_DescriptorIndicesVertex,
          cbuffer_binding_descriptor_indices_vertex_.address);
      current_graphics_root_up_to_date_ |= 1u << kRootParameter_Bindless_DescriptorIndicesVertex;
    }
    if (!(current_graphics_root_up_to_date_ & (1u << kRootParameter_Bindless_SamplerHeap))) {
      deferred_command_list_.D3DSetGraphicsRootDescriptorTable(kRootParameter_Bindless_SamplerHeap,
                                                               sampler_bindless_heap_gpu_start_);
      current_graphics_root_up_to_date_ |= 1u << kRootParameter_Bindless_SamplerHeap;
    }
    if (!(current_graphics_root_up_to_date_ & (1u << kRootParameter_Bindless_ViewHeap))) {
      deferred_command_list_.D3DSetGraphicsRootDescriptorTable(kRootParameter_Bindless_ViewHeap,
                                                               view_bindless_heap_gpu_start_);
      current_graphics_root_up_to_date_ |= 1u << kRootParameter_Bindless_ViewHeap;
    }
  } else {
    uint32_t extra_index;
    extra_index = current_graphics_root_bindful_extras_.textures_pixel;
    if (extra_index != RootBindfulExtraParameterIndices::kUnavailable &&
        !(current_graphics_root_up_to_date_ & (1u << extra_index))) {
      deferred_command_list_.D3DSetGraphicsRootDescriptorTable(extra_index,
                                                               gpu_handle_textures_pixel_);
      current_graphics_root_up_to_date_ |= 1u << extra_index;
    }
    extra_index = current_graphics_root_bindful_extras_.samplers_pixel;
    if (extra_index != RootBindfulExtraParameterIndices::kUnavailable &&
        !(current_graphics_root_up_to_date_ & (1u << extra_index))) {
      deferred_command_list_.D3DSetGraphicsRootDescriptorTable(extra_index,
                                                               gpu_handle_samplers_pixel_);
      current_graphics_root_up_to_date_ |= 1u << extra_index;
    }
    extra_index = current_graphics_root_bindful_extras_.textures_vertex;
    if (extra_index != RootBindfulExtraParameterIndices::kUnavailable &&
        !(current_graphics_root_up_to_date_ & (1u << extra_index))) {
      deferred_command_list_.D3DSetGraphicsRootDescriptorTable(extra_index,
                                                               gpu_handle_textures_vertex_);
      current_graphics_root_up_to_date_ |= 1u << extra_index;
    }
    extra_index = current_graphics_root_bindful_extras_.samplers_vertex;
    if (extra_index != RootBindfulExtraParameterIndices::kUnavailable &&
        !(current_graphics_root_up_to_date_ & (1u << extra_index))) {
      deferred_command_list_.D3DSetGraphicsRootDescriptorTable(extra_index,
                                                               gpu_handle_samplers_vertex_);
      current_graphics_root_up_to_date_ |= 1u << extra_index;
    }
  }

  return true;
}

void D3D12CommandProcessor::EvictOldReadbackBuffers(
    std::unordered_map<uint64_t, ReadbackBuffer>& buffer_map) {
  if (buffer_map.empty()) {
    return;
  }
  const uint64_t eviction_frame_floor = (frame_current_ > kReadbackBufferEvictionAgeFrames)
                                            ? (frame_current_ - kReadbackBufferEvictionAgeFrames)
                                            : 0;
  for (auto it = buffer_map.begin(); it != buffer_map.end();) {
    ReadbackBuffer& readback = it->second;
    bool evict =
        buffer_map.size() > kMaxReadbackBuffers || readback.last_used_frame < eviction_frame_floor;
    if (!evict) {
      ++it;
      continue;
    }
    for (uint32_t i = 0; i < 2; ++i) {
      if (readback.buffers[i]) {
        if (readback.mapped_data[i]) {
          readback.buffers[i]->Unmap(0, nullptr);
        }
        readback.buffers[i]->Release();
      }
      readback.buffers[i] = nullptr;
      readback.mapped_data[i] = nullptr;
      readback.sizes[i] = 0;
      readback.submission_written[i] = 0;
      readback.written_size[i] = 0;
    }
    it = buffer_map.erase(it);
  }
}

ID3D12Resource* D3D12CommandProcessor::RequestReadbackBuffer(uint32_t size) {
  if (size == 0) {
    return nullptr;
  }
  size = rex::align(size, kReadbackBufferSizeIncrement);
  if (size > readback_buffer_size_) {
    const ui::d3d12::D3D12Provider& provider = GetD3D12Provider();
    ID3D12Device* device = provider.GetDevice();
    D3D12_RESOURCE_DESC buffer_desc;
    ui::d3d12::util::FillBufferResourceDesc(buffer_desc, size, D3D12_RESOURCE_FLAG_NONE);
    ID3D12Resource* buffer;
    if (FAILED(device->CreateCommittedResource(
            &ui::d3d12::util::kHeapPropertiesReadback, provider.GetHeapFlagCreateNotZeroed(),
            &buffer_desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&buffer)))) {
      REXGPU_ERROR("Failed to create a {} MB readback buffer", size >> 20);
      return nullptr;
    }
    if (readback_buffer_ != nullptr) {
      readback_buffer_->Release();
    }
    readback_buffer_ = buffer;
    readback_buffer_size_ = size;
  }
  return readback_buffer_;
}

bool D3D12CommandProcessor::InitializeEmbeddedSceneHostVertexOutputResources() {
  ShutdownEmbeddedSceneHostVertexOutputResources();

  ID3D12Device* device = GetD3D12Provider().GetDevice();
  if (!device) {
    return false;
  }

  D3D12_RESOURCE_DESC output_desc;
  ui::d3d12::util::FillBufferResourceDesc(
      output_desc, kEmbeddedSceneHostVertexOutputRecordStride,
      D3D12_RESOURCE_FLAG_NONE);
  if (FAILED(device->CreateCommittedResource(
          &ui::d3d12::util::kHeapPropertiesDefault,
          GetD3D12Provider().GetHeapFlagCreateNotZeroed(), &output_desc,
          D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
          IID_PPV_ARGS(&embedded_scene_host_vertex_output_buffer_)))) {
    REXGPU_WARN(
        "D3D12CommandProcessor: Failed to allocate embedded scene host vertex output buffer");
    ShutdownEmbeddedSceneHostVertexOutputResources();
    return false;
  }
  embedded_scene_host_vertex_output_buffer_->SetName(
      L"Embedded Scene Host Vertex Output");
  embedded_scene_host_vertex_output_buffer_state_ =
      D3D12_RESOURCE_STATE_COPY_DEST;

  D3D12_RESOURCE_DESC zero_desc;
  ui::d3d12::util::FillBufferResourceDesc(zero_desc, sizeof(uint64_t),
                                          D3D12_RESOURCE_FLAG_NONE);
  if (FAILED(device->CreateCommittedResource(
          &ui::d3d12::util::kHeapPropertiesUpload, D3D12_HEAP_FLAG_NONE,
          &zero_desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
          IID_PPV_ARGS(&embedded_scene_host_vertex_output_zero_upload_)))) {
    REXGPU_WARN(
        "D3D12CommandProcessor: Failed to allocate embedded scene host vertex output counter upload");
    ShutdownEmbeddedSceneHostVertexOutputResources();
    return false;
  }
  void* zero_mapping = nullptr;
  D3D12_RANGE no_read_range = {0, 0};
  if (FAILED(embedded_scene_host_vertex_output_zero_upload_->Map(
          0, &no_read_range, &zero_mapping))) {
    REXGPU_WARN(
        "D3D12CommandProcessor: Failed to map embedded scene host vertex output counter upload");
    ShutdownEmbeddedSceneHostVertexOutputResources();
    return false;
  }
  *reinterpret_cast<uint64_t*>(zero_mapping) = 0;
  const D3D12_RANGE zero_write_range = {0, sizeof(uint64_t)};
  embedded_scene_host_vertex_output_zero_upload_->Unmap(0,
                                                         &zero_write_range);

  D3D12_RESOURCE_DESC readback_desc;
  ui::d3d12::util::FillBufferResourceDesc(
      readback_desc,
      uint64_t(kEmbeddedSceneHostVertexOutputRecordStride) *
          kEmbeddedSceneHostVertexOutputRecordCount,
      D3D12_RESOURCE_FLAG_NONE);
  if (FAILED(device->CreateCommittedResource(
          &ui::d3d12::util::kHeapPropertiesReadback,
          GetD3D12Provider().GetHeapFlagCreateNotZeroed(), &readback_desc,
          D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
          IID_PPV_ARGS(&embedded_scene_host_vertex_output_readback_)))) {
    REXGPU_WARN(
        "D3D12CommandProcessor: Failed to allocate embedded scene host vertex output readback");
    ShutdownEmbeddedSceneHostVertexOutputResources();
    return false;
  }
  const D3D12_RANGE readback_range = {
      0, SIZE_T(kEmbeddedSceneHostVertexOutputRecordStride) *
             kEmbeddedSceneHostVertexOutputRecordCount};
  void* readback_mapping = nullptr;
  if (FAILED(embedded_scene_host_vertex_output_readback_->Map(
          0, &readback_range, &readback_mapping))) {
    REXGPU_WARN(
        "D3D12CommandProcessor: Failed to map embedded scene host vertex output readback");
    ShutdownEmbeddedSceneHostVertexOutputResources();
    return false;
  }
  embedded_scene_host_vertex_output_readback_mapping_ =
      reinterpret_cast<uint8_t*>(readback_mapping);
  embedded_scene_host_vertex_output_record_count_ = 0;
  embedded_scene_host_vertex_output_records_ = {};
  return true;
}

void D3D12CommandProcessor::ShutdownEmbeddedSceneHostVertexOutputResources() {
  if (embedded_scene_host_vertex_output_readback_ &&
      embedded_scene_host_vertex_output_readback_mapping_) {
    embedded_scene_host_vertex_output_readback_->Unmap(0, nullptr);
  }
  embedded_scene_host_vertex_output_readback_mapping_ = nullptr;
  embedded_scene_host_vertex_output_readback_.Reset();
  embedded_scene_host_vertex_output_zero_upload_.Reset();
  embedded_scene_host_vertex_output_buffer_.Reset();
  embedded_scene_host_vertex_output_buffer_state_ =
      D3D12_RESOURCE_STATE_COPY_DEST;
  embedded_scene_host_vertex_output_resources_available_ = false;
  embedded_scene_host_vertex_output_record_count_ = 0;
  embedded_scene_host_vertex_output_records_ = {};
}

bool D3D12CommandProcessor::InitializeOcclusionQueryResources() {
  zpd_submit_after_draws_ = REXCVAR_GET(d3d12_zpd_submit_after_draws);
  zpd_early_submission_logged_ = false;
  zpd_early_end_count_ = 0;
  zpd_early_max_segment_draws_ = 0;
  if (zpd_submit_after_draws_) {
    std::fprintf(stderr, "REX_ZPD_EARLY_CONFIG after_draws=%u\n",
                 zpd_submit_after_draws_);
    std::fflush(stderr);
  }
  logical_occlusion_query_ = {};
  active_occlusion_query_ = {};
  occlusion_reports_.Clear();
  occlusion_query_slot_values_.clear();
  occlusion_query_cursor_ = 0;
  occlusion_query_resources_available_ = false;
  occlusion_query_heap_.Reset();
  occlusion_query_readback_.Reset();
  occlusion_query_readback_mapping_ = nullptr;
  zpd_gpu_timestamp_heap_.Reset();
  zpd_gpu_timestamp_readback_.Reset();
  zpd_gpu_timestamp_mapping_ = nullptr;
  zpd_gpu_timestamp_frequency_ = 0;
  zpd_gpu_timestamp_active_ = false;
  zpd_gpu_timestamp_pending_ = false;
  zpd_gpu_timestamp_submission_ = 0;
  zpd_gpu_calibration_window_ = 0;
  zpd_gpu_calibration_valid_ = false;
  zpd_gpu_submit_return_qpc_ = 0;
  zpd_gpu_qpc_frequency_ = 0;

  ID3D12Device* device = GetD3D12Provider().GetDevice();
  if (!device) {
    return false;
  }

  D3D12_QUERY_HEAP_DESC heap_desc;
  heap_desc.Type = D3D12_QUERY_HEAP_TYPE_OCCLUSION;
  heap_desc.Count = kMaxOcclusionQueries;
  heap_desc.NodeMask = 0;
  if (FAILED(device->CreateQueryHeap(&heap_desc, IID_PPV_ARGS(&occlusion_query_heap_)))) {
    REXGPU_WARN(
        "D3D12CommandProcessor: Failed to create occlusion query heap, using fake sample counts");
    return false;
  }

  D3D12_RESOURCE_DESC buffer_desc;
  ui::d3d12::util::FillBufferResourceDesc(buffer_desc, sizeof(uint64_t) * kMaxOcclusionQueries,
                                          D3D12_RESOURCE_FLAG_NONE);
  if (FAILED(device->CreateCommittedResource(&ui::d3d12::util::kHeapPropertiesReadback,
                                             GetD3D12Provider().GetHeapFlagCreateNotZeroed(),
                                             &buffer_desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                             IID_PPV_ARGS(&occlusion_query_readback_)))) {
    REXGPU_WARN(
        "D3D12CommandProcessor: Failed to allocate occlusion query readback buffer, using fake "
        "sample counts");
    occlusion_query_heap_.Reset();
    return false;
  }

  D3D12_RANGE read_range = {0, sizeof(uint64_t) * kMaxOcclusionQueries};
  void* mapping = nullptr;
  if (FAILED(occlusion_query_readback_->Map(0, &read_range, &mapping))) {
    REXGPU_WARN(
        "D3D12CommandProcessor: Failed to map occlusion query readback buffer, using fake sample "
        "counts");
    occlusion_query_readback_.Reset();
    occlusion_query_heap_.Reset();
    return false;
  }

  occlusion_query_readback_mapping_ = reinterpret_cast<uint64_t*>(mapping);
  occlusion_query_resources_available_ = true;
  if (REXCVAR_GET(embedded_zpd_gpu_timing)) {
    LARGE_INTEGER qpc_frequency;
    if (QueryPerformanceFrequency(&qpc_frequency)) {
      zpd_gpu_qpc_frequency_ = qpc_frequency.QuadPart;
    }
    ID3D12CommandQueue* queue = GetD3D12Provider().GetDirectQueue();
    if (queue && SUCCEEDED(queue->GetTimestampFrequency(
                     &zpd_gpu_timestamp_frequency_)) &&
        zpd_gpu_timestamp_frequency_) {
      D3D12_QUERY_HEAP_DESC timestamp_heap_desc = {};
      timestamp_heap_desc.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
      timestamp_heap_desc.Count = 2;
      if (SUCCEEDED(device->CreateQueryHeap(
              &timestamp_heap_desc, IID_PPV_ARGS(&zpd_gpu_timestamp_heap_)))) {
        D3D12_RESOURCE_DESC timestamp_buffer_desc;
        ui::d3d12::util::FillBufferResourceDesc(
            timestamp_buffer_desc, sizeof(uint64_t) * 2,
            D3D12_RESOURCE_FLAG_NONE);
        if (SUCCEEDED(device->CreateCommittedResource(
                &ui::d3d12::util::kHeapPropertiesReadback,
                GetD3D12Provider().GetHeapFlagCreateNotZeroed(),
                &timestamp_buffer_desc, D3D12_RESOURCE_STATE_COPY_DEST,
                nullptr, IID_PPV_ARGS(&zpd_gpu_timestamp_readback_)))) {
          D3D12_RANGE timestamp_range = {0, sizeof(uint64_t) * 2};
          void* timestamp_mapping = nullptr;
          if (SUCCEEDED(zpd_gpu_timestamp_readback_->Map(
                  0, &timestamp_range, &timestamp_mapping))) {
            zpd_gpu_timestamp_mapping_ =
                reinterpret_cast<uint64_t*>(timestamp_mapping);
          }
        }
      }
    }
    std::fprintf(stderr, "REX_ZPD_GPU_TIMING available=%u frequency=%llu\n",
                 zpd_gpu_timestamp_mapping_ ? 1u : 0u,
                 static_cast<unsigned long long>(zpd_gpu_timestamp_frequency_));
    std::fflush(stderr);
    if (!zpd_gpu_timestamp_mapping_) {
      zpd_gpu_timestamp_readback_.Reset();
      zpd_gpu_timestamp_heap_.Reset();
      zpd_gpu_timestamp_frequency_ = 0;
    }
  }
  return true;
}

void D3D12CommandProcessor::ShutdownOcclusionQueryResources() {
  DisableHostOcclusionQueries();

  if (occlusion_query_readback_ && occlusion_query_readback_mapping_) {
    occlusion_query_readback_->Unmap(0, nullptr);
  }
  occlusion_query_readback_mapping_ = nullptr;
  occlusion_query_readback_.Reset();
  occlusion_query_heap_.Reset();
  if (zpd_gpu_timestamp_readback_ && zpd_gpu_timestamp_mapping_) {
    zpd_gpu_timestamp_readback_->Unmap(0, nullptr);
  }
  zpd_gpu_timestamp_mapping_ = nullptr;
  zpd_gpu_timestamp_readback_.Reset();
  zpd_gpu_timestamp_heap_.Reset();
  zpd_gpu_timestamp_frequency_ = 0;
  zpd_gpu_timestamp_active_ = false;
  zpd_gpu_timestamp_pending_ = false;
  zpd_gpu_timestamp_submission_ = 0;
  zpd_gpu_calibration_window_ = 0;
  zpd_gpu_calibration_valid_ = false;
  zpd_gpu_submit_return_qpc_ = 0;
  zpd_gpu_qpc_frequency_ = 0;
}

bool D3D12CommandProcessor::AcquireOcclusionQueryIndex(uint32_t& host_index_out) {
  if (occlusion_query_cursor_ >= kMaxOcclusionQueries) {
    occlusion_query_cursor_ = 0;
  }
  const uint32_t host_index = occlusion_query_cursor_;
  // Don't overwrite a readback slot that still belongs to a segment in flight.
  const uint64_t await_submission =
      occlusion_reports_.AwaitSubmissionForHostIndex(host_index);
  if (kGpuDiagnostics && await_submission) {
    swap_intervals_.ZpdAwait(
        2, await_submission >= submission_current_ ||
               await_submission > submission_fence_->GetCompletedValue());
  }
  if (await_submission && (!RetireGuestOcclusionQuerySegments(await_submission) ||
                           occlusion_reports_.AwaitSubmissionForHostIndex(host_index))) {
    return false;
  }
  ++occlusion_query_cursor_;
  host_index_out = host_index;
  return true;
}

void D3D12CommandProcessor::DisableHostOcclusionQueries() {
  if (active_occlusion_query_.valid && occlusion_query_heap_) {
    if (CloseGuestOcclusionQuerySegment() && submission_open_) {
      EndSubmission(false);
    }
  }
  if (!occlusion_reports_.empty() && occlusion_query_readback_mapping_) {
    // Publish every report the guest has already ended.
    RetireGuestOcclusionQuerySegments(UINT64_MAX);
  }
  logical_occlusion_query_ = {};
  active_occlusion_query_ = {};
  occlusion_reports_.Clear();
  occlusion_query_slot_values_.clear();
  occlusion_query_cursor_ = 0;
  occlusion_query_resources_available_ = false;
}

bool D3D12CommandProcessor::BeginGuestOcclusionQuery(uint32_t sample_count_address) {
  if (!REXCVAR_GET(occlusion_query_enable) || !occlusion_query_resources_available_ ||
      logical_occlusion_query_.valid ||
      !XenosZPDReport::IsBeginRecord(sample_count_address)) {
    return false;
  }
  // Publish whatever has completed; nothing here waits for the host GPU.
  // The caller has already published any earlier report of this slot.
  if (!RetireGuestOcclusionQuerySegments(0)) {
    return false;
  }

  const uint32_t slot_base = XenosZPDReport::GetSlotBase(sample_count_address);
  if (!logical_occlusion_query_.Begin(
          sample_count_address, occlusion_query_slot_values_[slot_base])) {
    return false;
  }
  occlusion_reports_.Open();
  zpd_early_max_segment_draws_ = 0;
  if (!kernel_state_ && IsEmbeddedFrameDiagnosticsEnabled()) {
    ++embedded_frame_frontier.zpd_begins;
    embedded_frame_frontier.zpd_last_slot = slot_base;
    embedded_frame_frontier.zpd_last_begin_value =
        logical_occlusion_query_.begin_value;
  }

  if (!BeginGuestOcclusionQuerySegment()) {
    logical_occlusion_query_ = {};
    occlusion_reports_.DiscardOpen();
    return false;
  }
  return true;
}

bool D3D12CommandProcessor::BeginGuestOcclusionQuerySegment() {
  if (!logical_occlusion_query_.valid || active_occlusion_query_.valid ||
      !occlusion_query_heap_ ||
      !occlusion_query_readback_) {
    return false;
  }

  uint32_t host_index = 0;
  if (!AcquireOcclusionQueryIndex(host_index)) {
    return false;
  }
  if (!BeginSubmission(true)) {
    return false;
  }

  if (kGpuDiagnostics && zpd_gpu_timestamp_mapping_ && cp_cadence_.active &&
      !zpd_gpu_timestamp_active_ && !zpd_gpu_timestamp_pending_) {
    if (zpd_gpu_calibration_window_ != cp_cadence_.begin_tick) {
      zpd_gpu_calibration_window_ = cp_cadence_.begin_tick;
      zpd_gpu_calibration_valid_ = false;
    }
    // Only sample a segment in an existing sparse CP interval. Query slot 0
    // is not reused until the previous sampled segment has been retired.
    deferred_command_list_.D3DEndQuery(
        zpd_gpu_timestamp_heap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0);
    zpd_gpu_timestamp_active_ = true;
  }
  deferred_command_list_.D3DBeginQuery(occlusion_query_heap_.Get(), D3D12_QUERY_TYPE_OCCLUSION,
                                       host_index);
  active_occlusion_query_.host_index = host_index;
  uint64_t scale_x = render_target_cache_ ? render_target_cache_->GetDrawScaleX() : 1;
  uint64_t scale_y = render_target_cache_ ? render_target_cache_->GetDrawScaleY() : 1;
  uint64_t scale_area = std::max<uint64_t>(1, scale_x * scale_y);
  active_occlusion_query_.scale_area =
      scale_area > UINT32_MAX ? UINT32_MAX : uint32_t(scale_area);
  active_occlusion_query_.valid = true;
  if (!kernel_state_ && IsEmbeddedFrameDiagnosticsEnabled()) {
    ++embedded_frame_frontier.zpd_segments_opened;
  }
  return true;
}

bool D3D12CommandProcessor::UpdateGuestOcclusionQueryScale(uint32_t scale_area) {
  scale_area = std::max(scale_area, uint32_t(1));
  if (!logical_occlusion_query_.valid || !active_occlusion_query_.valid ||
      active_occlusion_query_.scale_area == scale_area) {
    return true;
  }
  // Host query samples from native and scaled draws cannot be divided by one
  // common area. Preserve the logical guest report but split its host segment
  // at the scale transition so each pending result carries one normalization.
  if (!CloseGuestOcclusionQuerySegment()) {
    return false;
  }
  return BeginGuestOcclusionQuerySegment();
}

bool D3D12CommandProcessor::CloseGuestOcclusionQuerySegment() {
  if (!active_occlusion_query_.valid) {
    return true;
  }
  if (!submission_open_ || !occlusion_query_heap_ ||
      !occlusion_query_readback_) {
    return false;
  }

  const uint32_t host_index = active_occlusion_query_.host_index;
  deferred_command_list_.D3DEndQuery(occlusion_query_heap_.Get(), D3D12_QUERY_TYPE_OCCLUSION,
                                     host_index);
  if (kGpuDiagnostics && zpd_gpu_timestamp_active_) {
    deferred_command_list_.D3DEndQuery(
        zpd_gpu_timestamp_heap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 1);
  }
  deferred_command_list_.D3DResolveQueryData(
      occlusion_query_heap_.Get(), D3D12_QUERY_TYPE_OCCLUSION, host_index, 1,
      occlusion_query_readback_.Get(), sizeof(uint64_t) * host_index);
  if (kGpuDiagnostics && zpd_gpu_timestamp_active_) {
    deferred_command_list_.D3DResolveQueryData(
        zpd_gpu_timestamp_heap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, 2,
        zpd_gpu_timestamp_readback_.Get(), 0);
    zpd_gpu_timestamp_active_ = false;
    zpd_gpu_timestamp_pending_ = true;
    zpd_gpu_timestamp_submission_ = submission_current_;
  }

  // EndSubmission signals this exact value before incrementing it.
  occlusion_reports_.AddSegment(host_index, active_occlusion_query_.scale_area,
                                submission_current_);
  active_occlusion_query_ = {};
  if (!kernel_state_ && IsEmbeddedFrameDiagnosticsEnabled()) {
    ++embedded_frame_frontier.zpd_segments_closed;
  }
  return true;
}

void D3D12CommandProcessor::BoundGuestOcclusionReportLag() {
  // Xenos writes a report when the GPU passes its END, a short way behind the
  // command processor; here the host GPU may trail up to kQueueFrames frames,
  // and a title polling from a small ring (The Darkness: five queries) then
  // reissues slots whose results never arrived. Waiting at the swap only for
  // the submission holding reports that ended max_lag swaps ago keeps V279's
  // asynchrony within a frame or two while the GPU keeps the newer frames
  // queued (no wait at all when the host GPU is ahead).
  const uint32_t max_lag = REXCVAR_GET(d3d12_zpd_max_publish_lag_frames);
  if (!max_lag || occlusion_reports_.ended.empty()) {
    return;
  }
  const uint64_t swap = guest_frame_count_.load(std::memory_order_relaxed);
  if (swap < max_lag) {
    return;
  }
  const uint64_t await_submission = occlusion_reports_.AwaitSubmissionForTag(swap - max_lag);
  if (!await_submission) {
    return;
  }
#if REX_GPU_DIAGNOSTICS
  const bool waits = await_submission > submission_completed_;
  const uint64_t wait_begin = waits ? rex::chrono::Clock::QueryHostTickCount() : 0;
#endif
  if (!RetireGuestOcclusionQuerySegments(await_submission)) {
    DisableHostOcclusionQueries();
  }
#if REX_GPU_DIAGNOSTICS
  if (waits) {
    ++ZpdDiag().bound_waits;
    ZpdDiag().bound_wait_ticks += rex::chrono::Clock::QueryHostTickCount() - wait_begin;
  }
#endif
}

bool D3D12CommandProcessor::RetireGuestOcclusionQuerySegments(
    uint64_t await_submission) {
  if (occlusion_reports_.empty()) {
    return true;
  }
  SparseZpdWorkScope retire_scope(cp_cadence_, 1);
  if (!occlusion_query_readback_mapping_) {
    return false;
  }
  if (await_submission == UINT64_MAX) {
    await_submission = occlusion_reports_.segments.empty()
                           ? 0
                           : occlusion_reports_.segments.back().submission;
  }

  {
    SparseZpdWorkScope fence_scope(cp_cadence_, 2);
    zpd_fence_check_active_ = kGpuDiagnostics && cp_cadence_.active;
    CheckSubmissionFence(await_submission);
    zpd_fence_check_active_ = false;
  }
  LARGE_INTEGER retire_qpc = {};
  if (kGpuDiagnostics && zpd_gpu_timestamp_pending_ && zpd_gpu_calibration_valid_) {
    QueryPerformanceCounter(&retire_qpc);
  }
  occlusion_reports_.Retire(
      submission_completed_, logical_occlusion_query_,
      [this](const XenosZPDDeferredReports::Segment& segment) {
        const uint64_t raw_samples =
            occlusion_query_readback_mapping_[segment.host_index];
        const uint64_t samples =
            NormalizeOcclusionSamples(raw_samples, segment.scale_area);
        if (!kernel_state_ && IsEmbeddedFrameDiagnosticsEnabled()) {
          ++embedded_frame_frontier.zpd_segments_retired;
          embedded_frame_frontier.zpd_raw_samples += raw_samples;
          embedded_frame_frontier.zpd_normalized_samples += samples;
        }
#if REX_GPU_DIAGNOSTICS
        {
          ZpdDiagStats& stats = ZpdDiag();
          if (segment.scale_area > 1) {
            ++stats.segments_scaled;
            stats.raw_scaled += raw_samples;
            stats.normalized_scaled += samples;
          } else {
            ++stats.segments_native;
            stats.raw_native += raw_samples;
          }
        }
#endif
        return samples;
      },
      [this](const XenosZPDDeferredReports::EndedReport& ended) {
        PublishGuestOcclusionReport(ended.report);
        if (kGpuDiagnostics) {
          const uint64_t swap = guest_frame_count_.load(std::memory_order_relaxed);
          swap_intervals_.ZpdPublished(swap >= ended.tag ? swap - ended.tag : 0,
                                       occlusion_reports_.ended.size() - 1);
        }
#if REX_GPU_DIAGNOSTICS
        {
          ZpdDiagStats& stats = ZpdDiag();
          const uint64_t swap = guest_frame_count_.load(std::memory_order_relaxed);
          const uint64_t lag = swap >= ended.tag ? swap - ended.tag : 0;
          const uint64_t delta = ended.report.accumulated_samples;
          ++stats.reports;
          stats.zero_reports += delta ? 0 : 1;
          stats.delta_sum += delta;
          stats.delta_max = std::max(stats.delta_max, delta);
          stats.lag_sum += lag;
          stats.lag_max = std::max(stats.lag_max, lag);
          stats.queue_max = std::max<uint64_t>(stats.queue_max, occlusion_reports_.ended.size());
          uint32_t slot = 0;
          while (slot < stats.slot_count && stats.slot_base[slot] != ended.report.slot_base) ++slot;
          if (slot == stats.slot_count && slot < ZpdDiagStats::kSlots) {
            stats.slot_base[slot] = ended.report.slot_base;
            ++stats.slot_count;
          }
          if (slot < stats.slot_count) {
            ++stats.slot_reports[slot];
            stats.slot_delta_sum[slot] += delta;
          }
          if (!stats.window_frame) stats.window_frame = swap;
          if (swap >= stats.window_frame + 300) {
            char slots[ZpdDiagStats::kSlots * 32] = {};
            size_t used = 0;
            for (uint32_t i = 0; i < stats.slot_count && used + 32 < sizeof(slots); ++i) {
              used += std::snprintf(slots + used, sizeof(slots) - used, "%s%08X:%llu:%llu",
                                    i ? "," : "", stats.slot_base[i],
                                    static_cast<unsigned long long>(stats.slot_reports[i]),
                                    static_cast<unsigned long long>(
                                        stats.slot_reports[i]
                                            ? stats.slot_delta_sum[i] / stats.slot_reports[i]
                                            : 0));
            }
            std::fprintf(
                stderr,
                "REX_ZPD_STATS frames=%llu reports=%llu zero=%llu delta_avg=%llu "
                "delta_max=%llu lag_avg_x100=%llu lag_max=%llu queue_max=%llu "
                "superseded=%llu bound_waits=%llu bound_wait_us=%llu "
                "segments_native=%llu segments_scaled=%llu raw_native=%llu "
                "raw_scaled=%llu normalized_scaled=%llu slots=%s\n",
                static_cast<unsigned long long>(swap - stats.window_frame),
                static_cast<unsigned long long>(stats.reports),
                static_cast<unsigned long long>(stats.zero_reports),
                static_cast<unsigned long long>(stats.reports ? stats.delta_sum / stats.reports : 0),
                static_cast<unsigned long long>(stats.delta_max),
                static_cast<unsigned long long>(stats.reports ? stats.lag_sum * 100 / stats.reports
                                                              : 0),
                static_cast<unsigned long long>(stats.lag_max),
                static_cast<unsigned long long>(stats.queue_max),
                static_cast<unsigned long long>(stats.superseded),
                static_cast<unsigned long long>(stats.bound_waits),
                static_cast<unsigned long long>(
                    stats.bound_wait_ticks * 1000000 /
                    std::max<uint64_t>(1, rex::chrono::Clock::QueryHostTickFrequency())),
                static_cast<unsigned long long>(stats.segments_native),
                static_cast<unsigned long long>(stats.segments_scaled),
                static_cast<unsigned long long>(stats.raw_native),
                static_cast<unsigned long long>(stats.raw_scaled),
                static_cast<unsigned long long>(stats.normalized_scaled), slots);
            std::fflush(stderr);
            stats = {};
            stats.window_frame = swap;
          }
        }
#endif
      });
  if (kGpuDiagnostics && zpd_gpu_timestamp_pending_ &&
      zpd_gpu_timestamp_submission_ <= submission_completed_) {
    const uint64_t begin_tick = zpd_gpu_timestamp_mapping_[0];
    const uint64_t end_tick = zpd_gpu_timestamp_mapping_[1];
    if (end_tick >= begin_tick) {
      cp_cadence_.OcclusionGpuTime(end_tick - begin_tick,
                                   zpd_gpu_timestamp_frequency_);
      if (zpd_gpu_calibration_valid_ && zpd_gpu_submit_return_qpc_ &&
          zpd_gpu_qpc_frequency_ && retire_qpc.QuadPart) {
        const double qpc_per_gpu_tick =
            double(zpd_gpu_qpc_frequency_) /
            double(zpd_gpu_timestamp_frequency_);
        const double gpu_begin_qpc =
            double(zpd_gpu_calibration_cpu_tick_) +
            (double(begin_tick) - double(zpd_gpu_calibration_gpu_tick_)) *
                qpc_per_gpu_tick;
        const double gpu_end_qpc =
            double(zpd_gpu_calibration_cpu_tick_) +
            (double(end_tick) - double(zpd_gpu_calibration_gpu_tick_)) *
                qpc_per_gpu_tick;
        const double submit_to_begin_ms =
            (gpu_begin_qpc - double(zpd_gpu_submit_return_qpc_)) *
            1000.0 / double(zpd_gpu_qpc_frequency_);
        const double end_to_retire_ms =
            (double(retire_qpc.QuadPart) - gpu_end_qpc) *
            1000.0 / double(zpd_gpu_qpc_frequency_);
        if (submit_to_begin_ms > -1000.0 &&
            submit_to_begin_ms < 1000.0 &&
            end_to_retire_ms > -1000.0 &&
            end_to_retire_ms < 1000.0) {
          cp_cadence_.OcclusionGpuQueueTime(submit_to_begin_ms,
                                             end_to_retire_ms);
        }
      }
    }
    zpd_gpu_timestamp_pending_ = false;
    zpd_gpu_submit_return_qpc_ = 0;
  }
  return occlusion_reports_.segments.empty() ||
         occlusion_reports_.segments.front().submission > await_submission;
}

void D3D12CommandProcessor::PublishGuestOcclusionReport(
    const XenosZPDReportAccumulator& report) {
  SettleUploadCopies();
  auto* end_counts = memory_->TranslatePhysical<
      xenos::xe_gpu_depth_sample_counts*>(report.end_record);
  auto* begin_counts = memory_->TranslatePhysical<
      xenos::xe_gpu_depth_sample_counts*>(report.begin_record);
  if (!end_counts || !begin_counts) {
    return;
  }
  const uint32_t begin_value = report.begin_value;
  {
    // Both records lie in the report's slot; guarded like every other guest
    // write by the command processor (host write, invalidates GPU copies).
    auto host_write = memory_->GuardPhysicalWrite(report.slot_base, XenosZPDReport::kSlotSizeBytes);
    XenosZPDReport::PublishReportDelta(begin_counts, end_counts, begin_value,
                                       report.accumulated_samples);
  }
  const uint32_t clamped_delta =
      XenosZPDReport::ClampSampleCount(report.accumulated_samples);
  occlusion_query_slot_values_[report.slot_base] =
      begin_value > UINT32_MAX - clamped_delta ? UINT32_MAX
                                               : begin_value + clamped_delta;
  if (!kernel_state_ && IsEmbeddedFrameDiagnosticsEnabled()) {
    ++embedded_frame_frontier.zpd_ends;
    embedded_frame_frontier.zpd_last_slot = report.slot_base;
    embedded_frame_frontier.zpd_last_begin_value = begin_value;
    embedded_frame_frontier.zpd_last_end_value =
        occlusion_query_slot_values_[report.slot_base];
  }
}

bool D3D12CommandProcessor::AwaitGuestOcclusionReportsInRange(uint32_t address,
                                                              uint32_t bytes,
                                                              uint32_t kind) {
  if (!occlusion_reports_.HasEndedReportInRange(address, bytes)) {
    return true;
  }
  const uint64_t await_submission =
      occlusion_reports_.AwaitSubmissionForRange(address, bytes);
  if (kGpuDiagnostics) {
    swap_intervals_.ZpdAwait(
        kind, await_submission >= submission_current_ ||
                  await_submission > submission_fence_->GetCompletedValue());
  }
  if (!RetireGuestOcclusionQuerySegments(await_submission)) {
    return false;
  }
  return !occlusion_reports_.HasEndedReportInRange(address, bytes);
}

void D3D12CommandProcessor::PrepareForWait() {
  CommandProcessor::PrepareForWait();
  if (occlusion_reports_.ended.empty()) {
    return;
  }
  // The guest keeps polling ended reports while the command processor waits.
  // Submit the recorded work containing their segments so the host GPU can
  // finish them, then publish every result that has completed.
  if (submission_open_ &&
      occlusion_reports_.HasSegmentInSubmission(submission_current_) &&
      !scratch_buffer_used_ && CanEndSubmissionImmediately()) {
    EndSubmission(false);
  }
  RetireGuestOcclusionQuerySegments(0);
}

void D3D12CommandProcessor::SettleUploadCopies() {
  if (!shared_memory_) return;
  const uint64_t ticket = shared_memory_->QueuedUploadCopies();
  // Settle counters feed the swap-interval report only (measurement builds).
  if (kGpuDiagnostics) ++upload_settle_calls_;
  if (!ticket || shared_memory_->CompletedUploadCopies() >= ticket) return;
  if (kGpuDiagnostics) ++upload_settle_blocked_;
  const uint64_t begin = kGpuDiagnostics ? rex::chrono::Clock::QueryHostTickCount() : 0;
  shared_memory_->WaitForUploadCopies(ticket);
  if (kGpuDiagnostics) {
    upload_settle_blocked_ticks_ += rex::chrono::Clock::QueryHostTickCount() - begin;
  }
}

std::string D3D12CommandProcessor::SwapIntervalBackendStats() {
  const uint64_t frequency = rex::chrono::Clock::QueryHostTickFrequency();
  char line[256];
  std::snprintf(line, sizeof(line),
                "REX_UPLOAD_COPY_STATS settle_calls=%llu settle_blocked=%llu "
                "settle_blocked_us=%llu queued=%llu completed=%llu",
                static_cast<unsigned long long>(upload_settle_calls_),
                static_cast<unsigned long long>(upload_settle_blocked_),
                static_cast<unsigned long long>(
                    frequency ? upload_settle_blocked_ticks_ * 1000000ull / frequency : 0),
                static_cast<unsigned long long>(
                    shared_memory_ ? shared_memory_->QueuedUploadCopies() : 0),
                static_cast<unsigned long long>(
                    shared_memory_ ? shared_memory_->CompletedUploadCopies() : 0));
  upload_settle_calls_ = 0;
  upload_settle_blocked_ = 0;
  upload_settle_blocked_ticks_ = 0;
  return line;
}

void D3D12CommandProcessor::PrepareForPacketMemoryWrite(uint32_t address,
                                                        uint32_t bytes) {
  SettleUploadCopies();
  if (occlusion_reports_.ended.empty()) {
    return;
  }
  if (!AwaitGuestOcclusionReportsInRange(address, bytes, 1)) {
    DisableHostOcclusionQueries();
  }
}

bool D3D12CommandProcessor::EndGuestOcclusionQuery(
    uint32_t sample_count_address,
    xenos::xe_gpu_depth_sample_counts* sample_counts) {
  if (!REXCVAR_GET(occlusion_query_enable) ||
      !occlusion_query_resources_available_ ||
      !logical_occlusion_query_.CanEnd(sample_count_address) ||
      !occlusion_query_heap_ || !occlusion_query_readback_) {
    return false;
  }

  if (zpd_submit_after_draws_) {
    const uint32_t end_count = ++zpd_early_end_count_;
    if (end_count <= 8 || !(end_count % 1024)) {
      std::fprintf(stderr,
                   "REX_ZPD_EARLY_END count=%u threshold=%u max_draws=%u "
                   "active_draws=%u scale_area=%u pending_segments=%zu\n",
                   end_count, zpd_submit_after_draws_,
                   zpd_early_max_segment_draws_, active_occlusion_query_.draws,
                   active_occlusion_query_.scale_area,
                   occlusion_reports_.segments.size());
      std::fflush(stderr);
    }
  }

  const uint32_t end_record = XenosZPDReport::GetRecordBase(sample_count_address);
  if (!sample_counts || end_record != sample_count_address) {
    sample_counts = memory_->TranslatePhysical<
        xenos::xe_gpu_depth_sample_counts*>(end_record);
  }
  auto* begin_counts = memory_->TranslatePhysical<
      xenos::xe_gpu_depth_sample_counts*>(
      logical_occlusion_query_.begin_record);
  if (!sample_counts || !begin_counts) {
    return false;
  }

  if (active_occlusion_query_.valid) {
    SparseZpdWorkScope submit_scope(cp_cadence_, 0);
    if (!CloseGuestOcclusionQuerySegment()) {
      return false;
    }
  }
  // The END record keeps the guest's pending sentinel until every host segment
  // of this report has completed; the exact result is published then, without
  // a submission or GPU wait here.
  occlusion_reports_.End(logical_occlusion_query_,
                         guest_frame_count_.load(std::memory_order_relaxed));
  if (kGpuDiagnostics) swap_intervals_.ZpdEnded(occlusion_reports_.ended.size());
  logical_occlusion_query_.Reset();
  return RetireGuestOcclusionQuerySegments(0);
}

uint64_t D3D12CommandProcessor::NormalizeOcclusionSamples(
    uint64_t samples, uint32_t scale_area) const {
  if (samples == 0 || scale_area <= 1) {
    return samples;
  }
  return (samples + (uint64_t(scale_area) >> 1)) / scale_area;
}

void D3D12CommandProcessor::WriteGammaRampSRV(bool is_pwl,
                                              D3D12_CPU_DESCRIPTOR_HANDLE handle) const {
  ID3D12Device* device = GetD3D12Provider().GetDevice();
  D3D12_SHADER_RESOURCE_VIEW_DESC desc;
  desc.Format = DXGI_FORMAT_R10G10B10A2_UNORM;
  desc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
  desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  desc.Buffer.StructureByteStride = 0;
  desc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;
  if (is_pwl) {
    desc.Format = DXGI_FORMAT_R16G16_UINT;
    desc.Buffer.FirstElement = 256 * 4 / 4;
    desc.Buffer.NumElements = 128 * 3;
  } else {
    desc.Format = DXGI_FORMAT_R10G10B10A2_UNORM;
    desc.Buffer.FirstElement = 0;
    desc.Buffer.NumElements = 256;
  }
  device->CreateShaderResourceView(gamma_ramp_buffer_.Get(), &desc, handle);
}

}  // namespace rex::graphics::d3d12
