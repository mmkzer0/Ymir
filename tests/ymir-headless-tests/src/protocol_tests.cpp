#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_tostring.hpp>

#include <ymir/debug/protocol/debug_command.hpp>
#include <ymir/debug/protocol/debug_event.hpp>
#include <ymir/debug/protocol/debug_result.hpp>
#include <ymir/debug/protocol/debug_types.hpp>
#include <ymir/debug/protocol/protocol_version.hpp>

#include <protocol/json_rpc_adapter.hpp>
#include <protocol/line_framer.hpp>

#include <cstring>
#include <variant>

TEST_CASE("Protocol version constants", "[protocol]") {
    CHECK(ymir::debug::kProtocolName == "ymir-debug");
    CHECK(ymir::debug::kProtocolVersion == "0.1.0");
    CHECK(ymir::debug::kTransport == "stdio-jsonrpc-lines");
}

TEST_CASE("DebugTarget string round-trip", "[protocol]") {
    CHECK(ToString(ymir::debug::DebugTarget::Sh2Master) == "sh2.master");
    CHECK(ToString(ymir::debug::DebugTarget::Sh2Slave) == "sh2.slave");
}

TEST_CASE("ExecutionState string round-trip", "[protocol]") {
    auto s = ymir::debug::ExecutionState::Starting;
    CHECK(ToString(s) == "starting");
    s = ymir::debug::ExecutionState::Paused;
    CHECK(ToString(s) == "paused");
    s = ymir::debug::ExecutionState::Running;
    CHECK(ToString(s) == "running");
}

TEST_CASE("StopReason string round-trip", "[protocol]") {
    CHECK(ToString(ymir::debug::StopReason::Breakpoint) == "breakpoint");
    CHECK(ToString(ymir::debug::StopReason::Step) == "step");
    CHECK(ToString(ymir::debug::StopReason::Pause) == "pause");
}

TEST_CASE("ErrorCode string round-trip", "[protocol]") {
    CHECK(ToString(ymir::debug::ErrorCode::InvalidState) == "invalid_state");
    CHECK(ToString(ymir::debug::ErrorCode::TargetDisabled) == "target_disabled");
    CHECK(ToString(ymir::debug::ErrorCode::InvalidAddress) == "invalid_address");
}

TEST_CASE("CommandMethod string round-trip", "[protocol]") {
    CHECK(ToString(ymir::debug::CommandMethod::RegsRead) == "regs.read");
    CHECK(ToString(ymir::debug::CommandMethod::MemPeek) == "mem.peek");
    CHECK(ToString(ymir::debug::CommandMethod::DisasmAt) == "disasm.at");
    CHECK(ToString(ymir::debug::CommandMethod::ExecContinue) == "exec.continue");
    CHECK(ToString(ymir::debug::CommandMethod::ExecPause) == "exec.pause");
    CHECK(ToString(ymir::debug::CommandMethod::ExecStepI) == "exec.stepi");
    CHECK(ToString(ymir::debug::CommandMethod::BreakpointSet) == "breakpoint.set");
}

TEST_CASE("RegsReadParams initializes", "[protocol]") {
    ymir::debug::RegsReadParams p;
    CHECK(p.target == ymir::debug::DebugTarget::Sh2Master);
}

TEST_CASE("MemPeekParams initializes", "[protocol]") {
    ymir::debug::MemPeekParams p;
    CHECK(p.count == 0);
    CHECK(p.address == 0);
}

TEST_CASE("BreakpointSetParams supports optional label", "[protocol]") {
    ymir::debug::BreakpointSetParams p;
    CHECK_FALSE(p.label.has_value());
    p.label = "boot";
    CHECK(p.label.value() == "boot");
}

TEST_CASE("ExecStepIResult shape", "[protocol]") {
    ymir::debug::ExecStepIResult r;
    r.reason = ymir::debug::StopReason::Step;
    r.target = ymir::debug::DebugTarget::Sh2Master;
    r.pc_before = 0x06004000;
    r.pc_after = 0x06004002;
    r.cycles_advanced = 1;
    r.counterpart_advanced = true;
    CHECK(r.reason == ymir::debug::StopReason::Step);
    CHECK(r.pc_before == 0x06004000);
    CHECK(r.counterpart_advanced);
}

