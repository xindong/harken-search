#include "ReaperBridgeVersion.h"

#define HARKEN_STRINGIFY_VALUE_IMPL(value) #value
#define HARKEN_STRINGIFY_VALUE(value) HARKEN_STRINGIFY_VALUE_IMPL(value)

// Static bytes read by the inspector. This is identity metadata, not a signature.
__attribute__((used, section("__TEXT,__harken_meta")))
static const char kHarkenReaperMetadata[] =
    "HARKEN-REAPER-METADATA-1\n"
    "ProductName=HarkenSearch\n"
    "FileDescription=HarkenSearch REAPER Extension\n"
    "FileVersion=" ARIS_REAPER_BRIDGE_VERSION_STRING "\n"
    "BridgeProtocolVersion=" HARKEN_STRINGIFY_VALUE(ARIS_REAPER_BRIDGE_PROTOCOL_VERSION) "\n"
    "BridgeArchitecture=" ARIS_REAPER_BRIDGE_ARCHITECTURE "\n"
    "BridgeManifest=" ARIS_REAPER_BRIDGE_MANIFEST "\n";

#undef HARKEN_STRINGIFY_VALUE
#undef HARKEN_STRINGIFY_VALUE_IMPL
