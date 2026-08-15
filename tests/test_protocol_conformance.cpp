// Regression tests for the 2026-08 protocol-conformance hardening pass:
// version negotiation, param validation, progressToken handling, resource
// templates, subscribe routing, pagination, crash vectors, capability gating.
#include <gtest/gtest.h>

#include <cppmcp/protocol.hpp>
#include <cppmcp/server.hpp>
#include <cppmcp/transport.hpp>
#include <cppmcp/types.hpp>

#include <sstream>

using namespace cppmcp;
using json = nlohmann::json;

namespace {

class ConfTransport : public ITransport {
public:
    void start() override { running_ = true; }
    void stop() override { running_ = false; }
    bool is_running() const override { return running_; }

    void send_message(const json& message) override {
        std::lock_guard<std::mutex> lock(write_mutex_);
        output_ << message.dump() << std::endl;
    }
    // Session-routed capture: send_to_session appends to per-session streams so
    // subscription-routing tests can assert which session saw a notification.
    void send_to_session(const std::string& sid, const json& message) override {
        std::lock_guard<std::mutex> lock(write_mutex_);
        sessions_[sid] << message.dump() << std::endl;
        output_ << message.dump() << std::endl;
    }

    void set_message_handler(MessageCallback handler) override { message_handler_ = std::move(handler); }
    void set_error_handler(ErrorCallback handler) override { error_handler_ = std::move(handler); }
    void set_io_context(asio::io_context*) override {}

    std::string get_output() { return output_.str(); }
    std::string get_session_output(const std::string& sid) { return sessions_[sid].str(); }
    void clear_output() {
        std::lock_guard<std::mutex> lock(write_mutex_);
        output_.str("");
        output_.clear();
    }

    MessageCallback& handler() { return message_handler_; }

    void deliver(const json& msg, const std::string& sid = "") {
        if (message_handler_) {
            message_handler_(msg, [this](const json& resp) { send_message(resp); }, sid);
        }
    }

private:
    std::stringstream output_;
    std::map<std::string, std::stringstream> sessions_;
    std::atomic<bool> running_{false};
    std::mutex write_mutex_;
    MessageCallback message_handler_;
    ErrorCallback error_handler_;
};

json init_params(const std::string& version = Protocol::LATEST_PROTOCOL_VERSION) {
    return json{
        {"protocolVersion", version},
        {"capabilities", json::object()},
        {"clientInfo", {"name", "t", "version", "1"}}
    };
}

json parse_first_line(const std::string& out) {
    return json::parse(out.substr(0, out.find('\n')));
}

json nth_line(const std::string& out, std::size_t n) {
    std::size_t pos = 0;
    for (std::size_t i = 0; i < n; ++i) {
        pos = out.find('\n', pos) + 1;
    }
    return json::parse(out.substr(pos, out.find('\n', pos) - pos));
}

}  // namespace

// --- Version negotiation ---

TEST(Conformance, InitializeEchoesSupportedVersion) {
    McpServer server(Implementation{"s", "1"});
    auto t = std::make_shared<ConfTransport>();
    server.connect(t);
    t->deliver(json{{"jsonrpc", "2.0"}, {"id", 1}, {"method", "initialize"},
                    {"params", init_params("2025-03-26")}});
    EXPECT_EQ(parse_first_line(t->get_output())["result"]["protocolVersion"], "2025-03-26");
}

TEST(Conformance, InitializeDowngradesUnknownVersion) {
    McpServer server(Implementation{"s", "1"});
    auto t = std::make_shared<ConfTransport>();
    server.connect(t);
    t->deliver(json{{"jsonrpc", "2.0"}, {"id", 1}, {"method", "initialize"},
                    {"params", init_params("1999-01-01")}});
    EXPECT_EQ(parse_first_line(t->get_output())["result"]["protocolVersion"],
              Protocol::LATEST_PROTOCOL_VERSION);
    EXPECT_EQ(server.negotiated_version(), Protocol::LATEST_PROTOCOL_VERSION);
}

