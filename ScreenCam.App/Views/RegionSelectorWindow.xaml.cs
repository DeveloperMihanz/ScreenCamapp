using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;
using System.Windows.Interop;
using System.Windows.Media;
using System.Windows.Shapes;
using ScreenCam.App.Models;
using ScreenCam.App.Services;

namespace ScreenCam.App.Views;

/// <summary>
/// Transparent full-screen overlay covering ONE monitor. One instance is shown
/// per monitor (see <see cref="RegionSelector"/>) so every overlay has a single,
/// well-defined DPI. The selection is kept in physical pixels relative to the
/// monitor and converted to/from WPF device-independent units for drawing.
/// </summary>
public partial class RegionSelectorWindow : Window
{
    private enum DragMode { None, Create, Move, N, S, E, W, NE, NW, SE, SW }

    private const double HandleSizeDip = 10;
    private const double HitToleranceDip = 8;
    private const int MinSizePx = 16;

    private readonly double _aspect; // camera width / height, used with Shift
    private readonly Rectangle[] _handles = new Rectangle[8];

    // Selection in physical pixels relative to the monitor (left, top, right, bottom).
    private bool _hasSelection;
    private double _l, _t, _r, _b;

    private DragMode _mode = DragMode.None;
    private Point _dragStartPx;
    private (double L, double T, double R, double B) _dragStartSel;
    private bool _completed;

    public MonitorInfo Monitor { get; }

    /// <summary>Raised once with the chosen region, or null when cancelled.</summary>
    public event EventHandler<ScreenRegion?>? Completed;

    /// <summary>Raised when the user starts a new selection on this monitor.</summary>
    public event EventHandler? SelectionStarted;

    public RegionSelectorWindow(MonitorInfo monitor, PixelRect? initialSelection, double aspect)
    {
        InitializeComponent();
        Monitor = monitor;
        _aspect = aspect > 0 ? aspect : 16.0 / 9.0;
        MonitorText.Text = monitor.DisplayName;

        // Provisional placement in DIPs; corrected to exact physical pixels once the HWND exists.
        Left = monitor.Bounds.Left / monitor.Scale;
        Top = monitor.Bounds.Top / monitor.Scale;
        Width = monitor.Bounds.Width / monitor.Scale;
        Height = monitor.Bounds.Height / monitor.Scale;

        for (int i = 0; i < _handles.Length; i++)
        {
            _handles[i] = new Rectangle
            {
                Width = HandleSizeDip,
                Height = HandleSizeDip,
                Fill = Brushes.White,
                Stroke = new SolidColorBrush(Color.FromRgb(0x3B, 0x82, 0xF6)),
                StrokeThickness = 2,
                IsHitTestVisible = false,
                Visibility = Visibility.Collapsed,
            };
            Surface.Children.Add(_handles[i]);
        }

        if (initialSelection is { } s && s.Width >= MinSizePx && s.Height >= MinSizePx)
            SetSelection(s.Left, s.Top, s.Right, s.Bottom);

        SourceInitialized += (_, _) => PlaceOnMonitor();
        Loaded += (_, _) =>
        {
            PlaceOnMonitor();
            UpdateVisuals();
            Surface.Focus();
        };
        DpiChanged += (_, _) => Dispatcher.BeginInvoke(() =>
        {
            PlaceOnMonitor();
            UpdateVisuals();
        });
        SizeChanged += (_, _) => UpdateVisuals();
        PreviewKeyDown += OnPreviewKeyDown;
        Closing += (_, _) => Complete(null);
    }

    // ------------------------------------------------------------------
    // Placement / coordinate conversion
    // ------------------------------------------------------------------
    private void PlaceOnMonitor()
    {
        var hwnd = new WindowInteropHelper(this).Handle;
        if (hwnd == IntPtr.Zero)
            return;
        var b = Monitor.Bounds;
        Win32.SetWindowPos(hwnd, Win32.HWND_TOPMOST, b.Left, b.Top, b.Width, b.Height, Win32.SWP_NOACTIVATE);
    }

    private DpiScale Dpi => VisualTreeHelper.GetDpi(this);

    private Point ToPx(Point dip) => new(dip.X * Dpi.DpiScaleX, dip.Y * Dpi.DpiScaleY);

    private Rect ToDip(double l, double t, double r, double b) =>
        new(l / Dpi.DpiScaleX, t / Dpi.DpiScaleY, Math.Max(0, (r - l) / Dpi.DpiScaleX), Math.Max(0, (b - t) / Dpi.DpiScaleY));

