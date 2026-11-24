# Installer

Two ways to install ScreenCam:

1. **Script installer** (no extra tools needed): `Scripts\install.ps1`, which is copied into
   `dist\ScreenCam\` by the build, together with `install.cmd`, `uninstall.ps1` and
   `uninstall.cmd`.
2. **Setup.exe** built with Inno Setup 6 from `ScreenCam.iss`:
   ```powershell
   powershell -ExecutionPolicy Bypass -File Scripts\build.ps1 -Installer
   ```
   The output is `dist\ScreenCam-<version>-Setup.exe`.

Both installers do the following:
- install to `C:\Program Files\ScreenCam Virtual Camera`
- register `ScreenCam.VirtualCameraSource.dll` (64-bit COM, HKLM), which the Windows Camera
  Frame Server needs
- grant LOCAL SERVICE read access to the program folder
- create a Start menu shortcut and an entry in *Installed apps*

User settings (`%APPDATA%\ScreenCam`) are kept across updates and uninstalls.
