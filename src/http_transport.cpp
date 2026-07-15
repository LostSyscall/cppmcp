#include "cppmcp/http_transport.hpp"
#include "cppmcp/jsonrpc.hpp"
#include "cppmcp/protocol.hpp"
#include "cppmcp/common.hpp"

#include <algorithm>
#include <iostream>
#include <random>

namespace cppmcp {

// --- HttpConnection ---
HttpConnection::HttpConnection(asio::ip::tcp::socket s)
    : socket(std::move(s)), read_timer_(socket.get_executor()) {
    llhttp_settings_init(&parser_settings);

    parser_settings.on_url = [](llhttp_t* parser, const char* at, size_t length) {
        auto* self = static_cast<HttpConnection*>(parser->data);
        self->current_url.append(at, length);
        return 0;
    };

    parser_settings.on_method = [](llhttp_t* parser, const char* at, size_t length) {
        auto* self = static_cast<HttpConnection*>(parser->data);
        self->current_method.append(at, length);
        return 0;
    };

    parser_settings.on_body = [](llhttp_t* parser, const char* at, size_t length) {
        auto* self = static_cast<HttpConnection*>(parser->data);
        if (self->current_body.size() + length > self->max_body_size) {
            return -1;  // oversize body -> llhttp error -> 400 (reject during streaming)
        }
        self->current_body.append(at, length);
        return 0;
    };

    parser_settings.on_header_field = [](llhttp_t* parser, const char* at, size_t length) {
        auto* self = static_cast<HttpConnection*>(parser->data);
        // Flush previous header when a new field starts
        if (!self->current_header_field.empty()) {
            std::string field = self->current_header_field;
            for (auto& c : field) c = static_cast<char>(tolower(c));
            self->headers[field] = self->current_header_value;
            self->current_header_field.clear();
            self->current_header_value.clear();
        }
        self->current_header_field.append(at, length);
        return 0;
    };

    parser_settings.on_header_value = [](llhttp_t* parser, const char* at, size_t length) {
        auto* self = static_cast<HttpConnection*>(parser->data);
        self->current_header_value.append(at, length);
        return 0;
    };

    parser_settings.on_headers_complete = [](llhttp_t* parser) {
        auto* self = static_cast<HttpConnection*>(parser->data);
        // Flush last header
        if (!self->current_header_field.empty()) {
            std::string field = self->current_header_field;
            for (auto& c : field) c = static_cast<char>(tolower(c));
            self->headers[field] = self->current_header_value;
            self->current_header_field.clear();
            self->current_header_value.clear();
        }
        return 0;
    };

    parser_settings.on_message_complete = [](llhttp_t* parser) -> int {
        auto* self = static_cast<HttpConnection*>(parser->data);
        // Headers are already flushed in on_headers_complete.
        self->request_complete = true;
        return HPE_PAUSED;  // pause so pipelined requests dispatch one at a time
    };

    llhttp_init(&parser, HTTP_REQUEST, &parser_settings);
    parser.data = this;
}

void HttpConnection::start_read_with_dispatch(HttpTransport* transport) {
    auto self = shared_from_this();
    if (read_timeout_ > std::chrono::milliseconds(0)) {
        read_timer_.expires_after(read_timeout_);
        read_timer_.async_wait([this, self](const asio::error_code& ec) {
            if (ec) {
                return;  // canceled by the read completion
            }
            asio::error_code ignore;
            socket.close(ignore);  // cancel the in-flight async_read
            on_error();
        });
    }
    asio::async_read(socket, read_buf, asio::transfer_at_least(1),
        [this, self, transport](const asio::error_code& ec, std::size_t bytes_transferred) {
            if (read_timeout_ > std::chrono::milliseconds(0)) {
                read_timer_.cancel();
            }
            if (ec) {
                on_error();
                return;
            }

            auto bufs = read_buf.data();
            auto begin = asio::buffers_begin(bufs);
            auto end = begin + bytes_transferred;
            std::string data(begin, end);
            read_buf.consume(bytes_transferred);

            // llhttp pauses at each message boundary (on_message_complete
            // returns HPE_PAUSED) so pipelined/coalesced requests are
            // dispatched one at a time instead of clobbering each other's
            // parsed state.
            const char* base = data.data();
            std::size_t total = data.size();
            std::size_t offset = 0;
            while (offset < total) {
                llhttp_errno err = llhttp_execute(&parser, base + offset, total - offset);
                if (err == HPE_OK) {
                    break;  // consumed; request may still be incomplete (await more)
                }
                if (err == HPE_PAUSED_UPGRADE) {
                    llhttp_resume_after_upgrade(&parser);
                    continue;
                }
                if (err == HPE_PAUSED) {
                    std::size_t parsed = static_cast<std::size_t>(llhttp_get_error_pos(&parser) - (base + offset));
                    offset += parsed;
                    if (request_complete && !current_method.empty()) {
                        transport->handle_request(self);
                    }
                    llhttp_reset(&parser);
                    current_url.clear();
                    current_method.clear();
                    current_body.clear();
                    headers.clear();
                    current_header_field.clear();
                    current_header_value.clear();
                    request_complete = false;
                    llhttp_resume(&parser);
                    continue;
                }
                enqueue_write(transport->build_http_response(400, "text/plain", "Bad request"));
                on_error();
                return;
            }

            if (active) {
                start_read_with_dispatch(transport);
            }
        });
}

void HttpConnection::enqueue_write(std::string data) {
    if (write_queue_) {
        write_queue_->enqueue(std::move(data));
    }
}

void HttpConnection::init_write_queue(std::size_t max_queued_bytes, QueueOverflowPolicy policy) {
    auto self = shared_from_this();
    write_queue_ = std::make_shared<AsyncWriteQueue>(
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
}

void HttpConnection::on_error() {
    if (active.exchange(false)) {
        if (on_disconnect) {
            on_disconnect();
        }
    }
}

// --- SseConnection ---
void SseConnection::push(const std::string& data) {
    if (!active.load() || !conn || !conn->write_queue_) {
        return;
    }
    conn->write_queue_->enqueue("data: " + data + "\n\n");
}

// --- Utility ---
static std::string generate_session_id() {
    std::random_device rd;
    std::mt19937 gen(rd());
    static const char chars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
    std::string id;
    id.reserve(32);
    for (int i = 0; i < 32; ++i) {
        id += chars[gen() % (sizeof(chars) - 1)];
    }
    return id;
}

// Parse the host out of an Origin header ("scheme://host[:port][/path]") and
// compare against loopback aliases / configured host. Anchored, so e.g.
// "localhost.evil.com" or "https://evil.com/127.0.0.1" are NOT accepted.
static bool is_origin_allowed(const std::string& origin, const std::string& config_host) {
    std::string host = origin;
    auto scheme_end = host.find("://");
    if (scheme_end != std::string::npos) {
        host = host.substr(scheme_end + 3);
    }
    auto path_start = host.find('/');
    if (path_start != std::string::npos) {
        host = host.substr(0, path_start);
    }
    auto colon = host.find(':');
    if (colon != std::string::npos) {
        host = host.substr(0, colon);
    }
    return host == "127.0.0.1" || host == "localhost" || host == "::1" || host == config_host;
}

std::string HttpTransport::status_text(int status) {
    switch (status) {
        case 200: return "OK";
        case 202: return "Accepted";
        case 204: return "No Content";
        case 400: return "Bad Request";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 500: return "Internal Server Error";
        default: return "Unknown";
    }
}

std::string HttpTransport::build_http_response(int status, const std::string& content_type,
                                                 const std::string& body,
                                                 std::vector<std::pair<std::string, std::string>> extra_headers) {
    std::string resp = "HTTP/1.1 " + std::to_string(status) + " " + status_text(status) + "\r\n";
    resp += "Content-Type: " + content_type + "\r\n";
    resp += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    for (auto& [k, v] : extra_headers) {
        resp += k + ": " + v + "\r\n";
    }
    resp += "\r\n" + body;
    return resp;
}

// --- HttpTransport ---
HttpTransport::HttpTransport(const HttpTransportConfig& config)
    : config_(config) {}

HttpTransport::~HttpTransport() {
    stop();
}

void HttpTransport::set_io_context(asio::io_context* io_ctx) {
    io_ctx_ = io_ctx;
}

void HttpTransport::start() {
    if (!io_ctx_) {
        if (error_handler_) error_handler_("HttpTransport: no io_context set");
        return;
    }
    if (config_.port < 0 || config_.port > 65535) {
        if (error_handler_) error_handler_("HttpTransport: invalid port " + std::to_string(config_.port));
        return;
    }

    // Create and open acceptor
    acceptor_ = std::make_unique<asio::ip::tcp::acceptor>(*io_ctx_);
    asio::ip::tcp::endpoint endpoint(asio::ip::tcp::v4(), static_cast<unsigned short>(config_.port));
    acceptor_->open(endpoint.protocol());
    acceptor_->set_option(asio::ip::tcp::acceptor::reuse_address(true));
    acceptor_->bind(endpoint);
    acceptor_->listen();

    running_ = true;
    do_accept();
}

void HttpTransport::stop() {
    running_ = false;

    {
        std::unique_lock<std::shared_mutex> lock(connections_mutex_);
        for (auto& conn : active_sse_connections_) {
            conn->active = false;
            if (conn->conn && conn->conn->write_queue_) {
                conn->conn->write_queue_->shutdown();
            }
        }
        active_sse_connections_.clear();
    }

    if (acceptor_) {
        asio::error_code ec;
        acceptor_->close(ec);
    }
}

bool HttpTransport::is_running() const {
    return running_;
}

int HttpTransport::get_port() const {
    if (acceptor_ && acceptor_->is_open()) {
        asio::error_code ec;
        auto ep = acceptor_->local_endpoint(ec);
        if (!ec) return static_cast<int>(ep.port());
    }
    return config_.port;
}

void HttpTransport::do_accept() {
    acceptor_->async_accept(
        [this](const asio::error_code& ec, asio::ip::tcp::socket socket) {
            if (ec) {
                if (running_) {
                    do_accept();
                }
                return;
            }

            // Connection cap: accept-then-close when at the limit.
            if (config_.max_connections > 0 &&
                active_connection_count_.load(std::memory_order_relaxed) >= config_.max_connections) {
                asio::error_code ignore;
                socket.close(ignore);
                if (running_) {
                    do_accept();
                }
                return;
            }

            asio::error_code no_delay_ec;
            socket.set_option(asio::ip::tcp::no_delay(true), no_delay_ec);
            auto conn = std::make_shared<HttpConnection>(std::move(socket));
            conn->max_body_size = config_.max_body_size;
            conn->read_timeout_ = config_.read_timeout;
            conn->init_write_queue(config_.write_queue_max_bytes, config_.write_queue_overflow);
            conn->on_disconnect = [this]() {
                if (active_connection_count_.load(std::memory_order_relaxed) > 0) {
                    --active_connection_count_;
                }
            };
            ++active_connection_count_;
            conn->start_read_with_dispatch(this);

            if (running_) {
                do_accept();
            }
        });
}

void HttpTransport::handle_request(std::shared_ptr<HttpConnection> conn) {
    std::string method = conn->current_method;
    std::string url = conn->current_url;

    // CORS origin check
    std::string origin;
    {
        auto it = conn->headers.find("origin");
        if (it != conn->headers.end()) origin = it->second;
    }
    if (!origin.empty()) {
        if (!is_origin_allowed(origin, config_.host)) {
            conn->enqueue_write(build_http_response(403, "text/plain", "Forbidden: Origin not allowed"));
            conn->active = false;
            return;
        }
    }

    // OPTIONS for CORS preflight
    if (method == "OPTIONS") {
        handle_options(conn);
        return;
    }

    if (config_.mode == HttpTransportMode::StreamableHttp) {
        if (url == config_.path) {
            if (method == "POST") { handle_post(conn); return; }
            if (method == "GET")  { handle_get_sse(conn); return; }
            if (method == "DELETE") { handle_delete(conn); return; }
        }
    } else {
        if (url == config_.sse_path && method == "GET") { handle_sse_connect(conn); return; }
        if (url == config_.message_path && method == "POST") { handle_sse_message(conn); return; }
    }

    conn->enqueue_write(build_http_response(404, "text/plain", "Not Found"));
    conn->active = false;
}

// --- CORS OPTIONS ---
void HttpTransport::handle_options(std::shared_ptr<HttpConnection> conn) {
    std::vector<std::pair<std::string, std::string>> headers;
    headers.emplace_back("Access-Control-Allow-Methods", "GET, POST, DELETE, OPTIONS");
    headers.emplace_back("Access-Control-Allow-Headers", "Content-Type, mcp-session-id");
    headers.emplace_back("Access-Control-Max-Age", "86400");
    std::string origin;
    {
        auto it = conn->headers.find("origin");
        if (it != conn->headers.end()) origin = it->second;
    }
    if (!origin.empty()) {
        headers.emplace_back("Access-Control-Allow-Origin", origin);
    }
    conn->enqueue_write(build_http_response(204, "text/plain", "", headers));
}

// --- Streamable HTTP: POST ---
void HttpTransport::handle_post(std::shared_ptr<HttpConnection> conn) {
    std::string content_type;
    {
        auto it = conn->headers.find("content-type");
        if (it != conn->headers.end()) content_type = it->second;
    }
    if (content_type.find("application/json") == std::string::npos) {
        conn->enqueue_write(build_http_response(400, "text/plain", "Content-Type must be application/json"));
        return;
    }

    std::string req_session_id;
    {
        auto it = conn->headers.find("mcp-session-id");
        if (it != conn->headers.end()) req_session_id = it->second;
    }

    nlohmann::json json_msg;
    try {
        json_msg = nlohmann::json::parse(conn->current_body);
    } catch (const nlohmann::json::parse_error& e) {
        auto error_resp = make_error_response_null_id(Protocol::PARSE_ERROR, e.what());
        conn->enqueue_write(build_http_response(200, "application/json", error_resp.dump()));
        return;
    }

    if (json_msg.is_array()) {
        handle_post_batch(conn, json_msg, req_session_id);
        return;
    }

    ParsedMessage parsed = parse_message(json_msg);

    auto session_header = [](const std::string& sid) {
        std::vector<std::pair<std::string, std::string>> h;
        if (!sid.empty()) h.emplace_back("mcp-session-id", sid);
        return h;
    };

    if (auto* err_resp = std::get_if<JsonRpcErrorResponse>(&parsed)) {
        auto j = make_error_response(err_resp->id, err_resp->error.code,
                                     err_resp->error.message, err_resp->error.data);
        conn->enqueue_write(build_http_response(200, "application/json", j.dump(),
                                                session_header(req_session_id)));
        return;
    }

    std::string method;
    if (auto* req = std::get_if<JsonRpcRequest>(&parsed)) {
        method = req->method;
    } else if (auto* notif = std::get_if<JsonRpcNotification>(&parsed)) {
        method = notif->method;
    }

    const bool is_initialize = (method == Protocol::METHOD_INITIALIZE);

    // Resolve or create the session this request belongs to.
    std::shared_ptr<HttpSession> session;
    std::string response_session_id;

    if (is_initialize) {
        if (!req_session_id.empty()) {
            conn->enqueue_write(build_http_response(400, "text/plain", "Session already exists"));
            return;
        }
        session = std::make_shared<HttpSession>();
        session->session_id = generate_session_id();
        session->initialized.store(false);
        response_session_id = session->session_id;
    } else {
        session = find_session(req_session_id);
        if (!session) {
            conn->enqueue_write(build_http_response(404, "text/plain", "Session not found"));
            return;
        }
        if (!session->initialized.load()) {
            conn->enqueue_write(build_http_response(403, "text/plain", "Session not initialized"));
            return;
        }
        response_session_id = session->session_id;
    }

    if (!message_handler_) {
        conn->enqueue_write(build_http_response(500, "text/plain", "No handler configured"));
        return;
    }

    // Run the handler with the session bound as the notification target. The
    // response goes into the HTTP body; notifications/extra responses route to
    // the session's SSE stream.
    // The sink writes the HTTP frame on the FIRST response it receives (works
    // for synchronous handlers and for handlers deferred to the worker pool);
    // any additional emissions route to the session's SSE stream.
    auto frame_written = std::make_shared<std::atomic<bool>>(false);
    auto self = shared_from_this();
    auto conn_ref = conn;
    auto sid = response_session_id;
    ITransport::ResponseSink sink = [self, conn_ref, sid, frame_written](const nlohmann::json& resp) {
        bool expected = false;
        if (frame_written->compare_exchange_strong(expected, true)) {
            std::vector<std::pair<std::string, std::string>> h;
            if (!sid.empty()) h.emplace_back("mcp-session-id", sid);
            conn_ref->enqueue_write(self->build_http_response(200, "application/json", resp.dump(), h));
        } else {
            self->send_to_session(sid, resp);
        }
    };

    try {
        message_handler_(json_msg, sink, response_session_id);
    } catch (...) {
        // Never propagate out of the io loop.
    }

    // Commit a new session only after a successful initialize. initialize is
    // never deferred, so the synchronous handler above has already run.
    if (is_initialize) {
        session->initialized.store(true);
        std::unique_lock<std::shared_mutex> lock(sessions_mutex_);
        sessions_[session->session_id] = session;
    }

    // Notifications get a 202 (no body). Request responses are written by the
    // sink (synchronously now, or when a deferred handler completes later).
    if (std::get_if<JsonRpcNotification>(&parsed)) {
        std::vector<std::pair<std::string, std::string>> h;
        if (!response_session_id.empty()) h.emplace_back("mcp-session-id", response_session_id);
        conn->enqueue_write(build_http_response(202, "text/plain", "", h));
    }
}

// --- Streamable HTTP: batch (JSON-RPC array) ---
// Note: elements are dispatched synchronously on the io thread. If a worker
// pool is configured, deferrable elements (tools/call etc.) still run through
// on_message and may be deferred — their responses are collected only if they
// complete synchronously. Prefer non-deferrable elements in batches, or run
// without a worker pool for full batch support.
void HttpTransport::handle_post_batch(std::shared_ptr<HttpConnection> conn,
                                      const nlohmann::json& batch,
                                      const std::string& req_session_id) {
    if (batch.empty()) {
        auto e = make_error_response_null_id(Protocol::INVALID_REQUEST, "Invalid Request");
        conn->enqueue_write(build_http_response(200, "application/json", e.dump()));
        return;
    }
    auto session = find_session(req_session_id);
    if (!session || !session->initialized.load()) {
        conn->enqueue_write(build_http_response(404, "text/plain", "Session not found"));
        return;
    }
    const std::string& sid = session->session_id;

    std::vector<nlohmann::json> responses;
    for (const auto& elem : batch) {
        auto body = std::make_shared<nlohmann::json>();
        auto captured = std::make_shared<bool>(false);
        ITransport::ResponseSink sub_sink = [body, captured](const nlohmann::json& r) {
            if (!*captured) {
                *body = r;
                *captured = true;
            }
        };
        try {
            message_handler_(elem, sub_sink, sid);
        } catch (...) {
            // per-element failure does not abort the batch
        }
        if (*captured) {
            responses.push_back(*body);
        }
    }

    std::vector<std::pair<std::string, std::string>> h;
    if (!sid.empty()) h.emplace_back("mcp-session-id", sid);
    if (responses.empty()) {
        // All notifications: no response body.
        conn->enqueue_write(build_http_response(202, "text/plain", "", h));
    } else {
        nlohmann::json arr = nlohmann::json::array();
        for (auto& r : responses) {
            arr.push_back(std::move(r));
        }
        conn->enqueue_write(build_http_response(200, "application/json", arr.dump(), h));
    }
}

// --- Streamable HTTP: GET SSE ---
void HttpTransport::handle_get_sse(std::shared_ptr<HttpConnection> conn) {
    std::string req_session_id;
    {
        auto it = conn->headers.find("mcp-session-id");
        if (it != conn->headers.end()) req_session_id = it->second;
    }
    auto session = find_session(req_session_id);
    if (!session) {
        conn->enqueue_write(build_http_response(404, "text/plain", "Session not found"));
        conn->active = false;
        return;
    }

    std::string accept;
    {
        auto it = conn->headers.find("accept");
        if (it != conn->headers.end()) accept = it->second;
    }
    if (accept.find("text/event-stream") == std::string::npos) {
        conn->enqueue_write(build_http_response(400, "text/plain", "Accept header must include text/event-stream"));
        conn->active = false;
        return;
    }

    auto sse_conn = std::make_shared<SseConnection>();
    sse_conn->conn = conn;
    conn->on_disconnect = [this, sse_conn, session]() {
        std::unique_lock<std::shared_mutex> lock(sessions_mutex_);
        session->sse_connections.remove(sse_conn);
    };

    std::string resp_headers = "HTTP/1.1 200 OK\r\n";
    resp_headers += "Content-Type: text/event-stream\r\n";
    resp_headers += "Cache-Control: no-cache\r\n";
    resp_headers += "Connection: keep-alive\r\n";
    resp_headers += "mcp-session-id: " + session->session_id + "\r\n";
    std::string origin;
    {
        auto it = conn->headers.find("origin");
        if (it != conn->headers.end()) origin = it->second;
    }
    if (!origin.empty()) {
        resp_headers += "Access-Control-Allow-Origin: " + origin + "\r\n";
    }
    resp_headers += "\r\n";

    conn->enqueue_write(resp_headers + ": connected\n\n");

    {
        std::unique_lock<std::shared_mutex> lock(sessions_mutex_);
        session->sse_connections.push_back(sse_conn);
    }
}

// --- Streamable HTTP: DELETE ---
void HttpTransport::handle_delete(std::shared_ptr<HttpConnection> conn) {
    std::string req_session_id;
    {
        auto it = conn->headers.find("mcp-session-id");
        if (it != conn->headers.end()) req_session_id = it->second;
    }

    {
        std::unique_lock<std::shared_mutex> lock(sessions_mutex_);
        auto it = sessions_.find(req_session_id);
        if (req_session_id.empty() || it == sessions_.end()) {
            conn->enqueue_write(build_http_response(404, "text/plain", "Session not found"));
            return;
        }
        for (auto& sc : it->second->sse_connections) {
            sc->active = false;
        }
        it->second->sse_connections.clear();
        sessions_.erase(it);  // session is re-creatable by a later initialize
    }

    conn->enqueue_write(build_http_response(200, "text/plain", ""));
}

// --- Legacy SSE: connect ---
void HttpTransport::handle_sse_connect(std::shared_ptr<HttpConnection> conn) {
    auto sse_conn = std::make_shared<SseConnection>();
    sse_conn->conn = conn;
    conn->on_disconnect = [this, sse_conn]() {
        unregister_sse_connection(sse_conn);
        if (active_connection_count_.load(std::memory_order_relaxed) > 0) {
            --active_connection_count_;
        }
    };

    std::string resp_headers = "HTTP/1.1 200 OK\r\n";
    resp_headers += "Content-Type: text/event-stream\r\n";
    resp_headers += "Cache-Control: no-cache\r\n";
    resp_headers += "Connection: keep-alive\r\n";
    resp_headers += "\r\n";

    std::string endpoint_event = "event: endpoint\ndata: " + config_.message_path + "\n\n";
    conn->enqueue_write(resp_headers + endpoint_event);

    {
        std::unique_lock<std::shared_mutex> lock(connections_mutex_);
        active_sse_connections_.push_back(sse_conn);
    }
}

// --- Legacy SSE: message ---
void HttpTransport::handle_sse_message(std::shared_ptr<HttpConnection> conn) {
    try {
        auto json_msg = nlohmann::json::parse(conn->current_body);
        if (message_handler_) {
            message_handler_(json_msg, [this](const nlohmann::json& resp) {
                push_to_sse_connections(resp);
            }, "");
        }
        conn->enqueue_write(build_http_response(202, "text/plain", ""));
    } catch (const nlohmann::json::parse_error& e) {
        conn->enqueue_write(build_http_response(400, "text/plain", "Invalid JSON: " + std::string(e.what())));
    }
}

// --- send_message (notifications only) ---
void HttpTransport::send_message(const nlohmann::json& message) {
    if (config_.mode == HttpTransportMode::SSE) {
        push_to_sse_connections(message);  // legacy flat list
        return;
    }
    // StreamableHttp broadcast: no specific session context.
    push_to_all_sessions(message);
}

void HttpTransport::send_to_session(const std::string& session_id, const nlohmann::json& message) {
    if (config_.mode == HttpTransportMode::SSE) {
        push_to_sse_connections(message);
        return;
    }
    push_to_session_sse(session_id, message);
}

void HttpTransport::push_to_sse_connections(const nlohmann::json& message) {
    std::string data = message.dump();
    std::shared_lock<std::shared_mutex> lock(connections_mutex_);
    for (auto& conn : active_sse_connections_) {
        if (conn->active.load()) {
            conn->push(data);
        }
    }
}

std::shared_ptr<HttpSession> HttpTransport::find_session(const std::string& id) {
    if (id.empty()) {
        return nullptr;
    }
    std::shared_lock<std::shared_mutex> lock(sessions_mutex_);
    auto it = sessions_.find(id);
    if (it == sessions_.end()) {
        return nullptr;
    }
    return it->second;
}

void HttpTransport::push_to_session_sse(const std::string& id, const nlohmann::json& message) {
    std::string data = message.dump();
    std::shared_lock<std::shared_mutex> lock(sessions_mutex_);
    auto it = sessions_.find(id);
    if (it == sessions_.end()) {
        return;
    }
    for (auto& sc : it->second->sse_connections) {
        if (sc->active.load()) {
            sc->push(data);
        }
    }
}

void HttpTransport::push_to_all_sessions(const nlohmann::json& message) {
    std::string data = message.dump();
    std::shared_lock<std::shared_mutex> lock(sessions_mutex_);
    for (auto& kv : sessions_) {
        for (auto& sc : kv.second->sse_connections) {
            if (sc->active.load()) {
                sc->push(data);
            }
        }
    }
}

void HttpTransport::set_message_handler(MessageCallback handler) {
    message_handler_ = std::move(handler);
}

void HttpTransport::set_error_handler(ErrorCallback handler) {
    error_handler_ = std::move(handler);
}

void HttpTransport::unregister_sse_connection(std::shared_ptr<SseConnection> conn) {
    if (!conn) {
        return;
    }
    std::unique_lock<std::shared_mutex> lock(connections_mutex_);
    conn->active = false;
    active_sse_connections_.erase(
        std::remove(active_sse_connections_.begin(), active_sse_connections_.end(), conn),
        active_sse_connections_.end());
}

} // namespace cppmcp