using Nebula.Core;
using System.Numerics;

var tests = 0;
void Check(bool condition, string name)
{
    if (!condition) throw new InvalidOperationException($"FAIL: {name}");
    Console.WriteLine($"PASS: {name}");
    tests++;
}

Photo Make(int id) => new()
{
    Id = id.ToString("x64"), OriginalPath = $"C:\\photos\\{id}.jpg", ThumbnailPath = $"C:\\cache\\{id}.jpg",
    Title = "Açores landscape", Artist = "José", Department = "Photography",
    Red = (id % 255) / 255f, Green = 0.5f, Blue = 0.25f, Width = 1600, Height = 1200
};
var photos = Enumerable.Range(0, 5000).Select(Make).ToArray();
Check(PhotoSearch.Matches(photos[0], "ACORES jose"), "accent-insensitive conjunctive metadata search");
Check(!PhotoSearch.Matches(photos[0], "jose sculpture"), "missing term does not match");
Check(PhotoSearch.Matches(photos[0], ""), "empty search includes every photo");
Check(PhotoSearch.Matches(photos[0], "0.jpg"), "filenames are searchable");
foreach (var arrangement in Enum.GetValues<Arrangement>())
{
    var positions = Layout.Positions(photos, arrangement);
    Check(positions.Length == 5000, $"{arrangement}: every item retained");
    Check(positions.All(p => float.IsFinite(p.X) && float.IsFinite(p.Y) && float.IsFinite(p.Z)), $"{arrangement}: finite positions");
    Check(positions.SequenceEqual(Layout.Positions(photos, arrangement)), $"{arrangement}: repeatable layout");
    Check(Layout.Positions([], arrangement).Length == 0, $"{arrangement}: empty library");
}
Check(Layout.Positions(photos, Arrangement.Cloud).Distinct().Count() == 5000, "cloud has no repeated positions at 5,000");
Check(Vector3.Distance(Layout.ToOklab(1, 1, 1), new Vector3(1, 0, 0)) < 0.001, "OKLab white");
Check(Layout.ToOklab(0, 0, 0) == Vector3.Zero, "OKLab black");
Check(photos[0].Aspect > 1.333f && photos[0].Aspect < 1.334f, "image aspect retained");
var square = Layout.Positions(photos.Take(100).ToArray(), Arrangement.Grid);
Check(square[0] == new Vector3(-10, 10, 0) && square[1] == new Vector3(-8, 10, 0) &&
      square[10] == new Vector3(-10, 8, 0), "grid uses 64-unit spacing at a 32-unit tile scale");
Check(!photos[0].ToString().Contains(photos[0].OriginalPath), "accessible item names do not expose filesystem metadata");
var dated = photos.Take(2).Select((p, i) => p with { TakenAt = new DateTimeOffset(2020 + i, 1, 1, 0, 0, 0, TimeSpan.Zero) }).ToArray();
var timePositions = Layout.Positions(dated, Arrangement.Time);
Check(timePositions[1].Y > timePositions[0].Y, "capture years advance on the helix");
Check(Photo.Readable("1875 date QS:P571,+1875-00-00T00:00:00Z/9") == "1875", "Wikidata machine dates are hidden");
Check(Photo.Readable("Jules Lachaise   /   Eugène-Pierre Gourdet") == "Jules Lachaise / Eugène-Pierre Gourdet", "padded separators collapse");
Check(Photo.Readable("ca. 1830") == "ca. 1830", "ordinary dates are unchanged");
var synthetic = new float[]
{
    1, 0, 0, 0,
    0.98f, 0.02f, 0, 0,
    0, 1, 0, 0,
    0.02f, 0.98f, 0, 0,
    0, 0, 1, 0,
    0, 0.02f, 0.98f, 0
};
var simA = Layout.SimilarityPositions(synthetic, 6, 4);
var simB = Layout.SimilarityPositions(synthetic, 6, 4);
Check(simA.SequenceEqual(simB), "similarity layout is deterministic");
Check(simA.All(p => float.IsFinite(p.X) && float.IsFinite(p.Y) && float.IsFinite(p.Z)), "similarity layout finite");
float Distance(int a, int b) => Vector3.Distance(simA[a], simA[b]);
Check(Distance(0, 1) < Distance(0, 2) && Distance(2, 3) < Distance(2, 4) && Distance(4, 5) < Distance(4, 0),
    "similarity layout preserves synthetic neighbours");
