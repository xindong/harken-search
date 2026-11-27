#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace aris::update {

// @stage7-contract
// API contract: dedicated owned-byte check requests and an explicit
// cancellation barrier cross the platform/application boundary; no range
// download, filesystem, clock, or native handle is exposed.

// Dedicated blocking fetch port for small, owned update-check payloads. It is
// intentionally separate from the ranged download transport.
enum class UpdateCheckFetchError {
    None,
    Network,
    Http4xx,
    Http5xx,
    BodyTooLarge,
    InvalidResponse,
    Cancelled,
};

struct UpdateCheckFetchRequest {
    std::string url;
    std::uint64_t timeoutMs = 0;
    std::size_t maxBodyBytes = 0;
    bool allowRedirects = true;
};

struct UpdateCheckFetchResult {
    UpdateCheckFetchError error = UpdateCheckFetchError::None;
    std::vector<std::uint8_t> body;
    std::string detail;
    std::uint32_t httpStatus = 0;

    bool ok() const { return error == UpdateCheckFetchError::None; }
};

class UpdateCheckCancellation {
public:
    virtual ~UpdateCheckCancellation() = default;
    virtual bool IsCancellationRequested() const = 0;
    class InterruptRegistration {
    public:
        virtual ~InterruptRegistration() = default;
    };
    // The registration owns a barrier: its destructor waits until copied or
    // in-flight handlers have finished. Native transports use it to abort a
    // blocking request without adding threads to policy.
    virtual std::unique_ptr<InterruptRegistration> RegisterInterrupt(
        std::function<void()> handler) const = 0;
};

class UpdateCheckTransportPort {
public:
    virtual ~UpdateCheckTransportPort() = default;
    virtual UpdateCheckFetchResult Fetch(
        const UpdateCheckFetchRequest& request,
        const UpdateCheckCancellation& cancellation) = 0;
};

}  // namespace aris::update
