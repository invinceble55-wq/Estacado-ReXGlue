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

#include <array>
#include <functional>
#include <future>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <rex/assert.h>
#include <rex/graphics/d3d12/shader.h>
#include <rex/graphics/d3d12/shared_memory.h>
#include <rex/graphics/pipeline/texture/cache.h>
#include <rex/graphics/pipeline/texture/prompt_icons.h>
#include <rex/graphics/pipeline/texture/texture_pack.h>
#include <rex/graphics/pipeline/texture/util.h>
#include <rex/graphics/embedded_texture_readback_policy.h>
#include <rex/graphics/offset_allocator.h>
#include <rex/graphics/pc_scene_color_history.h>
#include <rex/graphics/pc_draw_transform_history.h>
#include <rex/graphics/register_file.h>
#include <rex/graphics/xenos.h>
#include <rex/ui/d3d12/d3d12_api.h>
#include <rex/ui/d3d12/d3d12_provider.h>

namespace rex::graphics::d3d12 {

class D3D12CommandProcessor;

class D3D12TextureCache final : public TextureCache {
 public:
  // Keys that can be stored for checking validity whether descriptors for host
  // shader bindings are up to date.
  struct TextureSRVKey {
    TextureKey key;
    uint32_t host_swizzle;
    uint8_t swizzled_signs;
  };

  // Sampler parameters that can be directly converted to a host sampler or used
  // for binding checking validity whether samplers are up to date.
  union SamplerParameters {
    uint32_t value;
    struct {
      xenos::ClampMode clamp_x : 3;         // 3
      xenos::ClampMode clamp_y : 3;         // 6
      xenos::ClampMode clamp_z : 3;         // 9
      xenos::BorderColor border_color : 2;  // 11
      // For anisotropic, these are true.
      uint32_t mag_linear : 1;              // 12
      uint32_t min_linear : 1;              // 13
      uint32_t mip_linear : 1;              // 14
      xenos::AnisoFilter aniso_filter : 3;  // 17
      uint32_t mip_min_level : 4;           // 21
      uint32_t mip_base_map : 1;            // 22
      // Maximum mip level is in the texture resource itself, but mip_base_map
      // can be used to limit fetching to mip_min_level.
    };

    SamplerParameters() : value(0) { static_assert_size(*this, sizeof(value)); }
    bool operator==(const SamplerParameters& parameters) const { return value == parameters.value; }
    bool operator!=(const SamplerParameters& parameters) const { return value != parameters.value; }
  };

  static std::unique_ptr<D3D12TextureCache> Create(const RegisterFile& register_file,
                                                   D3D12SharedMemory& shared_memory,
                                                   uint32_t draw_resolution_scale_x,
                                                   uint32_t draw_resolution_scale_y,
                                                   D3D12CommandProcessor& command_processor,
                                                   bool bindless_resources_used) {
    std::unique_ptr<D3D12TextureCache> texture_cache(
        new D3D12TextureCache(register_file, shared_memory, draw_resolution_scale_x,
                              draw_resolution_scale_y, command_processor, bindless_resources_used));
    if (!texture_cache->Initialize()) {
      return nullptr;
    }
    return std::move(texture_cache);
  }

  ~D3D12TextureCache();

  void ClearCache() override;

  void BeginSubmission(uint64_t new_submission_index) override;
  void BeginFrame() override;
  void EndFrame();

  // Arms the same bounded one-shot loader/resource readback for the actual
  // output interval selected by a semantic resolve diagnostic. This avoids
  // relying on a title allocation address observed in an earlier process.
  void ArmTextureReadbackDiagnostic(uint32_t guest_address,
                                    uint32_t guest_length);

  // Must be called within a submission - creates and untiles textures needed by
  // shaders and puts them in the SRV state. This may bind compute pipelines
  // (notifying the command processor about that), so this must be called before
  // binding the actual drawing pipeline.
  void RequestTextures(uint32_t used_texture_mask) override;

  // Returns whether texture SRV keys stored externally are still valid for the
  // current bindings and host shader binding layout. Both keys and
  // host_shader_bindings must have host_shader_binding_count elements
  // (otherwise they are incompatible - like if this function returned false).
  bool AreActiveTextureSRVKeysUpToDate(const TextureSRVKey* keys,
                                       const D3D12Shader::TextureBinding* host_shader_bindings,
                                       size_t host_shader_binding_count) const;
  // Exports the current binding data to texture SRV keys so they can be stored
  // for checking whether subsequent draw calls can keep using the same
  // bindings. Write host_shader_binding_count keys.
  void WriteActiveTextureSRVKeys(TextureSRVKey* keys,
                                 const D3D12Shader::TextureBinding* host_shader_bindings,
                                 size_t host_shader_binding_count) const;
  void WriteActiveTextureBindfulSRV(const D3D12Shader::TextureBinding& host_shader_binding,
                                    D3D12_CPU_DESCRIPTOR_HANDLE handle);
  uint32_t GetActiveTextureBindlessSRVIndex(const D3D12Shader::TextureBinding& host_shader_binding);

  // Glow reconstruction with dedicated images (#16, graphics_glow_reconstruction
  // "dedicated"): for the fetches of the next RequestTextures only, a
  // resolution-scaled texture is replaced by an image made from it in its own
  // committed, zero-initialized memory (never placed or aliased with the
  // scaled resolve memory or other textures), with explicit transitions:
  // kNative, its box-reduced native cells at the guest size (for a pass
  // rasterized on the native grid); kReconstructed, the native bilinear of
  // those cells at every host texel (for a scaled output). The title's own
  // shaders then sample them; the reconstruction shader variant is not used.
  enum class GlowImageKind : uint8_t { kNative, kReconstructed };
  enum class GlowImageFootprint : uint8_t { kUnbounded, kRegion, kSourceNative };
  struct GlowImageRequest {
    GlowImageKind kind = GlowImageKind::kNative;
    GlowImageFootprint footprint = GlowImageFootprint::kUnbounded;
    // kRegion: native texels [left, top, right, bottom).
    int32_t region[4] = {};
  };
  void RequestGlowImages(uint32_t fetch_mask, const GlowImageRequest* requests) {
    glow_image_request_mask_ = fetch_mask;
    // Called for every draw: nothing to copy for the draws without glow.
    if (!fetch_mask) return;
    for (uint32_t i = 0; i < 32; ++i) {
      if (fetch_mask & (UINT32_C(1) << i)) glow_image_requests_[i] = requests[i];
    }
  }
  // As of the latest RequestTextures.
  bool IsActiveTextureGlowImage(uint32_t fetch_constant_index) const {
    return (glow_image_bound_mask_ >> fetch_constant_index) & 1;
  }
  bool IsActiveTextureNativeGlowImage(uint32_t fetch_constant_index) const {
    return IsActiveTextureGlowImage(fetch_constant_index) &&
           glow_image_bound_[fetch_constant_index]->kind == GlowImageKind::kNative;
  }
  // Changes whenever the set of bound glow images changes (descriptor
  // indices written for an earlier draw are then stale).
  uint64_t glow_image_binding_generation() const { return glow_image_binding_generation_; }

