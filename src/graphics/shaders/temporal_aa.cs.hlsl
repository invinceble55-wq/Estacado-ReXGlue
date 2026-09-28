// Temporal anti-aliasing resolve for The Darkness (rex/graphics/
// temporal_aa_policy.h). Runs right before the title's final composite reads
// its scene colour: reprojects the previous output with the resolved scene
// depth and both frames' cameras (the CPU passes one matrix that maps the
// current NDC to the previous clip position), clips it to the current
// neighbourhood and blends. Compiled by scripts/build-temporal-aa-shaders.ps1
// into bytecode/d3d12_5_1/temporal_aa_resolve_cs.h.
//
// Scene colour is the title's R16G16B16A16 UNORM target sampled with a 2^4
// exponent (linear HDR / 16); the resolve works in that encoding and writes
// the same encoding back, keeping the title's alpha.

cbuffer TemporalAaConstants : register(b0) {
  float4 reproject_row_0;
  float4 reproject_row_1;
  float4 reproject_row_2;
  float4 reproject_row_3;
  uint2 size;
  float2 size_inv;
  // Sub-pixel offset the current frame's scene draws were rasterized with.
  float2 jitter;
  // Weight of the history when it is valid (0 = no history).
  float history_weight;
  uint flags;
};

Texture2D<float4> current_color : register(t0);
Texture2D<float> current_depth : register(t1);
Texture2D<float4> history_color : register(t2);
RWTexture2D<float4> out_history : register(u0);
RWTexture2D<unorm float4> out_color : register(u1);
// Upscaler input colour (R16G16B16A16_FLOAT, linear: the scene colour x 16).
RWTexture2D<float4> out_upscaler_color : register(u2);
SamplerState linear_clamp : register(s0);

static const float kColorScale = 16.0;  // the title's 2^4 exponent
static const float kClipGamma = 1.25;

float3 RgbToYCoCg(float3 c) {
  return float3(dot(c, float3(0.25, 0.5, 0.25)), dot(c, float3(0.5, 0.0, -0.5)),
                dot(c, float3(-0.25, 0.5, -0.25)));
}

float3 YCoCgToRgb(float3 c) {
  return float3(c.x + c.y - c.z, c.x + c.z, c.x - c.y - c.z);
}

// 5-tap Catmull-Rom (corner taps dropped), sharper than bilinear history.
float3 SampleHistory(float2 uv) {
  float2 position = uv * float2(size);
  float2 center = floor(position - 0.5) + 0.5;
  float2 f = position - center;
  float2 f2 = f * f;
  float2 f3 = f2 * f;
  float2 w0 = -0.5 * f3 + f2 - 0.5 * f;
  float2 w1 = 1.5 * f3 - 2.5 * f2 + 1.0;
  float2 w2 = -1.5 * f3 + 2.0 * f2 + 0.5 * f;
  float2 w3 = 0.5 * f3 - 0.5 * f2;
  float2 w12 = w1 + w2;
  float2 tc12 = (center + w2 / w12) * size_inv;
  float2 tc0 = (center - 1.0) * size_inv;
  float2 tc3 = (center + 2.0) * size_inv;
  float4 sum =
      float4(history_color.SampleLevel(linear_clamp, float2(tc12.x, tc0.y), 0).rgb, 1.0) *
          (w12.x * w0.y) +
      float4(history_color.SampleLevel(linear_clamp, float2(tc0.x, tc12.y), 0).rgb, 1.0) *
          (w0.x * w12.y) +
      float4(history_color.SampleLevel(linear_clamp, float2(tc12.x, tc12.y), 0).rgb, 1.0) *
          (w12.x * w12.y) +
      float4(history_color.SampleLevel(linear_clamp, float2(tc3.x, tc12.y), 0).rgb, 1.0) *
          (w3.x * w12.y) +
      float4(history_color.SampleLevel(linear_clamp, float2(tc12.x, tc3.y), 0).rgb, 1.0) *
          (w12.x * w3.y);
  return max(sum.rgb / max(sum.a, 1e-4), 0.0);
}

