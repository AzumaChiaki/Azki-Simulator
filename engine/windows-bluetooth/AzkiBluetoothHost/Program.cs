using System.Text;
using System.Text.Json;
using AzkiBluetooth;

Console.InputEncoding = new UTF8Encoding(false);
Console.OutputEncoding = new UTF8Encoding(false);
using var host = new RadioHost();
var inFlight = new List<Task>();
while (await Console.In.ReadLineAsync() is { } line)
{
    if (line.Length > 1_048_576)
    {
        host.Write(new { id = (string?)null, ok = false, error = new { code = "request_too_large", message = "JSON line exceeds 1 MiB" } });
        continue;
    }
    JsonDocument document;
    try { document = JsonDocument.Parse(line); }
    catch (JsonException)
    {
        host.Write(new { id = (string?)null, ok = false, error = new { code = "invalid_json", message = "Expected a JSON object" } });
        continue;
    }
    using (document)
    {
        var request = document.RootElement.Clone();
        if (request.ValueKind != JsonValueKind.Object)
        {
            host.Write(new { id = (string?)null, ok = false, error = new { code = "invalid_request", message = "Expected a JSON object" } });
            continue;
        }
        inFlight.RemoveAll(task => task.IsCompleted);
        var isResponse = request.TryGetProperty("command", out var command) && command.ValueKind == JsonValueKind.String && command.GetString() == "peripheral.respond";
        if (inFlight.Count >= 128 && !isResponse)
        {
            var id = request.TryGetProperty("id", out var value) && value.ValueKind == JsonValueKind.String ? value.GetString() : null;
            host.Write(new { id, ok = false, error = new { code = "insufficient_resources", message = "At most128 outstanding control requests" } });
            continue;
        }
        // ATT responses must not wait behind a slow central connection. Each
        // operation domain is serialized by the host; responses are independent.
        inFlight.Add(host.HandleAsync(request));
    }
}
try { await Task.WhenAll(inFlight).WaitAsync(TimeSpan.FromSeconds(5)); }
catch (OperationCanceledException) { }
catch (TimeoutException) { }
finally
{
    host.RequestShutdown();
    try { await Task.WhenAll(inFlight).WaitAsync(TimeSpan.FromSeconds(1)); }
    catch (OperationCanceledException) { }
    catch (TimeoutException) { }
}
