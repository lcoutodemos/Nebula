using Microsoft.ML.OnnxRuntime;
using Microsoft.ML.OnnxRuntime.Tensors;
using Nebula.Core;
using System.Buffers.Binary;
using System.Buffers;
using System.Diagnostics;
using System.Numerics;
using System.Security.Cryptography;
using System.Text.Json;
using System.Runtime.InteropServices;
using System.Threading.Channels;
using Tokenizers.DotNet;

namespace Nebula.Embed;

public sealed class EmbeddingIndex : IDisposable
{
    public const string ModelId = "google/siglip2-so400m-patch16-384";
    public const int ImageSize = 384;
    public const int TokenLength = 64;
    public const double Tau = 109.86130976812342;
    public const double Beta = -15.9296875;

    private static readonly JsonSerializerOptions JsonOptions = new(JsonSerializerDefaults.Web)
    {
        WriteIndented = true,
        PropertyNameCaseInsensitive = true
    };

    private readonly string _libraryDir;
    private readonly string _modelDir;
    private readonly ModelManifest _modelManifest;
    private readonly Library _library;
    private InferenceSession? _visionSession;
    private InferenceSession? _textSession;
    private ReadOnlyTensorMap<Half>? _tokenTable;
    private CompactTextManifest? _activeCompactText;
    private bool _compactFilesVerified;
    private Timer? _textIdleTimer;
    private long _lastTextUse;
    private bool _textHasLoaded, _idleReleaseDisabled;
    private Tokenizer? _tokenizer;
    private ReadOnlyTensorMap<float>? _matrix;
    private string[] _ids = [];
    private Dictionary<string, int> _rowById = [];
    private float[] _rowNorms = [];
    private float[] _mean = [];
    private long _matrixWriteTicks;
    private DateTimeOffset _matrixCreatedUtc;
    private readonly object _textLock = new();
    private readonly object _stateLock = new();
    private readonly Dictionary<string, float[]> _textCache = new(StringComparer.Ordinal);
    private readonly Queue<string> _textCacheOrder = new();
    private int _visionBatchSize;
    private readonly SemaphoreSlim _embeddingGate = new(1, 1);
    private readonly CancellationTokenSource _lifetime = new();
    private int _disposed;

    private EmbeddingIndex(string libraryDir, string modelDir, Library library, ModelManifest manifest)
    {
        _libraryDir = libraryDir;
        _modelDir = modelDir;
        _library = library;
        _modelManifest = manifest;
        LoadPersistedEmbeddings();
    }

    public int Count => _ids.Length;
    public int Dimensions { get; private set; }
    public string Device { get; private set; } = "not initialized";
    public string ModelHash => _modelManifest.ModelHash;
    public string TextVariantId { get; private set; } = "fp32";
    public IReadOnlyList<string> PhotoIds => _ids;
    public IReadOnlyList<Photo> Photos => _library.Photos;
    internal float[] CopyEmbeddings()
    {
        using var matrix = CaptureMatrix(out var ids, out _, out _);
        var result = new float[checked(ids.Length * Dimensions)];
        for (var i = 0; i < ids.Length; i++) matrix.Row(i).CopyTo(result.AsSpan(i * Dimensions, Dimensions));
        return result;
    }
    internal string ModelDirectory => _modelDir;

    public static EmbeddingIndex Load(string libraryDir, string? modelDir = null)
    {
        libraryDir = Path.GetFullPath(libraryDir);
        modelDir ??= DefaultModelDirectory();
        var libraryPath = Path.Combine(libraryDir, "library.json");
        if (!File.Exists(libraryPath)) throw new FileNotFoundException("library.json was not found.", libraryPath);
        using var libraryFile = File.OpenRead(libraryPath);
        var library = JsonSerializer.Deserialize<Library>(libraryFile, JsonOptions)
            ?? throw new InvalidDataException("library.json could not be read.");
        var manifestPath = Path.Combine(modelDir, "manifest.json");
        if (!File.Exists(manifestPath)) throw new FileNotFoundException("The SigLIP ONNX manifest was not found. Run tools\\prepare_models.py first.", manifestPath);
        var manifest = JsonSerializer.Deserialize<ModelManifest>(File.ReadAllText(manifestPath), JsonOptions)
            ?? throw new InvalidDataException("Model manifest could not be read.");
        return new EmbeddingIndex(libraryDir, modelDir, library, manifest);
    }

