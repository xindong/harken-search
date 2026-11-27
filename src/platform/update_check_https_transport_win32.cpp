#include "update/UpdateCheckHttpsTransport.h"
#include "update/UpdateReleaseConfig.h"

#if defined(AURA_BUILD_DIAGNOSTICS) && AURA_BUILD_DIAGNOSTICS
#include "DiagnosticsLogger.h"
#define ARIS_UPDATE_DIAG(detail) DIAG_SYSTEM("AutoUpdate", detail)
#else
#define ARIS_UPDATE_DIAG(detail) do {} while (0)
#endif

#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <wininet.h>

#include <algorithm>
#include <limits>
#include <memory>
#include <mutex>
#include <utility>

namespace aris::update {
namespace {

class InternetSession final {
public:
    explicit InternetSession(HINTERNET handle = nullptr) : handle_(handle) {}
    ~InternetSession() {
        if (handle_) InternetCloseHandle(handle_);
    }
    InternetSession(const InternetSession&) = delete;
    InternetSession& operator=(const InternetSession&) = delete;
    explicit operator bool() const noexcept { return handle_ != nullptr; }
    HINTERNET release() noexcept { return std::exchange(handle_, nullptr); }

private:
    HINTERNET handle_ = nullptr;
};

class WinInetRequestState final
    : public std::enable_shared_from_this<WinInetRequestState> {
public:
    ~WinInetRequestState() { Abort(); }

    void Abort() noexcept {
        HINTERNET session = nullptr;
        HINTERNET request = nullptr;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            cancelled_ = true;
            abortRequested_ = true;
            session = std::exchange(session_, nullptr);
            request = std::exchange(request_, nullptr);
        }
        // Removing ownership under the lock makes this the sole close path.
        // WinINet treats InternetCloseHandle as the cancellation primitive for
        // an in-flight synchronous operation; Invoke observes the resulting
        // failure and maps it to Cancelled while no second close can occur.
        if (request) InternetCloseHandle(request);
        if (session) InternetCloseHandle(session);
    }

    bool Cancelled() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return cancelled_;
    }

    bool AdoptSession(HINTERNET handle) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (cancelled_ || abortRequested_) {
            if (handle) InternetCloseHandle(handle);
            return false;
        }
        session_ = handle;
        return session_ != nullptr;
    }

    bool AdoptRequest(HINTERNET handle) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (cancelled_ || abortRequested_) {
            if (handle) InternetCloseHandle(handle);
            return false;
        }
        request_ = handle;
        return request_ != nullptr;
    }

    void CloseRequest() noexcept {
        HINTERNET request = nullptr;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            request = std::exchange(request_, nullptr);
        }
        if (request) InternetCloseHandle(request);
    }

    bool InvokeSession(const std::function<bool(HINTERNET)>& operation) {
        HINTERNET session = nullptr;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (cancelled_ || !session_) return false;
            ++inFlight_;
            session = session_;
        }
        const bool succeeded = operation(session);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            --inFlight_;
            return succeeded && !cancelled_;
        }
    }

    HINTERNET InvokeOpenRequest(
        const std::function<HINTERNET(HINTERNET)>& operation) {
        HINTERNET session = nullptr;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (cancelled_ || !session_) return nullptr;
            ++inFlight_;
            session = session_;
        }
        HINTERNET request = operation(session);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            --inFlight_;
        }
        if (!AdoptRequest(request)) return nullptr;
        return request;
    }

    bool InvokeRequest(const std::function<bool(HINTERNET)>& operation) {
        HINTERNET request = nullptr;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (cancelled_ || !request_) return false;
            ++inFlight_;
            request = request_;
        }
        const bool succeeded = operation(request);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            --inFlight_;
            return succeeded && !cancelled_;
        }
    }

private:
    mutable std::mutex mutex_;
    HINTERNET session_ = nullptr;
    HINTERNET request_ = nullptr;
    std::uint32_t inFlight_ = 0;
    bool cancelled_ = false;
    bool abortRequested_ = false;
};

