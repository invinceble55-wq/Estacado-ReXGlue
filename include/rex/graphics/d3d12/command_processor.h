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

#pragma once

// REXGLUE_GPU_DIAGNOSTICS (CMake) -> REX_GPU_DIAGNOSTICS: diagnostics and work
// counters exist only in measurement builds.
#ifndef REX_GPU_DIAGNOSTICS
#define REX_GPU_DIAGNOSTICS 0
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <rex/assert.h>
#include <rex/graphics/command_processor.h>
#include <rex/graphics/d3d12/deferred_command_list.h>
#include <rex/graphics/d3d12/gpu_timing.h>
#include <rex/graphics/d3d12/graphics_system.h>
#include <rex/graphics/d3d12/pipeline_cache.h>
#include <rex/graphics/d3d12/primitive_processor.h>
#include <rex/graphics/d3d12/render_target_cache.h>
#include <rex/graphics/d3d12/shared_memory.h>
#include <rex/graphics/d3d12/texture_cache.h>
#include <rex/graphics/embedded_target_writer_capture_policy.h>
#include <rex/graphics/embedded_depth_resolve_capture_policy.h>
#include <rex/graphics/pipeline/shader/dxbc.h>
#include <rex/graphics/pipeline/render_target/native_shader_scale_policy.h>
#include <rex/graphics/pipeline/shader/dxbc_translator.h>
#include <rex/graphics/registers.h>
#include <rex/graphics/temporal_aa_policy.h>
#include <rex/graphics/util/draw.h>
#include <rex/graphics/xenos.h>
#include <rex/graphics/xenos_zpd_report.h>
#include <rex/system/kernel_state.h>
#include <rex/ui/d3d12/d3d12_descriptor_heap_pool.h>
#include <rex/ui/d3d12/d3d12_provider.h>
#include <rex/ui/d3d12/d3d12_upload_buffer_pool.h>
#include <rex/ui/d3d12/d3d12_util.h>

namespace rex::graphics::d3d12 {

// Measurement builds: RenderDoc's in-application API table (plugin_main.cpp)
// and a request to capture whole guest frames. RenderDoc's own frame boundary
// is a host present, and at high refresh rates one guest frame spans several;
// the command processor starts and ends the capture at guest swaps instead,
// after draining its submission thread, so a capture holds complete frames.
void SetRenderDocApi(void** api);
void RequestRenderDocGuestFrames(uint32_t frames);

class D3D12CommandProcessor : public CommandProcessor {
 public:
  explicit D3D12CommandProcessor(D3D12GraphicsSystem* graphics_system,
                                 system::KernelState* kernel_state);
  ~D3D12CommandProcessor();

  void ClearCaches() override;
  void InvalidateGpuMemory() override;

  void InitializeShaderStorage(const std::filesystem::path& cache_root, uint32_t title_id,
                               bool blocking) override;

  void RequestFrameTrace(const std::filesystem::path& root_path) override;

  void TracePlaybackWroteMemory(uint32_t base_ptr, uint32_t length) override;
  void MarkHostWrite(uint32_t base_ptr, uint32_t length) override;

  void RestoreEdramSnapshot(const void* snapshot) override;

  ui::d3d12::D3D12Provider& GetD3D12Provider() const {
    return *static_cast<ui::d3d12::D3D12Provider*>(graphics_system_->provider());
  }

  // Returns the deferred drawing command list for the currently open
  // submission.
  DeferredCommandList& GetDeferredCommandList() {
    assert_true(submission_open_);
    return deferred_command_list_;
  }

  uint64_t GetCurrentSubmission() const { return submission_current_; }
  uint64_t GetCompletedSubmission() const { return submission_completed_; }

#if REX_GPU_DIAGNOSTICS
  // Swap-interval diagnostic only (no effect unless that observer is active).
  void NoteSharedMemoryUpload(uint64_t bytes) { swap_intervals_.AddUpload(bytes); }
  // Swap-interval diagnostics only (texture creations per frame).
  bool SwapIntervalObserverActive() const { return swap_intervals_.active; }
  void NoteTextureCreation(uint64_t ticks, bool /*placed*/) {
    swap_intervals_.AddTextureCreation(ticks);
  }

  // GPU timing diagnostic (d3d12_gpu_timing, off by default): GPU work
  // recorded while a scope is alive is attributed to its category; the
  // previous category resumes when it ends. Only a null check when off.
  class GpuTimingScope {
   public:
    GpuTimingScope(D3D12CommandProcessor& command_processor, GpuTimingCategory category)
        : command_processor_(command_processor) {
      if (command_processor_.gpu_timing_) {
        previous_ = command_processor_.GpuTimingSwitch(category);
      }
    }
    ~GpuTimingScope() {
      if (command_processor_.gpu_timing_) {
        command_processor_.GpuTimingSwitch(previous_);
      }
    }
    GpuTimingScope(const GpuTimingScope&) = delete;
    GpuTimingScope& operator=(const GpuTimingScope&) = delete;

   private:
    D3D12CommandProcessor& command_processor_;
    GpuTimingCategory previous_ = GpuTimingCategory::kOther;
  };
  // Work recorded from now on belongs to the category (until the next switch).
  void GpuTimingMark(GpuTimingCategory category) {
    if (gpu_timing_) {
      GpuTimingSwitch(category);
    }
  }
  void GpuTimingCount(GpuTimingCounter counter, uint64_t value) {
    if (gpu_timing_) {
      GpuTimingAddCount(counter, value);
    }
  }
  bool GpuTimingEnabled() const { return gpu_timing_ != nullptr; }
  // d3d12_gpu_timing_passes: draw time is attributed to the render-target set
  // bound from now on (keys: depth, then colors; 0 for none).
  void GpuTimingNotePass(const uint32_t* render_target_keys);
  // Splits draw time by render-target set and shader pair (once per draw).
  void GpuTimingDrawPass();
  // True in the sampled frames whose transfers are logged
  // (d3d12_gpu_timing_transfer_log_interval).
  bool GpuTimingTransferLogFrame() const { return gpu_timing_ && GpuTimingLogFrame(); }
#else
  // Player builds (REXGLUE_GPU_DIAGNOSTICS off): the swap-interval observer
  // hooks and the GPU timing diagnostic are compiled out - no checks remain.
  void NoteSharedMemoryUpload(uint64_t) {}
  static constexpr bool SwapIntervalObserverActive() { return false; }
  void NoteTextureCreation(uint64_t, bool) {}
  class GpuTimingScope {
   public:
    GpuTimingScope(D3D12CommandProcessor&, GpuTimingCategory) {}
    GpuTimingScope(const GpuTimingScope&) = delete;
    GpuTimingScope& operator=(const GpuTimingScope&) = delete;
  };
  void GpuTimingMark(GpuTimingCategory) {}
  void GpuTimingCount(GpuTimingCounter, uint64_t) {}
  static constexpr bool GpuTimingEnabled() { return false; }
  void GpuTimingNotePass(const uint32_t*) {}
  void GpuTimingDrawPass() {}
  static constexpr bool GpuTimingTransferLogFrame() { return false; }
#endif

  // d3d12_gpu_frame_meter (on by default, two timestamps per submission): the
  // GPU busy time of each guest frame, for automatic settings.
  size_t GetRecentGpuFrameBusyUs(uint32_t* out, size_t capacity,
                                 uint64_t* total_frames_out = nullptr) const override;

  // Must be called when a subsystem does something like UpdateTileMappings so
  // it can be awaited in CheckSubmissionFence(submission_current_) if it was
  // done after the latest ExecuteCommandLists + Signal.
  void NotifyQueueOperationsDoneDirectly() {
    queue_operations_done_since_submission_signal_ = true;
  }

  uint64_t GetCurrentFrame() const { return frame_current_; }
  uint64_t GetCompletedFrame() const { return frame_completed_; }

