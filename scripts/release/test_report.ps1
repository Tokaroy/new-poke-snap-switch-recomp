# Release test report: one command, one local folder of evidence for a build (docs/release-test-report.md).
#
#   powershell -NoProfile -File scripts\release\test_report.ps1 -BuildDir build\snap [-Out <folder>] [-Quick] [-NoAB] [-NoLauncher]
#
#   -BuildDir   the build to test (host\build_snap.py --out <dir>; holds snap_boot.exe)
#   -Out        report folder (default build\release_report\<stamp>; inside the checkout it must be git-ignored, build\ is)
#   -Quick      unit tests plus the florio_eval ride only
#   -NoAB / -NoLauncher   skip those steps in a full run
# Runs one step at a time, children at BelowNormal, each heavy step capped at 10 minutes (a step over its cap is killed by PID).
# Writes <Out>\report.md (pass/fail summary first) and report.json. The folder holds game frames: it is LOCAL ONLY, never commit or upload it.
# Exit code: 0 every step passed or was a known failure, 1 a step failed, 2 bad arguments.
# Steps: unit tests (HLE with the padfx allowance, CRC, CLI flags, mods, and in a full run elf_check and picture rows), rides
# (florio_eval; full adds bench_florio) with fps and load times from the game's own logs, A/B captures (default vs Enhanced lighting),
# launcher screenshots. Child scripts may write harmless stderr (vswhere not found): results are judged by exit code and output.
[CmdletBinding(PositionalBinding = $false)]
param([Parameter(Mandatory)][string]$BuildDir, [string]$Out, [switch]$Quick, [switch]$NoAB, [switch]$NoLauncher, [int]$StepTimeout = 600)
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
Set-Location $root
$stamp = Get-Date -Format 'yyyyMMdd_HHmmss'

# ---- arguments
if (-not [IO.Path]::IsPathRooted($BuildDir)) { $BuildDir = Join-Path $root $BuildDir }
if (-not (Test-Path (Join-Path $BuildDir 'snap_boot.exe'))) { Write-Host "no snap_boot.exe in $BuildDir"; exit 2 }
$BuildDir = (Resolve-Path $BuildDir).Path
if (-not $Out) { $Out = Join-Path $root "build\release_report\$stamp" }
elseif (-not [IO.Path]::IsPathRooted($Out)) { $Out = Join-Path $root $Out }
New-Item -ItemType Directory -Force $Out | Out-Null
$Out = (Resolve-Path $Out).Path
if ($Out.StartsWith($root, [StringComparison]::OrdinalIgnoreCase)) {
    & git -C $root check-ignore -q $Out 2>$null
    if ($LASTEXITCODE -ne 0) { Write-Host "refusing: $Out is inside the checkout but not git-ignored; the report holds game frames. Use a folder under build\."; exit 2 }
}
$LogDir = Join-Path $Out 'logs'; $ImgDir = Join-Path $Out 'images'
New-Item -ItemType Directory -Force $LogDir, $ImgDir | Out-Null
$py = Join-Path $root 'tools\venv-recomp\Scripts\python.exe'; if (-not (Test-Path $py)) { $py = 'python' }
$lib = Join-Path $PSScriptRoot 'test_report_lib.py'
$AllowedHle = @('padfx')   # known HLE failure, owner-approved allowance (2026-11-23)

# main checkout (shared locks, play.lock, warm shader cache): the git common dir's parent; this checkout when it is the main one
$common = (& git -C $root rev-parse --git-common-dir 2>$null)
$mainRoot = $root
if ($common) { $c = (Resolve-Path $(if ([IO.Path]::IsPathRooted($common)) { $common } else { Join-Path $root $common })).Path; if ((Split-Path $c -Leaf) -eq '.git') { $mainRoot = Split-Path $c -Parent } }
$script:LockRoot = $mainRoot
$haveLocks = Test-Path (Join-Path $mainRoot 'host\locks.ps1')
if ($haveLocks) { . (Join-Path $mainRoot 'host\locks.ps1') }

