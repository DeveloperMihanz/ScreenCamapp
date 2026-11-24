<#
.SYNOPSIS
    Builds ScreenCam Virtual Camera (native C++ DLLs + WPF app) and creates a
    ready-to-install folder in dist\ScreenCam plus a ZIP of it.

.DESCRIPTION
    Requirements: Visual Studio 2022 (17.8+) with the workloads
      * "Desktop development with C++" (MSVC v143, Windows 11 SDK 10.0.22000.0 or newer)
      * ".NET desktop development" (.NET 8 SDK)

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File Scripts\build.ps1
    powershell -ExecutionPolicy Bypass -File Scripts\build.ps1 -Configuration Debug
    powershell -ExecutionPolicy Bypass -File Scripts\build.ps1 -Installer   # also compiles the Inno Setup installer
#>
[CmdletBinding()]
param(
    [ValidateSet('Release', 'Debug')]
    [string]$Configuration = 'Release',
    [switch]$Installer,
    # Bundle the .NET 8 runtime so end users don't have to install it.
    [switch]$SelfContained
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root

function Find-MSBuild {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path $vswhere)) {
        throw 'vswhere.exe was not found. Install Visual Studio 2022 with the "Desktop development with C++" and ".NET desktop development" workloads.'
    }
    $path = & $vswhere -latest -prerelease -products * -requires Microsoft.Component.MSBuild `
        -find 'MSBuild\**\Bin\MSBuild.exe' | Select-Object -First 1
    if (-not $path) { throw 'MSBuild.exe was not found in the Visual Studio installation.' }
    return $path
}

$msbuild = Find-MSBuild
Write-Host "Using MSBuild: $msbuild" -ForegroundColor Cyan

# 1) Build the whole solution (native projects first, then the app).
& $msbuild "$root\ScreenCam.sln" /restore /m /nologo /v:minimal `
    "/p:Configuration=$Configuration" /p:Platform=x64
if ($LASTEXITCODE -ne 0) { throw "Solution build failed (exit code $LASTEXITCODE)." }

$nativeDir = Join-Path $root "bin\x64\$Configuration"
foreach ($dll in 'ScreenCam.Native.dll', 'ScreenCam.VirtualCameraSource.dll') {
    if (-not (Test-Path (Join-Path $nativeDir $dll))) { throw "$dll was not produced in $nativeDir." }
}

# 2) Publish the app (framework-dependent, win-x64) into dist\ScreenCam.
$dist = Join-Path $root 'dist\ScreenCam'
if (Test-Path $dist) { Remove-Item $dist -Recurse -Force }
& $msbuild "$root\ScreenCam.App\ScreenCam.App.csproj" /restore /t:Publish /nologo /v:minimal `
    "/p:Configuration=$Configuration" /p:Platform=x64 /p:RuntimeIdentifier=win-x64 "/p:SelfContained=$($SelfContained.IsPresent.ToString().ToLower())" `
    "/p:PublishDir=$dist/"
if ($LASTEXITCODE -ne 0) { throw "Publish failed (exit code $LASTEXITCODE)." }

# 3) Add install / uninstall scripts and docs.
Copy-Item "$PSScriptRoot\install.ps1", "$PSScriptRoot\uninstall.ps1", "$PSScriptRoot\install.cmd", "$PSScriptRoot\uninstall.cmd" $dist
Copy-Item "$root\README.md", "$root\LICENSE" $dist
Copy-Item "$root\Documentation\TROUBLESHOOTING.md" $dist

# 4) ZIP the distributable folder.
$version = (Select-Xml -Path "$root\ScreenCam.App\ScreenCam.App.csproj" -XPath '//Version').Node.InnerText
$zip = Join-Path $root "dist\ScreenCam-$version-win-x64.zip"
if (Test-Path $zip) { Remove-Item $zip -Force }
Compress-Archive -Path "$dist\*" -DestinationPath $zip
Write-Host "Distributable folder: $dist" -ForegroundColor Green
Write-Host "Distributable ZIP:    $zip" -ForegroundColor Green

# 5) Optional Inno Setup installer.
if ($Installer) {
    $iscc = @(
        "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe",
        "$env:ProgramFiles\Inno Setup 6\ISCC.exe"
    ) | Where-Object { Test-Path $_ } | Select-Object -First 1
    if (-not $iscc) { throw 'Inno Setup 6 (ISCC.exe) was not found. Install it from https://jrsoftware.org/isinfo.php or omit -Installer.' }
    $isccArgs = @("/DAppVersion=$version", "/DSourceDir=$dist")
    if ($SelfContained) { $isccArgs += '/DSelfContained' }
    & $iscc @isccArgs "$root\Installer\ScreenCam.iss"
    if ($LASTEXITCODE -ne 0) { throw "Installer build failed (exit code $LASTEXITCODE)." }
    Write-Host "Installer written to $root\dist" -ForegroundColor Green
}

Write-Host ''
Write-Host 'Next step: run dist\ScreenCam\install.cmd (it asks for administrator rights).' -ForegroundColor Yellow
