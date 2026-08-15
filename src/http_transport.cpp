#include "cppmcp/http_transport.hpp"
#include "cppmcp/jsonrpc.hpp"
#include "cppmcp/logging.hpp"
#include "cppmcp/protocol.hpp"
#include "cppmcp/common.hpp"
#include "cppmcp/server.hpp"

#include <algorithm>
#include <fstream>
#include <iostream>
#include <random>

namespace cppmcp {

// --- HttpConnection ---
HttpConnection::HttpConnection(asio::ip::tcp::socket s)
    : socket(std::move(s)), strand_(socket.get_executor()), read_timer_(socket.get_executor()) {
    llhttp_settings_init(&parser_settings);

    parser_settings.on_url = [](llhttp_t* parser, const char* at, size_t length) {
        auto* self = static_cast<HttpConnection*>(parser->data);
        if (self->current_url.size() + length > self->max_header_line_size) {
            return -1;  // oversized request line
        }
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
        if (self->headers.size() >= self->max_header_count) {
            return -1;  // too many headers
        }
        if (self->current_header_field.size() + length > self->max_header_line_size) {
            return -1;  // oversized header name
        }
        self->current_header_field.append(at, length);
        return 0;
    };

    parser_settings.on_header_value = [](llhttp_t* parser, const char* at, size_t length) {
        auto* self = static_cast<HttpConnection*>(parser->data);
        if (self->current_header_value.size() + length > self->max_header_line_size) {
            return -1;  // oversized header value
        }
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
    if (read_timeout_ > std::chrono::milliseconds(0) && !is_streaming) {
        // SSE streams (is_streaming) are long-lived idle connections; the
        // request-read timeout does not apply to them.
        read_timer_.expires_after(read_timeout_);
        read_timer_.async_wait(asio::bind_executor(strand_, [this, self](const asio::error_code& ec) {
            if (ec) {
                return;  // canceled by the read completion
            }
            asio::error_code ignore;
            socket.close(ignore);  // cancel the in-flight async_read
            on_error();
        }));
    }
    asio::async_read(socket, read_buf, asio::transfer_at_least(1),
        asio::bind_executor(strand_, [this, self, transport](const asio::error_code& ec, std::size_t bytes_transferred) {
            if (read_timeout_ > std::chrono::milliseconds(0)) {
                read_timer_.cancel();
            }
            if (ec) {
                on_error();
                return;
            }

            // Bulk-copy the received bytes (the buffer sequence may be segmented).
            std::string data;
            data.reserve(bytes_transferred);
            for (auto it = asio::buffers_begin(read_buf.data());
                 it != asio::buffers_begin(read_buf.data()) + bytes_transferred;
                 ++it) {
                data.push_back(*it);
            }
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
        }));
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
        // on_release always runs (connection-count bookkeeping); it is NOT
        // overwritten by SSE handlers unlike on_disconnect.
        if (on_release) {
            on_release();
        }
        if (on_disconnect) {
            on_disconnect();
        }
    }
}

// --- SseConnection ---
void SseConnection::push(const std::string& frame) {
    if (!active.load() || !conn || !conn->write_queue_) {
        return;
    }
    conn->write_queue_->enqueue(frame);
}

// --- Utility ---

// Cryptographically secure random bytes. Used for session ids (which act as
// auth credentials: a request carrying mcp-session-id is trusted as that session).
static void crypto_random_bytes(char* out, std::size_t n) {
#ifdef _WIN32
    // BCryptGenRandom is available on Win Vista+. Fall back to RtlGenRandom.
    HMODULE h = LoadLibraryA("bcrypt.dll");
    if (h) {
        using Fn = NTSTATUS(NTAPI*)(LPCWSTR, PUCHAR, ULONG, ULONG);
        auto fn = reinterpret_cast<Fn>(GetProcAddress(h, "BCryptGenRandom"));
        if (fn && fn(nullptr, reinterpret_cast<PUCHAR>(out), static_cast<ULONG>(n), 2 /*BCRYPT_USE_SYSTEM_PREFERRED_RNG*/) == 0) {
            FreeLibrary(h);
            return;
        }
        FreeLibrary(h);
    }
    // Fallback: SystemFunction036 (RtlGenRandom)
    HMODULE adv = LoadLibraryA("advapi32.dll");
    if (adv) {
        using Fn = BOOLEAN(NTAPI*)(PVOID, ULONG);
        auto fn = reinterpret_cast<Fn>(GetProcAddress(adv, "SystemFunction036"));
        if (fn && fn(out, static_cast<ULONG>(n))) {
            FreeLibrary(adv);
            return;
        }
        FreeLibrary(adv);
    }
#else
    std::ifstream urandom("/dev/urandom", std::ios::binary);
    if (urandom.read(out, static_cast<std::streamsize>(n))) {
        return;
    }
#endif
    // Last-resort fallback (should not happen on supported platforms).
    std::random_device rd;
    for (std::size_t i = 0; i < n; ++i) {
        out[i] = static_cast<char>(rd());
    }
}

static std::string generate_session_id() {
    static const char chars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
    constexpr int ID_LEN = 32;
    std::string id;
    id.reserve(ID_LEN);
    // Pull enough random bytes then map onto the alphabet. Map by modulo;
    // bias is negligible for a 62-symbol set used as an opaque id.
    char buf[ID_LEN];
    crypto_random_bytes(buf, ID_LEN);
    for (int i = 0; i < ID_LEN; ++i) {
        id += chars[static_cast<unsigned char>(buf[i]) % (sizeof(chars) - 1)];
    }
    return id;
}

// Strip CR/LF/NUL from a header value to prevent HTTP response splitting.
static std::string sanitize_header_value(const std::string& v) {
    std::string out;
    out.reserve(v.size());
    for (char c : v) {
        if (c != '\r' && c != '\n' && c != '\0') {
            out += c;
        }
    }
    return out;
}

// Parse the host out of an Origin header ("scheme://[userinfo@]host[:port][/path]")
// and compare against loopback aliases / configured host. Anchored, so e.g.
// "localhost.evil.com" or "https://evil.com/127.0.0.1" are NOT accepted.
static bool is_origin_allowed(const std::string& origin, const std::string& config_host) {
    // Require a scheme and only accept http/https.
    auto scheme_end = origin.find("://");
    if (scheme_end == std::string::npos || scheme_end == 0) {
        return false;
    }
    std::string scheme = origin.substr(0, scheme_end);
    for (auto& c : scheme) c = static_cast<char>(tolower(c));
    if (scheme != "http" && scheme != "https") {
        return false;
    }
    std::string rest = origin.substr(scheme_end + 3);
    // Strip userinfo (everything before the first '@' that precedes any '/').
    auto slash = rest.find('/');
    auto at = rest.find('@');
    std::string hostport = rest.substr(0, slash);
    if (at != std::string::npos && (slash == std::string::npos || at < slash)) {
        hostport = hostport.substr(at + 1);
    }
    // Strip port.
    auto colon = hostport.find(':');
    std::string host = (colon != std::string::npos) ? hostport.substr(0, colon) : hostport;
    for (auto& c : host) c = static_cast<char>(tolower(c));
    return host == "127.0.0.1" || host == "localhost" || host == "::1" || host == config_host;
}

// Validate the Host header against the configured host[:port]. Defends against
// DNS rebinding: a browser resolving an attacker domain to 127.0.0.1 still
// sends the attacker's Host, which won't match. Empty Host is rejected.
static bool is_host_allowed(const std::string& host_header, const std::string& config_host, int /*config_port*/) {
    if (host_header.empty()) {
        return false;
    }
    // Allow the configured host with or without :port, plus loopback aliases.
    std::string h = host_header;
    auto colon = h.find(':');
    std::string host_only = (colon != std::string::npos) ? h.substr(0, colon) : h;
    for (auto& c : host_only) c = static_cast<char>(tolower(c));
    std::string ch = config_host;
    for (auto& c : ch) c = static_cast<char>(tolower(c));
    return host_only == ch || host_only == "127.0.0.1" || host_only == "localhost" || host_only == "::1";
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
        case 406: return "Not Acceptable";
        case 429: return "Too Many Requests";
        case 500: return "Internal Server Error";
        case 503: return "Service Unavailable";
        default: return "Unknown";
    }
}

