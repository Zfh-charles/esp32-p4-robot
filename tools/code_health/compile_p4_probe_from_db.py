#!/usr/bin/env python3
"""Compile one non-flashable P4 probe with a compile_commands template."""

from __future__ import annotations

import argparse
import json
import subprocess
import sys
from pathlib import Path


def under(path: Path, parent: Path) -> bool:
    try:
        path.relative_to(parent)
        return True
    except ValueError:
        return False


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo-root", type=Path, default=Path.cwd())
    parser.add_argument("--template", type=Path, required=True)
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--define", action="append", default=[])
    args = parser.parse_args()

    root = args.repo_root.resolve()
    database_path = root / "build" / "compile_commands.json"
    template = (root / args.template).resolve()
    source = (root / args.source).resolve()
    output = (root / args.output).resolve()
    build_root = (root / "build").resolve()
    if not under(template, root) or not under(source, root):
        raise SystemExit("template/source must stay inside repository")
    if not under(output, build_root):
        raise SystemExit("probe output must stay inside build/")
    if not template.is_file() or not source.is_file():
        raise SystemExit("template/source missing")

    entries = json.loads(database_path.read_text(encoding="utf-8"))
    entry = next(
        (item for item in entries if Path(item["file"]).resolve() == template),
        None,
    )
    if entry is None:
        raise SystemExit(f"compile template missing: {template}")
    command = entry["command"]
    old_output = entry.get("output")
    if str(template) not in command or not old_output or f"-o {old_output}" not in command:
        raise SystemExit("compile command shape changed")

    output.parent.mkdir(parents=True, exist_ok=True)
    source_arg = source.as_posix()
    output_arg = output.as_posix()
    include_arg = source.parent.as_posix()
    command = command.replace(str(template), source_arg, 1)
    command = command.replace(f"-o {old_output}", f"-o {output_arg}", 1)
    additions = [f"-I{include_arg}"] + [f"-D{value}" for value in args.define]
    command = command.replace(" -c ", " " + " ".join(additions) + " -c ", 1)
    compiler, response_arguments = command.split(" ", 1)
    response_path = output.with_suffix(output.suffix + ".rsp")
    response_path.write_text(response_arguments, encoding="utf-8")
    try:
        completed = subprocess.run(
            [compiler, f"@{response_path}"], cwd=root, shell=False
        )
        if completed.returncode != 0:
            return completed.returncode
    finally:
        response_path.unlink(missing_ok=True)
    print(f"P4_PROBE_COMPILE PASS source={source.name} output={output}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