  // Returns true if the barrier has been inserted (the new state is different).
  bool PushTransitionBarrier(ID3D12Resource* resource, D3D12_RESOURCE_STATES old_state,
                             D3D12_RESOURCE_STATES new_state,
                             UINT subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES);
  void PushAliasingBarrier(ID3D12Resource* old_resource, ID3D12Resource* new_resource);
  void PushUAVBarrier(ID3D12Resource* resource);
  void SubmitBarriers();

  // Finds or creates root signature for a pipeline.
  ID3D12RootSignature* GetRootSignature(const DxbcShader* vertex_shader,
                                        const DxbcShader* pixel_shader, bool tessellated);

  ui::d3d12::D3D12UploadBufferPool& GetConstantBufferPool() const { return *constant_buffer_pool_; }

  D3D12_CPU_DESCRIPTOR_HANDLE GetViewBindlessHeapCPUStart() const {
    assert_true(bindless_resources_used_);
    return view_bindless_heap_cpu_start_;
  }
  D3D12_GPU_DESCRIPTOR_HANDLE GetViewBindlessHeapGPUStart() const {
    assert_true(bindless_resources_used_);
    return view_bindless_heap_gpu_start_;
  }
  // Returns UINT32_MAX if no free descriptors. If the unbounded SRV range for
  // bindless resources is also used in the root signature of the draw /
  // dispatch referencing this descriptor, this must only be used to allocate
  // SRVs, otherwise it won't work on Nvidia Fermi (root signature creation will
  // fail)!
  uint32_t RequestPersistentViewBindlessDescriptor();
  void ReleaseViewBindlessDescriptorImmediately(uint32_t descriptor_index);
  // Request non-contiguous CBV/SRV/UAV descriptors for use only within the next
  // draw or dispatch command done for internal purposes. May change the current
  // descriptor heap. If the unbounded SRV range for bindless resources is also
  // used in the root signature of the draw / dispatch referencing these
  // descriptors, this must only be used to allocate SRVs, otherwise it won't
  // work on Nvidia Fermi (root signature creation will fail)!
  bool RequestOneUseSingleViewDescriptors(uint32_t count,
                                          ui::d3d12::util::DescriptorCpuGpuHandlePair* handles_out);
  // These are needed often, so they are always allocated.
  enum class SystemBindlessView : uint32_t {
    // Both may be bound as one root parameter.
    kSharedMemoryRawSRVAndNullRawUAVStart,
    kSharedMemoryRawSRV = kSharedMemoryRawSRVAndNullRawUAVStart,
    kNullRawUAV,

    // Both may be bound as one root parameter.
    kNullRawSRVAndSharedMemoryRawUAVStart,
    kNullRawSRV = kNullRawSRVAndSharedMemoryRawUAVStart,
    kSharedMemoryRawUAV,

    kSharedMemoryR32UintSRV,
    kSharedMemoryR32G32UintSRV,
    kSharedMemoryR32G32B32A32UintSRV,
    kSharedMemoryR32UintUAV,
    kSharedMemoryR32G32UintUAV,
    kSharedMemoryR32G32B32A32UintUAV,

    kEdramRawSRV,
    kEdramR32UintSRV,
    kEdramR32G32UintSRV,
    kEdramR32G32B32A32UintSRV,
    kEdramRawUAV,
    kEdramR32UintUAV,
    kEdramR32G32UintUAV,
    kEdramR32G32B32A32UintUAV,

    kGammaRampTableSRV,
    kGammaRampPWLSRV,

    // Beyond this point, SRVs are accessible to shaders through an unbounded
    // range - no descriptors of other types bound to shaders alongside
    // unbounded ranges - must be located beyond this point.
    kUnboundedSRVsStart,
    kNullTexture2DArray = kUnboundedSRVsStart,
    kNullTexture3D,
    kNullTextureCube,

    kCount,
  };
  ui::d3d12::util::DescriptorCpuGpuHandlePair GetSystemBindlessViewHandlePair(
      SystemBindlessView view) const;
  ui::d3d12::util::DescriptorCpuGpuHandlePair GetSharedMemoryUintPow2BindlessSRVHandlePair(
      uint32_t element_size_bytes_pow2) const;
  ui::d3d12::util::DescriptorCpuGpuHandlePair GetSharedMemoryUintPow2BindlessUAVHandlePair(
      uint32_t element_size_bytes_pow2) const;
  ui::d3d12::util::DescriptorCpuGpuHandlePair GetEdramUintPow2BindlessSRVHandlePair(
      uint32_t element_size_bytes_pow2) const;
  ui::d3d12::util::DescriptorCpuGpuHandlePair GetEdramUintPow2BindlessUAVHandlePair(
      uint32_t element_size_bytes_pow2) const;

  // Returns a single temporary GPU-side buffer within a submission for tasks
  // like texture untiling and resolving.
  ID3D12Resource* RequestScratchGPUBuffer(uint32_t size, D3D12_RESOURCE_STATES state);
  // This must be called when done with the scratch buffer, to notify the
  // command processor about the new state in case the buffer was transitioned
  // by its user.
  void ReleaseScratchGPUBuffer(ID3D12Resource* buffer, D3D12_RESOURCE_STATES new_state);

  // Returns a pipeline with deferred creation by its handle. May return nullptr
  // if failed to create the pipeline.
  ID3D12PipelineState* GetD3D12PipelineByHandle(void* handle) const {
    return pipeline_cache_->GetD3D12PipelineByHandle(handle);
  }

  // Sets the current cached values to external ones. This is for cache
  // invalidation primarily. A submission must be open.
  void SetExternalPipeline(ID3D12PipelineState* pipeline);
  void SetExternalGraphicsRootSignature(ID3D12RootSignature* root_signature);
  void SetViewport(const D3D12_VIEWPORT& viewport);
  void SetScissorRect(const D3D12_RECT& scissor_rect);
  void SetStencilReference(uint32_t stencil_ref);
  void SetPrimitiveTopology(D3D12_PRIMITIVE_TOPOLOGY primitive_topology);

  // Returns the text to display in the GPU backend name in the window title.
  std::string GetWindowTitleText() const;

 protected:
  bool QueryCadenceSubmission(uint64_t& submitted, uint64_t& completed) override;
  bool SetupContext() override;
  void ShutdownContext() override;

  void WriteRegister(uint32_t index, uint32_t value) override;
  void WriteRegisterFromPacket(uint32_t index, uint32_t value,
                               uint32_t packet, uint32_t data,
                               const pc_owned_camera_packet::Source* source = nullptr) override;
  void InvalidateRegisterProvenance() override;
  void WriteRegistersFromMem(uint32_t start_index, uint32_t* base, uint32_t num_registers) override;
  bool ExecutePacketType3_EVENT_WRITE_ZPD(memory::RingBuffer* reader, uint32_t packet,
                                          uint32_t count) override;
  void PrepareForWait() override;
  void PrepareForPacketMemoryWrite(uint32_t address, uint32_t bytes) override;
  // Copy stage: every queued guest-memory copy into an upload page is done
  // before anything tells the title the GPU consumed its data.
  void SettleGuestVisibleWork() override { SettleUploadCopies(); }
  void SettleUploadCopies();
  std::string SwapIntervalBackendStats() override;
  // Copy-stage counters (reported with the swap-interval diagnostic).
  uint64_t upload_settle_calls_ = 0;
  uint64_t upload_settle_blocked_ = 0;
  uint64_t upload_settle_blocked_ticks_ = 0;

  void OnGammaRamp256EntryTableValueWritten() override;
  void OnGammaRampPWLValueWritten() override;

  void IssueSwap(uint32_t frontbuffer_ptr, uint32_t frontbuffer_width,
                 uint32_t frontbuffer_height) override;

