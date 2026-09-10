#ifndef REMINDER_MQTT_TLS_H
#define REMINDER_MQTT_TLS_H

#include <memory>
#include <at_uart.h>

#if CONFIG_USE_REMINDER_POLL && !CONFIG_REMINDER_MQTT_TLS_INSECURE

bool ReminderTlsResetDefaultSslContext(std::shared_ptr<AtUart> at_uart);
bool ReminderMqttInstallCaCert(std::shared_ptr<AtUart> at_uart);

#else

inline bool ReminderTlsResetDefaultSslContext(std::shared_ptr<AtUart>) {
    return true;
}

inline bool ReminderMqttInstallCaCert(std::shared_ptr<AtUart>) {
    return true;
}

#endif

#endif
