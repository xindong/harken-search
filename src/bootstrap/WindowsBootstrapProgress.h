#pragma once

#include "bootstrap/BootstrapProgress.h"
#include <memory>

namespace aris::bootstrap {

// @stage7-contract
// API contract: the factory returns an owned optional presentation window;
// null or native creation failure leaves the updater's decisions unchanged.
// Report copies snapshots from one update thread to its own UI thread. Finish
// is called once after the update ends and joins the UI thread (waiting for
// dismissal on failure). Destruction closes only this window, never the update
// process. The caller's desktop is retained; the input desktop is not changed.

class BootstrapProgressWindow : public BootstrapProgressSink {
public:
    // Called only after the update has returned. Failure stays visible until
    // dismissed; success closes automatically. False preserves legacy errors.
    virtual bool Finish(int exitCode) = 0;
};

// Uses the caller's desktop, never switches the user's input desktop.
// forceReducedMotion supplements (never overrides) Windows accessibility.
std::unique_ptr<BootstrapProgressWindow> CreateBootstrapProgressWindow(
    bool forceReducedMotion = false, bool allowWebView = true) noexcept;

}  // namespace aris::bootstrap
