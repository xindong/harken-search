#include <cstdio>
#include <cstdlib>

// Inspection must never execute this initializer.
__attribute__((constructor)) static void RecordUnexpectedLoad() {
    if (const char* path = std::getenv("HARKEN_INSPECTION_LOAD_MARKER")) {
        if (FILE* output = std::fopen(path, "w")) {
            std::fputs("inspection executed the binary", output);
            std::fclose(output);
        }
    }
}
extern "C" __attribute__((visibility("default"))) int ReaperPluginEntry() { return 1; }
extern "C" __attribute__((visibility("hidden"))) int HiddenInspectionSymbol() { return 2; }
