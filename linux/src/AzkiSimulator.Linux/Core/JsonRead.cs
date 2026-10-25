using System.Globalization;
using System.Text.Json;
using System.Text.Json.Nodes;

namespace AzkiSimulator.Linux.Core;

/// <summary>Readers that treat a wrong backing type as absence instead of throwing.
/// JsonNode.GetValue&lt;T&gt;() throws when the node holds another JSON type, and an
/// unexpected throw inside an async command reaches the dispatcher, so every field
/// taken from the bridge or a CLI payload is read through here.</summary>
internal static class JsonRead
{
    public static string? AsString(JsonNode? node) =>
        node is not null && node.GetValueKind() == JsonValueKind.String ? node.GetValue<string>() : null;

    public static bool AsBool(JsonNode? node) => node is not null && node.GetValueKind() == JsonValueKind.True;

    public static bool TryNumber(JsonNode? node, out double value)
    {
        value = 0;
        return node is not null && node.GetValueKind() == JsonValueKind.Number
            && double.TryParse(node.ToJsonString(), NumberStyles.Float, CultureInfo.InvariantCulture, out value);
    }

    public static int AsInt(JsonNode? node, int fallback = 0) =>
        TryNumber(node, out var number) && number is >= int.MinValue and <= int.MaxValue &&
        Math.Round(number) == number ? (int)number : fallback;

    /// <summary>Array of strings, skipping entries that are not strings.</summary>
    public static IReadOnlyList<string> AsStringList(JsonNode? node) =>
        node is JsonArray array
            ? array.Select(JsonRead.AsString).Where(static value => value is not null).Select(value => value!).ToList()
            : new List<string>();
}
