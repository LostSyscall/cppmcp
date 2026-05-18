# cppmcp — C++ MCP Server Library
**[中文](docs/zh-CN/README.md)**

A C++ implementation of the [Model Context Protocol (MCP)](https://modelcontextprotocol.io/) server, conforming to MCP specification version `2025-03-26`. Supports four transport modes: stdio, SSE, Streamable HTTP, and local pipe (Windows Named Pipe / Unix Domain Socket), using JSON-RPC 2.0 for protocol communication.


## Features

- **Four transport modes**: stdio, SSE (legacy HTTP), Streamable HTTP, local pipe
- **Fully async I/O**: asio unified event loop + llhttp HTTP parser, zero thread kidnapping, zero polling
- **Full MCP protocol**: Tools, Resources, Prompts, Completions, Logging
- **Progress reporting**: real-time progress notifications during tool calls
- **Cross-platform**: Windows (MSVC 2019+) and Linux (GCC 11+)
- **C++17**: no advanced feature dependencies, VS2019 compatible
- **Lightweight deps**: only nlohmann-json, asio, llhttp
- **Full test coverage**: 36 unit tests + 18 real I/O integration tests, verified on Windows & Linux

## Quick Start

### Prerequisites

- CMake 3.15+
- C++17 compiler (MSVC 2019, GCC 11, Clang 14)
- [vcpkg](https://github.com/microsoft/vcpkg) package manager

### Install Dependencies

```bash
git clone https://github.com/your-org/cppmcp.git
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
cd build && ctest -E integration --output-on-failure

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
│   ├── server.hpp           # McpServer core class
│   ├── transport.hpp        # ITransport abstract interface
│   ├── types.hpp            # MCP types (Tool, Resource, Prompt, etc.)
│   ├── jsonrpc.hpp          # JSON-RPC 2.0 message parsing
│   ├── protocol.hpp         # Protocol constants & method names
│   ├── context.hpp          # RequestContext (progress, logging)
│   ├── exception.hpp        # Exception types
│   ├── common.hpp           # RequestId variant & utilities
│   ├── stdio_transport.hpp  # StdioTransport
│   ├── http_transport.hpp   # HttpTransport (SSE + Streamable HTTP)
│   └── local_pipe_transport.hpp # LocalPipeTransport
├── src/                     # Implementation files
├── examples/                # Example servers
│   ├── simple_stdio_server/
│   ├── streamable_http_server/
│   ├── http_sse_server/
│   └── local_pipe_server/
├── tests/                   # Unit & integration tests
│   ├── test_*.cpp           # 36 unit tests (TestTransport mock)
│   └── integration/         # 18 real I/O integration tests
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

The project contains 54 Google Test tests across two tiers:

### Unit Tests (36)

Using TestTransport mock (no real I/O):

- JSON-RPC 2.0 parsing & serialization (10)
- MCP type serialization (9)
- Server core logic (8)
- Integration: Resources, Prompts, notifications (9)

### Integration Tests (18)

Using real I/O transports, verified on Windows & Linux:

- LocalPipe real connections: ConnectAndInitialize, PingAfterInit, ToolsCall, MultipleSequentialRequests (4)
- Streamable HTTP real requests: Initialize, Ping, ToolsCall, SessionId, Notification202, Delete (6)
- SSE real streaming: ConnectGetEndpoint, InitializeViaPost, ToolsCallViaSse, ResourcesRead (4)
- Stdio subprocess pipes: InitializeAndPing, ToolsCall, ToolsList, MalformedJson (4)

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