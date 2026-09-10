#ifndef MJPEG_PRESENT_SINK_H
#define MJPEG_PRESENT_SINK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*mjpeg_present_frame_callback_t)(
    void *player_handle,
    const uint8_t *data,
    size_t decoded_size,
    uint32_t width,
    uint32_t height,
    void *user_data);

typedef struct {
    void *player_handle;
    const uint8_t *data;
    size_t decoded_size;
    uint32_t width;
    uint32_t height;
    uint32_t memcpy_ms;
    uint32_t decode_ms;
    mjpeg_present_frame_callback_t callback;
    void *callback_user_data;
} mjpeg_present_sink_frame_t;

typedef struct {
    uint64_t (*now_us)(void *context);
    bool (*window_active)(void *context);
    void (*breadcrumb)(void *context, const char *name);
    void (*note_frame)(
        void *context,
        uint32_t memcpy_ms,
        uint32_t decode_ms,
        uint32_t callback_ms);
    void (*yield_now)(void *context);
} mjpeg_present_sink_ops_t;

typedef struct {
    bool presented;
    uint32_t callback_ms;
} mjpeg_present_sink_result_t;

bool mjpeg_present_sink_present(
    const mjpeg_present_sink_frame_t *frame,
    const mjpeg_present_sink_ops_t *ops,
    void *ops_context,
    mjpeg_present_sink_result_t *result);

#ifdef __cplusplus
}
#endif

#endif
