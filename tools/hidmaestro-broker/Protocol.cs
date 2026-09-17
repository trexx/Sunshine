// Wire protocol shared with Sunshine. Mirrors src/platform/hidmaestro/protocol.h byte for
// byte: little-endian, byte-packed, fixed-size messages. Any change here must be paired
// with the C++ header and a bump of ProtocolVersion.

using System.Buffers.Binary;
using System.Text;

namespace Sunshine.HidMaestroBroker;

internal static class Protocol
{
    public const uint ProtocolVersion = 1;
    public const int MaxMessageSize = 8192;
    public const int MaxButtons = 32;
    public const int RoleCount = 16;
    public const byte RoleUnmapped = 0xFF;

    public const int HeaderSize = 16;
    public const int FieldSize = 12;
    public const int PlanSize = 8 + 8 * FieldSize + 4 + MaxButtons * FieldSize + RoleCount;  // 508

    public const int HelloRequestSize = HeaderSize + 4;
    public const int HelloResponseSize = HeaderSize + 4 + 32 + 4;  // 56
    public const int EnsureDriverRequestSize = HeaderSize;
    public const int EnsureDriverResponseSize = HeaderSize + 256;  // 272
    public const int CreateControllerRequestSize = HeaderSize + 4 + 64 + 64;  // 148
    public const int CreateControllerResponseSize = HeaderSize + PlanSize + 256;  // 780
    public const int DestroyControllerRequestSize = HeaderSize + 4;
    public const int SimpleResponseSize = HeaderSize;
    public const int StatusRequestSize = HeaderSize;
    public const int StatusResponseSize = HeaderSize + 4 + 4 + 32;  // 56
    public const int ShutdownRequestSize = HeaderSize;

    public enum RequestType : uint
    {
        Hello = 1,
        EnsureDriver = 2,
        CreateController = 3,
        DestroyController = 4,
        Status = 5,
        Shutdown = 6,
    }

    public enum Status : uint
    {
        Ok = 0,
        InvalidRequest = 1,
        UnsupportedVersion = 2,
        UnknownProfile = 3,
        DriverInstallFailed = 4,
        ControllerCreateFailed = 5,
        IndexInUse = 6,
        NotFound = 7,
        NotElevated = 8,
        InternalError = 9,
    }

    public enum PackMode : byte
    {
        HidGeneric = 0,
        SonyDs4 = 1,
        SonyDualSense = 2,
        SwitchPro = 3,
    }

    /// <summary>Sunshine button roles, indexing <see cref="PackingPlan.RoleToButton"/>.</summary>
    public enum ButtonRole : byte
    {
        A = 0,
        B = 1,
        X = 2,
        Y = 3,
        LeftBumper = 4,
        RightBumper = 5,
        Back = 6,
        Start = 7,
        LeftStick = 8,
        RightStick = 9,
        Guide = 10,
        Misc = 11,
        Paddle1 = 12,
        Paddle2 = 13,
        Paddle3 = 14,
        Paddle4 = 15,
    }

    public readonly record struct Header(uint Version, uint Size, uint Type, uint Reserved)
    {
        public static Header Read(ReadOnlySpan<byte> buffer) => new(
            BinaryPrimitives.ReadUInt32LittleEndian(buffer),
            BinaryPrimitives.ReadUInt32LittleEndian(buffer[4..]),
            BinaryPrimitives.ReadUInt32LittleEndian(buffer[8..]),
            BinaryPrimitives.ReadUInt32LittleEndian(buffer[12..]));
    }

    public struct Field
    {
        public ushort BitOffset;
        public byte BitSize;
        public bool Present;
        public int LogicalMin;
        public int LogicalMax;

        public void Write(ref SpanWriter w)
        {
            w.U16(BitOffset);
            w.U8(BitSize);
            w.U8(Present ? (byte)1 : (byte)0);
            w.I32(LogicalMin);
            w.I32(LogicalMax);
        }
    }

