#pragma once

#include <asio.hpp>

#include <atomic>
#include <chrono>
#include <functional>
#include <future>
#include <memory>
#include <optional>
#include <string>

#include <nlohmann/json.hpp>

#include "common.hpp"
#include "exception.hpp"
#include "jsonrpc.hpp"

namespace cppmcp {

class McpClient;
class RequestBuilder;

enum class RequestState : uint8_t {
    Waiting,
    Succeeded,   // got success response
    Errored,     // got error response
    TimedOut,    // steady_timer expired
    Cancelled,   // user cancelled
    Failed       // transport disconnected / client shutting down
};

// Unified terminal result shared by the future, on_complete and on_error paths.
struct McpOutcome {
    RequestState terminal = RequestState::Waiting;
    nlohmann::json result;                          // valid when Succeeded
    std::optional<JsonRpcErrorDetail> rpc_error;    // valid when Errored
    std::optional<McpException> failure;            // valid when TimedOut/Cancelled/Failed
};

using CompleteCallback = std::function<void(const nlohmann::json&)>;
using OutcomeCallback = std::function<void(const McpOutcome&)>;
using ProgressCallback = std::function<void(double, std::optional<double>)>;

// A pending outbound request. Its mutable state machine changes ONLY within the
// owning McpClient's strand; user threads interact solely via the std::future
// (internally synchronized) and via cancel()/state(). All async handlers arm
// with shared_from_this() so the object outlives its in-flight operations.
class PendingRequest : public std::enable_shared_from_this<PendingRequest> {
public:
    ~PendingRequest();

    const RequestId id;
    const std::string method;
    const RequestId progress_token;   // NullId when no progress; else == id or a unique token

    // ---- user-thread API ----
    // Block until terminal. Returns result on success; throws McpException on
    // any non-Succeeded terminal (server error / timeout / cancel / disconnect).
    nlohmann::json get();
    template <class Rep, class Period>
    std::future_status wait_for(const std::chrono::duration<Rep, Period>& timeout) {
        return future_.wait_for(timeout);
    }
    // Post a cancel onto the client strand; idempotent. Fires on_error with
    // RequestState::Cancelled and sends notifications/cancelled to the server.
    void cancel(std::string reason = "");
    RequestState state() const noexcept { return state_.load(std::memory_order_acquire); }

private:
    friend class McpClient;
    friend class RequestBuilder;

    PendingRequest(RequestId id, std::string method, RequestId progress_token);

    // ---- strand-only methods (called by McpClient within its strand) ----
    // Single terminal write point: cancel timer, set promise, dispatch callbacks.
    void finish(McpOutcome outcome);
    void deliver_progress(double progress, std::optional<double> total);
    void on_timer(const asio::error_code& ec);
    void dispatch_callbacks();

    // Arm the timeout timer on a given executor (typically the client strand).
    template <class Executor>
    void arm_timer(const Executor& executor) {
        if (timeout_.count() <= 0) {
            return;
        }
        timer_ = std::make_unique<asio::steady_timer>(executor, timeout_);
        auto self = shared_from_this();
        timer_->async_wait(asio::bind_executor(executor, [self](const asio::error_code& ec) {
            self->on_timer(ec);
        }));
    }

    std::atomic<RequestState> state_{RequestState::Waiting};
    std::unique_ptr<asio::steady_timer> timer_;
    std::chrono::milliseconds timeout_{0};
    CompleteCallback on_complete_;
    OutcomeCallback on_error_;
    ProgressCallback on_progress_;
    McpOutcome outcome_;

    std::promise<McpOutcome> promise_;
    std::future<McpOutcome> future_;

    std::weak_ptr<McpClient> client_;
    // Empty => dispatch callbacks inline on the strand; else post to this executor.
    std::optional<asio::any_io_executor> callback_executor_;
};

} // namespace cppmcp
