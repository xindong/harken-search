#include "DefaultAudioPlayerAssociation.h"

#include "platform/executable_path.h"
#include "platform/system.h"
#include "platform/text_encoding.h"

namespace {

const wchar_t* kAudioExts[] = {
    L".wav", L".mp3", L".flac", L".aiff", L".aif",
    L".ogg", L".m4a", L".wma", L".wv", nullptr
};

const char* kChoiceLeaves[] = {"UserChoice", "UserChoiceLatest", nullptr};

constexpr const char kProgId[] = "Auro.AudioFile";
constexpr const char kAppName[] = "HarkenSearch";
constexpr const char kCapabilitiesLeaf[] = "HarkenSearch";
constexpr const char kFileExtsRoot[] =
    "Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\FileExts";

std::string Join(const std::string& left, const std::string& right) {
    if (left.empty()) return right;
    if (right.empty()) return left;
    return left + "\\" + right;
}

std::string JoinRoot(const std::string& fileExtsRootUtf8, const wchar_t* ext, const char* leaf) {
    return Join(Join(fileExtsRootUtf8, Platform::TextEncoding::WideToUtf8Strict(ext)), leaf);
}

bool ProgIdIsOurs(const std::string& fileExtsRootUtf8, const wchar_t* ext, const char* leaf) {
    const auto progId = Platform::ReadRegistryString(
        Platform::RegistryHive::CurrentUser,
        JoinRoot(fileExtsRootUtf8, ext, leaf),
        "ProgId");
    return progId.status == Platform::RegistryStringStatus::Success &&
           progId.valueUtf8 == kProgId;
}

bool ClearOurProgId(const std::string& subkeyUtf8) {
    const auto current = Platform::ReadRegistryString(
        Platform::RegistryHive::CurrentUser, subkeyUtf8, "ProgId");
    if (current.status != Platform::RegistryStringStatus::Success ||
        current.valueUtf8 != kProgId) {
        return true;
    }
    if (!Platform::DeleteRegistryValue(
            Platform::RegistryHive::CurrentUser, subkeyUtf8, "ProgId")) {
        return false;
    }
    const auto after = Platform::ReadRegistryString(
        Platform::RegistryHive::CurrentUser, subkeyUtf8, "ProgId");
    return !(after.status == Platform::RegistryStringStatus::Success &&
             after.valueUtf8 == kProgId);
}

std::string ClassesRoot(const std::string& hkcuRootUtf8) {
    return Join(hkcuRootUtf8, "Software\\Classes");
}

} // namespace

bool RegisterDefaultProgramsCapabilitiesUnder(const std::string& hkcuRootUtf8) {
    const auto executablePath = Platform::GetExecutablePath();
    const std::string exeUtf8 =
        executablePath.status == Platform::ExecutablePathStatus::Success
            ? executablePath.pathUtf8
            : std::string{};
    const std::string classes = ClassesRoot(hkcuRootUtf8);
    bool ok = true;
    ok = Platform::WriteRegistryString(
             Platform::RegistryHive::CurrentUser,
             Join(classes, kProgId),
             "",
             "Audio File") &&
         ok;
    if (!exeUtf8.empty()) {
        const std::string command = std::string("\"") + exeUtf8 + "\" \"%1\"";
        ok = Platform::WriteRegistryString(
                 Platform::RegistryHive::CurrentUser,
                 Join(classes, std::string(kProgId) + "\\shell\\open\\command"),
                 "",
                 command) &&
             ok;
        ok = Platform::WriteRegistryString(
                 Platform::RegistryHive::CurrentUser,
                 Join(classes, std::string("Applications\\") + kAppName + ".exe\\shell\\open\\command"),
                 "",
                 command) &&
             ok;
    }

    const std::string capabilities = Join(
        hkcuRootUtf8, std::string("Software\\") + kCapabilitiesLeaf + "\\Capabilities");
    ok = Platform::WriteRegistryString(
             Platform::RegistryHive::CurrentUser, capabilities, "ApplicationName", "Harken Search") &&
         ok;
    ok = Platform::WriteRegistryString(
             Platform::RegistryHive::CurrentUser,
             capabilities,
             "ApplicationDescription",
             "Harken Search audio player") &&
         ok;
    for (int extensionIndex = 0; kAudioExts[extensionIndex]; ++extensionIndex) {
        const std::string ext = Platform::TextEncoding::WideToUtf8Strict(kAudioExts[extensionIndex]);
        ok = Platform::WriteRegistryString(
                 Platform::RegistryHive::CurrentUser,
                 Join(classes, ext + "\\OpenWithProgids"),
                 kProgId,
                 "") &&
             ok;
        ok = Platform::WriteRegistryString(
                 Platform::RegistryHive::CurrentUser,
                 Join(capabilities, "FileAssociations"),
                 ext,
                 kProgId) &&
             ok;
    }
    ok = Platform::WriteRegistryString(
             Platform::RegistryHive::CurrentUser,
             Join(hkcuRootUtf8, "Software\\RegisteredApplications"),
             kAppName,
             std::string("Software\\") + kCapabilitiesLeaf + "\\Capabilities") &&
         ok;
    Platform::NotifyShellAssociationsChanged();
    return ok;
}

bool RemoveDefaultPlayerUnder(const std::string& fileExtsRootUtf8) {
    bool ok = true;
    for (int extensionIndex = 0; kAudioExts[extensionIndex]; ++extensionIndex) {
        for (int leaf = 0; kChoiceLeaves[leaf]; ++leaf) {
            if (!ClearOurProgId(JoinRoot(fileExtsRootUtf8, kAudioExts[extensionIndex], kChoiceLeaves[leaf]))) {
                ok = false;
            }
        }
    }
    Platform::NotifyShellAssociationsChanged();
    return ok && !IsRegisteredAsDefaultUnder(fileExtsRootUtf8);
}

bool IsRegisteredAsDefaultUnder(const std::string& fileExtsRootUtf8) {
    for (int extensionIndex = 0; kAudioExts[extensionIndex]; ++extensionIndex) {
        for (int leaf = 0; kChoiceLeaves[leaf]; ++leaf) {
            if (ProgIdIsOurs(fileExtsRootUtf8, kAudioExts[extensionIndex], kChoiceLeaves[leaf])) {
                return true;
            }
        }
    }
    return false;
}

bool RegisterDefaultProgramsCapabilities() {
    return RegisterDefaultProgramsCapabilitiesUnder("");
}

bool IsSystemDefaultAudioPlayer() {
    for (int extensionIndex = 0; kAudioExts[extensionIndex]; ++extensionIndex) {
        const std::string ext = Platform::TextEncoding::WideToUtf8Strict(kAudioExts[extensionIndex]);
        const auto current = Platform::QueryEffectiveFileAssociation(ext);
        if (current.status == Platform::RegistryStringStatus::Success &&
            current.valueUtf8 == kProgId) {
            return true;
        }
    }
    return false;
}

void RegisterOpenWith() {
    (void)RegisterDefaultProgramsCapabilities();
}

void SetAsDefaultPlayer() {
    (void)RegisterDefaultProgramsCapabilities();
    (void)Platform::LaunchDefaultAppsSettings();
}

void RemoveDefaultPlayer() {
    (void)RemoveDefaultPlayerUnder(kFileExtsRoot);
    (void)Platform::LaunchDefaultAppsSettings();
}

bool IsRegisteredAsDefault() {
    return IsSystemDefaultAudioPlayer();
}
