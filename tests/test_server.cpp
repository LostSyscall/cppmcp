#include <gtest/gtest.h>

#include <cppmcp/server.hpp>
#include <cppmcp/transport.hpp>
#include <cppmcp/types.hpp>

#include <sstream>
#include <thread>

using namespace cppmcp;
using json = nlohmann::json;

// A simple test transport that captures output
class TestTransport : public ITransport {
public:
    void start() override { running_ = true; }
    void stop() override { running_ = false; }
    bool is_running() const override { return running_; }

    void send_message(const json& message) override {
        std::lock_guard<std::mutex> lock(write_mutex_);
        output_ << message.dump() << std::endl;
    }

    void set_message_handler(MessageCallback handler) override {
        message_handler_ = std::move(handler);
    }
    void set_error_handler(ErrorCallback handler) override {
        error_handler_ = std::move(handler);
    }
    void set_io_context(asio::io_context*) override {}

    std::string get_output() { return output_.str(); }

    MessageCallback& handler() { return message_handler_; }

    // Deliver a message to the server the way a real transport would: any
    // response the server produces is routed back through send_message (which
    // appends to the captured output).
    void deliver(const json& msg) {
        if (message_handler_) {
            message_handler_(msg, [this](const json& resp) {
                send_message(resp);
            }, "");
        }
    }

private:
    std::stringstream output_;
    std::atomic<bool> running_{false};
    std::mutex write_mutex_;
    MessageCallback message_handler_;
    ErrorCallback error_handler_;
};

TEST(ServerTest, HandleInitialize) {
    Implementation info{"test_server", "1.0.0"};
    ServerCapabilities caps;
    caps.tools = ToolsCapability();

    McpServer server(info, caps);

    json init_request = {
        {"jsonrpc", "2.0"},
        {"id", 1},
        {"method", "initialize"},
        {"params", {
            {"protocolVersion", "2025-03-26"},
            {"capabilities", json::object()},
            {"clientInfo", {"name", "test_client", "version", "1.0"}}
        }}
    };

    auto transport = std::make_shared<TestTransport>();
    server.connect(transport);

    // Simulate incoming message via transport
    transport->deliver(init_request);

    std::string output = transport->get_output();
    EXPECT_FALSE(output.empty());

    json response = json::parse(output.substr(0, output.find('\n')));
    EXPECT_EQ(response["jsonrpc"], "2.0");
    EXPECT_EQ(response["id"], 1);
    EXPECT_EQ(response["result"]["protocolVersion"], "2025-03-26");
    EXPECT_EQ(response["result"]["serverInfo"]["name"], "test_server");
}

TEST(ServerTest, HandlePing) {
    Implementation info{"test_server", "1.0.0"};
    McpServer server(info);

    auto transport = std::make_shared<TestTransport>();
    server.connect(transport);

    // First initialize
    transport->deliver(json{
        {"jsonrpc", "2.0"},
        {"id", 1},
        {"method", "initialize"},
        {"params", {
            {"protocolVersion", "2025-03-26"},
            {"capabilities", json::object()},
            {"clientInfo", {"name", "test", "version", "1.0"}}
        }}
    });

    // Send initialized notification
    transport->deliver(json{{"jsonrpc", "2.0"}, {"method", "notifications/initialized"}});

    // Now ping
    json ping_request = {
        {"jsonrpc", "2.0"},
        {"id", 2},
        {"method", "ping"}
    };
    transport->deliver(ping_request);

    // Find the ping response by id
    std::string output = transport->get_output();
    std::istringstream iss(output);
    std::string line;
    bool found = false;
    while (std::getline(iss, line)) {
        if (line.empty()) continue;
        json resp = json::parse(line);
        if (resp["id"] == 2) {
            EXPECT_EQ(resp["jsonrpc"], "2.0");
            EXPECT_TRUE(resp.contains("result"));
            found = true;
            break;
        }
    }
    EXPECT_TRUE(found);
}

TEST(ServerTest, HandleToolsList) {
    Implementation info{"test_server", "1.0.0"};
    ServerCapabilities caps;
    caps.tools = ToolsCapability();

    McpServer server(info, caps);

    Tool echo_tool;
    echo_tool.name = "echo";
    echo_tool.description = "Echo tool";
    echo_tool.input_schema = R"({"type": "object"})"_json;

    server.register_tool("echo", echo_tool,
        [](const json&, RequestContext&) -> CallToolResult {
            CallToolResult result;
            result.content.push_back(TextContent{"text", "echo"});
            result.is_error = false;
            return result;
        }
    );

    auto transport = std::make_shared<TestTransport>();
    server.connect(transport);

    // Initialize first
    transport->deliver(json{
        {"jsonrpc", "2.0"},
        {"id", 1},
        {"method", "initialize"},
        {"params", {
            {"protocolVersion", "2025-03-26"},
            {"capabilities", json::object()},
            {"clientInfo", {"name", "test", "version", "1.0"}}
        }}
    });
    transport->deliver(json{{"jsonrpc", "2.0"}, {"method", "notifications/initialized"}});

    json list_request = {
        {"jsonrpc", "2.0"},
        {"id", 3},
        {"method", "tools/list"}
    };

    transport->deliver(list_request);

    std::string output = transport->get_output();
    // Find tools/list response
    std::istringstream iss(output);
    std::string line;
    while (std::getline(iss, line)) {
        json resp = json::parse(line);
        if (resp.contains("result") && resp["result"].contains("tools")) {
            EXPECT_EQ(resp["result"]["tools"][0]["name"], "echo");
            break;
        }
    }
}

