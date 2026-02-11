#include <catch2/catch_test_macros.hpp>

#include <ymir/hw/sh2/sh2.hpp>

#include <map>

namespace sh2_block_cache {

using namespace ymir;

inline constexpr uint16 instrNOP = 0x0009;
inline constexpr uint16 instrBRAminus1 = 0xAFFF;
inline constexpr uint16 instrBRAminus4 = 0xAFFC;
inline constexpr uint16 instrMOVI_R0_1 = 0xE001;
inline constexpr uint16 instrMOVI_R0_2 = 0xE002;
inline constexpr uint16 instrMOVI_R0_3 = 0xE003;
inline constexpr uint16 instrMOVI_R0_4 = 0xE004;
inline constexpr uint16 instrMOVI_R0_7F = 0xE07F;
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

TEST_CASE_PERSISTENT_FIXTURE(TestSubject, "SH2 cached interpreter reuses predecoded opcode entries",
                             "[sh2][block-cache][fast-path][predecode]") {
    ResetState();
    SetInstructionSpan(kProgramStart, instrNOP, kProgramLength);
    SetInstruction(kProgramStart + 0u, instrMOVI_R0_1);
    SetInstruction(kProgramStart + 2u, instrMOVI_R0_2);
    SetInstruction(kProgramStart + 4u, instrMOVI_R0_3);
    SetInstruction(kProgramStart + 6u, instrMOVI_R0_4);
    probe.PC() = kProgramStart;

    const uint32 peekReadsBeforeBuild = counters.peekRead16;
    RunStep<false, false, true>();
    CHECK(probe.PC() == kProgramStart + 2u);
    CHECK(probe.R(0) == 1u);
    CHECK(counters.peekRead16 > peekReadsBeforeBuild);
    const uint32 peekReadsAfterBuild = counters.peekRead16;

    RunStep<false, false, true>();
    CHECK(probe.PC() == kProgramStart + 4u);
    CHECK(probe.R(0) == 2u);
    CHECK(counters.peekRead16 == peekReadsAfterBuild);

    RunStep<false, false, true>();
    CHECK(probe.PC() == kProgramStart + 6u);
    CHECK(probe.R(0) == 3u);
    CHECK(counters.peekRead16 == peekReadsAfterBuild);

    RunStep<false, false, true>();
    CHECK(probe.PC() == kProgramStart + 8u);
    CHECK(probe.R(0) == 4u);
    CHECK(counters.peekRead16 == peekReadsAfterBuild);
}

TEST_CASE_PERSISTENT_FIXTURE(TestSubject, "SH2 cached interpreter reuses same-page entries in page bucket chains",
                             "[sh2][block-cache][lookup]") {
    ResetState();

    constexpr uint32 entryA = kProgramStart;
    constexpr uint32 entryB = kProgramStart + 0x0200;
    constexpr uint32 instructionCount = ((entryB - kProgramStart) / 2u) + 64u;
    SetInstructionSpan(kProgramStart, instrNOP, instructionCount);

    probe.PC() = entryA;
    RunStep<false, false, true>();
    CHECK(probe.PC() == entryA + 2u);
    const uint32 peekReadsAfterEntryA = counters.peekRead16;

    probe.PC() = entryB;
    RunStep<false, false, true>();
    CHECK(probe.PC() == entryB + 2u);
    CHECK(counters.peekRead16 > peekReadsAfterEntryA);
    const uint32 peekReadsAfterEntryB = counters.peekRead16;

    probe.PC() = entryA;
    RunStep<false, false, true>();
    CHECK(counters.peekRead16 == peekReadsAfterEntryB);

    probe.PC() = entryB;
    RunStep<false, false, true>();
    CHECK(counters.peekRead16 == peekReadsAfterEntryB);
}

TEST_CASE_PERSISTENT_FIXTURE(TestSubject,
                             "SH2 cached interpreter keeps delay-slot and non-delay entries separate per PC",
                             "[sh2][block-cache][lookup][delay-slot]") {
    ResetState();

    constexpr uint32 branchPC = kProgramStart;
    constexpr uint32 sharedPC = branchPC + 2u;
    constexpr uint32 loopBranchPC = branchPC + 4u;
    constexpr uint32 loopDelayPC = branchPC + 6u;

    SetInstruction(branchPC, instrBRAminus1);
    SetInstruction(sharedPC, instrNOP);
    SetInstruction(loopBranchPC, instrBRAminus4);
    SetInstruction(loopDelayPC, instrNOP);

    probe.PC() = branchPC;

    RunStep<false, false, true>(); // Build (PC=branchPC, delay=false)
    CHECK(probe.PC() == sharedPC);
    CHECK(probe.IsInDelaySlot() == true);
    const uint32 peekReadsAfterBranch = counters.peekRead16;

    RunStep<false, false, true>(); // Build (PC=sharedPC, delay=true)
    CHECK(probe.PC() == sharedPC);
    CHECK(probe.IsInDelaySlot() == false);
    CHECK(counters.peekRead16 > peekReadsAfterBranch);
    const uint32 peekReadsAfterDelaySlotBuild = counters.peekRead16;

    RunStep<false, false, true>(); // Build (PC=sharedPC, delay=false)
    CHECK(probe.PC() == loopBranchPC);
    CHECK(probe.IsInDelaySlot() == false);
    CHECK(counters.peekRead16 > peekReadsAfterDelaySlotBuild);

    // Complete one loop iteration to reach the same two states again.
    RunStep<false, false, true>();
    CHECK(probe.PC() == loopDelayPC);
    CHECK(probe.IsInDelaySlot() == true);
    RunStep<false, false, true>();
    CHECK(probe.PC() == branchPC);
    CHECK(probe.IsInDelaySlot() == false);
    RunStep<false, false, true>();
    CHECK(probe.PC() == sharedPC);
    CHECK(probe.IsInDelaySlot() == true);

    const uint32 peekReadsBeforeDelaySlotReuse = counters.peekRead16;
    RunStep<false, false, true>(); // Reuse (PC=sharedPC, delay=true)
    CHECK(probe.PC() == sharedPC);
    CHECK(probe.IsInDelaySlot() == false);
    CHECK(counters.peekRead16 == peekReadsBeforeDelaySlotReuse);

    const uint32 peekReadsBeforeNonDelayReuse = counters.peekRead16;
    RunStep<false, false, true>(); // Reuse (PC=sharedPC, delay=false)
    CHECK(probe.PC() == loopBranchPC);
    CHECK(probe.IsInDelaySlot() == false);
    CHECK(counters.peekRead16 == peekReadsBeforeNonDelayReuse);
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

TEST_CASE_PERSISTENT_FIXTURE(TestSubject, "SH2 cache+block cache falls back to fetched opcode on mismatch",
                             "[sh2][block-cache][sh2-cache-combo][predecode]") {
    ResetState();
    SetInstructionSpan(kProgramStart, instrNOP, kProgramLength);
    probe.PC() = 0xA000'1000;

    RunStep<false, true, true>();
    CHECK(probe.PC() == 0xA000'1002u);
    const uint32 peekReadsAfterInitialBuild = counters.peekRead16;
    const uint32 normalReadsAfterInitialBuild = counters.normalRead16;

    // Modify backing instruction memory without explicit invalidation.
    SetInstruction(kProgramStart + 2u, instrMOVI_R0_7F);

    RunStep<false, true, true>();
    CHECK(probe.PC() == 0xA000'1004u);
    CHECK(probe.R(0) == 0x7Fu);
    CHECK(counters.peekRead16 == peekReadsAfterInitialBuild);
    CHECK(counters.normalRead16 > normalReadsAfterInitialBuild);

    const uint32 peekReadsBeforeRebuild = counters.peekRead16;
    RunStep<false, true, true>();
    CHECK(counters.peekRead16 > peekReadsBeforeRebuild);
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

TEST_CASE_PERSISTENT_FIXTURE(TestSubject, "SH2 page-LUT lookup keeps invalidation generation behavior",
                             "[sh2][block-cache][lookup][invalidation]") {
    ResetState();

    constexpr uint32 entryA = kProgramStart;
    constexpr uint32 entryB = kProgramStart + 0x80;
    constexpr uint32 instructionCount = ((entryB - kProgramStart) / 2u) + 64u;
    SetInstructionSpan(kProgramStart, instrNOP, instructionCount);

    probe.PC() = entryA;
    RunStep<false, false, true>();
    probe.PC() = entryB;
    RunStep<false, false, true>();
    const uint32 peekReadsAfterInitialBuild = counters.peekRead16;

    sh2.InvalidateBlockCacheRange(kProgramStart + 0x1000, 2);
    probe.PC() = entryA;
    RunStep<false, false, true>();
    probe.PC() = entryB;
    RunStep<false, false, true>();
    CHECK(counters.peekRead16 == peekReadsAfterInitialBuild);

    const uint32 peekReadsBeforeSamePageInvalidation = counters.peekRead16;
    sh2.InvalidateBlockCacheRange(kProgramStart + 0x20, 2);
    probe.PC() = entryA;
    RunStep<false, false, true>();
    CHECK(counters.peekRead16 > peekReadsBeforeSamePageInvalidation);
}

TEST_CASE_PERSISTENT_FIXTURE(TestSubject, "SH2 block cache pool recycle keeps rebuild and reuse behavior",
                             "[sh2][block-cache][lookup][recycle]") {
    ResetState();

    constexpr uint32 entryA = kProgramStart;
    constexpr uint32 entryB = kProgramStart + 0x40;
    constexpr uint32 instructionCount = ((entryB - kProgramStart) / 2u) + 64u;
    SetInstructionSpan(kProgramStart, instrNOP, instructionCount);

    probe.PC() = entryA;
    RunStep<false, false, true>();
    probe.PC() = entryB;
    RunStep<false, false, true>();
    const uint32 peekReadsAfterInitialBuild = counters.peekRead16;

    probe.PC() = entryA;
    RunStep<false, false, true>();
    probe.PC() = entryB;
    RunStep<false, false, true>();
    CHECK(counters.peekRead16 == peekReadsAfterInitialBuild);

    sh2.PurgeBlockCache();

    probe.PC() = entryA;
    RunStep<false, false, true>();
    probe.PC() = entryB;
    RunStep<false, false, true>();
    CHECK(counters.peekRead16 > peekReadsAfterInitialBuild);
    const uint32 peekReadsAfterRebuild = counters.peekRead16;

    probe.PC() = entryA;
    RunStep<false, false, true>();
    probe.PC() = entryB;
    RunStep<false, false, true>();
    CHECK(counters.peekRead16 == peekReadsAfterRebuild);
}

TEST_CASE_PERSISTENT_FIXTURE(TestSubject, "SH2 repeated clear and rebuild stays deterministic",
                             "[sh2][block-cache][lookup][recycle]") {
    ResetState();
    SetInstructionSpan(kProgramStart, instrNOP, kProgramLength);

    for (size_t i = 0; i < 4; ++i) {
        probe.PC() = kProgramStart;

        const uint32 peekReadsBeforeBuild = counters.peekRead16;
        RunStep<false, false, true>();
        CHECK(probe.PC() == kProgramStart + 2);
        CHECK(counters.peekRead16 > peekReadsBeforeBuild);

        const uint32 peekReadsAfterBuild = counters.peekRead16;
        RunStep<false, false, true>();
        CHECK(probe.PC() == kProgramStart + 4);
        CHECK(counters.peekRead16 == peekReadsAfterBuild);

        sh2.PurgeBlockCache();
    }
}

TEST_CASE_PERSISTENT_FIXTURE(TestSubject, "SH2 cap32 reuse window extends past old 16-op boundary",
                             "[sh2][block-cache][cap32]") {
    ResetState();
    SetInstructionSpan(kProgramStart, instrNOP, 128);
    probe.PC() = kProgramStart;

    const uint32 peekReadsBeforeBuild = counters.peekRead16;
    RunStep<false, false, true>();
    CHECK(probe.PC() == kProgramStart + 2u);
    CHECK(counters.peekRead16 > peekReadsBeforeBuild);
    const uint32 peekReadsAfterBuild = counters.peekRead16;

    // With the cap at 32 ops, no rebuild should occur while stepping through the first 32-entry window.
    for (uint32 i = 1; i < 32; ++i) {
        RunStep<false, false, true>();
        CHECK(probe.PC() == kProgramStart + (i + 1u) * 2u);
        CHECK(counters.peekRead16 == peekReadsAfterBuild);
    }
}

TEST_CASE_PERSISTENT_FIXTURE(TestSubject, "SH2 cap32 block still stops at page boundary",
                             "[sh2][block-cache][cap32]") {
    ResetState();

    constexpr uint32 entry = kProgramStart + 0xFF0u;
    SetInstructionSpan(entry, instrNOP, 128);
    probe.PC() = entry;

    const uint32 peekReadsBeforeBuild = counters.peekRead16;
    RunStep<false, false, true>();
    CHECK(probe.PC() == entry + 2u);
    CHECK(counters.peekRead16 > peekReadsBeforeBuild);
    const uint32 peekReadsAfterBuild = counters.peekRead16;

    // From 0x...FF0 to 0x...FFE there are 8 instructions; crossing to next page should trigger a rebuild.
    for (uint32 i = 1; i < 8; ++i) {
        RunStep<false, false, true>();
        CHECK(counters.peekRead16 == peekReadsAfterBuild);
    }
    CHECK(probe.PC() == entry + 16u);

    RunStep<false, false, true>();
    CHECK(probe.PC() == entry + 18u);
    CHECK(counters.peekRead16 > peekReadsAfterBuild);
}

TEST_CASE_PERSISTENT_FIXTURE(TestSubject, "SH2 cap32 block still stops at barriers",
                             "[sh2][block-cache][cap32]") {
    ResetState();

    constexpr uint32 entry = kProgramStart;
    SetInstruction(entry + 0u, instrNOP);
    SetInstruction(entry + 2u, instrNOP);
    SetInstruction(entry + 4u, instrBRAminus1);
    SetInstruction(entry + 6u, instrNOP);
    SetInstruction(entry + 8u, instrNOP);
    probe.PC() = entry;

    const uint32 peekReadsBeforeBuild = counters.peekRead16;
    RunStep<false, false, true>();
    CHECK(probe.PC() == entry + 2u);
    CHECK(counters.peekRead16 > peekReadsBeforeBuild);
    const uint32 peekReadsAfterBuild = counters.peekRead16;

    RunStep<false, false, true>();
    CHECK(probe.PC() == entry + 4u);
    CHECK(counters.peekRead16 == peekReadsAfterBuild);

    RunStep<false, false, true>();
    CHECK(probe.PC() == entry + 6u);
    CHECK(probe.IsInDelaySlot() == true);
    CHECK(counters.peekRead16 == peekReadsAfterBuild);

    // Branch delay-slot entrypoint should build a fresh single-op block.
    RunStep<false, false, true>();
    CHECK(probe.PC() == entry + 6u);
    CHECK(probe.IsInDelaySlot() == false);
    CHECK(counters.peekRead16 > peekReadsAfterBuild);
}

} // namespace sh2_block_cache