// --- Parameter validation -> INVALID_PARAMS (-32602), not -32603 ---

TEST(Conformance, ToolCallNameTypeErrorIsInvalidParams) {
    McpServer server(Implementation{"s", "1"});
    Tool tool;
    tool.name = "echo";
    tool.input_schema = json{{"type", "object"}};
    server.register_tool("echo", tool, [](const json&, RequestContext&) {
        return CallToolResult{};
    });
    auto t = std::make_shared<ConfTransport>();
    server.connect(t);
    t->deliver(json{{"jsonrpc", "2.0"}, {"id", 1}, {"method", "initialize"}, {"params", init_params()}});
    t->deliver(json{{"jsonrpc", "2.0"}, {"method", "notifications/initialized"}});
    t->clear_output();

    t->deliver(json{{"jsonrpc", "2.0"}, {"id", 2}, {"method", "tools/call"},
                    {"params", {{"name", 123}}}});
    EXPECT_EQ(parse_first_line(t->get_output())["error"]["code"], Protocol::INVALID_PARAMS);
}

TEST(Conformance, ToolInputSchemaRequiredValidated) {
    McpServer server(Implementation{"s", "1"});
    Tool tool;
    tool.name = "echo";
    tool.input_schema = json::parse(
        R"({"type":"object","properties":{"msg":{"type":"string"}},"required":["msg"]})");
    server.register_tool("echo", tool, [](const json&, RequestContext&) {
        return CallToolResult{};
    });
    auto t = std::make_shared<ConfTransport>();
    server.connect(t);
    t->deliver(json{{"jsonrpc", "2.0"}, {"id", 1}, {"method", "initialize"}, {"params", init_params()}});
    t->deliver(json{{"jsonrpc", "2.0"}, {"method", "notifications/initialized"}});
    t->clear_output();

    // missing required property
    t->deliver(json{{"jsonrpc", "2.0"}, {"id", 2}, {"method", "tools/call"},
                    {"params", {{"name", "echo"}, {"arguments", json::object()}}}});
    EXPECT_EQ(parse_first_line(t->get_output())["error"]["code"], Protocol::INVALID_PARAMS);

    // wrong property type
    t->clear_output();
    t->deliver(json{{"jsonrpc", "2.0"}, {"id", 3}, {"method", "tools/call"},
                    {"params", {{"name", "echo"}, {"arguments", {{"msg", 42}}}}}});
    EXPECT_EQ(parse_first_line(t->get_output())["error"]["code"], Protocol::INVALID_PARAMS);
}

TEST(Conformance, PromptRequiredArgumentValidated) {
    McpServer server(Implementation{"s", "1"});
    Prompt p;
    p.name = "greet";
    PromptArgument arg;
    arg.name = "who";
    arg.required = true;
    p.arguments = std::vector<PromptArgument>{arg};
    server.register_prompt("greet", p, [](const std::string&, const json&, RequestContext&) {
        return GetPromptResult{};
    });
    auto t = std::make_shared<ConfTransport>();
    server.connect(t);
    t->deliver(json{{"jsonrpc", "2.0"}, {"id", 1}, {"method", "initialize"}, {"params", init_params()}});
    t->deliver(json{{"jsonrpc", "2.0"}, {"method", "notifications/initialized"}});
    t->clear_output();

    t->deliver(json{{"jsonrpc", "2.0"}, {"id", 2}, {"method", "prompts/get"},
                    {"params", {{"name", "greet"}, {"arguments", json::object()}}}});
    EXPECT_EQ(parse_first_line(t->get_output())["error"]["code"], Protocol::INVALID_PARAMS);
}

// --- Crash vectors: malformed sub-fields must not assert/UB ---

