# 无公网服务器时：用 ngrok 给 ML307 4G 设备提供 poll_url

> 团队完整说明见 [docs/team-onboarding.md](../docs/team-onboarding.md) §4.1

## 步骤

### 1. 安装 ngrok

https://ngrok.com/download — 注册后获取 authtoken：

```powershell
ngrok config add-authtoken YOUR_NGROK_TOKEN
```

### 2. 启动推理 API（终端 1）

```powershell
cd tools\inference-api-demo
copy .env.example .env
# 编辑 DEMO_DEVICE_ID = 设备 MAC，SEED_DEMO_ON_START=0
python server.py
```

或：`.\run-local-no-seed.bat`

### 3. 启动 ngrok（终端 2）

```powershell
ngrok http 8765
```

复制 **Forwarding** 里的 HTTPS 地址，例如：

```
https://a1b2c3d4.ngrok-free.app
```

### 4. 写入固件 poll_url

`idf.py menuconfig` → `XVSENFENG_ALARM` → **默认 poll_url**：

```
https://a1b2c3d4.ngrok-free.app/v1/devices/{device_id}/reminders/pending
```

保存后 `idf.py build flash`（若 NVS 已有旧 poll_url，需擦除 `reminder_poll` 命名空间或 `erase-flash` 后重烧）。

参考模板：[sdkconfig.defaults.reminder.example](../sdkconfig.defaults.reminder.example)

### 5. 启动 MCP（终端 3，可选）

```powershell
cd tools\mcp-calculator
copy .env.example .env
pip install -r requirements.txt
python mcp_pipe.py backend_alert.py
```

## 一键启动

```powershell
tools\start-reminder-stack.bat
```

会开四个窗口：API（无种子）、periodic push、ngrok、MCP。**演示 push 默认仍可能推送告警**，联调空队列请手动只开 API + ngrok。

## 注意

- ngrok 免费版域名每次重启会变，需更新 menuconfig 并重新烧录/写 NVS
- 正式环境请换 VPS 固定域名，见 [team-onboarding.md](../docs/team-onboarding.md) §4.2
