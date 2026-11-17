# Fast local checks before a commit or a release (no remote CI, no git hooks). Exit 0 = all passed; non-zero = at least one failed, and the
# last line says which one and why.
#
#   powershell -NoProfile -File host\ci_check.ps1                 # commit mode: changed files, about 2 to 4 minutes
#   powershell -NoProfile -File host\ci_check.ps1 -Mode release   # every tracked file, plus the builder unit tests
#   powershell -NoProfile -File host\ci_check.ps1 -Only license,docs
#   powershell -NoProfile -File host\ci_check.ps1 -Skip hle,lifter
#
# Checks (name: what it does):
#   license   LICENSE and README agree (title, badge, License section, copyright holder), THIRD_PARTY_NOTICES.md exists
#   identity  leak scan (builder\leak_scan.py rules) of the changed files (release mode: all tracked files) and of the unpushed commits' metadata: game-derived
#             extensions and names, keys, the owner's names and addresses (build\release\leak_terms.txt), home paths, e-mail addresses
#   docs      banned-phrase scan (unslop) of the lines added to changed .md files
#   locks     host\tests\locks.Tests.ps1
#   quirks    tests\quirks\quirks_test.cpp (driver quirk matcher)
#   crc       tests\hle\run_crc_test.ps1
#   hle       tests\hle\run_tests.ps1 (HLE, save, option and host unit tests)
#   lifter    differential smoke of the AArch64-to-C lifter: tests\snap_func.py on a slice of 120 functions, Unicorn vs the lifted C
#   builder   (release mode) python -m unittest discover -s builder\tests
# The compiling checks (quirks, crc, hle, lifter) hold build\heavy.lock (one heavy build at a time on the PC); -NoLock skips it.
# Logs: build\w\ci_check\<name>.log. Everything runs at BelowNormal.
param([ValidateSet('commit', 'release')][string]$Mode = 'commit',
      [string[]]$Only = @(), [string[]]$Skip = @(), [switch]$NoLock, [int]$LockWaitSec = 1800)
$ErrorActionPreference = 'Stop'
# -File passes "a,b" as one string: split it
$Only = @($Only | ForEach-Object { $_ -split ',' } | Where-Object { $_ })
$Skip = @($Skip | ForEach-Object { $_ -split ',' } | Where-Object { $_ })
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
Set-Location $root
(Get-Process -Id $PID).PriorityClass = 'BelowNormal'
$py = Join-Path $root 'tools\venv-recomp\Scripts\python.exe'
$logDir = Join-Path $root 'build\w\ci_check'
New-Item -ItemType Directory -Force $logDir | Out-Null
. (Join-Path $PSScriptRoot 'locks.ps1')

# Runs a program with output to a log file; returns @{ rc; log; seconds }. A program that outlives $timeoutSec is killed by PID with its children.
function Invoke-Logged([string]$name, [string]$exe, [string[]]$argv, [int]$timeoutSec = 900, [hashtable]$env = @{}) {
    $log = Join-Path $logDir "$name.log"
    $err = Join-Path $logDir "$name.err"
    $saved = @{}
    foreach ($k in $env.Keys) { $saved[$k] = [Environment]::GetEnvironmentVariable($k); [Environment]::SetEnvironmentVariable($k, $env[$k]) }
    $sw = [Diagnostics.Stopwatch]::StartNew()
    try {
        $a = @{ FilePath = $exe; NoNewWindow = $true; PassThru = $true; RedirectStandardOutput = $log; RedirectStandardError = $err; WorkingDirectory = $root }
        if ($argv -and $argv.Count) { $a.ArgumentList = @($argv | ForEach-Object { if ($_ -match '\s') { '"' + $_ + '"' } else { $_ } }) }   # Start-Process does not quote
        $p = Start-Process @a
        $null = $p.Handle   # keep the handle so ExitCode stays readable
        if (-not $p.WaitForExit($timeoutSec * 1000)) {
            & taskkill /PID $p.Id /T /F | Out-Null
            return @{ rc = 124; log = $log; seconds = $sw.Elapsed.TotalSeconds; timeout = $true }
        }
        $p.WaitForExit()
        if (Test-Path $err) { Get-Content $err -ErrorAction SilentlyContinue | Add-Content $log }
        return @{ rc = $p.ExitCode; log = $log; seconds = $sw.Elapsed.TotalSeconds; timeout = $false }
    } finally {
        foreach ($k in $saved.Keys) { [Environment]::SetEnvironmentVariable($k, $saved[$k]) }
    }
}

