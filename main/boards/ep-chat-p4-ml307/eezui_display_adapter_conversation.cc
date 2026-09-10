#include "eezui_display_adapter.h"

#include "application.h"
#include "emotion_video_player.h"
#include "face_route_v2.h"
#include "face_speech_core_bank.h"
#include "screen_presenter.h"
#include "lvgl.h"

#include <algorithm>
#include <atomic>
#include <string.h>
#include <strings.h>
#include <esp_log.h>
#include <esp_rom_sys.h>
#include <esp_timer.h>

#define TAG "EezuiDisplayAdapter"

namespace {
constexpr int kLvglLockQuickMs = 50;
}

void EezuiDisplayAdapter::NotifyTtsStart() {
    speech_core_tts_started_.store(false, std::memory_order_release);
}

void EezuiDisplayAdapter::NotifyTtsAudioFirst() {
    speech_core_tts_started_.store(true, std::memory_order_release);
}

void EezuiDisplayAdapter::EnterConversationPresent() {
    if (bypass_lvgl_stopped_ || panel_owner_ == PanelOwner::kEmotion) {
        ESP_LOGW(TAG, "CTRL PRESENT enter recover LVGL");
        ReleasePanelToLvgl(false);
    }
    StopIdleBreathe("present_enter");

    const auto st = Application::GetInstance().GetDeviceState();
    const uint32_t new_speech_generation =
        presenter_ != nullptr ? presenter_->NotifySpeechState(st == kDeviceStateSpeaking) : 0;
    if (st == kDeviceStateSpeaking && new_speech_generation != 0 &&
        new_speech_generation != speech_core_generation_) {
        speech_core_generation_ = new_speech_generation;
        speech_core_full_committed_ = false;
    }
    if (st == kDeviceStateListening && speech_core_pending_generation_ != 0 &&
        speech_core_pending_generation_ == speech_core_generation_) {
        pending_face_emo_.clear();
        speech_core_pending_generation_ = 0;
    }
    if (st == kDeviceStateSpeaking) {
        strong_emotion_hold_pending_ = false;
        strong_emotion_hold_until_us_ = 0;
    }

    bool release_holding = emotion_release_active_;
    if (st == kDeviceStateListening || st == kDeviceStateConnecting) {
        if (st == kDeviceStateListening &&
            strong_emotion_hold_until_us_ > esp_timer_get_time() &&
            (strcasecmp(current_emotion_name_.c_str(), "angry") == 0 ||
             strcasecmp(current_emotion_name_.c_str(), "sad") == 0)) {
            StopMouthFollow("strong_hold");
            strong_emotion_hold_pending_ = EnsureFaceAnimTimer();
            if (strong_emotion_hold_pending_) {
                const int64_t remain_us =
                    std::max<int64_t>(1000, strong_emotion_hold_until_us_ - esp_timer_get_time());
                esp_timer_stop(face_anim_timer_);
                esp_timer_start_once(face_anim_timer_, remain_us);
                release_holding = true;
                ESP_LOGW(TAG, "s1fg strong_hold_arm emo=%s remain_ms=%lld",
                         current_emotion_name_.c_str(), (long long)(remain_us / 1000));
            }
        } else if (st == kDeviceStateListening) {
            release_holding = ArmEmotionRelease("present_listen");
        }
        if (!release_holding) {
            PresentStandbyBookend("present_listen");
            StopFacePanelAnim("present_listen");
        }
    } else if (st == kDeviceStateSpeaking && !pending_face_emo_.empty()) {
        // Pending emotions are one of the six canonical names. Snapshot the
        // value without allocating so this extracted no-throw adapter stays
        // link-neutral with the former in-class path.
        char emo[16] = {};
        strlcpy(emo, pending_face_emo_.c_str(), sizeof(emo));
        pending_face_emo_.clear();
        speech_core_pending_generation_ = 0;
        if (presenter_ != nullptr) {
            ESP_LOGW(TAG, "CTRL PRESENT commit emo=%s via=presenter why=flush_pending s1bj",
                     emo);
            presenter_->NotifyEmotion(emo);
        } else {
            ESP_LOGW(TAG, "CTRL PRESENT commit emo=%s via=leaf why=flush_pending_no_presenter s1bj",
                     emo);
            CommitFaceStillOnce(emo);
        }
    } else if (st == kDeviceStateSpeaking &&
               !(face_anim_roi_only_ || face_anim_sustain_ || face_anim_left_ > 0 ||
                 face_anim_need_prime_ || face_anim_await_settle_)) {
        if (new_speech_generation != 0 && FaceRouteV2_ProvisionalMouthEnabled()) {
            const char* active_core = FaceSpeechCore_ActiveEmotion();
            const bool canonical_standby_present =
                active_core && strcasecmp(active_core, "standby") == 0 &&
                strcasecmp(current_emotion_name_.c_str(), "standby") == 0;
            if (ArmMouthFollow("standby", canonical_standby_present)) {
                ESP_LOGW(TAG, "s1fg provisional_mouth arm canonical=1 gen=%u",
                         (unsigned)new_speech_generation);
                esp_rom_printf("!!FACE_S1FG provisional_mouth=arm canonical=1 gen=%u\n",
                               (unsigned)new_speech_generation);
            } else {
                ESP_LOGW(TAG, "s1fg provisional_mouth fallback=standby gen=%u",
                         (unsigned)new_speech_generation);
                PresentStandbyBookend("present_speak_fallback");
            }
        } else {
            PresentStandbyBookend("present_speak_seed");
        }
    }

    if (SafeLVGLLock(kLvglLockQuickMs)) {
        if (main_image_ != nullptr) lv_obj_add_flag(main_image_, LV_OBJ_FLAG_HIDDEN);
        EnsureDialogueCaptionStyle();
        CancelIdleHideTimer();
        SafeLVGLUnlock();
    }
    ESP_LOGW(TAG, "CTRL PRESENT enter state=%d pending=%d anim_left=%d", (int)st,
             pending_face_emo_.empty() ? 0 : 1, face_anim_left_);
    if (presenter_ != nullptr) presenter_->EnterConversation();
}

void EezuiDisplayAdapter::LeaveConversationPresent() {
    strong_emotion_hold_pending_ = false;
    strong_emotion_hold_until_us_ = 0;
    StopIdleBreathe("present_leave");
    const bool core_was_active = FaceSpeechCore_Active();
    if (core_was_active) {
        StopMouthFollow("speech_core_leave");
        FaceSpeechCore_Deactivate();
    }
    const bool release_holding =
        !core_was_active && (emotion_release_active_ || ArmEmotionRelease("present_leave"));
    if (!release_holding &&
        (face_anim_roi_only_ || face_anim_sustain_ || face_anim_left_ > 0 ||
         (last_bypass_rgb_ != nullptr &&
          strcasecmp(current_emotion_name_.c_str(), "standby") != 0))) {
        PresentStandbyBookend("present_leave");
    }
    if (!release_holding) StopFacePanelAnim("present_leave");
    pending_face_emo_.clear();
    speech_core_pending_generation_ = 0;
    speech_core_full_committed_ = false;
    if (presenter_ != nullptr) presenter_->LeaveConversation();
    if (!release_holding && Application::GetInstance().GetDeviceState() == kDeviceStateIdle &&
        emotion_player_ != nullptr &&
        emotion_video_player_seed_stills_ready(emotion_player_)) {
        PresentStandbyBookend("idle_standby");
    }
}
