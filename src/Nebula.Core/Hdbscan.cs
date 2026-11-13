namespace Nebula.Core;

public static class Hdbscan
{
    public static int[] LeafClusters(ReadOnlySpan<float> points, int rowCount, int dimensions, int minClusterSize = 10, int minSamples = 2)
    {
        if (rowCount <= 0) return [];
        if (rowCount == 1) return [-1];
        if (dimensions <= 0 || minClusterSize < 2 || minSamples < 1) throw new ArgumentOutOfRangeException(nameof(dimensions));
        if (points.Length < checked(rowCount * dimensions)) throw new ArgumentException("Point buffer is smaller than rowCount x dimensions.", nameof(points));

        var data = points[..checked(rowCount * dimensions)].ToArray();
        if (data.Any(v => !float.IsFinite(v))) throw new ArgumentException("Points contain non-finite values.", nameof(points));
        var kth = Math.Min(Math.Max(minSamples - 1, 0), rowCount - 1);
        var core = new double[rowCount];
        if (kth > 0)
        {
            var nearest = ExactNeighbors.Find(data, rowCount, dimensions, kth);
            for (var i = 0; i < rowCount; i++)
                core[i] = nearest.Distances[i][kth - 1];
        }
        var edges = new ReachabilityTree(data, rowCount, dimensions, core).MinimumSpanningTree();

        var order = edges.Select((edge, index) => (edge, index))
            .OrderBy(x => x.edge.Distance)
            .ThenBy(x => x.index)
            .ToArray();
        var maxNodes = Math.Max(1, 2 * rowCount - 1);
        var parent = Enumerable.Range(0, maxNodes).ToArray();
        var size = Enumerable.Repeat(1, maxNodes).ToArray();
        var nextNode = rowCount;
        var link = new List<(int Left, int Right, double Distance, int Size)>(rowCount - 1);

        int Find(int a)
        {
            var r = a;
            while (parent[r] != r) r = parent[r];
            while (parent[a] != r)
            {
                var p = parent[a];
                parent[a] = r;
                a = p;
            }
            return r;
        }

        foreach (var item in order)
        {
            var edge = item.edge;
            var ra = Find(edge.A);
            var rb = Find(edge.B);
            if (ra == rb) continue;
            link.Add((ra, rb, edge.Distance, size[ra] + size[rb]));
            parent[ra] = nextNode;
            parent[rb] = nextNode;
            size[nextNode] = size[ra] + size[rb];
            nextNode++;
        }
        if (link.Count == 0) return Enumerable.Repeat(-1, rowCount).ToArray();

        var root = rowCount + link.Count - 1;
        int Count(int n) => n >= rowCount ? link[n - rowCount].Size : 1;
        IEnumerable<int> Bfs(int start)
        {
            var level = new List<int> { start };
            while (level.Count > 0)
            {
                foreach (var n in level) yield return n;
                level = level.Where(n => n >= rowCount)
                    .SelectMany(n => new[] { link[n - rowCount].Left, link[n - rowCount].Right })
                    .ToList();
            }
        }

        var relabel = new int[root + 1];
        var ignore = new bool[root + 1];
        var nextLabel = rowCount + 1;
        relabel[root] = rowCount;
        var tree = new List<(int Parent, int Child, int Size)>();

        foreach (var n in Bfs(root).ToArray())
        {
            if (ignore[n] || n < rowCount) continue;
            var (left, right, _, _) = link[n - rowCount];
            var lc = Count(left);
            var rc = Count(right);

            void FallOut(int sub)
            {
                foreach (var s in Bfs(sub))
                {
                    if (s < rowCount) tree.Add((relabel[n], s, 1));
                    if (s < ignore.Length) ignore[s] = true;
                }
            }

            if (lc >= minClusterSize && rc >= minClusterSize)
            {
                relabel[left] = nextLabel++;
                tree.Add((relabel[n], relabel[left], lc));
                relabel[right] = nextLabel++;
                tree.Add((relabel[n], relabel[right], rc));
            }
            else if (lc < minClusterSize && rc < minClusterSize)
            {
                FallOut(left);
                FallOut(right);
            }
            else if (lc < minClusterSize)
            {
                relabel[right] = relabel[n];
                FallOut(left);
            }
            else
            {
                relabel[left] = relabel[n];
                FallOut(right);
            }
        }

        var clusterRows = tree.Where(t => t.Size > 1).ToArray();
        var leaves = new List<int>();
        if (clusterRows.Length > 0)
        {
            var children = new Dictionary<int, List<int>>();
            foreach (var row in clusterRows)
            {
                if (!children.TryGetValue(row.Parent, out var list)) children[row.Parent] = list = [];
                list.Add(row.Child);
            }
            var stack = new Stack<int>();
            stack.Push(rowCount);
            while (stack.TryPop(out var c))
            {
                if (children.TryGetValue(c, out var kids))
                    foreach (var kid in kids) stack.Push(kid);
                else
                    leaves.Add(c);
            }
        }

        var label = leaves.OrderBy(x => x).Select((leaf, i) => (leaf, i)).ToDictionary(x => x.leaf, x => x.i);
        var output = Enumerable.Repeat(-1, rowCount).ToArray();
        foreach (var row in tree.Where(t => t.Size == 1))
            output[row.Child] = label.TryGetValue(row.Parent, out var l) ? l : -1;
        return output;
    }