  void OnPrimaryBufferEnd() override;

  Shader* LoadShader(xenos::ShaderType shader_type, uint32_t guest_address,
                     const uint32_t* host_address, uint32_t dword_count) override;

  bool IssueDraw(xenos::PrimitiveType primitive_type, uint32_t index_count,
                 IndexBufferInfo* index_buffer_info, bool major_mode_explicit) override;
  bool IssueCopy() override;
  void OnPredicatedPacketSkipped(uint32_t opcode) override;

  void InitializeTrace() override;

 private:
  friend class D3D12RenderTargetCache;
  // The texture cache owns the scaled-resolve resource state. Its bounded
  // diagnostic readback must submit and await the exact copy without exposing
  // the command processor's general submission controls publicly.
  friend class D3D12TextureCache;

  static constexpr uint32_t kQueueFrames = 3;

  enum RootParameter : UINT {
    // Keep the size of the root signature at each stage 13 dwords or less
    // (better 12 or less) so it fits in user data on AMD. Descriptor tables are
    // 1 dword, root descriptors are 2 dwords (however, root descriptors require
    // less setup on the CPU - balance needs to be maintained).

    // CBVs are set in both bindful and bindless cases via root descriptors.

    // - Bindful resources - multiple root signatures depending on extra
    //   parameters.

    // These are always present.

    // Very frequently changed, especially for UI draws, and for models drawn in
    // multiple parts - contains vertex and texture fetch constants.
    kRootParameter_Bindful_FetchConstants = 0,  // +2 dwords = 2 in all.
    // Quite frequently changed (for one object drawn multiple times, for
    // instance - may contain projection matrices).
    kRootParameter_Bindful_FloatConstantsVertex,  // +2 = 4 in VS.
    // Less frequently changed (per-material).
    kRootParameter_Bindful_FloatConstantsPixel,  // +2 = 4 in PS.
    // May stay the same across many draws.
    kRootParameter_Bindful_SystemConstants,  // +2 = 6 in all.
    // Pretty rarely used and rarely changed - flow control constants.
    kRootParameter_Bindful_BoolLoopConstants,  // +2 = 8 in all.
    // Changed only when starting a new descriptor heap or when switching
    // between shared memory as SRV and UAV - shared memory byte address buffer
    // (as SRV and as UAV, either may be null if not used), and, if ROV is used
    // for EDRAM, EDRAM R32_UINT UAV.
    kRootParameter_Bindful_SharedMemoryAndEdram,  // +1 = 9 in all.

    kRootParameter_Bindful_Count_Base,

    // Extra parameter that may or may not exist:
    // - Pixel textures (+1 = 10 in PS).
    // - Pixel samplers (+1 = 11 in PS).
    // - Vertex textures (+1 = 10 in VS).
    // - Vertex samplers (+1 = 11 in VS).

    kRootParameter_Bindful_Count_Max = kRootParameter_Bindful_Count_Base + 4,

    // - Bindless resources - two global root signatures (for non-tessellated
    //   and tessellated drawing), so these are always present.

    kRootParameter_Bindless_FetchConstants = 0,    // +2 = 2 in all.
    kRootParameter_Bindless_FloatConstantsVertex,  // +2 = 4 in VS.
    kRootParameter_Bindless_FloatConstantsPixel,   // +2 = 4 in PS.
    // Changed per-material, texture and sampler descriptor indices.
    kRootParameter_Bindless_DescriptorIndicesPixel,   // +2 = 6 in PS.
    kRootParameter_Bindless_DescriptorIndicesVertex,  // +2 = 6 in VS.
    kRootParameter_Bindless_SystemConstants,          // +2 = 8 in all.
    kRootParameter_Bindless_BoolLoopConstants,        // +2 = 10 in all.
    // Changed only when switching between shared memory as SRV and UAV - shared
    // memory byte address buffer (as SRV and as UAV, either may be null if not
    // used).
    kRootParameter_Bindless_SharedMemory,  // +1 = 11 in all.
    // Unbounded sampler descriptor table - changed in case of overflow.
    kRootParameter_Bindless_SamplerHeap,  // +1 = 12 in all.
    // Unbounded SRV/UAV descriptor table - never changed.
    kRootParameter_Bindless_ViewHeap,  // +1 = 13 in all.

    kRootParameter_Bindless_Count,
  };

  struct RootBindfulExtraParameterIndices {
    uint32_t textures_pixel;
    uint32_t samplers_pixel;
    uint32_t textures_vertex;
    uint32_t samplers_vertex;
    static constexpr uint32_t kUnavailable = UINT32_MAX;
  };
  // Gets the indices of optional root parameters. Returns the total parameter
  // count.
  static uint32_t GetRootBindfulExtraParameterIndices(
      const DxbcShader* vertex_shader, const DxbcShader* pixel_shader,
      RootBindfulExtraParameterIndices& indices_out);

  // BeginSubmission and EndSubmission may be called at any time. If there's an
  // open non-frame submission, BeginSubmission(true) will promote it to a
  // frame. EndSubmission(true) will close the frame no matter whether the
  // submission has already been closed.
  // Submission (ExecuteCommandLists) boundaries are implicit full UAV and
  // aliasing barriers, and also result in common resource state promotion and
  // decay.

  // Rechecks submission number and reclaims per-submission resources. Pass 0 as
  // the submission to await to simply check status, or pass submission_current_
  // to wait for all queue operations to be completed.
  void CheckSubmissionFence(uint64_t await_submission);
  // If is_guest_command is true, a new full frame - with full cleanup of
  // resources and, if needed, starting capturing - is opened if pending (as
  // opposed to simply resuming after mid-frame synchronization). Returns
  // whether a submission is open currently and the device is not removed.
  bool BeginSubmission(bool is_guest_command);
  bool IsSubmissionOpen() const { return submission_open_; }
  // If is_swap is true, a full frame is closed - with, if needed, cache
  // clearing and stopping capturing. Returns whether the submission was done
  // successfully, if it has failed, leaves it open.
  bool EndSubmission(bool is_swap);
  // Checks if ending a submission right now would not cause potentially more
  // delay than it would reduce by making the GPU start working earlier - such
  // as when there are unfinished graphics pipeline creation requests that would
  // need to be fulfilled before actually submitting the command list.
  bool CanEndSubmissionImmediately() const;
  bool AwaitAllQueueOperationsCompletion() {
    CheckSubmissionFence(submission_current_);
    return submission_completed_ + 1 >= submission_current_;
  }
  void LogDeviceRemovalDiagnostics(ID3D12Device* device, HRESULT reason);

  void UpdateDebugMarkersEnabled();
  void PushDebugMarker(const char* format, ...);
  void PopDebugMarker();
  void InsertDebugMarker(const char* format, ...);
  bool debug_markers_enabled() const { return debug_markers_enabled_; }

  // Need to await submission completion before calling.
  void ClearCommandAllocatorCache();

  // Request descriptors and automatically rebind the descriptor heap on the
  // draw command list. Refer to D3D12DescriptorHeapPool::Request for partial /
  // full update explanation. Doesn't work when bindless descriptors are used.
  uint64_t RequestViewBindfulDescriptors(uint64_t previous_heap_index,
                                         uint32_t count_for_partial_update,
                                         uint32_t count_for_full_update,
                                         D3D12_CPU_DESCRIPTOR_HANDLE& cpu_handle_out,
                                         D3D12_GPU_DESCRIPTOR_HANDLE& gpu_handle_out);
  uint64_t RequestSamplerBindfulDescriptors(uint64_t previous_heap_index,
                                            uint32_t count_for_partial_update,
                                            uint32_t count_for_full_update,
                                            D3D12_CPU_DESCRIPTOR_HANDLE& cpu_handle_out,
                                            D3D12_GPU_DESCRIPTOR_HANDLE& gpu_handle_out);

