#!/usr/bin/env python3
"""Validate and execute the six host-only T2 replay scenarios."""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path
from typing import Any, Sequence

HERE = Path(__file__).resolve().parent
REPO_ROOT = HERE.parents[2]
if str(REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(REPO_ROOT))

from tools.validation.contracts.validate_trace import validate_trace  # noqa: E402
from tools.validation.replay.core.runner import run_trace  # noqa: E402

EXPECTED_SCENARIOS = {
    "cold_boot_first_round",
    "exit_session",
    "long_tts",
    "network_recovery",
    "same_emotion_cross_generation",
    "second_wake",
}


def default_paths() -> list[Path]:
    return sorted((HERE / "scenarios").glob("*.json"))


def run_paths(paths: Sequence[Path]) -> dict[str, Any]:
    results: list[dict[str, Any]] = []
    for path in paths:
        try:
            trace = json.loads(path.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError) as error:
            results.append({"scenario": path.stem, "passed": False, "errors": [f"read/parse: {error}"]})
            continue
        errors = validate_trace(trace)
        if errors:
            results.append({"scenario": path.stem, "trace_id": trace.get("trace_id"), "passed": False,
                            "errors": [f"contract: {error}" for error in errors]})
            continue
        replay = run_trace(trace)
        results.append({
            "scenario": path.stem,
            "trace_id": trace["trace_id"],
            "passed": replay.passed,
            "errors": list(replay.errors),
            "oracle_results": list(replay.oracle_results),
        })
    names = {item["scenario"] for item in results}
    missing = sorted(EXPECTED_SCENARIOS - names)
    unexpected = sorted(names - EXPECTED_SCENARIOS)
    suite_errors = []
    if missing:
        suite_errors.append(f"missing scenarios: {', '.join(missing)}")
    if unexpected:
        suite_errors.append(f"unexpected scenarios: {', '.join(unexpected)}")
    passed = not suite_errors and bool(results) and all(item["passed"] for item in results)
    return {
        "schema_version": 1,
        "suite": "p4-t2-six-scenarios-v1",
        "overall_status": "PASS" if passed else "FAIL",
        "proof_boundary": "T2 system model only; not production-path, RTOS, hardware, or product-feel evidence",
        "suite_errors": suite_errors,
        "counts": {"PASS": sum(item["passed"] for item in results),
                   "FAIL": sum(not item["passed"] for item in results)},
        "results": results,
    }


def write_report(path: Path, report: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + ".tmp")
    temporary.write_text(json.dumps(report, ensure_ascii=False, indent=2, sort_keys=True) + "\n",
                         encoding="utf-8")
    temporary.replace(path)


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("paths", nargs="*", type=Path)
    parser.add_argument("--report", type=Path, default=HERE / "reports" / "latest-t2.json")
    parser.add_argument("--quiet", action="store_true")
    args = parser.parse_args(argv)
    report = run_paths(args.paths or default_paths())
    write_report(args.report, report)
    if not args.quiet:
        print(f"T2_REPLAY {report['overall_status']} pass={report['counts']['PASS']} fail={report['counts']['FAIL']}")
        for item in report["results"]:
            print(f"- {item['scenario']}: {'PASS' if item['passed'] else 'FAIL'}")
            for error in item["errors"]:
                print(f"  {error}")
        for error in report["suite_errors"]:
            print(f"- suite: {error}")
        print(f"report={args.report}")
    return 0 if report["overall_status"] == "PASS" else 1


if __name__ == "__main__":
    raise SystemExit(main())
