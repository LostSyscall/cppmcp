#pragma once

#include <initializer_list>
#include <string>

#include "error_codes.hpp"

namespace cppmcp::Protocol {

// Protocol revisions the library understands, newest first.
constexpr const char* LATEST_PROTOCOL_VERSION = "2025-06-18";
constexpr const char* VERSION_2025_06_18 = "2025-06-18";
constexpr const char* VERSION_2025_03_26 = "2025-03-26";
// Best version offered when the client requests something unsupported.
constexpr const char* DEFAULT_NEGOTIATED_VERSION = VERSION_2025_06_18;

inline bool is_supported_version(const std::string& v) {
    return v == VERSION_2025_06_18 || v == VERSION_2025_03_26;
}

// Request method names
constexpr const char* METHOD_INITIALIZE = "initialize";
constexpr const char* METHOD_PING = "ping";
constexpr const char* METHOD_TOOLS_LIST = "tools/list";
constexpr const char* METHOD_TOOLS_CALL = "tools/call";
constexpr const char* METHOD_RESOURCES_LIST = "resources/list";
constexpr const char* METHOD_RESOURCES_READ = "resources/read";
constexpr const char* METHOD_RESOURCES_SUBSCRIBE = "resources/subscribe";
constexpr const char* METHOD_RESOURCES_UNSUBSCRIBE = "resources/unsubscribe";
constexpr const char* METHOD_RESOURCES_TEMPLATE_LIST = "resources/templates/list";
constexpr const char* METHOD_PROMPTS_LIST = "prompts/list";
constexpr const char* METHOD_PROMPTS_GET = "prompts/get";
constexpr const char* METHOD_COMPLETION_COMPLETE = "completion/complete";
constexpr const char* METHOD_LOGGING_SET_LEVEL = "logging/setLevel";

// Server -> client request method names (client handles inbound, replies)
constexpr const char* METHOD_SAMPLING_CREATE = "sampling/createMessage";
constexpr const char* METHOD_ELICITATION_CREATE = "elicitation/create";
constexpr const char* METHOD_ROOTS_LIST = "roots/list";

// Notification method names
constexpr const char* NOTIF_INITIALIZED = "notifications/initialized";
constexpr const char* NOTIF_PROGRESS = "notifications/progress";
constexpr const char* NOTIF_MESSAGE = "notifications/message";
constexpr const char* NOTIF_TOOLS_LIST_CHANGED = "notifications/tools/list_changed";
constexpr const char* NOTIF_RESOURCES_LIST_CHANGED = "notifications/resources/list_changed";
constexpr const char* NOTIF_RESOURCES_UPDATED = "notifications/resources/updated";
constexpr const char* NOTIF_PROMPTS_LIST_CHANGED = "notifications/prompts/list_changed";
constexpr const char* NOTIF_CANCELLED = "notifications/cancelled";
constexpr const char* NOTIF_ROOTS_LIST_CHANGED = "notifications/roots/list_changed";

} // namespace cppmcp::Protocol
