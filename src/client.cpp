#include "cppmcp/client.hpp"

#include "cppmcp/jsonrpc.hpp"
#include "cppmcp/logging.hpp"
#include "cppmcp/protocol.hpp"

#include <utility>

namespace cppmcp {

// ============================ RequestBuilder ============================

std::shared_ptr<PendingRequest> RequestBuilder::send() {
    return client_->submit_request(method_, std::move(params_),
                                   std::move(on_complete_), std::move(on_error_),
                                   std::move(on_progress_), timeout_, progress_token_);
}

// ============================ McpClient ============================

McpClient::McpClient(const Implementation& client_info, const ClientCapabilities& capabilities)
    : client_info_(client_info), capabilities_(capabilities) {
    init_owned_io();
}

McpClient::~McpClient() {
    stop();
}

void McpClient::init_owned_io() {
    owned_io_.emplace();
    io_ptr_ = &(*owned_io_);
    work_guard_ = std::make_unique<work_guard_t>(io_ptr_->get_executor());
    strand_ = std::make_unique<strand_t>(asio::make_strand(*io_ptr_));
}

void McpClient::set_io_context(asio::io_context* io_ctx) {
    if (running_.load() || !io_ctx) {
        return;
    }
    owns_io_context_ = false;
    owned_io_.reset();
    io_ptr_ = io_ctx;
    work_guard_.reset();
    strand_ = std::make_unique<strand_t>(asio::make_strand(*io_ptr_));
}

void McpClient::set_worker_threads(std::size_t n) {
    configured_workers_ = n;
}

void McpClient::set_callback_executor(asio::any_io_executor executor) {
    callback_executor_ = std::move(executor);
}

void McpClient::use_transport(std::shared_ptr<IClientTransport> transport) {
    transport_ = std::move(transport);
}

void McpClient::ensure_executor() {
    if (configured_workers_ > 0 && !executor_) {
        executor_ = std::make_unique<RequestExecutor>(configured_workers_);
        executor_->start();
    }
}

void McpClient::ensure_started() {
    if (running_.exchange(true)) {
        return;
    }
    ensure_executor();
    attach_transport_handlers();
    if (owns_io_context_) {
        auto self = shared_from_this();
        io_thread_ = std::thread([self]() {
            asio::error_code ec;
            self->io_ptr_->run(ec);
        });
    }
}

// (Re)arm the transport callbacks. Transports clear their handlers on
// disconnect(), so every connect() — first or reconnect — must re-attach them
// or every inbound message would be silently dropped.
void McpClient::attach_transport_handlers() {
    if (!transport_) {
        return;
    }
    transport_->set_io_context(io_ptr_);
    auto self = shared_from_this();
    transport_->set_message_handler([self](const nlohmann::json& msg) {
        asio::post(*self->strand_, [self, msg]() { self->on_transport_message(msg); });
    });
    transport_->set_error_handler([self](const std::string& err) {
        AsyncLogger::instance().log("McpClient transport error: " + err);
    });
    transport_->set_disconnect_handler([self]() {
        asio::post(*self->strand_, [self]() { self->on_transport_disconnect(); });
    });
}

nlohmann::json McpClient::make_initialize_params() {
    nlohmann::json caps_json;
    to_json(caps_json, capabilities_);
    nlohmann::json info_json;
    to_json(info_json, client_info_);
    return nlohmann::json{
        {"protocolVersion", Protocol::LATEST_PROTOCOL_VERSION},
        {"capabilities", std::move(caps_json)},
        {"clientInfo", std::move(info_json)}
    };
}

InitializeResult McpClient::connect(std::chrono::milliseconds timeout) {
    if (!transport_) {
        throw McpException(Protocol::CLIENT_NOT_CONNECTED, "no transport attached");
    }
    ensure_started();
    attach_transport_handlers();  // transports clear handlers on disconnect
    transport_->connect();
    auto pr = prepare(Protocol::METHOD_INITIALIZE, make_initialize_params())
                  .timeout(timeout)
                  .send();
    try {
        pr->get();
    } catch (...) {
        // Handshake failed (error/timeout/disconnect): roll back transport state
        // so a later connect() can start clean, then rethrow.
        connected_.store(false);
        transport_->disconnect();
        throw;
    }
    if (!server_result_) {
        connected_.store(false);
        transport_->disconnect();
        throw McpException(Protocol::INTERNAL_ERROR, "initialize completed but server result missing");
    }
    return *server_result_;
}

std::shared_ptr<PendingRequest> McpClient::async_connect(std::chrono::milliseconds timeout) {
    if (!transport_) {
        throw McpException(Protocol::CLIENT_NOT_CONNECTED, "no transport attached");
    }
    ensure_started();
    attach_transport_handlers();
    transport_->connect();
    return prepare(Protocol::METHOD_INITIALIZE, make_initialize_params()).timeout(timeout).send();
}

bool McpClient::has_capability(const std::string& cap) const {
    if (!server_result_) {
        return false;
    }
    const auto& c = server_result_->capabilities;
    if (cap == "tools") return static_cast<bool>(c.tools);
    if (cap == "resources") return static_cast<bool>(c.resources);
    if (cap == "prompts") return static_cast<bool>(c.prompts);
    if (cap == "logging") return static_cast<bool>(c.logging);
    if (cap == "completions") return static_cast<bool>(c.completions);
    return false;
}

RequestBuilder McpClient::prepare(const std::string& method, std::optional<nlohmann::json> params) {
    return RequestBuilder(this, method, std::move(params));
}

void McpClient::send_notification(const std::string& method, nlohmann::json params) {
    JsonRpcNotification notif;
    notif.method = method;
    notif.params = std::move(params);
    raw_send(serialize_notification(notif));
}

void McpClient::raw_send(nlohmann::json message) {
    if (transport_) {
        transport_->send_message(message);
    }
}

std::shared_ptr<PendingRequest> McpClient::submit_request(
    const std::string& method,
    std::optional<nlohmann::json> params,
    CompleteCallback on_complete,
    OutcomeCallback on_error,
    ProgressCallback on_progress,
    std::chrono::milliseconds timeout,
    std::optional<RequestId> progress_token_opt) {

    // Fast-fail on a dead connection instead of queueing into a void.
    if (!connected_.load() && method != Protocol::METHOD_INITIALIZE) {
        throw McpException(Protocol::CLIENT_NOT_CONNECTED,
                           "client is not connected (connect() first)");
    }
    if (timeout.count() <= 0 && default_timeout_.count() > 0) {
        timeout = default_timeout_;
    }

    int64_t num = next_id_.fetch_add(1);
    RequestId id = num;
    RequestId token = NullId{};
    if (on_progress) {
        token = progress_token_opt ? *progress_token_opt : RequestId(num);
        // Custom tokens must be strings: pending_map_ is keyed by RequestId in
        // one flat space, and a numeric custom token could collide with another
        // request's int64 id (silently detaching it from progress delivery).
        if (progress_token_opt && std::holds_alternative<int64_t>(*progress_token_opt)) {
            token = RequestId(num);
        }
        if (!params) {
            params = nlohmann::json::object();
        }
        if (!params->contains("_meta") || !(*params)["_meta"].is_object()) {
            (*params)["_meta"] = nlohmann::json::object();
        }
        // The token written to params must match the value keyed in pending_map_,
        // so a string/custom token round-trips correctly through server progress notifications.
        (*params)["_meta"]["progressToken"] = request_id_to_json(token);
    } else if (progress_token_opt) {
        token = *progress_token_opt;
    }

    auto pr = std::shared_ptr<PendingRequest>(new PendingRequest(id, method, token));
    pr->client_ = weak_from_this();
    pr->on_complete_ = std::move(on_complete);
    pr->on_error_ = std::move(on_error);
    pr->on_progress_ = std::move(on_progress);
    pr->timeout_ = timeout;
    if (callback_executor_) {
        pr->callback_executor_ = *callback_executor_;
    }

    auto self = shared_from_this();
    asio::post(*strand_, [self, pr, params = std::move(params), method, id]() {
        self->register_pending(pr);
        if (pr->timeout_.count() > 0) {
            pr->arm_timer(*self->strand_);
        }
        JsonRpcRequest jr;
        jr.id = id;
        jr.method = method;
        jr.params = params;
        self->raw_send(serialize_request(jr));
    });
    return pr;
}

void McpClient::register_pending(std::shared_ptr<PendingRequest> pr) {
    pending_map_[pr->id] = pr;
    if (!is_null_id(pr->progress_token) && !(pr->progress_token == pr->id)) {
        pending_map_[pr->progress_token] = pr;
    }
}

void McpClient::complete_pending(const RequestId& id, McpOutcome outcome) {
    auto it = pending_map_.find(id);
    if (it == pending_map_.end()) {
        return;
    }
    auto pr = it->second;
    if (outcome.terminal == RequestState::Succeeded && pr->method == Protocol::METHOD_INITIALIZE) {
        bool init_ok = true;
        try {
            InitializeResult ir;
            from_json(outcome.result, ir);
            check_negotiated_version(outcome.result);
            server_result_ = ir;
            connected_.store(true);
            initialized_.store(true);
        } catch (const McpException&) {
            // Unsupported negotiated version: fail the handshake instead of
            // silently continuing with mismatched protocol assumptions.
            init_ok = false;
        } catch (const std::exception& e) {
            AsyncLogger::instance().log(std::string("initialize result parse failed: ") + e.what());
            init_ok = false;
        }
        if (!init_ok) {
            pending_map_.erase(it);
            if (!is_null_id(pr->progress_token) && !(pr->progress_token == pr->id)) {
                pending_map_.erase(pr->progress_token);
            }
            McpOutcome fail;
            fail.terminal = RequestState::Errored;
            fail.rpc_error = JsonRpcErrorDetail{Protocol::INVALID_REQUEST,
                                                "server negotiated an unsupported protocolVersion"};
            pr->finish(std::move(fail));
            return;
        }
        JsonRpcNotification n;
        n.method = Protocol::NOTIF_INITIALIZED;
        raw_send(serialize_notification(n));
    }
    pending_map_.erase(it);
    if (!is_null_id(pr->progress_token) && !(pr->progress_token == pr->id)) {
        pending_map_.erase(pr->progress_token);
    }
    pr->finish(std::move(outcome));
}

void McpClient::check_negotiated_version(const nlohmann::json& result) {
    if (!result.contains("protocolVersion") || !result["protocolVersion"].is_string()) {
        throw McpException(Protocol::INVALID_REQUEST, "server response missing protocolVersion");
    }
    std::string v = result["protocolVersion"].get<std::string>();
    if (!Protocol::is_supported_version(v)) {
        throw McpException(Protocol::INVALID_REQUEST,
                           "server negotiated unsupported protocolVersion: " + v);
    }
}

void McpClient::request_cancel(const RequestId& id, std::string reason) {
    auto self = shared_from_this();
    asio::post(*strand_, [self, id, reason = std::move(reason)]() {
        auto it = self->pending_map_.find(id);
        if (it == self->pending_map_.end()) {
            return;
        }
        auto pr = it->second;
        if (pr->state() != RequestState::Waiting) {
            return;
        }
        nlohmann::json cancel_params = nlohmann::json{
            {"requestId", request_id_to_json(id)},
            {"reason", reason}
        };
        JsonRpcNotification n;
        n.method = Protocol::NOTIF_CANCELLED;
        n.params = std::move(cancel_params);
        self->raw_send(serialize_notification(n));
        McpOutcome o;
        o.terminal = RequestState::Cancelled;
        o.failure = McpException(Protocol::REQUEST_CANCELLED,
                                 reason.empty() ? std::string("cancelled by client") : reason);
        self->complete_pending(id, std::move(o));
    });
}

void McpClient::fail_all_pending(const std::string& reason) {
    // A pending request may be keyed twice in pending_map_ (once by id, once by
    // progress_token when they differ). finish() is idempotent (its state_
    // CAS guards re-entry), so calling it twice on the same pr is a no-op.
    for (auto& kv : pending_map_) {
        auto& pr = kv.second;
        if (pr->state() != RequestState::Waiting) {
            continue;
        }
        McpOutcome o;
        o.terminal = RequestState::Failed;
        o.failure = McpException(Protocol::REQUEST_FAILED, reason);
        pr->finish(std::move(o));
    }
    pending_map_.clear();
}

void McpClient::on_transport_disconnect() {
    connected_.store(false);
    fail_all_pending("transport disconnected");
    ClientDisconnectHandler h;
    {
        std::shared_lock<std::shared_mutex> lock(inbound_mutex_);
        h = disconnect_handler_;
    }
    if (h) {
        try {
            h();
        } catch (const std::exception& e) {
            AsyncLogger::instance().log(std::string("disconnect handler threw: ") + e.what());
        } catch (...) {
            AsyncLogger::instance().log("disconnect handler threw an unknown exception");
        }
    }
}

void McpClient::on_transport_message(nlohmann::json message) {
    ClientParsedMessage parsed;
    try {
        parsed = parse_message_client(message);
    } catch (const std::exception& e) {
        AsyncLogger::instance().log(std::string("McpClient message parse error: ") + e.what());
        return;
    }
    if (auto* req = std::get_if<JsonRpcRequest>(&parsed)) {
        handle_inbound_request(std::move(*req));
    } else if (auto* notif = std::get_if<JsonRpcNotification>(&parsed)) {
        handle_notification(*notif);
    } else if (auto* ok = std::get_if<JsonRpcSuccessResponse>(&parsed)) {
        McpOutcome o;
        o.terminal = RequestState::Succeeded;
        o.result = ok->result;
        complete_pending(ok->id, std::move(o));
    } else if (auto* err = std::get_if<JsonRpcErrorResponse>(&parsed)) {
        McpOutcome o;
        o.terminal = RequestState::Errored;
        o.rpc_error = err->error;
        complete_pending(err->id, std::move(o));
    }
}

void McpClient::handle_notification(const JsonRpcNotification& notif) {
    if (notif.method == Protocol::NOTIF_PROGRESS) {
        const auto& p = notif.params.value_or(nlohmann::json::object());
        if (!p.contains("progressToken")) {
            return;
        }
        RequestId token;
        try {
            from_json(p.at("progressToken"), token);
        } catch (const std::exception&) {
            return;
        }
        double progress = p.value("progress", 0.0);
        std::optional<double> total;
        if (p.contains("total")) {
            total = p.at("total").get<double>();
        }
        auto it = pending_map_.find(token);
        if (it != pending_map_.end()) {
            it->second->deliver_progress(progress, total);
        }
        return;
    }
    // Server push notifications -> optional user callbacks. A throwing user
    // callback must not take down the io thread.
    try {
        if (notif.method == Protocol::NOTIF_RESOURCES_UPDATED) {
            ResourceUpdatedHandler h;
            {
                std::shared_lock<std::shared_mutex> lock(inbound_mutex_);
                h = resource_updated_handler_;
            }
            if (h) {
                const auto& p = notif.params.value_or(nlohmann::json::object());
                h(p.value("uri", std::string{}));
            }
        } else if (notif.method == Protocol::NOTIF_TOOLS_LIST_CHANGED ||
                   notif.method == Protocol::NOTIF_RESOURCES_LIST_CHANGED ||
                   notif.method == Protocol::NOTIF_PROMPTS_LIST_CHANGED) {
            ListChangedHandler h;
            {
                std::shared_lock<std::shared_mutex> lock(inbound_mutex_);
                h = list_changed_handler_;
            }
            if (h) {
                std::string kind = notif.method == Protocol::NOTIF_TOOLS_LIST_CHANGED ? "tools"
                    : notif.method == Protocol::NOTIF_RESOURCES_LIST_CHANGED ? "resources"
                    : "prompts";
                h(kind);
            }
        } else if (notif.method == Protocol::NOTIF_MESSAGE) {
            ServerLogHandler h;
            {
                std::shared_lock<std::shared_mutex> lock(inbound_mutex_);
                h = server_log_handler_;
            }
            if (h) {
                LoggingMessageNotificationParams msg;
                const auto& p = notif.params.value_or(nlohmann::json::object());
                msg.level = p.value("level", std::string{"info"});
                msg.data = p.value("data", std::string{});
                if (p.contains("logger") && p["logger"].is_string()) {
                    msg.logger = p["logger"].get<std::string>();
                }
                h(msg);
            }
        } else if (notif.method == Protocol::NOTIF_ROOTS_LIST_CHANGED) {
            RootsChangedHandler h;
            {
                std::shared_lock<std::shared_mutex> lock(inbound_mutex_);
                h = roots_changed_handler_;
            }
            if (h) {
                h();
            }
        }
    } catch (const std::exception& e) {
        AsyncLogger::instance().log(std::string("notification handler threw: ") + e.what());
    } catch (...) {
        AsyncLogger::instance().log("notification handler threw an unknown exception");
    }
}

void McpClient::handle_inbound_request(JsonRpcRequest req) {
    if (req.method == Protocol::METHOD_PING) {
        raw_send(make_success_response(req.id, nlohmann::json::object()));
        return;
    }
    if (req.method == Protocol::METHOD_SAMPLING_CREATE) {
        SamplingHandler h;
        {
            std::shared_lock<std::shared_mutex> lock(inbound_mutex_);
            h = sampling_handler_;
        }
        auto run = [h](const nlohmann::json& params) -> nlohmann::json {
            if (!h) {
                throw MethodNotFoundError(Protocol::METHOD_SAMPLING_CREATE);
            }
            CreateMessageRequestParams p;
            from_json(params, p);
            CreateMessageResult r = h(p);
            nlohmann::json j;
            to_json(j, r);
            return j;
        };
        dispatch_inbound_request(std::move(req), std::move(run));
        return;
    }
    if (req.method == Protocol::METHOD_ELICITATION_CREATE) {
        ElicitationHandler h;
        {
            std::shared_lock<std::shared_mutex> lock(inbound_mutex_);
            h = elicitation_handler_;
        }
        auto run = [h](const nlohmann::json& params) -> nlohmann::json {
            if (!h) {
                throw MethodNotFoundError(Protocol::METHOD_ELICITATION_CREATE);
            }
            ElicitRequestParams p;
            from_json(params, p);
            ElicitResult r = h(p);
            nlohmann::json j;
            to_json(j, r);
            return j;
        };
        dispatch_inbound_request(std::move(req), std::move(run));
        return;
    }
    if (req.method == Protocol::METHOD_ROOTS_LIST) {
        RootsHandler h;
        {
            std::shared_lock<std::shared_mutex> lock(inbound_mutex_);
            h = roots_handler_;
        }
        auto run = [h](const nlohmann::json& /*params*/) -> nlohmann::json {
            if (!h) {
                throw MethodNotFoundError(Protocol::METHOD_ROOTS_LIST);
            }
            ListRootsResult r = h();
            nlohmann::json j;
            to_json(j, r);
            return j;
        };
        dispatch_inbound_request(std::move(req), std::move(run));
        return;
    }
    raw_send(make_error_response(req.id, Protocol::METHOD_NOT_FOUND, "Method not found: " + req.method));
}

void McpClient::dispatch_inbound_request(JsonRpcRequest req, std::function<nlohmann::json(const nlohmann::json&)> run) {
    nlohmann::json params = req.params.value_or(nlohmann::json::object());
    RequestId id = req.id;
    bool use_pool = static_cast<bool>(executor_) &&
                    (req.method == Protocol::METHOD_SAMPLING_CREATE ||
                     req.method == Protocol::METHOD_ELICITATION_CREATE ||
                     req.method == Protocol::METHOD_ROOTS_LIST);

    auto run_and_respond = [self = shared_from_this(), params = std::move(params),
                            run = std::move(run), id]() {
        nlohmann::json result;
        try {
            result = run(params);
        } catch (const McpException& e) {
            asio::post(*self->strand_, [self, id, code = e.code(), msg = e.message(), data = e.data()]() {
                self->raw_send(make_error_response(id, code, msg, data));
            });
            return;
        } catch (const std::exception& e) {
            asio::post(*self->strand_, [self, id, msg = std::string(e.what())]() {
                self->raw_send(make_error_response(id, Protocol::INTERNAL_ERROR, msg));
            });
            return;
        } catch (...) {
            asio::post(*self->strand_, [self, id]() {
                self->raw_send(make_error_response(id, Protocol::INTERNAL_ERROR, "unknown handler exception"));
            });
            return;
        }
        asio::post(*self->strand_, [self, id, result = std::move(result)]() {
            self->raw_send(make_success_response(id, std::move(result)));
        });
    };

    if (use_pool) {
        executor_->post(std::move(run_and_respond));
    } else {
        run_and_respond();
    }
}

// ============================ shutdown ============================

// Drain pending requests on the strand and return. If we cannot obtain a
// shared_ptr to ourselves (destruction in progress) or cannot post (no usable
// io loop), fall back to a synchronous fail. This avoids bad_weak_ptr from
// shared_from_this() when stop()/disconnect() runs from ~McpClient.
void McpClient::drain_pending(const std::string& reason) {
    if (stopping_.load() || !io_ptr_) {
        // Already stopped / never started — fail inline if anything is pending.
        connected_.store(false);
        fail_all_pending(reason);
        return;
    }
    // Inline-drain when we ARE on a strand/io thread: blocking on a posted
    // task here would deadlock the only thread that can run it. This covers
    // both the self-owned io thread and an external io thread that is
    // currently executing our strand (e.g. a user callback calling stop()).
    bool on_io_thread = (owns_io_context_ && io_thread_.joinable() &&
                         std::this_thread::get_id() == io_thread_.get_id()) ||
                        strand_->running_in_this_thread();
    if (on_io_thread) {
        connected_.store(false);
        fail_all_pending(reason);
        return;
    }
    auto self = weak_from_this().lock();
    std::promise<void> drained;
    auto f = drained.get_future();
    if (self) {
        asio::post(*strand_, [self, &drained, &reason]() {
            self->connected_.store(false);
            self->fail_all_pending(reason);
            drained.set_value();
        });
    } else {
        // Destructing: cannot post (io loop may be gone). Fail inline.
        connected_.store(false);
        fail_all_pending(reason);
        return;
    }
    if (owns_io_context_) {
        f.get();
    } else {
        // External io: best-effort wait; the owner must keep it running.
        if (f.wait_for(std::chrono::seconds(5)) == std::future_status::ready) {
            f.get();
        }
    }
}

void McpClient::disconnect() {
    if (transport_) {
        transport_->disconnect();
    }
    drain_pending("client disconnected");
}

void McpClient::stop() {
    if (stopping_.exchange(true)) {
        return;
    }

    if (transport_) {
        transport_->disconnect();
    }

    drain_pending("client shutting down");

    if (executor_) {
        executor_->stop();
    }

    work_guard_.reset();

    if (owns_io_context_ && io_thread_.joinable() &&
        std::this_thread::get_id() != io_thread_.get_id()) {
        io_thread_.join();
    }

    transport_.reset();
    running_.store(false);
    AsyncLogger::instance().flush();
}

// ============================ handler registration ============================

void McpClient::register_sampling_handler(SamplingHandler handler) {
    std::unique_lock<std::shared_mutex> lock(inbound_mutex_);
    sampling_handler_ = std::move(handler);
}

void McpClient::register_elicitation_handler(ElicitationHandler handler) {
    std::unique_lock<std::shared_mutex> lock(inbound_mutex_);
    elicitation_handler_ = std::move(handler);
}

void McpClient::register_roots_handler(RootsHandler handler) {
    std::unique_lock<std::shared_mutex> lock(inbound_mutex_);
    roots_handler_ = std::move(handler);
}

void McpClient::on_disconnect(ClientDisconnectHandler handler) {
    std::unique_lock<std::shared_mutex> lock(inbound_mutex_);
    disconnect_handler_ = std::move(handler);
}

void McpClient::on_resource_updated(ResourceUpdatedHandler handler) {
    std::unique_lock<std::shared_mutex> lock(inbound_mutex_);
    resource_updated_handler_ = std::move(handler);
}

void McpClient::on_list_changed(ListChangedHandler handler) {
    std::unique_lock<std::shared_mutex> lock(inbound_mutex_);
    list_changed_handler_ = std::move(handler);
}

void McpClient::on_server_log(ServerLogHandler handler) {
    std::unique_lock<std::shared_mutex> lock(inbound_mutex_);
    server_log_handler_ = std::move(handler);
}

void McpClient::on_roots_changed(RootsChangedHandler handler) {
    std::unique_lock<std::shared_mutex> lock(inbound_mutex_);
    roots_changed_handler_ = std::move(handler);
}

// ============================ convenience sync API ============================

namespace {
// Extract the nextCursor of a list response, if any.
std::string take_next_cursor(const nlohmann::json& result) {
    if (result.contains("nextCursor") && result["nextCursor"].is_string()) {
        return result["nextCursor"].get<std::string>();
    }
    return std::string{};
}

nlohmann::json list_params(std::optional<std::string> cursor) {
    if (!cursor) {
        return nlohmann::json::object();
    }
    return nlohmann::json{{"cursor", *cursor}};
}
}  // namespace

std::vector<Tool> McpClient::list_tools(std::optional<std::string> cursor, std::string* next_cursor) {
    auto result = prepare(Protocol::METHOD_TOOLS_LIST, list_params(cursor)).send()->get();
    if (next_cursor) {
        *next_cursor = take_next_cursor(result);
    }
    std::vector<Tool> tools;
    if (result.contains("tools")) {
        result.at("tools").get_to(tools);
    }
    return tools;
}

CallToolResult McpClient::call_tool(const std::string& name, const nlohmann::json& arguments) {
    nlohmann::json params = nlohmann::json{{"name", name}, {"arguments", arguments}};
    auto result = prepare(Protocol::METHOD_TOOLS_CALL, std::move(params)).send()->get();
    CallToolResult r;
    from_json(result, r);
    return r;
}

std::vector<Resource> McpClient::list_resources(std::optional<std::string> cursor, std::string* next_cursor) {
    auto result = prepare(Protocol::METHOD_RESOURCES_LIST, list_params(cursor)).send()->get();
    if (next_cursor) {
        *next_cursor = take_next_cursor(result);
    }
    std::vector<Resource> v;
    if (result.contains("resources")) {
        result.at("resources").get_to(v);
    }
    return v;
}

ReadResourceResult McpClient::read_resource(const std::string& uri) {
    nlohmann::json params = nlohmann::json{{"uri", uri}};
    auto result = prepare(Protocol::METHOD_RESOURCES_READ, std::move(params)).send()->get();
    ReadResourceResult r;
    from_json(result, r);
    return r;
}

std::vector<ResourceTemplate> McpClient::list_resource_templates(std::optional<std::string> cursor, std::string* next_cursor) {
    auto result = prepare(Protocol::METHOD_RESOURCES_TEMPLATE_LIST, list_params(cursor)).send()->get();
    if (next_cursor) {
        *next_cursor = take_next_cursor(result);
    }
    std::vector<ResourceTemplate> v;
    if (result.contains("resourceTemplates")) {
        result.at("resourceTemplates").get_to(v);
    }
    return v;
}

std::vector<Prompt> McpClient::list_prompts(std::optional<std::string> cursor, std::string* next_cursor) {
    auto result = prepare(Protocol::METHOD_PROMPTS_LIST, list_params(cursor)).send()->get();
    if (next_cursor) {
        *next_cursor = take_next_cursor(result);
    }
    std::vector<Prompt> v;
    if (result.contains("prompts")) {
        result.at("prompts").get_to(v);
    }
    return v;
}

GetPromptResult McpClient::get_prompt(const std::string& name, const nlohmann::json& arguments) {
    nlohmann::json params = nlohmann::json{{"name", name}, {"arguments", arguments}};
    auto result = prepare(Protocol::METHOD_PROMPTS_GET, std::move(params)).send()->get();
    GetPromptResult r;
    from_json(result, r);
    return r;
}

void McpClient::set_logging_level(const std::string& level) {
    nlohmann::json params = nlohmann::json{{"level", level}};
    prepare(Protocol::METHOD_LOGGING_SET_LEVEL, std::move(params)).send()->get();
}

CompleteResult McpClient::complete(const CompletionReference& ref, const CompletionArgument& argument) {
    nlohmann::json params = nlohmann::json{
        {"ref", {{"type", ref.type}, {"name", ref.name}}},
        {"argument", {{"name", argument.name}, {"value", argument.value}}}
    };
    auto result = prepare(Protocol::METHOD_COMPLETION_COMPLETE, std::move(params)).send()->get();
    CompleteResult r;
    nlohmann::json sub = result.contains("completion") ? result["completion"] : nlohmann::json::object();
    if (sub.contains("values")) sub.at("values").get_to(r.completion.values);
    if (sub.contains("hasMore")) sub.at("hasMore").get_to(r.completion.has_more.emplace());
    if (sub.contains("total")) sub.at("total").get_to(r.completion.total.emplace());
    return r;
}

void McpClient::subscribe_resource(const std::string& uri) {
    nlohmann::json params = nlohmann::json{{"uri", uri}};
    prepare(Protocol::METHOD_RESOURCES_SUBSCRIBE, std::move(params)).send()->get();
}

void McpClient::unsubscribe_resource(const std::string& uri) {
    nlohmann::json params = nlohmann::json{{"uri", uri}};
    prepare(Protocol::METHOD_RESOURCES_UNSUBSCRIBE, std::move(params)).send()->get();
}

void McpClient::ping() {
    prepare(Protocol::METHOD_PING).send()->get();
}

} // namespace cppmcp
