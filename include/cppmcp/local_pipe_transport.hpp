#pragma once

#include <atomic>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <thread>
#include <vector>

#include <asio.hpp>
#include <nlohmann/json.hpp>

#include "transport.hpp"

namespace cppmcp {

enum class PipeMode {
    SingleClient,
    MultiClient
};

struct LocalPipeConfig {
    std::string pipe_name = "cppmcp";
    PipeMode mode = PipeMode::SingleClient;
    int max_instances = 4;
    size_t buffer_size = 65536;
};

// Per-connection state for asio-based async I/O
struct PipeConnection {
#ifdef _WIN32
    asio::windows::stream_handle stream_handle;
#else
    asio::local::stream_protocol::socket socket;
#endif
    asio::streambuf read_buf;
    std::deque<std::string> write_queue;
    std::mutex write_mutex;
    std::atomic<bool> active{true};
    int connection_id = 0;

    ITransport::MessageCallback message_handler;
    ITransport::ErrorCallback error_handler;

#ifdef _WIN32
    explicit PipeConnection(asio::io_context& io_ctx, int id);
#else
    explicit PipeConnection(asio::local::stream_protocol::socket s, int id);
#endif

    void start_read();
    void handle_line(const std::string& line);
    void enqueue_write(std::string data);
    void start_write();
};

class LocalPipeTransport : public ITransport {
public:
    explicit LocalPipeTransport(const LocalPipeConfig& config = {});
    ~LocalPipeTransport() override;

    void start() override;
    void stop() override;
    bool is_running() const override;

    void send_message(const nlohmann::json& message) override;
    void set_message_handler(MessageCallback handler) override;
    void set_error_handler(ErrorCallback handler) override;
    void set_response_sender(ResponseSender sender) override;
    void set_io_context(asio::io_context* io_ctx) override;

private:
    std::string resolve_pipe_path() const;

#ifdef _WIN32
    void win32_accept_loop();
    std::thread win32_accept_thread_;
#else
    void do_accept();
    std::unique_ptr<asio::local::stream_protocol::acceptor> acceptor_;
#endif

    LocalPipeConfig config_;
    asio::io_context* io_ctx_ = nullptr;
    std::atomic<bool> running_{false};
    std::atomic<int> next_connection_id_{0};

    std::shared_mutex connections_mutex_;
    std::vector<std::shared_ptr<PipeConnection>> active_connections_;

    MessageCallback message_handler_;
    ErrorCallback error_handler_;
};

} // namespace cppmcp