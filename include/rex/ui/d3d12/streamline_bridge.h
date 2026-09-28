/**
 * @file        ui/d3d12/streamline_bridge.h
 * @brief       Optional NVIDIA Streamline (Reflex / PC Latency markers)
 *
 * Off unless the cvar streamline_reflex is non-zero at start: nothing is
 * loaded and every call is a no-op. When on, the signed sl.interposer.dll
 * beside the executable is verified and loaded at run time (no import
 * library), slInit runs before the Direct3D 12 device exists (manual hooking,
 * no over-the-air downloads) and the swap chain is created through
 * Streamline's proxy factory and queue. The guest frame ids for the Reflex
 * sleep and the PC Latency markers come from rex/ui/frame_latency.h. Frame
 * Generation will build on this. Builds without the SDK (player builds)
 * compile all of it as no-ops.
 *
 * @license     BSD 3-Clause License
 */

#pragma once

#include <cstdint>

struct ID3D12Device;
struct ID3D12CommandQueue;
struct ID3D12Resource;
struct IDXGIFactory2;
struct D3D12_COMMAND_QUEUE_DESC;

namespace rex::ui::d3d12::streamline {

// Device setup (D3D12Provider). InitializeBeforeDevice loads and initializes
// Streamline when the cvar asks for it; false when off, unavailable or
// refused (then nothing else does anything).
bool InitializeBeforeDevice();
bool IsActive();
void OnDeviceCreated(ID3D12Device* native_device, uint64_t adapter_luid);
// Creates the direct queue through Streamline's device proxy, keeping the
// proxy for swap chain creation and returning the native queue (with its own
// reference) for every submission. False when inactive or on failure.
bool CreateDirectQueue(ID3D12Device* native_device, const D3D12_COMMAND_QUEUE_DESC& desc,
                       ID3D12CommandQueue** native_queue_out);
void UpgradeFactory(IDXGIFactory2* native_factory);
// Streamline's proxies for swap chain creation, or nullptr when not in use.
IDXGIFactory2* SwapChainFactory();
ID3D12CommandQueue* SwapChainQueue();
// Before the native device, queue and factory are released.
void Shutdown();

// Per guest frame (ids from rex::ui::frame_latency). Producer thread at the
// frame start: Reflex mode changes and the Reflex sleep.
void ReflexSleep(uint32_t frame);
// PC Latency markers (the values of sl::PCLMarker).
enum class Marker : uint32_t {
  kSimulationStart = 0,
  kSimulationEnd = 1,
  kRenderSubmitStart = 2,
  kRenderSubmitEnd = 3,
  kPresentStart = 4,
  kPresentEnd = 5,
  kLatencyPing = 8,
  kControllerInputSample = 13,
};
void SetMarker(Marker marker, uint32_t frame);
// A PC Latency ping arrived since the last call (the frame start marks it).
bool TakeLatencyPing();
// Developer lines: NVIDIA's latency reports (REX_STREAMLINE_LATENCY / _FRAME).
void LogReport();

// DLSS Frame Generation (development test, streamline_frame_generation):
// loaded, supported and ready.
bool FrameGenerationActive();
// Frames presented per rendered frame while Frame Generation is on (0 off).
uint32_t FrameGenerationPresentedPerFrame();
// A frame's camera for Frame Generation (row-major, column vectors).
struct FrameGenerationCamera {
  // clip = view_to_clip * (p - position, 1): the title's rotation-only
  // view-projection (camera space = world axes at the camera).
  float view_to_clip[16];
  // Current clip -> previous frame's clip; identity on a cut.
  float clip_to_previous_clip[16];
  float position[3];
  float fov_y;
  float near_plane;
  float far_plane;
  bool reset;
};
// Command processor at the frame's swap: its inputs (typed scene depth,
// reversed; motion vectors in pixels, previous - current, unjittered), kept
// unchanged and in NON_PIXEL_SHADER_RESOURCE until the frame's present.
// Null resources: the frame has no inputs (Frame Generation stays off).
void FrameGenerationSetInputs(uint32_t frame, ID3D12Resource* depth, ID3D12Resource* motion,
                              uint32_t width, uint32_t height,
                              const FrameGenerationCamera* camera);
// Presenter, right before every Present: guest_frame = the image's frame (0
// untracked), frame = guest_frame on its first present (0 for a repaint),
// gameplay = not a menu or loading screen. Turns Frame Generation on or off
// and tags the frame's inputs (FrameGenerationSetInputs; placeholders when
// the tracking gives none and streamline_frame_generation_placeholders).
void FrameGenerationBeforePresent(uint32_t guest_frame, uint32_t frame, bool gameplay,
                                  uint32_t width, uint32_t height);

}  // namespace rex::ui::d3d12::streamline
