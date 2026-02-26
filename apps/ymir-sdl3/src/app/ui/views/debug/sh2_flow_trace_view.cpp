#include "sh2_flow_trace_view.hpp"

#include <algorithm>

using namespace ymir;

namespace app::ui {

namespace {
    constexpr uint32 kMinRows = 1;
    constexpr uint32 kMaxRows = 4096;
} // namespace

SH2FlowTraceView::SH2FlowTraceView(SharedContext &context, SH2Tracer &masterTracer, SH2Tracer &slaveTracer,
                                   SH2FlowTraceModel &model)
    : m_context(context)
    , m_masterTracer(masterTracer)
    , m_slaveTracer(slaveTracer)
    , m_model(model) {}

void SH2FlowTraceView::Display() {
    if (ImGui::BeginTabBar("sh2_flow_trace_tabs")) {
        if (ImGui::BeginTabItem("Master SH2")) {
            m_model.selectedCore = SH2FlowTraceModel::Core::Master;
            auto &tracer = m_masterTracer;
            DisplayControls(tracer);
            ImGui::Separator();
            const size_t rows = std::min<size_t>(tracer.traceEvents.Count(), static_cast<size_t>(m_model.rowLimit));
            DisplayStatus(tracer, rows);
            DisplayTraceTable(tracer, "msh2_flow_trace_table", rows);
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Slave SH2")) {
            m_model.selectedCore = SH2FlowTraceModel::Core::Slave;
            auto &tracer = m_slaveTracer;
            DisplayControls(tracer);
            ImGui::Separator();
            const size_t rows = std::min<size_t>(tracer.traceEvents.Count(), static_cast<size_t>(m_model.rowLimit));
            DisplayStatus(tracer, rows);
            DisplayTraceTable(tracer, "ssh2_flow_trace_table", rows);
            ImGui::EndTabItem();
        }

        ImGui::EndTabBar();
    }
}

void SH2FlowTraceView::DisplayControls(SH2Tracer &currentTracer) {
    if (ImGui::Button("Clear current")) {
        // Frontend-only clear for visible history. Backend capture/session reset is deferred.
        currentTracer.traceEvents.Clear();
    }
    ImGui::SameLine();
    if (ImGui::Button("Clear both")) {
        m_masterTracer.traceEvents.Clear();
        m_slaveTracer.traceEvents.Clear();
    }

    ImGui::SameLine();
    const bool capturing = m_model.dumpState == SH2FlowTraceModel::DumpState::Capturing;
    if (ImGui::Button(capturing ? "Stop dump (stub)" : "Start dump (stub)")) {
        m_model.dumpState = capturing ? SH2FlowTraceModel::DumpState::Idle : SH2FlowTraceModel::DumpState::Capturing;
    }
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::BeginItemTooltip()) {
        ImGui::TextUnformatted("Dump control is frontend-only for now.\nBackend capture/export wiring via EmuEvent is "
                               "deferred to the next step.");
        ImGui::EndTooltip();
    }

    const float width = ImGui::GetContentRegionAvail().x;
    ImGui::SetNextItemWidth(width > 280.0f ? 280.0f : width);
    int compressionMode = static_cast<int>(m_model.compressionMode);
    if (ImGui::Combo("Compression/folding", &compressionMode, "Off\0Runtime fold (stub)\0")) {
        m_model.compressionMode = static_cast<SH2FlowTraceModel::CompressionMode>(compressionMode);
    }
    if (ImGui::BeginItemTooltip()) {
        ImGui::TextUnformatted("Runtime compression/folding mode is a UI stub.\nNo backend processing is active yet.");
        ImGui::EndTooltip();
    }

    ImGui::SetNextItemWidth(280.0f * m_context.displayScale);
    ImGui::SliderScalar("Rows", ImGuiDataType_U32, &m_model.rowLimit, &kMinRows, &kMaxRows, "%u");
}

void SH2FlowTraceView::DisplayStatus(const SH2Tracer &tracer, size_t rows) const {
    const size_t count = tracer.traceEvents.Count();

    ImGui::Text("Core: %s", GetCoreLabel(m_model.selectedCore));
    ImGui::SameLine();
    ImGui::Text("| Events: %zu (showing %zu)", count, rows);
    ImGui::SameLine();
    ImGui::Text("| Dump: %s", GetDumpStateLabel(m_model.dumpState));
    ImGui::SameLine();
    ImGui::Text("| Mode: %s", GetCompressionModeLabel(m_model.compressionMode));

    ImGui::Text("Dropped: %llu | Flushed chunks: %llu", static_cast<unsigned long long>(m_model.droppedEvents),
                static_cast<unsigned long long>(m_model.flushedChunks));
}

