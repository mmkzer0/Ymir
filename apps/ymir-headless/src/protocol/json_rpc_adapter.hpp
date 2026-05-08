#pragma once

#include <nlohmann/json.hpp>

#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

namespace ymir::debug {

using JsonRpcId = std::variant<std::monostate, int, std::string>;

struct JsonRpcRequest {
    JsonRpcId id;
    std::string method;
    nlohmann::json params;
    bool is_notification{false};
};

struct JsonRpcResponse {
    JsonRpcId id;
    nlohmann::json result;
    std::optional<nlohmann::json> error;
};

struct JsonRpcNotification {
    std::string method;
    nlohmann::json params;
};

enum class JsonRpcError : int {
    ParseError = -32700,
    InvalidRequest = -32600,
    MethodNotFound = -32601,
    InvalidParams = -32602,
    InternalError = -32603,
};

class JsonRpcAdapter {
public:
    static nlohmann::json CreateErrorResponse(const JsonRpcId &id, JsonRpcError code, std::string_view message,
                                              const nlohmann::json &data = nullptr) {
        nlohmann::json response = {{"jsonrpc", "2.0"},
                                   {"id", IdToJson(id)},
                                   {"error", {{"code", static_cast<int>(code)}, {"message", message}}}};
        if (!data.is_null()) {
            response["error"]["data"] = data;
        }
        return response;
    }

    static nlohmann::json CreateSuccessResponse(const JsonRpcId &id, const nlohmann::json &result) {
        return {{"jsonrpc", "2.0"}, {"id", IdToJson(id)}, {"result", result}};
    }

    static nlohmann::json CreateNotification(std::string_view method, const nlohmann::json &params) {
        return {{"jsonrpc", "2.0"}, {"method", method}, {"params", params}};
    }

    static nlohmann::json CreateMethodNotFoundResponse(const JsonRpcId &id, std::string_view method) {
        return CreateErrorResponse(id, JsonRpcError::MethodNotFound, "Method not found",
                                   nlohmann::json{{"method", method}});
    }

    static std::optional<JsonRpcRequest> ParseRequest(std::string_view line, nlohmann::json &outError) {
        try {
            auto j = nlohmann::json::parse(line);

            if (j.is_array()) {
                outError = CreateErrorResponse(std::monostate{}, JsonRpcError::InvalidRequest,
                                               "Batch requests are not supported");
                return std::nullopt;
            }

            if (!j.is_object()) {
                outError = CreateErrorResponse(std::monostate{}, JsonRpcError::InvalidRequest, "Invalid request shape");
                return std::nullopt;
            }

            if (j.value("jsonrpc", "") != "2.0") {
                outError = CreateErrorResponse(std::monostate{}, JsonRpcError::InvalidRequest,
                                               "Invalid or missing jsonrpc version");
                return std::nullopt;
            }

            if (!j.contains("method") || !j["method"].is_string()) {
                outError =
                    CreateErrorResponse(std::monostate{}, JsonRpcError::InvalidRequest, "Missing or invalid method");
                return std::nullopt;
            }

            JsonRpcRequest req;
            req.method = j["method"].get<std::string>();
            req.params = j.value("params", nlohmann::json::object());

            if (j.contains("id")) {
                if (j["id"].is_number_integer()) {
                    req.id = j["id"].get<int>();
                } else if (j["id"].is_string()) {
                    req.id = j["id"].get<std::string>();
                } else if (j["id"].is_null()) {
                    req.id = std::monostate{};
                } else {
                    outError = CreateErrorResponse(std::monostate{}, JsonRpcError::InvalidRequest, "Invalid id type");
                    return std::nullopt;
                }
                req.is_notification = false;
            } else {
                req.is_notification = true;
            }

            return req;
        } catch (const nlohmann::json::parse_error &) {
            outError = CreateErrorResponse(std::monostate{}, JsonRpcError::ParseError, "Parse error");
            return std::nullopt;
        }
    }

private:
    static nlohmann::json IdToJson(const JsonRpcId &id) {
        return std::visit(
            [](auto &&arg) -> nlohmann::json {
                using T = std::decay_t<decltype(arg)>;
                if constexpr (std::is_same_v<T, std::monostate>) {
                    return nullptr;
                } else {
                    return arg;
                }
            },
            id);
    }
};

} // namespace ymir::debug
