#include <gtest/gtest.h>

#include <cppmcp/types.hpp>

using namespace cppmcp;
using json = nlohmann::json;

TEST(TypesTest, TextContentSerialization) {
    TextContent tc;
    tc.type = "text";
    tc.text = "Hello world";
    json j = tc;
    EXPECT_EQ(j["type"], "text");
    EXPECT_EQ(j["text"], "Hello world");

    TextContent tc2 = j.get<TextContent>();
    EXPECT_EQ(tc2.text, "Hello world");
}

TEST(TypesTest, ImageContentSerialization) {
    ImageContent ic;
    ic.type = "image";
    ic.data = "base64data";
    ic.mime_type = "image/png";
    json j = ic;
    EXPECT_EQ(j["type"], "image");
    EXPECT_EQ(j["data"], "base64data");
    EXPECT_EQ(j["mimeType"], "image/png");
}

TEST(TypesTest, ToolSerialization) {
    Tool t;
    t.name = "search";
    t.description = "Search for items";
    t.input_schema = R"({"type": "object"})"_json;
    t.annotations = ToolAnnotations();
    t.annotations->read_only_hint = true;
    json j = t;
    EXPECT_EQ(j["name"], "search");
    EXPECT_EQ(j["description"], "Search for items");
    EXPECT_TRUE(j["annotations"]["readOnlyHint"].get<bool>());

    Tool t2 = j.get<Tool>();
    EXPECT_EQ(t2.name, "search");
}

TEST(TypesTest, CallToolResultSerialization) {
    CallToolResult result;
    TextContent tc;
    tc.type = "text";
    tc.text = "Result data";
    result.content.push_back(tc);
    result.is_error = false;
    json j = result;
    EXPECT_EQ(j["content"][0]["type"], "text");
    EXPECT_EQ(j["content"][0]["text"], "Result data");
    // isError is omitted when false (MCP spec: false is the default).
    EXPECT_FALSE(j.value("isError", false));
}

TEST(TypesTest, ResourceSerialization) {
    Resource r;
    r.name = "config";
    r.uri = "file:///config.json";
    r.description = "Server config";
    r.mime_type = "application/json";
    json j = r;
    EXPECT_EQ(j["name"], "config");
    EXPECT_EQ(j["uri"], "file:///config.json");

    Resource r2 = j.get<Resource>();
    EXPECT_EQ(r2.name, "config");
}

TEST(TypesTest, PromptSerialization) {
    Prompt p;
    p.name = "code_review";
    p.description = "Code review template";
    PromptArgument arg;
    arg.name = "code";
    arg.required = true;
    p.arguments = std::vector<PromptArgument>{arg};
    json j = p;
    EXPECT_EQ(j["name"], "code_review");
    EXPECT_EQ(j["arguments"][0]["name"], "code");
    EXPECT_TRUE(j["arguments"][0]["required"].get<bool>());
}

TEST(TypesTest, ServerCapabilitiesSerialization) {
    ServerCapabilities caps;
    caps.tools = ToolsCapability();
    caps.tools->list_changed = false;
    caps.resources = ResourcesCapability();
    caps.resources->subscribe = true;
    caps.resources->list_changed = false;
    caps.prompts = PromptsCapability();
    caps.prompts->list_changed = true;

    json j = caps;
    EXPECT_TRUE(j.contains("tools"));
    EXPECT_TRUE(j.contains("resources"));
    EXPECT_TRUE(j.contains("prompts"));
    EXPECT_TRUE(j["resources"]["subscribe"].get<bool>());
}

TEST(TypesTest, InitializeResultSerialization) {
    InitializeResult ir;
    ir.protocol_version = "2025-03-26";
    ir.capabilities = ServerCapabilities();
    ir.server_info.name = "test_server";
    ir.server_info.version = "1.0.0";
    ir.instructions = "Welcome!";
    json j = ir;
    EXPECT_EQ(j["protocolVersion"], "2025-03-26");
    EXPECT_EQ(j["serverInfo"]["name"], "test_server");
    EXPECT_EQ(j["instructions"], "Welcome!");
}

TEST(TypesTest, ContentVariant) {
    Content c = TextContent{"text", "hello"};
    json j = content_to_json(c);
    EXPECT_EQ(j["type"], "text");
    EXPECT_EQ(j["text"], "hello");

    c = ImageContent{"image", "abc", "image/png"};
    j = content_to_json(c);
    EXPECT_EQ(j["type"], "image");

    json input = {{ "type", "text" }, { "text", "deserialized" }};
    Content c2 = content_from_json(input);
    auto* tc = std::get_if<TextContent>(&c2);
    ASSERT_NE(tc, nullptr);
    EXPECT_EQ(tc->text, "deserialized");
}

TEST(TypesTest, ResourceContentsWithText) {
    ResourceContents rc;
    rc.uri = "file:///test.txt";
    rc.mime_type = "text/plain";
    rc.text = "Hello";
    json j = rc;
    EXPECT_EQ(j["uri"], "file:///test.txt");
    EXPECT_TRUE(j.contains("text"));
    EXPECT_FALSE(j.contains("blob"));
}