// frontend/loading.cpp — the first frontend slice

#include <cstdint>
#include <cstring>
#include <cstdio>
#include "internal.h"

// ---- Loading screen (region art + title + flavor "tip" + continue prompt) ----
static const uintptr_t LS_TITLE_ID_OFF = 0x248;      // int32 title loc-id (0 = no title)   [change-key]
static const uintptr_t LS_TIP_ID_OFF   = 0x24c;
static const uintptr_t LS_TITLE_TXT_OFF= 0x2a8;      // char* -> localized title text
static const uintptr_t LS_TIP_TXT_OFF  = 0x2b8;      // char* -> localized flavor/tip text
static const uintptr_t LS_ACTIVE_OFF   = 0x2cc;      // byte: screen active/visible
static const uintptr_t LS_DONE_OFF     = 0x2cd;      // byte: DoneLoading -> "continue" available
static const uintptr_t LS_BUILT_OFF    = 0x2d4;      // byte: set 1 when the build completes
static const char* kLoadingContinueKey = "str_loading_screen_continue";  // "Press [SPACE] ... to Continue"

static const char* kPreambleKeyFmt   = "str_preamble_%d";
static const int   PREAMBLE_MAX_LINES = 16;      // sanity cap on the resolve loop
static const char* kPreambleTitle    = "Darkest Dungeon";
static volatile bool g_preambleSpoken  = false;  // announced for the current state entry

volatile bool g_preambleHold    = false;
DWORD         g_preambleHoldUntil = 0;    // backstop tick; input releases it sooner

static uint64_t      g_lsKey            = 0;     // (titleId<<32)|tipId last announced (0 = none up)
static volatile bool g_lsDoneLatch      = false; // announced the continue prompt for this screen
static volatile bool g_lsContinuePending= false;
static DWORD         g_lsSpokeAt        = 0;     // tick we last spoke loading-screen content
volatile bool        g_lsSuppressFocus  = false; // a loading screen is up -> frameCheck stays quiet

// ---- Intro preamble (runs on the game's MAIN thread — calls the resolver) ----
static int buildPreamble(uintptr_t base, char* full, int sz) {
    full[0] = 0;
    int got = 0;
    for (int i = 0; i < PREAMBLE_MAX_LINES; i++) {
        char key[32];
        _snprintf(key, sizeof key, kPreambleKeyFmt, i);
        key[sizeof key - 1] = 0;
        char line[512];
        if (!resolveKey(base, key, line, sizeof line)) break;   // no more preamble lines
        int cur = (int)strlen(full);
        _snprintf(full + cur, sz - cur, "%s%s", cur ? " " : "", line);
        full[sz - 1] = 0;
        got++;
    }
    if (got > 0) {
        int cur = (int)strlen(full);
        _snprintf(full + cur, sz - cur, " %s", kPreambleTitle);
        full[sz - 1] = 0;
    }
    return got;
}

static void speakPreamble(uintptr_t base) {
    char full[MAILBOX_SZ];
    int got = buildPreamble(base, full, sizeof full);
    logLine("preamble lines=%d text=\"%.100s\"", got, full);
    if (got > 0) {
        postSpeech(full);
        g_preambleHold = true;
        g_preambleHoldUntil = GetTickCount() + 90000;   // backstop; any input releases it sooner
    }
}

void checkPreamble(uintptr_t base, uintptr_t display) {
    static int lastState = -999;
    int st = display ? frontEndState(display) : -1;
    if (st != lastState) {
        logLine("festate %d -> %d", lastState, st);
        if (st == FE_STATE_TITLE) g_titlePrefixPending = true;
        lastState = st;
    }

    if (st != FE_STATE_PREAMBLE) { g_preambleSpoken = false; return; }
    if (g_preambleSpoken) return;
    g_preambleSpoken = true;          // set first so we never retry every frame
    speakPreamble(base);
}

// ---- Loading screen reader (main thread) ----

