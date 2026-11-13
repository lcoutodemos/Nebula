using Nebula.Core;
using System.Buffers;
using System.Buffers.Binary;
using System.Diagnostics;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using System.Text.RegularExpressions;

namespace Nebula.Embed;

public sealed record VisualGroup(
    int Id,
    string Label,
    string NamedBy,
    string Qualifier,
    IReadOnlyList<string> Hint,
    int Size,
    int[] Members,
    int Representative,
    float Coherence,
    IReadOnlyList<(int Year, int Count)> Years,
    IReadOnlyList<(string Term, float Score)> Tags,
    int SuperId,
    int[] Leaves)
{
    public string Name => string.IsNullOrEmpty(Label)
        ? Hint.Count == 0 ? "Mixed" : "Mixed · " + string.Join(", ", Hint)
        : NamedBy == "place" ? Label : TitleCase(Label);
    public string Title => Name + (string.IsNullOrEmpty(Qualifier) ? "" : $" · {Qualifier}");
    public string ShortName => IsNamed ? Name : "Mixed";
    public bool IsNamed => !string.IsNullOrEmpty(Label);

    private static string TitleCase(string value) =>
        string.Join(' ', value.Split(' ', StringSplitOptions.None)
            .Select(part => part.Length == 0 ? part : char.ToUpperInvariant(part[0]) + part[1..]));
}

public sealed class VisualGroupSet
{
    public required IReadOnlyList<VisualGroup> Groups { get; init; }
    public required IReadOnlyList<VisualGroup> Supers { get; init; }
    public required int[] PhotoGroup { get; init; }
    public required int[] PhotoSuper { get; init; }
    public required IReadOnlyList<(int A, int B, int N)> Links { get; init; }
    public required int[] Bridges { get; init; }
    public required IReadOnlyList<(int Index, float Similarity)>[] Knn { get; init; }
    public required TimeSpan Elapsed { get; init; }
    public required int UngroupedCount { get; init; }
}

public static partial class VisualGroups
{
    public const string Version = "visual-groups-v5-full-covariance-pca";
    private static readonly JsonSerializerOptions JsonOptions = new(JsonSerializerDefaults.Web)
    {
        WriteIndented = true,
        PropertyNameCaseInsensitive = true
    };

