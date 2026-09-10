#ifndef MJPEG_DECODER_DIRECT_HOST_STUBS_H
#define MJPEG_DECODER_DIRECT_HOST_STUBS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_NO_MEM -2
#define ESP_ERR_INVALID_ARG -3
#define ESP_ERR_INVALID_SIZE -4

#define MALLOC_CAP_SPIRAM 0x01U

typedef int esp_vc_err_t;
#define ESP_VC_ERR_OK 0
#define ESP_VC_ERR_BUF_NOT_ENOUGH 1
#define ESP_VC_ERR_FAIL 2

typedef void *esp_video_dec_handle_t;
typedef uint32_t esp_video_codec_pixel_fmt_t;

typedef struct {
    uint32_t width;
    uint32_t height;
} esp_video_codec_resolution_t;

typedef struct {
    esp_video_codec_resolution_t res;
} esp_video_codec_frame_info_t;

typedef struct {
    uint32_t token;
} esp_video_dec_cfg_t;

typedef struct {
    uint32_t pts;
    uint32_t dts;
    uint8_t *data;
    uint32_t size;
    uint32_t consumed;
} esp_video_dec_in_frame_t;

typedef struct {
    uint8_t *data;
    uint32_t size;
    uint32_t decoded_size;
} esp_video_dec_out_frame_t;

size_t heap_caps_get_free_size(uint32_t caps);
void *heap_caps_aligned_alloc(size_t alignment, size_t size, uint32_t caps);
void heap_caps_free(void *buffer);
int64_t esp_timer_get_time(void);
uint8_t *esp_video_codec_align_alloc(
    uint8_t alignment, uint32_t size, uint32_t *actual_size);
void esp_video_codec_free(void *buffer);
uint32_t esp_video_codec_get_image_size(
    esp_video_codec_pixel_fmt_t format,
    const esp_video_codec_resolution_t *resolution);
esp_vc_err_t esp_video_dec_open(
    const esp_video_dec_cfg_t *config, esp_video_dec_handle_t *handle);
esp_vc_err_t esp_video_dec_process(
    esp_video_dec_handle_t handle,
    esp_video_dec_in_frame_t *input,
    esp_video_dec_out_frame_t *output);
esp_vc_err_t esp_video_dec_get_frame_info(
    esp_video_dec_handle_t handle, esp_video_codec_frame_info_t *info);

#ifdef __cplusplus
}
#endif

#endif