    public async Task<EmbeddingRunReport> EnsureEmbeddedAsync(IProgress<string>? progress = null, CancellationToken cancellationToken = default)
    {
        ObjectDisposedException.ThrowIf(Volatile.Read(ref _disposed) != 0, this);
        using var linked = CancellationTokenSource.CreateLinkedTokenSource(cancellationToken, _lifetime.Token);
        await _embeddingGate.WaitAsync(linked.Token).ConfigureAwait(false);
        try
        {
            ObjectDisposedException.ThrowIf(Volatile.Read(ref _disposed) != 0, this);
            return await Task.Run(async () =>
            {
                // One process embeds a library at a time (the app and Nebula.Embed.Cli would otherwise each
                // load the model and redo the same rows). A waiter then finds the rows done and reuses them.
                // A named mutex is thread-affine, so a dedicated thread owns it for the whole run.
                var name = @"Local\Nebula.Embed." + Convert.ToHexString(System.Security.Cryptography.SHA256.HashData(
                    System.Text.Encoding.UTF8.GetBytes(_libraryDir.ToUpperInvariant())))[..32];
                var acquired = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
                var release = new ManualResetEventSlim(false);
                var holder = new Thread(() =>
                {
                    using var owner = new Mutex(false, name);
                    var held = false;
                    try
                    {
                        while (!held && !linked.IsCancellationRequested)
                        {
                            try { held = owner.WaitOne(1000); } catch (AbandonedMutexException) { held = true; }
                            if (!held) progress?.Report("waiting for another Nebula process that is understanding these photos");
                        }
                        if (!held) { acquired.TrySetCanceled(linked.Token); return; }
                        acquired.TrySetResult();
                        release.Wait();
                    }
                    catch (Exception error) { acquired.TrySetException(error); }
                    finally { if (held) owner.ReleaseMutex(); }
                }) { IsBackground = true, Name = "Nebula embedding owner" };
                holder.Start();
                try
                {
                    await acquired.Task.ConfigureAwait(false);
                    return await EnsureEmbeddedCoreAsync(progress, linked.Token).ConfigureAwait(false);
                }
                finally { release.Set(); holder.Join(); release.Dispose(); }
            }, linked.Token).ConfigureAwait(false);
        }
        finally { _embeddingGate.Release(); }
    }

    private async Task<EmbeddingRunReport> EnsureEmbeddedCoreAsync(IProgress<string>? progress, CancellationToken cancellationToken)
    {
        var current = TryReadSidecar();
        var reusable = new Dictionary<string, int>();
        var photosById = _library.Photos.ToDictionary(p => p.Id);
        if (current is not null && current.ModelHash == ModelHash && File.Exists(EmbeddingsPath))
        {
            if (current.Dimensions != _modelManifest.Dimensions) throw new InvalidDataException("Cached embedding dimensions differ from the checkpoint.");
            for (var i = 0; i < current.PhotoIds.Count; i++)
            {
                var row = current.PhotoIds[i];
                if (current.Rows.TryGetValue(row, out var meta) && IsSamePhoto(photosById.GetValueOrDefault(row), meta))
                {
                    reusable[row] = i;
                }
            }
        }

        cancellationToken.ThrowIfCancellationRequested();
        if (current is not null && reusable.Count == _library.Photos.Count &&
            current.PhotoIds.SequenceEqual(_library.Photos.Select(p => p.Id)))
        {
            if (!_ids.SequenceEqual(current.PhotoIds) || _matrixWriteTicks != File.GetLastWriteTimeUtc(EmbeddingsPath).Ticks ||
                _matrixCreatedUtc != current.CreatedUtc) LoadPersistedEmbeddings();
            return new EmbeddingRunReport(_library.Photos.Count, 0, reusable.Count, TimeSpan.Zero, Device, 0, Process.GetCurrentProcess().PeakWorkingSet64);
        }
        var output = new float[checked(_library.Photos.Count * _modelManifest.Dimensions)];
        var have = new bool[_library.Photos.Count];
        var kept = 0;
        var todo = new List<int>();
        using (var oldMap = reusable.Count > 0 ? new ReadOnlyTensorMap<float>(EmbeddingsPath, current!.PhotoIds.Count, current.Dimensions) : null)
        using (var old = oldMap?.Acquire())
        {
            for (var i = 0; i < _library.Photos.Count; i++)
            {
                var id = _library.Photos[i].Id;
                if (reusable.TryGetValue(id, out var row))
                {
                    old!.Row(row).CopyTo(output.AsSpan(i * _modelManifest.Dimensions, _modelManifest.Dimensions));
                    kept++; have[i] = true;
                }
                else todo.Add(i);
            }
        }

        cancellationToken.ThrowIfCancellationRequested();
        if (todo.Count == 0 && _ids.SequenceEqual(_library.Photos.Select(p => p.Id)))
            return new EmbeddingRunReport(_library.Photos.Count, 0, kept, TimeSpan.Zero, Device, 0, Process.GetCurrentProcess().PeakWorkingSet64);
        var sw = new Stopwatch();
        if (todo.Count > 0)
        {
            var initialization = Stopwatch.StartNew();
            EnsureVisionSession();
            progress?.Report($"embedding model initialization {initialization.Elapsed.TotalSeconds:F3}s");
            sw.Start();
            try { await EmbedBatchesAsync(todo, output, have, kept, progress, cancellationToken).ConfigureAwait(false); }
            finally
            {
                // The vision tower (~1.6 GB of GPU memory) is only needed while photos are being
                // understood; search uses the text tower. Release it as soon as the run ends.
                _visionSession?.Dispose();
                _visionSession = null;
            }
        }
        sw.Stop();
        WriteMatrixAtomically(EmbeddingsPath, output);
        var sidecar = new EmbeddingSidecar
        {
            ModelId = ModelId,
            ModelHash = ModelHash,
            Dimensions = _modelManifest.Dimensions,
            ImageSize = ImageSize,
            CreatedUtc = DateTimeOffset.UtcNow,
            Device = Device,
            PhotoIds = _library.Photos.Select(p => p.Id).ToList(),
            Rows = _library.Photos.ToDictionary(p => p.Id, PhotoRow.FromPhoto)
        };
        WriteJsonAtomically(SidecarPath, sidecar);
        LoadPersistedEmbeddings();
        return new EmbeddingRunReport(_library.Photos.Count, todo.Count, kept, sw.Elapsed, Device,
            todo.Count / Math.Max(sw.Elapsed.TotalSeconds, 1e-9), Process.GetCurrentProcess().PeakWorkingSet64);
    }

