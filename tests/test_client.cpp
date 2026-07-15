#include <gtest/gtest.h>

#include "cppmcp/client.hpp"

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

using namespace cppmcp;
using json = nlohmann::json;

namespace {

// Helper: build an initialize success response for the given request id.
json make_init_response(const RequestId& id) {
    json result;
    result["protocolVersion"] = "2025-03-26";
    result["capabilities"] = json{{"tools", json::object()}};
    result["serverInfo"] = json{{"name", "mock"}, {"version", "1.0"}};
    json resp;
    resp["jsonrpc"] = "2.0";
    resp["id"] = request_id_to_json(id);
    resp["result"] = std::move(result);
    return resp;
}

// In-process transport: records sent messages and lets the test inject inbound
// messages (posted onto the client's io loop). No real I/O.
class TestClientTransport : public IClientTransport,
                            public std::enable_shared_from_this<TestClientTransport> {
public:
    void connect() override { connected_.store(true); }
    void disconnect() override {
        connected_.store(false);
        message_handler_ = {};
        error_handler_ = {};
        disconnect_handler_ = {};
    }
    bool is_connected() const override { return connected_.load(); }

    void send_message(const nlohmann::json& message) override {
        {
            std::lock_guard<std::mutex> lock(mu_);
            sent_.push_back(message);
        }
        std::function<void(const json&)> cb;
        { std::lock_guard<std::mutex> lock(mu_); cb = on_send_; }
        if (cb) cb(message);
    }

    void set_message_handler(MessageCallback handler) override { message_handler_ = std::move(handler); }
    void set_error_handler(ErrorCallback handler) override { error_handler_ = std::move(handler); }
    void set_disconnect_handler(DisconnectCallback handler) override { disconnect_handler_ = std::move(handler); }
    void set_io_context(asio::io_context* io_ctx) override { io_ = io_ctx; }

    void deliver(const json& message) {
        auto self = shared_from_this();
        asio::post(*io_, [self, message]() {
            if (self->message_handler_) self->message_handler_(message);
        });
    }

    void set_on_send(std::function<void(const json&)> cb) {
        std::lock_guard<std::mutex> lock(mu_);
        on_send_ = std::move(cb);
    }

    json find_result_for(const RequestId& id) const {
        json target = request_id_to_json(id);
        std::lock_guard<std::mutex> lock(mu_);
        for (const auto& m : sent_) {
            if (m.contains("id") && m["id"] == target && m.contains("result")) return m;
        }
        return {};
    }

    asio::io_context* io_{nullptr};
    MessageCallback message_handler_;
    ErrorCallback error_handler_;
    DisconnectCallback disconnect_handler_;
    std::function<void(const json&)> on_send_;
    mutable std::mutex mu_;
    std::vector<json> sent_;
    std::atomic<bool> connected_{false};
};

} // namespace

class McpClientTest : public ::testing::Test {
protected:
    void SetUp() override {
        transport_ = std::make_shared<TestClientTransport>();
        client_ = std::make_shared<McpClient>(Implementation{"test_client", "1.0.0"});
        client_->use_transport(transport_);

        transport_->set_on_send([this](const json& m) {
            if (m.value("method", "") == "initialize" && m.contains("id")) {
                RequestId id;
                from_json(m["id"], id);
                transport_->deliver(make_init_response(id));
            }
        });
        client_->connect(std::chrono::seconds(5));
    }
    void TearDown() override { client_->stop(); }

    std::shared_ptr<TestClientTransport> transport_;
    std::shared_ptr<McpClient> client_;
};

TEST_F(McpClientTest, OutboundRequestResponse) {
    auto pr = client_->prepare(Protocol::METHOD_TOOLS_LIST).timeout(std::chrono::seconds(2)).send();
    json resp;
    resp["jsonrpc"] = "2.0";
    resp["id"] = request_id_to_json(pr->id);
    resp["result"] = json{{"tools", json::array()}};
    transport_->deliver(resp);
    json result = pr->get();
    EXPECT_TRUE(result["tools"].is_array());
}

