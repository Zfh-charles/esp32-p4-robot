#include "mjpeg_decoder_direct_candidate.h"

#include <limits.h>
#include <stdbool.h>
#include <string.h>

#ifndef MJPEG_DECODER_DIRECT_HOST_TEST
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_video_codec_utils.h"
#endif

#define MJPEG_DECODER_MIN_INPUT_SIZE (64U * 1024U)
#define MJPEG_DECODER_PSRAM_RESERVE (512U * 1024U)

static bool binding_is_valid(const mjpeg_decoder_direct_binding_t *binding)
{
    return binding != NULL && binding->input_buffer != NULL &&
           binding->input_buffer_size != NULL &&
           binding->output_buffer != NULL &&
           binding->output_buffer_size != NULL &&
           binding->decoder_handle != NULL &&
           binding->decoder_config != NULL && binding->frame_info != NULL;
}

esp_err_t mjpeg_decoder_player_decode_direct(
    const mjpeg_decoder_direct_binding_t *binding,
    const mjpeg_decoder_direct_config_t *config,
    const uint8_t *frame_data,
    size_t frame_size,
    uint32_t pts,
    mjpeg_decoder_direct_output_t *output)
{
    if (!binding_is_valid(binding) || config == NULL || frame_data == NULL ||
        frame_size == 0U || frame_size > UINT32_MAX || output == NULL ||
        config->output_alignment > UINT8_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(output, 0, sizeof(*output));

    if (*binding->input_buffer == NULL ||
        frame_size > *binding->input_buffer_size) {
        const size_t required_size = frame_size > MJPEG_DECODER_MIN_INPUT_SIZE
                                         ? frame_size
                                         : MJPEG_DECODER_MIN_INPUT_SIZE;
        if (required_size > SIZE_MAX - MJPEG_DECODER_PSRAM_RESERVE ||
            heap_caps_get_free_size(MALLOC_CAP_SPIRAM) <
                required_size + MJPEG_DECODER_PSRAM_RESERVE) {
            return ESP_ERR_NO_MEM;
        }
        if (*binding->input_buffer != NULL) {
            heap_caps_free(*binding->input_buffer);
        }
        const size_t alignment =
            config->input_alignment > 64U ? config->input_alignment : 64U;
        *binding->input_buffer = (uint8_t *)heap_caps_aligned_alloc(
            alignment, required_size, MALLOC_CAP_SPIRAM);
        if (*binding->input_buffer == NULL) {
            if (*binding->output_buffer != NULL) {
                esp_video_codec_free(*binding->output_buffer);
                *binding->output_buffer = NULL;
                *binding->output_buffer_size = 0U;
            }
            return ESP_ERR_NO_MEM;
        }
        *binding->input_buffer_size = (uint32_t)required_size;
    }

    if (frame_size > *binding->input_buffer_size) {
        return ESP_ERR_INVALID_SIZE;
    }
    const uint64_t memcpy_start_us = (uint64_t)esp_timer_get_time();
    memcpy(*binding->input_buffer, frame_data, frame_size);
    const uint64_t memcpy_end_us = (uint64_t)esp_timer_get_time();

    if (*binding->output_buffer == NULL) {
        const uint64_t needed =
            (uint64_t)config->canvas_width * config->canvas_height * 2U;
        if (needed > UINT32_MAX) {
            return ESP_ERR_INVALID_SIZE;
        }
        uint32_t actual_size = 0U;
        *binding->output_buffer = esp_video_codec_align_alloc(
            (uint8_t)config->output_alignment, (uint32_t)needed, &actual_size);
        if (*binding->output_buffer == NULL) {
            return ESP_ERR_NO_MEM;
        }
        *binding->output_buffer_size = actual_size;
    }

    if (*binding->decoder_handle == NULL &&
        esp_video_dec_open(binding->decoder_config, binding->decoder_handle) !=
            ESP_VC_ERR_OK) {
        return ESP_FAIL;
    }

    esp_video_dec_in_frame_t input = {
        .pts = pts,
        .dts = pts,
        .data = *binding->input_buffer,
        .size = (uint32_t)frame_size,
        .consumed = 0,
    };
    esp_video_dec_out_frame_t decoded = {
        .data = *binding->output_buffer,
        .size = *binding->output_buffer_size,
        .decoded_size = 0,
    };
    const uint64_t decode_start_us = (uint64_t)esp_timer_get_time();
    esp_vc_err_t result = esp_video_dec_process(
        *binding->decoder_handle, &input, &decoded);

    if (result == ESP_VC_ERR_BUF_NOT_ENOUGH) {
        if (esp_video_dec_get_frame_info(
                *binding->decoder_handle, binding->frame_info) != ESP_VC_ERR_OK) {
            return ESP_FAIL;
        }
        if (*binding->output_buffer != NULL) {
            esp_video_codec_free(*binding->output_buffer);
        }
        const uint32_t needed = esp_video_codec_get_image_size(
            (esp_video_codec_pixel_fmt_t)config->output_format,
            &binding->frame_info->res);
        uint32_t actual_size = 0U;
        *binding->output_buffer = esp_video_codec_align_alloc(
            (uint8_t)config->output_alignment, needed, &actual_size);
        if (*binding->output_buffer == NULL) {
            return ESP_ERR_NO_MEM;
        }
        *binding->output_buffer_size = actual_size;
        decoded.data = *binding->output_buffer;
        decoded.size = *binding->output_buffer_size;
        result = esp_video_dec_process(
            *binding->decoder_handle, &input, &decoded);
    }
    if (result != ESP_VC_ERR_OK) {
        return ESP_FAIL;
    }

    if (decoded.decoded_size > 0U &&
        (binding->frame_info->res.width == 0U ||
         binding->frame_info->res.height == 0U)) {
        (void)esp_video_dec_get_frame_info(
            *binding->decoder_handle, binding->frame_info);
    }
    const uint64_t decode_end_us = (uint64_t)esp_timer_get_time();
    output->data = *binding->output_buffer;
    output->decoded_size = decoded.decoded_size;
    output->width = binding->frame_info->res.width;
    output->height = binding->frame_info->res.height;
    output->memcpy_ms = (uint32_t)(
        memcpy_end_us >= memcpy_start_us
            ? (memcpy_end_us - memcpy_start_us) / 1000U
            : 0U);
    output->decode_ms = (uint32_t)(
        decode_end_us >= decode_start_us
            ? (decode_end_us - decode_start_us) / 1000U
            : 0U);
    return ESP_OK;
}
