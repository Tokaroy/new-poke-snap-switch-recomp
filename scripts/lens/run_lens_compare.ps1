# Same-moment Off/On frames of the lens effects (SNAP_LIGHT_AB=1: the effects are on in odd presents, off in even ones; nvn_lens_fx.cpp reads the same switch).
# usage: run_lens_compare.ps1 <route> <presentList> [buildDir] [effect=dof|mblur|both] [timeout=590] [extra env as one string name=value;name=value]
# <presentList> is a comma list of EVEN present numbers N: the script dumps N (effect on) and N+1 (effect off), one frame of ride motion apart
# (a frame dumped as p<N> carries the effects of present count N-1, as in scripts\light\run_light_compare.ps1).
# 4K output at render scale 2, 3x NEO-ONE speed is NOT used (the zoom timing in lens_zoom.txt is in presents, so the ride runs at its own speed).
# Frames land in analysis\boot\runs\<id>\route_<route>_<id>_NN.png; scripts\lens\strip_lens.py joins them into the Off/On strip in analysis\lens\.
param([string]$Route, [string]$At, [string]$BuildDir = '', [string]$Effect = 'dof', [int]$Timeout = 590, [string[]]$Extra = @())
$root = 'F:\AI\Recomps\new-pokemon-snap'
$pairs = @(); foreach ($p in ($At -split ',')) { $n = [int]$p; if ($n % 2) { $n++ }; $pairs += $n; $pairs += ($n + 1) }
$env:NVN_FRAME_DUMP_AT = ($pairs -join ',')
$env:NVN_FRAME_DUMP_FULL = '1'
$env:SNAP_WINDOW_W = '3840'; $env:SNAP_WINDOW_H = '2160'; $env:SNAP_RENDER_SCALE = '2'
$env:SNAP_WINDOW_FRAME_CAP = '0'
$env:SNAP_LIGHT_AB = '1'
$env:SNAP_LENS_LOG = '1'
if ($Effect -eq 'dof' -or $Effect -eq 'both') { $env:SNAP_BOKEH_DOF = '1' }
if ($Effect -eq 'mblur' -or $Effect -eq 'both') { $env:SNAP_MOTION_BLUR = '1' }
foreach ($e in ($Extra -split ';')) { if (-not $e) { continue }; $k, $v = $e -split '=', 2; Set-Item -Path "env:$k" -Value $v }
$a = @('-NoProfile', '-File', "$root\host\run_boot.ps1", '-Route', $Route, '-Timeout', $Timeout, '-ShotAt', 1, '-ShotEvery', 1, '-ShotMax', ($pairs.Count))
if ($BuildDir) { $a += @('-BuildDir', $BuildDir) }
& powershell @a
