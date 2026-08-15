#include <gtest/gtest.h>

#include "test_helpers.hpp"

#include <cppmcp/http_transport.hpp>

#include <chrono>
#include <deque>
#include <mutex>

using namespace cppmcp;
using namespace cppmcp::testing;
using json = nlohmann::json;

// --- SSE Client: reads SSE stream + posts messages ---
class SseClient {
public:
    SseClient(asio::io_context& io_ctx, const std::string& host, int port)
        : io_ctx_(io_ctx), host_(host), port_(port) {}

    // Open SSE stream, read until we get the endpoint event
    std::string open_sse_stream(std::chrono::milliseconds timeout = std::chrono::seconds(5)) {
        asio::ip::tcp::resolver resolver(io_ctx_);
        auto endpoints = resolver.resolve(host_, std::to_string(port_));
        sse_socket_ = std::make_unique<asio::ip::tcp::socket>(io_ctx_);
        asio::connect(*sse_socket_, endpoints);

        std::string request = build_http_get(host_, port_, "/sse",
            std::map<std::string, std::string>{{"Accept", "text/event-stream"}});
        asio::write(*sse_socket_, asio::buffer(request));

        // Read SSE events until we get endpoint event
        endpoint_path_ = read_sse_until_endpoint(timeout);
        return endpoint_path_;
    }

    // Read the next SSE data event (blocking with timeout)
    json read_sse_data(std::chrono::milliseconds timeout = std::chrono::seconds(5)) {
        std::string line;
        std::string event_type;
        std::string data;

        asio::steady_timer timer(io_ctx_, timeout);
        bool timed_out = false;
        timer.async_wait([&](const asio::error_code& ec) {
            if (!ec) timed_out = true;
        });

        // Read lines until we get a complete SSE frame (double newline)
        asio::streambuf buf;
        while (!timed_out) {
            asio::error_code ec;
            std::size_t n = asio::read_until(*sse_socket_, buf, '\n', ec);
            if (ec || n == 0) break;

            std::string raw_line;
            raw_line.reserve(n);
            auto bufs_data = buf.data();
            for (auto it = asio::buffers_begin(bufs_data);
                 it != asio::buffers_begin(bufs_data) + n; ++it) {
                raw_line += *it;
            }
            buf.consume(n);

            // Strip \r\n
            while (!raw_line.empty() && (raw_line.back() == '\n' || raw_line.back() == '\r')) {
                raw_line.pop_back();
            }

            if (raw_line.empty()) {
                // End of SSE frame
                if (!data.empty()) {
                    timer.cancel();
                    return json::parse(data);
                }
                continue;
            }

            if (raw_line.substr(0, 6) == "event:") {
                event_type = raw_line.substr(6);
                while (!event_type.empty() && event_type[0] == ' ') event_type.erase(0, 1);
            } else if (raw_line.substr(0, 5) == "data:") {
                data = raw_line.substr(5);
                while (!data.empty() && data[0] == ' ') data.erase(0, 1);
            }
        }

        timer.cancel();
        if (timed_out) throw std::runtime_error("SSE read timeout");
        return json();
    }

    // POST a JSON-RPC message to the messages endpoint
    HttpResponse post_message(const json& msg,
                               std::chrono::milliseconds timeout = std::chrono::seconds(3)) {
        asio::io_context post_io;
        asio::ip::tcp::resolver resolver(post_io);
        auto endpoints = resolver.resolve(host_, std::to_string(port_));
        asio::ip::tcp::socket socket(post_io);
        asio::connect(socket, endpoints);

        std::string body_str = msg.dump();
        std::string request = build_http_post(host_, port_, endpoint_path_,
            "application/json", body_str);
        asio::write(socket, asio::buffer(request));

        // Read HTTP response
        asio::streambuf response_buf;
        asio::read_until(socket, response_buf, "\r\n\r\n");

        std::string full_data;
        {
            auto bufs = response_buf.data();
            full_data = std::string(asio::buffers_begin(bufs),
                asio::buffers_begin(bufs) + response_buf.size());
        }

        // For 202 responses, body may be empty
        auto header_end = full_data.find("\r\n\r\n");
        std::string headers_only = full_data.substr(0, header_end);

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

        std::string body_so_far = full_data.substr(header_end + 4);
        if (content_length > static_cast<int>(body_so_far.size())) {
            asio::read(socket, response_buf,
                asio::transfer_exactly(content_length - body_so_far.size()));
            auto bufs = response_buf.data();
            body_so_far += std::string(asio::buffers_begin(bufs),
                asio::buffers_begin(bufs) + response_buf.size());
        }

        std::string full_response = headers_only + "\r\n\r\n" + body_so_far;
        return parse_http_response(full_response);
    }

