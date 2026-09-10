# 后台提醒 MCP Demo（对接官方小智云）

本目录为早期 demo 副本；**推荐直接使用同级的 [`../mcp-calculator/`](../mcp-calculator/)**（已纳入本仓库，含 `mcp_pipe.py` 与 `backend_alert.py`）。

- **待机主动播报**：由固件 `ReminderPoller` HTTP 轮询 [inference-api-demo](../inference-api-demo/)（见 [architecture-reminder-poll-mcp.md](../../docs/architecture-reminder-poll-mcp.md)）。
- **对话中查询**：MCP 同样轮询该 API，用户唤醒后问「有什么会议提醒」即可。

## 工具列表

| 工具 | 说明 |
|------|------|
| `backend_get_latest_alert` | 取最高优先级待提醒 |
| `backend_list_alerts` | 列出全部待提醒 |
| `backend_push_alert` | 模拟后台写入一条新提醒 |
| `backend_ack_alert` | 确认已读并删除 |

## 推荐启动方式（mcp-calculator）

```powershell
cd tools\mcp-calculator
copy .env.example .env
# 编辑 .env：MCP_ENDPOINT、INFERENCE_DEVICE_ID
pip install -r requirements.txt

# 终端 1：推理 API
cd ..\inference-api-demo
python server.py

# 终端 2：MCP 管道
cd ..\mcp-calculator
python mcp_pipe.py backend_alert.py
```

或一键启动：`tools\start-reminder-stack.bat`

在小智控制台 / Agent 配置里勾选该 MCP 服务。

## 对话测试话术

1. 对小球说：「后台有什么要提醒我的吗？」
2. LLM 应调用 `backend_get_latest_alert`，并播报 `spoken_hint` 中的内容。
3. 在 PC 上可先运行脚本模拟后台写入（见下）。

### 本地模拟后台写入

另开终端，用 MCP Inspector 或临时 Python 调用 `backend_push_alert`；或修改 `backend_alert.py` 里 `_PENDING_ALERTS` 初始数据后重启 `mcp_pipe`。

## 与固件的关系

- 固件 **无需改代码** 即可使用本 Demo（MCP 在云端/管道侧）。
- 若需「到点自动提醒」，请用固件 `GeneralTimer` + `SendMessage`（见方案文档方案 B）。

## 故障排查

| 现象 | 处理 |
|------|------|
| 工具不被调用 | 检查 MCP_ENDPOINT、控制台是否启用该 MCP |
| 无语音只有文字 | 确认设备已唤醒且会话正常 |
| Windows 编码乱码 | `backend_alert.py` 已处理 stderr/stdout UTF-8 |
