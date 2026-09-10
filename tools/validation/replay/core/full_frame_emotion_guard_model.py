"""Offline oracle for full-frame emotion commits around the TTS start edge.

This is test-only policy evidence, not production firmware code.
"""

from dataclasses import dataclass


@dataclass(frozen=True)
class Decision:
    action: str
    emotion: str
    generation: int
    reason: str


class FullFrameEmotionGuardModel:
    def __init__(self, *, guard_enabled: bool = True) -> None:
        self.guard_enabled = guard_enabled
        self.generation = 0
        self.tts_audio_started = False
        self.full_frame_commits = 0
        self.requested_emotion = ""
        self.pending_emotion = ""
        self.layered_capable = False
        self.history: list[Decision] = []

    def begin_speech(self, generation: int) -> None:
        if generation <= self.generation:
            raise ValueError("speech generation must increase")
        self.generation = generation
        self.tts_audio_started = False
        self.full_frame_commits = 0
        self.requested_emotion = ""
        self.pending_emotion = ""
        self.layered_capable = False

    def request_emotion(self, emotion: str, *, layered_capable: bool) -> None:
        if self.generation == 0 or not emotion:
            raise ValueError("active generation and emotion are required")
        self.requested_emotion = emotion
        self.layered_capable = layered_capable
        if self.tts_audio_started:
            self.pending_emotion = emotion

    def notify_tts_audio_first(self) -> None:
        if self.generation == 0:
            raise ValueError("TTS audio requires an active speech generation")
        self.tts_audio_started = True

    def request_full_frame(self, emotion: str, source: str = "opening_seed") -> Decision:
        if self.generation == 0 or not emotion:
            raise ValueError("active generation and emotion are required")
        if self.guard_enabled and self.layered_capable and source in {
            "static_hold_native",
            "late_static_hold_native",
        }:
            decision = Decision(
                "drop",
                emotion,
                self.generation,
                "prefer_layered_canonical",
            )
        elif self.guard_enabled and self.tts_audio_started:
            decision = Decision(
                "drop",
                emotion,
                self.generation,
                "tts_audio_started",
            )
        elif self.guard_enabled and self.full_frame_commits >= 1:
            decision = Decision(
                "drop",
                emotion,
                self.generation,
                "generation_full_frame_cap",
            )
        else:
            self.full_frame_commits += 1
            decision = Decision("commit", emotion, self.generation, source)
        self.history.append(decision)
        return decision
