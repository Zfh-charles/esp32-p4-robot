#include "../../main/domain/face_state_reducer.h"

#include <cassert>
#include <cstdint>
#include <initializer_list>

using Reducer = domain::FaceStateReducer;

namespace {

enum class EventKind : uint8_t {
    kSpeechStarted,
    kSpeechStopped,
    kEmotionRequested,
    kSafePoint,
};

struct Event {
    uint64_t at_ms;
    EventKind kind;
    const char* emotion = nullptr;
};

// Test-only reference for the exact requested/pending/committed rules in the
// legacy ScreenPresenter. It intentionally owns no production implementation.
class LegacyReference final {
public:
    Reducer::SpeechDecision ApplySpeech(bool speaking) {
        if (!speaking) {
            speech_turn_active_ = false;
            return {};
        }
        if (speech_turn_active_) {
            return {};
        }
        speech_turn_active_ = true;
        ++speech_generation_;
        if (speech_generation_ == 0) {
            ++speech_generation_;
        }
        bool migrated = false;
        if (has_pending_ && pending_generation_ != speech_generation_) {
            pending_generation_ = speech_generation_;
            migrated = true;
        }
        return {speech_generation_, migrated};
    }

    void Request(const char* emotion) {
        pending_fingerprint_ = Reducer::EmotionFingerprint(emotion);
        pending_generation_ = speech_generation_;
        has_pending_ = true;
    }

    Reducer::CommitDecision Commit() {
        if (!has_pending_) {
            return Reducer::CommitDecision::kNoPending;
        }
        if (has_committed_ && pending_fingerprint_ == committed_fingerprint_ &&
            pending_generation_ == committed_generation_) {
            has_pending_ = false;
            return Reducer::CommitDecision::kDeduplicated;
        }
        committed_fingerprint_ = pending_fingerprint_;
        committed_generation_ = pending_generation_;
        has_committed_ = true;
        has_pending_ = false;
        return Reducer::CommitDecision::kCommitted;
    }

    bool speech_turn_active() const { return speech_turn_active_; }
    uint32_t speech_generation() const { return speech_generation_; }
    bool has_pending() const { return has_pending_; }
    uint32_t pending_fingerprint() const { return pending_fingerprint_; }
    uint32_t pending_generation() const { return pending_generation_; }
    bool has_committed() const { return has_committed_; }
    uint32_t committed_fingerprint() const { return committed_fingerprint_; }
    uint32_t committed_generation() const { return committed_generation_; }

private:
    uint32_t speech_generation_ = 0;
    uint32_t pending_fingerprint_ = 0;
    uint32_t pending_generation_ = 0;
    uint32_t committed_fingerprint_ = 0;
    uint32_t committed_generation_ = 0;
    bool speech_turn_active_ = false;
    bool has_pending_ = false;
    bool has_committed_ = false;
};

void AssertStateEqual(const LegacyReference& legacy, const Reducer& reducer) {
    assert(legacy.speech_turn_active() == reducer.speech_turn_active());
    assert(legacy.speech_generation() == reducer.speech_generation());
    assert(legacy.has_pending() == reducer.has_pending());
    assert(legacy.pending_fingerprint() == reducer.pending_fingerprint());
    assert(legacy.pending_generation() == reducer.pending_generation());
    assert(legacy.has_committed() == reducer.has_committed());
    assert(legacy.committed_fingerprint() == reducer.committed_fingerprint());
    assert(legacy.committed_generation() == reducer.committed_generation());
}

void Replay(std::initializer_list<Event> trace) {
    LegacyReference legacy;
    Reducer reducer;
    uint64_t previous_ms = 0;
    for (const Event& event : trace) {
        assert(event.at_ms >= previous_ms);
        previous_ms = event.at_ms;
        switch (event.kind) {
            case EventKind::kSpeechStarted: {
                const auto expected = legacy.ApplySpeech(true);
                const auto actual = reducer.NotifySpeechState(true);
                assert(expected.generation == actual.generation);
                assert(expected.pending_migrated == actual.pending_migrated);
                break;
            }
            case EventKind::kSpeechStopped: {
                const auto expected = legacy.ApplySpeech(false);
                const auto actual = reducer.NotifySpeechState(false);
                assert(expected.generation == actual.generation);
                assert(expected.pending_migrated == actual.pending_migrated);
                break;
            }
            case EventKind::kEmotionRequested:
                legacy.Request(event.emotion);
                reducer.RequestEmotion(event.emotion);
                break;
            case EventKind::kSafePoint:
                assert(legacy.Commit() == reducer.CommitPending());
                break;
        }
        AssertStateEqual(legacy, reducer);
    }
}

}  // namespace

int main() {
    // Boot/standby seed: generation zero is a legitimate committed state.
    Replay({{0, EventKind::kEmotionRequested, "standby"},
            {1, EventKind::kSafePoint}});

    // Same generation deduplicates case-insensitively; a new speech generation
    // must commit the same emotion again so the mouth/life pipeline can re-arm.
    Replay({{10, EventKind::kSpeechStarted},
            {20, EventKind::kEmotionRequested, "Happy"},
            {21, EventKind::kSafePoint},
            {30, EventKind::kEmotionRequested, "happy"},
            {31, EventKind::kSafePoint},
            {1000, EventKind::kSpeechStopped},
            {1300, EventKind::kSpeechStarted},
            {1310, EventKind::kEmotionRequested, "happy"},
            {1311, EventKind::kSafePoint}});

    // While a ROI transaction owns the panel, only the newest pending emotion
    // survives; the safe point commits that latest value.
    Replay({{100, EventKind::kSpeechStarted},
            {120, EventKind::kEmotionRequested, "sad"},
            {130, EventKind::kEmotionRequested, "angry"},
            {200, EventKind::kSafePoint},
            {201, EventKind::kSafePoint}});

    // A pending request arriving between turns migrates to the next generation.
    Replay({{100, EventKind::kSpeechStarted},
            {400, EventKind::kSpeechStopped},
            {500, EventKind::kEmotionRequested, "loving"},
            {900, EventKind::kSpeechStarted},
            {920, EventKind::kSafePoint}});

    // Null requests preserve the legacy neutral fallback.
    Replay({{0, EventKind::kEmotionRequested, nullptr},
            {1, EventKind::kSafePoint}});
    return 0;
}
