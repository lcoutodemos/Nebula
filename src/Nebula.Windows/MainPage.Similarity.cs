using Microsoft.UI.Input;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Input;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Shapes;
using Nebula.Core;
using Nebula.Embed;
using Nebula_Windows.Services;
using System.Numerics;
using Windows.Foundation;
using DispatcherQueueTimer = Microsoft.UI.Dispatching.DispatcherQueueTimer;
using LayoutCanvas = Microsoft.UI.Xaml.Controls.Canvas;

namespace Nebula_Windows;

/// The Similarity view:
/// visual groups from the local image model, a live map of categories that follows the camera,
/// category rows, a pinned category's detail, canvas hover over groups, and the selected photo's
/// constellation. Hover previews a set (ring + lift, the rest recedes); click pins it.
public sealed partial class MainPage
{
    private VisualGroupSet? _visualGroups;
    private Library? _visualGroupsLibrary;
    private string _visualGroupsProgress = "";
    private int? _drillSuper;
    private int? _drillGroup;
    private string? _gatheredLabel;
    private int _hoverGroup = -1;
    private int _pressedGroup = -1;
    private Dictionary<int, float>? _pinnedHighlight;
    private NativeRenderer.Line[] _pinnedLines = [];
    private object? _previewOwner;
    private static readonly object CanvasPreview = new(), MapPreview = new();
    private Vector3[] _groupCentroids = [];
    private float[] _groupRadii = [];
    private int[] _photoGroup = [];
    private Vector3[] _categoryCentroids = [];
    private Vector3[]? _geometryPositions;
    private readonly List<(Ellipse Dot, TextBlock? Label, int Category)> _mapDots = [];
    private DispatcherQueueTimer? _mapTimer;
    private Matrix4x4 _lastMapView;
    private string _similarityPanelKey = "";

    private IReadOnlyList<VisualGroup> Categories =>
        _visualGroups is null ? [] : _visualGroups.Supers.Count > 0 ? _visualGroups.Supers : _visualGroups.Groups;

    private bool IsSimilarityPanel => _arrangement == Arrangement.Similarity && string.IsNullOrWhiteSpace(Search.Text) &&
        _scope == "All" && _selectedFacet is null;

    /// Group hover guard: groups are targets only in Similarity, with nothing gathered or searched.
    /// A selected photo owns the view: the groups behind it do not light up or name themselves
    /// under the cursor until it is deselected.
    private bool IsGroupHoverActive => IsSimilarityPanel && _gatheredLabel is null && _visualGroups is not null &&
        ViewModel.Selected is null && ReferenceEquals(_visualGroupsLibrary, _library);

    // ------------------------------------------------------------------ data

    /// After the semantic index is ready: groups.json if it matches, else the groups +
    /// categories stages on this PC (HDBSCAN on a 10-D PaCMAP, named by the engine).
    private void StartVisualGroups(Library library, EmbeddingIndex index, CancellationToken token)
    {
        _visualGroups = null;
        _visualGroupsLibrary = null;
        _visualGroupsProgress = Strings.Get("Groups_Finding");
        RefreshSimilarityPanel(force: true);
        var ids = library.Photos.Select(p => p.Id).ToArray();
        var folder = LibraryService.LibraryFolder(library.Source);
        _ = Task.Run(async () =>
        {
            await _semanticGate.WaitAsync(token);
            try
            {
                var started = System.Diagnostics.Stopwatch.StartNew();
                var progress = new Progress<string>(text => DispatcherQueue.TryEnqueue(() =>
                {
                    if (token.IsCancellationRequested) return;
                    _visualGroupsProgress = text;
                    RefreshSimilarityPanel(force: true);
                }));
                var set = await VisualGroups.LoadOrBuildAsync(index, ids, folder, progress, token);
                LibraryService.Log($"Visual groups: {set.Groups.Count} clusters, {set.Supers.Count} categories in {started.Elapsed.TotalSeconds:F1} s");
                DispatcherQueue.TryEnqueue(() =>
                {
                    if (token.IsCancellationRequested || !ReferenceEquals(library, _library)) return;
                    _visualGroups = set;
                    _visualGroupsLibrary = library;
                    _visualGroupsProgress = "";
                    _geometryPositions = null;
                    EnsureGroupGeometry();
                    RefreshSimilarityPanel(force: true);
                });
            }
            catch (OperationCanceledException) { }
            catch (Exception error)
            {
                LibraryService.Log($"Visual groups unavailable: {error}");
                DispatcherQueue.TryEnqueue(() =>
                {
                    if (token.IsCancellationRequested) return;
                    _visualGroupsProgress = Strings.Get("Groups_Failed");
                    RefreshSimilarityPanel(force: true);
                });
            }
            finally { _semanticGate.Release(); }
        });
    }

    private void ResetVisualGroups()
    {
        _visualGroups = null;
        _visualGroupsLibrary = null;
        _drillSuper = _drillGroup = null;
        _gatheredLabel = null;
        _hoverGroup = -1;
        _pinnedHighlight = null;
        _pinnedLines = [];
        _previewOwner = null;
        _groupCentroids = [];
        _groupRadii = [];
        _photoGroup = [];
        _categoryCentroids = [];
        _geometryPositions = null;
    }