var nn = Layout.NearestEmbeddingRows(synthetic, 6, 4, 1);
Check(nn[0][0] == 1 && nn[2][0] == 3 && nn[4][0] == 5, "embedding neighbours preserve cosine order");
var blobs = new List<float>();
void AddBlob(float cx, float cy, int n)
{
    for (var i = 0; i < n; i++)
    {
        blobs.Add(cx + (i % 5) * 0.01f);
        blobs.Add(cy + (i / 5) * 0.01f);
    }
}
AddBlob(0, 0, 15);
AddBlob(5, 0, 15);
AddBlob(0, 5, 15);
blobs.AddRange([20f, 20f, -20f, 20f, 20f, -20f]);
var hdb = Hdbscan.LeafClusters(blobs.ToArray(), 48, 2, minClusterSize: 10, minSamples: 2);
Check(hdb.Take(15).Distinct().Count() == 1 && hdb.Skip(15).Take(15).Distinct().Count() == 1 &&
      hdb.Skip(30).Take(15).Distinct().Count() == 1, "HDBSCAN finds synthetic blobs");
Check(hdb.Take(45).Where(x => x >= 0).Distinct().Count() == 3, "HDBSCAN returns three blob labels");
Check(hdb.Skip(45).All(x => x == -1), "HDBSCAN marks synthetic noise");
var random = new Random(42);
var exactFixture = Enumerable.Range(0, 73 * 19).Select(_ => (float)(random.NextDouble() * 2 - 1)).ToArray();
var exact = ExactNeighbors.Find(exactFixture, 73, 19, 7);
var cosine = ExactNeighbors.Find(exactFixture, 73, 19, 7, cosine: true);
for (var i = 0; i < 73; i++)
{
    var euclideanOrder = Enumerable.Range(0, 73).Where(j => i != j).OrderBy(j => ExactNeighbors.SquaredDistance(exactFixture, 19, i, j)).ThenBy(j => j).Take(7);
    if (!exact.Indices[i].SequenceEqual(euclideanOrder)) throw new InvalidOperationException("Exact Euclidean kNN differs from brute force.");
    var cosineOrder = Enumerable.Range(0, 73).Where(j => i != j)
        .OrderByDescending(j => Enumerable.Range(0, 19).Sum(d => (double)exactFixture[i * 19 + d] * exactFixture[j * 19 + d])).ThenBy(j => j).Take(7);
    if (!cosine.Indices[i].SequenceEqual(cosineOrder)) throw new InvalidOperationException("Exact cosine kNN differs from brute force.");
}
Check(true, "blocked kNN matches brute-force Euclidean and cosine, including partial tiles");
var duplicateRows = new float[17 * 10];
Array.Fill(duplicateRows, 100000f);
duplicateRows[16 * 10] += .125f;
var duplicates = ExactNeighbors.Find(duplicateRows, 17, 10, 4);
Check(duplicates.Indices[0].SequenceEqual(new[] { 1, 2, 3, 4 }), "duplicate kNN ties use ascending stable IDs");
Check(duplicates.Indices[16].SequenceEqual(new[] { 0, 1, 2, 3 }), "Gram cancellation cannot discard exact burst neighbours");
Check(ExactNeighbors.Find([1, 2], 1, 2, 12).Indices.Single().Length == 0, "singleton kNN has no self neighbour");
var product = ExactNeighbors.MultiplyTranspose(exactFixture, 73, exactFixture, 73, 19);
Check(Enumerable.Range(0, 73 * 73).All(i =>
    Math.Abs(product[i] - Enumerable.Range(0, 19).Sum(d => (double)exactFixture[i / 73 * 19 + d] * exactFixture[i % 73 * 19 + d])) < 2e-5),
    "blocked float32 GEMM matches scalar products");
