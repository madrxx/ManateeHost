@echo off
setlocal
cd /d "%~dp0"
set "cmakeExe=cmake"
where cmake >nul 2>nul
if errorlevel 1 (
  for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "visualStudioPath=%%i"
)
if defined visualStudioPath set "cmakeExe=%visualStudioPath%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
"%cmakeExe%" -S . -B build -G "Visual Studio 17 2022" -A x64
if errorlevel 1 goto failed
"%cmakeExe%" --build build --config Release --parallel
if errorlevel 1 goto failed
echo.
echo Built: build\Release\ManateeHost.exe
echo ManateeHost requires Windows 11 25H2 or later with MIDI Services enabled.
echo Keep the generated .dll and .pri beside the .exe.
pause
exit /b 0
:failed
echo.
echo Build failed.
pause
exit /b 1
