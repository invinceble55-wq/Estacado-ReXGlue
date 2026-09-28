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

#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include <rex/assert.h>
#include <rex/graphics/d3d12/pipeline_storage_seed.h>
#include <rex/graphics/d3d12/render_target_cache.h>
#include <rex/graphics/d3d12/shader.h>
#include <rex/graphics/flags.h>
#include <rex/graphics/pipeline/shader/dxbc_translator.h>
#include <rex/graphics/primitive_processor.h>
#include <rex/graphics/register_file.h>
#include <rex/graphics/registers.h>
#include <rex/graphics/xenos.h>
#include <rex/hash.h>
#include <rex/platform.h>
#include <rex/string/buffer.h>
#include <rex/thread.h>
#include <rex/ui/d3d12/d3d12_api.h>

namespace rex::graphics::d3d12 {

class D3D12CommandProcessor;

// Shader prewarm progress of this process (any thread): pipelines queued so
// far and pipelines handled (created, or taken over by a draw). The
// presenter's indicator shows the difference while it is not zero.
struct ShaderPrewarmStatus {
  uint32_t queued = 0;
  uint32_t handled = 0;
};
ShaderPrewarmStatus GetShaderPrewarmStatus();

class PipelineCache {
 public:
  static constexpr size_t kLayoutUIDEmpty = 0;

  PipelineCache(D3D12CommandProcessor& command_processor, const RegisterFile& register_file,
                const D3D12RenderTargetCache& render_target_cache, bool bindless_resources_used);
  ~PipelineCache();

  bool Initialize();
  void Shutdown();
  // No ClearCache because it's undesirable with the persistent shader storage
  // (if the storage is reloaded, effectively nothing is cleared, while the call
  // takes a long time, and if it's not, there will be heavy stuttering for the
  // rest of the execution of the guest).

  void InitializeShaderStorage(const std::filesystem::path& cache_root, uint32_t title_id,
                               bool blocking);
  void ShutdownShaderStorage();

  void EndSubmission();
  bool IsCreatingPipelines();

  D3D12Shader* LoadShader(xenos::ShaderType shader_type, const uint32_t* host_address,
                          uint32_t dword_count);
  // Analyze shader microcode on the translator thread.
  void AnalyzeShaderUcode(Shader& shader) { shader.AnalyzeUcode(ucode_disasm_buffer_); }

  // Retrieves the shader modification for the current state. The shader must
  // have microcode analyzed.
  DxbcShaderTranslator::Modification GetCurrentVertexShaderModification(
      const Shader& shader, Shader::HostVertexShaderType host_vertex_shader_type,
      uint32_t interpolator_mask) const;
  DxbcShaderTranslator::Modification GetCurrentPixelShaderModification(
      const Shader& shader, uint32_t interpolator_mask, uint32_t param_gen_pos,
      reg::RB_DEPTHCONTROL normalized_depth_control) const;

  // If draw_util::IsRasterizationPotentiallyDone is false, the pixel shader
  // MUST be made nullptr BEFORE calling this!
  bool ConfigurePipeline(D3D12Shader::D3D12Translation* vertex_shader,
                         D3D12Shader::D3D12Translation* pixel_shader,
                         const PrimitiveProcessor::ProcessingResult& primitive_processing_result,
                         reg::RB_DEPTHCONTROL normalized_depth_control,
                         uint32_t normalized_color_mask,
                         uint32_t bound_depth_and_color_render_target_bits,
                         const uint32_t* bound_depth_and_color_render_targets_formats,
                         void** pipeline_handle_out, ID3D12RootSignature** root_signature_out);

  // Returns a pipeline with deferred creation by its handle. May return nullptr
  // if failed to create the pipeline.
  ID3D12PipelineState* GetD3D12PipelineByHandle(void* handle) const {
    return reinterpret_cast<const Pipeline*>(handle)->state.load(std::memory_order_acquire);
  }

