#ifndef MJPEG_DECODER_CANDIDATE_H
#define MJPEG_DECODER_CANDIDATE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    MJPEG_DECODER_OK = 0,
    MJPEG_DECODER_INVALID_ARGUMENT,
    MJPEG_DECODER_INVALID_SIZE,
    MJPEG_DECODER_NO_MEM,
    MJPEG_DECODER_FAIL,
} mjpeg_decoder_status_t;

typedef enum {
    MJPEG_DECODER_BACKEND_OK = 0,
    MJPEG_DECODER_BACKEND_BUFFER_NOT_ENOUGH,
    MJPEG_DECODER_BACKEND_FAIL,
} mjpeg_decoder_backend_result_t;

typedef struct {
    uint32_t width;
    uint32_t height;
} mjpeg_decoder_frame_info_t;

typedef struct {
    uint8_t *input_buffer;
    size_t input_buffer_size;
    uint8_t *output_buffer;
    size_t output_buffer_size;
    void *decoder_handle;
    mjpeg_decoder_frame_info_t frame_info;
    size_t input_alignment;
    size_t output_alignment;
    uint32_t canvas_width;
    uint32_t canvas_height;
    uint32_t output_format;
} mjpeg_decoder_resources_t;

typedef struct {
    const uint8_t *frame_data;
    size_t frame_size;
    uint64_t pts;
} mjpeg_decoder_request_t;

typedef struct {
    uint8_t *data;
    size_t decoded_size;
    uint32_t width;
    uint32_t height;
    uint32_t memcpy_ms;
    uint32_t decode_ms;
} mjpeg_decoder_output_t;

typedef struct {
    size_t (*get_free_psram)(void *context);
    uint64_t (*now_us)(void *context);
    uint8_t *(*alloc_input)(void *context, size_t alignment, size_t size);
    void (*free_input)(void *context, uint8_t *buffer);
    uint8_t *(*alloc_output)(
        void *context, size_t alignment, size_t needed_size, size_t *actual_size);
    void (*free_output)(void *context, uint8_t *buffer);
    bool (*open_decoder)(void *context, void **handle);
    mjpeg_decoder_backend_result_t (*process)(
        void *context,
        void *handle,
        const uint8_t *input,
        size_t input_size,
        uint64_t pts,
        uint8_t *output,
        size_t output_size,
        size_t *decoded_size);
    bool (*get_frame_info)(
        void *context, void *handle, mjpeg_decoder_frame_info_t *info);
    size_t (*get_image_size)(
        void *context, uint32_t output_format, const mjpeg_decoder_frame_info_t *info);
} mjpeg_decoder_backend_ops_t;

/*
 * Offline G4-3 candidate. It owns synchronous buffer/decoder sequencing only.
 * Frame claiming, cache locks, playback timing, presentation and callbacks stay
 * outside this boundary. The file must not be added to the board source GLOB
 * until a separate production marker is approved.
 */
mjpeg_decoder_status_t mjpeg_decoder_candidate_decode(
    mjpeg_decoder_resources_t *resources,
    mjpeg_decoder_request_t request,
    const mjpeg_decoder_backend_ops_t *ops,
    void *backend_context,
    mjpeg_decoder_output_t *output);

#ifdef __cplusplus
}
#endif

#endif
