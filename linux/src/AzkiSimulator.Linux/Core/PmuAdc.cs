using System.Globalization;
using System.Text.Json.Nodes;

namespace AzkiSimulator.Linux.Core;

/// <summary>
/// Port of PMUADCDescriptors.swift. Register assignments follow best1503.c's
/// ISPI CS3/page0 read path; raw counts are not calibrated physical units.
/// </summary>
public static class PmuAdc
{
    public const int DefaultRawValue = 21_000;
    public const int RawMin = 0, RawMax = 65_535;

    /// <summary>JsonValue.TryGetValue is strict about the node's backing type, so numbers
    /// parsed from the wire and numbers built in memory need one shared reading path.</summary>
    private static bool TryNumber(JsonNode? node, out double value)
    {
        value = 0;
        return node is JsonValue
            && double.TryParse(node.ToJsonString(), NumberStyles.Float, CultureInfo.InvariantCulture, out value);
    }

    public static string Purpose(int channel, string? profileId = null) => (profileId, channel) switch
    {
        ("q66@4.100.177", 2) => "影响固件电池电压和电量估算 · 单位为原始计数（raw）",
        ("q66@4.100.177", 5) => "NTC 测温路径输入 · 原始计数（raw），尚未校准为 °C",
        _ => $"PMU 采样输入 {channel} · 原始计数（raw），尚未换算为物理量",
    };

    public static string Title(int channel, string? profileId = null) => (profileId, channel) switch
    {
        ("q66@4.100.177", 2) => "电池电压采样 · 0x58",
        ("q66@4.100.177", 5) => "NTC 温度采样 · 0x5B",
        _ => $"采样寄存器 0x{0x56 + channel:X2}",
    };

    public static int[]? Values(JsonNode? response)
    {
        var entries = response?["state"]?["values"]?.AsArray();
        if (entries is null || entries.Count != 8) return null;
        var values = new List<int>(8);
        foreach (var entry in entries)
        {
            if (!TryNumber(entry, out var number)
                || !double.IsFinite(number) || Math.Round(number) != number || number < RawMin || number > RawMax)
                return null;
            values.Add((int)number);
        }
        return values.ToArray();
    }

    public static string ReadStatus(int channel, JsonNode? response)
    {
        var state = response?["state"]?.AsObject();
        if (channel is < 0 or > 7 || state is null
            || !TryNumber(state["version"], out var version)
            || state["readCount"]?.AsArray() is not { Count: 8 } reads
            || state["lastReadVersion"]?.AsArray() is not { Count: 8 } versions
            || !TryNumber(reads[channel], out var count)
            || !TryNumber(versions[channel], out var lastVersion))
            return "尚未读取采样状态";
        if (version is not (>= 0 and <= 9_007_199_254_740_991) || lastVersion < 0 || lastVersion > version || count < 0)
            return "读取版本未知";
        if (count == 0) return "固件尚未读取此采样";
        if (Math.Abs(lastVersion - version) < double.Epsilon) return "固件已读取当前采样；应用效果未确认";
        return "固件仅读取过旧采样，等待当前输入";
    }

    public static string ReadCount(int channel, JsonNode? response)
    {
        var counts = response?["state"]?["readCount"]?.AsArray();
        if (channel is < 0 or > 7 || counts is null || counts.Count != 8
            || !TryNumber(counts[channel], out var number)
            || !double.IsFinite(number) || Math.Round(number) != number || number > 9_007_199_254_740_991)
            return "尚无读取记录";
        return $"本次启动固件读取 {Convert.ToInt64(number)} 次";
    }
}

/// <summary>Refresh of observed hardware state must not discard unapplied edits.</summary>
public sealed class PmuAdcDraft
{
    public string[] Text { get; private set; } = new string[8];
    private int[]? _observed;

    public int[]? Values
    {
        get
        {
            var parsed = new List<int>(8);
            foreach (var entry in Text)
                if (int.TryParse(entry, out var value) && value is >= PmuAdc.RawMin and <= PmuAdc.RawMax) parsed.Add(value);
            return parsed.Count == 8 ? parsed.ToArray() : null;
        }
    }

    public bool HasChanges => _observed is not null && Values is not null && !Values.SequenceEqual(_observed);

    public void Receive(JsonNode? response)
    {
        var latest = PmuAdc.Values(response);
        if (latest is null)
        {
            _observed = null;
            // Never null elements: the bound TextBox writes straight back into these rows.
            Text = Enumerable.Repeat(string.Empty, 8).ToArray();
            return;
        }
        for (var index = 0; index < 8; index++)
            if (_observed is null || (int.TryParse(Text[index], out var edited) && edited == _observed[index]))
                Text[index] = latest[index].ToString();
        _observed = latest;
    }

    public void DiscardEdits()
    {
        if (_observed is not null) Text = _observed.Select(static value => value.ToString()).ToArray();
    }

    /// <summary>Channel rows adopt refreshed text only when untouched or equal to the old observation.</summary>
    public bool CanAdopt(int channel, string current) =>
        _observed is null || string.IsNullOrEmpty(current) ||
        (int.TryParse(current, out var value) && _observed[channel] == value);

    public void UseBootDefaults() => Text = Enumerable.Repeat(PmuAdc.DefaultRawValue.ToString(), 8).ToArray();
}
