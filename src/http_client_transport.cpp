#include "cppmcp/http_client_transport.hpp"

#include "cppmcp/logging.hpp"
#include "cppmcp/protocol.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace cppmcp {

HttpClientTransport::HttpClientTransport(std::string host, uint16_t port, std::string endpoint)
    : host_(std::move(host)), port_(port), endpoint_(std::move(endpoint)) {}

HttpClientTransport::~HttpClientTransport() {
    disconnect();
}

void HttpClientTransport::set_message_handler(MessageCallback handler) {
    message_handler_ = std::move(handler);
}
void HttpClientTransport::set_error_handler(ErrorCallback handler) {
    error_handler_ = std::move(handler);
}
void HttpClientTransport::set_disconnect_handler(DisconnectCallback handler) {
    disconnect_handler_ = std::move(handler);
}
void HttpClientTransport::set_io_context(asio::io_context* io_ctx) {
    io_ctx_ = io_ctx;
}
void HttpClientTransport::set_extra_headers(std::vector<std::pair<std::string, std::string>> headers) {
    extra_headers_ = std::move(headers);
}

bool HttpClientTransport::is_connected() const {
    return connected_.load();
}

void HttpClientTransport::init_parser() {
    llhttp_settings_init(&settings_);

    settings_.on_header_field = [](llhttp_t* parser, const char* at, size_t length) -> int {
        auto* self = static_cast<HttpClientTransport*>(parser->data);
        if (!self->cur_header_field_.empty()) {
            self->flush_header();
        }
        self->cur_header_field_.append(at, length);
        return 0;
    };
    settings_.on_header_value = [](llhttp_t* parser, const char* at, size_t length) -> int {
        auto* self = static_cast<HttpClientTransport*>(parser->data);
        self->cur_header_value_.append(at, length);
        return 0;
    };
    settings_.on_headers_complete = [](llhttp_t* parser) -> int {
        auto* self = static_cast<HttpClientTransport*>(parser->data);
        self->response_status_ = static_cast<int>(parser->status_code);
        self->flush_header();
        return 0;
    };
    settings_.on_body = [](llhttp_t* parser, const char* at, size_t length) -> int {
        auto* self = static_cast<HttpClientTransport*>(parser->data);
        if (self->response_body_.size() + length > self->max_body_size_) {
            return -1;  // oversized response body -> parse error -> on_read error path
        }
        self->response_body_.append(at, length);
        return 0;
    };
    settings_.on_message_complete = [](llhttp_t* parser) -> int {
        auto* self = static_cast<HttpClientTransport*>(parser->data);
        self->message_complete_ = true;
        return HPE_PAUSED;
    };

    llhttp_init(&parser_, HTTP_RESPONSE, &settings_);
    parser_.data = this;
}

void HttpClientTransport::flush_header() {
    if (cur_header_field_.empty()) {
        return;
    }
    std::string field = cur_header_field_;
    for (auto& c : field) c = static_cast<char>(tolower(c));
    response_headers_[field] = cur_header_value_;
    if (field == "mcp-session-id" && !cur_header_value_.empty()) {
        std::lock_guard<std::mutex> lock(session_mutex_);
        session_id_ = cur_header_value_;
    }
    cur_header_field_.clear();
    cur_header_value_.clear();
}

void HttpClientTransport::connect() {
    bool was = connected_.exchange(true);
    if (was) {
        return;
    }
    if (!io_ctx_) {
        connected_.store(false);
        throw std::runtime_error("HttpClientTransport: no io_context");
    }
    stopping_.store(false);
    init_parser();

    try {
        asio::ip::tcp::resolver resolver(*io_ctx_);
        auto results = resolver.resolve(host_, std::to_string(port_));
        socket_ = std::make_unique<asio::ip::tcp::socket>(*io_ctx_);
        asio::connect(*socket_, results);
    } catch (...) {
        connected_.store(false);  // restore state so a later connect() can retry
        throw;
    }

    auto self = shared_from_this();
    write_queue_ = std::make_shared<AsyncWriteQueue>(
        [self](std::shared_ptr<std::string> buffer, AsyncWriteQueue::WriteCompletion completion) {
            asio::async_write(*self->socket_, asio::buffer(*buffer),
                [buffer, completion](const asio::error_code& ec, std::size_t) { completion(ec); });
        },
        [self](const asio::error_code&) {
            if (self->error_handler_) {
                self->error_handler_("http write error");
            }
        });

    connected_.store(true);
    start_read();
}

