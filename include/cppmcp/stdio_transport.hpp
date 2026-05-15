#pragma once

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

#include <nlohmann/json.hpp>

#include "transport.hpp"

namespace cppmcp {

class StdioTransport : public ITransport {
public:
    StdioTransport();

    void start() override;
    void stop() override;
    bool is_running() const override;

    void send_message(const nlohmann::json& message) override;
    void set_message_handler(MessageCallback handler) override;
    void set_error_handler(ErrorCallback handler) override;

private:
    void read_loop();

    std::thread reader_thread_;
    std::atomic<bool> running_{false};
    std::mutex write_mutex_;

    std::mutex start_mutex_;
    std::condition_variable start_cv_;
    bool reader_started_{false};

    MessageCallback message_handler_;
    ErrorCallback error_handler_;
};

} // namespace cppmcp