void SH2FlowTraceView::DisplayTraceTable(SH2Tracer &tracer, const char *tableId, size_t rows) {
    constexpr ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
                                      ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_ScrollY;

    const float paddingWidth = ImGui::GetStyle().FramePadding.x;
    ImGui::PushFont(m_context.fonts.monospace.regular, m_context.fontSizes.medium);
    const float hexCharWidth = ImGui::CalcTextSize("F").x;
    ImGui::PopFont();

    if (ImGui::BeginTable(tableId, 11, flags)) {
        ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed, paddingWidth * 2 + hexCharWidth * 10);
        ImGui::TableSetupColumn("Seq", ImGuiTableColumnFlags_WidthFixed, paddingWidth * 2 + hexCharWidth * 10);
        ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, 110.0f * m_context.displayScale);
        ImGui::TableSetupColumn("PC", ImGuiTableColumnFlags_WidthFixed, paddingWidth * 2 + hexCharWidth * 10);
        ImGui::TableSetupColumn("Target", ImGuiTableColumnFlags_WidthFixed, paddingWidth * 2 + hexCharWidth * 10);
        ImGui::TableSetupColumn("SP before", ImGuiTableColumnFlags_WidthFixed, paddingWidth * 2 + hexCharWidth * 10);
        ImGui::TableSetupColumn("SP after", ImGuiTableColumnFlags_WidthFixed, paddingWidth * 2 + hexCharWidth * 10);
        ImGui::TableSetupColumn("Delay", ImGuiTableColumnFlags_WidthFixed, 70.0f * m_context.displayScale);
        ImGui::TableSetupColumn("Has DS", ImGuiTableColumnFlags_WidthFixed, 80.0f * m_context.displayScale);
        ImGui::TableSetupColumn("Cond", ImGuiTableColumnFlags_WidthFixed, 70.0f * m_context.displayScale);
        ImGui::TableSetupColumn("Taken", ImGuiTableColumnFlags_WidthFixed, 70.0f * m_context.displayScale);
        ImGui::TableSetupScrollFreeze(1, 1);
        ImGui::TableHeadersRow();

        for (size_t i = 0; i < rows; ++i) {
            const auto evt = tracer.traceEvents.ReadReverse(i);

            ImGui::TableNextRow();

            if (ImGui::TableNextColumn()) {
                ImGui::PushFont(m_context.fonts.monospace.regular, m_context.fontSizes.medium);
                ImGui::Text("%llu", static_cast<unsigned long long>(evt.counter));
                ImGui::PopFont();
            }
            if (ImGui::TableNextColumn()) {
                ImGui::PushFont(m_context.fonts.monospace.regular, m_context.fontSizes.medium);
                ImGui::Text("%llu", static_cast<unsigned long long>(evt.sequenceId));
                ImGui::PopFont();
            }
            if (ImGui::TableNextColumn()) {
                ImGui::TextUnformatted(SH2Tracer::TraceEventMnemonic(evt.type));
            }
            if (ImGui::TableNextColumn()) {
                ImGui::PushFont(m_context.fonts.monospace.regular, m_context.fontSizes.medium);
                ImGui::Text("%08X", evt.pc);
                ImGui::PopFont();
            }
            if (ImGui::TableNextColumn()) {
                if (evt.targetValid) {
                    ImGui::PushFont(m_context.fonts.monospace.regular, m_context.fontSizes.medium);
                    ImGui::Text("%08X", evt.target);
                    ImGui::PopFont();
                } else {
                    ImGui::TextUnformatted("-");
                }
            }
            if (ImGui::TableNextColumn()) {
                ImGui::PushFont(m_context.fonts.monospace.regular, m_context.fontSizes.medium);
                ImGui::Text("%08X", evt.spBefore);
                ImGui::PopFont();
            }
            if (ImGui::TableNextColumn()) {
                ImGui::PushFont(m_context.fonts.monospace.regular, m_context.fontSizes.medium);
                ImGui::Text("%08X", evt.spAfter);
                ImGui::PopFont();
            }
            if (ImGui::TableNextColumn()) {
                ImGui::TextUnformatted(evt.delaySlot ? "Yes" : "No");
            }
            if (ImGui::TableNextColumn()) {
                ImGui::TextUnformatted(evt.hasDelaySlot ? "Yes" : "No");
            }
            if (ImGui::TableNextColumn()) {
                ImGui::TextUnformatted(evt.isConditionalBranch ? "Yes" : "No");
            }
            if (ImGui::TableNextColumn()) {
                ImGui::TextUnformatted(evt.isConditionalBranch ? (evt.branchTaken ? "Yes" : "No") : "-");
            }
        }

        ImGui::EndTable();
    }
}

const char *SH2FlowTraceView::GetCoreLabel(SH2FlowTraceModel::Core core) {
    return core == SH2FlowTraceModel::Core::Master ? "Master SH2" : "Slave SH2";
}

const char *SH2FlowTraceView::GetDumpStateLabel(SH2FlowTraceModel::DumpState state) {
    return state == SH2FlowTraceModel::DumpState::Capturing ? "Capturing (stub)" : "Idle";
}

const char *SH2FlowTraceView::GetCompressionModeLabel(SH2FlowTraceModel::CompressionMode mode) {
    switch (mode) {
    case SH2FlowTraceModel::CompressionMode::Off: return "Off";
    case SH2FlowTraceModel::CompressionMode::RuntimeFold: return "Runtime fold (stub)";
    }
    return "Unknown";
}

} // namespace app::ui
