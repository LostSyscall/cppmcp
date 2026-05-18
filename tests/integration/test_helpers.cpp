#include "test_helpers.hpp"

#include <cstdlib>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace cppmcp::testing {

// --- ServerThread ---
ServerThread::ServerThread(McpServer& server) : server_(server) {
    thread_ = std::thread([&]() { server_.run(); });
}

ServerThread::~ServerThread() {
    stop();
}

void ServerThread::wait_until_ready(std::chrono::milliseconds timeout) {
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (server_.is_running()) return;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
}

void ServerThread::stop() {
    server_.stop();
    if (thread_.joinable()) {
        thread_.join();
    }
}

// --- JSON-RPC helpers ---
nlohmann::json make_initialize_request(int id) {
    return nlohmann::json{
        {"jsonrpc", "2.0"},
        {"id", id},
        {"method", "initialize"},
        {"params", nlohmann::json{
            {"protocolVersion", "2025-03-26"},
            {"capabilities", nlohmann::json::object()},
            {"clientInfo", nlohmann::json{{"name", "test"}, {"version", "1.0"}}}
        }}
    };
}

nlohmann::json make_initialized_notification() {
    return nlohmann::json{
        {"jsonrpc", "2.0"},
        {"method", "notifications/initialized"}
    };
}

nlohmann::json make_ping_request(int id) {
    return nlohmann::json{
        {"jsonrpc", "2.0"},
        {"id", id},
        {"method", "ping"}
    };
}

nlohmann::json make_tools_list_request(int id) {
    return nlohmann::json{
        {"jsonrpc", "2.0"},
        {"id", id},
        {"method", "tools/list"},
        {"params", nlohmann::json::object()}
    };
}

nlohmann::json make_tools_call_request(int id, const std::string& tool_name,
                                        const nlohmann::json& arguments) {
    return nlohmann::json{
        {"jsonrpc", "2.0"},
        {"id", id},
        {"method", "tools/call"},
        {"params", nlohmann::json{
            {"name", tool_name},
            {"arguments", arguments}
        }}
    };
}

// --- HTTP response parsing ---
HttpResponse parse_http_response(const std::string& raw) {
    HttpResponse resp;
    auto header_end = raw.find("\r\n\r\n");
    if (header_end == std::string::npos) return resp;

    std::string headers_section = raw.substr(0, header_end);
    resp.body = raw.substr(header_end + 4);

    // Parse status line
    auto first_space = headers_section.find(' ');
    if (first_space == std::string::npos) return resp;
    auto second_space = headers_section.find(' ', first_space + 1);
    if (second_space == std::string::npos) return resp;
    resp.status = std::stoi(headers_section.substr(first_space + 1, second_space - first_space - 1));

    // Parse headers
    std::istringstream iss(headers_section);
    std::string line;
    std::getline(iss, line); // skip status line
    while (std::getline(iss, line)) {
        if (line.back() == '\r') line.pop_back();
        auto colon = line.find(':');
        if (colon != std::string::npos) {
            std::string key = line.substr(0, colon);
            std::string value = line.substr(colon + 1);
            while (!value.empty() && value[0] == ' ') value.erase(0, 1);
            // Normalize header keys to lowercase for lookup
            for (auto& c : key) c = static_cast<char>(tolower(c));
            resp.headers[key] = value;
        }
    }

    return resp;
}

// --- HTTP request builders ---
std::string build_http_post(const std::string& host, int port, const std::string& path,
                             const std::string& content_type, const std::string& body,
                             const std::map<std::string, std::string>& extra_headers) {
    std::string req = "POST " + path + " HTTP/1.1\r\n";
    req += "Host: " + host + ":" + std::to_string(port) + "\r\n";
    req += "Content-Type: " + content_type + "\r\n";
    req += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    req += "Accept: application/json\r\n";
    for (auto& [k, v] : extra_headers) {
        req += k + ": " + v + "\r\n";
    }
    req += "\r\n" + body;
    return req;
}

std::string build_http_get(const std::string& host, int port, const std::string& path,
                            const std::map<std::string, std::string>& extra_headers) {
    std::string req = "GET " + path + " HTTP/1.1\r\n";
    req += "Host: " + host + ":" + std::to_string(port) + "\r\n";
    for (auto& [k, v] : extra_headers) {
        req += k + ": " + v + "\r\n";
    }
    req += "\r\n";
    return req;
}

std::string build_http_delete(const std::string& host, int port, const std::string& path,
                               const std::map<std::string, std::string>& extra_headers) {
    std::string req = "DELETE " + path + " HTTP/1.1\r\n";
    req += "Host: " + host + ":" + std::to_string(port) + "\r\n";
    for (auto& [k, v] : extra_headers) {
        req += k + ": " + v + "\r\n";
    }
    req += "\r\n";
    return req;
}

// --- Utility ---
std::string get_unique_pipe_name() {
#ifdef _WIN32
    return "cppmcp_test_" + std::to_string(GetCurrentProcessId());
#else
    return "cppmcp_test_" + std::to_string(getpid());
#endif
}

std::shared_ptr<McpServer> create_test_server_with_tools() {
    Implementation info{"test_server", "1.0.0"};
    ServerCapabilities caps;
    caps.tools = ToolsCapability();

    auto server = std::make_shared<McpServer>(info, caps);

    Tool echo_tool;
    echo_tool.name = "echo";
    echo_tool.description = "Echoes back the input message";
    echo_tool.input_schema = nlohmann::json::parse(
        "{\"type\": \"object\", \"properties\": {\"message\": {\"type\": \"string\"}}, \"required\": [\"message\"]}"
    );

    server->register_tool("echo", echo_tool,
        [](const nlohmann::json& args, RequestContext&) -> CallToolResult {
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

    server->register_tool("add", add_tool,
        [](const nlohmann::json& args, RequestContext&) -> CallToolResult {
            CallToolResult result;
            double sum = args["a"].get<double>() + args["b"].get<double>();
            result.content.push_back(TextContent{"text", std::to_string(sum)});
            result.is_error = false;
            return result;
        }
    );

    return server;
}

} // namespace cppmcp::testing