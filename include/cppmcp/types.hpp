#pragma once

#include <optional>
#include <string>
#include <variant>
#include <vector>

#include <nlohmann/json.hpp>

#include "common.hpp"

namespace cppmcp {

// --- Content types ---
struct TextContent {
    std::string type = "text";
    std::string text;
};

struct ImageContent {
    std::string type = "image";
    std::string data;      // base64-encoded
    std::string mime_type;
};

struct ResourceContents {
    std::string uri;
    std::string mime_type;
    std::optional<std::string> text;
    std::optional<std::string> blob; // base64-encoded
};

struct EmbeddedResource {
    std::string type = "resource";
    ResourceContents resource;
};

struct ResourceLink {
    std::string type = "resource_link";
    std::string name;
    std::string uri;
    std::optional<std::string> description;
    std::optional<std::string> mime_type;
};

using Content = std::variant<TextContent, ImageContent, EmbeddedResource, ResourceLink>;
using ToolResultContent = Content;

// --- Tool ---
struct ToolAnnotations {
    std::optional<bool> read_only_hint;
    std::optional<bool> destructive_hint;
    std::optional<bool> idempotent_hint;
    std::optional<bool> open_world_hint;
};

struct Tool {
    std::string name;
    std::optional<std::string> title;
    std::optional<std::string> description;
    nlohmann::json input_schema;
    std::optional<nlohmann::json> output_schema;
    std::optional<ToolAnnotations> annotations;
};

struct CallToolResult {
    std::vector<ToolResultContent> content;
    std::optional<nlohmann::json> structured_content;
    bool is_error = false;
};

// --- Resource ---
struct Resource {
    std::string name;
    std::optional<std::string> title;
    std::string uri;
    std::optional<std::string> description;
    std::optional<std::string> mime_type;
};

struct ReadResourceResult {
    std::vector<ResourceContents> contents;
};

// --- Resource template ---
struct ResourceTemplate {
    std::string name;
    std::optional<std::string> title;
    std::string uri_template;
    std::optional<std::string> description;
    std::optional<std::string> mime_type;
};

// --- Prompt ---
struct PromptArgument {
    std::string name;
    std::optional<std::string> description;
    std::optional<bool> required;
};

struct Prompt {
    std::string name;
    std::optional<std::string> title;
    std::optional<std::string> description;
    std::optional<std::vector<PromptArgument>> arguments;
};

struct PromptMessage {
    std::string role; // "user" or "assistant"
    Content content;
};

struct GetPromptResult {
    std::optional<std::string> description;
    std::vector<PromptMessage> messages;
};

// --- Capabilities ---
struct PromptsCapability {
    std::optional<bool> list_changed;
};

struct ResourcesCapability {
    std::optional<bool> subscribe;
    std::optional<bool> list_changed;
};

struct ToolsCapability {
    std::optional<bool> list_changed;
};

struct LoggingCapability {};

struct CompletionsCapability {};

struct ServerCapabilities {
    std::optional<PromptsCapability> prompts;
    std::optional<ResourcesCapability> resources;
    std::optional<ToolsCapability> tools;
    std::optional<LoggingCapability> logging;
    std::optional<CompletionsCapability> completions;
    std::optional<nlohmann::json> experimental;
};

struct SamplingCapability {};
struct RootsCapability {
    std::optional<bool> list_changed;
};
struct ElicitationCapability {};

struct ClientCapabilities {
    std::optional<SamplingCapability> sampling;
    std::optional<ElicitationCapability> elicitation;
    std::optional<RootsCapability> roots;
    std::optional<nlohmann::json> experimental;
};

// --- Initialization ---
struct Implementation {
    std::string name;
    std::string version;
};

struct InitializeResult {
    std::string protocol_version;
    ServerCapabilities capabilities;
    Implementation server_info;
    std::optional<std::string> instructions;
};

struct InitializeRequestParams {
    std::string protocol_version;
    ClientCapabilities capabilities;
    Implementation client_info;
};

// --- Empty result ---
struct EmptyResult {};

// --- Completion ---
struct CompletionArgument {
    std::string name;
    std::string value;
};

struct CompletionReference {
    std::string type; // "ref/resource" or "ref/prompt"
    std::string name;
};

struct Completion {
    std::vector<std::string> values;
    std::optional<bool> has_more;
    std::optional<int64_t> total;
};

struct CompleteResult {
    Completion completion;
};

// --- Logging ---
struct LoggingMessageNotificationParams {
    std::string level;
    std::string data;
    std::optional<std::string> logger;
};

// --- Progress ---
struct ProgressNotificationParams {
    RequestId progress_token;
    double progress;
    std::optional<double> total;
};

// --- Serialization helpers ---
nlohmann::json content_to_json(const Content& c);
Content content_from_json(const nlohmann::json& j);

// to_json / from_json overloads for nlohmann/json ADL serialization
void to_json(nlohmann::json& j, const TextContent& t);
void from_json(const nlohmann::json& j, TextContent& t);

void to_json(nlohmann::json& j, const ImageContent& i);
void from_json(const nlohmann::json& j, ImageContent& i);

void to_json(nlohmann::json& j, const ResourceContents& r);
void from_json(const nlohmann::json& j, ResourceContents& r);

void to_json(nlohmann::json& j, const EmbeddedResource& e);
void from_json(const nlohmann::json& j, EmbeddedResource& e);

void to_json(nlohmann::json& j, const ResourceLink& rl);
void from_json(const nlohmann::json& j, ResourceLink& rl);

void to_json(nlohmann::json& j, const ToolAnnotations& ta);
void from_json(const nlohmann::json& j, ToolAnnotations& ta);

void to_json(nlohmann::json& j, const Tool& t);
void from_json(const nlohmann::json& j, Tool& t);

void to_json(nlohmann::json& j, const CallToolResult& r);
void from_json(const nlohmann::json& j, CallToolResult& r);

void to_json(nlohmann::json& j, const Resource& r);
void from_json(const nlohmann::json& j, Resource& r);

void to_json(nlohmann::json& j, const ReadResourceResult& r);
void from_json(const nlohmann::json& j, ReadResourceResult& r);

void to_json(nlohmann::json& j, const ResourceTemplate& rt);
void from_json(const nlohmann::json& j, ResourceTemplate& rt);

void to_json(nlohmann::json& j, const PromptArgument& pa);
void from_json(const nlohmann::json& j, PromptArgument& pa);

void to_json(nlohmann::json& j, const Prompt& p);
void from_json(const nlohmann::json& j, Prompt& p);

void to_json(nlohmann::json& j, const PromptMessage& pm);
void from_json(const nlohmann::json& j, PromptMessage& pm);

void to_json(nlohmann::json& j, const GetPromptResult& r);
void from_json(const nlohmann::json& j, GetPromptResult& r);

void to_json(nlohmann::json& j, const PromptsCapability& c);
void to_json(nlohmann::json& j, const ResourcesCapability& c);
void to_json(nlohmann::json& j, const ToolsCapability& c);
void to_json(nlohmann::json& j, const LoggingCapability& c);
void to_json(nlohmann::json& j, const CompletionsCapability& c);

void to_json(nlohmann::json& j, const ServerCapabilities& c);
void from_json(const nlohmann::json& j, ServerCapabilities& c);

void to_json(nlohmann::json& j, const Implementation& impl);
void from_json(const nlohmann::json& j, Implementation& impl);

void to_json(nlohmann::json& j, const InitializeResult& r);
void from_json(const nlohmann::json& j, InitializeResult& r);

void to_json(nlohmann::json& j, const EmptyResult&);

void to_json(nlohmann::json& j, const Completion& c);
void to_json(nlohmann::json& j, const CompleteResult& r);

} // namespace cppmcp