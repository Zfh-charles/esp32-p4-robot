#include "mjpeg_playback_clock.h"

#include <string.h>

#if defined(__GNUC__)
#define ZERO_TAX_ENTRY __attribute__((noinline, noipa))
#define ZERO_TAX_INLINE __attribute__((always_inline)) inline
#define ZERO_TAX_BARRIER(value) \
    __asm__ __volatile__("" : "+r"(value) : : "memory")
#else
#define ZERO_TAX_ENTRY
#define ZERO_TAX_INLINE inline
#define ZERO_TAX_BARRIER(value) ((void)(value))
#endif

bool mjpeg_playback_clock_init(
    mjpeg_playback_clock_state_t *state,
    uint32_t normal_interval_us,
    uint64_t now_us)
{
    if (state == NULL || normal_interval_us == 0U) {
        return false;
    }
    memset(state, 0, sizeof(*state));
    state->paused = true;
    state->normal_frame_interval_us = normal_interval_us;
    state->frame_interval_us = normal_interval_us;
    state->deadline_us = now_us;
    state->last_frame_time_us = now_us;
    state->last_decode_success_us = now_us;
    return true;
}

void mjpeg_playback_clock_pause(mjpeg_playback_clock_state_t *state)
{
    if (state != NULL) {
        state->paused = true;
    }
}

bool mjpeg_playback_clock_resume(
    mjpeg_playback_clock_state_t *state,
    uint64_t now_us,
    uint32_t normal_interval_us)
{
    if (state == NULL || normal_interval_us == 0U) {
        return false;
    }
    state->normal_frame_interval_us = normal_interval_us;
    state->warmup_until_us = now_us + MJPEG_CLOCK_WARMUP_US;
    state->soft_start_until_us = now_us + MJPEG_CLOCK_SOFT_START_US;
    state->deadline_us = state->warmup_until_us;
    state->last_decode_success_us = now_us;
    state->last_stall_diag_us = 0U;
    state->frame_interval_us = normal_interval_us < MJPEG_CLOCK_RESUME_MIN_INTERVAL_US
                                   ? MJPEG_CLOCK_RESUME_MIN_INTERVAL_US
                                   : normal_interval_us;
    state->paused = false;
    return true;
}

static ZERO_TAX_INLINE mjpeg_playback_clock_poll_t poll_direct(
    mjpeg_playback_clock_state_t *state,
    uint64_t now_us)
{
    mjpeg_playback_clock_poll_t poll = {0};
    if (state == NULL || state->paused) {
        poll.action = MJPEG_CLOCK_ACTION_PAUSED;
        poll.wait_us = 10000U;
        return poll;
    }
    if (state->warmup_until_us > now_us) {
        poll.action = MJPEG_CLOCK_ACTION_WARMUP;
        poll.wait_us = 5000U;
        return poll;
    }
    if (state->soft_start_until_us > now_us) {
        if (state->frame_interval_us < MJPEG_CLOCK_LOOP_SOFT_MIN_INTERVAL_US) {
            state->frame_interval_us = MJPEG_CLOCK_LOOP_SOFT_MIN_INTERVAL_US;
        }
    } else if (state->frame_interval_us != state->normal_frame_interval_us) {
        state->frame_interval_us = state->normal_frame_interval_us;
        poll.soft_start_finished = true;
    }
    if (now_us - state->last_decode_success_us > MJPEG_CLOCK_HANG_THRESHOLD_US) {
        poll.stall_detected = true;
        poll.yield_now = true;
        if (now_us - state->last_stall_diag_us > MJPEG_CLOCK_STALL_DIAG_INTERVAL_US) {
            state->last_stall_diag_us = now_us;
            poll.stall_diag = true;
        }
        state->deadline_us = now_us + state->frame_interval_us;
        poll.action = MJPEG_CLOCK_ACTION_WAIT;
        poll.wait_us = state->frame_interval_us;
        return poll;
    }
    if (now_us >= state->deadline_us) {
        poll.action = MJPEG_CLOCK_ACTION_DECODE;
        return poll;
    }
    poll.action = MJPEG_CLOCK_ACTION_WAIT;
    poll.wait_us = state->deadline_us - now_us;
    return poll;
}

ZERO_TAX_ENTRY mjpeg_playback_clock_poll_t mjpeg_playback_clock_poll(
    mjpeg_playback_clock_state_t *state,
    uint64_t now_us)
{
    ZERO_TAX_BARRIER(state);
    return poll_direct(state, now_us);
}

ZERO_TAX_ENTRY mjpeg_playback_clock_poll_t mjpeg_playback_clock_legacy_poll(
    mjpeg_playback_clock_state_t *state,
    uint64_t now_us)
{
    return poll_direct(state, now_us);
}

static ZERO_TAX_INLINE mjpeg_playback_clock_effect_t result_direct(
    mjpeg_playback_clock_state_t *state,
    mjpeg_playback_decode_result_t result,
    uint64_t now_us,
    uint32_t yield_interval)
{
    mjpeg_playback_clock_effect_t effect = {0};
    if (state == NULL) {
        return effect;
    }
    if (result == MJPEG_CLOCK_DECODE_OK) {
        state->last_frame_time_us = now_us;
        state->last_decode_success_us = now_us;
        state->current_frame++;
        state->decode_error_count = 0U;
        state->deadline_us = now_us + state->frame_interval_us;
        effect.delay_one_tick =
            yield_interval != 0U && state->current_frame % yield_interval == 0U;
    } else if (result == MJPEG_CLOCK_DECODE_EOF) {
        state->deadline_us = now_us;
        state->last_decode_success_us = now_us;
        effect.stream_ended = true;
    } else {
        state->decode_error_count++;
        effect.enter_error_state = state->decode_error_count > 10U;
    }
    return effect;
}

ZERO_TAX_ENTRY mjpeg_playback_clock_effect_t
mjpeg_playback_clock_on_decode_result(
    mjpeg_playback_clock_state_t *state,
    mjpeg_playback_decode_result_t result,
    uint64_t now_us,
    uint32_t yield_interval)
{
    ZERO_TAX_BARRIER(state);
    return result_direct(state, result, now_us, yield_interval);
}

ZERO_TAX_ENTRY mjpeg_playback_clock_effect_t
mjpeg_playback_clock_legacy_on_decode_result(
    mjpeg_playback_clock_state_t *state,
    mjpeg_playback_decode_result_t result,
    uint64_t now_us,
    uint32_t yield_interval)
{
    return result_direct(state, result, now_us, yield_interval);
}
