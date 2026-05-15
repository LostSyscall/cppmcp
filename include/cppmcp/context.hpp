#pragma once

#include <functional>
#include <memory>

#include <nlohmann/json.hpp>

#include "common.hpp"

namespace cppmcp {

class McpServer;

// Forward declaration
class ITransport;

class RequestContext {
public:
    RequestContext(McpServer& server, const RequestId& request_id,
                  std::shared_ptr<ITransport> transport);

    void report_progress(double progress, std::optional<double> total = std::nullopt);

    void log(const std::string& level, const std::string& data,
             std::optional<std::string> logger = std::nullopt);

    const RequestId& request_id() const { return request_id_; }

    // Set a progress token (defaults to request_id if not explicitly set)
    void set_progress_token(const RequestId& token) { progress_token_ = token; }

private:
    McpServer& server_;
    RequestId request_id_;
    RequestId progress_token_;
    std::shared_ptr<ITransport> transport_;
};

} // namespace cppmcp