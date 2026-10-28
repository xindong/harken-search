"""Deterministic Stage 7 public-header inventory baseline."""

from __future__ import annotations

import argparse
import hashlib
import json
import re
from pathlib import Path
from typing import Any

HEADER_SUFFIXES = {".h", ".hpp", ".inl"}
SOURCE_SUFFIXES = HEADER_SUFFIXES | {".c", ".cc", ".cpp", ".cxx", ".mm"}
INCLUDE_RE = re.compile(r"^\s*#\s*include\s*[<\"]([^>\"]+)[>\"]")
REVIEWED_CONTRACTS = {
    "src/CoverWallSelectionKey.h": "tests/test_cover_wall_page_dedupe.cpp",
    "src/InfoEditSession.h": "tests/test_info_edit_session.cpp",
    "src/InfoPageNavigation.h": "tests/test_info_page_navigation.cpp",
    "src/MiniWaveformPresentation.h": "tests/test_mini_waveform_presentation.cpp",
    "src/ScanDatabaseOpenDiagnostics.h": "tests/test_scan_database_open_diagnostics.cpp",
    "src/ui/EmptyStatePalette.h": "tests/test_empty_state_contrast.cpp",
    "src/ui/EmptyStateVisualAudit.h": "tests/test_empty_state_theme_audit.py",
    "src/ui/MistBlueChrome.h": "tests/test_mist_blue_theme.cpp",
    "src/ui/TransportBlendSlider.h": "tests/test_transport_blend_input.cpp",
    "src/ui/TransportContrast.h": "tests/test_transport_contrast.cpp",
    "src/ui/TransportValueText.h": "tests/test_transport_blend_input.cpp",
    "src/ui/TransportVisualAudit.h": "tests/test_transport_blend_input.cpp",
    "src/AuditionPluginChain.h": "tests/test_audition_plugin_chain.cpp",
    "src/ConsentManager.h": "tests/test_consent_manager.cpp",
    "src/DefaultAudioPlayerAssociation.h": "tests/test_default_audio_player_association.cpp",
    "src/DatabasePackageReplacement.h": "tests/test_database_package_replacement.cpp",
    "src/DiagnosticFileLifecycle.h": "tests/test_diagnostic_file_lifecycle.cpp",
    "src/DockDragSession.h": "tests/test_dock_drag_session.cpp",
    "src/DockHitTest.h": "tests/test_dock_hit_test.cpp",
    "src/DockLayout.h": "tests/test_dock_layout_tree.cpp",
    "src/DockNativeCaptionBridge.h": "tests/test_dock_native_caption_bridge.cpp",
    "src/DragPauseOnStartPolicy.h": "tests/test_drag_pause_on_start_policy.cpp",
    "src/FolderTreePathModePolicy.h": "tests/test_folder_tree_path_mode_policy.cpp",
    "src/OutputDeviceComboItem.h": "tests/test_output_device_combo_item.cpp",
    "src/PlaylistCoverScope.h": "tests/test_playlist_cover_scope.cpp",
    "src/PlaylistScopeCounts.h": "tests/test_playlist_scope_counts.cpp",
    "src/ResultRowHighlightSpans.h": "tests/test_result_row_highlight_spans.cpp",
    "src/SearchHitColumnIdentity.h": "tests/test_search_hit_column_identity.cpp",
    "src/SettingsBodyScrollHeight.h": "tests/test_settings_body_scroll_height.cpp",
    "src/SettingsSearchHighlightSpans.h": "tests/test_settings_search_highlight_spans.cpp",
    "src/SettingsMigration.h": "tests/test_settings_migration.cpp",
    "src/TagCloudContext.h": "tests/test_tag_cloud_context.cpp",
    "src/ViewportGlLifecycle.h": "tests/test_viewport_gl_lifecycle.cpp",
    "src/bootstrap/HarkenSearchBootstrapExitExplanation.h": "tests/test_bootstrap_exit_explanation.cpp",
    "src/platform/filesystem_probe_path_test_hooks.h": "tests/test_network_unreachable_probe.cpp",
    "src/platform/harken_user_data_layout.h": "tests/test_preview_isolation_guard.cpp",
    "src/platform/miniz_archive_reader.h": "tests/test_update_bootstrap_authority_port_boundary.py",
    "src/platform/update_deployment_identity.h": "tests/bootstrap/test_update_deployment_identity.cpp",
    "src/ui/dock/DockCaptionWndProc.h": "tests/test_dock_frame_live.cpp",
    "src/ui/dock/DockPanelStateBridge.h": "tests/test_dock_adapter_contract.py",
    "src/update/HarkenSearchUpdateNoticeModel.h": "tests/test_update_notice_content.cpp",
    "src/update/HarkenSearchUpdateNoticeUi.h": "tests/test_update_notice_presentation.cpp",
    "src/bootstrap/HarkenSearchBootstrapAdmission.h": "tests/bootstrap/test_aris_bootstrap_admission.cpp",
    "src/bootstrap/HarkenSearchBootstrapMain.h": "tests/bootstrap/test_aris_bootstrap_entrypoint.cpp",
    "src/bootstrap/HarkenSearchBootstrapPlan.h": "tests/bootstrap/test_aris_bootstrap_plan.cpp",
    "src/bootstrap/HarkenSearchBootstrapRuntime.h": "tests/bootstrap/test_aris_bootstrap_runtime.cpp",
    "src/update/HarkenSearchUpdateComposition.h": "tests/test_update_product_composition.py",
    "src/ImGuiMouseCaptureLoss.h": "tests/test_magic_eq_capture_loss_win32.cpp",
    "src/platform/floating_window_corners.h": "tests/test_floating_window_corners.cpp",
    "src/release_notes/ReleaseNotes.h": "tests/test_release_notes_native_log_contract.py",
    "src/platform/update_bootstrap_authority_port.h": "tests/test_update_bootstrap_authority_port_boundary.py",
    "src/platform/update_bootstrap_health_port.h": "tests/test_update_bootstrap_health_capacity_boundary.py",
    "src/platform/update_staging_capacity_port.h": "tests/test_update_bootstrap_health_capacity_boundary.py",
    "src/bootstrap/WindowsBootstrapFilesystem.h": "tests/bootstrap/test_aris_bootstrap_filesystem.cpp",
    "src/bootstrap/WindowsBootstrapLaunchBroker.h": "tests/bootstrap/test_aris_bootstrap_launch_broker.cpp",
    "src/HarkenSearchDesignTokens.generated.h": "tests/test_aris_design_tokens.py",
    "src/platform/filesystem.h": "tests/test_stage7_platform_contract.py",
    "src/platform/audio_file.h": "tests/test_stage7_platform_contract.py",
    "src/platform/audio_playback.h": "tests/test_stage7_platform_contract.py",
    "src/platform/audio_data_source.h": "tests/test_stage7_platform_contract.py",
    "src/platform/durable_file_ops.h": "tests/test_stage7_platform_contract.py",
    "src/platform/text_encoding.h": "tests/test_stage7_platform_contract.py",
    "src/platform/executable_path.h": "tests/test_stage7_platform_contract.py",
    "src/MetadataReader.h": "tests/test_stage7_platform_contract.py",
    "src/MetadataWriter.h": "tests/test_stage7_platform_contract.py",
    "src/AudioEngine.h": "tests/test_stage7_platform_contract.py",
    "src/TransferEngine.h": "tests/test_stage7_platform_contract.py",
    "src/platform/system.h": "tests/test_stage7_platform_contract.py",
    "src/DiagnosticsLogger.h": "tests/test_stage7_platform_contract.py",
    "src/Database.h": "tests/test_stage7_platform_contract.py",
    "src/Utils.h": "tests/test_stage7_platform_contract.py",
    "src/Theme.h": "tests/test_stage7_platform_contract.py",
    "src/AIEngine.h": "tests/test_stage7_platform_contract.py",
    "src/Localization.h": "tests/test_stage7_platform_contract.py",
    "src/PathUtils.h": "tests/test_stage7_platform_contract.py",
    "src/WaapiClient.h": "tests/test_stage7_platform_contract.py",
    "src/MediaFoundationAudioDecoder.h": "tests/test_stage7_platform_contract.py",
    "src/AppState.h": "tests/test_stage7_platform_contract.py",
    "src/BuildConfig.h": "tests/test_stage7_platform_contract.py",
    "src/CoverModule.h": "tests/test_stage7_platform_contract.py",
    "src/WwiseTransferConfig.h": "tests/test_stage7_platform_contract.py",
    "src/WaveformCachePolicy.h": "tests/test_stage7_platform_contract.py",
    "src/WaveformExtractor.h": "tests/test_stage7_platform_contract.py",
    "src/platform/text_encoding.inl": "tests/test_stage7_platform_contract.py",
    "src/HarkenSearchWidgets.h": "tests/test_stage7_platform_contract.py",
    "src/BackfillQueue.h": "tests/test_stage7_platform_contract.py",
    "src/BpmDetector.h": "tests/test_stage7_platform_contract.py",
    "src/MemoryBudget.h": "tests/test_stage7_platform_contract.py",
    "src/ThemeInspector.h": "tests/test_stage7_platform_contract.py",
    "src/HarkenSearchIcons.h": "tests/test_stage7_platform_contract.py",
    "src/AvatarCropper.h": "tests/test_stage7_platform_contract.py",
    "src/FeatureExtractor.h": "tests/test_stage7_platform_contract.py",
    "src/KeyDetector.h": "tests/test_stage7_platform_contract.py",
    "src/WindowsMain.h": "tests/test_stage7_platform_contract.py",
    "src/WwiseStructureTemplate.h": "tests/test_stage7_platform_contract.py",
    "src/platform/named_pipe.h": "tests/test_stage7_platform_contract.py",
    "src/BackfillWorkerPool.h": "tests/test_stage7_platform_contract.py",
    "src/BetaFeatureFlags.h": "tests/test_stage7_platform_contract.py",
    "src/CoverReplacementFoundation.h": "tests/test_stage7_platform_contract.py",
    "src/DevToolsModule.h": "tests/test_stage7_platform_contract.py",
    "src/ProToolsBridge.h": "tests/test_stage7_platform_contract.py",
    "src/SearchQuery.h": "tests/test_stage7_platform_contract.py",
    "src/UserActionLog.h": "tests/test_stage7_platform_contract.py",
    "src/WwiseTransferPersist.h": "tests/test_stage7_platform_contract.py",
    "src/platform/process_launch.h": "tests/test_stage7_platform_contract.py",
    "src/platform/process_query.h": "tests/test_stage7_platform_contract.py",
    "src/platform/thread_id.h": "tests/test_stage7_platform_contract.py",
    "src/AsyncWorker.h": "tests/test_stage7_platform_contract.py",
    "src/CueviFeedbackClient.h": "tests/test_stage6_platform_boundary.py",
    "src/CueviFeedbackCompletionWorkflow.h": "tests/test_feedback_transport_boundary.py",
    "src/SingleChannelExportWorkflow.h": "tests/test_single_channel_export_workflow_boundary.py",
    "src/SingleChannelExportCompletionWorkflow.h": "tests/test_single_channel_export_completion_workflow_boundary.py",
    "src/CoverKeyCachePersistenceWorkflow.h": "tests/test_cover_key_cache_persistence_workflow_boundary.py",
    "src/DeferredSidebarBootstrapWorkflow.h": "tests/test_deferred_sidebar_bootstrap_workflow_boundary.py",
    "src/ReaperFileListImportWorkflow.h": "tests/test_reaper_file_list_import_workflow_boundary.py",
    "src/AsyncSearchFacetResultWorkflow.h": "tests/test_async_search_facet_result_workflow_boundary.py",
    "src/AsyncSearchFacetResultStateAdapter.h": "tests/test_async_search_facet_result_state_adapter_boundary.py",
    "src/RandomShufflePlaybackWorkflow.h": "tests/test_random_shuffle_playback_workflow_boundary.py",
    "src/RandomShufflePlaybackStateAdapter.h": "tests/test_random_shuffle_playback_state_adapter_boundary.py",
    "src/AsyncSearchRefreshFacetsWorkflow.h": "tests/test_async_search_refresh_facets_boundary.py",
    "src/AsyncSearchRefreshFacetsAdapter.h": "tests/test_async_search_refresh_facets_adapter_boundary.py",
    "src/AsyncSearchAppendResultWorkflow.h": "tests/test_async_search_append_result_boundary.py",
    "src/AsyncSearchAppendResultStateAdapter.h": "tests/test_async_search_append_result_state_adapter_boundary.py",
    "src/AsyncSearchRefreshCountRepairWorkflow.h": "tests/test_async_search_refresh_count_repair_boundary.py",
    "src/AsyncSearchRefreshCountRepairAdapter.h": "tests/test_async_search_refresh_count_repair_adapter_boundary.py",
    "src/AsyncSearchRefreshCompletionWorkflow.h": "tests/test_async_search_refresh_completion_boundary.py",
    "src/AsyncSearchRefreshCompletionStateAdapter.h": "tests/test_async_search_refresh_completion_state_adapter_boundary.py",
    "src/AsyncSearchRefreshMemoryWorkflow.h": "tests/test_async_search_refresh_memory_boundary.py",
    "src/AsyncSearchRefreshMemoryAdapter.h": "tests/test_async_search_refresh_memory_adapter_boundary.py",
    "src/AsyncSearchRefreshMemoryStateAdapter.h": "tests/test_async_search_refresh_memory_state_adapter_boundary.py",
    "src/AsyncSearchRefreshLoadingWorkflow.h": "tests/test_async_search_refresh_loading_boundary.py",
    "src/AsyncSearchRefreshLoadingStateAdapter.h": "tests/test_async_search_refresh_loading_state_adapter_boundary.py",
    "src/AsyncSearchWaveformPreloadWorkflow.h": "tests/test_async_search_waveform_preload_boundary.py",
    "src/AsyncSearchWaveformPreloadStateAdapter.h": "tests/test_async_search_waveform_preload_state_adapter_boundary.py",
    "src/AsyncSearchCoverPreloadWorkflow.h": "tests/test_async_search_cover_preload_boundary.py",
    "src/AsyncSearchCoverPreloadAdapter.h": "tests/test_async_search_cover_preload_adapter_boundary.py",
    "src/AsyncSearchCoverPreloadStateAdapter.h": "tests/test_async_search_cover_preload_state_adapter_boundary.py",
    "src/AsyncSearchEmptyFallbackWorkflow.h": "tests/test_async_search_empty_fallback_boundary.py",
    "src/AsyncSearchEmptyFallbackStateAdapter.h": "tests/test_async_search_empty_fallback_state_adapter_boundary.py",
    "src/AsyncSearchRefreshDiagnosticsWorkflow.h": "tests/test_async_search_refresh_diagnostics_boundary.py",
    "src/AsyncSearchRefreshDiagnosticsStateAdapter.h": "tests/test_async_search_refresh_diagnostics_state_adapter_boundary.py",
    "src/AsyncSearchRefreshResultsWorkflow.h": "tests/test_async_search_refresh_results_boundary.py",
    "src/AsyncSearchRefreshResultsStateAdapter.h": "tests/test_async_search_refresh_results_state_adapter_boundary.py",
    "src/AsyncSearchRefreshResultCompositionAdapter.h": "tests/test_async_search_refresh_result_composition_adapter_boundary.py",
    "src/AsyncSearchResultApplicationAdapter.h": "tests/test_async_search_result_application_adapter_boundary.py",
    "src/AsyncSearchResultApplicationWorkflow.h": "tests/test_async_search_result_application_workflow_boundary.py",
    "src/AsyncSearchResultEffectFactory.h": "tests/test_async_search_result_effect_factory_boundary.py",
    "src/SearchFilterStatePolicy.h": "tests/test_search_filter_state_policy_boundary.py",
    "src/SearchKeywordCompiler.h": "tests/test_search_keyword_compiler_boundary.py",
    "src/SearchFilterFileTypeAdapter.h": "tests/test_search_filter_file_type_adapter_boundary.py",
    "src/DatabasePathNormalizationPolicy.h": "tests/test_database_path_normalization_policy_boundary.py",
    "src/SearchPresetFilterBuilders.h": "tests/test_search_preset_filter_builders_boundary.py",
    "src/DatabaseSearchFilterBuilder.h": "tests/test_database_search_filter_builder_boundary.py",
    "src/CoverWallFilterActivityPolicy.h": "tests/test_cover_wall_filter_activity_policy_boundary.py",
    "src/CoverFacetItemBuilder.h": "tests/test_cover_facet_item_builder_boundary.py",
    "src/CoverCachedKeyItemBuilder.h": "tests/test_cover_cached_key_item_builder_boundary.py",
    "src/CoverWallPageResultBuilder.h": "tests/test_cover_wall_page_result_builder_boundary.py",
    "src/CoverWallFacetRefreshPolicy.h": "tests/test_cover_wall_facet_refresh_policy_boundary.py",
    "src/CoverWallCachePolicy.h": "tests/test_cover_wall_cache_policy_boundary.py",
    "src/CoverWallTextureMirrorPolicy.h": "tests/test_cover_wall_texture_mirror_policy_boundary.py",
    "src/CoverWallFailureCooldownPolicy.h": "tests/test_cover_wall_failure_cooldown_policy_boundary.py",
    "src/CoverWallVisibleMirrorAudit.h": "tests/test_cover_wall_visible_mirror_audit_boundary.py",
    "src/AsyncSearchResultFilterAdapter.h": "tests/test_async_search_result_filter_adapter_boundary.py",
    "src/AsyncSearchCoverPrimingRuntimeAdapter.h": "tests/test_async_search_cover_priming_runtime_adapter_boundary.py",
    "src/CoverWallRefreshStateAdapter.h": "tests/test_cover_wall_refresh_state_adapter_boundary.py",
    "src/PlaylistQuickBar.h": "tests/test_playlist_quick_bar_boundary.py",
    "src/SidebarActivityBar.h": "tests/test_sidebar_activity_bar_boundary.py",
    "src/FileInfoPanel.h": "tests/test_file_info_panel_boundary.py",
    "src/BuiltinThemeScheme.h": "tests/test_builtin_theme_scheme_boundary.py",
    "src/TransientScrollbarHost.h": "tests/test_playlist_quick_bar_boundary.py",
    "src/CoverKeyFilterWorkflow.h": "tests/test_cover_key_filter_workflow_boundary.py",
    "src/CoverKeyFilterStateAdapter.h": "tests/test_cover_key_filter_state_adapter_boundary.py",
    "src/PlaybackHistoryStateAdapter.h": "tests/test_playback_history_state_adapter_boundary.py",
    "src/WaveformDetailSchedulingStateAdapter.h": "tests/test_waveform_detail_scheduling_state_adapter_boundary.py",
    "src/WaveformViewResetStateAdapter.h": "tests/test_waveform_view_reset_state_adapter_boundary.py",
    "src/AsyncSearchFacetWarmStartPolicy.h": "tests/test_async_search_facet_warm_start_policy_boundary.py",
    "src/AsyncSearchCountFacetRepairSubmission.h": "tests/test_async_search_count_facet_repair_submission_boundary.py",
    "src/AsyncSearchBrowsePreviewWorkflow.h": "tests/test_async_search_browse_preview_boundary.py",
    "src/AsyncSearchBrowsePreviewRefreshWorkflow.h": "tests/test_async_search_browse_preview_refresh_boundary.py",
    "src/AsyncSearchBrowsePreviewRefreshStateAdapter.h": "tests/test_async_search_browse_preview_refresh_state_adapter_boundary.py",
    "src/AsyncSearchResultLifecycleWorkflow.h": "tests/test_async_search_result_lifecycle_boundary.py",
    "src/AsyncSearchResultLifecycleStateAdapter.h": "tests/test_async_search_result_lifecycle_state_adapter_boundary.py",
    "src/AsyncSearchResultDispatchWorkflow.h": "tests/test_async_search_result_dispatch_boundary.py",
    "src/AsyncSearchQueryTimingRegistry.h": "tests/test_async_search_query_timing_boundary.py",
    "src/AsyncSearchResultPumpWorkflow.h": "tests/test_async_search_result_pump_workflow_boundary.py",
    "src/PlaylistResultFilterWorkflow.h": "tests/test_playlist_result_filter_workflow_boundary.py",
    "src/AsyncSearchPlaylistContextWorkflow.h": "tests/test_async_search_playlist_context_boundary.py",
    "src/AsyncSearchPlaylistContextStateAdapter.h": "tests/test_async_search_playlist_context_state_adapter_boundary.py",
    "src/AsyncSearchPlaylistResultPreparationWorkflow.h": "tests/test_async_search_playlist_result_preparation_boundary.py",
    "src/AsyncSearchPlaylistResultPreparationStateAdapter.h": "tests/test_async_search_playlist_result_preparation_state_adapter_boundary.py",
    "src/HarkenSearchMetrics.h": "tests/test_stage7_platform_contract.py",
    "src/BackgroundIoPolicy.h": "tests/test_stage7_platform_contract.py",
    "src/CenterSenseDsp.h": "tests/test_stage7_platform_contract.py",
    "src/CoverBrowseSurfaceState.h": "tests/test_stage7_platform_contract.py",
    "src/DAWPanel.h": "tests/test_stage7_platform_contract.py",
    "src/DawBridgeInstaller.h": "tests/test_stage7_platform_contract.py",
    "src/EmbedScript.h": "tests/test_stage7_platform_contract.py",
    "src/FeatureManifest.h": "tests/test_stage7_platform_contract.py",
    "src/FileBrowser.h": "tests/test_stage7_platform_contract.py",
    "src/HostSendModel.h": "tests/test_stage7_platform_contract.py",
    "src/ModuleUI.h": "tests/test_stage7_platform_contract.py",
    "src/MoodAnalyzer.h": "tests/test_stage7_platform_contract.py",
    "src/NamingScript.h": "tests/test_stage7_platform_contract.py",
    "src/ReaperBridgeClient.h": "tests/test_stage7_platform_contract.py",
    "src/SearchTextIndex.h": "tests/test_stage7_platform_contract.py",
    "src/SendToTargetSurfaceModel.h": "tests/test_stage7_platform_contract.py",
    "src/SpectrogramMath.h": "tests/test_stage7_platform_contract.py",
    "src/TagSystem.h": "tests/test_stage7_platform_contract.py",
    "src/TagVocabularyMapper.h": "tests/test_stage7_platform_contract.py",
    "src/TimeStretch.h": "tests/test_stage7_platform_contract.py",
    "src/UIRenderer.h": "tests/test_stage7_platform_contract.py",
    "src/platform/database_audio_probe.h": "tests/test_stage7_platform_contract.py",
    "src/platform/emergency_file_writer.h": "tests/test_stage7_platform_contract.py",
    "src/platform/file_hash.h": "tests/test_stage7_platform_contract.py",
    "src/platform/image_codec.h": "tests/test_stage7_platform_contract.py",
    "src/platform/metadata_legacy_tag_content.h": "tests/test_stage7_platform_contract.py",
    "src/platform/pe_inspection.h": "tests/test_stage7_platform_contract.py",
    "src/WwiseDiff.h": "tests/test_stage7_platform_contract.py",
    "src/WwiseQuery.h": "tests/test_stage7_platform_contract.py",
    "src/WwiseTransferExecute.h": "tests/test_stage7_platform_contract.py",
    "src/WwiseTransferPanel.h": "tests/test_stage7_platform_contract.py",
    "src/platform/database_audio_probe_backend.h": "tests/test_stage7_platform_contract.py",
    "src/platform/database_file_dialog.h": "tests/test_stage7_platform_contract.py",
    "src/platform/executable_file_dialog.h": "tests/test_executable_file_dialog_boundary.py",
    "src/platform/avatar_image_file_dialog.h": "tests/test_avatar_image_file_dialog_boundary.py",
    "src/platform/reaper_file_list_dialog.h": "tests/test_reaper_file_list_dialog_boundary.py",
    "src/platform/thesaurus_json_dialog.h": "tests/test_thesaurus_json_dialog_boundary.py",
    "src/platform/settings_import_dialog.h": "tests/test_settings_import_dialog_boundary.py",
    "src/platform/cover_image_import_dialog.h": "tests/test_cover_image_import_dialog_boundary.py",
    "src/platform/region_wav_export_dialog.h": "tests/test_region_wav_export_dialog_boundary.py",
    "src/RecordRemovalWorkflow.h": "tests/test_record_removal_workflow_boundary.py",
    "src/RecordRenameWorkflow.h": "tests/test_record_rename_workflow_boundary.py",
    "src/BatchRenamePolicy.h": "tests/test_batch_rename_policy_boundary.py",
    "src/BatchRenameExecutionWorkflow.h": "tests/test_batch_rename_execution_workflow_boundary.py",
    "src/BrowserSystemFolderSearchPolicy.h": "tests/test_browser_system_folder_search_policy_boundary.py",
    "src/BrowserSystemFolderSearchRuntime.h": "tests/test_browser_system_folder_search_runtime_boundary.py",
    "src/FilePlayerDeferredDbWarmupRuntime.h": "tests/test_file_player_deferred_db_warmup_runtime_boundary.py",
    "src/FilePlayerDatabaseOpenWorkflow.h": "tests/test_file_player_database_open_workflow_boundary.py",
    "src/DatabaseSwitchWorkerRuntime.h": "tests/test_database_switch_worker_runtime_boundary.py",
    "src/DatabaseSwitchBlockingOperation.h": "tests/test_database_switch_blocking_operation_boundary.py",
    "src/DatabaseSessionMediaCacheReset.h": "tests/test_database_session_media_cache_reset_boundary.py",
    "src/WaveformRebuildRuntimeState.h": "tests/test_waveform_rebuild_runtime_state_boundary.py",
    "src/WaveformRebuildBatchSelector.h": "tests/test_waveform_rebuild_batch_selector_boundary.py",
    "src/WaveformRebuildFailurePersistence.h": "tests/test_waveform_rebuild_failure_persistence_boundary.py",
    "src/WaveformRebuildRequestSubmission.h": "tests/test_waveform_rebuild_request_submission_boundary.py",
    "src/WaveformRebuildRecordWorkflow.h": "tests/test_waveform_rebuild_record_workflow_boundary.py",
    "src/WaveformRebuildBatchLifecycle.h": "tests/test_waveform_rebuild_batch_lifecycle_boundary.py",
    "src/WaveformRebuildCancellationRecovery.h": "tests/test_waveform_rebuild_cancellation_recovery_boundary.py",
    "src/WaveformRebuildCancellationRecoveryStateAdapter.h": "tests/test_waveform_rebuild_cancellation_recovery_state_adapter_boundary.py",
    "src/AITranscriptionResultStateAdapter.h": "tests/test_ai_transcription_result_state_adapter_boundary.py",
    "src/ScanWaveformBackfillDrainWorkflow.h": "tests/test_scan_waveform_backfill_drain_workflow_boundary.py",
    "src/SelectedWaveformRebuildWorkflow.h": "tests/test_selected_waveform_rebuild_workflow_boundary.py",
    "src/SelectedRecordRescanWorkflow.h": "tests/test_selected_record_rescan_workflow_boundary.py",
    "src/BpmBatchScanWorkflow.h": "tests/test_bpm_batch_scan_workflow_boundary.py",
    "src/BpmBatchResultWorkflow.h": "tests/test_bpm_batch_result_workflow_boundary.py",
    "src/BpmBatchCompletionWorkflow.h": "tests/test_bpm_batch_completion_workflow_boundary.py",
    "src/CoverBatchOperationWorkflow.h": "tests/test_cover_batch_operation_workflow_boundary.py",
    "src/EmbedMetadataBatchRuntime.h": "tests/test_embed_metadata_batch_runtime_boundary.py",
    "src/CoverRebuildRecordProcessor.h": "tests/test_cover_rebuild_record_processor_boundary.py",
    "src/CoverRebuildLifecycleWorkflow.h": "tests/test_cover_rebuild_lifecycle_workflow_boundary.py",
    "src/CoverRebuildBatchCommitWorkflow.h": "tests/test_cover_rebuild_batch_commit_workflow_boundary.py",
    "src/LoadAllBatchRuntime.h": "tests/test_load_all_batch_runtime_boundary.py",
    "src/AsyncSearchLoadAllResultStateAdapter.h": "tests/test_async_search_load_all_result_state_adapter_boundary.py",
    "src/SameFolderScanWorkflow.h": "tests/test_same_folder_scan_workflow_boundary.py",
    "src/FileTypeVisibilityPolicy.h": "tests/test_file_type_visibility_policy_boundary.py",
    "src/SingleMusicAnalysisResultWorkflow.h": "tests/test_single_music_analysis_result_workflow_boundary.py",
    "src/AdaptiveMiniWaveformFormatFinalization.h": "tests/test_adaptive_mini_waveform_format_finalization_boundary.py",
    "src/AdaptiveMiniWaveformFormatPreparation.h": "tests/test_adaptive_mini_waveform_format_preparation_boundary.py",
    "src/AdaptiveMiniWaveformRebuildActivation.h": "tests/test_adaptive_mini_waveform_rebuild_activation_boundary.py",
    "src/WaveformRebuildPumpGate.h": "tests/test_waveform_rebuild_pump_gate_boundary.py",
    "src/OriginalAudioWriteConfirmGate.h": "tests/test_original_audio_write_confirm_gate_boundary.py",
    "src/DatabaseSessionScanStateReset.h": "tests/test_database_session_scan_state_reset_boundary.py",
    "src/DatabaseSessionViewStateReset.h": "tests/test_database_session_view_state_reset_boundary.py",
    "src/SameFolderScanRuntime.h": "tests/test_same_folder_scan_runtime_boundary.py",
    "src/platform/file_watcher.h": "tests/test_stage7_platform_contract.py",
    "src/platform/hmac_sha256.h": "tests/test_stage7_platform_contract.py",
    "src/UndoSystem.h": "tests/test_stage7_platform_contract.py",
    "src/update/UpdateManifest.h": "tests/test_update_manifest.cpp",
    "src/update/UpdateApplicationService.h": "tests/test_update_application_service.cpp",
    "src/update/UpdateCheckService.h": "tests/test_update_check_service.cpp",
    "src/update/UpdateCheckHttpsTransport.h": "tests/test_update_check_https_transport.cpp",
    "src/update/UpdateCheckTransport.h": "tests/test_update_check_service.cpp",
    "src/update/UpdateReleaseConfig.h": "tests/test_update_product_composition.py",
    "src/update/UpdateState.h": "tests/test_update_coordinator.cpp",
    "src/update/UpdateCoordinator.h": "tests/test_update_coordinator.cpp",
    "src/update/UpdateDownloadManager.h": "tests/test_update_download_manager.cpp",
    "src/update/UpdateHttpTransport.h": "tests/test_update_http_transport.cpp",
    "src/update/UpdateRuntime.h": "tests/test_update_runtime.cpp",
    "src/update/UpdateStateStore.h": "tests/test_update_state_store.cpp",
    "src/update/UpdatePersistencePort.h": "tests/test_update_persistence_port.cpp",
    "src/platform/update_persistence_port.h": "tests/test_update_persistence_port.cpp",
    "src/platform/update_persistence_port_test_hooks.h": "tests/test_update_persistence_port.cpp",
    "src/platform/update_http_transport.h": "tests/test_update_http_transport.cpp",
    "src/platform/update_http_transport_test_hooks.h": "tests/test_update_http_transport_platform.cpp",
    "src/VoiceAssetOrganizer.h": "tests/test_stage7_platform_contract.py",
    "src/WaveformBucketMath.h": "tests/test_stage7_platform_contract.py",
    "src/WaveformPresentationPolicy.h": "tests/test_stage7_platform_contract.py",
    "src/platform/file_prefix_probe.h": "tests/test_stage7_platform_contract.py",
    "src/platform/cover_discovery.h": "tests/test_stage7_platform_contract.py",
    "src/platform/qa_files.h": "tests/test_stage7_platform_contract.py",
    "src/platform/pro_tools_path_encoding.h": "tests/test_stage7_platform_contract.py",
    "src/platform/search_text_gb2312.h": "tests/test_stage7_platform_contract.py",
    "src/platform/platform.h": "tests/test_stage7_platform_contract.py",
    "src/platform/theme_json_dialogs.h": "tests/test_stage7_platform_contract.py",
    "src/AnnotationBridge.h": "tests/test_stage7_platform_contract.py",
    "src/HarkenSearchThemeManager.h": "tests/test_stage7_platform_contract.py",
    "src/FrameBudgetDiagnostics.h": "tests/test_stage7_platform_contract.py",
    "src/NativeFloatReader.h": "tests/test_stage7_platform_contract.py",
    "src/ProductEdition.h": "tests/test_stage7_platform_contract.py",
    "src/ProductModeRouting.h": "tests/test_stage7_platform_contract.py",
    "src/ReaperBridgeProtocol.h": "tests/test_stage7_platform_contract.py",
    "src/SearchIntent.h": "tests/test_stage7_platform_contract.py",
    "src/WaapiResponseParse.h": "tests/test_stage7_platform_contract.py",
    "src/WaveformPrewarmPolicy.h": "tests/test_stage7_platform_contract.py",
    "src/ScanInterruption.h": "tests/test_stage7_platform_contract.py",
    "src/ResultActionMenu.h": "tests/test_stage7_platform_contract.py",
    "src/SimilaritySearch.h": "tests/test_stage7_platform_contract.py",
    "src/SimilarityQueryService.h": "tests/test_stage7_platform_contract.py",
    "src/SimilarityWorkflow.h": "tests/test_stage7_platform_contract.py",
    "src/WaveformRegionEditor.h": "tests/test_stage7_platform_contract.py",
    "src/CenterSenseDataSource.h": "tests/test_stage7_platform_contract.py",
    "src/GaplessEngine.h": "tests/test_stage7_platform_contract.py",
    "src/MusicAnalyzer.h": "tests/test_stage7_platform_contract.py",
    "src/MusicQueue.h": "tests/test_stage7_platform_contract.py",
    "src/MetadataFields.h": "tests/test_stage7_platform_contract.py",
    "src/ThemeEditor.h": "tests/test_stage7_platform_contract.py",
    "src/UCSCategory.h": "tests/test_stage7_platform_contract.py",
    "src/WelcomeGreeting.h": "tests/test_stage7_platform_contract.py",
    "src/ExternalLibraryCover.h": "tests/test_stage7_platform_contract.py",
    "src/coverwall_lab/CoverWallLabState.h": "tests/test_stage7_platform_contract.py",
    "src/ReaperBridgeStatus.h": "tests/test_stage7_platform_contract.py",
    "src/ReaperBridgeVersion.h": "tests/test_stage7_platform_contract.py",
    "src/CubaseAdapter.h": "tests/test_stage7_platform_contract.py",
    "src/GMPanel.h": "tests/test_stage7_platform_contract.py",
    "src/HostSendRuntime.h": "tests/test_stage7_platform_contract.py",
    "src/WorkflowEngine.h": "tests/test_stage7_platform_contract.py",
    "src/ImGuiAcrylicFader.h": "tests/test_stage7_platform_contract.py",
    "src/ThemeMascot.h": "tests/test_stage7_platform_contract.py",
    "src/WorkerThreadPolicy.h": "tests/test_stage7_platform_contract.py",
    "src/AsyncSnapshotWorker.h": "tests/test_stage7_platform_contract.py",
    "src/AudioEnvironmentPolicy.h": "tests/test_stage7_platform_contract.py",
    "src/DbNorm.h": "tests/test_stage7_platform_contract.py",
    "src/SearchCommands.h": "tests/test_stage7_platform_contract.py",
    "src/SpotRequest.h": "tests/test_stage7_platform_contract.py",
    "src/FolderTreeContextActions.h": "tests/test_stage7_platform_contract.py",
    "src/MiniWaveformGeometry.h": "tests/test_stage7_platform_contract.py",
    "src/ScanWorkflow.h": "tests/test_stage7_platform_contract.py",
    "src/FloatingPanelLayout.h": "tests/test_stage6_main_thread_frame_budget_probe_contract.py",
    "src/TapTempoAccumulator.h": "tests/test_stage7_platform_contract.py",
    "src/MiniWaveformRender.h": "tests/test_stage7_platform_contract.py",
    "src/MiniWaveformScheduler.h": "tests/test_stage7_platform_contract.py",
    "src/PlaylistFolderTree.h": "tests/test_stage7_platform_contract.py",
    "src/PlaylistDisplayedFileCountPolicy.h": "tests/test_playlist_displayed_file_count_policy_boundary.py",
    "src/PlaylistQuickBarMotion.h": "tests/test_stage7_platform_contract.py",
    "src/SelectedWaveformRebuildQueue.h": "tests/test_stage7_platform_contract.py",
    "src/ThemeEditorSchema.h": "tests/test_stage7_platform_contract.py",
    "src/VoiceDeliveryAcceptance.h": "tests/test_stage7_platform_contract.py",
    "src/WaveformCompression.h": "tests/test_stage7_platform_contract.py",
    "src/platform/database_paths.inl": "tests/test_stage7_platform_contract.py",
    "src/platform/file_times.inl": "tests/test_stage7_platform_contract.py",
    "src/RenderFrameRequestPolicy.h": "tests/test_render_frame_request_policy.cpp",
    "src/UiHostServices.h": "tests/test_stage4_dependency_audit.py",
    "src/platform/explorer_shell_path.h": "tests/test_open_with.cpp",
    "src/SimilarSoundsWindow.h": "tests/test_similar_sounds_window_contract.py",
    "src/RelinkWorkflow.h": "tests/test_relink_workflow_boundary.py",
    "src/platform/audio_file_dialog.h": "tests/test_relink_workflow_boundary.py",
    "src/frontend/AppStateAuditionFrontendAdapter.h": "tests/test_audition_frontend_boundary.py",
    "src/frontend/AuditionFrontendContract.h": "tests/test_audition_frontend_boundary.py",
    "src/frontend/AuditionFrontendController.h": "tests/test_audition_frontend_boundary.py",
    "src/frontend/AuditionNativeIntentWorkflow.h": "tests/test_audition_frontend_boundary.py",
    "src/ui/AuditionTransportView.h": "tests/test_audition_frontend_boundary.py",
    "src/ui/CoreWorkspaceStyle.h": "tests/test_core_workspace_style_boundary.py",
    "src/ui/ModuleWindowShell.h": "tests/test_core_workspace_style_boundary.py",
}

