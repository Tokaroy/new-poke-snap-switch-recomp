# Junction-safe recursive delete for the dev scripts (dot-sourced by host\locks.ps1, so every script that takes a lock has it).
#
# Windows PowerShell 5.1 `Remove-Item -Recurse` walks INTO junctions and directory symlinks and deletes what they point to.
# 2026-11-16 a cleanup of build\w\ct_tree (junctions into the repository) emptied tools\ for the whole project.
# Remove-TreeSafe unlinks every reparse point (junction, symlink, anything else) and never enters it.
#
#   Remove-TreeSafe $dir                       # one or more paths (-LiteralPath, wildcards are NOT expanded)
#   Get-ChildItem $gens -Directory | Remove-TreeSafe
#   Remove-TreeSafe $dir -Strict               # throw on the first failure instead of carrying on (default: like -ErrorAction SilentlyContinue)
#
# A root that is itself a link is unlinked (the target stays). Missing paths are ignored.
# Tests: tests\safe_rm_test.ps1 and builder\tests\test_safedel.py (the same rules for the Python builder).

function Test-ReparsePoint {
    param([Parameter(Mandatory)][string]$LiteralPath)
    try { $a = [IO.File]::GetAttributes($LiteralPath) } catch { return $false }
    return (($a -band [IO.FileAttributes]::ReparsePoint) -ne 0)
}

function Remove-LinkOnly {
    # a directory link goes with rmdir semantics (no recursion, target untouched); a file link with a plain delete
    param([Parameter(Mandatory)][string]$LiteralPath)
    $a = [IO.File]::GetAttributes($LiteralPath)
    if ($a -band [IO.FileAttributes]::Directory) { [IO.Directory]::Delete($LiteralPath, $false) }
    else { [IO.File]::Delete($LiteralPath) }
}

function Remove-TreeSafe {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory, ValueFromPipeline, ValueFromPipelineByPropertyName, Position = 0)]
        [Alias('FullName', 'Path')][string[]]$LiteralPath,
        [switch]$Strict)
    begin {
        function Remove-One([string]$p, [bool]$strict) {
            try {
                if (-not ([IO.File]::Exists($p) -or [IO.Directory]::Exists($p) -or (Test-ReparsePoint $p))) { return }
                if (Test-ReparsePoint $p) { Remove-LinkOnly $p; return }
                if ([IO.File]::Exists($p)) {
                    [IO.File]::SetAttributes($p, [IO.FileAttributes]::Normal); [IO.File]::Delete($p); return
                }
                foreach ($c in [IO.Directory]::EnumerateFileSystemEntries($p)) { Remove-One $c $strict }
                [IO.File]::SetAttributes($p, [IO.FileAttributes]::Directory)
                [IO.Directory]::Delete($p, $false)
            } catch { if ($strict) { throw } }
        }
    }
    process {
        foreach ($p in $LiteralPath) {
            if ([string]::IsNullOrWhiteSpace($p)) { continue }
            Remove-One ([IO.Path]::GetFullPath($p)) ([bool]$Strict)
        }
    }
}
