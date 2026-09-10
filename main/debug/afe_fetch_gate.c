#include "afe_fetch_gate.h"

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <esp_attr.h>
#include <esp_log.h>
#include <esp_timer.h>

static SemaphoreHandle_t s_mu = NULL;
static SemaphoreHandle_t s_mspi_budget_mu = NULL;
static portMUX_TYPE s_mspi_budget_init_mux = portMUX_INITIALIZER_UNLOCKED;
static volatile uint32_t s_flush_started = 0;
static volatile uint32_t s_flush_finished = 0;
static volatile uint32_t s_flush_active = 0;
static volatile uint32_t s_frame_ready = 0;
static portMUX_TYPE s_idle_visual_mux = portMUX_INITIALIZER_UNLOCKED;
static afe_idle_visual_notify_fn_t s_idle_visual_notify = NULL;
static void* s_idle_visual_ctx = NULL;
static bool s_idle_visual_pending = false;
static int64_t s_idle_visual_next_us = 0;
static uint32_t s_idle_visual_min_interval_ms = 900;

#define AFE_TXN_RTC_MAGIC 0xAFC1D1A6u

typedef enum {
    AFE_DIAG_IDLE = 0,
    AFE_DIAG_WAIT_GATE,
    AFE_DIAG_FETCH,
    AFE_DIAG_RETURNED,
} afe_diag_stage_t;

typedef struct {
    uint32_t magic;
    uint32_t face_seq;
    uint32_t face_stage;
    uint32_t face_ms;
    int32_t face_core;
    uint32_t afe_seq;
    uint32_t afe_stage;
    uint32_t afe_ms;
    int32_t afe_core;
    uint32_t flush_started;
    uint32_t flush_finished;
    uint32_t flush_active;
    uint32_t flush_ms;
    uint32_t frame_ready;
    uint32_t frame_ready_ms;
} afe_txn_rtc_t;

RTC_NOINIT_ATTR static afe_txn_rtc_t s_rtc_txn;
static const char* TAG = "AfeTxnDiag";

