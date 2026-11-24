namespace ScreenCam.App.Models;

/// <summary>
/// A rectangular screen area, stored relative to the monitor it was selected on
/// (physical pixels). Storing the monitor size at selection time lets us rescale
/// the region when the display resolution changes.
/// </summary>
public sealed class ScreenRegion
{
    public string MonitorDeviceName { get; set; } = string.Empty;
    public int MonitorLeft { get; set; }
    public int MonitorTop { get; set; }
    public int MonitorWidth { get; set; }
    public int MonitorHeight { get; set; }

    /// <summary>Offset from the monitor's top-left corner, physical pixels.</summary>
    public int X { get; set; }
    public int Y { get; set; }
    public int Width { get; set; }
    public int Height { get; set; }

    public bool IsValid => Width >= 2 && Height >= 2 && MonitorWidth > 0 && MonitorHeight > 0;

    public ScreenRegion Clone() => (ScreenRegion)MemberwiseClone();

    /// <summary>
    /// Maps the region onto the current layout of <paramref name="monitor"/>,
    /// scaling it proportionally if the monitor resolution changed, and clamps it
    /// to the monitor bounds. Returns virtual-screen coordinates.
    /// </summary>
    public PixelRect ResolveOn(MonitorInfo monitor)
    {
        double sx = MonitorWidth > 0 ? (double)monitor.Bounds.Width / MonitorWidth : 1.0;
        double sy = MonitorHeight > 0 ? (double)monitor.Bounds.Height / MonitorHeight : 1.0;
        int x = (int)Math.Round(X * sx);
        int y = (int)Math.Round(Y * sy);
        int w = (int)Math.Round(Width * sx);
        int h = (int)Math.Round(Height * sy);

        w = Math.Clamp(w, 2, monitor.Bounds.Width);
        h = Math.Clamp(h, 2, monitor.Bounds.Height);
        x = Math.Clamp(x, 0, monitor.Bounds.Width - w);
        y = Math.Clamp(y, 0, monitor.Bounds.Height - h);
        return new PixelRect(monitor.Bounds.Left + x, monitor.Bounds.Top + y, w, h);
    }

    public override string ToString() => $"{Width} × {Height} at ({X}, {Y})";
}

/// <summary>Integer rectangle in physical pixels.</summary>
public readonly record struct PixelRect(int Left, int Top, int Width, int Height)
{
    public int Right => Left + Width;
    public int Bottom => Top + Height;
    public bool Contains(int x, int y) => x >= Left && x < Right && y >= Top && y < Bottom;
}