  struct TelemetrySnapshot {
    uint64_t current_reuses = 0;
    uint64_t cache_hits = 0;
    uint64_t cache_misses = 0;
    uint64_t async_queued = 0;
    uint64_t sync_created = 0;
    uint64_t async_completed = 0;
    uint64_t async_failed = 0;
    uint64_t queue_depth = 0;
    uint64_t threads_busy = 0;
  };

  // Shader prewarm progress for the presenter's indicator: pipelines the
  // prewarm queued and created so far (0/0 when it has nothing to do).
  struct PrewarmProgress {
    uint32_t queued = 0;
    uint32_t created = 0;
  };
  PrewarmProgress GetPrewarmProgress() const {
    return {prewarm_pipelines_queued_.load(std::memory_order_relaxed),
            prewarm_pipelines_created_.load(std::memory_order_relaxed)};
  }

  // Cumulative, low-cost counters. The command processor samples these once
  // per embedded frame and computes deltas, avoiding per-pipeline text output.
  TelemetrySnapshot GetTelemetrySnapshot();

 private:
  REXPACKEDSTRUCT(ShaderStoredHeader, {
    uint64_t ucode_data_hash;

    uint32_t ucode_dword_count : 31;
    xenos::ShaderType type : 1;

    static constexpr uint32_t kVersion = 0x20201219;
  });

  // Update PipelineDescription::kVersion if any of the Pipeline* enums are
  // changed!

  enum class PipelineStripCutIndex : uint32_t {
    kNone,
    kFFFF,
    kFFFFFFFF,
  };

  enum class PipelineTessellationMode : uint32_t {
    kNone,
    kDiscrete,
    kContinuous,
    kAdaptive,
  };

  enum class PipelinePatchType : uint32_t {
    kNone,
    kLine,
    kTriangle,
    kQuad,
  };

  enum class PipelinePrimitiveTopologyType : uint32_t {
    kPoint,
    kLine,
    kTriangle,
  };

  enum class PipelineGeometryShader : uint32_t {
    kNone,
    kPointList,
    kRectangleList,
    kQuadList,
  };

  enum class PipelineCullMode : uint32_t {
    kNone,
    kFront,
    kBack,
    // Special case, handled via disabling the pixel shader and depth / stencil.
    kDisableRasterization,
  };

  enum class PipelineBlendFactor : uint32_t {
    kZero,
    kOne,
    kSrcColor,
    kInvSrcColor,
    kSrcAlpha,
    kInvSrcAlpha,
    kDestColor,
    kInvDestColor,
    kDestAlpha,
    kInvDestAlpha,
    kBlendFactor,
    kInvBlendFactor,
    kSrcAlphaSat,
  };

  // Update PipelineDescription::kVersion if anything is changed!
  REXPACKEDSTRUCT(PipelineRenderTarget, {
    uint32_t used : 1;                          // 1
    xenos::ColorRenderTargetFormat format : 4;  // 5
    PipelineBlendFactor src_blend : 4;          // 9
    PipelineBlendFactor dest_blend : 4;         // 13
    xenos::BlendOp blend_op : 3;                // 16
    PipelineBlendFactor src_blend_alpha : 4;    // 20
    PipelineBlendFactor dest_blend_alpha : 4;   // 24
    xenos::BlendOp blend_op_alpha : 3;          // 27
    uint32_t write_mask : 4;                    // 31
  });