    public async Task<IReadOnlyList<(string Id, float Score)>> SearchAsync(string query, CancellationToken cancellationToken = default)
    {
        using var matrix = CaptureMatrix(out var ids, out var mean, out var norms);
        var dimensions = Dimensions;
        var prompts = Prompts(query);
        if (prompts.Count == 0) return [];
        List<float[]> text;
        lock (_textLock)
        {
            ObjectDisposedException.ThrowIf(Volatile.Read(ref _disposed) != 0, this);
            text = GetTextRows(prompts);
        }
        cancellationToken.ThrowIfCancellationRequested();
        var q = QueryVector(text);
        var rawProb = new float[ids.Length];
        var dense = new float[ids.Length];
        Parallel.For(0, (ids.Length + 127) / 128, new ParallelOptions { CancellationToken = cancellationToken },
            () => ArrayPool<float>.Shared.Rent(dimensions),
            (block, _, scratch) =>
            {
                var centered = scratch.AsSpan(0, dimensions);
                for (var i = block * 128; i < Math.Min(ids.Length, (block + 1) * 128); i++)
                {
                    var row = matrix.Row(i);
                    var probability = 0f;
                    foreach (var t in text)
                        probability += (float)(1.0 / (1.0 + Math.Exp(-(Tau * Dot(row, t) + Beta))));
                    rawProb[i] = probability / text.Count;
                    FillCentered(row, mean, norms[i], centered);
                    dense[i] = Dot(centered, q);
                }
                return scratch;
            }, scratch => ArrayPool<float>.Shared.Return(scratch));
        ZScoreInPlace(dense);
        var best = new PriorityQueue<(int Index, float Score), (float Score, int Tie)>();
        for (var i = 0; i < ids.Length; i++)
        {
            if (rawProb[i] < 0.02f && dense[i] <= 2.5f) continue;
            var score = GSig(dense[i]);
            if (best.Count < 1500)
                best.Enqueue((i, score), (score, -i));
            else if (score > best.Peek().Score)
            {
                best.DequeueEnqueue((i, score), (score, -i));
            }
        }
        var order = best.UnorderedItems.Select(p => p.Element).OrderByDescending(p => p.Score).ThenBy(p => p.Index).Select(p => (ids[p.Index], p.Score)).ToArray();
        return await Task.FromResult(order);
    }

    public float[] Vector(string id)
    {
        lock (_stateLock)
        {
            ObjectDisposedException.ThrowIf(Volatile.Read(ref _disposed) != 0, this);
            if (!_rowById.TryGetValue(id, out var row)) throw new KeyNotFoundException(id);
            using var matrix = _matrix!.Acquire();
            return matrix.Row(row).ToArray();
        }
    }

    public IReadOnlyList<string> Similar(string id, int k)
    {
        int row, dimensions;
        float[] mean, norms;
        ReadOnlyTensorMap<float>.Lease lease;
        string[] ids;
        lock (_stateLock)
        {
            ObjectDisposedException.ThrowIf(Volatile.Read(ref _disposed) != 0, this);
            if (!_rowById.TryGetValue(id, out row)) throw new KeyNotFoundException(id);
            dimensions = Dimensions; ids = _ids; mean = _mean; norms = _rowNorms;
            lease = _matrix!.Acquire();
        }
        using var matrix = lease;
        k = Math.Clamp(k, 0, Math.Max(0, ids.Length - 1));
        if (k == 0) return [];
        var query = new float[dimensions];
        var centered = new float[dimensions];
        FillCentered(matrix.Row(row), mean, norms[row], query);
        var scores = new PriorityQueue<(int Index, float Score), (float Score, int Tie)>();
        for (var i = 0; i < ids.Length; i++)
            if (i != row)
            {
                FillCentered(matrix.Row(i), mean, norms[i], centered);
                var score = Dot(query, centered);
                if (scores.Count < k) scores.Enqueue((i, score), (score, -i));
                else if (score > scores.Peek().Score) scores.DequeueEnqueue((i, score), (score, -i));
            }
        return scores.UnorderedItems.Select(x => x.Element)
            .OrderByDescending(p => p.Score)
            .ThenBy(p => p.Index)
            .Take(k)
            .Select(p => ids[p.Index])
            .ToArray();
    }

    public Task<IReadOnlyList<float[]>> EncodeTextAsync(IEnumerable<string> texts, CancellationToken cancellationToken = default)
    {
        lock (_textLock)
        {
            ObjectDisposedException.ThrowIf(Volatile.Read(ref _disposed) != 0, this);
            cancellationToken.ThrowIfCancellationRequested();
            var rows = GetTextRows(texts.ToArray());
            return Task.FromResult<IReadOnlyList<float[]>>(rows.Select(r => r.ToArray()).ToArray());
        }
    }

    public void Dispose()
    {
        if (Interlocked.Exchange(ref _disposed, 1) != 0) return;
        _lifetime.Cancel();
        _embeddingGate.Wait();
        try
        {
            _visionSession?.Dispose();
            lock (_textLock) { _textIdleTimer?.Dispose(); _textIdleTimer = null; ReleaseTextResources(); }
            lock (_stateLock) { _matrix?.Dispose(); _matrix = null; }
        }
        finally { _embeddingGate.Release(); }
    }

    private string EmbeddingsPath => Path.Combine(_libraryDir, "embeddings.bin");
    private string SidecarPath => Path.Combine(_libraryDir, "embeddings.json");