  SamplerParameters GetSamplerParameters(const D3D12Shader::SamplerBinding& binding) const;
  void WriteSampler(SamplerParameters parameters, D3D12_CPU_DESCRIPTOR_HANDLE handle) const;

  // Returns whether the actual scale is not smaller than the requested one.
  static bool ClampDrawResolutionScaleToMaxSupported(uint32_t& scale_x, uint32_t& scale_y,
                                                     const ui::d3d12::D3D12Provider& provider);
  // Ensures the tiles backing the range in the buffers are allocated.
  bool EnsureScaledResolveMemoryCommitted(uint32_t start_unscaled, uint32_t length_unscaled,
                                          uint32_t length_scaled_alignment_log2 = 0) override;
  // Materializes pages that don't currently have a resolution-scaled
  // representation from authoritative 1x shared memory, using the scaled tiled
  // group layout. Used both before a partial resolve and when loading a texture
  // that contains a mixture of scaled and unscaled pages.
  bool InitializeUnscaledResolvePagesFromSharedMemory(
      uint32_t start_unscaled, uint32_t length_unscaled,
      uint32_t bytes_per_block_log2);
  // Makes the specified range of up to 1-2 GB currently accessible on the GPU.
  // One draw call can access only at most one range - the same memory is
  // accessible through different buffers based on the range needed, so aliasing
  // barriers are required.
  bool MakeScaledResolveRangeCurrent(uint32_t start_unscaled, uint32_t length_unscaled,
                                     uint32_t length_scaled_alignment_log2 = 0);
  // These functions create a view of the range specified in the last successful
  // MakeScaledResolveRangeCurrent call because that function must be called
  // before this.
  void CreateCurrentScaledResolveRangeUintPow2SRV(D3D12_CPU_DESCRIPTOR_HANDLE handle,
                                                  uint32_t element_size_bytes_pow2);
  void CreateCurrentScaledResolveRangeUintPow2UAV(D3D12_CPU_DESCRIPTOR_HANDLE handle,
                                                   uint32_t element_size_bytes_pow2);
  void CreateCurrentScaledResolveRangeRawUAV(
      D3D12_CPU_DESCRIPTOR_HANDLE handle);
  void TransitionCurrentScaledResolveRange(D3D12_RESOURCE_STATES new_state);
  uint64_t GetCurrentScaledResolveRangeStartScaled() const {
    return scaled_resolve_current_range_start_scaled_;
  }
  uint64_t GetCurrentScaledResolveRangeLengthScaled() const {
    return scaled_resolve_current_range_length_scaled_;
  }
  ID3D12Resource* GetCurrentScaledResolveBufferResource() {
    return GetCurrentScaledResolveBuffer().resource();
  }
  size_t GetCurrentScaledResolveBufferIndexPublic() const {
    return GetCurrentScaledResolveBufferIndex();
  }
  void MarkCurrentScaledResolveRangeUAVWritesCommitNeeded() {
    assert_true(IsDrawResolutionScaled());
    GetCurrentScaledResolveBuffer().SetUAVBarrierPending();
  }
  // Reads back exactly one scaled resolve output interval after its compute
  // dispatch. This is an opt-in, synchronous diagnostic boundary used to
  // distinguish resolve conversion from the later texture loader.
  bool CaptureCurrentScaledResolveRange(uint32_t start_unscaled,
                                        uint32_t length_unscaled,
                                        const char* diagnostic_label,
                                        const char* dump_path);

  // Returns the ID3D12Resource of the front buffer texture (in
  // NON_PIXEL_SHADER_RESOURCE state), or nullptr in case of failure, and writes
  // the description of its SRV. May call LoadTextureData, so the same
  // restrictions (such as about descriptor heap change possibility) apply.
  ID3D12Resource* RequestSwapTexture(D3D12_SHADER_RESOURCE_VIEW_DESC& srv_desc_out,
                                     xenos::TextureFormat& format_out,
                                     uint32_t* width_unscaled_out = nullptr,
                                     uint32_t* height_unscaled_out = nullptr);

  // Bounded diagnostics for proving the exact resource selected by a texture
  // fetch without exposing the cache's binding implementation to the command
  // processor. This is observational only and does not retain the resource.
  struct ActiveTextureDiagnostic {
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
  bool GetActiveTextureDiagnostic(uint32_t fetch_constant_index,
                                  ActiveTextureDiagnostic& diagnostic_out) const;
  void CaptureActiveTextureReadbackDiagnostics(
      const uint32_t* fetch_constant_indices, size_t fetch_constant_count,
      bool scaled_resolve_only = false, uint64_t context_draw_ordinal = 0,
      bool distinguish_draw_epoch = false);
  void CaptureFirstTemporalDepthBinding(uint32_t used_texture_mask,
                                       uint64_t context_draw_ordinal);

  // Actual final-composite scene input, preserved before mutable cache reuse.
  // Color storage only: these resources do not assert reprojection validity.
  bool OwnsSceneHistory() const;
  void RecordOwnedDrawTransform(const pc_draw_transform_history::Draw& draw, bool valid);
  void CopyOwnedSceneColor(uint64_t pixel_shader, bool verify_copy);
  struct OwnedSceneColorPair {
    ID3D12Resource* current = nullptr;
    ID3D12Resource* previous = nullptr;
    uint64_t current_frame = 0, previous_frame = 0, epoch = 0;
    uint32_t width = 0, height = 0, host_swizzle = 0;
    uint8_t swizzled_signs = 0;
    int32_t sample_exponent = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    // Optional exact per-draw inputs from these same color frames. Neither
    // array order nor matching bytes supplies cross-frame instance identity.
    const pc_draw_transform_history::Frame* current_draws = nullptr;
    const pc_draw_transform_history::Frame* previous_draws = nullptr;
    // Projection/viewport equality only, never camera-cut/history acceptance.
    bool projection_unchanged = false;
  };
  // Borrowed until the next frame boundary, same CP thread/queue only. Commands
  // must be recorded in the current submission; this call extends reuse fences.
  OwnedSceneColorPair UseOwnedSceneColor();

