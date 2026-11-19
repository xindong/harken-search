// src/HarkenSearchMetrics.h
// Aris UI v1 foundations - Layer 0 sizing/spacing/radius/icon constants.
// Every shared widget reads layout values from this header; if you need a
// new value, add a named constant here - never embed magic numbers at the
// call site.
#pragma once

#include "HarkenSearchDesignTokens.generated.h"

namespace aura::ui::metrics {

// @stage7-contract
// API contract: metrics are immutable UI token values in pixels/time units.
// Widgets consume them on the UI thread; changing a token is a deliberate
// visual-contract change and must not be inferred at call sites.

// Control heights (pixels)
constexpr float kHeightSm = aris::design::kControlHeightSm;   // icon buttons, inline micro-actions
constexpr float kHeightMd = aris::design::kControlHeightMd;   // default for all inputs/buttons
constexpr float kHeightLg = aris::design::kControlHeightLg;   // hero CTA, transport, key dialogs

// Horizontal padding inside controls (pixels)
constexpr float kPadXSm = aris::design::kControlPaddingXSm;
constexpr float kPadXMd = aris::design::kControlPaddingXMd;
constexpr float kPadXLg = aris::design::kControlPaddingXLg;

// Vertical padding (usually (height - content) / 2)
constexpr float kPadYSm = aris::design::kControlPaddingYSm;
constexpr float kPadYMd = aris::design::kControlPaddingYMd;
constexpr float kPadYLg = aris::design::kControlPaddingYLg;

// Gaps between items (pixels)
constexpr float kGap4  = aris::design::kSpace1;  // icon+label tight pairing
constexpr float kGap8  = aris::design::kSpace2;  // default item spacing
constexpr float kGap12 = aris::design::kSpace3;  // section members
constexpr float kGap16 = aris::design::kSpace4;  // sibling panels
constexpr float kGap24 = aris::design::kSpace6;  // dialog blocks

// Border radius (pixels)
constexpr float kRadiusTight   = aris::design::kRadiusTight;    // table cells, dense controls
constexpr float kRadiusDefault = aris::design::kRadiusControl;  // buttons, inputs, combos (baseline)
constexpr float kRadiusPanel   = aris::design::kRadiusPanel;    // cards, panels
constexpr float kRadiusCapsule = aris::design::kRadiusCapsule;  // capsule pills, RoseCedar frames
constexpr float kRadiusDialog  = 12.0f;                         // macOS-style alert panels

// Icon sizes (square, pixels)
constexpr float kIconXs = aris::design::kIconSizeXs;
constexpr float kIconSm = aris::design::kIconSizeSm;
constexpr float kIconMd = aris::design::kIconSizeMd;  // default
constexpr float kIconLg = aris::design::kIconSizeLg;
constexpr float kIconXl = aris::design::kIconSizeXl;  // hero buttons, splash

// Focus ring (see DrawFocusRing)
constexpr float kFocusRingWidth = aris::design::kFocusRingWidth;
constexpr float kFocusRingGap   = aris::design::kFocusRingGap;
constexpr float kFocusRingAlpha = aris::design::kFocusRingAlpha;

// Text size identifiers (indexed into font stack)
enum class TextSize { Label, Body, Heading, Display };
constexpr float kFontPxLabel   = 11.0f;
constexpr float kFontPxBody    = 13.0f;
constexpr float kFontPxHeading = 15.0f;
constexpr float kFontPxDisplay = 18.0f;

}
