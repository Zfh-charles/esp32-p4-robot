#include "mjpeg_decoder_stage.h"

#include <limits.h>
#include <string.h>

#define MJPEG_DECODER_MIN_INPUT_SIZE (64U * 1024U)
#define MJPEG_DECODER_PSRAM_RESERVE (512U * 1024U)

static bool ops_are_valid(const mjpeg_decoder_backend_ops_t *ops)
{
    return ops != NULL && ops->get_free_psram != NULL &&
           ops->now_us != NULL &&
           ops->alloc_input != NULL && ops->free_input != NULL &&
           ops->alloc_output != NULL && ops->free_output != NULL &&
           ops->open_decoder != NULL && ops->process != NULL &&
           ops->get_frame_info != NULL && ops->get_image_size != NULL;
}

static size_t at_least_64(size_t alignment)
{
    return alignment > 64U ? alignment : 64U;
}

mjpeg_decoder_status_t mjpeg_decoder_stage_decode(
    mjpeg_decoder_resources_t *resources,
    mjpeg_decoder_request_t request,
    const mjpeg_decoder_backend_ops_t *ops,
    void *backend_context,
    mjpeg_decoder_output_t *output)
{
    if (resources == NULL || output == NULL || !ops_are_valid(ops) ||
        request.frame_data == NULL || request.frame_size == 0U) {
        return MJPEG_DECODER_INVALID_ARGUMENT;
    }
    memset(output, 0, sizeof(*output));

    if (resources->input_buffer == NULL ||
        request.frame_size > resources->input_buffer_size) {
        const size_t required_size = request.frame_size > MJPEG_DECODER_MIN_INPUT_SIZE
                                         ? request.frame_size
                                         : MJPEG_DECODER_MIN_INPUT_SIZE;
        if (required_size > SIZE_MAX - MJPEG_DECODER_PSRAM_RESERVE ||
            ops->get_free_psram(backend_context) <
                required_size + MJPEG_DECODER_PSRAM_RESERVE) {
            return MJPEG_DECODER_NO_MEM;
        }

        if (resources->input_buffer != NULL) {
            ops->free_input(backend_context, resources->input_buffer);
        }
        resources->input_buffer = ops->alloc_input(
            backend_context, at_least_64(resources->input_alignment), required_size);
        if (resources->input_buffer == NULL) {
            if (resources->output_buffer != NULL) {
                ops->free_output(backend_context, resources->output_buffer);
                resources->output_buffer = NULL;
                resources->output_buffer_size = 0U;
            }
            return MJPEG_DECODER_NO_MEM;
        }
        resources->input_buffer_size = required_size;
    }

    if (request.frame_size > resources->input_buffer_size) {
        return MJPEG_DECODER_INVALID_SIZE;
    }
    const uint64_t memcpy_start_us = ops->now_us(backend_context);
    memcpy(resources->input_buffer, request.frame_data, request.frame_size);
    const uint64_t memcpy_end_us = ops->now_us(backend_context);

    if (resources->output_buffer == NULL) {
        const size_t needed_size =
            (size_t)resources->canvas_width * (size_t)resources->canvas_height * 2U;
        size_t actual_size = 0U;
        resources->output_buffer = ops->alloc_output(
            backend_context, resources->output_alignment, needed_size, &actual_size);
        if (resources->output_buffer == NULL) {
            return MJPEG_DECODER_NO_MEM;
        }
        resources->output_buffer_size = actual_size;
    }

    if (resources->decoder_handle == NULL &&
        !ops->open_decoder(backend_context, &resources->decoder_handle)) {
        return MJPEG_DECODER_FAIL;
    }

    size_t decoded_size = 0U;
    const uint64_t decode_start_us = ops->now_us(backend_context);
    mjpeg_decoder_backend_result_t result = ops->process(
        backend_context,
        resources->decoder_handle,
        resources->input_buffer,
        request.frame_size,
        request.pts,
        resources->output_buffer,
        resources->output_buffer_size,
        &decoded_size);

    if (result == MJPEG_DECODER_BACKEND_BUFFER_NOT_ENOUGH) {
        if (!ops->get_frame_info(
                backend_context, resources->decoder_handle, &resources->frame_info)) {
            return MJPEG_DECODER_FAIL;
        }
        if (resources->output_buffer != NULL) {
            ops->free_output(backend_context, resources->output_buffer);
        }
        const size_t needed_size = ops->get_image_size(
            backend_context, resources->output_format, &resources->frame_info);
        size_t actual_size = 0U;
        resources->output_buffer = ops->alloc_output(
            backend_context, resources->output_alignment, needed_size, &actual_size);
        if (resources->output_buffer == NULL) {
            return MJPEG_DECODER_NO_MEM;
        }
        resources->output_buffer_size = actual_size;
        result = ops->process(
            backend_context,
            resources->decoder_handle,
            resources->input_buffer,
            request.frame_size,
            request.pts,
            resources->output_buffer,
            resources->output_buffer_size,
            &decoded_size);
    }

    if (result != MJPEG_DECODER_BACKEND_OK) {
        return MJPEG_DECODER_FAIL;
    }

    if (decoded_size > 0U &&
        (resources->frame_info.width == 0U || resources->frame_info.height == 0U)) {
        (void)ops->get_frame_info(
            backend_context, resources->decoder_handle, &resources->frame_info);
    }
    output->data = resources->output_buffer;
    output->decoded_size = decoded_size;
    output->width = resources->frame_info.width;
    output->height = resources->frame_info.height;
    const uint64_t decode_end_us = ops->now_us(backend_context);
    output->memcpy_ms = (uint32_t)(
        memcpy_end_us >= memcpy_start_us
            ? (memcpy_end_us - memcpy_start_us) / 1000U
            : 0U);
    output->decode_ms = (uint32_t)(
        decode_end_us >= decode_start_us
            ? (decode_end_us - decode_start_us) / 1000U
            : 0U);
    return MJPEG_DECODER_OK;
}


mjpeg_decoder_status_t mjpeg_decoder_stage_run(
    mjpeg_decoder_owner_state_t *owner,
    mjpeg_decoder_request_t request,
    const mjpeg_decoder_backend_ops_t *ops,
    void *backend_context,
    mjpeg_decoder_output_t *output)
{
    if (owner == NULL) {
        return MJPEG_DECODER_INVALID_ARGUMENT;
    }

    mjpeg_decoder_resources_t resources = {
        .input_buffer = owner->input_buffer,
        .input_buffer_size = owner->input_buffer_size,
        .output_buffer = owner->output_buffer,
        .output_buffer_size = owner->output_buffer_size,
        .decoder_handle = owner->decoder_handle,
        .frame_info = {
            .width = owner->frame_width,
            .height = owner->frame_height,
        },
        .input_alignment = owner->input_alignment,
        .output_alignment = owner->output_alignment,
        .canvas_width = owner->canvas_width,
        .canvas_height = owner->canvas_height,
        .output_format = owner->output_format,
    };

    const mjpeg_decoder_status_t status = mjpeg_decoder_stage_decode(
        &resources, request, ops, backend_context, output);

    owner->input_buffer = resources.input_buffer;
    owner->input_buffer_size = resources.input_buffer_size;
    owner->output_buffer = resources.output_buffer;
    owner->output_buffer_size = resources.output_buffer_size;
    owner->decoder_handle = resources.decoder_handle;
    owner->frame_width = resources.frame_info.width;
    owner->frame_height = resources.frame_info.height;
    return status;
}
