using System;
using System.Collections.Generic;
using System.IO;
using AzkiSimulator.Linux.Services;
using Xunit;

namespace AzkiSimulator.Linux.Tests;

/// <summary>
/// A touch the guest never sees lifted is a stuck finger: the device keeps scrolling,
/// keeps the press highlight, and ignores the next tap. The macOS canvas owns that
/// guarantee with a four-method state machine, so the Linux canvas has to reproduce the
/// machine rather than forward each pointer event on its own.
/// </summary>
public class TouchContactTests
{
    [Fact]
    public void PressAndReleaseInsideEmitDownThenUpAtTheirOwnPoints()
    {
        var touch = new TouchContactTracker();
        Assert.Equal((3, 7, true), touch.Pressed(Point(3, 7)));
        Assert.Equal((4, 8, false), touch.Released(Point(4, 8)));
        Assert.Null(touch.Released(Point(4, 8)));
    }

    [Fact]
    public void ReleaseOutsideTheScreenStillLiftsTheFinger()
    {
        var touch = new TouchContactTracker();
        touch.Pressed(Point(120, 200));
        Assert.Equal((120, 200, false), touch.Released(null));
    }

    [Fact]
    public void DraggingOffTheScreenLiftsTheFingerAndEndsTheContact()
    {
        var touch = new TouchContactTracker();
        touch.Pressed(Point(10, 10));
        Assert.Equal((10, 10, false), touch.Moved(null));
        // Coming back inside is a new gesture: without a press there is nothing to drag.
        Assert.Null(touch.Moved(Point(20, 20)));
        Assert.Null(touch.Released(Point(20, 20)));
    }

    [Fact]
    public void PressOutsideStartsNoContactSoItsReleaseSendsNothing()
    {
        var touch = new TouchContactTracker();
        Assert.Null(touch.Pressed(null));
        Assert.Null(touch.Moved(Point(5, 5)));
        Assert.Null(touch.Released(null));
        Assert.Null(touch.Cancelled());
    }

    [Fact]
    public void LostCaptureLiftsAtTheRememberedContact()
    {
        var touch = new TouchContactTracker();
        touch.Pressed(Point(1, 2));
        touch.Moved(Point(3, 4));
        Assert.Equal((3, 4, false), touch.Cancelled());
        Assert.Null(touch.Cancelled());
    }

    /// <summary>However the pointer behaves, every down is answered and nothing lifts
    /// a finger that was never put down.</summary>
    [Fact]
    public void RandomGestureSequencesNeverLeaveAFingerDown()
    {
        var random = new Random(20261025);
        for (var round = 0; round < 5_000; round++)
        {
            var touch = new TouchContactTracker();
            var events = new List<(int X, int Y, bool Down)>();
            for (var step = 0; step < 12; step++)
            {
                var point = random.Next(4) == 0 ? null : Point(random.Next(450), random.Next(536));
                var emitted = step == 0 ? touch.Pressed(point)
                    : random.Next(20) == 0 ? touch.Cancelled()
                    : random.Next(6) == 0 ? touch.Released(point)
                    : touch.Moved(point);
                if (emitted is { } value) events.Add(value);
            }
            // The window always gets a final lift: releasing the pointer drops the capture,
            // and closing it does too, so the sequence cannot legitimately end mid-press.
            if (touch.Cancelled() is { } lift) events.Add(lift);
            var held = false;
            foreach (var (x, y, down) in events)
            {
                if (down)
                {
                    // A drag is a stream of downs, so only an up without a preceding down
                    // would mean the guest's touch state had come untracked.
                    held = true;
                }
                else
                {
                    // The lift may land on the release point, which the reference canvas
                    // prefers over the remembered contact, so only its order is checked
                    // here; the exact coordinates are the unit tests above.
                    Assert.True(held, $"round {round} lifted a finger that was never down at {x},{y}");
                    held = false;
                }
            }
            Assert.False(held, $"round {round} ended with the finger held down");
        }
    }

    [Fact]
    public void TheMacosReferenceCancelsTheContactWhenThePointerLeavesTheScreen()
    {
        var source = ReferenceCanvasSource();
        Assert.Contains("guard acceptsInput, let input = mapped(point, down: true) else { cancelContact(); return }", source);
        Assert.Contains("if let input = mapped(point, down: false) { contact = input }", source);
    }

    private static (int X, int Y)? Point(int x, int y) => (x, y);

    private static string ReferenceCanvasSource()
    {
        for (var probe = new DirectoryInfo(AppContext.BaseDirectory); probe is not null; probe = probe.Parent)
        {
            var candidate = Path.Combine(probe.FullName, "Sources/AzkiSimulator/UI/SimulatorNativeCanvas.swift");
            if (File.Exists(candidate)) return File.ReadAllText(candidate);
        }
        throw new FileNotFoundException("The macOS canvas is the reference for this state machine.");
    }
}
