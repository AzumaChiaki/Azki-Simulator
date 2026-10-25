using System.Buffers.Binary;
using System.Net.Sockets;
using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;

namespace AzkiSimulator.Linux.Core;

/// <summary>session.json descriptor with the fields the supervisor consumes.</summary>
public sealed record SessionManifest(string Id, string SocketName, string FramebufferName,
    int Width, int Height, string Backend, string ProfileId, string ApprovalDigest)
{
    public static SessionManifest Decode(string json)
    {
        var node = JsonNode.Parse(json)?.AsObject() ?? throw new InvalidDataException("会话描述符不是 JSON 对象");
        if (JsonRead.AsInt(node["version"], -1) != 1 || JsonRead.AsString(node["socket"]) != "control.sock"
            || JsonRead.AsString(node["framebuffer"]) != "frame.raw")
            throw new InvalidDataException("不支持的会话描述符");
        // These numbers size the frame read, so they are bounded here rather than
        // trusted into an int overflow in FrameReader.
        var width = JsonRead.AsInt(node["width"], -1);
        var height = JsonRead.AsInt(node["height"], -1);
        if (width is < 1 or > 4096 || height is < 1 or > 4096)
            throw new InvalidDataException("会话画面尺寸无效");
        return new SessionManifest(
            JsonRead.AsString(node["id"]) ?? throw new InvalidDataException("会话缺少标识"),
            "control.sock", "frame.raw", width, height,
            JsonRead.AsString(node["backend"]) ?? "qemu",
            JsonRead.AsString(node["profileID"]) ?? "",
            JsonRead.AsString(node["approvalDigest"]) ?? "");
    }
}

/// <summary>
/// One bounded Unix connection per command over control.sock, mirroring
/// SimulatorSessionConnection: envelope {version,id,command,arguments},
/// newline framing, absolute deadline, allowlist, kernel peer-uid check and
/// session-root revalidation around every exchange.
/// </summary>
public sealed class BridgeClient : IDisposable
{
    private static readonly HashSet<string> Permitted = new()
    {
        "status", "start", "stop", "input", "device-button", "screenshot", "logs", "inspect-package", "stage-package",
        "pmu-adc-get", "pmu-adc-set", "guest-status", "guest-connect", "guest-install", "guest-operation",
        "guest-inventory", "guest-select", "external-open", "external-status", "external-request", "external-result",
        "external-close", "external-reconnect", "live-get", "live-set",
    };
    private static readonly TimeSpan DefaultDeadline = TimeSpan.FromSeconds(15);
    // Pointer events are awaited on the input path, so they get a tighter budget than
    // a control command; a wedged helper must not be able to hold the gesture open.
    private static readonly TimeSpan InputDeadline = TimeSpan.FromSeconds(4);

    private readonly string _root;
    private readonly SessionManifest _manifest;
    private readonly long _rootDevice;
    private readonly long _rootInode;
    private readonly object _inputGate = new();
    private bool _closed;
    private (int X, int Y, bool Down)? _activeInput;

    public BridgeClient(string sessionDirectory)
    {
        _root = Path.TrimEndingDirectorySeparator(sessionDirectory);
        var rootStat = UnixNative.Stat(_root);
        if (!UnixNative.IsDirectory(rootStat.Mode) || rootStat.Uid != UnixNative.getuid() || (rootStat.Mode & UnixNative.WorldWritableBits) != 0)
            throw new InvalidDataException("会话文件夹须属于当前用户，且不能允许其他用户写入");
        _rootDevice = rootStat.Device;
        _rootInode = rootStat.Inode;
        var directory = UnixNative.OpenDirectory(_root);
        using (directory)
        {
            using var stream = UnixNative.OpenRegularFile(directory, "session.json", 16_384)
                               ?? throw new InvalidDataException("会话缺少 session.json");
            using var reader = new StreamReader(stream, Encoding.UTF8);
            _manifest = SessionManifest.Decode(reader.ReadToEnd());
        }
    }

    public SessionManifest Manifest => _manifest;

    public void Dispose()
    {
        if (_closed) return;
        _closed = true;
        ReleaseInput();
    }

    public JsonNode Command(string command, JsonObject? arguments = null)
    {
        EnsureOpen();
        if (!Permitted.Contains(command)) throw new BridgeRemoteError("不受支持的会话命令");
        if (command == "stop") ReleaseInput();
        return Exchange(command, arguments ?? new JsonObject(), DefaultDeadline);
    }

    public void Input(int x, int y, bool down)
    {
        EnsureOpen();
        if (x < 0 || x >= _manifest.Width || y < 0 || y >= _manifest.Height)
            throw new BridgeRemoteError("触摸坐标超出设备画面");
        if (down) { lock (_inputGate) { _activeInput = (x, y, true); } }
        Exchange("input", new JsonObject { ["x"] = x, ["y"] = y, ["down"] = down }, InputDeadline);
        if (!down) { lock (_inputGate) { _activeInput = null; } }
    }

    private void ReleaseInput()
    {
        (int X, int Y)? pending;
        lock (_inputGate)
        {
            if (_activeInput is not { } event_) return;
            pending = (event_.X, event_.Y);
            _activeInput = null;
        }
        try { Exchange("input", new JsonObject { ["x"] = pending.Value.X, ["y"] = pending.Value.Y, ["down"] = false }, InputDeadline); }
        catch (Exception) { /* best-effort release, mirrors the Swift behavior */ }
    }

