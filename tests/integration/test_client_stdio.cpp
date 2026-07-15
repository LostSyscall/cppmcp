#include <gtest/gtest.h>

#include "cppmcp/client.hpp"
#include "cppmcp/stdio_client_transport.hpp"
#include "cppmcp/types.hpp"

#include <chrono>
#include <string>
#include <variant>

using namespace cppmcp;
using json = nlohmann::json;

namespace {
std::string server_executable() {
#ifdef CPPMCP_STDIO_SERVER_PATH
    return CPPMCP_STDIO_SERVER_PATH;
#else
#ifdef _WIN32
    return "cppmcp_test_stdio_server.exe";
#else
    return "cppmcp_test_stdio_server";
#endif
#endif
}
} // namespace

// End-to-end: spawn the stdio server as a child, run the full client lifecycle
// (handshake -> list -> call -> ping -> shutdown).
TEST(McpClientStdio, HandshakeListAndCall) {
    Implementation info{"test_client", "1.0.0"};
    ClientCapabilities caps;
    auto client = std::make_shared<McpClient>(info, caps);
    auto transport = std::make_shared<StdioClientTransport>(server_executable());
    client->use_transport(transport);

    auto server_info = client->connect(std::chrono::seconds(20));
    EXPECT_EQ(server_info.server_info.name, "test_stdio_server");
    EXPECT_TRUE(client->has_capability("tools"));

    auto tools = client->list_tools();
    EXPECT_GE(tools.size(), 2u);

    CallToolResult r = client->call_tool("echo", json{{"message", "hello-from-client"}});
    ASSERT_EQ(r.content.size(), 1u);
    auto* txt = std::get_if<TextContent>(&r.content[0]);
    ASSERT_NE(txt, nullptr);
    EXPECT_EQ(txt->text, "hello-from-client");
    EXPECT_FALSE(r.is_error);

    client->ping();  // additional round-trip
    client->stop();
}

// Callback path: use the builder with on_complete instead of blocking get().
TEST(McpClientStdio, AsyncBuilderCallback) {
    Implementation info{"test_client", "1.0.0"};
    auto client = std::make_shared<McpClient>(info);
    client->use_transport(std::make_shared<StdioClientTransport>(server_executable()));

    client->connect(std::chrono::seconds(20));

    std::mutex m;
    std::condition_variable cv;
    bool done = false;
    json tools_json;

    client->prepare(Protocol::METHOD_TOOLS_LIST)
        .on_complete([&](const json& result) {
            std::lock_guard<std::mutex> lock(m);
            tools_json = result;
            done = true;
            cv.notify_all();
        })
        .timeout(std::chrono::seconds(10))
        .send();

    {
        std::unique_lock<std::mutex> lock(m);
        cv.wait_for(lock, std::chrono::seconds(15), [&] { return done; });
    }
    ASSERT_TRUE(done);
    EXPECT_TRUE(tools_json["tools"].is_array());
    EXPECT_GE(tools_json["tools"].size(), 2u);

    client->stop();
}
