# MQTT 唤醒提醒 — 本地调试与服务器部署

用 **MQTT 推送唤醒** 替代纯 HTTP 周期轮询：推理服务 `push` 后发布 MQTT，固件收到后 **立即** `GET /pending` 拉取队列。HTTP 轮询仅作兜底。

---

## 1. 架构

```
业务 / MCP / 脚本
       │
       ▼ POST /v1/devices/{MAC}/reminders/push
┌──────────────────────────────────────┐
│  VPS 提醒 API                         │
│  · 队列 _PENDING[device_id]           │
│  · push 成功后 publish MQTT（门铃）    │
└──────────────┬───────────────────────┘
               │ MQTT publish（无正文）
               ▼
     v1/notify/{mac_clean}     ← 生产；demo 见 team-onboarding
               │
               ▼ subscribe（ML307 4G）
┌──────────────────────────────────────┐
│  ESP32-P4 固件                        │
│  ReminderMqttWake → TriggerPoll()    │
│  ReminderPoller  → GET /pending      │
│  mcp_wake → 开小智云通道 + detect     │
│  等小智云 MCP 读队列 → TTS 推流播报    │
└──────────────────────────────────────┘
```

**服务器联调清单与待办**：[server-integration-checklist.md](server-integration-checklist.md)

| 通道 | 协议 | 谁发起 | 用途 |
|------|------|--------|------|
| **唤醒** | MQTT TLS 8883（生产） | 服务器 → 固件 | push 后秒级门铃，触发 GET；**payload 无提醒正文** |
| **拉取/确认** | HTTPS | 固件 → 服务器 | GET `/pending`、POST `/ack` |
| **兜底** | HTTPS 周期轮询 | 固件定时 | MQTT 丢包或离线恢复后补拉 |

**设备标识**：统一使用 **MAC**（小写 `aa:bb:cc:dd:ee:ff`），与 HTTP `device_id`、MQTT 主题后缀一致。

### 1.1 固件启动时序（idle + wake 稳定后再连 Reminder 网络）

与 ori 基线兼容：**不推迟唤醒词**，仅延后 HTTP/MQTT 负载。

| 阶段 | 行为 | Kconfig / 代码 |
|------|------|----------------|
| 进 idle | 立刻 `EnableWakeWordDetection`；有 SD 时 MJPEG standby | `ApplyAudioPolicyForState` |
| AFE 就绪前 | （ori，无 GIF 互斥） | — |
| wake=1 连续 5s | 启动 `ReminderPoller` + `ReminderMqttWake` | `REMINDER_BOOT_DEFER_SEC` |

完整说明见 [reminder-boot-defer-design.md](reminder-boot-defer-design.md)。

---

## 2. 需要移植/关注的文件

### 2.1 固件侧（烧录进设备）

| 文件 | 说明 |
|------|------|
| `main/reminder/reminder_mqtt_wake.h` | MQTT 唤醒客户端声明 |
| `main/reminder/reminder_mqtt_wake.cc` | 独立 MQTT 连接、订阅、回调 `TriggerPoll()` |
| `main/reminder/reminder_poller.h` | 新增 `TriggerPoll()` 立即唤醒轮询任务 |
| `main/reminder/reminder_poller.cc` | `xTaskNotifyGive` 实现即时 poll |
| `main/application.h` / `application.cc` | 启动 `ReminderMqttWake` |
| `main/Kconfig.projbuild` | `CONFIG_REMINDER_MQTT_WAKE*` menuconfig 项 |
| `main/CMakeLists.txt` | `CONFIG_REMINDER_MQTT_WAKE` 时编译 `reminder_mqtt_wake.cc` |
| `sdkconfig.defaults.reminder.example` | 团队参考默认配置（含 MQTT broker/topic） |

固件 **无需** 单独部署 MQTT broker 代码；只需 menuconfig 打开开关并填写 broker/topic，首次启动写入 NVS。

### 2.2 服务器侧（部署到 VPS）

