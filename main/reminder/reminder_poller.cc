#include "reminder_poller.h"

#include "application.h"
#include "board.h"
#include "device_state.h"
#include "reminder/boot_trace.h"
#include "reminder/reminder_trace.h"
#include "reminder/reminder_lifecycle_trace.h"
#include "settings.h"
#include "system_info.h"
#include "assets/lang_config.h"
#include "http.h"

#include <esp_log.h>
#include <cstdio>
#include <cstring>
#include <cJSON.h>
#include <esp_timer.h>
#include <freertos/task.h>

#define TAG "ReminderPoll"

#if CONFIG_USE_REMINDER_POLL

static void ReplaceAll(std::string& str, const std::string& from, const std::string& to) {
    if (from.empty()) {
        return;
    }
    size_t pos = 0;
    while ((pos = str.find(from, pos)) != std::string::npos) {
        str.replace(pos, from.length(), to);
        pos += to.length();
    }
}

std::string ReminderPoller::ExpandDeviceIdInUrl(const std::string& url_template) {
    std::string url = url_template;
    const std::string mac = SystemInfo::GetMacAddress();
    std::string mac_clean = mac;
    ReplaceAll(mac_clean, ":", "");
    ReplaceAll(url, "{device_id}", mac);
    ReplaceAll(url, "{mac}", mac);
    ReplaceAll(url, "{mac_clean}", mac_clean);
    return url;
}

void ReminderPoller::EnsureNvsConfigured() {
    Settings settings("reminder_poll", true);
    std::string poll_url = settings.GetString("poll_url");
    if (!poll_url.empty() &&
        (poll_url.find(":8444") != std::string::npos ||
         poll_url.find("114.245.178.62") != std::string::npos ||
         poll_url.find("114.245.176.144") != std::string::npos)) {
#ifdef CONFIG_REMINDER_POLL_DEFAULT_URL
        settings.SetString("poll_url", CONFIG_REMINDER_POLL_DEFAULT_URL);
        ESP_LOGW(TAG, "Migrated stale poll_url (old host/port) to menuconfig default");
#else
        return;
#endif
    } else if (!poll_url.empty()) {
        return;
    }

#ifdef CONFIG_REMINDER_POLL_DEFAULT_URL
    std::string default_url = CONFIG_REMINDER_POLL_DEFAULT_URL;
#else
    std::string default_url;
#endif
    if (default_url.empty()) {
        ESP_LOGW(TAG, "poll_url not set; configure NVS reminder_poll.poll_url or menuconfig default URL");
        return;
    }

    settings.SetString("poll_url", default_url);
#ifdef CONFIG_REMINDER_POLL_DEFAULT_ACK_URL
    std::string default_ack = CONFIG_REMINDER_POLL_DEFAULT_ACK_URL;
    if (!default_ack.empty()) {
        settings.SetString("ack_url", default_ack);
    }
#endif
    ESP_LOGI(TAG, "Wrote default poll_url to NVS (device_id=%s)", SystemInfo::GetMacAddress().c_str());
}

static ReminderDeliveryMode ParseDeliveryMode(cJSON* root) {
    bool speak = true;
    auto speak_item = cJSON_GetObjectItem(root, "speak");
    if (cJSON_IsBool(speak_item)) {
        speak = cJSON_IsTrue(speak_item);
    }
    if (!speak) {
        return ReminderDeliveryMode::kAlertOnly;
    }

    auto mode_item = cJSON_GetObjectItem(root, "delivery_mode");
    if (!cJSON_IsString(mode_item)) {
        return ReminderDeliveryMode::kMcpWake;
    }
    const char* mode = mode_item->valuestring;
    if (strcmp(mode, "direct_wake") == 0) {
        return ReminderDeliveryMode::kDirectWake;
    }
    if (strcmp(mode, "alert_only") == 0) {
        return ReminderDeliveryMode::kAlertOnly;
    }
    return ReminderDeliveryMode::kMcpWake;
}