    /// Group geometry: centroids and radii in the Similarity layout.
    private void EnsureGroupGeometry()
    {
        if (_visualGroups is null || _similarityPositions is not { IsCompletedSuccessfully: true } task) return;
        var positions = task.Result;
        if (ReferenceEquals(positions, _geometryPositions)) return;
        _geometryPositions = positions;
        (Vector3, float) Shape(int[] members)
        {
            var c = Vector3.Zero;
            var n = 0;
            foreach (var m in members) if (m >= 0 && m < positions.Length) { c += positions[m]; n++; }
            c /= Math.Max(n, 1);
            var r = 0f;
            foreach (var m in members) if (m >= 0 && m < positions.Length) r = MathF.Max(r, Vector3.Distance(positions[m], c));
            return (c, r);
        }
        var groups = _visualGroups.Groups.Select(g => Shape(g.Members)).ToArray();
        _groupCentroids = groups.Select(s => s.Item1).ToArray();
        _groupRadii = groups.Select(s => s.Item2).ToArray();
        _categoryCentroids = Categories.Select(g => Shape(g.Members).Item1).ToArray();
        // Photo → its cluster, from the clusters themselves (cached group files may omit the map).
        _photoGroup = new int[positions.Length];
        Array.Fill(_photoGroup, -1);
        for (var gi = 0; gi < _visualGroups.Groups.Count; gi++)
            foreach (var m in _visualGroups.Groups[gi].Members)
                if (m >= 0 && m < _photoGroup.Length && _photoGroup[m] < 0) _photoGroup[m] = gi;
    }

    // ------------------------------------------------------------------ highlight

    private void ApplyHighlight(IReadOnlyDictionary<int, float> weights, NativeRenderer.Line[] lines)
    {
        var ids = weights.Keys.ToArray();
        var values = ids.Select(i => weights[i]).ToArray();
        RenderAction(renderer =>
        {
            renderer.SetHighlight(ids.Select(id => checked((uint)id)).ToArray(), values);
            renderer.SetLines(lines);
        });
    }

    private void PreviewHighlight(IEnumerable<int> ids, object owner)
    {
        // Hover always shows what is under the pointer, even with a group pinned (ignoring hover
        // then reads as the pinned group being stuck). The pin returns on EndPreview.
        _previewOwner = owner;
        ApplyHighlight(ids.Distinct().ToDictionary(i => i, _ => 1f), []);
    }

    private void EndPreview()
    {
        _previewOwner = null;
        if (_pinnedHighlight is { } pinned) ApplyHighlight(pinned, _pinnedLines);
        else ApplyHighlight(new Dictionary<int, float>(), []);
    }

    /// A preview from the panel whose PointerExited never arrived (rows rebuilt under the pointer,
    /// or a fast exit WinUI misses) must not outlive the pointer leaving it.
    private void EndStalePreview(object? keep)
    {
        if (_previewOwner is not null && !ReferenceEquals(_previewOwner, keep)) EndPreview();
    }

    private void PinHighlight(IReadOnlyList<int> ids, IReadOnlyList<float>? weights = null, NativeRenderer.Line[]? lines = null)
    {
        var d = new Dictionary<int, float>();
        for (var k = 0; k < ids.Count; k++) d[ids[k]] = weights?[k] ?? 1;
        _pinnedHighlight = d;
        _pinnedLines = lines ?? [];
        _previewOwner = null;
        ApplyHighlight(d, _pinnedLines);
    }

    private void ClearHighlight()
    {
        _pinnedHighlight = null;
        _pinnedLines = [];
        _previewOwner = null;
        ApplyHighlight(new Dictionary<int, float>(), []);
    }

    private void FocusSet(int[] ids) { if (ids.Length > 0) RenderAction(renderer => renderer.FocusSet(ids.Select(id => checked((uint)id)).ToArray())); }

    // ------------------------------------------------------------------ pins

    private void PinSuper(int si, int[]? flyTo)
    {
        var cats = Categories;
        if (si < 0 || si >= cats.Count) return;
        if (_drillSuper == si && flyTo is null) { _drillSuper = null; ClearHighlight(); RefreshSimilarityPanel(force: true); ApplyFilter(); return; }
        _drillSuper = si;
        _drillGroup = null;
        PinHighlight(cats[si].Members);
        FocusSet(flyTo ?? cats[si].Members);
        RefreshSimilarityPanel(force: true);
        ApplyFilter();
    }

    private void PinGroup(int gi)
    {
        if (_visualGroups is null || gi < 0 || gi >= _visualGroups.Groups.Count) return;
        if (_drillGroup == gi) { _drillGroup = null; ClearHighlight(); RefreshSimilarityPanel(force: true); ApplyFilter(); return; }
        _drillGroup = gi;
        PinHighlight(_visualGroups.Groups[gi].Members);
        FocusSet(_visualGroups.Groups[gi].Members);
        RefreshSimilarityPanel(force: true);
        ApplyFilter();
    }

