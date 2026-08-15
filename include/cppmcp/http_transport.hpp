#pragma once

#include <atomic>
#include <chrono>
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
    // Address the acceptor binds to. Use "127.0.0.1" (default) to stay
    // loopback-only, "0.0.0.0" to listen on all interfaces, or a specific
    // local address. Also participates in Host/Origin validation.
    std::string host = "127.0.0.1";
    int port = 3000;
    std::string path = "/mcp";
    std::string sse_path = "/sse";
    std::string message_path = "/messages";
    size_t max_body_size = 1024 * 1024;
    size_t write_queue_max_bytes = 0;
    QueueOverflowPolicy write_queue_overflow = QueueOverflowPolicy::DropNewest;
    size_t max_connections = 1024;  // 0 = unlimited
    // Cap on request header count and per-header/URL length (DoS guard).
    size_t max_header_count = 100;
    size_t max_header_line_size = 16 * 1024;
    // Idle timeout while reading a request (slowloris guard). 0 = disabled.
    // Not applied to long-lived SSE streams (see session_idle_timeout).
    std::chrono::milliseconds read_timeout{30000};
    // Cap on concurrent MCP sessions (one per initialize). 0 = unlimited.
    size_t max_sessions = 256;
    // Idle TTL for a session with no activity. Defaults to 1h; 0 = disabled.
    // Expired sessions are reaped by a background sweep timer so abandoned
    // clients cannot permanently exhaust max_sessions.
    std::chrono::milliseconds session_idle_timeout{3600000};
    // Reject requests whose Host header does not match the configured host[:port].
    // Defaults to true; disable only for exotic proxy setups. Guards against
    // DNS-rebinding: a browser resolving an attacker domain to 127.0.0.1 still
    // sends the attacker's Host, which won't match.
    bool enforce_host_header = true;
};

struct HttpConnection : std::enable_shared_from_this<HttpConnection> {
    asio::ip::tcp::socket socket;
    asio::strand<asio::any_io_executor> strand_;
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
    // True once this connection has been promoted to an SSE stream — read
    // timeouts do not apply (SSE is a long-lived idle stream).
    bool is_streaming = false;
    size_t max_body_size = 1024 * 1024;
    size_t max_header_count = 100;
    size_t max_header_line_size = 16 * 1024;
    asio::steady_timer read_timer_;
    std::chrono::milliseconds read_timeout_{0};

    // Single serialized write queue for this connection's socket. Shared with
    // any SseConnection that wraps this HttpConnection, so handshake headers
    // and SSE frames never issue overlapping async_writes on the same socket.
    std::shared_ptr<AsyncWriteQueue> write_queue_;
    // Invoked exactly once when the connection becomes inactive (read/write
    // failure or shutdown). Always runs, regardless of who overwrites
    // on_disconnect — used for connection-count bookkeeping that must not be
    // skipped. on_disconnect (below) is for transport-level SSE cleanup and
    // MAY be replaced by SSE handlers.
    std::function<void()> on_release;
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

    // Enqueue an already-framed SSE event (e.g. "data: ...\n\n"). Caller frames once.
    void push(const std::string& frame);  // forwards to conn->write_queue_
};

// A Streamable HTTP session: one per initialize. Owns the SSE streams opened
// against it so notifications can be routed to the right client.
struct HttpSession {
    std::string session_id;
    std::atomic<bool> initialized{false};
    std::list<std::shared_ptr<SseConnection>> sse_connections;  // guarded by sessions_mutex_
    // Last activity timestamp (ms since epoch-ish; monotonic via steady_clock
    // on the transport). Updated under sessions_mutex_ on each request.
    std::chrono::steady_clock::time_point last_activity;
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
    void handle_post_batch(std::shared_ptr<HttpConnection> conn, const nlohmann::json& batch, const std::string& req_session_id);
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

    // Create and register a session. Returns nullptr if max_sessions is exceeded.
    std::shared_ptr<HttpSession> create_session();
    // Periodically reap sessions idle longer than session_idle_timeout.
    void arm_session_sweep();
    void sweep_sessions(const asio::error_code& ec);

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
    std::unique_ptr<asio::steady_timer> session_sweep_timer_;

    MessageCallback message_handler_;
    ErrorCallback error_handler_;
};

} // namespace cppmcp