    private double MonitorW => Monitor.Bounds.Width;
    private double MonitorH => Monitor.Bounds.Height;

    // ------------------------------------------------------------------
    // Public API used by RegionSelector
    // ------------------------------------------------------------------
    public void ClearSelection()
    {
        _hasSelection = false;
        _mode = DragMode.None;
        UpdateVisuals();
    }

    private void SetSelection(double l, double t, double r, double b)
    {
        _l = Math.Clamp(l, 0, MonitorW);
        _t = Math.Clamp(t, 0, MonitorH);
        _r = Math.Clamp(r, 0, MonitorW);
        _b = Math.Clamp(b, 0, MonitorH);
        _hasSelection = true;
    }

    private ScreenRegion? BuildRegion()
    {
        if (!_hasSelection)
            return null;
        int x = (int)Math.Round(_l), y = (int)Math.Round(_t);
        int w = (int)Math.Round(_r - _l) & ~1; // even sizes suit the NV12 pipeline
        int h = (int)Math.Round(_b - _t) & ~1;
        if (w < MinSizePx || h < MinSizePx)
            return null;
        return new ScreenRegion
        {
            MonitorDeviceName = Monitor.DeviceName,
            MonitorLeft = Monitor.Bounds.Left,
            MonitorTop = Monitor.Bounds.Top,
            MonitorWidth = Monitor.Bounds.Width,
            MonitorHeight = Monitor.Bounds.Height,
            X = x,
            Y = y,
            Width = w,
            Height = h,
        };
    }

    private void Complete(ScreenRegion? region)
    {
        if (_completed)
            return;
        _completed = true;
        Completed?.Invoke(this, region);
    }

    // ------------------------------------------------------------------
    // Mouse interaction
    // ------------------------------------------------------------------
    private DragMode HitTest(Point px)
    {
        if (!_hasSelection)
            return DragMode.Create;
        double tol = HitToleranceDip * Dpi.DpiScaleX;
        bool nearL = Math.Abs(px.X - _l) <= tol, nearR = Math.Abs(px.X - _r) <= tol;
        bool nearT = Math.Abs(px.Y - _t) <= tol, nearB = Math.Abs(px.Y - _b) <= tol;
        bool withinX = px.X >= _l - tol && px.X <= _r + tol;
        bool withinY = px.Y >= _t - tol && px.Y <= _b + tol;

        if (nearL && nearT) return DragMode.NW;
        if (nearR && nearT) return DragMode.NE;
        if (nearL && nearB) return DragMode.SW;
        if (nearR && nearB) return DragMode.SE;
        if (nearT && withinX) return DragMode.N;
        if (nearB && withinX) return DragMode.S;
        if (nearL && withinY) return DragMode.W;
        if (nearR && withinY) return DragMode.E;
        if (px.X > _l && px.X < _r && px.Y > _t && px.Y < _b) return DragMode.Move;
        return DragMode.Create;
    }

    private static Cursor CursorFor(DragMode mode) => mode switch
    {
        DragMode.Move => Cursors.SizeAll,
        DragMode.N or DragMode.S => Cursors.SizeNS,
        DragMode.E or DragMode.W => Cursors.SizeWE,
        DragMode.NE or DragMode.SW => Cursors.SizeNESW,
        DragMode.NW or DragMode.SE => Cursors.SizeNWSE,
        _ => Cursors.Cross,
    };

    private void Surface_MouseLeftButtonDown(object sender, MouseButtonEventArgs e)
    {
        var px = ToPx(e.GetPosition(Surface));
        var mode = HitTest(px);

        if (e.ClickCount == 2 && mode == DragMode.Move)
        {
            Confirm();
            return;
        }

        _mode = mode;
        _dragStartPx = new Point(Math.Clamp(px.X, 0, MonitorW), Math.Clamp(px.Y, 0, MonitorH));
        if (mode == DragMode.Create)
        {
            SetSelection(_dragStartPx.X, _dragStartPx.Y, _dragStartPx.X, _dragStartPx.Y);
            SelectionStarted?.Invoke(this, EventArgs.Empty);
        }
        _dragStartSel = (_l, _t, _r, _b);
        Surface.CaptureMouse();
        Surface.Focus();
        UpdateVisuals();
        e.Handled = true;
    }

