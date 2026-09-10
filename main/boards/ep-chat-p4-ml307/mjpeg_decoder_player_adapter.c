#include "mjpeg_decoder_player_adapter.h"

#include "mjpeg_decoder_stage.h"
#include "mjpeg_decoder_stage_esp.h"
#include "wdt_contention_diag.h"

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static bool adapter_is_valid(const mjpeg_decoder_player_adapter_t *adapter)
{
    return adapter != NULL && adapter->input_buffer != NULL &&
           adapter->input_buffer_size != NULL &&
           adapter->output_buffer != NULL &&
           adapter->output_buffer_size != NULL &&
           adapter->decoder_handle != NULL &&
           adapter->decoder_config != NULL &&
           adapter->frame_info != NULL;
}

static esp_err_t map_status(mjpeg_decoder_status_t status)
{
    if (status == MJPEG_DECODER_OK) {
        return ESP_OK;
    }
    if (status == MJPEG_DECODER_INVALID_SIZE) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (status == MJPEG_DECODER_NO_MEM) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_FAIL;
}

esp_err_t mjpeg_decoder_player_adapter_run(
    mjpeg_decoder_player_adapter_t *adapter,
    const uint8_t *frame_data,
    size_t frame_size)
{
    if (!adapter_is_valid(adapter)) {
        return ESP_ERR_INVALID_ARG;
    }

    mjpeg_decoder_owner_state_t owner = {
        .input_buffer = *adapter->input_buffer,
        .input_buffer_size = *adapter->input_buffer_size,
        .output_buffer = *adapter->output_buffer,
        .output_buffer_size = *adapter->output_buffer_size,
        .decoder_handle = *adapter->decoder_handle,
        .frame_width = adapter->frame_info->res.width,
        .frame_height = adapter->frame_info->res.height,
        .input_alignment = adapter->input_alignment,
        .output_alignment = adapter->output_alignment,
        .canvas_width = adapter->canvas_width,
        .canvas_height = adapter->canvas_height,
        .output_format = adapter->output_format,
    };
    const mjpeg_decoder_request_t request = {
        .frame_data = frame_data,
        .frame_size = frame_size,
        .pts = adapter->pts,
    };
    mjpeg_decoder_output_t output = {0};
    mjpeg_decoder_stage_esp_context_t esp_context = {
        .decoder_config = adapter->decoder_config,
    };
    const mjpeg_decoder_status_t status = mjpeg_decoder_stage_run(
        &owner, request, mjpeg_decoder_stage_esp_ops(), &esp_context, &output);

    *adapter->input_buffer = owner.input_buffer;
    *adapter->input_buffer_size = (uint32_t)owner.input_buffer_size;
    *adapter->output_buffer = owner.output_buffer;
    *adapter->output_buffer_size = (uint32_t)owner.output_buffer_size;
    *adapter->decoder_handle = (esp_video_dec_handle_t)owner.decoder_handle;
    adapter->frame_info->res.width = owner.frame_width;
    adapter->frame_info->res.height = owner.frame_height;

    const esp_err_t mapped_status = map_status(status);
    if (mapped_status != ESP_OK) {
        return mapped_status;
    }

    if (output.decoded_size > 0 && adapter->frame_cb != NULL) {
        const uint64_t callback_start_us = esp_timer_get_time();
        if (WdtContendWindowActive()) {
            WdtContendBreadcrumb("pre_frame_cb");
        }
        adapter->frame_cb(
            adapter->public_handle,
            output.data,
            output.decoded_size,
            output.width,
            output.height,
            adapter->frame_user_data);
        if (WdtContendWindowActive()) {
            WdtContendBreadcrumb("post_frame_cb");
        }
        const uint32_t callback_ms =
            (uint32_t)((esp_timer_get_time() - callback_start_us) / 1000ULL);
        WdtContendNoteMjpegFrame(
            output.memcpy_ms, output.decode_ms, callback_ms);
        if (WdtContendWindowActive()) {
            WdtContendBreadcrumb("post_frame_note");
            taskYIELD();
            WdtContendBreadcrumb("post_yield");
        }
    } else {
        WdtContendNoteMjpegFrame(output.memcpy_ms, output.decode_ms, 0);
    }
    return ESP_OK;
}
