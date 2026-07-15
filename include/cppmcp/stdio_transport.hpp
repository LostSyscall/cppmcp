#pragma once

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

#include <asio.hpp>
#include <nlohmann/json.hpp>

#include "transport.hpp"

namespace cppmcp {

class StdioTransport : public std::enable_shared_from_this<StdioTransport>, public ITransport {
public:
    StdioTransport();
    ~StdioTransport() override;

    void start() override;
    void stop() override;
    bool is_running() const override;

    void send_message(const nlohmann::json& message) override;
    void set_message_handler(MessageCallback handler) override;
    void set_error_handler(ErrorCallback handler) override;
    void set_disconnect_handler(DisconnectCallback handler) override {
        disconnect_handler_ = std::move(handler);
    }
    void set_io_context(asio::io_context* io_ctx) override;

private:
    void do_read();
    void on_read(const asio::error_code& ec, std::size_t bytes_transferred);
    void handle_line(const std::string& line);

#ifdef _WIN32
    void win32_read_loop();
    asio::io_context* io_ctx_ = nullptr;
    std::thread win32_reader_thread_;
#else
    asio::io_context* io_ctx_ = nullptr;
    std::unique_ptr<asio::posix::stream_descriptor> stdin_desc_;
    asio::streambuf read_buf_;
#endif

    std::atomic<bool> running_{false};
    size_t max_line_size = 4 * 1024 * 1024;
    std::mutex write_mutex_;

    MessageCallback message_handler_;
    ErrorCallback error_handler_;
    DisconnectCallback disconnect_handler_;
};

} // namespace cppmcp