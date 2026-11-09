#include "FixedBufferCopy.h"

#include <cstring>
#include <iostream>
#include <string>

namespace {
int failures = 0;

void Check(bool pass, const char* label) {
    std::cout << (pass ? "PASS " : "FAIL ") << label << std::endl;
    if (!pass) ++failures;
}
}

int main() {
    struct GuardedBuffer {
        char text[5]{};
        char guard = '!';
    } buffer;

    HarkenSearch::CopyTruncatedCString(buffer.text, std::string("abc"));
    Check(std::strcmp(buffer.text, "abc") == 0, "short text is preserved");

    HarkenSearch::CopyTruncatedCString(buffer.text, std::string("abcd"));
    Check(std::strcmp(buffer.text, "abcd") == 0, "exactly fitting text is preserved");

    HarkenSearch::CopyTruncatedCString(buffer.text, std::string("abcdef"));
    Check(std::strcmp(buffer.text, "abcd") == 0 && buffer.text[4] == char{0} &&
              buffer.guard == '!', "long text truncates and terminates without overflow");

    HarkenSearch::CopyTruncatedCString(buffer.text, std::string());
    Check(buffer.text[0] == char{0}, "empty text clears the buffer");

    HarkenSearch::CopyTruncatedCString(buffer.text, std::string({'a','b',char{0},'c','d'}));
    Check(std::strcmp(buffer.text, "ab") == 0, "embedded NUL matches C-string copying");

    char terminatorOnly[1] = {'x'};
    HarkenSearch::CopyTruncatedCString(terminatorOnly, std::string("abc"));
    Check(terminatorOnly[0] == char{0}, "one-byte buffer remains terminated");

    std::cout << "Fixed buffer copy: " << (failures == 0 ? "PASS" : "FAIL") << std::endl;
    return failures == 0 ? 0 : 1;
}