    public static async Task<VisualGroupSet> LoadOrBuildAsync(
        EmbeddingIndex index,
        IReadOnlyList<string> photoIdsInLibraryOrder,
        string libraryDir,
        IProgress<string>? progress,
        CancellationToken ct)
    {
        var sw = Stopwatch.StartNew();
        var groupsPath = Path.Combine(libraryDir, "groups.json");
        var photoHash = HashIds(photoIdsInLibraryOrder);
        if (File.Exists(groupsPath))
        {
            using var file = File.OpenRead(groupsPath);
            var cached = JsonSerializer.Deserialize<CacheFile>(file, JsonOptions);
            if (cached is not null && cached.Version == Version && cached.ModelHash == index.ModelHash && cached.PhotoIdsHash == photoHash)
            {
                progress?.Report("groups: reused groups.json");
                return FromCache(cached, sw.Elapsed);
            }
        }

        if (index.Count == 0) throw new InvalidOperationException("Embeddings are not loaded. Call EnsureEmbeddedAsync before building visual groups.");
        if (photoIdsInLibraryOrder.Count != index.Count) throw new ArgumentException("Photo id order does not match the embedding index.", nameof(photoIdsInLibraryOrder));
        for (var i = 0; i < photoIdsInLibraryOrder.Count; i++)
            if (photoIdsInLibraryOrder[i] != index.PhotoIds[i])
                throw new ArgumentException("Photo id order must match library.json / embeddings.json row order.", nameof(photoIdsInLibraryOrder));

        var n = index.Count;
        var d = index.Dimensions;
        var embeddings = index.CopyEmbeddings();
        var ec = CenteredRows(embeddings, n, d);
        progress?.Report("groups: PCA-to-100 + 10-D PaCMAP");
        var stageTime = Stopwatch.StartNew();
        var p10 = await Task.Run(() => Layout.PaCMAPProjection(ec, n, d, 10, seed: 0, nNeighbors: Math.Min(10, n - 1), mnRatio: 0.5, fpRatio: 2.0), ct).ConfigureAwait(false);
        progress?.Report($"groups: PCA/PaCMAP completed in {stageTime.Elapsed.TotalSeconds:F2}s");

        progress?.Report("groups: HDBSCAN leaf clusters");
        ct.ThrowIfCancellationRequested();
        stageTime.Restart();
        var labels = n >= 24 ? Hdbscan.LeafClusters(p10, n, 10, minClusterSize: 10, minSamples: 2) : Enumerable.Repeat(-1, n).ToArray();
        progress?.Report($"groups: exact HDBSCAN completed in {stageTime.Elapsed.TotalSeconds:F2}s");
        var originalGroups = labels.Where(x => x >= 0).Distinct().OrderBy(x => x).ToArray();
        var membersByOriginal = originalGroups.ToDictionary(g => g, _ => new List<int>());
        for (var i = 0; i < n; i++) if (labels[i] >= 0) membersByOriginal[labels[i]].Add(i);
        var centersByOriginal = membersByOriginal.ToDictionary(p => p.Key, p => Center(ec, d, p.Value));
        var thresholds = membersByOriginal.ToDictionary(p => p.Key, p =>
        {
            var center = centersByOriginal[p.Key];
            var sims = p.Value.Select(i => Dot(ec.AsSpan(i * d, d), center)).Order().ToArray();
            return Percentile(sims, 0.25);
        });

        var joined = 0;
        if (originalGroups.Length > 0)
        {
            for (var i = 0; i < n; i++)
            {
                if (labels[i] >= 0) continue;
                var best = originalGroups[0];
                var bestScore = Dot(ec.AsSpan(i * d, d), centersByOriginal[best]);
                foreach (var g in originalGroups.Skip(1))
                {
                    var score = Dot(ec.AsSpan(i * d, d), centersByOriginal[g]);
                    if (score > bestScore)
                    {
                        best = g;
                        bestScore = score;
                    }
                }
                if (bestScore >= thresholds[best])
                {
                    labels[i] = best;
                    joined++;
                }
            }
        }

        var groupOriginals = labels.Where(x => x >= 0)
            .GroupBy(x => x)
            .OrderByDescending(g => g.Count())
            .ThenBy(g => Array.IndexOf(originalGroups, g.Key))
            .Select(g => g.Key)
            .ToArray();
        var remap = groupOriginals.Select((g, i) => (g, i)).ToDictionary(x => x.g, x => x.i);
        var photoGroup = labels.Select(x => x >= 0 && remap.TryGetValue(x, out var g) ? g : -1).ToArray();

        progress?.Report("groups: full-embedding KNN and links");
        stageTime.Restart();
        var knn = BuildKnn(ec, n, d, 12, ct);
        progress?.Report($"groups: exact 12-NN completed in {stageTime.Elapsed.TotalSeconds:F2}s ({2.0 * n * n * d / stageTime.Elapsed.TotalSeconds / 1e9:F1} GFLOP/s)");
        var links = BuildLinks(photoGroup, knn);
        var bridges = BuildBridges(photoGroup, knn);

        progress?.Report("groups: bank and naming");
        var namer = await VocabularyNamer.CreateAsync(index, embeddings, ec, libraryDir, progress, ct);

        var groups = new List<VisualGroup>();
        var groupCenters = new List<float[]>();
        for (var gid = 0; gid < groupOriginals.Length; gid++)
        {
            ct.ThrowIfCancellationRequested();
            var members = photoGroup.Select((g, row) => (g, row)).Where(x => x.g == gid).Select(x => x.row).ToArray();
            var center = Center(ec, d, members);
            groupCenters.Add(center);
            var naming = namer.Name(members);
            var representative = Representative(ec, d, members, center);
            var coherence = members.Length == 0 ? 0 : members.Average(i => Dot(ec.AsSpan(i * d, d), center));
            groups.Add(new VisualGroup(gid, naming.Label, naming.NamedBy, "", naming.Hint, members.Length, members, representative,
                Round3(coherence), [], naming.Tags, -1, []));
        }

        var superBuckets = BuildCategoryBuckets(groups, groupCenters, d, namer);
        var photoSuper = Enumerable.Repeat(-1, n).ToArray();
        var mutableGroups = groups.ToArray();
        var supers = new List<VisualGroup>();
        for (var sid = 0; sid < superBuckets.Length; sid++)
        {
            var bucket = superBuckets[sid].ToArray();
            var members = bucket.SelectMany(g => g.Members).Distinct().OrderBy(i => i).ToArray();
            if (members.Length == 0) continue;
            var center = Center(ec, d, members);
            var naming = namer.Name(members);
            if (string.IsNullOrEmpty(naming.Label) && bucket.Any(g => g.IsNamed))
                naming = naming with { Label = bucket.Where(g => g.IsNamed).GroupBy(g => g.Label).OrderByDescending(g => g.Sum(x => x.Size)).First().Key, NamedBy = "word" };
            var representative = Representative(ec, d, members, center);
            var coherence = members.Average(i => Dot(ec.AsSpan(i * d, d), center));
            var leaves = bucket.Select(g => g.Id).Order().ToArray();
            foreach (var leaf in leaves)
            {
                var g = mutableGroups[leaf];
                mutableGroups[leaf] = g with { SuperId = sid };
            }
            foreach (var m in members) photoSuper[m] = sid;
            supers.Add(new VisualGroup(sid, naming.Label, naming.NamedBy, "", naming.Hint, members.Length, members, representative,
                Round3(coherence), [], naming.Tags, -1, leaves));
        }

        groups = mutableGroups.ToList();
        // Completed Tasks/caches must not retain the full feature matrices via async locals.
        embeddings = []; ec = []; p10 = [];
        var cache = ToCache(Version, index.ModelHash, photoHash, groups, supers, photoGroup, photoSuper, links, bridges, knn,
            joined, photoGroup.Count(g => g < 0), sw.Elapsed);
        WriteJsonAtomically(groupsPath, cache);
        sw.Stop();
        progress?.Report($"groups: {groups.Count} groups, {supers.Count} supers, {photoGroup.Count(g => g < 0)} ungrouped, {joined} joined in {sw.Elapsed}");
        return FromCache(cache, sw.Elapsed);
    }

    private sealed record NamingResult(string Label, string NamedBy, string[] Hint, IReadOnlyList<(string Term, float Score)> Tags);

    private static VisualGroup[][] BuildCategoryBuckets(IReadOnlyList<VisualGroup> groups, IReadOnlyList<float[]> centers, int dimensions, VocabularyNamer namer)
    {
        if (groups.Count <= 2) return groups.Select(g => new[] { g }).ToArray();
        var merges = WardMerges(centers, dimensions);
        var heights = merges.Select(m => m.Distance).Order().ToArray();
        var median = heights.Length % 2 == 1 ? heights[heights.Length / 2] : (heights[heights.Length / 2 - 1] + heights[heights.Length / 2]) / 2;
        var candidates = new[] { 3.5, 2.9, 2.4, 2.0, 1.8, 1.6, 1.4, 1.2, 1.0, 0.8 }.Select(x => Math.Max(1e-6, x * median)).Distinct().OrderDescending().ToArray();
        int[] chosen = Enumerable.Range(0, groups.Count).ToArray();
        foreach (var threshold in candidates)
        {
            var labels = FlatClusters(merges, groups.Count, threshold);
            var by = labels.Select((c, i) => (c, i)).GroupBy(x => x.c).Select(g => g.Select(x => x.i).ToArray()).ToArray();
            var groupedPhotos = Math.Max(groups.Sum(g => g.Size), 1);
            var namedPhotos = 0;
            foreach (var leaves in by)
            {
                var members = leaves.SelectMany(l => groups[l].Members).Distinct().Order().ToArray();
                var name = namer.Name(members);
                if (!string.IsNullOrEmpty(name.Label) || name.Hint.Length > 0)
                    namedPhotos += members.Length;
            }
            chosen = labels;
            if (namedPhotos / (double)groupedPhotos >= 0.95) break;
        }
        return chosen.Select((c, i) => (c, i))
            .GroupBy(x => x.c)
            .Select(g => g.Select(x => groups[x.i]).ToArray())
            .OrderByDescending(g => g.Sum(x => x.Size))
            .ThenBy(g => g.Min(x => x.Id))
            .ToArray();
    }