EXCLUDED_HEADERS = {
    "src/EditorSendSurfaceModel.h": "unused product draft with no source consumer",
    "src/GuiDbManager.h": "empty legacy placeholder header",
    "src/Version.h": "unused legacy compile-time edition header",
    "src/ed25519/tweetnacl.h": "vendored third-party cryptography implementation boundary",
    "src/platform/file_prefix_probe_test_hooks.h": "compile-time test-only hook surface",
    "src/platform/filesystem_atomic_replace_test_hooks.h": "test-only filesystem backend seam",
    "src/platform/named_pipe_client_test_seam.h": "test-only named-pipe client backend seam",
    "src/platform/named_pipe_server_test_seam.h": "test-only named-pipe server backend seam",
    "src/webui/WebUiEmbedded.generated.h": "generated embedded Web UI artifact",
}


def _relative(root: Path, path: Path) -> str:
    return path.relative_to(root).as_posix()


def discover_headers(root: Path) -> list[Path]:
    """Return source headers in deterministic relative-path order."""
    src = root / "src"
    if not src.is_dir():
        return []
    paths = [path for path in src.rglob("*") if path.is_file() and path.suffix in HEADER_SUFFIXES]
    return sorted(paths, key=lambda path: _relative(root, path))


def _classification(relative_path: str) -> str:
    if relative_path.startswith("src/platform/"):
        return "platform"
    return "private"


