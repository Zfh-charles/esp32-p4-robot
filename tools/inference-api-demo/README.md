# 推理提醒 API Demo

与固件 `ReminderPoller` / `ReminderMqttWake`、MCP `backend_alert.py` 共用 HTTP 接口；**push 后自动 MQTT 唤醒**固件立即 GET。

## 快速启动（本机）

```bash
cd tools/inference-api-demo
cp .env.example .env    # 编辑 MAC、MQTT_WAKE_* 等
pip install -r requirements.txt
python server.py
```

默认 `http://0.0.0.0:8765`。生产请设 `SEED_DEMO_ON_START=0`。

## 服务器部署

完整步骤、systemd、Nginx、配置表见：

**[docs/reminder-mqtt-wake.md](../../docs/reminder-mqtt-wake.md)**

**最小部署文件**：

| 文件 | 说明 |
|------|------|
| `server.py` | HTTP 队列 + MQTT publish |
| `requirements.txt` | 含 `paho-mqtt` |
| `.env` | 从 `.env.example` 复制 |

## 固件配置

`menuconfig` → `XVSENFENG_ALARM`：

| 项 | 说明 |
|----|------|
| 启用推理提醒 HTTP 轮询 | `CONFIG_USE_REMINDER_POLL` |
| 启用 MQTT 唤醒 | `CONFIG_REMINDER_MQTT_WAKE` |
| 默认 poll_url | 公网 HTTPS，`{device_id}` = MAC |
| 默认 MQTT broker / topic | 与 `.env` 中 `MQTT_WAKE_*` 一致 |

参考：`sdkconfig.defaults.reminder.example`

NVS 命名空间：`reminder_poll`（HTTP）、`reminder_mqtt`（MQTT）。

## 环境变量

见 `.env.example`。关键项：

| 变量 | 说明 |
|------|------|
| `MQTT_WAKE_ENABLED` | push 后是否发 MQTT（默认 1） |
| `MQTT_WAKE_BROKER` / `MQTT_WAKE_PORT` | 与固件 broker 一致 |
| `MQTT_WAKE_TOPIC_PREFIX` | 主题 `{prefix}/{MAC}` |

## 接口

| 方法 | 路径 | 说明 |
|------|------|------|
| GET | `/v1/devices/{device_id}/reminders/pending` | 固件轮询 |
| POST | `/v1/devices/{device_id}/reminders/push` | 写入队列 + MQTT 唤醒 |
| POST | `/v1/devices/{device_id}/reminders/ack` | 固件确认已播报 |

协议细节：[docs/architecture-reminder-poll-mcp.md](../../docs/architecture-reminder-poll-mcp.md)

## 联调工具

```bash
# 手动 MQTT 唤醒（不经过 push）
python test_mqtt_wake.py 30:ed:a0:e1:b5:28
```