# ---- state
$steps = New-Object System.Collections.ArrayList
$notes = New-Object System.Collections.ArrayList
$rides = New-Object System.Collections.ArrayList
$abs = New-Object System.Collections.ArrayList
$launcherImgs = @()
$allowedUsed = New-Object System.Collections.ArrayList
$tmpRoutes = New-Object System.Collections.ArrayList

function Add-Step([string]$Name, [string]$Status, [double]$Seconds, [string]$Detail) {
    [void]$steps.Add([ordered]@{ name = $Name; status = $Status; seconds = [math]::Round($Seconds, 0); detail = $Detail })
    Write-Host ("[{0}] {1} ({2:N0}s) {3}" -f $Status, $Name, $Seconds, $Detail)
}
function Q([string]$s) { if ($s -match '\s') { '"' + $s + '"' } else { $s } }

# One PowerShell child at BelowNormal, output to logs; killed by PID after $TimeoutSec. Returns exit code and output text.
function Invoke-Child([string]$Name, [string[]]$PsArgs, [int]$TimeoutSec) {
    $o = Join-Path $LogDir "$Name.out.txt"; $e = Join-Path $LogDir "$Name.err.txt"
    $argl = @('-NoProfile', '-ExecutionPolicy', 'Bypass') + $PsArgs
    $p = Start-Process -FilePath 'powershell.exe' -ArgumentList $argl -NoNewWindow -PassThru -RedirectStandardOutput $o -RedirectStandardError $e
    $null = $p.Handle
    try { $p.PriorityClass = 'BelowNormal' } catch {}
    $timedOut = $false
    if (-not $p.WaitForExit($TimeoutSec * 1000)) { taskkill /PID $p.Id /T /F 2>$null | Out-Null; $timedOut = $true }
    else { $p.WaitForExit() }
    $code = if ($timedOut) { 124 } elseif ($null -eq $p.ExitCode) { 0 } else { [int]$p.ExitCode }
    $txt = if (Test-Path $o) { Get-Content $o -Raw -ErrorAction SilentlyContinue } else { '' }
    return @{ Exit = $code; Text = "$txt"; TimedOut = $timedOut; OutFile = $o }
}
function Use-BuildLock([string]$What, [scriptblock]$Body) {
    $took = $false
    if ($haveLocks) {
        $took = Take-BuildLock $What 7200
        if (-not $took) { throw "could not take build\heavy.lock for $What" }
    }
    try { & $Body } finally { if ($took) { Release-Lock (Join-Path $script:LockRoot 'build\heavy.lock') } }
}

# ---- owner playing?
$playing = $false
foreach ($pl in @((Join-Path $mainRoot 'build\play.lock'), (Join-Path $root 'build\play.lock'))) {
    if (Test-Path $pl) {
        $t = (Get-Content $pl -ErrorAction SilentlyContinue) -join ' '
        if ($t -match 'pid=(\d+)' -and (Get-Process -Id ([int]$Matches[1]) -ErrorAction SilentlyContinue)) { $playing = $true }
    }
}
if ($playing) { [void]$notes.Add('The owner is playing (build\play.lock is live): rides, A/B captures and launcher screenshots were not run.') }

