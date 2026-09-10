param(
    [switch]$DryRun,
    [switch]$Reconfigure,
    [string]$ProjectRoot
)

$ErrorActionPreference = "Stop"

$wrapperRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$projectRoot = if ([string]::IsNullOrWhiteSpace($ProjectRoot)) {
    $wrapperRoot
} else {
    (Resolve-Path -LiteralPath $ProjectRoot).Path
}
$idfPath = "C:\esp_alm\v5.4.1\esp-idf"
$toolsPath = "C:\Users\0000\.espressif"
$pythonEnv = Join-Path $toolsPath "python_env\idf5.4_py3.14_env"
$python = Join-Path $pythonEnv "Scripts\python.exe"
$romElfDir = Join-Path $toolsPath "tools\esp-rom-elfs\20241011"
$ninja = Join-Path $toolsPath "tools\ninja\1.12.1\ninja.exe"
$riscvBin = Join-Path $toolsPath `
    "tools\riscv32-esp-elf\esp-14.2.0_20241119\riscv32-esp-elf\bin"
$cc = Join-Path $riscvBin "riscv32-esp-elf-gcc.exe"
$cxx = Join-Path $riscvBin "riscv32-esp-elf-g++.exe"

foreach ($required in @($idfPath, $pythonEnv, $romElfDir, $ninja, $cc, $cxx)) {
    if (-not (Test-Path -LiteralPath $required)) {
        throw "Missing ESP-IDF build dependency: $required"
    }
}

$env:IDF_PATH = $idfPath
$env:IDF_TOOLS_PATH = $toolsPath
$env:IDF_PYTHON_ENV_PATH = $pythonEnv
$env:ESP_ROM_ELF_DIR = $romElfDir
$env:GIT_CONFIG_COUNT = "1"
$env:GIT_CONFIG_KEY_0 = "safe.directory"
$env:GIT_CONFIG_VALUE_0 = "C:/esp_alm/v5.4.1/esp-idf"

$toolBins = @(
    (Join-Path $pythonEnv "Scripts"),
    $riscvBin,
    (Join-Path $toolsPath "tools\cmake\3.30.2\bin"),
    (Join-Path $toolsPath "tools\ninja\1.12.1"),
    (Join-Path $toolsPath "tools\ccache\4.10.2"),
    "C:\Program Files\Git\cmd",
    "C:\Program Files\Git\mingw64\bin",
    "C:\Program Files\Git\usr\bin"
)
$env:PATH = ($toolBins + $env:PATH) -join ";"

if ($Reconfigure) {
    & $python (Join-Path $idfPath "tools\idf.py") "-C" $projectRoot `
        "-D" "CMAKE_MAKE_PROGRAM=$ninja" `
        "-D" "CMAKE_PROGRAM_PATH=$riscvBin" `
        "reconfigure"
    if ($LASTEXITCODE -ne 0) {
        exit $LASTEXITCODE
    }
}

$ninjaArgs = @("-C", (Join-Path $projectRoot "build"))
if ($DryRun) {
    $ninjaArgs += "-n"
}

& $ninja @ninjaArgs
$buildExitCode = $LASTEXITCODE
if ($buildExitCode -ne 0 -or $DryRun) {
    exit $buildExitCode
}

& $python (Join-Path $wrapperRoot "tools\code_health\check_p4_image_layout.py") `
    (Join-Path $projectRoot "build\xiaozhi.bin")
exit $LASTEXITCODE
