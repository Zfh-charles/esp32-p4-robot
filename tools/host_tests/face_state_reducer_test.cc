#include "../../main/domain/face_state_reducer.h"

#include <cassert>

using Reducer = domain::FaceStateReducer;

int main() {
    Reducer state;
    assert(state.NotifySpeechState(false).generation == 0);

    auto speech = state.NotifySpeechState(true);
    assert(speech.generation == 1);
    assert(!speech.pending_migrated);
    assert(state.NotifySpeechState(true).generation == 0);

    state.RequestEmotion("Happy");
    assert(state.has_pending());
    assert(state.pending_generation() == 1);
    assert(state.CommitPending() == Reducer::CommitDecision::kCommitted);
    assert(!state.has_pending());
    assert(state.has_committed());

    state.RequestEmotion("happy");
    assert(state.CommitPending() == Reducer::CommitDecision::kDeduplicated);

    state.NotifySpeechState(false);
    speech = state.NotifySpeechState(true);
    assert(speech.generation == 2);
    state.RequestEmotion("happy");
    assert(state.CommitPending() == Reducer::CommitDecision::kCommitted);
    assert(state.committed_generation() == 2);

    state.RequestEmotion("sad");
    state.NotifySpeechState(false);
    speech = state.NotifySpeechState(true);
    assert(speech.generation == 3);
    assert(speech.pending_migrated);
    assert(state.pending_generation() == 3);
    assert(state.CommitPending() == Reducer::CommitDecision::kCommitted);

    state.RequestEmotion(nullptr);
    assert(state.pending_fingerprint() == Reducer::EmotionFingerprint("neutral"));
    assert(state.CommitPending() == Reducer::CommitDecision::kCommitted);
    assert(state.CommitPending() == Reducer::CommitDecision::kNoPending);
    return 0;
}
