using Microsoft.UI.Dispatching;
using Microsoft.UI.Input;
using Microsoft.UI.Windowing;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Automation;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Controls.Primitives;
using Microsoft.UI.Xaml.Input;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Media.Animation;
using Microsoft.UI.Xaml.Media.Imaging;
using Nebula.Core;
using Nebula_Windows.Services;
using Nebula_Windows.ViewModels;
using System.Diagnostics;
using System.Numerics;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using Windows.Foundation;
using Windows.Storage.Pickers;
using Windows.System;
using Windows.UI.ViewManagement;
using DispatcherQueueTimer = Microsoft.UI.Dispatching.DispatcherQueueTimer;
using DispatcherQueuePriority = Microsoft.UI.Dispatching.DispatcherQueuePriority;
using Layout = Nebula.Core.Layout;

namespace Nebula_Windows;

public sealed partial class MainPage : Page
{
    public MainPageViewModel ViewModel { get; } = new();
    private readonly LibraryService _libraries = new();
    private readonly UiState _preferences = UiState.Load();
    private readonly UISettings _uiSettings = new();
    private Microsoft.UI.System.ThemeSettings? _themeSettings;
    private bool IsHighContrast => _themeSettings?.HighContrast == true;
    // Reference identity matters: a rescan/preview may have the same source but different photo indices.
    // Weak keys let old libraries and their four cached layouts go away after an import or rescan.
    private readonly ConditionalWeakTable<Library, Dictionary<Arrangement, Task<Vector3[]>>> _positionCache = new();
    private NativeRenderer? _renderer;
    private RawTouchpad? _touchpad;
    private long _lastPreciseWheelTicks = long.MinValue / 2;
    private string _lastRendererError = "";
    private Library? _library;
    private Library? _pendingLibrary;
    private CancellationTokenSource? _import;
    private CancellationTokenSource? _previewCancellation;
    private readonly SemaphoreSlim _previewGate = new(1, 1);
    private SoftwareBitmapSource? _previewSource;
    private string? _previewAttemptedId;
    private long _previewGeneration;
    private DispatcherQueueTimer? _timer, _searchTimer, _hoverTimer, _panelMotionTimer;
    private readonly Dictionary<Border, PanelMotion> _panelMotions = [];
    private bool _hasPanelLayout, _isApplyingPanelFrame, _isSubmittingGlass;
    private Arrangement _arrangement;
    private Arrangement _requestedArrangement;
    private long _libraryRequestGeneration, _modeRequestGeneration;
    private bool _isUpdatingModeControls;
    private ExploreFacet? _selectedFacet;
    private string _scope = "All";
    private string _overviewTitle = Strings.Get("Explore_Title"), _overviewDescription = Strings.Get("Explore_Description");
    private bool _isReady, _isLoaded, _isDragging, _isOrbitDrag, _hasMoved, _isLeftClick, _hasPreview;
    private bool _isExploreExpanded, _hasInspectorOpened, _isSynchronizingSelection;
    private bool _isGlassEnabled, _isGlassQueued, _isGlassForced;
    private int _hoverIndex = -1;
    private long _lastHoverTick;
    private Point _previous, _pressed;
    private NativeRenderer.GlassPanel[] _lastGlass = [];
    private byte[] _lastStructuralMask = [];
    private int[] _lastResultIndices = [];
    private Arrangement _lastResultArrangement;
    private int _resultLayoutGeneration;
    private bool _hasSearchResults;
    private Point _hoverPoint;
    private Windows.Graphics.RectInt32[] _lastPassthrough = [];
    private bool _isTitleBarAttached;
    private readonly List<Fact> _facts = [];
    private int _factIndex;
    private string _overviewEyebrow = "";
    private double _exploreHeight;
    private int _wheelLogCount;
    private int _frameLogCount;

    private const double CardX = 10, CardTop = 8, CardWidth = 256, CardGap = 10;
    // Legacy wheel fallback only. DirectManipulation supplies real fractional DIPs, not wheel units.
    private const float OrbitPerWheelUnit = 0.6f;
    private const float OrbitPerTouchpadPoint = .0045f / .006f;

    private sealed record Fact(string Glyph, string Caption, string Value, string Body, ExploreFacet Facet, string[] Thumbs);

    private sealed class PanelMotion(Border panel, Rect layoutBounds, PanelFrame initial)
    {
        public Border Panel { get; } = panel;
        public Rect LayoutBounds { get; set; } = layoutBounds;
        public TranslateTransform Transform { get; } = new();
        public PanelTransition Transition { get; } = new(initial, false);
    }

    private static double MotionNow => Stopwatch.GetTimestamp() * 1000.0 / Stopwatch.Frequency;

