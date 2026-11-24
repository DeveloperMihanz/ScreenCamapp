<#
.SYNOPSIS
    Removes ScreenCam Virtual Camera.

.DESCRIPTION
    Unregisters the camera component, deletes the program folder, the Start menu
    shortcut and the uninstall entry. User settings in %APPDATA%\ScreenCam are kept
    unless -RemoveUserSettings is given.
#>
#Requires -RunAsAdministrator
[CmdletBinding()]
param(
    [string]$InstallDir = (Join-Path $env:ProgramFiles 'ScreenCam Virtual Camera'),
    [switch]$RemoveUserSettings
)

$ErrorActionPreference = 'Continue'
$AppName = 'ScreenCam Virtual Camera'
$dllPath = Join-Path $InstallDir 'ScreenCam.VirtualCameraSource.dll'

Write-Host '==> Stopping ScreenCam' -ForegroundColor Cyan
Get-Process -Name 'ScreenCam' -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 500

Write-Host '==> Unregistering the camera component' -ForegroundColor Cyan
if (Test-Path $dllPath) {
    Start-Process -FilePath "$env:SystemRoot\System32\regsvr32.exe" -ArgumentList '/u', '/s', "`"$dllPath`"" -Wait | Out-Null
}
# Make sure the COM key is gone even if the DLL was already deleted.
Remove-Item 'Registry::HKEY_LOCAL_MACHINE\SOFTWARE\Classes\CLSID\{DBBE7E28-D5A6-4C61-80E9-C51B85F982F2}' -Recurse -Force -ErrorAction SilentlyContinue

# Release the DLL if the Camera Frame Server still has it loaded.
foreach ($svc in 'FrameServer', 'FrameServerMonitor') {
    Stop-Service -Name $svc -Force -ErrorAction SilentlyContinue
}

Write-Host '==> Removing files' -ForegroundColor Cyan
$programs = [Environment]::GetFolderPath('CommonPrograms')
Remove-Item (Join-Path $programs "$AppName.lnk") -Force -ErrorAction SilentlyContinue
Remove-Item 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\ScreenCamVirtualCamera' -Recurse -Force -ErrorAction SilentlyContinue

if (Test-Path $InstallDir) {
    $runningFromInstallDir = $PSScriptRoot -and ((Resolve-Path $PSScriptRoot).Path.TrimEnd('\') -ieq (Resolve-Path $InstallDir).Path.TrimEnd('\'))
    if ($runningFromInstallDir) {
        # This script lives in the folder being deleted: finish from a detached cmd.
        Get-ChildItem $InstallDir -Exclude 'uninstall.ps1' | Remove-Item -Recurse -Force -ErrorAction SilentlyContinue
        Start-Process -FilePath cmd.exe -ArgumentList '/c', "timeout /t 2 >nul & rmdir /s /q `"$InstallDir`"" -WindowStyle Hidden
    } else {
        Remove-Item $InstallDir -Recurse -Force -ErrorAction SilentlyContinue
    }
}

if ($RemoveUserSettings) {
    Write-Host '==> Removing user settings and logs' -ForegroundColor Cyan
    Remove-Item (Join-Path $env:APPDATA 'ScreenCam') -Recurse -Force -ErrorAction SilentlyContinue
    Remove-Item (Join-Path $env:LOCALAPPDATA 'ScreenCam') -Recurse -Force -ErrorAction SilentlyContinue
}

Write-Host ''
Write-Host "$AppName was removed." -ForegroundColor Green