  REXPACKEDSTRUCT(PipelineDescription, {
    uint64_t vertex_shader_hash;
    uint64_t vertex_shader_modification;
    // 0 if drawing without a pixel shader.
    uint64_t pixel_shader_hash;
    uint64_t pixel_shader_modification;

    int32_t depth_bias;
    float depth_bias_slope_scaled;

    PipelineStripCutIndex strip_cut_index : 2;  // 2
    // PipelinePrimitiveTopologyType for a vertex shader.
    // xenos::TessellationMode for a domain shader.
    uint32_t primitive_topology_type_or_tessellation_mode : 2;  // 4
    // Zero for non-kVertex host_vertex_shader_type.
    PipelineGeometryShader geometry_shader : 2;       // 6
    uint32_t fill_mode_wireframe : 1;                 // 7
    PipelineCullMode cull_mode : 2;                   // 9
    uint32_t front_counter_clockwise : 1;             // 10
    uint32_t depth_clip : 1;                          // 11
    xenos::MsaaSamples host_msaa_samples : 2;         // 13
    xenos::DepthRenderTargetFormat depth_format : 1;  // 14
    xenos::CompareFunction depth_func : 3;            // 17
    uint32_t depth_write : 1;                         // 18
    uint32_t stencil_enable : 1;                      // 19
    uint32_t stencil_read_mask : 8;                   // 27
    // Native draw selected by draw_resolution_scale_threshold. This affects
    // the host slope-scaled depth-bias multiplier and therefore PSO identity.
    uint32_t resolution_scale_native : 1;             // 28
    // The explicit D24FS8 shader-depth conversion compatibility option affects
    // depth-only PSO identity.
    uint32_t depth_float24_convert_in_pixel_shader : 1;  // 29

    uint32_t stencil_write_mask : 8;                   // 8
    xenos::StencilOp stencil_front_fail_op : 3;        // 11
    xenos::StencilOp stencil_front_depth_fail_op : 3;  // 14
    xenos::StencilOp stencil_front_pass_op : 3;        // 17
    xenos::CompareFunction stencil_front_func : 3;     // 20
    xenos::StencilOp stencil_back_fail_op : 3;         // 23
    xenos::StencilOp stencil_back_depth_fail_op : 3;   // 26
    xenos::StencilOp stencil_back_pass_op : 3;         // 29
    xenos::CompareFunction stencil_back_func : 3;      // 32

    PipelineRenderTarget render_targets[xenos::kMaxColorRenderTargets];

    static constexpr uint32_t kVersion = 0x20260902;
  });

  REXPACKEDSTRUCT(PipelineStoredDescription, {
    uint64_t description_hash;
    PipelineDescription description;
  });

  struct PipelineRuntimeDescription {
    ID3D12RootSignature* root_signature;
    D3D12Shader::D3D12Translation* vertex_shader;
    D3D12Shader::D3D12Translation* pixel_shader;
    const std::vector<uint32_t>* geometry_shader;
    PipelineDescription description;
  };

  struct Pipeline;

  union GeometryShaderKey {
    uint32_t key;
    struct {
      PipelineGeometryShader type : 2;
      uint32_t interpolator_count : 5;
      uint32_t user_clip_plane_count : 3;
      uint32_t user_clip_plane_cull : 1;
      uint32_t has_vertex_kill_and : 1;
      uint32_t has_point_size : 1;
      uint32_t has_point_coordinates : 1;
      // PA_CL_CLIP_CNTL::ps_ucp_mode for point primitives.
      uint32_t point_ps_ucp_mode : 2;
    };

    GeometryShaderKey() : key(0) { static_assert_size(*this, sizeof(key)); }

    struct Hasher {
      size_t operator()(const GeometryShaderKey& key) const {
        return std::hash<uint32_t>{}(key.key);
      }
    };
    bool operator==(const GeometryShaderKey& other_key) const { return key == other_key.key; }
    bool operator!=(const GeometryShaderKey& other_key) const { return !(*this == other_key); }
  };

  D3D12Shader* LoadShader(xenos::ShaderType shader_type, const uint32_t* host_address,
                          uint32_t dword_count, uint64_t data_hash);

  // Can be called from multiple threads.
  bool TranslateAnalyzedShader(DxbcShaderTranslator& translator,
                               D3D12Shader::D3D12Translation& translation,
                               IDxbcConverter* dxbc_converter = nullptr,
                               IDxcUtils* dxc_utils = nullptr,
                               IDxcCompiler* dxc_compiler = nullptr);

  // If draw_util::IsRasterizationPotentiallyDone is false, the pixel shader
  // MUST be made nullptr BEFORE calling this! The shaders must be translated
  // and valid unless for_placeholder is true.
  bool GetCurrentStateDescription(
      D3D12Shader::D3D12Translation* vertex_shader, D3D12Shader::D3D12Translation* pixel_shader,
      const PrimitiveProcessor::ProcessingResult& primitive_processing_result,
      reg::RB_DEPTHCONTROL normalized_depth_control, uint32_t normalized_color_mask,
      uint32_t bound_depth_and_color_render_target_bits,
      const uint32_t* bound_depth_and_color_render_target_formats,
      PipelineRuntimeDescription& runtime_description_out, bool for_placeholder = false);

