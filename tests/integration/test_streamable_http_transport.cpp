#include <gtest/gtest.h>

#include "test_helpers.hpp"

#include <cppmcp/http_transport.hpp>

#include <chrono>

using namespace cppmcp;
using namespace cppmcp::testing;
using json = nlohmann::json;

// --- Simple synchronous HTTP client using asio ---
class HttpClient {
public:
    std::string post(const std::string& host, int port, const std::string& path,
                     const json& body,
                     const std::map<std::string, std::string>& extra_headers = {},
                     std::chrono::milliseconds timeout = std::chrono::seconds(3)) {
        asio::io_context io_ctx;
        asio::ip::tcp::resolver resolver(io_ctx);
        auto endpoints = resolver.resolve(host, std::to_string(port));
        asio::ip::tcp::socket socket(io_ctx);
        asio::connect(socket, endpoints);

        std::string body_str = body.dump();
        std::string request = build_http_post(host, port, path,
            "application/json", body_str, extra_headers);

        asio::steady_timer timer(io_ctx, timeout);
        timer.async_wait([&](const asio::error_code&) {
            socket.close();
        });

        asio::write(socket, asio::buffer(request));

        // Read response with Content-Length based reading
        asio::streambuf response_buf;
        asio::read_until(socket, response_buf, "\r\n\r\n");

        // Parse headers
        std::string header_section;
        {
            auto bufs = response_buf.data();
            header_section = std::string(asio::buffers_begin(bufs),
                asio::buffers_begin(bufs) + response_buf.size());
        }

        auto header_end = header_section.find("\r\n\r\n");
        std::string headers_only = header_section.substr(0, header_end);
        std::string body_so_far = header_section.substr(header_end + 4);

        // Extract Content-Length
        int content_length = 0;
        std::istringstream his(headers_only);
        std::string line;
        while (std::getline(his, line)) {
            if (line.back() == '\r') line.pop_back();
            auto colon = line.find(':');
            if (colon != std::string::npos) {
                std::string key = line.substr(0, colon);
                for (auto& c : key) c = static_cast<char>(tolower(c));
                if (key == "content-length") {
                    content_length = std::stoi(line.substr(colon + 2));
                }
            }
        }

        timer.cancel();

        // Read remaining body bytes if needed
        int remaining = content_length - static_cast<int>(body_so_far.size());
        if (remaining > 0) {
            asio::read(socket, response_buf, asio::transfer_exactly(remaining));
            auto bufs = response_buf.data();
            body_so_far += std::string(asio::buffers_begin(bufs),
                asio::buffers_begin(bufs) + response_buf.size());
            // The body_so_far now includes the headers+body from before + new data
            // We need just the body part after headers
            // Re-extract from full buffer
            std::string full_data;
            {
                auto all_bufs = response_buf.data();
                full_data = std::string(asio::buffers_begin(all_bufs),
                    asio::buffers_begin(all_bufs) + response_buf.size());
            }
            auto fe = full_data.find("\r\n\r\n");
            if (fe != std::string::npos) {
                body_so_far = full_data.substr(fe + 4);
            }
        }

        // Reconstruct full response for parsing
        std::string full_response = headers_only + "\r\n\r\n" + body_so_far;
        return full_response;
    }

    HttpResponse post_and_parse(const std::string& host, int port, const std::string& path,
                                const json& body,
                                const std::map<std::string, std::string>& extra_headers = {},
                                std::chrono::milliseconds timeout = std::chrono::seconds(3)) {
        auto raw = post(host, port, path, body, extra_headers, timeout);
        return parse_http_response(raw);
    }

    HttpResponse delete_and_parse(const std::string& host, int port, const std::string& path,
                                   const std::map<std::string, std::string>& extra_headers = {},
                                   std::chrono::milliseconds timeout = std::chrono::seconds(3)) {
        asio::io_context io_ctx;
        asio::ip::tcp::resolver resolver(io_ctx);
        auto endpoints = resolver.resolve(host, std::to_string(port));
        asio::ip::tcp::socket socket(io_ctx);
        asio::connect(socket, endpoints);

        std::string request = build_http_delete(host, port, path, extra_headers);
        asio::write(socket, asio::buffer(request));

        asio::streambuf response_buf;
        asio::read_until(socket, response_buf, "\r\n\r\n");

        std::string header_section;
        {
            auto bufs = response_buf.data();
            header_section = std::string(asio::buffers_begin(bufs),
                asio::buffers_begin(bufs) + response_buf.size());
        }

        auto header_end = header_section.find("\r\n\r\n");
        std::string full_response = header_section;

        return parse_http_response(full_response);
    }
};

