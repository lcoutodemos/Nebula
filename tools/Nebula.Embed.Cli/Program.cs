using Nebula.Core;
using Nebula.Embed;
using System.Diagnostics;
using System.Numerics;
using System.Text.Json;

if (args.Length > 0 && args[0] == "--memory-bench")
    return await MemoryBench.RunAsync(args.Skip(1).ToArray());
if (args.Length == 2 && args[0] == "--memory-self-test")
{
    MemoryBench.VerifyMappings(args[1]);
    return 0;
}

if (args.Length > 0 && args[0] == "--bench")
{
    await RunEngineBench(args.Skip(1).ToArray());
    return 0;
}

if (args.Length < 1 || args[0] is "-h" or "--help")
{
    Console.WriteLine("Usage: dotnet run --project tools\\Nebula.Embed.Cli -- <library dir> [--query \"...\"] [--groups] [--models <model dir>]");
    Console.WriteLine("       dotnet run --project tools\\Nebula.Embed.Cli -- --bench [--library <library dir>] [--stage <stage>] [--sizes 5000,50000] [--out <dir>]");
    Console.WriteLine("       dotnet run --project tools\\Nebula.Embed.Cli -- --memory-bench --library <library dir> [--out <dir>]");
    return args.Length == 0 ? 1 : 0;
}

var libraryDir = Path.GetFullPath(args[0]);
string? query = null;
string? modelDir = null;
var buildGroups = false;
for (var i = 1; i < args.Length; i++)
{
    if (args[i] == "--query" && i + 1 < args.Length) query = args[++i];
    else if (args[i] == "--models" && i + 1 < args.Length) modelDir = args[++i];
    else if (args[i] == "--groups") buildGroups = true;
    else throw new ArgumentException($"Unknown argument: {args[i]}");
}

var validationDir = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "Nebula", "Data");
Directory.CreateDirectory(validationDir);
var validationPath = Path.Combine(validationDir, "embed-validation.json");

using var index = EmbeddingIndex.Load(libraryDir, modelDir);
var process = Process.GetCurrentProcess();
var progress = new Progress<string>(Console.WriteLine);
var report = await index.EnsureEmbeddedAsync(progress);
process.Refresh();

var library = JsonSerializer.Deserialize<Library>(File.ReadAllText(Path.Combine(libraryDir, "library.json")),
    new JsonSerializerOptions(JsonSerializerDefaults.Web) { PropertyNameCaseInsensitive = true })!;
var byId = library.Photos.ToDictionary(p => p.Id);
var sampleQueries = new[]
{
    "a blue and white porcelain plate",
    "armor",
    "a portrait of a woman",
    "egyptian hieroglyphs",
    "a horse"
};
if (!string.IsNullOrWhiteSpace(query)) sampleQueries = [query];

var results = new Dictionary<string, object[]>();
foreach (var q in sampleQueries)
{
    var hits = await index.SearchAsync(q, CancellationToken.None);
    results[q] = hits.Take(5).Select(h =>
    {
        var p = byId[h.Id];
        return new
        {
            h.Id,
            Score = h.Score,
            p.Title,
            p.Department
        };
    }).Cast<object>().ToArray();
    Console.WriteLine();
    Console.WriteLine(q);
    foreach (var hit in results[q]) Console.WriteLine(JsonSerializer.Serialize(hit));
}

VisualGroupSet? groupSet = null;
if (buildGroups)
{
    groupSet = await VisualGroups.LoadOrBuildAsync(index, library.Photos.Select(p => p.Id).ToArray(), libraryDir, progress, CancellationToken.None);
    Console.WriteLine();
    Console.WriteLine($"groups: {groupSet.Groups.Count} groups, {groupSet.Supers.Count} supers, {groupSet.UngroupedCount} ungrouped, {groupSet.Elapsed}");
    Console.WriteLine("top supers:");
    foreach (var super in groupSet.Supers.OrderByDescending(g => g.Size).Take(15))
        Console.WriteLine($"{super.Id,3}  {super.Title,-42} {super.Size,5} photos  leaves {super.Leaves.Length}");
    Console.WriteLine("sample groups:");
    foreach (var group in groupSet.Groups.OrderByDescending(g => g.Size).Take(10))
        Console.WriteLine($"{group.Id,3}  {group.Title,-42} {group.Size,5} photos  super {group.SuperId}");
}

