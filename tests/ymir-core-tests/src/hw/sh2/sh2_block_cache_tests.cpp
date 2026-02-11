#include <catch2/catch_test_macros.hpp>

#include <ymir/hw/sh2/sh2.hpp>

#include <map>

namespace sh2_block_cache {

using namespace ymir;

inline constexpr uint16 instrNOP = 0x0009;
inline constexpr uint32 kProgramStart = 0x0000'1000;
inline constexpr uint32 kProgramLength = 32;

struct TestSubject {
    sys::SystemFeatures systemFeatures{};
    mutable core::Scheduler scheduler{};
    mutable sys::SH2Bus bus{};
    mutable sh2::SH2 sh2{scheduler, bus, true, systemFeatures};
    sh2::SH2::Probe &probe{sh2.GetProbe()};

    struct AccessCounters {
        uint32 normalRead8 = 0;
        uint32 normalRead16 = 0;
        uint32 normalRead32 = 0;
        uint32 normalWrite8 = 0;
        uint32 normalWrite16 = 0;
        uint32 normalWrite32 = 0;

        uint32 peekRead8 = 0;
        uint32 peekRead16 = 0;
        uint32 peekRead32 = 0;
        uint32 peekWrite8 = 0;
        uint32 peekWrite16 = 0;
        uint32 peekWrite32 = 0;
    };

    TestSubject() {
        bus.MapNormal(
            0x000'0000, 0x7FF'FFFF, this,
            [](uint32 address, void *ctx) -> uint8 { return static_cast<TestSubject *>(ctx)->Read8(address); },
            [](uint32 address, void *ctx) -> uint16 { return static_cast<TestSubject *>(ctx)->Read16(address); },
            [](uint32 address, void *ctx) -> uint32 { return static_cast<TestSubject *>(ctx)->Read32(address); },
            [](uint32 address, uint8 value, void *ctx) { static_cast<TestSubject *>(ctx)->Write8(address, value); },
            [](uint32 address, uint16 value, void *ctx) { static_cast<TestSubject *>(ctx)->Write16(address, value); },
            [](uint32 address, uint32 value, void *ctx) { static_cast<TestSubject *>(ctx)->Write32(address, value); });

        bus.MapSideEffectFree(
            0x000'0000, 0x7FF'FFFF, this,
            [](uint32 address, void *ctx) -> uint8 { return static_cast<TestSubject *>(ctx)->Peek8(address); },
            [](uint32 address, void *ctx) -> uint16 { return static_cast<TestSubject *>(ctx)->Peek16(address); },
            [](uint32 address, void *ctx) -> uint32 { return static_cast<TestSubject *>(ctx)->Peek32(address); },
            [](uint32 address, uint8 value, void *ctx) { static_cast<TestSubject *>(ctx)->Poke8(address, value); },
            [](uint32 address, uint16 value, void *ctx) { static_cast<TestSubject *>(ctx)->Poke16(address, value); },
            [](uint32 address, uint32 value, void *ctx) { static_cast<TestSubject *>(ctx)->Poke32(address, value); });
    }

    void ResetState() const {
        scheduler.Reset();
        sh2.Reset(true);
        counters = {};
        memory8.clear();
        memory16.clear();
        memory32.clear();
    }

    void SetInstruction(uint32 address, uint16 opcode) const {
        memory16[address & ~1u] = opcode;
    }

    void SetInstructionSpan(uint32 startAddress, uint16 opcode, uint32 count) const {
        for (uint32 i = 0; i < count; ++i) {
            SetInstruction(startAddress + i * 2u, opcode);
        }
    }

    template <bool debug, bool enableSH2Cache, bool enableBlockCache>
    uint64 RunStep() const {
        return sh2.Step<debug, enableSH2Cache, enableBlockCache>();
    }

    uint8 Read8(uint32 address) const {
        ++counters.normalRead8;
        auto it = memory8.find(address);
        return it != memory8.end() ? it->second : 0;
    }

    uint16 Read16(uint32 address) const {
        ++counters.normalRead16;
        auto it = memory16.find(address);
        return it != memory16.end() ? it->second : 0;
    }

    uint32 Read32(uint32 address) const {
        ++counters.normalRead32;
        auto it = memory32.find(address);
        return it != memory32.end() ? it->second : 0;
    }

    void Write8(uint32 address, uint8 value) const {
        ++counters.normalWrite8;
        memory8[address] = value;
    }

    void Write16(uint32 address, uint16 value) const {
        ++counters.normalWrite16;
        memory16[address & ~1u] = value;
    }

    void Write32(uint32 address, uint32 value) const {
        ++counters.normalWrite32;
        memory32[address & ~3u] = value;
    }

    uint8 Peek8(uint32 address) const {
        ++counters.peekRead8;
        auto it = memory8.find(address);
        return it != memory8.end() ? it->second : 0;
    }

    uint16 Peek16(uint32 address) const {
        ++counters.peekRead16;
        auto it = memory16.find(address);
        return it != memory16.end() ? it->second : 0;
    }

    uint32 Peek32(uint32 address) const {
        ++counters.peekRead32;
        auto it = memory32.find(address);
        return it != memory32.end() ? it->second : 0;
    }

    void Poke8(uint32 address, uint8 value) const {
        ++counters.peekWrite8;
        memory8[address] = value;
    }

