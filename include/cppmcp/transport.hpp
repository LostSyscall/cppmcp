#pragma once

#include <functional>
#include <string>

#include <asio.hpp>
#include <nlohmann/json.hpp>

namespace cppmcp {

class ITransport {
public:
    virtual ~ITransport() = default;

    virtual void start() = 0;
    virtual void stop() = 0;
    virtual bool is_running() const = 0;

    // Channel used to deliver the JSON-RPC RESPONSE for the request currently
    // being handled (e.g. into the Streamable HTTP POST body, or back to the
    // requesting pipe connection). Distinct from send_message() so that
    // notifications (progress/logging/list_changed) can never be mistaken for
    // a request response.
    using ResponseSink = std::function<void(const nlohmann::json& response)>;

    using MessageCallback = std::function<void(const nlohmann::json& message, ResponseSink respond, const std::string& session_id)>;

    using ErrorCallback = std::function<void(const std::string& error)>;

    // Invoked by a transport when the peer disconnects (e.g. stdin EOF) so the
    // server can shut down. Default is a no-op.
    using DisconnectCallback = std::function<void()>;

    // Deliver a server-initiated notification (no request id). Routes to the
    // appropriate channel (session SSE stream / all pipe connections / stdout).
    virtual void send_message(const nlohmann::json& message) = 0;

    // Deliver a server-initiated notification to a specific session (used by
    // async handlers to route progress/logging to the right client). Default
    // falls back to a broadcast via send_message.
    virtual void send_to_session(const std::string& session_id, const nlohmann::json& message) {
        (void)session_id;
        send_message(message);
    }

    virtual void set_message_handler(MessageCallback handler) = 0;
    virtual void set_error_handler(ErrorCallback handler) = 0;
    virtual void set_disconnect_handler(DisconnectCallback /*handler*/) {}
    virtual void set_io_context(asio::io_context* /*io_ctx*/) {}
};

} // namespace cppmcp