    private void EnsureVisionSession()
    {
        if (_visionSession is not null) return;
        var configured = Environment.GetEnvironmentVariable("NEB_EMBED_BATCH");
        _visionBatchSize = configured is null ? 1 : int.TryParse(configured, out var value) && value is >= 1 and <= 128
            ? value : throw new ArgumentException("NEB_EMBED_BATCH must be an integer from 1 to 128.");
        _visionSession = CreateSession(ModelFile(_modelManifest.VisionFile), _visionBatchSize);
        if (_visionSession.InputMetadata.Values.First().ElementType != typeof(float))
            throw new NotSupportedException("This preprocessing recipe requires the model's float32 input.");
    }

    private void EnsureTextSession()
    {
        if (_textSession is not null) return;
        var previousVariant = TextVariantId;
        TextVariantId = "fp32";
        var mode = Environment.GetEnvironmentVariable("NEB_TEXT_VARIANT") ?? "auto";
        if (mode is not ("auto" or "fp32" or "compact")) throw new ArgumentException("NEB_TEXT_VARIANT must be auto, fp32 or compact.");
        var compact = _modelManifest.CompactText;
        if (mode == "compact" && compact is null) throw new InvalidOperationException("No compact text variant is installed.");
        if (mode != "fp32" && compact is not null && (compact.Enabled || mode == "compact"))
        {
            try
            {
                if (compact.SourceModelHash != ModelHash || compact.TokenColumns != Dimensions || compact.TokenType != "float16")
                    throw new InvalidDataException("Compact text metadata does not match this checkpoint.");
                if (compact.Files.Count < 3 || !compact.Files.Any(file => file.Path == compact.File) ||
                    !compact.Files.Any(file => file.Path == compact.TokenFile))
                    throw new InvalidDataException("Compact text artifact hashes are incomplete.");
                if (!_compactFilesVerified)
                {
                    foreach (var file in compact.Files)
                    {
                        var path = ModelFile(file.Path);
                        using var input = File.OpenRead(path);
                        if (input.Length != file.Bytes || !Convert.ToHexString(SHA256.HashData(input)).Equals(file.Sha256, StringComparison.OrdinalIgnoreCase))
                            throw new InvalidDataException($"Compact model hash mismatch: {file.Path}");
                    }
                    _compactFilesVerified = true;
                }
                _tokenTable = new ReadOnlyTensorMap<Half>(ModelFile(compact.TokenFile), compact.TokenRows, compact.TokenColumns);
                _textSession = CreateSession(ModelFile(compact.File), fixedBatch: 16, allowCpuFallback: false, compactText: true);
                if (!_textSession.InputMetadata.TryGetValue(compact.EmbeddingInput, out var embeddingInput) || embeddingInput.ElementType != typeof(float) ||
                    !_textSession.InputMetadata.TryGetValue("input_ids", out var tokenInput) || tokenInput.ElementType != typeof(long))
                    throw new InvalidDataException("Compact text inputs do not match the CPU Gather contract.");
                _activeCompactText = compact;
                TextVariantId = compact.Id;
            }
            catch (Exception error) when (error is IOException or InvalidDataException or OnnxRuntimeException or NotSupportedException)
            {
                ReleaseTextResources();
                if (mode == "compact") throw;
                Trace.TraceWarning($"Compact text model unavailable; retaining fp32 fallback: {error.Message}");
            }
        }
        _textSession ??= CreateSession(ModelFile(_modelManifest.TextFile));
        _tokenizer ??= new Tokenizer(vocabPath: Path.Combine(_modelDir, "tokenizer.json"));
        if (previousVariant != TextVariantId) { _textCache.Clear(); _textCacheOrder.Clear(); }
    }

    private List<float[]> GetTextRows(IReadOnlyList<string> prompts)
    {
        if (_textSession is null && prompts.All(_textCache.ContainsKey))
            return prompts.Select(prompt => _textCache[prompt]).ToList();
        var reload = _textSession is null && _textHasLoaded;
        var started = Stopwatch.GetTimestamp();
        EnsureTextSession();
        var rows = EncodeText(prompts);
        _textHasLoaded = true;
        if (reload && Stopwatch.GetElapsedTime(started).TotalMilliseconds >= 1500)
        {
            _idleReleaseDisabled = true;
            Trace.TraceInformation("Text-session reload exceeded 1.5 seconds; retaining this session instead of evicting it again.");
        }
        _lastTextUse = Stopwatch.GetTimestamp();
        var compact = _activeCompactText;
        if (!_idleReleaseDisabled && compact is { Enabled: true, IdleReleaseSeconds: > 0, VerifiedReloadMs: > 0 and < 1500 })
        {
            _textIdleTimer ??= new Timer(static state =>
            {
                if (((WeakReference<EmbeddingIndex>)state!).TryGetTarget(out var index)) index.ReleaseIdleText();
            }, new WeakReference<EmbeddingIndex>(this), Timeout.InfiniteTimeSpan, Timeout.InfiniteTimeSpan);
            _textIdleTimer.Change(TimeSpan.FromSeconds(compact.IdleReleaseSeconds), Timeout.InfiniteTimeSpan);
        }
        else _textIdleTimer?.Change(Timeout.InfiniteTimeSpan, Timeout.InfiniteTimeSpan);
        return rows;
    }

