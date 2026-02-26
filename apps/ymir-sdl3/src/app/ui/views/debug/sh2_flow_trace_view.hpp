#pragma once

#include "sh2_flow_trace_model.hpp"

#include <app/debug/sh2_tracer.hpp>
#include <app/shared_context.hpp>

namespace app::ui {

class SH2FlowTraceView {
public:
    SH2FlowTraceView(SharedContext &context, SH2Tracer &masterTracer, SH2Tracer &slaveTracer, SH2FlowTraceModel &model);

    void Display();

private:
    SharedContext &m_context;
    SH2Tracer &m_masterTracer;
    SH2Tracer &m_slaveTracer;
    SH2FlowTraceModel &m_model;

    void DisplayControls(SH2Tracer &currentTracer);
    void DisplayStatus(const SH2Tracer &tracer, size_t rows) const;
    void DisplayTraceTable(SH2Tracer &tracer, const char *tableId, size_t rows);

    static const char *GetCoreLabel(SH2FlowTraceModel::Core core);
    static const char *GetDumpStateLabel(SH2FlowTraceModel::DumpState state);
    static const char *GetCompressionModeLabel(SH2FlowTraceModel::CompressionMode mode);
};

} // namespace app::ui
