# Test report and test plan

## Authoring environment

The project was written in a **Linux container** that has **no Windows, no Visual Studio,
no Windows SDK, no .NET SDK, no GPU and no camera stack**. Per the project brief, no test
that couldn't run is reported as passed.

## Tests that were performed

| Test | How | Result |
|---|---|---|
| Shared-memory frame protocol: init, publish, read, change detection, rejection of invalid sizes and corrupt headers | `Tests/PortableTests.cpp`, g++ 13, `-std=c++17 -Wall -Wextra -Wpedantic -Wshadow` | **Passed.** No compiler warnings |
| Protocol under concurrency: a writer thread publishes 3,000 frames (640×480) while a reader thread reads, and every accepted frame is checked for tearing | same | **Passed.** About 2,400–2,800 frames accepted per run, 0 inconsistent |
| BGRA → NV12 → BGRA round trip (BT.709 limited range) | same | **Passed.** Max channel error 3/255. White→Y 235 and black→Y 16, neutral chroma |
| NV12 nearest-neighbour scaling, identity copy with pitch, 2× upscale, letterboxing | same | **Passed** |
| Fit and Fill geometry (pillarbox, letterbox, crop), even alignment | same | **Passed** |
| Memory safety of the above | same tests with `-fsanitize=address,undefined` | **Passed.** No reports |
| Thread sanitizer | `-fsanitize=thread` | Reports races on slot pixel and metadata bytes. **Expected:** this is the seqlock pattern, where readers copy speculatively and discard torn copies through the sequence check (the concurrency test proves none are accepted). TSan also warns that it doesn't model `atomic_thread_fence`. Size fields are validated before every copy |
| XML well-formedness of all `.xaml`, `.csproj`, `.vcxproj` and `app.manifest` files | Python `xml.dom.minidom` | **Passed** |
| No TODO/FIXME placeholders in source | `grep` | **Passed** |

## Tests that could NOT be performed (no Windows / Visual Studio available)

| Required test | Status |
|---|---|
| Solution compilation (`ScreenCam.sln`) | **Not performed** |
| C# project compilation (WPF, .NET 8) | **Not performed** |
| Native C++ compilation (MSVC v143, Windows SDK, C++/WinRT) | **Not performed** |
| Virtual camera registration (`regsvr32`, `MFCreateVirtualCamera`) | **Not performed** |
| Camera discovery in Windows Camera / Chrome / Meet / Zoom / Teams / OBS | **Not performed** |
| Screen-region capture (Windows Graphics Capture) | **Not performed** |
| Frame delivery through the Frame Server | **Not performed** |
| Start/Stop lifecycle | **Not performed** |
| Resource cleanup / leak checks | **Not performed** |
| Multi-monitor, mixed-DPI and resolution-change handling | **Not performed** |
| Installer (`install.ps1`, Inno Setup) | **Not performed** |

The Windows-specific code was reviewed by reading it against the documented API contracts.
Expect the first build on a Windows machine to reveal compile errors that can only be found
with the real MSVC and Windows SDK headers. These would typically be small header or
signature differences. Please run the plan below and report anything that fails.

## Manual test plan (run on Windows 11)

### Build
1. Run `Scripts\build.ps1`. Expected: it completes, and `dist\ScreenCam` contains
   `ScreenCam.exe`, `ScreenCam.dll`, `ScreenCam.Native.dll` and
   `ScreenCam.VirtualCameraSource.dll`.
2. Run `Tests\PortableTests.exe` (build it with `cl`, see BUILD.md). Expected: 0 failures.

### Installation
3. Run `dist\ScreenCam\install.cmd`. Expected: it reports success. The registry key
   `HKLM\SOFTWARE\Classes\CLSID\{DBBE7E28-D5A6-4C61-80E9-C51B85F982F2}\InprocServer32`
   exists, and the Start menu shortcut is present.

### Application
4. Launch the app. Expected: no banner; status **Stopped** in red; button
   **START VIRTUAL CAMERA** in red.
5. Click **Select Screen Area**. Expected: an overlay appears on every monitor, and the main
   window hides. Drag to select, move and resize the selection, and use Shift and the arrow
   keys. Press **Enter**. Expected: the main window returns, the area size is shown, and
   the live preview appears.
6. Repeat on a second monitor with different scaling (for example 100% and 150%). Expected:
   the preview matches the selected pixels exactly.
7. Change the resolution and frame rate. Expected: the preview aspect follows, and settings
   are saved. Restart the app: the area, resolution and frame rate are restored.

### Virtual camera
8. Click **START VIRTUAL CAMERA**. Expected: the button turns green with the text
   **VIRTUAL CAMERA ACTIVE**, and the status reads **Active**.
9. Open the **Camera** app and switch to **ScreenCam Virtual Camera**. Expected: the screen
   area is shown, and the status reads "Streaming to an app · W × H @ fps (NV12)".
10. Repeat in Chrome (<https://webcamtests.com>), Google Meet, Zoom, Teams and OBS (Video
    Capture Device). Record the results per app. App-specific policies may reject virtual
    cameras; this is a known limitation.
11. Open the camera in two apps at the same time. Expected: both receive video (Frame Server
    sharing) unless one app requests exclusive access.
12. Click **Stop Virtual Camera**. Expected: the camera disappears from apps, and the button
    is red again. Repeat start and stop 20 times; ScreenCam's handle and memory counts in
    Task Manager should stay flat.
13. Close ScreenCam while an app is streaming. Expected: the camera is removed, and the app
    reports that the camera is gone. Running `tasklist /m ScreenCam.VirtualCameraSource.dll`
    shows the Frame Server unloading the DLL after a while.

### Robustness
14. Change the display resolution while streaming. Expected: capture restarts within about
    1 s and the area is rescaled.
15. Unplug the monitor that holds the area. Expected: a banner asks you to select a new
    area, and apps show the "no signal" pattern.
16. Turn off *Settings → Privacy & security → Camera → Let desktop apps access your camera*,
    then click Start. Expected: a clear "access denied" message, and the status is not
    shown as active.
17. Reboot with **Start automatically** enabled. Expected: the camera becomes **Active**
    only if it really started. Otherwise the error is shown.
18. With a static screen, CPU use is low; check Task Manager. A video playing in the area
    should reach the selected fps.
19. Run the uninstaller. Expected: the files, the COM key, the shortcut and the uninstall
    entry are removed.
