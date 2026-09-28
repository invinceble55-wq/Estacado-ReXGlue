/**
 * @file        rex/ui/d3d12/openxr_screen.h
 * @brief       VR (experimental): stereo frames, head and hand tracking.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#pragma once

// Experimental (display_vr / display_vr_simulate, off by default; docs/
// VR_PLAN.md). An OpenXR session on the presenter's D3D12 device:
// - Stereo (gameplay): the game renders alternate eyes (the runtime moves the
//   render view by the head pose and half the eye distance each frame and
//   tags the frame's swap with the view it used); the command processor
//   copies each finished HUD-less frame into that eye's image and the HUD
//   (finished frame minus HUD-less) into a HUD image; the presenter submits a
//   projection layer with the poses the frames were rendered with and the HUD
//   on a plane in front of the player.
// - Flat (menus, loading, frames without a scene): the presented image as a
//   screen 2 m in front of the player (stage 1).
// display_vr_simulate (sbs, anaglyph) runs the stereo path without a headset
// and shows the two eyes on the monitor (verification).
// openxr_loader.dll is loaded from the executable's folder only when
// display_vr is on; without it, a runtime or a headset the game stays flat.

#include <cstdint>

struct ID3D12Device;
struct ID3D12CommandQueue;
struct ID3D12Fence;
struct ID3D12GraphicsCommandList;
struct ID3D12Resource;

namespace rex::ui::d3d12::openxr_screen {

// The head pose for the next rendered frame (OpenXR axes: +x right, +y up,
// -z forward; metres) and the eyes relative to it.
struct HeadView {
  uint32_t id = 0;
  float orientation[4] = {0.0f, 0.0f, 0.0f, 1.0f};  // x y z w, in the play space
  float position[3] = {};
  float eye_offset[2][3] = {};  // head axes
  // Symmetric field of view the game renders each eye with.
  float tan_half_fov_x = 1.0f;
  float tan_half_fov_y = 0.5625f;
  // Hand aim (stage 2): the title's camera follows the right hand's yaw and
  // pitch (look input), so the head is rendered relative to that aim.
  bool aim_valid = false;
  float aim[4] = {0.0f, 0.0f, 0.0f, 1.0f};
};

// Hand controllers (stage 2-4), in the play space.
struct Hand {
  bool valid = false;
  float orientation[4] = {0.0f, 0.0f, 0.0f, 1.0f};  // aim pose
  float position[3] = {};
  float trigger = 0.0f;
  float grip = 0.0f;
  float stick[2] = {};
  // Bit 0 A/X, 1 B/Y, 2 stick click, 3 menu.
  uint32_t buttons = 0;
};
struct Controllers {
  Hand hand[2];  // 0 left, 1 right
};

// VR is configured (display_vr or display_vr_simulate): the command
// processor captures the HUD-less frame for the eye images.
bool Wanted();
// Look input for the title (hand aim and turning), radians since the last
// call: positive yaw turns right, positive pitch looks up.
void TakeLook(double& yaw, double& pitch);
// Comfort vignette strength for the next eye images (0 = none).
float VignetteStrength();
// The right hand's laser pointer on the flat screen (stage 4): screen
// coordinates 0..1 and the trigger, while the screen is shown.
bool Pointer(float& u, float& v, bool& pressed);
// The host's overlays (F1 settings, dialogs) are drawn on the presented
// image: the headset then also shows that image as a screen.
void SetOverlayVisible(bool visible);
// Stereo rendering is wanted now (a running session or the simulation).
bool StereoActive();
// The latest head pose (any thread); false while stereo is off.
bool LatestHeadView(HeadView& out);
bool LatestControllers(Controllers& out);

// Frame tag carried by the guest swap (the runtime writes it; 0 = none):
// bit 31 set, bit 30 = eye (1 right), bits 0-29 the HeadView id.
constexpr uint32_t kFrameTagValid = 0x80000000u;
constexpr uint32_t kFrameTagRightEye = 0x40000000u;
constexpr uint32_t kFrameTagIdMask = 0x3FFFFFFFu;

// Command processor, at a tagged swap whose frame has a scene: an eye image
// (render size, R8G8B8A8_UNORM UAV, sRGB-encoded values) and a HUD image
// (same, alpha = HUD) to write in its command list; Published() after
// recording, with the fence value that list signals. Null: nothing wanted.
ID3D12Resource* EyeTarget(ID3D12Device* device, uint32_t tag, uint32_t width, uint32_t height);
ID3D12Resource* HudTarget(ID3D12Device* device, uint32_t width, uint32_t height);
void Published(uint32_t tag, ID3D12Fence* fence, uint64_t fence_value);
// The simulation's monitor preview: the newest written image of each eye and
// the HUD (null until both eyes exist); mode 4 anaglyph, 8 side by side.
bool PreviewSources(ID3D12Resource*& left, ID3D12Resource*& right, ID3D12Resource*& hud,
                    uint32_t& mode);

// Presenter, before the back buffer leaves the render target state: copies
// the eye and HUD images into the headset's (recorded into list) and returns
// the flat screen image to copy the back buffer into (in
// D3D12_RESOURCE_STATE_RENDER_TARGET, same size, B8G8R8A8 family), or null.
// May wait for the headset's frame timing (xrWaitFrame).
ID3D12Resource* BeginFrame(ID3D12Device* device, ID3D12CommandQueue* queue, uint32_t width,
                           uint32_t height, ID3D12GraphicsCommandList* list);

// After the paint command list was executed: hands the images to the
// runtime (copied = the flat copy was recorded into that command list).
void EndFrame(bool copied);

// Ends the session (presenter teardown).
void Shutdown();

}  // namespace rex::ui::d3d12::openxr_screen
