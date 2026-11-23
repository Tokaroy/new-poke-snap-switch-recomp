# Same-photo score proof for HDR photo copies (the water proof, adapted). Runs route hdr_photos_ck: the ride runs with SNAP_HDR=scrgb, HD photos and SNAP_HDR_PHOTOS=1,
# a checkpoint at the start of the Evaluation, the photos scored with it on, then the option is flipped OFF through the dev control channel, the checkpoint is loaded and the same photos scored with it off.
# Compare the "shows photo <hash>" lines and the Score screenshots of the two passes in the run's analysis\boot\runs\<id>\ folder.
#   powershell -File host\hdr_photos_ck.ps1 [-BuildDir build\w\hp]
param([string]$BuildDir = '')
$root = Resolve-Path (Join-Path $PSScriptRoot '..')
Set-Location $root
. (Join-Path $PSScriptRoot 'safe_rm.ps1')
$ctl = Join-Path $root ('build\w\ctl_ck_' + $PID)
New-Item -ItemType Directory -Force $ctl | Out-Null
$env:SNAP_DEV = '1'; $env:SNAP_DEV_CTL = $ctl; $env:SNAP_NEO_SPEED = '3'
$env:SNAP_HDR = 'scrgb'; if (-not $env:SNAP_HD_PHOTOS) { $env:SNAP_HD_PHOTOS = '1' }; $env:SNAP_HDR_PHOTOS = '1'; $env:SNAP_HDPHOTO_LOG = '1'
$started = Get-Date
$args2 = @('-NoProfile', '-File', (Join-Path $root 'host\run_boot.ps1'), '-Route', 'hdr_photos_ck', '-Timeout', '600')
if ($BuildDir) { $args2 += @('-BuildDir', $BuildDir) }
$p = Start-Process powershell -ArgumentList $args2 -PassThru -WindowStyle Hidden -RedirectStandardOutput (Join-Path $root 'build\w\hdr_photos_ck.out')
$flipped = $false
while (-not $p.HasExited) {
    Start-Sleep -Seconds 2
    if ($flipped) { continue }
    $run = Get-ChildItem (Join-Path $root 'analysis\boot\runs') -Directory -ErrorAction SilentlyContinue | Where-Object { $_.LastWriteTime -gt $started -and (Select-String -Path (Join-Path $_.FullName 'boot_stderr.log') -SimpleMatch $ctl -Quiet -ErrorAction SilentlyContinue) } | Sort-Object LastWriteTime | Select-Object -Last 1
    if (-not $run) { continue }
    $err = Join-Path $run.FullName 'boot_stderr.log'
    if (-not (Test-Path $err)) { continue }
    $txt = Get-Content $err -Raw -ErrorAction SilentlyContinue
    if ($txt -match "wait 'ui=evaluation:End' met") {
        $info = Get-ChildItem $ctl -Filter '*.info' | Select-Object -First 1
        if (-not $info) { continue }
        $pid2 = $info.BaseName
        $cmd = 'set SNAP_HDR_PHOTOS 0'
        $cmd | Set-Content (Join-Path $ctl "$pid2.cmd.tmp")
        Move-Item (Join-Path $ctl "$pid2.cmd.tmp") (Join-Path $ctl "$pid2.cmd")
        $flipped = $true
        "flipped to Off at $(Get-Date -Format s) run=$($run.Name)" | Tee-Object (Join-Path $root 'build\w\hdr_photos_ck.flip')
    }
}
"exit code $($p.ExitCode)"
Remove-TreeSafe $ctl
