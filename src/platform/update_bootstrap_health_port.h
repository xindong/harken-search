#pragma once

#include <cstdint>
#include <memory>
#include <string>

namespace Platform::UpdateBootstrapHealth {

enum class Status {
    Accepted,
    Unsupported,
    InvalidChannel,
    Timeout,
    IoFailure,
};

struct Result {
    Status status = Status::Unsupported;
    std::string detail;

    bool ok() const { return status == Status::Accepted; }
};

class Port {
public:
    virtual ~Port() = default;
    virtual Result CaptureChallenge() = 0;
    virtual Result AcknowledgeReady() = 0;
};

struct CreateResult {
    Status status = Status::Unsupported;
    std::unique_ptr<Port> port;
    std::uint32_t systemError = 0;

    bool ok() const { return status != Status::Unsupported && port != nullptr; }
};

CreateResult CreateUpdateBootstrapHealthPort();

// The shipped old Bootstrap assigns stderr and the one-shot health stdout
// to the same handle. Preserve a private stdout duplicate and route stderr
// away before application startup can log. Caller owns the returned handle.
Result ProtectBootstrapHealthOutputFromStderr(std::uintptr_t* protectedOutput);

}  // namespace Platform::UpdateBootstrapHealth
