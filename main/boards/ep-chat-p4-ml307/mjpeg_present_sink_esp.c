#include "mjpeg_present_sink_esp.h"

#include "mjpeg_runtime_selection.h"

#include "wdt_contention_diag.h"

#include "esp_attr.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static uint64_t now_us(void *context)
{
    (void)context;
    return (uint64_t)esp_timer_get_time();
}

static bool window_active(void *context)
{
    (void)context;
    return WdtContendWindowActive();
}

static void breadcrumb(void *context, const char *name)
{
    (void)context;
    WdtContendBreadcrumb(name);
}

static void note_frame(
    void *context,
    uint32_t memcpy_ms,
    uint32_t decode_ms,
    uint32_t callback_ms)
{
    (void)context;
    WdtContendNoteMjpegFrame(memcpy_ms, decode_ms, callback_ms);
}

static void yield_now(void *context)
{
    (void)context;
    taskYIELD();
}

DRAM_ATTR static const mjpeg_present_sink_ops_t k_sink_ops = {
    .now_us = now_us,
    .window_active = window_active,
    .breadcrumb = breadcrumb,
    .note_frame = note_frame,
    .yield_now = yield_now,
};

const mjpeg_present_sink_ops_t *mjpeg_present_sink_esp_ops(void)
{
    return &k_sink_ops;
}

#if EMOTION_VIDEO_USE_PRESENT_SINK
typedef struct {
    emotion_video_frame_cb_t callback;
    void *user_data;
} present_callback_context_t;

static void present_frame(
    void *player_handle,
    const uint8_t *data,
    size_t decoded_size,
    uint32_t width,
    uint32_t height,
    void *user_data)
{
    present_callback_context_t *context =
        (present_callback_context_t *)user_data;
    context->callback(
        (emotion_video_handle_t)player_handle,
        (uint8_t *)data,
        (uint32_t)decoded_size,
        width,
        height,
        context->user_data);
}
#endif

bool mjpeg_present_sink_esp_present(
    emotion_video_handle_t player_handle,
    uint8_t *data,
    uint32_t decoded_size,
    uint32_t width,
    uint32_t height,
    uint32_t memcpy_ms,
    uint64_t decode_start_us,
    emotion_video_frame_cb_t callback,
    void *callback_user_data)
{
#if EMOTION_VIDEO_USE_PRESENT_SINK
    present_callback_context_t callback_context = {
        .callback = callback,
        .user_data = callback_user_data,
    };
    const mjpeg_present_sink_frame_t frame = {
        .player_handle = player_handle,
        .data = data,
        .decoded_size = decoded_size,
        .width = width,
        .height = height,
        .memcpy_ms = memcpy_ms,
        .decode_ms =
            (uint32_t)((esp_timer_get_time() - decode_start_us) / 1000ULL),
        .callback = callback != NULL ? present_frame : NULL,
        .callback_user_data = &callback_context,
    };
    mjpeg_present_sink_result_t result = {0};
    return mjpeg_present_sink_present(&frame, &k_sink_ops, NULL, &result);
#else
    if (decoded_size > 0U && callback != NULL) {
        const uint64_t callback_start_us = esp_timer_get_time();
        if (WdtContendWindowActive()) {
            WdtContendBreadcrumb("pre_frame_cb");
        }
        callback(player_handle, data, decoded_size, width, height, callback_user_data);
        if (WdtContendWindowActive()) {
            WdtContendBreadcrumb("post_frame_cb");
        }
        const uint32_t callback_ms = (uint32_t)(
            (esp_timer_get_time() - callback_start_us) / 1000ULL);
        const uint32_t decode_ms = (uint32_t)(
            (callback_start_us - decode_start_us) / 1000ULL);
        WdtContendNoteMjpegFrame(memcpy_ms, decode_ms, callback_ms);
        if (WdtContendWindowActive()) {
            WdtContendBreadcrumb("post_frame_note");
            taskYIELD();
            WdtContendBreadcrumb("post_yield");
        }
    } else {
        const uint32_t decode_ms = (uint32_t)(
            (esp_timer_get_time() - decode_start_us) / 1000ULL);
        WdtContendNoteMjpegFrame(memcpy_ms, decode_ms, 0U);
    }
    return true;
#endif
}
