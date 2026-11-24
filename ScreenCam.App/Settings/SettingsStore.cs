using System.IO;
using System.Text.Json;
using System.Text.Json.Serialization;
using ScreenCam.App.Services;

namespace ScreenCam.App.Settings;

/// <summary>
/// Loads and saves <see cref="AppSettings"/> as JSON in the user's roaming
/// AppData folder, which survives application restarts, Windows restarts and
/// application updates (the install folder is never used for user data).
/// </summary>
public static class SettingsStore
{
    private static readonly JsonSerializerOptions JsonOptions = new()
    {
        WriteIndented = true,
        Converters = { new JsonStringEnumConverter() },
        DefaultIgnoreCondition = JsonIgnoreCondition.WhenWritingNull,
    };

    public static string Directory { get; } =
        Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData), "ScreenCam");

    public static string FilePath { get; } = Path.Combine(Directory, "settings.json");

    public static AppSettings Load()
    {
        try
        {
            if (File.Exists(FilePath))
            {
                var json = File.ReadAllText(FilePath);
                var settings = JsonSerializer.Deserialize<AppSettings>(json, JsonOptions) ?? new AppSettings();
                settings.Normalize();
                Logger.Info($"Settings loaded from {FilePath}");
                return settings;
            }
        }
        catch (Exception ex)
        {
            Logger.Error("Settings file is unreadable; using defaults", ex);
            try
            {
                File.Copy(FilePath, FilePath + ".corrupt", overwrite: true);
            }
            catch
            {
                // best effort
            }
        }
        var defaults = new AppSettings();
        defaults.Normalize();
        return defaults;
    }

    public static void Save(AppSettings settings)
    {
        try
        {
            System.IO.Directory.CreateDirectory(Directory);
            var tmp = FilePath + ".tmp";
            File.WriteAllText(tmp, JsonSerializer.Serialize(settings, JsonOptions));
            File.Move(tmp, FilePath, overwrite: true); // atomic replace on NTFS
        }
        catch (Exception ex)
        {
            Logger.Error("Could not save settings", ex);
        }
    }
}
