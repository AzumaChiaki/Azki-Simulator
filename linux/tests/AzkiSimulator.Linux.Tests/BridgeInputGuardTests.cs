using System;
using System.IO;
using System.Net.Sockets;
using System.Text.Json.Nodes;
using AzkiSimulator.Linux.Core;
using Xunit;

namespace AzkiSimulator.Linux.Tests;

/// <summary>
/// The bridge is the only way a touch reaches the guest. A coordinate outside
/// the screen would be delivered as a device position the panel does not have,
/// and a command outside the permit list would run helper code the session
/// never asked for — so both are refused before the socket is touched.
/// </summary>
public sealed class BridgeInputGuardTests : IDisposable
{
    private readonly string _root;

    public BridgeInputGuardTests()
    {
        _root = Path.Combine(Path.GetTempPath(), $"azki-bridge-{Guid.NewGuid():N}");
        Directory.CreateDirectory(_root);
        File.SetUnixFileMode(_root, UnixFileMode.UserRead | UnixFileMode.UserWrite | UnixFileMode.UserExecute);
        File.WriteAllText(Path.Combine(_root, "session.json"),
            $$"""{"version":1,"id":"test-session","socket":"control.sock","framebuffer":"frame.raw","width":450,"height":536}""");
    }

    public void Dispose()
    {
        try { Directory.Delete(_root, recursive: true); } catch (IOException) { }
    }

    [Fact]
    public void ATouchOutsideTheScreenNeverReachesTheGuest()
    {
        using var client = new BridgeClient(_root);
        foreach (var (x, y) in new[] { (-1, 0), (0, -1), (450, 0), (0, 536), (9999, 9999) })
        {
            var failure = Assert.Throws<BridgeRemoteError>(() => client.Input(x, y, true));
            Assert.Equal("触摸坐标超出设备画面", failure.Message);
        }
    }

    [Fact]
    public void TheLastPixelIsStillInsideTheScreen()
    {
        using var client = new BridgeClient(_root);
        // The bound is inclusive of the last pixel, so the refusal above is a
        // range check and not a blanket one: this coordinate is accepted and
        // fails later, on the missing socket instead of on the coordinate.
        var failure = Record.Exception(() => client.Input(449, 535, true));
        Assert.IsType<SocketException>(failure);
    }

    [Fact]
    public void AnUnsupportedCommandIsRefusedBeforeAnyConnection()
    {
        using var client = new BridgeClient(_root);
        var failure = Assert.Throws<BridgeRemoteError>(() => client.Command("shell"));
        Assert.Equal("不受支持的会话命令", failure.Message);
    }

    [Fact]
    public void TheSessionFolderHasToBelongToThisUserAndStayPrivate()
    {
        File.SetUnixFileMode(_root, UnixFileMode.UserRead | UnixFileMode.UserWrite | UnixFileMode.UserExecute |
                                        UnixFileMode.GroupRead | UnixFileMode.GroupWrite | UnixFileMode.GroupExecute |
                                        UnixFileMode.OtherRead | UnixFileMode.OtherWrite | UnixFileMode.OtherExecute);
        var failure = Assert.Throws<InvalidDataException>(() => new BridgeClient(_root));
        Assert.Contains("当前用户", failure.Message);
    }

    [Fact]
    public void ASessionWithoutItsDescriptorIsRefused()
    {
        File.Delete(Path.Combine(_root, "session.json"));
        Assert.Throws<InvalidDataException>(() => new BridgeClient(_root));
    }

    [Fact]
    public void ThePermittedCommandsReachTheConnectionInsteadOfBeingRefused()
    {
        using var client = new BridgeClient(_root);
        var failure = Record.Exception(() => client.Command("status", new JsonObject()));
        Assert.IsType<SocketException>(failure);
    }
}
