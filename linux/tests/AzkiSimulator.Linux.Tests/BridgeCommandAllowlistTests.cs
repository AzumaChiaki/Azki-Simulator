using System;
using System.Collections.Generic;
using System.IO;
using System.Text.RegularExpressions;
using AzkiSimulator.Linux.Core;
using Xunit;

namespace AzkiSimulator.Linux.Tests;

/// <summary>
/// The bridge refuses every command outside its permit list. A command the
/// reference permits but this list does not is a feature that silently does
/// nothing at runtime — the PMU sliders, the guest package install and the
/// external bridge all travel that way — so the list is compared with the
/// reference instead of being trusted.
/// </summary>
public class BridgeCommandAllowlistTests
{
    [Fact]
    public void ThePermittedCommandsAreExactlyTheReferenceSet()
    {
        var reference = ReferenceCommands();
        Assert.NotEmpty(reference);
        Assert.Equal(reference.OrderBy(name => name, StringComparer.Ordinal),
            BridgeClient.Permitted.OrderBy(name => name, StringComparer.Ordinal));
    }

    [Fact]
    public void EveryCommandTheReferenceRefusesIsRefusedHereToo()
    {
        // A command outside the list must not reach the guest.
        Assert.DoesNotContain("shell", BridgeClient.Permitted);
        Assert.DoesNotContain("evaluate", BridgeClient.Permitted);
        Assert.DoesNotContain("", BridgeClient.Permitted);
    }

    private static IReadOnlyCollection<string> ReferenceCommands()
    {
        var commands = new List<string>();
        commands.AddRange(Quoted(new Regex(@"let permitted = \[(.*?)\]", RegexOptions.Singleline),
            ReferenceFile("Sources/AzkiSimulator/Core/SimulatorSessionConnection.swift")));
        var live = ReferenceFile("Sources/AzkiSimulator/Core/SimulatorLiveControls.swift");
        foreach (Match match in Regex.Matches(live, @"static let \w+ = ""([^""]+)"""))
            commands.Add(match.Groups[1].Value);
        return commands;
    }

    private static IEnumerable<string> Quoted(Regex pattern, string text) =>
        pattern.Matches(text).Cast<Match>()
            .SelectMany(match => Regex.Matches(match.Groups[1].Value, @"""([^""]+)""").Cast<Match>())
            .Select(match => match.Groups[1].Value);

    private static string ReferenceFile(string relativePath)
    {
        for (var probe = new DirectoryInfo(AppContext.BaseDirectory); probe is not null; probe = probe.Parent)
        {
            var candidate = Path.Combine(probe.FullName, relativePath);
            if (File.Exists(candidate)) return File.ReadAllText(candidate);
        }
        throw new FileNotFoundException($"The macOS session connection is the reference for this list: {relativePath}");
    }
}
