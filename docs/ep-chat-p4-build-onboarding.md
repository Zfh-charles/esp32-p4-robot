# EP Chat P4 ML307 编译与配置排障指南

本文档记录 **ESP32-P4 + EP_CHAT_P4_ML307（4G）** 板型在 ESP-IDF v5.4.1 下首次编译、烧录、显示时常见的问题与已合并到仓库的修复，便于新成员上手。

相关文档：[团队上手总览](team-onboarding.md) · [GitHub 协作](github-setup.md)

---

## 1. 环境与目标芯片

| 项 | 正确值 | 常见错误 |
|----|--------|----------|
| ESP-IDF | **v5.4.1** | 使用 5.3 或混用多版本 |
| Target | **`esp32p4`** | 误为 `esp32`（会编错芯片、链接失败） |
| 板型 | **`EP_CHAT_P4_ML307`** | `BREAD_COMPACT_ESP32` 等其它板 |

首次克隆后：

```powershell
cd esp32-p4-i2cpolling
idf.py set-target esp32p4
idf.py reconfigure
```

默认 target 见仓库根目录 `sdkconfig.defaults.esp32p4`（已提交，**不含**本机 `sdkconfig`）。

---

## 2. menuconfig 必配项

`sdkconfig` 在 `.gitignore` 中，**勿提交**含真实 ngrok 域名的文件。请参考模板：

- `sdkconfig.defaults.reminder.example` — 板型 + HTTP 轮询 + **MQTT 唤醒**（推荐 300s 兜底）

在 menuconfig 中确认：

```
Xiaozhi Assistant → Board Type → EP Chat P4 ML307
XVSENFENG_ALARM → 启用推理提醒 HTTP 轮询
  → 启用 MQTT 唤醒后立即 HTTP 拉取提醒
  → CONFIG_REMINDER_POLL_INTERVAL_SEC=300
  → CONFIG_REMINDER_POLL_DEFAULT_URL="https://<你的公网>/v1/devices/{device_id}/reminders/pending"
  → CONFIG_REMINDER_MQTT_WAKE_DEFAULT_BROKER="broker.emqx.io:1883"
  → CONFIG_REMINDER_MQTT_WAKE_DEFAULT_TOPIC="xiaozhi/reminder/wake/{device_id}"
  → CONFIG_REMINDER_WAKE_PHRASE="查提醒"
```

MQTT 与服务器部署详见 [reminder-mqtt-wake.md](reminder-mqtt-wake.md)。

设备 MAC 与 `tools/mcp-calculator/.env` 中 `INFERENCE_DEVICE_ID` 一致（串口日志 `mac=`）。

---

## 3. 已修复的编译问题

### 3.1 缺少 `esp_video_codec_types.h`

**现象**：`emotion_video_player.c` 编译报错，找不到 `esp_video_codec` 头文件。

**修复**：`main/idf_component.yml` 增加（仅 `esp32p4`）：

```yaml
espressif/esp_video_codec:
  version: ^0.5.5
  rules:
  - if: target in [esp32p4]
```

### 3.2 缺少 EEZ UI 生成文件

**现象**：`eezui_display_adapter.cc` 报错 `ui/ui.h: No such file`。

**原因**：EEZ Studio 导出的 `ui/` 目录未纳入仓库。

**修复**：`main/boards/ep-chat-p4-ml307/ui/` 提供**最小桩实现**（可编译、可显示文字；完整 EEZ 布局需团队原始 UI 工程替换）。

`main/CMakeLists.txt` 已为该板型增加 `ui/` 的 include 与源文件 glob。

### 3.3 `battery_monitor` 与官方 `bq27220` API 不匹配

**现象**：`ParamCEDV`、`bq27220_init`、`BatteryStatus` 等符号未定义。

**原因**：板级代码面向旧版私有 API；组件管理器拉取的是 `espressif/bq27220` v0.1.1。

**修复**（`battery_monitor.cc/.h`）：