  void UpdateFixedFunctionState(const draw_util::ViewportInfo& viewport_info,
                                const draw_util::Scissor& scissor, bool primitive_polygonal,
                                reg::RB_DEPTHCONTROL normalized_depth_control);
  void UpdateSystemConstantValues(bool shared_memory_is_uav, bool primitive_polygonal,
                                  uint32_t line_loop_closing_index, xenos::Endian index_endian,
                                  const draw_util::ViewportInfo& viewport_info,
                                  uint32_t used_texture_mask,
                                  reg::RB_DEPTHCONTROL normalized_depth_control,
                                  uint32_t normalized_color_mask);
  bool UpdateBindings(const D3D12Shader* vertex_shader, const D3D12Shader* pixel_shader,
                      ID3D12RootSignature* root_signature, bool shared_memory_is_uav);
  bool IssueCopy_ReadbackResolvePath();
  bool IssueDraw_MemexportReadbackFullPath(uint32_t total_size);
  bool IssueDraw_MemexportReadbackFastPath(uint32_t total_size);

  // Returns a buffer for reading GPU data back to the CPU. Assuming
  // synchronizing immediately after use. Always in COPY_DEST state.
  ID3D12Resource* RequestReadbackBuffer(uint32_t size);
  struct ReadbackBuffer {
    ID3D12Resource* buffers[2] = {nullptr, nullptr};
    uint32_t sizes[2] = {0, 0};
    void* mapped_data[2] = {nullptr, nullptr};
    uint64_t submission_written[2] = {0, 0};
    uint32_t written_size[2] = {0, 0};
    uint32_t current_index = 0;
    uint64_t last_used_frame = 0;
  };
  void EvictOldReadbackBuffers(std::unordered_map<uint64_t, ReadbackBuffer>& buffer_map);
  static constexpr uint32_t kReadbackBufferSizeIncrement = 16 * 1024 * 1024;
  static constexpr size_t kMaxReadbackBuffers = 256;
  static constexpr uint64_t kReadbackBufferEvictionAgeFrames = 60;
  static inline uint32_t AlignReadbackBufferSize(uint32_t size) {
    if (size < 1 * 1024 * 1024) {
      return rex::align(size, 256u * 1024u);
    }
    if (size < 4 * 1024 * 1024) {
      return rex::align(size, 1u * 1024u * 1024u);
    }
    return rex::align(size, kReadbackBufferSizeIncrement);
  }
  static inline uint64_t MakeReadbackResolveKey(uint32_t address, uint32_t length) {
    return (uint64_t(address) << 32) | uint64_t(length);
  }
  static inline uint64_t MakeMemexportReadbackKey(uint32_t first_base_address_dwords,
                                                  uint32_t total_size) {
    return (uint64_t(first_base_address_dwords) << 32) | uint64_t(total_size);
  }

  bool InitializeOcclusionQueryResources();
  void ShutdownOcclusionQueryResources();
  bool InitializeEmbeddedSceneHostVertexOutputResources();
  void ShutdownEmbeddedSceneHostVertexOutputResources();
  bool BeginGuestOcclusionQuery(uint32_t sample_count_address);
  bool EndGuestOcclusionQuery(uint32_t sample_count_address,
                              xenos::xe_gpu_depth_sample_counts* sample_counts);
  bool BeginGuestOcclusionQuerySegment();
  bool CloseGuestOcclusionQuerySegment();
  bool UpdateGuestOcclusionQueryScale(uint32_t scale_area);
  // Retires completed host segments and publishes finished guest reports.
  // await_submission 0 never waits; UINT64_MAX awaits every segment in flight.
  bool RetireGuestOcclusionQuerySegments(uint64_t await_submission);
  void PublishGuestOcclusionReport(const XenosZPDReportAccumulator& report);
  // Publishes every deferred report overlapping the guest range first.
  // kind (diagnostic counters only): 0 same-slot BEGIN, 1 packet write.
  bool AwaitGuestOcclusionReportsInRange(uint32_t address, uint32_t bytes,
                                         uint32_t kind);
  // At a guest swap: publishes (waiting for their host submissions if needed)
  // the reports that ended at least d3d12_zpd_max_publish_lag_frames swaps ago.
  void BoundGuestOcclusionReportLag();
  bool AcquireOcclusionQueryIndex(uint32_t& host_index_out);
  void DisableHostOcclusionQueries();
  uint64_t NormalizeOcclusionSamples(uint64_t samples, uint32_t scale_area) const;
  void InvalidateAllVertexBufferResidency();
  void InvalidateVertexBufferResidency(uint32_t vfetch_index);
  void InvalidateVertexBufferResidencyRange(uint32_t first_vfetch, uint32_t last_vfetch);

  void WriteGammaRampSRV(bool is_pwl, D3D12_CPU_DESCRIPTOR_HANDLE handle) const;

  bool device_removed_ = false;

  bool cache_clear_requested_ = false;

  HANDLE fence_completion_event_ = nullptr;

  bool submission_open_ = false;
  // Values of submission_fence_.
  uint64_t submission_current_ = 1;
  uint64_t submission_completed_ = 0;
  ID3D12Fence* submission_fence_ = nullptr;

  // Asynchronous submission (init-only d3d12_async_submission). EndSubmission
  // hands the recorded deferred stream to one worker thread, which replays it
  // into command_list_, executes it and signals submission_fence_ with the
  // submission's value, strictly in order. The command processor drains the
  // worker only where later queue work from another component must follow
  // (shutdown, and the presenter refresh when its completion can't be
  // deferred). A job without an allocator is an ordered task: it runs on the
  // worker after every earlier job has been executed and signaled.
  struct SubmissionJob {
    std::vector<uintmax_t> stream;
    ID3D12CommandAllocator* allocator = nullptr;
    uint64_t fence_value = 0;
    std::function<void()> task;
    // Copy stage: the job's upload pages are written once this copy is done.
    uint64_t upload_copy_ticket = 0;
  };
  void StartSubmissionThread();
  void StopSubmissionThread();
  void SubmissionThreadMain();
  void DrainSubmissions();
  // Runs task on the submission worker after all currently queued jobs (inline
  // when there is no worker).
  void EnqueueSubmissionTask(std::function<void()> task);
  // V292: the presenter's post-refresh steps (fence signal, mailbox
  // publication, immediate paint) run as an ordered worker task after the
  // frame's command lists instead of the command processor draining the
  // worker at every swap.
  bool presenter_completion_deferred_ = false;
  bool async_submission_ = false;
  std::thread submission_thread_;
  std::mutex submission_mutex_;
  std::condition_variable submission_work_cv_;
  std::condition_variable submission_idle_cv_;
  std::deque<SubmissionJob> submission_jobs_;
  std::vector<std::vector<uintmax_t>> submission_free_streams_;
  bool submission_worker_busy_ = false;
  bool submission_thread_stop_ = false;

  // For awaiting non-submission queue operations such as UpdateTileMappings in
  // AwaitAllQueueOperationsCompletion when they're queued after the latest
  // ExecuteCommandLists + Signal, thus won't be awaited by just awaiting the
  // submission.
  ID3D12Fence* queue_operations_since_submission_fence_ = nullptr;
  uint64_t queue_operations_since_submission_fence_last_ = 0;
  bool queue_operations_done_since_submission_signal_ = false;

  bool frame_open_ = false;
  // Guest frame index, since some transient resources can be reused across
  // submissions. Values updated in the beginning of a frame.
  uint64_t frame_current_ = 1;
  uint64_t frame_completed_ = 0;
  // Submission indices of frames that have already been submitted.
  uint64_t closed_frame_submissions_[kQueueFrames] = {};

