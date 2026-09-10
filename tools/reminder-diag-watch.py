#!/usr/bin/env python3
"""
ReminderTrace one-pass analyzer — mic/spk/UI/LC lifecycle.

Usage:
  python tools/reminder-diag-watch.py COM6
  python tools/reminder-diag-watch.py COM6 --file capture.log

Test plan (3 steps, one flash):
  1. Say wake word -> chat -> wait for idle (expect LC_SUMMARY User PASS + Standby PASS)
  2. Wait for poll alert (expect LC_SUMMARY Proactive PASS)
  3. Say wake word again (expect Standby wake_arm=OK)

Ctrl+C prints verdict with ROOT_CAUSE — no guessing needed.
"""
from __future__ import annotations

import argparse
import re
import sys
import time
from collections import deque
from dataclasses import dataclass, field
from pathlib import Path

try:
    import serial
except ImportError:
    serial = None  # type: ignore

ROOT_CAUSE_HINTS = {
    "wake_not_armed_after_idle_rearm": "idle 恢复后麦克风/唤醒词未就绪 — 查 EnterIdleStandby 是否重复调用或 I2S 路由冲突",
    "tts_json_ok_but_no_audio_packets": "云端发了 TTS 文本但设备未收到/未播放音频 — 查 state=speaking 与 route=Playback",
    "tts_audio_dropped_by_state_machine": "音频包被丢弃 — device_state 不是 speaking 且 route 不是 Playback",
    "channel_not_open": "MQTT/WS 音频通道未打开",
    "wake_not_sent": "主动告警未发送 wake 指令",
    "ack_missing_or_cancelled": "告警未完成 ack（超时/取消/无 TTS）",
    "channel_open_failed": "用户唤醒后通道打开失败",
    "ui_not_updated": "告警文案未显示到屏幕",
    "no_tts_json_from_server": "服务端未下发 TTS start",
}

ANOMALY_HINTS = {
    "W001": "待机应开唤醒词(wake=1)但未开",
    "W002": "待机应开麦克风(in=1)但未开",
    "W003": "语音采集中麦克风被关",
    "W004": "唤醒词在 Playback 路由",
    "W005": "待机 hold=1 未释放",
    "W006": "Proactive session 孤儿状态",
    "W007": "listening 但 wake/voice 均未运行",
    "W008": "Capture 模式麦克风被关",
    "W009": "有播放队列但喇叭未开",
    "W010": "pending 提醒但 session=None",
    "W011": "idle 但 session 仍为 User",
}

LC_SUMMARY_RE = re.compile(
    r"LC_SUMMARY \| owner=(?P<owner>\w+).*?RESULT=(?P<result>PASS|FAIL)"
    r"(?:.*?ROOT_CAUSE=(?P<cause>[^\s|]+))?"
)
LC_DIAG_RE = re.compile(r"LC_DIAG \| owner=(?P<owner>\w+).*?ROOT_CAUSE=(?P<cause>\S+)")
HW_RE = re.compile(
    r"HW \| mic=(?P<mic>-?\d+) spk=(?P<spk>-?\d+)(?: route=(?P<route>\w+))?"
)
HW_SNAP_RE = re.compile(
    r"HW_SNAP \| tag=(?P<tag>\S+).*?mic=(?P<mic>-?\d+) spk=(?P<spk>-?\d+) route=(?P<route>\S+)"
    r".*?wake=(?P<wake>-?\d+).*?ui_text=(?P<text>[^\s|]+)"
)


@dataclass
class LcResult:
    owner: str
    result: str
    cause: str
    raw: str
    ts: float = 0.0


@dataclass
class DiagStats:
    lines: int = 0
    anomalies: list[str] = field(default_factory=list)
    lc_results: list[LcResult] = field(default_factory=list)
    hw_mic: int = -1
    hw_spk: int = -1
    hw_route: str = "?"
    power_mic_off: int = 0
    tts_dropped: int = 0
    ui_text_count: int = 0
    ui_screen_count: int = 0
    spk_pcm_first: bool = False
    tts_decoded: int = 0
    poll_new: int = 0
    wake_blocks: int = 0
    recent: deque = field(default_factory=lambda: deque(maxlen=16))


