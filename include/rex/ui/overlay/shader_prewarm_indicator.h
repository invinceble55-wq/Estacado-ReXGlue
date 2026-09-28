/**
 * @file        rex/ui/overlay/shader_prewarm_indicator.h
 *
 * @brief       A small progress bar while pipelines are being prepared ahead
 *              of their first use (Estacado 0.9.1, D3D12 shader prewarm).
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#pragma once
#include <rex/ui/imgui_dialog.h>
#include <cstdint>
#include <functional>

namespace rex::ui {

// Pipelines queued and handled so far (monotonic counters of the session).
struct ShaderPrewarmProgress {
  uint32_t queued = 0;
  uint32_t handled = 0;
};

// Draws "Preparing shaders  done / total" with a thin bar in the top-left
// corner while the counters differ, counting from where they stood when the
// dialog was created; draws nothing otherwise. No input.
class ShaderPrewarmIndicatorDialog : public ImGuiDialog {
 public:
  using ProgressProvider = std::function<ShaderPrewarmProgress()>;

  ShaderPrewarmIndicatorDialog(ImGuiDrawer* imgui_drawer, ProgressProvider provider);
  ~ShaderPrewarmIndicatorDialog();

 protected:
  void OnDraw(ImGuiIO& io) override;

 private:
  ProgressProvider provider_;
  uint32_t batch_start_ = 0;
};

}  // namespace rex::ui