# ---- build and machine info
function Get-Info {
    $i = [ordered]@{}
    $sha = (& git -C $root rev-parse HEAD 2>$null) -join ''
    $i['Commit'] = if ($sha) { $sha.Substring(0, 12) } else { 'unknown' }
    $dirty = @(& git -C $root status --porcelain 2>$null | Where-Object { $_ -notmatch '^\?\? ' }).Count
    $i['Branch'] = ((& git -C $root rev-parse --abbrev-ref HEAD 2>$null) -join '')
    $i['Checkout state'] = if ($dirty -gt 0) { "$dirty tracked files modified (the build may not match the commit)" } else { 'clean' }
    $i['Describe'] = ((& git -C $root describe --tags --always 2>$null) -join '')
    $exe = Get-Item (Join-Path $BuildDir 'snap_boot.exe')
    $i['Build'] = "snap_boot.exe $([math]::Round($exe.Length / 1MB, 1)) MB, built $($exe.LastWriteTime.ToString('yyyy-MM-dd HH:mm')), sha256 $((Get-FileHash $exe.FullName -Algorithm SHA256).Hash.Substring(0, 16).ToLower())"
    $i['Report run'] = (Get-Date).ToString('yyyy-MM-dd HH:mm') + $(if ($Quick) { ' (quick)' } else { ' (full)' })
    $os = Get-CimInstance Win32_OperatingSystem
    $i['OS'] = "$($os.Caption) build $($os.BuildNumber)"
    $i['CPU'] = ((Get-CimInstance Win32_Processor | Select-Object -First 1).Name -replace '\s+', ' ').Trim()
    $i['RAM'] = "$([math]::Round((Get-CimInstance Win32_ComputerSystem).TotalPhysicalMemory / 1GB)) GB"
    $gpus = @(Get-CimInstance Win32_VideoController | Where-Object { $_.Name -notmatch 'Basic|Remote|Virtual|Parsec' } | ForEach-Object { "$($_.Name), driver $($_.DriverVersion)" })
    $i['GPU'] = if ($gpus.Count) { $gpus -join '; ' } else { 'unknown' }
    return $i
}
$info = Get-Info

# ---- 1. unit tests (compile and run; each under the build lock, one at a time)
function Invoke-UnitTest([string]$Name, [string]$Script, [string]$Detail, [switch]$Hle) {
    $t0 = Get-Date
    $res = $null
    try {
        Use-BuildLock "release report: $Name" {
            $script:res = Invoke-Child "unit_$Name" @('-File', (Q (Join-Path $root $Script))) $StepTimeout
        }
    } catch { Add-Step $Name 'FAIL' ((Get-Date) - $t0).TotalSeconds "$_"; return }
    $res = $script:res
    $sec = ((Get-Date) - $t0).TotalSeconds
    if ($res.TimedOut) { Add-Step $Name 'FAIL' $sec "killed after $StepTimeout s"; return }
    if ($Hle) {
        $j = $null
        try { $j = (& $py -I $lib hle $res.OutFile ($AllowedHle -join ',')) | ConvertFrom-Json } catch {}
        if (-not $j -or $j.tests -lt 1) { Add-Step $Name 'FAIL' $sec "no test results (exit $($res.Exit)); see logs\unit_$Name.out.txt"; return }
        foreach ($a in $j.allowed_failures) { [void]$allowedUsed.Add("$Name/$a") }
        $d = "$($j.tests) tests, $($j.checks) checks"
        if ($j.allowed_failures.Count) { $d += "; known failure allowed: $($j.allowed_failures -join ', ')" }
        if ($j.unexpected_failures.Count) { Add-Step $Name 'FAIL' $sec "$d; unexpected failures: $($j.unexpected_failures -join ', ')"; return }
        if ($res.Exit -ne 0 -and -not $j.allowed_failures.Count) { Add-Step $Name 'FAIL' $sec "$d; exit code $($res.Exit)"; return }
        Add-Step $Name 'PASS' $sec $d
        return
    }
    $last = ($res.Text -split "`r?`n" | Where-Object { $_.Trim() } | Select-Object -Last 1)
    if ($res.Exit -eq 0) { Add-Step $Name 'PASS' $sec "$Detail$(if ($last) { ': ' + $last.Trim() })" }
    else { Add-Step $Name 'FAIL' $sec "exit code $($res.Exit)$(if ($last) { ': ' + $last.Trim() })" }
}
Invoke-UnitTest 'HLE tests' 'tests\hle\run_tests.ps1' '' -Hle
Invoke-UnitTest 'CRC32 test' 'tests\hle\run_crc_test.ps1' ''
Invoke-UnitTest 'CLI flags test' 'tests\hle\run_cli_flags_test.ps1' ''
Invoke-UnitTest 'Mods tests' 'tests\mods\run_tests.ps1' ''
if (-not $Quick) {
    Invoke-UnitTest 'ELF check test' 'tests\hle\run_elf_check_test.ps1' ''
    Invoke-UnitTest 'Picture rows test' 'tests\hle\run_picture_rows_test.ps1' ''
}