bool ReminderPoller::ParsePendingResponse(const std::string& body, std::string& id, std::string& prompt,
                                          std::string& emotion, ReminderDeliveryMode& mode,
                                          std::string& wake_text) {
    cJSON* root = cJSON_Parse(body.c_str());
    if (root == nullptr) {
        return false;
    }
    auto has = cJSON_GetObjectItem(root, "has_reminder");
    if (!cJSON_IsTrue(has)) {
        cJSON_Delete(root);
        return false;
    }
    auto id_item = cJSON_GetObjectItem(root, "id");
    auto prompt_item = cJSON_GetObjectItem(root, "prompt");
    if (!cJSON_IsString(id_item) || !cJSON_IsString(prompt_item)) {
        cJSON_Delete(root);
        return false;
    }
    id = id_item->valuestring;
    prompt = prompt_item->valuestring;
    emotion = "neutral";
    auto emotion_item = cJSON_GetObjectItem(root, "emotion");
    if (cJSON_IsString(emotion_item)) {
        emotion = emotion_item->valuestring;
    }
    mode = ParseDeliveryMode(root);
    wake_text.clear();
    auto wake_item = cJSON_GetObjectItem(root, "wake_text");
    if (cJSON_IsString(wake_item)) {
        wake_text = wake_item->valuestring;
    }
    cJSON_Delete(root);
    return true;
}

static void ApplyReminderHttpHeaders(const std::unique_ptr<Http>& http) {
    http->SetHeader("Device-Id", SystemInfo::GetMacAddress().c_str());
    http->SetHeader("Accept", "application/json");
    http->SetHeader("User-Agent", "xiaozhi-reminder-poller/1.0");
    http->SetHeader("ngrok-skip-browser-warning", "69420");
}

bool ReminderPoller::PostAck(const std::string& ack_url, const std::string& id) {
    if (ack_url.empty()) {
        REMINDER_TRACE_LOG("ack_skip | no_url id=%s", id.c_str());
        return false;
    }
    auto network = Board::GetInstance().GetNetwork();
    auto http = network->CreateHttp(4);
    if (!http) {
        REMINDER_TRACE_LOG("ack_fail | http_create id=%s", id.c_str());
        return false;
    }
    http->SetHeader("Content-Type", "application/json");
    ApplyReminderHttpHeaders(http);
    std::string body = "{\"id\":\"" + id + "\"}";
    http->SetContent(std::move(body));
    BootTraceMark("HTTP_ACK_BEG", "post");
    if (http->Open("POST", ack_url)) {
        const int status = http->GetStatusCode();
        if (status == 200) {
            ESP_LOGI(TAG, "Ack posted for %s", id.c_str());
            REMINDER_TRACE_LOG("ack_ok | id=%s", id.c_str());
            http->Close();
            BootTraceMark("HTTP_ACK_END", "ok");
            return true;
        }
        ESP_LOGW(TAG, "Ack POST status %d for %s", status, id.c_str());
        REMINDER_TRACE_LOG("ack_http_fail | id=%s status=%d", id.c_str(), status);
        http->Close();
        BootTraceMark("HTTP_ACK_END", "status_fail");
        return false;
    }
    REMINDER_TRACE_LOG("ack_open_fail | id=%s", id.c_str());
    BootTraceMark("HTTP_ACK_END", "open_fail");
    return false;
}

ReminderPoller::ReminderPoller() = default;

ReminderPoller::~ReminderPoller() {
    Stop();
}

void ReminderPoller::Start() {
    if (running_) {
        return;
    }
    EnsureNvsConfigured();

    Settings settings("reminder_poll", false);
    if (settings.GetString("poll_url").empty()) {
        ESP_LOGW(TAG, "poll_url not set in NVS namespace reminder_poll, poller disabled");
        return;
    }
    running_ = true;
    xTaskCreate(
        [](void* arg) {
            static_cast<ReminderPoller*>(arg)->PollTask();
        },
        "reminder_poll", 8192, this, 2, &poll_task_handle_);
    ESP_LOGI(TAG, "Reminder poller started, interval %ds, mac=%s",
             CONFIG_REMINDER_POLL_INTERVAL_SEC, SystemInfo::GetMacAddress().c_str());
}

