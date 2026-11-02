#include "SettingsFileWriter.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#endif

struct FailingWriter : Platform::FileSystem::StreamWriter {
    inline static int mode=0;
    int writes=0;
    bool Open(const std::string& p, bool append, bool binary) { return mode!=1 && StreamWriter::Open(p,append,binary); }
    bool Write(std::string_view value) { return !(mode==2 && ++writes==2) && StreamWriter::Write(value); }
    bool Flush() { return mode!=3 && StreamWriter::Flush(); }
    bool CloseChecked() { const bool ok=StreamWriter::CloseChecked(); return ok && mode!=4; }
};
std::string Read(const std::filesystem::path& path) {
    std::ifstream in(path,std::ios::binary);
    return {std::istreambuf_iterator<char>(in),std::istreambuf_iterator<char>()};
}
int main(int argc,char** argv) {
    if(argc!=2)return 2;
    namespace fs=std::filesystem;
    const fs::path root=fs::u8path(argv[1]);fs::create_directories(root);
    const auto target=root/"settings.ini";
    const std::string original="old=keep-me\n";
    for(int failure=1;failure<=4;++failure) {
        {std::ofstream out(target,std::ios::binary|std::ios::trunc);out<<original;}
        FailingWriter::mode=failure;
        const bool saved=harken::settings::SaveKeyValueFile<FailingWriter>(target.u8string(),{{"volume","0.8"}});
        if(saved || Read(target)!=original) {std::cerr<<"failure phase "<<failure<<" did not preserve original settings";return 1;}
        for(const auto& entry:fs::directory_iterator(root))
            if(entry.path().filename().u8string().find(".pending-")!=std::string::npos)return 1;
    }
    if(!harken::settings::SaveKeyValueFile(target.u8string(),{{"volume","0.8"}}) ||
       Read(target)!="# Aris settings (auto-generated)\nvolume=0.8\n")return 1;
#ifdef _WIN32
    const auto savedBytes=Read(target);
    HANDLE locked=CreateFileW(target.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(locked==INVALID_HANDLE_VALUE)return 2;
    const bool replaced=harken::settings::SaveKeyValueFile(target.u8string(),{{"volume","0.1"}});
    CloseHandle(locked);
    if(replaced || Read(target)!=savedBytes)return 1;
#endif
    const auto blocked=root/"occupied.ini";fs::create_directories(blocked);
    if(harken::settings::SaveKeyValueFile(blocked.u8string(),{{"volume","0.1"}})||!fs::is_directory(blocked))return 1;
    for(const auto& entry:fs::directory_iterator(root))
        if(entry.path().filename().u8string().find(".pending-")!=std::string::npos)return 1;
    std::cout<<"settings preserve original bytes after open/write/flush/close/replace failures\n";
}