TEST_F(McpClientTest, ErrorResponseThrows) {
    auto pr = client_->prepare(Protocol::METHOD_TOOLS_CALL, json{{"name", "x"}})
                  .timeout(std::chrono::seconds(2)).send();
    json err;
    err["jsonrpc"] = "2.0";
    err["id"] = request_id_to_json(pr->id);
    json detail;
    detail["code"] = -32601;
    detail["message"] = "nope";
    err["error"] = std::move(detail);
    transport_->deliver(err);
    EXPECT_THROW(pr->get(), McpException);
}

TEST_F(McpClientTest, TimeoutWhenNoResponse) {
    auto pr = client_->prepare(Protocol::METHOD_PING).timeout(std::chrono::milliseconds(150)).send();
    EXPECT_THROW(pr->get(), McpException);
}

TEST_F(McpClientTest, CancelRequest) {
    auto pr = client_->prepare(Protocol::METHOD_PING).timeout(std::chrono::seconds(10)).send();
    pr->cancel("user-cancelled");
    EXPECT_THROW(pr->get(), McpException);

    json cancel_notif;
    {
        std::lock_guard<std::mutex> lock(transport_->mu_);
        for (const auto& m : transport_->sent_) {
            if (m.value("method", "") == Protocol::NOTIF_CANCELLED) { cancel_notif = m; break; }
        }
    }
    ASSERT_TRUE(cancel_notif.contains("params"));
    EXPECT_EQ(cancel_notif["params"]["requestId"], request_id_to_json(pr->id));
}

TEST_F(McpClientTest, ProgressNotificationRoutesToHandler) {
    std::atomic<double> received{-1.0};
    auto pr = client_->prepare(Protocol::METHOD_TOOLS_LIST)
                  .on_progress([&](double p, std::optional<double>) { received.store(p); })
                  .timeout(std::chrono::seconds(2))
                  .send();
    int64_t id = std::get<int64_t>(pr->id);

    json prog;
    prog["jsonrpc"] = "2.0";
    prog["method"] = Protocol::NOTIF_PROGRESS;
    json pp;
    pp["progressToken"] = id;
    pp["progress"] = 0.42;
    prog["params"] = std::move(pp);
    transport_->deliver(prog);

    json resp;
    resp["jsonrpc"] = "2.0";
    resp["id"] = request_id_to_json(pr->id);
    resp["result"] = json{{"tools", json::array()}};
    transport_->deliver(resp);

    pr->get();
    EXPECT_NEAR(received.load(), 0.42, 1e-9);
}

TEST_F(McpClientTest, ShutdownFailsPending) {
    auto pr = client_->prepare(Protocol::METHOD_PING).timeout(std::chrono::seconds(10)).send();
    client_->stop();
    EXPECT_THROW(pr->get(), McpException);
}

TEST_F(McpClientTest, InboundRootsRequest) {
    client_->register_roots_handler([]() -> ListRootsResult {
        ListRootsResult r;
        r.roots.push_back(Root{"file:///workspace", "workspace"});
        return r;
    });
    json req;
    req["jsonrpc"] = "2.0";
    req["id"] = "s1";
    req["method"] = Protocol::METHOD_ROOTS_LIST;
    transport_->deliver(req);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    json resp = transport_->find_result_for(RequestId{std::string{"s1"}});
    ASSERT_TRUE(resp.contains("result"));
    EXPECT_EQ(resp["result"]["roots"][0]["uri"], "file:///workspace");
}

TEST_F(McpClientTest, InboundPingAutoAnswered) {
    json req;
    req["jsonrpc"] = "2.0";
    req["id"] = "p1";
    req["method"] = Protocol::METHOD_PING;
    transport_->deliver(req);
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    json resp = transport_->find_result_for(RequestId{std::string{"p1"}});
    EXPECT_TRUE(resp.contains("result"));
}
