# usage: run_hd.ps1 <route> <rideStartPresent> <modsDir> [ab=1] [step=200] [n=45]
param([string]$Route, [int]$Start, [string]$Mods, [int]$Ab = 1, [int]$Step = 200, [int]$N = 45, [int]$Timeout = 590)
$root = 'A:\AI\Recomps\new-pokemon-snap'
$at = @(); for ($i = 1; $i -le $N; $i++) { $p = $Start + $i * $Step; $at += $p; $at += ($p + 1) }
$env:NVN_FRAME_DUMP_AT = ($at -join ',')
$env:NVN_FRAME_DUMP_FULL = '1'
$env:SNAP_NEO_SPEED = "3"
$env:SNAP_WINDOW_W = '3840'; $env:SNAP_WINDOW_H = '2160'; $env:SNAP_RENDER_SCALE = '2'
$env:SNAP_MODS_DIR = $Mods
if ($Ab -gt 0) { $env:SNAP_MODS_AB = [string]$Ab; $env:SNAP_MODS_AB_ORIG = "$root\extracted\drp-v262144" }
& powershell -NoProfile -File "$root\host\run_boot.ps1" -Route $Route -Timeout $Timeout -ShotAt 1 -ShotEvery 1 -ShotMax ($N * 2)
