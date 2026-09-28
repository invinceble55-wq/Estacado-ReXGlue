// Temporal upscaling of the finished frame (#3, #23; command_processor_
// temporal_upscale.cpp). The title's frame right after its final composite
// (HUD-less) is upscaled by DLSS / FSR 3.1 / XeSS; the HUD and subtitles,
// drawn after the composite, are taken from the finished frame wherever it
// differs from the HUD-less one and filtered spatially, so the temporal
// upscaler never smears them. Same root signature as temporal_aa.cs.hlsl.
// Compiled by scripts/build-temporal-aa-shaders.ps1 (entry points prepare and
// composite) into bytecode/d3d12_5_1/temporal_upscale_{prepare,composite}_cs.h.

cbuffer TemporalUpscaleConstants : register(b0) {
  uint2 in_size;
  uint2 out_size;
  float2 in_size_inv;
  float2 out_size_inv;
  // Per-channel difference above which a pixel belongs to the HUD.
  float hud_threshold;
  uint flags;
  float2 padding0;
  float4 padding1[3];
};

Texture2D<float4> hudless : register(t0);   // render size (after the composite)
Texture2D<float4> finished : register(t1);  // render size (with the HUD)
Texture2D<float4> upscaled : register(t2);  // output size (the upscaler's result)
RWTexture2D<float4> out_color : register(u0);         // prepare: render size RGBA16F
RWTexture2D<unorm float4> out_frame : register(u1);   // composite: output size
SamplerState linear_clamp : register(s0);

[numthreads(8, 8, 1)]
void prepare(uint3 id : SV_DispatchThreadID) {
  if (any(id.xy >= in_size)) return;
  float3 color = saturate(hudless[id.xy].rgb);
  // Flag 16 (VR comfort): darken the edges, strength padding0.x.
  if ((flags & 16u) != 0u) {
    float2 centred = (float2(id.xy) + 0.5) * in_size_inv * 2.0 - 1.0;
    float radius = length(centred * float2(1.0, 0.75));
    color *= 1.0 - padding0.x * smoothstep(0.45, 1.0, radius);
  }
  out_color[id.xy] = float4(color, 1.0);
}

float Difference(int2 pixel) {
  int2 q = clamp(pixel, int2(0, 0), int2(in_size) - 1);
  float3 d = abs(finished[q].rgb - hudless[q].rgb);
  return max(d.r, max(d.g, d.b));
}

[numthreads(8, 8, 1)]
void composite(uint3 id : SV_DispatchThreadID) {
  if (any(id.xy >= out_size)) return;
  float2 uv = (float2(id.xy) + 0.5) * out_size_inv;
  // The render pixels this output pixel's bilinear footprint touches, and one
  // more around them so the HUD's soft edges come along.
  int2 base = int2(floor(uv * float2(in_size) - 0.5));
  float difference = 0.0;
  [unroll] for (int y = -1; y <= 2; ++y) {
    [unroll] for (int x = -1; x <= 2; ++x) {
      difference = max(difference, Difference(base + int2(x, y)));
    }
  }
  float hud = saturate((difference - hud_threshold) * 16.0);
  float3 scene = upscaled[id.xy].rgb;
  float3 overlay = finished.SampleLevel(linear_clamp, uv, 0).rgb;
  // Flag 1: show the HUD mask in red (test).
  if ((flags & 1u) != 0u) overlay = float3(1.0, 0.0, 0.0);
  out_frame[id.xy] = float4(saturate(lerp(scene, overlay, hud)), 1.0);
}

// VR (experimental, display_vr / display_vr_simulate; openxr_screen.h): the
// HUD layer is the finished frame where it differs from the HUD-less one
// (alpha = that mask), shown on a plane in front of the player while the eyes
// see the HUD-less scene. Render size (in_size = out_size).
[numthreads(8, 8, 1)]
void vr_hud(uint3 id : SV_DispatchThreadID) {
  if (any(id.xy >= in_size)) return;
  float difference = 0.0;
  [unroll] for (int y = -1; y <= 1; ++y) {
    [unroll] for (int x = -1; x <= 1; ++x) {
      difference = max(difference, Difference(int2(id.xy) + int2(x, y)));
    }
  }
  float hud = saturate((difference - hud_threshold) * 16.0);
  out_frame[id.xy] = float4(saturate(finished[id.xy].rgb), hud);
}

// The monitor preview of the two eye images (no headset): t0 left eye, t1
// right eye (render size), t2 the HUD layer (render size, alpha). Flag 4:
// red-cyan anaglyph; flag 8: side by side (each eye squeezed to half width).
Texture2D<float4> hud_layer : register(t2);
[numthreads(8, 8, 1)]
void vr_preview(uint3 id : SV_DispatchThreadID) {
  if (any(id.xy >= out_size)) return;
  float2 uv = (float2(id.xy) + 0.5) * out_size_inv;
  float3 color;
  float4 hud;
  if ((flags & 8u) != 0u) {
    bool right = uv.x >= 0.5;
    float2 eye_uv = float2(frac(uv.x * 2.0), uv.y);
    color = right ? finished.SampleLevel(linear_clamp, eye_uv, 0).rgb
                  : hudless.SampleLevel(linear_clamp, eye_uv, 0).rgb;
    hud = hud_layer.SampleLevel(linear_clamp, eye_uv, 0);
  } else {
    float3 left_color = hudless.SampleLevel(linear_clamp, uv, 0).rgb;
    float3 right_color = finished.SampleLevel(linear_clamp, uv, 0).rgb;
    // Dubois-free simple anaglyph: red from the left eye's luminance-weighted
    // red, green and blue from the right eye.
    color = float3(dot(left_color, float3(0.299, 0.587, 0.114)), right_color.g, right_color.b);
    hud = hud_layer.SampleLevel(linear_clamp, uv, 0);
  }
  out_frame[id.xy] = float4(saturate(lerp(color, hud.rgb, hud.a)), 1.0);
}
