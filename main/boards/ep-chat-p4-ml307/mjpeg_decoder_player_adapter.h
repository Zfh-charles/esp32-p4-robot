#ifndef MJPEG_DECODER_PLAYER_ADAPTER_H
#define MJPEG_DECODER_PLAYER_ADAPTER_H

#include <stddef.h>
#include <stdint.h>

#include "emotion_video_player.h"
#include "esp_err.h"
#include "esp_video_dec.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t **input_buffer;
    uint32_t *input_buffer_size;
    uint8_t **output_buffer;
    uint32_t *output_buffer_size;
    esp_video_dec_handle_t *decoder_handle;
    esp_video_dec_cfg_t *decoder_config;
    esp_video_codec_frame_info_t *frame_info;
    size_t input_alignment;
    size_t output_alignment;
    uint32_t canvas_width;
    uint32_t canvas_height;
    uint32_t output_format;
    uint32_t pts;
    emotion_video_handle_t public_handle;
    emotion_video_frame_cb_t frame_cb;
    void *frame_user_data;
} mjpeg_decoder_player_adapter_t;

esp_err_t mjpeg_decoder_player_adapter_run(
    mjpeg_decoder_player_adapter_t *adapter,
    const uint8_t *frame_data,
    size_t frame_size);

#ifdef __cplusplus
}
#endif

#endif
