#pragma once

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <string_view>

namespace rex::graphics::render_target::native_shader_scale_policy {

// Explicit semantic annotations for rasterized data, not automatic guesses
// based on a surface's dimensions. Format: VS:PS:fetch:width:height:format;
// :filter preserves the native filter evaluation grid and source footprint.
// :filter_scaled preserves only the source footprint, evaluating the same
// continuous filter at scaled pixel centers (not replicating its native result).
// An image filter may end with :region=X,Y,W,H - the part of the texture that
// holds the image, in native texels - when the image occupies only part of a
// larger texture (for example a 160x90 buffer in the corner of a 1280x720
// surface): the footprint reconstruction then never blends texels outside it.
// The default six-field form is still single-sample rasterized data.
// Hashes are hexadecimal, remaining fields decimal. No title identities live
// in this reusable policy. An empty list leaves normal resolution scaling alone.
struct Rule {
  uint64_t vertex_hash = 0;
  uint64_t pixel_hash = 0;
  uint32_t fetch = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t format = 0;
  // Explicit image-filter annotation, unlike a rasterized lookup table.
  // Preserves both the native evaluation lattice and the input footprint.
  bool image_filter = false;
  bool scaled_filter_output = false;
  // Optional :region=X,Y,W,H (image filters only), in native texels.
  bool has_region = false;
  uint32_t region_left = 0;
  uint32_t region_top = 0;
  uint32_t region_right = 0;
  uint32_t region_bottom = 0;
  // Optional :msaa=N (image filters only): matches only draws with N samples.
  uint32_t msaa_samples = 0;
  // Optional :source=native (image filters only): the reconstruction applies
  // only while the fetched texture holds a tracked native-resolution resolve
  // region (native_resolve_region_tracking), bounded to that region; any other
  // texture bound to the same fetch (genuinely scaled content) is sampled
  // normally. For a consumer shared by native and scaled inputs, such as a
  // generic textured quad that draws both a native glow and a scene copy.
  bool native_source = false;
};

struct Rules {
  std::array<Rule, 32> entries{};
  uint32_t count = 0;
};

inline bool Parse(std::string_view text, Rules& output) {
  output = {};
  if (text.size() > 4096) return false;
  Rules parsed;
  while (!text.empty()) {
    if (parsed.count == parsed.entries.size()) return false;
    const size_t separator = text.find(';');
    std::string_view entry = text.substr(0, separator);
    // Trailing options (image filters): :msaa=N (1, 2 or 4 samples),
    // :region=X,Y,W,H and :source=native, in any order after the filter suffix.
    std::array<uint64_t, 4> region{};
    bool has_region = false;
    uint32_t msaa_samples = 0;
    bool native_source = false;
    size_t options_at = (std::min)((std::min)(entry.find(":region="), entry.find(":msaa=")),
                                   entry.find(":source="));
    // (std::min): windows.h defines a min macro.
    if (options_at != std::string_view::npos) {
      std::string_view options = entry.substr(options_at + 1);
      entry = entry.substr(0, options_at);
      while (!options.empty()) {
        const size_t end = options.find(':');
        const std::string_view option = options.substr(0, end);
        constexpr std::string_view region_key = "region=";
        constexpr std::string_view msaa_key = "msaa=";
        if (option.substr(0, region_key.size()) == region_key && !has_region) {
          std::string_view fields = option.substr(region_key.size());
          for (size_t i = 0; i < region.size(); ++i) {
            const size_t comma = fields.find(',');
            if ((i < 3) != (comma != std::string_view::npos)) return false;
            const auto value = fields.substr(0, comma);
            if (value.empty()) return false;
            const auto result = std::from_chars(value.data(), value.data() + value.size(), region[i]);
            if (result.ec != std::errc{} || result.ptr != value.data() + value.size()) return false;
            if (i < 3) fields.remove_prefix(comma + 1);
          }
          if (!region[2] || !region[3]) return false;
          has_region = true;
        } else if (option == "source=native" && !native_source) {
          native_source = true;
        } else if (option.substr(0, msaa_key.size()) == msaa_key && !msaa_samples) {
          const auto value = option.substr(msaa_key.size());
          const auto result = std::from_chars(value.data(), value.data() + value.size(), msaa_samples);
          if (result.ec != std::errc{} || result.ptr != value.data() + value.size() ||
              (msaa_samples != 1 && msaa_samples != 2 && msaa_samples != 4)) return false;
        } else {
          return false;
        }
        if (end == std::string_view::npos) break;
        options.remove_prefix(end + 1);
        if (options.empty()) return false;
      }
    }
    constexpr std::string_view scaled_suffix = ":filter_scaled";
    const bool scaled_filter_output = entry.size() >= scaled_suffix.size() &&
        entry.substr(entry.size() - scaled_suffix.size()) == scaled_suffix;
    const bool native_filter = entry.size() >= 7 && entry.substr(entry.size() - 7) == ":filter";
    const bool image_filter = native_filter || scaled_filter_output;
    if (scaled_filter_output) entry.remove_suffix(scaled_suffix.size());
    else if (native_filter) entry.remove_suffix(7);
    std::string_view fields = entry;
    std::array<uint64_t, 6> values{};
    for (size_t i = 0; i < values.size(); ++i) {
      const size_t end = fields.find(':');
      if ((i < 5) != (end != std::string_view::npos)) return false;
      const auto value = fields.substr(0, end);
      if (value.empty()) return false;
      const auto result = std::from_chars(value.data(), value.data() + value.size(),
                                           values[i], i < 2 ? 16 : 10);
      if (result.ec != std::errc{} || result.ptr != value.data() + value.size()) return false;
      if (i < 5) fields.remove_prefix(end + 1);
    }
    if (!values[0] || !values[1] || values[2] >= 32 ||
        !values[3] || values[3] > 16384 || !values[4] || values[4] > 16384 ||
        values[5] >= 64) return false;
    if ((has_region || msaa_samples || native_source) && !image_filter) return false;
    // A tracked region replaces an explicit one.
    if (native_source && has_region) return false;
    if (has_region && (region[0] + region[2] > values[3] || region[1] + region[3] > values[4])) {
      return false;
    }
    Rule rule{values[0], values[1], uint32_t(values[2]), uint32_t(values[3]),
              uint32_t(values[4]), uint32_t(values[5]), image_filter, scaled_filter_output,
              has_region, uint32_t(region[0]), uint32_t(region[1]),
              uint32_t(region[0] + region[2]), uint32_t(region[1] + region[3]), msaa_samples,
              native_source};
    for (uint32_t i = 0; i < parsed.count; ++i) {
      const Rule& previous = parsed.entries[i];
      if (previous.vertex_hash == rule.vertex_hash && previous.pixel_hash == rule.pixel_hash &&
          previous.fetch == rule.fetch && previous.width == rule.width &&
          previous.height == rule.height && previous.format == rule.format &&
          previous.msaa_samples == rule.msaa_samples) return false;
    }
    parsed.entries[parsed.count++] = rule;
    if (separator == std::string_view::npos) break;
    text.remove_prefix(separator + 1);
    if (text.empty()) return false;
  }
  output = parsed;
  return true;
}

inline bool Matches(const Rule& rule, uint64_t vertex_hash, uint64_t pixel_hash,
                    bool texture_2d, uint32_t width, uint32_t height,
                    uint32_t format, uint32_t msaa_log2, uint32_t color_mask,
                    uint32_t depth_control = 0) {
  // Image filters additionally permit depth/stencil-disabled MSAA draws (2x and
  // 4x): the source-footprint correction is a pixel shader change independent
  // of the destination's samples, and 4x draws keep a scaled output (see
  // RequiresNativeRasterization). Ordinary data rules retain the original
  // single-sample guard.
  if (rule.msaa_samples && rule.msaa_samples != (uint32_t(1) << msaa_log2)) return false;
  return vertex_hash == rule.vertex_hash && pixel_hash == rule.pixel_hash &&
         texture_2d && width == rule.width && height == rule.height &&
         format == rule.format && color_mask == 0xF &&
         (rule.image_filter ? (msaa_log2 <= 2 && (depth_control & 7) == 0)
                            : msaa_log2 == 0);
}

// The rules without their image filters (width 0 matches no texture), for
// frames a title renders with 2x MSAA (#16: black 2x4-pixel holes and
// flickering squares on some NVIDIA RTX 20/30 cards, only in those frames and
// only with the image filters). Data rules are unchanged; every rule keeps its
// index (per-rule logs).
inline Rules WithoutImageFilters(const Rules& rules) {
  Rules result = rules;
  for (uint32_t i = 0; i < result.count; ++i) {
    if (result.entries[i].image_filter) result.entries[i].width = 0;
  }
  return result;
}

inline bool RequiresNativeRasterization(const Rule& rule, uint32_t msaa_log2) {
  // Never expand rasterized lookup/data tables. The opt-in image filter alone
  // may keep a scaled output so a later ordinary sampler does not see repeated
  // native texels as distinct host texels. Source filtering remains independent.
  // MSAA image filters follow the same choice: :filter renders the pass on the
  // native grid (the title's own result), :filter_scaled keeps a scaled output
  // with only the source footprint corrected.
  (void)msaa_log2;
  return !rule.image_filter || !rule.scaled_filter_output;
}

inline bool FilterSamplingSupported(uint32_t scale_x, uint32_t scale_y,
                                    bool linear_min_mag, uint32_t maximum_mip,
                                    bool unsigned_components, bool clamp_to_edge = true) {
  // Pairs of host texels read with one bilinear lookup each exactly
  // box-reduce an SxS source cell and apply native bilinear weights (S*S
  // lookups per fetch, so S is capped). Other mips/sign domains need separate
  // implementations.
  return scale_x >= 2 && scale_x <= 4 && scale_y >= 2 && scale_y <= 4 &&
         linear_min_mag && maximum_mip == 0 &&
         unsigned_components && clamp_to_edge;
}

inline bool FilterInstructionSupported(bool normalized_coordinates,
                                       float guest_offset_x, float guest_offset_y,
                                       bool register_lod, bool register_gradients) {
  // Inspect guest immediates, not the translator's adjusted offset: even a
  // zero-offset tfetch receives a small host rounding epsilon later.
  return normalized_coordinates && guest_offset_x == 0.0f &&
         guest_offset_y == 0.0f && !register_lod && !register_gradients;
}

}  // namespace rex::graphics::render_target::native_shader_scale_policy