  // Temporal AA (V397, command_processor_temporal_aa.cpp): the host texture
  // bound to a fetch constant for the current draw, and the resolved scene
  // depth requested by key (the title writes it but samples it nowhere in the
  // frame). Borrowed for the current submission; `handle` moves the texture
  // between states through the cache's own state tracking.
  struct TemporalAaTexture {
    void* handle = nullptr;
    ID3D12Resource* resource = nullptr;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t guest_base = 0;
    uint32_t guest_format = 0;
  };
  bool GetTemporalAaBoundTexture(uint32_t fetch_constant_index,
                                 TemporalAaTexture& out) const;
  bool RequestTemporalAaDepth(uint32_t guest_base, uint32_t width, uint32_t height,
                              xenos::Endian endian, TemporalAaTexture& out);
  void TransitionTemporalAaTexture(void* handle, D3D12_RESOURCE_STATES state);
  void InvalidateOwnedSceneColor();

 protected:
  bool IsSignedVersionSeparateForFormat(TextureKey key) const override;
  bool IsScaledResolveSupportedForFormat(TextureKey key) const override;
  uint32_t GetHostFormatSwizzle(TextureKey key) const override;

  uint32_t GetMaxHostTextureWidthHeight(xenos::DataDimension dimension) const override;
  uint32_t GetMaxHostTextureDepthOrArraySize(xenos::DataDimension dimension) const override;

  std::unique_ptr<Texture> CreateTexture(TextureKey key) override;

  // This binds pipelines, allocates descriptors, and copies!
  bool LoadTextureDataFromResidentMemoryImpl(Texture& texture, bool load_base,
                                             bool load_mips) override;

  void UpdateTextureBindingsImpl(uint32_t fetch_constant_mask) override;

 private:
  static constexpr uint32_t kLoadGuestXThreadsPerGroupLog2 = 2;
  static constexpr uint32_t kLoadGuestYBlocksPerGroupLog2 = 5;

  struct HostFormat {
    // Format info for the regular case.
    // DXGI format (typeless when different signedness or number representation
    // is used) for the texture resource.
    DXGI_FORMAT dxgi_format_resource;
    // DXGI format for unsigned normalized or unsigned/signed float SRV.
    DXGI_FORMAT dxgi_format_unsigned;
    // The regular load shader, used when special load shaders (like
    // signed-specific or decompressing) aren't needed.
    LoadShaderIndex load_shader;
    // DXGI format for signed normalized or unsigned/signed float SRV.
    DXGI_FORMAT dxgi_format_signed;
    // If the signed version needs a different bit representation on the host,
    // this is the load shader for the signed version. Otherwise the regular
    // load_shader will be used for the signed version, and a single copy will
    // be created if both unsigned and signed are used.
    LoadShaderIndex load_shader_signed;

    // Do NOT add integer DXGI formats to this - they are not filterable, can
    // only be read with Load, not Sample! If any game is seen using num_format
    // 1 for fixed-point formats (for floating-point, it's normally set to 1
    // though), add a constant buffer containing multipliers for the
    // textures and multiplication to the tfetch implementation.

    // Whether the DXGI format, if not uncompressing the texture, consists of
    // blocks, thus copy regions must be aligned to block size (assuming it's
    // the same as the guest block size).
    bool is_block_compressed;
    // Uncompression info for when the regular host format for this texture is
    // block-compressed, but the size is not block-aligned, and thus such
    // texture cannot be created in Direct3D on PC and needs decompression,
    // however, such textures are common, for instance, in 4D5307E6. This only
    // supports unsigned normalized formats - let's hope GPUSIGN_SIGNED was not
    // used for DXN and DXT5A.
    DXGI_FORMAT dxgi_format_uncompressed;
    LoadShaderIndex load_shader_decompress;

    // Mapping of Xenos swizzle components to DXGI format components.
    uint32_t swizzle;
  };

  class D3D12Texture final : public Texture {
   public:
    union SRVDescriptorKey {
      uint32_t key;
      struct {
        uint32_t is_signed : 1;
        uint32_t host_swizzle : 12;
        uint32_t dimension : 2;
      };

      SRVDescriptorKey() : key(0) { static_assert_size(*this, sizeof(key)); }

      struct Hasher {
        size_t operator()(const SRVDescriptorKey& key) const {
          return std::hash<decltype(key.key)>{}(key.key);
        }
      };
      bool operator==(const SRVDescriptorKey& other_key) const { return key == other_key.key; }
      bool operator!=(const SRVDescriptorKey& other_key) const { return !(*this == other_key); }
    };

    ID3D12Resource* GetOrCreate3DAs2DResource(D3D12_RESOURCE_STATES end_state);

    explicit D3D12Texture(D3D12TextureCache& texture_cache, const TextureKey& key,
                          ID3D12Resource* resource, D3D12_RESOURCE_STATES resource_state,
                          bool track_usage = true);
    ~D3D12Texture();

    ID3D12Resource* resource() const { return resource_.Get(); }

    D3D12_RESOURCE_STATES SetResourceState(D3D12_RESOURCE_STATES new_state) {
      D3D12_RESOURCE_STATES old_state = resource_state_;
      resource_state_ = new_state;
      return old_state;
    }

    uint32_t GetSRVDescriptorIndex(SRVDescriptorKey descriptor_key) const {
      auto it = srv_descriptors_.find(descriptor_key);
      return it != srv_descriptors_.cend() ? it->second : UINT32_MAX;
    }

    void AddSRVDescriptorIndex(SRVDescriptorKey descriptor_key, uint32_t descriptor_index) {
      srv_descriptors_.emplace(descriptor_key, descriptor_index);
    }

    // HD texture packs (gpu_texture_replace): while a pack replacement is
    // shown, resource() is the replacement (always in the shader-resource
    // state, possibly shared with other textures of the same content) and the
    // texture's own guest-sized resource waits here.
    bool replaced() const { return replaced_original_ != nullptr; }
    uint64_t replacement_id() const { return replacement_id_; }
    void ShowReplacement(uint64_t id, Microsoft::WRL::ComPtr<ID3D12Resource> replacement,
                         D3D12_RESOURCE_STATES replacement_state) {
      replaced_original_ = std::move(resource_);
      replaced_original_state_ = resource_state_;
      resource_ = std::move(replacement);
      resource_state_ = replacement_state;
      replacement_id_ = id;
    }
    // Back to the texture's own resource; returns the replacement reference.
    Microsoft::WRL::ComPtr<ID3D12Resource> RestoreOriginal() {
      Microsoft::WRL::ComPtr<ID3D12Resource> replacement = std::move(resource_);
      resource_ = std::move(replaced_original_);
      resource_state_ = replaced_original_state_;
      replacement_id_ = 0;
      return replacement;
    }
    // Moves the cached view descriptors out (the caller releases them once the
    // GPU no longer uses them).
    void TakeSRVDescriptors(std::vector<uint32_t>& descriptors_out) {
      for (const auto& descriptor_pair : srv_descriptors_) {
        descriptors_out.push_back(descriptor_pair.second);
      }
      srv_descriptors_.clear();
    }

