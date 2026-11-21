# Same-view original vs Enhanced lighting frames (SNAP_LIGHT_AB=1: the lighting features are on in odd presents, off in even ones).
# usage: run_light_compare.ps1 <route> <rideStartPresent> [buildDir] [lighting=enhanced] [step=300] [n=4] [timeout=590] [extra env as one string name=value;name=value]
# Dumps present pairs (N, N+1) with N even from the ride start: N shows the Enhanced frame, N+1 the original (a frame dumped as p<N> carries the effects of present count N-1), one frame of rail motion apart.
# Pairs land in analysis\boot\runs\<id>\route_<route>_<id>_NN.png; scripts\light\pair_light.py copies them to analysis\lighting\compare\.
param([string]$Route, [int]$Start, [string]$BuildDir = '', [string]$Lighting = 'enhanced', [int]$Step = 300, [int]$N = 4, [int]$Timeout = 590, [string[]]$Extra = @())
$root = 'A:\AI\Recomps\new-pokemon-snap'
$at = @(); for ($i = 1; $i -le $N; $i++) { $p = $Start + $i * $Step; if ($p % 2) { $p++ }; $at += $p; $at += ($p + 1) }
$env:NVN_FRAME_DUMP_AT = ($at -join ',')
$env:NVN_FRAME_DUMP_FULL = '1'
$env:SNAP_NEO_SPEED = '3'
$env:SNAP_WINDOW_W = '3840'; $env:SNAP_WINDOW_H = '2160'; $env:SNAP_RENDER_SCALE = '2'
$env:SNAP_WINDOW_FRAME_CAP = '0'
$env:SNAP_LIGHTING = $Lighting
$env:SNAP_LIGHT_AB = '1'
$env:SNAP_FX_LOG = '1'
foreach ($e in ($Extra -split ';')) { if (-not $e) { continue }; $k, $v = $e -split '=', 2; Set-Item -Path "env:$k" -Value $v }
$a = @('-NoProfile', '-File', "$root\host\run_boot.ps1", '-Route', $Route, '-Timeout', $Timeout, '-ShotAt', 1, '-ShotEvery', 1, '-ShotMax', ($N * 2))
if ($BuildDir) { $a += @('-BuildDir', $BuildDir) }
& powershell @a
