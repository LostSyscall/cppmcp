#include "cppmcp/local_pipe_transport.hpp"
#include "cppmcp/jsonrpc.hpp"
#include "cppmcp/protocol.hpp"
#include "cppmcp/common.hpp"

#include <iostream>

#ifdef _WIN32
#include <windows.h>

namespace cppmcp {

// --- Helper: resolve pipe path ---
std::string LocalPipeTransport::resolve_pipe_path() const {
    return std::string("\\\\.\\pipe\\") + config_.pipe_name;
}

// --- LocalPipeConnection (overlapped write) ---
bool LocalPipeConnection::write_message(const std::string& data) {
    std::lock_guard<std::mutex> lock(write_mutex);
    if (handle == INVALID_HANDLE || !write_event) return false;

    HANDLE h = static_cast<HANDLE>(handle);
    HANDLE evt = static_cast<HANDLE>(write_event);
    ResetEvent(evt);

    OVERLAPPED ov = {};
    ov.hEvent = evt;

    DWORD bytes_written = 0;
    BOOL result = WriteFile(h, data.c_str(), static_cast<DWORD>(data.size()),
                            &bytes_written, &ov);

    DWORD last_error = GetLastError();

    if (result) {
        // Write completed immediately
        return bytes_written == static_cast<DWORD>(data.size());
    }

    if (last_error == ERROR_IO_PENDING) {
        DWORD transferred = 0;
        if (GetOverlappedResult(h, &ov, &transferred, TRUE)) {
            return transferred == static_cast<DWORD>(data.size());
        }
        return false;
    }

    return false;
}

// --- LocalPipeTransport ---
LocalPipeTransport::LocalPipeTransport(const LocalPipeConfig& config)
    : config_(config) {}

LocalPipeTransport::~LocalPipeTransport() {
    stop();
}

void LocalPipeTransport::start() {
    running_ = true;
    accept_event_ = CreateEvent(nullptr, TRUE, FALSE, nullptr);  // Manual-reset, initially non-signaled
    if (!accept_event_) {
        if (error_handler_) {
            error_handler_("CreateEvent failed for accept");
        }
        running_ = false;
        return;
    }
    accept_thread_ = std::thread(&LocalPipeTransport::accept_loop, this);
    std::cerr << "[cppmcp] Local pipe server starting on " << resolve_pipe_path()
              << " (mode: " << (config_.mode == PipeMode::SingleClient ? "SingleClient" : "MultiClient")
              << ", overlapped async)" << std::endl;
}

void LocalPipeTransport::accept_loop() {
    std::string path = resolve_pipe_path();

    while (running_) {
        // Create pipe instance with overlapped flag
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
            if (err == ERROR_CANNOT_MAKE) {
                std::unique_lock<std::mutex> lock(stop_mutex_);
                stop_cv_.wait_for(lock, std::chrono::milliseconds(config_.poll_interval_ms),
                                  [this] { return !running_.load(); });
                continue;
            }
            if (error_handler_) {
                error_handler_("CreateNamedPipe failed: " + std::to_string(err));
            }
            std::unique_lock<std::mutex> lock(stop_mutex_);
            stop_cv_.wait_for(lock, std::chrono::milliseconds(config_.poll_interval_ms),
                              [this] { return !running_.load(); });
            continue;
        }

        // Overlapped ConnectNamedPipe
        ResetEvent(static_cast<HANDLE>(accept_event_));
        OVERLAPPED ov = {};
        ov.hEvent = static_cast<HANDLE>(accept_event_);

        BOOL connected = ConnectNamedPipe(h, &ov);
        DWORD last_error = GetLastError();

        if (connected) {
            // Connected immediately (rare — client connected before our call)
        } else if (last_error == ERROR_PIPE_CONNECTED) {
            // Client already connected between CreateNamedPipe and ConnectNamedPipe
        } else if (last_error == ERROR_IO_PENDING) {
            // Async connect pending — wait with periodic running_ check
            bool got_connection = false;
            while (running_) {
                DWORD wait_result = WaitForSingleObjectEx(
                    static_cast<HANDLE>(accept_event_), config_.poll_interval_ms, TRUE);

                if (wait_result == WAIT_OBJECT_0) {
                    DWORD bytes_transferred = 0;
                    if (GetOverlappedResult(h, &ov, &bytes_transferred, FALSE)) {
                        got_connection = true;
                        break;
                    }
                    DWORD err = GetLastError();
                    if (err == ERROR_OPERATION_ABORTED) {
                        CloseHandle(h);
                        break;
                    }
                    CloseHandle(h);
                    if (error_handler_) {
                        error_handler_("ConnectNamedPipe async failed: " + std::to_string(err));
                    }
                    break;
                }
                if (wait_result == WAIT_TIMEOUT) {
                    if (!running_) {
                        CancelIoEx(h, &ov);
                        WaitForSingleObjectEx(static_cast<HANDLE>(accept_event_), 100, TRUE);
                        CloseHandle(h);
                        break;
                    }
                    continue;
                }
                // WAIT_FAILED
                CloseHandle(h);
                if (error_handler_) {
                    error_handler_("WaitForSingleObjectEx failed in accept");
                }
                break;
            }
            if (!running_) break;
            if (!got_connection) continue;
        } else {
            // Unexpected error
            CloseHandle(h);
            if (error_handler_) {
                error_handler_("ConnectNamedPipe failed: " + std::to_string(last_error));
            }
            continue;
        }

        if (!running_) {
            CloseHandle(h);
            break;
        }

        // Client connected
        int conn_id = next_connection_id_++;
        auto conn = std::make_shared<LocalPipeConnection>(h, conn_id);
        conn->active = true;
        conn->read_event = CreateEvent(nullptr, TRUE, FALSE, nullptr);
        conn->write_event = CreateEvent(nullptr, TRUE, FALSE, nullptr);

        {
            std::lock_guard<std::mutex> lock(connections_mutex_);
            active_connections_.push_back(conn);
        }

        conn->reader_thread = std::thread([this, conn]() {
            client_read_loop(conn);
        });

        std::cerr << "[cppmcp] Client #" << conn_id << " connected on pipe " << path << std::endl;

        if (config_.mode == PipeMode::SingleClient) {
            std::unique_lock<std::mutex> lock(stop_mutex_);
            stop_cv_.wait(lock, [this] { return !running_.load(); });
            break;
        }
        // MultiClient: loop continues creating next instance
    }

