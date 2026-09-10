#include "reminder_mqtt_wake.h"

#include "reminder_poller.h"
#include "reminder_mqtt_tls.h"
#include "reminder/boot_trace.h"
#include "board.h"
#include "settings.h"
#include "system_info.h"
#include "debug/afe_fetch_gate.h"
#include "mqtt.h"
#include <at_modem.h>

#include <esp_log.h>
#include <cstring>
#include <cstdio>
#include <cctype>
#include <mbedtls/md.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <memory>

#define TAG "ReminderMqtt"

#if CONFIG_USE_REMINDER_POLL && CONFIG_REMINDER_MQTT_WAKE

namespace {

constexpr uint32_t kFailureBackoffMs[] = {30000, 60000, 120000, 300000};
constexpr uint32_t kFailureJitterMaxMs = 15000;
constexpr uint32_t kDelaySliceMs = 1000;

class MspiNetworkScope {
public:
    MspiNetworkScope() : held_(MspiBudgetGateEnterNetwork()) {}
    ~MspiNetworkScope() {
        if (held_) {
            MspiBudgetGateLeaveNetwork();
        }
    }
    MspiNetworkScope(const MspiNetworkScope&) = delete;
    MspiNetworkScope& operator=(const MspiNetworkScope&) = delete;

private:
    bool held_;
};

void DisconnectWithIdleVisualQuiet(Mqtt* mqtt) {
    if (mqtt == nullptr) {
        return;
    }
    MspiNetworkScope quiet_idle_visual;
    mqtt->Disconnect();
}

}  // namespace

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

std::string ReminderMqttWake::ExpandDeviceIdInTopic(const std::string& topic_template) {
    std::string topic = topic_template;
    const std::string mac = SystemInfo::GetMacAddress();
    std::string mac_clean = mac;
    ReplaceAll(mac_clean, ":", "");
    ReplaceAll(topic, "{device_id}", mac);
    ReplaceAll(topic, "{mac}", mac);
    ReplaceAll(topic, "{mac_clean}", mac_clean);
    return topic;
}

#if CONFIG_REMINDER_MQTT_WAKE_DERIVE_PASSWORD
static bool HexDecode(const char* hex, unsigned char* out, size_t out_len) {
    if (hex == nullptr || (strlen(hex) % 2) != 0) {
        return false;
    }
    const size_t hex_len = strlen(hex);
    if (hex_len / 2 != out_len) {
        return false;
    }
    for (size_t i = 0; i < out_len; ++i) {
        char byte_str[3] = {hex[i * 2], hex[i * 2 + 1], '\0'};
        for (int j = 0; j < 2; ++j) {
            if (!std::isxdigit(static_cast<unsigned char>(byte_str[j]))) {
                return false;
            }
        }
        out[i] = static_cast<unsigned char>(strtoul(byte_str, nullptr, 16));
    }
    return true;
}

static std::string DeriveMqttPassword() {
    std::string mac_clean = SystemInfo::GetMacAddress();
    ReplaceAll(mac_clean, ":", "");
    const char* salt_hex = CONFIG_REMINDER_MQTT_WAKE_SECRET_SALT;
    const mbedtls_md_info_t* info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (info == nullptr || salt_hex == nullptr || salt_hex[0] == '\0') {
        return std::string();
    }
    unsigned char salt_key[32];
    if (!HexDecode(salt_hex, salt_key, sizeof(salt_key))) {
        ESP_LOGE(TAG, "Invalid MQTT password salt (expect 64 hex chars)");
        return std::string();
    }
    unsigned char out[32];
    if (mbedtls_md_hmac(info, salt_key, sizeof(salt_key),
                        reinterpret_cast<const unsigned char*>(mac_clean.data()), mac_clean.size(),
                        out) != 0) {
        return std::string();
    }
    char hex[33];
    for (int i = 0; i < 16; ++i) {
        snprintf(hex + i * 2, 3, "%02x", out[i]);
    }
    return std::string(hex, 32);
}
#endif

