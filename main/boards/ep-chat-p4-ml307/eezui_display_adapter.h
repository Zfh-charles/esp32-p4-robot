#ifndef EEZUI_DISPLAY_ADAPTER_H
#define EEZUI_DISPLAY_ADAPTER_H

#include "display.h"
#include "idle_flash_band_overlay.h"
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <esp_timer.h>
#include <atomic>
#include <string>
#include <vector>
#include <map>

extern "C" {
#include "emotion_video_player.h"
}

struct EmotionMapping {
    const char* emotion_name;
    const char* mjpeg_file;
};

enum class Status {
    kIdle,
    kListening,
    kThinking,
    kSpeaking,
    kConnecting,
    kError
};

extern "C" {
#include "ui/ui.h"
#include "ui/screens.h"
}

class EezuiDisplayAdapter;
class ScreenPresenter;

struct TimerData {
    EezuiDisplayAdapter* adapter;
    lv_obj_t* dialogue_box;
    lv_timer_t* hide_timer;
    ~TimerData();
};

class EezuiDisplayAdapter : public Display {
protected:
    esp_lcd_panel_io_handle_t panel_io_ = nullptr;
    esp_lcd_panel_handle_t panel_ = nullptr;

    lv_obj_t* dialogue_box_ = nullptr;
    lv_obj_t* main_image_ = nullptr;

    std::string typewriter_text_;
    size_t typewriter_index_;
    lv_timer_t* typewriter_timer_;
    bool typewriter_active_;

    lv_timer_t* hide_timer_ = nullptr;
    bool is_persistent_message_ = false;

    enum class InitState {
        NONE = 0,
        UI_READY = 1,
        EMOTION_READY = 2,
        ALL_READY = 3
    };

    Status current_status_ = Status::kIdle;
    InitState init_state_ = InitState::NONE;

    emotion_video_handle_t emotion_player_ = nullptr;
    lv_obj_t* video_canvas_ = nullptr;
    std::string current_emotion_name_;

    void SetupUI();
    void SetupBatteryUI();
    void StartTypewriterEffect(const std::string& text);
    void StopTypewriterEffect();
    void UpdateDialogueBoxHeight();

    bool InitEmotionSystem();
    void DeinitEmotionSystem();
    esp_err_t PlayMjpegEmotion(const char* emotion_name);
    void StopEmotionPlayback();
    void CreateVideoCanvas();

    void EnsureUILayerOrder();
    bool SafeLVGLLock(int timeout_ms = 100);
    void SafeLVGLUnlock();

    static void EmotionVideoFrameCallback(emotion_video_handle_t handle, uint8_t* frame_data,
                                          uint32_t frame_size, uint32_t width, uint32_t height,
                                          void* user_data);
    static void EmotionVideoEventCallback(emotion_video_event_t event, void* user_data);

    static void TypewriterTimerCallback(lv_timer_t* timer);
    static void DialogueBoxClickCallback(lv_event_t* e);
    virtual bool Lock(int timeout_ms = 0) override;
    virtual void Unlock() override;

    EezuiDisplayAdapter(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                        int width, int height);

public:
    virtual ~EezuiDisplayAdapter();

    virtual void SetEmotion(const char* emotion) override;
    virtual void SetChatMessage(const char* role, const char* content) override;
    virtual void UpdateStatusBar(bool update_all = false) override;

    esp_err_t ForceStopAndSwitchEmotion(const char* emotion_name);

    virtual void SetStatus(const char* status, const char* time = nullptr);
    virtual void SetStatus(const char* status);
    virtual void SetStatus(Status status);

    virtual void ShowNotification(const char* notification, int duration_ms = 3000) override;
    virtual void ShowNotification(const std::string& notification, int duration_ms = 3000) override;

    bool IsUIInitialized() const {
        return (static_cast<int>(init_state_) & static_cast<int>(InitState::UI_READY)) != 0;
    }
    bool IsUIReady() const { return IsUIInitialized(); }
    bool IsEmotionSystemReady() const {
        return (static_cast<int>(init_state_) & static_cast<int>(InitState::EMOTION_READY)) != 0;
    }
    bool IsFullyInitialized() const { return init_state_ == InitState::ALL_READY; }

    void SetHideTimer(lv_timer_t* timer) { hide_timer_ = timer; }
    emotion_video_handle_t GetEmotionPlayerHandle() const { return emotion_player_; }

