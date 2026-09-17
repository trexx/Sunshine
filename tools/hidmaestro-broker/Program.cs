// Entry point for the Sunshine HIDMaestro broker.
//
// Usage:
//   sunshine-hidmaestro-broker --pipe <name> --parent-pid <pid>
//       Serve lifecycle requests from Sunshine until told to shut down or the parent exits.
//   sunshine-hidmaestro-broker --dump-plan <profile-id>
//       Print the packing plan for a profile as JSON (no driver or elevation needed).
//   sunshine-hidmaestro-broker --list-profiles
//       Print every profile id the embedded catalog contains.

using System.Diagnostics;
using System.Text.Json;
using HIDMaestro;

namespace Sunshine.HidMaestroBroker;

internal static class Program
{
    /// <summary>Broker build number reported in the Hello response.</summary>
    public const uint BrokerVersion = 1;

    private static int Main(string[] args)
    {
        string? pipeName = null;
        int parentPid = 0;
        string? dumpPlan = null;
        bool listProfiles = false;

        for (int i = 0; i < args.Length; i++)
        {
            switch (args[i])
            {
                case "--pipe" when i + 1 < args.Length:
                    pipeName = args[++i];
                    break;
                case "--parent-pid" when i + 1 < args.Length:
                    parentPid = int.Parse(args[++i]);
                    break;
                case "--dump-plan" when i + 1 < args.Length:
                    dumpPlan = args[++i];
                    break;
                case "--list-profiles":
                    listProfiles = true;
                    break;
                default:
                    Console.Error.WriteLine($"Unknown argument: {args[i]}");
                    return 2;
            }
        }

        if (dumpPlan is not null || listProfiles)
            return RunOffline(dumpPlan, listProfiles);

        if (string.IsNullOrEmpty(pipeName))
        {
            Console.Error.WriteLine("Usage: sunshine-hidmaestro-broker --pipe <name> --parent-pid <pid>");
            return 2;
        }

        return RunServer(pipeName, parentPid);
    }

    private static int RunOffline(string? dumpPlan, bool listProfiles)
    {
        using var context = new HMContext();
        context.LoadDefaultProfiles();

        if (listProfiles)
        {
            foreach (var profile in context.AllProfiles.OrderBy(p => p.Id, StringComparer.Ordinal))
                Console.WriteLine($"{profile.Id}\t{profile.VendorId:X4}:{profile.ProductId:X4}\t{profile.Name}");
        }

        if (dumpPlan is not null)
        {
            var profile = context.GetProfile(dumpPlan);
            if (profile is null)
            {
                Console.Error.WriteLine($"Unknown profile '{dumpPlan}'");
                return 1;
            }
            var plan = PackingPlanBuilder.Build(profile);
            var json = new
            {
                profile = profile.Id,
                vid = profile.VendorId,
                pid = profile.ProductId,
                mode = plan.Mode.ToString(),
                report_id = plan.ReportId,
                data_size = plan.DataSize,
                packs_gip = plan.PacksGip,
                hat_positions = plan.HatPositions,
                hat_null = plan.HatNull,
                y_axis_hid_down = plan.YAxisHidDown,
                lx = FieldJson(plan.Lx),
                ly = FieldJson(plan.Ly),
                rx = FieldJson(plan.Rx),
                ry = FieldJson(plan.Ry),
                lt = FieldJson(plan.Lt),
                rt = FieldJson(plan.Rt),
                combined_z = FieldJson(plan.CombinedZ),
                hat = FieldJson(plan.Hat),
                buttons = plan.Buttons.Select(FieldJson).ToArray(),
                role_to_button = plan.RoleToButton.Select(b => (int)b).ToArray(),
            };
            Console.WriteLine(JsonSerializer.Serialize(json, new JsonSerializerOptions { WriteIndented = true }));
        }
        return 0;
    }

    private static object? FieldJson(Protocol.Field f)
        => f.Present ? new { bit_offset = f.BitOffset, bit_size = f.BitSize, logical_min = f.LogicalMin, logical_max = f.LogicalMax } : null;

    private static int RunServer(string pipeName, int parentPid)
    {
        using var shutdown = new CancellationTokenSource();
        ControllerHost host;
        try
        {
            host = new ControllerHost();
        }
        catch (Exception ex)
        {
            Log.Error($"Failed to initialize HIDMaestro SDK: {ex}");
            return 1;
        }

        using (host)
        {
            // Exit when Sunshine goes away even if the job object did not take us down.
            if (parentPid > 0)
            {
                try
                {
                    var parent = Process.GetProcessById(parentPid);
                    parent.EnableRaisingEvents = true;
                    parent.Exited += (_, _) =>
                    {
                        Log.Info($"Parent process {parentPid} exited; shutting down");
                        shutdown.Cancel();
                    };
                    if (parent.HasExited)
                        shutdown.Cancel();
                }
                catch (Exception ex)
                {
                    Log.Warn($"Cannot watch parent process {parentPid}: {ex.Message}");
                }
            }

            Console.CancelKeyPress += (_, e) =>
            {
                e.Cancel = true;
                shutdown.Cancel();
            };

            var server = new PipeServer(pipeName, host, shutdown);
            try
            {
                server.RunAsync().GetAwaiter().GetResult();
            }
            catch (Exception ex)
            {
                Log.Error($"Pipe server crashed: {ex}");
                return 1;
            }
            Log.Info("Disposing controllers");
        }
        return 0;
    }
}