# ---- 2. shader cache seed: a fresh checkout has an empty nvn_cache; rides then draw black frames and time out at map_select
function Test-HasFiles([string]$Dir) { (Test-Path $Dir) -and ($null -ne (Get-ChildItem $Dir -Recurse -File -ErrorAction SilentlyContinue | Select-Object -First 1)) }
function Initialize-ShaderCache {
    $seedSrc = Join-Path $mainRoot 'build\runs\slot0\nvn_cache'
    $targets = @((Join-Path $BuildDir 'nvn_cache'), (Join-Path $root 'build\runs\slot0\nvn_cache'))
    foreach ($tgt in $targets) {
        if ((Test-Path $tgt) -and ((Resolve-Path $tgt).Path -eq (Resolve-Path $seedSrc -ErrorAction SilentlyContinue).Path)) { continue }
        if (Test-HasFiles $tgt) { continue }
        if (-not (Test-HasFiles $seedSrc)) { [void]$notes.Add("Translated-shader cache is empty and no warm copy was found at the main checkout's slot 0; rides may show black frames and time out at map_select."); return }
        New-Item -ItemType Directory -Force $tgt | Out-Null
        & robocopy $seedSrc $tgt /E /XO /MT:16 /NFL /NDL /NJH /NJS /NP | Out-Null
        [void]$notes.Add("Seeded an empty translated-shader cache ($((Split-Path (Split-Path $tgt -Parent) -Leaf))\nvn_cache) from the main checkout's slot 0 cache (robocopy /E /XO). Without it a fresh checkout shows black frames and times out at map_select.")
    }
}

