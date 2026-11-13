using System.Numerics;
using System.Text;

namespace Nebula.Core;

public sealed record Photo
{
    public required string Id { get; init; }
    public required string OriginalPath { get; init; }
    public required string ThumbnailPath { get; init; }
    public required string Title { get; init; }
    public string Artist { get; init; } = "";
    public string Department { get; init; } = "";
    public string Medium { get; init; } = "";
    public string ArtworkDate { get; init; } = "";
    public string Credit { get; init; } = "";
    public string SourceUrl { get; init; } = "";
    public int Width { get; init; }
    public int Height { get; init; }
    public long SourceBytes { get; init; }
    public long SourceWriteUtcTicks { get; init; }
    public string MetadataFingerprint { get; init; } = "";
    public DateTimeOffset? TakenAt { get; init; }
    public float Red { get; init; }
    public float Green { get; init; }
    public float Blue { get; init; }
    public float? LabLightness { get; init; }
    public float? LabA { get; init; }
    public float? LabB { get; init; }
    public float Aspect => Height > 0 ? (float)Width / Height : 1;
    public string Dimensions => $"{Width:N0} × {Height:N0}";
    public string Attribution => string.Join(" · ", new[] { Readable(Artist), Readable(ArtworkDate) }.Where(s => s.Length > 0));
    public override string ToString() => string.IsNullOrWhiteSpace(Attribution) ? Title : $"{Title}, {Attribution}";

    /// Commons wikitext can leak Wikidata machine dates ("1875 date QS:P571,+1875-00-00T00:00:00Z/9")
    /// and padded separators; show only the human part.
    public static string Readable(string text)
    {
        if (string.IsNullOrWhiteSpace(text)) return "";
        var clean = System.Text.RegularExpressions.Regex.Replace(text, @"\s*\bdate\s+QS:P\d+\s*,\s*\S*", "");
        clean = System.Text.RegularExpressions.Regex.Replace(clean, @"\s+", " ");
        clean = System.Text.RegularExpressions.Regex.Replace(clean, @"\s*/\s*", " / ");
        return clean.Trim().TrimEnd(',', ';', '·').Trim();
    }
}

public sealed record Library(string Source, List<Photo> Photos, int Skipped, List<string> Errors);
/// Similarity is last so the 1–4 keys keep Cloud/Colour/Time/Grid; its positions come from image
/// embeddings (Layout.SimilarityPositions), not from photo metadata.
public enum Arrangement { Cloud, Colour, Time, Grid, Similarity }

public static class PhotoSearch
{
    public static string Normalize(string value)
    {
        var result = new StringBuilder();
        foreach (var character in value.Normalize(NormalizationForm.FormD))
            if (System.Globalization.CharUnicodeInfo.GetUnicodeCategory(character) !=
                System.Globalization.UnicodeCategory.NonSpacingMark)
                result.Append(char.ToLowerInvariant(character));
        return result.ToString().Normalize(NormalizationForm.FormC);
    }

    public static bool Matches(Photo photo, string query)
    {
        var terms = Normalize(query).Split(' ', StringSplitOptions.RemoveEmptyEntries);
        if (terms.Length == 0) return true;
        var text = Normalize(string.Join(" ", photo.Title, photo.Artist, photo.Department,
            photo.Medium, photo.ArtworkDate, Path.GetFileName(photo.OriginalPath)));
        return terms.All(text.Contains);
    }
}

