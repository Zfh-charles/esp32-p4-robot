#ifndef MJPEG_PLAYBACK_CLOCK_CANDIDATE_H
#define MJPEG_PLAYBACK_CLOCK_CANDIDATE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MJPEG_CLOCK_WARMUP_US 300000ULL
#define MJPEG_CLOCK_SOFT_START_US 2000000ULL
#define MJPEG_CLOCK_RESUME_MIN_INTERVAL_US 200000U
#define MJPEG_CLOCK_LOOP_SOFT_MIN_INTERVAL_US 100000U
#define MJPEG_CLOCK_HANG_THRESHOLD_US 1500000ULL
#define MJPEG_CLOCK_STALL_DIAG_INTERVAL_US 1000000ULL

typedef enum {
    MJPEG_CLOCK_ACTION_PAUSED = 0,
    MJPEG_CLOCK_ACTION_WARMUP,
    MJPEG_CLOCK_ACTION_WAIT,
    MJPEG_CLOCK_ACTION_DECODE,
} mjpeg_playback_clock_action_t;

typedef enum {
    MJPEG_CLOCK_DECODE_OK = 0,
    MJPEG_CLOCK_DECODE_EOF,
    MJPEG_CLOCK_DECODE_ERROR,
} mjpeg_playback_decode_result_t;

typedef struct {
    bool paused;
    uint32_t frame_interval_us;
    uint32_t normal_frame_interval_us;
    uint64_t deadline_us;
    uint64_t warmup_until_us;
    uint64_t soft_start_until_us;
    uint64_t last_frame_time_us;
    uint64_t last_decode_success_us;
    uint64_t last_stall_diag_us;
    uint32_t current_frame;
    uint32_t decode_error_count;
} mjpeg_playback_clock_state_t;

typedef struct {
    mjpeg_playback_clock_action_t action;
    uint64_t wait_us;
    bool soft_start_finished;
    bool stall_detected;
    bool stall_diag;
    bool yield_now;
} mjpeg_playback_clock_poll_t;

typedef struct {
    bool stream_ended;
    bool enter_error_state;
    bool delay_one_tick;
} mjpeg_playback_clock_effect_t;

bool mjpeg_playback_clock_init(
    mjpeg_playback_clock_state_t *state,
    uint32_t normal_interval_us,
    uint64_t now_us);
void mjpeg_playback_clock_pause(mjpeg_playback_clock_state_t *state);
bool mjpeg_playback_clock_resume(
    mjpeg_playback_clock_state_t *state,
    uint64_t now_us,
    uint32_t normal_interval_us);
mjpeg_playback_clock_poll_t mjpeg_playback_clock_poll(
    mjpeg_playback_clock_state_t *state,
    uint64_t now_us);
mjpeg_playback_clock_effect_t mjpeg_playback_clock_on_decode_result(
    mjpeg_playback_clock_state_t *state,
    mjpeg_playback_decode_result_t result,
    uint64_t now_us,
    uint32_t yield_interval);

#ifdef __cplusplus
}
#endif

#endif
