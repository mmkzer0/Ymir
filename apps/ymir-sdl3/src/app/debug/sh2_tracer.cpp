#include "sh2_tracer.hpp"

#include <ymir/hw/sh2/sh2.hpp>
#include <ymir/hw/sh2/sh2_disasm.hpp>

using namespace ymir;

namespace app {

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
    if (!traceInstructions && !traceFlowStack) {
        return;
    }

    const uint64 sequenceId = m_instructionSequenceCounter++;

    // execute write if tracing enabled
    if (traceInstructions) {
        instructions.Write({pc, opcode, delaySlot, sequenceId});
    }

    // if no exec trace, return as usual
    if (!traceFlowStack) {
        return;
    }

    const auto &instr = sh2::Disassemble(opcode);

    // start flow/stack/execution trace logic
    TraceEventType type{};
    uint32 target = 0;
    bool targetValid = false;
    std::optional<uint32> spAfter{};
    bool isConditionalBranch = false;
    bool branchTaken = false;

    // opcode was no enumerated barrier -> return early
    if (!ClassifyFlowEvent(pc, instr, delaySlot, type, target, targetValid, spAfter, isConditionalBranch,
                           branchTaken)) {
        return;
    }

    // build new event
    TraceEvent evt{};
    evt.type = type;
    evt.pc = pc;
    evt.target = target;
    evt.targetValid = targetValid;
    evt.delaySlot = delaySlot;
    evt.hasDelaySlot = instr.hasDelaySlot;
    evt.isConditionalBranch = isConditionalBranch;
    evt.branchTaken = branchTaken;
    evt.opcode = opcode;
    evt.sequenceId = sequenceId;
    evt.counter = m_traceEventCounter++;

    // ensure probe is not null
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

    // set sp after instruction
    evt.spAfter = spAfter.value_or(evt.spBefore);

    // forward event to ring buffer
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

bool SH2Tracer::ClassifyFlowEvent(uint32 pc, const sh2::DisassembledInstruction &instr, bool delaySlot,
                                  TraceEventType &type, uint32 &target, bool &targetValid,
                                  std::optional<uint32> &spAfter, bool &isConditionalBranch, bool &branchTaken) const {
    // TODO(test): add coverage for delay-slot metadata, conditional branch outcomes, and sequence correlation.
    // Keep trace output aligned with actual execution semantics.
    if (delaySlot && !instr.validInDelaySlot) {
        return false;
    }

    isConditionalBranch = false;
    branchTaken = false;

    auto setDispPCTarget = [&](const sh2::Operand &operand) {
        if (operand.type != sh2::Operand::Type::DispPC) {
            return false;
        }
        target = pc + static_cast<uint32>(operand.immDisp);
        targetValid = true;
        return true;
    };

    auto setRegisterTarget = [&](uint8 reg, uint32 bias) {
        if (m_probe == nullptr) {
            return false;
        }
        target = m_probe->R(reg) + bias;
        targetValid = true;
        return true;
    };

    switch (instr.mnemonic) {
    case sh2::Mnemonic::BSR:
        type = TraceEventType::Call;
        setDispPCTarget(instr.op1);
        return true;

    case sh2::Mnemonic::BSRF:
        type = TraceEventType::Call;
        if (instr.op1.type == sh2::Operand::Type::RnPC) {
            setRegisterTarget(instr.op1.reg, pc + 4);
        }
        return true;

    case sh2::Mnemonic::JSR:
        type = TraceEventType::Call;
        if (instr.op1.type == sh2::Operand::Type::AtRnPC) {
            setRegisterTarget(instr.op1.reg, 0);
        }
        return true;

    case sh2::Mnemonic::RTS:
        type = TraceEventType::Return;
        if (m_probe != nullptr) {
            target = m_probe->PR();
            targetValid = true;
        }
        return true;

    case sh2::Mnemonic::RTE:
        type = TraceEventType::ReturnFromException;
        if (m_probe != nullptr) {
            const uint32 stackPointer = m_probe->R(15);
            target = m_probe->MemPeekLong(stackPointer, false);
            targetValid = true;
            spAfter = stackPointer + 8;
        }
        return true;

    case sh2::Mnemonic::TRAPA:
        type = TraceEventType::Trap;
        if (m_probe != nullptr && instr.op1.type == sh2::Operand::Type::Imm) {
            const uint32 stackPointer = m_probe->R(15);
            const uint32 vectorAddress = m_probe->VBR() + static_cast<uint32>(instr.op1.immDisp);
            target = m_probe->MemPeekLong(vectorAddress, false);
            targetValid = true;
            spAfter = stackPointer - 8;
        }
        return true;

    case sh2::Mnemonic::BRA:
        type = TraceEventType::Branch;
        setDispPCTarget(instr.op1);
        return true;

    case sh2::Mnemonic::BRAF:
        type = TraceEventType::Branch;
        if (instr.op1.type == sh2::Operand::Type::RnPC) {
            setRegisterTarget(instr.op1.reg, pc + 4);
        }
        return true;

    case sh2::Mnemonic::BT:
    case sh2::Mnemonic::BTS:
    case sh2::Mnemonic::BF:
    case sh2::Mnemonic::BFS: {
        isConditionalBranch = true;
        if (m_probe != nullptr) {
            const bool testTrue = instr.mnemonic == sh2::Mnemonic::BT || instr.mnemonic == sh2::Mnemonic::BTS;
            branchTaken = testTrue ? m_probe->SR().T : !m_probe->SR().T;
        }
        type = TraceEventType::Branch;
        setDispPCTarget(instr.op1);
        return true;
    }

    case sh2::Mnemonic::JMP:
        type = TraceEventType::Jump;
        if (instr.op1.type == sh2::Operand::Type::AtRnPC) {
            setRegisterTarget(instr.op1.reg, 0);
        }
        return true;

    case sh2::Mnemonic::MOV:
        if (instr.opSize == sh2::OperandSize::Long && instr.op2.type == sh2::Operand::Type::AtMinusRn &&
            instr.op2.reg == 15) {
            type = TraceEventType::StackPush;
            if (m_probe != nullptr) {
                spAfter = m_probe->R(15) - 4;
            }
            return true;
        }
        if (instr.opSize == sh2::OperandSize::Long && instr.op1.type == sh2::Operand::Type::AtRnPlus &&
            instr.op1.reg == 15) {
            type = TraceEventType::StackPop;
            if (m_probe != nullptr) {
                spAfter = m_probe->R(15) + 4;
            }
            return true;
        }
        return false;

    case sh2::Mnemonic::ADD:
        if (instr.op1.type == sh2::Operand::Type::Imm && instr.op2.type == sh2::Operand::Type::Rn &&
            instr.op2.reg == 15) {
            const sint32 delta = instr.op1.immDisp;
            if (delta == 0) {
                return false;
            }
            type = delta > 0 ? TraceEventType::StackPop : TraceEventType::StackPush;
            if (m_probe != nullptr) {
                const sint64 nextSp = static_cast<sint64>(m_probe->R(15)) + static_cast<sint64>(delta);
                spAfter = static_cast<uint32>(nextSp);
            }
            return true;
        }
        return false;

    default: return false;
    }
}

} // namespace app