TEST_CASE("DebugCommand carries typed params", "[protocol]") {
    ymir::debug::DebugCommand command;
    command.method = ymir::debug::CommandMethod::MemPeek;
    command.params = ymir::debug::MemPeekParams{ymir::debug::DebugTarget::Sh2Master, 0x06004000, 16};

    REQUIRE(std::holds_alternative<ymir::debug::MemPeekParams>(command.params));
    const auto &params = std::get<ymir::debug::MemPeekParams>(command.params);
    CHECK(params.address == 0x06004000);
    CHECK(params.count == 16);
}

TEST_CASE("DebugResult carries payload or error", "[protocol]") {
    ymir::debug::DebugResult result{
        ymir::debug::DebugResultPayload{
            ymir::debug::BreakpointSetResult{"bp-1", ymir::debug::DebugTarget::Sh2Master, 0x06004000},
        },
    };

    REQUIRE(std::holds_alternative<ymir::debug::DebugResultPayload>(result));
    const auto &payload = std::get<ymir::debug::DebugResultPayload>(result);
    REQUIRE(std::holds_alternative<ymir::debug::BreakpointSetResult>(payload));

    result = ymir::debug::ErrorInfo{ymir::debug::ErrorCode::InvalidState, "not paused"};
    REQUIRE(std::holds_alternative<ymir::debug::ErrorInfo>(result));
    CHECK(std::get<ymir::debug::ErrorInfo>(result).code == ymir::debug::ErrorCode::InvalidState);
}

TEST_CASE("DebugStoppedEvent initializes", "[protocol]") {
    ymir::debug::DebugStoppedEvent e;
    e.instance_id = "local-001";
    e.reason = ymir::debug::StopReason::Breakpoint;
    e.target = ymir::debug::DebugTarget::Sh2Master;
    e.pc = 0x06004000;
    e.sequence = 1842;
    CHECK(e.instance_id == "local-001");
    CHECK_FALSE(e.breakpoint_id.has_value());
}

TEST_CASE("DebugEvent carries typed event payload", "[protocol]") {
    ymir::debug::DebugEvent event{
        ymir::debug::DebugStoppedEvent{"local-001", ymir::debug::StopReason::Breakpoint,
                                       ymir::debug::DebugTarget::Sh2Master, 0x06004000, 1842, std::nullopt},
    };

    REQUIRE(std::holds_alternative<ymir::debug::DebugStoppedEvent>(event.payload));
    const auto &stopped = std::get<ymir::debug::DebugStoppedEvent>(event.payload);
    CHECK(stopped.pc == 0x06004000);
}

TEST_CASE("ErrorInfo construction", "[protocol]") {
    ymir::debug::ErrorInfo err{ymir::debug::ErrorCode::InvalidState, "not paused"};
    CHECK(err.code == ymir::debug::ErrorCode::InvalidState);
    CHECK(err.message == "not paused");
}

TEST_CASE("LineFramer splits lines", "[protocol]") {
    std::vector<std::string> lines;
    ymir::debug::LineFramer framer([&](std::string_view line) { lines.emplace_back(line); },
                                   [](std::string_view err) { FAIL("Unexpected error: " << err); });

    SECTION("Single line") {
        std::string data = "hello\n";
        framer.Push(data.data(), data.length());
        REQUIRE(lines.size() == 1);
        CHECK(lines[0] == "hello");
    }

    SECTION("Multiple lines") {
        std::string data = "line1\nline2\n";
        framer.Push(data.data(), data.length());
        REQUIRE(lines.size() == 2);
        CHECK(lines[0] == "line1");
        CHECK(lines[1] == "line2");
    }

    SECTION("Partial lines") {
        std::string data1 = "part";
        std::string data2 = "ial\n";
        framer.Push(data1.data(), data1.length());
        CHECK(lines.empty());
        framer.Push(data2.data(), data2.length());
        REQUIRE(lines.size() == 1);
        CHECK(lines[0] == "partial");
    }
}

