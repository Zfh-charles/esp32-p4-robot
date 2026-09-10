# 本机推理 API（设备 MAC: 30:ed:a0:e1:b5:28）
$env:DEMO_DEVICE_ID = "30:ed:a0:e1:b5:28"
$env:INFERENCE_API_HOST = "0.0.0.0"
$env:INFERENCE_API_PORT = "8765"
python -m pip install -q -r requirements.txt
Write-Host "Inference API: http://127.0.0.1:8765"
Write-Host "Device ID: $env:DEMO_DEVICE_ID"
Write-Host "固件 poll_url 需公网地址，请另开终端运行: ngrok http 8765"
python server.py
