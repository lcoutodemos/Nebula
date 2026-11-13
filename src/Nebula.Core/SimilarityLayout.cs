using System.Numerics;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Runtime.Intrinsics;

namespace Nebula.Core;

public static partial class Layout
{
    /// <summary>
    /// The "semantic" layout: embeddings centred on the library mean
    /// and renormalised, PaCMAP (10 neighbours, MN 0.3, FP 4.0, seed 42),
    /// normed so the 99th-percentile |coordinate| is 900, scaled to a 1.5-tile median neighbour
    /// distance and relaxed apart at 1.4 tiles (150 sweeps, cap 3.2). Returned in renderer units
    /// (layout units / 32), like <see cref="Positions"/>.
    /// </summary>
    public static Vector3[] SimilarityPositions(ReadOnlySpan<float> embeddings, int rowCount, int dimensions, int seed = 42)
    {
        if (rowCount <= 0) return [];
        if (dimensions <= 0) throw new ArgumentOutOfRangeException(nameof(dimensions));
        if (embeddings.Length < checked(rowCount * dimensions)) throw new ArgumentException("The embedding buffer is smaller than rowCount × dimensions.", nameof(embeddings));
        if (rowCount == 1) return [Vector3.Zero];

        var mean = new double[dimensions];
        for (var i = 0; i < rowCount; i++)
            for (var d = 0; d < dimensions; d++) mean[d] += embeddings[i * dimensions + d];
        for (var d = 0; d < dimensions; d++) mean[d] /= rowCount;
        var x = new double[rowCount * dimensions];
        for (var i = 0; i < rowCount; i++)
        {
            var norm = 0.0;
            for (var d = 0; d < dimensions; d++)
            {
                var v = embeddings[i * dimensions + d] - mean[d];
                x[i * dimensions + d] = v;
                norm += v * v;
            }
            norm = Math.Max(Math.Sqrt(norm), 1e-9);
            for (var d = 0; d < dimensions; d++) x[i * dimensions + d] /= norm;
        }
        var y = PaCMAP.Project(x, rowCount, dimensions, outputDimensions: 3, nNeighbors: Math.Min(10, rowCount - 1),
            seed: (ulong)seed, mnRatio: 0.3, fpRatio: 4.0);

        var points = new Point[rowCount];
        for (var i = 0; i < rowCount; i++) points[i] = new Point(y[i * 3], y[i * 3 + 1], y[i * 3 + 2]);
        // Normalize: centred, with the 99th percentile of |coordinate| at 900.
        var centre = new Point(points.Average(p => p.X), points.Average(p => p.Y), points.Average(p => p.Z));
        for (var i = 0; i < rowCount; i++) points[i] -= centre;
        var magnitudes = points.SelectMany(p => new[] { Math.Abs(p.X), Math.Abs(p.Y), Math.Abs(p.Z) }).OrderBy(v => v).ToArray();
        var p99 = magnitudes[Math.Min(magnitudes.Length - 1, (int)(0.99 * (magnitudes.Length - 1)))];
        for (var i = 0; i < rowCount; i++) points[i] *= 900 / Math.Max(p99, 1e-9);
        // Scale so the median neighbour distance is 1.5 tiles, then relax overlaps at 1.4 tiles
        // (150 sweeps, cap 3.2).
        var nearest = Nearest(points);
        Array.Sort(nearest);
        var median = rowCount % 2 == 1 ? nearest[rowCount / 2] : (nearest[rowCount / 2 - 1] + nearest[rowCount / 2]) / 2;
        if (median > 1e-6)
            for (var i = 0; i < rowCount; i++) points[i] *= 1.5 * 32 / median;
        Relax(points, 1.4 * 32, iterations: 150, cap: 3.2);
        return points.Select(p => new Vector3((float)(p.X / 32), (float)(p.Y / 32), (float)(p.Z / 32))).ToArray();
    }

