from __future__ import annotations

import json
import unittest
from pathlib import Path

from .full_frame_emotion_guard_model import FullFrameEmotionGuardModel


FIXTURE = (
    Path(__file__).resolve().parents[1]
    / "regressions"
    / "s1gk_tts_second_full_frame.json"
)


def replay(model: FullFrameEmotionGuardModel) -> FullFrameEmotionGuardModel:
    trace = json.loads(FIXTURE.read_text(encoding="utf-8"))
    for event in trace["events"]:
        if event["type"] == "Visual.SpeechBegin":
            model.begin_speech(event["generation"])
        elif event["type"] == "Emotion.SemanticRequested":
            model.request_emotion(
                event["emotion"],
                layered_capable=event["layered_capable"],
            )
        elif event["type"] == "Emotion.FullFrameRequested":
            model.request_full_frame(event["emotion"], event["source"])
        elif event["type"] == "Audio.TtsFirst":
            model.notify_tts_audio_first()
    return model


class FullFrameEmotionGuardModelTest(unittest.TestCase):
    def test_real_s1gk_order_is_a_three_transaction_counterexample_without_guard(self):
        model = replay(FullFrameEmotionGuardModel(guard_enabled=False))
        self.assertEqual(
            ["commit", "commit", "commit"],
            [item.action for item in model.history],
        )
        self.assertEqual(
            ["happy", "happy", "neutral"],
            [item.emotion for item in model.history],
        )

    def test_guard_rejects_the_exact_unsafe_order_when_canonical_was_not_preloaded(self):
        model = replay(FullFrameEmotionGuardModel())
        self.assertEqual(["drop", "drop", "drop"], [item.action for item in model.history])
        self.assertEqual(
            ["prefer_layered_canonical", "tts_audio_started", "prefer_layered_canonical"],
            [item.reason for item in model.history],
        )
        self.assertEqual(0, model.full_frame_commits)

    def test_preloaded_canonical_is_the_single_safe_opening_transaction(self):
        model = FullFrameEmotionGuardModel()
        model.begin_speech(1)
        model.request_emotion("happy", layered_capable=True)
        self.assertEqual(
            "commit",
            model.request_full_frame("happy", "mouth_canonical_base").action,
        )
        model.notify_tts_audio_first()
        model.request_emotion("neutral", layered_capable=True)
        late = model.request_full_frame("neutral", "late_static_hold_native")
        self.assertEqual(("drop", "prefer_layered_canonical"), (late.action, late.reason))
        self.assertEqual(1, model.full_frame_commits)

    def test_new_generation_can_commit_a_new_preloaded_canonical(self):
        model = FullFrameEmotionGuardModel()
        model.begin_speech(1)
        model.request_emotion("happy", layered_capable=True)
        self.assertEqual("commit", model.request_full_frame("happy", "mouth_canonical_base").action)
        model.begin_speech(2)
        model.request_emotion("neutral", layered_capable=True)
        self.assertEqual("commit", model.request_full_frame("neutral", "mouth_canonical_base").action)

    def test_first_full_frame_after_audio_start_is_also_dropped(self):
        model = FullFrameEmotionGuardModel()
        model.begin_speech(1)
        model.request_emotion("happy", layered_capable=False)
        model.notify_tts_audio_first()
        self.assertEqual("drop", model.request_full_frame("happy").action)

    def test_late_emotions_are_latest_only_and_never_replayed_next_generation(self):
        model = FullFrameEmotionGuardModel()
        model.begin_speech(1)
        model.request_emotion("happy", layered_capable=True)
        self.assertEqual("commit", model.request_full_frame("happy", "mouth_canonical_base").action)
        model.notify_tts_audio_first()
        model.request_emotion("angry", layered_capable=True)
        model.request_emotion("sad", layered_capable=True)
        self.assertEqual("sad", model.pending_emotion)
        self.assertEqual(1, model.full_frame_commits)

        model.begin_speech(2)
        self.assertEqual("", model.pending_emotion)
        self.assertEqual(0, model.full_frame_commits)


if __name__ == "__main__":
    unittest.main()
