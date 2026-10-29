using System.IO;
using System.Linq;
using System.Text.Json;
using System.Text.Json.Nodes;
using System.Threading.Tasks;
using AzkiSimulator.Linux.Core;
using Xunit;

namespace AzkiSimulator.Linux.Tests;

/// <summary>
/// The incremental log request is the one bridge call whose argument rules live in a
/// separate language: the helper names the accepted keys, decides that a missing cursor
/// means "start at the tail", and refuses anything else. These tests hand the exact
/// request the desktop app builds to that shipped validator instead of restating its
/// rules, because a cursor the helper refuses fails every poll and the log panel simply
/// never fills.
/// </summary>
public class LogStreamCursorTests
{
    private const string Validator = """
        import json, sys
        sys.path.insert(0, %s)
        from azki_simulator import log_stream
        try:
            chunk = log_stream.read_chunk(None, json.loads(sys.argv[1]))
        except Exception as error:
            print(f"{type(error).__name__}: {error}")
            raise SystemExit(3)
        print(json.dumps({key: chunk[key] for key in ("version", "name", "missing", "reset", "nextOffset")}))
        """;

    private static async Task<(int ExitCode, string Stdout, string Stderr)> AskHelper(JsonObject request)
    {
        var runtime = PythonProbe.RuntimeDirectory();
        Assert.True(runtime is not null, "the repository runtime is the oracle for this contract");
        var script = Validator.Replace("%s", PythonProbe.Literal(runtime));
        return await PythonProbe.RunAsync(script, request.ToJsonString());
    }

    [Fact]
    public async Task TheFirstRequestCarriesNoCursorAndTheHelperAcceptsIt()
    {
        var accepted = await AskHelper(LogStream.Request(null));
        Assert.True(accepted.ExitCode == 0, $"the helper refused the opening request: {accepted.Stdout}{accepted.Stderr}");
        using var chunk = JsonDocument.Parse(accepted.Stdout);
        Assert.True(chunk.RootElement.GetProperty("missing").GetBoolean());
        Assert.Equal("stderr.log", chunk.RootElement.GetProperty("name").GetString());
    }

    /// <summary>The shape the Linux client had before this contract was written: an empty
    /// object is not an absent cursor, and the helper rejects it outright.</summary>
    [Fact]
    public async Task AnEmptyObjectCursorIsRefusedSoItMustNotBeSent()
    {
        var refused = await AskHelper(LogStream.Request(new JsonObject()));
        Assert.Equal(3, refused.ExitCode);
        Assert.Contains("Invalid incremental log cursor", refused.Stdout);
    }

    [Fact]
    public async Task AValidRequestSendsOnlyTheKeysTheHelperAllows()
    {
        var request = LogStream.Request(null);
        Assert.Equal(new[] { "cursor", "limit", "name", "stream" }, request.Select(pair => pair.Key).OrderBy(key => key).ToArray());
        var accepted = await AskHelper(request);
        Assert.True(accepted.ExitCode == 0, accepted.Stdout + accepted.Stderr);
    }

    [Fact]
    public async Task TheCursorAdvanceProducesIsAcceptedByTheSameHelper()
    {
        var (cursor, fault) = LogStream.Advance(Parse("""
            {"version": 2, "name": "stderr.log", "streamID": "2048:98765", "nextOffset": 4096,
             "dataBase64": "aGVsbG8=", "reset": false, "missing": false, "hasMore": false, "skippedBytes": 0}
            """));
        Assert.Null(fault);
        Assert.NotNull(cursor);
        var accepted = await AskHelper(LogStream.Request(cursor));
        Assert.True(accepted.ExitCode == 0, $"the helper refused our own cursor: {accepted.Stdout}");
    }

    [Fact]
    public void AStreamThatIsNotThereYetSendsTheReaderBackToNoCursor()
    {
        var (cursor, fault) = LogStream.Advance(Parse("""
            {"version": 2, "name": "stderr.log", "streamID": null, "nextOffset": 0, "dataBase64": "",
             "reset": true, "missing": true, "hasMore": false, "skippedBytes": 0}
            """));
        Assert.Null(fault);
        Assert.Null(cursor);
    }

    [Fact]
    public void ALegacyBridgeIsReportedRatherThanPolledForever()
    {
        var (cursor, fault) = LogStream.Advance(Parse("""{"version": 1, "name": "stderr.log", "text": "..."}"""));
        Assert.Null(cursor);
        Assert.Contains("增量日志", fault);
    }

    [Theory]
    [InlineData("""{"version":2,"streamID":"2048:9","nextOffset":1.5,"missing":false}""")]
    [InlineData("""{"version":2,"streamID":"2048:9","nextOffset":-1,"missing":false}""")]
    [InlineData("""{"version":2,"streamID":"2048:9","nextOffset":9007199254740992,"missing":false}""")]
    [InlineData("""{"version":2,"streamID":"2048:9","missing":false}""")]
    public void ACursorTheHelperWouldRefuseStopsTheStream(string payload)
    {
        var (cursor, fault) = LogStream.Advance(Parse(payload));
        Assert.Null(cursor);
        Assert.NotNull(fault);
    }

    [Fact]
    public void TheChunkSizeMatchesTheReference()
    {
        Assert.Equal(16_384, LogStream.Limit);
        var runtime = PythonProbe.RuntimeDirectory();
        Assert.True(runtime is not null);
        var swift = File.ReadAllText(Path.Combine(Directory.GetParent(runtime!)!.FullName,
            "Core/SimulatorLogStream.swift"));
        Assert.Contains(@"""limit"": .number(16_384)", swift);
    }

    private static JsonObject Parse(string payload) => JsonNode.Parse(payload)!.AsObject();
}
