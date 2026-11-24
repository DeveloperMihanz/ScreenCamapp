# Building ScreenCam Virtual Camera

## Required software

| Component | Version | Notes |
|---|---|---|
| Windows | 11 (build 22000+) to run the camera; Windows 10 2004+ can build and run the preview | `MFCreateVirtualCamera` is Windows 11 only |
| Visual Studio 2022 | 17.8 or newer (Community is fine) | |
| Workload **Desktop development with C++** | MSVC v143 toolset | includes MSBuild |
| **Windows 11 SDK** | 10.0.22000.0 or newer (10.0.22621.0 or 10.0.26100.0 recommended) | provides `mfvirtualcamera.h` and the C++/WinRT headers. Select it in the VS installer under *Individual components* |
| Workload **.NET desktop development** | .NET 8 SDK | |
| .NET 8 Desktop Runtime (x64) | 8.0.x | needed on machines that only run the app |
| Inno Setup 6 (optional) | 6.2+ | only to build `Setup.exe` |

You don't need any NuGet packages that need network access. C++/WinRT comes from the
Windows SDK (`Include\<sdk>\cppwinrt`).

## Build: command line (recommended)

From a normal PowerShell prompt in the repository root:

```powershell
powershell -ExecutionPolicy Bypass -File Scripts\build.ps1            # Release
powershell -ExecutionPolicy Bypass -File Scripts\build.ps1 -Configuration Debug
powershell -ExecutionPolicy Bypass -File Scripts\build.ps1 -Installer # also builds dist\ScreenCam-<ver>-Setup.exe
```

The script:

1. Finds MSBuild with `vswhere`.
2. Builds `ScreenCam.sln` for `x64`: `ScreenCam.VirtualCameraSource.dll` and
   `ScreenCam.Native.dll` go to `bin\x64\<Configuration>\`, followed by the WPF app.
3. Publishes the app (framework-dependent, win-x64) to `dist\ScreenCam\`, including the
   native DLLs and the install scripts.
4. Zips `dist\ScreenCam` to `dist\ScreenCam-<version>-win-x64.zip`.

## Build: Visual Studio

1. Open `ScreenCam.sln`.
2. Select **x64** and **Debug** or **Release**.
3. Run *Build → Build Solution*. The solution's project dependencies build the two C++
   projects before `ScreenCam.App`. An after-build step copies the native DLLs next to
   `ScreenCam.exe`.
4. Set **ScreenCam.App** as the startup project.

If you see `mfvirtualcamera.h: No such file`, install a Windows 11 SDK. Then right-click
each C++ project → *Properties → General → Windows SDK Version* and pick it. The projects
default to "10.0", which means the newest installed SDK.

## Install

The camera component must be registered machine-wide, because the Windows Camera Frame
Server service loads it.

```powershell
# from the build output
dist\ScreenCam\install.cmd                # asks for elevation
# or, already elevated:
powershell -ExecutionPolicy Bypass -File Scripts\install.ps1 -SourceDir dist\ScreenCam
```

`install.ps1` does the following:
- copies the files to `C:\Program Files\ScreenCam Virtual Camera`
- runs `regsvr32` on `ScreenCam.VirtualCameraSource.dll` (HKLM COM registration)
- grants `LOCAL SERVICE` read access
- creates the Start menu shortcut and the uninstall entry

To uninstall, use *Installed apps → ScreenCam Virtual Camera*, run `uninstall.cmd`, or run
`Scripts\uninstall.ps1 [-RemoveUserSettings]`.

## Debugging from Visual Studio

The app can run straight from `ScreenCam.App\bin\x64\Debug\net8.0-windows\`, but the camera
component still has to be registered. Use the developer helper, elevated:

```powershell
powershell -ExecutionPolicy Bypass -File Scripts\register-dev.ps1 -Configuration Debug
```

The helper registers `bin\x64\Debug\ScreenCam.VirtualCameraSource.dll` and grants
`LOCAL SERVICE` read access to that folder. If the repository is inside your user profile and
loading still fails, move it to a path such as `C:\src`.

After rebuilding the media source DLL, restart the Frame Server so that it loads the new
copy:

```powershell
Restart-Service FrameServer -Force
```

### Debugging the media source

The media source runs inside the Camera Frame Server: an `svchost.exe` running the
`FrameServer` service as LOCAL SERVICE. Follow these steps:

1. Start ScreenCam and click **START VIRTUAL CAMERA**.
2. Open the camera in an app, for example the Windows **Camera** app.
3. In Visual Studio, use *Debug → Attach to Process*. Enable *Show processes from all
   users*, then pick the `svchost.exe` that hosts `FrameServer`. You can find its PID with
   `tasklist /svc /fi "services eq FrameServer"`.
4. Trace output (`[ScreenCamSource] …`) is also visible in Sysinternals **DebugView**, with
   *Capture Global Win32* enabled.

The app logs to `%LOCALAPPDATA%\ScreenCam\logs\screencam.log`. `ScreenCam.Native` traces to
the debugger with the prefix `[ScreenCam.Native]`.

## Portable unit tests

The shared-memory protocol and the pixel conversion code don't depend on Windows:

```powershell
cd Tests
cl /std:c++17 /EHsc /O2 /W4 /I..\Shared PortableTests.cpp
.\PortableTests.exe
```

On Linux or macOS, use `g++ -std=c++17 -O2 -pthread -I../Shared PortableTests.cpp -o tests && ./tests`.

## SDK and Windows version limitations

| Feature | Minimum |
|---|---|
| Windows Graphics Capture (monitor capture) | Windows 10 1903 |
| Cursor on/off (`IsCursorCaptureEnabled`) | Windows 10 2004 |
| Hide yellow border (`IsBorderRequired`) | Windows 11. Ignored where it isn't permitted |
| Virtual camera (`MFCreateVirtualCamera`) | **Windows 11 21H2 (22000)** |
| Build headers (`mfvirtualcamera.h`) | Windows SDK 10.0.22000.0 |
