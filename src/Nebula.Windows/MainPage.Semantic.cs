using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Nebula.Core;
using Nebula.Embed;
using Nebula_Windows.Services;
using System.Numerics;

namespace Nebula_Windows;

/// The local-AI layer: SigLIP 2 (so400m-patch16-384, ONNX on DirectML) image
/// embeddings for the open library, semantic search over them, and the
/// PaCMAP Similarity arrangement. Everything runs on this PC; nothing is uploaded.
public sealed partial class MainPage
{
    private EmbeddingIndex? _semantic;
    private Library? _semanticLibrary;
    private Library? _semanticRequested;
    private Task<Vector3[]>? _similarityPositions;
    private readonly SemaphoreSlim _semanticGate = new(1, 1);
    private CancellationTokenSource? _semanticIndexing;
    private CancellationTokenSource? _semanticQueryCancellation;
    private string? _semanticQuery;
    private IReadOnlyList<(int Index, float Score)>? _semanticHits;
    private string _semanticProgress = "";

    private bool IsSemanticReady => _semantic is not null && ReferenceEquals(_semanticLibrary, _library);

    /// Page teardown: stop semantic work and release the model sessions and mapped matrix once no
    /// search or group build holds them. Loading the page again starts a fresh index.
    private void ReleaseSemantic()
    {
        _semanticIndexing?.Cancel();
        _semanticQueryCancellation?.Cancel();
        _mapTimer?.Stop();
        var semantic = _semantic;
        _semantic = null;
        _semanticLibrary = null;
        _semanticRequested = null;
        _similarityPositions = null;
        _semanticHits = null;
        _semanticQuery = null;
        if (semantic is null) return;
        _ = Task.Run(async () =>
        {
            await _semanticGate.WaitAsync();
            try { semantic.Dispose(); }
            finally { _semanticGate.Release(); }
        });
    }

    /// Called when a library becomes current. Loads embeddings if they exist; otherwise embeds in
    /// the background (resumable, only new or changed photos), then enables semantic search and
    /// Similarity. If the model is not installed the app stays on metadata search and says so.
    private void StartSemanticIndex(Library library)
    {
        if (ReferenceEquals(library, _semanticRequested)) return;
        _semanticRequested = library;
        _semanticIndexing?.Cancel();
        _semanticQueryCancellation?.Cancel();
        var previous = _semantic;
        _semantic = null;
        _semanticLibrary = null;
        _similarityPositions = null;
        _semanticHits = null;
        LeaveSimilarity();
        ResetVisualGroups();
        _semanticQuery = null;
        UpdateSimilarityAvailability();
        var cancellation = new CancellationTokenSource();
        _semanticIndexing = cancellation;
        var folder = LibraryService.LibraryFolder(library.Source);
        _ = Task.Run(async () =>
        {
            // Wait without the token: every holder of the gate has been cancelled too, and the
            // superseded index is disposed only once no search or group build uses it.
            await _semanticGate.WaitAsync();
            EmbeddingIndex? index = null;
            try
            {
                previous?.Dispose();
                cancellation.Token.ThrowIfCancellationRequested();
                index = EmbeddingIndex.Load(folder);
                // Always reconcile by photo identity, not count: a replaced photo keeps the count.
                // When nothing changed this only compares the sidecar and starts no model session.
                var progress = new Progress<string>(text => DispatcherQueue.TryEnqueue(() =>
                {
                    if (cancellation.IsCancellationRequested) return;
                    _semanticProgress = Strings.Format("Status_UnderstandingPhotos", text.Replace("embedded ", "").Split(" new")[0]);
                    if (string.IsNullOrWhiteSpace(Search.Text)) ApplyFilter();
                }));
                var report = await index.EnsureEmbeddedAsync(progress, cancellation.Token);
                if (report.EmbeddedPhotos > 0)
                    LibraryService.Log($"Semantic index: embedded {report.EmbeddedPhotos}, reused {report.ReusedPhotos} on {report.Device} " +
                        $"at {report.ImagesPerSecond:F1} images/s");
                LibraryService.Log($"Semantic index ready: {index.Count} photos, {index.Dimensions} dimensions, model {EmbeddingIndex.ModelId}");
                // Load the text tower now (~6 s cold on DirectML) so the first query is as fast as the rest.
                try
                {
                    var warm = System.Diagnostics.Stopwatch.StartNew();
                    await index.SearchAsync("a photo", cancellation.Token);
                    LibraryService.Log($"Semantic search warmed in {warm.ElapsedMilliseconds} ms");
                }
                catch (Exception error) when (error is not OperationCanceledException) { LibraryService.Log($"Search warm-up failed: {error.Message}"); }
                var ready = index;
                if (DispatcherQueue.TryEnqueue(() =>
                {
                    if (cancellation.IsCancellationRequested || !ReferenceEquals(library, _library)) { ready.Dispose(); return; }
                    _semantic = ready;
                    _semanticLibrary = library;
                    _semanticProgress = "";
                    UpdateSimilarityAvailability();
                    StartVisualGroups(library, ready, cancellation.Token);
                    ApplyFilter();
                }))
                {
                    index = null; // the dispatcher callback owns it now
                }
            }
            catch (OperationCanceledException) { }
            catch (Exception error)
            {
                LibraryService.Log($"Semantic index unavailable; metadata search only: {error}");
                DispatcherQueue.TryEnqueue(() =>
                {
                    if (cancellation.IsCancellationRequested) return;
                    _semanticProgress = "";
                    if (_library is not null) ApplyFilter();
                });
            }
            finally
            {
                index?.Dispose();
                _semanticGate.Release();
            }
        });
    }

