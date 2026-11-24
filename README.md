# ScreenCam Virtual Camera

ScreenCam turns any rectangle of your Windows desktop into a webcam. You pick an area of
the screen, click **START VIRTUAL CAMERA**, and **"ScreenCam Virtual Camera"** shows up as a
camera in Chrome, Google Meet, Zoom, Microsoft Teams, OBS Studio and other apps that use the
standard Windows camera APIs.

It uses only Windows components: **Windows Graphics Capture** to capture the screen,
**Direct3D 11** to crop and scale it on the GPU, and the **Windows 11 Virtual Camera API**
(`MFCreateVirtualCamera`, Media Foundation) to publish the camera. It doesn't need OBS,
ManyCam or any third-party driver.

> **Requires Windows 11 (build 22000 or later) to broadcast.** The Virtual Camera API doesn't
> exist on Windows 10. The app still installs and its preview works on Windows 10 2004+, but
> the camera can't be started there. See [Known limitations](#known-limitations).

## Features

- Select any area with a full-screen overlay: drag to create, drag inside to move, drag the
  handles to resize, hold **Shift** to keep the camera's aspect ratio, and use the arrow keys
  to nudge. It works on multi-monitor setups with mixed DPI scaling.
- Live preview of exactly what the camera sends.
- Output **640×480, 1280×720 or 1920×1080** at **30 or 60 fps**. Apps can pick any of these
  modes in NV12 or RGB32.
- Fit (letterbox), Fill (crop) or Stretch scaling.
- GPU scaling and color conversion (D3D11 video processor), with an automatic CPU fallback.
- Frames are only processed when the screen content changes, so a static screen costs
  almost nothing.
- The camera status is clear. The button reads **START VIRTUAL CAMERA** in red while the
  camera is stopped and **VIRTUAL CAMERA ACTIVE** in green while it runs. The status line
  shows whether an app is streaming and at what resolution.
- Settings (area, resolution, fps, preview, cursor, scaling, auto-start) persist in
  `%APPDATA%\ScreenCam\settings.json`, so they survive restarts, reboots and updates.
- The camera is created per session and is removed automatically when ScreenCam closes. It
  is never reported as active unless the native camera actually started.

## Build the desktop app (one click)

- **On your PC:** install Visual Studio 2022 (workloads *Desktop development with C++* and
  *.NET desktop development*, plus the Windows 11 SDK) and, optionally, Inno Setup 6. Then
  double-click **`BUILD-APP.cmd`**. You get `dist\ScreenCam-1.0.0-Setup.exe` and a portable ZIP.
  The .NET runtime is bundled, so users don't need to install it.
- **On GitHub (no Visual Studio needed):** push this folder to a GitHub repository. The
  workflow in `.github/workflows/build.yml` builds on a Windows runner; download
  `ScreenCam-win-x64` from the run in the *Actions* tab. Push a tag such as `v1.0.0` to get a
  Release with `Setup.exe` attached.

## Quick start (end users)

1. If you built without `-SelfContained`, install the **.NET 8 Desktop Runtime (x64)**:
   <https://dotnet.microsoft.com/download/dotnet/8.0>
2. Build the project (see [BUILD.md](BUILD.md)), or use a prebuilt `dist\ScreenCam` folder.
3. Run `dist\ScreenCam\install.cmd` and accept the administrator prompt. This copies the app
   to `C:\Program Files\ScreenCam Virtual Camera` and registers the camera component.
4. Start **ScreenCam Virtual Camera** from the Start menu.
5. Click **Select Screen Area**, drag a rectangle, and press **Enter**.
6. Click **START VIRTUAL CAMERA**.
7. In Zoom, Teams, Meet or OBS, choose **ScreenCam Virtual Camera** as the camera.

To uninstall, use *Settings → Apps → Installed apps → ScreenCam Virtual Camera*, or run
`uninstall.cmd`.

## Repository layout