void ReminderPoller::Stop() {
    running_ = false;
    if (poll_task_handle_ != nullptr) {
        xTaskNotifyGive(poll_task_handle_);
    }
}

void ReminderPoller::TriggerPoll() {
#if CONFIG_REMINDER_DEFER_BUSY_WAKE
    DeferPoll("mqtt");
#else
    if (poll_task_handle_ != nullptr) {
        ESP_LOGI(TAG, "MQTT wake -> trigger HTTP poll");
        xTaskNotifyGive(poll_task_handle_);
    }
#endif
}

#if CONFIG_REMINDER_DEFER_BUSY_WAKE
void ReminderPoller::DeferPoll(const char* reason) {
    const bool was_pending = deferred_poll_pending_.exchange(true);
    if (!was_pending) {
        deferred_wait_logged_.store(false);
        REMINDER_TRACE_LOG("poll_deferred_set | reason=%s", reason ? reason : "unknown");
    }
    if (poll_task_handle_ != nullptr) {
        xTaskNotifyGive(poll_task_handle_);
    }
}

void ReminderPoller::TracePollBlocked(const char* reason) {
    if (deferred_poll_pending_.load()) {
        if (!deferred_wait_logged_.exchange(true)) {
            REMINDER_TRACE_LOG("poll_deferred_wait | reason=%s", reason ? reason : "unknown");
        }
    } else {
        REMINDER_TRACE_LOG("poll_skip | reason=%s", reason ? reason : "unknown");
    }
}
#endif

