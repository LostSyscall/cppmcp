#include <cppmcp/server.hpp>
#include <cppmcp/http_transport.hpp>
#include <cppmcp/types.hpp>

#include <iostream>

using namespace cppmcp;
using json = nlohmann::json;

int main() {
    Implementation info{"cppmcp_http_example", "1.0.0"};

    ServerCapabilities caps;
    caps.tools = ToolsCapability();
    caps.logging = LoggingCapability();
    caps.prompts = PromptsCapability();
    caps.prompts->list_changed = true;

    McpServer server(info, caps);

    // Greeting tool
    Tool greet_tool;
    greet_tool.name = "greet";
    greet_tool.description = "Generates a greeting message for a person";
    greet_tool.input_schema = R"({
        "type": "object",
        "properties": {
            "name": {"type": "string", "description": "Person's name"}
        },
        "required": ["name"]
    })"_json;

    server.register_tool("greet", greet_tool,
        [](const json& args, RequestContext& ctx) -> CallToolResult {
            std::string name = args["name"].get<std::string>();
            CallToolResult result;
            result.content.push_back(TextContent{"text", "Hello, " + name + "! Welcome to the MCP server."});
            result.is_error = false;
            return result;
        }
    );

    // Code review prompt
    Prompt review_prompt;
    review_prompt.name = "code_review";
    review_prompt.description = "Prompt template for code review";
    review_prompt.arguments = std::vector<PromptArgument>{
        PromptArgument{"code", "The code to review", true},
        PromptArgument{"language", "Programming language"}
    };

    server.register_prompt("code_review", review_prompt,
        [](const std::string& name, const json& arguments, RequestContext& ctx) -> GetPromptResult {
            std::string code = arguments.contains("code") ? arguments["code"].get<std::string>() : "";
            std::string lang = arguments.contains("language") ? arguments["language"].get<std::string>() : "unknown";

            GetPromptResult result;
            result.description = "Code review prompt for " + lang;
            result.messages.push_back(PromptMessage{
                "user",
                TextContent{"text", "Please review this " + lang + " code:\n\n" + code}
            });
            return result;
        }
    );

    HttpTransportConfig config;
    config.mode = HttpTransportMode::StreamableHttp;
    config.host = "127.0.0.1";
    config.port = 3000;
    config.path = "/mcp";

    auto transport = std::make_shared<HttpTransport>(config);
    server.connect(transport);

    std::cerr << "[cppmcp_http_example] Starting Streamable HTTP MCP server on http://127.0.0.1:3000/mcp..." << std::endl;
    server.run();
    std::cerr << "[cppmcp_http_example] Server stopped." << std::endl;

    return 0;
}