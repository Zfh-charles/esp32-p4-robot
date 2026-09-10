# 团队上手与配置指南

本文档面向**新加入的开发者**：说明固件侧机制、PC/云端服务、以及「另一台 PC + 一块新板子」如何配齐关键信息并继续开发。

---

## 1. 系统总览

```
┌─────────────────────────────────────────────────────────────────────────┐
│ 官方小智云 (xiaozhi.me)                                                  │
│  · MQTT + UDP 音频通道                                                   │
│  · LLM 对话 + 云端 TTS 合成                                              │
│  · MCP WebSocket 接入点（wss://api.xiaozhi.me/mcp/?token=...）           │
└───────────────┬───────────────────────────────┬─────────────────────────┘
                │ 用户唤醒「你好小易」              │ 固件主动开通道 + TTS
                ▼                                 ▼
     ┌──────────────────┐              ┌──────────────────────────┐
     │ PC: mcp_pipe     │              │ ESP32-P4 固件             │
     │ backend_alert.py │              │ · Idle 待机 + 唤醒词      │
     │ (MCP 工具)       │              │ · ReminderMqttWake 订阅   │
     └────────┬─────────┘              │ · ReminderPoller GET/兜底  │
              │ 读/写同一队列             │ · GeneralTimer 闹钟       │
              └──────────────┬──────────┴────────────┬─────────────┘
                             ▼                       │ MQTT subscribe
                  ┌──────────────────────┐          │ (公网 broker)
                  │ HTTP 提醒队列服务      │◄─────────┘ push 后 publish 唤醒
                  │ tools/.../server.py  │
                  │ (本机+ngrok 或 VPS)    │
                  └──────────────────────┘
```

| 组件 | 运行位置 | 何时工作 | 作用 |
|------|----------|----------|------|
| **Idle 待机** | 固件 | 无用户对话、无主动提醒 | 轮询 HTTP、等待唤醒词 |
| **本地闹钟** | 固件 | 到点触发 | 本地定时 → 开云通道 TTS |
| **ReminderPoller** | 固件 | `idle` 且 `session=None` | GET `/pending` → 主动播报 |
| **ReminderMqttWake** | 固件 | 联网后常驻订阅 | 收 MQTT → 立即 `TriggerPoll()` |
| **小智云 TTS** | 云端 | 开音频通道后 | 将文本合成为语音下发设备 |
| **server.py** | PC/VPS | 7×24 | 维护每设备待提醒队列 |
| **mcp_pipe + backend_alert** | PC | 用户已唤醒对话 | LLM 调工具查/推提醒 |

**设备标识**：统一使用 **MAC 地址**（小写 `aa:bb:cc:dd:ee:ff`），与 HTTP 头 `Device-Id`、队列 `device_id` 一致。串口启动日志可见。

---

## 2. 固件侧机制

### 2.1 Idle 待机（Standby）

- **状态**：`kDeviceStateIdle`，`SessionKind::None`
- **行为**：显示待机 UI/表情（需 SD 卡 `/sdcard/mjpeg/standby.mjpeg`）；开启唤醒词检测；**ReminderPoller 在此状态下轮询**
- **跳过轮询**：非 idle、本地闹钟响、用户对话中、主动提醒进行中
- **关键代码**：`main/application.cc` — `EnterIdleStandby()`、`RestoreIdleReady()`

### 2.2 本地闹钟（GeneralTimer）

- **开关**：menuconfig → `XVSENFENG_ALARM` → `启用闹钟功能`（`CONFIG_USE_ALARM`）
- **行为**：到点 → `kDeviceStateAlarm` → 通过 `SendWakeWordDetected` 开云通道播报闹钟文案
- **关键代码**：`main/alarm/general_timer.cc`
- **与轮询关系**：闹钟响时 **ReminderPoller 暂停**，避免冲突

### 2.3 HTTP 轮询 + MQTT 唤醒（ReminderPoller / ReminderMqttWake）

