#include "DawSendNotice.inl"
#include "DawSendDeliveryFeedback.h"

#include <cstdio>
#include <stdexcept>
#include <thread>

ThemeColorScheme& ThemeColors::current() {
    static ThemeColorScheme theme;
    return theme;
}

const ImVec4& ThemeColors::popupMenuBG(const ThemeColorScheme& theme) {
    return theme.transportBarBG;
}

ImVec4 ThemeColors::popupBorder(const ThemeColorScheme& theme) {
    return theme.resultsOutline;
}

using namespace HarkenSearch::UI;

static int failures = 0;

static void Check(bool condition, const char* message) {
    if (condition) return;
    std::fprintf(stderr, "%s\n", message);
    ++failures;
}

static void CheckSendLifecycle() {
    DawSendFeedback feedback;
    Check(feedback.begin(4), "first send must start");
    const auto sending = feedback.snapshot();
    Check(sending.outcome == DawSendOutcome::Sending && sending.total == 4 && !sending.sent,
          "unreported progress must stay unknown");
    Check(!feedback.begin(9), "re-entry must not start another send");
    feedback.failBeforeSend("new prep error must not replace in-flight feedback");
    Check(feedback.snapshot().revision == sending.revision, "in-flight feedback was overwritten");

    std::thread worker([&feedback] { feedback.setProgress(2, 4); });
    worker.join();
    const auto progress = feedback.snapshot();
    Check(progress.outcome == DawSendOutcome::Sending && progress.sent == 2 && progress.total == 4,
          "worker progress did not publish a consistent count");

    DawSendFeedbackSnapshot result;
    result.outcome = ClassifyDawSendResult(false, 2, 4);
    result.sent = 2;
    result.detail = "host result";
    feedback.finish(std::move(result));
    const auto completed = feedback.snapshot();
    Check(completed.outcome == DawSendOutcome::Partial && completed.sent == 2 && completed.total == 4 &&
          completed.changedAt >= progress.changedAt && completed.revision != progress.revision,
          "completion must atomically publish outcome, count and timestamp");
    Check(feedback.begin(1), "next send must start even while the previous notice was visible");
    feedback.expire(completed.revision);
    const auto next = feedback.snapshot();
    Check(next.outcome == DawSendOutcome::Sending && !next.sent && next.total == 1 && next.detail.empty(),
          "next send retained stale data or was cleared by the old notice");
    Check(ClassifyDawSendResult(true, 1, 1) == DawSendOutcome::Success, "confirmed delivery is not success");
    Check(ClassifyDawSendResult(false, std::nullopt, 1) == DawSendOutcome::Failure,
          "unknown progress must not manufacture success or partial delivery");
}

static void CheckWorkerException(bool standardException) {
    DawSendFeedback feedback;
    Check(feedback.begin(3), "exception fixture could not start");
    std::string error;
    auto worker = MakeDawSendWorker(feedback, [&]() -> DawSendFeedbackSnapshot {
        feedback.setProgress(1, 3);
        if (standardException) throw std::runtime_error("mock dispatch I/O failure");
        throw 42;
    }, [&](const char* message) {
        error = message;
        throw std::runtime_error("mock diagnostic failure");
    });
    bool escaped = false;
    try {
        worker();
    } catch (...) {
        escaped = true;
    }
    const auto failure = feedback.snapshot();
    Check(!escaped, "an exception escaped the DAW worker");
    Check(failure.outcome == DawSendOutcome::Failure && failure.total == 3 && !failure.sent && failure.detail.empty(),
          "worker exception left sending or stale delivery data");
    Check(error == (standardException ? "mock dispatch I/O failure" : "unknown exception"),
          "worker exception did not reach diagnostics");
    Check(feedback.begin(1), "send after worker exception was blocked");
}

