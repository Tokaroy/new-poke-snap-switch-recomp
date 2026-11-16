@echo off
setlocal
pushd "%~dp0"
where cl >nul 2>nul
if not errorlevel 1 goto msvc
where g++ >nul 2>nul
if not errorlevel 1 goto gcc
echo SKIP: no cl or g++ on PATH; cheat VM native tests were not compiled or run.
popd
exit /b 2
:msvc
if not exist test-output mkdir test-output
cl /nologo /std:c++17 /EHsc /O2 /W4 cheat_vm.cpp tests\cheat_tests.cpp /Fotest-output\ /Fetest-output\cheat_tests.exe
if errorlevel 1 goto failed
goto run
:gcc
if not exist test-output mkdir test-output
g++ -std=c++17 -O2 -Wall -Wextra -pedantic cheat_vm.cpp tests\cheat_tests.cpp -o test-output\cheat_tests.exe
if errorlevel 1 goto failed
:run
test-output\cheat_tests.exe
set TEST_RESULT=%ERRORLEVEL%
popd
exit /b %TEST_RESULT%
:failed
popd
exit /b 1