- **开关**：`启用推理提醒 HTTP 轮询`（`CONFIG_USE_REMINDER_POLL`）
- **MQTT 唤醒**：`启用 MQTT 唤醒后立即 HTTP 拉取提醒`（`CONFIG_REMINDER_MQTT_WAKE`，推荐开启）
- **间隔**：`CONFIG_REMINDER_POLL_INTERVAL_SEC`（**生产建议 300s 兜底**；纯轮询调试可用 60s）
- **流程**：
  1. 服务器 `POST /push` → 入队 → **MQTT publish** `v1/notify/{mac_clean}`（生产）或 `xiaozhi/reminder/wake/{device_id}`（demo）
  2. 固件 `ReminderMqttWake` 收到 → `TriggerPoll()` → **立即** `GET {poll_url}`
  3. 无 MQTT 时由周期轮询兜底
  4. `has_reminder: false` → **完全静默**，不上屏、不 TTS
  5. `has_reminder: true` → 按 `delivery_mode` 投递（**默认 `mcp_wake`**）
  6. 播报成功 → `POST {ack_url}` → NVS 记录 `last_id` 防重复
- **NVS 命名空间** `reminder_poll`：

| 键 | 说明 |
|----|------|
| `poll_url` | 轮询地址，支持 `{device_id}` / `{mac}` 占位符 |
| `ack_url` | 确认地址，空则从 `poll_url` 的 `/pending` 改为 `/ack` |
| `last_id` | 已 ack 的提醒 ID |
| `last_ack_ts` | 最近 ack 时间（300s 内同 ID 跳过） |

- **NVS 命名空间** `reminder_mqtt`（MQTT 唤醒）：

| 键 | 说明 |
|----|------|
| `broker` | 如 `114.245.176.144:8883`（TLS）或 demo `broker.emqx.io:1883` |
| `topic` | 生产 `v1/notify/{mac_clean}`；demo `xiaozhi/reminder/wake/{device_id}` |
| `username` / `password` | 可选 |
| `client_id` | 可选，默认 `xiaozhi-{MAC无冒号}` |

- **关键代码**：
  - `main/reminder/reminder_poller.cc` — HTTP 拉取、`TriggerPoll()`
  - `main/reminder/reminder_mqtt_wake.cc` — MQTT 订阅与唤醒
  - `main/application.cc` — `DeliverReminder()` / `RunReminderDelivery()`

- **详细部署**：[reminder-mqtt-wake.md](reminder-mqtt-wake.md)（本地调试、VPS 迁移、配置表、移植文件清单）

### 2.4 主动提醒投递模式

> 完整约束与服务器待办：[server-integration-checklist.md](server-integration-checklist.md)

| 模式 | JSON 字段 | 固件行为 |
|------|-----------|----------|
| **mcp_wake**（**默认，生产必用**） | `delivery_mode: "mcp_wake"`, `wake_text: "查提醒"` | 开云通道 → 发 **wake opus** + 短 detect「查提醒」→ **idle 等小智云 TTS 推流**；正文由 **MCP 读 HTTP 队列** 后生成。**无消息不播报**；UI 推迟到 TTS 开始 |
| `direct_wake` | `delivery_mode: "direct_wake"` | 将 prompt（≤32 字）作为 detect 发出。**勿默认**：长文本易被小智云拒播/无声 |
| `alert_only` | `speak: false` | 仅屏幕 + 振动，不连云 TTS |

**铁律（勿反复犯错）：**

1. **禁止**把队列长 `prompt` 当 detect 上传小智云（除非充分实测的短句 `direct_wake`）。
2. **必须** `mcp_wake` + MCP pipe 在线 + 云主动 TTS；固件 `CONFIG_SEND_WAKE_WORD_DATA=y` 时会先发 wake opus。
3. **`prompt` 只写最终播报句**，自然口语；**不要**写「请播报…」「请用口语提醒用户…」等流程描述。
4. 队列为空 → 完全静默，不上屏、不 TTS。