| 文件 | 是否必须 | 说明 |
|------|----------|------|
| `tools/inference-api-demo/server.py` | **必须** | HTTP 队列 + `publish_mqtt_wake()` |
| `tools/inference-api-demo/requirements.txt` | **必须** | 含 `paho-mqtt>=2.0.0` |
| `tools/inference-api-demo/.env.example` | 模板 | 复制为 `.env`，填 MQTT/端口等 |
| `tools/inference-api-demo/test_mqtt_wake.py` | 可选 | 手动发 MQTT 唤醒，联调/排障 |
| `tools/mcp-calculator/backend_alert.py` | 可选 | MCP 写同一 HTTP API（与 MQTT 无关） |
| `tools/mcp-calculator/mcp_pipe.py` | 可选 | MCP 管道，跑在 PC 或同机 |

**最小服务器部署**：只需 `server.py` + `requirements.txt` + `.env`。

### 2.3 文档与脚本（团队协作用）

| 文件 | 说明 |
|------|------|
| `docs/reminder-mqtt-wake.md` | 本文档 |
| `docs/reminder-boot-defer-design.md` | idle+wake 延后 MQTT、WDT 止血、无 SD 显示兼容 |
| `docs/team-onboarding.md` | 团队总览与配置清单 |
| `sdkconfig.defaults.reminder.example` | 固件 Kconfig 参考 |
| `tools/ngrok-poll-url.md` | 无 VPS 时用 ngrok 暴露 HTTP |

---

## 3. 配置表

### 3.1 固件 menuconfig（`XVSENFENG_ALARM`）

| Kconfig 项 | 推荐值（生产） | 说明 |
|------------|----------------|------|
| `CONFIG_USE_REMINDER_POLL` | `y` | 启用 HTTP 轮询 |
| `CONFIG_REMINDER_MQTT_WAKE` | `y` | 启用 MQTT 唤醒 |
| `CONFIG_REMINDER_POLL_DEFAULT_URL` | `https://reminder.你的域名/v1/devices/{device_id}/reminders/pending` | 公网 HTTPS，ML307 4G 可达 |
| `CONFIG_REMINDER_POLL_DEFAULT_ACK_URL` | 留空 | 自动从 poll_url 推导 `/ack` |
| `CONFIG_REMINDER_POLL_INTERVAL_SEC` | `300` | 兜底轮询间隔（秒） |
| `CONFIG_REMINDER_BOOT_DEFER_SEC` | `5` | idle + wake=1 稳定此秒数后再启 HTTP/MQTT |
| `CONFIG_REMINDER_MQTT_WAKE_DEFAULT_BROKER` | `broker.emqx.io:1883` 或自建 `mqtt.你的域名:1883` | `host:port` |
| `CONFIG_REMINDER_MQTT_WAKE_DEFAULT_TOPIC` | `xiaozhi/reminder/wake/{device_id}` | 与服务器 `MQTT_WAKE_TOPIC_PREFIX` 一致 |
| `CONFIG_REMINDER_MQTT_WAKE_DEFAULT_USERNAME` | 留空或 broker 账号 | 自建 Mosquitto 时填写 |
| `CONFIG_REMINDER_MQTT_WAKE_DEFAULT_PASSWORD` | 留空或 broker 密码 | 自建 Mosquitto 时填写 |

首次启动写入 NVS 命名空间：

| NVS 命名空间 | 键 | 说明 |
|--------------|-----|------|
| `reminder_poll` | `poll_url` / `ack_url` / `last_id` | HTTP 轮询（已有） |
| `reminder_mqtt` | `broker` | 如 `broker.emqx.io:1883` |
| `reminder_mqtt` | `topic` | 如 `xiaozhi/reminder/wake/{device_id}` |
| `reminder_mqtt` | `username` / `password` | 可选 |
| `reminder_mqtt` | `client_id` | 可选，默认 `xiaozhi-{MAC无冒号}` |

**示例 MAC** `30:ed:a0:e1:b5:28` → 订阅主题：`xiaozhi/reminder/wake/30:ed:a0:e1:b5:28`

### 3.2 服务器环境变量（`.env`）