# ---- 3. rides
$waitLabels = @{
    'ui=title' = 'Boot to title'; 'ui=map_select' = 'Boot, title and lab hub to the Lental Map'; 'ui=map_select:StageSelect' = 'Title to the hub (StageSelect)'
    'ride' = 'Course load: map to ride start'; 'film<=0' = 'Ride until the film is used up'; 'rail>=110' = 'Ride to rail 110 (benchmark window)'
    'ui=photo_submit:FolderSelect' = 'Course end to photo select (folders)'; 'ui=photo_submit:PhotoSubmit' = 'Open a photo folder'
    'ui=evaluation' = 'Photo submit to the professor''s evaluation'; 'ui=evaluation:Score' = 'Evaluation to the score screen'; 'ui=evaluation:End' = 'Score screen to the end of the evaluation'
}
function Invoke-Ride([string]$Route, [string]$WinStart, [string]$WinEnd, [switch]$ScoreShots) {
    $t0 = Get-Date
    $r = [ordered]@{ route = $Route; status = 'FAIL'; exit = $null; route_end = $false; images = @(); waits = @() }
    $routeName = $Route; $perfDir = Join-Path $Out 'perf'; New-Item -ItemType Directory -Force $perfDir | Out-Null
    $prefix = Join-Path $perfDir $Route
    if ($ScoreShots) {
        # the route plus a short frame burst when the score screen and the end screen open (few frames, after the measured window)
        $routeName = "_trep_$stamp"
        $lines = New-Object System.Collections.ArrayList
        foreach ($l in (Get-Content (Join-Path $root "host\routes\$Route.txt"))) {
            [void]$lines.Add($l)
            if ($l -match '^wait ui=evaluation:Score\b') { foreach ($f in 150, 230, 310) { [void]$lines.Add("@f$f 1 shot=1") } }
        }
        $tmp = Join-Path $root "host\routes\$routeName.txt"
        Set-Content -Path $tmp -Value $lines -Encoding ascii
        [void]$tmpRoutes.Add($tmp)
    }
    $env:SNAP_PERF = $prefix
    try {
        $shotMax = if ($ScoreShots) { '12' } else { '0' }
        $a = @('-File', (Q (Join-Path $root 'host\run_boot.ps1')), '-Route', $routeName, '-BuildDir', (Q $BuildDir), '-Timeout', '590', '-ShotAt', '1000000', '-ShotEvery', '1000000', '-ShotMax', $shotMax)
        $res = Invoke-Child "ride_$Route" $a ($StepTimeout + 90)
    } finally { Remove-Item Env:SNAP_PERF -ErrorAction SilentlyContinue }
    $r.exit = $res.Exit
    $runId = $null
    if ($res.Text -match '(?m)^run (\S+) on ') { $runId = $Matches[1] }
    $sec = ((Get-Date) - $t0).TotalSeconds
    if (-not $runId) { $r.detail = 'no run started'; [void]$rides.Add($r); Add-Step "Ride $Route" 'FAIL' $sec "no run started (exit $($res.Exit)); see logs\ride_$Route.out.txt"; return }
    $err = Join-Path $root "analysis\boot\runs\$runId\boot_stderr.log"
    if (Test-Path $err) {
        $w = (& $py -I $lib waits $err) | ConvertFrom-Json
        $r.route_end = [bool]$w.route_end; $r.timeout = $w.timeout
        $prev = $null
        $r.waits = @($w.waits | ForEach-Object { [ordered]@{ label = $(if ($waitLabels.ContainsKey($_.cond)) { $waitLabels[$_.cond] } else { "wait $($_.cond)" }); after_s = $_.after_s; present = $_.present } })
        $r.window = @($WinStart, $WinEnd)
        $fr = "${prefix}_frames.csv"
        if (Test-Path $fr) {
            $p = (& $py -I $lib perf $fr $err $WinStart $WinEnd) | ConvertFrom-Json
            if ($p.error) { $r.perf_error = $p.error } else { $r.perf = $p }
        } else { $r.perf_error = 'the build wrote no SNAP_PERF frames log' }
        # filtered log copy: script waits, timeouts, crashes
        Get-Content $err -ErrorAction SilentlyContinue | Where-Object { $_ -match 'input script:|FATAL|crash|exception|Unhandled|TIMEOUT|vkCreate.*fail|DEVICE_LOST' } | Select-Object -First 400 |
            Set-Content (Join-Path $LogDir "ride_${Route}_boot_filtered.txt") -Encoding utf8
    } else { $r.perf_error = "no boot_stderr.log for run $runId" }
    if ($ScoreShots) {
        $shots = @(Get-ChildItem (Join-Path $root 'analysis\boot') -Filter "route_${routeName}_${runId}_??.png" -ErrorAction SilentlyContinue | Sort-Object Name)
        $n = 0
        $ocr = @()
        foreach ($s in $shots) {
            $dst = Join-Path $ImgDir ("ride_${Route}_score_{0:D2}.png" -f $n); Copy-Item $s.FullName $dst -Force
            $r.images += [ordered]@{ alt = "$Route frame $n"; path = "images/$(Split-Path $dst -Leaf)" }
            $n++
            if ($n -le 8) {
                $o = Invoke-Child "ocr_$n" @('-File', (Q (Join-Path $PSScriptRoot 'ocr_image.ps1')), '-Image', (Q $dst)) 60
                if ($o.Exit -eq 0 -and $o.Text.Trim()) { $ocr += ,@($dst, $o.Text.Trim()) }
            }
        }
        $r.score = if ($shots.Count -eq 0) { 'no frames were captured' } else { "$($shots.Count) frames captured" }
        # score screen text: numbers the OCR read on the captured frames (OCR is a read of the picture, not a game value)
        $nums = @()
        foreach ($o in $ocr) { foreach ($m in [regex]::Matches($o[1], '\b\d{1,3}(,\d{3})+\b|\b\d{3,6}\b')) { $nums += $m.Value } }
        $nums = @($nums | Select-Object -Unique)
        if ($nums.Count) { $r.score += "; numbers the OCR read on them: $($nums -join ', ') (read from the pictures, check against them)" } else { $r.score += '; the OCR read no numbers, check the pictures' }
        if ($w) { $sc = @($w.waits | Where-Object { $_.cond -eq 'ui=evaluation:Score' }).Count; $r.score = "score screen reached: $(if ($sc) { 'yes' } else { 'NO' }); " + $r.score }
        Get-ChildItem (Join-Path $root 'analysis\boot') -Filter "route_${routeName}_*" -ErrorAction SilentlyContinue | Remove-Item -Force -ErrorAction SilentlyContinue
    }
    $ok = ($res.Exit -eq 0) -and $r.route_end -and -not $r.timeout
    $needScore = $ScoreShots -and -not (@($r.waits | Where-Object { $_.label -like 'Evaluation to the score*' }).Count)
    $r.status = if ($ok -and -not $needScore) { 'PASS' } else { 'FAIL' }
    $detail = if ($r.perf) { "avg $($r.perf.avg_fps) fps, 1% low $($r.perf.low1_fps)" } else { "no fps ($($r.perf_error))" }
    if ($r.status -eq 'FAIL') { $detail = "exit $($r.exit)$(if ($r.timeout) { '; ' + $r.timeout })$(if (-not $r.route_end) { '; route end not reached' }); $detail" }
    [void]$rides.Add($r)
    Add-Step "Ride $Route" $r.status $sec $detail
}

