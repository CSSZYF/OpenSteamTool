using System;
using System.Collections.Generic;
using System.IO;
using System.Net.Http;
using System.Net.WebSockets;
using System.Text;
using System.Threading;
using System.Threading.Tasks;
using System.Web.Script.Serialization;

namespace DaveLauncher
{
    public sealed class SteamState
    {
        public bool Offline;
        public bool Connected;
        public bool ServicesReady;
    }

    public sealed class NativeSteamControl
    {
        private const int MaximumResponseBytes = 1024 * 1024;
        private const int CommandTimeoutMilliseconds = 15000;
        private readonly Uri targetListUri;

        private const string ReadStateExpression =
            "(function(){" +
            "var a=window.App;" +
            "if(!a||typeof a.GetServicesInitialized!=='function'||" +
            "typeof a.BIsOfflineMode!=='function'||!a.cm||" +
            "typeof a.cm.BConnectedToServer!=='function')" +
            "throw new Error('Steam state API is not ready');" +
            "var o=a.BIsOfflineMode(),c=a.cm.BConnectedToServer(),s=a.GetServicesInitialized();" +
            "if(typeof o!=='boolean'||typeof c!=='boolean'||typeof s!=='boolean')" +
            "throw new Error('Steam state API returned an unexpected type');" +
            "return {offline:o,connected:c,servicesReady:s};" +
            "})()";

        public NativeSteamControl(string endpoint)
        {
            Uri uri;
            if (!Uri.TryCreate(endpoint, UriKind.Absolute, out uri) ||
                !IsExpectedAuthority(uri, "http") || uri.AbsolutePath != "/" ||
                uri.Query.Length != 0 || uri.Fragment.Length != 0)
            {
                throw new ArgumentException(
                    "The Steam endpoint must be http://127.0.0.1:8080/.", "endpoint");
            }
            targetListUri = new Uri(uri, "/json/list");
        }

        public async Task<SteamState> ReadStateAsync(CancellationToken cancellationToken)
        {
            Dictionary<string, object> value = await EvaluateAsync(
                ReadStateExpression, cancellationToken).ConfigureAwait(false);
            return new SteamState
            {
                Offline = ReadBoolean(value, "offline"),
                Connected = ReadBoolean(value, "connected"),
                ServicesReady = ReadBoolean(value, "servicesReady")
            };
        }

        public Task GoOfflineAsync(CancellationToken cancellationToken)
        {
            return ChangeModeAsync(true, cancellationToken);
        }

        public Task GoOnlineAsync(CancellationToken cancellationToken)
        {
            return ChangeModeAsync(false, cancellationToken);
        }

        internal Task<Dictionary<string, object>> InstallPlayGateAsync(
            string script, bool noRunningApps, CancellationToken cancellationToken)
        {
            if (String.IsNullOrWhiteSpace(script) || script.Length > 128 * 1024)
                throw new InvalidOperationException("Steam 开始游戏控制脚本缺失或过大。");
            string prerequisite = "window.__OSTOfflinePlayPrerequisite={noRunningApps:" +
                (noRunningApps ? "true" : "false") + ",sampledAt:Date.now()};\n";
            return EvaluateAsync(prerequisite + script, cancellationToken);
        }

        internal Task<Dictionary<string, object>> ReadGateErrorAsync(CancellationToken cancellationToken)
        {
            // Return only our own gate status, never Steam UI objects, account data or tickets.
            const string expression = "(function(){var g=window.__OSTOfflinePlayGate;" +
                "return {installed:!!g,error:g&&typeof g.lastError==='string'?g.lastError:''," +
                "eventId:g&&typeof g.generation==='number'?g.generation:0};})()";
            return EvaluateAsync(expression, cancellationToken);
        }

        private async Task ChangeModeAsync(bool offline, CancellationToken cancellationToken)
        {
            string method = offline ? "GoOffline" : "GoOnline";
            string desired = offline ? "true" : "false";
            string expression =
                "(function(){var a=window.App;" +
                "if(!a||typeof a.BIsOfflineMode!=='function'||" +
                "typeof a.GetServicesInitialized!=='function'||" +
                "a.GetServicesInitialized()!==true||" +
                "typeof window.SteamClient==='undefined'||!SteamClient.User||" +
                "typeof SteamClient.User." + method + "!=='function')" +
                "throw new Error('Steam mode API is not ready');" +
                "var current=a.BIsOfflineMode();" +
                "if(typeof current!=='boolean')" +
                "throw new Error('Steam offline state is unavailable');" +
                "if(current!==" + desired + ")SteamClient.User." + method + "();" +
                "return {requested:true};})()";
            Dictionary<string, object> value = await EvaluateAsync(
                expression, cancellationToken).ConfigureAwait(false);
            if (!ReadBoolean(value, "requested"))
                throw new InvalidOperationException("Steam did not acknowledge the mode request.");
        }

