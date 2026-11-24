namespace ScreenCam.App.Models;

/// <summary>A connected display, in physical (device) pixels.</summary>
public sealed class MonitorInfo
{
    public required IntPtr Handle { get; init; }
    public required string DeviceName { get; init; }
    public required PixelRect Bounds { get; init; }
    public required PixelRect WorkArea { get; init; }
    public required bool IsPrimary { get; init; }
    public required int Index { get; init; }
    public uint DpiX { get; init; } = 96;

    public double Scale => DpiX / 96.0;

    public string DisplayName =>
        $"Display {Index + 1}{(IsPrimary ? " (primary)" : string.Empty)} — {Bounds.Width}×{Bounds.Height}, {Math.Round(Scale * 100)}%";
}
