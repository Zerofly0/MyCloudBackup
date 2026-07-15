//冒烟测试：备份、恢复、过滤规则、一致性
$ErrorActionPreference = "Stop"

$Root = Split-Path -Parent $PSScriptRoot
$BuildDir = Join-Path $Root "build"
$Core = Join-Path $BuildDir "Release\backup_core.exe"
if (!(Test-Path $Core)) {
    $Core = Join-Path $BuildDir "backup_core.exe"
}
if (!(Test-Path $Core)) {
    $Core = Join-Path $BuildDir "backup_core"
}
if (!(Test-Path $Core)) {
    throw "backup_core not found. Run cmake build first."
}

$Work = Join-Path $Root "tmp\smoke"
$Source = Join-Path $Work "source"
$Restore = Join-Path $Work "restore"
$Temp = Join-Path $Work "temp"
$Filter = Join-Path $Work "filter.json"
$Package = Join-Path $Temp "backup_test.bak.enc"

Remove-Item -LiteralPath $Work -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Path $Source, $Restore, $Temp | Out-Null
New-Item -ItemType Directory -Path (Join-Path $Source "docs") | Out-Null
"hello backup" | Set-Content -Encoding UTF8 -Path (Join-Path $Source "docs\hello.txt")
"ignore me" | Set-Content -Encoding UTF8 -Path (Join-Path $Source "skip.bin")
@{
    extensions = ".txt"
    nameContains = ""
    minSize = 0
    maxSize = 0
    modifiedAfter = ""
} | ConvertTo-Json | Set-Content -Encoding UTF8 -Path $Filter

& $Core backup --source $Source --output $Package --password "test-pass" --filter $Filter
& $Core restore --input $Package --restore $Restore --password "test-pass"

$Original = Get-Content -Raw -Path (Join-Path $Source "docs\hello.txt")
$Restored = Get-Content -Raw -Path (Join-Path $Restore "docs\hello.txt")
if ($Original -ne $Restored) {
    throw "restored content does not match"
}
if (Test-Path (Join-Path $Restore "skip.bin")) {
    throw "filter failed: skip.bin should not be restored"
}

Write-Host "smoke test passed"
