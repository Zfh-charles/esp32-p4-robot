#ifndef _APPLICATION_H_
#define _APPLICATION_H_

#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <freertos/task.h>
#include <esp_timer.h>

#include <string>
#include <atomic>
#include <mutex>
#include <deque>
#include <memory>

#include "protocol.h"
#include "ota.h"
#include "audio_service.h"
#include "device_state_event.h"
#if CONFIG_USE_ALARM
#include "general_timer.h"
#endif
#if CONFIG_USE_REMINDER_POLL
#include "reminder/reminder_poller.h"
#include "reminder/reminder_diag.h"
#if CONFIG_REMINDER_MQTT_WAKE
#include "reminder/reminder_mqtt_wake.h"
#endif
#endif


#define MAIN_EVENT_SCHEDULE (1 << 0)
#define MAIN_EVENT_SEND_AUDIO (1 << 1)
#define MAIN_EVENT_WAKE_WORD_DETECTED (1 << 2)
#define MAIN_EVENT_VAD_CHANGE (1 << 3)
#define MAIN_EVENT_ERROR (1 << 4)
#define MAIN_EVENT_CHECK_NEW_VERSION_DONE (1 << 5)
#define MAIN_EVENT_CLOCK_TICK (1 << 6)
#if CONFIG_USE_ALARM
#define MAIN_EVENT_ALARM (1 << 7)
#endif
#if CONFIG_BOARD_TYPE_EP_CHAT_P4_ML307
#define MAIN_EVENT_TTS_UDP_PRIME (1 << 8)
#endif



enum AecMode {
    kAecOff,
    kAecOnDeviceSide,
    kAecOnServerSide,
};

/** Who owns the current cloud/audio session — drives poller gate and audio policy. */
enum class SessionKind {
    None,               /* idle standby, wake word on */
    User,               /* user-initiated wake / conversation */
    ProactiveReminder,  /* device-initiated mcp_wake reminder */
};

class Application {
public:
    static Application& GetInstance() {
        static Application instance;
        return instance;
    }
    // 删除拷贝构造函数和赋值运算符
    Application(const Application&) = delete;
    Application& operator=(const Application&) = delete;

    void Start();
    void MainEventLoop();
    DeviceState GetDeviceState() const { return device_state_; }
    bool IsVoiceDetected() const { return audio_service_.IsVoiceDetected(); }
    void Schedule(std::function<void()> callback);
    void SetDeviceState(DeviceState state);
    void Alert(const char* status, const char* message, const char* emotion = "", const std::string_view& sound = "");
    void DismissAlert();
    void AbortSpeaking(AbortReason reason);
    void ToggleChatState();
    void StartListening();
    void StopListening();
    void HandleNetworkDown();
    void Reboot();
    void WakeWordInvoke(const std::string& wake_word);
    bool UpgradeFirmware(Ota& ota, const std::string& url = "");
    bool CanEnterSleepMode();
    void SendMcpMessage(const std::string& payload);
    void SetAecMode(AecMode mode);
    AecMode GetAecMode() const { return aec_mode_; }
    void PlaySound(const std::string_view& sound);
    void DeliverReminderSpeech(const std::string& message, const char* emotion = "neutral");
#if CONFIG_USE_REMINDER_POLL
    void DeliverReminder(ReminderDeliveryMode mode, const std::string& id, const std::string& prompt,
                         const std::string& wake_text, const char* emotion, const std::string& ack_url);
    SessionKind GetSessionKind() const { return session_kind_; }
    bool IsProactiveReminderPending() const { return !pending_reminder_ack_id_.empty(); }
    bool IsUserConversationActive() const;
    bool CanDeliverReminder() const;
    void ReminderTraceLog(const char* event, const char* detail = nullptr) const;
    ReminderDiagSnapshot BuildReminderDiagSnapshot();
    void RunReminderDiagnostics(const char* trigger, bool allow_auto_heal = false);
#endif
    AudioService& GetAudioService() { return audio_service_; }
    #if CONFIG_USE_ALARM
    GeneralTimer *general_timer_ = nullptr;
    bool IsAlarmRinging() const;
    void SendMessage(std::string& message);
    void SetAlarmEvent();
    void ClearAlarmEvent();
#endif

private:
    Application();
    ~Application();

    std::mutex mutex_;
    std::deque<std::function<void()>> main_tasks_;
    std::unique_ptr<Protocol> protocol_;
    EventGroupHandle_t event_group_ = nullptr;
    esp_timer_handle_t clock_timer_handle_ = nullptr;
    volatile DeviceState device_state_ = kDeviceStateUnknown;
    ListeningMode listening_mode_ = kListeningModeAutoStop;
    AecMode aec_mode_ = kAecOff;
    std::string last_error_message_;
    AudioService audio_service_;

    bool has_server_time_ = false;
    bool aborted_ = false;
    std::atomic<bool> visual_tts_audio_started_{false};
    int clock_ticks_ = 0;
    TaskHandle_t check_new_version_task_handle_ = nullptr;
    TaskHandle_t main_event_loop_task_handle_ = nullptr;

