# cppmcp — C++ MCP 服务器与客户端库

C++ 实现的 [MCP（Model Context Protocol）](https://modelcontextprotocol.io/) **服务器与客户端**库，符合 MCP 协议规范 `2025-03-26` 与 `2025-06-18`（按连接协商）。服务器支持四种传输模式：标准输入输出（stdio）、SSE、Streamable HTTP、本地管道（Windows Named Pipe / Unix Domain Socket）；客户端支持三种连接方式：stdio（拉起子进程）、Streamable HTTP、本地管道。全部使用 JSON-RPC 2.0 进行协议通信。

## 特性

- **服务器：四种传输模式** — stdio、SSE（传统 HTTP）、Streamable HTTP、本地管道
- **客户端：三种传输模式** — stdio（拉起子进程作为 server）、Streamable HTTP、本地管道
- **全异步 I/O**：asio 统一事件循环 + llhttp HTTP 解析，零线程绑架、零轮询
- **完整 MCP 协议（服务器）**：Tools、Resources、Prompts、Completions、Logging
- **完整 MCP 客户端**：list/call 工具、读资源、取 Prompt、ping；支持 server→client 的 `sampling`/`elicitation`/`roots`，并自动应答 `ping`
- **客户端双形态 API**：流式 `RequestBuilder`（`on_complete`/`on_error`/`on_progress`）+ `std::future`（`PendingRequest::get()`），另有阻塞便捷封装（`call_tool`、`list_tools` 等）
- **进度上报**：Tool 调用支持实时进度通知
- **高并发**：每个 `McpClient` 自带 `asio::strand`；多个客户端可并存，也可共享同一个 `io_context`
- **跨平台**：Windows（MSVC 2019+）与 Linux（GCC 11+）
- **C++17**：无依赖高级特性，兼容 VS2019
- **轻量依赖**：仅 nlohmann-json、asio、llhttp
- **协议加固**：版本协商、progressToken 回显、用户回调异常隔离、断连快速失败、有界关闭路径
- **完整测试**：63 单元测试 + 29 真实 I/O 集成测试（共 92 个），Windows/Linux 双平台验证（CI：MSVC + GCC，ASan/UBSan）

## 快速开始

### 环境要求

- CMake 3.15+
- C++17 编译器（MSVC 2019、GCC 11、Clang 14）
- [vcpkg](https://github.com/microsoft/vcpkg) 包管理器

### 安装依赖

```bash
# 克隆项目
git clone https://github.com/LostSyscall/cppmcp.git
cd cppmcp

# vcpkg 安装依赖（需要设置 VCPKG_ROOT）
vcpkg install
```

### 构建

```bash
cmake -B build \
    -DCMAKE_TOOLCHAIN_FILE=$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake \
    -DCPPMCP_BUILD_TESTS=ON \
    -DCPPMCP_BUILD_EXAMPLES=ON

cmake --build build
```

### 运行测试

```bash
cd build && ctest --output-on-failure
```

### Docker 构建（Linux 验证）

```bash
docker compose up build    # 构建并运行测试
docker compose run dev bash # 开发环境交互式 shell
```

## 项目结构

```
cppmcp/
├── include/cppmcp/          # 公共头文件
│   ├── server.hpp           # McpServer 核心类（服务器）
│   ├── transport.hpp        # ITransport 抽象接口（服务器）
│   ├── types.hpp            # MCP 类型定义（Tool, Resource, Prompt, CallToolResult 等）
│   ├── jsonrpc.hpp          # JSON-RPC 2.0 消息解析
│   ├── protocol.hpp         # 协议常量与方法名
│   ├── context.hpp          # RequestContext（进度上报、日志）
│   ├── exception.hpp        # 异常类型
│   ├── common.hpp           # RequestId 变体与通用工具
│   ├── error_codes.hpp      # JSON-RPC 错误码常量
│   ├── stdio_transport.hpp  # StdioTransport（服务器）
│   ├── http_transport.hpp   # HttpTransport（SSE + Streamable HTTP，服务器）
│   ├── local_pipe_transport.hpp # LocalPipeTransport（服务器）
│   ├── client.hpp           # McpClient 核心类（客户端）
│   ├── pending_request.hpp  # PendingRequest（future + 回调 + 状态机）
│   ├── client_transport.hpp # IClientTransport 抽象接口（客户端）
│   ├── stdio_client_transport.hpp  # StdioClientTransport（拉起子进程）
│   ├── http_client_transport.hpp   # HttpClientTransport（Streamable HTTP）
│   ├── local_pipe_client_transport.hpp # LocalPipeClientTransport
│   └── process.hpp          # 跨平台子进程拉起
├── src/                     # 实现文件
├── examples/                # 示例服务器 + 客户端示例
│   ├── simple_stdio_server/   # stdio 模式示例
│   ├── streamable_http_server/ # Streamable HTTP 模式示例
│   ├── http_sse_server/       # SSE 模式示例
│   ├── local_pipe_server/     # 本地管道模式示例
│   └── client_demo/           # McpClient ↔ 本地管道服务器 端到端示例
├── tests/                   # 单元与集成测试
│   ├── test_*.cpp           # 63 个单元测试（含 test_client.cpp mock 与 test_protocol_conformance.cpp）
│   └── integration/         # 29 个真实 I/O 集成测试（含 McpClient）
│       ├── test_local_pipe_transport.cpp   # LocalPipe 真实连接测试
│       ├── test_streamable_http_transport.cpp # HTTP POST 真实请求测试
│       ├── test_http_sse_transport.cpp     # SSE 流真实推送测试
│       ├── test_stdio_transport.cpp        # Stdio 子进程管道测试
│       ├── test_client_stdio.cpp           # McpClient stdio 端到端测试
│       └── test_client_http.cpp            # McpClient HTTP 端到端测试
├── Dockerfile               # Docker 多阶段构建
├── docker-compose.yml       # Docker Compose 配置
├── vcpkg.json               # vcpkg 依赖清单
└── CMakeLists.txt           # 顶层 CMake 配置
```

## 使用示例

### 最小 stdio 服务器

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

    // 注册工具
    Tool echo_tool;
    echo_tool.name = "echo";
    echo_tool.description = "回显输入消息";
    echo_tool.input_schema = R"({
        "type": "object",
        "properties": {
            "message": {"type": "string"}
        },
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

### Streamable HTTP 服务器

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

### SSE（传统 HTTP）服务器

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

### 本地管道服务器

```cpp
#include <cppmcp/local_pipe_transport.hpp>

LocalPipeConfig config;
config.pipe_name = "my_mcp_pipe";
config.mode = PipeMode::SingleClient;  // 或 PipeMode::MultiClient

auto transport = std::make_shared<LocalPipeTransport>(config);
server.connect(transport);
server.run();
// Windows: \\.\pipe\my_mcp_pipe
// Linux:   /tmp/my_mcp_pipe.sock
```

### 进度上报

```cpp
server.register_tool("long_task", tool_def,
    [](const nlohmann::json& args, RequestContext& ctx) -> CallToolResult {
        for (int i = 0; i < 10; ++i) {
            ctx.report_progress(i / 10.0, 1.0);  // 进度，总计
            // 执行工作...
        }
        CallToolResult result;
        result.content.push_back(TextContent{"text", "完成"});
        return result;
    });
```

### 资源注册

```cpp
Resource doc;
doc.name = "readme";
doc.uri = "file:///readme.md";
doc.description = "项目说明文档";

server.register_resource("file:///readme.md", doc,
    [](const std::string& uri, RequestContext&) -> ReadResourceResult {
        ReadResourceResult result;
        ResourceContents rc;
        rc.uri = uri;
        rc.mime_type = "text/plain";
        rc.text = "文档内容...";
        result.contents.push_back(rc);
        return result;
    });
```

### Prompt 注册

```cpp
Prompt greet;
greet.name = "greet";
greet.description = "生成问候语";
PromptArgument name_arg;
name_arg.name = "name";
name_arg.required = true;
greet.arguments = {name_arg};

server.register_prompt("greet", greet,
    [](const std::string&, const nlohmann::json& args, RequestContext&) -> GetPromptResult {
        GetPromptResult result;
        result.description = "问候语";
        result.messages.push_back(PromptMessage{
            "user", TextContent{"text", "你好，" + args["name"].get<std::string>()}
        });
        return result;
    });
```

### MCP 客户端

连接任意 MCP server —— 用 `StdioClientTransport` 拉起一个子进程，或经 HTTP / 本地管道连接 —— 然后列出并调用其工具：

```cpp
#include <cppmcp/client.hpp>
#include <cppmcp/stdio_client_transport.hpp>

using namespace cppmcp;

int main() {
    auto client = std::make_shared<McpClient>(Implementation{"my_client", "1.0.0"});
    client->use_transport(std::make_shared<StdioClientTransport>("./my_server"));

    auto server_info = client->connect();   // 阻塞式 initialize 握手

    // 阻塞便捷 API
    auto tools = client->list_tools();
    CallToolResult r = client->call_tool("echo", nlohmann::json{{"message", "hi"}});

    // 异步 builder：回调 + std::future，两者皆可使用
    auto pr = client->prepare(Protocol::METHOD_TOOLS_CALL,
                    nlohmann::json{{"name", "echo"},
                                   {"arguments", nlohmann::json{{"message", "async"}}}})
                  .on_progress([](double p, std::optional<double> total) { /* ... */ })
                  .timeout(std::chrono::seconds(30))
                  .send();
    nlohmann::json result = pr->get();      // 也可仅依赖回调

    client->stop();
}
```

其它传输：`HttpClientTransport(host, port, "/mcp")`、`LocalPipeClientTransport(pipe_name)`。连接前注册 server→client 的反向调用处理器：

```cpp
client->register_roots_handler([]() -> ListRootsResult {
    ListRootsResult r;
    r.roots.push_back(Root{"file:///workspace", "workspace"});
    return r;
});
client->register_sampling_handler(...);     // sampling/createMessage
client->register_elicitation_handler(...);  // elicitation/create
```

多个 `McpClient` 可独立运行；若要让多个客户端共享同一事件循环，在 `connect()` 前调用 `client->set_io_context(&io)`。回调默认在客户端 strand 上触发，可用 `set_callback_executor(...)` 移到其它执行器。

## 核心类

### McpServer

主服务器类，管理 MCP 协议生命周期与请求处理。

| 方法 | 说明 |
|------|------|
| `McpServer(info, caps)` | 构造，传入服务器信息与能力声明 |
| `register_tool(name, def, handler)` | 注册工具 |
| `register_resource(uri, def, handler)` | 注册资源 |
| `register_prompt(name, def, handler)` | 注册 Prompt |
| `register_completion(handler)` | 注册 Completion 处理器 |
| `connect(transport)` | 连接传输层 |
| `run()` | 运行服务器（阻塞） |
| `stop()` | 停止服务器 |
| `is_running()` | 检查服务器运行状态 |
| `notify_tools_list_changed()` | 通知工具列表变更 |
| `notify_resources_list_changed()` | 通知资源列表变更 |
| `notify_resources_updated(uri)` | 通知资源内容更新 |
| `notify_prompts_list_changed()` | 通知 Prompt 列表变更 |

### ITransport

传输层抽象接口，所有传输实现均继承此接口。

| 方法 | 说明 |
|------|------|
| `start()` | 启动传输层 |
| `stop()` | 停止传输层 |
| `is_running()` | 检查运行状态 |
| `send_message(json)` | 发送消息 |
| `set_message_handler(cb)` | 设置消息回调 |
| `set_error_handler(cb)` | 设置错误回调 |
| `set_io_context(io_ctx)` | 设置 asio io_context |
| `set_response_sender(sender)` | 设置响应发送器 |

### HttpTransport 特有方法

| 方法 | 说明 |
|------|------|
| `get_port()` | 获取实际监听端口（支持 port=0 随机分配） |

### McpClient（客户端）

主客户端类 —— 自持 `asio::io_context`（或挂载到外部 io），按 id 关联出站请求与响应。

| 方法 | 说明 |
|------|------|
| `McpClient(client_info, capabilities)` | 构造（自持 io_context） |
| `set_io_context(io_ctx)` | 挂载到外部 io_context（不起内部线程） |
| `set_worker_threads(n)` | 为慢的 server→client 处理器配线程池 |
| `set_callback_executor(exec)` | 将用户回调移出 strand |
| `use_transport(transport)` | 挂载 `IClientTransport` |
| `connect(timeout)` | 启动 io、连接传输、执行 `initialize` 握手（阻塞） |
| `async_connect(timeout)` | 非阻塞握手，返回 initialize 的 `PendingRequest` |
| `disconnect()` / `stop()` | 关闭传输 / 完全停止（幂等） |
| `prepare(method, params)` | 构造出站请求（`RequestBuilder`） |
| `send_notification(method, params)` | 发送无需响应的通知 |
| `list_tools` / `call_tool` / `list_resources` / `read_resource` / `list_prompts` / `get_prompt` / `ping` / `set_logging_level` | 阻塞便捷封装 |
| `register_sampling_handler` / `register_elicitation_handler` / `register_roots_handler` | 处理 server→client 请求 |
| `on_disconnect(handler)` | 对端断连回调 |
| `has_capability(name)` | 检查协商到的 server 能力（"tools"/"resources"/...） |

### RequestBuilder / PendingRequest

`prepare(...)` 返回 `RequestBuilder`；`.send()` 返回 `std::shared_ptr<PendingRequest>`。

| 项 | 说明 |
|------|------|
| `.on_complete(json)` / `.on_error(McpOutcome)` / `.on_progress(double, total?)` / `.timeout(dur)` | builder 配置（`send()` 前设置） |
| `PendingRequest::get()` | 阻塞直到终态；成功返回 result，失败抛 `McpException` |
| `PendingRequest::wait_for(dur)` | 带超时等待 |
| `PendingRequest::cancel(reason)` | 发起取消（发送 `notifications/cancelled`） |
| `PendingRequest::state()` | 当前 `RequestState`（Waiting/Succeeded/Errored/TimedOut/Cancelled/Failed） |

### IClientTransport（客户端）

客户端传输抽象接口（注意：无 `ResponseSink`，与服务器 `ITransport` 不同）。实现：`StdioClientTransport`、`HttpClientTransport`、`LocalPipeClientTransport`。

### RequestContext

请求上下文，在 Tool/Resource/Prompt 处理器中使用。

| 方法 | 说明 |
|------|------|
| `report_progress(progress, total)` | 上报进度通知 |
| `notify_logging(level, data)` | 发送日志通知 |

## 传输模式

| 模式 | 适用场景 | 连接方式 |
|------|----------|----------|
| **stdio** | 嵌入式进程、CLI 工具 | 标准输入/输出，行分隔 JSON-RPC |
| **Streamable HTTP** | Web 服务、远程调用 | POST /mcp，GET SSE 流，DELETE 断开 |
| **SSE** | 兼容旧版客户端 | GET /sse 接收事件，POST /messages 发送请求 |
| **本地管道** | 本地高性能 IPC | Windows: Named Pipe，Linux: Unix Domain Socket |

### Streamable HTTP

MCP 协议推荐的 HTTP 模式。单端点架构：

- `POST /mcp` — 发送请求，响应在 HTTP body 或 SSE 流中返回
- `GET /mcp` — 建立 SSE 流，接收服务器推送通知
- `DELETE /mcp` — 断开会话
- 会话管理：通过 `mcp-session-id` 头部标识

### 本地管道

高性能本地进程间通信：

- **SingleClient**：单连接模式，适用于一对一通信
- **MultiClient**：多连接模式，支持最多 `max_instances` 个并发连接
- Windows 使用 Overlapped I/O（`FILE_FLAG_OVERLAPPED` + asio stream_handle），Linux 使用 asio local::stream_protocol

## 测试

项目包含 73 个 Google Test 测试，覆盖两种测试层级：

### 单元测试（45 个）

使用 TestTransport / TestClientTransport mock（无真实 I/O），覆盖：

- JSON-RPC 2.0 解析与序列化（10 个）
- MCP 类型序列化（9 个）
- 服务器核心逻辑（8 个）
- 集成：Resources、Prompts、通知（9 个）
- 客户端核心（9 个）：出站响应关联、error→抛异常、超时、取消、progress 路由、shutdown 失败 pending、入站 roots/sampling 分发、ping 自动应答

### 集成测试（29 个）

使用真实 I/O 传输，在 Windows 和 Linux 上运行，覆盖四种传输模式：

- LocalPipe 真实连接：ConnectAndInitialize、PingAfterInit、ToolsCall、MultipleSequentialRequests（4 个）
- Streamable HTTP 真实请求：Initialize、Ping、ToolsCall、SessionId、Notification202、Delete（6 个）
- SSE 真实流推送：ConnectGetEndpoint、InitializeViaPost、ToolsCallViaSse、ResourcesRead（4 个）
- Stdio 子进程管道：InitializeAndPing、ToolsCall、ToolsList、MalformedJson（4 个）
- **McpClient 端到端（11 个）**：`McpClientStdio`（拉起 `cppmcp_test_stdio_server`，握手/list/call/progress/shutdown、builder 回调形态）、`McpClientHttp`（真实 Streamable HTTP 服务器）、`client_demo`（本地管道）

```bash
# 单元测试
cmake -B build -DCPPMCP_BUILD_TESTS=ON -DCMAKE_TOOLCHAIN_FILE=...
cmake --build build
cd build && ctest -E integration --output-on-failure

# 集成测试（需要真实 I/O，耗时较长）
cmake -B build -DCPPMCP_BUILD_TESTS=ON -DCPPMCP_BUILD_INTEGRATION_TESTS=ON -DCMAKE_TOOLCHAIN_FILE=...
cmake --build build
cd build && ctest -L integration --output-on-failure

# 运行全部测试
cd build && ctest --output-on-failure
```

### Linux Docker 测试

```bash
docker build --target build --network=host -t cppmcp-test .
docker run --rm cppmcp-test bash -c "cd build && ctest --output-on-failure"
```

## CMake 选项

| 选项 | 默认值 | 说明 |
|------|--------|------|
| `CPPMCP_BUILD_EXAMPLES` | ON | 构建示例服务器 |
| `CPPMCP_BUILD_TESTS` | OFF | 构建单元测试 |
| `CPPMCP_BUILD_INTEGRATION_TESTS` | OFF | 构建集成测试（真实 I/O） |

## 依赖

| 库 | 用途 |
|----|------|
| [nlohmann-json](https://github.com/nlohmann/json) | JSON 序列化 |
| [asio](https://github.com/chriskohlhoff/asio) | 异步 I/O（standalone） |
| [llhttp](https://github.com/nodejs/llhttp) | HTTP 解析 |
| [gtest](https://github.com/google/googletest) | 单元测试（可选） |

## 代码风格

- 命名空间：`cppmcp`
- 类名：PascalCase（`McpServer`、`ITransport`）
- 函数/方法：snake_case（`register_tool`、`send_message`）
- 成员变量：snake_case + 后缀下划线（`running_`、`config_`）
- 类型/结构体：PascalCase（`Tool`、`Resource`、`CallToolResult`）
- 常量：UPPER_SNAKE_CASE（`LATEST_PROTOCOL_VERSION`）

## 许可证

MIT License