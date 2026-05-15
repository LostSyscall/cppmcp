#include <gtest/gtest.h>

#include <cppmcp/jsonrpc.hpp>
#include <cppmcp/protocol.hpp>

using namespace cppmcp;
using json = nlohmann::json;

TEST(JsonRpcTest, ParseValidRequest) {
    json raw = {
        {"jsonrpc", "2.0"},
        {"id", 1},
        {"method", "initialize"},
        {"params", {"protocolVersion", "2025-03-26"}}
    };

    auto parsed = parse_message(raw);
    auto* req = std::get_if<JsonRpcRequest>(&parsed);
    ASSERT_NE(req, nullptr);
    EXPECT_EQ(req->method, "initialize");
    EXPECT_EQ(std::get<int64_t>(req->id), 1);
}

TEST(JsonRpcTest, ParseValidNotification) {
    json raw = {
        {"jsonrpc", "2.0"},
        {"method", "notifications/initialized"}
    };

    auto parsed = parse_message(raw);
    auto* notif = std::get_if<JsonRpcNotification>(&parsed);
    ASSERT_NE(notif, nullptr);
    EXPECT_EQ(notif->method, "notifications/initialized");
}

TEST(JsonRpcTest, ParseInvalidJsonRpcVersion) {
    json raw = {
        {"jsonrpc", "1.0"},
        {"id", 1},
        {"method", "ping"}
    };

    auto parsed = parse_message(raw);
    auto* err = std::get_if<JsonRpcErrorResponse>(&parsed);
    ASSERT_NE(err, nullptr);
    EXPECT_EQ(err->error.code, Protocol::INVALID_REQUEST);
}

TEST(JsonRpcTest, ParseMissingMethodAndId) {
    json raw = {
        {"jsonrpc", "2.0"}
    };

    auto parsed = parse_message(raw);
    auto* err = std::get_if<JsonRpcErrorResponse>(&parsed);
    ASSERT_NE(err, nullptr);
    EXPECT_EQ(err->error.code, Protocol::INVALID_REQUEST);
}

TEST(JsonRpcTest, MakeSuccessResponse) {
    RequestId id{int64_t(42)};
    json result = json::object();
    result["status"] = "ok";
    auto resp = make_success_response(id, result);

    EXPECT_EQ(resp["jsonrpc"], "2.0");
    EXPECT_EQ(resp["id"], 42);
    EXPECT_EQ(resp["result"]["status"], "ok");
}

TEST(JsonRpcTest, MakeErrorResponse) {
    RequestId id{int64_t(1)};
    auto resp = make_error_response(id, Protocol::METHOD_NOT_FOUND, "Method not found");

    EXPECT_EQ(resp["jsonrpc"], "2.0");
    EXPECT_EQ(resp["id"], 1);
    EXPECT_EQ(resp["error"]["code"], Protocol::METHOD_NOT_FOUND);
    EXPECT_EQ(resp["error"]["message"], "Method not found");
}

TEST(JsonRpcTest, MakeErrorResponseNullId) {
    auto resp = make_error_response_null_id(Protocol::PARSE_ERROR, "Parse error");

    EXPECT_EQ(resp["jsonrpc"], "2.0");
    EXPECT_TRUE(resp["id"].is_null());
    EXPECT_EQ(resp["error"]["code"], Protocol::PARSE_ERROR);
}

TEST(JsonRpcTest, SerializeRequest) {
    JsonRpcRequest req;
    req.id = int64_t(1);
    req.method = "tools/list";

    auto j = serialize_request(req);
    EXPECT_EQ(j["jsonrpc"], "2.0");
    EXPECT_EQ(j["id"], 1);
    EXPECT_EQ(j["method"], "tools/list");
}

TEST(JsonRpcTest, SerializeNotification) {
    JsonRpcNotification notif;
    notif.method = "notifications/progress";
    notif.params = json{{"progressToken", 1}, {"progress", 0.5}};

    auto j = serialize_notification(notif);
    EXPECT_EQ(j["jsonrpc"], "2.0");
    EXPECT_EQ(j["method"], "notifications/progress");
    EXPECT_EQ(j["params"]["progress"], 0.5);
}

TEST(JsonRpcTest, StringRequestId) {
    json raw = {
        {"jsonrpc", "2.0"},
        {"id", "abc-123"},
        {"method", "ping"}
    };

    auto parsed = parse_message(raw);
    auto* req = std::get_if<JsonRpcRequest>(&parsed);
    ASSERT_NE(req, nullptr);
    EXPECT_EQ(std::get<std::string>(req->id), "abc-123");
}