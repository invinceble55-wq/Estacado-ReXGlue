/**
 * @file        ui/overlay/host_settings_overlay.cpp
 * @brief       In-game settings overlay. See host_settings_overlay.h.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#include <rex/ui/overlay/host_settings_overlay.h>
#include <rex/ui/rtl_text.h>
#include <rex/ui/settings_detection.h>

#include <SDL3/SDL_stdinc.h>
#include <SDL3/SDL_video.h>
#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <filesystem>

namespace rex::ui {
namespace {

// Connected displays, in SDL order (the order the host's display index uses).
std::vector<settings::Choice> DetectDisplays() {
  std::vector<settings::Choice> choices;
  int count = 0;
  SDL_DisplayID* displays = SDL_GetDisplays(&count);
  for (int index = 0; displays && index < count; ++index) {
    std::string label = std::to_string(index + 1) + ": ";
    const char* name = SDL_GetDisplayName(displays[index]);
    label += name ? name : "Display";
    if (const SDL_DisplayMode* mode = SDL_GetDesktopDisplayMode(displays[index])) {
      label += " (" + std::to_string(mode->w) + " x " + std::to_string(mode->h) + ")";
    }
    choices.push_back({std::to_string(index), std::move(label)});
  }
  SDL_free(displays);
  return choices;
}

}  // namespace

ImGuiDrawer::FontSetupCallback HostSettingsFontSetup(ImFont** font) {
  return [font](ImFontAtlas* atlas) {
    *font = nullptr;
#if defined(_WIN32)
    const char* path = "C:\\Windows\\Fonts\\segoeui.ttf";
    if (std::filesystem::exists(path)) {
      // Latin plus Arabic and its presentation forms (the Arabic interface
      // draws pre-shaped text, rex/ui/rtl_text.h).
      static const ImWchar kRanges[] = {0x0020, 0x024F, 0x0600, 0x06FF, 0x2000, 0x206F,
                                        0xFB50, 0xFDFF, 0xFE70, 0xFEFF, 0};
      *font = atlas->AddFontFromFileTTF(path, 18.0f, nullptr, kRanges);
    }
#endif
    (void)atlas;
  };
}

HostSettingsOverlayDialog::HostSettingsOverlayDialog(ImGuiDrawer* imgui_drawer,
                                                     HostSettingsState& state, ImFont* font,
                                                     std::string toggle_key_name,
                                                     LiveCallback live, CloseCallback close,
                                                     QuitCallback quit, GamepadCallback gamepad)
    : ImGuiDialog(imgui_drawer),
      state_(state),
      font_(font),
      toggle_key_name_(std::move(toggle_key_name)),
      live_(std::move(live)),
      close_(std::move(close)),
      quit_(std::move(quit)),
      gamepad_(std::move(gamepad)) {}

HostSettingsOverlayDialog::~HostSettingsOverlayDialog() = default;

void HostSettingsOverlayDialog::Sync() {
  std::lock_guard<std::mutex> lock(state_.mutex);
  if (state_.generation == seen_generation_ && state_.refresh_hz == detected_refresh_hz_ &&
      state_.texture_pack_generation == seen_texture_pack_generation_) {
    return;
  }
  seen_generation_ = state_.generation;
  configured_ = state_.configured;
  persistence_ = state_.persistence;
  status_ = state_.status;
  bool detect = state_.refresh_hz != detected_refresh_hz_;
  bool detect_pack = state_.texture_pack_generation != seen_texture_pack_generation_;
  if (schema_.settings.size() != state_.schema.settings.size()) {
    schema_ = state_.schema;
    const auto displays = DetectDisplays();
    for (const settings::Setting& setting : schema_.settings) {
      if (setting.editor == settings::Editor::kDisplay && !displays.empty()) {
        model_.detected_choices[setting.key] = displays;
      }
    }
    detect = true;
    detect_pack = true;
  }
  model_.schema = &schema_;
  if (detect) {
    detected_refresh_hz_ = state_.refresh_hz;
    settings::DetectFrameRateChoices(model_, detected_refresh_hz_);
  }
  if (detect_pack) {
    seen_texture_pack_generation_ = state_.texture_pack_generation;
    if (seen_texture_pack_generation_) {
      settings::DetectTexturePack(model_, state_.texture_pack, state_.video_memory_bytes,
                                  state_.texture_pack_where);
    }
  }
  model_.saved = state_.saved;
  model_.defaults = state_.defaults;
  if (state_.upscalers_known) settings::DetectUpscalers(model_, state_.upscalers);
  // Edits the host has saved are no longer edits.
  for (auto it = model_.values.begin(); it != model_.values.end();) {
    const auto saved = model_.saved.find(it->first);
    const bool pending = state_.pending.count(it->first) != 0;
    if (!pending && saved != model_.saved.end() && saved->second == it->second) {
      it = model_.values.erase(it);
    } else {
      ++it;
    }
  }
}

bool HostSettingsOverlayDialog::FeedGamepad(ImGuiIO& io) {
  // XInput wButtons bits. Start and Back are left out: they are the chord
  // that opens and closes the overlay.
  constexpr uint16_t kDpadUp = 0x0001, kDpadDown = 0x0002, kDpadLeft = 0x0004,
                     kDpadRight = 0x0008, kShoulderL = 0x0100, kShoulderR = 0x0200,
                     kA = 0x1000, kB = 0x2000, kX = 0x4000, kY = 0x8000;
  static constexpr struct {
    uint16_t bit;
    ImGuiKey key;
  } kButtons[] = {
      {kDpadUp, ImGuiKey_GamepadDpadUp},     {kDpadDown, ImGuiKey_GamepadDpadDown},
      {kDpadLeft, ImGuiKey_GamepadDpadLeft}, {kDpadRight, ImGuiKey_GamepadDpadRight},
      {kShoulderL, ImGuiKey_GamepadL1},      {kShoulderR, ImGuiKey_GamepadR1},
      {kA, ImGuiKey_GamepadFaceDown},        {kB, ImGuiKey_GamepadFaceRight},
      {kX, ImGuiKey_GamepadFaceLeft},        {kY, ImGuiKey_GamepadFaceUp},
  };
  const OverlayGamepadState pad = gamepad_();
  io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
  // Buttons already held when the overlay opened (the chord, or A from the
  // game) count only after they are released and pressed again.
  if (!gamepad_primed_) {
    gamepad_suppressed_ = pad.buttons;
    gamepad_primed_ = true;
  }
  gamepad_suppressed_ &= pad.buttons;
  const uint16_t buttons = pad.buttons & ~gamepad_suppressed_;
  for (const auto& button : kButtons) io.AddKeyEvent(button.key, (buttons & button.bit) != 0);
  // Left stick, with the XInput dead zone.
  constexpr float kDeadZone = 7849.0f;
  auto axis = [&](ImGuiKey negative, ImGuiKey positive, int16_t raw) {
    const float value = float(raw);
    const float amount =
        std::clamp((std::abs(value) - kDeadZone) / (32767.0f - kDeadZone), 0.0f, 1.0f);
    io.AddKeyAnalogEvent(negative, value < -kDeadZone, value < 0.0f ? amount : 0.0f);
    io.AddKeyAnalogEvent(positive, value > kDeadZone, value > 0.0f ? amount : 0.0f);
  };
  axis(ImGuiKey_GamepadLStickLeft, ImGuiKey_GamepadLStickRight, pad.thumb_lx);
  axis(ImGuiKey_GamepadLStickDown, ImGuiKey_GamepadLStickUp, pad.thumb_ly);
  const bool b_pressed = (buttons & kB) && !(gamepad_previous_ & kB);
  gamepad_previous_ = buttons;
  return b_pressed;
}

void HostSettingsOverlayDialog::OnDraw(ImGuiIO& io) {
  Sync();
  const bool gamepad_back = gamepad_ && FeedGamepad(io);
  if (!styled_) {
    const float font_size = ImGui::GetStyle().FontSizeBase;
    settings::ApplyStyle(1.0f);
    ImGui::GetStyle().FontSizeBase = font_size;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;  // arrows/Tab/Space/Enter
    styled_ = true;
  }
  if (font_) ImGui::PushFont(font_, 18.0f);

  ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(0.0f, 0.0f), io.DisplaySize,
                                                IM_COL32(0, 0, 0, 150));
  const ImVec2 size(std::min(io.DisplaySize.x * 0.86f, 1120.0f),
                    std::min(io.DisplaySize.y * 0.86f, 740.0f));
  ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f),
                          ImGuiCond_Always, ImVec2(0.5f, 0.5f));
  ImGui::SetNextWindowSize(size, ImGuiCond_Always);
  ImGui::Begin("##host_settings", nullptr,
               ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                   ImGuiWindowFlags_NoSavedSettings);
  // A list or confirmation open since the last frame takes B for itself.
  const bool popup_was_open = ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId);
  const bool rtl = schema_.right_to_left;
  auto shown = [&](std::string_view text) {
    return rtl ? rtl::VisualLine(text) : std::string(text);
  };
  auto tr = [&](std::string_view english) { return settings::Translate(&schema_, english); };
  const std::string title = shown(tr("SETTINGS"));
  const std::string hint = shown(settings::FormatText(
      gamepad_ ? tr("{} or Esc to return to the game (controller: Back + Start or B)")
               : tr("{} or Esc to return to the game"),
      {toggle_key_name_}));
  if (rtl) {
    // Title on the right, the hint to its left.
    const ImGuiStyle& style = ImGui::GetStyle();
    const float width = ImGui::CalcTextSize(title.c_str()).x + style.ItemSpacing.x +
                        ImGui::CalcTextSize(hint.c_str()).x;
    const float available = ImGui::GetContentRegionAvail().x;
    if (available > width) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + available - width);
    ImGui::TextDisabled("%s", hint.c_str());
    ImGui::SameLine();
    ImGui::TextUnformatted(title.c_str());
  } else {
    ImGui::TextUnformatted(title.c_str());
    ImGui::SameLine();
    ImGui::TextDisabled("%s", hint.c_str());
  }
  ImGui::Separator();

  const float footer = ImGui::GetFrameHeightWithSpacing() + ImGui::GetStyle().ItemSpacing.y * 2;
  ImGui::BeginChild("##panel", ImVec2(0.0f, -footer), ImGuiChildFlags_None);
  if (!configured_) {
    ImGui::TextDisabled("%s", shown(tr("Settings are not available in this build.")).c_str());
  } else {
    const auto changes = settings::DrawPanel(model_, settings::Surface::kInGame);
    if (!changes.empty()) {
      std::lock_guard<std::mutex> lock(state_.mutex);
      for (const settings::Change& change : changes) {
        state_.pending[change.key] = change.value;
      }
      state_.status.clear();
      status_.clear();
    }
    for (const settings::Change& change : changes) {
      const settings::Setting* setting = schema_.Find(change.key);
      if (setting && setting->live && live_) live_(*setting, change.value);
    }
  }
  ImGui::EndChild();

  ImGui::Spacing();
  std::string footer_text;
  if (!persistence_) {
    footer_text = tr("Changes are not saved in this session (safe mode or test launch).");
  } else if (!status_.empty()) {
    footer_text = status_;
  } else {
    footer_text = tr("Changes are saved automatically. \"next start\" settings take effect the "
                     "next time the game starts.");
  }
  const float button = ImGui::GetFontSize() * 7.0f;
  const std::string resume = shown(tr("Resume")) + "##resume";
  auto resume_button = [&]() {
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.55f, 0.16f, 0.13f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.68f, 0.21f, 0.17f, 1.0f));
    // Close() deletes the dialog after this draw; OnClose tells the owner.
    if (ImGui::Button(resume.c_str(), ImVec2(button, 0.0f))) Close();
    ImGui::PopStyleColor(2);
  };
  // Quit game: asks first (progress since the last checkpoint is lost).
  bool quit_requested = false;
  const std::string quit = shown(tr("Quit game")) + "##quit";
  auto quit_button = [&]() {
    if (!quit_) return;
    if (ImGui::Button(quit.c_str(), ImVec2(button, 0.0f))) {
      ImGui::OpenPopup("##quitconfirm");
    }
    if (ImGui::BeginPopupModal("##quitconfirm", nullptr,
                               ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_AlwaysAutoResize)) {
      ImGui::TextUnformatted(shown(tr("Quit the game? Progress since the last checkpoint or "
                                      "save is lost."))
                                 .c_str());
      ImGui::Spacing();
      if (ImGui::Button((shown(tr("Quit game")) + "##quityes").c_str(), ImVec2(button, 0.0f))) {
        quit_requested = true;
        ImGui::CloseCurrentPopup();
      }
      ImGui::SameLine();
      if (ImGui::Button((shown(tr("Cancel")) + "##quitno").c_str(), ImVec2(button, 0.0f)) ||
          ImGui::IsKeyPressed(ImGuiKey_Escape, false) || gamepad_back) {
        ImGui::CloseCurrentPopup();
      }
      ImGui::EndPopup();
    }
  };
  const float buttons = quit_ ? button * 2.0f + ImGui::GetStyle().ItemSpacing.x : button;
  if (rtl) {
    // Mirrored: Resume on the left, the note right-aligned.
    resume_button();
    if (quit_) {
      ImGui::SameLine();
      quit_button();
    }
    ImGui::SameLine();
    const std::string note = shown(footer_text);
    const float available = ImGui::GetContentRegionAvail().x;
    const float width = ImGui::CalcTextSize(note.c_str()).x;
    if (available > width) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + available - width);
    ImGui::TextDisabled("%s", note.c_str());
  } else {
    ImGui::TextDisabled("%s", footer_text.c_str());
    ImGui::SameLine(ImGui::GetContentRegionMax().x - buttons);
    if (quit_) {
      quit_button();
      ImGui::SameLine();
    }
    resume_button();
  }
  // B at the top level (no open list or confirmation, nothing being edited,
  // no key being captured) returns to the game; inside, Dear ImGui cancels.
  const bool top_level = !popup_was_open &&
                         !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId) &&
                         !ImGui::IsAnyItemActive() && !CapturingKey();
  ImGui::End();
  if (font_) ImGui::PopFont();
  if (gamepad_back && top_level && !quit_requested) {
    Close();
    return;
  }
  if (quit_requested) {
    // The owner queues the window's close request (the game stops as after
    // Alt+F4); this dialog closes with it.
    quit_();
    Close();
  }
}

void HostSettingsOverlayDialog::OnClose() {
  if (close_) close_();
}

}  // namespace rex::ui