static void lsReadText(uintptr_t ls, char* title, int tsz, char* tip, int psz) {
    if (title && tsz) title[0] = 0;
    if (tip   && psz) tip[0]   = 0;
    uint32_t titleId = 0, tipId = 0;
    safeReadU32(ls + LS_TITLE_ID_OFF, &titleId);
    safeReadU32(ls + LS_TIP_ID_OFF,   &tipId);
    char titleRaw[MAILBOX_SZ] = {0}, tipRaw[MAILBOX_SZ] = {0};
    uintptr_t p = 0;
    if (titleId && safeReadPtr(ls + LS_TITLE_TXT_OFF, &p) && p > 0x10000)
        safeReadCStr(p, titleRaw, sizeof titleRaw);
    if (tipId && safeReadPtr(ls + LS_TIP_TXT_OFF, &p) && p > 0x10000)
        safeReadCStr(p, tipRaw, sizeof tipRaw);
    stripMarkup(titleRaw, title, tsz);
    stripMarkup(tipRaw,   tip,   psz);
}

static void lsComposeLine(const char* title, const char* tip, char* out, int outsz) {
    int n = _snprintf(out, outsz - 1, "%s%s%s%s%s", axs(AXS_LS_LOADING),
                      title[0] ? " " : "", title,
                      (title[0] && tip[0]) ? ". " : (tip[0] ? " " : ""), tip);
    if (n < 0) out[outsz - 1] = 0;
}

void lsReannounce(uintptr_t base) {
    uintptr_t ls = 0;
    if (!safeReadPtr(base + LS_PTR_RVA, &ls) || ls <= 0x10000) return;
    char title[MAILBOX_SZ] = {0}, tip[MAILBOX_SZ] = {0}, utter[MAILBOX_SZ];
    lsReadText(ls, title, sizeof title, tip, sizeof tip);
    if (!title[0] && !tip[0]) return;
    lsComposeLine(title, tip, utter, sizeof utter);
    logLine("loadingscreen: re-announcing on request");
    postSpeech(utter);
}

void checkLoadingScreen(uintptr_t base) {
    uintptr_t ls = 0;
    if (!safeReadPtr(base + LS_PTR_RVA, &ls) || ls <= 0x10000) {
        g_lsKey = 0; g_lsDoneLatch = false; g_lsContinuePending = false;
        g_lsSuppressFocus = false;
        return;
    }

    uint8_t  built = 0, active = 0, done = 0;
    uint32_t titleId = 0, tipId = 0;
    safeReadU8 (ls + LS_BUILT_OFF,  &built);
    safeReadU8 (ls + LS_ACTIVE_OFF, &active);
    safeReadU8 (ls + LS_DONE_OFF,   &done);
    safeReadU32(ls + LS_TITLE_ID_OFF, &titleId);
    safeReadU32(ls + LS_TIP_ID_OFF,   &tipId);

    // A live loading screen: hush the focus announcer so it can't talk over the story.
    g_lsSuppressFocus = (active != 0);

    if (!built) return;                       // still assembling; wait for the resolved text
    uint64_t key = ((uint64_t)titleId << 32) | tipId;
    DWORD now = GetTickCount();

    if (key != g_lsKey && (titleId || tipId)) {
        // New screen. Read the localized title + tip C-strings the game already resolved.
        char title[MAILBOX_SZ] = {0}, tip[MAILBOX_SZ] = {0};
        char utter[MAILBOX_SZ];
        lsReadText(ls, title, sizeof title, tip, sizeof tip);
        lsComposeLine(title, tip, utter, sizeof utter);

        g_lsKey = key;
        g_lsDoneLatch = false;
        g_lsContinuePending = false;
        logLine("loadingscreen titleId=%u tipId=%u done=%d title=\"%s\" tip=\"%.120s\"",
                titleId, tipId, done, title, tip);
        parkCursor("loading screen");
        if (title[0] || tip[0]) {
            postSpeech(utter);
            g_lsSpokeAt = now;
        }
    }

    if (done && !g_lsDoneLatch) {
        g_lsContinuePending = true;
        if (!g_hasPending) {
            char raw[256] = {0}, prompt[256] = {0};
            if (resolveKey(base, kLoadingContinueKey, raw, sizeof raw))
                stripMarkup(raw, prompt, sizeof prompt);
            if (!prompt[0]) strncpy(prompt, axs(AXS_LS_CONTINUE_FALLBACK), sizeof prompt - 1);
            postSpeech(prompt, /*interrupt=*/false);   // wait politely behind the flavor
            g_lsDoneLatch = true;
            g_lsContinuePending = false;
            logLine("loadingscreen continue (polite): \"%s\"", prompt);
        }
    }
}
