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

void to_json(nlohmann::json& j, const AudioContent& a) {
    j = nlohmann::json{{"type", a.type}, {"data", a.data}, {"mimeType", a.mime_type}};
}

void from_json(const nlohmann::json& j, AudioContent& a) {
    j.at("type").get_to(a.type);
    j.at("data").get_to(a.data);
    j.at("mimeType").get_to(a.mime_type);
}

void to_json(nlohmann::json& j, const ResourceContents& r) {
    j = nlohmann::json{{"uri", r.uri}};
    if (!r.mime_type.empty()) j["mimeType"] = r.mime_type;
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
    if (type == "audio") return j.get<AudioContent>();
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
    if (r.is_error) j["isError"] = true;  // omit on success (MCP spec: false is the default)
}

void from_json(const nlohmann::json& j, CallToolResult& r) {
    for (const auto& item : j.at("content")) {
        r.content.push_back(content_from_json(item));
    }
    if (j.contains("structuredContent")) j.at("structuredContent").get_to(r.structured_content.emplace());
    r.is_error = j.value("isError", false);  // tolerate third-party servers omitting it
}

void to_json(nlohmann::json& j, const ResourceAnnotations& ra) {
    j = nlohmann::json::object();
    if (ra.audience) j["audience"] = *ra.audience;
    if (ra.priority) j["priority"] = *ra.priority;
}

void from_json(const nlohmann::json& j, ResourceAnnotations& ra) {
    if (j.contains("audience")) j.at("audience").get_to(ra.audience.emplace());
    if (j.contains("priority")) j.at("priority").get_to(ra.priority.emplace());
}

void to_json(nlohmann::json& j, const Resource& r) {
    j = nlohmann::json{{"name", r.name}, {"uri", r.uri}};
    if (r.title) j["title"] = *r.title;
    if (r.description) j["description"] = *r.description;
    if (r.mime_type) j["mimeType"] = *r.mime_type;
    if (r.annotations) {
        nlohmann::json sub;
        to_json(sub, *r.annotations);
        j["annotations"] = sub;
    }
}

