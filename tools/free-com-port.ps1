param(
    [string]$Port = "COM6"
)

Write-Host "Checking processes that may hold $Port ..."

$pythonProcs = Get-CimInstance Win32_Process -Filter "Name='python.exe'"
$targets = @()
foreach ($p in $pythonProcs) {
    $cmd = $p.CommandLine
    if ($null -eq $cmd) { continue }
    if ($cmd -match 'idf_monitor|esptool|COM6|monitor\.py') {
        Write-Host "FOUND PID $($p.ProcessId): $cmd"
        $targets += $p.ProcessId
    }
}

if ($targets.Count -eq 0) {
    Write-Host "No obvious python serial holders found."
    Write-Host "All python.exe processes:"
    foreach ($p in $pythonProcs) {
        Write-Host "PID $($p.ProcessId): $($p.CommandLine)"
    }
    exit 1
}

foreach ($procId in $targets) {
    Write-Host "Stopping PID $procId ..."
    Stop-Process -Id $procId -Force -ErrorAction SilentlyContinue
}

Start-Sleep -Seconds 2
Write-Host "Done. Retry: idf.py -p $Port flash monitor"
