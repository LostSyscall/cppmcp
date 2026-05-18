#include "cppmcp/types.hpp"

namespace cppmcp {

void to_json(nlohmann::json& j, const TextContent& t) {
    j = nlohmann::json{{"type", t.type}, {"text", t.text}};
}

void from_json(const nlohmann::json& j, TextContent& t) {
    j.at("type").get_to(t.type);
    j.at("text").get_to(t.text);
}

void to_json(nlohmann::json& j, const ImageContent& i) {
    j = nlohmann::json{{"type", i.type}, {"data", i.data}, {"mimeType", i.mime_type}};
}

void from_json(const nlohmann::json& j, ImageContent& i) {
    j.at("type").get_to(i.type);
    j.at("data").get_to(i.data);
    j.at("mimeType").get_to(i.mime_type);
}

void to_json(nlohmann::json& j, const ResourceContents& r) {
    j = nlohmann::json{{"uri", r.uri}, {"mimeType", r.mime_type}};
    if (r.text) j["text"] = *r.text;
    if (r.blob) j["blob"] = *r.blob;
}

void from_json(const nlohmann::json& j, ResourceContents& r) {
    j.at("uri").get_to(r.uri);
    if (j.contains("mimeType")) j.at("mimeType").get_to(r.mime_type);
    if (j.contains("text")) j.at("text").get_to(r.text.emplace());
    if (j.contains("blob")) j.at("blob").get_to(r.blob.emplace());
}

void to_json(nlohmann::json& j, const EmbeddedResource& e) {
    j = nlohmann::json{{"type", e.type}, {"resource", e.resource}};
}

void from_json(const nlohmann::json& j, EmbeddedResource& e) {
    j.at("type").get_to(e.type);
    j.at("resource").get_to(e.resource);
}

void to_json(nlohmann::json& j, const ResourceLink& rl) {
    j = nlohmann::json{{"type", rl.type}, {"name", rl.name}, {"uri", rl.uri}};
    if (rl.description) j["description"] = *rl.description;
    if (rl.mime_type) j["mimeType"] = *rl.mime_type;
}

void from_json(const nlohmann::json& j, ResourceLink& rl) {
    j.at("type").get_to(rl.type);
    j.at("name").get_to(rl.name);
    j.at("uri").get_to(rl.uri);
    if (j.contains("description")) j.at("description").get_to(rl.description.emplace());
    if (j.contains("mimeType")) j.at("mimeType").get_to(rl.mime_type.emplace());
}

nlohmann::json content_to_json(const Content& c) {
    return std::visit([](const auto& v) { return nlohmann::json(v); }, c);
}

Content content_from_json(const nlohmann::json& j) {
    std::string type = j.at("type").get<std::string>();
    if (type == "text") return j.get<TextContent>();
    if (type == "image") return j.get<ImageContent>();
    if (type == "resource") return j.get<EmbeddedResource>();
    if (type == "resource_link") return j.get<ResourceLink>();
    throw std::invalid_argument("Unknown content type: " + type);
}

void to_json(nlohmann::json& j, const ToolAnnotations& ta) {
    j = nlohmann::json::object();
    if (ta.read_only_hint) j["readOnlyHint"] = *ta.read_only_hint;
    if (ta.destructive_hint) j["destructiveHint"] = *ta.destructive_hint;
    if (ta.idempotent_hint) j["idempotentHint"] = *ta.idempotent_hint;
    if (ta.open_world_hint) j["openWorldHint"] = *ta.open_world_hint;
}

void from_json(const nlohmann::json& j, ToolAnnotations& ta) {
    if (j.contains("readOnlyHint")) j.at("readOnlyHint").get_to(ta.read_only_hint.emplace());
    if (j.contains("destructiveHint")) j.at("destructiveHint").get_to(ta.destructive_hint.emplace());
    if (j.contains("idempotentHint")) j.at("idempotentHint").get_to(ta.idempotent_hint.emplace());
    if (j.contains("openWorldHint")) j.at("openWorldHint").get_to(ta.open_world_hint.emplace());
}

void to_json(nlohmann::json& j, const Tool& t) {
    j = nlohmann::json{{"name", t.name}, {"inputSchema", t.input_schema}};
    if (t.title) j["title"] = *t.title;
    if (t.description) j["description"] = *t.description;
    if (t.output_schema) j["outputSchema"] = *t.output_schema;
    if (t.annotations) j["annotations"] = *t.annotations;
}

void from_json(const nlohmann::json& j, Tool& t) {
    j.at("name").get_to(t.name);
    j.at("inputSchema").get_to(t.input_schema);
    if (j.contains("title")) j.at("title").get_to(t.title.emplace());
    if (j.contains("description")) j.at("description").get_to(t.description.emplace());
    if (j.contains("outputSchema")) j.at("outputSchema").get_to(t.output_schema.emplace());
    if (j.contains("annotations")) j.at("annotations").get_to(t.annotations.emplace());
}

void to_json(nlohmann::json& j, const CallToolResult& r) {
    std::vector<nlohmann::json> content_json;
    content_json.reserve(r.content.size());
    for (const auto& c : r.content) {
        content_json.emplace_back(content_to_json(c));
    }
    j = nlohmann::json::object();
    j["content"] = std::move(content_json);
    if (r.structured_content) j["structuredContent"] = *r.structured_content;
    j["isError"] = r.is_error;
}

void from_json(const nlohmann::json& j, CallToolResult& r) {
    for (const auto& item : j.at("content")) {
        r.content.push_back(content_from_json(item));
    }
    if (j.contains("structuredContent")) j.at("structuredContent").get_to(r.structured_content.emplace());
    j.at("isError").get_to(r.is_error);
}

void to_json(nlohmann::json& j, const Resource& r) {
    j = nlohmann::json{{"name", r.name}, {"uri", r.uri}};
    if (r.title) j["title"] = *r.title;
    if (r.description) j["description"] = *r.description;
    if (r.mime_type) j["mimeType"] = *r.mime_type;
}

void from_json(const nlohmann::json& j, Resource& r) {
    j.at("name").get_to(r.name);
    j.at("uri").get_to(r.uri);
    if (j.contains("title")) j.at("title").get_to(r.title.emplace());
    if (j.contains("description")) j.at("description").get_to(r.description.emplace());
    if (j.contains("mimeType")) j.at("mimeType").get_to(r.mime_type.emplace());
}

void to_json(nlohmann::json& j, const ReadResourceResult& r) {
    j = nlohmann::json{{"contents", r.contents}};
}

void from_json(const nlohmann::json& j, ReadResourceResult& r) {
    j.at("contents").get_to(r.contents);
}

void to_json(nlohmann::json& j, const ResourceTemplate& rt) {
    j = nlohmann::json{{"name", rt.name}, {"uriTemplate", rt.uri_template}};
    if (rt.title) j["title"] = *rt.title;
    if (rt.description) j["description"] = *rt.description;
    if (rt.mime_type) j["mimeType"] = *rt.mime_type;
}

void from_json(const nlohmann::json& j, ResourceTemplate& rt) {
    j.at("name").get_to(rt.name);
    j.at("uriTemplate").get_to(rt.uri_template);
    if (j.contains("title")) j.at("title").get_to(rt.title.emplace());
    if (j.contains("description")) j.at("description").get_to(rt.description.emplace());
    if (j.contains("mimeType")) j.at("mimeType").get_to(rt.mime_type.emplace());
}

void to_json(nlohmann::json& j, const PromptArgument& pa) {
    j = nlohmann::json{{"name", pa.name}};
    if (pa.description) j["description"] = *pa.description;
    if (pa.required) j["required"] = *pa.required;
}

void from_json(const nlohmann::json& j, PromptArgument& pa) {
    j.at("name").get_to(pa.name);
    if (j.contains("description")) j.at("description").get_to(pa.description.emplace());
    if (j.contains("required")) j.at("required").get_to(pa.required.emplace());
}

void to_json(nlohmann::json& j, const Prompt& p) {
    j = nlohmann::json{{"name", p.name}};
    if (p.title) j["title"] = *p.title;
    if (p.description) j["description"] = *p.description;
    if (p.arguments) j["arguments"] = *p.arguments;
}

void from_json(const nlohmann::json& j, Prompt& p) {
    j.at("name").get_to(p.name);
    if (j.contains("title")) j.at("title").get_to(p.title.emplace());
    if (j.contains("description")) j.at("description").get_to(p.description.emplace());
    if (j.contains("arguments")) j.at("arguments").get_to(p.arguments.emplace());
}

void to_json(nlohmann::json& j, const PromptMessage& pm) {
    j = nlohmann::json{{"role", pm.role}, {"content", content_to_json(pm.content)}};
}

void from_json(const nlohmann::json& j, PromptMessage& pm) {
    j.at("role").get_to(pm.role);
    pm.content = content_from_json(j.at("content"));
}

void to_json(nlohmann::json& j, const GetPromptResult& r) {
    j = nlohmann::json::object();
    if (r.description) j["description"] = *r.description;
    j["messages"] = r.messages;
}

void from_json(const nlohmann::json& j, GetPromptResult& r) {
    if (j.contains("description")) j.at("description").get_to(r.description.emplace());
    j.at("messages").get_to(r.messages);
}

void to_json(nlohmann::json& j, const PromptsCapability& c) {
    j = nlohmann::json::object();
    if (c.list_changed) j["listChanged"] = *c.list_changed;
}

void to_json(nlohmann::json& j, const ResourcesCapability& c) {
    j = nlohmann::json::object();
    if (c.subscribe) j["subscribe"] = *c.subscribe;
    if (c.list_changed) j["listChanged"] = *c.list_changed;
}

void to_json(nlohmann::json& j, const ToolsCapability& c) {
    j = nlohmann::json::object();
    if (c.list_changed) j["listChanged"] = *c.list_changed;
}

void to_json(nlohmann::json& j, const LoggingCapability&) {
    j = nlohmann::json::object();
}

void to_json(nlohmann::json& j, const CompletionsCapability&) {
    j = nlohmann::json::object();
}

void to_json(nlohmann::json& j, const ServerCapabilities& c) {
    j = nlohmann::json::object();
    if (c.prompts) { nlohmann::json sub; to_json(sub, *c.prompts); j["prompts"] = sub; }
    if (c.resources) { nlohmann::json sub; to_json(sub, *c.resources); j["resources"] = sub; }
    if (c.tools) { nlohmann::json sub; to_json(sub, *c.tools); j["tools"] = sub; }
    if (c.logging) { nlohmann::json sub; to_json(sub, *c.logging); j["logging"] = sub; }
    if (c.completions) { nlohmann::json sub; to_json(sub, *c.completions); j["completions"] = sub; }
    if (c.experimental) j["experimental"] = *c.experimental;
}

void from_json(const nlohmann::json& j, ServerCapabilities& c) {
    if (j.contains("prompts")) {
        PromptsCapability pc;
        if (j.at("prompts").contains("listChanged"))
            pc.list_changed = j.at("prompts").at("listChanged").get<bool>();
        c.prompts = pc;
    }
    if (j.contains("resources")) {
        ResourcesCapability rc;
        if (j.at("resources").contains("subscribe"))
            rc.subscribe = j.at("resources").at("subscribe").get<bool>();
        if (j.at("resources").contains("listChanged"))
            rc.list_changed = j.at("resources").at("listChanged").get<bool>();
        c.resources = rc;
    }
    if (j.contains("tools")) {
        ToolsCapability tc;
        if (j.at("tools").contains("listChanged"))
            tc.list_changed = j.at("tools").at("listChanged").get<bool>();
        c.tools = tc;
    }
    if (j.contains("logging")) c.logging = LoggingCapability{};
    if (j.contains("completions")) c.completions = CompletionsCapability{};
    if (j.contains("experimental")) c.experimental = j.at("experimental");
}

void to_json(nlohmann::json& j, const Implementation& impl) {
    j = nlohmann::json{{"name", impl.name}, {"version", impl.version}};
}

void from_json(const nlohmann::json& j, Implementation& impl) {
    j.at("name").get_to(impl.name);
    j.at("version").get_to(impl.version);
}

void to_json(nlohmann::json& j, const InitializeResult& r) {
    j = nlohmann::json{
        {"protocolVersion", r.protocol_version},
        {"capabilities", r.capabilities},
        {"serverInfo", r.server_info}
    };
    if (r.instructions) j["instructions"] = *r.instructions;
}

void from_json(const nlohmann::json& j, InitializeResult& r) {
    j.at("protocolVersion").get_to(r.protocol_version);
    j.at("capabilities").get_to(r.capabilities);
    j.at("serverInfo").get_to(r.server_info);
    if (j.contains("instructions")) j.at("instructions").get_to(r.instructions.emplace());
}

void to_json(nlohmann::json& j, const EmptyResult&) {
    j = nlohmann::json::object();
}

void to_json(nlohmann::json& j, const Completion& c) {
    j = nlohmann::json{{"values", c.values}};
    if (c.has_more) j["hasMore"] = *c.has_more;
    if (c.total) j["total"] = *c.total;
}

void to_json(nlohmann::json& j, const CompleteResult& r) {
    j = nlohmann::json{{"completion", r.completion}};
}

} // namespace cppmcp