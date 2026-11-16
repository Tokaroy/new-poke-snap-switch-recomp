# Message-text button letters under the Xbox style: runs host\routes\btntext_scan.txt hidden (one run slot, run_boot.ps1) with SNAP_BUTTON_ICONS=xbox,
# SNAP_BTN_TEXT_LOG=1 and an empty SNAP_CONFIG_DIR (default binds, position layout). -Off sets SNAP_BTN_TEXT=0 (the game's own letters, for the before capture).
# Prints the [btntext] lines (the private-use codepoints the font was asked for). Frames: analysis\boot\route_btntext_scan_<run>_NN.png.
param([switch]$Off, [string]$BuildDir, [string]$Style = 'xbox')
$ErrorActionPreference = 'Stop'
$root = Resolve-Path (Join-Path $PSScriptRoot '..\..')
Set-Location $root
$cfg = Join-Path $root 'build\testconfig_btntext'
Remove-Item -Recurse -Force $cfg -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $cfg | Out-Null
$env:SNAP_CONFIG_DIR = $cfg; $env:SNAP_INPUT_BACKGROUND = '1'; $env:SNAP_ROUTE_LOG = '1'
$env:SNAP_BUTTON_ICONS = $Style; $env:SNAP_BTN_TEXT_LOG = '1'
if ($Off) { $env:SNAP_BTN_TEXT = '0' } else { Remove-Item Env:\SNAP_BTN_TEXT -ErrorAction SilentlyContinue }
$ra = @(); if ($BuildDir) { $ra += @('-BuildDir', $BuildDir) }
$out = powershell -NoProfile -File host\run_boot.ps1 -Route btntext_scan -Timeout 400 -ShotAt 110 -ShotEvery 12 -ShotMax 16 @ra | Tee-Object -Variable keep
$rid = ($keep | Select-String -Pattern '^run (\S+) on slot' | Select-Object -First 1).Matches[0].Groups[1].Value
$stderr = Join-Path $root "analysis\boot\runs\$rid\boot_stderr.log"
Select-String -Path $stderr -Pattern 'btntext|modhooks' | ForEach-Object { $_.Line } | Select-Object -First 100 | Write-Host
Write-Host "run $rid (Off=$Off Style=$Style)"