def _source_files(root: Path) -> list[Path]:
    src = root / "src"
    if not src.is_dir():
        return []
    return sorted(
        (path for path in src.rglob("*") if path.is_file() and path.suffix in SOURCE_SUFFIXES),
        key=lambda path: _relative(root, path),
    )


def _consumer_map(root: Path, header_paths: set[str]) -> dict[str, list[str]]:
    by_name: dict[str, list[str]] = {}
    for relative_path in header_paths:
        by_name.setdefault(Path(relative_path).name, []).append(relative_path)
    consumers = {relative_path: set() for relative_path in header_paths}
    for source in _source_files(root):
        source_relative = _relative(root, source)
        for line in source.read_text(encoding="utf-8", errors="replace").splitlines():
            match = INCLUDE_RE.match(line)
            if not match:
                continue
            include = Path(match.group(1).replace("\\", "/"))
            candidates = []
            for base in (source.parent, root / "src"):
                try:
                    candidates.append((base / include).relative_to(root).as_posix())
                except ValueError:
                    pass
            target = next((candidate for candidate in candidates if candidate in header_paths), None)
            if target is None:
                matches = by_name.get(include.name, [])
                target = matches[0] if len(matches) == 1 else None
            if target and target != source_relative:
                consumers[target].add(source_relative)
    return {path: sorted(paths) for path, paths in consumers.items()}


