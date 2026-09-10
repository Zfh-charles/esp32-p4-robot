# 服务器侧联调检查清单与待办

> **发给服务器 / 后端同事**：与 EP Chat P4 ML307 固件（MAC `30:ed:a0:e1:b5:28`）做 MQTT 唤醒 + HTTP 队列 + 小智云 TTS 联调。  
> 固件侧 MQTT 订阅与 HTTP poll **已通**；端到端 TTS 依赖本文待办项。

---

## 1. 目标链路（当前设计，非过时方案）

```
业务 push 入队
    → MQTT publish（门铃，无内容）
        → 固件 GET /pending
            → 固件 mcp_wake：开小智云音频通道 + 短 detect「查提醒」+ wake opus
                → 小智云 LLM 调 MCP backend_get_latest_alert
                    → MCP 读同一 VPS /pending 队列
                        → 小智云 TTS 推流回固件播报
                            → 固件 POST /ack
```

**固件不会**把队列里的长 `prompt` 直接当用户消息上传给小智云（默认 `mcp_wake`）。  
**小智云必须主动下发 TTS 音频**；固件只等待 `tts` JSON + opus 音频包。

---

## 2. 铁律（避免反复踩坑）

### 2.1 禁止用长文本伪唤醒（`direct_wake` 慎用）

| ❌ 不要做 | 原因 |
|----------|------|
| 把完整 `prompt` 当 `listen/detect` 文本发给小智云 | 长文本易被判定为非真实用户对话，**拒播或无声** |
| 默认使用 `delivery_mode: direct_wake` | 同上；仅 ≤32 字且充分实测后可考虑 |
| 在 `prompt` 里写「请播报…」「请用口语提醒用户…」 | 会原样进 LLM/TTS，听起来像在念流程 |

**默认必须使用 `delivery_mode: mcp_wake`**：固件只发短 `wake_text`（如「查提醒」），并附带 **wake opus 音频包**（`CONFIG_SEND_WAKE_WORD_DATA=y`）。队列正文由 **MCP 读 HTTP 队列** 后交给小智云生成 TTS。

### 2.2 播报内容：有消息才播，只播消息本身

| 场景 | 期望 |
|------|------|
| 队列空 / `has_reminder: false` | **完全静默**：不上屏、不 TTS、不 MQTT 副作用 |
| 队列有提醒 | 只播报**提醒正文**，语气自然，像真人转述 |
| `prompt` 写法 | 写**最终要说给用户听的那句话**，不要写实现步骤 |

**prompt 示例：**

```json
// ❌ 差 — 像在描述流程，容易播成「请用简洁口语…」
"prompt": "请用简洁口语提醒用户：10分钟后有产品评审会，请提前进入会议室。"

// ✅ 好 — 就是要播的内容
"prompt": "十分钟后有产品评审会，记得进会议室。"
```

MCP 工具 `spoken_hint` 同理：应是自然口语，不要出现「正在查询提醒队列」之类流程描述。

### 2.3 MQTT 只是门铃

MQTT payload **不携带提醒正文**。正文只在 GET `/pending` 信封里。  
MQTT 主题、账号、TLS 必须与固件一致（见 §3）。

---

## 3. 与固件对齐的配置（生产）

| 项 | 固件当前值 | 服务器必须一致 |
|----|-----------|----------------|
| 设备 MAC（HTTP） | `30:ed:a0:e1:b5:28` | `device_id` / push / pending 路径 |
| MAC 无冒号 | `30eda0e1b528` | MQTT 主题、MQTT 用户名 |
| HTTP poll | `http://114.245.176.144:8443/v1/devices/{device_id}/reminders/pending` | 4G 可达，返回 200 |
| HTTP ack | `.../reminders/ack` | 固件播报成功后 POST |
| MQTT broker | `114.245.176.144:8883`（TLS） | push 后 publish 同一 broker |
| MQTT 主题 | `v1/notify/30eda0e1b528` | **`v1/notify/{mac_clean}`**，不是带冒号 MAC |
| MQTT 用户名 | `esp32_30eda0e1b528` | 模板 `esp32_{mac_clean}` |
| MQTT 密码 | HMAC 派生，**32 hex**（见 §3.1） | Mosquitto ACL 同一算法 |
| 默认投递模式 | `mcp_wake` | push / pending 均返回此字段 |
| wake_text | `查提醒`（可配置） | 与固件 `CONFIG_REMINDER_WAKE_PHRASE` 一致 |