    private static List<(int A, int B, double Distance)> WardMerges(IReadOnlyList<float[]> points, int dimensions)
    {
        var n = points.Count;
        var dist = new double[n * n];
        for (var i = 0; i < n; i++)
            for (var j = i + 1; j < n; j++)
                dist[i * n + j] = dist[j * n + i] = Euclidean(points[i], points[j], dimensions);
        var size = Enumerable.Repeat(1, n).ToArray();
        var active = Enumerable.Repeat(true, n).ToArray();
        var rep = Enumerable.Range(0, n).ToArray();
        var merges = new List<(int A, int B, double Distance)>();
        var chain = new List<int>();
        var remaining = n;
        while (remaining > 1)
        {
            if (chain.Count == 0) chain.Add(Array.FindIndex(active, x => x));
            var a = chain[^1];
            var prev = chain.Count >= 2 ? chain[^2] : -1;
            var b = -1;
            var best = double.PositiveInfinity;
            for (var j = 0; j < n; j++)
            {
                if (!active[j] || j == a) continue;
                var v = dist[a * n + j];
                if (v < best || (v == best && j == prev)) { best = v; b = j; }
            }
            if (b == prev)
            {
                chain.RemoveRange(chain.Count - 2, 2);
                var lo = Math.Min(a, b);
                var hi = Math.Max(a, b);
                var sa = size[a];
                var sb = size[b];
                merges.Add((rep[a], rep[b], best));
                for (var w = 0; w < n; w++)
                {
                    if (!active[w] || w == a || w == b) continue;
                    var sw = size[w];
                    var v = ((sa + sw) * dist[a * n + w] * dist[a * n + w] +
                             (sb + sw) * dist[b * n + w] * dist[b * n + w] -
                             sw * best * best) / (sa + sb + sw);
                    dist[lo * n + w] = dist[w * n + lo] = Math.Sqrt(Math.Max(v, 0));
                }
                active[hi] = false;
                size[lo] = size[a] + size[b];
                rep[lo] = Math.Min(rep[a], rep[b]);
                remaining--;
            }
            else
            {
                chain.Add(b);
            }
        }
        return merges.OrderBy(m => m.Distance).ToList();
    }

    private static int[] FlatClusters(IReadOnlyList<(int A, int B, double Distance)> merges, int n, double threshold)
    {
        var parent = Enumerable.Range(0, n).ToArray();
        int Find(int a)
        {
            var r = a;
            while (parent[r] != r) r = parent[r];
            while (parent[a] != r) { var p = parent[a]; parent[a] = r; a = p; }
            return r;
        }
        foreach (var (a, b, h) in merges)
        {
            if (h > threshold) continue;
            var ra = Find(a);
            var rb = Find(b);
            if (ra != rb) parent[Math.Max(ra, rb)] = Math.Min(ra, rb);
        }
        var ids = new Dictionary<int, int>();
        var output = new int[n];
        for (var i = 0; i < n; i++)
        {
            var r = Find(i);
            if (!ids.TryGetValue(r, out var id)) ids[r] = id = ids.Count;
            output[i] = id;
        }
        return output;
    }

    private static double Euclidean(float[] a, float[] b, int dimensions)
    {
        var sum = 0.0;
        for (var i = 0; i < dimensions; i++)
        {
            var d = a[i] - b[i];
            sum += d * d;
        }
        return Math.Sqrt(sum);
    }

    private sealed class VocabularyNamer
    {
        private const double NameCover = 0.60;
        private const double PartCover = 0.15;
        private const double Lift = 3.0;
        private const int TopK = 3;
        private const int TopKWide = 10;
        private const double WideTop = 0.05;
        private const double WideFloor = 0.01;
        private const double WideSure = 0.5;
        private const int WideMost = 32;
        private static readonly string[] Templates = ["{}", "a photo of {}.", "a picture of {}.", "a snapshot of {}.", "a photo showing {}."];
        private static readonly (string Family, string[] Words)[] FamilyWords =
        [
            ("screenshots", ["chat conversation screenshot", "social media post screenshot", "web page screenshot", "app interface screenshot", "screenshot", "code on a screen", "screen"]),
            ("people", ["person", "people", "man", "woman", "friend", "family", "crowd", "group of people", "portrait", "selfie", "face", "child", "baby", "kid", "group photo of friends"]),
            ("food", ["food", "meal", "breakfast", "lunch", "dinner", "pizza", "burger", "sandwich", "salad", "soup", "pasta", "rice", "bread", "cake", "dessert", "ice cream", "fruit", "vegetable", "cooking", "barbecue"]),
            ("drinks", ["coffee", "tea", "wine", "beer", "cocktail"]),
            ("water", ["beach", "ocean", "sea", "lake", "river", "swimming pool", "swimming in the sea", "sunset over the sea", "boat", "sailing", "surfing"]),
            ("documents", ["document", "receipt", "invoice", "ticket", "photo of a document", "handwriting", "notebook", "text", "writing", "book", "newspaper", "whiteboard"]),
            ("building work", ["construction", "repair", "tools", "garage", "factory", "machine"])
        ];

        private readonly string[] _core;
        private readonly string[] _wide;
        private readonly int _n;
        private readonly int _d;
        private readonly float[] _pcal;
        private readonly float[] _cz;
        private readonly byte[] _top;
        private readonly double[] _base;
        private readonly int[][] _cand;
        private readonly double[] _cbase;
        private readonly int[][] _wtop;
        private readonly double[] _wbase;
        private readonly (string Family, int[] Indices)[] _wideFamilies;
        private readonly double[] _wideFamilyBase;