var pcaA = ExactNeighbors.PrincipalComponents(exactFixture, 73, 19, 5);
var pcaB = ExactNeighbors.PrincipalComponents(exactFixture, 73, 19, 5);
Check(pcaA.SequenceEqual(pcaB), "full-covariance PCA is deterministic");
Check(pcaA.All(float.IsFinite), "parallel PCA produces finite scores");
var repeatHdb = Hdbscan.LeafClusters(blobs.ToArray(), 48, 2);
Check(hdb.SequenceEqual(repeatHdb), "exact Boruvka reachability clustering is deterministic");
using (var cancelled = new CancellationTokenSource())
{
    cancelled.Cancel();
    var observed = false;
    try { ExactNeighbors.Find(exactFixture, 73, 19, 7, cancellationToken: cancelled.Token); }
    catch (OperationCanceledException) { observed = true; }
    Check(observed, "exact kNN observes cancellation");
}
var rankOne = Enumerable.Range(0, 64).SelectMany(i => new[] { (float)i, 2f * i, -3f * i, 0f }).ToArray();
var rankPca = ExactNeighbors.PrincipalComponents(rankOne, 64, 4, 3);
Check(rankPca.All(float.IsFinite) && Enumerable.Range(0, 64).All(i => Math.Abs(rankPca[i * 3 + 1]) < .01f && Math.Abs(rankPca[i * 3 + 2]) < .01f),
    "rank-deficient PCA does not invent random projected dimensions");
var parallelFixture = Enumerable.Range(0, 300 * 16).Select(_ => (float)random.NextDouble()).ToArray();
var parallelA = Layout.PaCMAPProjection(parallelFixture, 300, 16, 3, 42, mnRatio: .3, fpRatio: 4);
var parallelB = Layout.PaCMAPProjection(parallelFixture, 300, 16, 3, 42, mnRatio: .3, fpRatio: 4);
Check(parallelA.SequenceEqual(parallelB), "parallel PaCMAP gradient partitions and ordered reduction are bit-repeatable");
Check(ExactNeighbors.PrincipalComponents([1, 2, 3, 4, 2, 4, 6, 8], 2, 4, 3).All(float.IsFinite),
    "PCA handles fewer rows than requested components");
Check(pcaA.SequenceEqual(ExactNeighbors.PrincipalComponents(exactFixture, 73, 19, 5, seed: 17)),
    "full PCA has no randomized seed dependence");
var eigenMethod = typeof(ExactNeighbors).GetMethod("SymmetricEigensystem",
    System.Reflection.BindingFlags.NonPublic | System.Reflection.BindingFlags.Static)!;
