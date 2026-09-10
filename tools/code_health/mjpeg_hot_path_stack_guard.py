#!/usr/bin/env python3
"""Measure MJPEG hot-path stack frames from a P4 ELF before runtime flip."""

from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
from pathlib import Path


DECODER_CHAIN = (
    "hw_decode_frame_optimized",
    "mjpeg_decoder_player_adapter_run",
    "mjpeg_decoder_stage_run",
    "mjpeg_decoder_stage_decode",
    "process_frame",
)
DECODER_DIRECT_CALLER = "hw_decode_frame_optimized"
DECODER_DIRECT_ENTRY = "mjpeg_decoder_player_decode_direct"
DECODER_DIRECT_FORBIDDEN_CALLEES = (
    "mjpeg_decoder_player_adapter_run",
    "mjpeg_decoder_stage_run",
    "mjpeg_decoder_stage_decode",
    "process_frame",
)
CLOCK_ADAPTER = "mjpeg_playback_clock_player_poll"
CLOCK_PRODUCTION = "mjpeg_playback_clock_poll"
CLOCK_LEGACY = "mjpeg_playback_clock_legacy_poll"
CLOCK_DIRECT_POLL = "mjpeg_playback_clock_player_poll_direct"
CLOCK_DIRECT_RESULT = "mjpeg_playback_clock_player_on_decode_result_direct"
CLOCK_DIRECT_FORBIDDEN_CALLEES = (
    "mjpeg_playback_clock_poll",
    "mjpeg_playback_clock_legacy_poll",
    "mjpeg_playback_clock_on_decode_result",
    "mjpeg_playback_clock_legacy_on_decode_result",
)

DEFAULT_BUDGETS = {
    "decoder": 208,  # legacy 144B + at most 64B specialization overhead
    "decoder-direct": 208,  # caller plus one <=64B direct ESP specialization
    "clock": 160,    # current stable adapter 128B + legacy body 32B
    "clock-direct": 160,  # one direct owner-binding entry at a time
}

NM_LINE = re.compile(
    r"^([0-9a-fA-F]+)\s+([0-9a-fA-F]+)\s+\w\s+(\S+)$"
)
STACK_ADJUST = re.compile(r"\baddi\s+sp\s*,\s*sp\s*,\s*-(\d+)\b")


def run_text(command: list[str]) -> str:
    completed = subprocess.run(
        command,
        check=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        encoding="utf-8",
        errors="replace",
    )
    return completed.stdout


def load_symbols(nm: Path, elf: Path) -> dict[str, tuple[int, int]]:
    symbols: dict[str, tuple[int, int]] = {}
    for line in run_text([str(nm), "-S", str(elf)]).splitlines():
        match = NM_LINE.match(line.strip())
        if match:
            symbols[match.group(3)] = (
                int(match.group(1), 16),
                int(match.group(2), 16),
            )
    return symbols


def disassemble_symbol(
    objdump: Path,
    elf: Path,
    symbols: dict[str, tuple[int, int]],
    name: str,
) -> str:
    if name not in symbols:
        raise KeyError(f"symbol missing: {name}")
    by_symbol = run_text(
        [str(objdump), "-d", f"--disassemble={name}", str(elf)]
    )
    if re.search(rf"<{re.escape(name)}>:\s*$", by_symbol, re.MULTILINE):
        return by_symbol
    # Linked P4 ELFs from older toolchains may not honor --disassemble for a
    # local symbol. Address fallback is safe there because linked sections do
    # not all start at zero; relocatable objects are handled above by name.
    address, size = symbols[name]
    return run_text(
        [
            str(objdump),
            "-d",
            f"--start-address={address}",
            f"--stop-address={address + size}",
            str(elf),
        ]
    )


def stack_frame_bytes(disassembly: str) -> int:
    match = STACK_ADJUST.search(disassembly)
    return int(match.group(1)) if match else 0