| 变量 | 默认 | 生产建议 | 说明 |
|------|------|----------|------|
| `INFERENCE_API_HOST` | `0.0.0.0` | `127.0.0.1` | 本机监听；前面加 Nginx 反代 |
| `INFERENCE_API_PORT` | `8765` | `8765` | 与 Nginx `proxy_pass` 一致 |
| `SEED_DEMO_ON_START` | `1` | **`0`** | 禁止启动时塞演示提醒 |
| `DEMO_DEVICE_ID` | 空 | 可留空 | 仅本地种子数据用 |
| `MQTT_WAKE_ENABLED` | `1` | **`1`** | push 后是否发 MQTT |
| `MQTT_WAKE_BROKER` | `broker.emqx.io` | 公网 broker 或自建域名 | 不含端口 |
| `MQTT_WAKE_PORT` | `1883` | `1883` | TLS 一般为 `8883`（需改 server 代码或 broker） |
| `MQTT_WAKE_TOPIC_PREFIX` | `xiaozhi/reminder/wake` | 与固件 topic 前缀一致 | 实际主题为 `{prefix}/{MAC}` |
| `MQTT_WAKE_USERNAME` | 空 | broker 账号 | 可选 |
| `MQTT_WAKE_PASSWORD` | 空 | broker 密码 | 可选 |

### 3.3 MCP 侧（若使用）

| 变量 | 服务器部署时 | 说明 |
|------|--------------|------|
| `INFERENCE_API_BASE` | `http://127.0.0.1:8765`（同机）或 `https://reminder.你的域名` | MCP 调 push/get 的基址 |
| `INFERENCE_DEVICE_ID` | 设备 MAC | 与固件一致 |

---

## 4. 服务器部署步骤

### 4.1 准备 VPS

- 系统：Ubuntu 22.04+ 或同类 Linux
- 开放：**443**（HTTPS API）、**1883**（若自建 Mosquitto）
- 域名：`reminder.your-domain.com`（API）、可选 `mqtt.your-domain.com`（broker）

### 4.2 上传代码

```bash
# 仅需 inference-api-demo 目录，或 clone 整个仓库
git clone <团队仓库> /opt/xiaozhi-reminder
cd /opt/xiaozhi-reminder/tools/inference-api-demo
python3 -m venv venv
source venv/bin/activate
pip install -r requirements.txt
cp .env.example .env
# 编辑 .env：SEED_DEMO_ON_START=0，MQTT_WAKE_* 与固件一致
```

### 4.3 systemd 服务

创建 `/etc/systemd/system/xiaozhi-reminder-api.service`：

```ini
[Unit]
Description=Xiaozhi Reminder HTTP + MQTT Wake API
After=network.target

[Service]
Type=simple
User=www-data
WorkingDirectory=/opt/xiaozhi-reminder/tools/inference-api-demo
EnvironmentFile=/opt/xiaozhi-reminder/tools/inference-api-demo/.env
ExecStart=/opt/xiaozhi-reminder/tools/inference-api-demo/venv/bin/python server.py
Restart=on-failure
RestartSec=5

[Install]
WantedBy=multi-user.target
```

```bash
sudo systemctl daemon-reload
sudo systemctl enable --now xiaozhi-reminder-api
sudo systemctl status xiaozhi-reminder-api
```

日志中 push 成功应出现：`[mqtt] wake published topic=xiaozhi/reminder/wake/...`

### 4.4 Nginx HTTPS 反代

```nginx
server {
    listen 443 ssl;
    server_name reminder.your-domain.com;
    ssl_certificate     /etc/letsencrypt/live/reminder.your-domain.com/fullchain.pem;
    ssl_certificate_key /etc/letsencrypt/live/reminder.your-domain.com/privkey.pem;

    location / {
        proxy_pass http://127.0.0.1:8765;
        proxy_set_header Host $host;
        proxy_set_header X-Real-IP $remote_addr;
        proxy_set_header X-Forwarded-For $proxy_add_x_forwarded_for;
        proxy_set_header X-Forwarded-Proto $scheme;
    }
}
```

```bash
sudo certbot --nginx -d reminder.your-domain.com
```

### 4.5 固件 poll_url 切换到服务器

menuconfig 或本地 `sdkconfig`：

```
CONFIG_REMINDER_POLL_DEFAULT_URL="https://reminder.your-domain.com/v1/devices/{device_id}/reminders/pending"
```