void HttpClientTransport::disconnect() {
    // Terminate the MCP session per spec (DELETE), best-effort fire-and-forget:
    // do NOT wait for the response — a keep-alive server may never close the
    // connection, and blocking here would hang the caller (observed as a
    // deadlock when the server keeps the POST socket open after 200).
    std::string sid_snapshot;
    {
        std::lock_guard<std::mutex> lock(session_mutex_);
        sid_snapshot = session_id_;
    }
    if (connected_.load() && !sid_snapshot.empty() && socket_ && socket_->is_open()) {
        try {
            asio::error_code ec;
            std::string req = "DELETE " + endpoint_ + " HTTP/1.1\r\n";
            req += "Host: " + host_ + ":" + std::to_string(port_) + "\r\n";
            req += "Mcp-Session-Id: " + sid_snapshot + "\r\n";
            req += "Connection: close\r\n\r\n";
            asio::write(*socket_, asio::buffer(req), ec);
        } catch (const std::exception&) {
            // best-effort only
        }
    }

    stopping_.store(true);
    connected_.store(false);
    if (write_queue_) {
        write_queue_->shutdown();
    }
    close_sse();
    if (socket_) {
        asio::error_code ec;
        socket_->close(ec);
    }
    {
        std::lock_guard<std::mutex> lock(session_mutex_);
        session_id_.clear();  // a reconnect must negotiate a fresh session
    }
    // Defer clearing handlers so the in-flight read completion (triggered by
    // socket close above) can still invoke disconnect_handler_. Posting keeps
    // this transport alive (via captured self) until the io loop runs it.
    if (io_ctx_ && (message_handler_ || error_handler_ || disconnect_handler_)) {
        auto self = shared_from_this();
        asio::post(*io_ctx_, [self]() { self->clear_handlers(); });
    } else {
        clear_handlers();
    }
}

void HttpClientTransport::clear_handlers() {
    message_handler_ = {};
    error_handler_ = {};
    disconnect_handler_ = {};
}

std::string HttpClientTransport::build_post_request(std::string body) const {
    std::string req = "POST " + endpoint_ + " HTTP/1.1\r\n";
    req += "Host: " + host_ + ":" + std::to_string(port_) + "\r\n";
    req += "Content-Type: application/json\r\n";
    req += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    req += "Accept: application/json, text/event-stream\r\n";
    std::string sid;
    {
        std::lock_guard<std::mutex> lock(session_mutex_);
        sid = session_id_;
    }
    if (!sid.empty()) {
        req += "Mcp-Session-Id: " + sid + "\r\n";
    }
    for (const auto& [k, v] : extra_headers_) {
        req += k + ": " + v + "\r\n";
    }
    req += "Connection: keep-alive\r\n";
    req += "\r\n";
    req += std::move(body);  // move-append: one copy total instead of two
    return req;
}

void HttpClientTransport::send_message(const nlohmann::json& message) {
    if (!write_queue_) {
        if (error_handler_) error_handler_("http transport not connected");
        return;
    }
    std::string body = message.dump();
    std::string req = build_post_request(std::move(body));
    write_queue_->enqueue(std::move(req));
}

void HttpClientTransport::start_read() {
    if (!socket_ || stopping_.load()) {
        return;
    }
    arm_read_timer();
    auto self = shared_from_this();
    socket_->async_read_some(read_buf_.prepare(8192),
        [self](const asio::error_code& ec, std::size_t bytes) { self->on_read(ec, bytes); });
}

void HttpClientTransport::arm_read_timer() {
    // No dedicated timer: the per-request timeout at the McpClient layer
    // bounds how long a request may hang; a transport-level deadline would
    // kill keep-alive sockets between requests. Kept as a hook.
}

void HttpClientTransport::on_read(const asio::error_code& ec, std::size_t bytes) {
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
    read_buf_.commit(bytes);
    auto bufs = read_buf_.data();
    std::string data(asio::buffers_begin(bufs), asio::buffers_begin(bufs) + read_buf_.size());
    read_buf_.consume(read_buf_.size());

    const char* base = data.data();
    std::size_t total = data.size();
    std::size_t offset = 0;
    while (offset < total) {
        llhttp_errno err = llhttp_execute(&parser_, base + offset, total - offset);
        if (err == HPE_OK) {
            break;  // consumed available; response may be incomplete (need more)
        }
        if (err == HPE_PAUSED) {
            std::size_t parsed = static_cast<std::size_t>(llhttp_get_error_pos(&parser_) - (base + offset));
            offset += parsed;
            if (message_complete_) {
                handle_response();
            }
            llhttp_reset(&parser_);
            response_body_.clear();
            response_headers_.clear();
            response_status_ = 0;
            message_complete_ = false;
            llhttp_resume(&parser_);
            continue;
        }
        if (error_handler_) {
            error_handler_(std::string("http response parse error: ") + llhttp_errno_name(err));
        }
        break;
    }

    if (!stopping_.load()) {
        start_read();
    }
}