def evaluate_frames(
    role: str,
    frames: dict[str, int],
    production_clock_calls_legacy: bool = False,
    direct_clock_calls_stage: bool = False,
    direct_decoder_calls_stage: bool = False,
    direct_decoder_indirect_call: bool = False,
    budget: int | None = None,
) -> dict:
    limit = DEFAULT_BUDGETS[role] if budget is None else budget
    if role == "decoder":
        required = DECODER_CHAIN
    elif role == "decoder-direct":
        required = (DECODER_DIRECT_CALLER, DECODER_DIRECT_ENTRY)
    elif role == "clock":
        required = (CLOCK_ADAPTER, CLOCK_PRODUCTION)
        if production_clock_calls_legacy:
            required += (CLOCK_LEGACY,)
    elif role == "clock-direct":
        required = (CLOCK_DIRECT_POLL, CLOCK_DIRECT_RESULT)
    else:
        raise ValueError(f"unknown role: {role}")
    missing = [name for name in required if name not in frames]
    observed = (
        max((frames.get(name, 0) for name in required), default=0)
        if role == "clock-direct"
        else sum(frames.get(name, 0) for name in required)
    )
    forbidden_clock_trampoline = bool(
        role == "clock" and production_clock_calls_legacy
    )
    forbidden_direct_stage_call = bool(
        role == "clock-direct" and direct_clock_calls_stage
    )
    decoder_direct_frame = frames.get(DECODER_DIRECT_ENTRY, 0)
    forbidden_decoder_stage_call = bool(
        role == "decoder-direct" and direct_decoder_calls_stage
    )
    forbidden_decoder_indirect_call = bool(
        role == "decoder-direct" and direct_decoder_indirect_call
    )
    decoder_direct_frame_exceeds_budget = bool(
        role == "decoder-direct" and decoder_direct_frame > 64
    )
    status = (
        "PASS"
        if not missing and observed <= limit and not forbidden_clock_trampoline
        and not forbidden_direct_stage_call
        and not forbidden_decoder_stage_call
        and not forbidden_decoder_indirect_call
        and not decoder_direct_frame_exceeds_budget
        else "FAIL"
    )
    return {
        "status": status,
        "role": role,
        "budget_bytes": limit,
        "observed_bytes": observed,
        "chain": list(required),
        "frames": {name: frames.get(name) for name in required},
        "missing": missing,
        "production_clock_calls_legacy": production_clock_calls_legacy,
        "forbidden_clock_trampoline": forbidden_clock_trampoline,
        "direct_clock_calls_stage": direct_clock_calls_stage,
        "forbidden_direct_stage_call": forbidden_direct_stage_call,
        "direct_decoder_calls_stage": direct_decoder_calls_stage,
        "forbidden_decoder_stage_call": forbidden_decoder_stage_call,
        "direct_decoder_indirect_call": direct_decoder_indirect_call,
        "forbidden_decoder_indirect_call": forbidden_decoder_indirect_call,
        "decoder_direct_frame_bytes": decoder_direct_frame,
        "decoder_direct_frame_budget_bytes": 64 if role == "decoder-direct" else None,
        "decoder_direct_frame_exceeds_budget": decoder_direct_frame_exceeds_budget,
    }


def analyze(
    role: str,
    elf: Path,
    nm: Path,
    objdump: Path,
    budget: int | None = None,
) -> dict:
    symbols = load_symbols(nm, elf)
    if role == "decoder":
        names = DECODER_CHAIN
    elif role == "decoder-direct":
        names = (DECODER_DIRECT_CALLER, DECODER_DIRECT_ENTRY)
    elif role == "clock-direct":
        names = (CLOCK_DIRECT_POLL, CLOCK_DIRECT_RESULT)
    else:
        names = (CLOCK_ADAPTER, CLOCK_PRODUCTION, CLOCK_LEGACY)
    frames: dict[str, int] = {}
    disassembly: dict[str, str] = {}
    for name in names:
        if name not in symbols:
            continue
        text = disassemble_symbol(objdump, elf, symbols, name)
        disassembly[name] = text
        frames[name] = stack_frame_bytes(text)
    clock_calls_legacy = bool(
        role == "clock"
        and re.search(
            rf"<\s*{re.escape(CLOCK_LEGACY)}(?:\+[^>]*)?>",
            disassembly.get(CLOCK_PRODUCTION, ""),
        )
    )
    direct_calls_stage = bool(
        role == "clock-direct"
        and any(
            re.search(
                rf"<\s*{re.escape(callee)}(?:\+[^>]*)?>",
                disassembly.get(name, ""),
            )
            for name in names
            for callee in CLOCK_DIRECT_FORBIDDEN_CALLEES
        )
    )
    decoder_calls_stage = bool(
        role == "decoder-direct"
        and any(
            re.search(
                rf"<\s*{re.escape(callee)}(?:\+[^>]*)?>",
                disassembly.get(DECODER_DIRECT_ENTRY, ""),
            )
            for callee in DECODER_DIRECT_FORBIDDEN_CALLEES
        )
    )
    decoder_indirect_call = bool(
        role == "decoder-direct"
        and any(
            "jalr" in line and "<" not in line and "ret" not in line
            for line in disassembly.get(DECODER_DIRECT_ENTRY, "").splitlines()
        )
    )
    report = evaluate_frames(
        role,
        frames,
        clock_calls_legacy,
        direct_calls_stage,
        decoder_calls_stage,
        decoder_indirect_call,
        budget,
    )
    report.update({"schema_version": 1, "elf": str(elf)})
    return report


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("elf", type=Path)
    parser.add_argument(
        "--role",
        choices=("decoder", "decoder-direct", "clock", "clock-direct"),
        required=True,
    )
    parser.add_argument("--nm", type=Path, required=True)
    parser.add_argument("--objdump", type=Path, required=True)
    parser.add_argument("--budget-bytes", type=int)
    args = parser.parse_args()
    result = analyze(
        args.role,
        args.elf.resolve(),
        args.nm.resolve(),
        args.objdump.resolve(),
        args.budget_bytes,
    )
    print(json.dumps(result, ensure_ascii=False, indent=2))
    return 0 if result["status"] == "PASS" else 1


if __name__ == "__main__":
    sys.exit(main())