    void PauseMjpegHeavyWork();
    void ResumeMjpegHeavyWork();
    void ResumeMjpegHeavyWorkAtFps(uint32_t fps);
    void StartDeferredEmotionPreload();
    /** P2+S1: file+index preload and RGB still seed (wake=0). */
    bool PreloadBaseEmotionsSync();
    /** After pause/P2: put SD seed face on canvas+panel (not boot splash). */
    esp_err_t CommitSdSeedFace(const char* emotion_name, bool panel_blit);
    /**
     * Boot/wake=0: sync LVGL face selftest (seed+incr invalidate). Logs FACE_SELFTEST.
     * Call before AFE arm so flush cannot fight wake.
     */
    esp_err_t RunLvglFaceSelfTest(const char* emotion_name);

    esp_err_t DirectPanelBlit(int x, int y, int w, int h, const uint16_t* rgb565);
    void RunI1DirectBlitSmokeOnce();
    void RunI2EmotionBypassOnce();

    void EnterConversationPresent();
    void LeaveConversationPresent();
    /** Protocol-thread notifications; atomic only, no LVGL/SD work. */
    void NotifyTtsStart();
    void NotifyTtsAudioFirst();

    void PresenterPlayEmotion(const char* emotion_name);
    void PresenterApplyDialogue(const char* role, const char* content);
    /** Caller already holds SafeLVGLLock — no nested lvgl_port_lock (s1bk coalesce). */
    void PresenterApplyDialogueAssumingLock(const char* role, const char* content);
    void PresenterAcquirePanel();
    void PresenterReleasePanel(bool restore_ui);
    // L3/s1an: queue captions only while Emotion owns panel (DirectPanelBlit).
    // LVGL ROI MID keeps panel with LVGL — captions apply live (caption_live).
    bool PresenterIsBypassAnimActive() const {
        return bypass_lvgl_stopped_ || panel_owner_ == PanelOwner::kEmotion;
    }
    /** True while FACE_SLICE ROI / settle is running — defer caption to ROI coalesce (s1bk).
     *  s1cr: enter_arc is full-frame stills; captions apply live (do not defer). */
    bool PresenterIsFaceRoiAnimActive() const {
        if (face_anim_enter_mode_) {
            return false;
        }
        return face_anim_left_ > 0 || face_anim_need_prime_ || face_anim_await_settle_ ||
               face_anim_sustain_;
    }
    bool PresenterSafeLVGLLock(int timeout_ms);
    void PresenterSafeLVGLUnlock();
    lv_obj_t* PresenterDialogueBox() const { return dialogue_box_; }
    int PresenterWidth() const { return width_; }
    int PresenterHeight() const { return height_; }
    bool PresenterLastFace(const uint8_t** rgb, uint32_t* size, uint32_t* w, uint32_t* h) const;
    esp_err_t PresenterSeedFaceToScreen();

private:
    friend class ScreenPresenter;
    enum class PanelOwner { kLvgl = 0, kEmotion = 1 };

