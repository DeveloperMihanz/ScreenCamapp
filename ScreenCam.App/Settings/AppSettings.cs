using ScreenCam.App.Models;

namespace ScreenCam.App.Settings;

/// <summary>User settings persisted to %APPDATA%\ScreenCam\settings.json.</summary>
public sealed class AppSettings
{
    public const int CurrentVersion = 1;

    public int Version { get; set; } = CurrentVersion;

    // Capture
    public ScreenRegion? Region { get; set; }
    public int OutputWidth { get; set; } = 1280;
    public int OutputHeight { get; set; } = 720;
    public int FrameRate { get; set; } = 30;
    public ScaleMode ScaleMode { get; set; } = ScaleMode.Fit;
    public bool ShowCursor { get; set; } = true;
    public bool HideCaptureBorder { get; set; } = true;

    // Preview
    public bool PreviewEnabled { get; set; } = true;
    public int PreviewMaxWidth { get; set; } = 640;

    // Behaviour
    /// <summary>Start the virtual camera when ScreenCam starts. The UI only shows
    /// "active" after the native camera actually started.</summary>
    public bool AutoStartCamera { get; set; }

    // Window placement
    public double? WindowLeft { get; set; }
    public double? WindowTop { get; set; }
    public double? WindowWidth { get; set; }
    public double? WindowHeight { get; set; }

    public AppSettings Clone()
    {
        var copy = (AppSettings)MemberwiseClone();
        copy.Region = Region?.Clone();
        return copy;
    }

    /// <summary>Repairs out-of-range values (e.g. from a hand-edited file).</summary>
    public void Normalize()
    {
        if (!ResolutionOption.All.Any(r => r.Width == OutputWidth && r.Height == OutputHeight))
        {
            OutputWidth = 1280;
            OutputHeight = 720;
        }
        if (!FrameRateOption.All.Any(f => f.Fps == FrameRate))
            FrameRate = 30;
        if (!Enum.IsDefined(ScaleMode))
            ScaleMode = ScaleMode.Fit;
        PreviewMaxWidth = Math.Clamp(PreviewMaxWidth, 160, 1280);
        if (Region is { IsValid: false })
            Region = null;
        Version = CurrentVersion;
    }
}