static void CheckDeliveryReceipts() {
    namespace Steinberg = HarkenSearch::CubaseAdapter;
    for (const std::string_view code : {"clipListFallback", "clipListFallbackNoSelectedAudioTrack",
             "clipListFallbackNoTimelineTargetAudioTrack", "clipListFallbackAfterClipIdImportFailed",
             "clipListModernFallback", "clipListMidFallback", "clipListLegacyFallback"}) {
        Check(IsDawSendClipListFallback(code), std::string(code).c_str());
    }
    Check(!IsDawSendClipListFallback("Clip List"), "display text was mistaken for a fallback receipt");
    Check(!IsDawSendClipListFallback("clipListSomethingUnknown"), "unknown receipt was treated as delivery");

    const auto classify = [](const Steinberg::DispatchResult& result, int total) {
        DawSendFeedbackSnapshot feedback;
        feedback.total = total;
        ApplySteinbergSendReceipt(result, feedback);
        return feedback;
    };
    Steinberg::DispatchResult receipt;
    receipt.success = true;
    receipt.code = Steinberg::ResultCode::Success;
    Check(classify(receipt, 3).outcome == DawSendOutcome::Failure,
          "bare OK was presented as confirmed delivery");
    receipt.insertedFileCount = 3;
    const auto counted = classify(receipt, 3);
    Check(counted.outcome == DawSendOutcome::Success && counted.sent == 3,
          "a reported file count was ignored when clip IDs were absent");
    receipt.insertedFileCount = 2;
    receipt.insertedClipIds = {"clip-a"};
    const auto shortCount = classify(receipt, 3);
    Check(shortCount.outcome == DawSendOutcome::Partial && shortCount.sent == 2,
          "reported file count did not take precedence over clip IDs or show partial delivery");
    receipt.insertedFileCount = 0;
    receipt.insertedClipIds = {"clip-a", "clip-b", "clip-c"};
    const auto zeroWithIds = classify(receipt, 3);
    Check(zeroWithIds.outcome == DawSendOutcome::Failure && !zeroWithIds.sent && !zeroWithIds.detail.empty(),
          "an explicit zero was overridden by clip IDs");
    receipt.code = Steinberg::ResultCode::PartialSuccess;
    const auto zeroPartial = classify(receipt, 3);
    Check(zeroPartial.outcome == DawSendOutcome::Failure && !zeroPartial.sent && !zeroPartial.detail.empty(),
          "a partial receipt without confirmed audio was reported as delivered");
    receipt.code = Steinberg::ResultCode::Success;
    receipt.insertedFileCount.reset();
    const auto identified = classify(receipt, 3);
    Check(identified.outcome == DawSendOutcome::Success && identified.sent == 3,
          "clip IDs did not confirm delivery when the legacy receipt had no count");
    receipt.code = Steinberg::ResultCode::PartialSuccess;
    Check(classify(receipt, 3).outcome == DawSendOutcome::Partial,
          "explicit partial receipt was promoted to full success");
}