        internal async Task<Dictionary<string, object>> EvaluateAsync(
            string expression, CancellationToken cancellationToken)
        {
            using (CancellationTokenSource timeout =
                CancellationTokenSource.CreateLinkedTokenSource(cancellationToken))
            {
                timeout.CancelAfter(CommandTimeoutMilliseconds);
                try
                {
                    Uri websocketUri = await FindTargetAsync(timeout.Token).ConfigureAwait(false);
                    using (ClientWebSocket socket = new ClientWebSocket())
                    {
                        socket.Options.Proxy = null;
                        await socket.ConnectAsync(websocketUri, timeout.Token).ConfigureAwait(false);
                        JavaScriptSerializer serializer = NewSerializer();
                        Dictionary<string, object> parameters = new Dictionary<string, object>
                        {
                            { "expression", expression },
                            { "returnByValue", true },
                            { "awaitPromise", true }
                        };
                        Dictionary<string, object> command = new Dictionary<string, object>
                        {
                            { "id", 1 },
                            { "method", "Runtime.evaluate" },
                            { "params", parameters }
                        };
                        byte[] request = Encoding.UTF8.GetBytes(serializer.Serialize(command));
                        await socket.SendAsync(new ArraySegment<byte>(request),
                            WebSocketMessageType.Text, true, timeout.Token).ConfigureAwait(false);
                        return await ReadEvaluationResultAsync(socket, timeout.Token).ConfigureAwait(false);
                    }
                }
                catch (OperationCanceledException)
                {
                    if (cancellationToken.IsCancellationRequested)
                        throw;
                    throw new TimeoutException("The native Steam command timed out after 15 seconds.");
                }
            }
        }

        private async Task<Uri> FindTargetAsync(CancellationToken cancellationToken)
        {
            using (HttpClientHandler handler = new HttpClientHandler
            {
                UseProxy = false,
                AllowAutoRedirect = false
            })
            using (HttpClient client = new HttpClient(handler))
            {
                client.Timeout = Timeout.InfiniteTimeSpan;
                using (HttpResponseMessage response = await client.GetAsync(targetListUri,
                    HttpCompletionOption.ResponseHeadersRead, cancellationToken).ConfigureAwait(false))
                {
                    if (!response.IsSuccessStatusCode)
                        throw new InvalidOperationException("The local Steam debugger returned an HTTP error.");
                    if (response.Content.Headers.ContentLength.HasValue &&
                        response.Content.Headers.ContentLength.Value > MaximumResponseBytes)
                        throw new InvalidDataException("The Steam target list exceeds the response limit.");
                    using (Stream stream = await response.Content.ReadAsStreamAsync().ConfigureAwait(false))
                    {
                        string json = await ReadBoundedTextAsync(stream, cancellationToken).ConfigureAwait(false);
                        object[] targets = ParseJson(json) as object[];
                        if (targets == null)
                            throw new InvalidDataException("The Steam target list is not a JSON array.");
                        Uri found = null;
                        foreach (object item in targets)
                        {
                            Dictionary<string, object> target = item as Dictionary<string, object>;
                            if (target == null || ReadOptionalString(target, "type") != "page")
                                continue;
                            Uri pageUri;
                            if (!Uri.TryCreate(ReadOptionalString(target, "url"), UriKind.Absolute,
                                out pageUri) || pageUri.Scheme != "https" ||
                                pageUri.Host != "steamloopback.host" || pageUri.Port != 443 ||
                                pageUri.UserInfo.Length != 0 || pageUri.AbsolutePath != "/index.html")
                                continue;
                            Uri websocketUri;
                            if (!Uri.TryCreate(ReadOptionalString(target, "webSocketDebuggerUrl"),
                                UriKind.Absolute, out websocketUri) ||
                                !IsExpectedAuthority(websocketUri, "ws") ||
                                !websocketUri.AbsolutePath.StartsWith("/devtools/page/", StringComparison.Ordinal) ||
                                websocketUri.AbsolutePath.Length <= "/devtools/page/".Length ||
                                websocketUri.Query.Length != 0 || websocketUri.Fragment.Length != 0)
                                throw new InvalidDataException("The Steam debugger returned an unexpected WebSocket address.");
                            if (found != null)
                                throw new InvalidOperationException("More than one Steam main UI target was found.");
                            found = websocketUri;
                        }
                        if (found == null)
                            throw new InvalidOperationException("The local Steam main UI debugger target is not ready.");
                        return found;
                    }
                }
            }
        }

