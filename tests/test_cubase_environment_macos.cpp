#include "CubaseEnvironment.h"

#include <cstdlib>
#include <iostream>
#include <string>
#include <utility>
#include <unistd.h>

namespace {

int failures = 0;

void Check(bool condition, const char* message) {
    if (condition) {
        std::cout << "PASS: " << message << '\n';
    } else {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

class EnvironmentRestore {
public:
    explicit EnvironmentRestore(std::string name) : name_(std::move(name)) {
        const char* current = std::getenv(name_.c_str());
        if (current != nullptr) {
            existed_ = true;
            value_ = current;
        }
    }

    ~EnvironmentRestore() {
        if (existed_) {
            setenv(name_.c_str(), value_.c_str(), 1);
        } else {
            unsetenv(name_.c_str());
        }
    }

private:
    std::string name_;
    std::string value_;
    bool existed_ = false;
};

} // namespace

int main() {
    const std::string name = "HARKEN_CUBASE_ENV_QA_" + std::to_string(getpid());
    EnvironmentRestore restore(name);
    using HarkenSearch::CubaseAdapter::Detail::ReadEnvVar;

    Check(unsetenv(name.c_str()) == 0, "isolated variable can be removed");
    Check(ReadEnvVar(name.c_str()).empty(), "missing variable returns empty");

    Check(setenv(name.c_str(), "", 1) == 0, "isolated variable can be empty");
    Check(ReadEnvVar(name.c_str()).empty(), "empty variable returns empty");

    Check(setenv(name.c_str(), "bridge-helper", 1) == 0,
          "isolated variable can be set");
    Check(ReadEnvVar(name.c_str()) == "bridge-helper",
          "nonempty variable returns its value");

    const std::string invalidUtf8(1, static_cast<char>(0xFF));
    Check(setenv(name.c_str(), invalidUtf8.c_str(), 1) == 0,
          "isolated variable can contain invalid UTF-8");
    Check(ReadEnvVar(name.c_str()).empty(),
          "invalid UTF-8 fails closed");
    Check(ReadEnvVar(nullptr).empty(), "null name fails closed");

    return failures == 0 ? 0 : 1;
}
