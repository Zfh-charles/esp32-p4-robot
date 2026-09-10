#include "screen_presenter.h"
#include "eezui_display_adapter.h"
#include "face_route_v2.h"

#include <cstring>
#include <strings.h>

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_rom_sys.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "lvgl.h"
#include "reminder/boot_trace.h"
#include "src/draw/lv_draw_buf.h"
#include "src/others/snapshot/lv_snapshot.h"

#define TAG "ScreenPresenter"

ScreenPresenter::ScreenPresenter(EezuiDisplayAdapter* host) : host_(host) {
    esp_timer_create_args_t args = {
        .callback = &ScreenPresenter::PresentTimerCallback,
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "screen_present",
        .skip_unhandled_events = true,
    };
    if (esp_timer_create(&args, &present_timer_) != ESP_OK) {
        present_timer_ = nullptr;
    }
    ESP_LOGW(TAG, "CTRL PRESENT init stage=%d compose_blit=%d (C2c=I4b pixels)",
             Stage(), UseComposeBlit() ? 1 : 0);
}

ScreenPresenter::~ScreenPresenter() {
    if (present_timer_ != nullptr) {
        esp_timer_stop(present_timer_);
        esp_timer_delete(present_timer_);
        present_timer_ = nullptr;
    }
    FreePresentBuf();
    FreeOverlayCache();
    if (face_rgb_ != nullptr) {
        heap_caps_free(face_rgb_);
        face_rgb_ = nullptr;
    }
}

void ScreenPresenter::EnterConversation() {
    // C2+: conversation state. Default C2c pixels = I4b (no compose hold).
    if (!StageAtLeast(2)) {
        ESP_LOGW(TAG, "CTRL PRESENT enter skipped stage=%d (need>=2)", Stage());
        return;
    }
    if (mode_ == Mode::kPresentConversation) {
        return;
    }
    const int64_t t0 = esp_timer_get_time();
    esp_rom_printf("!!PRESENT_ENTER core=%d compose=%d\n", (int)xPortGetCoreID(),
                   UseComposeBlit() ? 1 : 0);
    BootTraceMark("PRESENT_ENTER", "begin");
    ESP_LOGW(TAG, "CTRL PRESENT DIAG enter begin core=%d compose=%d",
             (int)xPortGetCoreID(), UseComposeBlit() ? 1 : 0);

    mode_ = Mode::kPresentConversation;
    // C2c: keep I4b emotion bypass; LVGL owns captions between blits.
    legacy_passthrough_ = !UseComposeBlit();
    conversation_since_us_ = t0;
    last_soak_log_us_ = conversation_since_us_;
    present_count_ = 0;

    if (!UseComposeBlit()) {
        if (host_ != nullptr) {
            const esp_err_t serr = host_->PresenterSeedFaceToScreen();
            ESP_LOGW(TAG, "CTRL PRESENT DIAG enter c2c seed err=%s total_ms=%d",
                     esp_err_to_name(serr), (int)((esp_timer_get_time() - t0) / 1000));
        }
        BootTraceMark("PRESENT_ENTER", "done_c2c");
        return;
    }

    BootTraceMark("PRESENT_BUF", "alloc");
    const int64_t tb = esp_timer_get_time();
    EnsurePresentBuf();
    if (host_ != nullptr && (face_rgb_ == nullptr || face_size_ == 0)) {
        const uint8_t* seed = nullptr;
        uint32_t seed_sz = 0, seed_w = 0, seed_h = 0;
        if (host_->PresenterLastFace(&seed, &seed_sz, &seed_w, &seed_h)) {
            CacheFace(seed, seed_sz, seed_w, seed_h);
            ESP_LOGW(TAG, "CTRL PRESENT DIAG seed_face %ux%u", (unsigned)seed_w,
                     (unsigned)seed_h);
        }
    }
    ESP_LOGW(TAG, "CTRL PRESENT DIAG buf_ms=%d px=%u",
             (int)((esp_timer_get_time() - tb) / 1000), (unsigned)present_px_);

    BootTraceMark("PRESENT_STOP", "lvgl_stop");
    if (host_ != nullptr) {
        host_->PresenterAcquirePanel();
    }
    dirty_face_ = (face_rgb_ != nullptr && face_size_ > 0);
    dirty_text_ = true;
    CommitPresent("enter");
    BootTraceMark("PRESENT_ENTER", "done");
    ESP_LOGW(TAG, "CTRL PRESENT DIAG enter done total_ms=%d",
             (int)((esp_timer_get_time() - t0) / 1000));
}