    void RestoreUiAfterDirectBlit(bool full_refr = true);
    void AcquirePanelForEmotion();
    void ReleasePanelToLvgl(bool restore_ui);
    void YieldPanelForUi(const char* reason);
    /** S1: blit boot-seeded RGB still — no session JPEG. */
    esp_err_t ShowEmotionViaBypass(const char* emotion_name);
    esp_err_t SyncCanvasFromRgb(const uint8_t* rgb, uint32_t size, uint32_t w, uint32_t h);
    /**
     * S1s/S1w/S1x LVGL face present.
     * seed_full: full canvas invalidate.
     * roi_only: face ROI invalidate (s1bs: full-width dirty row-band); never full-flush.
     */
    esp_err_t PresentFaceFrameToLvgl(const uint8_t* rgb, uint32_t size, uint32_t w, uint32_t h,
                                     bool seed_full, bool roi_only = false);
    /** Caller already holds SafeLVGLLock: hide main_image, show canvas, caption on top. */
    void ShowFaceCanvasLayers();
    /** s1bo: MID frame interval for the current cadence step. */
    int FaceCadenceMs(bool speaking) const;
    /** s1bo: back off the MID cadence from measured present cost / LVGL lock timeouts. */
    void NoteFacePresentPressure(int present_ms, esp_err_t perr, const char* phase);
    esp_err_t BlitRgbFrame(const uint8_t* rgb, uint32_t w, uint32_t h);
    /** Live path: strip blit + yield — no lvgl_port_stop. */
    esp_err_t BlitRgbFrameChunked(const uint8_t* rgb, uint32_t w, uint32_t h);
    /** S1e: HW partial — draw_bitmap ROI from full-frame seed (row stride = src_w). */
    esp_err_t BlitRgbRoiRowwise(const uint8_t* rgb, uint32_t src_w, uint32_t src_h, int roi_x,
                                int roi_y, int roi_w, int roi_h);
    esp_err_t PaintSeedEmotionPartial(const char* emotion_name);
    /** Speaking/TTS-safe: time-slice open/mid/close (s1ae). roi_follow=false → open still only. */
    esp_err_t CommitFaceStillOnce(const char* emotion_name, bool roi_follow = true);
    /** Paint standby seed as time-slice bookend (OPEN/CLOSE). */
    esp_err_t PresentStandbyBookend(const char* tag);
    void SetFaceLayersHiddenForPartial(bool hide_face_layers);
    void ShowSdFaceCanvasOnly();
    void HideVideoCanvasForDirectFace();
    bool CacheLastBypassFrame(const uint8_t* rgb, uint32_t size, uint32_t w, uint32_t h);
    esp_err_t ReblitLastBypassFrame();
    bool InConversationPresent() const;
    void StopFacePanelAnim(const char* reason);
    void StartFacePanelAnim(const char* emotion_name);
    /** L2/s1am: Presenter entry; speak-settle then MID12 (TTS-safe schedule). */
    void StartFaceRoiFollow(int follow_frames);
    /** s1cp-g: uniform full-frame enter then hold on seed (no band MID). */
    void StartFaceEnterTransition(const char* emotion_name);
    /** s1cr-h: bind layered mouth + arm speak-time mouth follow (no-op without SD pack). */
    bool ArmMouthFollow(const char* emotion_name, bool base_already_present = false);
    void StopMouthFollow(const char* why);
    void MouthFollowTick();
    esp_err_t PresentMouthPatch(uint8_t level);
    esp_err_t PresentLifeBand(uint8_t track, uint8_t frame, uint32_t lock_wait_ms = 40);
    esp_err_t PresentIdleFlashBand(uint8_t frame);
    esp_err_t PresentReleaseBand(uint8_t frame);
    bool ArmEmotionEnter(const char* emotion_name);
    bool ArmEmotionRelease(const char* why);
    void StopEmotionRelease(const char* why);
    void EmotionReleaseTick();
    esp_err_t PresentEyePatch(uint8_t level);
    esp_err_t PresentPoseBase(uint8_t pose);
    /** s1as: idle standby ROI breathe — budgeted cycle, rest back to seed bookmark. */
    void ArmIdleBreathe(const char* why);
    void StopIdleBreathe(const char* why);
    void IdleBreatheTick();
    bool ArmIdleBacklightBreathe(const char* why);
    void StopIdleBacklightBreathe(const char* why);
    void IdleBacklightBreatheTick();
    static void IdleBacklightBreatheTimerCb(void* arg);
    bool EnsureFaceAnimTimer();
    void FacePanelAnimTick();
    static void FacePanelAnimTimerCb(void* arg);
    static void FaceWorkerTickTrampoline(void* ctx);
    void HideFaceLayersForPanelHold();
    /** Same EEZ caption look before/during conversation. */
    void EnsureDialogueCaptionStyle();
    void ApplyDialogueCaption(const char* content, bool use_typewriter);
    void CancelIdleHideTimer();

    ScreenPresenter* presenter_ = nullptr;
    lv_obj_t* battery_panel_ = nullptr;
    lv_obj_t* battery_gauge_ = nullptr;
    lv_obj_t* battery_body_ = nullptr;
    lv_obj_t* battery_fill_ = nullptr;
    lv_obj_t* battery_tip_ = nullptr;
    lv_obj_t* battery_percent_label_ = nullptr;
    int last_battery_level_ = -1;
    bool last_battery_charging_ = false;
    bool last_battery_low_ = false;
    int64_t last_battery_update_us_ = 0;
    bool bypass_lvgl_stopped_ = false;
    PanelOwner panel_owner_ = PanelOwner::kLvgl;
    uint8_t* last_bypass_rgb_ = nullptr;
    uint32_t last_bypass_size_ = 0;
    uint32_t last_bypass_cap_ = 0;
    uint32_t last_bypass_w_ = 0;
    uint32_t last_bypass_h_ = 0;

