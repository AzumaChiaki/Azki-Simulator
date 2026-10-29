using System.Text;
using System.Text.Json.Nodes;

namespace AzkiSimulator.Linux.Core;

/// <summary>The incremental log request and cursor rules, copied from SimulatorLogStream.
/// Absence of a cursor is part of the wire contract: the helper reads null as "begin at the
/// tail" and rejects an empty object as an invalid cursor, which would fail every poll and
/// leave the panel silent for the whole session.</summary>
internal static class LogStream
{
    public const int Limit = 16_384;

    public static JsonObject Request(JsonNode? cursor) => new()
    {
        ["name"] = "stderr.log",
        ["stream"] = true,
        ["limit"] = Limit,
        ["cursor"] = cursor,
    };

    /// <summary>A null fault accepts the response and says what to send next; a set fault is
    /// the message to show, and the stream stops rather than looping on a bridge it cannot
    /// talk to.</summary>
    public static (JsonNode? Cursor, string? Fault) Advance(JsonNode response)
    {
        if (JsonRead.AsInt(response["version"]) != 2) return (null, "该会话尚不支持增量日志；请使用更新后的桥接服务");
        var streamID = JsonRead.AsString(response["streamID"]);
        if (JsonRead.AsBool(response["missing"]) || streamID is null) return (null, null);
        if (streamID.Length == 0 || Encoding.UTF8.GetByteCount(streamID) > 64
            || !JsonRead.TryNumber(response["nextOffset"], out var offset)
            || !double.IsFinite(offset) || Math.Round(offset) != offset
            || offset < 0 || offset > 9_007_199_254_740_991)
        {
            return (null, "日志游标无效");
        }
        return (new JsonObject { ["streamID"] = streamID, ["offset"] = (long)offset }, null);
    }
}