### 3.1 MQTT 密码派生（与固件 `DeriveMqttPassword()` 一致）

```
salt   = hex_decode(CONFIG_REMINDER_MQTT_WAKE_SECRET_SALT)  # 64 hex → 32 字节
message = mac_clean 的 ASCII，如 "30eda0e1b528"
password = HMAC-SHA256(key=salt, msg=message) 的前 16 字节 → 小写 hex（32 字符）
username = esp32_{mac_clean}
client_id = esp32-{mac_clean}
```

当前测试机密码（供 Mosquitto / 手动 publish 验证）：

```
username: esp32_30eda0e1b528
password: 829784e7eaa8e7d848f716dcbd84917a
topic:    v1/notify/30eda0e1b528
```

> 注意：与 `server-normalization-plan-v2.md` 中「UTF-8 salt + 64 hex 密码」描述不同；**以固件实现为准**（hex 解码 salt + 32 hex 截断）。

---

## 4. HTTP API 契约（信封 v1）

### GET `/v1/devices/{device_id}/reminders/pending`

无提醒：

```json
{ "schema": "v1", "has_reminder": false }
```

有提醒（**必须含 `delivery_mode`**）：

```json
{
  "schema": "v1",
  "has_reminder": true,
  "id": "meet-20260625-001",
  "speak": true,
  "delivery_mode": "mcp_wake",
  "wake_text": "查提醒",
  "emotion": "neutral",
  "title": "会议提醒",
  "prompt": "十分钟后有产品评审会，记得进会议室。",
  "expires_at": 1750000000
}
```

### POST `/v1/devices/{device_id}/reminders/push`

请求体示例：

```json
{
  "prompt": "十分钟后有产品评审会，记得进会议室。",
  "speak": true,
  "delivery_mode": "mcp_wake",
  "wake_text": "查提醒",
  "emotion": "neutral",
  "title": "会议提醒"
}
```

push 成功后：**同一时刻**向 `v1/notify/30eda0e1b528` publish MQTT（TLS 8883）。

### POST `/v1/devices/{device_id}/reminders/ack`

```json
{ "id": "meet-20260625-001" }
```

---

## 5. MCP pipe（联调必需，非可选）

固件 `mcp_wake` 模式下，小智云收到 detect「查提醒」后，需 LLM 调用 MCP 才能读到队列里的 `prompt`。

```
小智云 MCP WebSocket  ←→  mcp_pipe.py  ←→  backend_alert.py  ←→  VPS HTTP API
```

| 环境变量 | 联调值 |
|----------|--------|
| `MCP_ENDPOINT` | 小智云控制台 MCP 接入点 + token |
| `INFERENCE_API_BASE` | `http://114.245.176.144:8443`（或 VPS 内网地址） |
| `INFERENCE_DEVICE_ID` | `30:ed:a0:e1:b5:28` |
| `INFERENCE_WAKE_TEXT` | `查提醒` |

**MCP 不在线 → 典型现象：** 固件串口有 `Proactive reminder dispatched`，但 **90s 内无 TTS**（`proactive_tts_no_audio`）。

---

## 6. 检查清单（联调前逐项打勾）

### A. HTTP 队列服务

- [ ] GET `/pending` 对 MAC `30:ed:a0:e1:b5:28` 返回 200
- [ ] 空队列返回 `has_reminder: false`（固件已验证）
- [ ] POST `/push` 可写队列（**当前 PC 直连返回 403，待修复**）
- [ ] push 请求体含 `delivery_mode: mcp_wake` 与自然语言 `prompt`
- [ ] POST `/ack` 可确认并出队
- [ ] `prompt` 仅为播报正文，无「请播报/请查询」等流程用语

