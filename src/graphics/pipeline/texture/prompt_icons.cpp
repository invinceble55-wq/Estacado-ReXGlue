// Button prompts that follow the input device (0.9.1). See
// include/rex/graphics/pipeline/texture/prompt_icons.h.

#include <rex/graphics/pipeline/texture/prompt_icons.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iterator>

#include <rex/cvar.h>
#include <rex/hash.h>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

REXCVAR_DEFINE_STRING(input_button_prompts, "auto", "Input",
                      "Button prompts: auto (follow the last device used), xbox or keyboard");
REXCVAR_DEFINE_STRING(gpu_prompt_icon_source, "", "GPU",
                      "The title's GUI texture file (set by the host): its controller icons get "
                      "keyboard keycaps while keyboard prompts are shown");

namespace rex::graphics::prompt_icons {

namespace {

std::atomic<uint32_t> g_last_device{uint32_t(Device::kNone)};

std::atomic<BindingProvider> g_binding_provider{nullptr};

// The controller buttons whose keys prompts show (input_bind_* names).
constexpr std::string_view kButtons[] = {
    "a", "b", "x", "y", "start", "back", "left_shoulder", "right_shoulder",
    "left_trigger", "right_trigger", "lstick_press", "rstick_press", "dpad_up",
    "dpad_down", "dpad_left", "dpad_right", "lstick_up", "lstick_down",
    "lstick_left", "lstick_right"};

std::string BindingValue(std::string_view button) {
  const BindingProvider provider = g_binding_provider.load(std::memory_order_acquire);
  return provider ? provider(button) : std::string();
}

// The title's controller icons in GUI.xtc and the keys their keycaps show.
// In play the generic stick icons come from the tutorials' binding icons
// (Content/Registry/Sv.xcr): 360_S only for looking (GUI_Binding_Look), its
// click 360_C only for crouching. The left/right and up/down stick variants
// appear only on the controller layout pages and keep their art.
struct IconSpec {
  const char* name;
  std::string_view first;
  std::string_view second;
  std::string_view fixed;
};
constexpr IconSpec kIconSpecs[] = {
    {"GUI_Button_A", "a", {}, {}},
    {"GUI_Button_B", "b", {}, {}},
    {"GUI_Button_X", "x", {}, {}},
    {"GUI_Button_Y", "y", {}, {}},
    {"GUI_Button_Start", "start", {}, {}},
    {"GUI_Button_Back", "back", {}, {}},
    {"GUI_Button_LB", "left_shoulder", {}, {}},
    {"GUI_Button_RB", "right_shoulder", {}, {}},
    {"GUI_Button_LT", "left_trigger", {}, {}},
    {"GUI_Button_RT", "right_trigger", {}, {}},
    {"GUI_Button_DUp", "dpad_up", {}, {}},
    {"GUI_Button_DDown", "dpad_down", {}, {}},
    {"GUI_Button_DLeft", "dpad_left", {}, {}},
    {"GUI_Button_DRight", "dpad_right", {}, {}},
    {"GUI_Button_DUD", "dpad_up", "dpad_down", {}},
    {"GUI_Button_DRL", "dpad_left", "dpad_right", {}},
    {"GUI_Button_LC", "lstick_press", {}, {}},
    {"GUI_Button_RC", "rstick_press", {}, {}},
    {"GUI_Button_360_C", "lstick_press", {}, {}},
    {"GUI_Button_L", {}, {}, "WASD"},
    {"GUI_Button_R", {}, {}, "Mouse"},
    {"GUI_Button_L_LR", "lstick_left", "lstick_right", {}},
    {"GUI_Button_L_UD", "lstick_up", "lstick_down", {}},
    {"GUI_Button_R_LR", {}, {}, "Mouse"},
    {"GUI_Button_R_UD", {}, {}, "Mouse"},
    {"GUI_Button_360_S", {}, {}, "Mouse"},
};

uint32_t ReadU32(const std::vector<uint8_t>& data, size_t offset) {
  uint32_t value;
  std::memcpy(&value, data.data() + offset, sizeof(value));
  return value;
}

struct Range {
  size_t offset = 0;
  size_t size = 0;
};

// MOS DATAFILE2.0: u32 directory offset at 0x18; 0x30-byte entries
// {name[16], 0, 0, next sibling, first child, data offset, data size, user1,
// user2}, top level first.
bool FindDatafileEntries(const std::vector<uint8_t>& file, Range& textures, Range& directory) {
  static constexpr char kMagic[] = "MOS DATAFILE2.0";
  if (file.size() < 0x30 || std::memcmp(file.data(), kMagic, sizeof(kMagic)) != 0) return false;
  const size_t entries = ReadU32(file, 0x18);
  for (size_t entry = entries; entry + 0x30 <= file.size() && entry < entries + 0x30 * 64;
       entry += 0x30) {
    char name[17] = {};
    std::memcpy(name, file.data() + entry, 16);
    const size_t offset = ReadU32(file, entry + 0x20);
    const size_t size = ReadU32(file, entry + 0x24);
    if (offset > file.size() || size > file.size() - offset) continue;
    if (std::strcmp(name, "TEXTURES") == 0) textures = {offset, size};
    if (std::strncmp(name, "IMAGEDIRECTORY", 14) == 0) directory = {offset, size};
  }
  return textures.size && directory.size;
}

#if defined(_WIN32)
// Text coverage (0..255) of a label drawn with the system UI font, centred in
// a w x h box, the font as large as fits.
std::vector<uint8_t> TextCoverage(const std::wstring& text, int w, int h, int max_height) {
  std::vector<uint8_t> coverage(size_t(w) * h, 0);
  if (text.empty() || w <= 0 || h <= 0) return coverage;
  HDC dc = CreateCompatibleDC(nullptr);
  if (!dc) return coverage;
  BITMAPINFO info = {};
  info.bmiHeader.biSize = sizeof(info.bmiHeader);
  info.bmiHeader.biWidth = w;
  info.bmiHeader.biHeight = -h;
  info.bmiHeader.biPlanes = 1;
  info.bmiHeader.biBitCount = 32;
  info.bmiHeader.biCompression = BI_RGB;
  void* bits = nullptr;
  HBITMAP bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
  if (!bitmap || !bits) {
    if (bitmap) DeleteObject(bitmap);
    DeleteDC(dc);
    return coverage;
  }
  HGDIOBJ old_bitmap = SelectObject(dc, bitmap);
  // The largest font whose text fits (bold, grey antialiasing - no colour
  // fringes in an alpha mask).
  HFONT font = nullptr;
  for (int size = max_height; size >= 6; --size) {
    HFONT candidate = CreateFontW(-size, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                  OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
                                  DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    if (!candidate) break;
    HGDIOBJ previous = SelectObject(dc, candidate);
    SIZE extent = {};
    GetTextExtentPoint32W(dc, text.c_str(), int(text.size()), &extent);
    SelectObject(dc, previous);
    if (extent.cx <= w && extent.cy <= h + size / 3) {
      font = candidate;
      break;
    }
    DeleteObject(candidate);
  }
  if (font) {
    HGDIOBJ old_font = SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(255, 255, 255));
    RECT rect = {0, 0, w, h};
    DrawTextW(dc, text.c_str(), int(text.size()), &rect,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    GdiFlush();
    const auto* pixels = static_cast<const uint8_t*>(bits);
    for (size_t i = 0; i < coverage.size(); ++i) {
      // Grey antialiasing: the channels are equal; take the brightest.
      coverage[i] = std::max({pixels[i * 4], pixels[i * 4 + 1], pixels[i * 4 + 2]});
    }
    SelectObject(dc, old_font);
    DeleteObject(font);
  }
  SelectObject(dc, old_bitmap);
  DeleteObject(bitmap);
  DeleteDC(dc);
  return coverage;
}
#endif

std::wstring Widen(std::string_view utf8) {
  std::wstring out;
#if defined(_WIN32)
  if (utf8.empty()) return out;
  const int length =
      MultiByteToWideChar(CP_UTF8, 0, utf8.data(), int(utf8.size()), nullptr, 0);
  out.resize(size_t(std::max(length, 0)));
  if (length > 0) {
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), int(utf8.size()), out.data(), length);
  }
#else
  for (char c : utf8) out.push_back(wchar_t(uint8_t(c)));
#endif
  return out;
}

// Bounds of the texels with alpha above a quarter in linear BC3 blocks.
void Bc3AlphaBounds(const uint8_t* blocks, uint32_t width, uint32_t height, uint32_t& x0,
                    uint32_t& y0, uint32_t& x1, uint32_t& y1) {
  x0 = width;
  y0 = height;
  x1 = 0;
  y1 = 0;
  const uint32_t blocks_x = width / 4, blocks_y = height / 4;
  for (uint32_t by = 0; by < blocks_y; ++by) {
    for (uint32_t bx = 0; bx < blocks_x; ++bx) {
      const uint8_t* block = blocks + (size_t(by) * blocks_x + bx) * 16;
      const uint32_t a0 = block[0], a1 = block[1];
      uint32_t alpha[8] = {a0, a1};
      if (a0 > a1) {
        for (uint32_t i = 1; i < 7; ++i) alpha[i + 1] = ((7 - i) * a0 + i * a1) / 7;
      } else {
        for (uint32_t i = 1; i < 5; ++i) alpha[i + 1] = ((5 - i) * a0 + i * a1) / 5;
        alpha[6] = 0;
        alpha[7] = 255;
      }
      uint64_t bits = 0;
      for (int i = 0; i < 6; ++i) bits |= uint64_t(block[2 + i]) << (8 * i);
      for (uint32_t i = 0; i < 16; ++i) {
        if (alpha[(bits >> (3 * i)) & 7] <= 64) continue;
        const uint32_t x = bx * 4 + i % 4, y = by * 4 + i / 4;
        x0 = std::min(x0, x);
        y0 = std::min(y0, y);
        x1 = std::max(x1, x + 1);
        y1 = std::max(y1, y + 1);
      }
    }
  }
  if (x1 <= x0 || y1 <= y0) {
    x0 = 0;
    y0 = 0;
    x1 = width;
    y1 = height;
  }
}

// Coverage of a rounded rectangle (x0..x1, y0..y1, radius r) at a pixel
// centre, antialiased over one pixel.
float RoundedRectCoverage(float px, float py, float x0, float y0, float x1, float y1, float r) {
  const float cx = std::clamp(px, x0 + r, x1 - r);
  const float cy = std::clamp(py, y0 + r, y1 - r);
  const float dx = px - cx, dy = py - cy;
  const float distance = std::sqrt(dx * dx + dy * dy) - r;
  return std::clamp(0.5f - distance, 0.0f, 1.0f);
}

}  // namespace