    private VisualGroup? DrilledSet =>
        _visualGroups is null ? null
        : _drillGroup is int g && g < _visualGroups.Groups.Count ? _visualGroups.Groups[g]
        : _drillSuper is int s && s < Categories.Count ? Categories[s] : null;

    /// Subtitle when a group is pinned: "Title · N photos · Similarity".
    private string? SimilarityStatus(int total) =>
        _arrangement == Arrangement.Similarity && DrilledSet is { } set
            ? Strings.Format("Status_PinnedGroup", set.Title, set.Size, _gatheredLabel is null ? Strings.Get("Status_PinnedSimilarity") : Strings.Get("Status_PinnedGathered")) : null;

    private void OnDrillShow(object sender, RoutedEventArgs e)
    {
        if (DrilledSet is not { } set) return;
        // Gather: the set comes to the camera, the same motion as search.
        ClearHighlight();
        if (ViewModel.Selected is not null) ClearSelection();
        _gatheredLabel = set.Title;
        var members = set.Members.Distinct().ToArray();
        RenderAction(renderer => renderer.SetResults(GatherSearchResults(members)));
        RefreshSimilarityPanel(force: true);
        ApplyFilter();
    }

    private void OnDrillFocus(object sender, RoutedEventArgs e) { if (DrilledSet is { } set) FocusSet(set.Members); }

    private void OnDrillClear(object sender, RoutedEventArgs e) => ClearSimilarityDrill();

    private void ClearSimilarityDrill()
    {
        _drillSuper = _drillGroup = null;
        if (_gatheredLabel is not null)
        {
            _gatheredLabel = null;
            RenderAction(renderer => renderer.ClearResults());
            ResetSearchPresentation(clearStructuralMask: false);
        }
        ClearHighlight();
        RefreshSimilarityPanel(force: true);
        ApplyFilter();
    }

    /// Leaving Similarity, or a new library: no group state survives into another arrangement.
    private void LeaveSimilarity()
    {
        if (_hoverGroup >= 0) { _hoverGroup = -1; HoverTag.Opacity = 0; }
        if (_drillSuper is not null || _drillGroup is not null || _gatheredLabel is not null || _pinnedHighlight is not null || _previewOwner is not null)
        {
            _drillSuper = _drillGroup = null;
            if (_gatheredLabel is not null) { _gatheredLabel = null; RenderAction(renderer => renderer.ClearResults()); ResetSearchPresentation(clearStructuralMask: false); }
            ClearHighlight();
        }
        _mapTimer?.Stop();
    }

    // ------------------------------------------------------------------ selection constellation

    private void SimilaritySelectionChanged()
    {
        var s = ViewModel.Selected is { } photo && _library is not null ? _library.Photos.IndexOf(photo) : -1;
        if (s >= 0 && _arrangement == Arrangement.Similarity && string.IsNullOrWhiteSpace(Search.Text) &&
            _visualGroups is { } set && s < set.Knn.Length)
        {
            var nb = set.Knn[s].Take(6).ToArray();
            var ids = new List<int>();
            var ws = new List<float>();
            var lines = new List<NativeRenderer.Line>();
            for (var k = 0; k < nb.Length; k++)
            {
                var w = 0.55f - 0.25f * k / 5;
                ids.Add(nb[k].Index);
                ws.Add(w);
                if (k < 3) lines.Add(new NativeRenderer.Line { A = (uint)s, B = (uint)nb[k].Index, Weight = w });
            }
            PinHighlight(ids, ws, lines.ToArray());
        }
        else if (_pinnedLines.Length > 0)
        {
            ClearHighlight();
            if (_drillGroup is int gi && _visualGroups is { } g && gi < g.Groups.Count) PinHighlight(g.Groups[gi].Members);
            else if (_drillSuper is int si && si < Categories.Count) PinHighlight(Categories[si].Members);
        }
        RefreshSimilarityPanel(force: true);
    }

    // ------------------------------------------------------------------ canvas hover over groups

    /// The group under the pointer: the group of the nearest grouped photo drawn within a small
    /// radius of the cursor. A group's projected radius is its farthest member, so one outlier
    /// makes that disc huge and testing the cursor against it lights the group up from empty space
    /// far from the cluster. Testing the photos themselves only reacts to a group the cursor is actually over.
    private int PickGroup(Point dip)
    {
        if (!IsGroupHoverActive || _renderer is null) return -1;
        EnsureGroupGeometry();
        if (_geometryPositions is not { } positions || _photoGroup.Length != positions.Length) return -1;
        var view = _renderer.GetView();
        if (view.Width <= 0 || view.Height <= 0) return -1;
        var px = (float)(dip.X * view.ScaleX);
        var py = (float)(dip.Y * view.ScaleY);
        var reach = 10 * view.ScaleX;
        var best = -1;
        var bestD = reach * reach;
        var m = view.ViewProjection;
        for (var i = 0; i < positions.Length; i++)
        {
            var g = _photoGroup[i];
            if (g < 0) continue;
            var p = positions[i];
            var w = m.M41 * p.X + m.M42 * p.Y + m.M43 * p.Z + m.M44;
            if (w <= 1e-4f) continue;
            var sx = ((m.M11 * p.X + m.M12 * p.Y + m.M13 * p.Z + m.M14) / w * 0.5f + 0.5f) * view.Width;
            var dx = sx - px;
            if (dx * dx >= bestD) continue;
            var sy = (1 - ((m.M21 * p.X + m.M22 * p.Y + m.M23 * p.Z + m.M24) / w * 0.5f + 0.5f)) * view.Height;
            var d = dx * dx + (sy - py) * (sy - py);
            if (d < bestD) { bestD = d; best = g; }
        }
        return best;
    }

