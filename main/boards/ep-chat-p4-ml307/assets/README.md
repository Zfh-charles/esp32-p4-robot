# EP-Chat P4 板级资源

- **待机/表情**：`/sdcard/mjpeg/*.mjpeg`（含 `standby.mjpeg`），运行时由 SD MJPEG 硬解码播放
- **无 SD**：开机不播表情动画；文本与 Reminder TTS 仍可用（MQTT 联调不依赖 SD）
- **Flash assets**：仅 OGG 音效等通用资源，不再打包 `standby.gif`

更新待机动画：替换 SD 卡 `/mjpeg/standby.mjpeg` 即可，无需改固件。
