using ScreenCam.App.Models;

namespace ScreenCam.App.Services;

/// <summary>Enumerates displays in physical pixels (the process is per-monitor-v2 DPI aware).</summary>
public static class MonitorService
{
    public static IReadOnlyList<MonitorInfo> GetMonitors()
    {
        var handles = new List<IntPtr>();
        Win32.EnumDisplayMonitors(IntPtr.Zero, IntPtr.Zero, (IntPtr h, IntPtr hdc, ref Win32.RECT rect, IntPtr data) =>
        {
            handles.Add(h);
            return true;
        }, IntPtr.Zero);

        var raw = new List<(IntPtr Handle, Win32.MONITORINFOEX Info, uint Dpi)>();
        foreach (var h in handles)
        {
            var info = new Win32.MONITORINFOEX { cbSize = System.Runtime.InteropServices.Marshal.SizeOf<Win32.MONITORINFOEX>() };
            if (!Win32.GetMonitorInfo(h, ref info))
                continue;
            uint dpi = 96;
            try
            {
                if (Win32.GetDpiForMonitor(h, 0 /* MDT_EFFECTIVE_DPI */, out var dx, out _) == 0 && dx > 0)
                    dpi = dx;
            }
            catch (DllNotFoundException)
            {
            }
            raw.Add((h, info, dpi));
        }

        // Stable ordering: primary first, then left-to-right, top-to-bottom.
        var ordered = raw
            .OrderByDescending(m => (m.Info.dwFlags & Win32.MONITORINFOF_PRIMARY) != 0)
            .ThenBy(m => m.Info.rcMonitor.Left)
            .ThenBy(m => m.Info.rcMonitor.Top)
            .ToList();

        var result = new List<MonitorInfo>(ordered.Count);
        for (int i = 0; i < ordered.Count; i++)
        {
            var (h, info, dpi) = ordered[i];
            result.Add(new MonitorInfo
            {
                Handle = h,
                DeviceName = info.szDevice ?? string.Empty,
                Bounds = ToRect(info.rcMonitor),
                WorkArea = ToRect(info.rcWork),
                IsPrimary = (info.dwFlags & Win32.MONITORINFOF_PRIMARY) != 0,
                Index = i,
                DpiX = dpi,
            });
        }
        return result;
    }

    /// <summary>Finds the monitor a saved region belongs to: by device name, then by matching origin.</summary>
    public static MonitorInfo? FindForRegion(ScreenRegion region, IReadOnlyList<MonitorInfo> monitors)
    {
        return monitors.FirstOrDefault(m => string.Equals(m.DeviceName, region.MonitorDeviceName, StringComparison.OrdinalIgnoreCase))
            ?? monitors.FirstOrDefault(m => m.Bounds.Left == region.MonitorLeft && m.Bounds.Top == region.MonitorTop);
    }

    public static MonitorInfo? Primary(IReadOnlyList<MonitorInfo> monitors) =>
        monitors.FirstOrDefault(m => m.IsPrimary) ?? monitors.FirstOrDefault();

    private static PixelRect ToRect(Win32.RECT r) => new(r.Left, r.Top, r.Right - r.Left, r.Bottom - r.Top);
}