    private void ReleaseIdleText()
    {
        lock (_textLock)
        {
            if (Volatile.Read(ref _disposed) != 0 || _idleReleaseDisabled || _activeCompactText is not { } compact) return;
            var remaining = TimeSpan.FromSeconds(compact.IdleReleaseSeconds) - Stopwatch.GetElapsedTime(_lastTextUse);
            if (remaining > TimeSpan.Zero) { _textIdleTimer?.Change(remaining, Timeout.InfiniteTimeSpan); return; }
            ReleaseTextResources();
        }
    }

    private string ModelFile(string relative)
    {
        var root = Path.GetFullPath(_modelDir).TrimEnd(Path.DirectorySeparatorChar) + Path.DirectorySeparatorChar;
        var path = Path.GetFullPath(Path.Combine(root, relative));
        if (!path.StartsWith(root, StringComparison.OrdinalIgnoreCase)) throw new InvalidDataException("Model file escapes the model pack.");
        return path;
    }

    private static readonly Lazy<bool> TelemetryDisabled = new(() =>
    {
        OrtEnv.Instance().DisableTelemetryEvents();
        return true;
    }, LazyThreadSafetyMode.ExecutionAndPublication);

    private InferenceSession CreateSession(string modelPath, int? fixedBatch = null, bool allowCpuFallback = true, bool compactText = false)
    {
        // Nebula sends nothing off the PC; opt out of ONNX Runtime's own telemetry events too.
        // Every session creator waits here until the opt-out has completed.
        _ = TelemetryDisabled.Value;
        using var options = new SessionOptions { EnableMemoryPattern = false, ExecutionMode = ExecutionMode.ORT_SEQUENTIAL };
        if (compactText)
        {
            options.EnableCpuMemArena = false;
            options.AddSessionConfigEntry("session.use_device_allocator_for_initializers", "1");
        }
        if (fixedBatch is { } count) options.AddFreeDimensionOverrideByName("batch", count);
        try
        {
            options.AppendExecutionProvider_DML(0);
            Device = "DirectML GPU";
            return new InferenceSession(modelPath, options);
        }
        catch (OnnxRuntimeException error) when (allowCpuFallback)
        {
            Trace.TraceWarning($"DirectML initialization failed for {modelPath}: {error.Message}. Using CPU execution.");
            Device = "CPU";
            return new InferenceSession(modelPath);
        }
    }

    private sealed class InputBatch : IDisposable
    {
        public float[] Values { get; }
        public OrtValue Tensor { get; }
        public int[] Rows { get; set; } = [];
        public InputBatch(int size)
        {
            Values = GC.AllocateUninitializedArray<float>(size * 3 * ImageSize * ImageSize, pinned: true);
            Tensor = OrtValue.CreateTensorValueFromMemory(Values, [size, 3, ImageSize, ImageSize]);
        }
        public void Dispose() => Tensor.Dispose();
    }

    private async Task EmbedBatchesAsync(List<int> todo, float[] output, bool[] have, int kept, IProgress<string>? progress, CancellationToken ct)
    {
        using var stop = CancellationTokenSource.CreateLinkedTokenSource(ct);
        var free = Channel.CreateBounded<InputBatch>(3);
        var ready = Channel.CreateBounded<InputBatch>(2);
        var buffers = new List<InputBatch>();
        var batchSize = _visionBatchSize;
        var dimensions = _modelManifest.Dimensions;
        var rows = GC.AllocateUninitializedArray<float>(batchSize * dimensions, pinned: true);
        using var outputValue = OrtValue.CreateTensorValueFromMemory(rows, [batchSize, dimensions]);
        using var runOptions = new RunOptions();
        string[] inputNames = [_visionSession!.InputMetadata.Keys.First()];
        string[] outputNames = [_visionSession.OutputMetadata.Keys.First()];
        OrtValue[] inputs = new OrtValue[1];
        OrtValue[] outputs = [outputValue];
        double preprocessingMs = 0, inferenceMs = 0;
        Task producer = Task.CompletedTask;
        try
        {
            for (var i = 0; i < 3; i++)
            {
                var batch = new InputBatch(batchSize);
                buffers.Add(batch); free.Writer.TryWrite(batch);
            }
            producer = Task.Run(async () =>
            {
                try
                {
                    foreach (var chunk in todo.Chunk(batchSize))
                    {
                        var batch = await free.Reader.ReadAsync(stop.Token).ConfigureAwait(false);
                        batch.Rows = chunk;
                        var start = Stopwatch.GetTimestamp();
                        Parallel.For(0, chunk.Length, new ParallelOptions { CancellationToken = stop.Token, MaxDegreeOfParallelism = Math.Min(4, Environment.ProcessorCount) }, b =>
                        {
                            var photo = _library.Photos[chunk[b]];
                            var path = File.Exists(photo.ThumbnailPath) ? photo.ThumbnailPath : photo.OriginalPath;
                            ImagePreprocessor.WriteTensor(path, batch.Values.AsSpan(b * 3 * ImageSize * ImageSize, 3 * ImageSize * ImageSize));
                        });
                        batch.Values.AsSpan(chunk.Length * 3 * ImageSize * ImageSize).Clear();
                        preprocessingMs += Stopwatch.GetElapsedTime(start).TotalMilliseconds;
                        await ready.Writer.WriteAsync(batch, stop.Token).ConfigureAwait(false);
                    }
                    ready.Writer.Complete();
                }
                catch (Exception error)
                {
                    ready.Writer.TryComplete(error);
                    throw;
                }
            }, stop.Token);
            var done = 0; var checkpoint = kept; var lastReport = Stopwatch.GetTimestamp();
            await foreach (var batch in ready.Reader.ReadAllAsync(ct).ConfigureAwait(false))
            {
                ct.ThrowIfCancellationRequested();
                var start = Stopwatch.GetTimestamp();
                inputs[0] = batch.Tensor;
                _visionSession.Run(runOptions, inputNames, inputs, outputNames, outputs);
                inferenceMs += Stopwatch.GetElapsedTime(start).TotalMilliseconds;
                for (var b = 0; b < batch.Rows.Length; b++)
                {
                    var row = rows.AsSpan(b * dimensions, dimensions);
                    Normalize(row);
                    row.CopyTo(output.AsSpan(batch.Rows[b] * dimensions, dimensions));
                    have[batch.Rows[b]] = true;
                }
                done += batch.Rows.Length;
                if (done == todo.Count || Stopwatch.GetElapsedTime(lastReport).TotalSeconds >= .5)
                {
                    progress?.Report($"embedded {done}/{todo.Count} new photos ({kept} reused), batch {batchSize}");
                    lastReport = Stopwatch.GetTimestamp();
                }
                if (kept + done - checkpoint >= 1000 && kept + done < _library.Photos.Count)
                {
                    WriteCheckpoint(output, have);
                    checkpoint = kept + done;
                }
                await free.Writer.WriteAsync(batch, ct).ConfigureAwait(false);
            }
            await producer.ConfigureAwait(false);
            progress?.Report($"embedding phases: preprocessing {preprocessingMs / 1000:F3}s overlapped, inference/binding {inferenceMs / 1000:F3}s, float32 batch {batchSize}, {Device}");
        }
        finally
        {
            stop.Cancel();
            try { await producer.ConfigureAwait(false); }
            catch (OperationCanceledException) when (stop.IsCancellationRequested) { }
            finally { foreach (var buffer in buffers) buffer.Dispose(); }
        }
    }

