@echo off
rem Builds and runs build\nvn\hostbuf_test.exe (fault injection for the host buffer helper, no GPU).
rem Run at BelowNormal priority: cmd /c "start /belownormal /wait /b tests\nvn\build_hostbuf.bat"
setlocal
set ROOT=%~dp0..\..
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
if not exist "%ROOT%\build\nvn\hostbuf" mkdir "%ROOT%\build\nvn\hostbuf"
cl /nologo /std:c++20 /EHsc /O1 /W3 /MD /I"%ROOT%\runtime\nvn" /I"%ROOT%\tools\vulkan_sdk\Include" "%ROOT%\tests\nvn\hostbuf_test.cpp" /Fo"%ROOT%\build\nvn\hostbuf\\" /Fe"%ROOT%\build\nvn\hostbuf_test.exe" || exit /b 1
"%ROOT%\build\nvn\hostbuf_test.exe"
