#pragma once

#include <atomic>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <vector>

#include <asio.hpp>
#include <llhttp.h>
#include <nlohmann/json.hpp>

#include "transport.hpp"

namespace cppmcp {

class HttpTransport;  // Forward declaration for HttpConnection dispatch

enum class HttpTransportMode {
    SSE,              // Legacy: GET /sse + POST /messages
    StreamableHttp    // Modern: POST /mcp + GET /mcp SSE + DELETE /mcp
};

struct HttpTransportConfig {
    HttpTransportMode mode = HttpTransportMode::StreamableHttp;
    std::string host = "127.0.0.1";
    int port = 3000;
    std::string path = "/mcp";
    std::string sse_path = "/sse";
    std::string message_path = "/messages";
    size_t max_body_size = 1024 * 1024;
};

struct HttpConnection : std::enable_shared_from_this<HttpConnection> {
    asio::ip::tcp::socket socket;
    llhttp_t parser;
    llhttp_settings_t parser_settings;
    asio::streambuf read_buf;
    std::string current_url;
    std::string current_method;
    std::string current_body;
    std::map<std::string, std::string> headers;
    std::string current_header_field;
    std::string current_header_value;
    std::deque<std::string> write_queue;
    std::mutex write_mutex;
    std::atomic<bool> active{true};
    bool request_complete = false;

    explicit HttpConnection(asio::ip::tcp::socket s);
    void start_read_with_dispatch(HttpTransport* transport);
    void start_write();
    void enqueue_write(std::string data);
};

struct SseConnection : std::enable_shared_from_this<SseConnection> {
    std::shared_ptr<HttpConnection> conn;
    std::deque<std::string> write_queue;
    std::mutex write_mutex;
    std::atomic<bool> active{true};

    void push(const std::string& data);
    void start_write();
};

class HttpTransport : public ITransport {
public:
    friend struct HttpConnection;
    explicit HttpTransport(const HttpTransportConfig& config = {});
    ~HttpTransport() override;

    void start() override;
    void stop() override;
    bool is_running() const override;

    void send_message(const nlohmann::json& message) override;
    void set_message_handler(MessageCallback handler) override;
    void set_error_handler(ErrorCallback handler) override;
    void set_response_sender(ResponseSender sender) override;
    void set_io_context(asio::io_context* io_ctx) override;

    int get_port() const;

private:
    void do_accept();
    void handle_request(std::shared_ptr<HttpConnection> conn);

    // Streamable HTTP handlers
    void handle_post(std::shared_ptr<HttpConnection> conn);
    void handle_get_sse(std::shared_ptr<HttpConnection> conn);
    void handle_delete(std::shared_ptr<HttpConnection> conn);
    void handle_options(std::shared_ptr<HttpConnection> conn);

    // Legacy SSE handlers
    void handle_sse_connect(std::shared_ptr<HttpConnection> conn);
    void handle_sse_message(std::shared_ptr<HttpConnection> conn);

    void push_to_sse_connections(const nlohmann::json& message);

    std::string build_http_response(int status, const std::string& content_type,
                                      const std::string& body,
                                      std::vector<std::pair<std::string, std::string>> extra_headers = {});

    std::string status_text(int status);

    HttpTransportConfig config_;
    asio::io_context* io_ctx_ = nullptr;
    std::unique_ptr<asio::ip::tcp::acceptor> acceptor_;
    std::atomic<bool> running_{false};

    std::shared_mutex connections_mutex_;
    std::vector<std::shared_ptr<SseConnection>> active_sse_connections_;

    std::string current_session_id_;
    std::atomic<bool> session_initialized_{false};

    MessageCallback message_handler_;
    ErrorCallback error_handler_;
};

} // namespace cppmcp