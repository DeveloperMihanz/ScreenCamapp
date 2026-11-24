; Inno Setup 6 script for ScreenCam Virtual Camera.
; Build it with Scripts\build.ps1 -Installer, or manually:
;   ISCC.exe /DAppVersion=1.0.0 /DSourceDir=..\dist\ScreenCam ScreenCam.iss

#ifndef AppVersion
  #define AppVersion "1.0.0"
#endif
#ifndef SourceDir
  #define SourceDir "..\dist\ScreenCam"
#endif

[Setup]
AppId={{6C0F3C9E-6F49-4B8E-9C55-3F1C2B7A4D10}
AppName=ScreenCam Virtual Camera
AppVersion={#AppVersion}
AppPublisher=ScreenCam
DefaultDirName={autopf}\ScreenCam Virtual Camera
DefaultGroupName=ScreenCam Virtual Camera
DisableProgramGroupPage=yes
OutputDir=..\dist
OutputBaseFilename=ScreenCam-{#AppVersion}-Setup
SetupIconFile=..\ScreenCam.App\Assets\ScreenCam.ico
UninstallDisplayIcon={app}\ScreenCam.exe
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
PrivilegesRequired=admin
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0.19041
LicenseFile=..\LICENSE
CloseApplications=yes

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Files]
Source: "{#SourceDir}\*"; DestDir: "{app}"; Excludes: "install.ps1,install.cmd,ScreenCam.VirtualCameraSource.dll"; Flags: ignoreversion recursesubdirs createallsubdirs restartreplace uninsrestartdelete
; The camera component is registered for the Frame Server (64-bit COM, HKLM).
Source: "{#SourceDir}\ScreenCam.VirtualCameraSource.dll"; DestDir: "{app}"; Flags: ignoreversion regserver 64bit restartreplace uninsrestartdelete

[Icons]
Name: "{group}\ScreenCam Virtual Camera"; Filename: "{app}\ScreenCam.exe"
Name: "{autodesktop}\ScreenCam Virtual Camera"; Filename: "{app}\ScreenCam.exe"; Tasks: desktopicon

[Run]
; Let the Camera Frame Server (LOCAL SERVICE) read the program folder.
Filename: "{sys}\icacls.exe"; Parameters: """{app}"" /grant *S-1-5-19:(OI)(CI)RX /T /C /Q"; Flags: runhidden waituntilterminated
Filename: "{app}\ScreenCam.exe"; Description: "{cm:LaunchProgram,ScreenCam Virtual Camera}"; Flags: nowait postinstall skipifsilent

[UninstallRun]
Filename: "{cmd}"; Parameters: "/c taskkill /im ScreenCam.exe /f"; Flags: runhidden; RunOnceId: "KillApp"

[Code]
function IsDotNet8DesktopInstalled: Boolean;
var
  FindRec: TFindRec;
begin
  Result := FindFirst(ExpandConstant('{commonpf64}\dotnet\shared\Microsoft.WindowsDesktop.App\8.*'), FindRec);
  if Result then
    FindClose(FindRec);
end;

function InitializeSetup: Boolean;
begin
  Result := True;
#ifndef SelfContained
  if not IsDotNet8DesktopInstalled then
  begin
    if MsgBox('ScreenCam needs the .NET 8 Desktop Runtime (x64), which was not found.' + #13#10 + #13#10 +
              'Download it from https://dotnet.microsoft.com/download/dotnet/8.0 after setup finishes.' + #13#10 +
              'Continue installing anyway?', mbConfirmation, MB_YESNO) = IDNO then
      Result := False;
  end;
#endif
  if GetWindowsVersion < $0A0055F0 then  { 10.0.22000 }
    MsgBox('This PC runs a Windows version older than Windows 11 (build 22000).' + #13#10 +
           'ScreenCam will install and its preview will work, but the virtual camera requires Windows 11.',
           mbInformation, MB_OK);
end;
