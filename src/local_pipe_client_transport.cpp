#include "cppmcp/local_pipe_client_transport.hpp"

#include <chrono>
#include <stdexcept>
#include <thread>
#include <utility>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <asio.hpp>
#endif

namespace cppmcp {

LocalPipeClientTransport::LocalPipeClientTransport(std::string pipe_name)
    : pipe_name_(std::move(pipe_name)) {}

LocalPipeClientTransport::~LocalPipeClientTransport() {
    disconnect();
}

void LocalPipeClientTransport::set_message_handler(MessageCallback handler) {
    message_handler_ = std::move(handler);
}
void LocalPipeClientTransport::set_error_handler(ErrorCallback handler) {
    error_handler_ = std::move(handler);
}
void LocalPipeClientTransport::set_disconnect_handler(DisconnectCallback handler) {
    disconnect_handler_ = std::move(handler);
}
void LocalPipeClientTransport::set_io_context(asio::io_context* io_ctx) {
    io_ctx_ = io_ctx;
}

bool LocalPipeClientTransport::is_connected() const {
    return connected_.load();
}

std::string LocalPipeClientTransport::resolve_path() const {
#ifdef _WIN32
    return R"(\\.\pipe\)" + pipe_name_;
#else
    return "/tmp/" + pipe_name_ + ".sock";
#endif
}

void LocalPipeClientTransport::connect() {
    if (connected_.exchange(true)) {
        return;
    }
    if (!io_ctx_) {
        connected_.store(false);
        throw std::runtime_error("LocalPipeClientTransport: no io_context");
    }
    stopping_.store(false);

#ifdef _WIN32
    std::string path = resolve_path();
    HANDLE h = INVALID_HANDLE_VALUE;
    // Retry only on transient "not ready yet" conditions:
    //   ERROR_PIPE_BUSY      -> all pipe instances busy; wait then retry
    //   ERROR_FILE_NOT_FOUND -> server hasn't created the pipe yet; brief poll
    // Everything else (ACCESS_DENIED, INVALID_PARAMETER, ...) fails fast.
    constexpr int MAX_RETRIES = 100;
    constexpr auto POLL_INTERVAL = std::chrono::milliseconds(50);
    DWORD last_err = 0;
    for (int i = 0; i < MAX_RETRIES && h == INVALID_HANDLE_VALUE; ++i) {
        h = CreateFileA(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                        OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
        if (h == INVALID_HANDLE_VALUE) {
            last_err = GetLastError();
            if (last_err == ERROR_PIPE_BUSY) {
                // WaitNamedPipeA blocks until a pipe instance is free OR timeout.
                WaitNamedPipeA(path.c_str(), 200);
            } else if (last_err == ERROR_FILE_NOT_FOUND) {
                std::this_thread::sleep_for(POLL_INTERVAL);
            } else {
                // Non-transient error: do not retry.
                break;
            }
        }
    }
    if (h == INVALID_HANDLE_VALUE) {
        connected_.store(false);
        throw std::runtime_error("LocalPipeClientTransport: failed to connect pipe (" +
                                 std::to_string(last_err) + "): " + path);
    }
    handle_ = std::make_unique<asio::windows::stream_handle>(*io_ctx_, h);
#else
    socket_ = std::make_unique<asio::local::stream_protocol::socket>(*io_ctx_);
    asio::local::stream_protocol::endpoint ep(resolve_path());
    // Mirror the Windows retry loop: the server may not have bound+listened
    // yet when the client connects (slow machines / racing startup).
    asio::error_code ec;
    for (int i = 0; i < 100; ++i) {
        socket_->connect(ep, ec);
        if (!ec) break;
        // ENOENT: socket file not created yet; ECONNREFUSED: exists but not
        // listening. Both are transient during server startup.
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    if (ec) {
        connected_.store(false);
        socket_.reset();
        throw std::runtime_error("LocalPipeClientTransport: connect failed: " + ec.message());
    }
#endif

    auto self = shared_from_this();
    write_queue_ = std::make_shared<AsyncWriteQueue>(
        [self](std::shared_ptr<std::string> buffer, AsyncWriteQueue::WriteCompletion completion) {
#ifdef _WIN32
            asio::async_write(*self->handle_, asio::buffer(*buffer),
                [buffer, completion](const asio::error_code& ec, std::size_t) { completion(ec); });
#else
            asio::async_write(*self->socket_, asio::buffer(*buffer),
                [buffer, completion](const asio::error_code& ec, std::size_t) { completion(ec); });
#endif
        },
        [self](const asio::error_code&) {
            if (self->error_handler_) {
                self->error_handler_("local pipe write error");
            }
        });

    do_read();
}

void LocalPipeClientTransport::disconnect() {
    if (!stopping_.exchange(true)) {
        connected_.store(false);
    }
    connected_.store(false);
    if (write_queue_) {
        write_queue_->shutdown();
    }
#ifdef _WIN32
    if (handle_) {
        asio::error_code ec;
        handle_->close(ec);
    }
#else
    if (socket_) {
        asio::error_code ec;
        socket_->close(ec);
    }
#endif
    // Defer clearing handlers so the in-flight read completion (triggered by
    // handle/socket close) can still invoke disconnect_handler_.
    if (io_ctx_ && (message_handler_ || error_handler_ || disconnect_handler_)) {
        auto self = shared_from_this();
        asio::post(*io_ctx_, [self]() { self->clear_handlers(); });
    } else {
        clear_handlers();
    }
}

void LocalPipeClientTransport::clear_handlers() {
    message_handler_ = {};
    error_handler_ = {};
    disconnect_handler_ = {};
}

void LocalPipeClientTransport::send_message(const nlohmann::json& message) {
    if (!write_queue_) {
        return;
    }
    std::string data = message.dump() + "\n";
    if (data.size() > max_line_size_) {
        if (error_handler_) error_handler_("pipe message too large");
        return;
    }
    write_queue_->enqueue(std::move(data));
}

void LocalPipeClientTransport::handle_line(const std::string& line) {
    try {
        auto j = nlohmann::json::parse(line);
        if (message_handler_) {
            message_handler_(j);
        }
    } catch (const std::exception& e) {
        if (error_handler_) {
            error_handler_(std::string("pipe parse error: ") + e.what());
        }
    }
}

void LocalPipeClientTransport::do_read() {
    // Cap BEFORE arming async_read_until: without this, a peer sending data
    // with no '\n' grows read_buf_ unboundedly (the post-hoc line-size check
    // fires too late to bound memory).
    if (read_buf_.size() > max_line_size_) {
        if (error_handler_) error_handler_("pipe inbound line exceeds size cap, dropping connection");
        connected_.store(false);
        if (disconnect_handler_ && io_ctx_) {
            auto self = shared_from_this();
            asio::post(*io_ctx_, [self]() {
                if (self->disconnect_handler_) self->disconnect_handler_();
            });
        }
        return;
    }
    if (stopping_.load()) {
        return;
    }
    auto self = shared_from_this();
#ifdef _WIN32
    if (!handle_) return;
    asio::async_read_until(*handle_, read_buf_, '\n',
        [self](const asio::error_code& ec, std::size_t bytes) { self->on_read(ec, bytes); });
#else
    if (!socket_) return;
    asio::async_read_until(*socket_, read_buf_, '\n',
        [self](const asio::error_code& ec, std::size_t bytes) { self->on_read(ec, bytes); });
#endif
}

void LocalPipeClientTransport::on_read(const asio::error_code& ec, std::size_t bytes) {
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
    std::string line;
    line.reserve(bytes);
    asio::streambuf::const_buffers_type bufs = read_buf_.data();
    for (auto it = asio::buffers_begin(bufs); it != asio::buffers_begin(bufs) + bytes; ++it) {
        line += *it;
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
