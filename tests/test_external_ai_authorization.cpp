#include "ExternalAiAuthorization.h"
#include "ExternalAiAuthorizationStore.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <iostream>
#include <string>

namespace {

namespace fs = std::filesystem;
using HarkenSearch::ExternalAiAccess;
using HarkenSearch::ExternalAiAuthorization;
using HarkenSearch::ExternalAiCapability;

int failures = 0;

void Check(bool condition, const char* description)
{
    std::cout << (condition ? "PASS: " : "FAIL: ") << description << '\n';
    failures += !condition;
}

ExternalAiAuthorization ParseOk(const std::string& json)
{
    ExternalAiAuthorization authorization;
    std::string error;
    const bool ok = ExternalAiAuthorization::Parse(json, authorization, error);
    Check(ok, ("parse: " + json.substr(0, 60)).c_str());
    if (!ok) {
        std::cout << "  error: " << error << '\n';
    }
    return authorization;
}

bool ParseRejected(const std::string& json)
{
    ExternalAiAuthorization authorization;
    std::string error;
    return !ExternalAiAuthorization::Parse(json, authorization, error) && !error.empty();
}

std::string Escape(const std::string& value)
{
    std::string escaped;
    for (const char unit : value) {
        if (unit == '\\' || unit == '"') {
            escaped.push_back('\\');
        }
        escaped.push_back(unit);
    }
    return escaped;
}

std::string Document(const std::string& root, const std::string& capabilities, bool enabled = true,
    bool sample = false, const std::string& libraryId = "library-a", const std::string& rootsJson = "")
{
    const std::string roots = rootsJson.empty() ? "[\"" + Escape(root) + "\"]" : rootsJson;
    return std::string("{\"libraryId\":\"") + libraryId + "\",\"enabled\":" + (enabled ? "true" : "false") +
        ",\"sampleLibrary\":" + (sample ? "true" : "false") + ",\"roots\":" + roots +
        ",\"capabilities\":" + capabilities + "}";
}

}  // namespace