// --- Test Fixture ---
class StreamableHttpTest : public ::testing::Test {
protected:
    void SetUp() override {
        server_ = create_test_server_with_tools();

        // Also register a prompt for richer testing
        Prompt greet_prompt;
        greet_prompt.name = "greet";
        greet_prompt.description = "Greeting prompt";
        greet_prompt.arguments = std::vector<PromptArgument>{
            PromptArgument{"name", "Person name", true}
        };

        server_->register_prompt("greet", greet_prompt,
            [](const std::string& name, const json& args, RequestContext&) -> GetPromptResult {
                std::string person = args.contains("name") ? args["name"].get<std::string>() : "world";
                GetPromptResult result;
                result.description = "Greeting for " + person;
                result.messages.push_back(PromptMessage{
                    "user", TextContent{"text", "Hello " + person}
                });
                return result;
            }
        );

        HttpTransportConfig config;
        config.mode = HttpTransportMode::StreamableHttp;
        config.host = "127.0.0.1";
        config.port = 0; // Random port
        config.path = "/mcp";

        auto transport = std::make_shared<HttpTransport>(config);
        server_->connect(transport);
        transport_ = transport;

        server_thread_ = std::make_unique<ServerThread>(*server_);
        server_thread_->wait_until_ready();

        port_ = transport_->get_port();
    }

    void TearDown() override {
        server_thread_->stop();
    }

    std::shared_ptr<McpServer> server_;
    std::shared_ptr<HttpTransport> transport_;
    std::unique_ptr<ServerThread> server_thread_;
    int port_ = 0;
    std::string host_ = "127.0.0.1";
};

TEST_F(StreamableHttpTest, InitializeResponseInBody) {
    HttpClient client;
    auto resp = client.post_and_parse(host_, port_, "/mcp", make_initialize_request(1));

    EXPECT_EQ(resp.status, 200);
    EXPECT_TRUE(resp.headers.count("content-type"));
    EXPECT_NE(resp.headers["content-type"].find("application/json"), std::string::npos);

    json body = json::parse(resp.body);
    EXPECT_EQ(body["jsonrpc"], "2.0");
    EXPECT_EQ(body["id"], 1);
    EXPECT_TRUE(body.contains("result"));
    EXPECT_EQ(body["result"]["protocolVersion"], "2025-03-26");
}

TEST_F(StreamableHttpTest, PingViaPost) {
    HttpClient client;

    // Initialize first
    auto init_resp = client.post_and_parse(host_, port_, "/mcp", make_initialize_request(1));
    EXPECT_EQ(init_resp.status, 200);

    // Get session ID from init response
    std::string session_id = init_resp.headers.count("mcp-session-id") ?
        init_resp.headers["mcp-session-id"] : "";

    // Send initialized notification
    auto notif_resp = client.post_and_parse(host_, port_, "/mcp",
        make_initialized_notification(),
        session_id.empty() ? std::map<std::string, std::string>{} :
        std::map<std::string, std::string>{{"mcp-session-id", session_id}});
    EXPECT_EQ(notif_resp.status, 202);

    // Ping
    auto ping_resp = client.post_and_parse(host_, port_, "/mcp",
        make_ping_request(2),
        session_id.empty() ? std::map<std::string, std::string>{} :
        std::map<std::string, std::string>{{"mcp-session-id", session_id}});
    EXPECT_EQ(ping_resp.status, 200);

    json body = json::parse(ping_resp.body);
    EXPECT_TRUE(body.contains("result"));
}

TEST_F(StreamableHttpTest, ToolsCallViaPost) {
    HttpClient client;

    // Initialize
    auto init_resp = client.post_and_parse(host_, port_, "/mcp", make_initialize_request(1));
    std::string session_id = init_resp.headers.count("mcp-session-id") ?
        init_resp.headers["mcp-session-id"] : "";

    // Initialized notification
    client.post_and_parse(host_, port_, "/mcp", make_initialized_notification(),
        session_id.empty() ? std::map<std::string, std::string>{} :
        std::map<std::string, std::string>{{"mcp-session-id", session_id}});

    // Call echo tool
    auto call_resp = client.post_and_parse(host_, port_, "/mcp",
        make_tools_call_request(3, "echo", json{{"message", "hello HTTP"}}),
        std::map<std::string, std::string>{{"mcp-session-id", session_id}});
    EXPECT_EQ(call_resp.status, 200);

    json body = json::parse(call_resp.body);
    EXPECT_EQ(body["id"], 3);
    EXPECT_EQ(body["result"]["content"][0]["text"], "hello HTTP");
}

