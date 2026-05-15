#include "cppmcp/http_transport.hpp"
#include "cppmcp/jsonrpc.hpp"
#include "cppmcp/protocol.hpp"
#include "cppmcp/common.hpp"

#include <httplib.h>

#include <iostream>
#include <random>

namespace cppmcp {

// thread_local: each httplib request thread gets its own response sender,
// eliminating the data race on a shared member variable for concurrent requests.
static thread_local ITransport::ResponseSender tl_response_sender;

HttpTransport::HttpTransport(const HttpTransportConfig& config)
    : config_(config) {}

HttpTransport::~HttpTransport() {
    stop();
}

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

void HttpTransport::start() {
    http_server_ = std::make_unique<httplib::Server>();

    // Set body size limit
    http_server_->set_payload_max_length(config_.max_body_size);

    // CORS protection — restrict to localhost origins
    http_server_->set_pre_routing_handler([](const httplib::Request& req, httplib::Response& res) {
        std::string origin = req.get_header_value("Origin");
        if (!origin.empty()) {
            bool allowed = origin.find("127.0.0.1") != std::string::npos
                        || origin.find("localhost") != std::string::npos;
            if (allowed) {
                res.set_header("Access-Control-Allow-Origin", origin);
            } else {
                res.status = 403;
                res.set_content("Forbidden: Origin not allowed", "text/plain");
                return httplib::Server::HandlerResponse::Handled;
            }
        }
        return httplib::Server::HandlerResponse::Unhandled;
    });

    // Handle CORS preflight
    http_server_->Options(".*", [](const httplib::Request& req, httplib::Response& res) {
        res.set_header("Access-Control-Allow-Methods", "GET, POST, DELETE, OPTIONS");
        res.set_header("Access-Control-Allow-Headers", "Content-Type, mcp-session-id");
        res.set_header("Access-Control-Max-Age", "86400");
        res.status = 204;
    });

    if (config_.mode == HttpTransportMode::StreamableHttp) {
        http_server_->Post(config_.path, [this](const httplib::Request& req, httplib::Response& res) {
            handle_post(req, res);
        });
        http_server_->Get(config_.path, [this](const httplib::Request& req, httplib::Response& res) {
            handle_get_sse(req, res);
        });
        http_server_->Delete(config_.path, [this](const httplib::Request& req, httplib::Response& res) {
            handle_delete(req, res);
        });
    } else {
        http_server_->Get(config_.sse_path, [this](const httplib::Request& req, httplib::Response& res) {
            handle_sse_connect(req, res);
        });
        http_server_->Post(config_.message_path, [this](const httplib::Request& req, httplib::Response& res) {
            handle_sse_message(req, res);
        });
    }

    running_ = true;

    server_thread_ = std::thread([this]() {
        if (!http_server_->listen(config_.host, config_.port)) {
            if (error_handler_) error_handler_("Failed to start HTTP server");
            running_ = false;
        }
    });
}

void HttpTransport::stop() {
    running_ = false;
    // Signal all SSE connections to close
    {
        std::lock_guard<std::mutex> lock(connections_mutex_);
        for (auto& conn : active_connections_) {
            conn->active = false;
            conn->queue_cv.notify_one();
        }
    }
    if (http_server_) {
        http_server_->stop();
    }
    if (server_thread_.joinable()) {
        server_thread_.join();
    }
    http_server_.reset();
}

bool HttpTransport::is_running() const {
    return running_;
}

// --- Streamable HTTP handlers ---
void HttpTransport::handle_post(const httplib::Request& req, httplib::Response& res) {
    // Validate content type
    std::string content_type = req.get_header_value("Content-Type");
    if (content_type.find("application/json") == std::string::npos) {
        res.status = 400;
        res.set_content("Content-Type must be application/json", "text/plain");
        return;
    }

    try {
        auto json_msg = nlohmann::json::parse(req.body);

        // Session management
        std::string req_session_id = req.get_header_value("mcp-session-id");
        if (!session_initialized_.load(std::memory_order_acquire)) {
            std::lock_guard<std::mutex> lock(session_mutex_);
            if (!session_initialized_.load(std::memory_order_relaxed)) {
                current_session_id_ = generate_session_id();
                session_initialized_.store(true, std::memory_order_release);
            }
        } else if (!req_session_id.empty() && req_session_id != current_session_id_) {
            res.status = 404;
            res.set_content("Session not found", "text/plain");
            return;
        }

        auto parsed = parse_message(json_msg);

        if (auto* err_resp = std::get_if<JsonRpcErrorResponse>(&parsed)) {
            nlohmann::json j;
            j["jsonrpc"] = "2.0";
            j["id"] = request_id_to_json(err_resp->id);
            nlohmann::json error_obj;
            error_obj["code"] = err_resp->error.code;
            error_obj["message"] = err_resp->error.message;
            if (err_resp->error.data) error_obj["data"] = *err_resp->error.data;
            j["error"] = error_obj;
            res.set_content(j.dump(), "application/json");
            if (session_initialized_.load()) {
                res.set_header("mcp-session-id", current_session_id_);
            }
            return;
        }

        if (auto* notif = std::get_if<JsonRpcNotification>(&parsed)) {
            // Notifications don't need a direct HTTP response
            if (message_handler_) message_handler_(json_msg);
            res.status = 202;
            res.set_content("", "text/plain");
            if (session_initialized_.load()) {
                res.set_header("mcp-session-id", current_session_id_);
            }
            return;
        }

        // It's a request — capture the response via ResponseSender
        if (message_handler_) {
            nlohmann::json response_json;
            bool response_captured = false;

            // Set a thread-local ResponseSender that captures the response for HTTP body
            tl_response_sender = [&](const nlohmann::json& resp) {
                if (!response_captured) {
                    response_json = resp;
                    response_captured = true;
                } else {
                    // Additional messages (notifications) go to SSE connections
                    push_to_sse_connections(resp);
                }
            };

            message_handler_(json_msg);

            // Clear the response sender
            tl_response_sender = nullptr;

            if (response_captured) {
                res.set_content(response_json.dump(), "application/json");
            } else {
                res.status = 500;
                res.set_content("No response generated", "text/plain");
            }

            if (session_initialized_.load()) {
                res.set_header("mcp-session-id", current_session_id_);
            }
        }
    } catch (const nlohmann::json::parse_error& e) {
        auto error_resp = make_error_response_null_id(Protocol::PARSE_ERROR, e.what());
        res.set_content(error_resp.dump(), "application/json");
    }
}

void HttpTransport::handle_get_sse(const httplib::Request& req, httplib::Response& res) {
    if (!session_initialized_.load()) {
        res.status = 405;
        res.set_content("Method Not Allowed - session not initialized", "text/plain");
        return;
    }

    std::string req_session_id = req.get_header_value("mcp-session-id");
    if (!req_session_id.empty() && req_session_id != current_session_id_) {
        res.status = 404;
        res.set_content("Session not found", "text/plain");
        return;
    }

    auto conn = std::make_shared<SseConnection>();

    res.set_chunked_content_provider(
        "text/event-stream",
        [this, conn](size_t offset, httplib::DataSink& sink) -> bool {
            // Register this connection
            {
                std::lock_guard<std::mutex> lock(connections_mutex_);
                active_connections_.push_back(conn);
            }

            // Send initial comment
            std::string comment = ": connected\n\n";
            sink.write(comment.c_str(), comment.size());

            // Drain messages from queue using condition variable
            while (running_ && conn->active.load()) {
                std::string data;
                if (conn->wait_and_pop(data, 500)) {
                    std::string event = "data: " + data + "\n\n";
                    sink.write(event.c_str(), event.size());
                }
                // Also drain any accumulated messages
                std::vector<std::string> batch;
                conn->drain(batch);
                for (const auto& d : batch) {
                    std::string event = "data: " + d + "\n\n";
                    sink.write(event.c_str(), event.size());
                }
            }

            // Remove connection
            {
                std::lock_guard<std::mutex> lock(connections_mutex_);
                active_connections_.erase(
                    std::remove(active_connections_.begin(), active_connections_.end(), conn),
                    active_connections_.end());
            }

            return true;
        },
        [conn](bool success) {
            conn->active = false;
            conn->queue_cv.notify_one();
        }
    );

    if (session_initialized_.load()) {
        res.set_header("mcp-session-id", current_session_id_);
    }
}

void HttpTransport::handle_delete(const httplib::Request& req, httplib::Response& res) {
    std::string req_session_id = req.get_header_value("mcp-session-id");
    if (!req_session_id.empty() && req_session_id != current_session_id_) {
        res.status = 404;
        res.set_content("Session not found", "text/plain");
        return;
    }

    {
        std::lock_guard<std::mutex> lock(connections_mutex_);
        for (auto& conn : active_connections_) {
            conn->active = false;
            conn->queue_cv.notify_one();
        }
        active_connections_.clear();
    }

    {
        std::lock_guard<std::mutex> lock(session_mutex_);
        session_initialized_.store(false);
        current_session_id_.clear();
    }

    res.status = 200;
    res.set_content("", "text/plain");
}

// --- Legacy SSE handlers ---
void HttpTransport::handle_sse_connect(const httplib::Request& req, httplib::Response& res) {
    auto conn = std::make_shared<SseConnection>();

    res.set_chunked_content_provider(
        "text/event-stream",
        [this, conn](size_t offset, httplib::DataSink& sink) -> bool {
            {
                std::lock_guard<std::mutex> lock(connections_mutex_);
                active_connections_.push_back(conn);
            }

            // Send endpoint event
            std::string endpoint_data = config_.message_path;
            std::string endpoint_event = "event: endpoint\ndata: " + endpoint_data + "\n\n";
            sink.write(endpoint_event.c_str(), endpoint_event.size());

            // Drain messages from queue
            while (running_ && conn->active.load()) {
                std::string data;
                if (conn->wait_and_pop(data, 500)) {
                    std::string event = "data: " + data + "\n\n";
                    sink.write(event.c_str(), event.size());
                }
                std::vector<std::string> batch;
                conn->drain(batch);
                for (const auto& d : batch) {
                    std::string event = "data: " + d + "\n\n";
                    sink.write(event.c_str(), event.size());
                }
            }

            {
                std::lock_guard<std::mutex> lock(connections_mutex_);
                active_connections_.erase(
                    std::remove(active_connections_.begin(), active_connections_.end(), conn),
                    active_connections_.end());
            }

            return true;
        },
        [conn](bool success) {
            conn->active = false;
            conn->queue_cv.notify_one();
        }
    );
}

void HttpTransport::handle_sse_message(const httplib::Request& req, httplib::Response& res) {
    try {
        auto json_msg = nlohmann::json::parse(req.body);
        if (message_handler_) {
            message_handler_(json_msg);
        }
        res.status = 202;
        res.set_content("", "text/plain");
    } catch (const nlohmann::json::parse_error& e) {
        res.status = 400;
        res.set_content("Invalid JSON: " + std::string(e.what()), "text/plain");
    }
}

void HttpTransport::send_message(const nlohmann::json& message) {
    // If a thread-local response_sender is active (for Streamable HTTP POST handling),
    // route through that instead
    if (tl_response_sender) {
        tl_response_sender(message);
        return;
    }

    // Otherwise push to all SSE connections via queue
    std::lock_guard<std::mutex> lock(connections_mutex_);
    std::string data = message.dump();
    for (auto& conn : active_connections_) {
        if (conn->active.load()) {
            conn->push(data);
        }
    }
}

void HttpTransport::push_to_sse_connections(const nlohmann::json& message) {
    std::lock_guard<std::mutex> lock(connections_mutex_);
    std::string data = message.dump();
    for (auto& conn : active_connections_) {
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