#pragma once

#include <functional>
#include <memory>

#include <nlohmann/json.hpp>

#include "common.hpp"
#include "exception.hpp"
#include "types.hpp"

namespace cppmcp {

class McpServer;

// Forward declaration
class ITransport;

class RequestContext {
public:
    RequestContext(McpServer& server, const RequestId& request_id,
                  std::shared_ptr<ITransport> transport,
                  const std::string& session_id = std::string());

    void report_progress(double progress, std::optional<double> total = std::nullopt);

    void log(const std::string& level, const std::string& data,
             std::optional<std::string> logger = std::nullopt);

    // --- server -> client requests, bound to this request's session ---
    // Each returns false when the client did not declare the capability or the
    // transport is down. Callbacks run on an io thread; keep them fast.
    bool request_sampling(const CreateMessageRequestParams& params,
                          std::function<void(const CreateMessageResult&)> on_result = nullptr,
                          std::function<void(const McpException&)> on_error = nullptr) const;
    bool request_elicitation(const ElicitRequestParams& params,
                             std::function<void(const ElicitResult&)> on_result = nullptr,
                             std::function<void(const McpException&)> on_error = nullptr) const;
    bool request_roots(std::function<void(const ListRootsResult&)> on_result = nullptr,
                       std::function<void(const McpException&)> on_error = nullptr) const;

    const RequestId& request_id() const { return request_id_; }
    const std::string& session_id() const { return session_id_; }

    // Set a progress token (defaults to request_id if not explicitly set)
    void set_progress_token(const RequestId& token) { progress_token_ = token; }

private:
    McpServer& server_;
    RequestId request_id_;
    RequestId progress_token_;
    std::shared_ptr<ITransport> transport_;
    std::string session_id_;
};

} // namespace cppmcp
