#!/usr/bin/env python3
"""Guard the MJPEG strangler migration without pretending legacy debt is done."""

from __future__ import annotations

import argparse
import json
import re
import sys
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Any, Iterable


LEGACY_FUNCTIONS = (
    "build_frame_index",
    "hw_decode_frame_optimized",
    "decode_task",
    "emotion_video_player_decode_one_rgb565",
    "emotion_video_player_decode_next_n_rgb565",
    "emotion_video_player_decode_at_rgb565",
)

EVIDENCE_BLOCKERS = {
    "baseline_session_v2": "baseline_session_v2_pending",
    "g4_runtime_v2": "g4_v2_missing",
    "g4_soak_v3": "g4_v3_missing",
}

RUNTIME_MACROS = {
    "index_reader": "EMOTION_VIDEO_USE_INDEX_READER",
    "frame_source": "EMOTION_VIDEO_USE_FRAME_SOURCE",
    "decoder": "EMOTION_VIDEO_USE_DECODER_STAGE",
    "playback_clock": "EMOTION_VIDEO_USE_PLAYBACK_CLOCK",
    "present_sink": "EMOTION_VIDEO_USE_PRESENT_SINK",
}


@dataclass(frozen=True)
class AnchorResult:
    name: str
    function: str | None
    passed: bool
    detail: str


@dataclass(frozen=True)
class GuardReport:
    schema_version: int
    phase: str
    semantic_anchors_pass: bool
    manifest_valid: bool
    production_split: bool
    production_contracts_ready: bool
    runtime_ready: bool
    runtime_config_sync: bool
    g5_delete_ready: bool
    g5_certified_mixed_ready: bool
    g5_governance_ready: bool
    anchor_results: tuple[AnchorResult, ...]
    legacy_function_targets: tuple[str, ...]
    component_paths: dict[str, str | None]
    runtime_selection: dict[str, str | None]
    runtime_evidence_status: dict[str, str | None]
    migrated_roles: tuple[str, ...]
    certified_legacy_roles: tuple[str, ...]
    blockers: tuple[str, ...]
    closeout_blockers: tuple[str, ...]


@dataclass(frozen=True)
class AnchorSpec:
    name: str
    function: str | None
    ordered_patterns: tuple[str, ...]
    exact_counts: tuple[tuple[str, int], ...] = ()