    /// With a photo selected: the photo itself, or one it is highlighted with (Similarity's
    /// nearest neighbours, a pinned group). Everything else is dimmed context.
    private bool IsSelectionTarget(int index)
    {
        if (_library is null || ViewModel.Selected is not { } selected) return true;
        if (index < _library.Photos.Count && _library.Photos[index].Id == selected.Id) return true;
        return _pinnedHighlight?.ContainsKey(index) == true;
    }

    /// Called from OnPointerMoved after the photo pick. Returns true when a group owns the hover.
    private bool HoverGroupAt(Point canvasPoint, int photo, int tier)
    {
        var gi = -1;
        if (photo < 0 || tier == 0) gi = PickGroup(canvasPoint);
        if (gi != _hoverGroup)
        {
            if (gi < 0 && _hoverGroup >= 0) HoverTag.Opacity = 0; // the group's name leaves with the group
            _hoverGroup = gi;
            if (gi >= 0) PreviewHighlight(_visualGroups!.Groups[gi].Members, CanvasPreview);
            else if (ReferenceEquals(_previewOwner, CanvasPreview)) EndPreview();
        }
        if (gi < 0) return false;
        var g = _visualGroups!.Groups[gi];
        var cats = _visualGroups.Supers;
        var sup = g.SuperId >= 0 && g.SuperId < cats.Count ? cats[g.SuperId] : null;
        // An unnamed category says so, instead of a dangling " · 87".
        ShowTag(sup is not null ? $"{sup.Title} · {sup.Size:N0}" : $"{g.Title} · {g.Size:N0}");
        return true;
    }

    /// Mouse-up on a group from afar: fly to that cluster, pin its category, no photo selected.
    private bool ClickHoveredGroup()
    {
        var gi = _hoverGroup >= 0 ? _hoverGroup : _pressedGroup;
        _pressedGroup = -1;
        if (gi < 0 || _visualGroups is null || gi >= _visualGroups.Groups.Count) return false;
        _hoverGroup = -1;
        HoverTag.Opacity = 0;
        EndPreview();
        if (ViewModel.Selected is not null) ClearSelection();
        var g = _visualGroups.Groups[gi];
        if (g.SuperId >= 0 && g.SuperId < _visualGroups.Supers.Count) PinSuper(g.SuperId, flyTo: g.Members);
        else PinGroup(gi);
        return true;
    }

    private void ClearGroupHover()
    {
        if (_hoverGroup < 0) return;
        _hoverGroup = -1;
        if (ReferenceEquals(_previewOwner, CanvasPreview)) EndPreview();
    }

    // ------------------------------------------------------------------ panel

    private void RefreshSimilarityPanel(bool force = false)
    {
        if (!_isReady) return;
        var show = IsSimilarityPanel && _library is not null;
        SimilarityView.Visibility = show ? Visibility.Visible : Visibility.Collapsed;
        if (!show) { _mapTimer?.Stop(); return; }
        var selected = ViewModel.Selected?.Id ?? "";
        var key = $"{_visualGroups?.GetHashCode()}|{_drillSuper}|{_drillGroup}|{_gatheredLabel}|{_isExploreExpanded}|{selected}|{_visualGroupsProgress}|{ActualTheme}";
        if (!force && key == _similarityPanelKey) return;
        _similarityPanelKey = key;
        // Rebuilding removes the row under the pointer without a PointerExited.
        if (_previewOwner is Button) EndPreview();
        BuildSimilarityPanel();
    }

    private (string Title, string Description) SimilarityHeader()
    {
        if (_visualGroups is not { } set || _library is null) return (Strings.Get("Groups_Title"), _visualGroupsProgress.Length > 0 ? _visualGroupsProgress : Strings.Get("Groups_NeedsModel"));
        var cats = Categories;
        var photoSet = set.Supers.Count > 0 ? set.PhotoSuper : set.PhotoGroup;
        var ungrouped = photoSet.Count(v => v < 0);
        var n = _library.Photos.Count;
        return (Strings.Get("Groups_Title"), Strings.Format("Groups_Description", cats.Count, set.Groups.Count, n - ungrouped, n));
    }

