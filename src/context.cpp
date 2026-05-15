#include "cppmcp/context.hpp"
#include "cppmcp/server.hpp"

namespace cppmcp {

RequestContext::RequestContext(McpServer& server, const RequestId& request_id,
                               std::shared_ptr<ITransport> transport)
    : server_(server), request_id_(request_id), progress_token_(request_id), transport_(std::move(transport)) {}

void RequestContext::report_progress(double progress, std::optional<double> total) {
    server_.notify_progress(progress_token_, progress, total);
}

void RequestContext::log(const std::string& level, const std::string& data,
                         std::optional<std::string> logger) {
    server_.notify_logging(level, data, logger);
}

} // namespace cppmcp