@echo off
rem ===================================================================
rem  ScreenCam Virtual Camera - one-click build
rem  Double-click this file on Windows (Visual Studio 2022 installed).
rem  Output:
rem    dist\ScreenCam\                         ready-to-run app folder
rem    dist\ScreenCam-<ver>-win-x64.zip        portable ZIP
rem    dist\ScreenCam-<ver>-Setup.exe          installer (if Inno Setup 6 is installed)
rem  The .NET runtime is bundled, so end users need nothing extra.
rem ===================================================================
setlocal
cd /d "%~dp0"

set INSTALLER=
if exist "%ProgramFiles(x86)%\Inno Setup 6\ISCC.exe" set INSTALLER=-Installer
if exist "%ProgramFiles%\Inno Setup 6\ISCC.exe" set INSTALLER=-Installer

powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0Scripts\build.ps1" -Configuration Release -SelfContained %INSTALLER%
if errorlevel 1 (
  echo.
  echo BUILD FAILED. Scroll up for the error. See BUILD.md for the required software.
  pause
  exit /b 1
)

echo.
if "%INSTALLER%"=="" (
  echo Done. Inno Setup 6 was not found, so no Setup.exe was made.
  echo Install it from https://jrsoftware.org/isinfo.php and run this again for an installer,
  echo or run dist\ScreenCam\install.cmd now.
) else (
  echo Done. Run dist\ScreenCam-*-Setup.exe to install ScreenCam.
)
explorer "%~dp0dist"
pause
