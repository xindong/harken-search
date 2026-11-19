#pragma once

#include <cstdint>

// @stage7-contract
// API contract: scan-stop disposition helpers are pure, synchronous enum
// predicates. They borrow no state and mutate nothing.

namespace HarkenSearch {

enum class ScanStopDisposition : std::uint8_t {
    None = 0,
    PauseKeepCommitted,
    CancelRollback,
    CancelKeepCommitted,
};

inline bool IsScanPause(ScanStopDisposition disposition) {
    return disposition == ScanStopDisposition::PauseKeepCommitted;
}

inline bool IsScanKeepCommitted(ScanStopDisposition disposition) {
    return disposition == ScanStopDisposition::PauseKeepCommitted ||
        disposition == ScanStopDisposition::CancelKeepCommitted;
}

inline bool IsScanCancel(ScanStopDisposition disposition) {
    return disposition == ScanStopDisposition::CancelRollback ||
        disposition == ScanStopDisposition::CancelKeepCommitted;
}

inline bool IsScanRollbackCancel(ScanStopDisposition disposition) {
    return disposition == ScanStopDisposition::CancelRollback;
}

// One decision for every stop path: a keep-committed pause and a keep-committed
// cancel must both survive, and only CancelRollback may discard committed rows.
struct ScanStopPlan {
    bool paused = false;
    bool cancel = false;
    bool rollback = false;
    bool keepCommitted = false;
};

inline ScanStopPlan PlanScanStop(ScanStopDisposition disposition) {
    ScanStopPlan plan;
    plan.paused = IsScanPause(disposition);
    plan.cancel = IsScanCancel(disposition);
    plan.rollback = IsScanRollbackCancel(disposition);
    plan.keepCommitted = IsScanKeepCommitted(disposition);
    return plan;
}

} // namespace HarkenSearch
