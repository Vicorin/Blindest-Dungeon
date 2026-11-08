// game/gamebuild.cpp -- WHICH Darkest.exe is this process? One release of the game ships as TWO
// binaries

#include <windows.h>
#include "game/offsets.h"

// ---- FORCED RESOLVER ----
static bool axResolverForced() {
    wchar_t path[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", path, MAX_PATH);
    if (!n || n > MAX_PATH - 40) return false;
    wcscat_s(path, L"\\DarkestAccess\\force_resolver");
    return GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES;
}

bool axExeImportsSteam() {
    static bool cached = [] {
        auto base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
        auto nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
        auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
        if (!dir.VirtualAddress) return false;
        for (auto d = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress); d->Name; ++d)
            if (_stricmp(reinterpret_cast<const char*>(base + d->Name), "steam_api64.dll") == 0) return true;
        return false;
    }();
    return cached;
}

AxGameBuild axGameBuild() {
    static AxGameBuild cached = [] {
        if (axResolverForced()) return AX_BUILD_UNKNOWN;
        uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
        if (!base || dos->e_magic != IMAGE_DOS_SIGNATURE) return AX_BUILD_UNKNOWN;
        auto nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE) return AX_BUILD_UNKNOWN;
        uint32_t stamp = nt->FileHeader.TimeDateStamp;
        uint32_t size  = nt->OptionalHeader.SizeOfImage;
        if (stamp == GAME_PE_TIMESTAMP && size == GAME_PE_SIZEOFIMAGE)
            return AX_BUILD_STEAM;
        if (stamp == GAME_PE_TIMESTAMP_DRMFREE && size == GAME_PE_SIZEOFIMAGE_DRMFREE)
            return AX_BUILD_DRMFREE;
        if (stamp == GAME_PE_TIMESTAMP_DRMFREE_PREV && size == GAME_PE_SIZEOFIMAGE_DRMFREE_PREV)
            return AX_BUILD_DRMFREE_PREV;
        return AX_BUILD_UNKNOWN;
    }();
    return cached;
}

const char* axGameBuildName() {
    switch (axGameBuild()) {
        case AX_BUILD_STEAM:   return "Steam";
        case AX_BUILD_DRMFREE:      return "DRM-free (Steam depot copy)";   // _windowsnosteam\win64
        case AX_BUILD_DRMFREE_PREV: return "DRM-free (GOG)";
        default:               return "unknown";
    }
}