    // Grounded in WinUser.h (10.0.28000). Never infer a touchpad from small wheel deltas.
    [StructLayout(LayoutKind.Sequential)]
    private struct InputMessageSource { public uint DeviceType, OriginId; }
    [DllImport("user32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool GetCurrentInputMessageSource(out InputMessageSource source);
    [DllImport("user32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool GetPointerType(uint pointerId, out uint type);

    public MainPage()
    {
        InitializeComponent();
        _isReady = true;
        Destinations.SelectedItem = AllDestination;
        SystemAppearance.IsChecked = _preferences.Appearance == "System";
        LightAppearance.IsChecked = _preferences.Appearance == "Light";
        DarkAppearance.IsChecked = _preferences.Appearance == "Dark";
    }

    public static bool Not(bool value) => !value;
    public static bool CanRescan(bool hasLibrary, bool isImporting) => hasLibrary && !isImporting;

    /// Small decode for list thumbnails: 40 logical px renders sharp at 34 DIP on any scale.
    public static ImageSource? Thumb(string path) => string.IsNullOrEmpty(path) ? null
        : new BitmapImage(new Uri(path)) { DecodePixelWidth = 40, DecodePixelType = DecodePixelType.Logical };

    private static string MuseumSample()
    {
        var root = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "Nebula", "TestData");
        foreach (var name in new[] { "met-open-access-5000", "met-commons-5000", "met-5000" })
        {
            var folder = Path.Combine(root, name);
            if (Directory.Exists(folder) && Directory.EnumerateFiles(folder, "*.jpg").Any()) return folder;
        }
        return "";
    }

    private async void OnOpenMuseum(object sender, RoutedEventArgs e)
    {
        var folder = MuseumSample();
        if (folder.Length > 0 && !ViewModel.IsImporting) await ImportAsync(folder);
    }

    private void AttachTitleBar()
    {
        if (_isTitleBarAttached || App.Window is null) return;
        _isTitleBarAttached = true;
        App.Window.SetTitleBar(TitleDragRegion);
        App.Window.AppWindow.TitleBar.PreferredHeightOption = TitleBarHeightOption.Tall;
    }

    /// The toolbar row doubles as the caption strip: its glass capsules must stay interactive.
    private void UpdateTitleBarRegions()
    {
        if (App.Window is null || XamlRoot is null) return;
        var scale = XamlRoot.RasterizationScale;
        var rects = new List<Windows.Graphics.RectInt32>();
        foreach (var element in new FrameworkElement[] { SearchChrome, CommandChrome, ToolsChrome, FilterChip, FloatToggle, SourcePanel })
        {
            if (element.Visibility != Visibility.Visible || element.ActualWidth <= 0 || !element.IsHitTestVisible) continue;
            var bounds = element.TransformToVisual(null).TransformBounds(new Rect(0, 0, element.ActualWidth, element.ActualHeight));
            if (bounds.Top > 60 || bounds.Right <= 0) continue;
            rects.Add(new((int)Math.Floor(bounds.X * scale), (int)Math.Floor(bounds.Y * scale),
                (int)Math.Ceiling(bounds.Width * scale), (int)Math.Ceiling(bounds.Height * scale)));
        }
        var next = rects.ToArray();
        if (next.SequenceEqual(_lastPassthrough)) return;
        _lastPassthrough = next;
        try { InputNonClientPointerSource.GetForWindowId(App.Window.AppWindow.Id).SetRegionRects(NonClientRegionKind.Passthrough, next); }
        catch (Exception error) { LibraryService.Log($"Title bar regions could not be updated: {error.Message}"); }
    }

    private async void OnLoaded(object sender, RoutedEventArgs e)
    {
        if (_isLoaded) return;
        _isLoaded = true;
        try
        {
            AttachTitleBar();
            WatchChromeHover(ToolbarControls().ToArray());
            EmptyMuseum.Visibility = MuseumSample().Length > 0 ? Visibility.Visible : Visibility.Collapsed;
            ApplyAppearance();
            Root.ActualThemeChanged += OnActualThemeChanged;
            _uiSettings.AdvancedEffectsEnabledChanged += OnEffectsChanged;
            if (OperatingSystem.IsWindowsVersionAtLeast(10, 0, 19041))
                _uiSettings.AnimationsEnabledChanged += OnEffectsChanged;
            _themeSettings = Microsoft.UI.System.ThemeSettings.CreateForWindowId(App.Window.AppWindow.Id);
            _themeSettings.Changed += OnThemeSettingsChanged;
            _renderer = new NativeRenderer(Canvas);
            ResizeCanvas();
            ApplyEffects();
            LayoutChrome();
            try
            {
                // Fingers right ⇒ like dragging right, at 0.0045 rad per scroll point.
                _touchpad = new RawTouchpad(App.WindowHandle, CanStartTouchpad,
                    (dx, dy) =>
                    {
                        ClearHover();
                        RenderAction(renderer => renderer.Orbit(dx * OrbitPerTouchpadPoint, dy * OrbitPerTouchpadPoint));
                    }, LibraryService.Log);
            }
            catch (Exception error)
            {
                // No precision touchpad listener: the wheel path below still orbits from touchpad scrolling.
                LibraryService.Log($"Touchpad: freeform orbit unavailable, wheel orbit only: {error.Message}");
            }
            _timer = DispatcherQueue.CreateTimer();
            _timer.Interval = TimeSpan.FromSeconds(1);
            _timer.Tick += OnStats;
            _timer.Start();
            _searchTimer = DispatcherQueue.CreateTimer();
            _searchTimer.Interval = TimeSpan.FromMilliseconds(280);
            _searchTimer.IsRepeating = false;
            _searchTimer.Tick += (_, _) => ApplyFilter();
            _hoverTimer = DispatcherQueue.CreateTimer();
            _hoverTimer.Interval = TimeSpan.FromMilliseconds(320);
            _hoverTimer.IsRepeating = false;
            _hoverTimer.Tick += (_, _) => ShowHoverTag();
            var generation = _libraryRequestGeneration;
            var previous = await _libraries.LastLibraryAsync();
            if (_isLoaded && !ViewModel.IsImporting && generation == _libraryRequestGeneration && previous is { Photos.Count: > 0 })
                await ShowLibraryAsync(previous);
        }
        catch (Exception error) { Fail(Strings.Get("Error_CollectionOpen"), error); }
    }

    private void OnUnloaded(object sender, RoutedEventArgs e)
    {
        _isLoaded = false;
        _touchpad?.Dispose();
        _touchpad = null;
        ResetInspectorPreview();
        _libraryRequestGeneration++;
        _modeRequestGeneration++;
        _pendingLibrary = null;
        _import?.Cancel();
        _timer?.Stop();
        _searchTimer?.Stop();
        _hoverTimer?.Stop();
        _panelMotionTimer?.Stop();
        foreach (var motion in _panelMotions.Values) motion.Transition.Complete();
        _hasPanelLayout = false;
        Root.ActualThemeChanged -= OnActualThemeChanged;
        _uiSettings.AdvancedEffectsEnabledChanged -= OnEffectsChanged;
        if (OperatingSystem.IsWindowsVersionAtLeast(10, 0, 19041))
            _uiSettings.AnimationsEnabledChanged -= OnEffectsChanged;
        if (_themeSettings is not null) _themeSettings.Changed -= OnThemeSettingsChanged;
        _themeSettings = null;
        _renderer?.Dispose();
        _renderer = null;
        _lastGlass = [];
        ResetSearchPresentation();
        ReleaseSemantic();
    }

    private void Fail(string context, Exception error)
    {
        LibraryService.Log($"{context} {error}");
        ViewModel.Error = $"{context} {error.Message}";
        ViewModel.HasError = true;
    }

    private void RenderAction(Action<NativeRenderer> action)
    {
        if (_renderer is null) return;
        try { action(_renderer); }
        catch (Exception error) { Fail(Strings.Get("Error_Renderer"), error); }
    }

    private void SavePreferences()
    {
        try { _preferences.Save(); }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException)
        { Fail(Strings.Get("Error_PreferencesSave"), error); }
    }

    private void OnStats(DispatcherQueueTimer sender, object args)
    {
        if (_renderer is null) return;
        try
        {
            var stats = _renderer.GetStats();
            var frame = _renderer.GetFrameDiagnostics();
            if (frame.Tiles + frame.Dots > 0 && frame.PrepareMs + frame.OrderMs + frame.UploadMs + frame.SubmitMs > 0 &&
                _frameLogCount++ < 120)
                LibraryService.Log($"Frame: prepare {frame.PrepareMs:F2} order {frame.OrderMs:F2} upload {frame.UploadMs:F2} " +
                    $"submit {frame.SubmitMs:F2} ms; tiles {frame.Tiles:N0} dots {frame.Dots:N0}; upload {frame.InstanceUploadBytes / 1024:N0} KB");
            ViewModel.RendererStatus = ViewModel.HasLibrary ? Strings.Format("Status_RendererReady", stats.Resident, stats.Items) : "";
            var error = _renderer.GetError();
            if (error.Length > 0 && error != _lastRendererError)
            {
                _lastRendererError = error;
                LibraryService.Log(error);
                // One photo that cannot be decoded keeps its colour and is logged; it is not a
                // problem the person can act on, so it never becomes a notice.
                if (!error.Contains("decode failed", StringComparison.OrdinalIgnoreCase))
                {
                    ViewModel.Error = error;
                    ViewModel.HasError = true;
                }
            }
        }
        catch (Exception error) { _timer?.Stop(); Fail(Strings.Get("Error_RendererStatus"), error); }
    }

    private async void OnOpenFolder(object sender, RoutedEventArgs e)
    {
        if (ViewModel.IsImporting) return;
        try
        {
            var picker = new FolderPicker();
            picker.FileTypeFilter.Add("*");
            WinRT.Interop.InitializeWithWindow.Initialize(picker, App.WindowHandle);
            var folder = await picker.PickSingleFolderAsync();
            if (folder is not null && _isLoaded) await ImportAsync(folder.Path);
        }
        catch (Exception error) { Fail(Strings.Get("Error_FolderPicker"), error); }
    }

    private async Task ImportAsync(string source)
    {
        if (ViewModel.IsImporting) return;
        var previousLibrary = _library;
        _import?.Dispose();
        _import = new CancellationTokenSource();
        var importToken = _import.Token;
        var isAcceptingPreview = true;
        ViewModel.IsImporting = true;
        ViewModel.HasError = false;
        ViewModel.ImportPercent = 0;
        ViewModel.ImportDetail = Strings.Get("Import_LookingForPhotos");
        ImportBar.ScaleX = 0;
        if (!ViewModel.HasLibrary) ViewModel.Status = Strings.Get("Status_Importing");
        ImportPanel.Visibility = Visibility.Visible;
        _hasPreview = ViewModel.HasLibrary;
        LayoutChrome();
        QueueGlass();
        try
        {
            var progress = new Progress<ImportProgress>(state =>
            {
                if (!_isLoaded || !ViewModel.IsImporting || !isAcceptingPreview || importToken.IsCancellationRequested) return;
                ViewModel.ImportPercent = state.Total == 0 ? 0 : 100.0 * state.Processed / state.Total;
                ImportBar.ScaleX = ViewModel.ImportPercent / 100;
                if (!_hasPreview && state.Preview is not null)
                {
                    _hasPreview = true;
                    _ = ShowPreviewAsync(state.Preview, importToken);
                }
                ViewModel.ImportDetail = Strings.Format("Import_Progress", state.Imported, state.Total);
            });
            var library = await _libraries.ImportAsync(source, progress, importToken);
            isAcceptingPreview = false;
            if (!_isLoaded) return;
            var fit = previousLibrary is null || !string.Equals(previousLibrary.Source, source, StringComparison.OrdinalIgnoreCase);
            if (!await ShowLibraryAsync(library, fit, importToken)) return;
            if (library.Errors.Count > 0)
            {
                ViewModel.Error = Strings.Format("Error_ImportUnreadable", library.Errors.Count);
                ViewModel.HasError = true;
            }
        }
        catch (OperationCanceledException)
        {
            isAcceptingPreview = false;
            if (_isLoaded) await RestoreLibraryAsync(previousLibrary);
            ViewModel.Error = Strings.Get("Error_ImportCancelled");
            ViewModel.HasError = true;
        }
        catch (Exception error)
        {
            isAcceptingPreview = false;
            if (_isLoaded) await RestoreLibraryAsync(previousLibrary);
            Fail(Strings.Get("Error_ImportIncomplete"), error);
        }
        finally
        {
            isAcceptingPreview = false;
            ViewModel.IsImporting = false;
            ImportPanel.Visibility = Visibility.Collapsed;
            if (_library is not null && !ReferenceEquals(_library, _semanticLibrary)) StartSemanticIndex(_library);
            if (_library is not null) ApplyFilter();
            QueueGlass();
        }
    }

    private async Task RestoreLibraryAsync(Library? library)
    {
        // Invalidate a still-computing preview even when it never became the active library.
        _libraryRequestGeneration++;
        _modeRequestGeneration++;
        _pendingLibrary = null;
        if (library is not null)
        {
            if (ReferenceEquals(_library, library)) await SetArrangementAsync(_requestedArrangement);
            else await ShowLibraryAsync(library);
            return;
        }
        _library = null;
        RenderAction(renderer => { renderer.SetScene([], []); ResetSearchPresentation(); });
        ViewModel.HasLibrary = false;
        ViewModel.LibraryName = Strings.Get("Library_EmptyName");
        ViewModel.LibraryCount = Strings.Get("Library_CountNone");
        ViewModel.Status = Strings.Get("Status_NoPhotosYet");
        ViewModel.AllCount = ViewModel.RecentCount = ViewModel.FavouriteCount = "0";
        ViewModel.Results.Clear();
        ViewModel.Facets.Clear();
        ClearSelection();
        EmptyState.Visibility = Visibility.Visible;
        LayoutChrome();
    }

    private Task<Vector3[]> GetCachedPositionsAsync(Library library, Arrangement mode)
    {
        // Called only on the dispatcher. The worker sees an immutable snapshot, never XAML/native state.
        if (mode == Arrangement.Similarity) return SimilarityPositionsAsync(library);
        var layouts = _positionCache.GetValue(library, static _ => new());
        if (layouts.TryGetValue(mode, out var cached) && !cached.IsFaulted && !cached.IsCanceled)
            return cached;
        var photos = library.Photos.ToArray();
        var computation = Task.Run(() => Layout.Positions(photos, mode));
        layouts[mode] = computation;
        // A cancelled preview stops awaiting, not the shared computation. Observe any later failure.
        _ = computation.ContinueWith(static completed => { _ = completed.Exception; },
            CancellationToken.None, TaskContinuationOptions.OnlyOnFaulted | TaskContinuationOptions.ExecuteSynchronously,
            TaskScheduler.Default);
        return computation;
    }

    private async Task ShowPreviewAsync(Library library, CancellationToken cancellationToken)
    {
        try { await ShowLibraryAsync(library, cancellationToken: cancellationToken); }
        catch (OperationCanceledException) { }
        catch (Exception error)
        {
            if (_isLoaded && !cancellationToken.IsCancellationRequested)
                Fail(Strings.Get("Error_PreviewArrange"), error);
        }
    }

    private async Task<bool> ShowLibraryAsync(Library library, bool forceFit = false, CancellationToken cancellationToken = default)
    {
        var generation = ++_libraryRequestGeneration;
        _pendingLibrary = library;
        try
        {
            while (_isLoaded && generation == _libraryRequestGeneration && ReferenceEquals(_pendingLibrary, library))
            {
                cancellationToken.ThrowIfCancellationRequested();
                if (_requestedArrangement == Arrangement.Similarity && !ReferenceEquals(library, _semanticLibrary))
                {
                    _requestedArrangement = Arrangement.Cloud; // a new library has no embeddings loaded yet
                    UpdateArrangementControls(Arrangement.Cloud);
                }
                var mode = _requestedArrangement;
                var modeGeneration = _modeRequestGeneration;
                Vector3[] positions;
                try { positions = await GetCachedPositionsAsync(library, mode).WaitAsync(cancellationToken); }
                catch (OperationCanceledException) { throw; }
                catch when (mode != _requestedArrangement || modeGeneration != _modeRequestGeneration)
                {
                    // Failure of a superseded mode must not abort loading the newly requested one.
                    continue;
                }
                cancellationToken.ThrowIfCancellationRequested();
                if (!_isLoaded || generation != _libraryRequestGeneration || !ReferenceEquals(_pendingLibrary, library))
                    return false;
                // A mode change during import must never install the old mode. Reuse its cached result
                // later, and await the latest requested mode before atomically swapping the scene.
                if (mode != _requestedArrangement || modeGeneration != _modeRequestGeneration) continue;
                _modeRequestGeneration++;
                _arrangement = mode;
                CommitLibrary(library, positions, forceFit);
                return true;
            }
            return false;
        }
        catch when (!_isLoaded || generation != _libraryRequestGeneration || !ReferenceEquals(_pendingLibrary, library))
        {
            // A late failure from a superseded preview/import is no longer a collection error.
            return false;
        }
        finally
        {
            if (generation == _libraryRequestGeneration && ReferenceEquals(_pendingLibrary, library))
                _pendingLibrary = null;
        }
    }

    private void CommitLibrary(Library library, Vector3[] positions, bool forceFit)
    {
        // No awaits below: the UI photo indices and native scene change together on the dispatcher.
        // Until this point the previous library remains pickable and fully interactive.
        var isNewSource = !string.Equals(_library?.Source, library.Source, StringComparison.OrdinalIgnoreCase);
        _library = library;
        ViewModel.HasLibrary = library.Photos.Count > 0;
        var name = Path.GetFileName(library.Source.TrimEnd(Path.DirectorySeparatorChar));
        ViewModel.LibraryName = string.IsNullOrEmpty(name) ? library.Source : name;
        ViewModel.LibraryCount = Strings.Format("Library_Count", library.Photos.Count);
        ToolTipService.SetToolTip(SourceDetails, library.Source);
        EmptyState.Visibility = ViewModel.HasLibrary ? Visibility.Collapsed : Visibility.Visible;
        ClearSelection();
        if (isNewSource)
        {
            _hasInspectorOpened = false;
            _selectedFacet = null;
            _scope = "All";
            Search.Text = "";
            _isSynchronizingSelection = true;
            Destinations.SelectedItem = AllDestination;
            _isSynchronizingSelection = false;
        }
        BuildFacets();
        UpdateDestinationCounts();
        // Partial previews during an import are not indexed; the finished library is.
        if (!ViewModel.IsImporting && !ReferenceEquals(library, _semanticLibrary)) StartSemanticIndex(library);
        RenderAction(renderer =>
        {
            renderer.SetScene(library.Photos, positions);
            ResetSearchPresentation();
            if (isNewSource || forceFit) renderer.Fit();
        });
        ApplyFilter();
        LayoutChrome();
    }

    private static bool IsThisYear(Photo photo) => photo.TakenAt is { } date && date.ToLocalTime().Year == DateTimeOffset.Now.Year;

    private void UpdateDestinationCounts()
    {
        if (_library is null) return;
        ViewModel.AllCount = _library.Photos.Count.ToString("N0");
        ViewModel.RecentCount = _library.Photos.Count(IsThisYear).ToString("N0");
        ViewModel.FavouriteCount = _library.Photos.Count(p => _preferences.FavouriteIds.Contains(p.Id)).ToString("N0");
    }

    private void BuildFacets()
    {
        ViewModel.Facets.Clear();
        _facts.Clear();
        _factIndex = 0;
        if (_library is null) return;
        var photos = _library.Photos;
        var total = photos.Count;
        IReadOnlyList<ExploreFacet> Group(string kind, Func<Photo, string> key, bool byKey = false)
        {
            var groups = photos.GroupBy(key).ToArray();
            var ordered = byKey ? groups.OrderByDescending(g => g.Key) : groups.OrderByDescending(g => g.Count());
            var max = groups.Length == 0 ? 1 : groups.Max(g => g.Count());
            return ordered.Select(g => new ExploreFacet(g.Key, kind, g.Key, g.Count(), total, g.First().ThumbnailPath, (double)g.Count() / max)).ToArray();
        }
        var departments = photos.Any(p => p.Department.Length > 0)
            ? Group("Department", p => p.Department.Length > 0 ? p.Department : "Other") : [];
        var colours = Group("Colour", ColourFamily);
        var shapes = Group("Proportion", Proportion);
        var dated = photos.Count(p => p.TakenAt.HasValue);
        var years = dated > 0 ? Group("Year", p => p.TakenAt?.Year.ToString() ?? "Undated", byKey: true) : [];
        IReadOnlyList<ExploreFacet> listed;
        if (_arrangement == Arrangement.Time && years.Count > 0)
        {
            ViewModel.ExploreTitle = Strings.Get("Explore_TimeTitle");
            ViewModel.ExploreDescription = Strings.Format("Explore_TimeDescription", dated, total);
            _overviewEyebrow = Strings.Get("Explore_YearsEyebrow");
            listed = years;
        }
        else if (_arrangement == Arrangement.Colour)
        {
            ViewModel.ExploreTitle = Strings.Get("Explore_ColourTitle");
            ViewModel.ExploreDescription = Strings.Format("Explore_ColourDescription", total);
            _overviewEyebrow = Strings.Get("Explore_ColoursEyebrow");
            listed = colours;
        }
        else
        {
            ViewModel.ExploreTitle = Strings.Get("Explore_LibraryTitle");
            ViewModel.ExploreDescription = departments.Count > 0
                ? Strings.Format("Explore_LibraryDescriptionDepartments", total, departments.Count) : Strings.Format("Explore_LibraryDescription", total);
            _overviewEyebrow = departments.Count > 0 ? Strings.Get("Explore_DepartmentsEyebrow") : Strings.Get("Explore_ShapesEyebrow");
            listed = departments.Count > 0 ? departments : shapes;
        }
        foreach (var facet in listed) ViewModel.Facets.Add(facet);

        // Did you know: measured facts, each one a set of photos you can open.
        void AddFact(string glyph, string caption, IReadOnlyList<ExploreFacet> facets)
        {
            if (facets.Count < 2) return;
            var top = facets.MaxBy(f => f.Count)!;
            var thumbs = photos.Where(p => Matches(top, p)).Take(6).Select(p => p.ThumbnailPath).ToArray();
            _facts.Add(new(glyph, caption, Strings.Format("Fact_Value", top.Count), Strings.Format("Fact_Body", top.Name, (double)top.Count / total), top, thumbs));
        }
        AddFact("\uE8F1", Strings.Get("Fact_LargestDepartment"), departments);
        AddFact("\uE790", Strings.Get("Fact_MostCommonColour"), colours);
        AddFact("\uE787", Strings.Get("Fact_BusiestYear"), years.Where(y => y.Key != "Undated").ToArray());
        AddFact("\uE8B9", Strings.Get("Fact_MostCommonShape"), shapes);
        _overviewTitle = ViewModel.ExploreTitle;
        _overviewDescription = ViewModel.ExploreDescription;
        UpdateExploreContent();
    }

    private static bool Matches(ExploreFacet facet, Photo photo) => facet.Kind switch
    {
        "Department" => (photo.Department.Length > 0 ? photo.Department : "Other") == facet.Key,
        "Year" => (photo.TakenAt?.Year.ToString() ?? "Undated") == facet.Key,
        "Colour" => ColourFamily(photo) == facet.Key,
        "Proportion" => Proportion(photo) == facet.Key,
        _ => true
    };

    private static string Proportion(Photo photo) =>
        photo.Width == photo.Height ? "Square" : photo.Width > photo.Height ? "Landscape" : "Portrait";

    private static string ColourFamily(Photo photo)
    {
        var max = Math.Max(photo.Red, Math.Max(photo.Green, photo.Blue));
        var min = Math.Min(photo.Red, Math.Min(photo.Green, photo.Blue));
        var delta = max - min;
        if (delta < 0.06f || delta / Math.Max(max, .001f) < .13f) return "Neutral";
        var hue = max == photo.Red ? (photo.Green - photo.Blue) / delta
            : max == photo.Green ? 2 + (photo.Blue - photo.Red) / delta : 4 + (photo.Red - photo.Green) / delta;
        hue = (hue * 60 + 360) % 360;
        return hue switch
        {
            < 20 => "Red", < 45 => "Orange", < 70 => "Yellow", < 160 => "Green",
            < 195 => "Teal", < 260 => "Blue", < 295 => "Purple", < 340 => "Pink", _ => "Red"
        };
    }

    private bool MatchesFacet(Photo photo) => _selectedFacet is null || Matches(_selectedFacet, photo);

    private void ApplyFilter()
    {
        if (_library is null) return;
        var query = Search.Text.Trim();
        var hasQuery = query.Length > 0;
        // Structural destinations restrict the collection. Search is an independent presentation
        // layer: its nonmatches remain as faint context instead of being filtered out.
        var mask = new byte[_library.Photos.Count];
        var matchedIndices = new List<int>();
        // Semantic ranking when the image model has understood this library:
        // results in score order, each with its own confidence; title/artist/department matches
        // the model missed follow, so a museum label typed exactly is never lost.
        var semanticHits = hasQuery ? SemanticHitsFor(query) : null;
        if (hasQuery && IsSemanticReady && semanticHits is null) return; // ranking in flight: keep the current view
        var weights = new Dictionary<int, float>();
        if (semanticHits is not null)
            foreach (var (index, score) in semanticHits) weights[index] = Math.Clamp(0.35f + 0.65f * score, 0.35f, 1f);
        for (var i = 0; i < mask.Length; i++)
        {
            var photo = _library.Photos[i];
            if (_scope == "Year" && !IsThisYear(photo)) continue;
            if (_scope == "Favourites" && !_preferences.FavouriteIds.Contains(photo.Id)) continue;
            if (!MatchesFacet(photo)) continue;
            mask[i] = 1;
            if (semanticHits is null && (!hasQuery || PhotoSearch.Matches(photo, query))) matchedIndices.Add(i);
        }
        if (semanticHits is not null)
        {
            matchedIndices.AddRange(semanticHits.Select(h => h.Index).Where(i => mask[i] == 1));
            var semanticSet = matchedIndices.ToHashSet();
            for (var i = 0; i < mask.Length; i++)
                if (mask[i] == 1 && !semanticSet.Contains(i) && PhotoSearch.Matches(_library.Photos[i], query))
                {
                    matchedIndices.Add(i);
                    weights[i] = 0.5f;
                }
        }
        var count = matchedIndices.Count;
        var selected = ViewModel.Selected;
        _isSynchronizingSelection = true;
        try
        {
            // Resetting a ListView's collection is not a user deselection. Preserve the inspector
            // and native selection even when the selected photo is not among the listed matches.
            ViewModel.Results.Clear();
            foreach (var index in matchedIndices.Take(150)) ViewModel.Results.Add(_library.Photos[index]);
            Results.SelectedItem = selected is null ? null : ViewModel.Results.FirstOrDefault(photo => photo.Id == selected.Id);
        }
        finally { _isSynchronizingSelection = false; }
        ClearHover();
        RenderAction(renderer =>
        {
            if (!_lastStructuralMask.SequenceEqual(mask))
            {
                renderer.SetFilter(mask);
                _lastStructuralMask = mask;
            }
            if (hasQuery)
            {
                // Reuse identical targets rather than restarting a tween/camera fit for a query
                // spelling change, favourite toggle, or base-layout change with the same matches.
                // A different arrangement re-lays the same matches in that arrangement's shape.
                if (!_hasSearchResults || _lastResultArrangement != _arrangement || !_lastResultIndices.SequenceEqual(matchedIndices))
                {
                    _lastResultIndices = matchedIndices.ToArray();
                    _lastResultArrangement = _arrangement;
                    _hasSearchResults = true;
                    _ = PlaceSearchResultsAsync(_lastResultIndices, weights, semanticHits);
                }
            }
            else if (_hasSearchResults)
            {
                // Native returns to the latest base positions and saved pre-search camera.
                // Never clear/reseed the scene or force a yaw/pitch/fit here.
                renderer.ClearResults();
                ResetSearchPresentation(clearStructuralMask: false);
            }
        });
        var total = _library.Photos.Count;
        var visible = mask.Count(b => b == 1);
        ViewModel.ResultCount = Strings.Format("Results_Count", count, total);
        ViewModel.ResultsNote = count > 150 ? Strings.Get("Results_FirstNote") : "";
        var filters = new List<string>();
        if (_scope == "Year") filters.Add(Strings.Get("Filter_ThisYear"));
        if (_scope == "Favourites") filters.Add(Strings.Get("Filter_Favourites"));
        if (_selectedFacet is not null) filters.Add(_selectedFacet.Name);
        ViewModel.FilterLabel = string.Join(" · ", filters);
        FilterChip.Visibility = filters.Count > 0 ? Visibility.Visible : Visibility.Collapsed;
        ClearSearch.Visibility = Search.Text.Length > 0 ? Visibility.Visible : Visibility.Collapsed;
        // The subtitle line: what you are looking at, never a separate status bar.
        var searchKind = semanticHits is not null ? Strings.Get("Status_SearchKindSemantic") : Strings.Get("Status_SearchKindMetadata");
        var arrangementStatus = _semanticProgress.Length > 0 ? $"{_arrangement} · {_semanticProgress}" : $"{_arrangement}";
        ViewModel.Status = hasQuery ? Strings.Format("Status_SearchResults", query, count, total, searchKind)
            : SimilarityStatus(total) is { } similarityStatus ? similarityStatus
            : visible < total ? Strings.Format("Status_FilteredPhotos", visible, total, arrangementStatus)
            : Strings.Format("Status_Photos", total, arrangementStatus);
        UpdateExploreContent();
        LayoutChrome();
    }

    /// Search keeps the arrangement you are in: Cloud is the search volume (<see cref="Layout.FocusPositions"/>),
    /// Similarity lays the matches out as their own similarity cloud, Time and Colour as their own
    /// helix and colour solid, and Grid as a flat grid in rank order. Layout runs off the UI thread;
    /// only the latest request is installed.
    private async Task PlaceSearchResultsAsync(int[] indices, IReadOnlyDictionary<int, float> weights,
        IReadOnlyList<(int Index, float Score)>? semanticHits)
    {
        if (_library is not { } library || indices.Length == 0) return;
        var generation = ++_resultLayoutGeneration;
        var arrangement = _arrangement;
        var uniform = arrangement == Arrangement.Grid; // a grid reads as a grid only with equal tiles
        Func<Vector3[]> layout;
        if (arrangement == Arrangement.Grid)
            layout = () => Layout.RankedGrid(indices.Length, depth: -0.5f); // just in front of the faint library sheet
        else if (arrangement is Arrangement.Time or Arrangement.Colour)
        {
            var photos = indices.Select(i => library.Photos[i]).ToArray();
            layout = arrangement == Arrangement.Time ? () => Layout.TimeSearchPositions(photos) : () => Layout.Positions(photos, arrangement);
        }
        else if (arrangement == Arrangement.Similarity && SimilarityLayout(library, indices) is { } similarityLayout)
            layout = similarityLayout;
        else layout = FocusLayout(library, indices, semanticHits) ?? (() => GatherPositions(indices.Length));
        Vector3[] positions;
        var clock = System.Diagnostics.Stopwatch.StartNew();
        try { positions = await Task.Run(layout); }
        catch (Exception error)
        {
            LibraryService.Log($"Search layout for {arrangement} failed; gather ball shown: {error.Message}");
            positions = GatherPositions(indices.Length);
        }
        if (generation != _resultLayoutGeneration || !ReferenceEquals(library, _library) || !_hasSearchResults) return;
        LibraryService.Log($"Search {arrangement}: {indices.Length} matches laid out in {clock.ElapsedMilliseconds} ms.");
        var results = new NativeRenderer.Result[indices.Length];
        for (var k = 0; k < indices.Length; k++)
            results[k] = new()
            {
                Id = (uint)indices[k], X = positions[k].X, Y = positions[k].Y, Z = positions[k].Z,
                Weight = uniform ? 1 : weights.TryGetValue(indices[k], out var w) ? w : 1,
            };
        RenderAction(renderer => renderer.SetResults(results));
    }

    /// Similarity for the matches alone: the library's PaCMAP layout run on just their embeddings, so
    /// they form one similarity cloud at full size. Very large match sets (PaCMAP would take seconds)
    /// regather their positions from the library's Similarity map instead.
    private Func<Vector3[]>? SimilarityLayout(Library library, IReadOnlyList<int> indices)
    {
        if (!IsSemanticReady || !ReferenceEquals(library, _semanticLibrary)) return null;
        if (indices.Count > 4000)
        {
            if (_similarityPositions is not { IsCompletedSuccessfully: true } map || map.Result.Length != library.Photos.Count) return null;
            var subset = indices.Select(i => map.Result[i]).ToArray();
            return () => Layout.Regather(subset);
        }
        var dims = _semantic!.Dimensions;
        var matrix = new float[indices.Count * dims];
        for (var k = 0; k < indices.Count; k++)
        {
            try { _semantic.Vector(library.Photos[indices[k]].Id).CopyTo(matrix.AsSpan(k * dims)); }
            catch (KeyNotFoundException) { return null; }
        }
        return () => Layout.SubsetSimilarityPositions(matrix, indices.Count, dims);
    }

    /// The search arrangement: the matches as a volume shaped by their own
    /// image embeddings — what looks alike sits together, stronger matches nearer the camera — instead
    /// of a uniform ball. Needs the local image model; metadata-only search keeps the gather ball.
    private Func<Vector3[]>? FocusLayout(Library library, IReadOnlyList<int> indices, IReadOnlyList<(int Index, float Score)>? semanticHits)
    {
        if (!IsSemanticReady || indices.Count == 0) return null;
        var scoreOf = semanticHits?.ToDictionary(h => h.Index, h => h.Score) ?? new Dictionary<int, float>();
        var rows = new List<float[]>(indices.Count);
        var scores = new List<float>(indices.Count);
        foreach (var index in indices)
        {
            try { rows.Add(_semantic!.Vector(library.Photos[index].Id)); }
            catch (KeyNotFoundException) { return null; } // a photo not yet understood: keep the ball
            scores.Add(scoreOf.TryGetValue(index, out var s) ? s : 0);
        }
        // FocusPositions is right-handed with the camera on +Z; the native camera looks from −Z, so the
        // depth axis flips to keep the strongest matches nearest.
        return () => Layout.FocusPositions(rows, scores).Select(p => p with { Z = -p.Z }).ToArray();
    }

    private static NativeRenderer.Result[] GatherSearchResults(IReadOnlyList<int> indices, IReadOnlyDictionary<int, float>? weights = null)
    {
        var positions = GatherPositions(indices.Count);
        var results = new NativeRenderer.Result[indices.Count];
        for (var k = 0; k < indices.Count; k++)
            results[k] = new()
            {
                Id = (uint)indices[k], X = positions[k].X, Y = positions[k].Y, Z = positions[k].Z,
                Weight = weights is not null && weights.TryGetValue(indices[k], out var w) ? w : 1
            };
        return results;
    }

    private static Vector3[] GatherPositions(int length)
    {
        // Gather: a world-fixed Fibonacci ball, not a grid or a
        // camera-facing spiral. World units are layout units / 32. Results arrive in rank
        // order, so the best matches take the core; semantic confidence scales each tile.
        const float spacing = 82f / 32f;
        var count = Math.Max(length, 1);
        var positions = new Vector3[length];
        for (var k = 0; k < length; k++)
        {
            var t = (k + .5f) / count;
            var radius = spacing * MathF.Cbrt(3 * (k + .5f) / (4 * MathF.PI));
            var phi = MathF.Acos(Math.Clamp(1 - 2 * t, -1, 1));
            var theta = k * 2.399963f;
            positions[k] = new(radius * MathF.Sin(phi) * MathF.Cos(theta), radius * MathF.Cos(phi), radius * MathF.Sin(phi) * MathF.Sin(theta));
        }
        return positions;
    }

    private void ResetSearchPresentation(bool clearStructuralMask = true)
    {
        _hasSearchResults = false;
        _lastResultIndices = [];
        _resultLayoutGeneration++;
        if (clearStructuralMask) _lastStructuralMask = [];
    }

    private void OnSearchChanged(object sender, TextChangedEventArgs e)
    {
        if (!_isReady) return;
        SuppressBuiltInClearButton();
        // A query replaces any gathered or pinned group: search owns the gather now.
        if (!string.IsNullOrWhiteSpace(Search.Text) && (_gatheredLabel is not null || _pinnedHighlight is not null)) { _gatheredLabel = null; ClearHighlight(); }
        // A new query clears single-photo emphasis, so new matches are not dimmed.
        if (ViewModel.Selected is not null) ClearSelection();
        ClearSearch.Visibility = Search.Text.Length > 0 ? Visibility.Visible : Visibility.Collapsed;
        if (!string.IsNullOrWhiteSpace(Search.Text) && !_preferences.IsExploreVisible)
        {
            _preferences.IsExploreVisible = true;
            LayoutChrome();
        }
        _searchTimer?.Stop();
        _searchTimer?.Start();
    }

    private void OnSearchKeyDown(object sender, KeyRoutedEventArgs e)
    {
        if (e.Key == VirtualKey.Enter) { _searchTimer?.Stop(); ApplyFilter(); e.Handled = true; }
        else if (e.Key == VirtualKey.Escape && Search.Text.Length > 0) { OnClearSearch(sender, new()); e.Handled = true; }
        else if (e.Key == VirtualKey.Down && Results.Visibility == Visibility.Visible && ViewModel.Results.Count > 0)
        {
            Results.Focus(FocusState.Keyboard);
            e.Handled = true;
        }
    }

    private void OnSearchLoaded(object sender, RoutedEventArgs e) => SuppressBuiltInClearButton();

    private void OnSearchFocus(object sender, RoutedEventArgs e) =>
        SearchFocusRim.Opacity = Search.FocusState == FocusState.Unfocused ? 0 : 1;

    private void OnClearSearch(object sender, RoutedEventArgs e)
    {
        Search.Text = "";
        _searchTimer?.Stop();
        ApplyFilter();
        Search.Focus(FocusState.Programmatic);
    }

    /// The platform TextBox brings its own square clear button; the capsule has its own round one.
    private void SuppressBuiltInClearButton()
    {
        static DependencyObject? Find(DependencyObject root, string name)
        {
            for (var i = 0; i < VisualTreeHelper.GetChildrenCount(root); i++)
            {
                var child = VisualTreeHelper.GetChild(root, i);
                if (child is FrameworkElement { Name: var n } && n == name) return child;
                if (Find(child, name) is { } found) return found;
            }
            return null;
        }
        if (Find(Search, "DeleteButton") is Button button && !ReferenceEquals(button, _suppressedDeleteButton))
        {
            _suppressedDeleteButton = button;
            button.Width = 0;
            button.MinWidth = 0;
            button.IsHitTestVisible = false;
            button.IsTabStop = false;
            button.Visibility = Visibility.Collapsed;
            // The TextBox's ButtonVisible state turns it back on whenever text and focus meet.
            button.RegisterPropertyChangedCallback(UIElement.VisibilityProperty, static (sender, _) =>
            {
                if (sender is UIElement element && element.Visibility == Visibility.Visible) element.Visibility = Visibility.Collapsed;
            });
        }
    }

    private Button? _suppressedDeleteButton;

    private void OnDestinationChanged(object sender, SelectionChangedEventArgs e)
    {
        if (!_isReady || _isSynchronizingSelection || Destinations.SelectedItem is not ListViewItem { Tag: string scope }) return;
        _scope = scope;
        _selectedFacet = null;
        if (scope == "All" && Search.Text.Length > 0) Search.Text = "";
        _preferences.IsExploreVisible = true;
        ApplyFilter();
    }

    private void OnFacetClick(object sender, ItemClickEventArgs e)
    {
        if (e.ClickedItem is not ExploreFacet facet) return;
        _selectedFacet = facet;
        ApplyFilter();
        RenderAction(renderer => renderer.Fit());
    }

    private void OnClearFilter(object sender, RoutedEventArgs e)
    {
        _scope = "All";
        _selectedFacet = null;
        _isSynchronizingSelection = true;
        Destinations.SelectedItem = AllDestination;
        _isSynchronizingSelection = false;
        ApplyFilter(); // Deliberately preserves the independently entered metadata query.
    }

    private void OnExploreBack(object sender, RoutedEventArgs e)
    {
        Search.Text = "";
        _searchTimer?.Stop();
        OnClearFilter(sender, e);
    }

    private async void OnModeChecked(object sender, RoutedEventArgs e)
    {
        UpdateSegmentPill(animate: true);
        if (_isReady && !_isUpdatingModeControls && sender is FrameworkElement { Tag: string name } &&
            Enum.TryParse<Arrangement>(name, out var mode))
            await SetArrangementAsync(mode);
    }

    /// Toolbar menus drop from the capsule, not from the glyph: their edge lines up with the
    /// capsule's edge (left for leading buttons, right for More), so the menu reads as its extension.
    private static void ShowFromCapsule(MenuFlyout menu, FrameworkElement capsule, bool alignRight) =>
        menu.ShowAt(capsule, new FlyoutShowOptions
        {
            Placement = alignRight ? FlyoutPlacementMode.BottomEdgeAlignedRight : FlyoutPlacementMode.BottomEdgeAlignedLeft,
            ShowMode = FlyoutShowMode.Standard,
        });

    private void OnShowArrangeMenu(object sender, RoutedEventArgs e) => ShowFromCapsule(ArrangeMenu, CommandChrome, alignRight: false);

    /// The stock open animation slides the menu out from under a clip at the popup edge (a visible
    /// hard line under the capsule) and takes ~250 ms. Ours is a 110 ms fade in place: the acrylic
    /// backdrop belongs to the popup window, so moving the presenter would split tint from blur.
    /// The fade is a XAML Opacity storyboard on purpose: taking the presenter's composition
    /// (hand-off) visual breaks the windowed popup's offset, so the menu draws in place but its
    /// hit-testing sits a window-offset away and every click dismisses it.
    private void OnMenuOpened(object? sender, object e)
    {
        if (XamlRoot is null || !_uiSettings.AnimationsEnabled) return;
        foreach (var popup in VisualTreeHelper.GetOpenPopupsForXamlRoot(XamlRoot))
        {
            if (popup.Child is not MenuFlyoutPresenter presenter) continue;
            var fade = new DoubleAnimation
            {
                From = 0, To = 1, Duration = new Duration(TimeSpan.FromMilliseconds(110)),
                EasingFunction = new ExponentialEase { EasingMode = EasingMode.EaseOut, Exponent = 4 },
            };
            Storyboard.SetTarget(fade, presenter);
            Storyboard.SetTargetProperty(fade, "Opacity");
            var storyboard = new Storyboard();
            storyboard.Children.Add(fade);
            storyboard.Begin();
        }
    }
    private void OnShowPixelsMenu(object sender, RoutedEventArgs e) => ShowFromCapsule(PixelsMenu, ToolsChrome, alignRight: false);
    private void OnShowMoreMenu(object sender, RoutedEventArgs e) => ShowFromCapsule(MoreMenu, ToolsChrome, alignRight: true);

    /// ToggleMenuFlyoutItem draws the checkmark; a group still has exactly one checked item.
    private static void CheckOnly(IEnumerable<MenuFlyoutItemBase> items, object chosen)
    {
        foreach (var item in items.OfType<ToggleMenuFlyoutItem>()) item.IsChecked = ReferenceEquals(item, chosen);
    }

    private async void OnArrangeMenu(object sender, RoutedEventArgs e)
    {
        if (sender is ToggleMenuFlyoutItem toggle) toggle.IsChecked = true;
        if (sender is FrameworkElement { Tag: string name } && Enum.TryParse<Arrangement>(name, out var mode))
            await SetArrangementAsync(mode);
    }

    private RadioButton SegmentFor(Arrangement mode) => mode switch
    {
        Arrangement.Time => ModeTime, Arrangement.Colour => ModeColour, Arrangement.Grid => ModeGrid,
        Arrangement.Similarity => ModeSimilarity, _ => ModeCloud
    };

    private void UpdateArrangementControls(Arrangement mode)
    {
        _isUpdatingModeControls = true;
        try
        {
            SegmentFor(mode).IsChecked = true;
            ArrangementLabel.Text = mode.ToString();
            MenuCloud.IsChecked = mode == Arrangement.Cloud;
            MenuColour.IsChecked = mode == Arrangement.Colour;
            MenuTime.IsChecked = mode == Arrangement.Time;
            MenuGrid.IsChecked = mode == Arrangement.Grid;
            MenuSimilarity.IsChecked = mode == Arrangement.Similarity;
        }
        finally { _isUpdatingModeControls = false; }
        UpdateSegmentPill(animate: true);
    }

    /// The selection pill slides between segments (220 ms, decelerating), and the hairline
    /// separators on either side of the selected segment step aside.
    // The pill is placed from measured segment bounds; when the control unfolds from the compact
    // menu (or fonts/scale change) those bounds only exist after layout, so place it then.
    private void OnSegmentHostSizeChanged(object sender, SizeChangedEventArgs e) => UpdateSegmentPill(animate: false);

    private void UpdateSegmentPill(bool animate)
    {
        var segments = new[] { ModeCloud, ModeSimilarity, ModeTime, ModeColour, ModeGrid };
        var selected = Array.FindIndex(segments, s => s.IsChecked == true);
        if (selected < 0 || segments[selected].ActualWidth <= 0) return;
        var separators = new[] { Sep0, Sep1, Sep2, Sep3 };
        for (var i = 0; i < separators.Length; i++)
            separators[i].Opacity = i == selected || i + 1 == selected ? 0 : 1;
        var x = segments[selected].TransformToVisual(SegmentHost).TransformPoint(default).X;
        var width = segments[selected].ActualWidth;
        if (!animate || SegmentPill.Width <= 0 || !_uiSettings.AnimationsEnabled)
        {
            SegmentPillOffset.X = x;
            SegmentPill.Width = width;
            return;
        }
        var ease = new ExponentialEase { EasingMode = EasingMode.EaseOut, Exponent = 5 };
        var duration = new Duration(TimeSpan.FromMilliseconds(220));
        var move = new DoubleAnimation { To = x, Duration = duration, EasingFunction = ease };
        var size = new DoubleAnimation { To = width, Duration = duration, EasingFunction = ease, EnableDependentAnimation = true };
        Storyboard.SetTarget(move, SegmentPillOffset);
        Storyboard.SetTargetProperty(move, "X");
        Storyboard.SetTarget(size, SegmentPill);
        Storyboard.SetTargetProperty(size, "Width");
        var storyboard = new Storyboard();
        storyboard.Children.Add(move);
        storyboard.Children.Add(size);
        storyboard.Begin();
    }

    private async Task SetArrangementAsync(Arrangement mode)
    {
        if (mode == Arrangement.Similarity && !IsSemanticReady) { UpdateArrangementControls(_arrangement); return; }
        _requestedArrangement = mode;
        var generation = ++_modeRequestGeneration;
        var libraryGeneration = _libraryRequestGeneration;
        var library = _library;
        UpdateArrangementControls(mode);
        if (library is null) return; // Any pending library load will use this requested mode.
        if (_arrangement == mode) { ApplyFilter(); return; }
        if (ViewModel.Selected is not null) ClearSelection();
        try
        {
            var positions = await GetCachedPositionsAsync(library, mode);
            if (!_isLoaded || generation != _modeRequestGeneration || libraryGeneration != _libraryRequestGeneration ||
                !ReferenceEquals(library, _library) || mode != _requestedArrangement) return;
            // Keep the previous native view responsive while computing. Only the latest request
            // morphs positions; neither mode switching nor filter/search changes reset the camera.
            if (_arrangement == Arrangement.Similarity && mode != Arrangement.Similarity) LeaveSimilarity();
            // A new arrangement always recentres — target to
            // the new layout's centre at its home distance — so it never opens in an odd corner of
            // the previous one. Fit frames the new targets, not the positions mid-morph.
            RenderAction(renderer => { renderer.SetPositions(positions); renderer.Fit(); });
            _arrangement = mode;
            if (mode == Arrangement.Similarity) { _geometryPositions = null; EnsureGroupGeometry(); }
            BuildFacets();
            ApplyFilter();
            if (mode == Arrangement.Time && !library.Photos.Any(p => p.TakenAt.HasValue))
                ViewModel.Status = Strings.Format("Status_TimeNoDates", library.Photos.Count);
        }
        catch (Exception error)
        {
            if (!_isLoaded || generation != _modeRequestGeneration || libraryGeneration != _libraryRequestGeneration ||
                !ReferenceEquals(library, _library) || mode != _requestedArrangement) return;
            _requestedArrangement = _arrangement;
            UpdateArrangementControls(_arrangement);
            ApplyFilter();
            Fail(Strings.Get("Error_ArrangementCompute"), error);
        }
    }

    private void OnPixels(object sender, RoutedEventArgs e)
    {
        if (sender is FrameworkElement { Tag: string level } && int.TryParse(level, out var value))
        {
            CheckOnly(PixelsMenu.Items, sender);
            RenderAction(renderer => renderer.SetPixels(value));
            PixelsIcon.Glyph = value == 0 ? "\uF0E2" : "\uE80A";
        }
    }

    private bool IsExploreList => !string.IsNullOrWhiteSpace(Search.Text) || _scope != "All" || _selectedFacet is not null;

    /// Explore follows what you are looking at: the library's measurements,
    /// or — once a search, destination or measurement narrows it — the photos themselves.
    private void UpdateExploreContent()
    {
        if (!_isReady) return;
        if (IsSimilarityPanel && _library is not null)
        {
            // In Similarity the card shows the visual groups, not the measurements.
            var (title, description) = SimilarityHeader();
            ExploreBack.Visibility = Visibility.Collapsed;
            ViewModel.ExploreTitle = title;
            ViewModel.ExploreDescription = description;
            FactCard.Visibility = ListEyebrow.Visibility = Facets.Visibility = Results.Visibility = Visibility.Collapsed;
            NoResults.Visibility = NoFacets.Visibility = Visibility.Collapsed;
            RefreshSimilarityPanel();
            return;
        }
        SimilarityView.Visibility = Visibility.Collapsed;
        ListEyebrow.Visibility = Visibility.Visible;
        var isList = IsExploreList;
        var isSearch = !string.IsNullOrWhiteSpace(Search.Text);
        ExploreBack.Visibility = isList ? Visibility.Visible : Visibility.Collapsed;
        ViewModel.ExploreTitle = !isList ? _overviewTitle
            : isSearch ? Strings.Get("Explore_SearchTitle")
            : _selectedFacet is not null ? _selectedFacet.Name
            : _scope == "Favourites" ? Strings.Get("Explore_FavouritesTitle") : _scope == "Year" ? Strings.Get("Explore_ThisYearTitle") : Strings.Get("Explore_PhotosTitle");
        ViewModel.ExploreDescription = !isList ? _overviewDescription
            : isSearch ? Strings.Format("Explore_SearchDescription", ViewModel.ResultCount)
            : _scope == "Favourites" ? Strings.Format("Explore_FavouritesDescription", ViewModel.ResultCount)
            : ViewModel.ResultCount;
        ListEyebrow.Text = isList ? (ViewModel.ResultsNote.Length > 0 ? ViewModel.ResultsNote : Strings.Get("Explore_PhotosEyebrow")) : _overviewEyebrow;
        Facets.Visibility = isList ? Visibility.Collapsed : Visibility.Visible;
        Results.Visibility = isList ? Visibility.Visible : Visibility.Collapsed;
        NoResults.Visibility = isList && ViewModel.Results.Count == 0 ? Visibility.Visible : Visibility.Collapsed;
        NoFacets.Visibility = !isList && ViewModel.Facets.Count == 0 ? Visibility.Visible : Visibility.Collapsed;
        var showFact = !isList && _facts.Count > 0 && _exploreHeight >= 470;
        FactCard.Visibility = showFact ? Visibility.Visible : Visibility.Collapsed;
        if (showFact) ShowFact();
    }

    private void ShowFact()
    {
        if (_facts.Count == 0) return;
        var fact = _facts[_factIndex % _facts.Count];
        FactIcon.Glyph = fact.Glyph;
        FactCaption.Text = fact.Caption;
        FactValue.Text = fact.Value;
        FactBody.Text = fact.Body;
        FactThumbs.Children.Clear();
        foreach (var thumb in fact.Thumbs)
            FactThumbs.Children.Add(new Border
            {
                Width = 30, Height = 30, CornerRadius = new CornerRadius(6),
                Background = new ImageBrush { ImageSource = Thumb(thumb), Stretch = Stretch.UniformToFill }
            });
        AutomationProperties.SetName(FactCard, $"{fact.Caption}: {fact.Value}. {fact.Body}");
    }

    private void OnFactShow(object sender, RoutedEventArgs e)
    {
        if (_facts.Count == 0) return;
        _selectedFacet = _facts[_factIndex % _facts.Count].Facet;
        ApplyFilter();
        RenderAction(renderer => renderer.Fit());
    }

    private void OnFactNext(object sender, RoutedEventArgs e)
    {
        if (_facts.Count == 0) return;
        _factIndex = (_factIndex + 1) % _facts.Count;
        ShowFact();
    }

    private void OnFacetContainerChanging(ListViewBase sender, ContainerContentChangingEventArgs args)
    {
        if (args.Item is ExploreFacet facet && !args.InRecycleQueue)
        {
            AutomationProperties.SetAutomationId(args.ItemContainer, $"Facet_{facet.Kind}_{facet.Key}");
            AutomationProperties.SetName(args.ItemContainer, facet.Description);
        }
    }

    private void OnResultContainerChanging(ListViewBase sender, ContainerContentChangingEventArgs args)
    {
        if (args.Item is Photo photo && !args.InRecycleQueue)
        {
            AutomationProperties.SetAutomationId(args.ItemContainer, $"Photo_{photo.Id}");
            AutomationProperties.SetName(args.ItemContainer, $"{photo.Title}, {photo.Attribution}");
        }
    }

    private void OnResultSelected(object sender, SelectionChangedEventArgs e)
    {
        if (!_isSynchronizingSelection && Results.SelectedItem is Photo photo && _library is not null)
            SelectPhoto(_library.Photos.IndexOf(photo));
    }

    private void OnResultClick(object sender, ItemClickEventArgs e)
    {
        // A row is the keyboard/screen-reader twin of a canvas click: select and fly to it.
        if (!_isSynchronizingSelection && e.ClickedItem is Photo photo && photo != ViewModel.Selected && _library is not null)
            SelectPhoto(_library.Photos.IndexOf(photo), frame: true);
    }

    private void OnResultDoubleTapped(object sender, DoubleTappedRoutedEventArgs e)
    {
        if (Results.SelectedItem is Photo photo && Results.ContainerFromItem(photo) is FrameworkElement row &&
            new Rect(0, 0, row.ActualWidth, row.ActualHeight).Contains(e.GetPosition(row)))
        {
            OnFocusPhoto(sender, new());
            e.Handled = true;
        }
    }

    private void OnResultsKeyDown(object sender, KeyRoutedEventArgs e)
    {
        if (e.Key is VirtualKey.Space or VirtualKey.Enter && Results.SelectedItem is Photo)
        {
            OnFocusPhoto(sender, new());
            e.Handled = true;
        }
    }

    /// A click on a photo selects it, opens the inspector and flies to it
    /// (one history step per new selection, so Back and double-click return).
    private void SelectPhoto(int index, bool frame = false)
    {
        if (_library is null || index < 0 || index >= _library.Photos.Count) return;
        var photo = _library.Photos[index];
        var isNewSelection = !ReferenceEquals(ViewModel.Selected, photo);
        ViewModel.Selected = photo;
        RenderAction(renderer =>
        {
            renderer.Select(index);
            if (frame && isNewSelection) renderer.Focus(index);
        });
        _isSynchronizingSelection = true;
        Results.SelectedItem = photo;
        _isSynchronizingSelection = false;
        PhotoTitle.Text = photo.Title;
        PhotoArtist.Text = photo.Attribution;
        var date = photo.TakenAt is { } taken ? taken.ToLocalTime().ToString("d MMMM yyyy · HH:mm") : "";
        PhotoDetails.Text = string.Join("\n", new[] { photo.Department, photo.Medium, $"{photo.Width:N0} × {photo.Height:N0}", date }
            .Where(s => s.Length > 0));
        PhotoCredit.Text = photo.Credit;
        PhotoArtist.Visibility = photo.Attribution.Length > 0 ? Visibility.Visible : Visibility.Collapsed;
        PhotoCredit.Visibility = photo.Credit.Length > 0 ? Visibility.Visible : Visibility.Collapsed;
        // The preview frame takes the photo's own proportions inside a 232×150 box.
        var aspect = Math.Clamp(photo.Aspect, 0.25f, 4f);
        var frameHeight = Math.Min(150, 232 / aspect);
        PreviewFrame.Height = frameHeight;
        PreviewFrame.Width = frameHeight * aspect;
        PreviewFrame.Visibility = Visibility.Visible;
        if (isNewSelection || PreviewImage.Source is null)
        {
            ResetInspectorPreview();
            // Immediate cached image; never retain the preceding photo while decoding the next.
            PreviewImage.Source = new BitmapImage(new Uri(photo.ThumbnailPath)) { DecodePixelWidth = 256 };
        }
        InspectorEmpty.Visibility = Visibility.Collapsed;
        OpenOriginal.IsEnabled = FocusPhoto.IsEnabled = FavouriteButton.IsEnabled = true;
        UpdateFavouriteButton();
        _hasInspectorOpened = true;
        _preferences.IsInspectorVisible = true;
        _isExploreExpanded = false;
        SimilaritySelectionChanged();
        LayoutChrome();
    }

    private void ClearSelection()
    {
        ResetInspectorPreview();
        ViewModel.Selected = null;
        RenderAction(renderer => renderer.Select(-1));
        PhotoTitle.Text = PhotoArtist.Text = PhotoDetails.Text = PhotoCredit.Text = "";
        PreviewFrame.Visibility = Visibility.Collapsed;
        InspectorEmpty.Visibility = Visibility.Visible;
        OpenOriginal.IsEnabled = FocusPhoto.IsEnabled = FavouriteButton.IsEnabled = false;
        FavouriteButton.IsChecked = false;
        Results.SelectedItem = null;
        if (_isReady) SimilaritySelectionChanged();
        if (_isLoaded) LayoutChrome();
    }

    private void CancelInspectorPreview()
    {
        var cancellation = _previewCancellation;
        if (cancellation is null) return;
        _previewCancellation = null;
        _previewGeneration++;
        _previewAttemptedId = null;
        cancellation.Cancel();
        // The request owns disposal in finally, after WinRT has stopped using its token.
        // Closing/reselecting never disposes a bitmap while SetBitmapAsync is still uploading.
    }

    private void ResetInspectorPreview()
    {
        CancelInspectorPreview();
        _previewGeneration++;
        _previewAttemptedId = null;
        PreviewImage.Source = null;
        _previewSource?.Dispose();
        _previewSource = null;
        PreviewStatus.Text = "";
        PreviewStatus.Visibility = Visibility.Collapsed;
        ToolTipService.SetToolTip(PreviewStatus, null);
    }

    private void UpdateInspectorPreview()
    {
        if (!_isLoaded || !IsPanelTargetVisible(Inspector) || ViewModel.Selected is not { } photo)
        {
            CancelInspectorPreview();
            _previewAttemptedId = null;
            return;
        }
        if (_previewSource is not null || _previewAttemptedId == photo.Id) return;
        CancelInspectorPreview();
        var cancellation = new CancellationTokenSource();
        _previewCancellation = cancellation;
        _previewAttemptedId = photo.Id;
        var generation = ++_previewGeneration;
        PreviewStatus.Text = Strings.Get("Inspector_LoadingPreview");
        PreviewStatus.Visibility = Visibility.Visible;
        ToolTipService.SetToolTip(PreviewStatus, null);
        _ = LoadInspectorPreviewAsync(photo, generation, cancellation);
    }

    private bool IsCurrentPreview(Photo photo, long generation, CancellationTokenSource cancellation) =>
        _isLoaded && generation == _previewGeneration && ReferenceEquals(_previewCancellation, cancellation) &&
        !cancellation.IsCancellationRequested && ViewModel.Selected?.Id == photo.Id && IsPanelTargetVisible(Inspector);

    private async Task LoadInspectorPreviewAsync(Photo photo, long generation, CancellationTokenSource cancellation)
    {
        var token = cancellation.Token;
        var hasGate = false;
        SoftwareBitmapSource? nextSource = null;
        try
        {
            // One decode/upload at a time, plus the currently displayed source. Cancelled queued
            // selections allocate no bitmap; a rapid A→B→C cannot build up a full-resolution queue.
            await _previewGate.WaitAsync(token);
            hasGate = true;
            token.ThrowIfCancellationRequested();
            if (!IsCurrentPreview(photo, generation, cancellation)) return;
            using var bitmap = await LibraryService.PreviewAsync(photo, token);
            if (!IsCurrentPreview(photo, generation, cancellation)) return;
            nextSource = new SoftwareBitmapSource();
            // Continuation is on the UI dispatcher. Await the actual upload rather than cancelling
            // only its managed wait, so bitmap disposal cannot race native SetBitmapAsync.
            await nextSource.SetBitmapAsync(bitmap);
            if (!IsCurrentPreview(photo, generation, cancellation)) return;
            var oldSource = _previewSource;
            PreviewImage.Source = nextSource;
            _previewSource = nextSource;
            nextSource = null; // Ownership transferred to the one displayed source.
            oldSource?.Dispose();
            PreviewStatus.Visibility = Visibility.Collapsed;
        }
        catch (OperationCanceledException) when (token.IsCancellationRequested) { }
        catch (Exception error)
        {
            if (!IsCurrentPreview(photo, generation, cancellation)) return;
            // Unavailable/cloud-only originals are an inspector concern, not a renderer failure.
            // The cached thumbnail stays in place and no download/hydration is requested here.
            LibraryService.Log($"Local preview unavailable for photo {photo.Id}: {error}");
            PreviewStatus.Text = Strings.Get("Inspector_PreviewUnavailable");
            ToolTipService.SetToolTip(PreviewStatus, error.Message);
        }
        finally
        {
            nextSource?.Dispose();
            if (hasGate) _previewGate.Release();
            if (ReferenceEquals(_previewCancellation, cancellation)) _previewCancellation = null;
            cancellation.Dispose();
        }
    }

    private void OnCloseInspector(object sender, RoutedEventArgs e)
    {
        _preferences.IsInspectorVisible = false; // Closing the pane does not discard the selected photo.
        LayoutChrome();
        SavePreferences();
        CanvasHost.Focus(FocusState.Programmatic);
    }

    private void OnToggleInspector(object sender, RoutedEventArgs e)
    {
        var isVisible = IsPanelTargetVisible(Inspector);
        _hasInspectorOpened = true;
        _preferences.IsInspectorVisible = !isVisible;
        if (!isVisible) { _preferences.IsSidebarVisible = true; _isExploreExpanded = false; }
        LayoutChrome();
        SavePreferences();
    }

    private void UpdateFavouriteButton()
    {
        var isFavourite = ViewModel.Selected is { } photo && _preferences.FavouriteIds.Contains(photo.Id);
        FavouriteButton.IsChecked = isFavourite;
        FavouriteIcon.Glyph = isFavourite ? "\uEB52" : "\uEB51";
        AutomationProperties.SetName(FavouriteButton, isFavourite ? Strings.Get("Favourite_RemoveName") : Strings.Get("Favourite_AddName"));
        ToolTipService.SetToolTip(FavouriteButton, isFavourite ? Strings.Get("Favourite_RemoveTip") : Strings.Get("Favourite_AddTip"));
    }

    private void OnFavourite(object sender, RoutedEventArgs e)
    {
        if (ViewModel.Selected is not { } photo) return;
        var wasFavourite = _preferences.FavouriteIds.Contains(photo.Id);
        if (wasFavourite) _preferences.FavouriteIds.Remove(photo.Id); else _preferences.FavouriteIds.Add(photo.Id);
        try { _preferences.Save(); }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException)
        {
            if (wasFavourite) _preferences.FavouriteIds.Add(photo.Id); else _preferences.FavouriteIds.Remove(photo.Id);
            Fail(Strings.Get("Error_FavouriteSave"), error);
        }
        UpdateFavouriteButton();
        UpdateDestinationCounts();
        ApplyFilter();
    }

    private async void OnOpenOriginal(object sender, RoutedEventArgs e)
    {
        if (ViewModel.Selected is not { } photo) return;
        try
        {
            var file = await Windows.Storage.StorageFile.GetFileFromPathAsync(photo.OriginalPath);
            if (!await Launcher.LaunchFileAsync(file)) throw new IOException(Strings.Get("Error_NoAppForImage"));
        }
        catch (Exception error) { Fail(Strings.Get("Error_OriginalOpen"), error); }
    }

    private void OnFocusPhoto(object sender, RoutedEventArgs e)
    {
        if (_library is not null && ViewModel.Selected is { } photo)
            RenderAction(renderer => renderer.Focus(_library.Photos.IndexOf(photo)));
    }

    private static double DesiredHeight(Border panel, double width)
    {
        if (panel.Child is not UIElement content) return 0;
        var horizontal = panel.Padding.Left + panel.Padding.Right + panel.BorderThickness.Left + panel.BorderThickness.Right;
        var vertical = panel.Padding.Top + panel.Padding.Bottom + panel.BorderThickness.Top + panel.BorderThickness.Bottom;
        content.Measure(new Size(Math.Max(0, width - horizontal), double.PositiveInfinity));
        return content.DesiredSize.Height + vertical;
    }

    /// Geometry: one left column of floating cards from the
    /// top of the window (sidebar · Explore · photo), a toolbar row that is the caption strip,
    /// the search centred on the window and the arrangement controls against the caption buttons.
    private void LayoutChrome()
    {
        if (!_isReady || Root.ActualWidth <= 0 || Root.ActualHeight <= 0) return;
        ClearHover();
        var now = MotionNow;
        var width = Root.ActualWidth;
        var height = Root.ActualHeight;
        var scale = XamlRoot?.RasterizationScale ?? 1;
        var captionInset = App.Window is { } window ? window.AppWindow.TitleBar.RightInset / scale : 138;
        var showSidebar = _preferences.IsSidebarVisible;
        var isSidebarTransition = showSidebar != IsPanelTargetVisible(SourcePanel);

        // ---- toolbar row ----
        TrailingTools.Margin = new Thickness(0, 0, captionInset + 6, 0);
        FloatToggle.Visibility = showSidebar ? Visibility.Collapsed : Visibility.Visible;
        SidebarMenuItem.Text = showSidebar ? Strings.Get("Sidebar_Hide") : Strings.Get("Sidebar_Show");
        var identityLeft = showSidebar ? CardX + CardWidth + 18 : CardX + 40 + 14;
        ToolbarIdentity.Margin = new Thickness(identityLeft, 0, 0, 0);
        double ToolsLeft(bool compact)
        {
            SegmentHost.Visibility = compact ? Visibility.Collapsed : Visibility.Visible;
            CompactArrange.Visibility = compact ? Visibility.Visible : Visibility.Collapsed;
            TrailingTools.Measure(new Size(double.PositiveInfinity, 52));
            return width - (captionInset + 6) - TrailingTools.DesiredSize.Width;
        }
        double SearchRoom(double identityWidth, double toolsLeftEdge)
        {
            ToolbarIdentity.Visibility = identityWidth > 0 ? Visibility.Visible : Visibility.Collapsed;
            ToolbarIdentity.MaxWidth = Math.Max(identityWidth, 1);
            ToolbarIdentity.Measure(new Size(identityWidth, 52));
            var right = identityWidth > 0 ? identityLeft + ToolbarIdentity.DesiredSize.Width : identityLeft - 8;
            return toolsLeftEdge - 20 - (right + 20);
        }
        // Space is given up in order: the search narrows to a comfortable 220, then the library
        // name truncates, and only then do the arrangements fold into a menu; last, the name goes.
        const double comfortableSearch = 220;
        var toolsLeft = ToolsLeft(compact: false);
        if (SearchRoom(220, toolsLeft) < comfortableSearch && SearchRoom(140, toolsLeft) < comfortableSearch)
        {
            toolsLeft = ToolsLeft(compact: true);
            if (SearchRoom(220, toolsLeft) < comfortableSearch && SearchRoom(140, toolsLeft) < comfortableSearch)
                SearchRoom(0, toolsLeft);
        }
        var identityRight = ToolbarIdentity.Visibility == Visibility.Visible
            ? identityLeft + ToolbarIdentity.DesiredSize.Width : identityLeft - 8;
        var leftLimit = identityRight + 20;
        var rightLimit = toolsLeft - 20;
        var searchWidth = Math.Clamp(rightLimit - leftLimit, 150, 440);
        var searchX = Math.Clamp(width / 2 - searchWidth / 2, leftLimit, Math.Max(leftLimit, rightLimit - searchWidth));
        SearchChrome.Width = searchWidth;
        SearchChrome.Margin = new Thickness(searchX, 0, 0, 0);
        if (FilterChip.Visibility == Visibility.Visible)
        {
            FilterChip.Measure(new Size(double.PositiveInfinity, 30));
            FilterChip.Margin = new Thickness(searchX + searchWidth / 2 - FilterChip.DesiredSize.Width / 2, 56, 0, 0);
        }

        // ---- left column ----
        var columnBottom = height - CardGap;
        var sourceBounds = new Rect(CardX, CardTop, CardWidth, DesiredHeight(SourcePanel, CardWidth));
        var showInspector = showSidebar && _hasInspectorOpened && _preferences.IsInspectorVisible &&
            ViewModel.Selected is not null && !_isExploreExpanded;
        var showExplore = showSidebar && ViewModel.HasLibrary && _preferences.IsExploreVisible;
        var inspectorHeight = Math.Clamp(DesiredHeight(Inspector, CardWidth), 200, Math.Max(200, Math.Min(400, height * .48)));
        var inspectorBounds = new Rect(CardX, columnBottom - inspectorHeight, CardWidth, inspectorHeight);
        var exploreTop = sourceBounds.Bottom + CardGap;
        var exploreBottom = showInspector ? inspectorBounds.Top - CardGap : columnBottom;
        if (showInspector && exploreBottom - exploreTop < 150) showExplore = false;
        var exploreWidth = _isExploreExpanded ? Math.Min(540, width - 2 * CardX) : CardWidth;
        var exploreBounds = new Rect(CardX, exploreTop, exploreWidth, Math.Max(150, exploreBottom - exploreTop));
        _exploreHeight = showExplore ? exploreBounds.Height : 0;
        ExploreExpandIcon.Glyph = _isExploreExpanded ? "\uE73F" : "\uE740";
        ToolTipService.SetToolTip(ExpandExplore, _isExploreExpanded ? Strings.Get("Explore_CollapseTip") : Strings.Get("Explore_ExpandTip"));

        // ---- canvas-centred surfaces ----
        var contentLeft = showSidebar ? CardX + (showExplore && _isExploreExpanded ? exploreWidth : CardWidth) + 16 : 16;
        EmptyState.Margin = new Thickness(contentLeft, 56, 16, 40);
        var importWidth = Math.Min(380, Math.Max(220, width - contentLeft - 32));
        ImportPanel.Width = importWidth;
        ImportPanel.Margin = new Thickness(contentLeft + Math.Max(0, (width - contentLeft - importWidth) / 2), 0, 0, 20);

        var animate = _hasPanelLayout && _isLoaded && _renderer is not null &&
            _uiSettings.AnimationsEnabled && !IsHighContrast;
        var slideOffLeft = !showSidebar || isSidebarTransition;
        RetargetPanel(SourcePanel, sourceBounds, showSidebar, slideOffLeft, now, animate);
        RetargetPanel(ExplorePanel, exploreBounds, showExplore, slideOffLeft, now, animate);
        RetargetPanel(Inspector, inspectorBounds, showInspector, slideOffLeft, now, animate);
        _hasPanelLayout = true;
        ApplyPanelFrame(now);
        UpdateInspectorPreview();
        UpdateExploreContent();
    }

    private bool IsPanelTargetVisible(Border panel) => _panelMotions.TryGetValue(panel, out var motion)
        ? motion.Transition.IsVisibleTarget : panel.Visibility == Visibility.Visible;

    private void RetargetPanel(Border panel, Rect bounds, bool isVisible, bool slideOffLeft, double now, bool animate)
    {
        var hiddenX = slideOffLeft ? -bounds.Width - 24 : bounds.X - 24;
        var hidden = new PanelFrame(hiddenX, bounds.Y, bounds.Width, bounds.Height, 0);
        if (!_panelMotions.TryGetValue(panel, out var motion))
        {
            motion = new PanelMotion(panel, bounds, hidden);
            _panelMotions.Add(panel, motion);
            panel.RenderTransform = motion.Transform;
        }
        // While completely hidden, moving to a new source/window size needs no visible tween.
        // Mid-flight reversals instead sample the old trajectory before taking a new target.
        if (!motion.Transition.IsRunning && !motion.Transition.IsVisibleTarget)
            motion.Transition.Retarget(hidden, false, now, animate: false);
        motion.LayoutBounds = bounds;
        var target = isVisible ? new PanelFrame(bounds.X, bounds.Y, bounds.Width, bounds.Height, 1) : hidden;
        motion.Transition.Retarget(target, isVisible, now, animate);
        if (!isVisible) MoveFocusOutOfPanel(panel);
        panel.IsHitTestVisible = isVisible;
        // Closing remains paint-visible during its fade, but cannot accept Tab/accelerators or UIA Invoke.
        // Collapsed at completion removes its children from the control/content automation view.
        if (panel.Child is ContentControl content) content.IsEnabled = isVisible;
    }

    private void MoveFocusOutOfPanel(Border panel)
    {
        if (XamlRoot is null) return;
        var focused = FocusManager.GetFocusedElement(XamlRoot) as DependencyObject;
        while (focused is not null)
        {
            if (ReferenceEquals(focused, panel)) { CanvasHost.Focus(FocusState.Programmatic); return; }
            focused = VisualTreeHelper.GetParent(focused);
        }
    }

    private void ApplyPanelFrame(double now)
    {
        if (_isApplyingPanelFrame) return;
        _isApplyingPanelFrame = true;
        var isMoving = false;
        try
        {
            foreach (var motion in _panelMotions.Values)
            {
                var frame = motion.Transition.Sample(now);
                var panel = motion.Panel;
                panel.Margin = new Thickness(motion.LayoutBounds.X, motion.LayoutBounds.Y, 0, 0);
                panel.Width = frame.Width;
                panel.Height = frame.Height;
                motion.Transform.X = frame.X - motion.LayoutBounds.X;
                motion.Transform.Y = frame.Y - motion.LayoutBounds.Y;
                panel.Opacity = frame.Opacity;
                panel.Visibility = motion.Transition.IsVisibleTarget || motion.Transition.IsRunning
                    ? Visibility.Visible : Visibility.Collapsed;
                panel.IsHitTestVisible = motion.Transition.IsVisibleTarget && frame.Opacity > .05;
                isMoving |= motion.Transition.IsRunning;
            }
            // One dispatcher callback sets BOTH UI and native values. TransformToVisual observes
            // the XAML RenderTransform above (not an independent Composition Visual.Offset).
            // Width/Height morphs reflow the bounded list viewport rather than scaling glyphs.
            SubmitGlass();
        }
        finally { _isApplyingPanelFrame = false; }

        if (isMoving && _isLoaded)
        {
            if (_panelMotionTimer is null)
            {
                _panelMotionTimer = DispatcherQueue.CreateTimer();
                _panelMotionTimer.Interval = TimeSpan.FromMilliseconds(16);
                _panelMotionTimer.IsRepeating = true;
                _panelMotionTimer.Tick += OnPanelMotionTick;
            }
            if (!_panelMotionTimer.IsRunning) _panelMotionTimer.Start();
        }
        else _panelMotionTimer?.Stop(); // No periodic animation callback after the last deadline.
    }

    private void OnPanelMotionTick(DispatcherQueueTimer sender, object args)
    {
        if (!_isLoaded) { sender.Stop(); return; }
        ClearHover();
        ApplyPanelFrame(MotionNow);
    }

    private void CompletePanelMotion()
    {
        foreach (var motion in _panelMotions.Values) motion.Transition.Complete();
        _panelMotionTimer?.Stop();
        ApplyPanelFrame(MotionNow);
    }

    private void OnToggleSidebar(object sender, RoutedEventArgs e)
    {
        _preferences.IsSidebarVisible = !_preferences.IsSidebarVisible;
        ClearHover();
        LayoutChrome();
        SavePreferences();
        // Keep keyboard users on the control that now owns the toggle.
        if (_preferences.IsSidebarVisible) SidebarToggle.Focus(FocusState.Programmatic);
        else if (FloatToggle.Child is Control floating) floating.Focus(FocusState.Programmatic);
    }

    private void OnSidebarAccelerator(KeyboardAccelerator sender, KeyboardAcceleratorInvokedEventArgs args)
    { OnToggleSidebar(this, new()); args.Handled = true; }

    private async void OnRevealSource(object sender, RoutedEventArgs e)
    {
        if (_library is null) return;
        try { await Launcher.LaunchFolderPathAsync(_library.Source); }
        catch (Exception error) { Fail(Strings.Get("Error_FolderShow"), error); }
    }

    private void OnToggleExplore(object sender, RoutedEventArgs e)
    {
        var isVisible = IsPanelTargetVisible(ExplorePanel);
        _preferences.IsExploreVisible = !isVisible;
        if (!isVisible)
        {
            _preferences.IsSidebarVisible = true;
            if (Root.ActualHeight < 760) _isExploreExpanded = true;
        }
        LayoutChrome();
        SavePreferences();
    }

    private void OnExpandExplore(object sender, RoutedEventArgs e)
    {
        _isExploreExpanded = !_isExploreExpanded;
        LayoutChrome();
    }

    private void ApplyAppearance()
    {
        var theme = _preferences.Appearance switch
        {
            "Light" => ElementTheme.Light, "Dark" => ElementTheme.Dark, _ => ElementTheme.Default
        };
        Root.RequestedTheme = theme;
        // Includes the real Windows title strip; no forced light theme at app startup.
        if (XamlRoot?.Content is FrameworkElement windowRoot) windowRoot.RequestedTheme = theme;
    }

    private void OnAppearance(object sender, RoutedEventArgs e)
    {
        if (sender is not FrameworkElement { Tag: string name }) return;
        SystemAppearance.IsChecked = name == "System";
        LightAppearance.IsChecked = name == "Light";
        DarkAppearance.IsChecked = name == "Dark";
        _preferences.Appearance = name;
        ApplyAppearance();
        ApplyEffects();
        SavePreferences();
    }

    private void OnActualThemeChanged(FrameworkElement sender, object args) => ApplyEffects();
    private void OnEffectsChanged(UISettings sender, object args) => DispatcherQueue.TryEnqueue(ApplyEffects);
    private void OnThemeSettingsChanged(Microsoft.UI.System.ThemeSettings sender, object args) => DispatcherQueue.TryEnqueue(ApplyEffects);

    private void ApplyEffects()
    {
        if (!_isLoaded) return;
        _isGlassEnabled = _uiSettings.AdvancedEffectsEnabled && !IsHighContrast;
        VisualStateManager.GoToState(this, _isGlassEnabled ? "Glass" : "Opaque", false);
        if (!_uiSettings.AnimationsEnabled || IsHighContrast) CompletePanelMotion();
        RenderAction(renderer =>
        {
            renderer.SetTheme(Root.ActualTheme == ElementTheme.Light);
            renderer.SetReducedMotion(!_uiSettings.AnimationsEnabled);
        });
        QueueGlass(force: true);
    }

    // Native composites at most eight glass panes; order is priority (cards, then toolbar).
    private IEnumerable<Border> GlassPanels()
    {
        yield return SourcePanel;
        yield return ExplorePanel;
        yield return Inspector;
        yield return SearchChrome;
        yield return CommandChrome;
        yield return ToolsChrome;
        yield return FloatToggle;
        yield return ImportPanel;
        yield return FilterChip;
    }

    private void QueueGlass(bool force = false)
    {
        if (!_isReady || !_isLoaded) return;
        _isGlassForced |= force;
        // During a card transition the shared clock owns submission. SizeChanged raised by
        // the sampled width/height must not create a recursive queue or an extra animation clock.
        if (_isApplyingPanelFrame || _isSubmittingGlass || _panelMotionTimer?.IsRunning == true) return;
        if (_isGlassQueued) return;
        _isGlassQueued = true;
        DispatcherQueue.TryEnqueue(DispatcherQueuePriority.Low, () =>
        {
            _isGlassQueued = false;
            SubmitGlass();
        });
    }

    private void SubmitGlass()
    {
        if (!_isLoaded || _renderer is null || _isSubmittingGlass) return;
        _isSubmittingGlass = true;
        try
        {
            // Settle only this tick's sampled layout. Never subscribe to LayoutUpdated or
            // CompositionTarget.Rendering: idle glass produces no native calls.
            Root.UpdateLayout();
            var rects = new List<NativeRenderer.GlassPanel>(8);
            if (_isGlassEnabled)
            {
                foreach (var panel in GlassPanels())
                {
                    if (rects.Count == 8) break;
                    if (panel.Visibility != Visibility.Visible || panel.Opacity <= 0 ||
                        panel.ActualWidth <= 0 || panel.ActualHeight <= 0) continue;
                    var bounds = panel.TransformToVisual(Canvas).TransformBounds(new Rect(0, 0, panel.ActualWidth, panel.ActualHeight));
                    // Preserve negative origins and full dimensions. Clamping here would move
                    // the rounded corner as a card crosses the window edge; native clips the mask.
                    if (bounds.Right <= 0 || bounds.Bottom <= 0 ||
                        bounds.Left >= Canvas.ActualWidth || bounds.Top >= Canvas.ActualHeight) continue;
                    rects.Add(new()
                    {
                        X = (float)(bounds.X * Canvas.CompositionScaleX),
                        Y = (float)(bounds.Y * Canvas.CompositionScaleY),
                        Width = (float)(bounds.Width * Canvas.CompositionScaleX),
                        Height = (float)(bounds.Height * Canvas.CompositionScaleY),
                        Radius = (float)(panel.CornerRadius.TopLeft * Canvas.CompositionScaleX),
                        // Denser than .55/.68 so panel text stays legible over near-white
                        // museum photos and the plates read as frosted, not clear (native maps dark
                        // .67 to an effective .73).
                        Tint = Root.ActualTheme == ElementTheme.Light ? .78f : .67f,
                        Opacity = (float)panel.Opacity
                    });
                }
            }
            var next = rects.ToArray();
            if (_isGlassForced || !_lastGlass.SequenceEqual(next))
            {
                RenderAction(renderer => renderer.SetGlassPanels(next));
                _lastGlass = next;
            }
            _isGlassForced = false;
            UpdateTitleBarRegions();
        }
        finally { _isSubmittingGlass = false; }
    }

    private void OnGlassSizeChanged(object sender, SizeChangedEventArgs e) => QueueGlass();
    private void OnPageSizeChanged(object sender, SizeChangedEventArgs e) => LayoutChrome();
    private void OnCanvasSizeChanged(object sender, SizeChangedEventArgs e) => ResizeCanvas();
    private void OnCompositionScaleChanged(SwapChainPanel sender, object args) => ResizeCanvas();
    private void ResizeCanvas()
    {
        RenderAction(renderer => renderer.Resize((int)Math.Ceiling(Canvas.ActualWidth * Canvas.CompositionScaleX),
            (int)Math.Ceiling(Canvas.ActualHeight * Canvas.CompositionScaleY), Canvas.CompositionScaleX, Canvas.CompositionScaleY));
        QueueGlass();
    }

    private int Pick(Point point)
    {
        var index = -1;
        RenderAction(renderer => index = renderer.Pick((float)point.X * Canvas.CompositionScaleX, (float)point.Y * Canvas.CompositionScaleY));
        return index;
    }

    /// The photo under the pointer and its current level of detail (0 = a far dot).
    private (int Index, int Tier) PickWithTier(Point point)
    {
        (int Index, int Tier) result = (-1, 0);
        RenderAction(renderer => result = renderer.PickLod((float)point.X * Canvas.CompositionScaleX, (float)point.Y * Canvas.CompositionScaleY));
        return result;
    }

    /// The hover capsule with arbitrary text (a group's name), placed beside the pointer.
    private void ShowTag(string text)
    {
        HoverTagText.Text = text;
        HoverTag.Measure(new Size(360, 26));
        var x = _hoverPoint.X + 14;
        var y = _hoverPoint.Y + 20;
        if (x + HoverTag.DesiredSize.Width > Root.ActualWidth - 8) x = _hoverPoint.X - 14 - HoverTag.DesiredSize.Width;
        if (y + 26 > Root.ActualHeight - 8) y = _hoverPoint.Y - 34;
        HoverTag.Margin = new Thickness(Math.Max(8, x), Math.Max(8, y), 0, 0);
        HoverTag.Opacity = 1;
    }

    private bool IsOverChrome(Point point)
    {
        // Include transparent toolbar gaps and labels, not only the glass-backed controls.
        if (Toolbar.Visibility == Visibility.Visible &&
            Toolbar.TransformToVisual(Canvas).TransformBounds(new Rect(0, 0, Toolbar.ActualWidth, Toolbar.ActualHeight)).Contains(point))
            return true;
        return GlassPanels().Any(panel => panel.Visibility == Visibility.Visible && panel.IsHitTestVisible &&
            panel.TransformToVisual(Canvas).TransformBounds(new Rect(0, 0, panel.ActualWidth, panel.ActualHeight)).Contains(point));
    }

    /// A two-finger gesture orbits when this window is in front and the pointer is over the canvas
    /// (not a panel, menu or popup) — the same rule as wheel input.
    private bool CanStartTouchpad()
    {
        if (!_isLoaded || _renderer is null || !ViewModel.HasLibrary || _isDragging ||
            XamlRoot is null || VisualTreeHelper.GetOpenPopupsForXamlRoot(XamlRoot).Count > 0 ||
            GetForegroundWindow() != App.WindowHandle || !GetCursorPos(out var screen) || !ScreenToClient(App.WindowHandle, ref screen))
            return false;
        var point = new Point(screen.X / XamlRoot.RasterizationScale, screen.Y / XamlRoot.RasterizationScale);
        point = Root.TransformToVisual(Canvas).TransformPoint(point);
        return point.X >= 0 && point.Y >= 0 && point.X < Canvas.ActualWidth && point.Y < Canvas.ActualHeight &&
            !IsOverChrome(point);
    }

    [StructLayout(LayoutKind.Sequential)] private struct ScreenPoint { public int X, Y; }

    // Toolbar controls live in the caption strip, where WinUI does not reliably raise PointerExited
    // (moving off a control straight into the window-drag region goes to the non-client area), so
    // their built-in PointerOver state can stick. Hover is therefore computed here from the real
    // cursor: segments draw their own "Hover" layer, icon buttons use Normal/PointerOver states that
    // only this code sets. A 60 ms poll runs while the cursor is anywhere over the toolbar.
    private Control? _chromeHover;
    private DispatcherQueueTimer? _chromeHoverTimer;
    private Control[] _chromeControls = [];

    private void WatchChromeHover(Control[] controls)
    {
        _chromeControls = controls;
        foreach (var root in new UIElement[] { CommandChrome, ToolsChrome, SearchChrome, Toolbar })
            root.AddHandler(UIElement.PointerMovedEvent, new PointerEventHandler((_, _) => StartChromeHover()), handledEventsToo: true);
    }

    private void StartChromeHover()
    {
        if (_chromeHoverTimer is null)
        {
            _chromeHoverTimer = DispatcherQueue.CreateTimer();
            _chromeHoverTimer.Interval = TimeSpan.FromMilliseconds(60);
            _chromeHoverTimer.Tick += (_, _) => UpdateChromeHover();
        }
        UpdateChromeHover();
        if (!_chromeHoverTimer.IsRunning) _chromeHoverTimer.Start();
    }

    private void UpdateChromeHover()
    {
        Control? over = null;
        var anywhere = false;
        if (XamlRoot is not null && GetForegroundWindow() == App.WindowHandle &&
            GetCursorPos(out var screen) && ScreenToClient(App.WindowHandle, ref screen))
        {
            var scale = XamlRoot.RasterizationScale;
            var cursor = new Point(screen.X / scale, screen.Y / scale);
            anywhere = Toolbar.TransformToVisual(null).TransformBounds(new Rect(0, 0, Toolbar.ActualWidth, Toolbar.ActualHeight)).Contains(cursor);
            if (anywhere && VisualTreeHelper.GetOpenPopupsForXamlRoot(XamlRoot).Count == 0)
                foreach (var control in _chromeControls)
                {
                    if (control.Visibility != Visibility.Visible || !control.IsEnabled || control.ActualWidth <= 0) continue;
                    var bounds = control.TransformToVisual(null).TransformBounds(new Rect(0, 0, control.ActualWidth, control.ActualHeight));
                    if (bounds.Contains(cursor)) { over = control; break; }
                }
        }
        if (!ReferenceEquals(over, _chromeHover))
        {
            if (_chromeHover is not null) SetChromeHover(_chromeHover, false);
            if (over is not null) SetChromeHover(over, true);
            _chromeHover = over;
        }
        if (!anywhere && over is null) _chromeHoverTimer?.Stop();
    }

    private static void SetChromeHover(Control control, bool on)
    {
        // Segments and icon capsules both carry an owned "Hover" layer; a checked segment shows its pill instead.
        // Lookup via the template root: GetTemplateChild is protected, and the "Hover" name is template-scoped.
        var layer = FindName(control, "Hover") as UIElement;
        if (layer is null) return;
        layer.Opacity = on && !(control is RadioButton { IsChecked: true }) ? 1 : 0;
        static DependencyObject? FindName(DependencyObject root, string name)
        {
            for (var i = 0; i < VisualTreeHelper.GetChildrenCount(root); i++)
            {
                var child = VisualTreeHelper.GetChild(root, i);
                if (child is FrameworkElement { Name: var n } && n == name) return child;
                if (FindName(child, name) is { } found) return found;
            }
            return null;
        }
    }

    private IEnumerable<Control> ToolbarControls()
    {
        foreach (var root in new DependencyObject[] { CommandChrome, ToolsChrome, SearchChrome })
            foreach (var control in Descendants(root).OfType<Control>())
                if (control is ButtonBase) yield return control;
        static IEnumerable<DependencyObject> Descendants(DependencyObject parent)
        {
            for (var i = 0; i < VisualTreeHelper.GetChildrenCount(parent); i++)
            {
                var child = VisualTreeHelper.GetChild(parent, i);
                yield return child;
                foreach (var nested in Descendants(child)) yield return nested;
            }
        }
    }
    [DllImport("user32.dll")] private static extern nint GetForegroundWindow();
    [DllImport("user32.dll")] private static extern bool GetCursorPos(out ScreenPoint point);
    [DllImport("user32.dll")] private static extern bool ScreenToClient(nint hwnd, ref ScreenPoint point);

    private void ClearHover()
    {
        _hoverTimer?.Stop();
        HoverTag.Opacity = 0;
        if (_hoverIndex != -1) RenderAction(renderer => renderer.Hover(-1));
        _hoverIndex = -1;
        ClearGroupHover();
    }

    /// A small capsule beside the pointer naming what is under it.
    private void ShowHoverTag()
    {
        if (_hoverIndex < 0 || _isDragging || _library is null || _hoverIndex >= _library.Photos.Count) return;
        var photo = _library.Photos[_hoverIndex];
        HoverTagText.Text = string.IsNullOrWhiteSpace(photo.ArtworkDate) ? photo.Title : $"{photo.Title} · {photo.ArtworkDate}";
        HoverTag.Measure(new Size(360, 26));
        var x = _hoverPoint.X + 14;
        var y = _hoverPoint.Y + 20;
        if (x + HoverTag.DesiredSize.Width > Root.ActualWidth - 8) x = _hoverPoint.X - 14 - HoverTag.DesiredSize.Width;
        if (y + 26 > Root.ActualHeight - 8) y = _hoverPoint.Y - 34;
        // Margin, not RenderTransform: OpacityTransition puts the element in facade mode.
        HoverTag.Margin = new Thickness(Math.Max(8, x), Math.Max(8, y), 0, 0);
        HoverTag.Opacity = 1;
    }

    private void OnPanelPointerEntered(object sender, PointerRoutedEventArgs e) => ClearHover();
    private void OnPointerExited(object sender, PointerRoutedEventArgs e) => ClearHover();

    private void OnPointerPressed(object sender, PointerRoutedEventArgs e)
    {
        var point = e.GetCurrentPoint(Canvas);
        if (IsOverChrome(point.Position)) { ClearHover(); return; }
        if (point.PointerDeviceType == PointerDeviceType.Touch) return; // Native XAML manipulation owns touch.
        if (!point.Properties.IsLeftButtonPressed && !point.Properties.IsRightButtonPressed) return;
        _touchpad?.StopMomentum();
        _pressedGroup = _hoverGroup; // the hover is cleared for the drag; a click still means that group
        ClearHover();
        _isOrbitDrag = e.KeyModifiers.HasFlag(VirtualKeyModifiers.Menu) || point.Properties.IsRightButtonPressed;
        _isLeftClick = point.Properties.IsLeftButtonPressed && !_isOrbitDrag;
        _previous = _pressed = point.Position;
        _isDragging = true;
        _hasMoved = false;
        Canvas.CapturePointer(e.Pointer);
        CanvasHost.Focus(FocusState.Pointer);
        e.Handled = true;
    }

    private void OnPointerMoved(object sender, PointerRoutedEventArgs e)
    {
        var point = e.GetCurrentPoint(Canvas).Position;
        if (_isDragging)
        {
            if (!_hasMoved && Math.Abs(point.X - _pressed.X) + Math.Abs(point.Y - _pressed.Y) <= 4) return;
            _hasMoved = true;
            var dx = (float)(point.X - _previous.X);
            var dy = (float)(point.Y - _previous.Y);
            var isOrbit = _isOrbitDrag || e.KeyModifiers.HasFlag(VirtualKeyModifiers.Menu);
            RenderAction(renderer =>
            {
                // Click-drag pans one-to-one; Alt-drag or right-drag orbits.
                if (isOrbit) renderer.Orbit(dx, dy);
                else renderer.Pan(dx, dy);
            });
            _previous = point;
            e.Handled = true;
            return;
        }
        _hoverPoint = e.GetCurrentPoint(Root).Position;
        if (HoverTag.Opacity > 0) ShowHoverTag();
        if (Environment.TickCount64 - _lastHoverTick < 24) return;
        if (IsOverChrome(point)) { ClearHover(); return; }
        _lastHoverTick = Environment.TickCount64;
        EndStalePreview(CanvasPreview); // the pointer is on the canvas: no panel row or map dot is hovered
        var (index, tier) = PickWithTier(point);
        // A selected photo owns the view: everything behind it is dimmed context, not a target, so
        // only the selected photo and the neighbours it is drawn joined to respond to the cursor.
        if (ViewModel.Selected is not null && index >= 0 && !IsSelectionTarget(index)) index = -1;
        // Nothing resolved here (no photo, or only a far dot) → the group under the pointer.
        if (IsGroupHoverActive && HoverGroupAt(point, index, tier))
        {
            if (_hoverIndex != -1) { _hoverTimer?.Stop(); RenderAction(renderer => renderer.Hover(-1)); _hoverIndex = -1; }
            return;
        }
        if (_hoverGroup >= 0) { ClearGroupHover(); HoverTag.Opacity = 0; }
        if (index == _hoverIndex) return;
        ClearHover();
        _hoverIndex = index;
        RenderAction(renderer => renderer.Hover(index));
        if (index >= 0) _hoverTimer?.Start();
    }

    private void OnPointerReleased(object sender, PointerRoutedEventArgs e)
    {
        if (!_isDragging) return;
        var point = e.GetCurrentPoint(Canvas).Position;
        if (!_hasMoved && _isLeftClick && !IsOverChrome(point))
        {
            // A group from afar flies to its cluster; a photo is selected and framed;
            // empty space deselects.
            if (!ClickHoveredGroup())
            {
                var index = Pick(point);
                if (index >= 0) SelectPhoto(index, frame: true);
                else if (ViewModel.Selected is not null) ClearSelection();
                // Nothing selected: a click on empty space releases a pinned group or category
                // (the same order as Esc), so a pin never feels like a stuck hover.
                else if (IsSimilarityPanel && (_drillSuper is not null || _drillGroup is not null)) ClearSimilarityDrill();
            }
        }
        _isDragging = false;
        _pressedGroup = -1;
        Canvas.ReleasePointerCapture(e.Pointer);
        e.Handled = true;
    }

    private void OnPointerCaptureLost(object sender, PointerRoutedEventArgs e) => _isDragging = false;

    /// DirectManipulation owns claimed touchpad contacts before Windows applies scroll rails.
    /// This path retains mouse/Ctrl+wheel zoom and a logged fallback for unclaimed touchpads.
    private void OnPointerWheel(object sender, PointerRoutedEventArgs e)
    {
        ClearHover();
        var point = e.GetCurrentPoint(Canvas);
        if (IsOverChrome(point.Position)) return;
        var properties = point.Properties;
        var delta = properties.MouseWheelDelta;
        var isTouchpad = point.PointerDeviceType == PointerDeviceType.Touchpad
            || (GetPointerType(point.PointerId, out var type) && type == 5)          // PT_TOUCHPAD
            || (GetCurrentInputMessageSource(out var source) && source.DeviceType == 0x10); // IMDT_TOUCHPAD
        var isPinch = e.KeyModifiers.HasFlag(VirtualKeyModifiers.Control);
        // Fingers on the pad: the Raw Input listener is orbiting freely from the contacts, so the
        // system's axis-locked wheel copy of the same gesture is ignored. Pinch (Ctrl+wheel) zooms.
        if (!isPinch && _touchpad?.OwnsWheel == true)
        {
            e.Handled = true;
            return;
        }
        // Without the listener, precise scrolling means a touchpad, any sideways
        // scroll, or deltas that are not whole 120-unit notches. Held 400 ms so a fast flick that
        // lands on a multiple of 120 mid-gesture is not read as a zoom.
        var isPrecise = isTouchpad || properties.IsHorizontalMouseWheel || delta % 120 != 0;
        var nowTicks = Environment.TickCount64;
        if (isPrecise) _lastPreciseWheelTicks = nowTicks;
        else if (nowTicks - _lastPreciseWheelTicks < 400) isPrecise = true;
        if (_wheelLogCount < 12)
        {
            _wheelLogCount++;
            LibraryService.Log($"Wheel: device={point.PointerDeviceType} touchpad={isTouchpad} precise={isPrecise} horizontal={properties.IsHorizontalMouseWheel} delta={delta} ctrl={isPinch} rawTouchpad={_touchpad is not null}");
        }
        RenderAction(renderer =>
        {
            if (isPinch || !isPrecise) renderer.Zoom(delta);
            // Expressed as the drag that would produce the same motion: fingers right ⇒ drag right,
            // fingers down ⇒ drag down (Windows reports a sideways swipe right as negative H-wheel).
            else if (properties.IsHorizontalMouseWheel) renderer.Orbit(-delta * OrbitPerWheelUnit, 0);
            else renderer.Orbit(0, delta * OrbitPerWheelUnit);
        });
        e.Handled = true;
    }

    private void OnManipulationDelta(object sender, ManipulationDeltaRoutedEventArgs e)
    {
        if (e.PointerDeviceType == PointerDeviceType.Mouse) return;
        ClearHover();
        RenderAction(renderer =>
        {
            renderer.Orbit((float)e.Delta.Translation.X, (float)e.Delta.Translation.Y);
            if (e.Delta.Scale > 0) renderer.Zoom(MathF.Log(e.Delta.Scale) * 600);
        });
        e.Handled = true;
    }

    private void OnCanvasDoubleTapped(object sender, DoubleTappedRoutedEventArgs e)
    {
        // Double-click on a photo returns to the previous view; on empty space, the whole library.
        var point = e.GetPosition(Canvas);
        if (IsOverChrome(point)) return;
        var index = Pick(point);
        RenderAction(renderer => { if (index >= 0) renderer.Back(); else renderer.Fit(); });
        e.Handled = true;
    }

    private void OnCanvasTapped(object sender, TappedRoutedEventArgs e)
    {
        if (e.PointerDeviceType != PointerDeviceType.Touch) return;
        var point = e.GetPosition(Canvas);
        if (IsOverChrome(point)) return;
        var index = Pick(point);
        if (index >= 0) SelectPhoto(index, frame: true); else ClearSelection();
        e.Handled = true;
    }

    private async void OnCanvasKeyDown(object sender, KeyRoutedEventArgs e)
    {
        if (e.Key is >= VirtualKey.Number1 and <= VirtualKey.Number4)
        {
            e.Handled = true;
            await SetArrangementAsync((Arrangement)((int)e.Key - (int)VirtualKey.Number1));
        }
        else if (e.Key == VirtualKey.Number5 && IsSemanticReady)
        {
            e.Handled = true;
            await SetArrangementAsync(Arrangement.Similarity);
        }
        else if (e.Key is VirtualKey.Add or VirtualKey.Subtract)
        {
            RenderAction(renderer => renderer.Zoom(e.Key == VirtualKey.Add ? 120 : -120));
            e.Handled = true;
        }
        else if (e.Key == VirtualKey.Space) { OnFocusPhoto(sender, new()); e.Handled = true; }
        else if (e.Key is VirtualKey.Left or VirtualKey.Right or VirtualKey.Up or VirtualKey.Down)
        {
            RenderAction(renderer => renderer.Orbit(e.Key == VirtualKey.Left ? -24 : e.Key == VirtualKey.Right ? 24 : 0,
                e.Key == VirtualKey.Up ? -24 : e.Key == VirtualKey.Down ? 24 : 0));
            e.Handled = true;
        }
    }

    private void OnFindAccelerator(KeyboardAccelerator sender, KeyboardAcceleratorInvokedEventArgs args)
    { Search.Focus(FocusState.Keyboard); args.Handled = true; }
    private void OnOpenAccelerator(KeyboardAccelerator sender, KeyboardAcceleratorInvokedEventArgs args)
    { OnOpenFolder(this, new()); args.Handled = true; }
    private void OnFitAccelerator(KeyboardAccelerator sender, KeyboardAcceleratorInvokedEventArgs args)
    { OnFit(this, new()); args.Handled = true; }
    private void OnBackAccelerator(KeyboardAccelerator sender, KeyboardAcceleratorInvokedEventArgs args)
    { OnBack(this, new()); args.Handled = true; }
    private void OnEscapeAccelerator(KeyboardAccelerator sender, KeyboardAcceleratorInvokedEventArgs args)
    {
        if (ViewModel.Selected is not null) ClearSelection();
        else if (_drillSuper is not null || _drillGroup is not null || _gatheredLabel is not null) ClearSimilarityDrill();
        else if (_selectedFacet is not null || _scope != "All") OnClearFilter(this, new());
        else if (!string.IsNullOrEmpty(Search.Text))
        {
            Search.Text = "";
            _searchTimer?.Stop();
            ApplyFilter();
        }
        else if (_isExploreExpanded) { _isExploreExpanded = false; LayoutChrome(); }
        else RenderAction(renderer => renderer.Back());
        args.Handled = true;
    }

    private void OnFit(object sender, RoutedEventArgs e) => RenderAction(renderer => renderer.Fit());
    private void OnBack(object sender, RoutedEventArgs e) => RenderAction(renderer => renderer.Back());
    private void OnCancelImport(object sender, RoutedEventArgs e) => _import?.Cancel();
    private async void OnRescan(object sender, RoutedEventArgs e) { if (_library is not null) await ImportAsync(_library.Source); }

    private async Task ShowInformationAsync(string title, string text)
    {
        var close = new Button { Content = Strings.Get("Dialog_Close"), Style = (Style)Application.Current.Resources["QuietCapsuleStyle"], HorizontalAlignment = HorizontalAlignment.Right };
        AutomationProperties.SetAutomationId(close, "InformationClose");
        var panel = new StackPanel { Spacing = 20 };
        panel.Children.Add(new ScrollViewer
        {
            MaxHeight = 420, HorizontalScrollBarVisibility = ScrollBarVisibility.Disabled,
            Content = new TextBlock { Text = text, TextWrapping = TextWrapping.Wrap, MaxWidth = 520, FontSize = 14, LineHeight = 22 }
        });
        panel.Children.Add(close);
        var dialog = new ContentDialog { XamlRoot = XamlRoot, Title = title, Content = panel };
        close.Click += (_, _) => dialog.Hide();
        await dialog.ShowAsync();
    }

    private async void OnImportDetails(object sender, RoutedEventArgs e)
    {
        var text = _library is null ? Strings.Get("ImportDetails_NoLibrary") : Strings.Format("ImportDetails_Summary", _library.Photos.Count, _library.Skipped) +
            (_library.Errors.Count == 0 ? Strings.Get("ImportDetails_NoErrors") : string.Join("\n", _library.Errors.Take(30)));
        await ShowInformationAsync(Strings.Get("ImportDetails_Title"), text);
    }

    private async void OnControls(object sender, RoutedEventArgs e) => await ShowInformationAsync(Strings.Get("Controls_Title"),
        Strings.Get("Controls_Text"));

    private async void OnAbout(object sender, RoutedEventArgs e) => await ShowInformationAsync(Strings.Get("About_Title"),
        Strings.Get("About_Text"));
}