  static bool GetGeometryShaderKey(PipelineGeometryShader geometry_shader_type,
                                   DxbcShaderTranslator::Modification vertex_shader_modification,
                                   DxbcShaderTranslator::Modification pixel_shader_modification,
                                   GeometryShaderKey& key_out);
  static void CreateDxbcGeometryShader(GeometryShaderKey key, std::vector<uint32_t>& shader_out);
  const std::vector<uint32_t>& GetGeometryShader(GeometryShaderKey key);

  ID3D12PipelineState* CreateD3D12Pipeline(const PipelineRuntimeDescription& runtime_description);
  bool PrepareRuntimeDescriptionForQueuedCreation(Pipeline* pipeline,
                                                  PipelineRuntimeDescription& runtime_description);

  D3D12CommandProcessor& command_processor_;
  const RegisterFile& register_file_;
  const D3D12RenderTargetCache& render_target_cache_;
  bool bindless_resources_used_;

  // Temporary storage for AnalyzeUcode calls on the processor thread.
  string::StringBuffer ucode_disasm_buffer_;
  // Reusable shader translator for the processor thread.
  std::unique_ptr<DxbcShaderTranslator> shader_translator_;
  std::mutex translation_request_lock_;

  // Command processor thread DXIL conversion/disassembly interfaces, if DXIL
  // disassembly is enabled.
  IDxbcConverter* dxbc_converter_ = nullptr;
  IDxcUtils* dxc_utils_ = nullptr;
  IDxcCompiler* dxc_compiler_ = nullptr;

  // Ucode hash -> shader.
  std::unordered_map<uint64_t, D3D12Shader*, rex::IdentityHasher<uint64_t>> shaders_;

  struct LayoutUID {
    size_t uid;
    size_t vector_span_offset;
    size_t vector_span_length;
  };
  std::mutex layouts_mutex_;
  // Texture binding layouts of different shaders, for obtaining layout UIDs.
  std::vector<D3D12Shader::TextureBinding> texture_binding_layouts_;
  // Map of texture binding layouts used by shaders, for obtaining UIDs. Keys
  // are XXH3 hashes of layouts, values need manual collision resolution using
  // layout_vector_offset:layout_length of texture_binding_layouts_.
  std::unordered_multimap<uint64_t, LayoutUID, rex::IdentityHasher<uint64_t>>
      texture_binding_layout_map_;
  // Bindless sampler indices of different shaders, for obtaining layout UIDs.
  // For bindful, sampler count is used as the UID instead.
  std::vector<uint32_t> bindless_sampler_layouts_;
  // Keys are XXH3 hashes of used bindless sampler indices.
  std::unordered_multimap<uint64_t, LayoutUID, rex::IdentityHasher<uint64_t>>
      bindless_sampler_layout_map_;

  // Geometry shaders for Xenos primitive types not supported by Direct3D 12.
  std::unordered_map<GeometryShaderKey, std::vector<uint32_t>, GeometryShaderKey::Hasher>
      geometry_shaders_;

  // Empty depth-only pixel shader for writing to depth buffer via ROV when no
  // Xenos pixel shader provided.
  std::vector<uint8_t> depth_only_pixel_shader_;

