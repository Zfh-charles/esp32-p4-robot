#ifndef MJPEG_PLAYBACK_CLOCK_PLAYER_ADAPTER_H
#define MJPEG_PLAYBACK_CLOCK_PLAYER_ADAPTER_H

#include "mjpeg_playback_clock.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t *frame_interval_us;
    uint32_t *normal_frame_interval_us;
    uint64_t *deadline_us;
    uint64_t *warmup_until_us;
    uint64_t *soft_start_until_us;
    uint64_t *last_frame_time_us;
    uint64_t *last_decode_success_us;
    uint64_t *last_stall_diag_us;
    uint32_t *current_frame;
    uint32_t *decode_error_count;
} mjpeg_playback_clock_player_binding_t;

mjpeg_playback_clock_poll_t mjpeg_playback_clock_player_poll(
    const mjpeg_playback_clock_player_binding_t *binding,
    uint64_t now_us,
    bool use_stage);
mjpeg_playback_clock_effect_t mjpeg_playback_clock_player_on_decode_result(
    const mjpeg_playback_clock_player_binding_t *binding,
    mjpeg_playback_decode_result_t result,
    uint64_t now_us,
    uint32_t yield_interval,
    bool use_stage);

/* P4 production specialization: selection stays outside the per-frame entry
 * and owner fields are updated directly, avoiding a temporary clock state. */
mjpeg_playback_clock_poll_t mjpeg_playback_clock_player_poll_direct(
    const mjpeg_playback_clock_player_binding_t *binding,
    uint64_t now_us);
mjpeg_playback_clock_effect_t mjpeg_playback_clock_player_on_decode_result_direct(
    const mjpeg_playback_clock_player_binding_t *binding,
    mjpeg_playback_decode_result_t result,
    uint64_t now_us,
    uint32_t yield_interval);

#ifdef __cplusplus
}
#endif

#endif