public static partial class Layout
{
    public static Vector3[] Positions(IReadOnlyList<Photo> photos, Arrangement arrangement)
    {
        var count = photos.Count;
        if (count == 0) return [];
        var points = new Point[count];
        var timestamps = photos.Select(p => p.TakenAt?.ToUnixTimeSeconds() ?? 0L).ToArray();
        var byTime = Enumerable.Range(0, count).OrderBy(i => timestamps[i]).ToArray();
        var spread = Math.Max(1, Math.Cbrt(count / 1000.0));
        var random = new SplitMix(7);
        switch (arrangement)
        {
            case Arrangement.Grid:
                var columns = (int)Math.Ceiling(Math.Sqrt(count));
                for (var rank = 0; rank < count; rank++)
                    points[byTime[rank]] = new((rank % columns - columns / 2.0) * 64,
                        (columns / 2.0 - rank / columns) * 64, 0);
                break;
            case Arrangement.Time:
                random = new SplitMix(0);
                var first = timestamps.Min();
                var last = timestamps.Max();
                var years = (last - first) / (365.25 * 86400);
                for (var i = 0; i < count; i++)
                {
                    var fraction = (timestamps[i] - first) / (double)Math.Max(last - first, 1);
                    var angle = fraction * years * Math.Tau;
                    points[i] = new(Math.Cos(angle) * 700 * spread + random.Normal() * 18,
                        (fraction * 1600 - 800) * spread + random.Normal() * 18,
                        Math.Sin(angle) * 700 * spread + random.Normal() * 18);
                }
                break;
            case Arrangement.Colour:
                var labs = photos.Select(photo => photo.LabLightness is { } light && photo.LabA is { } a && photo.LabB is { } b
                    ? new Vector3(light, a, b) : ToOklab(photo.Red, photo.Green, photo.Blue)).ToArray();
                var meanLightness = labs.Average(lab => (double)lab.X);
                var chroma = labs.Select(lab => Math.Sqrt(lab.Y * lab.Y + lab.Z * lab.Z)).ToArray();
                var order = Enumerable.Range(0, count).OrderBy(i => chroma[i]).ToArray();
                var ranks = new int[count];
                for (var rank = 0; rank < count; rank++) ranks[order[rank]] = rank;
                random = new SplitMix(3);
                for (var i = 0; i < count; i++)
                {
                    var jitter = -Math.PI + Math.Tau * random.Uniform();
                    var weight = Math.Clamp(chroma[i] / 0.02, 0, 1);
                    var hue = Math.Atan2(labs[i].Z, labs[i].Y);
                    hue = Math.Atan2(weight * Math.Sin(hue) + (1 - weight) * Math.Sin(jitter),
                        weight * Math.Cos(hue) + (1 - weight) * Math.Cos(jitter));
                    var radius = 40 + 960 * Math.Sqrt(ranks[i] / (double)Math.Max(count - 1, 1));
                    points[i] = new Point(radius * Math.Cos(hue), (labs[i].X - meanLightness) * 2400, radius * Math.Sin(hue)) * spread;
                }
                break;
            default:
                for (var rank = 0; rank < count; rank++)
                {
                    var drift = -1 + 2 * (rank + 0.5) / count;
                    points[byTime[rank]] = new Point(drift * 420 + random.Normal() * 200,
                        random.Normal() * 215, random.Normal() * 235) * spread;
                }
                break;
        }
        if (arrangement != Arrangement.Grid && count > 1) Space(points);
        // Layouts are built at 32 units per resting tile width; the renderer uses one.
        return points.Select(p => new Vector3((float)(p.X / 32), (float)(p.Y / 32), (float)(p.Z / 32))).ToArray();
    }

    /// <summary>
    /// Search matches in Time: the dated matches on a helix in date order, oldest at the bottom like
    /// the library helix, evenly spaced so photos taken on the same day don't knot together; the
    /// undated ones as a compact cloud just below its start (where the library helix puts them).
    /// </summary>
    public static Vector3[] TimeSearchPositions(IReadOnlyList<Photo> photos)
    {
        var dated = Enumerable.Range(0, photos.Count).Where(i => photos[i].TakenAt.HasValue)
            .OrderBy(i => photos[i].TakenAt!.Value).ThenBy(i => i).ToArray();
        var undated = Enumerable.Range(0, photos.Count).Where(i => !photos[i].TakenAt.HasValue).ToArray();
        if (dated.Length == 0) return Positions(photos, Arrangement.Cloud);
        var result = new Vector3[photos.Count];
        // 2.2 tiles of arc between neighbours, 2.6 tiles between turns; wider than tall on screen.
        var perTurn = Math.Clamp((int)Math.Round(Math.Sqrt(dated.Length) * 2.5), 12, 90);
        var radius = perTurn * 2.2f / MathF.Tau;
        var turns = (dated.Length + perTurn - 1) / perTurn;
        for (var rank = 0; rank < dated.Length; rank++)
        {
            var angle = rank * MathF.Tau / perTurn;
            result[dated[rank]] = new(MathF.Cos(angle) * radius, (rank / (float)perTurn - turns / 2f) * 2.6f, MathF.Sin(angle) * radius);
        }
        if (undated.Length == 0) return result;
        var knot = Positions(undated.Select(i => photos[i]).ToArray(), Arrangement.Cloud);
        var knotCentre = knot.Aggregate(Vector3.Zero, (a, p) => a + p) / knot.Length;
        var drop = dated.Min(i => result[i].Y) - knot.Max(p => p.Y) - 3;
        for (var k = 0; k < undated.Length; k++)
            result[undated[k]] = knot[k] - new Vector3(knotCentre.X, 0, knotCentre.Z) + new Vector3(0, drop, 0);
        return result;
    }