  struct CommandAllocator {
    ID3D12CommandAllocator* command_allocator;
    uint64_t last_usage_submission;
    CommandAllocator* next;
  };
  CommandAllocator* command_allocator_writable_first_ = nullptr;
  CommandAllocator* command_allocator_writable_last_ = nullptr;
  CommandAllocator* command_allocator_submitted_first_ = nullptr;
  CommandAllocator* command_allocator_submitted_last_ = nullptr;
  ID3D12GraphicsCommandList* command_list_ = nullptr;
  ID3D12GraphicsCommandList1* command_list_1_ = nullptr;
  DeferredCommandList deferred_command_list_;

  bool debug_markers_enabled_ = false;

  // Viewport info caching - avoids redundant GetHostViewportInfo recalculation
  // when viewport-affecting register state hasn't changed between draws.
  struct ViewportCacheKey {
    uint32_t pa_cl_clip_cntl;
    uint32_t pa_cl_vte_cntl;
    uint32_t pa_su_sc_mode_cntl;
    uint32_t pa_su_vtx_cntl;
    uint32_t pa_sc_window_offset;
    uint32_t normalized_depth_control;
    uint32_t vport_regs[6];  // XSCALE, XOFFSET, YSCALE, YOFFSET, ZSCALE, ZOFFSET
    uint32_t flags;          // packed: convert_z_to_float24, full_float24, ps_writes_depth
    bool operator==(const ViewportCacheKey&) const = default;
  };
  ViewportCacheKey previous_viewport_key_{};
  draw_util::ViewportInfo previous_viewport_info_{};
  bool viewport_cache_valid_ = false;

  // Should bindless textures and samplers be used - many times faster
  // UpdateBindings than bindful (that becomes a significant bottleneck with
  // bindful - mainly because of CopyDescriptorsSimple, which takes the majority
  // of UpdateBindings time, and that's outside the emulator's control even).
  bool bindless_resources_used_ = false;

  std::unique_ptr<D3D12SharedMemory> shared_memory_;

  std::unique_ptr<D3D12RenderTargetCache> render_target_cache_;
  render_target::native_shader_scale_policy::Rules native_shader_grid_rules_;
  uint64_t native_shader_grid_logged_mask_ = 0;  // two bits per rule (MSAA class)
  // Footprint reconstruction per fetch for the current draw's matched image
  // filter rules. A filter variant compiles the reconstruction for every 2D
  // fetch; each one's region constant selects at runtime: kOff samples
  // normally, kRegion bounds it to a :region= rectangle, kSourceNative picks
  // the tracked native rectangle containing the sample (constants are written
  // after the bindings update, so the tracker sees the bound texture).
  enum class NativeFilterFetchMode : uint8_t { kOff, kUnbounded, kRegion, kSourceNative };
  bool native_filter_active_ = false;
  NativeFilterFetchMode native_filter_fetch_modes_[32] = {};
  float native_filter_fetch_regions_[32][4] = {};
  embedded_target_writer_capture_policy::Config embedded_target_writer_config_;
  embedded_target_writer_capture_policy::State embedded_target_writer_state_;
  embedded_depth_resolve_capture_policy::State embedded_depth_resolve_capture_state_;

  std::unique_ptr<ui::d3d12::D3D12UploadBufferPool> constant_buffer_pool_;

  static constexpr uint32_t kViewBindfulHeapSize = 32768;
  static_assert(kViewBindfulHeapSize <= D3D12_MAX_SHADER_VISIBLE_DESCRIPTOR_HEAP_SIZE_TIER_1);
  std::unique_ptr<ui::d3d12::D3D12DescriptorHeapPool> view_bindful_heap_pool_;
  // Currently bound descriptor heap - updated by RequestViewBindfulDescriptors.
  ID3D12DescriptorHeap* view_bindful_heap_current_;
  // Rationale: textures have 4 KB alignment in guest memory, and there can be
  // 512 MB / 4 KB in total of them at most, and multiply by 3 for different
  // swizzles, signedness, and multiple host textures for one guest texture, and
  // transient descriptors. Though in reality there will be a lot fewer of
  // course, this is just a "safe" value. The limit is 1000000 for resource
  // binding tier 2.
  static constexpr uint32_t kViewBindlessHeapSize = 262144;
  static_assert(kViewBindlessHeapSize <= D3D12_MAX_SHADER_VISIBLE_DESCRIPTOR_HEAP_SIZE_TIER_2);
  ID3D12DescriptorHeap* view_bindless_heap_ = nullptr;
  D3D12_CPU_DESCRIPTOR_HANDLE view_bindless_heap_cpu_start_;
  D3D12_GPU_DESCRIPTOR_HANDLE view_bindless_heap_gpu_start_;
  uint32_t view_bindless_heap_allocated_ = 0;
  std::vector<uint32_t> view_bindless_heap_free_;
  // <Descriptor index, submission where requested>, sorted by the submission
  // number.
  std::deque<std::pair<uint32_t, uint64_t>> view_bindless_one_use_descriptors_;

  // Direct3D 12 only allows shader-visible heaps with no more than 2048
  // samplers (due to Nvidia addressing). However, there's also possibly a weird
  // bug in the Nvidia driver (tested on 440.97 and earlier on Windows 10 1803)
  // that caused the sampler with index 2047 not to work if a heap with 8 or
  // less samplers also exists - in case of Xenia, it's the immediate drawer's
  // sampler heap.
  // FIXME(Triang3l): Investigate the issue with the sampler 2047 on Nvidia.
  static constexpr uint32_t kSamplerHeapSize = 2000;
  static_assert(kSamplerHeapSize <= D3D12_MAX_SHADER_VISIBLE_SAMPLER_HEAP_SIZE);
  std::unique_ptr<ui::d3d12::D3D12DescriptorHeapPool> sampler_bindful_heap_pool_;
  ID3D12DescriptorHeap* sampler_bindful_heap_current_;
  ID3D12DescriptorHeap* sampler_bindless_heap_current_ = nullptr;
  D3D12_CPU_DESCRIPTOR_HANDLE sampler_bindless_heap_cpu_start_;
  D3D12_GPU_DESCRIPTOR_HANDLE sampler_bindless_heap_gpu_start_;
  // Currently the sampler heap is used only for texture cache samplers, so
  // individual samplers are never freed, and using a simple linear allocator
  // inside the current heap without a free list.
  uint32_t sampler_bindless_heap_allocated_ = 0;
  // <Heap, overflow submission number>, if total sampler count used so far
  // exceeds kSamplerHeapSize, and the heap has been switched (this is not a
  // totally impossible situation considering Direct3D 9 has sampler parameter
  // state instead of sampler objects, and having one "unimportant" parameter
  // changed may result in doubling of sampler count). Sorted by the submission
  // number (so checking if the first can be reused is enough).
  std::deque<std::pair<ID3D12DescriptorHeap*, uint64_t>> sampler_bindless_heaps_overflowed_;
  // D3D12TextureCache::SamplerParameters::value -> indices within the current
  // bindless sampler heap.
  std::unordered_map<uint32_t, uint32_t> texture_cache_bindless_sampler_map_;

  // Root signatures for different descriptor counts.
  std::unordered_map<uint32_t, ID3D12RootSignature*> root_signatures_bindful_;
  ID3D12RootSignature* root_signature_bindless_vs_ = nullptr;
  ID3D12RootSignature* root_signature_bindless_ds_ = nullptr;

  std::unique_ptr<D3D12PrimitiveProcessor> primitive_processor_;

  std::unique_ptr<PipelineCache> pipeline_cache_;

  std::unique_ptr<D3D12TextureCache> texture_cache_;