ANCHORS = (
    AnchorSpec(
        "frame_cap",
        None,
        (r"#define\s+MAX_FRAMES_PER_VIDEO\s+300\b",),
    ),
    AnchorSpec(
        "index_wrapper_contract",
        "build_frame_index",
        (
            r"mjpeg_index_reader_build\s*\(",
            r"mjpeg_index_reader_build_legacy\s*\(",
            r"cache\s*->\s*buffer",
            r"cache\s*->\s*size",
            r"cache\s*->\s*frame_index",
            r"MAX_FRAMES_PER_VIDEO",
            r"&\s*frame_count",
            r"index_reader_yield",
            r"cache\s*->\s*frame_count\s*=\s*\(\s*int\s*\)\s*frame_count",
            r"cache\s*->\s*index_built\s*=\s*true",
        ),
    ),
    AnchorSpec(
        "frame_claim_before_decode_resources",
        "hw_decode_frame_optimized",
        (
            r"mjpeg_frame_source_view_t\s+frame_source",
            r"\.buffer\s*=\s*current_cache\s*->\s*buffer",
            r"\.entries\s*=\s*current_cache\s*->\s*frame_index",
            r"mjpeg_frame_source_claim_next\s*\(",
            r"mjpeg_frame_source_claim_next_legacy\s*\(",
            r"claim_result\s*!=\s*MJPEG_FRAME_SOURCE_OK",
            r"current_frame_index\s*=\s*\(\s*int\s*\)\s*frame_cursor",
            r"frame_data\s*=\s*\(\s*uint8_t\s*\*\s*\)\s*frame_claim\s*\.\s*data",
            r"xSemaphoreGive\s*\(\s*player\s*->\s*cache_mutex\s*\)",
            r"!\s*player\s*->\s*input_buffer",
            r"memcpy\s*\(\s*player\s*->\s*input_buffer",
            r"!\s*player\s*->\s*output_buffer",
            r"!\s*player\s*->\s*hw_dec_handle",
        ),
    ),
    AnchorSpec(
        "decode_resize_present_contract",
        "hw_decode_frame_optimized",
        (
            r"esp_video_dec_process\s*\(",
            r"vc_ret\s*==\s*ESP_VC_ERR_BUF_NOT_ENOUGH",
            r"esp_video_dec_get_frame_info\s*\(",
            r"esp_video_codec_free\s*\(",
            r"esp_video_codec_align_alloc\s*\(",
            r"esp_video_dec_process\s*\(",
            r"vc_ret\s*!=\s*ESP_VC_ERR_OK",
            r"out_frame\s*\.\s*decoded_size\s*>\s*0",
            r"(?:player\s*->\s*frame_cb|mjpeg_present_sink_esp_present\s*\()",
            r"(?:player\s*->\s*frame_cb\s*\(|player\s*->\s*frame_cb\s*,)",
        ),
        ((r"esp_video_dec_process\s*\(", 2),),
    ),
    AnchorSpec(
        "loop_clock_effect_contract",
        "decode_task",
        (
            r"mjpeg_playback_clock_player_poll\s*\(",
            r"clock_poll\s*\.\s*action\s*==\s*MJPEG_CLOCK_ACTION_DECODE",
            r"ret\s*=\s*hw_decode_frame_optimized\s*\(",
            r"mjpeg_playback_clock_player_on_decode_result\s*\(",
            r"ret\s*==\s*ESP_OK",
            r"effect\s*\.\s*delay_one_tick",
            r"ret\s*==\s*ESP_ERR_NOT_FOUND",
            r"handle_emotion_playback_complete\s*\(",
            r"effect\s*\.\s*enter_error_state",
            r"change_state\s*\(\s*player\s*,\s*EMOTION_VIDEO_STATE_ERROR\s*\)",
        ),
    ),
    AnchorSpec(
        "decode_one_callback_suppression",
        "emotion_video_player_decode_one_rgb565",
        (
            r"saved_cb\s*=\s*player\s*->\s*frame_cb",
            r"player\s*->\s*frame_cb\s*=\s*NULL",
            r"hw_decode_frame_optimized\s*\(",
            r"player\s*->\s*frame_cb\s*=\s*saved_cb",
        ),
    ),
    AnchorSpec(
        "next_n_eof_immediate_retry",
        "emotion_video_player_decode_next_n_rgb565",
        (
            r"saved_cb\s*=\s*player\s*->\s*frame_cb",
            r"player\s*->\s*frame_cb\s*=\s*NULL",
            r"ret\s*=\s*hw_decode_frame_optimized\s*\(",
            r"ret\s*==\s*ESP_ERR_NOT_FOUND",
            r"reset_current_emotion_position\s*\(",
            r"ret\s*=\s*hw_decode_frame_optimized\s*\(",
            r"player\s*->\s*frame_cb\s*=\s*saved_cb",
        ),
        ((r"hw_decode_frame_optimized\s*\(", 2),),
    ),
    AnchorSpec(
        "decode_at_modulo_and_callback_suppression",
        "emotion_video_player_decode_at_rgb565",
        (
            r"idx\s*=\s*frame_index\s*%\s*fc",
            r"current_frame_index\s*=\s*\(\s*int\s*\)\s*idx",
            r"saved_cb\s*=\s*player\s*->\s*frame_cb",
            r"player\s*->\s*frame_cb\s*=\s*NULL",
            r"ret\s*=\s*hw_decode_frame_optimized\s*\(",
            r"player\s*->\s*frame_cb\s*=\s*saved_cb",
        ),
        ((r"hw_decode_frame_optimized\s*\(", 1),),
    ),
)


def read_text(path: Path) -> str:
    return path.read_text(encoding="utf-8", errors="replace")


def _find_function_open(source: str, name: str) -> int | None:
    pattern = re.compile(
        rf"(?ms)^[\w\s\*]+?\b{re.escape(name)}\s*\([^;{{}}]*\)\s*\{{"
    )
    match = pattern.search(source)
    return None if match is None else match.end() - 1


def _matching_brace(source: str, opening: int) -> int | None:
    depth = 0
    state = "code"
    quote = ""
    index = opening
    while index < len(source):
        char = source[index]
        next_char = source[index + 1] if index + 1 < len(source) else ""
        if state == "line_comment":
            if char == "\n":
                state = "code"
        elif state == "block_comment":
            if char == "*" and next_char == "/":
                state = "code"
                index += 1
        elif state == "string":
            if char == "\\":
                index += 1
            elif char == quote:
                state = "code"
        elif char == "/" and next_char == "/":
            state = "line_comment"
            index += 1
        elif char == "/" and next_char == "*":
            state = "block_comment"
            index += 1
        elif char in {'"', "'"}:
            state = "string"
            quote = char
        elif char == "{":
            depth += 1
        elif char == "}":
            depth -= 1
            if depth == 0:
                return index
        index += 1
    return None


