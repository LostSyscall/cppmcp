#include "cppmcp/jsonrpc.hpp"
#include "cppmcp/common.hpp"

namespace cppmcp {

namespace {

// Shared jsonrpc-2.0 version check. Returns true on a valid object with the
// right version; false otherwise (caller maps that to an INVALID_REQUEST error).
bool check_jsonrpc_envelope(const nlohmann::json& raw) {
    return raw.is_object() && raw.contains("jsonrpc") && raw["jsonrpc"] == "2.0";
}

// Parse the "id" member of a REQUEST. JSON-RPC 2.0 forbids null (and fractional)
// ids on requests; a null id means "respond with null id" for envelope errors.
// Returns nullptr-parity via ok=false on unusable ids.
bool request_id_from_json(const nlohmann::json& raw, RequestId& out) {
    try {
        from_json(raw, out);
    } catch (const std::exception&) {
        return false;  // fractional/other invalid types -> INVALID_REQUEST
    }
    return !std::holds_alternative<NullId>(out);
}

// Build a request from a raw object that has both "id" and "method". On any
// structural problem returns a JsonRpcErrorResponse (id captured when possible).
JsonRpcErrorResponse make_method_error(const nlohmann::json& /*raw*/, const RequestId& id, const char* why) {
    return JsonRpcErrorResponse{"2.0", id, JsonRpcErrorDetail{Protocol::INVALID_REQUEST, why}};
}

// Validate params: present + non-null must be object or array.
bool params_ok(const nlohmann::json& raw) {
    if (!raw.contains("params") || raw["params"].is_null()) return true;
    return raw["params"].is_object() || raw["params"].is_array();
}

}  // namespace

ParsedMessage parse_message(const nlohmann::json& raw) {
    try {
        if (!check_jsonrpc_envelope(raw)) {
            return JsonRpcErrorResponse{
                "2.0", NullId{},
                JsonRpcErrorDetail{Protocol::INVALID_REQUEST, "Invalid or missing jsonrpc version"}
            };
        }

        if (raw.contains("id")) {
            if (raw.contains("method")) {
                RequestId id;
                if (!request_id_from_json(raw["id"], id)) {
                    return make_method_error(raw, NullId{},
                                             "Request id must be a string or number (null is not allowed)");
                }
                if (!raw["method"].is_string()) {
                    return make_method_error(raw, id, "Method must be a string");
                }
                if (!params_ok(raw)) {
                    return make_method_error(raw, id, "Params must be a structured value (object or array)");
                }
                JsonRpcRequest req;
                req.jsonrpc = "2.0";
                req.id = id;
                req.method = raw["method"].get<std::string>();
                if (raw.contains("params") && !raw["params"].is_null()) {
                    req.params = raw["params"];
                }
                return req;
            }
            // Server view: a response (id, no method) is unexpected — unless it
            // answers a server-initiated request (sampling/roots/etc.). The
            // server layer re-routes via on_client_response before treating
            // this as an error, so preserve success/error detail here.
            RequestId id;
            from_json(raw["id"], id);
            if (raw.contains("result")) {
                JsonRpcSuccessResponse resp;
                resp.jsonrpc = "2.0";
                resp.id = id;
                resp.result = raw["result"];
                return resp;
            }
            return JsonRpcErrorResponse{
                "2.0", id,
                JsonRpcErrorDetail{Protocol::INVALID_REQUEST, "Server received a response message (unexpected)"}
            };
        }

        if (raw.contains("method")) {
            if (!raw["method"].is_string()) {
                return JsonRpcErrorResponse{
                    "2.0", NullId{},
                    JsonRpcErrorDetail{Protocol::INVALID_REQUEST, "Method must be a string"}
                };
            }
            if (!params_ok(raw)) {
                return JsonRpcErrorResponse{
                    "2.0", NullId{},
                    JsonRpcErrorDetail{Protocol::INVALID_REQUEST, "Params must be a structured value (object or array)"}
                };
            }
            JsonRpcNotification notif;
            notif.jsonrpc = "2.0";
            notif.method = raw["method"].get<std::string>();
            if (raw.contains("params") && !raw["params"].is_null()) {
                notif.params = raw["params"];
            }
            return notif;
        }

        return JsonRpcErrorResponse{
            "2.0", NullId{},
            JsonRpcErrorDetail{Protocol::INVALID_REQUEST, "Message must have method or be a valid request"}
        };
    } catch (const std::exception& e) {
        return JsonRpcErrorResponse{
            "2.0", NullId{},
            JsonRpcErrorDetail{Protocol::PARSE_ERROR, "Failed to parse message: " + std::string(e.what())}
        };
    }
}

ClientParsedMessage parse_message_client(const nlohmann::json& raw) {
    try {
        if (!check_jsonrpc_envelope(raw)) {
            return JsonRpcErrorResponse{
                "2.0", NullId{},
                JsonRpcErrorDetail{Protocol::INVALID_REQUEST, "Invalid or missing jsonrpc version"}
            };
        }

        if (raw.contains("id")) {
            if (raw.contains("method")) {
                // Inbound Request (server: client->server request; client: server->client).
                RequestId id;
                if (!request_id_from_json(raw["id"], id)) {
                    return make_method_error(raw, NullId{},
                                             "Request id must be a string or number (null is not allowed)");
                }
                if (!raw["method"].is_string()) {
                    return make_method_error(raw, id, "Method must be a string");
                }
                if (!params_ok(raw)) {
                    return make_method_error(raw, id, "Params must be a structured value (object or array)");
                }
                JsonRpcRequest req;
                req.jsonrpc = "2.0";
                req.id = id;
                req.method = raw["method"].get<std::string>();
                if (raw.contains("params") && !raw["params"].is_null()) {
                    req.params = raw["params"];
                }
                return req;
            }
            // A message with id but no method is a RESPONSE.
            RequestId id;
            from_json(raw["id"], id);
            if (raw.contains("result")) {
                JsonRpcSuccessResponse resp;
                resp.jsonrpc = "2.0";
                resp.id = id;
                resp.result = raw["result"];
                return resp;
            }
            if (raw.contains("error") && raw["error"].is_object()) {
                const auto& err = raw["error"];
                JsonRpcErrorDetail detail;
                detail.code = err.value("code", Protocol::INTERNAL_ERROR);
                detail.message = err.value("message", std::string{"Unknown error"});
                if (err.contains("data")) detail.data = err["data"];
                return JsonRpcErrorResponse{"2.0", id, std::move(detail)};
            }
            return JsonRpcErrorResponse{
                "2.0", id,
                JsonRpcErrorDetail{Protocol::INVALID_REQUEST,
                                   raw.contains("result") || raw.contains("error")
                                       ? "Response has neither result nor error"
                                       : "Server received a response message (unexpected)"}
            };
        }

        if (raw.contains("method")) {
            // Notification (no id).
            if (!raw["method"].is_string()) {
                return JsonRpcErrorResponse{
                    "2.0", NullId{},
                    JsonRpcErrorDetail{Protocol::INVALID_REQUEST, "Method must be a string"}
                };
            }
            if (!params_ok(raw)) {
                return JsonRpcErrorResponse{
                    "2.0", NullId{},
                    JsonRpcErrorDetail{Protocol::INVALID_REQUEST, "Params must be a structured value (object or array)"}
                };
            }
            JsonRpcNotification notif;
            notif.jsonrpc = "2.0";
            notif.method = raw["method"].get<std::string>();
            if (raw.contains("params") && !raw["params"].is_null()) {
                notif.params = raw["params"];
            }
            return notif;
        }

        return JsonRpcErrorResponse{
            "2.0", NullId{},
            JsonRpcErrorDetail{Protocol::INVALID_REQUEST, "Message must have method or be a valid request/response"}
        };
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