TEST_F(StreamableHttpTest, SessionIdHeader) {
    HttpClient client;
    auto resp = client.post_and_parse(host_, port_, "/mcp", make_initialize_request(1));

    EXPECT_TRUE(resp.headers.count("mcp-session-id"));
    EXPECT_FALSE(resp.headers["mcp-session-id"].empty());
}

TEST_F(StreamableHttpTest, NotificationReturns202) {
    HttpClient client;

    // Initialize
    auto init_resp = client.post_and_parse(host_, port_, "/mcp", make_initialize_request(1));
    std::string session_id = init_resp.headers["mcp-session-id"];

    // Send initialized notification
    auto notif_resp = client.post_and_parse(host_, port_, "/mcp",
        make_initialized_notification(),
        std::map<std::string, std::string>{{"mcp-session-id", session_id}});
    EXPECT_EQ(notif_resp.status, 202);
}

TEST_F(StreamableHttpTest, DeleteSession) {
    HttpClient client;

    // Initialize
    auto init_resp = client.post_and_parse(host_, port_, "/mcp", make_initialize_request(1));
    std::string session_id = init_resp.headers["mcp-session-id"];

    // Delete session
    auto del_resp = client.delete_and_parse(host_, port_, "/mcp",
        std::map<std::string, std::string>{{"mcp-session-id", session_id}});
    EXPECT_EQ(del_resp.status, 200);
}

// Regression (#16): after DELETE, a new client must be able to initialize a
// fresh session (previously the session was permanently broken until restart).
TEST_F(StreamableHttpTest, DeleteSessionIsRecreatable) {
    HttpClient client;

    auto init1 = client.post_and_parse(host_, port_, "/mcp", make_initialize_request(1));
    ASSERT_EQ(init1.status, 200);
    std::string sid1 = init1.headers["mcp-session-id"];
    ASSERT_FALSE(sid1.empty());

    auto del = client.delete_and_parse(host_, port_, "/mcp",
        std::map<std::string, std::string>{{"mcp-session-id", sid1}});
    EXPECT_EQ(del.status, 200);

    auto init2 = client.post_and_parse(host_, port_, "/mcp", make_initialize_request(2));
    ASSERT_EQ(init2.status, 200);
    ASSERT_TRUE(init2.headers.count("mcp-session-id"));
    std::string sid2 = init2.headers["mcp-session-id"];
    EXPECT_FALSE(sid2.empty());
    EXPECT_NE(sid1, sid2);
}

// Regression (#5): a tool that emits a progress notification before returning
// must NOT have that notification hijack the HTTP response body. The body has
// to carry the CallToolResult; the notification routes to the SSE stream.
TEST(StreamableHttpProgressTest, ProgressDoesNotHijackBody) {
    Implementation info{"progress_server", "1.0.0"};
    ServerCapabilities caps;
    caps.tools = ToolsCapability();
    auto server = std::make_shared<McpServer>(info, caps);

    Tool t;
    t.name = "with_progress";
    t.description = "Reports progress then returns";
    t.input_schema = nlohmann::json::parse("{\"type\":\"object\"}");
    server->register_tool("with_progress", t,
        [](const nlohmann::json& /*args*/, RequestContext& ctx) -> CallToolResult {
            ctx.report_progress(0.5);
            CallToolResult r;
            r.content.push_back(TextContent{"text", "final-result"});
            r.is_error = false;
            return r;
        });

    HttpTransportConfig config;
    config.mode = HttpTransportMode::StreamableHttp;
    config.host = "127.0.0.1";
    config.port = 0;
    config.path = "/mcp";
    auto transport = std::make_shared<HttpTransport>(config);
    server->connect(transport);

    ServerThread thread(*server);
    thread.wait_until_ready();
    int port = transport->get_port();

    HttpClient client;
    auto init = client.post_and_parse("127.0.0.1", port, "/mcp", make_initialize_request(1));
    ASSERT_EQ(init.status, 200);
    std::map<std::string, std::string> sh;
    if (init.headers.count("mcp-session-id")) {
        sh["mcp-session-id"] = init.headers["mcp-session-id"];
    }
    client.post_and_parse("127.0.0.1", port, "/mcp", make_initialized_notification(), sh);

    auto resp = client.post_and_parse("127.0.0.1", port, "/mcp",
        make_tools_call_request(2, "with_progress", nlohmann::json::object()), sh);
    ASSERT_EQ(resp.status, 200);

    nlohmann::json body = nlohmann::json::parse(resp.body);
    EXPECT_EQ(body["id"], 2);
    EXPECT_TRUE(body.contains("result"));
    EXPECT_FALSE(body.contains("method"));  // must not be a notification
    EXPECT_EQ(body["result"]["content"][0]["text"], "final-result");
}