        private VocabularyNamer(string[] core, string[] wide, int n, int d, float[] pcal, float[] cz, byte[] top, double[] baseRates,
            int[][] cand, double[] cbase, int[][] wtop, double[] wbase, (string Family, int[] Indices)[] wideFamilies, double[] wideFamilyBase)
        {
            _core = core; _wide = wide; _n = n; _d = d; _pcal = pcal; _cz = cz; _top = top; _base = baseRates;
            _cand = cand; _cbase = cbase; _wtop = wtop; _wbase = wbase; _wideFamilies = wideFamilies; _wideFamilyBase = wideFamilyBase;
        }

        public static async Task<VocabularyNamer> CreateAsync(EmbeddingIndex index, float[] embeddings, float[] ec, string libraryDir, IProgress<string>? progress, CancellationToken ct)
        {
            var modelDir = index.ModelDirectory;
            string[] ReadLines(string name)
            {
                var path = Path.Combine(modelDir, name);
                if (!File.Exists(path)) throw new FileNotFoundException($"Vocabulary pack file {name} is missing. Run tools\\prepare_models.py --vocab-only.", path);
                return File.ReadAllLines(path).Select(x => x.Trim()).Where(x => x.Length > 0).ToArray();
            }

            var bank = ReadLines("bank_vocab.txt");
            var core = ReadLines("core_vocab.txt");
            var wide = ReadLines("wide_vocab.txt");
            var never = ReadLines("never_named.txt").ToHashSet(StringComparer.OrdinalIgnoreCase);
            var wideVectors = ReadNpyFloatMatrix(Path.Combine(modelDir, "wide_siglip2.npy"), out var wideRows, out var wideDims);
            if (wideRows != wide.Length || wideDims != index.Dimensions)
                throw new InvalidDataException($"wide_siglip2.npy shape {wideRows}x{wideDims} does not match wide vocab {wide.Length}x{index.Dimensions}.");
            var n = index.Count;
            var d = index.Dimensions;

            progress?.Report($"groups: bank stage ({bank.Length} bank words, {core.Length} core words, {wide.Length} wide words)");
            var qBank = await EncodeTerms(index, bank, ct);
            var muImg = MeanRows(embeddings, n, d);
            var muTxt = MeanRows(qBank, bank.Length, d);
            WriteNpy(Path.Combine(libraryDir, "center_img.npy"), muImg, 1, d);
            WriteNpy(Path.Combine(libraryDir, "center_txt.npy"), muTxt, 1, d);
            var qBankCentered = CenterText(qBank, bank.Length, d, muTxt);
            var bankScores = ComputeScores(ec, n, qBankCentered, bank.Length, d);
            var bias = new float[n];
            for (var i = 0; i < n; i++)
            {
                var best = new List<float>(8);
                for (var w = 0; w < bank.Length; w++)
                {
                    var s = bankScores[i * bank.Length + w];
                    if (best.Count < 8 || s > best[^1])
                    {
                        var at = best.Count;
                        while (at > 0 && best[at - 1] < s) at--;
                        best.Insert(at, s);
                        if (best.Count > 8) best.RemoveAt(best.Count - 1);
                    }
                }
                bias[i] = 0.5f * best.Average();
            }
            WriteNpy(Path.Combine(libraryDir, "nnn_bias.npy"), bias, 1, n);
            File.WriteAllText(Path.Combine(libraryDir, "calib.json"), JsonSerializer.Serialize(new
            {
                tau = EmbeddingIndex.Tau,
                beta = EmbeddingIndex.Beta,
                k = 8,
                alpha = 0.5,
                n_bank = bank.Length,
                repo = EmbeddingIndex.ModelId,
                emb_file = "embeddings.bin"
            }, JsonOptions));

            progress?.Report("groups: naming core scores");
            var qCore = await EncodeTerms(index, core, ct);
            var qCoreCentered = CenterText(qCore, core.Length, d, muTxt);
            var coreRaw = ComputeScores(embeddings, n, qCore, core.Length, d);
            var pcal = new float[coreRaw.Length];
            for (var i = 0; i < coreRaw.Length; i++)
                pcal[i] = (float)(1.0 / (1.0 + Math.Exp(-(EmbeddingIndex.Tau * coreRaw[i] + EmbeddingIndex.Beta))));
            var cz = ZScoreColumns(ComputeScores(ec, n, qCoreCentered, core.Length, d), n, core.Length);
            var top = new byte[n * core.Length];
            Parallel.For(0, n, i =>
            {
                foreach (var w in TopIndices(cz.AsSpan(i * core.Length, core.Length), Math.Min(TopK, core.Length)))
                    top[i * core.Length + w] = 1;
            });
            var baseRates = new double[core.Length];
            for (var w = 0; w < core.Length; w++)
                baseRates[w] = Enumerable.Range(0, n).Count(i => top[i * core.Length + w] != 0) / (double)Math.Max(n, 1);

            progress?.Report("groups: naming wide scores");
            NormalizeRows(wideVectors, wide.Length, d);
            var wideCentered = CenterText(wideVectors, wide.Length, d, muTxt);
            var wideRaw = ComputeScores(embeddings, n, wideVectors, wide.Length, d);
            var wideZ = ZScoreColumns(ComputeScores(ec, n, wideCentered, wide.Length, d), n, wide.Length);
            var wtop = new int[n][];
            Parallel.For(0, n, i => wtop[i] = TopIndices(wideZ.AsSpan(i * wide.Length, wide.Length), Math.Min(TopKWide, wide.Length)).ToArray());
            var wbase = new double[wide.Length];
            foreach (var row in wtop)
                foreach (var w in row)
                    wbase[w] += 1;
            for (var w = 0; w < wide.Length; w++) wbase[w] /= Math.Max(n, 1);

            var low = Cosine(WideFloor);
            var high = Cosine(WideSure);
            var bars = new float[wide.Length];
            Parallel.For(0, wide.Length, w =>
            {
                var col = ArrayPool<float>.Shared.Rent(n);
                try
                {
                    for (var i = 0; i < n; i++) col[i] = wideRaw[i * wide.Length + w];
                    var cut = SelectKth(col, n, Math.Min(n - 1, (int)((1 - WideTop) * n)));
                    bars[w] = Math.Min(Math.Max(cut, low), high);
                }
                finally { ArrayPool<float>.Shared.Return(col); }
            });
            var cand = new int[n][];
            Parallel.For(0, n, i =>
            {
                var keep = new List<int>();
                for (var w = 0; w < wide.Length; w++)
                    if (wideRaw[i * wide.Length + w] >= bars[w] && !never.Contains(wide[w]))
                        keep.Add(w);
                if (keep.Count > WideMost)
                    keep = keep.OrderByDescending(w => wideRaw[i * wide.Length + w] - bars[w]).Take(WideMost).Order().ToList();
                cand[i] = keep.ToArray();
            });
            var cbase = new double[wide.Length];
            foreach (var row in cand)
                foreach (var w in row)
                    cbase[w] += 1;
            for (var w = 0; w < wide.Length; w++) cbase[w] /= Math.Max(n, 1);

            var wideAt = wide.Select((word, i) => (word, i)).ToDictionary(x => x.word, x => x.i, StringComparer.Ordinal);
            var wideFamilies = FamilyWords
                .Select(f => (f.Family, Indices: f.Words.Where(wideAt.ContainsKey).Select(w => wideAt[w]).ToArray()))
                .Where(f => f.Indices.Length > 0)
                .ToArray();
            var wideFamilyBase = new double[wideFamilies.Length];
            for (var f = 0; f < wideFamilies.Length; f++)
            {
                var set = wideFamilies[f].Indices.ToHashSet();
                wideFamilyBase[f] = cand.Count(row => row.Any(set.Contains)) / (double)Math.Max(n, 1);
            }

            progress?.Report($"groups: naming ready ({cand.Count(x => x.Length > 0)} photos with carried words)");
            return new VocabularyNamer(core, wide, n, d, pcal, cz, top, baseRates, cand, cbase, wtop, wbase, wideFamilies, wideFamilyBase);
        }

