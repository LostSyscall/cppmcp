#include <cppmcp/server.hpp>
#include <cppmcp/local_pipe_transport.hpp>
#include <cppmcp/types.hpp>

#include <iostream>

using namespace cppmcp;
using json = nlohmann::json;

int main() {
    Implementation info{"cppmcp_pipe_example", "1.0.0"};

    ServerCapabilities caps;
    caps.tools = ToolsCapability();

    McpServer server(info, caps);

    // Echo tool
    Tool echo_tool;
    echo_tool.name = "echo";
    echo_tool.description = "Echoes back the input message";
    echo_tool.input_schema = R"({
        "type": "object",
        "properties": {
            "message": {"type": "string", "description": "The message to echo back"}
        },
        "required": ["message"]
    })"_json;

    server.register_tool("echo", echo_tool,
        [](const json& args, RequestContext& ctx) -> CallToolResult {
            std::string message = args["message"].get<std::string>();
            ctx.report_progress(0.5, 1.0);
            CallToolResult result;
            result.content.push_back(TextContent{"text", message});
            result.is_error = false;
            return result;
        }
    );

    // Addition tool
    Tool add_tool;
    add_tool.name = "add";
    add_tool.description = "Adds two numbers together";
    add_tool.input_schema = R"({
        "type": "object",
        "properties": {
            "a": {"type": "number", "description": "First number"},
            "b": {"type": "number", "description": "Second number"}
        },
        "required": ["a", "b"]
    })"_json;

    server.register_tool("add", add_tool,
        [](const json& args, RequestContext& ctx) -> CallToolResult {
            double a = args["a"].get<double>();
            double b = args["b"].get<double>();
            double result_val = a + b;
            CallToolResult result;
            result.content.push_back(TextContent{"text", std::to_string(result_val)});
            result.is_error = false;
            return result;
        }
    );

    LocalPipeConfig config;
    config.pipe_name = "cppmcp_mcp";
    config.mode = PipeMode::SingleClient;

    auto transport = std::make_shared<LocalPipeTransport>(config);
    server.connect(transport);

#ifdef _WIN32
    std::cerr << R"([cppmcp_pipe_example] Starting local pipe MCP server on \\.\pipe\cppmcp_mcp...)" << std::endl;
#else
    std::cerr << "[cppmcp_pipe_example] Starting local pipe MCP server on /tmp/cppmcp_mcp.sock..." << std::endl;
#endif
    server.run();

    std::cerr << "[cppmcp_pipe_example] Server stopped." << std::endl;
    return 0;
}