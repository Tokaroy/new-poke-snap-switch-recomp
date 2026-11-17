# One command: build the game for a player or for the owner's PC, ready for host\play.ps1.
#   powershell -NoProfile -File host\player_build.ps1                 build main + sdk libs and the exe (PGO when build\pgo\snap.profdata exists), run the HLE tests
#   powershell -NoProfile -File host\player_build.ps1 -Regenerate     regenerate the lifted C first (after lifter, hook or ABI edits), then build
#   powershell -NoProfile -File host\player_build.ps1 -Retrain        retrain the PGO profile on the current generated code first (about 25 min), then build
#   -NoPgo (libs and exe without the profile), -SkipTests, -Jobs <n>, -DryRun (print the steps, run nothing)
# Writes: build\eh\sdk, build\eh\main, build\snap (the shared outputs host\play.ps1 copies). Each step takes build\heavy.lock on its own
# (a lock goes stale after 20 minutes, so no step holds it longer than one compile), runs at BelowNormal priority and releases it. Never
# deletes anything. The PGO profile (build\pgo\snap.profdata) is local and derived from game code: it is never committed or shipped.
# Then: powershell -File host\play.ps1
[CmdletBinding(PositionalBinding=$false)]
param([switch]$Regenerate, [switch]$Retrain, [switch]$NoPgo, [switch]$SkipTests, [switch]$DryRun, [int]$Jobs = 16, [int]$TrainTimeout = 590)
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
Set-Location $root
. (Join-Path $PSScriptRoot 'locks.ps1')
if (-not $env:SNAP_LOCK_OWNER) { $env:SNAP_LOCK_OWNER = 'player_build' }
$py = Join-Path $root 'tools\venv-recomp\Scripts\python.exe'
if (-not (Test-Path $py)) { throw "$py not found (tools\venv-recomp is missing)" }
$prof = Join-Path $root 'build\pgo\snap.profdata'
$t0 = Get-Date

function Step([string]$what, [scriptblock]$body) {
    Write-Host ("[{0:mm\:ss}] {1}" -f ((Get-Date) - $t0), $what)
    if ($DryRun) { return }
    if (-not (Take-BuildLock "player_build: $what" 1800)) { throw "could not take build\heavy.lock for: $what" }
    try {
        $global:LASTEXITCODE = 0
        & $body
        if ($LASTEXITCODE -ne 0) { throw "step failed ($LASTEXITCODE): $what" }
    } finally { Release-Lock (Join-Path $script:LockRoot 'build\heavy.lock') }
}
function Py([string[]]$a) { & $py -I @a; if ($LASTEXITCODE -ne 0) { throw "python $($a -join ' ') exited $LASTEXITCODE" } }

if ($Regenerate) {
    foreach ($m in 'sdk', 'main') { Step "regenerate $m" { Py @('recomp\snap_gen.py', '--module', $m) } }
}