void ScreenPresenter::LeaveConversation() {
    if (!StageAtLeast(2)) {
        return;
    }
    if (mode_ != Mode::kPresentConversation) {
        return;
    }
    if (present_timer_ != nullptr) {
        esp_timer_stop(present_timer_);
    }
    mode_ = Mode::kLegacyLvgl;
    legacy_passthrough_ = true;
#if SCREEN_PRESENTER_USE_STATE_REDUCER
    face_state_.NotifySpeechState(false);
#else
    speech_turn_active_ = false;
#endif
    const int64_t dur_ms = (esp_timer_get_time() - conversation_since_us_) / 1000;
    ESP_LOGW(TAG, "CTRL PRESENT mode=legacy passthrough=1 soak_ms=%d presents=%u",
             (int)dur_ms, (unsigned)present_count_);
    // No sync restore/refr here — goodbye→idle arms wake; refr + emotion_decode
    // CONTEND_STALL kills AFE (neither hear nor speak after sad).
    if (host_ != nullptr) {
        host_->PresenterReleasePanel(false);
    }
}

uint32_t ScreenPresenter::NotifySpeechState(bool speaking) {
#if SCREEN_PRESENTER_USE_STATE_REDUCER
    const uint32_t old_pending_generation = face_state_.pending_generation();
    const auto decision = face_state_.NotifySpeechState(speaking);
    if (decision.generation == 0) {
        return 0;
    }
    ESP_LOGW(TAG, "s1es speech_generation=%u", (unsigned)decision.generation);
    esp_rom_printf("!!FACE_S1ES speech_gen=%u\n", (unsigned)decision.generation);
    if (decision.pending_migrated && !emotion_pending_.empty()) {
        ESP_LOGW(TAG, "s1ew pending_migrate emo=%s old_gen=%u new_gen=%u",
                 emotion_pending_.c_str(), (unsigned)old_pending_generation,
                 (unsigned)decision.generation);
    }
    return decision.generation;
#else
    if (!speaking) {
        speech_turn_active_ = false;
        return 0;
    }
    if (speech_turn_active_) {
        return 0;
    }
    speech_turn_active_ = true;
    ++speech_generation_;
    if (speech_generation_ == 0) ++speech_generation_;
    ESP_LOGW(TAG, "s1es speech_generation=%u", (unsigned)speech_generation_);
    esp_rom_printf("!!FACE_S1ES speech_gen=%u\n", (unsigned)speech_generation_);
    if (!emotion_pending_.empty() && emotion_pending_generation_ != speech_generation_) {
        ESP_LOGW(TAG, "s1ew pending_migrate emo=%s old_gen=%u new_gen=%u",
                 emotion_pending_.c_str(), (unsigned)emotion_pending_generation_,
                 (unsigned)speech_generation_);
        emotion_pending_generation_ = speech_generation_;
    }
    return speech_generation_;
#endif
}

