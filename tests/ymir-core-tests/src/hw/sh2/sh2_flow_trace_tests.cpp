#include <catch2/catch_test_macros.hpp>

#include <app/debug/sh2_tracer.hpp>

#include <ymir/hw/sh2/sh2.hpp>
#include <ymir/hw/sh2/sh2_disasm.hpp>

#include <array>
#include <map>

using namespace ymir;

namespace sh2_flow_trace {

inline constexpr uint16 instrNOP = 0x0009;
inline constexpr uint16 instrRTS = 0x000B;
inline constexpr uint16 instrBRA0 = 0xA000;
inline constexpr uint16 instrBRA1 = 0xA001;
inline constexpr uint16 instrBT1 = 0x8901;
inline constexpr uint16 instrBF1 = 0x8B01;
inline constexpr uint16 instrBTS0 = 0x8D00;
inline constexpr uint16 instrADD1R15 = 0x7F01;
inline constexpr uint16 instrADD4R15 = 0x7F04;

struct TestSubject {
    sys::SystemFeatures systemFeatures{};
    core::Scheduler scheduler{};
    sys::SH2Bus bus{};
    sh2::SH2 sh2{scheduler, bus, true, systemFeatures};
    sh2::SH2::Probe &probe{sh2.GetProbe()};
    app::SH2Tracer tracer{};

    TestSubject() {
        sh2.UseTracer(&tracer);
        bus.MapBoth(
            0x000'0000, 0x7FF'FFFF, this,
            [](uint32 address, void *ctx) -> uint8 { return static_cast<TestSubject *>(ctx)->Read8(address); },
            [](uint32 address, void *ctx) -> uint16 { return static_cast<TestSubject *>(ctx)->Read16(address); },
            [](uint32 address, void *ctx) -> uint32 { return static_cast<TestSubject *>(ctx)->Read32(address); },
            [](uint32 address, uint8 value, void *ctx) { static_cast<TestSubject *>(ctx)->Write8(address, value); },
            [](uint32 address, uint16 value, void *ctx) { static_cast<TestSubject *>(ctx)->Write16(address, value); },
            [](uint32 address, uint32 value, void *ctx) { static_cast<TestSubject *>(ctx)->Write32(address, value); });
        Reset();
        tracer.SetTraceFlowStack(true);
    }

    void Reset() {
        scheduler.Reset();
        sh2.Reset(true);
        mockedReads8.clear();
        mockedReads16.clear();
        mockedReads32.clear();
        tracer.instructions.Clear();
        tracer.traceEvents.Clear();
        tracer.interrupts.Clear();
        tracer.exceptions.Clear();
        tracer.divisions.Clear();
        tracer.dmaTransfers[0].Clear();
        tracer.dmaTransfers[1].Clear();
    }

    void Step() {
        scheduler.Advance(sh2.Step<true, false>());
    }

    void MockInstruction(uint32 address, uint16 opcode) {
        mockedReads16[address] = opcode;
    }

    uint32 DispPCTarget(uint32 pc, uint16 opcode) const {
        const auto &instr = sh2::Disassemble(opcode);
        REQUIRE(instr.op1.type == sh2::Operand::Type::DispPC);
        return static_cast<uint32>(static_cast<sint64>(pc) + static_cast<sint64>(instr.op1.immDisp));
    }

    uint8 Read8(uint32 address) {
        const auto it = mockedReads8.find(address);
        return it != mockedReads8.end() ? it->second : 0;
    }

    uint16 Read16(uint32 address) {
        const auto it = mockedReads16.find(address);
        return it != mockedReads16.end() ? it->second : 0;
    }

    uint32 Read32(uint32 address) {
        const auto it = mockedReads32.find(address);
        return it != mockedReads32.end() ? it->second : 0;
    }

    void Write8(uint32 address, uint8 value) {
        mockedReads8[address] = value;
    }

    void Write16(uint32 address, uint16 value) {
        mockedReads16[address] = value;
    }

    void Write32(uint32 address, uint32 value) {
        mockedReads32[address] = value;
    }

