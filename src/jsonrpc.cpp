#include "cppmcp/jsonrpc.hpp"
#include "cppmcp/common.hpp"

namespace cppmcp {

ParsedMessage parse_message(const nlohmann::json& raw) {
    try {
        // Validate jsonrpc version
        if (!raw.is_object() || !raw.contains("jsonrpc") || raw["jsonrpc"] != "2.0") {
            return JsonRpcErrorResponse{
                "2.0", NullId{},
                JsonRpcErrorDetail{Protocol::INVALID_REQUEST, "Invalid or missing jsonrpc version"}
            };
        }

        // Determine if this is a request (has id), notification (no id), or response
        if (raw.contains("id")) {
            if (raw.contains("method")) {
                // Validate method is a string
                if (!raw["method"].is_string()) {
                    RequestId id;
                    from_json(raw["id"], id);
                    return JsonRpcErrorResponse{
                        "2.0", id,
                        JsonRpcErrorDetail{Protocol::INVALID_REQUEST, "Method must be a string"}
                    };
                }

                JsonRpcRequest req;
                req.jsonrpc = "2.0";
                from_json(raw["id"], req.id);
                req.method = raw["method"].get<std::string>();
                if (raw.contains("params") && !raw["params"].is_null()) {
                    if (!raw["params"].is_object() && !raw["params"].is_array()) {
                        return JsonRpcErrorResponse{
                            "2.0", req.id,
                            JsonRpcErrorDetail{Protocol::INVALID_REQUEST, "Params must be a structured value (object or array)"}
                        };
                    }
                    req.params = raw["params"];
                }
                return req;
            }
            // Response — server shouldn't receive these
            RequestId id;
            from_json(raw["id"], id);
            return JsonRpcErrorResponse{
                "2.0", id,
                JsonRpcErrorDetail{Protocol::INVALID_REQUEST, "Server received a response message (unexpected)"}
            };
        } else if (raw.contains("method")) {
            if (!raw["method"].is_string()) {
                return JsonRpcErrorResponse{
                    "2.0", NullId{},
                    JsonRpcErrorDetail{Protocol::INVALID_REQUEST, "Method must be a string"}
                };
            }

            JsonRpcNotification notif;
            notif.jsonrpc = "2.0";
            notif.method = raw["method"].get<std::string>();
            if (raw.contains("params") && !raw["params"].is_null()) {
                if (!raw["params"].is_object() && !raw["params"].is_array()) {
                    return JsonRpcErrorResponse{
                        "2.0", NullId{},
                        JsonRpcErrorDetail{Protocol::INVALID_REQUEST, "Params must be a structured value (object or array)"}
                    };
                }
                notif.params = raw["params"];
            }
            return notif;
        } else {
            return JsonRpcErrorResponse{
                "2.0", NullId{},
                JsonRpcErrorDetail{Protocol::INVALID_REQUEST, "Message must have method or be a valid request"}
            };
        }
    } catch (const std::exception& e) {
        return JsonRpcErrorResponse{
            "2.0", NullId{},
            JsonRpcErrorDetail{Protocol::PARSE_ERROR, "Failed to parse message: " + std::string(e.what())}
        };
    }
}

nlohmann::json make_success_response(const RequestId& id, nlohmann::json result) {
    return nlohmann::json{
        {"jsonrpc", "2.0"},
        {"id", request_id_to_json(id)},
        {"result", std::move(result)}
    };
}

nlohmann::json make_error_response(const RequestId& id, int code,
                                   const std::string& message,
                                   std::optional<nlohmann::json> data) {
    nlohmann::json error_obj = nlohmann::json{{"code", code}, {"message", message}};
    if (data) error_obj["data"] = std::move(*data);
    return nlohmann::json{
        {"jsonrpc", "2.0"},
        {"id", request_id_to_json(id)},
        {"error", std::move(error_obj)}
    };
}

nlohmann::json make_error_response_null_id(int code, const std::string& message,
                                            std::optional<nlohmann::json> data) {
    nlohmann::json error_obj = nlohmann::json{{"code", code}, {"message", message}};
    if (data) error_obj["data"] = std::move(*data);
    return nlohmann::json{
        {"jsonrpc", "2.0"},
        {"id", nullptr},
        {"error", std::move(error_obj)}
    };
}

nlohmann::json serialize_request(const JsonRpcRequest& req) {
    nlohmann::json j = nlohmann::json{
        {"jsonrpc", "2.0"},
        {"id", request_id_to_json(req.id)},
        {"method", req.method}
    };
    if (req.params) j["params"] = std::move(*req.params);
    return j;
}

nlohmann::json serialize_notification(const JsonRpcNotification& notif) {
    nlohmann::json j = nlohmann::json{
        {"jsonrpc", "2.0"},
        {"method", notif.method}
    };
    if (notif.params) j["params"] = std::move(*notif.params);
    return j;
}

} // namespace cppmcp