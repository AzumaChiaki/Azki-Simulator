using System.Diagnostics;

namespace AzkiSimulator.Linux.Core;

/// <summary>Deadline clock that survives wall-clock changes, unlike DateTime.Now.</summary>
internal static class MonotonicClock
{
    private static readonly long Origin = Stopwatch.GetTimestamp();
    public static TimeSpan Now => Stopwatch.GetElapsedTime(Origin);
}