**prompt 示例：**

```json
// ❌ 差
"prompt": "请用简洁口语提醒用户：10分钟后有产品评审会。"

// ✅ 好
"prompt": "十分钟后有产品评审会，记得进会议室。"
```

- **反馈窗口**：`CONFIG_REMINDER_FEEDBACK_SEC`（默认 5s）— TTS 结束后短暂聆听用户反馈
- **唤醒短语**：`CONFIG_REMINDER_WAKE_PHRASE`（默认「查提醒」）— 仅 mcp_wake detect 用，不是播报正文

### 2.5 用户主动对话

- 用户说唤醒词 → `SessionKind::User` → 小智云 LLM + TTS
- 若 PC 侧 **mcp_pipe** 在线，LLM 可调用：
  - `backend_get_latest_alert` — 查最新提醒
  - `backend_push_alert` — 写入队列（固件下次 idle 轮询会播报）
  - `backend_list_alerts` / `backend_ack_alert`

---

## 3. PC / 云端服务机制

### 3.1 小智云 TTS（主动提醒）

- 固件 **不本地合成** 长文案，也 **不在 mcp_wake 下上传 queue prompt**。
- **mcp_wake 流程**：
  1. `OpenAudioChannel()` 连接官方 MQTT/UDP
  2. 发送 **wake opus 包**（若 `CONFIG_SEND_WAKE_WORD_DATA=y`）+ `SendWakeWordDetected("查提醒")`
  3. 小智云 LLM 调 MCP `backend_get_latest_alert` → 读 VPS 队列 `prompt`
  4. 小智云 **主动** 返回 `tts` JSON + opus 音频 → 固件播放
  5. 无 TTS  within 90s → `proactive_tts_no_audio`，不 ack
- **前提**：设备已激活；**MCP pipe 常驻**；队列 `prompt` 为自然播报句
- **限制**：官方云无待机 notify API；勿用长文本 direct_wake 代替上述流程

### 3.2 HTTP 队列服务（server.py）

路径：`tools/inference-api-demo/server.py`

| 方法 | 路径 | 说明 |
|------|------|------|
| GET | `/v1/devices/{device_id}/reminders/pending` | 固件轮询 |
| POST | `/v1/devices/{device_id}/reminders/push` | MCP/脚本/业务写入 |
| POST | `/v1/devices/{device_id}/reminders/ack` | 固件确认已播报 |

**环境变量**（见 `tools/inference-api-demo/.env.example`）：

| 变量 | 默认 | 说明 |
|------|------|------|
| `INFERENCE_API_HOST` | `0.0.0.0` | 监听地址 |
| `INFERENCE_API_PORT` | `8765` | 端口 |
| `DEMO_DEVICE_ID` | 空 | 启动时种子数据的 MAC |
| `SEED_DEMO_ON_START` | `1` | 设为 `0` 则空队列启动（**推荐生产/联调**） |
| `MQTT_WAKE_ENABLED` | `1` | push 后是否 MQTT 唤醒固件 |
| `MQTT_WAKE_BROKER` | `broker.emqx.io` | 与固件 NVS `reminder_mqtt.broker` 主机一致 |
| `MQTT_WAKE_PORT` | `1883` | MQTT 端口 |
| `MQTT_WAKE_TOPIC_PREFIX` | `xiaozhi/reminder/wake` | 实际主题 `{prefix}/{MAC}` |

push 成功且 MQTT 开启时，日志应出现：`[mqtt] wake published topic=...`

**推送测试**（默认不自动 push，避免无事件时打扰）：

```powershell
$env:PUSH_ON_START="1"
$env:DEMO_DEVICE_ID="aa:bb:cc:dd:ee:ff"
python tools/inference-api-demo/periodic_push.py
```

### 3.3 MCP 管道（mcp_pipe + backend_alert）

路径：`tools/mcp-calculator/`

```
小智云 MCP WebSocket  ←→  mcp_pipe.py  ←→  backend_alert.py  ←→  server.py
```

