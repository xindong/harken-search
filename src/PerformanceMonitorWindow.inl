static void DrawPerformanceMonitor(AppState& state) {
    namespace perf = harken::performance;
    namespace ui = aura::ui;
    auto& window = state.performanceMonitorWindow;
    const ui::StandaloneWindowOptions options{ImVec2(720, 620), ImVec2(520, 360)};
    if (!ui::BeginStandaloneWindow(L(PerfMonitor_Title), "PerformanceMonitor", &window, options)) {
        perf::SetEnabled(false);
        return;
    }

    perf::SetEnabled(true);
    const auto& snapshot = perf::Refresh();
    // The shared caption stays fixed. Scroll only the monitor content below it.
    if (ImGui::BeginChild("PerformanceMonitorBody", ImVec2(0, 0))) {
        ReadingTextScope reading;
        ImGui::PushStyleVar(ImGuiStyleVar_CellPadding,
            ImVec2(ui::metrics::kGap8, ui::metrics::kGap4));
        auto drawMetric = [](const char* label, std::optional<double> value, const char* format) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextWrapped("%s", label);
            ImGui::TableNextColumn();
            if (value) ImGui::Text(format, *value);
            else ImGui::TextDisabled("--");
        };
        auto mebibytes = [](auto bytes) -> std::optional<double> {
            constexpr double bytesPerMiB = 1024.0 * 1024.0;
            if (!bytes) return {};
            return *bytes / bytesPerMiB;
        };

        ui::SectionHeader(L(PerfMonitor_Refresh));
        if (BeginTransientScrollbarTable("PerformanceMetrics", 2,
                ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn("##Metric", ImGuiTableColumnFlags_WidthStretch, 2.0f);
            ImGui::TableSetupColumn("##Value", ImGuiTableColumnFlags_WidthStretch, 1.0f);
            drawMetric(L(PerfMonitor_Cpu), snapshot.cpuPercent, "%.2f%%");
            drawMetric(L(PerfMonitor_Read), mebibytes(snapshot.readBytesPerSecond), "%.2f MiB/s");
            drawMetric(L(PerfMonitor_Write), mebibytes(snapshot.writeBytesPerSecond), "%.2f MiB/s");
            drawMetric(L(PerfMonitor_WorkingSet), mebibytes(snapshot.workingSetBytes), "%.1f MiB");
            drawMetric(L(PerfMonitor_Private), mebibytes(snapshot.privateBytes), "%.1f MiB");
            ImGui::EndTable();
        }
        ImGui::TextWrapped("%s", L(PerfMonitor_ProcessHint));
        ui::Divider();
        ui::SectionHeader(L(PerfMonitor_ModulesTitle));

        constexpr std::array moduleLabels{
            LStr::PerfMonitor_Interface, LStr::PerfMonitor_Scan, LStr::PerfMonitor_Search,
            LStr::PerfMonitor_Similarity, LStr::PerfMonitor_AudioDecode, LStr::PerfMonitor_Waveform,
            LStr::PerfMonitor_Cover, LStr::PerfMonitor_Backfill,
        };
        static_assert(moduleLabels.size() == perf::ModuleCount);
        if (BeginTransientScrollbarTable("PerformanceModules", 3,
                ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH)) {
            ImGui::TableSetupColumn(L(PerfMonitor_Module), ImGuiTableColumnFlags_WidthStretch, 1.5f);
            ImGui::TableSetupColumn(L(PerfMonitor_Activity), ImGuiTableColumnFlags_WidthStretch, 1.0f);
            ImGui::TableSetupColumn(L(PerfMonitor_ModuleCpu), ImGuiTableColumnFlags_WidthStretch, 1.2f);
            ImGui::TableNextRow(ImGuiTableRowFlags_Headers);
            for (const char* label : {L(PerfMonitor_Module), L(PerfMonitor_Activity), L(PerfMonitor_ModuleCpu)}) {
                ImGui::TableNextColumn();
                ImGui::TextWrapped("%s", label);
            }
            auto drawModule = [&](const char* label, bool active, std::optional<double> cpu) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextWrapped("%s", label);
                ImGui::TableNextColumn();
                if (active) ImGui::TextWrapped("%s", L(PerfMonitor_Active));
                else if (cpu.value_or(0) > 0) ImGui::TextWrapped("%s", L(PerfMonitor_Recent));
                else ImGui::TextDisabled("%s", L(PerfMonitor_Idle));
                ImGui::TableNextColumn();
                if (cpu) ImGui::Text("%.1f ms", *cpu);
                else ImGui::TextDisabled("--");
            };
            for (std::size_t i = 0; i < moduleLabels.size(); ++i) {
                const auto& sample = snapshot.modules[i];
                drawModule(GetLocalizedString(moduleLabels[i], g_currentLanguage), sample.active > 0,
                           snapshot.hasModuleInterval ? std::optional{sample.cpuMilliseconds} : std::nullopt);
            }
            drawModule(L(PerfMonitor_Playback), state.audioEngine.isPlaying(), std::nullopt);
            ImGui::EndTable();
        }
        ImGui::TextWrapped("%s", L(PerfMonitor_ModuleHint));
        ImGui::PopStyleVar();
    }
    ImGui::EndChild();
    ui::EndStandaloneWindow();
}
