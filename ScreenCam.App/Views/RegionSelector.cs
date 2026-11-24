using System.Windows;
using ScreenCam.App.Models;
using ScreenCam.App.Services;

namespace ScreenCam.App.Views;

/// <summary>
/// Shows one <see cref="RegionSelectorWindow"/> per monitor and returns the
/// region the user confirmed (or null if cancelled). A region always belongs to
/// a single monitor, which matches how Windows Graphics Capture works.
/// </summary>
public static class RegionSelector
{
    public static Task<ScreenRegion?> SelectAsync(ScreenRegion? current, double cameraAspect)
    {
        var monitors = MonitorService.GetMonitors();
        var tcs = new TaskCompletionSource<ScreenRegion?>();
        if (monitors.Count == 0)
        {
            tcs.SetResult(null);
            return tcs.Task;
        }

        var windows = new List<RegionSelectorWindow>();
        bool done = false;

        void Finish(ScreenRegion? result)
        {
            if (done)
                return;
            done = true;
            foreach (var w in windows.ToArray())
            {
                try
                {
                    w.Close();
                }
                catch (InvalidOperationException)
                {
                    // already closing
                }
            }
            tcs.TrySetResult(result);
        }

        var currentMonitor = current is null ? null : MonitorService.FindForRegion(current, monitors);
        foreach (var monitor in monitors)
        {
            PixelRect? initial = null;
            if (current is not null && currentMonitor is not null && ReferenceEquals(monitor, currentMonitor))
            {
                var abs = current.ResolveOn(monitor);
                initial = new PixelRect(abs.Left - monitor.Bounds.Left, abs.Top - monitor.Bounds.Top, abs.Width, abs.Height);
            }

            var window = new RegionSelectorWindow(monitor, initial, cameraAspect);
            window.Completed += (_, region) => Finish(region);
            window.SelectionStarted += (sender, _) =>
            {
                foreach (var other in windows)
                {
                    if (!ReferenceEquals(other, sender))
                        other.ClearSelection();
                }
            };
            windows.Add(window);
        }

        foreach (var w in windows)
            w.Show();

        // Give keyboard focus to the overlay under the mouse pointer.
        RegionSelectorWindow? target = null;
        if (Win32.GetCursorPos(out var pt))
            target = windows.FirstOrDefault(w => w.Monitor.Bounds.Contains(pt.X, pt.Y));
        (target ?? windows[0]).Activate();

        return tcs.Task;
    }
}
