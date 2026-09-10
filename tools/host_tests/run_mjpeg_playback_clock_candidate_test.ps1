$ErrorActionPreference = "Stop"

$projectRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
$cCompiler = (Get-Command gcc -ErrorAction Stop).Source
$cppCompiler = (Get-Command g++ -ErrorAction Stop).Source
$productionDir = Join-Path $projectRoot "main\boards\ep-chat-p4-ml307"
$clockObject = Join-Path $env:TEMP "mjpeg_playback_clock.o"
$adapterObject = Join-Path $env:TEMP "mjpeg_playback_clock_player_adapter.o"
$testBinary = Join-Path $env:TEMP "mjpeg_playback_clock_candidate_test.exe"

$cCompileArgs = @(
    "-std=c11",
    "-Wall",
    "-Wextra",
    "-Werror",
    "-I$productionDir",
    "-c",
    (Join-Path $productionDir "mjpeg_playback_clock.c"),
    "-o",
    $clockObject
)
$adapterCompileArgs = @(
    "-std=c11", "-Wall", "-Wextra", "-Werror", "-I$productionDir", "-c",
    (Join-Path $productionDir "mjpeg_playback_clock_player_adapter.c"),
    "-o", $adapterObject
)

$linkArgs = @(
    "-std=c++17",
    "-Wall",
    "-Wextra",
    "-Werror",
    "-I$productionDir",
    (Join-Path $projectRoot "tools\host_tests\mjpeg_playback_clock_candidate_test.cc"),
    $clockObject,
    $adapterObject,
    "-o",
    $testBinary
)

try {
    & $cCompiler @cCompileArgs
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    & $cCompiler @adapterCompileArgs
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    & $cppCompiler @linkArgs
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    & $testBinary
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
} finally {
    Remove-Item -LiteralPath $clockObject -Force -ErrorAction SilentlyContinue
    Remove-Item -LiteralPath $adapterObject -Force -ErrorAction SilentlyContinue
    Remove-Item -LiteralPath $testBinary -Force -ErrorAction SilentlyContinue
}

exit 0