        private static async Task<Dictionary<string, object>> ReadEvaluationResultAsync(
            ClientWebSocket socket, CancellationToken cancellationToken)
        {
            byte[] buffer = new byte[8192];
            int totalBytes = 0;
            while (true)
            {
                using (MemoryStream message = new MemoryStream())
                {
                    WebSocketReceiveResult received;
                    do
                    {
                        received = await socket.ReceiveAsync(new ArraySegment<byte>(buffer),
                            cancellationToken).ConfigureAwait(false);
                        if (received.MessageType != WebSocketMessageType.Text)
                            throw new InvalidDataException("The Steam debugger closed the connection or returned non-text data.");
                        totalBytes += received.Count;
                        if (totalBytes > MaximumResponseBytes)
                            throw new InvalidDataException("The Steam debugger exceeds the response limit.");
                        message.Write(buffer, 0, received.Count);
                    }
                    while (!received.EndOfMessage);
                    string json = new UTF8Encoding(false, true).GetString(message.ToArray());
                    Dictionary<string, object> reply = ParseJson(json)
                        as Dictionary<string, object>;
                    if (reply == null)
                        throw new InvalidDataException("The Steam debugger response is not a JSON object.");
                    object id;
                    if (!reply.TryGetValue("id", out id))
                    {
                        if (ReadOptionalString(reply, "method") != null)
                            continue;
                        throw new InvalidDataException("The Steam debugger response has no command identifier.");
                    }
                    if (!(id is int) || (int)id != 1)
                        throw new InvalidDataException("The Steam debugger response has an unexpected command identifier.");
                    if (reply.ContainsKey("error"))
                        throw new InvalidOperationException("The Steam debugger rejected the native command.");
                    Dictionary<string, object> result = ReadObject(reply, "result");
                    if (result.ContainsKey("exceptionDetails"))
                        throw new InvalidOperationException("The native Steam API is unavailable or rejected the command.");
                    Dictionary<string, object> remoteObject = ReadObject(result, "result");
                    if (ReadOptionalString(remoteObject, "type") != "object")
                        throw new InvalidDataException("The native Steam API returned an unexpected result type.");
                    return ReadObject(remoteObject, "value");
                }
            }
        }

        private static async Task<string> ReadBoundedTextAsync(
            Stream stream, CancellationToken cancellationToken)
        {
            byte[] buffer = new byte[8192];
            using (MemoryStream output = new MemoryStream())
            {
                int count;
                while ((count = await stream.ReadAsync(buffer, 0, buffer.Length,
                    cancellationToken).ConfigureAwait(false)) != 0)
                {
                    if (output.Length + count > MaximumResponseBytes)
                        throw new InvalidDataException("The Steam target list exceeds the response limit.");
                    output.Write(buffer, 0, count);
                }
                return new UTF8Encoding(false, true).GetString(output.ToArray());
            }
        }

        private static bool IsExpectedAuthority(Uri uri, string scheme)
        {
            return uri.Scheme == scheme && uri.Host == "127.0.0.1" && uri.Port == 8080 &&
                uri.Authority == "127.0.0.1:8080" && uri.UserInfo.Length == 0;
        }

        private static JavaScriptSerializer NewSerializer()
        {
            return new JavaScriptSerializer { MaxJsonLength = MaximumResponseBytes, RecursionLimit = 64 };
        }

        private static object ParseJson(string json)
        {
            try { return NewSerializer().DeserializeObject(json); }
            catch (ArgumentException) { throw new InvalidDataException("The local Steam response contains invalid JSON."); }
            catch (InvalidOperationException) { throw new InvalidDataException("The local Steam response contains invalid JSON."); }
        }

        private static string ReadOptionalString(Dictionary<string, object> value, string key)
        {
            object result;
            return value.TryGetValue(key, out result) ? result as string : null;
        }

        private static Dictionary<string, object> ReadObject(
            Dictionary<string, object> value, string key)
        {
            object result;
            Dictionary<string, object> dictionary;
            if (!value.TryGetValue(key, out result) ||
                (dictionary = result as Dictionary<string, object>) == null)
                throw new InvalidDataException("The Steam debugger response is missing a required object.");
            return dictionary;
        }

        private static bool ReadBoolean(Dictionary<string, object> value, string key)
        {
            object result;
            if (!value.TryGetValue(key, out result) || !(result is bool))
                throw new InvalidDataException("The native Steam API returned an invalid state value.");
            return (bool)result;
        }
    }
}
