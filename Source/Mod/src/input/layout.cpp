// input/layout.cpp -- the KEYBOARD LAYOUT FOLD

#include <windows.h>
#include <cstdio>
#include <cstring>
#include "internal.h"

// ---- The SDL entry points ----
typedef uint32_t    (__cdecl *SdlScanFromKeyFn)(uint32_t keycode);
typedef uint32_t    (__cdecl *SdlKeyFromScanFn)(uint32_t scancode);
typedef const char* (__cdecl *SdlKeyNameFn)(uint32_t keycode);

static SdlScanFromKeyFn g_sdlScanFromKey = nullptr;
static SdlKeyFromScanFn g_sdlKeyFromScan = nullptr;
static SdlKeyNameFn     g_sdlKeyName     = nullptr;
static bool             g_sdlResolved    = false;   // resolve attempted (success or not)
static HKL              g_hkl            = nullptr; // the layout the fold table was built for
static bool             g_haveHkl        = false;

static uint32_t g_fold[256];
static int      g_foldCount = 0;

// ---- The US-QWERTY home of each key the mod can name ----
static const uint8_t KL_SCAN_A   = 4;    // 'a'..'z' = 4..29
static const uint8_t KL_SCAN_1   = 30;   // '1'..'9' = 30..38
static const uint8_t KL_SCAN_0   = 39;   // '0'
struct KlPunctHome { uint32_t sym; uint8_t scan; };
static const KlPunctHome kPunctHomes[] = {
    { 0x2d, 45 },   // '-'
    { 0x3d, 46 },   // '='
    { 0x5b, 47 },   // '['
    { 0x5d, 48 },   // ']'
    { 0x5c, 49 },
    { 0x3b, 51 },   // ';'
    { 0x27, 52 },   // '\''
    { 0x60, 53 },
    { 0x2c, 54 },
    { 0x2e, 55 },   // '.'  (the combat / activity log)
    { 0x2f, 56 },   // '/'
};
static const int KL_PUNCT_COUNT = (int)(sizeof kPunctHomes / sizeof kPunctHomes[0]);

// ---- Building the table ----
static void klAddIfMissing(uint32_t sym, uint8_t usScan) {
    // SDL_SCANCODE_UNKNOWN is 0: "no key on this layout prints that character".
    if (g_sdlScanFromKey(sym) != 0) return;        // the layout HAS it -> match by letter, as ever
    g_fold[usScan] = sym;
    g_foldCount++;
}

static void klRebuild() {
    memset(g_fold, 0, sizeof g_fold);
    g_foldCount = 0;
    if (!g_sdlScanFromKey) return;
    for (uint32_t c = 'a'; c <= 'z'; c++) klAddIfMissing(c, (uint8_t)(KL_SCAN_A + (c - 'a')));
    for (uint32_t c = '1'; c <= '9'; c++) klAddIfMissing(c, (uint8_t)(KL_SCAN_1 + (c - '1')));
    klAddIfMissing('0', KL_SCAN_0);
    for (int i = 0; i < KL_PUNCT_COUNT; i++) klAddIfMissing(kPunctHomes[i].sym, kPunctHomes[i].scan);

    char klid[KL_NAMELENGTH] = "";
    if (!GetKeyboardLayoutNameA(klid)) klid[0] = 0;
    if (!g_foldCount) {
        logLine("layout: %s (hkl=%p) -- every mod key exists on this layout, fold inactive",
                klid[0] ? klid : "?", (void*)g_hkl);
        return;
    }
    char list[256] = "";
    int used = 0;
    for (int s = 0; s < 256 && used < (int)sizeof list - 8; s++) {
        if (!g_fold[s]) continue;
        int n = _snprintf(list + used, sizeof list - used, "%s%c",
                          used ? " " : "", (char)g_fold[s]);
        if (n <= 0) break;
        used += n;
    }
    list[sizeof list - 1] = 0;
    logLine("layout: %s (hkl=%p) -- %d key(s) absent, folded to their US positions: %s",
            klid[0] ? klid : "?", (void*)g_hkl, g_foldCount, list);
}

// ---- Refresh ----
void klRefresh() {
    if (!g_sdlResolved) {
        g_sdlResolved = true;
        HMODULE sdl = GetModuleHandleA("SDL2.dll");
        if (!sdl) { logLine("layout: SDL2.dll not in the process -- fold DISABLED"); return; }
        g_sdlScanFromKey = (SdlScanFromKeyFn)GetProcAddress(sdl, "SDL_GetScancodeFromKey");
        g_sdlKeyFromScan = (SdlKeyFromScanFn)GetProcAddress(sdl, "SDL_GetKeyFromScancode");
        g_sdlKeyName     = (SdlKeyNameFn)    GetProcAddress(sdl, "SDL_GetKeyName");
        if (!g_sdlKeyFromScan) logLine("layout: SDL_GetKeyFromScancode missing -- pos: bindings will not resolve");
        if (!g_sdlKeyName)     logLine("layout: SDL_GetKeyName missing -- unnamed keys stay numeric");
        if (!g_sdlScanFromKey) {
            logLine("layout: SDL_GetScancodeFromKey not exported -- fold DISABLED");
            return;
        }
    }
    if (!g_sdlScanFromKey) return;
    HKL now = GetKeyboardLayout(0);
    if (g_haveHkl && now == g_hkl) return;
    g_hkl = now;
    g_haveHkl = true;
    klRebuild();
}

// ---- The scancode a keycode lives at ----
uint32_t klScancodeForKey(uint32_t sym) {
    klRefresh();
    if (!g_sdlScanFromKey) return 0;
    return g_sdlScanFromKey(sym);
}

// ---- ...and back ----
uint32_t klKeyForScancode(uint32_t scan) {
    klRefresh();
    if (!g_sdlKeyFromScan || scan == 0 || scan >= 512) return 0;
    return g_sdlKeyFromScan(scan);
}

// ---- Naming a key SDL's way ----
bool klKeyName(uint32_t sym, char* out, int outsz) {
    if (!out || outsz <= 0) return false;
    out[0] = 0;
    klRefresh();
    if (!g_sdlKeyName) return false;
    const char* n = g_sdlKeyName(sym);            // SDL's own static buffer; main thread only
    if (!n || !n[0]) return false;
    _snprintf(out, outsz, "%s", n);
    out[outsz - 1] = 0;
    return true;
}

// ---- The fold ----
bool klFold(uintptr_t base, uint32_t* sym, uint32_t scan) {
    klRefresh();
    if (!g_foldCount) return false;                 // the common case: one branch and out
    if (g_textInputActive) return false;
    if (scan >= 256) return false;
    uint32_t canon = g_fold[scan];
    if (!canon || canon == *sym) return false;

    char owner[0x44];
    if (kbActionForKey(base, (int)*sym, owner, sizeof owner)) {
        logLine("layout: not folding scan=%u (sym=0x%x) -> '%c': the GAME binds it to %s",
                scan, *sym, (char)canon, owner);
        return false;
    }
    *sym = canon;
    return true;
}
