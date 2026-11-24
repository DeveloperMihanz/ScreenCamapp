@echo off
rem Builds ScreenCam (Release). Pass "Debug" as the first argument for a debug build.
set CFG=%1
if "%CFG%"=="" set CFG=Release
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0build.ps1" -Configuration %CFG%
exit /b %ERRORLEVEL%
