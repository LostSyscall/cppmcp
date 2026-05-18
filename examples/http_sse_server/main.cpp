#include <cppmcp/server.hpp>
#include <cppmcp/http_transport.hpp>
#include <cppmcp/types.hpp>

#include <chrono>
#include <ctime>
#include <iostream>

using namespace cppmcp;
using json = nlohmann::json;

int main() {
    Implementation info{"cppmcp_sse_example", "1.0.0"};

    ServerCapabilities caps;
    caps.tools = ToolsCapability();
    caps.logging = LoggingCapability();
    caps.resources = ResourcesCapability();
    caps.resources->subscribe = true;

    McpServer server(info, caps);

    // Get time tool
    Tool time_tool;
    time_tool.name = "get_time";
    time_tool.description = "Returns the current server time";
    time_tool.input_schema = R"({"type": "object", "properties": {}})"_json;

    server.register_tool("get_time", time_tool,
        [](const json& args, RequestContext& ctx) -> CallToolResult {
            auto now = std::chrono::system_clock::now();
            auto time_t = std::chrono::system_clock::to_time_t(now);
            std::string time_str = std::ctime(&time_t);
            if (!time_str.empty() && time_str.back() == '\n') time_str.pop_back();
            CallToolResult result;
            result.content.push_back(TextContent{"text", "Current time: " + time_str});
            result.is_error = false;
            return result;
        }
    );

    // Sample resource
    Resource config_resource;
    config_resource.name = "config";
    config_resource.uri = "file:///config.json";
    config_resource.description = "Server configuration";
    config_resource.mime_type = "application/json";

    server.register_resource("file:///config.json", config_resource,
        [](const std::string& uri, RequestContext& ctx) -> ReadResourceResult {
            ResourceContents contents;
            contents.uri = uri;
            contents.mime_type = "application/json";
            contents.text = "{\"version\": \"1.0.0\", \"name\": \"cppmcp\"}";
            return ReadResourceResult{{contents}};
        }
    );

    HttpTransportConfig config;
    config.mode = HttpTransportMode::SSE;
    config.host = "127.0.0.1";
    config.port = 3001;
    config.sse_path = "/sse";
    config.message_path = "/messages";

    auto transport = std::make_shared<HttpTransport>(config);
    server.connect(transport);

    std::cerr << "[cppmcp_sse_example] Starting SSE MCP server on http://127.0.0.1:3001..." << std::endl;
    server.run();
    std::cerr << "[cppmcp_sse_example] Server stopped." << std::endl;

    return 0;
}