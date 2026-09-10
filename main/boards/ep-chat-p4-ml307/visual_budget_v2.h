#pragma once

#include <stddef.h>
#include <stdint.h>
#include <atomic>

#include "device_state.h"

enum class VisualBudgetLevel : uint8_t {
    Frozen = 0,
    StaticOnly,
    MouthReduced,
    MouthOnly,
    TransitionCandidate,
};

struct VisualBudgetSample {
    DeviceState device_state = kDeviceStateUnknown;
    size_t decode_queue = 0;
    size_t playback_queue = 0;
    size_t send_queue = 0;
    uint32_t output_age_ms = 0;
    bool playback_hold = false;
};

struct VisualBudgetDecision {
    VisualBudgetLevel level = VisualBudgetLevel::Frozen;
    const char* reason = "unknown";
};

inline VisualBudgetDecision VisualBudgetV2_ClassifyShadow(const VisualBudgetSample& sample) {
    const size_t downlink_depth = sample.decode_queue + sample.playback_queue;

    if (sample.playback_hold) {
        return {VisualBudgetLevel::Frozen, "playback_hold"};
    }

    switch (sample.device_state) {
        case kDeviceStateSpeaking:
            if (downlink_depth >= 3) {
                return {VisualBudgetLevel::MouthOnly, "speaking_headroom"};
            }
            // Speaking starts before the first PCM frame is queued. Keep a
            // reduced provisional budget so first-mouth latency stays bounded.
            return {VisualBudgetLevel::MouthReduced,
                    downlink_depth >= 1 || sample.output_age_ms <= 160
                        ? "speaking_thin"
                        : "speaking_provisional"};
        case kDeviceStateListening:
            return {VisualBudgetLevel::StaticOnly, "listening"};
        case kDeviceStateIdle:
            if (downlink_depth == 0 && sample.output_age_ms >= 700) {
                // Candidate only. The renderer must still pass AFE, MQTT,
                // fullscreen-singleton and per-tick burst gates.
                return {VisualBudgetLevel::TransitionCandidate, "idle_quiet"};
            }
            return {VisualBudgetLevel::StaticOnly, "idle_settle"};
        default:
            return {VisualBudgetLevel::Frozen, "state_guard"};
    }
}

inline std::atomic<uint8_t>& VisualBudgetV2_PublishedStorage() {
    static std::atomic<uint8_t> level{static_cast<uint8_t>(VisualBudgetLevel::Frozen)};
    return level;
}

inline void VisualBudgetV2_Publish(VisualBudgetLevel level) {
    VisualBudgetV2_PublishedStorage().store(static_cast<uint8_t>(level),
                                            std::memory_order_release);
}

inline VisualBudgetLevel VisualBudgetV2_Current() {
    return static_cast<VisualBudgetLevel>(
        VisualBudgetV2_PublishedStorage().load(std::memory_order_acquire));
}

inline const char* VisualBudgetV2_LevelName(VisualBudgetLevel level) {
    switch (level) {
        case VisualBudgetLevel::Frozen: return "Frozen";
        case VisualBudgetLevel::StaticOnly: return "StaticOnly";
        case VisualBudgetLevel::MouthReduced: return "MouthReduced";
        case VisualBudgetLevel::MouthOnly: return "MouthOnly";
        case VisualBudgetLevel::TransitionCandidate: return "TransitionCandidate";
        default: return "Unknown";
    }
}
