#pragma once

#include <atomic>
#include <functional>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <asio.hpp>
#include <llhttp.h>
#include <nlohmann/json.hpp>

#include "async_write_queue.hpp"
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
    size_t write_queue_max_bytes = 0;
    QueueOverflowPolicy write_queue_overflow = QueueOverflowPolicy::DropNewest;
    size_t max_connections = 1024;  // 0 = unlimited
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
    std::atomic<bool> active{true};
    bool request_complete = false;
    size_t max_body_size = 1024 * 1024;

    // Single serialized write queue for this connection's socket. Shared with
    // any SseConnection that wraps this HttpConnection, so handshake headers
    // and SSE frames never issue overlapping async_writes on the same socket.
    std::shared_ptr<AsyncWriteQueue> write_queue_;
    std::function<void()> on_disconnect;

    explicit HttpConnection(asio::ip::tcp::socket s);
    void start_read_with_dispatch(HttpTransport* transport);
    void init_write_queue(std::size_t max_queued_bytes, QueueOverflowPolicy policy);
    void on_error();                  // read/write failure -> deactivate + unregister
    void enqueue_write(std::string data);  // forwards to write_queue_
};

struct SseConnection : std::enable_shared_from_this<SseConnection> {
    std::shared_ptr<HttpConnection> conn;
    std::atomic<bool> active{true};

    void push(const std::string& data);  // forwards to conn->write_queue_
};

// A Streamable HTTP session: one per initialize. Owns the SSE streams opened
// against it so notifications can be routed to the right client.
struct HttpSession {
    std::string session_id;
    std::atomic<bool> initialized{false};
    std::list<std::shared_ptr<SseConnection>> sse_connections;  // guarded by sessions_mutex_
};

class HttpTransport : public std::enable_shared_from_this<HttpTransport>, public ITransport {
public:
    friend struct HttpConnection;
    friend struct SseConnection;
    explicit HttpTransport(const HttpTransportConfig& config = {});
    ~HttpTransport() override;

    void start() override;
    void stop() override;
    bool is_running() const override;

    void send_message(const nlohmann::json& message) override;
    void send_to_session(const std::string& session_id, const nlohmann::json& message) override;
    void set_message_handler(MessageCallback handler) override;
    void set_error_handler(ErrorCallback handler) override;
    void set_io_context(asio::io_context* io_ctx) override;

    int get_port() const;

    // Called by a connection when it errors so it can be dropped from any
    // registry that holds it (SSE list / legacy list).
    void unregister_sse_connection(std::shared_ptr<SseConnection> conn);

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

    void push_to_sse_connections(const nlohmann::json& message);  // legacy SSE mode

    std::shared_ptr<HttpSession> find_session(const std::string& id);
    void push_to_session_sse(const std::string& id, const nlohmann::json& message);
    void push_to_all_sessions(const nlohmann::json& message);

    std::string build_http_response(int status, const std::string& content_type,
                                      const std::string& body,
                                      std::vector<std::pair<std::string, std::string>> extra_headers = {});

    std::string status_text(int status);

    HttpTransportConfig config_;
    asio::io_context* io_ctx_ = nullptr;
    std::unique_ptr<asio::ip::tcp::acceptor> acceptor_;
    std::atomic<bool> running_{false};
    std::atomic<std::size_t> active_connection_count_{0};

    std::shared_mutex connections_mutex_;
    // Legacy SSE mode (GET /sse) streams. StreamableHttp mode uses sessions_.
    std::vector<std::shared_ptr<SseConnection>> active_sse_connections_;

    std::shared_mutex sessions_mutex_;
    std::unordered_map<std::string, std::shared_ptr<HttpSession>> sessions_;

    MessageCallback message_handler_;
    ErrorCallback error_handler_;
};

} // namespace cppmcp
