#include "platform/theme_json_dialogs.h"

namespace Platform::ThemeDialogs {

#ifndef __APPLE__
std::string OpenThemeJsonFileDialogUtf8(void*, const std::string&) {
    return "";
}
#endif

#ifndef __APPLE__
std::string SaveThemeJsonFileDialogUtf8(void*, const std::string&) {
    return "";
}
#endif

} // namespace Platform::ThemeDialogs