    void OnWakeWordDetected();
    void CheckNewVersion(Ota& ota);
    void CheckAssetsVersion();
    void ShowActivationCode(const std::string& code, const std::string& message);
    void SetListeningMode(ListeningMode mode);
    void ApplyAudioPolicyForState(DeviceState state, DeviceState previous_state);
    void RestoreIdleReady(bool force_capture = true);
    void EndSessionAndRestoreIdle(bool force_capture = true);

#if CONFIG_USE_REMINDER_POLL
    SessionKind session_kind_ = SessionKind::None;
    ReminderPoller* reminder_poller_ = nullptr;
#if CONFIG_REMINDER_MQTT_WAKE
    ReminderMqttWake* reminder_mqtt_wake_ = nullptr;
#endif
    std::string pending_reminder_ack_id_;
    std::string pending_reminder_ack_url_;
    bool proactive_reminder_tts_started_ = false;
    int proactive_audio_packets_ = 0;
    esp_timer_handle_t proactive_reminder_timer_handle_ = nullptr;
    void CompletePendingReminderAck();
    void CancelPendingReminderAck();
    void StartProactiveReminderTimeout();
    void StopProactiveReminderTimeout();
    void StartProactiveFeedbackWindow();
    void BeginProactiveFeedbackWindow();
    void HandleProactiveFeedbackTimer();
    void FinishProactiveReminder();
    void StopProactiveFeedbackTimer();
    void SetSessionKind(SessionKind kind, const char* reason);
    struct ReminderDeliverPayload {
        ReminderDeliveryMode effective_mode = ReminderDeliveryMode::kMcpWake;
        std::string id;
        std::string display_text;
        std::string detect_text;
        std::string emotion_str;
        std::string ack_url;
    };
    void RunReminderDelivery(const ReminderDeliverPayload& payload);
    /** Scheme D: single-path idle recovery (one I2S switch + wake rearm). */
    void EnterIdleStandby(bool show_wake_hint = true);
    void UpdateCapturePowerHold();
    void TryStartDeferredReminderNet();
#if CONFIG_BOARD_TYPE_EP_CHAT_P4_ML307
    void TryStartDeferredEmotionPreload();
#endif
    esp_timer_handle_t proactive_feedback_timer_handle_ = nullptr;
    bool proactive_feedback_waiting_drain_ = false;
    uint16_t proactive_feedback_drain_checks_ = 0;
    bool deferred_reminder_services_pending_ = false;
    bool deferred_reminder_net_started_ = false;
    int64_t wake_running_since_us_ = 0;
    int64_t reminder_net_started_since_us_ = 0;
    int reminder_net_defer_sec_ = CONFIG_REMINDER_BOOT_DEFER_SEC;
    bool boot_flash_protection_mode_ = false;
    uint32_t boot_crash_streak_ = 0;
    bool idle_rearm_in_progress_ = false;
    std::string last_reminder_display_text_;
    std::string last_reminder_emotion_;
    bool suppress_display_clear_on_channel_close_ = false;
    bool suppress_channel_close_rearm_ = false;
    bool pending_reminder_after_channel_close_ = false;
    int64_t last_idle_standby_us_ = 0;
    ReminderDeliverPayload pending_reminder_payload_;
#endif
#if CONFIG_BOARD_TYPE_EP_CHAT_P4_ML307
    bool deferred_emotion_preload_started_ = false;
    int visual_budget_shadow_level_ = -1;
    uint32_t visual_budget_shadow_samples_ = 0;
    void UpdateVisualBudgetShadow();
    bool mjpeg_wake_coexist_paused_ = false;
    /** Proven stable: do not auto-resume MJPEG while AFE wake is armed (HP_WDT). */
    bool mjpeg_skip_resume_while_wake_ = true;
    /**
     * Proven stable (v8/v9-r0): pause MJPEG for entire conversation.
     * Face stays on last idle frame; no decode/flush during listen/speak.
     * P2: base emotions preloaded once before first wake arm (cache for later bypass).
     */
    bool mjpeg_skip_resume_while_listening_ = true;
    bool mjpeg_skip_resume_while_speaking_ = true;
    uint32_t mjpeg_speaking_fps_ = 5;  // unused while speaking skip=true
    /** Avoid deferred preload while wake armed (audio_detection stack overflow). */
    bool emotion_skip_preload_while_wake_ = true;
    int64_t emotion_preload_arm_since_us_ = 0;
    int64_t mjpeg_resume_since_us_ = 0;
    int mjpeg_resume_defer_sec_ = CONFIG_REMINDER_BOOT_DEFER_SEC;
    int preload_after_net_stagger_sec_ = 3;
    /** When cloud omits type=llm, apply STT keyword face hint at speaking (still-only). */
    std::string stt_face_hint_;
#endif
};


class TaskPriorityReset {
public:
    TaskPriorityReset(BaseType_t priority) {
        original_priority_ = uxTaskPriorityGet(NULL);
        vTaskPrioritySet(NULL, priority);
    }
    ~TaskPriorityReset() {
        vTaskPrioritySet(NULL, original_priority_);
    }

private:
    BaseType_t original_priority_;
};

#endif // _APPLICATION_H_