    /// <summary>
    /// <see cref="SimilarityPositions"/> for a small subset (search matches), fast enough to run per
    /// query and packed to the search volume's size: rows are first brought to at most 100
    /// principal axes with a randomized range finder (two power iterations) instead of PaCMAP's
    /// full-covariance PCA, which costs ~3.5 s at 1152 dimensions however few the rows.
    /// </summary>
    public static Vector3[] SubsetSimilarityPositions(float[] embeddings, int rowCount, int dimensions, int components = 100)
    {
        var n = rowCount; var d = dimensions; var k = Math.Min(components, n);
        if (n <= 2) return SimilarityPositions(embeddings, n, d);
        var layout = d <= components ? SimilarityPositions(embeddings, n, d) : Reduced();
        // The library spacing rule (median neighbour 1.5 tiles) flings a few tight clusters of
        // matches far apart. Pack the cloud to the search volume's size (90th-percentile radius
        // 2·∛n tiles, outliers held within 1.5× of it), then push overlapping tiles apart.
        var centre = layout.Aggregate(Vector3.Zero, (a, p) => a + p) / n;
        var radii = layout.Select(p => (p - centre).Length()).OrderBy(r => r).ToArray();
        var target = 2 * MathF.Cbrt(n);
        var scale = MathF.Min(1, target / MathF.Max(radii[(int)(0.9 * (n - 1))], 1e-6f));
        var packed = new Point[n];
        for (var i = 0; i < n; i++)
        {
            var v = (layout[i] - centre) * scale;
            var r = v.Length();
            if (r > 1.5f * target) v *= 1.5f * target / r;
            packed[i] = new Point(v.X * 32.0, v.Y * 32.0, v.Z * 32.0);
        }
        Relax(packed, 51.2);
        return packed.Select(p => new Vector3((float)(p.X / 32), (float)(p.Y / 32), (float)(p.Z / 32))).ToArray();

        Vector3[] Reduced()
        {
            var x = new double[n * d];
            var mean = new double[d];
            for (var i = 0; i < n; i++) for (var t = 0; t < d; t++) mean[t] += embeddings[i * d + t] / (double)n;
            for (var i = 0; i < n; i++)
            {
                var norm = 0.0;
                for (var t = 0; t < d; t++) { var v = embeddings[i * d + t] - mean[t]; x[i * d + t] = v; norm += v * v; }
                norm = Math.Max(Math.Sqrt(norm), 1e-9);
                for (var t = 0; t < d; t++) x[i * d + t] /= norm;
            }
            var random = new SplitMix(42);
            var g = new double[d * k];
            for (var i = 0; i < g.Length; i++) g[i] = random.Normal();
            var y = Orthonormalize(Multiply(x, n, d, g, k), n, k);
            for (var q = 0; q < 2; q++)
            {
                var z = Orthonormalize(MultiplyTransposed(x, n, d, y, k), d, k);
                y = Orthonormalize(Multiply(x, n, d, z, k), n, k);
            }
            var basis = Orthonormalize(MultiplyTransposed(x, n, d, y, k), d, k);
            var scores = Multiply(x, n, d, basis, k);
            return SimilarityPositions(Array.ConvertAll(scores, v => (float)v), n, k);
        }

        // a (rows × inner) · b (inner × columns), row-major.
        static double[] Multiply(double[] a, int rows, int inner, double[] b, int columns)
        {
            var result = new double[rows * columns];
            Parallel.For(0, rows, i =>
            {
                for (var t = 0; t < inner; t++)
                {
                    var value = a[i * inner + t];
                    if (value == 0) continue;
                    for (var j = 0; j < columns; j++) result[i * columns + j] += value * b[t * columns + j];
                }
            });
            return result;
        }
        // aᵀ (inner × rows) · b (rows × columns), row-major.
        static double[] MultiplyTransposed(double[] a, int rows, int inner, double[] b, int columns)
        {
            var result = new double[inner * columns];
            Parallel.For(0, inner, t =>
            {
                for (var i = 0; i < rows; i++)
                {
                    var value = a[i * inner + t];
                    for (var j = 0; j < columns; j++) result[t * columns + j] += value * b[i * columns + j];
                }
            });
            return result;
        }
        // Modified Gram-Schmidt on the columns of a row-major (rows × columns) matrix.
        static double[] Orthonormalize(double[] m, int rows, int columns)
        {
            for (var j = 0; j < columns; j++)
            {
                for (var p = 0; p < j; p++)
                {
                    var dot = 0.0;
                    for (var i = 0; i < rows; i++) dot += m[i * columns + p] * m[i * columns + j];
                    for (var i = 0; i < rows; i++) m[i * columns + j] -= dot * m[i * columns + p];
                }
                var norm = 0.0;
                for (var i = 0; i < rows; i++) norm += m[i * columns + j] * m[i * columns + j];
                norm = Math.Sqrt(norm);
                for (var i = 0; i < rows; i++) m[i * columns + j] = norm > 1e-12 ? m[i * columns + j] / norm : 0;
            }
            return m;
        }
    }

    public static int[][] NearestEmbeddingRows(ReadOnlySpan<float> embeddings, int rowCount, int dimensions, int k = 12)
    {
        if (rowCount <= 0 || k <= 0) return [];
        if (dimensions <= 0) throw new ArgumentOutOfRangeException(nameof(dimensions));
        var length = checked(rowCount * dimensions);
        if (embeddings.Length < length) throw new ArgumentException("The embedding buffer is smaller than rowCount x dimensions.", nameof(embeddings));
        return ExactNeighbors.Find(embeddings[..length].ToArray(), rowCount, dimensions, k, cosine: true).Indices;
    }

