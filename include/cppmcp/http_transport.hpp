#pragma once

#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>

#include <httplib.h>
#include <nlohmann/json.hpp>

#include "transport.hpp"

namespace cppmcp {

enum class HttpTransportMode {
    SSE,              // Legacy: GET /sse + POST /messages
    StreamableHttp    // Modern: POST /mcp + GET /mcp SSE + DELETE /mcp
};

struct HttpTransportConfig {
    HttpTransportMode mode = HttpTransportMode::StreamableHttp;
    std::string host = "127.0.0.1";
    int port = 3000;
    std::string path = "/mcp";
    std::string sse_path = "/sse";
    std::string message_path = "/messages";
    size_t max_body_size = 1024 * 1024; // 1MB default
};

class HttpTransport : public ITransport {
public:
    explicit HttpTransport(const HttpTransportConfig& config = {});
    ~HttpTransport() override;

    void start() override;
    void stop() override;
    bool is_running() const override;

    void send_message(const nlohmann::json& message) override;
    void set_message_handler(MessageCallback handler) override;
    void set_error_handler(ErrorCallback handler) override;
    void set_response_sender(ResponseSender sender) override;

private:
    struct SseConnection {
        std::queue<std::string> message_queue;
        std::mutex queue_mutex;
        std::condition_variable queue_cv;
        std::atomic<bool> active{true};

        void push(const std::string& data) {
            {
                std::lock_guard<std::mutex> lock(queue_mutex);
                message_queue.push(data);
            }
            queue_cv.notify_one();
        }

        bool wait_and_pop(std::string& data, int timeout_ms) {
            std::unique_lock<std::mutex> lock(queue_mutex);
            return queue_cv.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                [this] { return !message_queue.empty() || !active.load(); })
                && !message_queue.empty();
        }

        void drain(std::vector<std::string>& out) {
            std::lock_guard<std::mutex> lock(queue_mutex);
            while (!message_queue.empty()) {
                out.push_back(std::move(message_queue.front()));
                message_queue.pop();
            }
        }
    };

    // Streamable HTTP handlers
    void handle_post(const httplib::Request& req, httplib::Response& res);
    void handle_get_sse(const httplib::Request& req, httplib::Response& res);
    void handle_delete(const httplib::Request& req, httplib::Response& res);

    // Legacy SSE handlers
    void handle_sse_connect(const httplib::Request& req, httplib::Response& res);
    void handle_sse_message(const httplib::Request& req, httplib::Response& res);

    void push_to_sse_connections(const nlohmann::json& message);

    HttpTransportConfig config_;
    std::unique_ptr<httplib::Server> http_server_;
    std::thread server_thread_;
    std::atomic<bool> running_{false};

    std::mutex connections_mutex_;
    std::vector<std::shared_ptr<SseConnection>> active_connections_;

    std::mutex session_mutex_;
    std::string current_session_id_;
    std::atomic<bool> session_initialized_{false};

    MessageCallback message_handler_;
    ErrorCallback error_handler_;
};

} // namespace cppmcp