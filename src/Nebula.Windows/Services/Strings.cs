using System.Globalization;
using Microsoft.Windows.ApplicationModel.Resources;

namespace Nebula_Windows.Services;

/// <summary>User-visible strings from Strings\en-US\Resources.resw.</summary>
internal static class Strings
{
    private static readonly ResourceLoader Loader = new();

    public static string Get(string key) => Loader.GetString(key);

    public static string Format(string key, params object?[] args) => string.Format(CultureInfo.CurrentCulture, Get(key), args);
}
