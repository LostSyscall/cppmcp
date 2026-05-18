#include <cppmcp/server.hpp>
#include <cppmcp/stdio_transport.hpp>
#include <cppmcp/types.hpp>

#include <iostream>

using namespace cppmcp;
using json = nlohmann::json;

int main() {
    Implementation info{"cppmcp_stdio_example", "1.0.0"};

    ServerCapabilities caps;
    caps.tools = ToolsCapability{};
    caps.resources = ResourcesCapability{};
    caps.prompts = PromptsCapability{};

    McpServer server(info, caps);

    // --- Tools ---
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
            return result;
        }
    );

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
    add_tool.annotations = ToolAnnotations{};
    add_tool.annotations->read_only_hint = true;

    server.register_tool("add", add_tool,
        [](const json& args, RequestContext&) -> CallToolResult {
            double a = args["a"].get<double>();
            double b = args["b"].get<double>();
            CallToolResult result;
            result.content.push_back(TextContent{"text", std::to_string(a + b)});
            return result;
        }
    );

    // --- Resources ---
    Resource hello_resource;
    hello_resource.name = "hello";
    hello_resource.uri = "cppmcp://hello";
    hello_resource.description = "A friendly greeting resource";

    server.register_resource("cppmcp://hello", hello_resource,
        [](const std::string& uri, RequestContext&) -> ReadResourceResult {
            ReadResourceResult result;
            ResourceContents rc;
            rc.uri = uri;
            rc.mime_type = "text/plain";
            rc.text = "Hello from cppmcp!";
            result.contents.push_back(rc);
            return result;
        }
    );

    // --- Prompts ---
    Prompt greet_prompt;
    greet_prompt.name = "greet";
    greet_prompt.description = "Generate a greeting for a given name";
    PromptArgument name_arg;
    name_arg.name = "name";
    name_arg.required = true;
    greet_prompt.arguments = {name_arg};

    server.register_prompt("greet", greet_prompt,
        [](const std::string&, const json& args, RequestContext&) -> GetPromptResult {
            std::string name = args["name"].get<std::string>();
            GetPromptResult result;
            result.description = "A greeting prompt";
            result.messages.push_back(PromptMessage{"user", TextContent{"text", "Please greet " + name}});
            return result;
        }
    );

    auto transport = std::make_shared<StdioTransport>();
    server.connect(transport);

    std::cerr << "[cppmcp_stdio_example] Starting stdio MCP server..." << std::endl;
    server.run();
    std::cerr << "[cppmcp_stdio_example] Server stopped." << std::endl;

    return 0;
}