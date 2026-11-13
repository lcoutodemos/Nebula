using Microsoft.UI.Xaml;
using Microsoft.UI.Windowing;
using System.Runtime.InteropServices;
using Windows.Graphics;
using Windows.UI.ViewManagement;

namespace Nebula_Windows;

/// <summary>
/// The application window: a frame hosting <see cref="MainPage"/>, with content extended into the
/// title bar and an initial size scaled for the window's DPI.
/// </summary>
public sealed partial class MainWindow : Window
{
    [DllImport("user32.dll")]
    private static extern uint GetDpiForWindow(nint hwnd);

    public MainWindow()
    {
        InitializeComponent();

        // The page's toolbar row is the caption strip (a unified toolbar). The page calls
        // SetTitleBar for its drag region and marks its capsules as pass-through.
        ExtendsContentIntoTitleBar = true;
        AppWindow.TitleBar.PreferredHeightOption = TitleBarHeightOption.Tall;
        AppWindow.TitleBar.ButtonBackgroundColor = Microsoft.UI.Colors.Transparent;
        AppWindow.TitleBar.ButtonInactiveBackgroundColor = Microsoft.UI.Colors.Transparent;

        AppWindow.SetIcon("Assets/AppIcon.ico");

        // Navigate the root frame to the main page on startup.
        RootFrame.Navigate(typeof(MainPage));
        var scale = GetDpiForWindow(WinRT.Interop.WindowNative.GetWindowHandle(this)) / 96.0;
        AppWindow.Resize(new SizeInt32((int)(1560 * scale), (int)(980 * scale)));
        if (AppWindow.Presenter is OverlappedPresenter presenter)
        {
            presenter.PreferredMinimumWidth = (int)(860 * scale);
            presenter.PreferredMinimumHeight = (int)(620 * scale);
        }
        ((FrameworkElement)Content).ActualThemeChanged += (_, _) => UpdateCaptionButtons();
        UpdateCaptionButtons();
    }

    private void UpdateCaptionButtons()
    {
        var titleBar = AppWindow.TitleBar;
        if (new AccessibilitySettings().HighContrast)
        {
            titleBar.ButtonForegroundColor = null;
            titleBar.ButtonHoverForegroundColor = null;
            titleBar.ButtonHoverBackgroundColor = null;
            return;
        }
        var foreground = ((FrameworkElement)Content).ActualTheme == ElementTheme.Dark
            ? Microsoft.UI.Colors.White : Microsoft.UI.Colors.Black;
        titleBar.ButtonForegroundColor = foreground;
        titleBar.ButtonHoverForegroundColor = foreground;
        titleBar.ButtonHoverBackgroundColor = Windows.UI.Color.FromArgb(24, foreground.R, foreground.G, foreground.B);
    }
}