std::wstring Wide(const std::string& value) {
    if (value.empty() ||
        value.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return {};
    }
    const int length = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), nullptr, 0);
    if (length <= 0) return {};
    std::wstring result(static_cast<std::size_t>(length), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                            static_cast<int>(value.size()), result.data(),
                            length) != length) {
        return {};
    }
    return result;
}

std::string Utf8(const std::wstring& value) {
    if (value.empty() ||
        value.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return {};
    }
    const int length = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (length <= 0) return {};
    std::string result(static_cast<std::size_t>(length), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
                            static_cast<int>(value.size()), result.data(),
                            length, nullptr, nullptr) != length) {
        return {};
    }
    return result;
}

UpdateCheckFetchResult Failure(UpdateCheckFetchError error,
                               std::string detail,
                               std::uint32_t httpStatus = 0) {
    UpdateCheckFetchResult result;

    result.error = error;
    result.detail = std::move(detail);
    result.httpStatus = httpStatus;
    return result;
}

bool IsAvailabilityStatus(std::uint32_t status) {
    return status == 403 || status == 404 || status == 408 || status == 429 ||
           (status >= 500 && status < 600);
}

class WinInetResponseReadPort final : public UpdateCheckHttpsReadPort {
public:
    explicit WinInetResponseReadPort(
        std::shared_ptr<WinInetRequestState> state)
        : state_(std::move(state)) {}

    UpdateCheckReadResult Read(std::size_t maxBytes,
                               std::uint64_t) override {
        std::vector<std::uint8_t> bytes(maxBytes);
        DWORD read = 0;
        const bool succeeded = state_->InvokeRequest([&](HINTERNET request) {
            if (InternetReadFile(request, bytes.data(),
                                 static_cast<DWORD>(bytes.size()), &read)) {
                return true;
            }
            return false;
        });
        if (!succeeded) {
            const auto error = GetLastError();
            return {state_->Cancelled() || error == ERROR_OPERATION_ABORTED
                        ? UpdateCheckReadStatus::Aborted
                        : UpdateCheckReadStatus::Network,
                    {}};
        }
        if (read == 0) return {UpdateCheckReadStatus::End, {}};
        bytes.resize(read);
        return {UpdateCheckReadStatus::Chunk, std::move(bytes)};
    }

    void Abort() noexcept override { state_->Abort(); }

private:
    std::shared_ptr<WinInetRequestState> state_;
};

