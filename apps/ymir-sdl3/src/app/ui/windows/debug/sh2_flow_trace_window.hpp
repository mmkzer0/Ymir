#pragma once

#include <app/ui/window_base.hpp>

#include <app/ui/views/debug/sh2_flow_trace_model.hpp>
#include <app/ui/views/debug/sh2_flow_trace_view.hpp>

namespace app::ui {

class SH2FlowTraceWindow : public WindowBase {
public:
    SH2FlowTraceWindow(SharedContext &context);

protected:
    void PrepareWindow() override;
    void DrawContents() override;

private:
    SH2FlowTraceModel m_model;
    SH2FlowTraceView m_flowTraceView;
};

} // namespace app::ui
