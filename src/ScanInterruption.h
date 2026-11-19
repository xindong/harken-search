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

} // namespace HarkenSearch