void SetBindingProvider(BindingProvider provider) {
  g_binding_provider.store(provider, std::memory_order_release);
}

void NoteInputDevice(Device device) {
  if (device != Device::kNone) g_last_device.store(uint32_t(device), std::memory_order_relaxed);
}

Device LastInputDevice() { return Device(g_last_device.load(std::memory_order_relaxed)); }

bool KeyboardPromptsWanted() {
  const std::string& mode = REXCVAR_GET(input_button_prompts);
  if (mode == "keyboard") return true;
  if (mode == "xbox") return false;
  return LastInputDevice() == Device::kKeyboardMouse;
}

uint32_t LabelsGeneration() {
  uint64_t hash = 1469598103934665603ull;
  auto mix = [&](std::string_view text) {
    for (char c : text) hash = (hash ^ uint8_t(c)) * 1099511628211ull;
    hash = (hash ^ 0xFF) * 1099511628211ull;
  };
  for (std::string_view button : kButtons) mix(BindingValue(button));
  return uint32_t(hash ^ (hash >> 32));
}

std::string KeyLabel(std::string_view value) {
  static constexpr std::pair<std::string_view, std::string_view> kNames[] = {
      {"Escape", "Esc"},      {"Return", "Enter"},     {"Backspace", "Bksp"},
      {"Delete", "Del"},      {"Insert", "Ins"},       {"PageUp", "PgUp"},
      {"PageDown", "PgDn"},   {"Control", "Ctrl"},     {"CapsLock", "Caps"},
      {"NumLock", "NumLk"},   {"ScrollLock", "ScrLk"}, {"PrintScreen", "PrtSc"},
      {"NumpadEnter", "Enter"}, {"NumpadPlus", "Num+"}, {"NumpadMinus", "Num-"},
      {"NumpadStar", "Num*"}, {"NumpadSlash", "Num/"}, {"Backtick", "`"},
      {"Minus", "-"},         {"Plus", "="},           {"Comma", ","},
      {"Period", "."},        {"Semicolon", ";"},      {"Slash", "/"},
      {"Backslash", "\\"},    {"LBracket", "["},       {"RBracket", "]"},
      {"Quote", "'"},         {"X1", "M4"},            {"X2", "M5"},
      {"Up", "\xE2\x86\x91"}, {"Down", "\xE2\x86\x93"}, {"Left", "\xE2\x86\x90"},
      {"Right", "\xE2\x86\x92"},
  };
  for (const auto& [name, label] : kNames) {
    if (value == name) return std::string(label);
  }
  if (value.size() == 7 && value.substr(0, 6) == "Numpad") {
    return "Num" + std::string(value.substr(6));
  }
  return std::string(value);
}