    if (accept_event_) {
        CloseHandle(static_cast<HANDLE>(accept_event_));
        accept_event_ = nullptr;
    }
}

std::string LocalPipeTransport::read_message(std::shared_ptr<LocalPipeConnection> conn) {
    // Overlapped message-mode read: ReadFile returns one pipe message per call
    HANDLE h = static_cast<HANDLE>(conn->handle);
    HANDLE evt = static_cast<HANDLE>(conn->read_event);
    std::string result;
    std::vector<char> buffer(config_.buffer_size);

    ResetEvent(evt);
    OVERLAPPED ov = {};
    ov.hEvent = evt;

    DWORD bytes_read = 0;
    BOOL success = ReadFile(h, buffer.data(), static_cast<DWORD>(buffer.size()),
                            &bytes_read, &ov);

    DWORD last_error = GetLastError();

    if (success) {
        // Read completed immediately
        result.append(buffer.data(), bytes_read);
    } else if (last_error == ERROR_IO_PENDING) {
        // Async read pending — wait with periodic running_ check
        bool read_complete = false;
        while (running_ && conn->active.load()) {
            DWORD wait_result = WaitForSingleObjectEx(evt, config_.poll_interval_ms, TRUE);
            if (wait_result == WAIT_OBJECT_0) {
                DWORD transferred = 0;
                if (GetOverlappedResult(h, &ov, &transferred, FALSE)) {
                    result.append(buffer.data(), transferred);
                    read_complete = true;
                    break;
                }
                DWORD err = GetLastError();
                if (err == ERROR_BROKEN_PIPE || err == ERROR_PIPE_NOT_CONNECTED ||
                    err == ERROR_OPERATION_ABORTED) {
                    return "";
                }
                if (error_handler_) {
                    error_handler_("Overlapped read failed: " + std::to_string(err));
                }
                return "";
            }
            if (wait_result == WAIT_TIMEOUT) {
                if (!running_ || !conn->active.load()) {
                    CancelIoEx(h, &ov);
                    WaitForSingleObjectEx(evt, 100, TRUE);
                    return "";
                }
                continue;
            }
            // WAIT_FAILED
            return "";
        }
        if (!read_complete) return "";
    } else if (last_error == ERROR_BROKEN_PIPE || last_error == ERROR_PIPE_NOT_CONNECTED) {
        return "";
    } else {
        if (error_handler_) {
            error_handler_("ReadFile failed: " + std::to_string(last_error));
        }
        return "";
    }

    // Read remaining fragments if message was larger than buffer
    while (GetLastError() == ERROR_MORE_DATA) {
        ResetEvent(evt);
        OVERLAPPED ov2 = {};
        ov2.hEvent = evt;
        DWORD more_bytes = 0;
        BOOL more_success = ReadFile(h, buffer.data(), static_cast<DWORD>(buffer.size()),
                                     &more_bytes, &ov2);
        DWORD more_error = GetLastError();
        if (more_success) {
            result.append(buffer.data(), more_bytes);
        } else if (more_error == ERROR_IO_PENDING) {
            DWORD transferred = 0;
            if (GetOverlappedResult(h, &ov2, &transferred, TRUE)) {
                result.append(buffer.data(), transferred);
            } else {
                break;
            }
        } else {
            break;
        }
    }

    return result;
}