    // Placement in the texture heap pool; the range is returned to the pool
    // after the resource is released (textures are destroyed only once the
    // GPU has completed their last use).
    void SetHeapPlacement(int32_t heap_index, uint64_t offset, uint64_t size) {
      heap_index_ = heap_index;
      heap_offset_ = offset;
      heap_size_ = size;
    }

    // Incremented by every load of the texture's data (dedicated glow images
    // made from it are then stale).
    uint64_t content_epoch() const { return content_epoch_; }
    void BumpContentEpoch() { ++content_epoch_; }

   private:
    uint64_t content_epoch_ = 0;
    Microsoft::WRL::ComPtr<ID3D12Resource> resource_;
    D3D12_RESOURCE_STATES resource_state_;
    int32_t heap_index_ = -1;
    uint64_t heap_offset_ = 0;
    uint64_t heap_size_ = 0;
    std::unique_ptr<D3D12Texture> texture_3d_as_2d_;

    // For bindful - indices in the non-shader-visible descriptor cache for
    // copying to the shader-visible heap (much faster than recreating, which,
    // according to profiling, was often a bottleneck in many games).
    // For bindless - indices in the global shader-visible descriptor heap.
    std::unordered_map<SRVDescriptorKey, uint32_t, SRVDescriptorKey::Hasher> srv_descriptors_;

    Microsoft::WRL::ComPtr<ID3D12Resource> replaced_original_;
    D3D12_RESOURCE_STATES replaced_original_state_ = D3D12_RESOURCE_STATE_COMMON;
    uint64_t replacement_id_ = 0;
  };

  static constexpr uint32_t kSRVDescriptorCachePageSize = 65536;

  struct SRVDescriptorCachePage {
   public:
    explicit SRVDescriptorCachePage(ID3D12DescriptorHeap* heap)
        : heap_(heap), heap_start_(heap->GetCPUDescriptorHandleForHeapStart()) {}
    SRVDescriptorCachePage(const SRVDescriptorCachePage& page) = delete;
    SRVDescriptorCachePage& operator=(const SRVDescriptorCachePage& page) = delete;
    SRVDescriptorCachePage(SRVDescriptorCachePage&& page) {
      std::swap(heap_, page.heap_);
      std::swap(heap_start_, page.heap_start_);
    }
    SRVDescriptorCachePage& operator=(SRVDescriptorCachePage&& page) {
      std::swap(heap_, page.heap_);
      std::swap(heap_start_, page.heap_start_);
      return *this;
    }

    ID3D12DescriptorHeap* heap() const { return heap_.Get(); }
    D3D12_CPU_DESCRIPTOR_HANDLE heap_start() const { return heap_start_; }

   private:
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> heap_;
    D3D12_CPU_DESCRIPTOR_HANDLE heap_start_;
  };

  struct D3D12TextureBinding {
    // Descriptor indices of texture and texture_signed of the respective
    // TextureBinding returned from FindOrCreateTextureDescriptor.
    uint32_t descriptor_index;
    uint32_t descriptor_index_signed;

    D3D12TextureBinding() { Reset(); }

    void Reset() {
      descriptor_index = UINT32_MAX;
      descriptor_index_signed = UINT32_MAX;
    }
  };

  class ScaledResolveVirtualBuffer {
   public:
    explicit ScaledResolveVirtualBuffer(ID3D12Resource* resource,
                                        D3D12_RESOURCE_STATES resource_state)
        : resource_(resource), resource_state_(resource_state) {}
    ID3D12Resource* resource() const { return resource_.Get(); }
    D3D12_RESOURCE_STATES SetResourceState(D3D12_RESOURCE_STATES new_state) {
      D3D12_RESOURCE_STATES old_state = resource_state_;
      if (old_state == D3D12_RESOURCE_STATE_UNORDERED_ACCESS) {
        uav_barrier_pending_ = false;
      }
      resource_state_ = new_state;
      return old_state;
    }
    // After writing through a UAV.
    void SetUAVBarrierPending() {
      if (resource_state_ == D3D12_RESOURCE_STATE_UNORDERED_ACCESS) {
        uav_barrier_pending_ = true;
      }
    }
    // After an aliasing barrier (which is even stronger than an UAV barrier).
    void ClearUAVBarrierPending() { uav_barrier_pending_ = false; }

   private:
    Microsoft::WRL::ComPtr<ID3D12Resource> resource_;
    D3D12_RESOURCE_STATES resource_state_;
    bool uav_barrier_pending_ = false;
  };

  explicit D3D12TextureCache(const RegisterFile& register_file, D3D12SharedMemory& shared_memory,
                             uint32_t draw_resolution_scale_x, uint32_t draw_resolution_scale_y,
                             D3D12CommandProcessor& command_processor,
                             bool bindless_resources_used);

  bool Initialize();

  // Whether decompression is needed on the host (Direct3D only allows creation
  // of block-compressed textures with 4x4-aligned dimensions on PC).
  bool IsDecompressionNeeded(xenos::TextureFormat format, uint32_t width, uint32_t height) const;
  DXGI_FORMAT GetDXGIResourceFormat(xenos::TextureFormat format, uint32_t width,
                                    uint32_t height) const {
    const HostFormat& host_format = host_formats_[uint32_t(format)];
    return IsDecompressionNeeded(format, width, height) ? host_format.dxgi_format_uncompressed
                                                        : host_format.dxgi_format_resource;
  }
  DXGI_FORMAT GetDXGIResourceFormat(TextureKey key) const {
    return GetDXGIResourceFormat(key.format, key.GetWidth(), key.GetHeight());
  }
  DXGI_FORMAT GetDXGIUnormFormat(xenos::TextureFormat format, uint32_t width,
                                 uint32_t height) const {
    const HostFormat& host_format = host_formats_[uint32_t(format)];
    return IsDecompressionNeeded(format, width, height) ? host_format.dxgi_format_uncompressed
                                                        : host_format.dxgi_format_unsigned;
  }
  DXGI_FORMAT GetDXGIUnormFormat(TextureKey key) const {
    return GetDXGIUnormFormat(key.format, key.GetWidth(), key.GetHeight());
  }

  LoadShaderIndex GetLoadShaderIndex(TextureKey key) const;

