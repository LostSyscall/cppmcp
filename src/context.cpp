#include "cppmcp/context.hpp"
#include "cppmcp/server.hpp"

namespace cppmcp {

RequestContext::RequestContext(McpServer& server, const RequestId& request_id,
                               std::shared_ptr<ITransport> transport,
                               const std::string& session_id)
    : server_(server), request_id_(request_id), progress_token_(request_id),
      transport_(std::move(transport)), session_id_(session_id) {}

void RequestContext::report_progress(double progress, std::optional<double> total) {
    server_.notify_progress(progress_token_, progress, total, session_id_);
}

void RequestContext::log(const std::string& level, const std::string& data,
                         std::optional<std::string> logger) {
    server_.notify_logging(level, data, logger, session_id_);
}

bool RequestContext::request_sampling(const CreateMessageRequestParams& params,
                                      std::function<void(const CreateMessageResult&)> on_result,
                                      std::function<void(const McpException&)> on_error) const {
    return server_.request_sampling(params, std::move(on_result), std::move(on_error), session_id_);
}

bool RequestContext::request_elicitation(const ElicitRequestParams& params,
                                         std::function<void(const ElicitResult&)> on_result,
                                         std::function<void(const McpException&)> on_error) const {
    return server_.request_elicitation(params, std::move(on_result), std::move(on_error), session_id_);
}

bool RequestContext::request_roots(std::function<void(const ListRootsResult&)> on_result,
                                   std::function<void(const McpException&)> on_error) const {
    return server_.request_roots(std::move(on_result), std::move(on_error), session_id_);
}

} // namespace cppmcp