**`.env` 配置**（从 `.env.example` 复制，**勿提交 `.env`**）：

```ini
MCP_ENDPOINT=wss://api.xiaozhi.me/mcp/?token=YOUR_TOKEN
INFERENCE_API_BASE=http://127.0.0.1:8765
INFERENCE_DEVICE_ID=aa:bb:cc:dd:ee:ff
INFERENCE_POLL_INTERVAL_SEC=30
```

启动：

```powershell
cd tools\mcp-calculator
pip install -r requirements.txt
python mcp_pipe.py backend_alert.py
```

---

## 4. 公网访问：ngrok 与 VPS

EP Chat P4 ML307 使用 **4G（ML307）**，无法访问 `127.0.0.1` 或局域网 IP，**poll_url 必须是公网 HTTPS**。

### 4.1 当前调试方式：ngrok

```powershell
# 终端 1
cd tools\inference-api-demo
python server.py

# 终端 2
ngrok http 8765
# 复制 Forwarding HTTPS，例如 https://abcd1234.ngrok-free.app
```

固件 poll_url 模板：

```
https://abcd1234.ngrok-free.app/v1/devices/{device_id}/reminders/pending
```

写入方式（二选一）：

1. `idf.py menuconfig` → `XVSENFENG_ALARM` → **默认 poll_url**
2. 首次烧录前编辑本地 `sdkconfig`（该文件 **不提交 Git**）

**注意**：

- ngrok 免费域名 **每次重启会变** → 更新 poll_url 并 **重烧或擦 NVS** `reminder_poll`
- 固件 HTTP 已带 `ngrok-skip-browser-warning: 69420` 头

详细步骤：[tools/ngrok-poll-url.md](../tools/ngrok-poll-url.md)

### 4.2 迁移到公有云服务器（推荐团队/生产）

