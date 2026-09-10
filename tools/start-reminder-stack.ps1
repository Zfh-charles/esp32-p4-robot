# 一键启动推理 API + MCP 管道（需先配置 mcp-calculator\.env）
param(
    [string]$DeviceMac = "30:ed:a0:e1:b5:28",
    [string]$McpCalculatorDir,
    [int]$ApiPort = 8765
)

$ErrorActionPreference = "Stop"
if (-not $McpCalculatorDir) {
    $McpCalculatorDir = Join-Path $PSScriptRoot "mcp-calculator"
}
$Root = Split-Path -Parent $PSScriptRoot
$ApiDir = Join-Path $Root "inference-api-demo"

if ($DeviceMac) {
    $env:DEMO_DEVICE_ID = $DeviceMac
}

Write-Host "Checking ngrok ..."
ngrok version

Write-Host "Starting Inference API on port $ApiPort (no demo seed) ..."
Start-Process powershell -ArgumentList @(
    "-NoExit", "-Command",
    "cd '$ApiDir'; `$env:DEMO_DEVICE_ID='$($env:DEMO_DEVICE_ID)'; `$env:SEED_DEMO_ON_START='0'; python server.py"
)

Start-Sleep -Seconds 2

Write-Host "Starting periodic_push (every 180s) ..."
Start-Process powershell -ArgumentList @(
    "-NoExit", "-Command",
    "cd '$ApiDir'; `$env:DEMO_DEVICE_ID='$($env:DEMO_DEVICE_ID)'; `$env:PUSH_INTERVAL_SEC='180'; python periodic_push.py"
)

Start-Sleep -Seconds 1

Write-Host "Starting ngrok http $ApiPort ..."
Start-Process powershell -ArgumentList @(
    "-NoExit", "-Command",
    "ngrok http $ApiPort"
)

Start-Sleep -Seconds 1

Write-Host "Starting mcp_pipe (backend_alert) ..."
if (-not (Test-Path (Join-Path $McpCalculatorDir ".env"))) {
    Write-Warning "Copy mcp-calculator\.env.example to .env and set MCP_ENDPOINT + INFERENCE_DEVICE_ID"
}
Start-Process powershell -ArgumentList @(
    "-NoExit", "-Command",
    "cd '$McpCalculatorDir'; pip install -q -r requirements.txt; python mcp_pipe.py backend_alert.py"
)

Write-Host "Done. API + periodic push + ngrok + MCP pipe started in separate windows."
Write-Host "Ensure sdkconfig poll_url matches ngrok HTTPS URL."
