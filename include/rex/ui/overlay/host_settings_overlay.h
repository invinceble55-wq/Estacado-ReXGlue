/**
 * @file        rex/ui/overlay/host_settings_overlay.h
 * @brief       In-game settings overlay driven by the host's settings schema.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#pragma once

// The embedding host describes its settings (rex/ui/settings_schema.h) and
// the saved values; the overlay renders them with the shared settings panel
// and queues every edit for the host, which saves it through its own
// validated configuration writer and confirms with the new saved values.
// Settings marked live are also applied immediately by the caller's live
// callback. The overlay never opens configuration files itself.

#include <rex/ui/imgui_dialog.h>
#include <rex/ui/settings_detection.h>
#include <rex/ui/settings_panel.h>

#include <cstdint>
#include <functional>
#include <mutex>
#include <string>

struct ImFont;

namespace rex::ui {

// Shared between the host-facing exports (any thread) and the dialog (UI
// thread); every field is guarded by mutex.
struct HostSettingsState {
  std::mutex mutex;
  settings::Schema schema;
  settings::Values saved;
  settings::Values defaults;
  settings::Values pending;  // edits not yet collected by the host
  std::string status;        // host's last save result
  bool configured = false;
  bool persistence = false;  // the host saves edits (false: e.g. safe mode)
  uint64_t generation = 0;   // bumps whenever schema/saved/status change
  double refresh_hz = 0.0;   // the game window's display (0: unknown)
  // Installed HD texture packs, scanned by the host when the overlay opens.
  settings::TexturePackFolder texture_pack;
  uint64_t video_memory_bytes = 0;  // the graphics card's own (0: unknown)
  std::string texture_pack_where;   // where packs go, for the note
  uint64_t texture_pack_generation = 0;  // bumps with every scan
  // Vendor upscalers this build can run (runtime DLL present and compiled in).
  settings::UpscalerRuntimes upscalers;
  bool upscalers_known = false;
};

// Font setup for the overlay's ImGui drawer: adds the UI font (Segoe UI on
// Windows when present) and reports it through *font (null: drawer default).
ImGuiDrawer::FontSetupCallback HostSettingsFontSetup(ImFont** font);

// Owned by raw pointer: the owner deletes it outside of drawing, or the
// dialog closes itself (Resume) and reports that through `close` just before
// deleting itself (ImGuiDialog::Close semantics).
class HostSettingsOverlayDialog : public ImGuiDialog {
 public:
  using LiveCallback = std::function<void(const settings::Setting&, const std::string&)>;
  using CloseCallback = std::function<void()>;
  using QuitCallback = std::function<void()>;

  // `quit` (optional) adds a "Quit game" button beside Resume; after the
  // player confirms, it asks the owner to end the game (djanice1980, #7).
  HostSettingsOverlayDialog(ImGuiDrawer* imgui_drawer, HostSettingsState& state, ImFont* font,
                            std::string toggle_key_name, LiveCallback live,
                            CloseCallback close, QuitCallback quit = nullptr);
  ~HostSettingsOverlayDialog() override;

  // A key editor waits for a key: Escape is then a binding, not "close".
  bool CapturingKey() const { return settings::IsCapturingKey(model_); }

 protected:
  void OnDraw(ImGuiIO& io) override;
  void OnClose() override;

 private:
  void Sync();

  HostSettingsState& state_;
  ImFont* font_ = nullptr;
  std::string toggle_key_name_;
  LiveCallback live_;
  CloseCallback close_;
  QuitCallback quit_;
  settings::PanelModel model_;
  settings::Schema schema_;
  std::string status_;
  bool persistence_ = false;
  bool configured_ = false;
  bool styled_ = false;
  uint64_t seen_generation_ = UINT64_MAX;
  double detected_refresh_hz_ = -1.0;
  uint64_t seen_texture_pack_generation_ = 0;
};

}  // namespace rex::ui
