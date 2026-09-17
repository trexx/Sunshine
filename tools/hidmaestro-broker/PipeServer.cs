// Message-mode named pipe server: one request/response per connection, single-threaded.
//
// Sunshine connects, transacts one fixed-size message and disconnects (TransactNamedPipe), so
// the server accepts, reads one message, answers, waits for the client to drain and
// disconnects. PipeOptions.CurrentUserOnly restricts clients to the same user as this
// process (SYSTEM under the Sunshine service, the elevated user otherwise) and rejects
// remote clients.

using System.IO.Pipes;

namespace Sunshine.HidMaestroBroker;

internal sealed class PipeServer
{
    private readonly string _pipeName;
    private readonly ControllerHost _host;
    private readonly CancellationTokenSource _shutdown;

    public PipeServer(string pipeName, ControllerHost host, CancellationTokenSource shutdown)
    {
        _pipeName = pipeName;
        _host = host;
        _shutdown = shutdown;
    }

    public async Task RunAsync()
    {
        var token = _shutdown.Token;
        var request = new byte[Protocol.MaxMessageSize];
        var response = new byte[Protocol.MaxMessageSize];

        Log.Info($"Listening on \\\\.\\pipe\\{_pipeName}");
        while (!token.IsCancellationRequested)
        {
            NamedPipeServerStream pipe;
            try
            {
                pipe = new NamedPipeServerStream(
                    _pipeName,
                    PipeDirection.InOut,
                    1,
                    PipeTransmissionMode.Message,
                    PipeOptions.Asynchronous | PipeOptions.CurrentUserOnly,
                    Protocol.MaxMessageSize,
                    Protocol.MaxMessageSize);
            }
            catch (Exception ex)
            {
                Log.Error($"CreateNamedPipe failed: {ex.Message}");
                await Task.Delay(500, token).ConfigureAwait(false);
                continue;
            }

            using (pipe)
            {
                try
                {
                    await pipe.WaitForConnectionAsync(token).ConfigureAwait(false);
                }
                catch (OperationCanceledException)
                {
                    break;
                }
                catch (Exception ex)
                {
                    Log.Warn($"WaitForConnection failed: {ex.Message}");
                    continue;
                }

                try
                {
                    int length = await ReadMessageAsync(pipe, request, token).ConfigureAwait(false);
                    if (length <= 0)
                        continue;

                    int responseLength = Dispatch(request.AsSpan(0, length), response);
                    await pipe.WriteAsync(response.AsMemory(0, responseLength), token).ConfigureAwait(false);
                    await pipe.FlushAsync(token).ConfigureAwait(false);
                    pipe.WaitForPipeDrain();
                }
                catch (OperationCanceledException)
                {
                    break;
                }
                catch (Exception ex)
                {
                    Log.Warn($"Pipe transaction failed: {ex.Message}");
                }
                finally
                {
                    try { if (pipe.IsConnected) pipe.Disconnect(); } catch { }
                }
            }
        }
        Log.Info("Pipe server stopped");
    }

    private static async Task<int> ReadMessageAsync(NamedPipeServerStream pipe, byte[] buffer, CancellationToken token)
    {
        int total = 0;
        do
        {
            if (total >= buffer.Length)
                return -1;
            int read = await pipe.ReadAsync(buffer.AsMemory(total), token).ConfigureAwait(false);
            if (read == 0)
                return total;
            total += read;
        } while (!pipe.IsMessageComplete);
        return total;
    }

    private int Dispatch(ReadOnlySpan<byte> request, byte[] response)
    {
        if (request.Length < Protocol.HeaderSize)
            return WriteSimple(response, Protocol.Status.InvalidRequest);

        var header = Protocol.Header.Read(request);
        if (header.Version != Protocol.ProtocolVersion)
        {
            Log.Warn($"Rejected request with protocol version {header.Version} (expected {Protocol.ProtocolVersion})");
            return WriteSimple(response, Protocol.Status.UnsupportedVersion);
        }
        if (header.Size != request.Length)
            return WriteSimple(response, Protocol.Status.InvalidRequest);

        switch ((Protocol.RequestType)header.Type)
        {
            case Protocol.RequestType.Hello when request.Length == Protocol.HelloRequestSize:
                return WriteHello(response);

            case Protocol.RequestType.EnsureDriver when request.Length == Protocol.EnsureDriverRequestSize:
            {
                var status = _host.EnsureDriver(out var error);
                var w = new Protocol.SpanWriter(response);
                Protocol.WriteResponseHeader(ref w, Protocol.EnsureDriverResponseSize, status);
                w.FixedString(error, 256);
                return w.Position;
            }

            case Protocol.RequestType.CreateController when request.Length == Protocol.CreateControllerRequestSize:
            {
                var r = new Protocol.SpanReader(request, Protocol.HeaderSize);
                int index = (int)r.U32();
                string profileId = r.FixedString(64);
                string identityKey = r.FixedString(64);

                var status = _host.Create(index, profileId, identityKey, out var plan, out var error);
                var w = new Protocol.SpanWriter(response);
                Protocol.WriteResponseHeader(ref w, Protocol.CreateControllerResponseSize, status);
                if (plan is not null)
                    plan.Write(ref w);
                else
                    w.Zero(Protocol.PlanSize);
                w.FixedString(error, 256);
                return w.Position;
            }

            case Protocol.RequestType.DestroyController when request.Length == Protocol.DestroyControllerRequestSize:
            {
                var r = new Protocol.SpanReader(request, Protocol.HeaderSize);
                int index = (int)r.U32();
                return WriteSimple(response, _host.Destroy(index));
            }

            case Protocol.RequestType.Status when request.Length == Protocol.StatusRequestSize:
            {
                var w = new Protocol.SpanWriter(response);
                Protocol.WriteResponseHeader(ref w, Protocol.StatusResponseSize, Protocol.Status.Ok);
                w.U8(ControllerHost.IsDriverInstalled() ? (byte)1 : (byte)0);
                w.Zero(3);
                w.U32((uint)_host.LiveControllers);
                w.FixedString(_host.SdkVersion, 32);
                return w.Position;
            }

            case Protocol.RequestType.Shutdown when request.Length == Protocol.ShutdownRequestSize:
                Log.Info("Shutdown requested");
                _shutdown.Cancel();
                return WriteSimple(response, Protocol.Status.Ok);

            default:
                Log.Warn($"Rejected request type {header.Type} with size {request.Length}");
                return WriteSimple(response, Protocol.Status.InvalidRequest);
        }
    }

    private int WriteHello(byte[] response)
    {
        var w = new Protocol.SpanWriter(response);
        Protocol.WriteResponseHeader(ref w, Protocol.HelloResponseSize, Protocol.Status.Ok);
        w.U32(Program.BrokerVersion);
        w.FixedString(_host.SdkVersion, 32);
        w.U8(ControllerHost.IsDriverInstalled() ? (byte)1 : (byte)0);
        w.U8(ControllerHost.IsElevated() ? (byte)1 : (byte)0);
        w.Zero(2);
        return w.Position;
    }

    private static int WriteSimple(byte[] response, Protocol.Status status)
    {
        var w = new Protocol.SpanWriter(response);
        Protocol.WriteResponseHeader(ref w, Protocol.SimpleResponseSize, status);
        return w.Position;
    }
}