    public sealed class PackingPlan
    {
        public PackMode Mode;
        public byte ReportId;
        public ushort DataSize;
        public bool PacksGip;
        public byte HatPositions;
        public byte HatNull;
        public bool YAxisHidDown = true;
        public Field Lx, Ly, Rx, Ry, Lt, Rt, CombinedZ, Hat;
        public readonly List<Field> Buttons = new();
        public readonly byte[] RoleToButton = Enumerable.Repeat(RoleUnmapped, RoleCount).ToArray();

        public void Write(ref SpanWriter w)
        {
            int start = w.Position;
            w.U8((byte)Mode);
            w.U8(ReportId);
            w.U16(DataSize);
            w.U8(PacksGip ? (byte)1 : (byte)0);
            w.U8(HatPositions);
            w.U8(HatNull);
            w.U8(YAxisHidDown ? (byte)1 : (byte)0);
            Lx.Write(ref w);
            Ly.Write(ref w);
            Rx.Write(ref w);
            Ry.Write(ref w);
            Lt.Write(ref w);
            Rt.Write(ref w);
            CombinedZ.Write(ref w);
            Hat.Write(ref w);
            int count = Math.Min(Buttons.Count, MaxButtons);
            w.U8((byte)count);
            w.Zero(3);
            for (int i = 0; i < MaxButtons; i++)
            {
                if (i < count)
                {
                    var f = Buttons[i];
                    f.Write(ref w);
                }
                else
                {
                    w.Zero(FieldSize);
                }
            }
            w.Bytes(RoleToButton);
            if (w.Position - start != PlanSize)
                throw new InvalidOperationException($"PackingPlan serialized to {w.Position - start} bytes, expected {PlanSize}.");
        }
    }

    public static void WriteResponseHeader(ref SpanWriter w, uint size, Status status)
    {
        w.U32(ProtocolVersion);
        w.U32(size);
        w.U32((uint)status);
        w.U32(0);
    }

    /// <summary>Minimal little-endian span writer with fixed-width string support.</summary>
    public ref struct SpanWriter
    {
        private readonly Span<byte> _buffer;
        public int Position { get; private set; }

        public SpanWriter(Span<byte> buffer)
        {
            _buffer = buffer;
            Position = 0;
        }

        public void U8(byte v) { _buffer[Position] = v; Position += 1; }
        public void U16(ushort v) { BinaryPrimitives.WriteUInt16LittleEndian(_buffer[Position..], v); Position += 2; }
        public void U32(uint v) { BinaryPrimitives.WriteUInt32LittleEndian(_buffer[Position..], v); Position += 4; }
        public void I32(int v) { BinaryPrimitives.WriteInt32LittleEndian(_buffer[Position..], v); Position += 4; }
        public void Zero(int count) { _buffer.Slice(Position, count).Clear(); Position += count; }
        public void Bytes(ReadOnlySpan<byte> bytes) { bytes.CopyTo(_buffer[Position..]); Position += bytes.Length; }

        /// <summary>Write a NUL-terminated UTF-8 string into a fixed-width field, truncating if needed.</summary>
        public void FixedString(string? value, int width)
        {
            var dst = _buffer.Slice(Position, width);
            dst.Clear();
            if (!string.IsNullOrEmpty(value))
            {
                int max = width - 1;
                var bytes = Encoding.UTF8.GetBytes(value);
                int n = Math.Min(bytes.Length, max);
                bytes.AsSpan(0, n).CopyTo(dst);
            }
            Position += width;
        }
    }

    /// <summary>Minimal little-endian span reader with fixed-width string support.</summary>
    public ref struct SpanReader
    {
        private readonly ReadOnlySpan<byte> _buffer;
        public int Position { get; private set; }

        public SpanReader(ReadOnlySpan<byte> buffer, int position = 0)
        {
            _buffer = buffer;
            Position = position;
        }

        public uint U32() { var v = BinaryPrimitives.ReadUInt32LittleEndian(_buffer[Position..]); Position += 4; return v; }

        public string FixedString(int width)
        {
            var span = _buffer.Slice(Position, width);
            Position += width;
            int nul = span.IndexOf((byte)0);
            if (nul >= 0) span = span[..nul];
            return Encoding.UTF8.GetString(span);
        }
    }
}
