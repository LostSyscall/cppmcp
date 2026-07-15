#pragma once

#include <asio.hpp>

#include <cstddef>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <utility>

namespace cppmcp {

// What to do when an enqueue would exceed max_queued_bytes.
enum class QueueOverflowPolicy : uint8_t {
    DropNewest,    // silently drop the newly-enqueued item (notifications/SSE)
    CloseOnError   // deactivate the queue and signal on_error_ (response writes)
};

// Serialized, socket-agnostic async write queue.
//
// Guarantees at most ONE outstanding write per descriptor (the `writing_` gate)
// and never re-arms while holding `mutex_` (no recursive lock on a
// non-recursive std::mutex). The completion handler keeps this queue alive via
// shared_from_this(); the caller-supplied `do_write` keeps the buffer alive via
// the shared_ptr<string> argument.
class AsyncWriteQueue : public std::enable_shared_from_this<AsyncWriteQueue> {
public:
    using WriteCompletion = std::function<void(const asio::error_code& ec)>;
    using DoWrite = std::function<void(std::shared_ptr<std::string> buffer, WriteCompletion completion)>;
    using OnError = std::function<void(const asio::error_code& ec)>;

    AsyncWriteQueue(DoWrite do_write, OnError on_error,
                    std::size_t max_queued_bytes = 0,
                    QueueOverflowPolicy policy = QueueOverflowPolicy::DropNewest)
        : do_write_(std::move(do_write)), on_error_(std::move(on_error)),
          max_queued_bytes_(max_queued_bytes), policy_(policy) {}

    // Thread-safe enqueue. Returns false if the item was dropped (DropNewest on
    // overflow) or the queue is inactive; true otherwise. Backpressure: when
    // the queued bytes would exceed max_queued_bytes (0 = unbounded), either
    // drop the new item or deactivate + signal on_error_ per the policy.
    bool enqueue(std::string data) {
        bool arm = false;
        bool overflow_close = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!active_) {
                return false;
            }
            if (max_queued_bytes_ > 0 && queued_bytes_ + data.size() > max_queued_bytes_) {
                if (policy_ == QueueOverflowPolicy::CloseOnError) {
                    active_ = false;
                    writing_ = false;
                    overflow_close = true;
                } else {
                    return false;  // DropNewest: drop the new item
                }
            } else {
                queued_bytes_ += data.size();
                queue_.push_back(std::move(data));
                if (!writing_) {
                    writing_ = true;
                    arm = true;
                }
            }
        }
        if (overflow_close && on_error_) {
            on_error_(asio::error::make_error_code(asio::error::basic_errors::operation_aborted));
        }
        if (arm) {
            arm_write();
        }
        return !overflow_close;
    }

    // Marks the queue inactive and discards pending data. An in-flight write
    // (if any) is allowed to finish; its completion will not re-arm.
    void shutdown() {
        std::deque<std::string> discarded;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            active_ = false;
            queued_bytes_ = 0;
            queue_.swap(discarded);
        }
    }

    bool is_active() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return active_;
    }

private:
    void arm_write() {
        // Pre-acquire a self-reference so the completion lambda below is valid
        // even if enqueue() has already returned.
        auto self = shared_from_this();
        auto buffer = std::make_shared<std::string>();
        bool do_call = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!active_ || queue_.empty()) {
                writing_ = false;
                return;
            }
            queued_bytes_ -= queue_.front().size();
            *buffer = std::move(queue_.front());
            queue_.pop_front();
            do_call = true;
        }
        if (do_call) {
            do_write_(buffer, [self](const asio::error_code& ec) {
                self->on_write_complete(ec);
            });
        }
    }

    void on_write_complete(const asio::error_code& ec) {
        if (ec) {
            bool was_active = false;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                was_active = active_;
                active_ = false;
                writing_ = false;
            }
            if (was_active && on_error_) {
                on_error_(ec);
            }
            return;
        }

        bool more = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (active_ && !queue_.empty()) {
                more = true;  // keep writing_ == true
            } else {
                writing_ = false;
            }
        }
        if (more) {
            arm_write();  // re-arm OUTSIDE the lock
        }
    }

    DoWrite do_write_;
    OnError on_error_;
    std::deque<std::string> queue_;
    mutable std::mutex mutex_;
    std::size_t max_queued_bytes_;
    QueueOverflowPolicy policy_;
    std::size_t queued_bytes_ = 0;
    bool writing_ = false;  // true while a write is outstanding
    bool active_ = true;
};

} // namespace cppmcp