  // Bytes 0x0...0x3FF - 256-entry gamma ramp table with B10G10R10X2 data (read
  // as R10G10B10X2 with swizzle).
  // Bytes 0x400...0x9FF - 128-entry PWL R16G16 gamma ramp (R - base, G - delta,
  // low 6 bits of each are zero, 3 elements per entry).
  Microsoft::WRL::ComPtr<ID3D12Resource> gamma_ramp_buffer_;
  D3D12_RESOURCE_STATES gamma_ramp_buffer_state_;
  // Upload buffer for an image that is the same as gamma_ramp_, but with
  // kQueueFrames array layers.
  Microsoft::WRL::ComPtr<ID3D12Resource> gamma_ramp_upload_buffer_;
  uint8_t* gamma_ramp_upload_buffer_mapping_ = nullptr;
  bool gamma_ramp_256_entry_table_up_to_date_ = false;
  bool gamma_ramp_pwl_up_to_date_ = false;

  struct ApplyGammaConstants {
    uint32_t size[2];
  };
  enum class ApplyGammaRootParameter : UINT {
    kConstants,
    kDestination,
    kSource,
    kRamp,

    kCount,
  };
  Microsoft::WRL::ComPtr<ID3D12RootSignature> apply_gamma_root_signature_;
  Microsoft::WRL::ComPtr<ID3D12PipelineState> apply_gamma_table_pipeline_;
  Microsoft::WRL::ComPtr<ID3D12PipelineState> apply_gamma_table_fxaa_luma_pipeline_;
  Microsoft::WRL::ComPtr<ID3D12PipelineState> apply_gamma_pwl_pipeline_;
  Microsoft::WRL::ComPtr<ID3D12PipelineState> apply_gamma_pwl_fxaa_luma_pipeline_;

  struct FxaaConstants {
    uint32_t size[2];
    float size_inv[2];
  };
  enum class FxaaRootParameter : UINT {
    kConstants,
    kDestination,
    kSource,

    kCount,
  };
  Microsoft::WRL::ComPtr<ID3D12RootSignature> fxaa_root_signature_;
  Microsoft::WRL::ComPtr<ID3D12PipelineState> fxaa_pipeline_;
  Microsoft::WRL::ComPtr<ID3D12PipelineState> fxaa_extreme_pipeline_;

  struct ResolveDownscaleConstants {
    uint32_t scale_x;
    uint32_t scale_y;
    uint32_t pixel_size_log2;
    uint32_t tile_count;
    uint32_t half_pixel_offset;
  };
  enum class ResolveDownscaleRootParameter : UINT {
    kConstants,
    kSource,
    kDestination,

    kCount,
  };
  Microsoft::WRL::ComPtr<ID3D12RootSignature> resolve_downscale_root_signature_;
  Microsoft::WRL::ComPtr<ID3D12PipelineState> resolve_downscale_pipeline_;
  Microsoft::WRL::ComPtr<ID3D12Resource> resolve_downscale_buffer_;
  uint32_t resolve_downscale_buffer_size_ = 0;

  // PWL gamma ramp can result in values with more precision than 10bpc. Though
  // those sub-10bpc bits don't have any noticeable visual effect, so normally
  // R10G10B10A2_UNORM is enough. But what's the most important is that for the
  // original FXAA shader, the luma needs to be written to the alpha channel.
  // For simplicity (to avoid modifying the FXAA shader and adding more texture
  // fetches into it), and for the highest quality (preserving all 13 bits that
  // may be generated by applying the PWL gamma ramp with an increment of 2^3,
  // and also leaving some space for the result of applying fractional weights
  // to calculate the luma), using R16G16B16A16_UNORM instead of
  // R10G10B10X2_UNORM with a separate alpha texture.
  static constexpr DXGI_FORMAT kFxaaSourceTextureFormat = DXGI_FORMAT_R16G16B16A16_UNORM;
  // Kept in NON_PIXEL_SHADER_RESOURCE state.
  Microsoft::WRL::ComPtr<ID3D12Resource> fxaa_source_texture_;
  uint64_t fxaa_source_texture_submission_ = 0;

  // SMAA 1x (swap_post_effect smaa): three compute passes over the gamma-
  // corrected guest output (shaders/smaa.cs.hlsl). Sources are single-
  // descriptor tables t0-t2, the destination u0; static samplers s0 linear
  // and s1 point, both clamped.
  enum class SmaaRootParameter : UINT {
    kConstants,
    kSource0,
    kSource1,
    kSource2,
    kDestination,

    kCount,
  };
  Microsoft::WRL::ComPtr<ID3D12RootSignature> smaa_root_signature_;
  Microsoft::WRL::ComPtr<ID3D12PipelineState> smaa_edge_detection_pipeline_;
  Microsoft::WRL::ComPtr<ID3D12PipelineState> smaa_blending_weight_pipeline_;
  Microsoft::WRL::ComPtr<ID3D12PipelineState> smaa_neighborhood_blending_pipeline_;
  // The reference's precomputed area (160x560 R8G8) and search (64x16 R8)
  // tables, uploaded at the first SMAA frame; NON_PIXEL_SHADER_RESOURCE.
  Microsoft::WRL::ComPtr<ID3D12Resource> smaa_area_texture_;
  Microsoft::WRL::ComPtr<ID3D12Resource> smaa_search_texture_;
  bool smaa_lookup_textures_uploaded_ = false;
  // Guest-output-sized intermediates (color after gamma, edges, blending
  // weights), kept in NON_PIXEL_SHADER_RESOURCE state.
  Microsoft::WRL::ComPtr<ID3D12Resource> smaa_color_texture_;
  Microsoft::WRL::ComPtr<ID3D12Resource> smaa_edges_texture_;
  Microsoft::WRL::ComPtr<ID3D12Resource> smaa_blend_texture_;
  uint64_t smaa_textures_submission_ = 0;
  bool EnsureSmaaResources(uint32_t width, uint32_t height);

  // Temporal anti-aliasing (V397, gpu_temporal_aa, default off;
  // command_processor_temporal_aa.cpp, temporal_aa_policy.h): the camera of
  // each rendered frame is voted from its depth-writing scene draws, the
  // scene draws are rasterized with a sub-pixel viewport jitter, and right
  // before the title's final composite samples its scene colour a compute
  // resolve blends it with the reprojected history.
  enum class TemporalAaRootParameter : UINT {
    kConstants,
    kColor,
    kDepth,
    kHistory,
    kOutHistory,
    kOutColor,
    kOutUpscalerColor,
    kCount,
  };
  struct TemporalAaFrame {
    temporal_aa::CameraVote vote;
    temporal_aa::Camera previous_camera;
    uint64_t frame = 0;
    // Jitter the scene draws of this frame (the previous one was resolved).
    bool armed = false;
    bool resolved = false;
    bool history_valid = false;
    uint32_t history_index = 0;
    uint32_t depth_base = 0;
    uint32_t depth_pitch = 0;
    uint32_t depth_info = 0;
    // The scene target: the render target (surface info, EDRAM colour base)
    // the colour resolves into the composite's scene colour read from. Only
    // depth-tested geometry drawn to it is jittered; learned in one frame,
    // used in the next.
    uint32_t scene_color_base = 0;
    uint32_t scene_color_bytes = 0;
    uint32_t scene_surface_info = 0;
    uint32_t scene_color_edram_base = 0;
    bool scene_target_valid = false;
    uint32_t next_surface_info = 0;
    uint32_t next_color_edram_base = 0;
    bool next_target_valid = false;
    uint64_t resolves = 0;
    uint64_t reports = 0;
  };
  bool temporal_aa_enabled_ = false;
  bool temporal_aa_jitter_draw_ = false;
  float temporal_aa_jitter_[2] = {};
  std::unique_ptr<TemporalAaFrame> temporal_aa_frame_;
  Microsoft::WRL::ComPtr<ID3D12RootSignature> temporal_aa_root_signature_;
  Microsoft::WRL::ComPtr<ID3D12PipelineState> temporal_aa_pipeline_;
  Microsoft::WRL::ComPtr<ID3D12Resource> temporal_aa_history_[2];
  Microsoft::WRL::ComPtr<ID3D12Resource> temporal_aa_output_;
  uint64_t temporal_aa_textures_submission_ = 0;
  // Upscalers on the same inputs (native AA modes: NVIDIA DLSS/DLAA, AMD FSR
  // 3.1, Intel XeSS), with camera motion vectors computed from the resolved
  // depth; the built-in resolve takes over when one is unavailable.
  struct TemporalAaUpscaler;
  TemporalAaUpscaler* temporal_aa_upscaler_ = nullptr;
  void TemporalAaInitialize();
  void TemporalAaShutdown();
  void TemporalAaDraw(uint64_t vertex_shader_hash, uint64_t pixel_shader_hash,
                      reg::RB_DEPTHCONTROL normalized_depth_control);
  void TemporalAaCopy();
  void TemporalAaEndFrame();
  bool TemporalAaResolve();
  bool TemporalAaEnsureResources(uint32_t width, uint32_t height);
  bool TemporalAaUpscalerInitialize(uint32_t kind);
  bool TemporalAaUpscalerEnsure(uint32_t width, uint32_t height);
  // Runs on the thread that executes the deferred command stream.
  static void TemporalAaUpscalerEvaluate(const void* payload,
                                         ID3D12GraphicsCommandList* command_list);
  // After a deferred ExternalCall: nothing bound on the command list is known.
  void InvalidateCommandListStateAfterExternalCall();

