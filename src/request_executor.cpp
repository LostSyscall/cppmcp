#include "cppmcp/request_executor.hpp"

namespace cppmcp {

RequestExecutor::RequestExecutor(std::size_t num_threads)
    : work_guard_(pool_io_.get_executor()),
      num_threads_(num_threads == 0 ? 1 : num_threads) {}

RequestExecutor::~RequestExecutor() {
    stop();
}

void RequestExecutor::start() {
    if (started_.exchange(true)) {
        return;
    }
    threads_.reserve(num_threads_);
    for (std::size_t i = 0; i < num_threads_; ++i) {
        threads_.emplace_back([this]() {
            pool_io_.run();
        });
    }
}

void RequestExecutor::stop() {
    started_.store(false);
    work_guard_.reset();
    pool_io_.stop();
    for (auto& t : threads_) {
        if (t.joinable()) {
            t.join();
        }
    }
    threads_.clear();
}

} // namespace cppmcp
