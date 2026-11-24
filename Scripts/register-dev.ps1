<#
.SYNOPSIS
    Developer helper: registers (or unregisters) the camera component straight
    from the build output so the app can be debugged from Visual Studio.

.NOTES
    The Camera Frame Server runs as LOCAL SERVICE, which cannot read folders inside
    a user profile by default. This script grants LOCAL SERVICE read access to the
    build output folder. If your repository is in a location LOCAL SERVICE cannot
    traverse, clone it somewhere like C:\src instead.
#>
#Requires -RunAsAdministrator
[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Debug',
    [switch]$Unregister
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$binDir = Join-Path $root "bin\x64\$Configuration"
$dll = Join-Path $binDir 'ScreenCam.VirtualCameraSource.dll'
if (-not (Test-Path $dll)) { throw "$dll not found. Build the solution ($Configuration|x64) first." }

if ($Unregister) {
    Start-Process "$env:SystemRoot\System32\regsvr32.exe" -ArgumentList '/u', '/s', "`"$dll`"" -Wait
    Write-Host "Unregistered $dll" -ForegroundColor Green
    return
}

& icacls.exe $binDir /grant '*S-1-5-19:(OI)(CI)RX' /C /Q | Out-Null
$p = Start-Process "$env:SystemRoot\System32\regsvr32.exe" -ArgumentList '/s', "`"$dll`"" -Wait -PassThru
if ($p.ExitCode -ne 0) { throw "regsvr32 failed with exit code $($p.ExitCode)" }
Write-Host "Registered $dll" -ForegroundColor Green
Write-Host 'Tip: after rebuilding the DLL, restart the Frame Server so it reloads it:  Restart-Service FrameServer -Force'
