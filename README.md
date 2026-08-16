# cppmcp — C++ MCP Server & Client Library
**[中文](docs/zh-CN/README.md)**

A C++ implementation of the [Model Context Protocol (MCP)](https://modelcontextprotocol.io/) **server and client**, conforming to MCP specification versions `2025-03-26` and `2025-06-18` (negotiated per connection). The server supports four transport modes (stdio, SSE, Streamable HTTP, local pipe). The client connects via stdio (spawn child process), Streamable HTTP, or local pipe. All communication uses JSON-RPC 2.0.


## Features

- **Server: four transport modes** — stdio, SSE (legacy HTTP), Streamable HTTP, local pipe
- **Client: three transport modes** — stdio (spawn a child server process), Streamable HTTP, local pipe
- **Fully async I/O**: asio unified event loop + llhttp HTTP parser, zero thread kidnapping, zero polling
- **Full MCP protocol (server)**: Tools, Resources (+ URI templates, subscriptions), Prompts, Completions, Logging, pagination; inputSchema/param validation
- **Server → client requests**: `request_sampling()` / `request_elicitation()` / `request_roots()` with capability gating
- **Full MCP client**: list/call tools, read resources (subscribe/unsubscribe), get prompts, completion, ping; handles server→client `sampling`/`elicitation`/`roots`, auto-answers `ping`, receives push notifications (`resources/updated`, `list_changed`, log messages) — including over HTTP via the GET SSE stream
- **Dual-form client API**: fluent `RequestBuilder` with `on_complete`/`on_error`/`on_progress` + `std::future` (`PendingRequest::get()`), plus blocking convenience wrappers (`call_tool`, `list_tools`, ...)
- **Progress reporting**: real-time progress notifications during tool calls
- **High concurrency**: each `McpClient` owns an `asio::strand`; multiple clients coexist or share one `io_context`
- **Cross-platform**: Windows (MSVC 2019+) and Linux (GCC 11+)
- **C++17**: no advanced feature dependencies, VS2019 compatible
- **Lightweight deps**: only nlohmann-json, asio, llhttp
- **Hardened**: protocol-version negotiation, progressToken echo, exception-isolated user callbacks, fail-fast disconnects, bounded shutdown paths
- **Full test coverage**: 63 unit tests + 29 real I/O integration tests (92 total), verified on Windows & Linux (CI: MSVC + GCC, ASan/UBSan)

## Installation (consume as a package)

Build once, install, then consume from another CMake project via `find_package`:

```bash
cmake -B build -DCMAKE_TOOLCHAIN_FILE=$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake
cmake --build build --config Release
cmake --install build --prefix /your/prefix --config Release
```

This installs three libraries (`cppmcp_common`, `cppmcp_server`, `cppmcp_client`),
the headers under `include/cppmcp/`, and a config package under
`lib/cmake/cppmcp/`. In the consuming project:

```cmake
find_package(cppmcp 0.2 REQUIRED COMPONENTS server client)
target_link_libraries(my_app PRIVATE cppmcp::cppmcp_server cppmcp::cppmcp_client)
```

`cppmcp_common` (and the asio/llhttp/nlohmann-json dependencies it carries)
come in transitively. Note: the exported `find_dependency(asio CONFIG)` assumes
dependencies were installed by vcpkg/conan — building asio from a source
tarball (which ships no CMake config package) is not supported by the export.

## Quick Start

### Prerequisites

- CMake 3.15+
- C++17 compiler (MSVC 2019, GCC 11, Clang 14)
- [vcpkg](https://github.com/microsoft/vcpkg) package manager

### Install Dependencies

```bash
git clone https://github.com/LostSyscall/cppmcp.git
cd cppmcp
vcpkg install
```

### Build

```bash
cmake -B build \
    -DCMAKE_TOOLCHAIN_FILE=$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake \
    -DCPPMCP_BUILD_TESTS=ON \
    -DCPPMCP_BUILD_EXAMPLES=ON

cmake --build build
```

### Run Tests

```bash
# Unit tests only (fast)
cd build && ctest -LE integration --output-on-failure

# Integration tests (real I/O, slower)
cmake -B build -DCPPMCP_BUILD_INTEGRATION_TESTS=ON ...
cd build && ctest -L integration --output-on-failure

# All tests
cd build && ctest --output-on-failure
```

### Docker (Linux verification)

```bash
docker compose up build    # Build and run tests
docker compose run dev bash # Interactive dev shell
```

## Project Structure

```
cppmcp/
├── include/cppmcp/          # Public headers
│   ├── server.hpp           # McpServer core class (server)
│   ├── transport.hpp        # ITransport abstract interface (server)
│   ├── types.hpp            # MCP types (Tool, Resource, Prompt, CallToolResult, ...)
│   ├── jsonrpc.hpp          # JSON-RPC 2.0 message parsing
│   ├── protocol.hpp         # Protocol constants & method names
│   ├── context.hpp          # RequestContext (progress, logging)
│   ├── exception.hpp        # Exception types
│   ├── common.hpp           # RequestId variant & utilities
│   ├── stdio_transport.hpp  # StdioTransport (server)
│   ├── http_transport.hpp   # HttpTransport (SSE + Streamable HTTP, server)
│   ├── local_pipe_transport.hpp # LocalPipeTransport (server)
│   ├── client.hpp           # McpClient core class (client)
│   ├── pending_request.hpp  # PendingRequest (future + callbacks + state machine)
│   ├── client_transport.hpp # IClientTransport abstract interface (client)
│   ├── stdio_client_transport.hpp  # StdioClientTransport (spawn child)
│   ├── http_client_transport.hpp   # HttpClientTransport (Streamable HTTP)
│   ├── local_pipe_client_transport.hpp # LocalPipeClientTransport
│   └── process.hpp          # Cross-platform child-process spawn
├── src/                     # Implementation files
├── examples/                # Example servers + client demo
│   ├── simple_stdio_server/
│   ├── streamable_http_server/
│   ├── http_sse_server/
│   ├── local_pipe_server/
│   └── client_demo/         # McpClient ↔ local-pipe server end-to-end demo
├── tests/                   # Unit & integration tests
│   ├── test_*.cpp           # 45 unit tests (incl. test_client.cpp mock)
│   └── integration/         # 29 real I/O integration tests (incl. McpClient)
├── docs/zh-CN/              # Chinese documentation
├── Dockerfile               # Docker multi-stage build
├── docker-compose.yml
├── vcpkg.json
└── CMakeLists.txt
```

## Usage Examples

### Minimal stdio server

```cpp
#include <cppmcp/server.hpp>
#include <cppmcp/stdio_transport.hpp>
#include <cppmcp/types.hpp>

using namespace cppmcp;

int main() {
    Implementation info{"my_server", "1.0.0"};
    ServerCapabilities caps;
    caps.tools = ToolsCapability{};

    McpServer server(info, caps);

    Tool echo_tool;
    echo_tool.name = "echo";
    echo_tool.description = "Echoes the input message";
    echo_tool.input_schema = R"({
        "type": "object",
        "properties": {"message": {"type": "string"}},
        "required": ["message"]
    })"_json;

    server.register_tool("echo", echo_tool,
        [](const nlohmann::json& args, RequestContext&) -> CallToolResult {
            CallToolResult result;
            result.content.push_back(TextContent{"text", args["message"].get<std::string>()});
            return result;
        });

    auto transport = std::make_shared<StdioTransport>();
    server.connect(transport);
    server.run();
}
```

### Streamable HTTP server

```cpp
#include <cppmcp/server.hpp>
#include <cppmcp/http_transport.hpp>

HttpTransportConfig config;
config.mode = HttpTransportMode::StreamableHttp;
config.host = "127.0.0.1";
config.port = 3000;
config.path = "/mcp";

auto transport = std::make_shared<HttpTransport>(config);
server.connect(transport);
server.run();
```

### SSE (legacy HTTP) server

```cpp
HttpTransportConfig config;
config.mode = HttpTransportMode::SSE;
config.host = "127.0.0.1";
config.port = 3001;
config.sse_path = "/sse";
config.message_path = "/messages";

auto transport = std::make_shared<HttpTransport>(config);
server.connect(transport);
server.run();
```

### Local pipe server

```cpp
#include <cppmcp/local_pipe_transport.hpp>

LocalPipeConfig config;
config.pipe_name = "my_mcp_pipe";
config.mode = PipeMode::SingleClient;  // or PipeMode::MultiClient

auto transport = std::make_shared<LocalPipeTransport>(config);
server.connect(transport);
server.run();
// Windows: \\.\pipe\my_mcp_pipe
// Linux:   /tmp/my_mcp_pipe.sock
```

### Progress reporting

```cpp
server.register_tool("long_task", tool_def,
    [](const nlohmann::json& args, RequestContext& ctx) -> CallToolResult {
        for (int i = 0; i < 10; ++i) {
            ctx.report_progress(i / 10.0, 1.0);
        }
        CallToolResult result;
        result.content.push_back(TextContent{"text", "Done"});
        return result;
    });
```

### Resource registration

```cpp
Resource doc;
doc.name = "readme";
doc.uri = "file:///readme.md";
doc.description = "Project documentation";

server.register_resource("file:///readme.md", doc,
    [](const std::string& uri, RequestContext&) -> ReadResourceResult {
        ReadResourceResult result;
        ResourceContents rc;
        rc.uri = uri;
        rc.mime_type = "text/plain";
        rc.text = "Documentation content...";
        result.contents.push_back(rc);
        return result;
    });
```

### Prompt registration

```cpp
Prompt greet;
greet.name = "greet";
greet.description = "Generate a greeting";
PromptArgument name_arg;
name_arg.name = "name";
name_arg.required = true;
greet.arguments = {name_arg};

server.register_prompt("greet", greet,
    [](const std::string&, const nlohmann::json& args, RequestContext&) -> GetPromptResult {
        GetPromptResult result;
        result.description = "Greeting";
        result.messages.push_back(PromptMessage{
            "user", TextContent{"text", "Hello, " + args["name"].get<std::string>()}
        });
        return result;
    });
```

### MCP Client

Connect to any MCP server — spawn it as a child process (`StdioClientTransport`), or reach it over HTTP / local pipe — then list and call its tools:

```cpp
#include <cppmcp/client.hpp>
#include <cppmcp/stdio_client_transport.hpp>

using namespace cppmcp;

int main() {
    auto client = std::make_shared<McpClient>(Implementation{"my_client", "1.0.0"});
    client->use_transport(std::make_shared<StdioClientTransport>("./my_server"));

    auto server_info = client->connect();   // blocking initialize handshake

    // Blocking convenience API
    auto tools = client->list_tools();
    CallToolResult r = client->call_tool("echo", nlohmann::json{{"message", "hi"}});

    // Async builder: callbacks + std::future, both usable
    auto pr = client->prepare(Protocol::METHOD_TOOLS_CALL,
                    nlohmann::json{{"name", "echo"},
                                   {"arguments", nlohmann::json{{"message", "async"}}}})
                  .on_progress([](double p, std::optional<double> total) { /* ... */ })
                  .timeout(std::chrono::seconds(30))
                  .send();
    nlohmann::json result = pr->get();      // or rely solely on callbacks

    client->stop();
}
```

Other transports: `HttpClientTransport(host, port, "/mcp")`, `LocalPipeClientTransport(pipe_name)`. To answer server→client requests, register handlers before connecting:

```cpp
client->register_roots_handler([]() -> ListRootsResult {
    ListRootsResult r;
    r.roots.push_back(Root{"file:///workspace", "workspace"});
    return r;
});
client->register_sampling_handler(...);     // sampling/createMessage
client->register_elicitation_handler(...);  // elicitation/create
```

Multiple `McpClient`s run independently; to share one event loop across them, call `client->set_io_context(&io)` before `connect()`. Callbacks normally fire on the client's strand — move them off with `set_callback_executor(...)`.

## Core Classes

### McpServer

Main server class, manages MCP protocol lifecycle and request processing.

| Method | Description |
|--------|-------------|
| `McpServer(info, caps)` | Construct with server info & capability declaration |
| `register_tool(name, def, handler)` | Register a tool |
| `register_resource(uri, def, handler)` | Register a resource |
| `register_prompt(name, def, handler)` | Register a prompt |
| `register_completion(handler)` | Register completion handler |
| `connect(transport)` | Connect a transport |
| `run()` | Run server (blocking) |
| `stop()` | Stop server |
| `is_running()` | Check server running state |
| `notify_tools_list_changed()` | Notify tool list changed |
| `notify_resources_list_changed()` | Notify resource list changed |
| `notify_resources_updated(uri)` | Notify resource content updated |
| `notify_prompts_list_changed()` | Notify prompt list changed |

### ITransport

Abstract transport interface. All transport implementations inherit from this.

| Method | Description |
|--------|-------------|
| `start()` | Start transport |
| `stop()` | Stop transport |
| `is_running()` | Check running state |
| `send_message(json)` | Send message |
| `set_message_handler(cb)` | Set message callback |
| `set_error_handler(cb)` | Set error callback |
| `set_io_context(io_ctx)` | Set asio io_context |
| `set_response_sender(sender)` | Set response sender |

### HttpTransport-specific

| Method | Description |
|--------|-------------|
| `get_port()` | Get actual listening port (supports port=0 random assignment) |

### McpClient (client)

Main client class — owns an `asio::io_context` (or attaches to an external one) and correlates responses to outbound requests by id.

| Method | Description |
|--------|-------------|
| `McpClient(client_info, capabilities)` | Construct (self-owned io_context) |
| `set_io_context(io_ctx)` | Attach to an external io_context (no internal thread) |
| `set_worker_threads(n)` | Worker pool for slow server→client handlers |
| `set_callback_executor(exec)` | Move user callbacks off the strand |
| `use_transport(transport)` | Attach an `IClientTransport` |
| `connect(timeout)` | Start io, connect transport, run `initialize` handshake (blocking) |
| `async_connect(timeout)` | Non-blocking handshake; returns the initialize `PendingRequest` |
| `disconnect()` / `stop()` | Close transport / full shutdown (idempotent) |
| `prepare(method, params)` | Build an outbound request (`RequestBuilder`) |
| `send_notification(method, params)` | Fire-and-forget notification |
| `list_tools` / `call_tool` / `list_resources` / `read_resource` / `list_prompts` / `get_prompt` / `ping` / `set_logging_level` | Blocking convenience wrappers |
| `register_sampling_handler` / `register_elicitation_handler` / `register_roots_handler` | Handle server→client requests |
| `on_disconnect(handler)` | Peer-disconnect callback |
| `has_capability(name)` | Check negotiated server capability ("tools"/"resources"/...) |

### RequestBuilder / PendingRequest

`prepare(...)` returns a `RequestBuilder`; `.send()` returns a `std::shared_ptr<PendingRequest>`.

| Item | Description |
|--------|-------------|
| `.on_complete(json)` / `.on_error(McpOutcome)` / `.on_progress(double, total?)` / `.timeout(dur)` | Builder config (set before `send()`) |
| `PendingRequest::get()` | Block until terminal; returns result, throws `McpException` on failure |
| `PendingRequest::wait_for(dur)` | Timed wait |
| `PendingRequest::cancel(reason)` | Post a cancel (sends `notifications/cancelled`) |
| `PendingRequest::state()` | Current `RequestState` (Waiting/Succeeded/Errored/TimedOut/Cancelled/Failed) |

### IClientTransport (client)

Abstract client-transport interface (note: NO `ResponseSink`, unlike `ITransport`). Implementations: `StdioClientTransport`, `HttpClientTransport`, `LocalPipeClientTransport`.

## Transport Modes

| Mode | Use case | Connection |
|------|----------|------------|
| **stdio** | Embedded processes, CLI tools | stdin/stdout, newline-delimited JSON-RPC |
| **Streamable HTTP** | Web services, remote calls | POST /mcp, GET SSE stream, DELETE disconnect |
| **SSE** | Legacy client compatibility | GET /sse for events, POST /messages for requests |
| **Local pipe** | High-performance local IPC | Windows: Named Pipe, Linux: Unix Domain Socket |

### Streamable HTTP

MCP-recommended HTTP mode. Single-endpoint architecture:

- `POST /mcp` — send request, response in HTTP body or SSE stream
- `GET /mcp` — establish SSE stream for server-push notifications
- `DELETE /mcp` — terminate session
- Session management via `mcp-session-id` header

### Local Pipe

High-performance local inter-process communication:

- **SingleClient**: single connection mode, one-to-one communication
- **MultiClient**: multi-connection mode, up to `max_instances` concurrent connections
- Windows uses Overlapped I/O (`FILE_FLAG_OVERLAPPED` + asio stream_handle), Linux uses asio local::stream_protocol

## Testing

The project contains 92 Google Test tests across two tiers:

### Unit Tests (63)

Using TestTransport / TestClientTransport mocks (no real I/O):

- JSON-RPC 2.0 parsing & serialization (10)
- MCP type serialization (9)
- Server core logic (8)
- Integration: Resources, Prompts, notifications (9)
- Client core (14): outbound response correlation, error→throw, timeout, cancel, progress routing, shutdown-fails-pending, inbound roots/sampling dispatch, ping auto-answer, reconnect, callback exception isolation, fail-fast when disconnected, notification routing, unsupported-version handshake
- Protocol conformance (14): version negotiation, parameter type errors → -32602, inputSchema/required-argument validation, completion crash vectors, setLevel enum, progressToken echo, resource template matching, subscribe capability gating + per-session routing, pagination, null-id rejection

### Integration Tests (29)

Using real I/O transports, verified on Windows & Linux:

- LocalPipe real connections: ConnectAndInitialize, PingAfterInit, ToolsCall, MultipleSequentialRequests (4)
- Streamable HTTP real requests: Initialize, Ping, ToolsCall, SessionId, Notification202, Delete (6)
- SSE real streaming: ConnectGetEndpoint, InitializeViaPost, ToolsCallViaSse, ResourcesRead (4)
- Stdio subprocess pipes: InitializeAndPing, ToolsCall, ToolsList, MalformedJson (4)
- **McpClient end-to-end (11)**: `McpClientStdio` (spawn `cppmcp_test_stdio_server`, handshake/list/call/progress/shutdown, builder callback form), `McpClientHttp` (real Streamable HTTP server), `client_demo` (local pipe)

## CMake Options

| Option | Default | Description |
|--------|---------|-------------|
| `CPPMCP_BUILD_EXAMPLES` | ON | Build example servers |
| `CPPMCP_BUILD_TESTS` | OFF | Build unit tests |
| `CPPMCP_BUILD_INTEGRATION_TESTS` | OFF | Build integration tests (real I/O) |

## Dependencies

| Library | Purpose |
|---------|---------|
| [nlohmann-json](https://github.com/nlohmann/json) | JSON serialization |
| [asio](https://github.com/chriskohlhoff/asio) | Async I/O (standalone) |
| [llhttp](https://github.com/nodejs/llhttp) | HTTP parsing |
| [gtest](https://github.com/google/googletest) | Testing (optional) |

## Code Style

- Namespace: `cppmcp`
- Classes: PascalCase (`McpServer`, `ITransport`)
- Functions/methods: snake_case (`register_tool`, `send_message`)
- Members: snake_case + trailing underscore (`running_`, `config_`)
- Types/structs: PascalCase (`Tool`, `Resource`, `CallToolResult`)
- Constants: UPPER_SNAKE_CASE (`LATEST_PROTOCOL_VERSION`)

## License

MIT License