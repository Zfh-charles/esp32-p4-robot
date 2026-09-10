"""
按需向推理 API 推送提醒（演示用）。默认不自动推送，避免无真实事件时打扰设备。

环境变量:
  INFERENCE_API_BASE   默认 http://127.0.0.1:8765
  DEMO_DEVICE_ID       设备 MAC（与固件一致）
  PUSH_INTERVAL_SEC    推送间隔秒，默认 0（不循环；>0 时才定时 push）
  PUSH_TITLE           提醒标题
  PUSH_PROMPT          完整提醒文案（进队列，由 MCP 读出后 TTS）
  PUSH_WAKE_TEXT       idle 主动推流时的短唤醒词，默认「查提醒」
  PUSH_EMOTION         表情，默认 fear
  PUSH_ON_START        默认 0，设为 1 时启动立即 push 一条
"""
from __future__ import annotations

import json
import os
import sys
import time
import urllib.error
import urllib.request

if sys.platform == "win32":
    sys.stderr.reconfigure(encoding="utf-8")
    sys.stdout.reconfigure(encoding="utf-8")

API_BASE = os.environ.get("INFERENCE_API_BASE", "http://127.0.0.1:8765").rstrip("/")
DEVICE_ID = os.environ.get(
    "DEMO_DEVICE_ID",
    os.environ.get("INFERENCE_DEVICE_ID", "30:ed:a0:e1:b5:28"),
)
INTERVAL_SEC = int(os.environ.get("PUSH_INTERVAL_SEC", "0"))
TITLE = os.environ.get("PUSH_TITLE", "跌倒告警")
PROMPT = os.environ.get(
    "PUSH_PROMPT",
    "请用简洁口语提醒用户：检测到老人可能摔倒了，请立即查看！",
)
EMOTION = os.environ.get("PUSH_EMOTION", "fear")
WAKE_TEXT = os.environ.get("PUSH_WAKE_TEXT", "查提醒")
PUSH_ON_START = os.environ.get("PUSH_ON_START", "0") != "0"


def push_alert() -> dict:
    url = f"{API_BASE}/v1/devices/{DEVICE_ID}/reminders/push"
    body = {
        "title": TITLE,
        "prompt": PROMPT,
        "emotion": EMOTION,
        "speak": True,
        "delivery_mode": "mcp_wake",
        "wake_text": WAKE_TEXT,
    }
    data = json.dumps(body, ensure_ascii=False).encode("utf-8")
    req = urllib.request.Request(
        url,
        data=data,
        method="POST",
        headers={"Content-Type": "application/json", "Accept": "application/json"},
    )
    with urllib.request.urlopen(req, timeout=15) as resp:
        return json.loads(resp.read().decode("utf-8"))


def main() -> None:
    print(f"[periodic] device={DEVICE_ID} interval={INTERVAL_SEC}s on_start={PUSH_ON_START}")
    print(f"[periodic] API {API_BASE}")
    if INTERVAL_SEC <= 0 and not PUSH_ON_START:
        print("[periodic] idle — set PUSH_ON_START=1 or PUSH_INTERVAL_SEC>0 to push")
        return

    if PUSH_ON_START:
        try:
            result = push_alert()
            print(f"[periodic] pushed on start: {result.get('id', result)}")
        except (urllib.error.URLError, json.JSONDecodeError, TimeoutError) as e:
            print(f"[periodic] push failed (is server.py running?): {e}")
            sys.exit(1)

    if INTERVAL_SEC <= 0:
        return

    while True:
        time.sleep(INTERVAL_SEC)
        try:
            result = push_alert()
            print(f"[periodic] pushed: {result.get('id', result)}")
        except (urllib.error.URLError, json.JSONDecodeError, TimeoutError) as e:
            print(f"[periodic] push failed: {e}")


if __name__ == "__main__":
    main()
