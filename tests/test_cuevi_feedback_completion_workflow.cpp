#include "CueviFeedbackCompletionWorkflow.h"

#include <iostream>
#include <string>

namespace {

int failures = 0;

void Check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

std::string ResolveError(const CueviFeedback::Result& result) {
    return "resolved:" + result.detail;
}

void TestSuccessTransition() {
    bool submitting = true;
    bool submitOk = false;
    std::string error = "old error";
    std::string workItemId = "old item";
    aura::feedback::CueviFeedbackCompletionState state{
        submitting, submitOk, error, workItemId};

    CueviFeedback::Result result;
    result.ok = true;
    result.workItemId = "CU-42";
    aura::feedback::ApplyCueviFeedbackCompletion(state, result, ResolveError);

    Check(!submitting, "success finishes the submission");
    Check(submitOk, "success sets the success flag");
    Check(error.empty(), "success clears the previous error");
    Check(workItemId == "CU-42", "success publishes the work item id");
}

void TestFailureTransitionPreservesPriorWorkItem() {
    bool submitting = true;
    bool submitOk = true;
    std::string error;
    std::string workItemId = "existing item";
    aura::feedback::CueviFeedbackCompletionState state{
        submitting, submitOk, error, workItemId};

    CueviFeedback::Result result;
    result.ok = false;
    result.error = CueviFeedback::Error::Network;
    result.detail = "offline";
    aura::feedback::ApplyCueviFeedbackCompletion(state, result, ResolveError);

    Check(!submitting, "failure finishes the submission");
    Check(!submitOk, "failure clears the success flag");
    Check(error == "resolved:offline", "failure uses the caller error resolver");
    Check(workItemId == "existing item", "failure preserves the prior work item id");
}

}  // namespace

int main() {
    TestSuccessTransition();
    TestFailureTransitionPreservesPriorWorkItem();
    if (failures != 0) {
        std::cerr << failures << " assertion(s) failed\n";
        return 1;
    }
    std::cout << "Cuevi feedback completion workflow tests passed\n";
    return 0;
}
