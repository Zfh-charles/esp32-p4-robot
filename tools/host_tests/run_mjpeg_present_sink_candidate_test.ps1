$ErrorActionPreference = "Stop"

$projectRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
$cCompiler = (Get-Command gcc -ErrorAction Stop).Source
$cppCompiler = (Get-Command g++ -ErrorAction Stop).Source
$productionDir = Join-Path $projectRoot "main\boards\ep-chat-p4-ml307"
$productionObject = Join-Path $env:TEMP "mjpeg_present_sink_production.o"
$testBinary = Join-Path $env:TEMP "mjpeg_present_sink_production_test.exe"

$cCompileArgs = @(
    "-std=c11", "-Wall", "-Wextra", "-Werror", "-I$productionDir", "-c",
    (Join-Path $productionDir "mjpeg_present_sink.c"), "-o", $productionObject
)
$linkArgs = @(
    "-std=c++17", "-Wall", "-Wextra", "-Werror", "-I$productionDir",
    (Join-Path $projectRoot "tools\host_tests\mjpeg_present_sink_candidate_test.cc"),
    $productionObject, "-o", $testBinary
)

try {
    & $cCompiler @cCompileArgs
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    & $cppCompiler @linkArgs
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    & $testBinary
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
} finally {
    Remove-Item -LiteralPath $productionObject -Force -ErrorAction SilentlyContinue
    Remove-Item -LiteralPath $testBinary -Force -ErrorAction SilentlyContinue
}

exit 0