        public NamingResult Name(int[] members)
        {
            var (label, support, lift, kind) = CoverageName(members);
            var hint = Array.Empty<string>();
            if (string.IsNullOrEmpty(label))
            {
                var second = SecondName(members);
                label = second.Label;
                kind = second.Kind;
                hint = second.Hint;
            }
            return new NamingResult(label, kind, hint, Votes(members).Take(5).Select(v => (v.Term, Round3(v.Score))).ToArray());
        }

        private (string Label, double Support, double Lift, string Kind) CoverageName(int[] members)
        {
            if (members.Length == 0) return ("", 0, 0, "");
            var counts = new double[_wide.Length];
            foreach (var i in members)
                foreach (var w in _cand[i])
                    counts[w] += 1;
            var best = -1;
            var bestCov = 0.0;
            var bestLift = 0.0;
            for (var w = 0; w < _wide.Length; w++)
            {
                var cov = counts[w] / members.Length;
                if (cov < NameCover) continue;
                var lf = cov / Math.Max(_cbase[w], 1e-9);
                if (cov > bestCov || (cov == bestCov && lf > bestLift))
                {
                    best = w; bestCov = cov; bestLift = lf;
                }
            }
            if (best >= 0) return (_wide[best], bestCov, bestLift, "word");

            var bestFamily = "";
            var bestFamilyCov = 0.0;
            var bestFamilyLift = 0.0;
            for (var f = 0; f < _wideFamilies.Length; f++)
            {
                var set = _wideFamilies[f].Indices.ToHashSet();
                var cov = members.Count(i => _cand[i].Any(set.Contains)) / (double)members.Length;
                if (cov >= NameCover && cov > bestFamilyCov)
                {
                    bestFamily = _wideFamilies[f].Family;
                    bestFamilyCov = cov;
                    bestFamilyLift = cov / Math.Max(_wideFamilyBase[f], 1e-9);
                }
            }
            return string.IsNullOrEmpty(bestFamily) ? ("", 0, 0, "") : (bestFamily, bestFamilyCov, bestFamilyLift, "family");
        }

        private (string Label, string Kind, string[] Hint) SecondName(int[] members)
        {
            var hint = MixedHint(members);
            return ("", "", hint);
        }

        private string[] MixedHint(int[] members)
        {
            if (members.Length == 0) return [];
            var counts = new double[_wide.Length];
            foreach (var i in members)
                foreach (var w in _cand[i])
                    counts[w] += 1;
            var output = new List<string>();
            foreach (var w in counts.Select((count, i) => (count, i)).OrderByDescending(x => x.count).ThenBy(x => x.i))
            {
                var cov = w.count / members.Length;
                if (cov < PartCover) break;
                if (cov / Math.Max(_cbase[w.i], 1e-9) < Lift) continue;
                var word = _wide[w.i];
                var parts = word.Split(' ', StringSplitOptions.RemoveEmptyEntries).ToHashSet(StringComparer.Ordinal);
                if (output.Any(o => o.Split(' ', StringSplitOptions.RemoveEmptyEntries).ToHashSet(StringComparer.Ordinal).Overlaps(parts))) continue;
                output.Add(word);
                if (output.Count == 2) break;
            }
            return output.ToArray();
        }

        private IReadOnlyList<(string Term, double Score)> Votes(int[] members)
        {
            var pm = new double[_core.Length];
            var zm = new double[_core.Length];
            foreach (var i in members)
            {
                for (var w = 0; w < _core.Length; w++)
                {
                    pm[w] += _pcal[i * _core.Length + w];
                    zm[w] += _cz[i * _core.Length + w];
                }
            }
            var votes = new Dictionary<string, double>(StringComparer.Ordinal);
            var order = new List<string>();
            foreach (var scores in new[] { pm, zm })
            {
                foreach (var item in scores.Select((score, i) => (score, i)).OrderByDescending(x => x.score).Take(3).Select((x, rank) => (x.i, rank)))
                {
                    var term = _core[item.i];
                    if (!votes.ContainsKey(term)) order.Add(term);
                    votes[term] = votes.GetValueOrDefault(term) + (3 - item.rank);
                }
            }
            return order.Select(t => (t, votes[t])).OrderByDescending(x => x.Item2).ToArray();
        }

