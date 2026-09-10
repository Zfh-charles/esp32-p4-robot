#!/usr/bin/env python3
"""Architecture ratchet for first-party firmware code.

The guard accepts existing debt but rejects growth. Baseline increases are a
review decision; this tool intentionally has no --update-baseline option.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Iterable


SOURCE_SUFFIXES = {".c", ".cc", ".cpp", ".h", ".hpp"}
INCLUDE_RE = re.compile(r'^\s*#\s*include\s*[<"]([^>"]+)[>"]')


@dataclass(frozen=True)
class Finding:
    rule: str
    path: str
    actual: object
    limit: object
    message: str


@dataclass(frozen=True)
class Report:
    files: int
    lines: int
    hotspot_lines: int
    findings: tuple[Finding, ...]

    @property
    def passed(self) -> bool:
        return not self.findings


def read_text(path: Path) -> str:
    return path.read_text(encoding="utf-8", errors="replace")


def line_count(path: Path) -> int:
    with path.open("r", encoding="utf-8", errors="replace") as stream:
        return sum(1 for _ in stream)


def source_files(repo_root: Path) -> Iterable[Path]:
    main = repo_root / "main"
    if not main.is_dir():
        raise FileNotFoundError(f"missing source root: {main}")
    return (
        path
        for path in main.rglob("*")
        if path.is_file() and path.suffix.lower() in SOURCE_SUFFIXES
    )


def quoted_includes(text: str) -> set[str]:
    result: set[str] = set()
    for line in text.splitlines():
        match = INCLUDE_RE.match(line)
        if match:
            result.add(match.group(1).replace("\\", "/"))
    return result


def check(repo_root: Path, baseline: dict) -> Report:
    paths = list(source_files(repo_root))
    counts = {
        path.relative_to(repo_root).as_posix(): line_count(path) for path in paths
    }
    findings: list[Finding] = []

    legacy_limits = baseline["legacy_file_line_limits"]
    default_limit = int(baseline["default_new_file_max_lines"])
    for rel, actual in sorted(counts.items()):
        limit = int(legacy_limits.get(rel, default_limit))
        if actual > limit:
            findings.append(
                Finding(
                    "file-size-ratchet",
                    rel,
                    actual,
                    limit,
                    "extract a cohesive responsibility; do not raise the baseline",
                )
            )

    hotspots = baseline["hotspot_files"]
    missing_hotspots = [path for path in hotspots if path not in counts]
    hotspot_lines = sum(counts.get(path, 0) for path in hotspots)
    if missing_hotspots:
        findings.append(
            Finding(
                "hotspot-presence",
                ",".join(missing_hotspots),
                "missing",
                "present or baseline migration",
                "record an intentional strangler deletion before changing the baseline",
            )
        )
    if hotspot_lines > int(baseline["hotspot_total_max_lines"]):
        findings.append(
            Finding(
                "hotspot-total-ratchet",
                ",".join(hotspots),
                hotspot_lines,
                baseline["hotspot_total_max_lines"],
                "the three main hotspots may only shrink in aggregate",
            )
        )

    token = baseline["board_conditional_token"]
    for rel, limit in baseline["board_conditional_limits"].items():
        path = repo_root / rel
        actual = read_text(path).count(token) if path.exists() else 0
        if actual > int(limit):
            findings.append(
                Finding(
                    "core-board-conditional-ratchet",
                    rel,
                    actual,
                    limit,
                    "move board decisions behind a port/facade",
                )
            )

    application = repo_root / "main/application.cc"
    actual_board_includes = {
        include
        for include in quoted_includes(read_text(application))
        if include.startswith("boards/")
    }
    allowed_board_includes = set(baseline["allowed_application_board_includes"])
    for include in sorted(actual_board_includes - allowed_board_includes):
        findings.append(
            Finding(
                "core-board-dependency-ratchet",
                "main/application.cc",
                include,
                sorted(allowed_board_includes),
                "new board dependencies belong behind an inward-owned port",
            )
        )

    clean_roots = tuple(baseline["clean_layer_roots"])
    clean_limit = int(baseline["clean_layer_max_lines"])
    forbidden = tuple(baseline["clean_layer_forbidden_include_fragments"])
    for rel, actual in sorted(counts.items()):
        if not rel.startswith(clean_roots):
            continue
        if actual > clean_limit:
            findings.append(
                Finding(
                    "clean-layer-size",
                    rel,
                    actual,
                    clean_limit,
                    "keep policy modules small and cohesive",
                )
            )
        for include in sorted(quoted_includes(read_text(repo_root / rel))):
            hit = next((part for part in forbidden if part in include.lower()), None)
            if hit:
                findings.append(
                    Finding(
                        "clean-layer-dependency",
                        rel,
                        include,
                        "framework-free",
                        f"forbidden dependency fragment: {hit}",
                    )
                )

    return Report(
        files=len(counts),
        lines=sum(counts.values()),
        hotspot_lines=hotspot_lines,
        findings=tuple(findings),
    )


def print_human(report: Report) -> None:
    state = "PASS" if report.passed else "FAIL"
    print(
        f"ARCH_GUARD {state} files={report.files} lines={report.lines} "
        f"hotspot_lines={report.hotspot_lines} findings={len(report.findings)}"
    )
    for finding in report.findings:
        print(
            f"- {finding.rule}: {finding.path}: actual={finding.actual!r} "
            f"limit={finding.limit!r}; {finding.message}"
        )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    default_root = Path(__file__).resolve().parents[2]
    parser.add_argument("--repo-root", type=Path, default=default_root)
    parser.add_argument(
        "--baseline",
        type=Path,
        default=Path(__file__).with_name("architecture_baseline.json"),
    )
    parser.add_argument("--json", action="store_true", dest="as_json")
    args = parser.parse_args()

    baseline = json.loads(read_text(args.baseline))
    report = check(args.repo_root.resolve(), baseline)
    if args.as_json:
        payload = asdict(report)
        payload["passed"] = report.passed
        print(json.dumps(payload, ensure_ascii=False, indent=2))
    else:
        print_human(report)
    return 0 if report.passed else 1


if __name__ == "__main__":
    sys.exit(main())