void LocalPipeTransport::client_read_loop(std::shared_ptr<LocalPipeConnection> conn) {
    while (running_ && conn->active.load()) {
        std::string message = read_message(conn);

        if (message.empty()) {
            conn->active = false;
            break;
        }

        try {
            auto json_msg = nlohmann::json::parse(message);
            {
                std::lock_guard<std::mutex> rlock(responding_mutex_);
                responding_connection_ = conn;
            }
            if (message_handler_) {
                message_handler_(json_msg);
            }
            {
                std::lock_guard<std::mutex> rlock(responding_mutex_);
                responding_connection_ = nullptr;
            }
        } catch (const nlohmann::json::parse_error& e) {
            auto error_resp = make_error_response_null_id(Protocol::PARSE_ERROR, e.what());
            {
                std::lock_guard<std::mutex> rlock(responding_mutex_);
                responding_connection_ = conn;
            }
            conn->write_message(error_resp.dump());
            {
                std::lock_guard<std::mutex> rlock(responding_mutex_);
                responding_connection_ = nullptr;
            }
            if (error_handler_) {
                error_handler_("JSON parse error: " + std::string(e.what()));
            }
        } catch (const std::exception& e) {
            if (error_handler_) {
                error_handler_("Error processing message: " + std::string(e.what()));
            }
        }
    }

    std::cerr << "[cppmcp] Client #" << conn->connection_id << " disconnected" << std::endl;

    // Clean up pipe handle and overlapped events
    if (conn->handle != LocalPipeConnection::INVALID_HANDLE) {
        HANDLE h = static_cast<HANDLE>(conn->handle);
        FlushFileBuffers(h);
        DisconnectNamedPipe(h);
        CloseHandle(h);
        conn->handle = LocalPipeConnection::INVALID_HANDLE;
    }
    if (conn->read_event) {
        CloseHandle(static_cast<HANDLE>(conn->read_event));
        conn->read_event = nullptr;
    }
    if (conn->write_event) {
        CloseHandle(static_cast<HANDLE>(conn->write_event));
        conn->write_event = nullptr;
    }

    // Remove from active connections
    {
        std::lock_guard<std::mutex> lock(connections_mutex_);
        active_connections_.erase(
            std::remove(active_connections_.begin(), active_connections_.end(), conn),
            active_connections_.end());
    }
}