        private static async Task<float[]> EncodeTerms(EmbeddingIndex index, IReadOnlyList<string> terms, CancellationToken ct)
        {
            var prompts = terms.SelectMany(term => Templates.Select(t => t.Replace("{}", term, StringComparison.Ordinal))).ToArray();
            var rows = await index.EncodeTextAsync(prompts, ct);
            var d = index.Dimensions;
            var output = new float[terms.Count * d];
            for (var i = 0; i < terms.Count; i++)
            {
                var row = new float[d];
                for (var t = 0; t < Templates.Length; t++)
                    for (var k = 0; k < d; k++)
                        row[k] += rows[i * Templates.Length + t][k] / Templates.Length;
                Normalize(row);
                Array.Copy(row, 0, output, i * d, d);
            }
            return output;
        }

        private static float[] ComputeScores(float[] rows, int n, float[] vectors, int m, int d)
        {
            return ExactNeighbors.MultiplyTranspose(rows, n, vectors, m, d);
        }

        private static float[] ZScoreColumns(float[] scores, int n, int m)
        {
            Parallel.For(0, (m + 31) / 32, block =>
            {
                var start = block * 32; var count = Math.Min(32, m - start);
                var mean = new double[count];
                var variance = new double[count];
                for (var i = 0; i < n; i++)
                    for (var j = 0; j < count; j++) mean[j] += scores[i * m + start + j];
                for (var j = 0; j < count; j++) mean[j] /= n;
                for (var i = 0; i < n; i++)
                    for (var j = 0; j < count; j++)
                    {
                        var delta = scores[i * m + start + j] - mean[j];
                        variance[j] += delta * delta;
                    }
                for (var j = 0; j < count; j++) variance[j] = Math.Sqrt(variance[j] / n) + 1e-9;
                for (var i = 0; i < n; i++)
                    for (var j = 0; j < count; j++) scores[i * m + start + j] = (float)((scores[i * m + start + j] - mean[j]) / variance[j]);
            });
            return scores;
        }

        private static float SelectKth(float[] values, int count, int k)
        {
            var left = 0; var right = count - 1;
            while (left < right)
            {
                var pivot = values[left + (right - left) / 2];
                var low = left; var at = left; var high = right;
                while (at <= high)
                {
                    if (values[at] < pivot) { (values[low], values[at]) = (values[at], values[low]); low++; at++; }
                    else if (values[at] > pivot) { (values[at], values[high]) = (values[high], values[at]); high--; }
                    else at++;
                }
                if (k < low) right = low - 1;
                else if (k > high) left = high + 1;
                else return values[k];
            }
            return values[k];
        }

        private static float[] MeanRows(float[] rows, int n, int d)
        {
            var mean = new float[d];
            for (var i = 0; i < n; i++)
                for (var k = 0; k < d; k++)
                    mean[k] += rows[i * d + k] / n;
            return mean;
        }

        private static float[] CenterText(float[] rows, int n, int d, float[] mean)
        {
            var output = new float[rows.Length];
            for (var i = 0; i < n; i++)
            {
                for (var k = 0; k < d; k++) output[i * d + k] = rows[i * d + k] - mean[k];
                Normalize(output.AsSpan(i * d, d));
            }
            return output;
        }

        private static IReadOnlyList<int> TopIndices(ReadOnlySpan<float> row, int k)
        {
            var best = new List<(int Index, float Score)>(k + 1);
            for (var i = 0; i < row.Length; i++)
            {
                var score = row[i];
                if (best.Count < k || score > best[^1].Score)
                {
                    var at = best.Count;
                    while (at > 0 && best[at - 1].Score < score) at--;
                    best.Insert(at, (i, score));
                    if (best.Count > k) best.RemoveAt(best.Count - 1);
                }
            }
            return best.Select(x => x.Index).ToArray();
        }

        private static float Cosine(double probability) => (float)((Math.Log(probability / (1 - probability)) - EmbeddingIndex.Beta) / EmbeddingIndex.Tau);
    }

    private static string Normalize(string value)
    {
        value = PhotoSearch.Normalize(Photo.Readable(value));
        value = Regex.Replace(value, @"[^\p{L}\p{Nd}\s]+", " ");
        return Regex.Replace(value, @"\s+", " ").Trim();
    }

    private static float[] CenteredRows(float[] embeddings, int n, int d)
    {
        var mean = new float[d];
        for (var i = 0; i < n; i++)
            for (var k = 0; k < d; k++)
                mean[k] += embeddings[i * d + k] / n;
        var output = new float[embeddings.Length];
        for (var i = 0; i < n; i++)
        {
            for (var k = 0; k < d; k++) output[i * d + k] = embeddings[i * d + k] - mean[k];
            Normalize(output.AsSpan(i * d, d));
        }
        return output;
    }

    private static IReadOnlyList<(int Index, float Similarity)>[] BuildKnn(float[] rows, int n, int d, int k, CancellationToken ct)
    {
        var result = ExactNeighbors.Find(rows, n, d, k, cosine: true, cancellationToken: ct);
        var knn = new IReadOnlyList<(int Index, float Similarity)>[n];
        Parallel.For(0, n, i =>
        {
            knn[i] = result.Indices[i].Select((id, j) => (id, (float)result.Distances[i][j])).ToArray();
        });
        return knn;
    }

    private static IReadOnlyList<(int A, int B, int N)> BuildLinks(int[] photoGroup, IReadOnlyList<(int Index, float Similarity)>[] knn)
    {
        var links = new Dictionary<(int, int), int>();
        var order = new List<(int, int)>();
        for (var i = 0; i < photoGroup.Length; i++)
        {
            var gi = photoGroup[i];
            if (gi < 0) continue;
            foreach (var (j, _) in knn[i].Take(6))
            {
                if (j < 0 || j >= photoGroup.Length) continue;
                var gj = photoGroup[j];
                if (gj < 0 || gj == gi) continue;
                var key = (Math.Min(gi, gj), Math.Max(gi, gj));
                if (!links.ContainsKey(key)) order.Add(key);
                links[key] = links.GetValueOrDefault(key) + 1;
            }
        }
        return order.Select(k => (A: k.Item1, B: k.Item2, N: links[k]))
            .OrderByDescending(x => x.N)
            .ThenBy(x => x.A)
            .ThenBy(x => x.B)
            .Take(100)
            .ToArray();
    }

