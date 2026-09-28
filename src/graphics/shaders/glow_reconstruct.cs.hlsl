// Glow reconstruction with dedicated images (#16, graphics_glow_reconstruction
// diagnostics "dedicated_reconstructed" and "dedicated_scaled"; Test A,
// "dedicated", samples the native cells): the image a scaled-output draw
// samples instead of a resolution-scaled texture. Each host texel center gets
// the native bilinear
// of the box-reduced cells (glow_box_cs, unclamped), exactly what the
// reconstruction shader variant computes for a sample there:
// - xe_glow_rect_count 0: everywhere, clamped to the texture;
// - bit 0 of xe_glow_flags: everywhere, clamped to xe_glow_rects[0] (:region=);
// - otherwise (:source=native): inside the tracked native rectangles (oldest
//   first, the newest containing one wins), clamped to that rectangle; texels
//   outside every rectangle keep the scaled texture's own value.
// Filtering a sample just inside a rectangle also reads the host texels one
// texel outside it, where the variant never blends anything beyond the
// rectangle, so a border one host texel wide gets the rectangle's clamped
// reconstruction too: around a rectangle outside every other one, and where a
// newer rectangle lies inside an older one it overlaps (a smaller glow image
// written over a larger one; a stretched small image would otherwise pick up
// the larger image's middle). Rectangles that only touch keep their own
// interiors.
// scripts/build-debug-shaders.ps1 (FXC) -> bytecode/d3d12_5_1/glow_reconstruct_cs.h

cbuffer XeGlowConstants : register(b0) {
  uint2 xe_glow_native_size;
  uint2 xe_glow_scale;
  uint2 xe_glow_host_size;
  uint xe_glow_rect_count;
  uint xe_glow_flags;
  int4 xe_glow_rects[4];
};

Texture2DArray<float4> xe_glow_source : register(t0);
Texture2D<float4> xe_glow_cells : register(t1);
RWTexture2D<float4> xe_glow_dest : register(u0);

// A box-reduced native cell: from the cells image, or (bit 2 of xe_glow_flags,
// d3d12_debug_glow_image_direct) straight from the scaled texture.
float4 XeGlowCell(int2 cell) {
  [branch] if (xe_glow_flags & 4u) {
    int2 origin = cell * int2(xe_glow_scale);
    float4 sum = float4(0.0, 0.0, 0.0, 0.0);
    [loop] for (uint y = 0u; y < xe_glow_scale.y; ++y) {
      [loop] for (uint x = 0u; x < xe_glow_scale.x; ++x) {
        sum += xe_glow_source.Load(int4(origin + int2(int(x), int(y)), 0, 0));
      }
    }
    return sum / float(xe_glow_scale.x * xe_glow_scale.y);
  }
  return xe_glow_cells.Load(int3(cell, 0));
}

bool XeGlowContains(int4 rect, float2 position, float2 margin) {
  return all(position >= float2(rect.xy) - margin) && all(position < float2(rect.zw) + margin);
}

bool XeGlowOverlap(int4 a, int4 b) {
  return all(max(a.xy, b.xy) < min(a.zw, b.zw));
}

[numthreads(8, 8, 1)]
void main(uint3 xe_thread_id : SV_DispatchThreadID) {
  [branch] if (any(xe_thread_id.xy >= xe_glow_host_size)) {
    return;
  }
  [branch] if (xe_glow_flags & 2u) {
    // Diagnostics (d3d12_debug_glow_image_copy): an exact copy of the texture.
    xe_glow_dest[xe_thread_id.xy] = xe_glow_source.Load(int4(int2(xe_thread_id.xy), 0, 0));
    return;
  }
  // The native position of this host texel's center.
  float2 position = (float2(xe_thread_id.xy) + 0.5) / float2(xe_glow_scale);
  int4 rect = int4(0, 0, int2(xe_glow_native_size));
  bool inside = true;
  [branch] if (xe_glow_flags & 1u) {
    rect = xe_glow_rects[0];
  } else if (xe_glow_rect_count != 0u) {
    float2 border = 1.0 / float2(xe_glow_scale);
    // The newest rectangle containing the texel.
    int interior = -1;
    [loop] for (uint i = 0u; i < xe_glow_rect_count; ++i) {
      [flatten] if (XeGlowContains(xe_glow_rects[i], position, float2(0.0, 0.0))) {
        interior = int(i);
      }
    }
    // The newest bordering rectangle that may take the texel: any one outside
    // every rectangle, otherwise a newer one overlapping the containing one.
    int chosen = interior;
    [loop] for (uint j = 0u; j < xe_glow_rect_count; ++j) {
      [flatten] if (int(j) > interior && XeGlowContains(xe_glow_rects[j], position, border) &&
                    (interior < 0 ||
                     XeGlowOverlap(xe_glow_rects[j], xe_glow_rects[uint(max(interior, 0))]))) {
        chosen = int(j);
      }
    }
    inside = chosen >= 0;
    [flatten] if (inside) {
      rect = xe_glow_rects[uint(max(chosen, 0))];
    }
  }
  [branch] if (!inside) {
    xe_glow_dest[xe_thread_id.xy] = xe_glow_source.Load(int4(int2(xe_thread_id.xy), 0, 0));
    return;
  }
  float2 lower = position - 0.5;
  float2 fraction = frac(lower);
  int2 low_bound = max(rect.xy, int2(0, 0));
  int2 high_bound = min(rect.zw, int2(xe_glow_native_size)) - 1;
  int2 cell0 = clamp(int2(floor(lower)), low_bound, high_bound);
  int2 cell1 = clamp(int2(floor(lower)) + 1, low_bound, high_bound);
  float4 top = lerp(XeGlowCell(int2(cell0.x, cell0.y)), XeGlowCell(int2(cell1.x, cell0.y)),
                    fraction.x);
  float4 bottom = lerp(XeGlowCell(int2(cell0.x, cell1.y)), XeGlowCell(int2(cell1.x, cell1.y)),
                       fraction.x);
  xe_glow_dest[xe_thread_id.xy] = lerp(top, bottom, fraction.y);
}
