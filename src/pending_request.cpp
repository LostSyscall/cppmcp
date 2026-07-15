#include "cppmcp/pending_request.hpp"

#include "cppmcp/client.hpp"
#include "cppmcp/protocol.hpp"

namespace cppmcp {

PendingRequest::PendingRequest(RequestId id, std::string method, RequestId progress_token)
    : id(std::move(id)),
      method(std::move(method)),
      progress_token(std::move(progress_token)),
      future_(promise_.get_future()) {}

PendingRequest::~PendingRequest() {
    // The client always finish()es (and thus set_value()s the promise) before
    // removing the request from its table, so there is no broken_promise risk.
    if (timer_) {
        asio::error_code ec;
        timer_->cancel(ec);
    }
}

nlohmann::json PendingRequest::get() {
    McpOutcome o = future_.get();
    if (o.terminal == RequestState::Succeeded) {
        return std::move(o.result);
    }
    if (o.failure) {
        throw McpException(*o.failure);
    }
    if (o.rpc_error) {
        throw McpException(o.rpc_error->code, o.rpc_error->message, o.rpc_error->data);
    }
    throw McpException(Protocol::REQUEST_FAILED, "request terminated without a result");
}

void PendingRequest::cancel(std::string reason) {
    auto c = client_.lock();
    if (c) {
        c->request_cancel(id, std::move(reason));
    }
}

void PendingRequest::finish(McpOutcome outcome) {
    RequestState prev = state_.exchange(outcome.terminal, std::memory_order_acq_rel);
    if (prev != RequestState::Waiting) {
        return;
    }
    outcome_ = std::move(outcome);
    if (timer_) {
        asio::error_code ec;
        timer_->cancel(ec);
    }
    try {
        promise_.set_value(outcome_);
    } catch (const std::future_error&) {
        // promise already satisfied; defensive only
    }
    dispatch_callbacks();
}

void PendingRequest::deliver_progress(double progress, std::optional<double> total) {
    if (state_.load(std::memory_order_acquire) != RequestState::Waiting) {
        return;
    }
    if (!on_progress_) {
        return;
    }
    auto self = shared_from_this();
    auto fire = [self, progress, total]() {
        if (self->on_progress_) {
            self->on_progress_(progress, total);
        }
    };
    if (callback_executor_) {
        asio::post(*callback_executor_, std::move(fire));
    } else {
        fire();
    }
}

void PendingRequest::on_timer(const asio::error_code& ec) {
    if (ec == asio::error::operation_aborted) {
        return;
    }
    if (state_.load(std::memory_order_acquire) != RequestState::Waiting) {
        return;
    }
    auto c = client_.lock();
    if (!c) {
        return;
    }
    McpOutcome o;
    o.terminal = RequestState::TimedOut;
    o.failure = McpException(Protocol::REQUEST_TIMED_OUT, "request timed out");
    c->complete_pending(id, std::move(o));
}

void PendingRequest::dispatch_callbacks() {
    auto self = shared_from_this();
    auto fire = [self]() {
        const auto& o = self->outcome_;
        if (o.terminal == RequestState::Succeeded) {
            if (self->on_complete_) {
                self->on_complete_(o.result);
            }
        } else if (self->on_error_) {
            self->on_error_(o);
        }
    };
    if (callback_executor_) {
        asio::post(*callback_executor_, std::move(fire));
    } else {
        fire();
    }
}

} // namespace cppmcp
