using System.Globalization;
using System.Text;
using System.Text.RegularExpressions;

namespace Nebula.Embed;

internal static partial class QueryText
{
    private static readonly Dictionary<string, string> Irregular = new(StringComparer.Ordinal)
    {
        ["person"] = "people", ["man"] = "men", ["woman"] = "women", ["child"] = "children", ["foot"] = "feet",
        ["tooth"] = "teeth", ["mouse"] = "mice", ["goose"] = "geese", ["leaf"] = "leaves", ["life"] = "lives",
        ["knife"] = "knives", ["wife"] = "wives", ["half"] = "halves", ["loaf"] = "loaves", ["shelf"] = "shelves",
        ["cactus"] = "cacti", ["fungus"] = "fungi", ["focus"] = "foci"
    };
    private static readonly Dictionary<string, string> IrregularInverse = Irregular.ToDictionary(p => p.Value, p => p.Key, StringComparer.Ordinal);
    private static readonly HashSet<string> Uncountable = ["food", "water", "snow", "rain", "grass", "sand", "hair", "furniture", "music", "art", "light", "sky", "fish", "sheep", "deer", "aircraft", "series", "species"];
    private static readonly HashSet<string> Articles = ["a", "an", "the", "some", "my", "his", "her", "their", "our"];

    public static IReadOnlyList<string> Variants(string query)
    {
        var q = Normalize(query);
        if (q.Length == 0) return [];
        var words = q.Split(' ', StringSplitOptions.RemoveEmptyEntries);
        var core = words.Where(w => !Articles.Contains(w)).ToArray();
        if (core.Length == 0) core = words;
        var output = new List<string> { string.Join(' ', core) };
        var head = core[^1];
        var singular = Singularize(head);
        foreach (var form in new[] { singular, Pluralize(singular) })
            if (form != head) output.Add(string.Join(' ', core[..^1].Concat([form])));
        return output.Distinct(StringComparer.Ordinal).ToArray();
    }

    private static string Normalize(string value)
    {
        var text = value.Normalize(NormalizationForm.FormKD).ToLowerInvariant();
        var sb = new StringBuilder(text.Length);
        foreach (var c in text)
        {
            var cat = CharUnicodeInfo.GetUnicodeCategory(c);
            if (cat == UnicodeCategory.NonSpacingMark) continue;
            sb.Append(char.IsLetterOrDigit(c) || c == '\'' || c == '-' ? c : ' ');
        }
        var q = Space().Replace(sb.ToString(), " ").Trim();
        while (true)
        {
            var next = Carrier().Replace(q, "").Trim();
            if (next == q) return q;
            q = next;
        }
    }

    private static string Singularize(string word)
    {
        if (Uncountable.Contains(word) || Irregular.ContainsKey(word)) return word;
        if (IrregularInverse.TryGetValue(word, out var irregular)) return irregular;
        if (word.EndsWith("ies", StringComparison.Ordinal) && word.Length > 4) return word[..^3] + "y";
        if (word.EndsWith("ves", StringComparison.Ordinal)) return word[..^3] + "f";
        if (Regex.IsMatch(word, "(ches|shes|xes|zes)$")) return word[..^2];
        if (word.EndsWith('s') && !word.EndsWith("ss", StringComparison.Ordinal)) return word[..^1];
        return word;
    }

    private static string Pluralize(string word)
    {
        if (Uncountable.Contains(word)) return word;
        if (Irregular.TryGetValue(word, out var irregular)) return irregular;
        if (IrregularInverse.ContainsKey(word)) return word;
        if (Regex.IsMatch(word, "(s|x|z|ch|sh)$")) return word + "es";
        if (Regex.IsMatch(word, "[^aeiou]y$")) return word[..^1] + "ies";
        if (word.EndsWith('f')) return word[..^1] + "ves";
        if (word.EndsWith("fe", StringComparison.Ordinal)) return word[..^2] + "ves";
        return word + "s";
    }

    [GeneratedRegex("\\s+")]
    private static partial Regex Space();

    [GeneratedRegex("^(show me|show|find|search for|pictures? of|photos? of|images? of|pics? of)\\s+")]
    private static partial Regex Carrier();
}
