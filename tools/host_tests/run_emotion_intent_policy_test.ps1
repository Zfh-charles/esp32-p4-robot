$ErrorActionPreference = "Stop"

$projectRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
$compiler = (Get-Command g++ -ErrorAction Stop).Source
$testBinary = Join-Path $env:TEMP "xiaozhi_emotion_intent_policy_test.exe"

& $compiler `
    "-std=c++17" `
    "-Wall" `
    "-Wextra" `
    "-Werror" `
    (Join-Path $projectRoot "tools\host_tests\emotion_intent_policy_test.cc") `
    (Join-Path $projectRoot "main\domain\emotion_intent_policy.cc") `
    "-o" $testBinary

if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

& $testBinary
exit $LASTEXITCODE
