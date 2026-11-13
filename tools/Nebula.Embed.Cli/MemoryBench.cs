using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Text.Json;
using Nebula.Embed;
using Nebula.Core;

internal static class MemoryBench
{
    private static readonly JsonSerializerOptions Json = new() { WriteIndented = true, PropertyNameCaseInsensitive = true };
    internal static readonly string[] Queries =
    [
        "a blue and white porcelain plate", "armor", "a portrait of a woman", "Egyptian hieroglyphs", "a horse",
        "red flowers in a vase", "a landscape with mountains", "gold jewelry", "a wooden chair", "a medieval manuscript",
        "a Japanese woodblock print", "a bronze sculpture", "a silver coin", "a glass bottle", "an embroidered textile",
        "a musical instrument", "a marble statue", "a ceramic bowl", "a painting of the sea", "a photograph of a building",
        "a map of a city", "a clock", "a pair of shoes", "a decorative mirror", "a bird on a branch",
        "an ancient drinking vessel", "a drawing in black ink", "a geometric pattern", "a carved stone relief", "a table",
        "a mask", "a basket", "a sailing ship", "a child playing", "a landscape in winter",
        "a red dress", "a butterfly", "a religious painting", "a still life with fruit", "a garden",
        "uma paisagem com árvores", "um cachorro na praia", "un retrato de una mujer", "une peinture de fleurs",
        "eine Landschaft mit Bergen", "un vaso di ceramica", "青い陶磁器の皿", "古代的青铜雕像",
        "لوحة لمنظر طبيعي", "abstract shapes in black and white"
    ];

    private sealed record MemorySample(string Phase, long PrivateBytes, long WorkingSetBytes, long ManagedBytes,
        double GpuDedicatedBytes, double GpuSharedBytes, int CounterSamples);
    private sealed record QueryResult(string Query, double Milliseconds, string[] Ids, float[] Scores);

