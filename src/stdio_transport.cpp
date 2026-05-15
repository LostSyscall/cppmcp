#include "cppmcp/stdio_transport.hpp"
#include "cppmcp/jsonrpc.hpp"

#include <iostream>

namespace cppmcp {

StdioTransport::StdioTransport() {}

void StdioTransport::start() {
    running_ = true;
    reader_thread_ = std::thread(&StdioTransport::read_loop, this);
    // Wait for reader thread to signal it has started
    std::unique_lock<std::mutex> lock(start_mutex_);
    start_cv_.wait(lock, [this] { return reader_started_; });
}

void StdioTransport::stop() {
    running_ = false;
    if (reader_thread_.joinable()) {
        reader_thread_.join();
    }
}

bool StdioTransport::is_running() const {
    return running_;
}

void StdioTransport::read_loop() {
    // Signal that reader thread has started
    {
        std::lock_guard<std::mutex> lock(start_mutex_);
        reader_started_ = true;
    }
    start_cv_.notify_one();

    std::string line;
    while (running_ && std::getline(std::cin, line)) {
        if (line.empty()) continue;

        try {
            auto json_msg = nlohmann::json::parse(line);
            if (message_handler_) {
                message_handler_(json_msg);
            }
        } catch (const nlohmann::json::parse_error& e) {
            auto error_resp = make_error_response_null_id(Protocol::PARSE_ERROR, e.what());
            send_message(error_resp);
            if (error_handler_) {
                error_handler_("JSON parse error: " + std::string(e.what()));
            }
        } catch (const std::exception& e) {
            if (error_handler_) {
                error_handler_("Error processing message: " + std::string(e.what()));
            }
        }
    }
    // Mark as stopped whether from EOF (stdin closed) or explicit stop()
    running_ = false;
}

void StdioTransport::send_message(const nlohmann::json& message) {
    std::lock_guard<std::mutex> lock(write_mutex_);
    std::cout << message.dump() << std::endl;
}

void StdioTransport::set_message_handler(MessageCallback handler) {
    message_handler_ = std::move(handler);
}

void StdioTransport::set_error_handler(ErrorCallback handler) {
    error_handler_ = std::move(handler);
}

} // namespace cppmcp