std::string TextKeyLabel(std::string_view value) {
  if (value == "Up" || value == "Down" || value == "Left" || value == "Right") {
    return std::string(value);
  }
  return KeyLabel(value);
}

std::vector<ButtonLabel> CurrentLabels() {
  std::vector<ButtonLabel> labels;
  labels.reserve(std::size(kButtons));
  for (std::string_view button : kButtons) {
    labels.push_back({button, KeyLabel(BindingValue(button))});
  }
  return labels;
}

uint64_t BlockSignature(const uint8_t* blocks, size_t size) {
  return XXH3_64bits(blocks, size);
}

bool IconSet::Load(const std::filesystem::path& gui_textures, std::string& error) {
  icons_.clear();
  std::ifstream stream(gui_textures, std::ios::binary);
  if (!stream) {
    error = "cannot open the GUI texture file";
    return false;
  }
  const std::vector<uint8_t> file((std::istreambuf_iterator<char>(stream)),
                                  std::istreambuf_iterator<char>());
  Range textures, directory;
  if (!FindDatafileEntries(file, textures, directory)) {
    error = "not a GUI texture datafile";
    return false;
  }
  // Image records: FFFFFFFF, 0, name length, name, then words where
  // {mips, 0x300, ...} start; the icon's data header {0x5000, total size,
  // width, height, 0x800, 0, 4, base size, 0, 0x10} precedes its BC3 blocks.
  const uint8_t* records = file.data() + directory.offset;
  for (size_t at = 0; at + 12 <= directory.size;) {
    uint32_t marker, zero, length;
    std::memcpy(&marker, records + at, 4);
    std::memcpy(&zero, records + at + 4, 4);
    std::memcpy(&length, records + at + 8, 4);
    if (marker != 0xFFFFFFFFu || zero != 0 || length == 0 || length > 64 ||
        at + 12 + length > directory.size) {
      ++at;
      continue;
    }
    std::string name(reinterpret_cast<const char*>(records + at + 12), length);
    while (!name.empty() && (name.back() == ' ' || name.back() == '\0')) name.pop_back();
    const size_t body = at + 12 + length;
    at = body;
    const IconSpec* spec = nullptr;
    for (const IconSpec& candidate : kIconSpecs) {
      if (name == candidate.name) spec = &candidate;
    }
    if (!spec) continue;
    // The words of this record (up to the next marker).
    size_t words = body;
    while (words + 4 <= directory.size && ReadU32(file, directory.offset + words) != 0x300) ++words;
    if (words < body + 4 || words + 32 > directory.size) continue;
    const size_t w0 = directory.offset + words - 4;
    const uint32_t size = ReadU32(file, w0 + 12);
    const uint32_t width = ReadU32(file, w0 + 16);
    const uint32_t height = ReadU32(file, w0 + 20);
    const uint32_t offset = ReadU32(file, w0 + 32);
    if (!width || !height || width > 512 || height > 512 || (width & 3) || (height & 3)) continue;
    // The data header near the recorded offset.
    const uint32_t header[4] = {0x5000, size, width, height};
    const size_t search_begin = offset > 64 ? offset - 64 : 0;
    const size_t search_end = std::min<size_t>(textures.size, size_t(offset) + 64);
    size_t found = SIZE_MAX;
    for (size_t p = search_begin; p + 40 <= search_end; ++p) {
      if (std::memcmp(file.data() + textures.offset + p, header, sizeof(header)) == 0) {
        found = p;
        break;
      }
    }
    const size_t base_size = size_t(width) * height;  // BC3: one byte per texel
    if (found == SIZE_MAX || found + 40 + base_size > textures.size ||
        ReadU32(file, textures.offset + found + 28) != base_size) {
      continue;
    }
    Icon icon;
    icon.name = name;
    icon.width = width;
    icon.height = height;
    icon.signature = BlockSignature(file.data() + textures.offset + found + 40, base_size);
    Bc3AlphaBounds(file.data() + textures.offset + found + 40, width, height, icon.art_x0,
                   icon.art_y0, icon.art_x1, icon.art_y1);
    icon.first = spec->first;
    icon.second = spec->second;
    icon.fixed = spec->fixed;
    icons_.push_back(std::move(icon));
  }
  if (icons_.empty()) {
    error = "no controller icons found";
    return false;
  }
  return true;
}

