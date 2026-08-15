#pragma once

#include <asio.hpp>

#include <atomic>
#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "client_transport.hpp"
#include "common.hpp"
#include "pending_request.hpp"
#include "protocol.hpp"
#include "request_executor.hpp"
#include "types.hpp"

namespace cppmcp {

// Fluent builder for an outbound request. Configure callbacks/timeout, then
// send() — at send() an id is allocated, the request is registered and written.
// Callbacks are set BEFORE send() so there is no "response arrived first" race.
class RequestBuilder {
public:
    RequestBuilder& on_complete(CompleteCallback cb) { on_complete_ = std::move(cb); return *this; }
    RequestBuilder& on_error(OutcomeCallback cb) { on_error_ = std::move(cb); return *this; }
    RequestBuilder& on_progress(ProgressCallback cb) { on_progress_ = std::move(cb); return *this; }
    RequestBuilder& timeout(std::chrono::milliseconds t) { timeout_ = t; return *this; }
    // Explicit progress token; default (none) uses the request id when on_progress is set.
    RequestBuilder& progress_token(RequestId token) { progress_token_ = std::move(token); return *this; }

    std::shared_ptr<PendingRequest> send();

private:
    friend class McpClient;
    RequestBuilder(McpClient* client, std::string method, std::optional<nlohmann::json> params)
        : client_(client), method_(std::move(method)), params_(std::move(params)) {}

    McpClient* client_;
    std::string method_;
    std::optional<nlohmann::json> params_;
    CompleteCallback on_complete_;
    OutcomeCallback on_error_;
    ProgressCallback on_progress_;
    std::chrono::milliseconds timeout_{0};
    std::optional<RequestId> progress_token_;
};

using SamplingHandler = std::function<CreateMessageResult(const CreateMessageRequestParams&)>;
using ElicitationHandler = std::function<ElicitResult(const ElicitRequestParams&)>;
using RootsHandler = std::function<ListRootsResult()>;
using ClientDisconnectHandler = std::function<void()>;
// Server-initiated notifications the client can observe.
using ResourceUpdatedHandler = std::function<void(const std::string& uri)>;
using ListChangedHandler = std::function<void(const std::string& list_kind)>;  // tools/resources/prompts
using ServerLogHandler = std::function<void(const LoggingMessageNotificationParams& msg)>;
using RootsChangedHandler = std::function<void()>;

// High-performance async MCP client. Owns an io_context (or attaches to an
// external one), serializes all internal state through a strand, and correlates
// responses to outbound requests by id via std::promise/future.
//
// MUST be created via std::make_shared (internally uses shared_from_this()).
// Multiple McpClient instances coexist freely; sharing one io_context across
// them is safe because each has its own strand.
class McpClient : public std::enable_shared_from_this<McpClient> {
public:
    McpClient(const Implementation& client_info,
              const ClientCapabilities& capabilities = ClientCapabilities{});
    ~McpClient();

    McpClient(const McpClient&) = delete;
    McpClient& operator=(const McpClient&) = delete;

    // --- configuration (call before connect) ---
    // Attach to an external io_context instead of running an internal thread.
    void set_io_context(asio::io_context* io_ctx);
    // Worker pool for slow server->client handlers (sampling/elicitation/roots).
    void set_worker_threads(std::size_t n);
    // Dispatch user callbacks (on_complete/on_error/on_progress) on this
    // executor instead of inline on the client strand. Use a separate strand or
    // pool to keep slow user code off the I/O path.
    void set_callback_executor(asio::any_io_executor executor);
    // Default per-request timeout applied when a request sets none (0 disables;
    // useful to avoid a hung request blocking forever against a dead server).
    void set_default_request_timeout(std::chrono::milliseconds t) { default_timeout_ = t; }

    void use_transport(std::shared_ptr<IClientTransport> transport);

    // --- lifecycle ---
    // Start the I/O thread (if self-owned), connect the transport, and perform
    // the initialize handshake. Blocks until the handshake completes or times
    // out. Returns the server's InitializeResult. May be called again after
    // disconnect() to reconnect.
    InitializeResult connect(std::chrono::milliseconds timeout = std::chrono::seconds(30));
    // Non-blocking variant: returns the initialize PendingRequest.
    std::shared_ptr<PendingRequest> async_connect(std::chrono::milliseconds timeout = std::chrono::seconds(30));

    // Close the transport and fail all pending requests; the I/O thread stays up.
    void disconnect();
    // Full shutdown: disconnect + stop executor + join I/O thread. Idempotent.
    void stop();

    bool is_connected() const { return connected_.load(); }
    bool is_running() const { return running_.load(); }

    // --- post-handshake state ---
    const std::optional<InitializeResult>& server_result() const { return server_result_; }
    bool has_capability(const std::string& cap) const;

    // --- outbound request API ---
    RequestBuilder prepare(const std::string& method,
                           std::optional<nlohmann::json> params = std::nullopt);
    void send_notification(const std::string& method, nlohmann::json params = nlohmann::json::object());