void HttpClientTransport::handle_response() {
    if (response_status_ == 200 || response_status_ == 202) {
        // Server chose to stream the response over SSE (allowed by the spec):
        // parse "data:" frames as JSON-RPC messages.
        auto ct = response_headers_.find("content-type");
        bool is_sse = ct != response_headers_.end() &&
                      ct->second.find("text/event-stream") != std::string::npos;
        if (is_sse) {
            handle_sse_frame("", response_body_);
            // Open the standalone GET stream once we have a session (server->client pushes).
            if (!sse_open_ && !session_id_snapshot().empty()) {
                start_sse_get();
            }
            return;
        }
        if (!response_body_.empty()) {
            try {
                auto j = nlohmann::json::parse(response_body_);
                if (message_handler_) {
                    message_handler_(j);
                }
                // First successful POST: the session exists now — open the GET
                // SSE stream for server-initiated pushes.
                if (!sse_open_ && !session_id_snapshot().empty()) {
                    start_sse_get();
                }
            } catch (const std::exception& e) {
                AsyncLogger::instance().log(std::string("http body parse error: ") + e.what());
                if (error_handler_) {
                    error_handler_("http body parse error");
                }
            }
        }
        return;
    }
    // Non-2xx: synthesize a JSON-RPC error response so the corresponding
    // pending request completes instead of hanging (e.g. 404 session expired,
    // which per spec means the client must re-initialize).
    if (message_handler_) {
        int code = Protocol::INTERNAL_ERROR;
        std::string msg = "http server returned status " + std::to_string(response_status_);
        if (response_status_ == 404) {
            code = Protocol::CLIENT_NOT_CONNECTED;
            msg += " (session not found — reconnect required)";
        } else if (response_status_ == 400) {
            code = Protocol::INVALID_REQUEST;
        } else if (response_status_ == 429) {
            code = Protocol::REQUEST_FAILED;
        }
        nlohmann::json body;
        try {
            body = nlohmann::json::parse(response_body_);
        } catch (const std::exception&) {
            body = nlohmann::json::object();
        }
        nlohmann::json err = nlohmann::json{{"code", code}, {"message", msg}};
        if (!body.empty()) {
            err["data"] = body;
        }
        // null id: the client cannot map this to a request, but the error
        // surfaces via parse_message_client as a JsonRpcErrorResponse.
        message_handler_(nlohmann::json{
            {"jsonrpc", "2.0"},
            {"id", nullptr},
            {"error", err}
        });
    }
    if (error_handler_) {
        error_handler_("http server returned status " + std::to_string(response_status_));
    }
}

// --- GET SSE stream (server -> client pushes) ---

void HttpClientTransport::start_sse_get() {
    if (sse_open_ || stopping_.load() || !io_ctx_ || session_id_snapshot().empty()) {
        return;
    }
    sse_open_ = true;  // reserve: no concurrent opens
    auto self = shared_from_this();
    // Fully async: resolve -> connect -> send GET -> read loop. Never blocks
    // the io thread that is busy serving the POST channel.
    auto resolver = std::make_shared<asio::ip::tcp::resolver>(*io_ctx_);
    sse_socket_ = std::make_unique<asio::ip::tcp::socket>(*io_ctx_);
    resolver->async_resolve(host_, std::to_string(port_),
        [self, resolver](const asio::error_code& ec, asio::ip::tcp::resolver::results_type results) {
            if (ec || !self->sse_socket_) {
                self->sse_open_ = false;
                if (self->sse_socket_) self->sse_socket_.reset();
                return;
            }
            asio::async_connect(*self->sse_socket_, results,
                [self, resolver](const asio::error_code& cec, const asio::ip::tcp::endpoint&) {
                    if (cec || !self->sse_socket_) {
                        self->sse_open_ = false;
                        if (self->sse_socket_) self->sse_socket_.reset();
                        return;
                    }
                    std::string req = "GET " + self->endpoint_ + " HTTP/1.1\r\n";
                    req += "Host: " + self->host_ + ":" + std::to_string(self->port_) + "\r\n";
                    req += "Accept: text/event-stream\r\n";
                    req += "Mcp-Session-Id: " + self->session_id_snapshot() + "\r\n";
                    for (const auto& [k, v] : self->extra_headers_) {
                        req += k + ": " + v + "\r\n";
                    }
                    req += "Connection: keep-alive\r\n\r\n";
                    auto req_buf = std::make_shared<std::string>(std::move(req));
                    asio::async_write(*self->sse_socket_, asio::buffer(*req_buf),
                        [self, resolver, req_buf](const asio::error_code& wec, std::size_t) {
                            if (wec) {
                                self->sse_open_ = false;
                                return;
                            }
                            AsyncLogger::instance().log("SSE GET stream opened");
                            self->sse_socket_->async_read_some(self->sse_buf_.prepare(8192),
                                [self](const asio::error_code& e, std::size_t bytes) {
                                    self->on_sse_read(e, bytes);
                                });
                        });
                });
        });
}