TEST(Conformance, CompletionMalformedRefNoCrash) {
    McpServer server(Implementation{"s", "1"});
    server.register_completion([](const CompletionReference&, const CompletionArgument&) {
        CompleteResult r;
        r.completion.values = {"x"};
        return r;
    });
    auto t = std::make_shared<ConfTransport>();
    server.connect(t);
    t->deliver(json{{"jsonrpc", "2.0"}, {"id", 1}, {"method", "initialize"}, {"params", init_params()}});
    t->deliver(json{{"jsonrpc", "2.0"}, {"method", "notifications/initialized"}});
    t->clear_output();

    // Previously: const operator[] on missing "type" -> JSON_ASSERT / UB.
    t->deliver(json{{"jsonrpc", "2.0"}, {"id", 2}, {"method", "completion/complete"},
                    {"params", {{"ref", json::object()}}}});
    EXPECT_EQ(parse_first_line(t->get_output())["result"]["completion"]["values"][0], "x");
}

TEST(Conformance, SetLevelInvalidValueRejected) {
    McpServer server(Implementation{"s", "1"});
    auto t = std::make_shared<ConfTransport>();
    server.connect(t);
    t->deliver(json{{"jsonrpc", "2.0"}, {"id", 1}, {"method", "initialize"}, {"params", init_params()}});
    t->deliver(json{{"jsonrpc", "2.0"}, {"method", "notifications/initialized"}});
    t->clear_output();

    t->deliver(json{{"jsonrpc", "2.0"}, {"id", 2}, {"method", "logging/setLevel"},
                    {"params", {{"level", "chatty"}}}});
    EXPECT_EQ(parse_first_line(t->get_output())["error"]["code"], Protocol::INVALID_PARAMS);
}

// --- progressToken: the client's token must be echoed in progress notifications ---

TEST(Conformance, ProgressTokenFromMetaEchoed) {
    McpServer server(Implementation{"s", "1"});
    Tool tool;
    tool.name = "slow";
    tool.input_schema = json{{"type", "object"}};
    server.register_tool("slow", tool, [](const json&, RequestContext& ctx) {
        ctx.report_progress(0.5, 1.0);
        CallToolResult r;
        r.content.push_back(TextContent{"text", "done"});
        return r;
    });
    auto t = std::make_shared<ConfTransport>();
    server.connect(t);
    t->deliver(json{{"jsonrpc", "2.0"}, {"id", 1}, {"method", "initialize"}, {"params", init_params()}});
    t->deliver(json{{"jsonrpc", "2.0"}, {"method", "notifications/initialized"}});
    t->clear_output();

    // Custom STRING token (as a third-party SDK would send) — previously the
    // server always used the request id, so the client could never match it.
    t->deliver(json{{"jsonrpc", "2.0"},
                    {"id", 2},
                    {"method", "tools/call"},
                    {"params", {{"name", "slow"},
                                {"_meta", {{"progressToken", "tok-abc"}}}}}});
    // Line 0: the progress notification; line 1: the tool result.
    json progress = parse_first_line(t->get_output());
    EXPECT_EQ(progress["method"], "notifications/progress");
    EXPECT_EQ(progress["params"]["progressToken"], "tok-abc");
    json result = nth_line(t->get_output(), 1);
    EXPECT_EQ(result["id"], 2);
}

// --- Resource templates (RFC 6570 subset matching) ---

