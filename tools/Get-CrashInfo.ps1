param(
    [Parameter(Position=0)]
    [string]$GtaPath = (Get-Location).Path
)

$ErrorActionPreference = 'Stop'

$GtaPath = (Resolve-Path -LiteralPath $GtaPath).Path
$targetDir = Join-Path $GtaPath 'cleo\debug\CrashInfo'
$target = Join-Path $targetDir 'EN-CrashList.txt'
$url = 'https://raw.githubusercontent.com/JuniorDjjr/CrashInfo/main/Lists/GTA-SA-10US/EN-CrashList.txt'

New-Item -ItemType Directory -Force -Path $targetDir | Out-Null
Invoke-WebRequest -Uri $url -OutFile $target

Write-Host "CrashInfo downloaded to: $target"
