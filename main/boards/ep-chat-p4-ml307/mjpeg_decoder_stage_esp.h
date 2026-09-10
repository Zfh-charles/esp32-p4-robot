#ifndef MJPEG_DECODER_STAGE_ESP_H
#define MJPEG_DECODER_STAGE_ESP_H

#include "mjpeg_decoder_stage.h"

#include "esp_video_dec.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    esp_video_dec_cfg_t *decoder_config;
} mjpeg_decoder_stage_esp_context_t;

const mjpeg_decoder_backend_ops_t *mjpeg_decoder_stage_esp_ops(void);

#ifdef __cplusplus
}
#endif

#endif