void ReminderMqttWake::EnsureNvsConfigured() {
    Settings settings("reminder_mqtt", true);
    std::string nvs_broker = settings.GetString("broker");
    bool stale_broker = !nvs_broker.empty() &&
        (nvs_broker.find("114.245.178.62") != std::string::npos ||
         nvs_broker.find("114.245.176.144") != std::string::npos ||
         nvs_broker.find("broker.emqx.io") != std::string::npos);
    if (!stale_broker && !nvs_broker.empty()) {
        return;
    }

#ifdef CONFIG_REMINDER_MQTT_WAKE_DEFAULT_BROKER
    std::string broker = CONFIG_REMINDER_MQTT_WAKE_DEFAULT_BROKER;
#else
    std::string broker;
#endif
    if (broker.empty()) {
        ESP_LOGW(TAG, "MQTT wake broker not configured");
        return;
    }

    if (!nvs_broker.empty() && stale_broker) {
        ESP_LOGW(TAG, "Migrated stale MQTT wake broker to menuconfig default");
    }
    settings.SetString("broker", broker);
#ifdef CONFIG_REMINDER_MQTT_WAKE_DEFAULT_TOPIC
    std::string topic = CONFIG_REMINDER_MQTT_WAKE_DEFAULT_TOPIC;
    if (!topic.empty()) {
        settings.SetString("topic", topic);
    }
#endif
#ifdef CONFIG_REMINDER_MQTT_WAKE_DEFAULT_USERNAME
    std::string username = CONFIG_REMINDER_MQTT_WAKE_DEFAULT_USERNAME;
    if (!username.empty()) {
        settings.SetString("username", username);
    }
#endif
#if defined(CONFIG_REMINDER_MQTT_WAKE_DEFAULT_PASSWORD) && !CONFIG_REMINDER_MQTT_WAKE_DERIVE_PASSWORD
    std::string password = CONFIG_REMINDER_MQTT_WAKE_DEFAULT_PASSWORD;
    if (!password.empty()) {
        settings.SetString("password", password);
    }
#endif
#ifdef CONFIG_REMINDER_MQTT_WAKE_DEFAULT_CLIENT_ID
    std::string client_id = CONFIG_REMINDER_MQTT_WAKE_DEFAULT_CLIENT_ID;
    if (!client_id.empty()) {
        settings.SetString("client_id", client_id);
    }
#endif
    ESP_LOGI(TAG, "Wrote default MQTT wake broker to NVS (device=%s)", SystemInfo::GetMacAddress().c_str());
}

ReminderMqttWake::ReminderMqttWake() = default;

ReminderMqttWake::~ReminderMqttWake() {
    Stop();
}

void ReminderMqttWake::Start(ReminderPoller* poller) {
    if (running_ || poller == nullptr) {
        return;
    }
    EnsureNvsConfigured();

    Settings settings("reminder_mqtt", false);
    if (settings.GetString("broker").empty()) {
        ESP_LOGW(TAG, "MQTT wake disabled: broker not set in NVS reminder_mqtt.broker");
        return;
    }

    poller_ = poller;
    running_ = true;
    xTaskCreate(
        [](void* arg) {
            static_cast<ReminderMqttWake*>(arg)->MqttTask();
        },
        "reminder_mqtt", 8192, this, 2, &task_handle_);
    ESP_LOGI(TAG, "MQTT wake listener started, mac=%s", SystemInfo::GetMacAddress().c_str());
}

void ReminderMqttWake::Stop() {
    // s1aw: 只置 running_=false，禁止在此立刻 mqtt_.reset()。
    // 历史竞态：Stop 与 ConnectOnce/Subscribe 并发销毁 → Core1 LoadProhibited
    // (xQueueSemaphoreTake←pthread_mutex←AtUart::SendCommandWithData)。
    running_ = false;
    if (task_handle_ != nullptr) {
        for (int i = 0; i < 100 && task_handle_ != nullptr; ++i) {
            vTaskDelay(pdMS_TO_TICKS(50));
        }
    }
    if (mqtt_) {
        DisconnectWithIdleVisualQuiet(mqtt_.get());
        vTaskDelay(pdMS_TO_TICKS(50));
        mqtt_.reset();
    }
    poller_ = nullptr;
}

