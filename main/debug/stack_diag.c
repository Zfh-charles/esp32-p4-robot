#include "stack_diag.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define TAG "StackDiag"

static volatile bool s_preload_active = false;
static volatile uint32_t s_afe_hwm_words = 0;

void StackDiagSetPreloadActive(bool active) {
    s_preload_active = active;
}

bool StackDiagPreloadActive(void) {
    return s_preload_active;
}

void StackDiagNoteAfeHwm(uint32_t hwm_words) {
    s_afe_hwm_words = hwm_words;
}

void StackDiagLog(const char* stage, const char* detail) {
    const unsigned afe_hwm = (unsigned)s_afe_hwm_words;
    const unsigned self_hwm = (unsigned)uxTaskGetStackHighWaterMark(NULL);
    const unsigned free_int = (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    const unsigned largest_int =
        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    const unsigned free_psram = (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    const char* self = pcTaskGetName(NULL);

    ESP_LOGW(TAG,
             "STACK_DIAG stage=%s detail=%s | self=%s self_hwm=%u | "
             "audio_detection_hwm=%u | free_int=%u largest_int=%u free_psram=%u "
             "preload_active=%d t_ms=%u",
             stage ? stage : "?",
             detail ? detail : "-",
             self ? self : "?",
             self_hwm,
             afe_hwm,
             free_int,
             largest_int,
             free_psram,
             s_preload_active ? 1 : 0,
             (unsigned)(esp_timer_get_time() / 1000ULL));

    if (afe_hwm > 0 && afe_hwm < 256) {
        ESP_LOGE(TAG,
                 "STACK_DIAG WARN audio_detection_hwm=%u words (~%u bytes) — near overflow",
                 afe_hwm,
                 afe_hwm * 4);
    }
}
