#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <shared_mutex>
#include <string>
#include <thread>
#include <vector>

#include <asio.hpp>
#include <nlohmann/json.hpp>

#include "async_write_queue.hpp"
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
    size_t max_line_size = 4 * 1024 * 1024;
    size_t write_queue_max_bytes = 0;
    QueueOverflowPolicy write_queue_overflow = QueueOverflowPolicy::DropNewest;
};

// Per-connection state for asio-based async I/O
struct PipeConnection : std::enable_shared_from_this<PipeConnection> {
#ifdef _WIN32
    asio::windows::stream_handle stream_handle;
#else
    asio::local::stream_protocol::socket socket;
#endif
    asio::strand<asio::any_io_executor> strand_;
    asio::streambuf read_buf{64 * 1024 * 1024};  // hard cap; watermark check drops oversized peers first
    std::shared_ptr<AsyncWriteQueue> write_queue;
    std::atomic<bool> active{true};
    int connection_id = 0;
    size_t max_line_size = 4 * 1024 * 1024;
    std::function<void()> on_disconnect;

    ITransport::MessageCallback message_handler;
    ITransport::ErrorCallback error_handler;

#ifdef _WIN32
    explicit PipeConnection(asio::io_context& io_ctx, int id);
#else
    explicit PipeConnection(asio::local::stream_protocol::socket s, int id);
#endif

    void start_read();
    void handle_read_completion(const asio::error_code& ec, std::size_t bytes_transferred);
    void handle_line(const std::string& line);
    void init_write_queue(std::size_t max_queued_bytes, QueueOverflowPolicy policy);
    void on_error();           // read/write failure -> deactivate + unregister
    void enqueue_write(std::string data);
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
    void set_io_context(asio::io_context* io_ctx) override;

    void unregister_connection(std::shared_ptr<PipeConnection> conn);

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
