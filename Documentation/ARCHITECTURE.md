# Architecture

```
 ┌───────────────────────── user session ─────────────────────────┐    ┌──────────── session 0 ─────────────┐
 │ ScreenCam.exe (WPF, .NET 8)                                     │    │ svchost.exe  (FrameServer service, │
 │   MainWindow / RegionSelector / Settings                        │    │              LOCAL SERVICE)        │
 │        │ P/Invoke (C API)                                       │    │                                     │
 │ ScreenCam.Native.dll                                            │    │ ScreenCam.VirtualCameraSource.dll   │
 │   ScreenCapture ── Windows Graphics Capture (monitor)           │    │   Activator (IMFActivate)           │
 │        │ GPU crop → D3D11 video processor (scale, BGRA→NV12)    │    │   MediaSource (IMFMediaSourceEx,    │
 │        ▼                                                        │    │     IKsControl, IMFGetService,      │
 │   FrameWriter ──────── Global\ shared section (NV12, 2 slots) ──┼────┼─▶   IMFSampleAllocatorControl)      │
 │                                                                 │    │   MediaStream (IMFMediaStream2)     │
 │   VirtualCameraController ── MFCreateVirtualCamera(CLSID) ──────┼────┼─▶ FrameReader                       │
 └─────────────────────────────────────────────────────────────────┘    └──────────────┬──────────────────────┘
                                                                                        │ frames
                                                       Zoom / Teams / Chrome / OBS ◀────┘ (via Frame Server)
```

## Why two native DLLs?

The Windows 11 Virtual Camera API (`MFCreateVirtualCamera`) doesn't take frames from the
calling app. It takes the **CLSID of a Media Foundation media source**. When an app opens
the camera, the **Windows Camera Frame Server** service (session 0, LOCAL SERVICE) creates
that COM object in its own process, and every client app receives frames from the Frame
Server.

The two DLLs split the work as follows:

- `ScreenCam.VirtualCameraSource.dll` is that media source. It must be registered in HKLM
  (`regsvr32`), and the LOCAL SERVICE account must be able to read it.
- `ScreenCam.Native.dll` runs inside the app. A session-0 service can't capture the user's
  desktop, so capture has to happen in the user's session. This DLL captures and processes
  frames, then hands them to the media source.

## Frame transport (Shared/SharedFrameProtocol.h)

- The transport is one pagefile-backed section named
  `Global\ScreenCamVirtualCamera_Frames_v1`. It holds a 4 KiB header followed by two NV12
  slots, each sized for 1920×1080.
- The **media source creates** the section. Services hold `SeCreateGlobalPrivilege`. Its
  DACL grants LOCAL SERVICE, SYSTEM, Administrators and Interactive users access; nobody else
  gets in. The app opens it, and an elevated app may also create it.
- The writer uses a **seqlock per slot** and always fills the slot that isn't the latest. A
  reader that loses a race detects it from the sequence number and keeps its previous frame.
  The reader validates every size field before copying, so a corrupt header can't cause an
  overflow.
- No cross-session events are used. The media source delivers frames at the negotiated rate
  from its own high-resolution timer and re-reads the latest slot each time. The app writes
  only when the screen changed.
- **Heartbeats** go both ways. The app shows "streaming to an app…" and sends frames only
  while the source is alive. The source shows a "no signal" pattern if the app stops sending
  for more than 3 s.
- The app also writes its chosen output mode. The source lists that mode first when an app
  enumerates the camera's media types.

## Capture pipeline (ScreenCam.Native/Capture)

1. A `GraphicsCaptureItem` is created for the monitor that contains the selected area
   (`IGraphicsCaptureItemInterop::CreateForMonitor`), with a free-threaded frame pool
   (2 buffers, BGRA).
2. `FrameArrived` runs on a thread-pool thread. It GPU-copies only the selected rectangle
   (`CopySubresourceRegion`) into a crop texture and marks the frame dirty. When the monitor
   resolution changes, the pool is recreated (`Recreate`).
3. A worker thread runs at the output frame rate (high-resolution waitable timer). On a dirty
   frame, the **D3D11 video processor** scales the crop with Fit/Fill/Stretch and converts it
   to NV12 (BT.709, limited range) on the GPU. The worker then copies the result to a
   staging texture, maps it, and publishes it, plus a ~15 fps BGRA preview for the UI.
4. If the video processor isn't available (some VMs, WARP), the CPU path takes over:
   staging BGRA → `FrameConvert.h` scaling and BT.709 conversion.

All use of the immediate context goes through one mutex, and the device is multithread
protected.

## Media source (ScreenCam.VirtualCameraSource)

- **Activator**: an `IMFActivate` with an attribute store. `ActivateObject` creates a single
  `MediaSource`.
- **MediaSource**: a live source (`MFMEDIASOURCE_IS_LIVE`) with one stream. It implements
  `Start`/`Stop`/`Shutdown` with the standard `MENewStream`/`MEUpdatedStream`/
  `MESourceStarted`/`MESourceStopped` events. `IKsControl` reports no property sets.
  `IMFSampleAllocatorControl` accepts the Frame Server's sample allocator.
- **MediaStream**: an `IMFMediaStream2` that has these stream attributes:
  `MF_DEVICESTREAM_STREAM_CATEGORY = PINNAME_VIDEO_CAPTURE`, `FRAMESERVER_SHARED = 1` and
  `FRAMESOURCE_TYPES = Color`.
  - It advertises 12 media types: {NV12, RGB32} × {640×480, 1280×720, 1920×1080} ×
    {30, 60} fps.
  - A delivery thread answers `RequestSample` tokens, paced to the frame duration, with
    `MFGetSystemTime` timestamps.
  - It uses the provided allocator when available and falls back to `MFCreate2DMediaBuffer`
    otherwise.
- If the app's frame size differs from the negotiated one, the stream scales the frame
  (nearest neighbour) and letterboxes it to keep the aspect ratio.

## Lifetime and error handling

- The camera is created with `MFVirtualCameraLifetime_Session` and
  `MFVirtualCameraAccess_CurrentUser`. It needs no elevation and disappears automatically
  when ScreenCam exits or crashes, so it can never be left "active" without a producer.
- **START** checks the OS version, checks the COM registration and makes sure capture is
  running, then calls `MFCreateVirtualCamera` and `Start`. The UI only turns green after
  this succeeds.
- **STOP** calls `Stop`, `Remove` and `Shutdown`, then releases the camera. A single app
  instance (named mutex) and a single capture session prevent duplicates.
- Display changes are handled in three ways:
  - `SystemEvents.DisplaySettingsChanged` is debounced, the saved region is re-resolved
    (rescaled if the resolution changed), and capture restarts.
  - `GraphicsCaptureItem.Closed` (monitor unplugged) is detected and reported.
  - DPI: the process is Per-Monitor-V2 aware, so all coordinates sent to native code are
    physical pixels. The overlay uses one window per monitor, so each window has a single
    DPI.

## Settings

`%APPDATA%\ScreenCam\settings.json` holds the region relative to its monitor, plus the
monitor's device name and size at selection time, the output size, fps, scaling, cursor,
preview and auto-start settings, and the window placement. It is written atomically
(temp file and rename). A corrupt file is backed up as `settings.json.corrupt` and
defaults are used.
