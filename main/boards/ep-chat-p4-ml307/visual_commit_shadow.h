#pragma once

#include <stdint.h>

#include "visual_budget_v2.h"

/**
 * P2 shadow-only bridge from product state to a resource-governed visual commit.
 * It owns no task, queue, lock, heap buffer, DMA object or display handle.
 */
enum class VisualIntentKind : uint8_t {
    None = 0,
    SpeakingMouthBand,
    IdleLifeBand,
};

enum class VisualCommitBackend : uint8_t {
    None = 0,
    LayeredBand,
    PrecomposedBand,
};

struct VisualResourceClaim {
    uint32_t deadline_ms = 0;
    uint32_t psram_burst_bytes = 0;
    uint16_t max_rows = 0;
    bool droppable = true;
};

struct VisualIntent {
    VisualIntentKind kind = VisualIntentKind::None;
    uint32_t sequence = 0;
    VisualResourceClaim claim;
};

struct VisualCommitShadowDecision {
    VisualCommitBackend backend = VisualCommitBackend::None;
    bool admitted = false;
    const char* reason = "no_intent";
};

inline constexpr uint16_t kVisualCommitShadowMaxRows = 48;
inline constexpr uint32_t kVisualCommitShadowBandBurstBytes = 480U * 48U * 2U;
inline constexpr bool kVisualCommitShadowApply = false;
static_assert(!kVisualCommitShadowApply, "P2 must remain shadow-only");

inline VisualIntent VisualCommitShadow_BuildIntent(const VisualBudgetSample& sample,
                                                   const VisualBudgetDecision& budget,
                                                   uint32_t sequence) {
    VisualIntent intent;
    intent.sequence = sequence;

    if (sample.device_state == kDeviceStateSpeaking) {
        intent.kind = VisualIntentKind::SpeakingMouthBand;
        intent.claim = {120, kVisualCommitShadowBandBurstBytes,
                        kVisualCommitShadowMaxRows, true};
    } else if (sample.device_state == kDeviceStateIdle &&
               budget.level == VisualBudgetLevel::TransitionCandidate) {
        intent.kind = VisualIntentKind::IdleLifeBand;
        intent.claim = {7000, kVisualCommitShadowBandBurstBytes,
                        kVisualCommitShadowMaxRows, true};
    }
    return intent;
}

inline VisualCommitShadowDecision VisualCommitShadow_Evaluate(
    const VisualBudgetSample& sample, const VisualBudgetDecision& budget,
    const VisualIntent& intent) {
    if (intent.kind == VisualIntentKind::None) {
        return {};
    }
    if (intent.claim.max_rows > kVisualCommitShadowMaxRows ||
        intent.claim.psram_burst_bytes > kVisualCommitShadowBandBurstBytes) {
        return {VisualCommitBackend::None, false, "claim_cap"};
    }
    if (sample.playback_hold) {
        return {VisualCommitBackend::None, false, "playback_hold"};
    }

    if (intent.kind == VisualIntentKind::SpeakingMouthBand) {
        const bool mouth_budget = budget.level == VisualBudgetLevel::MouthReduced ||
                                  budget.level == VisualBudgetLevel::MouthOnly;
        return mouth_budget
                   ? VisualCommitShadowDecision{VisualCommitBackend::LayeredBand, true,
                                                "mouth_budget"}
                   : VisualCommitShadowDecision{VisualCommitBackend::None, false,
                                                "mouth_drop"};
    }

    const bool idle_quiet = budget.level == VisualBudgetLevel::TransitionCandidate &&
                            sample.decode_queue == 0 && sample.playback_queue == 0;
    return idle_quiet
               ? VisualCommitShadowDecision{VisualCommitBackend::PrecomposedBand, true,
                                            "idle_quiet"}
               : VisualCommitShadowDecision{VisualCommitBackend::None, false,
                                            "idle_drop"};
}

inline const char* VisualIntentKindName(VisualIntentKind kind) {
    switch (kind) {
        case VisualIntentKind::SpeakingMouthBand: return "mouth_band";
        case VisualIntentKind::IdleLifeBand: return "idle_life_band";
        default: return "none";
    }
}

inline const char* VisualCommitBackendName(VisualCommitBackend backend) {
    switch (backend) {
        case VisualCommitBackend::LayeredBand: return "layered_band";
        case VisualCommitBackend::PrecomposedBand: return "precomposed_band";
        default: return "none";
    }
}
