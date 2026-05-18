#include "cppmcp/local_pipe_transport.hpp"
#include "cppmcp/jsonrpc.hpp"
#include "cppmcp/protocol.hpp"
#include "cppmcp/common.hpp"

#include <iostream>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

// thread_local: response routing for local pipe connections
static thread_local std::shared_ptr<cppmcp::PipeConnection> tl_responding_conn;

namespace cppmcp {

// --- PipeConnection ---
#ifdef _WIN32
PipeConnection::PipeConnection(asio::io_context& io_ctx, int id)
    : stream_handle(io_ctx), connection_id(id) {}
#else
PipeConnection::PipeConnection(asio::local::stream_protocol::socket s, int id)
    : socket(std::move(s)), connection_id(id) {}
#endif

void PipeConnection::start_read() {
#ifdef _WIN32
    asio::async_read(stream_handle, read_buf, asio::transfer_at_least(1),
        [this](const asio::error_code& ec, std::size_t bytes_transferred) {
            if (ec) {
                active = false;
                std::cerr << "[cppmcp] Client #" << connection_id << " disconnected" << std::endl;
                return;
            }

            auto bufs = read_buf.data();
            auto begin = asio::buffers_begin(bufs);
            auto end = begin + bytes_transferred;
            std::string data(begin, end);
            read_buf.consume(bytes_transferred);

            while (!data.empty() && (data.back() == '\n' || data.back() == '\r')) {
                data.pop_back();
            }

            if (!data.empty()) {
                handle_line(data);
            }

            if (active) {
                start_read();
            }
        });
#else
    asio::async_read_until(socket, read_buf, '\n',
        [this](const asio::error_code& ec, std::size_t bytes_transferred) {
            if (ec) {
                active = false;
                std::cerr << "[cppmcp] Client #" << connection_id << " disconnected" << std::endl;
                return;
            }

            std::string line;
            line.reserve(bytes_transferred);
            auto bufs = read_buf.data();
            for (auto it = asio::buffers_begin(bufs); it != asio::buffers_begin(bufs) + bytes_transferred; ++it) {
                line += *it;
            }
            read_buf.consume(bytes_transferred);

            while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) {
                line.pop_back();
            }

            if (!line.empty()) {
                handle_line(line);
            }

            if (active) {
                start_read();
            }
        });
#endif
}

void PipeConnection::handle_line(const std::string& line) {
    try {
        auto json_msg = nlohmann::json::parse(line);
        tl_responding_conn = std::shared_ptr<PipeConnection>(this, [](PipeConnection*){});  // non-owning
        if (message_handler) {
            message_handler(json_msg);
        }
        tl_responding_conn = nullptr;
    } catch (const nlohmann::json::parse_error& e) {
        auto error_resp = make_error_response_null_id(Protocol::PARSE_ERROR, e.what());
        tl_responding_conn = std::shared_ptr<PipeConnection>(this, [](PipeConnection*){});
        enqueue_write(error_resp.dump());
        tl_responding_conn = nullptr;
        if (error_handler) {
            error_handler("JSON parse error: " + std::string(e.what()));
        }
    } catch (const std::exception& e) {
        if (error_handler) {
            error_handler("Error processing message: " + std::string(e.what()));
        }
    }
}

void PipeConnection::enqueue_write(std::string data) {
    data += "\n";

    bool was_empty;
    {
        std::lock_guard<std::mutex> lock(write_mutex);
        was_empty = write_queue.empty();
        write_queue.push_back(std::move(data));
    }
    if (was_empty) {
        start_write();
    }
}

void PipeConnection::start_write() {
    auto data_ptr = std::make_shared<std::string>();
    {
        std::lock_guard<std::mutex> lock(write_mutex);
        if (write_queue.empty() || !active) return;
        *data_ptr = std::move(write_queue.front());
        write_queue.pop_front();
    }

#ifdef _WIN32
    asio::async_write(stream_handle, asio::buffer(*data_ptr),
        [this, data_ptr](const asio::error_code& ec, std::size_t) {
            if (ec) { active = false; return; }
            std::lock_guard<std::mutex> lock(write_mutex);
            if (!write_queue.empty() && active) start_write();
        });
#else
    asio::async_write(socket, asio::buffer(*data_ptr),
        [this, data_ptr](const asio::error_code& ec, std::size_t) {
            if (ec) { active = false; return; }
            std::lock_guard<std::mutex> lock(write_mutex);
            if (!write_queue.empty() && active) start_write();
        });
#endif
}

