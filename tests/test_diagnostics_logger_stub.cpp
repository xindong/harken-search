#include "DiagnosticsLogger.h"
#include <iostream>

int main() {
    auto& logger = DiagnosticsLogger::instance();
    logger.setEnabled(true);
    if (logger.logSystemAt("Retention", "disabled build", 1) ||
        logger.isEnabled() || logger.entryCount() != 0 ||
        !logger.exportJsonString().empty()) {
        std::cerr << "FAIL: diagnostics-disabled builds must not retain entries\n";
        return 1;
    }
    return 0;
}
