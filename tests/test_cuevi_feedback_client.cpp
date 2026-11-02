#include "CueviFeedbackClient.h"
#include "nlohmann_json.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <future>
#include <memory>
#include <string>
#include <thread>

namespace {

int failures = 0;

void Check(bool condition, const char* message) {
    if (condition) return;
    std::fprintf(stderr, "FAIL: %s\n", message);
    ++failures;
}

CueviFeedback::Submission BaseSubmission() {
    CueviFeedback::Submission input;
    input.projectId = "project-aris-github-xindong-aris-main";
    input.boardId = "board-airs";
    input.milestoneId = "milestone-airs";
    input.category = CueviFeedback::Category::Bug;
    input.source = CueviFeedback::Source::Manual;
    input.title = "Feedback title";
    input.description = "Feedback description";
    input.submittedAt = "2026-07-24T02:00:00.000Z";
    input.productVersion = "1.0.1";
    input.buildId = "test-build";
    input.releaseChannel = "test";
    input.environment = {{"os", "Windows 11"}, {"locale", "zh-CN"}};
    input.anonymous = true;
    input.submissionId = "7d562c42-6a43-42f1-9848-247f238cc954";
    input.idempotencyKey = "airs-feedback:" + input.submissionId;
    return input;
}

} // namespace

