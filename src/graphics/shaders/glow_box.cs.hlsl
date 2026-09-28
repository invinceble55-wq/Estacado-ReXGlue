// Glow reconstruction with dedicated images (#16, graphics_glow_reconstruction
// "dedicated"): box-reduces every native cell of a resolution-scaled texture
// (scale_x * scale_y host texels) into a native-size image. With a clamp
// rectangle, the cells outside it repeat its edge cells, so filtering the image
// never blends texels from beyond it (the :region= of an image filter rule).
// scripts/build-debug-shaders.ps1 (FXC) -> bytecode/d3d12_5_1/glow_box_cs.h

cbuffer XeGlowConstants : register(b0) {
  uint2 xe_glow_native_size;
  uint2 xe_glow_scale;
  uint2 xe_glow_host_size;
  uint xe_glow_rect_count;
  uint xe_glow_flags;
  // Native texels [left, top, right, bottom); xe_glow_rects[0] is the clamp
  // rectangle here when bit 0 of xe_glow_flags is set.
  int4 xe_glow_rects[4];
};

Texture2DArray<float4> xe_glow_source : register(t0);
RWTexture2D<float4> xe_glow_dest : register(u0);

[numthreads(8, 8, 1)]
void main(uint3 xe_thread_id : SV_DispatchThreadID) {
  [branch] if (any(xe_thread_id.xy >= xe_glow_native_size)) {
    return;
  }
  int2 cell = int2(xe_thread_id.xy);
  [branch] if (xe_glow_flags & 1u) {
    cell = clamp(cell, xe_glow_rects[0].xy, xe_glow_rects[0].zw - 1);
  }
  int2 origin = cell * int2(xe_glow_scale);
  float4 sum = float4(0.0, 0.0, 0.0, 0.0);
  [loop] for (uint y = 0u; y < xe_glow_scale.y; ++y) {
    [loop] for (uint x = 0u; x < xe_glow_scale.x; ++x) {
      sum += xe_glow_source.Load(int4(origin + int2(int(x), int(y)), 0, 0));
    }
  }
  xe_glow_dest[xe_thread_id.xy] = sum / float(xe_glow_scale.x * xe_glow_scale.y);
}
