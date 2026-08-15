#pragma once

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <shared_mutex>
#include <string>
#include <vector>

#include <asio.hpp>
#include <nlohmann/json.hpp>

#include "common.hpp"
#include "context.hpp"
#include "exception.hpp"
#include "jsonrpc.hpp"
#include "protocol.hpp"
#include "request_executor.hpp"
#include "transport.hpp"
#include "types.hpp"

namespace cppmcp {

// --- Handler types ---
using ToolHandler = std::function<CallToolResult(const nlohmann::json& arguments, RequestContext& ctx)>;
using ToolListHandler = std::function<std::vector<Tool>()>;

using ResourceHandler = std::function<ReadResourceResult(const std::string& uri, RequestContext& ctx)>;
using ResourceListHandler = std::function<std::vector<Resource>()>;
using ResourceTemplateListHandler = std::function<std::vector<ResourceTemplate>()>;
// Handles reads of any URI matching the registered RFC 6570-style template
// (simple {var} level expansion). Receives the concrete URI plus the extracted
// variable map.
using ResourceTemplateHandler = std::function<ReadResourceResult(
        const std::string& uri, const std::map<std::string, std::string>& variables, RequestContext& ctx)>;

using PromptHandler = std::function<GetPromptResult(const std::string& name,
                                                     const nlohmann::json& arguments,
                                                     RequestContext& ctx)>;
using PromptListHandler = std::function<std::vector<Prompt>()>;

using CompletionHandler = std::function<CompleteResult(const CompletionReference& ref,
                                                        const CompletionArgument& arg)>;

using InitializeHandler = std::function<InitializeResult(const nlohmann::json& params)>;
using LoggingLevelHandler = std::function<void(const std::string& level)>;
// Server-side log sink. If set, library diagnostics (startup, transport errors,
// notifications) are routed here instead of stderr. Falls back to AsyncLogger
// when unset. level is "info"/"warning"/"error".
using LogHandler = std::function<void(const std::string& level, const std::string& message)>;

// --- server -> client request callbacks (results arrive async) ---
using SamplingResultHandler = std::function<void(const CreateMessageResult& result)>;
using SamplingErrorHandler = std::function<void(const McpException& error)>;
using ElicitationResultHandler = std::function<void(const ElicitResult& result)>;
using ElicitationErrorHandler = std::function<void(const McpException& error)>;
using RootsResultHandler = std::function<void(const ListRootsResult& result)>;
using RootsErrorHandler = std::function<void(const McpException& error)>;

// Cached serialized results of the list handlers. Populated lazily after run()
// (frozen registries) and invalidated by invalidate_list_cache() / list_changed.
struct ListCache {
    std::optional<nlohmann::json> tools_list_result;
    std::optional<nlohmann::json> resources_list_result;
    std::optional<nlohmann::json> resource_templates_list_result;
    std::optional<nlohmann::json> prompts_list_result;
};

class McpServer {
public:
    McpServer(const Implementation& info, const ServerCapabilities& capabilities = {});
    ~McpServer();

    // --- Handler Registration ---
    void register_tool(const std::string& name, const Tool& tool_def, ToolHandler handler);
    void register_tool_list(ToolListHandler handler);

    void register_resource(const std::string& uri, const Resource& resource_def, ResourceHandler handler);
    void register_resource_list(ResourceListHandler handler);
    void register_resource_template_list(ResourceTemplateListHandler handler);
    // Register a URI template (RFC 6570 simple-string subset, e.g.
    // "weather://{city}/current") whose handler serves every matching read.
    // Matching templates are also advertised via resources/templates/list when
    // no explicit template-list handler is registered.
    void register_resource_template(const std::string& uri_template,
                                    const ResourceTemplate& template_def,
                                    ResourceTemplateHandler handler);

    void register_prompt(const std::string& name, const Prompt& prompt_def, PromptHandler handler);
    void register_prompt_list(PromptListHandler handler);

    void register_completion(CompletionHandler handler);
    void register_initialize_handler(InitializeHandler handler);
    void register_logging_level_handler(LoggingLevelHandler handler);

    // --- Transport Connection ---
    void connect(std::shared_ptr<ITransport> transport);

    // --- Server Lifecycle ---
    void run();
    void stop();
    bool is_running() const { return running_.load(); }

    // Configure a worker thread pool (call before run()). With N>0, slow
    // tools/call, resources/read and prompts/get handlers run on the pool so
    // they don't block the transport's io loop. 0 = synchronous (default).
    void set_worker_threads(std::size_t n);
    void set_io_threads(std::size_t n);
    // Route library diagnostics to a user sink instead of stderr.
    void set_log_handler(LogHandler handler);
    // Page size for list responses (0 = single unpaginated response, default).
    // When set, tools/resources/templates/prompts lists honor the client's
    // cursor param and emit nextCursor.
    void set_list_page_size(std::size_t n);

