using System.Net.Sockets;
using AzkiSimulator.Linux.Core;
using Xunit;

namespace AzkiSimulator.Linux.Tests;

/// <summary>
/// The session connection trusts the peer only when its credentials are the
/// caller's own, so a wrong option number is not a loud failure: SO_BSDCOMPAT
/// (16) answers with four zero bytes, which the old code read as a credential
/// mismatch and reported as "SO_PEERCRED unavailable: 0". Reading a real socket
/// pair is the only way to tell 17 from 16.
/// </summary>
public sealed class PeerCredentialTests : IDisposable
{
    private readonly string _root = Path.Combine(Path.GetTempPath(), $"azki-peer-{Guid.NewGuid():N}");

    [Fact]
    public void ReadsThePeerOfAConnectedUnixSocket()
    {
        Directory.CreateDirectory(_root);
        var path = Path.Combine(_root, "control.sock");
        using var listener = new Socket(AddressFamily.Unix, SocketType.Stream, ProtocolType.Unspecified);
        listener.Bind(new UnixDomainSocketEndPoint(path));
        listener.Listen(1);
        using var client = new Socket(AddressFamily.Unix, SocketType.Stream, ProtocolType.Unspecified);
        client.Connect(new UnixDomainSocketEndPoint(path));
        using var server = listener.Accept();

        var (pid, uid, _) = UnixNative.PeerCredentials(server.Handle);

        Assert.Equal(Environment.ProcessId, pid);
        Assert.Equal(UnixNative.getuid(), uid);
    }

    public void Dispose()
    {
        try { Directory.Delete(_root, recursive: true); } catch (IOException) { }
    }
}
