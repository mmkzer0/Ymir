#pragma once

#include <string_view>

namespace ymir::debug {

inline constexpr std::string_view kProtocolName = "ymir-debug";
inline constexpr std::string_view kProtocolVersion = "0.1.0";
inline constexpr std::string_view kTransport = "stdio-jsonrpc-lines";

} // namespace ymir::debug
