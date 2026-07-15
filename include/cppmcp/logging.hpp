#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>

namespace cppmcp {

// Best-effort async logger: log() is non-blocking (bounded queue + a dedicated
// drain thread writing to stderr). Use it for hot-path diagnostics (e.g.
// per-connection messages) so the io thread never blocks on stderr. Cold-path
// startup/error messages may still use stderr directly.
class AsyncLogger {
public:
    static AsyncLogger& instance();
    void log(std::string msg);   // drops if the queue is full or already flushed
    void flush();                // stop the drain thread, flush remaining, join (idempotent)

private:
    AsyncLogger();
    ~AsyncLogger();
    AsyncLogger(const AsyncLogger&) = delete;
    AsyncLogger& operator=(const AsyncLogger&) = delete;
    void drain_loop();

    std::deque<std::string> queue_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::atomic<bool> stopping_{false};
    std::atomic<bool> flushed_{false};
    std::thread drain_thread_;
};

} // namespace cppmcp
