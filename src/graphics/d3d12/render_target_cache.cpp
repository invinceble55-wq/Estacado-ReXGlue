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

#include "thirdparty/dxbc/DXBCChecksum.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <memory>
#include <string>
#include <tuple>
#include <utility>

#include <rex/assert.h>
#include <rex/cvar.h>
#include <rex/graphics/d3d12/command_processor.h>
#include <rex/graphics/d3d12/deferred_command_list.h>
#include <rex/graphics/d3d12/render_target_cache.h>
#include <rex/graphics/d3d12/texture_cache.h>
#include <rex/graphics/flags.h>
#include <rex/graphics/format/dxbc.h>
#include <rex/graphics/pipeline/shader/dxbc_translator.h>
#include <rex/graphics/trace_writer.h>
#include <rex/graphics/util/draw.h>
#include <rex/graphics/xenos.h>
#include <rex/logging.h>
#include <rex/math.h>
#include <rex/string.h>
#include <rex/ui/d3d12/d3d12_provider.h>
#include <rex/ui/d3d12/d3d12_util.h>
#include "../shaders/bytecode/d3d12_5_1/diagnostic_depth_samples_single.h"
#include "../shaders/bytecode/d3d12_5_1/diagnostic_depth_samples_msaa.h"
#include "../shaders/bytecode/d3d12_5_1/diagnostic_color_samples_single.h"
#include "../shaders/bytecode/d3d12_5_1/diagnostic_color_samples_msaa.h"

REXCVAR_DEFINE_BOOL(embedded_camera_scene_alias_capture, false, "GPU/Diagnostics",
                    "Selected scene frame: bounded individual colour samples before/after ownership aliases");

REXCVAR_DEFINE_BOOL(embedded_resolve_depth_source_capture, false, "GPU",
                    "Bounded deferred raw host depth/stencil samples before resolve");

REXCVAR_DEFINE_BOOL(native_stencil_value_output_d3d12_intel, false, "GPU/D3D12",
                    "Native stencil value output for Intel D3D12");

REXCVAR_DEFINE_STRING(render_target_path_d3d12, "", "GPU/D3D12",
                      "D3D12 render target implementation path")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

// Black 4x8-pixel cells on some GPU generations (#16) match single 256-byte
// blocks of a 64bpp render target, the granularity of colour compression.
// Simultaneous access keeps a resource from being compressed at all.
REXCVAR_DEFINE_BOOL(d3d12_render_target_uncompressed, false, "GPU/D3D12",
                    "Diagnostics: create single-sample colour render targets with simultaneous "
                    "access, which keeps the GPU from compressing them (finds compression "
                    "problems; slower)")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
// Without a pixel shader stencil output (NVIDIA), a depth transfer clears the
// stencil of the transferred rectangles and rebuilds it bit by bit. The game
// runs one every frame (above 1x from the scaled 4x MSAA depth into a native
// buffer), and a stencil-only rectangle clear of a compressed 64bpp buffer is
// an unusual driver path; 4x8-pixel tiles with a stale stencil would hide the
// lamps' light or add shadows there (#16 dots, #20 bands).
REXCVAR_DEFINE_BOOL(d3d12_transfer_stencil_clear_by_draw, false, "GPU/D3D12",
                    "Diagnostics: depth render target transfers clear the stencil of the "
                    "transferred rectangles by drawing instead of with ClearDepthStencilView")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_BOOL(native_stencil_value_output, true, "GPU", "Enable native stencil value output");

REXCVAR_DEFINE_BOOL(
    embedded_scene_depth_transfer_trace, false, "GPU/Diagnostics",
    "Bounded ownership-transfer trace for the scaled title scene depth/stencil surface");

// Defined by command_processor.cpp. The render-target cache consumes the same
// opt-in diagnostic switch so the draw-side and ownership-transfer captures are
// guaranteed to describe one selected frame.
REXCVAR_DECLARE(bool, embedded_mixed_scale_transition_capture);
REXCVAR_DECLARE(uint32_t, embedded_target_writer_capture_count);
REXCVAR_DECLARE(bool, embedded_temporal_depth_resolve_capture);

namespace rex::graphics::d3d12 {

// Defined by command_processor.cpp. This is true only for the bounded swap
// selected by embedded_gameplay_capture_* and for a physical manual capture.
bool IsCurrentEmbeddedGameplayCaptureFrame();
bool D3D12RenderTargetCache::current_draw_depth_float24_convert_in_pixel_shader() const {
  return depth_float24_convert_in_pixel_shader_;
}

// Generated with `xb buildshaders`.
namespace shaders {
#include "../shaders/bytecode/d3d12_5_1/clear_uint2_ps.h"
#include "../shaders/bytecode/d3d12_5_1/fullscreen_cw_vs.h"
#include "../shaders/bytecode/d3d12_5_1/host_depth_store_1xmsaa_cs.h"
#include "../shaders/bytecode/d3d12_5_1/host_depth_store_2xmsaa_cs.h"
#include "../shaders/bytecode/d3d12_5_1/host_depth_store_4xmsaa_cs.h"
#include "../shaders/bytecode/d3d12_5_1/passthrough_position_xy_vs.h"
#include "../shaders/bytecode/d3d12_5_1/resolve_clear_32bpp_cs.h"
#include "../shaders/bytecode/d3d12_5_1/resolve_clear_32bpp_scaled_cs.h"
#include "../shaders/bytecode/d3d12_5_1/resolve_clear_64bpp_cs.h"
#include "../shaders/bytecode/d3d12_5_1/resolve_clear_64bpp_scaled_cs.h"
#include "../shaders/bytecode/d3d12_5_1/resolve_fast_32bpp_1x2xmsaa_cs.h"
#include "../shaders/bytecode/d3d12_5_1/resolve_fast_32bpp_1x2xmsaa_scaled_cs.h"
#include "../shaders/bytecode/d3d12_5_1/resolve_fast_32bpp_4xmsaa_cs.h"
#include "../shaders/bytecode/d3d12_5_1/resolve_fast_32bpp_4xmsaa_scaled_cs.h"
#include "../shaders/bytecode/d3d12_5_1/resolve_fast_64bpp_1x2xmsaa_cs.h"
#include "../shaders/bytecode/d3d12_5_1/resolve_fast_64bpp_1x2xmsaa_scaled_cs.h"
#include "../shaders/bytecode/d3d12_5_1/resolve_fast_64bpp_4xmsaa_cs.h"
#include "../shaders/bytecode/d3d12_5_1/resolve_fast_64bpp_4xmsaa_scaled_cs.h"
#include "../shaders/bytecode/d3d12_5_1/resolve_full_128bpp_cs.h"
#include "../shaders/bytecode/d3d12_5_1/resolve_full_128bpp_scaled_cs.h"
#include "../shaders/bytecode/d3d12_5_1/resolve_full_16bpp_cs.h"
#include "../shaders/bytecode/d3d12_5_1/resolve_full_16bpp_scaled_cs.h"
#include "../shaders/bytecode/d3d12_5_1/resolve_full_32bpp_cs.h"
#include "../shaders/bytecode/d3d12_5_1/resolve_full_32bpp_scaled_cs.h"
#include "../shaders/bytecode/d3d12_5_1/resolve_full_64bpp_cs.h"
#include "../shaders/bytecode/d3d12_5_1/resolve_full_64bpp_scaled_cs.h"
#include "../shaders/bytecode/d3d12_5_1/resolve_full_8bpp_cs.h"
#include "../shaders/bytecode/d3d12_5_1/resolve_full_8bpp_scaled_cs.h"
}  // namespace shaders

const D3D12RenderTargetCache::ResolveCopyShaderCode
    D3D12RenderTargetCache::kResolveCopyShaders[size_t(
        draw_util::ResolveCopyShaderIndex::kCount)] = {
        {shaders::resolve_fast_32bpp_1x2xmsaa_cs, sizeof(shaders::resolve_fast_32bpp_1x2xmsaa_cs),
         shaders::resolve_fast_32bpp_1x2xmsaa_scaled_cs,
         sizeof(shaders::resolve_fast_32bpp_1x2xmsaa_scaled_cs)},
        {shaders::resolve_fast_32bpp_4xmsaa_cs, sizeof(shaders::resolve_fast_32bpp_4xmsaa_cs),
         shaders::resolve_fast_32bpp_4xmsaa_scaled_cs,
         sizeof(shaders::resolve_fast_32bpp_4xmsaa_scaled_cs)},
        {shaders::resolve_fast_64bpp_1x2xmsaa_cs, sizeof(shaders::resolve_fast_64bpp_1x2xmsaa_cs),
         shaders::resolve_fast_64bpp_1x2xmsaa_scaled_cs,
         sizeof(shaders::resolve_fast_64bpp_1x2xmsaa_scaled_cs)},
        {shaders::resolve_fast_64bpp_4xmsaa_cs, sizeof(shaders::resolve_fast_64bpp_4xmsaa_cs),
         shaders::resolve_fast_64bpp_4xmsaa_scaled_cs,
         sizeof(shaders::resolve_fast_64bpp_4xmsaa_scaled_cs)},
        {shaders::resolve_full_8bpp_cs, sizeof(shaders::resolve_full_8bpp_cs),
         shaders::resolve_full_8bpp_scaled_cs, sizeof(shaders::resolve_full_8bpp_scaled_cs)},
        {shaders::resolve_full_16bpp_cs, sizeof(shaders::resolve_full_16bpp_cs),
         shaders::resolve_full_16bpp_scaled_cs, sizeof(shaders::resolve_full_16bpp_scaled_cs)},
        {shaders::resolve_full_32bpp_cs, sizeof(shaders::resolve_full_32bpp_cs),
         shaders::resolve_full_32bpp_scaled_cs, sizeof(shaders::resolve_full_32bpp_scaled_cs)},
        {shaders::resolve_full_64bpp_cs, sizeof(shaders::resolve_full_64bpp_cs),
         shaders::resolve_full_64bpp_scaled_cs, sizeof(shaders::resolve_full_64bpp_scaled_cs)},
        {shaders::resolve_full_128bpp_cs, sizeof(shaders::resolve_full_128bpp_cs),
         shaders::resolve_full_128bpp_scaled_cs, sizeof(shaders::resolve_full_128bpp_scaled_cs)},
};

const uint32_t D3D12RenderTargetCache::kTransferUsedRootParameters[size_t(
    TransferRootSignatureIndex::kCount)] = {
    // kColor
    kTransferUsedRootParameterColorSRVBit | kTransferUsedRootParameterAddressConstantBit,
    // kDepth
    kTransferUsedRootParameterDepthSRVBit | kTransferUsedRootParameterAddressConstantBit,
    // kDepthStencil
    kTransferUsedRootParameterDepthSRVBit | kTransferUsedRootParameterStencilSRVBit |
        kTransferUsedRootParameterAddressConstantBit,
    // kColorToStencilBit
    kTransferUsedRootParameterStencilMaskConstantBit | kTransferUsedRootParameterColorSRVBit |
        kTransferUsedRootParameterAddressConstantBit,
    // kStencilToStencilBit
    kTransferUsedRootParameterStencilMaskConstantBit | kTransferUsedRootParameterStencilSRVBit |
        kTransferUsedRootParameterAddressConstantBit,
    // kColorAndHostDepth
    kTransferUsedRootParameterColorSRVBit | kTransferUsedRootParameterAddressConstantBit |
        kTransferUsedRootParameterHostDepthSRVBit |
        kTransferUsedRootParameterHostDepthAddressConstantBit,
    // kDepthAndHostDepth
    kTransferUsedRootParameterDepthSRVBit | kTransferUsedRootParameterAddressConstantBit |
        kTransferUsedRootParameterHostDepthSRVBit |
        kTransferUsedRootParameterHostDepthAddressConstantBit,
    // kDepthStencilAndHostDepth
    kTransferUsedRootParameterDepthSRVBit | kTransferUsedRootParameterStencilSRVBit |
        kTransferUsedRootParameterAddressConstantBit | kTransferUsedRootParameterHostDepthSRVBit |
        kTransferUsedRootParameterHostDepthAddressConstantBit,
};

const D3D12RenderTargetCache::TransferModeInfo
    D3D12RenderTargetCache::kTransferModes[size_t(TransferMode::kCount)] = {
        // kColorToDepth
        {TransferOutput::kDepth, TransferRootSignatureIndex::kColor,
         TransferRootSignatureIndex::kColor},
        // kColorToColor
        {TransferOutput::kColor, TransferRootSignatureIndex::kColor,
         TransferRootSignatureIndex::kColor},
        // kDepthToDepth
        {TransferOutput::kDepth, TransferRootSignatureIndex::kDepth,
         TransferRootSignatureIndex::kDepthStencil},
        // kDepthToColor
        {TransferOutput::kColor, TransferRootSignatureIndex::kDepthStencil,
         TransferRootSignatureIndex::kDepthStencil},
        // kColorToStencilBit
        {TransferOutput::kStencilBit, TransferRootSignatureIndex::kColorToStencilBit,
         TransferRootSignatureIndex::kColorToStencilBit},
        // kDepthToStencilBit
        {TransferOutput::kStencilBit, TransferRootSignatureIndex::kStencilToStencilBit,
         TransferRootSignatureIndex::kStencilToStencilBit},
        // kColorAndHostDepthToDepth
        {TransferOutput::kDepth, TransferRootSignatureIndex::kColorAndHostDepth,
         TransferRootSignatureIndex::kColorAndHostDepth},
        // kDepthAndHostDepthToDepth
        {TransferOutput::kDepth, TransferRootSignatureIndex::kDepthAndHostDepth,
         TransferRootSignatureIndex::kDepthStencilAndHostDepth},
};

D3D12RenderTargetCache::~D3D12RenderTargetCache() {
  Shutdown(true);
}

bool D3D12RenderTargetCache::Initialize() {
  const ui::d3d12::D3D12Provider& provider = command_processor_.GetD3D12Provider();
  ID3D12Device* device = provider.GetDevice();

  if (REXCVAR_GET(render_target_path_d3d12) == "rtv") {
    path_ = Path::kHostRenderTargets;
  } else if (REXCVAR_GET(render_target_path_d3d12) == "rov") {
    path_ = Path::kPixelShaderInterlock;
  } else {
    // As of April 2021 (driver version 27.20.0100.9316), on Intel (tested on
    // UHD Graphics 630), the "always" stencil comparison function isn't working
    // properly, so clears in the Xbox 360's Direct3D 9 don't work. Forcing ROV
    // there.
#if 1
    // The ROV path is currently much slower generally.
    // TODO(Triang3l): Make ROV the default when it's optimized better (for
    // instance, using static shader modifications to pass render target
    // parameters).
    path_ = provider.GetAdapterVendorID() == ui::GraphicsProvider::GpuVendorID::kIntel
                ? Path::kPixelShaderInterlock
                : Path::kHostRenderTargets;
#else
    // The AMD shader compiler crashes very often with Xenia's custom
    // output-merger code as of March 2021.
    path_ = provider.GetAdapterVendorID() == ui::GraphicsProvider::GpuVendorID::kAMD
                ? Path::kHostRenderTargets
                : Path::kPixelShaderInterlock;
#endif
  }
  if (path_ == Path::kPixelShaderInterlock && !provider.AreRasterizerOrderedViewsSupported()) {
    path_ = Path::kHostRenderTargets;
  }
  std::fprintf(stderr,
               "REX_EMBEDDED_RENDER_TARGET_PATH requested=%s selected=%s rov_supported=%u "
               "vendor=0x%04X\n",
               REXCVAR_GET(render_target_path_d3d12).c_str(),
               path_ == Path::kPixelShaderInterlock ? "rov" : "rtv",
               provider.AreRasterizerOrderedViewsSupported() ? 1u : 0u,
               static_cast<unsigned>(provider.GetAdapterVendorID()));

  // Create the buffer for reinterpreting EDRAM contents.
  uint32_t edram_buffer_size =
      xenos::kEdramSizeBytes * (draw_resolution_scale_x() * draw_resolution_scale_y());
  D3D12_RESOURCE_DESC edram_buffer_desc;
  ui::d3d12::util::FillBufferResourceDesc(edram_buffer_desc, edram_buffer_size,
                                          D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
  // The first operation will likely be depth self-comparison with host render
  // targets or drawing with ROV.
  edram_buffer_state_ = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
  // Creating zeroed for stable initial value with ROV (though on a real
  // console it has to be cleared anyway probably) and not to leak irrelevant
  // data to trace dumps when not covered by host render targets entirely.
  if (FAILED(device->CreateCommittedResource(
          &ui::d3d12::util::kHeapPropertiesDefault, D3D12_HEAP_FLAG_NONE, &edram_buffer_desc,
          edram_buffer_state_, nullptr, IID_PPV_ARGS(&edram_buffer_)))) {
    REXGPU_ERROR("D3D12RenderTargetCache: Failed to create the EDRAM buffer");
    Shutdown();
    return false;
  }
  edram_buffer_->SetName(L"EDRAM Buffer");
  edram_buffer_modification_status_ = EdramBufferModificationStatus::kUnmodified;

  // Create non-shader-visible descriptors of the EDRAM buffer for copying.
  D3D12_DESCRIPTOR_HEAP_DESC edram_buffer_descriptor_heap_desc;
  edram_buffer_descriptor_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
  edram_buffer_descriptor_heap_desc.NumDescriptors = uint32_t(EdramBufferDescriptorIndex::kCount);
  edram_buffer_descriptor_heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
  edram_buffer_descriptor_heap_desc.NodeMask = 0;
  if (FAILED(device->CreateDescriptorHeap(&edram_buffer_descriptor_heap_desc,
                                          IID_PPV_ARGS(&edram_buffer_descriptor_heap_)))) {
    REXGPU_ERROR(
        "D3D12RenderTargetCache: Failed to create the descriptor heap for "
        "EDRAM buffer views");
    Shutdown();
    return false;
  }
  edram_buffer_descriptor_heap_start_ =
      edram_buffer_descriptor_heap_->GetCPUDescriptorHandleForHeapStart();
  ui::d3d12::util::CreateBufferRawSRV(
      device,
      provider.OffsetViewDescriptor(edram_buffer_descriptor_heap_start_,
                                    uint32_t(EdramBufferDescriptorIndex::kRawSRV)),
      edram_buffer_, edram_buffer_size);
  ui::d3d12::util::CreateBufferTypedSRV(
      device,
      provider.OffsetViewDescriptor(edram_buffer_descriptor_heap_start_,
                                    uint32_t(EdramBufferDescriptorIndex::kR32UintSRV)),
      edram_buffer_, DXGI_FORMAT_R32_UINT, edram_buffer_size >> 2);
  ui::d3d12::util::CreateBufferTypedSRV(
      device,
      provider.OffsetViewDescriptor(edram_buffer_descriptor_heap_start_,
                                    uint32_t(EdramBufferDescriptorIndex::kR32G32UintSRV)),
      edram_buffer_, DXGI_FORMAT_R32G32_UINT, edram_buffer_size >> 3);
  ui::d3d12::util::CreateBufferTypedSRV(
      device,
      provider.OffsetViewDescriptor(edram_buffer_descriptor_heap_start_,
                                    uint32_t(EdramBufferDescriptorIndex::kR32G32B32A32UintSRV)),
      edram_buffer_, DXGI_FORMAT_R32G32B32A32_UINT, edram_buffer_size >> 4);
  ui::d3d12::util::CreateBufferRawUAV(
      device,
      provider.OffsetViewDescriptor(edram_buffer_descriptor_heap_start_,
                                    uint32_t(EdramBufferDescriptorIndex::kRawUAV)),
      edram_buffer_, edram_buffer_size);
  ui::d3d12::util::CreateBufferTypedUAV(
      device,
      provider.OffsetViewDescriptor(edram_buffer_descriptor_heap_start_,
                                    uint32_t(EdramBufferDescriptorIndex::kR32UintUAV)),
      edram_buffer_, DXGI_FORMAT_R32_UINT, edram_buffer_size >> 2);
  ui::d3d12::util::CreateBufferTypedUAV(
      device,
      provider.OffsetViewDescriptor(edram_buffer_descriptor_heap_start_,
                                    uint32_t(EdramBufferDescriptorIndex::kR32G32UintUAV)),
      edram_buffer_, DXGI_FORMAT_R32G32_UINT, edram_buffer_size >> 3);
  ui::d3d12::util::CreateBufferTypedUAV(
      device,
      provider.OffsetViewDescriptor(edram_buffer_descriptor_heap_start_,
                                    uint32_t(EdramBufferDescriptorIndex::kR32G32B32A32UintUAV)),
      edram_buffer_, DXGI_FORMAT_R32G32B32A32_UINT, edram_buffer_size >> 4);

  bool draw_resolution_scaled = IsDrawResolutionScaled();

  // Create the resolve copying root signature.
  std::array<D3D12_ROOT_PARAMETER, 3> resolve_copy_root_parameters;
  // Parameter 0 is constants.
  resolve_copy_root_parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  resolve_copy_root_parameters[0].Constants.ShaderRegister = 0;
  resolve_copy_root_parameters[0].Constants.RegisterSpace = 0;
  // Binding all of the shared memory at 1x resolution, portions with scaled
  // resolution.
  resolve_copy_root_parameters[0].Constants.Num32BitValues =
      (draw_resolution_scaled ? sizeof(draw_util::ResolveCopyShaderConstants::DestRelative)
                              : sizeof(draw_util::ResolveCopyShaderConstants)) /
      sizeof(uint32_t);
  resolve_copy_root_parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  // Parameter 1 is the destination (shared memory).
  D3D12_DESCRIPTOR_RANGE resolve_copy_dest_range;
  resolve_copy_dest_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
  resolve_copy_dest_range.NumDescriptors = 1;
  resolve_copy_dest_range.BaseShaderRegister = 0;
  resolve_copy_dest_range.RegisterSpace = 0;
  resolve_copy_dest_range.OffsetInDescriptorsFromTableStart = 0;
  resolve_copy_root_parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  resolve_copy_root_parameters[1].DescriptorTable.NumDescriptorRanges = 1;
  resolve_copy_root_parameters[1].DescriptorTable.pDescriptorRanges = &resolve_copy_dest_range;
  resolve_copy_root_parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  // Parameter 2 is the source (EDRAM).
  D3D12_DESCRIPTOR_RANGE resolve_copy_source_range;
  resolve_copy_source_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
  resolve_copy_source_range.NumDescriptors = 1;
  resolve_copy_source_range.BaseShaderRegister = 0;
  resolve_copy_source_range.RegisterSpace = 0;
  resolve_copy_source_range.OffsetInDescriptorsFromTableStart = 0;
  resolve_copy_root_parameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  resolve_copy_root_parameters[2].DescriptorTable.NumDescriptorRanges = 1;
  resolve_copy_root_parameters[2].DescriptorTable.pDescriptorRanges = &resolve_copy_source_range;
  resolve_copy_root_parameters[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  D3D12_ROOT_SIGNATURE_DESC resolve_copy_root_signature_desc;
  resolve_copy_root_signature_desc.NumParameters = UINT(resolve_copy_root_parameters.size());
  resolve_copy_root_signature_desc.pParameters = resolve_copy_root_parameters.data();
  resolve_copy_root_signature_desc.NumStaticSamplers = 0;
  resolve_copy_root_signature_desc.pStaticSamplers = nullptr;
  resolve_copy_root_signature_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;
  resolve_copy_root_signature_ =
      ui::d3d12::util::CreateRootSignature(provider, resolve_copy_root_signature_desc);
  if (resolve_copy_root_signature_ == nullptr) {
    REXGPU_ERROR(
        "D3D12RenderTargetCache: Failed to create the resolve copy root "
        "signature");
    Shutdown();
    return false;
  }
  if (draw_resolution_scaled) {
    // Native threshold resolves use full unscaled constants and authoritative
    // shared memory, unlike the globally scaled resolve-buffer path.
    resolve_copy_root_parameters[0].Constants.Num32BitValues =
        sizeof(draw_util::ResolveCopyShaderConstants) / sizeof(uint32_t);
    resolve_copy_native_root_signature_ =
        ui::d3d12::util::CreateRootSignature(
            provider, resolve_copy_root_signature_desc);
    if (resolve_copy_native_root_signature_ == nullptr) {
      REXGPU_ERROR(
          "D3D12RenderTargetCache: Failed to create the native resolve-copy "
          "root signature");
      Shutdown();
      return false;
    }
  }
  // Direct resolve currently shares the root signature shape with the resolve
  // copy pass (constants + destination UAV + source SRV) and may diverge later.
  direct_resolve_root_signature_color_ = resolve_copy_root_signature_;
  direct_resolve_root_signature_depth_ = resolve_copy_root_signature_;
  direct_resolve_root_signature_color_->AddRef();
  direct_resolve_root_signature_depth_->AddRef();

  // Create the resolve copying pipelines.
  for (size_t i = 0; i < size_t(draw_util::ResolveCopyShaderIndex::kCount); ++i) {
    const draw_util::ResolveCopyShaderInfo& resolve_copy_shader_info =
        draw_util::resolve_copy_shader_info[i];
    const ResolveCopyShaderCode& resolve_copy_shader_code = kResolveCopyShaders[i];
    // Somewhat verification whether resolve_copy_shaders_ is up to date.
    assert_true(resolve_copy_shader_code.unscaled && resolve_copy_shader_code.unscaled_size &&
                resolve_copy_shader_code.scaled && resolve_copy_shader_code.scaled_size);
    ID3D12PipelineState* resolve_copy_pipeline = ui::d3d12::util::CreateComputePipeline(
        device,
        draw_resolution_scaled ? resolve_copy_shader_code.scaled
                               : resolve_copy_shader_code.unscaled,
        draw_resolution_scaled ? resolve_copy_shader_code.scaled_size
                               : resolve_copy_shader_code.unscaled_size,
        resolve_copy_root_signature_);
    if (resolve_copy_pipeline == nullptr) {
      REXGPU_ERROR("D3D12RenderTargetCache: Failed to create {} resolve copy pipeline",
                   resolve_copy_shader_info.debug_name);
      Shutdown();
      return false;
    }
    std::u16string resolve_copy_pipeline_name =
        rex::string::to_utf16(resolve_copy_shader_info.debug_name);
    resolve_copy_pipeline->SetName(reinterpret_cast<LPCWSTR>(resolve_copy_pipeline_name.c_str()));
    resolve_copy_pipelines_[i] = resolve_copy_pipeline;
    if (draw_resolution_scaled) {
      ID3D12PipelineState* resolve_copy_native_pipeline =
          ui::d3d12::util::CreateComputePipeline(
              device, resolve_copy_shader_code.unscaled,
              resolve_copy_shader_code.unscaled_size,
              resolve_copy_native_root_signature_);
      if (resolve_copy_native_pipeline == nullptr) {
        REXGPU_ERROR(
            "D3D12RenderTargetCache: Failed to create {} native "
            "resolve-copy pipeline",
            resolve_copy_shader_info.debug_name);
        Shutdown();
        return false;
      }
      resolve_copy_native_pipeline->SetName(
          reinterpret_cast<LPCWSTR>(resolve_copy_pipeline_name.c_str()));
      resolve_copy_native_pipelines_[i] = resolve_copy_native_pipeline;
    }
  }

  // Using the cvar on emulator initialization so used pipelines are consistent
  // across different titles launched in one emulator instance.
  use_stencil_reference_output_ =
      REXCVAR_GET(native_stencil_value_output) &&
      provider.IsPSSpecifiedStencilReferenceSupported() &&
      (REXCVAR_GET(native_stencil_value_output_d3d12_intel) ||
       provider.GetAdapterVendorID() != ui::GraphicsProvider::GpuVendorID::kIntel);
  std::fprintf(stderr,
               "REX_RENDER_TARGET_TRANSFER_POLICY stencil_reference_output=%u "
               "ps_specified_stencil_ref=%u\n",
               use_stencil_reference_output_ ? 1u : 0u,
               provider.IsPSSpecifiedStencilReferenceSupported() ? 1u : 0u);
  std::fflush(stderr);

  if (path_ == Path::kHostRenderTargets) {
    // Host render targets.

    gamma_render_target_as_unorm16_ = REXCVAR_GET(gamma_render_target_as_unorm16);

    depth_float24_round_ = REXCVAR_GET(depth_float24_round);
    depth_float24_convert_in_pixel_shader_ = REXCVAR_GET(depth_float24_convert_in_pixel_shader);

    // Check if 2x MSAA is supported or needs to be emulated with 4x MSAA
    // instead.
    if (REXCVAR_GET(native_2x_msaa)) {
      msaa_2x_supported_ = true;
      static const DXGI_FORMAT kRenderTargetDXGIFormats[] = {
          DXGI_FORMAT_R16G16B16A16_FLOAT,
          DXGI_FORMAT_R16G16B16A16_SNORM,
          DXGI_FORMAT_R32G32_FLOAT,
          DXGI_FORMAT_D32_FLOAT_S8X24_UINT,
          DXGI_FORMAT_R10G10B10A2_UNORM,
          DXGI_FORMAT_R8G8B8A8_UNORM,
          DXGI_FORMAT_R16G16_FLOAT,
          DXGI_FORMAT_R16G16_SNORM,
          DXGI_FORMAT_R32_FLOAT,
          DXGI_FORMAT_D24_UNORM_S8_UINT,
          // For ownership transfer.
          DXGI_FORMAT_R16G16B16A16_UINT,
          DXGI_FORMAT_R32G32_UINT,
          DXGI_FORMAT_R16G16_UINT,
          DXGI_FORMAT_R32_UINT,
      };
      D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS multisample_quality_levels;
      multisample_quality_levels.SampleCount = 2;
      multisample_quality_levels.Flags = D3D12_MULTISAMPLE_QUALITY_LEVELS_FLAG_NONE;
      for (size_t i = 0; i < rex::countof(kRenderTargetDXGIFormats); ++i) {
        multisample_quality_levels.Format = kRenderTargetDXGIFormats[i];
        multisample_quality_levels.NumQualityLevels = 0;
        if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS,
                                               &multisample_quality_levels,
                                               sizeof(multisample_quality_levels))) ||
            !multisample_quality_levels.NumQualityLevels) {
          msaa_2x_supported_ = false;
          break;
        }
      }
    } else {
      msaa_2x_supported_ = false;
    }
    if (msaa_2x_supported_ && gamma_render_target_as_unorm16_) {
      D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS multisample_quality_levels;
      multisample_quality_levels.SampleCount = 2;
      multisample_quality_levels.Flags = D3D12_MULTISAMPLE_QUALITY_LEVELS_FLAG_NONE;
      multisample_quality_levels.Format = DXGI_FORMAT_R16G16B16A16_UNORM;
      multisample_quality_levels.NumQualityLevels = 0;
      if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS,
                                             &multisample_quality_levels,
                                             sizeof(multisample_quality_levels))) ||
          !multisample_quality_levels.NumQualityLevels) {
        msaa_2x_supported_ = false;
      }
    }
    if (!msaa_2x_supported_) {
      REXGPU_WARN(
          "2x MSAA is not supported, emulated via top-left and bottom-right "
          "samples of 4x MSAA");
    }

    descriptor_pool_color_ = std::make_unique<ui::d3d12::D3D12CpuDescriptorPool>(
        provider, D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 11);
    descriptor_pool_depth_ = std::make_unique<ui::d3d12::D3D12CpuDescriptorPool>(
        provider, D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 11);
    descriptor_pool_srv_ = std::make_unique<ui::d3d12::D3D12CpuDescriptorPool>(
        provider, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 11);

    // Create null render target descriptors for gaps, must be fully typed
    // (though in pipeline states, DXGI_FORMAT_UNKNOWN must be used instead -
    // this would also cause a mismatching format error in the debug layer, but
    // it's a bug in the debug layer itself - needs to be suppressed, and
    // already fixed in some version of Windows).
    null_rtv_descriptor_ss_ = descriptor_pool_color_->AllocateDescriptor();
    null_rtv_descriptor_ms_ = descriptor_pool_color_->AllocateDescriptor();
    if (!null_rtv_descriptor_ss_ || !null_rtv_descriptor_ms_) {
      Shutdown();
      return false;
    }
    D3D12_RENDER_TARGET_VIEW_DESC null_rtv_desc;
    // The format doesn't matter, but it must be bindable as a render target,
    // not DXGI_FORMAT_UNKNOWN.
    null_rtv_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    null_rtv_desc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    null_rtv_desc.Texture2D.MipSlice = 0;
    null_rtv_desc.Texture2D.PlaneSlice = 0;
    device->CreateRenderTargetView(nullptr, &null_rtv_desc, null_rtv_descriptor_ss_.GetHandle());
    null_rtv_desc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DMS;
    device->CreateRenderTargetView(nullptr, &null_rtv_desc, null_rtv_descriptor_ms_.GetHandle());

    // For host depth -> same depth transfers, host depth storing root signature
    // and pipelines.
    D3D12_ROOT_PARAMETER
    host_depth_store_root_parameters[kHostDepthStoreRootParameterCount];
    // Constants.
    D3D12_ROOT_PARAMETER& host_depth_store_root_constants =
        host_depth_store_root_parameters[kHostDepthStoreRootParameterConstants];
    host_depth_store_root_constants.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    host_depth_store_root_constants.Constants.ShaderRegister = 0;
    host_depth_store_root_constants.Constants.RegisterSpace = 0;
    host_depth_store_root_constants.Constants.Num32BitValues =
        sizeof(HostDepthStoreConstants) / sizeof(uint32_t);
    host_depth_store_root_constants.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    // Source.
    D3D12_DESCRIPTOR_RANGE host_depth_store_root_source_range;
    host_depth_store_root_source_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    host_depth_store_root_source_range.NumDescriptors = 1;
    host_depth_store_root_source_range.BaseShaderRegister = 0;
    host_depth_store_root_source_range.RegisterSpace = 0;
    host_depth_store_root_source_range.OffsetInDescriptorsFromTableStart = 0;
    D3D12_ROOT_PARAMETER& host_depth_store_root_source =
        host_depth_store_root_parameters[kHostDepthStoreRootParameterSource];
    host_depth_store_root_source.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    host_depth_store_root_source.DescriptorTable.NumDescriptorRanges = 1;
    host_depth_store_root_source.DescriptorTable.pDescriptorRanges =
        &host_depth_store_root_source_range;
    host_depth_store_root_source.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    // Destination.
    D3D12_DESCRIPTOR_RANGE host_depth_store_root_dest_range;
    host_depth_store_root_dest_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    host_depth_store_root_dest_range.NumDescriptors = 1;
    host_depth_store_root_dest_range.BaseShaderRegister = 0;
    host_depth_store_root_dest_range.RegisterSpace = 0;
    host_depth_store_root_dest_range.OffsetInDescriptorsFromTableStart = 0;
    D3D12_ROOT_PARAMETER& host_depth_store_root_dest =
        host_depth_store_root_parameters[kHostDepthStoreRootParameterDest];
    host_depth_store_root_dest.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    host_depth_store_root_dest.DescriptorTable.NumDescriptorRanges = 1;
    host_depth_store_root_dest.DescriptorTable.pDescriptorRanges =
        &host_depth_store_root_dest_range;
    host_depth_store_root_dest.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    // Root signature.
    D3D12_ROOT_SIGNATURE_DESC host_depth_store_root_desc;
    host_depth_store_root_desc.NumParameters = UINT(rex::countof(host_depth_store_root_parameters));
    host_depth_store_root_desc.pParameters = host_depth_store_root_parameters;
    host_depth_store_root_desc.NumStaticSamplers = 0;
    host_depth_store_root_desc.pStaticSamplers = nullptr;
    host_depth_store_root_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;
    host_depth_store_root_signature_ =
        ui::d3d12::util::CreateRootSignature(provider, host_depth_store_root_desc);
    if (!host_depth_store_root_signature_) {
      REXGPU_ERROR(
          "D3D12RenderTargetCache: Failed to create the host depth storing "
          "root signature");
      Shutdown();
      return false;
    }
    // Pipelines.
    // 1 sample.
    host_depth_store_pipelines_[size_t(xenos::MsaaSamples::k1X)] =
        ui::d3d12::util::CreateComputePipeline(device, shaders::host_depth_store_1xmsaa_cs,
                                               sizeof(shaders::host_depth_store_1xmsaa_cs),
                                               host_depth_store_root_signature_);
    if (!host_depth_store_pipelines_[size_t(xenos::MsaaSamples::k1X)]) {
      REXGPU_ERROR(
          "D3D12RenderTargetCache: Failed to create the 1-sample host depth "
          "storing pipeline");
      Shutdown();
      return false;
    }
    host_depth_store_pipelines_[size_t(xenos::MsaaSamples::k1X)]->SetName(
        L"Host Depth Store 1xMSAA");
    // 2 samples.
    host_depth_store_pipelines_[size_t(xenos::MsaaSamples::k2X)] =
        ui::d3d12::util::CreateComputePipeline(device, shaders::host_depth_store_2xmsaa_cs,
                                               sizeof(shaders::host_depth_store_2xmsaa_cs),
                                               host_depth_store_root_signature_);
    if (!host_depth_store_pipelines_[size_t(xenos::MsaaSamples::k2X)]) {
      REXGPU_ERROR(
          "D3D12RenderTargetCache: Failed to create the 2-sample host depth "
          "storing pipeline");
      Shutdown();
      return false;
    }
    host_depth_store_pipelines_[size_t(xenos::MsaaSamples::k2X)]->SetName(
        L"Host Depth Store 2xMSAA");
    // 4 samples.
    host_depth_store_pipelines_[size_t(xenos::MsaaSamples::k4X)] =
        ui::d3d12::util::CreateComputePipeline(device, shaders::host_depth_store_4xmsaa_cs,
                                               sizeof(shaders::host_depth_store_4xmsaa_cs),
                                               host_depth_store_root_signature_);
    if (!host_depth_store_pipelines_[size_t(xenos::MsaaSamples::k4X)]) {
      REXGPU_ERROR(
          "D3D12RenderTargetCache: Failed to create the 4-sample host depth "
          "storing pipeline");
      Shutdown();
      return false;
    }
    host_depth_store_pipelines_[size_t(xenos::MsaaSamples::k4X)]->SetName(
        L"Host Depth Store 4xMSAA");

    // Transfer and clear vertex buffer, for quads of up to tile granularity.
    transfer_vertex_buffer_pool_ = std::make_unique<ui::d3d12::D3D12UploadBufferPool>(
        provider, std::max(ui::d3d12::D3D12UploadBufferPool::kDefaultPageSize,
                           sizeof(float) * 2 * 6 * Transfer::kMaxCutoutBorderRectangles *
                               xenos::kEdramTileCount));

    // Transfer root signatures.
    D3D12_DESCRIPTOR_RANGE transfer_root_color_srv_range;
    transfer_root_color_srv_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    transfer_root_color_srv_range.NumDescriptors = 1;
    transfer_root_color_srv_range.BaseShaderRegister = kTransferSRVRegisterColor;
    transfer_root_color_srv_range.RegisterSpace = 0;
    transfer_root_color_srv_range.OffsetInDescriptorsFromTableStart = 0;
    D3D12_DESCRIPTOR_RANGE transfer_root_depth_srv_range;
    transfer_root_depth_srv_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    transfer_root_depth_srv_range.NumDescriptors = 1;
    transfer_root_depth_srv_range.BaseShaderRegister = kTransferSRVRegisterDepth;
    transfer_root_depth_srv_range.RegisterSpace = 0;
    transfer_root_depth_srv_range.OffsetInDescriptorsFromTableStart = 0;
    D3D12_DESCRIPTOR_RANGE transfer_root_stencil_srv_range;
    transfer_root_stencil_srv_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    transfer_root_stencil_srv_range.NumDescriptors = 1;
    transfer_root_stencil_srv_range.BaseShaderRegister = kTransferSRVRegisterStencil;
    transfer_root_stencil_srv_range.RegisterSpace = 0;
    transfer_root_stencil_srv_range.OffsetInDescriptorsFromTableStart = 0;
    D3D12_DESCRIPTOR_RANGE transfer_root_host_depth_srv_range;
    transfer_root_host_depth_srv_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    transfer_root_host_depth_srv_range.NumDescriptors = 1;
    transfer_root_host_depth_srv_range.BaseShaderRegister = kTransferSRVRegisterHostDepth;
    transfer_root_host_depth_srv_range.RegisterSpace = 0;
    transfer_root_host_depth_srv_range.OffsetInDescriptorsFromTableStart = 0;
    D3D12_ROOT_PARAMETER
    transfer_root_parameters[kTransferUsedRootParameterCount];
    D3D12_ROOT_SIGNATURE_DESC transfer_root_desc;
    transfer_root_desc.pParameters = transfer_root_parameters;
    transfer_root_desc.NumStaticSamplers = 0;
    transfer_root_desc.pStaticSamplers = nullptr;
    transfer_root_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    for (size_t i = 0; i < size_t(TransferRootSignatureIndex::kCount); ++i) {
      uint32_t transfer_root_mask = kTransferUsedRootParameters[i];
      // Stencil mask constant.
      if (transfer_root_mask & kTransferUsedRootParameterStencilMaskConstantBit) {
        D3D12_ROOT_PARAMETER& transfer_root_stencil_mask_constant =
            transfer_root_parameters[rex::bit_count(
                transfer_root_mask & (kTransferUsedRootParameterStencilMaskConstantBit - 1))];
        transfer_root_stencil_mask_constant.ParameterType =
            D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        transfer_root_stencil_mask_constant.Constants.ShaderRegister =
            kTransferCBVRegisterStencilMask;
        transfer_root_stencil_mask_constant.Constants.RegisterSpace = 0;
        transfer_root_stencil_mask_constant.Constants.Num32BitValues = 1;
        transfer_root_stencil_mask_constant.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
      }
      // Color SRV.
      if (transfer_root_mask & kTransferUsedRootParameterColorSRVBit) {
        D3D12_ROOT_PARAMETER& transfer_root_color_srv = transfer_root_parameters[rex::bit_count(
            transfer_root_mask & (kTransferUsedRootParameterColorSRVBit - 1))];
        transfer_root_color_srv.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        transfer_root_color_srv.DescriptorTable.NumDescriptorRanges = 1;
        transfer_root_color_srv.DescriptorTable.pDescriptorRanges = &transfer_root_color_srv_range;
        transfer_root_color_srv.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
      }
      // Depth SRV.
      if (transfer_root_mask & kTransferUsedRootParameterDepthSRVBit) {
        D3D12_ROOT_PARAMETER& transfer_root_depth_srv = transfer_root_parameters[rex::bit_count(
            transfer_root_mask & (kTransferUsedRootParameterDepthSRVBit - 1))];
        transfer_root_depth_srv.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        transfer_root_depth_srv.DescriptorTable.NumDescriptorRanges = 1;
        transfer_root_depth_srv.DescriptorTable.pDescriptorRanges = &transfer_root_depth_srv_range;
        transfer_root_depth_srv.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
      }
      // Stencil SRV.
      if (transfer_root_mask & kTransferUsedRootParameterStencilSRVBit) {
        D3D12_ROOT_PARAMETER& transfer_root_stencil_srv = transfer_root_parameters[rex::bit_count(
            transfer_root_mask & (kTransferUsedRootParameterStencilSRVBit - 1))];
        transfer_root_stencil_srv.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        transfer_root_stencil_srv.DescriptorTable.NumDescriptorRanges = 1;
        transfer_root_stencil_srv.DescriptorTable.pDescriptorRanges =
            &transfer_root_stencil_srv_range;
        transfer_root_stencil_srv.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
      }
      // Address constant.
      if (transfer_root_mask & kTransferUsedRootParameterAddressConstantBit) {
        D3D12_ROOT_PARAMETER& transfer_root_address_constant =
            transfer_root_parameters[rex::bit_count(
                transfer_root_mask & (kTransferUsedRootParameterAddressConstantBit - 1))];
        transfer_root_address_constant.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        transfer_root_address_constant.Constants.ShaderRegister = kTransferCBVRegisterAddress;
        transfer_root_address_constant.Constants.RegisterSpace = 0;
        transfer_root_address_constant.Constants.Num32BitValues =
            sizeof(TransferAddressConstant) / sizeof(uint32_t);
        transfer_root_address_constant.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
      }
      // Host depth SRV.
      if (transfer_root_mask & kTransferUsedRootParameterHostDepthSRVBit) {
        D3D12_ROOT_PARAMETER& transfer_root_host_depth_srv =
            transfer_root_parameters[rex::bit_count(
                transfer_root_mask & (kTransferUsedRootParameterHostDepthSRVBit - 1))];
        transfer_root_host_depth_srv.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        transfer_root_host_depth_srv.DescriptorTable.NumDescriptorRanges = 1;
        transfer_root_host_depth_srv.DescriptorTable.pDescriptorRanges =
            &transfer_root_host_depth_srv_range;
        transfer_root_host_depth_srv.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
      }
      // Host depth address constant.
      if (transfer_root_mask & kTransferUsedRootParameterHostDepthAddressConstantBit) {
        D3D12_ROOT_PARAMETER& transfer_root_host_address_constant =
            transfer_root_parameters[rex::bit_count(
                transfer_root_mask & (kTransferUsedRootParameterHostDepthAddressConstantBit - 1))];
        transfer_root_host_address_constant.ParameterType =
            D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        transfer_root_host_address_constant.Constants.ShaderRegister =
            kTransferCBVRegisterHostDepthAddress;
        transfer_root_host_address_constant.Constants.RegisterSpace = 0;
        transfer_root_host_address_constant.Constants.Num32BitValues =
            sizeof(TransferAddressConstant) / sizeof(uint32_t);
        transfer_root_host_address_constant.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
      }
      transfer_root_desc.NumParameters = rex::bit_count(transfer_root_mask);
      assert_true(transfer_root_desc.NumParameters <= kTransferUsedRootParameterCount);
      transfer_root_signatures_[i] =
          ui::d3d12::util::CreateRootSignature(provider, transfer_root_desc);
      if (!transfer_root_signatures_[i]) {
        REXGPU_ERROR(
            "D3D12RenderTargetCache: Failed to create the render target "
            "ownership transfer root signature {:X}",
            transfer_root_mask);
        Shutdown();
        return false;
      }
    }

    // Dumping root signatures.
    D3D12_DESCRIPTOR_RANGE dump_root_source_range;
    dump_root_source_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    dump_root_source_range.NumDescriptors = 1;
    dump_root_source_range.BaseShaderRegister = 0;
    dump_root_source_range.RegisterSpace = 0;
    dump_root_source_range.OffsetInDescriptorsFromTableStart = 0;
    D3D12_DESCRIPTOR_RANGE dump_root_stencil_range;
    dump_root_stencil_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    dump_root_stencil_range.NumDescriptors = 1;
    dump_root_stencil_range.BaseShaderRegister = 1;
    dump_root_stencil_range.RegisterSpace = 0;
    dump_root_stencil_range.OffsetInDescriptorsFromTableStart = 0;
    D3D12_DESCRIPTOR_RANGE dump_root_edram_range;
    dump_root_edram_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    dump_root_edram_range.NumDescriptors = 1;
    dump_root_edram_range.BaseShaderRegister = 0;
    dump_root_edram_range.RegisterSpace = 0;
    dump_root_edram_range.OffsetInDescriptorsFromTableStart = 0;
    D3D12_ROOT_PARAMETER
    dump_root_color_parameters[kDumpRootParameterColorCount];
    D3D12_ROOT_PARAMETER
    dump_root_depth_parameters[kDumpRootParameterDepthCount];
    for (uint32_t i = 0; i < 2; ++i) {
      // Offsets.
      D3D12_ROOT_PARAMETER& dump_root_offsets =
          i ? dump_root_depth_parameters[kDumpRootParameterOffsets]
            : dump_root_color_parameters[kDumpRootParameterOffsets];
      dump_root_offsets.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
      dump_root_offsets.Constants.ShaderRegister = kDumpCbufferOffsets;
      dump_root_offsets.Constants.RegisterSpace = 0;
      dump_root_offsets.Constants.Num32BitValues = sizeof(DumpOffsets) / sizeof(uint32_t);
      dump_root_offsets.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
      // Source.
      D3D12_ROOT_PARAMETER& dump_root_source =
          i ? dump_root_depth_parameters[kDumpRootParameterSource]
            : dump_root_color_parameters[kDumpRootParameterSource];
      dump_root_source.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
      dump_root_source.DescriptorTable.NumDescriptorRanges = 1;
      dump_root_source.DescriptorTable.pDescriptorRanges = &dump_root_source_range;
      dump_root_source.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
      // Stencil.
      if (i) {
        D3D12_ROOT_PARAMETER& dump_root_stencil =
            dump_root_depth_parameters[kDumpRootParameterDepthStencil];
        dump_root_stencil.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        dump_root_stencil.DescriptorTable.NumDescriptorRanges = 1;
        dump_root_stencil.DescriptorTable.pDescriptorRanges = &dump_root_stencil_range;
        dump_root_stencil.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
      }
      // Pitches.
      D3D12_ROOT_PARAMETER& dump_root_pitches =
          i ? dump_root_depth_parameters[kDumpRootParameterDepthPitches]
            : dump_root_color_parameters[kDumpRootParameterColorPitches];
      dump_root_pitches.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
      dump_root_pitches.Constants.ShaderRegister = kDumpCbufferPitches;
      dump_root_pitches.Constants.RegisterSpace = 0;
      dump_root_pitches.Constants.Num32BitValues = sizeof(DumpPitches) / sizeof(uint32_t);
      dump_root_pitches.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
      // EDRAM.
      D3D12_ROOT_PARAMETER& dump_root_edram =
          i ? dump_root_depth_parameters[kDumpRootParameterDepthEdram]
            : dump_root_color_parameters[kDumpRootParameterColorEdram];
      dump_root_edram.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
      dump_root_edram.DescriptorTable.NumDescriptorRanges = 1;
      dump_root_edram.DescriptorTable.pDescriptorRanges = &dump_root_edram_range;
      dump_root_edram.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    }
    D3D12_ROOT_SIGNATURE_DESC dump_root_desc;
    dump_root_desc.NumParameters = UINT(rex::countof(dump_root_color_parameters));
    dump_root_desc.pParameters = dump_root_color_parameters;
    dump_root_desc.NumStaticSamplers = 0;
    dump_root_desc.pStaticSamplers = nullptr;
    dump_root_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;
    dump_root_signature_color_ = ui::d3d12::util::CreateRootSignature(provider, dump_root_desc);
    if (!dump_root_signature_color_) {
      REXGPU_ERROR(
          "D3D12RenderTargetCache: Failed to create the color render target "
          "dumping root signature");
      Shutdown();
      return false;
    }
    dump_root_desc.NumParameters = UINT(rex::countof(dump_root_depth_parameters));
    dump_root_desc.pParameters = dump_root_depth_parameters;
    dump_root_signature_depth_ = ui::d3d12::util::CreateRootSignature(provider, dump_root_desc);
    if (!dump_root_signature_depth_) {
      REXGPU_ERROR(
          "D3D12RenderTargetCache: Failed to create the depth render target "
          "dumping root signature");
      Shutdown();
      return false;
    }

    // k_32_FLOAT and k_32_32_FLOAT clear root signature and pipelines.
    D3D12_ROOT_PARAMETER uint32_rtv_clear_root_constants;
    uint32_rtv_clear_root_constants.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    uint32_rtv_clear_root_constants.Constants.ShaderRegister = 0;
    uint32_rtv_clear_root_constants.Constants.RegisterSpace = 0;
    uint32_rtv_clear_root_constants.Constants.Num32BitValues = 2;
    uint32_rtv_clear_root_constants.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_ROOT_SIGNATURE_DESC uint32_rtv_clear_root_desc;
    uint32_rtv_clear_root_desc.NumParameters = 1;
    uint32_rtv_clear_root_desc.pParameters = &uint32_rtv_clear_root_constants;
    uint32_rtv_clear_root_desc.NumStaticSamplers = 0;
    uint32_rtv_clear_root_desc.pStaticSamplers = nullptr;
    uint32_rtv_clear_root_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;
    uint32_rtv_clear_root_signature_ =
        ui::d3d12::util::CreateRootSignature(provider, uint32_rtv_clear_root_desc);
    if (!uint32_rtv_clear_root_signature_) {
      REXGPU_ERROR(
          "D3D12RenderTargetCache: Failed to create the k_32_FLOAT / "
          "k_32_32_FLOAT render target clearing root signature");
      Shutdown();
      return false;
    }
    D3D12_GRAPHICS_PIPELINE_STATE_DESC uint32_rtv_clear_pipeline_desc = {};
    uint32_rtv_clear_pipeline_desc.pRootSignature = uint32_rtv_clear_root_signature_;
    uint32_rtv_clear_pipeline_desc.VS.pShaderBytecode = shaders::fullscreen_cw_vs;
    uint32_rtv_clear_pipeline_desc.VS.BytecodeLength = sizeof(shaders::fullscreen_cw_vs);
    uint32_rtv_clear_pipeline_desc.PS.pShaderBytecode = shaders::clear_uint2_ps;
    uint32_rtv_clear_pipeline_desc.PS.BytecodeLength = sizeof(shaders::clear_uint2_ps);
    uint32_rtv_clear_pipeline_desc.BlendState.RenderTarget[0].RenderTargetWriteMask =
        D3D12_COLOR_WRITE_ENABLE_ALL;
    uint32_rtv_clear_pipeline_desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    uint32_rtv_clear_pipeline_desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    uint32_rtv_clear_pipeline_desc.RasterizerState.DepthClipEnable = TRUE;
    uint32_rtv_clear_pipeline_desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    uint32_rtv_clear_pipeline_desc.NumRenderTargets = 1;
    for (size_t i = 0; i < 2; ++i) {
      uint32_rtv_clear_pipeline_desc.RTVFormats[0] =
          GetColorOwnershipTransferDXGIFormat(i ? xenos::ColorRenderTargetFormat::k_32_32_FLOAT
                                                : xenos::ColorRenderTargetFormat::k_32_FLOAT);
      for (size_t j = size_t(xenos::MsaaSamples::k1X); j <= size_t(xenos::MsaaSamples::k4X); ++j) {
        if (xenos::MsaaSamples(j) == xenos::MsaaSamples::k2X && !msaa_2x_supported_) {
          // Using sample 0 as 0 and 3 as 1 for 2x instead.
          uint32_rtv_clear_pipeline_desc.SampleMask = 0b1001;
          uint32_rtv_clear_pipeline_desc.SampleDesc.Count = 4;
        } else {
          uint32_rtv_clear_pipeline_desc.SampleMask = UINT_MAX;
          uint32_rtv_clear_pipeline_desc.SampleDesc.Count = 1 << j;
        }
        ID3D12PipelineState* uint32_rtv_clear_pipeline;
        if (FAILED(device->CreateGraphicsPipelineState(&uint32_rtv_clear_pipeline_desc,
                                                       IID_PPV_ARGS(&uint32_rtv_clear_pipeline)))) {
          REXGPU_ERROR(
              "D3D12RenderTargetCache: Failed to create the {} {}-sample "
              "render target clearing pipeline",
              i ? "k_32_32_FLOAT" : "k_32_FLOAT", uint32_t(1) << j);
          Shutdown();
          return false;
        }
        uint32_rtv_clear_pipelines_[i][j] = uint32_rtv_clear_pipeline;
        auto uint32_rtv_clear_pipeline_name = rex::string::to_utf16(fmt::format(
            "Resolve Clear {} {}xMSAA", i ? "k_32_32_FLOAT" : "k_32_FLOAT", uint32_t(1) << j));
        uint32_rtv_clear_pipeline->SetName(
            reinterpret_cast<LPCWSTR>(uint32_rtv_clear_pipeline_name.c_str()));
      }
    }

    // d3d12_transfer_stencil_clear_by_draw: stencil REPLACE with the
    // reference 0 over a full-viewport triangle, depth untouched. A missing
    // pipeline falls back to ClearDepthStencilView.
    if (REXCVAR_GET(d3d12_transfer_stencil_clear_by_draw)) {
      D3D12_GRAPHICS_PIPELINE_STATE_DESC stencil_clear_pipeline_desc = {};
      stencil_clear_pipeline_desc.pRootSignature = uint32_rtv_clear_root_signature_;
      stencil_clear_pipeline_desc.VS.pShaderBytecode = shaders::fullscreen_cw_vs;
      stencil_clear_pipeline_desc.VS.BytecodeLength = sizeof(shaders::fullscreen_cw_vs);
      stencil_clear_pipeline_desc.SampleMask = UINT_MAX;
      stencil_clear_pipeline_desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
      stencil_clear_pipeline_desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
      stencil_clear_pipeline_desc.RasterizerState.DepthClipEnable = FALSE;
      D3D12_DEPTH_STENCIL_DESC& stencil_clear_depth_stencil =
          stencil_clear_pipeline_desc.DepthStencilState;
      stencil_clear_depth_stencil.DepthEnable = FALSE;
      stencil_clear_depth_stencil.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
      stencil_clear_depth_stencil.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
      stencil_clear_depth_stencil.StencilEnable = TRUE;
      stencil_clear_depth_stencil.StencilReadMask = UINT8_MAX;
      stencil_clear_depth_stencil.StencilWriteMask = UINT8_MAX;
      stencil_clear_depth_stencil.FrontFace.StencilFailOp = D3D12_STENCIL_OP_KEEP;
      stencil_clear_depth_stencil.FrontFace.StencilDepthFailOp = D3D12_STENCIL_OP_KEEP;
      stencil_clear_depth_stencil.FrontFace.StencilPassOp = D3D12_STENCIL_OP_REPLACE;
      stencil_clear_depth_stencil.FrontFace.StencilFunc = D3D12_COMPARISON_FUNC_ALWAYS;
      stencil_clear_depth_stencil.BackFace = stencil_clear_depth_stencil.FrontFace;
      stencil_clear_pipeline_desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
      stencil_clear_pipeline_desc.NumRenderTargets = 0;
      for (size_t i = 0; i < 2; ++i) {
        stencil_clear_pipeline_desc.DSVFormat = GetDepthDSVDXGIFormat(
            i ? xenos::DepthRenderTargetFormat::kD24FS8 : xenos::DepthRenderTargetFormat::kD24S8);
        for (size_t j = size_t(xenos::MsaaSamples::k1X); j <= size_t(xenos::MsaaSamples::k4X); ++j) {
          // All samples like ClearDepthStencilView (2x as 4x included).
          stencil_clear_pipeline_desc.SampleDesc.Count =
              (xenos::MsaaSamples(j) == xenos::MsaaSamples::k2X && !msaa_2x_supported_)
                  ? 4
                  : UINT(1) << j;
          if (FAILED(device->CreateGraphicsPipelineState(
                  &stencil_clear_pipeline_desc,
                  IID_PPV_ARGS(&transfer_stencil_clear_pipelines_[i][j])))) {
            transfer_stencil_clear_pipelines_[i][j] = nullptr;
          }
        }
      }
      std::fprintf(stderr, "REX_GPU_TEST_SWITCHES transfer_stencil_clear_by_draw=%u\n",
                   transfer_stencil_clear_pipelines_[1][size_t(xenos::MsaaSamples::k1X)] ? 1u
                                                                                        : 0u);
      std::fflush(stderr);
    }

    // FXC-compiled depth / stencil dumping shader is ~2 KB, reserve 4 KB for
    // some additional space.
    built_shader_.reserve(1024);
  } else if (path_ == Path::kPixelShaderInterlock) {
    // Pixel shader interlock (rasterizer-ordered view).

    // Blending is done in linear space directly in shaders.
    gamma_render_target_as_unorm16_ = false;

    // Always true float24 depth rounded to the nearest even.
    depth_float24_round_ = true;
    depth_float24_convert_in_pixel_shader_ = true;

    // Only ForcedSampleCount, which doesn't support 2x.
    msaa_2x_supported_ = false;

    // Create the resolve EDRAM buffer clearing root signature.
    std::array<D3D12_ROOT_PARAMETER, 2> resolve_rov_clear_root_parameters;
    // Parameter 0 is constants.
    resolve_rov_clear_root_parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    resolve_rov_clear_root_parameters[0].Constants.ShaderRegister = 0;
    resolve_rov_clear_root_parameters[0].Constants.RegisterSpace = 0;
    // Binding all of the shared memory at 1x resolution, portions with scaled
    // resolution.
    resolve_rov_clear_root_parameters[0].Constants.Num32BitValues =
        sizeof(draw_util::ResolveClearShaderConstants) / sizeof(uint32_t);
    resolve_rov_clear_root_parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    // Parameter 1 is the destination (EDRAM).
    D3D12_DESCRIPTOR_RANGE resolve_rov_clear_dest_range;
    resolve_rov_clear_dest_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    resolve_rov_clear_dest_range.NumDescriptors = 1;
    resolve_rov_clear_dest_range.BaseShaderRegister = 0;
    resolve_rov_clear_dest_range.RegisterSpace = 0;
    resolve_rov_clear_dest_range.OffsetInDescriptorsFromTableStart = 0;
    resolve_rov_clear_root_parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    resolve_rov_clear_root_parameters[1].DescriptorTable.NumDescriptorRanges = 1;
    resolve_rov_clear_root_parameters[1].DescriptorTable.pDescriptorRanges =
        &resolve_rov_clear_dest_range;
    resolve_rov_clear_root_parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_ROOT_SIGNATURE_DESC resolve_rov_clear_root_signature_desc;
    resolve_rov_clear_root_signature_desc.NumParameters =
        UINT(resolve_rov_clear_root_parameters.size());
    resolve_rov_clear_root_signature_desc.pParameters = resolve_rov_clear_root_parameters.data();
    resolve_rov_clear_root_signature_desc.NumStaticSamplers = 0;
    resolve_rov_clear_root_signature_desc.pStaticSamplers = nullptr;
    resolve_rov_clear_root_signature_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;
    resolve_rov_clear_root_signature_ =
        ui::d3d12::util::CreateRootSignature(provider, resolve_rov_clear_root_signature_desc);
    if (resolve_rov_clear_root_signature_ == nullptr) {
      REXGPU_ERROR(
          "D3D12RenderTargetCache: Failed to create the resolve EDRAM buffer "
          "clear root signature");
      Shutdown();
      return false;
    }

    // Create the resolve EDRAM buffer clearing pipelines.
    resolve_rov_clear_32bpp_pipeline_ = ui::d3d12::util::CreateComputePipeline(
        device,
        draw_resolution_scaled ? shaders::resolve_clear_32bpp_scaled_cs
                               : shaders::resolve_clear_32bpp_cs,
        draw_resolution_scaled ? sizeof(shaders::resolve_clear_32bpp_scaled_cs)
                               : sizeof(shaders::resolve_clear_32bpp_cs),
        resolve_rov_clear_root_signature_);
    if (resolve_rov_clear_32bpp_pipeline_ == nullptr) {
      REXGPU_ERROR(
          "D3D12RenderTargetCache: Failed to create the 32bpp resolve EDRAM "
          "buffer clear pipeline");
      Shutdown();
      return false;
    }
    resolve_rov_clear_32bpp_pipeline_->SetName(L"Resolve Clear 32bpp");
    resolve_rov_clear_64bpp_pipeline_ = ui::d3d12::util::CreateComputePipeline(
        device,
        draw_resolution_scaled ? shaders::resolve_clear_64bpp_scaled_cs
                               : shaders::resolve_clear_64bpp_cs,
        draw_resolution_scaled ? sizeof(shaders::resolve_clear_64bpp_scaled_cs)
                               : sizeof(shaders::resolve_clear_64bpp_cs),
        resolve_rov_clear_root_signature_);
    if (resolve_rov_clear_64bpp_pipeline_ == nullptr) {
      REXGPU_ERROR(
          "D3D12RenderTargetCache: Failed to create the 64bpp resolve EDRAM "
          "buffer clear pipeline");
      Shutdown();
      return false;
    }
    resolve_rov_clear_64bpp_pipeline_->SetName(L"Resolve Clear 64bpp");
  } else {
    assert_unhandled_case(path_);
    Shutdown();
    return false;
  }

  InitializeCommon();

  return true;
}

bool D3D12RenderTargetCache::CaptureEmbeddedColorTarget(
    const char* diagnostic_label, const char* dump_path,
    uint64_t maximum_fp16_bytes) {
  if (path_ != Path::kHostRenderTargets) {
    std::fprintf(stderr,
                 "REX_EMBEDDED_PROMPT_RTV_READBACK result=skipped path=rov\n");
    std::fflush(stderr);
    return false;
  }

  RenderTarget* const* accumulated = last_update_accumulated_render_targets();
  auto* render_target = static_cast<D3D12RenderTarget*>(accumulated[1]);
  if (!render_target) {
    std::fprintf(stderr,
                 "REX_EMBEDDED_PROMPT_RTV_READBACK result=missing_color0\n");
    std::fflush(stderr);
    return false;
  }

  if (maximum_fp16_bytes) {
    const auto desc = render_target->resource()->GetDesc();
    // A configurable target selector must not send another host format into
    // the legacy FP16 content interpreter, or allocate an unbounded readback.
    if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
        desc.Format != DXGI_FORMAT_R16G16B16A16_FLOAT ||
        !desc.Height || desc.Width > maximum_fp16_bytes / 8 / desc.Height) {
      std::fprintf(stderr,
                   "REX_EMBEDDED_RTV_CAPTURE label=%s result=rejected_contract "
                   "format=%u size=%llux%u limit=%llu\n",
                   diagnostic_label, uint32_t(desc.Format),
                   static_cast<unsigned long long>(desc.Width), desc.Height,
                   static_cast<unsigned long long>(maximum_fp16_bytes));
      std::fflush(stderr);
      return false;
    }
  }
  return CaptureEmbeddedColorTarget(render_target, diagnostic_label, dump_path);
}

bool D3D12RenderTargetCache::QueueEmbeddedColorTarget(
    const char* diagnostic_label, const char* dump_path,
    uint64_t maximum_fp16_bytes) {
  if (path_ != Path::kHostRenderTargets || !diagnostic_label || !dump_path ||
      !command_processor_.IsSubmissionOpen() ||
      queued_color_target_readback_count_ >= 9 ||
      maximum_fp16_bytes > UINT64_C(48) * 1024 * 1024) return false;
  auto* target = static_cast<D3D12RenderTarget*>(
      last_update_accumulated_render_targets()[1]);
  if (!target) return false;
  PendingColorTargetReadback pending;
  if (!pending.copy.Create(command_processor_.GetD3D12Provider().GetDevice(),
                            target->resource(), maximum_fp16_bytes)) return false;
  pending.submission = command_processor_.GetCurrentSubmission();
  pending.label = diagnostic_label;
  pending.path = dump_path;
  command_processor_.SubmitBarriers();
  pending.copy.Enqueue(command_processor_.GetDeferredCommandList(),
                        target->resource_state());
  pending_color_target_readbacks_.push_back(std::move(pending));
  ++queued_color_target_readback_count_;
  return true;
}

bool D3D12RenderTargetCache::QueueCameraDepthClearReadback(
    uint64_t frame, uint64_t draw, uint64_t clear_draw, bool after_alias) {
  if (!frame || !draw || !clear_draw || path_ != Path::kHostRenderTargets ||
      !command_processor_.IsSubmissionOpen()) return false;
  auto* target = static_cast<D3D12RenderTarget*>(last_update_accumulated_render_targets()[0]);
  if (!target) return false;
  const auto key = target->key();
  if (!key.is_depth || key.base_tiles != 0 || key.pitch_tiles_at_32bpp != 16 ||
      key.msaa_samples != (after_alias ? xenos::MsaaSamples::k2X : xenos::MsaaSamples::k4X) ||
      key.GetDepthFormat() != xenos::DepthRenderTargetFormat::kD24FS8 ||
      GetKeyScaleX(key) != 1 || GetKeyScaleY(key) != 1) return false;
  const auto desc = target->resource()->GetDesc();
  const bool queued = QueueDepthSourceReadback(target, 0, 0, frame, draw, after_alias);
  std::fprintf(stderr,
      "REX_CAMERA_DEPTH_CLEAR frame=%llu draw=%llu clear_draw=%llu stage=%s queued=%u "
      "key=%08X resource=%p guest_msaa=%u host_samples=%u source_width=%llu source_height=%u "
      "rows=%u scope=host_depth_samples_first_rows_not_resolved_depth\n",
      static_cast<unsigned long long>(frame), static_cast<unsigned long long>(draw),
      static_cast<unsigned long long>(clear_draw), after_alias ? "after_alias" : "after_clear",
      queued ? 1u : 0u, key.key, static_cast<void*>(target->resource()), uint32_t(key.msaa_samples),
      desc.SampleDesc.Count, static_cast<unsigned long long>(desc.Width), desc.Height,
      embedded_camera_depth_clear_policy::ReadbackBudget::kRows);
  std::fflush(stderr);
  return queued;
}

bool D3D12RenderTargetCache::QueueDepthSourceReadback(
    D3D12RenderTarget* target, uint32_t ordinal, uint32_t destination,
    uint64_t camera_frame, uint64_t camera_draw, bool after_alias) {
  if (!target || !target->key().is_depth || !command_processor_.IsSubmissionOpen() ||
      (!camera_frame && (captured_depth_sources_.size() >= 2 ||
      std::find(captured_depth_sources_.begin(), captured_depth_sources_.end(),
                target->resource()) != captured_depth_sources_.end()))) return false;
  const auto desc = target->resource()->GetDesc();
  const uint64_t camera_bytes = camera_frame
      ? camera_depth_readback_budget_.Reserve(desc.Width, desc.Height, desc.SampleDesc.Count) : 0;
  if (camera_frame && (!camera_draw || !camera_bytes)) return false;
  constexpr uint64_t kLimit = UINT64_C(64) * 1024 * 1024;
  if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
      desc.Format != DXGI_FORMAT_R32G8X24_TYPELESS || desc.DepthOrArraySize != 1 ||
      desc.MipLevels != 1 || !desc.Width || !desc.Height ||
      (desc.SampleDesc.Count != 1 && desc.SampleDesc.Count != 2 && desc.SampleDesc.Count != 4) ||
      desc.Width > kLimit / 8 / desc.Height / desc.SampleDesc.Count) return false;
  PendingDepthSourceReadback p;
  p.source = target->resource(); p.width = uint32_t(desc.Width); p.height = desc.Height;
  p.samples = desc.SampleDesc.Count; p.bytes = uint64_t(p.width) * p.height * p.samples * 8;
  if (camera_frame) {
    p.height = embedded_camera_depth_clear_policy::ReadbackBudget::kRows;
    p.bytes = camera_bytes;
    p.camera_frame = camera_frame;
    p.camera_draw = camera_draw;
  }
  p.ordinal = ordinal; p.destination = destination;
  p.path = "rex_resolve_depth_source_" + std::to_string(ordinal) + ".bin";
  if (camera_frame) {
    p.path = "rex_camera_depth_frame_" + std::to_string(camera_frame) + "_draw_" +
        std::to_string(camera_draw) + (after_alias ? "_after_alias.bin" : "_after_clear.bin");
  }
  auto* device = command_processor_.GetD3D12Provider().GetDevice();
  D3D12_RESOURCE_DESC buffer_desc;
  ui::d3d12::util::FillBufferResourceDesc(buffer_desc, p.bytes, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
  if (FAILED(device->CreateCommittedResource(&ui::d3d12::util::kHeapPropertiesDefault,
      D3D12_HEAP_FLAG_NONE, &buffer_desc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
      nullptr, IID_PPV_ARGS(&p.output)))) return false;
  buffer_desc.Flags = D3D12_RESOURCE_FLAG_NONE;
  if (FAILED(device->CreateCommittedResource(&ui::d3d12::util::kHeapPropertiesReadback,
      D3D12_HEAP_FLAG_NONE, &buffer_desc, D3D12_RESOURCE_STATE_COPY_DEST,
      nullptr, IID_PPV_ARGS(&p.readback)))) return false;
  D3D12_DESCRIPTOR_RANGE ranges[2] = {};
  D3D12_ROOT_PARAMETER parameters[4] = {};
  parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  parameters[0].Constants.Num32BitValues = 3;
  for (uint32_t i = 0; i < 2; ++i) {
    ranges[i].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[i].NumDescriptors = 1; ranges[i].BaseShaderRegister = i;
    parameters[i + 1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[i + 1].DescriptorTable = {1, &ranges[i]};
  }
  parameters[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
  D3D12_ROOT_SIGNATURE_DESC root_desc = {};
  root_desc.NumParameters = 4; root_desc.pParameters = parameters;
  Microsoft::WRL::ComPtr<ID3DBlob> serialized;
  if (FAILED(D3D12SerializeRootSignature(&root_desc, D3D_ROOT_SIGNATURE_VERSION_1,
                                        &serialized, nullptr)) ||
      FAILED(device->CreateRootSignature(0, serialized->GetBufferPointer(),
          serialized->GetBufferSize(), IID_PPV_ARGS(&p.root)))) return false;
  D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline_desc = {};
  pipeline_desc.pRootSignature = p.root.Get();
  pipeline_desc.CS = p.samples == 1
      ? D3D12_SHADER_BYTECODE{diagnostic_depth_samples_single, sizeof(diagnostic_depth_samples_single)}
      : D3D12_SHADER_BYTECODE{diagnostic_depth_samples_msaa, sizeof(diagnostic_depth_samples_msaa)};
  if (FAILED(device->CreateComputePipelineState(&pipeline_desc, IID_PPV_ARGS(&p.pipeline)))) return false;
  ui::d3d12::util::DescriptorCpuGpuHandlePair descriptors[2];
  if (!command_processor_.RequestOneUseSingleViewDescriptors(2, descriptors)) return false;
  device->CopyDescriptorsSimple(1, descriptors[0].first, target->descriptor_srv().GetHandle(),
                                 D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  device->CopyDescriptorsSimple(1, descriptors[1].first, target->descriptor_srv_stencil().GetHandle(),
                                 D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  p.submission = command_processor_.GetCurrentSubmission();
  const auto previous = target->resource_state();
  command_processor_.PushTransitionBarrier(target->resource(), previous,
                                           D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  command_processor_.SubmitBarriers();
  auto& commands = command_processor_.GetDeferredCommandList();
  command_processor_.SetExternalPipeline(p.pipeline.Get());
  commands.D3DSetComputeRootSignature(p.root.Get());
  const uint32_t constants[] = {p.width, p.height, p.samples};
  commands.D3DSetComputeRoot32BitConstants(0, 3, constants, 0);
  commands.D3DSetComputeRootDescriptorTable(1, descriptors[0].second);
  commands.D3DSetComputeRootDescriptorTable(2, descriptors[1].second);
  commands.D3DSetComputeRootUnorderedAccessView(3, p.output->GetGPUVirtualAddress());
  commands.D3DDispatch((p.width + 7) / 8, (p.height + 7) / 8, p.samples);
  command_processor_.PushTransitionBarrier(target->resource(),
      D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, previous);
  command_processor_.PushTransitionBarrier(p.output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                           D3D12_RESOURCE_STATE_COPY_SOURCE);
  command_processor_.SubmitBarriers();
  commands.D3DCopyBufferRegion(p.readback.Get(), 0, p.output.Get(), 0, p.bytes);
  std::fprintf(stderr,
      "%s ordinal=%u dest_base=0x%08X resource=%p "
      "width=%u height=%u samples=%u bytes=%llu submission=%llu path=%s\n",
      camera_frame ? "REX_CAMERA_DEPTH_CLEAR_QUEUED" : "REX_EMBEDDED_DEPTH_SOURCE_QUEUED",
      ordinal, destination, static_cast<void*>(target->resource()), p.width, p.height, p.samples,
      static_cast<unsigned long long>(p.bytes), static_cast<unsigned long long>(p.submission), p.path.c_str());
  if (!camera_frame) captured_depth_sources_.push_back(target->resource());
  pending_depth_source_readbacks_.push_back(std::move(p));
  return true;
}

void D3D12RenderTargetCache::CompleteDepthSourceReadbacks() {
  const uint64_t completed = command_processor_.GetCompletedSubmission();
  while (!pending_depth_source_readbacks_.empty() &&
         pending_depth_source_readbacks_.front().submission <= completed) {
    const auto& p = pending_depth_source_readbacks_.front();
    D3D12_RANGE range{0, SIZE_T(p.bytes)};
    void* data = nullptr;
    size_t written = 0;
    uint32_t hash = 2166136261u;
    if (SUCCEEDED(p.readback->Map(0, &range, &data))) {
      if (p.camera_frame) {
        const auto* bytes = static_cast<const uint8_t*>(data);
        for (uint64_t i = 0; i < p.bytes; ++i) hash = (hash ^ bytes[i]) * 16777619u;
      }
      if (FILE* file = std::fopen(p.path.c_str(), "wb")) {
        written = std::fwrite(data, 1, size_t(p.bytes), file);
        if (std::fclose(file) != 0) written = 0;
      }
      D3D12_RANGE no_write{0, 0}; p.readback->Unmap(0, &no_write);
    }
    std::fprintf(stderr,
        "%s result=%u ordinal=%u bytes=%llu written=%llu "
        "submission=%llu completed=%llu path=%s camera_frame=%llu camera_draw=%llu hash=%08X\n",
        p.camera_frame ? "REX_CAMERA_DEPTH_CLEAR_COMPLETE" :
            (p.samples ? "REX_EMBEDDED_DEPTH_SOURCE_COMPLETE" : "REX_EMBEDDED_DEPTH_EDRAM_COMPLETE"),
        written == p.bytes ? 1u : 0u,
        p.ordinal, static_cast<unsigned long long>(p.bytes), static_cast<unsigned long long>(written),
        static_cast<unsigned long long>(p.submission), static_cast<unsigned long long>(completed), p.path.c_str(),
        static_cast<unsigned long long>(p.camera_frame), static_cast<unsigned long long>(p.camera_draw), hash);
    pending_depth_source_readbacks_.pop_front();
  }
}

void D3D12RenderTargetCache::CaptureSceneAliasTransfers(
    RenderTarget* const* targets, const std::vector<Transfer>* transfers, bool after) {
  if (!scene_update_capture_ || !REXCVAR_GET(embedded_camera_scene_alias_capture)) return;
  const auto& context = *scene_update_capture_;
  using Policy = embedded_scene_alias_capture_policy::Budget;
  const auto record = [&](uint32_t ordinal, D3D12RenderTarget* color) {
    auto& pair = scene_alias_pairs_[ordinal - 1];
    ++scene_alias_budget_.attempts;
    const bool queued = QueueSceneAliasReadback(color, ordinal, after);
    if (queued) ++scene_alias_budget_.queued; else ++scene_alias_budget_.failures;
    std::fprintf(stderr,
        "REX_SCENE_ALIAS_EVENT frame=%llu update=%llu next_draw=%llu pair=%u out_update=%llu out_draw=%llu "
        "stage=%s queued=%u color=%p color_key=%08X alias=%p alias_key=%08X start=%u end=%u "
        "scope=sample_copy_at_transfer_boundary_not_payload_validity\n",
        static_cast<unsigned long long>(context.frame), static_cast<unsigned long long>(context.update),
        static_cast<unsigned long long>(context.next_draw), ordinal,
        static_cast<unsigned long long>(pair.out_update), static_cast<unsigned long long>(pair.out_draw),
        after ? "after_restore" : "before_alias", queued ? 1u : 0u,
        static_cast<void*>(pair.color.Get()), Policy::kColorKey,
        static_cast<void*>(pair.alias.Get()), Policy::kDepthKey, pair.start, pair.end);
  };
  for (uint32_t slot = 0; slot <= xenos::kMaxColorRenderTargets; ++slot) {
    auto* dest = static_cast<D3D12RenderTarget*>(targets[slot]);
    if (!dest) continue;
    for (const auto& transfer : transfers[slot]) {
      auto* source = static_cast<D3D12RenderTarget*>(transfer.source);
      if (!source) continue;
      if (!after && dest->key().key == Policy::kDepthKey && source->key().key == Policy::kColorKey) {
        const auto desc = source->resource()->GetDesc();
        const auto alias_desc = dest->resource()->GetDesc();
        bool pending = false;
        for (uint32_t i = 0; i < scene_alias_budget_.pairs; ++i)
          pending |= !scene_alias_pairs_[i].returned;
        if (pending || GetKeyScaleX(source->key()) != 1 || GetKeyScaleY(source->key()) != 1 ||
            GetKeyScaleX(dest->key()) != 1 || GetKeyScaleY(dest->key()) != 1 ||
            desc.Width != 1280 || desc.Height < 384 || desc.SampleDesc.Count != 2 ||
            alias_desc.Width != 640 || alias_desc.SampleDesc.Count != 4 ||
            alias_desc.Format != DXGI_FORMAT_R32G8X24_TYPELESS) {
          ++scene_alias_budget_.dropped;
          continue;
        }
        const uint32_t ordinal = scene_alias_budget_.Reserve(context.frame, transfer.start_tiles, transfer.end_tiles);
        if (!ordinal) continue;
        auto& pair = scene_alias_pairs_[ordinal - 1];
        pair.color = source->resource(); pair.alias = dest->resource();
        pair.out_update = context.update; pair.out_draw = context.next_draw;
        pair.start = transfer.start_tiles; pair.end = transfer.end_tiles;
        record(ordinal, source);
      } else if (after && dest->key().key == Policy::kColorKey && source->key().key == Policy::kDepthKey) {
        for (uint32_t i = 0; i < scene_alias_budget_.pairs; ++i) {
          auto& pair = scene_alias_pairs_[i];
          if (pair.returned || scene_alias_budget_.frame != context.frame ||
              context.update <= pair.out_update || context.next_draw <= pair.out_draw ||
              pair.color.Get() != dest->resource() || pair.alias.Get() != source->resource() ||
              pair.start != transfer.start_tiles || pair.end != transfer.end_tiles) continue;
          pair.returned = true;
          record(i + 1, dest);
        }
      }
    }
  }
  if (after) {
    uint32_t pending = 0;
    for (uint32_t i = 0; i < scene_alias_budget_.pairs; ++i)
      pending += scene_alias_pairs_[i].returned ? 0u : 1u;
    std::fprintf(stderr,
        "REX_SCENE_ALIAS_UPDATE frame=%llu update=%llu next_draw=%llu pairs=%u attempts=%u queued=%u "
        "failures=%u dropped=%u pending=%u reserved_bytes=%llu scope=bounded_capture_not_payload_validity\n",
        static_cast<unsigned long long>(context.frame), static_cast<unsigned long long>(context.update),
        static_cast<unsigned long long>(context.next_draw), scene_alias_budget_.pairs,
        scene_alias_budget_.attempts, scene_alias_budget_.queued, scene_alias_budget_.failures,
        scene_alias_budget_.dropped, pending, static_cast<unsigned long long>(scene_alias_budget_.reserved_bytes));
  }
}

bool D3D12RenderTargetCache::QueueSceneAliasReadback(D3D12RenderTarget* target, uint32_t ordinal, bool after) {
  if (!scene_update_capture_ || !target || !ordinal || ordinal > scene_alias_budget_.pairs ||
      !command_processor_.IsSubmissionOpen()) return false;
  const auto& pair = scene_alias_pairs_[ordinal - 1];
  const auto& context = *scene_update_capture_;
  PendingSceneAliasReadback pending;
  auto* device = command_processor_.GetD3D12Provider().GetDevice();
  if (!pending.copy.Create(device, target->resource(), 0, (pair.start - 768) / 16 * 8,
      1280, (pair.end - pair.start) / 16 * 8,
      embedded_scene_alias_capture_policy::Budget::CopyBytes(pair.start, pair.end),
      {diagnostic_color_samples_single, sizeof(diagnostic_color_samples_single)},
      {diagnostic_color_samples_msaa, sizeof(diagnostic_color_samples_msaa)})) return false;
  ui::d3d12::util::DescriptorCpuGpuHandlePair descriptor;
  if (!command_processor_.RequestOneUseSingleViewDescriptors(1, &descriptor)) return false;
  pending.copy.WriteSRV(device, descriptor.first);
  pending.frame = context.frame; pending.update = context.update; pending.draw = context.next_draw;
  pending.pair = ordinal; pending.after = after;
  pending.path = "rex_scene_alias_frame_" + std::to_string(context.frame) + "_pair_" +
      std::to_string(ordinal) + (after ? "_after_restore.bin" : "_before_alias.bin");
  pending.submission = command_processor_.GetCurrentSubmission();
  command_processor_.SubmitBarriers();
  // Invalidate the cached guest pipeline. This runs before guest binding, and
  // never changes a graphics root signature, viewport, scissor or attachment.
  command_processor_.SetExternalPipeline(pending.copy.pipeline.Get());
  pending.copy.Enqueue(command_processor_.GetDeferredCommandList(), target->resource_state(), descriptor.second);
  std::fprintf(stderr,
      "REX_SCENE_ALIAS_QUEUED frame=%llu update=%llu next_draw=%llu pair=%u stage=%s color=%p "
      "rect=%u,%u,%u,%u host_width=%llu host_height=%u format=%u samples=%u bytes=%llu submission=%llu path=%s\n",
      static_cast<unsigned long long>(pending.frame), static_cast<unsigned long long>(pending.update),
      static_cast<unsigned long long>(pending.draw), ordinal, after ? "after_restore" : "before_alias",
      static_cast<void*>(target->resource()), pending.copy.left, pending.copy.top,
      pending.copy.width, pending.copy.height, static_cast<unsigned long long>(pending.copy.source_desc.Width),
      pending.copy.source_desc.Height, uint32_t(pending.copy.source_desc.Format), pending.copy.samples,
      static_cast<unsigned long long>(pending.copy.bytes), static_cast<unsigned long long>(pending.submission),
      pending.path.c_str());
  pending_scene_alias_readbacks_.push_back(std::move(pending));
  return true;
}

void D3D12RenderTargetCache::CompleteSceneAliasReadbacks() {
  const uint64_t completed = command_processor_.GetCompletedSubmission();
  while (!pending_scene_alias_readbacks_.empty() && pending_scene_alias_readbacks_.front().submission <= completed) {
    const auto& pending = pending_scene_alias_readbacks_.front();
    D3D12_RANGE range{0, SIZE_T(pending.copy.bytes)};
    void* data = nullptr;
    size_t written = 0;
    uint32_t hash = 2166136261u;
    if (SUCCEEDED(pending.copy.readback->Map(0, &range, &data))) {
      const auto* bytes = static_cast<const uint8_t*>(data);
      for (uint64_t i = 0; i < pending.copy.bytes; ++i) hash = (hash ^ bytes[i]) * 16777619u;
      if (FILE* file = std::fopen(pending.path.c_str(), "wb")) {
        written = std::fwrite(data, 1, size_t(pending.copy.bytes), file);
        if (std::fclose(file) != 0) written = 0;
      }
      D3D12_RANGE no_write{0, 0}; pending.copy.readback->Unmap(0, &no_write);
    }
    std::fprintf(stderr,
        "REX_SCENE_ALIAS_COMPLETE frame=%llu update=%llu next_draw=%llu pair=%u stage=%s result=%u "
        "bytes=%llu written=%llu submission=%llu completed=%llu hash=%08X path=%s\n",
        static_cast<unsigned long long>(pending.frame), static_cast<unsigned long long>(pending.update),
        static_cast<unsigned long long>(pending.draw), pending.pair, pending.after ? "after_restore" : "before_alias",
        written == pending.copy.bytes ? 1u : 0u, static_cast<unsigned long long>(pending.copy.bytes),
        static_cast<unsigned long long>(written), static_cast<unsigned long long>(pending.submission),
        static_cast<unsigned long long>(completed), hash, pending.path.c_str());
    std::fflush(stderr);
    pending_scene_alias_readbacks_.pop_front();
  }
}

void D3D12RenderTargetCache::CompleteColorTargetReadbacks() {
  CompleteSceneAliasReadbacks();
  CompleteDepthSourceReadbacks();
  const uint64_t completed = command_processor_.GetCompletedSubmission();
  while (!pending_color_target_readbacks_.empty() &&
         pending_color_target_readbacks_.front().submission <= completed) {
    const auto& pending = pending_color_target_readbacks_.front();
    const auto& copy = pending.copy;
    D3D12_RANGE range = {0, SIZE_T(copy.buffer_bytes)};
    void* mapping = nullptr;
    size_t written = 0;
    uint32_t hash = 2166136261u;
    const bool mapped = SUCCEEDED(copy.buffer->Map(0, &range, &mapping));
    if (mapped) {
      FILE* file = std::fopen(pending.path.c_str(), "wb");
      for (UINT row = 0; row < copy.rows; ++row) {
        const uint8_t* data = static_cast<const uint8_t*>(mapping) +
            copy.footprint.Offset + size_t(row) * copy.footprint.Footprint.RowPitch;
        for (UINT64 i = 0; i < copy.row_bytes; ++i) hash = (hash ^ data[i]) * 16777619u;
        if (file) written += std::fwrite(data, 1, size_t(copy.row_bytes), file);
      }
      if (file && std::fclose(file) != 0) written = 0;
      D3D12_RANGE no_write = {0, 0};
      copy.buffer->Unmap(0, &no_write);
    }
    std::fprintf(stderr,
        "REX_EMBEDDED_RTV_CAPTURE label=%s dump=%s dumped=%llu logical_bytes=%llu "
        "deferred=1 submission=%llu completed=%llu\n",
        pending.label.c_str(), pending.path.c_str(),
        static_cast<unsigned long long>(written),
        static_cast<unsigned long long>(copy.row_bytes * copy.rows),
        static_cast<unsigned long long>(pending.submission),
        static_cast<unsigned long long>(completed));
    std::fprintf(stderr,
        "REX_EMBEDDED_PROMPT_RTV_READBACK result=%s resource=%p "
        "size=%llux%u format=%u source_samples=%u row_pitch=%u "
        "row_bytes=%llu rows=%u bytes=%llu fnv1a=0x%08X\n",
        mapped && written == copy.row_bytes * copy.rows ? "ok" : "failed",
        static_cast<void*>(copy.source.Get()),
        static_cast<unsigned long long>(copy.source_desc.Width), copy.source_desc.Height,
        uint32_t(copy.source_desc.Format), copy.source_desc.SampleDesc.Count,
        copy.footprint.Footprint.RowPitch, static_cast<unsigned long long>(copy.row_bytes),
        copy.rows, static_cast<unsigned long long>(copy.buffer_bytes), hash);
    std::fflush(stderr);
    pending_color_target_readbacks_.pop_front();
  }
}

bool D3D12RenderTargetCache::CaptureEmbeddedSceneDepthTarget(
    const char* diagnostic_label, const char* dump_path) {
  if (path_ != Path::kHostRenderTargets) {
    std::fprintf(stderr,
                 "REX_EMBEDDED_SCENE_DEPTH_READBACK result=skipped path=rov\n");
    std::fflush(stderr);
    return false;
  }

  RenderTarget* const* accumulated = last_update_accumulated_render_targets();
  auto* render_target = static_cast<D3D12RenderTarget*>(accumulated[0]);
  if (!render_target) {
    std::fprintf(stderr,
                 "REX_EMBEDDED_SCENE_DEPTH_READBACK result=missing_depth\n");
    std::fflush(stderr);
    return false;
  }
  const RenderTargetKey key = render_target->key();
  const uint32_t scale_x = GetKeyScaleX(key);
  const uint32_t scale_y = GetKeyScaleY(key);
  if (!key.is_depth || key.base_tiles != 0 ||
      key.pitch_tiles_at_32bpp != 16 ||
      key.msaa_samples != xenos::MsaaSamples::k2X ||
      key.GetDepthFormat() != xenos::DepthRenderTargetFormat::kD24FS8 ||
      scale_x > 2 || scale_y > 2) {
    std::fprintf(
        stderr,
        "REX_EMBEDDED_SCENE_DEPTH_READBACK result=unexpected_target "
        "key=0x%08X scale=%ux%u\n",
        key.key, scale_x, scale_y);
    std::fflush(stderr);
    return false;
  }

  // The dynamically observed ownership span is 0..768 tiles, represented as
  // 48 rows at the target's 16-tile pitch (1280x384 guest samples). Dumping the
  // host DSV through the ordinary packed-EDRAM shader preserves both float24
  // depth and stencil in the canonical scaled sample layout.
  constexpr uint32_t kDumpPitchTiles = 16;
  constexpr uint32_t kDumpRows = 48;
  constexpr uint32_t kDumpTileCount = kDumpPitchTiles * kDumpRows;
  if (!DumpRenderTargets(0, kDumpPitchTiles, kDumpRows,
                         kDumpPitchTiles)) {
    std::fprintf(stderr,
                 "REX_EMBEDDED_SCENE_DEPTH_READBACK result=dump_failed\n");
    std::fflush(stderr);
    return false;
  }

  const auto restore_render_targets = [&]() {
    are_current_command_list_render_targets_valid_ = false;
    SetCommandListRenderTargets(last_update_accumulated_render_targets());
  };
  constexpr uint64_t kBytesPerNativeTile =
      xenos::kEdramSizeBytes / xenos::kEdramTileCount;
  const uint64_t readback_size =
      uint64_t(kDumpTileCount) * kBytesPerNativeTile * scale_x * scale_y;
  D3D12_RESOURCE_DESC readback_desc;
  ui::d3d12::util::FillBufferResourceDesc(
      readback_desc, readback_size, D3D12_RESOURCE_FLAG_NONE);
  const ui::d3d12::D3D12Provider& provider =
      command_processor_.GetD3D12Provider();
  Microsoft::WRL::ComPtr<ID3D12Resource> readback;
  if (FAILED(provider.GetDevice()->CreateCommittedResource(
          &ui::d3d12::util::kHeapPropertiesReadback,
          provider.GetHeapFlagCreateNotZeroed(), &readback_desc,
          D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
          IID_PPV_ARGS(&readback)))) {
    restore_render_targets();
    std::fprintf(
        stderr,
        "REX_EMBEDDED_SCENE_DEPTH_READBACK result=create_failed bytes=%llu\n",
        static_cast<unsigned long long>(readback_size));
    std::fflush(stderr);
    return false;
  }

  TransitionEdramBuffer(D3D12_RESOURCE_STATE_COPY_SOURCE);
  command_processor_.SubmitBarriers();
  command_processor_.GetDeferredCommandList().D3DCopyBufferRegion(
      readback.Get(), 0, edram_buffer_, 0, readback_size);
  if (!command_processor_.AwaitAllQueueOperationsCompletion()) {
    restore_render_targets();
    std::fprintf(stderr,
                 "REX_EMBEDDED_SCENE_DEPTH_READBACK result=await_failed\n");
    std::fflush(stderr);
    return false;
  }

  D3D12_RANGE read_range = {0, SIZE_T(readback_size)};
  void* mapping = nullptr;
  if (FAILED(readback->Map(0, &read_range, &mapping))) {
    restore_render_targets();
    std::fprintf(stderr,
                 "REX_EMBEDDED_SCENE_DEPTH_READBACK result=map_failed\n");
    std::fflush(stderr);
    return false;
  }

  const uint32_t* samples = static_cast<const uint32_t*>(mapping);
  FILE* dump_file = dump_path ? std::fopen(dump_path, "wb") : nullptr;
  const size_t dumped_bytes = dump_file
                                  ? std::fwrite(mapping, 1, size_t(readback_size),
                                                dump_file)
                                  : 0;
  if (dump_file) {
    std::fclose(dump_file);
  }

  const uint32_t tile_width = xenos::kEdramTileWidthSamples * scale_x;
  const uint32_t tile_height = xenos::kEdramTileHeightSamples * scale_y;
  const uint32_t tile_samples = tile_width * tile_height;
  uint64_t group_count = 0;
  uint64_t exact_group_count = 0;
  uint64_t mismatch_group_count = 0;
  uint64_t parity_stencil_nonzero[4] = {};
  uint64_t parity_stencil_80[4] = {};
  uint64_t parity_stencil_histogram[4][256] = {};
  uint64_t parity_depth_nonzero[4] = {};
  uint64_t parity_depth_nonzero_stencil_7f[4] = {};
  uint64_t parity_depth_nonzero_stencil_80[4] = {};
  uint32_t parity_hash[4] = {2166136261u, 2166136261u, 2166136261u,
                             2166136261u};
  uint32_t parity_depth_hash[4] = {2166136261u, 2166136261u,
                                   2166136261u, 2166136261u};
  for (uint32_t tile = 0; tile < kDumpTileCount; ++tile) {
    const uint32_t tile_base = tile * tile_samples;
    for (uint32_t guest_y = 0;
         guest_y < xenos::kEdramTileHeightSamples; ++guest_y) {
      for (uint32_t guest_x = 0;
           guest_x < xenos::kEdramTileWidthSamples; ++guest_x) {
        uint32_t first_value = 0;
        bool exact = true;
        for (uint32_t parity_y = 0; parity_y < scale_y; ++parity_y) {
          for (uint32_t parity_x = 0; parity_x < scale_x; ++parity_x) {
            const uint32_t parity = parity_y * scale_x + parity_x;
            const uint32_t scaled_x = guest_x * scale_x + parity_x;
            const uint32_t scaled_y = guest_y * scale_y + parity_y;
            const int32_t depth_half_swap =
                scaled_x >= tile_width / 2 ? -int32_t(tile_width / 2)
                                           : int32_t(tile_width / 2);
            const uint32_t sample_index =
                tile_base + scaled_y * tile_width + scaled_x +
                depth_half_swap;
            const uint32_t value = samples[sample_index];
            if (!parity) {
              first_value = value;
            } else if (value != first_value) {
              exact = false;
            }
            const uint32_t stencil = value & 0xFF;
            parity_stencil_nonzero[parity] += stencil != 0;
            parity_stencil_80[parity] += stencil == 0x80;
            ++parity_stencil_histogram[parity][stencil];
            const uint32_t depth = value >> 8;
            const bool depth_nonzero = depth != 0;
            parity_depth_nonzero[parity] += depth_nonzero;
            parity_depth_nonzero_stencil_7f[parity] +=
                depth_nonzero && stencil == 0x7F;
            parity_depth_nonzero_stencil_80[parity] +=
                depth_nonzero && stencil == 0x80;
            for (uint32_t byte_index = 0; byte_index < 4; ++byte_index) {
              parity_hash[parity] =
                  (parity_hash[parity] ^
                   uint8_t(value >> (byte_index * 8))) *
                  16777619u;
            }
            // Hash only the 24 stored depth bits. This distinguishes a real
            // depth mutation from the expected stencil-only changes between
            // the prepass/control and shaded draws.
            for (uint32_t byte_index = 0; byte_index < 3; ++byte_index) {
              parity_depth_hash[parity] =
                  (parity_depth_hash[parity] ^
                   uint8_t(depth >> (byte_index * 8))) *
                  16777619u;
            }
          }
        }
        ++group_count;
        if (exact) {
          ++exact_group_count;
        } else {
          ++mismatch_group_count;
        }
      }
    }
  }

  D3D12_RANGE write_range = {0, 0};
  readback->Unmap(0, &write_range);
  restore_render_targets();
  std::fprintf(
      stderr,
      "REX_EMBEDDED_SCENE_DEPTH_READBACK result=ok label=%s dump=%s "
      "bytes=%llu dumped=%llu key=0x%08X scale=%ux%u groups=%llu "
      "exact=%llu mismatch=%llu hash=%08X,%08X,%08X,%08X "
      "depth_hash=%08X,%08X,%08X,%08X "
      "stencil_nonzero=%llu,%llu,%llu,%llu "
      "stencil_80=%llu,%llu,%llu,%llu "
      "stencil_7B=%llu,%llu,%llu,%llu "
      "stencil_7C=%llu,%llu,%llu,%llu "
      "stencil_7D=%llu,%llu,%llu,%llu "
      "stencil_7E=%llu,%llu,%llu,%llu "
      "stencil_7F=%llu,%llu,%llu,%llu "
      "stencil_81=%llu,%llu,%llu,%llu "
      "depth_nonzero=%llu,%llu,%llu,%llu "
      "depth_nonzero_stencil_7F=%llu,%llu,%llu,%llu "
      "depth_nonzero_stencil_80=%llu,%llu,%llu,%llu\n",
      diagnostic_label ? diagnostic_label : "unnamed",
      dump_path ? dump_path : "disabled",
      static_cast<unsigned long long>(readback_size),
      static_cast<unsigned long long>(dumped_bytes), key.key, scale_x, scale_y,
      static_cast<unsigned long long>(group_count),
      static_cast<unsigned long long>(exact_group_count),
      static_cast<unsigned long long>(mismatch_group_count), parity_hash[0],
      parity_hash[1], parity_hash[2], parity_hash[3],
      parity_depth_hash[0], parity_depth_hash[1], parity_depth_hash[2],
      parity_depth_hash[3],
      static_cast<unsigned long long>(parity_stencil_nonzero[0]),
      static_cast<unsigned long long>(parity_stencil_nonzero[1]),
      static_cast<unsigned long long>(parity_stencil_nonzero[2]),
      static_cast<unsigned long long>(parity_stencil_nonzero[3]),
      static_cast<unsigned long long>(parity_stencil_80[0]),
      static_cast<unsigned long long>(parity_stencil_80[1]),
      static_cast<unsigned long long>(parity_stencil_80[2]),
      static_cast<unsigned long long>(parity_stencil_80[3]),
      static_cast<unsigned long long>(parity_stencil_histogram[0][0x7B]),
      static_cast<unsigned long long>(parity_stencil_histogram[1][0x7B]),
      static_cast<unsigned long long>(parity_stencil_histogram[2][0x7B]),
      static_cast<unsigned long long>(parity_stencil_histogram[3][0x7B]),
      static_cast<unsigned long long>(parity_stencil_histogram[0][0x7C]),
      static_cast<unsigned long long>(parity_stencil_histogram[1][0x7C]),
      static_cast<unsigned long long>(parity_stencil_histogram[2][0x7C]),
      static_cast<unsigned long long>(parity_stencil_histogram[3][0x7C]),
      static_cast<unsigned long long>(parity_stencil_histogram[0][0x7D]),
      static_cast<unsigned long long>(parity_stencil_histogram[1][0x7D]),
      static_cast<unsigned long long>(parity_stencil_histogram[2][0x7D]),
      static_cast<unsigned long long>(parity_stencil_histogram[3][0x7D]),
      static_cast<unsigned long long>(parity_stencil_histogram[0][0x7E]),
      static_cast<unsigned long long>(parity_stencil_histogram[1][0x7E]),
      static_cast<unsigned long long>(parity_stencil_histogram[2][0x7E]),
      static_cast<unsigned long long>(parity_stencil_histogram[3][0x7E]),
      static_cast<unsigned long long>(parity_stencil_histogram[0][0x7F]),
      static_cast<unsigned long long>(parity_stencil_histogram[1][0x7F]),
      static_cast<unsigned long long>(parity_stencil_histogram[2][0x7F]),
      static_cast<unsigned long long>(parity_stencil_histogram[3][0x7F]),
      static_cast<unsigned long long>(parity_stencil_histogram[0][0x81]),
      static_cast<unsigned long long>(parity_stencil_histogram[1][0x81]),
      static_cast<unsigned long long>(parity_stencil_histogram[2][0x81]),
      static_cast<unsigned long long>(parity_stencil_histogram[3][0x81]),
      static_cast<unsigned long long>(parity_depth_nonzero[0]),
      static_cast<unsigned long long>(parity_depth_nonzero[1]),
      static_cast<unsigned long long>(parity_depth_nonzero[2]),
      static_cast<unsigned long long>(parity_depth_nonzero[3]),
      static_cast<unsigned long long>(parity_depth_nonzero_stencil_7f[0]),
      static_cast<unsigned long long>(parity_depth_nonzero_stencil_7f[1]),
      static_cast<unsigned long long>(parity_depth_nonzero_stencil_7f[2]),
      static_cast<unsigned long long>(parity_depth_nonzero_stencil_7f[3]),
      static_cast<unsigned long long>(parity_depth_nonzero_stencil_80[0]),
      static_cast<unsigned long long>(parity_depth_nonzero_stencil_80[1]),
      static_cast<unsigned long long>(parity_depth_nonzero_stencil_80[2]),
      static_cast<unsigned long long>(parity_depth_nonzero_stencil_80[3]));
  std::fflush(stderr);
  return true;
}

bool D3D12RenderTargetCache::CaptureEmbeddedColorTarget(
    D3D12RenderTarget* render_target, const char* diagnostic_label,
    const char* dump_path,
    EmbeddedColorTargetDiagnosticSummary* summary_out) {
  assert_not_null(render_target);

  ID3D12Resource* resource = render_target->resource();
  const D3D12_RESOURCE_DESC resource_desc = resource->GetDesc();
  if (resource_desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D) {
    std::fprintf(stderr,
                 "REX_EMBEDDED_PROMPT_RTV_READBACK result=unsupported dimension=%u\n",
                 uint32_t(resource_desc.Dimension));
    std::fflush(stderr);
    return false;
  }

  const ui::d3d12::D3D12Provider& provider = command_processor_.GetD3D12Provider();
  ID3D12Device* device = provider.GetDevice();
  D3D12_RESOURCE_DESC copy_resource_desc = resource_desc;
  Microsoft::WRL::ComPtr<ID3D12Resource> resolved_resource;
  ID3D12Resource* copy_resource = resource;
  if (resource_desc.SampleDesc.Count > 1) {
    copy_resource_desc.Alignment = 0;
    copy_resource_desc.SampleDesc.Count = 1;
    copy_resource_desc.SampleDesc.Quality = 0;
    copy_resource_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    const HRESULT create_resolve_target_result = device->CreateCommittedResource(
        &ui::d3d12::util::kHeapPropertiesDefault, D3D12_HEAP_FLAG_NONE,
        &copy_resource_desc, D3D12_RESOURCE_STATE_RESOLVE_DEST, nullptr,
        IID_PPV_ARGS(&resolved_resource));
    if (FAILED(create_resolve_target_result)) {
      std::fprintf(stderr,
                   "REX_EMBEDDED_PROMPT_RTV_READBACK result=resolve_target_failed "
                   "hr=0x%08X size=%llux%u format=%u samples=%u flags=0x%X\n",
                   uint32_t(create_resolve_target_result),
                   static_cast<unsigned long long>(copy_resource_desc.Width),
                   copy_resource_desc.Height, uint32_t(copy_resource_desc.Format),
                   resource_desc.SampleDesc.Count, uint32_t(copy_resource_desc.Flags));
      std::fflush(stderr);
      return false;
    }
    copy_resource = resolved_resource.Get();
  }

  D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint;
  UINT footprint_rows = 0;
  UINT64 footprint_row_size = 0;
  UINT64 readback_size = 0;
  device->GetCopyableFootprints(&copy_resource_desc, 0, 1, 0, &footprint,
                                &footprint_rows, &footprint_row_size,
                                &readback_size);

  D3D12_RESOURCE_DESC readback_desc;
  ui::d3d12::util::FillBufferResourceDesc(readback_desc, readback_size,
                                          D3D12_RESOURCE_FLAG_NONE);
  Microsoft::WRL::ComPtr<ID3D12Resource> readback;
  if (FAILED(device->CreateCommittedResource(
          &ui::d3d12::util::kHeapPropertiesReadback,
          provider.GetHeapFlagCreateNotZeroed(), &readback_desc,
          D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
          IID_PPV_ARGS(&readback)))) {
    std::fprintf(stderr,
                 "REX_EMBEDDED_PROMPT_RTV_READBACK result=create_failed bytes=%llu\n",
                 static_cast<unsigned long long>(readback_size));
    std::fflush(stderr);
    return false;
  }

  const D3D12_RESOURCE_STATES old_state = render_target->SetResourceState(
      resource_desc.SampleDesc.Count > 1 ? D3D12_RESOURCE_STATE_RESOLVE_SOURCE
                                         : D3D12_RESOURCE_STATE_COPY_SOURCE);
  command_processor_.PushTransitionBarrier(
      resource, old_state,
      resource_desc.SampleDesc.Count > 1 ? D3D12_RESOURCE_STATE_RESOLVE_SOURCE
                                         : D3D12_RESOURCE_STATE_COPY_SOURCE);
  command_processor_.SubmitBarriers();
  if (resource_desc.SampleDesc.Count > 1) {
    command_processor_.GetDeferredCommandList().D3DResolveSubresource(
        resolved_resource.Get(), 0, resource, 0, resource_desc.Format);
    command_processor_.PushTransitionBarrier(
        resolved_resource.Get(), D3D12_RESOURCE_STATE_RESOLVE_DEST,
        D3D12_RESOURCE_STATE_COPY_SOURCE);
    command_processor_.PushTransitionBarrier(
        resource, render_target->SetResourceState(old_state), old_state);
    command_processor_.SubmitBarriers();
  }
  D3D12_TEXTURE_COPY_LOCATION source_location = {};
  source_location.pResource = copy_resource;
  source_location.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  source_location.SubresourceIndex = 0;
  D3D12_TEXTURE_COPY_LOCATION dest_location = {};
  dest_location.pResource = readback.Get();
  dest_location.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
  dest_location.PlacedFootprint = footprint;
  command_processor_.GetDeferredCommandList().D3DCopyTextureRegion(
      &dest_location, 0, 0, 0, &source_location, nullptr);
  if (resource_desc.SampleDesc.Count == 1) {
    command_processor_.PushTransitionBarrier(
        resource, render_target->SetResourceState(old_state), old_state);
    command_processor_.SubmitBarriers();
  }
  if (!command_processor_.AwaitAllQueueOperationsCompletion()) {
    std::fprintf(stderr,
                 "REX_EMBEDDED_PROMPT_RTV_READBACK result=await_failed\n");
    std::fflush(stderr);
    return false;
  }

  D3D12_RANGE read_range = {0, SIZE_T(readback_size)};
  void* mapping = nullptr;
  if (FAILED(readback->Map(0, &read_range, &mapping))) {
    std::fprintf(stderr,
                 "REX_EMBEDDED_PROMPT_RTV_READBACK result=map_failed\n");
    std::fflush(stderr);
    return false;
  }

  const uint8_t* bytes = static_cast<const uint8_t*>(mapping) + footprint.Offset;
  uint32_t hash = 2166136261u;
  const size_t logical_row_bytes = size_t(footprint_row_size);
  FILE* dump_file = dump_path ? std::fopen(dump_path, "wb") : nullptr;
  size_t dumped_bytes = 0;
  for (UINT row = 0; row < footprint_rows; ++row) {
    const uint8_t* row_bytes = bytes + size_t(row) * footprint.Footprint.RowPitch;
    for (size_t i = 0; i < logical_row_bytes; ++i) {
      hash = (hash ^ row_bytes[i]) * 16777619u;
    }
    if (dump_file) {
      dumped_bytes += std::fwrite(row_bytes, 1, logical_row_bytes, dump_file);
    }
  }
  if (dump_file) {
    std::fclose(dump_file);
  }

  uint64_t nonzero_pixels = 0;
  uint64_t nonzero_rgb_pixels = 0;
  uint64_t nonzero_alpha_pixels = 0;
  uint64_t beige_like_pixels = 0;
  uint32_t nonzero_min_x = uint32_t(copy_resource_desc.Width);
  uint32_t nonzero_min_y = copy_resource_desc.Height;
  uint32_t nonzero_max_x = 0;
  uint32_t nonzero_max_y = 0;
  float component_min[4] = {1.0e30f, 1.0e30f, 1.0e30f, 1.0e30f};
  float component_max[4] = {-1.0e30f, -1.0e30f, -1.0e30f, -1.0e30f};
  uint32_t first_nonzero_logged = 0;
  // Eight distinct raw FP16 pixel values are sufficient to reject the common
  // zero-plus-clear initialization surfaces without retaining a large set or
  // introducing an unbounded diagnostic allocation.
  std::array<uint64_t, 8> unique_pixel_values = {};
  uint32_t unique_pixel_value_count = 0;
  for (uint32_t y = 0; y < copy_resource_desc.Height; ++y) {
    const uint8_t* row_bytes = bytes + size_t(y) * footprint.Footprint.RowPitch;
    for (uint32_t x = 0; x < copy_resource_desc.Width; ++x) {
      const uint16_t* pixel =
          reinterpret_cast<const uint16_t*>(row_bytes + size_t(x) * 8);
      uint64_t raw_pixel = 0;
      std::memcpy(&raw_pixel, pixel, sizeof(raw_pixel));
      bool raw_pixel_seen = false;
      for (uint32_t unique_index = 0;
           unique_index < unique_pixel_value_count; ++unique_index) {
        if (unique_pixel_values[unique_index] == raw_pixel) {
          raw_pixel_seen = true;
          break;
        }
      }
      if (!raw_pixel_seen &&
          unique_pixel_value_count < unique_pixel_values.size()) {
        unique_pixel_values[unique_pixel_value_count++] = raw_pixel;
      }
      const bool rgb_nonzero = pixel[0] || pixel[1] || pixel[2];
      const bool alpha_nonzero = pixel[3] != 0;
      if (rgb_nonzero) {
        ++nonzero_rgb_pixels;
      }
      if (alpha_nonzero) {
        ++nonzero_alpha_pixels;
      }
      if (!rgb_nonzero && !alpha_nonzero) {
        continue;
      }
      ++nonzero_pixels;
      nonzero_min_x = std::min(nonzero_min_x, x);
      nonzero_min_y = std::min(nonzero_min_y, y);
      nonzero_max_x = std::max(nonzero_max_x, x);
      nonzero_max_y = std::max(nonzero_max_y, y);
      float values[4];
      for (uint32_t component = 0; component < 4; ++component) {
        values[component] = rex::xenos_half_to_float(pixel[component]);
        component_min[component] =
            std::min(component_min[component], values[component]);
        component_max[component] =
            std::max(component_max[component], values[component]);
      }
      if (values[0] > 0.8f && values[1] > 0.8f && values[2] > 0.7f) {
        ++beige_like_pixels;
      }
      if (first_nonzero_logged < 8) {
        std::fprintf(stderr,
                     "REX_EMBEDDED_PROMPT_RTV_NONZERO ordinal=%u xy=%u,%u "
                     "raw=%04X,%04X,%04X,%04X value=%.9g,%.9g,%.9g,%.9g\n",
                     first_nonzero_logged, x, y, pixel[0], pixel[1], pixel[2],
                     pixel[3], values[0], values[1], values[2], values[3]);
        ++first_nonzero_logged;
      }
    }
  }

  const auto log_pixel = [&](const char* name, uint32_t x, uint32_t y) {
    x = std::min(x, uint32_t(copy_resource_desc.Width - 1));
    y = std::min(y, uint32_t(copy_resource_desc.Height - 1));
    const uint16_t* pixel = reinterpret_cast<const uint16_t*>(
        bytes + size_t(y) * footprint.Footprint.RowPitch + size_t(x) * 8);
    std::fprintf(stderr,
                 "REX_EMBEDDED_PROMPT_RTV_PIXEL name=%s xy=%u,%u "
                 "raw=%04X,%04X,%04X,%04X value=%.9g,%.9g,%.9g,%.9g\n",
                 name, x, y, pixel[0], pixel[1], pixel[2], pixel[3],
                 rex::xenos_half_to_float(pixel[0]),
                 rex::xenos_half_to_float(pixel[1]),
                 rex::xenos_half_to_float(pixel[2]),
                 rex::xenos_half_to_float(pixel[3]));
  };
  std::fprintf(stderr,
               "REX_EMBEDDED_RTV_CAPTURE label=%s dump=%s dumped=%llu "
               "logical_bytes=%llu\n",
               diagnostic_label ? diagnostic_label : "unnamed",
               dump_path ? dump_path : "disabled",
               static_cast<unsigned long long>(dumped_bytes),
               static_cast<unsigned long long>(logical_row_bytes) *
                   footprint_rows);
  std::fprintf(stderr,
               "REX_EMBEDDED_PROMPT_RTV_READBACK result=ok resource=%p "
               "size=%llux%u format=%u source_samples=%u row_pitch=%u "
               "row_bytes=%llu rows=%u bytes=%llu fnv1a=0x%08X\n",
               static_cast<void*>(resource),
               static_cast<unsigned long long>(copy_resource_desc.Width),
               copy_resource_desc.Height, uint32_t(copy_resource_desc.Format),
               resource_desc.SampleDesc.Count,
               footprint.Footprint.RowPitch,
               static_cast<unsigned long long>(footprint_row_size), footprint_rows,
               static_cast<unsigned long long>(readback_size), hash);
  std::fprintf(
      stderr,
      "REX_EMBEDDED_PROMPT_RTV_SUMMARY pixels=%llu nonzero=%llu rgb=%llu alpha=%llu "
      "beige_like=%llu bounds=%u,%u-%u,%u min=%.9g,%.9g,%.9g,%.9g "
      "max=%.9g,%.9g,%.9g,%.9g\n",
      static_cast<unsigned long long>(copy_resource_desc.Width) *
          copy_resource_desc.Height,
      static_cast<unsigned long long>(nonzero_pixels),
      static_cast<unsigned long long>(nonzero_rgb_pixels),
      static_cast<unsigned long long>(nonzero_alpha_pixels),
      static_cast<unsigned long long>(beige_like_pixels), nonzero_min_x,
      nonzero_min_y, nonzero_max_x, nonzero_max_y, component_min[0],
      component_min[1], component_min[2], component_min[3], component_max[0],
      component_max[1], component_max[2], component_max[3]);
  log_pixel("top_left", 0, 0);
  log_pixel("quarter", uint32_t(copy_resource_desc.Width / 4),
            uint32_t(copy_resource_desc.Height / 4));
  log_pixel("center", uint32_t(copy_resource_desc.Width / 2),
            uint32_t(copy_resource_desc.Height / 2));
  log_pixel("prompt_center", uint32_t(copy_resource_desc.Width / 2), 600);
  std::fflush(stderr);

  D3D12_RANGE write_range = {0, 0};
  readback->Unmap(0, &write_range);
  if (summary_out) {
    summary_out->fnv1a = hash;
    summary_out->unique_pixel_values_capped = unique_pixel_value_count;
    summary_out->width = uint32_t(copy_resource_desc.Width);
    summary_out->height = copy_resource_desc.Height;
    summary_out->source_samples = resource_desc.SampleDesc.Count;
  }
  return !dump_path || dumped_bytes == logical_row_bytes * footprint_rows;
}

bool D3D12RenderTargetCache::CaptureEmbeddedResolveSource(
    const draw_util::ResolveInfo& resolve_info, const char* diagnostic_label,
    const char* dump_path) {
  if (GetPath() != Path::kHostRenderTargets) {
    std::fprintf(stderr,
                 "REX_EMBEDDED_RESOLVE_SOURCE result=skipped label=%s path=rov\n",
                 diagnostic_label ? diagnostic_label : "unnamed");
    std::fflush(stderr);
    return false;
  }

  uint32_t dump_base;
  uint32_t dump_row_length_used;
  uint32_t dump_rows;
  uint32_t dump_pitch;
  resolve_info.GetCopyEdramTileSpan(dump_base, dump_row_length_used,
                                    dump_rows, dump_pitch);
  std::vector<ResolveCopyDumpRectangle> rectangles;
  GetResolveCopyRectanglesToDump(dump_base, dump_row_length_used, dump_rows,
                                 dump_pitch, rectangles);
  if (rectangles.empty()) {
    std::fprintf(
        stderr,
        "REX_EMBEDDED_RESOLVE_SOURCE result=missing_owner label=%s "
        "base=%u row_length=%u rows=%u pitch=%u\n",
        diagnostic_label ? diagnostic_label : "unnamed", dump_base,
        dump_row_length_used, dump_rows, dump_pitch);
    std::fflush(stderr);
    return false;
  }

  std::vector<D3D12RenderTarget*> unique_render_targets;
  unique_render_targets.reserve(rectangles.size());
  for (size_t rectangle_index = 0; rectangle_index < rectangles.size();
       ++rectangle_index) {
    const ResolveCopyDumpRectangle& rectangle = rectangles[rectangle_index];
    auto* render_target =
        static_cast<D3D12RenderTarget*>(rectangle.render_target);
    if (!render_target) {
      continue;
    }
    const RenderTargetKey key = render_target->key();
    std::fprintf(
        stderr,
        "REX_EMBEDDED_RESOLVE_SOURCE_RECT label=%s rectangle=%llu "
        "resource=%p key=0x%08X base=%u pitch32=%u pitch=%u msaa=%u "
        "depth=%u format=%u scale_native=%u scale=%ux%u "
        "row_first=%u rows=%u first_start=%u last_end=%u\n",
        diagnostic_label ? diagnostic_label : "unnamed",
        static_cast<unsigned long long>(rectangle_index),
        static_cast<void*>(render_target->resource()), key.key, key.base_tiles,
        key.pitch_tiles_at_32bpp, key.GetPitchTiles(),
        uint32_t(key.msaa_samples), key.is_depth, key.resource_format,
        key.scale_native, GetKeyScaleX(key), GetKeyScaleY(key),
        rectangle.row_first, rectangle.rows, rectangle.row_first_start,
        rectangle.row_last_end);
    if (std::find(unique_render_targets.begin(), unique_render_targets.end(),
                  render_target) == unique_render_targets.end()) {
      unique_render_targets.push_back(render_target);
    }
  }

  if (unique_render_targets.empty()) {
    std::fprintf(stderr,
                 "REX_EMBEDDED_RESOLVE_SOURCE result=missing_resource label=%s\n",
                 diagnostic_label ? diagnostic_label : "unnamed");
    std::fflush(stderr);
    return false;
  }

  bool all_captured = true;
  for (size_t target_index = 0; target_index < unique_render_targets.size();
       ++target_index) {
    std::string indexed_path;
    const char* target_dump_path = dump_path;
    if (dump_path && unique_render_targets.size() > 1) {
      indexed_path = dump_path;
      indexed_path += ".source";
      indexed_path += std::to_string(target_index);
      target_dump_path = indexed_path.c_str();
    }
    const bool captured = CaptureEmbeddedColorTarget(
        unique_render_targets[target_index], diagnostic_label,
        target_dump_path);
    all_captured &= captured;
    std::fprintf(stderr,
                 "REX_EMBEDDED_RESOLVE_SOURCE_TARGET result=%u label=%s "
                 "target=%llu targets=%llu dump=%s\n",
                 captured ? 1u : 0u,
                 diagnostic_label ? diagnostic_label : "unnamed",
                 static_cast<unsigned long long>(target_index),
                 static_cast<unsigned long long>(unique_render_targets.size()),
                 target_dump_path ? target_dump_path : "disabled");
  }
  std::fflush(stderr);
  return all_captured;
}

bool D3D12RenderTargetCache::CaptureEmbeddedEdramBuffer(
    const draw_util::ResolveInfo& resolve_info, const char* diagnostic_label,
    const char* dump_path) {
  if (!dump_path || !edram_buffer_) {
    return false;
  }

  const uint64_t readback_size = edram_buffer_->GetDesc().Width;
  if (!readback_size || readback_size > SIZE_MAX) {
    std::fprintf(stderr,
                 "REX_EMBEDDED_RESOLVE_BOUNDARY result=invalid_size "
                 "stage=edram_after_dump label=%s bytes=%llu\n",
                 diagnostic_label ? diagnostic_label : "unnamed",
                 static_cast<unsigned long long>(readback_size));
    std::fflush(stderr);
    return false;
  }

  D3D12_RESOURCE_DESC readback_desc;
  ui::d3d12::util::FillBufferResourceDesc(
      readback_desc, readback_size, D3D12_RESOURCE_FLAG_NONE);
  const ui::d3d12::D3D12Provider& provider =
      command_processor_.GetD3D12Provider();
  Microsoft::WRL::ComPtr<ID3D12Resource> readback;
  if (FAILED(provider.GetDevice()->CreateCommittedResource(
          &ui::d3d12::util::kHeapPropertiesReadback,
          provider.GetHeapFlagCreateNotZeroed(), &readback_desc,
          D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
          IID_PPV_ARGS(&readback)))) {
    std::fprintf(stderr,
                 "REX_EMBEDDED_RESOLVE_BOUNDARY result=create_failed "
                 "stage=edram_after_dump label=%s bytes=%llu\n",
                 diagnostic_label ? diagnostic_label : "unnamed",
                 static_cast<unsigned long long>(readback_size));
    std::fflush(stderr);
    return false;
  }

  const D3D12_RESOURCE_STATES old_state = edram_buffer_state_;
  TransitionEdramBuffer(D3D12_RESOURCE_STATE_COPY_SOURCE);
  command_processor_.SubmitBarriers();
  command_processor_.GetDeferredCommandList().D3DCopyBufferRegion(
      readback.Get(), 0, edram_buffer_, 0, readback_size);
  TransitionEdramBuffer(old_state);
  command_processor_.SubmitBarriers();
  if (!command_processor_.AwaitAllQueueOperationsCompletion()) {
    std::fprintf(stderr,
                 "REX_EMBEDDED_RESOLVE_BOUNDARY result=await_failed "
                 "stage=edram_after_dump label=%s\n",
                 diagnostic_label ? diagnostic_label : "unnamed");
    std::fflush(stderr);
    return false;
  }

  D3D12_RANGE read_range = {0, SIZE_T(readback_size)};
  void* mapping = nullptr;
  if (FAILED(readback->Map(0, &read_range, &mapping))) {
    std::fprintf(stderr,
                 "REX_EMBEDDED_RESOLVE_BOUNDARY result=map_failed "
                 "stage=edram_after_dump label=%s\n",
                 diagnostic_label ? diagnostic_label : "unnamed");
    std::fflush(stderr);
    return false;
  }

  const uint8_t* bytes = static_cast<const uint8_t*>(mapping);
  uint32_t hash = 2166136261u;
  for (uint64_t i = 0; i < readback_size; ++i) {
    hash = (hash ^ bytes[i]) * 16777619u;
  }
  FILE* dump_file = std::fopen(dump_path, "wb");
  const size_t dumped_bytes =
      dump_file ? std::fwrite(mapping, 1, size_t(readback_size), dump_file) : 0;
  if (dump_file) {
    std::fclose(dump_file);
  }

  uint32_t dump_base = 0;
  uint32_t dump_row_length_used = 0;
  uint32_t dump_rows = 0;
  uint32_t dump_pitch = 0;
  resolve_info.GetCopyEdramTileSpan(dump_base, dump_row_length_used, dump_rows,
                                    dump_pitch);
  std::fprintf(
      stderr,
      "REX_EMBEDDED_RESOLVE_BOUNDARY result=%s stage=edram_after_dump "
      "label=%s dump=%s bytes=%llu fnv1a=0x%08X scale=%ux%u "
      "span_base=%u span_row_length=%u span_rows=%u span_pitch=%u\n",
      dumped_bytes == size_t(readback_size) ? "ok" : "write_failed",
      diagnostic_label ? diagnostic_label : "unnamed", dump_path,
      static_cast<unsigned long long>(readback_size), hash,
      draw_resolution_scale_x(), draw_resolution_scale_y(), dump_base,
      dump_row_length_used, dump_rows, dump_pitch);
  std::fflush(stderr);
  D3D12_RANGE write_range = {0, 0};
  readback->Unmap(0, &write_range);
  return dumped_bytes == size_t(readback_size);
}

void D3D12RenderTargetCache::Shutdown(bool from_destructor) {
  pending_scene_alias_readbacks_.clear();
  scene_alias_pairs_ = {};
  scene_alias_budget_ = {};
  pending_color_target_readbacks_.clear();
  pending_depth_source_readbacks_.clear();
  camera_depth_readback_budget_ = {};
  captured_depth_sources_.clear();
  ui::d3d12::util::ReleaseAndNull(resolve_rov_clear_64bpp_pipeline_);
  ui::d3d12::util::ReleaseAndNull(resolve_rov_clear_32bpp_pipeline_);
  ui::d3d12::util::ReleaseAndNull(resolve_rov_clear_root_signature_);

  for (size_t i = 0; i < 2; ++i) {
    for (size_t j = size_t(xenos::MsaaSamples::k1X); j <= size_t(xenos::MsaaSamples::k4X); ++j) {
      ui::d3d12::util::ReleaseAndNull(transfer_stencil_clear_pipelines_[i][j]);
      ui::d3d12::util::ReleaseAndNull(uint32_rtv_clear_pipelines_[i][j]);
    }
  }
  ui::d3d12::util::ReleaseAndNull(uint32_rtv_clear_root_signature_);

  for (const auto& dump_pipeline_pair : dump_pipelines_) {
    if (dump_pipeline_pair.second) {
      dump_pipeline_pair.second->Release();
    }
  }
  dump_pipelines_.clear();
  for (const auto& direct_resolve_pipeline_pair : direct_resolve_pipelines_) {
    bool aliased_resolve_copy_pipeline = false;
    for (ID3D12PipelineState* resolve_copy_pipeline : resolve_copy_pipelines_) {
      if (direct_resolve_pipeline_pair.second == resolve_copy_pipeline) {
        aliased_resolve_copy_pipeline = true;
        break;
      }
    }
    if (direct_resolve_pipeline_pair.second && !aliased_resolve_copy_pipeline) {
      direct_resolve_pipeline_pair.second->Release();
    }
  }
  direct_resolve_pipelines_.clear();
  ui::d3d12::util::ReleaseAndNull(direct_resolve_root_signature_depth_);
  ui::d3d12::util::ReleaseAndNull(direct_resolve_root_signature_color_);
  ui::d3d12::util::ReleaseAndNull(dump_root_signature_depth_);
  ui::d3d12::util::ReleaseAndNull(dump_root_signature_color_);

  for (const auto& transfer_pipeline_array_pair : transfer_stencil_bit_pipelines_) {
    for (ID3D12PipelineState* transfer_pipeline : transfer_pipeline_array_pair.second) {
      if (transfer_pipeline) {
        transfer_pipeline->Release();
      }
    }
  }
  transfer_stencil_bit_pipelines_.clear();
  for (const auto& transfer_pipeline_pair : transfer_pipelines_) {
    if (transfer_pipeline_pair.second) {
      transfer_pipeline_pair.second->Release();
    }
  }
  transfer_pipelines_.clear();
  for (size_t i = 0; i < rex::countof(transfer_root_signatures_); ++i) {
    ui::d3d12::util::ReleaseAndNull(transfer_root_signatures_[i]);
  }

  transfer_vertex_buffer_pool_.reset();

  for (size_t i = 0; i < rex::countof(host_depth_store_pipelines_); ++i) {
    ui::d3d12::util::ReleaseAndNull(host_depth_store_pipelines_[i]);
  }
  ui::d3d12::util::ReleaseAndNull(host_depth_store_root_signature_);

  null_rtv_descriptor_ms_.Free();
  null_rtv_descriptor_ss_.Free();
  descriptor_pool_srv_.reset();
  descriptor_pool_depth_.reset();
  descriptor_pool_color_.reset();

  for (size_t i = 0; i < rex::countof(resolve_copy_native_pipelines_); ++i) {
    ui::d3d12::util::ReleaseAndNull(resolve_copy_native_pipelines_[i]);
  }
  ui::d3d12::util::ReleaseAndNull(resolve_copy_native_root_signature_);
  for (size_t i = 0; i < rex::countof(resolve_copy_pipelines_); ++i) {
    ui::d3d12::util::ReleaseAndNull(resolve_copy_pipelines_[i]);
  }
  ui::d3d12::util::ReleaseAndNull(resolve_copy_root_signature_);

  edram_snapshot_restore_pool_.reset();
  ui::d3d12::util::ReleaseAndNull(edram_snapshot_download_buffer_);

  ui::d3d12::util::ReleaseAndNull(edram_buffer_descriptor_heap_);
  ui::d3d12::util::ReleaseAndNull(edram_buffer_);

  if (!from_destructor) {
    ShutdownCommon();
  }
}

void D3D12RenderTargetCache::CompletedSubmissionUpdated() {
  CompleteColorTargetReadbacks();
  if (edram_snapshot_restore_pool_) {
    edram_snapshot_restore_pool_->Reclaim(command_processor_.GetCompletedSubmission());
  }
  if (transfer_vertex_buffer_pool_) {
    transfer_vertex_buffer_pool_->Reclaim(command_processor_.GetCompletedSubmission());
  }
}

void D3D12RenderTargetCache::BeginSubmission() {
  // New command list - render targets not bound.
  InvalidateCommandListRenderTargets();
  // ExecuteCommandLists is a full UAV barrier.
  if (edram_buffer_modification_status_ != EdramBufferModificationStatus::kUnmodified) {
    assert_true(edram_buffer_state_ == D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    edram_buffer_modification_status_ = EdramBufferModificationStatus::kUnmodified;
    PixelShaderInterlockFullEdramBarrierPlaced();
  }
}

bool D3D12RenderTargetCache::Update(bool is_rasterization_done,
                                    reg::RB_DEPTHCONTROL normalized_depth_control,
                                    uint32_t normalized_color_mask, const Shader& vertex_shader,
                                    bool native_shader_grid) {
  // scene_update_capture_ is set only by the measurement-build scene capture.
  if (kGpuDiagnostics && scene_update_capture_) {
    // The capture records this draw's complete ownership update.
    InvalidateUpdateMemo();
  }
  if (!RenderTargetCache::Update(is_rasterization_done, normalized_depth_control,
                                 normalized_color_mask, vertex_shader, native_shader_grid)) {
    return false;
  }
  switch (GetPath()) {
    case Path::kHostRenderTargets: {
      RenderTarget* const* depth_and_color_render_targets =
          last_update_accumulated_render_targets();
      if (kGpuDiagnostics && scene_update_capture_) {
        RecordSceneUpdateTargets(depth_and_color_render_targets, last_update_transfers(),
                                 *scene_update_capture_);
      }
      if (kGpuDiagnostics && scene_update_capture_) {
        CaptureSceneAliasTransfers(depth_and_color_render_targets, last_update_transfers(), false);
      }
      // Update already established ownership. Keep the empty-work check at the
      // caller so a draw with no transfer also avoids the transfer helper's
      // register saves and stack setup. Resolve clears use their own call below.
      bool has_transfer = false;
      for (uint32_t i = 0; i < 1 + xenos::kMaxColorRenderTargets; ++i) {
        if (!last_update_transfers()[i].empty()) {
          has_transfer = true;
          break;
        }
      }
      if (LastUpdateSkippedDepthTransfers() && command_processor_.GpuTimingTransferLogFrame()) {
        const RenderTarget* depth_rt = depth_and_color_render_targets[0];
        std::fprintf(stderr,
                     "REX_GPU_TRANSFER_SKIPPED frame=%llu dest_key=%08X reason=overwritten_depth "
                     "ranges=%u\n",
                     static_cast<unsigned long long>(command_processor_.GetCurrentFrame()),
                     depth_rt ? depth_rt->key().key : 0u, LastUpdateSkippedDepthTransfers());
      }
      if (has_transfer && command_processor_.GpuTimingTransferLogFrame()) {
        // The draw whose render target bindings caused the transfers below.
        const RegisterFile& regs = register_file();
        std::fprintf(
            stderr,
            "REX_GPU_TRANSFER_DRAW frame=%llu vs=%016llX depthcontrol=%08X "
            "normalized_depthcontrol=%08X color_mask=%08X normalized_color_mask=%08X "
            "stencilrefmask=%08X stencilrefmask_bf=%08X modecontrol=%08X surface_info=%08X "
            "depth_info=%08X color_info0=%08X clip_cntl=%08X sc_mode_cntl=%08X "
            "draw_initiator=%08X window_scissor_br=%08X vport_yscale=%.2f vport_yoffset=%.2f "
            "window_scissor_tl=%08X window_offset=%08X vte_cntl=%08X vtx_cntl=%08X "
            "overwrite_check=%s rect=%.2f,%.2f,%.2f,%.2f range=%ux%u scissor=%d,%d,%d,%d\n",
            static_cast<unsigned long long>(command_processor_.GetCurrentFrame()),
            static_cast<unsigned long long>(vertex_shader.ucode_data_hash()),
            regs[XE_GPU_REG_RB_DEPTHCONTROL], normalized_depth_control.value,
            regs[XE_GPU_REG_RB_COLOR_MASK], normalized_color_mask,
            regs[XE_GPU_REG_RB_STENCILREFMASK], regs[XE_GPU_REG_RB_STENCILREFMASK_BF],
            regs[XE_GPU_REG_RB_MODECONTROL], regs[XE_GPU_REG_RB_SURFACE_INFO],
            regs[XE_GPU_REG_RB_DEPTH_INFO], regs[XE_GPU_REG_RB_COLOR_INFO],
            regs[XE_GPU_REG_PA_CL_CLIP_CNTL], regs[XE_GPU_REG_PA_SU_SC_MODE_CNTL],
            regs[XE_GPU_REG_VGT_DRAW_INITIATOR], regs[XE_GPU_REG_PA_SC_WINDOW_SCISSOR_BR],
            regs.Get<float>(XE_GPU_REG_PA_CL_VPORT_YSCALE),
            regs.Get<float>(XE_GPU_REG_PA_CL_VPORT_YOFFSET),
            regs[XE_GPU_REG_PA_SC_WINDOW_SCISSOR_TL], regs[XE_GPU_REG_PA_SC_WINDOW_OFFSET],
            regs[XE_GPU_REG_PA_CL_VTE_CNTL], regs[XE_GPU_REG_PA_SU_VTX_CNTL],
            LastDepthOverwriteCheck().result, LastDepthOverwriteCheck().rect[0],
            LastDepthOverwriteCheck().rect[1], LastDepthOverwriteCheck().rect[2],
            LastDepthOverwriteCheck().rect[3], LastDepthOverwriteCheck().range_width,
            LastDepthOverwriteCheck().range_height, LastDepthOverwriteCheck().scissor[0],
            LastDepthOverwriteCheck().scissor[1], LastDepthOverwriteCheck().scissor[2],
            LastDepthOverwriteCheck().scissor[3]);
      }
      if (has_transfer) {
        D3D12CommandProcessor::GpuTimingScope transfer_timing(command_processor_,
                                                              GpuTimingCategory::kTransfer);
        PerformTransfersAndResolveClears(1 + xenos::kMaxColorRenderTargets,
                                         depth_and_color_render_targets, last_update_transfers(),
                                         nullptr, nullptr, nullptr, scene_update_capture_);
      } else if (kGpuDiagnostics && scene_update_capture_) {
        scene_update_capture_->helper_completed = true;
      }
      if (kGpuDiagnostics && scene_update_capture_) {
        CaptureSceneAliasTransfers(depth_and_color_render_targets, last_update_transfers(), true);
      }
      SetCommandListRenderTargets(depth_and_color_render_targets);
      if (command_processor_.GpuTimingEnabled()) {
        // Draw time from here on belongs to this render-target set.
        uint32_t keys[1 + xenos::kMaxColorRenderTargets];
        LastUpdateRenderTargetKeys(keys);
        command_processor_.GpuTimingNotePass(keys);
      }
    } break;
    case Path::kPixelShaderInterlock: {
      // For ROV, only the barrier is needed - already scheduled if required.
      // But the buffer will be used for ROV drawing now.
      TransitionEdramBuffer(D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
      // Commit preceding UAV (but not ROV) writes like clears as they aren't
      // synchronized with ROV accesses.
      CommitEdramBufferUAVWrites(EdramBufferModificationStatus::kAsUAV);
      // TODO(Triang3l): Check if this draw call modifies color or depth /
      // stencil, at least coarsely, to prevent useless barriers.
      MarkEdramBufferModified(EdramBufferModificationStatus::kAsROV);
    } break;
    default:
      assert_unhandled_case(GetPath());
      return false;
  }
  return true;
}

void D3D12RenderTargetCache::WriteEdramRawSRVDescriptor(D3D12_CPU_DESCRIPTOR_HANDLE handle) {
  const ui::d3d12::D3D12Provider& provider = command_processor_.GetD3D12Provider();
  ID3D12Device* device = provider.GetDevice();
  device->CopyDescriptorsSimple(
      1, handle,
      provider.OffsetViewDescriptor(edram_buffer_descriptor_heap_start_,
                                    uint32_t(EdramBufferDescriptorIndex::kRawSRV)),
      D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
}

void D3D12RenderTargetCache::WriteEdramRawUAVDescriptor(D3D12_CPU_DESCRIPTOR_HANDLE handle) {
  const ui::d3d12::D3D12Provider& provider = command_processor_.GetD3D12Provider();
  ID3D12Device* device = provider.GetDevice();
  device->CopyDescriptorsSimple(
      1, handle,
      provider.OffsetViewDescriptor(edram_buffer_descriptor_heap_start_,
                                    uint32_t(EdramBufferDescriptorIndex::kRawUAV)),
      D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
}

void D3D12RenderTargetCache::WriteEdramUintPow2SRVDescriptor(D3D12_CPU_DESCRIPTOR_HANDLE handle,
                                                             uint32_t element_size_bytes_pow2) {
  EdramBufferDescriptorIndex descriptor_index;
  switch (element_size_bytes_pow2) {
    case 2:
      descriptor_index = EdramBufferDescriptorIndex::kR32UintSRV;
      break;
    case 3:
      descriptor_index = EdramBufferDescriptorIndex::kR32G32UintSRV;
      break;
    case 4:
      descriptor_index = EdramBufferDescriptorIndex::kR32G32B32A32UintSRV;
      break;
    default:
      assert_unhandled_case(element_size_bytes_pow2);
      return;
  }
  const ui::d3d12::D3D12Provider& provider = command_processor_.GetD3D12Provider();
  ID3D12Device* device = provider.GetDevice();
  device->CopyDescriptorsSimple(1, handle,
                                provider.OffsetViewDescriptor(edram_buffer_descriptor_heap_start_,
                                                              uint32_t(descriptor_index)),
                                D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
}

void D3D12RenderTargetCache::WriteEdramUintPow2UAVDescriptor(D3D12_CPU_DESCRIPTOR_HANDLE handle,
                                                             uint32_t element_size_bytes_pow2) {
  EdramBufferDescriptorIndex descriptor_index;
  switch (element_size_bytes_pow2) {
    case 2:
      descriptor_index = EdramBufferDescriptorIndex::kR32UintUAV;
      break;
    case 3:
      descriptor_index = EdramBufferDescriptorIndex::kR32G32UintUAV;
      break;
    case 4:
      descriptor_index = EdramBufferDescriptorIndex::kR32G32B32A32UintUAV;
      break;
    default:
      assert_unhandled_case(element_size_bytes_pow2);
      return;
  }
  const ui::d3d12::D3D12Provider& provider = command_processor_.GetD3D12Provider();
  ID3D12Device* device = provider.GetDevice();
  device->CopyDescriptorsSimple(1, handle,
                                provider.OffsetViewDescriptor(edram_buffer_descriptor_heap_start_,
                                                              uint32_t(descriptor_index)),
                                D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
}

// Whether the dump for this resolve writes the pixel-centre depth of 2x MSAA
// depth render targets (see resolve_depth_pixel_center).
static bool IsResolveDumpingDepthCenter(const draw_util::ResolveInfo& resolve_info) {
  return REXCVAR_GET(resolve_depth_pixel_center) &&
         resolve_info.rb_copy_control.copy_src_select >= xenos::kMaxColorRenderTargets;
}

bool D3D12RenderTargetCache::Resolve(const memory::Memory& memory, D3D12SharedMemory& shared_memory,
                                     D3D12TextureCache& texture_cache,
                                     uint32_t& written_address_out, uint32_t& written_length_out,
                                     const char* embedded_capture_label,
                                     const char* embedded_capture_path,
                                     bool* written_scaled_out,
                                     const char* embedded_edram_capture_path,
                                     const char* embedded_scaled_capture_path,
                                     const embedded_scene_resolve_capture_policy::Context* scene_capture_context) {
  written_address_out = 0;
  written_length_out = 0;
  if (written_scaled_out) {
    *written_scaled_out = false;
  }

  // The embedded title host doesn't initialize ReXGlue's normal logging
  // frontend. Keep failure diagnostics local and bounded so dropped resolves
  // can be classified without changing their behavior.
  const auto log_embedded_resolve_failure = [&](const char* stage) {
    static uint64_t failure_ordinal = 0;
    const uint64_t ordinal = ++failure_ordinal;
    // Empty-after-scissor resolves are common in this title when UI/video
    // rectangles are deliberately positioned wholly outside the active
    // scissor. Preserve enough evidence to catch a new failure class without
    // synchronously flushing one record every 256 no-op resolves forever.
    // First occurrences and powers of two retain ordering and growth evidence.
    if (ordinal > 8 && (ordinal & (ordinal - 1))) {
      return;
    }
    const RegisterFile& regs = register_file();
    const reg::RB_COPY_CONTROL control = regs.Get<reg::RB_COPY_CONTROL>();
    const reg::RB_COPY_DEST_INFO dest_info = regs.Get<reg::RB_COPY_DEST_INFO>();
    const reg::RB_COPY_DEST_PITCH dest_pitch = regs.Get<reg::RB_COPY_DEST_PITCH>();
    const reg::RB_SURFACE_INFO surface_info = regs.Get<reg::RB_SURFACE_INFO>();
    const xenos::xe_gpu_vertex_fetch_t fetch = regs.GetVertexFetch(0);
    const char* rejection_reason = "downstream";
    if (!std::strcmp(stage, "get_resolve_info")) {
      if (control.copy_command != xenos::CopyCommand::kRaw &&
          control.copy_command != xenos::CopyCommand::kConvert) {
        rejection_reason = "copy_command";
      } else if (fetch.type != xenos::FetchConstantType::kVertex || fetch.size != 6) {
        rejection_reason = "vertex_fetch";
      } else if (surface_info.msaa_samples > xenos::MsaaSamples::k4X) {
        rejection_reason = "msaa";
      } else {
        // These are the only early exits in the pinned GetResolveInfo after
        // the validated checks above.
        rejection_reason = "empty_after_scissor";
      }
    }
    uint32_t vertex_words[6]{};
    const uint32_t vertex_address = fetch.address * sizeof(uint32_t);
    if (const uint8_t* vertex_data = memory.TranslatePhysical(vertex_address)) {
      std::memcpy(vertex_words, vertex_data, sizeof(vertex_words));
    }
    uint32_t source_color_info = 0;
    if (control.copy_src_select < xenos::kMaxColorRenderTargets) {
      source_color_info =
          regs.Get<reg::RB_COLOR_INFO>(
                  reg::RB_COLOR_INFO::rt_register_indices[control.copy_src_select])
              .value;
    }
    std::fprintf(
        stderr,
        "REX_EMBEDDED_RESOLVE_FAILURE ordinal=%llu stage=%s reason=%s control=0x%08X "
        "dest_info=0x%08X dest_base=0x%08X dest_pitch=0x%08X surface=0x%08X "
        "source_color=0x%08X vertex_fetch=%08X,%08X vertex_address=0x%08X "
        "window_scissor=%08X,%08X screen_scissor=%08X,%08X window_offset=%08X "
        "sc_mode=%08X vtx_control=%08X "
        "vertex_words=%08X,%08X,%08X,%08X,%08X,%08X\n",
        static_cast<unsigned long long>(ordinal), stage, rejection_reason, control.value,
        dest_info.value,
        regs[XE_GPU_REG_RB_COPY_DEST_BASE], dest_pitch.value, surface_info.value,
        source_color_info, fetch.dword_0, fetch.dword_1, vertex_address,
        regs[XE_GPU_REG_PA_SC_WINDOW_SCISSOR_TL], regs[XE_GPU_REG_PA_SC_WINDOW_SCISSOR_BR],
        regs[XE_GPU_REG_PA_SC_SCREEN_SCISSOR_TL], regs[XE_GPU_REG_PA_SC_SCREEN_SCISSOR_BR],
        regs[XE_GPU_REG_PA_SC_WINDOW_OFFSET], regs[XE_GPU_REG_PA_SU_SC_MODE_CNTL],
        regs[XE_GPU_REG_PA_SU_VTX_CNTL], vertex_words[0],
        vertex_words[1], vertex_words[2], vertex_words[3], vertex_words[4], vertex_words[5]);
    std::fflush(stderr);
  };

  bool draw_resolution_scaled = IsDrawResolutionScaled();

  draw_util::ResolveInfo resolve_info;
  bool fixed_16_truncated_to_minus_1_to_1 = IsFixed16TruncatedToMinus1To1();
  if (!draw_util::GetResolveInfo(register_file(), memory, trace_writer_, draw_resolution_scale_x(),
                                 draw_resolution_scale_y(), fixed_16_truncated_to_minus_1_to_1,
                                 fixed_16_truncated_to_minus_1_to_1, resolve_info)) {
    log_embedded_resolve_failure("get_resolve_info");
    return false;
  }

  // Nothing to copy/clear.
  if (!resolve_info.coordinate_info.width_div_8 || !resolve_info.height_div_8) {
    return true;
  }

  if (embedded_capture_label && resolve_info.copy_dest_extent_length) {
    const bool capture_ok = CaptureEmbeddedResolveSource(
        resolve_info, embedded_capture_label, embedded_capture_path);
    std::fprintf(
        stderr,
        "REX_EMBEDDED_RESOLVE_SOURCE_CAPTURE result=%u label=%s "
        "dest_start=0x%08X dest_length=%u\n",
        capture_ok ? 1u : 0u, embedded_capture_label,
        resolve_info.copy_dest_extent_start,
        resolve_info.copy_dest_extent_length);
    std::fflush(stderr);
  }

  DeferredCommandList& command_list = command_processor_.GetDeferredCommandList();

  // Copying.
  bool copied = false;
  if (resolve_info.copy_dest_extent_length) {
    D3D12CommandProcessor::GpuTimingScope copy_timing(command_processor_,
                                                      GpuTimingCategory::kResolveCopy);
    bool copy_native = false;
    uint32_t dump_base = 0;
    uint32_t dump_row_length_used = 0;
    uint32_t dump_rows = 0;
    uint32_t dump_pitch = 0;
    if (GetPath() == Path::kHostRenderTargets) {
      resolve_info.GetCopyEdramTileSpan(dump_base, dump_row_length_used,
                                        dump_rows, dump_pitch);
      copy_native = IsResolveSourceNativeOnly(
          dump_base, dump_row_length_used, dump_rows, dump_pitch);
      if (copy_native &&
          !draw_util::GetResolveInfo(
              register_file(), memory, trace_writer_, 1, 1,
              fixed_16_truncated_to_minus_1_to_1,
              fixed_16_truncated_to_minus_1_to_1, resolve_info)) {
        log_embedded_resolve_failure("native_get_resolve_info");
        return false;
      }
    }
    const bool copy_dest_scaled = draw_resolution_scaled && !copy_native;
    if (command_processor_.GpuTimingEnabled()) {
      command_processor_.GpuTimingCount(GpuTimingCounter::kResolveCopies, 1);
      command_processor_.GpuTimingCount(
          GpuTimingCounter::kResolveCopyBytes,
          uint64_t(resolve_info.copy_dest_extent_length) *
              (copy_dest_scaled ? draw_resolution_scale_x() * draw_resolution_scale_y() : 1));
    }
    uint32_t depth_boundary_ordinal = 0;
    // Bounded provenance only: explain native/mixed EDRAM ownership before
    // the existing resolve chooses its destination representation. Metadata is
    // passive; the separate default-off depth observer adds deferred copies,
    // never ownership edits or new submission boundaries.
    static uint32_t resolve_grid_trace_count = 0;
    // The depth exporter needs the actual post-clipping resolve rectangle and
    // encoding too. Reuse this bounded, selected-frame provenance observer;
    // window scissors or contiguous addresses alone do not prove image layout.
    if ((REXCVAR_GET(embedded_target_writer_capture_count) ||
         (REXCVAR_GET(embedded_temporal_depth_resolve_capture) &&
          resolve_info.IsCopyingDepth())) &&
        IsCurrentEmbeddedGameplayCaptureFrame() && resolve_grid_trace_count < 64 &&
        GetPath() == Path::kHostRenderTargets) {
      ++resolve_grid_trace_count;
      std::vector<ResolveCopyDumpRectangle> grid_rectangles;
      GetResolveCopyRectanglesToDump(dump_base, dump_row_length_used, dump_rows,
                                     dump_pitch, grid_rectangles);
      uint32_t native_rectangles = 0;
      for (const auto& rectangle : grid_rectangles) {
        native_rectangles += rectangle.render_target->key().scale_native ? 1u : 0u;
      }
      std::fprintf(stderr,
          "REX_EMBEDDED_RESOLVE_GRID ordinal=%u source_native=%u dest_scaled=%u "
          "dest_base=0x%08X extent=0x%08X+%u tiles=%u,%u,%u,%u "
          "rectangles=%u native_rectangles=%u original_base=%08X rect=%u,%u,%u,%u pitch=%u format=%u endian=%u\n",
          resolve_grid_trace_count, copy_native ? 1u : 0u, copy_dest_scaled ? 1u : 0u,
          resolve_info.copy_dest_base, resolve_info.copy_dest_extent_start,
          resolve_info.copy_dest_extent_length, dump_base, dump_row_length_used,
          dump_rows, dump_pitch, uint32_t(grid_rectangles.size()), native_rectangles,
          resolve_info.copy_dest_original_base, resolve_info.copy_dest_rect[0],
          resolve_info.copy_dest_rect[1], resolve_info.copy_dest_rect[2], resolve_info.copy_dest_rect[3],
          uint32_t(resolve_info.copy_dest_coordinate_info.pitch_aligned_div_32) << 5,
          uint32_t(resolve_info.copy_dest_info.copy_dest_format),
          uint32_t(resolve_info.copy_dest_info.copy_dest_endian));
      // Counts alone don't prove the requested EDRAM rectangle is owned.
      // Keep this metadata under the existing one-frame/64-resolve budget,
      // with a separate global 64-owner cap. Unlogged owners remain explicitly
      // unknown. The opt-in source observer selects at most one owner resource
      // per resolve ordinal so filenames and the paired EDRAM epoch are unique.
      static uint32_t resolve_owner_trace_count = 0;
      uint32_t owner_logged = 0;
      for (const auto& rectangle : grid_rectangles) {
        if (resolve_owner_trace_count >= 64) break;
        ++resolve_owner_trace_count;
        ++owner_logged;
        auto* owner = static_cast<D3D12RenderTarget*>(rectangle.render_target);
        const auto owner_key = owner->key();
        const auto desc = owner->resource()->GetDesc();
        if (REXCVAR_GET(embedded_resolve_depth_source_capture) && owner_key.is_depth &&
            !depth_boundary_ordinal) {
          const bool queued = QueueDepthSourceReadback(owner, resolve_grid_trace_count,
                                                       resolve_info.copy_dest_base);
          if (queued) depth_boundary_ordinal = resolve_grid_trace_count;
          std::fprintf(stderr,
              "REX_EMBEDDED_DEPTH_SOURCE_SELECTION ordinal=%u queued=%u\n",
              resolve_grid_trace_count, queued ? 1u : 0u);
        }
        std::fprintf(stderr,
            "REX_EMBEDDED_RESOLVE_OWNER ordinal=%u owner=%u dest_base=0x%08X "
            "resource=%p key=0x%08X base=%u pitch32=%u pitch=%u msaa=%u "
            "depth=%u format=%u scale_native=%u row_first=%u rows=%u "
            "first_start=%u last_end=%u host_width=%llu host_height=%u "
            "host_format=%u host_samples=%u\n",
            resolve_grid_trace_count, owner_logged - 1, resolve_info.copy_dest_base,
            static_cast<void*>(owner->resource()), owner_key.key, owner_key.base_tiles,
            owner_key.pitch_tiles_at_32bpp, owner_key.GetPitchTiles(),
            uint32_t(owner_key.msaa_samples), owner_key.is_depth,
            owner_key.resource_format, owner_key.scale_native, rectangle.row_first,
            rectangle.rows, rectangle.row_first_start, rectangle.row_last_end,
            static_cast<unsigned long long>(desc.Width), desc.Height,
            uint32_t(desc.Format), desc.SampleDesc.Count);
      }
      if (owner_logged != grid_rectangles.size()) {
        std::fprintf(stderr,
            "REX_EMBEDDED_RESOLVE_OWNER_TRUNCATED ordinal=%u logged=%u total=%u\n",
            resolve_grid_trace_count, owner_logged, uint32_t(grid_rectangles.size()));
      }
    }

    // Separate passive scene chronology. The caller reserved one of at most64
    // events in its single selected frame. Do not consume the existing depth/
    // writer metadata counters or arm their optional depth-source copies.
    if (scene_capture_context && GetPath() == Path::kHostRenderTargets) {
      const auto& c = *scene_capture_context;
      std::vector<ResolveCopyDumpRectangle> rectangles;
      GetResolveCopyRectanglesToDump(dump_base, dump_row_length_used, dump_rows,
                                    dump_pitch, rectangles);
      static uint32_t scene_owner_trace_count = 0;
      uint32_t logged = 0;
      std::fprintf(stderr,
          "REX_SCENE_RESOLVE_LAYOUT frame=%llu resolve=%llu last_draw=%llu "
          "source_native=%u dest_scaled=%u dest_base=%08X extent=%08X+%u "
          "tiles=%u,%u,%u,%u rectangles=%u original_base=%08X rect=%u,%u,%u,%u "
          "pitch=%u format=%u endian=%u\n",
          static_cast<unsigned long long>(c.frame), static_cast<unsigned long long>(c.ordinal),
          static_cast<unsigned long long>(c.last_draw), copy_native ? 1u : 0u, copy_dest_scaled ? 1u : 0u,
          resolve_info.copy_dest_base, resolve_info.copy_dest_extent_start,
          resolve_info.copy_dest_extent_length, dump_base, dump_row_length_used, dump_rows, dump_pitch,
          uint32_t(rectangles.size()), resolve_info.copy_dest_original_base,
          resolve_info.copy_dest_rect[0], resolve_info.copy_dest_rect[1],
          resolve_info.copy_dest_rect[2], resolve_info.copy_dest_rect[3],
          uint32_t(resolve_info.copy_dest_coordinate_info.pitch_aligned_div_32) << 5,
          uint32_t(resolve_info.copy_dest_info.copy_dest_format),
          uint32_t(resolve_info.copy_dest_info.copy_dest_endian));
      for (const auto& rectangle : rectangles) {
        if (scene_owner_trace_count == 128) break;
        ++scene_owner_trace_count;
        auto* owner = static_cast<D3D12RenderTarget*>(rectangle.render_target);
        const auto key = owner->key();
        const auto desc = owner->resource()->GetDesc();
        std::fprintf(stderr,
            "REX_SCENE_RESOLVE_OWNER frame=%llu resolve=%llu last_draw=%llu owner=%u "
            "resource=%p key=%08X base=%u pitch32=%u pitch=%u msaa=%u depth=%u format=%u "
            "scale_native=%u row_first=%u rows=%u first_start=%u last_end=%u "
            "host_width=%llu host_height=%u host_format=%u host_samples=%u\n",
            static_cast<unsigned long long>(c.frame), static_cast<unsigned long long>(c.ordinal),
            static_cast<unsigned long long>(c.last_draw), logged++, static_cast<void*>(owner->resource()),
            key.key, key.base_tiles, key.pitch_tiles_at_32bpp, key.GetPitchTiles(),
            uint32_t(key.msaa_samples), key.is_depth, key.resource_format, key.scale_native,
            rectangle.row_first, rectangle.rows, rectangle.row_first_start, rectangle.row_last_end,
            static_cast<unsigned long long>(desc.Width), desc.Height, uint32_t(desc.Format), desc.SampleDesc.Count);
      }
      std::fprintf(stderr,
          "REX_SCENE_RESOLVE_OWNERS frame=%llu resolve=%llu logged=%u total=%u\n",
          static_cast<unsigned long long>(c.frame), static_cast<unsigned long long>(c.ordinal),
          logged, uint32_t(rectangles.size()));
    }

    draw_util::ResolveCopyShaderConstants copy_shader_constants;
    uint32_t copy_group_count_x, copy_group_count_y;
    draw_util::ResolveCopyShaderIndex copy_shader =
        resolve_info.GetCopyShader(copy_native ? 1 : draw_resolution_scale_x(),
                                   copy_native ? 1 : draw_resolution_scale_y(),
                                   copy_shader_constants, copy_group_count_x,
                                   copy_group_count_y);
    assert_true(copy_group_count_x && copy_group_count_y);
    if (copy_shader != draw_util::ResolveCopyShaderIndex::kUnknown) {
      bool direct_resolved = false;
      if (GetPath() == Path::kHostRenderTargets) {
        if (!copy_native && REXCVAR_GET(direct_host_resolve)) {
          direct_resolved =
              TryResolveCopyDirectly(resolve_info, copy_shader,
                                     copy_dest_scaled);
          if (direct_resolved) {
            ++direct_resolve_success_count_;
          } else {
            ++direct_resolve_fallback_count_;
          }
        }
        if (!direct_resolved) {
          // Dump the current contents of the render targets owning the affected
          // range to edram_buffer_.
          D3D12CommandProcessor::GpuTimingScope dump_timing(command_processor_,
                                                            GpuTimingCategory::kResolveDump);
          if (!DumpRenderTargets(dump_base, dump_row_length_used, dump_rows,
                                 dump_pitch, copy_native,
                                 IsResolveDumpingDepthCenter(resolve_info))) {
            REXGPU_ERROR("D3D12RenderTargetCache: Failed to dump host render targets for resolve");
            log_embedded_resolve_failure("dump_render_targets");
            return false;
          }
        }
        if (depth_boundary_ordinal) {
          // Pair the actual production dump with the host-source snapshot.
          // Copy only; no additional dump, CPU wait, or guest-memory upload.
          PendingDepthSourceReadback p;
          p.source = edram_buffer_;
          p.bytes = edram_buffer_->GetDesc().Width;
          p.ordinal = depth_boundary_ordinal;
          p.destination = resolve_info.copy_dest_base;
          p.path = "rex_resolve_depth_edram_" + std::to_string(p.ordinal) + ".bin";
          D3D12_RESOURCE_DESC buffer_desc;
          ui::d3d12::util::FillBufferResourceDesc(buffer_desc, p.bytes, D3D12_RESOURCE_FLAG_NONE);
          const bool prepared = p.bytes && p.bytes <= UINT64_C(64) * 1024 * 1024 &&
              SUCCEEDED(command_processor_.GetD3D12Provider().GetDevice()->CreateCommittedResource(
                  &ui::d3d12::util::kHeapPropertiesReadback, D3D12_HEAP_FLAG_NONE,
                  &buffer_desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                  IID_PPV_ARGS(&p.readback)));
          if (prepared) {
            p.submission = command_processor_.GetCurrentSubmission();
            const auto previous = edram_buffer_state_;
            TransitionEdramBuffer(D3D12_RESOURCE_STATE_COPY_SOURCE);
            command_processor_.SubmitBarriers();
            command_list.D3DCopyBufferRegion(p.readback.Get(), 0, edram_buffer_, 0, p.bytes);
            TransitionEdramBuffer(previous);
            command_processor_.SubmitBarriers();
          }
          std::fprintf(stderr,
              "REX_EMBEDDED_DEPTH_EDRAM_QUEUED result=%u ordinal=%u dest_base=0x%08X "
              "bytes=%llu submission=%llu scale=%ux%u path=%s\n",
              prepared ? 1u : 0u, p.ordinal, p.destination,
              static_cast<unsigned long long>(p.bytes), static_cast<unsigned long long>(p.submission),
              draw_resolution_scale_x(), draw_resolution_scale_y(), p.path.c_str());
          if (prepared) pending_depth_source_readbacks_.push_back(std::move(p));
        }
        if (embedded_edram_capture_path) {
          const bool edram_capture_ok = CaptureEmbeddedEdramBuffer(
              resolve_info, embedded_capture_label,
              embedded_edram_capture_path);
          std::fprintf(
              stderr,
              "REX_EMBEDDED_RESOLVE_BOUNDARY_STAGE stage=edram_after_dump "
              "result=%u label=%s\n",
              edram_capture_ok ? 1u : 0u,
              embedded_capture_label ? embedded_capture_label : "unnamed");
          std::fflush(stderr);
        }
      }

      const FormatInfo* copy_dest_format_info = FormatInfo::Get(
          uint32_t(resolve_info.copy_dest_info.copy_dest_format));
      uint32_t copy_dest_pixel_size_log2 = 0;
      bool copy_dest_pixel_size_supported =
          copy_dest_format_info && copy_dest_format_info->bits_per_pixel >= 8 &&
          copy_dest_format_info->bits_per_pixel <= 128 &&
          rex::bit_scan_forward(copy_dest_format_info->bits_per_pixel >> 3,
                                &copy_dest_pixel_size_log2) &&
          copy_dest_format_info->bits_per_pixel ==
              (UINT32_C(8) << copy_dest_pixel_size_log2);

      // Make sure there is memory to write to.
      bool copy_dest_committed;
      if (copy_dest_scaled) {
        // Committing starting with the beginning of the potentially written
        // extent, but making the buffer containing the base current as the
        // beginning of the bound buffer is the base.
        copy_dest_committed =
            copy_dest_pixel_size_supported &&
            texture_cache.InitializeUnscaledResolvePagesFromSharedMemory(
                resolve_info.copy_dest_extent_start,
                resolve_info.copy_dest_extent_length,
                copy_dest_pixel_size_log2) &&
            texture_cache.EnsureScaledResolveMemoryCommitted(
                resolve_info.copy_dest_extent_start, resolve_info.copy_dest_extent_length) &&
            texture_cache.MakeScaledResolveRangeCurrent(resolve_info.copy_dest_base,
                                                        resolve_info.copy_dest_extent_start -
                                                            resolve_info.copy_dest_base +
                                                            resolve_info.copy_dest_extent_length);
      } else {
        copy_dest_committed = shared_memory.RequestRange(resolve_info.copy_dest_extent_start,
                                                         resolve_info.copy_dest_extent_length);
      }
      if (copy_dest_committed) {
        // Every embedded D3D12 resolve-copy shader declares ByteAddressBuffer
        // and RWByteAddressBuffer (native and scaled). The shared shader-info
        // table still describes typed views used by other backends; it is not
        // this DXBC binding ABI. In particular a uint4 EDRAM SRV has one quarter
        // as many elements and truncates raw loads at 2.5MiB on the tested GPU.
        // Probe331 + gpu_resolve_raw_views reproduce depth rows256..383 missing
        // and an all-zero base1536 shadow resolve with that mismatched view.
        // Keep raw descriptors at both ends; do not alter Vulkan's metadata.
        // Write the descriptors and transition the resources.
        // Full shared memory without resolution scaling, range of the scaled
        // resolve buffer with scaling because only at least 128 * 2^20 R32
        // elements must be addressable
        // (D3D12_REQ_BUFFER_RESOURCE_TEXEL_COUNT_2_TO_EXP).
        ui::d3d12::util::DescriptorCpuGpuHandlePair descriptor_dest;
        ui::d3d12::util::DescriptorCpuGpuHandlePair descriptor_source;
        ui::d3d12::util::DescriptorCpuGpuHandlePair descriptors[2];
        if (command_processor_.RequestOneUseSingleViewDescriptors(
                bindless_resources_used_ ? uint32_t(copy_dest_scaled) : 2,
                descriptors)) {
          if (bindless_resources_used_) {
            if (copy_dest_scaled) {
              descriptor_dest = descriptors[0];
            } else {
              descriptor_dest = command_processor_.GetSystemBindlessViewHandlePair(
                  D3D12CommandProcessor::SystemBindlessView::kSharedMemoryRawUAV);
            }
            descriptor_source = command_processor_.GetSystemBindlessViewHandlePair(
                D3D12CommandProcessor::SystemBindlessView::kEdramRawSRV);
          } else {
            descriptor_dest = descriptors[0];
            if (!copy_dest_scaled) {
              shared_memory.WriteRawUAVDescriptor(descriptor_dest.first);
            }
            descriptor_source = descriptors[1];
            WriteEdramRawSRVDescriptor(descriptor_source.first);
          }
          if (copy_dest_scaled) {
            texture_cache.CreateCurrentScaledResolveRangeRawUAV(descriptor_dest.first);
            texture_cache.TransitionCurrentScaledResolveRange(
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
          } else {
            shared_memory.UseForWriting();
          }
          TransitionEdramBuffer(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

          // Submit the resolve.
          command_list.D3DSetComputeRootSignature(
              copy_native ? resolve_copy_native_root_signature_
                          : resolve_copy_root_signature_);
          command_list.D3DSetComputeRootDescriptorTable(2, descriptor_source.second);
          command_list.D3DSetComputeRootDescriptorTable(1, descriptor_dest.second);
          if (copy_dest_scaled) {
            command_list.D3DSetComputeRoot32BitConstants(
                0, sizeof(copy_shader_constants.dest_relative) / sizeof(uint32_t),
                &copy_shader_constants.dest_relative, 0);
          } else {
            command_list.D3DSetComputeRoot32BitConstants(
                0, sizeof(copy_shader_constants) / sizeof(uint32_t), &copy_shader_constants, 0);
          }
          command_processor_.SetExternalPipeline(
              copy_native
                  ? resolve_copy_native_pipelines_[size_t(copy_shader)]
                  : resolve_copy_pipelines_[size_t(copy_shader)]);
          command_processor_.SubmitBarriers();
          command_list.D3DDispatch(copy_group_count_x, copy_group_count_y, 1);

          // Order the resolve with other work using the destination as a UAV.
          if (copy_dest_scaled) {
            texture_cache.MarkCurrentScaledResolveRangeUAVWritesCommitNeeded();
          } else {
            shared_memory.MarkUAVWritesCommitNeeded();
          }

          if (copy_dest_scaled && embedded_scaled_capture_path) {
            const bool scaled_capture_ok =
                texture_cache.CaptureCurrentScaledResolveRange(
                    resolve_info.copy_dest_extent_start,
                    resolve_info.copy_dest_extent_length,
                    embedded_capture_label,
                    embedded_scaled_capture_path);
            std::fprintf(
                stderr,
                "REX_EMBEDDED_RESOLVE_BOUNDARY_STAGE "
                "stage=scaled_after_copy result=%u label=%s\n",
                scaled_capture_ok ? 1u : 0u,
                embedded_capture_label ? embedded_capture_label : "unnamed");
            std::fflush(stderr);
          }

          // Invalidate textures and mark the range as scaled if needed.
          native_resolve::Write native_region_write;
          native_region_write.layout = {
              resolve_info.copy_dest_original_base,
              uint32_t(resolve_info.copy_dest_coordinate_info.pitch_aligned_div_32) << 5,
              uint32_t(resolve_info.copy_dest_info.copy_dest_format),
              uint32_t(resolve_info.copy_dest_info.copy_dest_endian),
              copy_dest_pixel_size_log2};
          native_region_write.rect = {resolve_info.copy_dest_rect[0],
              resolve_info.copy_dest_rect[1], resolve_info.copy_dest_rect[2],
              resolve_info.copy_dest_rect[3]};
          native_region_write.extent_start = resolve_info.copy_dest_extent_start;
          native_region_write.extent_length = resolve_info.copy_dest_extent_length;
          native_region_write.layout_known =
              !resolve_info.copy_dest_info.copy_dest_array && !resolve_info.IsCopyingDepth();
          texture_cache.MarkRangeAsResolved(resolve_info.copy_dest_extent_start,
                                            resolve_info.copy_dest_extent_length,
                                            copy_dest_pixel_size_log2,
                                            copy_dest_scaled, &native_region_write);
          written_address_out = resolve_info.copy_dest_extent_start;
          written_length_out = resolve_info.copy_dest_extent_length;
          if (written_scaled_out) {
            *written_scaled_out = copy_dest_scaled;
          }
          copied = true;
        } else {
          log_embedded_resolve_failure("copy_descriptor_request");
        }
      } else {
        log_embedded_resolve_failure("copy_destination_memory");
        REXGPU_ERROR(
            "D3D12RenderTargetCache: Failed to obtain the resolve destination "
            "memory region");
      }
    } else {
      log_embedded_resolve_failure("copy_shader_unknown");
    }
  } else {
    copied = true;
  }

  // Clearing.
  bool cleared = false;
  bool clear_depth = resolve_info.IsClearingDepth();
  bool clear_color = resolve_info.IsClearingColor();
  if (clear_depth || clear_color) {
    D3D12CommandProcessor::GpuTimingScope clear_timing(command_processor_,
                                                       GpuTimingCategory::kResolveClear);
    switch (GetPath()) {
      case Path::kHostRenderTargets: {
        Transfer::Rectangle clear_rectangle;
        RenderTarget* clear_render_targets[2];
        // If PrepareHostRenderTargetsResolveClear returns false, may be just an
        // empty region (success) or an error - don't care.
        const bool clear_prepared = PrepareHostRenderTargetsResolveClear(
            resolve_info, clear_rectangle, clear_render_targets[0], clear_transfers_[0],
            clear_render_targets[1], clear_transfers_[1]);
        if (scene_capture_context) {
          const auto& c = *scene_capture_context;
          std::fprintf(stderr,
              "REX_SCENE_CLEAR_PREPARE frame=%llu resolve=%llu last_draw=%llu "
              "depth_requested=%u color_requested=%u prepared=%u submission=%llu "
              "scope=actual_preparation_not_clear_execution\n",
              static_cast<unsigned long long>(c.frame), static_cast<unsigned long long>(c.ordinal),
              static_cast<unsigned long long>(c.last_draw), clear_depth ? 1u : 0u,
              clear_color ? 1u : 0u, clear_prepared ? 1u : 0u,
              static_cast<unsigned long long>(command_processor_.GetCurrentSubmission()));
        }
        if (clear_prepared) {
          uint64_t clear_values[2];
          clear_values[0] = resolve_info.rb_depth_clear;
          clear_values[1] =
              resolve_info.rb_color_clear | (uint64_t(resolve_info.rb_color_clear_lo) << 32);
          PerformTransfersAndResolveClears(2, clear_render_targets, clear_transfers_, clear_values,
                                           &clear_rectangle, scene_capture_context);
        }
        cleared = true;
      } break;
      case Path::kPixelShaderInterlock: {
        ui::d3d12::util::DescriptorCpuGpuHandlePair descriptor_edram;
        bool descriptor_edram_obtained;
        if (bindless_resources_used_) {
          descriptor_edram = command_processor_.GetSystemBindlessViewHandlePair(
              D3D12CommandProcessor::SystemBindlessView ::kEdramR32G32B32A32UintUAV);
          descriptor_edram_obtained = true;
        } else {
          descriptor_edram_obtained =
              command_processor_.RequestOneUseSingleViewDescriptors(1, &descriptor_edram);
          if (descriptor_edram_obtained) {
            WriteEdramUintPow2UAVDescriptor(descriptor_edram.first, 4);
          }
        }
        if (descriptor_edram_obtained) {
          TransitionEdramBuffer(D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
          // Should be safe to only commit once (if was UAV / ROV previously -
          // if there was nothing to copy, only to clear, for some reason, for
          // instance), overlap of the depth and the color ranges is highly
          // unlikely.
          CommitEdramBufferUAVWrites();
          command_list.D3DSetComputeRootSignature(resolve_rov_clear_root_signature_);
          command_list.D3DSetComputeRootDescriptorTable(1, descriptor_edram.second);
          std::pair<uint32_t, uint32_t> clear_group_count = resolve_info.GetClearShaderGroupCount(
              draw_resolution_scale_x(), draw_resolution_scale_y());
          assert_true(clear_group_count.first && clear_group_count.second);
          if (clear_depth) {
            draw_util::ResolveClearShaderConstants depth_clear_constants;
            resolve_info.GetDepthClearShaderConstants(depth_clear_constants);
            command_list.D3DSetComputeRoot32BitConstants(
                0, sizeof(depth_clear_constants) / sizeof(uint32_t), &depth_clear_constants, 0);
            command_processor_.SetExternalPipeline(resolve_rov_clear_32bpp_pipeline_);
            command_processor_.SubmitBarriers();
            command_list.D3DDispatch(clear_group_count.first, clear_group_count.second, 1);
          }
          if (clear_color) {
            draw_util::ResolveClearShaderConstants color_clear_constants;
            resolve_info.GetColorClearShaderConstants(color_clear_constants);
            if (clear_depth) {
              // Non-RT-specific constants have already been set.
              command_list.D3DSetComputeRoot32BitConstants(
                  0, sizeof(color_clear_constants.rt_specific) / sizeof(uint32_t),
                  &color_clear_constants.rt_specific,
                  offsetof(draw_util::ResolveClearShaderConstants, rt_specific) / sizeof(uint32_t));
            } else {
              command_list.D3DSetComputeRoot32BitConstants(
                  0, sizeof(color_clear_constants) / sizeof(uint32_t), &color_clear_constants, 0);
            }
            command_processor_.SetExternalPipeline(resolve_info.color_edram_info.format_is_64bpp
                                                       ? resolve_rov_clear_64bpp_pipeline_
                                                       : resolve_rov_clear_32bpp_pipeline_);
            command_processor_.SubmitBarriers();
            command_list.D3DDispatch(clear_group_count.first, clear_group_count.second, 1);
          }
          MarkEdramBufferModified();
          cleared = true;
        } else {
          log_embedded_resolve_failure("clear_descriptor_request");
        }
      } break;
      default:
        assert_unhandled_case(GetPath());
    }
  } else {
    cleared = true;
  }

  if (!copied || !cleared) {
    log_embedded_resolve_failure(!copied ? "copy_incomplete" : "clear_incomplete");
  }

  return copied && cleared;
}

bool D3D12RenderTargetCache::InitializeTraceSubmitDownloads() {
  if (IsDrawResolutionScaled()) {
    // No 1:1 mapping.
    return false;
  }
  if (!edram_snapshot_download_buffer_) {
    D3D12_RESOURCE_DESC edram_snapshot_download_buffer_desc;
    ui::d3d12::util::FillBufferResourceDesc(edram_snapshot_download_buffer_desc,
                                            xenos::kEdramSizeBytes, D3D12_RESOURCE_FLAG_NONE);
    const ui::d3d12::D3D12Provider& provider = command_processor_.GetD3D12Provider();
    ID3D12Device* device = provider.GetDevice();
    if (FAILED(device->CreateCommittedResource(
            &ui::d3d12::util::kHeapPropertiesReadback, provider.GetHeapFlagCreateNotZeroed(),
            &edram_snapshot_download_buffer_desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
            IID_PPV_ARGS(&edram_snapshot_download_buffer_)))) {
      REXGPU_ERROR(
          "D3D12RenderTargetCache: Failed to create a EDRAM snapshot download "
          "buffer");
      return false;
    }
  }
  if (GetPath() == Path::kHostRenderTargets) {
    // Dump all host render targets to edram_buffer_.
    if (!DumpRenderTargets(0, xenos::kEdramTileCount, 1, xenos::kEdramTileCount)) {
      REXGPU_ERROR("D3D12RenderTargetCache: Failed to dump host render targets for trace");
      return false;
    }
  }
  TransitionEdramBuffer(D3D12_RESOURCE_STATE_COPY_SOURCE);
  command_processor_.SubmitBarriers();
  command_processor_.GetDeferredCommandList().D3DCopyBufferRegion(
      edram_snapshot_download_buffer_, 0, edram_buffer_, 0, xenos::kEdramSizeBytes);
  return true;
}

void D3D12RenderTargetCache::InitializeTraceCompleteDownloads() {
  if (!edram_snapshot_download_buffer_) {
    return;
  }
  void* download_mapping;
  if (SUCCEEDED(edram_snapshot_download_buffer_->Map(0, nullptr, &download_mapping))) {
    trace_writer_.WriteEdramSnapshot(download_mapping);
    D3D12_RANGE download_write_range = {};
    edram_snapshot_download_buffer_->Unmap(0, &download_write_range);
  } else {
    REXGPU_ERROR(
        "D3D12RenderTargetCache: Failed to map the EDRAM snapshot download "
        "buffer");
  }
  edram_snapshot_download_buffer_->Release();
  edram_snapshot_download_buffer_ = nullptr;
}

void D3D12RenderTargetCache::RestoreEdramSnapshot(const void* snapshot) {
  if (IsDrawResolutionScaled()) {
    // No 1:1 mapping.
    return;
  }

  // Create the buffer - will be used for copying to either a 32-bit 1280x2048
  // render target or the EDRAM buffer.
  const ui::d3d12::D3D12Provider& provider = command_processor_.GetD3D12Provider();
  if (!edram_snapshot_restore_pool_) {
    edram_snapshot_restore_pool_ =
        std::make_unique<ui::d3d12::D3D12UploadBufferPool>(provider, xenos::kEdramSizeBytes);
  }
  ID3D12Resource* upload_buffer;
  size_t upload_buffer_offset;
  void* upload_buffer_mapping = edram_snapshot_restore_pool_->Request(
      command_processor_.GetCurrentSubmission(), xenos::kEdramSizeBytes, 1, &upload_buffer,
      &upload_buffer_offset, nullptr);
  if (!upload_buffer_mapping) {
    REXGPU_ERROR(
        "D3D12RenderTargetCache: Failed to get a buffer for restoring a EDRAM "
        "snapshot");
    return;
  }

  DeferredCommandList& command_list = command_processor_.GetDeferredCommandList();

  switch (GetPath()) {
    case Path::kHostRenderTargets: {
      // k_32_FLOAT because it's unambiguous (not effected by something like
      // DXGI_FORMAT_R8G8B8A8 vs. DXGI_FORMAT_B8G8R8A8).
      D3D12RenderTarget* full_edram_render_target =
          static_cast<D3D12RenderTarget*>(PrepareFullEdram1280xRenderTargetForSnapshotRestoration(
              xenos::ColorRenderTargetFormat::k_32_FLOAT));
      if (!full_edram_render_target) {
        return;
      }
      D3D12_TEXTURE_COPY_LOCATION copy_source_location;
      copy_source_location.pResource = upload_buffer;
      copy_source_location.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
      UINT64 copy_total_bytes;
      D3D12_RESOURCE_DESC full_edram_render_target_desc =
          full_edram_render_target->resource()->GetDesc();
      provider.GetDevice()->GetCopyableFootprints(&full_edram_render_target_desc, 0, 1, 0,
                                                  &copy_source_location.PlacedFootprint, nullptr,
                                                  nullptr, &copy_total_bytes);
      // 1280 width * sizeof(uint32_t) is aligned to
      // D3D12_TEXTURE_DATA_PITCH_ALIGNMENT (256).
      assert_true(copy_total_bytes <= xenos::kEdramSizeBytes);
      assert_false(full_edram_render_target->key().Is64bpp());
      uint32_t pitch_tiles = full_edram_render_target->key().pitch_tiles_at_32bpp;
      uint32_t tile_rows = xenos::kEdramTileCount / pitch_tiles;
      assert_true(pitch_tiles * tile_rows == xenos::kEdramTileCount);
      const uint8_t* snapshot_sample_row = reinterpret_cast<const uint8_t*>(snapshot);
      for (uint32_t y_tile = 0; y_tile < tile_rows; ++y_tile) {
        uint8_t* upload_buffer_tile_row_origin =
            reinterpret_cast<uint8_t*>(upload_buffer_mapping) +
            copy_source_location.PlacedFootprint.Offset +
            xenos::kEdramTileHeightSamples * y_tile *
                copy_source_location.PlacedFootprint.Footprint.RowPitch;
        for (uint32_t x_tile = 0; x_tile < pitch_tiles; ++x_tile) {
          uint8_t* upload_buffer_sample_row =
              upload_buffer_tile_row_origin +
              sizeof(uint32_t) * xenos::kEdramTileWidthSamples * x_tile;
          for (uint32_t sample_row = 0; sample_row < xenos::kEdramTileHeightSamples; ++sample_row) {
            std::memcpy(upload_buffer_sample_row, snapshot_sample_row,
                        sizeof(uint32_t) * xenos::kEdramTileWidthSamples);
            snapshot_sample_row += sizeof(uint32_t) * xenos::kEdramTileWidthSamples;
            upload_buffer_sample_row += copy_source_location.PlacedFootprint.Footprint.RowPitch;
          }
        }
      }
      command_processor_.PushTransitionBarrier(
          full_edram_render_target->resource(),
          full_edram_render_target->SetResourceState(D3D12_RESOURCE_STATE_COPY_DEST),
          D3D12_RESOURCE_STATE_COPY_DEST);
      command_processor_.SubmitBarriers();
      D3D12_TEXTURE_COPY_LOCATION copy_dest_location;
      copy_dest_location.pResource = full_edram_render_target->resource();
      copy_dest_location.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
      copy_dest_location.SubresourceIndex = 0;
      command_list.D3DCopyTextureRegion(&copy_dest_location, 0, 0, 0, &copy_source_location,
                                        nullptr);
    } break;

    case Path::kPixelShaderInterlock: {
      std::memcpy(upload_buffer_mapping, snapshot, xenos::kEdramSizeBytes);
      TransitionEdramBuffer(D3D12_RESOURCE_STATE_COPY_DEST);
      command_processor_.SubmitBarriers();
      command_list.D3DCopyBufferRegion(edram_buffer_, 0, upload_buffer,
                                       UINT64(upload_buffer_offset), xenos::kEdramSizeBytes);
    } break;

    default:
      assert_unhandled_case(GetPath());
  }
}

DXGI_FORMAT D3D12RenderTargetCache::GetColorResourceDXGIFormat(
    xenos::ColorRenderTargetFormat format) const {
  // Typed should be preferred over typeless so there are more opportunities for
  // compression.
  switch (format) {
    case xenos::ColorRenderTargetFormat::k_8_8_8_8:
      return DXGI_FORMAT_R8G8B8A8_UNORM;
    case xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA:
      return gamma_render_target_as_unorm16_ ? DXGI_FORMAT_R16G16B16A16_UNORM
                                             : DXGI_FORMAT_R8G8B8A8_UNORM;
    case xenos::ColorRenderTargetFormat::k_2_10_10_10:
    case xenos::ColorRenderTargetFormat::k_2_10_10_10_AS_10_10_10_10:
      return DXGI_FORMAT_R10G10B10A2_UNORM;
    case xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT:
    case xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT_AS_16_16_16_16:
      return DXGI_FORMAT_R16G16B16A16_FLOAT;
    // SNORM has two representations of -1.
    case xenos::ColorRenderTargetFormat::k_16_16:
      return DXGI_FORMAT_R16G16_TYPELESS;
    case xenos::ColorRenderTargetFormat::k_16_16_16_16:
      return DXGI_FORMAT_R16G16B16A16_TYPELESS;
    // Floating-point - ensure NaN propagation during ownership transfer for
    // unmodified data.
    case xenos::ColorRenderTargetFormat::k_16_16_FLOAT:
      return DXGI_FORMAT_R16G16_TYPELESS;
    case xenos::ColorRenderTargetFormat::k_16_16_16_16_FLOAT:
      return DXGI_FORMAT_R16G16B16A16_TYPELESS;
    // TODO(Triang3l): Check if NaN propagation defined in the D3D11.3
    // specification can be relied on for 32-bit float render targets.
    case xenos::ColorRenderTargetFormat::k_32_FLOAT:
      return DXGI_FORMAT_R32_TYPELESS;
    case xenos::ColorRenderTargetFormat::k_32_32_FLOAT:
      return DXGI_FORMAT_R32G32_TYPELESS;
    default:
      assert_unhandled_case(format);
      return DXGI_FORMAT_UNKNOWN;
  }
}

DXGI_FORMAT D3D12RenderTargetCache::GetColorDrawDXGIFormat(
    xenos::ColorRenderTargetFormat format) const {
  switch (format) {
    case xenos::ColorRenderTargetFormat::k_16_16:
      return DXGI_FORMAT_R16G16_SNORM;
    case xenos::ColorRenderTargetFormat::k_16_16_16_16:
      return DXGI_FORMAT_R16G16B16A16_SNORM;
    case xenos::ColorRenderTargetFormat::k_16_16_FLOAT:
      return DXGI_FORMAT_R16G16_FLOAT;
    case xenos::ColorRenderTargetFormat::k_16_16_16_16_FLOAT:
      return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case xenos::ColorRenderTargetFormat::k_32_FLOAT:
      return DXGI_FORMAT_R32_FLOAT;
    case xenos::ColorRenderTargetFormat::k_32_32_FLOAT:
      return DXGI_FORMAT_R32G32_FLOAT;
    default:
      return GetColorResourceDXGIFormat(format);
  }
}

DXGI_FORMAT D3D12RenderTargetCache::GetColorOwnershipTransferDXGIFormat(
    xenos::ColorRenderTargetFormat format, bool* is_integer_out) const {
  if (is_integer_out) {
    *is_integer_out = true;
  }
  switch (format) {
    case xenos::ColorRenderTargetFormat::k_16_16:
    case xenos::ColorRenderTargetFormat::k_16_16_FLOAT:
      return DXGI_FORMAT_R16G16_UINT;
    case xenos::ColorRenderTargetFormat::k_16_16_16_16:
    case xenos::ColorRenderTargetFormat::k_16_16_16_16_FLOAT:
      return DXGI_FORMAT_R16G16B16A16_UINT;
    case xenos::ColorRenderTargetFormat::k_32_FLOAT:
      return DXGI_FORMAT_R32_UINT;
    case xenos::ColorRenderTargetFormat::k_32_32_FLOAT:
      return DXGI_FORMAT_R32G32_UINT;
    default:
      if (is_integer_out) {
        *is_integer_out = false;
      }
      return GetColorDrawDXGIFormat(format);
  }
}

DXGI_FORMAT D3D12RenderTargetCache::GetDepthResourceDXGIFormat(
    xenos::DepthRenderTargetFormat format) {
  switch (format) {
    case xenos::DepthRenderTargetFormat::kD24S8:
      return DXGI_FORMAT_R24G8_TYPELESS;
    case xenos::DepthRenderTargetFormat::kD24FS8:
      return DXGI_FORMAT_R32G8X24_TYPELESS;
    default:
      assert_unhandled_case(format);
      return DXGI_FORMAT_UNKNOWN;
  }
}

DXGI_FORMAT D3D12RenderTargetCache::GetDepthDSVDXGIFormat(xenos::DepthRenderTargetFormat format) {
  switch (format) {
    case xenos::DepthRenderTargetFormat::kD24S8:
      return DXGI_FORMAT_D24_UNORM_S8_UINT;
    case xenos::DepthRenderTargetFormat::kD24FS8:
      return DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
    default:
      assert_unhandled_case(format);
      return DXGI_FORMAT_UNKNOWN;
  }
}

DXGI_FORMAT D3D12RenderTargetCache::GetDepthSRVDepthDXGIFormat(
    xenos::DepthRenderTargetFormat format) {
  switch (format) {
    case xenos::DepthRenderTargetFormat::kD24S8:
      return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    case xenos::DepthRenderTargetFormat::kD24FS8:
      return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
    default:
      assert_unhandled_case(format);
      return DXGI_FORMAT_UNKNOWN;
  }
}

DXGI_FORMAT D3D12RenderTargetCache::GetDepthSRVStencilDXGIFormat(
    xenos::DepthRenderTargetFormat format) {
  switch (format) {
    case xenos::DepthRenderTargetFormat::kD24S8:
      return DXGI_FORMAT_X24_TYPELESS_G8_UINT;
    case xenos::DepthRenderTargetFormat::kD24FS8:
      return DXGI_FORMAT_X32_TYPELESS_G8X24_UINT;
    default:
      assert_unhandled_case(format);
      return DXGI_FORMAT_UNKNOWN;
  }
}

RenderTargetCache::RenderTarget* D3D12RenderTargetCache::CreateRenderTarget(RenderTargetKey key) {
  ID3D12Device* device = command_processor_.GetD3D12Provider().GetDevice();

  D3D12_RESOURCE_DESC resource_desc;
  resource_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  resource_desc.Alignment = 0;
  resource_desc.Width = key.GetWidth() * GetKeyScaleX(key);
  resource_desc.Height =
      GetRenderTargetHeight(key.pitch_tiles_at_32bpp, key.msaa_samples) *
      GetKeyScaleY(key);
  resource_desc.DepthOrArraySize = 1;
  resource_desc.MipLevels = 1;
  if (key.is_depth) {
    resource_desc.Format = GetDepthResourceDXGIFormat(key.GetDepthFormat());
  } else {
    resource_desc.Format = GetColorResourceDXGIFormat(key.GetColorFormat());
  }
  assert_true(resource_desc.Format != DXGI_FORMAT_UNKNOWN);
  if (resource_desc.Format == DXGI_FORMAT_UNKNOWN) {
    REXGPU_ERROR("D3D12RenderTargetCache: Unknown {} render target format {}",
                 key.is_depth ? "depth" : "color", uint32_t(key.resource_format));
    return nullptr;
  }
  if (key.msaa_samples == xenos::MsaaSamples::k2X && !msaa_2x_supported()) {
    resource_desc.SampleDesc.Count = 4;
  } else {
    resource_desc.SampleDesc.Count = UINT(1) << UINT(key.msaa_samples);
  }
  resource_desc.SampleDesc.Quality = 0;
  resource_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
  resource_desc.Flags = key.is_depth ? D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL
                                     : D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
  // Simultaneous access is not allowed with MSAA or depth.
  const bool uncompressed = !key.is_depth && resource_desc.SampleDesc.Count == 1 &&
                            REXCVAR_GET(d3d12_render_target_uncompressed);
  if (uncompressed) {
    resource_desc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS;
  }
  // The first access will be ownership transfer into this render target or
  // starting to draw directly.
  D3D12_RESOURCE_STATES resource_state =
      key.is_depth ? D3D12_RESOURCE_STATE_DEPTH_WRITE : D3D12_RESOURCE_STATE_RENDER_TARGET;
  D3D12_CLEAR_VALUE optimized_clear_value;
  if (key.is_depth) {
    optimized_clear_value.Format = GetDepthDSVDXGIFormat(key.GetDepthFormat());
    // Fixed-point depth is generally direct (1 being the farthest),
    // floating-point is used for more uniform precision across the range (0
    // being the farthest).
    optimized_clear_value.DepthStencil.Depth =
        key.GetDepthFormat() == xenos::DepthRenderTargetFormat::kD24S8 ? 1.0f : 0.0f;
    optimized_clear_value.DepthStencil.Stencil = 0;
  } else {
    optimized_clear_value.Format = GetColorDrawDXGIFormat(key.GetColorFormat());
    optimized_clear_value.Color[0] = 0.0f;
    optimized_clear_value.Color[1] = 0.0f;
    optimized_clear_value.Color[2] = 0.0f;
    optimized_clear_value.Color[3] = 0.0f;
  }
  // Create zeroed for more determinism, primarily with respect to compression
  // and depth float24 / float32 mirroring.
  Microsoft::WRL::ComPtr<ID3D12Resource> resource;
  if (FAILED(device->CreateCommittedResource(&ui::d3d12::util::kHeapPropertiesDefault,
                                             D3D12_HEAP_FLAG_NONE, &resource_desc, resource_state,
                                             &optimized_clear_value, IID_PPV_ARGS(&resource)))) {
    if (!uncompressed) {
      return nullptr;
    }
    // The diagnostic flag was refused: create the render target normally.
    std::fprintf(stderr, "REX_GPU_TEST_SWITCHES uncompressed_render_target_refused key=%08X\n",
                 key.key);
    std::fflush(stderr);
    resource_desc.Flags &= ~D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS;
    if (FAILED(device->CreateCommittedResource(&ui::d3d12::util::kHeapPropertiesDefault,
                                               D3D12_HEAP_FLAG_NONE, &resource_desc,
                                               resource_state, &optimized_clear_value,
                                               IID_PPV_ARGS(&resource)))) {
      return nullptr;
    }
  }
  {
    std::u16string resource_name = rex::string::to_utf16(key.GetDebugName());
    resource->SetName(reinterpret_cast<LPCWSTR>(resource_name.c_str()));
  }

  ui::d3d12::D3D12CpuDescriptorPool& descriptor_pool =
      key.is_depth ? *descriptor_pool_depth_ : *descriptor_pool_color_;
  ui::d3d12::D3D12CpuDescriptorPool::Descriptor descriptor_draw =
      descriptor_pool.AllocateDescriptor();
  ui::d3d12::D3D12CpuDescriptorPool::Descriptor descriptor_srv =
      descriptor_pool_srv_->AllocateDescriptor();
  if (!descriptor_draw.IsValid() || !descriptor_srv.IsValid()) {
    return nullptr;
  }
  D3D12_CPU_DESCRIPTOR_HANDLE descriptor_draw_handle = descriptor_draw.GetHandle();
  ui::d3d12::D3D12CpuDescriptorPool::Descriptor descriptor_load_separate;
  ui::d3d12::D3D12CpuDescriptorPool::Descriptor descriptor_srv_stencil;
  D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc;
  srv_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  if (resource_desc.SampleDesc.Count > 1) {
    srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DMS;
  } else {
    srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv_desc.Texture2D.MostDetailedMip = 0;
    srv_desc.Texture2D.MipLevels = 1;
    srv_desc.Texture2D.PlaneSlice = 0;
    srv_desc.Texture2D.ResourceMinLODClamp = 0.0f;
  }
  if (key.is_depth) {
    // DSV and stencil SRV.
    descriptor_srv_stencil = descriptor_pool_srv_->AllocateDescriptor();
    if (!descriptor_srv_stencil.IsValid()) {
      return nullptr;
    }
    D3D12_DEPTH_STENCIL_VIEW_DESC dsv_desc;
    dsv_desc.Format = optimized_clear_value.Format;
    dsv_desc.Flags = D3D12_DSV_FLAG_NONE;
    D3D12_SHADER_RESOURCE_VIEW_DESC stencil_srv_desc;
    stencil_srv_desc.Format = GetDepthSRVStencilDXGIFormat(key.GetDepthFormat());
    stencil_srv_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    if (resource_desc.SampleDesc.Count > 1) {
      dsv_desc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DMS;
      stencil_srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DMS;
    } else {
      dsv_desc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
      dsv_desc.Texture2D.MipSlice = 0;
      stencil_srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
      stencil_srv_desc.Texture2D.MostDetailedMip = 0;
      stencil_srv_desc.Texture2D.MipLevels = 1;
      stencil_srv_desc.Texture2D.PlaneSlice = 1;
      stencil_srv_desc.Texture2D.ResourceMinLODClamp = 0.0f;
    }
    device->CreateDepthStencilView(resource.Get(), &dsv_desc, descriptor_draw_handle);
    device->CreateShaderResourceView(resource.Get(), &stencil_srv_desc,
                                     descriptor_srv_stencil.GetHandle());
    // Depth SRV.
    srv_desc.Format = GetDepthSRVDepthDXGIFormat(key.GetDepthFormat());
  } else {
    // Drawing RTV.
    D3D12_RENDER_TARGET_VIEW_DESC rtv_desc;
    rtv_desc.Format = optimized_clear_value.Format;
    if (resource_desc.SampleDesc.Count > 1) {
      rtv_desc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DMS;
    } else {
      rtv_desc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
      rtv_desc.Texture2D.MipSlice = 0;
      rtv_desc.Texture2D.PlaneSlice = 0;
    }
    device->CreateRenderTargetView(resource.Get(), &rtv_desc, descriptor_draw_handle);
    // Ownership transfer RTV.
    DXGI_FORMAT load_format = GetColorOwnershipTransferDXGIFormat(key.GetColorFormat());
    if (rtv_desc.Format != load_format) {
      descriptor_load_separate = descriptor_pool.AllocateDescriptor();
      if (!descriptor_load_separate.IsValid()) {
        return nullptr;
      }
      rtv_desc.Format = load_format;
      device->CreateRenderTargetView(resource.Get(), &rtv_desc,
                                     descriptor_load_separate.GetHandle());
    }
    // SRV for ownership transfer and dumping.
    srv_desc.Format = load_format;
  }
  device->CreateShaderResourceView(resource.Get(), &srv_desc, descriptor_srv.GetHandle());

  return new D3D12RenderTarget(key, resource.Get(), std::move(descriptor_draw),
                               std::move(descriptor_load_separate), std::move(descriptor_srv),
                               std::move(descriptor_srv_stencil), resource_state);
}

bool D3D12RenderTargetCache::IsHostDepthEncodingDifferent(
    xenos::DepthRenderTargetFormat format) const {
  if (format == xenos::DepthRenderTargetFormat::kD24FS8) {
    return !depth_float24_convert_in_pixel_shader_;
  }
  return false;
}

bool D3D12RenderTargetCache::IsGammaFormatHostStorageSeparate() const {
  return gamma_render_target_as_unorm16_;
}

void D3D12RenderTargetCache::RequestPixelShaderInterlockBarrier() {
  CommitEdramBufferUAVWrites();
}

void D3D12RenderTargetCache::TransitionEdramBuffer(D3D12_RESOURCE_STATES new_state) {
  if (command_processor_.PushTransitionBarrier(edram_buffer_, edram_buffer_state_, new_state)) {
    // Resetting edram_buffer_modification_status_ only if the barrier has been
    // truly inserted - in particular, not resetting it for UAV > UAV as
    // barriers are dropped if the state hasn't been changed.
    edram_buffer_modification_status_ = EdramBufferModificationStatus::kUnmodified;
  }
  edram_buffer_state_ = new_state;
}

void D3D12RenderTargetCache::MarkEdramBufferModified(
    EdramBufferModificationStatus modification_status) {
  assert_true(modification_status != EdramBufferModificationStatus::kUnmodified);
  assert_true(edram_buffer_state_ == D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  if (edram_buffer_state_ != D3D12_RESOURCE_STATE_UNORDERED_ACCESS) {
    return;
  }
  // max because being modified as a UAV requires stricter synchronization than
  // as ROV.
  edram_buffer_modification_status_ =
      std::max(edram_buffer_modification_status_, modification_status);
}

void D3D12RenderTargetCache::CommitEdramBufferUAVWrites(
    EdramBufferModificationStatus commit_status) {
  assert_true(commit_status != EdramBufferModificationStatus::kUnmodified);
  if (edram_buffer_modification_status_ < commit_status) {
    return;
  }
  assert_true(edram_buffer_state_ == D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  if (edram_buffer_state_ == D3D12_RESOURCE_STATE_UNORDERED_ACCESS) {
    command_processor_.PushUAVBarrier(edram_buffer_);
  }
  edram_buffer_modification_status_ = EdramBufferModificationStatus::kUnmodified;
  PixelShaderInterlockFullEdramBarrierPlaced();
}

// Host sample indices used by the transfer remapping helpers:
// - 4x bit 0 is horizontal and bit 1 is vertical in Direct3D 10.1+.
// - Native 2x uses top sample 1 and bottom sample 0.
// - 2x emulated as 4x uses top sample 0 and bottom sample 3.

// Convert view pixel coordinates in r0.xy and a host sample index to the
// canonical guest sample coordinates. The guest pixel and scaled subpixel are
// kept in r1.xy and r2.xy when resolution scaling is active.
static void CanonicalizeSample(dxbc::Assembler& a,
                               xenos::MsaaSamples msaa_samples,
                               dxbc::Src host_sample, bool msaa_2x_supported,
                               uint32_t scale_x, uint32_t scale_y,
                               dxbc::Src& u_out, dxbc::Src& v_out,
                               bool& scaled_out) {
  bool scaled = scale_x > 1 || scale_y > 1;
  scaled_out = scaled;
  dxbc::Src guest_x(dxbc::Src::R(0, dxbc::Src::kXXXX));
  dxbc::Src guest_y(dxbc::Src::R(0, dxbc::Src::kYYYY));
  if (scaled) {
    a.OpUDiv(dxbc::Dest::R(1, 0b0011), dxbc::Dest::R(2, 0b0011),
             dxbc::Src::R(0, 0b01000100),
             dxbc::Src::LU(scale_x, scale_y, scale_x, scale_y));
    guest_x = dxbc::Src::R(1, dxbc::Src::kXXXX);
    guest_y = dxbc::Src::R(1, dxbc::Src::kYYYY);
  }
  u_out = guest_x;
  v_out = guest_y;
  if (msaa_samples >= xenos::MsaaSamples::k4X) {
    a.OpBFI(dxbc::Dest::R(2, 0b0100), dxbc::Src::LU(1), dxbc::Src::LU(1),
            host_sample, guest_x);
    a.OpUShR(dxbc::Dest::R(2, 0b1000), guest_x, dxbc::Src::LU(1));
    a.OpBFI(dxbc::Dest::R(1, 0b0001), dxbc::Src::LU(30), dxbc::Src::LU(2),
            dxbc::Src::R(2, dxbc::Src::kWWWW),
            dxbc::Src::R(2, dxbc::Src::kZZZZ));
    a.OpUShR(dxbc::Dest::R(2, 0b0100), host_sample, dxbc::Src::LU(1));
    a.OpBFI(dxbc::Dest::R(2, 0b0100), dxbc::Src::LU(1), dxbc::Src::LU(1),
            dxbc::Src::R(2, dxbc::Src::kZZZZ), guest_y);
    a.OpUShR(dxbc::Dest::R(2, 0b1000), guest_y, dxbc::Src::LU(1));
    a.OpBFI(dxbc::Dest::R(1, 0b0010), dxbc::Src::LU(30), dxbc::Src::LU(2),
            dxbc::Src::R(2, dxbc::Src::kWWWW),
            dxbc::Src::R(2, dxbc::Src::kZZZZ));
    u_out = dxbc::Src::R(1, dxbc::Src::kXXXX);
    v_out = dxbc::Src::R(1, dxbc::Src::kYYYY);
  } else if (msaa_samples == xenos::MsaaSamples::k2X) {
    if (msaa_2x_supported) {
      a.OpXOr(dxbc::Dest::R(2, 0b0100), host_sample, dxbc::Src::LU(1));
    } else {
      a.OpUShR(dxbc::Dest::R(2, 0b0100), host_sample, dxbc::Src::LU(1));
    }
    a.OpUShR(dxbc::Dest::R(2, 0b1000), guest_x, dxbc::Src::LU(1));
    a.OpBFI(dxbc::Dest::R(2, 0b1000), dxbc::Src::LU(1), dxbc::Src::LU(1),
            dxbc::Src::R(2, dxbc::Src::kWWWW), guest_y);
    a.OpBFI(dxbc::Dest::R(1, 0b0001), dxbc::Src::LU(1), dxbc::Src::LU(1),
            dxbc::Src::R(2, dxbc::Src::kZZZZ), guest_x);
    a.OpUShR(dxbc::Dest::R(2, 0b0100), guest_y, dxbc::Src::LU(1));
    a.OpBFI(dxbc::Dest::R(1, 0b0010), dxbc::Src::LU(30), dxbc::Src::LU(2),
            dxbc::Src::R(2, dxbc::Src::kZZZZ),
            dxbc::Src::R(2, dxbc::Src::kWWWW));
    u_out = dxbc::Src::R(1, dxbc::Src::kXXXX);
    v_out = dxbc::Src::R(1, dxbc::Src::kYYYY);
  }
}

// Convert canonical guest sample coordinates back to view pixels and host
// sample index, preserving the subpixel within a resolution-scaled sample.
static void DecanonicalizeSample(dxbc::Assembler& a,
                                 xenos::MsaaSamples msaa_samples, dxbc::Src u,
                                 dxbc::Src v, bool scaled,
                                 bool msaa_2x_supported, uint32_t scale_x,
                                 uint32_t scale_y, dxbc::Src& x_out,
                                 dxbc::Src& y_out, dxbc::Src& sample_out) {
  x_out = u;
  y_out = v;
  if (msaa_samples >= xenos::MsaaSamples::k4X) {
    a.OpUBFE(dxbc::Dest::R(2, 0b0100), dxbc::Src::LU(1), dxbc::Src::LU(1), u);
    a.OpAnd(dxbc::Dest::R(2, 0b1000), v, dxbc::Src::LU(2));
    a.OpOr(dxbc::Dest::R(1, 0b0100), dxbc::Src::R(2, dxbc::Src::kZZZZ),
           dxbc::Src::R(2, dxbc::Src::kWWWW));
    sample_out = dxbc::Src::R(1, dxbc::Src::kZZZZ);
    a.OpUShR(dxbc::Dest::R(2, 0b0100), u, dxbc::Src::LU(2));
    a.OpBFI(dxbc::Dest::R(1, 0b0001), dxbc::Src::LU(31), dxbc::Src::LU(1),
            dxbc::Src::R(2, dxbc::Src::kZZZZ), u);
    a.OpUShR(dxbc::Dest::R(2, 0b1000), v, dxbc::Src::LU(2));
    a.OpBFI(dxbc::Dest::R(1, 0b0010), dxbc::Src::LU(31), dxbc::Src::LU(1),
            dxbc::Src::R(2, dxbc::Src::kWWWW), v);
    x_out = dxbc::Src::R(1, dxbc::Src::kXXXX);
    y_out = dxbc::Src::R(1, dxbc::Src::kYYYY);
  } else if (msaa_samples == xenos::MsaaSamples::k2X) {
    a.OpUBFE(dxbc::Dest::R(2, 0b0100), dxbc::Src::LU(1), dxbc::Src::LU(1), u);
    if (msaa_2x_supported) {
      a.OpXOr(dxbc::Dest::R(1, 0b0100), dxbc::Src::R(2, dxbc::Src::kZZZZ),
              dxbc::Src::LU(1));
    } else {
      a.OpBFI(dxbc::Dest::R(1, 0b0100), dxbc::Src::LU(1), dxbc::Src::LU(1),
              dxbc::Src::R(2, dxbc::Src::kZZZZ),
              dxbc::Src::R(2, dxbc::Src::kZZZZ));
    }
    sample_out = dxbc::Src::R(1, dxbc::Src::kZZZZ);
    a.OpUShR(dxbc::Dest::R(2, 0b1000), v, dxbc::Src::LU(1));
    a.OpBFI(dxbc::Dest::R(1, 0b0001), dxbc::Src::LU(1), dxbc::Src::LU(1),
            dxbc::Src::R(2, dxbc::Src::kWWWW), u);
    a.OpUShR(dxbc::Dest::R(2, 0b1000), v, dxbc::Src::LU(2));
    a.OpBFI(dxbc::Dest::R(1, 0b0010), dxbc::Src::LU(31), dxbc::Src::LU(1),
            dxbc::Src::R(2, dxbc::Src::kWWWW), v);
    x_out = dxbc::Src::R(1, dxbc::Src::kXXXX);
    y_out = dxbc::Src::R(1, dxbc::Src::kYYYY);
  }
  if (scaled) {
    a.OpUMAd(dxbc::Dest::R(1, 0b0011), dxbc::Src::R(1),
             dxbc::Src::LU(scale_x, scale_y, 1, 1), dxbc::Src::R(2));
    x_out = dxbc::Src::R(1, dxbc::Src::kXXXX);
    y_out = dxbc::Src::R(1, dxbc::Src::kYYYY);
  }
}

ID3D12PipelineState* const* D3D12RenderTargetCache::GetOrCreateTransferPipelines(
    TransferShaderKey key) {
  const TransferModeInfo& mode = kTransferModes[size_t(key.mode)];
  bool dest_is_stencil_bit = (mode.output == TransferOutput::kStencilBit);

  if (dest_is_stencil_bit) {
    auto pipelines_it = transfer_stencil_bit_pipelines_.find(key);
    if (pipelines_it != transfer_stencil_bit_pipelines_.end()) {
      return pipelines_it->second[0] ? pipelines_it->second.data() : nullptr;
    }
  } else {
    auto pipeline_it = transfer_pipelines_.find(key);
    if (pipeline_it != transfer_pipelines_.end()) {
      return pipeline_it->second ? &pipeline_it->second : nullptr;
    }
  }

  uint32_t rs = kTransferUsedRootParameters[size_t(use_stencil_reference_output_
                                                       ? mode.root_signature_with_stencil_ref
                                                       : mode.root_signature_no_stencil_ref)];

  // If not dest_is_color, it's depth, or stencil bit - 40-sample columns are
  // swapped as opposed to color source.
  bool dest_is_color = (mode.output == TransferOutput::kColor);

  xenos::ColorRenderTargetFormat dest_color_format =
      xenos::ColorRenderTargetFormat(key.dest_resource_format);
  xenos::DepthRenderTargetFormat dest_depth_format =
      xenos::DepthRenderTargetFormat(key.dest_resource_format);
  bool dest_is_64bpp = dest_is_color && xenos::IsColorRenderTargetFormat64bpp(dest_color_format);

  xenos::ColorRenderTargetFormat source_color_format =
      xenos::ColorRenderTargetFormat(key.source_resource_format);
  xenos::DepthRenderTargetFormat source_depth_format =
      xenos::DepthRenderTargetFormat(key.source_resource_format);
  // If not source_is_color, it's depth / stencil - 40-sample columns are
  // swapped as opposed to color destination.
  bool source_is_color = (rs & kTransferUsedRootParameterColorSRVBit) != 0;
  bool source_is_64bpp;
  uint32_t source_color_format_component_count;
  uint32_t source_color_srv_component_mask;
  bool source_color_is_uint;
  if (source_is_color) {
    assert_zero(rs & kTransferUsedRootParameterDepthSRVBit);
    assert_zero(rs & kTransferUsedRootParameterStencilSRVBit);
    source_is_64bpp = xenos::IsColorRenderTargetFormat64bpp(source_color_format);
    source_color_format_component_count =
        xenos::GetColorRenderTargetFormatComponentCount(source_color_format);
    if (dest_is_stencil_bit) {
      if (source_is_64bpp && !dest_is_64bpp) {
        // Need one component, but choosing from the two 32bpp halves of the
        // 64bpp sample.
        source_color_srv_component_mask = 0b1 | (0b1 << (source_color_format_component_count >> 1));
      } else {
        // Red is at least 8 bits per component in all formats.
        source_color_srv_component_mask = 0b1;
      }
    } else {
      source_color_srv_component_mask = (uint32_t(1) << source_color_format_component_count) - 1;
    }
    GetColorOwnershipTransferDXGIFormat(source_color_format, &source_color_is_uint);
  } else {
    source_is_64bpp = false;
    source_color_format_component_count = 0;
    source_color_srv_component_mask = 0;
    source_color_is_uint = false;
  }

  bool shader_uses_stencil_reference_output =
      mode.output == TransferOutput::kDepth && use_stencil_reference_output_;

  // Because of built_shader_.resize(), pointers can't be kept persistently
  // here! Resizing also zeroes the memory.

  built_shader_.clear();

  // RDEF, ISGN, OSGN, SHEX, optionally SFI0, STAT.
  uint32_t blob_count = 5 + uint32_t(shader_uses_stencil_reference_output);

  // Allocate space for the container header and the blob offsets.
  built_shader_.resize(sizeof(dxbc::ContainerHeader) / sizeof(uint32_t) + blob_count);
  uint32_t blob_offset_position_dwords = sizeof(dxbc::ContainerHeader) / sizeof(uint32_t);
  uint32_t blob_position_dwords = uint32_t(built_shader_.size());
  constexpr uint32_t kBlobHeaderSizeDwords = sizeof(dxbc::BlobHeader) / sizeof(uint32_t);

  uint32_t name_ptr;

  // ***************************************************************************
  // Resource definition
  // ***************************************************************************

  built_shader_[blob_offset_position_dwords] = uint32_t(blob_position_dwords * sizeof(uint32_t));
  uint32_t rdef_position_dwords = blob_position_dwords + kBlobHeaderSizeDwords;
  // Not needed, as the next operation done is resize, to allocate the space for
  // both the blob header and the resource definition header.
  // built_shader_.resize(rdef_position_dwords);

  // Allocate space for the RDEF header.
  built_shader_.resize(rdef_position_dwords + sizeof(dxbc::RdefHeader) / sizeof(uint32_t));
  // Generator name.
  dxbc::AppendAlignedString(built_shader_, "Xenia");

  // Constant types - uint (aka "dword" when it's scalar) only.
  // Names.
  name_ptr = uint32_t((built_shader_.size() - rdef_position_dwords) * sizeof(uint32_t));
  uint32_t rdef_dword_name_ptr = name_ptr;
  name_ptr += dxbc::AppendAlignedString(built_shader_, "dword");
  // Types.
  uint32_t rdef_type_uint_position_dwords = uint32_t(built_shader_.size());
  uint32_t rdef_type_uint_ptr =
      uint32_t((rdef_type_uint_position_dwords - rdef_position_dwords) * sizeof(uint32_t));
  built_shader_.resize(rdef_type_uint_position_dwords + sizeof(dxbc::RdefType) / sizeof(uint32_t));
  {
    auto& rdef_type_uint =
        *reinterpret_cast<dxbc::RdefType*>(built_shader_.data() + rdef_type_uint_position_dwords);
    rdef_type_uint.variable_class = dxbc::RdefVariableClass::kScalar;
    rdef_type_uint.variable_type = dxbc::RdefVariableType::kUInt;
    rdef_type_uint.row_count = 1;
    rdef_type_uint.column_count = 1;
    rdef_type_uint.name_ptr = rdef_dword_name_ptr;
  }

  // Constants, if needed:
  // - uint xe_transfer_stencil_mask
  // - uint xe_transfer_address
  // - uint xe_transfer_host_depth_address
  uint32_t rdef_constant_count = 0;
  uint32_t rdef_constant_index_stencil_mask =
      (rs & kTransferUsedRootParameterStencilMaskConstantBit) ? rdef_constant_count++ : UINT32_MAX;
  assert_false(dest_is_stencil_bit && rdef_constant_index_stencil_mask == UINT32_MAX);
  uint32_t rdef_constant_index_address =
      (rs & kTransferUsedRootParameterAddressConstantBit) ? rdef_constant_count++ : UINT32_MAX;
  assert_true(rdef_constant_index_address != UINT32_MAX);
  uint32_t rdef_constant_index_host_depth_address =
      (rs & kTransferUsedRootParameterHostDepthAddressConstantBit) ? rdef_constant_count++
                                                                   : UINT32_MAX;
  // Names.
  name_ptr = uint32_t((built_shader_.size() - rdef_position_dwords) * sizeof(uint32_t));
  uint32_t rdef_xe_transfer_stencil_mask_name_ptr = name_ptr;
  if (rdef_constant_index_stencil_mask != UINT32_MAX) {
    name_ptr += dxbc::AppendAlignedString(built_shader_, "xe_transfer_stencil_mask");
  }
  uint32_t rdef_xe_transfer_address_name_ptr = name_ptr;
  if (rdef_constant_index_address != UINT32_MAX) {
    name_ptr += dxbc::AppendAlignedString(built_shader_, "xe_transfer_address");
  }
  uint32_t rdef_xe_transfer_host_depth_address_name_ptr = name_ptr;
  if (rdef_constant_index_host_depth_address != UINT32_MAX) {
    name_ptr += dxbc::AppendAlignedString(built_shader_, "xe_transfer_host_depth_address");
  }
  // Constants.
  uint32_t rdef_constants_position_dwords = uint32_t(built_shader_.size());
  uint32_t rdef_constants_ptr =
      uint32_t((rdef_constants_position_dwords - rdef_position_dwords) * sizeof(uint32_t));
  built_shader_.resize(rdef_constants_position_dwords +
                       sizeof(dxbc::RdefVariable) / sizeof(uint32_t) * rdef_constant_count);
  {
    auto rdef_constants = reinterpret_cast<dxbc::RdefVariable*>(built_shader_.data() +
                                                                rdef_constants_position_dwords);
    // uint xe_transfer_stencil_mask
    if (rdef_constant_index_stencil_mask != UINT32_MAX) {
      dxbc::RdefVariable& rdef_constant_stencil_mask =
          rdef_constants[rdef_constant_index_stencil_mask];
      rdef_constant_stencil_mask.name_ptr = rdef_xe_transfer_stencil_mask_name_ptr;
      rdef_constant_stencil_mask.size_bytes = sizeof(uint32_t);
      rdef_constant_stencil_mask.flags = dxbc::kRdefVariableFlagUsed;
      rdef_constant_stencil_mask.type_ptr = rdef_type_uint_ptr;
      rdef_constant_stencil_mask.start_texture = UINT32_MAX;
      rdef_constant_stencil_mask.start_sampler = UINT32_MAX;
    }
    // uint xe_transfer_address
    if (rdef_constant_index_address != UINT32_MAX) {
      dxbc::RdefVariable& rdef_constant_address = rdef_constants[rdef_constant_index_address];
      rdef_constant_address.name_ptr = rdef_xe_transfer_address_name_ptr;
      rdef_constant_address.size_bytes = sizeof(uint32_t);
      rdef_constant_address.flags = dxbc::kRdefVariableFlagUsed;
      rdef_constant_address.type_ptr = rdef_type_uint_ptr;
      rdef_constant_address.start_texture = UINT32_MAX;
      rdef_constant_address.start_sampler = UINT32_MAX;
    }
    // uint xe_transfer_host_depth_address
    if (rdef_constant_index_host_depth_address != UINT32_MAX) {
      dxbc::RdefVariable& rdef_constant_host_depth_address =
          rdef_constants[rdef_constant_index_host_depth_address];
      rdef_constant_host_depth_address.name_ptr = rdef_xe_transfer_host_depth_address_name_ptr;
      rdef_constant_host_depth_address.size_bytes = sizeof(uint32_t);
      rdef_constant_host_depth_address.flags = dxbc::kRdefVariableFlagUsed;
      rdef_constant_host_depth_address.type_ptr = rdef_type_uint_ptr;
      rdef_constant_host_depth_address.start_texture = UINT32_MAX;
      rdef_constant_host_depth_address.start_sampler = UINT32_MAX;
    }
  }

  // Constant buffers, if needed:
  // - xe_transfer_stencil_mask { uint xe_transfer_stencil_mask; }
  // - xe_transfer_address { uint xe_transfer_address; }
  // - xe_transfer_host_depth_address { uint xe_transfer_host_depth_address; }
  // Reusing the constant names for constant buffers.
  uint32_t rdef_cbuffer_count = 0;
  uint32_t cbuffer_index_stencil_mask =
      rdef_constant_index_stencil_mask != UINT32_MAX ? rdef_cbuffer_count++ : UINT32_MAX;
  uint32_t cbuffer_index_address =
      rdef_constant_index_address != UINT32_MAX ? rdef_cbuffer_count++ : UINT32_MAX;
  uint32_t cbuffer_index_host_depth_address =
      rdef_constant_index_host_depth_address != UINT32_MAX ? rdef_cbuffer_count++ : UINT32_MAX;
  uint32_t rdef_cbuffer_position_dwords = uint32_t(built_shader_.size());
  built_shader_.resize(rdef_cbuffer_position_dwords +
                       sizeof(dxbc::RdefCbuffer) / sizeof(uint32_t) * rdef_cbuffer_count);
  {
    auto rdef_cbuffers =
        reinterpret_cast<dxbc::RdefCbuffer*>(built_shader_.data() + rdef_cbuffer_position_dwords);
    // xe_transfer_stencil_mask
    if (cbuffer_index_stencil_mask != UINT32_MAX) {
      dxbc::RdefCbuffer& rdef_cbuffer_stencil_mask = rdef_cbuffers[cbuffer_index_stencil_mask];
      rdef_cbuffer_stencil_mask.name_ptr = rdef_xe_transfer_stencil_mask_name_ptr;
      rdef_cbuffer_stencil_mask.variable_count = 1;
      rdef_cbuffer_stencil_mask.variables_ptr = uint32_t(
          rdef_constants_ptr + sizeof(dxbc::RdefVariable) * rdef_constant_index_stencil_mask);
      rdef_cbuffer_stencil_mask.size_vector_aligned_bytes = sizeof(uint32_t) * 4;
    }
    // xe_transfer_address
    if (cbuffer_index_address != UINT32_MAX) {
      dxbc::RdefCbuffer& rdef_cbuffer_address = rdef_cbuffers[cbuffer_index_address];
      rdef_cbuffer_address.name_ptr = rdef_xe_transfer_address_name_ptr;
      rdef_cbuffer_address.variable_count = 1;
      rdef_cbuffer_address.variables_ptr =
          uint32_t(rdef_constants_ptr + sizeof(dxbc::RdefVariable) * rdef_constant_index_address);
      rdef_cbuffer_address.size_vector_aligned_bytes = sizeof(uint32_t) * 4;
    }
    // xe_transfer_host_depth_address
    if (cbuffer_index_host_depth_address != UINT32_MAX) {
      dxbc::RdefCbuffer& rdef_cbuffer_host_depth_address =
          rdef_cbuffers[cbuffer_index_host_depth_address];
      rdef_cbuffer_host_depth_address.name_ptr = rdef_xe_transfer_host_depth_address_name_ptr;
      rdef_cbuffer_host_depth_address.variable_count = 1;
      rdef_cbuffer_host_depth_address.variables_ptr = uint32_t(
          rdef_constants_ptr + sizeof(dxbc::RdefVariable) * rdef_constant_index_host_depth_address);
      rdef_cbuffer_host_depth_address.size_vector_aligned_bytes = sizeof(uint32_t) * 4;
    }
  }

  // Bindings.
  // - Texture2D/Texture2DMS<floatN/uintN> xe_transfer_color
  // - Texture2D/Texture2DMS<float> xe_transfer_depth
  // - Texture2D/Texture2DMS<uint2> xe_transfer_stencil
  // - Texture2D<float>/Texture2DMS<float>/Buffer<uint> xe_transfer_host_depth
  // - Constant buffers
  uint32_t rdef_srv_count = 0;
  uint32_t srv_index_color =
      (rs & kTransferUsedRootParameterColorSRVBit) ? rdef_srv_count++ : UINT32_MAX;
  uint32_t srv_index_depth =
      (rs & kTransferUsedRootParameterDepthSRVBit) ? rdef_srv_count++ : UINT32_MAX;
  uint32_t srv_index_stencil =
      (rs & kTransferUsedRootParameterStencilSRVBit) ? rdef_srv_count++ : UINT32_MAX;
  uint32_t srv_index_host_depth =
      (rs & kTransferUsedRootParameterHostDepthSRVBit) ? rdef_srv_count++ : UINT32_MAX;
  uint32_t rdef_binding_count = rdef_srv_count + rdef_cbuffer_count;
  // Names.
  name_ptr = uint32_t((built_shader_.size() - rdef_position_dwords) * sizeof(uint32_t));
  uint32_t rdef_xe_transfer_color_name_ptr = name_ptr;
  if (srv_index_color != UINT32_MAX) {
    name_ptr += dxbc::AppendAlignedString(built_shader_, "xe_transfer_color");
  }
  uint32_t rdef_xe_transfer_depth_name_ptr = name_ptr;
  if (srv_index_depth != UINT32_MAX) {
    name_ptr += dxbc::AppendAlignedString(built_shader_, "xe_transfer_depth");
  }
  uint32_t rdef_xe_transfer_stencil_name_ptr = name_ptr;
  if (srv_index_stencil != UINT32_MAX) {
    name_ptr += dxbc::AppendAlignedString(built_shader_, "xe_transfer_stencil");
  }
  uint32_t rdef_xe_transfer_host_depth_name_ptr = name_ptr;
  if (srv_index_host_depth != UINT32_MAX) {
    name_ptr += dxbc::AppendAlignedString(built_shader_, "xe_transfer_host_depth");
  }
  // Bindings.
  uint32_t rdef_binding_position_dwords = uint32_t(built_shader_.size());
  built_shader_.resize(rdef_binding_position_dwords +
                       sizeof(dxbc::RdefInputBind) / sizeof(uint32_t) * rdef_binding_count);
  {
    auto rdef_bindings =
        reinterpret_cast<dxbc::RdefInputBind*>(built_shader_.data() + rdef_binding_position_dwords);
    uint32_t rdef_binding_index = 0;
    // xe_transfer_color
    if (srv_index_color != UINT32_MAX) {
      dxbc::RdefInputBind& rdef_binding_color = rdef_bindings[rdef_binding_index++];
      rdef_binding_color.name_ptr = rdef_xe_transfer_color_name_ptr;
      rdef_binding_color.type = dxbc::RdefInputType::kTexture;
      rdef_binding_color.return_type =
          source_color_is_uint ? dxbc::ResourceReturnType::kUInt : dxbc::ResourceReturnType::kFloat;
      if (key.source_msaa_samples != xenos::MsaaSamples::k1X) {
        rdef_binding_color.dimension = dxbc::RdefDimension::kSRVTexture2DMS;
      } else {
        rdef_binding_color.dimension = dxbc::RdefDimension::kSRVTexture2D;
        rdef_binding_color.sample_count = UINT32_MAX;
      }
      rdef_binding_color.bind_point = kTransferSRVRegisterColor;
      rdef_binding_color.bind_count = 1;
      assert_not_zero(source_color_srv_component_mask);
      rdef_binding_color.flags = (32 - rex::lzcnt(source_color_srv_component_mask) - 1)
                                 << dxbc::kRdefInputFlagsComponentsShift;
      rdef_binding_color.id = srv_index_color;
    }
    // xe_transfer_depth
    if (srv_index_depth != UINT32_MAX) {
      dxbc::RdefInputBind& rdef_binding_depth = rdef_bindings[rdef_binding_index++];
      rdef_binding_depth.name_ptr = rdef_xe_transfer_depth_name_ptr;
      rdef_binding_depth.type = dxbc::RdefInputType::kTexture;
      rdef_binding_depth.return_type = dxbc::ResourceReturnType::kFloat;
      if (key.source_msaa_samples != xenos::MsaaSamples::k1X) {
        rdef_binding_depth.dimension = dxbc::RdefDimension::kSRVTexture2DMS;
      } else {
        rdef_binding_depth.dimension = dxbc::RdefDimension::kSRVTexture2D;
        rdef_binding_depth.sample_count = UINT32_MAX;
      }
      rdef_binding_depth.bind_point = kTransferSRVRegisterDepth;
      rdef_binding_depth.bind_count = 1;
      rdef_binding_depth.id = srv_index_depth;
    }
    // xe_transfer_stencil
    if (srv_index_stencil != UINT32_MAX) {
      dxbc::RdefInputBind& rdef_binding_stencil = rdef_bindings[rdef_binding_index++];
      rdef_binding_stencil.name_ptr = rdef_xe_transfer_stencil_name_ptr;
      rdef_binding_stencil.type = dxbc::RdefInputType::kTexture;
      rdef_binding_stencil.return_type = dxbc::ResourceReturnType::kUInt;
      if (key.source_msaa_samples != xenos::MsaaSamples::k1X) {
        rdef_binding_stencil.dimension = dxbc::RdefDimension::kSRVTexture2DMS;
      } else {
        rdef_binding_stencil.dimension = dxbc::RdefDimension::kSRVTexture2D;
        rdef_binding_stencil.sample_count = UINT32_MAX;
      }
      rdef_binding_stencil.bind_point = kTransferSRVRegisterStencil;
      rdef_binding_stencil.bind_count = 1;
      rdef_binding_stencil.flags = dxbc::kRdefInputFlags2Component;
      rdef_binding_stencil.id = srv_index_stencil;
    }
    // xe_transfer_host_depth
    if (srv_index_host_depth != UINT32_MAX) {
      dxbc::RdefInputBind& rdef_binding_host_depth = rdef_bindings[rdef_binding_index++];
      rdef_binding_host_depth.name_ptr = rdef_xe_transfer_host_depth_name_ptr;
      rdef_binding_host_depth.type = dxbc::RdefInputType::kTexture;
      if (key.host_depth_source_is_copy) {
        // Float as uint.
        rdef_binding_host_depth.return_type = dxbc::ResourceReturnType::kUInt;
        rdef_binding_host_depth.dimension = dxbc::RdefDimension::kSRVBuffer;
        rdef_binding_host_depth.sample_count = UINT32_MAX;
      } else {
        rdef_binding_host_depth.return_type = dxbc::ResourceReturnType::kFloat;
        if (key.host_depth_source_msaa_samples != xenos::MsaaSamples::k1X) {
          rdef_binding_host_depth.dimension = dxbc::RdefDimension::kSRVTexture2DMS;
        } else {
          rdef_binding_host_depth.dimension = dxbc::RdefDimension::kSRVTexture2D;
          rdef_binding_host_depth.sample_count = UINT32_MAX;
        }
      }
      rdef_binding_host_depth.bind_point = kTransferSRVRegisterHostDepth;
      rdef_binding_host_depth.bind_count = 1;
      rdef_binding_host_depth.id = srv_index_host_depth;
    }
    // xe_transfer_stencil_mask
    if (cbuffer_index_stencil_mask != UINT32_MAX) {
      dxbc::RdefInputBind& rdef_binding_stencil_mask = rdef_bindings[rdef_binding_index++];
      rdef_binding_stencil_mask.name_ptr = rdef_xe_transfer_stencil_mask_name_ptr;
      rdef_binding_stencil_mask.type = dxbc::RdefInputType::kCbuffer;
      rdef_binding_stencil_mask.bind_point = kTransferCBVRegisterStencilMask;
      rdef_binding_stencil_mask.bind_count = 1;
      rdef_binding_stencil_mask.flags = dxbc::kRdefInputFlagUserPacked;
      rdef_binding_stencil_mask.id = cbuffer_index_stencil_mask;
    }
    // xe_transfer_address
    if (cbuffer_index_address != UINT32_MAX) {
      dxbc::RdefInputBind& rdef_binding_address = rdef_bindings[rdef_binding_index++];
      rdef_binding_address.name_ptr = rdef_xe_transfer_address_name_ptr;
      rdef_binding_address.type = dxbc::RdefInputType::kCbuffer;
      rdef_binding_address.bind_point = kTransferCBVRegisterAddress;
      rdef_binding_address.bind_count = 1;
      rdef_binding_address.flags = dxbc::kRdefInputFlagUserPacked;
      rdef_binding_address.id = cbuffer_index_address;
    }
    // xe_transfer_host_depth_address
    if (cbuffer_index_host_depth_address != UINT32_MAX) {
      dxbc::RdefInputBind& rdef_binding_host_depth_address = rdef_bindings[rdef_binding_index++];
      rdef_binding_host_depth_address.name_ptr = rdef_xe_transfer_host_depth_address_name_ptr;
      rdef_binding_host_depth_address.type = dxbc::RdefInputType::kCbuffer;
      rdef_binding_host_depth_address.bind_point = kTransferCBVRegisterHostDepthAddress;
      rdef_binding_host_depth_address.bind_count = 1;
      rdef_binding_host_depth_address.flags = dxbc::kRdefInputFlagUserPacked;
      rdef_binding_host_depth_address.id = cbuffer_index_host_depth_address;
    }
  }

  // Header.
  {
    auto& rdef_header =
        *reinterpret_cast<dxbc::RdefHeader*>(built_shader_.data() + rdef_position_dwords);
    rdef_header.cbuffer_count = rdef_cbuffer_count;
    rdef_header.cbuffers_ptr =
        uint32_t((rdef_cbuffer_position_dwords - rdef_position_dwords) * sizeof(uint32_t));
    rdef_header.input_bind_count = rdef_binding_count;
    rdef_header.input_binds_ptr =
        uint32_t((rdef_binding_position_dwords - rdef_position_dwords) * sizeof(uint32_t));
    rdef_header.shader_model = dxbc::RdefShaderModel::kPixelShader5_1;
    rdef_header.compile_flags =
        dxbc::kCompileFlagNoPreshader | dxbc::kCompileFlagPreferFlowControl |
        dxbc::kCompileFlagIeeeStrictness | dxbc::kCompileFlagAllResourcesBound;
    // Generator name is right after the header.
    rdef_header.generator_name_ptr = sizeof(dxbc::RdefHeader);
    rdef_header.fourcc = dxbc::RdefHeader::FourCC::k5_1;
    rdef_header.InitializeSizes();
  }

  {
    auto& blob_header =
        *reinterpret_cast<dxbc::BlobHeader*>(built_shader_.data() + blob_position_dwords);
    blob_header.fourcc = dxbc::BlobHeader::FourCC::kResourceDefinition;
    blob_position_dwords = uint32_t(built_shader_.size());
    blob_header.size_bytes = (blob_position_dwords - kBlobHeaderSizeDwords) * sizeof(uint32_t) -
                             built_shader_[blob_offset_position_dwords++];
  }

  // ***************************************************************************
  // Input signature
  // ***************************************************************************

  // Registers for accessing in the shader code - multiple inputs may be packed
  // into the same register.
  enum InputRegister : uint32_t {
    kInputRegisterPosition,
    kInputRegisterSampleIndex,
    kInputRegisterCount,
  };

  // Position, and for multisampled, sample index.
  uint32_t isgn_parameter_count = 1 + uint32_t(key.dest_msaa_samples != xenos::MsaaSamples::k1X);

  // Reserve space for the header and the parameters.
  built_shader_[blob_offset_position_dwords] = uint32_t(blob_position_dwords * sizeof(uint32_t));
  uint32_t isgn_position_dwords = blob_position_dwords + kBlobHeaderSizeDwords;
  built_shader_.resize(isgn_position_dwords + sizeof(dxbc::Signature) / sizeof(uint32_t) +
                       sizeof(dxbc::SignatureParameter) / sizeof(uint32_t) * isgn_parameter_count);

  // Names (after the parameters).
  name_ptr = uint32_t((built_shader_.size() - isgn_position_dwords) * sizeof(uint32_t));
  uint32_t isgn_sv_position_name_ptr = name_ptr;
  name_ptr += dxbc::AppendAlignedString(built_shader_, "SV_Position");
  uint32_t isgn_sv_sample_index_name_ptr = name_ptr;
  if (key.dest_msaa_samples != xenos::MsaaSamples::k1X) {
    name_ptr += dxbc::AppendAlignedString(built_shader_, "SV_SampleIndex");
  }

  // Header and parameters.
  {
    // Header.
    auto& isgn_header =
        *reinterpret_cast<dxbc::Signature*>(built_shader_.data() + isgn_position_dwords);
    isgn_header.parameter_count = isgn_parameter_count;
    isgn_header.parameter_info_ptr = sizeof(dxbc::Signature);
    // Parameters.
    auto isgn_parameters = reinterpret_cast<dxbc::SignatureParameter*>(
        built_shader_.data() + isgn_position_dwords + sizeof(dxbc::Signature) / sizeof(uint32_t));
    uint32_t isgn_parameter_index = 0;
    // SV_Position.xy
    dxbc::SignatureParameter& isgn_sv_position = isgn_parameters[isgn_parameter_index++];
    isgn_sv_position.semantic_name_ptr = isgn_sv_position_name_ptr;
    isgn_sv_position.system_value = dxbc::Name::kPosition;
    isgn_sv_position.component_type = dxbc::SignatureRegisterComponentType::kFloat32;
    isgn_sv_position.register_index = kInputRegisterPosition;
    isgn_sv_position.mask = 0b1111;
    isgn_sv_position.always_reads_mask = 0b0011;
    // SV_SampleIndex
    if (key.dest_msaa_samples != xenos::MsaaSamples::k1X) {
      dxbc::SignatureParameter& isgn_sv_sample_index = isgn_parameters[isgn_parameter_index++];
      isgn_sv_sample_index.semantic_name_ptr = isgn_sv_sample_index_name_ptr;
      isgn_sv_sample_index.system_value = dxbc::Name::kSampleIndex;
      isgn_sv_sample_index.component_type = dxbc::SignatureRegisterComponentType::kUInt32;
      isgn_sv_sample_index.register_index = kInputRegisterSampleIndex;
      isgn_sv_sample_index.mask = 0b0001;
      isgn_sv_sample_index.always_reads_mask = 0b0001;
    }
  }

  {
    auto& blob_header =
        *reinterpret_cast<dxbc::BlobHeader*>(built_shader_.data() + blob_position_dwords);
    blob_header.fourcc = dxbc::BlobHeader::FourCC::kInputSignature;
    blob_position_dwords = uint32_t(built_shader_.size());
    blob_header.size_bytes = (blob_position_dwords - kBlobHeaderSizeDwords) * sizeof(uint32_t) -
                             built_shader_[blob_offset_position_dwords++];
  }

  // ***************************************************************************
  // Output signature
  // ***************************************************************************

  // Color or depth.
  uint32_t osgn_parameter_count = 0;
  uint32_t osgn_parameter_index_sv_target =
      mode.output == TransferOutput::kColor ? osgn_parameter_count++ : UINT32_MAX;
  uint32_t osgn_parameter_index_sv_depth =
      mode.output == TransferOutput::kDepth ? osgn_parameter_count++ : UINT32_MAX;
  uint32_t osgn_parameter_index_sv_stencil_ref =
      shader_uses_stencil_reference_output ? osgn_parameter_count++ : UINT32_MAX;

  // Reserve space for the header and the parameters.
  built_shader_[blob_offset_position_dwords] = uint32_t(blob_position_dwords * sizeof(uint32_t));
  uint32_t osgn_position_dwords = blob_position_dwords + kBlobHeaderSizeDwords;
  built_shader_.resize(osgn_position_dwords + sizeof(dxbc::Signature) / sizeof(uint32_t) +
                       sizeof(dxbc::SignatureParameter) / sizeof(uint32_t) * osgn_parameter_count);

  // Names (after the parameters).
  name_ptr = uint32_t((built_shader_.size() - osgn_position_dwords) * sizeof(uint32_t));
  uint32_t osgn_sv_target_name_ptr = name_ptr;
  if (osgn_parameter_index_sv_target != UINT32_MAX) {
    name_ptr += dxbc::AppendAlignedString(built_shader_, "SV_Target");
  }
  uint32_t osgn_sv_depth_name_ptr = name_ptr;
  if (osgn_parameter_index_sv_depth != UINT32_MAX) {
    name_ptr += dxbc::AppendAlignedString(built_shader_, "SV_Depth");
  }
  uint32_t osgn_sv_stencil_ref_name_ptr = name_ptr;
  if (osgn_parameter_index_sv_stencil_ref != UINT32_MAX) {
    name_ptr += dxbc::AppendAlignedString(built_shader_, "SV_StencilRef");
  }

  bool dest_color_is_uint;
  if (mode.output == TransferOutput::kColor) {
    GetColorOwnershipTransferDXGIFormat(dest_color_format, &dest_color_is_uint);
  } else {
    dest_color_is_uint = false;
  }

  // Header and parameters.
  {
    // Header.
    auto& osgn_header =
        *reinterpret_cast<dxbc::Signature*>(built_shader_.data() + osgn_position_dwords);
    osgn_header.parameter_count = osgn_parameter_count;
    osgn_header.parameter_info_ptr = sizeof(dxbc::Signature);
    // Parameters.
    auto osgn_parameters = reinterpret_cast<dxbc::SignatureParameter*>(
        built_shader_.data() + osgn_position_dwords + sizeof(dxbc::Signature) / sizeof(uint32_t));
    // SV_Target
    if (osgn_parameter_index_sv_target != UINT32_MAX) {
      dxbc::SignatureParameter& osgn_sv_target = osgn_parameters[osgn_parameter_index_sv_target];
      osgn_sv_target.semantic_name_ptr = osgn_sv_target_name_ptr;
      osgn_sv_target.component_type = dest_color_is_uint
                                          ? dxbc::SignatureRegisterComponentType::kUInt32
                                          : dxbc::SignatureRegisterComponentType::kFloat32;
      osgn_sv_target.register_index = 0;
      osgn_sv_target.mask = 0b1111;
    }
    // SV_Depth
    if (osgn_parameter_index_sv_depth != UINT32_MAX) {
      dxbc::SignatureParameter& osgn_sv_depth = osgn_parameters[osgn_parameter_index_sv_depth];
      osgn_sv_depth.semantic_name_ptr = osgn_sv_depth_name_ptr;
      osgn_sv_depth.component_type = dxbc::SignatureRegisterComponentType::kFloat32;
      osgn_sv_depth.register_index = UINT32_MAX;
      osgn_sv_depth.mask = 0b0001;
      osgn_sv_depth.never_writes_mask = 0b1110;
    }
    // SV_StencilRef
    if (osgn_parameter_index_sv_stencil_ref != UINT32_MAX) {
      dxbc::SignatureParameter& osgn_sv_stencil_ref =
          osgn_parameters[osgn_parameter_index_sv_stencil_ref];
      osgn_sv_stencil_ref.semantic_name_ptr = osgn_sv_stencil_ref_name_ptr;
      // Older versions of FXC incorrectly expect SV_StencilRef to be float,
      // it's always uint in DXC and also in the latest versions of FXC.
      osgn_sv_stencil_ref.component_type = dxbc::SignatureRegisterComponentType::kUInt32;
      osgn_sv_stencil_ref.register_index = UINT32_MAX;
      osgn_sv_stencil_ref.mask = 0b0001;
      osgn_sv_stencil_ref.never_writes_mask = 0b1110;
    }
  }

  {
    auto& blob_header =
        *reinterpret_cast<dxbc::BlobHeader*>(built_shader_.data() + blob_position_dwords);
    blob_header.fourcc = dxbc::BlobHeader::FourCC::kOutputSignature;
    blob_position_dwords = uint32_t(built_shader_.size());
    blob_header.size_bytes = (blob_position_dwords - kBlobHeaderSizeDwords) * sizeof(uint32_t) -
                             built_shader_[blob_offset_position_dwords++];
  }

  // ***************************************************************************
  // Shader program
  // ***************************************************************************

  built_shader_[blob_offset_position_dwords] = uint32_t(blob_position_dwords * sizeof(uint32_t));
  uint32_t shex_position_dwords = blob_position_dwords + kBlobHeaderSizeDwords;
  built_shader_.resize(shex_position_dwords);

  built_shader_.push_back(dxbc::VersionToken(dxbc::ProgramType::kPixelShader, 5, 1));
  // Reserve space for the length token.
  built_shader_.push_back(0);

  dxbc::Statistics stat;
  std::memset(&stat, 0, sizeof(dxbc::Statistics));
  dxbc::Assembler a(built_shader_, stat);

  a.OpDclGlobalFlags(dxbc::kGlobalFlagAllResourcesBound);
  if (cbuffer_index_stencil_mask != UINT32_MAX) {
    a.OpDclConstantBuffer(
        dxbc::Src::CB(dxbc::Src::Dcl, cbuffer_index_stencil_mask, kTransferCBVRegisterStencilMask,
                      kTransferCBVRegisterStencilMask),
        1);
  }
  if (cbuffer_index_address != UINT32_MAX) {
    a.OpDclConstantBuffer(dxbc::Src::CB(dxbc::Src::Dcl, cbuffer_index_address,
                                        kTransferCBVRegisterAddress, kTransferCBVRegisterAddress),
                          1);
  }
  if (cbuffer_index_host_depth_address != UINT32_MAX) {
    a.OpDclConstantBuffer(
        dxbc::Src::CB(dxbc::Src::Dcl, cbuffer_index_host_depth_address,
                      kTransferCBVRegisterHostDepthAddress, kTransferCBVRegisterHostDepthAddress),
        1);
  }
  if (srv_index_color != UINT32_MAX) {
    a.OpDclResource(
        key.source_msaa_samples != xenos::MsaaSamples::k1X ? dxbc::ResourceDimension::kTexture2DMS
                                                           : dxbc::ResourceDimension::kTexture2D,
        dxbc::ResourceReturnTypeX4Token(source_color_is_uint ? dxbc::ResourceReturnType::kUInt
                                                             : dxbc::ResourceReturnType::kFloat),
        dxbc::Src::T(dxbc::Src::Dcl, srv_index_color, kTransferSRVRegisterColor,
                     kTransferSRVRegisterColor));
  }
  if (srv_index_depth != UINT32_MAX) {
    a.OpDclResource(key.source_msaa_samples != xenos::MsaaSamples::k1X
                        ? dxbc::ResourceDimension::kTexture2DMS
                        : dxbc::ResourceDimension::kTexture2D,
                    dxbc::ResourceReturnTypeX4Token(dxbc::ResourceReturnType::kFloat),
                    dxbc::Src::T(dxbc::Src::Dcl, srv_index_depth, kTransferSRVRegisterDepth,
                                 kTransferSRVRegisterDepth));
  }
  if (srv_index_stencil != UINT32_MAX) {
    a.OpDclResource(key.source_msaa_samples != xenos::MsaaSamples::k1X
                        ? dxbc::ResourceDimension::kTexture2DMS
                        : dxbc::ResourceDimension::kTexture2D,
                    dxbc::ResourceReturnTypeX4Token(dxbc::ResourceReturnType::kUInt),
                    dxbc::Src::T(dxbc::Src::Dcl, srv_index_stencil, kTransferSRVRegisterStencil,
                                 kTransferSRVRegisterStencil));
  }
  if (srv_index_host_depth != UINT32_MAX) {
    a.OpDclResource(key.host_depth_source_is_copy
                        ? dxbc::ResourceDimension::kBuffer
                        : (key.host_depth_source_msaa_samples != xenos::MsaaSamples::k1X
                               ? dxbc::ResourceDimension::kTexture2DMS
                               : dxbc::ResourceDimension::kTexture2D),
                    dxbc::ResourceReturnTypeX4Token(dxbc::ResourceReturnType::kFloat),
                    dxbc::Src::T(dxbc::Src::Dcl, srv_index_host_depth,
                                 kTransferSRVRegisterHostDepth, kTransferSRVRegisterHostDepth));
  }
  a.OpDclInputPSSIV(dxbc::InterpolationMode::kLinearNoPerspective,
                    dxbc::Dest::V1D(kInputRegisterPosition, 0b0011), dxbc::Name::kPosition);
  if (key.dest_msaa_samples != xenos::MsaaSamples::k1X) {
    a.OpDclInputPSSGV(dxbc::Dest::V1D(kInputRegisterSampleIndex, 0b0001), dxbc::Name::kSampleIndex);
  }
  if (osgn_parameter_index_sv_target != UINT32_MAX) {
    a.OpDclOutput(dxbc::Dest::O(0));
  }
  if (osgn_parameter_index_sv_depth != UINT32_MAX) {
    a.OpDclOutput(dxbc::Dest::ODepth());
  }
  if (osgn_parameter_index_sv_stencil_ref != UINT32_MAX) {
    a.OpDclOutput(dxbc::Dest::OStencilRef());
  }
  // r0:r2 are involved at least in common addressing code. Texture loads
  // usually can overwrite some of the addressing temps as they are only needed
  // for the coordinates for that load. Cross-class transfers keep the original
  // destination coordinates in one additional temp for host-depth access.
  bool cross_scale_class = key.source_scale_native != key.dest_scale_native;
  a.OpDclTemps(3 + uint32_t(cross_scale_class));

  uint32_t dest_scale_x =
      key.dest_scale_native ? 1 : this->draw_resolution_scale_x();
  uint32_t dest_scale_y =
      key.dest_scale_native ? 1 : this->draw_resolution_scale_y();
  uint32_t source_scale_x =
      key.source_scale_native ? 1 : this->draw_resolution_scale_x();
  uint32_t source_scale_y =
      key.source_scale_native ? 1 : this->draw_resolution_scale_y();
  uint32_t dest_tile_width_samples =
      xenos::kEdramTileWidthSamples * dest_scale_x;
  uint32_t dest_tile_height_samples =
      xenos::kEdramTileHeightSamples * dest_scale_y;
  uint32_t source_tile_width_samples =
      xenos::kEdramTileWidthSamples * source_scale_x;
  uint32_t source_tile_height_samples =
      xenos::kEdramTileHeightSamples * source_scale_y;

  // Split the destination pixel index into 32bpp tile in r0.zw and
  // 32bpp-tile-relative pixel index in r0.xy.
  // r0.xy = pixel XY as uint
  a.OpFToU(dxbc::Dest::R(0, 0b0011), dxbc::Src::V1D(kInputRegisterPosition));
  uint32_t dest_tile_width_pixels =
      dest_tile_width_samples >>
      (uint32_t(dest_is_64bpp) + uint32_t(key.dest_msaa_samples >= xenos::MsaaSamples::k4X));
  uint32_t dest_tile_height_pixels =
      dest_tile_height_samples >>
      uint32_t(key.dest_msaa_samples >= xenos::MsaaSamples::k2X);
  // r0.xy = destination pixel XY index within the 32bpp tile
  // r0.zw = 32bpp tile XY index
  a.OpUDiv(dxbc::Dest::R(0, 0b1100), dxbc::Dest::R(0, 0b0011), dxbc::Src::R(0, 0b01000100),
           dxbc::Src::LU(dest_tile_width_pixels, dest_tile_height_pixels, dest_tile_width_pixels,
                         dest_tile_height_pixels));

  // r1.x = destination pitch in 32bpp tiles
  a.OpUBFE(dxbc::Dest::R(1, 0b0001), dxbc::Src::LU(xenos::kEdramPitchTilesBits), dxbc::Src::LU(0),
           dxbc::Src::CB(cbuffer_index_address, kTransferCBVRegisterAddress, 0, dxbc::Src::kXXXX));
  // r0.z = 32bpp tile index relative to the destination base
  // r0.w = free
  // r1.x = free
  a.OpUMAd(dxbc::Dest::R(0, 0b0100), dxbc::Src::R(1, dxbc::Src::kXXXX),
           dxbc::Src::R(0, dxbc::Src::kWWWW), dxbc::Src::R(0, dxbc::Src::kZZZZ));

  if (cross_scale_class) {
    // Convert tile-local destination coordinates to source scale space. The
    // remaining address remapping assumes both sides use one scale.
    a.OpMov(dxbc::Dest::R(3, 0b0011), dxbc::Src::R(0));
    if (key.dest_scale_native) {
      // Native destination reading a scaled source: choose the center host
      // sample corresponding to each guest pixel.
      a.OpUMAd(dxbc::Dest::R(0, 0b0011),
               dxbc::Src::LU(source_scale_x, source_scale_y, 0, 0),
               dxbc::Src::R(0),
               dxbc::Src::LU(source_scale_x >> 1, source_scale_y >> 1, 0,
                             0));
    } else {
      // Scaled destination reading a native source: duplicate guest pixels.
      a.OpUDiv(dxbc::Dest::R(0, 0b0011), dxbc::Dest::Null(),
               dxbc::Src::R(0),
               dxbc::Src::LU(dest_scale_x, dest_scale_y, 1, 1));
    }
  }

  // Now the tile index doesn't have any dependencies on the destination. The
  // dword index within the source tile, however, is calculated from both the
  // source and the destination pixel size, sample count and color vs. depth.

  // Source can be 64bpp or 32bpp - depth if only depth is available, color in
  // all other cases.

  // Load the source to r1 (or low to r0, high to r1 if need 64bpp color as the
  // result, as the address is loaded to r1).

  // Source pixel and sample index within the 32bpp tile.
  // X to r1.x (or keep r0.x if not modifying).
  // Y to r1.y (or keep r0.y if not modifying).
  // Sample index to r1.z (or use v# if not modifying); r1.z will also be set
  // to 0 before sampling for the LOD of the single-sampled source (needs to
  // be in the register).
  // If 64bpp -> 32bpp, also the needed half in r0.w.

  dxbc::Src dest_sample(dxbc::Src::V1D(kInputRegisterSampleIndex, dxbc::Src::kXXXX));
  dxbc::Src source_sample(dest_sample);
  uint32_t source_tile_pixel_x_reg = 0;
  uint32_t source_tile_pixel_y_reg = 0;
  // Remap destination samples to source samples through canonical EDRAM sample
  // coordinates. All 1x, 2x and 4x views of an allocation share this layout.
  if (key.source_msaa_samples != key.dest_msaa_samples ||
      source_is_64bpp != dest_is_64bpp) {
    dxbc::Src canonical_u(dxbc::Src::R(0, dxbc::Src::kXXXX));
    dxbc::Src canonical_v(dxbc::Src::R(0, dxbc::Src::kYYYY));
    bool canonical_scaled;
    CanonicalizeSample(a, key.dest_msaa_samples, dest_sample,
                       msaa_2x_supported_, source_scale_x, source_scale_y,
                       canonical_u, canonical_v, canonical_scaled);
    if (dest_is_64bpp && !source_is_64bpp) {
      // A 64bpp destination sample is formed from two adjacent canonical
      // 32bpp source columns.
      a.OpIShL(dxbc::Dest::R(1, 0b0001), canonical_u, dxbc::Src::LU(1));
      canonical_u = dxbc::Src::R(1, dxbc::Src::kXXXX);
    } else if (!dest_is_64bpp && source_is_64bpp) {
      // Select one half of the 64bpp source sample.
      a.OpAnd(dxbc::Dest::R(0, 0b1000), canonical_u, dxbc::Src::LU(1));
      a.OpUShR(dxbc::Dest::R(1, 0b0001), canonical_u, dxbc::Src::LU(1));
      canonical_u = dxbc::Src::R(1, dxbc::Src::kXXXX);
    }
    dxbc::Src source_pixel_x(canonical_u);
    dxbc::Src source_pixel_y(canonical_v);
    DecanonicalizeSample(a, key.source_msaa_samples, canonical_u, canonical_v,
                         canonical_scaled, msaa_2x_supported_, source_scale_x,
                         source_scale_y, source_pixel_x, source_pixel_y,
                         source_sample);
    source_tile_pixel_x_reg = 1;
    source_tile_pixel_y_reg =
        (key.source_msaa_samples == xenos::MsaaSamples::k1X &&
         key.dest_msaa_samples == xenos::MsaaSamples::k1X && !canonical_scaled)
            ? 0
            : 1;
  }
  uint32_t source_pixel_width_dwords_log2 =
      uint32_t(key.source_msaa_samples >= xenos::MsaaSamples::k4X) + uint32_t(source_is_64bpp);

  if (source_is_color != dest_is_color) {
    // Copying between color and depth / stencil - swap 40-32bpp-sample columns
    // in the pixel index within the source 32bpp tile using r1.w as temporary.
    uint32_t source_32bpp_tile_half_pixels =
        source_tile_width_samples >> (1 + source_pixel_width_dwords_log2);
    a.OpULT(dxbc::Dest::R(1, 0b1000), dxbc::Src::R(source_tile_pixel_x_reg, dxbc::Src::kXXXX),
            dxbc::Src::LU(source_32bpp_tile_half_pixels));
    a.OpMovC(dxbc::Dest::R(1, 0b1000), dxbc::Src::R(1, dxbc::Src::kWWWW),
             dxbc::Src::LI(int32_t(source_32bpp_tile_half_pixels)),
             dxbc::Src::LI(-int32_t(source_32bpp_tile_half_pixels)));
    a.OpIAdd(dxbc::Dest::R(1, 0b0001), dxbc::Src::R(source_tile_pixel_x_reg, dxbc::Src::kXXXX),
             dxbc::Src::R(1, dxbc::Src::kWWWW));
    source_tile_pixel_x_reg = 1;
    // r1.w = free
  }

  // Current register allocation:
  // r0.xy = pixel index within the destination 32bpp tile
  // r0.z = 32bpp tile index relative to the destination base
  // r0.w for 64bpp -> 32bpp - needed 32bpp half index of 64bpp data
  // r1.xy = pixel index within the source 32bpp tile
  // r1.z for 2x/4x -> = sample index within the source pixel

  // Apply the source 32bpp tile index.
  // r1.w = destination to source EDRAM tile adjustment
  a.OpIBFE(dxbc::Dest::R(1, 0b1000), dxbc::Src::LU(xenos::kEdramBaseTilesBits + 1),
           dxbc::Src::LU(xenos::kEdramPitchTilesBits * 2),
           dxbc::Src::CB(cbuffer_index_address, kTransferCBVRegisterAddress, 0, dxbc::Src::kXXXX));
  // r1.w = 32bpp tile index within the source, or the tile index within the
  //        source minus the EDRAM tile count if transferring across addressing
  //        wrapping (if negative)
  a.OpIAdd(dxbc::Dest::R(1, 0b1000), dxbc::Src::R(0, dxbc::Src::kZZZZ),
           dxbc::Src::R(1, dxbc::Src::kWWWW));
  // r1.w = 32bpp tile index within the source
  a.OpAnd(dxbc::Dest::R(1, 0b1000), dxbc::Src::R(1, dxbc::Src::kWWWW),
          dxbc::Src::LU(xenos::kEdramTileCount - 1));
  // r2.x = source pitch in 32bpp tiles
  a.OpUBFE(dxbc::Dest::R(2, 0b0001), dxbc::Src::LU(xenos::kEdramPitchTilesBits),
           dxbc::Src::LU(xenos::kEdramPitchTilesBits),
           dxbc::Src::CB(cbuffer_index_address, kTransferCBVRegisterAddress, 0, dxbc::Src::kXXXX));
  // r1.w = source tile row
  // r2.x = source 32bpp tile within the row
  a.OpUDiv(dxbc::Dest::R(1, 0b1000), dxbc::Dest::R(2, 0b0001), dxbc::Src::R(1, dxbc::Src::kWWWW),
           dxbc::Src::R(2, dxbc::Src::kXXXX));
  // r1.x = pixel X within the source texture
  // r2.x = free
  a.OpUMAd(dxbc::Dest::R(1, 0b0001),
           dxbc::Src::LU(source_tile_width_samples >>
                         source_pixel_width_dwords_log2),
           dxbc::Src::R(2, dxbc::Src::kXXXX),
           dxbc::Src::R(source_tile_pixel_x_reg, dxbc::Src::kXXXX));
  // r1.y = pixel Y within the source texture
  // r1.w = free
  a.OpUMAd(dxbc::Dest::R(1, 0b0010),
           dxbc::Src::LU(source_tile_height_samples >>
                         uint32_t(key.source_msaa_samples >= xenos::MsaaSamples::k2X)),
           dxbc::Src::R(1, dxbc::Src::kWWWW),
           dxbc::Src::R(source_tile_pixel_y_reg, dxbc::Src::kYYYY));

  // Load the source to r1, or, for 32bpp | 32bpp -> 64bpp, the first dword to
  // r0 since addressing will not be needed anymore for color, and the second
  // dword to r1.
  // Depth will be loaded to w before loading stencil (so it doesn't overwrite
  // the coordinates needed for stencil loading).
  // Stencil will be loaded to x.
  // Color will be loaded to x...w.
  bool source_load_is_two_dwords = !source_is_64bpp && dest_is_64bpp;
  if (key.source_msaa_samples != xenos::MsaaSamples::k1X) {
    for (uint32_t i = 0; i <= uint32_t(source_load_is_two_dwords); ++i) {
      uint32_t source_load_register = source_load_is_two_dwords ? i : 1;
      if (srv_index_depth != UINT32_MAX) {
        a.OpLdMS(dxbc::Dest::R(source_load_register, 0b1000), dxbc::Src::R(1), 0b0011,
                 dxbc::Src::T(srv_index_depth, kTransferSRVRegisterDepth, dxbc::Src::kXXXX),
                 source_sample);
      }
      if (srv_index_stencil != UINT32_MAX) {
        a.OpLdMS(dxbc::Dest::R(source_load_register, 0b0001), dxbc::Src::R(1), 0b0011,
                 dxbc::Src::T(srv_index_stencil, kTransferSRVRegisterStencil, dxbc::Src::kYYYY),
                 source_sample);
      } else if (srv_index_color != UINT32_MAX) {
        a.OpLdMS(dxbc::Dest::R(source_load_register, source_color_srv_component_mask),
                 dxbc::Src::R(1), 0b0011, dxbc::Src::T(srv_index_color, kTransferSRVRegisterColor),
                 source_sample);
      }
      if (source_load_is_two_dwords && !i) {
        // The high 32bpp half of a 64bpp destination sample is the same
        // sample of the horizontally adjacent source pixel in canonical space.
        a.OpIAdd(dxbc::Dest::R(1, 0b0001),
                 dxbc::Src::R(1, dxbc::Src::kXXXX),
                 dxbc::Src::LU(source_scale_x));
      }
    }
  } else {
    // Write zero to the LOD index in r1.z.
    a.OpMov(dxbc::Dest::R(1, 0b0100), dxbc::Src::LU(0));
    dxbc::Src source_coordinates(dxbc::Src::R(1, 0b10000100));
    for (uint32_t i = 0; i <= uint32_t(source_load_is_two_dwords); ++i) {
      uint32_t source_load_register = source_load_is_two_dwords ? i : 1;
      if (srv_index_depth != UINT32_MAX) {
        a.OpLd(dxbc::Dest::R(source_load_register, 0b1000), source_coordinates, 0b1011,
               dxbc::Src::T(srv_index_depth, kTransferSRVRegisterDepth, dxbc::Src::kXXXX));
      }
      if (srv_index_stencil != UINT32_MAX) {
        a.OpLd(dxbc::Dest::R(source_load_register, 0b0001), source_coordinates, 0b1011,
               dxbc::Src::T(srv_index_stencil, kTransferSRVRegisterStencil, dxbc::Src::kYYYY));
      } else if (srv_index_color != UINT32_MAX) {
        a.OpLd(dxbc::Dest::R(source_load_register, source_color_srv_component_mask),
               source_coordinates, 0b1011,
               dxbc::Src::T(srv_index_color, kTransferSRVRegisterColor));
      }
      if (source_load_is_two_dwords && !i) {
        a.OpIAdd(dxbc::Dest::R(1, 0b0001),
                 dxbc::Src::R(1, dxbc::Src::kXXXX),
                 dxbc::Src::LU(source_scale_x));
      }
    }
  }
  // Pick the needed 32bpp half of the 64bpp color based on r0.w.
  if (source_is_64bpp && !dest_is_64bpp) {
    uint32_t source_color_half_component_count = source_color_format_component_count >> 1;
    if (dest_is_stencil_bit) {
      a.OpMovC(dxbc::Dest::R(1, 0b0001), dxbc::Src::R(0, dxbc::Src::kWWWW),
               dxbc::Src::R(1).Select(source_color_half_component_count),
               dxbc::Src::R(1, dxbc::Src::kXXXX));
    } else {
      uint32_t color_high_dword_swizzle =
          (source_color_half_component_count * 0b01010101) &
          ~((uint32_t(1) << (source_color_half_component_count * 2)) - 1);
      for (uint32_t i = 0; i < source_color_half_component_count; ++i) {
        color_high_dword_swizzle |= (source_color_half_component_count + i) << (i * 2);
      }
      a.OpMovC(dxbc::Dest::R(1, (1 << source_color_half_component_count) - 1),
               dxbc::Src::R(0, dxbc::Src::kWWWW), dxbc::Src::R(1, color_high_dword_swizzle),
               dxbc::Src::R(1));
    }
  }

  if (osgn_parameter_index_sv_stencil_ref != UINT32_MAX && srv_index_stencil != UINT32_MAX) {
    // For the depth -> depth case, write the stencil loaded to r1.x directly to
    // the output.
    assert_true(mode.output == TransferOutput::kDepth);
    a.OpMov(dxbc::Dest::OStencilRef(), dxbc::Src::R(1, dxbc::Src::kXXXX));
  }

  if (dest_is_64bpp) {
    // Handle construction of 64bpp color, either from two 32-bit samples in r0
    // and r1, or from one 64bpp sample in r1. Using r2.x as temporary when
    // needed.
    // If color_packed_in_r0x_and_r1x, use the generic path for combining two
    // 32-bit samples - as raw in r0.x and r1.x - into the destination.
    bool color_packed_in_r0x_and_r1x = false;
    if (source_is_color) {
      switch (source_color_format) {
        case xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA: {
          // 8_8_8_8_GAMMA is represented by linear stored in
          // R16G16B16A16_UNORM.
          for (uint32_t i = 0; i < 2; ++i) {
            for (uint32_t j = 0; j < 3; ++j) {
              DxbcShaderTranslator::PreSaturatedLinearToPWLGamma(a, i, j, i, j, 2, 0, 2, 1);
            }
          }
        }
          [[fallthrough]];
        case xenos::ColorRenderTargetFormat::k_8_8_8_8: {
          color_packed_in_r0x_and_r1x = true;
          for (uint32_t i = 0; i < 2; ++i) {
            a.OpMAd(dxbc::Dest::R(i), dxbc::Src::R(i), dxbc::Src::LF(255.0f), dxbc::Src::LF(0.5f));
            a.OpFToU(dxbc::Dest::R(i), dxbc::Src::R(i));
            for (uint32_t j = 1; j < 4; ++j) {
              a.OpBFI(dxbc::Dest::R(i, 0b0001), dxbc::Src::LU(8), dxbc::Src::LU(j * 8),
                      dxbc::Src::R(i).Select(j), dxbc::Src::R(i, dxbc::Src::kXXXX));
            }
          }
        } break;
        case xenos::ColorRenderTargetFormat::k_2_10_10_10:
        case xenos::ColorRenderTargetFormat::k_2_10_10_10_AS_10_10_10_10: {
          color_packed_in_r0x_and_r1x = true;
          for (uint32_t i = 0; i < 2; ++i) {
            a.OpMAd(dxbc::Dest::R(i), dxbc::Src::R(i),
                    dxbc::Src::LF(1023.0f, 1023.0f, 1023.0f, 3.0f), dxbc::Src::LF(0.5f));
            a.OpFToU(dxbc::Dest::R(i), dxbc::Src::R(i));
            for (uint32_t j = 1; j < 4; ++j) {
              a.OpBFI(dxbc::Dest::R(i, 0b0001), dxbc::Src::LU(j == 3 ? 2 : 10),
                      dxbc::Src::LU(j * 10), dxbc::Src::R(i).Select(j),
                      dxbc::Src::R(i, dxbc::Src::kXXXX));
            }
          }
        } break;
        case xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT:
        case xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT_AS_16_16_16_16: {
          color_packed_in_r0x_and_r1x = true;
          for (uint32_t i = 0; i < 2; ++i) {
            // Float16 has a wider range for both color and alpha, also NaNs -
            // clamp and convert.
            for (uint32_t j = 0; j < 3; ++j) {
              DxbcShaderTranslator::UnclampedFloat32To7e3(a, i, j, i, j, 2, 0);
              if (j) {
                a.OpBFI(dxbc::Dest::R(i, 0b0001), dxbc::Src::LU(10), dxbc::Src::LU(j * 10),
                        dxbc::Src::R(i).Select(j), dxbc::Src::R(i, dxbc::Src::kXXXX));
              }
            }
            // Saturate and convert the alpha.
            a.OpMov(dxbc::Dest::R(i, 0b1000), dxbc::Src::R(i, dxbc::Src::kWWWW), true);
            a.OpMAd(dxbc::Dest::R(i, 0b1000), dxbc::Src::R(i, dxbc::Src::kWWWW),
                    dxbc::Src::LF(3.0f), dxbc::Src::LF(0.5f));
            a.OpFToU(dxbc::Dest::R(i, 0b1000), dxbc::Src::R(i, dxbc::Src::kWWWW));
            a.OpBFI(dxbc::Dest::R(i, 0b0001), dxbc::Src::LU(2), dxbc::Src::LU(30),
                    dxbc::Src::R(i, dxbc::Src::kWWWW), dxbc::Src::R(i, dxbc::Src::kXXXX));
          }
        } break;
        // All 64bpp formats, and all 16 bits per component formats, are
        // represented as integers in ownership transfer for safe handling of
        // NaNs and -32768 / -32767.
        case xenos::ColorRenderTargetFormat::k_16_16:
        case xenos::ColorRenderTargetFormat::k_16_16_FLOAT: {
          if (dest_color_format == xenos::ColorRenderTargetFormat::k_32_32_FLOAT) {
            for (uint32_t i = 0; i < 2; ++i) {
              a.OpBFI(dxbc::Dest::O(0, 1 << i), dxbc::Src::LU(16), dxbc::Src::LU(16),
                      dxbc::Src::R(i, dxbc::Src::kYYYY), dxbc::Src::R(i, dxbc::Src::kXXXX));
            }
          } else {
            a.OpMov(dxbc::Dest::O(0, 0b0011), dxbc::Src::R(0));
            a.OpMov(dxbc::Dest::O(0, 0b1100), dxbc::Src::R(1, 0b0100 << 4));
          }
        } break;
        case xenos::ColorRenderTargetFormat::k_16_16_16_16:
        case xenos::ColorRenderTargetFormat::k_16_16_16_16_FLOAT: {
          if (dest_color_format == xenos::ColorRenderTargetFormat::k_32_32_FLOAT) {
            a.OpBFI(dxbc::Dest::O(0, 0b0011), dxbc::Src::LU(16), dxbc::Src::LU(16),
                    dxbc::Src::R(1, 0b1101), dxbc::Src::R(1, 0b1000));
          } else {
            a.OpMov(dxbc::Dest::O(0), dxbc::Src::R(1));
          }
        } break;
        case xenos::ColorRenderTargetFormat::k_32_FLOAT: {
          color_packed_in_r0x_and_r1x = true;
        } break;
        case xenos::ColorRenderTargetFormat::k_32_32_FLOAT: {
          if (dest_color_format == xenos::ColorRenderTargetFormat::k_32_32_FLOAT) {
            a.OpMov(dxbc::Dest::O(0, 0b0011), dxbc::Src::R(1));
          } else {
            a.OpUBFE(dxbc::Dest::O(0), dxbc::Src::LU(16), dxbc::Src::LU(0, 16, 0, 16),
                     dxbc::Src::R(1, 0b01010000));
          }
        } break;
      }
    } else {
      assert_not_zero(rs & kTransferUsedRootParameterDepthSRVBit);
      color_packed_in_r0x_and_r1x = true;
      for (uint32_t i = 0; i < 2; ++i) {
        switch (source_depth_format) {
          case xenos::DepthRenderTargetFormat::kD24S8: {
            // Round to the nearest even integer. This seems to be the correct
            // conversion, adding +0.5 and rounding towards zero results in red
            // instead of black in the 4D5307E6 clear shader.
            a.OpMul(dxbc::Dest::R(i, 0b1000), dxbc::Src::R(i, dxbc::Src::kWWWW),
                    dxbc::Src::LF(float(0xFFFFFF)));
            a.OpRoundNE(dxbc::Dest::R(i, 0b1000), dxbc::Src::R(i, dxbc::Src::kWWWW));
            a.OpFToU(dxbc::Dest::R(i, 0b1000), dxbc::Src::R(i, dxbc::Src::kWWWW));
          } break;
          case xenos::DepthRenderTargetFormat::kD24FS8: {
            // Convert using r1.y as temporary.
            // When converting the depth in pixel shaders, it's always exact,
            // truncating not to insert additional rounding instructions.
            DxbcShaderTranslator::PreClampedDepthTo20e4(
                a, i, 3, i, 3, 1, 1,
                !depth_float24_convert_in_pixel_shader() && depth_float24_round(), true);
          } break;
        }
        // Merge depth and stencil into r0/r1.x.
        a.OpBFI(dxbc::Dest::R(i, 0b0001), dxbc::Src::LU(24), dxbc::Src::LU(8),
                dxbc::Src::R(i, dxbc::Src::kWWWW), dxbc::Src::R(i, dxbc::Src::kXXXX));
      }
    }
    if (color_packed_in_r0x_and_r1x) {
      if (dest_color_format == xenos::ColorRenderTargetFormat::k_32_32_FLOAT) {
        a.OpMov(dxbc::Dest::O(0, 0b0001), dxbc::Src::R(0, dxbc::Src::kXXXX));
        a.OpMov(dxbc::Dest::O(0, 0b0010), dxbc::Src::R(1, dxbc::Src::kXXXX));
      } else {
        for (uint32_t i = 0; i < 2; ++i) {
          a.OpUBFE(dxbc::Dest::O(0, 0b11 << (i * 2)), dxbc::Src::LU(16),
                   dxbc::Src::LU(0, 16, 0, 16), dxbc::Src::R(i, dxbc::Src::kXXXX));
        }
      }
    }
  } else {
    // Handle a 32bpp destination (32bpp color, or depth / stencil). If
    // color_packed_in_r1x is true, a raw 32bpp color value was written, and
    // common handling will be done.
    bool color_packed_in_r1x = false;
    bool depth_loaded_in_guest_format = false;
    if (source_is_color) {
      switch (source_color_format) {
        case xenos::ColorRenderTargetFormat::k_8_8_8_8:
        case xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA: {
          if (dest_is_stencil_bit) {
            if (source_color_format == xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA) {
              DxbcShaderTranslator::PreSaturatedLinearToPWLGamma(a, 1, 0, 1, 0, 2, 0, 2, 1);
            }
            a.OpMAd(dxbc::Dest::R(1, 0b0001), dxbc::Src::R(1, dxbc::Src::kXXXX),
                    dxbc::Src::LF(255.0f), dxbc::Src::LF(0.5f));
            a.OpFToU(dxbc::Dest::R(1, 0b0001), dxbc::Src::R(1, dxbc::Src::kXXXX));
          } else if (dest_is_color &&
                     (dest_color_format == xenos::ColorRenderTargetFormat::k_8_8_8_8 ||
                      dest_color_format == xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA)) {
            if (source_color_format != dest_color_format) {
              // Color space conversion between k_8_8_8_8 and
              // k_8_8_8_8_GAMMA.
              if (dest_color_format != xenos::ColorRenderTargetFormat::k_8_8_8_8) {
                for (uint32_t i = 0; i < 3; ++i) {
                  DxbcShaderTranslator::PreSaturatedLinearToPWLGamma(a, 1, i, 1, i, 2, 0, 2, 1);
                }
              } else {
                for (uint32_t i = 0; i < 3; ++i) {
                  DxbcShaderTranslator::PWLGammaToLinear(a, 1, i, 1, i, true, 2, 0, 2, 1);
                }
              }
            }
            // Same or converted format - passthrough.
            a.OpMov(dxbc::Dest::O(0), dxbc::Src::R(1));
          } else if (mode.output == TransferOutput::kDepth) {
            if (source_color_format == xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA) {
              for (uint32_t i = osgn_parameter_index_sv_stencil_ref != UINT32_MAX ? 0 : 1; i < 3;
                   ++i) {
                DxbcShaderTranslator::PreSaturatedLinearToPWLGamma(a, 1, i, 1, i, 2, 0, 2, 1);
              }
            }
            // When need only depth, not stencil, skip the red component.
            a.OpMAd(dxbc::Dest::R(
                        1, osgn_parameter_index_sv_stencil_ref != UINT32_MAX ? 0b1111 : 0b1110),
                    dxbc::Src::R(1), dxbc::Src::LF(255.0f), dxbc::Src::LF(0.5f));
            a.OpFToU(dxbc::Dest::R(1, 0b1110), dxbc::Src::R(1));
            if (osgn_parameter_index_sv_stencil_ref != UINT32_MAX) {
              // Write the red component to the stencil reference.
              a.OpFToU(dxbc::Dest::OStencilRef(), dxbc::Src::R(1, dxbc::Src::kXXXX));
            }
            // Put depth in 0:23 of r1.w.
            // r1.y = 0xGGBB0000.
            a.OpBFI(dxbc::Dest::R(1, 0b0010), dxbc::Src::LU(8), dxbc::Src::LU(8),
                    dxbc::Src::R(1, dxbc::Src::kZZZZ), dxbc::Src::R(1, dxbc::Src::kYYYY));
            // r1.w = 0xGGBBAA00.
            a.OpBFI(dxbc::Dest::R(1, 0b1000), dxbc::Src::LU(8), dxbc::Src::LU(16),
                    dxbc::Src::R(1, dxbc::Src::kWWWW), dxbc::Src::R(1, dxbc::Src::kYYYY));
          } else {
            if (source_color_format == xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA) {
              for (uint32_t i = 0; i < 3; ++i) {
                DxbcShaderTranslator::PreSaturatedLinearToPWLGamma(a, 1, i, 1, i, 2, 0, 2, 1);
              }
            }
            color_packed_in_r1x = true;
            a.OpMAd(dxbc::Dest::R(1), dxbc::Src::R(1), dxbc::Src::LF(255.0f), dxbc::Src::LF(0.5f));
            a.OpFToU(dxbc::Dest::R(1), dxbc::Src::R(1));
            for (uint32_t i = 1; i < 4; ++i) {
              a.OpBFI(dxbc::Dest::R(1, 0b0001), dxbc::Src::LU(8), dxbc::Src::LU(i * 8),
                      dxbc::Src::R(1).Select(i), dxbc::Src::R(1, dxbc::Src::kXXXX));
            }
          }
        } break;
        case xenos::ColorRenderTargetFormat::k_2_10_10_10:
        case xenos::ColorRenderTargetFormat::k_2_10_10_10_AS_10_10_10_10: {
          if (dest_is_stencil_bit) {
            a.OpMAd(dxbc::Dest::R(1, 0b0001), dxbc::Src::R(1, dxbc::Src::kXXXX),
                    dxbc::Src::LF(1023.0f), dxbc::Src::LF(0.5f));
            a.OpFToU(dxbc::Dest::R(1, 0b0001), dxbc::Src::R(1, dxbc::Src::kXXXX));
          } else if (dest_is_color &&
                     (dest_color_format == xenos::ColorRenderTargetFormat::k_2_10_10_10 ||
                      dest_color_format ==
                          xenos::ColorRenderTargetFormat::k_2_10_10_10_AS_10_10_10_10)) {
            a.OpMov(dxbc::Dest::O(0), dxbc::Src::R(1));
          } else {
            color_packed_in_r1x = true;
            a.OpMAd(dxbc::Dest::R(1), dxbc::Src::R(1),
                    dxbc::Src::LF(1023.0f, 1023.0f, 1023.0f, 3.0f), dxbc::Src::LF(0.5f));
            a.OpFToU(dxbc::Dest::R(1), dxbc::Src::R(1));
            for (uint32_t i = 1; i < 4; ++i) {
              a.OpBFI(dxbc::Dest::R(1, 0b0001), dxbc::Src::LU(i == 3 ? 2 : 10),
                      dxbc::Src::LU(i * 10), dxbc::Src::R(1).Select(i),
                      dxbc::Src::R(1, dxbc::Src::kXXXX));
            }
          }
        } break;
        case xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT:
        case xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT_AS_16_16_16_16: {
          if (dest_is_stencil_bit) {
            DxbcShaderTranslator::UnclampedFloat32To7e3(a, 1, 0, 1, 0, 2, 0);
          } else if (dest_is_color &&
                     (dest_color_format == xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT ||
                      dest_color_format ==
                          xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT_AS_16_16_16_16)) {
            a.OpMov(dxbc::Dest::O(0), dxbc::Src::R(1));
          } else {
            color_packed_in_r1x = true;
            // Float16 has a wider range for both color and alpha, also NaNs -
            // clamp and convert.
            for (uint32_t i = 0; i < 3; ++i) {
              DxbcShaderTranslator::UnclampedFloat32To7e3(a, 1, i, 1, i, 2, 0);
              if (i) {
                a.OpBFI(dxbc::Dest::R(1, 0b0001), dxbc::Src::LU(10), dxbc::Src::LU(i * 10),
                        dxbc::Src::R(1).Select(i), dxbc::Src::R(1, dxbc::Src::kXXXX));
              }
            }
            // Saturate and convert the alpha.
            a.OpMov(dxbc::Dest::R(1, 0b1000), dxbc::Src::R(1, dxbc::Src::kWWWW), true);
            a.OpMAd(dxbc::Dest::R(1, 0b1000), dxbc::Src::R(1, dxbc::Src::kWWWW),
                    dxbc::Src::LF(3.0f), dxbc::Src::LF(0.5f));
            a.OpFToU(dxbc::Dest::R(1, 0b1000), dxbc::Src::R(1, dxbc::Src::kWWWW));
            a.OpBFI(dxbc::Dest::R(1, 0b0001), dxbc::Src::LU(2), dxbc::Src::LU(30),
                    dxbc::Src::R(1, dxbc::Src::kWWWW), dxbc::Src::R(1, dxbc::Src::kXXXX));
          }
        } break;
        case xenos::ColorRenderTargetFormat::k_16_16:
        case xenos::ColorRenderTargetFormat::k_16_16_16_16:
        case xenos::ColorRenderTargetFormat::k_16_16_FLOAT:
        case xenos::ColorRenderTargetFormat::k_16_16_16_16_FLOAT: {
          // All 16 bits per component formats are represented as integers in
          // ownership transfer for safe handling of NaNs and -32768 / -32767.
          if (dest_is_stencil_bit) {
            // High bits are not important for discarding, as only one bit is
            // checked - already loaded to red.
          } else if (dest_is_color &&
                     (dest_color_format == xenos::ColorRenderTargetFormat::k_16_16 ||
                      dest_color_format == xenos::ColorRenderTargetFormat::k_16_16_FLOAT)) {
            a.OpMov(dxbc::Dest::O(0, 0b0011), dxbc::Src::R(1));
          } else {
            color_packed_in_r1x = true;
            a.OpBFI(dxbc::Dest::R(1, 0b0001), dxbc::Src::LU(16), dxbc::Src::LU(16),
                    dxbc::Src::R(1, dxbc::Src::kYYYY), dxbc::Src::R(1, dxbc::Src::kXXXX));
          }
        } break;
        case xenos::ColorRenderTargetFormat::k_32_FLOAT:
        case xenos::ColorRenderTargetFormat::k_32_32_FLOAT: {
          color_packed_in_r1x = true;
        } break;
      }
    } else if (rs & kTransferUsedRootParameterDepthSRVBit) {
      if (dest_is_color || dest_depth_format != source_depth_format) {
        // Need to reinterpret the depth value as color or as a different depth
        // format.
        depth_loaded_in_guest_format = true;
        switch (source_depth_format) {
          case xenos::DepthRenderTargetFormat::kD24S8: {
            // Round to the nearest even integer. This seems to be the correct
            // conversion, adding +0.5 and rounding towards zero results in red
            // instead of black in the 4D5307E6 clear shader.
            a.OpMul(dxbc::Dest::R(1, 0b1000), dxbc::Src::R(1, dxbc::Src::kWWWW),
                    dxbc::Src::LF(float(0xFFFFFF)));
            a.OpRoundNE(dxbc::Dest::R(1, 0b1000), dxbc::Src::R(1, dxbc::Src::kWWWW));
            a.OpFToU(dxbc::Dest::R(1, 0b1000), dxbc::Src::R(1, dxbc::Src::kWWWW));
          } break;
          case xenos::DepthRenderTargetFormat::kD24FS8: {
            // Convert using r1.y as temporary.
            // When converting the depth in pixel shaders, it's always exact,
            // truncating not to insert additional rounding instructions.
            DxbcShaderTranslator::PreClampedDepthTo20e4(
                a, 1, 3, 1, 3, 1, 1,
                !depth_float24_convert_in_pixel_shader() && depth_float24_round(), true);
          } break;
        }
        if (dest_is_color) {
          // Merge depth and stencil into r1.x for reinterpretation as color.
          color_packed_in_r1x = true;
          a.OpBFI(dxbc::Dest::R(1, 0b0001), dxbc::Src::LU(24), dxbc::Src::LU(8),
                  dxbc::Src::R(1, dxbc::Src::kWWWW), dxbc::Src::R(1, dxbc::Src::kXXXX));
        }
      }
    }
    switch (mode.output) {
      case TransferOutput::kColor:
        // Unless a special path was taken, unpack the raw 32bpp value into the
        // 32bpp color output. Any register can be used as temporary if needed -
        // this is the end of the shader.
        if (color_packed_in_r1x) {
          switch (dest_color_format) {
            case xenos::ColorRenderTargetFormat::k_8_8_8_8: {
              a.OpUBFE(dxbc::Dest::R(1), dxbc::Src::LU(8), dxbc::Src::LU(0, 8, 16, 24),
                       dxbc::Src::R(1, dxbc::Src::kXXXX));
              a.OpUToF(dxbc::Dest::R(1), dxbc::Src::R(1));
              a.OpMul(dxbc::Dest::O(0), dxbc::Src::R(1), dxbc::Src::LF(1.0f / 255.0f));
            } break;
            case xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA: {
              // 8_8_8_8_GAMMA is represented by linear stored in
              // R16G16B16A16_UNORM.
              a.OpUBFE(dxbc::Dest::R(1), dxbc::Src::LU(8), dxbc::Src::LU(0, 8, 16, 24),
                       dxbc::Src::R(1, dxbc::Src::kXXXX));
              a.OpUToF(dxbc::Dest::R(1), dxbc::Src::R(1));
              a.OpMul(dxbc::Dest::R(1, 0b0111), dxbc::Src::R(1), dxbc::Src::LF(1.0f / 255.0f));
              a.OpMul(dxbc::Dest::O(0, 0b1000), dxbc::Src::R(1), dxbc::Src::LF(1.0f / 255.0f));
              for (uint32_t i = 0; i < 3; ++i) {
                DxbcShaderTranslator::PWLGammaToLinear(a, 1, i, 1, i, true, 0, 0, 0, 1);
              }
              a.OpMov(dxbc::Dest::O(0, 0b0111), dxbc::Src::R(1));
            } break;
            case xenos::ColorRenderTargetFormat::k_2_10_10_10:
            case xenos::ColorRenderTargetFormat::k_2_10_10_10_AS_10_10_10_10: {
              a.OpUBFE(dxbc::Dest::R(1), dxbc::Src::LU(10, 10, 10, 2), dxbc::Src::LU(0, 10, 20, 30),
                       dxbc::Src::R(1, dxbc::Src::kXXXX));
              a.OpUToF(dxbc::Dest::R(1), dxbc::Src::R(1));
              a.OpMul(dxbc::Dest::O(0), dxbc::Src::R(1),
                      dxbc::Src::LF(1.0f / 1023.0f, 1.0f / 1023.0f, 1.0f / 1023.0f, 1.0f / 3.0f));
            } break;
            case xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT:
            case xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT_AS_16_16_16_16: {
              // Color using r1.yz as temporary.
              for (uint32_t i = 0; i < 3; ++i) {
                DxbcShaderTranslator::Float7e3To32(a, dxbc::Dest::O(0, 1 << i), 1, 0, i * 10, 1, 1,
                                                   1, 2);
              }
              // Alpha.
              a.OpUBFE(dxbc::Dest::R(1, 0b1000), dxbc::Src::LU(2), dxbc::Src::LU(30),
                       dxbc::Src::R(1, dxbc::Src::kXXXX));
              a.OpUToF(dxbc::Dest::R(1, 0b1000), dxbc::Src::R(1, dxbc::Src::kWWWW));
              a.OpMul(dxbc::Dest::O(0, 0b1000), dxbc::Src::R(1, dxbc::Src::kWWWW),
                      dxbc::Src::LF(1.0f / 3.0f));
            } break;
            case xenos::ColorRenderTargetFormat::k_16_16:
            case xenos::ColorRenderTargetFormat::k_16_16_FLOAT: {
              // All 16 bits per component formats are represented as integers
              // in ownership transfer for safe handling of NaNs and
              // -32768 / -32767.
              a.OpUBFE(dxbc::Dest::O(0, 0b0011), dxbc::Src::LU(16), dxbc::Src::LU(0, 16, 0, 0),
                       dxbc::Src::R(1, dxbc::Src::kXXXX));
            } break;
            case xenos::ColorRenderTargetFormat::k_32_FLOAT: {
              // Already as a 32-bit value.
              a.OpMov(dxbc::Dest::O(0, 0b0001), dxbc::Src::R(1, dxbc::Src::kXXXX));
            } break;
            default:
              // A 64bpp format (handled separately) or an invalid one.
              assert_unhandled_case(dest_color_format);
          }
        }
        break;
      case TransferOutput::kDepth:
        if (source_is_color || depth_loaded_in_guest_format) {
          if (color_packed_in_r1x) {
            // Extract the depth bits to r1.w.
            a.OpUBFE(dxbc::Dest::R(1, 0b1000), dxbc::Src::LU(24), dxbc::Src::LU(8),
                     dxbc::Src::R(1, dxbc::Src::kXXXX));
            if (osgn_parameter_index_sv_stencil_ref != UINT32_MAX) {
              // Extract the stencil bits to the stencil reference.
              // The depth -> depth case is handled earlier, not long after
              // loading the stencil, for simplicity.
              a.OpUBFE(dxbc::Dest::OStencilRef(), dxbc::Src::LU(8), dxbc::Src::LU(0),
                       dxbc::Src::R(1, dxbc::Src::kXXXX));
            }
          }
          // r1.w contains the depth in the guest format. If a host depth source
          // is available, need to check if it's up to date - if it is, the host
          // precision value needs to be written. Otherwise, the new guest value
          // needs to be converted to the host format. Using `if` here because
          // it's likely that the values will either be the same - if not
          // modified - or different - if cleared or totally overwritten - in
          // large amounts of samples, usually whole waves, at once.
          if (rs & kTransferUsedRootParameterHostDepthSRVBit) {
            // Load the host float32 depth to r0.x, check if, when converted to
            // the guest format, it's the same as the guest source, thus up to
            // date, and if it is, write host float32 depth to r1.w, otherwise
            // do the guest -> host conversion on the `else` path.

            // Current register allocation:
            // r0.xy = pixel index within the destination 32bpp tile
            // r0.z = 32bpp tile index relative to the destination base
            // r1.w = depth in guest format

            if (cross_scale_class) {
              // Host-depth sources always share the destination scale.
              a.OpMov(dxbc::Dest::R(0, 0b0011), dxbc::Src::R(3));
            }

            if (key.host_depth_source_is_copy) {
              // Get the address in the EDRAM scratch buffer and load from
              // there.
              // The beginning of the buffer is (0, 0) of the destination.
              // 40-sample columns are not swapped for addressing simplicity
              // (because this is used for depth -> depth transfers, where
              // swapping isn't needed).
              // Convert samples to pixels.
              assert_true(key.host_depth_source_msaa_samples == xenos::MsaaSamples::k1X);
              if (key.dest_msaa_samples >= xenos::MsaaSamples::k2X) {
                if (key.dest_msaa_samples >= xenos::MsaaSamples::k4X) {
                  // Horizontal sample index in bit 0.
                  a.OpBFI(dxbc::Dest::R(0, 0b0001), dxbc::Src::LU(31), dxbc::Src::LU(1),
                          dxbc::Src::R(0, dxbc::Src::kXXXX), dest_sample);
                }
                // Vertical sample index as 1 or 0 in bit 0 for true 2x or as 0
                // or 1 in bit 1 for 4x or for 2x emulated as 4x.
                if (key.dest_msaa_samples == xenos::MsaaSamples::k2X && msaa_2x_supported_) {
                  a.OpBFI(dxbc::Dest::R(0, 0b0010), dxbc::Src::LU(31), dxbc::Src::LU(1),
                          dxbc::Src::R(0, dxbc::Src::kYYYY), dest_sample);
                  a.OpXOr(dxbc::Dest::R(0, 0b0010), dxbc::Src::R(0, dxbc::Src::kYYYY),
                          dxbc::Src::LU(1));
                } else {
                  // Using r0.w as a temporary.
                  a.OpUShR(dxbc::Dest::R(0, 0b1000), dest_sample, dxbc::Src::LU(1));
                  a.OpBFI(dxbc::Dest::R(0, 0b0010), dxbc::Src::LU(31), dxbc::Src::LU(1),
                          dxbc::Src::R(0, dxbc::Src::kYYYY), dxbc::Src::R(0, dxbc::Src::kWWWW));
                }
              }
              // Combine the tile sample index and the tile index into buffer
              // address to r0.x.
              // The tile index doesn't need to be wrapped, as the host depth is
              // written to the beginning of the buffer, without the base
              // offset.
              a.OpUMAd(dxbc::Dest::R(0, 0b0001),
                       dxbc::Src::LU(dest_tile_width_samples),
                       dxbc::Src::R(0, dxbc::Src::kYYYY), dxbc::Src::R(0, dxbc::Src::kXXXX));
              a.OpUMAd(dxbc::Dest::R(0, 0b0001),
                       dxbc::Src::LU(dest_tile_width_samples *
                                     dest_tile_height_samples),
                       dxbc::Src::R(0, dxbc::Src::kZZZZ), dxbc::Src::R(0, dxbc::Src::kXXXX));
              // Load from the buffer.
              a.OpLd(dxbc::Dest::R(0, 0b0001), dxbc::Src::R(0, dxbc::Src::kXXXX), 0b0001,
                     dxbc::Src::T(srv_index_host_depth, kTransferSRVRegisterHostDepth,
                                  dxbc::Src::kXXXX));
            } else {
              // Adjust the tile index from the destination to the host depth
              // source.
              // r0.w = destination to host depth source EDRAM tile adjustment
              a.OpIBFE(dxbc::Dest::R(0, 0b1000), dxbc::Src::LU(xenos::kEdramBaseTilesBits + 1),
                       dxbc::Src::LU(xenos::kEdramPitchTilesBits * 2),
                       dxbc::Src::CB(cbuffer_index_host_depth_address,
                                     kTransferCBVRegisterHostDepthAddress, 0, dxbc::Src::kXXXX));
              // r0.z = tile index relative to the host depth source base, or
              //        the tile index within the host depth source minus the
              //        EDRAM tile count if transferring across addressing
              //        wrapping (if negative)
              // r0.w = free
              a.OpIAdd(dxbc::Dest::R(0, 0b0100), dxbc::Src::R(0, dxbc::Src::kZZZZ),
                       dxbc::Src::R(0, dxbc::Src::kWWWW));
              // r0.z = tile index relative to the host depth source base
              a.OpAnd(dxbc::Dest::R(0, 0b0100), dxbc::Src::R(0, dxbc::Src::kZZZZ),
                      dxbc::Src::LU(xenos::kEdramTileCount - 1));
              // Convert position and sample index from within the destination
              // tile to within the host depth source tile, like for the guest
              // render target, but for 32bpp -> 32bpp only.
              dxbc::Src host_depth_source_sample(dest_sample);
              if (key.host_depth_source_msaa_samples != key.dest_msaa_samples) {
                dxbc::Src host_depth_u(dxbc::Src::R(0, dxbc::Src::kXXXX));
                dxbc::Src host_depth_v(dxbc::Src::R(0, dxbc::Src::kYYYY));
                bool host_depth_scaled;
                CanonicalizeSample(a, key.dest_msaa_samples, dest_sample,
                                   msaa_2x_supported_, dest_scale_x,
                                   dest_scale_y, host_depth_u, host_depth_v,
                                   host_depth_scaled);
                dxbc::Src host_depth_x(host_depth_u);
                dxbc::Src host_depth_y(host_depth_v);
                DecanonicalizeSample(a, key.host_depth_source_msaa_samples,
                                     host_depth_u, host_depth_v,
                                     host_depth_scaled, msaa_2x_supported_,
                                     dest_scale_x, dest_scale_y, host_depth_x,
                                     host_depth_y, host_depth_source_sample);
                // The remapped coordinates are in r1.xy. The helpers preserve
                // r1.w (guest depth), and the sample index is consumed before
                // the pitch reuses r1.x.
                a.OpMov(dxbc::Dest::R(0, 0b0011), dxbc::Src::R(1));
              }
              // r1.x = host depth source pitch in tiles
              a.OpUBFE(dxbc::Dest::R(1, 0b0001), dxbc::Src::LU(xenos::kEdramPitchTilesBits),
                       dxbc::Src::LU(xenos::kEdramPitchTilesBits),
                       dxbc::Src::CB(cbuffer_index_host_depth_address,
                                     kTransferCBVRegisterHostDepthAddress, 0, dxbc::Src::kXXXX));
              // r0.z = host depth source tile row
              // r1.x = host depth source tile within the row
              a.OpUDiv(dxbc::Dest::R(0, 0b0100), dxbc::Dest::R(1, 0b0001),
                       dxbc::Src::R(0, dxbc::Src::kZZZZ), dxbc::Src::R(1, dxbc::Src::kXXXX));
              // r0.x = pixel X within the host depth source texture
              // r1.x = free
              a.OpUMAd(
                  dxbc::Dest::R(0, 0b0001),
                  dxbc::Src::LU(
                      dest_tile_width_samples >>
                      uint32_t(key.host_depth_source_msaa_samples >=
                               xenos::MsaaSamples::k4X)),
                  dxbc::Src::R(1, dxbc::Src::kXXXX), dxbc::Src::R(0, dxbc::Src::kXXXX));
              // r0.y = pixel Y within the host depth source texture
              // r0.z = free
              a.OpUMAd(dxbc::Dest::R(0, 0b0010),
                       dxbc::Src::LU(
                            dest_tile_height_samples >>
                           uint32_t(key.host_depth_source_msaa_samples >= xenos::MsaaSamples::k2X)),
                       dxbc::Src::R(0, dxbc::Src::kZZZZ), dxbc::Src::R(0, dxbc::Src::kYYYY));
              // Load from the host depth texture.
              if (key.host_depth_source_msaa_samples != xenos::MsaaSamples::k1X) {
                a.OpLdMS(dxbc::Dest::R(0, 0b0001), dxbc::Src::R(0), 0b0011,
                         dxbc::Src::T(srv_index_host_depth, kTransferSRVRegisterHostDepth,
                                      dxbc::Src::kXXXX),
                         host_depth_source_sample);
              } else {
                // Write zero to the LOD index in r0.z.
                a.OpMov(dxbc::Dest::R(0, 0b0100), dxbc::Src::LU(0));
                a.OpLd(dxbc::Dest::R(0, 0b0001), dxbc::Src::R(0, 0b10000100), 0b1011,
                       dxbc::Src::T(srv_index_host_depth, kTransferSRVRegisterHostDepth,
                                    dxbc::Src::kXXXX));
              }
            }
            // Convert the host depth value in r0.x to the guest format in r0.y
            // using r0.z as a temporary and check if it matches the value in
            // the currently owning guest render target.
            switch (dest_depth_format) {
              case xenos::DepthRenderTargetFormat::kD24S8: {
                // Round to the nearest even integer. This seems to be the
                // correct, adding +0.5 and rounding towards zero results in red
                // instead of black in the 4D5307E6 clear shader.
                a.OpMul(dxbc::Dest::R(0, 0b0010), dxbc::Src::R(0, dxbc::Src::kXXXX),
                        dxbc::Src::LF(float(0xFFFFFF)));
                a.OpRoundNE(dxbc::Dest::R(0, 0b0010), dxbc::Src::R(0, dxbc::Src::kYYYY));
                a.OpFToU(dxbc::Dest::R(0, 0b0010), dxbc::Src::R(0, dxbc::Src::kYYYY));
              } break;
              case xenos::DepthRenderTargetFormat::kD24FS8: {
                // When converting the depth in pixel shaders, it's always
                // exact, truncating not to insert additional rounding
                // instructions.
                DxbcShaderTranslator::PreClampedDepthTo20e4(
                    a, 0, 1, 0, 0, 0, 2,
                    !depth_float24_convert_in_pixel_shader() && depth_float24_round(), true);
              } break;
            }
            a.OpIEq(dxbc::Dest::R(0, 0b0010), dxbc::Src::R(0, dxbc::Src::kYYYY),
                    dxbc::Src::R(1, dxbc::Src::kWWWW));
            a.OpIf(true, dxbc::Src::R(0, dxbc::Src::kYYYY));
            // If the host depth is up to date, write it to oDepth at the host
            // precision instead of converting the guest depth.
            a.OpMov(dxbc::Dest::R(1, 0b1000), dxbc::Src::R(0, dxbc::Src::kXXXX));
            a.OpElse();
          }
          // Convert using r0.x as a temporary.
          switch (dest_depth_format) {
            case xenos::DepthRenderTargetFormat::kD24S8: {
              // Multiplying by 1.0 / 0xFFFFFF produces an incorrect result (for
              // 0xC00000, for instance - which is 2_10_10_10 clear to 0001) -
              // rescale from 0...0xFFFFFF to 0...0x1000000 doing what true
              // float division followed by multiplication does (on x86-64 MSVC
              // with default SSE rounding) - values starting from 0x800000
              // become bigger by 1; then accurately bias the result's exponent.
              a.OpUShR(dxbc::Dest::R(0, 0b0001), dxbc::Src::R(1, dxbc::Src::kWWWW),
                       dxbc::Src::LU(23));
              a.OpIAdd(dxbc::Dest::R(1, 0b1000), dxbc::Src::R(1, dxbc::Src::kWWWW),
                       dxbc::Src::R(0, dxbc::Src::kXXXX));
              a.OpUToF(dxbc::Dest::R(1, 0b1000), dxbc::Src::R(1, dxbc::Src::kWWWW));
              a.OpMul(dxbc::Dest::R(1, 0b1000), dxbc::Src::R(1, dxbc::Src::kWWWW),
                      dxbc::Src::LF(1.0f / float(1 << 24)));
            } break;
            case xenos::DepthRenderTargetFormat::kD24FS8: {
              DxbcShaderTranslator::Depth20e4To32(a, dxbc::Dest::R(1, 0b1000), 1, 3, 0, 1, 3, 0, 0,
                                                  true);
            } break;
          }
          // Host depth is different, or not available - convert the guest depth
          // to the destination format.
          if (rs & kTransferUsedRootParameterHostDepthSRVBit) {
            // Close the conditional for the host / guest depth.
            a.OpEndIf();
          }
        }
        a.OpMov(dxbc::Dest::ODepth(), dxbc::Src::R(1, dxbc::Src::kWWWW));
        break;
      case TransferOutput::kStencilBit:
        // Discard the sample if the needed stencil bit is not set.
        assert_true(cbuffer_index_stencil_mask != UINT32_MAX);
        a.OpAnd(dxbc::Dest::R(0, 0b0001), dxbc::Src::R(1, dxbc::Src::kXXXX),
                dxbc::Src::CB(cbuffer_index_stencil_mask, kTransferCBVRegisterStencilMask, 0,
                              dxbc::Src::kXXXX));
        a.OpDiscard(false, dxbc::Src::R(0, dxbc::Src::kXXXX));
        break;
    }
  }

  if (dest_is_color) {
    // Fill the unused components of the color result.
    uint32_t dest_color_component_count =
        xenos::GetColorRenderTargetFormatComponentCount(dest_color_format);
    uint32_t dest_color_unwritten_mask = 0b1111 & ~uint32_t((1 << dest_color_component_count) - 1);
    if (dest_color_component_count < 4) {
      a.OpMov(dxbc::Dest::O(0, dest_color_unwritten_mask), dxbc::Src::LU(0));
    }
  }

  a.OpRet();

  // Write the shader program length in dwords.
  built_shader_[shex_position_dwords + 1] = uint32_t(built_shader_.size()) - shex_position_dwords;

  {
    auto& blob_header =
        *reinterpret_cast<dxbc::BlobHeader*>(built_shader_.data() + blob_position_dwords);
    blob_header.fourcc = dxbc::BlobHeader::FourCC::kShaderEx;
    blob_position_dwords = uint32_t(built_shader_.size());
    blob_header.size_bytes = (blob_position_dwords - kBlobHeaderSizeDwords) * sizeof(uint32_t) -
                             built_shader_[blob_offset_position_dwords++];
  }

  // ***************************************************************************
  // Shader feature info
  // ***************************************************************************

  if (shader_uses_stencil_reference_output) {
    built_shader_[blob_offset_position_dwords] = uint32_t(blob_position_dwords * sizeof(uint32_t));
    uint32_t sfi0_position_dwords = blob_position_dwords + kBlobHeaderSizeDwords;
    built_shader_.resize(sfi0_position_dwords + sizeof(dxbc::ShaderFeatureInfo) / sizeof(uint32_t));
    auto& shader_feature_info =
        *reinterpret_cast<dxbc::ShaderFeatureInfo*>(built_shader_.data() + sfi0_position_dwords);
    shader_feature_info.feature_flags[0] |= dxbc::kShaderFeature0_StencilRef;
    {
      auto& blob_header =
          *reinterpret_cast<dxbc::BlobHeader*>(built_shader_.data() + blob_position_dwords);
      blob_header.fourcc = dxbc::BlobHeader::FourCC::kShaderFeatureInfo;
      blob_position_dwords = uint32_t(built_shader_.size());
      blob_header.size_bytes = (blob_position_dwords - kBlobHeaderSizeDwords) * sizeof(uint32_t) -
                               built_shader_[blob_offset_position_dwords++];
    }
  }

  // ***************************************************************************
  // Statistics
  // ***************************************************************************

  built_shader_[blob_offset_position_dwords] = uint32_t(blob_position_dwords * sizeof(uint32_t));
  uint32_t stat_position_dwords = blob_position_dwords + kBlobHeaderSizeDwords;
  built_shader_.resize(stat_position_dwords + sizeof(dxbc::Statistics) / sizeof(uint32_t));
  std::memcpy(built_shader_.data() + stat_position_dwords, &stat, sizeof(dxbc::Statistics));
  {
    auto& blob_header =
        *reinterpret_cast<dxbc::BlobHeader*>(built_shader_.data() + blob_position_dwords);
    blob_header.fourcc = dxbc::BlobHeader::FourCC::kStatistics;
    blob_position_dwords = uint32_t(built_shader_.size());
    blob_header.size_bytes = (blob_position_dwords - kBlobHeaderSizeDwords) * sizeof(uint32_t) -
                             built_shader_[blob_offset_position_dwords++];
  }

  // ***************************************************************************
  // Container header
  // ***************************************************************************

  uint32_t built_shader_size_bytes = uint32_t(built_shader_.size() * sizeof(uint32_t));
  {
    auto& container_header = *reinterpret_cast<dxbc::ContainerHeader*>(built_shader_.data());
    container_header.InitializeIdentification();
    container_header.size_bytes = built_shader_size_bytes;
    container_header.blob_count = blob_count;
    CalculateDXBCChecksum(reinterpret_cast<unsigned char*>(built_shader_.data()),
                          static_cast<unsigned int>(built_shader_size_bytes),
                          reinterpret_cast<unsigned int*>(&container_header.hash));
  }

  // ***************************************************************************
  // Pipeline
  // ***************************************************************************

  ID3D12PipelineState* const* pipelines;
  ID3D12Device* device = command_processor_.GetD3D12Provider().GetDevice();
  D3D12_INPUT_ELEMENT_DESC pipeline_input_element_desc;
  pipeline_input_element_desc.SemanticName = "POSITION";
  pipeline_input_element_desc.SemanticIndex = 0;
  pipeline_input_element_desc.Format = DXGI_FORMAT_R32G32_FLOAT;
  pipeline_input_element_desc.InputSlot = 0;
  pipeline_input_element_desc.AlignedByteOffset = 0;
  pipeline_input_element_desc.InputSlotClass = D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;
  pipeline_input_element_desc.InstanceDataStepRate = 0;
  D3D12_GRAPHICS_PIPELINE_STATE_DESC pipeline_desc = {};
  pipeline_desc.pRootSignature = transfer_root_signatures_[size_t(
      use_stencil_reference_output_ ? mode.root_signature_with_stencil_ref
                                    : mode.root_signature_no_stencil_ref)];
  pipeline_desc.VS.pShaderBytecode = shaders::passthrough_position_xy_vs;
  pipeline_desc.VS.BytecodeLength = sizeof(shaders::passthrough_position_xy_vs);
  pipeline_desc.PS.pShaderBytecode = built_shader_.data();
  pipeline_desc.PS.BytecodeLength = built_shader_size_bytes;
  if (key.dest_msaa_samples == xenos::MsaaSamples::k2X && !msaa_2x_supported_) {
    // Using sample 0 as 0 and 3 as 1 for 2x instead.
    pipeline_desc.SampleMask = 0b1001;
    pipeline_desc.SampleDesc.Count = 4;
  } else {
    pipeline_desc.SampleMask = UINT_MAX;
    pipeline_desc.SampleDesc.Count = UINT(1) << UINT(key.dest_msaa_samples);
  }
  pipeline_desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
  pipeline_desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
  pipeline_desc.RasterizerState.DepthClipEnable = TRUE;
  pipeline_desc.InputLayout.pInputElementDescs = &pipeline_input_element_desc;
  pipeline_desc.InputLayout.NumElements = 1;
  pipeline_desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  if (dest_is_stencil_bit) {
    pipeline_desc.DepthStencilState.StencilEnable = TRUE;
    pipeline_desc.DepthStencilState.FrontFace.StencilFailOp = D3D12_STENCIL_OP_KEEP;
    pipeline_desc.DepthStencilState.FrontFace.StencilDepthFailOp = D3D12_STENCIL_OP_KEEP;
    pipeline_desc.DepthStencilState.FrontFace.StencilPassOp = D3D12_STENCIL_OP_REPLACE;
    pipeline_desc.DepthStencilState.FrontFace.StencilFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    pipeline_desc.DepthStencilState.BackFace = pipeline_desc.DepthStencilState.FrontFace;
    pipeline_desc.DSVFormat = GetDepthDSVDXGIFormat(dest_depth_format);
    // Even if creation fails, still store the null pointers not to try to
    // create again.
    std::array<ID3D12PipelineState*, 8>& stencil_bit_pipelines =
        transfer_stencil_bit_pipelines_
            .emplace(std::piecewise_construct, std::make_tuple(key), std::make_tuple())
            .first->second;
    bool stencil_pipelines_created = true;
    for (uint32_t i = 0; i < 8; ++i) {
      pipeline_desc.DepthStencilState.StencilWriteMask = UINT8(1) << i;
      if (SUCCEEDED(device->CreateGraphicsPipelineState(&pipeline_desc,
                                                        IID_PPV_ARGS(&stencil_bit_pipelines[i])))) {
        continue;
      }
      stencil_pipelines_created = false;
      for (uint32_t j = 0; j < i; ++j) {
        stencil_bit_pipelines[j]->Release();
        stencil_bit_pipelines[j] = nullptr;
      }
      break;
    }
    pipelines = stencil_pipelines_created ? stencil_bit_pipelines.data() : nullptr;
  } else {
    if (dest_is_color) {
      pipeline_desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
      pipeline_desc.NumRenderTargets = 1;
      pipeline_desc.RTVFormats[0] = GetColorOwnershipTransferDXGIFormat(dest_color_format);
    } else {
      pipeline_desc.DepthStencilState.DepthEnable = TRUE;
      pipeline_desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
      pipeline_desc.DepthStencilState.DepthFunc = REXCVAR_GET(depth_transfer_not_equal_test)
                                                      ? D3D12_COMPARISON_FUNC_NOT_EQUAL
                                                      : D3D12_COMPARISON_FUNC_ALWAYS;
      if (use_stencil_reference_output_) {
        pipeline_desc.DepthStencilState.StencilEnable = TRUE;
        pipeline_desc.DepthStencilState.StencilWriteMask = UINT8_MAX;
        pipeline_desc.DepthStencilState.FrontFace.StencilFailOp = D3D12_STENCIL_OP_KEEP;
        pipeline_desc.DepthStencilState.FrontFace.StencilDepthFailOp =
            REXCVAR_GET(depth_transfer_not_equal_test) ? D3D12_STENCIL_OP_REPLACE
                                                       : D3D12_STENCIL_OP_KEEP;
        pipeline_desc.DepthStencilState.FrontFace.StencilPassOp = D3D12_STENCIL_OP_REPLACE;
        // Using ALWAYS, not NOT_EQUAL, so depth writing is unaffected by
        // stencil being different.
        pipeline_desc.DepthStencilState.FrontFace.StencilFunc = D3D12_COMPARISON_FUNC_ALWAYS;
        pipeline_desc.DepthStencilState.BackFace = pipeline_desc.DepthStencilState.FrontFace;
      }
      pipeline_desc.DSVFormat = GetDepthDSVDXGIFormat(dest_depth_format);
    }
    ID3D12PipelineState* pipeline;
    if (FAILED(device->CreateGraphicsPipelineState(&pipeline_desc, IID_PPV_ARGS(&pipeline)))) {
      pipeline = nullptr;
    }
    // Even if creation fails, still store the null pointer not to try to create
    // again.
    // Return a pointer to the persistent location.
    ID3D12PipelineState*& inserted_pipeline =
        transfer_pipelines_.emplace(key, pipeline).first->second;
    pipelines = inserted_pipeline ? &inserted_pipeline : nullptr;
  }
  // TODO(Triang3l): Pipeline state name debug names (lots of variables - but
  // not very important since everything can be derived from the bindings and
  // outputs in a debugger).

  if (!pipelines) {
    // Stencil bit copying uses only the stencil SRV for depth / stencil source,
    // can't use srv_index_depth for checking.
    const char* source_format_name =
        (rs & kTransferUsedRootParameterColorSRVBit)
            ? xenos::GetColorRenderTargetFormatName(source_color_format)
            : xenos::GetDepthRenderTargetFormatName(source_depth_format);
    const char* dest_format_name = mode.output == TransferOutput::kColor
                                       ? xenos::GetColorRenderTargetFormatName(dest_color_format)
                                       : xenos::GetDepthRenderTargetFormatName(dest_depth_format);
    if (srv_index_host_depth != UINT32_MAX) {
      REXGPU_ERROR(
          "D3D12RenderTargetCache: Failed to create a render target ownership "
          "transfer pipeline for {}-sample {} + {}-sample host depth{} -> "
          "{}-sample {} for mode {}",
          uint32_t(1) << uint32_t(key.source_msaa_samples), source_format_name,
          uint32_t(1) << uint32_t(key.host_depth_source_msaa_samples),
          key.host_depth_source_is_copy ? " copy" : "",
          uint32_t(1) << uint32_t(key.dest_msaa_samples), dest_format_name, uint32_t(key.mode));
    } else {
      REXGPU_ERROR(
          "D3D12RenderTargetCache: Failed to create a render target ownership "
          "transfer pipeline for {}-sample {} -> {}-sample {} for mode {}",
          uint32_t(1) << uint32_t(key.source_msaa_samples), source_format_name,
          uint32_t(1) << uint32_t(key.dest_msaa_samples), dest_format_name, uint32_t(key.mode));
    }
  }
  return pipelines;
}

void D3D12RenderTargetCache::RecordSceneUpdateTargets(
    RenderTarget* const* targets, const std::vector<Transfer>* transfers,
    embedded_scene_transfer_capture_policy::UpdateContext& c) {
  const auto resource_id = [](RenderTarget* rt) -> unsigned long long {
    return rt ? reinterpret_cast<uintptr_t>(static_cast<D3D12RenderTarget*>(rt)->resource()) : 0;
  };
  for (uint32_t i = 0; i < 1 + xenos::kMaxColorRenderTargets; ++i) {
    if (!targets[i]) continue;
    ++c.targets;
    if (c.Record()) {
      const auto& rt = *static_cast<D3D12RenderTarget*>(targets[i]);
      const auto desc = rt.resource()->GetDesc();
      std::fprintf(stderr,
          "REX_SCENE_UPDATE_TARGET frame=%llu update=%llu next_draw=%llu slot=%u resource=%016llX key=%08X "
          "host_width=%llu host_height=%u host_format=%u host_samples=%u scale=%ux%u\n",
          static_cast<unsigned long long>(c.frame), static_cast<unsigned long long>(c.update),
          static_cast<unsigned long long>(c.next_draw), i, resource_id(targets[i]), rt.key().key,
          static_cast<unsigned long long>(desc.Width), desc.Height, uint32_t(desc.Format), desc.SampleDesc.Count,
          GetKeyScaleX(rt.key()), GetKeyScaleY(rt.key()));
    }
    for (const auto& transfer : transfers[i]) {
      ++c.planned;
      if (!c.Record()) continue;
      std::fprintf(stderr,
          "REX_SCENE_UPDATE_PLAN frame=%llu update=%llu next_draw=%llu slot=%u start=%u end=%u "
          "source=%016llX source_key=%08X dest=%016llX dest_key=%08X host_depth=%016llX host_depth_key=%08X "
          "scope=planned_transfer_not_execution\n",
          static_cast<unsigned long long>(c.frame), static_cast<unsigned long long>(c.update),
          static_cast<unsigned long long>(c.next_draw), i, transfer.start_tiles, transfer.end_tiles,
          resource_id(transfer.source), transfer.source ? transfer.source->key().key : 0,
          resource_id(targets[i]), targets[i]->key().key, resource_id(transfer.host_depth_source),
          transfer.host_depth_source ? transfer.host_depth_source->key().key : 0);
    }
  }
  OwnershipSnapshot owners[32];
  c.owners = CopyOwnershipSnapshot(owners, uint32_t(rex::countof(owners)));
  if (c.owners > rex::countof(owners)) c.Drop(c.owners - uint32_t(rex::countof(owners)));
  for (uint32_t i = 0; i < std::min(c.owners, uint32_t(rex::countof(owners))); ++i) {
    if (!c.Record()) continue;
    const auto& owner = owners[i];
    std::fprintf(stderr,
        "REX_SCENE_UPDATE_OWNER frame=%llu update=%llu next_draw=%llu owner=%u start=%u end=%u "
        "resource=%016llX key=%08X host_depth_unorm=%08X host_depth_float=%08X "
        "scope=post_update_bookkeeping_not_transfer_completion\n",
        static_cast<unsigned long long>(c.frame), static_cast<unsigned long long>(c.update),
        static_cast<unsigned long long>(c.next_draw), i, owner.start, owner.end,
        resource_id(owner.render_target), owner.key, owner.host_depth_unorm, owner.host_depth_float);
  }
}

void D3D12RenderTargetCache::PerformTransfersAndResolveClears(
    uint32_t render_target_count, RenderTarget* const* render_targets,
    const std::vector<Transfer>* render_target_transfers,
    const uint64_t* render_target_resolve_clear_values,
    const Transfer::Rectangle* resolve_clear_rectangle,
    const embedded_scene_resolve_capture_policy::Context* scene_capture_context,
    embedded_scene_transfer_capture_policy::UpdateContext* update_capture) {
  assert_true(GetPath() == Path::kHostRenderTargets);

  const ui::d3d12::D3D12Provider& provider = command_processor_.GetD3D12Provider();
  ID3D12Device* device = provider.GetDevice();
  uint64_t current_submission = command_processor_.GetCurrentSubmission();
  DeferredCommandList& command_list = command_processor_.GetDeferredCommandList();

  embedded_scene_resolve_capture_policy::ClearCommands scene_clear_command_budget;
  const auto record_update_failure = [&](const char* reason) {
    if (!update_capture) return;
    ++update_capture->failures;
    if (!update_capture->Record()) return;
    const auto& c = *update_capture;
    std::fprintf(stderr,
        "REX_SCENE_UPDATE_FAILURE frame=%llu update=%llu next_draw=%llu reason=%s submission=%llu\n",
        static_cast<unsigned long long>(c.frame), static_cast<unsigned long long>(c.update),
        static_cast<unsigned long long>(c.next_draw), reason,
        static_cast<unsigned long long>(command_processor_.GetCurrentSubmission()));
  };
  D3D12_RECT clear_rect;
  bool resolve_clear_needed = render_target_resolve_clear_values && resolve_clear_rectangle;
  if (resolve_clear_needed) {
    uint32_t resolve_clear_scale_x = draw_resolution_scale_x();
    uint32_t resolve_clear_scale_y = draw_resolution_scale_y();
    for (uint32_t i = 0; i < render_target_count; ++i) {
      if (render_targets[i]) {
        resolve_clear_scale_x = GetKeyScaleX(render_targets[i]->key());
        resolve_clear_scale_y = GetKeyScaleY(render_targets[i]->key());
        break;
      }
    }
    // Assuming the rectangle is already clamped by the setup function from the
    // common render target cache.
    clear_rect.left =
        LONG(resolve_clear_rectangle->x_pixels * resolve_clear_scale_x);
    clear_rect.top =
        LONG(resolve_clear_rectangle->y_pixels * resolve_clear_scale_y);
    clear_rect.right =
        LONG((resolve_clear_rectangle->x_pixels + resolve_clear_rectangle->width_pixels) *
             resolve_clear_scale_x);
    clear_rect.bottom =
        LONG((resolve_clear_rectangle->y_pixels + resolve_clear_rectangle->height_pixels) *
             resolve_clear_scale_y);
  }

  // Do host depth storing for the depth destination (assuming there can be only
  // one depth destination) where depth destination == host depth source.
  bool host_depth_store_set_up = false;
  for (uint32_t i = 0; i < render_target_count; ++i) {
    RenderTarget* dest_rt = render_targets[i];
    if (!dest_rt) {
      continue;
    }
    auto& dest_d3d12_rt = *static_cast<D3D12RenderTarget*>(dest_rt);
    RenderTargetKey dest_rt_key = dest_d3d12_rt.key();
    if (!dest_rt_key.is_depth) {
      continue;
    }
    const std::vector<Transfer>& depth_transfers = render_target_transfers[i];
    for (const Transfer& transfer : depth_transfers) {
      if (transfer.host_depth_source != dest_rt) {
        continue;
      }
      assert_false(dest_rt_key.scale_native);
      if (!host_depth_store_set_up) {
        // Bindings.
        // 0 - source.
        // 1 - EDRAM if bindful.
        ui::d3d12::util::DescriptorCpuGpuHandlePair host_depth_store_descriptors[2];
        if (!command_processor_.RequestOneUseSingleViewDescriptors(
                1 + uint32_t(!bindless_resources_used_), host_depth_store_descriptors)) {
          record_update_failure("host_depth_store_descriptors");
          continue;
        }
        command_list.D3DSetComputeRootSignature(host_depth_store_root_signature_);
        // Destination (EDRAM uint4 buffer).
        if (bindless_resources_used_) {
          command_list.D3DSetComputeRootDescriptorTable(
              kHostDepthStoreRootParameterDest,
              command_processor_.GetEdramUintPow2BindlessUAVHandlePair(4).second);
        } else {
          const ui::d3d12::util::DescriptorCpuGpuHandlePair& host_depth_store_descriptor_dest =
              host_depth_store_descriptors[1];
          WriteEdramUintPow2UAVDescriptor(host_depth_store_descriptor_dest.first, 4);
          command_list.D3DSetComputeRootDescriptorTable(kHostDepthStoreRootParameterDest,
                                                        host_depth_store_descriptor_dest.second);
        }
        // Depth source texture.
        const ui::d3d12::util::DescriptorCpuGpuHandlePair& host_depth_store_descriptor_source =
            host_depth_store_descriptors[0];
        device->CopyDescriptorsSimple(1, host_depth_store_descriptor_source.first,
                                      dest_d3d12_rt.descriptor_srv().GetHandle(),
                                      D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        command_list.D3DSetComputeRootDescriptorTable(kHostDepthStoreRootParameterSource,
                                                      host_depth_store_descriptor_source.second);
        // Render target constant.
        HostDepthStoreRenderTargetConstant host_depth_store_render_target_constant =
            GetHostDepthStoreRenderTargetConstant(dest_rt_key.pitch_tiles_at_32bpp,
                                                  msaa_2x_supported_);
        command_list.D3DSetComputeRoot32BitConstants(
            kHostDepthStoreRootParameterConstants,
            sizeof(host_depth_store_render_target_constant) / sizeof(uint32_t),
            &host_depth_store_render_target_constant,
            offsetof(HostDepthStoreConstants, render_target) / sizeof(uint32_t));
        // Barriers - don't need to try to combine them with the rest of
        // render target transfer barriers now - if this happens, after host
        // depth storing, NON_PIXEL_SHADER_RESOURCE -> DEPTH_WRITE will be done
        // anyway even in the best case, so it's not possible to have all the
        // barriers in one place here.
        TransitionEdramBuffer(D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        command_processor_.PushTransitionBarrier(
            dest_d3d12_rt.resource(),
            dest_d3d12_rt.SetResourceState(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        // Pipeline.
        command_processor_.SetExternalPipeline(
            host_depth_store_pipelines_[size_t(dest_rt_key.msaa_samples)]);
        host_depth_store_set_up = true;
      }
      Transfer::Rectangle transfer_rectangles[Transfer::kMaxRectanglesWithCutout];
      uint32_t transfer_rectangle_count = transfer.GetRectangles(
          dest_rt_key.base_tiles, dest_rt_key.pitch_tiles_at_32bpp, dest_rt_key.msaa_samples, false,
          transfer_rectangles, resolve_clear_rectangle);
      assert_not_zero(transfer_rectangle_count);
      HostDepthStoreRectangleConstant host_depth_store_rectangle_constant;
      for (uint32_t j = 0; j < transfer_rectangle_count; ++j) {
        uint32_t group_count_x, group_count_y;
        GetHostDepthStoreRectangleInfo(transfer_rectangles[j], dest_rt_key.msaa_samples,
                                       host_depth_store_rectangle_constant, group_count_x,
                                       group_count_y);
        command_list.D3DSetComputeRoot32BitConstants(
            kHostDepthStoreRootParameterConstants,
            sizeof(host_depth_store_rectangle_constant) / sizeof(uint32_t),
            &host_depth_store_rectangle_constant,
            offsetof(HostDepthStoreConstants, rectangle) / sizeof(uint32_t));
        {
          D3D12CommandProcessor::GpuTimingScope host_depth_timing(
              command_processor_, GpuTimingCategory::kTransferHostDepth);
          command_processor_.SubmitBarriers();
          command_list.D3DDispatch(group_count_x, group_count_y, 1);
        }
        if (command_processor_.GpuTimingEnabled()) {
          const uint64_t samples = (uint64_t(transfer_rectangles[j].width_pixels) *
                                    transfer_rectangles[j].height_pixels *
                                    GetKeyScaleX(dest_rt_key) * GetKeyScaleY(dest_rt_key))
                                   << uint32_t(dest_rt_key.msaa_samples);
          command_processor_.GpuTimingCount(GpuTimingCounter::kHostDepthStores, 1);
          command_processor_.GpuTimingCount(GpuTimingCounter::kHostDepthStoreSamples, samples);
          if (command_processor_.GpuTimingTransferLogFrame()) {
            std::fprintf(stderr,
                         "REX_GPU_TRANSFER_HOST_DEPTH_STORE frame=%llu dest_key=%08X base=%u "
                         "pitch=%u msaa=%u rect=%u,%u,%u,%u samples=%llu\n",
                         static_cast<unsigned long long>(command_processor_.GetCurrentFrame()),
                         dest_rt_key.key, dest_rt_key.base_tiles, dest_rt_key.GetPitchTiles(),
                         uint32_t(dest_rt_key.msaa_samples), transfer_rectangles[j].x_pixels,
                         transfer_rectangles[j].y_pixels, transfer_rectangles[j].width_pixels,
                         transfer_rectangles[j].height_pixels,
                         static_cast<unsigned long long>(samples));
          }
        }
        MarkEdramBufferModified();
        if (update_capture) {
          ++update_capture->depth_stores;
          if (update_capture->Record()) {
            const auto& c = *update_capture;
            const auto& rect = transfer_rectangles[j];
            std::fprintf(stderr,
                "REX_SCENE_UPDATE_DEPTH_STORE frame=%llu update=%llu next_draw=%llu slot=%u "
                "resource=%016llX key=%08X start=%u end=%u rect=%u,%u,%u,%u groups=%u,%u "
                "submission=%llu scope=queued_host_depth_store_not_gpu_completion\n",
                static_cast<unsigned long long>(c.frame), static_cast<unsigned long long>(c.update),
                static_cast<unsigned long long>(c.next_draw), i,
                static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(dest_d3d12_rt.resource())),
                dest_rt_key.key, transfer.start_tiles, transfer.end_tiles,
                rect.x_pixels, rect.y_pixels, rect.width_pixels, rect.height_pixels,
                group_count_x, group_count_y,
                static_cast<unsigned long long>(command_processor_.GetCurrentSubmission()));
          }
        }
      }
    }
    break;
  }

  // Try to insert as many barriers as possible in one place, hoping that in the
  // best case (no cross-copying between current render targets), barriers will
  // need to be only inserted here, not between transfers. In case of
  // cross-copying, if the destination use is going to happen before the source
  // use, choose the destination state, otherwise the source state - to match
  // the order in which transfers will actually happen (otherwise there will be
  // just a useless switch back and forth).
  for (uint32_t i = 0; i < render_target_count; ++i) {
    RenderTarget* dest_rt = render_targets[i];
    if (!dest_rt) {
      continue;
    }
    auto& dest_d3d12_rt = *static_cast<D3D12RenderTarget*>(dest_rt);
    const std::vector<Transfer>& dest_transfers = render_target_transfers[i];
    if (!resolve_clear_needed && dest_transfers.empty()) {
      continue;
    }
    // Transition the sources, only if not going to be used as destinations
    // earlier.
    for (const Transfer& transfer : render_target_transfers[i]) {
      bool source_previously_used_as_dest = false;
      bool host_depth_source_previously_used_as_dest = false;
      for (uint32_t j = 0; j < i; ++j) {
        if (render_target_transfers[j].empty()) {
          continue;
        }
        const RenderTarget* previous_rt = render_targets[j];
        if (transfer.source == previous_rt) {
          source_previously_used_as_dest = true;
        }
        if (transfer.host_depth_source == previous_rt) {
          host_depth_source_previously_used_as_dest = true;
        }
      }
      if (!source_previously_used_as_dest) {
        auto& source_d3d12_rt = *static_cast<D3D12RenderTarget*>(transfer.source);
        command_processor_.PushTransitionBarrier(
            source_d3d12_rt.resource(),
            source_d3d12_rt.SetResourceState(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
      }
      // transfer.host_depth_source == dest_rt means the EDRAM buffer will be
      // used instead, no need to transition.
      if (transfer.host_depth_source && transfer.host_depth_source != dest_rt &&
          !host_depth_source_previously_used_as_dest) {
        auto& host_depth_source_d3d12_rt =
            *static_cast<D3D12RenderTarget*>(transfer.host_depth_source);
        command_processor_.PushTransitionBarrier(
            host_depth_source_d3d12_rt.resource(),
            host_depth_source_d3d12_rt.SetResourceState(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
      }
    }
    // Transition the destination, only if not going to be used as a source
    // earlier.
    bool dest_used_previously_as_source = false;
    for (uint32_t j = 0; j < i; ++j) {
      for (const Transfer& previous_transfer : render_target_transfers[j]) {
        if (previous_transfer.source == dest_rt || previous_transfer.host_depth_source == dest_rt) {
          dest_used_previously_as_source = true;
          break;
        }
      }
    }
    if (!dest_used_previously_as_source) {
      D3D12_RESOURCE_STATES dest_state = dest_d3d12_rt.key().is_depth
                                             ? D3D12_RESOURCE_STATE_DEPTH_WRITE
                                             : D3D12_RESOURCE_STATE_RENDER_TARGET;
      command_processor_.PushTransitionBarrier(
          dest_d3d12_rt.resource(), dest_d3d12_rt.SetResourceState(dest_state), dest_state);
    }
  }
  if (host_depth_store_set_up) {
    // Will be reading copied host depth from the EDRAM buffer.
    TransitionEdramBuffer(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
  }

  // Copy source descriptors to the shader-visible heap.
  // Clear previously set shader-visible descriptor indices.
  for (uint32_t i = 0; i < render_target_count; ++i) {
    RenderTarget* dest_rt = render_targets[i];
    if (!dest_rt) {
      continue;
    }
    for (const Transfer& transfer : render_target_transfers[i]) {
      assert_not_null(transfer.source);
      auto& source_d3d12_rt = *static_cast<D3D12RenderTarget*>(transfer.source);
      source_d3d12_rt.SetTemporarySRVDescriptorIndex(UINT32_MAX);
      source_d3d12_rt.SetTemporarySRVDescriptorIndexStencil(UINT32_MAX);
      auto* host_depth_source_d3d12_rt =
          static_cast<D3D12RenderTarget*>(transfer.host_depth_source);
      if (host_depth_source_d3d12_rt) {
        host_depth_source_d3d12_rt->SetTemporarySRVDescriptorIndex(UINT32_MAX);
        host_depth_source_d3d12_rt->SetTemporarySRVDescriptorIndexStencil(UINT32_MAX);
      }
    }
  }
  current_temporary_descriptors_cpu_.clear();
  uint32_t host_depth_copy_srv_index;
  if (host_depth_store_set_up && !bindless_resources_used_) {
    host_depth_copy_srv_index = uint32_t(current_temporary_descriptors_cpu_.size());
    current_temporary_descriptors_cpu_.push_back(provider.OffsetViewDescriptor(
        edram_buffer_descriptor_heap_start_, uint32_t(EdramBufferDescriptorIndex::kR32UintSRV)));
  } else {
    host_depth_copy_srv_index = UINT32_MAX;
  }
  for (uint32_t i = 0; i < render_target_count; ++i) {
    RenderTarget* dest_rt = render_targets[i];
    if (!dest_rt) {
      continue;
    }
    bool dest_is_depth = dest_rt->key().is_depth;
    for (const Transfer& transfer : render_target_transfers[i]) {
      assert_not_null(transfer.source);
      auto& source_d3d12_rt = *static_cast<D3D12RenderTarget*>(transfer.source);
      if (source_d3d12_rt.temporary_srv_descriptor_index() == UINT32_MAX) {
        source_d3d12_rt.SetTemporarySRVDescriptorIndex(
            uint32_t(current_temporary_descriptors_cpu_.size()));
        current_temporary_descriptors_cpu_.push_back(source_d3d12_rt.descriptor_srv().GetHandle());
      }
      if (source_d3d12_rt.key().is_depth &&
          source_d3d12_rt.temporary_srv_descriptor_index_stencil() == UINT32_MAX) {
        source_d3d12_rt.SetTemporarySRVDescriptorIndexStencil(
            uint32_t(current_temporary_descriptors_cpu_.size()));
        current_temporary_descriptors_cpu_.push_back(
            source_d3d12_rt.descriptor_srv_stencil().GetHandle());
      }
      bool source_is_depth = source_d3d12_rt.key().is_depth;
      auto* host_depth_source_d3d12_rt =
          static_cast<D3D12RenderTarget*>(transfer.host_depth_source);
      // The host_depth_source_d3d12_rt == dest_rt case would use the EDRAM
      // buffer instead.
      if (host_depth_source_d3d12_rt && host_depth_source_d3d12_rt != dest_rt &&
          host_depth_source_d3d12_rt->temporary_srv_descriptor_index() == UINT32_MAX) {
        host_depth_source_d3d12_rt->SetTemporarySRVDescriptorIndex(
            uint32_t(current_temporary_descriptors_cpu_.size()));
        current_temporary_descriptors_cpu_.push_back(
            host_depth_source_d3d12_rt->descriptor_srv().GetHandle());
      }
    }
  }
  uint32_t descriptor_count = uint32_t(current_temporary_descriptors_cpu_.size());
  current_temporary_descriptors_gpu_.resize(descriptor_count);
  if (!command_processor_.RequestOneUseSingleViewDescriptors(
          descriptor_count, current_temporary_descriptors_gpu_.data())) {
    record_update_failure("transfer_descriptors");
    return;
  }
  for (uint32_t i = 0; i < descriptor_count; ++i) {
    device->CopyDescriptorsSimple(1, current_temporary_descriptors_gpu_[i].first,
                                  current_temporary_descriptors_cpu_[i],
                                  D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  }

  // Perform the transfers and clears.

  bool transfer_viewport_set = false;
  float pixels_to_ndc_unscaled = 2.0f / float(D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION);

  TransferRootSignatureIndex last_transfer_root_signature_index =
      TransferRootSignatureIndex::kCount;
  uint32_t transfer_root_parameters_set = 0;
  uint32_t last_descriptor_index_color = UINT32_MAX;
  uint32_t last_descriptor_index_depth = UINT32_MAX;
  uint32_t last_descriptor_index_stencil = UINT32_MAX;
  uint32_t last_descriptor_index_host_depth_non_copy = UINT32_MAX;
  bool last_descriptor_host_depth_is_copy = false;
  TransferAddressConstant last_address_constant;
  TransferAddressConstant last_host_depth_address_constant;

  for (uint32_t i = 0; i < render_target_count; ++i) {
    RenderTarget* dest_rt = render_targets[i];
    if (!dest_rt) {
      continue;
    }

    const std::vector<Transfer>& current_transfers = render_target_transfers[i];
    if (current_transfers.empty() && !resolve_clear_needed) {
      continue;
    }

    auto& dest_d3d12_rt = *static_cast<D3D12RenderTarget*>(dest_rt);
    RenderTargetKey dest_rt_key = dest_d3d12_rt.key();
    bool capture_embedded_mixed_scale_dest_after_transfer = false;

    // Probe160 localized the first bad scaled scene writer to a pass whose
    // color shader is unconditional, but whose depth/stencil test consumes the
    // D24FS8 surface at EDRAM base 0. Record only ownership changes for that
    // exact surface. This is observational, disabled by default, and bounded
    // across the process lifetime so it can't become per-frame tracing.
    static uint32_t embedded_scene_depth_transfer_trace_count = 0;
    const bool trace_embedded_scene_depth_transfer =
        REXCVAR_GET(embedded_scene_depth_transfer_trace) &&
        embedded_scene_depth_transfer_trace_count < 32 &&
        dest_rt_key.is_depth && !dest_rt_key.scale_native &&
        dest_rt_key.base_tiles == 0 &&
        dest_rt_key.pitch_tiles_at_32bpp == 16 &&
        dest_rt_key.msaa_samples == xenos::MsaaSamples::k2X &&
        dest_rt_key.GetDepthFormat() ==
            xenos::DepthRenderTargetFormat::kD24FS8 &&
        !current_transfers.empty();
    if (trace_embedded_scene_depth_transfer) {
      const uint32_t trace_ordinal =
          ++embedded_scene_depth_transfer_trace_count;
      std::fprintf(
          stderr,
          "REX_EMBEDDED_SCENE_DEPTH_TRANSFER ordinal=%u dest_key=0x%08X "
          "dest_scale=%ux%u transfers=%llu clear=%u\n",
          trace_ordinal, dest_rt_key.key, GetKeyScaleX(dest_rt_key),
          GetKeyScaleY(dest_rt_key),
          static_cast<unsigned long long>(current_transfers.size()),
          resolve_clear_needed ? 1u : 0u);
      for (size_t transfer_index = 0;
           transfer_index < current_transfers.size(); ++transfer_index) {
        const Transfer& transfer = current_transfers[transfer_index];
        assert_not_null(transfer.source);
        const RenderTargetKey source_key = transfer.source->key();
        const RenderTargetKey host_depth_key =
            transfer.host_depth_source
                ? transfer.host_depth_source->key()
                : RenderTargetKey();
        Transfer::Rectangle rectangles[Transfer::kMaxRectanglesWithCutout];
        const uint32_t rectangle_count = transfer.GetRectangles(
            dest_rt_key.base_tiles, dest_rt_key.GetPitchTiles(),
            dest_rt_key.msaa_samples, dest_rt_key.Is64bpp(), rectangles,
            resolve_clear_rectangle);
        std::fprintf(
            stderr,
            "REX_EMBEDDED_SCENE_DEPTH_TRANSFER_SOURCE ordinal=%u index=%llu "
            "tiles=%u-%u source_key=0x%08X source_depth=%u "
            "source_scale=%ux%u source_msaa=%u source_format=%u "
            "host_depth_key=0x%08X rectangles=%u\n",
            trace_ordinal, static_cast<unsigned long long>(transfer_index),
            transfer.start_tiles, transfer.end_tiles, source_key.key,
            source_key.is_depth, GetKeyScaleX(source_key),
            GetKeyScaleY(source_key), uint32_t(source_key.msaa_samples),
            source_key.resource_format, host_depth_key.key, rectangle_count);
        for (uint32_t rectangle_index = 0;
             rectangle_index < rectangle_count; ++rectangle_index) {
          const Transfer::Rectangle& rectangle = rectangles[rectangle_index];
          std::fprintf(
              stderr,
              "REX_EMBEDDED_SCENE_DEPTH_TRANSFER_RECT ordinal=%u "
              "source=%llu rectangle=%u xy=%u,%u size=%ux%u\n",
              trace_ordinal,
              static_cast<unsigned long long>(transfer_index), rectangle_index,
              rectangle.x_pixels, rectangle.y_pixels,
              rectangle.width_pixels, rectangle.height_pixels);
        }
      }
      std::fflush(stderr);
    }

    // Late barrier in case there was cross-copying that prevented merging of
    // barriers.
    D3D12_RESOURCE_STATES dest_state = dest_rt_key.is_depth ? D3D12_RESOURCE_STATE_DEPTH_WRITE
                                                            : D3D12_RESOURCE_STATE_RENDER_TARGET;
    command_processor_.PushTransitionBarrier(
        dest_d3d12_rt.resource(), dest_d3d12_rt.SetResourceState(dest_state), dest_state);

    if (!current_transfers.empty()) {
      are_current_command_list_render_targets_valid_ = false;
      if (dest_rt_key.is_depth) {
        D3D12_CPU_DESCRIPTOR_HANDLE dsv_handle = dest_d3d12_rt.descriptor_draw().GetHandle();
        command_list.D3DOMSetRenderTargets(0, nullptr, FALSE, &dsv_handle);
        if (!use_stencil_reference_output_) {
          command_processor_.SetStencilReference(UINT8_MAX);
        }
      } else {
        D3D12_CPU_DESCRIPTOR_HANDLE rtv_handle =
            dest_d3d12_rt.descriptor_load_separate().IsValid()
                ? dest_d3d12_rt.descriptor_load_separate().GetHandle()
                : dest_d3d12_rt.descriptor_draw().GetHandle();
        command_list.D3DOMSetRenderTargets(1, &rtv_handle, FALSE, nullptr);
      }

      uint32_t dest_pitch_tiles = dest_rt_key.GetPitchTiles();
      bool dest_is_64bpp = dest_rt_key.Is64bpp();
      float pixels_to_ndc_x =
          pixels_to_ndc_unscaled * GetKeyScaleX(dest_rt_key);
      float pixels_to_ndc_y =
          pixels_to_ndc_unscaled * GetKeyScaleY(dest_rt_key);

      // Gather shader keys and sort to reduce pipeline state and binding
      // switches. Also gather stencil rectangles to clear if needed.
      bool need_stencil_bit_draws = dest_rt_key.is_depth && !use_stencil_reference_output_;
      current_transfer_invocations_.clear();
      current_transfer_invocations_.reserve(current_transfers.size()
                                            << uint32_t(need_stencil_bit_draws));
      uint32_t rt_sort_index = 0;
      TransferShaderKey new_transfer_shader_key;
      new_transfer_shader_key.dest_msaa_samples = dest_rt_key.msaa_samples;
      new_transfer_shader_key.dest_resource_format = dest_rt_key.resource_format;
      new_transfer_shader_key.dest_scale_native = dest_rt_key.scale_native;
      uint32_t stencil_clear_rectangle_count = 0;
      for (uint32_t j = 0; j <= uint32_t(need_stencil_bit_draws); ++j) {
        // j == 0 - color or depth.
        // j == 1 - stencil bits.
        // Stencil bit writing always requires a different root signature,
        // handle these separately. Stencil never has a host depth source.
        // Clear previously set sort indices.
        for (const Transfer& transfer : current_transfers) {
          auto* host_depth_source_d3d12_rt =
              static_cast<D3D12RenderTarget*>(transfer.host_depth_source);
          if (host_depth_source_d3d12_rt) {
            host_depth_source_d3d12_rt->SetTemporarySortIndex(UINT32_MAX);
          }
          assert_not_null(transfer.source);
          auto& source_d3d12_rt = *static_cast<D3D12RenderTarget*>(transfer.source);
          source_d3d12_rt.SetTemporarySortIndex(UINT32_MAX);
        }
        for (const Transfer& transfer : current_transfers) {
          assert_not_null(transfer.source);
          auto& source_d3d12_rt = *static_cast<D3D12RenderTarget*>(transfer.source);
          D3D12RenderTarget* host_depth_source_d3d12_rt =
              j ? nullptr : static_cast<D3D12RenderTarget*>(transfer.host_depth_source);
          if (host_depth_source_d3d12_rt &&
              host_depth_source_d3d12_rt->temporary_sort_index() == UINT32_MAX) {
            host_depth_source_d3d12_rt->SetTemporarySortIndex(rt_sort_index++);
          }
          if (source_d3d12_rt.temporary_sort_index() == UINT32_MAX) {
            source_d3d12_rt.SetTemporarySortIndex(rt_sort_index++);
          }
          RenderTargetKey source_rt_key = source_d3d12_rt.key();
          new_transfer_shader_key.source_msaa_samples = source_rt_key.msaa_samples;
          new_transfer_shader_key.source_resource_format = source_rt_key.resource_format;
          new_transfer_shader_key.source_scale_native =
              source_rt_key.scale_native;
          assert_true(!host_depth_source_d3d12_rt ||
                      host_depth_source_d3d12_rt->key().scale_native ==
                          dest_rt_key.scale_native);
          bool host_depth_source_is_copy = host_depth_source_d3d12_rt == &dest_d3d12_rt;
          new_transfer_shader_key.host_depth_source_is_copy = host_depth_source_is_copy;
          // The host depth copy buffer has only raw samples.
          new_transfer_shader_key.host_depth_source_msaa_samples =
              (host_depth_source_d3d12_rt && !host_depth_source_is_copy)
                  ? host_depth_source_d3d12_rt->key().msaa_samples
                  : xenos::MsaaSamples::k1X;
          if (j) {
            new_transfer_shader_key.mode = source_rt_key.is_depth
                                               ? TransferMode::kDepthToStencilBit
                                               : TransferMode::kColorToStencilBit;
            stencil_clear_rectangle_count += transfer.GetRectangles(
                dest_rt_key.base_tiles, dest_pitch_tiles, dest_rt_key.msaa_samples, dest_is_64bpp,
                nullptr, resolve_clear_rectangle);
          } else {
            if (dest_rt_key.is_depth) {
              if (host_depth_source_d3d12_rt) {
                new_transfer_shader_key.mode = source_rt_key.is_depth
                                                   ? TransferMode::kDepthAndHostDepthToDepth
                                                   : TransferMode::kColorAndHostDepthToDepth;
              } else {
                new_transfer_shader_key.mode = source_rt_key.is_depth ? TransferMode::kDepthToDepth
                                                                      : TransferMode::kColorToDepth;
              }
            } else {
              new_transfer_shader_key.mode = source_rt_key.is_depth ? TransferMode::kDepthToColor
                                                                    : TransferMode::kColorToColor;
            }
          }
          current_transfer_invocations_.emplace_back(transfer, new_transfer_shader_key);
          if (j) {
            current_transfer_invocations_.back().transfer.host_depth_source = nullptr;
          }
        }
      }
      std::sort(current_transfer_invocations_.begin(), current_transfer_invocations_.end());

      // The old draw-side mixed-scale observer only caught a uniform
      // initialization pass. Inspect the actual ownership transfers in the
      // selected frame and preserve the first source with real image content.
      // This is disabled by default, restricted to The Darkness' dynamically
      // observed color allocation at EDRAM base 0x300, and bounded for the
      // process lifetime. It is diagnostic only and never changes guest data.
      static uint32_t embedded_mixed_scale_transfer_inspection_count = 0;
      static bool embedded_mixed_scale_nonuniform_transfer_captured = false;
      if (REXCVAR_GET(embedded_mixed_scale_transition_capture) &&
          IsCurrentEmbeddedGameplayCaptureFrame() &&
          !embedded_mixed_scale_nonuniform_transfer_captured &&
          !dest_rt_key.is_depth && dest_rt_key.base_tiles == 0x300) {
        for (const TransferInvocation& invocation :
             current_transfer_invocations_) {
          if (embedded_mixed_scale_transfer_inspection_count >= 64) {
            break;
          }
          const TransferShaderKey shader_key = invocation.shader_key;
          if (shader_key.mode != TransferMode::kColorToColor ||
              shader_key.source_scale_native ==
                  shader_key.dest_scale_native ||
              !invocation.transfer.source) {
            continue;
          }
          auto* source = static_cast<D3D12RenderTarget*>(
              invocation.transfer.source);
          const RenderTargetKey source_key = source->key();
          if (source_key.is_depth || source_key.base_tiles != 0x300) {
            continue;
          }
          // CaptureEmbeddedColorTarget's detailed content summary interprets
          // eight-byte FP16 pixels. The dynamically reached HDR ownership pair
          // uses this format; ignore unrelated aliases rather than
          // misclassifying a different host representation.
          if (source->resource()->GetDesc().Format !=
                  DXGI_FORMAT_R16G16B16A16_FLOAT ||
              dest_d3d12_rt.resource()->GetDesc().Format !=
                  DXGI_FORMAT_R16G16B16A16_FLOAT) {
            continue;
          }
          const uint32_t inspection_ordinal =
              ++embedded_mixed_scale_transfer_inspection_count;
          EmbeddedColorTargetDiagnosticSummary source_summary;
          const bool inspected = CaptureEmbeddedColorTarget(
              source, "mixed_scale_transfer_source_inspect", nullptr,
              &source_summary);
          std::fprintf(
              stderr,
              "REX_EMBEDDED_MIXED_SCALE_TRANSFER ordinal=%u inspected=%u "
              "source_key=0x%08X dest_key=0x%08X source_scale=%ux%u "
              "dest_scale=%ux%u source_msaa=%u dest_msaa=%u "
              "source_format=%u dest_format=%u tiles=%u-%u "
              "source_size=%ux%u source_samples=%u source_hash=0x%08X "
              "source_unique_capped=%u\n",
              inspection_ordinal, inspected ? 1u : 0u, source_key.key,
              dest_rt_key.key, GetKeyScaleX(source_key),
              GetKeyScaleY(source_key), GetKeyScaleX(dest_rt_key),
              GetKeyScaleY(dest_rt_key),
              uint32_t(source_key.msaa_samples),
              uint32_t(dest_rt_key.msaa_samples),
              source_key.resource_format, dest_rt_key.resource_format,
              invocation.transfer.start_tiles,
              invocation.transfer.end_tiles, source_summary.width,
              source_summary.height, source_summary.source_samples,
              source_summary.fnv1a,
              source_summary.unique_pixel_values_capped);
          std::fflush(stderr);
          if (!inspected ||
              source_summary.unique_pixel_values_capped < 8) {
            continue;
          }
          embedded_mixed_scale_nonuniform_transfer_captured = true;
          capture_embedded_mixed_scale_dest_after_transfer = true;
          const bool source_captured = CaptureEmbeddedColorTarget(
              source, "mixed_scale_transfer_source_nonuniform",
              "rex_mixed_scale_transfer_source_nonuniform_fp16.bin");
          std::fprintf(
              stderr,
              "REX_EMBEDDED_MIXED_SCALE_TRANSFER_SOURCE result=%u "
              "ordinal=%u path=%s\n",
              source_captured ? 1u : 0u, inspection_ordinal,
              "rex_mixed_scale_transfer_source_nonuniform_fp16.bin");
          std::fflush(stderr);
          break;
        }
      }

      // Clear the stencil to 0 where it will be loaded - will be setting the
      // bits that need to be 1 by discarding samples. Clearing everything here
      // to reduce context switches internally in the driver if clear causes
      // them.
      ID3D12PipelineState* stencil_clear_pipeline =
          stencil_clear_rectangle_count
              ? transfer_stencil_clear_pipelines_
                    [size_t(dest_rt_key.GetDepthFormat() ==
                            xenos::DepthRenderTargetFormat::kD24FS8)]
                    [size_t(dest_rt_key.msaa_samples)]
              : nullptr;
      if (stencil_clear_pipeline) {
        // d3d12_transfer_stencil_clear_by_draw: the same rectangles, each a
        // viewport and scissor for one full-viewport triangle.
        D3D12CommandProcessor::GpuTimingScope stencil_clear_timing(
            command_processor_, GpuTimingCategory::kTransferStencil);
        command_processor_.SetExternalGraphicsRootSignature(uint32_rtv_clear_root_signature_);
        command_processor_.SetExternalPipeline(stencil_clear_pipeline);
        command_processor_.SetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        command_processor_.SetStencilReference(0);
        command_processor_.SubmitBarriers();
        for (const Transfer& transfer : current_transfers) {
          Transfer::Rectangle transfer_stencil_clear_rectangles[Transfer::kMaxRectanglesWithCutout];
          uint32_t transfer_stencil_clear_rectangle_count = transfer.GetRectangles(
              dest_rt_key.base_tiles, dest_pitch_tiles, dest_rt_key.msaa_samples, dest_is_64bpp,
              transfer_stencil_clear_rectangles, resolve_clear_rectangle);
          for (uint32_t j = 0; j < transfer_stencil_clear_rectangle_count; ++j) {
            const Transfer::Rectangle& stencil_clear_rectangle =
                transfer_stencil_clear_rectangles[j];
            D3D12_RECT stencil_clear_rect;
            stencil_clear_rect.left =
                LONG(stencil_clear_rectangle.x_pixels * GetKeyScaleX(dest_rt_key));
            stencil_clear_rect.top =
                LONG(stencil_clear_rectangle.y_pixels * GetKeyScaleY(dest_rt_key));
            stencil_clear_rect.right =
                LONG((stencil_clear_rectangle.x_pixels + stencil_clear_rectangle.width_pixels) *
                     GetKeyScaleX(dest_rt_key));
            stencil_clear_rect.bottom =
                LONG((stencil_clear_rectangle.y_pixels + stencil_clear_rectangle.height_pixels) *
                     GetKeyScaleY(dest_rt_key));
            if (stencil_clear_rect.right <= stencil_clear_rect.left ||
                stencil_clear_rect.bottom <= stencil_clear_rect.top) {
              continue;
            }
            D3D12_VIEWPORT stencil_clear_viewport;
            stencil_clear_viewport.TopLeftX = float(stencil_clear_rect.left);
            stencil_clear_viewport.TopLeftY = float(stencil_clear_rect.top);
            stencil_clear_viewport.Width = float(stencil_clear_rect.right - stencil_clear_rect.left);
            stencil_clear_viewport.Height =
                float(stencil_clear_rect.bottom - stencil_clear_rect.top);
            stencil_clear_viewport.MinDepth = 0.0f;
            stencil_clear_viewport.MaxDepth = 1.0f;
            command_processor_.SetViewport(stencil_clear_viewport);
            command_processor_.SetScissorRect(stencil_clear_rect);
            command_list.D3DDrawInstanced(3, 1, 0, 0);
          }
        }
        // The stencil bit draws use the reference 0xFF; the transfer viewport
        // and scissor are set again below.
        command_processor_.SetStencilReference(UINT8_MAX);
        transfer_viewport_set = false;
      } else if (stencil_clear_rectangle_count) {
        D3D12CommandProcessor::GpuTimingScope stencil_clear_timing(
            command_processor_, GpuTimingCategory::kTransferStencil);
        command_processor_.SubmitBarriers();
        D3D12_RECT* stencil_clear_rect_write_ptr = command_list.ClearDepthStencilViewAllocatedRects(
            dest_d3d12_rt.descriptor_draw().GetHandle(), D3D12_CLEAR_FLAG_STENCIL, 0.0f, 0,
            stencil_clear_rectangle_count);
        assert_not_null(stencil_clear_rect_write_ptr);
        for (const Transfer& transfer : current_transfers) {
          Transfer::Rectangle transfer_stencil_clear_rectangles[Transfer::kMaxRectanglesWithCutout];
          uint32_t transfer_stencil_clear_rectangle_count = transfer.GetRectangles(
              dest_rt_key.base_tiles, dest_pitch_tiles, dest_rt_key.msaa_samples, dest_is_64bpp,
              transfer_stencil_clear_rectangles, resolve_clear_rectangle);
          for (uint32_t j = 0; j < transfer_stencil_clear_rectangle_count; ++j) {
            const Transfer::Rectangle& stencil_clear_rectangle =
                transfer_stencil_clear_rectangles[j];
            stencil_clear_rect_write_ptr->left =
                LONG(stencil_clear_rectangle.x_pixels *
                     GetKeyScaleX(dest_rt_key));
            stencil_clear_rect_write_ptr->top =
                LONG(stencil_clear_rectangle.y_pixels *
                     GetKeyScaleY(dest_rt_key));
            stencil_clear_rect_write_ptr->right =
                LONG((stencil_clear_rectangle.x_pixels + stencil_clear_rectangle.width_pixels) *
                     GetKeyScaleX(dest_rt_key));
            stencil_clear_rect_write_ptr->bottom =
                LONG((stencil_clear_rectangle.y_pixels + stencil_clear_rectangle.height_pixels) *
                     GetKeyScaleY(dest_rt_key));
            ++stencil_clear_rect_write_ptr;
          }
        }
      }

      // Perform the transfers for the render target.

      if (!transfer_viewport_set) {
        transfer_viewport_set = true;
        // Will be passing NDC directly, set the viewport to the maximum host
        // render target size for simplicity. Using a power-of-two scale for
        // exact pixel coordinates.
        D3D12_VIEWPORT transfer_viewport;
        transfer_viewport.TopLeftX = 0.0f;
        transfer_viewport.TopLeftY = 0.0f;
        transfer_viewport.Width = float(D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION);
        transfer_viewport.Height = float(D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION);
        transfer_viewport.MinDepth = 0.0f;
        transfer_viewport.MaxDepth = 1.0f;
        command_processor_.SetViewport(transfer_viewport);
        // TODO(Triang3l): Reduce scissor to the smallest transfer region for
        // more tiling friendliness.
        D3D12_RECT transfer_scissor;
        transfer_scissor.left = 0;
        transfer_scissor.top = 0;
        transfer_scissor.right = LONG(D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION);
        transfer_scissor.bottom = LONG(D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION);
        command_processor_.SetScissorRect(transfer_scissor);
      }

      for (auto it = current_transfer_invocations_.cbegin();
           it != current_transfer_invocations_.cend(); ++it) {
        const TransferInvocation& transfer_invocation_first = *it;
        // Will be merging transfers from the same source into one mesh.
        auto it_merged_first = it, it_merged_last = it;
        uint32_t transfer_rectangle_count = transfer_invocation_first.transfer.GetRectangles(
            dest_rt_key.base_tiles, dest_pitch_tiles, dest_rt_key.msaa_samples, dest_is_64bpp,
            nullptr, resolve_clear_rectangle);
        for (auto it_merge = std::next(it_merged_first);
             it_merge != current_transfer_invocations_.cend(); ++it_merge) {
          if (!transfer_invocation_first.CanBeMergedIntoOneDraw(*it_merge)) {
            break;
          }
          transfer_rectangle_count += it_merge->transfer.GetRectangles(
              dest_rt_key.base_tiles, dest_pitch_tiles, dest_rt_key.msaa_samples, dest_is_64bpp,
              nullptr, resolve_clear_rectangle);
          it_merged_last = it_merge;
        }
        assert_not_zero(transfer_rectangle_count);
        // Skip the merged transfers in the subsequent iterations.
        it = it_merged_last;

        assert_not_null(it->transfer.source);
        auto& source_d3d12_rt = *static_cast<D3D12RenderTarget*>(it->transfer.source);
        auto* host_depth_source_d3d12_rt =
            static_cast<D3D12RenderTarget*>(it->transfer.host_depth_source);
        TransferShaderKey transfer_shader_key = it->shader_key;
        const TransferModeInfo& transfer_mode_info =
            kTransferModes[size_t(transfer_shader_key.mode)];
        TransferRootSignatureIndex transfer_root_signature_index =
            use_stencil_reference_output_ ? transfer_mode_info.root_signature_with_stencil_ref
                                          : transfer_mode_info.root_signature_no_stencil_ref;
        uint32_t transfer_root_parameters_used =
            kTransferUsedRootParameters[size_t(transfer_root_signature_index)];
        bool is_stencil_bit =
            (transfer_root_parameters_used & kTransferUsedRootParameterStencilMaskConstantBit) != 0;

        // Late barriers in case there was cross-copying that prevented merging
        // of barriers.
        command_processor_.PushTransitionBarrier(
            source_d3d12_rt.resource(),
            source_d3d12_rt.SetResourceState(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        if (host_depth_source_d3d12_rt) {
          if (transfer_shader_key.host_depth_source_is_copy) {
            // Reading copied host depth from the EDRAM buffer.
            TransitionEdramBuffer(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
          } else {
            // Reading host depth from the texture.
            command_processor_.PushTransitionBarrier(
                host_depth_source_d3d12_rt->resource(),
                host_depth_source_d3d12_rt->SetResourceState(
                    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
          }
        }

        uint32_t transfer_vertex_count = 6 * transfer_rectangle_count;
        D3D12_VERTEX_BUFFER_VIEW transfer_rectangle_buffer_view;
        transfer_rectangle_buffer_view.StrideInBytes = sizeof(float) * 2;
        transfer_rectangle_buffer_view.SizeInBytes =
            transfer_rectangle_buffer_view.StrideInBytes * transfer_vertex_count;
        float* transfer_rectangle_write_ptr =
            reinterpret_cast<float*>(transfer_vertex_buffer_pool_->Request(
                current_submission, transfer_rectangle_buffer_view.SizeInBytes, sizeof(float),
                nullptr, nullptr, &transfer_rectangle_buffer_view.BufferLocation));
        if (!transfer_rectangle_write_ptr) {
          record_update_failure("transfer_vertex_allocation");
          continue;
        }
        uint64_t transfer_area = 0;
        for (auto it_merged = it_merged_first; it_merged <= it_merged_last; ++it_merged) {
          Transfer::Rectangle transfer_invocation_rectangles[Transfer::kMaxRectanglesWithCutout];
          uint32_t transfer_invocation_rectangle_count = it_merged->transfer.GetRectangles(
              dest_rt_key.base_tiles, dest_pitch_tiles, dest_rt_key.msaa_samples, dest_is_64bpp,
              transfer_invocation_rectangles, resolve_clear_rectangle);
          assert_not_zero(transfer_invocation_rectangle_count);
          for (uint32_t j = 0; j < transfer_invocation_rectangle_count; ++j) {
            const Transfer::Rectangle& transfer_rectangle = transfer_invocation_rectangles[j];
            transfer_area += uint64_t(transfer_rectangle.width_pixels) * transfer_rectangle.height_pixels;
            float transfer_rectangle_x0 = -1.0f + transfer_rectangle.x_pixels * pixels_to_ndc_x;
            float transfer_rectangle_y0 = 1.0f - transfer_rectangle.y_pixels * pixels_to_ndc_y;
            float transfer_rectangle_x1 =
                transfer_rectangle_x0 + transfer_rectangle.width_pixels * pixels_to_ndc_x;
            float transfer_rectangle_y1 =
                transfer_rectangle_y0 - transfer_rectangle.height_pixels * pixels_to_ndc_y;
            // O-*
            // |/
            // *
            *(transfer_rectangle_write_ptr++) = transfer_rectangle_x0;
            *(transfer_rectangle_write_ptr++) = transfer_rectangle_y0;
            // *-O
            // |/
            // *
            *(transfer_rectangle_write_ptr++) = transfer_rectangle_x1;
            *(transfer_rectangle_write_ptr++) = transfer_rectangle_y0;
            // *-*
            // |/
            // O
            *(transfer_rectangle_write_ptr++) = transfer_rectangle_x0;
            *(transfer_rectangle_write_ptr++) = transfer_rectangle_y1;
            //   *
            //  /|
            // O-*
            *(transfer_rectangle_write_ptr++) = transfer_rectangle_x0;
            *(transfer_rectangle_write_ptr++) = transfer_rectangle_y1;
            //   O
            //  /|
            // *-*
            *(transfer_rectangle_write_ptr++) = transfer_rectangle_x1;
            *(transfer_rectangle_write_ptr++) = transfer_rectangle_y0;
            //   *
            //  /|
            // *-O
            *(transfer_rectangle_write_ptr++) = transfer_rectangle_x1;
            *(transfer_rectangle_write_ptr++) = transfer_rectangle_y1;
          }
        }
        command_processor_.SetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        command_list.D3DIASetVertexBuffers(0, 1, &transfer_rectangle_buffer_view);

        ID3D12PipelineState* const* transfer_pipelines =
            GetOrCreateTransferPipelines(transfer_shader_key);
        if (!transfer_pipelines) {
          record_update_failure("transfer_pipeline");
          continue;
        }
        if (last_transfer_root_signature_index != transfer_root_signature_index) {
          last_transfer_root_signature_index = transfer_root_signature_index;
          command_processor_.SetExternalGraphicsRootSignature(
              transfer_root_signatures_[size_t(transfer_root_signature_index)]);
          transfer_root_parameters_set = 0;
        }

        // Invalidate outdated bindings.
        if (transfer_root_parameters_used & kTransferUsedRootParameterColorSRVBit) {
          uint32_t descriptor_index_color = source_d3d12_rt.temporary_srv_descriptor_index();
          assert_true(descriptor_index_color != UINT32_MAX);
          if (last_descriptor_index_color != descriptor_index_color) {
            last_descriptor_index_color = descriptor_index_color;
            transfer_root_parameters_set &= ~kTransferUsedRootParameterColorSRVBit;
          }
        }
        if (transfer_root_parameters_used & kTransferUsedRootParameterDepthSRVBit) {
          uint32_t descriptor_index_depth = source_d3d12_rt.temporary_srv_descriptor_index();
          assert_true(descriptor_index_depth != UINT32_MAX);
          if (last_descriptor_index_depth != descriptor_index_depth) {
            last_descriptor_index_depth = descriptor_index_depth;
            transfer_root_parameters_set &= ~kTransferUsedRootParameterDepthSRVBit;
          }
        }
        if (transfer_root_parameters_used & kTransferUsedRootParameterStencilSRVBit) {
          uint32_t descriptor_index_stencil =
              source_d3d12_rt.temporary_srv_descriptor_index_stencil();
          assert_true(descriptor_index_stencil != UINT32_MAX);
          if (last_descriptor_index_stencil != descriptor_index_stencil) {
            last_descriptor_index_stencil = descriptor_index_stencil;
            transfer_root_parameters_set &= ~kTransferUsedRootParameterStencilSRVBit;
          }
        }
        if (transfer_root_parameters_used & kTransferUsedRootParameterHostDepthSRVBit) {
          if (transfer_shader_key.host_depth_source_is_copy) {
            if (!last_descriptor_host_depth_is_copy) {
              last_descriptor_host_depth_is_copy = true;
              transfer_root_parameters_set &= ~kTransferUsedRootParameterHostDepthSRVBit;
            }
          } else {
            assert_not_null(host_depth_source_d3d12_rt);
            uint32_t descriptor_index_host_depth =
                host_depth_source_d3d12_rt->temporary_srv_descriptor_index();
            assert_true(descriptor_index_host_depth != UINT32_MAX);
            if (last_descriptor_host_depth_is_copy ||
                last_descriptor_index_host_depth_non_copy != descriptor_index_host_depth) {
              transfer_root_parameters_set &= ~kTransferUsedRootParameterHostDepthSRVBit;
            }
            last_descriptor_host_depth_is_copy = false;
            last_descriptor_index_host_depth_non_copy = descriptor_index_host_depth;
          }
        }
        if (transfer_root_parameters_used & kTransferUsedRootParameterAddressConstantBit) {
          RenderTargetKey source_rt_key = source_d3d12_rt.key();
          TransferAddressConstant address_constant;
          address_constant.dest_pitch = dest_pitch_tiles;
          address_constant.source_pitch = source_rt_key.GetPitchTiles();
          address_constant.source_to_dest =
              int32_t(dest_rt_key.base_tiles) - int32_t(source_rt_key.base_tiles);
          if (last_address_constant != address_constant) {
            last_address_constant = address_constant;
            transfer_root_parameters_set &= ~kTransferUsedRootParameterAddressConstantBit;
          }
        }
        if (transfer_root_parameters_used & kTransferUsedRootParameterHostDepthAddressConstantBit) {
          assert_not_null(host_depth_source_d3d12_rt);
          RenderTargetKey host_depth_source_rt_key = host_depth_source_d3d12_rt->key();
          TransferAddressConstant host_depth_address_constant;
          host_depth_address_constant.dest_pitch = dest_pitch_tiles;
          host_depth_address_constant.source_pitch = host_depth_source_rt_key.GetPitchTiles();
          host_depth_address_constant.source_to_dest =
              int32_t(dest_rt_key.base_tiles) - int32_t(host_depth_source_rt_key.base_tiles);
          if (last_host_depth_address_constant != host_depth_address_constant) {
            last_host_depth_address_constant = host_depth_address_constant;
            transfer_root_parameters_set &= ~kTransferUsedRootParameterHostDepthAddressConstantBit;
          }
        }

        // Apply the new bindings.
        uint32_t transfer_root_parameters_unset =
            transfer_root_parameters_used & ~transfer_root_parameters_set;
        if (transfer_root_parameters_unset &
            kTransferUsedRootParameterHostDepthAddressConstantBit) {
          command_list.D3DSetGraphicsRoot32BitConstants(
              rex::bit_count(transfer_root_parameters_used &
                             (kTransferUsedRootParameterHostDepthAddressConstantBit - 1)),
              sizeof(last_host_depth_address_constant) / sizeof(uint32_t),
              &last_host_depth_address_constant, 0);
          transfer_root_parameters_set |= kTransferUsedRootParameterHostDepthAddressConstantBit;
        }
        if (transfer_root_parameters_unset & kTransferUsedRootParameterHostDepthSRVBit) {
          D3D12_GPU_DESCRIPTOR_HANDLE descriptor_gpu_handle;
          if (last_descriptor_host_depth_is_copy) {
            if (bindless_resources_used_) {
              descriptor_gpu_handle =
                  command_processor_
                      .GetSystemBindlessViewHandlePair(
                          D3D12CommandProcessor::SystemBindlessView ::kEdramR32UintSRV)
                      .second;
            } else {
              assert_true(host_depth_copy_srv_index != UINT32_MAX);
              descriptor_gpu_handle =
                  current_temporary_descriptors_gpu_[host_depth_copy_srv_index].second;
            }
          } else {
            assert_true(last_descriptor_index_host_depth_non_copy != UINT32_MAX);
            descriptor_gpu_handle =
                current_temporary_descriptors_gpu_[last_descriptor_index_host_depth_non_copy]
                    .second;
          }
          command_list.D3DSetGraphicsRootDescriptorTable(
              rex::bit_count(transfer_root_parameters_used &
                             (kTransferUsedRootParameterHostDepthSRVBit - 1)),
              descriptor_gpu_handle);
          transfer_root_parameters_set |= kTransferUsedRootParameterHostDepthSRVBit;
        }
        if (transfer_root_parameters_unset & kTransferUsedRootParameterAddressConstantBit) {
          command_list.D3DSetGraphicsRoot32BitConstants(
              rex::bit_count(transfer_root_parameters_used &
                             (kTransferUsedRootParameterAddressConstantBit - 1)),
              sizeof(last_address_constant) / sizeof(uint32_t), &last_address_constant, 0);
          transfer_root_parameters_set |= kTransferUsedRootParameterAddressConstantBit;
        }
        if (transfer_root_parameters_unset & kTransferUsedRootParameterStencilSRVBit) {
          assert_true(last_descriptor_index_stencil != UINT32_MAX);
          command_list.D3DSetGraphicsRootDescriptorTable(
              rex::bit_count(transfer_root_parameters_used &
                             (kTransferUsedRootParameterStencilSRVBit - 1)),
              current_temporary_descriptors_gpu_[last_descriptor_index_stencil].second);
          transfer_root_parameters_set |= kTransferUsedRootParameterStencilSRVBit;
        }
        if (transfer_root_parameters_unset & kTransferUsedRootParameterDepthSRVBit) {
          assert_true(last_descriptor_index_depth != UINT32_MAX);
          command_list.D3DSetGraphicsRootDescriptorTable(
              rex::bit_count(transfer_root_parameters_used &
                             (kTransferUsedRootParameterDepthSRVBit - 1)),
              current_temporary_descriptors_gpu_[last_descriptor_index_depth].second);
          transfer_root_parameters_set |= kTransferUsedRootParameterDepthSRVBit;
        }
        if (transfer_root_parameters_unset & kTransferUsedRootParameterColorSRVBit) {
          assert_true(last_descriptor_index_color != UINT32_MAX);
          command_list.D3DSetGraphicsRootDescriptorTable(
              rex::bit_count(transfer_root_parameters_used &
                             (kTransferUsedRootParameterColorSRVBit - 1)),
              current_temporary_descriptors_gpu_[last_descriptor_index_color].second);
          transfer_root_parameters_set |= kTransferUsedRootParameterColorSRVBit;
        }

        // Draw the transfer rectangles.
        const GpuTimingCategory transfer_timing_category =
            is_stencil_bit ? GpuTimingCategory::kTransferStencil
                           : (dest_rt_key.is_depth ? GpuTimingCategory::kTransferDepth
                                                   : GpuTimingCategory::kTransferColor);
        D3D12CommandProcessor::GpuTimingScope transfer_draw_timing(command_processor_,
                                                                   transfer_timing_category);
        if (command_processor_.GpuTimingEnabled()) {
          const uint32_t passes = is_stencil_bit ? 8 : 1;
          const uint64_t samples = (transfer_area * GetKeyScaleX(dest_rt_key) *
                                    GetKeyScaleY(dest_rt_key) * passes)
                                   << uint32_t(dest_rt_key.msaa_samples);
          const GpuTimingCounter draws_counter =
              is_stencil_bit ? GpuTimingCounter::kTransferStencilDraws
                             : (dest_rt_key.is_depth ? GpuTimingCounter::kTransferDepthDraws
                                                     : GpuTimingCounter::kTransferColorDraws);
          command_processor_.GpuTimingCount(draws_counter, passes);
          command_processor_.GpuTimingCount(GpuTimingCounter(uint32_t(draws_counter) + 1), samples);
          if (command_processor_.GpuTimingTransferLogFrame()) {
            const RenderTargetKey source_log_key = source_d3d12_rt.key();
            std::fprintf(stderr,
                         "REX_GPU_TRANSFER frame=%llu dest_key=%08X dest_depth=%u dest_base=%u "
                         "dest_pitch=%u dest_format=%u dest_msaa=%u dest_native=%u "
                         "source_key=%08X source_depth=%u source_base=%u source_pitch=%u "
                         "source_format=%u source_msaa=%u mode=%u host_depth=%u merged=%u "
                         "rects=%u area=%llu passes=%u samples=%llu resolve_clear=%u\n",
                         static_cast<unsigned long long>(command_processor_.GetCurrentFrame()),
                         dest_rt_key.key, dest_rt_key.is_depth, dest_rt_key.base_tiles,
                         dest_pitch_tiles, dest_rt_key.resource_format,
                         uint32_t(dest_rt_key.msaa_samples), dest_rt_key.scale_native,
                         source_log_key.key, source_log_key.is_depth, source_log_key.base_tiles,
                         source_log_key.GetPitchTiles(), source_log_key.resource_format,
                         uint32_t(source_log_key.msaa_samples), uint32_t(transfer_shader_key.mode),
                         host_depth_source_d3d12_rt ? 1u : 0u,
                         uint32_t(std::distance(it_merged_first, it_merged_last) + 1),
                         transfer_rectangle_count, static_cast<unsigned long long>(transfer_area),
                         passes, static_cast<unsigned long long>(samples),
                         resolve_clear_needed ? 1u : 0u);
          }
        }
        command_processor_.SubmitBarriers();
        for (uint32_t j = 0; j <= uint32_t(is_stencil_bit) * 7; ++j) {
          if (is_stencil_bit) {
            uint32_t transfer_stencil_bit = uint32_t(1) << j;
            command_list.D3DSetGraphicsRoot32BitConstants(
                rex::bit_count(transfer_root_parameters_used &
                               (kTransferUsedRootParameterStencilMaskConstantBit - 1)),
                sizeof(transfer_stencil_bit) / sizeof(uint32_t), &transfer_stencil_bit, 0);
          }
          command_processor_.SetExternalPipeline(transfer_pipelines[j]);
          command_list.D3DDrawInstanced(transfer_vertex_count, 1, 0, 0);
          // Observe each original invocation in this actually queued merged draw.
          // Do not describe a requested transfer as executed after an allocation
          // or pipeline failure, and do not add commands to obtain evidence.
          if (update_capture) {
            for (auto observed = it_merged_first; observed <= it_merged_last; ++observed) {
              ++update_capture->commands;
              if (!update_capture->Record()) continue;
              const auto& c = *update_capture;
              const auto& transfer = observed->transfer;
              const auto* host_depth = static_cast<D3D12RenderTarget*>(transfer.host_depth_source);
              std::fprintf(stderr,
                  "REX_SCENE_UPDATE_COMMAND frame=%llu update=%llu next_draw=%llu slot=%u "
                  "start=%u end=%u source=%016llX source_key=%08X dest=%016llX dest_key=%08X "
                  "host_depth=%016llX mode=%u shader_key=%08X pass=%u passes=%u vertices=%u "
                  "submission=%llu scope=queued_transfer_draw_not_gpu_completion\n",
                  static_cast<unsigned long long>(c.frame), static_cast<unsigned long long>(c.update),
                  static_cast<unsigned long long>(c.next_draw), i, transfer.start_tiles, transfer.end_tiles,
                  static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(source_d3d12_rt.resource())),
                  source_d3d12_rt.key().key,
                  static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(dest_d3d12_rt.resource())),
                  dest_rt_key.key,
                  host_depth ? static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(host_depth->resource())) : 0,
                  uint32_t(transfer_shader_key.mode), transfer_shader_key.key,
                  j, is_stencil_bit ? 8u : 1u, transfer_vertex_count,
                  static_cast<unsigned long long>(command_processor_.GetCurrentSubmission()));
            }
          }
        }
      }
    }

    // Perform the clear.
    if (resolve_clear_needed) {
      uint64_t clear_value = render_target_resolve_clear_values[i];
      // Invoke only after the actual existing command is recorded. No new
      // command, readback, wait or transfer is introduced by this observer.
      const auto record_scene_clear_command = [&](const char* method, const float* values,
                                                   uint32_t value_count) {
        if (!scene_capture_context || render_target_count != 2 ||
            !scene_clear_command_budget.Record(i)) return;
        const auto& c = *scene_capture_context;
        const auto desc = dest_d3d12_rt.resource()->GetDesc();
        uint32_t words[4] = {};
        if (values && value_count <= 4) std::memcpy(words, values, value_count * sizeof(uint32_t));
        std::fprintf(stderr,
            "REX_SCENE_CLEAR_COMMAND frame=%llu resolve=%llu last_draw=%llu slot=%u "
            "method=%s resource=%p key=%08X depth=%u format=%u scale=%ux%u "
            "host_width=%llu host_height=%u host_format=%u host_samples=%u "
            "guest_rect=%u,%u,%u,%u host_rect=%ld,%ld,%ld,%ld "
            "value=%016llX float_count=%u float_words=%08X,%08X,%08X,%08X "
            "submission=%llu scope=queued_host_clear_not_gpu_completion\n",
            static_cast<unsigned long long>(c.frame), static_cast<unsigned long long>(c.ordinal),
            static_cast<unsigned long long>(c.last_draw), i, method,
            static_cast<void*>(dest_d3d12_rt.resource()), dest_rt_key.key,
            dest_rt_key.is_depth, dest_rt_key.resource_format,
            GetKeyScaleX(dest_rt_key), GetKeyScaleY(dest_rt_key),
            static_cast<unsigned long long>(desc.Width), desc.Height, uint32_t(desc.Format),
            desc.SampleDesc.Count, resolve_clear_rectangle->x_pixels,
            resolve_clear_rectangle->y_pixels, resolve_clear_rectangle->width_pixels,
            resolve_clear_rectangle->height_pixels, clear_rect.left, clear_rect.top,
            clear_rect.right, clear_rect.bottom, static_cast<unsigned long long>(clear_value),
            value_count, words[0], words[1], words[2], words[3],
            static_cast<unsigned long long>(command_processor_.GetCurrentSubmission()));
      };
      if (dest_rt_key.is_depth) {
        uint32_t depth_guest_clear_value = (uint32_t(clear_value) >> 8) & 0xFFFFFF;
        float depth_host_clear_value = 0.0f;
        switch (dest_rt_key.GetDepthFormat()) {
          case xenos::DepthRenderTargetFormat::kD24S8:
            depth_host_clear_value = xenos::UNorm24To32(depth_guest_clear_value);
            break;
          case xenos::DepthRenderTargetFormat::kD24FS8:
            // Taking [0, 2) -> [0, 1) remapping into account.
            depth_host_clear_value = xenos::Float20e4To32(depth_guest_clear_value) * 0.5f;
            break;
        }
        command_processor_.PushTransitionBarrier(
            dest_d3d12_rt.resource(),
            dest_d3d12_rt.SetResourceState(D3D12_RESOURCE_STATE_DEPTH_WRITE),
            D3D12_RESOURCE_STATE_DEPTH_WRITE);
        command_processor_.SubmitBarriers();
        command_list.D3DClearDepthStencilView(dest_d3d12_rt.descriptor_draw().GetHandle(),
                                              D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL,
                                              depth_host_clear_value, UINT(clear_value) & 0xFF, 1,
                                              &clear_rect);
        record_scene_clear_command("depth_stencil_clear", &depth_host_clear_value, 1);
      } else {
        float color_clear_value[4] = {};
        bool clear_via_drawing = false;
        switch (dest_rt_key.GetColorFormat()) {
          case xenos::ColorRenderTargetFormat::k_8_8_8_8: {
            for (uint32_t j = 0; j < 4; ++j) {
              color_clear_value[j] = ((clear_value >> (j * 8)) & 0xFF) * (1.0f / 0xFF);
            }
          } break;
          case xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA: {
            // 8_8_8_8_GAMMA is represented by linear stored in
            // R16G16B16A16_UNORM.
            for (uint32_t j = 0; j < 4; ++j) {
              color_clear_value[j] = ((clear_value >> (j * 8)) & 0xFF) * (1.0f / 0xFF);
            }
            for (uint32_t j = 0; j < 3; ++j) {
              color_clear_value[j] = xenos::PWLGammaToLinear(color_clear_value[j]);
            }
          } break;
          case xenos::ColorRenderTargetFormat::k_2_10_10_10:
          case xenos::ColorRenderTargetFormat::k_2_10_10_10_AS_10_10_10_10: {
            for (uint32_t j = 0; j < 3; ++j) {
              color_clear_value[j] = ((clear_value >> (j * 10)) & 0x3FF) * (1.0f / 0x3FF);
            }
            color_clear_value[3] = ((clear_value >> 30) & 0x3) * (1.0f / 0x3);
          } break;
          case xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT:
          case xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT_AS_16_16_16_16: {
            for (uint32_t j = 0; j < 3; ++j) {
              color_clear_value[j] = xenos::Float7e3To32((clear_value >> (j * 10)) & 0x3FF);
            }
            color_clear_value[3] = ((clear_value >> 30) & 0x3) * (1.0f / 0x3);
          } break;
          case xenos::ColorRenderTargetFormat::k_16_16:
          case xenos::ColorRenderTargetFormat::k_16_16_FLOAT: {
            // Using uint for loading both. Disregarding the current -32...32
            // vs. -1...1 settings for consistency with color clear via depth
            // aliasing.
            for (uint32_t j = 0; j < 2; ++j) {
              color_clear_value[j] = float((clear_value >> (j * 16)) & 0xFFFF);
            }
          } break;
          case xenos::ColorRenderTargetFormat::k_16_16_16_16:
          case xenos::ColorRenderTargetFormat::k_16_16_16_16_FLOAT: {
            // Using uint for loading both. Disregarding the current -32...32
            // vs. -1...1 settings for consistency with color clear via depth
            // aliasing.
            for (uint32_t j = 0; j < 4; ++j) {
              color_clear_value[j] = float((clear_value >> (j * 16)) & 0xFFFF);
            }
          } break;
          case xenos::ColorRenderTargetFormat::k_32_FLOAT: {
            // Using uint for proper denormal and NaN handling.
            color_clear_value[0] = float(uint32_t(clear_value));
            // Numbers > 2^24 can't be represented with a step of 1 as floats,
            // need to clear by drawing a uint rectangle.
            if (uint64_t(color_clear_value[0]) != uint32_t(clear_value)) {
              clear_via_drawing = true;
            }
          } break;
          case xenos::ColorRenderTargetFormat::k_32_32_FLOAT: {
            // Using uint for proper denormal and NaN handling.
            color_clear_value[0] = float(uint32_t(clear_value));
            color_clear_value[1] = float(uint32_t(clear_value >> 32));
            // Numbers > 2^24 can't be represented with a step of 1 as floats,
            // need to clear by drawing a uint rectangle.
            if (uint64_t(color_clear_value[0]) != uint32_t(clear_value) ||
                uint64_t(color_clear_value[1]) != uint32_t(clear_value >> 32)) {
              clear_via_drawing = true;
            }
          } break;
        }
        command_processor_.PushTransitionBarrier(
            dest_d3d12_rt.resource(),
            dest_d3d12_rt.SetResourceState(D3D12_RESOURCE_STATE_RENDER_TARGET),
            D3D12_RESOURCE_STATE_RENDER_TARGET);
        if (clear_via_drawing) {
          D3D12_CPU_DESCRIPTOR_HANDLE clear_rtv_handle =
              dest_d3d12_rt.descriptor_load_separate().IsValid()
                  ? dest_d3d12_rt.descriptor_load_separate().GetHandle()
                  : dest_d3d12_rt.descriptor_draw().GetHandle();
          command_list.D3DOMSetRenderTargets(1, &clear_rtv_handle, FALSE, nullptr);
          are_current_command_list_render_targets_valid_ = true;
          D3D12_VIEWPORT clear_viewport;
          clear_viewport.TopLeftX = float(clear_rect.left);
          clear_viewport.TopLeftY = float(clear_rect.top);
          clear_viewport.Width = float(clear_rect.right - clear_rect.left);
          clear_viewport.Height = float(clear_rect.bottom - clear_rect.top);
          clear_viewport.MinDepth = 0.0f;
          clear_viewport.MaxDepth = 1.0f;
          command_processor_.SetViewport(clear_viewport);
          command_processor_.SetScissorRect(clear_rect);
          command_processor_.SetExternalGraphicsRootSignature(uint32_rtv_clear_root_signature_);
          uint32_t clear_via_drawing_value[2] = {uint32_t(clear_value),
                                                 uint32_t(clear_value >> 32)};
          command_list.D3DSetGraphicsRoot32BitConstants(0, 2, clear_via_drawing_value, 0);
          command_processor_.SetExternalPipeline(uint32_rtv_clear_pipelines_[size_t(
              dest_rt_key.GetColorFormat() ==
              xenos::ColorRenderTargetFormat::k_32_32_FLOAT)][size_t(dest_rt_key.msaa_samples)]);
          command_processor_.SetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
          command_list.D3DDrawInstanced(3, 1, 0, 0);
          record_scene_clear_command("uint_draw", nullptr, 0);
        } else {
          command_processor_.SubmitBarriers();
          command_list.D3DClearRenderTargetView(
              dest_d3d12_rt.descriptor_load_separate().IsValid()
                  ? dest_d3d12_rt.descriptor_load_separate().GetHandle()
                  : dest_d3d12_rt.descriptor_draw().GetHandle(),
              color_clear_value, 1, &clear_rect);
          record_scene_clear_command("rtv_clear", color_clear_value, 4);
        }
      }
    }
    if (capture_embedded_mixed_scale_dest_after_transfer) {
      constexpr char kMixedScaleTransferDestPath[] =
          "rex_mixed_scale_transfer_dest_after_fp16.bin";
      EmbeddedColorTargetDiagnosticSummary dest_summary;
      const bool captured = CaptureEmbeddedColorTarget(
          &dest_d3d12_rt, "mixed_scale_transfer_dest_after",
          kMixedScaleTransferDestPath, &dest_summary);
      // AwaitAllQueueOperationsCompletion starts a fresh command list. Make
      // the next draw re-establish its attachments rather than trusting stale
      // command-list-local binding state.
      are_current_command_list_render_targets_valid_ = false;
      std::fprintf(
          stderr,
          "REX_EMBEDDED_MIXED_SCALE_TRANSFER_DEST result=%u path=%s "
          "dest_key=0x%08X size=%ux%u samples=%u hash=0x%08X "
          "unique_capped=%u\n",
          captured ? 1u : 0u, kMixedScaleTransferDestPath,
          dest_rt_key.key, dest_summary.width, dest_summary.height,
          dest_summary.source_samples, dest_summary.fnv1a,
          dest_summary.unique_pixel_values_capped);
      std::fflush(stderr);
    }
  }
  if (update_capture) update_capture->helper_completed = true;
  if (scene_capture_context && resolve_clear_needed && render_target_count == 2) {
    const auto& c = *scene_capture_context;
    std::fprintf(stderr,
        "REX_SCENE_CLEAR_END frame=%llu resolve=%llu last_draw=%llu commands=%u "
        "target_bits=%X submission=%llu scope=queued_host_clear_not_gpu_completion\n",
        static_cast<unsigned long long>(c.frame), static_cast<unsigned long long>(c.ordinal),
        static_cast<unsigned long long>(c.last_draw), scene_clear_command_budget.count(),
        scene_clear_command_budget.target_bits,
        static_cast<unsigned long long>(command_processor_.GetCurrentSubmission()));
  }
}

void D3D12RenderTargetCache::SetCommandListRenderTargets(
    RenderTarget* const* depth_and_color_render_targets) {
  assert_true(GetPath() == Path::kHostRenderTargets);

  // Ensure the render targets are in the needed resource state.
  if (depth_and_color_render_targets[0]) {
    auto& d3d12_rt = *static_cast<D3D12RenderTarget*>(depth_and_color_render_targets[0]);
    command_processor_.PushTransitionBarrier(
        d3d12_rt.resource(), d3d12_rt.SetResourceState(D3D12_RESOURCE_STATE_DEPTH_WRITE),
        D3D12_RESOURCE_STATE_DEPTH_WRITE);
  }
  for (uint32_t i = 0; i < xenos::kMaxColorRenderTargets; ++i) {
    RenderTarget* render_target = depth_and_color_render_targets[1 + i];
    if (!render_target) {
      continue;
    }
    auto& d3d12_rt = *static_cast<D3D12RenderTarget*>(render_target);
    command_processor_.PushTransitionBarrier(
        d3d12_rt.resource(), d3d12_rt.SetResourceState(D3D12_RESOURCE_STATE_RENDER_TARGET),
        D3D12_RESOURCE_STATE_RENDER_TARGET);
  }

  // Bind the render targets.
  if (are_current_command_list_render_targets_valid_ &&
      std::memcmp(current_command_list_render_targets_, depth_and_color_render_targets,
                  sizeof(current_command_list_render_targets_))) {
    are_current_command_list_render_targets_valid_ = false;
  }
  if (!are_current_command_list_render_targets_valid_) {
    std::memcpy(current_command_list_render_targets_, depth_and_color_render_targets,
                sizeof(current_command_list_render_targets_));
    D3D12_CPU_DESCRIPTOR_HANDLE dsv_handle;
    if (depth_and_color_render_targets[0]) {
      dsv_handle = static_cast<const D3D12RenderTarget*>(depth_and_color_render_targets[0])
                       ->descriptor_draw()
                       .GetHandle();
    }
    D3D12_CPU_DESCRIPTOR_HANDLE rtv_handles[xenos::kMaxColorRenderTargets];
    uint32_t rtv_count = 0;
    for (uint32_t i = 0; i < xenos::kMaxColorRenderTargets; ++i) {
      const RenderTarget* render_target = depth_and_color_render_targets[1 + i];
      if (!render_target) {
        continue;
      }
      // Fill the gaps with a null descriptor.
      while (rtv_count < i) {
        rtv_handles[rtv_count++] = render_target->key().msaa_samples != xenos::MsaaSamples::k1X
                                       ? null_rtv_descriptor_ms_.GetHandle()
                                       : null_rtv_descriptor_ss_.GetHandle();
      }
      auto& d3d12_rt = *static_cast<const D3D12RenderTarget*>(render_target);
      rtv_handles[rtv_count++] = d3d12_rt.descriptor_draw().GetHandle();
    }
    command_processor_.GetDeferredCommandList().D3DOMSetRenderTargets(
        rtv_count, rtv_handles, FALSE, depth_and_color_render_targets[0] ? &dsv_handle : nullptr);
    are_current_command_list_render_targets_valid_ = true;
  }
}

ID3D12PipelineState* D3D12RenderTargetCache::GetOrCreateDumpPipeline(DumpPipelineKey key) {
  auto pipeline_it = dump_pipelines_.find(key);
  if (pipeline_it != dump_pipelines_.end()) {
    return pipeline_it->second;
  }

  // Because of built_shader_.resize(), pointers can't be kept persistently
  // here! Resizing also zeroes the memory.

  built_shader_.clear();

  // RDEF, ISGN, OSGN, SHEX, STAT.
  constexpr uint32_t kBlobCount = 5;

  // Allocate space for the container header and the blob offsets.
  built_shader_.resize(sizeof(dxbc::ContainerHeader) / sizeof(uint32_t) + kBlobCount);
  uint32_t blob_offset_position_dwords = sizeof(dxbc::ContainerHeader) / sizeof(uint32_t);
  uint32_t blob_position_dwords = uint32_t(built_shader_.size());
  constexpr uint32_t kBlobHeaderSizeDwords = sizeof(dxbc::BlobHeader) / sizeof(uint32_t);

  uint32_t name_ptr;

  // ***************************************************************************
  // Resource definition
  // ***************************************************************************

  built_shader_[blob_offset_position_dwords] = uint32_t(blob_position_dwords * sizeof(uint32_t));
  uint32_t rdef_position_dwords = blob_position_dwords + kBlobHeaderSizeDwords;
  // Not needed, as the next operation done is resize, to allocate the space for
  // both the blob header and the resource definition header.
  // built_shader_.resize(rdef_position_dwords);

  // Allocate space for the RDEF header.
  built_shader_.resize(rdef_position_dwords + sizeof(dxbc::RdefHeader) / sizeof(uint32_t));
  // Generator name.
  dxbc::AppendAlignedString(built_shader_, "Xenia");

  // Constant types - uint (aka "dword" when it's scalar) only.
  // Names.
  name_ptr = uint32_t((built_shader_.size() - rdef_position_dwords) * sizeof(uint32_t));
  uint32_t rdef_dword_name_ptr = name_ptr;
  name_ptr += dxbc::AppendAlignedString(built_shader_, "dword");
  // Types.
  uint32_t rdef_type_uint_position_dwords = uint32_t(built_shader_.size());
  uint32_t rdef_type_uint_ptr =
      uint32_t((rdef_type_uint_position_dwords - rdef_position_dwords) * sizeof(uint32_t));
  built_shader_.resize(rdef_type_uint_position_dwords + sizeof(dxbc::RdefType) / sizeof(uint32_t));
  {
    auto& rdef_type_uint =
        *reinterpret_cast<dxbc::RdefType*>(built_shader_.data() + rdef_type_uint_position_dwords);
    rdef_type_uint.variable_class = dxbc::RdefVariableClass::kScalar;
    rdef_type_uint.variable_type = dxbc::RdefVariableType::kUInt;
    rdef_type_uint.row_count = 1;
    rdef_type_uint.column_count = 1;
    rdef_type_uint.name_ptr = rdef_dword_name_ptr;
  }

  // Constants:
  // - uint xe_edram_dump_offsets
  // - uint xe_edram_dump_pitches
  enum Constant : uint32_t {
    kConstantOffsets,
    kConstantPitches,
    kConstantCount,
  };
  // Names.
  name_ptr = uint32_t((built_shader_.size() - rdef_position_dwords) * sizeof(uint32_t));
  uint32_t rdef_xe_edram_dump_offsets_name_ptr = name_ptr;
  name_ptr += dxbc::AppendAlignedString(built_shader_, "xe_edram_dump_offsets");
  uint32_t rdef_xe_edram_dump_pitches_name_ptr = name_ptr;
  name_ptr += dxbc::AppendAlignedString(built_shader_, "xe_edram_dump_pitches");
  // Constants.
  uint32_t rdef_constants_position_dwords = uint32_t(built_shader_.size());
  uint32_t rdef_constants_ptr =
      uint32_t((rdef_constants_position_dwords - rdef_position_dwords) * sizeof(uint32_t));
  built_shader_.resize(rdef_constants_position_dwords +
                       sizeof(dxbc::RdefVariable) / sizeof(uint32_t) * kConstantCount);
  {
    auto rdef_constants = reinterpret_cast<dxbc::RdefVariable*>(built_shader_.data() +
                                                                rdef_constants_position_dwords);
    // uint xe_edram_dump_offsets
    dxbc::RdefVariable& rdef_constant_offsets = rdef_constants[kConstantOffsets];
    rdef_constant_offsets.name_ptr = rdef_xe_edram_dump_offsets_name_ptr;
    rdef_constant_offsets.size_bytes = sizeof(uint32_t);
    rdef_constant_offsets.flags = dxbc::kRdefVariableFlagUsed;
    rdef_constant_offsets.type_ptr = rdef_type_uint_ptr;
    rdef_constant_offsets.start_texture = UINT32_MAX;
    rdef_constant_offsets.start_sampler = UINT32_MAX;
    // uint xe_edram_dump_pitches
    dxbc::RdefVariable& rdef_constant_pitches = rdef_constants[kConstantPitches];
    rdef_constant_pitches.name_ptr = rdef_xe_edram_dump_pitches_name_ptr;
    rdef_constant_pitches.size_bytes = sizeof(uint32_t);
    rdef_constant_pitches.flags = dxbc::kRdefVariableFlagUsed;
    rdef_constant_pitches.type_ptr = rdef_type_uint_ptr;
    rdef_constant_pitches.start_texture = UINT32_MAX;
    rdef_constant_pitches.start_sampler = UINT32_MAX;
  }

  // Constant buffers:
  // - xe_edram_dump_offsets : b0 { uint xe_edram_dump_offsets; }
  // - xe_edram_dump_pitches : b1 { uint xe_edram_dump_pitches; }
  // Reusing the constant names for constant buffers.
  uint32_t rdef_cbuffer_position_dwords = uint32_t(built_shader_.size());
  built_shader_.resize(rdef_cbuffer_position_dwords +
                       sizeof(dxbc::RdefCbuffer) / sizeof(uint32_t) * kDumpCbufferCount);
  {
    auto rdef_cbuffers =
        reinterpret_cast<dxbc::RdefCbuffer*>(built_shader_.data() + rdef_cbuffer_position_dwords);
    // xe_edram_dump_offsets
    dxbc::RdefCbuffer& rdef_cbuffer_offsets = rdef_cbuffers[kDumpCbufferOffsets];
    rdef_cbuffer_offsets.name_ptr = rdef_xe_edram_dump_offsets_name_ptr;
    rdef_cbuffer_offsets.variable_count = 1;
    rdef_cbuffer_offsets.variables_ptr =
        uint32_t(rdef_constants_ptr + sizeof(dxbc::RdefVariable) * kConstantOffsets);
    rdef_cbuffer_offsets.size_vector_aligned_bytes = sizeof(uint32_t) * 4;
    // xe_edram_dump_pitches
    dxbc::RdefCbuffer& rdef_cbuffer_pitches = rdef_cbuffers[kDumpCbufferPitches];
    rdef_cbuffer_pitches.name_ptr = rdef_xe_edram_dump_pitches_name_ptr;
    rdef_cbuffer_pitches.variable_count = 1;
    rdef_cbuffer_pitches.variables_ptr =
        uint32_t(rdef_constants_ptr + sizeof(dxbc::RdefVariable) * kConstantPitches);
    rdef_cbuffer_pitches.size_vector_aligned_bytes = sizeof(uint32_t) * 4;
  }

  // Bindings.
  // - Texture2D/Texture2DMS<float4/uint4> xe_edram_dump_source : t0
  // - Optionally, Texture2D/Texture2DMS<uint2> xe_edram_dump_stencil : t1
  // - RWBuffer<uint/uint2> xe_edram : u0
  // - Constant buffers
  uint32_t rdef_binding_count = 1 + key.is_depth + 1 + kDumpCbufferCount;
  // Names.
  name_ptr = uint32_t((built_shader_.size() - rdef_position_dwords) * sizeof(uint32_t));
  uint32_t rdef_xe_edram_dump_source_name_ptr = name_ptr;
  name_ptr += dxbc::AppendAlignedString(built_shader_, "xe_edram_dump_source");
  uint32_t rdef_xe_edram_dump_stencil_name_ptr = name_ptr;
  if (key.is_depth) {
    name_ptr += dxbc::AppendAlignedString(built_shader_, "xe_edram_dump_stencil");
  }
  uint32_t rdef_xe_edram_name_ptr = name_ptr;
  name_ptr += dxbc::AppendAlignedString(built_shader_, "xe_edram");
  // Bindings.
  uint32_t rdef_binding_position_dwords = uint32_t(built_shader_.size());
  built_shader_.resize(rdef_binding_position_dwords +
                       sizeof(dxbc::RdefInputBind) / sizeof(uint32_t) * rdef_binding_count);
  bool source_is_uint;
  if (key.is_depth) {
    source_is_uint = false;
  } else {
    GetColorOwnershipTransferDXGIFormat(key.GetColorFormat(), &source_is_uint);
  }
  dxbc::ResourceReturnType source_return_type =
      source_is_uint ? dxbc::ResourceReturnType::kUInt : dxbc::ResourceReturnType::kFloat;
  uint32_t source_component_count =
      key.is_depth ? 1 : xenos::GetColorRenderTargetFormatComponentCount(key.GetColorFormat());
  bool format_is_64bpp =
      !key.is_depth && xenos::IsColorRenderTargetFormat64bpp(key.GetColorFormat());
  {
    auto rdef_bindings =
        reinterpret_cast<dxbc::RdefInputBind*>(built_shader_.data() + rdef_binding_position_dwords);
    uint32_t rdef_binding_index = 0;
    // xe_edram_dump_source
    dxbc::RdefInputBind& rdef_binding_source = rdef_bindings[rdef_binding_index++];
    rdef_binding_source.name_ptr = rdef_xe_edram_dump_source_name_ptr;
    rdef_binding_source.type = dxbc::RdefInputType::kTexture;
    rdef_binding_source.return_type = source_return_type;
    if (key.msaa_samples != xenos::MsaaSamples::k1X) {
      rdef_binding_source.dimension = dxbc::RdefDimension::kSRVTexture2DMS;
      // Sample count is dynamic on Shader Model 5.
    } else {
      rdef_binding_source.dimension = dxbc::RdefDimension::kSRVTexture2D;
      rdef_binding_source.sample_count = UINT32_MAX;
    }
    rdef_binding_source.bind_count = 1;
    rdef_binding_source.flags = (source_component_count - 1)
                                << dxbc::kRdefInputFlagsComponentsShift;
    // xe_edram_dump_stencil
    if (key.is_depth) {
      dxbc::RdefInputBind& rdef_binding_stencil = rdef_bindings[rdef_binding_index++];
      rdef_binding_stencil.name_ptr = rdef_xe_edram_dump_stencil_name_ptr;
      rdef_binding_stencil.type = dxbc::RdefInputType::kTexture;
      rdef_binding_stencil.return_type = dxbc::ResourceReturnType::kUInt;
      rdef_binding_stencil.dimension = rdef_binding_source.dimension;
      rdef_binding_stencil.sample_count = rdef_binding_source.sample_count;
      rdef_binding_stencil.bind_point = 1;
      rdef_binding_stencil.bind_count = 1;
      rdef_binding_stencil.flags = dxbc::kRdefInputFlags2Component;
      rdef_binding_stencil.id = 1;
    }
    // xe_edram
    dxbc::RdefInputBind& rdef_binding_edram = rdef_bindings[rdef_binding_index++];
    rdef_binding_edram.name_ptr = rdef_xe_edram_name_ptr;
    rdef_binding_edram.type = dxbc::RdefInputType::kUAVRWTyped;
    rdef_binding_edram.return_type = dxbc::ResourceReturnType::kUInt;
    rdef_binding_edram.dimension = dxbc::RdefDimension::kUAVBuffer;
    rdef_binding_edram.sample_count = UINT32_MAX;
    rdef_binding_edram.bind_count = 1;
    rdef_binding_edram.flags = format_is_64bpp ? dxbc::kRdefInputFlags2Component : 0;
    // xe_edram_dump_offsets
    dxbc::RdefInputBind& rdef_binding_offsets = rdef_bindings[rdef_binding_index++];
    rdef_binding_offsets.name_ptr = rdef_xe_edram_dump_offsets_name_ptr;
    rdef_binding_offsets.type = dxbc::RdefInputType::kCbuffer;
    rdef_binding_offsets.bind_point = kDumpCbufferOffsets;
    rdef_binding_offsets.bind_count = 1;
    rdef_binding_offsets.flags = dxbc::kRdefInputFlagUserPacked;
    rdef_binding_offsets.id = kDumpCbufferOffsets;
    // xe_edram_dump_pitches
    dxbc::RdefInputBind& rdef_binding_pitches = rdef_bindings[rdef_binding_index++];
    rdef_binding_pitches.name_ptr = rdef_xe_edram_dump_pitches_name_ptr;
    rdef_binding_pitches.type = dxbc::RdefInputType::kCbuffer;
    rdef_binding_pitches.bind_point = kDumpCbufferPitches;
    rdef_binding_pitches.bind_count = 1;
    rdef_binding_pitches.flags = dxbc::kRdefInputFlagUserPacked;
    rdef_binding_pitches.id = kDumpCbufferPitches;
  }

  // Header.
  {
    auto& rdef_header =
        *reinterpret_cast<dxbc::RdefHeader*>(built_shader_.data() + rdef_position_dwords);
    rdef_header.cbuffer_count = kDumpCbufferCount;
    rdef_header.cbuffers_ptr =
        uint32_t((rdef_cbuffer_position_dwords - rdef_position_dwords) * sizeof(uint32_t));
    rdef_header.input_bind_count = rdef_binding_count;
    rdef_header.input_binds_ptr =
        uint32_t((rdef_binding_position_dwords - rdef_position_dwords) * sizeof(uint32_t));
    rdef_header.shader_model = dxbc::RdefShaderModel::kComputeShader5_1;
    rdef_header.compile_flags =
        dxbc::kCompileFlagNoPreshader | dxbc::kCompileFlagPreferFlowControl |
        dxbc::kCompileFlagIeeeStrictness | dxbc::kCompileFlagAllResourcesBound;
    // Generator name is right after the header.
    rdef_header.generator_name_ptr = sizeof(dxbc::RdefHeader);
    rdef_header.fourcc = dxbc::RdefHeader::FourCC::k5_1;
    rdef_header.InitializeSizes();
  }

  {
    auto& blob_header =
        *reinterpret_cast<dxbc::BlobHeader*>(built_shader_.data() + blob_position_dwords);
    blob_header.fourcc = dxbc::BlobHeader::FourCC::kResourceDefinition;
    blob_position_dwords = uint32_t(built_shader_.size());
    blob_header.size_bytes = (blob_position_dwords - kBlobHeaderSizeDwords) * sizeof(uint32_t) -
                             built_shader_[blob_offset_position_dwords++];
  }

  // ***************************************************************************
  // Input and output signatures (empty)
  // ***************************************************************************

  for (uint32_t i = 0; i < 2; ++i) {
    built_shader_[blob_offset_position_dwords] = uint32_t(blob_position_dwords * sizeof(uint32_t));
    uint32_t signature_position_dwords = blob_position_dwords + kBlobHeaderSizeDwords;
    built_shader_.resize(signature_position_dwords + sizeof(dxbc::Signature) / sizeof(uint32_t));
    {
      auto& signature =
          *reinterpret_cast<dxbc::Signature*>(built_shader_.data() + signature_position_dwords);
      // Empty - just set parameter pointer to the end.
      signature.parameter_info_ptr = sizeof(dxbc::Signature);
    }
    {
      auto& blob_header =
          *reinterpret_cast<dxbc::BlobHeader*>(built_shader_.data() + blob_position_dwords);
      blob_header.fourcc = i ? dxbc::BlobHeader::FourCC::kOutputSignature
                             : dxbc::BlobHeader::FourCC::kInputSignature;
      blob_position_dwords = uint32_t(built_shader_.size());
      blob_header.size_bytes = (blob_position_dwords - kBlobHeaderSizeDwords) * sizeof(uint32_t) -
                               built_shader_[blob_offset_position_dwords++];
    }
  }

  // ***************************************************************************
  // Shader program
  // ***************************************************************************

  built_shader_[blob_offset_position_dwords] = uint32_t(blob_position_dwords * sizeof(uint32_t));
  uint32_t shex_position_dwords = blob_position_dwords + kBlobHeaderSizeDwords;
  built_shader_.resize(shex_position_dwords);

  built_shader_.push_back(dxbc::VersionToken(dxbc::ProgramType::kComputeShader, 5, 1));
  // Reserve space for the length token.
  built_shader_.push_back(0);

  dxbc::Statistics stat;
  std::memset(&stat, 0, sizeof(dxbc::Statistics));
  dxbc::Assembler a(built_shader_, stat);

  a.OpDclGlobalFlags(dxbc::kGlobalFlagAllResourcesBound);
  a.OpDclConstantBuffer(
      dxbc::Src::CB(dxbc::Src::Dcl, kDumpCbufferOffsets, kDumpCbufferOffsets, kDumpCbufferOffsets),
      1);
  a.OpDclConstantBuffer(
      dxbc::Src::CB(dxbc::Src::Dcl, kDumpCbufferPitches, kDumpCbufferPitches, kDumpCbufferPitches),
      1);
  // Source texture.
  dxbc::ResourceDimension source_dimension = key.msaa_samples != xenos::MsaaSamples::k1X
                                                 ? dxbc::ResourceDimension::kTexture2DMS
                                                 : dxbc::ResourceDimension::kTexture2D;
  a.OpDclResource(
      source_dimension,
      dxbc::ResourceReturnTypeX4Token(source_is_uint ? dxbc::ResourceReturnType::kUInt
                                                     : dxbc::ResourceReturnType::kFloat),
      dxbc::Src::T(dxbc::Src::Dcl, 0, 0, 0));
  // Source stencil texture.
  if (key.is_depth) {
    a.OpDclResource(source_dimension,
                    dxbc::ResourceReturnTypeX4Token(dxbc::ResourceReturnType::kUInt),
                    dxbc::Src::T(dxbc::Src::Dcl, 1, 1, 1));
  }
  // EDRAM buffer.
  a.OpDclUnorderedAccessViewTyped(dxbc::ResourceDimension::kBuffer, 0,
                                  dxbc::ResourceReturnTypeX4Token(dxbc::ResourceReturnType::kUInt),
                                  dxbc::Src::U(dxbc::Src::Dcl, 0, 0, 0));
  a.OpDclInput(dxbc::Dest::VThreadID(0b0011));
  bool depth_center =
      key.depth_center && key.is_depth && key.msaa_samples == xenos::MsaaSamples::k2X;
  // r0 - addressing before the load, then addressing and conversion scratch
  // r1 - addressing scratch before the load, then data
  // r2, r3 - pixel-centre depth when depth_center
  stat.temp_register_count = depth_center ? 4 : 2;
  a.OpDclTemps(stat.temp_register_count);
  // There's no strict dependency on the group size here, for simplicity of
  // calculations especially with resolution scaling, dividing manually (as the
  // group size is not unlimited). The only restriction is that an integer
  // multiple of it must be 80x16 samples (and no larger than that) for 32bpp,
  // or 40x16 samples for 64bpp (because only a half of the pair of tiles may
  // need to be dumped). The group size limit in Direct3D 11 is 1024, and 40x16
  // fits in it, while 80x16 doesn't.
  a.OpDclThreadGroup(40, 16, 1);

  uint32_t draw_resolution_scale_x =
      key.native_layout ? 1 : this->draw_resolution_scale_x();
  uint32_t draw_resolution_scale_y =
      key.native_layout ? 1 : this->draw_resolution_scale_y();

  // For now, as the exact addressing in 64bpp render targets relatively to
  // 32bpp is unknown, treating 64bpp tiles as storing 40x16 samples rather than
  // 80x16 for simplicity of addressing into the texture.

  uint32_t tile_width =
      (xenos::kEdramTileWidthSamples * draw_resolution_scale_x) >> uint32_t(format_is_64bpp);
  uint32_t tile_height = xenos::kEdramTileHeightSamples * draw_resolution_scale_y;

  // Get the parts of the address - tile row index within the dispatch to r0.zw,
  // sample Y within the tile to r0.xy.
  // r0.x = X sample position within the tile
  // r0.y = Y sample position within the tile
  // r0.z = X tile position
  // r0.w = Y tile position
  a.OpUDiv(dxbc::Dest::R(0, 0b1100), dxbc::Dest::R(0, 0b0011), dxbc::Src::VThreadID(0b01000100),
           dxbc::Src::LU(tile_width, tile_height, tile_width, tile_height));

  // Extract the dump rectangle tile row pitch to r1.x.
  // r0.x = X sample position within the tile
  // r0.y = Y sample position within the tile
  // r0.z = X tile position
  // r0.w = Y tile position
  // r1.x = dump rectangle pitch in tiles
  a.OpUBFE(dxbc::Dest::R(1, 0b0001), dxbc::Src::LU(xenos::kEdramPitchTilesBits), dxbc::Src::LU(0),
           dxbc::Src::CB(kDumpCbufferPitches, kDumpCbufferPitches, 0, dxbc::Src::kXXXX));
  // Get the tile index in the EDRAM relative to the dump rectangle base tile to
  // r0.w.
  // r0.x = X sample position within the tile
  // r0.y = Y sample position within the tile
  // r0.z = free
  // r0.w = tile index relative to the dump rectangle base
  // r1.x = free
  a.OpUMAd(dxbc::Dest::R(0, 0b1000), dxbc::Src::R(0, dxbc::Src::kWWWW),
           dxbc::Src::R(1, dxbc::Src::kXXXX), dxbc::Src::R(0, dxbc::Src::kZZZZ));

  // Extract the index of the first tile (taking EDRAM addressing wrapping into
  // account) of the dispatch in the EDRAM to r0.z.
  // r0.x = X sample position within the tile
  // r0.y = Y sample position within the tile
  // r0.z = first EDRAM tile index in the dispatch
  // r0.w = tile index relative to the dump rectangle base
  a.OpUBFE(dxbc::Dest::R(0, 0b0100), dxbc::Src::LU(xenos::kEdramBaseTilesBits + 1),
           dxbc::Src::LU(0),
           dxbc::Src::CB(kDumpCbufferOffsets, kDumpCbufferOffsets, 0, dxbc::Src::kXXXX));
  // Add the base tile in the dispatch to the dispatch-local tile index to r0.w,
  // not wrapping yet so in case of a wraparound, the address relative to the
  // base in the texture after subtraction of the base won't be negative.
  // r0.x = X sample position within the tile
  // r0.y = Y sample position within the tile
  // r0.z = free
  // r0.w = non-wrapped tile index in the EDRAM
  a.OpIAdd(dxbc::Dest::R(0, 0b1000), dxbc::Src::R(0, dxbc::Src::kWWWW),
           dxbc::Src::R(0, dxbc::Src::kZZZZ));
  // Wrap the address of the tile in the EDRAM to r0.z.
  // r0.x = X sample position within the tile
  // r0.y = Y sample position within the tile
  // r0.z = wrapped tile index in the EDRAM
  // r0.w = non-wrapped tile index in the EDRAM
  a.OpAnd(dxbc::Dest::R(0, 0b0100), dxbc::Src::R(0, dxbc::Src::kWWWW),
          dxbc::Src::LU(xenos::kEdramTileCount - 1));
  // Convert the tile index to samples and add the X sample index to it to r0.z.
  // r0.x = X sample position within the tile
  // r0.y = Y sample position within the tile
  // r0.z = tile sample offset in the EDRAM plus X sample offset
  // r0.w = non-wrapped tile index in the EDRAM
  a.OpUMAd(dxbc::Dest::R(0, 0b0100), dxbc::Src::R(0, dxbc::Src::kZZZZ),
           dxbc::Src::LU(draw_resolution_scale_x * draw_resolution_scale_y *
                         (xenos::kEdramTileWidthSamples >> uint32_t(format_is_64bpp)) *
                         xenos::kEdramTileHeightSamples),
           dxbc::Src::R(0, dxbc::Src::kXXXX));
  // Add the contribution of the Y sample position within the tile to the sample
  // address in the EDRAM to r0.z.
  // r0.x = X sample position within the tile
  // r0.y = Y sample position within the tile
  // r0.z = sample offset in the EDRAM without the depth column swapping
  // r0.w = non-wrapped tile index in the EDRAM
  a.OpUMAd(dxbc::Dest::R(0, 0b0100), dxbc::Src::R(0, dxbc::Src::kYYYY), dxbc::Src::LU(tile_width),
           dxbc::Src::R(0, dxbc::Src::kZZZZ));
  if (key.is_depth) {
    uint32_t tile_width_half = tile_width >> 1;
    // Get which 40-sample half within the tile is being processed to r1.x.
    // r0.x = X sample position within the tile
    // r0.y = Y sample position within the tile
    // r0.z = sample offset in the EDRAM without the depth column swapping
    // r0.w = non-wrapped tile index in the EDRAM
    // r1.x = 0xFFFFFFFF if in the right 40-sample half, 0 otherwise
    a.OpUGE(dxbc::Dest::R(1, 0b0001), dxbc::Src::R(0, dxbc::Src::kXXXX),
            dxbc::Src::LU(tile_width_half));
    // Get the offset needed to swap 40-sample halves for depth.
    // r0.x = X sample position within the tile
    // r0.y = Y sample position within the tile
    // r0.z = sample offset in the EDRAM without the depth column swapping
    // r0.w = non-wrapped tile index in the EDRAM
    // r1.x = depth half-tile flipping offset
    a.OpMovC(dxbc::Dest::R(1, 0b0001), dxbc::Src::R(1, dxbc::Src::kXXXX),
             dxbc::Src::LI(-int32_t(tile_width_half)), dxbc::Src::LI(int32_t(tile_width_half)));
    // Swap 40-sample columns in the depth buffer in the destination address in
    // r0.w to get the final address of the sample in EDRAM.
    // r0.x = X sample position within the tile
    // r0.y = Y sample position within the tile
    // r0.z = sample offset in the EDRAM
    // r0.w = non-wrapped tile index in the EDRAM
    // r1.x = free
    a.OpIAdd(dxbc::Dest::R(0, 0b0100), dxbc::Src::R(0, dxbc::Src::kZZZZ),
             dxbc::Src::R(1, dxbc::Src::kXXXX));
  }

  // Extract the source texture base tile index to r1.x.
  // r0.x = X sample position within the tile
  // r0.y = Y sample position within the tile
  // r0.z = sample offset in the EDRAM
  // r0.w = non-wrapped tile index in the EDRAM
  // r1.x = source texture base tile index
  a.OpUBFE(dxbc::Dest::R(1, 0b0001), dxbc::Src::LU(xenos::kEdramBaseTilesBits),
           dxbc::Src::LU(xenos::kEdramBaseTilesBits + 1),
           dxbc::Src::CB(kDumpCbufferOffsets, kDumpCbufferOffsets, 0, dxbc::Src::kXXXX));
  // Get the linear tile index within the source texture to r0.w.
  // r0.x = X sample position within the tile
  // r0.y = Y sample position within the tile
  // r0.z = sample offset in the EDRAM
  // r0.w = linear tile index in the source texture
  // r1.x = free
  a.OpIAdd(dxbc::Dest::R(0, 0b1000), dxbc::Src::R(0, dxbc::Src::kWWWW),
           -dxbc::Src::R(1, dxbc::Src::kXXXX));
  // Get the source texture pitch in tiles to r1.x.
  // r0.x = X sample position within the tile
  // r0.y = Y sample position within the tile
  // r0.z = sample offset in the EDRAM
  // r0.w = linear tile index in the source texture
  // r1.x = source texture pitch in tiles
  a.OpUBFE(dxbc::Dest::R(1, 0b0001), dxbc::Src::LU(xenos::kEdramPitchTilesBits),
           dxbc::Src::LU(xenos::kEdramPitchTilesBits),
           dxbc::Src::CB(kDumpCbufferPitches, kDumpCbufferPitches, 0, dxbc::Src::kXXXX));
  // Split the linear tile index in the source texture into X and Y in tiles.
  // r0.x = X sample position within the tile
  // r0.y = Y sample position within the tile
  // r0.z = sample offset in the EDRAM
  // r0.w = X tile index within the tile row in the source texture
  // r1.x = Y tile row index within the source texture
  a.OpUDiv(dxbc::Dest::R(1, 0b0001), dxbc::Dest::R(0, 0b1000), dxbc::Src::R(0, dxbc::Src::kWWWW),
           dxbc::Src::R(1, dxbc::Src::kXXXX));
  // Add the source texture tile X offset to the source texture sample X
  // coordinate.
  // r0.x = X sample position within the source texture
  // r0.y = Y sample position within the tile
  // r0.z = sample offset in the EDRAM
  // r0.w = free
  // r1.x = Y tile row index within the source texture
  a.OpUMAd(dxbc::Dest::R(0, 0b0001), dxbc::Src::R(0, dxbc::Src::kWWWW), dxbc::Src::LU(tile_width),
           dxbc::Src::R(0, dxbc::Src::kXXXX));
  // Add the source texture tile Y offset to the source texture sample Y
  // coordinate.
  // r0.x = X sample position within the source texture
  // r0.y = Y sample position within the source texture
  // r0.z = sample offset in the EDRAM
  // r1.x = free
  a.OpUMAd(dxbc::Dest::R(0, 0b0010), dxbc::Src::R(1, dxbc::Src::kXXXX),
           dxbc::Src::LU(xenos::kEdramTileHeightSamples * draw_resolution_scale_y),
           dxbc::Src::R(0, dxbc::Src::kYYYY));
  // Will be using the source texture coordinates from r0.xy, and for
  // single-sampled source, LOD from r0.w.
  dxbc::Src source_address_src(dxbc::Src::R(0, 0b11000100));
  if (key.msaa_samples >= xenos::MsaaSamples::k2X) {
    // r0.xy holds the canonical sample coordinates within the EDRAM layout.
    // Convert them into the pixel and the sample of the multisampled view
    // using the canonical layout formulas, at guest pixel granularity when the
    // layout is scaled.
    uint32_t layout_scale_x =
        key.native_layout ? 1 : this->draw_resolution_scale_x();
    uint32_t layout_scale_y =
        key.native_layout ? 1 : this->draw_resolution_scale_y();
    bool layout_scaled = layout_scale_x > 1 || layout_scale_y > 1;
    if (layout_scaled) {
      // r0.xy = guest canonical sample coordinates
      // r1.xy = subpixel within the guest sample
      a.OpUDiv(dxbc::Dest::R(0, 0b0011), dxbc::Dest::R(1, 0b0011),
               dxbc::Src::R(0),
               dxbc::Src::LU(layout_scale_x, layout_scale_y, 1, 1));
    }
    if (key.msaa_samples >= xenos::MsaaSamples::k4X) {
      // The 4x sample index has bit 0 horizontal and bit 1 vertical, same as
      // the canonical layout.
      // r0.w = horizontal sample index = (u >> 1) & 1
      a.OpUBFE(dxbc::Dest::R(0, 0b1000), dxbc::Src::LU(1), dxbc::Src::LU(1),
               dxbc::Src::R(0, dxbc::Src::kXXXX));
      // r1.z = vertical sample index = (v >> 1) & 1
      a.OpUBFE(dxbc::Dest::R(1, 0b0100), dxbc::Src::LU(1), dxbc::Src::LU(1),
               dxbc::Src::R(0, dxbc::Src::kYYYY));
      // r0.w = sample index within the source pixel
      a.OpBFI(dxbc::Dest::R(0, 0b1000), dxbc::Src::LU(1), dxbc::Src::LU(1),
              dxbc::Src::R(1, dxbc::Src::kZZZZ),
              dxbc::Src::R(0, dxbc::Src::kWWWW));
      // Guest pixel per axis = ((c >> 2) << 1) | (c & 1).
      // r1.z = u >> 2
      a.OpUShR(dxbc::Dest::R(1, 0b0100), dxbc::Src::R(0, dxbc::Src::kXXXX),
               dxbc::Src::LU(2));
      // r0.x = X guest pixel position
      a.OpBFI(dxbc::Dest::R(0, 0b0001), dxbc::Src::LU(31), dxbc::Src::LU(1),
              dxbc::Src::R(1, dxbc::Src::kZZZZ),
              dxbc::Src::R(0, dxbc::Src::kXXXX));
      // r1.z = v >> 2
      a.OpUShR(dxbc::Dest::R(1, 0b0100), dxbc::Src::R(0, dxbc::Src::kYYYY),
               dxbc::Src::LU(2));
      // r0.y = Y guest pixel position
      a.OpBFI(dxbc::Dest::R(0, 0b0010), dxbc::Src::LU(31), dxbc::Src::LU(1),
              dxbc::Src::R(1, dxbc::Src::kZZZZ),
              dxbc::Src::R(0, dxbc::Src::kYYYY));
    } else {
      // 2x MSAA source texture sample index.
      // Extract the vertical sample index to r0.w.
      // r0.x = X pixel position within the source texture
      // r0.y = Y sample position within the source texture
      // r0.z = sample offset in the EDRAM
      // r0.w = guest vertical sample index = (u >> 1) & 1
      a.OpUBFE(dxbc::Dest::R(0, 0b1000), dxbc::Src::LU(1), dxbc::Src::LU(1),
               dxbc::Src::R(0, dxbc::Src::kXXXX));
      // Guest pixel X = (u & ~2) | (v & 2).
      // r1.z = v & 2
      a.OpAnd(dxbc::Dest::R(1, 0b0100), dxbc::Src::R(0, dxbc::Src::kYYYY),
              dxbc::Src::LU(2));
      // r0.x = (u & ~2)
      a.OpAnd(dxbc::Dest::R(0, 0b0001), dxbc::Src::R(0, dxbc::Src::kXXXX),
              dxbc::Src::LU(~uint32_t(2)));
      // r0.x = X guest pixel position
      a.OpOr(dxbc::Dest::R(0, 0b0001), dxbc::Src::R(0, dxbc::Src::kXXXX),
             dxbc::Src::R(1, dxbc::Src::kZZZZ));
      // Guest pixel Y = ((v & ~3) >> 1) | (v & 1).
      // r1.z = v & 1
      a.OpAnd(dxbc::Dest::R(1, 0b0100), dxbc::Src::R(0, dxbc::Src::kYYYY),
              dxbc::Src::LU(1));
      // r0.y = v & ~3
      a.OpAnd(dxbc::Dest::R(0, 0b0010), dxbc::Src::R(0, dxbc::Src::kYYYY),
              dxbc::Src::LU(~uint32_t(3)));
      // r0.y = (v & ~3) >> 1
      a.OpUShR(dxbc::Dest::R(0, 0b0010), dxbc::Src::R(0, dxbc::Src::kYYYY),
               dxbc::Src::LU(1));
      // r0.y = Y guest pixel position
      a.OpOr(dxbc::Dest::R(0, 0b0010), dxbc::Src::R(0, dxbc::Src::kYYYY),
             dxbc::Src::R(1, dxbc::Src::kZZZZ));
      // Convert the 2x MSAA sample index from the guest to Direct3D 10.1+.
      // r0.w = sample index within the source pixel
      a.OpMovC(dxbc::Dest::R(0, 0b1000), dxbc::Src::R(0, dxbc::Src::kWWWW),
               dxbc::Src::LU(draw_util::GetD3D10SampleIndexForGuest2xMSAA(
                   1, msaa_2x_supported_)),
               dxbc::Src::LU(draw_util::GetD3D10SampleIndexForGuest2xMSAA(
                   0, msaa_2x_supported_)));
    }
    if (!key.source_scale_native && layout_scaled) {
      // Scaled source in the scaled layout. Restore the subpixel position.
      // r0.xy = XY pixel position within the source texture
      a.OpUMAd(dxbc::Dest::R(0, 0b0011), dxbc::Src::R(0),
               dxbc::Src::LU(layout_scale_x, layout_scale_y, 1, 1),
               dxbc::Src::R(1));
    }
    // With a native source and a scaled layout, the guest pixel position comes
    // directly from the source texture position, and all the scaled sample
    // slots covering one guest sample receive its value.
    // Load the source to r1.
    // r0.x = X pixel position within the source texture if stencil is needed
    // r0.y = Y pixel position within the source texture if stencil is needed
    // r0.z = sample offset in the EDRAM
    // r0.w = sample index within the source pixel if stencil is needed
    // r1 = source texel value
    a.OpLdMS(dxbc::Dest::R(1, (1 << source_component_count) - 1),
             source_address_src, 0b0011, dxbc::Src::T(0, 0),
             dxbc::Src::R(0, dxbc::Src::kWWWW));
    if (key.is_depth) {
      // Load the source stencil to r1.y.
      // r0.x = free
      // r0.y = free
      // r0.z = sample offset in the EDRAM
      // r0.w = free
      // r1.x = source depth value
      // r1.y = source stencil value
      a.OpLdMS(dxbc::Dest::R(1, 0b0010), source_address_src, 0b0011,
               dxbc::Src::T(1, 1), dxbc::Src::R(0, dxbc::Src::kWWWW));
    }
    if (depth_center) {
      // Replace the depth of the sample with the depth at the pixel centre -
      // the mean of the top and the bottom samples, diagonal on the host - if
      // the pixel lies on one surface with its neighbours: the samples may
      // differ by at most what the one-sided depth gradients towards the
      // flatter neighbours allow. They are half of the X and Y gradients apart
      // with native 2x at (-4, -4) and (4, 4) sixteenths, a quarter of X and
      // three quarters of Y with 2x as 4x samples 0 and 3; the limits add a
      // margin for curved meshes. Silhouette pixels keep the depth of their
      // own sample, as the mean of two surfaces is a point on neither.
      // Neighbours outside the texture load zeros and only fail the test.
      dxbc::Src depth_src(dxbc::Src::T(0, 0, dxbc::Src::kXXXX));
      dxbc::Src sample_top_src(
          dxbc::Src::LU(draw_util::GetD3D10SampleIndexForGuest2xMSAA(0, msaa_2x_supported_)));
      dxbc::Src sample_bottom_src(
          dxbc::Src::LU(draw_util::GetD3D10SampleIndexForGuest2xMSAA(1, msaa_2x_supported_)));
      // r0.xy = pixel position
      // r0.z = sample offset in the EDRAM
      // r1.x = source depth value
      // r1.y = source stencil value
      // r2.x = top sample depth
      // r2.y = bottom sample depth
      a.OpLdMS(dxbc::Dest::R(2, 0b0001), source_address_src, 0b0011, depth_src, sample_top_src);
      a.OpLdMS(dxbc::Dest::R(2, 0b0010), source_address_src, 0b0011, depth_src,
               sample_bottom_src);
      // r3.xy = left neighbour position
      // r3.zw = right neighbour position
      a.OpIAdd(dxbc::Dest::R(3), dxbc::Src::R(0, 0b01000100), dxbc::Src::LI(-1, 0, 1, 0));
      // r2.z = left neighbour top sample depth
      // r2.w = right neighbour top sample depth
      a.OpLdMS(dxbc::Dest::R(2, 0b0100), dxbc::Src::R(3, 0b01000100), 0b0011, depth_src,
               sample_top_src);
      a.OpLdMS(dxbc::Dest::R(2, 0b1000), dxbc::Src::R(3, 0b11101110), 0b0011, depth_src,
               sample_top_src);
      // r3.x = smaller one-sided X gradient
      a.OpAdd(dxbc::Dest::R(3, 0b0001), dxbc::Src::R(2, dxbc::Src::kXXXX),
              -dxbc::Src::R(2, dxbc::Src::kZZZZ));
      a.OpAdd(dxbc::Dest::R(3, 0b0010), dxbc::Src::R(2, dxbc::Src::kWWWW),
              -dxbc::Src::R(2, dxbc::Src::kXXXX));
      a.OpMin(dxbc::Dest::R(3, 0b0001), dxbc::Src::R(3, dxbc::Src::kXXXX).Abs(),
              dxbc::Src::R(3, dxbc::Src::kYYYY).Abs());
      // r3.zw = upper neighbour position
      a.OpIAdd(dxbc::Dest::R(3, 0b1100), dxbc::Src::R(0, 0b01000000), dxbc::Src::LI(0, 0, 0, -1));
      // r2.z = upper neighbour top sample depth
      a.OpLdMS(dxbc::Dest::R(2, 0b0100), dxbc::Src::R(3, 0b11101110), 0b0011, depth_src,
               sample_top_src);
      // r3.zw = lower neighbour position
      a.OpIAdd(dxbc::Dest::R(3, 0b1100), dxbc::Src::R(0, 0b01000000), dxbc::Src::LI(0, 0, 0, 1));
      // r2.w = lower neighbour top sample depth
      a.OpLdMS(dxbc::Dest::R(2, 0b1000), dxbc::Src::R(3, 0b11101110), 0b0011, depth_src,
               sample_top_src);
      // r3.y = smaller one-sided Y gradient
      a.OpAdd(dxbc::Dest::R(3, 0b0100), dxbc::Src::R(2, dxbc::Src::kXXXX),
              -dxbc::Src::R(2, dxbc::Src::kZZZZ));
      a.OpAdd(dxbc::Dest::R(3, 0b1000), dxbc::Src::R(2, dxbc::Src::kWWWW),
              -dxbc::Src::R(2, dxbc::Src::kXXXX));
      a.OpMin(dxbc::Dest::R(3, 0b0010), dxbc::Src::R(3, dxbc::Src::kZZZZ).Abs(),
              dxbc::Src::R(3, dxbc::Src::kWWWW).Abs());
      // r3.x = the largest difference between the samples on one surface
      a.OpAdd(dxbc::Dest::R(3, 0b0001), dxbc::Src::R(3, dxbc::Src::kXXXX),
              dxbc::Src::R(3, dxbc::Src::kYYYY));
      a.OpMul(dxbc::Dest::R(3, 0b0001), dxbc::Src::R(3, dxbc::Src::kXXXX),
              dxbc::Src::LF(msaa_2x_supported_ ? 0.625f : 0.875f));
      // r3.y = difference between the samples
      a.OpAdd(dxbc::Dest::R(3, 0b0010), dxbc::Src::R(2, dxbc::Src::kYYYY),
              -dxbc::Src::R(2, dxbc::Src::kXXXX));
      // r3.x = whether the pixel lies on one surface
      a.OpGE(dxbc::Dest::R(3, 0b0001), dxbc::Src::R(3, dxbc::Src::kXXXX),
             dxbc::Src::R(3, dxbc::Src::kYYYY).Abs());
      // r2.z = depth at the pixel centre
      a.OpAdd(dxbc::Dest::R(2, 0b0100), dxbc::Src::R(2, dxbc::Src::kXXXX),
              dxbc::Src::R(2, dxbc::Src::kYYYY));
      a.OpMul(dxbc::Dest::R(2, 0b0100), dxbc::Src::R(2, dxbc::Src::kZZZZ), dxbc::Src::LF(0.5f));
      // r1.x = depth to store
      a.OpMovC(dxbc::Dest::R(1, 0b0001), dxbc::Src::R(3, dxbc::Src::kXXXX),
               dxbc::Src::R(2, dxbc::Src::kZZZZ), dxbc::Src::R(1, dxbc::Src::kXXXX));
    }
  } else {
    if (key.source_scale_native && !key.native_layout &&
        IsDrawResolutionScaled()) {
      a.OpUDiv(dxbc::Dest::R(0, 0b0011), dxbc::Dest::Null(),
               dxbc::Src::R(0),
               dxbc::Src::LU(this->draw_resolution_scale_x(),
                             this->draw_resolution_scale_y(), 1, 1));
    }
    // Write the LOD index (0) to the register with texture coordinates for
    // loading from the single-sampled source texture.
    // r0.x = X pixel position within the source texture
    // r0.y = Y pixel position within the source texture
    // r0.z = sample offset in the EDRAM
    // r0.w = LOD for the texture load (zero)
    a.OpMov(dxbc::Dest::R(0, 0b1000), dxbc::Src::LF(0.0f));
    // Load the source to r1.
    // r0.x = X pixel position within the source texture if stencil is needed
    // r0.y = Y pixel position within the source texture if stencil is needed
    // r0.z = sample offset in the EDRAM
    // r0.w = LOD for the texture load (zero)
    // r1 = source texel value
    a.OpLd(dxbc::Dest::R(1, (1 << source_component_count) - 1), source_address_src, 0b1011,
           dxbc::Src::T(0, 0));
    if (key.is_depth) {
      // Load the source stencil to r1.y.
      // r0.x = free
      // r0.y = free
      // r0.z = sample offset in the EDRAM
      // r0.w = free
      // r1.x = source depth value
      // r1.y = source stencil value
      a.OpLd(dxbc::Dest::R(1, 0b0010), source_address_src, 0b1011, dxbc::Src::T(1, 1));
    }
  }

  // Pack in the needed format, writing the result to r1.x for 32bpp or r1.xy
  // for 64bpp.
  // r0.xyw are usable as temporary storage.
  if (key.is_depth) {
    switch (key.GetDepthFormat()) {
      case xenos::DepthRenderTargetFormat::kD24S8:
        // Round to the nearest even integer. This seems to be the correct
        // conversion, adding +0.5 and rounding towards zero results in red
        // instead of black in the 4D5307E6 clear shader.
        a.OpMul(dxbc::Dest::R(1, 0b0001), dxbc::Src::R(1, dxbc::Src::kXXXX),
                dxbc::Src::LF(float(0xFFFFFF)));
        a.OpRoundNE(dxbc::Dest::R(1, 0b0001), dxbc::Src::R(1, dxbc::Src::kXXXX));
        a.OpFToU(dxbc::Dest::R(1, 0b0001), dxbc::Src::R(1, dxbc::Src::kXXXX));
        break;
      case xenos::DepthRenderTargetFormat::kD24FS8:
        // Convert to [0, 2) float24 from [0, 1) float32, using r0.x as
        // temporary.
        // When converting the depth in pixel shaders, it's always exact,
        // truncating not to insert additional rounding instructions.
        DxbcShaderTranslator::PreClampedDepthTo20e4(
            a, 1, 0, 1, 0, 0, 0, !depth_float24_convert_in_pixel_shader() && depth_float24_round(),
            true);
        break;
    }
    // Combine 24-bit depth and stencil into r1.x.
    a.OpBFI(dxbc::Dest::R(1, 0b0001), dxbc::Src::LU(24), dxbc::Src::LU(8),
            dxbc::Src::R(1, dxbc::Src::kXXXX), dxbc::Src::R(1, dxbc::Src::kYYYY));
  } else {
    switch (key.GetColorFormat()) {
      case xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA:
        // 8_8_8_8_GAMMA is represented by linear stored in
        // R16G16B16A16_UNORM.
        assert_false(source_is_uint);
        for (uint32_t i = 0; i < 3; ++i) {
          DxbcShaderTranslator::PreSaturatedLinearToPWLGamma(a, 1, i, 1, i, 0, 0, 0, 1);
        }
        a.OpMAd(dxbc::Dest::R(1), dxbc::Src::R(1), dxbc::Src::LF(255.0f), dxbc::Src::LF(0.5f));
        a.OpFToU(dxbc::Dest::R(1), dxbc::Src::R(1));
        for (uint32_t i = 1; i < 4; ++i) {
          a.OpBFI(dxbc::Dest::R(1, 0b0001), dxbc::Src::LU(8), dxbc::Src::LU(i * 8),
                  dxbc::Src::R(1).Select(i), dxbc::Src::R(1, dxbc::Src::kXXXX));
        }
        break;
      case xenos::ColorRenderTargetFormat::k_8_8_8_8:
        if (!source_is_uint) {
          a.OpMAd(dxbc::Dest::R(1), dxbc::Src::R(1), dxbc::Src::LF(255.0f), dxbc::Src::LF(0.5f));
          a.OpFToU(dxbc::Dest::R(1), dxbc::Src::R(1));
        }
        for (uint32_t i = 1; i < 4; ++i) {
          a.OpBFI(dxbc::Dest::R(1, 0b0001), dxbc::Src::LU(8), dxbc::Src::LU(i * 8),
                  dxbc::Src::R(1).Select(i), dxbc::Src::R(1, dxbc::Src::kXXXX));
        }
        break;
      case xenos::ColorRenderTargetFormat::k_2_10_10_10:
      case xenos::ColorRenderTargetFormat::k_2_10_10_10_AS_10_10_10_10:
        if (!source_is_uint) {
          a.OpMAd(dxbc::Dest::R(1), dxbc::Src::R(1), dxbc::Src::LF(1023.0f, 1023.0f, 1023.0f, 3.0f),
                  dxbc::Src::LF(0.5f));
          a.OpFToU(dxbc::Dest::R(1), dxbc::Src::R(1));
        }
        for (uint32_t i = 1; i < 4; ++i) {
          a.OpBFI(dxbc::Dest::R(1, 0b0001), dxbc::Src::LU(i == 3 ? 2 : 10), dxbc::Src::LU(i * 10),
                  dxbc::Src::R(1).Select(i), dxbc::Src::R(1, dxbc::Src::kXXXX));
        }
        break;
      case xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT:
      case xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT_AS_16_16_16_16:
        // Float16 has a wider range for both color and alpha, also NaNs.
        // Color - clamp and convert.
        // Convert red in r1.x to the result register r1.x - the same, but
        // UnclampedFloat32To7e3 allows that - using r0.x as a temporary.
        DxbcShaderTranslator::UnclampedFloat32To7e3(a, 1, 0, 1, 0, 0, 0);
        for (uint32_t i = 1; i < 3; ++i) {
          // Convert green and blue to a temporary register r0.x using r0.y
          // as an internal temporary, then insert them into the result in
          // r1.x.
          DxbcShaderTranslator::UnclampedFloat32To7e3(a, 0, 0, 1, i, 0, 1);
          a.OpBFI(dxbc::Dest::R(1, 0b0001), dxbc::Src::LU(10), dxbc::Src::LU(i * 10),
                  dxbc::Src::R(0, dxbc::Src::kXXXX), dxbc::Src::R(1, dxbc::Src::kXXXX));
        }
        // Alpha - saturate and convert.
        a.OpMov(dxbc::Dest::R(1, 0b1000), dxbc::Src::R(1, dxbc::Src::kWWWW), true);
        a.OpMAd(dxbc::Dest::R(1, 0b1000), dxbc::Src::R(1, dxbc::Src::kWWWW), dxbc::Src::LF(3.0f),
                dxbc::Src::LF(0.5f));
        a.OpFToU(dxbc::Dest::R(1, 0b1000), dxbc::Src::R(1, dxbc::Src::kWWWW));
        a.OpBFI(dxbc::Dest::R(1, 0b0001), dxbc::Src::LU(2), dxbc::Src::LU(30),
                dxbc::Src::R(1, dxbc::Src::kWWWW), dxbc::Src::R(1, dxbc::Src::kXXXX));
        break;
      case xenos::ColorRenderTargetFormat::k_16_16:
      case xenos::ColorRenderTargetFormat::k_16_16_FLOAT:
        assert_true(source_is_uint);
        a.OpBFI(dxbc::Dest::R(1, 0b0001), dxbc::Src::LU(16), dxbc::Src::LU(16),
                dxbc::Src::R(1, dxbc::Src::kYYYY), dxbc::Src::R(1, dxbc::Src::kXXXX));
        break;
      case xenos::ColorRenderTargetFormat::k_16_16_16_16:
      case xenos::ColorRenderTargetFormat::k_16_16_16_16_FLOAT:
        assert_true(source_is_uint);
        a.OpBFI(dxbc::Dest::R(1, 0b0011), dxbc::Src::LU(16), dxbc::Src::LU(16),
                dxbc::Src::R(1, 0b1101), dxbc::Src::R(1, 0b1000));
        break;
      case xenos::ColorRenderTargetFormat::k_32_FLOAT:
      case xenos::ColorRenderTargetFormat::k_32_32_FLOAT:
        assert_true(source_is_uint);
        // Already has the needed representation.
        break;
    }
  }

  // Write the sample to the destination address stored in r0.z.
  a.OpStoreUAVTyped(dxbc::Dest::U(0, 0), dxbc::Src::R(0, dxbc::Src::kZZZZ), 1,
                    dxbc::Src::R(1, format_is_64bpp ? 0b0100 : dxbc::Src::kXXXX));

  a.OpRet();

  // Write the shader program length in dwords.
  built_shader_[shex_position_dwords + 1] = uint32_t(built_shader_.size()) - shex_position_dwords;

  {
    auto& blob_header =
        *reinterpret_cast<dxbc::BlobHeader*>(built_shader_.data() + blob_position_dwords);
    blob_header.fourcc = dxbc::BlobHeader::FourCC::kShaderEx;
    blob_position_dwords = uint32_t(built_shader_.size());
    blob_header.size_bytes = (blob_position_dwords - kBlobHeaderSizeDwords) * sizeof(uint32_t) -
                             built_shader_[blob_offset_position_dwords++];
  }

  // ***************************************************************************
  // Statistics
  // ***************************************************************************

  built_shader_[blob_offset_position_dwords] = uint32_t(blob_position_dwords * sizeof(uint32_t));
  uint32_t stat_position_dwords = blob_position_dwords + kBlobHeaderSizeDwords;
  built_shader_.resize(stat_position_dwords + sizeof(dxbc::Statistics) / sizeof(uint32_t));
  std::memcpy(built_shader_.data() + stat_position_dwords, &stat, sizeof(dxbc::Statistics));
  {
    auto& blob_header =
        *reinterpret_cast<dxbc::BlobHeader*>(built_shader_.data() + blob_position_dwords);
    blob_header.fourcc = dxbc::BlobHeader::FourCC::kStatistics;
    blob_position_dwords = uint32_t(built_shader_.size());
    blob_header.size_bytes = (blob_position_dwords - kBlobHeaderSizeDwords) * sizeof(uint32_t) -
                             built_shader_[blob_offset_position_dwords++];
  }

  // ***************************************************************************
  // Container header
  // ***************************************************************************

  uint32_t built_shader_size_bytes = uint32_t(built_shader_.size() * sizeof(uint32_t));
  {
    auto& container_header = *reinterpret_cast<dxbc::ContainerHeader*>(built_shader_.data());
    container_header.InitializeIdentification();
    container_header.size_bytes = built_shader_size_bytes;
    container_header.blob_count = kBlobCount;
    CalculateDXBCChecksum(reinterpret_cast<unsigned char*>(built_shader_.data()),
                          static_cast<unsigned int>(built_shader_size_bytes),
                          reinterpret_cast<unsigned int*>(&container_header.hash));
  }

  // ***************************************************************************
  // Pipeline
  // ***************************************************************************
  ID3D12PipelineState* pipeline = ui::d3d12::util::CreateComputePipeline(
      command_processor_.GetD3D12Provider().GetDevice(), built_shader_.data(),
      built_shader_size_bytes,
      key.is_depth ? dump_root_signature_depth_ : dump_root_signature_color_);
  const char* format_name = key.is_depth
                                ? xenos::GetDepthRenderTargetFormatName(key.GetDepthFormat())
                                : xenos::GetColorRenderTargetFormatName(key.GetColorFormat());
  if (pipeline) {
    std::u16string pipeline_name = rex::string::to_utf16(
        fmt::format("RT Dump {} {}xMSAA{}", format_name, uint32_t(1) << uint32_t(key.msaa_samples),
                    depth_center ? " Centre" : ""));
    pipeline->SetName(reinterpret_cast<LPCWSTR>(pipeline_name.c_str()));
  } else {
    REXGPU_ERROR(
        "D3D12RenderTargetCache: Failed to create a render target dumping "
        "pipeline for {}-sample render targets with format {}",
        uint32_t(1) << uint32_t(key.msaa_samples), format_name);
  }
  // Even if creation fails, still store the null pointer not to try to create
  // again.
  dump_pipelines_.emplace(key, pipeline);
  return pipeline;
}

ID3D12PipelineState* D3D12RenderTargetCache::GetOrCreateDirectResolvePipeline(
    DirectResolvePipelineKey key) {
  auto pipeline_it = direct_resolve_pipelines_.find(key);
  if (pipeline_it != direct_resolve_pipelines_.end()) {
    return pipeline_it->second;
  }
  ID3D12PipelineState* pipeline = nullptr;
  // Until dedicated direct host RT -> shared memory shaders are added, reuse
  // the resolve copy pipelines to keep all resolve shader modes wired for the
  // direct preflight path.
  size_t copy_shader_index = size_t(key.copy_shader);
  if (copy_shader_index < size_t(draw_util::ResolveCopyShaderIndex::kCount)) {
    pipeline = resolve_copy_pipelines_[copy_shader_index];
  }
  direct_resolve_pipelines_.emplace(key, pipeline);
  return pipeline;
}

bool D3D12RenderTargetCache::TryResolveCopyDirectly(const draw_util::ResolveInfo& resolve_info,
                                                    draw_util::ResolveCopyShaderIndex copy_shader,
                                                    bool draw_resolution_scaled) {
  ++direct_resolve_attempt_count_;
  (void)copy_shader;
  (void)draw_resolution_scaled;
  if (!direct_resolve_root_signature_color_ || !direct_resolve_root_signature_depth_) {
    return false;
  }

  uint32_t dump_base;
  uint32_t dump_row_length_used;
  uint32_t dump_rows;
  uint32_t dump_pitch;
  resolve_info.GetCopyEdramTileSpan(dump_base, dump_row_length_used, dump_rows, dump_pitch);
  GetResolveCopyDispatchesToDump(dump_base, dump_row_length_used, dump_rows, dump_pitch,
                                 dump_rectangles_, direct_resolve_dispatches_);
  if (direct_resolve_dispatches_.empty()) {
    return false;
  }
  const bool depth_center = IsResolveDumpingDepthCenter(resolve_info);

  for (const ResolveCopyDumpRectangle& rectangle : dump_rectangles_) {
    const auto* render_target = static_cast<const D3D12RenderTarget*>(rectangle.render_target);
    if (render_target == nullptr) {
      return false;
    }
    DumpPipelineKey dump_pipeline_key;
    dump_pipeline_key.msaa_samples = render_target->key().msaa_samples;
    dump_pipeline_key.resource_format = render_target->key().resource_format;
    dump_pipeline_key.is_depth = render_target->key().is_depth;
    dump_pipeline_key.source_scale_native =
        render_target->key().scale_native;
    dump_pipeline_key.native_layout = 0;
    dump_pipeline_key.depth_center =
        uint32_t(depth_center && render_target->key().is_depth &&
                 render_target->key().msaa_samples == xenos::MsaaSamples::k2X);
    if (!GetOrCreateDumpPipeline(dump_pipeline_key)) {
      return false;
    }
    DirectResolvePipelineKey direct_pipeline_key;
    direct_pipeline_key.dump_pipeline_key = dump_pipeline_key;
    direct_pipeline_key.copy_shader = copy_shader;
    direct_pipeline_key.draw_resolution_scaled = draw_resolution_scaled;
    if (!GetOrCreateDirectResolvePipeline(direct_pipeline_key)) {
      return false;
    }
  }

  // Dedicated direct resolve dispatches are staged behind the same preflight;
  // keep using the existing dump path until source-image direct shaders land.
  return DumpRenderTargets(dump_base, dump_row_length_used, dump_rows, dump_pitch, false,
                           depth_center);
}

bool D3D12RenderTargetCache::DumpRenderTargets(uint32_t dump_base, uint32_t dump_row_length_used,
                                               uint32_t dump_rows,
                                               uint32_t dump_pitch,
                                               bool native_layout, bool depth_center) {
  assert_true(GetPath() == Path::kHostRenderTargets);

  GetResolveCopyRectanglesToDump(dump_base, dump_row_length_used, dump_rows, dump_pitch,
                                 dump_rectangles_);
  if (dump_rectangles_.empty()) {
    return true;
  }

  // Clear previously set temporary indices.
  for (const ResolveCopyDumpRectangle& rectangle : dump_rectangles_) {
    auto& d3d12_rt = *static_cast<D3D12RenderTarget*>(rectangle.render_target);
    d3d12_rt.SetTemporarySortIndex(UINT32_MAX);
    d3d12_rt.SetTemporarySRVDescriptorIndex(UINT32_MAX);
    d3d12_rt.SetTemporarySRVDescriptorIndexStencil(UINT32_MAX);
  }
  // Gather all needed barriers and info needed to create descriptors and to
  // sort the invocations.
  TransitionEdramBuffer(D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  dump_invocations_.clear();
  dump_invocations_.reserve(dump_rectangles_.size());
  current_temporary_descriptors_cpu_.clear();
  bool any_sources_32bpp_64bpp[2] = {};
  uint32_t rt_sort_index = 0;
  for (const ResolveCopyDumpRectangle& rectangle : dump_rectangles_) {
    auto& d3d12_rt = *static_cast<D3D12RenderTarget*>(rectangle.render_target);
    command_processor_.PushTransitionBarrier(
        d3d12_rt.resource(),
        d3d12_rt.SetResourceState(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    if (d3d12_rt.temporary_sort_index() == UINT32_MAX) {
      d3d12_rt.SetTemporarySortIndex(rt_sort_index++);
    }
    if (d3d12_rt.temporary_srv_descriptor_index() == UINT32_MAX) {
      d3d12_rt.SetTemporarySRVDescriptorIndex(uint32_t(current_temporary_descriptors_cpu_.size()));
      current_temporary_descriptors_cpu_.push_back(d3d12_rt.descriptor_srv().GetHandle());
    }
    RenderTargetKey rt_key = d3d12_rt.key();
    if (rt_key.is_depth && d3d12_rt.temporary_srv_descriptor_index_stencil() == UINT32_MAX) {
      d3d12_rt.SetTemporarySRVDescriptorIndexStencil(
          uint32_t(current_temporary_descriptors_cpu_.size()));
      current_temporary_descriptors_cpu_.push_back(d3d12_rt.descriptor_srv_stencil().GetHandle());
    }
    any_sources_32bpp_64bpp[size_t(rt_key.Is64bpp())] = true;
    assert_true(!native_layout || rt_key.scale_native);
    DumpPipelineKey pipeline_key;
    pipeline_key.msaa_samples = rt_key.msaa_samples;
    pipeline_key.resource_format = rt_key.resource_format;
    pipeline_key.is_depth = rt_key.is_depth;
    pipeline_key.source_scale_native = rt_key.scale_native;
    pipeline_key.native_layout = uint32_t(native_layout);
    pipeline_key.depth_center = uint32_t(depth_center && rt_key.is_depth &&
                                         rt_key.msaa_samples == xenos::MsaaSamples::k2X);
    dump_invocations_.emplace_back(rectangle, pipeline_key);
  }
  // 32bpp and 64bpp.
  size_t edram_uav_indices[2] = {SIZE_MAX, SIZE_MAX};
  const ui::d3d12::D3D12Provider& provider = command_processor_.GetD3D12Provider();
  if (!bindless_resources_used_) {
    if (any_sources_32bpp_64bpp[0]) {
      edram_uav_indices[0] = current_temporary_descriptors_cpu_.size();
      current_temporary_descriptors_cpu_.push_back(provider.OffsetViewDescriptor(
          edram_buffer_descriptor_heap_start_, uint32_t(EdramBufferDescriptorIndex::kR32UintUAV)));
    }
    if (any_sources_32bpp_64bpp[1]) {
      edram_uav_indices[1] = current_temporary_descriptors_cpu_.size();
      current_temporary_descriptors_cpu_.push_back(
          provider.OffsetViewDescriptor(edram_buffer_descriptor_heap_start_,
                                        uint32_t(EdramBufferDescriptorIndex::kR32G32UintUAV)));
    }
  }

  // Copy source descriptors to a shader-visible heap.
  ID3D12Device* device = provider.GetDevice();
  uint32_t descriptor_count = uint32_t(current_temporary_descriptors_cpu_.size());
  current_temporary_descriptors_gpu_.resize(descriptor_count);
  if (!command_processor_.RequestOneUseSingleViewDescriptors(
          descriptor_count, current_temporary_descriptors_gpu_.data())) {
    return false;
  }
  for (uint32_t i = 0; i < descriptor_count; ++i) {
    device->CopyDescriptorsSimple(1, current_temporary_descriptors_gpu_[i].first,
                                  current_temporary_descriptors_cpu_[i],
                                  D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  }

  // Sort the invocations to reduce context and binding switches.
  std::sort(dump_invocations_.begin(), dump_invocations_.end());

  // Dump the render targets.
  DeferredCommandList& command_list = command_processor_.GetDeferredCommandList();
  ID3D12RootSignature* last_root_signature = nullptr;
  uint32_t root_parameters_set = 0;
  uint32_t last_descriptor_index_source = UINT32_MAX;
  uint32_t last_descriptor_index_stencil = UINT32_MAX;
  bool last_edram_uav_is_64bpp = false;
  DumpOffsets last_offsets;
  DumpPitches last_pitches;
  bool all_pipelines_available = true;
  for (const DumpInvocation& invocation : dump_invocations_) {
    const ResolveCopyDumpRectangle& rectangle = invocation.rectangle;
    auto& d3d12_rt = *static_cast<D3D12RenderTarget*>(rectangle.render_target);
    RenderTargetKey rt_key = d3d12_rt.key();
    DumpPipelineKey pipeline_key = invocation.pipeline_key;
    ID3D12PipelineState* pipeline = GetOrCreateDumpPipeline(pipeline_key);
    if (!pipeline) {
      all_pipelines_available = false;
      continue;
    }
    command_processor_.SetExternalPipeline(pipeline);

    ID3D12RootSignature* root_signature =
        pipeline_key.is_depth ? dump_root_signature_depth_ : dump_root_signature_color_;
    if (last_root_signature != root_signature) {
      last_root_signature = root_signature;
      command_list.D3DSetComputeRootSignature(root_signature);
      root_parameters_set = 0;
    }

    DumpRootParameter root_parameter_edram =
        pipeline_key.is_depth ? kDumpRootParameterDepthEdram : kDumpRootParameterColorEdram;
    uint32_t root_parameter_edram_bit = uint32_t(1) << root_parameter_edram;
    bool format_is_64bpp = rt_key.Is64bpp();
    if (last_edram_uav_is_64bpp != format_is_64bpp) {
      last_edram_uav_is_64bpp = format_is_64bpp;
      root_parameters_set &= ~root_parameter_edram_bit;
    }
    if (!(root_parameters_set & root_parameter_edram_bit)) {
      D3D12_GPU_DESCRIPTOR_HANDLE descriptor_handle_edram;
      if (bindless_resources_used_) {
        descriptor_handle_edram =
            command_processor_
                .GetEdramUintPow2BindlessUAVHandlePair(2 + uint32_t(last_edram_uav_is_64bpp))
                .second;
      } else {
        assert_true(edram_uav_indices[size_t(last_edram_uav_is_64bpp)] != SIZE_MAX);
        descriptor_handle_edram =
            current_temporary_descriptors_gpu_[edram_uav_indices[size_t(last_edram_uav_is_64bpp)]]
                .second;
      }
      command_list.D3DSetComputeRootDescriptorTable(root_parameter_edram, descriptor_handle_edram);
      root_parameters_set |= root_parameter_edram_bit;
    }

    DumpRootParameter root_parameter_pitches =
        pipeline_key.is_depth ? kDumpRootParameterDepthPitches : kDumpRootParameterColorPitches;
    uint32_t root_parameter_pitches_bit = uint32_t(1) << root_parameter_pitches;
    DumpPitches pitches;
    pitches.dest_pitch = dump_pitch;
    pitches.source_pitch = rt_key.GetPitchTiles();
    if (last_pitches != pitches) {
      last_pitches = pitches;
      root_parameters_set &= ~root_parameter_pitches_bit;
    }
    if (!(root_parameters_set & root_parameter_pitches_bit)) {
      command_list.D3DSetComputeRoot32BitConstants(
          root_parameter_pitches, sizeof(last_pitches) / sizeof(uint32_t), &last_pitches, 0);
      root_parameters_set |= root_parameter_pitches_bit;
    }

    if (pipeline_key.is_depth) {
      constexpr uint32_t kDumpRootParameterDepthStencilBit = uint32_t(1)
                                                             << kDumpRootParameterDepthStencil;
      uint32_t descriptor_index_stencil = d3d12_rt.temporary_srv_descriptor_index_stencil();
      assert_true(descriptor_index_stencil != UINT32_MAX);
      if (last_descriptor_index_stencil != descriptor_index_stencil) {
        last_descriptor_index_stencil = descriptor_index_stencil;
        root_parameters_set &= ~kDumpRootParameterDepthStencilBit;
      }
      if (!(root_parameters_set & kDumpRootParameterDepthStencilBit)) {
        command_list.D3DSetComputeRootDescriptorTable(
            kDumpRootParameterDepthStencil,
            current_temporary_descriptors_gpu_[last_descriptor_index_stencil].second);
        root_parameters_set |= kDumpRootParameterDepthStencilBit;
      }
    }

    constexpr uint32_t kDumpRootParameterSourceBit = uint32_t(1) << kDumpRootParameterSource;
    uint32_t descriptor_index_source = d3d12_rt.temporary_srv_descriptor_index();
    assert_true(descriptor_index_source != UINT32_MAX);
    if (last_descriptor_index_source != descriptor_index_source) {
      last_descriptor_index_source = descriptor_index_source;
      root_parameters_set &= ~kDumpRootParameterSourceBit;
    }
    if (!(root_parameters_set & kDumpRootParameterSourceBit)) {
      command_list.D3DSetComputeRootDescriptorTable(
          kDumpRootParameterSource,
          current_temporary_descriptors_gpu_[last_descriptor_index_source].second);
      root_parameters_set |= kDumpRootParameterSourceBit;
    }

    constexpr uint32_t kDumpRootParameterOffsetsBit = uint32_t(1) << kDumpRootParameterOffsets;
    DumpOffsets offsets;
    offsets.source_base_tiles = rt_key.base_tiles;
    ResolveCopyDumpRectangle::Dispatch dispatches[ResolveCopyDumpRectangle::kMaxDispatches];
    uint32_t dispatch_count = rectangle.GetDispatches(dump_pitch, dump_row_length_used, dispatches);
    for (uint32_t i = 0; i < dispatch_count; ++i) {
      const ResolveCopyDumpRectangle::Dispatch& dispatch = dispatches[i];
      offsets.dispatch_first_tile = dump_base + dispatch.offset;
      if (last_offsets != offsets) {
        last_offsets = offsets;
        root_parameters_set &= ~kDumpRootParameterOffsetsBit;
      }
      if (!(root_parameters_set & kDumpRootParameterOffsetsBit)) {
        command_list.D3DSetComputeRoot32BitConstants(
            kDumpRootParameterOffsets, sizeof(last_offsets) / sizeof(uint32_t), &last_offsets, 0);
        root_parameters_set |= kDumpRootParameterOffsetsBit;
      }
      command_processor_.SubmitBarriers();
      // Processing 40 x 16 x scale samples per dispatch (a 32bpp tile in two
      // dispatches at 1x1 scale, 64bpp in one dispatch).
      command_list.D3DDispatch((dispatch.width_tiles *
                                (native_layout ? 1
                                               : draw_resolution_scale_x()))
                                   << uint32_t(!format_is_64bpp),
                               dispatch.height_tiles *
                                   (native_layout ? 1
                                                  : draw_resolution_scale_y()),
                               1);
    }
    MarkEdramBufferModified();
  }
  return all_pipelines_available;
}

}  // namespace rex::graphics::d3d12
