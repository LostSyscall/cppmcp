#include "cppmcp/local_pipe_transport.hpp"
#include "cppmcp/jsonrpc.hpp"
#include "cppmcp/protocol.hpp"
#include "cppmcp/common.hpp"
#include "cppmcp/logging.hpp"

#include <algorithm>
#include <iostream>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace cppmcp {

// --- PipeConnection ---
#ifdef _WIN32
PipeConnection::PipeConnection(asio::io_context& io_ctx, int id)
    : stream_handle(io_ctx), strand_(stream_handle.get_executor()), connection_id(id) {}
#else
PipeConnection::PipeConnection(asio::local::stream_protocol::socket s, int id)
    : socket(std::move(s)), strand_(socket.get_executor()), connection_id(id) {}
#endif

void PipeConnection::start_read() {
    auto self = shared_from_this();
#ifdef _WIN32
    asio::async_read_until(stream_handle, read_buf, '\n',
        asio::bind_executor(strand_, [this, self](const asio::error_code& ec, std::size_t bytes_transferred) {
            handle_read_completion(ec, bytes_transferred);
        }));
#else
    asio::async_read_until(socket, read_buf, '\n',
        asio::bind_executor(strand_, [this, self](const asio::error_code& ec, std::size_t bytes_transferred) {
            handle_read_completion(ec, bytes_transferred);
        }));
#endif
}

void PipeConnection::handle_read_completion(const asio::error_code& ec, std::size_t bytes_transferred) {
    if (ec) {
        AsyncLogger::instance().log("[cppmcp] Client #" + std::to_string(connection_id) + " disconnected");
        on_error();
        return;
    }

    std::string line;
    line.reserve(bytes_transferred);
    auto bufs = read_buf.data();
    for (auto it = asio::buffers_begin(bufs); it != asio::buffers_begin(bufs) + bytes_transferred; ++it) {
        line += *it;
    }
    read_buf.consume(bytes_transferred);

    if (line.size() > max_line_size) {
        enqueue_write(make_error_response_null_id(Protocol::PARSE_ERROR, "Message too large").dump());
        on_error();
        return;
    }

    while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) {
        line.pop_back();
    }

    if (!line.empty()) {
        handle_line(line);
    }

    if (active) {
        start_read();
    }
}

void PipeConnection::handle_line(const std::string& line) {
    auto self = shared_from_this();
    try {
        auto json_msg = nlohmann::json::parse(line);
        if (message_handler) {
            ITransport::ResponseSink sink = [self](const nlohmann::json& resp) {
                self->enqueue_write(resp.dump());
            };
            message_handler(json_msg, sink, "");
        }
    } catch (const nlohmann::json::parse_error& e) {
        auto error_resp = make_error_response_null_id(Protocol::PARSE_ERROR, e.what());
        enqueue_write(error_resp.dump());
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
    if (!write_queue) {
        return;
    }
    data += "\n";
    write_queue->enqueue(std::move(data));
}

void PipeConnection::init_write_queue(std::size_t max_queued_bytes, QueueOverflowPolicy policy) {
    auto self = shared_from_this();
#ifdef _WIN32
    write_queue = std::make_shared<AsyncWriteQueue>(
        [self](std::shared_ptr<std::string> buffer, AsyncWriteQueue::WriteCompletion completion) {
            asio::async_write(self->stream_handle, asio::buffer(*buffer),
                [buffer, completion](const asio::error_code& ec, std::size_t) {
                    completion(ec);
                });
        },
        [self](const asio::error_code& ec) {
            (void)ec;
            self->on_error();
        },
        max_queued_bytes, policy);
#else
    write_queue = std::make_shared<AsyncWriteQueue>(
        [self](std::shared_ptr<std::string> buffer, AsyncWriteQueue::WriteCompletion completion) {
            asio::async_write(self->socket, asio::buffer(*buffer),
                [buffer, completion](const asio::error_code& ec, std::size_t) {
                    completion(ec);
                });
        },
        [self](const asio::error_code& ec) {
            (void)ec;
            self->on_error();
        },
        max_queued_bytes, policy);
#endif
}

void PipeConnection::on_error() {
    if (active.exchange(false)) {
        if (on_disconnect) {
            on_disconnect();
        }
    }
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
    return R"(\\.\pipe\)" + config_.pipe_name;
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
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
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

            conn->max_line_size = config_.max_line_size;
            conn->init_write_queue(config_.write_queue_max_bytes, config_.write_queue_overflow);
            conn->on_disconnect = [this, conn]() {
                unregister_connection(conn);
            };

            {
                std::unique_lock<std::shared_mutex> lock(connections_mutex_);
                active_connections_.push_back(conn);
            }

            conn->start_read();
            AsyncLogger::instance().log("[cppmcp] Client #" + std::to_string(conn_id) + " connected on pipe");
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
            if (conn->write_queue) {
                conn->write_queue->shutdown();
            }
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
            conn->max_line_size = config_.max_line_size;
            conn->init_write_queue(config_.write_queue_max_bytes, config_.write_queue_overflow);
            conn->on_disconnect = [this, conn]() {
                unregister_connection(conn);
            };

            {
                std::unique_lock<std::shared_mutex> lock(connections_mutex_);
                active_connections_.push_back(conn);
            }

            conn->start_read();
            AsyncLogger::instance().log("[cppmcp] Client #" + std::to_string(conn_id) + " connected");

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
            if (conn->write_queue) {
                conn->write_queue->shutdown();
            }
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

void LocalPipeTransport::unregister_connection(std::shared_ptr<PipeConnection> conn) {
    if (!conn) {
        return;
    }
    std::unique_lock<std::shared_mutex> lock(connections_mutex_);
    conn->active = false;
    active_connections_.erase(
        std::remove(active_connections_.begin(), active_connections_.end(), conn),
        active_connections_.end());
}

} // namespace cppmcp