static uint32_t DiagMs(void) {
    return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

static void EnsureRtc(void) {
    if (s_rtc_txn.magic == AFE_TXN_RTC_MAGIC) {
        return;
    }
    s_rtc_txn = (afe_txn_rtc_t){0};
    s_rtc_txn.magic = AFE_TXN_RTC_MAGIC;
    s_rtc_txn.face_core = -1;
    s_rtc_txn.afe_core = -1;
}

static void AfeFetchGateEnsure(void) {
    if (s_mu != NULL) {
        return;
    }
    s_mu = xSemaphoreCreateMutex();
}

void AfeFetchGateEnter(void) {
    AfeFetchGateEnsure();
    EnsureRtc();
    s_rtc_txn.afe_seq++;
    s_rtc_txn.afe_stage = AFE_DIAG_WAIT_GATE;
    s_rtc_txn.afe_ms = DiagMs();
    s_rtc_txn.afe_core = (int32_t)xPortGetCoreID();
    if (s_mu != NULL) {
        xSemaphoreTake(s_mu, portMAX_DELAY);
    }
    s_rtc_txn.afe_stage = AFE_DIAG_FETCH;
    s_rtc_txn.afe_ms = DiagMs();
}

static void MspiBudgetGateEnsure(void) {
    if (s_mspi_budget_mu != NULL) {
        return;
    }
    SemaphoreHandle_t candidate = xSemaphoreCreateMutex();
    if (candidate == NULL) {
        return;
    }
    portENTER_CRITICAL(&s_mspi_budget_init_mux);
    if (s_mspi_budget_mu == NULL) {
        s_mspi_budget_mu = candidate;
        candidate = NULL;
    }
    portEXIT_CRITICAL(&s_mspi_budget_init_mux);
    if (candidate != NULL) {
        vSemaphoreDelete(candidate);
    }
}

void AfeFetchGateSetIdleVisualNotify(afe_idle_visual_notify_fn_t notify, void* ctx) {
#if S1FC_S_AFE_EDGE_TOKEN
    portENTER_CRITICAL(&s_idle_visual_mux);
    s_idle_visual_notify = notify;
    s_idle_visual_ctx = ctx;
    if (notify == NULL) {
        s_idle_visual_pending = false;
    }
    portEXIT_CRITICAL(&s_idle_visual_mux);
#else
    (void)notify;
    (void)ctx;
#endif
}

void AfeFetchGateRequestIdleVisualEdge(uint32_t min_interval_ms) {
#if S1FC_S_AFE_EDGE_TOKEN
    if (min_interval_ms < 250) {
        min_interval_ms = 250;
    }
    portENTER_CRITICAL(&s_idle_visual_mux);
    s_idle_visual_min_interval_ms = min_interval_ms;
    s_idle_visual_pending = true;
    portEXIT_CRITICAL(&s_idle_visual_mux);
#else
    (void)min_interval_ms;
#endif
}

void AfeFetchGateCancelIdleVisualEdge(void) {
#if S1FC_S_AFE_EDGE_TOKEN
    portENTER_CRITICAL(&s_idle_visual_mux);
    s_idle_visual_pending = false;
    portEXIT_CRITICAL(&s_idle_visual_mux);
#endif
}

static void MaybeNotifyIdleVisualEdge(void) {
#if S1FC_S_AFE_EDGE_TOKEN
    afe_idle_visual_notify_fn_t notify = NULL;
    void* ctx = NULL;
    const int64_t now_us = esp_timer_get_time();
    portENTER_CRITICAL(&s_idle_visual_mux);
    if (s_idle_visual_pending && s_idle_visual_notify != NULL &&
        now_us >= s_idle_visual_next_us) {
        s_idle_visual_pending = false;
        s_idle_visual_next_us =
            now_us + (int64_t)s_idle_visual_min_interval_ms * 1000LL;
        notify = s_idle_visual_notify;
        ctx = s_idle_visual_ctx;
    }
    portEXIT_CRITICAL(&s_idle_visual_mux);
    if (notify != NULL) {
        notify(ctx);
    }
#endif
}

void AfeFetchGateLeave(void) {
    EnsureRtc();
    s_rtc_txn.afe_stage = AFE_DIAG_RETURNED;
    s_rtc_txn.afe_ms = DiagMs();
    if (s_mu != NULL) {
        xSemaphoreGive(s_mu);
    }
    s_rtc_txn.afe_stage = AFE_DIAG_IDLE;
    s_rtc_txn.afe_ms = DiagMs();
    // s1fc: callback runs only after the AFE mutex is released.  It must never
    // decode, paint, log, wait, or touch LVGL; it only posts a latest-value tick.
    MaybeNotifyIdleVisualEdge();
}

bool AfeFetchGateTryLock(uint32_t timeout_ms) {
    AfeFetchGateEnsure();
    if (s_mu == NULL) {
        return true;
    }
    return xSemaphoreTake(s_mu, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

void AfeFetchGateUnlock(void) {
    if (s_mu != NULL) {
        xSemaphoreGive(s_mu);
    }
}

bool AfeFetchGateBusy(void) {
    AfeFetchGateEnsure();
    if (s_mu == NULL) {
        return false;
    }
    if (xSemaphoreTake(s_mu, 0) == pdTRUE) {
        xSemaphoreGive(s_mu);
        return false;
    }
    return true;
}

bool MspiBudgetGateEnterNetwork(void) {
#if S1FA_Q_MQTT_IDLE_QUIET
    MspiBudgetGateEnsure();
    if (s_mspi_budget_mu != NULL) {
        return xSemaphoreTake(s_mspi_budget_mu, portMAX_DELAY) == pdTRUE;
    }
    return false;
#else
    return false;
#endif
}

void MspiBudgetGateLeaveNetwork(void) {
#if S1FA_Q_MQTT_IDLE_QUIET
    if (s_mspi_budget_mu != NULL) {
        xSemaphoreGive(s_mspi_budget_mu);
    }
#endif
}

bool MspiBudgetGateTryEnterIdleVisual(void) {
#if S1FA_Q_MQTT_IDLE_QUIET
    MspiBudgetGateEnsure();
    return s_mspi_budget_mu == NULL || xSemaphoreTake(s_mspi_budget_mu, 0) == pdTRUE;
#else
    return true;
#endif
}

void MspiBudgetGateLeaveIdleVisual(void) {
#if S1FA_Q_MQTT_IDLE_QUIET
    if (s_mspi_budget_mu != NULL) {
        xSemaphoreGive(s_mspi_budget_mu);
    }
#endif
}

void AfeFetchGatePrepareDisplayFence(afe_display_fence_t* fence) {
    if (fence == NULL) {
        return;
    }
    fence->flush_started = __atomic_load_n(&s_flush_started, __ATOMIC_ACQUIRE);
    fence->flush_finished = __atomic_load_n(&s_flush_finished, __ATOMIC_ACQUIRE);
    fence->frame_ready = __atomic_load_n(&s_frame_ready, __ATOMIC_ACQUIRE);
}

void AfeFetchGateNoteDisplayFlush(bool is_start) {
    EnsureRtc();
    if (is_start) {
        __atomic_add_fetch(&s_flush_active, 1, __ATOMIC_ACQ_REL);
        __atomic_add_fetch(&s_flush_started, 1, __ATOMIC_RELEASE);
        s_rtc_txn.flush_started++;
        s_rtc_txn.flush_active++;
        s_rtc_txn.flush_ms = DiagMs();
        return;
    }
    uint32_t active = __atomic_load_n(&s_flush_active, __ATOMIC_ACQUIRE);
    while (active > 0 &&
           !__atomic_compare_exchange_n(&s_flush_active, &active, active - 1, false,
                                        __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
    }
    __atomic_add_fetch(&s_flush_finished, 1, __ATOMIC_RELEASE);
    s_rtc_txn.flush_finished++;
    if (s_rtc_txn.flush_active > 0) {
        s_rtc_txn.flush_active--;
    }
    s_rtc_txn.flush_ms = DiagMs();
}

void AfeFetchGateNoteDisplayFrameReady(void) {
    EnsureRtc();
    __atomic_add_fetch(&s_frame_ready, 1, __ATOMIC_RELEASE);
    s_rtc_txn.frame_ready++;
    s_rtc_txn.frame_ready_ms = DiagMs();
}

bool AfeFetchGateWaitDisplayFence(const afe_display_fence_t* fence,
                                  uint32_t timeout_ms,
                                  uint32_t* waited_ms) {
    if (waited_ms != NULL) {
        *waited_ms = 0;
    }
    if (fence == NULL) {
        return false;
    }
    const TickType_t begin = xTaskGetTickCount();
    const TickType_t timeout = pdMS_TO_TICKS(timeout_ms);
    for (;;) {
        const uint32_t ready = __atomic_load_n(&s_frame_ready, __ATOMIC_ACQUIRE);
        // s1cw: FLUSH_FINISH only means esp_lcd_panel_draw_bitmap() returned.
        // A 400-row invalidate is split into ~8 strips and DMA continues after
        // that event.  Only the LCD transfer-done callback for LVGL's final
        // strip proves that the frame no longer reads PSRAM.
        if (ready != fence->frame_ready) {
            if (waited_ms != NULL) {
                *waited_ms = (uint32_t)((xTaskGetTickCount() - begin) * portTICK_PERIOD_MS);
            }
            return true;
        }
        if ((xTaskGetTickCount() - begin) >= timeout) {
            if (waited_ms != NULL) {
                *waited_ms = (uint32_t)((xTaskGetTickCount() - begin) * portTICK_PERIOD_MS);
            }
            return false;
        }
        vTaskDelay(1);
    }
}

void AfeFetchGateBeginFaceTxn(void) {
    EnsureRtc();
    s_rtc_txn.face_seq++;
    s_rtc_txn.face_stage = AFE_FACE_STAGE_BEGIN;
    s_rtc_txn.face_ms = DiagMs();
    s_rtc_txn.face_core = (int32_t)xPortGetCoreID();
}

void AfeFetchGateNoteFaceStage(afe_face_stage_t stage) {
    EnsureRtc();
    s_rtc_txn.face_stage = (uint32_t)stage;
    s_rtc_txn.face_ms = DiagMs();
    s_rtc_txn.face_core = (int32_t)xPortGetCoreID();
}

void AfeFetchGateBootReportAndReset(void) {
    EnsureRtc();
    ESP_LOGW(TAG,
             "WDT_LAST face_seq=%u face_stage=%u face_t=%u face_core=%d "
             "afe_seq=%u afe_stage=%u afe_t=%u afe_core=%d "
             "flush=%u/%u active=%u flush_t=%u ready=%u ready_t=%u s1cw",
             (unsigned)s_rtc_txn.face_seq, (unsigned)s_rtc_txn.face_stage,
             (unsigned)s_rtc_txn.face_ms, (int)s_rtc_txn.face_core,
             (unsigned)s_rtc_txn.afe_seq, (unsigned)s_rtc_txn.afe_stage,
             (unsigned)s_rtc_txn.afe_ms, (int)s_rtc_txn.afe_core,
             (unsigned)s_rtc_txn.flush_started, (unsigned)s_rtc_txn.flush_finished,
             (unsigned)s_rtc_txn.flush_active, (unsigned)s_rtc_txn.flush_ms,
             (unsigned)s_rtc_txn.frame_ready, (unsigned)s_rtc_txn.frame_ready_ms);
    s_rtc_txn = (afe_txn_rtc_t){0};
    s_rtc_txn.magic = AFE_TXN_RTC_MAGIC;
    s_rtc_txn.face_core = -1;
    s_rtc_txn.afe_core = -1;
}
