#pragma once

/**
 * s1cr layered mouth — independent small RGB565 patches from SD dialogue_v2 pack.
 * Audio only publishes mouth_level; LVGL present happens on face_worker / face timer.
 * No pack / flag off → Ready()=false, dialogue stays StaticHold+enter (no-op).
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef S1CR_H_MOUTH
#define S1CR_H_MOUTH 1
#endif

bool FaceMouth_Enabled(void);
void FaceMouth_SetEnabled(bool on);

/** Boot probe: look for /sdcard/dialogue_v2/pack_manifest.json */
void FaceMouth_BootProbe(void);

/** Serialize patch-bank bind/free with face-worker reads. Recursive by design. */
bool FaceMouth_Lock(uint32_t timeout_ms);
void FaceMouth_Unlock(void);

/** Bind emotion folder; loads mouth level rgb565 patches + ROI from manifest.json */
bool FaceMouth_BindEmotion(const char* emotion_name);
void FaceMouth_Clear(void);

bool FaceMouth_Ready(void);

/** AudioOutputTask only: RMS → level 0..3 (latest-value). No LVGL. */
void FaceMouth_PublishFromPcm(const int16_t* pcm, size_t samples);

uint8_t FaceMouth_Level(void);
uint8_t FaceMouth_ConsumeLevelIfChanged(uint8_t* out_level);

const uint8_t* FaceMouth_PatchRgb565(uint8_t level, uint16_t* out_w, uint16_t* out_h);
/** Canonical full-frame base that the precomposited mouth patches were built against. */
const uint8_t* FaceMouth_HoldBaseRgb565(uint16_t* out_w, uint16_t* out_h);
uint8_t FaceMouth_PoseCount(void);
const uint8_t* FaceMouth_PoseBaseRgb565(uint8_t pose, uint16_t* out_w, uint16_t* out_h);
const uint8_t* FaceMouth_PosePatchRgb565(uint8_t pose, uint8_t level,
                                        uint16_t* out_w, uint16_t* out_h);
const uint8_t* FaceMouth_EyePatchRgb565(uint8_t pose, uint8_t level,
                                       uint16_t* out_w, uint16_t* out_h);
void FaceMouth_EyeRoi(int* x, int* y, int* w, int* h);
void FaceMouth_Roi(int* x, int* y, int* w, int* h);
bool FaceMouth_MouthPrecomposited(void);
bool FaceMouth_LifeReady(void);
bool FaceMouth_LifePrecomposited(void);
uint8_t FaceMouth_LifeTrackCount(void);
uint8_t FaceMouth_LifeFrameCount(uint8_t track);
bool FaceMouth_LifeOverlapsMouth(uint8_t track);
bool FaceMouth_LifeFrame(uint8_t track, uint8_t index, const uint8_t** rgb565, const uint8_t** mask_a8,
                         uint16_t* w, uint16_t* h, int* x, int* y);
/** s1eu: standby-common-hub strong-emotion entry, one <=48-row band per tick. */
bool FaceMouth_EnterReady(void);
uint8_t FaceMouth_EnterFrameCount(void);
uint16_t FaceMouth_EnterIntervalMs(void);
bool FaceMouth_EnterFrame(uint8_t index, const uint8_t** rgb565,
                          uint16_t* w, uint16_t* h, int* x, int* y);
/** s1et: precomposited strong-emotion release, flattened to one <=48-row band per tick. */
bool FaceMouth_ReleaseReady(void);
uint8_t FaceMouth_ReleaseFrameCount(void);
uint16_t FaceMouth_ReleaseIntervalMs(void);
bool FaceMouth_ReleaseFrame(uint8_t index, const uint8_t** rgb565,
                            uint16_t* w, uint16_t* h, int* x, int* y);

#ifdef __cplusplus
}
#endif
