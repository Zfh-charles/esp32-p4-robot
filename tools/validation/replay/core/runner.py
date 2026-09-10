"""Run and adjudicate ``p4.trace.v1`` against the T2 system model."""

from __future__ import annotations

from dataclasses import dataclass
from typing import Any

from .clock import FakeClock
from .executor import FakeExecutor
from .model import SessionReplayModel


@dataclass(frozen=True)
class ReplayResult:
    passed: bool
    errors: tuple[str, ...]
    actual_effects: tuple[dict[str, Any], ...]
    delivered_events: tuple[dict[str, Any], ...]
    oracle_results: tuple[dict[str, Any], ...]

    def to_dict(self) -> dict[str, Any]:
        return {
            "passed": self.passed,
            "errors": list(self.errors),
            "actual_effects": list(self.actual_effects),
            "delivered_events": list(self.delivered_events),
            "oracle_results": list(self.oracle_results),
            "proof_boundary": "T2 system model only; not production-path or hardware evidence",
        }


def _count(entries: list[dict[str, Any]], type_name: str) -> int:
    return sum(item.get("type") == type_name for item in entries)


def _judge(expected: dict[str, Any], events: list[dict[str, Any]], effects: list[dict[str, Any]]):
    results: list[dict[str, Any]] = []
    errors: list[str] = []

    def record(oracle_id: str, passed: bool, detail: str) -> None:
        results.append({"id": oracle_id, "passed": passed, "detail": detail})
        if not passed:
            errors.append(f"oracle {oracle_id}: {detail}")

    for field, bound in (("m_plus", "min_count"), ("m_minus", "max_count")):
        for oracle in expected.get(field, []):
            entries = events if oracle["target"] == "event" else effects
            actual = _count(entries, oracle["type"])
            limit = oracle[bound]
            passed = actual >= limit if field == "m_plus" else actual <= limit
            operator = ">=" if field == "m_plus" else "<="
            record(oracle["id"], passed, f"count={actual}, required{operator}{limit}")

    event_types = {event["seq"]: event["type"] for event in events}
    for inv in expected.get("invariants", []):
        kind = inv["kind"]
        if kind == "effect_order":
            before = [i for i, effect in enumerate(effects) if effect["type"] == inv["before_type"]]
            after = [i for i, effect in enumerate(effects) if effect["type"] == inv["after_type"]]
            record(inv["id"], bool(before and after and before[0] < after[0]), f"positions={before[:1]} before {after[:1]}")
        elif kind == "count":
            entries = events if inv["target"] == "event" else effects
            actual = _count(entries, inv["type"])
            minimum, maximum = inv.get("min_count", 0), inv.get("max_count")
            passed = actual >= minimum and (maximum is None or actual <= maximum)
            record(inv["id"], passed, f"count={actual}, bounds=[{minimum},{maximum}]")
        elif kind == "cause_type":
            matches = [effect for effect in effects if effect["type"] == inv["effect_type"]]
            passed = bool(matches) and all(event_types.get(effect["caused_by_event_seq"]) == inv["event_type"] for effect in matches)
            record(inv["id"], passed, f"matched={len(matches)}")
        else:
            record(inv.get("id", "unknown"), False, f"unsupported invariant kind {kind!r}")
    return results, errors


def run_trace(trace: dict[str, Any]) -> ReplayResult:
    """Execute a validated trace; scheduling errors become deterministic failures."""
    errors: list[str] = []
    if trace.get("schema_version") != "p4.trace.v1":
        return ReplayResult(False, ("unsupported schema_version",), (), (), ())
    clock = FakeClock()
    executor = FakeExecutor(clock)
    model = SessionReplayModel(clock, executor)
    last_seq = -1
    try:
        for event in trace.get("events", []):
            seq = event["seq"]
            if seq <= last_seq:
                raise ValueError("event seq must be strictly increasing")
            last_seq = seq
            clock.advance_to(event["t_ms"])
            executor.run_due()
            model.dispatch(event)
            executor.run_due()
        executor.run_until_idle()
    except (KeyError, TypeError, ValueError, RuntimeError) as exc:
        errors.append(f"replay error: {exc}")

    expected = trace.get("expected", {})
    if model.effects != expected.get("effects", []):
        errors.append(f"effect sequence mismatch: expected={expected.get('effects', [])!r}, actual={model.effects!r}")
    oracle_results, oracle_errors = _judge(expected, model.delivered_events, model.effects)
    errors.extend(oracle_errors)
    return ReplayResult(
        not errors,
        tuple(errors),
        tuple(model.effects),
        tuple(model.delivered_events),
        tuple(oracle_results),
    )