    public static float[] PaCMAPProjection(ReadOnlySpan<float> vectors, int rowCount, int dimensions, int outputDimensions,
        int seed, int nNeighbors = 10, double mnRatio = 0.5, double fpRatio = 2.0)
    {
        if (rowCount <= 0) return [];
        if (dimensions <= 0 || outputDimensions <= 0) throw new ArgumentOutOfRangeException(nameof(dimensions));
        if (nNeighbors < 0 || !double.IsFinite(mnRatio) || !double.IsFinite(fpRatio) || mnRatio < 0 || fpRatio < 0)
            throw new ArgumentOutOfRangeException(nameof(nNeighbors));
        if (vectors.Length < checked(rowCount * dimensions)) throw new ArgumentException("The vector buffer is smaller than rowCount × dimensions.", nameof(vectors));
        var x = new double[rowCount * dimensions];
        for (var i = 0; i < x.Length; i++) x[i] = vectors[i];
        var y = PaCMAP.Project(x, rowCount, dimensions, outputDimensions, Math.Min(nNeighbors, rowCount - 1), (ulong)seed, mnRatio, fpRatio);
        var output = new float[y.Length];
        for (var i = 0; i < y.Length; i++) output[i] = (float)y[i];
        return output;
    }

    private static class PaCMAP
    {
        public static double[] Project(double[] x0, int n, int d0, int outputDimensions, int nNeighbors, ulong seed, double mnRatio = 0.5, double fpRatio = 2.0)
        {
            var d = Math.Min(100, d0);
            var x = d0 > d ? PrincipalComponents(x0, n, d0, d) : x0;
            if (d0 <= d) d = d0;
            var mn = (int)Math.Round(nNeighbors * mnRatio, MidpointRounding.ToEven);
            var fp = Math.Min((int)Math.Round(nNeighbors * fpRatio, MidpointRounding.ToEven), n - 1 - nNeighbors);
            if (nNeighbors + mn + fp >= n)
            {
                var q = n / (1 + mnRatio + fpRatio);
                nNeighbors = Math.Min(n - 1, (int)q);
                mn = Math.Min(n - 1, (int)(q * mnRatio));
                fp = Math.Min(n - 1, (int)(q * fpRatio));
            }
            var extra = Math.Min(nNeighbors + 50, n - 1);
            var nearest = NearestRowsEuclidean(x, n, d, extra);

            var sig = new double[n];
            for (var i = 0; i < n; i++)
            {
                var s = 0.0;
                var c = 0;
                for (var a = 3; a < Math.Min(6, extra); a++) { s += nearest.Distances[i][a]; c++; }
                sig[i] = Math.Max(c == 0 ? 1e-10 : s / c, 1e-10);
            }

            var nnPairs = new List<(int, int)>(n * nNeighbors);
            for (var i = 0; i < n; i++)
            {
                var order = Enumerable.Range(0, extra).OrderBy(a =>
                {
                    var j = nearest.Indices[i][a];
                    var dist = nearest.Distances[i][a];
                    return dist * dist / sig[i] / sig[j];
                }).ThenBy(a => a);
                foreach (var a in order.Take(nNeighbors)) nnPairs.Add((i, nearest.Indices[i][a]));
            }

            var mnPairs = new List<(int, int)>(n * mn);
            var fpPairs = new List<(int, int)>(n * fp);
            for (var i = 0; i < n; i++)
            {
                var partners = new HashSet<int>();
                for (var j = 0; j < mn; j++)
                {
                    var rng = new SplitMix(seed + (ulong)(i * Math.Max(mn, 1) + j) * 0x9E3779B97F4A7C15UL);
                    var picks = new List<int>(6);
                    while (picks.Count < Math.Min(6, n - 1 - partners.Count))
                    {
                        var c = (int)(rng.Next() % (ulong)n);
                        if (c != i && !picks.Contains(c) && !partners.Contains(c)) picks.Add(c);
                    }
                    if (picks.Count == 0) continue;
                    var p = picks.OrderBy(c => Distance2(x, d, i, c)).ThenBy(c => c).Skip(Math.Min(1, picks.Count - 1)).First();
                    partners.Add(p);
                    mnPairs.Add((i, p));
                }

                var reject = new HashSet<int>();
                for (var q = i * nNeighbors; q < (i + 1) * nNeighbors; q++) reject.Add(nnPairs[q].Item2);
                var chosen = new List<int>();
                var seen = new HashSet<int>();
                var rngFp = new SplitMix((seed ^ 0xF0F0F0F0UL) + (ulong)i * 0xBF58476D1CE4E5B9UL);
                while (chosen.Count < Math.Min(fp, n - 1 - reject.Count))
                {
                    var c = (int)(rngFp.Next() % (ulong)n);
                    if (c != i && !reject.Contains(c) && seen.Add(c)) chosen.Add(c);
                }
                foreach (var c in chosen) fpPairs.Add((i, c));
            }

            var y = new double[n * outputDimensions];
            for (var i = 0; i < n; i++)
                for (var d2 = 0; d2 < Math.Min(outputDimensions, d); d2++)
                    y[i * outputDimensions + d2] = 0.01 * x[i * d + d2];

            var m = new double[y.Length];
            var v = new double[y.Length];
            var g = new double[y.Length];
            // Fixed logical partitions and ordered reduction: scheduling never changes the sum.
            var chunks = n < 256 ? 1 : 16;
            var gradients = Enumerable.Range(0, chunks).Select(_ => new double[y.Length]).ToArray();
            var near = nnPairs.ToArray();
            var mid = mnPairs.ToArray();
            var far = fpPairs.ToArray();
            for (var itr = 0; itr < 450; itr++)
            {
                var wMn = itr < 100 ? (1 - itr / 100.0) * 1000 + (itr / 100.0) * 3 : itr < 200 ? 3 : 0;
                var wNb = itr < 100 ? 2 : itr < 200 ? 3 : 1;
                Parallel.For(0, chunks, c =>
                {
                    var local = gradients[c];
                    Array.Clear(local);
                    Accumulate(y, local, outputDimensions, near, near.Length * c / chunks, near.Length * (c + 1) / chunks, wNb, 0);
                    if (wMn > 0) Accumulate(y, local, outputDimensions, mid, mid.Length * c / chunks, mid.Length * (c + 1) / chunks, wMn, 1);
                    Accumulate(y, local, outputDimensions, far, far.Length * c / chunks, far.Length * (c + 1) / chunks, 1, 2);
                });
                Array.Copy(gradients[0], g, g.Length);
                for (var c = 1; c < chunks; c++)
                {
                    var local = gradients[c];
                    var e = 0;
                    for (; e <= g.Length - Vector<double>.Count; e += Vector<double>.Count)
                        (new Vector<double>(g, e) + new Vector<double>(local, e)).CopyTo(g, e);
                    for (; e < g.Length; e++) g[e] += local[e];
                }
                var lrT = Math.Sqrt(1 - Math.Pow(0.999, itr + 1)) / (1 - Math.Pow(0.9, itr + 1));
                for (var e = 0; e < y.Length; e++)
                {
                    m[e] += 0.1 * (g[e] - m[e]);
                    v[e] += 0.001 * (g[e] * g[e] - v[e]);
                    y[e] -= lrT * m[e] / (Math.Sqrt(v[e]) + 1e-7);
                }
            }
            return y;
        }

