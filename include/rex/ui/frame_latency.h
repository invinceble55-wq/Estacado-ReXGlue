/**
 * @file        ui/frame_latency.h
 * @brief       Guest frame identity, the low-latency frame-queue limit and
 *              the input-to-present measurement
 *
 * A host-side, read-only view of the title's frames: an id from the title's
 * frame start on its producer thread, through its builder and its guest swap
 * (render thread), the command processor's swap, to the host present of that
 * frame's guest output image. Uses:
 * - The frame-queue limit (player setting display_low_latency, on: 2 frames
 *   with a half-frame lead; developer display_low_latency_queue/_lead_us
 *   override it): the frame start waits (bounded) until the frame that many
 *   frames before it has been presented, so the title reads its input closer
 *   to the display (a host wait at an existing boundary, like the frame
 *   limiter; no guest state changes).
 * - NVIDIA Streamline (Reflex sleep and PC Latency markers), which takes its
 *   frame ids from here.
 * - frame_latency_report (developer): input-to-present and pairing lines.
 * Nothing is tracked unless one of them is on.
 *
 * @license     BSD 3-Clause License
 */

#pragma once

#include <cstdint>

namespace rex::ui::frame_latency {

// Producer thread, at the title's frame start (before its frame-pool wait
// and input processing): the queue limit, the Reflex sleep, a new frame.
void FrameStart();
// Producer thread: the title reads its input for the frame.
void InputSample();
// Producer thread, at the frame's builder: the frame will be swapped.
void FrameBuilt();
// Render thread, in VdSwap: the oldest built frame's commands are complete.
void GuestSwap();
// Command processor, at its swap: BeginRenderSubmit takes the frame,
// EndRenderSubmit follows the frame's final submission.
void BeginRenderSubmit();
void EndRenderSubmit();
// The frame of the swap the command processor is processing (0 = none); the
// presenter keeps it with the guest output image refreshed for that swap.
uint32_t RenderSubmitFrame();
// Presenter, around Present of a guest output image's frame. BeginPresent
// returns the frame to pass to EndPresent (0 for a repaint or no frame).
uint32_t BeginPresent(uint32_t frame);
void EndPresent(uint32_t frame);

// The title shows a menu, the title screen or a loading screen (the plugin's
// view, rex_gpu_embedded_set_menu_state): Frame Generation stays off then.
void SetMenuState(bool in_menu);
bool InMenu();

}  // namespace rex::ui::frame_latency
