using System.Text.Json;

namespace Nebula_Windows.Services;

/// <summary>UI-only sidecar. Never writes to the source folder or immutable library metadata.</summary>
public sealed class UiState
{
    private static readonly string StatePath = Path.Combine(LibraryService.DataRoot, "ui-state.json");

    public string Appearance { get; set; } = "System";
    public bool IsSidebarVisible { get; set; } = true;
    public bool IsExploreVisible { get; set; } = true;
    public bool IsInspectorVisible { get; set; } = true;
    public HashSet<string> FavouriteIds { get; set; } = [];

    public static UiState Load()
    {
        try
        {
            var state = File.Exists(StatePath)
                ? JsonSerializer.Deserialize<UiState>(File.ReadAllText(StatePath)) ?? new()
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
