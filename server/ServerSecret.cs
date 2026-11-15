using System.Security.Cryptography;

namespace AutoMesh.Server;

public static class ServerSecret
{
    public static string LoadOrCreate()
    {
        var configured = Environment.GetEnvironmentVariable("AUTOMESH_SERVER_TOKEN");
        if (!string.IsNullOrEmpty(configured))
        {
            if (configured.Length < 12)
                throw new InvalidOperationException("AUTOMESH_SERVER_TOKEN must have at least 12 characters.");
            return configured;
        }
        var directory = Path.Combine(
            Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData), "AutoMeshServer");
        Directory.CreateDirectory(directory);
        var filename = Path.Combine(directory, "admin.token");
        if (File.Exists(filename))
        {
            var existing = File.ReadAllText(filename).Trim();
            if (existing.Length < 12) throw new InvalidOperationException("Invalid admin.token file.");
            return existing;
        }
        var created = Convert.ToHexString(RandomNumberGenerator.GetBytes(24)).ToLowerInvariant();
        using (var stream = new FileStream(filename, FileMode.CreateNew, FileAccess.Write, FileShare.None))
        using (var writer = new StreamWriter(stream))
            writer.WriteLine(created);
        if (!OperatingSystem.IsWindows())
            File.SetUnixFileMode(filename, UnixFileMode.UserRead | UnixFileMode.UserWrite);
        return created;
    }
}
