#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <atomic>
#include <string>

enum class ReminderDeliveryMode {
    kMcpWake,      // idle 主动推流：短 detect + 云端 MCP 读队列 + 延后 ack
    kDirectWake,   // 直接把 prompt 当 detect（仅适合短句）
    kAlertOnly,    // 仅屏幕/振动，不连云 TTS
};

class ReminderPoller {
public:
    ReminderPoller();
    ~ReminderPoller();

    void Start();
    void Stop();

    /** Wake the poll loop immediately (e.g. MQTT push notification). */
    void TriggerPoll();

    static void EnsureNvsConfigured();
    static bool PostAck(const std::string& ack_url, const std::string& id);

private:
    bool running_ = false;
    TaskHandle_t poll_task_handle_ = nullptr;
#if CONFIG_REMINDER_DEFER_BUSY_WAKE
    std::atomic<bool> deferred_poll_pending_{false};
    std::atomic<bool> deferred_wait_logged_{false};
#endif
    void PollTask();
    void DoPollOnce();
#if CONFIG_REMINDER_DEFER_BUSY_WAKE
    void DeferPoll(const char* reason);
    void TracePollBlocked(const char* reason);
#endif
    static std::string ExpandDeviceIdInUrl(const std::string& url_template);
    static bool ParsePendingResponse(const std::string& body, std::string& id, std::string& prompt,
                                     std::string& emotion, ReminderDeliveryMode& mode,
                                     std::string& wake_text);
};
