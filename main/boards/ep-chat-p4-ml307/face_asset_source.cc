#include "face_asset_source.h"

#include "face_mouth_layer.h"
#include "face_speech_core_bank.h"

bool FaceAsset_Ready(void) {
    return FaceSpeechCore_Active() || FaceMouth_Ready();
}
uint8_t FaceAsset_PoseCount(void) {
    return FaceSpeechCore_Active() ? 1 : FaceMouth_PoseCount();
}
const uint8_t* FaceAsset_PoseBase(uint8_t pose, uint16_t* w, uint16_t* h) {
    if (FaceSpeechCore_Active()) return pose == 0 ? FaceSpeechCore_Base(w, h) : nullptr;
    return FaceMouth_PoseBaseRgb565(pose, w, h);
}
const uint8_t* FaceAsset_PosePatch(uint8_t pose, uint8_t level,
                                   uint16_t* w, uint16_t* h) {
    if (FaceSpeechCore_Active()) return pose == 0 ? FaceSpeechCore_Patch(level, w, h) : nullptr;
    return FaceMouth_PosePatchRgb565(pose, level, w, h);
}
const uint8_t* FaceAsset_EyePatch(uint8_t pose, uint8_t level,
                                  uint16_t* w, uint16_t* h) {
    if (FaceSpeechCore_Active()) return nullptr;
    return FaceMouth_EyePatchRgb565(pose, level, w, h);
}
void FaceAsset_EyeRoi(int* x, int* y, int* w, int* h) {
    if (!FaceSpeechCore_Active()) return FaceMouth_EyeRoi(x, y, w, h);
    if (x) *x = 0;
    if (y) *y = 0;
    if (w) *w = 0;
    if (h) *h = 0;
}
void FaceAsset_MouthRoi(int* x, int* y, int* w, int* h) {
    if (FaceSpeechCore_Active()) return FaceSpeechCore_Roi(x, y, w, h);
    FaceMouth_Roi(x, y, w, h);
}
bool FaceAsset_MouthPrecomposited(void) {
    return FaceSpeechCore_Active() ? FaceSpeechCore_MouthPrecomposited()
                                   : FaceMouth_MouthPrecomposited();
}
bool FaceAsset_LifeReady(void) {
    return !FaceSpeechCore_Active() && FaceMouth_LifeReady();
}
bool FaceAsset_LifePrecomposited(void) {
    return FaceAsset_LifeReady() && FaceMouth_LifePrecomposited();
}
uint8_t FaceAsset_LifeTrackCount(void) {
    return FaceAsset_LifeReady() ? FaceMouth_LifeTrackCount() : 0;
}
uint8_t FaceAsset_LifeFrameCount(uint8_t track) {
    return FaceAsset_LifeReady() ? FaceMouth_LifeFrameCount(track) : 0;
}
bool FaceAsset_LifeOverlapsMouth(uint8_t track) {
    return FaceAsset_LifeReady() && FaceMouth_LifeOverlapsMouth(track);
}
bool FaceAsset_LifeFrame(uint8_t track, uint8_t index, const uint8_t** rgb565,
                         const uint8_t** mask_a8, uint16_t* w, uint16_t* h,
                         int* x, int* y) {
    return FaceAsset_LifeReady() &&
           FaceMouth_LifeFrame(track, index, rgb565, mask_a8, w, h, x, y);
}
