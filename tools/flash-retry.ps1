# Rebuild + retry esptool flash. Verify serial shows FW_MARKER emotion_pipeline_v6
param(
    [string]$Port = "COM6",
    [int]$MaxAttempts = 40,
    [int]$IntervalSec = 3,
    [switch]$KillSerial,
    [switch]$SkipBuild
)

$ErrorActionPreference = "Continue"
$root = Join-Path $PSScriptRoot ".." | Resolve-Path
$build = Join-Path $root "build"
$export = "C:\esp_alm\v5.4.1\esp-idf\export.bat"
$killScript = Join-Path $PSScriptRoot "kill-serial-holders.ps1"

if (-not $SkipBuild) {
    Write-Host "=== Building firmware ==="
    $buildCmd = "call `"$export`" >nul 2>&1 && cd /d `"$root`" && idf.py build"
    cmd /c $buildCmd
    if ($LASTEXITCODE -ne 0) {
        Write-Host "BUILD FAILED"
        exit 1
    }
    Write-Host "BUILD OK"
}

Write-Host "=== Flash retry on $Port (max $MaxAttempts attempts) ==="
Write-Host "After flash, serial MUST show: FW_MARKER emotion_pipeline_v7f_uihw"
Write-Host ""

if ($KillSerial -and (Test-Path $killScript)) {
    & $killScript -Port $Port
    Start-Sleep -Seconds 1
}

for ($i = 1; $i -le $MaxAttempts; $i++) {
    Write-Host "`n--- Attempt $i/$MaxAttempts $(Get-Date -Format HH:mm:ss) ---"
    $cmd = @"
call "$export" >nul 2>&1 && cd /d "$build" && python -m esptool --chip esp32p4 -p $Port -b 115200 --before usb_reset --after hard_reset write_flash --flash_mode dio --flash_freq 80m --flash_size 16MB 0x2000 bootloader/bootloader.bin 0x20000 xiaozhi.bin 0x8000 partition_table/partition-table.bin 0xd000 ota_data_initial.bin 0x800000 generated_assets.bin
"@
    cmd /c $cmd
    if ($LASTEXITCODE -eq 0) {
        Write-Host "`n=== FLASH SUCCESS ==="
        Write-Host "Run: python tools\_serial_probe.py  (look for FW_MARKER emotion_pipeline_v6)"
        exit 0
    }
    Start-Sleep -Seconds $IntervalSec
}

Write-Host "`n=== FLASH FAILED ==="
exit 1