### B. MQTT 唤醒

- [ ] Broker `114.245.176.144:8883` TLS 可用
- [ ] 设备 ACL：`esp32_30eda0e1b528` 可 **subscribe** `v1/notify/30eda0e1b528`
- [ ] 服务端 publish 账号有 **publish** 同主题权限
- [ ] push 成功后自动 publish（或联调阶段可手动 publish 验证）
- [ ] 主题使用 **`mac_clean`（无冒号）**，不是 `30:ed:a0:e1:b5:28`

### C. MCP + 小智云

- [ ] `mcp_pipe.py` + `backend_alert.py` 常驻运行
- [ ] `INFERENCE_API_BASE` 指向 VPS 8443，非 localhost（除非 pipe 跑在 VPS 本机）
- [ ] 小智云控制台 MCP 接入点 token 有效
- [ ] LLM 在收到「查提醒」类 detect 时会调 `backend_get_latest_alert`
- [ ] MCP 返回的 `spoken_hint` / 云 TTS 内容为队列 `prompt` 的自然复述，非流程描述

### D. 联调观测（固件串口，设备 idle 待机）

- [ ] `MQTT wake subscribed: v1/notify/30eda0e1b528`
- [ ] push/MQTT 后：`MQTT wake -> trigger HTTP poll`
- [ ] `poll_new | mode=mcp_wake`
- [ ] `Proactive reminder ... dispatched [session-v12]`
- [ ] **`proactive_tts_start`** + `<<` TTS 字幕（关键成功标志）
- [ ] `proactive_audio_packets > 0`
- [ ] 播报结束后 POST ack

---

## 7. 待办事项（请服务器侧认领）

| 优先级 | 待办 | 负责人 | 状态 |
|--------|------|--------|------|
| P0 | **修复 POST `/push` 403**（提供鉴权方式或白名单） | 后端 | ⬜ |
| P0 | **确认 push 后 MQTT publish** 到 `v1/notify/30eda0e1b528`（8883 TLS） | 后端 | ⬜ |
| P0 | **部署并保持 MCP pipe 在线**，`INFERENCE_API_BASE` 指向 8443 | 后端/运维 | ⬜ |
| P1 | pending/push 响应统一带 `delivery_mode: mcp_wake` + `wake_text` | 后端 | ⬜ |
| P1 | 队列 `prompt` 改为自然播报句（见 §2.2），清理流程式模板 | 产品/后端 | ⬜ |
| P1 | Mosquitto 密码派生与固件对齐（§3.1，32 hex） | 后端 | ⬜ |
| P2 | 提供联调 push 脚本或 curl 示例（含鉴权头） | 后端 | ⬜ |
| P2 | push 成功后服务端日志：`[mqtt] wake published topic=...` | 后端 | ⬜ |

---

## 8. 联调步骤（建议顺序）

1. **空队列 MQTT**：publish 到 `v1/notify/30eda0e1b528` → 固件应 `poll_no_reminder`，**无 TTS**
2. **push 一条 mcp_wake 提醒**（自然语言 prompt）→ 自动 MQTT → 固件 `poll_new` → `dispatched`
3. **确认 MCP pipe 在线** → 串口出现 `proactive_tts_start` 与 TTS 字幕
4. **确认 ack** → 再次 poll 为 `has_reminder: false`

---

## 9. 相关文档

- [team-onboarding.md](team-onboarding.md) — 团队总览
- [architecture-reminder-poll-mcp.md](architecture-reminder-poll-mcp.md) — 架构与 API
- [reminder-mqtt-wake.md](reminder-mqtt-wake.md) — MQTT 部署细节
- [server-normalization-plan-v2.md](server-normalization-plan-v2.md) — 信封 v1 与 salt 说明（密码长度以本文 §3.1 为准）
