# Keyboard-and-mouse-only ride checks (hidden run, one run slot via host\run_boot.ps1), no controller token anywhere in the routes. Each run uses its own
# SNAP_CONFIG_DIR (build\testconfig_kbm), so the default binding table is what is tested:
#   (default)  host\routes\florio_kbm.txt        keyboard only, title to Score, the save and the album (no mouse token)
#   -Mouse     host\routes\florio_kbm_mouse.txt  keyboard + mouse: left button = shutter, right button and wheel = zoom, click probes in menus
#   -Click     host\routes\florio_kbm_click.txt  left clicks only: title, lab hub Research icon (probe: does the game take a click in menus)
#   -Items     host\routes\florio_kbm_items.txt  fixture unlock_all: left click shutter, right button zoom, J orb, K fruit, I scan
#   -Config <file>  start from a copy of that input.json instead of an empty config folder (old saved files: the defaults_rev migration)
# Needs a build that has the src= script token (a build of the tree with host/routes/rebind_shutter.txt working). Prints the route lines; exit 1 if the route did not reach its end.
param([switch]$Items, [switch]$Mouse, [switch]$Click, [string]$BuildDir, [string]$Config)
$ErrorActionPreference = 'Stop'
$root = Resolve-Path (Join-Path $PSScriptRoot '..\..')
Set-Location $root
$route = 'florio_kbm'; $shots = @('-ShotAt', '40', '-ShotEvery', '2', '-ShotMax', '45')
if ($Mouse) { $route = 'florio_kbm_mouse'; $shots = @('-ShotAt', '8', '-ShotEvery', '6', '-ShotMax', '90') }
if ($Click) { $route = 'florio_kbm_click'; $shots = @('-ShotAt', '10', '-ShotEvery', '4', '-ShotMax', '20') }
if ($Items) { $route = 'florio_kbm_items'; $shots = @('-ShotAt', '41', '-ShotEvery', '1', '-ShotMax', '30') }
$cfg = Join-Path $root 'build\testconfig_kbm'
Remove-Item -Recurse -Force $cfg -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $cfg | Out-Null
if ($Config) { Copy-Item $Config (Join-Path $cfg 'input.json') }
$env:SNAP_CONFIG_DIR = $cfg
$env:SNAP_INPUT_BACKGROUND = '1'; $env:SNAP_ROUTE_LOG = '1'
$ra = @(); if ($BuildDir) { $ra += @('-BuildDir', $BuildDir) }
$out = powershell -NoProfile -File host\run_boot.ps1 -Route $route -Timeout 590 @shots @ra | Tee-Object -Variable keep
if (-not ($keep | Select-String -Pattern '^run (\S+) on slot')) { Write-Host ($keep | Select-Object -Last 5); Write-Host "KBM RIDE FAILED: run_boot did not start a run"; exit 2 }
$rid = ($keep | Select-String -Pattern '^run (\S+) on slot' | Select-Object -First 1).Matches[0].Groups[1].Value
$stderr = Join-Path $root "analysis\boot\runs\$rid\boot_stderr.log"
if (-not (Test-Path $stderr)) { $stderr = Join-Path $root 'analysis\boot_stderr.log' }
$lines = @(Select-String -Path $stderr -Pattern "input script: (unknown|wait '|end of route|TIMEOUT)" | ForEach-Object { $_.Line })
$lines | Select-Object -Last 24 | Write-Host
$bad = @($lines | Where-Object { $_ -match 'unknown token|TIMEOUT' }).Count
$end = @($lines | Where-Object { $_ -match 'end of route reached' }).Count
if ($bad -gt 0 -or $end -eq 0) { Write-Host 'KBM RIDE FAILED'; exit 1 }
Write-Host "KBM RIDE PASSED ($route, run $rid)"