def function_body(source: str, name: str) -> str | None:
    opening = _find_function_open(source, name)
    if opening is None:
        return None
    closing = _matching_brace(source, opening)
    return None if closing is None else source[opening : closing + 1]


def _ordered_match(text: str, patterns: Iterable[str]) -> tuple[bool, str]:
    cursor = 0
    for ordinal, pattern in enumerate(patterns, start=1):
        match = re.search(pattern, text[cursor:], flags=re.MULTILINE | re.DOTALL)
        if match is None:
            return False, f"missing_or_misordered[{ordinal}]={pattern}"
        cursor += match.end()
    return True, "ordered anchors present"


def check_anchors(source: str) -> tuple[AnchorResult, ...]:
    results: list[AnchorResult] = []
    for spec in ANCHORS:
        target = source if spec.function is None else function_body(source, spec.function)
        if target is None:
            results.append(AnchorResult(spec.name, spec.function, False, "function missing"))
            continue
        passed, detail = _ordered_match(target, spec.ordered_patterns)
        if passed:
            for pattern, expected in spec.exact_counts:
                actual = len(re.findall(pattern, target, flags=re.MULTILINE | re.DOTALL))
                if actual != expected:
                    passed = False
                    detail = f"count mismatch pattern={pattern} expected={expected} actual={actual}"
                    break
        results.append(AnchorResult(spec.name, spec.function, passed, detail))
    return tuple(results)


def _load_manifest(path: Path) -> tuple[dict[str, Any], list[str]]:
    errors: list[str] = []
    try:
        manifest = json.loads(read_text(path))
    except (OSError, json.JSONDecodeError) as exc:
        return {}, [f"manifest_unreadable:{exc}"]
    if manifest.get("schema_version") != 3:
        errors.append("manifest_schema_invalid")
    required_roles = manifest.get("required_roles")
    components = manifest.get("components")
    evidence = manifest.get("evidence")
    runtime_config = manifest.get("runtime_config")
    runtime_selection = manifest.get("runtime_selection")
    runtime_evidence = manifest.get("runtime_evidence")
    production_contracts = manifest.get("production_contracts")
    legacy_exceptions = manifest.get("legacy_exceptions", {})
    if not isinstance(required_roles, list) or not all(isinstance(x, str) for x in required_roles):
        errors.append("manifest_roles_invalid")
    if not isinstance(components, dict):
        errors.append("manifest_components_invalid")
    if not isinstance(evidence, dict):
        errors.append("manifest_evidence_invalid")
    if not isinstance(runtime_config, str) or not runtime_config:
        errors.append("manifest_runtime_config_invalid")
    if not isinstance(runtime_selection, dict):
        errors.append("manifest_runtime_selection_invalid")
    if not isinstance(runtime_evidence, dict):
        errors.append("manifest_runtime_evidence_invalid")
    if not isinstance(production_contracts, dict):
        errors.append("manifest_production_contracts_invalid")
    if not isinstance(legacy_exceptions, dict):
        errors.append("manifest_legacy_exceptions_invalid")
    return manifest, errors


def _legacy_exception_valid(item: Any) -> bool:
    if not isinstance(item, dict):
        return False
    failure_markers = item.get("failure_markers")
    return (
        item.get("decision") == "permanent_legacy"
        and item.get("status") == "approved"
        and item.get("j5_stop") is True
        and isinstance(item.get("reason"), str)
        and bool(item.get("reason"))
        and isinstance(failure_markers, list)
        and len(failure_markers) >= 2
        and len(set(failure_markers)) == len(failure_markers)
        and all(isinstance(marker, str) and marker for marker in failure_markers)
        and isinstance(item.get("fallback_marker"), str)
        and bool(item.get("fallback_marker"))
        and item.get("fallback_status") == "pass"
    )


