#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Hypothesis-driven WDT localization after MJPEG resume.
 * Serial filter: CONTEND_
 *
 * Hypotheses (look for CONTEND_HYP):
 *   H1  Died inside LVGL flush (last_bc=flush_start, no flush_done before rst)
 *   H2  unlock_ms wall-clock includes LVGL flush preempt
 *       (flush_n increases across SafeLVGLUnlock; unlock_ms large)
 *   H3  unlock expensive but flush_n unchanged => cost is elsewhere / flush unhooked
 *   H4  First post-resume frame alone is enough to trip (frame_n=1 then rst)
 *
 * Events:
 *   CONTEND_ARM / CONTEND_HB / CONTEND_TMR_HB / CONTEND_STALL / CONTEND_STARVE
 *   CONTEND_CB / CONTEND_UNLOCK / CONTEND_FLUSH / CONTEND_RENDER / CONTEND_BC / CONTEND_HYP
 */

void WdtContendArmMjpegResume(void);
/** s1ao: arm same CONTEND window for speaking-face ROI path (flush/pin logs). */
void WdtContendArmFaceSpeak(void);
bool WdtContendWindowActive(void);
void WdtContendNoteAfeFetch(uint32_t fetch_ms);
void WdtContendNoteMjpegFrame(uint32_t memcpy_ms, uint32_t hw_ms, uint32_t cb_ms);
void WdtContendNoteLvglBusy(uint32_t wait_ms, const char* holder_name);
void WdtContendNoteLvglHold(uint32_t hold_ms, uint32_t bytes, const char* task_name);
void WdtContendMainTick(void);
void WdtContendLogStall(const char* where, uint32_t cost_ms);

void WdtContendNoteCbPhases(uint32_t wait_ms,
                            uint32_t getbuf_ms,
                            uint32_t canvas_memcpy_ms,
                            uint32_t invalidate_ms,
                            uint32_t unlock_ms,
                            uint32_t bytes);

/**
 * Call around SafeLVGLUnlock in frame_cb:
 *   flush_n_before = WdtContendFlushCount();
 *   ... unlock ...
 *   WdtContendNoteUnlock(unlock_ms, flush_n_before);
 */
uint32_t WdtContendFlushCount(void);
void WdtContendNoteUnlock(uint32_t unlock_wall_ms, uint32_t flush_n_before);

void WdtContendBreadcrumb(const char* phase);

void WdtContendNoteFlush(bool is_start, uint32_t area_px);
void WdtContendNoteRender(bool is_start, uint32_t area_px);

#ifdef __cplusplus
}
#endif