int main() {
    using namespace CueviFeedback;

    const auto anonymous = nlohmann::json::parse(BuildSubmissionJson(BaseSubmission()));
    Check(anonymous["category"] == "bug", "manual Bug maps to bug");
    Check(anonymous["source"] == "airs_feedback", "manual feedback maps to airs_feedback");
    Check(!anonymous.contains("reporter"), "anonymous feedback omits reporter");
    Check(!anonymous.contains("diagnosticLog"), "empty diagnostic log is omitted");

    auto named = BaseSubmission();
    named.anonymous = false;
    named.reporter = "Sound Designer";
    named.diagnosticLog = "diagnostic";
    named.category = Category::Requirement;
    const auto namedJson = nlohmann::json::parse(BuildSubmissionJson(named));
    Check(namedJson["category"] == "requirement", "feature suggestion maps to requirement");
    Check(namedJson["reporter"] == "Sound Designer", "named feedback includes reporter");
    Check(namedJson["diagnosticLog"] == "diagnostic", "checked diagnostic log is included");

    auto crash = BaseSubmission();
    crash.source = Source::CrashRecovery;
    const auto crashJson = nlohmann::json::parse(BuildSubmissionJson(crash));
    Check(crashJson["source"] == "crash_recovery", "crash report maps to crash_recovery");

    Check(ValidateSubmission(BaseSubmission()) == Error::None, "valid submission passes validation");
    auto invalid = BaseSubmission();
    invalid.title.clear();
    Check(ValidateSubmission(invalid) == Error::Validation, "empty title is rejected");
    invalid = BaseSubmission();
    invalid.title.assign(257, 'x');
    Check(ValidateSubmission(invalid) == Error::Validation, "title over 256 bytes is rejected");
    invalid = BaseSubmission();
    invalid.description.assign(8193, 'x');
    Check(ValidateSubmission(invalid) == Error::Validation, "description over 8 KiB is rejected");
    invalid = BaseSubmission();
    invalid.diagnosticLog.assign(65537, 'x');
    Check(ValidateSubmission(invalid) == Error::Validation, "diagnostic log over 64 KiB is rejected");
    invalid = BaseSubmission();
    invalid.environment["oversized"] = std::string(131072, 'x');
    Check(ValidateSubmission(invalid) == Error::Validation, "total request over 128 KiB is rejected");

    const auto bug = ParseResponse(201, R"({"workItemId":"BUG-20260724-01","category":"bug","stage":"backlog","replayed":false,"receivedAt":"2026-07-24T02:00:01.000Z"})", Category::Bug);
    Check(bug.ok && bug.workItemId == "BUG-20260724-01", "valid Bug response is accepted");
    const auto requirement = ParseResponse(200, R"({"workItemId":"RQ-20260724-02","category":"requirement","stage":"backlog","replayed":true,"receivedAt":"2026-07-24T02:00:02.000Z"})", Category::Requirement);
    Check(requirement.ok && requirement.replayed, "valid requirement replay is accepted");
    for (const char* body : {
             R"({"workItemId":""})",
             R"({"workItemId":"CV-1"})",
             R"({"workItemId":7})",
             R"({"workItemId":"BUG-20260724-01","category":"bug","stage":"backlog","receivedAt":"2026-07-24T02:00:01.000Z"})",
             R"({"workItemId":"BUG-20260724-01","category":"bug","stage":"backlog","replayed":false})",
             R"({"workItemId":"BUG-20260724-01","category":"bug","stage":"done","replayed":false,"receivedAt":"2026-07-24T02:00:01.000Z"})",
             R"({"workItemId":"BUG-20260724-01","category":"requirement","stage":"backlog","replayed":false,"receivedAt":"2026-07-24T02:00:01.000Z"})",
             R"({"workItemId":"RQ-20260724-01","category":"bug","stage":"backlog","replayed":false,"receivedAt":"2026-07-24T02:00:01.000Z"})",
             R"({"workItemId":"BUG-20260724-01","category":"bug","stage":"backlog","replayed":"false","receivedAt":"2026-07-24T02:00:01.000Z"})",
             R"({"workItemId":"BUG-20260724-01","category":"bug","stage":"backlog","replayed":false,"receivedAt":7})",
             R"({"workItemId":"BUG-20260724-01","category":"bug","stage":"backlog","replayed":false,"receivedAt":"not-a-time"})",
             R"({"workItemId":"BUG-20260724-01","category":"bug","stage":"backlog","replayed":false,"receivedAt":"2026-07-24T02:00:01.000Z","webUrl":false})"}) {
        const auto malformed = ParseResponse(200, body, Category::Bug);
        Check(!malformed.ok && malformed.error == Error::MalformedResponse,
              "malformed success response is rejected without throwing");
    }
    Check(ParseResponse(401, "{}", Category::Bug).error == Error::Unauthorized, "401 maps to Unauthorized");
    Check(ParseResponse(403, "{}", Category::Bug).error == Error::Unauthorized, "403 maps to Unauthorized");
    Check(ParseResponse(408, "{}", Category::Bug).error == Error::Timeout, "408 maps to Timeout");
    Check(ParseResponse(413, "{}", Category::Bug).error == Error::Validation, "413 maps to Validation");
    Check(ParseResponse(422, "{}", Category::Bug).error == Error::Validation, "422 maps to Validation");
    Check(ParseResponse(429, R"({"error":{"message":7}})", Category::Bug).error == Error::RateLimited,
          "429 maps to RateLimited even with malformed error detail");
    Check(ParseResponse(503, "{}", Category::Bug).error == Error::Server, "5xx maps to Server");

    std::string diagnosticLog;
    for (int i = 0; i < 200; ++i) {
        diagnosticLog += R"({"category":"Path","detail":")" + std::string(190, '\\') + R"(","type":0,"durationMs":0,"value":0,"timestampMs":1,"threadId":1,"repeat":1})" + "\n";
    }
    diagnosticLog += "tail-marker\n";
    for (Source source : {Source::Manual, Source::CrashRecovery}) {
        auto bounded = BaseSubmission();
        bounded.source = source;
        bounded.diagnosticLog = diagnosticLog;
        Check(FitDiagnosticLogToSubmissionLimits(bounded), "UI diagnostic log can be fitted to transport limits");
        Check(bounded.diagnosticLog.size() <= 65536, "fitted diagnostic log respects the 64 KiB field limit");
        Check(BuildSubmissionJson(bounded).size() <= 128 * 1024, "fitted diagnostic log respects the total payload limit");
        Check(bounded.diagnosticLog.find("tail-marker") != std::string::npos, "diagnostic truncation preserves the newest log tail");
        Check(ValidateSubmission(bounded) == Error::None, "fitted manual and crash submissions pass validation");
    }

    std::promise<void> enteredPromise;
    std::promise<void> releasePromise;
    auto entered = enteredPromise.get_future();
    auto release = releasePromise.get_future().share();
    const auto destroyStarted = std::chrono::steady_clock::now();
    {
        auto task = AsyncSubmission::Start([&enteredPromise, release]() mutable {
            enteredPromise.set_value();
            release.wait();
            Result result;
            result.error = Error::Timeout;
            return result;
        });
        Check(entered.wait_for(std::chrono::seconds(1)) == std::future_status::ready,
              "async feedback operation starts on a worker thread");
    }
    const auto destroyElapsed = std::chrono::steady_clock::now() - destroyStarted;
    Check(destroyElapsed < std::chrono::milliseconds(100),
          "destroying an in-flight feedback task never blocks the UI");
    releasePromise.set_value();

    constexpr int kExitStressIterations = 64;
    auto enteredCount = std::make_shared<std::atomic<int>>(0);
    auto finishedCount = std::make_shared<std::atomic<int>>(0);
    auto allEnteredPromise = std::make_shared<std::promise<void>>();
    auto allFinishedPromise = std::make_shared<std::promise<void>>();
    auto allEntered = allEnteredPromise->get_future();
    auto allFinished = allFinishedPromise->get_future();
    std::promise<void> stressReleasePromise;
    auto stressRelease = stressReleasePromise.get_future().share();
    auto longestDestroy = std::chrono::steady_clock::duration::zero();
    for (int i = 0; i < kExitStressIterations; ++i) {
        auto task = AsyncSubmission::Start(
            [enteredCount, finishedCount, allEnteredPromise, allFinishedPromise,
             stressRelease, kExitStressIterations]() mutable {
                if (enteredCount->fetch_add(1) + 1 == kExitStressIterations) {
                    allEnteredPromise->set_value();
                }
                stressRelease.wait();
                Result result;
                result.error = Error::Timeout;
                if (finishedCount->fetch_add(1) + 1 == kExitStressIterations) {
                    allFinishedPromise->set_value();
                }
                return result;
            });
        const auto destroyStarted = std::chrono::steady_clock::now();
        task = AsyncSubmission{};
        longestDestroy = std::max(longestDestroy, std::chrono::steady_clock::now() - destroyStarted);
    }
    Check(allEntered.wait_for(std::chrono::seconds(3)) == std::future_status::ready,
          "rapid exit stress starts every detached feedback worker");
    Check(longestDestroy < std::chrono::milliseconds(100),
          "rapidly abandoning feedback tasks never blocks the UI owner");
    stressReleasePromise.set_value();
    Check(allFinished.wait_for(std::chrono::seconds(3)) == std::future_status::ready,
          "detached feedback workers finish safely after their UI owner is gone");
    Check(finishedCount->load() == kExitStressIterations,
          "rapid exit stress completes every released feedback operation");

    const auto https = ParseEndpoint("https://cuevi.test/v1/feedback-submissions");
    Check(https && https->secure && https->host == L"cuevi.test" && https->path == L"/v1/feedback-submissions",
          "HTTPS endpoint is accepted");
    const auto localhost = ParseEndpoint("http://localhost:14317/v1/feedback-submissions");
    Check(localhost && !localhost->secure && localhost->host == L"localhost" && localhost->port == 14317,
          "localhost HTTP development endpoint is accepted");
    const auto loopback = ParseEndpoint("http://127.0.0.1:14317/v1/feedback-submissions");
    Check(loopback && !loopback->secure && loopback->host == L"127.0.0.1",
          "loopback HTTP development endpoint is accepted");
    for (const char* url : {
             "http://cuevi.example/v1/feedback-submissions",
             "http://localhost.evil.example/v1/feedback-submissions",
             "http://127.0.0.1.evil.example/v1/feedback-submissions",
             "https://user@cuevi.test/v1/feedback-submissions",
             "https://cuevi.test",
             "https://cuevi.test/v1/feedback-submissions#fragment",
             "https://cuevi.test:0/v1/feedback-submissions",
             "https://cuevi.test:999999999999999999999999/v1/feedback-submissions"}) {
        Check(!ParseEndpoint(url), "unsafe or malformed endpoint is rejected");
    }

    const auto id = GenerateSubmissionId();
    Check(IsUuid(id), "generated submission ID is an RFC 4122 version-4 UUID");

    Check(!IsUiEntryEnabled(Config{"", "key", 15000, "project-aris", "board-airs", "milestone-airs"}), "empty endpoint hides the feedback UI entry");
    Check(!IsUiEntryEnabled(Config{"https://cuevi.test/v1/feedback-submissions", "", 15000, "project-aris", "board-airs", "milestone-airs"}), "empty key hides the feedback UI entry");
    Check(!IsUiEntryEnabled(Config{"https://cuevi.test/v1/feedback-submissions", "restricted-key", 15000, "", "board-airs", "milestone-airs"}), "empty project id hides the feedback UI entry");
    Check(!IsUiEntryEnabled(Config{"https://cuevi.test/v1/feedback-submissions", "restricted-key", 15000, "project-aris", "", "milestone-airs"}), "empty board id hides the feedback UI entry");
    Check(!IsUiEntryEnabled(Config{"https://cuevi.test/v1/feedback-submissions", "restricted-key", 15000, "project-aris", "board-airs", ""}), "empty milestone id hides the feedback UI entry");
    Check(!IsUiEntryEnabled(Config{"not-a-url", "restricted-key", 15000, "project-aris", "board-airs", "milestone-airs"}), "unparseable endpoint hides the feedback UI entry");
    Check(!IsUiEntryEnabled(Config{"", "", 15000, "", "", ""}), "empty endpoint and key hide the feedback UI entry");
    Check(IsUiEntryEnabled(Config{"https://cuevi.test/v1/feedback-submissions", "restricted-key", 15000, "project-aris", "board-airs", "milestone-airs"}), "configured endpoint, key, and routing keep the feedback UI entry enabled");

    if (failures != 0) return 1;
    std::printf("PASS: Cuevi feedback payload, validation, response, endpoint, async, and UUID contracts\n");
    return 0;
}
