#include "sh2_flow_trace_window.hpp"

namespace app::ui {

SH2FlowTraceWindow::SH2FlowTraceWindow(SharedContext &context)
    : WindowBase(context)
    , m_flowTraceView(context, context.tracers.masterSH2, context.tracers.slaveSH2, m_model) {

    m_windowConfig.name = "SH2 flow trace";
}

void SH2FlowTraceWindow::PrepareWindow() {
    ImGui::SetNextWindowSizeConstraints(ImVec2(720 * m_context.displayScale, 240 * m_context.displayScale),
                                        ImVec2(1200 * m_context.displayScale, FLT_MAX));
}

void SH2FlowTraceWindow::DrawContents() {
    m_flowTraceView.Display();
}

} // namespace app::ui
