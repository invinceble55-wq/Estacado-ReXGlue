/**
 * @file        ui/overlay/shader_prewarm_indicator.cpp
 * @brief       See shader_prewarm_indicator.h.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#include <rex/ui/overlay/shader_prewarm_indicator.h>
#include <imgui.h>

#include <algorithm>
#include <utility>

namespace rex::ui {

ShaderPrewarmIndicatorDialog::ShaderPrewarmIndicatorDialog(ImGuiDrawer* imgui_drawer,
                                                           ProgressProvider provider)
    : ImGuiDialog(imgui_drawer), provider_(std::move(provider)) {
  if (provider_) batch_start_ = provider_().handled;
}

ShaderPrewarmIndicatorDialog::~ShaderPrewarmIndicatorDialog() = default;

void ShaderPrewarmIndicatorDialog::OnDraw(ImGuiIO& io) {
  (void)io;
  if (!provider_) return;
  const ShaderPrewarmProgress progress = provider_();
  if (progress.handled >= progress.queued || progress.queued <= batch_start_) return;
  const uint32_t total = progress.queued - batch_start_;
  const uint32_t done = std::min(progress.handled - batch_start_, total);
  ImGui::SetNextWindowPos(ImVec2(12.0f, 12.0f), ImGuiCond_Always);
  ImGui::SetNextWindowBgAlpha(0.55f);
  constexpr ImGuiWindowFlags kFlags =
      ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoMove |
      ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoInputs |
      ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoFocusOnAppearing;
  if (ImGui::Begin("##shader_prewarm", nullptr, kFlags)) {
    ImGui::Text("Preparing shaders  %u / %u", done, total);
    ImGui::ProgressBar(float(done) / float(total), ImVec2(200.0f, 5.0f), "");
  }
  ImGui::End();
}

}  // namespace rex::ui