    // Invalidate cached list-handler results (call after mutating the
    // underlying tool/resource/prompt sets).
    void invalidate_list_cache();

    // --- Notification Sending ---
    void send_notification(const std::string& method, nlohmann::json params = {}, const std::string& session_id = "");
    void notify_tools_list_changed();
    void notify_resources_list_changed();
    // Resource-changed notification routed per-session: emitted to sessions that
    // subscribed to this URI (resources/subscribe), falling back to all sessions
    // when no session ever subscribed (single-client stdio/pipe semantics).
    void notify_resources_updated(const std::string& uri);
    void notify_prompts_list_changed();
    void notify_progress(const RequestId& progress_token, double progress,
                         std::optional<double> total = std::nullopt,
                         const std::string& session_id = "");
    void notify_logging(const std::string& level, const std::string& data,
                        std::optional<std::string> logger = std::nullopt,
                        const std::string& session_id = "");

    // --- server -> client requests (sampling / elicitation / roots) ---
    // Send sampling/createMessage to the client of `session_id` ("" = the only
    // connection, stdio/pipe). Returns false when the client did not declare
    // the sampling capability. The result/error callback runs on an io thread;
    // it must not block. Requests without a callback still await a response
    // (protocol requirement); the result is dropped.
    bool request_sampling(const CreateMessageRequestParams& params,
                          SamplingResultHandler on_result, SamplingErrorHandler on_error,
                          const std::string& session_id = "");
    bool request_elicitation(const ElicitRequestParams& params,
                             ElicitationResultHandler on_result, ElicitationErrorHandler on_error,
                             const std::string& session_id = "");
    bool request_roots(RootsResultHandler on_result, RootsErrorHandler on_error,
                       const std::string& session_id = "");
    // Tell the client our cached roots are stale (roots/list_changed).
    void notify_roots_list_changed(const std::string& session_id = "");

    const Implementation& server_info() const { return server_info_; }
    const ServerCapabilities& capabilities() const { return capabilities_; }
    // Protocol version negotiated with the current client ("" before initialize).
    std::string negotiated_version() const;

private:
    // Message processing
    void on_message(const nlohmann::json& message, ITransport::ResponseSink respond, const std::string& session_id);
    nlohmann::json process_request(const JsonRpcRequest& req, const std::string& session_id);
    void process_notification(const JsonRpcNotification& notif, const std::string& session_id);
    // Response to a server-initiated request arriving on any transport.
    void on_client_response(const JsonRpcSuccessResponse& resp);
    void on_client_error_response(const JsonRpcErrorResponse& resp);

    // --- internal helpers (consolidate duplicated patterns) ---
    // Pre-run guard shared by every register_*. Logs and returns false if running.
    bool ensure_pre_run(const char* action);
    // Unified list-request handler: cache check -> produce items -> cache fill.
    // produce_items builds the per-element JSON array (without the wrapper key);
    // this wraps it as {result_key: [...]} and applies the cache under one lock.
    nlohmann::json handle_list_request(
        const RequestId& id,
        std::optional<nlohmann::json> ListCache::*cache_field,
        const char* result_key,
        const nlohmann::json& params,
        std::function<std::vector<nlohmann::json>()> produce_items);
    // Diagnostic router: user sink if set, else AsyncLogger.
    void log_(const std::string& level, const std::string& message) const;
    // Track client capabilities/protocol version from an initialize request.
    void record_client_hello(const nlohmann::json& params, const std::string& session_id);

    // Method dispatch
    nlohmann::json dispatch_method(const std::string& method, const RequestId& id,
                                   const std::optional<nlohmann::json>& params,
                                   const std::string& session_id);

    // Specific method handlers
    nlohmann::json handle_initialize(const RequestId& id, const nlohmann::json& params, const std::string& session_id);
    nlohmann::json handle_ping(const RequestId& id);
    nlohmann::json handle_tools_list(const RequestId& id, const nlohmann::json& params);
    nlohmann::json handle_tools_call(const RequestId& id, const nlohmann::json& params, const std::string& session_id);
    nlohmann::json handle_resources_list(const RequestId& id, const nlohmann::json& params);
    nlohmann::json handle_resources_read(const RequestId& id, const nlohmann::json& params, const std::string& session_id);
    nlohmann::json handle_resources_subscribe(const RequestId& id, const nlohmann::json& params, const std::string& session_id);
    nlohmann::json handle_resources_unsubscribe(const RequestId& id, const nlohmann::json& params, const std::string& session_id);
    nlohmann::json handle_resources_templates_list(const RequestId& id, const nlohmann::json& params);
    nlohmann::json handle_prompts_list(const RequestId& id, const nlohmann::json& params);
    nlohmann::json handle_prompts_get(const RequestId& id, const nlohmann::json& params, const std::string& session_id);
    nlohmann::json handle_completion_complete(const RequestId& id, const nlohmann::json& params);
    nlohmann::json handle_logging_set_level(const RequestId& id, const nlohmann::json& params);

