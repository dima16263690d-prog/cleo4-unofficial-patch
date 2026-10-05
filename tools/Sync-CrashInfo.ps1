# Sync the established GTA SA 1.0 US CrashInfo database used by DebugUtils.
# The upstream project is MIT-licensed:
# https://github.com/JuniorDjjr/CrashInfo
#
# Run from the repository root:
#   powershell -ExecutionPolicy Bypass -File .\tools\Sync-CrashInfo.ps1
#
$ErrorActionPreference = "Stop"

$repoRoot = Split-Path -Parent $PSScriptRoot
$targetDir = Join-Path $repoRoot "Plugins\DebugUtils\CrashInfo"
$target = Join-Path $targetDir "GTA-SA-10US-EN-CrashList.txt"
$url = "https://raw.githubusercontent.com/JuniorDjjr/CrashInfo/main/Lists/GTA-SA-10US/EN-CrashList.txt"

New-Item -ItemType Directory -Force -Path $targetDir | Out-Null
Invoke-WebRequest -Uri $url -OutFile $target -UseBasicParsing

$lines = Get-Content -LiteralPath $target
$errorCount = @($lines | Where-Object { $_ -like "Error: *" }).Count

Write-Host "CrashInfo synced:"
Write-Host "  Source: $url"
Write-Host "  Target: $target"
Write-Host "  Signatures: $errorCount"
