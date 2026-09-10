#ifndef MJPEG_PRESENT_SINK_ESP_H
#define MJPEG_PRESENT_SINK_ESP_H

#include "mjpeg_present_sink.h"
#include "emotion_video_player.h"

#ifdef __cplusplus
extern "C" {
#endif

const mjpeg_present_sink_ops_t *mjpeg_present_sink_esp_ops(void);

bool mjpeg_present_sink_esp_present(
    emotion_video_handle_t player_handle,
    uint8_t *data,
    uint32_t decoded_size,
    uint32_t width,
    uint32_t height,
    uint32_t memcpy_ms,
    uint64_t decode_start_us,
    emotion_video_frame_cb_t callback,
    void *callback_user_data);

#ifdef __cplusplus
}
#endif

#endif