// --- LocalPipeTransport ---
LocalPipeTransport::LocalPipeTransport(const LocalPipeConfig& config)
    : config_(config) {}

LocalPipeTransport::~LocalPipeTransport() {
    stop();
}

void LocalPipeTransport::set_io_context(asio::io_context* io_ctx) {
    io_ctx_ = io_ctx;
}

std::string LocalPipeTransport::resolve_pipe_path() const {
#ifdef _WIN32
    return std::string("\\\\.\\pipe\\") + config_.pipe_name;
#else
    return std::string("/tmp/") + config_.pipe_name + ".sock";
#endif
}

#ifdef _WIN32
// --- Windows: thin accept thread + asio stream_handle ---
void LocalPipeTransport::start() {
    if (!io_ctx_) {
        if (error_handler_) error_handler_("LocalPipeTransport: no io_context set");
        return;
    }

    running_ = true;
    win32_accept_thread_ = std::thread(&LocalPipeTransport::win32_accept_loop, this);

    std::cerr << "[cppmcp] Local pipe server starting on " << resolve_pipe_path()
              << " (mode: " << (config_.mode == PipeMode::SingleClient ? "SingleClient" : "MultiClient")
              << ", asio async)" << std::endl;
}

void LocalPipeTransport::win32_accept_loop() {
    std::string path = resolve_pipe_path();

    while (running_) {
        HANDLE h = CreateNamedPipeA(
            path.c_str(),
            PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
            PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
            config_.mode == PipeMode::SingleClient ? 1 : config_.max_instances,
            static_cast<DWORD>(config_.buffer_size),
            static_cast<DWORD>(config_.buffer_size),
            0,
            nullptr
        );

        if (h == INVALID_HANDLE_VALUE) {
            DWORD err = GetLastError();
            if (error_handler_) error_handler_("CreateNamedPipe failed: " + std::to_string(err));
            if (!running_) break;
            Sleep(500);
            continue;
        }

        HANDLE accept_evt = CreateEvent(nullptr, TRUE, FALSE, nullptr);
        if (!accept_evt) { CloseHandle(h); continue; }

        OVERLAPPED ov = {};
        ov.hEvent = accept_evt;

        BOOL connected = ConnectNamedPipe(h, &ov);
        DWORD last_error = GetLastError();
        bool got_connection = false;

        if (connected || last_error == ERROR_PIPE_CONNECTED) {
            got_connection = true;
        } else if (last_error == ERROR_IO_PENDING) {
            while (running_) {
                DWORD wait_result = WaitForSingleObjectEx(accept_evt, 500, TRUE);
                if (wait_result == WAIT_OBJECT_0) {
                    DWORD bytes_transferred = 0;
                    if (GetOverlappedResult(h, &ov, &bytes_transferred, FALSE)) {
                        got_connection = true;
                        break;
                    }
                    DWORD err = GetLastError();
                    if (err == ERROR_OPERATION_ABORTED) { CloseHandle(h); break; }
                    CloseHandle(h);
                    if (error_handler_) error_handler_("ConnectNamedPipe async failed: " + std::to_string(err));
                    break;
                }
                if (wait_result == WAIT_TIMEOUT) {
                    if (!running_) {
                        CancelIoEx(h, &ov);
                        WaitForSingleObjectEx(accept_evt, 100, TRUE);
                        CloseHandle(h);
                        break;
                    }
                    continue;
                }
                CloseHandle(h);
                break;
            }
        } else {
            CloseHandle(h);
            if (error_handler_) error_handler_("ConnectNamedPipe failed: " + std::to_string(last_error));
        }

        CloseHandle(accept_evt);

        if (!running_) break;
        if (!got_connection) continue;

        // Client connected — post handle assignment into asio event loop
        int conn_id = next_connection_id_++;
        asio::post(*io_ctx_, [this, h, conn_id]() {
            auto conn = std::make_shared<PipeConnection>(*io_ctx_, conn_id);
            conn->active = true;
            conn->message_handler = message_handler_;
            conn->error_handler = error_handler_;

            asio::error_code ec;
            conn->stream_handle.assign(h, ec);
            if (ec) {
                CloseHandle(h);
                if (error_handler_) error_handler_("Failed to assign pipe handle: " + ec.message());
                return;
            }

            {
                std::unique_lock<std::shared_mutex> lock(connections_mutex_);
                active_connections_.push_back(conn);
            }

            conn->start_read();
            std::cerr << "[cppmcp] Client #" << conn_id << " connected on pipe" << std::endl;
        });

        if (config_.mode == PipeMode::SingleClient) {
            while (running_.load()) { Sleep(500); }
            break;
        }
    }
}

