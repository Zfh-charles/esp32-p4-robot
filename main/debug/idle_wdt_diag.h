#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Idle/wake-stable HP_WDT localization (diag only — no policy change).
 *
 * Hypotheses (look for IDLE_WDT + next boot prev_phase):
 *   H1  Die inside EnterIdleStandby / RestoreIdleReady
 *       → last Mark before rst; prev_phase=IDLE_* mid-enter
 *   H2  Die inside wake-stable MJPEG_SKIP / PrintHeap / ClearCrashStreak
 *       → Mark skip_begin without skip_end; prev_phase=IDLE_SKIP_*
 *   H3  AFE fetch chronically slow after idle; WDT elsewhere under AFE load
 *       → dense IDLE_WDT afe_slow; hb continues then sudden rst
 *   H4  Main CLOCK_TICK stops (main starved)
 *       → no IDLE_WDT hb after arm
 *
 * Serial filter: IDLE_WDT
 */

void IdleWdtArm(const char* why);
bool IdleWdtWindowActive(void);
void IdleWdtMark(const char* phase, const char* detail);
void IdleWdtNoteAfeFetch(uint32_t fetch_ms);
void IdleWdtMainHb(void);

#ifdef __cplusplus
}
#endif
