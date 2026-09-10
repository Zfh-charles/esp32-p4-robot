#pragma once

#include <cstdint>

namespace domain {

// Pure, one-slot latest-value scheduler. It owns no task, lock, heap storage,
// renderer or hardware handle. Every evaluation consumes its candidate.
class VisualCommitScheduler final {
public:
    static constexpr uint16_t kMaxRows = 48;
    static constexpr uint32_t kMaxBurstBytes = 480U * kMaxRows * 2U;

    enum class IntentKind : uint8_t { kMouthBand = 0, kIdleLifeBand };
    enum class Budget : uint8_t {
        kFrozen = 0,
        kStaticOnly,
        kMouthReduced,
        kMouthOnly,
        kTransition,
    };
    enum class Backend : uint8_t { kNone = 0, kLayeredBand, kPrecomposedBand };
    enum class Action : uint8_t { kNone = 0, kQueued, kDropped, kCommit };
    enum class Reason : uint8_t {
        kEmpty = 0,
        kLatest,
        kSuperseded,
        kOutOfOrder,
        kFutureTimestamp,
        kExpired,
        kStaleGeneration,
        kClaimCap,
        kResourceBusy,
        kMouthBudget,
        kLifeBudget,
        kAdmitted,
    };

    struct Intent {
        IntentKind kind = IntentKind::kMouthBand;
        uint32_t sequence = 0;
        uint32_t generation = 0;
        uint64_t created_at_ms = 0;
        uint32_t deadline_ms = 0;
        uint16_t max_rows = kMaxRows;
        uint32_t burst_bytes = kMaxBurstBytes;
    };

    struct Decision {
        Action action = Action::kNone;
        Backend backend = Backend::kNone;
        uint32_t sequence = 0;
        uint32_t superseded_sequence = 0;
        Reason reason = Reason::kEmpty;
    };

    bool SetGeneration(uint32_t generation) noexcept {
        if (generation < generation_) return false;
        generation_ = generation;
        return true;
    }

    Decision Submit(const Intent& intent) noexcept {
        if (has_sequence_ && intent.sequence <= last_sequence_) {
            return {Action::kDropped, Backend::kNone, intent.sequence, 0,
                    Reason::kOutOfOrder};
        }
        last_sequence_ = intent.sequence;
        has_sequence_ = true;
        const uint32_t superseded = has_pending_ ? pending_.sequence : 0;
        pending_ = intent;
        has_pending_ = true;
        return {Action::kQueued, Backend::kNone, intent.sequence, superseded,
                superseded == 0 ? Reason::kLatest : Reason::kSuperseded};
    }

    Decision Evaluate(uint64_t now_ms, Budget budget,
                      bool resource_busy = false) noexcept {
        if (!has_pending_) return {};
        const Intent intent = pending_;
        has_pending_ = false;
        if (now_ms < intent.created_at_ms) return Drop(intent, Reason::kFutureTimestamp);
        if (now_ms - intent.created_at_ms > intent.deadline_ms) {
            return Drop(intent, Reason::kExpired);
        }
        if (intent.generation != generation_) return Drop(intent, Reason::kStaleGeneration);
        if (intent.max_rows > kMaxRows || intent.burst_bytes > kMaxBurstBytes) {
            return Drop(intent, Reason::kClaimCap);
        }
        if (resource_busy) return Drop(intent, Reason::kResourceBusy);
        if (intent.kind == IntentKind::kMouthBand) {
            if (budget != Budget::kMouthReduced && budget != Budget::kMouthOnly) {
                return Drop(intent, Reason::kMouthBudget);
            }
            return {Action::kCommit, Backend::kLayeredBand, intent.sequence, 0,
                    Reason::kAdmitted};
        }
        if (budget != Budget::kTransition) return Drop(intent, Reason::kLifeBudget);
        return {Action::kCommit, Backend::kPrecomposedBand, intent.sequence, 0,
                Reason::kAdmitted};
    }

    bool has_pending() const noexcept { return has_pending_; }
    uint32_t generation() const noexcept { return generation_; }

private:
    static Decision Drop(const Intent& intent, Reason reason) noexcept {
        return {Action::kDropped, Backend::kNone, intent.sequence, 0, reason};
    }

    Intent pending_{};
    uint32_t generation_ = 0;
    uint32_t last_sequence_ = 0;
    bool has_pending_ = false;
    bool has_sequence_ = false;
};

}  // namespace domain
