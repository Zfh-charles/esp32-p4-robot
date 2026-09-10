#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * s1fa-q: serialize low-priority idle visual DMA against ML307 MQTT AT
 * transactions.  Speaking mouth updates deliberately do not use this gate.
 * R0 rollback: set to 0 and rebuild.
 */
#ifndef S1FA_Q_MQTT_IDLE_QUIET
#define S1FA_Q_MQTT_IDLE_QUIET 1
#endif

/**
 * s1fb-r: use a deterministic phase-dither sequence after an idle visual
 * misses the AFE gate.  A fixed 900 ms retry is an exact multiple of the
 * roughly 30 ms AFE fetch cadence and can starve forever at one busy phase.
 * R0 rollback restores the s1fa fixed retry without touching the MQTT gate.
 */
#ifndef S1FB_R_IDLE_AFE_DITHER
#define S1FB_R_IDLE_AFE_DITHER 1
#endif

/**
 * s1fc-s: replace blind idle-visual polling with a rate-limited token emitted
 * immediately after one AFE fetch releases the gate.  The callback must stay
 * non-blocking; it only wakes the lower-priority face worker.
 * R0 rollback: set to 0 to restore the s1fb phase-dither retry path.
 */
#ifndef S1FC_S_AFE_EDGE_TOKEN
#define S1FC_S_AFE_EDGE_TOKEN 1
#endif

typedef void (*afe_idle_visual_notify_fn_t)(void* ctx);
void AfeFetchGateSetIdleVisualNotify(afe_idle_visual_notify_fn_t notify, void* ctx);
void AfeFetchGateRequestIdleVisualEdge(uint32_t min_interval_ms);
void AfeFetchGateCancelIdleVisualEdge(void);

/**
 * s1cl/s1cm: serialize AFE fetch vs idle face present (MSPI-751 skew).
 * Enter/Leave wrap fetch_with_delay; face uses TryLock around present.
 */
void AfeFetchGateEnter(void);
void AfeFetchGateLeave(void);

/** Non-blocking / timed lock for face path. Pair with Unlock. */
bool AfeFetchGateTryLock(uint32_t timeout_ms);
void AfeFetchGateUnlock(void);

/** True if fetch currently holds the gate (timeout 0 probe). */
bool AfeFetchGateBusy(void);

/** Network owns this gate while an AT operation may block; idle visuals only
 * try-lock and drop/back off when busy. */
bool MspiBudgetGateEnterNetwork(void);
void MspiBudgetGateLeaveNetwork(void);
bool MspiBudgetGateTryEnterIdleVisual(void);
void MspiBudgetGateLeaveIdleVisual(void);

/**
 * Fence an LVGL render/flush while the caller still owns AfeFetchGate.
 * Prepare under the LVGL lock before invalidation, unlock LVGL, then wait.
 * This closes the gap where canvas invalidation returned before MIPI finished
 * reading PSRAM and AFE entered WakeNet on the other core.
 */
typedef struct {
    uint32_t flush_started;
    uint32_t flush_finished;
    uint32_t frame_ready;
} afe_display_fence_t;

void AfeFetchGatePrepareDisplayFence(afe_display_fence_t* fence);
void AfeFetchGateNoteDisplayFlush(bool is_start);
/** Called by the LCD transfer-done callback for LVGL's final strip. */
void AfeFetchGateNoteDisplayFrameReady(void);
bool AfeFetchGateWaitDisplayFence(const afe_display_fence_t* fence,
                                  uint32_t timeout_ms,
                                  uint32_t* waited_ms);

/** s1cv: allocation-free RTC breadcrumbs for bare HP-WDT resets. */
typedef enum {
    AFE_FACE_STAGE_IDLE = 0,
    AFE_FACE_STAGE_BEGIN,
    AFE_FACE_STAGE_CANVAS_WRITE,
    AFE_FACE_STAGE_INVALIDATE,
    AFE_FACE_STAGE_UNLOCK,
    AFE_FACE_STAGE_FENCE_WAIT,
    AFE_FACE_STAGE_DONE,
} afe_face_stage_t;

void AfeFetchGateBeginFaceTxn(void);
void AfeFetchGateNoteFaceStage(afe_face_stage_t stage);
void AfeFetchGateBootReportAndReset(void);

#ifdef __cplusplus
}
#endif
