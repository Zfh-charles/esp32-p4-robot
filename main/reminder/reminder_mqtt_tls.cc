#include "reminder_mqtt_tls.h"

#include <esp_log.h>
#include <string>

#define TAG "ReminderMqttTls"

#if CONFIG_USE_REMINDER_POLL && !CONFIG_REMINDER_MQTT_TLS_INSECURE

extern "C" {
extern const char mqtt_ca_pem_start[] asm("_binary_mqtt_ca_pem_start");
extern const char mqtt_ca_pem_end[] asm("_binary_mqtt_ca_pem_end");
}

static const char kCaFilename[] = "mqtt_ca.pem";
static constexpr int kReminderSslContextId = 1;
static bool s_ca_installed = false;

bool ReminderTlsResetDefaultSslContext(std::shared_ptr<AtUart> at_uart) {
    if (!at_uart) {
        return false;
    }
    // Context 0: OTA / xiaozhi cloud HTTPS (no custom CA, skip server verify)
    return at_uart->SendCommand("AT+MSSLCFG=\"auth\",0,0");
}

bool ReminderMqttInstallCaCert(std::shared_ptr<AtUart> at_uart) {
    if (s_ca_installed) {
        return true;
    }
    if (!at_uart) {
        ESP_LOGE(TAG, "AtUart unavailable");
        return false;
    }

    if (!ReminderTlsResetDefaultSslContext(at_uart)) {
        ESP_LOGW(TAG, "Failed to reset SSL context 0 (OTA HTTPS)");
    }

    const char* cert_start = mqtt_ca_pem_start;
    const size_t cert_len = static_cast<size_t>(mqtt_ca_pem_end - cert_start);
    if (cert_len == 0) {
        ESP_LOGE(TAG, "Embedded CA cert empty");
        return false;
    }

    const std::string write_cmd = std::string("AT+MSSLCERTWR=\"") + kCaFilename + "\",0," + std::to_string(cert_len);
    if (!at_uart->SendCommandWithData(write_cmd, 10000, true, cert_start, cert_len)) {
        ESP_LOGE(TAG, "MSSLCERTWR failed: %s", at_uart->GetResponse().c_str());
        return false;
    }

    const std::string bind_cmd = std::string("AT+MSSLCFG=\"cert\",") + std::to_string(kReminderSslContextId) +
                                ",\"" + kCaFilename + "\"";
    if (!at_uart->SendCommand(bind_cmd)) {
        ESP_LOGE(TAG, "MSSLCFG cert failed: %s", at_uart->GetResponse().c_str());
        return false;
    }

    const std::string auth_cmd = std::string("AT+MSSLCFG=\"auth\",") + std::to_string(kReminderSslContextId) + ",1";
    if (!at_uart->SendCommand(auth_cmd)) {
        ESP_LOGE(TAG, "MSSLCFG auth failed: %s", at_uart->GetResponse().c_str());
        return false;
    }

    s_ca_installed = true;
    ESP_LOGI(TAG, "Reminder CA on SSL ctx %d (%u bytes)", kReminderSslContextId, static_cast<unsigned>(cert_len));
    return true;
}

#endif