    /// <summary>
    /// A subset of an existing arrangement (e.g. search matches in Similarity) re-spaced as its own
    /// cloud: same relative shape, centred on the origin, with the library layouts' tile spacing.
    /// </summary>
    public static Vector3[] Regather(IReadOnlyList<Vector3> positions)
    {
        var count = positions.Count;
        if (count == 0) return [];
        var centre = Vector3.Zero;
        foreach (var p in positions) centre += p / count;
        var points = positions.Select(p => new Point((p.X - centre.X) * 32.0, (p.Y - centre.Y) * 32.0, (p.Z - centre.Z) * 32.0)).ToArray();
        if (count > 1) Space(points);
        return points.Select(p => new Vector3((float)(p.X / 32), (float)(p.Y / 32), (float)(p.Z / 32))).ToArray();
    }

    /// <summary>
    /// Ranked items as a flat, camera-facing grid: first item top-left, reading order, the library
    /// Grid's 64-unit pitch. <paramref name="depth"/> lifts the sheet off the library plane.
    /// </summary>
    public static Vector3[] RankedGrid(int count, float depth = 0)
    {
        if (count <= 0) return [];
        var columns = (int)Math.Ceiling(Math.Sqrt(count));
        var rows = (count + columns - 1) / columns;
        var result = new Vector3[count];
        for (var rank = 0; rank < count; rank++)
            result[rank] = new((float)((rank % columns - (columns - 1) / 2.0) * 2), (float)(((rows - 1) / 2.0 - rank / columns) * 2), depth);
        return result;
    }

    // Normalise to a 64-unit median nearest-neighbour spacing, then push overlapping tiles apart.
    private static void Space(Point[] points)
    {
        var count = points.Length;
        var nearest = count >= 20_000 ? NearestSample(points, 4096) : Nearest(points);
        Array.Sort(nearest);
        var median = count >= 20_000
            ? nearest[nearest.Length / 2]
            : count % 2 == 1 ? nearest[count / 2] : (nearest[count / 2 - 1] + nearest[count / 2]) / 2;
        if (median > 1e-6)
            for (var i = 0; i < count; i++) points[i] *= 64 / median;
        if (count >= 20_000)
            Relax(points, 51.2, iterations: count >= 80_000 ? 2 : 8, cap: 8.0);
        else
            Relax(points, 51.2);
    }

    private readonly record struct Point(double X, double Y, double Z)
    {
        public double Length => Math.Sqrt(X * X + Y * Y + Z * Z);
        public static Point operator +(Point a, Point b) => new(a.X + b.X, a.Y + b.Y, a.Z + b.Z);
        public static Point operator -(Point a, Point b) => new(a.X - b.X, a.Y - b.Y, a.Z - b.Z);
        public static Point operator *(Point a, double b) => new(a.X * b, a.Y * b, a.Z * b);
    }

    private struct SplitMix(ulong seed)
    {
        private ulong _state = seed;
        public double Uniform()
        {
            _state = unchecked(_state + 0x9E3779B97F4A7C15UL);
            var value = _state;
            value = unchecked((value ^ (value >> 30)) * 0xBF58476D1CE4E5B9UL);
            value = unchecked((value ^ (value >> 27)) * 0x94D049BB133111EBUL);
            value ^= value >> 31;
            return (value >> 11) / 9007199254740992.0;
        }
        public double Normal() => Math.Sqrt(-2 * Math.Log(Math.Max(Uniform(), 1e-300))) * Math.Cos(Math.Tau * Uniform());
    }

    private static (int X, int Y, int Z) Cell(Point p, double size) =>
        ((int)Math.Floor(p.X / size), (int)Math.Floor(p.Y / size), (int)Math.Floor(p.Z / size));

    private static Dictionary<(int, int, int), List<int>> Cells(Point[] points, double size)
    {
        var cells = new Dictionary<(int, int, int), List<int>>();
        for (var i = 0; i < points.Length; i++)
        {
            var cell = Cell(points[i], size);
            if (!cells.TryGetValue(cell, out var members)) cells[cell] = members = [];
            members.Add(i);
        }
        return cells;
    }

    private static double[] Nearest(Point[] points)
    {
        var extent = Math.Max(points.Max(p => p.X) - points.Min(p => p.X),
            Math.Max(points.Max(p => p.Y) - points.Min(p => p.Y), points.Max(p => p.Z) - points.Min(p => p.Z)));
        var size = Math.Max(extent / Math.Cbrt(points.Length), 1e-9);
        var cells = Cells(points, size);
        var distances = new double[points.Length];
        for (var i = 0; i < points.Length; i++)
        {
            var cell = Cell(points[i], size);
            var best = double.PositiveInfinity;
            for (var radius = 0; radius < 64; radius++)
            {
                for (var x = -radius; x <= radius; x++)
                    for (var y = -radius; y <= radius; y++)
                        for (var z = -radius; z <= radius; z++)
                        {
                            if (Math.Max(Math.Abs(x), Math.Max(Math.Abs(y), Math.Abs(z))) != radius) continue;
                            if (!cells.TryGetValue((cell.X + x, cell.Y + y, cell.Z + z), out var members)) continue;
                            foreach (var j in members)
                                if (i != j) best = Math.Min(best, (points[i] - points[j]).Length);
                        }
                if (best <= radius * size) break;
            }
            distances[i] = double.IsFinite(best) ? best : 0;
        }
        return distances;
    }

