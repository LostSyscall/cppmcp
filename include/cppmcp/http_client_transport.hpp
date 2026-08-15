#pragma once

#include "async_write_queue.hpp"
#include "client_transport.hpp"

#include <asio.hpp>
#include <llhttp.h>
#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace cppmcp {

// Client transport over Streamable HTTP. Maintains a persistent TCP connection,
// serializes JSON-RPC messages as POST requests through an AsyncWriteQueue, and
// parses responses with llhttp (HTTP_RESPONSE), dispatching each response body
// to the message handler. Captures the mcp-session-id from the initialize
// response and sends it on every subsequent request. Also opens the GET SSE
// stream (server->client pushes: sampling/roots/notifications) after the first
// successful POST, and terminates the session with DELETE on disconnect().
class HttpClientTransport : public IClientTransport,
                            public std::enable_shared_from_this<HttpClientTransport> {
public:
    HttpClientTransport(std::string host, uint16_t port, std::string endpoint = "/mcp");
    ~HttpClientTransport();

    HttpClientTransport(const HttpClientTransport&) = delete;
    HttpClientTransport& operator=(const HttpClientTransport&) = delete;

    void connect() override;
    void disconnect() override;
    bool is_connected() const override;
    void send_message(const nlohmann::json& message) override;

    void set_message_handler(MessageCallback handler) override;
    void set_error_handler(ErrorCallback handler) override;
    void set_disconnect_handler(DisconnectCallback handler) override;
    void set_io_context(asio::io_context* io_ctx) override;

    std::string session_id() const override { return session_id_; }

    // Extra headers (e.g. Authorization) appended to every POST/GET/DELETE.
    void set_extra_headers(std::vector<std::pair<std::string, std::string>> headers);
    // Per-response read timeout (0 = none). A stalled server then surfaces as
    // a disconnect instead of a silently dead socket.
    void set_read_timeout(std::chrono::milliseconds t) { read_timeout_ = t; }

private:
    void init_parser();
    void flush_header();
    void start_read();
    void on_read(const asio::error_code& ec, std::size_t bytes);
    void handle_response();
    void handle_sse_frame(const std::string& event, const std::string& data);
    void start_sse_get();
    void on_sse_read(const asio::error_code& ec, std::size_t bytes);
    void close_sse();
    void arm_read_timer();
    void clear_handlers();
    std::string build_post_request(std::string body) const;

    std::string host_;
    uint16_t port_;
    std::string endpoint_;
    asio::io_context* io_ctx_{nullptr};
    MessageCallback message_handler_;
    ErrorCallback error_handler_;
    DisconnectCallback disconnect_handler_;
    std::atomic<bool> connected_{false};
    std::atomic<bool> stopping_{false};
    std::vector<std::pair<std::string, std::string>> extra_headers_;
    std::chrono::milliseconds read_timeout_{0};

    std::unique_ptr<asio::ip::tcp::socket> socket_;
    std::unique_ptr<asio::ip::tcp::socket> sse_socket_;   // GET stream (separate connection)
    std::shared_ptr<AsyncWriteQueue> write_queue_;
    asio::streambuf read_buf_;
    asio::streambuf sse_buf_;
    std::string sse_event_;
    std::string sse_data_;
    bool sse_open_{false};

    llhttp_t parser_;
    llhttp_settings_t settings_;
    std::string cur_header_field_;
    std::string cur_header_value_;
    std::map<std::string, std::string> response_headers_;
    std::string response_body_;
    int response_status_{0};
    bool message_complete_{false};
    std::string session_id_;
    std::size_t max_body_size_{16 * 1024 * 1024};  // cap response body (client-side DoS guard)
};

} // namespace cppmcp