class WinInetUpdateCheckTransport final : public UpdateCheckTransportPort {
public:
    UpdateCheckFetchResult Fetch(
        const UpdateCheckFetchRequest& request,
        const UpdateCheckCancellation& cancellation) override {
        if (!IsApprovedUpdateCheckUrl(request.url, true) ||
            request.timeoutMs == 0 || request.maxBodyBytes == 0 ||
            request.maxBodyBytes > kUpdateCheckHttpsMaximumBodyBytes ||
            request.timeoutMs > static_cast<std::uint64_t>(MAXDWORD)) {
            return Failure(UpdateCheckFetchError::InvalidResponse,
                           "invalid trusted update check request");
        }

        const auto state = std::make_shared<WinInetRequestState>();
        // This is installed before InternetOpenW/InternetOpenUrlW and remains
        // alive through every query, body read, and request teardown.
        auto registration = cancellation.RegisterInterrupt([state]() {
            state->Abort();
        });
        if (cancellation.IsCancellationRequested() || state->Cancelled()) {
            state->Abort();
            return Failure(UpdateCheckFetchError::Cancelled,
                           "update check cancelled");
        }

        InternetSession opened(InternetOpenW(
            L"Harken Search Update Check/1", INTERNET_OPEN_TYPE_PRECONFIG,
            nullptr, nullptr, 0));
        if (!opened) {
            return Failure(state->Cancelled()
                               ? UpdateCheckFetchError::Cancelled
                               : UpdateCheckFetchError::Network,
                           "update check HTTPS session failed");
        }
        // Transfer ownership to the request state. A cancellation that raced
        // setup closes this handle in AdoptSession before it can be reused.
        HINTERNET session = opened.release();
        if (!state->AdoptSession(session)) {
            return Failure(UpdateCheckFetchError::Cancelled,
                           "update check cancelled during session setup");
        }

        auto timeout = static_cast<DWORD>(request.timeoutMs);
        auto readTimeout = static_cast<DWORD>(
            std::min<std::uint64_t>(request.timeoutMs,
                                    kUpdateCheckHttpsReadTimeoutMs));
        const bool optionsApplied = state->InvokeSession([&](HINTERNET handle) {
            return InternetSetOptionW(handle, INTERNET_OPTION_CONNECT_TIMEOUT,
                                      &timeout, sizeof(timeout)) &&
                   InternetSetOptionW(handle, INTERNET_OPTION_SEND_TIMEOUT,
                                      &timeout, sizeof(timeout)) &&
                   InternetSetOptionW(handle, INTERNET_OPTION_RECEIVE_TIMEOUT,
                                      &readTimeout, sizeof(readTimeout));
        });
        if (!optionsApplied) {
            return Failure(state->Cancelled()
                               ? UpdateCheckFetchError::Cancelled
                               : UpdateCheckFetchError::InvalidResponse,
                           "update check timeout policy could not be applied");
        }

        bool availabilityFailure = false;
        const auto fetchCandidate = [&](std::string current) {
            availabilityFailure = false;
            constexpr int kMaximumRedirects = 3;
            for (int redirect = 0; redirect <= kMaximumRedirects; ++redirect) {
                if (cancellation.IsCancellationRequested() ||
                    state->Cancelled()) {
                    state->Abort();
                    return Failure(UpdateCheckFetchError::Cancelled,
                                   "update check cancelled");
                }
                const auto wideUrl = Wide(current);
                if (wideUrl.empty()) {
                    return Failure(UpdateCheckFetchError::InvalidResponse,
                                   "update check URL encoding failed");
                }
                const auto requestHandle = state->InvokeOpenRequest(
                    [&](HINTERNET sessionHandle) {
                        DWORD flags = INTERNET_FLAG_RELOAD |
                                      INTERNET_FLAG_NO_CACHE_WRITE |
                                      INTERNET_FLAG_NO_UI |
                                      INTERNET_FLAG_NO_AUTO_REDIRECT;
                        if (current.rfind("https://", 0) == 0) {
                            flags |= INTERNET_FLAG_SECURE;
                        }
                        return InternetOpenUrlW(sessionHandle, wideUrl.c_str(),
                                                nullptr, 0, flags, 0);
                    });
                if (!requestHandle) {
                    availabilityFailure = !state->Cancelled();
                    return Failure(state->Cancelled()
                                       ? UpdateCheckFetchError::Cancelled
                                       : UpdateCheckFetchError::Network,
                                   "update check HTTPS request failed");
                }

                DWORD status = 0;
                DWORD statusBytes = sizeof(status);
                const bool statusRead = state->InvokeRequest(
                    [&](HINTERNET handle) {
                        return HttpQueryInfoW(
                                   handle, HTTP_QUERY_STATUS_CODE |
                                               HTTP_QUERY_FLAG_NUMBER,
                                   &status, &statusBytes, nullptr) != FALSE;
                    });
                if (!statusRead) {
                    return Failure(state->Cancelled()
                                       ? UpdateCheckFetchError::Cancelled
                                       : UpdateCheckFetchError::InvalidResponse,
                                   "update check response status is missing");
                }
                if (status >= 300 && status < 400) {
                    if (!request.allowRedirects) {
                        return Failure(UpdateCheckFetchError::InvalidResponse,
                                       "signed update pair cannot redirect",
                                       status);
                    }
                    if (redirect == kMaximumRedirects) {
                        return Failure(UpdateCheckFetchError::InvalidResponse,
                                       "update check redirect limit exceeded");
                    }
                    DWORD locationBytes = 0;
                    const bool sized = state->InvokeRequest(
                        [&](HINTERNET handle) {
                            HttpQueryInfoW(handle, HTTP_QUERY_LOCATION, nullptr,
                                           &locationBytes, nullptr);
                            return GetLastError() == ERROR_INSUFFICIENT_BUFFER;
                        });
                    if (!sized || locationBytes < sizeof(wchar_t)) {
                        return Failure(
                            state->Cancelled()
                                ? UpdateCheckFetchError::Cancelled
                                : UpdateCheckFetchError::InvalidResponse,
                            "update check redirect is missing");
                    }
                    std::wstring location(locationBytes / sizeof(wchar_t),
                                          L'\0');
                    DWORD copiedBytes = locationBytes;
                    const bool copied = state->InvokeRequest(
                        [&](HINTERNET handle) {
                            return HttpQueryInfoW(handle, HTTP_QUERY_LOCATION,
                                                  location.data(), &copiedBytes,
                                                  nullptr) != FALSE;
                        });
                    if (!copied) {
                        return Failure(
                            state->Cancelled()
                                ? UpdateCheckFetchError::Cancelled
                                : UpdateCheckFetchError::InvalidResponse,
                            "update check redirect is invalid");
                    }
                    location.resize(copiedBytes / sizeof(wchar_t));
                    while (!location.empty() && location.back() == L'\0') {
                        location.pop_back();
                    }
                    current = Utf8(location);
                    if (!IsApprovedUpdateCheckUrl(current, false)) {
                        return Failure(UpdateCheckFetchError::InvalidResponse,
                                       "update check redirect is not approved");
                    }
                    state->CloseRequest();
                    continue;
                }

                const auto classified = ClassifyUpdateCheckHttpStatus(status);
                if (classified != UpdateCheckFetchError::None) {
                    availabilityFailure = IsAvailabilityStatus(status);
                    return Failure(
                        classified,
                        "update check HTTP status " + std::to_string(status) +
                            " rejected",
                        status);
                }

                WinInetResponseReadPort reader(state);
                auto result = ReadUpdateCheckHttpsBody(
                    reader, cancellation, request.maxBodyBytes);
                availabilityFailure =
                    result.error == UpdateCheckFetchError::Network;
                return result;
            }
            return Failure(UpdateCheckFetchError::InvalidResponse,
                           "update check redirect failed");
        };

        ARIS_UPDATE_DIAG(IsCompanyCloudStableManifestUrl(request.url)
                             ? "CheckSourceAttempt=CompanyCloudManifest"
                             : IsCompanyCloudBootstrapPackageUrl(request.url)
                                 ? "CheckSourceAttempt=CompanyCloudBootstrapPlan"
                                 : "CheckSourceAttempt=GitHub");
        auto last = fetchCandidate(request.url);
        if (last.ok()) {
            ARIS_UPDATE_DIAG(IsCompanyCloudStableManifestUrl(request.url)
                                 ? "CheckSource=CompanyCloudManifest"
                                 : IsCompanyCloudBootstrapPackageUrl(request.url)
                                     ? "CheckSource=CompanyCloudBootstrapPlan"
                                     : "CheckSource=GitHub");
        }
        return last;
    }
};

}  // namespace

UpdateCheckHttpsCreateResult CreateUpdateCheckHttpsTransport() {
    UpdateCheckHttpsCreateResult result;
    result.status = UpdateCheckHttpsCreateStatus::Success;
    result.port = std::make_unique<WinInetUpdateCheckTransport>();
    return result;
}

}  // namespace aris::update

#endif

#undef ARIS_UPDATE_DIAG