  static constexpr bool AreDimensionsCompatible(xenos::FetchOpDimension binding_dimension,
                                                xenos::DataDimension resource_dimension) {
    switch (binding_dimension) {
      case xenos::FetchOpDimension::k1D:
      case xenos::FetchOpDimension::k2D:
        return resource_dimension == xenos::DataDimension::k1D ||
               resource_dimension == xenos::DataDimension::k2DOrStacked ||
               resource_dimension == xenos::DataDimension::k3D;
      case xenos::FetchOpDimension::k3DOrStacked:
        return resource_dimension == xenos::DataDimension::k3D;
      case xenos::FetchOpDimension::kCube:
        return resource_dimension == xenos::DataDimension::kCube;
      default:
        return false;
    }
  }

  // Returns the index of an existing of a newly created non-shader-visible
  // cached (for bindful) or a shader-visible global (for bindless) descriptor,
  // or UINT32_MAX if failed to create.
  uint32_t FindOrCreateTextureDescriptor(D3D12Texture& texture, xenos::DataDimension dimension,
                                         bool is_signed, uint32_t host_swizzle);
  void TryCompletePromptTextureReadbackDiagnostic();
  void TryCompleteTextureReadbackDiagnostic();
  void TryCompleteActiveTextureReadbackDiagnostics();

  // HD texture packs, phase 1 (gpu_texture_dump, off by default; see
  // rex/graphics/pipeline/texture/texture_pack.h): after a texture's full load
  // its host resource is copied to a readback buffer; EndFrame writes the
  // completed ones as DDS files through a background thread.
  struct PendingTextureDump {
    uint64_t submission = 0;
    uint64_t id = 0;
    Microsoft::WRL::ComPtr<ID3D12Resource> readback;
    std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints;
    std::vector<UINT> rows;
    std::vector<UINT64> row_bytes;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t mip_levels = 0;
    TextureKey key;
  };
  static constexpr size_t kMaxPendingTextureDumps = 1024;
  // Content id of a texture's current guest data (0 if unreadable).
  uint64_t ComputeTextureContentId(const D3D12Texture& texture) const;
  void QueueTextureDump(D3D12Texture& texture, uint64_t id);
  // Records a copy of the texture's resource into a new readback buffer.
  bool RecordTextureReadback(D3D12Texture& texture, uint64_t id, PendingTextureDump& readback);
  // Language-pack overlays: the loaded texture is read back once, the pack's
  // blocks are laid over it and the result goes to the loader like a file.
  bool QueueOverlayReadback(D3D12Texture& texture, uint64_t id);
  void ProcessOverlayReadbacks();
  std::vector<PendingTextureDump> pending_overlays_;

  // HD texture packs, phase 2 (gpu_texture_replace, off by default): textures
  // whose content id has a pack file get the replacement at the next frame
  // start (the command processor rebuilds every texture view then); the file
  // is read and parsed on a background thread and uploaded within a per-frame
  // budget. A replaced texture whose guest data changes goes back to its own
  // resource and reloads.
  // Created on the loader thread: the texture (copy destination state) and
  // a filled upload buffer.
  struct PreparedPackUpload {
    Microsoft::WRL::ComPtr<ID3D12Resource> texture;
    Microsoft::WRL::ComPtr<ID3D12Resource> upload;
    std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints;
    uint64_t bytes = 0;
  };
  struct PackReplacement {
    enum class State : uint8_t { kIndexed, kLoading, kFailed };
    State state = State::kIndexed;
    std::shared_ptr<PreparedPackUpload> prepared;     // not yet copied
    Microsoft::WRL::ComPtr<ID3D12Resource> resource;  // shader-resource state
  };
  static std::shared_ptr<void> PreparePackUpload(ID3D12Device* device,
                                                 D3D12_HEAP_FLAGS heap_flags,
                                                 const texture_pack::DdsImage& image);
  struct DeferredTextureRelease {
    uint64_t submission = 0;
    Microsoft::WRL::ComPtr<ID3D12Resource> resource;
    std::vector<uint32_t> descriptors;
  };
  void EnsurePackIndex();
  void OfferTextureForReplacement(D3D12Texture& texture, uint64_t id);
  void RevertTextureReplacement(D3D12Texture& texture);
  void UpdatePackReplacements();
  void RecordPackUpload(PackReplacement& replacement);
  uint64_t replacement_last_logged_ = 0;
  // Preload: every pack file is loaded and uploaded from startup (at most
  // kMaxPackLoadsInFlight files at a time), so gameplay only swaps views.
  static constexpr uint32_t kMaxPackLoadsInFlight = 16;
  static constexpr uint32_t kPackSwapIntervalFrames = 8;
  std::vector<uint64_t> pack_preload_order_;
  size_t pack_preload_next_ = 0;
  uint32_t pack_loads_in_flight_ = 0;
  uint32_t pack_frames_since_swap_ = 0;
  bool pack_preload_logged_ = false;
  // Loaded and prepared on the loader thread, copy not recorded yet.
  uint32_t pack_uploads_waiting_ = 0;
  int64_t pack_index_qpc_ = 0;
  // Content-id hashing on the command processor this frame (texture packs).
  uint32_t pack_hash_textures_frame_ = 0;
  uint64_t pack_hash_bytes_frame_ = 0;
  uint64_t pack_hash_ticks_frame_ = 0;
  void OnD3D12TextureDestroyed(D3D12Texture& texture);
  bool pack_index_built_ = false;
  texture_pack::PackIndex pack_index_;
  std::unordered_map<uint64_t, PackReplacement> pack_replacements_;
  std::vector<std::pair<D3D12Texture*, uint64_t>> textures_awaiting_replacement_;
  std::vector<DeferredTextureRelease> deferred_texture_releases_;
  bool replacement_bindings_dirty_ = false;
  uint64_t textures_replaced_ = 0;
  uint64_t textures_reverted_ = 0;
  uint64_t replacement_bytes_uploaded_ = 0;
  // Keyboard button prompts (prompt_icons.h): the title's controller icon
  // textures, recognised by their texels when loaded, show generated
  // keycaps while keyboard prompts are wanted (the views switch at a frame
  // start, like pack replacements; the texture's own resource stays).
  struct PromptIconTexture {
    int32_t icon = -1;
    uint64_t content_id = 0;
    // A keycap is shown (the texture's own resource waits, like a pack's).
    bool keycap = false;
  };
  void EnsurePromptIconSet();
  // Hash of the base level as linear BC blocks in host byte order (0 if the
  // guest data is unreadable).
  uint64_t ComputePromptIconSignature(const D3D12Texture& texture) const;
  void NotePromptIconCandidate(D3D12Texture& texture);
  void UpdatePromptIcons();
  bool prompt_icon_set_loaded_ = false;
  prompt_icons::IconSet prompt_icon_set_;
  std::unordered_map<D3D12Texture*, PromptIconTexture> prompt_icon_textures_;
  // Per icon, in the shader-resource state; built at the first keyboard use
  // and again when the bindings change.
  std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> prompt_keycaps_;
  uint32_t prompt_keycaps_generation_ = 0;
  std::future<std::vector<std::shared_ptr<PreparedPackUpload>>> prompt_keycap_build_;
  uint32_t prompt_keycap_build_generation_ = 0;
  uint32_t prompt_keycap_failed_generation_ = 0;
  bool prompt_keyboard_shown_ = false;
  void ProcessTextureDumps();
  std::vector<PendingTextureDump> pending_texture_dumps_;
  std::unordered_set<uint64_t> dumped_texture_ids_;
  uint64_t texture_dumps_written_ = 0;
  uint64_t texture_dumps_skipped_ = 0;
  void ReleaseTextureDescriptor(uint32_t descriptor_index);
  D3D12_CPU_DESCRIPTOR_HANDLE GetTextureDescriptorCPUHandle(uint32_t descriptor_index) const;