        private static double[] PrincipalComponents(double[] x, int n, int d, int k)
        {
            return ExactNeighbors.PrincipalComponents(Array.ConvertAll(x, v => (float)v), n, d, k).Select(v => (double)v).ToArray();
        }

        private static (int[][] Indices, double[][] Distances) NearestRowsEuclidean(double[] x, int n, int d, int k)
        {
            var result = ExactNeighbors.Find(Array.ConvertAll(x, v => (float)v), n, d, k);
            return (result.Indices, result.Distances);
        }

        private static void Accumulate(double[] y, double[] g, int d, (int A, int B)[] pairs, int start, int end, double weight, int kind)
        {
            for (var p = start; p < end; p++)
            {
                var (i, j) = pairs[p];
                var dist = 1.0;
                for (var k = 0; k < d; k++)
                {
                    var e = y[i * d + k] - y[j * d + k];
                    dist += e * e;
                }
                var offset = kind == 0 ? 10 : kind == 1 ? 10000 : 1;
                var w = weight * (kind == 0 ? 20 : kind == 1 ? 20000 : -2) / ((offset + dist) * (offset + dist));
                for (var k = 0; k < d; k++)
                {
                    var e = w * (y[i * d + k] - y[j * d + k]);
                    g[i * d + k] += e; g[j * d + k] -= e;
                }
            }
        }

        private static double Distance2(double[] x, int d, int a, int b)
        {
            var s = 0.0;
            for (var j = 0; j < d; j++)
            {
                var e = x[a * d + j] - x[b * d + j];
                s += e * e;
            }
            return s;
        }

