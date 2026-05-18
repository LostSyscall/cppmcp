#include <cppmcp/server.hpp>
#include <cppmcp/stdio_transport.hpp>
#include <cppmcp/types.hpp>

using namespace cppmcp;
using json = nlohmann::json;

int main() {
    Implementation info{"test_stdio_server", "1.0.0"};
    ServerCapabilities caps;
    caps.tools = ToolsCapability();

    McpServer server(info, caps);

    Tool echo_tool;
    echo_tool.name = "echo";
    echo_tool.description = "Echoes back the input message";
    echo_tool.input_schema = nlohmann::json::parse(
        "{\"type\": \"object\", \"properties\": {\"message\": {\"type\": \"string\"}}, \"required\": [\"message\"]}"
    );

    server.register_tool("echo", echo_tool,
        [](const json& args, RequestContext&) -> CallToolResult {
            CallToolResult result;
            result.content.push_back(TextContent{"text", args["message"].get<std::string>()});
            result.is_error = false;
            return result;
        }
    );

    Tool add_tool;
    add_tool.name = "add";
    add_tool.description = "Adds two numbers";
    add_tool.input_schema = nlohmann::json::parse(
        "{\"type\": \"object\", \"properties\": {\"a\": {\"type\": \"number\"}, \"b\": {\"type\": \"number\"}}, \"required\": [\"a\", \"b\"]}"
    );

    server.register_tool("add", add_tool,
        [](const json& args, RequestContext&) -> CallToolResult {
            CallToolResult result;
            double sum = args["a"].get<double>() + args["b"].get<double>();
            result.content.push_back(TextContent{"text", std::to_string(sum)});
            result.is_error = false;
            return result;
        }
    );

    auto transport = std::make_shared<StdioTransport>();
    server.connect(transport);
    server.run();

    return 0;
}