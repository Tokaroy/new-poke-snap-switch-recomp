# Cheats tab check: boots the game (hidden, one run slot via run_boot.ps1) with the hand-written cheat file in tests\cheats, runs ui_cheats_script.txt (F1 > Cheats) and
# dumps the overlay frames (NVN_FRAME_DUMP_POST) to analysis\ui\cheats_dump. Own files: build\testconfig_chk, build\testprofiles_chk, build\testsave_chk.
# Prints the ui-test lines of the run; exit 1 on a failed check or a missing SUMMARY line.
param([string]$BuildDir)
$ErrorActionPreference = 'Stop'
$root = Resolve-Path (Join-Path $PSScriptRoot '..\..')
Set-Location $root
$cfg = Join-Path $root 'build\testconfig_chk'
Remove-Item -Recurse -Force $cfg -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $cfg, 'analysis\ui' | Out-Null
'{"version":2,"mouse":{"lock_cursor":false}}' | Set-Content (Join-Path $cfg 'input.json')
$pdir = Join-Path $root 'build\testprofiles_chk'
Remove-Item -Recurse -Force $pdir -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force (Join-Path $pdir 'p1\save') | Out-Null
$now = [int][double]::Parse((Get-Date -UFormat %s))
@"
{"version": 1, "next": 2, "last_used": "p1", "profiles": [
 {"id": "p1", "name": "Cheats test", "created": $($now - 864000), "last_played": $($now - 86400)}], "pending": []}
"@ | Set-Content (Join-Path $pdir 'profiles.json') -Encoding ascii
$dump = Join-Path $root 'analysis\ui\cheats_dump'
Remove-Item -Recurse -Force $dump -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $dump | Out-Null
$env:SNAP_PROFILES_DIR = $pdir; $env:SNAP_PROFILE = 'Cheats test'; $env:SNAP_PROFILE_PICKER = '0'
$env:SNAP_SAVE_DIR = (Join-Path $root 'build\testsave_chk')
$env:SNAP_WINDOW_SAVE = '1'; $env:SNAP_CONFIG_DIR = $cfg; $env:SNAP_MODS_DIR = (Join-Path $cfg 'mods'); $env:SNAP_INPUT_BACKGROUND = '1'
$env:SNAP_WINDOW_W = '1920'; $env:SNAP_WINDOW_H = '1080'; $env:SNAP_SHADER_PRECOMPILE = 'playing'
$env:NVN_FRAME_DUMP = $dump; $env:NVN_FRAME_DUMP_POST = '1'; $env:NVN_FRAME_DUMP_FULL = '1'
$env:NVN_FRAME_DUMP_FROM = '22'; $env:NVN_FRAME_DUMP_EVERY = '2'; $env:NVN_FRAME_DUMP_MAX = '12'
$env:SNAP_UI_TEST = Join-Path $root 'tests\hle\ui_cheats_script.txt'
$env:SNAP_CHEATS = '1'; $env:SNAP_CHEATS_DIR = (Join-Path $root 'tests\cheats'); $env:SNAP_CHEATS_ON = 'Log main header while ZL is held'
$ra = @(); if ($BuildDir) { $ra += @('-BuildDir', $BuildDir) }
$out = powershell -NoProfile -File host\run_boot.ps1 -Timeout 200 @ra run | Tee-Object -Variable keep
$rid = ($keep | Select-String -Pattern '^run (\S+) on slot' | Select-Object -First 1).Matches[0].Groups[1].Value
$stderr = Join-Path $root "analysis\boot\runs\$rid\boot_stderr.log"
$lines = @(Select-String -Path $stderr -Pattern 'ui-test' | ForEach-Object { $_.Line -replace '^\[ *[0-9.]+\] \[hle\] ', '' })
$lines | Write-Host
$bad = @($lines | Where-Object { $_ -match ' FAIL\b' -or $_ -match 'SUMMARY \d+ checks, [1-9]\d* failed' }).Count
$sum = @($lines | Where-Object { $_ -match 'SUMMARY \d+ checks, 0 failed' }).Count
if ($bad -gt 0) { Write-Host 'CHEATS UI TEST FAILED'; exit 1 }
if ($sum -eq 0) { Write-Host 'CHEATS UI TEST FAILED: no SUMMARY line'; exit 1 }
Write-Host 'CHEATS UI TEST PASSED'
