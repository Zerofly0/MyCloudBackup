$ErrorActionPreference = "Stop"

$Root = Split-Path -Parent $PSScriptRoot
$Gxx = "C:\mingw64\bin\g++.exe"
$Build = Join-Path $Root "build"
$Source = Join-Path $Root "src\core\backup_core.cpp"
$Output = Join-Path $Build "backup_core.exe"

if (!(Test-Path $Gxx)) {
    throw "g++ not found at $Gxx. Install MinGW-w64 or update this script."
}

New-Item -ItemType Directory -Force -Path $Build | Out-Null
& $Gxx -std=c++17 -O2 -Wall -Wextra -municode -static -static-libgcc -static-libstdc++ -o $Output $Source
Write-Host "built $Output"