def evaluate(repo_root: Path, manifest_path: Path) -> GuardReport:
    manifest, manifest_errors = _load_manifest(manifest_path)
    legacy_rel = manifest.get(
        "legacy_source", "main/boards/ep-chat-p4-ml307/emotion_video_player.c"
    )
    legacy_path = repo_root / legacy_rel
    source = read_text(legacy_path) if legacy_path.exists() else ""
    anchor_results = check_anchors(source)
    anchors_pass = bool(source) and all(result.passed for result in anchor_results)

    required_roles = manifest.get("required_roles", [])
    raw_components = manifest.get("components", {})
    component_paths: dict[str, str | None] = {}
    runtime_selection: dict[str, str | None] = {}
    runtime_evidence_status: dict[str, str | None] = {}
    migrated_roles: list[str] = []
    production_split = bool(required_roles)
    production_contracts_ready = bool(required_roles)
    runtime_ready = bool(required_roles)
    raw_runtime_selection = manifest.get("runtime_selection", {})
    for role in required_roles:
        value = raw_components.get(role) if isinstance(raw_components, dict) else None
        component_paths[role] = value if isinstance(value, str) else None
        if not isinstance(value, str) or not value or not (repo_root / value).is_file():
            production_split = False
        elif value == legacy_rel:
            production_split = False
        else:
            migrated_roles.append(role)
        selected = (
            raw_runtime_selection.get(role)
            if isinstance(raw_runtime_selection, dict)
            else None
        )
        runtime_selection[role] = selected if isinstance(selected, str) else None
        if selected != "production":
            runtime_ready = False

    raw_production_contracts = manifest.get("production_contracts", {})
    for role in required_roles:
        contract = (
            raw_production_contracts.get(role, {})
            if isinstance(raw_production_contracts, dict)
            else {}
        )
        if (
            not isinstance(contract, dict)
            or not isinstance(contract.get("path"), str)
            or contract.get("path") != component_paths.get(role)
            or contract.get("status") != "pass"
            or not isinstance(contract.get("host_gate"), str)
            or not contract.get("host_gate")
        ):
            production_contracts_ready = False

    runtime_config_rel = manifest.get("runtime_config")
    runtime_config_path = (
        repo_root / runtime_config_rel
        if isinstance(runtime_config_rel, str)
        else None
    )
    runtime_config_source = (
        read_text(runtime_config_path)
        if runtime_config_path is not None and runtime_config_path.is_file()
        else ""
    )
    runtime_config_sync = bool(runtime_config_source)
    for role in required_roles:
        macro = RUNTIME_MACROS.get(role)
        selected = runtime_selection.get(role)
        expected = 1 if selected == "production" else 0
        match = (
            re.search(
                rf"(?m)^\s*#define\s+{re.escape(macro)}\s+([01])\s*$",
                runtime_config_source,
            )
            if macro is not None
            else None
        )
        if match is None or int(match.group(1)) != expected:
            runtime_config_sync = False

    present_legacy = tuple(name for name in LEGACY_FUNCTIONS if function_body(source, name))
    blockers = list(manifest_errors)
    closeout_blockers = list(manifest_errors)
    if not anchors_pass:
        blockers.append("legacy_semantic_anchor_mismatch")
        closeout_blockers.append("legacy_semantic_anchor_mismatch")
    if not production_split:
        blockers.append("g4_production_split_missing")
        closeout_blockers.append("g4_production_split_missing")
    if not production_contracts_ready:
        blockers.append("g4_production_contracts_invalid")
        closeout_blockers.append("g4_production_contracts_invalid")
    if not runtime_ready:
        blockers.append("g4_runtime_selection_incomplete")
    if not runtime_config_sync:
        blockers.append("g4_runtime_config_manifest_mismatch")
        closeout_blockers.append("g4_runtime_config_manifest_mismatch")
    evidence = manifest.get("evidence", {})
    for key, blocker in EVIDENCE_BLOCKERS.items():
        item = evidence.get(key, {}) if isinstance(evidence, dict) else {}
        if not isinstance(item, dict) or item.get("status") != "pass":
            blockers.append(blocker)
            closeout_blockers.append(blocker)
    runtime_evidence = manifest.get("runtime_evidence", {})
    legacy_exceptions = manifest.get("legacy_exceptions", {})
    certified_legacy_roles: list[str] = []
    for role in required_roles:
        item = (
            runtime_evidence.get(role, {})
            if isinstance(runtime_evidence, dict)
            else {}
        )
        status = item.get("status") if isinstance(item, dict) else None
        runtime_evidence_status[role] = status if isinstance(status, str) else None
        if status == "pass":
            continue
        if status == "fail":
            blockers.append(f"g4_{role}_runtime_evidence_failed")
        elif status == "pending":
            blockers.append(f"g4_{role}_runtime_evidence_pending")
        elif status is None:
            blockers.append(f"g4_{role}_runtime_evidence_missing")
        else:
            blockers.append(f"g4_{role}_runtime_evidence_invalid")

        selected = runtime_selection.get(role)
        if selected == "production":
            if status != "pass":
                closeout_blockers.append(
                    f"g5_{role}_selected_production_without_pass"
                )
        elif selected == "legacy":
            exception = (
                legacy_exceptions.get(role)
                if isinstance(legacy_exceptions, dict)
                else None
            )
            if status == "fail" and _legacy_exception_valid(exception):
                certified_legacy_roles.append(role)
            else:
                closeout_blockers.append(f"g5_{role}_legacy_exception_invalid")
        else:
            closeout_blockers.append(f"g5_{role}_runtime_selection_invalid")

    manifest_valid = not manifest_errors
    delete_ready = not blockers
    closeout_blockers = list(dict.fromkeys(closeout_blockers))
    certified_mixed_ready = bool(certified_legacy_roles) and not closeout_blockers
    governance_ready = delete_ready or certified_mixed_ready
    if not manifest_valid:
        phase = "invalid_manifest"
    elif not anchors_pass:
        phase = "legacy_drift"
    elif production_split and runtime_ready:
        phase = "dual_path"
    elif production_split:
        phase = "dual_path_guarded"
    elif migrated_roles:
        phase = "partial_split"
    else:
        phase = "legacy_guarded"
    return GuardReport(
        schema_version=3,
        phase=phase,
        semantic_anchors_pass=anchors_pass,
        manifest_valid=manifest_valid,
        production_split=production_split,
        production_contracts_ready=production_contracts_ready,
        runtime_ready=runtime_ready,
        runtime_config_sync=runtime_config_sync,
        g5_delete_ready=delete_ready,
        g5_certified_mixed_ready=certified_mixed_ready,
        g5_governance_ready=governance_ready,
        anchor_results=anchor_results,
        legacy_function_targets=present_legacy,
        component_paths=component_paths,
        runtime_selection=runtime_selection,
        runtime_evidence_status=runtime_evidence_status,
        migrated_roles=tuple(migrated_roles),
        certified_legacy_roles=tuple(certified_legacy_roles),
        blockers=tuple(dict.fromkeys(blockers)),
        closeout_blockers=tuple(closeout_blockers),
    )