    private void Surface_MouseMove(object sender, MouseEventArgs e)
    {
        var px = ToPx(e.GetPosition(Surface));
        if (_mode == DragMode.None)
        {
            Surface.Cursor = CursorFor(HitTest(px));
            return;
        }

        bool keepAspect = (Keyboard.Modifiers & ModifierKeys.Shift) != 0;
        double cx = Math.Clamp(px.X, 0, MonitorW);
        double cy = Math.Clamp(px.Y, 0, MonitorH);
        var (sl, st, sr, sb) = _dragStartSel;

        switch (_mode)
        {
            case DragMode.Create:
            {
                double x0 = _dragStartPx.X, y0 = _dragStartPx.Y;
                if (keepAspect)
                {
                    double w = Math.Abs(cx - x0);
                    double h = w / _aspect;
                    cy = y0 + (cy >= y0 ? h : -h);
                    if (cy < 0 || cy > MonitorH)
                    {
                        cy = Math.Clamp(cy, 0, MonitorH);
                        double hh = Math.Abs(cy - y0);
                        cx = x0 + (cx >= x0 ? hh * _aspect : -hh * _aspect);
                    }
                }
                SetSelection(Math.Min(x0, cx), Math.Min(y0, cy), Math.Max(x0, cx), Math.Max(y0, cy));
                break;
            }
            case DragMode.Move:
            {
                double w = sr - sl, h = sb - st;
                double nl = Math.Clamp(sl + (px.X - _dragStartPx.X), 0, MonitorW - w);
                double nt = Math.Clamp(st + (px.Y - _dragStartPx.Y), 0, MonitorH - h);
                SetSelection(nl, nt, nl + w, nt + h);
                break;
            }
            default:
                ResizeTo(cx, cy, keepAspect, sl, st, sr, sb);
                break;
        }
        UpdateVisuals();
    }

    private void ResizeTo(double cx, double cy, bool keepAspect, double l, double t, double r, double b)
    {
        bool left = _mode is DragMode.W or DragMode.NW or DragMode.SW;
        bool right = _mode is DragMode.E or DragMode.NE or DragMode.SE;
        bool top = _mode is DragMode.N or DragMode.NW or DragMode.NE;
        bool bottom = _mode is DragMode.S or DragMode.SW or DragMode.SE;

        if (left) l = Math.Min(cx, r - MinSizePx);
        if (right) r = Math.Max(cx, l + MinSizePx);
        if (top) t = Math.Min(cy, b - MinSizePx);
        if (bottom) b = Math.Max(cy, t + MinSizePx);

        // Corner drag with Shift: derive the height from the width.
        if (keepAspect && (left || right) && (top || bottom))
        {
            double h = (r - l) / _aspect;
            if (top) t = b - h; else b = t + h;
            if (t < 0 || b > MonitorH)
            {
                t = Math.Max(t, 0);
                b = Math.Min(b, MonitorH);
                double w = (b - t) * _aspect;
                if (left) l = r - w; else r = l + w;
            }
        }
        SetSelection(l, t, r, b);
    }

    private void Surface_MouseLeftButtonUp(object sender, MouseButtonEventArgs e)
    {
        if (_mode == DragMode.None)
            return;
        Surface.ReleaseMouseCapture();
        if (_mode == DragMode.Create && (_r - _l < MinSizePx || _b - _t < MinSizePx))
            _hasSelection = false; // a click, not a drag
        _mode = DragMode.None;
        UpdateVisuals();
        e.Handled = true;
    }

    // ------------------------------------------------------------------
    // Keyboard and buttons
    // ------------------------------------------------------------------
    private void OnPreviewKeyDown(object sender, KeyEventArgs e)
    {
        switch (e.Key)
        {
            case Key.Escape:
                Complete(null);
                e.Handled = true;
                break;
            case Key.Enter:
                Confirm();
                e.Handled = true;
                break;
            case Key.Left or Key.Right or Key.Up or Key.Down when _hasSelection:
            {
                double step = (Keyboard.Modifiers & ModifierKeys.Shift) != 0 ? 10 : 1;
                double dx = e.Key == Key.Left ? -step : e.Key == Key.Right ? step : 0;
                double dy = e.Key == Key.Up ? -step : e.Key == Key.Down ? step : 0;
                double w = _r - _l, h = _b - _t;
                double nl = Math.Clamp(_l + dx, 0, MonitorW - w);
                double nt = Math.Clamp(_t + dy, 0, MonitorH - h);
                SetSelection(nl, nt, nl + w, nt + h);
                UpdateVisuals();
                e.Handled = true;
                break;
            }
        }
    }

