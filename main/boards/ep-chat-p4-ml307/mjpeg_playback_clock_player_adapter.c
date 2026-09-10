#include "mjpeg_playback_clock_player_adapter.h"

#include <stddef.h>

static bool binding_valid(const mjpeg_playback_clock_player_binding_t *binding)
{
    return binding != NULL && binding->frame_interval_us != NULL &&
           binding->normal_frame_interval_us != NULL && binding->deadline_us != NULL &&
           binding->warmup_until_us != NULL && binding->soft_start_until_us != NULL &&
           binding->last_frame_time_us != NULL &&
           binding->last_decode_success_us != NULL && binding->last_stall_diag_us != NULL &&
           binding->current_frame != NULL && binding->decode_error_count != NULL;
}

static mjpeg_playback_clock_state_t load_state(
    const mjpeg_playback_clock_player_binding_t *binding)
{
    const mjpeg_playback_clock_state_t state = {
        .paused = false,
        .frame_interval_us = *binding->frame_interval_us,
        .normal_frame_interval_us = *binding->normal_frame_interval_us,
        .deadline_us = *binding->deadline_us,
        .warmup_until_us = *binding->warmup_until_us,
        .soft_start_until_us = *binding->soft_start_until_us,
        .last_frame_time_us = *binding->last_frame_time_us,
        .last_decode_success_us = *binding->last_decode_success_us,
        .last_stall_diag_us = *binding->last_stall_diag_us,
        .current_frame = *binding->current_frame,
        .decode_error_count = *binding->decode_error_count,
    };
    return state;
}

static void store_state(
    const mjpeg_playback_clock_player_binding_t *binding,
    const mjpeg_playback_clock_state_t *state)
{
    *binding->frame_interval_us = state->frame_interval_us;
    *binding->normal_frame_interval_us = state->normal_frame_interval_us;
    *binding->deadline_us = state->deadline_us;
    *binding->warmup_until_us = state->warmup_until_us;
    *binding->soft_start_until_us = state->soft_start_until_us;
    *binding->last_frame_time_us = state->last_frame_time_us;
    *binding->last_decode_success_us = state->last_decode_success_us;
    *binding->last_stall_diag_us = state->last_stall_diag_us;
    *binding->current_frame = state->current_frame;
    *binding->decode_error_count = state->decode_error_count;
}

mjpeg_playback_clock_poll_t mjpeg_playback_clock_player_poll(
    const mjpeg_playback_clock_player_binding_t *binding,
    uint64_t now_us,
    bool use_stage)
{
    mjpeg_playback_clock_poll_t poll = {.action = MJPEG_CLOCK_ACTION_PAUSED};
    if (!binding_valid(binding)) {
        return poll;
    }
    mjpeg_playback_clock_state_t state = load_state(binding);
    poll = use_stage ? mjpeg_playback_clock_poll(&state, now_us)
                     : mjpeg_playback_clock_legacy_poll(&state, now_us);
    store_state(binding, &state);
    return poll;
}

mjpeg_playback_clock_effect_t mjpeg_playback_clock_player_on_decode_result(
    const mjpeg_playback_clock_player_binding_t *binding,
    mjpeg_playback_decode_result_t result,
    uint64_t now_us,
    uint32_t yield_interval,
    bool use_stage)
{
    mjpeg_playback_clock_effect_t effect = {0};
    if (!binding_valid(binding)) {
        return effect;
    }
    mjpeg_playback_clock_state_t state = load_state(binding);
    effect = use_stage
                 ? mjpeg_playback_clock_on_decode_result(
                       &state, result, now_us, yield_interval)
                 : mjpeg_playback_clock_legacy_on_decode_result(
                       &state, result, now_us, yield_interval);
    store_state(binding, &state);
    return effect;
}

mjpeg_playback_clock_poll_t mjpeg_playback_clock_player_poll_direct(
    const mjpeg_playback_clock_player_binding_t *binding,
    uint64_t now_us)
{
    mjpeg_playback_clock_poll_t poll = {.action = MJPEG_CLOCK_ACTION_PAUSED};
    if (!binding_valid(binding)) {
        return poll;
    }
    if (*binding->warmup_until_us > now_us) {
        poll.action = MJPEG_CLOCK_ACTION_WARMUP;
        poll.wait_us = 5000U;
        return poll;
    }
    if (*binding->soft_start_until_us > now_us) {
        if (*binding->frame_interval_us < MJPEG_CLOCK_LOOP_SOFT_MIN_INTERVAL_US) {
            *binding->frame_interval_us = MJPEG_CLOCK_LOOP_SOFT_MIN_INTERVAL_US;
        }
    } else if (*binding->frame_interval_us != *binding->normal_frame_interval_us) {
        *binding->frame_interval_us = *binding->normal_frame_interval_us;
        poll.soft_start_finished = true;
    }
    if (now_us - *binding->last_decode_success_us > MJPEG_CLOCK_HANG_THRESHOLD_US) {
        poll.stall_detected = true;
        poll.yield_now = true;
        if (now_us - *binding->last_stall_diag_us >
            MJPEG_CLOCK_STALL_DIAG_INTERVAL_US) {
            *binding->last_stall_diag_us = now_us;
            poll.stall_diag = true;
        }
        *binding->deadline_us = now_us + *binding->frame_interval_us;
        poll.action = MJPEG_CLOCK_ACTION_WAIT;
        poll.wait_us = *binding->frame_interval_us;
        return poll;
    }
    if (now_us >= *binding->deadline_us) {
        poll.action = MJPEG_CLOCK_ACTION_DECODE;
        return poll;
    }
    poll.action = MJPEG_CLOCK_ACTION_WAIT;
    poll.wait_us = *binding->deadline_us - now_us;
    return poll;
}

mjpeg_playback_clock_effect_t mjpeg_playback_clock_player_on_decode_result_direct(
    const mjpeg_playback_clock_player_binding_t *binding,
    mjpeg_playback_decode_result_t result,
    uint64_t now_us,
    uint32_t yield_interval)
{
    mjpeg_playback_clock_effect_t effect = {0};
    if (!binding_valid(binding)) {
        return effect;
    }
    if (result == MJPEG_CLOCK_DECODE_OK) {
        *binding->last_frame_time_us = now_us;
        *binding->last_decode_success_us = now_us;
        ++*binding->current_frame;
        *binding->decode_error_count = 0U;
        *binding->deadline_us = now_us + *binding->frame_interval_us;
        effect.delay_one_tick =
            yield_interval != 0U && *binding->current_frame % yield_interval == 0U;
    } else if (result == MJPEG_CLOCK_DECODE_EOF) {
        *binding->deadline_us = now_us;
        *binding->last_decode_success_us = now_us;
        effect.stream_ended = true;
    } else {
        ++*binding->decode_error_count;
        effect.enter_error_state = *binding->decode_error_count > 10U;
    }
    return effect;
}
