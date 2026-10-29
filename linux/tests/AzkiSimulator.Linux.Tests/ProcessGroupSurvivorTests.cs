using System;
using System.Diagnostics;
using System.IO;
using System.Threading;
using AzkiSimulator.Linux.Core;
using Xunit;

namespace AzkiSimulator.Linux.Tests;

/// <summary>
/// A guest that survives its session keeps the device running: the simulator
/// would report the session as stopped while the emulator is still holding the
/// hardware. The check that decides "stopped" reads the process group out of
/// /proc, so it is exercised here with a real process group of our own.
/// </summary>
public class ProcessGroupSurvivorTests
{
    /// <summary>Starts a process that leads its own group, the way the guest does,
    /// with one more member inside it.</summary>
    private static Process StartGroupLeader()
    {
        var info = new ProcessStartInfo
        {
            FileName = "/usr/bin/setsid",
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            UseShellExecute = false,
        };
        info.ArgumentList.Add("/bin/sh");
        info.ArgumentList.Add("-c");
        info.ArgumentList.Add("sleep 300 & sleep 300");
        var process = Process.Start(info) ?? throw new InvalidOperationException("cannot start the group leader");
        for (var attempt = 0; attempt < 100; attempt++)
        {
            if (Members(process.Id).Count > 0) return process;
            Thread.Sleep(20);
        }
        return process;
    }

    /// <summary>The pids in a group, read the same way the production check does.</summary>
    private static System.Collections.Generic.List<int> Members(int group)
    {
        var members = new System.Collections.Generic.List<int>();
        foreach (var entry in Directory.EnumerateDirectories("/proc"))
        {
            var name = Path.GetFileName(entry);
            if (name.Length == 0 || !name.All(char.IsAsciiDigit) || !int.TryParse(name, out var pid)) continue;
            try
            {
                var text = File.ReadAllText(Path.Combine(entry, "stat"));
                var close = text.LastIndexOf(')');
                if (close < 1 || text.Length <= close + 2) continue;
                var fields = text.Substring(close + 2).Split(' ');
                if (fields.Length > 2 && int.TryParse(fields[2], out var pgrp) && pgrp == group) members.Add(pid);
            }
            catch (Exception error) when (error is IOException or UnauthorizedAccessException) { }
        }
        return members;
    }

    [Fact]
    public void AMemberThatOutlivesTheLeaderIsReported()
    {
        using var leader = StartGroupLeader();
        var group = leader.Id;
        Assert.NotEmpty(Members(group));
        Assert.True(ProcessGroupSurvivors.Any(group));

        // Killing only the leader leaves its member behind. That is exactly the
        // case the cleanup check exists for: the session is gone, the guest is not.
        try { leader.Kill(entireProcessTree: false); } catch (InvalidOperationException) { }
        Assert.True(leader.WaitForExit(10_000));
        Assert.True(ProcessGroupSurvivors.Any(group));

        UnixNative.killpg(group, 9);
        for (var attempt = 0; attempt < 100 && ProcessGroupSurvivors.Any(group); attempt++) Thread.Sleep(20);
        Assert.False(ProcessGroupSurvivors.Any(group));
    }

    [Fact]
    public void AnEmptyGroupIsNotReported()
    {
        using var leader = StartGroupLeader();
        var group = leader.Id;
        UnixNative.killpg(group, 9);
        try { leader.WaitForExit(10_000); } catch (InvalidOperationException) { }
        for (var attempt = 0; attempt < 100 && ProcessGroupSurvivors.Any(group); attempt++) Thread.Sleep(20);
        Assert.False(ProcessGroupSurvivors.Any(group));
    }

    [Fact]
    public void AnImpossibleGroupIsNeverReported()
    {
        Assert.False(ProcessGroupSurvivors.Any(0));
        Assert.False(ProcessGroupSurvivors.Any(-1));
    }

    /// <summary>The supervisor bwrap forks leads its own group, and the host has to reap
    /// that group rather than the launcher's. These pin the discovery the session
    /// supervisor depends on, including the two pictures it must refuse: no child yet,
    /// and more than one group-leading child.</summary>
    private static int DiscoveryOf(string script)
    {
        using var launcher = Start("sh", "-c", script);
        var found = 0;
        try
        {
            for (var attempt = 0; attempt < 250; attempt++)
            {
                found = ProcessGroupSurvivors.GroupLeadingChild(launcher.Id);
                if (found != 0) break;
                Thread.Sleep(20);
            }
            return found;
        }
        finally
        {
            // The children outlive a shell that was only there to fork them, so every
            // process this launcher spawned is reaped here, group leader or not.
            foreach (var child in ChildrenOf(launcher.Id))
            {
                try { Process.GetProcessById(child).Kill(entireProcessTree: false); }
                catch (Exception) { }
            }
            try { launcher.Kill(entireProcessTree: false); } catch (InvalidOperationException) { }
        }
    }

    private static Process Start(string fileName, params string[] arguments)
    {
        var info = new ProcessStartInfo
        {
            FileName = "/bin/" + fileName,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            UseShellExecute = false,
        };
        foreach (var argument in arguments) info.ArgumentList.Add(argument);
        var process = Process.Start(info) ?? throw new InvalidOperationException("cannot start " + fileName);
        _ = process.StandardOutput.ReadToEndAsync();
        return process;
    }

    /// <summary>The pids a process forked, read from the same /proc fields the
    /// production check uses.</summary>
    private static System.Collections.Generic.List<int> ChildrenOf(int parent)
    {
        var children = new System.Collections.Generic.List<int>();
        foreach (var entry in Directory.EnumerateDirectories("/proc"))
        {
            var name = Path.GetFileName(entry);
            if (name.Length == 0 || !name.All(char.IsAsciiDigit) || !int.TryParse(name, out var pid)) continue;
            try
            {
                var text = File.ReadAllText(Path.Combine(entry, "stat"));
                var fields = text.Substring(text.LastIndexOf(')') + 2).Split(' ');
                if (fields.Length > 1 && int.TryParse(fields[1], out var ppid) && ppid == parent) children.Add(pid);
            }
            catch (Exception error) when (error is IOException or UnauthorizedAccessException) { }
        }
        return children;
    }

    [Fact]
    public void AChildThatLeadsItsOwnGroupIsFound()
    {
        Assert.True(DiscoveryOf("setsid sleep 300 & sleep 300 & wait") > 0,
            "the group-leading child was not found");
    }

    [Fact]
    public void AChildThatOnlyInheritsItsLaunchersGroupIsNot()
    {
        // No setsid: the child stays in the launcher's group, which is not a supervisor
        // and must never be published or signalled as one.
        using var launcher = Start("sh", "-c", "sleep 300 & echo GO; wait");
        try
        {
            for (var attempt = 0; attempt < 25; attempt++)
            {
                Assert.Equal(0, ProcessGroupSurvivors.GroupLeadingChild(launcher.Id));
                Thread.Sleep(50);
            }
        }
        finally
        {
            foreach (var child in ChildrenOf(launcher.Id))
            {
                try { Process.GetProcessById(child).Kill(entireProcessTree: false); }
                catch (Exception) { }
            }
        }
    }

    [Fact]
    public void TwoGroupLeadingChildrenAreRefused()
    {
        Assert.Equal(-1, DiscoveryOf("setsid sleep 300 & setsid sleep 300 & wait"));
    }
}
