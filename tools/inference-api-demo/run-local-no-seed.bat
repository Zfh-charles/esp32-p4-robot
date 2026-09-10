@echo off

REM 推理 API（不自动 seed 演示会议，配合 periodic_push 测试）

cd /d %~dp0

set DEMO_DEVICE_ID=30:ed:a0:e1:b5:28

set SEED_DEMO_ON_START=0

set INFERENCE_API_HOST=0.0.0.0

set INFERENCE_API_PORT=8765

echo Inference API: http://127.0.0.1:8765 (no demo seed)

echo Device ID: %DEMO_DEVICE_ID%

python server.py

