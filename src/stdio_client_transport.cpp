#include "cppmcp/stdio_client_transport.hpp"
#include "cppmcp/logging.hpp"

#include <utility>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace cppmcp {

StdioClientTransport::StdioClientTransport(std::string executable, std::vector<std::string> args)
    : executable_(std::move(executable)), args_(std::move(args)) {}

StdioClientTransport::~StdioClientTransport() {
    disconnect();
}

void StdioClientTransport::set_message_handler(MessageCallback handler) {
    message_handler_ = std::move(handler);
}
void StdioClientTransport::set_error_handler(ErrorCallback handler) {
    error_handler_ = std::move(handler);
}
void StdioClientTransport::set_disconnect_handler(DisconnectCallback handler) {
    disconnect_handler_ = std::move(handler);
}
void StdioClientTransport::set_io_context(asio::io_context* io_ctx) {
    io_ctx_ = io_ctx;
}

bool StdioClientTransport::is_connected() const {
    return connected_.load();
}

void StdioClientTransport::connect() {
    if (connected_.exchange(true)) {
        return;
    }
    if (!io_ctx_) {
        connected_.store(false);
        if (error_handler_) error_handler_("StdioClientTransport: no io_context");
        return;
    }
    stopping_.store(false);
    try {
        process_.spawn(executable_, args_, env_, cwd_);
    } catch (...) {
        connected_.store(false);  // restore state so a later connect() can retry
        throw;
    }

    auto self = shared_from_this();

#ifdef _WIN32
    stdin_handle_ = std::make_unique<asio::windows::stream_handle>(*io_ctx_);
    stdout_handle_ = std::make_unique<asio::windows::stream_handle>(*io_ctx_);
    asio::error_code e1, e2;
    stdin_handle_->assign(process_.stdin_write(), e1);
    stdout_handle_->assign(process_.stdout_read(), e2);
    if (e1 || e2) {
        connected_.store(false);
        if (error_handler_) error_handler_("StdioClientTransport: handle assign failed");
        disconnect();
        return;
    }
    // Ownership of the two parent-side handles now belongs to the stream_handles;
    // Process must NOT close them on terminate. Mark by clearing them in Process.
    process_.release_stdin();
    process_.release_stdout();
#else
    stdin_desc_ = std::make_unique<asio::posix::stream_descriptor>(*io_ctx_, ::dup(process_.stdin_fd()));
    stdout_desc_ = std::make_unique<asio::posix::stream_descriptor>(*io_ctx_, ::dup(process_.stdout_fd()));
#endif

    write_queue_ = std::make_shared<AsyncWriteQueue>(
        [self](std::shared_ptr<std::string> buffer, AsyncWriteQueue::WriteCompletion completion) {
#ifdef _WIN32
            asio::async_write(*self->stdin_handle_, asio::buffer(*buffer),
                [buffer, completion](const asio::error_code& ec, std::size_t) { completion(ec); });
#else
            asio::async_write(*self->stdin_desc_, asio::buffer(*buffer),
                [buffer, completion](const asio::error_code& ec, std::size_t) { completion(ec); });
#endif
        },
        [self](const asio::error_code&) {
            if (self->error_handler_) {
                self->error_handler_("stdio write error");
            }
            // A write error means the child is gone; trigger disconnect path.
            self->on_read(asio::error::operation_aborted, 0);
        });

    do_read();
}

