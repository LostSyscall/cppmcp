#include <cppmcp/client.hpp>
#include <cppmcp/local_pipe_client_transport.hpp>
#include <cppmcp/local_pipe_transport.hpp>
#include <cppmcp/server.hpp>
#include <cppmcp/types.hpp>

#include <chrono>
#include <iostream>
#include <optional>
#include <string>
#include <thread>
#include <variant>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

using namespace cppmcp;
using json = nlohmann::json;

static std::string unique_pipe_name() {
#ifdef _WIN32
    return "cppmcp_client_demo_" + std::to_string(GetCurrentProcessId());
#else
    return "cppmcp_client_demo_" + std::to_string(getpid());
#endif
}

int main() {
    const std::string pipe_name = unique_pipe_name();

    // --- Start a pipe MCP server in a background thread ---
    Implementation server_info{"demo_server", "1.0.0"};
    ServerCapabilities caps;
    caps.tools = ToolsCapability();
    McpServer server(server_info, caps);

    Tool echo;
    echo.name = "echo";
    echo.description = "Echo a message back, reporting progress";
    echo.input_schema = R"({"type":"object","properties":{"message":{"type":"string"}},"required":["message"]})"_json;
    server.register_tool("echo", echo,
        [](const json& args, RequestContext& ctx) -> CallToolResult {
            ctx.report_progress(0.5, 1.0);
            CallToolResult r;
            r.content.push_back(TextContent{"text", args["message"].get<std::string>()});
            return r;
        });

    LocalPipeConfig cfg;
    cfg.pipe_name = pipe_name;
    cfg.mode = PipeMode::SingleClient;
    server.connect(std::make_shared<LocalPipeTransport>(cfg));
    std::thread server_thread([&]() { server.run(); });
    // Let the server bind the pipe before the client connects.
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    // --- Client side ---
    auto client = std::make_shared<McpClient>(Implementation{"demo_client", "1.0.0"});
    client->use_transport(std::make_shared<LocalPipeClientTransport>(pipe_name));

    auto sr = client->connect(std::chrono::seconds(10));
    std::cout << "Connected to server: " << sr.server_info.name
              << " (protocol " << sr.protocol_version << ")\n";

    auto tools = client->list_tools();
    std::cout << "Server exposes " << tools.size() << " tool(s):\n";
    for (const auto& t : tools) {
        std::cout << "  - " << t.name;
        if (t.description) std::cout << ": " << *t.description;
        std::cout << "\n";
    }

    // Synchronous convenience call.
    CallToolResult echo_r = client->call_tool("echo", json{{"message", "hello-from-demo"}});
    if (!echo_r.content.empty()) {
        if (auto* txt = std::get_if<TextContent>(&echo_r.content[0])) {
            std::cout << "call_tool(echo) -> " << txt->text << "\n";
        }
    }

    // Builder form with progress + result (PendingRequest).
    json args;
    args["message"] = "async-call";
    json params;
    params["name"] = "echo";
    params["arguments"] = std::move(args);
    auto pr = client->prepare(Protocol::METHOD_TOOLS_CALL, std::move(params))
                  .on_progress([](double p, std::optional<double> total) {
                      std::cout << "  progress " << p;
                      if (total) std::cout << " / " << *total;
                      std::cout << "\n";
                  })
                  .timeout(std::chrono::seconds(10))
                  .send();
    json async_r = pr->get();
    if (async_r.contains("content")) {
        std::cout << "prepare(echo) -> " << async_r["content"][0]["text"] << "\n";
    }

    client->stop();
    server.stop();
    server_thread.join();
    std::cout << "Done.\n";
    return 0;
}