    private List<float[]> EncodeText(IReadOnlyList<string> prompts)
    {
        var output = new float[prompts.Count][];
        var missing = Enumerable.Range(0, prompts.Count).Where(i => !_textCache.ContainsKey(prompts[i])).ToArray();
        for (var i = 0; i < prompts.Count; i++) if (_textCache.TryGetValue(prompts[i], out var cached)) output[i] = cached;
        var batchSize = _activeCompactText is null ? 64 : 16;
        for (var start = 0; start < missing.Length; start += batchSize)
        {
            var count = Math.Min(batchSize, missing.Length - start);
            var inputCount = _activeCompactText is null ? count : batchSize;
            var ids = new DenseTensor<long>(new[] { inputCount, TokenLength });
            var pad = _modelManifest.PadTokenId;
            ids.Buffer.Span.Fill(pad);
            for (var i = 0; i < count; i++)
            {
                var tokenIds = _tokenizer!.Encode(prompts[missing[start + i]]).Select(Convert.ToInt64).Take(TokenLength).ToArray();
                for (var j = 0; j < TokenLength; j++) ids[i, j] = j < tokenIds.Length ? tokenIds[j] : pad;
            }
            var rows = _activeCompactText is null ? RunFloat(_textSession!, ids, Dimensions) : RunCompactText(ids, inputCount);
            for (var i = 0; i < count; i++)
            {
                var row = rows.AsSpan(i * Dimensions, Dimensions).ToArray();
                Normalize(row);
                var prompt = prompts[missing[start + i]];
                output[missing[start + i]] = row;
                if (!_textCache.ContainsKey(prompt))
                {
                    while (_textCache.Count >= 512) _textCache.Remove(_textCacheOrder.Dequeue());
                    _textCache.Add(prompt, row); _textCacheOrder.Enqueue(prompt);
                }
            }
        }
        return output.ToList();
    }

    private float[] RunCompactText(DenseTensor<long> ids, int count)
    {
        var compact = _activeCompactText!;
        using var table = _tokenTable!.Acquire();
        var input = new DenseTensor<float>([count, TokenLength, Dimensions]);
        for (var i = 0; i < count * TokenLength; i++)
        {
            var token = ids.Buffer.Span[i];
            if (token < 0 || token >= compact.TokenRows) throw new InvalidDataException("Tokenizer ID is outside the compact vocabulary.");
            var row = table.Row((int)token);
            var destination = input.Buffer.Span.Slice(i * Dimensions, Dimensions);
            for (var d = 0; d < Dimensions; d++) destination[d] = (float)row[d];
        }
        using var results = _textSession!.Run([
            NamedOnnxValue.CreateFromTensor("input_ids", ids),
            NamedOnnxValue.CreateFromTensor(compact.EmbeddingInput, input)]);
        var output = results.First().AsTensor<float>().ToArray();
        if (output.Length != count * Dimensions) throw new InvalidDataException("Compact text output shape differs from the checkpoint.");
        return output;
    }

    private void ReleaseTextResources()
    {
        _textSession?.Dispose(); _textSession = null;
        _tokenTable?.Dispose(); _tokenTable = null;
        _activeCompactText = null;
    }

    internal void ReleaseTextSessionForMeasurement()
    {
        lock (_textLock)
        {
            ReleaseTextResources();
            _textCache.Clear(); _textCacheOrder.Clear();
        }
    }

    private static float[] RunFloat<T>(InferenceSession session, DenseTensor<T> input, int dimensions)
    {
        var inputName = session.InputMetadata.Keys.First();
        using var results = session.Run([NamedOnnxValue.CreateFromTensor(inputName, input)]);
        var tensor = results.First().AsTensor<float>();
        var data = tensor.ToArray();
        if (data.Length % dimensions != 0) throw new InvalidDataException($"Model output length {data.Length} is not divisible by {dimensions}.");
        return data;
    }