int main(int argc, char** argv)
{
    fs::path scratch = argc > 1 ? fs::u8path(argv[1]) : fs::current_path();
    const auto serial = std::chrono::steady_clock::now().time_since_epoch().count();
    const fs::path base = scratch / ("external-ai-authorization-" + std::to_string(serial));
    const fs::path root = base / "Sfx";
    const fs::path prefixTwin = base / "Sfx2";
    const fs::path outside = base / "Elsewhere";
    fs::create_directories(root / "wind");
    fs::create_directories(prefixTwin / "wind");
    fs::create_directories(outside / "wind");

    const auto rootText = root.u8string();
    const auto prefixText = prefixTwin.u8string();
    const auto outsideText = outside.u8string();
    const auto insideFile = (root / "wind" / "door.wav").u8string();
    const auto prefixFile = (prefixTwin / "wind" / "door.wav").u8string();
    const auto outsideFile = (outside / "wind" / "door.wav").u8string();

    const std::string allRead = "{\"searchAudio\":\"read\",\"findSimilar\":\"read\",\"findFolders\":\"read\","
        "\"readFileName\":\"read\",\"readMetadata\":\"read\",\"editMetadata\":\"none\","
        "\"aiNamingMetadata\":\"read\"}";

    const auto allowed = ParseOk(Document(rootText, allRead));
    Check(allowed.enabled() && allowed.libraryId() == "library-a", "parsed switch and library identity");
    Check(allowed.Authorize(ExternalAiCapability::SearchAudio, "library-a", insideFile) == ExternalAiAccess::ReadOnly,
        "granted root returns read-only");
    Check(allowed.Authorize(ExternalAiCapability::AiNamingMetadata, "library-a", insideFile) == ExternalAiAccess::ReadOnly,
        "AI naming and tagging permission is independently scoped");
    Check(allowed.Authorize(ExternalAiCapability::SearchAudio, "library-a", rootText) == ExternalAiAccess::ReadOnly,
        "root itself is inside the grant");
    Check(allowed.Authorize(ExternalAiCapability::SearchAudio, "library-a", prefixFile) == ExternalAiAccess::Denied,
        "shared name prefix is not inside the root");
    Check(allowed.Authorize(ExternalAiCapability::SearchAudio, "library-a", outsideFile) == ExternalAiAccess::Denied,
        "sibling directory is denied");
    Check(allowed.Authorize(ExternalAiCapability::SearchAudio, "library-a", "") == ExternalAiAccess::Denied,
        "library-wide query without a target is denied");
    Check(allowed.Authorize(ExternalAiCapability::SearchAudio, "library-a", "wind/door.wav") == ExternalAiAccess::Denied,
        "relative path is denied");
    Check(allowed.Authorize(ExternalAiCapability::SearchAudio, "library-a", rootText + "/../Elsewhere/wind/door.wav") ==
            ExternalAiAccess::Denied,
        "traversal out of the root is denied");
    Check(allowed.Authorize(ExternalAiCapability::SearchAudio, "library-b", insideFile) == ExternalAiAccess::Denied,
        "another library identity is denied");

    // A directory the user added but did not tick stays in the document and
    // grants nothing; the ticked sibling keeps working.
    const std::string tickedRoots = "[{\"path\":\"" + Escape(rootText) + "\",\"enabled\":true}," +
        "{\"path\":\"" + Escape(prefixTwin.u8string()) + "\",\"enabled\":false}]";
    const auto partlyTicked = ParseOk(Document(rootText, allRead, true, false, "library-a", tickedRoots));
    Check(partlyTicked.roots().size() == 2, "unticked directory stays in the document");
    Check(partlyTicked.Authorize(ExternalAiCapability::SearchAudio, "library-a", insideFile) == ExternalAiAccess::ReadOnly,
        "ticked directory still grants");
    Check(partlyTicked.Authorize(ExternalAiCapability::SearchAudio, "library-a", prefixFile) == ExternalAiAccess::Denied,
        "unticked directory is not authorized");
    const std::string allUnticked = "[{\"path\":\"" + Escape(rootText) + "\",\"enabled\":false}]";
    const auto nothingTicked = ParseOk(Document(rootText, allRead, true, false, "library-a", allUnticked));
    Check(nothingTicked.Authorize(ExternalAiCapability::SearchAudio, "library-a", insideFile) == ExternalAiAccess::Denied,
        "no ticked directory denies every capability");

    const auto legacy = ParseOk(Document(rootText, allRead));
    Check(legacy.roots().size() == 1 && legacy.roots().front().enabled,
        "legacy string roots still count as ticked");

    const auto disabled = ParseOk(Document(rootText, allRead, false));
    Check(disabled.Authorize(ExternalAiCapability::FindFolders, "library-a", rootText) == ExternalAiAccess::Denied,
        "switch off denies every capability");

    const auto noRoots = ParseOk(Document(rootText, allRead, true, false, "library-a", "[]"));
    Check(noRoots.Authorize(ExternalAiCapability::ReadMetadata, "library-a", insideFile) == ExternalAiAccess::Denied,
        "no ticked directory denies access");

    const auto separateReads = ParseOk(Document(rootText,
        "{\"readFileName\":\"read\",\"readMetadata\":\"none\",\"editMetadata\":\"edit\"}"));
    Check(separateReads.Authorize(ExternalAiCapability::ReadFileName, "library-a", insideFile) ==
            ExternalAiAccess::ReadOnly,
        "file name read stays independent");
    Check(separateReads.Authorize(ExternalAiCapability::ReadMetadata, "library-a", insideFile) == ExternalAiAccess::Denied,
        "metadata read stays closed when only file names are granted");
    Check(separateReads.Authorize(ExternalAiCapability::EditMetadata, "library-a", insideFile) ==
            ExternalAiAccess::Editable,
        "metadata edit is granted only when configured");

    const auto reversedReads = ParseOk(Document(rootText, "{\"readFileName\":\"none\",\"readMetadata\":\"read\"}"));
    Check(reversedReads.Authorize(ExternalAiCapability::ReadFileName, "library-a", insideFile) == ExternalAiAccess::Denied,
        "file name read can be closed while metadata stays readable");

    const auto sample = ParseOk(Document(rootText, "{\"editMetadata\":\"edit\"}", true, true));
    Check(sample.Authorize(ExternalAiCapability::EditMetadata, "library-a", insideFile) == ExternalAiAccess::ReadOnly,
        "sample library never becomes editable");

    const auto revoked = ParseOk(Document(rootText, "{\"searchAudio\":\"read\",\"editMetadata\":\"none\"}"));
    Check(revoked.Authorize(ExternalAiCapability::EditMetadata, "library-a", insideFile) == ExternalAiAccess::Denied,
        "lowered capability applies to the next request");

    const auto shrunk = ParseOk(Document(rootText, "{\"searchAudio\":\"read\"}", true, false, "library-a",
        "[\"" + Escape(prefixText) + "\"]"));
    Check(shrunk.Authorize(ExternalAiCapability::SearchAudio, "library-a", insideFile) == ExternalAiAccess::Denied,
        "removing a root revokes it immediately");

    Check(ParseRejected("{\"libraryId\":\"a\"}"), "missing enabled is rejected");
    Check(ParseRejected("{\"libraryId\":\"\",\"enabled\":true}"), "empty library id is rejected");
    Check(ParseRejected(Document(rootText, "{\"searchAudio\":\"write\"}")), "unknown access level is rejected");
    Check(ParseRejected(Document(rootText, "{\"searchAudioo\":\"read\"}")), "misspelled capability is rejected");
    Check(ParseRejected(Document(rootText, "{}", true, false, "library-a", "[\"\"]")), "empty root is rejected");
    Check(ParseRejected("{\"libraryId\":\"a\",\"enabled\":true,\"rootz\":[]}"), "unknown field is rejected");

    // --- Per-library document store used by the settings page ---
    const std::string settingsDir = (base / "settings-root").u8string();
    const std::string libraryIdText = "0123456789abcdef0123456789abcdef";
    const std::string libraryPath = HarkenSearch::ExternalAiSettingsPath(settingsDir, libraryIdText);
    Check(!libraryPath.empty() && libraryPath.find("external-ai") != std::string::npos &&
            libraryPath.size() > 5 && libraryPath.substr(libraryPath.size() - 5) == ".json",
        "settings path places the document under external-ai/<library>.json");
    Check(HarkenSearch::ExternalAiSettingsPath(settingsDir, "../escape").empty(),
        "settings path refuses an identity that is not a plain file name");
    Check(HarkenSearch::ExternalAiSettingsPath("", "library-a").empty(), "settings path needs a settings directory");

    HarkenSearch::ExternalAiSettings defaultSettings;
    defaultSettings.libraryId = libraryIdText;
    std::string storeError;
    bool existed = true;
    Check(HarkenSearch::LoadExternalAiSettings(libraryPath, libraryIdText, defaultSettings, storeError, &existed),
        "missing document loads as a usable default");
    Check(!existed && !defaultSettings.enabled && defaultSettings.roots.empty(),
        "a library without a document starts disabled with no ticked directory");
    for (const auto capability : HarkenSearch::ExternalAiCapabilities) {
        Check(defaultSettings.capability(capability) == ExternalAiAccess::Denied,
            "a library without a document keeps every capability closed");
    }

    HarkenSearch::ExternalAiSettings stored;
    stored.libraryId = libraryIdText;
    stored.enabled = true;
    stored.roots = {{rootText, true}, {prefixText, false}};
    stored.capabilities[0] = ExternalAiAccess::ReadOnly;
    stored.capabilities[1] = ExternalAiAccess::ReadOnly;
    stored.capabilities[3] = ExternalAiAccess::ReadOnly;
    stored.capabilities[5] = ExternalAiAccess::Editable;
    stored.capabilities[6] = ExternalAiAccess::ReadOnly;

    const std::string serialized = HarkenSearch::SerializeExternalAiSettings(stored);
    HarkenSearch::ExternalAiSettings reparsed;
    Check(!serialized.empty() &&
            HarkenSearch::ParseExternalAiSettings(serialized, libraryIdText, reparsed, storeError),
        "serialized document parses back for the same library");
    Check(reparsed.enabled == stored.enabled && reparsed.roots == stored.roots,
        "round trip keeps the switch and the ticked directories");
    bool capabilitiesMatch = true;
    for (const auto capability : HarkenSearch::ExternalAiCapabilities) {
        capabilitiesMatch = capabilitiesMatch && reparsed.capability(capability) == stored.capability(capability);
    }
    Check(capabilitiesMatch, "round trip keeps every capability level");
    Check(!HarkenSearch::ParseExternalAiSettings(serialized, "library-b", reparsed, storeError),
        "a document saved for one library is rejected for another");

    Check(HarkenSearch::SaveExternalAiSettings(libraryPath, stored, storeError), "document saves to disk");
    std::string savedBytes;
    {
        std::ifstream saved(libraryPath, std::ios::binary);
        savedBytes.assign((std::istreambuf_iterator<char>(saved)), std::istreambuf_iterator<char>());
    }
    Check(savedBytes == serialized, "saved file matches the serialized document");
    Check(savedBytes.size() >= 2 && static_cast<unsigned char>(savedBytes[0]) != 0xEF,
        "saved file has no byte order mark");

    stored.enabled = false;
    Check(HarkenSearch::SaveExternalAiSettings(libraryPath, stored, storeError),
        "saving again into an existing authorization directory succeeds");
    stored.enabled = true;
    Check(HarkenSearch::SaveExternalAiSettings(libraryPath, stored, storeError),
        "a later save replaces the previous document");
    HarkenSearch::ExternalAiSettings reloaded;
    existed = false;
    Check(HarkenSearch::LoadExternalAiSettings(libraryPath, libraryIdText, reloaded, storeError, &existed) &&
            existed && reloaded.enabled && reloaded.roots.size() == 2 &&
            reloaded.capability(ExternalAiCapability::EditMetadata) == ExternalAiAccess::Editable,
        "saved document loads back with the same grants");

    const auto connectorView = ParseOk(savedBytes);
    Check(connectorView.Authorize(ExternalAiCapability::ReadFileName, libraryIdText, insideFile) ==
            ExternalAiAccess::ReadOnly,
        "the connector accepts a document written by the settings store");

    {
        std::ofstream broken(libraryPath, std::ios::binary | std::ios::trunc);
        broken << "{\"libraryId\":\"" << libraryIdText << "\",\"enabled\":\"yes\"}";
    }
    HarkenSearch::ExternalAiSettings afterBroken;
    existed = false;
    Check(!HarkenSearch::LoadExternalAiSettings(libraryPath, libraryIdText, afterBroken, storeError, &existed) &&
            existed && !afterBroken.enabled,
        "a malformed document is reported instead of granting access");
    // Library identity shown on the settings page and copied into the connector command.
    const std::string idForward = HarkenSearch::ExternalAiLibraryIdForPath("C:/Sounds/Selected/Library.db");
    const std::string idBackward = HarkenSearch::ExternalAiLibraryIdForPath("c:\\sounds\\selected\\library.db");
    Check(!idForward.empty() && idForward.rfind("lib-", 0) == 0,
        "library identity uses the stable lib- prefix");
    Check(idForward == idBackward, "identity folds drive-letter case and slash style");
    Check(idForward != HarkenSearch::ExternalAiLibraryIdForPath("D:/Sounds/Selected/Library.db"),
        "a different library file gets a different identity");
    Check(HarkenSearch::ExternalAiLibraryIdForPath("").empty(), "an empty path has no library identity");
    bool identityCharactersValid = !idForward.empty() && idForward.size() <= 64;
    for (const char unit : idForward) {
        identityCharactersValid = identityCharactersValid &&
            ((unit >= '0' && unit <= '9') || (unit >= 'a' && unit <= 'z') || unit == '-');
    }
    Check(identityCharactersValid, "identity can name an authorization document");
    std::error_code ignored;
    fs::remove_all(base, ignored);
    std::cout << (failures == 0 ? "ALL PASS" : "FAILURES") << ": " << failures << '\n';
    return failures == 0 ? 0 : 1;
}