foreach (var size in new[] { 1, 2, 3, 19 })
{
    var symmetric = new double[size * size];
    for (var i = 0; i < size; i++)
        for (var j = 0; j <= i; j++) symmetric[i * size + j] = symmetric[j * size + i] = random.NextDouble() * 2 - 1;
    if (eigenMethod.Invoke(null, [symmetric.ToArray(), size]) is not ValueTuple<double[], double[]> decomposition)
        throw new InvalidOperationException("No symmetric eigendecomposition result.");
    var (values, vectors) = decomposition;
    for (var col = 0; col < size; col++)
        for (var row = 0; row < size; row++)
        {
            double av = 0, orthogonal = 0;
            for (var j = 0; j < size; j++)
            {
                av += symmetric[row * size + j] * vectors[j * size + col];
                orthogonal += vectors[j * size + row] * vectors[j * size + col];
            }
            if (Math.Abs(av - values[col] * vectors[row * size + col]) > 1e-10 ||
                Math.Abs(orthogonal - (row == col ? 1 : 0)) > 1e-10)
                throw new InvalidOperationException("Full covariance eigenvectors fail residual or orthogonality.");
        }
}
Check(true, "full symmetric eigensolver satisfies Av=lambda*v and orthogonality");
// Spacing at scale: every analytic layout is scaled to a 2-tile median neighbour distance
// (2 × 32 layout units); renderer units are tiles, so the median must be ~2 at
// 50K too, not ~0.9 (the old sampled estimate measured samples against each other).
static float MedianNearest(Vector3[] p, int sample)
{
    var d = new float[sample];
    System.Threading.Tasks.Parallel.For(0, sample, s =>
    {
        var i = (int)((long)s * p.Length / sample);
        var best = float.MaxValue;
        for (var j = 0; j < p.Length; j++) if (j != i) best = MathF.Min(best, Vector3.DistanceSquared(p[i], p[j]));
        d[s] = MathF.Sqrt(best);
    });
    Array.Sort(d);
    return d[sample / 2];
}
var big = Enumerable.Range(0, 50_000).Select(Make).ToArray();
foreach (var mode in new[] { Arrangement.Cloud, Arrangement.Colour })
{
    var median = MedianNearest(Layout.Positions(big, mode), 2000);
    Console.WriteLine($"  {mode} 50K median nearest = {median:F2} tiles");
    Check(median > 1.6f && median < 2.6f, $"{mode} at 50K keeps the 2-tile median spacing");
}
// Search volume: matches that look alike sit together, stronger matches sit
// nearer (+Z here; the app flips Z for the native camera), and no two tiles overlap.
{
    var rng = new Random(7);
    float[] Row(int cluster)
    {
        var r = new float[64];
        for (var j = 0; j < 64; j++) r[j] = (float)(rng.NextDouble() - 0.5) * 0.4f;
        r[cluster == 0 ? 3 : 11] += 3;
        return r;
    }
    var rows = Enumerable.Range(0, 600).Select(i => Row(i % 2)).ToArray();
    var scores = Enumerable.Range(0, 600).Select(_ => (float)rng.NextDouble()).ToArray();
    var focus = Layout.FocusPositions(rows, scores);
    Check(focus.SequenceEqual(Layout.FocusPositions(rows, scores)), "search volume is deterministic");
    Check(focus.All(p => float.IsFinite(p.X) && float.IsFinite(p.Y) && float.IsFinite(p.Z)), "search volume is finite");
    float MeanX(int c) => Enumerable.Range(0, 600).Where(i => i % 2 == c).Average(i => focus[i].X);
    var spreadX = MathF.Sqrt(focus.Average(p => p.X * p.X));
    Check(MathF.Abs(MeanX(0) - MeanX(1)) > spreadX, "visually distinct matches separate along the first axis");
    var order = Enumerable.Range(0, 600).OrderBy(i => scores[i]).ToArray();
    var weakZ = order.Take(150).Average(i => focus[i].Z);
    var strongZ = order.TakeLast(150).Average(i => focus[i].Z);
    Check(strongZ > weakZ, "stronger matches sit nearer the camera");
    var nearest = focus.Select((p, i) => focus.Where((_, j) => j != i).Min(q => Vector3.Distance(p, q))).OrderBy(d => d).ToArray();
    Console.WriteLine($"  search volume: 1st-percentile spacing {nearest[6]:F2}, median {nearest[300]:F2} tiles");
    Check(nearest[6] > 1.2f, "search tiles are pushed apart (48-unit cells, 1.5 tiles)");
    var many = Enumerable.Range(0, 3000).Select(i => Row(i % 2)).ToArray();
    var manyFocus = Layout.FocusPositions(many, Enumerable.Range(0, 3000).Select(i => i / 3000f).ToArray());
    Check(manyFocus.Length == 3000 && manyFocus.All(p => float.IsFinite(p.X)), "large searches fit axes on a sample and place every match");
}
{
    // Search in Grid: rank 0 top-left, reading order, 2-unit pitch, centred, lifted off the library sheet.
    var grid = Layout.RankedGrid(7, depth: -0.5f);
    Check(grid[0] == new Vector3(-2, 2, -0.5f) && grid[1] == new Vector3(0, 2, -0.5f) && grid[3] == new Vector3(-2, 0, -0.5f),
        "search grid is in rank order, top-left first");
    Check(Layout.RankedGrid(0).Length == 0, "empty search grid");
    // Search in Similarity: a sparse corner of the map regathers as a compact cloud with the same shape.
    var sparse = new[] { new Vector3(100, 100, 100), new Vector3(140, 100, 100), new Vector3(100, 180, 100), new Vector3(300, 100, 100) };
    var gathered = Layout.Regather(sparse);
    var centre = gathered.Aggregate(Vector3.Zero, (a, p) => a + p) / gathered.Length;
    Check(centre.Length() < 0.5f, "regathered matches are centred on the origin");
    Check(Vector3.Distance(gathered[0], gathered[1]) < Vector3.Distance(gathered[0], gathered[2]) &&
          Vector3.Distance(gathered[0], gathered[2]) < Vector3.Distance(gathered[0], gathered[3]), "regather keeps who sits nearest whom");
    Check(Vector3.Distance(gathered[0], gathered[1]) < 4, "regather packs at library tile spacing");
    // Search in Time: an undated knot no longer flings the dated helix apart.
    var mixed = Enumerable.Range(0, 200).Select(i => i < 140 ? photos[i] :
        photos[i] with { TakenAt = new DateTimeOffset(2004 + (i - 140) % 15, 1 + i % 12, 1, 0, 0, 0, TimeSpan.Zero) }).ToArray();
    var time = Layout.TimeSearchPositions(mixed);
    var together = Layout.Positions(mixed, Arrangement.Time);
    float Height(Vector3[] p) => p.Skip(140).Max(q => q.Y) - p.Skip(140).Min(q => q.Y);
    Console.WriteLine($"  search Time: dated helix {Height(time):F1} tall alone, {Height(together):F1} with the undated knot");
    Check(time.All(p => float.IsFinite(p.X)) && Height(time) < Height(together) / 2, "search Time keeps dated matches at tile scale");
    Check(time.Take(140).Max(p => p.Y) < time.Skip(140).Min(p => p.Y), "undated matches sit below the dated helix");
    var byDate = Enumerable.Range(140, 60).OrderBy(i => mixed[i].TakenAt).ThenBy(i => i).ToArray();
    Check(time[byDate[^1]].Y > time[byDate[0]].Y, "later matches climb the search helix");
    // Search in Similarity: the matches' own similarity cloud, reduced by randomized PCA first.
    var rand = new Random(3);
    var centres = Enumerable.Range(0, 4).Select(_ => Enumerable.Range(0, 300).Select(_ => (float)(rand.NextDouble() - 0.5)).ToArray()).ToArray();
    var matches = new float[160 * 300];
    for (var i = 0; i < 160; i++) for (var t = 0; t < 300; t++) matches[i * 300 + t] = centres[i % 4][t] + (float)(rand.NextDouble() - 0.5) * 0.6f;
    var cloud = Layout.SubsetSimilarityPositions(matches, 160, 300);
    Check(cloud.SequenceEqual(Layout.SubsetSimilarityPositions(matches, 160, 300)) && cloud.All(p => float.IsFinite(p.X)), "search similarity is deterministic and finite");
    var sameGroup = Enumerable.Range(0, 160).Count(i => Enumerable.Range(0, 160).Where(j => j != i).MinBy(j => Vector3.Distance(cloud[i], cloud[j])) % 4 == i % 4);
    Check(sameGroup >= 150, "search similarity keeps look-alike matches together");
    var cloudCentre = cloud.Aggregate(Vector3.Zero, (a, p) => a + p) / cloud.Length;
    Check(cloud.Max(p => Vector3.Distance(p, cloudCentre)) < 4 * MathF.Cbrt(160), "search similarity is packed to the search volume's size");
}
Console.WriteLine($"{tests} checks passed.");
