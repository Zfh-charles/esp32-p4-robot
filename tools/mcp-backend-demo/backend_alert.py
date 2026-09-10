"""
小智外部 MCP + 推理提醒 API（与固件 ReminderPoller 同源队列）

环境变量:
  INFERENCE_API_BASE   例如 http://127.0.0.1:8765 或 https://your-ngrok.app
  INFERENCE_DEVICE_ID  与固件 MAC 一致（小写 aa:bb:cc:dd:ee:ff）
  INFERENCE_POLL_INTERVAL_SEC  默认 30

派生 URL（可用环境变量覆盖）:
  INFERENCE_POLL_URL  .../v1/devices/{device_id}/reminders/pending
  INFERENCE_PUSH_URL  .../v1/devices/{device_id}/reminders/push
  INFERENCE_ACK_URL   .../v1/devices/{device_id}/reminders/ack
"""
from __future__ import annotations

import json
import logging
import os
import sys
import threading
import time
import urllib.error
import urllib.request
from datetime import datetime, timezone

from fastmcp import FastMCP

if sys.platform == "win32":
    sys.stderr.reconfigure(encoding="utf-8")
    sys.stdout.reconfigure(encoding="utf-8")

logging.basicConfig(level=logging.INFO)
logger = logging.getLogger("BackendAlert")

mcp = FastMCP("BackendAlert")

_API_BASE = os.environ.get("INFERENCE_API_BASE", "http://127.0.0.1:8765").rstrip("/")
_DEVICE_ID = os.environ.get("INFERENCE_DEVICE_ID", "00:00:00:00:00:00")
_POLL_INTERVAL = int(os.environ.get("INFERENCE_POLL_INTERVAL_SEC", "30"))

_PATH_TMPL = "/v1/devices/{device_id}/reminders"


def _url(kind: str) -> str:
    env_key = f"INFERENCE_{kind.upper()}_URL"
    custom = os.environ.get(env_key, "")
    if custom:
        return _expand_url(custom)
    return _expand_url(f"{_API_BASE}{_PATH_TMPL}/{kind}")


def _expand_url(url: str) -> str:
    return url.replace("{device_id}", _DEVICE_ID).replace("{mac}", _DEVICE_ID)


def _http_json(method: str, url: str, body: dict | None = None, timeout: int = 10) -> dict:
    data = None
    headers = {"Accept": "application/json"}
    if body is not None:
        data = json.dumps(body, ensure_ascii=False).encode("utf-8")
        headers["Content-Type"] = "application/json"
    req = urllib.request.Request(url, data=data, headers=headers, method=method)
    with urllib.request.urlopen(req, timeout=timeout) as resp:
        return json.loads(resp.read().decode("utf-8"))


_PENDING_ALERTS: list[dict] = []
_CACHE_LOCK = threading.Lock()


def _fetch_pending() -> None:
    url = _url("pending")
    try:
        data = _http_json("GET", url)
    except (urllib.error.URLError, json.JSONDecodeError, TimeoutError) as e:
        logger.warning("poll failed: %s", e)
        return

    with _CACHE_LOCK:
        _PENDING_ALERTS.clear()
        if data.get("has_reminder"):
            _PENDING_ALERTS.append(
                {
                    "id": data.get("id", "unknown"),
                    "level": "warning",
                    "title": data.get("title", "提醒"),
                    "message": data.get("prompt", ""),
                    "emotion": data.get("emotion", "neutral"),
                    "created_at": _now_iso(),
                }
            )
            logger.info("poll: cached reminder %s", data.get("id"))


def _poll_loop() -> None:
    while True:
        _fetch_pending()
        time.sleep(_POLL_INTERVAL)


def _now_iso() -> str:
    return datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


_poll_thread = threading.Thread(target=_poll_loop, daemon=True, name="inference_poll")
_poll_thread.start()
_fetch_pending()


@mcp.tool()
def backend_get_latest_alert() -> dict:
    """Get the highest-priority pending alert from the backend inference service.
    Use when the user asks about meetings or important reminders."""
    with _CACHE_LOCK:
        alerts = list(_PENDING_ALERTS)
    if not alerts:
        return {
            "success": True,
            "has_alert": False,
            "spoken_hint": "目前没有需要提醒的事项。",
        }
    alert = alerts[0]
    logger.info("get_latest_alert: %s", alert["id"])
    return {
        "success": True,
        "has_alert": True,
        "alert": alert,
        "spoken_hint": f"{alert['title']}：{alert['message']}",
    }


@mcp.tool()
def backend_list_alerts() -> dict:
    """List all pending backend alerts as JSON."""
    with _CACHE_LOCK:
        alerts = list(_PENDING_ALERTS)
    return {"success": True, "count": len(alerts), "alerts": alerts}


@mcp.tool()
def backend_push_alert(
    title: str,
    message: str,
    level: str = "info",
    emotion: str = "neutral",
    speak: bool = True,
) -> dict:
    """Push a reminder into the shared inference API queue.
    The device ReminderPoller will pick it up on next idle poll and speak via Xiaozhi TTS."""
    try:
        result = _http_json(
            "POST",
            _url("push"),
            {
                "title": title,
                "prompt": message,
                "emotion": emotion,
                "speak": speak,
            },
        )
    except (urllib.error.URLError, json.JSONDecodeError, TimeoutError) as e:
        logger.error("push failed: %s", e)
        return {"success": False, "error": str(e)}

    _fetch_pending()
    logger.info("push_alert: %s", json.dumps(result, ensure_ascii=False))
    return {"success": True, "result": result}


@mcp.tool()
def backend_ack_alert(alert_id: str) -> dict:
    """Acknowledge a reminder in the shared inference API (same as firmware after TTS)."""
    try:
        result = _http_json("POST", _url("ack"), {"id": alert_id})
    except (urllib.error.URLError, json.JSONDecodeError, TimeoutError) as e:
        logger.error("ack failed: %s", e)
        return {"success": False, "error": str(e)}

    _fetch_pending()
    return {"success": True, "result": result}


if __name__ == "__main__":
    logger.info("BackendAlert MCP for device %s, API %s", _DEVICE_ID, _API_BASE)
    mcp.run(transport="stdio")