bool ReminderMqttWake::ConnectOnce() {
    if (!running_) {
        return false;
    }

    BootTraceMark("MQTT_BEG", "connect");

    Settings settings("reminder_mqtt", false);
    const std::string broker_endpoint = settings.GetString("broker");
    const std::string topic_template = settings.GetString("topic", "xiaozhi/reminder/wake/{device_id}");
    const std::string username = ExpandDeviceIdInTopic(settings.GetString("username"));
    std::string password = settings.GetString("password");
#if CONFIG_REMINDER_MQTT_WAKE_DERIVE_PASSWORD
    password = DeriveMqttPassword();
    if (password.empty()) {
        ESP_LOGE(TAG, "Derived MQTT password empty (check salt config)");
    }
#endif

    if (broker_endpoint.empty()) {
        BootTraceMark("MQTT_END", "no_broker");
        return false;
    }

    std::string broker_address;
    int broker_port = 1883;
    const size_t pos = broker_endpoint.find(':');
    if (pos != std::string::npos) {
        broker_address = broker_endpoint.substr(0, pos);
        broker_port = std::stoi(broker_endpoint.substr(pos + 1));
    } else {
        broker_address = broker_endpoint;
    }

    const std::string subscribe_topic = ExpandDeviceIdInTopic(topic_template);
    std::string client_id = ExpandDeviceIdInTopic(settings.GetString("client_id"));
    if (client_id.empty()) {
        client_id = "xiaozhi-" + SystemInfo::GetMacAddress();
        ReplaceAll(client_id, ":", "");
    }

    auto network = Board::GetInstance().GetNetwork();
#if !CONFIG_REMINDER_MQTT_TLS_INSECURE
    if (broker_port == 8883) {
        auto* at_modem = dynamic_cast<AtModem*>(network);
        if (at_modem == nullptr || !ReminderMqttInstallCaCert(at_modem->GetAtUart())) {
            ESP_LOGE(TAG, "MQTT CA cert install failed");
            BootTraceMark("MQTT_END", "ca_fail");
            return false;
        }
    }
#endif

    // 旧连接残留：本任务内先安全释放
    if (mqtt_) {
        DisconnectWithIdleVisualQuiet(mqtt_.get());
        vTaskDelay(pdMS_TO_TICKS(50));
        mqtt_.reset();
    }

    if (!running_) {
        BootTraceMark("MQTT_END", "stop_pre");
        return false;
    }

    mqtt_ = network->CreateMqtt(1);
    if (!mqtt_) {
        ESP_LOGE(TAG, "Failed to create MQTT client (index 1)");
        BootTraceMark("MQTT_END", "create_fail");
        return false;
    }

    ReminderPoller* poller = poller_;
    mqtt_->SetKeepAlive(120);
    mqtt_->OnMessage([this, poller](const std::string& topic, const std::string& payload) {
        ESP_LOGI(TAG, "Wake message [%s]: %s", topic.c_str(), payload.c_str());
        if (running_ && poller != nullptr) {
            poller->TriggerPoll();
        }
    });

    ESP_LOGI(TAG, "Connecting MQTT wake broker %s:%d topic=%s",
             broker_address.c_str(), broker_port, subscribe_topic.c_str());
    if (!running_) {
        mqtt_.reset();
        BootTraceMark("MQTT_END", "stop_pre_conn");
        return false;
    }
    BootTraceMark("MQTT_CONN_BEG", "at");
    bool connected = false;
    {
        MspiNetworkScope quiet_idle_visual;
        connected = mqtt_->Connect(broker_address, broker_port, client_id, username, password);
    }
    if (!connected) {
        ESP_LOGW(TAG, "MQTT wake connect failed");
        mqtt_.reset();
        BootTraceMark("MQTT_END", "conn_fail");
        return false;
    }
    BootTraceMark("MQTT_CONN_OK", "at");

    if (!running_) {
        DisconnectWithIdleVisualQuiet(mqtt_.get());
        vTaskDelay(pdMS_TO_TICKS(50));
        mqtt_.reset();
        BootTraceMark("MQTT_END", "stop_post");
        return false;
    }

    BootTraceMark("MQTT_SUB_BEG", "at");
    bool subscribed = false;
    {
        MspiNetworkScope quiet_idle_visual;
        subscribed = mqtt_->Subscribe(subscribe_topic);
    }
    if (!subscribed) {
        ESP_LOGW(TAG, "MQTT wake subscribe failed: %s", subscribe_topic.c_str());
        DisconnectWithIdleVisualQuiet(mqtt_.get());
        vTaskDelay(pdMS_TO_TICKS(50));
        mqtt_.reset();
        BootTraceMark("MQTT_END", "sub_fail");
        return false;
    }
    BootTraceMark("MQTT_SUB_OK", "at");

    ESP_LOGI(TAG, "MQTT wake subscribed: %s", subscribe_topic.c_str());
    BootTraceMark("MQTT_OK", subscribe_topic.c_str());

    // s1ay: IsConnected() 每次发 AT+MQTTSTATE（占 UART）。
    // 相位：MQTT_STATE_BEG→…→MQTT_STATE_OK；若崩在 BEG=卡在 AT；若 HTTP_POLL_*=卡在 poll。
    int wait_ticks = 0;
    BootTraceMark("MQTT_WAIT", "alive");
    while (running_ && mqtt_) {
        BootTraceMark("MQTT_STATE_BEG", "chk");
        bool connected = false;
        {
            MspiNetworkScope quiet_idle_visual;
            connected = mqtt_->IsConnected();
        }
        BootTraceMark("MQTT_STATE_OK", connected ? "1" : "0");
        if (!connected) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(5000));
        if (++wait_ticks % 12 == 0) { // ~60s
            BootTraceMark("MQTT_WAIT", "alive");
        }
    }

    if (mqtt_) {
        BootTraceMark("MQTT_DISC_BEG", "disc");
        DisconnectWithIdleVisualQuiet(mqtt_.get());
        vTaskDelay(pdMS_TO_TICKS(50)); // 给 UART URC 回调退场
        mqtt_.reset();
        BootTraceMark("MQTT_DISC_END", "ok");
    }
    BootTraceMark("MQTT_END", running_ ? "disc" : "stop");
    return true;
}