var manifestPath = modelDir is null
    ? Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "Nebula", "Models", "siglip2-so400m-patch16-384-onnx", "manifest.json")
    : Path.Combine(modelDir, "manifest.json");
object? manifest = File.Exists(manifestPath) ? JsonSerializer.Deserialize<JsonElement>(File.ReadAllText(manifestPath)) : null;
var validation = new
{
    CreatedUtc = DateTimeOffset.UtcNow,
    LibraryDir = libraryDir,
    Model = new
    {
        EmbeddingIndex.ModelId,
        index.ModelHash,
        Manifest = manifest
    },
    report.TotalPhotos,
    report.EmbeddedPhotos,
    report.ReusedPhotos,
    report.Elapsed,
    ImagesPerSecond = report.ImagesPerSecond,
    report.Device,
    PeakWorkingSetBytes = process.PeakWorkingSet64,
    EmbeddingPeakWorkingSetBytes = report.PeakMemoryBytes,
    QueryResults = results,
    Groups = groupSet is null ? null : new
    {
        Count = groupSet.Groups.Count,
        SuperCount = groupSet.Supers.Count,
        groupSet.UngroupedCount,
        groupSet.Elapsed,
        TopSupers = groupSet.Supers.OrderByDescending(g => g.Size).Take(15).Select(g => new
        {
            g.Id,
            g.Title,
            g.Label,
            g.NamedBy,
            g.Size,
            Leaves = g.Leaves.Length
        }),
        SampleGroups = groupSet.Groups.OrderByDescending(g => g.Size).Take(10).Select(g => new
        {
            g.Id,
            g.Title,
            g.Label,
            g.NamedBy,
            g.Size,
            g.SuperId
        })
    }
};
File.WriteAllText(validationPath, JsonSerializer.Serialize(validation, new JsonSerializerOptions(JsonSerializerDefaults.Web) { WriteIndented = true }));
Console.WriteLine();
Console.WriteLine($"Validation written to {validationPath}");
Console.WriteLine($"{report.TotalPhotos} photos, {report.EmbeddedPhotos} embedded, {report.ReusedPhotos} reused, {report.ImagesPerSecond:F2} images/s, {report.Device}");
return 0;

static void RunBench()
{
    var rows = new List<(string Name, int N, TimeSpan Time, string Notes)>();
    foreach (var n in new[] { 50_000, 100_000 })
    {
        var photos = SyntheticPhotos(n);
        foreach (var arrangement in new[] { Arrangement.Cloud, Arrangement.Colour, Arrangement.Time, Arrangement.Grid })
        {
            var sw = Stopwatch.StartNew();
            var positions = Layout.Positions(photos, arrangement);
            sw.Stop();
            rows.Add(($"Layout.{arrangement}", n, sw.Elapsed, positions.Length == n ? "ok" : "bad count"));
        }
    }

    foreach (var (n, d) in new[] { (5_000, 64), (10_000, 64) })
    {
        var vectors = SyntheticVectors(n, d);
        var sw = Stopwatch.StartNew();
        var positions = Layout.SimilarityPositions(vectors, n, d);
        sw.Stop();
        rows.Add(($"SimilarityPositions d={d}", n, sw.Elapsed, positions.Length == n ? "ok" : "bad count"));
    }

    Console.WriteLine("| Bench | N | Seconds | Notes |");
    Console.WriteLine("| --- | ---: | ---: | --- |");
    foreach (var row in rows)
        Console.WriteLine($"| {row.Name} | {row.N:N0} | {row.Time.TotalSeconds:F3} | {row.Notes} |");
}

static Photo[] SyntheticPhotos(int count)
{
    var output = new Photo[count];
    for (var i = 0; i < count; i++)
    {
        var t = new DateTimeOffset(2000 + i % 24, 1 + i % 12, 1 + i % 27, 0, 0, 0, TimeSpan.Zero);
        output[i] = new Photo
        {
            Id = i.ToString("x16"),
            OriginalPath = $"C:\\synthetic\\{i}.jpg",
            ThumbnailPath = $"C:\\synthetic\\thumbs\\{i}.jpg",
            Title = "Synthetic",
            Width = 1600,
            Height = 1200,
            TakenAt = t,
            Red = ((i * 37) & 255) / 255f,
            Green = ((i * 73) & 255) / 255f,
            Blue = ((i * 109) & 255) / 255f
        };
    }
    return output;
}