    private void BuildSimilarityPanel()
    {
        var expanded = _isExploreExpanded;
        GroupsPending.Visibility = _visualGroups is null ? Visibility.Visible : Visibility.Collapsed;
        GroupsPending.Text = _visualGroups is null
            ? (_visualGroupsProgress.Length > 0 ? char.ToUpper(_visualGroupsProgress[0]) + _visualGroupsProgress[1..] : Strings.Get("Groups_Pending"))
            : "";
        BuildNearest(expanded);
        var hasSel = NearestSection.Visibility == Visibility.Visible;
        GroupMapFrame.Height = expanded ? 220 : hasSel ? 96 : 124;
        BuildMap(expanded);
        BuildCategoryRows(expanded);
        BuildDrill(expanded);
        BuildBridges(expanded);
        if (_visualGroups is not null) StartMapTimer();
    }

    private Border Thumb(int index, double size, double radius)
    {
        var border = new Border
        {
            Width = size, Height = size, CornerRadius = new CornerRadius(radius),
            Background = (Brush)Application.Current.Resources["ThumbPlaceholderBrush"],
        };
        if (_library is not null && index >= 0 && index < _library.Photos.Count)
            border.Child = new Border
            {
                CornerRadius = new CornerRadius(radius),
                Background = new ImageBrush { ImageSource = Thumb(_library.Photos[index].ThumbnailPath), Stretch = Stretch.UniformToFill },
            };
        return border;
    }

    private static Brush Mono(double alpha)
    {
        var ink = (Application.Current.Resources["TextFillColorPrimaryBrush"] as SolidColorBrush)?.Color ?? Microsoft.UI.Colors.White;
        return new SolidColorBrush(Windows.UI.Color.FromArgb((byte)Math.Round(255 * alpha), ink.R, ink.G, ink.B));
    }

    private void BuildNearest(bool expanded)
    {
        NearestGrid.Children.Clear();
        NearestGrid.ColumnDefinitions.Clear();
        NearestGrid.RowDefinitions.Clear();
        var s = ViewModel.Selected is { } photo && _library is not null ? _library.Photos.IndexOf(photo) : -1;
        if (s < 0 || _visualGroups is not { } set || s >= set.Knn.Length) { NearestSection.Visibility = Visibility.Collapsed; return; }
        NearestSection.Visibility = Visibility.Visible;
        const int perRow = 6;
        for (var c = 0; c < perRow; c++) NearestGrid.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        var nb = set.Knn[s].Take(expanded ? 12 : 6).ToArray();
        for (var r = 0; r < (nb.Length + perRow - 1) / perRow; r++) NearestGrid.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
        for (var k = 0; k < nb.Length; k++)
        {
            var (j, sim) = nb[k];
            var cell = new StackPanel { Spacing = 4 };
            var thumb = new Button
            {
                Padding = new Thickness(0), MinWidth = 0, MinHeight = 0, CornerRadius = new CornerRadius(6),
                Style = (Style)Application.Current.Resources["SubtleButtonStyle"], HorizontalAlignment = HorizontalAlignment.Stretch,
                Content = new Viewbox { Stretch = Stretch.UniformToFill, Child = Thumb(j, 34, 5) },
            };
            ToolTipService.SetToolTip(thumb, Strings.Format("Nearest_SimilarityTip", sim));
            Microsoft.UI.Xaml.Automation.AutomationProperties.SetName(thumb, Strings.Format("Nearest_ThumbName", _library!.Photos[j].Title, sim));
            var target = j;
            thumb.Click += (_, _) => SelectPhoto(target, frame: true);
            cell.Children.Add(thumb);
            var bar = new Grid { Height = 3 };
            bar.Children.Add(new Border { CornerRadius = new CornerRadius(1.5), Background = Mono(0.07) });
            bar.Children.Add(new Border
            {
                CornerRadius = new CornerRadius(1.5), Background = Mono(0.7), RenderTransformOrigin = new Point(0, 0.5),
                RenderTransform = new ScaleTransform { ScaleX = Math.Clamp((sim - 0.2) / 0.6, 0.1, 1) },
            });
            cell.Children.Add(bar);
            Grid.SetColumn(cell, k % perRow);
            Grid.SetRow(cell, k / perRow);
            NearestGrid.Children.Add(cell);
        }
    }

    // ---- the map: categories as bubbles at their projected positions (they follow the cloud) ----

