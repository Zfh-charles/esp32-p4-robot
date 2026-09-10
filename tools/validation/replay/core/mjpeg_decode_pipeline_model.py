"""G4b oracle for current MJPEG source/decode/present fault semantics.

The model composes the G4a cached frame source with a deterministic decoder
plan.  It captures observable ordering from ``emotion_video_player.c`` while
deliberately excluding allocation sizes, PSRAM, JPEG hardware, RTOS locks, and
LVGL latency.  It is a strangler fixture, not a production implementation.
"""

from __future__ import annotations

from dataclasses import dataclass

from .mjpeg_frame_source_model import CachedFrameSourceModel, FrameClaim


_DECODE_RESULTS = {"ok", "buffer_not_enough", "fail"}


@dataclass(frozen=True)
class DecodePlan:
    input_alloc_ok: bool = True
    output_alloc_ok: bool = True
    decoder_open_ok: bool = True
    first_result: str = "ok"
    frame_info_ok: bool = True
    resize_alloc_ok: bool = True
    retry_result: str = "ok"
    decoded_size: int = 1

    def __post_init__(self) -> None:
        if self.first_result not in _DECODE_RESULTS:
            raise ValueError(f"unsupported first_result: {self.first_result}")
        if self.retry_result not in _DECODE_RESULTS:
            raise ValueError(f"unsupported retry_result: {self.retry_result}")
        if self.decoded_size < 0:
            raise ValueError("decoded_size must be non-negative")


@dataclass(frozen=True)
class DecodeOutcome:
    status: str
    frame_index: int | None
    presented: bool = False


class MjpegDecodePipelineModel:
    """Model loop and synchronous bypass ownership around one decoder."""

    def __init__(self, source: CachedFrameSourceModel) -> None:
        self.source = source
        self.effects: list[tuple[str, int | None]] = []
        self.current_frame = 0
        self.decode_error_count = 0
        self.state = "playing"

    def _effect(self, name: str, frame_index: int | None = None) -> None:
        self.effects.append((name, frame_index))

    def _decode_claim(
        self,
        claim: FrameClaim,
        plan: DecodePlan,
        *,
        callback_enabled: bool,
    ) -> DecodeOutcome:
        if claim.status != "ok":
            return DecodeOutcome(claim.status, claim.frame_index)

        frame_index = claim.frame_index
        self._effect("source.claim", frame_index)
        # The source cursor has already advanced at this point in production.
        if not plan.input_alloc_ok:
            self._effect("input.alloc_failed", frame_index)
            return DecodeOutcome("no_mem", frame_index)
        if not plan.output_alloc_ok:
            self._effect("output.alloc_failed", frame_index)
            return DecodeOutcome("no_mem", frame_index)
        if not plan.decoder_open_ok:
            self._effect("decoder.open_failed", frame_index)
            return DecodeOutcome("fail", frame_index)

        self._effect("decoder.process", frame_index)
        result = plan.first_result
        if result == "buffer_not_enough":
            self._effect("decoder.get_frame_info", frame_index)
            if not plan.frame_info_ok:
                self._effect("decoder.frame_info_failed", frame_index)
                return DecodeOutcome("fail", frame_index)
            self._effect("output.reallocate", frame_index)
            if not plan.resize_alloc_ok:
                self._effect("output.realloc_failed", frame_index)
                return DecodeOutcome("no_mem", frame_index)
            self._effect("decoder.retry", frame_index)
            result = plan.retry_result

        # Production retries BUF_NOT_ENOUGH only once. A second occurrence is
        # handled by the generic non-OK branch.
        if result != "ok":
            self._effect("decoder.failed", frame_index)
            return DecodeOutcome("fail", frame_index)

        presented = plan.decoded_size > 0 and callback_enabled
        if presented:
            self._effect("present.frame", frame_index)
        else:
            self._effect("present.skipped", frame_index)
        self._effect("decoder.ok", frame_index)
        return DecodeOutcome("ok", frame_index, presented)

    def loop_tick(self, plan: DecodePlan | None = None) -> DecodeOutcome:
        """Mirror decode_task ownership for one due frame."""

        claim = self.source.claim_next()
        if claim.status == "not_found":
            # The task resets and makes the deadline due, but does not decode
            # frame zero until a later loop iteration.
            self.source.reset()
            self._effect("loop.reset")
            return DecodeOutcome("not_found", None)

        outcome = self._decode_claim(
            claim,
            plan or DecodePlan(),
            callback_enabled=True,
        )
        if outcome.status == "ok":
            self.current_frame += 1
            self.decode_error_count = 0
            self._effect("loop.success", outcome.frame_index)
        else:
            self.decode_error_count += 1
            self._effect("loop.decode_error", outcome.frame_index)
            if self.decode_error_count > 10:
                self.state = "error"
                self._effect("loop.error_state", outcome.frame_index)
        return outcome

    def bypass_decode_at(
        self,
        requested_index: int,
        plan: DecodePlan | None = None,
    ) -> DecodeOutcome:
        """Mirror decode_at_rgb565: seek once, suppress callback, no loop stats."""

        claim = self.source.seek_and_claim(requested_index)
        return self._decode_claim(
            claim,
            plan or DecodePlan(),
            callback_enabled=False,
        )

    def bypass_decode_next_n(
        self,
        count: int,
        plans: list[DecodePlan] | None = None,
    ) -> DecodeOutcome:
        """Mirror decode_next_n, including immediate EOF reset and retry."""

        remaining = max(count, 1)
        supplied = plans or []
        plan_cursor = 0
        outcome = DecodeOutcome("fail", None)
        for _ in range(remaining):
            claim = self.source.claim_next()
            if claim.status == "not_found":
                self.source.reset()
                self._effect("bypass.reset")
                claim = self.source.claim_next()
            plan = supplied[plan_cursor] if plan_cursor < len(supplied) else DecodePlan()
            plan_cursor += 1
            outcome = self._decode_claim(claim, plan, callback_enabled=False)
            if outcome.status != "ok":
                break
        return outcome
