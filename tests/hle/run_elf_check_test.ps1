# Builds and runs the converted-game-file check test (tests\hle\elf_check_test.cpp). Output: tests\hle\out\elf_check_test.exe
# Optional: run_elf_check_test.ps1 <main_elf> <sdk_elf> also checks the real converted files.
$ErrorActionPreference = 'Stop'
$root = Resolve-Path (Join-Path $PSScriptRoot '..\..')
$vc = "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
cmd /c "`"$vc`" >nul && set" | ForEach-Object { if ($_ -match '^([^=]+)=(.*)$') { [Environment]::SetEnvironmentVariable($matches[1], $matches[2]) } }
$env:PATH = "$root\tools\llvm\bin;" + $env:PATH
$out = Join-Path $PSScriptRoot 'out'
New-Item -ItemType Directory -Force $out | Out-Null
$exe = Join-Path $out 'elf_check_test.exe'
& clang++ -std=c++17 -O1 -D_CRT_SECURE_NO_WARNINGS (Join-Path $PSScriptRoot 'elf_check_test.cpp') (Join-Path $root 'host\elf_check.cpp') -o $exe
if ($LASTEXITCODE -ne 0) { throw "build failed" }
& $exe @args
exit $LASTEXITCODE