    private void BuildMap(bool expanded)
    {
        GroupMap.Children.Clear();
        _mapDots.Clear();
        _lastMapView = default;
        if (_visualGroups is null) return;
        EnsureGroupGeometry();
        var cats = Categories;
        var take = Math.Min(cats.Count, expanded ? 200 : 100);
        // Drawn smallest first; the pinned one last, on top.
        var order = Enumerable.Range(0, take).OrderBy(i => _drillSuper == i ? 1 : 0).ThenBy(i => cats[i].Size).ToArray();
        foreach (var i in order)
        {
            var g = cats[i];
            var pinned = _drillSuper == i;
            var r = 2.0 + Math.Sqrt(g.Size) * (expanded ? 0.22 : 0.16);
            var dot = new Ellipse
            {
                Width = 2 * r, Height = 2 * r,
                Fill = pinned ? Mono(0.9) : Mono(0.22), Stroke = pinned ? null : Mono(0.25), StrokeThickness = 1,
                Tag = i,
            };
            ToolTipService.SetToolTip(dot, $"{g.Title} · {g.Size:N0}");
            GroupMap.Children.Add(dot);
            TextBlock? label = null;
            if (pinned || (expanded && i < 12))
            {
                label = new TextBlock
                {
                    Text = g.ShortName, FontSize = 10, MaxWidth = 96, TextTrimming = TextTrimming.CharacterEllipsis, IsHitTestVisible = false,
                    FontWeight = pinned ? Microsoft.UI.Text.FontWeights.SemiBold : Microsoft.UI.Text.FontWeights.Medium,
                    Foreground = pinned ? (Brush)Application.Current.Resources["TextFillColorPrimaryBrush"] : (Brush)Application.Current.Resources["TextFillColorSecondaryBrush"],
                };
                GroupMap.Children.Add(label);
            }
            _mapDots.Add((dot, label, i));
        }
        UpdateMap(force: true);
    }

    private void StartMapTimer()
    {
        if (_mapTimer is null)
        {
            _mapTimer = DispatcherQueue.CreateTimer();
            _mapTimer.Interval = TimeSpan.FromMilliseconds(100);   // the map redraws about every 6th frame
            _mapTimer.Tick += (_, _) => UpdateMap(force: false);
        }
        if (!_mapTimer.IsRunning) _mapTimer.Start();
    }

    private void OnGroupMapSizeChanged(object sender, SizeChangedEventArgs e) => UpdateMap(force: true);

    private void UpdateMap(bool force)
    {
        if (_renderer is null || _mapDots.Count == 0 || SimilarityView.Visibility != Visibility.Visible || !IsPanelTargetVisible(ExplorePanel)) return;
        var view = _renderer.GetView();
        if (!force && view.ViewProjection == _lastMapView) return;
        _lastMapView = view.ViewProjection;
        var w = GroupMap.ActualWidth;
        var h = GroupMap.ActualHeight;
        if (w <= 0 || h <= 0) return;
        // Project with the map's own aspect: the same camera, shaped to the map box.
        var aspectFix = view.Height > 0 && h > 0 ? (float)((view.Width / view.Height) / (w / h)) : 1f;
        var projected = new List<(int Slot, float X, float Y)>();
        for (var k = 0; k < _mapDots.Count; k++)
        {
            var c = _mapDots[k].Category < _categoryCentroids.Length ? _categoryCentroids[_mapDots[k].Category] : Vector3.Zero;
            var m = view.ViewProjection;
            var cx = m.M11 * c.X + m.M12 * c.Y + m.M13 * c.Z + m.M14;
            var cy = m.M21 * c.X + m.M22 * c.Y + m.M23 * c.Z + m.M24;
            var cw = m.M41 * c.X + m.M42 * c.Y + m.M43 * c.Z + m.M44;
            if (cw <= 0) { _mapDots[k].Dot.Visibility = Visibility.Collapsed; if (_mapDots[k].Label is { } l0) l0.Visibility = Visibility.Collapsed; continue; }
            projected.Add((k, cx / cw * aspectFix, cy / cw));
        }
        var mags = projected.SelectMany(p => new[] { MathF.Abs(p.X), MathF.Abs(p.Y) }).OrderBy(v => v).ToArray();
        var maxR = MathF.Max(0.05f, mags.Length == 0 ? 0.05f : mags[Math.Min(mags.Length - 1, (int)(mags.Length * 0.85))]);
        var placed = new List<Rect>();
        foreach (var (slot, x, y) in projected)
        {
            var (dot, label, category) = _mapDots[slot];
            var r = dot.Width / 2;
            var px = w / 2 + Math.Clamp(x / maxR, -1, 1) * (w / 2 - 10);
            var py = h / 2 - Math.Clamp(y / maxR, -1, 1) * (h / 2 - 10);
            dot.Visibility = Visibility.Visible;
            LayoutCanvas.SetLeft(dot, px - r);
            LayoutCanvas.SetTop(dot, py - r);
            if (label is null) continue;
            label.Measure(new Size(96, 20));
            var lw = Math.Min(96, Math.Max(label.DesiredSize.Width, label.Text.Length * 5.6));
            // Right of the dot when it fits, else left of it, so a label never runs off the map.
            var lx = px + r + 3 + lw <= w - 4 ? px + r + 3 : px - r - 3 - lw;
            var lr = new Rect(Math.Max(4, lx), py - 7, lw, 14);
            var pinned = _drillSuper == category;
            var free = pinned || !placed.Any(q => q.X < lr.X + lr.Width && lr.X < q.X + q.Width && q.Y < lr.Y + lr.Height && lr.Y < q.Y + q.Height);
            label.Visibility = free ? Visibility.Visible : Visibility.Collapsed;
            if (!free) continue;
            LayoutCanvas.SetLeft(label, lr.X);
            LayoutCanvas.SetTop(label, lr.Y - 2);
            placed.Add(lr);
        }
    }