TEST(Conformance, ResourceTemplateMatchAndVariables) {
    McpServer server(Implementation{"s", "1"});
    ServerCapabilities caps;
    ResourcesCapability rc;
    rc.subscribe = true;
    caps.resources = rc;
    McpServer capped(Implementation{"s", "1"}, caps);

    ResourceTemplate tpl;
    tpl.name = "weather";
    tpl.uri_template = "weather://{city}/current";
    capped.register_resource_template("weather://{city}/current", tpl,
        [](const std::string& uri,
           const std::map<std::string, std::string>& vars,
           RequestContext&) {
            ReadResourceResult r;
            ResourceContents c;
            c.uri = uri;
            c.text = "city=" + vars.at("city");
            r.contents.push_back(c);
            return r;
        });
    auto t = std::make_shared<ConfTransport>();
    capped.connect(t);
    t->deliver(json{{"jsonrpc", "2.0"}, {"id", 1}, {"method", "initialize"}, {"params", init_params()}});
    t->deliver(json{{"jsonrpc", "2.0"}, {"method", "notifications/initialized"}});
    t->clear_output();

    t->deliver(json{{"jsonrpc", "2.0"}, {"id", 2}, {"method", "resources/read"},
                    {"params", {{"uri", "weather://beijing/current"}}}});
    json resp = parse_first_line(t->get_output());
    ASSERT_TRUE(resp.contains("result")) << resp.dump();
    EXPECT_EQ(resp["result"]["contents"][0]["text"], "city=beijing");

    // Non-matching URI -> METHOD_NOT_FOUND
    t->clear_output();
    t->deliver(json{{"jsonrpc", "2.0"}, {"id", 3}, {"method", "resources/read"},
                    {"params", {{"uri", "weather://beijing/forecast"}}}});
    EXPECT_EQ(parse_first_line(t->get_output())["error"]["code"], Protocol::METHOD_NOT_FOUND);

    // Templates are advertised via resources/templates/list
    t->clear_output();
    t->deliver(json{{"jsonrpc", "2.0"}, {"id", 4}, {"method", "resources/templates/list"}});
    json tl = parse_first_line(t->get_output());
    ASSERT_EQ(tl["result"]["resourceTemplates"].size(), 1u);
    EXPECT_EQ(tl["result"]["resourceTemplates"][0]["uriTemplate"], "weather://{city}/current");
}

// --- resources/subscribe: capability-gated + per-session routing ---

TEST(Conformance, SubscribeGatedByCapability) {
    McpServer server(Implementation{"s", "1"});  // no resources capability
    auto t = std::make_shared<ConfTransport>();
    server.connect(t);
    t->deliver(json{{"jsonrpc", "2.0"}, {"id", 1}, {"method", "initialize"}, {"params", init_params()}});
    t->deliver(json{{"jsonrpc", "2.0"}, {"method", "notifications/initialized"}});
    t->clear_output();

    t->deliver(json{{"jsonrpc", "2.0"}, {"id", 2}, {"method", "resources/subscribe"},
                    {"params", {{"uri", "file:///a"}}}});
    EXPECT_EQ(parse_first_line(t->get_output())["error"]["code"], Protocol::METHOD_NOT_FOUND);
}

TEST(Conformance, NotifyResourcesUpdatedRoutesToSubscribers) {
    ServerCapabilities caps;
    ResourcesCapability rc;
    rc.subscribe = true;
    caps.resources = rc;
    McpServer server(Implementation{"s", "1"}, caps);
    auto t = std::make_shared<ConfTransport>();
    server.connect(t);

    t->deliver(json{{"jsonrpc", "2.0"}, {"id", 1}, {"method", "initialize"}, {"params", init_params()}}, "sess-A");
    t->deliver(json{{"jsonrpc", "2.0"}, {"method", "notifications/initialized"}}, "sess-A");
    t->deliver(json{{"jsonrpc", "2.0"}, {"id", 2}, {"method", "initialize"}, {"params", init_params()}}, "sess-B");
    t->deliver(json{{"jsonrpc", "2.0"}, {"method", "notifications/initialized"}}, "sess-B");

    // Only A subscribes to the resource.
    t->deliver(json{{"jsonrpc", "2.0"}, {"id", 3}, {"method", "resources/subscribe"},
                    {"params", {{"uri", "file:///x"}}}}, "sess-A");
    t->clear_output();

    server.notify_resources_updated("file:///x");

    std::string a = t->get_session_output("sess-A");
    std::string b = t->get_session_output("sess-B");
    EXPECT_NE(a.find("notifications/resources/updated"), std::string::npos)
        << "subscriber must receive the update";
    EXPECT_EQ(b.find("notifications/resources/updated"), std::string::npos)
        << "non-subscriber must NOT receive the update";
}

