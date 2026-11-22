#pragma once

#include "CubaseAdapter.h"
#include "DawSendFeedback.h"

#include <string_view>

namespace HarkenSearch::UI {

inline bool IsDawSendClipListFallback(std::string_view receiptCode) {
    return receiptCode == "clipListFallback" ||
        receiptCode == "clipListFallbackNoSelectedAudioTrack" ||
        receiptCode == "clipListFallbackNoTimelineTargetAudioTrack" ||
        receiptCode == "clipListFallbackAfterClipIdImportFailed" ||
        receiptCode == "clipListModernFallback" ||
        receiptCode == "clipListMidFallback" ||
        receiptCode == "clipListLegacyFallback";
}

inline void ApplySteinbergSendReceipt(const CubaseAdapter::DispatchResult& receipt,
                                     DawSendFeedbackSnapshot& feedback) {
    feedback.sent.reset();
    if (receipt.insertedFileCount) {
        if (*receipt.insertedFileCount > 0) feedback.sent = receipt.insertedFileCount;
    } else if (!receipt.insertedClipIds.empty()) {
        feedback.sent = static_cast<int>(receipt.insertedClipIds.size());
    }
    const bool partial = receipt.code == CubaseAdapter::ResultCode::PartialSuccess;
    if (!feedback.sent && (receipt.success || partial)) {
        feedback.outcome = DawSendOutcome::Failure;
        feedback.detail.setKey(LStr::DawSend_NoDeliveryConfirmation);
    } else if (partial) {
        feedback.outcome = DawSendOutcome::Partial;
    } else {
        feedback.outcome = ClassifyDawSendResult(receipt.success, feedback.sent, feedback.total);
    }
}

}