    private void LoadPersistedEmbeddings()
    {
        lock (_stateLock) LoadPersistedEmbeddingsCore();
    }

    private void LoadPersistedEmbeddingsCore()
    {
        var sidecar = TryReadSidecar();
        if (sidecar is null || sidecar.ModelHash != ModelHash || !File.Exists(EmbeddingsPath))
        {
            _matrix?.Dispose(); _matrix = null;
            _ids = [];
            _rowById = [];
            _rowNorms = [];
            _mean = [];
            Dimensions = _modelManifest.Dimensions;
            return;
        }
        if (sidecar.Dimensions != _modelManifest.Dimensions)
            throw new InvalidDataException("Cached embedding dimensions do not match the model manifest.");
        var ids = sidecar.PhotoIds.ToArray();
        if (ids.Length == 0) { _matrix?.Dispose(); _matrix = null; _ids = []; _rowById = []; _mean = []; _rowNorms = []; return; }
        var matrix = new ReadOnlyTensorMap<float>(EmbeddingsPath, ids.Length, sidecar.Dimensions);
        try
        {
            using var reader = matrix.Acquire();
            var (mean, norms) = ComputeNormalization(reader, ids.Length, sidecar.Dimensions);
            var lookup = ids.Select((id, row) => (id, row)).ToDictionary(p => p.id, p => p.row);
            var previous = _matrix;
            _matrix = matrix; _ids = ids; _rowById = lookup; _mean = mean; _rowNorms = norms;
            Dimensions = sidecar.Dimensions;
            _matrixWriteTicks = File.GetLastWriteTimeUtc(EmbeddingsPath).Ticks;
            _matrixCreatedUtc = sidecar.CreatedUtc;
            previous?.Dispose();
        }
        catch { matrix.Dispose(); throw; }
    }

    private static (float[] Mean, float[] Norms) ComputeNormalization(ReadOnlyTensorMap<float>.Lease matrix, int count, int dimensions)
    {
        var mean = new float[dimensions];
        for (var i = 0; i < count; i++)
        {
            var row = matrix.Row(i);
            for (var d = 0; d < dimensions; d++) mean[d] += row[d] / count;
        }
        var norms = new float[count];
        Parallel.For(0, count, i =>
        {
            var row = matrix.Row(i);
            double sum = 0;
            for (var d = 0; d < dimensions; d++) { var value = row[d] - mean[d]; sum += value * value; }
            if (!double.IsFinite(sum)) throw new InvalidDataException("Embedding matrix contains non-finite values.");
            norms[i] = (float)Math.Max(Math.Sqrt(sum), 1e-9);
        });
        return (mean, norms);
    }

    private ReadOnlyTensorMap<float>.Lease CaptureMatrix(out string[] ids, out float[] mean, out float[] norms)
    {
        lock (_stateLock)
        {
            ObjectDisposedException.ThrowIf(Volatile.Read(ref _disposed) != 0, this);
            if (_matrix is null) throw new InvalidOperationException("No embeddings are loaded. Run EnsureEmbeddedAsync first.");
            ids = _ids; mean = _mean; norms = _rowNorms;
            return _matrix.Acquire();
        }
    }

    private static void FillCentered(ReadOnlySpan<float> raw, float[] mean, float norm, Span<float> output)
    {
        // Preserve the original float32 subtract/divide and dot, not an algebraic
        // rearrangement whose rounding can change a sigmoid gate or a ranking tie.
        for (var d = 0; d < output.Length; d++) output[d] = (raw[d] - mean[d]) / norm;
    }

    private float[] QueryVector(IReadOnlyList<float[]> textRows)
    {
        var q = new float[Dimensions];
        foreach (var t in textRows)
            for (var d = 0; d < Dimensions; d++)
                q[d] += t[d] / textRows.Count;
        Normalize(q);
        return q;
    }

    private EmbeddingSidecar? TryReadSidecar()
    {
        if (!File.Exists(SidecarPath)) return null;
        using var file = File.OpenRead(SidecarPath);
        return JsonSerializer.Deserialize<EmbeddingSidecar>(file, JsonOptions);
    }

    private static bool IsSamePhoto(Photo? photo, PhotoRow row) =>
        photo is not null &&
        photo.SourceBytes == row.SourceBytes &&
        photo.SourceWriteUtcTicks == row.SourceWriteUtcTicks &&
        photo.MetadataFingerprint == row.MetadataFingerprint &&
        photo.ThumbnailPath == row.ThumbnailPath;

