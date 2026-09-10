#pragma once

/*
 * Single source of truth for the MJPEG strangler runtime selection.
 *
 * Production stages remain compiled and host-tested while a value of 0 keeps
 * the proven legacy body active. Flip one role per firmware marker only.
 */
#define EMOTION_VIDEO_USE_INDEX_READER 1
#define EMOTION_VIDEO_USE_FRAME_SOURCE 1
#define EMOTION_VIDEO_USE_DECODER_STAGE 0
#define EMOTION_VIDEO_USE_PLAYBACK_CLOCK 0

/* PresentSink is isolated from Decoder/Clock: it only preserves the proven
 * callback/WDT observation order and retains the complete legacy R0 branch. */
#define EMOTION_VIDEO_USE_PRESENT_SINK 1