def print_human(report: GuardReport) -> None:
    state = "PASS" if report.semantic_anchors_pass and report.manifest_valid else "FAIL"
    print(
        f"MJPEG_STRANGLER {state} phase={report.phase} "
        f"anchors={sum(x.passed for x in report.anchor_results)}/{len(report.anchor_results)} "
        f"migrated={len(report.migrated_roles)} "
        f"split={int(report.production_split)} contracts={int(report.production_contracts_ready)} "
        f"runtime_sync={int(report.runtime_config_sync)} "
        f"g5_delete_ready={int(report.g5_delete_ready)} "
        f"g5_mixed_ready={int(report.g5_certified_mixed_ready)} "
        f"g5_governance_ready={int(report.g5_governance_ready)}"
    )
    for result in report.anchor_results:
        if not result.passed:
            print(f"- {result.name}: {result.detail}")
    if report.blockers:
        print(f"- blockers: {', '.join(report.blockers)}")
    if report.closeout_blockers:
        print(f"- closeout_blockers: {', '.join(report.closeout_blockers)}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    default_root = Path(__file__).resolve().parents[2]
    parser.add_argument("--repo-root", type=Path, default=default_root)
    parser.add_argument(
        "--manifest",
        type=Path,
        default=Path(__file__).with_name("mjpeg_strangler_manifest.json"),
    )
    parser.add_argument("--json", action="store_true", dest="as_json")
    parser.add_argument("--require-g5-ready", action="store_true")
    parser.add_argument("--require-governance-ready", action="store_true")
    args = parser.parse_args()
    report = evaluate(args.repo_root.resolve(), args.manifest.resolve())
    if args.as_json:
        print(json.dumps(asdict(report), ensure_ascii=False, indent=2))
    else:
        print_human(report)
    healthy = report.semantic_anchors_pass and report.manifest_valid
    if args.require_g5_ready:
        healthy = healthy and report.g5_delete_ready
    if args.require_governance_ready:
        healthy = healthy and report.g5_governance_ready
    return 0 if healthy else 1


if __name__ == "__main__":
    sys.exit(main())
