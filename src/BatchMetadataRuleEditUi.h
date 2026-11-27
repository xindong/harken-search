#pragma once

#include "AppState.h"

// Entry points shared by the batch-metadata rule tab and the isolated preview
// QA command. Both act on the current selection; the tab calls them from its
// buttons, the QA command calls them directly to prove the same path.

namespace aura::ui {

void RefreshBatchRuleEditPreview(AppState& state);
bool ApplyBatchRuleEdit(AppState& state);

} // namespace aura::ui
