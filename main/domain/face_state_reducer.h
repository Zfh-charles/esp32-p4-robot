#pragma once

#include <cstdint>

namespace domain {

// Pure requested/pending/committed state. It deliberately owns no renderer,
// timer, lock or heap-backed string so it can be exercised on the host before
// it is ever allowed to drive the legacy presenter.
class FaceStateReducer final {
public:
    enum class CommitDecision : uint8_t {
        kNoPending = 0,
        kDeduplicated = 1,
        kCommitted = 2,
    };

    struct SpeechDecision {
        uint32_t generation = 0;
        bool pending_migrated = false;
    };

    SpeechDecision NotifySpeechState(bool speaking) noexcept {
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

    void RequestEmotion(const char* emotion_name) noexcept {
        pending_fingerprint_ = EmotionFingerprint(emotion_name);
        pending_generation_ = speech_generation_;
        has_pending_ = true;
    }

    CommitDecision CommitPending() noexcept {
        if (!has_pending_) {
            return CommitDecision::kNoPending;
        }
        if (has_committed_ && pending_fingerprint_ == committed_fingerprint_ &&
            pending_generation_ == committed_generation_) {
            has_pending_ = false;
            return CommitDecision::kDeduplicated;
        }
        committed_fingerprint_ = pending_fingerprint_;
        committed_generation_ = pending_generation_;
        has_committed_ = true;
        has_pending_ = false;
        return CommitDecision::kCommitted;
    }

    bool speech_turn_active() const noexcept { return speech_turn_active_; }
    uint32_t speech_generation() const noexcept { return speech_generation_; }
    bool has_pending() const noexcept { return has_pending_; }
    uint32_t pending_fingerprint() const noexcept { return pending_fingerprint_; }
    uint32_t pending_generation() const noexcept { return pending_generation_; }
    bool has_committed() const noexcept { return has_committed_; }
    uint32_t committed_fingerprint() const noexcept { return committed_fingerprint_; }
    uint32_t committed_generation() const noexcept { return committed_generation_; }

    static uint32_t EmotionFingerprint(const char* emotion_name) noexcept {
        if (emotion_name == nullptr) {
            emotion_name = "neutral";
        }
        uint32_t hash = 2166136261u;
        for (const unsigned char* p =
                 reinterpret_cast<const unsigned char*>(emotion_name);
             *p != 0; ++p) {
            unsigned char ch = *p;
            if (ch >= 'A' && ch <= 'Z') {
                ch = static_cast<unsigned char>(ch + ('a' - 'A'));
            }
            hash ^= ch;
            hash *= 16777619u;
        }
        return hash;
    }

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

}  // namespace domain
