namespace ScreenCam.App.Models;

public sealed record ResolutionOption(int Width, int Height, string Label)
{
    public static readonly IReadOnlyList<ResolutionOption> All = new[]
    {
        new ResolutionOption(640, 480, "640 × 480 (4:3)"),
        new ResolutionOption(1280, 720, "1280 × 720 (HD 720p)"),
        new ResolutionOption(1920, 1080, "1920 × 1080 (Full HD 1080p)"),
    };

    public override string ToString() => Label;
}

public sealed record FrameRateOption(int Fps, string Label)
{
    public static readonly IReadOnlyList<FrameRateOption> All = new[]
    {
        new FrameRateOption(30, "30 fps"),
        new FrameRateOption(60, "60 fps"),
    };

    public override string ToString() => Label;
}

/// <summary>How the selected area is mapped onto the camera frame (matches the native enum).</summary>
public enum ScaleMode
{
    /// <summary>Keep aspect ratio, add black bars.</summary>
    Fit = 0,
    /// <summary>Fill the frame, distorting the aspect ratio.</summary>
    Stretch = 1,
    /// <summary>Keep aspect ratio, crop to fill the frame.</summary>
    Fill = 2,
}

public enum CameraState
{
    Unsupported,
    Stopped,
    Starting,
    Active,
    Error,
}
