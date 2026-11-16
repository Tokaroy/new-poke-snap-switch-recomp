@echo off
setlocal
pushd "%~dp0"
set CPU_RESULT=2
where cl >nul 2>nul
if not errorlevel 1 goto msvc
where g++ >nul 2>nul
if not errorlevel 1 goto gcc
echo SKIP: no cl or g++ on PATH; CPU tests were not compiled or run.
goto shaders
:msvc
if not exist test-output mkdir test-output
cl /nologo /std:c++17 /EHsc /O2 /W4 ocean_cpu.cpp tests\cpu_tests.cpp /Fotest-output\ /Fetest-output\ocean_cpu_tests.exe
if errorlevel 1 goto compile_failed
goto run_cpu
:gcc
if not exist test-output mkdir test-output
g++ -std=c++17 -O2 -Wall -Wextra -pedantic ocean_cpu.cpp tests\cpu_tests.cpp -o test-output\ocean_cpu_tests.exe
if errorlevel 1 goto compile_failed
:run_cpu
test-output\ocean_cpu_tests.exe
set CPU_RESULT=%ERRORLEVEL%
goto shaders
:compile_failed
set CPU_RESULT=1
:shaders
where glslangValidator >nul 2>nul
if errorlevel 1 (
    echo SKIP: glslangValidator not on PATH; shaders were not compiled.
    goto done
)
if not exist test-output mkdir test-output
for %%S in (spectrum_init evolve ifft output) do (
    glslangValidator -V --target-env vulkan1.1 -Ishaders shaders\%%S.comp -o test-output\%%S.spv
    if errorlevel 1 goto shader_failed
)
echo PASS: all four compute shaders compiled.
goto done
:shader_failed
set CPU_RESULT=1
:done
popd
exit /b %CPU_RESULT%