    private int MapDotAt(Point p)
    {
        for (var k = _mapDots.Count - 1; k >= 0; k--)
        {
            var dot = _mapDots[k].Dot;
            if (dot.Visibility != Visibility.Visible) continue;
            var r = Math.Max(dot.Width / 2, 6);
            var cx = LayoutCanvas.GetLeft(dot) + dot.Width / 2;
            var cy = LayoutCanvas.GetTop(dot) + dot.Height / 2;
            if ((p.X - cx) * (p.X - cx) + (p.Y - cy) * (p.Y - cy) <= r * r) return _mapDots[k].Category;
        }
        return -1;
    }

    private int _mapHover = -1;

    private void OnGroupMapPointerMoved(object sender, PointerRoutedEventArgs e)
    {
        var hit = MapDotAt(e.GetCurrentPoint(GroupMap).Position);
        if (hit == _mapHover) return;
        _mapHover = hit;
        GroupMap.Background = new SolidColorBrush(Microsoft.UI.Colors.Transparent);
        ProtectedCursor = hit >= 0 ? InputSystemCursor.Create(InputSystemCursorShape.Hand) : null;
        if (hit >= 0) PreviewHighlight(Categories[hit].Members, MapPreview);
        else if (ReferenceEquals(_previewOwner, MapPreview)) EndPreview();
    }

    private void OnGroupMapPointerExited(object sender, PointerRoutedEventArgs e)
    {
        if (_mapHover < 0) return;
        _mapHover = -1;
        ProtectedCursor = null;
        if (ReferenceEquals(_previewOwner, MapPreview)) EndPreview();
    }

    private void OnGroupMapTapped(object sender, TappedRoutedEventArgs e)
    {
        var hit = MapDotAt(e.GetPosition(GroupMap));
        if (hit < 0) return;
        _mapHover = -1;
        if (_visualGroups!.Supers.Count > 0) PinSuper(hit, flyTo: null); else PinGroup(hit);
        e.Handled = true;
    }

    // ---- rows: categories, the pinned category's clusters, connected groups, bridges ----

