/**
 * @file        rex/ui/settings_detection.h
 * @brief       Host detections shown by every settings surface (V330).
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#pragma once

// What a surface knows about the player's machine turned into choices and
// notes of the shared settings panel, identically for the launcher and the
// in-game overlay: frame rates in plain numbers built from the display's
// refresh rate, with the recommended one marked and uneven ones hinted; HD
// texture packs offered only when one is installed, with their cost.

#include <rex/ui/frame_rate_policy.h>
#include <rex/ui/settings_panel.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>

namespace rex::ui::settings {

inline constexpr std::string_view kFrameRateKey = "display.frame_rate";

// refresh_hz <= 0: unknown (the schema's own list stays).
// FrameRateOptionLabel in the host's interface language.
inline std::string LocalizedFrameRateOptionLabel(const Schema* schema,
                                                 const FrameRateOption& option) {
  if (option.value == "uncapped") return Translate(schema, "Uncapped (with VSync Off)");
  std::string label;
  if (option.fps > 0.0) {
    label = FormatFrameRate(option.fps);
  } else if (option.value == "half_refresh") {
    label = Translate(schema, "Half the display refresh");
  } else if (option.value == "refresh") {
    label = Translate(schema, "Display refresh");
  } else {
    label = option.value;
  }
  if (option.value == "original") label += Translate(schema, " (Original)");
  if (option.recommended) label += Translate(schema, " - Recommended");
  if (option.uneven && option.fps > 0.0) {
    label += Translate(schema, " - may stutter without G-SYNC/FreeSync");
  }
  return label;
}

inline void DetectFrameRateChoices(PanelModel& model, double refresh_hz) {
  if (!model.schema || !model.schema->Find(kFrameRateKey)) return;
  const std::string key(kFrameRateKey);
  if (!(refresh_hz > 0.0)) {
    model.detected_choices.erase(key);
    model.detected_notes.erase(key);
    return;
  }
  std::vector<Choice> choices;
  std::string recommended;
  const std::vector<FrameRateOption> options = FrameRateOptionsForDisplay(refresh_hz);
  for (const FrameRateOption& option : options) {
    choices.push_back({option.value, LocalizedFrameRateOptionLabel(model.schema, option), false});
    if (option.recommended) recommended = FormatFrameRate(option.fps);
  }
  // The schema's other values (the custom cap, rates this display doesn't
  // list) stay reachable in the advanced view; a rate already offered under
  // another value (144 as "refresh" on 144 Hz) isn't repeated.
  if (const Setting* setting = model.schema->Find(kFrameRateKey)) {
    for (const Choice& choice : setting->choices) {
      bool present = false;
      for (const Choice& existing : choices) present |= existing.value == choice.value;
      FrameRateMode mode = FrameRateMode::kOriginal;
      uint32_t cap = 0;
      const bool parsed = ParseFrameRate(choice.value, mode, cap);
      if (parsed && mode == FrameRateMode::k60) cap = 60;
      if (!present && parsed && (mode == FrameRateMode::kCustom || mode == FrameRateMode::k60) &&
          cap != 0) {
        for (const FrameRateOption& option : options) {
          present |= std::fabs(option.fps - double(cap)) < 0.5;
        }
      }
      if (!present) choices.push_back({choice.value, choice.label, true});
    }
  }
  model.detected_choices[key] = std::move(choices);
  std::string note =
      FormatText(Translate(model.schema, "Your monitor: {} Hz"), {FormatFrameRate(refresh_hz)});
  if (!recommended.empty()) {
    note += FormatText(Translate(model.schema, " - recommended {} FPS"), {recommended});
  }
  model.detected_notes[key] = std::move(note);
}

inline constexpr std::string_view kHdTexturesKey = "graphics.hd_textures";

// An HD texture pack folder from file names and sizes alone (no file is
// opened, so any surface can scan it at once).
struct TexturePackFolder {
  uint32_t textures = 0;
  uint64_t disk_bytes = 0;
  // Each texture is its own GPU allocation of at least 64 KB.
  uint64_t video_memory_bytes = 0;
};

// <16 hex digits>.dds, any case.
inline bool IsTexturePackFileName(const std::filesystem::path& path) {
  const auto name = path.filename().native();
  if (name.size() != 20 || name[16] != '.') return false;
  for (size_t i = 0; i < 16; ++i) {
    const auto c = name[i];
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) {
      return false;
    }
  }
  const auto lower = [](auto c) { return (c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c; };
  return lower(name[17]) == 'd' && lower(name[18]) == 'd' && lower(name[19]) == 's';
}

inline TexturePackFolder ScanTexturePackFolder(const std::filesystem::path& folder) {
  TexturePackFolder pack;
  std::error_code error;
  if (folder.empty() || !std::filesystem::is_directory(folder, error)) return pack;
  constexpr uint64_t kAllocation = 64 * 1024;
  constexpr uint64_t kHeader = 148;  // DDS header with the DX10 extension
  for (std::filesystem::recursive_directory_iterator
           it(folder, std::filesystem::directory_options::skip_permission_denied, error),
       end;
       !error && it != end; it.increment(error)) {
    std::error_code entry_error;
    if (!it->is_regular_file(entry_error) || !IsTexturePackFileName(it->path())) continue;
    const uint64_t bytes = it->file_size(entry_error);
    if (entry_error) continue;
    ++pack.textures;
    pack.disk_bytes += bytes;
    const uint64_t data = bytes > kHeader ? bytes - kHeader : bytes;
    pack.video_memory_bytes += (data + kAllocation - 1) / kAllocation * kAllocation;
  }
  return pack;
}

// "540 MB", "1.3 GB", "16 GB".
inline std::string FormatMemorySize(uint64_t bytes) {
  char text[32];
  const double mb = double(bytes) / (1024.0 * 1024.0);
  if (mb >= 1000.0) {
    const double gb = mb / 1024.0;
    const bool whole = std::fabs(gb - std::round(gb)) < 0.1;  // cards report a bit under
    std::snprintf(text, sizeof(text), whole ? "%.0f GB" : "%.1f GB", gb);
  } else {
    std::snprintf(text, sizeof(text), "%.0f MB", mb < 1.0 && bytes ? 1.0 : mb);
  }
  return text;
}

inline constexpr std::string_view kLanguageKey = "general.language";

// An in-game language pack folder (language_packs/<name>): the translated
// strings, the pack fonts and, optionally, the subtitles. Only file names are
// checked.
struct LanguagePackFolder {
  bool strings = false;
  bool fonts = false;
  bool subtitles = false;
};

inline LanguagePackFolder ScanLanguagePackFolder(const std::filesystem::path& folder) {
  LanguagePackFolder pack;
  std::error_code error;
  if (folder.empty() || !std::filesystem::is_directory(folder, error)) return pack;
  pack.strings = std::filesystem::is_regular_file(folder / "strings.tsv", error);
  pack.fonts = std::filesystem::is_directory(folder / "content" / "Content" / "Fonts", error);
  pack.subtitles = std::filesystem::is_regular_file(
      folder / "content" / "Content_Eng" / "Dialogues" / "All.xcd", error);
  return pack;
}

// Arabic is always offered (the launcher and the in-game settings are fully
// Arabic); the note says whether the game's own text follows (a pack is
// installed) or where a pack goes. where: "the language_packs\arabic folder
// in the game folder".
inline void DetectLanguagePack(PanelModel& model, const LanguagePackFolder& pack,
                               std::string_view where) {
  if (!model.schema || !model.schema->Find(kLanguageKey)) return;
  const std::string key(kLanguageKey);
  if (pack.strings && pack.fonts) {
    model.detected_notes[key] = Translate(
        model.schema, pack.subtitles
                          ? "Arabic language pack installed: menus, messages and subtitles in "
                            "Arabic (speech stays English)."
                          : "Arabic language pack installed: menus and messages in Arabic "
                            "(speech stays English).");
    return;
  }
  model.detected_notes[key] =
      FormatText(Translate(model.schema,
                           "Arabic sets this launcher and the in-game settings to Arabic. The "
                           "game's own text needs an Arabic language pack in {}."),
                 {Translate(model.schema, where)});
}

// The HD texture setting is offered only with a pack installed; the note
// gives the pack's size and video memory next to the card's, and a pack
// taking more than a quarter of the card's memory gets a caution.
// card_video_memory_bytes 0: unknown. where: where packs go ("the
// texture_packs folder in the game folder").
inline void DetectTexturePack(PanelModel& model, const TexturePackFolder& pack,
                              uint64_t card_video_memory_bytes, std::string_view where) {
  if (!model.schema || !model.schema->Find(kHdTexturesKey)) return;
  const std::string key(kHdTexturesKey);
  model.detected_warnings.erase(key);
  if (!pack.textures) {
    model.unavailable.insert(key);
    model.detected_notes[key] = FormatText(
        Translate(model.schema, "No texture pack installed. Packs go in {}."),
        {Translate(model.schema, where)});
    return;
  }
  model.unavailable.erase(key);
  std::string note = FormatText(
      Translate(model.schema, pack.textures == 1
                                  ? "Installed: {} texture, {} on disk. Needs about {} more "
                                    "video memory"
                                  : "Installed: {} textures, {} on disk. Needs about {} more "
                                    "video memory"),
      {std::to_string(pack.textures), FormatMemorySize(pack.disk_bytes),
       FormatMemorySize(pack.video_memory_bytes)});
  if (card_video_memory_bytes) {
    note += FormatText(Translate(model.schema, " (this graphics card has {})"),
                       {FormatMemorySize(card_video_memory_bytes)});
  }
  model.detected_notes[key] = note + ".";
  if (card_video_memory_bytes && pack.video_memory_bytes * 4 > card_video_memory_bytes) {
    model.detected_warnings[key] = Translate(
        model.schema,
        "That is more than a quarter of this card's video memory: the game may stutter or "
        "fail to load the pack. Lower the internal scale or leave this off.");
  }
}

inline constexpr std::string_view kTemporalAaKey = "graphics.temporal_aa";

// The vendor upscalers whose runtime DLL is in the game folder (a build
// without an SDK ships neither the DLL nor the option).
struct UpscalerRuntimes {
  bool dlss = false;
  bool fsr = false;
  bool xess = false;
  // NVIDIA DLSS also needs an NVIDIA RTX card; the host checks the adapter
  // (true when it cannot tell).
  bool dlss_card = true;
};

inline UpscalerRuntimes ScanUpscalerRuntimes(const std::filesystem::path& folder) {
  std::error_code error;
  UpscalerRuntimes found;
  found.dlss = std::filesystem::is_regular_file(folder / L"nvngx_dlss.dll", error);
  found.fsr = std::filesystem::is_regular_file(folder / L"amd_fidelityfx_dx12.dll", error);
  found.xess = std::filesystem::is_regular_file(folder / L"libxess.dll", error);
  return found;
}

// Temporal AA offers NVIDIA DLAA, AMD FSR 3.1 and Intel XeSS only when their
// runtime is there (TAA is built in), and DLSS only on an NVIDIA RTX card. A
// current choice that cannot run stays listed, marked: the game uses TAA.
inline void DetectUpscalers(PanelModel& model, const UpscalerRuntimes& runtimes) {
  if (!model.schema) return;
  const Setting* setting = model.schema->Find(kTemporalAaKey);
  if (!setting) return;
  const std::string key(kTemporalAaKey);
  const std::string current = ValueOf(model, key);
  std::vector<Choice> choices;
  for (Choice choice : setting->choices) {
    const bool missing = (choice.value == "dlss" && !runtimes.dlss) ||
                         (choice.value == "fsr" && !runtimes.fsr) ||
                         (choice.value == "xess" && !runtimes.xess);
    const bool no_card = choice.value == "dlss" && runtimes.dlss && !runtimes.dlss_card;
    if (missing || no_card) {
      if (choice.value != current) continue;
      choice.label += Translate(model.schema, no_card ? " - needs an NVIDIA RTX card, TAA is used"
                                                      : " - not installed, TAA is used");
    }
    choices.push_back(std::move(choice));
  }
  model.detected_choices[key] = std::move(choices);
}

}  // namespace rex::ui::settings
