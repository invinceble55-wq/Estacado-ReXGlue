/**
 * @file        ui/overlay/achievement_toast.cpp
 * @brief       Achievement toast implementation. See achievement_toast.h for details.
 *
 * @copyright   Copyright (c) 2026 Rien Gupta <rgupta9@scu.edu>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#include <rex/ui/overlay/achievement_toast.h>
#include <imgui.h>

#include <algorithm>

#include <rex/ui/immediate_drawer.h>

namespace rex::ui {

AchievementToastDialog::AchievementToastDialog(ImGuiDrawer* drawer,
                                               ImmediateDrawer* immediate_drawer,
                                               rex::Runtime* runtime)
    : AchievementNotificationDialog(drawer), icon_cache_(immediate_drawer, runtime) {}

AchievementToastDialog::~AchievementToastDialog() {}

void AchievementToastDialog::Push(const rex::system::AchievementEvent& event) {
  std::lock_guard<std::mutex> lock(mutex_);
  queue_.push_back({event, std::chrono::steady_clock::now()});
}

void AchievementToastDialog::OnDraw(ImGuiIO& io) {
  auto now = std::chrono::steady_clock::now();

  PendingToast toast;
  float age = 0.0f;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    while (!queue_.empty()) {
      if (!queue_.front().visible_since) {
        queue_.front().visible_since = now;
      }
      age = std::chrono::duration<float>(now - *queue_.front().visible_since).count();
      if (age >= kDisplaySeconds) {
        queue_.pop_front();
      } else {
        break;
      }
    }
    if (queue_.empty()) {
      return;
    }
    toast = queue_.front();
  }

  float alpha = 1.0f;
  if (age < 0.3f)
    alpha = age / 0.3f;
  else if (age > kDisplaySeconds - 0.5f)
    alpha = (kDisplaySeconds - age) / 0.5f;
  alpha = std::max(0.0f, std::min(1.0f, alpha));

  // Sized from the display height (18 px text at 1080p), bottom right.
  const float text_size = std::clamp(io.DisplaySize.y / 1080.0f * 18.0f, 14.0f, 40.0f);
  if (font_) ImGui::PushFont(font_, text_size);
  const float pad = ImGui::GetFontSize();
  ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - pad, io.DisplaySize.y - pad * 3.0f),
                          ImGuiCond_Always, ImVec2(1.0f, 1.0f));
  ImGui::SetNextWindowSizeConstraints(ImVec2(ImGui::GetFontSize() * 15.0f, 0.0f),
                                      ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.3f));
  ImGui::SetNextWindowBgAlpha(0.88f * alpha);

  ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoNav |
                           ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                           ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_AlwaysAutoResize |
                           ImGuiWindowFlags_NoFocusOnAppearing;
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, pad * 0.35f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(pad * 0.75f, pad * 0.6f));
  ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 1.0f, alpha));
  if (ImGui::Begin("##ach_toast", nullptr, flags)) {
    const float icon_size = ImGui::GetFontSize() * 2.5f;
    ImmediateTexture* icon = icon_cache_.GetIcon(toast.event.achievement);
    if (icon) {
      ImGui::Image(reinterpret_cast<ImTextureID>(icon), ImVec2(icon_size, icon_size));
      ImGui::SameLine();
    }

    ImGui::BeginGroup();
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.85f, 0.2f, alpha));
    ImGui::TextUnformatted("Achievement Unlocked");
    ImGui::PopStyleColor();
    ImGui::TextUnformatted(toast.event.achievement.label.c_str());
    ImGui::SameLine();
    ImGui::TextDisabled(" (%dG)", static_cast<int>(toast.event.achievement.gamerscore));
    ImGui::EndGroup();
  }
  ImGui::End();
  ImGui::PopStyleColor();
  ImGui::PopStyleVar(2);
  if (font_) ImGui::PopFont();
}

bool AchievementToastDialog::Busy() {
  std::lock_guard<std::mutex> lock(mutex_);
  return !queue_.empty();
}

}  // namespace rex::ui