$stage = 'rides'
if (-not $playing) {
    Initialize-ShaderCache
    Invoke-Ride 'florio_eval' 'ride' 'film<=0' -ScoreShots
    if (-not $Quick) { Invoke-Ride 'bench_florio' 'ride' 'rail>=110' }
} else { foreach ($n in @('Ride florio_eval') + $(if (-not $Quick) { @('Ride bench_florio') } else { @() })) { Add-Step $n 'SKIP' 0 'owner is playing' } }

# ---- 4. A/B captures
if (-not $Quick) {
    if ($NoAB -or $playing) { Add-Step 'A/B captures' 'SKIP' 0 $(if ($playing) { 'owner is playing' } else { '-NoAB' }) }
    else {
        $points = @(@('florio_mid', 'core_florio_a', 'ride40'), @('beach_sea', 'core_beach_0', 'beach_sea'))
        $abOut = Join-Path $Out 'ab'; New-Item -ItemType Directory -Force $abOut | Out-Null
        foreach ($pt in $points) {
            $t0 = Get-Date
            $a = @('-File', (Q (Join-Path $root 'host\ab_capture.ps1')), '-Route', $pt[1], '-Moment', $pt[2], '-Effect', 'lighting', '-Values', 'off,on', '-Set', 'SNAP_LIGHTING=enhanced',
                   '-BuildDir', (Q $BuildDir), '-OutDir', (Q $abOut), '-Name', $pt[0])
            $res = Invoke-Child "ab_$($pt[0])" $a ($StepTimeout + 90)
            $sec = ((Get-Date) - $t0).TotalSeconds
            $png = Join-Path $abOut "ab_$($pt[0]).png"; $jf = Join-Path $abOut "ab_$($pt[0]).json"
            $st = switch ($res.Exit) { 0 { 'PASS' } 11 { 'PASS' } default { 'FAIL' } }
            $d = switch ($res.Exit) { 0 { 'frames captured, photo frame check passed' } 10 { 'photo frame check FAILED (the effect changed a photo frame)' } 11 { 'frames captured, photo check inconclusive' } 124 { "killed after $($StepTimeout + 90) s" } default { "run failed (exit $($res.Exit))" } }
            $rec = [ordered]@{ name = "$($pt[1]) at $($pt[2])"; status = $st; detail = $d; images = @() }
            if (Test-Path $png) { $rec.images += [ordered]@{ alt = "A/B $($pt[0])"; path = "ab/ab_$($pt[0]).png" } }
            [void]$abs.Add($rec)
            Add-Step "A/B $($pt[0])" $st $sec $d
        }
    }
}