static void CheckNoticeLayout() {
    ImGui::CreateContext();
    auto& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.DisplaySize = ImVec2(640, 420);
    unsigned char* pixels = nullptr;
    int atlasWidth = 0;
    int atlasHeight = 0;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &atlasWidth, &atlasHeight);
    ImGui::NewFrame();
    ImGui::Begin("DAW notice fixture");
    ImGui::Button("Existing send button");
    auto* viewport = ImGui::GetWindowViewport();
    auto* windowDrawList = ImGui::GetWindowDrawList();
    const auto windowVertices = windowDrawList->VtxBuffer.Size;
    const auto lastItem = GImGui->LastItemData.ID;
    const auto navWindow = GImGui->NavWindow;
    const ImRect anchor(ImVec2(350.0f, 375.0f), ImVec2(510.0f, 403.0f));

    std::string longUtf8;
    for (int index = 0; index < 180; ++index) longUtf8 += u8"路径音轨É";
    auto& font = *ImGui::GetFont();
    const float fontSize = ImGui::GetFontSize();
    const auto wrapped = WrapDawSendNoticeText(longUtf8, font, fontSize, 100.0f);
    std::string reassembled;
    for (const auto line : wrapped) {
        const char* end = line.data() + line.size();
        for (const char* cursor = line.data(); cursor < end;) {
            unsigned int codepoint = 0;
            const int consumed = ImTextCharFromUtf8(&codepoint, cursor, end);
            Check(codepoint != IM_UNICODE_CODEPOINT_INVALID, "wrap split a UTF-8 code point");
            cursor += consumed;
        }
        reassembled += line;
        Check(font.CalcTextSizeA(fontSize, 1e9f, 0.0f, line.data(), end).x <= 101.0f,
              "a long-token line exceeded the wrap width");
    }
    Check(wrapped.size() > 2 && reassembled == longUtf8, "wrapping lost part of a long token");
    const std::string mixed = u8"REAPER 桥接未建立连接请检查组件版本以及重新启动宿主后重试" + longUtf8;
    const auto mixedLines = WrapDawSendNoticeText(mixed, font, fontSize, 240.0f - fontSize / 14.0f * 36.0f);
    Check(mixedLines.size() > 1 && mixedLines.front().find(u8"桥接") != std::string_view::npos,
          "a 240px notice stranded REAPER alone instead of using the line for Chinese characters");
    std::string mixedReassembled;
    for (const auto line : mixedLines) mixedReassembled += line;
    Check(mixedReassembled == mixed, "mixed-script wrapping lost bytes");

    DawSendFeedbackSnapshot feedback;
    feedback.detail.setKeyStr(LStr::DBst_HandFailFmt, longUtf8);
    feedback.remediation = u8"最后一行：重新启动并重试";
    Check(feedback.detail.Render().find(longUtf8) != std::string::npos, "ledger interpolation truncated the keyed detail");
    for (const auto outcome : {DawSendOutcome::Sending, DawSendOutcome::Success, DawSendOutcome::Partial,
                               DawSendOutcome::Fallback, DawSendOutcome::Failure, DawSendOutcome::BridgeFailure}) {
        feedback.outcome = outcome;
        const auto foregroundVertices = ImGui::GetForegroundDrawList(viewport)->VtxBuffer.Size;
        const auto first = DrawDawSendNotice(feedback, viewport, anchor, 0.0, 160.0f, 1);
        Check(first.rendered && first.viewportId == viewport->ID &&
              first.rect.Min.x >= viewport->WorkPos.x && first.rect.Max.x <= anchor.Max.x &&
              first.rect.Min.y >= viewport->WorkPos.y && first.rect.Max.y < anchor.Min.y &&
              first.rect.GetWidth() <= 160.0f && first.wrapWidth < first.rect.GetWidth(),
              "foreground notice escaped the viewport or covered the send button");
        Check(first.overflow && first.page == 1 && first.pageCount > 1 && !first.degraded,
              "oversized feedback did not create readable pages");
        Check(ImGui::GetForegroundDrawList(viewport)->VtxBuffer.Size > foregroundVertices &&
              windowDrawList->VtxBuffer.Size == windowVertices &&
              GImGui->LastItemData.ID == lastItem && GImGui->NavWindow == navWindow,
              "notice altered the host draw list or focus instead of drawing on the foreground");
        size_t seenLines = first.visibleBodyLines;
        for (size_t page = 2; page <= first.pageCount; ++page) {
            const auto current = DrawDawSendNotice(feedback, viewport, anchor, 0.0, 160.0f, page);
            Check(current.rendered && current.page == page && current.pageCount == first.pageCount,
                  "a page of feedback could not be displayed");
            seenLines += current.visibleBodyLines;
        }
        Check(seenLines == first.bodyLines, "pagination skipped or duplicated wrapped body lines");
    }
    const auto expired = DrawDawSendNotice(feedback, viewport, anchor, 1e6, 160.0f);
    Check(expired.expired && !expired.rendered, "completed notice must expire");
    feedback.outcome = DawSendOutcome::Sending;
    Check(DrawDawSendNotice(feedback, viewport, anchor, 1e6, 160.0f).rendered,
          "in-flight feedback must not expire");
    ImGui::End();
    ImGui::Render();
    ImGui::DestroyContext();
}

int main() {
    CheckSendLifecycle();
    CheckWorkerException(true);
    CheckWorkerException(false);
    CheckDeliveryReceipts();
    CheckNoticeLayout();
    return failures == 0 ? 0 : 1;
}