void LocalPipeTransport::stop() {
    if (!running_.load()) return;
    running_ = false;

    // Signal stop condition variable (unblocks SingleClient wait)
    stop_cv_.notify_all();

    // Cancel I/O and close handles on all active connections
    std::vector<std::shared_ptr<LocalPipeConnection>> connections_copy;
    {
        std::lock_guard<std::mutex> lock(connections_mutex_);
        connections_copy = active_connections_;
    }

    for (auto& conn : connections_copy) {
        conn->active = false;
        if (conn->handle != LocalPipeConnection::INVALID_HANDLE) {
            HANDLE h = static_cast<HANDLE>(conn->handle);
            CancelIoEx(h, nullptr);
            DisconnectNamedPipe(h);
            CloseHandle(h);
            conn->handle = LocalPipeConnection::INVALID_HANDLE;
        }
        // Signal read/write events to unblock any waiting threads
        if (conn->read_event) {
            SetEvent(static_cast<HANDLE>(conn->read_event));
        }
        if (conn->write_event) {
            SetEvent(static_cast<HANDLE>(conn->write_event));
        }
    }

    // Join reader threads
    for (auto& conn : connections_copy) {
        if (conn->reader_thread.joinable()) {
            conn->reader_thread.join();
        }
        if (conn->read_event) {
            CloseHandle(static_cast<HANDLE>(conn->read_event));
            conn->read_event = nullptr;
        }
        if (conn->write_event) {
            CloseHandle(static_cast<HANDLE>(conn->write_event));
            conn->write_event = nullptr;
        }
    }

    {
        std::lock_guard<std::mutex> lock(connections_mutex_);
        active_connections_.clear();
    }

    // Signal accept event to unblock accept loop
    if (accept_event_) {
        SetEvent(static_cast<HANDLE>(accept_event_));
    }

    if (accept_thread_.joinable()) {
        accept_thread_.join();
    }
}

bool LocalPipeTransport::is_running() const {
    return running_.load();
}

void LocalPipeTransport::send_message(const nlohmann::json& message) {
    std::string data = message.dump();

    {
        std::lock_guard<std::mutex> rlock(responding_mutex_);
        if (responding_connection_ && responding_connection_->active.load()) {
            responding_connection_->write_message(data);
            return;
        }
    }

    std::lock_guard<std::mutex> lock(connections_mutex_);
    for (auto& conn : active_connections_) {
        if (conn->active.load()) {
            conn->write_message(data);
        }
    }
}

void LocalPipeTransport::set_message_handler(MessageCallback handler) {
    message_handler_ = std::move(handler);
}

void LocalPipeTransport::set_error_handler(ErrorCallback handler) {
    error_handler_ = std::move(handler);
}

} // namespace cppmcp

#else // Unix: Unix Domain Socket with poll-based async I/O

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <poll.h>
#include <cstring>
#include <cerrno>

