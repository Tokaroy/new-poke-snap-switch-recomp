# Remapped shutter key, end to end (hidden run, one run slot via host\run_boot.ps1): input.json binds A to pad:east and key:P only; host\routes\rebind_shutter.txt rides Florio,
# holds the old default key L (no photo may be taken), then P (a photo is taken: the film counter drops from 72). Own config folder: build\testconfig_rebind.
# Prints the route lines of the run; exit 1 if the route did not reach its end.
param([string]$BuildDir)
$ErrorActionPreference = 'Stop'
$root = Resolve-Path (Join-Path $PSScriptRoot '..\..')
Set-Location $root
$cfg = Join-Path $root 'build\testconfig_rebind'
Remove-Item -Recurse -Force $cfg -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $cfg | Out-Null
'{"version":2,"mouse":{"lock_cursor":false},"buttons":{"A":["pad:east","key:P"]}}' | Set-Content (Join-Path $cfg 'input.json')
$env:SNAP_CONFIG_DIR = $cfg; $env:SNAP_INPUT_BACKGROUND = '1'; $env:SNAP_ROUTE_LOG = '1'
$ra = @(); if ($BuildDir) { $ra += @('-BuildDir', $BuildDir) }
$out = powershell -NoProfile -File host\run_boot.ps1 -Route rebind_shutter -Timeout 400 @ra | Tee-Object -Variable keep
$rid = ($keep | Select-String -Pattern '^run (\S+) on slot' | Select-Object -First 1).Matches[0].Groups[1].Value
$stderr = Join-Path $root "analysis\boot\runs\$rid\boot_stderr.log"
$lines = @(Select-String -Path $stderr -Pattern "input script: (wait '|end of route|TIMEOUT)|route: film counter|input script: seg \d+ t=.* buttons=0x[1-9a-f]" | ForEach-Object { $_.Line })
$lines | Select-Object -Last 14 | Write-Host
$ok = @($lines | Where-Object { $_ -match "wait '!film<=71' met" }).Count -gt 0 -and @($lines | Where-Object { $_ -match "wait 'film<=71' met" }).Count -gt 0 -and @($lines | Where-Object { $_ -match 'end of route reached' }).Count -gt 0
if (-not $ok) { Write-Host 'REBIND PROOF FAILED'; exit 1 }
Write-Host "REBIND PROOF PASSED (run $rid)"
