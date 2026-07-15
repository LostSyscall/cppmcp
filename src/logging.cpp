#include "cppmcp/logging.hpp"

#include <iostream>
#include <utility>

namespace cppmcp {

AsyncLogger::AsyncLogger() : drain_thread_(&AsyncLogger::drain_loop, this) {}

AsyncLogger::~AsyncLogger() {
    flush();
}

AsyncLogger& AsyncLogger::instance() {
    static AsyncLogger inst;
    return inst;
}

void AsyncLogger::log(std::string msg) {
    if (flushed_.load(std::memory_order_relaxed)) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (queue_.size() >= 1024) {
            return;  // bound: drop on overflow to stay non-blocking
        }
        queue_.push_back(std::move(msg));
    }
    cv_.notify_one();
}

void AsyncLogger::drain_loop() {
    while (true) {
        std::string msg;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this]() {
                return stopping_.load(std::memory_order_relaxed) || !queue_.empty();
            });
            if (queue_.empty()) {
                if (stopping_.load(std::memory_order_relaxed)) {
                    return;
                }
                continue;
            }
            msg = std::move(queue_.front());
            queue_.pop_front();
        }
        std::cerr << msg << '\n';
    }
}

void AsyncLogger::flush() {
    bool expected = false;
    if (!flushed_.compare_exchange_strong(expected, true)) {
        return;  // already flushed
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_.store(true, std::memory_order_relaxed);
    }
    cv_.notify_all();
    if (drain_thread_.joinable()) {
        drain_thread_.join();
    }
    // Flush anything enqueued between the drain exit and now.
    std::deque<std::string> remaining;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        remaining.swap(queue_);
    }
    for (auto& m : remaining) {
        std::cerr << m << '\n';
    }
}

} // namespace cppmcp