  // Prewarm states of a pipeline (Pipeline::prewarm).
  enum : uint8_t {
    kPrewarmNone = 0,  // created for a draw or from the storage
    kPrewarmQueued,    // waiting in prewarm_creation_queue_
    kPrewarmCreating,  // a prewarm thread is creating it
    kPrewarmDone,      // created by a prewarm thread
    kPrewarmClaimed,   // a draw took it over from the prewarm queue
  };
  struct Pipeline {
    // nullptr if creation has failed.
    std::atomic<ID3D12PipelineState*> state{nullptr};
    std::atomic<ID3D12RootSignature*> root_signature{nullptr};
    PipelineRuntimeDescription description;
    D3D12Shader::D3D12Translation* pending_vertex_shader = nullptr;
    D3D12Shader::D3D12Translation* pending_pixel_shader = nullptr;
    uint8_t priority = 0;
    std::atomic<uint8_t> prewarm{kPrewarmNone};
  };
  struct PipelineCreationPriorityComparator {
    bool operator()(const Pipeline* a, const Pipeline* b) const {
      uint8_t priority_a = a ? a->priority : 0;
      uint8_t priority_b = b ? b->priority : 0;
      return priority_a < priority_b;
    }
  };
  // All previously generated pipelines identified by hash and the description.
  std::unordered_multimap<uint64_t, Pipeline*, rex::IdentityHasher<uint64_t>> pipelines_;

  // Previously used pipeline. This matches our current state settings and
  // allows us to quickly(ish) reuse the pipeline if no registers have been
  // changed.
  Pipeline* current_pipeline_ = nullptr;

  // Currently open shader storage path.
  std::filesystem::path shader_storage_cache_root_;
  uint32_t shader_storage_title_id_ = 0;

  // Shader storage output stream, for preload in the next emulator runs.
  FILE* shader_storage_file_ = nullptr;
  // For only writing shaders to the currently open storage once, incremented
  // when switching the storage.
  uint32_t shader_storage_index_ = 0;
  bool shader_storage_file_flush_needed_ = false;

  // Pipeline storage output stream, for preload in the next emulator runs.
  FILE* pipeline_storage_file_ = nullptr;
  bool pipeline_storage_file_flush_needed_ = false;

  // Thread for asynchronous writing to the storage streams.
  void StorageWriteThread();
  std::mutex storage_write_request_lock_;
  std::condition_variable storage_write_request_cond_;
  // Storage thread input is protected with storage_write_request_lock_, and the
  // thread is notified about its change via storage_write_request_cond_.
  std::deque<const Shader*> storage_write_shader_queue_;
  std::deque<PipelineStoredDescription> storage_write_pipeline_queue_;
  bool storage_write_flush_shaders_ = false;
  bool storage_write_flush_pipelines_ = false;
  bool storage_write_thread_shutdown_ = false;
  std::unique_ptr<rex::thread::Thread> storage_write_thread_;

  // Pipeline creation threads.
  void CreationThread(size_t thread_index);
  void CreateQueuedPipelinesOnProcessorThread();
  std::mutex creation_request_lock_;
  std::condition_variable creation_request_cond_;
  // Protected with creation_request_lock_, notify_one creation_request_cond_
  // when set.
  std::priority_queue<Pipeline*, std::vector<Pipeline*>, PipelineCreationPriorityComparator>
      creation_queue_;
  // Number of threads that are currently creating a pipeline - incremented when
  // a pipeline is dequeued (the completion event can't be triggered before this
  // is zero). Protected with creation_request_lock_.
  size_t creation_threads_busy_ = 0;
  // Manual-reset event set when the last queued pipeline is created and there
  // are no more pipelines to create. This is triggered by the thread creating
  // the last pipeline.
  std::unique_ptr<rex::thread::Event> creation_completion_event_;
  // Whether setting the event on completion is queued. Protected with
  // creation_request_lock_, notify_one creation_request_cond_ when set.
  bool creation_completion_set_event_ = false;
  // Creation threads with this index or above need to be shut down as soon as
  // possible. Protected with creation_request_lock_, notify_all
  // creation_request_cond_ when set.
  size_t creation_threads_shutdown_from_ = SIZE_MAX;
  std::vector<std::unique_ptr<rex::thread::Thread>> creation_threads_;

  // ConfigurePipeline runs on the command processor thread. Completion may be
  // reported by worker threads, so only those counters need atomics.
  uint64_t telemetry_current_reuses_ = 0;
  uint64_t telemetry_cache_hits_ = 0;
  uint64_t telemetry_cache_misses_ = 0;
  uint64_t telemetry_async_queued_ = 0;
  uint64_t telemetry_sync_created_ = 0;
  std::atomic<uint64_t> telemetry_async_completed_{0};
  std::atomic<uint64_t> telemetry_async_failed_{0};

