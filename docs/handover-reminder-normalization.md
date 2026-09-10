# 交接存档 / Handover — Reminder 生产归一化

> 用途:重启或换会话后,快速恢复“当前业务流上下文”。
> 最后更新:2026-06-21 (UTC+8)

## 1. 目标与架构

- 单一固件镜像跑在多台 ESP32-P4 设备上(生产归一化)。
- 唤醒/提醒通路:**MQTT 唤醒为主 + HTTP 轮询兜底**(+ 远端 MCP-pipe)。
  - MQTT:设备订阅 `v1/notify/{mac_clean}`,收到消息即唤醒拉取。
  - HTTP 轮询:每 60s GET `…/v1/devices/{device_id}/reminders/pending` 作为兜底。
- **启动时序（ori + 延后 Reminder 网络）**：进 `idle` 即开 wake；HTTP/MQTT 在 wake=1 稳定 **5s** 后再连网。详见 [reminder-boot-defer-design.md](reminder-boot-defer-design.md)。
- **启动时序（ori + Reminder 网络延后）**：进 `idle` 即开 wake；HTTP/MQTT 在 wake=1 稳定 5s 后启动。开机表情走 SD MJPEG（ori），无 Flash GIF。
- 设备身份用 MAC:
  - `{device_id}` = `30:ed:a0:e1:b5:28`(带冒号)
  - `{mac_clean}` = `30eda0e1b528`(去冒号小写)

## 2. 当前进度(状态:开发中 — MQTT 联调 + WDT 止血)

- P1/P2/P3 归一化改动已完成(见 §3)。
- **P4 — ori 开机 + MQTT 延后**（2026-06-21）:
  - 移除 Scheme B Flash 待机 GIF，恢复 SD MJPEG ori 开机路径。
  - ori：`idle` 即 `EnableWakeWordDetection`。
  - `REMINDER_BOOT_DEFER_SEC=5`：仅延后 ReminderPoller + ReminderMqttWake。
  - 联调重点：MQTT 上线后的固件/服务器归一化配置。
  - 设计文档：[reminder-boot-defer-design.md](reminder-boot-defer-design.md)
- 待验证：无 SD 下设备稳定 ≥30s → MQTT subscribed → 服务器 push → TTS/文本。

## 3. 已完成的改动

### P1 — 占位符 + 模板化
- `main/reminder/reminder_poller.cc`:`ExpandDeviceIdInUrl` 支持 `{mac_clean}`。
- `main/reminder/reminder_mqtt_wake.cc`:`ExpandDeviceIdInTopic` 支持 `{mac_clean}`;
  `ConnectOnce` 对 `username` / `client_id` 做模板展开。
- topic/username/client_id 改为 `{mac_clean}` 模板。

### P2 — 信封 v1 + severity + 重诵
- `main/reminder/reminder_poller.h`:`ParsePendingResponse` 增加 `std::string& severity` 输出参数。
- `main/reminder/reminder_poller.cc`:解析 `schema` / `type` / `severity`;
  `DoPollOnce` 对非 `kMcpWake` 模式按 `CONFIG_REMINDER_ANNOUNCE_REPEAT` 次数重诵。
- Kconfig:新增 `REMINDER_ANNOUNCE_REPEAT`(int,默认 1,范围 1–5)。

### P3 — 派生密码 + TLS 开关
- Kconfig 新增:
  - `REMINDER_MQTT_WAKE_DERIVE_PASSWORD`(bool,默认 n)
  - `REMINDER_MQTT_WAKE_SECRET_SALT`(string,默认 "",仅在派生开启时可见)
  - `REMINDER_MQTT_TLS_INSECURE`(bool,默认 y)
- `main/reminder/reminder_mqtt_wake.cc`:`DeriveMqttPassword()` 用 mbedtls HMAC-SHA256
  从 `salt + mac_clean` 计算密码;`ConnectOnce` 中按开关覆盖 NVS 密码。
- `managed_components/78__esp-ml307/src/ml307/ml307_mqtt.cc`:端口 8883 时,
  按 `CONFIG_REMINDER_MQTT_TLS_INSECURE` 决定是否 `AT+MSSLCFG="auth",0,0`(关服务端证书校验)。

## 4. 当前 sdkconfig 关键值

```
CONFIG_USE_REMINDER_POLL=y
CONFIG_REMINDER_POLL_DEFAULT_URL="http://114.245.176.144:8443/v1/devices/{device_id}/reminders/pending"
CONFIG_REMINDER_POLL_INTERVAL_SEC=60
CONFIG_REMINDER_ANNOUNCE_REPEAT=1
CONFIG_REMINDER_MQTT_WAKE=y
CONFIG_REMINDER_MQTT_WAKE_DEFAULT_BROKER="114.245.176.144:8883"
CONFIG_REMINDER_MQTT_WAKE_DEFAULT_TOPIC="v1/notify/{mac_clean}"
CONFIG_REMINDER_MQTT_WAKE_DEFAULT_USERNAME="esp32_{mac_clean}"
CONFIG_REMINDER_MQTT_WAKE_DEFAULT_PASSWORD="D3WamDq5shfqqg3BK5AcFKLebdGcREop"
CONFIG_REMINDER_MQTT_WAKE_DEFAULT_CLIENT_ID="esp32-{mac_clean}"
# CONFIG_REMINDER_MQTT_WAKE_DERIVE_PASSWORD is not set
CONFIG_REMINDER_MQTT_TLS_INSECURE=y
```

> 注:NVS 会持久化旧配置。改了上面这些默认值后,首刷需 `erase-flash` 才能让新默认值生效。

## 5. 下一步(待办)

1. 编译烧录并抓取 45s 无 SD 串口，确认无 WDT、见 `Reminder network started`
2. 释放 COM 端口:`powershell -ExecutionPolicy Bypass -File tools\free-com-port.ps1`
3. 烧写:`idf.py -p COM6 flash monitor`
4. 验证连通:监视串口,确认
   - `Ml307Mqtt` 连接成功 + 订阅 `v1/notify/30eda0e1b528`
   - `Wake armed — reminder net in 5s` → `Reminder network started`
   - `ReminderTrace: poll_begin | url=http://114.245.176.144:8443/...` 正常返回

### 关键命令前缀(激活 IDF 环境)
```
cmd /c "call C:\esp_alm\v5.4.1\esp-idf\export.bat && set PYTHONIOENCODING=utf-8 && cd /d C:\Users\0000\Downloads\reset\esp32-p4-i2cpolling && idf.py ..."
```

## 6. 环境坑位记录

- IDE clangd 报的 C/C++ 错误是误报(交叉工具链),以 `idf.py build` 为准。
- COM6 易被残留 `idf_monitor` 占用 → 用 `tools\free-com-port.ps1` 释放。
- 多次中断构建会在 Windows 留下孤儿 `ninja`/`python` 进程,拖慢后续构建 → 重启可清理。
- 串口探针打印 Unicode 需 `sys.stdout.reconfigure(encoding="utf-8", errors="replace")`。