完整步骤（含 MQTT、systemd、Nginx）：**[reminder-mqtt-wake.md §4](reminder-mqtt-wake.md#4-服务器部署步骤)**

**概要**：

1. **准备 VPS**，开放 443；可选自建 Mosquitto 开放 1883
2. **部署最小文件集**（见下表）到 `/opt/xiaozhi-reminder/tools/inference-api-demo/`
3. **配置 `.env`**：`SEED_DEMO_ON_START=0`，`MQTT_WAKE_*` 与固件 broker/topic 一致
4. **systemd** 守护 `python server.py`
5. **Nginx + HTTPS** 反代 `8765` → `https://reminder.your-domain.com`
6. **固件 poll_url** 改为固定域名；**MQTT broker** 与服务器 `.env` 一致
7. **不再需要 ngrok**；多设备靠 MAC 区分队列

**服务器需拷贝/关注的文件**：

| 路径 | 必须 |
|------|------|
| `tools/inference-api-demo/server.py` | ✓ |
| `tools/inference-api-demo/requirements.txt` | ✓ |
| `tools/inference-api-demo/.env`（从 `.env.example` 复制） | ✓ |
| `tools/inference-api-demo/test_mqtt_wake.py` | 联调可选 |

```bash
# VPS 示例
git clone <本仓库> /opt/xiaozhi-reminder
cd /opt/xiaozhi-reminder/tools/inference-api-demo
python3 -m venv venv && source venv/bin/activate
pip install -r requirements.txt
cp .env.example .env   # 编辑 SEED_DEMO_ON_START=0、MQTT_WAKE_*
# systemd + nginx 见 reminder-mqtt-wake.md
```

固件 poll_url 模板：

```
https://reminder.your-domain.com/v1/devices/{device_id}/reminders/pending
```

MCP 侧 `INFERENCE_API_BASE`：与 server 同机用 `http://127.0.0.1:8765`；分离部署则填 VPS 内网或 HTTPS 基址。

---

## 5. 配置清单（一张表）

新板子 + 新 PC 联调时，按此表逐项填写：

| # | 配置项 | 在哪里配 | 示例 / 说明 |
|---|--------|----------|-------------|
| 1 | **设备 MAC** | 串口日志 `mac=` | `30:ed:a0:e1:b5:28` |
| 2 | **小智云激活** | 小智控制台 | 设备能正常对话 |
| 3 | **MCP_ENDPOINT** | `tools/mcp-calculator/.env` | 控制台 → MCP 接入点 + token |
| 4 | **INFERENCE_DEVICE_ID** | 同上 + `inference-api-demo/.env` | = MAC |
| 5 | **poll_url** | menuconfig 或 NVS `reminder_poll` | `https://<公网>/v1/devices/{device_id}/reminders/pending` |
| 6 | **ack_url** | 可选 menuconfig | 留空则自动 `/pending` → `/ack` |
| 7 | **轮询间隔（兜底）** | menuconfig | **300**（MQTT 为主）；纯轮询调试 60 |
| 8 | **MQTT broker** | menuconfig 或 NVS `reminder_mqtt` | 生产 `114.245.176.144:8883` |
| 9 | **MQTT 主题** | menuconfig 或 NVS `reminder_mqtt` | 生产 `v1/notify/{mac_clean}` |
| 10 | **服务器 MQTT / push / MCP** | VPS | 见 [server-integration-checklist.md](server-integration-checklist.md) |
| 11 | **COM 口** | `tools/build-flash.bat` 参数 | 本机 `COM6` / `COM3` 等 |
| 12 | **ESP-IDF 路径** | `tools/build-flash.bat` 第 13 行 | 团队统一 `C:\esp_alm\v5.4.1\esp-idf` |
| 13 | **SD 卡表情** | 物理 SD | `/sdcard/mjpeg/standby.mjpeg` 等 |

---

## 6. 新成员上手步骤（另一台 PC）

> **编译/白屏排障**：见 [ep-chat-p4-build-onboarding.md](ep-chat-p4-build-onboarding.md)（target、依赖、UI 桩、SD 表情、battery_monitor 等）。

### 6.1 环境

1. 安装 [ESP-IDF v5.4.1](https://docs.espressif.com/projects/esp-idf/en/v5.4.1/esp32p4/get-started/index.html)
2. 安装 Python 3.8+、Git、ngrok（调试期）
3. `git clone <团队仓库>` 到本地

### 6.2 固件（第一块板）

```powershell
cd esp32-p4-i2cpolling
idf.py set-target esp32p4          # 仅首次
idf.py menuconfig
#   Board: EP Chat P4 ML307
#   XVSENFENG_ALARM: 启用轮询 + MQTT 唤醒 + 填 poll_url / broker
copy sdkconfig.defaults.reminder.example sdkconfig.reminder.local
#   参考 example 中的 CONFIG_* 项合并到 menuconfig

tools\build-flash.bat COMx         # x = 本机端口
```

串口应出现：

```
I ReminderPoll: Reminder poller started, interval 300s, mac=...
I ReminderMqtt: MQTT wake subscribed: v1/notify/<mac_clean>
I ReminderPoll: poll_no_reminder          # 队列为空时
```

### 6.3 PC 服务

```powershell
# 推理 API
cd tools\inference-api-demo
copy .env.example .env
# 编辑 DEMO_DEVICE_ID=<MAC>, SEED_DEMO_ON_START=0
python server.py

# ngrok（无 VPS 时）
ngrok http 8765
# 更新 poll_url → 重烧或擦 NVS

# MCP
cd tools\mcp-calculator
copy .env.example .env
pip install -r requirements.txt
python mcp_pipe.py backend_alert.py
```

### 6.4 验证

| 步骤 | 期望 |
|------|------|
| 空队列 idle | 仅 `poll_no_reminder`，**无 TTS、无弹窗** |
| `POST /push`（mcp_wake + 自然 prompt） | MQTT publish → `MQTT wake -> trigger HTTP poll` → `dispatched` → **`proactive_tts_start`** |
| 仅 MQTT（队列已有提醒） | 立即 poll，非等兜底间隔 |
| MCP pipe 离线 | 有 `dispatched` 但 **无 TTS**（90s 超时）— 说明 MCP 必开 |
| 唤醒问「有什么提醒」 | MCP 返回队列内容（User 对话路径） |

服务器侧完整清单：[server-integration-checklist.md](server-integration-checklist.md)

### 6.5 更换 poll_url / 擦 NVS

NVS 中 `poll_url` **首次写入后持久保存**。更换 ngrok 域名时需：

- `idf.py erase-flash` 后重烧，或
- 使用 NVS 工具删除 `reminder_poll` 命名空间后重启

---

## 7. 继续开发

| 方向 | 入口文件 |
|------|----------|
| 轮询逻辑 / 静默策略 | `main/reminder/reminder_poller.cc`, `main/application.cc` |
| **MQTT 唤醒** | `main/reminder/reminder_mqtt_wake.cc` |
| 闹钟 | `main/alarm/general_timer.cc` |
| 板级引脚/显示 | `main/boards/ep-chat-p4-ml307/` |
| Kconfig 开关 | `main/Kconfig.projbuild` → `XVSENFENG_ALARM` |
| HTTP API + MQTT publish | `tools/inference-api-demo/server.py` |
| MCP 新工具 | `tools/mcp-calculator/backend_alert.py` |
| 串口诊断 | `tools/reminder-diag-watch.py` |
| MQTT 手动唤醒测试 | `tools/inference-api-demo/test_mqtt_wake.py` |

相关设计文档：

- [server-integration-checklist.md](server-integration-checklist.md) — **服务器联调清单与待办（发给后端）**
- [reminder-mqtt-wake.md](reminder-mqtt-wake.md) — MQTT 唤醒本地/服务器部署
- [architecture-reminder-poll-mcp.md](architecture-reminder-poll-mcp.md) — 架构与 mcp_wake 流程
- [standby-reminder-deployment.md](standby-reminder-deployment.md)
- [proactive-reminder-official-cloud.md](proactive-reminder-official-cloud.md) — 官方云能力边界

---

## 8. 常见问题

| 现象 | 原因 | 处理 |
|------|------|------|
| 一直 `poll_no_reminder` | 队列空 / MAC 不匹配 | 检查 `DEMO_DEVICE_ID` 与串口 MAC |
| 轮询无 HTTP 响应 | poll_url 错 / ngrok 过期 | 更新 URL，擦 NVS |
| push 后要等很久才播报 | MQTT 未通，仅靠兜底轮询 | 查 `ReminderMqtt subscribed`；对齐 broker/topic |
| 有 wake 无 TTS | MCP 未在线 / 云未推 TTS / 非 idle | 查 `proactive_tts_no_audio`；开 mcp_pipe；见 server-integration-checklist |
| 有提醒但不 TTS | 用了 direct_wake 或 prompt 像流程描述 | 改 `mcp_wake`；prompt 写自然播报句 |
| push 403 | VPS 鉴权未开 | 后端修复，见 server-integration-checklist |
| push 无 `[mqtt]` 日志 | 未装 paho-mqtt / 多实例 server | `pip install paho-mqtt`；只保留一个 `server.py` |
| 无消息仍打扰 | 演示脚本自动 push | `SEED_DEMO_ON_START=0`, `PUSH_ON_START=0` |
| 烧录失败 | COM 占用 / 未进下载模式 | `free-com-port.ps1`，BOOT+RESET |
| MCP 工具不可用 | `.env` token 过期 | 控制台重新获取 MCP_ENDPOINT |

---

## 9. 密钥与 Git

**切勿提交**：`sdkconfig`（含 ngrok URL）、`tools/mcp-calculator/.env`、ngrok authtoken、小智 MCP token。

仓库只保留 `*.example` 模板。详见 [github-setup.md](github-setup.md)。
