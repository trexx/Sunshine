// Builds the per-controller packing plan Sunshine uses to write input frames itself.
//
// The plan is derived from HIDMaestro's own descriptor parser (HidReportBuilder) plus the
// profile's authored layout / button map, so Sunshine's generic bitfield writer produces the
// same bytes the SDK's SubmitState would.

using HIDMaestro;
using HIDMaestro.Internal;

namespace Sunshine.HidMaestroBroker;

internal static class PackingPlanBuilder
{
    // Profiles Sunshine packs with a fixed, hand-written layout instead of the generic plan.
    private static readonly Dictionary<string, Protocol.PackMode> FixedModes = new(StringComparer.OrdinalIgnoreCase)
    {
        ["dualshock-4-v2"] = Protocol.PackMode.SonyDs4,
        ["dualshock-4-v1"] = Protocol.PackMode.SonyDs4,
        ["dualsense"] = Protocol.PackMode.SonyDualSense,
        ["dualsense-edge"] = Protocol.PackMode.SonyDualSense,
        ["switch-pro"] = Protocol.PackMode.SwitchPro,
    };

    public static Protocol.PackingPlan Build(HMProfile profile)
    {
        var descriptor = profile.GetDescriptorBytes()
            ?? throw new InvalidOperationException($"Profile '{profile.Id}' has no HID descriptor.");

        var builder = HidReportBuilder.Parse(descriptor, profile.AxisMap, 0);
        builder.ButtonMap = profile.ButtonMap;
        if (profile.Layout is not null and not HMUnspecifiedLayout)
        {
            try
            {
                HMLayoutValidator.Validate(profile.Layout, builder);
                builder.ApplyLayoutSemantics(profile.Layout);
            }
            catch (Exception ex)
            {
                Log.Warn($"Layout semantics for '{profile.Id}' not applied: {ex.Message}");
            }
        }

        var plan = new Protocol.PackingPlan
        {
            Mode = FixedModes.TryGetValue(profile.Id, out var fixedMode) ? fixedMode : Protocol.PackMode.HidGeneric,
            ReportId = builder.InputReportId,
            DataSize = (ushort)((builder.InputReportBitSize + 7) / 8),
            PacksGip = RequiresXusbCompanion(profile),
            YAxisHidDown = true,
            Lx = ToField(builder.LeftStickX),
            Ly = ToField(builder.LeftStickY),
            Rx = ToField(builder.RightStickX),
            Ry = ToField(builder.RightStickY),
            Lt = ToField(builder.LeftTrigger),
            Rt = ToField(builder.RightTrigger),
            CombinedZ = ToField(builder.CombinedTrigger),
            Hat = ToField(builder.HatSwitch),
        };

        if (builder.HatSwitch is { } hat)
        {
            int positions = hat.LogicalMax - hat.LogicalMin + 1;
            plan.HatPositions = (byte)Math.Clamp(positions, 0, 255);
            plan.HatNull = (byte)(hat.LogicalMin > 0 ? 0 : hat.LogicalMax + 1);
        }

        foreach (var button in builder.Buttons.Take(Protocol.MaxButtons))
            plan.Buttons.Add(ToField(button));

        MapRoles(profile, builder, plan);
        return plan;
    }

    private static Protocol.Field ToField(HidReportBuilder.InputField? field)
    {
        if (field is null)
            return default;
        return new Protocol.Field
        {
            BitOffset = (ushort)field.BitOffset,
            BitSize = (byte)field.BitSize,
            Present = true,
            LogicalMin = field.LogicalMin,
            LogicalMax = field.LogicalMax,
        };
    }

    // Mirrors ControllerProfile.RequiresXusbCompanion (internal to the SDK): Xbox-branded
    // Microsoft profiles that are not bound to an upper filter get an XUSB companion, which
    // reads the 14-byte GIP slice of the shared input section.
    private static bool RequiresXusbCompanion(HMProfile profile)
    {
        if (profile.VendorId != 0x045E || profile.DriverMode is not null)
            return false;
        const StringComparison cmp = StringComparison.OrdinalIgnoreCase;
        return profile.Id.Contains("xbox", cmp)
            || profile.Name.Contains("xbox", cmp)
            || (profile.ProductString?.Contains("xbox", cmp) ?? false);
    }

