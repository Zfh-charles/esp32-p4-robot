@echo off
REM Inference API for device 30:ed:a0:e1:b5:28
cd /d %~dp0
set DEMO_DEVICE_ID=30:ed:a0:e1:b5:28
set INFERENCE_API_HOST=0.0.0.0
set INFERENCE_API_PORT=8765
echo Inference API: http://127.0.0.1:8765
echo Device ID: %DEMO_DEVICE_ID%
python -m pip install -q -r requirements.txt
python server.py