    void Poke16(uint32 address, uint16 value) const {
        ++counters.peekWrite16;
        memory16[address & ~1u] = value;
    }

    void Poke32(uint32 address, uint32 value) const {
        ++counters.peekWrite32;
        memory32[address & ~3u] = value;
    }

    mutable AccessCounters counters{};
    mutable std::map<uint32, uint8> memory8;
    mutable std::map<uint32, uint16> memory16;
    mutable std::map<uint32, uint32> memory32;
};

TEST_CASE_PERSISTENT_FIXTURE(TestSubject, "SH2 uncached interpreter uses normal fetch path",
                             "[sh2][block-cache][baseline]") {
    ResetState();
    SetInstructionSpan(kProgramStart, instrNOP, kProgramLength);
    probe.PC() = kProgramStart;

    const uint32 pcStart = probe.PC();
    const uint32 initialNormalReads = counters.normalRead16;
    const uint32 initialPeekReads = counters.peekRead16;

    RunStep<false, false, false>();
    CHECK(probe.PC() == pcStart + 2);
    CHECK(counters.normalRead16 > initialNormalReads);
    CHECK(counters.peekRead16 == initialPeekReads);

    const uint32 normalReadsAfterFirstStep = counters.normalRead16;
    const uint32 peekReadsAfterFirstStep = counters.peekRead16;

    RunStep<false, false, false>();
    CHECK(probe.PC() == pcStart + 4);
    CHECK(counters.normalRead16 > normalReadsAfterFirstStep);
    CHECK(counters.peekRead16 == peekReadsAfterFirstStep);
}

TEST_CASE_PERSISTENT_FIXTURE(TestSubject, "SH2 cached interpreter reuses built block without normal fetches",
                             "[sh2][block-cache][fast-path]") {
    ResetState();
    SetInstructionSpan(kProgramStart, instrNOP, kProgramLength);
    probe.PC() = kProgramStart;

    const uint32 pcStart = probe.PC();

    const uint32 initialNormalReads = counters.normalRead16;
    const uint32 initialPeekReads = counters.peekRead16;
    RunStep<false, false, true>();
    CHECK(probe.PC() == pcStart + 2);
    CHECK(counters.peekRead16 > initialPeekReads);
    CHECK(counters.normalRead16 == initialNormalReads);

    const uint32 normalReadsAfterFirstStep = counters.normalRead16;
    const uint32 peekReadsAfterFirstStep = counters.peekRead16;
    RunStep<false, false, true>();
    CHECK(probe.PC() == pcStart + 4);
    CHECK(counters.peekRead16 == peekReadsAfterFirstStep);
    CHECK(counters.normalRead16 == normalReadsAfterFirstStep);
}

TEST_CASE_PERSISTENT_FIXTURE(TestSubject, "SH2 cache emulation keeps fetch side effects with cached interpreter",
                             "[sh2][block-cache][sh2-cache-combo]") {
    ResetState();
    SetInstructionSpan(kProgramStart, instrNOP, kProgramLength);

    // 0xAxxxxxxx selects cache-through addressing so each instruction fetch still hits the normal bus path.
    probe.PC() = 0xA000'1000;

    const uint32 pcStart = probe.PC();
    const uint32 initialNormalReads = counters.normalRead16;
    const uint32 initialPeekReads = counters.peekRead16;

    RunStep<false, true, true>();
    CHECK(probe.PC() == pcStart + 2);
    CHECK(counters.peekRead16 > initialPeekReads);
    CHECK(counters.normalRead16 > initialNormalReads);

    const uint32 normalReadsAfterFirstStep = counters.normalRead16;
    const uint32 peekReadsAfterFirstStep = counters.peekRead16;

    RunStep<false, true, true>();
    CHECK(probe.PC() == pcStart + 4);
    CHECK(counters.peekRead16 == peekReadsAfterFirstStep);
    CHECK(counters.normalRead16 > normalReadsAfterFirstStep);
}

TEST_CASE_PERSISTENT_FIXTURE(TestSubject, "SH2 block cache cursor survives unrelated page invalidation",
                             "[sh2][block-cache][invalidation]") {
    ResetState();
    SetInstructionSpan(kProgramStart, instrNOP, kProgramLength);
    probe.PC() = kProgramStart;

    RunStep<false, false, true>();
    const uint32 peekReadsAfterFirstStep = counters.peekRead16;
    const uint32 normalReadsAfterFirstStep = counters.normalRead16;

    sh2.InvalidateBlockCacheRange(kProgramStart + 0x1000, 2);
    RunStep<false, false, true>();

    CHECK(counters.peekRead16 == peekReadsAfterFirstStep);
    CHECK(counters.normalRead16 == normalReadsAfterFirstStep);
}

TEST_CASE_PERSISTENT_FIXTURE(TestSubject, "SH2 block cache rebuilds after same-page invalidation",
                             "[sh2][block-cache][invalidation]") {
    ResetState();
    SetInstructionSpan(kProgramStart, instrNOP, kProgramLength);
    probe.PC() = kProgramStart;

    RunStep<false, false, true>();
    const uint32 peekReadsAfterFirstStep = counters.peekRead16;

    sh2.InvalidateBlockCacheRange(kProgramStart + 0x20, 2);
    RunStep<false, false, true>();

    CHECK(counters.peekRead16 > peekReadsAfterFirstStep);
}

} // namespace sh2_block_cache