function Last-Lines([string]$log, [string]$pattern = '', [int]$n = 1) {
    $lines = @(Get-Content $log -ErrorAction SilentlyContinue | Where-Object { $_ -and $_.Trim() })
    if ($pattern) { $m = @($lines | Where-Object { $_ -match $pattern }); if ($m.Count) { return ($m | Select-Object -Last $n) -join ' | ' } }
    return ($lines | Select-Object -Last $n) -join ' | '
}

function Shorten([string]$s, [int]$max = 230) { if ($s.Length -gt $max) { $s.Substring(0, $max) + '...' } else { $s } }

# ---- the checks: each returns @{ ok; reason }
function Check-Python-Helper([string]$sub, [string[]]$extra = @()) {
    $r = Invoke-Logged $sub $py (@('-I', (Join-Path $PSScriptRoot 'ci_checks.py'), $sub) + $extra) 300
    $res = Last-Lines $r.log '^RESULT ' 1
    if ($res -match '^RESULT (PASS|FAIL) [a-z]+: (.*)$') { return @{ ok = ($Matches[1] -eq 'PASS' -and $r.rc -eq 0); reason = $Matches[2] } }
    return @{ ok = $false; reason = "no result line (exit $($r.rc)): " + (Shorten (Last-Lines $r.log '' 2)) }
}

function Check-License { Check-Python-Helper 'license' }
function Check-Identity { if ($Mode -eq 'release') { Check-Python-Helper 'identity' @('--full') } else { Check-Python-Helper 'identity' } }
function Check-Docs { Check-Python-Helper 'docs' }

function Check-Locks {
    $r = Invoke-Logged 'locks' 'powershell.exe' @('-NoProfile', '-File', (Join-Path $root 'host\tests\locks.Tests.ps1')) 600
    $line = Last-Lines $r.log '(?i)pass|fail|ok' 1
    return @{ ok = ($r.rc -eq 0); reason = $(if ($r.rc -eq 0) { "locks tests passed ($line)" } else { "locks tests failed (exit $($r.rc)): " + (Shorten (Last-Lines $r.log '(?i)fail' 2)) }) }
}

function Check-Quirks {
    $out = Join-Path $logDir 'quirks_test.exe'
    $vc = 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat'
    $bat = Join-Path $logDir 'quirks_run.cmd'
    Set-Content -Path $bat -Encoding ASCII -Value @(
        '@echo off', "call `"$vc`" >nul", "set PATH=$root\tools\llvm\bin;%PATH%", "cd /d $root",
        "clang++ -std=c++20 -I runtime\nvn tests\quirks\quirks_test.cpp runtime\nvn\nvn_quirks.cpp -o `"$out`"", 'if errorlevel 1 exit /b 1', "`"$out`"", 'exit /b %errorlevel%')
    $r = Invoke-Logged 'quirks' 'cmd.exe' @('/c', $bat) 300
    return @{ ok = ($r.rc -eq 0); reason = $(if ($r.rc -eq 0) { 'quirks matcher test passed' } else { "quirks test failed (exit $($r.rc)): " + (Shorten (Last-Lines $r.log '(?i)FAIL|error' 2)) }) }
}

function Check-Crc {
    $r = Invoke-Logged 'crc' 'powershell.exe' @('-NoProfile', '-File', (Join-Path $root 'tests\hle\run_crc_test.ps1')) 600
    return @{ ok = ($r.rc -eq 0); reason = $(if ($r.rc -eq 0) { 'crc32 test passed' } else { "crc32 test failed (exit $($r.rc)): " + (Shorten (Last-Lines $r.log '(?i)FAIL|error' 2)) }) }
}

function Check-Hle {
    $r = Invoke-Logged 'hle' 'powershell.exe' @('-NoProfile', '-File', (Join-Path $root 'tests\hle\run_tests.ps1')) 900
    if ($r.timeout) { return @{ ok = $false; reason = 'HLE tests timed out after 900 s' } }
    $sum = Last-Lines $r.log '(?i)\d+\s+checks?|passed|failed' 1
    if ($r.rc -eq 0 -and $sum -notmatch '(?i)[1-9]\d*\s+failed') { return @{ ok = $true; reason = "HLE tests passed ($(Shorten $sum 120))" } }
    return @{ ok = $false; reason = "HLE tests failed (exit $($r.rc)): " + (Shorten (Last-Lines $r.log '(?i)FAIL|error' 2)) }
}

