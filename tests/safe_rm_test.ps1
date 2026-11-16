# Guard test for host\safe_rm.ps1: Remove-TreeSafe must unlink junctions and symlinks, never enter them.
# Also fails if a host script goes back to a recursive Remove-Item on a tree it did not just create.
#   powershell -NoProfile -File tests\safe_rm_test.ps1        (exit 0 = pass)
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
. (Join-Path $root 'host\safe_rm.ps1')
$fail = 0
function Check([string]$name, [bool]$ok) { if ($ok) { "PASS $name" } else { "FAIL $name"; $script:fail++ } }

$base = Join-Path ([IO.Path]::GetTempPath()) ('safe_rm_test_' + $PID)
$precious = Join-Path $base 'precious'; $scratch = Join-Path $base 'scratch'
try {
    New-Item -ItemType Directory -Force (Join-Path $precious 'sub'), (Join-Path $scratch 'a\b') | Out-Null
    'keep' | Set-Content (Join-Path $precious 'sub\keep.txt'); 'keep' | Set-Content (Join-Path $precious 'top.txt')
    'x' | Set-Content (Join-Path $scratch 'a\b\x.txt')
    $ro = Join-Path $scratch 'ro.txt'; 'r' | Set-Content $ro; Set-ItemProperty $ro -Name IsReadOnly -Value $true
    cmd /c mklink /J (Join-Path $scratch 'a\tools') $precious | Out-Null
    Check 'junction created' (Test-ReparsePoint (Join-Path $scratch 'a\tools'))

    Remove-TreeSafe $scratch
    Check 'scratch tree removed' (-not (Test-Path $scratch))
    Check 'junction target keeps top.txt' (Test-Path (Join-Path $precious 'top.txt'))
    Check 'junction target keeps sub\keep.txt' (Test-Path (Join-Path $precious 'sub\keep.txt'))

    # the root itself is a link
    cmd /c mklink /J (Join-Path $base 'rootlink') $precious | Out-Null
    Remove-TreeSafe (Join-Path $base 'rootlink')
    Check 'root link removed' (-not (Test-Path (Join-Path $base 'rootlink')))
    Check 'root link target intact' (Test-Path (Join-Path $precious 'sub\keep.txt'))

    # pipeline form, and a missing path is not an error
    New-Item -ItemType Directory -Force (Join-Path $base 'p1'), (Join-Path $base 'p2') | Out-Null
    cmd /c mklink /J (Join-Path $base 'p1\tools') $precious | Out-Null
    Get-ChildItem $base -Directory -Filter 'p?' | Remove-TreeSafe
    Remove-TreeSafe (Join-Path $base 'does-not-exist')
    Check 'pipeline removes both' (-not (Test-Path (Join-Path $base 'p1')) -and -not (Test-Path (Join-Path $base 'p2')))
    Check 'pipeline target intact' (Test-Path (Join-Path $precious 'top.txt'))
} finally {
    # clean up without following anything
    Get-ChildItem $base -Recurse -Force -Attributes ReparsePoint -ErrorAction SilentlyContinue | ForEach-Object { try { [IO.Directory]::Delete($_.FullName, $false) } catch {} }
    Remove-TreeSafe $base
}

# static guard: the host scripts that clean up build, run and scratch folders must not use a following delete
$guarded = 'host\run_boot.ps1', 'host\run_boot_mods.ps1', 'host\ab_capture.ps1', 'host\water_score_ck.ps1', 'host\play.ps1', 'host\locks.ps1'
foreach ($g in $guarded) {
    $f = Join-Path $root $g
    if (-not (Test-Path $f)) { continue }
    $hits = @(Select-String -Path $f -Pattern 'Remove-Item[^|#]*-Recurse|-Recurse[^|#]*\|\s*Remove-Item' | Where-Object { $_.Line -notmatch '^\s*#' })
    Check "no recursive Remove-Item in $g" ($hits.Count -eq 0)
}
if ($fail) { "FAILED: $fail" ; exit 1 }
'ALL PASS'
exit 0