    void close() {
        if (sse_socket_) {
            asio::error_code ec;
            sse_socket_->close(ec);
        }
    }

private:
    std::string read_sse_until_endpoint(std::chrono::milliseconds timeout) {
        std::string event_type;
        std::string data;

        asio::steady_timer timer(io_ctx_, timeout);
        bool timed_out = false;
        timer.async_wait([&](const asio::error_code& ec) {
            if (!ec) timed_out = true;
        });

        asio::streambuf buf;
        while (!timed_out) {
            asio::error_code ec;
            std::size_t n = asio::read_until(*sse_socket_, buf, '\n', ec);
            if (ec || n == 0) break;

            std::string raw_line;
            raw_line.reserve(n);
            auto bufs_data = buf.data();
            for (auto it = asio::buffers_begin(bufs_data);
                 it != asio::buffers_begin(bufs_data) + n; ++it) {
                raw_line += *it;
            }
            buf.consume(n);

            while (!raw_line.empty() && (raw_line.back() == '\n' || raw_line.back() == '\r')) {
                raw_line.pop_back();
            }

            if (raw_line.empty()) {
                // End of SSE frame
                if (event_type == "endpoint" && !data.empty()) {
                    timer.cancel();
                    return data;
                }
                event_type.clear();
                data.clear();
                continue;
            }

            if (raw_line.substr(0, 6) == "event:") {
                event_type = raw_line.substr(6);
                while (!event_type.empty() && event_type[0] == ' ') event_type.erase(0, 1);
            } else if (raw_line.substr(0, 5) == "data:") {
                data = raw_line.substr(5);
                while (!data.empty() && data[0] == ' ') data.erase(0, 1);
            }
        }

        timer.cancel();
        throw std::runtime_error("SSE endpoint event not received within timeout");
    }

    asio::io_context& io_ctx_;
    std::string host_;
    int port_;
    std::string endpoint_path_;
    std::unique_ptr<asio::ip::tcp::socket> sse_socket_;
};

// --- Test Fixture ---
class SseTransportTest : public ::testing::Test {
protected:
    void SetUp() override {
        server_ = create_test_server_with_tools();

        // Add a resource for testing
        Resource config_resource;
        config_resource.name = "config";
        config_resource.uri = "file:///config.json";
        config_resource.description = "Configuration";
        config_resource.mime_type = "application/json";

        server_->register_resource("file:///config.json", config_resource,
            [](const std::string& uri, RequestContext&) -> ReadResourceResult {
                ResourceContents contents;
                contents.uri = uri;
                contents.mime_type = "application/json";
                contents.text = "{\"version\": \"1.0\"}";
                return ReadResourceResult{{contents}};
            }
        );

        HttpTransportConfig config;
        config.mode = HttpTransportMode::SSE;
        config.host = "127.0.0.1";
        config.port = 0; // Random port
        config.sse_path = "/sse";
        config.message_path = "/messages";

        auto transport = std::make_shared<HttpTransport>(config);
        server_->connect(transport);
        transport_ = transport;

        server_thread_ = std::make_unique<ServerThread>(*server_);
        server_thread_->wait_until_ready();

        port_ = transport_->get_port();
    }

    void TearDown() override {
        server_thread_->stop();
        // Reset members in a controlled order (thread -> transport -> server)
        // instead of leaving it to the fixture destructor: destruction there
        // races with leftover io handlers under optimized builds.
        server_thread_.reset();
        transport_.reset();
        server_.reset();
    }

    std::shared_ptr<McpServer> server_;
    std::shared_ptr<HttpTransport> transport_;
    std::unique_ptr<ServerThread> server_thread_;
    int port_ = 0;
    std::string host_ = "127.0.0.1";
};

TEST_F(SseTransportTest, ConnectAndGetEndpoint) {
    asio::io_context client_io;
    SseClient client(client_io, host_, port_);

    std::string endpoint = client.open_sse_stream();
    EXPECT_FALSE(endpoint.empty());
    EXPECT_NE(endpoint.find("/messages"), std::string::npos);

    client.close();
}

TEST_F(SseTransportTest, InitializeViaPost) {
    asio::io_context client_io;
    SseClient client(client_io, host_, port_);

    std::string endpoint = client.open_sse_stream();

    // POST initialize
    auto post_resp = client.post_message(make_initialize_request(1));
    EXPECT_EQ(post_resp.status, 202);

    // Read response from SSE stream
    auto sse_resp = client.read_sse_data();

    EXPECT_EQ(sse_resp["jsonrpc"], "2.0");
    EXPECT_EQ(sse_resp["id"], 1);
    EXPECT_TRUE(sse_resp.contains("result"));
    EXPECT_EQ(sse_resp["result"]["protocolVersion"], "2025-03-26");

    client.close();
}

TEST_F(SseTransportTest, ToolsCallViaSse) {
    asio::io_context client_io;
    SseClient client(client_io, host_, port_);

    std::string endpoint = client.open_sse_stream();

    // Initialize
    client.post_message(make_initialize_request(1));
    auto init_resp = client.read_sse_data();
    EXPECT_EQ(init_resp["id"], 1);

    // Initialized notification
    client.post_message(make_initialized_notification());

    // Call echo tool
    client.post_message(make_tools_call_request(3, "echo", json{{"message", "sse test"}}));
    auto call_resp = client.read_sse_data();

    EXPECT_EQ(call_resp["id"], 3);
    EXPECT_EQ(call_resp["result"]["content"][0]["text"], "sse test");

    client.close();
}

TEST_F(SseTransportTest, ResourcesReadViaSse) {
    asio::io_context client_io;
    SseClient client(client_io, host_, port_);

    std::string endpoint = client.open_sse_stream();

    // Initialize
    client.post_message(make_initialize_request(1));
    auto init_resp = client.read_sse_data();

    client.post_message(make_initialized_notification());

    // Read resource
    json read_req = {
        {"jsonrpc", "2.0"},
        {"id", 4},
        {"method", "resources/read"},
        {"params", json{{"uri", "file:///config.json"}}}
    };
    client.post_message(read_req);
    auto read_resp = client.read_sse_data();

    EXPECT_EQ(read_resp["id"], 4);
    EXPECT_TRUE(read_resp["result"].contains("contents"));

    client.close();
}