    private void Confirm()
    {
        var region = BuildRegion();
        if (region is not null)
            Complete(region);
    }

    private void Confirm_Click(object sender, RoutedEventArgs e) => Confirm();

    private void Cancel_Click(object sender, RoutedEventArgs e) => Complete(null);

    private void WholeDisplay_Click(object sender, RoutedEventArgs e)
    {
        SelectionStarted?.Invoke(this, EventArgs.Empty);
        SetSelection(0, 0, MonitorW, MonitorH);
        UpdateVisuals();
    }

    // ------------------------------------------------------------------
    // Rendering
    // ------------------------------------------------------------------
    private void UpdateVisuals()
    {
        double width = ActualWidth > 0 ? ActualWidth : Width;
        double height = ActualHeight > 0 ? ActualHeight : Height;
        DimOuter.Rect = new Rect(0, 0, width, height);
        Hint.Visibility = _hasSelection ? Visibility.Collapsed : Visibility.Visible;

        if (!_hasSelection)
        {
            DimHole.Rect = Rect.Empty;
            SelectionBorder.Visibility = Visibility.Collapsed;
            SizeLabel.Visibility = Visibility.Collapsed;
            Toolbar.Visibility = Visibility.Collapsed;
            foreach (var hnd in _handles)
                hnd.Visibility = Visibility.Collapsed;
            return;
        }

        var sel = ToDip(_l, _t, _r, _b);
        DimHole.Rect = sel;

        SelectionBorder.Visibility = Visibility.Visible;
        Canvas.SetLeft(SelectionBorder, sel.Left - 1);
        Canvas.SetTop(SelectionBorder, sel.Top - 1);
        SelectionBorder.Width = sel.Width + 2;
        SelectionBorder.Height = sel.Height + 2;

        // Handles: NW, N, NE, E, SE, S, SW, W
        var points = new[]
        {
            new Point(sel.Left, sel.Top), new Point(sel.Left + sel.Width / 2, sel.Top), new Point(sel.Right, sel.Top),
            new Point(sel.Right, sel.Top + sel.Height / 2), new Point(sel.Right, sel.Bottom),
            new Point(sel.Left + sel.Width / 2, sel.Bottom), new Point(sel.Left, sel.Bottom),
            new Point(sel.Left, sel.Top + sel.Height / 2),
        };
        bool showHandles = _mode != DragMode.Create;
        for (int i = 0; i < _handles.Length; i++)
        {
            _handles[i].Visibility = showHandles ? Visibility.Visible : Visibility.Collapsed;
            Canvas.SetLeft(_handles[i], points[i].X - HandleSizeDip / 2);
            Canvas.SetTop(_handles[i], points[i].Y - HandleSizeDip / 2);
        }

        int wPx = (int)Math.Round(_r - _l) & ~1, hPx = (int)Math.Round(_b - _t) & ~1;
        SizeText.Text = $"{wPx} × {hPx} px";
        SizeLabel.Visibility = Visibility.Visible;
        SizeLabel.Measure(new Size(double.PositiveInfinity, double.PositiveInfinity));
        double labelTop = sel.Top - SizeLabel.DesiredSize.Height - 6;
        Canvas.SetLeft(SizeLabel, sel.Left);
        Canvas.SetTop(SizeLabel, labelTop >= 0 ? labelTop : sel.Top + 6);

        bool showToolbar = _mode == DragMode.None && wPx >= MinSizePx && hPx >= MinSizePx;
        Toolbar.Visibility = showToolbar ? Visibility.Visible : Visibility.Collapsed;
        if (showToolbar)
        {
            Toolbar.Measure(new Size(double.PositiveInfinity, double.PositiveInfinity));
            var ts = Toolbar.DesiredSize;
            double x = Math.Clamp(sel.Right - ts.Width, 4, Math.Max(4, width - ts.Width - 4));
            double y = sel.Bottom + 8;
            if (y + ts.Height > height - 4)
                y = sel.Top - ts.Height - 8;           // above the selection
            if (y < 4)
                y = sel.Bottom - ts.Height - 8;        // inside, bottom edge
            Canvas.SetLeft(Toolbar, x);
            Canvas.SetTop(Toolbar, Math.Max(4, y));
        }
    }
}