  size_t GetScaledResolveBufferCount() const {
    assert_true(IsDrawResolutionScaled());
    // Make sure any range up to 1 GB is accessible through 1 or 2 buffers.
    // 2x2 scale buffers - just one 2 GB buffer for all 2 GB.
    // 3x3 scale buffers - 4 buffers:
    //  +0.0 +0.5 +1.0 +1.5 +2.0 +2.5 +3.0 +3.5 +4.0 +4.5
    // |___________________|___________________|
    //           |___________________|______________|
    // Buffer N has an offset of N * 1 GB in the scaled resolve address space.
    // The logic is:
    // - 2 GB can be accessed through a [0 GB ... 2 GB) buffer - only need one.
    // - 2.1 GB needs [0 GB ... 2 GB) and [1 GB ... 2.1 GB) - two buffers.
    // - 3 GB needs [0 GB ... 2 GB) and [1 GB ... 3 GB) - two buffers.
    // - 3.1 GB needs [0 GB ... 2 GB), [1 GB ... 3 GB) and [2 GB ... 3.1 GB) -
    //   three buffers.
    uint64_t address_space_size = uint64_t(SharedMemory::kBufferSize) *
                                  (draw_resolution_scale_x() * draw_resolution_scale_y());
    return size_t((address_space_size - 1) >> 30);
  }
  // Returns indices of two scaled resolve virtual buffers that the location in
  // memory may be accessible through. May be the same if it's a location near
  // the beginning or the end of the address represented only by one buffer.
  std::array<size_t, 2> GetPossibleScaledResolveBufferIndices(uint64_t address_scaled) const {
    assert_true(IsDrawResolutionScaled());
    size_t address_gb = size_t(address_scaled >> 30);
    size_t max_index = GetScaledResolveBufferCount() - 1;
    // In different cases for 3x3:
    //  +0.0 +0.5 +1.0 +1.5 +2.0 +2.5 +3.0 +3.5 +4.0 +4.5
    // |12________2________|1_________2________|
    //           |1_________2________|1_________12__|
    return std::array<size_t, 2>{std::min(address_gb, max_index),
                                 std::min(std::max(address_gb, size_t(1)) - size_t(1), max_index)};
  }
  // The index is also the gigabyte offset of the buffer from the start of the
  // scaled physical memory address space.
  size_t GetCurrentScaledResolveBufferIndex() const {
    return scaled_resolve_1gb_buffer_indices_[scaled_resolve_current_range_start_scaled_ >> 30];
  }
  ScaledResolveVirtualBuffer& GetCurrentScaledResolveBuffer() {
    ScaledResolveVirtualBuffer* scaled_resolve_buffer =
        scaled_resolve_2gb_buffers_[GetCurrentScaledResolveBufferIndex()].get();
    assert_not_null(scaled_resolve_buffer);
    return *scaled_resolve_buffer;
  }

  xenos::ClampMode NormalizeClampMode(xenos::ClampMode clamp_mode) const;

  // GetSamplerParameters is a pure function of the six fetch constant words,
  // the binding's filter overrides and the anisotropic override. Keep the last
  // result per fetch constant and return it for an exact repeat.
  struct SamplerParametersMemo {
    uint32_t fetch[6];
    uint32_t binding_filters;  // 0 = empty entry.
    int32_t anisotropic_override;
    SamplerParameters parameters;
  };
  mutable std::array<SamplerParametersMemo, 32> sampler_parameters_memo_{};

  static const HostFormat host_formats_[64];

  D3D12CommandProcessor& command_processor_;
  bool bindless_resources_used_;

  // Texture heap pool (V300, d3d12_texture_heap_pool): textures are placed in
  // pre-created heaps instead of each getting its own committed allocation,
  // which cost a driver allocation plus a residency wait on the command
  // processor thread for every new texture (first-encounter hitches).
  struct TextureHeap {
    explicit TextureHeap(uint64_t capacity) : allocator(capacity) {}
    Microsoft::WRL::ComPtr<ID3D12Heap> heap;
    OffsetAllocator allocator;
  };
  static constexpr uint64_t kTextureHeapSize = UINT64_C(64) << 20;
  static constexpr uint32_t kTextureHeapInitialCount = 4;
  static constexpr uint32_t kTextureHeapMaxCount = 16;
  bool CreateTextureHeap();
  // Returns a placed resource (and its placement) or nullptr to fall back to a
  // committed resource.
  Microsoft::WRL::ComPtr<ID3D12Resource> CreatePlacedTexture(
      const D3D12_RESOURCE_DESC& desc, D3D12_RESOURCE_STATES state, int32_t& heap_index,
      uint64_t& heap_offset, uint64_t& heap_size);
  void FreeTextureHeapRange(int32_t heap_index, uint64_t offset, uint64_t size);
  std::vector<std::unique_ptr<TextureHeap>> texture_heaps_;
  uint64_t texture_heap_placed_ = 0;
  uint64_t texture_heap_fallbacks_ = 0;
  // #16 diagnostic (d3d12_debug_poison_scaled_resolve): 0xFF source for new
  // scaled-resolve heaps.
  Microsoft::WRL::ComPtr<ID3D12Resource> debug_poison_upload_buffer_;
  uint32_t debug_poisoned_heaps_ = 0;
  void PoisonScaledResolveHeap(uint32_t heap_index, size_t buffer_index);

