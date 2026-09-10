@echo off

REM 每 3 分钟推送「老人摔倒」提醒（需 server.py 已启动）

cd /d %~dp0

set DEMO_DEVICE_ID=30:ed:a0:e1:b5:28

set INFERENCE_API_BASE=http://127.0.0.1:8765

set PUSH_INTERVAL_SEC=180

set PUSH_TITLE=跌倒告警

set PUSH_PROMPT=请用简洁口语提醒用户：检测到老人可能摔倒了，请立即查看！

set PUSH_EMOTION=fear

set PUSH_ON_START=1

echo Periodic push every %PUSH_INTERVAL_SEC%s to device %DEMO_DEVICE_ID%

python periodic_push.py

