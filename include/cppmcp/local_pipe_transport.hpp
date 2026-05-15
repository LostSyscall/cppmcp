#pragma once

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "transport.hpp"

namespace cppmcp {

enum class PipeMode {
    SingleClient,   // 1 connection at a time
    MultiClient     // N simultaneous connections
};

struct LocalPipeConfig {
    std::string pipe_name = "cppmcp";  // Windows: \\.\pipe\cppmcp, Unix: /tmp/cppmcp.sock
    PipeMode mode = PipeMode::SingleClient;
    int max_instances = 4;             // Max simultaneous connections (MultiClient)
    int buffer_size = 65536;           // Read/write buffer size in bytes
    int poll_interval_ms = 500;        // Wait interval for running_ checks (WaitForSingleObjectEx, poll, stop_cv)
};

// Per-connection state (platform-agnostic handle type)
struct LocalPipeConnection {
#ifdef _WIN32
    using HandleType = void*;  // HANDLE
    static constexpr HandleType INVALID_HANDLE = nullptr;
#else
    using HandleType = int;
    static constexpr HandleType INVALID_HANDLE = -1;
#endif

    HandleType handle = INVALID_HANDLE;
    std::thread reader_thread;
    std::mutex write_mutex;
    std::atomic<bool> active{false};
    int connection_id = 0;

#ifdef _WIN32
    void* read_event = nullptr;   // Manual-reset event for overlapped reads
    void* write_event = nullptr;  // Manual-reset event for overlapped writes
#else
    std::string read_buffer;      // Line-based framing accumulator
#endif

    LocalPipeConnection(HandleType h, int id) : handle(h), connection_id(id) {}

    bool write_message(const std::string& data);
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

private:
    std::string resolve_pipe_path() const;

    void accept_loop();
    void client_read_loop(std::shared_ptr<LocalPipeConnection> conn);
    std::string read_message(std::shared_ptr<LocalPipeConnection> conn);

    LocalPipeConfig config_;
    std::atomic<bool> running_{false};
    std::atomic<int> next_connection_id_{0};

    std::mutex responding_mutex_;
    std::shared_ptr<LocalPipeConnection> responding_connection_;

    std::thread accept_thread_;

#ifdef _WIN32
    void* accept_event_ = nullptr;  // Manual-reset event for overlapped ConnectNamedPipe
#else
    int listen_fd_ = -1;
#endif

    std::mutex connections_mutex_;
    std::vector<std::shared_ptr<LocalPipeConnection>> active_connections_;

    std::mutex stop_mutex_;
    std::condition_variable stop_cv_;  // Replaces busy-wait on stop signal

    MessageCallback message_handler_;
    ErrorCallback error_handler_;
};

} // namespace cppmcp