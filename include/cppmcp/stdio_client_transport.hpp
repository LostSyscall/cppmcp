#pragma once

#include "async_write_queue.hpp"
#include "client_transport.hpp"
#include "process.hpp"

#include <asio.hpp>
#include <nlohmann/json.hpp>

#include <atomic>
#include <cstddef>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace cppmcp {

// Client transport over a child process's stdin/stdout. Spawns a server as a
// child process and speaks newline-delimited JSON-RPC with it.
//
// Windows: anonymous pipes are not overlapped-capable, so reading runs on a
// thin reader thread that posts each line into the io loop, and writes are
// serialized through the io loop with sync WriteFile. Unix: posix
// stream_descriptor with async_read_until + AsyncWriteQueue (fully async).
class StdioClientTransport : public IClientTransport,
                             public std::enable_shared_from_this<StdioClientTransport> {
public:
    StdioClientTransport(std::string executable, std::vector<std::string> args = {});
    ~StdioClientTransport();

    StdioClientTransport(const StdioClientTransport&) = delete;
    StdioClientTransport& operator=(const StdioClientTransport&) = delete;

    void connect() override;
    void disconnect() override;
    bool is_connected() const override;
    void send_message(const nlohmann::json& message) override;

    void set_message_handler(MessageCallback handler) override;
    void set_error_handler(ErrorCallback handler) override;
    void set_disconnect_handler(DisconnectCallback handler) override;
    void set_io_context(asio::io_context* io_ctx) override;

private:
    void handle_line(const std::string& line);
    void clear_handlers();

#ifdef _WIN32
    void win32_read_loop();
#else
    void do_read();
    void on_read(const asio::error_code& ec, std::size_t bytes);
#endif

    std::string executable_;
    std::vector<std::string> args_;
    Process process_;
    asio::io_context* io_ctx_{nullptr};
    MessageCallback message_handler_;
    ErrorCallback error_handler_;
    DisconnectCallback disconnect_handler_;
    std::atomic<bool> connected_{false};
    std::atomic<bool> stopping_{false};
    std::mutex write_mutex_;

#ifdef _WIN32
    std::thread reader_thread_;
#else
    std::unique_ptr<asio::posix::stream_descriptor> stdin_desc_;
    std::unique_ptr<asio::posix::stream_descriptor> stdout_desc_;
    asio::streambuf read_buf_;
    std::shared_ptr<AsyncWriteQueue> write_queue_;
#endif

    static constexpr std::size_t max_line_size_ = 16 * 1024 * 1024;
};

} // namespace cppmcp
