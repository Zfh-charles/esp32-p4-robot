$ErrorActionPreference = "Stop"

$projectRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
$cCompiler = (Get-Command gcc -ErrorAction Stop).Source
$cppCompiler = (Get-Command g++ -ErrorAction Stop).Source
$productionDir = Join-Path $projectRoot "main\boards\ep-chat-p4-ml307"
$candidate = Join-Path $projectRoot "tools\validation\candidates\mjpeg_playback_clock_zero_tax_candidate.c"
$clockObject = Join-Path $env:TEMP "mjpeg_playback_clock_zero_tax.o"
$adapterObject = Join-Path $env:TEMP "mjpeg_playback_clock_zero_tax_adapter.o"
$testBinary = Join-Path $env:TEMP "mjpeg_playback_clock_zero_tax_test.exe"

try {
    & $cCompiler -std=c11 -O2 -Wall -Wextra -Werror "-I$productionDir" -c $candidate -o $clockObject
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    & $cCompiler -std=c11 -O2 -Wall -Wextra -Werror "-I$productionDir" -c `
        (Join-Path $productionDir "mjpeg_playback_clock_player_adapter.c") -o $adapterObject
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    & $cppCompiler -std=c++17 -O2 -Wall -Wextra -Werror "-I$productionDir" `
        (Join-Path $projectRoot "tools\host_tests\mjpeg_playback_clock_candidate_test.cc") `
        $clockObject $adapterObject -o $testBinary
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    & $testBinary
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
} finally {
    Remove-Item -LiteralPath $clockObject -Force -ErrorAction SilentlyContinue
    Remove-Item -LiteralPath $adapterObject -Force -ErrorAction SilentlyContinue
    Remove-Item -LiteralPath $testBinary -Force -ErrorAction SilentlyContinue
}

exit 0
