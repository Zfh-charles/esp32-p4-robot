#pragma once

/*
 * Immutable dialogue-time face bank.
 *
 * Preload is allowed only in the existing wake=0 P2 window.  Once published,
 * dialogue-time selection is O(1): no SD I/O, JSON parse, allocation or free.
 * The legacy FaceMouth bind path stays compiled as the R0 fallback.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef S1GL_Y_SPEECH_CORE_BANK
#define S1GL_Y_SPEECH_CORE_BANK 0
#endif

#if S1GL_Y_SPEECH_CORE_BANK

bool FaceSpeechCore_Enabled(void);
void FaceSpeechCore_SetEnabled(bool on);

/** All-or-nothing preload of six canonical bases and four mouth levels. */
bool FaceSpeechCore_Preload(void);
bool FaceSpeechCore_Ready(const char* emotion_name);

/** O(1) active-bank switch.  Returns false without changing the old selection. */
bool FaceSpeechCore_Select(const char* emotion_name);
void FaceSpeechCore_Deactivate(void);
bool FaceSpeechCore_Active(void);
const char* FaceSpeechCore_ActiveEmotion(void);

const uint8_t* FaceSpeechCore_Base(uint16_t* out_w, uint16_t* out_h);
const uint8_t* FaceSpeechCore_Patch(uint8_t level, uint16_t* out_w, uint16_t* out_h);
void FaceSpeechCore_Roi(int* x, int* y, int* w, int* h);
bool FaceSpeechCore_MouthPrecomposited(void);
size_t FaceSpeechCore_TotalBytes(void);

#else

static inline bool FaceSpeechCore_Enabled(void) { return false; }
static inline void FaceSpeechCore_SetEnabled(bool on) { (void)on; }
static inline bool FaceSpeechCore_Preload(void) { return false; }
static inline bool FaceSpeechCore_Ready(const char* emotion_name) {
    (void)emotion_name;
    return false;
}
static inline bool FaceSpeechCore_Select(const char* emotion_name) {
    (void)emotion_name;
    return false;
}
static inline void FaceSpeechCore_Deactivate(void) {}
static inline bool FaceSpeechCore_Active(void) { return false; }
static inline const char* FaceSpeechCore_ActiveEmotion(void) { return NULL; }
static inline const uint8_t* FaceSpeechCore_Base(uint16_t* out_w, uint16_t* out_h) {
    if (out_w) *out_w = 0;
    if (out_h) *out_h = 0;
    return NULL;
}
static inline const uint8_t* FaceSpeechCore_Patch(uint8_t level,
                                                  uint16_t* out_w,
                                                  uint16_t* out_h) {
    (void)level;
    return FaceSpeechCore_Base(out_w, out_h);
}
static inline void FaceSpeechCore_Roi(int* x, int* y, int* w, int* h) {
    if (x) *x = 0;
    if (y) *y = 0;
    if (w) *w = 0;
    if (h) *h = 0;
}
static inline bool FaceSpeechCore_MouthPrecomposited(void) { return false; }
static inline size_t FaceSpeechCore_TotalBytes(void) { return 0; }

#endif

#ifdef __cplusplus
}
#endif
