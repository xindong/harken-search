#include "audio_file_dialog.h"
#include "Localization.h"
#include <string>

using namespace std::string_literals;

#include "text_encoding.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <commdlg.h>

namespace Platform::AudioDialogs {

std::string OpenAudioFileDialogUtf8(void* ownerWindow) {
    OPENFILENAMEW ofn = {};
    wchar_t fileName[MAX_PATH] = L"";
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = static_cast<HWND>(ownerWindow);
    const std::wstring filterBuf =
        std::wstring(Lw(Dlg_FilterAudioFiles)) + L"\0*.wav;*.mp3;*.flac;*.aiff;*.aif;*.ogg;*.m4a;*.wma;*.wv\0"s +
        std::wstring(Lw(Dlg_FilterAllFiles)) + L"\0*.*\0"s;
    ofn.lpstrFilter = filterBuf.c_str();
    ofn.lpstrFile = fileName;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    if (!GetOpenFileNameW(&ofn)) return {};
    return Platform::TextEncoding::WideToUtf8Permissive(fileName);
}

} // namespace Platform::AudioDialogs
