@echo off
rem Uninstalls ScreenCam Virtual Camera (requests administrator rights).
powershell.exe -NoProfile -ExecutionPolicy Bypass -Command "Start-Process powershell.exe -Verb RunAs -Wait -ArgumentList '-NoProfile','-ExecutionPolicy','Bypass','-NoExit','-File','\"%~dp0uninstall.ps1\"'"