    private static int[] BuildBridges(int[] photoGroup, IReadOnlyList<(int Index, float Similarity)>[] knn)
    {
        return Enumerable.Range(0, photoGroup.Length)
            .Select(i => (Index: i, Span: knn[i].Take(6).Select(p => p.Index >= 0 && p.Index < photoGroup.Length ? photoGroup[p.Index] : -1).Where(g => g >= 0 && g != photoGroup[i]).Distinct().Count()))
            .Where(x => x.Span >= 3)
            .OrderByDescending(x => x.Span)
            .ThenBy(x => x.Index)
            .Take(12)
            .Select(x => x.Index)
            .ToArray();
    }

    private static float[] Center(float[] rows, int d, IReadOnlyList<int> members)
    {
        var center = new float[d];
        foreach (var i in members)
            for (var k = 0; k < d; k++)
                center[k] += rows[i * d + k] / Math.Max(members.Count, 1);
        Normalize(center);
        return center;
    }

    private static int Representative(float[] rows, int d, IReadOnlyList<int> members, float[] center)
    {
        var best = members[0];
        var bestScore = Dot(rows.AsSpan(best * d, d), center);
        foreach (var i in members.Skip(1))
        {
            var score = Dot(rows.AsSpan(i * d, d), center);
            if (score > bestScore)
            {
                best = i;
                bestScore = score;
            }
        }
        return best;
    }

    private static float[] CenterTextRows(IReadOnlyList<float[]> rows, int d)
    {
        var mean = new float[d];
        foreach (var row in rows)
            for (var k = 0; k < d; k++)
                mean[k] += row[k] / rows.Count;
        var output = new float[rows.Count * d];
        for (var i = 0; i < rows.Count; i++)
        {
            for (var k = 0; k < d; k++) output[i * d + k] = rows[i][k] - mean[k];
            Normalize(output.AsSpan(i * d, d));
        }
        return output;
    }

    private static float Dot(ReadOnlySpan<float> a, ReadOnlySpan<float> b)
    {
        var sum = 0f;
        for (var i = 0; i < a.Length; i++) sum += a[i] * b[i];
        return sum;
    }

    private static void Normalize(float[] values) => Normalize(values.AsSpan());
    private static void Normalize(Span<float> values)
    {
        double sum = 0;
        foreach (var v in values) sum += v * v;
        var norm = (float)Math.Max(Math.Sqrt(sum), 1e-9);
        for (var i = 0; i < values.Length; i++) values[i] /= norm;
    }

    private static void NormalizeRows(float[] rows, int n, int d)
    {
        for (var i = 0; i < n; i++) Normalize(rows.AsSpan(i * d, d));
    }

