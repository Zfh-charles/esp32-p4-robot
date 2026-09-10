$ErrorActionPreference = "Stop"

$projectRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
$cCompiler = (Get-Command gcc -ErrorAction Stop).Source
$cppCompiler = (Get-Command g++ -ErrorAction Stop).Source
$candidateDir = Join-Path $projectRoot "tools\validation\candidates"
$hostDir = Join-Path $projectRoot "tools\host_tests"
$candidateObject = Join-Path $env:TEMP "mjpeg_decoder_direct_candidate.o"
$testBinary = Join-Path $env:TEMP "mjpeg_decoder_direct_candidate_test.exe"

try {
    & $cCompiler -std=c11 -O2 -Wall -Wextra -Werror `
        -DMJPEG_DECODER_DIRECT_HOST_TEST "-I$candidateDir" "-I$hostDir" `
        -c (Join-Path $candidateDir "mjpeg_decoder_direct_candidate.c") `
        -o $candidateObject
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    & $cppCompiler -std=c++17 -O2 -Wall -Wextra -Werror `
        -DMJPEG_DECODER_DIRECT_HOST_TEST "-I$candidateDir" "-I$hostDir" `
        (Join-Path $hostDir "mjpeg_decoder_direct_candidate_test.cc") `
        $candidateObject -o $testBinary
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    & $testBinary
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
} finally {
    Remove-Item -LiteralPath $candidateObject -Force -ErrorAction SilentlyContinue
    Remove-Item -LiteralPath $testBinary -Force -ErrorAction SilentlyContinue
}

exit 0
