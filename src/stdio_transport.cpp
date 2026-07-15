#include "cppmcp/stdio_transport.hpp"
#include "cppmcp/jsonrpc.hpp"
#include "cppmcp/protocol.hpp"

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

#ifdef _WIN32
    // Windows console stdin doesn't support overlapped I/O.
    // Use a thin reader thread that posts each line into the asio event loop.
    auto self = shared_from_this();
    win32_reader_thread_ = std::thread([self]() {
        self->win32_read_loop();
    });
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
    // The reader thread blocks in std::getline on the console/pipe handle,
    // which Ctrl+C does not interrupt. Joining would deadlock, so detach it;
    // it exits on its own when stdin reaches EOF, or is terminated with the
    // process on exit.
    if (win32_reader_thread_.joinable()) {
        win32_reader_thread_.detach();
    }
#else
    if (stdin_desc_ && stdin_desc_->is_open()) {
        asio::error_code ec;
        stdin_desc_->close(ec);
    }
#endif
}

bool StdioTransport::is_running() const {
    return running_;
}

void StdioTransport::send_message(const nlohmann::json& message) {
    std::string data = message.dump() + "\n";
    if (io_ctx_) {
        // Serialize writes through the asio event loop
        asio::post(*io_ctx_, [this, data]() {
            std::lock_guard<std::mutex> lock(write_mutex_);
            std::cout << data;
            std::cout.flush();
        });
    } else {
        std::lock_guard<std::mutex> lock(write_mutex_);
        std::cout << data;
        std::cout.flush();
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
    auto self = shared_from_this();
    std::string line;
    while (running_ && std::getline(std::cin, line)) {
        if (line.empty()) continue;
        if (line.size() > max_line_size) {
            if (error_handler_) error_handler_("Message too large, discarded");
            continue;
        }
        // Post into asio event loop for thread-safe handler invocation
        std::string captured_line = line;
        asio::post(*io_ctx_, [self, captured_line]() {
            self->handle_line(captured_line);
        });
    }
    running_ = false;
    // stdin reached EOF: tell the server to shut down.
    if (io_ctx_ && disconnect_handler_) {
        asio::post(*io_ctx_, [self]() {
            if (self->disconnect_handler_) {
                self->disconnect_handler_();
            }
        });
    }
}

#else

void StdioTransport::do_read() {
    asio::async_read_until(*stdin_desc_, read_buf_, '\n',
        [this](const asio::error_code& ec, std::size_t bytes_transferred) {
            on_read(ec, bytes_transferred);
        });
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
        line += *it;
    }
    read_buf_.consume(bytes_transferred);

    if (line.size() > max_line_size) {
        if (error_handler_) error_handler_("Message too large, discarded");
        if (running_) do_read();
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

#endif

void StdioTransport::handle_line(const std::string& line) {
    try {
        auto json_msg = nlohmann::json::parse(line);
        if (message_handler_) {
            message_handler_(json_msg, [this](const nlohmann::json& resp) {
                send_message(resp);
            }, "");
        }
    } catch (const nlohmann::json::parse_error& e) {
        auto error_resp = make_error_response_null_id(Protocol::PARSE_ERROR, e.what());
        send_message(error_resp);
        if (error_handler_) {
            error_handler_("JSON parse error: " + std::string(e.what()));
        }
    } catch (const std::exception& e) {
        if (error_handler_) {
            error_handler_("Error processing message: " + std::string(e.what()));
        }
    }
}

} // namespace cppmcp