  // Dedicated glow images (RequestGlowImages).
  struct GlowConstants {
    uint32_t native_size[2];
    uint32_t scale[2];
    uint32_t host_size[2];
    uint32_t rect_count;
    // Bit 0: rects[0] clamps every texel (:region=).
    uint32_t flags;
    int32_t rects[4][4];
  };
  struct GlowImage {
    const D3D12Texture* source = nullptr;
    uint64_t source_epoch = 0;
    GlowImageKind kind = GlowImageKind::kNative;
    uint32_t host_swizzle = 0;
    DXGI_FORMAT view_format = DXGI_FORMAT_UNKNOWN;
    GlowConstants constants = {};
    bool made = false;
    // R32G32B32A32_FLOAT box-reduced cells at the guest size.
    Microsoft::WRL::ComPtr<ID3D12Resource> cells;
    D3D12_RESOURCE_STATES cells_state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    // kReconstructed: the texture's view format at the host size.
    Microsoft::WRL::ComPtr<ID3D12Resource> reconstructed;
    D3D12_RESOURCE_STATES reconstructed_state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    // The bound view (bindless index or bindful cache index), host swizzle.
    uint32_t srv_descriptor = UINT32_MAX;
    uint64_t last_used_submission = 0;
    uint64_t last_used_frame = 0;
  };
  static constexpr size_t kMaxGlowImages = 16;
  bool EnsureGlowPipelines();
  bool IsGlowTypedStoreSupported(DXGI_FORMAT format);
  uint32_t AllocateGlowImageDescriptor();
  GlowImage* PrepareGlowImage(D3D12Texture& texture, uint32_t host_swizzle,
                              const GlowImageRequest& request,
                              const native_resolve::Rect* tracked, size_t tracked_count);
  // Releases once the GPU has completed the image's last use (all = also the
  // ones still in use, for ClearCache with the GPU idle and destruction).
  void ReleaseGlowImage(GlowImage& image, bool immediately);
  void ReleaseGlowImages(bool immediately);
  void EvictUnusedGlowImages();
  std::vector<std::unique_ptr<GlowImage>> glow_images_;
  uint32_t glow_image_request_mask_ = 0;
  GlowImageRequest glow_image_requests_[32];
  uint32_t glow_image_bound_mask_ = 0;
  GlowImage* glow_image_bound_[32] = {};
  uint32_t glow_image_bound_descriptors_[32] = {};
  uint64_t glow_image_binding_generation_ = 0;
  uint64_t glow_frame_ = 0;
  uint64_t glow_images_made_ = 0;
  uint32_t glow_images_logged_ = 0;
  Microsoft::WRL::ComPtr<ID3D12RootSignature> glow_root_signature_;
  Microsoft::WRL::ComPtr<ID3D12PipelineState> glow_box_pipeline_;
  Microsoft::WRL::ComPtr<ID3D12PipelineState> glow_reconstruct_pipeline_;
  bool glow_pipelines_attempted_ = false;
  std::unordered_map<uint32_t, bool> glow_typed_store_support_;
  // #16 diagnostics (d3d12_debug_glow_image_dump): read back the source
  // texture, the cells and the image of chosen glow image regenerations and
  // write them as DDS files to the texture dump folder.
  struct PendingGlowDump {
    uint64_t submission = 0;
    std::string name;
    Microsoft::WRL::ComPtr<ID3D12Resource> readback;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
    UINT rows = 0;
    UINT64 row_bytes = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    uint32_t width = 0;
    uint32_t height = 0;
  };
  std::vector<PendingGlowDump> pending_glow_dumps_;
  uint32_t glow_dump_regenerations_ = 0;
  void QueueGlowDump(ID3D12Resource* resource, D3D12_RESOURCE_STATES state,
                     DXGI_FORMAT format, const std::string& name);
  void ProcessGlowDumps();

  Microsoft::WRL::ComPtr<ID3D12RootSignature> load_root_signature_;
  std::array<Microsoft::WRL::ComPtr<ID3D12PipelineState>, kLoadShaderCount> load_pipelines_;
  // Load pipelines for resolution-scaled resolve targets.
  std::array<Microsoft::WRL::ComPtr<ID3D12PipelineState>, kLoadShaderCount> load_pipelines_scaled_;
  Microsoft::WRL::ComPtr<ID3D12PipelineState>
      scaled_resolve_initialize_pipeline_;

  std::vector<SRVDescriptorCachePage> srv_descriptor_cache_;
  uint32_t srv_descriptor_cache_allocated_;
  // Indices of cached descriptors used by deleted textures, for reuse.
  std::vector<uint32_t> srv_descriptor_cache_free_;

  enum class NullSRVDescriptorIndex {
    k2DArray,
    k3D,
    kCube,

    kCount,
  };
  // Contains null SRV descriptors of dimensions from NullSRVDescriptorIndex.
  // For copying, not shader-visible.
  Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> null_srv_descriptor_heap_;
  D3D12_CPU_DESCRIPTOR_HANDLE null_srv_descriptor_heap_start_;

  std::array<D3D12TextureBinding, xenos::kTextureFetchConstantCount> d3d12_texture_bindings_;

  // One-shot verification of the dynamically identified PRESS START texture.
  // The first half receives the loader scratch buffer, and the second receives
  // the resulting BC1 texture resource. Completion is checked on a later GPU
  // submission, so the normal draw path is never synchronously stalled.
  Microsoft::WRL::ComPtr<ID3D12Resource> prompt_texture_readback_;
  uint64_t prompt_texture_readback_submission_ = 0;
  uint32_t prompt_texture_readback_guest_base_ = 0;
  uint32_t prompt_texture_readback_copy_size_ = 0;
  uint32_t prompt_texture_readback_resource_offset_ = 0;
  uint32_t prompt_texture_cpu_expected_hash_ = 0;

  // One-shot, explicitly address-gated comparison of the loader scratch
  // output and the actual shader-visible host texture. Disabled by default;
  // completion is asynchronous on a later GPU submission.
  Microsoft::WRL::ComPtr<ID3D12Resource> texture_readback_;
  uint64_t texture_readback_submission_ = 0;
  uint32_t texture_readback_guest_base_ = 0;
  uint32_t texture_readback_guest_size_ = 0;
  uint32_t texture_readback_payload_size_ = 0;
  uint32_t texture_readback_resource_offset_ = 0;
  uint32_t texture_readback_row_pitch_ = 0;
  uint32_t texture_readback_row_bytes_ = 0;
  uint32_t texture_readback_row_count_ = 0;
  uint32_t texture_readback_width_ = 0;
  uint32_t texture_readback_height_ = 0;
  uint32_t texture_readback_depth_ = 0;
  uint32_t texture_readback_format_ = 0;
  uint32_t texture_readback_scale_x_ = 1;
  uint32_t texture_readback_scale_y_ = 1;
  bool texture_readback_scaled_resolve_ = false;
  bool texture_readback_diagnostic_started_ = false;
  uint64_t texture_readback_armed_address_min_ = 0;
  uint64_t texture_readback_armed_address_max_ = 0;