if ($Retrain) {
    $gen = 'build\w\player_gen'
    $flags = '-march=x86-64-v3 -fprofile-generate'
    Step 'instrumented sdk lib' { Py @('recomp\snap_build.py', '--gen', 'generated\v262144\sdk', '--out', "$gen\sdk", '--lib', 'snap_sdk', '--jobs', $Jobs, '--cflags', $flags) }
    Step 'instrumented main lib' { Py @('recomp\snap_build.py', '--gen', 'generated\v262144\main', '--out', "$gen\main", '--lib', 'snap_main', '--jobs', $Jobs, '--cflags', $flags) }
    Step 'instrumented exe' { Py @('host\build_snap.py', '--out', "$gen\exe", '--pgo-gen', '--main-lib', "$gen\main\lib\snap_main.lib", '--sdk-lib', "$gen\sdk\lib\snap_sdk.lib") }
    $raw = Join-Path $root 'build\w\player_gen\raw'
    if (-not $DryRun) {
        if (Test-Path $raw) { Get-ChildItem $raw -Filter *.profraw | Remove-Item -Force }   # only our own counter files, no recursion
        New-Item -ItemType Directory -Force $raw | Out-Null
    }
    Write-Host "[$(Get-Date -f mm:ss)] training run (florio_photos, at most $TrainTimeout s)"
    if (-not $DryRun) {
        $env:LLVM_PROFILE_FILE = Join-Path $raw 'snap-%p.profraw'
        & powershell -NoProfile -File (Join-Path $PSScriptRoot 'run_boot.ps1') -Route florio_photos -BuildDir $gen\exe -Timeout $TrainTimeout | Out-Host
        Remove-Item Env:LLVM_PROFILE_FILE -ErrorAction SilentlyContinue
        if (-not (Get-ChildItem $raw -Filter *.profraw -ErrorAction SilentlyContinue)) { throw 'the training run wrote no .profraw files' }
    }
    Step 'merge the profile' {
        if (Test-Path $prof) { Copy-Item $prof (Join-Path $root ('build\pgo\snap.prev-{0}.profdata' -f (Get-Item $prof).LastWriteTime.ToString('yyyyMMdd'))) -Force }
        & (Join-Path $root 'tools\llvm\bin\llvm-profdata.exe') merge -o $prof (Get-ChildItem $raw -Filter *.profraw).FullName
    }
}

$pgoSdk = @(); $pgoExe = @()
if ($NoPgo) { $pgoSdk = @('--pgo', 'off'); $pgoExe = @('--pgo-use', 'off', '--rebuild') }
elseif (Test-Path $prof) {
    $age = (Get-Date) - (Get-Item $prof).LastWriteTime
    $newest = Get-ChildItem (Join-Path $root 'generated\v262144\main') -Filter *.c | Sort-Object LastWriteTime | Select-Object -Last 1
    if ($newest -and $newest.LastWriteTime -gt (Get-Item $prof).LastWriteTime) {
        Write-Warning ("the PGO profile is older than the generated main code ({0:N1} days). Functions that changed lose PGO until it is retrained: add -Retrain" -f $age.TotalDays)
    }
    Write-Host "PGO: $prof ($([int]$age.TotalHours) h old)"
} else { Write-Host 'PGO: no build\pgo\snap.profdata, building without it (add -Retrain to make one)' }

Step 'sdk lib' { Py (@('recomp\snap_build.py', '--gen', 'generated\v262144\sdk', '--out', 'build\eh\sdk', '--lib', 'snap_sdk', '--jobs', $Jobs) + $pgoSdk) }
Step 'main lib' { Py (@('recomp\snap_build.py', '--gen', 'generated\v262144\main', '--out', 'build\eh\main', '--lib', 'snap_main', '--jobs', $Jobs) + $pgoSdk) }
Step 'exe (build\snap)' { Py (@('host\build_snap.py') + $pgoExe) }

if (-not $SkipTests) {
    Write-Host "[$(Get-Date -f mm:ss)] HLE tests"
    if (-not $DryRun) {
        # the test script's stderr (a missing vswhere on PATH, warnings) must not stop this script: only its summary line counts
        $vsi = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer'
        if ((Test-Path $vsi) -and ($env:PATH -notlike "*$vsi*")) { $env:PATH += ";$vsi" }
        $ea = $ErrorActionPreference; $ErrorActionPreference = 'Continue'
        $out = & powershell -NoProfile -File (Join-Path $PSScriptRoot '..\tests\hle\run_tests.ps1') 2>&1 | Out-String
        $ErrorActionPreference = $ea
        $sum = ($out -split "`r?`n" | Where-Object { $_ -match '\d+ checks, \d+ failed' } | Select-Object -Last 1)
        Write-Host $sum
        if ($sum -notmatch ' 0 failed') { throw "HLE tests failed:`n$out" }
    }
}
Write-Host ("[{0:mm\:ss}] done: build\snap\snap_boot.exe ({1}). Play with: powershell -File host\play.ps1" -f ((Get-Date) - $t0), $(if ($NoPgo) { 'no PGO' } elseif (Test-Path $prof) { 'PGO' } else { 'no PGO profile' }))
