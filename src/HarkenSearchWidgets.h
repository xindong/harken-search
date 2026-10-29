// src/HarkenSearchWidgets.h
// Aris UI v1 - public widget API. Every window-level control in Aris must use
// these functions instead of calling ImGui primitives directly. All colors
// resolve through ThemeColors::current(); all sizes through shared metrics.
#pragma once

#include "imgui.h"
#include <string>
#include <vector>
#include <functional>

namespace aura::ui {

// @stage7-contract
// API contract: widget helpers are UI-thread drawing calls. They consume caller
// values during the frame, do not retain transient string storage, and must use
// the shared theme/metric tokens rather than direct platform state.

namespace focus {
    // Call once per frame, before any widgets are drawn.
    void UpdateFocusTracker();
    // Used by widget internals; returns true if keyboard/nav drove the last input.
    bool IsKeyboardInputActive();
}

// Draws a focus ring around the last submitted ImGui item when keyboard
// focus is on it. Safe to call unconditionally.
void DrawFocusRing();

// Button (Task 7)

enum class ButtonVariant {
    PrimarySearch,   // orange - local query / processing (Search, Scan, Rebuild)
    PrimaryTransfer, // green  - outbound send / export (Transfer, Send, Export)
    Secondary,       // neutral fill (Save, Rename, Apply)
    Ghost,           // transparent background, label only
    Danger,          // destructive (Delete, Clear, Remove)
    Icon,            // icon-only, transparent
    Quiet            // subdued ordinary action (Cancel, Close, Exit)
};
enum class ButtonSize { Sm, Md, Lg };

struct ButtonOptions {
    ButtonVariant variant  = ButtonVariant::Secondary;
    ButtonSize    size     = ButtonSize::Md;
    const char*   icon     = nullptr;  // Lucide glyph UTF-8 string, optional
    bool          loading  = false;
    bool          disabled = false;
    const char*   tooltip  = nullptr;
};

// Returns true on click. Renders focus ring automatically.
bool Button(const char* label, const ButtonOptions& opts = {});

// Ordinary action with the caller's existing layout: zero = auto, negative =
// fill the available width, positive = exact pixels. Uses the same appearance
// as Button without imposing ButtonSize padding. Not for tabs or graphic tools.
bool ActionButton(const char* label, const ImVec2& size = ImVec2(0, 0),
                  ButtonVariant variant = ButtonVariant::Secondary);
// Inline text action: retains SmallButton's compact baseline and dimensions.
bool SmallActionButton(const char* label, ButtonVariant variant = ButtonVariant::Secondary);

// Icon-only convenience wrapper. `icon` is a Lucide glyph string.
bool IconButton(const char* icon, const char* tooltip,
                ButtonSize size = ButtonSize::Sm);

// TextInput / NumberInput (Task 8)

enum class TextInputVariant { Text, Multiline, Number, Password, Path };

struct TextInputOptions {
    TextInputVariant variant         = TextInputVariant::Text;
    const char*      placeholder     = nullptr;
    const char*      leadingIcon     = nullptr;  // Lucide glyph, optional
    const char*      trailingIcon    = nullptr;  // Lucide glyph, optional
    const char*      trailingTooltip = nullptr;
    bool             disabled        = false;
    bool             readonly        = false;
    const char*      errorText       = nullptr;  // non-null = invalid state
    int              maxLength       = 0;        // 0 = unlimited
    int              multilineRows   = 3;        // only for Multiline
    float            width           = 0.0f;     // >0 = explicit pixel width; 0 = fill ContentRegionAvail
    ImGuiInputTextFlags inputFlags    = 0;        // additional ImGui input behavior (e.g. EnterReturnsTrue)
    bool inheritContextColors = false;            // honor an explicitly scoped surface palette
};

// Returns true if `value` changed. If `trailingClicked` is non-null and
// the trailing icon button was pressed this frame, sets *trailingClicked = true.
bool TextInput(const char* label, std::string* value,
               const TextInputOptions& opts = {},
               bool* trailingClicked = nullptr);

bool NumberInput(const char* label, float* value, float min, float max,
                 float step = 1.0f, const char* format = "%.2f",
                 const TextInputOptions& opts = {});

// Checkbox / Radio / Toggle (Task 9)

bool Checkbox(const char* label, bool* value, bool disabled = false);
bool Radio(const char* label, int* currentValue, int thisValue, bool disabled = false);
bool Toggle(const char* label, bool* value, bool disabled = false);

// Slider (Task 10)

bool SliderFloat(const char* label, float* value, float min, float max,
                 const char* format = "%.2f",
                 bool showValueInline = true);

bool SliderRange(const char* label, float* minVal, float* maxVal,
                 float min, float max, const char* format = "%.2f");

// Combo / ComboMulti (Task 11)

enum class ComboVariant { Default, MultiSelect, Searchable };

// items[] must have at least itemCount entries. Returns true on selection change.
bool Combo(const char* label, int* currentIndex,
           const char* const items[], int itemCount,
           ComboVariant variant = ComboVariant::Default);

// selections is resized to itemCount if smaller. Returns true on any toggle.
bool ComboMulti(const char* label, std::vector<bool>* selections,
                const char* const items[], int itemCount);

// Tooltip / Popup (Task 12)

// Themed tooltip for simple text. Call after IsItemHovered() at the call site.
void Tooltip(const char* text);

struct SettingsRowOptions {
    const char* tooltip = nullptr;
    bool disabled = false;
};

bool BeginSettingsGroup(const char* id);
void EndSettingsGroup();
bool SettingsToggleRow(const char* label, bool* value,
                       const SettingsRowOptions& opts = {});
// enabled==nullptr: label + slider + click-to-edit number on one row, no toggle.
bool SettingsSliderRow(const char* label, bool* enabled, int* value,
                       int min, int max, const char* valueFormat,
                       const SettingsRowOptions& opts = {});
bool SettingsSliderRow(const char* label, bool* enabled, float* value,
                       float min, float max, const char* valueFormat,
                       const SettingsRowOptions& opts = {});
bool SettingsButtonRow(const char* label, const ImVec2& size,
                       const SettingsRowOptions& opts = {});

enum class SettingsSearchLabelKind {
    Text,
    Check,
    Button,
    Separator
};

void BeginSettingsSearchHighlight(const char* query, const char* activeLabel,
                                  const char* keywords = nullptr);
void EndSettingsSearchHighlight();
void NotifySettingsSearchLabeledItem(const char* label,
                                     SettingsSearchLabelKind kind = SettingsSearchLabelKind::Text,
                                     const ImVec2* textOrigin = nullptr);
void DrawSettingsSearchableLabel(const char* label);
bool TakeSettingsSearchHitLayout(ImVec2* outMin, ImVec2* outMax);

// Themed tooltip with arbitrary content. `body` renders inside the tooltip.
void TooltipRich(const std::function<void()>& body);

// Themed popup wrapper. Pair with ImGui::OpenPopup(id). Returns true if open.
bool BeginPopup(const char* id);
void EndPopup();

// Standalone window API.

enum class StandaloneWindowAction { None, Minimize, ToggleMaximize, Close };

struct StandaloneWindowState {
    bool open = false;
    bool minimized = false;
    bool maximized = false;
    bool requestFocus = false;
    bool requestMove = false;
    ImVec2 restorePos{0.0f, 0.0f};
    ImVec2 restoreSize{0.0f, 0.0f};
};

struct StandaloneWindowOptions {
    ImVec2 defaultSize{1040.0f, 514.0f};
    ImVec2 minimumSize{720.0f, 360.0f};
};

void OpenStandaloneWindow(StandaloneWindowState* state);
bool BeginStandaloneWindow(const char* title, const char* stableId,
                           StandaloneWindowState* state,
                           const StandaloneWindowOptions& options = {});
void EndStandaloneWindow();

// Modal dialog API.

// Themed modal dialog. Pass `open` pointing at a bool; set to true to
// request opening this frame. Returns true if the dialog is currently
// rendering; caller pairs with EndDialog() when true. size {0,0} = auto.
bool BeginDialog(const char* title, bool* open,
                 float width = 480.0f, float height = 0.0f);
void EndDialog();

// Panel / SectionHeader / Divider (Task 14)

enum class PanelVariant { Peer, Surface, Inset };
enum class DividerVariant { Subtle, Strong, Inset };

// Panel is a themed child region. Pair with EndPanel() when this returns true.
bool BeginPanel(const char* id, PanelVariant variant,
                float width = 0.0f, float height = 0.0f,
                ImGuiWindowFlags wflags = 0);
void EndPanel();

// Section header with optional collapse chevron and optional trailing
// renderer (e.g. an IconButton). Returns true if content should render.
bool SectionHeader(const char* title, bool* expanded = nullptr,
                   const std::function<void()>& trailing = {});

void Divider(DividerVariant v = DividerVariant::Subtle);

// Workflow feedback controls.

enum class ScanJobStatus { Queued, Running, Paused, Complete, Failed };

struct ScanJobCardOptions {
    const char* title = nullptr;
    const char* detail = nullptr;
    ScanJobStatus status = ScanJobStatus::Queued;
    float progress = 0.0f;
    int current = 0;
    int total = 0;
    float itemsPerSecond = 0.0f;
};

void ScanJobCard(const char* id, const ScanJobCardOptions& options);

enum class RecoveryBannerIntent { Info, Warning, Danger };

struct RecoveryBannerOptions {
    RecoveryBannerIntent intent = RecoveryBannerIntent::Info;
    const char* title = nullptr;
    const char* message = nullptr;
    bool loading = false;
    float height = 56.0f;
    float actionWidth = 280.0f;
};

void RecoveryBanner(const char* id, const RecoveryBannerOptions& options,
                    const std::function<void()>& actions = {});

}
