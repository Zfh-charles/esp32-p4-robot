$ErrorActionPreference = "Stop"

$projectRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
$cCompiler = (Get-Command gcc -ErrorAction Stop).Source
$cppCompiler = (Get-Command g++ -ErrorAction Stop).Source
$sourceDir = Join-Path $projectRoot "main\boards\ep-chat-p4-ml307"
$stageObject = Join-Path $env:TEMP "mjpeg_decoder_stage.o"
$testBinary = Join-Path $env:TEMP "mjpeg_decoder_stage_test.exe"

$cCompileArgs = @(
    "-std=c11",
    "-Wall",
    "-Wextra",
    "-Werror",
    "-I$sourceDir",
    "-c",
    (Join-Path $sourceDir "mjpeg_decoder_stage.c"),
    "-o",
    $stageObject
)

$linkArgs = @(
    "-std=c++17",
    "-Wall",
    "-Wextra",
    "-Werror",
    "-I$sourceDir",
    (Join-Path $projectRoot "tools\host_tests\mjpeg_decoder_candidate_test.cc"),
    $stageObject,
    "-o",
    $testBinary
)

try {
    & $cCompiler @cCompileArgs
    if ($LASTEXITCODE -ne 0) {
        exit $LASTEXITCODE
    }
    & $cppCompiler @linkArgs
    if ($LASTEXITCODE -ne 0) {
        exit $LASTEXITCODE
    }
    & $testBinary
    if ($LASTEXITCODE -ne 0) {
        exit $LASTEXITCODE
    }
} finally {
    Remove-Item -LiteralPath $stageObject -Force -ErrorAction SilentlyContinue
    Remove-Item -LiteralPath $testBinary -Force -ErrorAction SilentlyContinue
}

exit 0