void ScreenPresenter::NotifyEmotion(const char* emotion_name) {
    if (!emotion_name) emotion_name = "neutral";
    emotion_name_ = emotion_name;
    dirty_face_ = true;
    ESP_LOGW(TAG, "CTRL PRESENT notify emotion=%s mode=%d pass=%d", emotion_name,
             static_cast<int>(mode_), legacy_passthrough_ ? 1 : 0);
    ESP_LOGW(TAG, "SAD_DIAG PRESENT notify emo=%s mode=%d compose=%d task=%s", emotion_name,
             static_cast<int>(mode_), UseComposeBlit() ? 1 : 0, pcTaskGetName(nullptr));
    if (host_ == nullptr) {
        ESP_LOGW(TAG, "SAD_DIAG PRESENT notify abort=no_host emo=%s", emotion_name);
        return;
    }

    if (FaceRouteV2_Emotion3Enabled()) {
        emotion_pending_ = emotion_name;
#if SCREEN_PRESENTER_USE_STATE_REDUCER
        face_state_.RequestEmotion(emotion_name);
        const uint32_t pending_generation = face_state_.pending_generation();
        const uint32_t committed_generation = face_state_.committed_generation();
#else
        emotion_pending_generation_ = speech_generation_;
        const uint32_t pending_generation = emotion_pending_generation_;
        const uint32_t committed_generation = emotion_committed_generation_;
#endif
        ESP_LOGW(TAG, "s1es requested→pending emo=%s gen=%u committed=%s cgen=%u",
                 emotion_name, (unsigned)pending_generation,
                 emotion_committed_.empty() ? "-" : emotion_committed_.c_str(),
                 (unsigned)committed_generation);
        esp_rom_printf("!!FACE_S1CN b=pending emo=%s\n", emotion_name);
        if (!host_->PresenterIsFaceRoiAnimActive()) {
            FlushPendingEmotion("notify_idle");
        } else {
            ESP_LOGW(TAG, "s1cn-b defer commit emo=%s (roi_active)", emotion_name);
        }
        return;
    }

    const int64_t t0 = esp_timer_get_time();
    ESP_LOGW(TAG, "CTRL PRESENT commit emo=%s via=presenter s1bj", emotion_name);
    host_->PresenterPlayEmotion(emotion_name);
    ESP_LOGW(TAG, "SAD_DIAG PRESENT play_done emo=%s cost_ms=%d", emotion_name,
             (int)((esp_timer_get_time() - t0) / 1000));
}

void ScreenPresenter::FlushPendingEmotion(const char* why) {
    if (!FaceRouteV2_Emotion3Enabled() || host_ == nullptr) return;
    if (emotion_pending_.empty()) {
        return;
    }
#if SCREEN_PRESENTER_USE_STATE_REDUCER
    const uint32_t generation = face_state_.pending_generation();
    const auto decision = face_state_.CommitPending();
    if (decision == domain::FaceStateReducer::CommitDecision::kDeduplicated) {
        ESP_LOGW(TAG, "s1es dedupe_same_generation emo=%s gen=%u",
                 emotion_pending_.c_str(), (unsigned)generation);
        emotion_pending_.clear();
        return;
    }
    if (decision == domain::FaceStateReducer::CommitDecision::kNoPending) {
        return;
    }
#else
    if (!emotion_committed_.empty() &&
        strcasecmp(emotion_pending_.c_str(), emotion_committed_.c_str()) == 0 &&
        emotion_pending_generation_ == emotion_committed_generation_) {
        ESP_LOGW(TAG, "s1es dedupe_same_generation emo=%s gen=%u",
                 emotion_pending_.c_str(), (unsigned)emotion_pending_generation_);
        emotion_pending_.clear();
        return;
    }
    const uint32_t generation = emotion_pending_generation_;
#endif
    const std::string emo = emotion_pending_;
    emotion_pending_.clear();
    emotion_committed_ = emo;
#if !SCREEN_PRESENTER_USE_STATE_REDUCER
    emotion_committed_generation_ = generation;
#endif
    emotion_name_ = emo;
    const int64_t t0 = esp_timer_get_time();
    ESP_LOGW(TAG, "s1es commit emo=%s gen=%u why=%s via=presenter", emo.c_str(),
             (unsigned)generation, why ? why : "-");
    esp_rom_printf("!!FACE_S1CN b=commit emo=%s\n", emo.c_str());
    host_->PresenterPlayEmotion(emo.c_str());
    ESP_LOGW(TAG, "SAD_DIAG PRESENT play_done emo=%s cost_ms=%d s1cn-b", emo.c_str(),
             (int)((esp_timer_get_time() - t0) / 1000));
}

void ScreenPresenter::OnFaceFrameCached(const uint8_t* rgb, uint32_t size, uint32_t w,
                                        uint32_t h) {
    if (!StageAtLeast(2)) {
        return;
    }
    CacheFace(rgb, size, w, h);
    dirty_face_ = true;
    if (IsConversationMode() && UseComposeBlit()) {
        SchedulePresent("face");
    }
}

