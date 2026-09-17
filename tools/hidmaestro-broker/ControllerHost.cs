// Owns the HIDMaestro SDK context and the live HMController objects.
//
// Controllers are pinned to Sunshine's gamepad slot index via CreateControllerAt so Sunshine
// can derive the shared-memory object names (Global\HIDMaestroInput<N> ...) without asking.

using System.Security.Principal;
using HIDMaestro;
using Microsoft.Win32;

namespace Sunshine.HidMaestroBroker;

internal sealed class ControllerHost : IDisposable
{
    private const string ManifestRegPath = @"SOFTWARE\HIDMaestro";
    private const string ManifestRegValue = "InstalledManifestSha256";

    private readonly HMContext _context = new();
    private readonly Dictionary<int, HMController> _controllers = new();
    private readonly object _lock = new();
    private bool _driverEnsured;

    public string SdkVersion { get; }

    public ControllerHost()
    {
        int loaded = _context.LoadDefaultProfiles();
        SdkVersion = typeof(HMContext).Assembly.GetName().Version?.ToString() ?? "unknown";
        Log.Info($"HIDMaestro.Core {SdkVersion} loaded with {loaded} profiles");
    }

    public int LiveControllers
    {
        get { lock (_lock) return _controllers.Count; }
    }

    /// <summary>True when the SDK's same-version driver manifest marker is present.</summary>
    public static bool IsDriverInstalled()
    {
        try
        {
            using var key = Registry.LocalMachine.OpenSubKey(ManifestRegPath, writable: false);
            return key?.GetValue(ManifestRegValue) is string s && s.Length > 0;
        }
        catch
        {
            return false;
        }
    }

    public static bool IsElevated()
    {
        try
        {
            using var identity = WindowsIdentity.GetCurrent();
            return new WindowsPrincipal(identity).IsInRole(WindowsBuiltInRole.Administrator);
        }
        catch
        {
            return false;
        }
    }

    /// <summary>Install the driver if needed. Idempotent; fast when already installed.</summary>
    public Protocol.Status EnsureDriver(out string error)
    {
        error = "";
        lock (_lock)
        {
            if (_driverEnsured)
                return Protocol.Status.Ok;
            if (!IsElevated())
            {
                error = "HIDMaestro driver installation requires administrator rights.";
                return Protocol.Status.NotElevated;
            }
            try
            {
                var sw = System.Diagnostics.Stopwatch.StartNew();
                _context.InstallDriver();
                _driverEnsured = true;
                Log.Info($"HIDMaestro driver ready in {sw.ElapsedMilliseconds} ms");
                return Protocol.Status.Ok;
            }
            catch (Exception ex)
            {
                error = ex.Message;
                Log.Error($"InstallDriver failed: {ex}");
                return Protocol.Status.DriverInstallFailed;
            }
        }
    }

    public Protocol.Status Create(int index, string profileId, string identityKey, out Protocol.PackingPlan? plan, out string error)
    {
        plan = null;
        error = "";

        var status = EnsureDriver(out error);
        if (status != Protocol.Status.Ok)
            return status;

        var profile = _context.GetProfile(profileId);
        if (profile is null)
        {
            error = $"Unknown HIDMaestro profile '{profileId}'.";
            return Protocol.Status.UnknownProfile;
        }

        lock (_lock)
        {
            if (_controllers.ContainsKey(index))
            {
                error = $"Controller index {index} is already in use.";
                return Protocol.Status.IndexInUse;
            }

            try
            {
                plan = PackingPlanBuilder.Build(profile);
            }
            catch (Exception ex)
            {
                error = $"Failed to build packing plan: {ex.Message}";
                Log.Error(error);
                return Protocol.Status.InternalError;
            }

            try
            {
                var sw = System.Diagnostics.Stopwatch.StartNew();
                var key = string.IsNullOrEmpty(identityKey) ? $"sunshine-slot{index}" : identityKey;
                var controller = _context.CreateControllerAt(index, profile, key);
                _controllers[index] = controller;
                Log.Info($"Created controller {index} as '{profile.Id}' ({key}) in {sw.ElapsedMilliseconds} ms");
                return Protocol.Status.Ok;
            }
            catch (Exception ex)
            {
                plan = null;
                error = ex.Message;
                Log.Error($"CreateControllerAt({index}, {profile.Id}) failed: {ex}");
                return Protocol.Status.ControllerCreateFailed;
            }
        }
    }

    public Protocol.Status Destroy(int index)
    {
        HMController? controller;
        lock (_lock)
        {
            if (!_controllers.Remove(index, out controller))
                return Protocol.Status.NotFound;
        }
        try
        {
            controller.Dispose();
            Log.Info($"Destroyed controller {index}");
        }
        catch (Exception ex)
        {
            Log.Warn($"Dispose of controller {index} threw: {ex.Message}");
        }
        return Protocol.Status.Ok;
    }

    public void Dispose()
    {
        List<HMController> controllers;
        lock (_lock)
        {
            controllers = _controllers.Values.ToList();
            _controllers.Clear();
        }
        foreach (var controller in controllers)
        {
            try { controller.Dispose(); } catch (Exception ex) { Log.Warn($"Dispose threw: {ex.Message}"); }
        }
        try { _context.Dispose(); } catch (Exception ex) { Log.Warn($"HMContext.Dispose threw: {ex.Message}"); }
    }
}

internal static class Log
{
    private static readonly object Sync = new();

    public static void Info(string message) => Write("Info", message);
    public static void Warn(string message) => Write("Warning", message);
    public static void Error(string message) => Write("Error", message);

    // Sunshine redirects the broker's stderr into its own log file, so keep the format close
    // to Sunshine's "[time]: Level: message" lines.
    private static void Write(string level, string message)
    {
        lock (Sync)
        {
            Console.Error.WriteLine($"[{DateTime.Now:yyyy-MM-dd HH:mm:ss.fff}]: {level}: hidmaestro-broker: {message}");
        }
    }
}
