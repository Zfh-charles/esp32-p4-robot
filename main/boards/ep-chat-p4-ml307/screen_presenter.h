#ifndef SCREEN_PRESENTER_H
#define SCREEN_PRESENTER_H

#include <cstdint>
#include <string>

#include <esp_err.h>
#include <esp_timer.h>

#include "domain/face_state_reducer.h"

class EezuiDisplayAdapter;

/**
 * Iteration gate (hypothesis → verify → next). Only raise after the current stage passes.
 *   0 = C0 scaffolding, pixel path == I4b hold-anim
 *   1 = C1 state + dialogue queue during anim
 *   2 = C2c conversation state; pixels = I4b (bypass face + LVGL captions).
 *       Full-screen compose blit caused HP_WDT + silent TTS — deferred.
 *   3 = C3 soak / optional compose re-enable after coexist proven
 *
 * SCREEN_PRESENT_USE_COMPOSE_BLIT=1 re-enables experimental face+text offscreen compose.
 */
#ifndef SCREEN_PRESENT_STAGE
#define SCREEN_PRESENT_STAGE 2
#endif

#ifndef SCREEN_PRESENT_USE_COMPOSE_BLIT
#define SCREEN_PRESENT_USE_COMPOSE_BLIT 0
#endif

// R0 for the G3b state-owner flip. Zero restores the legacy scalar fields and
// branch logic without changing any presenter call site.
#ifndef SCREEN_PRESENTER_USE_STATE_REDUCER
#define SCREEN_PRESENTER_USE_STATE_REDUCER 1
#endif

/**
 * React-style screen presenter: UI = f(ScreenState).
 * C2c default: state + queue; pixels via host I4b (not continuous compose).
 */
class ScreenPresenter {
public:
    enum class Mode {
        kLegacyLvgl = 0,
        kPresentConversation = 1,
    };

    explicit ScreenPresenter(EezuiDisplayAdapter* host);
    ~ScreenPresenter();

    static constexpr int Stage() { return SCREEN_PRESENT_STAGE; }
    static constexpr bool StageAtLeast(int s) { return SCREEN_PRESENT_STAGE >= s; }
    static constexpr bool UseComposeBlit() { return SCREEN_PRESENT_USE_COMPOSE_BLIT != 0; }

    void EnterConversation();
    void LeaveConversation();
    /** Mark Listening<->Speaking edges so same-emotion replies are new render generations. */
    /** Returns the new generation once per Listening->Speaking edge, otherwise zero. */
    uint32_t NotifySpeechState(bool speaking);
    bool IsConversationMode() const {
        return StageAtLeast(2) && mode_ == Mode::kPresentConversation;
    }

    void NotifyEmotion(const char* emotion_name);
    /** s1cn-b: commit pending emotion at a safe switch point (slice done / idle). */
    void FlushPendingEmotion(const char* why);
    void NotifyDialogue(const char* role, const char* content);
    void NotifyStatusPhase(const char* phase_label);

    void OnFaceFrameCached(const uint8_t* rgb, uint32_t size, uint32_t w, uint32_t h);
    void OnBypassAnimEnded();

    /** s1bk: apply dirty caption under caller's LVGL lock (ROI coalesce). Returns true if painted. */
    bool FlushCaptionIfDirty();

    bool HasQueuedDialogue() const { return queued_dialogue_pending_; }
    const std::string& QueuedDialogue() const { return queued_dialogue_; }
    void ClearQueuedDialogue();

    void SchedulePresent(const char* reason);
    esp_err_t CommitPresent(const char* reason);

    Mode mode() const { return mode_; }
    bool legacy_passthrough() const { return legacy_passthrough_; }

private:
    static void PresentTimerCallback(void* arg);
    bool EnsurePresentBuf();
    void FreePresentBuf();
    bool CacheFace(const uint8_t* rgb, uint32_t size, uint32_t w, uint32_t h);
    esp_err_t RenderCompose();
    void ApplyDialogueToLvglNow(const char* role, const char* content);
    void SoakTick();
    void FreeOverlayCache();
    void ApplyOverlayCache(int W, int H);
    bool StoreOverlayCache(const uint16_t* src, int stride_px, int x0, int y0, int sw, int sh);

    EezuiDisplayAdapter* host_ = nullptr;
    Mode mode_ = Mode::kLegacyLvgl;
    /** true → emotion uses host ShowEmotionViaBypass (I4b). */
    bool legacy_passthrough_ = true;

    std::string emotion_name_;
    /** s1cn-b: requested → pending → committed (overwrite pending; commit at safe points). */
    std::string emotion_pending_;
    std::string emotion_committed_;
#if SCREEN_PRESENTER_USE_STATE_REDUCER
    domain::FaceStateReducer face_state_;
#else
    uint32_t speech_generation_ = 0;
    uint32_t emotion_pending_generation_ = 0;
    uint32_t emotion_committed_generation_ = 0;
    bool speech_turn_active_ = false;
#endif
    std::string dialogue_text_;
    std::string caption_role_;
    std::string status_phase_;
    std::string queued_dialogue_;
    std::string queued_role_;
    bool queued_dialogue_pending_ = false;
    bool dirty_face_ = false;
    bool dirty_text_ = false;

    uint8_t* face_rgb_ = nullptr;
    uint32_t face_size_ = 0;
    uint32_t face_cap_ = 0;
    uint32_t face_w_ = 0;
    uint32_t face_h_ = 0;

    uint16_t* present_buf_ = nullptr;
    uint32_t present_px_ = 0;

    uint16_t* overlay_rgb_ = nullptr;
    uint32_t overlay_cap_px_ = 0;
    int overlay_x_ = 0;
    int overlay_y_ = 0;
    int overlay_w_ = 0;
    int overlay_h_ = 0;
    bool overlay_valid_ = false;

    esp_timer_handle_t present_timer_ = nullptr;
    int64_t last_present_us_ = 0;
    int64_t conversation_since_us_ = 0;
    int64_t last_soak_log_us_ = 0;
    uint32_t present_count_ = 0;

    static constexpr int64_t kMinPresentIntervalUs = 2000000;  // compose experimental only
    static constexpr int64_t kSoakLogIntervalUs = 30000000;
};

#endif  // SCREEN_PRESENTER_H
