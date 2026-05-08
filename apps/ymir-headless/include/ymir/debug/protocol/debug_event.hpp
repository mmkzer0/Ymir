#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "debug_types.hpp"

namespace ymir::debug {

struct DebugStoppedEvent {
    std::string instance_id;
    StopReason reason;
    DebugTarget target;
    uint32_t pc{};
    uint32_t sequence{};
    std::optional<std::string> breakpoint_id;
};

struct InstanceReadyEvent {
    std::string_view protocol;
    std::string_view protocol_version;
    std::string_view transport;
    std::string instance_id;
    ExecutionState state;
    std::vector<std::string> capabilities;
};

using DebugEventPayload = std::variant<DebugStoppedEvent, InstanceReadyEvent>;

struct DebugEvent {
    DebugEventPayload payload;
};

} // namespace ymir::debug