    // Outbound request plumbing
    bool send_server_request(const std::string& method, const nlohmann::json& params,
                             std::function<void(const nlohmann::json&)> on_result,
                             std::function<void(const McpException&)> on_error,
                             const std::string& session_id);
    void complete_server_request(int64_t id, bool ok, const nlohmann::json& payload);
    void fail_expired_server_requests();

public:
    // Scoped guard: while active, deferrable methods run synchronously so a
    // batch dispatcher can collect every response into one array body.
    struct BatchDispatchGuard {
        BatchDispatchGuard() { begin_batch_dispatch(); }
        ~BatchDispatchGuard() { end_batch_dispatch(); }
    };

private:
    static void begin_batch_dispatch();
    static void end_batch_dispatch();

    // State
    Implementation server_info_;
    ServerCapabilities capabilities_;
    LogHandler log_handler_;  // optional; log_() falls back to AsyncLogger
    std::shared_ptr<ITransport> transport_;
    mutable std::shared_mutex handlers_mutex_;
    std::atomic<bool> running_{false};
    std::atomic<bool> initialized_{false};
    std::atomic<bool> frozen_{false};
    std::atomic<bool> stopping_{false};

    asio::io_context io_ctx_;
    asio::executor_work_guard<asio::io_context::executor_type> work_guard_;
    asio::signal_set signals_;
    std::unique_ptr<RequestExecutor> executor_;
    std::size_t configured_workers_ = 0;
    std::size_t configured_io_threads_ = 0;
    ListCache list_cache_;
    mutable std::mutex list_cache_mutex_;
    std::atomic<bool> list_cache_enabled_{true};
    std::size_t list_page_size_ = 0;

    // Handler registries (mutable pre-runtime, frozen at run() time)
    std::map<std::string, std::pair<Tool, ToolHandler>> tool_handlers_;
    ToolListHandler tool_list_handler_;

    std::map<std::string, std::pair<Resource, ResourceHandler>> resource_handlers_;
    ResourceListHandler resource_list_handler_;
    ResourceTemplateListHandler resource_template_list_handler_;
    std::map<std::string, std::pair<ResourceTemplate, ResourceTemplateHandler>> resource_template_handlers_;

    std::map<std::string, std::pair<Prompt, PromptHandler>> prompt_handlers_;
    PromptListHandler prompt_list_handler_;

    CompletionHandler completion_handler_;
    InitializeHandler initialize_handler_;
    LoggingLevelHandler logging_level_handler_;

    // Frozen copies — immutable after run(), lock-free reads during runtime
    std::map<std::string, std::pair<Tool, ToolHandler>> frozen_tool_handlers_;
    ToolListHandler frozen_tool_list_handler_;
    std::map<std::string, std::pair<Resource, ResourceHandler>> frozen_resource_handlers_;
    ResourceListHandler frozen_resource_list_handler_;
    ResourceTemplateListHandler frozen_resource_template_list_handler_;
    std::map<std::string, std::pair<ResourceTemplate, ResourceTemplateHandler>> frozen_resource_template_handlers_;
    std::map<std::string, std::pair<Prompt, PromptHandler>> frozen_prompt_handlers_;
    PromptListHandler frozen_prompt_list_handler_;
    CompletionHandler frozen_completion_handler_;
    InitializeHandler frozen_initialize_handler_;
    LoggingLevelHandler frozen_logging_level_handler_;

    // Per-session client state (capabilities + negotiated version), keyed by
    // session id (HTTP) or "" (stdio/pipe single client).
    struct ClientHello {
        ClientCapabilities capabilities;
        std::string protocol_version;
    };
    std::map<std::string, ClientHello> client_hellos_;  // guarded by handlers_mutex_
    std::string negotiated_version_;                    // guarded by handlers_mutex_

    // Subscribed resources, per session: session -> subscribed URIs.
    std::map<std::string, std::set<std::string>> subscribed_resources_;  // guarded by handlers_mutex_

    // Server-initiated requests awaiting a client response.
    struct OutboundPending {
        std::string method;
        std::string session_id;
        std::function<void(const nlohmann::json&)> on_result;
        std::function<void(const McpException&)> on_error;
    };
    std::mutex outbound_mutex_;
    std::map<int64_t, OutboundPending> outbound_pending_;
    std::atomic<int64_t> next_outbound_id_{1};
};

} // namespace cppmcp