#pragma once

// Test seam for durable_file_ops_macos.cpp.
//
// The durability contract lives in branches a real filesystem will not produce
// on demand: F_FULLFSYNC rejecting the command, a real write error such as EIO,
// and a directory that cannot be opened or synced. Without a seam those
// branches are only covered by reading the source, which is exactly how the
// errors were swallowed in the first place.
//
// A zero field means "call the real syscall". Only the macOS implementation
// includes this header, and only when AURO_DURABLE_FILE_OPS_TESTING is defined.

namespace Platform::FileSystem::Testing {

struct DurableFileOpFaultScript {
    // Linux-style failures to inject before the real call. 0 = no injection.
    int fullsyncError = 0;
    int fsyncError = 0;
    int directoryOpenError = 0;
};

void SetDurableFileOpFaultScript(DurableFileOpFaultScript* script);

} // namespace Platform::FileSystem::Testing
