using Nebula_Windows.Services;
using System.Diagnostics;
using System.Security.Cryptography;
using System.Text.Json;

if (args.Length < 2 || !int.TryParse(args[1], out var minimum) || minimum < 1)
{
    Console.Error.WriteLine("Usage: Nebula.Import <source-folder> <minimum-valid-images> [--allow-file-errors]");
    return 2;
}
var source = Path.GetFullPath(args[0]);
using var cancellation = new CancellationTokenSource();
Console.CancelKeyPress += (_, eventArgs) => { eventArgs.Cancel = true; cancellation.Cancel(); };
var watch = Stopwatch.StartNew();
var last = -1;
var progress = new Progress<ImportProgress>(state =>
{
    var bucket = state.Processed / 100;
    if (Interlocked.Exchange(ref last, bucket) != bucket || state.Processed == state.Total)
        Console.WriteLine($"{state.Processed}/{state.Total} examined; {state.Imported} Windows-decoded images");
});
try
{
    var library = await new LibraryService().ImportAsync(source, progress, cancellation.Token);
    var mismatches = new List<string>();
    foreach (var photo in library.Photos)
    {
        await using var file = File.OpenRead(photo.OriginalPath);
        var hash = Convert.ToHexString(await SHA256.HashDataAsync(file, cancellation.Token)).ToLowerInvariant();
        if (hash != photo.Id) mismatches.Add(photo.OriginalPath);
    }
    var passed = library.Photos.Count >= minimum && mismatches.Count == 0 &&
        (library.Errors.Count == 0 || args.Contains("--allow-file-errors", StringComparer.Ordinal));
    var summary = new
    {
        status = passed ? "complete" : "incomplete",
        source, minimum_images = minimum, imported = library.Photos.Count,
        distinct_source_hashes = library.Photos.Select(photo => photo.Id).Distinct().Count(),
        skipped = library.Skipped, errors = library.Errors, original_hash_mismatches = mismatches,
        thumbnails_present = library.Photos.Count(photo => File.Exists(photo.ThumbnailPath)),
        capture_dates = library.Photos.Count(photo => photo.TakenAt.HasValue),
        decoder = "Windows.Graphics.Imaging; same linked LibraryService source as the WinUI application",
        elapsed_seconds = watch.Elapsed.TotalSeconds, os = Environment.OSVersion.ToString(),
        architecture = System.Runtime.InteropServices.RuntimeInformation.ProcessArchitecture.ToString(),
        updated_at = DateTimeOffset.UtcNow,
    };
    var report = Path.Combine(LibraryService.DataRoot, "import-validation.json");
    await File.WriteAllTextAsync(report, JsonSerializer.Serialize(summary, new JsonSerializerOptions { WriteIndented = true }));
    Console.WriteLine(JsonSerializer.Serialize(summary));
    Console.WriteLine(report);
    return passed ? 0 : 1;
}
catch (Exception error)
{
    LibraryService.Log($"Headless import failed: {error}");
    Console.Error.WriteLine(error);
    return 1;
}
