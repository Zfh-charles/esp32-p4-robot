#pragma once

#include "audio_service.h"
#include "device_state.h"

#include <esp_log.h>

/** Unified diagnostic tag — serial filter: ReminderTrace */
#define REMINDER_TRACE_TAG "ReminderTrace"

#define REMINDER_TRACE_LOG(fmt, ...) ESP_LOGI(REMINDER_TRACE_TAG, fmt, ##__VA_ARGS__)

inline const char* ReminderTraceAudioRoute(AudioRoute route) {
    switch (route) {
        case AudioRoute::Capture:
            return "Capture";
        case AudioRoute::Playback:
            return "Playback";
        case AudioRoute::Duplex:
            return "Duplex";
        default:
            return "?";
    }
}

inline const char* ReminderTraceSessionKindName(int kind) {
    switch (kind) {
        case 0:
            return "None";
        case 1:
            return "User";
        case 2:
            return "Proactive";
        default:
            return "?";
    }
}

inline const char* ReminderTraceDeviceStateName(DeviceState state) {
    switch (state) {
        case kDeviceStateUnknown:
            return "unknown";
        case kDeviceStateStarting:
            return "starting";
        case kDeviceStateWifiConfiguring:
            return "configuring";
        case kDeviceStateIdle:
            return "idle";
        case kDeviceStateConnecting:
            return "connecting";
        case kDeviceStateListening:
            return "listening";
        case kDeviceStateSpeaking:
            return "speaking";
        case kDeviceStateUpgrading:
            return "upgrading";
        case kDeviceStateActivating:
            return "activating";
        case kDeviceStateAudioTesting:
            return "audio_testing";
#if CONFIG_USE_ALARM
        case kDeviceStateAlarm:
            return "alarm";
#endif
        case kDeviceStateFatalError:
            return "fatal_error";
        default:
            return "invalid_state";
    }
}
