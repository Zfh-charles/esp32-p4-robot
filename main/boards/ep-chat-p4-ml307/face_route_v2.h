#pragma once

/**
 * s1cn–s1cp route-v2 defense batch — orthogonal runtime flags (G2).
 *
 * | Flag | Define                 | Role |
 * |------|------------------------|------|
 * | a    | S1CN_A_WORKER          | face_worker: decode/present off main_event_loop |
 * | b    | S1CN_B_EMOTION3        | emotion requested/pending/committed |
 * | c    | S1CN_C_FS_GATE         | fullscreen singleton + AFE safety window |
 * | d    | S1CN_D_QUIET_LOG       | rate-limit per-frame FACE_* logs |
 * | e    | S1CO_E_STATIC_DIALOGUE | dialogue StaticHold — no MID 400-row band |
 * | f    | S1CP_F_FULL_STILL      | breathe/hold/bookend use seed_full (kill seam) |
 * | g    | S1CP_G_ENTER_ARC       | short full-frame enter, then hold on seed |
 * | p    | S1EZ_P_LIFE_AFE_FENCE  | idle life final-DMA fence + audio priority |
 *
 * Mouth layer (h) lives in face_mouth_layer.h as S1CR_H_MOUTH — not toggled here.
 *
 * Default ON for this marker; bisect = set one define to 0 and rebuild,
 * or FaceRouteV2_Set*(false) then soft-reboot (atomics survive in RAM until reset).
 * Known-good rollback = reflash s1cm (E8); structural-only extract = s1db.
 */

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef S1CN_A_WORKER
#define S1CN_A_WORKER 1
#endif
#ifndef S1CN_B_EMOTION3
#define S1CN_B_EMOTION3 1
#endif
#ifndef S1CN_C_FS_GATE
#define S1CN_C_FS_GATE 1
#endif
#ifndef S1CN_D_QUIET_LOG
#define S1CN_D_QUIET_LOG 1
#endif
#ifndef S1CO_E_STATIC_DIALOGUE
#define S1CO_E_STATIC_DIALOGUE 1
#endif
#ifndef S1CP_F_FULL_STILL
#define S1CP_F_FULL_STILL 1
#endif
#ifndef S1CP_G_ENTER_ARC
#define S1CP_G_ENTER_ARC 0
#endif
#ifndef S1EF_I_LIFE_LAYER
#define S1EF_I_LIFE_LAYER 1
#endif
#ifndef S1EP_J_CANONICAL_EMOTION
#define S1EP_J_CANONICAL_EMOTION 1
#endif
#ifndef S1EP_K_STANDBY_LIFE
#define S1EP_K_STANDBY_LIFE 1
#endif
#ifndef S1ET_L_RELEASE_HUB
#define S1ET_L_RELEASE_HUB 0
#endif
#ifndef S1EW_M_PROVISIONAL_MOUTH
#define S1EW_M_PROVISIONAL_MOUTH 1
#endif
#ifndef S1EX_N_MOUTH_GAIN_SOFT
#define S1EX_N_MOUTH_GAIN_SOFT 1
#endif
#ifndef S1EY_O_MOUTH_LARGE_GAIN_SOFT
#define S1EY_O_MOUTH_LARGE_GAIN_SOFT 1
#endif
#ifndef S1EZ_P_LIFE_AFE_FENCE
#define S1EZ_P_LIFE_AFE_FENCE 1
#endif
#ifndef S1FD_T_IDLE_BACKLIGHT_BREATHE
#define S1FD_T_IDLE_BACKLIGHT_BREATHE 1
#endif
#ifndef S1FL_U_VISUAL_BUDGET_SHADOW
#define S1FL_U_VISUAL_BUDGET_SHADOW 1
#endif
#ifndef S1FM_V_VISUAL_BUDGET_MOUTH_APPLY
#define S1FM_V_VISUAL_BUDGET_MOUTH_APPLY 1
#endif
#ifndef S1FN_W_VISUAL_BUDGET_LIFE_APPLY
#define S1FN_W_VISUAL_BUDGET_LIFE_APPLY 1
#endif
#ifndef S1FU_X_IDLE_LIFE_TOKEN_CANARY
#define S1FU_X_IDLE_LIFE_TOKEN_CANARY 1
#endif
#ifndef S1GT_Y_IDLE_FLASH_OVERLAY
#define S1GT_Y_IDLE_FLASH_OVERLAY 1
#endif
bool FaceRouteV2_WorkerEnabled(void);
bool FaceRouteV2_Emotion3Enabled(void);
bool FaceRouteV2_FsGateEnabled(void);
bool FaceRouteV2_QuietLogEnabled(void);
bool FaceRouteV2_StaticDialogueEnabled(void);
bool FaceRouteV2_FullStillEnabled(void);
bool FaceRouteV2_EnterArcEnabled(void);
bool FaceRouteV2_LifeLayerEnabled(void);
bool FaceRouteV2_CanonicalEmotionEnabled(void);
bool FaceRouteV2_StandbyLifeEnabled(void);
bool FaceRouteV2_ReleaseHubEnabled(void);
bool FaceRouteV2_ProvisionalMouthEnabled(void);
bool FaceRouteV2_MouthGainSoftEnabled(void);
bool FaceRouteV2_MouthLargeGainSoftEnabled(void);
bool FaceRouteV2_LifeAfeFenceEnabled(void);
bool FaceRouteV2_IdleBacklightBreatheEnabled(void);
bool FaceRouteV2_VisualBudgetShadowEnabled(void);
bool FaceRouteV2_VisualBudgetMouthApplyEnabled(void);
bool FaceRouteV2_VisualBudgetLifeApplyEnabled(void);
bool FaceRouteV2_IdleLifeTokenCanaryEnabled(void);
bool FaceRouteV2_IdleFlashOverlayEnabled(void);

