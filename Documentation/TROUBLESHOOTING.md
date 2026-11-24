# Troubleshooting

Logs: `%LOCALAPPDATA%\ScreenCam\logs\screencam.log`. Use *Settings → Open log folder* to get
there. *Settings → Diagnostics* shows the Windows build, Virtual Camera API availability and
the component registration state.

## "ScreenCam Virtual Camera" doesn't appear in an app

1. Check that the status says **Active**. The camera exists only while ScreenCam runs and
   the camera is started.
2. Make sure the app lists cameras after the camera was started. Restart the app, or
   re-open its camera settings.
3. Go to *Settings → Privacy & security → Camera* and turn on **Camera access** and **Let
   desktop apps access your camera**.
4. Check that you're on **Windows 11** (`winver` shows build 22000 or later). Windows 10
   has no Virtual Camera API.
5. Make sure the browser isn't blocking the camera. In Chrome, open
   `chrome://settings/content/camera`.

## Banner: "The ScreenCam camera component is not registered"

Run `install.cmd`, or `Scripts\install.ps1` as Administrator, then click **Check again**.
To verify manually:

```powershell
reg query "HKLM\SOFTWARE\Classes\CLSID\{DBBE7E28-D5A6-4C61-80E9-C51B85F982F2}\InprocServer32"
```

The default value must point to an existing `ScreenCam.VirtualCameraSource.dll`.

## The camera shows a dark striped "no signal" image

The media source is running, but it isn't receiving frames from the app. Work through these
causes:

- **ScreenCam's capture isn't running.** Check the status bar for "Capture stopped". Select
  a screen area again.
- **The selected display was disconnected.** Select a new area.
- **The shared section couldn't be created.** Attach DebugView with *Capture Global Win32*
  and look for `[ScreenCamSource] Shared section unavailable …`. Some hardened systems
  remove `SeCreateGlobalPrivilege` from service accounts. As a workaround, run ScreenCam
  once **as administrator**. The app then creates the section itself with the correct
  permissions.
- **The app stopped sending frames.** The image returns within about 1 s of capture
  resuming.

## Error: "Windows denied access to create the virtual camera"

Camera privacy settings or a device policy (Group Policy / Intune: *Camera → Allow camera*)
block camera access. Enable the camera in *Settings → Privacy & security → Camera* or ask
your administrator.

## Error: "Could not start the virtual camera: 0x…"

- `0x80040154` (class not registered): reinstall as Administrator.
- `0x80070005` (access denied): see the previous section.
- Other codes: restart the Frame Server (`Restart-Service FrameServer -Force`, elevated),
  then try again. If the error persists, send the log file with your bug report.

## An app shows its own error, for example "camera is being used by another application"

- Another app may be holding the camera exclusively. The Frame Server normally shares
  ScreenCam's stream (the stream is marked *Frame Server shared*), but some apps open
  cameras in exclusive mode. Close other camera apps, then retry.
- Some apps show messages such as *"A third-party app is modifying the video"* or
  *"Hardware not capable of being secured"*. These come from that app's own policy for
  virtual or software cameras. ScreenCam registers as a standard software camera through
  the Windows API. It doesn't impersonate hardware and doesn't bypass these checks, so it
  can't make such an app accept it. Try the app's web version, or check its settings for
  an option to allow virtual cameras.

## The preview is black or freezes

- Protected (DRM) video and windows that exclude themselves from capture show as black.
- The preview updates only when the screen content changes. A static screen is expected to
  look frozen.
- Remote Desktop sessions and some virtual machines fall back to CPU processing; the status
  bar shows "CPU scaling". Capture still works, but needs more CPU.

## A yellow border appears around the captured display

Windows draws it to show that capture is active. On Windows 11, *Settings → Hide the Windows
capture border* asks Windows to hide it where policy permits. Windows 10 always shows it.

## The selected area is in the wrong place after changing scaling or resolution

Areas are stored relative to their display and rescaled when the resolution changes. If the
area still looks wrong (for example, after rearranging displays), select it again.

## Updating to a new version fails with "file in use"

`install.ps1` automatically restarts the Camera Frame Server when the old DLL is locked. If
that fails, close every app that uses a camera and run the installer again.
