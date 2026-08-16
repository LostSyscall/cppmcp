#pragma once

#include "async_write_queue.hpp"
#include "client_transport.hpp"

#include <asio.hpp>
#include <nlohmann/json.hpp>

#include <atomic>
#include <cstddef>
#include <memory>
#include <string>

namespace cppmcp {

// Client transport over a local pipe: Windows named pipe (client end) or a Unix
// domain socket. Fully async: async_read_until('\n') + AsyncWriteQueue.
class LocalPipeClientTransport : public IClientTransport,
                                 public std::enable_shared_from_this<LocalPipeClientTransport> {
public:
    explicit LocalPipeClientTransport(std::string pipe_name);
    ~LocalPipeClientTransport();

    LocalPipeClientTransport(const LocalPipeClientTransport&) = delete;
    LocalPipeClientTransport& operator=(const LocalPipeClientTransport&) = delete;

    void connect() override;
    void disconnect() override;
    bool is_connected() const override;
    void send_message(const nlohmann::json& message) override;

    void set_message_handler(MessageCallback handler) override;
    void set_error_handler(ErrorCallback handler) override;
    void set_disconnect_handler(DisconnectCallback handler) override;
    void set_io_context(asio::io_context* io_ctx) override;

private:
    void do_read();
    void on_read(const asio::error_code& ec, std::size_t bytes);
    void handle_line(const std::string& line);
    void clear_handlers();
    std::string resolve_path() const;

    std::string pipe_name_;
    asio::io_context* io_ctx_{nullptr};
    MessageCallback message_handler_;
    ErrorCallback error_handler_;
    DisconnectCallback disconnect_handler_;
    std::atomic<bool> connected_{false};
    std::atomic<bool> stopping_{false};

#ifdef _WIN32
    std::unique_ptr<asio::windows::stream_handle> handle_;
#else
    std::unique_ptr<asio::local::stream_protocol::socket> socket_;
#endif
    // Bounded buffer (see StdioClientTransport for the rationale/headroom).
    static constexpr std::size_t max_line_size_ = 16 * 1024 * 1024;
    asio::streambuf read_buf_{max_line_size_ + 1024 * 1024};
    std::shared_ptr<AsyncWriteQueue> write_queue_;
};

} // namespace cppmcp