    /// Nearest-neighbour distance for an evenly spaced sample of photos, each measured against the
    /// WHOLE set (not against the other samples, which are sparser and would overstate the spacing
    /// by ~cbrt(N/sample) — at 50K that packed every layout 2.3× too tight).
    private static double[] NearestSample(Point[] points, int sampleCount)
    {
        sampleCount = Math.Min(sampleCount, points.Length);
        var extent = Math.Max(points.Max(p => p.X) - points.Min(p => p.X),
            Math.Max(points.Max(p => p.Y) - points.Min(p => p.Y), points.Max(p => p.Z) - points.Min(p => p.Z)));
        var size = Math.Max(extent / Math.Cbrt(points.Length), 1e-9);
        var cells = Cells(points, size);
        var distances = new double[sampleCount];
        Parallel.For(0, sampleCount, s =>
        {
            var i = (int)((long)s * points.Length / sampleCount);
            var cell = Cell(points[i], size);
            var best = double.PositiveInfinity;
            for (var radius = 0; radius < 64; radius++)
            {
                for (var x = -radius; x <= radius; x++)
                    for (var y = -radius; y <= radius; y++)
                        for (var z = -radius; z <= radius; z++)
                        {
                            if (Math.Max(Math.Abs(x), Math.Max(Math.Abs(y), Math.Abs(z))) != radius) continue;
                            if (!cells.TryGetValue((cell.X + x, cell.Y + y, cell.Z + z), out var members)) continue;
                            foreach (var j in members)
                                if (i != j) best = Math.Min(best, (points[i] - points[j]).Length);
                        }
                if (best <= radius * size) break;
            }
            distances[s] = double.IsFinite(best) ? best : 0;
        });
        return distances;
    }

    private static void Relax(Point[] points, double minimum, int iterations = 400, double cap = 4.8)
    {
        var displacement = new Point[points.Length];
        for (var iteration = 0; iteration < iterations; iteration++)
        {
            var cells = Cells(points, minimum);
            Array.Clear(displacement);
            var any = false;
            for (var i = 0; i < points.Length; i++)
            {
                var cell = Cell(points[i], minimum);
                for (var x = -1; x <= 1; x++)
                    for (var y = -1; y <= 1; y++)
                        for (var z = -1; z <= 1; z++)
                        {
                            if (!cells.TryGetValue((cell.X + x, cell.Y + y, cell.Z + z), out var members)) continue;
                            foreach (var j in members)
                            {
                                if (j <= i) continue;
                                var delta = points[i] - points[j];
                                var distance = delta.Length + 1e-6;
                                if (distance - 1e-6 >= minimum) continue;
                                var push = delta * ((minimum - distance) * 0.45 / distance);
                                displacement[i] += push;
                                displacement[j] -= push;
                                any = true;
                            }
                        }
            }
            if (!any) break;
            for (var i = 0; i < points.Length; i++)
                points[i] += displacement[i] * Math.Min(1, cap / Math.Max(displacement[i].Length, 1e-9));
            if ((iteration + 1) % 20 == 0 && Nearest(points).Count(d => d < 0.95 * minimum) / (double)points.Length < 0.005)
                break;
        }
    }

    public static Vector3 ToOklab(float red, float green, float blue)
    {
        static float Linear(float c) => c <= 0.04045f ? c / 12.92f : MathF.Pow((c + 0.055f) / 1.055f, 2.4f);
        var r = Linear(red); var g = Linear(green); var b = Linear(blue);
        var l = MathF.Cbrt(0.4122214708f * r + 0.5363325363f * g + 0.0514459929f * b);
        var m = MathF.Cbrt(0.2119034982f * r + 0.6806995451f * g + 0.1073969566f * b);
        var s = MathF.Cbrt(0.0883024619f * r + 0.2817188376f * g + 0.6299787005f * b);
        return new(0.2104542553f * l + 0.793617785f * m - 0.0040720468f * s,
            1.9779984951f * l - 2.428592205f * m + 0.4505937099f * s,
            0.0259040371f * l + 0.7827717662f * m - 0.808675766f * s);
    }
}