TEST_CASE("LineFramer enforces max length", "[protocol]") {
    std::string error;
    std::vector<std::string> lines;
    ymir::debug::LineFramer framer([&](std::string_view line) { lines.emplace_back(line); },
                                   [&](std::string_view err) { error = err; });

    std::string maxLine(ymir::debug::LineFramer::kMaxLineLength, 'a');
    maxLine += '\n';
    framer.Push(maxLine.data(), maxLine.length());
    REQUIRE(lines.size() == 1);
    CHECK(lines[0].size() == ymir::debug::LineFramer::kMaxLineLength);

    lines.clear();
    std::string longLine(ymir::debug::LineFramer::kMaxLineLength + 1, 'a');
    longLine += "\nvalid\n";

    framer.Push(longLine.data(), longLine.length());
    CHECK(error == "Line length limit exceeded");
    REQUIRE(lines.size() == 1);
    CHECK(lines[0] == "valid");
}

TEST_CASE("JsonRpcAdapter parses requests", "[protocol]") {
    nlohmann::json error;

    SECTION("Valid request") {
        std::string line = R"({"jsonrpc": "2.0", "method": "debug.version", "id": 1})";
        auto req = ymir::debug::JsonRpcAdapter::ParseRequest(line, error);
        REQUIRE(req.has_value());
        CHECK(req->method == "debug.version");
        CHECK(std::get<int>(req->id) == 1);
        CHECK_FALSE(req->is_notification);
    }

    SECTION("Valid notification") {
        std::string line = R"({"jsonrpc": "2.0", "method": "instance.ready"})";
        auto req = ymir::debug::JsonRpcAdapter::ParseRequest(line, error);
        REQUIRE(req.has_value());
        CHECK(req->method == "instance.ready");
        CHECK(req->is_notification);
    }

    SECTION("Batch request reject") {
        std::string line = R"([{"jsonrpc": "2.0", "method": "test"}])";
        auto req = ymir::debug::JsonRpcAdapter::ParseRequest(line, error);
        CHECK_FALSE(req.has_value());
        CHECK(error["error"]["message"] == "Batch requests are not supported");
    }

    SECTION("Malformed JSON") {
        std::string line = R"({"jsonrpc": "2.0", "method": )";
        auto req = ymir::debug::JsonRpcAdapter::ParseRequest(line, error);
        CHECK_FALSE(req.has_value());
        CHECK(error["error"]["code"] == static_cast<int>(ymir::debug::JsonRpcError::ParseError));
    }
}

TEST_CASE("JsonRpcAdapter creates method-not-found errors", "[protocol]") {
    const auto error =
        ymir::debug::JsonRpcAdapter::CreateMethodNotFoundResponse(ymir::debug::JsonRpcId{42}, "unknown.method");

    CHECK(error["id"] == 42);
    CHECK(error["error"]["code"] == static_cast<int>(ymir::debug::JsonRpcError::MethodNotFound));
    CHECK(error["error"]["data"]["method"] == "unknown.method");
}

TEST_CASE("JsonRpcAdapter ID preservation", "[protocol]") {
    nlohmann::json error;

    SECTION("Integer ID") {
        std::string line = R"({"jsonrpc": "2.0", "method": "debug.version", "id": 42})";
        auto req = ymir::debug::JsonRpcAdapter::ParseRequest(line, error);
        REQUIRE(req.has_value());
        CHECK(std::get<int>(req->id) == 42);
    }

    SECTION("String ID") {
        std::string line = R"({"jsonrpc": "2.0", "method": "debug.version", "id": "req-001"})";
        auto req = ymir::debug::JsonRpcAdapter::ParseRequest(line, error);
        REQUIRE(req.has_value());
        CHECK(std::get<std::string>(req->id) == "req-001");
    }
}