std::string HttpTransport::build_http_response(int status, const std::string& content_type,
                                                 const std::string& body,
                                                 std::vector<std::pair<std::string, std::string>> extra_headers) {
    std::string resp = "HTTP/1.1 " + std::to_string(status) + " " + status_text(status) + "\r\n";
    resp += "Content-Type: " + sanitize_header_value(content_type) + "\r\n";
    resp += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    for (auto& [k, v] : extra_headers) {
        resp += sanitize_header_value(k) + ": " + sanitize_header_value(v) + "\r\n";
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

    // Create and open acceptor. Resolve the configured host so the listener
    // actually binds to it (tcp::v4() alone would bind ANY — exposing a
    // "localhost" server to every interface).
    asio::error_code resolve_ec;
    asio::ip::address bind_addr = asio::ip::make_address(config_.host, resolve_ec);
    if (resolve_ec || config_.host.empty()) {
        if (error_handler_) error_handler_("HttpTransport: invalid bind host '" + config_.host + "'");
        return;
    }
    acceptor_ = std::make_unique<asio::ip::tcp::acceptor>(*io_ctx_);
    asio::ip::tcp::endpoint endpoint(bind_addr, static_cast<unsigned short>(config_.port));
    acceptor_->open(endpoint.protocol());
    acceptor_->set_option(asio::ip::tcp::acceptor::reuse_address(true));
    acceptor_->bind(endpoint);
    acceptor_->listen();

    running_ = true;
    if (config_.read_timeout <= std::chrono::milliseconds(0)) {
        AsyncLogger::instance().log("[cppmcp] HttpTransport: read_timeout is 0 (slowloris guard disabled)");
    }
    arm_session_sweep();
    do_accept();
}

void HttpTransport::stop() {
    running_ = false;

    if (session_sweep_timer_) {
        asio::error_code ec;
        session_sweep_timer_->cancel(ec);
    }

    {
        std::vector<std::shared_ptr<SseConnection>> to_close;
        {
            std::unique_lock<std::shared_mutex> lock(connections_mutex_);
            for (auto& conn : active_sse_connections_) {
                conn->active = false;
                if (conn->conn) {
                    to_close.push_back(conn);
                }
            }
            active_sse_connections_.clear();
        }
        // Close sockets OUTSIDE connections_mutex_: on_error() -> on_disconnect
        // re-acquires it (std::shared_mutex is not recursive).
        for (auto& sc : to_close) {
            if (sc->conn->write_queue_) {
                sc->conn->write_queue_->shutdown();
            }
            asio::error_code ignore;
            sc->conn->socket.close(ignore);
            sc->conn->on_error();
        }
    }

    // Reap all Streamable HTTP sessions: close their SSE sockets so idle read
    // loops release the connection slots, then clear the map.
    {
        std::vector<std::shared_ptr<SseConnection>> to_close;
        {
            std::unique_lock<std::shared_mutex> lock(sessions_mutex_);
            for (auto& kv : sessions_) {
                for (auto& sc : kv.second->sse_connections) {
                    sc->active = false;
                    if (sc->conn) {
                        to_close.push_back(sc);
                    }
                }
                kv.second->sse_connections.clear();
            }
            sessions_.clear();
        }
        // Same recursion hazard as above: close outside the lock.
        for (auto& sc : to_close) {
            if (sc->conn->write_queue_) {
                sc->conn->write_queue_->shutdown();
            }
            asio::error_code ignore;
            sc->conn->socket.close(ignore);
            sc->conn->on_error();
        }
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

            // Connection cap: reply 503 then close, so the client can
            // distinguish throttling from a network failure.
            if (config_.max_connections > 0 &&
                active_connection_count_.load(std::memory_order_relaxed) >= config_.max_connections) {
                std::string resp = build_http_response(503, "text/plain", "Service Unavailable: too many connections");
                asio::async_write(socket, asio::buffer(resp),
                    [s = std::move(socket)](const asio::error_code&, std::size_t) mutable {
                        asio::error_code ignore;
                        s.close(ignore);
                    });
                if (running_) {
                    do_accept();
                }
                return;
            }

            asio::error_code no_delay_ec;
            socket.set_option(asio::ip::tcp::no_delay(true), no_delay_ec);
            auto conn = std::make_shared<HttpConnection>(std::move(socket));
            conn->max_body_size = config_.max_body_size;
            conn->max_header_count = config_.max_header_count;
            conn->max_header_line_size = config_.max_header_line_size;
            conn->read_timeout_ = config_.read_timeout;
            conn->init_write_queue(config_.write_queue_max_bytes, config_.write_queue_overflow);
            // Count decrement lives on on_release so SSE handlers (which replace
            // on_disconnect) can never leak the connection cap.
            conn->on_release = [this]() {
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

    // Host header validation (DNS-rebinding guard). Applied before anything else.
    if (config_.enforce_host_header) {
        std::string host_header;
        auto hit = conn->headers.find("host");
        if (hit != conn->headers.end()) host_header = hit->second;
        if (!is_host_allowed(host_header, config_.host, config_.port)) {
            conn->enqueue_write(build_http_response(403, "text/plain", "Forbidden: Host not allowed"));
            conn->active = false;
            return;
        }
    }

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
        std::string path_only = url;
        auto q = path_only.find('?');
        if (q != std::string::npos) {
            path_only.resize(q);  // tolerate query strings on the MCP path
        }
        if (path_only == config_.path) {
            if (method == "POST") { handle_post(conn); return; }
            if (method == "GET")  { handle_get_sse(conn); return; }
            if (method == "DELETE") { handle_delete(conn); return; }
        }
    } else {
        std::string path_only = url;
        auto q = path_only.find('?');
        if (q != std::string::npos) {
            path_only.resize(q);
        }
        if (path_only == config_.sse_path && method == "GET") { handle_sse_connect(conn); return; }
        if (path_only == config_.message_path && method == "POST") { handle_sse_message(conn); return; }
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
    // Streamable HTTP: POST must accept both JSON and SSE responses.
    std::string accept;
    {
        auto it = conn->headers.find("accept");
        if (it != conn->headers.end()) accept = it->second;
    }
    if (accept.find("application/json") == std::string::npos &&
        accept.find("text/event-stream") == std::string::npos && !accept.empty()) {
        conn->enqueue_write(build_http_response(406, "text/plain",
                                                "Accept header must include application/json or text/event-stream"));
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
        AsyncLogger::instance().log(std::string("JSON parse error: ") + e.what());
        auto error_resp = make_error_response_null_id(Protocol::PARSE_ERROR, "Parse error");
        conn->enqueue_write(build_http_response(400, "application/json", error_resp.dump()));
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
    bool inserted_uninitialized = false;  // rolled back if the initialize handler throws

    if (is_initialize) {
        if (!req_session_id.empty()) {
            conn->enqueue_write(build_http_response(400, "text/plain", "Session already exists"));
            return;
        }
        // create_session() enforces max_sessions and registers an uninitialized
        // session up front, so server-initiated emissions during initialize
        // (progress via the sink's second-emission path) can already be routed.
        session = create_session();
        if (!session) {
            conn->enqueue_write(build_http_response(429, "text/plain", "Too many sessions"));
            return;
        }
        response_session_id = session->session_id;
        inserted_uninitialized = true;
    } else {
        // Spec: a non-initialize request without mcp-session-id is a client
        // error (400); an unrecognized/terminated id is 404.
        if (req_session_id.empty()) {
            auto e = make_error_response_null_id(Protocol::INVALID_PARAMS,
                                                 "Missing Mcp-Session-Id header");
            conn->enqueue_write(build_http_response(400, "application/json", e.dump()));
            return;
        }
        session = find_session(req_session_id);
        if (!session) {
            conn->enqueue_write(build_http_response(404, "text/plain", "Session not found"));
            return;
        }
        if (!session->initialized.load()) {
            auto e = make_error_response_null_id(Protocol::SERVER_NOT_INITIALIZED,
                                                 "Session not initialized");
            conn->enqueue_write(build_http_response(400, "application/json", e.dump()));
            return;
        }
        response_session_id = session->session_id;
    }

    // Touch session activity for idle-TTL bookkeeping.
    {
        std::unique_lock<std::shared_mutex> lock(sessions_mutex_);
        session->last_activity = std::chrono::steady_clock::now();
    }

    if (!message_handler_) {
        if (inserted_uninitialized) {
            std::unique_lock<std::shared_mutex> lock(sessions_mutex_);
            sessions_.erase(session->session_id);
        }
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
    auto first_body = std::make_shared<std::optional<nlohmann::json>>();
    auto self = shared_from_this();
    auto conn_ref = conn;
    auto sid = response_session_id;
    ITransport::ResponseSink sink = [self, conn_ref, sid, frame_written, first_body](const nlohmann::json& resp) {
        bool expected = false;
        if (frame_written->compare_exchange_strong(expected, true)) {
            *first_body = resp;
            std::vector<std::pair<std::string, std::string>> h;
            if (!sid.empty()) h.emplace_back("mcp-session-id", sid);
            conn_ref->enqueue_write(self->build_http_response(200, "application/json", resp.dump(), h));
        } else {
            self->send_to_session(sid, resp);
        }
    };

    bool handler_ok = true;
    try {
        message_handler_(json_msg, sink, response_session_id);
    } catch (...) {
        handler_ok = false;
        // Never propagate out of the io loop.
    }

    // Commit a new session only after a successful initialize. initialize is
    // never deferred, so the synchronous handler above has already run. A
    // JSON-RPC error body (e.g. user initialize handler rejected the client)
    // counts as failure: the session must not survive.
    if (is_initialize) {
        bool init_ok = handler_ok && frame_written->load() && first_body->has_value() &&
                       !(*first_body)->contains("error");
        if (init_ok) {
            session->initialized.store(true);
        } else {
            // Roll back the half-registered session so it doesn't leak.
            std::unique_lock<std::shared_mutex> lock(sessions_mutex_);
            sessions_.erase(session->session_id);
        }
    }

    // Notifications get a 202 (no body). Request responses are written by the
    // sink (synchronously now, or when a deferred handler completes later).
    if (handler_ok && std::get_if<JsonRpcNotification>(&parsed)) {
        std::vector<std::pair<std::string, std::string>> h;
        if (!response_session_id.empty()) h.emplace_back("mcp-session-id", response_session_id);
        conn->enqueue_write(build_http_response(202, "text/plain", "", h));
    }
}

// --- Streamable HTTP: batch (JSON-RPC array) ---
// Elements are dispatched synchronously (deferral disabled via
// McpServer::BatchDispatchGuard) so every response is collected into the
// single array body, even with a worker pool configured.
void HttpTransport::handle_post_batch(std::shared_ptr<HttpConnection> conn,
                                      const nlohmann::json& batch,
                                      const std::string& req_session_id) {
    if (batch.empty()) {
        auto e = make_error_response_null_id(Protocol::INVALID_REQUEST, "Invalid Request");
        conn->enqueue_write(build_http_response(200, "application/json", e.dump()));
        return;
    }
    if (req_session_id.empty()) {
        auto e = make_error_response_null_id(Protocol::INVALID_PARAMS, "Missing Mcp-Session-Id header");
        conn->enqueue_write(build_http_response(400, "application/json", e.dump()));
        return;
    }
    auto session = find_session(req_session_id);
    if (!session) {
        conn->enqueue_write(build_http_response(404, "text/plain", "Session not found"));
        return;
    }
    if (!session->initialized.load()) {
        auto e = make_error_response_null_id(Protocol::SERVER_NOT_INITIALIZED, "Session not initialized");
        conn->enqueue_write(build_http_response(400, "application/json", e.dump()));
        return;
    }
    const std::string& sid = session->session_id;

    std::vector<nlohmann::json> responses;
    {
        McpServer::BatchDispatchGuard guard;
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
    resp_headers += "mcp-session-id: " + sanitize_header_value(session->session_id) + "\r\n";
    // Origin was already validated in handle_request; reflect it (sanitized) for the browser.
    std::string origin;
    {
        auto it = conn->headers.find("origin");
        if (it != conn->headers.end()) origin = it->second;
    }
    if (!origin.empty()) {
        resp_headers += "Access-Control-Allow-Origin: " + sanitize_header_value(origin) + "\r\n";
    }
    resp_headers += "\r\n";

    // Promote to a long-lived SSE stream: cancel the request-read timeout so it
    // is not killed while idle, and mark it so start_read_with_dispatch stops arming it.
    conn->is_streaming = true;
    asio::error_code timer_ec;
    conn->read_timer_.cancel(timer_ec);

    conn->enqueue_write(resp_headers + ": connected\n\n");

    {
        std::unique_lock<std::shared_mutex> lock(sessions_mutex_);
        session->sse_connections.push_back(sse_conn);
        session->last_activity = std::chrono::steady_clock::now();
    }
}

// --- Streamable HTTP: DELETE ---
void HttpTransport::handle_delete(std::shared_ptr<HttpConnection> conn) {
    std::string req_session_id;
    {
        auto it = conn->headers.find("mcp-session-id");
        if (it != conn->headers.end()) req_session_id = it->second;
    }

    std::vector<std::shared_ptr<SseConnection>> to_close;
    {
        std::unique_lock<std::shared_mutex> lock(sessions_mutex_);
        auto it = sessions_.find(req_session_id);
        if (req_session_id.empty() || it == sessions_.end()) {
            conn->enqueue_write(build_http_response(404, "text/plain", "Session not found"));
            return;
        }
        for (auto& sc : it->second->sse_connections) {
            sc->active = false;
            if (sc->conn) {
                to_close.push_back(sc);
            }
        }
        it->second->sse_connections.clear();
        sessions_.erase(it);  // session is re-creatable by a later initialize
    }
    // Close sockets OUTSIDE sessions_mutex_: on_error() -> on_disconnect
    // re-acquires it (std::shared_mutex is not recursive).
    for (auto& sc : to_close) {
        if (sc->conn->write_queue_) {
            sc->conn->write_queue_->shutdown();
        }
        asio::error_code ignore;
        sc->conn->socket.close(ignore);  // drop the SSE stream too
        sc->conn->on_error();            // release the connection slot
    }

    conn->enqueue_write(build_http_response(200, "text/plain", ""));
}

// --- Legacy SSE: connect ---
void HttpTransport::handle_sse_connect(std::shared_ptr<HttpConnection> conn) {
    auto sse_conn = std::make_shared<SseConnection>();
    sse_conn->conn = conn;
    conn->on_disconnect = [this, sse_conn]() {
        unregister_sse_connection(sse_conn);
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
// DEPRECATED: legacy SSE mode (GET /sse + POST /messages) broadcasts every
// response to ALL connected SSE clients. If more than one client is attached,
// one client's tool-call response is visible to the others. Prefer
// StreamableHttp mode, which routes responses per-session. Kept for backward
// compatibility with existing single-client deployments.
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
        AsyncLogger::instance().log(std::string("JSON parse error: ") + e.what());
        conn->enqueue_write(build_http_response(400, "text/plain", "Invalid JSON"));
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
    std::string frame = "data: " + message.dump() + "\n\n";  // frame once
    std::shared_lock<std::shared_mutex> lock(connections_mutex_);
    for (auto& conn : active_sse_connections_) {
        if (conn->active.load()) {
            conn->push(frame);
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

std::shared_ptr<HttpSession> HttpTransport::create_session() {
    if (config_.max_sessions > 0) {
        std::shared_lock<std::shared_mutex> lock(sessions_mutex_);
        if (sessions_.size() >= config_.max_sessions) {
            return nullptr;  // cap reached
        }
    }
    auto session = std::make_shared<HttpSession>();
    session->session_id = generate_session_id();
    session->initialized.store(false);
    session->last_activity = std::chrono::steady_clock::now();
    std::unique_lock<std::shared_mutex> lock(sessions_mutex_);
    // Re-check under exclusive lock to avoid racing another creator past the cap.
    if (config_.max_sessions > 0 && sessions_.size() >= config_.max_sessions) {
        return nullptr;
    }
    sessions_[session->session_id] = session;
    return session;
}

void HttpTransport::arm_session_sweep() {
    if (config_.session_idle_timeout <= std::chrono::milliseconds(0) || !io_ctx_) {
        return;
    }
    if (!session_sweep_timer_) {
        session_sweep_timer_ = std::make_unique<asio::steady_timer>(*io_ctx_);
    }
    // Sweep at the timeout granularity (clamped to a sane interval).
    auto period = config_.session_idle_timeout;
    if (period < std::chrono::seconds(1)) {
        period = std::chrono::seconds(1);
    }
    session_sweep_timer_->expires_after(period);
    auto self = shared_from_this();
    session_sweep_timer_->async_wait([self](const asio::error_code& ec) {
        self->sweep_sessions(ec);
    });
}

void HttpTransport::sweep_sessions(const asio::error_code& ec) {
    if (ec || !running_.load()) {
        return;  // timer cancelled / transport stopping
    }
    auto now = std::chrono::steady_clock::now();
    std::vector<std::shared_ptr<SseConnection>> to_close;
    {
        std::unique_lock<std::shared_mutex> lock(sessions_mutex_);
        for (auto it = sessions_.begin(); it != sessions_.end();) {
            auto idle = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - it->second->last_activity);
            if (idle > config_.session_idle_timeout) {
                for (auto& sc : it->second->sse_connections) {
                    sc->active = false;
                    if (sc->conn) {
                        to_close.push_back(sc);
                    }
                }
                it->second->sse_connections.clear();
                it = sessions_.erase(it);
            } else {
                ++it;
            }
        }
    }
    // Close outside the lock: on_error() -> on_disconnect re-acquires it.
    for (auto& sc : to_close) {
        if (sc->conn->write_queue_) {
            sc->conn->write_queue_->shutdown();
        }
        asio::error_code ignore;
        sc->conn->socket.close(ignore);
        sc->conn->on_error();
    }
    arm_session_sweep();  // re-arm
}

void HttpTransport::push_to_session_sse(const std::string& id, const nlohmann::json& message) {
    std::string frame = "data: " + message.dump() + "\n\n";  // frame once
    std::shared_lock<std::shared_mutex> lock(sessions_mutex_);
    auto it = sessions_.find(id);
    if (it == sessions_.end()) {
        return;
    }
    for (auto& sc : it->second->sse_connections) {
        if (sc->active.load()) {
            sc->push(frame);
        }
    }
}

void HttpTransport::push_to_all_sessions(const nlohmann::json& message) {
    std::string frame = "data: " + message.dump() + "\n\n";  // frame once
    std::shared_lock<std::shared_mutex> lock(sessions_mutex_);
    for (auto& kv : sessions_) {
        for (auto& sc : kv.second->sse_connections) {
            if (sc->active.load()) {
                sc->push(frame);
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