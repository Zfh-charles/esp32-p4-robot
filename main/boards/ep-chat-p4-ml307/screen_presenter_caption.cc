#include "screen_presenter.h"

#include "eezui_display_adapter.h"

#include <cstring>

#include <esp_log.h>
#include <esp_rom_sys.h>

#define TAG "ScreenPresenter"

void ScreenPresenter::NotifyDialogue(const char* role, const char* content) {
    if (!content || content[0] == '\0') {
        return;
    }
    dialogue_text_ = content;
    caption_role_ = role ? role : "";
    dirty_text_ = true;

    // L3: queue only while Emotion owns panel; LVGL ROI MID → caption_live.
    if (StageAtLeast(1) && host_ != nullptr && host_->PresenterIsBypassAnimActive()) {
        queued_dialogue_ = content;
        queued_role_ = role ? role : "";
        queued_dialogue_pending_ = true;
        ESP_LOGW(TAG, "CTRL PRESENT dialogue queued (panel hold) len=%u s1an",
                 (unsigned)dialogue_text_.size());
        if (IsConversationMode() && UseComposeBlit()) {
            SchedulePresent("dialogue_queued");
        }
        return;
    }

    // s1bk: during FACE_SLICE ROI, defer paint to PresentFaceFrameToLvgl coalesce.
    if (host_ != nullptr && host_->PresenterIsFaceRoiAnimActive()) {
        ESP_LOGW(TAG, "SAD_DIAG L3 caption_defer coalesce role=%s len=%u s1bk",
                 role ? role : "-", (unsigned)dialogue_text_.size());
        return;
    }

    ApplyDialogueToLvglNow(role, content);
    dirty_text_ = false;
    ESP_LOGW(TAG, "SAD_DIAG L3 caption_live role=%s len=%u s1an", role ? role : "-",
             (unsigned)dialogue_text_.size());
    if (IsConversationMode() && UseComposeBlit()) {
        SchedulePresent("dialogue");
        return;
    }
    ESP_LOGW(TAG, "CTRL PRESENT dialogue apply len=%u stage=%d compose=%d",
             (unsigned)dialogue_text_.size(), Stage(), UseComposeBlit() ? 1 : 0);
}

void ScreenPresenter::NotifyStatusPhase(const char* phase_label) {
    if (!phase_label) {
        return;
    }
    status_phase_ = phase_label;
    dialogue_text_ = phase_label;
    caption_role_ = "status";
    dirty_text_ = true;

    if (StageAtLeast(1) && host_ != nullptr && host_->PresenterIsBypassAnimActive()) {
        queued_dialogue_ = phase_label;
        queued_role_ = "status";
        queued_dialogue_pending_ = true;
        ESP_LOGW(TAG, "CTRL PRESENT status queued (panel hold) s1an");
        if (IsConversationMode() && UseComposeBlit()) {
            SchedulePresent("status_queued");
        }
        return;
    }

    if (host_ != nullptr && host_->PresenterIsFaceRoiAnimActive()) {
        ESP_LOGW(TAG, "SAD_DIAG L3 status_defer coalesce len=%u s1bk",
                 (unsigned)strlen(phase_label));
        return;
    }

    ApplyDialogueToLvglNow("status", phase_label);
    dirty_text_ = false;
    ESP_LOGW(TAG, "SAD_DIAG L3 caption_live role=status len=%u s1an",
             (unsigned)strlen(phase_label));
    if (IsConversationMode() && UseComposeBlit()) {
        SchedulePresent("status");
    }
}

void ScreenPresenter::OnBypassAnimEnded() {
    if (!StageAtLeast(1)) {
        return;
    }
    if (queued_dialogue_pending_) {
        ApplyDialogueToLvglNow(queued_role_.c_str(), queued_dialogue_.c_str());
        ClearQueuedDialogue();
        dirty_text_ = false;
    } else if (dirty_text_ && !dialogue_text_.empty()) {
        const char* role = caption_role_.empty() ? "assistant" : caption_role_.c_str();
        ApplyDialogueToLvglNow(role, dialogue_text_.c_str());
        dirty_text_ = false;
        ESP_LOGW(TAG, "SAD_DIAG L3 caption_flush_end role=%s len=%u s1cr", role,
                 (unsigned)dialogue_text_.size());
        esp_rom_printf("!!FACE_CAPTION flush_end s1cr\n");
    }
    if (IsConversationMode() && UseComposeBlit()) {
        CommitPresent("anim_end");
    }
}

bool ScreenPresenter::FlushCaptionIfDirty() {
    if (!dirty_text_ || dialogue_text_.empty() || host_ == nullptr) {
        return false;
    }
    const char* role = caption_role_.empty() ? "assistant" : caption_role_.c_str();
    host_->PresenterApplyDialogueAssumingLock(role, dialogue_text_.c_str());
    dirty_text_ = false;
    ESP_LOGW(TAG, "SAD_DIAG L3 caption_coalesce role=%s len=%u s1bk", role,
             (unsigned)dialogue_text_.size());
    return true;
}

void ScreenPresenter::ClearQueuedDialogue() {
    queued_dialogue_pending_ = false;
    queued_dialogue_.clear();
    queued_role_.clear();
}

void ScreenPresenter::ApplyDialogueToLvglNow(const char* role, const char* content) {
    if (host_ == nullptr || !content) {
        return;
    }
    host_->PresenterApplyDialogue(role, content);
}
