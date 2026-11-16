# Experimental rows test: boots the real game hidden (host\run_boot.ps1) on a throw-away profile with tests\hle\ui_rt_rows_script.txt and checks the ui-test lines.
# Frames with the menu overlay go to analysis\ui\rt_rows_dump (use them as a look at the page). One run slot; refuses while build\play.lock is live.
param([string]$BuildDir, [switch]$Visible)
$ErrorActionPreference = 'Stop'
$root = Resolve-Path (Join-Path $PSScriptRoot '..\..')
Set-Location $root
if (Test-Path 'build\play.lock') { $pl = (Get-Content 'build\play.lock') -join ' '; if ($pl -match 'pid=(\d+)' -and (Get-Process -Id ([int]$Matches[1]) -ErrorAction SilentlyContinue)) { Write-Host "play.lock is live ($pl): not running"; exit 4 } }
$cfg = Join-Path $root 'build\testconfig_rtrows'; $pdir = Join-Path $root 'build\testprofiles_rtrows'; $dump = Join-Path $root 'analysis\ui\rt_rows_dump'
Remove-Item -Recurse -Force $cfg, $pdir, $dump -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $cfg, (Join-Path $pdir 'p1\save'), $dump, 'analysis\ui' | Out-Null
'{"version":2,"mouse":{"lock_cursor":false}}' | Set-Content (Join-Path $cfg 'input.json')
$now = [int][double]::Parse((Get-Date -UFormat %s))
@"
{"version": 1, "next": 2, "last_used": "p1", "profiles": [
 {"id": "p1", "name": "RT rows test", "created": $($now - 864000), "last_played": $($now - 86400)}], "pending": []}
"@ | Set-Content (Join-Path $pdir 'profiles.json') -Encoding ascii
$env:SNAP_UI_TEST = (Join-Path $root 'tests\hle\ui_rt_rows_script.txt'); Remove-Item Env:\SNAP_UI_TEST_RESTART -ErrorAction SilentlyContinue
$env:SNAP_PROFILES_DIR = $pdir; $env:SNAP_PROFILE = 'RT rows test'; $env:SNAP_SAVE_DIR = (Join-Path $root 'build\testsave_rtrows'); $env:SNAP_WINDOW_SAVE = '1'
$env:SNAP_CONFIG_DIR = $cfg; $env:SNAP_INPUT_BACKGROUND = '1'; $env:SNAP_WINDOW_W = '1920'; $env:SNAP_WINDOW_H = '1080'; $env:SNAP_PROFILE_PICKER = '0'
$env:SNAP_SHADER_PRECOMPILE = 'playing'; $env:NVN_FRAME_DUMP_POST = '1'; $env:NVN_FRAME_DUMP_FULL = '1'
$env:NVN_FRAME_DUMP = $dump; $env:NVN_FRAME_DUMP_FROM = '22'; $env:NVN_FRAME_DUMP_EVERY = '2'; $env:NVN_FRAME_DUMP_MAX = '30'
$ra = @(); if ($Visible) { $ra += '-Visible' }; if ($BuildDir) { $ra += @('-BuildDir', $BuildDir) }
$keep = powershell -NoProfile -File host\run_boot.ps1 -Timeout 200 @ra run | Tee-Object -Variable keep
$rid = ($keep | Select-String -Pattern '^run (\S+) on slot' | Select-Object -First 1).Matches[0].Groups[1].Value
if (-not $rid) { throw 'no run id' }
$stderr = Join-Path $root "analysis\boot\runs\$rid\boot_stderr.log"
$u = @(Select-String -Path $stderr -Pattern 'ui-test' | ForEach-Object { $_.Line })
$u | Select-Object -Last 40 | Write-Host
$nfail = @($u | Where-Object { $_ -match ' FAIL\b' -or $_ -match 'SUMMARY \d+ checks, [1-9]\d* failed' }).Count
$nsum = @($u | Where-Object { $_ -match 'SUMMARY \d+ checks, 0 failed' }).Count
if ($nfail -gt 0) { Write-Host "RT ROWS TEST FAILED ($nfail failing line(s), run $rid)"; exit 1 }
elseif ($nsum -eq 0) { Write-Host "RT ROWS TEST FAILED: no SUMMARY line (run $rid)"; exit 1 }
else { Write-Host "RT ROWS TEST PASSED (run $rid)" }