        private struct SplitMix(ulong seed)
        {
            private ulong _state = seed;
            public ulong Next()
            {
                _state += 0x9E3779B97F4A7C15UL;
                var value = _state;
                value = (value ^ (value >> 30)) * 0xBF58476D1CE4E5B9UL;
                value = (value ^ (value >> 27)) * 0x94D049BB133111EBUL;
                return value ^ (value >> 31);
            }
        }
    }
}

/// <summary>Exact bounded-memory neighbours and deterministic PCA, shared by layout and grouping.</summary>
public static class ExactNeighbors
{
    public sealed record Result(int[][] Indices, double[][] Distances);

    public static Result Find(float[] x, int n, int d, int k, bool cosine = false, CancellationToken cancellationToken = default)
    {
        Validate(x, n, d);
        if (k < 0) throw new ArgumentOutOfRangeException(nameof(k));
        k = Math.Min(k, Math.Max(0, n - 1));
        var indices = new int[n][];
        var distances = new double[n][];
        var norms = new double[n];
        Parallel.For(0, n, i =>
        {
            double norm = 0;
            for (var j = 0; j < d; j++) norm += (double)x[i * d + j] * x[i * d + j];
            norms[i] = norm;
        });
        const int rowBlock = 32, columnBlock = 128;
        var options = new ParallelOptions { CancellationToken = cancellationToken };
        Parallel.For(0, (n + rowBlock - 1) / rowBlock, options, block =>
        {
            var start = block * rowBlock;
            var count = Math.Min(rowBlock, n - start);
            var heaps = Enumerable.Range(0, count).Select(_ => new NeighborHeap(k)).ToArray();
            var scores = new float[rowBlock * columnBlock];
            for (var column = 0; column < n && k > 0; column += columnBlock)
            {
                options.CancellationToken.ThrowIfCancellationRequested();
                var width = Math.Min(columnBlock, n - column);
                GramTile(x, x, d, start, count, column, width, scores, columnBlock);
                for (var row = 0; row < count; row++)
                {
                    var i = start + row;
                    var heap = heaps[row];
                    for (var col = 0; col < width; col++)
                    {
                        var j = column + col;
                        if (i == j) continue;
                        if (cosine)
                        {
                            var approximate = -scores[row * columnBlock + col];
                            var error = (d + 16) * 2.4e-7 * (norms[i] + norms[j]);
                            if (heap.Count < k || approximate - error <= heap.Worst)
                            {
                                double exact = 0;
                                for (var coordinate = 0; coordinate < d; coordinate++)
                                    exact += (double)x[i * d + coordinate] * x[j * d + coordinate];
                                heap.Add(j, -exact);
                            }
                        }
                        else
                        {
                            var approximate = norms[i] + norms[j] - 2 * scores[row * columnBlock + col];
                            // Conservative roundoff bound; directly remeasure every possible contender,
                            // not just k+10, including duplicate/burst rows with cancellation in the Gram matrix.
                            var error = (d + 16) * 2.4e-7 * (norms[i] + norms[j]);
                            if (heap.Count < k || approximate - error <= heap.Worst)
                                heap.Add(j, SquaredDistance(x, d, i, j));
                        }
                    }
                }
            }
            for (var row = 0; row < count; row++)
            {
                var sorted = heaps[row].Sorted();
                indices[start + row] = sorted.Select(v => v.Id).ToArray();
                distances[start + row] = sorted.Select(v => cosine ? -v.Distance : Math.Sqrt(v.Distance)).ToArray();
            }
        });
        return new Result(indices, distances);
    }

    private sealed class NeighborHeap(int capacity)
    {
        private readonly (int Id, double Distance)[] _values = new (int, double)[capacity];
        public int Count { get; private set; }
        public double Worst => Count < capacity ? double.PositiveInfinity : _values[0].Distance;
        private static bool Worse((int Id, double Distance) a, (int Id, double Distance) b) =>
            a.Distance > b.Distance || (a.Distance == b.Distance && a.Id > b.Id);
        public void Add(int id, double distance)
        {
            if (capacity == 0) return;
            var value = (id, distance);
            if (Count < capacity)
            {
                var at = Count++;
                while (at > 0)
                {
                    var parent = (at - 1) / 2;
                    if (!Worse(value, _values[parent])) break;
                    _values[at] = _values[parent]; at = parent;
                }
                _values[at] = value;
            }
            else if (Worse(_values[0], value))
            {
                var at = 0;
                while (2 * at + 1 < Count)
                {
                    var child = at * 2 + 1;
                    if (child + 1 < Count && Worse(_values[child + 1], _values[child])) child++;
                    if (!Worse(_values[child], value)) break;
                    _values[at] = _values[child]; at = child;
                }
                _values[at] = value;
            }
        }
        public (int Id, double Distance)[] Sorted() =>
            _values.AsSpan(0, Count).ToArray().OrderBy(v => v.Distance).ThenBy(v => v.Id).ToArray();
    }

