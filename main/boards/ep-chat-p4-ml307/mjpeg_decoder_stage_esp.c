#include "mjpeg_decoder_stage_esp.h"

#include <limits.h>

#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_video_codec_utils.h"

static size_t get_free_psram(void *context)
{
    (void)context;
    return heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
}

static uint64_t now_us(void *context)
{
    (void)context;
    return (uint64_t)esp_timer_get_time();
}

static uint8_t *alloc_input(void *context, size_t alignment, size_t size)
{
    (void)context;
    return heap_caps_aligned_alloc(alignment, size, MALLOC_CAP_SPIRAM);
}

static void free_input(void *context, uint8_t *buffer)
{
    (void)context;
    heap_caps_free(buffer);
}

static uint8_t *alloc_output(
    void *context, size_t alignment, size_t needed_size, size_t *actual_size)
{
    (void)context;
    if (actual_size == NULL || alignment > UINT8_MAX ||
        needed_size > UINT32_MAX) {
        return NULL;
    }
    uint32_t actual = 0;
    uint8_t *buffer = esp_video_codec_align_alloc(
        (uint8_t)alignment, (uint32_t)needed_size, &actual);
    *actual_size = actual;
    return buffer;
}

static void free_output(void *context, uint8_t *buffer)
{
    (void)context;
    esp_video_codec_free(buffer);
}

static bool open_decoder(void *context, void **handle)
{
    mjpeg_decoder_stage_esp_context_t *esp_context =
        (mjpeg_decoder_stage_esp_context_t *)context;
    if (esp_context == NULL || esp_context->decoder_config == NULL ||
        handle == NULL) {
        return false;
    }
    esp_video_dec_handle_t decoder = NULL;
    if (esp_video_dec_open(esp_context->decoder_config, &decoder) != ESP_VC_ERR_OK) {
        return false;
    }
    *handle = decoder;
    return true;
}

static mjpeg_decoder_backend_result_t process_frame(
    void *context,
    void *handle,
    const uint8_t *input,
    size_t input_size,
    uint64_t pts,
    uint8_t *output,
    size_t output_size,
    size_t *decoded_size)
{
    (void)context;
    if (handle == NULL || input == NULL || output == NULL ||
        decoded_size == NULL || input_size > UINT32_MAX ||
        output_size > UINT32_MAX || pts > UINT32_MAX) {
        return MJPEG_DECODER_BACKEND_FAIL;
    }

    esp_video_dec_in_frame_t in_frame = {
        .pts = (uint32_t)pts,
        .dts = (uint32_t)pts,
        .data = (uint8_t *)input,
        .size = (uint32_t)input_size,
        .consumed = 0,
    };
    esp_video_dec_out_frame_t out_frame = {
        .data = output,
        .size = (uint32_t)output_size,
        .decoded_size = 0,
    };
    const esp_vc_err_t result =
        esp_video_dec_process((esp_video_dec_handle_t)handle, &in_frame, &out_frame);
    *decoded_size = out_frame.decoded_size;
    if (result == ESP_VC_ERR_OK) {
        return MJPEG_DECODER_BACKEND_OK;
    }
    if (result == ESP_VC_ERR_BUF_NOT_ENOUGH) {
        return MJPEG_DECODER_BACKEND_BUFFER_NOT_ENOUGH;
    }
    return MJPEG_DECODER_BACKEND_FAIL;
}

static bool get_frame_info(
    void *context, void *handle, mjpeg_decoder_frame_info_t *info)
{
    (void)context;
    if (handle == NULL || info == NULL) {
        return false;
    }
    esp_video_codec_frame_info_t frame_info = {0};
    if (esp_video_dec_get_frame_info(
            (esp_video_dec_handle_t)handle, &frame_info) != ESP_VC_ERR_OK) {
        return false;
    }
    info->width = frame_info.res.width;
    info->height = frame_info.res.height;
    return true;
}

static size_t get_image_size(
    void *context, uint32_t output_format, const mjpeg_decoder_frame_info_t *info)
{
    (void)context;
    if (info == NULL) {
        return 0;
    }
    esp_video_codec_resolution_t resolution = {
        .width = info->width,
        .height = info->height,
    };
    return esp_video_codec_get_image_size(
        (esp_video_codec_pixel_fmt_t)output_format, &resolution);
}

DRAM_ATTR static const mjpeg_decoder_backend_ops_t k_decoder_ops = {
    .get_free_psram = get_free_psram,
    .now_us = now_us,
    .alloc_input = alloc_input,
    .free_input = free_input,
    .alloc_output = alloc_output,
    .free_output = free_output,
    .open_decoder = open_decoder,
    .process = process_frame,
    .get_frame_info = get_frame_info,
    .get_image_size = get_image_size,
};

const mjpeg_decoder_backend_ops_t *mjpeg_decoder_stage_esp_ops(void)
{
    return &k_decoder_ops;
}