  struct ActiveTextureReadbackDiagnostic {
    Microsoft::WRL::ComPtr<ID3D12Resource> readback;
    Microsoft::WRL::ComPtr<ID3D12Resource> depth_snapshot;
    uint64_t snapshot_frame = 0;
    uint64_t submission = 0;
    uint64_t resource_identity = 0;
    uint64_t context_draw_ordinal = 0;
    uint32_t fetch_constant_index = 0;
    uint32_t array_slice = 0;
    uint32_t array_size = 1;
    uint32_t guest_base = 0;
    uint32_t guest_size = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t depth = 0;
    uint32_t format = 0;
    uint32_t row_pitch = 0;
    uint32_t row_bytes = 0;
    uint32_t row_count = 0;
    uint32_t scale_x = 1;
    uint32_t scale_y = 1;
    bool scaled_resolve = false;
  };
  std::vector<ActiveTextureReadbackDiagnostic>
      active_texture_readback_diagnostics_;
  embedded_texture_readback_policy::State
      active_texture_readback_diagnostic_policy_;
  bool active_texture_readback_limit_reported_ = false;
  bool temporal_first_binding_attempted_ = false;

  void FinishOwnedSceneColor();
  void CompleteOwnedSceneColorVerification();
  pc_scene_color_history::State owned_scene_color_state_;
  pc_draw_transform_history::Frame pending_owned_draws_;
  std::array<pc_draw_transform_history::Frame,
             pc_scene_color_history::State::kSlots> owned_draws_;
  std::array<Microsoft::WRL::ComPtr<ID3D12Resource>,
             pc_scene_color_history::State::kSlots> owned_scene_color_;
  uint32_t owned_scene_color_swizzle_ = 0;
  uint8_t owned_scene_color_signs_ = 0;
  int32_t owned_scene_color_exponent_ = 0;
  uint32_t owned_scene_color_reported_ = 0;
  uint32_t owned_camera_frames_reported_ = 0;
  pc_draw_transform_history::FollowupSamples owned_camera_followups_;
  struct OwnedColorVerification {
    std::array<Microsoft::WRL::ComPtr<ID3D12Resource>, 2> readback;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    uint64_t frame = 0, submission = 0, bytes = 0;
    uint32_t row_bytes = 0, rows = 0;
    bool finished = false;
  };
  std::array<OwnedColorVerification, 2> owned_color_verifications_;
  uint32_t owned_color_verification_count_ = 0;

  // Unsupported texture formats used during this frame (for research and
  // testing).
  enum : uint8_t {
    kUnsupportedResourceBit = 1,
    kUnsupportedUnormBit = kUnsupportedResourceBit << 1,
    kUnsupportedSnormBit = kUnsupportedUnormBit << 1,
  };
  uint8_t unsupported_format_features_used_[64];

  // The tiled buffer for resolved data with resolution scaling.
  // Because on Direct3D 12 (at least on Windows 10 2004) typed SRV or UAV
  // creation fails for offsets above 4 GB, a single tiled 4.5 GB buffer can't
  // be used for 3x3 resolution scaling.
  // Instead, "sliding window" buffers allowing to access a single range of up
  // to 1 GB (or up to 2 GB, depending on the low bits) at any moment are used.
  // Parts of 4.5 GB address space can be accessed through 2 GB buffers as:
  //  +0.0 +0.5 +1.0 +1.5 +2.0 +2.5 +3.0 +3.5 +4.0 +4.5
  // |___________________|___________________|      or
  //           |___________________|______________|
  // (2 GB is also the amount of scaled physical memory with 2x resolution
  // scale, and older Intel GPUs, while support tiled resources, only support 31
  // virtual address bits per resource).
  // Index is first gigabyte. Only including buffers containing over 1 GB
  // (because otherwise the data will be fully contained in another).
  // Size is calculated the same as in GetScaledResolveBufferCount.
  std::array<std::unique_ptr<ScaledResolveVirtualBuffer>,
             (uint64_t(SharedMemory::kBufferSize) *
                  (kMaxDrawResolutionScaleAlongAxis * kMaxDrawResolutionScaleAlongAxis) -
              1) /
                 (UINT32_C(1) << 30)>
      scaled_resolve_2gb_buffers_;
  // Not very big heaps (16 MB) because they are needed pretty sparsely. One
  // 2x-scaled 1280x720x32bpp texture is slighly bigger than 14 MB.
  static constexpr uint32_t kScaledResolveHeapSizeLog2 = 24;
  static constexpr uint32_t kScaledResolveHeapSize = uint32_t(1) << kScaledResolveHeapSizeLog2;
  static_assert((kScaledResolveHeapSize % D3D12_TILED_RESOURCE_TILE_SIZE_IN_BYTES) == 0,
                "Scaled resolve heap size must be a multiple of Direct3D tile size");
  static_assert(kScaledResolveHeapSizeLog2 <= SharedMemory::kBufferSizeLog2,
                "Scaled resolve heaps are assumed to be wholly mappable irrespective of "
                "resolution scale, never truncated, for example, if the scaled resolve "
                "address space is 4.5 GB, but the heap size is 1 GB");
  static_assert(kScaledResolveHeapSizeLog2 <= 30,
                "Scaled resolve heaps are assumed to only be wholly mappable to up to "
                "two 2 GB buffers");
  // Resident portions of the tiled buffer.
  std::vector<Microsoft::WRL::ComPtr<ID3D12Heap>> scaled_resolve_heaps_;
  // Number of currently resident portions of the tiled buffer, for profiling.
  uint32_t scaled_resolve_heap_count_ = 0;
  // Current scaled resolve state.
  // For aliasing barrier placement, last owning buffer index for each of 1 GB.
  size_t scaled_resolve_1gb_buffer_indices_[(uint64_t(SharedMemory::kBufferSize) *
                                                 kMaxDrawResolutionScaleAlongAxis *
                                                 kMaxDrawResolutionScaleAlongAxis +
                                             ((uint32_t(1) << 30) - 1)) >>
                                            30];
  // Range used in the last successful MakeScaledResolveRangeCurrent call.
  uint64_t scaled_resolve_current_range_start_scaled_;
  uint64_t scaled_resolve_current_range_length_scaled_;
};

}  // namespace rex::graphics::d3d12
