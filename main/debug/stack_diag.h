#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Serial filter: STACK_DIAG */
void StackDiagLog(const char* stage, const char* detail);
void StackDiagSetPreloadActive(bool active);
bool StackDiagPreloadActive(void);
/** Called from audio_detection with its own uxTaskGetStackHighWaterMark(NULL). */
void StackDiagNoteAfeHwm(uint32_t hwm_words);

#ifdef __cplusplus
}
#endif