    private static string DefaultModelDirectory() =>
        Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "Nebula", "Models", "siglip2-so400m-patch16-384-onnx");

    private static void WriteMatrixAtomically(string path, float[] values)
    {
        var tmp = path + ".part";
        using (var file = File.Create(tmp)) file.Write(MemoryMarshal.AsBytes(values.AsSpan()));
        File.Move(tmp, path, overwrite: true);
    }

    private static void WriteJsonAtomically<T>(string path, T value)
    {
        var tmp = path + ".part";
        using (var file = File.Create(tmp)) JsonSerializer.Serialize(file, value, JsonOptions);
        File.Move(tmp, path, overwrite: true);
    }

    private void WriteCheckpoint(float[] matrix, bool[] have)
    {
        var rows = have.Select((ok, row) => (ok, row)).Where(x => x.ok).Select(x => x.row).ToArray();
        var compact = new float[rows.Length * _modelManifest.Dimensions];
        for (var i = 0; i < rows.Length; i++)
            Array.Copy(matrix, rows[i] * _modelManifest.Dimensions, compact, i * _modelManifest.Dimensions, _modelManifest.Dimensions);
        WriteMatrixAtomically(EmbeddingsPath, compact);
        var sidecar = new EmbeddingSidecar
        {
            ModelId = ModelId,
            ModelHash = ModelHash,
            Dimensions = _modelManifest.Dimensions,
            ImageSize = ImageSize,
            CreatedUtc = DateTimeOffset.UtcNow,
            Device = Device,
            PhotoIds = rows.Select(row => _library.Photos[row].Id).ToList(),
            Rows = rows.ToDictionary(row => _library.Photos[row].Id, row => PhotoRow.FromPhoto(_library.Photos[row]))
        };
        WriteJsonAtomically(SidecarPath, sidecar);
    }

    private static void Normalize(float[] values) => Normalize(values.AsSpan());
    private static void Normalize(Span<float> values)
    {
        double sum = 0;
        foreach (var v in values) sum += v * v;
        if (!double.IsFinite(sum)) throw new InvalidDataException("Embedding contains non-finite values.");
        var norm = (float)Math.Max(Math.Sqrt(sum), 1e-9);
        for (var i = 0; i < values.Length; i++) values[i] /= norm;
    }

    private static float Dot(ReadOnlySpan<float> a, ReadOnlySpan<float> b)
    {
        return System.Numerics.Tensors.TensorPrimitives.Dot(a, b);
    }

    private static float[] ZScore(float[] values)
    {
        var mean = values.Average();
        var variance = values.Sum(v => (v - mean) * (v - mean)) / Math.Max(values.Length, 1);
        var std = (float)Math.Sqrt(variance) + 1e-9f;
        return values.Select(v => (v - (float)mean) / std).ToArray();
    }

    private static void ZScoreInPlace(float[] values)
    {
        var mean = 0.0;
        for (var i = 0; i < values.Length; i++) mean += values[i];
        mean /= Math.Max(values.Length, 1);
        var variance = 0.0;
        for (var i = 0; i < values.Length; i++)
        {
            var d = values[i] - mean;
            variance += d * d;
        }
        var std = (float)Math.Sqrt(variance / Math.Max(values.Length, 1)) + 1e-9f;
        for (var i = 0; i < values.Length; i++) values[i] = (values[i] - (float)mean) / std;
    }

    private static float GSig(float x) => 1f / (1f + MathF.Exp(-(x - 2f)));

    private static IReadOnlyList<string> Prompts(string query)
    {
        var variants = QueryText.Variants(query);
        if (variants.Count == 0) return [$"a photo of {query}"];
        string[] templates = ["{}", "a photo of {}.", "a picture of {}.", "a snapshot of {}.", "a photo showing {}."];
        return variants.SelectMany(v => templates.Select(t => t.Replace("{}", v, StringComparison.Ordinal))).ToArray();
    }

    public sealed record EmbeddingRunReport(int TotalPhotos, int EmbeddedPhotos, int ReusedPhotos, TimeSpan Elapsed, string Device, double ImagesPerSecond, long PeakMemoryBytes);

    private sealed record ModelManifest
    {
        public string ModelId { get; init; } = EmbeddingIndex.ModelId;
        public string Revision { get; init; } = "";
        public string VisionFile { get; init; } = "vision.onnx";
        public string TextFile { get; init; } = "text.onnx";
        public int Dimensions { get; init; } = 1152;
        public long PadTokenId { get; init; }
        public string ModelHash { get; init; } = "";
        public CompactTextManifest? CompactText { get; init; }
    }

    private sealed record CompactTextManifest
    {
        public string Id { get; init; } = "";
        public string SourceModelHash { get; init; } = "";
        public string File { get; init; } = "";
        public string TokenFile { get; init; } = "";
        public int TokenRows { get; init; }
        public int TokenColumns { get; init; }
        public string EmbeddingInput { get; init; } = "";
        public string TokenType { get; init; } = "";
        public bool Enabled { get; init; }
        public int IdleReleaseSeconds { get; init; }
        public double VerifiedReloadMs { get; init; }
        public List<ModelFileRecord> Files { get; init; } = [];
    }

    private sealed record ModelFileRecord
    {
        public string Path { get; init; } = "";
        public long Bytes { get; init; }
        public string Sha256 { get; init; } = "";
    }


    private sealed record EmbeddingSidecar
    {
        public string ModelId { get; init; } = "";
        public string ModelHash { get; init; } = "";
        public int Dimensions { get; init; }
        public int ImageSize { get; init; }
        public DateTimeOffset CreatedUtc { get; init; }
        public string Device { get; init; } = "";
        public List<string> PhotoIds { get; init; } = [];
        public Dictionary<string, PhotoRow> Rows { get; init; } = [];
    }

    private sealed record PhotoRow
    {
        public long SourceBytes { get; init; }
        public long SourceWriteUtcTicks { get; init; }
        public string MetadataFingerprint { get; init; } = "";
        public string ThumbnailPath { get; init; } = "";
        public static PhotoRow FromPhoto(Photo p) => new()
        {
            SourceBytes = p.SourceBytes,
            SourceWriteUtcTicks = p.SourceWriteUtcTicks,
            MetadataFingerprint = p.MetadataFingerprint,
            ThumbnailPath = p.ThumbnailPath
        };
    }
}