  // Prewarm (Estacado 0.9.1, #5). The game builds most of its shaders at run
  // time, so no game file holds their microcode. The packaged shader index
  // (pipeline_storage_seed.h, no microcode) names the shaders the stored
  // pipeline descriptions need; a scan thread recognises them in guest
  // physical memory once the game has created them (typically while a level
  // loads), translates them and hands them to the processor thread, which
  // adopts them at EndSubmission and queues the pipelines whose shaders are
  // all present on prewarm creation threads, before their first draw. A draw
  // that needs a pipeline still in that queue takes it over (normal creation);
  // one being created is awaited at EndSubmission. Found shaders also go to
  // the shader storage, so the next start preloads them.
  void StartPrewarm(const std::filesystem::path& index_path,
                    const std::vector<PipelineStoredDescription>& stored_descriptions);
  void StopPrewarm();
  void PrewarmScanThread();
  void PrewarmCreationThread();
  void AdoptPrewarmedShaders();
  void ClaimPrewarmPipeline(Pipeline* pipeline);
  void AwaitClaimedPrewarmPipelines();
  void ReportPipelineUse();
  // Scan thread input, fixed while it runs: indexed shaders not yet loaded,
  // sorted by prefix hash, a bit filter of the prefix hashes and the
  // modifications the pending descriptions need per shader.
  std::vector<pipeline_storage_seed::ShaderIndexEntry> prewarm_index_;
  std::vector<uint64_t> prewarm_prefix_filter_;
  std::unordered_map<uint64_t, std::vector<uint64_t>> prewarm_modifications_;
  // Indexed shaders the scan thread first rebuilds from the title's own
  // ProgramCache (<title>.xshp + gpu_program_cache_source), before the game
  // creates them: the whole list is ready at startup, even on a first start.
  std::vector<pipeline_storage_seed::ShaderPatchEntry> prewarm_rebuild_;
  std::vector<uint8_t> prewarm_program_cache_;
  // Descriptions whose shaders were missing at the preload (processor thread).
  std::vector<PipelineStoredDescription> prewarm_pending_;
  std::mutex prewarm_lock_;
  std::condition_variable prewarm_cond_;
  bool prewarm_shutdown_ = false;  // prewarm_lock_
  std::vector<D3D12Shader*> prewarm_ready_;       // prewarm_lock_: scan -> processor
  std::deque<Pipeline*> prewarm_creation_queue_;  // prewarm_lock_
  std::vector<Pipeline*> prewarm_claimed_creating_;  // processor thread, this submission
  std::unique_ptr<rex::thread::Thread> prewarm_scan_thread_;
  std::vector<std::unique_ptr<rex::thread::Thread>> prewarm_creation_threads_;
  std::atomic<uint32_t> prewarm_shaders_found_{0};
  std::atomic<uint32_t> prewarm_pipelines_queued_{0};
  std::atomic<uint32_t> prewarm_pipelines_created_{0};
  uint32_t prewarm_shaders_adopted_ = 0;   // processor thread
  uint32_t prewarm_pipelines_claimed_ = 0;  // processor thread
  // Pipelines created because a draw needed them (each a possible hitch on a
  // cold driver cache), reported with the prewarm counters.
  uint64_t draw_created_pipelines_ = 0;
  // Submissions that waited at least 0.5 ms for pipeline creation, their
  // total and longest wait, and how many waited 16 ms or more.
  uint64_t creation_waits_ = 0;
  uint64_t creation_wait_us_total_ = 0;
  uint64_t creation_wait_us_max_ = 0;
  uint64_t creation_waits_over_16ms_ = 0;
  uint64_t reported_draw_created_pipelines_ = 0;  // signature of the last report
  uint64_t reported_pipeline_use_tick_ = 0;
};

}  // namespace rex::graphics::d3d12
