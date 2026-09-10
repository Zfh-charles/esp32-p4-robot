$ErrorActionPreference = "Stop"

$projectRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
$compiler = (Get-Command g++ -ErrorAction Stop).Source
$tests = @(
    "face_state_reducer_test.cc",
    "face_state_reducer_replay_test.cc",
    "visual_commit_scheduler_test.cc"
)

foreach ($test in $tests) {
    $testBinary = Join-Path $env:TEMP (([IO.Path]::GetFileNameWithoutExtension($test)) + ".exe")
    try {
        & $compiler `
            "-std=c++17" `
            "-Wall" `
            "-Wextra" `
            "-Werror" `
            (Join-Path $projectRoot "tools\host_tests\$test") `
            "-o" $testBinary

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
}

exit 0
