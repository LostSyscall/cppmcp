#include "cppmcp/server.hpp"
#include "cppmcp/transport.hpp"
#include "cppmcp/logging.hpp"

#include <algorithm>
#include <iostream>
#include <regex>

namespace cppmcp {

McpServer::McpServer(const Implementation& info, const ServerCapabilities& capabilities)
    : server_info_(info), capabilities_(capabilities),
      work_guard_(io_ctx_.get_executor()),
      signals_(io_ctx_, SIGINT, SIGTERM) {}

McpServer::~McpServer() {
    stop();
    // Drop the transport BEFORE member destruction proceeds: transport_ is
    // declared before io_ctx_, so the default (reverse) destruction order
    // would destroy io_ctx_ first and leave HttpTransport's sockets/acceptor
    // referencing a dead io_context — a crash under optimized builds.
    transport_.reset();
}

// --- internal helpers ---

void McpServer::log_(const std::string& level, const std::string& message) const {
    if (log_handler_) {
        log_handler_(level, message);
        return;
    }
    AsyncLogger::instance().log("[" + level + "] [cppmcp] " + message);
}

void McpServer::set_log_handler(LogHandler handler) {
    std::unique_lock<std::shared_mutex> lock(handlers_mutex_);
    log_handler_ = std::move(handler);
}

bool McpServer::ensure_pre_run(const char* action) {
    if (running_.load()) {
        log_("warning", std::string("Cannot ") + action + " after server is running");
        return false;
    }
    return true;
}

void McpServer::register_tool(const std::string& name, const Tool& tool_def, ToolHandler handler) {
    if (!ensure_pre_run("register handlers")) return;
    std::unique_lock<std::shared_mutex> lock(handlers_mutex_);
    if (tool_handlers_.count(name)) {
        log_("warning", "Tool '" + name + "' already registered, overwriting");
    }
    tool_handlers_[name] = {tool_def, std::move(handler)};
}

void McpServer::register_tool_list(ToolListHandler handler) {
    if (!ensure_pre_run("register handlers")) return;
    std::unique_lock<std::shared_mutex> lock(handlers_mutex_);
    tool_list_handler_ = std::move(handler);
}

void McpServer::register_resource(const std::string& uri, const Resource& resource_def, ResourceHandler handler) {
    if (!ensure_pre_run("register handlers")) return;
    std::unique_lock<std::shared_mutex> lock(handlers_mutex_);
    if (resource_handlers_.count(uri)) {
        log_("warning", "Resource '" + uri + "' already registered, overwriting");
    }
    resource_handlers_[uri] = {resource_def, std::move(handler)};
}

void McpServer::register_resource_list(ResourceListHandler handler) {
    if (!ensure_pre_run("register handlers")) return;
    std::unique_lock<std::shared_mutex> lock(handlers_mutex_);
    resource_list_handler_ = std::move(handler);
}

void McpServer::register_resource_template_list(ResourceTemplateListHandler handler) {
    if (!ensure_pre_run("register handlers")) return;
    std::unique_lock<std::shared_mutex> lock(handlers_mutex_);
    resource_template_list_handler_ = std::move(handler);
}

void McpServer::register_resource_template(const std::string& uri_template,
                                           const ResourceTemplate& template_def,
                                           ResourceTemplateHandler handler) {
    if (!ensure_pre_run("register handlers")) return;
    std::unique_lock<std::shared_mutex> lock(handlers_mutex_);
    if (resource_template_handlers_.count(uri_template)) {
        log_("warning", "Resource template '" + uri_template + "' already registered, overwriting");
    }
    ResourceTemplate def = template_def;
    def.uri_template = uri_template;  // the registered template wins
    resource_template_handlers_[uri_template] = {def, std::move(handler)};
}

void McpServer::register_prompt(const std::string& name, const Prompt& prompt_def, PromptHandler handler) {
    if (!ensure_pre_run("register handlers")) return;
    std::unique_lock<std::shared_mutex> lock(handlers_mutex_);
    prompt_handlers_[name] = {prompt_def, std::move(handler)};
}

void McpServer::register_prompt_list(PromptListHandler handler) {
    if (!ensure_pre_run("register handlers")) return;
    std::unique_lock<std::shared_mutex> lock(handlers_mutex_);
    prompt_list_handler_ = std::move(handler);
}

void McpServer::register_completion(CompletionHandler handler) {
    if (!ensure_pre_run("register handlers")) return;
    std::unique_lock<std::shared_mutex> lock(handlers_mutex_);
    completion_handler_ = std::move(handler);
}

void McpServer::register_initialize_handler(InitializeHandler handler) {
    if (!ensure_pre_run("register handlers")) return;
    std::unique_lock<std::shared_mutex> lock(handlers_mutex_);
    initialize_handler_ = std::move(handler);
}

void McpServer::register_logging_level_handler(LoggingLevelHandler handler) {
    if (!ensure_pre_run("register handlers")) return;
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
    transport_->set_error_handler([this](const std::string& err) {
        log_("error", "Transport error: " + err);
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

void McpServer::set_io_threads(std::size_t n) {
    if (running_.load(std::memory_order_relaxed)) {
        return;  // must be configured before run()
    }
    configured_io_threads_ = n;
}

void McpServer::set_list_page_size(std::size_t n) {
    list_page_size_ = n;
    invalidate_list_cache();
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
        log_("error", "No transport connected");
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
        frozen_resource_template_handlers_ = resource_template_handlers_;
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
        log_("error", std::string("Transport start failed: ") + e.what());
        running_ = false;
        frozen_.store(false, std::memory_order_release);
        work_guard_.reset();
        return;
    }

    std::vector<std::thread> io_pool;
    for (std::size_t i = 1; i < configured_io_threads_; ++i) {
        io_pool.emplace_back([this]() {
            try {
                io_ctx_.run();
            } catch (const std::exception& e) {
                // A peer tripping a streambuf hard cap mid-read throws
                // length_error from inside asio; log instead of terminating.
                log_("error", std::string("io loop died: ") + e.what());
            }
        });
    }
    try {
        io_ctx_.run();  // main thread also runs the loop
    } catch (const std::exception& e) {
        log_("error", std::string("io loop died: ") + e.what());
    }
    for (auto& t : io_pool) {
        if (t.joinable()) {
            t.join();
        }
    }
}

void McpServer::stop() {
    if (stopping_.exchange(true)) {
        return;
    }
    // frozen_ stays true: once frozen, registries never unfreeze — running
    // handlers may still read frozen copies while the loop drains to a stop.
    running_ = false;
    if (executor_) {
        executor_->stop();  // join workers before transports/loop go away
    }
    if (transport_) {
        // Detach our callbacks so any in-flight transport handler cannot reach
        // a destructing server (defensive; executor_ already joined above for
        // the deferred path).
        transport_->set_message_handler(nullptr);
        transport_->set_error_handler(nullptr);
        transport_->set_disconnect_handler(nullptr);
        transport_->stop();  // drain/close transports before stopping the loop
    }
    signals_.cancel();
    work_guard_.reset();
    io_ctx_.stop();
    {
        std::lock_guard<std::mutex> lock(outbound_mutex_);
        outbound_pending_.clear();
    }
    AsyncLogger::instance().flush();
}

namespace {
bool is_deferrable(const std::string& method) {
    return method == Protocol::METHOD_TOOLS_CALL
        || method == Protocol::METHOD_RESOURCES_READ
        || method == Protocol::METHOD_PROMPTS_GET;
}

// While dispatching a JSON-RPC batch, responses must be produced synchronously
// (the transport collects them into one array body), so deferral is disabled.
thread_local bool g_batch_dispatch = false;
}  // namespace

void McpServer::begin_batch_dispatch() { g_batch_dispatch = true; }
void McpServer::end_batch_dispatch() { g_batch_dispatch = false; }


// RFC 6570 subset: translate "weather://{city}/current" into a regex with
// named captures for each {var} (unreserved + pct-encoded chars allowed).
std::regex uri_template_to_regex(const std::string& tpl) {
    // Escape every regex metachar in literal segments: RFC 6570 literals may
    // legally contain ( ) * + ? [ ] { } | ^ $ \ — unescaped, an unbalanced
    // bracket throws regex_error and a balanced group shifts the variable
    // capture alignment (handler would receive wrong values).
    auto is_meta = [](char c) {
        return c == '.' || c == '+' || c == '*' || c == '?' ||
               c == '(' || c == ')' || c == '[' || c == ']' ||
               c == '{' || c == '}' || c == '|' || c == '^' || c == '$' || c == '\\';
    };
    std::string pattern;
    pattern += "^";
    std::string var;
    bool in_var = false;
    for (char c : tpl) {
        if (c == '{') {
            in_var = true;
            var.clear();
        } else if (c == '}') {
            if (in_var && !var.empty()) {
                pattern += "([^/\\?\\#]+)";
            }
            in_var = false;
        } else if (in_var) {
            var += c;
        } else if (is_meta(c)) {
            pattern += '\\';
            pattern += c;
        } else {
            pattern += c;
        }
    }
    pattern += "$";
    return std::regex(pattern);
}

// Typed extraction helpers: report INVALID_PARAMS instead of tripping nlohmann
// type_error (which would surface as -32603 INTERNAL_ERROR).
bool get_string_param(const nlohmann::json& params, const char* key, std::string& out, std::string& err) {
    if (!params.contains(key)) {
        err = std::string("Missing '") + key + "' parameter";
        return false;
    }
    const auto& v = params[key];
    if (!v.is_string()) {
        err = std::string("Parameter '") + key + "' must be a string";
        return false;
    }
    out = v.get<std::string>();
    return true;
}

const std::vector<std::string>& valid_log_levels() {
    static const std::vector<std::string> levels = {
        "debug", "info", "notice", "warning", "error", "critical", "alert", "emergency"
    };
    return levels;
}

// Minimal JSON Schema validation subset for tool inputSchema: supports type,
// required, properties (nested), enum, items for arrays. Returns "" on ok,
// else a human-readable violation path.
std::string schema_validate(const nlohmann::json& value, const nlohmann::json& schema, const std::string& path) {
    if (!schema.is_object()) {
        return "";  // not a schema we understand; accept
    }
    if (schema.contains("type")) {
        const auto& t = schema["type"];
        bool ok = true;
        if (t.is_string()) {
            const std::string& ts = t.get_ref<const std::string&>();
            if (ts == "object") ok = value.is_object();
            else if (ts == "array") ok = value.is_array();
            else if (ts == "string") ok = value.is_string();
            else if (ts == "number") ok = value.is_number();
            else if (ts == "integer") ok = value.is_number_integer();
            else if (ts == "boolean") ok = value.is_boolean();
            else if (ts == "null") ok = value.is_null();
        } else if (t.is_array()) {
            ok = false;
            for (const auto& alt : t) {
                std::string sub;
                if (schema_validate(value, nlohmann::json{{"type", alt}}, path).empty()) {
                    ok = true;
                    break;
                }
            }
        }
        if (!ok) {
            return path.empty() ? "value does not match schema type" : ("'" + path + "' does not match schema type");
        }
    }
    if (schema.contains("enum") && schema["enum"].is_array()) {
        bool found = false;
        for (const auto& e : schema["enum"]) {
            if (value == e) {
                found = true;
                break;
            }
        }
        if (!found) {
            return (path.empty() ? std::string("value") : ("'" + path + "'")) + " is not one of the allowed values";
        }
    }
    if (value.is_object()) {
        if (schema.contains("required") && schema["required"].is_array()) {
            for (const auto& req : schema["required"]) {
                if (req.is_string() && !value.contains(req.get_ref<const std::string&>())) {
                    return "missing required property '" + req.get_ref<const std::string&>() + "'";
                }
            }
        }
        if (schema.contains("properties") && schema["properties"].is_object()) {
            for (auto it = value.begin(); it != value.end(); ++it) {
                auto pit = schema["properties"].find(it.key());
                if (pit != schema["properties"].end()) {
                    std::string sub = schema_validate(it.value(), *pit, path.empty() ? it.key() : path + "." + it.key());
                    if (!sub.empty()) {
                        return sub;
                    }
                }
            }
        }
    }
    if (value.is_array() && schema.contains("items") && schema["items"].is_object()) {
        for (std::size_t i = 0; i < value.size(); ++i) {
            std::string sub = schema_validate(value[i], schema["items"], path + "[" + std::to_string(i) + "]");
            if (!sub.empty()) {
                return sub;
            }
        }
    }
    return "";
}

void McpServer::on_message(const nlohmann::json& message, ITransport::ResponseSink respond, const std::string& session_id) {
    auto parsed = parse_message(message);

    if (auto* req = std::get_if<JsonRpcRequest>(&parsed)) {
        if (executor_ && !g_batch_dispatch && is_deferrable(req->method)) {
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
        process_notification(*notif, session_id);
    } else if (auto* ok_resp = std::get_if<JsonRpcSuccessResponse>(&parsed)) {
        on_client_response(*ok_resp);  // answer to a server-initiated request
    } else if (auto* err_resp = std::get_if<JsonRpcErrorResponse>(&parsed)) {
        if (const auto* num = std::get_if<int64_t>(&err_resp->id)) {
            // May answer a server-initiated request rather than being a stray.
            bool is_outbound = false;
            {
                std::lock_guard<std::mutex> lock(outbound_mutex_);
                is_outbound = outbound_pending_.count(*num) > 0;
            }
            if (is_outbound) {
                on_client_error_response(*err_resp);
                return;
            }
        }
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

void McpServer::process_notification(const JsonRpcNotification& notif, const std::string& /*session_id*/) {
    if (notif.method == Protocol::NOTIF_INITIALIZED) {
        initialized_ = true;
    } else if (notif.method == Protocol::NOTIF_CANCELLED) {
        // Best-effort cancellation: parse the requestId being cancelled. C++ has
        // no thread-cancellation primitive, so a deferred handler in flight is
        // not interruptible — but we record the intent so a future cooperative
        // cancel hook can consult it. The id is logged for observability.
        std::string rid_str = "<unknown>";
        if (notif.params && notif.params->is_object() && notif.params->contains("requestId")) {
            try {
                rid_str = notif.params->at("requestId").dump();
            } catch (const std::exception&) {}
        }
        log_("info", "Received cancellation notification for requestId " + rid_str);
    }
}

void McpServer::record_client_hello(const nlohmann::json& params, const std::string& session_id) {
    ClientHello hello;
    try {
        if (params.contains("capabilities") && params["capabilities"].is_object()) {
            from_json(params["capabilities"], hello.capabilities);
        }
        if (params.contains("protocolVersion") && params["protocolVersion"].is_string()) {
            hello.protocol_version = params["protocolVersion"].get<std::string>();
        }
    } catch (const std::exception&) {
        // Malformed capabilities: proceed with empty ones (no capability-gated features).
    }
    std::unique_lock<std::shared_mutex> lock(handlers_mutex_);
    client_hellos_[session_id] = hello;
}

std::string McpServer::negotiated_version() const {
    std::shared_lock<std::shared_mutex> lock(handlers_mutex_);
    return negotiated_version_;
}

nlohmann::json McpServer::dispatch_method(const std::string& method, const RequestId& id,
                                           const std::optional<nlohmann::json>& params,
                                           const std::string& session_id) {
    static const nlohmann::json empty_params = nlohmann::json::object();
    const nlohmann::json& p = params.value_or(empty_params);

    if (method == Protocol::METHOD_INITIALIZE) {
        return handle_initialize(id, p, session_id);
    }
    if (method == Protocol::METHOD_PING) {
        return handle_ping(id);
    }

    // Must be initialized before calling other methods
    if (!initialized_) {
        return make_error_response(id, Protocol::SERVER_NOT_INITIALIZED,
                                   "Server not initialized. Send initialize request first.");
    }

    if (method == Protocol::METHOD_TOOLS_LIST) return handle_tools_list(id, p);
    if (method == Protocol::METHOD_TOOLS_CALL) return handle_tools_call(id, p, session_id);
    if (method == Protocol::METHOD_RESOURCES_LIST) return handle_resources_list(id, p);
    if (method == Protocol::METHOD_RESOURCES_READ) return handle_resources_read(id, p, session_id);
    if (method == Protocol::METHOD_RESOURCES_SUBSCRIBE) return handle_resources_subscribe(id, p, session_id);
    if (method == Protocol::METHOD_RESOURCES_UNSUBSCRIBE) return handle_resources_unsubscribe(id, p, session_id);
    if (method == Protocol::METHOD_RESOURCES_TEMPLATE_LIST) return handle_resources_templates_list(id, p);
    if (method == Protocol::METHOD_PROMPTS_LIST) return handle_prompts_list(id, p);
    if (method == Protocol::METHOD_PROMPTS_GET) return handle_prompts_get(id, p, session_id);
    if (method == Protocol::METHOD_COMPLETION_COMPLETE) return handle_completion_complete(id, p);
    if (method == Protocol::METHOD_LOGGING_SET_LEVEL) return handle_logging_set_level(id, p);

    return make_error_response(id, Protocol::METHOD_NOT_FOUND, "Method not found: " + method);
}

nlohmann::json McpServer::handle_initialize(const RequestId& id, const nlohmann::json& params, const std::string& session_id) {
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

    record_client_hello(params, session_id);

    // protocolVersion negotiation: echo the client's version when supported;
    // otherwise respond with our latest supported version. An explicit custom
    // handler may override (it receives the raw params).
    std::string requested;
    if (params.contains("protocolVersion") && params["protocolVersion"].is_string()) {
        requested = params["protocolVersion"].get<std::string>();
    }
    std::string negotiated = Protocol::is_supported_version(requested)
                                 ? requested
                                 : Protocol::DEFAULT_NEGOTIATED_VERSION;
    {
        std::unique_lock<std::shared_mutex> lock(handlers_mutex_);
        negotiated_version_ = negotiated;
    }

    if (init_handler) {
        auto result = init_handler(params);
        if (result.protocol_version.empty()) {
            result.protocol_version = negotiated;
        }
        return make_success_response(id, nlohmann::json(result));
    }

    InitializeResult result;
    result.protocol_version = negotiated;
    result.capabilities = caps;
    result.server_info = info;
    return make_success_response(id, nlohmann::json(result));
}

nlohmann::json McpServer::handle_ping(const RequestId& id) {
    return make_success_response(id, nlohmann::json(EmptyResult{}));
}

// Unified list-request flow: optional cache hit -> produce items -> cache fill.
// produce_items builds the per-element JSON array (without the wrapper key);
// this wraps it as {result_key: [...]} and applies the cache under one lock.
nlohmann::json McpServer::handle_list_request(
        const RequestId& id,
        std::optional<nlohmann::json> ListCache::*cache_field,
        const char* result_key,
        const nlohmann::json& params,
        std::function<std::vector<nlohmann::json>()> produce_items) {
    // Pagination applies only when a page size is configured; the cursor is an
    // opaque offset when it is all digits.
    std::size_t offset = 0;
    bool paginated = list_page_size_ > 0;
    if (paginated && params.contains("cursor") && params["cursor"].is_string()) {
        const std::string& cur = params["cursor"].get_ref<const std::string&>();
        if (!cur.empty() && cur.find_first_not_of("0123456789") == std::string::npos) {
            offset = static_cast<std::size_t>(std::stoull(cur));
        } else if (!cur.empty()) {
            return make_error_response(id, Protocol::INVALID_PARAMS, "Invalid cursor");
        }
    }

    bool cacheable = list_cache_enabled_.load(std::memory_order_relaxed) &&
                     frozen_.load(std::memory_order_acquire) && !paginated;
    if (cacheable) {
        std::lock_guard<std::mutex> lock(list_cache_mutex_);
        if ((list_cache_.*cache_field).has_value()) {
            return make_success_response(id, *(list_cache_.*cache_field));
        }
    }
    std::vector<nlohmann::json> items = produce_items();
    nlohmann::json result;
    if (paginated) {
        std::size_t end = (std::min)(offset + list_page_size_, items.size());
        std::vector<nlohmann::json> page(items.begin() + (std::min)(offset, items.size()), items.begin() + end);
        result[result_key] = std::move(page);
        if (end < items.size()) {
            result["nextCursor"] = std::to_string(end);
        }
        return make_success_response(id, result);
    }
    result[result_key] = std::move(items);
    if (cacheable) {
        std::lock_guard<std::mutex> lock(list_cache_mutex_);
        list_cache_.*cache_field = result;
    }
    return make_success_response(id, result);
}

nlohmann::json McpServer::handle_tools_list(const RequestId& id, const nlohmann::json& params) {
    return handle_list_request(id, &ListCache::tools_list_result, "tools", params, [this]() {
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
        std::vector<nlohmann::json> out;
        out.reserve(tools.size());
        for (const auto& t : tools) out.emplace_back(nlohmann::json(t));
        return out;
    });
}

nlohmann::json McpServer::handle_tools_call(const RequestId& id, const nlohmann::json& params, const std::string& session_id) {
    std::string tool_name;
    std::string err;
    if (!get_string_param(params, "name", tool_name, err)) {
        return make_error_response(id, Protocol::INVALID_PARAMS, err);
    }
    ToolHandler handler;
    Tool tool_def;

    if (frozen_.load(std::memory_order_acquire)) {
        auto it = frozen_tool_handlers_.find(tool_name);
        if (it == frozen_tool_handlers_.end()) {
            return make_error_response(id, Protocol::METHOD_NOT_FOUND, "Tool not found: " + tool_name);
        }
        handler = it->second.second;
        tool_def = it->second.first;
    } else {
        std::shared_lock<std::shared_mutex> lock(handlers_mutex_);
        auto it = tool_handlers_.find(tool_name);
        if (it == tool_handlers_.end()) {
            return make_error_response(id, Protocol::METHOD_NOT_FOUND, "Tool not found: " + tool_name);
        }
        handler = it->second.second;
        tool_def = it->second.first;
    }

    static const nlohmann::json empty_args = nlohmann::json::object();
    const nlohmann::json& arguments = params.contains("arguments") && params["arguments"].is_object()
                                          ? params["arguments"]
                                          : empty_args;

    // inputSchema subset validation (type/required/enum/items) before invoking
    // the handler, so bad params fail as -32602 instead of inside user code.
    if (tool_def.input_schema.is_object()) {
        std::string verr = schema_validate(arguments, tool_def.input_schema, "");
        if (!verr.empty()) {
            return make_error_response(id, Protocol::INVALID_PARAMS,
                                       "Invalid arguments for tool '" + tool_name + "': " + verr);
        }
    }

    // Honor the client's _meta.progressToken (fall back to the request id).
    RequestId progress_token = id;
    if (params.contains("_meta") && params["_meta"].is_object() &&
        params["_meta"].contains("progressToken") && !params["_meta"]["progressToken"].is_null()) {
        try {
            from_json(params["_meta"]["progressToken"], progress_token);
        } catch (const std::exception&) {
            // unusable token: fall back to request id
        }
    }
    RequestContext ctx(*this, id, transport_, session_id);
    ctx.set_progress_token(progress_token);

    auto result = handler(arguments, ctx);
    return make_success_response(id, nlohmann::json(result));
}

nlohmann::json McpServer::handle_resources_list(const RequestId& id, const nlohmann::json& params) {
    return handle_list_request(id, &ListCache::resources_list_result, "resources", params, [this]() {
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
        std::vector<nlohmann::json> out;
        out.reserve(resources.size());
        for (const auto& r : resources) out.emplace_back(nlohmann::json(r));
        return out;
    });
}

nlohmann::json McpServer::handle_resources_read(const RequestId& id, const nlohmann::json& params, const std::string& session_id) {
    std::string uri;
    std::string err;
    if (!get_string_param(params, "uri", uri, err)) {
        return make_error_response(id, Protocol::INVALID_PARAMS, err);
    }
    ResourceHandler handler;

    if (frozen_.load(std::memory_order_acquire)) {
        auto it = frozen_resource_handlers_.find(uri);
        if (it != frozen_resource_handlers_.end()) {
            handler = it->second.second;
        }
    } else {
        std::shared_lock<std::shared_mutex> lock(handlers_mutex_);
        auto it = resource_handlers_.find(uri);
        if (it != resource_handlers_.end()) {
            handler = it->second.second;
        }
    }

    RequestContext ctx(*this, id, transport_, session_id);

    if (handler) {
        auto result = handler(uri, ctx);
        return make_success_response(id, nlohmann::json(result));
    }

    // No exact registration: try URI templates (most specific first: longer
    // literal prefixes win, which map ordering approximates).
    if (frozen_.load(std::memory_order_acquire)) {
        for (const auto& [tpl, pair] : frozen_resource_template_handlers_) {
            std::regex re = uri_template_to_regex(tpl);
            std::smatch match;
            if (std::regex_match(uri, match, re)) {
                std::map<std::string, std::string> variables;
                // Re-extract variable names in order (regex captures align).
                std::string var;
                std::vector<std::string> names;
                for (char c : tpl) {
                    if (c == '{') {
                        var.clear();
                    } else if (c == '}') {
                        if (!var.empty()) names.push_back(var);
                        var.clear();
                    } else if (!var.empty() || std::isalnum(static_cast<unsigned char>(c)) || c == '_') {
                        var += c;
                    }
                }
                for (std::size_t i = 0; i + 1 < match.size() && i < names.size(); ++i) {
                    variables[names[i]] = match[i + 1].str();
                }
                auto result = pair.second(uri, variables, ctx);
                return make_success_response(id, nlohmann::json(result));
            }
        }
    } else {
        std::shared_lock<std::shared_mutex> lock(handlers_mutex_);
        for (const auto& [tpl, pair] : resource_template_handlers_) {
            std::regex re = uri_template_to_regex(tpl);
            std::smatch match;
            if (std::regex_match(uri, match, re)) {
                std::map<std::string, std::string> variables;
                std::string var;
                std::vector<std::string> names;
                for (char c : tpl) {
                    if (c == '{') {
                        var.clear();
                    } else if (c == '}') {
                        if (!var.empty()) names.push_back(var);
                        var.clear();
                    } else if (!var.empty() || std::isalnum(static_cast<unsigned char>(c)) || c == '_') {
                        var += c;
                    }
                }
                for (std::size_t i = 0; i + 1 < match.size() && i < names.size(); ++i) {
                    variables[names[i]] = match[i + 1].str();
                }
                auto result = pair.second(uri, variables, ctx);
                return make_success_response(id, nlohmann::json(result));
            }
        }
    }

    return make_error_response(id, Protocol::METHOD_NOT_FOUND, "Resource not found: " + uri);
}

nlohmann::json McpServer::handle_resources_subscribe(const RequestId& id, const nlohmann::json& params, const std::string& session_id) {
    // Capability gate: reject subscriptions when we don't advertise subscribe.
    if (!capabilities_.resources || !capabilities_.resources->subscribe) {
        return make_error_response(id, Protocol::METHOD_NOT_FOUND, "Resource subscriptions not supported");
    }
    std::string uri;
    std::string err;
    if (!get_string_param(params, "uri", uri, err)) {
        return make_error_response(id, Protocol::INVALID_PARAMS, err);
    }
    {
        std::unique_lock<std::shared_mutex> lock(handlers_mutex_);
        subscribed_resources_[session_id].insert(uri);
    }
    return make_success_response(id, nlohmann::json(EmptyResult{}));
}

nlohmann::json McpServer::handle_resources_unsubscribe(const RequestId& id, const nlohmann::json& params, const std::string& session_id) {
    std::string uri;
    std::string err;
    if (!get_string_param(params, "uri", uri, err)) {
        return make_error_response(id, Protocol::INVALID_PARAMS, err);
    }
    {
        std::unique_lock<std::shared_mutex> lock(handlers_mutex_);
        auto it = subscribed_resources_.find(session_id);
        if (it != subscribed_resources_.end()) {
            it->second.erase(uri);
        }
    }
    return make_success_response(id, nlohmann::json(EmptyResult{}));
}

nlohmann::json McpServer::handle_resources_templates_list(const RequestId& id, const nlohmann::json& params) {
    return handle_list_request(id, &ListCache::resource_templates_list_result, "resourceTemplates", params, [this]() {
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
        } else {
            // Advertise registered resource templates (register_resource_template).
            if (frozen_.load(std::memory_order_acquire)) {
                for (const auto& [tpl, pair] : frozen_resource_template_handlers_) {
                    templates.push_back(pair.first);
                }
            } else {
                std::shared_lock<std::shared_mutex> lock(handlers_mutex_);
                for (const auto& [tpl, pair] : resource_template_handlers_) {
                    templates.push_back(pair.first);
                }
            }
        }
        std::vector<nlohmann::json> out;
        out.reserve(templates.size());
        for (const auto& t : templates) out.emplace_back(nlohmann::json(t));
        return out;
    });
}

nlohmann::json McpServer::handle_prompts_list(const RequestId& id, const nlohmann::json& params) {
    return handle_list_request(id, &ListCache::prompts_list_result, "prompts", params, [this]() {
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
        std::vector<nlohmann::json> out;
        out.reserve(prompts.size());
        for (const auto& p : prompts) out.emplace_back(nlohmann::json(p));
        return out;
    });
}

nlohmann::json McpServer::handle_prompts_get(const RequestId& id, const nlohmann::json& params, const std::string& session_id) {
    std::string prompt_name;
    std::string err;
    if (!get_string_param(params, "name", prompt_name, err)) {
        return make_error_response(id, Protocol::INVALID_PARAMS, err);
    }
    PromptHandler handler;
    Prompt prompt_def;

    if (frozen_.load(std::memory_order_acquire)) {
        auto it = frozen_prompt_handlers_.find(prompt_name);
        if (it == frozen_prompt_handlers_.end()) {
            return make_error_response(id, Protocol::METHOD_NOT_FOUND, "Prompt not found: " + prompt_name);
        }
        handler = it->second.second;
        prompt_def = it->second.first;
    } else {
        std::shared_lock<std::shared_mutex> lock(handlers_mutex_);
        auto it = prompt_handlers_.find(prompt_name);
        if (it == prompt_handlers_.end()) {
            return make_error_response(id, Protocol::METHOD_NOT_FOUND, "Prompt not found: " + prompt_name);
        }
        handler = it->second.second;
        prompt_def = it->second.first;
    }

    static const nlohmann::json empty_args = nlohmann::json::object();
    const nlohmann::json& arguments = params.contains("arguments") && params["arguments"].is_object()
                                          ? params["arguments"]
                                          : empty_args;

    // Validate required prompt arguments per the registered Prompt definition.
    if (prompt_def.arguments) {
        for (const auto& arg : *prompt_def.arguments) {
            if (arg.required && (!arguments.contains(arg.name) || arguments[arg.name].is_null())) {
                return make_error_response(id, Protocol::INVALID_PARAMS,
                                           "Missing required argument '" + arg.name + "' for prompt '" + prompt_name + "'");
            }
        }
    }

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
    if (params.contains("ref") && params["ref"].is_object()) {
        const auto& r = params["ref"];
        if (r.contains("type") && r["type"].is_string()) ref.type = r["type"].get<std::string>();
        if (r.contains("name") && r["name"].is_string()) ref.name = r["name"].get<std::string>();
    }
    CompletionArgument arg;
    if (params.contains("argument") && params["argument"].is_object()) {
        const auto& a = params["argument"];
        if (a.contains("name") && a["name"].is_string()) arg.name = a["name"].get<std::string>();
        if (a.contains("value") && a["value"].is_string()) arg.value = a["value"].get<std::string>();
    }

    auto result = handler(ref, arg);
    return make_success_response(id, nlohmann::json(result));
}

nlohmann::json McpServer::handle_logging_set_level(const RequestId& id, const nlohmann::json& params) {
    std::string level;
    std::string err;
    if (!get_string_param(params, "level", level, err)) {
        return make_error_response(id, Protocol::INVALID_PARAMS, err);
    }
    const auto& levels = valid_log_levels();
    if (std::find(levels.begin(), levels.end(), level) == levels.end()) {
        return make_error_response(id, Protocol::INVALID_PARAMS, "Invalid log level: " + level);
    }
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
    // Gate on stopping_ (not running_): handler-driven notifications must flow
    // in unit tests that drive the transport directly without McpServer::run().
    if (!transport_ || stopping_.load()) return;
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
    nlohmann::json params{{"uri", uri}};
    // Route to sessions that subscribed to this URI. When no session ever
    // subscribed (stdio/pipe, or HTTP clients that never called subscribe),
    // broadcast — preserving single-client semantics.
    std::vector<std::string> targets;
    {
        std::shared_lock<std::shared_mutex> lock(handlers_mutex_);
        for (const auto& [sid, uris] : subscribed_resources_) {
            if (uris.count(uri)) {
                targets.push_back(sid);
            }
        }
    }
    if (targets.empty()) {
        send_notification(Protocol::NOTIF_RESOURCES_UPDATED, std::move(params));
        return;
    }
    for (const auto& sid : targets) {
        send_notification(Protocol::NOTIF_RESOURCES_UPDATED, params, sid);
    }
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

// --- server -> client requests ---

bool McpServer::send_server_request(const std::string& method, const nlohmann::json& params,
                                    std::function<void(const nlohmann::json&)> on_result,
                                    std::function<void(const McpException&)> on_error,
                                    const std::string& session_id) {
    if (!transport_ || !running_) {
        return false;
    }
    int64_t rid = next_outbound_id_.fetch_add(1);
    {
        std::lock_guard<std::mutex> lock(outbound_mutex_);
        outbound_pending_[rid] = {method, session_id, std::move(on_result), std::move(on_error)};
    }
    JsonRpcRequest req;
    req.id = rid;
    req.method = method;
    req.params = params;
    try {
        transport_->send_to_session(session_id, serialize_request(req));
    } catch (const std::exception& e) {
        std::lock_guard<std::mutex> lock(outbound_mutex_);
        outbound_pending_.erase(rid);
        log_("error", std::string("Failed to send server request: ") + e.what());
        return false;
    }
    return true;
}

void McpServer::complete_server_request(int64_t id, bool ok, const nlohmann::json& payload) {
    OutboundPending p;
    {
        std::lock_guard<std::mutex> lock(outbound_mutex_);
        auto it = outbound_pending_.find(id);
        if (it == outbound_pending_.end()) {
            return;
        }
        p = std::move(it->second);
        outbound_pending_.erase(it);
    }
    try {
        if (ok) {
            if (p.on_result) {
                p.on_result(payload);
            }
        } else {
            int code = Protocol::INTERNAL_ERROR;
            std::string msg = "client error";
            if (payload.is_object()) {
                code = payload.value("code", Protocol::INTERNAL_ERROR);
                msg = payload.value("message", msg);
            }
            if (p.on_error) {
                p.on_error(McpException(code, msg));
            }
        }
    } catch (const std::exception& e) {
        log_("error", std::string("server-request callback threw: ") + e.what());
    } catch (...) {
        log_("error", "server-request callback threw an unknown exception");
    }
}

void McpServer::on_client_response(const JsonRpcSuccessResponse& resp) {
    if (const auto* num = std::get_if<int64_t>(&resp.id)) {
        complete_server_request(*num, true, resp.result);
    }
}

void McpServer::on_client_error_response(const JsonRpcErrorResponse& resp) {
    if (const auto* num = std::get_if<int64_t>(&resp.id)) {
        nlohmann::json err;
        err["code"] = resp.error.code;
        err["message"] = resp.error.message;
        if (resp.error.data) {
            err["data"] = *resp.error.data;
        }
        complete_server_request(*num, false, err);
    }
}

void McpServer::fail_expired_server_requests() {
    // No deadlines are tracked for outbound requests in this version; the
    // client eventually answers or disconnects. Hook kept for symmetry.
}

bool McpServer::request_sampling(const CreateMessageRequestParams& params,
                                 SamplingResultHandler on_result, SamplingErrorHandler on_error,
                                 const std::string& session_id) {
    std::string sid = session_id;
    if (sid.empty()) {
        std::shared_lock<std::shared_mutex> lock(handlers_mutex_);
        auto it = client_hellos_.find("");
        if (it != client_hellos_.end() && !it->second.capabilities.sampling) {
            return false;
        }
    } else {
        std::shared_lock<std::shared_mutex> lock(handlers_mutex_);
        auto it = client_hellos_.find(sid);
        if (it == client_hellos_.end() || !it->second.capabilities.sampling) {
            return false;
        }
    }
    nlohmann::json p;
    to_json(p, params);
    return send_server_request(Protocol::METHOD_SAMPLING_CREATE, p,
        [on_result](const nlohmann::json& j) {
            if (on_result) {
                try {
                    CreateMessageResult r;
                    from_json(j, r);
                    on_result(r);
                } catch (const std::exception&) {
                    // malformed client response: dropped
                }
            }
        },
        [on_error](const McpException& e) {
            if (on_error) on_error(e);
        },
        sid);
}

bool McpServer::request_elicitation(const ElicitRequestParams& params,
                                    ElicitationResultHandler on_result, ElicitationErrorHandler on_error,
                                    const std::string& session_id) {
    std::string sid = session_id;
    if (sid.empty()) {
        std::shared_lock<std::shared_mutex> lock(handlers_mutex_);
        auto it = client_hellos_.find("");
        if (it != client_hellos_.end() && !it->second.capabilities.elicitation) {
            return false;
        }
    } else {
        std::shared_lock<std::shared_mutex> lock(handlers_mutex_);
        auto it = client_hellos_.find(sid);
        if (it == client_hellos_.end() || !it->second.capabilities.elicitation) {
            return false;
        }
    }
    nlohmann::json p;
    to_json(p, params);
    return send_server_request(Protocol::METHOD_ELICITATION_CREATE, p,
        [on_result](const nlohmann::json& j) {
            if (on_result) {
                try {
                    ElicitResult r;
                    from_json(j, r);
                    on_result(r);
                } catch (const std::exception&) {
                }
            }
        },
        [on_error](const McpException& e) {
            if (on_error) on_error(e);
        },
        sid);
}

bool McpServer::request_roots(RootsResultHandler on_result, RootsErrorHandler on_error,
                              const std::string& session_id) {
    std::string sid = session_id;
    if (sid.empty()) {
        std::shared_lock<std::shared_mutex> lock(handlers_mutex_);
        auto it = client_hellos_.find("");
        if (it != client_hellos_.end() && !it->second.capabilities.roots) {
            return false;
        }
    } else {
        std::shared_lock<std::shared_mutex> lock(handlers_mutex_);
        auto it = client_hellos_.find(sid);
        if (it == client_hellos_.end() || !it->second.capabilities.roots) {
            return false;
        }
    }
    return send_server_request(Protocol::METHOD_ROOTS_LIST, nlohmann::json::object(),
        [on_result](const nlohmann::json& j) {
            if (on_result) {
                try {
                    ListRootsResult r;
                    from_json(j, r);
                    on_result(r);
                } catch (const std::exception&) {
                }
            }
        },
        [on_error](const McpException& e) {
            if (on_error) on_error(e);
        },
        sid);
}

void McpServer::notify_roots_list_changed(const std::string& session_id) {
    send_notification(Protocol::NOTIF_ROOTS_LIST_CHANGED, nlohmann::json::object(), session_id);
}

} // namespace cppmcp