| 旧 API | 官方 API |
|--------|----------|
| `BatteryStatus` | `battery_status_t` |
| `ParamCEDV` | `gauging_config_t` + `parameter_cedv_t` |
| `bq27220_init/deinit` | `bq27220_create/delete` |
| 直接传 `i2c_master_bus_handle_t` | `i2c_bus_create()` 包装已有共享 I2C 总线 |

依赖：`main/idf_component.yml` 中 `espressif/bq27220: ^0.1.1`（`esp32p4`）。

### 3.4 轮询相关符号未定义

**现象**：`application.cc` 中 `ReminderPoller` 等未声明。

**原因**：未开启 `CONFIG_USE_REMINDER_POLL`。

**处理**：按第 2 节打开轮询，或不需要轮询时在 menuconfig 关闭。

---

## 4. 显示：白板 / 无文字 / 无表情

### 4.1 无 SD 卡

- **表情动画**来自 SD 卡 MJPEG，路径：`/sdcard/mjpeg/standby.mjpeg` 等（仓库不含视频文件，需向团队索取或自备）。
- 无 SD 时**不会**播放待机小球动画，属预期行为。

### 4.2 有背光但整屏白板、无中文

**原因**（已修复）：

1. UI 桩未绑定中文字体 `font_puhui_basic_30_4` → 中文不渲染。
2. 待机逻辑在有条目时 3 秒后隐藏对话框 → 无表情时像「空屏」。
3. 默认白底 + 无字形 → 看起来像白板。

**修复**（`eezui_display_adapter.cc` + `ui/ui.c`）：

- 对话框使用中文字体与浅色字色、深色背景。
- **无 SD / 表情系统未就绪时**保持提示文字，不自动隐藏。
- 无 SD 时显示：`请插入SD卡 / 并放入 /mjpeg/ 表情文件`。

### 4.3 SD 卡目录结构（最小）

```
/sdcard/mjpeg/standby.mjpeg    ← 启动必需
/sdcard/mjpeg/neutral.mjpeg
/sdcard/mjpeg/happy.mjpeg
/sdcard/mjpeg/sad.mjpeg
/sdcard/mjpeg/angry.mjpeg
/sdcard/mjpeg/loving.mjpeg
```

---

## 5. 推荐编译烧录流程

```powershell
C:\esp_alm\v5.4.1\esp-idf\export.bat
cd esp32-p4-i2cpolling
idf.py set-target esp32p4
idf.py menuconfig    # 按第 2 节检查
idf.py build
idf.py -p COMx flash monitor
```

若 NVS 中残留旧 `poll_url`：

```powershell
idf.py -p COMx erase-flash flash monitor
```

PC 端服务一键启动：`tools\start-reminder-stack.bat`（API + 定时推送 + ngrok + mcp_pipe）。

---

## 6. 串口正常日志参考

| 日志 | 含义 |
|------|------|
| `I ReminderPoll: Reminder poller started, interval 300s, mac=...` | 轮询已启动（兜底） |
| `I ReminderMqtt: MQTT wake subscribed: xiaozhi/reminder/wake/<MAC>` | MQTT 唤醒就绪 |
| `I ReminderPoll: MQTT wake -> trigger HTTP poll` | 收到唤醒，立即拉队列 |
| `I ReminderPoll: poll_no_reminder` | 队列为空，静默 |
| `I EezuiDisplayAdapter: ✅ UI初始化完成` | LVGL UI 就绪 |
| `E EezuiDisplayAdapter: ❌ SD卡未挂载，无法初始化表情系统` | 无 SD，仅文字 UI |
| `I SD扫描器: ✅ SD卡初始化成功` | 可加载 MJPEG 表情 |

---

## 7. 若仍有完整 EEZ UI 工程

用 EEZ Studio 重新导出 `main/boards/ep-chat-p4-ml307/ui/`，覆盖当前桩文件即可恢复设计稿布局；编译依赖与 `battery_monitor`、`esp_video_codec` 修复仍适用。
