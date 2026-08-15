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
// Fully async on both platforms: reads via async_read_until, writes via an
// AsyncWriteQueue. On Windows the parent-side pipe handles are created with
// FILE_FLAG_OVERLAPPED (see Process) so asio can drive them directly — no
// reader thread, no detached thread at shutdown.
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

    // Extra environment variables for the child (merged over the parent env),
    // and an optional working directory. Must be set before connect().
    void set_env(std::map<std::string, std::string> env) { env_ = std::move(env); }
    void set_working_dir(std::string cwd) { cwd_ = std::move(cwd); }
    // Child exit code after disconnect (-1 when the child was killed).
    int exit_code() const { return exit_code_; }

private:
    void handle_line(const std::string& line);
    void clear_handlers();
    void do_read();
    void on_read(const asio::error_code& ec, std::size_t bytes);

    std::string executable_;
    std::vector<std::string> args_;
    std::map<std::string, std::string> env_;
    std::string cwd_;
    int exit_code_ = -1;
    Process process_;
    asio::io_context* io_ctx_{nullptr};
    MessageCallback message_handler_;
    ErrorCallback error_handler_;
    DisconnectCallback disconnect_handler_;
    std::atomic<bool> connected_{false};
    std::atomic<bool> stopping_{false};

#ifdef _WIN32
    std::unique_ptr<asio::windows::stream_handle> stdin_handle_;
    std::unique_ptr<asio::windows::stream_handle> stdout_handle_;
#else
    std::unique_ptr<asio::posix::stream_descriptor> stdin_desc_;
    std::unique_ptr<asio::posix::stream_descriptor> stdout_desc_;
#endif
    asio::streambuf read_buf_;
    std::shared_ptr<AsyncWriteQueue> write_queue_;

    static constexpr std::size_t max_line_size_ = 16 * 1024 * 1024;
};

} // namespace cppmcp
