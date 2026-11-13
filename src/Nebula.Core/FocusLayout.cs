using System.Numerics;

namespace Nebula.Core;

public static partial class Layout
{
    /// <summary>
    /// Search results as a volume: X and Y are the matches' own semantic
    /// structure (their first two principal axes), Z blends the third axis with match confidence,
    /// strongest nearest; then a 48-unit grid pushes overlapping tiles apart. Returned in tile widths
    /// (layout units / 32), like
    /// <see cref="Positions"/>. <paramref name="rows"/> are the matches' image embeddings.
    /// </summary>
    public static Vector3[] FocusPositions(IReadOnlyList<float[]> rows, IReadOnlyList<float> scores,
        float blend = 0.55f, float radius0 = 430, float depth0 = 1100, int maxFitRows = 2048)
    {
        var n = rows.Count;
        if (n == 0) return [];
        if (n == 1) return [Vector3.Zero];
        var d = rows[0].Length;
        var mean = new float[d];
        foreach (var row in rows) for (var j = 0; j < d; j++) mean[j] += row[j] / n;
        var x = new float[n][];
        for (var i = 0; i < n; i++)
        {
            x[i] = new float[d];
            for (var j = 0; j < d; j++) x[i][j] = rows[i][j] - mean[j];
        }
        // Three principal axes by power iteration with deflation; the start vector is the
        // deterministic sin(j·(k+7))·12.9898. Every match is kept, so for large searches the
        // axes come from an even sample and every match is projected onto them.
        var fit = n <= maxFitRows ? x : Enumerable.Range(0, maxFitRows).Select(s => x[(int)((long)s * n / maxFitRows)]).ToArray();
        var p = new float[n, 3];
        var components = new List<float[]>();
        for (var k = 0; k < Math.Min(3, Math.Min(n, d)); k++)
        {
            var v = new float[d];
            for (var j = 0; j < d; j++) v[j] = (float)Math.Sin(j * (k + 7) * 12.9898);
            Normalize(v);
            for (var iteration = 0; iteration < 40; iteration++)
            {
                var w = new float[d];
                foreach (var row in fit)
                {
                    var s = Dot(row, v);
                    for (var j = 0; j < d; j++) w[j] += row[j] * s;
                }
                foreach (var c in components)
                {
                    var s = Dot(w, c);
                    for (var j = 0; j < d; j++) w[j] -= s * c[j];
                }
                Normalize(w);
                v = w;
            }
            components.Add(v);
            for (var i = 0; i < n; i++) p[i, k] = Dot(x[i], v);
        }
        for (var a = 0; a < 3; a++)
        {
            var m = 0f;
            for (var i = 0; i < n; i++) m += p[i, a] / n;
            for (var i = 0; i < n; i++) p[i, a] -= m;
        }
        var magnitudes = new float[n * 3];
        for (var i = 0; i < n; i++) for (var a = 0; a < 3; a++) magnitudes[i * 3 + a] = Math.Abs(p[i, a]);
        var sc = Percentile(magnitudes, 95);
        if (sc == 0) sc = 1;
        float s0 = scores.Min(), s1 = scores.Max();
        var radius = radius0;
        var depth = depth0;
        var k0 = Math.Min(1, Math.Max(0.35f, MathF.Sqrt(n / 80f)));
        radius *= k0; depth *= k0;
        if (n > 300) { var g = MathF.Cbrt(n / 300f); radius *= g; depth *= g; }
        var outp = new Vector3[n];
        for (var i = 0; i < n; i++)
        {
            var c = (scores[i] - s0) / Math.Max(s1 - s0, 1e-9f);
            var v = new Vector3(p[i, 0] / sc * radius, p[i, 1] / sc * radius * 0.72f,
                blend * p[i, 2] / sc * (depth * 0.45f) + (1 - blend) * (c - 0.5f) * depth);
            outp[i] = float.IsFinite(v.X) && float.IsFinite(v.Y) && float.IsFinite(v.Z) ? v : Vector3.Zero;
        }
        // De-overlap on a grid of 48-unit cells, 40 sweeps.
        for (var sweep = 0; sweep < 40; sweep++)
        {
            var grid = new Dictionary<(int, int, int), List<int>>();
            for (var i = 0; i < n; i++)
            {
                var key = ((int)MathF.Floor(outp[i].X / 48), (int)MathF.Floor(outp[i].Y / 48), (int)MathF.Floor(outp[i].Z / 48));
                if (!grid.TryGetValue(key, out var list)) grid[key] = list = [];
                list.Add(i);
            }
            var moved = false;
            var push = new Vector3[n];
            foreach (var ((cx, cy, cz), members) in grid)
                for (var dx = -1; dx <= 1; dx++)
                    for (var dy = -1; dy <= 1; dy++)
                        for (var dz = -1; dz <= 1; dz++)
                        {
                            if (!grid.TryGetValue((cx + dx, cy + dy, cz + dz), out var other)) continue;
                            foreach (var i in members)
                                foreach (var j in other)
                                {
                                    if (j <= i) continue;
                                    var dv = outp[i] - outp[j];
                                    var dist = dv.Length() + 1e-6f;
                                    if (dist >= 48) continue;
                                    moved = true;
                                    var f = (48 - dist) * 0.5f / dist;
                                    push[i] += dv * f;
                                    push[j] -= dv * f;
                                }
                        }
            if (!moved) break;
            for (var i = 0; i < n; i++) outp[i] += push[i];
        }
        for (var i = 0; i < n; i++) outp[i] /= 32;
        return outp;

        static float Dot(float[] a, float[] b) { var s = 0f; for (var j = 0; j < a.Length; j++) s += a[j] * b[j]; return s; }
        static void Normalize(float[] v) { var n2 = MathF.Max(MathF.Sqrt(Dot(v, v)), 1e-9f); for (var j = 0; j < v.Length; j++) v[j] /= n2; }
        static float Percentile(float[] values, double q)
        {
            if (values.Length == 0) return 0;
            var s = (float[])values.Clone();
            Array.Sort(s);
            var pos = (s.Length - 1) * q / 100;
            var lo = (int)Math.Floor(pos);
            var hi = Math.Min(lo + 1, s.Length - 1);
            return s[lo] + (float)(pos - lo) * (s[hi] - s[lo]);
        }
    }
}
