using System.Text.Json;

namespace Nebula_Windows.Services;

/// <summary>UI-only sidecar. Never writes to the source folder or immutable library metadata.</summary>
public sealed class UiState
{
    private static readonly string StatePath = Path.Combine(LibraryService.DataRoot, "ui-state.json");
    /// Where earlier builds kept these preferences. Read only until the first save.
    private static readonly string EarlierStatePath = Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "NebulaWindows", "ui-state.json");

    public string Appearance { get; set; } = "System";
    public bool IsSidebarVisible { get; set; } = true;
    public bool IsExploreVisible { get; set; } = true;
    public bool IsInspectorVisible { get; set; } = true;
    public HashSet<string> FavouriteIds { get; set; } = [];

    public static UiState Load()
    {
        try
        {
            var path = File.Exists(StatePath) ? StatePath : EarlierStatePath;
            var state = File.Exists(path)
                ? JsonSerializer.Deserialize<UiState>(File.ReadAllText(path)) ?? new()
                : new UiState();
            state.FavouriteIds ??= [];
            return state;
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException or JsonException)
        {
            LibraryService.Log($"UI preferences could not be restored: {error.Message}");
            return new();
        }
    }

    public void Save()
    {
        Directory.CreateDirectory(Path.GetDirectoryName(StatePath)!);
        var temporary = StatePath + ".tmp";
        File.WriteAllText(temporary, JsonSerializer.Serialize(this));
        File.Move(temporary, StatePath, true);
    }
}
