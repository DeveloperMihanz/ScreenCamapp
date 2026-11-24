using System.Diagnostics;
using System.Threading;
using System.Windows;
using System.Windows.Threading;
using ScreenCam.App.Services;

namespace ScreenCam.App;

public partial class App : Application
{
    private const string SingleInstanceMutexName = "Local\\ScreenCamVirtualCamera.SingleInstance";
    private Mutex? _singleInstanceMutex;

    protected override void OnStartup(StartupEventArgs e)
    {
        base.OnStartup(e);

        // One instance per user session: a second instance would create a second
        // capture session and fight over the virtual camera.
        _singleInstanceMutex = new Mutex(initiallyOwned: true, SingleInstanceMutexName, out bool createdNew);
        if (!createdNew)
        {
            ActivateExistingInstance();
            _singleInstanceMutex.Dispose();
            _singleInstanceMutex = null;
            Shutdown(0);
            return;
        }

        DispatcherUnhandledException += OnDispatcherUnhandledException;
        AppDomain.CurrentDomain.UnhandledException += (_, args) =>
            Logger.Error("Unhandled exception", args.ExceptionObject as Exception);
        TaskScheduler.UnobservedTaskException += (_, args) =>
        {
            Logger.Error("Unobserved task exception", args.Exception);
            args.SetObserved();
        };

        Logger.Info($"ScreenCam {typeof(App).Assembly.GetName().Version} starting on {Environment.OSVersion}");

        var window = new MainWindow();
        MainWindow = window;
        window.Show();
    }

    protected override void OnExit(ExitEventArgs e)
    {
        Logger.Info("ScreenCam exiting");
        if (_singleInstanceMutex is not null)
        {
            try
            {
                _singleInstanceMutex.ReleaseMutex();
            }
            catch (ApplicationException)
            {
                // not owned by this thread
            }
            _singleInstanceMutex.Dispose();
        }
        base.OnExit(e);
    }

    private static void ActivateExistingInstance()
    {
        try
        {
            var current = Process.GetCurrentProcess();
            foreach (var p in Process.GetProcessesByName(current.ProcessName))
            {
                if (p.Id == current.Id || p.MainWindowHandle == IntPtr.Zero)
                    continue;
                if (Win32.IsIconic(p.MainWindowHandle))
                    Win32.ShowWindow(p.MainWindowHandle, Win32.SW_RESTORE);
                Win32.SetForegroundWindow(p.MainWindowHandle);
                return;
            }
        }
        catch
        {
            // ignore
        }
        MessageBox.Show("ScreenCam Virtual Camera is already running.", "ScreenCam",
            MessageBoxButton.OK, MessageBoxImage.Information);
    }

    private void OnDispatcherUnhandledException(object sender, DispatcherUnhandledExceptionEventArgs e)
    {
        Logger.Error("Unhandled UI exception", e.Exception);
        MessageBox.Show($"An unexpected error occurred:\n\n{e.Exception.Message}\n\nDetails were written to:\n{Logger.FilePath}",
            "ScreenCam", MessageBoxButton.OK, MessageBoxImage.Error);
        e.Handled = true;
    }
}