    public static async Task<int> RunAsync(string[] args)
    {
        var options = new Dictionary<string, string>(StringComparer.Ordinal);
        for (var i = 0; i < args.Length; i += 2)
        {
            if (i + 1 >= args.Length || !new[] { "--library", "--models", "--out", "--reference", "--text-mode", "--groups", "--hold-seconds", "--reload", "--reload-count" }.Contains(args[i]))
                throw new ArgumentException($"Invalid memory benchmark option {args[i]}.");
            options.Add(args[i], args[i + 1]);
        }
        var local = Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData);
        var library = Path.GetFullPath(options.GetValueOrDefault("--library")
            ?? throw new ArgumentException("--memory-bench needs --library <library folder> (a library with embeddings)."));
        var output = Path.GetFullPath(options.GetValueOrDefault("--out") ?? Path.Combine(local, "Nebula", "Bench", "memory-50k", DateTime.UtcNow.ToString("yyyyMMdd-HHmmss")));
        if (output.Equals(library, StringComparison.OrdinalIgnoreCase) || output.StartsWith(library + Path.DirectorySeparatorChar, StringComparison.OrdinalIgnoreCase))
            throw new ArgumentException("Memory benchmark output must be outside the application library.");
        Directory.CreateDirectory(output);
        if (options.TryGetValue("--text-mode", out var mode)) Environment.SetEnvironmentVariable("NEB_TEXT_VARIANT", mode);
        var samples = new List<MemorySample>();
        void Sample(string phase)
        {
            var sample = SampleMemory(phase);
            samples.Add(sample);
            Console.WriteLine(JsonSerializer.Serialize(sample));
            File.WriteAllText(Path.Combine(output, "memory.json"), JsonSerializer.Serialize(samples, Json));
        }
        Sample("process-start");
        var clock = Stopwatch.StartNew();
        using var index = EmbeddingIndex.Load(library, options.GetValueOrDefault("--models"));
        var loadMs = clock.Elapsed.TotalMilliseconds;
        Sample("index-loaded");
        clock.Restart();
        await index.SearchAsync(Queries[0]);
        var coldMs = clock.Elapsed.TotalMilliseconds;
        Sample("first-query");
        var texts = Queries.SelectMany(q => new[] { q, $"a photo of {q}.", $"a detailed picture of {q}.", $"a museum photograph showing {q}." }).ToArray();
        var vectors = await index.EncodeTextAsync(texts);
        File.WriteAllText(Path.Combine(output, "texts.json"), JsonSerializer.Serialize(texts, Json));
        using (var file = File.Create(Path.Combine(output, "text-vectors.bin")))
            foreach (var vector in vectors) file.Write(MemoryMarshal.AsBytes(vector.AsSpan()));
        var results = new List<QueryResult>();
        foreach (var query in Queries)
        {
            clock.Restart();
            var hits = await index.SearchAsync(query);
            results.Add(new(query, clock.Elapsed.TotalMilliseconds, hits.Select(h => h.Id).ToArray(), hits.Select(h => h.Score).ToArray()));
        }
        File.WriteAllText(Path.Combine(output, "rankings.json"), JsonSerializer.Serialize(results, Json));
        var warmMs = new List<double>();
        foreach (var query in Queries)
        {
            clock.Restart(); await index.SearchAsync(query);
            warmMs.Add(clock.Elapsed.TotalMilliseconds);
        }
        Sample("warm-uncollected");
        GC.Collect(); GC.WaitForPendingFinalizers(); GC.Collect();
        Sample("warm-collected");
        VisualGroupSet? retainedGroups = null;
        if (options.GetValueOrDefault("--groups") == "true")
        {
            // Copy only cache metadata into scratch; never rebuild/write the application's group cache.
            var groupDirectory = Path.Combine(output, "group-cache");
            Directory.CreateDirectory(groupDirectory);
            File.Copy(Path.Combine(library, "groups.json"), Path.Combine(groupDirectory, "groups.json"), true);
            retainedGroups = await VisualGroups.LoadOrBuildAsync(index, index.PhotoIds, groupDirectory, new Progress<string>(Console.WriteLine), CancellationToken.None);
            Sample("groups-loaded-uncollected");
            GC.Collect(); GC.WaitForPendingFinalizers(); GC.Collect();
            Sample("groups-loaded-collected");
        }
        double? reloadMs = null;
        var reloadSamples = new List<double>();
        if (options.GetValueOrDefault("--reload") == "true")
        {
            var count = int.Parse(options.GetValueOrDefault("--reload-count", "1"));
            if (count is < 1 or > 5) throw new ArgumentException("Reload count must be 1 to 5.");
            for (var i = 0; i < count; i++)
            {
                index.ReleaseTextSessionForMeasurement();
                GC.Collect(); GC.WaitForPendingFinalizers(); GC.Collect();
                Sample($"text-released-{i}");
                clock.Restart();
                await index.SearchAsync("an ornate copper water pitcher");
                reloadSamples.Add(clock.Elapsed.TotalMilliseconds);
                Sample($"text-reloaded-{i}");
            }
            reloadMs = reloadSamples.Max();
        }
        if (int.TryParse(options.GetValueOrDefault("--hold-seconds"), out var hold))
        {
            Console.WriteLine($"Holding idle for {hold} seconds, PID {Environment.ProcessId}");
            await Task.Delay(TimeSpan.FromSeconds(hold));
            Sample("after-idle");
            clock.Restart();
            await index.SearchAsync("a previously unseen ornate copper water pitcher");
            reloadMs = clock.Elapsed.TotalMilliseconds;
            Sample("after-idle-query");
        }
        var parity = options.TryGetValue("--reference", out var reference) ? Compare(reference, texts, vectors, results) : null;
        var report = new
        {
            pid = Environment.ProcessId,
            library,
            index.Count,
            index.Dimensions,
            index.ModelHash,
            index.Device,
            textVariantId = index.TextVariantId,
            textMode = options.GetValueOrDefault("--text-mode", "auto"),
            loadMs,
            coldMs,
            reloadMs,
            reloadSamples,
            warmP50Ms = Percentile(warmMs, .5),
            warmP95Ms = Percentile(warmMs, .95),
            newQueryP50Ms = Percentile(results.Select(r => r.Milliseconds), .5),
            newQueryP95Ms = Percentile(results.Select(r => r.Milliseconds), .95),
            parity
        };
        File.WriteAllText(Path.Combine(output, "report.json"), JsonSerializer.Serialize(report, Json));
        Console.WriteLine(JsonSerializer.Serialize(report, Json));
        GC.KeepAlive(retainedGroups);
        return 0;
    }

    private static object Compare(string reference, string[] texts, IReadOnlyList<float[]> vectors, List<QueryResult> actual)
    {
        var expectedTexts = JsonSerializer.Deserialize<string[]>(File.ReadAllText(Path.Combine(reference, "texts.json")))!;
        if (!texts.SequenceEqual(expectedTexts)) throw new InvalidDataException("Reference query corpus differs.");
        var bytes = File.ReadAllBytes(Path.Combine(reference, "text-vectors.bin"));
        var referenceVectors = MemoryMarshal.Cast<byte, float>(bytes);
        if (referenceVectors.Length != vectors.Count * vectors[0].Length) throw new InvalidDataException("Reference vector shape differs.");
        var cosines = new List<double>();
        for (var row = 0; row < vectors.Count; row++)
        {
            var a = referenceVectors.Slice(row * vectors[row].Length, vectors[row].Length);
            var b = vectors[row];
            double dot = 0, aa = 0, bb = 0;
            for (var j = 0; j < a.Length; j++) { dot += (double)a[j] * b[j]; aa += (double)a[j] * a[j]; bb += (double)b[j] * b[j]; }
            cosines.Add(dot / Math.Sqrt(aa * bb));
        }
        var expected = JsonSerializer.Deserialize<QueryResult[]>(File.ReadAllText(Path.Combine(reference, "rankings.json")), Json)!;
        if (!expected.Select(r => r.Query).SequenceEqual(actual.Select(r => r.Query))) throw new InvalidDataException("Reference rankings differ in query order.");
        var comparisons = actual.Select((r, i) =>
        {
            var old = expected[i];
            var top = old.Ids.Take(20).ToArray();
            var overlap = top.Length == 0 ? (r.Ids.Length == 0 ? 1.0 : 0) : top.Intersect(r.Ids.Take(20)).Count() / (double)top.Length;
            return new { r.Query, top20Overlap = overlap, identicalAllIds = old.Ids.SequenceEqual(r.Ids), identicalAllScores = old.Scores.SequenceEqual(r.Scores) };
        }).ToArray();
        return new
        {
            queries = cosines.Count,
            minimumCosine = cosines.Min(),
            meanCosine = cosines.Average(),
            rankingQueries = comparisons.Length,
            minimumTop20Overlap = comparisons.Min(x => x.top20Overlap),
            meanTop20Overlap = comparisons.Average(x => x.top20Overlap),
            identicalRankings = comparisons.Count(x => x.identicalAllIds),
            comparisons
        };
    }

    private static double Percentile(IEnumerable<double> values, double percentile)
    {
        var sorted = values.Order().ToArray();
        return sorted[(int)((sorted.Length - 1) * percentile)];
    }

    internal static void VerifyMappings(string directory)
    {
        Directory.CreateDirectory(directory);
        var path = Path.Combine(directory, "mapping-fixture.bin");
        var part = path + ".part";
        float[] oldValues = [1, 2, 3, 4, 5, 6], newValues = [7, 8, 9, 10, 11, 12];
        File.WriteAllBytes(path, MemoryMarshal.AsBytes(oldValues.AsSpan()).ToArray());
        var owner = new ReadOnlyTensorMap<float>(path, 2, 3);
        using (var reader = owner.Acquire())
        {
            owner.Dispose();
            if (!reader.Row(0).SequenceEqual(oldValues.AsSpan(0, 3))) throw new InvalidOperationException("Owner disposal invalidated a reader.");
            File.WriteAllBytes(part, MemoryMarshal.AsBytes(newValues.AsSpan()).ToArray());
            File.Move(part, path, true);
            using var replacement = new ReadOnlyTensorMap<float>(path, 2, 3);
            using var fresh = replacement.Acquire();
            if (!fresh.Row(1).SequenceEqual(newValues.AsSpan(3, 3)) || !reader.Row(1).SequenceEqual(oldValues.AsSpan(3, 3)))
                throw new InvalidOperationException("Atomic replacement changed an in-flight mapping.");
            var rejected = false;
            try { owner.Acquire(); } catch (ObjectDisposedException) { rejected = true; }
            if (!rejected) throw new InvalidOperationException("Disposed mapping accepted a new reader.");
        }
        var badShape = false;
        try { using var invalid = new ReadOnlyTensorMap<float>(path, 3, 3); }
        catch (InvalidDataException) { badShape = true; }
        if (!badShape) throw new InvalidOperationException("Invalid mapped shape was accepted.");
        File.Delete(path);
        Console.WriteLine("PASS read leases, disposal, atomic replacement and mapped shape validation");

        var libraryPath = Path.Combine(directory, "library");
        var modelPath = Path.Combine(directory, "models");
        Directory.CreateDirectory(libraryPath); Directory.CreateDirectory(modelPath);
        var photos = Enumerable.Range(0, 3).Select(i => new Photo
        {
            Id = i.ToString(),
            OriginalPath = "unused",
            ThumbnailPath = "unused",
            Title = "fixture"
        }).ToList();
        File.WriteAllText(Path.Combine(modelPath, "manifest.json"), JsonSerializer.Serialize(new { dimensions = 3, modelHash = "fixture" }));
        File.WriteAllText(Path.Combine(libraryPath, "library.json"), JsonSerializer.Serialize(new Library("fixture", photos, 0, [])));
        File.WriteAllText(Path.Combine(libraryPath, "embeddings.json"), JsonSerializer.Serialize(new
        {
            modelHash = "fixture",
            dimensions = 3,
            photoIds = photos.Select(p => p.Id),
            rows = photos.ToDictionary(p => p.Id, p => new { sourceBytes = 0, sourceWriteUtcTicks = 0, metadataFingerprint = "", thumbnailPath = p.ThumbnailPath })
        }));
        float[] features = [1, 0, 0, .98f, .02f, 0, 0, 1, 0];
        File.WriteAllBytes(Path.Combine(libraryPath, "embeddings.bin"), MemoryMarshal.AsBytes(features.AsSpan()).ToArray());
        using var index = EmbeddingIndex.Load(libraryPath, modelPath);
        var vector = index.Vector("0"); vector[0] = 99;
        if (index.Vector("0")[0] != 1 || !index.Similar("0", 1).SequenceEqual(new[] { "1" }) || index.Similar("0", 0).Count != 0)
            throw new InvalidOperationException("Mapped Vector/Similar contract changed.");
        var resume = index.EnsureEmbeddedAsync().GetAwaiter().GetResult();
        if (resume.ReusedPhotos != 3 || resume.EmbeddedPhotos != 0 || resume.Device != "not initialized")
            throw new InvalidOperationException("Mapped cache resume created an unnecessary model session.");
        Console.WriteLine("PASS Vector copy isolation, exact Similar ordering and no-model cached resume");
    }

    private static MemorySample SampleMemory(string phase)
    {
        using var process = Process.GetCurrentProcess();
        process.Refresh();
        var script = "$ErrorActionPreference='Stop'; $p=@((Get-Counter -ListSet 'GPU Process Memory').PathsWithInstances | Where-Object {$_ -like '*(pid_" +
            Environment.ProcessId + "_*)\\Dedicated Usage' -or $_ -like '*(pid_" + Environment.ProcessId +
            "_*)\\Shared Usage'}); $s=@(); if($p.Count){ $s=(Get-Counter -Counter $p).CounterSamples }; " +
            "$d=($s | Where-Object {$_.Path -like '*dedicated usage'} | Measure-Object CookedValue -Sum).Sum; " +
            "$h=($s | Where-Object {$_.Path -like '*shared usage'} | Measure-Object CookedValue -Sum).Sum; " +
            "@{dedicated=[double]$d;shared=[double]$h;samples=@($s).Count}|ConvertTo-Json -Compress";
        var start = new ProcessStartInfo("powershell.exe") { RedirectStandardOutput = true, RedirectStandardError = true, UseShellExecute = false, CreateNoWindow = true };
        foreach (var argument in new[] { "-NoProfile", "-NonInteractive", "-Command", script }) start.ArgumentList.Add(argument);
        using var counter = Process.Start(start) ?? throw new InvalidOperationException("Could not launch GPU counter probe.");
        var stdoutTask = counter.StandardOutput.ReadToEndAsync();
        var stderrTask = counter.StandardError.ReadToEndAsync();
        if (!counter.WaitForExit(30000)) { counter.Kill(true); throw new TimeoutException("GPU counter probe timed out."); }
        var stdout = stdoutTask.GetAwaiter().GetResult();
        var stderr = stderrTask.GetAwaiter().GetResult();
        if (counter.ExitCode != 0) throw new InvalidOperationException($"GPU counter probe failed: {stderr}");
        using var data = JsonDocument.Parse(stdout);
        return new(phase, process.PrivateMemorySize64, process.WorkingSet64, GC.GetTotalMemory(false),
            data.RootElement.GetProperty("dedicated").GetDouble(), data.RootElement.GetProperty("shared").GetDouble(),
            data.RootElement.GetProperty("samples").GetInt32());
    }
}