    std::map<uint32, uint8> mockedReads8;
    std::map<uint32, uint16> mockedReads16;
    std::map<uint32, uint32> mockedReads32;
};

TEST_CASE("SH2 flow tracer marks delay-slot providers", "[sh2][trace][flow][delay-slot]") {
    TestSubject test{};
    constexpr uint32 startPC = 0x0600'1000;

    test.MockInstruction(startPC, instrBRA1);
    test.MockInstruction(startPC + 2, instrNOP);
    test.probe.PC() = startPC;

    const uint32 expectedTarget = test.DispPCTarget(startPC, instrBRA1);

    test.Step();

    REQUIRE(test.tracer.traceEvents.Count() == 1);
    const auto event = test.tracer.traceEvents.ReadReverse(0);
    CHECK(event.type == app::SH2Tracer::TraceEventType::Branch);
    CHECK(event.pc == startPC);
    CHECK(event.targetValid);
    CHECK(event.target == expectedTarget);
    CHECK(event.hasDelaySlot);
    CHECK_FALSE(event.delaySlot);
    CHECK(test.probe.PC() == startPC + 2);
}

TEST_CASE("SH2 flow tracer emits conditional branch events when not taken", "[sh2][trace][flow][conditional]") {
    TestSubject test{};
    constexpr uint32 startPC = 0x0600'2000;

    test.MockInstruction(startPC, instrBT1);
    test.probe.PC() = startPC;
    test.probe.SR().T = 0;

    const uint32 expectedTarget = test.DispPCTarget(startPC, instrBT1);

    test.Step();

    REQUIRE(test.tracer.traceEvents.Count() == 1);
    const auto event = test.tracer.traceEvents.ReadReverse(0);
    CHECK(event.type == app::SH2Tracer::TraceEventType::Branch);
    CHECK(event.isConditionalBranch);
    CHECK_FALSE(event.branchTaken);
    CHECK(event.targetValid);
    CHECK(event.target == expectedTarget);
    CHECK(test.probe.PC() == startPC + 2);
    CHECK(event.target != test.probe.PC());
}

TEST_CASE("SH2 flow tracer emits conditional branch events when taken", "[sh2][trace][flow][conditional]") {
    TestSubject test{};
    constexpr uint32 startPC = 0x0600'3000;

    test.MockInstruction(startPC, instrBT1);
    test.probe.PC() = startPC;
    test.probe.SR().T = 1;

    const uint32 expectedTarget = test.DispPCTarget(startPC, instrBT1);

    test.Step();

    REQUIRE(test.tracer.traceEvents.Count() == 1);
    const auto event = test.tracer.traceEvents.ReadReverse(0);
    CHECK(event.type == app::SH2Tracer::TraceEventType::Branch);
    CHECK(event.isConditionalBranch);
    CHECK(event.branchTaken);
    CHECK(event.targetValid);
    CHECK(event.target == expectedTarget);
    CHECK(test.probe.PC() == expectedTarget);
}

TEST_CASE("SH2 flow tracer sequence IDs correlate instruction and event streams", "[sh2][trace][flow][sequence]") {
    TestSubject test{};
    constexpr uint32 startPC = 0x0600'4000;
    constexpr uint32 startSP = 0x0600'2000;

    test.MockInstruction(startPC, instrBRA0);
    test.MockInstruction(startPC + 2, instrADD4R15);
    test.probe.PC() = startPC;
    test.probe.R(15) = startSP;

    test.Step();
    test.Step();

    REQUIRE(test.tracer.instructions.Count() == 2);
    REQUIRE(test.tracer.traceEvents.Count() == 2);

    const auto instr0 = test.tracer.instructions.Read(0);
    const auto instr1 = test.tracer.instructions.Read(1);
    const auto event0 = test.tracer.traceEvents.Read(0);
    const auto event1 = test.tracer.traceEvents.Read(1);

    CHECK(instr1.sequenceId == instr0.sequenceId + 1);
    CHECK(event1.counter == event0.counter + 1);
    CHECK(event0.sequenceId == instr0.sequenceId);
    CHECK(event1.sequenceId == instr1.sequenceId);
    CHECK(event0.pc == instr0.pc);
    CHECK(event1.pc == instr1.pc);
    CHECK(event0.opcode == instr0.opcode);
    CHECK(event1.opcode == instr1.opcode);
}

TEST_CASE("SH2 flow tracer marks delay-slot instruction execution in event metadata",
          "[sh2][trace][flow][delay-slot]") {
    TestSubject test{};
    constexpr uint32 startPC = 0x0600'5000;
    constexpr uint32 startSP = 0x0600'2400;

    test.MockInstruction(startPC, instrBRA0);
    test.MockInstruction(startPC + 2, instrADD4R15);
    test.probe.PC() = startPC;
    test.probe.R(15) = startSP;

    test.Step();
    test.Step();

    REQUIRE(test.tracer.traceEvents.Count() == 2);

    const auto providerEvent = test.tracer.traceEvents.Read(0);
    const auto slotEvent = test.tracer.traceEvents.Read(1);

    CHECK(providerEvent.type == app::SH2Tracer::TraceEventType::Branch);
    CHECK_FALSE(providerEvent.delaySlot);
    CHECK(providerEvent.hasDelaySlot);

    CHECK(slotEvent.type == app::SH2Tracer::TraceEventType::StackPop);
    CHECK(slotEvent.delaySlot);
    CHECK_FALSE(slotEvent.hasDelaySlot);
    CHECK(slotEvent.spBefore == startSP);
    CHECK(slotEvent.spAfter == startSP + 4);
    CHECK(test.probe.R(15) == startSP + 4);
}

TEST_CASE("SH2 flow tracer suppresses invalid delay-slot barriers", "[sh2][trace][flow][delay-slot][optional]") {
    TestSubject test{};
    constexpr uint32 startPC = 0x0600'6000;
    debug::ISH2Tracer &tracerInterface = test.tracer;

    test.probe.PR() = 0x0600'1234;
    test.tracer.traceEvents.Clear();

    tracerInterface.ExecuteInstruction(startPC, instrRTS, false);
    REQUIRE(test.tracer.traceEvents.Count() == 1);

    tracerInterface.ExecuteInstruction(startPC + 2, instrRTS, true);
    CHECK(test.tracer.traceEvents.Count() == 1);
    CHECK(test.tracer.traceEvents.ReadReverse(0).pc == startPC);
}

TEST_CASE("SH2 flow tracer hasDelaySlot metadata matches disassembly", "[sh2][trace][flow][metadata][optional]") {
    TestSubject test{};
    constexpr std::array<uint16, 5> opcodes = {instrBRA0, instrBTS0, instrBF1, instrRTS, instrADD1R15};
    debug::ISH2Tracer &tracerInterface = test.tracer;
    uint32 pc = 0x0600'7000;

    test.tracer.traceEvents.Clear();

    for (const uint16 opcode : opcodes) {
        const size_t eventsBefore = test.tracer.traceEvents.Count();
        const auto &disasm = sh2::Disassemble(opcode);

        tracerInterface.ExecuteInstruction(pc, opcode, false);

        REQUIRE(test.tracer.traceEvents.Count() == eventsBefore + 1);
        const auto event = test.tracer.traceEvents.ReadReverse(0);
        CHECK(event.hasDelaySlot == disasm.hasDelaySlot);
        pc += 2;
    }
}

TEST_CASE("SH2 flow tracer keeps target candidate for not-taken branches",
          "[sh2][trace][flow][conditional][optional]") {
    TestSubject test{};
    constexpr uint32 startPC = 0x0600'8000;

    test.MockInstruction(startPC, instrBF1);
    test.probe.PC() = startPC;
    test.probe.SR().T = 1;

    const uint32 expectedTarget = test.DispPCTarget(startPC, instrBF1);

    test.Step();

    REQUIRE(test.tracer.traceEvents.Count() == 1);
    const auto event = test.tracer.traceEvents.ReadReverse(0);
    CHECK(event.type == app::SH2Tracer::TraceEventType::Branch);
    CHECK(event.isConditionalBranch);
    CHECK_FALSE(event.branchTaken);
    CHECK(event.targetValid);
    CHECK(event.target == expectedTarget);
    CHECK(test.probe.PC() == startPC + 2);
    CHECK(event.target != test.probe.PC());
}

TEST_CASE("SH2 flow tracer sequence IDs are independent per tracer", "[sh2][trace][flow][sequence][optional]") {
    app::SH2Tracer tracerA{};
    app::SH2Tracer tracerB{};
    debug::ISH2Tracer &tracerInterfaceA = tracerA;
    debug::ISH2Tracer &tracerInterfaceB = tracerB;

    tracerA.traceInstructions = true;
    tracerB.traceInstructions = true;

    tracerInterfaceA.ExecuteInstruction(0x0600'9000, instrNOP, false);
    tracerInterfaceB.ExecuteInstruction(0x0600'A000, instrNOP, false);

    REQUIRE(tracerA.instructions.Count() == 1);
    REQUIRE(tracerB.instructions.Count() == 1);
    CHECK(tracerA.instructions.Read(0).sequenceId == 0);
    CHECK(tracerB.instructions.Read(0).sequenceId == 0);

    tracerInterfaceA.ExecuteInstruction(0x0600'9002, instrNOP, false);

    REQUIRE(tracerA.instructions.Count() == 2);
    CHECK(tracerA.instructions.Read(1).sequenceId == 1);
    CHECK(tracerB.instructions.Read(0).sequenceId == 0);
}

} // namespace sh2_flow_trace
