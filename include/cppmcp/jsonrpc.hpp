#pragma once

#include <optional>
#include <string>
#include <variant>

#include <nlohmann/json.hpp>

#include "common.hpp"
#include "exception.hpp"
#include "protocol.hpp"

namespace cppmcp {

struct JsonRpcErrorDetail {
    int code;
    std::string message;
    std::optional<nlohmann::json> data;
};

struct JsonRpcRequest {
    std::string jsonrpc = "2.0";
    RequestId id;
    std::string method;
    std::optional<nlohmann::json> params;
};

struct JsonRpcNotification {
    std::string jsonrpc = "2.0";
    std::string method;
    std::optional<nlohmann::json> params;
};

struct JsonRpcSuccessResponse {
    std::string jsonrpc = "2.0";
    RequestId id;
    nlohmann::json result;
};

struct JsonRpcErrorResponse {
    std::string jsonrpc = "2.0";
    RequestId id;
    JsonRpcErrorDetail error;
};

using JsonRpcResponse = std::variant<JsonRpcSuccessResponse, JsonRpcErrorResponse>;

using ParsedMessage = std::variant<JsonRpcRequest, JsonRpcNotification, JsonRpcErrorResponse>;

// Client-side parse result: unlike the server variant, a response (has id, no
// method) is classified as Success/Error rather than treated as an error.
using ClientParsedMessage = std::variant<JsonRpcRequest,
                                         JsonRpcNotification,
                                         JsonRpcSuccessResponse,
                                         JsonRpcErrorResponse>;

// Parse a raw JSON object into a classified JSON-RPC message (server view).
ParsedMessage parse_message(const nlohmann::json& raw);

// Parse a raw JSON object into a classified JSON-RPC message (client view).
ClientParsedMessage parse_message_client(const nlohmann::json& raw);

// Construct success response
nlohmann::json make_success_response(const RequestId& id, nlohmann::json result);

// Construct error response
nlohmann::json make_error_response(const RequestId& id, int code,
                                   const std::string& message,
                                   std::optional<nlohmann::json> data = std::nullopt);

// Construct error response with null id (for parse errors)
nlohmann::json make_error_response_null_id(int code, const std::string& message,
                                            std::optional<nlohmann::json> data = std::nullopt);

// Serialize any JSON-RPC message to JSON
nlohmann::json serialize_request(const JsonRpcRequest& req);
nlohmann::json serialize_notification(const JsonRpcNotification& notif);

} // namespace cppmcp