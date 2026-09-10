#ifndef MJPEG_DECODER_DIRECT_CANDIDATE_H
#define MJPEG_DECODER_DIRECT_CANDIDATE_H

#include <stddef.h>
#include <stdint.h>

#ifdef MJPEG_DECODER_DIRECT_HOST_TEST
#include "mjpeg_decoder_direct_host_stubs.h"
#else
#include "esp_err.h"
#include "esp_video_dec.h"
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Pointer-only binding: no per-frame owner/resource value copy. */
typedef struct {
    uint8_t **input_buffer;
    uint32_t *input_buffer_size;
    uint8_t **output_buffer;
    uint32_t *output_buffer_size;
    esp_video_dec_handle_t *decoder_handle;
    esp_video_dec_cfg_t *decoder_config;
    esp_video_codec_frame_info_t *frame_info;
} mjpeg_decoder_direct_binding_t;

typedef struct {
    size_t input_alignment;
    size_t output_alignment;
    uint32_t canvas_width;
    uint32_t canvas_height;
    uint32_t output_format;
} mjpeg_decoder_direct_config_t;

typedef struct {
    uint8_t *data;
    uint32_t decoded_size;
    uint32_t width;
    uint32_t height;
    uint32_t memcpy_ms;
    uint32_t decode_ms;
} mjpeg_decoder_direct_output_t;

esp_err_t mjpeg_decoder_player_decode_direct(
    const mjpeg_decoder_direct_binding_t *binding,
    const mjpeg_decoder_direct_config_t *config,
    const uint8_t *frame_data,
    size_t frame_size,
    uint32_t pts,
    mjpeg_decoder_direct_output_t *output);

#ifdef __cplusplus
}
#endif

#endif
