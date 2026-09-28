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
#include <array>
#include <filesystem>
#include <string>
#include <atomic>
#include <chrono>
#include <future>
#include <cfloat>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <memory>
#include <utility>
#include <vector>

#include <rex/assert.h>
#include <rex/cvar.h>
#include <rex/dbg.h>
#include <rex/graphics/d3d12/command_processor.h>
#include <rex/graphics/d3d12/shared_memory.h>
#include <rex/graphics/d3d12/texture_cache.h>
#include <rex/graphics/flags.h>
#include <rex/graphics/pipeline/texture/info.h>
#include <rex/graphics/pipeline/texture/conversion.h>
#include <rex/graphics/pipeline/texture/texture_pack.h>
#include <rex/graphics/pipeline/texture/util.h>
#include <rex/graphics/xenos.h>
#include <rex/logging.h>
#include <rex/perf/counter.h>
#include <rex/math.h>
#include <rex/ui/d3d12/d3d12_upload_buffer_pool.h>
#include <rex/ui/d3d12/d3d12_util.h>

REXCVAR_DECLARE(std::string, gpu_prompt_icon_source);

REXCVAR_DEFINE_BOOL(graphics_hd_textures, false, "Graphics",
                    "Show installed HD texture packs (texture_packs folder next to the game) "
                    "instead of the title's textures")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(gpu_texture_dump, false, "GPU",
                    "HD texture packs (modding): write each newly seen texture once as a DDS "
                    "file into the dump folder (host format, all mips)");
REXCVAR_DEFINE_BOOL(gpu_texture_replace, false, "GPU",
                    "HD texture packs, developer switch: same as graphics_hd_textures");
REXCVAR_DEFINE_UINT32(gpu_texture_replace_upload_mb_per_frame, 8, "GPU",
                      "HD texture packs: most replacement data uploaded per frame (MB)");

namespace {
std::filesystem::path TexturePackFolder() { return rex::graphics::texture_pack::PackFolder(); }

std::filesystem::path TextureDumpFolder() { return rex::graphics::texture_pack::DumpFolder(); }

bool HdTexturePacksEnabled() {
  return REXCVAR_GET(graphics_hd_textures) || REXCVAR_GET(gpu_texture_replace);
}

// HD texture packs, or the textures of an active language pack (set once
// with the startup configuration, before any texture loads).
bool TextureReplacementEnabled() {
  static const bool language = !rex::graphics::texture_pack::LanguageFolder().empty();
  return HdTexturePacksEnabled() || language;
}
}  // namespace

REXCVAR_DEFINE_UINT32(
    embedded_texture_readback_address_min, 0, "GPU/Diagnostics",
    "Inclusive guest base address for the one-shot host texture load readback")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_UINT32(
    embedded_texture_readback_address_max, 0, "GPU/Diagnostics",
    "Exclusive guest base address for the one-shot host texture load readback")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_BOOL(embedded_temporal_depth_snapshot, false, "GPU/Diagnostics",
    "Snapshot selected bounded D24FS8 texture readbacks on GPU; not DLSS activation")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(embedded_temporal_depth_first_binding, false, "GPU/Diagnostics",
    "Attempt one actual depth binding snapshot with frame diagnostics; no scene-depth claim")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_BOOL(embedded_pc_scene_history, false, "GPU/Experimental",
    "Own rendered scene-color inputs on GPU; no temporal reconstruction activation")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_BOOL(d3d12_texture_heap_pool, true, "GPU",
    "Place textures in pre-created heaps instead of one committed allocation each "
    "(avoids per-texture driver allocation and residency waits when textures first appear)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace rex::graphics::d3d12 {

namespace {

constexpr bool kPromptTextureDiagnosticsEnabled = false;

uint32_t HashTextureDiagnosticBytes(const uint8_t* bytes, size_t length) {
  uint32_t hash = 2166136261u;
  for (size_t i = 0; i < length; ++i) {
    hash = (hash ^ bytes[i]) * 16777619u;
  }
  return hash;
}

uint32_t HashTextureDiagnosticRows(const uint8_t* bytes,
                                   uint32_t row_pitch,
                                   uint32_t row_bytes,
                                   uint32_t row_count) {
  uint32_t hash = 2166136261u;
  for (uint32_t row = 0; row < row_count; ++row) {
    const uint8_t* row_bytes_begin = bytes + size_t(row) * row_pitch;
    for (uint32_t byte = 0; byte < row_bytes; ++byte) {
      hash = (hash ^ row_bytes_begin[byte]) * 16777619u;
    }
  }
  return hash;
}

}  // namespace

void D3D12TextureCache::ArmTextureReadbackDiagnostic(
    uint32_t guest_address, uint32_t guest_length) {
  if (!guest_length || texture_readback_diagnostic_started_) {
    return;
  }
  texture_readback_armed_address_min_ = guest_address;
  texture_readback_armed_address_max_ =
      uint64_t(guest_address) + uint64_t(guest_length);
  std::fprintf(
      stderr,
      "REX_EMBEDDED_TEXTURE_LOAD_READBACK_ARMED address=0x%08X bytes=%u "
      "end=0x%llX\n",
      guest_address, guest_length,
      static_cast<unsigned long long>(texture_readback_armed_address_max_));
  std::fflush(stderr);
}

// Generated with `xb buildshaders`.
namespace shaders {
#include "../shaders/bytecode/d3d12_5_1/scaled_resolve_initialize_cs.h"
#include "../shaders/bytecode/d3d12_5_1/texture_load_128bpb_cs.h"
#include "../shaders/bytecode/d3d12_5_1/texture_load_128bpb_scaled_cs.h"
#include "../shaders/bytecode/d3d12_5_1/texture_load_16bpb_cs.h"
#include "../shaders/bytecode/d3d12_5_1/texture_load_16bpb_scaled_cs.h"
#include "../shaders/bytecode/d3d12_5_1/texture_load_32bpb_cs.h"
#include "../shaders/bytecode/d3d12_5_1/texture_load_32bpb_scaled_cs.h"
#include "../shaders/bytecode/d3d12_5_1/texture_load_64bpb_cs.h"
#include "../shaders/bytecode/d3d12_5_1/texture_load_64bpb_scaled_cs.h"
#include "../shaders/bytecode/d3d12_5_1/texture_load_8bpb_cs.h"
#include "../shaders/bytecode/d3d12_5_1/texture_load_8bpb_scaled_cs.h"
#include "../shaders/bytecode/d3d12_5_1/texture_load_bgrg8_rgb8_cs.h"
#include "../shaders/bytecode/d3d12_5_1/texture_load_bgrg8_rgbg8_cs.h"
#include "../shaders/bytecode/d3d12_5_1/texture_load_ctx1_cs.h"
#include "../shaders/bytecode/d3d12_5_1/texture_load_depth_float_cs.h"
#include "../shaders/bytecode/d3d12_5_1/texture_load_depth_float_scaled_cs.h"
#include "../shaders/bytecode/d3d12_5_1/texture_load_depth_unorm_cs.h"
#include "../shaders/bytecode/d3d12_5_1/texture_load_depth_unorm_scaled_cs.h"
#include "../shaders/bytecode/d3d12_5_1/texture_load_dxn_rg8_cs.h"
#include "../shaders/bytecode/d3d12_5_1/texture_load_dxt1_rgba8_cs.h"
#include "../shaders/bytecode/d3d12_5_1/texture_load_dxt3_rgba8_cs.h"
#include "../shaders/bytecode/d3d12_5_1/texture_load_dxt3a_cs.h"
#include "../shaders/bytecode/d3d12_5_1/texture_load_dxt3aas1111_bgra4_cs.h"
#include "../shaders/bytecode/d3d12_5_1/texture_load_dxt5_rgba8_cs.h"
#include "../shaders/bytecode/d3d12_5_1/texture_load_dxt5a_r8_cs.h"
#include "../shaders/bytecode/d3d12_5_1/texture_load_gbgr8_grgb8_cs.h"
#include "../shaders/bytecode/d3d12_5_1/texture_load_gbgr8_rgb8_cs.h"
#include "../shaders/bytecode/d3d12_5_1/texture_load_r10g11b11_rgba16_cs.h"
#include "../shaders/bytecode/d3d12_5_1/texture_load_r10g11b11_rgba16_scaled_cs.h"
#include "../shaders/bytecode/d3d12_5_1/texture_load_r10g11b11_rgba16_snorm_cs.h"
#include "../shaders/bytecode/d3d12_5_1/texture_load_r10g11b11_rgba16_snorm_scaled_cs.h"
#include "../shaders/bytecode/d3d12_5_1/texture_load_r11g11b10_rgba16_cs.h"
#include "../shaders/bytecode/d3d12_5_1/texture_load_r11g11b10_rgba16_scaled_cs.h"
#include "../shaders/bytecode/d3d12_5_1/texture_load_r11g11b10_rgba16_snorm_cs.h"
#include "../shaders/bytecode/d3d12_5_1/texture_load_r11g11b10_rgba16_snorm_scaled_cs.h"
#include "../shaders/bytecode/d3d12_5_1/texture_load_r4g4b4a4_b4g4r4a4_cs.h"
#include "../shaders/bytecode/d3d12_5_1/texture_load_r4g4b4a4_b4g4r4a4_scaled_cs.h"
#include "../shaders/bytecode/d3d12_5_1/texture_load_r5g5b5a1_b5g5r5a1_cs.h"
#include "../shaders/bytecode/d3d12_5_1/texture_load_r5g5b5a1_b5g5r5a1_scaled_cs.h"
#include "../shaders/bytecode/d3d12_5_1/texture_load_r5g5b6_b5g6r5_swizzle_rbga_cs.h"
#include "../shaders/bytecode/d3d12_5_1/texture_load_r5g5b6_b5g6r5_swizzle_rbga_scaled_cs.h"
#include "../shaders/bytecode/d3d12_5_1/texture_load_r5g6b5_b5g6r5_cs.h"
#include "../shaders/bytecode/d3d12_5_1/texture_load_r5g6b5_b5g6r5_scaled_cs.h"
}  // namespace shaders

const D3D12TextureCache::HostFormat D3D12TextureCache::host_formats_[64] = {
    // k_1_REVERSE
    {DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown,
     xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR},
    // k_1
    {DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown,
     xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR},
    // k_8
    {DXGI_FORMAT_R8_TYPELESS, DXGI_FORMAT_R8_UNORM, kLoadShaderIndex8bpb, DXGI_FORMAT_R8_SNORM,
     kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown,
     xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR},
    // k_1_5_5_5
    // Red and blue swapped in the load shader for simplicity.
    {DXGI_FORMAT_B5G5R5A1_UNORM, DXGI_FORMAT_B5G5R5A1_UNORM, kLoadShaderIndexR5G5B5A1ToB5G5R5A1,
     DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA},
    // k_5_6_5
    // Red and blue swapped in the load shader for simplicity.
    {DXGI_FORMAT_B5G6R5_UNORM, DXGI_FORMAT_B5G6R5_UNORM, kLoadShaderIndexR5G6B5ToB5G6R5,
     DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, xenos::XE_GPU_TEXTURE_SWIZZLE_RGBB},
    // k_6_5_5
    // On the host, green bits in blue, blue bits in green.
    {DXGI_FORMAT_B5G6R5_UNORM, DXGI_FORMAT_B5G6R5_UNORM,
     kLoadShaderIndexR5G5B6ToB5G6R5WithRBGASwizzle, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown,
     false, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown, XE_GPU_MAKE_TEXTURE_SWIZZLE(R, B, G, G)},
    // k_8_8_8_8
    {DXGI_FORMAT_R8G8B8A8_TYPELESS, DXGI_FORMAT_R8G8B8A8_UNORM, kLoadShaderIndex32bpb,
     DXGI_FORMAT_R8G8B8A8_SNORM, kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA},
    // k_2_10_10_10
    {DXGI_FORMAT_R10G10B10A2_TYPELESS, DXGI_FORMAT_R10G10B10A2_UNORM, kLoadShaderIndex32bpb,
     DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA},
    // k_8_A
    {DXGI_FORMAT_R8_TYPELESS, DXGI_FORMAT_R8_UNORM, kLoadShaderIndex8bpb, DXGI_FORMAT_R8_SNORM,
     kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown,
     xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR},
    // k_8_B
    {DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown,
     xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR},
    // k_8_8
    {DXGI_FORMAT_R8G8_TYPELESS, DXGI_FORMAT_R8G8_UNORM, kLoadShaderIndex16bpb,
     DXGI_FORMAT_R8G8_SNORM, kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, xenos::XE_GPU_TEXTURE_SWIZZLE_RGGG},
    // k_Cr_Y1_Cb_Y0_REP
    // Red and blue swapped in the load shader for simplicity.
    // TODO(Triang3l): The DXGI_FORMAT_R8G8B8A8_U/SNORM conversion is usable for
    // the signed version, separate unsigned and signed load shaders completely
    // (as one doesn't need decompression for this format, while another does).
    {DXGI_FORMAT_G8R8_G8B8_UNORM, DXGI_FORMAT_G8R8_G8B8_UNORM, kLoadShaderIndexGBGR8ToGRGB8,
     DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown, true, DXGI_FORMAT_R8G8B8A8_UNORM,
     kLoadShaderIndexGBGR8ToRGB8, xenos::XE_GPU_TEXTURE_SWIZZLE_RGBB},
    // k_Y1_Cr_Y0_Cb_REP
    // Red and blue swapped in the load shader for simplicity.
    // TODO(Triang3l): The DXGI_FORMAT_R8G8B8A8_U/SNORM conversion is usable for
    // the signed version, separate unsigned and signed load shaders completely
    // (as one doesn't need decompression for this format, while another does).
    {DXGI_FORMAT_R8G8_B8G8_UNORM, DXGI_FORMAT_R8G8_B8G8_UNORM, kLoadShaderIndexBGRG8ToRGBG8,
     DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown, true, DXGI_FORMAT_R8G8B8A8_UNORM,
     kLoadShaderIndexBGRG8ToRGB8, xenos::XE_GPU_TEXTURE_SWIZZLE_RGBB},
    // k_16_16_EDRAM
    // Not usable as a texture, also has -32...32 range.
    {DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown,
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGGG},
    // k_8_8_8_8_A
    {DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown,
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA},
    // k_4_4_4_4
    // Red and blue swapped in the load shader for simplicity.
    {DXGI_FORMAT_B4G4R4A4_UNORM, DXGI_FORMAT_B4G4R4A4_UNORM, kLoadShaderIndexRGBA4ToBGRA4,
     DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA},
    // k_10_11_11
    {DXGI_FORMAT_R16G16B16A16_TYPELESS, DXGI_FORMAT_R16G16B16A16_UNORM,
     kLoadShaderIndexR11G11B10ToRGBA16, DXGI_FORMAT_R16G16B16A16_SNORM,
     kLoadShaderIndexR11G11B10ToRGBA16SNorm, false, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown,
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGBB},
    // k_11_11_10
    {DXGI_FORMAT_R16G16B16A16_TYPELESS, DXGI_FORMAT_R16G16B16A16_UNORM,
     kLoadShaderIndexR10G11B11ToRGBA16, DXGI_FORMAT_R16G16B16A16_SNORM,
     kLoadShaderIndexR10G11B11ToRGBA16SNorm, false, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown,
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGBB},
    // k_DXT1
    {DXGI_FORMAT_BC1_UNORM, DXGI_FORMAT_BC1_UNORM, kLoadShaderIndex64bpb, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, true, DXGI_FORMAT_R8G8B8A8_UNORM, kLoadShaderIndexDXT1ToRGBA8,
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA},
    // k_DXT2_3
    {DXGI_FORMAT_BC2_UNORM, DXGI_FORMAT_BC2_UNORM, kLoadShaderIndex128bpb, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, true, DXGI_FORMAT_R8G8B8A8_UNORM, kLoadShaderIndexDXT3ToRGBA8,
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA},
    // k_DXT4_5
    {DXGI_FORMAT_BC3_UNORM, DXGI_FORMAT_BC3_UNORM, kLoadShaderIndex128bpb, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, true, DXGI_FORMAT_R8G8B8A8_UNORM, kLoadShaderIndexDXT5ToRGBA8,
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA},
    // k_16_16_16_16_EDRAM
    // Not usable as a texture, also has -32...32 range.
    {DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown,
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA},
    // R32_FLOAT for depth because shaders would require an additional SRV to
    // sample stencil, which we don't provide.
    // k_24_8
    {DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R32_FLOAT, kLoadShaderIndexDepthUnorm,
     DXGI_FORMAT_R32_FLOAT, kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR},
    // k_24_8_FLOAT
    {DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R32_FLOAT, kLoadShaderIndexDepthFloat,
     DXGI_FORMAT_R32_FLOAT, kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR},
    // k_16
    {DXGI_FORMAT_R16_TYPELESS, DXGI_FORMAT_R16_UNORM, kLoadShaderIndex16bpb, DXGI_FORMAT_R16_SNORM,
     kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown,
     xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR},
    // k_16_16
    {DXGI_FORMAT_R16G16_TYPELESS, DXGI_FORMAT_R16G16_UNORM, kLoadShaderIndex32bpb,
     DXGI_FORMAT_R16G16_SNORM, kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, xenos::XE_GPU_TEXTURE_SWIZZLE_RGGG},
    // k_16_16_16_16
    {DXGI_FORMAT_R16G16B16A16_TYPELESS, DXGI_FORMAT_R16G16B16A16_UNORM, kLoadShaderIndex64bpb,
     DXGI_FORMAT_R16G16B16A16_SNORM, kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA},
    // k_16_EXPAND
    {DXGI_FORMAT_R16_FLOAT, DXGI_FORMAT_R16_FLOAT, kLoadShaderIndex16bpb, DXGI_FORMAT_R16_FLOAT,
     kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown,
     xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR},
    // k_16_16_EXPAND
    {DXGI_FORMAT_R16G16_FLOAT, DXGI_FORMAT_R16G16_FLOAT, kLoadShaderIndex32bpb,
     DXGI_FORMAT_R16G16_FLOAT, kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, xenos::XE_GPU_TEXTURE_SWIZZLE_RGGG},
    // k_16_16_16_16_EXPAND
    {DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R16G16B16A16_FLOAT, kLoadShaderIndex64bpb,
     DXGI_FORMAT_R16G16B16A16_FLOAT, kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA},
    // k_16_FLOAT
    {DXGI_FORMAT_R16_FLOAT, DXGI_FORMAT_R16_FLOAT, kLoadShaderIndex16bpb, DXGI_FORMAT_R16_FLOAT,
     kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown,
     xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR},
    // k_16_16_FLOAT
    {DXGI_FORMAT_R16G16_FLOAT, DXGI_FORMAT_R16G16_FLOAT, kLoadShaderIndex32bpb,
     DXGI_FORMAT_R16G16_FLOAT, kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, xenos::XE_GPU_TEXTURE_SWIZZLE_RGGG},
    // k_16_16_16_16_FLOAT
    {DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R16G16B16A16_FLOAT, kLoadShaderIndex64bpb,
     DXGI_FORMAT_R16G16B16A16_FLOAT, kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA},
    // k_32
    {DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown,
     xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR},
    // k_32_32
    {DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown,
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGGG},
    // k_32_32_32_32
    {DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown,
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA},
    // k_32_FLOAT
    {DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R32_FLOAT, kLoadShaderIndex32bpb, DXGI_FORMAT_R32_FLOAT,
     kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown,
     xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR},
    // k_32_32_FLOAT
    {DXGI_FORMAT_R32G32_FLOAT, DXGI_FORMAT_R32G32_FLOAT, kLoadShaderIndex64bpb,
     DXGI_FORMAT_R32G32_FLOAT, kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, xenos::XE_GPU_TEXTURE_SWIZZLE_RGGG},
    // k_32_32_32_32_FLOAT
    {DXGI_FORMAT_R32G32B32A32_FLOAT, DXGI_FORMAT_R32G32B32A32_FLOAT, kLoadShaderIndex128bpb,
     DXGI_FORMAT_R32G32B32A32_FLOAT, kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA},
    // k_32_AS_8
    {DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown,
     xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR},
    // k_32_AS_8_8
    {DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown,
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGGG},
    // k_16_MPEG
    {DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown,
     xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR},
    // k_16_16_MPEG
    {DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown,
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGGG},
    // k_8_INTERLACED
    {DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown,
     xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR},
    // k_32_AS_8_INTERLACED
    {DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown,
     xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR},
    // k_32_AS_8_8_INTERLACED
    {DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown,
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGGG},
    // k_16_INTERLACED
    {DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown,
     xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR},
    // k_16_MPEG_INTERLACED
    {DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown,
     xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR},
    // k_16_16_MPEG_INTERLACED
    {DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown,
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGGG},
    // k_DXN
    {DXGI_FORMAT_BC5_UNORM, DXGI_FORMAT_BC5_UNORM, kLoadShaderIndex128bpb, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, true, DXGI_FORMAT_R8G8_UNORM, kLoadShaderIndexDXNToRG8,
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGGG},
    // k_8_8_8_8_AS_16_16_16_16
    {DXGI_FORMAT_R8G8B8A8_TYPELESS, DXGI_FORMAT_R8G8B8A8_UNORM, kLoadShaderIndex32bpb,
     DXGI_FORMAT_R8G8B8A8_SNORM, kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA},
    // k_DXT1_AS_16_16_16_16
    {DXGI_FORMAT_BC1_UNORM, DXGI_FORMAT_BC1_UNORM, kLoadShaderIndex64bpb, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, true, DXGI_FORMAT_R8G8B8A8_UNORM, kLoadShaderIndexDXT1ToRGBA8,
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA},
    // k_DXT2_3_AS_16_16_16_16
    {DXGI_FORMAT_BC2_UNORM, DXGI_FORMAT_BC2_UNORM, kLoadShaderIndex128bpb, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, true, DXGI_FORMAT_R8G8B8A8_UNORM, kLoadShaderIndexDXT3ToRGBA8,
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA},
    // k_DXT4_5_AS_16_16_16_16
    {DXGI_FORMAT_BC3_UNORM, DXGI_FORMAT_BC3_UNORM, kLoadShaderIndex128bpb, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, true, DXGI_FORMAT_R8G8B8A8_UNORM, kLoadShaderIndexDXT5ToRGBA8,
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA},
    // k_2_10_10_10_AS_16_16_16_16
    {DXGI_FORMAT_R10G10B10A2_UNORM, DXGI_FORMAT_R10G10B10A2_UNORM, kLoadShaderIndex32bpb,
     DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA},
    // k_10_11_11_AS_16_16_16_16
    {DXGI_FORMAT_R16G16B16A16_TYPELESS, DXGI_FORMAT_R16G16B16A16_UNORM,
     kLoadShaderIndexR11G11B10ToRGBA16, DXGI_FORMAT_R16G16B16A16_SNORM,
     kLoadShaderIndexR11G11B10ToRGBA16SNorm, false, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown,
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGBB},
    // k_11_11_10_AS_16_16_16_16
    {DXGI_FORMAT_R16G16B16A16_TYPELESS, DXGI_FORMAT_R16G16B16A16_UNORM,
     kLoadShaderIndexR10G11B11ToRGBA16, DXGI_FORMAT_R16G16B16A16_SNORM,
     kLoadShaderIndexR10G11B11ToRGBA16SNorm, false, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown,
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGBB},
    // k_32_32_32_FLOAT
    {DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown,
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGBB},
    // k_DXT3A
    // R8_UNORM has the same size as BC2, but doesn't have the 4x4 size
    // alignment requirement.
    {DXGI_FORMAT_R8_UNORM, DXGI_FORMAT_R8_UNORM, kLoadShaderIndexDXT3A, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown,
     xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR},
    // k_DXT5A
    {DXGI_FORMAT_BC4_UNORM, DXGI_FORMAT_BC4_UNORM, kLoadShaderIndex64bpb, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, true, DXGI_FORMAT_R8_UNORM, kLoadShaderIndexDXT5AToR8,
     xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR},
    // k_CTX1
    {DXGI_FORMAT_R8G8_UNORM, DXGI_FORMAT_R8G8_UNORM, kLoadShaderIndexCTX1, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown,
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGGG},
    // k_DXT3A_AS_1_1_1_1
    {DXGI_FORMAT_B4G4R4A4_UNORM, DXGI_FORMAT_B4G4R4A4_UNORM, kLoadShaderIndexDXT3AAs1111ToBGRA4,
     DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA},
    // k_8_8_8_8_GAMMA_EDRAM
    // Not usable as a texture.
    {DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown,
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA},
    // k_2_10_10_10_FLOAT_EDRAM
    // Not usable as a texture.
    {DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown,
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA},
};

D3D12TextureCache::D3D12TextureCache(const RegisterFile& register_file,
                                     D3D12SharedMemory& shared_memory,
                                     uint32_t draw_resolution_scale_x,
                                     uint32_t draw_resolution_scale_y,
                                     D3D12CommandProcessor& command_processor,
                                     bool bindless_resources_used)
    : TextureCache(register_file, shared_memory, draw_resolution_scale_x, draw_resolution_scale_y),
      command_processor_(command_processor),
      bindless_resources_used_(bindless_resources_used) {}

D3D12TextureCache::~D3D12TextureCache() {
  // While the texture descriptor cache still exists (referenced by
  // ~D3D12Texture), destroy all textures.
  DestroyAllTextures(true);

  // First release the buffers to detach them from the heaps.
  for (std::unique_ptr<ScaledResolveVirtualBuffer>& scaled_resolve_buffer_ptr :
       scaled_resolve_2gb_buffers_) {
    scaled_resolve_buffer_ptr.reset();
  }
  scaled_resolve_heaps_.clear();
  COUNT_profile_set("gpu/texture_cache/scaled_resolve_buffer_used_mb", 0);
}

bool D3D12TextureCache::Initialize() {
  const ui::d3d12::D3D12Provider& provider = command_processor_.GetD3D12Provider();
  ID3D12Device* device = provider.GetDevice();

  if (REXCVAR_GET(d3d12_texture_heap_pool)) {
    // Created during loading so first appearances of textures in play only
    // place resources; more heaps are added when these fill up.
    for (uint32_t i = 0; i < kTextureHeapInitialCount; ++i) {
      if (!CreateTextureHeap()) {
        break;
      }
    }
  }

  if (IsDrawResolutionScaled()) {
    // Buffers not used yet - no need aliasing barriers to change ownership of
    // gigabytes between even and odd buffers.
    std::memset(scaled_resolve_1gb_buffer_indices_, UINT8_MAX,
                sizeof(scaled_resolve_1gb_buffer_indices_));
    assert_true(scaled_resolve_heaps_.empty());
    uint64_t scaled_resolve_address_space_size =
        uint64_t(SharedMemory::kBufferSize) *
        (draw_resolution_scale_x() * draw_resolution_scale_y());
    scaled_resolve_heaps_.resize(
        size_t(scaled_resolve_address_space_size >> kScaledResolveHeapSizeLog2));
  }
  scaled_resolve_heap_count_ = 0;

  // Create the loading root signature.
  D3D12_ROOT_PARAMETER root_parameters[3];
  // Parameter 0 is constants (changed multiple times when untiling).
  root_parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  root_parameters[0].Constants.ShaderRegister = 0;
  root_parameters[0].Constants.RegisterSpace = 0;
  root_parameters[0].Constants.Num32BitValues = sizeof(LoadConstants) / sizeof(uint32_t);
  root_parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  // Parameter 1 is the source (may be changed multiple times for the same
  // destination).
  D3D12_DESCRIPTOR_RANGE root_dest_range;
  root_dest_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
  root_dest_range.NumDescriptors = 1;
  root_dest_range.BaseShaderRegister = 0;
  root_dest_range.RegisterSpace = 0;
  root_dest_range.OffsetInDescriptorsFromTableStart = 0;
  root_parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  root_parameters[1].DescriptorTable.NumDescriptorRanges = 1;
  root_parameters[1].DescriptorTable.pDescriptorRanges = &root_dest_range;
  root_parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  // Parameter 2 is the destination.
  D3D12_DESCRIPTOR_RANGE root_source_range;
  root_source_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
  root_source_range.NumDescriptors = 1;
  root_source_range.BaseShaderRegister = 0;
  root_source_range.RegisterSpace = 0;
  root_source_range.OffsetInDescriptorsFromTableStart = 0;
  root_parameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  root_parameters[2].DescriptorTable.NumDescriptorRanges = 1;
  root_parameters[2].DescriptorTable.pDescriptorRanges = &root_source_range;
  root_parameters[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  D3D12_ROOT_SIGNATURE_DESC root_signature_desc;
  root_signature_desc.NumParameters = UINT(rex::countof(root_parameters));
  root_signature_desc.pParameters = root_parameters;
  root_signature_desc.NumStaticSamplers = 0;
  root_signature_desc.pStaticSamplers = nullptr;
  root_signature_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;
  *(load_root_signature_.ReleaseAndGetAddressOf()) =
      ui::d3d12::util::CreateRootSignature(provider, root_signature_desc);
  if (!load_root_signature_) {
    REXGPU_ERROR(
        "D3D12TextureCache: Failed to create the texture loading root "
        "signature");
    return false;
  }

  if (IsDrawResolutionScaled()) {
    *(scaled_resolve_initialize_pipeline_.ReleaseAndGetAddressOf()) =
        ui::d3d12::util::CreateComputePipeline(
            device, shaders::scaled_resolve_initialize_cs,
            sizeof(shaders::scaled_resolve_initialize_cs),
            load_root_signature_.Get());
    if (!scaled_resolve_initialize_pipeline_) {
      REXGPU_ERROR(
          "D3D12TextureCache: Failed to create the scaled resolve page "
          "initialization pipeline");
      return false;
    }
  }

  // Specify the load shader code.
  D3D12_SHADER_BYTECODE load_shader_code[kLoadShaderCount] = {};
  load_shader_code[kLoadShaderIndex8bpb] =
      D3D12_SHADER_BYTECODE{shaders::texture_load_8bpb_cs, sizeof(shaders::texture_load_8bpb_cs)};
  load_shader_code[kLoadShaderIndex16bpb] =
      D3D12_SHADER_BYTECODE{shaders::texture_load_16bpb_cs, sizeof(shaders::texture_load_16bpb_cs)};
  load_shader_code[kLoadShaderIndex32bpb] =
      D3D12_SHADER_BYTECODE{shaders::texture_load_32bpb_cs, sizeof(shaders::texture_load_32bpb_cs)};
  load_shader_code[kLoadShaderIndex64bpb] =
      D3D12_SHADER_BYTECODE{shaders::texture_load_64bpb_cs, sizeof(shaders::texture_load_64bpb_cs)};
  load_shader_code[kLoadShaderIndex128bpb] = D3D12_SHADER_BYTECODE{
      shaders::texture_load_128bpb_cs, sizeof(shaders::texture_load_128bpb_cs)};
  load_shader_code[kLoadShaderIndexR5G5B5A1ToB5G5R5A1] =
      D3D12_SHADER_BYTECODE{shaders::texture_load_r5g5b5a1_b5g5r5a1_cs,
                            sizeof(shaders::texture_load_r5g5b5a1_b5g5r5a1_cs)};
  load_shader_code[kLoadShaderIndexR5G6B5ToB5G6R5] = D3D12_SHADER_BYTECODE{
      shaders::texture_load_r5g6b5_b5g6r5_cs, sizeof(shaders::texture_load_r5g6b5_b5g6r5_cs)};
  load_shader_code[kLoadShaderIndexR5G5B6ToB5G6R5WithRBGASwizzle] =
      D3D12_SHADER_BYTECODE{shaders::texture_load_r5g5b6_b5g6r5_swizzle_rbga_cs,
                            sizeof(shaders::texture_load_r5g5b6_b5g6r5_swizzle_rbga_cs)};
  load_shader_code[kLoadShaderIndexRGBA4ToBGRA4] =
      D3D12_SHADER_BYTECODE{shaders::texture_load_r4g4b4a4_b4g4r4a4_cs,
                            sizeof(shaders::texture_load_r4g4b4a4_b4g4r4a4_cs)};
  load_shader_code[kLoadShaderIndexGBGR8ToGRGB8] = D3D12_SHADER_BYTECODE{
      shaders::texture_load_gbgr8_grgb8_cs, sizeof(shaders::texture_load_gbgr8_grgb8_cs)};
  load_shader_code[kLoadShaderIndexGBGR8ToRGB8] = D3D12_SHADER_BYTECODE{
      shaders::texture_load_gbgr8_rgb8_cs, sizeof(shaders::texture_load_gbgr8_rgb8_cs)};
  load_shader_code[kLoadShaderIndexBGRG8ToRGBG8] = D3D12_SHADER_BYTECODE{
      shaders::texture_load_bgrg8_rgbg8_cs, sizeof(shaders::texture_load_bgrg8_rgbg8_cs)};
  load_shader_code[kLoadShaderIndexBGRG8ToRGB8] = D3D12_SHADER_BYTECODE{
      shaders::texture_load_bgrg8_rgb8_cs, sizeof(shaders::texture_load_bgrg8_rgb8_cs)};
  load_shader_code[kLoadShaderIndexR10G11B11ToRGBA16] = D3D12_SHADER_BYTECODE{
      shaders::texture_load_r10g11b11_rgba16_cs, sizeof(shaders::texture_load_r10g11b11_rgba16_cs)};
  load_shader_code[kLoadShaderIndexR10G11B11ToRGBA16SNorm] =
      D3D12_SHADER_BYTECODE{shaders::texture_load_r10g11b11_rgba16_snorm_cs,
                            sizeof(shaders::texture_load_r10g11b11_rgba16_snorm_cs)};
  load_shader_code[kLoadShaderIndexR11G11B10ToRGBA16] = D3D12_SHADER_BYTECODE{
      shaders::texture_load_r11g11b10_rgba16_cs, sizeof(shaders::texture_load_r11g11b10_rgba16_cs)};
  load_shader_code[kLoadShaderIndexR11G11B10ToRGBA16SNorm] =
      D3D12_SHADER_BYTECODE{shaders::texture_load_r11g11b10_rgba16_snorm_cs,
                            sizeof(shaders::texture_load_r11g11b10_rgba16_snorm_cs)};
  load_shader_code[kLoadShaderIndexDXT1ToRGBA8] = D3D12_SHADER_BYTECODE{
      shaders::texture_load_dxt1_rgba8_cs, sizeof(shaders::texture_load_dxt1_rgba8_cs)};
  load_shader_code[kLoadShaderIndexDXT3ToRGBA8] = D3D12_SHADER_BYTECODE{
      shaders::texture_load_dxt3_rgba8_cs, sizeof(shaders::texture_load_dxt3_rgba8_cs)};
  load_shader_code[kLoadShaderIndexDXT5ToRGBA8] = D3D12_SHADER_BYTECODE{
      shaders::texture_load_dxt5_rgba8_cs, sizeof(shaders::texture_load_dxt5_rgba8_cs)};
  load_shader_code[kLoadShaderIndexDXNToRG8] = D3D12_SHADER_BYTECODE{
      shaders::texture_load_dxn_rg8_cs, sizeof(shaders::texture_load_dxn_rg8_cs)};
  load_shader_code[kLoadShaderIndexDXT3A] =
      D3D12_SHADER_BYTECODE{shaders::texture_load_dxt3a_cs, sizeof(shaders::texture_load_dxt3a_cs)};
  load_shader_code[kLoadShaderIndexDXT3AAs1111ToBGRA4] =
      D3D12_SHADER_BYTECODE{shaders::texture_load_dxt3aas1111_bgra4_cs,
                            sizeof(shaders::texture_load_dxt3aas1111_bgra4_cs)};
  load_shader_code[kLoadShaderIndexDXT5AToR8] = D3D12_SHADER_BYTECODE{
      shaders::texture_load_dxt5a_r8_cs, sizeof(shaders::texture_load_dxt5a_r8_cs)};
  load_shader_code[kLoadShaderIndexCTX1] =
      D3D12_SHADER_BYTECODE{shaders::texture_load_ctx1_cs, sizeof(shaders::texture_load_ctx1_cs)};
  load_shader_code[kLoadShaderIndexDepthUnorm] = D3D12_SHADER_BYTECODE{
      shaders::texture_load_depth_unorm_cs, sizeof(shaders::texture_load_depth_unorm_cs)};
  load_shader_code[kLoadShaderIndexDepthFloat] = D3D12_SHADER_BYTECODE{
      shaders::texture_load_depth_float_cs, sizeof(shaders::texture_load_depth_float_cs)};
  D3D12_SHADER_BYTECODE load_shader_code_scaled[kLoadShaderCount] = {};
  if (IsDrawResolutionScaled()) {
    load_shader_code_scaled[kLoadShaderIndex8bpb] = D3D12_SHADER_BYTECODE{
        shaders::texture_load_8bpb_scaled_cs, sizeof(shaders::texture_load_8bpb_scaled_cs)};
    load_shader_code_scaled[kLoadShaderIndex16bpb] = D3D12_SHADER_BYTECODE{
        shaders::texture_load_16bpb_scaled_cs, sizeof(shaders::texture_load_16bpb_scaled_cs)};
    load_shader_code_scaled[kLoadShaderIndex32bpb] = D3D12_SHADER_BYTECODE{
        shaders::texture_load_32bpb_scaled_cs, sizeof(shaders::texture_load_32bpb_scaled_cs)};
    load_shader_code_scaled[kLoadShaderIndex64bpb] = D3D12_SHADER_BYTECODE{
        shaders::texture_load_64bpb_scaled_cs, sizeof(shaders::texture_load_64bpb_scaled_cs)};
    load_shader_code_scaled[kLoadShaderIndex128bpb] = D3D12_SHADER_BYTECODE{
        shaders::texture_load_128bpb_scaled_cs, sizeof(shaders::texture_load_128bpb_scaled_cs)};
    load_shader_code_scaled[kLoadShaderIndexR5G5B5A1ToB5G5R5A1] =
        D3D12_SHADER_BYTECODE{shaders::texture_load_r5g5b5a1_b5g5r5a1_scaled_cs,
                              sizeof(shaders::texture_load_r5g5b5a1_b5g5r5a1_scaled_cs)};
    load_shader_code_scaled[kLoadShaderIndexR5G6B5ToB5G6R5] =
        D3D12_SHADER_BYTECODE{shaders::texture_load_r5g6b5_b5g6r5_scaled_cs,
                              sizeof(shaders::texture_load_r5g6b5_b5g6r5_scaled_cs)};
    load_shader_code_scaled[kLoadShaderIndexR5G5B6ToB5G6R5WithRBGASwizzle] =
        D3D12_SHADER_BYTECODE{shaders::texture_load_r5g5b6_b5g6r5_swizzle_rbga_scaled_cs,
                              sizeof(shaders::texture_load_r5g5b6_b5g6r5_swizzle_rbga_scaled_cs)};
    load_shader_code_scaled[kLoadShaderIndexRGBA4ToBGRA4] =
        D3D12_SHADER_BYTECODE{shaders::texture_load_r4g4b4a4_b4g4r4a4_scaled_cs,
                              sizeof(shaders::texture_load_r4g4b4a4_b4g4r4a4_scaled_cs)};
    load_shader_code_scaled[kLoadShaderIndexR10G11B11ToRGBA16] =
        D3D12_SHADER_BYTECODE{shaders::texture_load_r10g11b11_rgba16_scaled_cs,
                              sizeof(shaders::texture_load_r10g11b11_rgba16_scaled_cs)};
    load_shader_code_scaled[kLoadShaderIndexR10G11B11ToRGBA16SNorm] =
        D3D12_SHADER_BYTECODE{shaders::texture_load_r10g11b11_rgba16_snorm_scaled_cs,
                              sizeof(shaders::texture_load_r10g11b11_rgba16_snorm_scaled_cs)};
    load_shader_code_scaled[kLoadShaderIndexR11G11B10ToRGBA16] =
        D3D12_SHADER_BYTECODE{shaders::texture_load_r11g11b10_rgba16_scaled_cs,
                              sizeof(shaders::texture_load_r11g11b10_rgba16_scaled_cs)};
    load_shader_code_scaled[kLoadShaderIndexR11G11B10ToRGBA16SNorm] =
        D3D12_SHADER_BYTECODE{shaders::texture_load_r11g11b10_rgba16_snorm_scaled_cs,
                              sizeof(shaders::texture_load_r11g11b10_rgba16_snorm_scaled_cs)};
    load_shader_code_scaled[kLoadShaderIndexDepthUnorm] =
        D3D12_SHADER_BYTECODE{shaders::texture_load_depth_unorm_scaled_cs,
                              sizeof(shaders::texture_load_depth_unorm_scaled_cs)};
    load_shader_code_scaled[kLoadShaderIndexDepthFloat] =
        D3D12_SHADER_BYTECODE{shaders::texture_load_depth_float_scaled_cs,
                              sizeof(shaders::texture_load_depth_float_scaled_cs)};
  }

  // Create the loading pipelines.
  for (size_t i = 0; i < kLoadShaderCount; ++i) {
    const D3D12_SHADER_BYTECODE& current_load_shader_code = load_shader_code[i];
    if (!current_load_shader_code.pShaderBytecode) {
      continue;
    }
    *(load_pipelines_[i].ReleaseAndGetAddressOf()) = ui::d3d12::util::CreateComputePipeline(
        device, current_load_shader_code.pShaderBytecode, current_load_shader_code.BytecodeLength,
        load_root_signature_.Get());
    if (!load_pipelines_[i]) {
      REXGPU_ERROR(
          "D3D12TextureCache: Failed to create the texture loading pipeline "
          "for shader {}",
          i);
      return false;
    }
    if (IsDrawResolutionScaled()) {
      const D3D12_SHADER_BYTECODE& current_load_shader_code_scaled = load_shader_code_scaled[i];
      if (current_load_shader_code_scaled.pShaderBytecode) {
        *(load_pipelines_scaled_[i].ReleaseAndGetAddressOf()) =
            ui::d3d12::util::CreateComputePipeline(
                device, current_load_shader_code_scaled.pShaderBytecode,
                current_load_shader_code_scaled.BytecodeLength, load_root_signature_.Get());
        if (!load_pipelines_scaled_[i]) {
          REXGPU_ERROR(
              "D3D12TextureCache: Failed to create the resolution-scaled "
              "texture loading pipeline for shader {}",
              i);
          return false;
        }
      }
    }
  }

  srv_descriptor_cache_allocated_ = 0;

  // Create a heap with null SRV descriptors, since it's faster to copy a
  // descriptor than to create an SRV, and null descriptors are used a lot (for
  // the signed version when only unsigned is used, for instance).
  D3D12_DESCRIPTOR_HEAP_DESC null_srv_descriptor_heap_desc;
  null_srv_descriptor_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
  null_srv_descriptor_heap_desc.NumDescriptors = uint32_t(NullSRVDescriptorIndex::kCount);
  null_srv_descriptor_heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
  null_srv_descriptor_heap_desc.NodeMask = 0;
  if (FAILED(device->CreateDescriptorHeap(&null_srv_descriptor_heap_desc,
                                          IID_PPV_ARGS(&null_srv_descriptor_heap_)))) {
    REXGPU_ERROR(
        "D3D12TextureCache: Failed to create the descriptor heap for null "
        "SRVs");
    return false;
  }
  null_srv_descriptor_heap_start_ = null_srv_descriptor_heap_->GetCPUDescriptorHandleForHeapStart();
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
      provider.OffsetViewDescriptor(null_srv_descriptor_heap_start_,
                                    uint32_t(NullSRVDescriptorIndex::k2DArray)));
  null_srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
  null_srv_desc.Texture3D.MostDetailedMip = 0;
  null_srv_desc.Texture3D.MipLevels = 1;
  null_srv_desc.Texture3D.ResourceMinLODClamp = 0.0f;
  device->CreateShaderResourceView(
      nullptr, &null_srv_desc,
      provider.OffsetViewDescriptor(null_srv_descriptor_heap_start_,
                                    uint32_t(NullSRVDescriptorIndex::k3D)));
  null_srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
  null_srv_desc.TextureCube.MostDetailedMip = 0;
  null_srv_desc.TextureCube.MipLevels = 1;
  null_srv_desc.TextureCube.ResourceMinLODClamp = 0.0f;
  device->CreateShaderResourceView(
      nullptr, &null_srv_desc,
      provider.OffsetViewDescriptor(null_srv_descriptor_heap_start_,
                                    uint32_t(NullSRVDescriptorIndex::kCube)));

  return true;
}

void D3D12TextureCache::ClearCache() {
  InvalidateOwnedSceneColor();
  TextureCache::ClearCache();

  // Clear texture descriptor cache.
  srv_descriptor_cache_free_.clear();
  srv_descriptor_cache_allocated_ = 0;
  srv_descriptor_cache_.clear();
}

void D3D12TextureCache::BeginSubmission(uint64_t new_submission_index) {
  TextureCache::BeginSubmission(new_submission_index);

  // ExecuteCommandLists is a full UAV and aliasing barrier.
  if (IsDrawResolutionScaled()) {
    size_t scaled_resolve_buffer_count = GetScaledResolveBufferCount();
    for (size_t i = 0; i < scaled_resolve_buffer_count; ++i) {
      ScaledResolveVirtualBuffer* scaled_resolve_buffer = scaled_resolve_2gb_buffers_[i].get();
      if (scaled_resolve_buffer) {
        scaled_resolve_buffer->ClearUAVBarrierPending();
      }
    }
    std::memset(scaled_resolve_1gb_buffer_indices_, UINT8_MAX,
                sizeof(scaled_resolve_1gb_buffer_indices_));
  }
}

void D3D12TextureCache::BeginFrame() {
  TextureCache::BeginFrame();
  UpdatePromptIcons();
  if (!pack_index_built_ && TextureReplacementEnabled()) {
    EnsurePackIndex();
  }
  if (!textures_awaiting_replacement_.empty() || !deferred_texture_releases_.empty() ||
      replacement_bindings_dirty_ || pack_loads_in_flight_ ||
      pack_preload_next_ < pack_preload_order_.size() || !pack_preload_logged_) {
    if (pack_index_built_ || !deferred_texture_releases_.empty() ||
        replacement_bindings_dirty_) {
      UpdatePackReplacements();
    }
  }
  CompleteOwnedSceneColorVerification();

  std::memset(unsupported_format_features_used_, 0, sizeof(unsupported_format_features_used_));
}

void D3D12TextureCache::EndFrame() {
  FinishOwnedSceneColor();
  if (pack_hash_textures_frame_) {
    LARGE_INTEGER now, frequency;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&frequency);
    const double hash_us = double(pack_hash_ticks_frame_) * 1e6 / double(frequency.QuadPart);
    if (hash_us >= 500.0) {
      std::fprintf(stderr, "REX_TEXTURE_PACK_HASH qpc=%lld textures=%u kb=%llu us=%.0f\n",
                   static_cast<long long>(now.QuadPart), pack_hash_textures_frame_,
                   static_cast<unsigned long long>(pack_hash_bytes_frame_ >> 10), hash_us);
    }
    pack_hash_textures_frame_ = 0;
    pack_hash_bytes_frame_ = 0;
    pack_hash_ticks_frame_ = 0;
  }
  if (!pending_texture_dumps_.empty()) {
    ProcessTextureDumps();
  }
  if (!pending_overlays_.empty()) {
    ProcessOverlayReadbacks();
  }
  // Report used unsupported texture formats.
  bool unsupported_header_written = false;
  for (uint32_t i = 0; i < 64; ++i) {
    uint32_t unsupported_features = unsupported_format_features_used_[i];
    if (unsupported_features == 0) {
      continue;
    }
    if (!unsupported_header_written) {
      REXGPU_ERROR("Unsupported texture formats used in the frame:");
      unsupported_header_written = true;
    }
    REXGPU_ERROR("* {}{}{}{}", FormatInfo::Get(xenos::TextureFormat(i))->name,
                 unsupported_features & kUnsupportedResourceBit ? " resource" : "",
                 unsupported_features & kUnsupportedUnormBit ? " unsigned" : "",
                 unsupported_features & kUnsupportedSnormBit ? " signed" : "");
    unsupported_format_features_used_[i] = 0;
  }
}

