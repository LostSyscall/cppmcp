#pragma once

namespace cppmcp::Protocol {

// JSON-RPC error codes
constexpr int PARSE_ERROR = -32700;
constexpr int INVALID_REQUEST = -32600;
constexpr int METHOD_NOT_FOUND = -32601;
constexpr int INVALID_PARAMS = -32602;
constexpr int INTERNAL_ERROR = -32603;

// MCP-specific error codes (outside the JSON-RPC reserved range)
constexpr int SERVER_NOT_INITIALIZED = -32002;

// Local client-side outcome codes (NOT standard JSON-RPC). McpClient uses these
// inside McpException/McpOutcome to signal local terminal states (timeout,
// cancel, transport disconnect, shutdown) so they are distinguishable from real
// JSON-RPC error codes returned by a server.
constexpr int REQUEST_CANCELLED = -32800;
constexpr int REQUEST_TIMED_OUT = -32801;
constexpr int CLIENT_NOT_CONNECTED = -32802;
constexpr int REQUEST_FAILED = -32803;

} // namespace cppmcp::Protocol