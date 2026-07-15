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
    stopping_.store(false);
    process_.spawn(executable_, args_);

#ifdef _WIN32
    auto self = shared_from_this();
    reader_thread_ = std::thread([self]() { self->win32_read_loop(); });
#else
    if (!io_ctx_) {
        connected_.store(false);
        if (error_handler_) error_handler_("StdioClientTransport: no io_context");
        return;
    }
    stdin_desc_ = std::make_unique<asio::posix::stream_descriptor>(*io_ctx_, ::dup(process_.stdin_fd()));
    stdout_desc_ = std::make_unique<asio::posix::stream_descriptor>(*io_ctx_, ::dup(process_.stdout_fd()));

    auto self = shared_from_this();
    write_queue_ = std::make_shared<AsyncWriteQueue>(
        [self](std::shared_ptr<std::string> buffer, AsyncWriteQueue::WriteCompletion completion) {
            asio::async_write(*self->stdin_desc_, asio::buffer(*buffer),
                [buffer, completion](const asio::error_code& ec, std::size_t) { completion(ec); });
        },
        [self](const asio::error_code& ec) {
            if (self->error_handler_) {
                self->error_handler_("stdio write error");
            }
            (void)ec;
        });
    do_read();
#endif
}

void StdioClientTransport::disconnect() {
    if (!connected_.exchange(false) && !stopping_.exchange(true)) {
        // already disconnected and stopping — still ensure cleanup below
    }
    stopping_.store(true);

#ifdef _WIN32
    if (reader_thread_.joinable()) {
        reader_thread_.detach();  // exits on EOF when the child is killed below
    }
#else
    if (write_queue_) {
        write_queue_->shutdown();
    }
    if (stdin_desc_) {
        asio::error_code ec;
        stdin_desc_->close(ec);
    }
    if (stdout_desc_) {
        asio::error_code ec;
        stdout_desc_->close(ec);
    }
#endif

    process_.terminate();
    clear_handlers();
}

void StdioClientTransport::clear_handlers() {
    message_handler_ = {};
    error_handler_ = {};
    disconnect_handler_ = {};
}

void StdioClientTransport::send_message(const nlohmann::json& message) {
    std::string data = message.dump() + "\n";
    if (data.size() > max_line_size_) {
        if (error_handler_) error_handler_("stdio message too large");
        return;
    }
#ifdef _WIN32
    if (!io_ctx_ || !process_.stdin_write()) {
        return;
    }
    auto self = shared_from_this();
    asio::post(*io_ctx_, [self, data]() {
        std::lock_guard<std::mutex> lock(self->write_mutex_);
        HANDLE h = self->process_.stdin_write();
        if (!h) return;
        DWORD written = 0;
        WriteFile(h, data.c_str(), static_cast<DWORD>(data.size()), &written, nullptr);
    });
#else
    if (write_queue_) {
        write_queue_->enqueue(std::move(data));
    }
#endif
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
        if (error_handler_) {
            error_handler_(std::string("stdio parse error: ") + e.what());
        }
    }
}

#ifdef _WIN32

void StdioClientTransport::win32_read_loop() {
    auto self = shared_from_this();
    std::string buffer;
    char chunk[8192];
    while (!stopping_.load()) {
        DWORD read_bytes = 0;
        BOOL ok = ReadFile(process_.stdout_read(), chunk, sizeof(chunk), &read_bytes, nullptr);
        if (!ok || read_bytes == 0) {
            break;  // EOF or pipe closed (child exited)
        }
        buffer.append(chunk, read_bytes);
        std::string::size_type pos;
        while ((pos = buffer.find('\n')) != std::string::npos) {
            std::string line = buffer.substr(0, pos);
            buffer.erase(0, pos + 1);
            while (!line.empty() && line.back() == '\r') line.pop_back();
            if (!line.empty()) {
                std::string captured = std::move(line);
                asio::post(*io_ctx_, [self, captured]() { self->handle_line(captured); });
            }
        }
    }
    connected_.store(false);
    if (io_ctx_) {
        asio::post(*io_ctx_, [self]() {
            if (self->disconnect_handler_) {
                self->disconnect_handler_();
            }
        });
    }
}

#else

void StdioClientTransport::do_read() {
    if (!stdout_desc_ || stopping_.load()) {
        return;
    }
    auto self = shared_from_this();
    asio::async_read_until(*stdout_desc_, read_buf_, '\n',
        [self](const asio::error_code& ec, std::size_t bytes) { self->on_read(ec, bytes); });
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

#endif

} // namespace cppmcp