namespace cppmcp {

// --- Helper: resolve socket path ---
std::string LocalPipeTransport::resolve_pipe_path() const {
    return std::string("/tmp/") + config_.pipe_name + ".sock";
}

// --- LocalPipeConnection ---
bool LocalPipeConnection::write_message(const std::string& data) {
    std::lock_guard<std::mutex> lock(write_mutex);
    if (handle == INVALID_HANDLE) return false;
    std::string framed = data + "\n";
    ssize_t total_sent = 0;
    size_t remaining = framed.size();
    while (remaining > 0) {
        ssize_t sent = send(handle, framed.c_str() + total_sent, remaining, 0);
        if (sent <= 0) return false;
        total_sent += sent;
        remaining -= static_cast<size_t>(sent);
    }
    return true;
}

// --- LocalPipeTransport ---
LocalPipeTransport::LocalPipeTransport(const LocalPipeConfig& config)
    : config_(config) {}

LocalPipeTransport::~LocalPipeTransport() {
    stop();
}

void LocalPipeTransport::start() {
    running_ = true;

    std::string path = resolve_pipe_path();
    unlink(path.c_str());

    listen_fd_ = socket(AF_UNIX, SOCK_STREAM, 0);
    if (listen_fd_ < 0) {
        if (error_handler_) {
            error_handler_("socket() failed: " + std::string(strerror(errno)));
        }
        running_ = false;
        return;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);

    if (bind(listen_fd_, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        if (error_handler_) {
            error_handler_("bind() failed: " + std::string(strerror(errno)));
        }
        close(listen_fd_);
        listen_fd_ = -1;
        running_ = false;
        return;
    }

    if (listen(listen_fd_, config_.max_instances) < 0) {
        if (error_handler_) {
            error_handler_("listen() failed: " + std::string(strerror(errno)));
        }
        close(listen_fd_);
        listen_fd_ = -1;
        running_ = false;
        return;
    }

    accept_thread_ = std::thread(&LocalPipeTransport::accept_loop, this);
    std::cerr << "[cppmcp] Local pipe server starting on " << path
              << " (mode: " << (config_.mode == PipeMode::SingleClient ? "SingleClient" : "MultiClient")
              << ", poll-based async)" << std::endl;
}

void LocalPipeTransport::accept_loop() {
    while (running_) {
        struct pollfd pfd;
        pfd.fd = listen_fd_;
        pfd.events = POLLIN;

        int poll_result = poll(&pfd, 1, config_.poll_interval_ms);
        if (poll_result < 0) {
            if (errno != EINTR) {
                if (error_handler_) {
                    error_handler_("poll() failed: " + std::string(strerror(errno)));
                }
            }
            continue;
        }

        if (poll_result == 0) continue;

        int client_fd = accept(listen_fd_, nullptr, nullptr);
        if (client_fd < 0) {
            if (error_handler_) {
                error_handler_("accept() failed: " + std::string(strerror(errno)));
            }
            continue;
        }

        if (!running_) {
            close(client_fd);
            break;
        }

        int conn_id = next_connection_id_++;
        auto conn = std::make_shared<LocalPipeConnection>(client_fd, conn_id);
        conn->active = true;

        {
            std::lock_guard<std::mutex> lock(connections_mutex_);
            active_connections_.push_back(conn);
        }

        conn->reader_thread = std::thread([this, conn]() {
            client_read_loop(conn);
        });

        std::cerr << "[cppmcp] Client #" << conn_id << " connected on socket "
                  << resolve_pipe_path() << std::endl;

        if (config_.mode == PipeMode::SingleClient) {
            std::unique_lock<std::mutex> lock(stop_mutex_);
            stop_cv_.wait(lock, [this] { return !running_.load(); });
            break;
        }
    }
}

std::string LocalPipeTransport::read_message(std::shared_ptr<LocalPipeConnection> conn) {
    // Line-based framing with poll-based async recv
    while (running_ && conn->active.load()) {
        size_t newline_pos = conn->read_buffer.find('\n');
        if (newline_pos != std::string::npos) {
            std::string message = conn->read_buffer.substr(0, newline_pos);
            conn->read_buffer.erase(0, newline_pos + 1);
            return message;
        }

        // Poll the client fd for incoming data
        struct pollfd pfd;
        pfd.fd = conn->handle;
        pfd.events = POLLIN;

        int poll_result = poll(&pfd, 1, config_.poll_interval_ms);
        if (poll_result < 0) {
            if (errno != EINTR) {
                if (error_handler_) {
                    error_handler_("poll() on client fd failed: " + std::string(strerror(errno)));
                }
            }
            continue;
        }
        if (poll_result == 0) continue;

        std::vector<char> buf(config_.buffer_size);
        ssize_t bytes = recv(conn->handle, buf.data(), buf.size(), 0);
        if (bytes <= 0) return "";

        conn->read_buffer.append(buf.data(), static_cast<size_t>(bytes));
    }
    return "";
}

void LocalPipeTransport::client_read_loop(std::shared_ptr<LocalPipeConnection> conn) {
    while (running_ && conn->active.load()) {
        std::string message = read_message(conn);

        if (message.empty()) {
            conn->active = false;
            break;
        }

        try {
            auto json_msg = nlohmann::json::parse(message);
            {
                std::lock_guard<std::mutex> rlock(responding_mutex_);
                responding_connection_ = conn;
            }
            if (message_handler_) {
                message_handler_(json_msg);
            }
            {
                std::lock_guard<std::mutex> rlock(responding_mutex_);
                responding_connection_ = nullptr;
            }
        } catch (const nlohmann::json::parse_error& e) {
            auto error_resp = make_error_response_null_id(Protocol::PARSE_ERROR, e.what());
            {
                std::lock_guard<std::mutex> rlock(responding_mutex_);
                responding_connection_ = conn;
            }
            conn->write_message(error_resp.dump());
            {
                std::lock_guard<std::mutex> rlock(responding_mutex_);
                responding_connection_ = nullptr;
            }
            if (error_handler_) {
                error_handler_("JSON parse error: " + std::string(e.what()));
            }
        } catch (const std::exception& e) {
            if (error_handler_) {
                error_handler_("Error processing message: " + std::string(e.what()));
            }
        }
    }

    std::cerr << "[cppmcp] Client #" << conn->connection_id << " disconnected" << std::endl;

    if (conn->handle != LocalPipeConnection::INVALID_HANDLE) {
        close(conn->handle);
        conn->handle = LocalPipeConnection::INVALID_HANDLE;
    }

    {
        std::lock_guard<std::mutex> lock(connections_mutex_);
        active_connections_.erase(
            std::remove(active_connections_.begin(), active_connections_.end(), conn),
            active_connections_.end());
    }
}

void LocalPipeTransport::stop() {
    if (!running_.load()) return;
    running_ = false;

    stop_cv_.notify_all();

    std::vector<std::shared_ptr<LocalPipeConnection>> connections_copy;
    {
        std::lock_guard<std::mutex> lock(connections_mutex_);
        connections_copy = active_connections_;
    }

    for (auto& conn : connections_copy) {
        conn->active = false;
        if (conn->handle != LocalPipeConnection::INVALID_HANDLE) {
            close(conn->handle);
            conn->handle = LocalPipeConnection::INVALID_HANDLE;
        }
    }

    for (auto& conn : connections_copy) {
        if (conn->reader_thread.joinable()) {
            conn->reader_thread.join();
        }
    }

    {
        std::lock_guard<std::mutex> lock(connections_mutex_);
        active_connections_.clear();
    }

    if (listen_fd_ >= 0) {
        close(listen_fd_);
        listen_fd_ = -1;
    }

    unlink(resolve_pipe_path().c_str());

    if (accept_thread_.joinable()) {
        accept_thread_.join();
    }
}

bool LocalPipeTransport::is_running() const {
    return running_.load();
}

void LocalPipeTransport::send_message(const nlohmann::json& message) {
    std::string data = message.dump();

    {
        std::lock_guard<std::mutex> rlock(responding_mutex_);
        if (responding_connection_ && responding_connection_->active.load()) {
            responding_connection_->write_message(data);
            return;
        }
    }

    std::lock_guard<std::mutex> lock(connections_mutex_);
    for (auto& conn : active_connections_) {
        if (conn->active.load()) {
            conn->write_message(data);
        }
    }
}

void LocalPipeTransport::set_message_handler(MessageCallback handler) {
    message_handler_ = std::move(handler);
}

void LocalPipeTransport::set_error_handler(ErrorCallback handler) {
    error_handler_ = std::move(handler);
}

} // namespace cppmcp

#endif