    private Button GroupRow(VisualGroup g, double thumb, bool pinned, double fraction, bool showBar, Action click, string tip)
    {
        var grid = new Grid { ColumnSpacing = 10 };
        grid.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(thumb) });
        grid.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        grid.Children.Add(Thumb(g.Representative, thumb, thumb >= 30 ? 6 : 5));
        var text = new Grid { RowSpacing = 6, ColumnSpacing = 8, VerticalAlignment = VerticalAlignment.Center };
        text.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        text.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        text.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
        if (showBar) text.RowDefinitions.Add(new RowDefinition { Height = new GridLength(3) });
        var weight = pinned ? Microsoft.UI.Text.FontWeights.SemiBold : Microsoft.UI.Text.FontWeights.Normal;
        var title = new TextBlock { FontSize = 13, TextTrimming = TextTrimming.CharacterEllipsis, FontWeight = weight, TextWrapping = TextWrapping.NoWrap };
        if (g.IsNamed || g.Hint.Count == 0) title.Text = g.Title;
        else
        {
            // "Mixed" is the claim; what it is made of is evidence, in grey.
            title.Inlines.Add(new Microsoft.UI.Xaml.Documents.Run { Text = Strings.Get("Group_Mixed") });
            title.Inlines.Add(new Microsoft.UI.Xaml.Documents.Run
            {
                Text = " · " + string.Join(", ", g.Hint) + (g.Qualifier.Length == 0 ? "" : " · " + g.Qualifier),
                Foreground = (Brush)Application.Current.Resources["TextFillColorSecondaryBrush"], FontWeight = Microsoft.UI.Text.FontWeights.Normal,
            });
        }
        text.Children.Add(title);
        var count = new TextBlock
        {
            Text = g.Size.ToString("N0"), FontSize = 12, VerticalAlignment = VerticalAlignment.Bottom, Margin = new Thickness(0, 0, 2, 0),
            Foreground = (Brush)Application.Current.Resources["TextFillColorSecondaryBrush"],
        };
        Grid.SetColumn(count, 1);
        text.Children.Add(count);
        if (showBar)
        {
            var track = new Border { CornerRadius = new CornerRadius(1.5), Background = (Brush)Application.Current.Resources["BarTrackBrush"] };
            var fill = new Border
            {
                CornerRadius = new CornerRadius(1.5), RenderTransformOrigin = new Point(0, 0.5),
                Background = pinned ? Mono(0.75) : (Brush)Application.Current.Resources["BarFillBrush"],
                RenderTransform = new ScaleTransform { ScaleX = Math.Clamp(fraction, 0.02, 1) },
            };
            foreach (var b in new[] { track, fill }) { Grid.SetRow(b, 1); Grid.SetColumnSpan(b, 2); text.Children.Add(b); }
        }
        Grid.SetColumn(text, 1);
        grid.Children.Add(text);
        var button = new Button
        {
            Style = (Style)Application.Current.Resources["RowButtonStyle"], Content = grid, Margin = new Thickness(4, 0, 4, 0),
            Padding = new Thickness(8, 5, 8, 5), CornerRadius = new CornerRadius(10),
        };
        if (pinned) button.Background = (Brush)Application.Current.Resources["RowSelectedBrush"];
        if (tip.Length > 0) ToolTipService.SetToolTip(button, tip);
        Microsoft.UI.Xaml.Automation.AutomationProperties.SetName(button, Strings.Format("Group_RowName", g.Title, g.Size));
        var members = g.Members;
        button.PointerEntered += (_, _) => PreviewHighlight(members, button);
        button.PointerExited += (_, _) => { if (ReferenceEquals(_previewOwner, button)) EndPreview(); };
        button.Click += (_, _) => click();
        return button;
    }

    private void BuildCategoryRows(bool expanded)
    {
        CategoryRows.Children.Clear();
        MoreCategories.Visibility = Visibility.Collapsed;
        if (_visualGroups is null) return;
        var cats = Categories;
        var rows = expanded ? cats.Count : Math.Min(cats.Count, _drillSuper is null ? 8 : 10);
        var top = Math.Max(1, cats.Count > 0 ? cats[0].Size : 1);
        for (var i = 0; i < rows; i++)
        {
            var index = i;
            var g = cats[i];
            var row = GroupRow(g, 30, _drillSuper == i, (double)g.Size / top, showBar: true,
                () => { if (_visualGroups!.Supers.Count > 0) PinSuper(index, flyTo: null); else PinGroup(index); },
                string.Join(", ", g.Tags.Select(t => t.Term)));
            Microsoft.UI.Xaml.Automation.AutomationProperties.SetAutomationId(row, $"Category_{i}");
            CategoryRows.Children.Add(row);
        }
        if (!expanded && cats.Count > rows)
        {
            MoreCategories.Text = Strings.Format("Categories_More", cats.Count - rows);
            MoreCategories.Visibility = Visibility.Visible;
        }
    }

    private void BuildDrill(bool expanded)
    {
        DrillLeaves.Children.Clear();
        LinkRows.Children.Clear();
        DrillLeavesTitle.Visibility = DrillLeaves.Visibility = LinksTitle.Visibility = LinkRows.Visibility = Visibility.Collapsed;
        if (_visualGroups is not { } set || DrilledSet is not { } g) { DrillSection.Visibility = Visibility.Collapsed; return; }
        DrillSection.Visibility = Visibility.Visible;
        DrillTitle.Text = g.Title.ToUpperInvariant();
        var tags = string.Join(", ", g.Tags.Take(3).Select(t => t.Term));
        if (_drillGroup is int gi)
        {
            DrillSummary.Text = Strings.Format("Drill_GroupSummary", g.Size, g.Coherence) + (tags.Length > 0 ? " · " + tags : "");
            if (expanded)
            {
                foreach (var (a, b, n) in set.Links.Where(l => l.A == gi || l.B == gi).Take(5))
                {
                    var other = a == gi ? b : a;
                    if (other < 0 || other >= set.Groups.Count) continue;
                    var o = set.Groups[other];
                    var row = GroupRow(o, 22, false, 0, showBar: false, () => PinGroup(other), Strings.Format("Drill_Links", n));
                    LinkRows.Children.Add(row);
                }
                LinksTitle.Visibility = LinkRows.Visibility = LinkRows.Children.Count > 0 ? Visibility.Visible : Visibility.Collapsed;
            }
        }
        else
        {
            DrillSummary.Text = Strings.Format("Drill_CategorySummary", g.Size, g.Leaves.Length) + (tags.Length > 0 ? " · " + tags : "");
            if (expanded)
            {
                foreach (var lid in g.Leaves.Take(40))
                {
                    if (lid < 0 || lid >= set.Groups.Count) continue;
                    var leaf = set.Groups[lid];
                    var members = leaf.Members;
                    DrillLeaves.Children.Add(GroupRow(leaf, 22, false, 0, showBar: false, () => FocusSet(members), ""));
                }
                DrillLeavesTitle.Visibility = DrillLeaves.Visibility = DrillLeaves.Children.Count > 0 ? Visibility.Visible : Visibility.Collapsed;
            }
        }
    }

    private void BuildBridges(bool expanded)
    {
        BridgeThumbs.Children.Clear();
        if (!expanded || _visualGroups is not { } set || set.Bridges.Length == 0) { BridgeSection.Visibility = Visibility.Collapsed; return; }
        BridgeSection.Visibility = Visibility.Visible;
        foreach (var b in set.Bridges.Take(8))
        {
            var target = b;
            var button = new Button
            {
                Padding = new Thickness(0), MinWidth = 0, MinHeight = 0, CornerRadius = new CornerRadius(6),
                Style = (Style)Application.Current.Resources["SubtleButtonStyle"], Content = Thumb(b, 44, 6),
            };
            Microsoft.UI.Xaml.Automation.AutomationProperties.SetName(button, _library!.Photos[b].Title);
            button.Click += (_, _) => SelectPhoto(target, frame: true);
            BridgeThumbs.Children.Add(button);
        }
    }
}
