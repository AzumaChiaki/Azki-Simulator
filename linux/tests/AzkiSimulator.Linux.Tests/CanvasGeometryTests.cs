using System;
using System.IO;
using Avalonia;
using AzkiSimulator.Linux.Services;
using Xunit;

namespace AzkiSimulator.Linux.Tests;

/// <summary>
/// The canvas maps a window point to a device pixel. Getting it wrong puts the
/// touch where the user did not press: the guest scrolls the wrong row and the
/// press highlight lands off the control. The macOS canvas owns the mapping in
/// SimulatorCanvasGeometry, so the Linux port is pinned to that formula here.
/// </summary>
public class CanvasGeometryTests
{
    // 450x536 is the Linux guest panel; it is deliberately not one of the four
    // Apple watch sizes the macOS app whitelists.
    private static readonly Size Panel = new(450, 536);

    [Fact]
    public void TheContentKeepsThePanelAspectRatioAndSitsInTheMiddle()
    {
        var bounds = new Rect(0, 0, 1000, 1000);
        var rect = CanvasGeometry.ContentRect(bounds, Panel);

        Assert.Equal(Panel.Width / Panel.Height, rect.Width / rect.Height, 6);
        Assert.Equal(bounds.Width, rect.Height, 6);
        Assert.Equal((bounds.Width - rect.Width) / 2, rect.X, 6);
        Assert.Equal(0, rect.Y, 6);
    }

    [Fact]
    public void AWiderWindowBarsTheTopAndBottomInsteadOfTheSides()
    {
        var bounds = new Rect(0, 0, 2000, 400);
        var rect = CanvasGeometry.ContentRect(bounds, Panel);

        Assert.Equal(400, rect.Height, 6);
        Assert.Equal(Panel.Width / Panel.Height, rect.Width / rect.Height, 6);
        Assert.Equal((bounds.Width - rect.Width) / 2, rect.X, 6);
    }

    [Fact]
    public void DegenerateGeometryHasNoContentAndAcceptsNoInput()
    {
        var panel = new Size(450, 536);
        foreach (var bounds in new[]
                 {
                     new Rect(0, 0, 0, 400),
                     new Rect(0, 0, 400, -10),
                     new Rect(double.NaN, 0, 400, 400),
                     new Rect(0, 0, double.PositiveInfinity, 400),
                 })
        {
            Assert.Equal(default, CanvasGeometry.ContentRect(bounds, panel));
            Assert.Null(CanvasGeometry.Input(new Point(10, 10), bounds, panel));
        }

        foreach (var size in new[] { new Size(0, 536), new Size(450, 0), new Size(double.NaN, 536) })
        {
            Assert.Equal(default, CanvasGeometry.ContentRect(new Rect(0, 0, 400, 400), size));
            Assert.Null(CanvasGeometry.Input(new Point(10, 10), new Rect(0, 0, 400, 400), size));
        }
    }

    [Fact]
    public void TheTopLeftCornerIsTheFirstPixelAndTheFarCornerTheLast()
    {
        var bounds = new Rect(0, 0, 900, 700);
        var rect = CanvasGeometry.ContentRect(bounds, Panel);

        Assert.Equal((0, 0), CanvasGeometry.Input(new Point(rect.X, rect.Y), bounds, Panel));
        Assert.Equal(((int)Panel.Width - 1, (int)Panel.Height - 1),
            CanvasGeometry.Input(new Point(rect.Right - 0.001, rect.Bottom - 0.001), bounds, Panel));
    }

    [Fact]
    public void TheRightAndBottomEdgesBelongToNoPixel()
    {
        var bounds = new Rect(0, 0, 900, 700);
        var rect = CanvasGeometry.ContentRect(bounds, Panel);

        Assert.Null(CanvasGeometry.Input(new Point(rect.Right, rect.Y + 1), bounds, Panel));
        Assert.Null(CanvasGeometry.Input(new Point(rect.X + 1, rect.Bottom), bounds, Panel));
        Assert.Null(CanvasGeometry.Input(new Point(rect.X - 0.5, rect.Y + 1), bounds, Panel));
        Assert.Null(CanvasGeometry.Input(new Point(rect.X + 1, rect.Y - 0.5), bounds, Panel));
        Assert.Null(CanvasGeometry.Input(new Point(double.NaN, rect.Y + 1), bounds, Panel));
    }

    [Fact]
    public void EveryPanelPixelIsReachableWithoutDrift()
    {
        var panel = new Size(64, 48);
        var bounds = new Rect(13, 29, 500, 400);
        var rect = CanvasGeometry.ContentRect(bounds, panel);

        for (var x = 0; x < panel.Width; x++)
        {
            var point = new Point(rect.X + (x + 0.5) / panel.Width * rect.Width, rect.Y + rect.Height / 2);
            Assert.Equal(x, CanvasGeometry.Input(point, bounds, panel)!.Value.X);
        }
        for (var y = 0; y < panel.Height; y++)
        {
            var point = new Point(rect.X + rect.Width / 2, rect.Y + (y + 0.5) / panel.Height * rect.Height);
            Assert.Equal(y, CanvasGeometry.Input(point, bounds, panel)!.Value.Y);
        }
    }

    [Fact]
    public void TheMappingFollowsTheReferenceFormula()
    {
        var source = ReferenceGeometrySource();
        Assert.Contains("let scale = min(bounds.width / pixelSize.width, bounds.height / pixelSize.height)", source);
        Assert.Contains("point.x >= rect.minX, point.x < rect.maxX, point.y >= rect.minY, point.y < rect.maxY", source);
        Assert.Contains("min(Int(pixelSize.width) - 1, Int((point.x - rect.minX) / rect.width * pixelSize.width))", source);
    }

    [Fact]
    public void ThePanelSizeIsNotRestrictedToTheAppleWatchSizes()
    {
        // The reference refuses any pixel size outside its four watch panels.
        // The Linux guest runs a 450x536 panel, so the port keeps the arithmetic
        // and drops the whitelist: the frame contract checks the geometry against
        // the session manifest instead.
        var reference = ReferenceGeometrySource();
        Assert.Contains("case .pixels192x490: 192", reference);
        Assert.Contains("case .pixels432x514: 432", reference);

        var bounds = new Rect(0, 0, 900, 700);
        Assert.NotNull(CanvasGeometry.Input(new Point(450, 350), bounds, Panel));
    }

    private static string ReferenceGeometrySource()
    {
        for (var probe = new DirectoryInfo(AppContext.BaseDirectory); probe is not null; probe = probe.Parent)
        {
            var candidate = Path.Combine(probe.FullName, "Sources/AzkiSimulator/Core/SimulatorFrame.swift");
            if (File.Exists(candidate)) return File.ReadAllText(candidate);
        }
        throw new FileNotFoundException("The macOS canvas geometry is the reference for this mapping.");
    }
}
