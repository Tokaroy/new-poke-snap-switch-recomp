# Dot-source: . (Join-Path $PSScriptRoot 'test_lock.ps1')
# One-at-a-time lock for the boot test scripts (they share build\test* folders). Same rule as host\locks.ps1 for a dead owner: a lock whose PID is
# gone (or whose PID now belongs to a process started after the lock was written) is stale and is taken over; a lock held by a live process is
# never touched. Unlike locks.ps1 there is no 20-minute age limit, because these tests wait for run slots and legitimately run for an hour.
# Record: "<name> pid=<PID> start=<process start ticks> <time>". Old records ("switch-test <PID> <time>", "ui-test <PID> <time>") still parse.
# Enter-TestLock creates the file atomically (CreateNew). Exit-TestLock removes it only when it holds this process's PID.

function Get-TestLockOwner([string]$Text) {
    if ($Text -match '(?:^|\s)pid=(\d+)(?:\s|$)') { $ownerPid = [int]$Matches[1] }
    elseif ($Text -match '^\S+\s+(\d+)\s') { $ownerPid = [int]$Matches[1] }
    else { return $null }
    $ticks = 0L
    if ($Text -match '(?:^|\s)start=(\d+)(?:\s|$)') { $ticks = [int64]$Matches[1] }
    [pscustomobject]@{ Pid = $ownerPid; Ticks = $ticks }
}

function Test-TestLockStale([string]$Path) {
    $item = Get-Item -LiteralPath $Path -ErrorAction SilentlyContinue
    if ($null -eq $item) { return $false }
    $text = ''; try { $text = [IO.File]::ReadAllText($Path).Trim() } catch { return $false }
    $o = Get-TestLockOwner $text
    if ($null -eq $o) { return (([DateTime]::UtcNow - $item.LastWriteTimeUtc).TotalSeconds -gt 12) }   # empty or unreadable record: 12 s to finish writing
    $p = Get-Process -Id $o.Pid -ErrorAction SilentlyContinue
    if ($null -eq $p) { return $true }
    if ($o.Ticks -gt 0) { try { return $p.StartTime.Ticks -ne $o.Ticks } catch { return $false } }   # the PID was reused by another process
    return $false
}

function Enter-TestLock([string]$Path, [string]$Name, [int]$WaitSec = 2000) {
    New-Item -ItemType Directory -Force (Split-Path -Parent $Path) | Out-Null
    $tag = "$Name pid=$PID start=$((Get-Process -Id $PID).StartTime.Ticks)"
    $until = (Get-Date).AddSeconds($WaitSec)
    while ($true) {
        try {
            $fs = [IO.File]::Open($Path, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::Read)
            $b = [Text.Encoding]::ASCII.GetBytes("$tag $(Get-Date -Format s)"); $fs.Write($b, 0, $b.Length); $fs.Close()
            return $true
        } catch [System.IO.IOException] {
            if (Test-TestLockStale $Path) {
                $held = ''; try { $held = [IO.File]::ReadAllText($Path).Trim() } catch {}
                Write-Host "$(Split-Path -Leaf $Path) is stale ($held): its owner is gone; taking it over"
                Remove-Item -LiteralPath $Path -Force -ErrorAction SilentlyContinue
                continue
            }
            if ((Get-Date) -gt $until) { return $false }
            Start-Sleep -Seconds 5
        }
    }
}

function Exit-TestLock([string]$Path) {
    $held = ''; try { $held = [IO.File]::ReadAllText($Path) } catch { return }
    $o = Get-TestLockOwner $held
    if ($null -ne $o -and $o.Pid -eq $PID) { Remove-Item -LiteralPath $Path -Force -ErrorAction SilentlyContinue }
}
