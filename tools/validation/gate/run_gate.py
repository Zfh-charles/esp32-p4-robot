#!/usr/bin/env python3
"""Run manifest-defined P4 host checks and emit one stable JSON report."""

from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
import time
from pathlib import Path
from typing import Any, Mapping, Sequence


STATUSES = ("FAIL", "PASS", "SKIP", "TIMEOUT")
MAX_CAPTURE_CHARS = 16_000


def read_json(path: Path) -> dict[str, Any]:
    with path.open("r", encoding="utf-8") as stream:
        value = json.load(stream)
    if not isinstance(value, dict):
        raise ValueError(f"JSON root must be an object: {path}")
    return value


def normalize_capture(value: str | bytes | None) -> str:
    if value is None:
        return ""
    if isinstance(value, bytes):
        value = value.decode("utf-8", errors="replace")
    value = value.replace("\r\n", "\n").replace("\r", "\n")
    if len(value) <= MAX_CAPTURE_CHARS:
        return value
    omitted = len(value) - MAX_CAPTURE_CHARS
    return value[:MAX_CAPTURE_CHARS] + f"\n... <truncated {omitted} chars>"


def resolve_command(tokens: object, repo_root: Path) -> list[str]:
    if not isinstance(tokens, list) or not tokens or not all(isinstance(x, str) for x in tokens):
        raise ValueError("command must be a non-empty string array")
    variables = {"python": sys.executable, "repo": str(repo_root)}
    try:
        return [token.format_map(variables) for token in tokens]
    except KeyError as error:
        raise ValueError(f"unknown command variable: {error.args[0]}") from error


def skipped(check: Mapping[str, Any], reason: str) -> dict[str, Any]:
    return {
        "id": check["id"],
        "label": check["label"],
        "tier": check["tier"],
        "required": bool(check.get("required", True)),
        "status": "SKIP",
        "exit_code": None,
        "duration_ms": 0,
        "command": [],
        "reason": reason,
        "stdout": "",
        "stderr": "",
    }


def run_check(check: Mapping[str, Any], repo_root: Path) -> dict[str, Any]:
    required = bool(check.get("required", True))
    when_path = check.get("when_path")
    if when_path and not (repo_root / str(when_path)).exists():
        if bool(check.get("optional_when_missing", False)):
            return skipped(check, f"optional path missing: {when_path}")
        return {
            **skipped(check, f"required path missing: {when_path}"),
            "status": "FAIL",
            "required": required,
        }

    command = resolve_command(check["command"], repo_root)
    timeout = check["timeout_seconds"]
    started = time.monotonic()
    common = {
        "id": check["id"],
        "label": check["label"],
        "tier": check["tier"],
        "required": required,
        "exit_code": None,
        "command": command,
        "stdout": "",
        "stderr": "",
    }
    environment = os.environ.copy()
    environment.setdefault("PYTHONUTF8", "1")
    environment.setdefault("PYTHONIOENCODING", "utf-8")
    environment.setdefault("PYTHONDONTWRITEBYTECODE", "1")
    try:
        completed = subprocess.run(
            command,
            cwd=repo_root,
            env=environment,
            stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            encoding="utf-8",
            errors="replace",
            timeout=float(timeout),
            check=False,
        )
        passed = completed.returncode == 0
        return {
            **common,
            "status": "PASS" if passed else "FAIL",
            "exit_code": completed.returncode,
            "duration_ms": round((time.monotonic() - started) * 1000),
            "reason": "exit code 0" if passed else f"exit code {completed.returncode}",
            "stdout": normalize_capture(completed.stdout),
            "stderr": normalize_capture(completed.stderr),
        }
    except subprocess.TimeoutExpired as error:
        return {
            **common,
            "status": "TIMEOUT",
            "duration_ms": round((time.monotonic() - started) * 1000),
            "reason": f"exceeded {timeout} seconds",
            "stdout": normalize_capture(error.stdout),
            "stderr": normalize_capture(error.stderr),
        }
    except OSError as error:
        return {
            **common,
            "status": "FAIL",
            "duration_ms": round((time.monotonic() - started) * 1000),
            "reason": f"unable to start: {error}",
        }


def execute_manifest(manifest: Mapping[str, Any], scope: str, repo_root: Path) -> dict[str, Any]:
    if manifest.get("schema_version") != 1:
        raise ValueError("manifest schema_version must be 1")
    checks = manifest.get("checks")
    if not isinstance(checks, list):
        raise ValueError("manifest checks must be an array")
    selected: list[dict[str, Any]] = []
    seen: set[str] = set()
    required_fields = ("id", "label", "tier", "scopes", "timeout_seconds", "command")
    for index, raw in enumerate(checks):
        if not isinstance(raw, dict):
            raise ValueError(f"checks[{index}] must be an object")
        missing = [field for field in required_fields if field not in raw]
        if missing:
            raise ValueError(f"checks[{index}] missing fields: {', '.join(missing)}")
        check_id = raw["id"]
        if not isinstance(check_id, str) or not check_id or check_id in seen:
            raise ValueError(f"checks[{index}].id must be unique and non-empty")
        seen.add(check_id)
        timeout = raw["timeout_seconds"]
        if isinstance(timeout, bool) or not isinstance(timeout, (int, float)) or timeout <= 0:
            raise ValueError(f"checks[{index}].timeout_seconds must be positive")
        resolve_command(raw["command"], repo_root)
        if scope in raw["scopes"]:
            selected.append(raw)
    if not selected:
        raise ValueError(f"scope selects no checks: {scope}")

    results = [run_check(check, repo_root) for check in selected]
    failed = [
        result["id"]
        for result in results
        if result["required"] and result["status"] in ("FAIL", "TIMEOUT")
    ]
    return {
        "schema_version": 1,
        "manifest_version": str(manifest.get("manifest_version", "unspecified")),
        "scope": scope,
        "overall_status": "FAIL" if failed else "PASS",
        "failed_required": failed,
        "counts": {status: sum(r["status"] == status for r in results) for status in STATUSES},
        "results": results,
    }


def write_report(path: Path, report: Mapping[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + ".tmp")
    temporary.write_text(
        json.dumps(report, ensure_ascii=False, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    temporary.replace(path)


def main(argv: Sequence[str] | None = None) -> int:
    here = Path(__file__).resolve().parent
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--manifest", type=Path, default=here / "gate_manifest.json")
    parser.add_argument("--repo-root", type=Path, default=here.parents[2])
    parser.add_argument("--scope", default="fast")
    parser.add_argument("--report", type=Path)
    parser.add_argument("--quiet", action="store_true")
    args = parser.parse_args(argv)
    report_path = args.report or (here / "reports" / f"latest-{args.scope}.json")
    try:
        report = execute_manifest(read_json(args.manifest), args.scope, args.repo_root.resolve())
        write_report(report_path, report)
    except (OSError, ValueError, json.JSONDecodeError) as error:
        print(f"VALIDATION_GATE CONFIG_ERROR: {error}", file=sys.stderr)
        return 2
    if not args.quiet:
        counts = report["counts"]
        print(
            f"VALIDATION_GATE {report['overall_status']} scope={args.scope} "
            f"pass={counts['PASS']} fail={counts['FAIL']} "
            f"timeout={counts['TIMEOUT']} skip={counts['SKIP']}"
        )
        for result in report["results"]:
            print(
                f"- {result['tier']} {result['id']}: {result['status']} "
                f"({result['duration_ms']}ms; {result['reason']})"
            )
        print(f"report={report_path}")
    return 0 if report["overall_status"] == "PASS" else 1


if __name__ == "__main__":
    sys.exit(main())