void ScreenPresenter::SchedulePresent(const char* reason) {
    if (!IsConversationMode() || !UseComposeBlit()) {
        return;
    }
    // Always async via esp_timer — never Commit under LVGL task lock (typewriter).
    if (present_timer_ == nullptr) {
        return;
    }
    const int64_t now = esp_timer_get_time();
    const int64_t elapsed = (last_present_us_ > 0) ? (now - last_present_us_) : kMinPresentIntervalUs;
    uint64_t wait_us = 1000;  // min 1ms
    if (elapsed < kMinPresentIntervalUs) {
        wait_us = (uint64_t)(kMinPresentIntervalUs - elapsed);
        if (wait_us < 1000) {
            wait_us = 1000;
        }
    }
    esp_timer_stop(present_timer_);
    esp_timer_start_once(present_timer_, wait_us);
    ESP_LOGW(TAG, "CTRL PRESENT schedule wait_us=%u reason=%s",
             (unsigned)wait_us, reason ? reason : "-");
}

esp_err_t ScreenPresenter::CommitPresent(const char* reason) {
    if (host_ == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!IsConversationMode() || !UseComposeBlit()) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!dirty_face_ && !dirty_text_ && face_rgb_ == nullptr) {
        return ESP_OK;
    }
    if (!EnsurePresentBuf()) {
        ESP_LOGW(TAG, "CTRL PRESENT commit fail=no_buf reason=%s", reason ? reason : "-");
        return ESP_ERR_NO_MEM;
    }

    const int64_t t0 = esp_timer_get_time();
    BootTraceMark("PRESENT_COMPOSE", reason ? reason : "-");
    esp_rom_printf("!!PRESENT_COMPOSE %s core=%d\n", reason ? reason : "-",
                   (int)xPortGetCoreID());
    ESP_LOGW(TAG, "CTRL PRESENT DIAG commit begin reason=%s core=%d dirty_f=%d dirty_t=%d",
             reason ? reason : "-", (int)xPortGetCoreID(), dirty_face_ ? 1 : 0,
             dirty_text_ ? 1 : 0);

    esp_err_t err = RenderCompose();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "CTRL PRESENT render fail err=%s reason=%s", esp_err_to_name(err),
                 reason ? reason : "-");
        return err;
    }

    BootTraceMark("PRESENT_BLIT", reason ? reason : "-");
    esp_rom_printf("!!PRESENT_BLIT\n");
    const int64_t tb = esp_timer_get_time();
    host_->PresenterAcquirePanel();
    err = host_->DirectPanelBlit(0, 0, host_->PresenterWidth(), host_->PresenterHeight(),
                                 present_buf_);
    const int64_t blit_ms = (esp_timer_get_time() - tb) / 1000;
    esp_rom_printf("!!PRESENT_BLIT_DONE ms=%d\n", (int)blit_ms);
    const int64_t dt = (esp_timer_get_time() - t0) / 1000;
    last_present_us_ = esp_timer_get_time();
    present_count_++;
    dirty_face_ = false;
    dirty_text_ = false;
    BootTraceMark("PRESENT_DONE", reason ? reason : "-");
    ESP_LOGW(TAG,
             "CTRL PRESENT DIAG commit ok reason=%s total_ms=%d blit_ms=%d n=%u "
             "face=%ux%u err=%s",
             reason ? reason : "-", (int)dt, (int)blit_ms, (unsigned)present_count_,
             (unsigned)face_w_, (unsigned)face_h_, esp_err_to_name(err));
    SoakTick();
    return err;
}

void ScreenPresenter::PresentTimerCallback(void* arg) {
    auto* self = static_cast<ScreenPresenter*>(arg);
    if (self == nullptr) {
        return;
    }
    self->CommitPresent("timer");
}

void ScreenPresenter::SoakTick() {
    if (!StageAtLeast(3)) {
        return;
    }
    const int64_t now = esp_timer_get_time();
    if (now - last_soak_log_us_ < kSoakLogIntervalUs) {
        return;
    }
    last_soak_log_us_ = now;
    const int64_t dur_ms = (now - conversation_since_us_) / 1000;
    ESP_LOGW(TAG, "CTRL PRESENT soak_heartbeat ms=%d presents=%u emo=%s",
             (int)dur_ms, (unsigned)present_count_,
             emotion_name_.empty() ? "-" : emotion_name_.c_str());
}