def hint_for(cause: str) -> str:
    return ROOT_CAUSE_HINTS.get(cause, cause)


def handle_line(line: str, stats: DiagStats, verbose: bool) -> None:
    line = line.strip()
    if not line or "ReminderTrace" not in line:
        return
    stats.lines += 1
    now = time.time()
    stats.recent.append(line[:160])

    if "ANOMALY" in line:
        stats.anomalies.append(line)
        for code, hint in ANOMALY_HINTS.items():
            if code in line:
                print(f"\n*** [{code}] {hint}")
                break
        print(f"  {line}")
        return

    m = LC_SUMMARY_RE.search(line)
    if m:
        cause = m.group("cause") or "-"
        lr = LcResult(m.group("owner"), m.group("result"), cause, line, now)
        stats.lc_results.append(lr)
        icon = "OK" if lr.result == "PASS" else "FAIL"
        print(f"\n=== LC [{icon}] owner={lr.owner} RESULT={lr.result} ROOT_CAUSE={lr.cause}")
        if lr.result == "FAIL" and lr.cause not in ("-", ""):
            print(f"    -> {hint_for(lr.cause)}")
        if verbose:
            print(f"    {line}")
        return

    m = LC_DIAG_RE.search(line)
    if m:
        print(f"\n>>> LC_DIAG owner={m.group('owner')} -> {hint_for(m.group('cause'))}")
        return

    m = HW_SNAP_RE.search(line)
    if m:
        print(
            f"\n--- SNAP [{m.group('tag')}] mic={m.group('mic')} spk={m.group('spk')} "
            f"route={m.group('route')} wake={m.group('wake')} text={m.group('text')}"
        )
        return

    m = HW_RE.search(line)
    if m:
        if m.group("mic") is not None:
            stats.hw_mic = int(m.group("mic"))
        if m.group("spk") is not None:
            stats.hw_spk = int(m.group("spk"))
        if m.group("route"):
            stats.hw_route = m.group("route")
        if "spk_pcm=" in line:
            stats.spk_pcm_first = True
            print(f"\n>>> SPEAKER PCM: {line[line.find('HW |'):][:100]}")
        if "tts_pipe stage=decoded ok=1" in line:
            stats.tts_decoded += 1
        if "tts_pipe" in line and "ok=0" in line:
            print(f"\n!!! TTS PIPE FAIL: {line[line.find('HW |'):][:100]}")
        if "power_timeout" in line or "power_input_off" in line:
            stats.power_mic_off += 1
            print(f"\n!!! MIC POWER OFF (count={stats.power_mic_off}): {line[:120]}")
        elif verbose and ("route=" in line or "spk_op=" in line):
            print(f"HW: {line[line.find('HW |'):][:100]}")
        return

    if "UI | kind=screen" in line and "applied=1" in line:
        stats.ui_screen_count += 1
        if verbose:
            print(line[line.find("UI |"):][:120])
        return

    if "UI | kind=text" in line and "applied=1" in line:
        stats.ui_text_count += 1
        if verbose:
            print(line[line.find("UI |"):][:120])
        return

    if "UI | kind=tts" in line and "audio_dropped" in line:
        stats.tts_dropped += 1
        print(f"\n!!! TTS AUDIO DROPPED #{stats.tts_dropped}: {line[line.find('UI |'):][:120]}")
        return

    if "poll_new" in line:
        stats.poll_new += 1
        print(f"\n+++ POLL_NEW: {line}")
        return

    if "wake_word_blocked" in line:
        stats.wake_blocks += 1
        print(f"\n!!! WAKE BLOCKED: {line}")
        return

    if verbose and any(k in line for k in ("LC |", "deliver_", "enter_idle", "proactive_")):
        print(line[:140])


