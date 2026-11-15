# Builds and runs the CRC32 PCLMUL unit test (tests\hle\crc32_clmul_test.cpp). Output: tests\hle\out\crc32_clmul_test.exe
$ErrorActionPreference = 'Stop'
$root = Resolve-Path (Join-Path $PSScriptRoot '..\..')
$vc = "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
cmd /c "`"$vc`" >nul && set" | ForEach-Object { if ($_ -match '^([^=]+)=(.*)$') { [Environment]::SetEnvironmentVariable($matches[1], $matches[2]) } }
$env:PATH = "$root\tools\llvm\bin;" + $env:PATH
$out = Join-Path $PSScriptRoot 'out'
New-Item -ItemType Directory -Force $out | Out-Null
$exe = Join-Path $out 'crc32_clmul_test.exe'
& clang++ -std=c++20 -O2 -I"$root\runtime" (Join-Path $PSScriptRoot 'crc32_clmul_test.cpp') -o $exe
if ($LASTEXITCODE -ne 0) { throw "build failed" }
& $exe
exit $LASTEXITCODE