static float[] SyntheticVectors(int rows, int dims)
{
    var result = new float[rows * dims];
    var centers = new float[64 * dims];
    var rng = new Random(42);
    for (var c = 0; c < 64; c++)
    {
        var norm = 0.0;
        for (var d = 0; d < dims; d++)
        {
            var v = (float)(rng.NextDouble() * 2 - 1);
            centers[c * dims + d] = v;
            norm += v * v;
        }

        norm = Math.Sqrt(norm);
        for (var d = 0; d < dims; d++) centers[c * dims + d] /= (float)norm;
    }
    Parallel.For(0, rows, i =>
    {
        var c = i % 64;
        var local = new Random(i * 7919 + 17);
        var norm = 0.0;
        for (var d = 0; d < dims; d++)
        {
            var v = centers[c * dims + d] + (float)((local.NextDouble() * 2 - 1) * 0.05);
            result[i * dims + d] = v;
            norm += v * v;
        }
        norm = Math.Sqrt(norm);
        for (var d = 0; d < dims; d++) result[i * dims + d] /= (float)norm;
    });
    return result;
}

static async Task RunEngineBench(string[] options)
{
    var allowed = new HashSet<string>(["--library", "--out", "--stage", "--sizes", "--dimensions", "--input"], StringComparer.Ordinal);
    for (var i = 0; i < options.Length; i += 2)
        if (!allowed.Contains(options[i]) || i + 1 >= options.Length) throw new ArgumentException($"Invalid benchmark option: {options[i]}");
    string? Option(string name)
    {
        var at = Array.IndexOf(options, name);
        if (at < 0) return null;
        if (at + 1 == options.Length) throw new ArgumentException($"Missing value for {name}.");
        return options[at + 1];
    }
    var local = Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData);
    var source = Option("--library");
    var output = Path.GetFullPath(Option("--out") ?? Path.Combine(local, "Nebula", "Bench", "engine"));
    if (source is not null && output.Equals(Path.GetFullPath(source), StringComparison.OrdinalIgnoreCase))
        throw new ArgumentException("Benchmark output must not be the application's library.");
    Directory.CreateDirectory(output);
    var stage = Option("--stage") ?? "algorithms";
    if (stage == "layouts") { RunBench(); return; }
    if (!new[] { "algorithms", "reference", "knn", "layout", "p10", "hdb", "pca", "embed", "search", "groups", "verify", "resume" }.Contains(stage))
        throw new ArgumentException($"Unknown benchmark stage: {stage}");
    var counts = (Option("--sizes") ?? "5000,50000,100000").Split(',').Select(int.Parse).ToArray();
    var dims = int.Parse(Option("--dimensions") ?? "1152");
    Console.WriteLine($"Engine benchmark: {Environment.ProcessorCount} logical CPUs, SIMD width {Vector<float>.Count}, {System.Runtime.InteropServices.RuntimeInformation.ProcessArchitecture}");
    var real = source is not null && File.Exists(Path.Combine(source, "embeddings.bin")) ? ReadFloats(Path.Combine(source, "embeddings.bin")) : [];
    var report = new List<object>();
    foreach (var n in counts)
    {
        if (n < 2 || dims <= 0) throw new ArgumentOutOfRangeException(nameof(options));
        var x = real.Length >= n * dims && dims == 1152 ? real.AsSpan(0, n * dims).ToArray() :
            real.Length > 0 && dims == 1152 ? ExpandReal(real, n, dims) : SyntheticVectors(n, dims);
        var centered = CenterRows(x, n, dims);
        void Record(string name, Stopwatch sw, object? detail = null)
        {
            var row = new { name, n, dimensions = name == "hdbscan" ? 10 : dims, seconds = sw.Elapsed.TotalSeconds, detail };
            report.Add(row);
            Console.WriteLine(JsonSerializer.Serialize(row));
            var json = JsonSerializer.Serialize(report, new JsonSerializerOptions { WriteIndented = true });
            File.WriteAllText(Path.Combine(output, "measurements.json"), json);
            File.WriteAllText(Path.Combine(output, $"{stage}-measurements.json"), json);
        }
        if (stage == "resume")
        {
            var path = Path.GetFullPath(Option("--input") ?? throw new ArgumentException("Resume needs --input pointing to a benchmark library inside --out."));
            if (!path.StartsWith(output.TrimEnd(Path.DirectorySeparatorChar) + Path.DirectorySeparatorChar, StringComparison.OrdinalIgnoreCase))
                throw new ArgumentException("Resume may only write a benchmark library inside --out.");
            var sw = Stopwatch.StartNew();
            using var index = EmbeddingIndex.Load(path);
            var result = await index.EnsureEmbeddedAsync();
            sw.Stop();
            if (result.EmbeddedPhotos != 0 || result.ReusedPhotos != n || result.Device != "not initialized")
                throw new InvalidOperationException("Cold resume did not reuse all rows without creating an inference session.");
            Record("cold-cache-resume", sw, result);
        }
        if (stage == "verify")
        {
            var namer = typeof(VisualGroups).GetNestedType("VocabularyNamer", System.Reflection.BindingFlags.NonPublic)!;
            var select = namer.GetMethod("SelectKth", System.Reflection.BindingFlags.Static | System.Reflection.BindingFlags.NonPublic)!;
            var zscore = namer.GetMethod("ZScoreColumns", System.Reflection.BindingFlags.Static | System.Reflection.BindingFlags.NonPublic)!;
            var rng = new Random(42);
            foreach (var length in new[] { 1, 2, 17, 103 })
            {
                var values = Enumerable.Range(0, length).Select(_ => (float)rng.Next(0, 10)).ToArray();
                foreach (var k in Enumerable.Range(0, length))
                    if (select.Invoke(null, [values.ToArray(), length, k]) is not float value || value != values.Order().ElementAt(k))
                        throw new InvalidOperationException("Naming percentile differs from exact sorted reference.");
            }
            const int nr = 71, nc = 37;
            var input = Enumerable.Range(0, nr * nc).Select(_ => (float)rng.NextDouble()).ToArray();
            var expected = input.ToArray();
            for (var j = 0; j < nc; j++)
            {
                double mean = 0, variance = 0;
                for (var i = 0; i < nr; i++) mean += input[i * nc + j];
                mean /= nr;
                for (var i = 0; i < nr; i++) { var delta = input[i * nc + j] - mean; variance += delta * delta; }
                var sd = Math.Sqrt(variance / nr) + 1e-9;
                for (var i = 0; i < nr; i++) expected[i * nc + j] = (float)((input[i * nc + j] - mean) / sd);
            }
            if (zscore.Invoke(null, [input, nr, nc]) is not float[] actual || !actual.SequenceEqual(expected))
                throw new InvalidOperationException("Blocked naming z-scores differ from the scalar reference.");
            Console.WriteLine("PASS exact naming percentile and bit-identical blocked column z-scores");
        }
        if (stage is "knn" or "algorithms" or "reference")
        {
            var sw = Stopwatch.StartNew();
            var knn = Layout.NearestEmbeddingRows(centered, n, dims, 12);
            sw.Stop();
            Record("exact-knn-12", sw, new { gflops = 2.0 * n * n * dims / sw.Elapsed.TotalSeconds / 1e9 });
            WriteInts(Path.Combine(output, $"knn-{n}.bin"), knn.SelectMany(r => r).ToArray());
        }
        if (stage == "pca")
        {
            var sw = Stopwatch.StartNew();
            var pca = ExactNeighbors.PrincipalComponents(centered, n, dims, Math.Min(100, dims));
            sw.Stop(); Record("pca-100", sw);
            WriteFloats(Path.Combine(output, $"pca-{n}.bin"), pca);
        }
        if (stage is "layout" or "algorithms" or "reference")
        {
            var sw = Stopwatch.StartNew();
            var layout = Layout.SimilarityPositions(x, n, dims);
            sw.Stop();
            Record("similarity", sw);
            WriteFloats(Path.Combine(output, $"layout-{n}.bin"), layout.SelectMany(v => new[] { v.X, v.Y, v.Z }).ToArray());
        }
        if (stage is "p10" or "algorithms" or "reference")
        {
            var sw = Stopwatch.StartNew();
            var p10 = Layout.PaCMAPProjection(centered, n, dims, 10, 0);
            sw.Stop();
            Record("pacmap-10", sw);
            WriteFloats(Path.Combine(output, $"p10-{n}.bin"), p10);
            sw.Restart();
            var labels = Hdbscan.LeafClusters(p10, n, 10);
            sw.Stop();
            Record("hdbscan", sw, new { groups = labels.Where(v => v >= 0).Distinct().Count() });
            WriteInts(Path.Combine(output, $"labels-{n}.bin"), labels);
        }
        if (stage == "hdb")
        {
            var input = Option("--input") ?? Path.Combine(output, $"p10-{n}.bin");
            var p10 = ReadFloats(input);
            var sw = Stopwatch.StartNew();
            var labels = Hdbscan.LeafClusters(p10, n, 10);
            sw.Stop();
            Record("hdbscan", sw);
            WriteInts(Path.Combine(output, $"labels-{n}.bin"), labels);
        }
        if (stage is "embed" or "search" or "groups")
        {
            if (source is null) throw new ArgumentException($"The {stage} stage needs --library <library folder>.");
            var jsonOptions = new JsonSerializerOptions(JsonSerializerDefaults.Web) { PropertyNameCaseInsensitive = true, WriteIndented = true };
            var library = JsonSerializer.Deserialize<Library>(File.ReadAllText(Path.Combine(source, "library.json")), jsonOptions)!;
            var chosen = Enumerable.Range(0, library.Photos.Count).OrderBy(i => new Random(i ^ 42).Next()).ToArray();
            var sourceRows = Enumerable.Range(0, n).Select(i => stage == "embed" ? chosen[i % chosen.Length] : i % library.Photos.Count).ToArray();
            var photos = Enumerable.Range(0, n).Select(i =>
            {
                var photo = library.Photos[sourceRows[i]];
                return i < library.Photos.Count ? photo : photo with { Id = $"{photo.Id}-bench-{i}" };
            }).ToArray();
            var dir = Path.Combine(output, $"{stage}-{n}");
            if (stage == "embed" && File.Exists(Path.Combine(dir, "embeddings.bin")))
                throw new InvalidOperationException("Fresh throughput requires a new --out; existing benchmark embeddings will not be overwritten.");
            Directory.CreateDirectory(dir);
            File.WriteAllText(Path.Combine(dir, "library.json"), JsonSerializer.Serialize(new Library(library.Source, photos.ToList(), library.Skipped, library.Errors), jsonOptions));
            if (stage != "embed")
            {
                var sidecar = System.Text.Json.Nodes.JsonNode.Parse(File.ReadAllText(Path.Combine(source, "embeddings.json")))!;
                sidecar["photoIds"] = JsonSerializer.SerializeToNode(photos.Select(p => p.Id).ToArray());
                WriteFloats(Path.Combine(dir, "embeddings.bin"), x);
                File.WriteAllText(Path.Combine(dir, "embeddings.json"), sidecar.ToJsonString());
            }
            using var index = EmbeddingIndex.Load(dir);
            var progress = new InlineProgress(Console.WriteLine);
            if (stage == "embed")
            {
                var result = await index.EnsureEmbeddedAsync(progress);
                report.Add(result);
                Console.WriteLine(JsonSerializer.Serialize(result));
                if (result.ReusedPhotos != 0) Console.WriteLine("REUSED: choose a new --out for a fresh throughput measurement.");
                if (real.Length == 0 || dims != index.Dimensions) throw new InvalidOperationException("Embedding parity needs the real reference matrix and matching dimensions.");
                var parity = photos.Select((p, i) => Cosine(index.Vector(p.Id), real.AsSpan(sourceRows[i] * dims, dims))).ToArray();
                Console.WriteLine($"Embedding parity minimum={parity.Min():F9}, mean={parity.Average():F9}");
                File.WriteAllText(Path.Combine(output, "embedding-parity.json"), JsonSerializer.Serialize(parity));
            }
            else if (stage == "search")
            {
                await index.SearchAsync("a blue and white porcelain plate");
                var times = new List<double>();
                foreach (var queryText in Enumerable.Repeat(new[] { "armor", "a horse", "a portrait", "a blue and white porcelain plate" }, 5).SelectMany(q => q))
                {
                    var sw = Stopwatch.StartNew();
                    var hits = await index.SearchAsync(queryText);
                    sw.Stop(); times.Add(sw.Elapsed.TotalMilliseconds);
                    Record("search", sw, new { queryText, count = hits.Count });
                }
                Console.WriteLine($"Search median={times.Order().ElementAt(times.Count / 2):F2}ms p95={times.Order().ElementAt((int)(times.Count * .95) - 1):F2}ms");
            }
            else
            {
                var sw = Stopwatch.StartNew();
                var groups = await VisualGroups.LoadOrBuildAsync(index, photos.Select(p => p.Id).ToArray(), dir, progress, CancellationToken.None);
                sw.Stop(); Record("visual-groups-total", sw, new { count = groups.Groups.Count, groups.UngroupedCount });
            }
        }
    }
    var measurements = JsonSerializer.Serialize(report, new JsonSerializerOptions { WriteIndented = true });
    File.WriteAllText(Path.Combine(output, "measurements.json"), measurements);
    File.WriteAllText(Path.Combine(output, $"{stage}-measurements.json"), measurements);
}