// Regression (#6/#7): concurrent sessions are isolated; deleting one session
// must not affect another.
TEST_F(StreamableHttpTest, MultipleSessionsAreIndependent) {
    HttpClient a, b;
    auto ia = a.post_and_parse(host_, port_, "/mcp", make_initialize_request(1));
    auto ib = b.post_and_parse(host_, port_, "/mcp", make_initialize_request(2));
    ASSERT_EQ(ia.status, 200);
    ASSERT_EQ(ib.status, 200);
    std::string sida = ia.headers["mcp-session-id"];
    std::string sidb = ib.headers["mcp-session-id"];
    EXPECT_NE(sida, sidb);

    a.post_and_parse(host_, port_, "/mcp", make_initialized_notification(),
                     {{"mcp-session-id", sida}});
    b.post_and_parse(host_, port_, "/mcp", make_initialized_notification(),
                     {{"mcp-session-id", sidb}});

    // A deletes its own session.
    auto del = a.delete_and_parse(host_, port_, "/mcp", {{"mcp-session-id", sida}});
    EXPECT_EQ(del.status, 200);

    // B is unaffected and still works.
    auto pb = b.post_and_parse(host_, port_, "/mcp", make_ping_request(3),
                               {{"mcp-session-id", sidb}});
    EXPECT_EQ(pb.status, 200);

    // A's session is gone.
    auto pa = a.post_and_parse(host_, port_, "/mcp", make_ping_request(4),
                               {{"mcp-session-id", sida}});
    EXPECT_EQ(pa.status, 404);
}

// Regression (#12): a body larger than max_body_size is rejected during
// streaming, not after buffering the whole request.
TEST(StreamableHttpOversizeTest, OversizeBodyRejected) {
    Implementation info{"oversize_server", "1.0.0"};
    ServerCapabilities caps;
    auto server = std::make_shared<McpServer>(info, caps);

    HttpTransportConfig config;
    config.mode = HttpTransportMode::StreamableHttp;
    config.host = "127.0.0.1";
    config.port = 0;
    config.path = "/mcp";
    config.max_body_size = 64;
    auto transport = std::make_shared<HttpTransport>(config);
    server->connect(transport);

    ServerThread thread(*server);
    thread.wait_until_ready();
    int port = transport->get_port();

    HttpClient client;
    // make_initialize_request serializes to well over 64 bytes.
    auto resp = client.post_and_parse("127.0.0.1", port, "/mcp", make_initialize_request(1), {});
    EXPECT_EQ(resp.status, 400);
}

