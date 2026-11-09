#pragma once

#include "platform/system.h"

#include <cstdlib>
#include <string>

namespace HarkenSearch::CubaseAdapter::Detail {

inline std::string ReadEnvVar(const char* name) {
#ifdef _WIN32
    char* env = nullptr;
    size_t len = 0;
    if (_dupenv_s(&env, &len, name) != 0 || env == nullptr) {
        return {};
    }
    std::string value(env);
    std::free(env);
    return value;
#else
    if (name == nullptr) return {};
    const auto result = Platform::ReadEnvironmentVariable(name);
    return result.status == Platform::EnvironmentStringStatus::Success
        ? result.valueUtf8 : std::string{};
#endif
}

} // namespace HarkenSearch::CubaseAdapter::Detail
