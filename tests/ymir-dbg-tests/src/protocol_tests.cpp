#include <catch2/catch_test_macros.hpp>

#include <protocol/json_rpc_adapter.hpp>
#include <protocol/line_framer.hpp>

#include <string>
#include <string_view>
#include <variant>
#include <vector>

TEST_CASE("ymir-dbg LineFramer buffers protocol lines", "[protocol]") {
    std::vector<std::string> lines;
    ymir::debug::LineFramer framer([&](std::string_view line) { lines.emplace_back(line); },
                                   [](std::string_view err) { FAIL("Unexpected error: " << err); });

    const std::string first = R"({"jsonrpc":"2.0","id":"a)";
    const std::string second = R"(bc","result":{}})"
                               "\n";

    framer.Push(first.data(), first.size());
    CHECK(lines.empty());

    framer.Push(second.data(), second.size());
    REQUIRE(lines.size() == 1);
    CHECK(lines[0] == R"({"jsonrpc":"2.0","id":"abc","result":{}})");
}

TEST_CASE("ymir-dbg LineFramer recovers after oversized line", "[protocol]") {
    std::string error;
    std::vector<std::string> lines;
    ymir::debug::LineFramer framer([&](std::string_view line) { lines.emplace_back(line); },
                                   [&](std::string_view err) { error = err; });

    std::string oversized(ymir::debug::LineFramer::kMaxLineLength + 1, 'x');
    oversized += "\n";
    oversized += R"({"jsonrpc":"2.0","method":"debug.version","id":1})";
    oversized += "\n";

    framer.Push(oversized.data(), oversized.size());
    CHECK(error == "Line length limit exceeded");
    REQUIRE(lines.size() == 1);
    CHECK(lines[0] == R"({"jsonrpc":"2.0","method":"debug.version","id":1})");
}

TEST_CASE("ymir-dbg JsonRpcAdapter preserves request IDs", "[protocol]") {
    nlohmann::json error;

    auto integerRequest =
        ymir::debug::JsonRpcAdapter::ParseRequest(R"({"jsonrpc":"2.0","method":"debug.version","id":7})", error);
    REQUIRE(integerRequest.has_value());
    CHECK(std::get<int>(integerRequest->id) == 7);
    CHECK_FALSE(integerRequest->is_notification);

    auto stringRequest =
        ymir::debug::JsonRpcAdapter::ParseRequest(R"({"jsonrpc":"2.0","method":"debug.version","id":"req-7"})", error);
    REQUIRE(stringRequest.has_value());
    CHECK(std::get<std::string>(stringRequest->id) == "req-7");
}

TEST_CASE("ymir-dbg JsonRpcAdapter handles malformed and unsupported input", "[protocol]") {
    nlohmann::json error;

    auto malformed = ymir::debug::JsonRpcAdapter::ParseRequest(R"({"jsonrpc":"2.0","method":)", error);
    CHECK_FALSE(malformed.has_value());
    CHECK(error["error"]["code"] == static_cast<int>(ymir::debug::JsonRpcError::ParseError));

    auto batch = ymir::debug::JsonRpcAdapter::ParseRequest(R"([{"jsonrpc":"2.0","method":"debug.version"}])", error);
    CHECK_FALSE(batch.has_value());
    CHECK(error["error"]["code"] == static_cast<int>(ymir::debug::JsonRpcError::InvalidRequest));
}