// --- Pagination ---

TEST(Conformance, ListPagination) {
    McpServer server(Implementation{"s", "1"});
    Tool tool;
    tool.name = "t";
    tool.input_schema = json{{"type", "object"}};
    for (int i = 0; i < 5; ++i) {
        Tool t2 = tool;
        t2.name = "tool" + std::to_string(i);
        server.register_tool(t2.name, t2, [](const json&, RequestContext&) {
            return CallToolResult{};
        });
    }
    server.set_list_page_size(2);
    auto t = std::make_shared<ConfTransport>();
    server.connect(t);
    t->deliver(json{{"jsonrpc", "2.0"}, {"id", 1}, {"method", "initialize"}, {"params", init_params()}});
    t->deliver(json{{"jsonrpc", "2.0"}, {"method", "notifications/initialized"}});

    t->clear_output();
    t->deliver(json{{"jsonrpc", "2.0"}, {"id", 2}, {"method", "tools/list"}});
    json page1 = parse_first_line(t->get_output());
    ASSERT_EQ(page1["result"]["tools"].size(), 2u);
    EXPECT_EQ(page1["result"]["nextCursor"], "2");

    t->clear_output();
    t->deliver(json{{"jsonrpc", "2.0"}, {"id", 3}, {"method", "tools/list"},
                    {"params", {{"cursor", "2"}}}});
    json page2 = parse_first_line(t->get_output());
    ASSERT_EQ(page2["result"]["tools"].size(), 2u);
    EXPECT_EQ(page2["result"]["nextCursor"], "4");

    t->clear_output();
    t->deliver(json{{"jsonrpc", "2.0"}, {"id", 4}, {"method", "tools/list"},
                    {"params", {{"cursor", "4"}}}});
    json page3 = parse_first_line(t->get_output());
    ASSERT_EQ(page3["result"]["tools"].size(), 1u);
    EXPECT_FALSE(page3["result"].contains("nextCursor"));
}

// --- JSON-RPC envelope: id:null on requests is rejected ---

TEST(Conformance, NullRequestIdRejected) {
    McpServer server(Implementation{"s", "1"});
    auto t = std::make_shared<ConfTransport>();
    server.connect(t);
    t->deliver(json{{"jsonrpc", "2.0"}, {"id", nullptr}, {"method", "ping"}});
    json resp = parse_first_line(t->get_output());
    EXPECT_TRUE(resp.contains("error"));
    EXPECT_EQ(resp["error"]["code"], Protocol::INVALID_REQUEST);
}

// --- server -> client requests: capability gating ---

TEST(Conformance, SamplingGatedByClientCapability) {
    McpServer server(Implementation{"s", "1"});
    auto t = std::make_shared<ConfTransport>();
    server.connect(t);
    t->deliver(json{{"jsonrpc", "2.0"}, {"id", 1}, {"method", "initialize"},
                    {"params", {{"protocolVersion", Protocol::LATEST_PROTOCOL_VERSION},
                                {"capabilities", json::object()},
                                {"clientInfo", {"name", "t", "version", "1"}}}}});
    // Client did NOT declare sampling -> request_sampling must refuse.
    CreateMessageRequestParams p;
    SamplingMessage m;
    m.role = "user";
    m.content = TextContent{"text", "hi"};
    p.messages.push_back(m);
    p.max_tokens = 10;
    EXPECT_FALSE(server.request_sampling(p, nullptr, nullptr));

    // Now the client answers a roots request.
    json roots_req = json{{"jsonrpc", "2.0"}, {"id", 77}, {"method", "roots/list"},
                          {"params", json::object()}};
    // A client that declared roots would answer:
    json roots_resp = json{{"jsonrpc", "2.0"}, {"id", 77},
                           {"result", {{"roots", json::array()}}}};
    // Deliver the answer the way a transport would.
    t->handler()(roots_resp, [](const json&) {}, "");
    // (no crash, stray response is tolerated)
}