烧录后若 NVS 已有旧 ngrok URL，需 `erase-flash` 或删除 NVS 命名空间 `reminder_poll` / `reminder_mqtt` 后重启。

### 4.6 MQTT Broker 选型

| 方案 | 适用 | 固件 broker 填 |
|------|------|----------------|
| **EMQX 公共** | 快速上线、免运维 | `broker.emqx.io:1883` |
| **自建 Mosquitto** | 生产隔离、需账号 | `mqtt.your-domain.com:1883` |

服务器 `MQTT_WAKE_BROKER` 与固件 `reminder_mqtt.broker` **主机名必须一致**，且 ML307 4G 能解析并连通。

---

## 5. 本地调试（开发机）

### 5.1 启动 API

```powershell
cd tools\inference-api-demo
copy .env.example .env
python -m pip install -r requirements.txt
python server.py
```

### 5.2 HTTP 仍用 ngrok（4G 板）

ML307 无法访问 `192.168.x.x`，`poll_url` 用 ngrok HTTPS；MQTT 用公网 broker，**无需** ngrok。

```
https://<ngrok>/v1/devices/{device_id}/reminders/pending
```

### 5.3 手动发 MQTT 唤醒（方式 2）

```powershell
python -c "import paho.mqtt.publish as p; p.single('xiaozhi/reminder/wake/30:ed:a0:e1:b5:28', payload='{\"type\":\"reminder_wake\",\"device_id\":\"30:ed:a0:e1:b5:28\"}', hostname='broker.emqx.io', port=1883); print('ok')"
```

或：

```powershell
python tools\inference-api-demo\test_mqtt_wake.py 30:ed:a0:e1:b5:28
```

### 5.4 完整链路测试

```powershell
# 1. 写入队列
python -c "import urllib.request,json; d=json.dumps({'prompt':'MQTT唤醒测试','speak':True}).encode(); print(urllib.request.urlopen(urllib.request.Request('http://127.0.0.1:8765/v1/devices/30:ed:a0:e1:b5:28/reminders/push',data=d,headers={'Content-Type':'application/json'})).read().decode())"

# 2. 若未自动 MQTT，再手动发唤醒（见上）
```

串口期望（数秒内）：

```
I ReminderMqtt: Wake message [...]
I ReminderPoll: MQTT wake -> trigger HTTP poll
I ReminderPoll: New reminder ...
```

---

## 6. 验证清单

| 步骤 | 期望 |
|------|------|
| 固件启动 | `MQTT wake subscribed: v1/notify/<mac_clean>` |
| POST push | 服务器 publish MQTT；固件 `MQTT wake -> trigger HTTP poll` |
| 空队列仅 MQTT | `poll_no_reminder`，**无 TTS** |
| 有队列 mcp_wake | `dispatched` → **`proactive_tts_start`** → TTS 播自然 prompt |
| MCP 离线 | 有 `dispatched` 无 TTS（90s 超时） |

完整服务器待办：[server-integration-checklist.md](server-integration-checklist.md)

---

## 7. 故障排查

| 现象 | 检查 |
|------|------|
| 无 `ReminderMqtt` 日志 | menuconfig `CONFIG_REMINDER_MQTT_WAKE`；NVS `reminder_mqtt.broker` 非空 |
| 有 subscribe 无 Wake message | broker 地址不一致；服务器 `MQTT_WAKE_ENABLED=0`；防火墙挡 1883 |
| 有 wake 无 TTS | `poll_url` 不可达；设备非 idle；队列空 |
| push 无 `[mqtt]` 日志 | `pip install paho-mqtt`；**仅保留一个** `server.py` 进程 |
| 仍等 60s 才播报 | MQTT 未通，仅靠兜底轮询；查 broker 与主题 MAC |
| 换域名后无效 | NVS 旧 URL → `erase-flash` 或清 `reminder_poll` / `reminder_mqtt` |

---

## 8. 相关文档

- [team-onboarding.md](team-onboarding.md) — 团队总览与配置一张表
- [architecture-reminder-poll-mcp.md](architecture-reminder-poll-mcp.md) — HTTP 队列与 MCP 协议
- [tools/ngrok-poll-url.md](../tools/ngrok-poll-url.md) — 无 VPS 时 HTTP 公网暴露
