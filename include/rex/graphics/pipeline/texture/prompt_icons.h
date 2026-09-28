#pragma once

// Button prompts that follow the input device (0.9.1): while the player last
// used the keyboard or mouse, the title's controller button icons (its
// GUI_Button_* textures) show keycaps with the bound key instead, and the
// host's string lookup shows key names in prompt text (the runtime). Xbox
// icons come back as soon as a controller is used; input_button_prompts
// = "xbox" or "keyboard" fixes either.
//
// The icons are recognised by their pixels, not their guest address: the
// title's GUI.xtc (gpu_prompt_icon_source, set by the host) stores each icon
// as linear little-endian BC3 blocks after a 40-byte header, so the hash of
// those blocks equals the hash of a loaded texture's base level untiled and
// in host byte order. Keycaps are drawn here with the system UI font (no art
// from other games); their labels follow the key bindings.

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include <rex/graphics/pipeline/texture/texture_pack.h>

namespace rex::graphics::prompt_icons {

enum class Device : uint32_t { kNone = 0, kController = 1, kKeyboardMouse = 2 };

// The key bound to a controller button (an input_bind_* value: "E", "Space",
// "LMB", ...; empty when unbound). Set by the plugin, which owns the input
// cvars.
using BindingProvider = std::string (*)(std::string_view button);
void SetBindingProvider(BindingProvider provider);

// The device of the latest player input (reported by the host's input poll).
void NoteInputDevice(Device device);
Device LastInputDevice();
// input_button_prompts with the last device: keyboard prompts wanted now.
bool KeyboardPromptsWanted();
// Changes whenever a binding cvar or input_button_prompts changes (labels and
// keycaps are rebuilt).
uint32_t LabelsGeneration();

// The labels of the controller buttons as keyboard keys, in the binding
// order of PromptButtons() ("a", "b", ...), for the host's prompt text.
struct ButtonLabel {
  std::string_view button;  // binding name: a, b, x, y, start, ...
  std::string label;        // UTF-8, e.g. "E", "Space", "LMB"
};
std::vector<ButtonLabel> CurrentLabels();
// Display name of an input_bind_* value ("Escape" -> "Esc", "LMB" -> "LMB",
// "Up" -> an arrow).
std::string KeyLabel(std::string_view binding_value);
// The same name for the title's text, which draws Latin-1 only: arrows as
// words ("Up"), "Space", "Escape" -> "Esc".
std::string TextKeyLabel(std::string_view binding_value);

// One of the title's controller icons.
struct Icon {
  std::string name;      // GUI_Button_A, ...
  uint32_t width = 0;    // guest texels
  uint32_t height = 0;
  uint64_t signature = 0;
  // Where the title's art is (alpha > 1/4), in guest texels: the keycap
  // stays inside it (wide stick icons have their art in the middle and are
  // drawn overlapping).
  uint32_t art_x0 = 0, art_y0 = 0, art_x1 = 0, art_y1 = 0;
  // Binding names whose keys the keycap shows (one or two).
  std::string_view first;
  std::string_view second;
  // Fixed text instead of keys (sticks: "WASD", "Mouse").
  std::string_view fixed;
};

class IconSet {
 public:
  // Reads the icon blocks from the title's GUI.xtc. False (and empty) when
  // the file is missing or has another layout.
  bool Load(const std::filesystem::path& gui_textures, std::string& error);
  bool empty() const { return icons_.empty(); }
  const std::vector<Icon>& icons() const { return icons_; }
  // Candidate guest sizes (all icons are 2D BC3).
  bool IsCandidateSize(uint32_t width, uint32_t height) const;
  // Index of the icon with these texels, or -1.
  int32_t Find(uint32_t width, uint32_t height, uint64_t signature) const;

 private:
  std::vector<Icon> icons_;
};

// Signature of a base level given as linear BC blocks in host byte order.
uint64_t BlockSignature(const uint8_t* blocks, size_t size);

// The keycap shown for an icon: RGBA8 with a full mip chain, 4x the icon's
// guest size (the title draws icons up to about that size at internal 4x).
texture_pack::DdsImage RenderKeycap(const Icon& icon, const std::vector<ButtonLabel>& labels);

}  // namespace rex::graphics::prompt_icons