    public static double SquaredDistance(float[] x, int d, int a, int b)
    {
        double sum = 0;
        for (var j = 0; j < d; j++)
        {
            var delta = x[a * d + j] - x[b * d + j];
            sum += delta * delta;
        }
        return sum;
    }

    private static void Validate(float[] x, int n, int d)
    {
        ArgumentNullException.ThrowIfNull(x);
        if (n < 0 || d <= 0 || x.Length != checked(n * d)) throw new ArgumentException("Invalid matrix dimensions.");
        if (x.Any(v => !float.IsFinite(v))) throw new ArgumentException("Matrix contains non-finite values.", nameof(x));
    }

    private static void GramTile(float[] a, float[] b, int d, int start, int count, int column, int width, float[] output, int stride)
    {
        var r = 0;
        for (; r + 3 < count; r += 4)
        {
            var c = 0;
            for (; c + 3 < width; c += 4)
                Kernel4(a, b, (start + r) * d, (column + c) * d, d, output, r * stride + c, stride);
            for (; c < width; c++)
                for (var q = 0; q < 4; q++)
                    output[(r + q) * stride + c] = Dot(a.AsSpan((start + r + q) * d, d), b.AsSpan((column + c) * d, d));
        }
        for (; r < count; r++)
            for (var c = 0; c < width; c++)
                output[r * stride + c] = Dot(a.AsSpan((start + r) * d, d), b.AsSpan((column + c) * d, d));
    }

    public static float Dot(ReadOnlySpan<float> a, ReadOnlySpan<float> b)
    {
        if (a.Length != b.Length) throw new ArgumentException("Vector lengths differ.");
        var sum = Vector<float>.Zero;
        var j = 0;
        for (; j <= a.Length - Vector<float>.Count; j += Vector<float>.Count)
            sum = Vector.MultiplyAddEstimate(new Vector<float>(a.Slice(j)), new Vector<float>(b.Slice(j)), sum);
        var result = Vector.Sum(sum);
        for (; j < a.Length; j++) result += a[j] * b[j];
        return result;
    }

    [MethodImpl(MethodImplOptions.AggressiveOptimization)]
    private static void Kernel4(float[] a, float[] b, int ai, int bi, int d, float[] output, int o, int stride)
    {
        ref var ar = ref MemoryMarshal.GetArrayDataReference(a);
        ref var br = ref MemoryMarshal.GetArrayDataReference(b);
        var s00 = Vector128<float>.Zero; var s01 = s00; var s02 = s00; var s03 = s00;
        var s10 = s00; var s11 = s00; var s12 = s00; var s13 = s00;
        var s20 = s00; var s21 = s00; var s22 = s00; var s23 = s00;
        var s30 = s00; var s31 = s00; var s32 = s00; var s33 = s00;
        var j = 0;
        for (; j + 3 < d; j += 4)
        {
            var a0 = Vector128.LoadUnsafe(ref ar, (nuint)(ai + j));
            var a1 = Vector128.LoadUnsafe(ref ar, (nuint)(ai + d + j));
            var a2 = Vector128.LoadUnsafe(ref ar, (nuint)(ai + 2 * d + j));
            var a3 = Vector128.LoadUnsafe(ref ar, (nuint)(ai + 3 * d + j));
            var b0 = Vector128.LoadUnsafe(ref br, (nuint)(bi + j));
            var b1 = Vector128.LoadUnsafe(ref br, (nuint)(bi + d + j));
            var b2 = Vector128.LoadUnsafe(ref br, (nuint)(bi + 2 * d + j));
            var b3 = Vector128.LoadUnsafe(ref br, (nuint)(bi + 3 * d + j));
            s00 = Vector128.MultiplyAddEstimate(a0, b0, s00); s01 = Vector128.MultiplyAddEstimate(a0, b1, s01);
            s02 = Vector128.MultiplyAddEstimate(a0, b2, s02); s03 = Vector128.MultiplyAddEstimate(a0, b3, s03);
            s10 = Vector128.MultiplyAddEstimate(a1, b0, s10); s11 = Vector128.MultiplyAddEstimate(a1, b1, s11);
            s12 = Vector128.MultiplyAddEstimate(a1, b2, s12); s13 = Vector128.MultiplyAddEstimate(a1, b3, s13);
            s20 = Vector128.MultiplyAddEstimate(a2, b0, s20); s21 = Vector128.MultiplyAddEstimate(a2, b1, s21);
            s22 = Vector128.MultiplyAddEstimate(a2, b2, s22); s23 = Vector128.MultiplyAddEstimate(a2, b3, s23);
            s30 = Vector128.MultiplyAddEstimate(a3, b0, s30); s31 = Vector128.MultiplyAddEstimate(a3, b1, s31);
            s32 = Vector128.MultiplyAddEstimate(a3, b2, s32); s33 = Vector128.MultiplyAddEstimate(a3, b3, s33);
        }
        output[o] = Vector128.Sum(s00); output[o + 1] = Vector128.Sum(s01); output[o + 2] = Vector128.Sum(s02); output[o + 3] = Vector128.Sum(s03);
        output[o + stride] = Vector128.Sum(s10); output[o + stride + 1] = Vector128.Sum(s11); output[o + stride + 2] = Vector128.Sum(s12); output[o + stride + 3] = Vector128.Sum(s13);
        output[o + 2 * stride] = Vector128.Sum(s20); output[o + 2 * stride + 1] = Vector128.Sum(s21); output[o + 2 * stride + 2] = Vector128.Sum(s22); output[o + 2 * stride + 3] = Vector128.Sum(s23);
        output[o + 3 * stride] = Vector128.Sum(s30); output[o + 3 * stride + 1] = Vector128.Sum(s31); output[o + 3 * stride + 2] = Vector128.Sum(s32); output[o + 3 * stride + 3] = Vector128.Sum(s33);
        for (; j < d; j++)
            for (var r = 0; r < 4; r++)
                for (var c = 0; c < 4; c++) output[o + r * stride + c] += a[ai + r * d + j] * b[bi + c * d + j];
    }