// Regression (#11): two requests pipelined into a single TCP write on one
// keep-alive connection must BOTH be dispatched (previously the first was
// lost/overwritten). Uses a raw socket with a 3s deadline.
TEST_F(StreamableHttpTest, PipelinedRequestsBothDispatched) {
    HttpClient client;
    auto init = client.post_and_parse(host_, port_, "/mcp", make_initialize_request(1));
    ASSERT_EQ(init.status, 200);
    std::map<std::string, std::string> sh;
    if (init.headers.count("mcp-session-id")) {
        sh["mcp-session-id"] = init.headers["mcp-session-id"];
    }
    client.post_and_parse(host_, port_, "/mcp", make_initialized_notification(), sh);

    asio::io_context io;
    asio::ip::tcp::resolver resolver(io);
    auto endpoints = resolver.resolve(host_, std::to_string(port_));
    asio::ip::tcp::socket socket(io);
    asio::connect(socket, endpoints);

    std::string req1 = build_http_post(host_, port_, "/mcp", "application/json",
                                       make_ping_request(10).dump(), sh);
    std::string req2 = build_http_post(host_, port_, "/mcp", "application/json",
                                       make_ping_request(11).dump(), sh);
    asio::write(socket, asio::buffer(req1 + req2));

    std::string all;
    char rbuf[1024];
    asio::steady_timer timer(io, std::chrono::seconds(3));
    timer.async_wait([&](const asio::error_code& ec) {
        if (!ec) {
            socket.close();
        }
    });

    std::function<void()> read_more;
    bool both = false;
    read_more = [&]() {
        socket.async_read_some(asio::buffer(rbuf),
            [&](const asio::error_code& ec, std::size_t n) {
                if (ec) {
                    return;
                }
                all.append(rbuf, n);
                if (all.find("\"id\":10") != std::string::npos &&
                    all.find("\"id\":11") != std::string::npos) {
                    both = true;
                    timer.cancel();
                    return;
                }
                read_more();
            });
    };
    read_more();
    io.run();

    EXPECT_TRUE(both);
    EXPECT_NE(all.find("\"id\":10"), std::string::npos);
    EXPECT_NE(all.find("\"id\":11"), std::string::npos);
}

// Concurrency stress: io threads (set_io_threads) + worker pool
// (set_worker_threads) under concurrent clients. Verifies responses are correct
// (no cross-talk / corruption) and the process does not crash — strands protect
// parser state and explicit session routing holds under multi-threading.
TEST(StreamableHttpMultiThreadTest, ConcurrentClientsNoCorruption) {
    Implementation info{"mt_server", "1.0.0"};
    ServerCapabilities caps;
    caps.tools = ToolsCapability();
    auto server = std::make_shared<McpServer>(info, caps);
    server->set_io_threads(4);
    server->set_worker_threads(4);

    Tool t;
    t.name = "slow_echo";
    t.description = "Echoes after a short delay";
    t.input_schema = nlohmann::json::parse(
        "{\"type\":\"object\",\"properties\":{\"message\":{\"type\":\"string\"}},\"required\":[\"message\"]}");
    server->register_tool("slow_echo", t,
        [](const nlohmann::json& args, RequestContext&) -> CallToolResult {
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
            CallToolResult r;
            r.content.push_back(TextContent{"text", args["message"].get<std::string>()});
            r.is_error = false;
            return r;
        });

    HttpTransportConfig config;
    config.mode = HttpTransportMode::StreamableHttp;
    config.host = "127.0.0.1";
    config.port = 0;
    config.path = "/mcp";
    auto transport = std::make_shared<HttpTransport>(config);
    server->connect(transport);

    ServerThread thread(*server);
    thread.wait_until_ready();
    int port = transport->get_port();

    constexpr int kClients = 4;
    constexpr int kPerClient = 4;
    std::vector<std::thread> clients;
    std::atomic<int> errors{0};

    for (int i = 0; i < kClients; ++i) {
        clients.emplace_back([&, i]() {
            HttpClient c;
            auto init = c.post_and_parse("127.0.0.1", port, "/mcp", make_initialize_request(1));
            if (init.status != 200) { ++errors; return; }
            std::string sid = init.headers.count("mcp-session-id") ? init.headers["mcp-session-id"] : "";
            std::map<std::string, std::string> sh;
            if (!sid.empty()) sh["mcp-session-id"] = sid;
            c.post_and_parse("127.0.0.1", port, "/mcp", make_initialized_notification(), sh);

            for (int j = 0; j < kPerClient; ++j) {
                int id = i * 100 + j;
                std::string msg = "c" + std::to_string(id);
                auto resp = c.post_and_parse("127.0.0.1", port, "/mcp",
                    make_tools_call_request(id, "slow_echo", nlohmann::json{{"message", msg}}), sh);
                if (resp.status != 200) { ++errors; continue; }
                try {
                    auto body = nlohmann::json::parse(resp.body);
                    if (body.value("id", -1) != id) { ++errors; continue; }
                    if (body["result"]["content"][0]["text"] != msg) { ++errors; }
                } catch (...) {
                    ++errors;
                }
            }
        });
    }

    for (auto& th : clients) {
        if (th.joinable()) th.join();
    }

    EXPECT_EQ(errors.load(), 0);
}