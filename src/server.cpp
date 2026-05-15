#include "cppmcp/server.hpp"
#include "cppmcp/transport.hpp"

#include <iostream>

namespace cppmcp {

McpServer::McpServer(const Implementation& info, const ServerCapabilities& capabilities)
    : server_info_(info), capabilities_(capabilities) {}

void McpServer::register_tool(const std::string& name, const Tool& tool_def, ToolHandler handler) {
    if (running_.load()) {
        std::cerr << "[cppmcp] Cannot register handlers after server is running" << std::endl;
        return;
    }
    std::lock_guard<std::mutex> lock(handlers_mutex_);
    if (tool_handlers_.count(name)) {
        std::cerr << "[cppmcp] Tool '" << name << "' already registered, overwriting" << std::endl;
    }
    tool_handlers_[name] = {tool_def, handler};
}

void McpServer::register_tool_list(ToolListHandler handler) {
    if (running_.load()) {
        std::cerr << "[cppmcp] Cannot register handlers after server is running" << std::endl;
        return;
    }
    std::lock_guard<std::mutex> lock(handlers_mutex_);
    tool_list_handler_ = std::move(handler);
}

void McpServer::register_resource(const std::string& uri, const Resource& resource_def, ResourceHandler handler) {
    if (running_.load()) {
        std::cerr << "[cppmcp] Cannot register handlers after server is running" << std::endl;
        return;
    }
    std::lock_guard<std::mutex> lock(handlers_mutex_);
    resource_handlers_[uri] = {resource_def, handler};
}

void McpServer::register_resource_list(ResourceListHandler handler) {
    if (running_.load()) {
        std::cerr << "[cppmcp] Cannot register handlers after server is running" << std::endl;
        return;
    }
    std::lock_guard<std::mutex> lock(handlers_mutex_);
    resource_list_handler_ = std::move(handler);
}

void McpServer::register_resource_template_list(ResourceTemplateListHandler handler) {
    if (running_.load()) {
        std::cerr << "[cppmcp] Cannot register handlers after server is running" << std::endl;
        return;
    }
    std::lock_guard<std::mutex> lock(handlers_mutex_);
    resource_template_list_handler_ = std::move(handler);
}

void McpServer::register_prompt(const std::string& name, const Prompt& prompt_def, PromptHandler handler) {
    if (running_.load()) {
        std::cerr << "[cppmcp] Cannot register handlers after server is running" << std::endl;
        return;
    }
    std::lock_guard<std::mutex> lock(handlers_mutex_);
    prompt_handlers_[name] = {prompt_def, handler};
}

void McpServer::register_prompt_list(PromptListHandler handler) {
    if (running_.load()) {
        std::cerr << "[cppmcp] Cannot register handlers after server is running" << std::endl;
        return;
    }
    std::lock_guard<std::mutex> lock(handlers_mutex_);
    prompt_list_handler_ = std::move(handler);
}

void McpServer::register_completion(CompletionHandler handler) {
    if (running_.load()) {
        std::cerr << "[cppmcp] Cannot register handlers after server is running" << std::endl;
        return;
    }
    std::lock_guard<std::mutex> lock(handlers_mutex_);
    completion_handler_ = std::move(handler);
}

void McpServer::register_initialize_handler(InitializeHandler handler) {
    if (running_.load()) {
        std::cerr << "[cppmcp] Cannot register handlers after server is running" << std::endl;
        return;
    }
    std::lock_guard<std::mutex> lock(handlers_mutex_);
    initialize_handler_ = std::move(handler);
}

void McpServer::register_logging_level_handler(LoggingLevelHandler handler) {
    if (running_.load()) {
        std::cerr << "[cppmcp] Cannot register handlers after server is running" << std::endl;
        return;
    }
    std::lock_guard<std::mutex> lock(handlers_mutex_);
    logging_level_handler_ = std::move(handler);
}

void McpServer::connect(std::shared_ptr<ITransport> transport) {
    std::lock_guard<std::mutex> lock(handlers_mutex_);
    transport_ = transport;
    transport_->set_message_handler([this](const nlohmann::json& msg) {
        on_message(msg);
    });
    transport_->set_error_handler([](const std::string& err) {
        std::cerr << "[cppmcp] Transport error: " << err << std::endl;
    });
}

void McpServer::run() {
    if (!transport_) {
        std::cerr << "[cppmcp] No transport connected" << std::endl;
        return;
    }
    running_ = true;
    transport_->start();
    // Block until stop() is called or transport stops naturally (e.g., stdin EOF)
    std::unique_lock<std::mutex> lock(stop_mutex_);
    while (running_.load()) {
        stop_cv_.wait_for(lock, std::chrono::milliseconds(STOP_CHECK_INTERVAL_MS));
        if (!running_.load()) break;
        if (transport_ && !transport_->is_running()) {
            running_ = false;
            break;
        }
    }
}

void McpServer::stop() {
    running_ = false;
    stop_cv_.notify_all();
    if (transport_) {
        transport_->stop();
    }
}

void McpServer::on_message(const nlohmann::json& message) {
    auto parsed = parse_message(message);

    if (auto* req = std::get_if<JsonRpcRequest>(&parsed)) {
        auto response = process_request(*req);
        if (transport_) {
            transport_->send_message(response);
        }
    } else if (auto* notif = std::get_if<JsonRpcNotification>(&parsed)) {
        process_notification(*notif);
    } else if (auto* err_resp = std::get_if<JsonRpcErrorResponse>(&parsed)) {
        nlohmann::json j;
        j["jsonrpc"] = "2.0";
        j["id"] = request_id_to_json(err_resp->id);
        nlohmann::json error_obj;
        error_obj["code"] = err_resp->error.code;
        error_obj["message"] = err_resp->error.message;
        if (err_resp->error.data) error_obj["data"] = *err_resp->error.data;
        j["error"] = error_obj;
        if (transport_) {
            transport_->send_message(j);
        }
    }
}

nlohmann::json McpServer::process_request(const JsonRpcRequest& req) {
    try {
        return dispatch_method(req.method, req.id, req.params);
    } catch (const McpException& e) {
        return make_error_response(req.id, e.code(), e.message(), e.data());
    } catch (const std::exception& e) {
        return make_error_response(req.id, Protocol::INTERNAL_ERROR, e.what());
    } catch (...) {
        return make_error_response(req.id, Protocol::INTERNAL_ERROR, "Unknown error");
    }
}

void McpServer::process_notification(const JsonRpcNotification& notif) {
    if (notif.method == Protocol::NOTIF_INITIALIZED) {
        initialized_ = true;
    } else if (notif.method == Protocol::NOTIF_CANCELLED) {
        // Handle cancellation — for now just log
        std::cerr << "[cppmcp] Received cancellation notification" << std::endl;
    }
}

nlohmann::json McpServer::dispatch_method(const std::string& method, const RequestId& id,
                                           const std::optional<nlohmann::json>& params) {
    if (method == Protocol::METHOD_INITIALIZE) {
        return handle_initialize(id, params.value_or(nlohmann::json::object()));
    }
    if (method == Protocol::METHOD_PING) {
        return handle_ping(id);
    }

    // Must be initialized before calling other methods
    if (!initialized_) {
        return make_error_response(id, Protocol::INVALID_REQUEST,
                                   "Server not initialized. Send initialize request first.");
    }

    if (method == Protocol::METHOD_TOOLS_LIST) return handle_tools_list(id);
    if (method == Protocol::METHOD_TOOLS_CALL) return handle_tools_call(id, params.value_or(nlohmann::json::object()));
    if (method == Protocol::METHOD_RESOURCES_LIST) return handle_resources_list(id);
    if (method == Protocol::METHOD_RESOURCES_READ) return handle_resources_read(id, params.value_or(nlohmann::json::object()));
    if (method == Protocol::METHOD_RESOURCES_SUBSCRIBE) return handle_resources_subscribe(id, params.value_or(nlohmann::json::object()));
    if (method == Protocol::METHOD_RESOURCES_UNSUBSCRIBE) return handle_resources_unsubscribe(id, params.value_or(nlohmann::json::object()));
    if (method == Protocol::METHOD_RESOURCES_TEMPLATE_LIST) return handle_resources_templates_list(id);
    if (method == Protocol::METHOD_PROMPTS_LIST) return handle_prompts_list(id);
    if (method == Protocol::METHOD_PROMPTS_GET) return handle_prompts_get(id, params.value_or(nlohmann::json::object()));
    if (method == Protocol::METHOD_COMPLETION_COMPLETE) return handle_completion_complete(id, params.value_or(nlohmann::json::object()));
    if (method == Protocol::METHOD_LOGGING_SET_LEVEL) return handle_logging_set_level(id, params.value_or(nlohmann::json::object()));

    return make_error_response(id, Protocol::METHOD_NOT_FOUND, "Method not found: " + method);
}

nlohmann::json McpServer::handle_initialize(const RequestId& id, const nlohmann::json& params) {
    InitializeHandler init_handler;
    ServerCapabilities caps;
    Implementation info;
    {
        std::lock_guard<std::mutex> lock(handlers_mutex_);
        init_handler = initialize_handler_;
        caps = capabilities_;
        info = server_info_;
    }

    if (init_handler) {
        auto result = init_handler(params);
        return make_success_response(id, nlohmann::json(result));
    }

    InitializeResult result;
    result.protocol_version = Protocol::DEFAULT_NEGOTIATED_VERSION;
    result.capabilities = caps;
    result.server_info = info;
    return make_success_response(id, nlohmann::json(result));
}

nlohmann::json McpServer::handle_ping(const RequestId& id) {
    return make_success_response(id, nlohmann::json(EmptyResult{}));
}

nlohmann::json McpServer::handle_tools_list(const RequestId& id) {
    ToolListHandler list_handler;
    std::map<std::string, std::pair<Tool, ToolHandler>> handlers_copy;
    {
        std::lock_guard<std::mutex> lock(handlers_mutex_);
        list_handler = tool_list_handler_;
        handlers_copy = tool_handlers_;
    }

    std::vector<Tool> tools;
    if (list_handler) {
        tools = list_handler();
    } else {
        for (const auto& [name, pair] : handlers_copy) {
            tools.push_back(pair.first);
        }
    }

    nlohmann::json result;
    result["tools"] = nlohmann::json::array();
    for (const auto& t : tools) {
        result["tools"].push_back(nlohmann::json(t));
    }
    return make_success_response(id, result);
}

nlohmann::json McpServer::handle_tools_call(const RequestId& id, const nlohmann::json& params) {
    if (!params.contains("name")) {
        return make_error_response(id, Protocol::INVALID_PARAMS, "Missing 'name' parameter");
    }

    std::string tool_name = params["name"].get<std::string>();
    ToolHandler handler;
    {
        std::lock_guard<std::mutex> lock(handlers_mutex_);
        auto it = tool_handlers_.find(tool_name);
        if (it == tool_handlers_.end()) {
            return make_error_response(id, Protocol::METHOD_NOT_FOUND, "Tool not found: " + tool_name);
        }
        handler = it->second.second;
    }

    nlohmann::json arguments = params.contains("arguments") ? params["arguments"] : nlohmann::json::object();
    RequestContext ctx(*this, id, transport_);

    auto result = handler(arguments, ctx);
    return make_success_response(id, nlohmann::json(result));
}

nlohmann::json McpServer::handle_resources_list(const RequestId& id) {
    ResourceListHandler list_handler;
    std::vector<Resource> resources;
    {
        std::lock_guard<std::mutex> lock(handlers_mutex_);
        list_handler = resource_list_handler_;
        if (!list_handler) {
            for (const auto& [uri, pair] : resource_handlers_) {
                resources.push_back(pair.first);
            }
        }
    }

    if (list_handler) {
        resources = list_handler();
    }

    nlohmann::json result;
    result["resources"] = nlohmann::json::array();
    for (const auto& r : resources) {
        result["resources"].push_back(nlohmann::json(r));
    }
    return make_success_response(id, result);
}

nlohmann::json McpServer::handle_resources_read(const RequestId& id, const nlohmann::json& params) {
    if (!params.contains("uri")) {
        return make_error_response(id, Protocol::INVALID_PARAMS, "Missing 'uri' parameter");
    }

    std::string uri = params["uri"].get<std::string>();
    ResourceHandler handler;
    {
        std::lock_guard<std::mutex> lock(handlers_mutex_);
        auto it = resource_handlers_.find(uri);
        if (it == resource_handlers_.end()) {
            return make_error_response(id, Protocol::METHOD_NOT_FOUND, "Resource not found: " + uri);
        }
        handler = it->second.second;
    }

    RequestContext ctx(*this, id, transport_);
    auto result = handler(uri, ctx);
    return make_success_response(id, nlohmann::json(result));
}

nlohmann::json McpServer::handle_resources_subscribe(const RequestId& id, const nlohmann::json& params) {
    if (!params.contains("uri")) {
        return make_error_response(id, Protocol::INVALID_PARAMS, "Missing 'uri' parameter");
    }
    std::string uri = params["uri"].get<std::string>();
    {
        std::lock_guard<std::mutex> lock(handlers_mutex_);
        subscribed_resources_.insert(uri);
    }
    return make_success_response(id, nlohmann::json(EmptyResult{}));
}

nlohmann::json McpServer::handle_resources_unsubscribe(const RequestId& id, const nlohmann::json& params) {
    if (!params.contains("uri")) {
        return make_error_response(id, Protocol::INVALID_PARAMS, "Missing 'uri' parameter");
    }
    std::string uri = params["uri"].get<std::string>();
    {
        std::lock_guard<std::mutex> lock(handlers_mutex_);
        subscribed_resources_.erase(uri);
    }
    return make_success_response(id, nlohmann::json(EmptyResult{}));
}

nlohmann::json McpServer::handle_resources_templates_list(const RequestId& id) {
    ResourceTemplateListHandler list_handler;
    {
        std::lock_guard<std::mutex> lock(handlers_mutex_);
        list_handler = resource_template_list_handler_;
    }

    std::vector<ResourceTemplate> templates;
    if (list_handler) {
        templates = list_handler();
    }

    nlohmann::json result;
    result["resourceTemplates"] = nlohmann::json::array();
    for (const auto& t : templates) {
        result["resourceTemplates"].push_back(nlohmann::json(t));
    }
    return make_success_response(id, result);
}

nlohmann::json McpServer::handle_prompts_list(const RequestId& id) {
    PromptListHandler list_handler;
    std::vector<Prompt> prompts;
    {
        std::lock_guard<std::mutex> lock(handlers_mutex_);
        list_handler = prompt_list_handler_;
        if (!list_handler) {
            for (const auto& [name, pair] : prompt_handlers_) {
                prompts.push_back(pair.first);
            }
        }
    }

    if (list_handler) {
        prompts = list_handler();
    }

    nlohmann::json result;
    result["prompts"] = nlohmann::json::array();
    for (const auto& p : prompts) {
        result["prompts"].push_back(nlohmann::json(p));
    }
    return make_success_response(id, result);
}

nlohmann::json McpServer::handle_prompts_get(const RequestId& id, const nlohmann::json& params) {
    if (!params.contains("name")) {
        return make_error_response(id, Protocol::INVALID_PARAMS, "Missing 'name' parameter");
    }

    std::string prompt_name = params["name"].get<std::string>();
    PromptHandler handler;
    {
        std::lock_guard<std::mutex> lock(handlers_mutex_);
        auto it = prompt_handlers_.find(prompt_name);
        if (it == prompt_handlers_.end()) {
            return make_error_response(id, Protocol::METHOD_NOT_FOUND, "Prompt not found: " + prompt_name);
        }
        handler = it->second.second;
    }

    nlohmann::json arguments = params.contains("arguments") ? params["arguments"] : nlohmann::json::object();
    RequestContext ctx(*this, id, transport_);

    auto result = handler(prompt_name, arguments, ctx);
    return make_success_response(id, nlohmann::json(result));
}

nlohmann::json McpServer::handle_completion_complete(const RequestId& id, const nlohmann::json& params) {
    CompletionHandler handler;
    {
        std::lock_guard<std::mutex> lock(handlers_mutex_);
        if (!completion_handler_) {
            return make_error_response(id, Protocol::METHOD_NOT_FOUND, "Completions not supported");
        }
        handler = completion_handler_;
    }

    CompletionReference ref;
    if (params.contains("ref")) {
        ref.type = params["ref"]["type"].get<std::string>();
        ref.name = params["ref"]["name"].get<std::string>();
    }
    CompletionArgument arg;
    if (params.contains("argument")) {
        arg.name = params["argument"]["name"].get<std::string>();
        arg.value = params["argument"]["value"].get<std::string>();
    }

    auto result = handler(ref, arg);
    return make_success_response(id, nlohmann::json(result));
}

nlohmann::json McpServer::handle_logging_set_level(const RequestId& id, const nlohmann::json& params) {
    if (!params.contains("level")) {
        return make_error_response(id, Protocol::INVALID_PARAMS, "Missing 'level' parameter");
    }

    std::string level = params["level"].get<std::string>();
    LoggingLevelHandler handler;
    {
        std::lock_guard<std::mutex> lock(handlers_mutex_);
        handler = logging_level_handler_;
    }

    if (handler) {
        handler(level);
    }
    return make_success_response(id, nlohmann::json(EmptyResult{}));
}

// --- Notification sending ---
void McpServer::send_notification(const std::string& method, const nlohmann::json& params) {
    if (!transport_ || !running_) return;
    nlohmann::json notif;
    notif["jsonrpc"] = "2.0";
    notif["method"] = method;
    if (!params.is_null()) notif["params"] = params;
    try {
        transport_->send_message(notif);
    } catch (const std::exception&) {
        // Notification send failure is non-critical; swallow to avoid propagating to handler
    }
}

void McpServer::notify_tools_list_changed() {
    send_notification(Protocol::NOTIF_TOOLS_LIST_CHANGED);
}

void McpServer::notify_resources_list_changed() {
    send_notification(Protocol::NOTIF_RESOURCES_LIST_CHANGED);
}

void McpServer::notify_resources_updated(const std::string& uri) {
    send_notification(Protocol::NOTIF_RESOURCES_UPDATED, {{ "uri", uri }});
}

void McpServer::notify_prompts_list_changed() {
    send_notification(Protocol::NOTIF_PROMPTS_LIST_CHANGED);
}

void McpServer::notify_progress(const RequestId& progress_token, double progress,
                                 std::optional<double> total) {
    nlohmann::json params;
    params["progressToken"] = request_id_to_json(progress_token);
    params["progress"] = progress;
    if (total) params["total"] = *total;
    send_notification(Protocol::NOTIF_PROGRESS, params);
}

void McpServer::notify_logging(const std::string& level, const std::string& data,
                                std::optional<std::string> logger) {
    nlohmann::json params;
    params["level"] = level;
    params["data"] = data;
    if (logger) params["logger"] = *logger;
    send_notification(Protocol::NOTIF_MESSAGE, params);
}

} // namespace cppmcp