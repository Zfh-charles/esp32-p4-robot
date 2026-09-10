# Kill Python processes holding the serial port (esptool, idf_monitor, etc.).
# Does NOT kill the calling PowerShell process.
param(
    [string]$Port = "COM6"
)

$selfPid = $PID
$parentPid = (Get-CimInstance Win32_Process -Filter "ProcessId=$selfPid").ParentProcessId

Get-CimInstance Win32_Process -Filter "Name='python.exe'" | ForEach-Object {
    $cmd = $_.CommandLine
    if ($cmd -match 'esptool|idf_monitor|read-com|_serial_probe') {
        if ($cmd -match $Port) {
            Write-Host "Killing python PID $($_.ProcessId): $cmd"
            Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue
        }
    }
}

Start-Sleep -Seconds 1
Write-Host "COM ports:" ([System.IO.Ports.SerialPort]::getportnames() -join ', ')
