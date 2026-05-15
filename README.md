# cppmcp — C++ MCP 服务器库

C++ 实现的 [MCP（Model Context Protocol）](https://modelcontextprotocol.io/) 服务器库，符合 MCP 协议规范 `2025-03-26` 版本。支持四种传输模式：标准输入输出（stdio）、SSE、Streamable HTTP、本地管道（Windows Named Pipe / Unix Domain Socket），使用 JSON-RPC 2.0 进行协议通信。

## 特性

- **四种传输模式**：stdio、SSE（传统 HTTP）、Streamable HTTP、本地管道
- **完整 MCP 协议**：Tools、Resources、Prompts、Completions、Logging
- **进度上报**：Tool 调用支持实时进度通知
- **跨平台**：Windows（MSVC 2019+）与 Linux（GCC 11+）
- **C++17**：无依赖高级特性，兼容 VS2019
- **轻量依赖**：仅 nlohmann-json 和 cpp-httplib

## 快速开始

### 环境要求

- CMake 3.15+
- C++17 编译器（MSVC 2019、GCC 11、Clang 14）
- [vcpkg](https://github.com/microsoft/vcpkg) 包管理器

### 安装依赖

```bash
# 克隆项目
git clone https://github.com/your-org/cppmcp.git
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
│   ├── server.hpp           # McpServer 核心类
│   ├── transport.hpp        # ITransport 抽象接口
│   ├── types.hpp            # MCP 类型定义（Tool, Resource, Prompt 等）
│   ├── jsonrpc.hpp          # JSON-RPC 2.0 消息解析
│   ├── protocol.hpp         # 协议常量与方法名
│   ├── context.hpp          # RequestContext（进度上报、日志）
│   ├── exception.hpp        # 异常类型
│   ├── common.hpp           # RequestId 变体与通用工具
│   ├── error_codes.hpp      # JSON-RPC 错误码常量
│   ├── stdio_transport.hpp  # StdioTransport
│   ├── http_transport.hpp   # HttpTransport（SSE + Streamable HTTP）
│   └── local_pipe_transport.hpp # LocalPipeTransport
├── src/                     # 实现文件
├── examples/                # 示例服务器
│   ├── simple_stdio_server/   # stdio 模式示例
│   ├── streamable_http_server/ # Streamable HTTP 模式示例
│   ├── http_sse_server/       # SSE 模式示例
│   └── local_pipe_server/     # 本地管道模式示例
├── tests/                   # 单元与集成测试（36 个测试）
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
- Windows 使用 Overlapped I/O（`FILE_FLAG_OVERLAPPED`），Linux 使用 `poll` 非阻塞 I/O

## 测试

项目包含 36 个 Google Test 测试，覆盖：

- JSON-RPC 2.0 解析与序列化（10 个）
- MCP 类型序列化（9 个）
- 服务器核心逻辑（8 个）
- 集成测试：Resources、Prompts、通知（9 个）

```bash
cmake -B build -DCPPMCP_BUILD_TESTS=ON -DCMAKE_TOOLCHAIN_FILE=...
cmake --build build
cd build && ctest --output-on-failure
```

## CMake 选项

| 选项 | 默认值 | 说明 |
|------|--------|------|
| `CPPMCP_BUILD_EXAMPLES` | ON | 构建示例服务器 |
| `CPPMCP_BUILD_TESTS` | OFF | 构建单元测试 |

## 依赖

| 库 | 用途 |
|----|------|
| [nlohmann-json](https://github.com/nlohmann/json) | JSON 序列化 |
| [cpp-httplib](https://github.com/yhirose/cpp-httplib) | HTTP 服务器 |
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