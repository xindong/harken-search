// Existing native specialty UI, hosted by the settings action dialog.
                    ImGui::Text("%s", L(Main_HealthCenterTitle));
                    ImGui::Separator();
                    static RepairCenterSnapshot s_repairSnapshot;
                    static bool s_repairSnapshotReady = false;
                    if (aura::ui::SettingsButtonRow(
                            L(Main_RefreshHealthBtn),
                            ImVec2(180, 28),
                            aura::ui::SettingsRowOptions{
                                L(Main_RefreshHealthHint) })) {
                        s_repairSnapshot = state.database.getRepairCenterSnapshot();
                        s_repairSnapshotReady = true;
                    }                    if (s_repairSnapshotReady) {
                        const ImVec4 okColor(0.35f, 0.85f, 0.45f, 1.0f);
                        const ImVec4 warnColor(1.0f, 0.68f, 0.22f, 1.0f);
                        ImGui::TextColored(s_repairSnapshot.needsAttention ? warnColor : okColor,
                            "%s", s_repairSnapshot.needsAttention ? L(Main_NeedAttention) : L(Main_AllClear));
                        ImGui::Text(L(Main_DbFilesFmt),
                            FileBrowser::formatFileSize((int64_t)s_repairSnapshot.packageBytes).c_str(),
                            FileBrowser::formatFileSize((int64_t)s_repairSnapshot.walBytes).c_str(),
                            FileBrowser::formatFileSize((int64_t)s_repairSnapshot.shmBytes).c_str());
                        ImGui::Text(L(Main_LibRootsFmt),
                            s_repairSnapshot.libraryRootCount,
                            s_repairSnapshot.missingLibraryRootCount);
                        ImGui::Text(L(Main_WaveCacheStatsFmt),
                            s_repairSnapshot.failedWaveformRows,
                            s_repairSnapshot.pendingWaveformRows,
                            s_repairSnapshot.missingCoverThumbRows);
                        if (!s_repairSnapshot.warnings.empty()) {
                            ImGui::TextDisabled(L(Main_HealthHintLabel));
                            for (size_t i = 0; i < s_repairSnapshot.warnings.size() && i < 4; ++i) {
                                ImGui::BulletText("%s", s_repairSnapshot.warnings[i].c_str());
                            }
                        }
                        if (!s_repairSnapshot.unreachableLibraryRoots.empty()) {
                            ImGui::TextDisabled(L(Scan_NetworkLibraryUnreachable));
                            for (size_t i = 0; i < s_repairSnapshot.unreachableLibraryRoots.size() && i < 3; ++i) {
                                ImGui::BulletText("%s", s_repairSnapshot.unreachableLibraryRoots[i].c_str());
                            }
                        }
                        if (!s_repairSnapshot.missingLibraryRoots.empty()) {
                            ImGui::TextDisabled(L(Main_MissingRootsLabel));
                            for (size_t i = 0; i < s_repairSnapshot.missingLibraryRoots.size() && i < 3; ++i) {
                                ImGui::BulletText("%s", s_repairSnapshot.missingLibraryRoots[i].c_str());
                            }
                        }
                    } else {
                        ImGui::TextDisabled(L(Main_HealthExplainHint));
                    }
                    ImGui::Spacing();

                    ImGui::BeginDisabled(state.cleanupInvalidRecordsActive);
                    if (aura::ui::ActionButton(L(Set_CleanInvalid2), ImVec2(180, 28))) {
                        if (!state.cleanupInvalidRecordsActive) {
                            state.cleanupInvalidRecordsActive = true;
                            state.cleanupInvalidRecordsCancel = false;
                            state.cleanupInvalidRecordsTotal = static_cast<int>(state.database.getFileCount());
                            state.cleanupInvalidRecordsProgress = 0;
                            std::thread([&state]() {
                                (void)state.database.cleanupMissingFiles(
                                    [&state](int cur, int tot, const std::string&, bool) {
                                        state.cleanupInvalidRecordsProgress = cur;
                                        state.cleanupInvalidRecordsTotal = tot;
                                    },
                                    &state.cleanupInvalidRecordsCancel);
                                state.cleanupInvalidRecordsActive = false;
                                state.needsDatabaseRefresh = true;
                            }).detach();
                        }
                    }
                    ImGui::EndDisabled();
                    if (state.cleanupInvalidRecordsActive) {
                        const int prog = state.cleanupInvalidRecordsProgress.load(std::memory_order_relaxed);
                        const int tot = state.cleanupInvalidRecordsTotal.load(std::memory_order_relaxed);
                        float frac = (tot > 0) ? (float)prog / (float)tot : 0.0f;
                        char buf[128];
                        snprintf(buf, sizeof(buf), u8"%d / %d", prog, tot);
                        DrawUnifiedProgressBar(frac, ImVec2(-1, 0), buf);
                        if (aura::ui::SmallActionButton(L(Set_CancelClean), aura::ui::ButtonVariant::Quiet)) state.cleanupInvalidRecordsCancel = true;
                    }
                    // 重建封面索引始终新起一行，避免清理进行时 SameLine 随「取消清理」导致位置跑偏
                    ImGui::BeginDisabled(state.coverRebuildActive);
                    if (aura::ui::ActionButton(L(Set_RebuildCoverIndex), ImVec2(180, 28))) {
                        state.audioEngine.stop();
                        ForceCoverWallReload(state);
                        StartCoverRebuild(state);
                    }
                    ImGui::EndDisabled();
                    if (state.coverRebuildActive) {
                        float progress = state.coverRebuildTotal > 0
                            ? (float)state.coverRebuildProgress / (float)state.coverRebuildTotal : 0.0f;
                        char buf[128];
                        snprintf(buf, sizeof(buf), L(Scan_ProgressCountFmt),
                                 (long long)state.coverRebuildProgress, (long long)state.coverRebuildTotal, state.coverRebuildFound);
                        DrawUnifiedProgressBar(progress, ImVec2(-1, 0), buf);
                        if (aura::ui::SmallActionButton(L(Set_CancelRebuild), aura::ui::ButtonVariant::Quiet)) state.coverRebuildCancel = true;
                    }

                    ImGui::Spacing();
                    ImGui::Text("%s", L(Set_WaveCache));
                    ImGui::Separator();
                    static int s_detailPurged = -1, s_miniPurged = -1;
                    if (aura::ui::ActionButton(L(Set_ClearDetailCache), ImVec2(180, 28), aura::ui::ButtonVariant::Danger)) {
                        s_detailPurged = state.database.purgeAllWaveformDetail();
                        DIAG_ACTION_FMT("Database", "ManualPurgeWaveformDetail: rows=%d", s_detailPurged);
                    }
                    if (HoveredForHelp()) aura::ui::Tooltip(L(Set_ClearDetailHint));
                    if (s_detailPurged >= 0) {
                        ImGui::SameLine();
                        ImGui::TextColored(ImVec4(0.3f,0.8f,0.3f,1), L(Set_ClearedN), s_detailPurged);
                    }
                    if (aura::ui::ActionButton(L(Set_ClearMiniCache), ImVec2(180, 28), aura::ui::ButtonVariant::Danger)) {
                        s_miniPurged = state.database.purgeAllMiniWaveform();
                        DIAG_ACTION_FMT("Database", "ManualPurgeMiniWaveform: rows=%d", s_miniPurged);
                    }
                    if (HoveredForHelp()) aura::ui::Tooltip(L(Set_ClearMiniHint));
                    if (s_miniPurged >= 0) {
                        ImGui::SameLine();
                        ImGui::TextColored(ImVec4(0.3f,0.8f,0.3f,1), L(Set_ClearedN), s_miniPurged);
                    }


if (aura::ui::RenderRelinkTool(state)) RefreshSearchResults(state, true);
