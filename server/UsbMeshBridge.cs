using System.Collections.Concurrent;
using System.IO.Ports;
using System.Text.Json;
using System.Threading.Channels;

namespace AutoMesh.Server;

public sealed record MeshDevice(
    string Uuid, string Name, string Mac, string State,
    string FirmwareVersion, int Address, bool Online, bool MeshConfigured);

/// <summary>
/// Exactly one physical serial port and one Mesh provisioner. ESP32 retains
/// all Mesh keys in NVS; the host never holds a copy of those secrets.
/// </summary>
public sealed class UsbMeshBridge : BackgroundService
{
    private readonly object _guard = new();
    private readonly Channel<string> _outbox = Channel.CreateBounded<string>(
        new BoundedChannelOptions(32) { SingleReader = true,
            SingleWriter = false, FullMode = BoundedChannelFullMode.Wait });
    private readonly ConcurrentDictionary<string, MeshDevice> _devices = new();
    private string? _desiredPort;
    private string? _connectedPort;
    private volatile bool _connected;
    private volatile bool _meshReady;
    private volatile bool _usbMode;
    private string? _version;
    private string? _lastError;
    private DateTimeOffset? _lastEvent;
    private long _selectionGeneration;

    public string[] GetPorts() => SerialPort.GetPortNames()
        .OrderBy(x => x, StringComparer.OrdinalIgnoreCase).ToArray();

    public object GetStatus() => new {
        desiredPort = _desiredPort,
        connectedPort = _connectedPort,
        connected = _connected,
        meshReady = _meshReady,
        usbMode = _usbMode,
        firmwareVersion = _version,
        lastError = _lastError,
        lastEvent = _lastEvent,
        deviceCount = _devices.Count
    };

    public MeshDevice[] GetDevices() => _devices.Values
        .OrderBy(x => x.Address == 0 ? int.MaxValue : x.Address)
        .ThenBy(x => x.Name).ToArray();

    public bool SelectPort(string? name, out string error)
    {
        error = "";
        if (string.IsNullOrWhiteSpace(name) || name.Length > 100 ||
            !GetPorts().Contains(name, StringComparer.OrdinalIgnoreCase))
        {
            error = "Выбранного последовательного порта нет в списке";
            return false;
        }
        lock (_guard)
        {
            _desiredPort = name;
            _selectionGeneration++;
        }
        return true;
    }

    public bool Send(string command) =>
        _connected && _outbox.Writer.TryWrite(command);

    protected override async Task ExecuteAsync(CancellationToken stoppingToken)
    {
        // Do not auto-open arbitrary COM ports that may belong to other devices.
        var initial = Environment.GetEnvironmentVariable("AUTOMESH_USB_PORT");
        if (!string.IsNullOrEmpty(initial))
        {
            lock (_guard) { _desiredPort = initial; _selectionGeneration++; }
        }

        while (!stoppingToken.IsCancellationRequested)
        {
            string? name;
            long generation;
            lock (_guard) { name = _desiredPort; generation = _selectionGeneration; }
            if (string.IsNullOrWhiteSpace(name))
            {
                await Task.Delay(500, stoppingToken);
                continue;
            }
            try
            {
                using var port = new SerialPort(name, 115200)
                {
                    ReadTimeout = 750,
                    WriteTimeout = 2500,
                    NewLine = "\n",
                    DtrEnable = false,
                    RtsEnable = false
                };
                port.Open();
                _connectedPort = name;
                _connected = true;
                _lastError = null;
                _meshReady = false;
                _version = null;
                _devices.Clear();
                Console.WriteLine("AutoMesh USB connected: " + name);
                port.WriteLine("HELLO");
                DateTimeOffset nextRefresh = DateTimeOffset.MinValue;
                while (!stoppingToken.IsCancellationRequested &&
                       generation == Interlocked.Read(ref _selectionGeneration))
                {
                    while (_outbox.Reader.TryRead(out var command))
                        port.WriteLine(command);
                    if (DateTimeOffset.UtcNow >= nextRefresh)
                    {
                        port.WriteLine("HELLO");
                        port.WriteLine("LIST");
                        nextRefresh = DateTimeOffset.UtcNow.AddSeconds(5);
                    }
                    try
                    {
                        var line = port.ReadLine();
                        if (line.Length <= 8192) ParseLine(line.Trim());
                    }
                    catch (TimeoutException) { /* Poll for new commands */ }
                }
            }
            catch (Exception e) when (e is not OperationCanceledException)
            {
                _lastError = e.Message;
                Console.Error.WriteLine("AutoMesh USB: " + e.Message);
            }
            finally
            {
                _connected = false;
                _meshReady = false;
                _connectedPort = null;
            }
            await Task.Delay(1200, stoppingToken);
        }
    }

    private void ParseLine(string line)
    {
        if (!line.StartsWith("AUTOMESH {", StringComparison.Ordinal)) return;
        try
        {
            using var parsed = JsonDocument.Parse(line["AUTOMESH ".Length..]);
            var data = parsed.RootElement;
            var type = Text(data, "event");
            _lastEvent = DateTimeOffset.UtcNow;
            switch (type)
            {
                case "ready":
                    _version = Text(data, "version");
                    _usbMode = Flag(data, "usb_mode");
                    _meshReady = Flag(data, "mesh_ready");
                    break;
                case "snapshot_begin":
                    _devices.Clear();
                    break;
                case "device":
                    StoreDevice(data);
                    break;
                case "candidate":
                {
                    var uuid = Text(data, "uuid");
                    if (uuid.Length != 32) break;
                    _devices.TryAdd(uuid, new MeshDevice(uuid, "", Text(data, "addr"),
                        "pending", "", 0, false, false));
                    break;
                }
                case "provisioned":
                {
                    var uuid = Text(data, "uuid");
                    if (_devices.TryGetValue(uuid, out var old))
                        _devices[uuid] = old with { State = "provisioning",
                            Address = Number(data, "address") };
                    break;
                }
                case "error":
                    _lastError = Text(data, "reason");
                    break;
                case "usb_mode_changed":
                    _usbMode = Flag(data, "enabled");
                    break;
            }
        }
        catch (JsonException) { /* Ignore unrelated and truncated serial logs */ }
    }

    private void StoreDevice(JsonElement data)
    {
        var uuid = Text(data, "uuid");
        if (uuid.Length != 32) return;
        _devices[uuid] = new MeshDevice(uuid, Text(data, "name"),
            Text(data, "mac"), Text(data, "state"),
            Text(data, "firmware_version"), Number(data, "address"),
            Flag(data, "online"), Flag(data, "mesh_configured"));
    }

    private static string Text(JsonElement e, string key) =>
        e.TryGetProperty(key, out var value) && value.ValueKind == JsonValueKind.String
            ? value.GetString() ?? "" : "";

    private static bool Flag(JsonElement e, string key) =>
        e.TryGetProperty(key, out var value) && value.ValueKind == JsonValueKind.True;

    private static int Number(JsonElement e, string key) =>
        e.TryGetProperty(key, out var value) && value.ValueKind == JsonValueKind.Number &&
        value.TryGetInt32(out var number) ? number : 0;
}
