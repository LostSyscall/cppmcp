#include "cppmcp/stdio_transport.hpp"
#include "cppmcp/jsonrpc.hpp"
#include "cppmcp/logging.hpp"
#include "cppmcp/protocol.hpp"

#include <condition_variable>
#include <deque>
#include <iostream>

namespace cppmcp {

StdioTransport::StdioTransport() {}

StdioTransport::~StdioTransport() {
    stop();
}

void StdioTransport::set_io_context(asio::io_context* io_ctx) {
    io_ctx_ = io_ctx;
}

void StdioTransport::start() {
    running_ = true;

    // Dedicated stdout writer: std::cout on the io thread would block the whole
    // event loop when the parent stops reading the pipe.
    {
        std::lock_guard<std::mutex> lock(write_mutex_);
        write_stopping_ = false;
    }
    writer_thread_ = std::thread([this]() { write_loop(); });

#ifdef _WIN32
    // If stdin is a pipe (the common MCP case — another process feeds us via a
    // redirected pipe), it IS overlapped-capable and we can drive it with asio
    // (no reader thread). A real console (char device) is not overlapped, so we
    // fall back to the reader-thread path there.
    DWORD ft = GetFileType(GetStdHandle(STD_INPUT_HANDLE));
    if (ft == FILE_TYPE_PIPE && io_ctx_) {
        use_async_stdin_ = true;
        stdin_handle_ = std::make_unique<asio::windows::stream_handle>(*io_ctx_);
        asio::error_code ec;
        stdin_handle_->assign(GetStdHandle(STD_INPUT_HANDLE), ec);
        if (ec) {
            use_async_stdin_ = false;
            stdin_handle_.reset();
            if (error_handler_) error_handler_("StdioTransport: stdin assign failed");
        } else {
            do_read();
            return;
        }
    }
    if (!use_async_stdin_) {
        auto self = shared_from_this();
        win32_reader_thread_ = std::thread([self]() {
            self->win32_read_loop();
        });
    }
#else
    // Unix: use asio async read on stdin fd
    if (!io_ctx_) {
        if (error_handler_) error_handler_("StdioTransport: no io_context set");
        running_ = false;
        return;
    }
    stdin_desc_ = std::make_unique<asio::posix::stream_descriptor>(*io_ctx_, STDIN_FILENO);
    do_read();
#endif
}

void StdioTransport::stop() {
    running_ = false;

#ifdef _WIN32
    if (use_async_stdin_ && stdin_handle_) {
        asio::error_code ec;
        stdin_handle_->close(ec);  // cancels in-flight async_read_until
    } else if (win32_reader_thread_.joinable()) {
        // The reader thread blocks in std::getline on the console handle, which
        // Ctrl+C does not interrupt. Joining would deadlock, so detach it; it
        // exits on EOF or when the process terminates. (Console-only fallback.)
        // The read loop keeps only a weak ref to this transport, so a detached
        // wakeup after destruction is harmless.
        win32_reader_thread_.detach();
    }
#else
    if (stdin_desc_ && stdin_desc_->is_open()) {
        asio::error_code ec;
        stdin_desc_->close(ec);
    }
#endif

    // Stop the writer thread after draining whatever is already queued.
    if (writer_thread_.joinable()) {
        {
            std::lock_guard<std::mutex> lock(write_mutex_);
            write_stopping_ = true;
        }
        write_cv_.notify_all();
        writer_thread_.join();
    }
}

bool StdioTransport::is_running() const {
    return running_;
}

void StdioTransport::send_message(const nlohmann::json& message) {
    std::string data = message.dump() + "\n";
    // Hand off to the dedicated writer thread: a full stdout pipe (parent not
    // reading) must never block the io/event-loop thread. The bounded queue
    // drops the OLDEST message when full — better to lose one notification
    // than to stall the whole server.
    {
        std::unique_lock<std::mutex> lock(write_mutex_);
        if (write_stopping_) {
            return;
        }
        if (write_queue_bytes_ + data.size() > max_queued_write_bytes_) {
            if (!write_queue_.empty()) {
                write_queue_.pop_front();  // drop oldest
            }
        }
        write_queue_bytes_ += data.size();
        write_queue_.push_back(std::move(data));
    }
    write_cv_.notify_one();
}

void StdioTransport::write_loop() {
    std::unique_lock<std::mutex> lock(write_mutex_);
    while (true) {
        write_cv_.wait(lock, [this]() { return write_stopping_ || !write_queue_.empty(); });
        if (write_stopping_ && write_queue_.empty()) {
            return;
        }
        std::string data = std::move(write_queue_.front());
        write_queue_.pop_front();
        write_queue_bytes_ -= data.size();
        lock.unlock();
        // Blocking write on the DEDICATED thread: a full stdout pipe stalls only
        // this thread, never the io loop.
        std::fwrite(data.data(), 1, data.size(), stdout);
        std::fflush(stdout);
        lock.lock();
    }
}

void StdioTransport::set_message_handler(MessageCallback handler) {
    message_handler_ = std::move(handler);
}

void StdioTransport::set_error_handler(ErrorCallback handler) {
    error_handler_ = std::move(handler);
}

#ifdef _WIN32

void StdioTransport::win32_read_loop() {
    // Weak ref: this thread may outlive the transport (detached in stop()).
    // Posting keeps the transport alive until the lambda runs; if the transport
    // is already gone, weak_ptr::lock() fails and we drop the line.
    std::weak_ptr<StdioTransport> weak = shared_from_this();
    std::string line;
    while (running_ && std::getline(std::cin, line)) {
        if (line.empty()) continue;
        if (line.size() > max_line_size) {
            if (error_handler_) error_handler_("Message too large, discarded");
            continue;
        }
        std::string captured_line = line;
        if (io_ctx_) {
            asio::post(*io_ctx_, [weak, captured_line]() {
                if (auto self = weak.lock()) {
                    self->handle_line(captured_line);
                }
            });
        }
    }
    running_ = false;
    // stdin reached EOF: tell the server to shut down. Do NOT gate on
    // running_ here — it was just set false above, and stop() detaches this
    // thread instead of joining it, so a genuine EOF must still propagate.
    if (io_ctx_) {
        asio::post(*io_ctx_, [weak]() {
            if (auto self = weak.lock()) {
                if (self->disconnect_handler_) {
                    self->disconnect_handler_();
                }
            }
        });
    }
}

#endif

void StdioTransport::do_read() {
    auto self = shared_from_this();
#ifdef _WIN32
    if (!stdin_handle_ || !use_async_stdin_) {
        return;
    }
    asio::async_read_until(*stdin_handle_, read_buf_, '\n',
        [self](const asio::error_code& ec, std::size_t bytes_transferred) {
            self->on_read(ec, bytes_transferred);
        });
#else
    asio::async_read_until(*stdin_desc_, read_buf_, '\n',
        [self](const asio::error_code& ec, std::size_t bytes_transferred) {
            self->on_read(ec, bytes_transferred);
        });
#endif
}

void StdioTransport::on_read(const asio::error_code& ec, std::size_t bytes_transferred) {
    if (ec) {
        running_ = false;
        if (disconnect_handler_) {
            disconnect_handler_();  // stdin closed/errored -> shut down
        }
        return;
    }

    // Extract the line from streambuf (includes the '\n')
    std::string line;
    line.reserve(bytes_transferred);
    asio::streambuf::const_buffers_type bufs = read_buf_.data();
    for (auto it = asio::buffers_begin(bufs); it != asio::buffers_begin(bufs) + bytes_transferred; ++it) {
        line.push_back(*it);
    }
    read_buf_.consume(bytes_transferred);

    // A line longer than max_line_size means the peer is either broken or
    // hostile: drop the connection instead of continuing to buffer.
    if (line.size() > max_line_size) {
        if (error_handler_) error_handler_("Message too large, dropping connection");
        running_ = false;
        if (disconnect_handler_) {
            disconnect_handler_();
        }
        return;
    }

    // Strip trailing newline
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) {
        line.pop_back();
    }

    if (!line.empty()) {
        handle_line(line);
    }

    // Continue reading
    if (running_) {
        do_read();
    }
}

void StdioTransport::handle_line(const std::string& line) {
    try {
        auto json_msg = nlohmann::json::parse(line);
        if (message_handler_) {
            message_handler_(json_msg, [this](const nlohmann::json& resp) {
                send_message(resp);
            }, "");
        }
    } catch (const nlohmann::json::parse_error& e) {
        AsyncLogger::instance().log(std::string("JSON parse error: ") + e.what());
        auto error_resp = make_error_response_null_id(Protocol::PARSE_ERROR, "Parse error");
        send_message(error_resp);
        if (error_handler_) {
            error_handler_("JSON parse error");
        }
    } catch (const std::exception& e) {
        AsyncLogger::instance().log(std::string("Error processing message: ") + e.what());
        if (error_handler_) {
            error_handler_("Error processing message");
        }
    }
}

} // namespace cppmcp