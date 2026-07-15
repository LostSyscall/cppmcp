#include "cppmcp/server.hpp"
#include "cppmcp/transport.hpp"
#include "cppmcp/logging.hpp"

#include <iostream>

namespace cppmcp {

McpServer::McpServer(const Implementation& info, const ServerCapabilities& capabilities)
    : server_info_(info), capabilities_(capabilities),
      work_guard_(io_ctx_.get_executor()),
      signals_(io_ctx_, SIGINT, SIGTERM) {}

void McpServer::register_tool(const std::string& name, const Tool& tool_def, ToolHandler handler) {
    if (running_.load()) {
        std::cerr << "[cppmcp] Cannot register handlers after server is running" << std::endl;
        return;
    }
    std::unique_lock<std::shared_mutex> lock(handlers_mutex_);
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
    std::unique_lock<std::shared_mutex> lock(handlers_mutex_);
    tool_list_handler_ = std::move(handler);
}

void McpServer::register_resource(const std::string& uri, const Resource& resource_def, ResourceHandler handler) {
    if (running_.load()) {
        std::cerr << "[cppmcp] Cannot register handlers after server is running" << std::endl;
        return;
    }
    std::unique_lock<std::shared_mutex> lock(handlers_mutex_);
    resource_handlers_[uri] = {resource_def, handler};
}

void McpServer::register_resource_list(ResourceListHandler handler) {
    if (running_.load()) {
        std::cerr << "[cppmcp] Cannot register handlers after server is running" << std::endl;
        return;
    }
    std::unique_lock<std::shared_mutex> lock(handlers_mutex_);
    resource_list_handler_ = std::move(handler);
}

void McpServer::register_resource_template_list(ResourceTemplateListHandler handler) {
    if (running_.load()) {
        std::cerr << "[cppmcp] Cannot register handlers after server is running" << std::endl;
        return;
    }
    std::unique_lock<std::shared_mutex> lock(handlers_mutex_);
    resource_template_list_handler_ = std::move(handler);
}

void McpServer::register_prompt(const std::string& name, const Prompt& prompt_def, PromptHandler handler) {
    if (running_.load()) {
        std::cerr << "[cppmcp] Cannot register handlers after server is running" << std::endl;
        return;
    }
    std::unique_lock<std::shared_mutex> lock(handlers_mutex_);
    prompt_handlers_[name] = {prompt_def, handler};
}

void McpServer::register_prompt_list(PromptListHandler handler) {
    if (running_.load()) {
        std::cerr << "[cppmcp] Cannot register handlers after server is running" << std::endl;
        return;
    }
    std::unique_lock<std::shared_mutex> lock(handlers_mutex_);
    prompt_list_handler_ = std::move(handler);
}

void McpServer::register_completion(CompletionHandler handler) {
    if (running_.load()) {
        std::cerr << "[cppmcp] Cannot register handlers after server is running" << std::endl;
        return;
    }
    std::unique_lock<std::shared_mutex> lock(handlers_mutex_);
    completion_handler_ = std::move(handler);
}

void McpServer::register_initialize_handler(InitializeHandler handler) {
    if (running_.load()) {
        std::cerr << "[cppmcp] Cannot register handlers after server is running" << std::endl;
        return;
    }
    std::unique_lock<std::shared_mutex> lock(handlers_mutex_);
    initialize_handler_ = std::move(handler);
}

void McpServer::register_logging_level_handler(LoggingLevelHandler handler) {
    if (running_.load()) {
        std::cerr << "[cppmcp] Cannot register handlers after server is running" << std::endl;
        return;
    }
    std::unique_lock<std::shared_mutex> lock(handlers_mutex_);
    logging_level_handler_ = std::move(handler);
}

void McpServer::connect(std::shared_ptr<ITransport> transport) {
    std::unique_lock<std::shared_mutex> lock(handlers_mutex_);
    transport_ = std::move(transport);
    transport_->set_io_context(&io_ctx_);
    transport_->set_message_handler([this](const nlohmann::json& msg, ITransport::ResponseSink respond, const std::string& session_id) {
        on_message(msg, std::move(respond), session_id);
    });
    transport_->set_error_handler([](const std::string& err) {
        std::cerr << "[cppmcp] Transport error: " << err << std::endl;
    });
    transport_->set_disconnect_handler([this]() {
        stop();  // e.g. stdio stdin EOF -> shut the server down
    });
}

void McpServer::set_worker_threads(std::size_t n) {
    if (running_.load(std::memory_order_relaxed)) {
        return;  // must be configured before run()
    }
    configured_workers_ = n;
}

void McpServer::invalidate_list_cache() {
    std::lock_guard<std::mutex> lock(list_cache_mutex_);
    list_cache_.tools_list_result.reset();
    list_cache_.resources_list_result.reset();
    list_cache_.resource_templates_list_result.reset();
    list_cache_.prompts_list_result.reset();
}

void McpServer::run() {
    if (!transport_) {
        std::cerr << "[cppmcp] No transport connected" << std::endl;
        return;
    }

    // Freeze handler registries
    {
        std::unique_lock<std::shared_mutex> lock(handlers_mutex_);
        frozen_tool_handlers_ = tool_handlers_;
        frozen_tool_list_handler_ = tool_list_handler_;
        frozen_resource_handlers_ = resource_handlers_;
        frozen_resource_list_handler_ = resource_list_handler_;
        frozen_resource_template_list_handler_ = resource_template_list_handler_;
        frozen_prompt_handlers_ = prompt_handlers_;
        frozen_prompt_list_handler_ = prompt_list_handler_;
        frozen_completion_handler_ = completion_handler_;
        frozen_initialize_handler_ = initialize_handler_;
        frozen_logging_level_handler_ = logging_level_handler_;
    }
    frozen_.store(true, std::memory_order_release);

    running_ = true;

    signals_.async_wait([this](const asio::error_code&, int) {
        stop();
    });

    if (configured_workers_ > 0) {
        executor_ = std::make_unique<RequestExecutor>(configured_workers_);
        executor_->start();
    }

    try {
        transport_->start();
    } catch (const std::exception& e) {
        std::cerr << "[cppmcp] Transport start failed: " << e.what() << std::endl;
        running_ = false;
        frozen_.store(false, std::memory_order_release);
        work_guard_.reset();
        return;
    }

    io_ctx_.run();
}

void McpServer::stop() {
    if (stopping_.exchange(true)) {
        return;
    }
    frozen_.store(false, std::memory_order_release);
    running_ = false;
    if (executor_) {
        executor_->stop();  // join workers before transports/loop go away
    }
    if (transport_) {
        transport_->stop();  // drain/close transports before stopping the loop
    }
    signals_.cancel();
    work_guard_.reset();
    io_ctx_.stop();
    AsyncLogger::instance().flush();
}

namespace {
bool is_deferrable(const std::string& method) {
    return method == Protocol::METHOD_TOOLS_CALL
        || method == Protocol::METHOD_RESOURCES_READ
        || method == Protocol::METHOD_PROMPTS_GET;
}
}  // namespace

void McpServer::on_message(const nlohmann::json& message, ITransport::ResponseSink respond, const std::string& session_id) {
    auto parsed = parse_message(message);

    if (auto* req = std::get_if<JsonRpcRequest>(&parsed)) {
        if (executor_ && is_deferrable(req->method)) {
            // Run a potentially-slow handler on the worker pool so the io loop
            // is not blocked. The response is delivered back on the io thread.
            JsonRpcRequest req_copy = *req;
            std::string sid_copy = session_id;
            executor_->post([this, req_copy, respond, sid_copy]() {
                nlohmann::json response = process_request(req_copy, sid_copy);
                asio::post(io_ctx_, [respond, response = std::move(response)]() mutable {
                    if (respond) {
                        respond(response);
                    }
                });
            });
            return;
        }
        auto response = process_request(*req, session_id);
        if (respond) {
            respond(response);
        }
    } else if (auto* notif = std::get_if<JsonRpcNotification>(&parsed)) {
        process_notification(*notif);
    } else if (auto* err_resp = std::get_if<JsonRpcErrorResponse>(&parsed)) {
        auto j = make_error_response(err_resp->id, err_resp->error.code,
                                     err_resp->error.message, err_resp->error.data);
        if (respond) {
            respond(j);
        }
    }
}

nlohmann::json McpServer::process_request(const JsonRpcRequest& req, const std::string& session_id) {
    try {
        return dispatch_method(req.method, req.id, req.params, session_id);
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
                                           const std::optional<nlohmann::json>& params,
                                           const std::string& session_id) {
    static const nlohmann::json empty_params = nlohmann::json::object();
    const nlohmann::json& p = params.value_or(empty_params);

    if (method == Protocol::METHOD_INITIALIZE) {
        return handle_initialize(id, p);
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
    if (method == Protocol::METHOD_TOOLS_CALL) return handle_tools_call(id, p, session_id);
    if (method == Protocol::METHOD_RESOURCES_LIST) return handle_resources_list(id);
    if (method == Protocol::METHOD_RESOURCES_READ) return handle_resources_read(id, p, session_id);
    if (method == Protocol::METHOD_RESOURCES_SUBSCRIBE) return handle_resources_subscribe(id, p);
    if (method == Protocol::METHOD_RESOURCES_UNSUBSCRIBE) return handle_resources_unsubscribe(id, p);
    if (method == Protocol::METHOD_RESOURCES_TEMPLATE_LIST) return handle_resources_templates_list(id);
    if (method == Protocol::METHOD_PROMPTS_LIST) return handle_prompts_list(id);
    if (method == Protocol::METHOD_PROMPTS_GET) return handle_prompts_get(id, p, session_id);
    if (method == Protocol::METHOD_COMPLETION_COMPLETE) return handle_completion_complete(id, p);
    if (method == Protocol::METHOD_LOGGING_SET_LEVEL) return handle_logging_set_level(id, p);

    return make_error_response(id, Protocol::METHOD_NOT_FOUND, "Method not found: " + method);
}

nlohmann::json McpServer::handle_initialize(const RequestId& id, const nlohmann::json& params) {
    InitializeHandler init_handler;
    ServerCapabilities caps;
    Implementation info;

    if (frozen_.load(std::memory_order_acquire)) {
        init_handler = frozen_initialize_handler_;
        caps = capabilities_;
        info = server_info_;
    } else {
        std::shared_lock<std::shared_mutex> lock(handlers_mutex_);
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
    if (list_cache_enabled_.load(std::memory_order_relaxed) && frozen_.load(std::memory_order_acquire)) {
        std::lock_guard<std::mutex> lock(list_cache_mutex_);
        if (list_cache_.tools_list_result.has_value()) {
            return make_success_response(id, *list_cache_.tools_list_result);
        }
    }
    ToolListHandler list_handler;
    std::vector<Tool> tools;

    if (frozen_.load(std::memory_order_acquire)) {
        list_handler = frozen_tool_list_handler_;
        if (list_handler) {
            tools = list_handler();
        } else {
            for (const auto& [name, pair] : frozen_tool_handlers_) {
                tools.push_back(pair.first);
            }
        }
    } else {
        std::shared_lock<std::shared_mutex> lock(handlers_mutex_);
        list_handler = tool_list_handler_;
        if (list_handler) {
            lock.unlock();
            tools = list_handler();
        } else {
            for (const auto& [name, pair] : tool_handlers_) {
                tools.push_back(pair.first);
            }
        }
    }

    std::vector<nlohmann::json> tools_json;
    tools_json.reserve(tools.size());
    for (const auto& t : tools) {
        tools_json.emplace_back(nlohmann::json(t));
    }
    nlohmann::json result;
    result["tools"] = std::move(tools_json);
    if (list_cache_enabled_.load(std::memory_order_relaxed) && frozen_.load(std::memory_order_acquire)) {
        std::lock_guard<std::mutex> lock(list_cache_mutex_);
        list_cache_.tools_list_result = result;
    }
    return make_success_response(id, result);
}

nlohmann::json McpServer::handle_tools_call(const RequestId& id, const nlohmann::json& params, const std::string& session_id) {
    if (!params.contains("name")) {
        return make_error_response(id, Protocol::INVALID_PARAMS, "Missing 'name' parameter");
    }

    const std::string& tool_name = params["name"].get_ref<const std::string&>();
    ToolHandler handler;

    if (frozen_.load(std::memory_order_acquire)) {
        auto it = frozen_tool_handlers_.find(tool_name);
        if (it == frozen_tool_handlers_.end()) {
            return make_error_response(id, Protocol::METHOD_NOT_FOUND, "Tool not found: " + tool_name);
        }
        handler = it->second.second;
    } else {
        std::shared_lock<std::shared_mutex> lock(handlers_mutex_);
        auto it = tool_handlers_.find(tool_name);
        if (it == tool_handlers_.end()) {
            return make_error_response(id, Protocol::METHOD_NOT_FOUND, "Tool not found: " + tool_name);
        }
        handler = it->second.second;
    }

    static const nlohmann::json empty_args = nlohmann::json::object();
    const nlohmann::json& arguments = params.contains("arguments") ? params["arguments"] : empty_args;
    RequestContext ctx(*this, id, transport_, session_id);

    auto result = handler(arguments, ctx);
    return make_success_response(id, nlohmann::json(result));
}

nlohmann::json McpServer::handle_resources_list(const RequestId& id) {
    if (list_cache_enabled_.load(std::memory_order_relaxed) && frozen_.load(std::memory_order_acquire)) {
        std::lock_guard<std::mutex> lock(list_cache_mutex_);
        if (list_cache_.resources_list_result.has_value()) {
            return make_success_response(id, *list_cache_.resources_list_result);
        }
    }
    ResourceListHandler list_handler;
    std::vector<Resource> resources;

    if (frozen_.load(std::memory_order_acquire)) {
        list_handler = frozen_resource_list_handler_;
        if (!list_handler) {
            for (const auto& [uri, pair] : frozen_resource_handlers_) {
                resources.push_back(pair.first);
            }
        }
    } else {
        std::shared_lock<std::shared_mutex> lock(handlers_mutex_);
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

    std::vector<nlohmann::json> resources_json;
    resources_json.reserve(resources.size());
    for (const auto& r : resources) {
        resources_json.emplace_back(nlohmann::json(r));
    }
    nlohmann::json result;
    result["resources"] = std::move(resources_json);
    if (list_cache_enabled_.load(std::memory_order_relaxed) && frozen_.load(std::memory_order_acquire)) {
        std::lock_guard<std::mutex> lock(list_cache_mutex_);
        list_cache_.resources_list_result = result;
    }
    return make_success_response(id, result);
}

nlohmann::json McpServer::handle_resources_read(const RequestId& id, const nlohmann::json& params, const std::string& session_id) {
    if (!params.contains("uri")) {
        return make_error_response(id, Protocol::INVALID_PARAMS, "Missing 'uri' parameter");
    }

    const std::string& uri = params["uri"].get_ref<const std::string&>();
    ResourceHandler handler;

    if (frozen_.load(std::memory_order_acquire)) {
        auto it = frozen_resource_handlers_.find(uri);
        if (it == frozen_resource_handlers_.end()) {
            return make_error_response(id, Protocol::METHOD_NOT_FOUND, "Resource not found: " + uri);
        }
        handler = it->second.second;
    } else {
        std::shared_lock<std::shared_mutex> lock(handlers_mutex_);
        auto it = resource_handlers_.find(uri);
        if (it == resource_handlers_.end()) {
            return make_error_response(id, Protocol::METHOD_NOT_FOUND, "Resource not found: " + uri);
        }
        handler = it->second.second;
    }

    RequestContext ctx(*this, id, transport_, session_id);
    auto result = handler(uri, ctx);
    return make_success_response(id, nlohmann::json(result));
}

nlohmann::json McpServer::handle_resources_subscribe(const RequestId& id, const nlohmann::json& params) {
    if (!params.contains("uri")) {
        return make_error_response(id, Protocol::INVALID_PARAMS, "Missing 'uri' parameter");
    }
    const std::string& uri = params["uri"].get_ref<const std::string&>();
    {
        std::unique_lock<std::shared_mutex> lock(handlers_mutex_);
        subscribed_resources_.insert(uri);
    }
    return make_success_response(id, nlohmann::json(EmptyResult{}));
}

nlohmann::json McpServer::handle_resources_unsubscribe(const RequestId& id, const nlohmann::json& params) {
    if (!params.contains("uri")) {
        return make_error_response(id, Protocol::INVALID_PARAMS, "Missing 'uri' parameter");
    }
    const std::string& uri = params["uri"].get_ref<const std::string&>();
    {
        std::unique_lock<std::shared_mutex> lock(handlers_mutex_);
        subscribed_resources_.erase(uri);
    }
    return make_success_response(id, nlohmann::json(EmptyResult{}));
}

nlohmann::json McpServer::handle_resources_templates_list(const RequestId& id) {
    if (list_cache_enabled_.load(std::memory_order_relaxed) && frozen_.load(std::memory_order_acquire)) {
        std::lock_guard<std::mutex> lock(list_cache_mutex_);
        if (list_cache_.resource_templates_list_result.has_value()) {
            return make_success_response(id, *list_cache_.resource_templates_list_result);
        }
    }
    ResourceTemplateListHandler list_handler;

    if (frozen_.load(std::memory_order_acquire)) {
        list_handler = frozen_resource_template_list_handler_;
    } else {
        std::shared_lock<std::shared_mutex> lock(handlers_mutex_);
        list_handler = resource_template_list_handler_;
    }

    std::vector<ResourceTemplate> templates;
    if (list_handler) {
        templates = list_handler();
    }

    std::vector<nlohmann::json> templates_json;
    templates_json.reserve(templates.size());
    for (const auto& t : templates) {
        templates_json.emplace_back(nlohmann::json(t));
    }
    nlohmann::json result;
    result["resourceTemplates"] = std::move(templates_json);
    if (list_cache_enabled_.load(std::memory_order_relaxed) && frozen_.load(std::memory_order_acquire)) {
        std::lock_guard<std::mutex> lock(list_cache_mutex_);
        list_cache_.resource_templates_list_result = result;
    }
    return make_success_response(id, result);
}

nlohmann::json McpServer::handle_prompts_list(const RequestId& id) {
    if (list_cache_enabled_.load(std::memory_order_relaxed) && frozen_.load(std::memory_order_acquire)) {
        std::lock_guard<std::mutex> lock(list_cache_mutex_);
        if (list_cache_.prompts_list_result.has_value()) {
            return make_success_response(id, *list_cache_.prompts_list_result);
        }
    }
    PromptListHandler list_handler;
    std::vector<Prompt> prompts;

    if (frozen_.load(std::memory_order_acquire)) {
        list_handler = frozen_prompt_list_handler_;
        if (!list_handler) {
            for (const auto& [name, pair] : frozen_prompt_handlers_) {
                prompts.push_back(pair.first);
            }
        }
    } else {
        std::shared_lock<std::shared_mutex> lock(handlers_mutex_);
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

    std::vector<nlohmann::json> prompts_json;
    prompts_json.reserve(prompts.size());
    for (const auto& p : prompts) {
        prompts_json.emplace_back(nlohmann::json(p));
    }
    nlohmann::json result;
    result["prompts"] = std::move(prompts_json);
    if (list_cache_enabled_.load(std::memory_order_relaxed) && frozen_.load(std::memory_order_acquire)) {
        std::lock_guard<std::mutex> lock(list_cache_mutex_);
        list_cache_.prompts_list_result = result;
    }
    return make_success_response(id, result);
}

nlohmann::json McpServer::handle_prompts_get(const RequestId& id, const nlohmann::json& params, const std::string& session_id) {
    if (!params.contains("name")) {
        return make_error_response(id, Protocol::INVALID_PARAMS, "Missing 'name' parameter");
    }

    const std::string& prompt_name = params["name"].get_ref<const std::string&>();
    PromptHandler handler;

    if (frozen_.load(std::memory_order_acquire)) {
        auto it = frozen_prompt_handlers_.find(prompt_name);
        if (it == frozen_prompt_handlers_.end()) {
            return make_error_response(id, Protocol::METHOD_NOT_FOUND, "Prompt not found: " + prompt_name);
        }
        handler = it->second.second;
    } else {
        std::shared_lock<std::shared_mutex> lock(handlers_mutex_);
        auto it = prompt_handlers_.find(prompt_name);
        if (it == prompt_handlers_.end()) {
            return make_error_response(id, Protocol::METHOD_NOT_FOUND, "Prompt not found: " + prompt_name);
        }
        handler = it->second.second;
    }

    static const nlohmann::json empty_args = nlohmann::json::object();
    const nlohmann::json& arguments = params.contains("arguments") ? params["arguments"] : empty_args;
    RequestContext ctx(*this, id, transport_, session_id);

    auto result = handler(prompt_name, arguments, ctx);
    return make_success_response(id, nlohmann::json(result));
}

nlohmann::json McpServer::handle_completion_complete(const RequestId& id, const nlohmann::json& params) {
    CompletionHandler handler;

    if (frozen_.load(std::memory_order_acquire)) {
        if (!frozen_completion_handler_) {
            return make_error_response(id, Protocol::METHOD_NOT_FOUND, "Completions not supported");
        }
        handler = frozen_completion_handler_;
    } else {
        std::shared_lock<std::shared_mutex> lock(handlers_mutex_);
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

    if (frozen_.load(std::memory_order_acquire)) {
        handler = frozen_logging_level_handler_;
    } else {
        std::shared_lock<std::shared_mutex> lock(handlers_mutex_);
        handler = logging_level_handler_;
    }

    if (handler) {
        handler(level);
    }
    return make_success_response(id, nlohmann::json(EmptyResult{}));
}

// --- Notification sending ---
void McpServer::send_notification(const std::string& method, nlohmann::json params, const std::string& session_id) {
    if (!transport_ || !running_) return;
    JsonRpcNotification notif;
    notif.method = method;
    if (!params.is_null()) notif.params = std::move(params);
    try {
        if (session_id.empty()) {
            transport_->send_message(serialize_notification(notif));
        } else {
            transport_->send_to_session(session_id, serialize_notification(notif));
        }
    } catch (const std::exception&) {
        // Notification send failure is non-critical; swallow to avoid propagating to handler
    }
}

void McpServer::notify_tools_list_changed() {
    invalidate_list_cache();
    send_notification(Protocol::NOTIF_TOOLS_LIST_CHANGED);
}

void McpServer::notify_resources_list_changed() {
    invalidate_list_cache();
    send_notification(Protocol::NOTIF_RESOURCES_LIST_CHANGED);
}

void McpServer::notify_resources_updated(const std::string& uri) {
    send_notification(Protocol::NOTIF_RESOURCES_UPDATED, {{ "uri", uri }});
}

void McpServer::notify_prompts_list_changed() {
    invalidate_list_cache();
    send_notification(Protocol::NOTIF_PROMPTS_LIST_CHANGED);
}

void McpServer::notify_progress(const RequestId& progress_token, double progress,
                                 std::optional<double> total, const std::string& session_id) {
    nlohmann::json params = nlohmann::json{
        {"progressToken", request_id_to_json(progress_token)},
        {"progress", progress}
    };
    if (total) params["total"] = *total;
    send_notification(Protocol::NOTIF_PROGRESS, std::move(params), session_id);
}

void McpServer::notify_logging(const std::string& level, const std::string& data,
                                std::optional<std::string> logger, const std::string& session_id) {
    nlohmann::json params = nlohmann::json{{"level", level}, {"data", data}};
    if (logger) params["logger"] = *logger;
    send_notification(Protocol::NOTIF_MESSAGE, std::move(params), session_id);
}

} // namespace cppmcp