    private static float[] ReadNpyFloatMatrix(string path, out int rows, out int cols)
    {
        const long MaxBytes = 1L << 30;
        var length = new FileInfo(path).Length;
        if (length > MaxBytes) throw new InvalidDataException($"{path} is larger than {MaxBytes} bytes.");
        var data = File.ReadAllBytes(path);
        if (data.Length < 16 || data[0] != 0x93 || Encoding.ASCII.GetString(data, 1, 5) != "NUMPY")
            throw new InvalidDataException($"{path} is not a NumPy .npy file.");
        var major = data[6];
        if (major is not (1 or 2 or 3)) throw new InvalidDataException($"{path} has unsupported .npy version {major}.");
        var headerLength = major == 1
            ? BinaryPrimitives.ReadUInt16LittleEndian(data.AsSpan(8, 2))
            : BinaryPrimitives.ReadInt32LittleEndian(data.AsSpan(8, 4));
        var headerStart = major == 1 ? 10 : 12;
        if (headerLength <= 0 || headerLength > 65536 || headerStart + headerLength > data.Length)
            throw new InvalidDataException($"{path} has an invalid .npy header length.");
        var header = Encoding.ASCII.GetString(data, headerStart, headerLength);
        var descr = header.Contains("'<f4'") || header.Contains("\"<f4\"") ? "f4" :
            header.Contains("'<f2'") || header.Contains("\"<f2\"") ? "f2" : "";
        if (descr.Length == 0 || header.Contains("True")) throw new InvalidDataException($"{path} must be little-endian C-order float16/float32.");
        var shapeMatch = Regex.Match(header, @"'shape': \(([^)]*)\)");
        if (!shapeMatch.Success) shapeMatch = Regex.Match(header, @"""shape"": \(([^)]*)\)");
        if (!shapeMatch.Success) throw new InvalidDataException($"{path} has no shape in its .npy header.");
        var dims = shapeMatch.Groups[1].Value.Split(',', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries)
            .Select(value => int.Parse(value, System.Globalization.CultureInfo.InvariantCulture)).ToArray();
        if (dims.Length is < 1 or > 2 || dims.Any(d => d <= 0)) throw new InvalidDataException($"{path} must hold a non-empty 1-D or 2-D array.");
        rows = dims[0];
        cols = dims.Length > 1 ? dims[1] : 1;
        var offset = headerStart + headerLength;
        var count = checked(rows * cols);
        var elementSize = descr == "f4" ? sizeof(float) : 2;
        if (checked((long)count * elementSize) != data.Length - offset)
            throw new InvalidDataException($"{path} payload does not match its shape {rows}x{cols}.");
        var output = new float[count];
        if (descr == "f4")
        {
            Buffer.BlockCopy(data, offset, output, 0, output.Length * sizeof(float));
        }
        else
        {
            for (var i = 0; i < output.Length; i++)
            {
                var bits = BinaryPrimitives.ReadUInt16LittleEndian(data.AsSpan(offset + i * 2, 2));
                output[i] = (float)BitConverter.UInt16BitsToHalf(bits);
            }
        }
        return output;
    }

    private static void WriteNpy(string path, float[] values, int rows, int cols)
    {
        var header = $"{{'descr': '<f4', 'fortran_order': False, 'shape': ({rows}, {cols}), }}";
        while ((10 + header.Length + 1) % 64 != 0) header += " ";
        header += "\n";
        var bytes = new byte[10 + header.Length + values.Length * sizeof(float)];
        bytes[0] = 0x93;
        Encoding.ASCII.GetBytes("NUMPY").CopyTo(bytes, 1);
        bytes[6] = 1;
        BinaryPrimitives.WriteUInt16LittleEndian(bytes.AsSpan(8, 2), (ushort)header.Length);
        Encoding.ASCII.GetBytes(header).CopyTo(bytes, 10);
        Buffer.BlockCopy(values, 0, bytes, 10 + header.Length, values.Length * sizeof(float));
        var tmp = path + ".part";
        File.WriteAllBytes(tmp, bytes);
        File.Move(tmp, path, overwrite: true);
    }

    private static float Percentile(float[] sorted, double q)
    {
        if (sorted.Length == 0) return 0;
        var p = q * (sorted.Length - 1);
        var lo = (int)Math.Floor(p);
        var hi = Math.Min(lo + 1, sorted.Length - 1);
        return (float)(sorted[lo] + (p - lo) * (sorted[hi] - sorted[lo]));
    }

    private static float Round3(double value) => (float)Math.Round(value, 3, MidpointRounding.ToEven);

    private static string HashIds(IReadOnlyList<string> ids)
    {
        using var sha = SHA256.Create();
        var bytes = Encoding.UTF8.GetBytes(string.Join('\n', ids));
        return Convert.ToHexString(sha.ComputeHash(bytes)).ToLowerInvariant();
    }

    private static void WriteJsonAtomically<T>(string path, T value)
    {
        var tmp = path + ".part";
        using (var file = File.Create(tmp)) JsonSerializer.Serialize(file, value, JsonOptions);
        File.Move(tmp, path, overwrite: true);
    }

}

internal sealed record CacheFile
{
    public string Version { get; init; } = "";
    public string ModelHash { get; init; } = "";
    public string PhotoIdsHash { get; init; } = "";
    public DateTimeOffset CreatedUtc { get; init; }
    public double ElapsedSeconds { get; init; }
    public int JoinedUngrouped { get; init; }
    public int UngroupedCount { get; init; }
    public List<GroupDto> Groups { get; init; } = [];
    public List<GroupDto> Supers { get; init; } = [];
    public int[] PhotoGroup { get; init; } = [];
    public int[] PhotoSuper { get; init; } = [];
    public List<LinkDto> Links { get; init; } = [];
    public int[] Bridges { get; init; } = [];
    public List<List<KnnDto>> Knn { get; init; } = [];
}

internal sealed record GroupDto
{
    public int Id { get; init; }
    public string Label { get; init; } = "";
    public string NamedBy { get; init; } = "";
    public string Qualifier { get; init; } = "";
    public string[] Hint { get; init; } = [];
    public int Size { get; init; }
    public int[] Members { get; init; } = [];
    public int Representative { get; init; }
    public float Coherence { get; init; }
    public List<YearDto> Years { get; init; } = [];
    public List<TagDto> Tags { get; init; } = [];
    public int SuperId { get; init; }
    public int[] Leaves { get; init; } = [];
}

internal sealed record TagDto(string Term, float Score);
internal sealed record YearDto(int Year, int Count);
internal sealed record LinkDto(int A, int B, int N);
internal sealed record KnnDto(int Index, float Similarity);

public static partial class VisualGroups
{
    private static CacheFile ToCache(string version, string modelHash, string photoHash, IReadOnlyList<VisualGroup> groups,
        IReadOnlyList<VisualGroup> supers, int[] photoGroup, int[] photoSuper, IReadOnlyList<(int A, int B, int N)> links,
        int[] bridges, IReadOnlyList<(int Index, float Similarity)>[] knn, int joined, int ungrouped, TimeSpan elapsed) => new()
    {
        Version = version,
        ModelHash = modelHash,
        PhotoIdsHash = photoHash,
        CreatedUtc = DateTimeOffset.UtcNow,
        ElapsedSeconds = elapsed.TotalSeconds,
        JoinedUngrouped = joined,
        UngroupedCount = ungrouped,
        Groups = groups.Select(ToDto).ToList(),
        Supers = supers.Select(ToDto).ToList(),
        PhotoGroup = photoGroup,
        PhotoSuper = photoSuper,
        Links = links.Select(l => new LinkDto(l.A, l.B, l.N)).ToList(),
        Bridges = bridges,
        Knn = knn.Select(row => row.Select(k => new KnnDto(k.Index, k.Similarity)).ToList()).ToList()
    };

    private static VisualGroupSet FromCache(CacheFile cache, TimeSpan elapsed) => new()
    {
        Groups = cache.Groups.Select(FromDto).ToArray(),
        Supers = cache.Supers.Select(FromDto).ToArray(),
        PhotoGroup = cache.PhotoGroup,
        PhotoSuper = cache.PhotoSuper,
        Links = cache.Links.Select(l => (l.A, l.B, l.N)).ToArray(),
        Bridges = cache.Bridges,
        Knn = cache.Knn.Select(row => (IReadOnlyList<(int Index, float Similarity)>)row.Select(k => (k.Index, k.Similarity)).ToArray()).ToArray(),
        Elapsed = elapsed,
        UngroupedCount = cache.UngroupedCount
    };

    private static GroupDto ToDto(VisualGroup g) => new()
    {
        Id = g.Id,
        Label = g.Label,
        NamedBy = g.NamedBy,
        Qualifier = g.Qualifier,
        Hint = g.Hint.ToArray(),
        Size = g.Size,
        Members = g.Members,
        Representative = g.Representative,
        Coherence = g.Coherence,
        Years = g.Years.Select(y => new YearDto(y.Year, y.Count)).ToList(),
        Tags = g.Tags.Select(t => new TagDto(t.Term, t.Score)).ToList(),
        SuperId = g.SuperId,
        Leaves = g.Leaves
    };

    private static VisualGroup FromDto(GroupDto g) => new(g.Id, g.Label, g.NamedBy, g.Qualifier, g.Hint, g.Size, g.Members,
        g.Representative, g.Coherence, g.Years.Select(y => (y.Year, y.Count)).ToArray(),
        g.Tags.Select(t => (t.Term, t.Score)).ToArray(), g.SuperId, g.Leaves);
}