    private void EnsureOpen()
    {
        if (_closed) throw new BridgeConnectionClosed();
        ValidateRoot();
    }

    private void ValidateRoot()
    {
        var stat = UnixNative.Stat(_root);
        if (!UnixNative.IsDirectory(stat.Mode) || stat.Device != _rootDevice || stat.Inode != _rootInode
            || stat.Uid != UnixNative.getuid() || (stat.Mode & UnixNative.WorldWritableBits) != 0)
            throw new InvalidDataException("会话根目录的类型、身份或权限已改变");
        var socketPath = Path.Combine(_root, _manifest.SocketName);
        if (UnixNative.TryStat(socketPath, out var socketStat, out var socketError))
        {
            if (socketError != null) throw socketError;
            if (!UnixNative.IsSocket(socketStat.Mode) || socketStat.Uid != UnixNative.getuid() || (socketStat.Mode & UnixNative.GroupOtherAccessBits) != 0)
                throw new InvalidDataException("控制通道必须是会话根目录内、仅当前用户可访问的 Unix socket");
        }
    }

    private JsonNode Exchange(string command, JsonObject arguments, TimeSpan deadlineLength)
    {
        if (_closed) throw new BridgeConnectionClosed();
        var deadline = MonotonicClock.Now + deadlineLength;
        ValidateRoot();
        // guest-/external- commands open upstream connections inside the helper's own
        // budget, so they are told how long the host is still willing to wait. The
        // caller's arguments object is copied rather than mutated: a retried command
        // must send exactly what the caller built.
        var body = arguments.DeepClone().AsObject();
        if (command.StartsWith("guest-") || command.StartsWith("external-"))
            body["connectionTimeoutSeconds"] = (int)Math.Ceiling(deadlineLength.TotalSeconds);
        using var socket = new Socket(AddressFamily.Unix, SocketType.Stream, ProtocolType.Unspecified);
        var remaining = Math.Clamp((int)(deadline - MonotonicClock.Now).TotalMilliseconds, 250, 3_600_000);
        socket.SendTimeout = socket.ReceiveTimeout = remaining;
        try
        {
            socket.Connect(new UnixDomainSocketEndPoint(Path.Combine(_root, _manifest.SocketName)));
        }
        catch (SocketException error) when (error.SocketErrorCode is SocketError.ConnectionRefused or SocketError.TimedOut or SocketError.ConnectionAborted)
        {
            ValidateRoot();
            throw new BridgeConnectionClosed();
        }
        var (_, peerUid, _) = UnixNative.PeerCredentials(socket.Handle);
        if (peerUid != UnixNative.getuid()) throw new BridgeConnectionClosed();
        ValidateRoot();

        var request = new JsonObject
        {
            ["version"] = 1,
            ["id"] = _manifest.Id,
            ["command"] = command,
            ["arguments"] = body,
        }.ToJsonString() + "\n";
        var payload = Encoding.UTF8.GetBytes(request);
        if (payload.Length >= 65_536) throw new BridgeRemoteError("请求超出限制");
        socket.Send(payload, SocketFlags.None);

        var buffer = new byte[4096];
        var response = new MemoryStream(4096);
        while (response.Length <= 1_048_576 && MonotonicClock.Now < deadline)
        {
            int read;
            try { read = socket.Receive(buffer, SocketFlags.None); }
            catch (SocketException error) when (error.SocketErrorCode is SocketError.Interrupted or SocketError.TimedOut)
            {
                throw new BridgeConnectionClosed();
            }
            if (read <= 0) throw new BridgeConnectionClosed();
            // The newline is one byte, so scanning only the bytes just received stays
            // correct and keeps a multi-megabyte log response out of quadratic copies.
            var newline = Array.IndexOf(buffer, (byte)'\n', 0, read);
            if (newline < 0)
            {
                response.Write(buffer, 0, read);
                if (response.Length > 1_048_576) throw new BridgeRemoteError("响应超出大小限制");
                continue;
            }
            response.Write(buffer, 0, newline + 1);
            if (response.Length > 1_048_576) throw new BridgeRemoteError("响应超出大小限制");
            var reply = JsonSerializer.Deserialize<JsonObject>(
                Encoding.UTF8.GetString(response.GetBuffer(), 0, (int)response.Length - 1))
                ?? throw new BridgeRemoteError("响应不是 JSON 对象");
            if (!JsonRead.AsBool(reply["ok"]))
            {
                var code = JsonRead.AsString(reply["error"]?["code"]) ?? "unknown";
                var message = JsonRead.AsString(reply["error"]?["message"]) ?? code;
                if (code is "timeout" or "guest_timeout") throw new BridgeTimeoutException(message);
                throw new BridgeRemoteError($"{message} [{code}]");
            }
            return reply["result"] ?? new JsonObject();
        }
        throw new BridgeTimeoutException("响应超时");
    }
}

public sealed class BridgeConnectionClosed : IOException
{
    public BridgeConnectionClosed() : base("模拟器会话已断开") { }
}

public sealed class BridgeTimeoutException(string message) : IOException(message);
