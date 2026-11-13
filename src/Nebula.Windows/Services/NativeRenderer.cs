using Microsoft.UI.Xaml.Controls;
using Nebula.Core;
using System.Numerics;
using System.Runtime.InteropServices;

namespace Nebula_Windows.Services;

public sealed partial class NativeRenderer : IDisposable
{
    private nint _handle;

    [StructLayout(LayoutKind.Sequential)]
    private struct Item
    {
        public uint Id;
        public float X, Y, Z, Aspect, Red, Green, Blue;
        public nint ImagePath;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct ItemV2
    {
        public Item Item;
        public nint OriginalPath;
        public ulong ContentVersion;
    }

    [StructLayout(LayoutKind.Sequential, Pack = 8)]
    public struct FocusStats
    {
        public uint Size;
        public int Id;
        public uint SourceWidth, SourceHeight, TextureWidth, TextureHeight;
        public uint RoiX, RoiY, RoiWidth, RoiHeight, State;
        public float DecodeMs;
        public ulong GpuBytes;
        public int Error;
        public uint Pending;
    }

    public unsafe FocusStats GetFocusStats()
    {
        var result = new FocusStats { Size = (uint)Marshal.SizeOf<FocusStats>() };
        Check(GetFocusStatsNative(_handle, &result));
        return result;
    }

    public void SetDepthOptions(bool atmosphere, bool focusDof) =>
        Check(SetDepthOptionsNative(_handle, atmosphere ? 1 : 0, focusDof ? 1 : 0));

    [StructLayout(LayoutKind.Sequential, Pack = 8)]
    public struct FrameDiagnostics
    {
        public uint Size, StateFlags;
        public float PrepareMs, OrderMs, UploadMs, SubmitMs;
        public ulong InstanceUploadBytes, DensityUploadBytes, Tiles, Dots;
    }