  // Unsubmitted barrier batch.
  std::vector<D3D12_RESOURCE_BARRIER> barriers_;

  // Init-only, default-off draw batching experiment. Reset only after an
  // actual submission; a rejected/failed early submission retains its budget.
  uint32_t guest_draw_submit_limit_ = 0;
  uint32_t submission_guest_draws_ = 0;
  uint64_t guest_draw_submit_count_ = 0;

  // <Submission where requested, resource>, sorted by the submission number.
  std::deque<std::pair<uint64_t, ID3D12Resource*>> resources_for_deletion_;

  static constexpr uint32_t kScratchBufferSizeIncrement = 16 * 1024 * 1024;
  ID3D12Resource* scratch_buffer_ = nullptr;
  uint32_t scratch_buffer_size_ = 0;
  D3D12_RESOURCE_STATES scratch_buffer_state_;
  bool scratch_buffer_used_ = false;

  ID3D12Resource* readback_buffer_ = nullptr;
  uint32_t readback_buffer_size_ = 0;
  std::unordered_map<uint64_t, ReadbackBuffer> readback_buffers_;
  std::unordered_map<uint64_t, ReadbackBuffer> memexport_readback_buffers_;

  static constexpr uint32_t kMaxOcclusionQueries = 8192;
  Microsoft::WRL::ComPtr<ID3D12QueryHeap> occlusion_query_heap_;
  Microsoft::WRL::ComPtr<ID3D12Resource> occlusion_query_readback_;
  uint64_t* occlusion_query_readback_mapping_ = nullptr;
  uint32_t occlusion_query_cursor_ = 0;
  bool occlusion_query_resources_available_ = false;
  XenosZPDReportAccumulator logical_occlusion_query_;
  struct ActiveOcclusionQuery {
    uint32_t host_index = UINT32_MAX;
    uint32_t scale_area = 1;
    uint32_t draws = 0;
    bool valid = false;
  } active_occlusion_query_;
  uint32_t zpd_submit_after_draws_ = 0;
  bool zpd_early_submission_logged_ = false;
  uint32_t zpd_early_end_count_ = 0;
  uint32_t zpd_early_max_segment_draws_ = 0;
  bool zpd_fence_check_active_ = false;
  Microsoft::WRL::ComPtr<ID3D12QueryHeap> zpd_gpu_timestamp_heap_;
  Microsoft::WRL::ComPtr<ID3D12Resource> zpd_gpu_timestamp_readback_;
  uint64_t* zpd_gpu_timestamp_mapping_ = nullptr;
  uint64_t zpd_gpu_timestamp_frequency_ = 0;
  bool zpd_gpu_timestamp_active_ = false;
  bool zpd_gpu_timestamp_pending_ = false;
  uint64_t zpd_gpu_timestamp_submission_ = 0;
  uint64_t zpd_gpu_calibration_window_ = 0;
  uint64_t zpd_gpu_calibration_gpu_tick_ = 0;
  uint64_t zpd_gpu_calibration_cpu_tick_ = 0;
  uint64_t zpd_gpu_submit_return_qpc_ = 0;
  uint64_t zpd_gpu_qpc_frequency_ = 0;
  bool zpd_gpu_calibration_valid_ = false;
  // Host segments in flight and guest reports awaiting their exact result.
  XenosZPDDeferredReports occlusion_reports_;
  std::unordered_map<uint32_t, uint32_t> occlusion_query_slot_values_;

  // Diagnostic-only stream-output capture of the actual host VS SV_Position
  // bytes for the eight matching internal-scale scene draws. The output and
  // filled-size counter share one default buffer; each bounded record is
  // copied to its own persistently mapped readback segment.
  static constexpr uint32_t kEmbeddedSceneHostVertexOutputRecordCount = 8;
  static constexpr uint32_t kEmbeddedSceneHostVertexOutputMaximumVertices =
      16384;
  static constexpr uint32_t kEmbeddedSceneHostVertexOutputDataSize =
      kEmbeddedSceneHostVertexOutputMaximumVertices * sizeof(float) * 4;
  static constexpr uint32_t kEmbeddedSceneHostVertexOutputRecordStride =
      kEmbeddedSceneHostVertexOutputDataSize + sizeof(uint64_t);
  struct EmbeddedSceneHostVertexOutputRecord {
    const char* kind = nullptr;
    uint64_t vertex_shader_hash = 0;
    uint32_t expected_vertex_count = 0;
  };
  Microsoft::WRL::ComPtr<ID3D12Resource>
      embedded_scene_host_vertex_output_buffer_;
  Microsoft::WRL::ComPtr<ID3D12Resource>
      embedded_scene_host_vertex_output_zero_upload_;
  Microsoft::WRL::ComPtr<ID3D12Resource>
      embedded_scene_host_vertex_output_readback_;
  uint8_t* embedded_scene_host_vertex_output_readback_mapping_ = nullptr;
  D3D12_RESOURCE_STATES embedded_scene_host_vertex_output_buffer_state_ =
      D3D12_RESOURCE_STATE_COPY_DEST;
  bool embedded_scene_host_vertex_output_resources_available_ = false;
  uint32_t embedded_scene_host_vertex_output_record_count_ = 0;
  std::array<EmbeddedSceneHostVertexOutputRecord,
             kEmbeddedSceneHostVertexOutputRecordCount>
      embedded_scene_host_vertex_output_records_{};
  struct VertexBufferState {
    uint32_t address = UINT32_MAX;
    uint32_t size = UINT32_MAX;
  };
  std::array<VertexBufferState, 96> vertex_buffer_states_{};
  uint64_t vertex_buffers_in_sync_[2] = {};
  // SharedMemory::invalidation_epoch() when the in-sync bits were last set:
  // any invalidation since then re-checks the residency of unchanged slots.
  uint64_t vertex_buffers_residency_epoch_ = UINT64_MAX;
#if REX_GPU_DIAGNOSTICS
  // Measurement builds (REX_VERTEX_SLOT_RESIDENCY): unchanged slots found
  // resident (skipped) or not (requested again; the pre-V351 cache skipped
  // these and drew from invalid pages), and changed slots.
  uint64_t vertex_slot_skipped_ = 0;
  uint64_t vertex_slot_revalidated_ = 0;
  uint64_t vertex_slot_changed_ = 0;
  uint64_t vertex_slot_frames_ = 0;
#endif

