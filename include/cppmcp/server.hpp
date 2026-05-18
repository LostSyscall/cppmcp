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
#include "jsonrpc.hpp"
#include "protocol.hpp"
#include "types.hpp"

namespace cppmcp {

// --- Handler types ---
using ToolHandler = std::function<CallToolResult(const nlohmann::json& arguments, RequestContext& ctx)>;
using ToolListHandler = std::function<std::vector<Tool>()>;

using ResourceHandler = std::function<ReadResourceResult(const std::string& uri, RequestContext& ctx)>;
using ResourceListHandler = std::function<std::vector<Resource>()>;
using ResourceTemplateListHandler = std::function<std::vector<ResourceTemplate>()>;

using PromptHandler = std::function<GetPromptResult(const std::string& name,
                                                     const nlohmann::json& arguments,
                                                     RequestContext& ctx)>;
using PromptListHandler = std::function<std::vector<Prompt>()>;

using CompletionHandler = std::function<CompleteResult(const CompletionReference& ref,
                                                        const CompletionArgument& arg)>;

using InitializeHandler = std::function<InitializeResult(const nlohmann::json& params)>;
using LoggingLevelHandler = std::function<void(const std::string& level)>;

class McpServer {
public:
    McpServer(const Implementation& info, const ServerCapabilities& capabilities = {});

    // --- Handler Registration ---
    void register_tool(const std::string& name, const Tool& tool_def, ToolHandler handler);
    void register_tool_list(ToolListHandler handler);

    void register_resource(const std::string& uri, const Resource& resource_def, ResourceHandler handler);
    void register_resource_list(ResourceListHandler handler);
    void register_resource_template_list(ResourceTemplateListHandler handler);

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

    // --- Notification Sending ---
    void send_notification(const std::string& method, nlohmann::json params = {});
    void notify_tools_list_changed();
    void notify_resources_list_changed();
    void notify_resources_updated(const std::string& uri);
    void notify_prompts_list_changed();
    void notify_progress(const RequestId& progress_token, double progress,
                         std::optional<double> total = std::nullopt);
    void notify_logging(const std::string& level, const std::string& data,
                        std::optional<std::string> logger = std::nullopt);

    const Implementation& server_info() const { return server_info_; }
    const ServerCapabilities& capabilities() const { return capabilities_; }

private:
    // Message processing
    void on_message(const nlohmann::json& message);
    nlohmann::json process_request(const JsonRpcRequest& req);
    void process_notification(const JsonRpcNotification& notif);

    // Method dispatch
    nlohmann::json dispatch_method(const std::string& method, const RequestId& id,
                                   const std::optional<nlohmann::json>& params);

    // Specific method handlers
    nlohmann::json handle_initialize(const RequestId& id, const nlohmann::json& params);
    nlohmann::json handle_ping(const RequestId& id);
    nlohmann::json handle_tools_list(const RequestId& id);
    nlohmann::json handle_tools_call(const RequestId& id, const nlohmann::json& params);
    nlohmann::json handle_resources_list(const RequestId& id);
    nlohmann::json handle_resources_read(const RequestId& id, const nlohmann::json& params);
    nlohmann::json handle_resources_subscribe(const RequestId& id, const nlohmann::json& params);
    nlohmann::json handle_resources_unsubscribe(const RequestId& id, const nlohmann::json& params);
    nlohmann::json handle_resources_templates_list(const RequestId& id);
    nlohmann::json handle_prompts_list(const RequestId& id);
    nlohmann::json handle_prompts_get(const RequestId& id, const nlohmann::json& params);
    nlohmann::json handle_completion_complete(const RequestId& id, const nlohmann::json& params);
    nlohmann::json handle_logging_set_level(const RequestId& id, const nlohmann::json& params);

    // State
    Implementation server_info_;
    ServerCapabilities capabilities_;
    std::shared_ptr<ITransport> transport_;
    std::shared_mutex handlers_mutex_;
    std::atomic<bool> running_{false};
    std::atomic<bool> initialized_{false};
    std::atomic<bool> frozen_{false};

    asio::io_context io_ctx_;
    asio::executor_work_guard<asio::io_context::executor_type> work_guard_;
    asio::signal_set signals_;

    // Handler registries (mutable pre-runtime, frozen at run() time)
    std::map<std::string, std::pair<Tool, ToolHandler>> tool_handlers_;
    ToolListHandler tool_list_handler_;

    std::map<std::string, std::pair<Resource, ResourceHandler>> resource_handlers_;
    ResourceListHandler resource_list_handler_;
    ResourceTemplateListHandler resource_template_list_handler_;

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
    std::map<std::string, std::pair<Prompt, PromptHandler>> frozen_prompt_handlers_;
    PromptListHandler frozen_prompt_list_handler_;
    CompletionHandler frozen_completion_handler_;
    InitializeHandler frozen_initialize_handler_;
    LoggingLevelHandler frozen_logging_level_handler_;

    // Subscribed resources tracking
    std::set<std::string> subscribed_resources_;
};

} // namespace cppmcp