# ngrok setup helper for Xiaozhi firmware poll_url
# Usage: powershell -ExecutionPolicy Bypass -File tools\setup-ngrok.ps1

$NgrokUrl = "https://bin.equinox.io/c/bNyj1mQVY4c/ngrok-v3-stable-windows-amd64.zip"
$InstallDir = "$env:USERPROFILE\ngrok"
$ZipPath = "$env:TEMP\ngrok.zip"

Write-Host "========================================" -ForegroundColor Cyan
Write-Host " ngrok setup (Xiaozhi poll_url)" -ForegroundColor Cyan
Write-Host "========================================" -ForegroundColor Cyan
Write-Host ""

$ngrokExe = "$InstallDir\ngrok.exe"
if (Test-Path $ngrokExe) {
    Write-Host "OK: found ngrok at $ngrokExe" -ForegroundColor Green
} else {
    Write-Host "Step 1/3: downloading ngrok ..." -ForegroundColor Yellow
    New-Item -ItemType Directory -Force -Path $InstallDir | Out-Null
    Invoke-WebRequest -Uri $NgrokUrl -OutFile $ZipPath -UseBasicParsing
    Expand-Archive -Path $ZipPath -DestinationPath $InstallDir -Force
    Remove-Item $ZipPath -Force
    Write-Host "OK: installed to $InstallDir" -ForegroundColor Green
}

Write-Host ""
Write-Host "Step 2/3: configure authtoken (one-time)" -ForegroundColor Yellow
Write-Host "  1. https://dashboard.ngrok.com/signup"
Write-Host "  2. https://dashboard.ngrok.com/get-started/your-authtoken"
Write-Host ""
$token = Read-Host "Paste ngrok authtoken (Enter to skip)"
if ($token) {
    & $ngrokExe config add-authtoken $token
    Write-Host "OK: authtoken saved" -ForegroundColor Green
} else {
    Write-Host "SKIP: run manually: ngrok config add-authtoken YOUR_TOKEN" -ForegroundColor Yellow
}

Write-Host ""
Write-Host "Step 3/3: how to start" -ForegroundColor Yellow
Write-Host "  Terminal A: tools\inference-api-demo\run-local.bat"
Write-Host "  Terminal B: ngrok http 8765"
Write-Host ""
Write-Host "Copy the https Forwarding URL into menuconfig poll_url:"
Write-Host "  https://xxxx.ngrok-free.app/v1/devices/{device_id}/reminders/pending"
Write-Host ""

$start = Read-Host "Start ngrok http 8765 now? (y/n)"
if ($start -eq "y") {
    Write-Host "Starting ngrok (Ctrl+C to stop)..." -ForegroundColor Green
    & $ngrokExe http 8765
}