```
ScreenCam-Virtual-Camera/
├── ScreenCam.sln                       Visual Studio 2022 solution (x64)
├── ScreenCam.App/                      C# / .NET 8 / WPF desktop application
│   ├── MainWindow.xaml(.cs)            main UI and app logic
│   ├── Views/                          region selector overlay, settings dialog
│   ├── Services/                       native interop, capture engine facade, monitors, logging
│   ├── Models/                         region, monitor, output options, camera state
│   ├── Settings/                       persisted settings (JSON)
│   ├── Themes/                         styles
│   └── Assets/                         logo and icon
├── ScreenCam.Native/                   C++ DLL used by the app (P/Invoke)
│   ├── Capture/                        Windows Graphics Capture + D3D11 crop/scale/NV12
│   ├── Camera/                         MFCreateVirtualCamera lifetime
│   ├── MediaFoundation/                MF runtime, shared-memory frame writer
│   ├── Headers/                        public C API (ScreenCamNative.h)
│   └── Source/                         exports, diagnostics
├── ScreenCam.VirtualCameraSource/      C++ COM media source loaded by the Windows Camera Frame Server
├── Shared/                             protocol, conversion and ID headers shared by both C++ DLLs
├── Tests/                              portable unit tests (protocol and pixel conversion)
├── Installer/                          Inno Setup script
├── Scripts/                            build / install / uninstall / developer registration
├── Documentation/                      architecture, troubleshooting, testing report
├── README.md  BUILD.md  LICENSE
```

The project adds one folder to the requested structure, `ScreenCam.VirtualCameraSource`. It
is needed because Windows loads a virtual camera's media source **inside the Camera Frame
Server service**, not inside the app. That source must therefore be a separate COM DLL. See
[Documentation/ARCHITECTURE.md](Documentation/ARCHITECTURE.md).

## Known limitations

- **Windows 11 is required for the camera.** `MFCreateVirtualCamera` is a Windows 11 API.
  On Windows 10 the app explains this and only the preview works. A DirectShow-only fallback
  was not implemented: it would be visible only to DirectShow apps, and the Virtual Camera
  API was preferred.
- **Apps decide whether to accept a camera.** Some apps or security policies reject virtual
  cameras or ask for a "secure"/hardware camera. ScreenCam doesn't hide that it's a virtual
  camera, and it doesn't try to bypass such checks.
- **One display per area.** An area belongs to one monitor. A selection can't span two
  monitors, because Windows Graphics Capture captures one monitor at a time.
- **Yellow capture border.** On Windows 10, and on some Windows 11 configurations, Windows
  draws a border around the captured display. ScreenCam asks Windows 11 to hide it when
  allowed.
- **Protected content.** DRM video and windows that exclude themselves from capture appear
  black. This is enforced by Windows.
- **Format chosen by the app.** Each app picks the camera mode (resolution, fps, NV12/RGB32).
  ScreenCam lists your chosen mode first. If an app picks a different mode, the image is
  scaled for it.
- **One interactive user at a time.** The frame channel is shared system-wide. Two users
  signed in at the same time (fast user switching), each running ScreenCam, aren't supported.
- **x64 only.** An ARM64 build would need ARM64 configurations, which aren't included.
- **Not compiled in the authoring environment.** The source was written without access to
  Windows or Visual Studio. See [Documentation/TESTING.md](Documentation/TESTING.md) for
  exactly what was and wasn't verified.

## Documentation

- [BUILD.md](BUILD.md): required software, building, installing, debugging
- [Documentation/ARCHITECTURE.md](Documentation/ARCHITECTURE.md): how the pieces fit together
- [Documentation/TROUBLESHOOTING.md](Documentation/TROUBLESHOOTING.md): common problems and fixes
- [Documentation/TESTING.md](Documentation/TESTING.md): test report and manual test plan

## License

MIT. See [LICENSE](LICENSE).
