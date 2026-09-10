"""Deterministic T2 session system model, not a production-path proof."""

from __future__ import annotations

from dataclasses import dataclass
from typing import Any

from .clock import FakeClock
from .executor import FakeExecutor


@dataclass
class _Fault:
    action: str
    target_type: str
    delay_ms: int = 0


class SessionReplayModel:
    def __init__(self, clock: FakeClock | None = None, executor: FakeExecutor | None = None) -> None:
        self.clock = clock or FakeClock()
        self.executor = executor or FakeExecutor(self.clock)
        if self.executor.clock is not self.clock:
            raise ValueError("executor and model must share the same FakeClock")
        self.effects: list[dict[str, Any]] = []
        self.delivered_events: list[dict[str, Any]] = []
        self.online = True
        self.session_active = False
        self.wake_enabled = False
        self.playing = False
        self.speech_generation = 0
        self.pending_emotion: tuple[str, int] | None = None
        self.committed_emotion: tuple[str, int] | None = None
        self.reconnect_backoff_ms = 1_000
        self.exit_pending = False
        self._seen_chunks: set[tuple[int, object]] = set()
        self._faults: list[_Fault] = []

    def _emit(self, type_name: str, cause_seq: int, payload: dict[str, Any] | None = None) -> None:
        effect: dict[str, Any] = {
            "seq": len(self.effects) + 1,
            "type": type_name,
            "caused_by_event_seq": cause_seq,
        }
        if payload:
            effect["payload"] = payload
        self.effects.append(effect)

    def dispatch(self, event: dict[str, Any]) -> None:
        event_type = event["type"]
        if event_type == "Fault.Inject":
            self.delivered_events.append(event.copy())
            payload = event.get("payload", {})
            action = payload.get("action")
            target = payload.get("target_type")
            delay_ms = payload.get("delay_ms", 0)
            if action not in {"duplicate_next", "drop_next", "delay_next"}:
                raise ValueError(f"unsupported fault action: {action!r}")
            if not isinstance(target, str) or not target:
                raise ValueError("Fault.Inject requires target_type")
            if not isinstance(delay_ms, int) or isinstance(delay_ms, bool) or delay_ms < 0:
                raise ValueError("Fault.Inject delay_ms must be a non-negative integer")
            self._faults.append(_Fault(action, target, delay_ms))
            return

        index = next((i for i, fault in enumerate(self._faults) if fault.target_type == event_type), None)
        if index is None:
            self._deliver(event)
            return
        fault = self._faults.pop(index)
        if fault.action == "drop_next":
            return
        if fault.action == "delay_next":
            snapshot = {**event, "payload": dict(event.get("payload", {}))}
            self.executor.schedule(fault.delay_ms, lambda: self._deliver(snapshot))
            return
        self._deliver(event)
        self._deliver(event)

    def _deliver(self, event: dict[str, Any]) -> None:
        self.delivered_events.append(event.copy())
        seq = event["seq"]
        type_name = event["type"]
        payload = event.get("payload", {})
        if type_name == "Boot.Ready":
            self.wake_enabled = True
            self._emit("UI.ShowStandby", seq)
            self._emit("Wake.Enable", seq)
        elif type_name == "Wake.Detected":
            if self.wake_enabled and not self.session_active and self.online:
                self.wake_enabled = False
                self.session_active = True
                self._emit("Wake.Disable", seq)
                self._emit("Session.Begin", seq)
                self._emit("Audio.OpenUplink", seq)
                self._emit("UI.ShowListening", seq)
        elif type_name == "TTS.Start":
            if not self.session_active or self.playing:
                return
            self.speech_generation += 1
            self.playing = True
            self._seen_chunks.clear()
            self._emit("Audio.StartPlayback", seq, {"generation": self.speech_generation})
            self._emit("Visual.SpeechBegin", seq, {"generation": self.speech_generation})
        elif type_name == "TTS.Chunk":
            if not self.playing:
                return
            chunk_payload = dict(payload)
            chunk_payload.setdefault("generation", self.speech_generation)
            chunk_key = (self.speech_generation, chunk_payload.get("chunk_index", seq))
            if chunk_key in self._seen_chunks:
                return
            self._seen_chunks.add(chunk_key)
            self._emit("Audio.QueueChunk", seq, chunk_payload)
        elif type_name == "TTS.End":
            if not self.playing:
                return
            self.playing = False
            generation = self.speech_generation
            self._emit("Audio.StopPlayback", seq, {"generation": generation})
            self._emit("Visual.SpeechEnd", seq, {"generation": generation})
            self._emit("UI.ShowListening", seq)
        elif type_name == "Session.ExitRequested":
            if not self.session_active or self.exit_pending:
                return
            self.exit_pending = True
            self._emit("Session.CloseRequest", seq)

            def finish_exit() -> None:
                self.session_active = False
                self.exit_pending = False
                self.playing = False
                self.wake_enabled = True
                self._emit("Session.Closed", seq)
                self._emit("Wake.Enable", seq)
                self._emit("UI.ShowStandby", seq)

            self.executor.schedule(50, finish_exit)
        elif type_name == "Network.Disconnected":
            self.online = False
            self._emit("Network.Offline", seq)
            self.executor.schedule(1_000, lambda: self._emit("Network.ReconnectAttempt", seq))
        elif type_name == "Network.ReconnectResult":
            if bool(payload.get("success")):
                self.online = True
                self.reconnect_backoff_ms = 1_000
                self._emit("Network.Online", seq)
            else:
                delay = self.reconnect_backoff_ms
                self.executor.schedule(delay, lambda: self._emit("Network.ReconnectAttempt", seq))
                self.reconnect_backoff_ms = min(delay * 2, 32_000)
        elif type_name == "Emotion.Requested":
            emotion = payload.get("emotion")
            if not isinstance(emotion, str) or not emotion:
                raise ValueError("Emotion.Requested requires payload.emotion")
            generation = payload.get("generation", self.speech_generation)
            if not isinstance(generation, int) or isinstance(generation, bool) or generation < 0:
                raise ValueError("emotion generation must be a non-negative integer")
            self.pending_emotion = (emotion, generation)
            self._emit("Emotion.Pending", seq, {"emotion": emotion, "generation": generation})
        elif type_name == "Visual.SafePoint" and self.pending_emotion is not None:
            emotion, generation = self.pending_emotion
            output = {"emotion": emotion, "generation": generation}
            if self.committed_emotion == self.pending_emotion:
                self._emit("Emotion.Deduplicated", seq, output)
            else:
                self.committed_emotion = self.pending_emotion
                self._emit("Emotion.Commit", seq, output)
            self.pending_emotion = None