    public unsafe FrameDiagnostics GetFrameDiagnostics()
    {
        var result = new FrameDiagnostics { Size = (uint)Marshal.SizeOf<FrameDiagnostics>() };
        Check(GetFrameDiagnosticsNative(_handle, &result));
        return result;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct Stats
    {
        public uint Size, Items, Resident, Pending, Failed;
        public float Fps;
        public ulong Frames;
        public readonly uint DecodeFailures => Failed;
    }

    // nb_set_glass: six packed floats, DRAWABLE pixels, at most eight rectangles.
    // Native owns the blur, rounded mask and rim; XAML adds only a near-transparent veil.
    [StructLayout(LayoutKind.Sequential)]
    public struct GlassRect : IEquatable<GlassRect>
    {
        public float X, Y, Width, Height, Radius, Tint;
        public readonly bool Equals(GlassRect other) =>
            X == other.X && Y == other.Y && Width == other.Width && Height == other.Height &&
            Radius == other.Radius && Tint == other.Tint;
    }

    public unsafe void SetGlass(GlassRect[] rects)
    {
        if (rects.Length > 8) throw new ArgumentOutOfRangeException(nameof(rects));
        fixed (GlassRect* pointer = rects) Check(SetGlassNative(_handle, pointer, rects.Length));
    }

    // Back-compatible extension: NbGlassPanel is 28 bytes, including the SAME opacity
    // applied to the XAML panel on the current animation tick. Negative origins are valid.
    [StructLayout(LayoutKind.Sequential, Pack = 4)]
    public struct GlassPanel : IEquatable<GlassPanel>
    {
        public float X, Y, Width, Height, Radius, Tint, Opacity;
        public readonly bool Equals(GlassPanel other) =>
            X == other.X && Y == other.Y && Width == other.Width && Height == other.Height &&
            Radius == other.Radius && Tint == other.Tint && Opacity == other.Opacity;
    }

    public unsafe void SetGlassPanels(GlassPanel[] panels)
    {
        if (panels.Length > 8) throw new ArgumentOutOfRangeException(nameof(panels));
        fixed (GlassPanel* pointer = panels) Check(SetGlassPanelsNative(_handle, pointer, panels.Length));
    }

    public void Hover(int id) => Check(HoverNative(_handle, id));
    public void Focus(int id) => Check(FocusNative(_handle, id));
    public void Back() => Check(BackNative(_handle));
    public void SetReducedMotion(bool reduced) => Check(SetReducedMotionNative(_handle, reduced ? 1 : 0));
    public void SetPixels(int level) => Check(SetPixelsNative(_handle, Math.Clamp(level, 0, 5)));

    // NbResult ABI: uint32 id + four float32 values, exactly 20 bytes on ARM64 and x64.
    [StructLayout(LayoutKind.Sequential, Pack = 4)]
    public struct Result
    {
        public uint Id;
        public float X, Y, Z, Weight;
    }

    public unsafe void SetResults(Result[] results)
    {
        // count=0 is an active query with no matches, not a request to clear the query.
        fixed (Result* pointer = results) Check(SetResultsNative(_handle, pointer, results.Length));
    }

    public void ClearResults() => Check(ClearResultsNative(_handle));

    public unsafe void SetHighlight(uint[] ids, float[] weights)
    {
        ArgumentNullException.ThrowIfNull(ids);
        ArgumentNullException.ThrowIfNull(weights);
        if (ids.Length != weights.Length) throw new ArgumentException("Each highlighted ID needs one weight.", nameof(weights));
        fixed (uint* idPointer = ids)
        fixed (float* weightPointer = weights)
            Check(SetHighlightNative(_handle, idPointer, weightPointer, ids.Length));
    }

    [StructLayout(LayoutKind.Sequential, Pack = 4)]
    public struct Line
    {
        public uint A, B;
        public float Weight;
    }

    public unsafe void SetLines(Line[] lines)
    {
        ArgumentNullException.ThrowIfNull(lines);
        if (lines.Length > 512) throw new ArgumentOutOfRangeException(nameof(lines));
        fixed (Line* pointer = lines) Check(SetLinesNative(_handle, pointer, lines.Length));
    }

    public unsafe void FocusSet(uint[] ids)
    {
        ArgumentNullException.ThrowIfNull(ids);
        fixed (uint* pointer = ids) Check(FocusSetNative(_handle, pointer, ids.Length));
    }

    [StructLayout(LayoutKind.Sequential, Pack = 4)]
    public struct View
    {
        public uint Size;
        // Row-major storage for column-vector multiplication. With System.Numerics use
        // Vector4.Transform(world, Matrix4x4.Transpose(ViewProjection)).
        public Matrix4x4 ViewProjection;
        public float Width, Height, ScaleX, ScaleY, PixelsPerUnitAtDistance1;
    }

    public unsafe View GetView()
    {
        var result = new View { Size = (uint)Marshal.SizeOf<View>() };
        Check(GetViewNative(_handle, &result));
        return result; // Width/Height are zero before the first presented frame.
    }

    public (int Id, int Tier) PickLod(float xPixels, float yPixels)
    {
        Check(PickLodNative(_handle, xPixels, yPixels, out var id, out var tier));
        return (id, tier);
    }

    [StructLayout(LayoutKind.Sequential, Pack = 8)]
    public struct LodStats
    {
        public uint Size, Items, Visible, Dots, Mosaics, Thumbnails, Previews, MosaicReady;
        public uint ThumbnailResident, PreviewResident, Pending, Failed;
        public float Cap0, Cap1, Cap2, CpuMs, GpuMs, FrameMs, PickMs;
        public uint MosaicPending;
        public ulong Frames, GpuFrame, Flips, Uploads, Evictions, GpuBytes;
        public readonly uint DecodeFailures => Failed;
    }

    public unsafe LodStats GetLodStats()
    {
        var result = new LodStats { Size = (uint)Marshal.SizeOf<LodStats>() };
        Check(GetLodStatsNative(_handle, &result));
        return result;
    }

    public NativeRenderer(SwapChainPanel panel)
    {
        var inspectable = WinRT.MarshalInspectable<SwapChainPanel>.FromManaged(panel);
        try { Check(Create(inspectable, out _handle)); }
        finally { Marshal.Release(inspectable); }
    }

    private static void Check(int result)
    {
        if (result < 0) Marshal.ThrowExceptionForHR(result);
    }

    public unsafe void SetScene(IReadOnlyList<Photo> photos, Vector3[] positions)
    {
        var items = new ItemV2[photos.Count];
        try
        {
            for (var i = 0; i < items.Length; i++)
            {
                var photo = photos[i]; var point = positions[i];
                items[i].Item = new Item
                {
                    Id = (uint)i, X = point.X, Y = point.Y, Z = point.Z, Aspect = photo.Aspect,
                    Red = photo.Red, Green = photo.Green, Blue = photo.Blue,
                    ImagePath = Marshal.StringToCoTaskMemUni(photo.ThumbnailPath)
                };
                items[i].OriginalPath = Marshal.StringToCoTaskMemUni(photo.OriginalPath);
                var version = unchecked((ulong)photo.SourceWriteUtcTicks ^ ((ulong)photo.SourceBytes * 1099511628211UL));
                foreach (var character in photo.MetadataFingerprint) version = unchecked((version ^ character) * 1099511628211UL);
                items[i].ContentVersion = version;
            }
            fixed (ItemV2* pointer = items) Check(SetSceneV2Native(_handle, pointer, items.Length));
        }
        finally
        {
            foreach (var item in items)
            {
                if (item.Item.ImagePath != 0) Marshal.FreeCoTaskMem(item.Item.ImagePath);
                if (item.OriginalPath != 0) Marshal.FreeCoTaskMem(item.OriginalPath);
            }
        }
    }

    public unsafe void SetPositions(Vector3[] positions)
    {
        fixed (Vector3* pointer = positions) Check(SetPositionsNative(_handle, (float*)pointer, positions.Length));
    }

    public unsafe void SetFilter(byte[] mask)
    {
        fixed (byte* pointer = mask) Check(SetFilterNative(_handle, pointer, mask.Length));
    }

    public void Resize(int width, int height, float xScale, float yScale) => Check(ResizeNative(_handle, Math.Max(1, width), Math.Max(1, height), xScale, yScale));
    public void Orbit(float x, float y) => Check(OrbitNative(_handle, x, y));
    public void Pan(float x, float y) => Check(PanNative(_handle, x, y));
    public void Zoom(float delta) => Check(ZoomNative(_handle, delta));
    public void Fit() => Check(FitNative(_handle));
    public void Select(int id) => Check(SelectNative(_handle, id));
    public int Pick(float x, float y) { Check(PickNative(_handle, x, y, out var id)); return id; }
    public void SetTheme(bool light) => Check(SetThemeNative(_handle, light ? 1 : 0));
    public Stats GetStats() { var result = new Stats { Size = (uint)Marshal.SizeOf<Stats>() }; Check(GetStatsNative(_handle, ref result)); return result; }
    public unsafe string GetError()
    {
        var buffer = stackalloc char[1024];
        buffer[0] = '\0';
        Check(GetErrorNative(_handle, buffer, 1024));
        return new string(buffer);
    }
    public void Dispose()
    {
        if (_handle != 0) { Destroy(_handle); _handle = 0; }
    }

    [LibraryImport("NebulaRender.dll", EntryPoint = "nb_create")] private static partial int Create(nint panel, out nint renderer);
    [LibraryImport("NebulaRender.dll", EntryPoint = "nb_destroy")] private static partial void Destroy(nint renderer);
    [LibraryImport("NebulaRender.dll", EntryPoint = "nb_resize")] private static partial int ResizeNative(nint renderer, int width, int height, float xScale, float yScale);
    [LibraryImport("NebulaRender.dll", EntryPoint = "nb_set_scene")] private static unsafe partial int SetSceneNative(nint renderer, Item* items, int count);
    [LibraryImport("NebulaRender.dll", EntryPoint = "nb_set_positions")] private static unsafe partial int SetPositionsNative(nint renderer, float* positions, int count);
    [LibraryImport("NebulaRender.dll", EntryPoint = "nb_set_filter")] private static unsafe partial int SetFilterNative(nint renderer, byte* mask, int count);
    [LibraryImport("NebulaRender.dll", EntryPoint = "nb_select")] private static partial int SelectNative(nint renderer, int id);
    [LibraryImport("NebulaRender.dll", EntryPoint = "nb_pick")] private static partial int PickNative(nint renderer, float x, float y, out int id);
    [LibraryImport("NebulaRender.dll", EntryPoint = "nb_orbit")] private static partial int OrbitNative(nint renderer, float x, float y);
    [LibraryImport("NebulaRender.dll", EntryPoint = "nb_pan")] private static partial int PanNative(nint renderer, float x, float y);
    [LibraryImport("NebulaRender.dll", EntryPoint = "nb_zoom")] private static partial int ZoomNative(nint renderer, float delta);
    [LibraryImport("NebulaRender.dll", EntryPoint = "nb_fit")] private static partial int FitNative(nint renderer);
    [LibraryImport("NebulaRender.dll", EntryPoint = "nb_set_theme")] private static partial int SetThemeNative(nint renderer, int light);
    [LibraryImport("NebulaRender.dll", EntryPoint = "nb_get_stats")] private static partial int GetStatsNative(nint renderer, ref Stats stats);
    [LibraryImport("NebulaRender.dll", EntryPoint = "nb_get_error")] private static unsafe partial int GetErrorNative(nint renderer, char* text, int capacity);
    [LibraryImport("NebulaRender.dll", EntryPoint = "nb_set_glass")] private static unsafe partial int SetGlassNative(nint renderer, GlassRect* rects, int count);
    [LibraryImport("NebulaRender.dll", EntryPoint = "nb_hover")] private static partial int HoverNative(nint renderer, int id);
    [LibraryImport("NebulaRender.dll", EntryPoint = "nb_focus")] private static partial int FocusNative(nint renderer, int id);
    [LibraryImport("NebulaRender.dll", EntryPoint = "nb_back")] private static partial int BackNative(nint renderer);
    [LibraryImport("NebulaRender.dll", EntryPoint = "nb_set_reduced_motion")] private static partial int SetReducedMotionNative(nint renderer, int reduced);
    [LibraryImport("NebulaRender.dll", EntryPoint = "nb_set_pixels")] private static partial int SetPixelsNative(nint renderer, int level);
    [LibraryImport("NebulaRender.dll", EntryPoint = "nb_set_results")] private static unsafe partial int SetResultsNative(nint renderer, Result* results, int count);
    [LibraryImport("NebulaRender.dll", EntryPoint = "nb_clear_results")] private static partial int ClearResultsNative(nint renderer);
    [LibraryImport("NebulaRender.dll", EntryPoint = "nb_set_glass_panels")] private static unsafe partial int SetGlassPanelsNative(nint renderer, GlassPanel* panels, int count);
    [LibraryImport("NebulaRender.dll", EntryPoint = "nb_set_highlight")] private static unsafe partial int SetHighlightNative(nint renderer, uint* ids, float* weights, int count);
    [LibraryImport("NebulaRender.dll", EntryPoint = "nb_set_lines")] private static unsafe partial int SetLinesNative(nint renderer, Line* lines, int count);
    [LibraryImport("NebulaRender.dll", EntryPoint = "nb_focus_set")] private static unsafe partial int FocusSetNative(nint renderer, uint* ids, int count);
    [LibraryImport("NebulaRender.dll", EntryPoint = "nb_get_view")] private static unsafe partial int GetViewNative(nint renderer, View* view);
    [LibraryImport("NebulaRender.dll", EntryPoint = "nb_pick_lod")] private static partial int PickLodNative(nint renderer, float xPixels, float yPixels, out int id, out int tier);
    [LibraryImport("NebulaRender.dll", EntryPoint = "nb_get_lod_stats")] private static unsafe partial int GetLodStatsNative(nint renderer, LodStats* stats);
    [LibraryImport("NebulaRender.dll", EntryPoint = "nb_set_scene_v2")] private static unsafe partial int SetSceneV2Native(nint renderer, ItemV2* items, int count);
    [LibraryImport("NebulaRender.dll", EntryPoint = "nb_get_focus_stats")] private static unsafe partial int GetFocusStatsNative(nint renderer, FocusStats* stats);
    [LibraryImport("NebulaRender.dll", EntryPoint = "nb_set_depth_options")] private static partial int SetDepthOptionsNative(nint renderer, int atmosphere, int focusDof);
    [LibraryImport("NebulaRender.dll", EntryPoint = "nb_get_frame_diagnostics")] private static unsafe partial int GetFrameDiagnosticsNative(nint renderer, FrameDiagnostics* diagnostics);
}
