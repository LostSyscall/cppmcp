#pragma once

#include <asio.hpp>

#include <atomic>
#include <cstddef>
#include <thread>
#include <utility>
#include <vector>

namespace cppmcp {

// A fixed-size worker thread pool backed by its own io_context. Used to run
// potentially-blocking handlers (tools/call, resources/read, prompts/get) off
// the transport's io thread so a slow handler never starves the event loop.
class RequestExecutor {
public:
    explicit RequestExecutor(std::size_t num_threads);
    ~RequestExecutor();

    RequestExecutor(const RequestExecutor&) = delete;
    RequestExecutor& operator=(const RequestExecutor&) = delete;

    void start();  // spawn the worker threads (idempotent)
    void stop();   // drop the work guard, stop the pool, join (idempotent)

    template <typename F>
    void post(F&& f) {
        asio::post(pool_io_, std::forward<F>(f));
    }

private:
    asio::io_context pool_io_;
    asio::executor_work_guard<asio::io_context::executor_type> work_guard_;
    std::vector<std::thread> threads_;
    std::size_t num_threads_;
    std::atomic<bool> started_{false};
};

} // namespace cppmcp