function Check-Lifter {
    $tag = 'ci'
    $res = Join-Path $root "analysis\recomp\diff_$tag\diff_results.json"
    Remove-Item $res -ErrorAction SilentlyContinue
    $r = Invoke-Logged 'lifter' $py @('-I', 'tests\snap_func.py', '--sweep', '7:120', '--cases', '4', '--retry-untested', '8', '--steps', '100000',
                                       '--cflags', '-march=x86-64-v3 -O2', '--jobs', '2') 900 @{ SNAP_DIFF_TAG = $tag }
    foreach ($d in @("generated\subset_$tag", "build\subset_$tag")) { Remove-Item -Recurse -Force (Join-Path $root $d) -ErrorAction SilentlyContinue }
    if (-not (Test-Path $res)) { return @{ ok = $false; reason = "lifter diff produced no result (exit $($r.rc)): " + (Shorten (Last-Lines $r.log '' 2)) } }
    $s = (Get-Content $res -Raw | ConvertFrom-Json).summary
    $st = $s.status
    $fail = [int]($st.fail); $crash = [int]($st.crash); $pass = [int]($st.pass); $unt = [int]($st.untested)
    if ($fail + $crash -gt 0) { return @{ ok = $false; reason = "lifter differs from the AArch64 reference in $fail function(s), $crash crash(es) (repros: analysis\recomp\diff_$tag\repro)" } }
    if ($pass -lt 40) { return @{ ok = $false; reason = "lifter smoke tested only $pass function(s) ($unt untested): the harness is not working" } }
    return @{ ok = $true; reason = "lifter diff smoke: $pass functions equal to the reference, $unt untested, $($s.cases_compared) states" }
}

function Check-Builder {
    $r = Invoke-Logged 'builder' $py @('-I', '-m', 'unittest', 'discover', '-s', 'builder\tests') 900
    $line = Last-Lines $r.log '^(OK|FAILED|Ran )' 2
    return @{ ok = ($r.rc -eq 0); reason = $(if ($r.rc -eq 0) { "builder unit tests passed ($line)" } else { "builder unit tests failed: " + (Shorten $line) }) }
}

$checks = [ordered]@{
    license  = @{ fn = 'Check-License';  heavy = $false }
    identity = @{ fn = 'Check-Identity'; heavy = $false }
    docs     = @{ fn = 'Check-Docs';     heavy = $false }
    locks    = @{ fn = 'Check-Locks';    heavy = $false }
    quirks   = @{ fn = 'Check-Quirks';   heavy = $true }
    crc      = @{ fn = 'Check-Crc';      heavy = $true }
    hle      = @{ fn = 'Check-Hle';      heavy = $true }
    lifter   = @{ fn = 'Check-Lifter';   heavy = $true }
}
if ($Mode -eq 'release') { $checks['builder'] = @{ fn = 'Check-Builder'; heavy = $false } }

$want = @($checks.Keys | Where-Object { ($Only.Count -eq 0 -or $Only -contains $_) -and ($Skip -notcontains $_) })
if ($want.Count -eq 0) { Write-Host 'CI CHECK FAILED: no check selected (names: ' ($checks.Keys -join ', ') ')'; exit 2 }

$total = [Diagnostics.Stopwatch]::StartNew()
$failed = @()
$haveLock = $false
$lockPath = Join-Path $root 'build\heavy.lock'
try {
    foreach ($name in $want) {
        if ($checks[$name].heavy -and -not $haveLock -and -not $NoLock) {
            Write-Host "[ci_check] taking build\heavy.lock for the compiling checks..."
            if (-not (Take-BuildLock 'ci_check (compiling checks)' $LockWaitSec)) {
                $failed += @{ name = $name; reason = "could not take build\heavy.lock within $LockWaitSec s (use -NoLock to run without it)" }
                Write-Host ("[ci_check] FAIL  {0,-9} {1}" -f $name, $failed[-1].reason)
                continue
            }
            $haveLock = $true
        }
        $sw = [Diagnostics.Stopwatch]::StartNew()
        try { $res = & $checks[$name].fn } catch { $res = @{ ok = $false; reason = "check crashed: $($_.Exception.Message)" } }
        $tag = if ($res.ok) { 'PASS' } else { 'FAIL' }
        Write-Host ("[ci_check] {0}  {1,-9} {2}  ({3:N0} s)" -f $tag, $name, $res.reason, $sw.Elapsed.TotalSeconds)
        if (-not $res.ok) { $failed += @{ name = $name; reason = $res.reason } }
    }
} finally {
    if ($haveLock) { Release-Lock $lockPath }
}

if ($failed.Count -eq 0) {
    Write-Host ("CI CHECK PASSED: {0} check(s) in {1:N0} s ({2} mode)" -f $want.Count, $total.Elapsed.TotalSeconds, $Mode)
    exit 0
}
$one = Shorten ("{0}: {1}" -f $failed[0].name, $failed[0].reason) 260
Write-Host ("CI CHECK FAILED: {0}{1}" -f $one, $(if ($failed.Count -gt 1) { " (+$($failed.Count - 1) more failed: $((($failed | Select-Object -Skip 1) | ForEach-Object { $_.name }) -join ', '))" } else { '' }))
exit 1
