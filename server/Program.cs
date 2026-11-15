using System.Security.Cryptography;
using System.Text;
using AutoMesh.Server;

var builder = WebApplication.CreateBuilder(args);
// Restrict to the local computer; remote access needs a secured reverse proxy.
builder.WebHost.UseUrls(Environment.GetEnvironmentVariable("AUTOMESH_LISTEN")
    ?? "http://127.0.0.1:8765");
builder.Services.AddSingleton<UsbMeshBridge>();
builder.Services.AddHostedService(p => p.GetRequiredService<UsbMeshBridge>());
var app = builder.Build();
var secret = ServerSecret.LoadOrCreate();
Console.WriteLine("AutoMesh Server administrator token: " + secret);
Console.WriteLine("Open http://127.0.0.1:8765/ on this computer.");

static bool Authorized(HttpRequest request, string expected)
{
    var actual = request.Headers["X-AutoMesh-Admin"].ToString();
    if (actual.Length == 0 || actual.Length != expected.Length) return false;
    return CryptographicOperations.FixedTimeEquals(
        Encoding.UTF8.GetBytes(actual), Encoding.UTF8.GetBytes(expected));
}

app.UseDefaultFiles();
app.UseStaticFiles();
app.MapGet("/api/health", () => Results.Ok(new { healthy = true }));
app.MapGet("/api/auth", (HttpRequest request) =>
    Authorized(request, secret) ? Results.Ok(new { authorized = true }) : Results.Unauthorized());
app.MapGet("/api/status", (HttpRequest request, UsbMeshBridge usb) =>
    Authorized(request, secret) ? Results.Ok(usb.GetStatus()) : Results.Unauthorized());
app.MapGet("/api/ports", (HttpRequest request, UsbMeshBridge usb) =>
    Authorized(request, secret) ? Results.Ok(usb.GetPorts()) : Results.Unauthorized());
app.MapGet("/api/devices", (HttpRequest request, UsbMeshBridge usb) =>
    Authorized(request, secret) ? Results.Ok(usb.GetDevices()) : Results.Unauthorized());
app.MapPost("/api/port", (HttpRequest request, PortRequest input, UsbMeshBridge usb) =>
{
    if (!Authorized(request, secret)) return Results.Unauthorized();
    return usb.SelectPort(input.Port, out var error)
        ? Results.Ok(new { selected = input.Port }) : Results.BadRequest(new { error });
});
app.MapPost("/api/refresh", (HttpRequest request, UsbMeshBridge usb) =>
{
    if (!Authorized(request, secret)) return Results.Unauthorized();
    return usb.Send("LIST") ? Results.Accepted() : Results.Conflict(new { error = "Нет USB-соединения" });
});
app.MapPost("/api/usb-mode", (HttpRequest request, ModeRequest input, UsbMeshBridge usb) =>
{
    if (!Authorized(request, secret)) return Results.Unauthorized();
    return usb.Send(input.Enabled ? "USBMODE 1" : "USBMODE 0")
        ? Results.Accepted() : Results.Conflict(new { error = "Нет USB-соединения" });
});
app.MapPost("/api/devices/{uuid}/approve", (HttpRequest request, string uuid, UsbMeshBridge usb) =>
{
    if (!Authorized(request, secret)) return Results.Unauthorized();
    if (!ValidUuid(uuid)) return Results.BadRequest(new { error = "Некорректный UUID" });
    return usb.Send("APPROVE " + uuid.ToLowerInvariant())
        ? Results.Accepted() : Results.Conflict(new { error = "Нет USB-соединения" });
});
app.MapPost("/api/devices/{uuid}/deny", (HttpRequest request, string uuid, UsbMeshBridge usb) =>
{
    if (!Authorized(request, secret)) return Results.Unauthorized();
    if (!ValidUuid(uuid)) return Results.BadRequest(new { error = "Некорректный UUID" });
    return usb.Send("DENY " + uuid.ToLowerInvariant())
        ? Results.Accepted() : Results.Conflict(new { error = "Нет USB-соединения" });
});
app.Run();

static bool ValidUuid(string value) => value.Length == 32 &&
    value.All(c => c is >= '0' and <= '9' or >= 'a' and <= 'f' or >= 'A' and <= 'F');

record PortRequest(string Port);
record ModeRequest(bool Enabled);
