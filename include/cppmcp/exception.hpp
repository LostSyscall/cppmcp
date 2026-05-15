#pragma once

#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>

#include "error_codes.hpp"

namespace cppmcp {

class McpException : public std::runtime_error {
public:
    explicit McpException(int code, const std::string& message,
                          std::optional<nlohmann::json> data = std::nullopt)
        : std::runtime_error(message), code_(code), message_str_(message), data_(std::move(data)) {}

    int code() const noexcept { return code_; }
    const std::string& message() const noexcept { return message_str_; }
    const std::optional<nlohmann::json>& data() const noexcept { return data_; }

private:
    int code_;
    std::string message_str_;
    std::optional<nlohmann::json> data_;
};

class ParseError : public McpException {
public:
    explicit ParseError(const std::string& msg = "Parse error")
        : McpException(Protocol::PARSE_ERROR, msg) {}
};

class InvalidRequestError : public McpException {
public:
    explicit InvalidRequestError(const std::string& msg = "Invalid request")
        : McpException(Protocol::INVALID_REQUEST, msg) {}
};

class MethodNotFoundError : public McpException {
public:
    explicit MethodNotFoundError(const std::string& method)
        : McpException(Protocol::METHOD_NOT_FOUND, "Method not found: " + method) {}
};

class InvalidParamsError : public McpException {
public:
    explicit InvalidParamsError(const std::string& msg = "Invalid params")
        : McpException(Protocol::INVALID_PARAMS, msg) {}
};

class InternalError : public McpException {
public:
    explicit InternalError(const std::string& msg = "Internal error")
        : McpException(Protocol::INTERNAL_ERROR, msg) {}
};

} // namespace cppmcp