def print_report(stats: DiagStats) -> None:
    print("\n" + "=" * 70)
    print("REMINDER DIAG REPORT (one-pass)")
    print("=" * 70)
    print(f"Trace lines: {stats.lines} | anomalies: {len(stats.anomalies)} | poll_new: {stats.poll_new}")
    print(f"HW last: mic={stats.hw_mic} spk={stats.hw_spk} route={stats.hw_route}")
    print(f"mic_power_off: {stats.power_mic_off} | tts_dropped: {stats.tts_dropped}")
    print(f"ui_text: {stats.ui_text_count} | ui_screen: {stats.ui_screen_count} | spk_pcm: {stats.spk_pcm_first} | tts_decoded: {stats.tts_decoded}")
    print(f"wake_blocked: {stats.wake_blocks}")

    if stats.lc_results:
        print("\nLifecycle summaries:")
        fails = []
        for i, lr in enumerate(stats.lc_results, 1):
            mark = "PASS" if lr.result == "PASS" else "FAIL"
            extra = f"  cause={lr.cause}" if lr.result == "FAIL" else ""
            print(f"  {i}. [{mark}] {lr.owner}{extra}")
            if lr.result == "FAIL":
                fails.append(lr)
        if fails:
            print("\nVERDICT: FAIL")
            for lr in fails:
                print(f"  - {lr.owner}: {hint_for(lr.cause)}")
            print("\nNext fix target (do NOT re-flash until log shows which):")
            p = next((f for f in fails if f.owner == "Proactive"), fails[0])
            print(f"  -> {p.owner} / {p.cause}")
        else:
            print("\nVERDICT: PASS (all LC flows OK)")
    else:
        print("\nVERDICT: INCOMPLETE — no LC_SUMMARY seen; run full test (chat -> alert -> idle)")

    if stats.power_mic_off > 0:
        print(f"\nWARNING: mic powered off {stats.power_mic_off}x during session — wake will fail")
    if stats.tts_dropped > 0:
        print(f"WARNING: {stats.tts_dropped} TTS audio packets dropped — check state/route timing")
    if stats.tts_decoded > 0 and not stats.spk_pcm_first:
        print("WARNING: TTS decoded but no spk_pcm — PCM never reached ES8311 write")
    if stats.ui_text_count > 0 and stats.ui_screen_count == 0:
        print("WARNING: SetChatMessage traced but no screen render — check eezui bypass path")

    print("=" * 70)


def stream_port(port: str, baud: int, duration: float | None, verbose: bool) -> None:
    if serial is None:
        sys.exit("pip install pyserial")
    stats = DiagStats()
    ser = serial.Serial(port, baud, timeout=0.5)
    print(f"Watching {port} @ {baud} — filter ReminderTrace (Ctrl+C for report)")
    print("Test: 1) wake+chat  2) wait alert  3) wake again")
    deadline = time.time() + duration if duration else None
    try:
        while deadline is None or time.time() < deadline:
            raw = ser.readline()
            if not raw:
                continue
            handle_line(raw.decode("utf-8", errors="replace"), stats, verbose)
    except KeyboardInterrupt:
        pass
    finally:
        ser.close()
        print_report(stats)


def stream_file(path: Path, verbose: bool) -> None:
    stats = DiagStats()
    with path.open(encoding="utf-8", errors="replace") as f:
        for line in f:
            handle_line(line, stats, verbose)
    print_report(stats)


def main() -> None:
    p = argparse.ArgumentParser(description="ReminderTrace one-pass analyzer")
    p.add_argument("port", nargs="?", default="COM6")
    p.add_argument("--file", "-f", type=Path)
    p.add_argument("--baud", type=int, default=115200)
    p.add_argument("--duration", type=int, default=0)
    p.add_argument("--verbose", "-v", action="store_true")
    args = p.parse_args()

    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")

    if args.file:
        stream_file(args.file, args.verbose)
    else:
        dur = float(args.duration) if args.duration > 0 else None
        stream_port(args.port, args.baud, dur, args.verbose)


if __name__ == "__main__":
    main()