    private static double Euclidean(ReadOnlySpan<float> x, int d, int a, int b)
    {
        var sum = 0.0;
        var ao = a * d;
        var bo = b * d;
        for (var i = 0; i < d; i++)
        {
            var delta = x[ao + i] - x[bo + i];
            sum += delta * delta;
        }
        return Math.Sqrt(sum);
    }

    // Exact Boruvka: every component's cheapest outgoing edge is searched against the
    // full kd-tree, not just a connected kNN graph (which need not contain the exact MST).
    private sealed class ReachabilityTree
    {
        private sealed record Node(int Start, int Count, float[] Low, float[] High, double MinCore)
        {
            public int Left = -1, Right = -1;
        }
        private readonly float[] _points;
        private readonly int _n, _d;
        private readonly double[] _core;
        private readonly int[] _order;
        private readonly List<Node> _nodes = [];
        public ReachabilityTree(float[] points, int n, int d, double[] core)
        {
            _points = points; _n = n; _d = d; _core = core;
            _order = Enumerable.Range(0, n).ToArray();
            Build(0, n);
        }
        private int Build(int start, int count)
        {
            var lo = Enumerable.Repeat(float.PositiveInfinity, _d).ToArray();
            var hi = Enumerable.Repeat(float.NegativeInfinity, _d).ToArray();
            var minCore = double.PositiveInfinity;
            for (var p = start; p < start + count; p++)
            {
                var i = _order[p]; minCore = Math.Min(minCore, _core[i]);
                for (var j = 0; j < _d; j++)
                {
                    lo[j] = Math.Min(lo[j], _points[i * _d + j]);
                    hi[j] = Math.Max(hi[j], _points[i * _d + j]);
                }
            }
            var node = new Node(start, count, lo, hi, minCore);
            var index = _nodes.Count; _nodes.Add(node);
            if (count > 16)
            {
                var axis = 0;
                for (var j = 1; j < _d; j++) if (hi[j] - lo[j] > hi[axis] - lo[axis]) axis = j;
                Array.Sort(_order, start, count, Comparer<int>.Create((a, b) =>
                {
                    var comparison = _points[a * _d + axis].CompareTo(_points[b * _d + axis]);
                    return comparison == 0 ? a.CompareTo(b) : comparison;
                }));
                node.Left = Build(start, count / 2);
                node.Right = Build(start + count / 2, count - count / 2);
            }
            return index;
        }
        public List<(int A, int B, double Distance)> MinimumSpanningTree()
        {
            var parent = Enumerable.Range(0, _n).ToArray();
            var components = new int[_n];
            var uniform = new int[_nodes.Count];
            var candidates = new (int A, int B, double Distance)[_n];
            var result = new List<(int A, int B, double Distance)>(_n - 1);
            int Find(int a)
            {
                while (parent[a] != a) { parent[a] = parent[parent[a]]; a = parent[a]; }
                return a;
            }
            while (result.Count < _n - 1)
            {
                for (var i = 0; i < _n; i++) components[i] = Find(i);
                for (var node = _nodes.Count - 1; node >= 0; node--)
                {
                    var entry = _nodes[node];
                    var component = components[_order[entry.Start]];
                    if (entry.Left >= 0)
                        uniform[node] = uniform[entry.Left] == uniform[entry.Right] ? uniform[entry.Left] : -1;
                    else
                    {
                        for (var p = entry.Start + 1; p < entry.Start + entry.Count; p++)
                            if (components[_order[p]] != component) { component = -1; break; }
                        uniform[node] = component;
                    }
                }
                Parallel.For(0, _n, i =>
                {
                    var best = (A: i, B: -1, Distance: double.PositiveInfinity);
                    Search(0, i, components, uniform, ref best);
                    candidates[i] = best;
                });
                var minimum = new Dictionary<int, (int A, int B, double Distance)>();
                for (var i = 0; i < _n; i++)
                {
                    var edge = candidates[i];
                    if (edge.B < 0) continue;
                    if (!minimum.TryGetValue(components[i], out var previous) || Compare(edge, previous) < 0)
                        minimum[components[i]] = edge;
                }
                var added = 0;
                foreach (var edge in minimum.Values.OrderBy(e => e.Distance).ThenBy(e => Math.Min(e.A, e.B)).ThenBy(e => Math.Max(e.A, e.B)))
                {
                    var a = Find(edge.A); var b = Find(edge.B);
                    if (a == b) continue;
                    parent[Math.Max(a, b)] = Math.Min(a, b);
                    result.Add(edge); added++;
                }
                if (added == 0) throw new InvalidOperationException("Exact reachability MST could not connect finite points.");
            }
            return result;
        }
        private static int Compare((int A, int B, double Distance) a, (int A, int B, double Distance) b)
        {
            var distance = a.Distance.CompareTo(b.Distance);
            if (distance != 0) return distance;
            var low = Math.Min(a.A, a.B).CompareTo(Math.Min(b.A, b.B));
            return low != 0 ? low : Math.Max(a.A, a.B).CompareTo(Math.Max(b.A, b.B));
        }
        private double Bound(int index, int point)
        {
            var node = _nodes[index]; double distance = 0;
            for (var j = 0; j < _d; j++)
            {
                var value = _points[point * _d + j];
                var delta = value < node.Low[j] ? node.Low[j] - value : value > node.High[j] ? value - node.High[j] : 0;
                distance += delta * delta;
            }
            return Math.Max(Math.Sqrt(distance), Math.Max(_core[point], node.MinCore));
        }
        private void Search(int index, int point, int[] components, int[] uniform, ref (int A, int B, double Distance) best)
        {
            if (uniform[index] == components[point] || Bound(index, point) > best.Distance) return;
            var node = _nodes[index];
            if (node.Left >= 0)
            {
                var leftFirst = Bound(node.Left, point) <= Bound(node.Right, point);
                Search(leftFirst ? node.Left : node.Right, point, components, uniform, ref best);
                Search(leftFirst ? node.Right : node.Left, point, components, uniform, ref best);
            }
            else for (var p = node.Start; p < node.Start + node.Count; p++)
            {
                var other = _order[p];
                if (components[other] == components[point]) continue;
                var distance = Math.Max(Euclidean(_points, _d, point, other), Math.Max(_core[point], _core[other]));
                var edge = (point, other, distance);
                if (Compare(edge, best) < 0) best = edge;
            }
        }
    }
}
