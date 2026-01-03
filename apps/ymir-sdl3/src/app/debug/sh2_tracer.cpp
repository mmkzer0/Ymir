#include "sh2_tracer.hpp"

using namespace ymir;

namespace app {

void SH2Tracer::ResetInterruptCounter() {
    m_interruptCounter = 0;
}

void SH2Tracer::ResetDivisionCounter() {
    m_divisionCounter = 0;
}

void SH2Tracer::ResetDMACounter(uint32 channel) {
    m_dmaCounter[channel] = 0;
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
    if (!traceInstructions) {
        return;
    }

    instructions.Write({pc, opcode, delaySlot});
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

} // namespace app
