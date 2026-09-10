# EP Chat P4 ML307 — 小智待机提醒固件

基于 [78/xiaozhi-esp32](https://github.com/78/xiaozhi-esp32) 的 ESP32-P4 固件，面向 **EP Chat P4 ML307**（4G ML307 + 480×480 屏）。在官方小智云语音/TTS 之上，扩展了：

- **本地闹钟**（到点 TTS）
- **HTTP 轮询告警**（待机 idle 主动播报）
- **MCP + HTTP 队列**（PC/服务器侧推理与对话内查提醒）

---

## 快速导航

| 文档 | 用途 |
|------|------|
| **[团队上手与配置指南](docs/team-onboarding.md)** | **新成员必读**：架构、固件/PC/云端配置、ngrok 与公网部署 |
| [待机提醒完整部署](docs/standby-reminder-deployment.md) | 端到端部署步骤 |
| [架构：轮询 + MCP + TTS](docs/architecture-reminder-poll-mcp.md) | REST API 约定与数据流 |
| [ngrok 调试 poll_url](tools/ngrok-poll-url.md) | 无公网服务器时的快速隧道 |
| [GitHub 协作说明](docs/github-setup.md) | 仓库初始化、提交规范、密钥管理 |

---

## 硬件与工具链

| 项 | 说明 |
|----|------|
| 板型 | `ep-chat-p4-ml307`（menuconfig 默认） |
| 芯片 | ESP32-P4 |
| 网络 | ML307 4G（**poll_url 必须公网 HTTPS**） |
| ESP-IDF | **v5.4.1**（团队统一版本） |
| 编译 | `tools\build-flash.bat COM6`（COM 口按本机修改） |

---

## 最小启动（开发机）

```powershell
# 1. 推理 API
cd tools\inference-api-demo
copy .env.example .env    # 填 DEMO_DEVICE_ID = 设备 MAC
pip install -r requirements.txt
python server.py

# 2. 公网隧道（ML307 必需）
ngrok http 8765
# → 将 HTTPS 地址写入 menuconfig → XVSENFENG_ALARM → 默认 poll_url

# 3. MCP（对话内查/推提醒，可选）
cd tools\mcp-calculator
copy .env.example .env    # 填 MCP_ENDPOINT、INFERENCE_DEVICE_ID
pip install -r requirements.txt
python mcp_pipe.py backend_alert.py

# 4. 固件
tools\build-flash.bat COM6
```

一键开四个窗口（API + ngrok + MCP）：`tools\start-reminder-stack.bat`

---

## 仓库结构

```
main/                          # 固件源码
  application.cc               # 会话、idle、主动提醒投递
  reminder/reminder_poller.cc    # HTTP 轮询
  alarm/general_timer.cc       # 本地闹钟
  boards/ep-chat-p4-ml307/     # 板级配置
tools/
  inference-api-demo/            # HTTP 提醒队列 server.py
  mcp-calculator/                # MCP pipe + backend_alert.py
  build-flash.bat                # 编译烧录
  reminder-diag-watch.py         # 串口生命周期诊断
docs/                            # 设计与部署文档
```

---

## 许可证

继承上游 xiaozhi-esp32 及相关组件许可证；商用前请分别确认。