void LocalPipeTransport::stop() {
    if (!running_.load()) return;
    running_ = false;

    {
        std::unique_lock<std::shared_mutex> lock(connections_mutex_);
        for (auto& conn : active_connections_) {
            conn->active = false;
            asio::error_code ec;
            conn->stream_handle.close(ec);
        }
        active_connections_.clear();
    }

    if (win32_accept_thread_.joinable()) {
        win32_accept_thread_.join();
    }
}

#else
// --- Unix: asio local::stream_protocol ---
void LocalPipeTransport::start() {
    if (!io_ctx_) {
        if (error_handler_) error_handler_("LocalPipeTransport: no io_context set");
        return;
    }

    running_ = true;

    std::string path = resolve_pipe_path();
    unlink(path.c_str());

    acceptor_ = std::make_unique<asio::local::stream_protocol::acceptor>(*io_ctx_);
    asio::local::stream_protocol::endpoint endpoint(path);

    asio::error_code ec;
    acceptor_->open(endpoint.protocol(), ec);
    if (ec) { if (error_handler_) error_handler_("open() failed: " + ec.message()); running_ = false; return; }

    acceptor_->bind(endpoint, ec);
    if (ec) { if (error_handler_) error_handler_("bind() failed: " + ec.message()); running_ = false; return; }

    acceptor_->listen(config_.max_instances, ec);
    if (ec) { if (error_handler_) error_handler_("listen() failed: " + ec.message()); running_ = false; return; }

    do_accept();

    std::cerr << "[cppmcp] Local pipe server starting on " << path
              << " (mode: " << (config_.mode == PipeMode::SingleClient ? "SingleClient" : "MultiClient")
              << ", asio async)" << std::endl;
}

void LocalPipeTransport::do_accept() {
    acceptor_->async_accept(
        [this](const asio::error_code& ec, asio::local::stream_protocol::socket socket) {
            if (ec) {
                if (running_) do_accept();
                return;
            }

            if (!running_) { socket.close(); return; }

            int conn_id = next_connection_id_++;
            auto conn = std::make_shared<PipeConnection>(std::move(socket), conn_id);
            conn->active = true;
            conn->message_handler = message_handler_;
            conn->error_handler = error_handler_;

            {
                std::unique_lock<std::shared_mutex> lock(connections_mutex_);
                active_connections_.push_back(conn);
            }

            conn->start_read();
            std::cerr << "[cppmcp] Client #" << conn_id << " connected" << std::endl;

            if (config_.mode == PipeMode::SingleClient) return;

            if (running_) do_accept();
        });
}

void LocalPipeTransport::stop() {
    if (!running_.load()) return;
    running_ = false;

    {
        std::unique_lock<std::shared_mutex> lock(connections_mutex_);
        for (auto& conn : active_connections_) {
            conn->active = false;
            asio::error_code ec;
            conn->socket.close(ec);
        }
        active_connections_.clear();
    }

    if (acceptor_) {
        asio::error_code ec;
        acceptor_->close(ec);
    }

    unlink(resolve_pipe_path().c_str());
}

#endif

bool LocalPipeTransport::is_running() const {
    return running_;
}

void LocalPipeTransport::send_message(const nlohmann::json& message) {
    std::string data = message.dump();

    if (tl_responding_conn && tl_responding_conn->active.load()) {
        tl_responding_conn->enqueue_write(data);
        return;
    }

    std::shared_lock<std::shared_mutex> lock(connections_mutex_);
    for (auto& conn : active_connections_) {
        if (conn->active.load()) {
            conn->enqueue_write(data);
        }
    }
}

void LocalPipeTransport::set_message_handler(MessageCallback handler) {
    message_handler_ = std::move(handler);
}

void LocalPipeTransport::set_error_handler(ErrorCallback handler) {
    error_handler_ = std::move(handler);
}

void LocalPipeTransport::set_response_sender(ResponseSender sender) {
    // No-op: tl_responding_conn handles response routing
}

} // namespace cppmcp