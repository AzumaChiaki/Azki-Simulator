using System.Diagnostics;
using System.Text.Json.Nodes;
using AzkiSimulator.Linux.Core;
using Xunit;

namespace AzkiSimulator.Linux.Tests;

/// <summary>
/// Deleting an instance's private data on Linux. The reference grants this command no write
/// access at all, so its Swift call site reports a failure and the folder leaks; the port
/// tried to do better by granting the instance folder, which bwrap then binds — and a bound
/// directory is a mount point that cannot be removed from inside the namespace. These tests
/// pin the shape that actually works: the parent becomes writable and every sibling is
/// re-bound read-only on top, so the command can finish its own removal and nothing else.
/// Runs only with AZKI_APP_ROOT at an extracted package.
/// </summary>
[Collection("ProcessEnvironment")]
public class InstanceDeleteCliTests
{
    private static readonly Guid Target = Guid.Parse("6f1c2b3a-4d5e-4f60-8a9b-0c1d2e3f4a5b");
    private static readonly Guid Sibling = Guid.Parse("7a2b3c4d-5e6f-4a0b-9c8d-1e2f3a4b5c6d");

    private static string MakeInstance(string library, Guid id, params (string Name, string Content)[] files)
    {
        var path = Path.Combine(library, "instances", id.ToString());
        Directory.CreateDirectory(Path.Combine(path, "storage"));
        foreach (var (name, content) in files) File.WriteAllText(Path.Combine(path, "storage", name), content);
        return path;
    }

    private static string? LibraryRoot()
    {
        var root = Environment.GetEnvironmentVariable("AZKI_DATA_ROOT");
        return string.IsNullOrEmpty(root) ? null : Path.Combine(root, "com.azki.AzkiSimulator", "FirmwareLibrary");
    }

    [Fact]
    [Trait("Category", "Integration")]
    public async Task RemovesTheInstanceFolderAndLeavesItsSiblings()
    {
        var layout = EngineLayout.TryResolve();
        Assert.True(layout is not null, "需要 AZKI_APP_ROOT 指向解包后的发布件（scripts/build-linux.sh 的产物）");
        var dataRoot = Path.Combine(Path.GetTempPath(), $"azki-cli-{Guid.NewGuid():N}");
        var previous = Environment.GetEnvironmentVariable("AZKI_DATA_ROOT");
        Environment.SetEnvironmentVariable("AZKI_DATA_ROOT", dataRoot);
        try
        {
            var library = LibraryRoot()!;
            Directory.CreateDirectory(Path.Combine(library, "instances"));
            var target = MakeInstance(library, Target, ("settings.json", "{}"), ("image.bin", new string('x', 2048)));
            var sibling = MakeInstance(library, Sibling, ("settings.json", "{\"keep\":true}"));

            var runner = new InspectionRunner(layout!);
            CliResult result = await runner.RunAsync(
                "instance-delete", new[] { "--instance-id", Target.ToString() });
            if (result.Payload is not JsonObject)
            {
                // The sandbox itself can be killed before it says anything: this machine
                // runs under a system-wide OOM storm from another workload, and the kernel
                // takes the sandbox with it. The command is idempotent, so one retry tells
                // an external kill apart from a defect in the command.
                result = await runner.RunAsync(
                    "instance-delete", new[] { "--instance-id", Target.ToString() });
            }
            // The receipt is the contract: the payload printed it, so the command did its
            // work. The launcher can also be killed by a signal after that, which shows up
            // as exit 137 and says nothing about the deletion.
            Assert.True(result.Payload is JsonObject, $"命令没有留下回执：stdout={result.Raw}；stderr={result.Error}");
            var receipt = result.Payload as JsonObject;
            Assert.True(receipt is not null, $"回执不是 JSON：{result.Raw}");
            Assert.True(JsonRead.AsBool(receipt["removed"]), "回执没有确认删除");
            Assert.True(JsonRead.AsInt(receipt["entries"]) >= 3, "回执里的条目数对不上实际删掉的东西");
            Assert.False(Directory.Exists(target), "实例目录还在：被绑成挂载点后沙箱删不掉它自己");
            Assert.True(Directory.Exists(sibling));
            Assert.Equal("{\"keep\":true}",
                File.ReadAllText(Path.Combine(sibling, "storage", "settings.json")));
        }
        finally
        {
            Environment.SetEnvironmentVariable("AZKI_DATA_ROOT", previous);
            try { Directory.Delete(dataRoot, recursive: true); } catch (IOException) { }
        }
    }

    [Fact]
    [Trait("Category", "Integration")]
    public async Task CannotTouchASiblingEvenThoughTheParentIsWritable()
    {
        var layout = EngineLayout.TryResolve();
        Assert.True(layout is not null, "需要 AZKI_APP_ROOT 指向解包后的发布件（scripts/build-linux.sh 的产物）");
        var dataRoot = Path.Combine(Path.GetTempPath(), $"azki-cli-{Guid.NewGuid():N}");
        var previous = Environment.GetEnvironmentVariable("AZKI_DATA_ROOT");
        Environment.SetEnvironmentVariable("AZKI_DATA_ROOT", dataRoot);
        try
        {
            var library = LibraryRoot()!;
            Directory.CreateDirectory(Path.Combine(library, "instances"));
            MakeInstance(library, Target, ("settings.json", "{}"));
            var sibling = MakeInstance(library, Sibling, ("settings.json", "{}"));

            // The same policy the command runs under, with a payload that only tries to
            // unlink a sibling's file. If the re-bind did not hold, the file would be gone.
            async Task<(int Exit, string Error)> ProbeAsync()
            {
                var policy = new SandboxPolicy { ReadOnlyTrees = { layout!.EngineRoot, library } };
                CliGrants.Apply(policy, "instance-delete", new[] { "--instance-id", Target.ToString() }, library);
                var info = new ProcessStartInfo
                {
                    FileName = SandboxPolicy.Launcher,
                    RedirectStandardOutput = true,
                    RedirectStandardError = true,
                    UseShellExecute = false,
                };
                foreach (var argument in policy.BuildArguments()) info.ArgumentList.Add(argument);
                // The kernel speaks the locale of the child, so the refusal is read in C.
                info.Environment["LC_ALL"] = "C";
                info.Environment["LANG"] = "C";
                foreach (var argument in new[] { "/bin/sh", "-c",
                    $"rm -f {sibling}/storage/settings.json" }) info.ArgumentList.Add(argument);
                using var process = Process.Start(info)!;
                var output = await process.StandardOutput.ReadToEndAsync();
                var error = await process.StandardError.ReadToEndAsync();
                await process.WaitForExitAsync();
                return (process.ExitCode, error + output);
            }

            var probe = await ProbeAsync();
            if (probe.Exit > 128)
            {
                // The sandbox was killed by a signal (this machine is under a system-wide
                // OOM storm from another workload), which says nothing about the grant.
                probe = await ProbeAsync();
            }

            Assert.True(File.Exists(sibling + "/storage/settings.json"), "兄弟实例的文件被删掉了：" + probe.Error);
            // The kernel's own refusal is the proof; a successful unlink leaves this empty.
            Assert.Contains("Read-only file system", probe.Error, StringComparison.Ordinal);
        }
        finally
        {
            Environment.SetEnvironmentVariable("AZKI_DATA_ROOT", previous);
            try { Directory.Delete(dataRoot, recursive: true); } catch (IOException) { }
        }
    }
}