def build_register(root: Path) -> dict[str, Any]:
    headers: list[dict[str, Any]] = []
    for path in discover_headers(root):
        relative_path = _relative(root, path)
        digest = hashlib.sha256(path.read_bytes()).hexdigest()
        if relative_path in REVIEWED_CONTRACTS:
            contract_fields = {
                "contract_state": "reviewed",
                "contract_evidence": REVIEWED_CONTRACTS[relative_path],
            }
        elif relative_path in EXCLUDED_HEADERS:
            contract_fields = {
                "contract_state": "excluded",
                "exclusion_reason": EXCLUDED_HEADERS[relative_path],
            }
        else:
            contract_fields = {"contract_state": "unreviewed"}
        headers.append(
            {
                "path": relative_path,
                "classification": _classification(relative_path),
                "sha256": digest,
                **contract_fields,
            }
        )
    consumers = _consumer_map(root, {item["path"] for item in headers})
    for item in headers:
        item["consumers"] = consumers[item["path"]]
        item["consumer_count"] = len(item["consumers"])
    register = {
        "schema_version": 1,
        "headers": headers,
    }
    register["include_cycles"] = find_public_include_cycles(root, register)
    return register


def find_include_cycles(graph: dict[str, set[str]]) -> list[list[str]]:
    """Return directed cycles in stable, canonical path order."""
    cycles: set[tuple[str, ...]] = set()
    visited: set[str] = set()

    def canonicalize(nodes: list[str]) -> tuple[str, ...]:
        ring = nodes[:-1]
        rotations = [ring[index:] + ring[:index] for index in range(len(ring))]
        chosen = min(rotations)
        return tuple(chosen + [chosen[0]])

    def visit(node: str, path: list[str]) -> None:
        if node in path:
            index = path.index(node)
            cycles.add(canonicalize(path[index:] + [node]))
            return
        if node in visited:
            return
        path.append(node)
        for target in sorted(graph.get(node, set())):
            visit(target, path)
        path.pop()
        visited.add(node)

    for node in sorted(graph):
        visit(node, [])
    return [list(cycle) for cycle in sorted(cycles)]