    private static void MapRoles(HMProfile profile, HidReportBuilder builder, Protocol.PackingPlan plan)
    {
        int count = plan.Buttons.Count;

        void Set(Protocol.ButtonRole role, int index)
        {
            if (index < 0 || index >= count)
                return;
            if (plan.RoleToButton[(int)role] == Protocol.RoleUnmapped)
                plan.RoleToButton[(int)role] = (byte)index;
        }

        // 1. Authored gamepad layout: the most reliable source of semantic roles.
        if (profile.Layout is HMGamepadLayout layout)
        {
            foreach (var binding in layout.FaceButtons.Concat(layout.ShoulderButtons).Concat(layout.SystemButtons).Concat(layout.ExtraButtons))
            {
                var role = RoleFor(binding.Role);
                if (role is { } r)
                    Set(r, binding.ButtonIndex);
            }
            foreach (var stick in layout.Sticks)
            {
                if (stick.ClickButton is { } click)
                    Set(stick.Side == HMStickSide.Left ? Protocol.ButtonRole.LeftStick : Protocol.ButtonRole.RightStick, click);
            }
        }

        // 2. Profile button map: HMButton bit position -> descriptor button index.
        //    HMButton order: A, B, X, Y, LB, RB, Back, Start, LS, RS, Guide, Touchpad,
        //    Share, RightPaddle, LeftPaddle, Misc1, RightPaddle2, LeftPaddle2.
        int[]? map = builder.ButtonMap;
        int Mapped(int bit)
        {
            if (map is null)
                return bit;
            return bit < map.Length ? map[bit] : -1;
        }

        Set(Protocol.ButtonRole.A, Mapped(0));
        Set(Protocol.ButtonRole.B, Mapped(1));
        Set(Protocol.ButtonRole.X, Mapped(2));
        Set(Protocol.ButtonRole.Y, Mapped(3));
        Set(Protocol.ButtonRole.LeftBumper, Mapped(4));
        Set(Protocol.ButtonRole.RightBumper, Mapped(5));
        Set(Protocol.ButtonRole.Back, Mapped(6));
        Set(Protocol.ButtonRole.Start, Mapped(7));
        Set(Protocol.ButtonRole.LeftStick, Mapped(8));
        Set(Protocol.ButtonRole.RightStick, Mapped(9));
        Set(Protocol.ButtonRole.Guide, Mapped(10));
        Set(Protocol.ButtonRole.Misc, Mapped(12));   // Share
        Set(Protocol.ButtonRole.Misc, Mapped(15));   // Misc1
        Set(Protocol.ButtonRole.Paddle1, Mapped(13));
        Set(Protocol.ButtonRole.Paddle2, Mapped(14));
        Set(Protocol.ButtonRole.Paddle3, Mapped(16));
        Set(Protocol.ButtonRole.Paddle4, Mapped(17));
    }

    private static Protocol.ButtonRole? RoleFor(HMButtonRole role) => role switch
    {
        HMButtonRole.FaceA or HMButtonRole.FaceCross => Protocol.ButtonRole.A,
        HMButtonRole.FaceB or HMButtonRole.FaceCircle => Protocol.ButtonRole.B,
        HMButtonRole.FaceX or HMButtonRole.FaceSquare => Protocol.ButtonRole.X,
        HMButtonRole.FaceY or HMButtonRole.FaceTriangle => Protocol.ButtonRole.Y,
        HMButtonRole.LeftBumper => Protocol.ButtonRole.LeftBumper,
        HMButtonRole.RightBumper => Protocol.ButtonRole.RightBumper,
        HMButtonRole.LeftStickClick => Protocol.ButtonRole.LeftStick,
        HMButtonRole.RightStickClick => Protocol.ButtonRole.RightStick,
        HMButtonRole.Back or HMButtonRole.View => Protocol.ButtonRole.Back,
        HMButtonRole.Start or HMButtonRole.Options or HMButtonRole.Menu => Protocol.ButtonRole.Start,
        HMButtonRole.Guide or HMButtonRole.Home or HMButtonRole.Ps or HMButtonRole.Xbox => Protocol.ButtonRole.Guide,
        HMButtonRole.Share or HMButtonRole.Capture or HMButtonRole.Mute or HMButtonRole.Misc1 => Protocol.ButtonRole.Misc,
        HMButtonRole.PaddleP1 => Protocol.ButtonRole.Paddle1,
        HMButtonRole.PaddleP2 => Protocol.ButtonRole.Paddle2,
        HMButtonRole.PaddleP3 => Protocol.ButtonRole.Paddle3,
        HMButtonRole.PaddleP4 => Protocol.ButtonRole.Paddle4,
        _ => null,
    };
}
