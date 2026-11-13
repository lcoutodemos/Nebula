using System.Drawing;
using System.Drawing.Imaging;
using System.Runtime.InteropServices;
using System.Collections.Concurrent;

namespace Nebula.Embed;

internal static class ImagePreprocessor
{
    private static readonly ConcurrentDictionary<(int Input, int Output), (int Start, int Count, int[] Weights)[]> CoefficientCache = new();
    public static void WriteTensor(string path, Span<float> destination)
    {
        const int size = EmbeddingIndex.ImageSize;
        if (destination.Length < 3 * size * size) throw new ArgumentException("Destination tensor is too small.", nameof(destination));
        using var image = Image.FromFile(path);
        using var bitmap = new Bitmap(image.Width, image.Height, PixelFormat.Format32bppArgb);
        using (var g = Graphics.FromImage(bitmap))
            g.DrawImage(image, 0, 0, image.Width, image.Height);
        var src = ReadPixels(bitmap);
        ResizeBilinear(src, bitmap.Width, bitmap.Height, size, size, destination);
    }

    private static Pixel[] ReadPixels(Bitmap bitmap)
    {
        var rect = new Rectangle(0, 0, bitmap.Width, bitmap.Height);
        var data = bitmap.LockBits(rect, ImageLockMode.ReadOnly, PixelFormat.Format32bppArgb);
        try
        {
            var bytes = new byte[data.Stride * data.Height];
            Marshal.Copy(data.Scan0, bytes, 0, bytes.Length);
            var output = new Pixel[bitmap.Width * bitmap.Height];
            for (var y = 0; y < bitmap.Height; y++)
            {
                var row = y * data.Stride;
                for (var x = 0; x < bitmap.Width; x++)
                {
                    var o = row + x * 4;
                    output[y * bitmap.Width + x] = new Pixel(bytes[o + 2], bytes[o + 1], bytes[o]);
                }
            }
            return output;
        }
        finally
        {
            bitmap.UnlockBits(data);
        }
    }

    private static void ResizeBilinear(Pixel[] source, int width, int height, int outWidth, int outHeight, Span<float> destination)
    {
        var h = Coefficients(width, outWidth);
        var v = Coefficients(height, outHeight);
        var temp = new Pixel[outWidth * height];
        for (var y = 0; y < height; y++)
        {
            for (var x = 0; x < outWidth; x++)
            {
                var (start, count, weights) = h[x];
                var r = 1 << 21; var g = 1 << 21; var b = 1 << 21;
                for (var i = 0; i < count; i++)
                {
                    var p = source[y * width + start + i];
                    r += p.R * weights[i];
                    g += p.G * weights[i];
                    b += p.B * weights[i];
                }
                temp[y * outWidth + x] = new Pixel(Clip(r), Clip(g), Clip(b));
            }
        }
        var plane = outWidth * outHeight;
        for (var y = 0; y < outHeight; y++)
        {
            var (start, count, weights) = v[y];
            for (var x = 0; x < outWidth; x++)
            {
                var r = 1 << 21; var g = 1 << 21; var b = 1 << 21;
                for (var i = 0; i < count; i++)
                {
                    var p = temp[(start + i) * outWidth + x];
                    r += p.R * weights[i];
                    g += p.G * weights[i];
                    b += p.B * weights[i];
                }
                var offset = y * outWidth + x;
                destination[offset] = Clip(r) / 127.5f - 1f;
                destination[plane + offset] = Clip(g) / 127.5f - 1f;
                destination[2 * plane + offset] = Clip(b) / 127.5f - 1f;
            }
        }
    }

    private static (int Start, int Count, int[] Weights)[] Coefficients(int input, int output)
    {
        // Imported thumbnail dimensions are bounded. Don't retain arbitrary original-image kernels.
        return input <= 1024 ? CoefficientCache.GetOrAdd((input, output), key => BuildCoefficients(key.Input, key.Output)) : BuildCoefficients(input, output);
    }

    private static (int Start, int Count, int[] Weights)[] BuildCoefficients(int input, int output)
    {
        var scale = (double)input / output;
        var support = Math.Max(scale, 1.0);
        var ss = 1.0 / support;
        var coeffs = new (int, int, int[])[output];
        for (var xx = 0; xx < output; xx++)
        {
            var center = (xx + 0.5) * scale;
            var xmin = Math.Max((int)(center - support + 0.5), 0);
            var count = Math.Min((int)(center + support + 0.5), input) - xmin;
            count = Math.Max(count, 0);
            var raw = new double[count];
            var total = 0.0;
            for (var x = 0; x < count; x++)
            {
                var t = Math.Abs((x + xmin - center + 0.5) * ss);
                raw[x] = t < 1 ? 1 - t : 0;
                total += raw[x];
            }
            var weights = raw.Select(w =>
            {
                var v = total != 0 ? w / total : w;
                return (int)(v * (1 << 22) + (v < 0 ? -0.5 : 0.5));
            }).ToArray();
            coeffs[xx] = (xmin, count, weights);
        }
        return coeffs;
    }

    private static byte Clip(int value) => (byte)Math.Clamp(value >> 22, 0, 255);

    private readonly record struct Pixel(byte R, byte G, byte B);
}