void FaceRouteV2_SetWorker(bool on);
void FaceRouteV2_SetEmotion3(bool on);
void FaceRouteV2_SetFsGate(bool on);
void FaceRouteV2_SetQuietLog(bool on);
void FaceRouteV2_SetStaticDialogue(bool on);
void FaceRouteV2_SetFullStill(bool on);
void FaceRouteV2_SetEnterArc(bool on);
void FaceRouteV2_SetLifeLayer(bool on);
void FaceRouteV2_SetCanonicalEmotion(bool on);
void FaceRouteV2_SetStandbyLife(bool on);
void FaceRouteV2_SetReleaseHub(bool on);
void FaceRouteV2_SetProvisionalMouth(bool on);
void FaceRouteV2_SetMouthGainSoft(bool on);
void FaceRouteV2_SetIdleBacklightBreathe(bool on);
void FaceRouteV2_SetMouthLargeGainSoft(bool on);
void FaceRouteV2_SetLifeAfeFence(bool on);
void FaceRouteV2_SetVisualBudgetShadow(bool on);
void FaceRouteV2_SetVisualBudgetMouthApply(bool on);
void FaceRouteV2_SetVisualBudgetLifeApply(bool on);
void FaceRouteV2_SetIdleLifeTokenCanary(bool on);
void FaceRouteV2_SetIdleFlashOverlay(bool on);

/** Boot banner: FW sub-markers s1cn-a..d on/off. */
void FaceRouteV2_BootLog(void);

/**
 * s1cn-a: queue-depth-1 face worker. tick_fn(ctx) runs on Core1 stack>=12288.
 * Safe to call repeatedly; first call creates the task.
 */
typedef void (*FaceRouteV2TickFn)(void* ctx);
void FaceRouteV2_EnsureWorker(FaceRouteV2TickFn tick_fn, void* ctx);
/** Overwrite-post a tick; drops stale if worker is busy. */
void FaceRouteV2_PostTick(void);

/**
 * s1cn-c: at most one seed_full / fullscreen present.
 * If afe_already_held=false, takes AfeFetchGate (60ms) for safety window; End releases it.
 * If afe_already_held=true (e.g. idle breathe), only enforces singleton — no nested lock.
 * Returns false → caller must skip the fullscreen present.
 */
bool FaceRouteV2_TryBeginFullscreen(const char* why, bool afe_already_held);
void FaceRouteV2_EndFullscreen(void);
bool FaceRouteV2_FullscreenBusy(void);

/** s1cn-d: true = emit this per-frame log (rate-limited when quiet). */
bool FaceRouteV2_ShouldLogFrame(void);

#ifdef __cplusplus
}
#endif
