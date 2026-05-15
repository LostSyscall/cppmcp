#pragma once

#include <cstdint>
#include <string>
#include <variant>

#include <nlohmann/json.hpp>

namespace cppmcp {

// NullId represents a JSON-RPC null id (for parse/invalid request errors)
struct NullId {};

using RequestId = std::variant<NullId, int64_t, std::string>;

// JSON serialization for RequestId
inline void to_json(nlohmann::json& j, const RequestId& id) {
    if (const auto* null_id = std::get_if<NullId>(&id)) {
        j = nullptr;
    } else if (const auto* int_id = std::get_if<int64_t>(&id)) {
        j = *int_id;
    } else {
        j = std::get<std::string>(id);
    }
}

inline void from_json(const nlohmann::json& j, RequestId& id) {
    if (j.is_null()) {
        id = NullId{};
    } else if (j.is_number_integer()) {
        id = j.get<int64_t>();
    } else if (j.is_string()) {
        id = j.get<std::string>();
    } else {
        throw std::invalid_argument("RequestId must be null, integer, or string");
    }
}

// Shared helper: convert RequestId to nlohmann::json
inline nlohmann::json request_id_to_json(const RequestId& id) {
    nlohmann::json j;
    to_json(j, id);
    return j;
}

// Check if RequestId is null (for error responses)
inline bool is_null_id(const RequestId& id) {
    return std::holds_alternative<NullId>(id);
}

} // namespace cppmcp