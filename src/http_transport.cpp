#include "cppmcp/http_transport.hpp"
#include "cppmcp/jsonrpc.hpp"
#include "cppmcp/protocol.hpp"
#include "cppmcp/common.hpp"

#include <iostream>
#include <random>

namespace cppmcp {

// thread_local: each request thread gets its own response sender
static thread_local ITransport::ResponseSender tl_response_sender;

// --- HttpConnection ---
HttpConnection::HttpConnection(asio::ip::tcp::socket s)
    : socket(std::move(s)) {
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

    parser_settings.on_message_complete = [](llhttp_t* parser) {
        auto* self = static_cast<HttpConnection*>(parser->data);
        // Flush last header if pending
        if (!self->current_header_field.empty()) {
            std::string field = self->current_header_field;
            for (auto& c : field) c = static_cast<char>(tolower(c));
            self->headers[field] = self->current_header_value;
            self->current_header_field.clear();
            self->current_header_value.clear();
        }
        self->request_complete = true;
        return 0;
    };

    llhttp_init(&parser, HTTP_REQUEST, &parser_settings);
    parser.data = this;
}

void HttpConnection::start_read_with_dispatch(HttpTransport* transport) {
    auto self = shared_from_this();
    asio::async_read(socket, read_buf, asio::transfer_at_least(1),
        [this, self, transport](const asio::error_code& ec, std::size_t bytes_transferred) {
            if (ec) {
                active = false;
                return;
            }

            auto bufs = read_buf.data();
            auto begin = asio::buffers_begin(bufs);
            auto end = begin + bytes_transferred;
            std::string data(begin, end);
            read_buf.consume(bytes_transferred);

            llhttp_errno err = llhttp_execute(&parser, data.c_str(), data.size());
            if (err == HPE_PAUSED_UPGRADE) {
                llhttp_resume_after_upgrade(&parser);
            } else if (err != HPE_OK) {
                std::string resp = transport->build_http_response(400, "text/plain", "Bad request");
                enqueue_write(resp);
                active = false;
                return;
            }

            if (request_complete && !current_method.empty()) {
                transport->handle_request(self);
                llhttp_reset(&parser);
                current_url.clear();
                current_method.clear();
                current_body.clear();
                headers.clear();
                current_header_field.clear();
                current_header_value.clear();
                request_complete = false;
            }

            if (active) {
                start_read_with_dispatch(transport);
            }
        });
}

void HttpConnection::enqueue_write(std::string data) {
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

void HttpConnection::start_write() {
    auto self = shared_from_this();
    auto data_ptr = std::make_shared<std::string>();
    {
        std::lock_guard<std::mutex> lock(write_mutex);
        if (write_queue.empty() || !active) return;
        *data_ptr = std::move(write_queue.front());
        write_queue.pop_front();
    }
    asio::async_write(socket, asio::buffer(*data_ptr),
        [this, self, data_ptr](const asio::error_code& ec, std::size_t) {
            if (ec) {
                active = false;
                return;
            }
            {
                std::lock_guard<std::mutex> lock(write_mutex);
                if (!write_queue.empty() && active) {
                    start_write();
                }
            }
        });
}

// --- SseConnection ---
void SseConnection::push(const std::string& data) {
    std::string frame = "data: " + data + "\n\n";
    bool was_empty;
    {
        std::lock_guard<std::mutex> lock(write_mutex);
        was_empty = write_queue.empty();
        write_queue.push_back(std::move(frame));
    }
    if (was_empty && active) {
        start_write();
    }
}

void SseConnection::start_write() {
    auto self = shared_from_this();
    auto data_ptr = std::make_shared<std::string>();
    {
        std::lock_guard<std::mutex> lock(write_mutex);
        if (write_queue.empty() || !active) return;
        *data_ptr = std::move(write_queue.front());
        write_queue.pop_front();
    }
    asio::async_write(conn->socket, asio::buffer(*data_ptr),
        [this, self, data_ptr](const asio::error_code& ec, std::size_t) {
            if (ec) {
                active = false;
                return;
            }
            {
                std::lock_guard<std::mutex> lock(write_mutex);
                if (!write_queue.empty() && active) {
                    start_write();
                }
            }
        });
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

    current_session_id_ = generate_session_id();
    session_initialized_.store(true, std::memory_order_relaxed);

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

            auto conn = std::make_shared<HttpConnection>(std::move(socket));
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
        bool allowed = origin.find("127.0.0.1") != std::string::npos
                    || origin.find("localhost") != std::string::npos;
        if (!allowed) {
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
        std::vector<std::pair<std::string, std::string>> h;
        if (session_initialized_.load()) h.emplace_back("mcp-session-id", current_session_id_);
        conn->enqueue_write(build_http_response(400, "text/plain", "Content-Type must be application/json", h));
        return;
    }

    std::string req_session_id;
    {
        auto it = conn->headers.find("mcp-session-id");
        if (it != conn->headers.end()) req_session_id = it->second;
    }
    if (!req_session_id.empty() && req_session_id != current_session_id_) {
        conn->enqueue_write(build_http_response(404, "text/plain", "Session not found"));
        return;
    }

    if (conn->current_body.size() > config_.max_body_size) {
        conn->enqueue_write(build_http_response(400, "text/plain", "Request body too large"));
        return;
    }

    try {
        auto json_msg = nlohmann::json::parse(conn->current_body);
        auto parsed = parse_message(json_msg);

        if (auto* err_resp = std::get_if<JsonRpcErrorResponse>(&parsed)) {
            auto j = make_error_response(err_resp->id, err_resp->error.code,
                                         err_resp->error.message, err_resp->error.data);
            std::vector<std::pair<std::string, std::string>> h;
            if (session_initialized_.load()) h.emplace_back("mcp-session-id", current_session_id_);
            conn->enqueue_write(build_http_response(200, "application/json", j.dump(), h));
            return;
        }

        if (auto* notif = std::get_if<JsonRpcNotification>(&parsed)) {
            if (message_handler_) message_handler_(json_msg);
            std::vector<std::pair<std::string, std::string>> h;
            if (session_initialized_.load()) h.emplace_back("mcp-session-id", current_session_id_);
            conn->enqueue_write(build_http_response(202, "text/plain", "", h));
            return;
        }

        // Request — use tl_response_sender for HTTP body routing
        if (message_handler_) {
            nlohmann::json response_json;
            bool response_captured = false;

            tl_response_sender = [&](const nlohmann::json& resp) {
                if (!response_captured) {
                    response_json = resp;
                    response_captured = true;
                } else {
                    push_to_sse_connections(resp);
                }
            };

            message_handler_(json_msg);
            tl_response_sender = nullptr;

            std::vector<std::pair<std::string, std::string>> h;
            if (session_initialized_.load()) h.emplace_back("mcp-session-id", current_session_id_);

            if (response_captured) {
                conn->enqueue_write(build_http_response(200, "application/json", response_json.dump(), h));
            } else {
                conn->enqueue_write(build_http_response(500, "text/plain", "No response generated", h));
            }
        }
    } catch (const nlohmann::json::parse_error& e) {
        auto error_resp = make_error_response_null_id(Protocol::PARSE_ERROR, e.what());
        conn->enqueue_write(build_http_response(200, "application/json", error_resp.dump()));
    }
}

// --- Streamable HTTP: GET SSE ---
void HttpTransport::handle_get_sse(std::shared_ptr<HttpConnection> conn) {
    if (!session_initialized_.load()) {
        conn->enqueue_write(build_http_response(405, "text/plain", "Method Not Allowed - session not initialized"));
        conn->active = false;
        return;
    }

    std::string req_session_id;
    {
        auto it = conn->headers.find("mcp-session-id");
        if (it != conn->headers.end()) req_session_id = it->second;
    }
    if (!req_session_id.empty() && req_session_id != current_session_id_) {
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

    // Create SSE connection
    auto sse_conn = std::make_shared<SseConnection>();
    sse_conn->conn = conn;

    // Build SSE response headers
    std::string resp_headers = "HTTP/1.1 200 OK\r\n";
    resp_headers += "Content-Type: text/event-stream\r\n";
    resp_headers += "Cache-Control: no-cache\r\n";
    resp_headers += "Connection: keep-alive\r\n";
    if (session_initialized_.load()) {
        resp_headers += "mcp-session-id: " + current_session_id_ + "\r\n";
    }
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
        std::unique_lock<std::shared_mutex> lock(connections_mutex_);
        active_sse_connections_.push_back(sse_conn);
    }
}

// --- Streamable HTTP: DELETE ---
void HttpTransport::handle_delete(std::shared_ptr<HttpConnection> conn) {
    std::string req_session_id;
    {
        auto it = conn->headers.find("mcp-session-id");
        if (it != conn->headers.end()) req_session_id = it->second;
    }
    if (!req_session_id.empty() && req_session_id != current_session_id_) {
        conn->enqueue_write(build_http_response(404, "text/plain", "Session not found"));
        return;
    }

    {
        std::unique_lock<std::shared_mutex> lock(connections_mutex_);
        for (auto& c : active_sse_connections_) {
            c->active = false;
        }
        active_sse_connections_.clear();
    }

    session_initialized_.store(false);
    current_session_id_.clear();

    conn->enqueue_write(build_http_response(200, "text/plain", ""));
}

// --- Legacy SSE: connect ---
void HttpTransport::handle_sse_connect(std::shared_ptr<HttpConnection> conn) {
    auto sse_conn = std::make_shared<SseConnection>();
    sse_conn->conn = conn;

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
            message_handler_(json_msg);
        }
        conn->enqueue_write(build_http_response(202, "text/plain", ""));
    } catch (const nlohmann::json::parse_error& e) {
        conn->enqueue_write(build_http_response(400, "text/plain", "Invalid JSON: " + std::string(e.what())));
    }
}

// --- send_message ---
void HttpTransport::send_message(const nlohmann::json& message) {
    if (tl_response_sender) {
        tl_response_sender(message);
        return;
    }

    std::string data = message.dump();
    std::shared_lock<std::shared_mutex> lock(connections_mutex_);
    for (auto& conn : active_sse_connections_) {
        if (conn->active.load()) {
            conn->push(data);
        }
    }
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

void HttpTransport::set_message_handler(MessageCallback handler) {
    message_handler_ = std::move(handler);
}

void HttpTransport::set_error_handler(ErrorCallback handler) {
    error_handler_ = std::move(handler);
}

void HttpTransport::set_response_sender(ResponseSender sender) {
    tl_response_sender = std::move(sender);
}

} // namespace cppmcp