# Reminder 启动时序设计（ori 开机 + MQTT 延后）

> 备份/交接文档。最后更新：2026-06-21

## 1. 设计原则

| 项 | 策略 |
|----|------|
| **开机显示** | **ori 基线**：SD MJPEG 硬解码（有 SD）；无 SD 时不播表情，仅文本/语音 |
| **唤醒** | 进 `idle` 即 `EnableWakeWordDetection`（ori，不推迟 wake） |
| **Reminder 网络** | `wake=1` 稳定 **5s** 后再启 HTTP 轮询 + MQTT（`REMINDER_BOOT_DEFER_SEC`） |
| **联调重点** | MQTT 上线后的 push → poll → TTS/文本；归一化配置见 handover 文档 |

**已移除**：Scheme B Flash 待机 GIF、`standby.gif` 编译打包、AFE/GIF 互斥逻辑。

## 2. 启动时间线

```
上电 → 网络/OTA → SetDeviceState(idle)
  → SetEmotion("standby") → InitEmotionSystem（需 SD）→ MJPEG standby
  → EnableWakeWordDetection（ori，立即）
  → wake=1 连续 5s → ReminderPoller + ReminderMqttWake Start()
```

无 SD：`InitEmotionSystem` 失败，dialogue 提示插卡；**不影响** MQTT/TTS/文本（若设备稳定）。

## 3. 代码入口

- `Application::TryStartDeferredReminderNet()` — `main/application.cc`
- `CONFIG_REMINDER_BOOT_DEFER_SEC` — `main/Kconfig.projbuild`
- `EezuiDisplayAdapter::SetEmotion()` — SD MJPEG only — `eezui_display_adapter.cc`
- `emotion_video_player_init()` — 初始化时加载 standby MJPEG — `emotion_video_player.c`

## 4. 联调验收

串口期望（**MQTT 上线后**）：

1. `Wake armed — reminder net in 5s`
2. `Reminder network started …` + MQTT subscribed
3. 服务器 push → `MQTT wake -> trigger HTTP poll` → TTS / 文本

## 5. 相关文档

- [handover-reminder-normalization.md](handover-reminder-normalization.md)
- [reminder-mqtt-wake.md](reminder-mqtt-wake.md)