void HttpClientTransport::on_sse_read(const asio::error_code& ec, std::size_t bytes) {
    if (ec) {
        sse_open_ = false;
        // The GET stream dying does not imply the POST channel is dead; just log.
        AsyncLogger::instance().log("SSE GET stream closed: " + ec.message());
        return;
    }
    sse_buf_.commit(bytes);
    auto bufs = sse_buf_.data();
    std::string data(asio::buffers_begin(bufs), asio::buffers_begin(bufs) + sse_buf_.size());
    sse_buf_.consume(sse_buf_.size());

    // Minimal SSE framing: lines; "event: X", "data: Y", blank line dispatches.
    std::size_t start = 0;
    while (start < data.size()) {
        std::size_t nl = data.find('\n', start);
        std::string line = data.substr(start, nl == std::string::npos ? std::string::npos : nl - start);
        while (!line.empty() && (line.back() == '\r')) {
            line.pop_back();
        }
        if (line.empty()) {
            if (!sse_data_.empty()) {
                handle_sse_frame(sse_event_, sse_data_);
                sse_event_.clear();
                sse_data_.clear();
            }
        } else if (line.rfind("event:", 0) == 0) {
            sse_event_ = line.substr(6);
            while (!sse_event_.empty() && sse_event_.front() == ' ') sse_event_.erase(0, 1);
        } else if (line.rfind("data:", 0) == 0) {
            std::string d = line.substr(5);
            while (!d.empty() && d.front() == ' ') d.erase(0, 1);
            if (!sse_data_.empty()) sse_data_ += "\n";
            sse_data_ += d;
        }  // ignore comments (:) and other fields
        if (nl == std::string::npos) {
            break;
        }
        start = nl + 1;
    }

    if (!stopping_.load() && sse_socket_) {
        auto self = shared_from_this();
        sse_socket_->async_read_some(sse_buf_.prepare(8192),
            [self](const asio::error_code& e, std::size_t b) { self->on_sse_read(e, b); });
    }
}

void HttpClientTransport::handle_sse_frame(const std::string& /*event*/, const std::string& data) {
    if (data.empty() || !message_handler_) {
        return;
    }
    // The body may hold multiple SSE frames or be a bare JSON doc; parse each
    // "data:"-looking chunk.
    std::size_t start = 0;
    while (start < data.size()) {
        std::size_t nl = data.find('\n', start);
        std::string line = data.substr(start, nl == std::string::npos ? std::string::npos : nl - start);
        while (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.rfind("data:", 0) == 0) {
            std::string payload = line.substr(5);
            while (!payload.empty() && payload.front() == ' ') payload.erase(0, 1);
            try {
                auto j = nlohmann::json::parse(payload);
                message_handler_(j);
            } catch (const std::exception&) {
                // keep-alive comments / non-JSON frames are ignored
            }
        } else if (!line.empty()) {
            // bare JSON (server streamed the response body without framing)
            try {
                auto j = nlohmann::json::parse(line);
                message_handler_(j);
            } catch (const std::exception&) {
            }
        }
        if (nl == std::string::npos) {
            break;
        }
        start = nl + 1;
    }
}

void HttpClientTransport::close_sse() {
    if (sse_socket_) {
        asio::error_code ec;
        sse_socket_->close(ec);
        sse_socket_.reset();
    }
    sse_open_ = false;
    sse_event_.clear();
    sse_data_.clear();
}

} // namespace cppmcp
