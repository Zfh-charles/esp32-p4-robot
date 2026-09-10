#include "idle_wdt_diag.h"

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_rom_sys.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <string.h>

#define TAG "IdleWdt"

/* Observe ~20s after enter_idle (covers 5s wake-stable + margin). */
static const int64_t kWindowUs = 20 * 1000000LL;
static const uint32_t kAfeSlowMs = 80;
static const uint32_t kAfeHardMs = 200;

static int64_t s_arm_us = 0;
static char s_why[24];
static uint32_t s_afe_slow_n = 0;
static uint32_t s_afe_hard_n = 0;
static uint32_t s_last_afe_ms = 0;
static uint32_t s_max_afe_ms = 0;
static uint32_t s_hb_n = 0;

bool IdleWdtWindowActive(void) {
    if (s_arm_us == 0) {
        return false;
    }
    return (esp_timer_get_time() - s_arm_us) < kWindowUs;
}

void IdleWdtArm(const char* why) {
    s_arm_us = esp_timer_get_time();
    s_afe_slow_n = 0;
    s_afe_hard_n = 0;
    s_last_afe_ms = 0;
    s_max_afe_ms = 0;
    s_hb_n = 0;
    if (why && why[0]) {
        strncpy(s_why, why, sizeof(s_why) - 1);
        s_why[sizeof(s_why) - 1] = '\0';
    } else {
        strncpy(s_why, "-", sizeof(s_why) - 1);
    }
    ESP_LOGW(TAG, "IDLE_WDT arm why=%s window_s=20", s_why);
    esp_rom_printf("!!IDLE_WDT arm\n");
}

void IdleWdtMark(const char* phase, const char* detail) {
    if (s_arm_us == 0) {
        return;
    }
    const int64_t now = esp_timer_get_time();
    const int elapsed_ms = (int)((now - s_arm_us) / 1000);
    const size_t free_int = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    const size_t free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    ESP_LOGW(TAG,
             "IDLE_WDT mark phase=%s detail=%s elapsed_ms=%d task=%s core=%d | "
             "afe_last_ms=%u afe_max_ms=%u afe_slow_n=%u afe_hard_n=%u | "
             "free_int=%u free_psram=%u",
             phase ? phase : "-", detail ? detail : "-", elapsed_ms,
             pcTaskGetName(NULL), (int)xPortGetCoreID(), (unsigned)s_last_afe_ms,
             (unsigned)s_max_afe_ms, (unsigned)s_afe_slow_n, (unsigned)s_afe_hard_n,
             (unsigned)free_int, (unsigned)free_psram);
    esp_rom_printf("!!IDLE_WDT %s\n", phase ? phase : "-");
}

void IdleWdtNoteAfeFetch(uint32_t fetch_ms) {
    if (!IdleWdtWindowActive()) {
        return;
    }
    s_last_afe_ms = fetch_ms;
    if (fetch_ms > s_max_afe_ms) {
        s_max_afe_ms = fetch_ms;
    }
    if (fetch_ms < kAfeSlowMs) {
        return;
    }
    s_afe_slow_n++;
    if (fetch_ms >= kAfeHardMs) {
        s_afe_hard_n++;
    }
    if (fetch_ms >= kAfeHardMs || (s_afe_slow_n % 3) == 1) {
        ESP_LOGW(TAG,
                 "IDLE_WDT afe_slow fetch_ms=%u slow_n=%u hard_n=%u task=%s core=%d",
                 (unsigned)fetch_ms, (unsigned)s_afe_slow_n, (unsigned)s_afe_hard_n,
                 pcTaskGetName(NULL), (int)xPortGetCoreID());
        esp_rom_printf("!!IDLE_WDT afe %u\n", (unsigned)fetch_ms);
    }
}

void IdleWdtMainHb(void) {
    if (!IdleWdtWindowActive()) {
        return;
    }
    s_hb_n++;
    const int elapsed_s = (int)((esp_timer_get_time() - s_arm_us) / 1000000LL);
    const size_t free_int = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    ESP_LOGW(TAG,
             "IDLE_WDT hb n=%u elapsed_s=%d afe_last_ms=%u afe_max_ms=%u "
             "afe_slow_n=%u afe_hard_n=%u free_int=%u task=%s",
             (unsigned)s_hb_n, elapsed_s, (unsigned)s_last_afe_ms, (unsigned)s_max_afe_ms,
             (unsigned)s_afe_slow_n, (unsigned)s_afe_hard_n, (unsigned)free_int,
             pcTaskGetName(NULL));
    esp_rom_printf("!!IDLE_WDT hb %u\n", (unsigned)s_hb_n);
}