void StdioClientTransport::disconnect() {
    stopping_.store(true);
    bool was_connected = connected_.exchange(false);

    if (write_queue_) {
        write_queue_->shutdown();
    }
#ifdef _WIN32
    if (stdout_handle_) {
        asio::error_code ec;
        stdout_handle_->close(ec);  // cancels in-flight async_read_until
    }
    if (stdin_handle_) {
        asio::error_code ec;
        stdin_handle_->close(ec);  // signals EOF -> graceful child exit
    }
#else
    if (stdin_desc_) {
        asio::error_code ec;
        stdin_desc_->close(ec);
    }
    if (stdout_desc_) {
        asio::error_code ec;
        stdout_desc_->close(ec);
    }
#endif
    // MCP shutdown convention: closing stdin lets the child exit cleanly.
    // Give it a grace window, then escalate to a forceful kill.
    if (was_connected && !process_.wait_for_exit(2000)) {
        process_.terminate();
        exit_code_ = -1;
    } else if (was_connected) {
        exit_code_ = process_.exit_code();
    }

    if (was_connected || message_handler_ || error_handler_ || disconnect_handler_) {
        if (io_ctx_ && (message_handler_ || error_handler_ || disconnect_handler_)) {
            // Defer clearing so the in-flight read completion (triggered by close)
            // can still invoke disconnect_handler_.
            auto self = shared_from_this();
            asio::post(*io_ctx_, [self]() { self->clear_handlers(); });
        } else {
            clear_handlers();
        }
    }
}

void StdioClientTransport::clear_handlers() {
    message_handler_ = {};
    error_handler_ = {};
    disconnect_handler_ = {};
}

void StdioClientTransport::send_message(const nlohmann::json& message) {
    if (!write_queue_) {
        return;
    }
    std::string data = message.dump() + "\n";
    if (data.size() > max_line_size_) {
        if (error_handler_) error_handler_("stdio message too large");
        return;
    }
    write_queue_->enqueue(std::move(data));
}

void StdioClientTransport::handle_line(const std::string& line) {
    if (line.size() > max_line_size_) {
        if (error_handler_) error_handler_("stdio line too large, discarded");
        return;
    }
    try {
        auto j = nlohmann::json::parse(line);
        if (message_handler_) {
            message_handler_(j);
        }
    } catch (const std::exception& e) {
        AsyncLogger::instance().log(std::string("stdio parse error: ") + e.what());
        if (error_handler_) {
            error_handler_("stdio parse error");
        }
    }
}

void StdioClientTransport::do_read() {
    // Cap BEFORE arming async_read_until: without this, a peer sending data
    // with no '\n' grows read_buf_ unboundedly (the post-hoc line-size check
    // in on_read fires too late to bound memory).
    if (read_buf_.size() > max_line_size_) {
        if (error_handler_) error_handler_("stdio inbound line exceeds size cap, dropping connection");
        connected_.store(false);
        if (disconnect_handler_ && io_ctx_) {
            auto self = shared_from_this();
            asio::post(*io_ctx_, [self]() {
                if (self->disconnect_handler_) self->disconnect_handler_();
            });
        }
        return;
    }
#ifdef _WIN32
    if (!stdout_handle_ || stopping_.load()) {
        return;
    }
    auto self = shared_from_this();
    asio::async_read_until(*stdout_handle_, read_buf_, '\n',
        [self](const asio::error_code& ec, std::size_t bytes) { self->on_read(ec, bytes); });
#else
    if (!stdout_desc_ || stopping_.load()) {
        return;
    }
    auto self = shared_from_this();
    asio::async_read_until(*stdout_desc_, read_buf_, '\n',
        [self](const asio::error_code& ec, std::size_t bytes) { self->on_read(ec, bytes); });
#endif
}

void StdioClientTransport::on_read(const asio::error_code& ec, std::size_t bytes) {
    if (ec) {
        connected_.store(false);
        if (disconnect_handler_ && io_ctx_) {
            auto self = shared_from_this();
            asio::post(*io_ctx_, [self]() {
                if (self->disconnect_handler_) self->disconnect_handler_();
            });
        }
        return;
    }
    // Bulk-extract the line (buffer sequence may be segmented).
    std::string line;
    line.reserve(bytes);
    auto bufs = read_buf_.data();
    for (auto it = asio::buffers_begin(bufs); it != asio::buffers_begin(bufs) + bytes; ++it) {
        line.push_back(*it);
    }
    read_buf_.consume(bytes);
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) {
        line.pop_back();
    }
    if (!line.empty()) {
        handle_line(line);
    }
    if (!stopping_.load()) {
        do_read();
    }
}

} // namespace cppmcp