def _include_graph(root: Path, paths: set[str]) -> dict[str, set[str]]:
    graph = {path: set() for path in paths}
    for relative_path in sorted(paths):
        source = root / Path(relative_path)
        for line in source.read_text(encoding="utf-8", errors="replace").splitlines():
            match = INCLUDE_RE.match(line)
            if not match:
                continue
            include = Path(match.group(1).replace("\\", "/"))
            candidates = [
                (source.parent / include).relative_to(root).as_posix(),
                (root / "src" / include).relative_to(root).as_posix(),
            ]
            for candidate in candidates:
                if candidate in paths:
                    graph[relative_path].add(candidate)
                    break
    return graph


def find_public_include_cycles(root: Path, register: dict[str, Any]) -> list[list[str]]:
    paths = {
        item["path"]
        for item in register["headers"]
        if item["classification"] in {"public", "platform"}
    }
    return find_include_cycles(_include_graph(root, paths))


def write_register(root: Path, output: Path) -> None:
    register = build_register(root)
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(register, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path.cwd())
    parser.add_argument("--write", action="store_true", help="write the tracked Stage 7 JSON register")
    parser.add_argument("--check-cycles", action="store_true", help="fail when public-header cycles exist")
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()

    output = args.output or args.root / "docs/plans/refactor/stage-7-api-register.json"
    register = build_register(args.root)
    if args.check_cycles:
        if register["include_cycles"]:
            for cycle in register["include_cycles"]:
                print(" -> ".join(cycle))
            return 1
        print("Public header include cycles: 0")
        return 0
    if args.write:
        output.parent.mkdir(parents=True, exist_ok=True)
        output.write_text(json.dumps(register, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
        print(f"Wrote {output}")
    else:
        print(json.dumps(register, indent=2, ensure_ascii=False))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