bool IconSet::IsCandidateSize(uint32_t width, uint32_t height) const {
  for (const Icon& icon : icons_) {
    if (icon.width == width && icon.height == height) return true;
  }
  return false;
}

int32_t IconSet::Find(uint32_t width, uint32_t height, uint64_t signature) const {
  for (size_t i = 0; i < icons_.size(); ++i) {
    const Icon& icon = icons_[i];
    if (icon.width == width && icon.height == height && icon.signature == signature) {
      return int32_t(i);
    }
  }
  return -1;
}

texture_pack::DdsImage RenderKeycap(const Icon& icon, const std::vector<ButtonLabel>& labels) {
  auto label_of = [&](std::string_view button) -> std::string {
    for (const ButtonLabel& label : labels) {
      if (label.button == button) return label.label;
    }
    return {};
  };
  std::string text;
  if (!icon.fixed.empty()) {
    text = std::string(icon.fixed);
  } else {
    text = label_of(icon.first);
    if (!icon.second.empty()) {
      const std::string second = label_of(icon.second);
      if (!second.empty() && second != text) text = text.empty() ? second : text + " " + second;
    }
  }
  if (text.empty()) text = "?";

  const uint32_t scale = std::clamp<uint32_t>(256 / std::max(icon.width, icon.height), 1, 4);
  const uint32_t w = icon.width * scale, h = icon.height * scale;
  // The keycap: as tall as the title's art, as wide as the text needs (at
  // least square), centred on the art and never wider than it.
  const float art_x0 = float(icon.art_x0 * scale), art_x1 = float(icon.art_x1 * scale);
  const float art_y0 = float(icon.art_y0 * scale), art_y1 = float(icon.art_y1 * scale);
  const float art_w = std::max(art_x1 - art_x0, 4.0f), art_h = std::max(art_y1 - art_y0, 4.0f);
  const float margin = art_h * 0.04f;
  const float cap_h = art_h - 2.0f * margin;
  std::vector<uint8_t> text_cover;
  float cap_w = std::min(cap_h, art_w);
#if defined(_WIN32)
  {
    // Measure at the cap's text height to choose the cap width.
    const int text_h = int(cap_h * 0.62f);
    const std::wstring wide = Widen(text);
    HDC dc = CreateCompatibleDC(nullptr);
    if (dc) {
      HFONT font = CreateFontW(-text_h, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                               OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
                               DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
      if (font) {
        HGDIOBJ previous = SelectObject(dc, font);
        SIZE extent = {};
        GetTextExtentPoint32W(dc, wide.c_str(), int(wide.size()), &extent);
        SelectObject(dc, previous);
        DeleteObject(font);
        cap_w = std::clamp(float(extent.cx) + cap_h * 0.5f, std::min(cap_h, art_w),
                           std::max(art_w - 2.0f * margin, std::min(cap_h, art_w)));
      }
      DeleteDC(dc);
    }
  }
#endif
  const float centre_x = (art_x0 + art_x1) * 0.5f;
  const float x0 = centre_x - cap_w * 0.5f, x1 = x0 + cap_w;
  const float y0 = art_y0 + margin, y1 = y0 + cap_h;
  const float radius = cap_h * 0.2f;
  const float border = std::max(1.5f, cap_h * 0.07f);
  const int inner_w = int(cap_w - 2.0f * border - cap_h * 0.12f);
  const int inner_h = int(cap_h - 2.0f * border);
#if defined(_WIN32)
  text_cover = TextCoverage(Widen(text), std::max(inner_w, 1), std::max(inner_h, 1),
                            int(cap_h * 0.62f));
#endif
  const int text_x0 = int(std::lround(centre_x - float(inner_w) * 0.5f));
  const int text_y0 = int(std::lround((y0 + y1) * 0.5f - float(inner_h) * 0.5f));

  // Premultiplied RGBA while composing.
  std::vector<float> rgba(size_t(w) * h * 4, 0.0f);
  for (uint32_t y = 0; y < h; ++y) {
    for (uint32_t x = 0; x < w; ++x) {
      const float px = float(x) + 0.5f, py = float(y) + 0.5f;
      const float outer = RoundedRectCoverage(px, py, x0, y0, x1, y1, radius);
      if (outer <= 0.0f) continue;
      const float inner = RoundedRectCoverage(px, py, x0 + border, y0 + border, x1 - border,
                                              y1 - border, std::max(radius - border, 0.0f));
      // Light rim, dark face (slightly lighter at the top), white text.
      const float shade = 0.16f + 0.08f * (1.0f - (py - y0) / cap_h);
      const float rim = outer - inner;
      // Colours here are straight (the rim's and the face's own); alpha
      // blends them by coverage.
      float a = rim + inner * 0.94f;
      float r = a > 0.0f ? (0.86f * rim + shade * inner * 0.94f) / a : 0.86f;
      float g = r, b = r;
      const int tx = int(x) - text_x0, ty = int(y) - text_y0;
      if (!text_cover.empty() && tx >= 0 && ty >= 0 && tx < inner_w && ty < inner_h) {
        const float t = float(text_cover[size_t(ty) * inner_w + tx]) / 255.0f * inner;
        r = r * (1.0f - t) + t;
        g = g * (1.0f - t) + t;
        b = b * (1.0f - t) + t;
      }
      float* out = &rgba[(size_t(y) * w + x) * 4];
      out[0] = r * a;
      out[1] = g * a;
      out[2] = b * a;
      out[3] = a;
    }
  }

  texture_pack::DdsImage image;
  image.dxgi_format = 28;  // DXGI_FORMAT_R8G8B8A8_UNORM
  image.width = w;
  image.height = h;
  uint32_t level_w = w, level_h = h;
  std::vector<float> level = std::move(rgba);
  for (;;) {
    image.mip_offsets.push_back(image.data.size());
    image.mip_row_bytes.push_back(level_w * 4);
    image.mip_rows.push_back(level_h);
    for (size_t i = 0; i < size_t(level_w) * level_h; ++i) {
      const float a = level[i * 4 + 3];
      for (int c = 0; c < 3; ++c) {
        // Unpremultiplied; transparent texels keep the rim colour so
        // filtering never darkens the edge.
        const float value = a > 0.0f ? level[i * 4 + c] / a : 0.86f;
        image.data.push_back(uint8_t(std::lround(std::clamp(value, 0.0f, 1.0f) * 255.0f)));
      }
      image.data.push_back(uint8_t(std::lround(std::clamp(a, 0.0f, 1.0f) * 255.0f)));
    }
    ++image.mip_levels;
    if (level_w == 1 && level_h == 1) break;
    const uint32_t next_w = std::max(level_w / 2, 1u), next_h = std::max(level_h / 2, 1u);
    std::vector<float> next(size_t(next_w) * next_h * 4, 0.0f);
    for (uint32_t y = 0; y < next_h; ++y) {
      for (uint32_t x = 0; x < next_w; ++x) {
        for (int c = 0; c < 4; ++c) {
          float sum = 0.0f;
          int count = 0;
          for (uint32_t sy = y * 2; sy < std::min(y * 2 + 2, level_h); ++sy) {
            for (uint32_t sx = x * 2; sx < std::min(x * 2 + 2, level_w); ++sx) {
              sum += level[(size_t(sy) * level_w + sx) * 4 + c];
              ++count;
            }
          }
          next[(size_t(y) * next_w + x) * 4 + c] = count ? sum / float(count) : 0.0f;
        }
      }
    }
    level = std::move(next);
    level_w = next_w;
    level_h = next_h;
  }
  return image;
}

}  // namespace rex::graphics::prompt_icons
