$ErrorActionPreference = "Stop"

$projectRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
$compiler = (Get-Command g++ -ErrorAction Stop).Source
$sourceDir = Join-Path $projectRoot "main\boards\ep-chat-p4-ml307"
$testBinary = Join-Path $env:TEMP "mjpeg_frame_source_test.exe"

$compileArgs = @(
    "-std=c++17",
    "-Wall",
    "-Wextra",
    "-Werror",
    "-I$sourceDir",
    (Join-Path $projectRoot "tools\host_tests\mjpeg_frame_source_test.cc"),
    (Join-Path $sourceDir "mjpeg_frame_source.c"),
    "-o",
    $testBinary
)

try {
    & $compiler @compileArgs
    if ($LASTEXITCODE -ne 0) {
        exit $LASTEXITCODE
    }
    & $testBinary
    if ($LASTEXITCODE -ne 0) {
        exit $LASTEXITCODE
    }
} finally {
    Remove-Item -LiteralPath $testBinary -Force -ErrorAction SilentlyContinue
}

exit 0
