# Focus guard check (docs/controls.md "Switching to another app"): leaving the window for a snipping tool or Alt+Tab must not press Plus, B or the touch screen
# when it comes back. Hidden runs through host\run_boot.ps1 (run slots), one per scenario and guard setting:
#   menu  the lab hub main menu (no ride: Esc is B, Tab is Plus)      ride  the Florio ride (Esc and Tab are both Plus: the pause menu)
# Each run replays, with SNAP_WINDOW_TEST: blur, Esc down, Tab down (both while the window is in the background), focus, Esc up, Tab up (both still down at the focus change),
# then one real Tab press 2 s later. Guard on (the default): no Plus / B press and no pause menu before that last press, and the last press works.
# Guard off (SNAP_FOCUS_GUARD=0, the "before"): the held keys press Plus / B when the window comes back (the test would catch the bug). Counts come from
# "input: Plus pressed" / "B pressed" (SNAP_FOCUS_GUARD_LOG=1) and "route: ui scene 'pause_menu'" (SNAP_ROUTE_LOG=1) in boot_stderr.log.
# Takes build\focus_test.lock (a lock whose process is gone is taken over). Exit 0 = pass, 1 = failed, 4 = lock busy. Logs: analysis\ui\focus_test.log
param([string]$BuildDir, [string[]]$Scenarios = @('menu', 'ride'), [int[]]$Guard = @(0, 1), [switch]$Visible)
$ErrorActionPreference = 'Stop'
$root = Resolve-Path (Join-Path $PSScriptRoot '..\..')
Set-Location $root
if (Test-Path 'build\play.lock') { $pl = (Get-Content 'build\play.lock') -join ' '; if ($pl -match 'pid=(\d+)' -and (Get-Process -Id ([int]$Matches[1]) -ErrorAction SilentlyContinue)) { Write-Host "play.lock held: $pl"; exit 3 } }
. (Join-Path $PSScriptRoot 'test_lock.ps1')
$lock = 'build\focus_test.lock'
if (-not (Enter-TestLock $lock 'focus-test' 2000)) { Write-Host "FOCUS TEST NOT RUN: $lock is held by another run ($([IO.File]::ReadAllText($lock)))"; exit 4 }
$fails = 0
function Check($ok, $what) { if ($ok) { Write-Host "ok   $what" } else { Write-Host "FAIL $what"; $script:fails++ } }
$seq = 'blur@3,keydown:escape@4,keydown:tab@5,focus@6,keyup:escape@7,keyup:tab@8,keydown:tab@10,keyup:tab@10.4'
try {
  New-Item -ItemType Directory -Force 'analysis\ui' | Out-Null
  Remove-Item 'analysis\ui\focus_test.log' -ErrorAction SilentlyContinue
  $cfg = Join-Path $root 'build\testconfig_focus'
  Remove-Item -Recurse -Force $cfg -ErrorAction SilentlyContinue
  New-Item -ItemType Directory -Force $cfg | Out-Null
  '{"version":2,"mouse":{"lock_cursor":false}}' | Set-Content (Join-Path $cfg 'input.json')
  $env:SNAP_CONFIG_DIR = $cfg
  $env:SNAP_PROFILE_PICKER = '0'
  $env:SNAP_WINDOW_PAUSE = '1'          # a real blur freezes the game: the keys are first seen by the poll after the focus is back
  $env:SNAP_FOCUS_GUARD_LOG = '1'; $env:SNAP_ROUTE_LOG = '1'
  foreach ($sc in $Scenarios) {
    $cond = if ($sc -eq 'ride') { 'ride>=25' } else { 'ui=main_menu' }
    foreach ($g in $Guard) {
      $env:SNAP_FOCUS_GUARD = [string]$g
      $env:SNAP_WINDOW_TEST = "wait:$cond,$seq"
      $ra = @(); if ($Visible) { $ra += '-Visible' }; if ($BuildDir) { $ra += @('-BuildDir', $BuildDir) }
      $stderr = $null
      for ($try = 1; $try -le 3 -and -not $stderr; $try++) {   # a run slot can be closed (the owner is using the PC) or lost to a start-up address clash: ask again
        powershell -NoProfile -File host\run_boot.ps1 -Route "focus_$sc" -Timeout 590 @ra | Tee-Object -Variable keep | Out-Host
        $m = $keep | Select-String -Pattern '^run (\S+) on slot' | Select-Object -First 1
        $rid = if ($m) { $m.Matches[0].Groups[1].Value } else { '' }
        $cand = if ($rid) { Join-Path $root "analysis\boot\runs\$rid\boot_stderr.log" } else { '' }
        if ($cand -and (Test-Path $cand) -and (Select-String -Path $cand -Pattern 'input script: .* entries' -Quiet)) { $stderr = $cand } else { Write-Host "$sc guard=$g : try $try gave no usable run ($rid)"; Start-Sleep -Seconds 20 }
      }
      if (-not $stderr) { throw "$sc guard=$g : no usable run after 3 tries" }
      $name = "$sc guard=$g"
      "=== $name (run $rid)" | Add-Content 'analysis\ui\focus_test.log'
      $before = 0; $after = 0; $inSeq = $false; $final = $false; $sawBlur = $false; $sawFocusBack = $false; $sawWait = $false
      foreach ($l in (Get-Content $stderr)) {
        if ($l -match '\[window\] test: wait:.* met') { $sawWait = $true }
        if ($l -match '\[window\] test: (blur|focus|keydown:|keyup:)') { $inSeq = $true; if ($l -match 'test: blur') { $sawBlur = $true } }
        if ($l -match '\[window\] test: keydown:tab at t=10\.0') { $final = $true }
        if ($l -match 'input: focus is back') { $sawFocusBack = $true }
        if ($l -match '\[window\] test|input: (Plus|B) pressed|input: focus is back|route: ui scene ''pause_menu''') {
          "$($l.Substring(0, [math]::Min($l.Length, 200)))" | Add-Content 'analysis\ui\focus_test.log'
          if ($inSeq -and $l -match 'input: (Plus|B) pressed|route: ui scene ''pause_menu''') { if ($final) { $after++ } else { $before++ } }
        }
      }
      Write-Host "$name : presses and pause menus before the final Tab: $before, from the final Tab on: $after"
      Check ($sawWait -and $sawBlur) "$name : the game reached the scene and the window test ran"
      if ($g -eq 1) {
        Check ($sawFocusBack) "$name : the guard saw the focus come back with keys held"
        Check ($before -eq 0) "$name : nothing pressed and no pause menu while the window was away and when it came back"
        Check ($after -ge 1) "$name : a real Tab press afterwards still works"
      } else {
        Check ($before -ge 1) "$name : without the guard the held keys do press (the test catches the bug)"
      }
    }
  }
} finally { Exit-TestLock $lock }
if ($fails) { Write-Host "FOCUS TEST FAILED: $fails check(s) (see analysis\ui\focus_test.log)"; exit 1 }
Write-Host 'FOCUS TEST PASSED'