TEST(ServerTest, HandleToolsCall) {
    Implementation info{"test_server", "1.0.0"};
    ServerCapabilities caps;
    caps.tools = ToolsCapability();

    McpServer server(info, caps);

    Tool echo_tool;
    echo_tool.name = "echo";
    echo_tool.description = "Echo tool";
    echo_tool.input_schema = R"({"type": "object"})"_json;

    server.register_tool("echo", echo_tool,
        [](const json& args, RequestContext&) -> CallToolResult {
            CallToolResult result;
            result.content.push_back(TextContent{"text", args["message"].get<std::string>()});
            result.is_error = false;
            return result;
        }
    );

    auto transport = std::make_shared<TestTransport>();
    server.connect(transport);

    // Initialize first
    transport->deliver(json{
        {"jsonrpc", "2.0"},
        {"id", 1},
        {"method", "initialize"},
        {"params", {
            {"protocolVersion", "2025-03-26"},
            {"capabilities", json::object()},
            {"clientInfo", {"name", "test", "version", "1.0"}}
        }}
    });
    transport->deliver(json{{"jsonrpc", "2.0"}, {"method", "notifications/initialized"}});

    json call_request = {
        {"jsonrpc", "2.0"},
        {"id", 4},
        {"method", "tools/call"},
        {"params", json::object({
            {"name", "echo"},
            {"arguments", json::object({{"message", "hello"}})}
        })}
    };

    transport->deliver(call_request);

    std::string output = transport->get_output();
    std::istringstream iss(output);
    std::string line;
    bool found = false;
    while (std::getline(iss, line)) {
        if (line.empty()) continue;
        json resp = json::parse(line);
        if (resp["id"] == 4) {
            EXPECT_EQ(resp["result"]["content"][0]["text"], "hello");
            // isError omitted when false (MCP spec default).
            EXPECT_FALSE(resp["result"].value("isError", false));
            found = true;
            break;
        }
    }
    EXPECT_TRUE(found);
}

TEST(ServerTest, RejectRequestBeforeInit) {
    Implementation info{"test_server", "1.0.0"};
    McpServer server(info);

    auto transport = std::make_shared<TestTransport>();
    server.connect(transport);

    json list_request = {
        {"jsonrpc", "2.0"},
        {"id", 5},
        {"method", "tools/list"}
    };

    transport->deliver(list_request);

    std::string output = transport->get_output();
    json response = json::parse(output.substr(0, output.find('\n')));
    EXPECT_EQ(response["error"]["code"], Protocol::SERVER_NOT_INITIALIZED);
}

TEST(ServerTest, MethodNotFound) {
    Implementation info{"test_server", "1.0.0"};
    McpServer server(info);

    auto transport = std::make_shared<TestTransport>();
    server.connect(transport);

    // Initialize first
    transport->deliver(json{
        {"jsonrpc", "2.0"},
        {"id", 1},
        {"method", "initialize"},
        {"params", {
            {"protocolVersion", "2025-03-26"},
            {"capabilities", json::object()},
            {"clientInfo", {"name", "test", "version", "1.0"}}
        }}
    });
    transport->deliver(json{{"jsonrpc", "2.0"}, {"method", "notifications/initialized"}});

    json unknown_request = {
        {"jsonrpc", "2.0"},
        {"id", 6},
        {"method", "unknown/method"}
    };

    transport->deliver(unknown_request);

    std::string output = transport->get_output();
    std::istringstream iss(output);
    std::string line;
    while (std::getline(iss, line)) {
        json resp = json::parse(line);
        if (resp["id"] == 6) {
            EXPECT_EQ(resp["error"]["code"], Protocol::METHOD_NOT_FOUND);
            break;
        }
    }
}

TEST(ServerTest, NullRequestIdInErrors) {
    // Test that parse errors produce null id (JSON-RPC spec)
    auto error_resp = make_error_response_null_id(Protocol::PARSE_ERROR, "Parse error");
    EXPECT_TRUE(error_resp["id"].is_null());
}

TEST(ServerTest, RequestIdSupportsNull) {
    RequestId null_id = NullId{};
    nlohmann::json j = request_id_to_json(null_id);
    EXPECT_TRUE(j.is_null());

    RequestId int_id{int64_t(42)};
    j = request_id_to_json(int_id);
    EXPECT_EQ(j.get<int64_t>(), 42);

    RequestId str_id{"abc"};
    j = request_id_to_json(str_id);
    EXPECT_EQ(j.get<std::string>(), "abc");
}