static float[] ReadFloats(string path)
{
    var bytes = File.ReadAllBytes(path);
    if (bytes.Length % 4 != 0) throw new InvalidDataException(path);
    var values = new float[bytes.Length / 4];
    Buffer.BlockCopy(bytes, 0, values, 0, bytes.Length);
    return values;
}
static void WriteFloats(string path, float[] values) => File.WriteAllBytes(path, System.Runtime.InteropServices.MemoryMarshal.AsBytes(values.AsSpan()).ToArray());
static void WriteInts(string path, int[] values) => File.WriteAllBytes(path, System.Runtime.InteropServices.MemoryMarshal.AsBytes(values.AsSpan()).ToArray());
static float[] CenterRows(float[] x, int n, int d)
{
    var mean = new double[d];
    for (var i = 0; i < n; i++) for (var j = 0; j < d; j++) mean[j] += x[i * d + j] / (double)n;
    var output = new float[x.Length];
    Parallel.For(0, n, i =>
    {
        double norm = 0;
        for (var j = 0; j < d; j++) { var v = (float)(x[i * d + j] - mean[j]); output[i * d + j] = v; norm += v * v; }
        var scale = (float)(1 / Math.Max(Math.Sqrt(norm), 1e-9));
        for (var j = 0; j < d; j++) output[i * d + j] *= scale;
    });
    return output;
}
static float[] ExpandReal(float[] real, int n, int d)
{
    var sourceCount = real.Length / d;
    var output = new float[checked(n * d)];
    Parallel.For(0, n, i =>
    {
        var rng = new Random(i * 7919 + 42);
        var a = i % sourceCount; var b = rng.Next(sourceCount);
        double norm = 0;
        for (var j = 0; j < d; j++)
        {
            var v = real[a * d + j] * .94f + real[b * d + j] * .06f + (float)(rng.NextDouble() - .5) * .002f;
            output[i * d + j] = v; norm += v * v;
        }
        var scale = (float)(1 / Math.Sqrt(norm));
        for (var j = 0; j < d; j++) output[i * d + j] *= scale;
    });
    return output;
}
static double Cosine(ReadOnlySpan<float> a, ReadOnlySpan<float> b)
{
    double dot = 0, na = 0, nb = 0;
    for (var i = 0; i < a.Length; i++) { dot += (double)a[i] * b[i]; na += (double)a[i] * a[i]; nb += (double)b[i] * b[i]; }
    return dot / Math.Sqrt(na * nb);
}
sealed class InlineProgress(Action<string> report) : IProgress<string>
{
    public void Report(string value) => report(value);
}