void D3D12TextureCache::RequestTextures(uint32_t used_texture_mask) {
#if XE_GPU_FINE_GRAINED_DRAW_SCOPES
  SCOPE_profile_cpu_f("gpu");
#endif  // XE_GPU_FINE_GRAINED_DRAW_SCOPES

  TextureCache::RequestTextures(used_texture_mask);

  // Pre-create 3D-as-2D wrappers before draw setup. Wrapper loading may bind
  // compute pipelines and must happen in the texture request phase.
  if (REXCVAR_GET(gpu_3d_to_2d_texture)) {
    uint32_t textures_3d = used_texture_mask;
    uint32_t index_3d;
    while (rex::bit_scan_forward(textures_3d, &index_3d)) {
      textures_3d &= ~(uint32_t(1) << index_3d);
      const TextureBinding* binding = GetValidTextureBinding(index_3d);
      if (!binding || binding->key.dimension != xenos::DataDimension::k3D) {
        continue;
      }
      D3D12Texture* texture = static_cast<D3D12Texture*>(binding->texture);
      if (texture) {
        texture->GetOrCreate3DAs2DResource(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
                                           D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
      }
      D3D12Texture* texture_signed = static_cast<D3D12Texture*>(binding->texture_signed);
      if (texture_signed) {
        texture_signed->GetOrCreate3DAs2DResource(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
                                                  D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
      }
    }
  }

  // Transition the textures to the needed usage - always in
  // NON_PIXEL_SHADER_RESOURCE | PIXEL_SHADER_RESOURCE states because barriers
  // between read-only stages, if needed, are discouraged (also if these were
  // tracked separately, checks would be needed to make sure, if the same
  // texture is bound through different fetch constants to both VS and PS, it
  // would be in both states).
  uint32_t textures_remaining = used_texture_mask;
  uint32_t index;
  while (rex::bit_scan_forward(textures_remaining, &index)) {
    textures_remaining &= ~(uint32_t(1) << index);
    const TextureBinding* binding = GetValidTextureBinding(index);
    if (!binding) {
      continue;
    }
    D3D12Texture* binding_texture = static_cast<D3D12Texture*>(binding->texture);
    if (binding_texture != nullptr) {
      // Will be referenced by the command list, so mark as used.
      binding_texture->MarkAsUsed();
      command_processor_.PushTransitionBarrier(
          binding_texture->resource(),
          binding_texture->SetResourceState(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
                                            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
          D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
              D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    }
    D3D12Texture* binding_texture_signed = static_cast<D3D12Texture*>(binding->texture_signed);
    if (binding_texture_signed != nullptr) {
      binding_texture_signed->MarkAsUsed();
      command_processor_.PushTransitionBarrier(
          binding_texture_signed->resource(),
          binding_texture_signed->SetResourceState(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
                                                   D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
          D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
              D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    }
  }

  textures_remaining = used_texture_mask;
  while (rex::bit_scan_forward(textures_remaining, &index)) {
    textures_remaining &= ~(uint32_t(1) << index);
    const TextureBinding* binding = GetValidTextureBinding(index);
    if (!kPromptTextureDiagnosticsEnabled || !binding ||
        binding->key.format != xenos::TextureFormat::k_DXT1 ||
        !binding->key.tiled || binding->key.dimension != xenos::DataDimension::k2DOrStacked ||
        binding->key.GetWidth() != 512 || binding->key.GetHeight() != 191) {
      continue;
    }
    D3D12Texture* texture = static_cast<D3D12Texture*>(binding->texture);
    const D3D12TextureBinding& d3d12_binding = d3d12_texture_bindings_[index];
    const uint64_t descriptor_and_state =
        (uint64_t(d3d12_binding.descriptor_index) << 32) |
        uint64_t(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
                 D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    shared_memory().RecordTextureLifecycleDiagnosticEvent(
        SharedMemory::TextureLifecycleDiagnosticEventType::kDrawBinding,
        binding->key.base_page << 12,
        texture ? texture->GetGuestBaseSize()
                : binding->key.GetGuestLayout().base.level_data_extent_bytes,
        texture ? uint64_t(reinterpret_cast<uintptr_t>(texture->resource())) : 0,
        descriptor_and_state, true);
    shared_memory().DumpTextureLifecycleDiagnosticEvents();
  }
}

bool D3D12TextureCache::AreActiveTextureSRVKeysUpToDate(
    const TextureSRVKey* keys, const D3D12Shader::TextureBinding* host_shader_bindings,
    size_t host_shader_binding_count) const {
  for (size_t i = 0; i < host_shader_binding_count; ++i) {
    const TextureSRVKey& key = keys[i];
    const TextureBinding* binding = GetValidTextureBinding(host_shader_bindings[i].fetch_constant);
    if (!binding) {
      if (key.key.is_valid) {
        return false;
      }
      continue;
    }
    if (key.key != binding->key || key.host_swizzle != binding->host_swizzle ||
        key.swizzled_signs != binding->swizzled_signs) {
      return false;
    }
  }
  return true;
}

bool D3D12TextureCache::GetActiveTextureDiagnostic(
    uint32_t fetch_constant_index,
    ActiveTextureDiagnostic& diagnostic_out) const {
  diagnostic_out = ActiveTextureDiagnostic{};
  if (fetch_constant_index >= d3d12_texture_bindings_.size()) {
    return false;
  }
  const TextureBinding* binding =
      GetValidTextureBinding(fetch_constant_index);
  if (!binding) {
    return false;
  }
  const Texture* texture = binding->texture ? binding->texture
                                            : binding->texture_signed;
  if (!texture) {
    return false;
  }
  const TextureKey& key = texture->key();
  const D3D12Texture* d3d12_texture =
      static_cast<const D3D12Texture*>(texture);
  const D3D12_RESOURCE_DESC resource_desc =
      d3d12_texture->resource()->GetDesc();
  const D3D12TextureBinding& d3d12_binding =
      d3d12_texture_bindings_[fetch_constant_index];
  diagnostic_out.guest_base = key.base_page << 12;
  diagnostic_out.guest_size = texture->GetGuestBaseSize();
  diagnostic_out.guest_width = key.GetWidth();
  diagnostic_out.guest_height = key.GetHeight();
  diagnostic_out.guest_depth_or_array_size = key.GetDepthOrArraySize();
  diagnostic_out.guest_format = uint32_t(key.format);
  diagnostic_out.guest_dimension = uint32_t(key.dimension);
  diagnostic_out.guest_tiled = key.tiled;
  diagnostic_out.scaled_resolve = key.scaled_resolve;
  diagnostic_out.outdated_mask = texture->outdated_mask();
  diagnostic_out.descriptor_index = d3d12_binding.descriptor_index;
  diagnostic_out.descriptor_index_signed =
      d3d12_binding.descriptor_index_signed;
  diagnostic_out.resource_identity =
      uint64_t(reinterpret_cast<uintptr_t>(d3d12_texture->resource()));
  diagnostic_out.resource_width = resource_desc.Width;
  diagnostic_out.resource_height = resource_desc.Height;
  diagnostic_out.resource_depth_or_array_size =
      resource_desc.DepthOrArraySize;
  diagnostic_out.resource_mip_levels = resource_desc.MipLevels;
  diagnostic_out.resource_format = uint32_t(resource_desc.Format);
  return true;
}

bool D3D12TextureCache::OwnsSceneHistory() const {
  // The owned scene-color history is an experiment: measurement builds only.
  return kGpuDiagnostics && REXCVAR_GET(embedded_pc_scene_history);
}

void D3D12TextureCache::RecordOwnedDrawTransform(
    const pc_draw_transform_history::Draw& draw, bool valid) {
  if (OwnsSceneHistory()) pending_owned_draws_.Add(command_processor_.GetCurrentFrame(), draw, valid);
}

void D3D12TextureCache::InvalidateOwnedSceneColor() {
  // Keep allocations and fence ownership even when semantic history is lost.
  owned_scene_color_state_.Invalidate();
  // A cache reset partway through a frame cannot certify a partial draw set.
  pending_owned_draws_.failed = true;
}

bool D3D12TextureCache::GetTemporalAaBoundTexture(uint32_t fetch_constant_index,
                                                  TemporalAaTexture& out) const {
  out = {};
  if (fetch_constant_index >= d3d12_texture_bindings_.size()) return false;
  const TextureBinding* binding = GetValidTextureBinding(fetch_constant_index);
  if (!binding) return false;
  Texture* texture = binding->texture ? binding->texture : binding->texture_signed;
  if (!texture) return false;
  auto* d3d12_texture = static_cast<D3D12Texture*>(texture);
  const D3D12_RESOURCE_DESC desc = d3d12_texture->resource()->GetDesc();
  out.handle = d3d12_texture;
  out.resource = d3d12_texture->resource();
  out.format = desc.Format;
  out.width = uint32_t(desc.Width);
  out.height = desc.Height;
  out.guest_base = texture->key().base_page << 12;
  out.guest_format = uint32_t(texture->key().format);
  return true;
}

bool D3D12TextureCache::RequestTemporalAaDepth(uint32_t guest_base, uint32_t width,
                                               uint32_t height, xenos::Endian endian,
                                               TemporalAaTexture& out) {
  out = {};
  if (!guest_base || !width || !height || width > 8192 || height > 8192) return false;
  TextureKey key;
  key.base_page = guest_base >> 12;
  key.dimension = xenos::DataDimension::k2DOrStacked;
  key.width_minus_1 = width - 1;
  key.height_minus_1 = height - 1;
  key.tiled = 1;
  key.pitch = (width + 31) >> 5;
  key.format = xenos::TextureFormat::k_24_8_FLOAT;
  key.endianness = endian;
  key.is_valid = 1;
  Texture* texture = FindOrCreateTexture(key);
  if (!texture || !LoadTextureData(*texture)) return false;
  texture->MarkAsUsed();
  auto* d3d12_texture = static_cast<D3D12Texture*>(texture);
  constexpr D3D12_RESOURCE_STATES kShaderResourceStates =
      D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
  command_processor_.PushTransitionBarrier(d3d12_texture->resource(),
                                           d3d12_texture->SetResourceState(kShaderResourceStates),
                                           kShaderResourceStates);
  const D3D12_RESOURCE_DESC desc = d3d12_texture->resource()->GetDesc();
  out.handle = d3d12_texture;
  out.resource = d3d12_texture->resource();
  out.format = desc.Format;
  out.width = uint32_t(desc.Width);
  out.height = desc.Height;
  out.guest_base = guest_base;
  out.guest_format = uint32_t(key.format);
  return true;
}

void D3D12TextureCache::TransitionTemporalAaTexture(void* handle, D3D12_RESOURCE_STATES state) {
  auto* d3d12_texture = static_cast<D3D12Texture*>(handle);
  if (!d3d12_texture) return;
  command_processor_.PushTransitionBarrier(d3d12_texture->resource(),
                                           d3d12_texture->SetResourceState(state), state);
}

D3D12TextureCache::OwnedSceneColorPair D3D12TextureCache::UseOwnedSceneColor() {
  OwnedSceneColorPair pair;
  auto& state = owned_scene_color_state_;
  if (!OwnsSceneHistory() ||
      !state.Use(command_processor_.GetCurrentSubmission())) return pair;
  pair.current = owned_scene_color_[state.current].Get();
  pair.current_frame = state.slots[state.current].frame;
  if (state.previous >= 0) {
    pair.previous = owned_scene_color_[state.previous].Get();
    pair.previous_frame = state.slots[state.previous].frame;
  }
  const auto desc = pair.current->GetDesc();
  pair.epoch = state.epoch;
  pair.width = uint32_t(desc.Width);
  pair.height = desc.Height;
  pair.format = desc.Format;
  pair.host_swizzle = owned_scene_color_swizzle_;
  pair.swizzled_signs = owned_scene_color_signs_;
  pair.sample_exponent = owned_scene_color_exponent_;
  if (owned_draws_[state.current].Valid(pair.current_frame))
    pair.current_draws = &owned_draws_[state.current];
  if (state.previous >= 0 && owned_draws_[state.previous].Valid(pair.previous_frame))
    pair.previous_draws = &owned_draws_[state.previous];
  pair.projection_unchanged = pair.current_draws && pair.previous_draws &&
      pair.current_draws->SameProjection(*pair.previous_draws);
  return pair;
}

void D3D12TextureCache::FinishOwnedSceneColor() {
  if (!OwnsSceneHistory()) return;
  bool proof_frame = false;
  for (const auto& proof : owned_color_verifications_)
    proof_frame |= proof.frame == command_processor_.GetCurrentFrame();
  const uint64_t frame = command_processor_.GetCurrentFrame();
  const bool committed = owned_scene_color_state_.Finish(frame);
  if (committed) {
    auto& owned = owned_draws_[owned_scene_color_state_.current];
    owned.Reset();
    if (pending_owned_draws_.Valid(frame)) owned = pending_owned_draws_;
  }
  pending_owned_draws_.Reset();
  // Manual image proofs and CPU publication windows need not cover the same
  // submissions. Also report the first two committed frames with a qualified
  // camera input, without changing frame retention or image-proof scheduling.
  bool camera_input_frame = false;
  if (committed && owned_camera_frames_reported_ < 2) {
    const auto& owned = owned_draws_[owned_scene_color_state_.current];
    if (owned.Valid(frame)) {
      for (uint32_t i = 0; i < owned.count && !camera_input_frame; ++i) {
        pc_owned_camera_packet::Source source;
        camera_input_frame = owned.draws[i].CopyOwnedCamera(source);
      }
    }
    if (camera_input_frame) ++owned_camera_frames_reported_;
  }
  const bool followup_frame = owned_camera_followups_.Select(frame, proof_frame, committed);
  if (committed && (owned_scene_color_reported_ < 8 || proof_frame || camera_input_frame || followup_frame)) {
    const auto pair = UseOwnedSceneColor();
    ++owned_scene_color_reported_;
    std::fprintf(stderr,
        "REX_PC_SCENE_COLOR frame=%llu previous=%llu epoch=%llu width=%u height=%u format=%u "
        "swizzle=%u signs=%u exponent=%d current_resource=%016llX previous_resource=%016llX "
        "state=shader_resource scope=owned_color_only temporal_valid=0\n",
        static_cast<unsigned long long>(pair.current_frame),
        static_cast<unsigned long long>(pair.previous_frame),
        static_cast<unsigned long long>(pair.epoch), pair.width, pair.height,
        uint32_t(pair.format), pair.host_swizzle, uint32_t(pair.swizzled_signs), pair.sample_exponent,
        static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(pair.current)),
        static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(pair.previous)));
    std::fprintf(stderr,
        "REX_PC_DRAW_TRANSFORMS frame=%llu previous=%llu current_count=%u previous_count=%u "
        "scope=submitted_current_inputs instance_identity=0 reprojection_valid=0\n",
        static_cast<unsigned long long>(pair.current_frame),
        static_cast<unsigned long long>(pair.previous_frame),
        pair.current_draws ? pair.current_draws->count : 0,
        pair.previous_draws ? pair.previous_draws->count : 0);
    if (proof_frame) {
      const auto* current = pair.current_draws;
      const auto* previous = pair.previous_draws;
      const bool valid = current && current->projection.valid;
      std::fprintf(stderr,
          "REX_PC_PROJECTION frame=%llu previous=%llu valid=%u previous_valid=%u unchanged=%u raw=",
          static_cast<unsigned long long>(pair.current_frame),
          static_cast<unsigned long long>(pair.previous_frame), valid,
          previous && previous->projection.valid, pair.projection_unchanged);
      for (uint32_t i = 0; i < 16; ++i)
        std::fprintf(stderr, "%s%08X", i ? "," : "", valid ? current->projection.words[i] : 0);
      std::fprintf(stderr, " scope=unique_sparse_projection camera_pose=0 temporal_valid=0\n");
    }
    if ((proof_frame || camera_input_frame || followup_frame) && pair.current_draws) {
      std::fprintf(stderr, "REX_PC_DRAW_SAMPLE frame=%llu image_proof=%u owned_camera_trigger=%u followup=%u\n",
          static_cast<unsigned long long>(pair.current_frame), proof_frame, camera_input_frame, followup_frame);
      for (uint32_t i = 0; i < pair.current_draws->count; ++i) {
        const auto& draw = pair.current_draws->draws[i];
        std::fprintf(stderr, "REX_PC_DRAW_INPUT frame=%llu index=%u vs=%016llX raw=",
            static_cast<unsigned long long>(pair.current_frame), i,
            static_cast<unsigned long long>(draw.vertex_shader));
        for (uint32_t j = 0; j < draw.constants.size(); ++j)
          std::fprintf(stderr, "%s%08X", j ? "," : "", draw.constants[j]);
        std::fprintf(stderr, " viewport=");
        for (uint32_t j = 0; j < draw.viewport.size(); ++j)
          std::fprintf(stderr, "%s%08X", j ? "," : "", draw.viewport[j]);
        std::fprintf(stderr, " window=%08X scissor=%08X,%08X vte=%08X clip=%08X surface=%08X depth=%08X\n",
            draw.window_offset, draw.scissor_tl, draw.scissor_br, draw.vte,
            draw.clip, draw.surface, draw.depth);
        std::fprintf(stderr, "REX_PC_DRAW_WRITERS frame=%llu index=%u records=",
            static_cast<unsigned long long>(pair.current_frame), i);
        for (uint32_t j = 0; j < draw.writers.size(); ++j) {
          const auto& writer = draw.writers[j];
          std::fprintf(stderr, "%s%llu:%08X:%08X:%08X:%u", j ? "," : "",
              static_cast<unsigned long long>(writer.sequence), writer.packet_physical,
              writer.physical_address, writer.value, writer.bulk);
        }
        std::fprintf(stderr, " scope=last_observed_register_write camera_identity=0 allocation_lifetime=0\n");
        std::fprintf(stderr, "REX_PC_DRAW_EXECUTIONS frame=%llu index=%u records=",
            static_cast<unsigned long long>(pair.current_frame), i);
        for (uint32_t j = 0; j < draw.writers.size(); ++j) {
          const auto& token = draw.writers[j].execution;
          std::fprintf(stderr, "%s%llu:%llu:%llu:%llu:%u", j ? "," : "",
              static_cast<unsigned long long>(token.buffer),
              static_cast<unsigned long long>(token.parent),
              static_cast<unsigned long long>(token.packet),
              static_cast<unsigned long long>(token.parent_packet), token.depth);
        }
        std::fprintf(stderr, " scope=actual_cp_execution cpu_publication_identity=0 camera_identity=0\n");
        std::fprintf(stderr, "REX_PC_DRAW_ROOT_PUBLICATIONS frame=%llu index=%u records=",
            static_cast<unsigned long long>(pair.current_frame), i);
        for (uint32_t j = 0; j < draw.writers.size(); ++j) {
          std::fprintf(stderr, "%s%llu", j ? "," : "",
              static_cast<unsigned long long>(draw.writers[j].execution.root_publication));
        }
        std::fprintf(stderr, " scope=root_ring_publication child_allocation_identity=0 camera_identity=0\n");
        pc_owned_camera_packet::Source camera;
        const bool owned_camera = draw.CopyOwnedCamera(camera);
        std::fprintf(stderr,
            "REX_PC_DRAW_OWNED_CAMERA frame=%llu index=%u valid=%u packet=%llu constant=%llu source=%llu item=%u camera=",
            static_cast<unsigned long long>(pair.current_frame), i, owned_camera,
            static_cast<unsigned long long>(camera.packet),
            static_cast<unsigned long long>(camera.constant),
            static_cast<unsigned long long>(camera.publication), camera.item);
        for (uint32_t j = 0; j < 16; ++j)
          std::fprintf(stderr, "%s%08X", j ? "," : "", camera.camera_current[j]);
        std::fprintf(stderr, " scope=owned_cpu_camera_at_actual_draw persistent_identity=0 reprojection_validity=0\n");
      }
    }
  }
}

void D3D12TextureCache::CopyOwnedSceneColor(uint64_t pixel_shader, bool verify_copy) {
  if (!OwnsSceneHistory() ||
      pixel_shader != UINT64_C(0xA59B41D0BD79484B)) return;
  auto& state = owned_scene_color_state_;
  const uint64_t frame = command_processor_.GetCurrentFrame();
  if (state.pending_frame == frame) { state.Fail(frame); return; }
  pending_owned_draws_.Seal(frame);
  const TextureBinding* binding = GetValidTextureBinding(0);
  if (!binding || !binding->texture) { state.Fail(frame); return; }
  auto* texture = static_cast<D3D12Texture*>(binding->texture);
  const auto& key = texture->key();
  const auto desc = texture->resource()->GetDesc();
  const auto fetch = register_file().GetTextureFetch(0);
  // Initial reviewed title path only. Unsupported grids/formats fail closed.
  if (!pc_scene_color_history::SupportedSource(uint32_t(key.format), uint32_t(desc.Format),
          binding->swizzled_signs, fetch.num_format != 0, key.GetWidth(), key.GetHeight()) ||
      desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || desc.Width != 1280 ||
      desc.Height != 720 || desc.DepthOrArraySize != 1 || desc.SampleDesc.Count != 1 ||
      draw_resolution_scale_x() != 1 || draw_resolution_scale_y() != 1) {
    state.Fail(frame);
    return;
  }
  if (state.current >= 0 && (owned_scene_color_swizzle_ != binding->host_swizzle ||
      owned_scene_color_signs_ != binding->swizzled_signs ||
      owned_scene_color_exponent_ != fetch.exp_adjust)) state.Invalidate();
  const int slot = state.Reserve(frame, command_processor_.GetCompletedSubmission());
  if (slot < 0) return;
  auto* device = command_processor_.GetD3D12Provider().GetDevice();
  auto snapshot_desc = desc;
  // Same format group: exact bits, with the actual unsigned sampling view.
  snapshot_desc.Format = DXGI_FORMAT_R16G16B16A16_UNORM;
  snapshot_desc.MipLevels = 1;
  snapshot_desc.Flags = D3D12_RESOURCE_FLAG_NONE;
  auto& snapshot = owned_scene_color_[slot];
  constexpr auto kRead = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
                         D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
  if (!snapshot) {
    const auto allocation = device->GetResourceAllocationInfo(0, 1, &snapshot_desc);
    if (allocation.SizeInBytes > 8u * 1024u * 1024u ||
        FAILED(device->CreateCommittedResource(&ui::d3d12::util::kHeapPropertiesDefault,
            command_processor_.GetD3D12Provider().GetHeapFlagCreateNotZeroed(),
            &snapshot_desc, kRead, nullptr, IID_PPV_ARGS(&snapshot)))) {
      state.Fail(frame);
      return;
    }
  }
  texture->MarkAsUsed();
  command_processor_.PushTransitionBarrier(texture->resource(),
      texture->SetResourceState(D3D12_RESOURCE_STATE_COPY_SOURCE), D3D12_RESOURCE_STATE_COPY_SOURCE);
  command_processor_.PushTransitionBarrier(snapshot.Get(), kRead, D3D12_RESOURCE_STATE_COPY_DEST);
  command_processor_.SubmitBarriers();
  auto& commands = command_processor_.GetDeferredCommandList();
  D3D12_TEXTURE_COPY_LOCATION source{}, destination{};
  source.pResource = texture->resource();
  source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  destination.pResource = snapshot.Get();
  destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  commands.D3DCopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
  state.Copied(command_processor_.GetCurrentSubmission());
  owned_scene_color_swizzle_ = binding->host_swizzle;
  owned_scene_color_signs_ = binding->swizzled_signs;
  owned_scene_color_exponent_ = fetch.exp_adjust;

  // Two bounded proof frames, only after a manual capture request. Normal
  // operation never reads back, waits or splits the command submission.
  if ((verify_copy || owned_color_verification_count_ == 1) &&
      owned_color_verification_count_ < owned_color_verifications_.size()) {
    auto& proof = owned_color_verifications_[owned_color_verification_count_++];
    proof.frame = frame;
    proof.submission = command_processor_.GetCurrentSubmission();
    std::fprintf(stderr,
        "REX_PC_SCENE_COLOR_COPY frame=%llu submission=%llu source=%016llX owned=%016llX "
        "guest_base=%08X guest_format=%u shader=A59B41D0BD79484B slot=0\n",
        static_cast<unsigned long long>(frame), static_cast<unsigned long long>(proof.submission),
        static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(source.pResource)),
        static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(destination.pResource)),
        key.base_page << 12, uint32_t(key.format));
    uint64_t row_bytes = 0;
    device->GetCopyableFootprints(&snapshot_desc, 0, 1, 0, &proof.footprint,
        &proof.rows, &row_bytes, &proof.bytes);
    proof.row_bytes = uint32_t(row_bytes);
    D3D12_RESOURCE_DESC buffer_desc;
    ui::d3d12::util::FillBufferResourceDesc(buffer_desc, proof.bytes, D3D12_RESOURCE_FLAG_NONE);
    bool allocated = proof.bytes <= 8u * 1024u * 1024u;
    for (auto& readback : proof.readback) {
      if (!allocated || FAILED(device->CreateCommittedResource(
          &ui::d3d12::util::kHeapPropertiesReadback,
          command_processor_.GetD3D12Provider().GetHeapFlagCreateNotZeroed(), &buffer_desc,
          D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback)))) allocated = false;
    }
    if (allocated) {
      command_processor_.PushTransitionBarrier(snapshot.Get(),
          D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
      command_processor_.SubmitBarriers();
      for (uint32_t i = 0; i < 2; ++i) {
        D3D12_TEXTURE_COPY_LOCATION target{};
        target.pResource = proof.readback[i].Get();
        target.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        target.PlacedFootprint = proof.footprint;
        commands.D3DCopyTextureRegion(&target, 0, 0, 0, i ? &destination : &source, nullptr);
      }
      command_processor_.PushTransitionBarrier(snapshot.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, kRead);
    } else {
      proof.finished = true;
      std::fprintf(stderr, "REX_PC_SCENE_COLOR_VERIFY frame=%llu allocated=0\n",
          static_cast<unsigned long long>(frame));
      command_processor_.PushTransitionBarrier(snapshot.Get(), D3D12_RESOURCE_STATE_COPY_DEST, kRead);
    }
  } else command_processor_.PushTransitionBarrier(snapshot.Get(), D3D12_RESOURCE_STATE_COPY_DEST, kRead);
  command_processor_.PushTransitionBarrier(texture->resource(), texture->SetResourceState(kRead), kRead);
  command_processor_.SubmitBarriers();
}

void D3D12TextureCache::CompleteOwnedSceneColorVerification() {
  for (auto& proof : owned_color_verifications_) {
    if (!proof.submission || proof.finished ||
        command_processor_.GetCompletedSubmission() < proof.submission) continue;
    proof.finished = true;
    std::array<void*, 2> mapped{};
    const size_t bytes = size_t(proof.footprint.Footprint.RowPitch) * (proof.rows - 1) + proof.row_bytes;
    const D3D12_RANGE range{0, bytes}, no_write{0, 0};
    bool equal = true, written = true;
    for (uint32_t i = 0; i < 2; ++i) {
      if (FAILED(proof.readback[i]->Map(0, &range, &mapped[i]))) equal = false;
    }
    if (mapped[0] && mapped[1]) {
      char path[128];
      std::snprintf(path, sizeof(path), "rex_pc_scene_color_frame_%llu.rgba16unorm",
          static_cast<unsigned long long>(proof.frame));
      FILE* file = std::fopen(path, "wb");
      char source_path[128];
      std::snprintf(source_path, sizeof(source_path), "rex_pc_scene_color_frame_%llu_source.rgba16unorm",
          static_cast<unsigned long long>(proof.frame));
      FILE* source_file = std::fopen(source_path, "wb");
      written = file != nullptr && source_file != nullptr;
      for (uint32_t y = 0; y < proof.rows; ++y) {
        const size_t offset = size_t(y) * proof.footprint.Footprint.RowPitch;
        const auto* owned = static_cast<const uint8_t*>(mapped[1]) + offset;
        equal &= std::memcmp(static_cast<const uint8_t*>(mapped[0]) + offset, owned, proof.row_bytes) == 0;
        if (file && std::fwrite(owned, 1, proof.row_bytes, file) != proof.row_bytes) written = false;
        if (source_file && std::fwrite(static_cast<const uint8_t*>(mapped[0]) + offset,
            1, proof.row_bytes, source_file) != proof.row_bytes) written = false;
      }
      if (file && std::fclose(file)) written = false;
      if (source_file && std::fclose(source_file)) written = false;
    } else written = false;
    std::fprintf(stderr,
        "REX_PC_SCENE_COLOR_VERIFY frame=%llu submission=%llu completed=%llu rows=%u row_bytes=%u "
        "equal=%u written=%u scope=source_vs_owned_exact_bytes\n",
        static_cast<unsigned long long>(proof.frame), static_cast<unsigned long long>(proof.submission),
        static_cast<unsigned long long>(command_processor_.GetCompletedSubmission()),
        proof.rows, proof.row_bytes, equal, written);
    for (uint32_t i = 0; i < 2; ++i) {
      if (mapped[i]) proof.readback[i]->Unmap(0, &no_write);
      proof.readback[i].Reset();
    }
  }
}

void D3D12TextureCache::CaptureFirstTemporalDepthBinding(
    uint32_t used_texture_mask, uint64_t context_draw_ordinal) {
  if (!REXCVAR_GET(embedded_temporal_depth_first_binding) ||
      !REXCVAR_GET(embedded_temporal_depth_snapshot) ||
      temporal_first_binding_attempted_ || !context_draw_ordinal) return;
  for (uint32_t slot = 0; slot < 32; ++slot) {
    if (!(used_texture_mask & (uint32_t(1) << slot))) continue;
    const TextureBinding* binding = GetValidTextureBinding(slot);
    if (!binding || !binding->texture) continue;
    const auto& key = binding->texture->key();
    auto* texture = static_cast<D3D12Texture*>(binding->texture);
    const auto desc = texture->resource()->GetDesc();
    if (key.format != xenos::TextureFormat::k_24_8_FLOAT ||
        desc.Format != DXGI_FORMAT_R32_FLOAT ||
        desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
        desc.DepthOrArraySize != 1 || desc.SampleDesc.Count != 1) continue;
    // One attempt, including allocation failure. Never turn a failed capture
    // into an unbounded retry loop or infer scene ownership from the format.
    temporal_first_binding_attempted_ = true;
    const auto before = active_texture_readback_diagnostics_.size();
    CaptureActiveTextureReadbackDiagnostics(&slot, 1, false,
                                           context_draw_ordinal, true);
    std::fprintf(stderr,
        "REX_TEMPORAL_FIRST_BINDING draw=%llu slot=%u queued=%u scope=pre_draw_depth_binding_not_scene_identity\n",
        static_cast<unsigned long long>(context_draw_ordinal), slot,
        active_texture_readback_diagnostics_.size() > before ? 1u : 0u);
    return;
  }
}

void D3D12TextureCache::CaptureActiveTextureReadbackDiagnostics(
    const uint32_t* fetch_constant_indices, size_t fetch_constant_count,
    bool scaled_resolve_only, uint64_t context_draw_ordinal,
    bool distinguish_draw_epoch) {
  if (!fetch_constant_indices || !fetch_constant_count) {
    return;
  }
  const size_t queued_before = active_texture_readback_diagnostics_.size();
  ID3D12Device* device = command_processor_.GetD3D12Provider().GetDevice();
  DeferredCommandList& command_list = command_processor_.GetDeferredCommandList();
  constexpr D3D12_RESOURCE_STATES kShaderResourceStates =
      D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
      D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
  for (size_t i = 0; i < fetch_constant_count; ++i) {
    const uint32_t fetch_constant_index = fetch_constant_indices[i];
    const TextureBinding* binding =
        GetValidTextureBinding(fetch_constant_index);
    Texture* texture = binding ? (binding->texture ? binding->texture
                                                   : binding->texture_signed)
                               : nullptr;
    if (!texture) {
      continue;
    }
    const TextureKey& key = texture->key();
    if (scaled_resolve_only && !key.scaled_resolve) {
      continue;
    }
    D3D12Texture* d3d12_texture = static_cast<D3D12Texture*>(texture);
    ID3D12Resource* texture_resource = d3d12_texture->resource();
    const uint64_t resource_identity =
        uint64_t(reinterpret_cast<uintptr_t>(texture_resource));
    const D3D12_RESOURCE_DESC texture_desc = texture_resource->GetDesc();
    const uint32_t base_array_size =
        texture_desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D
            ? 1u
            : uint32_t(texture_desc.DepthOrArraySize);
    // This diagnostic targets ordinary title resources and six-face cube maps.
    // Keep malformed or unexpectedly large arrays from turning one bounded
    // capture into an unbounded readback operation.
    if (!base_array_size || base_array_size > 16) {
      continue;
    }
    bool resource_transitioned = false;
    for (uint32_t array_slice = 0; array_slice < base_array_size;
         ++array_slice) {
      const uint32_t subresource_index =
          array_slice * uint32_t(texture_desc.MipLevels);
      D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
      UINT row_count = 0;
      UINT64 row_bytes = 0;
      UINT64 total_bytes = 0;
      device->GetCopyableFootprints(&texture_desc, subresource_index, 1, 0,
                                    &footprint, &row_count, &row_bytes,
                                    &total_bytes);
      if (!row_count || row_bytes > UINT32_MAX || total_bytes > UINT32_MAX) {
        continue;
      }
      const bool capture_depth_snapshot =
          REXCVAR_GET(embedded_temporal_depth_snapshot) && context_draw_ordinal &&
          key.format == xenos::TextureFormat::k_24_8_FLOAT &&
          texture_desc.Format == DXGI_FORMAT_R32_FLOAT &&
          texture_desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
          base_array_size == 1 && texture_desc.SampleDesc.Count == 1;
      auto snapshot_desc = texture_desc;
      snapshot_desc.MipLevels = 1;
      snapshot_desc.Flags = D3D12_RESOURCE_FLAG_NONE;
      uint64_t reservation_bytes = total_bytes;
      if (capture_depth_snapshot) {
        const auto allocation = device->GetResourceAllocationInfo(0, 1, &snapshot_desc);
        if (allocation.SizeInBytes == UINT64_MAX ||
            allocation.SizeInBytes > embedded_texture_readback_policy::kMaximumBytes ||
            total_bytes > embedded_texture_readback_policy::kMaximumBytes - allocation.SizeInBytes) {
          continue;
        }
        reservation_bytes += allocation.SizeInBytes;
      }
      const embedded_texture_readback_policy::ReserveResult reserve_result =
          embedded_texture_readback_policy::TryReserve(
              active_texture_readback_diagnostic_policy_, resource_identity,
              array_slice, reservation_bytes,
              distinguish_draw_epoch ? context_draw_ordinal : 0);
      if (reserve_result !=
          embedded_texture_readback_policy::ReserveResult::kAccepted) {
        if (!active_texture_readback_limit_reported_ &&
            (reserve_result == embedded_texture_readback_policy::ReserveResult::
                                   kSubresourceLimit ||
             reserve_result == embedded_texture_readback_policy::ReserveResult::
                                   kByteLimit)) {
          active_texture_readback_limit_reported_ = true;
          std::fprintf(
              stderr,
              "REX_EMBEDDED_ACTIVE_TEXTURE_READBACK_LIMIT reason=%s "
              "captured=%llu bytes=%llu max_captured=%llu max_bytes=%llu\n",
              reserve_result ==
                      embedded_texture_readback_policy::ReserveResult::
                          kSubresourceLimit
                  ? "subresources"
                  : "bytes",
              static_cast<unsigned long long>(
                  active_texture_readback_diagnostic_policy_.key_count),
              static_cast<unsigned long long>(
                  active_texture_readback_diagnostic_policy_.reserved_bytes),
              static_cast<unsigned long long>(
                  embedded_texture_readback_policy::kMaximumSubresources),
              static_cast<unsigned long long>(
                  embedded_texture_readback_policy::kMaximumBytes));
          std::fflush(stderr);
        }
        continue;
      }
      if (!resource_transitioned && distinguish_draw_epoch && context_draw_ordinal) {
        native_resolve::Rect native_region;
        const bool native_region_found = GetActiveNativeResolveRegion(fetch_constant_index, native_region);
        native_resolve::Rect sampling_region;
        const bool sampling_eligible = GetActiveNativeResolveRegion(fetch_constant_index, sampling_region, true);
        std::fprintf(stderr, "REX_EMBEDDED_NATIVE_TEXTURE_REGION draw=%llu slot=%u found=%u rect=%u,%u,%u,%u sampling_eligible=%u\n",
            static_cast<unsigned long long>(context_draw_ordinal), fetch_constant_index,
            native_region_found ? 1u : 0u, native_region.left, native_region.top,
            native_region.right, native_region.bottom, sampling_eligible ? 1u : 0u);
        // This runs only after the bounded readback reservation succeeds.
        // Unscaled pages may be native resolves OR untouched/CPU data, so do
        // not classify them as native producers without the resolve trace.
        const auto guest_layout = key.GetGuestLayout();
        std::vector<std::pair<uint32_t, uint32_t>> unscaled_ranges;
        GetUnscaledResolvePageRanges(key.base_page << 12,
            guest_layout.base.level_data_extent_bytes, unscaled_ranges);
        uint64_t unscaled_bytes = 0;
        for (const auto& range : unscaled_ranges) unscaled_bytes += range.second;
        std::fprintf(stderr,
            "REX_EMBEDDED_TEXTURE_GRID draw=%llu slot=%u base=0x%08X extent=%u "
            "logical=%ux%u scaled_view=%u unscaled_bytes=%llu ranges=%u truncated=%u unscaled_ranges=",
            static_cast<unsigned long long>(context_draw_ordinal), fetch_constant_index,
            key.base_page << 12, guest_layout.base.level_data_extent_bytes,
            key.GetWidth(), key.GetHeight(), key.scaled_resolve,
            static_cast<unsigned long long>(unscaled_bytes), uint32_t(unscaled_ranges.size()),
            unscaled_ranges.size() > 64 ? 1u : 0u);
        for (size_t range = 0; range < std::min(unscaled_ranges.size(), size_t(64)); ++range) {
          std::fprintf(stderr, "%s%08X+%u", range ? ";" : "",
              unscaled_ranges[range].first, unscaled_ranges[range].second);
        }
        std::fprintf(stderr, "\n");
      }
      if (!resource_transitioned) {
        command_processor_.PushTransitionBarrier(
            texture_resource,
            d3d12_texture->SetResourceState(D3D12_RESOURCE_STATE_COPY_SOURCE),
            D3D12_RESOURCE_STATE_COPY_SOURCE);
        command_processor_.SubmitBarriers();
        resource_transitioned = true;
      }
      D3D12_RESOURCE_DESC readback_desc;
      ui::d3d12::util::FillBufferResourceDesc(
          readback_desc, total_bytes, D3D12_RESOURCE_FLAG_NONE);
      Microsoft::WRL::ComPtr<ID3D12Resource> readback;
      const ui::d3d12::D3D12Provider& provider =
          command_processor_.GetD3D12Provider();
      if (FAILED(device->CreateCommittedResource(
              &ui::d3d12::util::kHeapPropertiesReadback,
              provider.GetHeapFlagCreateNotZeroed(), &readback_desc,
              D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
              IID_PPV_ARGS(&readback)))) {
        continue;
      }
      D3D12_TEXTURE_COPY_LOCATION destination = {};
      destination.pResource = readback.Get();
      destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
      destination.PlacedFootprint = footprint;
      D3D12_TEXTURE_COPY_LOCATION source = {};
      source.pResource = texture_resource;
      source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
      source.SubresourceIndex = subresource_index;
      Microsoft::WRL::ComPtr<ID3D12Resource> depth_snapshot;
      if (capture_depth_snapshot) {
        // Independent level-zero storage. Never alias a mutable texture-cache
        // resource as temporal history. Reservation above bounds allocations.
        if (SUCCEEDED(device->CreateCommittedResource(
                &ui::d3d12::util::kHeapPropertiesDefault,
                provider.GetHeapFlagCreateNotZeroed(), &snapshot_desc,
                D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                IID_PPV_ARGS(&depth_snapshot)))) {
          D3D12_TEXTURE_COPY_LOCATION snapshot_location = {};
          snapshot_location.pResource = depth_snapshot.Get();
          snapshot_location.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
          command_list.D3DCopyTextureRegion(&snapshot_location, 0, 0, 0, &source, nullptr);
          command_processor_.PushTransitionBarrier(depth_snapshot.Get(),
              D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
          command_processor_.SubmitBarriers();
          source = snapshot_location;
        }
      }
      command_list.D3DCopyTextureRegion(&destination, 0, 0, 0, &source,
                                        nullptr);

      ActiveTextureReadbackDiagnostic diagnostic;
      diagnostic.readback = std::move(readback);
      diagnostic.depth_snapshot = std::move(depth_snapshot);
      diagnostic.snapshot_frame = command_processor_.GetCurrentFrame();
      diagnostic.submission = command_processor_.GetCurrentSubmission();
      diagnostic.resource_identity = resource_identity;
      diagnostic.context_draw_ordinal = context_draw_ordinal;
      diagnostic.fetch_constant_index = fetch_constant_index;
      diagnostic.array_slice = array_slice;
      diagnostic.array_size = base_array_size;
      diagnostic.guest_base = key.base_page << 12;
      diagnostic.guest_size = texture->GetGuestBaseSize();
      diagnostic.width = uint32_t(texture_desc.Width);
      diagnostic.height = texture_desc.Height;
      diagnostic.depth = footprint.Footprint.Depth;
      diagnostic.format = uint32_t(texture_desc.Format);
      diagnostic.row_pitch = footprint.Footprint.RowPitch;
      diagnostic.row_bytes = uint32_t(row_bytes);
      diagnostic.row_count = row_count * footprint.Footprint.Depth;
      diagnostic.scale_x = draw_resolution_scale_x();
      diagnostic.scale_y = draw_resolution_scale_y();
      diagnostic.scaled_resolve = key.scaled_resolve;
      active_texture_readback_diagnostics_.push_back(std::move(diagnostic));
    }
    if (resource_transitioned) {
      command_processor_.PushTransitionBarrier(
          texture_resource,
          d3d12_texture->SetResourceState(kShaderResourceStates),
          kShaderResourceStates);
      command_processor_.SubmitBarriers();
    }
  }
  if (active_texture_readback_diagnostics_.size() != queued_before) {
    std::fprintf(
        stderr,
        "REX_EMBEDDED_ACTIVE_TEXTURE_READBACKS_QUEUED count=%llu bytes=%llu "
        "submission=%llu context_draw=%llu scaled_only=%u\n",
        static_cast<unsigned long long>(
            active_texture_readback_diagnostic_policy_.key_count),
        static_cast<unsigned long long>(
            active_texture_readback_diagnostic_policy_.reserved_bytes),
        static_cast<unsigned long long>(
            command_processor_.GetCurrentSubmission()),
        static_cast<unsigned long long>(context_draw_ordinal),
        scaled_resolve_only ? 1u : 0u);
    std::fflush(stderr);
  }
}

void D3D12TextureCache::WriteActiveTextureSRVKeys(
    TextureSRVKey* keys, const D3D12Shader::TextureBinding* host_shader_bindings,
    size_t host_shader_binding_count) const {
  for (size_t i = 0; i < host_shader_binding_count; ++i) {
    TextureSRVKey& key = keys[i];
    const TextureBinding* binding = GetValidTextureBinding(host_shader_bindings[i].fetch_constant);
    if (!binding) {
      key.key.MakeInvalid();
      key.host_swizzle = xenos::XE_GPU_TEXTURE_SWIZZLE_0000;
      key.swizzled_signs = kSwizzledSignsUnsigned;
      continue;
    }
    key.key = binding->key;
    key.host_swizzle = binding->host_swizzle;
    key.swizzled_signs = binding->swizzled_signs;
  }
}

void D3D12TextureCache::WriteActiveTextureBindfulSRV(
    const D3D12Shader::TextureBinding& host_shader_binding, D3D12_CPU_DESCRIPTOR_HANDLE handle) {
  assert_false(bindless_resources_used_);
  uint32_t descriptor_index = UINT32_MAX;
  Texture* texture = nullptr;
  uint32_t fetch_constant_index = host_shader_binding.fetch_constant;
  const TextureBinding* binding = GetValidTextureBinding(fetch_constant_index);
  if (binding && AreDimensionsCompatible(host_shader_binding.dimension, binding->key.dimension)) {
    bool force_special_view = binding->key.dimension == xenos::DataDimension::k3D &&
                              (host_shader_binding.dimension == xenos::FetchOpDimension::k1D ||
                               host_shader_binding.dimension == xenos::FetchOpDimension::k2D);
    const D3D12TextureBinding& d3d12_binding = d3d12_texture_bindings_[fetch_constant_index];
    if (host_shader_binding.is_signed) {
      // Not supporting signed compressed textures - hopefully DXN and DXT5A are
      // not used as signed.
      if (texture_util::IsAnySignSigned(binding->swizzled_signs)) {
        texture = IsSignedVersionSeparateForFormat(binding->key) ? binding->texture_signed
                                                                 : binding->texture;
        if (force_special_view && texture) {
          descriptor_index = FindOrCreateTextureDescriptor(*static_cast<D3D12Texture*>(texture),
                                                           xenos::DataDimension::k2DOrStacked, true,
                                                           binding->host_swizzle);
        } else {
          descriptor_index = d3d12_binding.descriptor_index_signed;
        }
      }
    } else {
      if (texture_util::IsAnySignNotSigned(binding->swizzled_signs)) {
        texture = binding->texture;
        if (force_special_view && texture) {
          descriptor_index = FindOrCreateTextureDescriptor(*static_cast<D3D12Texture*>(texture),
                                                           xenos::DataDimension::k2DOrStacked,
                                                           false, binding->host_swizzle);
        } else {
          descriptor_index = d3d12_binding.descriptor_index;
        }
      }
    }
  }
  const ui::d3d12::D3D12Provider& provider = command_processor_.GetD3D12Provider();
  D3D12_CPU_DESCRIPTOR_HANDLE source_handle;
  if (descriptor_index != UINT32_MAX) {
    assert_not_null(texture);
    texture->MarkAsUsed();
    source_handle = GetTextureDescriptorCPUHandle(descriptor_index);
  } else {
    NullSRVDescriptorIndex null_descriptor_index;
    switch (host_shader_binding.dimension) {
      case xenos::FetchOpDimension::k3DOrStacked:
        null_descriptor_index = NullSRVDescriptorIndex::k3D;
        break;
      case xenos::FetchOpDimension::kCube:
        null_descriptor_index = NullSRVDescriptorIndex::kCube;
        break;
      default:
        assert_true(host_shader_binding.dimension == xenos::FetchOpDimension::k1D ||
                    host_shader_binding.dimension == xenos::FetchOpDimension::k2D);
        null_descriptor_index = NullSRVDescriptorIndex::k2DArray;
    }
    source_handle = provider.OffsetViewDescriptor(null_srv_descriptor_heap_start_,
                                                  uint32_t(null_descriptor_index));
  }
  if (kPromptTextureDiagnosticsEnabled && binding &&
      binding->key.format == xenos::TextureFormat::k_DXT1 && binding->key.tiled &&
      binding->key.dimension == xenos::DataDimension::k2DOrStacked &&
      binding->key.GetWidth() == 512 && binding->key.GetHeight() == 191) {
    D3D12Texture* d3d12_texture = static_cast<D3D12Texture*>(texture);
    shared_memory().RecordTextureLifecycleDiagnosticEvent(
        SharedMemory::TextureLifecycleDiagnosticEventType::kShaderViewSelected,
        binding->key.base_page << 12,
        texture ? texture->GetGuestBaseSize()
                : binding->key.GetGuestLayout().base.level_data_extent_bytes,
        d3d12_texture ? uint64_t(reinterpret_cast<uintptr_t>(d3d12_texture->resource())) : 0,
        descriptor_index, true);
    shared_memory().DumpTextureLifecycleDiagnosticEvents();
  }
  auto device = provider.GetDevice();
  {
#if XE_GPU_FINE_GRAINED_DRAW_SCOPES
    SCOPE_profile_cpu_i("gpu",
                        "rex::graphics::d3d12::D3D12TextureCache::WriteActiveTextureBindfulSRV->"
                        "CopyDescriptorsSimple");
#endif  // XE_GPU_FINE_GRAINED_DRAW_SCOPES
    device->CopyDescriptorsSimple(1, handle, source_handle, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  }
}

uint32_t D3D12TextureCache::GetActiveTextureBindlessSRVIndex(
    const D3D12Shader::TextureBinding& host_shader_binding) {
  assert_true(bindless_resources_used_);
  uint32_t descriptor_index = UINT32_MAX;
  uint32_t fetch_constant_index = host_shader_binding.fetch_constant;
  const TextureBinding* binding = GetValidTextureBinding(fetch_constant_index);
  if (binding && AreDimensionsCompatible(host_shader_binding.dimension, binding->key.dimension)) {
    bool force_special_view = binding->key.dimension == xenos::DataDimension::k3D &&
                              (host_shader_binding.dimension == xenos::FetchOpDimension::k1D ||
                               host_shader_binding.dimension == xenos::FetchOpDimension::k2D);
    const D3D12TextureBinding& d3d12_binding = d3d12_texture_bindings_[fetch_constant_index];
    if (force_special_view) {
      Texture* texture = nullptr;
      bool use_signed =
          host_shader_binding.is_signed && texture_util::IsAnySignSigned(binding->swizzled_signs);
      if (use_signed) {
        texture = IsSignedVersionSeparateForFormat(binding->key) ? binding->texture_signed
                                                                 : binding->texture;
      } else {
        texture = binding->texture;
      }
      if (texture) {
        descriptor_index = FindOrCreateTextureDescriptor(*static_cast<D3D12Texture*>(texture),
                                                         xenos::DataDimension::k2DOrStacked,
                                                         use_signed, binding->host_swizzle);
      }
    } else {
      descriptor_index = host_shader_binding.is_signed ? d3d12_binding.descriptor_index_signed
                                                       : d3d12_binding.descriptor_index;
    }
  }
  if (descriptor_index == UINT32_MAX) {
    switch (host_shader_binding.dimension) {
      case xenos::FetchOpDimension::k3DOrStacked:
        descriptor_index = uint32_t(D3D12CommandProcessor::SystemBindlessView::kNullTexture3D);
        break;
      case xenos::FetchOpDimension::kCube:
        descriptor_index = uint32_t(D3D12CommandProcessor::SystemBindlessView::kNullTextureCube);
        break;
      default:
        assert_true(host_shader_binding.dimension == xenos::FetchOpDimension::k1D ||
                    host_shader_binding.dimension == xenos::FetchOpDimension::k2D);
        descriptor_index = uint32_t(D3D12CommandProcessor::SystemBindlessView::kNullTexture2DArray);
    }
  }
  if (kPromptTextureDiagnosticsEnabled && binding &&
      binding->key.format == xenos::TextureFormat::k_DXT1 && binding->key.tiled &&
      binding->key.dimension == xenos::DataDimension::k2DOrStacked &&
      binding->key.GetWidth() == 512 && binding->key.GetHeight() == 191) {
    D3D12Texture* texture = static_cast<D3D12Texture*>(binding->texture);
    shared_memory().RecordTextureLifecycleDiagnosticEvent(
        SharedMemory::TextureLifecycleDiagnosticEventType::kShaderViewSelected,
        binding->key.base_page << 12,
        texture ? texture->GetGuestBaseSize()
                : binding->key.GetGuestLayout().base.level_data_extent_bytes,
        texture ? uint64_t(reinterpret_cast<uintptr_t>(texture->resource())) : 0,
        descriptor_index, true);
    shared_memory().DumpTextureLifecycleDiagnosticEvents();
  }
  return descriptor_index;
}

D3D12TextureCache::SamplerParameters D3D12TextureCache::GetSamplerParameters(
    const D3D12Shader::SamplerBinding& binding) const {
  const auto& regs = register_file();
  xenos::xe_gpu_texture_fetch_t fetch = regs.GetTextureFetch(binding.fetch_constant);

  static_assert(sizeof(fetch) == sizeof(SamplerParametersMemo::fetch));
  const uint32_t binding_filters = UINT32_C(0x10000) | uint32_t(binding.mag_filter) |
                                   (uint32_t(binding.min_filter) << 4) |
                                   (uint32_t(binding.mip_filter) << 8) |
                                   (uint32_t(binding.aniso_filter) << 12);
  const int32_t anisotropic_override = REXCVAR_GET(anisotropic_override);
  SamplerParametersMemo& memo = sampler_parameters_memo_[binding.fetch_constant & 31];
  if (memo.binding_filters == binding_filters &&
      memo.anisotropic_override == anisotropic_override &&
      !std::memcmp(memo.fetch, &fetch, sizeof(memo.fetch))) {
    return memo.parameters;
  }

  SamplerParameters parameters;

  xenos::ClampMode fetch_clamp_x, fetch_clamp_y, fetch_clamp_z;
  texture_util::GetClampModesForDimension(fetch, fetch_clamp_x, fetch_clamp_y, fetch_clamp_z);
  parameters.clamp_x = NormalizeClampMode(fetch_clamp_x);
  parameters.clamp_y = NormalizeClampMode(fetch_clamp_y);
  parameters.clamp_z = NormalizeClampMode(fetch_clamp_z);
  if (xenos::ClampModeUsesBorder(parameters.clamp_x) ||
      xenos::ClampModeUsesBorder(parameters.clamp_y) ||
      xenos::ClampModeUsesBorder(parameters.clamp_z)) {
    parameters.border_color = fetch.border_color;
  } else {
    parameters.border_color = xenos::BorderColor::k_ABGR_Black;
  }

  uint32_t mip_min_level, mip_max_level;
  texture_util::GetSubresourcesFromFetchConstant(fetch, nullptr, nullptr, nullptr, nullptr, nullptr,
                                                 &mip_min_level, &mip_max_level);
  parameters.mip_min_level = mip_min_level;
  bool has_mips = mip_max_level > mip_min_level;

  xenos::TextureFilter mag_filter = binding.mag_filter == xenos::TextureFilter::kUseFetchConst
                                        ? fetch.mag_filter
                                        : binding.mag_filter;
  xenos::TextureFilter min_filter = binding.min_filter == xenos::TextureFilter::kUseFetchConst
                                        ? fetch.min_filter
                                        : binding.min_filter;
  xenos::TextureFilter mip_filter = binding.mip_filter == xenos::TextureFilter::kUseFetchConst
                                        ? fetch.mip_filter
                                        : binding.mip_filter;
  bool min_mag_linear = (mag_filter == xenos::TextureFilter::kLinear) &&
                        (min_filter == xenos::TextureFilter::kLinear);
  bool mip_filter_bilinear_or_trilinear =
      mip_filter == xenos::TextureFilter::kPoint || mip_filter == xenos::TextureFilter::kLinear;
  bool mip_base_map = mip_filter == xenos::TextureFilter::kBaseMap;

  // TODO(Triang3l): Disable filtering for texture formats not supporting it.
  xenos::AnisoFilter aniso_filter = binding.aniso_filter == xenos::AnisoFilter::kUseFetchConst
                                        ? fetch.aniso_filter
                                        : binding.aniso_filter;
  if (anisotropic_override > -1 && anisotropic_override < 6 && has_mips && !mip_base_map &&
      min_mag_linear && mip_filter_bilinear_or_trilinear) {
    aniso_filter = xenos::AnisoFilter(anisotropic_override);
  }
  aniso_filter = std::min(aniso_filter, xenos::AnisoFilter::kMax_16_1);
  parameters.aniso_filter = aniso_filter;
  if (aniso_filter != xenos::AnisoFilter::kDisabled) {
    parameters.mag_linear = 1;
    parameters.min_linear = 1;
    parameters.mip_linear = 1;
  } else {
    parameters.mag_linear = mag_filter == xenos::TextureFilter::kLinear;
    parameters.min_linear = min_filter == xenos::TextureFilter::kLinear;
    parameters.mip_linear = mip_filter == xenos::TextureFilter::kLinear;
  }
  parameters.mip_base_map = mip_base_map;

  std::memcpy(memo.fetch, &fetch, sizeof(memo.fetch));
  memo.binding_filters = binding_filters;
  memo.anisotropic_override = anisotropic_override;
  memo.parameters = parameters;
  return parameters;
}

void D3D12TextureCache::WriteSampler(SamplerParameters parameters,
                                     D3D12_CPU_DESCRIPTOR_HANDLE handle) const {
  D3D12_SAMPLER_DESC desc;
  if (parameters.aniso_filter != xenos::AnisoFilter::kDisabled) {
    desc.Filter = D3D12_FILTER_ANISOTROPIC;
    desc.MaxAnisotropy = 1u << (uint32_t(parameters.aniso_filter) - 1);
  } else {
    D3D12_FILTER_TYPE d3d_filter_min =
        parameters.min_linear ? D3D12_FILTER_TYPE_LINEAR : D3D12_FILTER_TYPE_POINT;
    D3D12_FILTER_TYPE d3d_filter_mag =
        parameters.mag_linear ? D3D12_FILTER_TYPE_LINEAR : D3D12_FILTER_TYPE_POINT;
    D3D12_FILTER_TYPE d3d_filter_mip =
        parameters.mip_linear ? D3D12_FILTER_TYPE_LINEAR : D3D12_FILTER_TYPE_POINT;
    desc.Filter = D3D12_ENCODE_BASIC_FILTER(d3d_filter_min, d3d_filter_mag, d3d_filter_mip,
                                            D3D12_FILTER_REDUCTION_TYPE_STANDARD);
    desc.MaxAnisotropy = 1;
  }
  static const D3D12_TEXTURE_ADDRESS_MODE kAddressModeMap[] = {
      /* kRepeat               */ D3D12_TEXTURE_ADDRESS_MODE_WRAP,
      /* kMirroredRepeat       */ D3D12_TEXTURE_ADDRESS_MODE_MIRROR,
      /* kClampToEdge          */ D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
      /* kMirrorClampToEdge    */ D3D12_TEXTURE_ADDRESS_MODE_MIRROR_ONCE,
      // No GL_CLAMP (clamp to half edge, half border) equivalent in Direct3D
      // 12, but there's no Direct3D 9 equivalent anyway, and too weird to be
      // suitable for intentional real usage.
      /* kClampToHalfway       */ D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
      // No mirror and clamp to border equivalents in Direct3D 12, but they
      // aren't there in Direct3D 9 either.
      /* kMirrorClampToHalfway */ D3D12_TEXTURE_ADDRESS_MODE_MIRROR_ONCE,
      /* kClampToBorder        */ D3D12_TEXTURE_ADDRESS_MODE_BORDER,
      /* kMirrorClampToBorder  */ D3D12_TEXTURE_ADDRESS_MODE_MIRROR_ONCE,
  };
  desc.AddressU = kAddressModeMap[uint32_t(parameters.clamp_x)];
  desc.AddressV = kAddressModeMap[uint32_t(parameters.clamp_y)];
  desc.AddressW = kAddressModeMap[uint32_t(parameters.clamp_z)];
  // LOD biasing is performed in shaders.
  desc.MipLODBias = 0.0f;
  desc.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
  switch (parameters.border_color) {
    case xenos::BorderColor::k_ABGR_White:
      desc.BorderColor[0] = 1.0f;
      desc.BorderColor[1] = 1.0f;
      desc.BorderColor[2] = 1.0f;
      desc.BorderColor[3] = 1.0f;
      break;
    case xenos::BorderColor::k_ACBYCR_Black:
      desc.BorderColor[0] = 0.5f;
      desc.BorderColor[1] = 0.0f;
      desc.BorderColor[2] = 0.5f;
      desc.BorderColor[3] = 0.0f;
      break;
    case xenos::BorderColor::k_ACBCRY_Black:
      desc.BorderColor[0] = 0.0f;
      desc.BorderColor[1] = 0.5f;
      desc.BorderColor[2] = 0.5f;
      desc.BorderColor[3] = 0.0f;
      break;
    default:
      assert_true(parameters.border_color == xenos::BorderColor::k_ABGR_Black);
      desc.BorderColor[0] = 0.0f;
      desc.BorderColor[1] = 0.0f;
      desc.BorderColor[2] = 0.0f;
      desc.BorderColor[3] = 0.0f;
      break;
  }
  desc.MinLOD = float(parameters.mip_min_level);
  if (parameters.mip_base_map) {
    // "It is undefined whether LOD clamping based on MinLOD and MaxLOD Sampler
    // states should happen before or after deciding if magnification is
    // occuring" - Direct3D 11.3 Functional Specification.
    // Using the GL_NEAREST / GL_LINEAR minification filter emulation logic
    // described in the Vulkan VkSamplerCreateInfo specification, preserving
    // magnification vs. minification - point mip sampling (usable only without
    // anisotropic filtering on Direct3D 12) and MaxLOD 0.25. With anisotropic
    // filtering, magnification vs. minification doesn't matter as the filter is
    // always linear for both on Direct3D 12 - but linear filtering specifically
    // is what must not be done for kBaseMap, so setting MaxLOD to MinLOD.
    desc.MaxLOD = desc.MinLOD;
    if (parameters.aniso_filter == xenos::AnisoFilter::kDisabled) {
      assert_false(parameters.mip_linear);
      desc.MaxLOD += 0.25f;
    }
  } else {
    // Maximum mip level is in the texture resource itself.
    desc.MaxLOD = FLT_MAX;
  }
  ID3D12Device* device = command_processor_.GetD3D12Provider().GetDevice();
  device->CreateSampler(&desc, handle);
}

bool D3D12TextureCache::ClampDrawResolutionScaleToMaxSupported(
    uint32_t& scale_x, uint32_t& scale_y, const ui::d3d12::D3D12Provider& provider) {
  bool was_clamped;
  if (provider.GetTiledResourcesTier() < D3D12_TILED_RESOURCES_TIER_1) {
    was_clamped = scale_x > 1 || scale_y > 1;
    scale_x = 1;
    scale_y = 1;
    return !was_clamped;
  }
  // Limit to the virtual address space available for a resource.
  was_clamped = false;
  uint32_t virtual_address_bits_per_resource = provider.GetVirtualAddressBitsPerResource();
  while (scale_x > 1 || scale_y > 1) {
    uint64_t highest_scaled_address = uint64_t(SharedMemory::kBufferSize) * (scale_x * scale_y) - 1;
    if (uint32_t(64) - rex::lzcnt(highest_scaled_address) <= virtual_address_bits_per_resource) {
      break;
    }
    // When reducing from a square size, prefer decreasing the horizontal
    // resolution as vertical resolution difference is visible more clearly in
    // perspective.
    was_clamped = true;
    if (scale_x >= scale_y) {
      --scale_x;
    } else {
      --scale_y;
    }
  }
  return !was_clamped;
}

bool D3D12TextureCache::EnsureScaledResolveMemoryCommitted(uint32_t start_unscaled,
                                                           uint32_t length_unscaled,
                                                           uint32_t length_scaled_alignment_log2) {
  assert_true(IsDrawResolutionScaled());

  if (length_unscaled == 0) {
    return true;
  }
  if (start_unscaled > SharedMemory::kBufferSize ||
      (SharedMemory::kBufferSize - start_unscaled) < length_unscaled) {
    // Exceeds the physical address space.
    return false;
  }

  uint32_t draw_resolution_scale_area = draw_resolution_scale_x() * draw_resolution_scale_y();
  uint64_t first_scaled = uint64_t(start_unscaled) * draw_resolution_scale_area;
  uint64_t length_scaled_alignment_bits = (UINT64_C(1) << length_scaled_alignment_log2) - 1;
  uint64_t last_scaled =
      (uint64_t(start_unscaled + (length_unscaled - 1)) * draw_resolution_scale_area +
       length_scaled_alignment_bits) &
      ~length_scaled_alignment_bits;

  const ui::d3d12::D3D12Provider& provider = command_processor_.GetD3D12Provider();
  ID3D12Device* device = provider.GetDevice();

  // Ensure GPU virtual memory for buffers that may be used to access the range
  // is allocated - buffers are created. Always creating both buffers for all
  // addresses before creating the heaps so when creating a new buffer, it can
  // be safely assumed that no existing heaps should be mapped to it.
  std::array<size_t, 2> possible_buffers_first =
      GetPossibleScaledResolveBufferIndices(first_scaled);
  std::array<size_t, 2> possible_buffers_last = GetPossibleScaledResolveBufferIndices(last_scaled);
  size_t possible_buffer_first = std::min(possible_buffers_first[0], possible_buffers_first[1]);
  size_t possible_buffer_last = std::max(possible_buffers_last[0], possible_buffers_last[1]);
  for (size_t i = possible_buffer_first; i <= possible_buffer_last; ++i) {
    if (scaled_resolve_2gb_buffers_[i]) {
      continue;
    }
    D3D12_RESOURCE_DESC scaled_resolve_buffer_desc;
    // Buffer indices are gigabytes.
    ui::d3d12::util::FillBufferResourceDesc(
        scaled_resolve_buffer_desc,
        std::min(
            uint64_t(1) << 31,
            uint64_t(SharedMemory::kBufferSize) * draw_resolution_scale_area - (uint64_t(i) << 30)),
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    // The first access will be a resolve.
    constexpr D3D12_RESOURCE_STATES kScaledResolveVirtualBufferInitialState =
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    ID3D12Resource* scaled_resolve_buffer_resource;
    if (FAILED(device->CreateReservedResource(&scaled_resolve_buffer_desc,
                                              kScaledResolveVirtualBufferInitialState, nullptr,
                                              IID_PPV_ARGS(&scaled_resolve_buffer_resource)))) {
      REXGPU_ERROR(
          "D3D12TextureCache: Failed to create a 2 GB tiled buffer for draw "
          "resolution scaling");
      return false;
    }
    scaled_resolve_2gb_buffers_[i] =
        std::unique_ptr<ScaledResolveVirtualBuffer>(new ScaledResolveVirtualBuffer(
            scaled_resolve_buffer_resource, kScaledResolveVirtualBufferInitialState));
    scaled_resolve_buffer_resource->Release();
  }

  uint32_t heap_first = uint32_t(first_scaled >> kScaledResolveHeapSizeLog2);
  uint32_t heap_last = uint32_t(last_scaled >> kScaledResolveHeapSizeLog2);
  for (uint32_t i = heap_first; i <= heap_last; ++i) {
    if (scaled_resolve_heaps_[i]) {
      continue;
    }
    auto direct_queue = provider.GetDirectQueue();
    D3D12_HEAP_DESC heap_desc = {};
    heap_desc.SizeInBytes = kScaledResolveHeapSize;
    heap_desc.Properties.Type = D3D12_HEAP_TYPE_DEFAULT;
    heap_desc.Flags = D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS | provider.GetHeapFlagCreateNotZeroed();
    Microsoft::WRL::ComPtr<ID3D12Heap> scaled_resolve_heap;
    if (FAILED(device->CreateHeap(&heap_desc, IID_PPV_ARGS(&scaled_resolve_heap)))) {
      REXGPU_ERROR("D3D12TextureCache: Failed to create a scaled resolve tile heap");
      return false;
    }
    scaled_resolve_heaps_[i] = scaled_resolve_heap;
    ++scaled_resolve_heap_count_;
    COUNT_profile_set("gpu/texture_cache/scaled_resolve_buffer_used_mb",
                      scaled_resolve_heap_count_ << (kScaledResolveHeapSizeLog2 - 20));
    D3D12_TILED_RESOURCE_COORDINATE region_start_coordinates;
    region_start_coordinates.Y = 0;
    region_start_coordinates.Z = 0;
    region_start_coordinates.Subresource = 0;
    D3D12_TILE_REGION_SIZE region_size;
    region_size.NumTiles = kScaledResolveHeapSize / D3D12_TILED_RESOURCE_TILE_SIZE_IN_BYTES;
    region_size.UseBox = FALSE;
    D3D12_TILE_RANGE_FLAGS range_flags = D3D12_TILE_RANGE_FLAG_NONE;
    UINT heap_range_start_offset = 0;
    UINT range_tile_count = kScaledResolveHeapSize / D3D12_TILED_RESOURCE_TILE_SIZE_IN_BYTES;
    std::array<size_t, 2> buffer_indices =
        GetPossibleScaledResolveBufferIndices(uint64_t(i) << kScaledResolveHeapSizeLog2);
    for (size_t j = 0; j < 2; ++j) {
      size_t buffer_index = buffer_indices[j];
      if (j && buffer_index == buffer_indices[0]) {
        break;
      }
      region_start_coordinates.X =
          UINT(((uint64_t(i) << kScaledResolveHeapSizeLog2) - (uint64_t(buffer_index) << 30)) /
               D3D12_TILED_RESOURCE_TILE_SIZE_IN_BYTES);
      direct_queue->UpdateTileMappings(
          scaled_resolve_2gb_buffers_[buffer_index]->resource(), 1, &region_start_coordinates,
          &region_size, scaled_resolve_heap.Get(), 1, &range_flags, &heap_range_start_offset,
          &range_tile_count, D3D12_TILE_MAPPING_FLAG_NONE);
    }
    command_processor_.NotifyQueueOperationsDoneDirectly();
  }
  return true;
}

bool D3D12TextureCache::MakeScaledResolveRangeCurrent(uint32_t start_unscaled,
                                                      uint32_t length_unscaled,
                                                      uint32_t length_scaled_alignment_log2) {
  assert_true(IsDrawResolutionScaled());

  if (!length_unscaled || start_unscaled >= SharedMemory::kBufferSize ||
      (SharedMemory::kBufferSize - start_unscaled) < length_unscaled) {
    // If length is 0, the needed buffer can't be chosen because no buffer is
    // needed.
    return false;
  }

  uint32_t draw_resolution_scale_area = draw_resolution_scale_x() * draw_resolution_scale_y();
  uint64_t start_scaled = uint64_t(start_unscaled) * draw_resolution_scale_area;
  uint64_t length_scaled_alignment_bits = (UINT64_C(1) << length_scaled_alignment_log2) - 1;
  uint64_t length_scaled =
      (uint64_t(length_unscaled) * draw_resolution_scale_area + length_scaled_alignment_bits) &
      ~length_scaled_alignment_bits;
  uint64_t last_scaled = start_scaled + (length_scaled - 1);

  // Get one or two buffers that can hold the whole range.
  std::array<size_t, 2> possible_buffer_indices_first =
      GetPossibleScaledResolveBufferIndices(start_scaled);
  std::array<size_t, 2> possible_buffer_indices_last =
      GetPossibleScaledResolveBufferIndices(last_scaled);
  size_t possible_buffer_indices_common[2];
  size_t possible_buffer_indices_common_count = 0;
  for (size_t i = 0;
       i <= size_t(possible_buffer_indices_first[0] != possible_buffer_indices_first[1]); ++i) {
    size_t possible_buffer_index_first = possible_buffer_indices_first[i];
    for (size_t j = 0;
         j <= size_t(possible_buffer_indices_last[0] != possible_buffer_indices_last[1]); ++j) {
      if (possible_buffer_indices_last[j] == possible_buffer_index_first) {
        bool possible_buffer_index_already_added = false;
        for (size_t k = 0; k < possible_buffer_indices_common_count; ++k) {
          if (possible_buffer_indices_common[k] == possible_buffer_index_first) {
            possible_buffer_index_already_added = true;
            break;
          }
        }
        if (!possible_buffer_index_already_added) {
          assert_true(possible_buffer_indices_common_count < 2);
          possible_buffer_indices_common[possible_buffer_indices_common_count++] =
              possible_buffer_index_first;
        }
      }
    }
  }
  if (!possible_buffer_indices_common_count) {
    // Too wide range requested - no buffer that contains both the start and the
    // end.
    return false;
  }

  size_t gigabyte_first = size_t(start_scaled >> 30);
  size_t gigabyte_last = size_t(last_scaled >> 30);

  // Choose the buffer that the range will be accessed through.
  size_t new_buffer_index;
  if (possible_buffer_indices_common_count >= 2) {
    // Prefer the buffer that is already used to make less aliasing barriers.
    assert_true(gigabyte_first + 1 >= gigabyte_last);
    size_t possible_buffer_indices_already_used[2] = {};
    for (size_t i = gigabyte_first; i <= gigabyte_last; ++i) {
      size_t gigabyte_current_buffer_index = scaled_resolve_1gb_buffer_indices_[i];
      for (size_t j = 0; j < possible_buffer_indices_common_count; ++j) {
        if (possible_buffer_indices_common[j] == gigabyte_current_buffer_index) {
          ++possible_buffer_indices_already_used[j];
        }
      }
    }
    new_buffer_index = possible_buffer_indices_common[size_t(
        possible_buffer_indices_already_used[1] > possible_buffer_indices_already_used[0])];
  } else {
    // The range can be accessed only by one buffer.
    new_buffer_index = possible_buffer_indices_common[0];
  }

  // Switch the current buffer for the range.
  const ScaledResolveVirtualBuffer* new_buffer =
      scaled_resolve_2gb_buffers_[new_buffer_index].get();
  assert_not_null(new_buffer);
  ID3D12Resource* new_buffer_resource = new_buffer->resource();
  for (size_t i = gigabyte_first; i <= gigabyte_last; ++i) {
    size_t gigabyte_current_buffer_index = scaled_resolve_1gb_buffer_indices_[i];
    if (gigabyte_current_buffer_index == new_buffer_index) {
      continue;
    }
    if (gigabyte_current_buffer_index != SIZE_MAX) {
      ScaledResolveVirtualBuffer* gigabyte_current_buffer =
          scaled_resolve_2gb_buffers_[gigabyte_current_buffer_index].get();
      assert_not_null(gigabyte_current_buffer);
      command_processor_.PushAliasingBarrier(gigabyte_current_buffer->resource(),
                                             new_buffer_resource);
      // An aliasing barrier synchronizes and flushes everything.
      gigabyte_current_buffer->ClearUAVBarrierPending();
    }
    scaled_resolve_1gb_buffer_indices_[i] = new_buffer_index;
  }

  scaled_resolve_current_range_start_scaled_ = start_scaled;
  scaled_resolve_current_range_length_scaled_ = length_scaled;
  return true;
}

bool D3D12TextureCache::InitializeUnscaledResolvePagesFromSharedMemory(
    uint32_t start_unscaled, uint32_t length_unscaled,
    uint32_t bytes_per_block_log2) {
  assert_true(IsDrawResolutionScaled());
  assert_true(bytes_per_block_log2 <= 4);
  if (!scaled_resolve_initialize_pipeline_ || bytes_per_block_log2 > 4) {
    return false;
  }

  std::vector<std::pair<uint32_t, uint32_t>> unscaled_ranges;
  GetUnscaledResolvePageRanges(start_unscaled, length_unscaled,
                               unscaled_ranges);
  if (unscaled_ranges.empty()) {
    return true;
  }

  const uint32_t scale_area =
      draw_resolution_scale_x() * draw_resolution_scale_y();
  // A MakeScaledResolveRangeCurrent request must fit within a sliding 2 GB
  // buffer. Keep initialization chunks at or below 1 GB in scaled space and on
  // complete guest-page boundaries.
  constexpr uint32_t kMaxDispatchScaledBytes =
      UINT32_C(65535) * UINT32_C(256) * UINT32_C(4);
  uint32_t max_chunk_unscaled = std::min(
      (UINT32_C(1) << 30) / scale_area,
      kMaxDispatchScaledBytes / scale_area);
  max_chunk_unscaled &= ~((UINT32_C(1) << 12) - 1);
  if (!max_chunk_unscaled) {
    return false;
  }

  D3D12SharedMemory& d3d12_shared_memory =
      static_cast<D3D12SharedMemory&>(shared_memory());
  DeferredCommandList& command_list =
      command_processor_.GetDeferredCommandList();
  ID3D12Device* device =
      command_processor_.GetD3D12Provider().GetDevice();

  struct Constants {
    uint32_t source_offset;
    uint32_t source_length;
    uint32_t scale_x;
    uint32_t scale_y;
    uint32_t bytes_per_block_log2;
  };
  static_assert(sizeof(Constants) <= sizeof(LoadConstants));

  for (const std::pair<uint32_t, uint32_t>& range : unscaled_ranges) {
    uint32_t chunk_start = range.first;
    uint32_t remaining = range.second;
    while (remaining) {
      uint32_t chunk_length = std::min(remaining, max_chunk_unscaled);
      if (!d3d12_shared_memory.RequestRange(chunk_start, chunk_length) ||
          !EnsureScaledResolveMemoryCommitted(chunk_start, chunk_length, 2) ||
          !MakeScaledResolveRangeCurrent(chunk_start, chunk_length, 2)) {
        return false;
      }

      ui::d3d12::util::DescriptorCpuGpuHandlePair descriptors[2];
      if (!command_processor_.RequestOneUseSingleViewDescriptors(2,
                                                                  descriptors)) {
        return false;
      }
      d3d12_shared_memory.WriteRawSRVDescriptor(descriptors[0].first);
      CreateCurrentScaledResolveRangeRawUAV(descriptors[1].first);

      d3d12_shared_memory.UseForReading();
      TransitionCurrentScaledResolveRange(
          D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

      command_processor_.SetExternalPipeline(
          scaled_resolve_initialize_pipeline_.Get());
      command_list.D3DSetComputeRootSignature(load_root_signature_.Get());
      Constants constants = {chunk_start, chunk_length,
                             draw_resolution_scale_x(),
                             draw_resolution_scale_y(),
                             bytes_per_block_log2};
      command_list.D3DSetComputeRoot32BitConstants(
          0, sizeof(constants) / sizeof(uint32_t), &constants, 0);
      command_list.D3DSetComputeRootDescriptorTable(1,
                                                    descriptors[0].second);
      command_list.D3DSetComputeRootDescriptorTable(2,
                                                    descriptors[1].second);
      command_processor_.SubmitBarriers();
      uint64_t destination_length =
          uint64_t(chunk_length) * scale_area;
      command_list.D3DDispatch(
          uint32_t((destination_length / 4 + 255) / 256), 1, 1);
      MarkCurrentScaledResolveRangeUAVWritesCommitNeeded();
      // Order the initialization before the partial resolve that will overlay
      // it. A real state transition also commits pending UAV writes.
      TransitionCurrentScaledResolveRange(
          D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

      chunk_start += chunk_length;
      remaining -= chunk_length;
    }
  }
  return true;
}

void D3D12TextureCache::TransitionCurrentScaledResolveRange(D3D12_RESOURCE_STATES new_state) {
  assert_true(IsDrawResolutionScaled());
  ScaledResolveVirtualBuffer& buffer = GetCurrentScaledResolveBuffer();
  command_processor_.PushTransitionBarrier(buffer.resource(), buffer.SetResourceState(new_state),
                                           new_state);
}

bool D3D12TextureCache::CaptureCurrentScaledResolveRange(
    uint32_t start_unscaled, uint32_t length_unscaled,
    const char* diagnostic_label, const char* dump_path) {
  if (!dump_path || !length_unscaled || !IsDrawResolutionScaled()) {
    return false;
  }

  const uint32_t scale_area =
      draw_resolution_scale_x() * draw_resolution_scale_y();
  const uint64_t start_scaled = uint64_t(start_unscaled) * scale_area;
  const uint64_t length_scaled = uint64_t(length_unscaled) * scale_area;
  const uint64_t end_scaled = start_scaled + length_scaled;
  const uint64_t current_end_scaled =
      scaled_resolve_current_range_start_scaled_ +
      scaled_resolve_current_range_length_scaled_;
  constexpr uint64_t kMaximumDiagnosticBytes = UINT64_C(256) << 20;
  if (!length_scaled || length_scaled > kMaximumDiagnosticBytes ||
      start_scaled < scaled_resolve_current_range_start_scaled_ ||
      end_scaled > current_end_scaled) {
    std::fprintf(
        stderr,
        "REX_EMBEDDED_RESOLVE_BOUNDARY result=range_rejected "
        "stage=scaled_after_copy label=%s start=0x%llX bytes=%llu "
        "current=0x%llX+%llu\n",
        diagnostic_label ? diagnostic_label : "unnamed",
        static_cast<unsigned long long>(start_scaled),
        static_cast<unsigned long long>(length_scaled),
        static_cast<unsigned long long>(
            scaled_resolve_current_range_start_scaled_),
        static_cast<unsigned long long>(
            scaled_resolve_current_range_length_scaled_));
    std::fflush(stderr);
    return false;
  }

  const size_t buffer_index = GetCurrentScaledResolveBufferIndex();
  const uint64_t buffer_base = uint64_t(buffer_index) << 30;
  ScaledResolveVirtualBuffer& buffer = GetCurrentScaledResolveBuffer();
  ID3D12Resource* resource = buffer.resource();
  const uint64_t source_offset = start_scaled - buffer_base;
  if (start_scaled < buffer_base ||
      source_offset + length_scaled > resource->GetDesc().Width) {
    std::fprintf(stderr,
                 "REX_EMBEDDED_RESOLVE_BOUNDARY result=buffer_rejected "
                 "stage=scaled_after_copy label=%s buffer=%llu "
                 "offset=%llu bytes=%llu resource_bytes=%llu\n",
                 diagnostic_label ? diagnostic_label : "unnamed",
                 static_cast<unsigned long long>(buffer_index),
                 static_cast<unsigned long long>(source_offset),
                 static_cast<unsigned long long>(length_scaled),
                 static_cast<unsigned long long>(resource->GetDesc().Width));
    std::fflush(stderr);
    return false;
  }

  D3D12_RESOURCE_DESC readback_desc;
  ui::d3d12::util::FillBufferResourceDesc(
      readback_desc, length_scaled, D3D12_RESOURCE_FLAG_NONE);
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
                 "stage=scaled_after_copy label=%s bytes=%llu\n",
                 diagnostic_label ? diagnostic_label : "unnamed",
                 static_cast<unsigned long long>(length_scaled));
    std::fflush(stderr);
    return false;
  }

  const D3D12_RESOURCE_STATES old_state =
      buffer.SetResourceState(D3D12_RESOURCE_STATE_COPY_SOURCE);
  command_processor_.PushTransitionBarrier(
      resource, old_state, D3D12_RESOURCE_STATE_COPY_SOURCE);
  command_processor_.SubmitBarriers();
  command_processor_.GetDeferredCommandList().D3DCopyBufferRegion(
      readback.Get(), 0, resource, source_offset, length_scaled);
  command_processor_.PushTransitionBarrier(
      resource, buffer.SetResourceState(old_state), old_state);
  command_processor_.SubmitBarriers();
  if (!command_processor_.AwaitAllQueueOperationsCompletion()) {
    std::fprintf(stderr,
                 "REX_EMBEDDED_RESOLVE_BOUNDARY result=await_failed "
                 "stage=scaled_after_copy label=%s\n",
                 diagnostic_label ? diagnostic_label : "unnamed");
    std::fflush(stderr);
    return false;
  }

  D3D12_RANGE read_range = {0, SIZE_T(length_scaled)};
  void* mapping = nullptr;
  if (FAILED(readback->Map(0, &read_range, &mapping))) {
    std::fprintf(stderr,
                 "REX_EMBEDDED_RESOLVE_BOUNDARY result=map_failed "
                 "stage=scaled_after_copy label=%s\n",
                 diagnostic_label ? diagnostic_label : "unnamed");
    std::fflush(stderr);
    return false;
  }

  const uint8_t* bytes = static_cast<const uint8_t*>(mapping);
  uint32_t hash = 2166136261u;
  for (uint64_t i = 0; i < length_scaled; ++i) {
    hash = (hash ^ bytes[i]) * 16777619u;
  }
  FILE* dump_file = std::fopen(dump_path, "wb");
  const size_t dumped_bytes =
      dump_file ? std::fwrite(mapping, 1, size_t(length_scaled), dump_file) : 0;
  if (dump_file) {
    std::fclose(dump_file);
  }
  std::fprintf(
      stderr,
      "REX_EMBEDDED_RESOLVE_BOUNDARY result=%s stage=scaled_after_copy "
      "label=%s dump=%s guest=0x%08X+%u scaled=0x%llX+%llu "
      "buffer=%llu offset=%llu fnv1a=0x%08X scale=%ux%u old_state=0x%X\n",
      dumped_bytes == size_t(length_scaled) ? "ok" : "write_failed",
      diagnostic_label ? diagnostic_label : "unnamed", dump_path,
      start_unscaled, length_unscaled,
      static_cast<unsigned long long>(start_scaled),
      static_cast<unsigned long long>(length_scaled),
      static_cast<unsigned long long>(buffer_index),
      static_cast<unsigned long long>(source_offset), hash,
      draw_resolution_scale_x(), draw_resolution_scale_y(), uint32_t(old_state));
  std::fflush(stderr);
  D3D12_RANGE write_range = {0, 0};
  readback->Unmap(0, &write_range);
  return dumped_bytes == size_t(length_scaled);
}

void D3D12TextureCache::CreateCurrentScaledResolveRangeUintPow2SRV(
    D3D12_CPU_DESCRIPTOR_HANDLE handle, uint32_t element_size_bytes_pow2) {
  assert_true(IsDrawResolutionScaled());
  size_t buffer_index = GetCurrentScaledResolveBufferIndex();
  const ScaledResolveVirtualBuffer* buffer = scaled_resolve_2gb_buffers_[buffer_index].get();
  assert_not_null(buffer);
  ui::d3d12::util::CreateBufferTypedSRV(
      command_processor_.GetD3D12Provider().GetDevice(), handle, buffer->resource(),
      ui::d3d12::util::GetUintPow2DXGIFormat(element_size_bytes_pow2),
      uint32_t(scaled_resolve_current_range_length_scaled_ >> element_size_bytes_pow2),
      (scaled_resolve_current_range_start_scaled_ - (uint64_t(buffer_index) << 30)) >>
          element_size_bytes_pow2);
}

void D3D12TextureCache::CreateCurrentScaledResolveRangeUintPow2UAV(
    D3D12_CPU_DESCRIPTOR_HANDLE handle, uint32_t element_size_bytes_pow2) {
  assert_true(IsDrawResolutionScaled());
  size_t buffer_index = GetCurrentScaledResolveBufferIndex();
  const ScaledResolveVirtualBuffer* buffer = scaled_resolve_2gb_buffers_[buffer_index].get();
  assert_not_null(buffer);
  ui::d3d12::util::CreateBufferTypedUAV(
      command_processor_.GetD3D12Provider().GetDevice(), handle, buffer->resource(),
      ui::d3d12::util::GetUintPow2DXGIFormat(element_size_bytes_pow2),
      uint32_t(scaled_resolve_current_range_length_scaled_ >> element_size_bytes_pow2),
      (scaled_resolve_current_range_start_scaled_ - (uint64_t(buffer_index) << 30)) >>
          element_size_bytes_pow2);
}

void D3D12TextureCache::CreateCurrentScaledResolveRangeRawUAV(
    D3D12_CPU_DESCRIPTOR_HANDLE handle) {
  assert_true(IsDrawResolutionScaled());
  size_t buffer_index = GetCurrentScaledResolveBufferIndex();
  const ScaledResolveVirtualBuffer* buffer =
      scaled_resolve_2gb_buffers_[buffer_index].get();
  assert_not_null(buffer);
  assert_true(scaled_resolve_current_range_length_scaled_ <= UINT32_MAX);
  ui::d3d12::util::CreateBufferRawUAV(
      command_processor_.GetD3D12Provider().GetDevice(), handle,
      buffer->resource(), uint32_t(scaled_resolve_current_range_length_scaled_),
      scaled_resolve_current_range_start_scaled_ -
          (uint64_t(buffer_index) << 30));
}

ID3D12Resource* D3D12TextureCache::RequestSwapTexture(D3D12_SHADER_RESOURCE_VIEW_DESC& srv_desc_out,
                                                      xenos::TextureFormat& format_out,
                                                      uint32_t* width_unscaled_out,
                                                      uint32_t* height_unscaled_out) {
  const auto& regs = register_file();
  xenos::xe_gpu_texture_fetch_t fetch = regs.GetTextureFetch(0);
  TextureKey key;
  BindingInfoFromFetchConstant(fetch, key, nullptr);
  if (!key.is_valid || key.base_page == 0 || key.dimension != xenos::DataDimension::k2DOrStacked) {
    return nullptr;
  }
  D3D12Texture* texture = static_cast<D3D12Texture*>(FindOrCreateTexture(key));
  if (texture == nullptr || !LoadTextureData(*texture)) {
    return nullptr;
  }
  texture->MarkAsUsed();
  // The swap texture is likely to be used only for the presentation compute
  // shader, and not during emulation, where it'd be NON_PIXEL_SHADER_RESOURCE |
  // PIXEL_SHADER_RESOURCE.
  ID3D12Resource* texture_resource = texture->resource();
  command_processor_.PushTransitionBarrier(
      texture_resource, texture->SetResourceState(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
      D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  srv_desc_out.Format = GetDXGIUnormFormat(key);
  srv_desc_out.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
  srv_desc_out.Shader4ComponentMapping =
      GuestToHostSwizzle(fetch.swizzle, GetHostFormatSwizzle(key)) |
      D3D12_SHADER_COMPONENT_MAPPING_ALWAYS_SET_BIT_AVOIDING_ZEROMEM_MISTAKES;
  srv_desc_out.Texture2D.MostDetailedMip = 0;
  srv_desc_out.Texture2D.MipLevels = 1;
  srv_desc_out.Texture2D.PlaneSlice = 0;
  srv_desc_out.Texture2D.ResourceMinLODClamp = 0.0f;
  // Only texture->key, not the result of BindingInfoFromFetchConstant, contains
  // whether the texture is scaled.
  key = texture->key();
  if (width_unscaled_out) {
    *width_unscaled_out = key.GetWidth();
  }
  if (height_unscaled_out) {
    *height_unscaled_out = key.GetHeight();
  }
  format_out = key.format;
  return texture_resource;
}

D3D12TextureCache::D3D12Texture::D3D12Texture(D3D12TextureCache& texture_cache,
                                              const TextureKey& key, ID3D12Resource* resource,
                                              D3D12_RESOURCE_STATES resource_state,
                                              bool track_usage)
    : Texture(texture_cache, key, track_usage),
      resource_(resource),
      resource_state_(resource_state) {
  ID3D12Device* device = texture_cache.command_processor_.GetD3D12Provider().GetDevice();
  D3D12_RESOURCE_DESC resource_desc = resource_->GetDesc();
  SetHostMemoryUsage(device->GetResourceAllocationInfo(0, 1, &resource_desc).SizeInBytes);
}

D3D12TextureCache::D3D12Texture::~D3D12Texture() {
  auto& d3d12_texture_cache = static_cast<D3D12TextureCache&>(texture_cache());
  d3d12_texture_cache.OnD3D12TextureDestroyed(*this);
  for (const auto& descriptor_pair : srv_descriptors_) {
    d3d12_texture_cache.ReleaseTextureDescriptor(descriptor_pair.second);
  }
  if (replaced_original_) {
    // Textures are destroyed once the GPU has finished their last use; the
    // pack keeps its own reference to the replacement.
    resource_ = std::move(replaced_original_);
  }
  if (heap_index_ >= 0) {
    // Release the placed resource before its range can be placed again.
    resource_.Reset();
    d3d12_texture_cache.FreeTextureHeapRange(heap_index_, heap_offset_, heap_size_);
  }
}

bool D3D12TextureCache::CreateTextureHeap() {
  if (texture_heaps_.size() >= kTextureHeapMaxCount) {
    return false;
  }
  const ui::d3d12::D3D12Provider& provider = command_processor_.GetD3D12Provider();
  D3D12_HEAP_DESC heap_desc = {};
  heap_desc.SizeInBytes = kTextureHeapSize;
  heap_desc.Properties.Type = D3D12_HEAP_TYPE_DEFAULT;
  heap_desc.Alignment = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
  // Non-render-target textures only (valid on resource heap tier 1 too).
  heap_desc.Flags =
      D3D12_HEAP_FLAG_ALLOW_ONLY_NON_RT_DS_TEXTURES | provider.GetHeapFlagCreateNotZeroed();
  auto heap = std::make_unique<TextureHeap>(kTextureHeapSize);
  if (FAILED(provider.GetDevice()->CreateHeap(&heap_desc, IID_PPV_ARGS(&heap->heap)))) {
    REXGPU_WARN("D3D12TextureCache: Failed to create a {} MB texture heap",
                kTextureHeapSize >> 20);
    return false;
  }
  texture_heaps_.push_back(std::move(heap));
  return true;
}

Microsoft::WRL::ComPtr<ID3D12Resource> D3D12TextureCache::CreatePlacedTexture(
    const D3D12_RESOURCE_DESC& desc, D3D12_RESOURCE_STATES state, int32_t& heap_index,
    uint64_t& heap_offset, uint64_t& heap_size) {
  Microsoft::WRL::ComPtr<ID3D12Resource> resource;
  if (texture_heaps_.empty()) {
    return resource;
  }
  ID3D12Device* device = command_processor_.GetD3D12Provider().GetDevice();
  // Small textures may use 4 KB placement when the device allows it.
  D3D12_RESOURCE_DESC placed_desc = desc;
  placed_desc.Alignment = D3D12_SMALL_RESOURCE_PLACEMENT_ALIGNMENT;
  D3D12_RESOURCE_ALLOCATION_INFO info = device->GetResourceAllocationInfo(0, 1, &placed_desc);
  if (info.SizeInBytes == UINT64_MAX ||
      info.Alignment != D3D12_SMALL_RESOURCE_PLACEMENT_ALIGNMENT) {
    placed_desc.Alignment = 0;
    info = device->GetResourceAllocationInfo(0, 1, &placed_desc);
  }
  // Large textures keep their own allocation (rare; would fragment the heaps).
  if (info.SizeInBytes == UINT64_MAX || info.SizeInBytes > kTextureHeapSize / 4 ||
      info.Alignment > D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT) {
    ++texture_heap_fallbacks_;
    return resource;
  }
  for (size_t attempt = 0; attempt < 2; ++attempt) {
    for (size_t i = 0; i < texture_heaps_.size(); ++i) {
      TextureHeap& heap = *texture_heaps_[i];
      uint64_t offset;
      if (!heap.allocator.Allocate(info.SizeInBytes, info.Alignment, offset)) {
        continue;
      }
      if (FAILED(device->CreatePlacedResource(heap.heap.Get(), offset, &placed_desc, state,
                                              nullptr, IID_PPV_ARGS(&resource)))) {
        heap.allocator.Free(offset, info.SizeInBytes);
        ++texture_heap_fallbacks_;
        return nullptr;
      }
      heap_index = int32_t(i);
      heap_offset = offset;
      heap_size = info.SizeInBytes;
      ++texture_heap_placed_;
      return resource;
    }
    // All heaps full: add one (a single allocation for many textures).
    if (attempt || !CreateTextureHeap()) {
      break;
    }
  }
  ++texture_heap_fallbacks_;
  return resource;
}

void D3D12TextureCache::FreeTextureHeapRange(int32_t heap_index, uint64_t offset,
                                             uint64_t size) {
  if (heap_index >= 0 && size_t(heap_index) < texture_heaps_.size()) {
    texture_heaps_[size_t(heap_index)]->allocator.Free(offset, size);
  }
}

bool D3D12TextureCache::IsDecompressionNeeded(xenos::TextureFormat format, uint32_t width,
                                              uint32_t height) const {
  DXGI_FORMAT dxgi_format_uncompressed = host_formats_[uint32_t(format)].dxgi_format_uncompressed;
  if (dxgi_format_uncompressed == DXGI_FORMAT_UNKNOWN) {
    return false;
  }
  const FormatInfo* format_info = FormatInfo::Get(format);
  if (!(width & (format_info->block_width - 1)) && !(height & (format_info->block_height - 1))) {
    return false;
  }
  // UnalignedBlockTexturesSupported is for block-compressed textures with the
  // block size of 4x4, but not for 2x1 (4:2:2) subsampled formats.
  if (format_info->block_width == 4 && format_info->block_height == 4 &&
      command_processor_.GetD3D12Provider().AreUnalignedBlockTexturesSupported()) {
    return false;
  }
  return true;
}

TextureCache::LoadShaderIndex D3D12TextureCache::GetLoadShaderIndex(TextureKey key) const {
  const HostFormat& host_format = host_formats_[uint32_t(key.format)];
  if (key.signed_separate) {
    return host_format.load_shader_signed;
  }
  if (IsDecompressionNeeded(key.format, key.GetWidth(), key.GetHeight())) {
    return host_format.load_shader_decompress;
  }
  return host_format.load_shader;
}

bool D3D12TextureCache::IsSignedVersionSeparateForFormat(TextureKey key) const {
  const HostFormat& host_format = host_formats_[uint32_t(key.format)];
  return host_format.load_shader_signed != kLoadShaderIndexUnknown &&
         host_format.load_shader_signed != host_format.load_shader;
}

bool D3D12TextureCache::IsScaledResolveSupportedForFormat(TextureKey key) const {
  LoadShaderIndex load_shader = GetLoadShaderIndex(key);
  return load_shader != kLoadShaderIndexUnknown && load_pipelines_scaled_[load_shader] != nullptr;
}

uint32_t D3D12TextureCache::GetHostFormatSwizzle(TextureKey key) const {
  // Dense cache-line-aligned swizzle array avoids cache misses from accessing
  // the full HostFormat struct on every texture fetch.
  alignas(64) static const auto swizzle_cache = []() {
    std::array<uint16_t, 64> arr{};
    for (int i = 0; i < 64; ++i) {
      arr[i] = static_cast<uint16_t>(host_formats_[i].swizzle);
    }
    return arr;
  }();
  return swizzle_cache[uint32_t(key.format)];
}

uint32_t D3D12TextureCache::GetMaxHostTextureWidthHeight(xenos::DataDimension dimension) const {
  switch (dimension) {
    case xenos::DataDimension::k1D:
    case xenos::DataDimension::k2DOrStacked:
      // 1D and 2D are emulated as 2D arrays.
      return D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION;
    case xenos::DataDimension::k3D:
      return D3D12_REQ_TEXTURE3D_U_V_OR_W_DIMENSION;
    case xenos::DataDimension::kCube:
      return D3D12_REQ_TEXTURECUBE_DIMENSION;
    default:
      assert_unhandled_case(dimension);
      return 0;
  }
}

uint32_t D3D12TextureCache::GetMaxHostTextureDepthOrArraySize(
    xenos::DataDimension dimension) const {
  switch (dimension) {
    case xenos::DataDimension::k1D:
    case xenos::DataDimension::k2DOrStacked:
      // 1D and 2D are emulated as 2D arrays.
      return D3D12_REQ_TEXTURE2D_ARRAY_AXIS_DIMENSION;
    case xenos::DataDimension::k3D:
      return D3D12_REQ_TEXTURE3D_U_V_OR_W_DIMENSION;
    case xenos::DataDimension::kCube:
      return D3D12_REQ_TEXTURE2D_ARRAY_AXIS_DIMENSION / 6 * 6;
    default:
      assert_unhandled_case(dimension);
      return 0;
  }
}

std::unique_ptr<TextureCache::Texture> D3D12TextureCache::CreateTexture(TextureKey key) {
  D3D12_RESOURCE_DESC desc;
  desc.Format = GetDXGIResourceFormat(key);
  if (desc.Format == DXGI_FORMAT_UNKNOWN) {
    unsupported_format_features_used_[uint32_t(key.format)] |= kUnsupportedResourceBit;
    return nullptr;
  }
  if (key.dimension == xenos::DataDimension::k3D) {
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE3D;
  } else {
    // 1D textures are treated as 2D for simplicity.
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  }
  desc.Alignment = 0;
  desc.Width = key.GetWidth();
  desc.Height = key.GetHeight();
  if (key.scaled_resolve) {
    desc.Width *= draw_resolution_scale_x();
    desc.Height *= draw_resolution_scale_y();
  }
  desc.DepthOrArraySize = key.GetDepthOrArraySize();
  desc.MipLevels = key.mip_max_level + 1;
  desc.SampleDesc.Count = 1;
  desc.SampleDesc.Quality = 0;
  desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
  // Untiling through a buffer instead of using unordered access because copying
  // is not done that often.
  desc.Flags = D3D12_RESOURCE_FLAG_NONE;
  const ui::d3d12::D3D12Provider& provider = command_processor_.GetD3D12Provider();
  ID3D12Device* device = provider.GetDevice();
  // Assuming untiling will be the next operation.
  D3D12_RESOURCE_STATES resource_state = D3D12_RESOURCE_STATE_COPY_DEST;
  const bool timed_creation = command_processor_.SwapIntervalObserverActive();
  const uint64_t creation_begin =
      timed_creation ? rex::chrono::Clock::QueryHostTickCount() : 0;
  int32_t heap_index = -1;
  uint64_t heap_offset = 0, heap_size = 0;
  Microsoft::WRL::ComPtr<ID3D12Resource> resource;
  if (REXCVAR_GET(d3d12_texture_heap_pool)) {
    resource = CreatePlacedTexture(desc, resource_state, heap_index, heap_offset, heap_size);
  }
  if (!resource &&
      FAILED(device->CreateCommittedResource(&ui::d3d12::util::kHeapPropertiesDefault,
                                             provider.GetHeapFlagCreateNotZeroed(), &desc,
                                             resource_state, nullptr, IID_PPV_ARGS(&resource)))) {
    return nullptr;
  }
  if (timed_creation) {
    command_processor_.NoteTextureCreation(
        rex::chrono::Clock::QueryHostTickCount() - creation_begin, heap_index >= 0);
  }
  if (kPromptTextureDiagnosticsEnabled &&
      key.format == xenos::TextureFormat::k_DXT1 && key.tiled &&
      key.dimension == xenos::DataDimension::k2DOrStacked && key.GetWidth() == 512 &&
      key.GetHeight() == 191) {
    const uint32_t base = key.base_page << 12;
    const uint32_t bytes = key.GetGuestLayout().base.level_data_extent_bytes;
    constexpr uint32_t kPromptBackgroundGuestSourceSpan = 0xF800;
    shared_memory().BeginTextureLifecycleDiagnostic(
        base, std::min(bytes, kPromptBackgroundGuestSourceSpan));
    shared_memory().RecordTextureLifecycleDiagnosticEvent(
        SharedMemory::TextureLifecycleDiagnosticEventType::kHostTextureCreated, base, bytes,
        uint64_t(reinterpret_cast<uintptr_t>(resource.Get())), uint64_t(desc.Format), true);
  }
  auto texture = std::make_unique<D3D12Texture>(*this, key, resource.Get(), resource_state);
  if (heap_index >= 0) {
    texture->SetHeapPlacement(heap_index, heap_offset, heap_size);
  }
  return texture;
}

bool D3D12TextureCache::LoadTextureDataFromResidentMemoryImpl(Texture& texture, bool load_base,
                                                              bool load_mips) {
  D3D12Texture& d3d12_texture = static_cast<D3D12Texture&>(texture);
  TextureKey texture_key = d3d12_texture.key();
  if (d3d12_texture.replaced()) {
    // A pack replacement is shown: keep it while the guest data is the same,
    // otherwise go back to the texture's own resource and load all of it.
    if (ComputeTextureContentId(d3d12_texture) == d3d12_texture.replacement_id()) {
      return true;
    }
    RevertTextureReplacement(d3d12_texture);
    load_base = true;
    load_mips = true;
  }

  DeferredCommandList& command_list = command_processor_.GetDeferredCommandList();
  ID3D12Device* device = command_processor_.GetD3D12Provider().GetDevice();
  D3D12CommandProcessor::GpuTimingScope load_timing(command_processor_,
                                                    GpuTimingCategory::kTextureLoad);

  // Get the pipeline.
  LoadShaderIndex load_shader = GetLoadShaderIndex(texture_key);
  if (load_shader == kLoadShaderIndexUnknown) {
    return false;
  }
  bool texture_resolution_scaled = texture_key.scaled_resolve;
  ID3D12PipelineState* pipeline = texture_resolution_scaled
                                      ? load_pipelines_scaled_[load_shader].Get()
                                      : load_pipelines_[load_shader].Get();
  if (pipeline == nullptr) {
    return false;
  }
  if (command_processor_.GpuTimingEnabled()) {
    const uint64_t guest_bytes = uint64_t(load_base ? texture.GetGuestBaseSize() : 0) +
                                 uint64_t(load_mips ? texture.GetGuestMipsSize() : 0);
    command_processor_.GpuTimingCount(GpuTimingCounter::kTextureLoads, 1);
    command_processor_.GpuTimingCount(GpuTimingCounter::kTextureLoadsScaled,
                                      texture_resolution_scaled ? 1 : 0);
    command_processor_.GpuTimingCount(
        GpuTimingCounter::kTextureLoadBytes,
        guest_bytes * (texture_resolution_scaled
                           ? draw_resolution_scale_x() * draw_resolution_scale_y()
                           : 1));
  }
  const LoadShaderInfo& load_shader_info = GetLoadShaderInfo(load_shader);

  const bool prompt_background_diagnostic =
      kPromptTextureDiagnosticsEnabled &&
      texture_key.format == xenos::TextureFormat::k_DXT1 && texture_key.tiled &&
      texture_key.dimension == xenos::DataDimension::k2DOrStacked &&
      texture_key.GetWidth() == 512 && texture_key.GetHeight() == 191;
  const uint32_t texture_guest_base = texture_key.base_page << 12;
  const uint32_t texture_readback_address_min =
      REXCVAR_GET(embedded_texture_readback_address_min);
  const uint32_t texture_readback_address_max =
      REXCVAR_GET(embedded_texture_readback_address_max);
  const bool texture_readback_cvar_match =
      texture_readback_address_max > texture_readback_address_min &&
      texture_guest_base >= texture_readback_address_min &&
      texture_guest_base < texture_readback_address_max;
  const uint64_t texture_guest_end =
      uint64_t(texture_guest_base) + d3d12_texture.GetGuestBaseSize();
  const bool texture_readback_armed_match =
      texture_readback_armed_address_max_ >
          texture_readback_armed_address_min_ &&
      uint64_t(texture_guest_base) < texture_readback_armed_address_max_ &&
      texture_guest_end > texture_readback_armed_address_min_;
  const bool texture_readback_diagnostic =
      kGpuDiagnostics && (texture_readback_cvar_match || texture_readback_armed_match);
  if (prompt_background_diagnostic) {
    const D3D12_RESOURCE_DESC resource_desc = d3d12_texture.resource()->GetDesc();
    const uint64_t load_flags = uint64_t(load_shader) | (uint64_t(load_base ? 1u : 0u) << 32) |
                                (uint64_t(load_mips ? 1u : 0u) << 33) |
                                (uint64_t(texture_key.scaled_resolve ? 1u : 0u) << 34);
    shared_memory().RecordTextureLifecycleDiagnosticEvent(
        SharedMemory::TextureLifecycleDiagnosticEventType::kD3D12LoadBegin,
        texture_key.base_page << 12, d3d12_texture.GetGuestBaseSize(), load_flags,
        uint64_t(resource_desc.Format), true);
  }

  // Get the guest layout.
  const texture_util::TextureGuestLayout& guest_layout = d3d12_texture.guest_layout();
  xenos::DataDimension dimension = texture_key.dimension;
  bool is_3d = dimension == xenos::DataDimension::k3D;
  bool is_3d_tiling = is_3d || d3d12_texture.force_load_3d_tiling();
  uint32_t width = texture_key.GetWidth();
  uint32_t height = texture_key.GetHeight();
  uint32_t depth_or_array_size = texture_key.GetDepthOrArraySize();
  uint32_t depth = is_3d ? depth_or_array_size : 1;
  uint32_t array_size = is_3d ? 1 : depth_or_array_size;
  xenos::TextureFormat guest_format = texture_key.format;
  const FormatInfo* guest_format_info = FormatInfo::Get(guest_format);
  uint32_t block_width = guest_format_info->block_width;
  uint32_t block_height = guest_format_info->block_height;
  uint32_t bytes_per_block = guest_format_info->bytes_per_block();
  if (texture_resolution_scaled) {
    assert_true(bytes_per_block && !(bytes_per_block & (bytes_per_block - 1)) &&
                bytes_per_block <= 16);
    uint32_t bytes_per_block_log2 = rex::log2_floor(bytes_per_block);
    const auto inspect_scaled_layout =
        [&](const char* level_kind, uint32_t start, uint32_t length) {
          const scaled_resolve_util::PageLayoutSummary summary =
              GetScaledResolvePageLayoutSummary(
                  start, length, bytes_per_block_log2);
          if (!summary.mismatching_page_count) {
            return;
          }
          static std::atomic<uint64_t> mismatch_ordinal{0};
          const uint64_t ordinal =
              mismatch_ordinal.fetch_add(1, std::memory_order_relaxed) + 1;
          if (ordinal > 16 && (ordinal & (ordinal - 1))) {
            return;
          }
          std::fprintf(
              stderr,
              "REX_EMBEDDED_SCALED_TEXTURE_LAYOUT_MISMATCH ordinal=%llu "
              "level=%s start=0x%08X length=%u format=%u name=%s "
              "size=%ux%ux%u expected_bpb_log2=%u unscaled_pages=%u "
              "matching_pages=%u mismatch_pages=%u first_page=0x%05X "
              "first_actual_bpb_log2=%u\n",
              static_cast<unsigned long long>(ordinal), level_kind, start,
              length, uint32_t(guest_format), guest_format_info->name, width,
              height, depth_or_array_size, bytes_per_block_log2,
              summary.unscaled_page_count, summary.matching_page_count,
              summary.mismatching_page_count,
              summary.first_mismatching_page,
              summary.first_mismatching_bytes_per_block_log2);
          std::fflush(stderr);
        };
    if (load_base) {
      inspect_scaled_layout("base", texture_key.base_page << 12,
                            d3d12_texture.GetGuestBaseSize());
    }
    if (load_mips) {
      inspect_scaled_layout("mips", texture_key.mip_page << 12,
                            d3d12_texture.GetGuestMipsSize());
    }
    if ((load_base &&
         !InitializeUnscaledResolvePagesFromSharedMemory(
             texture_key.base_page << 12,
             d3d12_texture.GetGuestBaseSize(), bytes_per_block_log2)) ||
        (load_mips &&
         !InitializeUnscaledResolvePagesFromSharedMemory(
             texture_key.mip_page << 12,
             d3d12_texture.GetGuestMipsSize(), bytes_per_block_log2))) {
      return false;
    }
  }
  uint32_t level_first = load_base ? 0 : 1;
  uint32_t level_last = load_mips ? texture_key.mip_max_level : 0;
  assert_true(level_first <= level_last);
  uint32_t level_packed = guest_layout.packed_level;
  uint32_t level_stored_first = std::min(level_first, level_packed);
  uint32_t level_stored_last = std::min(level_last, level_packed);
  uint32_t texture_resolution_scale_x = texture_resolution_scaled ? draw_resolution_scale_x() : 1;
  uint32_t texture_resolution_scale_y = texture_resolution_scaled ? draw_resolution_scale_y() : 1;

  // The loop counter can mean two things depending on whether the packed mip
  // tail is stored as mip 0, because in this case, it would be ambiguous since
  // both the base and the mips would be on "level 0", but stored in separate
  // places.
  uint32_t loop_level_first, loop_level_last;
  if (level_packed == 0) {
    // Packed mip tail is the level 0 - may need to load mip tails for the base,
    // the mips, or both.
    // Loop iteration 0 - base packed mip tail.
    // Loop iteration 1 - mips packed mip tail.
    loop_level_first = uint32_t(level_first != 0);
    loop_level_last = uint32_t(level_last != 0);
  } else {
    // Packed mip tail is not the level 0.
    // Loop iteration is the actual level being loaded.
    loop_level_first = level_stored_first;
    loop_level_last = level_stored_last;
  }

  // Get the host layout and the buffer.
  bool host_block_compressed = host_formats_[uint32_t(guest_format)].is_block_compressed &&
                               !IsDecompressionNeeded(guest_format, width, height);
  uint32_t host_block_width = host_block_compressed ? block_width : 1;
  uint32_t host_block_height = host_block_compressed ? block_height : 1;
  uint32_t host_x_blocks_per_thread = UINT32_C(1)
                                      << load_shader_info.guest_x_blocks_per_thread_log2;
  if (!host_block_compressed) {
    // Decompressing guest blocks.
    host_x_blocks_per_thread *= block_width;
  }
  UINT64 copy_buffer_size = 0;
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT host_slice_layout_base;
  UINT64 host_slice_size_base;
  // Indexing is the same as for guest stored mips:
  // 1...min(level_last, level_packed) if level_packed is not 0, or only 0 if
  // level_packed == 0.
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT
  host_slice_layouts_mips[xenos::kTextureMaxMips];
  UINT64 host_slice_sizes_mips[xenos::kTextureMaxMips];
  // Using custom calculations instead of GetCopyableFootprints because
  // shaders may unconditionally copy multiple blocks along X per thread for
  // simplicity, to make sure all rows (also including the last one -
  // GetCopyableFootprints aligns row offsets, but not the total size) are
  // properly padded to the number of blocks copied in an invocation without
  // implicit assumptions about D3D12_TEXTURE_DATA_PITCH_ALIGNMENT.
  DXGI_FORMAT host_copy_format = GetDXGIResourceFormat(guest_format, width, height);
  for (uint32_t loop_level = loop_level_first; loop_level <= loop_level_last; ++loop_level) {
    bool is_base = loop_level == 0;
    uint32_t level = (level_packed == 0) ? 0 : loop_level;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT& level_host_slice_layout =
        is_base ? host_slice_layout_base : host_slice_layouts_mips[level];
    level_host_slice_layout.Offset = copy_buffer_size;
    level_host_slice_layout.Footprint.Format = host_copy_format;
    if (level == level_packed) {
      // Loading the packed tail for the base or the mips - load the whole tail
      // to copy regions out of it.
      const texture_util::TextureGuestLayout::Level& guest_layout_packed =
          is_base ? guest_layout.base : guest_layout.mips[level];
      level_host_slice_layout.Footprint.Width = guest_layout_packed.x_extent_blocks * block_width;
      level_host_slice_layout.Footprint.Height = guest_layout_packed.y_extent_blocks * block_height;
      level_host_slice_layout.Footprint.Depth = guest_layout_packed.z_extent;
    } else {
      level_host_slice_layout.Footprint.Width = std::max(width >> level, uint32_t(1));
      level_host_slice_layout.Footprint.Height = std::max(height >> level, uint32_t(1));
      level_host_slice_layout.Footprint.Depth = std::max(depth >> level, uint32_t(1));
    }
    level_host_slice_layout.Footprint.Width =
        rex::round_up(level_host_slice_layout.Footprint.Width * texture_resolution_scale_x,
                      UINT(host_block_width));
    level_host_slice_layout.Footprint.Height =
        rex::round_up(level_host_slice_layout.Footprint.Height * texture_resolution_scale_y,
                      UINT(host_block_height));
    level_host_slice_layout.Footprint.RowPitch =
        rex::align(rex::round_up(level_host_slice_layout.Footprint.Width / host_block_width,
                                 host_x_blocks_per_thread) *
                       load_shader_info.bytes_per_host_block,
                   uint32_t(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT));
    UINT64 level_host_slice_size =
        rex::align(UINT64(level_host_slice_layout.Footprint.RowPitch) *
                       (level_host_slice_layout.Footprint.Height / host_block_height) *
                       level_host_slice_layout.Footprint.Depth,
                   UINT64(D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT));
    (is_base ? host_slice_size_base : host_slice_sizes_mips[level]) = level_host_slice_size;
    copy_buffer_size += level_host_slice_size * array_size;
  }
  D3D12_RESOURCE_STATES copy_buffer_state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
  ID3D12Resource* copy_buffer =
      command_processor_.RequestScratchGPUBuffer(uint32_t(copy_buffer_size), copy_buffer_state);
  if (copy_buffer == nullptr) {
    return false;
  }

  // Begin loading.
  // May use different buffers for scaled base and mips, and also addressability
  // of more than 128 * 2^20 (2^D3D12_REQ_BUFFER_RESOURCE_TEXEL_COUNT_2_TO_EXP)
  // texels is not mandatory - need two separate UAV descriptors for base and
  // mips.
  // Destination.
  uint32_t descriptor_count = 1;
  if (texture_resolution_scaled) {
    // Source - base and mips, one or both.
    descriptor_count += (level_first == 0 && level_last != 0) ? 2 : 1;
  } else {
    // Source - shared memory.
    if (!bindless_resources_used_) {
      ++descriptor_count;
    }
  }
  ui::d3d12::util::DescriptorCpuGpuHandlePair descriptors_allocated[3];
  if (!command_processor_.RequestOneUseSingleViewDescriptors(descriptor_count,
                                                             descriptors_allocated)) {
    command_processor_.ReleaseScratchGPUBuffer(copy_buffer, copy_buffer_state);
    return false;
  }
  uint32_t descriptor_write_index = 0;
  command_processor_.SetExternalPipeline(pipeline);
  command_list.D3DSetComputeRootSignature(load_root_signature_.Get());
  // Set up the destination descriptor.
  assert_true(descriptor_write_index < descriptor_count);
  ui::d3d12::util::DescriptorCpuGpuHandlePair descriptor_dest =
      descriptors_allocated[descriptor_write_index++];
  ui::d3d12::util::CreateBufferTypedUAV(
      device, descriptor_dest.first, copy_buffer,
      ui::d3d12::util::GetUintPow2DXGIFormat(load_shader_info.dest_bpe_log2),
      uint32_t(copy_buffer_size) >> load_shader_info.dest_bpe_log2);
  command_list.D3DSetComputeRootDescriptorTable(2, descriptor_dest.second);
  // Set up the unscaled source descriptor (scaled needs two descriptors that
  // depend on the buffer being current, so they will be set later - for mips,
  // after loading the base is done).
  if (!texture_resolution_scaled) {
    D3D12SharedMemory& d3d12_shared_memory = static_cast<D3D12SharedMemory&>(shared_memory());
    d3d12_shared_memory.UseForReading();
    ui::d3d12::util::DescriptorCpuGpuHandlePair descriptor_unscaled_source;
    if (bindless_resources_used_) {
      descriptor_unscaled_source = command_processor_.GetSharedMemoryUintPow2BindlessSRVHandlePair(
          load_shader_info.source_bpe_log2);
    } else {
      assert_true(descriptor_write_index < descriptor_count);
      descriptor_unscaled_source = descriptors_allocated[descriptor_write_index++];
      d3d12_shared_memory.WriteUintPow2SRVDescriptor(descriptor_unscaled_source.first,
                                                     load_shader_info.source_bpe_log2);
    }
    command_list.D3DSetComputeRootDescriptorTable(1, descriptor_unscaled_source.second);
  }

  // Submit the copy buffer population commands.

  auto& cbuffer_pool = command_processor_.GetConstantBufferPool();
  LoadConstants load_constants;
  // 3 bits for each.
  assert_true(texture_resolution_scale_x <= 7);
  assert_true(texture_resolution_scale_y <= 7);
  load_constants.is_tiled_3d_endian_scale =
      uint32_t(texture_key.tiled) | (uint32_t(is_3d_tiling) << 1) |
      (uint32_t(texture_key.endianness) << 2) | (texture_resolution_scale_x << 4) |
      (texture_resolution_scale_y << 7);

  // The loop is slices within levels because the base and the levels may need
  // different portions of the scaled resolve virtual address space to be
  // available through buffers, and to create a descriptor, the buffer start
  // address is required - which may be different for base and mips.
  bool scaled_mips_source_set_up = false;
  uint32_t guest_x_blocks_per_group_log2 = load_shader_info.GetGuestXBlocksPerGroupLog2();
  for (uint32_t loop_level = loop_level_first; loop_level <= loop_level_last; ++loop_level) {
    bool is_base = loop_level == 0;
    uint32_t level = (level_packed == 0) ? 0 : loop_level;

    uint32_t guest_address = (is_base ? texture_key.base_page : texture_key.mip_page) << 12;

    // Set up the base or mips source, also making it accessible if loading from
    // scaled resolve memory.
    if (texture_resolution_scaled && (is_base || !scaled_mips_source_set_up)) {
      uint32_t guest_size_unscaled =
          is_base ? d3d12_texture.GetGuestBaseSize() : d3d12_texture.GetGuestMipsSize();
      if (!MakeScaledResolveRangeCurrent(guest_address, guest_size_unscaled,
                                         load_shader_info.source_bpe_log2)) {
        command_processor_.ReleaseScratchGPUBuffer(copy_buffer, copy_buffer_state);
        return false;
      }
      TransitionCurrentScaledResolveRange(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
      assert_true(descriptor_write_index < descriptor_count);
      ui::d3d12::util::DescriptorCpuGpuHandlePair descriptor_scaled_source =
          descriptors_allocated[descriptor_write_index++];
      CreateCurrentScaledResolveRangeUintPow2SRV(descriptor_scaled_source.first,
                                                 load_shader_info.source_bpe_log2);
      command_list.D3DSetComputeRootDescriptorTable(1, descriptor_scaled_source.second);
      if (!is_base) {
        scaled_mips_source_set_up = true;
      }
    }

    if (texture_resolution_scaled) {
      // Offset already applied in the buffer because more than 512 MB can't be
      // directly addresses as R32 on some hardware (above
      // 2^D3D12_REQ_BUFFER_RESOURCE_TEXEL_COUNT_2_TO_EXP).
      load_constants.guest_offset = 0;
    } else {
      load_constants.guest_offset = guest_address;
    }
    if (!is_base) {
      load_constants.guest_offset += guest_layout.mip_offsets_bytes[level] *
                                     (texture_resolution_scale_x * texture_resolution_scale_y);
    }
    const texture_util::TextureGuestLayout::Level& level_guest_layout =
        is_base ? guest_layout.base : guest_layout.mips[level];
    load_constants.guest_pitch_aligned =
        level_guest_layout.row_pitch_bytes / bytes_per_block;
    load_constants.guest_z_stride_block_rows_aligned = level_guest_layout.z_slice_stride_block_rows;
    assert_true(!is_3d_tiling || !(load_constants.guest_z_stride_block_rows_aligned &
                                   (xenos::kTextureTileWidthHeight - 1)));

    uint32_t level_width, level_height, level_depth;
    if (level == level_packed) {
      // This is the packed mip tail, containing not only the specified level,
      // but also other levels at different offsets - load the entire needed
      // extents.
      level_width = level_guest_layout.x_extent_blocks * block_width;
      level_height = level_guest_layout.y_extent_blocks * block_height;
      level_depth = level_guest_layout.z_extent;
    } else {
      level_width = std::max(width >> level, uint32_t(1));
      level_height = std::max(height >> level, uint32_t(1));
      level_depth = std::max(depth >> level, uint32_t(1));
    }
    load_constants.size_blocks[0] =
        (level_width + (block_width - 1)) / block_width * texture_resolution_scale_x;
    load_constants.size_blocks[1] =
        (level_height + (block_height - 1)) / block_height * texture_resolution_scale_y;
    load_constants.size_blocks[2] = level_depth;
    load_constants.height_texels = level_height;

    uint32_t group_count_x =
        (load_constants.size_blocks[0] + ((UINT32_C(1) << guest_x_blocks_per_group_log2) - 1)) >>
        guest_x_blocks_per_group_log2;
    uint32_t group_count_y =
        (load_constants.size_blocks[1] + ((UINT32_C(1) << kLoadGuestYBlocksPerGroupLog2) - 1)) >>
        kLoadGuestYBlocksPerGroupLog2;

    const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& level_host_slice_layout =
        is_base ? host_slice_layout_base : host_slice_layouts_mips[level];
    uint32_t host_slice_size =
        uint32_t(is_base ? host_slice_size_base : host_slice_sizes_mips[level]);
    load_constants.host_offset = uint32_t(level_host_slice_layout.Offset);
    load_constants.host_pitch = level_host_slice_layout.Footprint.RowPitch;

    command_list.D3DSetComputeRoot32BitConstants(0, sizeof(load_constants) / sizeof(uint32_t),
                                                 &load_constants, 0);

    uint32_t level_array_slice_stride_bytes_scaled =
        level_guest_layout.array_slice_stride_bytes *
        (texture_resolution_scale_x * texture_resolution_scale_y);
    for (uint32_t slice = 0; slice < array_size; ++slice) {
      if (slice != 0) {
        command_list.D3DSetComputeRoot32BitConstants(
            0, sizeof(load_constants.guest_offset) / sizeof(uint32_t), &load_constants.guest_offset,
            offsetof(LoadConstants, guest_offset) / sizeof(uint32_t));
        command_list.D3DSetComputeRoot32BitConstants(
            0, sizeof(load_constants.host_offset) / sizeof(uint32_t), &load_constants.host_offset,
            offsetof(LoadConstants, host_offset) / sizeof(uint32_t));
      }
      assert_true(copy_buffer_state == D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
      command_processor_.SubmitBarriers();
      command_list.D3DDispatch(group_count_x, group_count_y, load_constants.size_blocks[2]);
      load_constants.guest_offset += level_array_slice_stride_bytes_scaled;
      load_constants.host_offset += host_slice_size;
    }
  }

  // Update LRU caching because the texture will be used by the command list.
  d3d12_texture.MarkAsUsed();

  // Submit copying from the copy buffer to the host texture.
  ID3D12Resource* texture_resource = d3d12_texture.resource();
  command_processor_.PushTransitionBarrier(
      texture_resource, d3d12_texture.SetResourceState(D3D12_RESOURCE_STATE_COPY_DEST),
      D3D12_RESOURCE_STATE_COPY_DEST);
  command_processor_.PushTransitionBarrier(copy_buffer, copy_buffer_state,
                                           D3D12_RESOURCE_STATE_COPY_SOURCE);
  copy_buffer_state = D3D12_RESOURCE_STATE_COPY_SOURCE;
  command_processor_.SubmitBarriers();
  uint32_t texture_level_count = texture_key.mip_max_level + 1;
  D3D12_TEXTURE_COPY_LOCATION location_source, location_dest;
  location_source.pResource = copy_buffer;
  location_source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
  location_dest.pResource = texture_resource;
  location_dest.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  for (uint32_t level = level_first; level <= level_last; ++level) {
    uint32_t guest_level = std::min(level, level_packed);
    location_source.PlacedFootprint =
        level ? host_slice_layouts_mips[guest_level] : host_slice_layout_base;
    location_dest.SubresourceIndex = level;
    UINT64 host_slice_size = level ? host_slice_sizes_mips[guest_level] : host_slice_size_base;
    D3D12_BOX source_box;
    const D3D12_BOX* source_box_ptr;
    if (level >= level_packed) {
      uint32_t level_offset_blocks_x, level_offset_blocks_y, level_offset_z;
      texture_util::GetPackedMipOffset(width, height, depth, guest_format, level,
                                       level_offset_blocks_x, level_offset_blocks_y,
                                       level_offset_z);
      source_box.left = level_offset_blocks_x * block_width * texture_resolution_scale_x;
      source_box.top = level_offset_blocks_y * block_height * texture_resolution_scale_y;
      source_box.front = level_offset_z;
      source_box.right =
          source_box.left +
          rex::align(std::max((width * texture_resolution_scale_x) >> level, uint32_t(1)),
                     host_block_width);
      source_box.bottom =
          source_box.top +
          rex::align(std::max((height * texture_resolution_scale_y) >> level, uint32_t(1)),
                     host_block_height);
      source_box.back = source_box.front + std::max(depth >> level, uint32_t(1));
      source_box_ptr = &source_box;
    } else {
      source_box_ptr = nullptr;
    }
    for (uint32_t slice = 0; slice < array_size; ++slice) {
      command_list.D3DCopyTextureRegion(&location_dest, 0, 0, 0, &location_source, source_box_ptr);
      location_dest.SubresourceIndex += texture_level_count;
      location_source.PlacedFootprint.Offset += host_slice_size;
    }
  }

  if (prompt_background_diagnostic && load_base && !prompt_texture_readback_) {
    // Build an independent CPU reference for exactly the base-level transform
    // that the GPU loader has just recorded. For this texture the BC1 block
    // rows occupy the entire 48 KiB footprint, so the hash also covers all
    // bytes copied into the host texture.
    const uint32_t expected_size = uint32_t(host_slice_size_base);
    std::vector<uint8_t> expected(expected_size, 0);
    texture_conversion::UntileInfo untile_info = {};
    untile_info.offset_x = 0;
    untile_info.offset_y = 0;
    untile_info.width = (width + block_width - 1) / block_width;
    untile_info.height = (height + block_height - 1) / block_height;
    untile_info.input_pitch = guest_layout.base.row_pitch_bytes / bytes_per_block;
    untile_info.output_pitch = host_slice_layout_base.Footprint.RowPitch / bytes_per_block;
    untile_info.input_format_info = guest_format_info;
    untile_info.output_format_info = guest_format_info;
    const xenos::Endian texture_endian = texture_key.endianness;
    untile_info.copy_callback = [texture_endian](void* output, const void* input, size_t length) {
      texture_conversion::CopySwapBlock(texture_endian, output, input, length);
    };
    std::vector<uint8_t> guest_source(d3d12_texture.GetGuestBaseSize());
    if (!shared_memory().CopyTextureLifecycleDiagnosticSource(
            texture_key.base_page << 12, uint32_t(guest_source.size()), guest_source.data())) {
      return false;
    }
    texture_conversion::Untile(expected.data(), guest_source.data(), &untile_info);
    prompt_texture_cpu_expected_hash_ =
        HashTextureDiagnosticBytes(expected.data(), expected.size());
    shared_memory().RecordTextureLifecycleDiagnosticEvent(
        SharedMemory::TextureLifecycleDiagnosticEventType::kCpuUntileExpected,
        texture_key.base_page << 12, d3d12_texture.GetGuestBaseSize(),
        prompt_texture_cpu_expected_hash_, expected_size, true);

    const uint32_t resource_offset = rex::align(expected_size, uint32_t(512));
    D3D12_RESOURCE_DESC readback_desc;
    ui::d3d12::util::FillBufferResourceDesc(readback_desc, resource_offset + expected_size,
                                            D3D12_RESOURCE_FLAG_NONE);
    const ui::d3d12::D3D12Provider& provider = command_processor_.GetD3D12Provider();
    Microsoft::WRL::ComPtr<ID3D12Resource> readback;
    if (SUCCEEDED(device->CreateCommittedResource(
            &ui::d3d12::util::kHeapPropertiesReadback, provider.GetHeapFlagCreateNotZeroed(),
            &readback_desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
            IID_PPV_ARGS(&readback)))) {
      command_list.D3DCopyBufferRegion(readback.Get(), 0, copy_buffer, 0, expected_size);

      D3D12_TEXTURE_COPY_LOCATION texture_readback_dest = {};
      texture_readback_dest.pResource = readback.Get();
      texture_readback_dest.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
      texture_readback_dest.PlacedFootprint = host_slice_layout_base;
      texture_readback_dest.PlacedFootprint.Offset = resource_offset;
      D3D12_TEXTURE_COPY_LOCATION texture_readback_source = {};
      texture_readback_source.pResource = texture_resource;
      texture_readback_source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
      texture_readback_source.SubresourceIndex = 0;
      command_processor_.PushTransitionBarrier(
          texture_resource, d3d12_texture.SetResourceState(D3D12_RESOURCE_STATE_COPY_SOURCE),
          D3D12_RESOURCE_STATE_COPY_SOURCE);
      command_processor_.SubmitBarriers();
      command_list.D3DCopyTextureRegion(&texture_readback_dest, 0, 0, 0,
                                        &texture_readback_source, nullptr);

      prompt_texture_readback_ = std::move(readback);
      prompt_texture_readback_submission_ = command_processor_.GetCurrentSubmission();
      prompt_texture_readback_guest_base_ = texture_key.base_page << 12;
      prompt_texture_readback_copy_size_ = expected_size;
      prompt_texture_readback_resource_offset_ = resource_offset;
      shared_memory().RecordTextureLifecycleDiagnosticEvent(
          SharedMemory::TextureLifecycleDiagnosticEventType::kGpuUntileReadbackQueued,
          texture_key.base_page << 12, d3d12_texture.GetGuestBaseSize(),
          prompt_texture_readback_submission_,
          (uint64_t(resource_offset) << 32) | expected_size, true);
    }
  }

  if (texture_readback_diagnostic && load_base &&
      !texture_readback_diagnostic_started_ &&
      host_slice_size_base <= UINT32_MAX) {
    // Capture both representations of one explicitly selected base-level
    // load. The scratch copy proves what the loader produced, while the
    // texture copy proves what the shader-visible D3D12 resource received.
    // This is queued asynchronously and is disabled unless a non-empty guest
    // address interval is supplied.
    const uint32_t row_pitch = host_slice_layout_base.Footprint.RowPitch;
    const uint32_t row_bytes =
        host_slice_layout_base.Footprint.Width / host_block_width *
        load_shader_info.bytes_per_host_block;
    const uint32_t row_count =
        host_slice_layout_base.Footprint.Height / host_block_height *
        host_slice_layout_base.Footprint.Depth;
    const uint64_t payload_size_64 = uint64_t(row_pitch) * row_count;
    if (row_bytes <= row_pitch &&
        payload_size_64 <= UINT32_MAX -
                               (D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - 1)) {
      const uint32_t payload_size = uint32_t(payload_size_64);
      const uint32_t resource_offset =
          rex::align(payload_size,
                     uint32_t(D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT));
      if (resource_offset <= UINT32_MAX - payload_size) {
        D3D12_RESOURCE_DESC readback_desc;
        ui::d3d12::util::FillBufferResourceDesc(
            readback_desc, uint64_t(resource_offset) + payload_size,
            D3D12_RESOURCE_FLAG_NONE);
        const ui::d3d12::D3D12Provider& provider =
            command_processor_.GetD3D12Provider();
        Microsoft::WRL::ComPtr<ID3D12Resource> readback;
        if (SUCCEEDED(device->CreateCommittedResource(
                &ui::d3d12::util::kHeapPropertiesReadback,
                provider.GetHeapFlagCreateNotZeroed(), &readback_desc,
                D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                IID_PPV_ARGS(&readback)))) {
          command_list.D3DCopyBufferRegion(
              readback.Get(), 0, copy_buffer,
              host_slice_layout_base.Offset, payload_size);

          D3D12_TEXTURE_COPY_LOCATION texture_readback_dest = {};
          texture_readback_dest.pResource = readback.Get();
          texture_readback_dest.Type =
              D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
          texture_readback_dest.PlacedFootprint = host_slice_layout_base;
          texture_readback_dest.PlacedFootprint.Offset = resource_offset;
          D3D12_TEXTURE_COPY_LOCATION texture_readback_source = {};
          texture_readback_source.pResource = texture_resource;
          texture_readback_source.Type =
              D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
          texture_readback_source.SubresourceIndex = 0;
          command_processor_.PushTransitionBarrier(
              texture_resource,
              d3d12_texture.SetResourceState(D3D12_RESOURCE_STATE_COPY_SOURCE),
              D3D12_RESOURCE_STATE_COPY_SOURCE);
          command_processor_.SubmitBarriers();
          command_list.D3DCopyTextureRegion(
              &texture_readback_dest, 0, 0, 0,
              &texture_readback_source, nullptr);

          const D3D12_RESOURCE_DESC texture_desc =
              texture_resource->GetDesc();
          texture_readback_ = std::move(readback);
          texture_readback_submission_ =
              command_processor_.GetCurrentSubmission();
          texture_readback_guest_base_ = texture_guest_base;
          texture_readback_guest_size_ = d3d12_texture.GetGuestBaseSize();
          texture_readback_payload_size_ = payload_size;
          texture_readback_resource_offset_ = resource_offset;
          texture_readback_row_pitch_ = row_pitch;
          texture_readback_row_bytes_ = row_bytes;
          texture_readback_row_count_ = row_count;
          texture_readback_width_ =
              host_slice_layout_base.Footprint.Width;
          texture_readback_height_ =
              host_slice_layout_base.Footprint.Height;
          texture_readback_depth_ =
              host_slice_layout_base.Footprint.Depth;
          texture_readback_format_ = uint32_t(texture_desc.Format);
          texture_readback_scale_x_ = texture_resolution_scale_x;
          texture_readback_scale_y_ = texture_resolution_scale_y;
          texture_readback_scaled_resolve_ = texture_resolution_scaled;
          texture_readback_diagnostic_started_ = true;
          texture_readback_armed_address_min_ = 0;
          texture_readback_armed_address_max_ = 0;
          std::fprintf(
              stderr,
              "REX_EMBEDDED_TEXTURE_LOAD_READBACK_QUEUED base=0x%08X "
              "guest_bytes=%u width=%u height=%u depth=%u format=%u "
              "row_pitch=%u row_bytes=%u rows=%u scaled_resolve=%u "
              "scale=%ux%u submission=%llu\n",
              texture_readback_guest_base_, texture_readback_guest_size_,
              texture_readback_width_, texture_readback_height_,
              texture_readback_depth_, texture_readback_format_,
              texture_readback_row_pitch_, texture_readback_row_bytes_,
              texture_readback_row_count_,
              texture_readback_scaled_resolve_ ? 1u : 0u,
              texture_readback_scale_x_, texture_readback_scale_y_,
              static_cast<unsigned long long>(
                  texture_readback_submission_));
          std::fflush(stderr);
        }
      }
    }
  }

  command_processor_.ReleaseScratchGPUBuffer(copy_buffer, copy_buffer_state);

  // Keyboard button prompts: the title's controller icons are recognised
  // when their base level loads (small 2D BC3 textures of the icons' sizes).
  if (load_base && !texture_key.scaled_resolve && texture_key.format == xenos::TextureFormat::k_DXT4_5 &&
      texture_key.dimension == xenos::DataDimension::k2DOrStacked &&
      texture_key.GetDepthOrArraySize() == 1) {
    EnsurePromptIconSet();
    if (prompt_icon_set_.IsCandidateSize(texture_key.GetWidth(), texture_key.GetHeight())) {
      NotePromptIconCandidate(d3d12_texture);
    }
  }

  // A full load (base and every mip): HD texture pack dump and replacement.
  const bool texture_dump = REXCVAR_GET(gpu_texture_dump);
  const bool texture_replace = TextureReplacementEnabled();
  if ((texture_dump || texture_replace) && load_base &&
      (load_mips || texture_key.mip_max_level == 0) && !texture_key.scaled_resolve &&
      texture_key.dimension == xenos::DataDimension::k2DOrStacked &&
      texture_key.GetDepthOrArraySize() == 1) {
    if (texture_replace) {
      EnsurePackIndex();
    }
    // Packs whose entries all name their title texture (guest size and
    // format) need content ids only for textures of those kinds.
    const bool pack_candidate =
        !pack_index_.entries.empty() &&
        (!pack_index_.guest_filter ||
         pack_index_.guest_keys.count(texture_pack::GuestTextureKey(
             texture_key.GetWidth(), texture_key.GetHeight(), uint32_t(texture_key.format))));
    if (texture_dump || pack_candidate) {
      LARGE_INTEGER hash_begin, hash_end;
      QueryPerformanceCounter(&hash_begin);
      const uint64_t id = ComputeTextureContentId(d3d12_texture);
      QueryPerformanceCounter(&hash_end);
      ++pack_hash_textures_frame_;
      pack_hash_bytes_frame_ += uint64_t(d3d12_texture.GetGuestBaseSize()) +
                                (texture_key.mip_max_level ? d3d12_texture.GetGuestMipsSize() : 0);
      pack_hash_ticks_frame_ += uint64_t(hash_end.QuadPart - hash_begin.QuadPart);
      if (id && texture_dump) {
        QueueTextureDump(d3d12_texture, id);
      }
      if (id && texture_replace && pack_candidate && pack_index_.entries.count(id)) {
        OfferTextureForReplacement(d3d12_texture, id);
      }
    }
  }

  if (prompt_background_diagnostic) {
    shared_memory().RecordTextureLifecycleDiagnosticEvent(
        SharedMemory::TextureLifecycleDiagnosticEventType::kD3D12LoadRecorded,
        texture_key.base_page << 12, d3d12_texture.GetGuestBaseSize(),
        uint64_t(reinterpret_cast<uintptr_t>(texture_resource)), uint64_t(copy_buffer_size), true);
  }

  return true;
}

void D3D12TextureCache::TryCompletePromptTextureReadbackDiagnostic() {
  if (!prompt_texture_readback_ || !prompt_texture_readback_submission_ ||
      command_processor_.GetCompletedSubmission() < prompt_texture_readback_submission_) {
    return;
  }
  D3D12_RANGE read_range = {0, size_t(prompt_texture_readback_resource_offset_) +
                                   prompt_texture_readback_copy_size_};
  void* mapping = nullptr;
  if (SUCCEEDED(prompt_texture_readback_->Map(0, &read_range, &mapping))) {
    const uint8_t* bytes = static_cast<const uint8_t*>(mapping);
    const uint32_t scratch_hash =
        HashTextureDiagnosticBytes(bytes, prompt_texture_readback_copy_size_);
    const uint32_t texture_hash = HashTextureDiagnosticBytes(
        bytes + prompt_texture_readback_resource_offset_, prompt_texture_readback_copy_size_);
    // Preserve the two final linear BC1 representations once so they can be
    // decoded independently of both the guest-source decoder and the runtime
    // hash comparison. This is bounded diagnostic evidence only.
    static bool prompt_texture_linear_readbacks_written = false;
    if (!prompt_texture_linear_readbacks_written) {
      prompt_texture_linear_readbacks_written = true;
      char scratch_path[128];
      char texture_path[128];
      std::snprintf(scratch_path, sizeof(scratch_path),
                    "rex_prompt_background_bc1_untile_%08X.bin", scratch_hash);
      std::snprintf(texture_path, sizeof(texture_path),
                    "rex_prompt_background_bc1_resource_%08X.bin", texture_hash);
      const auto write_readback = [this](const char* path, const uint8_t* data) {
        FILE* file = std::fopen(path, "wb");
        if (!file) {
          return false;
        }
        const bool written =
            std::fwrite(data, 1, prompt_texture_readback_copy_size_, file) ==
            prompt_texture_readback_copy_size_;
        std::fclose(file);
        return written;
      };
      const bool scratch_written = write_readback(scratch_path, bytes);
      const bool texture_written = write_readback(
          texture_path, bytes + prompt_texture_readback_resource_offset_);
      std::fprintf(stderr,
                   "REX_EMBEDDED_PROMPT_LINEAR_READBACK scratch=%s written=%u "
                   "resource=%s written=%u bytes=%u\n",
                   scratch_path, scratch_written ? 1u : 0u, texture_path,
                   texture_written ? 1u : 0u, prompt_texture_readback_copy_size_);
      std::fflush(stderr);
    }
    shared_memory().RecordTextureLifecycleDiagnosticEvent(
        SharedMemory::TextureLifecycleDiagnosticEventType::kGpuUntileReadbackReady,
        prompt_texture_readback_guest_base_,
        prompt_texture_readback_copy_size_, scratch_hash, prompt_texture_cpu_expected_hash_, true);
    shared_memory().RecordTextureLifecycleDiagnosticEvent(
        SharedMemory::TextureLifecycleDiagnosticEventType::kHostTextureReadbackReady,
        prompt_texture_readback_guest_base_,
        prompt_texture_readback_copy_size_, texture_hash, scratch_hash, true);
    D3D12_RANGE write_range = {};
    prompt_texture_readback_->Unmap(0, &write_range);
  }
  prompt_texture_readback_.Reset();
  prompt_texture_readback_submission_ = 0;
  prompt_texture_readback_guest_base_ = 0;
  shared_memory().DumpTextureLifecycleDiagnosticEvents();
}

void D3D12TextureCache::TryCompleteTextureReadbackDiagnostic() {
  if (!texture_readback_ || !texture_readback_submission_ ||
      command_processor_.GetCompletedSubmission() <
          texture_readback_submission_) {
    return;
  }
  D3D12_RANGE read_range = {
      0, size_t(texture_readback_resource_offset_) +
             texture_readback_payload_size_};
  void* mapping = nullptr;
  if (SUCCEEDED(texture_readback_->Map(0, &read_range, &mapping))) {
    const uint8_t* bytes = static_cast<const uint8_t*>(mapping);
    const uint8_t* resource_bytes =
        bytes + texture_readback_resource_offset_;
    const uint32_t scratch_hash = HashTextureDiagnosticRows(
        bytes, texture_readback_row_pitch_, texture_readback_row_bytes_,
        texture_readback_row_count_);
    const uint32_t resource_hash = HashTextureDiagnosticRows(
        resource_bytes, texture_readback_row_pitch_,
        texture_readback_row_bytes_, texture_readback_row_count_);

    char scratch_path[192];
    char resource_path[192];
    std::snprintf(
        scratch_path, sizeof(scratch_path),
        "rex_texture_load_scratch_%08X_%ux%u_f%u_s%ux%u_%08X.bin",
        texture_readback_guest_base_, texture_readback_width_,
        texture_readback_height_, texture_readback_format_,
        texture_readback_scale_x_, texture_readback_scale_y_, scratch_hash);
    std::snprintf(
        resource_path, sizeof(resource_path),
        "rex_texture_resource_%08X_%ux%u_f%u_s%ux%u_%08X.bin",
        texture_readback_guest_base_, texture_readback_width_,
        texture_readback_height_, texture_readback_format_,
        texture_readback_scale_x_, texture_readback_scale_y_, resource_hash);
    const auto write_readback =
        [this](const char* path, const uint8_t* data) {
          FILE* file = std::fopen(path, "wb");
          if (!file) {
            return false;
          }
          const bool written =
              std::fwrite(data, 1, texture_readback_payload_size_, file) ==
              texture_readback_payload_size_;
          std::fclose(file);
          return written;
        };
    const bool scratch_written = write_readback(scratch_path, bytes);
    const bool resource_written =
        write_readback(resource_path, resource_bytes);
    std::fprintf(
        stderr,
        "REX_EMBEDDED_TEXTURE_LOAD_READBACK_READY base=0x%08X "
        "guest_bytes=%u width=%u height=%u depth=%u format=%u "
        "row_pitch=%u row_bytes=%u rows=%u scaled_resolve=%u scale=%ux%u "
        "scratch_hash=%08X scratch=%s scratch_written=%u "
        "resource_hash=%08X resource=%s resource_written=%u identical=%u\n",
        texture_readback_guest_base_, texture_readback_guest_size_,
        texture_readback_width_, texture_readback_height_,
        texture_readback_depth_, texture_readback_format_,
        texture_readback_row_pitch_, texture_readback_row_bytes_,
        texture_readback_row_count_,
        texture_readback_scaled_resolve_ ? 1u : 0u,
        texture_readback_scale_x_, texture_readback_scale_y_, scratch_hash,
        scratch_path, scratch_written ? 1u : 0u, resource_hash,
        resource_path, resource_written ? 1u : 0u,
        scratch_hash == resource_hash ? 1u : 0u);
    std::fflush(stderr);
    D3D12_RANGE write_range = {};
    texture_readback_->Unmap(0, &write_range);
  }
  texture_readback_.Reset();
  texture_readback_submission_ = 0;
}

void D3D12TextureCache::TryCompleteActiveTextureReadbackDiagnostics() {
  for (ActiveTextureReadbackDiagnostic& diagnostic :
       active_texture_readback_diagnostics_) {
    if (!diagnostic.readback || !diagnostic.submission ||
        command_processor_.GetCompletedSubmission() < diagnostic.submission) {
      continue;
    }
    // GetCopyableFootprints doesn't include trailing row-pitch padding after
    // the final row in TotalBytes. Keep the Map range within that exact
    // allocation while still copying active bytes from every row.
    const size_t mapped_size =
        size_t(diagnostic.row_pitch) * (diagnostic.row_count - 1) +
        diagnostic.row_bytes;
    D3D12_RANGE read_range = {0, mapped_size};
    void* mapping = nullptr;
    if (SUCCEEDED(diagnostic.readback->Map(0, &read_range, &mapping))) {
      const uint8_t* bytes = static_cast<const uint8_t*>(mapping);
      const uint32_t resource_hash = HashTextureDiagnosticRows(
          bytes, diagnostic.row_pitch, diagnostic.row_bytes,
          diagnostic.row_count);
      if (diagnostic.depth_snapshot) {
        std::fprintf(stderr,
            "REX_TEMPORAL_DEPTH_SNAPSHOT_READY frame=%llu draw=%llu submission=%llu completed=%llu source=0x%016llX snapshot=0x%016llX width=%u height=%u hash=%08X state=COPY_SOURCE scope=encoded_depth_not_sdk_ready\n",
            static_cast<unsigned long long>(diagnostic.snapshot_frame),
            static_cast<unsigned long long>(diagnostic.context_draw_ordinal),
            static_cast<unsigned long long>(diagnostic.submission),
            static_cast<unsigned long long>(command_processor_.GetCompletedSubmission()),
            static_cast<unsigned long long>(diagnostic.resource_identity),
            static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(diagnostic.depth_snapshot.Get())),
            diagnostic.width, diagnostic.height, resource_hash);
      }
      char resource_path[192];
      if (diagnostic.array_size > 1) {
        std::snprintf(
            resource_path, sizeof(resource_path),
            "rex_active_texture_slot%u_%08X_%ux%u_f%u_s%ux%u_a%uof%u_r%016llX_d%llu_%08X.bin",
            diagnostic.fetch_constant_index, diagnostic.guest_base,
            diagnostic.width, diagnostic.height, diagnostic.format,
            diagnostic.scale_x, diagnostic.scale_y, diagnostic.array_slice,
            diagnostic.array_size,
            static_cast<unsigned long long>(diagnostic.resource_identity),
            static_cast<unsigned long long>(diagnostic.context_draw_ordinal),
            resource_hash);
      } else {
        std::snprintf(
            resource_path, sizeof(resource_path),
            "rex_active_texture_slot%u_%08X_%ux%u_f%u_s%ux%u_r%016llX_d%llu_%08X.bin",
            diagnostic.fetch_constant_index, diagnostic.guest_base,
            diagnostic.width, diagnostic.height, diagnostic.format,
            diagnostic.scale_x, diagnostic.scale_y,
            static_cast<unsigned long long>(diagnostic.resource_identity),
            static_cast<unsigned long long>(diagnostic.context_draw_ordinal),
            resource_hash);
      }
      FILE* file = std::fopen(resource_path, "wb");
      bool resource_written = file != nullptr;
      if (file) {
        for (uint32_t row = 0; row < diagnostic.row_count; ++row) {
          if (std::fwrite(bytes + size_t(row) * diagnostic.row_pitch, 1,
                          diagnostic.row_bytes, file) !=
              diagnostic.row_bytes) {
            resource_written = false;
            break;
          }
        }
        std::fclose(file);
      }
      std::fprintf(
          stderr,
          "REX_EMBEDDED_ACTIVE_TEXTURE_READBACK_READY slot=%u "
          "array_slice=%u array_size=%u base=0x%08X guest_bytes=%u "
          "width=%u height=%u depth=%u "
          "format=%u row_pitch=%u row_bytes=%u rows=%u scaled_resolve=%u "
          "scale=%ux%u identity=0x%016llX context_draw=%llu "
          "resource_hash=%08X resource=%s written=%u\n",
          diagnostic.fetch_constant_index, diagnostic.array_slice,
          diagnostic.array_size, diagnostic.guest_base,
          diagnostic.guest_size, diagnostic.width, diagnostic.height,
          diagnostic.depth, diagnostic.format, diagnostic.row_pitch,
          diagnostic.row_bytes, diagnostic.row_count,
          diagnostic.scaled_resolve ? 1u : 0u, diagnostic.scale_x,
          diagnostic.scale_y,
          static_cast<unsigned long long>(diagnostic.resource_identity),
          static_cast<unsigned long long>(diagnostic.context_draw_ordinal),
          resource_hash, resource_path,
          resource_written ? 1u : 0u);
      std::fflush(stderr);
      D3D12_RANGE write_range = {};
      diagnostic.readback->Unmap(0, &write_range);
    }
    diagnostic.readback.Reset();
    diagnostic.depth_snapshot.Reset();
    diagnostic.submission = 0;
  }
}

namespace {

// View format for a pack replacement: its own format read as plain values
// (the title applies its own gamma), signed where D3D12 has a signed twin.
DXGI_FORMAT ReplacementViewFormat(DXGI_FORMAT format, bool is_signed) {
  switch (format) {
    case DXGI_FORMAT_BC4_UNORM:
      return is_signed ? DXGI_FORMAT_BC4_SNORM : format;
    case DXGI_FORMAT_BC5_UNORM:
      return is_signed ? DXGI_FORMAT_BC5_SNORM : format;
    case DXGI_FORMAT_R8G8B8A8_UNORM:
      return is_signed ? DXGI_FORMAT_R8G8B8A8_SNORM : format;
    case DXGI_FORMAT_R8G8_UNORM:
      return is_signed ? DXGI_FORMAT_R8G8_SNORM : format;
    case DXGI_FORMAT_R8_UNORM:
      return is_signed ? DXGI_FORMAT_R8_SNORM : format;
    case DXGI_FORMAT_R16G16B16A16_UNORM:
      return is_signed ? DXGI_FORMAT_R16G16B16A16_SNORM : format;
    default:
      return is_signed ? DXGI_FORMAT_UNKNOWN : format;
  }
}
}  // namespace

uint64_t D3D12TextureCache::ComputeTextureContentId(const D3D12Texture& texture) const {
  const TextureKey key = texture.key();
  const uint32_t base_size = texture.GetGuestBaseSize();
  const uint32_t mips_size = key.mip_max_level && key.mip_page ? texture.GetGuestMipsSize() : 0;
  const uint8_t* base = shared_memory().GuestPhysicalForRead(key.base_page << 12, base_size);
  const uint8_t* mips =
      mips_size ? shared_memory().GuestPhysicalForRead(key.mip_page << 12, mips_size) : nullptr;
  if (!base || (mips_size && !mips)) {
    return 0;
  }
  texture_pack::GuestTextureDesc desc;
  desc.format = uint32_t(key.format);
  desc.dimension = uint32_t(key.dimension);
  desc.width = key.GetWidth();
  desc.height = key.GetHeight();
  desc.depth_or_array_size = key.GetDepthOrArraySize();
  desc.mip_max_level = key.mip_max_level;
  desc.pitch = key.pitch;
  desc.tiled = key.tiled;
  desc.packed_mips = key.packed_mips;
  desc.endianness = uint32_t(key.endianness);
  desc.signed_separate = key.signed_separate;
  return texture_pack::ContentId(desc, base, base_size, mips, mips_size);
}

void D3D12TextureCache::EnsurePackIndex() {
  if (pack_index_built_) {
    return;
  }
  pack_index_built_ = true;
  {
    // Texture creation and the upload buffer fill run on the loader thread.
    const ui::d3d12::D3D12Provider& loader_provider = command_processor_.GetD3D12Provider();
    ID3D12Device* device = loader_provider.GetDevice();
    const D3D12_HEAP_FLAGS heap_flags = loader_provider.GetHeapFlagCreateNotZeroed();
    texture_pack::ReplacementLoader::Get().SetPrepare(
        [device, heap_flags](const texture_pack::DdsImage& image) {
          return PreparePackUpload(device, heap_flags, image);
        });
  }
  const std::filesystem::path root = TexturePackFolder();
  const std::filesystem::path language_root = texture_pack::LanguageFolder();
  {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    pack_index_qpc_ = now.QuadPart;
  }
  // The language pack first: its replacements (translated text) win over an
  // HD pack's version of the same texture.
  pack_index_ = language_root.empty() ? texture_pack::PackIndex()
                                      : texture_pack::BuildPackIndex(language_root);
  const size_t language_entries = pack_index_.entries.size();
  if (HdTexturePacksEnabled()) {
    texture_pack::MergePackIndex(pack_index_, texture_pack::BuildPackIndex(root));
  }
  texture_pack::FinalizePackIndex(pack_index_);
  pack_preload_order_.reserve(pack_index_.entries.size());
  for (const auto& entry : pack_index_.entries) {
    // Overlays need the title's texture first (OfferTextureForReplacement).
    if (!entry.second.overlay) {
      pack_preload_order_.push_back(entry.first);
    }
  }
  uint64_t vram_budget = 0;
  const ui::d3d12::D3D12Provider& provider = command_processor_.GetD3D12Provider();
  Microsoft::WRL::ComPtr<IDXGIFactory4> factory4;
  Microsoft::WRL::ComPtr<IDXGIAdapter3> adapter3;
  if (provider.GetDXGIFactory() &&
      SUCCEEDED(provider.GetDXGIFactory()->QueryInterface(IID_PPV_ARGS(&factory4))) &&
      SUCCEEDED(factory4->EnumAdapterByLuid(provider.GetDevice()->GetAdapterLuid(),
                                            IID_PPV_ARGS(&adapter3)))) {
    DXGI_QUERY_VIDEO_MEMORY_INFO info = {};
    if (SUCCEEDED(adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info))) {
      vram_budget = info.Budget;
    }
  }
  std::fprintf(stderr,
               "REX_TEXTURE_PACK qpc=%lld folder=%s hd=%u language_folder=%s "
               "language_files=%zu files=%zu disk_mb=%.1f vram_mb=%.1f "
               "rejected=%u duplicates=%u vram_budget_mb=%.0f guest_filter=%u kinds=%zu\n",
               static_cast<long long>(pack_index_qpc_), texture_pack::PathText(root).c_str(),
               HdTexturePacksEnabled() ? 1u : 0u,
               language_root.empty() ? "none" : texture_pack::PathText(language_root).c_str(),
               language_entries,
               pack_index_.entries.size(), double(pack_index_.file_bytes) / (1024.0 * 1024.0),
               double(pack_index_.gpu_bytes) / (1024.0 * 1024.0), pack_index_.rejected,
               pack_index_.duplicates, double(vram_budget) / (1024.0 * 1024.0),
               pack_index_.guest_filter ? 1u : 0u, pack_index_.guest_keys.size());
  std::fflush(stderr);
}

void D3D12TextureCache::OfferTextureForReplacement(D3D12Texture& texture, uint64_t id) {
  const auto entry = pack_index_.entries.find(id);
  if (entry == pack_index_.entries.end()) {
    return;
  }
  if (entry->second.overlay) {
    // A language pack's glyphs over the title's own texture: read it back at
    // its first load, then it is replaced like any pack texture.
    PackReplacement& replacement = pack_replacements_[id];
    if (replacement.state == PackReplacement::State::kFailed) {
      return;
    }
    if (replacement.state == PackReplacement::State::kIndexed && !replacement.resource &&
        !replacement.prepared) {
      if (!QueueOverlayReadback(texture, id)) {
        replacement.state = PackReplacement::State::kFailed;
        return;
      }
      replacement.state = PackReplacement::State::kLoading;
      ++pack_loads_in_flight_;
    }
    for (const auto& awaiting : textures_awaiting_replacement_) {
      if (awaiting.first == &texture) {
        return;
      }
    }
    textures_awaiting_replacement_.emplace_back(&texture, id);
    return;
  }
  // Same aspect ratio only: the title samples with normalized coordinates.
  const TextureKey key = texture.key();
  if (uint64_t(entry->second.width) * key.GetHeight() !=
      uint64_t(entry->second.height) * key.GetWidth()) {
    return;
  }
  PackReplacement& replacement = pack_replacements_[id];
  if (replacement.state == PackReplacement::State::kFailed) {
    return;
  }
  if (replacement.state == PackReplacement::State::kIndexed && !replacement.resource) {
    // Wanted now: ahead of the preload order.
    texture_pack::ReplacementLoader::Get().Request(id, entry->second.path);
    replacement.state = PackReplacement::State::kLoading;
    ++pack_loads_in_flight_;
  }
  for (const auto& awaiting : textures_awaiting_replacement_) {
    if (awaiting.first == &texture) {
      return;
    }
  }
  textures_awaiting_replacement_.emplace_back(&texture, id);
}

void D3D12TextureCache::RevertTextureReplacement(D3D12Texture& texture) {
  DeferredTextureRelease release;
  release.submission = command_processor_.GetCurrentSubmission();
  release.resource = texture.RestoreOriginal();
  texture.TakeSRVDescriptors(release.descriptors);
  deferred_texture_releases_.push_back(std::move(release));
  replacement_bindings_dirty_ = true;
  ++textures_reverted_;
}

std::shared_ptr<void> D3D12TextureCache::PreparePackUpload(ID3D12Device* device,
                                                          D3D12_HEAP_FLAGS heap_flags,
                                                          const texture_pack::DdsImage& image) {
  auto prepared = std::make_shared<PreparedPackUpload>();
  LARGE_INTEGER t0, t1, t2, t3;
  QueryPerformanceCounter(&t0);
  D3D12_RESOURCE_DESC desc = {};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  desc.Width = image.width;
  desc.Height = image.height;
  desc.DepthOrArraySize = 1;
  desc.MipLevels = UINT16(image.mip_levels);
  desc.Format = DXGI_FORMAT(image.dxgi_format);
  desc.SampleDesc.Count = 1;
  desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
  desc.Flags = D3D12_RESOURCE_FLAG_NONE;
  if (FAILED(device->CreateCommittedResource(&ui::d3d12::util::kHeapPropertiesDefault, heap_flags,
                                             &desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                             IID_PPV_ARGS(&prepared->texture)))) {
    return nullptr;
  }
  prepared->footprints.resize(image.mip_levels);
  std::vector<UINT> rows(image.mip_levels);
  std::vector<UINT64> row_bytes(image.mip_levels);
  UINT64 total_bytes = 0;
  device->GetCopyableFootprints(&desc, 0, image.mip_levels, 0, prepared->footprints.data(),
                                rows.data(), row_bytes.data(), &total_bytes);
  QueryPerformanceCounter(&t1);
  D3D12_RESOURCE_DESC upload_desc;
  ui::d3d12::util::FillBufferResourceDesc(upload_desc, total_bytes, D3D12_RESOURCE_FLAG_NONE);
  if (FAILED(device->CreateCommittedResource(&ui::d3d12::util::kHeapPropertiesUpload, heap_flags,
                                             &upload_desc, D3D12_RESOURCE_STATE_GENERIC_READ,
                                             nullptr, IID_PPV_ARGS(&prepared->upload)))) {
    return nullptr;
  }
  QueryPerformanceCounter(&t2);
  void* mapping = nullptr;
  const D3D12_RANGE no_read = {};
  if (FAILED(prepared->upload->Map(0, &no_read, &mapping))) {
    return nullptr;
  }
  for (uint32_t level = 0; level < image.mip_levels; ++level) {
    const size_t copy_bytes =
        std::min(size_t(row_bytes[level]), size_t(image.mip_row_bytes[level]));
    const uint32_t copy_rows = std::min(uint32_t(rows[level]), image.mip_rows[level]);
    for (uint32_t row = 0; row < copy_rows; ++row) {
      std::memcpy(static_cast<uint8_t*>(mapping) + prepared->footprints[level].Offset +
                      size_t(row) * prepared->footprints[level].Footprint.RowPitch,
                  image.data.data() + image.mip_offsets[level] +
                      size_t(row) * image.mip_row_bytes[level],
                  copy_bytes);
    }
  }
  prepared->upload->Unmap(0, nullptr);
  prepared->bytes = total_bytes;
  QueryPerformanceCounter(&t3);
  LARGE_INTEGER frequency;
  QueryPerformanceFrequency(&frequency);
  const double to_us = 1e6 / double(frequency.QuadPart);
  std::fprintf(stderr,
               "REX_TEXTURE_PACK_PREPARE qpc=%lld texture_us=%.0f upload_us=%.0f fill_us=%.0f "
               "kb=%llu\n",
               static_cast<long long>(t3.QuadPart), double(t1.QuadPart - t0.QuadPart) * to_us,
               double(t2.QuadPart - t1.QuadPart) * to_us, double(t3.QuadPart - t2.QuadPart) * to_us,
               static_cast<unsigned long long>(total_bytes >> 10));
  return prepared;
}

void D3D12TextureCache::RecordPackUpload(PackReplacement& replacement) {
  PreparedPackUpload& prepared = *replacement.prepared;
  DeferredCommandList& command_list = command_processor_.GetDeferredCommandList();
  for (UINT level = 0; level < UINT(prepared.footprints.size()); ++level) {
    D3D12_TEXTURE_COPY_LOCATION dest = {};
    dest.pResource = prepared.texture.Get();
    dest.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dest.SubresourceIndex = level;
    D3D12_TEXTURE_COPY_LOCATION source = {};
    source.pResource = prepared.upload.Get();
    source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    source.PlacedFootprint = prepared.footprints[level];
    command_list.D3DCopyTextureRegion(&dest, 0, 0, 0, &source, nullptr);
  }
  command_processor_.PushTransitionBarrier(prepared.texture.Get(),
                                           D3D12_RESOURCE_STATE_COPY_DEST,
                                           D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
                                               D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
  command_processor_.SubmitBarriers();
  DeferredTextureRelease release;
  release.submission = command_processor_.GetCurrentSubmission();
  release.resource = std::move(prepared.upload);
  deferred_texture_releases_.push_back(std::move(release));
  replacement.resource = std::move(prepared.texture);
  replacement_bytes_uploaded_ += prepared.bytes;
  replacement.prepared.reset();
  if (pack_uploads_waiting_) {
    --pack_uploads_waiting_;
  }
}

void D3D12TextureCache::UpdatePackReplacements() {
  if (!deferred_texture_releases_.empty()) {
    const uint64_t completed = command_processor_.GetCompletedSubmission();
    for (size_t i = 0; i < deferred_texture_releases_.size();) {
      if (deferred_texture_releases_[i].submission > completed) {
        ++i;
        continue;
      }
      for (uint32_t descriptor : deferred_texture_releases_[i].descriptors) {
        ReleaseTextureDescriptor(descriptor);
      }
      deferred_texture_releases_[i] = std::move(deferred_texture_releases_.back());
      deferred_texture_releases_.pop_back();
    }
  }
  if (!pack_replacements_.empty()) {
    for (auto& finished : texture_pack::ReplacementLoader::Get().TakeFinished()) {
      const auto it = pack_replacements_.find(finished.id);
      if (it == pack_replacements_.end()) {
        continue;
      }
      if (pack_loads_in_flight_) {
        --pack_loads_in_flight_;
      }
      if (!finished.image || !finished.image->mip_levels || !finished.payload) {
        it->second.state = PackReplacement::State::kFailed;
      } else {
        it->second.prepared = std::static_pointer_cast<PreparedPackUpload>(finished.payload);
        ++pack_uploads_waiting_;
      }
    }
  }
  bool swapped = false;
  uint32_t swapped_count = 0;
  uint32_t uploads_count = 0;
  uint64_t upload_bytes = 0;
  // Uploads: any prepared replacement, within the per-frame copy budget (the
  // first one always goes so a large file cannot stall the queue).
  const uint64_t full_budget =
      uint64_t(REXCVAR_GET(gpu_texture_replace_upload_mb_per_frame)) << 20;
  uint64_t budget = full_budget;
  for (auto& replacement_pair : pack_replacements_) {
    PackReplacement& replacement = replacement_pair.second;
    if (replacement.resource || !replacement.prepared) {
      continue;
    }
    const uint64_t bytes = replacement.prepared->bytes;
    if (bytes > budget && budget != full_budget) {
      break;
    }
    budget -= std::min(budget, bytes);
    RecordPackUpload(replacement);
    ++uploads_count;
    upload_bytes += bytes;
  }
  // Preload: keep the loader busy with the rest of the pack.
  while (pack_loads_in_flight_ < kMaxPackLoadsInFlight &&
         pack_preload_next_ < pack_preload_order_.size()) {
    const uint64_t id = pack_preload_order_[pack_preload_next_++];
    PackReplacement& replacement = pack_replacements_[id];
    if (replacement.state != PackReplacement::State::kIndexed || replacement.resource ||
        replacement.prepared) {
      continue;
    }
    const auto entry = pack_index_.entries.find(id);
    if (entry == pack_index_.entries.end()) {
      continue;
    }
    texture_pack::ReplacementLoader::Get().Request(id, entry->second.path);
    replacement.state = PackReplacement::State::kLoading;
    ++pack_loads_in_flight_;
  }
  if (!pack_preload_logged_ && pack_preload_next_ >= pack_preload_order_.size() &&
      !pack_loads_in_flight_ && !pack_uploads_waiting_) {
    // Every pack file is on the GPU: the whole background load, for the
    // honest cost shown in the settings.
    pack_preload_logged_ = true;
    LARGE_INTEGER now, frequency;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&frequency);
    std::fprintf(stderr, "REX_TEXTURE_PACK_PRELOADED qpc=%lld uploaded_mb=%.1f seconds=%.2f\n",
                 static_cast<long long>(now.QuadPart),
                 double(replacement_bytes_uploaded_) / (1024.0 * 1024.0),
                 double(now.QuadPart - pack_index_qpc_) / double(frequency.QuadPart));
    std::fflush(stderr);
  }
  // Swaps: batched, so the views are rebuilt at most every few frames.
  ++pack_frames_since_swap_;
  if (!textures_awaiting_replacement_.empty() &&
      pack_frames_since_swap_ >= kPackSwapIntervalFrames) {
    for (size_t i = 0; i < textures_awaiting_replacement_.size();) {
      D3D12Texture& texture = *textures_awaiting_replacement_[i].first;
      const uint64_t id = textures_awaiting_replacement_[i].second;
      const auto it = pack_replacements_.find(id);
      bool done = it == pack_replacements_.end() ||
                  it->second.state == PackReplacement::State::kFailed || texture.replaced();
      if (!done && it->second.resource) {
        // The guest data may have changed while the file was loading.
        if (ComputeTextureContentId(texture) == id) {
          DeferredTextureRelease release;
          release.submission = command_processor_.GetCurrentSubmission();
          texture.TakeSRVDescriptors(release.descriptors);
          deferred_texture_releases_.push_back(std::move(release));
          texture.ShowReplacement(id, it->second.resource,
                                  D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
                                      D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
          ++textures_replaced_;
          ++swapped_count;
          swapped = true;
        }
        done = true;
      }
      if (done) {
        textures_awaiting_replacement_[i] = textures_awaiting_replacement_.back();
        textures_awaiting_replacement_.pop_back();
      } else {
        ++i;
      }
    }
    if (swapped) {
      pack_frames_since_swap_ = 0;
    }
  }
  if (uploads_count || swapped_count) {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    std::fprintf(stderr,
                 "REX_TEXTURE_PACK_FRAME qpc=%lld uploads=%u upload_kb=%llu swapped=%u "
                 "awaiting=%zu\n",
                 static_cast<long long>(now.QuadPart), uploads_count,
                 static_cast<unsigned long long>(upload_bytes >> 10), swapped_count,
                 textures_awaiting_replacement_.size());
  }
  if (swapped || replacement_bindings_dirty_) {
    replacement_bindings_dirty_ = false;
    ResetTextureBindings();
    if (swapped && (replacement_last_logged_ == 0 ||
                    textures_replaced_ >= replacement_last_logged_ + 64)) {
      replacement_last_logged_ = textures_replaced_;
      std::fprintf(stderr,
                   "REX_TEXTURE_PACK_REPLACED textures=%llu reverted=%llu uploaded_mb=%.1f "
                   "awaiting=%zu\n",
                   static_cast<unsigned long long>(textures_replaced_),
                   static_cast<unsigned long long>(textures_reverted_),
                   double(replacement_bytes_uploaded_) / (1024.0 * 1024.0),
                   textures_awaiting_replacement_.size());
      std::fflush(stderr);
    }
  }
}

void D3D12TextureCache::OnD3D12TextureDestroyed(D3D12Texture& texture) {
  prompt_icon_textures_.erase(&texture);
  for (size_t i = 0; i < textures_awaiting_replacement_.size();) {
    if (textures_awaiting_replacement_[i].first == &texture) {
      textures_awaiting_replacement_[i] = textures_awaiting_replacement_.back();
      textures_awaiting_replacement_.pop_back();
    } else {
      ++i;
    }
  }
}

void D3D12TextureCache::QueueTextureDump(D3D12Texture& texture, uint64_t id) {
  // Render targets (scaled resolves), 3D, cube and array textures are left
  // out in this first version (checked by the caller).
  const TextureKey key = texture.key();
  if (pending_texture_dumps_.size() >= kMaxPendingTextureDumps) {
    ++texture_dumps_skipped_;
    return;
  }
  if (!dumped_texture_ids_.insert(id).second) {
    return;
  }
  std::error_code error;
  if (std::filesystem::exists(
          TextureDumpFolder() / (texture_pack::IdName(id) + ".dds"), error)) {
    return;  // written by an earlier session
  }
  PendingTextureDump dump;
  if (!RecordTextureReadback(texture, id, dump)) {
    dumped_texture_ids_.erase(id);
    return;
  }
  pending_texture_dumps_.push_back(std::move(dump));
}

bool D3D12TextureCache::RecordTextureReadback(D3D12Texture& texture, uint64_t id,
                                              PendingTextureDump& dump) {
  const TextureKey key = texture.key();
  ID3D12Resource* resource = texture.resource();
  const D3D12_RESOURCE_DESC resource_desc = resource->GetDesc();
  if (resource_desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
      resource_desc.DepthOrArraySize != 1) {
    return false;
  }
  const UINT subresources = resource_desc.MipLevels;
  dump.footprints.resize(subresources);
  dump.rows.resize(subresources);
  dump.row_bytes.resize(subresources);
  UINT64 total_bytes = 0;
  ID3D12Device* device = command_processor_.GetD3D12Provider().GetDevice();
  device->GetCopyableFootprints(&resource_desc, 0, subresources, 0, dump.footprints.data(),
                                dump.rows.data(), dump.row_bytes.data(), &total_bytes);
  D3D12_RESOURCE_DESC readback_desc;
  ui::d3d12::util::FillBufferResourceDesc(readback_desc, total_bytes, D3D12_RESOURCE_FLAG_NONE);
  if (FAILED(device->CreateCommittedResource(
          &ui::d3d12::util::kHeapPropertiesReadback,
          command_processor_.GetD3D12Provider().GetHeapFlagCreateNotZeroed(), &readback_desc,
          D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&dump.readback)))) {
    return false;
  }
  command_processor_.PushTransitionBarrier(
      resource, texture.SetResourceState(D3D12_RESOURCE_STATE_COPY_SOURCE),
      D3D12_RESOURCE_STATE_COPY_SOURCE);
  command_processor_.SubmitBarriers();
  DeferredCommandList& command_list = command_processor_.GetDeferredCommandList();
  for (UINT subresource = 0; subresource < subresources; ++subresource) {
    D3D12_TEXTURE_COPY_LOCATION dest = {};
    dest.pResource = dump.readback.Get();
    dest.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dest.PlacedFootprint = dump.footprints[subresource];
    D3D12_TEXTURE_COPY_LOCATION source = {};
    source.pResource = resource;
    source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    source.SubresourceIndex = subresource;
    command_list.D3DCopyTextureRegion(&dest, 0, 0, 0, &source, nullptr);
  }
  dump.submission = command_processor_.GetCurrentSubmission();
  dump.id = id;
  dump.format = resource_desc.Format;
  dump.width = uint32_t(resource_desc.Width);
  dump.height = resource_desc.Height;
  dump.mip_levels = subresources;
  dump.key = key;
  return true;
}

bool D3D12TextureCache::QueueOverlayReadback(D3D12Texture& texture, uint64_t id) {
  PendingTextureDump readback;
  if (!RecordTextureReadback(texture, id, readback)) {
    return false;
  }
  pending_overlays_.push_back(std::move(readback));
  return true;
}

void D3D12TextureCache::ProcessOverlayReadbacks() {
  const uint64_t completed = command_processor_.GetCompletedSubmission();
  for (size_t i = 0; i < pending_overlays_.size();) {
    PendingTextureDump& readback = pending_overlays_[i];
    if (readback.submission > completed) {
      ++i;
      continue;
    }
    const uint64_t id = readback.id;
    std::string error;
    texture_pack::DdsImage overlay;
    const auto entry = pack_index_.entries.find(id);
    uint32_t x = 0, y = 0;
    bool ok = entry != pack_index_.entries.end() &&
              texture_pack::ReadDdsFile(entry->second.path, overlay, error);
    if (ok) {
      x = entry->second.overlay_x;
      y = entry->second.overlay_y;
      // BC3 blocks over a BC3 texture, block-aligned and inside it.
      const bool bc3 = readback.format == DXGI_FORMAT_BC3_TYPELESS ||
                       readback.format == DXGI_FORMAT_BC3_UNORM ||
                       readback.format == DXGI_FORMAT_BC3_UNORM_SRGB;
      if (!bc3 || overlay.dxgi_format != uint32_t(DXGI_FORMAT_BC3_UNORM) || !overlay.mip_levels ||
          (x | y) % 4 || x + overlay.width > readback.width ||
          y + overlay.height > readback.height) {
        error = "the texture or the overlay is not BC3, or the overlay does not fit";
        ok = false;
      }
    }
    auto image = std::make_shared<texture_pack::DdsImage>();
    void* mapping = nullptr;
    if (ok && FAILED(readback.readback->Map(0, nullptr, &mapping))) {
      error = "readback map failed";
      ok = false;
    }
    if (ok) {
      // The title's texture as loaded (every mip), tightly packed.
      image->dxgi_format = uint32_t(DXGI_FORMAT_BC3_UNORM);
      image->width = readback.width;
      image->height = readback.height;
      image->mip_levels = readback.mip_levels;
      size_t size = 0;
      for (uint32_t level = 0; level < readback.mip_levels; ++level) {
        image->mip_offsets.push_back(size);
        image->mip_row_bytes.push_back(uint32_t(readback.row_bytes[level]));
        image->mip_rows.push_back(readback.rows[level]);
        size += size_t(readback.row_bytes[level]) * readback.rows[level];
      }
      image->data.resize(size);
      const auto* bytes = static_cast<const uint8_t*>(mapping);
      for (uint32_t level = 0; level < readback.mip_levels; ++level) {
        const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& footprint = readback.footprints[level];
        for (uint32_t row = 0; row < readback.rows[level]; ++row) {
          std::memcpy(image->data.data() + image->mip_offsets[level] +
                          size_t(row) * image->mip_row_bytes[level],
                      bytes + footprint.Offset + size_t(row) * footprint.Footprint.RowPitch,
                      image->mip_row_bytes[level]);
        }
      }
      const D3D12_RANGE no_write = {};
      readback.readback->Unmap(0, &no_write);
      // The overlay's block rows over mip 0 (16 bytes per 4 x 4 block).
      for (uint32_t row = 0; row < overlay.mip_rows[0]; ++row) {
        std::memcpy(image->data.data() + image->mip_offsets[0] +
                        size_t(y / 4 + row) * image->mip_row_bytes[0] + size_t(x / 4) * 16,
                    overlay.data.data() + overlay.mip_offsets[0] +
                        size_t(row) * overlay.mip_row_bytes[0],
                    overlay.mip_row_bytes[0]);
      }
      std::fprintf(stderr, "REX_TEXTURE_OVERLAY id=%016llx result=1 x=%u y=%u w=%u h=%u\n",
                   static_cast<unsigned long long>(id), x, y, overlay.width, overlay.height);
      texture_pack::ReplacementLoader::Get().Offer(id, std::move(image));
    } else {
      std::fprintf(stderr, "REX_TEXTURE_OVERLAY id=%016llx result=0 reason=%s\n",
                   static_cast<unsigned long long>(id), error.c_str());
      if (const auto replacement = pack_replacements_.find(id);
          replacement != pack_replacements_.end()) {
        replacement->second.state = PackReplacement::State::kFailed;
      }
      if (pack_loads_in_flight_) {
        --pack_loads_in_flight_;
      }
    }
    std::fflush(stderr);
    pending_overlays_[i] = std::move(pending_overlays_.back());
    pending_overlays_.pop_back();
  }
}

void D3D12TextureCache::ProcessTextureDumps() {
  const uint64_t completed = command_processor_.GetCompletedSubmission();
  const std::filesystem::path directory = TextureDumpFolder();
  auto& writer = texture_pack::FileWriter::Get();
  for (size_t i = 0; i < pending_texture_dumps_.size();) {
    PendingTextureDump& dump = pending_texture_dumps_[i];
    if (dump.submission > completed) {
      ++i;
      continue;
    }
    std::vector<uint8_t> data;
    void* mapping = nullptr;
    const D3D12_RANGE read_range = {0, SIZE_T(dump.readback->GetDesc().Width)};
    if (SUCCEEDED(dump.readback->Map(0, &read_range, &mapping))) {
      for (uint32_t level = 0; level < dump.mip_levels; ++level) {
        const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& footprint = dump.footprints[level];
        const size_t row_bytes = size_t(dump.row_bytes[level]);
        const size_t row_count = size_t(dump.rows[level]) * footprint.Footprint.Depth;
        const uint8_t* source = static_cast<const uint8_t*>(mapping) + footprint.Offset;
        for (size_t row = 0; row < row_count; ++row) {
          const uint8_t* row_start = source + row * footprint.Footprint.RowPitch;
          data.insert(data.end(), row_start, row_start + row_bytes);
        }
      }
      const D3D12_RANGE write_range = {};
      dump.readback->Unmap(0, &write_range);
      const std::string name = texture_pack::IdName(dump.id);
      writer.Enqueue(directory / (name + ".dds"),
                     texture_pack::BuildDds2D(uint32_t(dump.format), dump.width, dump.height,
                                              dump.mip_levels, data));
      char json[512];
      const int json_length = std::snprintf(
          json, sizeof(json),
          "{\"id\":\"%s\",\"guest_format\":%u,\"guest_format_name\":\"%s\",\"guest_width\":%u,"
          "\"guest_height\":%u,\"width\":%u,"
          "\"height\":%u,\"mip_levels\":%u,\"dxgi_format\":%u,\"tiled\":%u,\"packed_mips\":%u,"
          "\"endianness\":%u,\"signed_separate\":%u}\n",
          name.c_str(), uint32_t(dump.key.format), FormatInfo::Get(dump.key.format)->name,
          dump.key.GetWidth(), dump.key.GetHeight(),
          dump.width, dump.height, dump.mip_levels, uint32_t(dump.format),
          uint32_t(dump.key.tiled), uint32_t(dump.key.packed_mips),
          uint32_t(dump.key.endianness), uint32_t(dump.key.signed_separate));
      if (json_length > 0) {
        const size_t json_bytes = std::min(size_t(json_length), sizeof(json) - 1);
        writer.Enqueue(directory / (name + ".json"),
                       std::vector<uint8_t>(json, json + json_bytes));
      }
      ++texture_dumps_written_;
      if ((texture_dumps_written_ & 0xFF) == 1) {
        std::fprintf(stderr,
                     "REX_TEXTURE_DUMP textures=%llu skipped=%llu files_written=%llu "
                     "files_dropped=%llu dir=%s\n",
                     static_cast<unsigned long long>(texture_dumps_written_),
                     static_cast<unsigned long long>(texture_dumps_skipped_),
                     static_cast<unsigned long long>(writer.written()),
                     static_cast<unsigned long long>(writer.dropped()),
                     texture_pack::PathText(directory).c_str());
        std::fflush(stderr);
      }
    }
    pending_texture_dumps_[i] = std::move(pending_texture_dumps_.back());
    pending_texture_dumps_.pop_back();
  }
}

void D3D12TextureCache::UpdateTextureBindingsImpl(uint32_t fetch_constant_mask) {
  // Readback diagnostics are queued only in measurement builds.
  if (kGpuDiagnostics) {
    TryCompletePromptTextureReadbackDiagnostic();
    TryCompleteTextureReadbackDiagnostic();
    TryCompleteActiveTextureReadbackDiagnostics();
  }
  uint32_t bindings_remaining = fetch_constant_mask;
  uint32_t binding_index;
  while (rex::bit_scan_forward(bindings_remaining, &binding_index)) {
    bindings_remaining &= ~(UINT32_C(1) << binding_index);
    D3D12TextureBinding& d3d12_binding = d3d12_texture_bindings_[binding_index];
    d3d12_binding.Reset();
    const TextureBinding* binding = GetValidTextureBinding(binding_index);
    if (!binding) {
      continue;
    }
    if (IsSignedVersionSeparateForFormat(binding->key)) {
      if (binding->texture && texture_util::IsAnySignNotSigned(binding->swizzled_signs)) {
        d3d12_binding.descriptor_index =
            FindOrCreateTextureDescriptor(*static_cast<D3D12Texture*>(binding->texture),
                                          binding->key.dimension, false, binding->host_swizzle);
      }
      if (binding->texture_signed && texture_util::IsAnySignSigned(binding->swizzled_signs)) {
        d3d12_binding.descriptor_index_signed =
            FindOrCreateTextureDescriptor(*static_cast<D3D12Texture*>(binding->texture_signed),
                                          binding->key.dimension, true, binding->host_swizzle);
      }
    } else {
      D3D12Texture* texture = static_cast<D3D12Texture*>(binding->texture);
      if (texture) {
        if (texture_util::IsAnySignNotSigned(binding->swizzled_signs)) {
          d3d12_binding.descriptor_index = FindOrCreateTextureDescriptor(
              *texture, binding->key.dimension, false, binding->host_swizzle);
        }
        if (texture_util::IsAnySignSigned(binding->swizzled_signs)) {
          d3d12_binding.descriptor_index_signed = FindOrCreateTextureDescriptor(
              *texture, binding->key.dimension, true, binding->host_swizzle);
        }
      }
    }
  }
}

ID3D12Resource* D3D12TextureCache::D3D12Texture::GetOrCreate3DAs2DResource(
    D3D12_RESOURCE_STATES end_state) {
  if (!REXCVAR_GET(gpu_3d_to_2d_texture)) {
    return nullptr;
  }

  auto& d3d12_cache = static_cast<D3D12TextureCache&>(texture_cache());

  if (texture_3d_as_2d_) {
    d3d12_cache.command_processor_.PushTransitionBarrier(
        texture_3d_as_2d_->resource(), texture_3d_as_2d_->SetResourceState(end_state), end_state);
    return texture_3d_as_2d_->resource();
  }

  const ui::d3d12::D3D12Provider& provider = d3d12_cache.command_processor_.GetD3D12Provider();
  ID3D12Device* device = provider.GetDevice();

  D3D12_RESOURCE_DESC desc = {};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  desc.Alignment = 0;
  desc.Width = key().GetWidth();
  desc.Height = key().GetHeight();
  desc.DepthOrArraySize = 1;
  desc.MipLevels = 1;
  desc.Format = d3d12_cache.GetDXGIResourceFormat(key());
  if (desc.Format == DXGI_FORMAT_UNKNOWN) {
    return nullptr;
  }
  desc.SampleDesc.Count = 1;
  desc.SampleDesc.Quality = 0;
  desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
  desc.Flags = D3D12_RESOURCE_FLAG_NONE;

  D3D12_RESOURCE_STATES initial_state = D3D12_RESOURCE_STATE_COPY_DEST;
  Microsoft::WRL::ComPtr<ID3D12Resource> resource_2d;
  if (FAILED(device->CreateCommittedResource(&ui::d3d12::util::kHeapPropertiesDefault,
                                             provider.GetHeapFlagCreateNotZeroed(), &desc,
                                             initial_state, nullptr, IID_PPV_ARGS(&resource_2d)))) {
    REXGPU_ERROR("D3D12TextureCache: Failed to create 3D-as-2D wrapper resource");
    return nullptr;
  }

  TextureKey key_2d = key();
  key_2d.depth_or_array_size_minus_1 = 0;
  key_2d.mip_max_level = 0;
  texture_3d_as_2d_.reset(
      new D3D12Texture(d3d12_cache, key_2d, resource_2d.Get(), initial_state, false));
  texture_3d_as_2d_->SetForceLoad3DTiling(true);

  if (!d3d12_cache.LoadTextureData(*texture_3d_as_2d_)) {
    REXGPU_ERROR("D3D12TextureCache: Failed to load 3D-as-2D wrapper data");
    texture_3d_as_2d_.reset();
    return nullptr;
  }

  d3d12_cache.command_processor_.PushTransitionBarrier(
      texture_3d_as_2d_->resource(), texture_3d_as_2d_->SetResourceState(end_state), end_state);
  return texture_3d_as_2d_->resource();
}

uint32_t D3D12TextureCache::FindOrCreateTextureDescriptor(D3D12Texture& texture,
                                                          xenos::DataDimension dimension,
                                                          bool is_signed, uint32_t host_swizzle) {
  D3D12Texture::SRVDescriptorKey descriptor_key;
  descriptor_key.key = 0;
  descriptor_key.is_signed = uint32_t(is_signed);
  descriptor_key.host_swizzle = host_swizzle;
  descriptor_key.dimension = uint32_t(dimension);

  // Try to find an existing descriptor.
  uint32_t existing_descriptor_index = texture.GetSRVDescriptorIndex(descriptor_key);
  if (existing_descriptor_index != UINT32_MAX) {
    PROFILE_TEXTURE_CACHE_HIT();
    return existing_descriptor_index;
  }
  PROFILE_TEXTURE_CACHE_MISS();

  TextureKey texture_key = texture.key();

  // Create a new bindless or cached descriptor if supported.
  D3D12_SHADER_RESOURCE_VIEW_DESC desc = {};

  if (IsSignedVersionSeparateForFormat(texture_key) &&
      texture_key.signed_separate != uint32_t(is_signed)) {
    // Not the version with the needed signedness.
    return UINT32_MAX;
  }
  xenos::TextureFormat format = texture_key.format;
  if (is_signed) {
    // Not supporting signed compressed textures - hopefully DXN and DXT5A are
    // not used as signed.
    desc.Format = host_formats_[uint32_t(format)].dxgi_format_signed;
  } else {
    desc.Format = GetDXGIUnormFormat(texture_key);
  }
  if (desc.Format == DXGI_FORMAT_UNKNOWN) {
    unsupported_format_features_used_[uint32_t(format)] |=
        is_signed ? kUnsupportedSnormBit : kUnsupportedUnormBit;
    return UINT32_MAX;
  }

  uint32_t mip_levels = texture_key.mip_max_level + 1;
  if (texture.replaced()) {
    const D3D12_RESOURCE_DESC replacement_desc = texture.resource()->GetDesc();
    desc.Format = ReplacementViewFormat(replacement_desc.Format, is_signed);
    if (desc.Format == DXGI_FORMAT_UNKNOWN) {
      return UINT32_MAX;
    }
    mip_levels = replacement_desc.MipLevels;
  }
  ID3D12Resource* resource_for_view = texture.resource();
  switch (dimension) {
    case xenos::DataDimension::k3D:
      desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
      desc.Texture3D.MostDetailedMip = 0;
      desc.Texture3D.MipLevels = mip_levels;
      desc.Texture3D.ResourceMinLODClamp = 0.0f;
      break;
    case xenos::DataDimension::kCube:
      desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
      desc.TextureCube.MostDetailedMip = 0;
      desc.TextureCube.MipLevels = mip_levels;
      desc.TextureCube.ResourceMinLODClamp = 0.0f;
      break;
    case xenos::DataDimension::k1D:
    case xenos::DataDimension::k2DOrStacked:
      desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
      if (texture_key.dimension == xenos::DataDimension::k3D) {
        resource_for_view =
            texture.GetOrCreate3DAs2DResource(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
                                              D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        if (!resource_for_view) {
          return UINT32_MAX;
        }
        desc.Texture2DArray.MostDetailedMip = 0;
        desc.Texture2DArray.MipLevels = 1;
        desc.Texture2DArray.FirstArraySlice = 0;
        desc.Texture2DArray.ArraySize = 1;
        desc.Texture2DArray.PlaneSlice = 0;
        desc.Texture2DArray.ResourceMinLODClamp = 0.0f;
      } else {
        desc.Texture2DArray.MostDetailedMip = 0;
        desc.Texture2DArray.MipLevels = mip_levels;
        desc.Texture2DArray.FirstArraySlice = 0;
        desc.Texture2DArray.ArraySize = texture_key.GetDepthOrArraySize();
        desc.Texture2DArray.PlaneSlice = 0;
        desc.Texture2DArray.ResourceMinLODClamp = 0.0f;
      }
      break;
    default:
      assert_unhandled_case(dimension);
      return UINT32_MAX;
  }

  desc.Shader4ComponentMapping =
      host_swizzle | D3D12_SHADER_COMPONENT_MAPPING_ALWAYS_SET_BIT_AVOIDING_ZEROMEM_MISTAKES;

  ID3D12Device* device = command_processor_.GetD3D12Provider().GetDevice();
  uint32_t descriptor_index;
  if (bindless_resources_used_) {
    descriptor_index = command_processor_.RequestPersistentViewBindlessDescriptor();
    if (descriptor_index == UINT32_MAX) {
      REXGPU_ERROR(
          "Failed to create a texture descriptor - no free bindless view "
          "descriptors");
      return UINT32_MAX;
    }
  } else {
    if (!srv_descriptor_cache_free_.empty()) {
      descriptor_index = srv_descriptor_cache_free_.back();
      srv_descriptor_cache_free_.pop_back();
    } else {
      // Allocated + 1 (including the descriptor that is being added), rounded
      // up to kSRVDescriptorCachePageSize, (allocated + 1 + size - 1).
      uint32_t cache_pages_needed =
          (srv_descriptor_cache_allocated_ + kSRVDescriptorCachePageSize) /
          kSRVDescriptorCachePageSize;
      if (srv_descriptor_cache_.size() < cache_pages_needed) {
        D3D12_DESCRIPTOR_HEAP_DESC cache_heap_desc;
        cache_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        cache_heap_desc.NumDescriptors = kSRVDescriptorCachePageSize;
        cache_heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        cache_heap_desc.NodeMask = 0;
        while (srv_descriptor_cache_.size() < cache_pages_needed) {
          Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> cache_heap;
          if (FAILED(device->CreateDescriptorHeap(&cache_heap_desc, IID_PPV_ARGS(&cache_heap)))) {
            REXGPU_ERROR(
                "D3D12TextureCache: Failed to create a texture descriptor - "
                "couldn't create a descriptor cache heap");
            return UINT32_MAX;
          }
          srv_descriptor_cache_.emplace_back(cache_heap.Get());
        }
      }
      descriptor_index = srv_descriptor_cache_allocated_++;
    }
  }
  device->CreateShaderResourceView(resource_for_view, &desc,
                                   GetTextureDescriptorCPUHandle(descriptor_index));
  texture.AddSRVDescriptorIndex(descriptor_key, descriptor_index);
  if (kPromptTextureDiagnosticsEnabled &&
      texture_key.format == xenos::TextureFormat::k_DXT1 && texture_key.tiled &&
      texture_key.dimension == xenos::DataDimension::k2DOrStacked &&
      texture_key.GetWidth() == 512 && texture_key.GetHeight() == 191) {
    static bool prompt_descriptor_logged = false;
    if (!prompt_descriptor_logged) {
      prompt_descriptor_logged = true;
      std::fprintf(
          stderr,
          "REX_EMBEDDED_PROMPT_SRV resource=0x%016llX descriptor=0x%08X "
          "format=%u dimension=%u mapping=0x%08X mip_levels=%u first_slice=%u "
          "array_size=%u plane=%u min_lod=%.9g host_swizzle=0x%03X signed=%u\n",
          static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(resource_for_view)),
          descriptor_index, uint32_t(desc.Format), uint32_t(desc.ViewDimension),
          desc.Shader4ComponentMapping, desc.Texture2DArray.MipLevels,
          desc.Texture2DArray.FirstArraySlice, desc.Texture2DArray.ArraySize,
          desc.Texture2DArray.PlaneSlice, desc.Texture2DArray.ResourceMinLODClamp,
          host_swizzle, is_signed ? 1u : 0u);
      std::fflush(stderr);
    }
    shared_memory().RecordTextureLifecycleDiagnosticEvent(
        SharedMemory::TextureLifecycleDiagnosticEventType::kDescriptorCreated,
        texture_key.base_page << 12, texture.GetGuestBaseSize(),
        uint64_t(reinterpret_cast<uintptr_t>(resource_for_view)),
        (uint64_t(descriptor_index) << 32) | uint64_t(desc.Format), true);
  }
  return descriptor_index;
}

void D3D12TextureCache::ReleaseTextureDescriptor(uint32_t descriptor_index) {
  if (bindless_resources_used_) {
    command_processor_.ReleaseViewBindlessDescriptorImmediately(descriptor_index);
  } else {
    srv_descriptor_cache_free_.push_back(descriptor_index);
  }
}

D3D12_CPU_DESCRIPTOR_HANDLE D3D12TextureCache::GetTextureDescriptorCPUHandle(
    uint32_t descriptor_index) const {
  const ui::d3d12::D3D12Provider& provider = command_processor_.GetD3D12Provider();
  if (bindless_resources_used_) {
    return provider.OffsetViewDescriptor(command_processor_.GetViewBindlessHeapCPUStart(),
                                         descriptor_index);
  }
  D3D12_CPU_DESCRIPTOR_HANDLE heap_start =
      srv_descriptor_cache_[descriptor_index / kSRVDescriptorCachePageSize].heap_start();
  uint32_t heap_offset = descriptor_index % kSRVDescriptorCachePageSize;
  return provider.OffsetViewDescriptor(heap_start, heap_offset);
}

xenos::ClampMode D3D12TextureCache::NormalizeClampMode(xenos::ClampMode clamp_mode) const {
  if (clamp_mode == xenos::ClampMode::kClampToHalfway) {
    // No GL_CLAMP (clamp to half edge, half border) equivalent in Direct3D 12,
    // but there's no Direct3D 9 equivalent anyway, and too weird to be suitable
    // for intentional real usage.
    return xenos::ClampMode::kClampToEdge;
  }
  if (clamp_mode == xenos::ClampMode::kMirrorClampToHalfway ||
      clamp_mode == xenos::ClampMode::kMirrorClampToBorder) {
    // No Direct3D 12 equivalents.
    return xenos::ClampMode::kMirrorClampToEdge;
  }
  return clamp_mode;
}


void D3D12TextureCache::EnsurePromptIconSet() {
  if (prompt_icon_set_loaded_) {
    return;
  }
  prompt_icon_set_loaded_ = true;
  const std::string& source = REXCVAR_GET(gpu_prompt_icon_source);
  if (source.empty()) {
    return;
  }
  std::string error;
  const bool loaded = prompt_icon_set_.Load(std::filesystem::u8path(source), error);
  std::fprintf(stderr, "REX_PROMPT_ICONS loaded=%u icons=%zu%s%s\n", loaded ? 1u : 0u,
               prompt_icon_set_.icons().size(), loaded ? "" : " error=",
               loaded ? "" : error.c_str());
  std::fflush(stderr);
}

uint64_t D3D12TextureCache::ComputePromptIconSignature(const D3D12Texture& texture) const {
  const TextureKey key = texture.key();
  const texture_util::TextureGuestLayout::Level& level = texture.guest_layout().base;
  const uint32_t base_size = texture.GetGuestBaseSize();
  const uint8_t* base = shared_memory().GuestPhysicalForRead(key.base_page << 12, base_size);
  if (!base || level.row_pitch_bytes < 16) {
    return 0;
  }
  // BC3: 16-byte blocks of 4x4 texels; the base level untiled into rows and
  // swapped to host byte order like the texture load shaders do.
  const uint32_t blocks_x = (key.GetWidth() + 3) >> 2;
  const uint32_t blocks_y = (key.GetHeight() + 3) >> 2;
  std::vector<uint8_t> blocks(size_t(blocks_x) * blocks_y * 16);
  for (uint32_t by = 0; by < blocks_y; ++by) {
    for (uint32_t bx = 0; bx < blocks_x; ++bx) {
      const int64_t offset =
          key.tiled ? int64_t(texture_util::GetTiledOffset2D(int32_t(bx), int32_t(by),
                                                             level.row_pitch_bytes >> 4, 4))
                    : int64_t(by) * level.row_pitch_bytes + int64_t(bx) * 16;
      if (offset < 0 || uint64_t(offset) + 16 > base_size) {
        return 0;
      }
      uint8_t* out = &blocks[(size_t(by) * blocks_x + bx) * 16];
      std::memcpy(out, base + offset, 16);
      switch (key.endianness) {
        case xenos::Endian::k8in16:
          for (size_t i = 0; i < 16; i += 2) std::swap(out[i], out[i + 1]);
          break;
        case xenos::Endian::k8in32:
          for (size_t i = 0; i < 16; i += 4) {
            std::swap(out[i], out[i + 3]);
            std::swap(out[i + 1], out[i + 2]);
          }
          break;
        case xenos::Endian::k16in32:
          for (size_t i = 0; i < 16; i += 4) {
            std::swap(out[i], out[i + 2]);
            std::swap(out[i + 1], out[i + 3]);
          }
          break;
        default:
          break;
      }
    }
  }
  return prompt_icons::BlockSignature(blocks.data(), blocks.size());
}

void D3D12TextureCache::NotePromptIconCandidate(D3D12Texture& texture) {
  const TextureKey key = texture.key();
  const uint64_t signature = ComputePromptIconSignature(texture);
  const int32_t icon =
      signature ? prompt_icon_set_.Find(key.GetWidth(), key.GetHeight(), signature) : -1;
  if (icon < 0) {
    prompt_icon_textures_.erase(&texture);
    return;
  }
  PromptIconTexture& entry = prompt_icon_textures_[&texture];
  entry.icon = icon;
  // The id a shown keycap carries: a reload keeps it while the guest data is
  // unchanged (LoadTextureDataFromResidentMemoryImpl).
  entry.content_id = ComputeTextureContentId(texture);
  static uint32_t logged = 0;
  if (logged < 64) {
    ++logged;
    std::fprintf(stderr, "REX_PROMPT_ICON name=%s base=0x%08X size=%ux%u tiled=%u endian=%u\n",
                 prompt_icon_set_.icons()[size_t(icon)].name.c_str(), key.base_page << 12,
                 key.GetWidth(), key.GetHeight(), uint32_t(key.tiled), uint32_t(key.endianness));
    std::fflush(stderr);
  }
}

void D3D12TextureCache::UpdatePromptIcons() {
  if (prompt_icon_set_.empty()) {
    return;
  }
  // Keycaps are drawn and their textures created on a worker as soon as the
  // icons are known, and again when the bindings change (labels generation),
  // so the first keyboard input only switches views.
  const uint32_t generation = prompt_icons::LabelsGeneration();
  if (!prompt_keycap_build_.valid() && generation != prompt_keycaps_generation_ &&
      generation != prompt_keycap_failed_generation_) {
    const ui::d3d12::D3D12Provider& provider = command_processor_.GetD3D12Provider();
    ID3D12Device* device = provider.GetDevice();
    const D3D12_HEAP_FLAGS heap_flags = provider.GetHeapFlagCreateNotZeroed();
    std::vector<prompt_icons::Icon> icons = prompt_icon_set_.icons();
    std::vector<prompt_icons::ButtonLabel> labels = prompt_icons::CurrentLabels();
    prompt_keycap_build_generation_ = generation;
    prompt_keycap_build_ = std::async(
        std::launch::async,
        [device, heap_flags, icons = std::move(icons), labels = std::move(labels)]() {
          std::vector<std::shared_ptr<PreparedPackUpload>> prepared;
          for (const prompt_icons::Icon& icon : icons) {
            const texture_pack::DdsImage image = prompt_icons::RenderKeycap(icon, labels);
            auto upload = std::static_pointer_cast<PreparedPackUpload>(
                PreparePackUpload(device, heap_flags, image));
            if (!upload) {
              return std::vector<std::shared_ptr<PreparedPackUpload>>();
            }
            prepared.push_back(std::move(upload));
          }
          return prepared;
        });
  }
  bool views_changed = false;
  if (prompt_keycap_build_.valid() &&
      prompt_keycap_build_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
    std::vector<std::shared_ptr<PreparedPackUpload>> prepared = prompt_keycap_build_.get();
    if (prepared.size() != prompt_icon_set_.icons().size()) {
      prompt_keycap_failed_generation_ = prompt_keycap_build_generation_;
      std::fprintf(stderr, "REX_PROMPT_KEYCAPS built=0 generation=%08X\n",
                   prompt_keycap_build_generation_);
      std::fflush(stderr);
    } else {
      DeferredCommandList& command_list = command_processor_.GetDeferredCommandList();
      std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> keycaps;
      for (const std::shared_ptr<PreparedPackUpload>& upload : prepared) {
        for (UINT level = 0; level < UINT(upload->footprints.size()); ++level) {
          D3D12_TEXTURE_COPY_LOCATION dest = {};
          dest.pResource = upload->texture.Get();
          dest.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
          dest.SubresourceIndex = level;
          D3D12_TEXTURE_COPY_LOCATION source = {};
          source.pResource = upload->upload.Get();
          source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
          source.PlacedFootprint = upload->footprints[level];
          command_list.D3DCopyTextureRegion(&dest, 0, 0, 0, &source, nullptr);
        }
        command_processor_.PushTransitionBarrier(
            upload->texture.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        DeferredTextureRelease release;
        release.submission = command_processor_.GetCurrentSubmission();
        release.resource = std::move(upload->upload);
        deferred_texture_releases_.push_back(std::move(release));
        keycaps.push_back(std::move(upload->texture));
      }
      command_processor_.SubmitBarriers();
      // Textures showing the previous keycaps take the new ones below.
      for (auto& [texture, entry] : prompt_icon_textures_) {
        if (entry.keycap && texture->replaced()) {
          DeferredTextureRelease release;
          release.submission = command_processor_.GetCurrentSubmission();
          release.resource = texture->RestoreOriginal();
          texture->TakeSRVDescriptors(release.descriptors);
          deferred_texture_releases_.push_back(std::move(release));
          views_changed = true;
        }
        entry.keycap = false;
      }
      prompt_keycaps_ = std::move(keycaps);
      prompt_keycaps_generation_ = prompt_keycap_build_generation_;
      std::fprintf(stderr, "REX_PROMPT_KEYCAPS built=%zu generation=%08X\n", prompt_keycaps_.size(),
                   prompt_keycaps_generation_);
      std::fflush(stderr);
    }
  }
  const bool keyboard = !prompt_keycaps_.empty() && prompt_icons::KeyboardPromptsWanted();
  uint32_t switched = 0;
  for (auto& [texture, entry] : prompt_icon_textures_) {
    if (entry.keycap && !texture->replaced()) {
      // Reverted by a reload (its guest data changed).
      entry.keycap = false;
    }
    const bool want = keyboard && size_t(entry.icon) < prompt_keycaps_.size();
    if (want && !entry.keycap && !texture->replaced()) {
      DeferredTextureRelease release;
      release.submission = command_processor_.GetCurrentSubmission();
      texture->TakeSRVDescriptors(release.descriptors);
      deferred_texture_releases_.push_back(std::move(release));
      texture->ShowReplacement(entry.content_id, prompt_keycaps_[size_t(entry.icon)],
                               D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
                                   D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
      entry.keycap = true;
      ++switched;
    } else if (!want && entry.keycap) {
      DeferredTextureRelease release;
      release.submission = command_processor_.GetCurrentSubmission();
      release.resource = texture->RestoreOriginal();
      texture->TakeSRVDescriptors(release.descriptors);
      deferred_texture_releases_.push_back(std::move(release));
      entry.keycap = false;
      ++switched;
    }
  }
  if (switched || views_changed) {
    replacement_bindings_dirty_ = true;
  }
  if (keyboard != prompt_keyboard_shown_ || switched) {
    std::fprintf(stderr, "REX_PROMPT_MODE keyboard=%u icons=%zu switched=%u\n", keyboard ? 1u : 0u,
                 prompt_icon_textures_.size(), switched);
    std::fflush(stderr);
  }
  prompt_keyboard_shown_ = keyboard;
}

}  // namespace rex::graphics::d3d12