  std::atomic<bool> pix_capture_requested_ = false;
  bool pix_capturing_;

  // The current fixed-function drawing state.
  D3D12_VIEWPORT ff_viewport_;
  D3D12_RECT ff_scissor_;
  float ff_blend_factor_[4];
  uint32_t ff_stencil_ref_;
  bool ff_viewport_update_needed_;
  bool ff_scissor_update_needed_;
  bool ff_blend_factor_update_needed_;
  bool ff_stencil_ref_update_needed_;

  // Currently bound pipeline, either a graphics pipeline from the pipeline
  // cache (with potentially deferred creation - current_external_pipeline_ is
  // nullptr in this case) or a non-Xenos graphics or compute pipeline
  // (current_guest_pipeline_ is nullptr in this case).
  void* current_guest_pipeline_;
  ID3D12PipelineState* current_external_pipeline_;

  // Currently bound graphics root signature.
  ID3D12RootSignature* current_graphics_root_signature_;
  // Extra parameters which may or may not be present.
  RootBindfulExtraParameterIndices current_graphics_root_bindful_extras_;
  // Whether root parameters are up to date - reset if a new signature is bound.
  uint32_t current_graphics_root_up_to_date_;

  // System shader constants.
  DxbcShaderTranslator::SystemConstants system_constants_;

  // Float constant usage masks of the last draw call.
  uint64_t current_float_constant_map_vertex_[4];
  uint64_t current_float_constant_map_pixel_[4];

  // Constant buffer bindings.
  struct ConstantBufferBinding {
    D3D12_GPU_VIRTUAL_ADDRESS address;
    bool up_to_date;
  };
  ConstantBufferBinding cbuffer_binding_system_;
  ConstantBufferBinding cbuffer_binding_float_vertex_;
  ConstantBufferBinding cbuffer_binding_float_pixel_;
  ConstantBufferBinding cbuffer_binding_bool_loop_;
  ConstantBufferBinding cbuffer_binding_fetch_;
  ConstantBufferBinding cbuffer_binding_descriptor_indices_vertex_;
  ConstantBufferBinding cbuffer_binding_descriptor_indices_pixel_;

  // Bounded PRESS START diagnostics. The CPU pointer is the allocation backing
  // the currently bound tightly-packed vertex float constants, so the embedded
  // host can verify the data actually made visible to the host shader rather
  // than only inspecting the Xenos register file. Constant write provenance is
  // retained only for c0-c10, the range used by the verified prompt shaders.
  const uint8_t* embedded_float_vertex_cpu_address_ = nullptr;
  uint32_t embedded_float_vertex_cpu_size_ = 0;
  const uint8_t* embedded_float_pixel_cpu_address_ = nullptr;
  uint32_t embedded_float_pixel_cpu_size_ = 0;
  using EmbeddedFloatConstantWrite = pc_constant_writer::Record;
  static constexpr uint32_t kEmbeddedTrackedFloatConstantDwords = 20 * 4;
#if REX_GPU_DIAGNOSTICS
  std::array<EmbeddedFloatConstantWrite, kEmbeddedTrackedFloatConstantDwords>
      embedded_float_constant_writes_{};
  uint64_t embedded_float_constant_write_sequence_ = 0;
#else
  // Player builds: write provenance is recorded only with ring publication
  // tracking (measurement builds). Static storage that is never touched, so
  // the 14 KB record table no longer splits the binding state.
  static inline std::array<EmbeddedFloatConstantWrite, kEmbeddedTrackedFloatConstantDwords>
      embedded_float_constant_writes_{};
  static inline uint64_t embedded_float_constant_write_sequence_ = 0;
#endif

  // Whether the latest shared memory and EDRAM buffer binding contains the
  // shared memory UAV rather than the SRV.
  // Separate descriptor tables for the SRV and the UAV, even though only one is
  // accessed dynamically in the shaders, are used to prevent a validation
  // message about missing resource states in PIX.
  std::optional<bool> current_shared_memory_binding_is_uav_;

  // Pages with the descriptors currently used for handling Xenos draw calls.
  uint64_t draw_view_bindful_heap_index_;
  uint64_t draw_sampler_bindful_heap_index_;

  // Whether the last used texture sampler bindings have been written to the
  // current view descriptor heap.
  bool bindful_textures_written_vertex_;
  bool bindful_textures_written_pixel_;
  bool bindful_samplers_written_vertex_;
  bool bindful_samplers_written_pixel_;
  // Layout UIDs and last texture and sampler bindings written to the current
  // descriptor heaps (for bindful) or descriptor index constant buffer (for
  // bindless) with the last used descriptor layout. Valid only when:
  // - For bindful, when bindful_#_written_#_ is true.
  // - For bindless, when cbuffer_binding_descriptor_indices_#_.up_to_date is
  //   true.
  size_t current_texture_layout_uid_vertex_;
  size_t current_texture_layout_uid_pixel_;
  size_t current_sampler_layout_uid_vertex_;
  size_t current_sampler_layout_uid_pixel_;
  // Size of these should be ignored when checking whether these are up to date,
  // layout UID should be checked first (they will be different for different
  // binding counts).
  std::vector<D3D12TextureCache::TextureSRVKey> current_texture_srv_keys_vertex_;
  std::vector<D3D12TextureCache::TextureSRVKey> current_texture_srv_keys_pixel_;
  std::vector<D3D12TextureCache::SamplerParameters> current_samplers_vertex_;
  std::vector<D3D12TextureCache::SamplerParameters> current_samplers_pixel_;
  std::vector<uint32_t> current_sampler_bindless_indices_vertex_;
  std::vector<uint32_t> current_sampler_bindless_indices_pixel_;

  // Latest bindful descriptor handles used for handling Xenos draw calls.
  D3D12_GPU_DESCRIPTOR_HANDLE gpu_handle_shared_memory_srv_and_edram_;
  D3D12_GPU_DESCRIPTOR_HANDLE gpu_handle_shared_memory_uav_and_edram_;
  D3D12_GPU_DESCRIPTOR_HANDLE gpu_handle_textures_vertex_;
  D3D12_GPU_DESCRIPTOR_HANDLE gpu_handle_textures_pixel_;
  D3D12_GPU_DESCRIPTOR_HANDLE gpu_handle_samplers_vertex_;
  D3D12_GPU_DESCRIPTOR_HANDLE gpu_handle_samplers_pixel_;

  // Current primitive topology.
  D3D_PRIMITIVE_TOPOLOGY primitive_topology_;

  // Temporary storage for memexport stream constants used in the draw.
  std::vector<draw_util::MemExportRange> memexport_ranges_;

  // GPU timing diagnostic state (allocated only when d3d12_gpu_timing is on;
  // kept last so the object layout before it is unchanged).
  struct GpuTimingState;
  GpuTimingCategory GpuTimingSwitch(GpuTimingCategory category);
  // Boundary writes (frame and submission starts / ends) may use the reserve.
  void GpuTimingWrite(bool boundary = false);
  void GpuTimingAddCount(GpuTimingCounter counter, uint64_t value);
  bool GpuTimingLogFrame() const;
  void InitializeGpuTiming();
  void GpuTimingBeginSubmission(bool submission_opened, bool is_opening_frame);
  void GpuTimingEndSubmission();
  void GpuTimingCloseFrame();
  std::unique_ptr<GpuTimingState> gpu_timing_;
  struct GpuFrameMeter;
  void InitializeGpuFrameMeter();
  void GpuFrameMeterBeginSubmission(bool submission_opened, bool is_opening_frame);
  void GpuFrameMeterEndSubmission();
  void GpuFrameMeterCloseFrame();
  std::unique_ptr<GpuFrameMeter> gpu_frame_meter_;
};

}  // namespace rex::graphics::d3d12
