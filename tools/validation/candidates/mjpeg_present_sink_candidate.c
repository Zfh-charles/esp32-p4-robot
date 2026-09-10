#include "mjpeg_present_sink_candidate.h"

#include <string.h>

static bool ops_are_valid(const mjpeg_present_sink_ops_t *ops)
{
    return ops != NULL && ops->now_us != NULL && ops->window_active != NULL &&
           ops->breadcrumb != NULL && ops->note_frame != NULL &&
           ops->yield_now != NULL;
}

bool mjpeg_present_sink_candidate_present(
    const mjpeg_present_sink_frame_t *frame,
    const mjpeg_present_sink_ops_t *ops,
    void *ops_context,
    mjpeg_present_sink_result_t *result)
{
    if (frame == NULL || result == NULL || !ops_are_valid(ops) ||
        (frame->decoded_size > 0U && frame->data == NULL)) {
        return false;
    }
    memset(result, 0, sizeof(*result));

    if (frame->decoded_size == 0U || frame->callback == NULL) {
        ops->note_frame(ops_context, frame->memcpy_ms, frame->decode_ms, 0U);
        return true;
    }

    const uint64_t callback_start_us = ops->now_us(ops_context);
    if (ops->window_active(ops_context)) {
        ops->breadcrumb(ops_context, "pre_frame_cb");
    }
    frame->callback(
        frame->player_handle,
        frame->data,
        frame->decoded_size,
        frame->width,
        frame->height,
        frame->callback_user_data);
    if (ops->window_active(ops_context)) {
        ops->breadcrumb(ops_context, "post_frame_cb");
    }
    const uint64_t callback_end_us = ops->now_us(ops_context);
    const uint32_t callback_ms = (uint32_t)(
        callback_end_us >= callback_start_us
            ? (callback_end_us - callback_start_us) / 1000U
            : 0U);
    ops->note_frame(
        ops_context, frame->memcpy_ms, frame->decode_ms, callback_ms);
    if (ops->window_active(ops_context)) {
        ops->breadcrumb(ops_context, "post_frame_note");
        ops->yield_now(ops_context);
        ops->breadcrumb(ops_context, "post_yield");
    }

    result->presented = true;
    result->callback_ms = callback_ms;
    return true;
}