    /// <summary>A * transpose(B), with row-major storage and no shared mutable reduction.</summary>
    public static float[] MultiplyTranspose(float[] a, int rows, float[] b, int columns, int dimensions)
    {
        ArgumentNullException.ThrowIfNull(a);
        ArgumentNullException.ThrowIfNull(b);
        if (rows < 0 || columns < 0 || dimensions <= 0) throw new ArgumentOutOfRangeException(nameof(dimensions));
        if (a.Length != checked(rows * dimensions) || b.Length != checked(columns * dimensions)) throw new ArgumentException("Invalid product dimensions.");
        var output = new float[checked(rows * columns)];
        Parallel.For(0, (rows + 31) / 32, block =>
        {
            var start = block * 32;
            var count = Math.Min(32, rows - start);
            var tile = new float[32 * 128];
            for (var column = 0; column < columns; column += 128)
            {
                var width = Math.Min(128, columns - column);
                GramTile(a, b, dimensions, start, count, column, width, tile, 128);
                for (var row = 0; row < count; row++)
                    Array.Copy(tile, row * 128, output, (start + row) * columns + column, width);
            }
        });
        return output;
    }

    public static float[] PrincipalComponents(float[] x, int n, int d, int k, int seed = 42)
    {
        Validate(x, n, d);
        if (k <= 0 || k > d) throw new ArgumentOutOfRangeException(nameof(k));
        if (n <= 1) return new float[n * k];
        // Keep the seed parameter for compatibility; full-covariance PCA is not randomized.
        var mean = new float[d];
        for (var i = 0; i < n; i++) for (var j = 0; j < d; j++) mean[j] += x[i * d + j];
        for (var j = 0; j < d; j++) mean[j] /= n;
        var centered = new float[x.Length];
        Parallel.For(0, n, i =>
        {
            for (var j = 0; j < d; j++) centered[i * d + j] = x[i * d + j] - mean[j];
        });
        var transposed = Transpose(centered, n, d);
        var covariance = MultiplyTranspose(transposed, d, transposed, d, n);
        if (covariance.Any(v => !float.IsFinite(v))) throw new InvalidOperationException("PCA covariance overflowed.");
        var (values, eigenvectors) = SymmetricEigensystem(Array.ConvertAll(covariance, v => (double)v), d);
        var order = Enumerable.Range(0, d).OrderByDescending(i => values[i]).ThenBy(i => i).Take(k).ToArray();
        var basis = new float[k * d];
        for (var component = 0; component < k; component++)
        {
            var largest = 0.0;
            for (var j = 0; j < d; j++)
            {
                var value = eigenvectors[j * d + order[component]];
                if (Math.Abs(value) > Math.Abs(largest)) largest = value;
            }
            for (var j = 0; j < d; j++) basis[component * d + j] = (float)(eigenvectors[j * d + order[component]] * (largest < 0 ? -1 : 1));
        }
        return MultiplyTranspose(centered, n, basis, k, d);
    }