void ReminderPoller::DoPollOnce() {
    auto& app = Application::GetInstance();
#if CONFIG_USE_ALARM
    if (app.GetDeviceState() == kDeviceStateAlarm || app.IsAlarmRinging()) {
#if CONFIG_REMINDER_DEFER_BUSY_WAKE
        TracePollBlocked("alarm");
#else
        REMINDER_TRACE_LOG("poll_skip | reason=alarm");
#endif
        ESP_LOGD(TAG, "Skip poll, local alarm ringing");
        return;
    }
#endif
    if (app.GetDeviceState() != kDeviceStateIdle) {
#if CONFIG_REMINDER_DEFER_BUSY_WAKE
        char reason[48];
        snprintf(reason, sizeof(reason), "state_%s",
                 ReminderTraceDeviceStateName(app.GetDeviceState()));
        TracePollBlocked(reason);
#else
        REMINDER_TRACE_LOG("poll_skip | reason=state_%s", ReminderTraceDeviceStateName(app.GetDeviceState()));
#endif
        ESP_LOGD(TAG, "Skip poll, device state not idle");
        return;
    }
    if (app.GetSessionKind() != SessionKind::None) {
#if CONFIG_REMINDER_DEFER_BUSY_WAKE
        char reason[48];
        snprintf(reason, sizeof(reason), "session_%s",
                 ReminderTraceSessionKindName(static_cast<int>(app.GetSessionKind())));
        TracePollBlocked(reason);
#else
        REMINDER_TRACE_LOG("poll_skip | reason=session_%s",
                           ReminderTraceSessionKindName(static_cast<int>(app.GetSessionKind())));
#endif
        ESP_LOGI(TAG, "Skip poll, session active (kind=%d)", (int)app.GetSessionKind());
        return;
    }

#if CONFIG_REMINDER_DEFER_BUSY_WAKE
    const bool was_deferred = deferred_poll_pending_.exchange(false);
    deferred_wait_logged_.store(false);
    if (was_deferred) {
        REMINDER_TRACE_LOG("poll_deferred_begin");
    }
#endif

    Settings settings("reminder_poll", false);
    std::string poll_url = ExpandDeviceIdInUrl(settings.GetString("poll_url"));
    if (poll_url.empty()) {
        REMINDER_TRACE_LOG("poll_skip | reason=no_poll_url");
        return;
    }

    REMINDER_TRACE_LOG("poll_begin | url=%s", poll_url.c_str());
    // s1ay: 只诊断。崩时 prev_phase 落在 HTTP_* / MQTT_STATE_* 才能判 UART 争用。
    BootTraceMark("HTTP_POLL_BEG", "get");
    auto network = Board::GetInstance().GetNetwork();
    auto http = network->CreateHttp(4);
    if (!http) {
        REMINDER_TRACE_LOG("poll_fail | reason=http_create");
        ESP_LOGW(TAG, "Failed to create HTTP client");
        BootTraceMark("HTTP_POLL_END", "create_fail");
        return;
    }
    http->SetHeader("Content-Type", "application/json");
    ApplyReminderHttpHeaders(http);

    BootTraceMark("HTTP_POLL_OPEN", "beg");
    if (!http->Open("GET", poll_url)) {
        REMINDER_TRACE_LOG("poll_fail | reason=http_open");
        ESP_LOGW(TAG, "HTTP open failed: %s", poll_url.c_str());
        BootTraceMark("HTTP_POLL_END", "open_fail");
        return;
    }
    BootTraceMark("HTTP_POLL_OPEN", "ok");
    const int status = http->GetStatusCode();
    if (status != 200) {
        REMINDER_TRACE_LOG("poll_fail | reason=http_status status=%d", status);
        ESP_LOGW(TAG, "HTTP status %d for %s", status, poll_url.c_str());
        http->Close();
        BootTraceMark("HTTP_POLL_END", "status_fail");
        return;
    }

    BootTraceMark("HTTP_POLL_READ", "beg");
    std::string body = http->ReadAll();
    http->Close();
    BootTraceMark("HTTP_POLL_END", "ok");
    REMINDER_TRACE_LOG("poll_ok | body_len=%u", (unsigned)body.size());

    std::string id, prompt, emotion, wake_text;
    ReminderDeliveryMode mode = ReminderDeliveryMode::kMcpWake;
    if (!ParsePendingResponse(body, id, prompt, emotion, mode, wake_text)) {
        REMINDER_TRACE_LOG("poll_no_reminder");
        return;
    }

    Settings rw_settings("reminder_poll", true);
    const std::string last_id = rw_settings.GetString("last_id");
    if (last_id == id) {
        const int64_t now_sec = esp_timer_get_time() / 1000000LL;
        const int64_t ack_ts = rw_settings.GetInt("last_ack_ts", 0);
        constexpr int64_t kAckDedupeSec = 300;
        if (ack_ts > 0 && (now_sec - ack_ts) < kAckDedupeSec) {
            REMINDER_TRACE_LOG("poll_skip | reason=already_acked id=%s age=%ds",
                               id.c_str(), (int)(now_sec - ack_ts));
            ESP_LOGI(TAG, "Reminder %s already acked %ds ago, skip", id.c_str(),
                     (int)(now_sec - ack_ts));
            return;
        }
    }

    const char* mode_name = "mcp_wake";
    if (mode == ReminderDeliveryMode::kDirectWake) {
        mode_name = "direct_wake";
    } else if (mode == ReminderDeliveryMode::kAlertOnly) {
        mode_name = "alert_only";
    }
    ESP_LOGI(TAG, "New reminder %s [%s]: %s", id.c_str(), mode_name, prompt.c_str());
    REMINDER_TRACE_LOG("poll_new | id=%s mode=%s", id.c_str(), mode_name);
    ReminderLcMark(ReminderLcOwner::Proactive, ReminderLcPhase::PollNew, 1, id.c_str());

    std::string ack_url = settings.GetString("ack_url");
    if (ack_url.empty()) {
        size_t pos = poll_url.find("/pending");
        if (pos != std::string::npos) {
            ack_url = poll_url.substr(0, pos) + "/ack";
        }
    } else {
        ack_url = ExpandDeviceIdInUrl(ack_url);
    }

    app.Schedule([this, id, prompt, emotion, wake_text, mode, ack_url]() {
        auto& application = Application::GetInstance();
#if CONFIG_USE_ALARM
        if (application.GetDeviceState() == kDeviceStateAlarm || application.IsAlarmRinging()) {
            REMINDER_TRACE_LOG("deliver_skip | reason=alarm id=%s", id.c_str());
            ESP_LOGW(TAG, "Skip delivery for %s, local alarm ringing", id.c_str());
#if CONFIG_REMINDER_DEFER_BUSY_WAKE
            DeferPoll("deliver_alarm");
#endif
            return;
        }
#endif
        if (application.GetDeviceState() != kDeviceStateIdle) {
            REMINDER_TRACE_LOG("deliver_skip | reason=state_%s id=%s",
                               ReminderTraceDeviceStateName(application.GetDeviceState()), id.c_str());
            ESP_LOGW(TAG, "Skip delivery for %s, device no longer idle", id.c_str());
#if CONFIG_REMINDER_DEFER_BUSY_WAKE
            DeferPoll("deliver_state");
#endif
            return;
        }
        if (application.IsProactiveReminderPending()) {
            REMINDER_TRACE_LOG("deliver_skip | reason=proactive_pending id=%s", id.c_str());
            ESP_LOGW(TAG, "Skip delivery for %s, proactive reminder in flight", id.c_str());
#if CONFIG_REMINDER_DEFER_BUSY_WAKE
            DeferPoll("deliver_proactive_pending");
#endif
            return;
        }
        if (application.GetSessionKind() != SessionKind::None) {
            REMINDER_TRACE_LOG("deliver_skip | reason=session_%s id=%s",
                               ReminderTraceSessionKindName(static_cast<int>(application.GetSessionKind())),
                               id.c_str());
            ESP_LOGW(TAG, "Skip delivery for %s, session active", id.c_str());
#if CONFIG_REMINDER_DEFER_BUSY_WAKE
            DeferPoll("deliver_session");
#endif
            return;
        }

        const char* deliver_mode = "mcp_wake";
        if (mode == ReminderDeliveryMode::kDirectWake) {
            deliver_mode = "direct_wake";
        } else if (mode == ReminderDeliveryMode::kAlertOnly) {
            deliver_mode = "alert_only";
        }
        REMINDER_TRACE_LOG("deliver_schedule | id=%s mode=%s", id.c_str(), deliver_mode);
        ReminderLcMark(ReminderLcOwner::Proactive, ReminderLcPhase::DeliverSchedule, 1, id.c_str());

        if (mode == ReminderDeliveryMode::kAlertOnly) {
            application.Alert("提醒", prompt.c_str(), emotion.c_str(), Lang::Sounds::OGG_VIBRATION);
            if (ReminderPoller::PostAck(ack_url, id)) {
                Settings ack_settings("reminder_poll", true);
                ack_settings.SetString("last_id", id);
                ack_settings.SetInt("last_ack_ts", (int32_t)(esp_timer_get_time() / 1000000LL));
            }
            return;
        }

        application.DeliverReminder(mode, id, prompt, wake_text, emotion.c_str(), ack_url);

        if (mode == ReminderDeliveryMode::kDirectWake) {
            if (ReminderPoller::PostAck(ack_url, id)) {
                Settings ack_settings("reminder_poll", true);
                ack_settings.SetString("last_id", id);
                ack_settings.SetInt("last_ack_ts", (int32_t)(esp_timer_get_time() / 1000000LL));
            }
        }
    });
}

void ReminderPoller::PollTask() {
    while (running_) {
        DoPollOnce();
        const uint32_t interval_ms = CONFIG_REMINDER_POLL_INTERVAL_SEC * 1000U;
#if CONFIG_REMINDER_DEFER_BUSY_WAKE
        const uint32_t wait_ms = deferred_poll_pending_.load() ? 1000U : interval_ms;
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(wait_ms));
#else
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(interval_ms));
#endif
    }
    poll_task_handle_ = nullptr;
    vTaskDelete(nullptr);
}

#endif  // CONFIG_USE_REMINDER_POLL