# ---- 5. launcher screenshots
if (-not $Quick) {
    if ($NoLauncher -or $playing) { Add-Step 'Launcher screenshots' 'SKIP' 0 $(if ($playing) { 'owner is playing' } else { '-NoLauncher' }) }
    else {
        $t0 = Get-Date
        try {
            $exe = Join-Path $root 'build\launcher_ui\launcher_ui_test.exe'
            if (-not (Test-Path $exe)) {
                Use-BuildLock 'release report: launcher preview build' { $script:lb = Invoke-Child 'launcher_build' @('-Command', "& '$py' -I host\build_launcher_ui_test.py; exit `$LASTEXITCODE") $StepTimeout }
                if ($script:lb.Exit -ne 0) { throw "launcher preview build failed (exit $($script:lb.Exit))" }
            }
            $ld = Join-Path $Out 'launcher'
            $res = Invoke-Child 'launcher_capture' @('-File', (Q (Join-Path $root 'scripts\ui\capture_launcher.ps1')), '-Out', (Q $ld), '-Sizes', '1920x1080') $StepTimeout
            $pngs = @(Get-ChildItem $ld -Filter *.png -ErrorAction SilentlyContinue | Sort-Object Name)
            if ($res.Exit -eq 0 -and $pngs.Count -ge 10) {
                $sheet = Join-Path $ld 'contact_sheet_1920x1080.png'
                $null = Invoke-Child 'launcher_sheet' @('-Command', "& python -E -P '$(Join-Path $root 'scripts\ui\contact_sheet.py')' '$ld' 1920x1080 '$sheet'") 120
                if (Test-Path $sheet) { $launcherImgs += [ordered]@{ alt = 'Launcher pages, contact sheet'; path = 'launcher/contact_sheet_1920x1080.png' } }
                foreach ($n in 'home', 'settings_Picture', 'saves') { $f = Join-Path $ld "${n}_1920x1080.png"; if (Test-Path $f) { $launcherImgs += [ordered]@{ alt = "Launcher $n"; path = "launcher/${n}_1920x1080.png" } } }
                Add-Step 'Launcher screenshots' 'PASS' ((Get-Date) - $t0).TotalSeconds "$($pngs.Count) pictures at 1920x1080 from the standalone launcher preview built from this commit's source"
            } else { Add-Step 'Launcher screenshots' 'FAIL' ((Get-Date) - $t0).TotalSeconds "exit $($res.Exit), $($pngs.Count) pictures" }
        } catch { Add-Step 'Launcher screenshots' 'FAIL' ((Get-Date) - $t0).TotalSeconds "$_" }
    }
}

# ---- cleanup and report
foreach ($t in $tmpRoutes) { Remove-Item $t -Force -ErrorAction SilentlyContinue }
$notRun = @('Photo-scoring differential (tests/photoscore) needs a recorded capture and a readable rewrite kept outside the repository; run it by hand before a release that touches scoring.',
            'Courses other than Florio, save-state, HD texture and DLSS paths are not ridden; use the per-change testing budget in AGENTS.md for those.')
if ($Quick) { $notRun += 'Quick run: no second ride, A/B captures or launcher screenshots.' }
$data = [ordered]@{ quick = [bool]$Quick; steps = @($steps); info = $info; notes = @($notes); rides = @($rides); ab = @($abs); launcher = $launcherImgs; allowed = @($allowedUsed); not_run = $notRun
                    scrub = @($root, $mainRoot, $Out) }
$dj = Join-Path $LogDir 'report_data.json'
$data | ConvertTo-Json -Depth 8 | Set-Content $dj -Encoding utf8
$overall = (& $py -I $lib render $dj (Join-Path $Out 'report.md') (Join-Path $Out 'report.json')) | Select-Object -Last 1
# the logs may carry user paths too
& $py -I $lib scrub @(Get-ChildItem $LogDir -File | ForEach-Object FullName)
Write-Host ''
Write-Host "Release test report: $overall"
Write-Host "Report: $(Join-Path $Out 'report.md')  (local only: holds game frames, do not commit or upload)"
if ($overall -eq 'PASS') { exit 0 } else { exit 1 }
