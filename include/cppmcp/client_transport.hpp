#pragma once

#include <asio.hpp>

#include <functional>
#include <string>

#include <nlohmann/json.hpp>

namespace cppmcp {

// Client-side transport interface. Mirrors ITransport but with reversed
// semantics: a client CONNECTS to a single server, SENDS requests/notifications,
// and RECEIVES responses + server-initiated pushes. There is no ResponseSink —
// the client correlates responses by request id at the McpClient layer, and any
// reply the client must produce (to a server-initiated request) is just another
// outbound message via send_message().
class IClientTransport {
public:
    virtual ~IClientTransport() = default;

    // Establish the byte stream (spawn child / TCP connect / pipe connect) and
    // arm the async read loop. Does NOT perform protocol handshake.
    virtual void connect() = 0;
    // Tear down the stream and cancel all in-flight async ops.
    virtual void disconnect() = 0;
    virtual bool is_connected() const = 0;

    // Serialize and send one JSON-RPC message. Thread-safe (queues internally).
    virtual void send_message(const nlohmann::json& message) = 0;

    // Delivers every parsed inbound message (response / notification /
    // server-initiated request). Invoked on the transport's io executor.
    using MessageCallback = std::function<void(const nlohmann::json& message)>;
    using ErrorCallback = std::function<void(const std::string& error)>;
    // Peer closed the stream (stdin EOF / TCP reset / pipe closed).
    using DisconnectCallback = std::function<void()>;

    virtual void set_message_handler(MessageCallback handler) = 0;
    virtual void set_error_handler(ErrorCallback handler) = 0;
    virtual void set_disconnect_handler(DisconnectCallback handler) {}
    virtual void set_io_context(asio::io_context* io_ctx) = 0;

    // Transport-managed session id (Streamable HTTP mcp-session-id). Empty for
    // stdio / local pipe. The HTTP transport captures it from the initialize
    // POST response and sends it on every subsequent request.
    virtual std::string session_id() const { return ""; }
};

} // namespace cppmcp
