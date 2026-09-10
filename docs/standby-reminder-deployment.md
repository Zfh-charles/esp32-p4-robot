# 待机轮询提醒 — 完整部署指南（小智云 + mcp-calculator + 固件）

> **团队上手请先读：[team-onboarding.md](team-onboarding.md)**（配置清单、ngrok/VPS、新 PC/新板子步骤）  
> **生产联调 / 服务器待办：[server-integration-checklist.md](server-integration-checklist.md)**  
> **当前架构（mcp_wake + MQTT 门铃）：[architecture-reminder-poll-mcp.md](architecture-reminder-poll-mcp.md)**

> 基于 [78/xiaozhi-esp32](https://github.com/78/xiaozhi-esp32) 官方协议，语音/TTS 走小智云；待机唤醒由 **MQTT 门铃 + HTTP 轮询** 触发，默认 **`mcp_wake`**（短 detect + wake opus → MCP 读队列 → 云 TTS）。

---

## 1. 架构总览

```
┌─────────────────────────────────────────────────────────────────┐
│ 官方小智云 (xiaozhi.me)                                          │
│  · MQTT + UDP 音频                                               │
│  · MCP 接入点 wss://... （对话时 LLM 调工具）                      │
└────────────┬───────────────────────────────┬──────────────────────┘
             │ 用户唤醒对话                    │ OpenAudioChannel + TTS
             ▼                                 ▼
    ┌─────────────────┐                 ┌──────────────────┐
    │ 本机 mcp_pipe   │                 │ ESP32 固件        │
    │ backend_alert   │                 │ ReminderPoller    │
    └────────┬────────┘                 └────────┬─────────┘
             │ 读/写同一队列                       │ HTTP GET pending
             └──────────────┬──────────────────────┘
                            ▼
                 ┌──────────────────────┐
                 │ 推理提醒 API          │
                 │ tools/.../server.py  │
                 │ (本机或公网 VPS)      │
                 └──────────────────────┘
```

| 组件 | 何时工作 | 作用 |
|------|----------|------|
| **ReminderPoller**（固件） | 待机 idle | MQTT/HTTP 拉取 → **`mcp_wake`** → 云 TTS |
| **backend_alert**（MCP） | 用户已唤醒对话 | LLM 查/推提醒，与固件同源 |
| **server.py**（推理 API） | 7×24 在线 | 维护待提醒队列 |
| **小智云** | 常在线 | 语音通道 + TTS，不提供待机推送 |

---

## 2. 你需要提供的信息

部署前请准备：

| 项 | 说明 | 如何获取 |
|----|------|----------|
| **MCP_ENDPOINT** | 小智云 MCP WebSocket 地址 | 小智控制台 → 设备/MCP 接入点 |
| **设备 MAC** | `aa:bb:cc:dd:ee:ff` 小写 | 固件串口启动日志 `MAC Address:` |
| **公网 poll_url** | ML307 4G 可访问的 HTTPS 地址 | ngrok / 云服务器（见 §4） |
| **小智云已激活** | 设备能正常语音对话 | 常规定价流程 |

---

## 3. 固件配置

### 3.1 打开轮询功能

`idf.py menuconfig` → `Component config` → `Xiaozhi Assistant` → `XVSENFENG_ALARM`：

- `[*] 启用推理提醒 HTTP 轮询（待机播报）`
- `默认 poll_url`：填公网地址，支持 `{device_id}` 占位符  
  例：`https://abc.ngrok-free.app/v1/devices/{device_id}/reminders/pending`
- `提醒轮询间隔`：调试可设 `30`

首次启动且 NVS 无 `poll_url` 时，自动将 menuconfig 默认值写入 NVS（永久保存）。

### 3.2 编译烧录

```powershell
cd C:\esp_alm\v5.4.1\esp-idf && export.bat
cd C:\Users\0000\Documents\xiaozhi-p4-epdainaozhong0109\xiaozhi-p4-epdainaozhong
idf.py build flash monitor
```

### 3.3 串口验证

正常日志（**mcp_wake**，队列为空时无 TTS）：

```
I ReminderPoll: Reminder poller started, interval 300s, mac=aa:bb:cc:dd:ee:ff
I ReminderMqtt: MQTT wake subscribed: v1/notify/aabbccddeeff
I ReminderPoll: poll_no_reminder
```

有提醒且 MCP 在线时：

```
I ReminderMqtt: MQTT wake -> trigger HTTP poll
I ReminderPoll: poll_new | mode=mcp_wake
I Application: Proactive reminder ... dispatched
I Application: proactive_tts_start
I ReminderPoll: Ack posted for meet-xxx
```

---

## 4. 推理 API（本机 / 公网）

### 4.1 本机启动

```powershell
cd tools\inference-api-demo
pip install -r requirements.txt
$env:DEMO_DEVICE_ID="aa:bb:cc:dd:ee:ff"   # 你的 MAC
python server.py
```

接口：

- `GET  /v1/devices/{id}/reminders/pending` — 固件轮询
- `POST /v1/devices/{id}/reminders/push`   — MCP/推理写入
- `POST /v1/devices/{id}/reminders/ack`    — 播报确认

### 4.2 ML307 4G 需要公网地址

本机 `127.0.0.1` / `192.168.x.x` 设备访问不到。任选其一：

**A. ngrok（快速调试）**

```powershell
ngrok http 8765
# 得到 https://xxxx.ngrok-free.app
# poll_url = https://xxxx.ngrok-free.app/v1/devices/{device_id}/reminders/pending
```

**B. 云服务器（永久）**

将 `server.py` 部署到 VPS，绑定域名 + HTTPS，poll_url 填域名。

---

## 5. MCP 服务（mcp-calculator）

### 5.1 配置

```powershell
cd tools\mcp-calculator
copy .env.example .env
# 编辑 .env，填入 MCP_ENDPOINT 和 INFERENCE_DEVICE_ID
pip install -r requirements.txt
```

`.env` 示例：

```
MCP_ENDPOINT=wss://api.xiaozhi.me/mcp/?token=YOUR_TOKEN
INFERENCE_API_BASE=http://127.0.0.1:8765
INFERENCE_DEVICE_ID=aa:bb:cc:dd:ee:ff
```

### 5.2 启动

**终端 1** — 推理 API：

```powershell
cd xiaozhi-p4-epdainaozhong\tools\inference-api-demo
python server.py
```

**终端 2** — MCP 管道：

```powershell
cd tools\mcp-calculator
python mcp_pipe.py
# 或只启 backend_alert:
python mcp_pipe.py backend_alert.py
```

`mcp_config.json` 已默认启用 `backend-alert`，禁用 calculator 示例。

### 5.3 MCP 工具

| 工具 | 场景 |
|------|------|
| `backend_get_latest_alert` | 用户问「有什么会议提醒」 |
| `backend_push_alert` | LLM/你主动推一条提醒进队列 → **固件下次轮询会播报** |
| `backend_ack_alert` | 手动确认已处理 |
| `backend_list_alerts` | 列出待提醒 |

---

## 6. 端到端验证流程

1. 启动 `server.py`（`DEMO_DEVICE_ID` = 设备 MAC）
2. 启动 `mcp_pipe.py backend_alert.py`（`MCP_ENDPOINT` 已配置）
3. `POST /push`（`delivery_mode: mcp_wake`，**自然语言 prompt**）→ MQTT publish → 固件 `dispatched` → **`proactive_tts_start`**
4. 空队列仅 MQTT → `poll_no_reminder`，**无 TTS**
5. 唤醒设备问「有什么提醒」→ LLM 调 `backend_get_latest_alert` 应答

---

## 7. 数据流时序

```
T-10min  推理/MCP backend_push_alert → POST /push → 队列入库 + MQTT publish
T-9min   固件 idle：MQTT 触发 GET /pending → has_reminder:true, mcp_wake
         → 开通道 + wake opus + detect「查提醒」
         → 小智云 MCP 读队列 → 云 TTS 推流
         → POST /ack → 队列清除
T-8min   用户唤醒问「还有提醒吗」→ MCP backend_get_latest_alert → 无
```

---

## 8. 常见问题

| 现象 | 原因 | 处理 |
|------|------|------|
| `poll_url not set` | NVS 空且 menuconfig 默认 URL 未填 | menuconfig 填默认 URL 或手动写 NVS |
| `HTTP open failed` | 4G 访问不到 API | 换公网/ngrok 地址 |
| `HTTP status 404` | MAC 与 `DEMO_DEVICE_ID` 不一致 | 统一为小写 MAC |
| MCP 工具无响应 | `MCP_ENDPOINT` 错或未启动 mcp_pipe | 检查 .env 和小智控制台 |
| 播报一次后不再播 | `last_id` 去重 | 推新 id 或清 NVS `last_id` |
| 对话中不播 | 仅 idle 触发 | 等回到待机 |

---

## 9. 相关文件

| 路径 | 说明 |
|------|------|
| `main/reminder/reminder_poller.cc` | 固件轮询 |
| `main/reminder/reminder_mqtt_wake.cc` | MQTT 唤醒 |
| `main/application.cc` | `DeliverReminder` / `RunReminderDelivery` |
| `tools/inference-api-demo/server.py` | 推理 API |
| `tools/mcp-calculator/backend_alert.py` | MCP 工具 |
| `tools/mcp-calculator/mcp_pipe.py` | 小智云 MCP 管道 |
| `docs/architecture-reminder-poll-mcp.md` | 架构与 API 契约 |
| `docs/server-integration-checklist.md` | **服务器联调清单（发给后端）** |
