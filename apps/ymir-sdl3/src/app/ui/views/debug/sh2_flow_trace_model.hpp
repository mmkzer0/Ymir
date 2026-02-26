#pragma once

#include <ymir/core/types.hpp>

namespace app::ui {

/// Model/shared state for SH-2 flow trace UI components (model-view pattern).
struct SH2FlowTraceModel {
    enum class Core : uint8 {
        Master,
        Slave,
    };

    enum class DumpState : uint8 {
        Idle,
        Capturing,
    };

    enum class CompressionMode : uint8 {
        Off,
        RuntimeFold,
    };

    Core selectedCore = Core::Master;
    DumpState dumpState = DumpState::Idle;
    CompressionMode compressionMode = CompressionMode::Off;

    uint32 rowLimit = 256;

    // Placeholder counters for future async dump pipeline integration.
    uint64 droppedEvents = 0;
    uint64 flushedChunks = 0;
};

} // namespace app::ui