    /** S1s/S1w: LVGL face follow on esp_timer (ROI-only when face_anim_roi_only_). */
    esp_timer_handle_t face_anim_timer_ = nullptr;
    std::string face_anim_emo_;
    int face_anim_left_ = 0;
    /** Remaining ROI frames allowed this emotion (cap TTS-safe follow). */
    int face_anim_roi_budget_ = 0;
    /** s1ad/s1af: remaining frames at fast burst interval before slow follow. */
    int face_anim_fast_left_ = 0;
    /** s1af: MID done but TTS still speaking — hold canvas, close on speak end. */
    bool face_anim_sustain_ = false;
    /** s1am: speaking — wait TTS/I2S settle before ROI MID (serial: WDT on 2nd frame). */
    bool face_anim_await_settle_ = false;
    int face_anim_pending_follow_ = 0;
    uint32_t face_anim_gen_ = 0;
    bool face_anim_need_prime_ = false;
    bool face_anim_roi_only_ = false;
    /** s1cp-g: FACE_SLICE enter uses seed_full every tick; ends on emotion seed. */
    bool face_anim_enter_mode_ = false;
    /** s1cr-h: speaking mouth patch follow (independent of enter/MID). */
    bool mouth_follow_ = false;
    /** Keep a just-committed strong emotion visible when cloud TTS has no audio. */
    bool strong_emotion_hold_pending_ = false;
    int64_t strong_emotion_hold_until_us_ = 0;
    /** s1et: angry/sad -> standby hub, one precomposited <=48-row band per tick. */
    bool emotion_release_active_ = false;
    bool emotion_release_entering_ = false;
    uint8_t emotion_release_frame_ = 0;
    uint8_t mouth_pose_ = 0;
    /** Displayed mouth pose. Target PCM level is rate-limited to avoid hard jumps. */
    uint8_t mouth_visual_level_ = 0;
    uint8_t mouth_budget_phase_ = 0;
    uint8_t mouth_budget_logged_level_ = 0xff;
    uint8_t pose_blink_stage_ = 0;
    bool pose_blink_will_swap_ = false;
    int64_t eye_blink_due_us_ = 0;
    int64_t pose_switch_due_us_ = 0;
    uint8_t life_target_pose_ = 0;
    uint8_t life_blend_stage_ = 0;
    bool life_budget_deferred_ = false;
    int64_t life_due_us_ = 0;
    /** s1bl: frames advanced per MID tick after prime (1 = consecutive). */
    uint32_t face_anim_arc_step_ = 1;
    /** s1cb/E: absolute MJPEG frame index for MID decode_at (O(1) per tick). */
    uint32_t face_anim_frame_idx_ = 0;
    /** s1bo: MID cadence step — 0 = target rate; raised only on measured present/lock cost. */
    int face_cadence_step_ = 0;
    int face_slow_streak_ = 0;
    uint32_t face_rate_log_gen_ = 0;
    /** s1bv: when row-band is capped, alternate top vs bottom anchor each present. */
    bool face_band_cap_bot_ = false;
    /** s1as: idle-only standby micro-motion (not speak loop_continue). */
    bool idle_breathe_ = false;
    /** s1ep-k: standby life assets replace legacy 400-row MJPEG breathe. */
    bool idle_breathe_small_life_ = false;
    /** s1gt: flash-mapped opaque child image; never writes/reads the PSRAM face canvas. */
    IdleFlashBandOverlay idle_flash_overlay_;
    bool idle_breathe_flash_overlay_ = false;
    uint8_t idle_life_track_ = 0;
    uint8_t idle_life_frame_ = 0;
    uint8_t idle_life_repair_attempts_ = 0;
    bool idle_life_repair_pending_ = false;
    bool idle_breathe_need_prime_ = false;
    int idle_breathe_left_ = 0;
    uint32_t idle_breathe_gen_ = 0;
    /** s1bx: standby breathe arc step (fc / cycle_frames), not consecutive mid-clip. */
    uint32_t idle_breathe_arc_step_ = 1;
    /** s1by: absolute MJPEG frame index for next decode_at (O(1) per tick). */
    uint32_t idle_breathe_frame_idx_ = 0;
    /** s1fd: PSRAM-free standby life via low-amplitude PWM backlight pulse. */
    esp_timer_handle_t idle_backlight_timer_ = nullptr;
    bool idle_backlight_breathe_ = false;
    uint8_t idle_backlight_base_ = 0;
    uint8_t idle_backlight_last_command_ = 0;
    uint8_t idle_backlight_phase_ = 0;
    /** LLM emotion arrived on listen edge — play when speaking starts. */
    std::string pending_face_emo_;
    uint32_t speech_core_generation_ = 0;
    uint32_t speech_core_pending_generation_ = 0;
    std::atomic<bool> speech_core_tts_started_{false};
    bool speech_core_full_committed_ = false;
    /** Idle breathe (or other) currently holds AfeFetchGate — s1cn-c must not nest-lock. */
    bool face_holds_afe_gate_ = false;
};

class MipiEezuiDisplayAdapter : public EezuiDisplayAdapter {
public:
    MipiEezuiDisplayAdapter(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                            int width, int height, int offset_x, int offset_y, bool mirror_x,
                            bool mirror_y, bool swap_xy);
};

#endif  // EEZUI_DISPLAY_ADAPTER_H
