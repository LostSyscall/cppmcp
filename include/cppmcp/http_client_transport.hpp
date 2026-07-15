#pragma once

#include "async_write_queue.hpp"
#include "client_transport.hpp"

#include <asio.hpp>
#include <llhttp.h>
#include <nlohmann/json.hpp>

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <string>

namespace cppmcp {

// Client transport over Streamable HTTP. Maintains a persistent TCP connection,
// serializes JSON-RPC messages as POST requests through an AsyncWriteQueue, and
// parses responses with llhttp (HTTP_RESPONSE), dispatching each response body
// to the message handler. Captures the mcp-session-id from the initialize
// response and sends it on every subsequent request.
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

private:
    void init_parser();
    void flush_header();
    void start_read();
    void on_read(const asio::error_code& ec, std::size_t bytes);
    void handle_response();
    void clear_handlers();
    std::string build_post_request(const std::string& body) const;

    std::string host_;
    uint16_t port_;
    std::string endpoint_;
    asio::io_context* io_ctx_{nullptr};
    MessageCallback message_handler_;
    ErrorCallback error_handler_;
    DisconnectCallback disconnect_handler_;
    std::atomic<bool> connected_{false};
    std::atomic<bool> stopping_{false};

    std::unique_ptr<asio::ip::tcp::socket> socket_;
    std::shared_ptr<AsyncWriteQueue> write_queue_;
    asio::streambuf read_buf_;

    llhttp_t parser_;
    llhttp_settings_t settings_;
    std::string cur_header_field_;
    std::string cur_header_value_;
    std::map<std::string, std::string> response_headers_;
    std::string response_body_;
    int response_status_{0};
    bool message_complete_{false};
    std::string session_id_;
};

} // namespace cppmcp