    private void UpdateSimilarityAvailability()
    {
        var ready = IsSemanticReady;
        ModeSimilarity.IsEnabled = ready;
        MenuSimilarity.IsEnabled = ready;
        ToolTipService.SetToolTip(ModeSimilarity, ready ? Strings.Get("Similarity_ReadyTip")
            : _semanticProgress.Length > 0 ? Strings.Get("Similarity_PendingTip") : Strings.Get("Similarity_NeedsModelTip"));
    }

    /// Semantic ranking for the current query, or null while it is still being computed (the
    /// caller keeps the previous presentation instead of flashing a metadata result first).
    private IReadOnlyList<(int Index, float Score)>? SemanticHitsFor(string query)
    {
        if (!IsSemanticReady) return null;
        if (_semanticQuery == query && _semanticHits is not null) return _semanticHits;
        if (_semanticQuery == query) return null; // in flight
        _semanticQuery = query;
        _semanticHits = null;
        _semanticQueryCancellation?.Cancel();
        var cancellation = new CancellationTokenSource();
        _semanticQueryCancellation = cancellation;
        var index = _semantic!;
        var library = _library!;
        var rowOf = new Dictionary<string, int>(library.Photos.Count);
        for (var i = 0; i < library.Photos.Count; i++) rowOf[library.Photos[i].Id] = i;
        _ = Task.Run(async () =>
        {
            await _semanticGate.WaitAsync(cancellation.Token);
            try
            {
                var started = System.Diagnostics.Stopwatch.StartNew();
                var ranked = await index.SearchAsync(query, cancellation.Token);
                var hits = ranked.Where(r => rowOf.ContainsKey(r.Id)).Select(r => (rowOf[r.Id], r.Score)).ToArray();
                LibraryService.Log($"Semantic search: {hits.Length} hits in {started.ElapsedMilliseconds} ms");
                DispatcherQueue.TryEnqueue(() =>
                {
                    if (cancellation.IsCancellationRequested || _semanticQuery != query || !ReferenceEquals(library, _library)) return;
                    _semanticHits = hits;
                    ApplyFilter();
                });
            }
            catch (OperationCanceledException) { }
            catch (Exception error)
            {
                LibraryService.Log($"Semantic search failed; metadata results shown: {error}");
                DispatcherQueue.TryEnqueue(() =>
                {
                    if (cancellation.IsCancellationRequested || _semanticQuery != query) return;
                    _semanticHits = [];
                    ApplyFilter();
                });
            }
            finally { _semanticGate.Release(); }
        });
        return null;
    }

    /// Similarity positions: PaCMAP over the SigLIP rows, cached beside the embeddings so the
    /// projection is computed once per library state.
    private Task<Vector3[]> SimilarityPositionsAsync(Library library)
    {
        if (!IsSemanticReady || !ReferenceEquals(library, _semanticLibrary))
            return Task.FromException<Vector3[]>(new InvalidOperationException(Strings.Get("Error_SimilarityNotReady")));
        if (_similarityPositions is { IsFaulted: false, IsCanceled: false } cached) return cached;
        var index = _semantic!;
        var folder = LibraryService.LibraryFolder(library.Source);
        var ids = library.Photos.Select(p => p.Id).ToArray();
        _similarityPositions = Task.Run(() =>
        {
            var cachePath = Path.Combine(folder, "similarity.bin");
            var key = "similarity-layout-v2|" + index.ModelHash + "|" + string.Join(",", ids);
            var keyHash = Convert.ToHexString(System.Security.Cryptography.SHA256.HashData(System.Text.Encoding.UTF8.GetBytes(key)));
            if (TryReadSimilarity(cachePath, keyHash, ids.Length) is { } stored) return stored;
            var started = System.Diagnostics.Stopwatch.StartNew();
            var dims = index.Dimensions;
            var matrix = new float[ids.Length * dims];
            for (var i = 0; i < ids.Length; i++) index.Vector(ids[i]).CopyTo(matrix.AsSpan(i * dims));
            var positions = Nebula.Core.Layout.SimilarityPositions(matrix, ids.Length, dims);
            LibraryService.Log($"Similarity layout: {ids.Length} photos in {started.Elapsed.TotalSeconds:F1} s");
            WriteSimilarity(cachePath, keyHash, positions);
            return positions;
        });
        return _similarityPositions;
    }

    private static Vector3[]? TryReadSimilarity(string path, string keyHash, int count)
    {
        try
        {
            if (!File.Exists(path)) return null;
            using var reader = new BinaryReader(File.OpenRead(path));
            if (reader.ReadString() != keyHash || reader.ReadInt32() != count) return null;
            var positions = new Vector3[count];
            for (var i = 0; i < count; i++) positions[i] = new(reader.ReadSingle(), reader.ReadSingle(), reader.ReadSingle());
            return positions;
        }
        catch (Exception error) when (error is IOException or EndOfStreamException or UnauthorizedAccessException) { return null; }
    }

    private static void WriteSimilarity(string path, string keyHash, Vector3[] positions)
    {
        try
        {
            var temp = path + ".tmp";
            using (var writer = new BinaryWriter(File.Create(temp)))
            {
                writer.Write(keyHash);
                writer.Write(positions.Length);
                foreach (var p in positions) { writer.Write(p.X); writer.Write(p.Y); writer.Write(p.Z); }
            }
            File.Move(temp, path, overwrite: true);
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException)
        {
            LibraryService.Log($"Similarity layout cache not written: {error.Message}");
        }
    }
}
