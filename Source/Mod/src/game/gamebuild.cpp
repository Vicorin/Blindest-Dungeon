// game/gamebuild.cpp -- WHICH Darkest.exe is this process? One release of the game ships as TWO
// binaries

#include <windows.h>
#include "game/offsets.h"

AxGameBuild axGameBuild() {
    static AxGameBuild cached = [] {
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
        return AX_BUILD_UNKNOWN;
    }();
    return cached;
}

const char* axGameBuildName() {
    switch (axGameBuild()) {
        case AX_BUILD_STEAM:   return "Steam";
        case AX_BUILD_DRMFREE: return "DRM-free (GOG)";
        default:               return "unknown";
    }
}
