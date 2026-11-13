using CommunityToolkit.Mvvm.ComponentModel;
using Nebula.Core;
using Nebula_Windows.Services;
using System.Collections.ObjectModel;

namespace Nebula_Windows.ViewModels;

public sealed record ExploreFacet(string Name, string Kind, string Key, int Count, int Total,
    string Thumbnail = "", double Fraction = 0)
{
    public string CountLabel => $"{Count:N0} / {Total:N0}";
    public string CountText => Count.ToString("N0");
    public string Description => Strings.Format("Facet_Description", Name, Count, Total);
}

public partial class MainPageViewModel : ObservableObject
{
    [ObservableProperty] public partial string LibraryName { get; set; } = Strings.Get("Library_DefaultName");
    [ObservableProperty] public partial string LibraryCount { get; set; } = Strings.Get("Library_NoFolder");
    [ObservableProperty] public partial string Status { get; set; } = Strings.Get("Status_Default");
    [ObservableProperty] public partial string RendererStatus { get; set; } = "";
    [ObservableProperty] public partial bool IsImporting { get; set; }
    [ObservableProperty] public partial double ImportPercent { get; set; }
    [ObservableProperty] public partial string ImportDetail { get; set; } = "";
    [ObservableProperty] public partial string Error { get; set; } = "";
    [ObservableProperty] public partial bool HasError { get; set; }
    [ObservableProperty] public partial bool HasLibrary { get; set; }
    [ObservableProperty] public partial Photo? Selected { get; set; }
    [ObservableProperty] public partial string ResultCount { get; set; } = Strings.Get("Results_None");
    [ObservableProperty] public partial string ResultsNote { get; set; } = "";
    [ObservableProperty] public partial string ExploreTitle { get; set; } = Strings.Get("Explore_Title");
    [ObservableProperty] public partial string ExploreDescription { get; set; } = Strings.Get("Explore_Description");
    [ObservableProperty] public partial string FilterLabel { get; set; } = "";
    [ObservableProperty] public partial string AllCount { get; set; } = "0";
    [ObservableProperty] public partial string RecentCount { get; set; } = "0";
    [ObservableProperty] public partial string FavouriteCount { get; set; } = "0";
    public ObservableCollection<Photo> Results { get; } = [];
    public ObservableCollection<ExploreFacet> Facets { get; } = [];
}
