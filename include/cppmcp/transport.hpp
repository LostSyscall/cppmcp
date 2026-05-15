#pragma once

#include <functional>
#include <string>

#include <nlohmann/json.hpp>

namespace cppmcp {

class ITransport {
public:
    virtual ~ITransport() = default;

    virtual void start() = 0;
    virtual void stop() = 0;
    virtual bool is_running() const = 0;

    virtual void send_message(const nlohmann::json& message) = 0;

    using MessageCallback = std::function<void(const nlohmann::json& message)>;
    virtual void set_message_handler(MessageCallback handler) = 0;

    using ErrorCallback = std::function<void(const std::string& error)>;
    virtual void set_error_handler(ErrorCallback handler) = 0;

    // Optional: allows transports to intercept response routing.
    // When set, the server calls this instead of send_message for the direct response
    // to a request. This enables HTTP transports to return the response in the HTTP body
    // rather than pushing it through SSE.
    using ResponseSender = std::function<void(const nlohmann::json& response)>;
    virtual void set_response_sender(ResponseSender sender) {}
};

} // namespace cppmcp