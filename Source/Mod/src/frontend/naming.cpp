// frontend/naming.cpp -- THE TEXT-FIELD MIRROR

#include <cstdint>
#include <cstring>
#include <cstdio>
#include "internal.h"

// ---- Estate naming (text field after difficulty select) ----
static const uintptr_t SLOT_ESTATE_NAME_OFF = 0xc0;   // C-string estate name (<=0x20) in a SaveSlot

static const char* kDefaultEstate = "Darkest";

bool             g_nameModeHero     = false;

int feNamingSub(uintptr_t display) {
    uint32_t v = 0;
    if (!display || !safeReadU32(display + FE_NAMING_SUB_OFF, &v)) return -1;
    return (int)v;
}

static void mirrorAppend(const char* s) {
    size_t cur = strlen(g_typed), add = strlen(s);
    if (cur + add < sizeof g_typed) memcpy(g_typed + cur, s, add + 1);
}
static void mirrorPop() {
    size_t n = strlen(g_typed);
    if (!n) return;
    size_t i = n - 1;
    while (i > 0 && ((unsigned char)g_typed[i] & 0xC0) == 0x80) i--;   // UTF-8 continuation
    g_typed[i] = 0;
}

static void speakMirror() {
    const char* content = g_typed[0] ? g_typed : axs(AXS_NM_BLANK);
    if (strcmp(content, g_lastName) == 0) return;
    postSpeech(content);
    strncpy(g_lastName, content, sizeof g_lastName - 1);
    g_lastName[sizeof g_lastName - 1] = 0;
    logLine("naming SPEAK mirror=\"%s\"", g_typed);
}

void beginNaming(uintptr_t display) {
    if (g_naming) return;
    g_naming = true;
    g_lastKeyTick = GetTickCount();
    strncpy(g_typed, kDefaultEstate, sizeof g_typed - 1);
    g_typed[sizeof g_typed - 1] = 0;
    strncpy(g_lastName, g_typed, sizeof g_lastName - 1);   // seed already spoken in the cue
    g_lastName[sizeof g_lastName - 1] = 0;

    char line[256];
    _snprintf(line, sizeof line, "%s %s. %s", axs(AXS_NM_ESTATE_HEAD), kDefaultEstate, axs(AXS_NM_HELP));
    line[sizeof line - 1] = 0;
    postSpeech(line);
    logLine("naming OPEN sub=%d seed=\"%s\"", feNamingSub(display), kDefaultEstate);
}

void endNaming() {
    if (!g_naming) return;
    g_naming = false;
    g_nameModeHero = false;
    g_typed[0] = g_lastName[0] = 0;
    logLine("naming CLOSE");
}

void announceCommittedName(uintptr_t base) {
    char name[64]; name[0] = 0;
    bool ok = safeReadCStr(base + COMMIT_NAME_RVA, name, sizeof name);
    logLine("naming COMMIT ok=%d estate=\"%s\"", ok ? 1 : 0, ok ? name : "");
    if (ok && name[0]) {
        char line[128];
        _snprintf(line, sizeof line, axs(AXS_NM_ESTATE_NAMED_FMT), name);
        line[sizeof line - 1] = 0;
        postSpeech(line);
    }
}

void handleInputEvent(uintptr_t base, void* ev) {
    (void)base;
    if (!g_naming) return;                       // only relevant while the box is live
    uintptr_t e = (uintptr_t)ev;
    uint32_t type = 0;
    if (!safeReadU32(e, &type)) return;

    if (type == SDL_EVT_TEXTINPUT) {
        char chars[32]; chars[0] = 0;
        safeReadCStr(e + SDL_TEXTINPUT_TEXT, chars, sizeof chars);
        mirrorAppend(chars);
        g_lastKeyTick = GetTickCount();
        speakMirror();
    } else if (type == SDL_EVT_KEYDOWN) {
        uint32_t sym = 0; safeReadU32(e + SDL_KEY_SYM_OFF, &sym);
        if (sym == SDLK_BACKSPACE) { mirrorPop(); g_lastKeyTick = GetTickCount(); speakMirror(); }
        else if (sym == SDLK_RETURN || sym == SDLK_KP_ENTER) {
            if (g_nameModeHero) return;
            g_commitCheckAt = GetTickCount() + 300;   // read the committed name shortly after
            endNaming();
            g_reopenBlocked = true;
        }
        else if (sym == SDLK_ESCAPE) {
            if (g_nameModeHero) return;   // same: checkTownRename reads the outcome back
            endNaming();
            g_reopenBlocked = true;
        }
    }
}
