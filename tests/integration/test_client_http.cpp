#include <gtest/gtest.h>

#include "test_helpers.hpp"

#include <cppmcp/client.hpp>
#include <cppmcp/http_client_transport.hpp>
#include <cppmcp/http_transport.hpp>
#include <cppmcp/types.hpp>

#include <chrono>
#include <memory>
#include <variant>

using namespace cppmcp;
using namespace cppmcp::testing;
using json = nlohmann::json;

// End-to-end over Streamable HTTP: start the real HttpTransport server on a
// random port, connect with HttpClientTransport, and run client->server calls.
TEST(McpClientHttp, HandshakeListAndCall) {
    auto server = create_test_server_with_tools();

    HttpTransportConfig config;
    config.mode = HttpTransportMode::StreamableHttp;
    config.host = "127.0.0.1";
    config.port = 0;
    config.path = "/mcp";
    auto transport = std::make_shared<HttpTransport>(config);
    server->connect(transport);
    ServerThread server_thread(*server);
    server_thread.wait_until_ready();
    int port = transport->get_port();

    auto client = std::make_shared<McpClient>(Implementation{"http_client_test", "1.0.0"});
    client->use_transport(std::make_shared<HttpClientTransport>("127.0.0.1",
                                                                 static_cast<uint16_t>(port),
                                                                 "/mcp"));

    auto sr = client->connect(std::chrono::seconds(10));
    EXPECT_EQ(sr.server_info.name, "test_server");
    EXPECT_TRUE(client->has_capability("tools"));

    auto tools = client->list_tools();
    EXPECT_GE(tools.size(), 2u);

    CallToolResult r = client->call_tool("echo", json{{"message", "http-echo"}});
    ASSERT_FALSE(r.content.empty());
    auto* txt = std::get_if<TextContent>(&r.content[0]);
    ASSERT_NE(txt, nullptr);
    EXPECT_EQ(txt->text, "http-echo");
    EXPECT_FALSE(r.is_error);

    client->ping();

    client->stop();
    server_thread.stop();
}
