#include "cppmcp/http_client_transport.hpp"

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
        session_id_ = cur_header_value_;
    }
    cur_header_field_.clear();
    cur_header_value_.clear();
}

void HttpClientTransport::connect() {
    if (connected_.exchange(true)) {
        return;
    }
    if (!io_ctx_) {
        connected_.store(false);
        throw std::runtime_error("HttpClientTransport: no io_context");
    }
    stopping_.store(false);
    init_parser();

    asio::ip::tcp::resolver resolver(*io_ctx_);
    auto results = resolver.resolve(host_, std::to_string(port_));
    socket_ = std::make_unique<asio::ip::tcp::socket>(*io_ctx_);
    asio::connect(*socket_, results);

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
    stopping_.store(true);
    connected_.store(false);
    if (write_queue_) {
        write_queue_->shutdown();
    }
    if (socket_) {
        asio::error_code ec;
        socket_->close(ec);
    }
    clear_handlers();
}

void HttpClientTransport::clear_handlers() {
    message_handler_ = {};
    error_handler_ = {};
    disconnect_handler_ = {};
}

std::string HttpClientTransport::build_post_request(const std::string& body) const {
    std::string req = "POST " + endpoint_ + " HTTP/1.1\r\n";
    req += "Host: " + host_ + ":" + std::to_string(port_) + "\r\n";
    req += "Content-Type: application/json\r\n";
    req += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    req += "Accept: application/json, text/event-stream\r\n";
    if (!session_id_.empty()) {
        req += "Mcp-Session-Id: " + session_id_ + "\r\n";
    }
    req += "Connection: keep-alive\r\n";
    req += "\r\n" + body;
    return req;
}

void HttpClientTransport::send_message(const nlohmann::json& message) {
    if (!write_queue_) {
        if (error_handler_) error_handler_("http transport not connected");
        return;
    }
    std::string body = message.dump();
    std::string req = build_post_request(body);
    write_queue_->enqueue(std::move(req));
}

void HttpClientTransport::start_read() {
    if (!socket_ || stopping_.load()) {
        return;
    }
    auto self = shared_from_this();
    socket_->async_read_some(read_buf_.prepare(8192),
        [self](const asio::error_code& ec, std::size_t bytes) { self->on_read(ec, bytes); });
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
        if (!response_body_.empty()) {
            try {
                auto j = nlohmann::json::parse(response_body_);
                if (message_handler_) {
                    message_handler_(j);
                }
            } catch (const std::exception& e) {
                if (error_handler_) {
                    error_handler_(std::string("http body parse error: ") + e.what());
                }
            }
        }
    } else if (error_handler_) {
        error_handler_("http server returned status " + std::to_string(response_status_));
    }
}

} // namespace cppmcp
