using Nebula.Core;
using System.Collections.Concurrent;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using Windows.Graphics.Imaging;
using Windows.Security.Cryptography;
using Windows.Storage;
using Windows.Storage.Streams;
using FileAttributes = System.IO.FileAttributes;

namespace Nebula_Windows.Services;

public sealed record ImportProgress(int Processed, int Total, int Imported, string Message, Library? Preview = null);

public sealed class LibraryService
{
    public static string DataRoot { get; } = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "Nebula", "Data");
    private static readonly HashSet<string> Extensions = [".jpg", ".jpeg", ".png", ".webp", ".tif", ".tiff", ".bmp", ".heic", ".heif"];
    private static readonly JsonSerializerOptions JsonOptions = new() { WriteIndented = true };
    public static string PreferencesPath => Path.Combine(DataRoot, "preferences.json");

    private static readonly Lock LogLock = new();
    private const long LogLimitBytes = 8 * 1024 * 1024;

    /// Appends a line to application.log. Never throws: logging must not turn an error path into a
    /// second failure. When the log passes 8 MB it becomes application.old.log and a new one starts.
    public static void Log(string message)
    {
        try
        {
            lock (LogLock)
            {
                Directory.CreateDirectory(DataRoot);
                var path = Path.Combine(DataRoot, "application.log");
                var file = new FileInfo(path);
                if (file.Exists && file.Length > LogLimitBytes) File.Move(path, Path.Combine(DataRoot, "application.old.log"), true);
                File.AppendAllText(path, $"{DateTimeOffset.Now:O} {message}{Environment.NewLine}");
            }
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException) { }
    }

    public async Task<Library?> LastLibraryAsync()
    {
        if (!File.Exists(PreferencesPath)) return null;
        var source = JsonSerializer.Deserialize<string>(await File.ReadAllTextAsync(PreferencesPath));
        if (string.IsNullOrWhiteSpace(source)) return null;
        var path = Path.Combine(LibraryFolder(source), "library.json");
        if (!File.Exists(path)) return null;
        return JsonSerializer.Deserialize<Library>(await File.ReadAllTextAsync(path));
    }

    public static string LibraryFolder(string source) => Path.Combine(DataRoot, "Libraries",
        Convert.ToHexString(SHA256.HashData(Encoding.UTF8.GetBytes(Path.GetFullPath(source).ToUpperInvariant())))[..24]);

    private static async Task WriteAtomicAsync<T>(string path, T value, CancellationToken token)
    {
        var temporary = path + ".tmp";
        await File.WriteAllTextAsync(temporary, JsonSerializer.Serialize(value, JsonOptions), token);
        File.Move(temporary, path, true);
    }

    public Task<Library> ImportAsync(string source, IProgress<ImportProgress> progress, CancellationToken token) =>
        Task.Run(async () =>
        {
            source = Path.GetFullPath(source);
            if (!Directory.Exists(source)) throw new DirectoryNotFoundException($"The source folder is unavailable: {source}");
            var output = LibraryFolder(source);
            Directory.CreateDirectory(Path.Combine(output, "thumbs"));
            var errors = new List<string>();
            var files = new List<string>();
            var directories = new Stack<string>();
            directories.Push(source);
            while (directories.Count > 0)
            {
                token.ThrowIfCancellationRequested();
                var directory = directories.Pop();
                try
                {
                    foreach (var child in Directory.EnumerateDirectories(directory))
                    {
                        var attributes = File.GetAttributes(child);
                        if ((attributes & FileAttributes.ReparsePoint) == 0 && !IsCloudOnly(attributes)) directories.Push(child);
                    }
                    files.AddRange(Directory.EnumerateFiles(directory).Where(path => Extensions.Contains(Path.GetExtension(path).ToLowerInvariant())));
                }
                catch (Exception error) when (error is UnauthorizedAccessException or IOException)
                {
                    errors.Add($"{directory}: {error.Message}");
                }
            }
            files.Sort(StringComparer.OrdinalIgnoreCase);
            var metadata = new Dictionary<string, JsonElement>(StringComparer.OrdinalIgnoreCase);
            var metManifest = Path.Combine(source, "met.jsonl");
            if (File.Exists(metManifest))
            {
                await using var stream = new FileStream(metManifest, FileMode.Open, FileAccess.Read,
                    FileShare.ReadWrite | FileShare.Delete, 65536, FileOptions.Asynchronous);
                if (stream.Length > 128 * 1024 * 1024) throw new InvalidDataException("Museum metadata exceeds the 128 MiB import budget.");
                var snapshot = new byte[(int)stream.Length];
                await stream.ReadExactlyAsync(snapshot, token);
                var text = Encoding.UTF8.GetString(snapshot);
                if (text.Length > 0 && !text.EndsWith('\n'))
                {
                    var lastComplete = text.LastIndexOf('\n');
                    text = lastComplete < 0 ? "" : text[..(lastComplete + 1)];
                    Log("Museum metadata is still being appended; the unfinished final record will be read on rescan.");
                }
                foreach (var line in text.Split('\n'))
                {
                    if (string.IsNullOrWhiteSpace(line)) continue;
                    using var document = JsonDocument.Parse(line);
                    if (document.RootElement.TryGetProperty("saved", out var saved) && saved.GetString() is { } filename)
                        metadata[filename] = document.RootElement.Clone();
                }
            }
            var cachePath = Path.Combine(output, "library.json");
            var previous = File.Exists(cachePath)
                ? JsonSerializer.Deserialize<Library>(await File.ReadAllTextAsync(cachePath, token)) : null;
            var cached = previous?.Photos.ToDictionary(photo => photo.OriginalPath, StringComparer.OrdinalIgnoreCase)
                ?? new Dictionary<string, Photo>(StringComparer.OrdinalIgnoreCase);
            var results = new Photo?[files.Count];
            var errorsBag = new ConcurrentBag<string>();
            var skippedCount = 0;
            var processed = 0;
            var imported = 0;
            var progressLock = new object();
            var maxDegree = Math.Max(1, Environment.ProcessorCount - 2);
            await Parallel.ForEachAsync(Enumerable.Range(0, files.Count), new ParallelOptions
            {
                MaxDegreeOfParallelism = maxDegree,
                CancellationToken = token
            }, async (i, ct) =>
            {
                ct.ThrowIfCancellationRequested();
                var path = files[i];
                try
                {
                    var info = new FileInfo(path);
                    if (IsCloudOnly(info.Attributes))
                    {
                        Interlocked.Increment(ref skippedCount);
                        errorsBag.Add($"{info.Name}: cloud-only file skipped without hydration.");
                        return;
                    }
                    Photo photo;
                    metadata.TryGetValue(info.Name, out var record);
                    var metadataFingerprint = record.ValueKind == JsonValueKind.Object
                        ? Convert.ToHexString(SHA256.HashData(Encoding.UTF8.GetBytes(record.GetRawText()))) : "";
                    if (cached.TryGetValue(path, out var existing) && File.Exists(existing.ThumbnailPath) &&
                        existing.SourceBytes == info.Length && existing.SourceWriteUtcTicks == info.LastWriteTimeUtc.Ticks &&
                        existing.LabLightness.HasValue && existing.MetadataFingerprint == metadataFingerprint)
                    {
                        photo = existing;
                    }
                    else
                    {
                        photo = await DecodeAsync(info, output, record, metadataFingerprint, ct);
                    }
                    results[i] = photo;
                    Interlocked.Increment(ref imported);
                }
                catch (Exception error) when (error is not OperationCanceledException and not OutOfMemoryException)
                {
                    Interlocked.Increment(ref skippedCount);
                    errorsBag.Add($"{Path.GetFileName(path)}: {error.Message}");
                    Log($"Import failed for {path}: {error}");
                }
                var now = Interlocked.Increment(ref processed);
                if (now % 50 == 0 || now == files.Count)
                {
                    lock (progressLock)
                    {
                        var previewPhotos = OrderedDistinct(results);
                        var preview = previewPhotos.Count >= 50 && previewPhotos.Count < 70
                            ? new Library(source, previewPhotos, Volatile.Read(ref skippedCount), errorsBag.Order(StringComparer.Ordinal).ToList()) : null;
                        progress.Report(new(now, files.Count, Volatile.Read(ref imported), Path.GetFileName(path), preview));
                    }
                }
            });
            token.ThrowIfCancellationRequested();
            var photos = OrderedDistinct(results);
            var skipped = skippedCount + results.Count(p => p is not null) - photos.Count;
            errors.AddRange(errorsBag.Order(StringComparer.Ordinal));
            var library = new Library(source, photos, skipped, errors);
            await WriteAtomicAsync(cachePath, library, token);
            await WriteAtomicAsync(PreferencesPath, source, token);
            Log($"Imported {photos.Count}/{files.Count} images from {source}; skipped {skipped}; {errors.Count} errors.");
            return library;
        }, token);

    private static List<Photo> OrderedDistinct(IReadOnlyList<Photo?> results)
    {
        var photos = new List<Photo>();
        var identities = new HashSet<string>();
        for (var i = 0; i < results.Count; i++)
            if (results[i] is { } photo && identities.Add(photo.Id))
                photos.Add(photo);
        return photos;
    }

    private static bool IsCloudOnly(FileAttributes attributes) => ((int)attributes & (0x1000 | 0x40000 | 0x400000)) != 0;

    public static Task<SoftwareBitmap> PreviewAsync(Photo photo, CancellationToken token) => Task.Run(async () =>
    {
        if (IsCloudOnly(File.GetAttributes(photo.OriginalPath)))
            throw new IOException("The original is cloud-only; Nebula will not download it automatically.");
        var file = await StorageFile.GetFileFromPathAsync(photo.OriginalPath).AsTask(token);
        using var stream = await file.OpenAsync(FileAccessMode.Read).AsTask(token);
        var decoder = await BitmapDecoder.CreateAsync(stream).AsTask(token);
        if ((long)decoder.PixelWidth * decoder.PixelHeight > 100_000_000)
            throw new InvalidDataException("The original exceeds the 100-megapixel preview budget.");
        var scale = Math.Min(1.0, 1200.0 / Math.Max(decoder.PixelWidth, decoder.PixelHeight));
        var transform = new BitmapTransform
        {
            ScaledWidth = Math.Max(1u, (uint)Math.Round(decoder.PixelWidth * scale)),
            ScaledHeight = Math.Max(1u, (uint)Math.Round(decoder.PixelHeight * scale)),
            InterpolationMode = BitmapInterpolationMode.Fant
        };
        return await decoder.GetSoftwareBitmapAsync(BitmapPixelFormat.Bgra8, BitmapAlphaMode.Premultiplied,
            transform, ExifOrientationMode.RespectExifOrientation, ColorManagementMode.ColorManageToSRgb).AsTask(token);
    }, token);

    private static async Task<Photo> DecodeAsync(FileInfo file, string output, JsonElement metadata, string metadataFingerprint, CancellationToken token)
    {
        static string Text(JsonElement data, string key) => data.ValueKind == JsonValueKind.Object &&
            data.TryGetProperty(key, out var value) && value.ValueKind == JsonValueKind.String ? value.GetString() ?? "" : "";
        string hash;
        await using (var input = file.OpenRead())
            hash = Convert.ToHexString(await SHA256.HashDataAsync(input, token)).ToLowerInvariant();
        // A plain file stream, not a StorageFile broker object per photo (50K of those exhaust memory).
        await using var fileStream = new FileStream(file.FullName, FileMode.Open, FileAccess.Read, FileShare.Read, 1 << 16, FileOptions.Asynchronous);
        using var source = fileStream.AsRandomAccessStream();
        var decoder = await BitmapDecoder.CreateAsync(source).AsTask(token);
        if ((long)decoder.PixelWidth * decoder.PixelHeight > 100_000_000)
            throw new InvalidDataException("Image exceeds the 100-megapixel import budget.");
        var scale = Math.Min(1.0, 256.0 / Math.Max(decoder.PixelWidth, decoder.PixelHeight));
        var transform = new BitmapTransform
        {
            ScaledWidth = Math.Max(1u, (uint)Math.Round(decoder.PixelWidth * scale)),
            ScaledHeight = Math.Max(1u, (uint)Math.Round(decoder.PixelHeight * scale)),
            InterpolationMode = BitmapInterpolationMode.Fant
        };
        using var bitmap = await decoder.GetSoftwareBitmapAsync(BitmapPixelFormat.Bgra8, BitmapAlphaMode.Ignore,
            transform, ExifOrientationMode.RespectExifOrientation, ColorManagementMode.ColorManageToSRgb).AsTask(token);
        var buffer = new Windows.Storage.Streams.Buffer((uint)(bitmap.PixelWidth * bitmap.PixelHeight * 4));
        bitmap.CopyToBuffer(buffer);
        CryptographicBuffer.CopyToByteArray(buffer, out var pixels);
        double red = 0, green = 0, blue = 0, lightness = 0, labA = 0, labB = 0;
        for (var i = 0; i < pixels.Length; i += 4)
        {
            blue += pixels[i]; green += pixels[i + 1]; red += pixels[i + 2];
            var lab = Nebula.Core.Layout.ToOklab(pixels[i + 2] / 255f, pixels[i + 1] / 255f, pixels[i] / 255f);
            lightness += lab.X; labA += lab.Y; labB += lab.Z;
        }
        var divisor = pixels.Length / 4 * 255.0;
        var thumbnail = Path.Combine(output, "thumbs", hash + ".jpg");
        // Identical files (same hash) decode in parallel: each writes its own temporary, last rename wins.
        var temporaryPath = $"{thumbnail}.{Guid.NewGuid():N}.tmp";
        var temporary = await StorageFile.GetFileFromPathAsync(await CreateEmptyAsync(temporaryPath, token)).AsTask(token);
        using (var stream = await temporary.OpenAsync(FileAccessMode.ReadWrite).AsTask(token))
        {
            var encoder = await BitmapEncoder.CreateAsync(BitmapEncoder.JpegEncoderId, stream).AsTask(token);
            encoder.SetSoftwareBitmap(bitmap);
            await encoder.FlushAsync().AsTask(token);
        }
        File.Move(temporaryPath, thumbnail, true);
        DateTimeOffset? taken = null;
        try
        {
            // Read the EXIF capture date from the decoder that is already open. The shell property
            // system (StorageFile.Properties) creates a property handler per file and, across tens of
            // thousands of parallel imports, fails with OutOfMemory.
            var values = await decoder.BitmapProperties.GetPropertiesAsync(["System.Photo.DateTaken"]).AsTask(token);
            if (values.TryGetValue("System.Photo.DateTaken", out var value) && value.Value is DateTimeOffset date && date.Year >= 1900)
                taken = date;
        }
        catch (Exception error) when (error is System.Runtime.InteropServices.COMException or ArgumentException or NotSupportedException)
        {
            Log($"Capture metadata unavailable for {file.FullName}; decoded image retained: {error.Message}");
        }
        var title = Text(metadata, "title");
        return new Photo
        {
            Id = hash, OriginalPath = file.FullName, ThumbnailPath = thumbnail,
            Title = string.IsNullOrWhiteSpace(title) ? Path.GetFileNameWithoutExtension(file.Name) : title,
            Artist = Text(metadata, "artist"), Department = Text(metadata, "department"), Medium = Text(metadata, "medium"),
            ArtworkDate = Text(metadata, "date"), Credit = Text(metadata, "credit"),
            SourceUrl = Text(metadata, "page"), TakenAt = taken,
            Width = (int)decoder.OrientedPixelWidth, Height = (int)decoder.OrientedPixelHeight,
            SourceBytes = file.Length, SourceWriteUtcTicks = file.LastWriteTimeUtc.Ticks,
            MetadataFingerprint = metadataFingerprint,
            Red = (float)(red / divisor), Green = (float)(green / divisor), Blue = (float)(blue / divisor),
            LabLightness = (float)(lightness / (pixels.Length / 4)), LabA = (float)(labA / (pixels.Length / 4)),
            LabB = (float)(labB / (pixels.Length / 4))
        };
    }

    private static async Task<string> CreateEmptyAsync(string path, CancellationToken token)
    {
        await File.WriteAllBytesAsync(path, [], token);
        return path;
    }
}
