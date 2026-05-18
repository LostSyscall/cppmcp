#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>

#include <asio.hpp>
#include <nlohmann/json.hpp>

#include <cppmcp/server.hpp>
#include <cppmcp/transport.hpp>
#include <cppmcp/types.hpp>

namespace cppmcp::testing {

// RAII: runs McpServer in a background thread
class ServerThread {
public:
    explicit ServerThread(McpServer& server);
    ~ServerThread();

    void wait_until_ready(std::chrono::milliseconds timeout = std::chrono::seconds(5));
    void stop();

private:
    McpServer& server_;
    std::thread thread_;
};

// --- JSON-RPC message helpers ---
nlohmann::json make_initialize_request(int id);
nlohmann::json make_initialized_notification();
nlohmann::json make_ping_request(int id);
nlohmann::json make_tools_list_request(int id);
nlohmann::json make_tools_call_request(int id, const std::string& tool_name,
                                        const nlohmann::json& arguments);

// --- HTTP response helpers ---
struct HttpResponse {
    int status = 0;
    std::map<std::string, std::string> headers;
    std::string body;
};

HttpResponse parse_http_response(const std::string& raw);

// Build an HTTP POST request string
std::string build_http_post(const std::string& host, int port, const std::string& path,
                             const std::string& content_type, const std::string& body,
                             const std::map<std::string, std::string>& extra_headers = {});

// Build an HTTP GET request string
std::string build_http_get(const std::string& host, int port, const std::string& path,
                            const std::map<std::string, std::string>& extra_headers = {});

// Build an HTTP DELETE request string
std::string build_http_delete(const std::string& host, int port, const std::string& path,
                               const std::map<std::string, std::string>& extra_headers = {});

// --- Utility ---
std::string get_unique_pipe_name();

// Create a minimal McpServer with echo + add tools (used by multiple test fixtures)
std::shared_ptr<McpServer> create_test_server_with_tools();

} // namespace cppmcp::testing