    private static float[] Transpose(float[] x, int rows, int columns)
    {
        var output = new float[x.Length];
        Parallel.For(0, (rows + 63) / 64, block =>
        {
            for (var c = 0; c < columns; c += 64)
                for (var i = block * 64; i < Math.Min(rows, block * 64 + 64); i++)
                    for (var j = c; j < Math.Min(columns, c + 64); j++) output[j * rows + i] = x[i * columns + j];
        });
        return output;
    }

    private static (double[] Values, double[] Vectors) SymmetricEigensystem(double[] matrix, int n)
    {
        var q = new double[n * n];
        for (var i = 0; i < n; i++) q[i * n + i] = 1;
        // Orthogonal Householder similarity transformations reduce the complete matrix,
        // followed by converged implicit QL; no truncated subspace or iteration approximation.
        for (var column = 0; column < n - 2; column++)
        {
            var start = column + 1;
            var length = n - start;
            var v = new double[length];
            double norm = 0;
            for (var i = 0; i < length; i++)
            {
                v[i] = matrix[(start + i) * n + column];
                norm += v[i] * v[i];
            }
            norm = Math.Sqrt(norm);
            if (norm == 0) continue;
            var alpha = -Math.CopySign(norm, v[0]);
            v[0] -= alpha;
            var scale = Math.Sqrt(v.Sum(value => value * value));
            for (var i = 0; i < length; i++) v[i] /= scale;
            var w = new double[length];
            for (var i = 0; i < length; i++)
            {
                double dot = 0;
                var row = (start + i) * n + start;
                for (var j = 0; j < length; j++) dot += matrix[row + j] * v[j];
                w[i] = 2 * dot;
            }
            double beta = 0;
            for (var i = 0; i < length; i++) beta += v[i] * w[i];
            for (var i = 0; i < length; i++) w[i] -= beta * v[i];
            for (var i = 0; i < length; i++)
                for (var j = 0; j <= i; j++)
                {
                    var value = matrix[(start + i) * n + start + j] - v[i] * w[j] - w[i] * v[j];
                    matrix[(start + i) * n + start + j] = matrix[(start + j) * n + start + i] = value;
                }
            for (var i = 0; i < n; i++)
            {
                double dot = 0;
                var row = i * n + start;
                for (var j = 0; j < length; j++) dot += q[row + j] * v[j];
                for (var j = 0; j < length; j++) q[row + j] -= 2 * dot * v[j];
            }
            matrix[start * n + column] = matrix[column * n + start] = alpha;
            for (var i = start + 1; i < n; i++) matrix[i * n + column] = matrix[column * n + i] = 0;
        }
        var diagonal = new double[n];
        var off = new double[n];
        for (var i = 0; i < n; i++)
        {
            diagonal[i] = matrix[i * n + i];
            if (i + 1 < n) off[i] = matrix[(i + 1) * n + i];
        }
        const double epsilon = 2.2204460492503131e-16;
        for (var left = 0; left < n; left++)
        {
            var iterations = 0;
            while (true)
            {
                var right = left;
                while (right < n - 1 && Math.Abs(off[right]) > epsilon * (Math.Abs(diagonal[right]) + Math.Abs(diagonal[right + 1]))) right++;
                if (right == left) break;
                if (++iterations > 128) throw new InvalidOperationException("PCA symmetric eigensolver did not converge.");
                var g = (diagonal[left + 1] - diagonal[left]) / (2 * off[left]);
                var norm = double.Hypot(g, 1);
                g = diagonal[right] - diagonal[left] + off[left] / (g + Math.CopySign(norm, g));
                double sine = 1, cosine = 1, shift = 0;
                var split = false;
                for (var i = right - 1; i >= left; i--)
                {
                    var f = sine * off[i];
                    var b = cosine * off[i];
                    norm = double.Hypot(f, g);
                    off[i + 1] = norm;
                    if (norm == 0)
                    {
                        diagonal[i + 1] -= shift;
                        off[right] = 0; split = true; break;
                    }
                    sine = f / norm; cosine = g / norm;
                    g = diagonal[i + 1] - shift;
                    var correction = (diagonal[i] - g) * sine + 2 * cosine * b;
                    shift = sine * correction;
                    diagonal[i + 1] = g + shift;
                    g = cosine * correction - b;
                    for (var row = 0; row < n; row++)
                    {
                        var a = q[row * n + i]; var c = q[row * n + i + 1];
                        q[row * n + i + 1] = sine * a + cosine * c;
                        q[row * n + i] = cosine * a - sine * c;
                    }
                }
                if (split) continue;
                diagonal[left] -= shift;
                off[left] = g; off[right] = 0;
            }
        }
        if (diagonal.Any(value => !double.IsFinite(value))) throw new InvalidOperationException("PCA eigenvalues are non-finite.");
        return (diagonal, q);
    }
}