void from_json(const nlohmann::json& j, Resource& r) {
    j.at("name").get_to(r.name);
    j.at("uri").get_to(r.uri);
    if (j.contains("title")) j.at("title").get_to(r.title.emplace());
    if (j.contains("description")) j.at("description").get_to(r.description.emplace());
    if (j.contains("mimeType")) j.at("mimeType").get_to(r.mime_type.emplace());
    if (j.contains("annotations")) {
        ResourceAnnotations ra;
        from_json(j.at("annotations"), ra);
        r.annotations = ra;
    }
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

void to_json(nlohmann::json& j, const SamplingCapability&) {
    j = nlohmann::json::object();
}

void to_json(nlohmann::json& j, const ElicitationCapability&) {
    j = nlohmann::json::object();
}

void to_json(nlohmann::json& j, const RootsCapability& c) {
    j = nlohmann::json::object();
    if (c.list_changed) j["listChanged"] = *c.list_changed;
}

void to_json(nlohmann::json& j, const ClientCapabilities& c) {
    j = nlohmann::json::object();
    if (c.sampling) { nlohmann::json sub; to_json(sub, *c.sampling); j["sampling"] = sub; }
    if (c.elicitation) { nlohmann::json sub; to_json(sub, *c.elicitation); j["elicitation"] = sub; }
    if (c.roots) { nlohmann::json sub; to_json(sub, *c.roots); j["roots"] = sub; }
    if (c.experimental) j["experimental"] = *c.experimental;
}

void from_json(const nlohmann::json& j, ClientCapabilities& c) {
    if (j.contains("sampling")) c.sampling = SamplingCapability{};
    if (j.contains("elicitation")) c.elicitation = ElicitationCapability{};
    if (j.contains("roots")) {
        RootsCapability rc;
        if (j.at("roots").contains("listChanged")) {
            rc.list_changed = j.at("roots").at("listChanged").get<bool>();
        }
        c.roots = rc;
    }
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

void to_json(nlohmann::json& j, const ModelHint& h) {
    j = nlohmann::json::object();
    if (h.name) j["name"] = *h.name;
}

void from_json(const nlohmann::json& j, ModelHint& h) {
    if (j.contains("name")) j.at("name").get_to(h.name.emplace());
}

void to_json(nlohmann::json& j, const ModelPreferences& p) {
    j = nlohmann::json::object();
    if (!p.hints.empty()) {
        nlohmann::json hints = nlohmann::json::array();
        for (const auto& h : p.hints) {
            nlohmann::json hj;
            to_json(hj, h);
            hints.push_back(hj);
        }
        j["hints"] = std::move(hints);
    }
    if (p.cost_priority) j["costPriority"] = *p.cost_priority;
    if (p.speed_priority) j["speedPriority"] = *p.speed_priority;
    if (p.intelligence_priority) j["intelligencePriority"] = *p.intelligence_priority;
}

void from_json(const nlohmann::json& j, ModelPreferences& p) {
    if (j.contains("hints")) {
        for (const auto& item : j.at("hints")) {
            ModelHint h;
            from_json(item, h);
            p.hints.push_back(h);
        }
    }
    if (j.contains("costPriority")) j.at("costPriority").get_to(p.cost_priority.emplace());
    if (j.contains("speedPriority")) j.at("speedPriority").get_to(p.speed_priority.emplace());
    if (j.contains("intelligencePriority")) j.at("intelligencePriority").get_to(p.intelligence_priority.emplace());
}

void to_json(nlohmann::json& j, const SamplingMessage& m) {
    j = nlohmann::json{{"role", m.role}, {"content", content_to_json(m.content)}};
}

void from_json(const nlohmann::json& j, SamplingMessage& m) {
    j.at("role").get_to(m.role);
    m.content = content_from_json(j.at("content"));
}

void to_json(nlohmann::json& j, const CreateMessageRequestParams& p) {
    nlohmann::json messages = nlohmann::json::array();
    for (const auto& m : p.messages) {
        nlohmann::json mj;
        to_json(mj, m);
        messages.push_back(mj);
    }
    j = nlohmann::json{{"messages", messages}};
    if (p.model_preferences) {
        nlohmann::json mp;
        to_json(mp, *p.model_preferences);
        j["modelPreferences"] = std::move(mp);
    }
    if (p.system_prompt) j["systemPrompt"] = *p.system_prompt;
    if (p.include_context) j["includeContext"] = *p.include_context;
    if (p.temperature) j["temperature"] = *p.temperature;
    if (p.max_tokens) j["maxTokens"] = *p.max_tokens;
    if (!p.stop_sequences.empty()) j["stopSequences"] = p.stop_sequences;
    if (p.metadata) j["metadata"] = *p.metadata;
}

void from_json(const nlohmann::json& j, CreateMessageRequestParams& p) {
    for (const auto& item : j.at("messages")) {
        SamplingMessage m;
        from_json(item, m);
        p.messages.push_back(m);
    }
    if (j.contains("modelPreferences")) {
        ModelPreferences mp;
        from_json(j.at("modelPreferences"), mp);
        p.model_preferences = mp;
    }
    if (j.contains("systemPrompt")) j.at("systemPrompt").get_to(p.system_prompt.emplace());
    if (j.contains("includeContext")) j.at("includeContext").get_to(p.include_context.emplace());
    if (j.contains("temperature")) j.at("temperature").get_to(p.temperature.emplace());
    if (j.contains("maxTokens")) j.at("maxTokens").get_to(p.max_tokens.emplace());
    if (j.contains("stopSequences")) j.at("stopSequences").get_to(p.stop_sequences);
    if (j.contains("metadata")) p.metadata = j.at("metadata");
}

void to_json(nlohmann::json& j, const CreateMessageResult& r) {
    j = nlohmann::json{
        {"role", r.role},
        {"content", content_to_json(r.content)},
        {"model", r.model},
        {"stopReason", r.stop_reason}
    };
}

void from_json(const nlohmann::json& j, CreateMessageResult& r) {
    j.at("role").get_to(r.role);
    r.content = content_from_json(j.at("content"));
    j.at("model").get_to(r.model);
    if (j.contains("stopReason")) j.at("stopReason").get_to(r.stop_reason);
}

void to_json(nlohmann::json& j, const ElicitRequestParams& p) {
    j = nlohmann::json{{"message", p.message}};
    if (p.requested_schema) j["requestedSchema"] = *p.requested_schema;
}

void from_json(const nlohmann::json& j, ElicitRequestParams& p) {
    j.at("message").get_to(p.message);
    if (j.contains("requestedSchema")) p.requested_schema = j.at("requestedSchema");
}

void to_json(nlohmann::json& j, const ElicitResult& r) {
    j = nlohmann::json{{"action", r.action}};
    if (r.content) j["content"] = *r.content;
}

void from_json(const nlohmann::json& j, ElicitResult& r) {
    j.at("action").get_to(r.action);
    if (j.contains("content")) r.content = j.at("content");
}

void to_json(nlohmann::json& j, const Root& r) {
    j = nlohmann::json{{"uri", r.uri}};
    if (r.name) j["name"] = *r.name;
}

void from_json(const nlohmann::json& j, Root& r) {
    j.at("uri").get_to(r.uri);
    if (j.contains("name")) j.at("name").get_to(r.name.emplace());
}

void to_json(nlohmann::json& j, const ListRootsResult& r) {
    j = nlohmann::json{{"roots", r.roots}};
}

void from_json(const nlohmann::json& j, ListRootsResult& r) {
    j.at("roots").get_to(r.roots);
}

} // namespace cppmcp