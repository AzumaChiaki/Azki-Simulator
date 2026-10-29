namespace AzkiSimulator.Linux.Core;

/// <summary>
/// Reads process-group membership out of /proc.
///
/// The guest is started as its own group leader, so the group's id is the guest's
/// pid. /proc is read directly rather than through Process.GetProcesses plus
/// getpgid: another user's pid answers EPERM, which would make every shared
/// machine report a survivor, and a pid that vanishes mid-scan is not evidence
/// of one either.
/// </summary>
internal static class ProcessGroupSurvivors
{
    public static bool Any(int group)
    {
        if (group <= 0) return false;
        foreach (var entry in Entries())
        {
            if (entry.Pid != group && entry.Group == group) return true;
        }
        return false;
    }

    /// <summary>Find the guest supervisor among the children of the launcher process.
    /// bwrap's payload leads its own group because --new-session calls setsid before the
    /// exec, so a child whose group equals its pid is that supervisor.</summary>
    /// <returns>The supervisor's pid, zero while it has not appeared yet, and -1 if the
    /// launcher owns more than one group-leading child, which is not the picture this
    /// session may safely signal.</returns>
    public static int GroupLeadingChild(int launcher)
    {
        if (launcher <= 0) return 0;
        var found = 0;
        foreach (var entry in Entries())
        {
            if (entry.Parent != launcher || entry.Pid != entry.Group) continue;
            if (found != 0) return -1;
            found = entry.Pid;
        }
        return found;
    }

    private static IEnumerable<(int Pid, int Parent, int Group)> Entries()
    {
        foreach (var directory in Directory.EnumerateDirectories("/proc"))
        {
            var name = Path.GetFileName(directory);
            if (name.Length == 0 || !name.All(char.IsAsciiDigit) || !int.TryParse(name, out var pid)) continue;
            var entry = Read(directory, pid);
            if (entry is not null) yield return entry.Value;
        }
    }

    /// <summary>A pid that exits or turns out to belong to another user mid-scan is not
    /// evidence of anything, so its record is simply skipped.</summary>
    private static (int Pid, int Parent, int Group)? Read(string directory, int pid)
    {
        try
        {
            var text = File.ReadAllText(Path.Combine(directory, "stat"));
            var close = text.LastIndexOf(')');
            if (close < 1 || text.Length <= close + 2) return null;
            // After the parenthesised command: state, ppid, pgrp.
            var fields = text.Substring(close + 2).Split(' ');
            if (fields.Length <= 2) return null;
            if (!int.TryParse(fields[1], out var parent) || !int.TryParse(fields[2], out var group)) return null;
            return (pid, parent, group);
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException)
        {
            return null;
        }
    }
}
