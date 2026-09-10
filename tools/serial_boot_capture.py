#!/usr/bin/env python3
"""Capture full serial boot log with reset analysis. Waits for port if absent."""

import re
import sys
import time
from datetime import datetime
from pathlib import Path

import serial

sys.stdout.reconfigure(encoding="utf-8", errors="replace")

PORT = sys.argv[1] if len(sys.argv) > 1 else "COM6"
SECONDS = float(sys.argv[2]) if len(sys.argv) > 2 else 120.0
WAIT_PORT_SEC = float(sys.argv[3]) if len(sys.argv) > 3 else 90.0

OUT_DIR = Path(__file__).resolve().parent / "logs"
OUT_DIR.mkdir(exist_ok=True)
STAMP = datetime.now().strftime("%Y%m%d_%H%M%S")
OUT_FILE = OUT_DIR / f"boot_{PORT}_{STAMP}.log"

HIGHLIGHT = re.compile(
    r"(BootTrace|ReminderTrace|FW_MARKER|WDT|Guru|panic|HP_SYS|REBOOT_AFTER|CRASH|SUMMARY|ANOMALY|Flash standby|AfeWakeWord|AFE:)"
)


def wait_for_port(port: str, timeout: float) -> serial.Serial:
    deadline = time.time() + timeout
    last_err = None
    while time.time() < deadline:
        try:
            ser = serial.Serial(port, 115200, timeout=0.2)
            return ser
        except serial.SerialException as e:
            last_err = e
            time.sleep(0.5)
    raise serial.SerialException(f"Port {port} not available after {timeout}s: {last_err}")


def analyze(path: Path) -> str:
    text = path.read_text(encoding="utf-8", errors="replace")
    resets = len(re.findall(r"rst:0x", text))
    wdt = len(re.findall(r"HP_SYS_HP_WDT_RESET|TASK_WDT|INT_WDT", text))
    panic = len(re.findall(r"Guru Meditation", text))
    boot_markers = re.findall(r"BootTrace: FW_MARKER (\S+)", text)
    reboot_after = re.findall(r"BootTrace: REBOOT_AFTER \| reason=(\S+) prev_phase=(\S+) prev_t=(\d+)ms", text)
    phases = re.findall(r"BootTrace: PH \| t=(\d+)ms phase=(\S+)", text)
    last_phase_before_reset = []
    chunks = text.split("rst:0x")
    for chunk in chunks[1:]:
        hits = re.findall(r"BootTrace: PH \| t=\d+ms phase=(\S+)", chunk)
        if hits:
            last_phase_before_reset.append(hits[-1])
    lines = [
        f"file={path.name}",
        f"resets={resets} wdt={wdt} panic={panic}",
        f"fw_markers={boot_markers[-3:] if boot_markers else []}",
        f"reboot_after={reboot_after[-3:] if reboot_after else []}",
        f"last_phases_before_reset={last_phase_before_reset[-5:]}",
        f"final_phases={phases[-8:] if phases else []}",
    ]
    return "\n".join(lines)


def main() -> int:
    print(f"Waiting for {PORT} (up to {WAIT_PORT_SEC}s)...", flush=True)
    ser = wait_for_port(PORT, WAIT_PORT_SEC)
    print(f"Connected. Capturing {SECONDS}s -> {OUT_FILE}", flush=True)

    ser.setDTR(False)
    ser.setRTS(True)
    time.sleep(0.08)
    ser.setRTS(False)
    time.sleep(0.05)

    end = time.time() + SECONDS
    buf = b""
    with OUT_FILE.open("w", encoding="utf-8", errors="replace") as f:
        f.write(f"# capture {PORT} {datetime.now().isoformat()}\n")
        while time.time() < end:
            data = ser.read(4096)
            if not data:
                continue
            buf += data
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                text = line.decode("utf-8", errors="replace").rstrip("\r")
                f.write(text + "\n")
                f.flush()
                prefix = ">> " if HIGHLIGHT.search(text) else "   "
                print(prefix + text, flush=True)

    ser.close()
    summary = analyze(OUT_FILE)
    summary_path = OUT_FILE.with_suffix(".summary.txt")
    summary_path.write_text(summary, encoding="utf-8")
    print("\n=== ANALYSIS ===")
    print(summary)
    print(f"=== saved {OUT_FILE} ===")
    return 0


if __name__ == "__main__":
    sys.exit(main())
