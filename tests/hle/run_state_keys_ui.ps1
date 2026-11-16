# Save-state hotkeys through the real input path: ui_state_keys_script.txt injects F5 at 60 s and F8 at 67 s (UI-test "press key:..."), the hub sees them like physical keys.
# Pass: boot_stderr.log has "state save 'player': ok" then "state load 'player': ok". Overlay frames go to analysis\ui\state_dump.
param([string]$BuildDir)
$ErrorActionPreference = 'Stop'
$root = Resolve-Path (Join-Path $PSScriptRoot '..\..')
Set-Location $root
$scr = $PSScriptRoot
$cfg = Join-Path $root 'build\testconfig_st'
Remove-Item -Recurse -Force $cfg -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $cfg, 'analysis\ui' | Out-Null
'{"version":2,"mouse":{"lock_cursor":false}}' | Set-Content (Join-Path $cfg 'input.json')
$pdir = Join-Path $root 'build\testprofiles_st'
Remove-Item -Recurse -Force $pdir -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force (Join-Path $pdir 'p1\save') | Out-Null
$now = [int][double]::Parse((Get-Date -UFormat %s))
@"
{"version": 1, "next": 2, "last_used": "p1", "profiles": [
 {"id": "p1", "name": "State test", "created": $($now - 864000), "last_played": $($now - 86400)}], "pending": []}
"@ | Set-Content (Join-Path $pdir 'profiles.json') -Encoding ascii
$dump = Join-Path $root 'analysis\ui\state_dump'
Remove-Item -Recurse -Force $dump -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $dump | Out-Null
$env:SNAP_PROFILES_DIR = $pdir; $env:SNAP_PROFILE = 'State test'; $env:SNAP_PROFILE_PICKER = '0'
$env:SNAP_SAVE_DIR = (Join-Path $root 'build\testsave_st')
$env:SNAP_WINDOW_SAVE = '1'; $env:SNAP_CONFIG_DIR = $cfg; $env:SNAP_MODS_DIR = (Join-Path $cfg 'mods'); $env:SNAP_INPUT_BACKGROUND = '1'
$env:SNAP_WINDOW_W = '1920'; $env:SNAP_WINDOW_H = '1080'; $env:SNAP_SHADER_PRECOMPILE = 'playing'
$env:NVN_FRAME_DUMP = $dump; $env:NVN_FRAME_DUMP_POST = '1'; $env:NVN_FRAME_DUMP_FULL = '1'
$env:NVN_FRAME_DUMP_FROM = '60'; $env:NVN_FRAME_DUMP_EVERY = '1.5'; $env:NVN_FRAME_DUMP_MAX = '14'
$env:SNAP_UI_TEST = Join-Path $scr 'ui_state_keys_script.txt'
Remove-Item Env:SNAP_CHECKPOINT -ErrorAction SilentlyContinue
$ra = @(); if ($BuildDir) { $ra += @('-BuildDir', $BuildDir) }
$out = powershell -NoProfile -File host\run_boot.ps1 -Timeout 200 @ra run | Tee-Object -Variable keep
$rid = ($keep | Select-String -Pattern '^run (\S+) on slot' | Select-Object -First 1).Matches[0].Groups[1].Value
$stderr = Join-Path $root "analysis\boot\runs\$rid\boot_stderr.log"
Write-Host "run $rid"
Select-String -Path $stderr -Pattern 'ui-test|state (save|load)|nvn: state' | ForEach-Object { $_.Line } | Select-Object -First 20 | Write-Host
