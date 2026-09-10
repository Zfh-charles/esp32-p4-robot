#!/usr/bin/env python3
"""Validate p4.trace.v1 without third-party packages."""
from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path
from typing import Any

VERSION = "p4.trace.v1"
TYPE_RE = re.compile(r"^[A-Za-z][A-Za-z0-9_.-]*$")
ENVS = {"log", "host", "replay", "hil"}


def is_int(value: Any) -> bool:
    return isinstance(value, int) and not isinstance(value, bool)


def keys(obj: dict[str, Any], required: set[str], allowed: set[str], path: str, out: list[str]) -> None:
    out.extend(f"{path}: missing required key '{key}'" for key in sorted(required - obj.keys()))
    out.extend(f"{path}: unknown key '{key}'" for key in sorted(obj.keys() - allowed))


def valid_type(value: Any, path: str, out: list[str]) -> bool:
    if not isinstance(value, str) or not TYPE_RE.fullmatch(value):
        out.append(f"{path}: invalid type name")
        return False
    return True


def count(trace: dict[str, Any], target: str, type_name: str) -> int:
    entries = trace.get("events", []) if target == "event" else trace.get("expected", {}).get("effects", [])
    return sum(isinstance(item, dict) and item.get("type") == type_name for item in entries)


def validate_trace(trace: Any) -> list[str]:
    errors: list[str] = []
    if not isinstance(trace, dict):
        return ["$: must be an object"]
    top = {"schema_version", "trace_id", "source", "sanitized", "events", "expected"}
    keys(trace, top, top, "$", errors)
    if trace.get("schema_version") != VERSION:
        errors.append(f"$.schema_version: must be '{VERSION}'")
    if not isinstance(trace.get("trace_id"), str) or not trace.get("trace_id", "").strip():
        errors.append("$.trace_id: must be a non-empty string")
    if trace.get("sanitized") is not True:
        errors.append("$.sanitized: must be true before fixture use")

    source = trace.get("source")
    if not isinstance(source, dict):
        errors.append("$.source: must be an object")
    else:
        keys(source, {"fw_marker", "environment"}, {"fw_marker", "environment", "captured_at"}, "$.source", errors)
        marker = source.get("fw_marker")
        if not isinstance(marker, str) or not marker.startswith("boot_trace_"):
            errors.append("$.source.fw_marker: must start with 'boot_trace_'")
        if source.get("environment") not in ENVS:
            errors.append(f"$.source.environment: must be one of {sorted(ENVS)}")
        stamp = source.get("captured_at")
        if stamp is not None and (not isinstance(stamp, str) or "T" not in stamp):
            errors.append("$.source.captured_at: must be an ISO-8601-like date-time")

    events = trace.get("events")
    if not isinstance(events, list) or not events:
        errors.append("$.events: must be a non-empty array")
        events = []
    event_types: dict[int, str] = {}
    last_seq = last_time = -1
    for index, event in enumerate(events):
        path = f"$.events[{index}]"
        if not isinstance(event, dict):
            errors.append(f"{path}: must be an object")
            continue
        keys(event, {"seq", "t_ms", "type"}, {"seq", "t_ms", "type", "payload"}, path, errors)
        seq, time, type_name = event.get("seq"), event.get("t_ms"), event.get("type")
        seq_ok = is_int(seq) and seq >= 0
        if not seq_ok:
            errors.append(f"{path}.seq: must be a non-negative integer")
        elif seq <= last_seq:
            errors.append(f"{path}.seq: must be strictly increasing")
        else:
            last_seq = seq
        if not is_int(time) or time < 0:
            errors.append(f"{path}.t_ms: must be a non-negative integer")
        elif time < last_time:
            errors.append(f"{path}.t_ms: must be monotonic non-decreasing")
        else:
            last_time = time
        type_ok = valid_type(type_name, f"{path}.type", errors)
        if "payload" in event and not isinstance(event["payload"], dict):
            errors.append(f"{path}.payload: must be an object")
        if seq_ok and type_ok:
            event_types[seq] = type_name

    expected = trace.get("expected")
    if not isinstance(expected, dict):
        errors.append("$.expected: must be an object")
        expected = {}
    else:
        fields = {"effects", "m_plus", "m_minus", "invariants"}
        keys(expected, fields, fields, "$.expected", errors)
    effects = expected.get("effects")
    if not isinstance(effects, list):
        errors.append("$.expected.effects: must be an array")
        effects = []
    last_effect_seq = -1
    for index, effect in enumerate(effects):
        path = f"$.expected.effects[{index}]"
        if not isinstance(effect, dict):
            errors.append(f"{path}: must be an object")
            continue
        keys(effect, {"seq", "type", "caused_by_event_seq"}, {"seq", "type", "caused_by_event_seq", "payload"}, path, errors)
        seq = effect.get("seq")
        if not is_int(seq) or seq < 0:
            errors.append(f"{path}.seq: must be a non-negative integer")
        elif seq <= last_effect_seq:
            errors.append(f"{path}.seq: must be strictly increasing")
        else:
            last_effect_seq = seq
        valid_type(effect.get("type"), f"{path}.type", errors)
        cause = effect.get("caused_by_event_seq")
        if not is_int(cause) or cause < 0:
            errors.append(f"{path}.caused_by_event_seq: must be a non-negative integer")
        elif cause not in event_types:
            errors.append(f"{path}.caused_by_event_seq: references missing event seq {cause}")
        if "payload" in effect and not isinstance(effect["payload"], dict):
            errors.append(f"{path}.payload: must be an object")

    oracle_ids: set[str] = set()
    for field, positive in (("m_plus", True), ("m_minus", False)):
        entries = expected.get(field)
        if not isinstance(entries, list) or not entries:
            errors.append(f"$.expected.{field}: must be a non-empty array")
            continue
        limit_name = "min_count" if positive else "max_count"
        for index, entry in enumerate(entries):
            path = f"$.expected.{field}[{index}]"
            if not isinstance(entry, dict):
                errors.append(f"{path}: must be an object")
                continue
            required = {"id", "target", "type", limit_name}
            keys(entry, required, required, path, errors)
            oracle_id = entry.get("id")
            if not isinstance(oracle_id, str) or not oracle_id.strip():
                errors.append(f"{path}.id: must be non-empty")
            elif oracle_id in oracle_ids:
                errors.append(f"{path}.id: duplicate oracle id '{oracle_id}'")
            else:
                oracle_ids.add(oracle_id)
            target, type_name, limit = entry.get("target"), entry.get("type"), entry.get(limit_name)
            if target not in {"event", "effect"}:
                errors.append(f"{path}.target: must be event/effect")
            valid_type(type_name, f"{path}.type", errors)
            limit_ok = is_int(limit) and (limit >= 1 if positive else limit == 0)
            if not limit_ok:
                errors.append(f"{path}.{limit_name}: invalid bound")
            elif target in {"event", "effect"} and isinstance(type_name, str):
                actual = count(trace, target, type_name)
                if positive and actual < limit:
                    errors.append(f"{path}: M+ expected at least {limit}, observed {actual}")
                if not positive and actual > limit:
                    errors.append(f"{path}: M- expected at most {limit}, observed {actual}")

    invariants = expected.get("invariants")
    if not isinstance(invariants, list) or not invariants:
        errors.append("$.expected.invariants: must be a non-empty array")
        invariants = []
    positions: dict[str, list[int]] = {}
    for pos, effect in enumerate(effects):
        if isinstance(effect, dict) and isinstance(effect.get("type"), str):
            positions.setdefault(effect["type"], []).append(pos)
    for index, inv in enumerate(invariants):
        path = f"$.expected.invariants[{index}]"
        if not isinstance(inv, dict):
            errors.append(f"{path}: must be an object")
            continue
        oracle_id = inv.get("id")
        if not isinstance(oracle_id, str) or not oracle_id.strip():
            errors.append(f"{path}.id: must be non-empty")
        elif oracle_id in oracle_ids:
            errors.append(f"{path}.id: duplicate oracle id '{oracle_id}'")
        else:
            oracle_ids.add(oracle_id)
        kind = inv.get("kind")
        if kind == "effect_order":
            keys(inv, {"id", "kind", "before_type", "after_type"}, {"id", "kind", "before_type", "after_type"}, path, errors)
            before, after = inv.get("before_type"), inv.get("after_type")
            if not isinstance(before, str) or not isinstance(after, str) or not positions.get(before) or not positions.get(after):
                errors.append(f"{path}: effect_order references an absent effect type")
            elif positions[before][0] >= positions[after][0]:
                errors.append(f"{path}: '{before}' must occur before '{after}'")
        elif kind == "count":
            keys(inv, {"id", "kind", "target", "type"}, {"id", "kind", "target", "type", "min_count", "max_count"}, path, errors)
            target, type_name = inv.get("target"), inv.get("type")
            minimum, maximum = inv.get("min_count", 0), inv.get("max_count")
            if target not in {"event", "effect"} or not isinstance(type_name, str):
                errors.append(f"{path}: count requires target/type")
            elif not is_int(minimum) or minimum < 0 or (maximum is not None and (not is_int(maximum) or maximum < minimum)):
                errors.append(f"{path}: count bounds must satisfy 0 <= min <= max")
            else:
                actual = count(trace, target, type_name)
                if actual < minimum or (maximum is not None and actual > maximum):
                    errors.append(f"{path}: count observed {actual}, outside bounds")
        elif kind == "cause_type":
            keys(inv, {"id", "kind", "effect_type", "event_type"}, {"id", "kind", "effect_type", "event_type"}, path, errors)
            effect_type, event_type = inv.get("effect_type"), inv.get("event_type")
            matching = [item for item in effects if isinstance(item, dict) and item.get("type") == effect_type]
            if not isinstance(effect_type, str) or not isinstance(event_type, str) or not matching:
                errors.append(f"{path}: cause_type requires an existing effect type")
            elif any(event_types.get(item.get("caused_by_event_seq")) != event_type for item in matching):
                errors.append(f"{path}: effect '{effect_type}' has a cause other than '{event_type}'")
        else:
            errors.append(f"{path}.kind: unsupported invariant kind '{kind}'")
    return errors


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("paths", nargs="+", type=Path)
    parser.add_argument("--json", action="store_true", dest="as_json")
    args = parser.parse_args(argv)
    failed = False
    for path in args.paths:
        try:
            errors = validate_trace(json.loads(path.read_text(encoding="utf-8")))
        except (OSError, json.JSONDecodeError) as exc:
            errors = [f"read/parse error: {exc}"]
        failed |= bool(errors)
        if args.as_json:
            print(json.dumps({"path": str(path), "valid": not errors, "errors": errors}, ensure_ascii=False, sort_keys=True))
        elif errors:
            print(f"FAIL {path}")
            print("\n".join(f"  - {error}" for error in errors))
        else:
            print(f"PASS {path}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
