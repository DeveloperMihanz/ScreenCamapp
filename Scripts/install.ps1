<#
.SYNOPSIS
    Installs ScreenCam Virtual Camera and registers its camera component.

.DESCRIPTION
    * Copies the application to "%ProgramFiles%\ScreenCam Virtual Camera".
    * Registers ScreenCam.VirtualCameraSource.dll (COM, HKLM) with regsvr32 so the
      Windows Camera Frame Server can load it.
    * Grants LOCAL SERVICE (the Frame Server account) read access to the folder.
    * Creates a Start menu shortcut and an "Apps & features" uninstall entry.
    Must run elevated. User settings (%APPDATA%\ScreenCam) are never touched.

.PARAMETER SourceDir
    Folder containing ScreenCam.exe and the native DLLs. Defaults to the folder of
    this script (when run from dist\ScreenCam) or ..\dist\ScreenCam.
#>
#Requires -RunAsAdministrator
[CmdletBinding()]
param(
    [string]$SourceDir,
    [string]$InstallDir = (Join-Path $env:ProgramFiles 'ScreenCam Virtual Camera'),
    [switch]$NoShortcut
)

$ErrorActionPreference = 'Stop'
$AppName = 'ScreenCam Virtual Camera'
$SourceDll = 'ScreenCam.VirtualCameraSource.dll'
$UninstallKey = 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\ScreenCamVirtualCamera'

function Write-Step($text) { Write-Host "==> $text" -ForegroundColor Cyan }

if (-not $SourceDir) {
    if (Test-Path (Join-Path $PSScriptRoot 'ScreenCam.exe')) { $SourceDir = $PSScriptRoot }
    else { $SourceDir = Join-Path (Split-Path -Parent $PSScriptRoot) 'dist\ScreenCam' }
}
$SourceDir = (Resolve-Path $SourceDir).Path

foreach ($file in 'ScreenCam.exe', 'ScreenCam.dll', 'ScreenCam.Native.dll', $SourceDll) {
    if (-not (Test-Path (Join-Path $SourceDir $file))) {
        throw "Required file '$file' not found in '$SourceDir'. Build first with Scripts\build.ps1."
    }
}

# --- Environment checks (warnings only) ---------------------------------------
$build = [int](Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion').CurrentBuildNumber
if ($build -lt 22000) {
    Write-Warning "Windows build $build detected. The Windows Virtual Camera API requires Windows 11 (build 22000+). The app will install and the preview will work, but the camera cannot be started."
}
$hasDesktopRuntime = $false
# A self-contained build carries its own runtime (hostfxr.dll next to the app).
if (Test-Path (Join-Path $PSScriptRoot 'hostfxr.dll')) { $hasDesktopRuntime = $true }
try {
    $runtimes = & dotnet --list-runtimes 2>$null
    $hasDesktopRuntime = [bool]($runtimes | Select-String -Pattern '^Microsoft\.WindowsDesktop\.App 8\.')
} catch { }
if (-not $hasDesktopRuntime) {
    Write-Warning '.NET 8 Desktop Runtime (x64) was not detected. Install it from https://dotnet.microsoft.com/download/dotnet/8.0 (".NET Desktop Runtime").'
}

# --- Stop running instances ----------------------------------------------------
Write-Step 'Stopping running ScreenCam instances'
Get-Process -Name 'ScreenCam' -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
Start-Sleep -Milliseconds 500

# --- Copy files -----------------------------------------------------------------
function Copy-AppFiles {
    New-Item -ItemType Directory -Force -Path $InstallDir | Out-Null
    Copy-Item -Path (Join-Path $SourceDir '*') -Destination $InstallDir -Recurse -Force
}

Write-Step "Copying files to $InstallDir"
try {
    Copy-AppFiles
} catch {
    # An older camera DLL may still be loaded by the Camera Frame Server.
    Write-Warning 'Some files are in use; restarting the Windows Camera Frame Server and retrying.'
    foreach ($svc in 'FrameServer', 'FrameServerMonitor') {
        Stop-Service -Name $svc -Force -ErrorAction SilentlyContinue
    }
    Start-Sleep -Seconds 1
    Copy-AppFiles
}
Copy-Item (Join-Path $PSScriptRoot 'uninstall.ps1') $InstallDir -Force -ErrorAction SilentlyContinue

# --- Permissions for the Frame Server (LOCAL SERVICE, S-1-5-19) ----------------------
Write-Step 'Granting the Camera Frame Server read access'
& icacls.exe $InstallDir /grant '*S-1-5-19:(OI)(CI)RX' /T /C /Q | Out-Null

# --- COM registration -------------------------------------------------------------
Write-Step "Registering $SourceDll"
$dllPath = Join-Path $InstallDir $SourceDll
$proc = Start-Process -FilePath "$env:SystemRoot\System32\regsvr32.exe" -ArgumentList '/s', "`"$dllPath`"" -Wait -PassThru
if ($proc.ExitCode -ne 0) {
    throw "regsvr32 failed with exit code $($proc.ExitCode). The camera component could not be registered."
}
$clsidKey = 'Registry::HKEY_LOCAL_MACHINE\SOFTWARE\Classes\CLSID\{DBBE7E28-D5A6-4C61-80E9-C51B85F982F2}\InprocServer32'
if (-not (Test-Path $clsidKey)) { throw 'Registration did not create the expected COM registry key.' }

# --- Shortcut -----------------------------------------------------------------------
$exePath = Join-Path $InstallDir 'ScreenCam.exe'
if (-not $NoShortcut) {
    Write-Step 'Creating Start menu shortcut'
    $programs = [Environment]::GetFolderPath('CommonPrograms')
    $shell = New-Object -ComObject WScript.Shell
    $lnk = $shell.CreateShortcut((Join-Path $programs "$AppName.lnk"))
    $lnk.TargetPath = $exePath
    $lnk.WorkingDirectory = $InstallDir
    $lnk.IconLocation = "$exePath,0"
    $lnk.Description = 'Broadcast any area of your screen as a webcam'
    $lnk.Save()
}

# --- Apps & features entry -------------------------------------------------------------
Write-Step 'Registering uninstall entry'
$version = (Get-Item $exePath).VersionInfo.ProductVersion
New-Item -Path $UninstallKey -Force | Out-Null
$uninstallCmd = "powershell.exe -NoProfile -ExecutionPolicy Bypass -File `"$InstallDir\uninstall.ps1`""
$props = @{
    DisplayName     = $AppName
    DisplayVersion  = "$version"
    Publisher       = 'ScreenCam'
    InstallLocation = $InstallDir
    DisplayIcon     = "$exePath,0"
    UninstallString = $uninstallCmd
    NoModify        = 1
    NoRepair        = 1
}
foreach ($k in $props.Keys) {
    $type = if ($props[$k] -is [int]) { 'DWord' } else { 'String' }
    New-ItemProperty -Path $UninstallKey -Name $k -Value $props[$k] -PropertyType $type -Force | Out-Null
}

Write-Host ''
Write-Host "$AppName was installed successfully." -ForegroundColor Green
Write-Host "Start it from the Start menu or run: `"$exePath`""
