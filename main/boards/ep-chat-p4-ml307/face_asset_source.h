#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

bool FaceAsset_Ready(void);
uint8_t FaceAsset_PoseCount(void);
const uint8_t* FaceAsset_PoseBase(uint8_t pose, uint16_t* w, uint16_t* h);
const uint8_t* FaceAsset_PosePatch(uint8_t pose, uint8_t level, uint16_t* w, uint16_t* h);
const uint8_t* FaceAsset_EyePatch(uint8_t pose, uint8_t level, uint16_t* w, uint16_t* h);
void FaceAsset_EyeRoi(int* x, int* y, int* w, int* h);
void FaceAsset_MouthRoi(int* x, int* y, int* w, int* h);
bool FaceAsset_MouthPrecomposited(void);
bool FaceAsset_LifeReady(void);
bool FaceAsset_LifePrecomposited(void);
uint8_t FaceAsset_LifeTrackCount(void);
uint8_t FaceAsset_LifeFrameCount(uint8_t track);
bool FaceAsset_LifeOverlapsMouth(uint8_t track);
bool FaceAsset_LifeFrame(uint8_t track, uint8_t index, const uint8_t** rgb565,
                         const uint8_t** mask_a8, uint16_t* w, uint16_t* h,
                         int* x, int* y);

#ifdef __cplusplus
}
#endif