    // --- convenience synchronous wrappers (build + get) ---
    std::vector<Tool> list_tools(std::optional<std::string> cursor = std::nullopt,
                                 std::string* next_cursor = nullptr);
    CallToolResult call_tool(const std::string& name, const nlohmann::json& arguments = nlohmann::json::object());
    std::vector<Resource> list_resources(std::optional<std::string> cursor = std::nullopt,
                                         std::string* next_cursor = nullptr);
    ReadResourceResult read_resource(const std::string& uri);
    std::vector<ResourceTemplate> list_resource_templates(std::optional<std::string> cursor = std::nullopt,
                                                          std::string* next_cursor = nullptr);
    std::vector<Prompt> list_prompts(std::optional<std::string> cursor = std::nullopt,
                                     std::string* next_cursor = nullptr);
    GetPromptResult get_prompt(const std::string& name, const nlohmann::json& arguments = nlohmann::json::object());
    void set_logging_level(const std::string& level);
    CompleteResult complete(const CompletionReference& ref, const CompletionArgument& argument);
    void subscribe_resource(const std::string& uri);
    void unsubscribe_resource(const std::string& uri);
    void ping();

    // --- server -> client handler registration ---
    void register_sampling_handler(SamplingHandler handler);
    void register_elicitation_handler(ElicitationHandler handler);
    void register_roots_handler(RootsHandler handler);
    void on_disconnect(ClientDisconnectHandler handler);
    // Server push notifications. Handlers run on the client strand (or the
    // callback executor when set); keep them fast.
    void on_resource_updated(ResourceUpdatedHandler handler);
    void on_list_changed(ListChangedHandler handler);
    void on_server_log(ServerLogHandler handler);
    void on_roots_changed(RootsChangedHandler handler);

private:
    friend class PendingRequest;
    friend class RequestBuilder;

    using strand_t = asio::strand<asio::io_context::executor_type>;
    using work_guard_t = asio::executor_work_guard<asio::io_context::executor_type>;

    void init_owned_io();
    void attach_transport_handlers();  // (re)arm transport callbacks; safe to call repeatedly
    void ensure_started();   // start io thread (self-owned) once
    void ensure_executor();  // lazily start worker pool if configured

    // strand-only
    void on_transport_message(nlohmann::json message);
    void handle_success(const JsonRpcSuccessResponse& resp);
    void handle_error_resp(const JsonRpcErrorResponse& resp);
    void handle_inbound_request(JsonRpcRequest req);
    void handle_notification(const JsonRpcNotification& notif);
    void dispatch_inbound_request(JsonRpcRequest req, std::function<nlohmann::json(const nlohmann::json&)> run);
    void fail_all_pending(const std::string& reason);
    void on_transport_disconnect();
    // Drain pending requests, preferring a strand post; falls back to a
    // synchronous fail when no shared_ptr is obtainable (destruction) or the
    // io loop is gone — avoids bad_weak_ptr from ~McpClient.
    void drain_pending(const std::string& reason);

    // user-thread entry: allocates id, builds pr, posts registration to strand.
    std::shared_ptr<PendingRequest> submit_request(
        const std::string& method,
        std::optional<nlohmann::json> params,
        CompleteCallback on_complete,
        OutcomeCallback on_error,
        ProgressCallback on_progress,
        std::chrono::milliseconds timeout,
        std::optional<RequestId> progress_token);

    // strand-only helpers
    void register_pending(std::shared_ptr<PendingRequest> pr);
    void complete_pending(const RequestId& id, McpOutcome outcome);  // erase + finish
    void request_cancel(const RequestId& id, std::string reason);
    void raw_send(nlohmann::json message);

    nlohmann::json make_initialize_params();
    // Verify the server's negotiated protocolVersion is one we support.
    void check_negotiated_version(const nlohmann::json& result);

    // config
    Implementation client_info_;
    ClientCapabilities capabilities_;
    std::size_t configured_workers_{0};
    std::chrono::milliseconds default_timeout_{0};

    // io
    asio::io_context* io_ptr_{nullptr};
    std::optional<asio::io_context> owned_io_;
    bool owns_io_context_{true};
    std::unique_ptr<work_guard_t> work_guard_;
    std::unique_ptr<strand_t> strand_;
    std::thread io_thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> stopping_{false};

    std::shared_ptr<IClientTransport> transport_;
    std::unique_ptr<RequestExecutor> executor_;
    std::optional<asio::any_io_executor> callback_executor_;

    // pending outbound (strand-only)
    std::map<RequestId, std::shared_ptr<PendingRequest>> pending_map_;
    std::atomic<int64_t> next_id_{1};

    // handshake state (written on strand / read after connect returns)
    std::optional<InitializeResult> server_result_;
    std::atomic<bool> connected_{false};
    std::atomic<bool> initialized_{false};

    // server -> client handlers
    std::shared_mutex inbound_mutex_;
    SamplingHandler sampling_handler_;
    ElicitationHandler elicitation_handler_;
    RootsHandler roots_handler_;
    ClientDisconnectHandler disconnect_handler_;
    ResourceUpdatedHandler resource_updated_handler_;
    ListChangedHandler list_changed_handler_;
    ServerLogHandler server_log_handler_;
    RootsChangedHandler roots_changed_handler_;
};

} // namespace cppmcp
