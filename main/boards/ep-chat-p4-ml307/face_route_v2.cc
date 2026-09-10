#include "face_route_v2.h"

#include <atomic>

#include <esp_attr.h>
#include <esp_log.h>
#include <esp_rom_sys.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

#include "afe_fetch_gate.h"

#define TAG "FaceRouteV2"

namespace {

// Boot-only diagnostics live in DRAM so adding an R0 flag cannot split the
// fixed P4 TCM segment at the pre-IROM 64 KiB boundary.
DRAM_ATTR const char kFaceRouteBootFormat[] =
    "!!FACE_S1GT a=%d b=%d c=%d d=%d e=%d f=%d g=%d i=%d j=%d k=%d l=%d "
    "m=%d n=%d o=%d p=%d q=%d r=%d s=%d t=%d u=%d v=%d w=%d x=%d y=%d "
    "flash_band=4x48 drop=1 abort_base=1\n";

std::atomic<bool> g_a_worker{S1CN_A_WORKER != 0};
std::atomic<bool> g_b_emotion3{S1CN_B_EMOTION3 != 0};
std::atomic<bool> g_c_fs_gate{S1CN_C_FS_GATE != 0};
std::atomic<bool> g_d_quiet{S1CN_D_QUIET_LOG != 0};
std::atomic<bool> g_e_static_dialogue{S1CO_E_STATIC_DIALOGUE != 0};
std::atomic<bool> g_f_full_still{S1CP_F_FULL_STILL != 0};
std::atomic<bool> g_g_enter_arc{S1CP_G_ENTER_ARC != 0};
std::atomic<bool> g_i_life_layer{S1EF_I_LIFE_LAYER != 0};
std::atomic<bool> g_j_canonical_emotion{S1EP_J_CANONICAL_EMOTION != 0};
std::atomic<bool> g_k_standby_life{S1EP_K_STANDBY_LIFE != 0};
std::atomic<bool> g_l_release_hub{S1ET_L_RELEASE_HUB != 0};
std::atomic<bool> g_m_provisional_mouth{S1EW_M_PROVISIONAL_MOUTH != 0};
std::atomic<bool> g_n_mouth_gain_soft{S1EX_N_MOUTH_GAIN_SOFT != 0};
std::atomic<bool> g_o_mouth_large_gain_soft{S1EY_O_MOUTH_LARGE_GAIN_SOFT != 0};
std::atomic<bool> g_p_life_afe_fence{S1EZ_P_LIFE_AFE_FENCE != 0};
std::atomic<bool> g_t_idle_backlight_breathe{S1FD_T_IDLE_BACKLIGHT_BREATHE != 0};
std::atomic<bool> g_u_visual_budget_shadow{S1FL_U_VISUAL_BUDGET_SHADOW != 0};
std::atomic<bool> g_v_visual_budget_mouth_apply{S1FM_V_VISUAL_BUDGET_MOUTH_APPLY != 0};
std::atomic<bool> g_w_visual_budget_life_apply{S1FN_W_VISUAL_BUDGET_LIFE_APPLY != 0};
std::atomic<bool> g_x_idle_life_token_canary{S1FU_X_IDLE_LIFE_TOKEN_CANARY != 0};
std::atomic<bool> g_y_idle_flash_overlay{S1GT_Y_IDLE_FLASH_OVERLAY != 0};

std::atomic<bool> g_fs_busy{false};
std::atomic<bool> g_fs_holds_afe{false};

FaceRouteV2TickFn g_tick_fn = nullptr;
void* g_tick_ctx = nullptr;
QueueHandle_t g_tick_q = nullptr;
TaskHandle_t g_worker = nullptr;
std::atomic<uint32_t> g_frame_log_n{0};

void FaceWorkerTask(void* /*arg*/) {
    for (;;) {
        uint32_t token = 0;
        if (xQueueReceive(g_tick_q, &token, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        // Drain to latest (overwrite semantics).
        while (xQueueReceive(g_tick_q, &token, 0) == pdTRUE) {
        }
        FaceRouteV2TickFn fn = g_tick_fn;
        void* ctx = g_tick_ctx;
        if (fn != nullptr && ctx != nullptr) {
            fn(ctx);
        }
    }
}

}  // namespace

bool FaceRouteV2_WorkerEnabled(void) {
    return g_a_worker.load(std::memory_order_relaxed);
}
bool FaceRouteV2_Emotion3Enabled(void) {
    return g_b_emotion3.load(std::memory_order_relaxed);
}
bool FaceRouteV2_FsGateEnabled(void) {
    return g_c_fs_gate.load(std::memory_order_relaxed);
}
bool FaceRouteV2_QuietLogEnabled(void) {
    return g_d_quiet.load(std::memory_order_relaxed);
}
bool FaceRouteV2_StaticDialogueEnabled(void) {
    return g_e_static_dialogue.load(std::memory_order_relaxed);
}
bool FaceRouteV2_FullStillEnabled(void) {
    return g_f_full_still.load(std::memory_order_relaxed);
}
bool FaceRouteV2_EnterArcEnabled(void) {
    return g_g_enter_arc.load(std::memory_order_relaxed);
}
bool FaceRouteV2_LifeLayerEnabled(void) {
    return g_i_life_layer.load(std::memory_order_relaxed);
}
bool FaceRouteV2_CanonicalEmotionEnabled(void) {
    return g_j_canonical_emotion.load(std::memory_order_relaxed);
}
bool FaceRouteV2_StandbyLifeEnabled(void) {
    return g_k_standby_life.load(std::memory_order_relaxed);
}
bool FaceRouteV2_ReleaseHubEnabled(void) {
    return g_l_release_hub.load(std::memory_order_relaxed);
}
bool FaceRouteV2_ProvisionalMouthEnabled(void) {
    return g_m_provisional_mouth.load(std::memory_order_relaxed);
}
bool FaceRouteV2_MouthGainSoftEnabled(void) {
    return g_n_mouth_gain_soft.load(std::memory_order_relaxed);
}
bool FaceRouteV2_MouthLargeGainSoftEnabled(void) {
    return g_o_mouth_large_gain_soft.load(std::memory_order_relaxed);
}
bool FaceRouteV2_LifeAfeFenceEnabled(void) {
    return g_p_life_afe_fence.load(std::memory_order_relaxed);
}
bool FaceRouteV2_IdleBacklightBreatheEnabled(void) {
    return g_t_idle_backlight_breathe.load(std::memory_order_relaxed);
}
bool FaceRouteV2_VisualBudgetShadowEnabled(void) {
    return g_u_visual_budget_shadow.load(std::memory_order_relaxed);
}
bool FaceRouteV2_VisualBudgetMouthApplyEnabled(void) {
    return g_v_visual_budget_mouth_apply.load(std::memory_order_relaxed);
}
bool FaceRouteV2_VisualBudgetLifeApplyEnabled(void) {
    return g_w_visual_budget_life_apply.load(std::memory_order_relaxed);
}
bool FaceRouteV2_IdleLifeTokenCanaryEnabled(void) {
    return g_x_idle_life_token_canary.load(std::memory_order_relaxed);
}
bool FaceRouteV2_IdleFlashOverlayEnabled(void) {
    return g_y_idle_flash_overlay.load(std::memory_order_relaxed);
}
void FaceRouteV2_SetWorker(bool on) {
    g_a_worker.store(on, std::memory_order_relaxed);
}
void FaceRouteV2_SetEmotion3(bool on) {
    g_b_emotion3.store(on, std::memory_order_relaxed);
}
void FaceRouteV2_SetFsGate(bool on) {
    g_c_fs_gate.store(on, std::memory_order_relaxed);
}
void FaceRouteV2_SetQuietLog(bool on) {
    g_d_quiet.store(on, std::memory_order_relaxed);
}
void FaceRouteV2_SetStaticDialogue(bool on) {
    g_e_static_dialogue.store(on, std::memory_order_relaxed);
}
void FaceRouteV2_SetFullStill(bool on) {
    g_f_full_still.store(on, std::memory_order_relaxed);
}
void FaceRouteV2_SetEnterArc(bool on) {
    g_g_enter_arc.store(on, std::memory_order_relaxed);
}
void FaceRouteV2_SetLifeLayer(bool on) {
    g_i_life_layer.store(on, std::memory_order_relaxed);
}
void FaceRouteV2_SetCanonicalEmotion(bool on) {
    g_j_canonical_emotion.store(on, std::memory_order_relaxed);
}
void FaceRouteV2_SetStandbyLife(bool on) {
    g_k_standby_life.store(on, std::memory_order_relaxed);
}
void FaceRouteV2_SetReleaseHub(bool on) {
    g_l_release_hub.store(on, std::memory_order_relaxed);
}
void FaceRouteV2_SetProvisionalMouth(bool on) {
    g_m_provisional_mouth.store(on, std::memory_order_relaxed);
}
void FaceRouteV2_SetMouthGainSoft(bool on) {
    g_n_mouth_gain_soft.store(on, std::memory_order_relaxed);
}
void FaceRouteV2_SetMouthLargeGainSoft(bool on) {
    g_o_mouth_large_gain_soft.store(on, std::memory_order_relaxed);
}
void FaceRouteV2_SetLifeAfeFence(bool on) {
    g_p_life_afe_fence.store(on, std::memory_order_relaxed);
}
void FaceRouteV2_SetIdleBacklightBreathe(bool on) {
    g_t_idle_backlight_breathe.store(on, std::memory_order_relaxed);
}
void FaceRouteV2_SetVisualBudgetShadow(bool on) {
    g_u_visual_budget_shadow.store(on, std::memory_order_relaxed);
}
void FaceRouteV2_SetVisualBudgetMouthApply(bool on) {
    g_v_visual_budget_mouth_apply.store(on, std::memory_order_relaxed);
}
void FaceRouteV2_SetVisualBudgetLifeApply(bool on) {
    g_w_visual_budget_life_apply.store(on, std::memory_order_relaxed);
}
void FaceRouteV2_SetIdleLifeTokenCanary(bool on) {
    g_x_idle_life_token_canary.store(on, std::memory_order_relaxed);
}
void FaceRouteV2_SetIdleFlashOverlay(bool on) {
    g_y_idle_flash_overlay.store(on, std::memory_order_relaxed);
}
void FaceRouteV2_BootLog(void) {
    esp_rom_printf(
        kFaceRouteBootFormat,
        FaceRouteV2_WorkerEnabled() ? 1 : 0,
        FaceRouteV2_Emotion3Enabled() ? 1 : 0,
        FaceRouteV2_FsGateEnabled() ? 1 : 0,
        FaceRouteV2_QuietLogEnabled() ? 1 : 0,
        FaceRouteV2_StaticDialogueEnabled() ? 1 : 0,
        FaceRouteV2_FullStillEnabled() ? 1 : 0,
        FaceRouteV2_EnterArcEnabled() ? 1 : 0,
        FaceRouteV2_LifeLayerEnabled() ? 1 : 0,
        FaceRouteV2_CanonicalEmotionEnabled() ? 1 : 0,
        FaceRouteV2_StandbyLifeEnabled() ? 1 : 0,
        FaceRouteV2_ReleaseHubEnabled() ? 1 : 0,
        FaceRouteV2_ProvisionalMouthEnabled() ? 1 : 0,
        FaceRouteV2_MouthGainSoftEnabled() ? 1 : 0,
        FaceRouteV2_MouthLargeGainSoftEnabled() ? 1 : 0,
        FaceRouteV2_LifeAfeFenceEnabled() ? 1 : 0,
        0, 0, 0,
        FaceRouteV2_IdleBacklightBreatheEnabled() ? 1 : 0,
        FaceRouteV2_VisualBudgetShadowEnabled() ? 1 : 0,
        FaceRouteV2_VisualBudgetMouthApplyEnabled() ? 1 : 0,
        FaceRouteV2_VisualBudgetLifeApplyEnabled() ? 1 : 0,
        FaceRouteV2_IdleLifeTokenCanaryEnabled() ? 1 : 0,
        FaceRouteV2_IdleFlashOverlayEnabled() ? 1 : 0);
}

void FaceRouteV2_EnsureWorker(FaceRouteV2TickFn tick_fn, void* ctx) {
    g_tick_fn = tick_fn;
    g_tick_ctx = ctx;
    if (g_tick_q == nullptr) {
        g_tick_q = xQueueCreate(1, sizeof(uint32_t));
    }
    if (g_worker != nullptr || g_tick_q == nullptr || tick_fn == nullptr) {
        return;
    }
    // s1ez: audio_detection is prio 3.  Visual work must be lower priority so a
    // waiting AFE fetch wins the cross-core gate at the next release.
    const UBaseType_t worker_priority = FaceRouteV2_LifeAfeFenceEnabled() ? 2 : 3;
    BaseType_t ok =
        xTaskCreatePinnedToCore(FaceWorkerTask, "face_worker", 12288, nullptr, worker_priority,
                                &g_worker, 1);
    if (ok != pdPASS) {
        g_worker = nullptr;
        ESP_LOGE(TAG, "s1cn-a face_worker create FAIL");
        esp_rom_printf("!!FACE_S1CN a=create_fail\n");
        return;
    }
    ESP_LOGW(TAG, "s1ez face_worker up stack=12288 core=1 prio=%u audio_prio=3",
             (unsigned)worker_priority);
    esp_rom_printf("!!FACE_S1CN a=worker_up\n");
}

void FaceRouteV2_PostTick(void) {
    if (g_tick_q == nullptr) {
        return;
    }
    uint32_t token = 1;
    // Overwrite: if full, drop old then send.
    if (xQueueSend(g_tick_q, &token, 0) != pdTRUE) {
        uint32_t dumped = 0;
        (void)xQueueReceive(g_tick_q, &dumped, 0);
        (void)xQueueSend(g_tick_q, &token, 0);
    }
}

bool FaceRouteV2_TryBeginFullscreen(const char* why, bool afe_already_held) {
    if (!FaceRouteV2_FsGateEnabled()) {
        return true;
    }
    bool expected = false;
    if (!g_fs_busy.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        ESP_LOGW(TAG, "s1cn-c skip_busy why=%s", why ? why : "-");
        esp_rom_printf("!!FACE_S1CN c=skip_busy\n");
        return false;
    }
    if (!afe_already_held) {
        if (!AfeFetchGateTryLock(60)) {
            g_fs_busy.store(false, std::memory_order_release);
            ESP_LOGW(TAG, "s1cn-c skip_afe why=%s", why ? why : "-");
            esp_rom_printf("!!FACE_S1CN c=skip_afe\n");
            return false;
        }
        g_fs_holds_afe.store(true, std::memory_order_relaxed);
    } else {
        g_fs_holds_afe.store(false, std::memory_order_relaxed);
    }
    ESP_LOGW(TAG, "s1cn-c begin why=%s afe_held=%d", why ? why : "-", afe_already_held ? 1 : 0);
    return true;
}

void FaceRouteV2_EndFullscreen(void) {
    if (!g_fs_busy.load(std::memory_order_acquire)) {
        return;
    }
    if (g_fs_holds_afe.exchange(false, std::memory_order_acq_rel)) {
        AfeFetchGateUnlock();
    }
    g_fs_busy.store(false, std::memory_order_release);
}

bool FaceRouteV2_FullscreenBusy(void) {
    return g_fs_busy.load(std::memory_order_acquire);
}

bool FaceRouteV2_ShouldLogFrame(void) {
    if (!FaceRouteV2_QuietLogEnabled()) {
        return true;
    }
    // ~1/8 of per-frame rows/cost/breathe ticks.
    const uint32_t n = g_frame_log_n.fetch_add(1, std::memory_order_relaxed) + 1;
    return (n & 7u) == 1u;
}
