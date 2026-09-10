param(
    [Parameter(Mandatory = $true)]
    [string]$Path,
    [ValidateRange(4096, 8388608)]
    [int]$Bytes = 262144,
    [string]$Pattern = ""
)

$ErrorActionPreference = "Stop"
$resolved = (Resolve-Path -LiteralPath $Path).Path
$share = [System.IO.FileShare]::ReadWrite -bor [System.IO.FileShare]::Delete
$stream = [System.IO.FileStream]::new(
    $resolved,
    [System.IO.FileMode]::Open,
    [System.IO.FileAccess]::Read,
    $share
)

try {
    $count = [int][Math]::Min([int64]$Bytes, $stream.Length)
    [void]$stream.Seek(-$count, [System.IO.SeekOrigin]::End)
    $buffer = [byte[]]::new($count)
    $read = 0
    while ($read -lt $count) {
        $n = $stream.Read($buffer, $read, $count - $read)
        if ($n -le 0) { break }
        $read += $n
    }
} finally {
    $stream.Dispose()
}

$text = [System.Text.Encoding]::UTF8.GetString($buffer, 0, $read)
if ($count -lt (Get-Item -LiteralPath $resolved).Length) {
    $firstNewline = $text.IndexOf("`n")
    if ($firstNewline -ge 0) {
        $text = $text.Substring($firstNewline + 1)
    }
}

$lines = $text -split "`r?`n"
if ([string]::IsNullOrEmpty($Pattern)) {
    $lines
} else {
    $lines | Where-Object { $_ -match $Pattern }
}
