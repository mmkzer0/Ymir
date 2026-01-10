#include "sh2_tracer.hpp"

#include <ymir/hw/sh2/sh2.hpp>
#include <ymir/core/types.hpp>

using namespace ymir;

namespace app {

namespace {

FORCE_INLINE sint32 SignExtend12(uint16 value) {
    return static_cast<sint32>(static_cast<sint16>((value & 0x0FFF) << 4)) >> 4;
}

FORCE_INLINE sint32 SignExtend8(uint8 value) {
    return static_cast<sint32>(static_cast<sint8>(value));
}

} // namespace

const char *SH2Tracer::TraceEventMnemonic(TraceEventType type) {
    switch (type) {
    case TraceEventType::Call: return "call";
    case TraceEventType::Return: return "ret";
    case TraceEventType::ReturnFromException: return "rte";
    case TraceEventType::Trap: return "trap";
    case TraceEventType::Branch: return "branch";
    case TraceEventType::Jump: return "jump";
    case TraceEventType::Interrupt: return "intr";
    case TraceEventType::Exception: return "exc";
    case TraceEventType::StackPush: return "push";
    case TraceEventType::StackPop: return "pop";
    }
    return "unknown";
}

void SH2Tracer::ResetInterruptCounter() {
    m_interruptCounter = 0;
}

void SH2Tracer::ResetDivisionCounter() {
    m_divisionCounter = 0;
}

void SH2Tracer::ResetDMACounter(uint32 channel) {
    m_dmaCounter[channel] = 0;
}

void SH2Tracer::AttachSH2(const ymir::sh2::SH2 *sh2) {
    if (sh2 != nullptr) {
        m_probe = &sh2->GetProbe();
    }
}

void SH2Tracer::SetTraceFlowStack(bool enable) {
    traceFlowStack = enable;
    if (enable) {
        traceInstructions = true;
        traceInterrupts = true;
        traceExceptions = true;
        traceDivisions = true;
        traceDMA = true;
    }
}

void SH2Tracer::ExecuteInstruction(uint32 pc, uint16 opcode, bool delaySlot) {
    // execute write if tracing enabled
    if (traceInstructions) {
        instructions.Write({pc, opcode, delaySlot});
    }

    // if no exec trace, return as usual
    if (!traceFlowStack) {
        return;
    }

    // start flow/stack/execution trace logic 
    TraceEventType type{};
    uint32 target = 0;
    bool targetValid = false;
    uint32 spAfter = 0;

    if (!ClassifyFlowEvent(pc, opcode, type, target, targetValid, spAfter)) {
        return;
    }

    TraceEvent evt{};
    evt.type = type;
    evt.pc = pc;
    evt.target = target;
    evt.targetValid = targetValid;
    evt.delaySlot = delaySlot;
    evt.opcode = opcode;
    evt.counter = m_traceEventCounter++;

    if (m_probe != nullptr) {
        evt.regs = m_probe->R();
        evt.pr = m_probe->PR();
        evt.sr = m_probe->SR().u32;
        evt.gbr = m_probe->GBR();
        evt.vbr = m_probe->VBR();
        evt.spBefore = m_probe->R(15);
    } else {
        evt.regs.fill(0);
        evt.pr = evt.sr = evt.gbr = evt.vbr = 0;
        evt.spBefore = 0;
    }

    evt.spAfter = spAfter != 0 ? spAfter : evt.spBefore;

    traceEvents.Write(evt);
}

void SH2Tracer::Interrupt(uint8 vecNum, uint8 level, sh2::InterruptSource source, uint32 pc) {
    if (!traceInterrupts) {
        return;
    }

    interrupts.Write({vecNum, level, source, pc, m_interruptCounter++});
}

void SH2Tracer::Exception(uint8 vecNum, uint32 pc, uint32 sr) {
    if (!traceExceptions) {
        return;
    }

    exceptions.Write({vecNum, pc, sr});
}

void SH2Tracer::Begin32x32Division(sint32 dividend, sint32 divisor, bool overflowIntrEnable) {
    if (!traceDivisions) {
        return;
    }

    divisions.Write({.dividend = dividend,
                     .divisor = divisor,
                     .overflowIntrEnable = overflowIntrEnable,
                     .finished = false,
                     .div64 = false,
                     .counter = m_divisionCounter++});

    ++divStats.div32s;
}

void SH2Tracer::Begin64x32Division(sint64 dividend, sint32 divisor, bool overflowIntrEnable) {
    if (!traceDivisions) {
        return;
    }

    divisions.Write({.dividend = dividend,
                     .divisor = divisor,
                     .overflowIntrEnable = overflowIntrEnable,
                     .finished = false,
                     .div64 = true,
                     .counter = m_divisionCounter++});

    ++divStats.div64s;
}

void SH2Tracer::EndDivision(sint32 quotient, sint32 remainder, bool overflow) {
    if (!traceDivisions) {
        return;
    }

    auto &div = divisions.GetLast();
    if (div.finished) {
        return;
    }

    div.quotient = quotient;
    div.remainder = remainder;
    div.overflow = overflow;
    div.finished = true;

    if (overflow) {
        ++divStats.overflows;
        if (div.overflowIntrEnable) {
            ++divStats.interrupts;
        }
    }
}

void SH2Tracer::DMAXferBegin(uint32 channel, uint32 srcAddress, uint32 dstAddress, uint32 count, uint32 unitSize,
                             sint32 srcInc, sint32 dstInc) {
    if (!traceDMA) {
        return;
    }

    dmaTransfers[channel].Write({
        .srcAddress = srcAddress,
        .dstAddress = dstAddress,
        .count = count,
        .unitSize = unitSize,
        .srcInc = srcInc,
        .dstInc = dstInc,
        .finished = false,
        .counter = m_dmaCounter[channel]++,
    });

    ++dmaStats[channel].numTransfers;
}

void SH2Tracer::DMAXferData(uint32 channel, uint32 srcAddress, uint32 dstAddress, uint32 data, uint32 unitSize) {
    if (!traceDMA) {
        return;
    }

    auto &xfer = dmaTransfers[channel].GetLast();
    if (xfer.finished) {
        return;
    }

    // TODO: store transfer units in a shared/limited buffer or write directly to disk

    // auto &unit = xfer.units.emplace_back();
    // unit.srcAddress = srcAddress;
    // unit.dstAddress = dstAddress;
    // unit.data = data;
    // unit.unitSize = unitSize;

    dmaStats[channel].bytesTransferred += std::min(unitSize, 4u); // 16-byte transfers send four 4-byte units
}

void SH2Tracer::DMAXferEnd(uint32 channel, bool irqRaised) {
    if (!traceDMA) {
        return;
    }

    auto &xfer = dmaTransfers[channel].GetLast();
    if (xfer.finished) {
        return;
    }

    xfer.finished = true;
    xfer.irqRaised = irqRaised;

    if (irqRaised) {
        ++dmaStats[channel].interrupts;
    }
}

// map instructions to barrier type enumeration
// type, target, targetValid and spAfter get called by value for modification
// returns a bool so path in ExecuteInstruction() only gets taken if barrier event
// TODO: check again for correctness -> sh2 programming manual
bool SH2Tracer::ClassifyFlowEvent(uint32 pc, uint16 opcode, TraceEventType &type, uint32 &target, bool &targetValid,
                                  uint32 &spAfter) const {
    // dest/src operand for register ops
    const uint8 n = (opcode >> 8) & 0xF;
    const uint8 m = (opcode >> 8) & 0xF;

    // --- Calls ---
    if ((opcode & 0xF000) == 0xB000) { // BSR disp12
        // Branch to Subroutine
        // Delayed branch: PC → PR, disp × 2 + PC → PC
        // "The 12-bit displacement is sign-extended and doubled"
        const sint32 disp = SignExtend12(opcode) << 1;      // disp = dispatch * 2
        // PC = PC + (disp<<1) + 4 
        target = pc + 4 + disp;
        targetValid = true;
        type = TraceEventType::Call;
        return true;
    }
    if ((opcode & 0xF0FF) == 0x0003) { // BSRF Rm
        // Branch to Subroutine Far
        // Delayed branch: PC → PR, Rm + PC → PC
        targetValid = m_probe != nullptr;
        target = targetValid ? ( m_probe->R(m) + pc ) : 0;
        type = TraceEventType::Call;
        return true;
    }
    if ((opcode & 0xF0FF) == 0x400B) { // JSR @Rm
        // Jump to Subroutine
        // Delayed branch, PC → PR, Rm → PC
        targetValid = m_probe != nullptr;
        // PC = R[m] + 4;
        target = targetValid ? (m_probe->R(m) + 4) : 0;
        type = TraceEventType::Call;
        return true;
    }

    // --- Returns ---
    if (opcode == 0x000B) { // RTS
        // Return from Subroutine
        // Delayed branch: PR → PC 
        // PC = PR + 4
        type = TraceEventType::Return;
        return true;
    }
    if (opcode == 0x002B) { // RTE
        // Return from Exception
        // Delayed branch: stack area → PC/SR
        type = TraceEventType::ReturnFromException;
        return true;
    }

    // --- Trap ---
    if ((opcode & 0xFF00) == 0xC300) { // TRAPA #imm
        // Trap Always
        // PC/SR → stack area, (imm × 4 + VBR) → PC
        const uint32 imm = opcode & 0xFF;
        targetValid = m_probe != nullptr;
        if (targetValid) {
            // PC = Read_Long(VBR + (imm<<2)) + 4
            target = m_probe->VBR() + (imm << 2) + 4;
        }
        type = TraceEventType::Trap;
        // R[15] -= 4 twice (SR and PC-2 on stack)
        spAfter -= 8;
        return true;
    }

    // --- Branches ---
    if ((opcode & 0xF000) == 0xA000) { // BRA disp12
        // Branch
        // Delayed branch: disp × 2 + PC → PC
        // "The 12-bit displacement is sign-extended and doubled"
        const sint32 disp = SignExtend12(opcode) << 1;      // disp = dispatch * 2
        // PC = PC + (disp<<1) + 4;
        target = pc + 4 + disp;
        targetValid = true;
        type = TraceEventType::Branch;
        return true;
    }
    if ((opcode & 0xF0FF) == 0x0023) { // BRAF Rm
        // Delayed branch, Rm + PC → PC 
        targetValid = m_probe != nullptr;
        target = targetValid ? m_probe->R(m) : 0;
        type = TraceEventType::Branch;
        return true;
    }
    if ((opcode & 0xFF00) == 0x8900 || (opcode & 0xFF00) == 0x8D00 || // BT/BTS
        (opcode & 0xFF00) == 0x8B00 || (opcode & 0xFF00) == 0x8F00) { // BF/BFS
        // BT:      If T = 1, disp × 2 + PC → PC; if T = 0, nop (where label is disp + PC)
        // BT/S:    Delayed branch, if T = 1, disp × 2 + PC → PC; if T = 0, nop
        // BF:      If T = 0, disp × 2 + PC → PC; if T = 1, nop (where label is disp × 2 + PC)
        // BF/S:    Delayed branch, if T = 0, disp × 2 + PC → PC; if T = 1, nop
        bool taken = false;
        if (m_probe != nullptr) {
            const bool T = m_probe->SR().T;
            const bool isBT = (opcode & 0x0100) == 0x0100;  // BT/BTS set bit 8 = 1?
            const bool isBF = !isBT;
            taken = (isBT && T) || (isBF && !T);
        }
        if (taken || m_probe == nullptr) {
            const sint32 disp = SignExtend8(static_cast<uint8>(opcode & 0xFF)) << 1;
            target = pc + 4 + disp;
            targetValid = true;
            type = TraceEventType::Branch;
            return true;
        }
        return false;
    }

    // --- Jump ---
    if ((opcode & 0xF0FF) == 0x402B) { // JMP @Rm
        targetValid = m_probe != nullptr;
        target = targetValid ? m_probe->R(m) : 0;
        type = TraceEventType::Jump;
        return true;
    }

    // --- Stack push/pop (mov.l) ---
    if ((opcode & 0xF00F) == 0x2006 && n == 0xF) { // mov.l Rm, @-R15
        type = TraceEventType::StackPush;
        if (m_probe != nullptr) {
            spAfter = m_probe->R(15) - 4;
        }
        return true;
    }
    if ((opcode & 0xF00F) == 0x6006 && m == 0xF) { // mov.l @R15+, Rn
        type = TraceEventType::StackPop;
        if (m_probe != nullptr) {
            spAfter = m_probe->R(15) + 4;
        }
        return true;
    }

    // --- Stack adjust (add #imm, R15) ---
    if ((opcode & 0xF000) == 0x7000 && n == 0xF) {
        const sint32 delta = SignExtend8(static_cast<uint8>(opcode & 0xFF));
        if (delta != 0) {
            type = delta > 0 ? TraceEventType::StackPop : TraceEventType::StackPush;
            if (m_probe != nullptr) {
                spAfter = static_cast<uint32>(m_probe->R(15) + delta);
            }
            return true;
        }
    }

    return false;
}

} // namespace app