void ReminderMqttWake::MqttTask() {
    ESP_LOGI(TAG, "task start");
    BootTraceMark("MQTT_TASK", "start");

    // Failed connections use jittered exponential backoff so a dead reminder
    // endpoint cannot create a permanent 40-second hammer. The first MQTT
    // connection remains immediate so proactive reminders are ready at boot.
    auto delay_while_running = [this](uint32_t delay_ms) {
        while (running_ && delay_ms > 0) {
            const uint32_t slice_ms = delay_ms < kDelaySliceMs ? delay_ms : kDelaySliceMs;
            vTaskDelay(pdMS_TO_TICKS(slice_ms));
            delay_ms -= slice_ms;
        }
    };

    uint32_t failure_streak = 0;
    while (running_) {
        if (!ConnectOnce()) {
            if (failure_streak < UINT32_MAX) {
                ++failure_streak;
            }
            const size_t last = (sizeof(kFailureBackoffMs) / sizeof(kFailureBackoffMs[0])) - 1;
            const size_t index = failure_streak - 1 < last ? failure_streak - 1 : last;
            const uint32_t base_ms = kFailureBackoffMs[index];
            const uint32_t jitter_seed = static_cast<uint32_t>(xTaskGetTickCount()) ^
                                         (failure_streak * 2654435761U);
            const uint32_t jitter_ms = jitter_seed % (kFailureJitterMaxMs + 1U);
            const uint32_t delay_ms = base_ms + jitter_ms;
            char detail[32];
            snprintf(detail, sizeof(detail), "n=%lu wait=%lu",
                     static_cast<unsigned long>(failure_streak),
                     static_cast<unsigned long>(delay_ms));
            ESP_LOGW(TAG,
                     "MQTT_FAIL_BACKOFF streak=%u base_ms=%u jitter_ms=%u delay_ms=%u s1fj",
                     static_cast<unsigned>(failure_streak), static_cast<unsigned>(base_ms),
                     static_cast<unsigned>(jitter_ms), static_cast<unsigned>(delay_ms));
            BootTraceMark("MQTT_BACKOFF", detail);
            delay_while_running(delay_ms);
        } else {
            if (failure_streak > 0) {
                ESP_LOGI(TAG, "MQTT_FAIL_BACKOFF recovered previous_streak=%u s1fj",
                         static_cast<unsigned>(failure_streak));
            }
            failure_streak = 0;
            delay_while_running(10000);
        }
    }
    ESP_LOGI(TAG, "task exit");
    BootTraceMark("MQTT_TASK", "exit");
    task_handle_ = nullptr;
    vTaskDelete(nullptr);
}

#endif  // CONFIG_USE_REMINDER_POLL && CONFIG_REMINDER_MQTT_WAKE
