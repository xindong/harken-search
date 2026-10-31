#pragma once
// SoundManager - 新模块 UI 集成
// ModuleUI.h - 各模块对话框/面板的 UI 渲染接口
//
// 每个模块对应一个 Render 函数，在 RenderUI() 中调用。

#include <string>
#include "MiniWaveformPresentation.h"

struct AppState;

// @stage7-contract
// API contract: module UI functions run on the UI thread during the documented
// frame/menu phases, consume AppState by reference without taking ownership,
// and do not retain transient ImGui or state references after return.

// 模块初始化 (在 WinMain 中调用一次)

void InitializeModules();

// 菜单栏 "工具" 菜单绘制 (在菜单栏循环中调用)

void RenderToolsMenu(AppState& state);

// 模块对话框渲染 (在 RenderUI 尾部调用)

// 渲染所有已打开的模块对话框/窗口
void RenderModuleDialogs(AppState& state);

// 工作区功能窗口的正文渲染入口；外层宿主由主工作区统一提供圆角外壳、
// 标题拖拽、关闭和内边距，正文本身不创建 ImGui 顶层窗口。
void RenderUCSBrowserContent(AppState& state);
void RenderTagBrowserContent(AppState& state);
void RenderTransferDialogContent(AppState& state);
void RenderWorkflowDialogContent(AppState& state);

// Release CPU waveform data; explicit invalidation also releases the texture.
void ReleaseWaveformEvictedEntry(AppState& state, const std::string& key,
    MiniWaveformPresentation::Removal removal =
        MiniWaveformPresentation::Removal::ExplicitInvalidation);
void InvalidateMiniWaveTextureForPath(const std::string& key);
void InvalidateMiniWaveTextureAfterFrame(const std::string& key);
void FlushDeferredMiniWaveTextureDeletes();