float Luma(float3 c) { return dot(c, float3(0.299, 0.587, 0.114)) * kColorScale; }

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  if (any(id.xy >= size)) {
    return;
  }
  int2 pixel = int2(id.xy);
  int2 last = int2(size) - 1;

  if ((flags & 4u) != 0u) {
    // After an upscaler (DLSS/FSR/XeSS): its linear output (history_color)
    // back to the title's encoding, keeping the scene colour's alpha.
    float3 upscaled = max(history_color[pixel].rgb, 0.0);
    // Linear input (flag 8): back to the title's square-root encoding.
    if ((flags & 8u) != 0u) upscaled = sqrt(upscaled);
    upscaled /= kColorScale;
    out_color[pixel] = float4(saturate(upscaled), current_color[pixel].a);
    return;
  }

  if ((flags & 2u) != 0u) {
    // Upscaler inputs: linear colour and motion vectors (out_history is the
    // R16G16_FLOAT motion target): from the unjittered position of the
    // surface seen at this pixel to where it was in the previous frame, in
    // pixels (+y down; jitter not included), as DLSS, FSR and XeSS expect.
    // The title's scene colour is square-root encoded (its final composite
    // squares it before tone mapping): flag 8 hands the upscaler linear light.
    // Flag 16: motion vectors only (Frame Generation inputs), no colour.
    if ((flags & 16u) == 0u) {
      float3 scene = current_color[pixel].rgb * kColorScale;
      if ((flags & 8u) != 0u) scene *= scene;
      out_upscaler_color[pixel] = float4(scene, 1.0);
    }
    float2 position = float2(pixel) + 0.5 - jitter;
    float4 ndc = float4(position.x * 2.0 * size_inv.x - 1.0, 1.0 - position.y * 2.0 * size_inv.y,
                        1.0 - current_depth[pixel], 1.0);
    float4 previous_clip = float4(dot(reproject_row_0, ndc), dot(reproject_row_1, ndc),
                                  dot(reproject_row_2, ndc), dot(reproject_row_3, ndc));
    float2 motion = 0.0;
    if ((flags & 1u) != 0u && previous_clip.w > 1e-6) {
      float2 previous_ndc = previous_clip.xy / previous_clip.w;
      motion = float2(previous_ndc.x * 0.5 + 0.5, 0.5 - previous_ndc.y * 0.5) * float2(size) -
               position;
    }
    out_history[pixel] = float4(motion, 0.0, 0.0);
    return;
  }

  float4 center = current_color[pixel];

  // Neighbourhood statistics (YCoCg) and the nearest depth for the motion
  // (reversed depth: larger is nearer), so edges move with the foreground.
  float3 m1 = 0.0;
  float3 m2 = 0.0;
  float nearest = -1.0;
  int2 nearest_pixel = pixel;
  [unroll] for (int y = -1; y <= 1; ++y) {
    [unroll] for (int x = -1; x <= 1; ++x) {
      int2 q = clamp(pixel + int2(x, y), int2(0, 0), last);
      float3 c = RgbToYCoCg(current_color[q].rgb);
      m1 += c;
      m2 += c * c;
      float d = current_depth[q];
      if (d > nearest) {
        nearest = d;
        nearest_pixel = q;
      }
    }
  }
  float3 mean = m1 / 9.0;
  float3 sigma = sqrt(max(m2 / 9.0 - mean * mean, 0.0));
  float3 box_min = mean - kClipGamma * sigma;
  float3 box_max = mean + kClipGamma * sigma;

  // Where the nearest surface was in the previous frame (unjittered pixels).
  float2 sample_position = float2(nearest_pixel) + 0.5 - jitter;
  float4 ndc = float4(sample_position.x * 2.0 * size_inv.x - 1.0,
                      1.0 - sample_position.y * 2.0 * size_inv.y, 1.0 - nearest, 1.0);
  float4 previous_clip = float4(dot(reproject_row_0, ndc), dot(reproject_row_1, ndc),
                                dot(reproject_row_2, ndc), dot(reproject_row_3, ndc));
  bool valid = (flags & 1u) != 0u && previous_clip.w > 1e-6;
  float2 previous_ndc = previous_clip.xy / max(previous_clip.w, 1e-6);
  float2 motion = float2(previous_ndc.x * 0.5 + 0.5, 0.5 - previous_ndc.y * 0.5) -
                  sample_position * size_inv;
  float2 history_uv = (float2(pixel) + 0.5) * size_inv + motion;
  valid = valid && all(history_uv >= 0.0) && all(history_uv <= 1.0);

  float3 result = center.rgb;
  if (valid) {
    float3 history = SampleHistory(history_uv);
    float3 clipped = YCoCgToRgb(clamp(RgbToYCoCg(history), box_min, box_max));
    // Luminance-weighted blend (bright history cannot dominate).
    float current_weight = (1.0 - history_weight) / (1.0 + Luma(center.rgb));
    float past_weight = history_weight / (1.0 + Luma(clipped));
    result = (center.rgb * current_weight + clipped * past_weight) /
             max(current_weight + past_weight, 1e-6);
  }
  out_history[pixel] = float4(result, 1.0);
  out_color[pixel] = float4(saturate(result), center.a);
}
