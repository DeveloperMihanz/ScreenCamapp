using System.IO;
using System.Text;

namespace ScreenCam.App.Services;

/// <summary>Minimal thread-safe file logger: %LOCALAPPDATA%\ScreenCam\logs\screencam.log (rotated at 1 MB).</summary>
public static class Logger
{
    private static readonly object Sync = new();
    private const long MaxBytes = 1024 * 1024;

    public static string Directory { get; } =
        Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "ScreenCam", "logs");

    public static string FilePath { get; } = Path.Combine(Directory, "screencam.log");

    public static void Info(string message) => Write("INFO ", message);

    public static void Warn(string message) => Write("WARN ", message);

    public static void Error(string message, Exception? ex = null) =>
        Write("ERROR", ex is null ? message : $"{message}: {ex}");

    private static void Write(string level, string message)
    {
        var line = $"{DateTime.Now:yyyy-MM-dd HH:mm:ss.fff} [{level}] {message}{Environment.NewLine}";
        System.Diagnostics.Debug.Write(line);
        lock (Sync)
        {
            try
            {
                System.IO.Directory.CreateDirectory(Directory);
                var info = new FileInfo(FilePath);
                if (info.Exists && info.Length > MaxBytes)
                    File.Move(FilePath, FilePath + ".1", overwrite: true);
                File.AppendAllText(FilePath, line, Encoding.UTF8);
            }
            catch
            {
                // Logging must never crash the app.
            }
        }
    }
}
