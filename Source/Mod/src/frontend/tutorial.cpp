// frontend/tutorial.cpp -- THE TUTORIAL POPUPS

#include <cstdint>
#include <cstring>
#include <cstdio>
#include "internal.h"

// ---- Tutorial popups (UI::Panel::TutorialPopup) ----
static const uintptr_t TUT_ID_OFF      = 0xd0;       // inline C-string: the popup id (the %s)

// ---- Tutorial popup: read the id, resolve, speak (main thread) ----
static const DWORD TUT_HOLD_MS = 500;
static char  g_tutSpokenId[64] = { 0 };  // id last announced ("" = none)
static DWORD g_tutSeenTick     = 0;      // tick of the most recent detour call (any id)
static uintptr_t g_tutSelf     = 0;
static uint32_t  g_tutAppear   = 0;

static bool tutReadId(uintptr_t self, char* out, int outsz) {
    if (out && outsz) out[0] = 0;
    if (self <= 0x10000) return false;
    if (!safeReadCStr(self + TUT_ID_OFF, out, outsz) || !out[0]) {
        logLine("tutorialpopup self=0x%llx no id@+0xd0", (unsigned long long)self);
        return false;
    }
    int n = 0;
    for (const char* p = out; *p; p++, n++) {
        char c = *p;
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) {
            logLine("tutorialpopup self=0x%llx bad id=\"%s\"", (unsigned long long)self, out);
            return false;
        }
    }
    return n > 0 && n <= 40;
}

static bool tutComposeLine(uintptr_t base, const char* id, char* out, int outsz) {
    char keyT[96], keyD[96], rawT[512], rawD[1536];
    _snprintf(keyT, sizeof keyT, "tutorial_popup_%s_title", id);
    _snprintf(keyD, sizeof keyD, "tutorial_popup_%s_description", id);
    bool haveT = resolveKey(base, keyT, rawT, sizeof rawT);
    bool haveD = resolveKey(base, keyD, rawD, sizeof rawD);

    char title[512] = { 0 }, desc[1536] = { 0 };
    if (haveT) stripMarkup(rawT, title, sizeof title);
    if (haveD) stripMarkup(rawD, desc,  sizeof desc);

    if (haveT && haveD)      _snprintf(out, outsz, axEndsSentence(title, strlen(title)) ? "%s %s" : "%s. %s",
                                       title, desc);
    else if (haveT)          _snprintf(out, outsz, "%s", title);
    else if (haveD)          _snprintf(out, outsz, "%s", desc);
    else { out[0] = 0; return false; }
    out[outsz - 1] = 0;
    return true;
}

static void tutAppendCloseHelp(uintptr_t base, char* buf, int bufsz) {
    bool ctrl = g_inputIsController;
    const char* key = axs(!ctrl ? AXS_KEY_ESCAPE
                                : (altControllerGlyphs(base) ? AXS_BTN_CIRCLE : AXS_BTN_B));
    const char* fmt = axs(AXS_TUT_CLOSE_HELP_FMT);
    if (!fmt[0] || !buf[0]) return;
    char help[96];
    _snprintf(help, sizeof help, fmt, key);
    help[sizeof help - 1] = 0;
    axAppendHintText(buf, bufsz, help);    // supplies the missing final period, as before
}

bool tutPopupActive() {
    return g_tutSelf && g_tutSeenTick && (GetTickCount() - g_tutSeenTick) < TUT_GAP_MS;
}

void announceTutorialPopup(uintptr_t self) {
    DWORD now = GetTickCount();
    DWORD gap = now - g_tutSeenTick;     // time since the popup hook last fired
    g_tutSeenTick = now;
    g_tutSelf     = self;
    if (!g_enabled || self <= 0x10000) return;

    char id[64];
    if (!tutReadId(self, id, sizeof id)) return;

    if (strcmp(id, g_tutSpokenId) == 0 && gap < TUT_GAP_MS) return;
    strncpy(g_tutSpokenId, id, sizeof g_tutSpokenId - 1);
    g_tutSpokenId[sizeof g_tutSpokenId - 1] = 0;
    g_tutAppear++;

    char utter[MAILBOX_SZ];
    if (!tutComposeLine(g_base, id, utter, sizeof utter)) {
        _snprintf(utter, sizeof utter, "%s", axs(AXS_TUT_POPUP));
        tutAppendCloseHelp(g_base, utter, sizeof utter);
        logLine("tutorialpopup id=%s UNRESOLVED (keys missed)", id);
        postSpeech(utter, true, SPK_EVENT);
        return;
    }
    tutAppendCloseHelp(g_base, utter, sizeof utter);
    postSpeech(utter, true, SPK_EVENT);
    logLine("tutorialpopup id=%s text=\"%s\"", id, utter);
}

void tutReannounce(uintptr_t base) {
    char id[64], utter[MAILBOX_SZ];
    if (!tutReadId(g_tutSelf, id, sizeof id)) return;
    if (!tutComposeLine(base, id, utter, sizeof utter)) {
        logLine("tutorialpopup: '/' on id=%s, nothing resolves", id);
        _snprintf(utter, sizeof utter, "%s", axs(AXS_TUT_POPUP));
        tutAppendCloseHelp(base, utter, sizeof utter);
        postSpeech(utter);
        return;
    }
    tutAppendCloseHelp(base, utter, sizeof utter);
    logLine("tutorialpopup: '/' re-reading id=%s", id);
    postSpeech(utter);
}

// ---- Escape while a CONFIRM DIALOG sits under the popup: click the popup's own close icon ----
static const int TUT_MAX_CLOSE_ELEMS = 8;
static bool tutClickCloseIcon(uintptr_t base) {
    uintptr_t vb = 0, ve = 0;
    int found = 0;
    float ex = 0, ey = 0;
    if (safeReadPtr(base + VEC_BEGIN_RVA, &vb) && safeReadPtr(base + VEC_END_RVA, &ve) && vb && ve > vb) {
        uintptr_t n = (ve - vb) / ELEM_STRIDE;
        if (n > 4096) n = 4096;
        for (uintptr_t i = 0; i < n; i++) {
            int64_t id = 0;
            if (!safeReadI64(vb + i * ELEM_STRIDE + ELEM_ID_OFF, &id)) continue;
            if ((uint64_t)id != (uint64_t)UI_CLOSE_ELEM_ID) continue;
            if (found == 0) elemPos(vb + i * ELEM_STRIDE, &ex, &ey);
            if (++found >= TUT_MAX_CLOSE_ELEMS) break;
        }
    }
    if (found == 0) {
        logLine("tutorialkey: a confirm dialog underneath, but no 'slct' close icon is on screen");
        return false;
    }
    uint32_t lb[2] = { 0, 0 }, lc[2] = { 0, 0 };
    safeReadU32(base + TUT_LAYOUT_RVA + 0, &lb[0]); safeReadU32(base + TUT_LAYOUT_RVA + 4, &lb[1]);
    safeReadU32(base + TUT_LAYOUT_RVA + TUT_LAYOUT_CLOSE_OFF + 0, &lc[0]);
    safeReadU32(base + TUT_LAYOUT_RVA + TUT_LAYOUT_CLOSE_OFF + 4, &lc[1]);
    float lx = u32AsFloatM(lb[0]) + u32AsFloatM(lc[0]);
    float ly = u32AsFloatM(lb[1]) + u32AsFloatM(lc[1]);
    if (found > 1)
        logLine("⚠ tutorialkey: %d 'slct' elements on screen -- clicking the first, at (%.0f,%.0f)",
                found, ex, ey);

    if (clickQueued()) {                           // the previous press is still landing
        logLine("tutorialkey: a click is already in flight -- swallowing the second Escape");
        return true;
    }
    bool ok = frontEndClickElementId((int64_t)UI_CLOSE_ELEM_ID);
    logLine("tutorialkey: Escape with a confirm dialog underneath -> clicking the popup's close icon "
            "'slct' at (%.0f,%.0f) [layout says (%.0f,%.0f) = base (%.0f,%.0f) + close (%.0f,%.0f)] -> %s",
            ex, ey, lx, ly, u32AsFloatM(lb[0]), u32AsFloatM(lb[1]), u32AsFloatM(lc[0]), u32AsFloatM(lc[1]),
            ok ? "clicked" : "NO CLICK, handing Escape to the game");
    return ok;
}

// ---- THE POPUP READER's rows ----
static MvCursor g_tutCur = { 0xffffffffu, 0 };

static MvRows* tutBuildRows(uintptr_t base, uint32_t* sigOut, int* answersOut) {
    MvRows* r = mvBegin();
    if (sigOut) *sigOut = g_tutAppear;
    if (answersOut) *answersOut = 0;
    char id[64];
    if (!tutReadId(g_tutSelf, id, sizeof id)) return r;
    char keyT[96], keyD[96], rawT[512], rawD[1536];
    _snprintf(keyT, sizeof keyT, "tutorial_popup_%s_title", id);
    _snprintf(keyD, sizeof keyD, "tutorial_popup_%s_description", id);
    bool haveT = resolveKey(base, keyT, rawT, sizeof rawT);
    bool haveD = resolveKey(base, keyD, rawD, sizeof rawD);
    if (haveT) mvText(r, rawT);
    if (haveD) mvText(r, rawD);
    if (!haveT && !haveD) mvTextLine(r, axs(AXS_TUT_POPUP));
    bool dialog = confirmDialogOpen(base);
    if (dialog) {
        uint32_t dsig = 0;
        int answers = cdAppendRowsUnderPopup(base, r, &dsig);
        if (answersOut) *answersOut = answers;
        if (sigOut) *sigOut = g_tutAppear * 2654435761u ^ dsig;
    }
    if (!dialog || feElemOnScreen((int64_t)UI_CLOSE_ELEM_ID)) mvClose(r);
    return r;
}

static uint32_t g_tutCloseTriedAppear = 0xffffffffu;
static DWORD    g_tutCloseTriedTick   = 0;
static const DWORD TUT_CLOSE_SETTLE_MS = TUT_GAP_MS + 200;
static uint32_t g_tutStackDumpedAppear = 0xffffffffu;

static void tutCloseRow(uintptr_t base) {
    if (clickQueued()) { logLine("tutorialkey: Close row while a click is landing -- swallowed"); return; }
    if (confirmDialogOpen(base)) {
        if (tutClickCloseIcon(base)) {
            g_tutCloseTriedAppear = g_tutAppear;      // a following Escape goes to the answers
            g_tutCloseTriedTick   = GetTickCount();
            return;
        }
        logLine("tutorialkey: Close row, dialog underneath, no close icon -- sending Escape anyway");
    }
    if (!mvSendEscape("tutorial")) postSpeech(axs(AXS_ACTION_FAILED));
}

bool routeTutorialKey(uintptr_t base, uint32_t sym, uint16_t mod, uint8_t repeat) {
    bool dialog = confirmDialogOpen(base);
    if (dialog && g_tutStackDumpedAppear != g_tutAppear) {
        g_tutStackDumpedAppear = g_tutAppear;
        feDumpFocusVector("tutorial popup over a confirm dialog");
    }
    if (sym == SDLK_ESCAPE) {
        if (mod & (KMOD_LALT | KMOD_RALT | KMOD_LCTRL | KMOD_RCTRL | KMOD_LSHIFT | KMOD_RSHIFT))
            return false;
        if (!dialog) return false;                     // the game's own Escape works: hand it over
        if (repeat) return true;                       // one click per physical press
        if (clickQueued()) {
            logLine("tutorialkey: a click is already in flight -- swallowing the second Escape");
            return true;
        }
        // First Escape of this appearance: the popup's own close icon (the 09-05 door).
        if (g_tutCloseTriedAppear != g_tutAppear) {
            if (tutClickCloseIcon(base)) {
                g_tutCloseTriedAppear = g_tutAppear;
                g_tutCloseTriedTick   = GetTickCount();
                return true;
            }
        } else if (GetTickCount() - g_tutCloseTriedTick < TUT_CLOSE_SETTLE_MS) {
            return true;                               // the click may still be closing it
        }
        uint32_t sig = 0;
        int answers = 0;
        MvRows* r = tutBuildRows(base, &sig, &answers);
        mvSync(&g_tutCur, sig, r);
        int first = mvFirstNonText(r);
        if (answers > 0 && first >= 0) {
            g_tutCur.row = first;
            logLine("tutorialkey: Escape cannot close the popup over a dialog -> cursor to the "
                    "dialog's first answer (row %d of %d)", first + 1, r->n);
            mvSpeak(&g_tutCur, r, nullptr);
            return true;
        }
        logLine("tutorialkey: Escape over a dialog, no close icon and no answer registered -- "
                "handing Escape to the game");
        return false;                                  // nothing the mod can press: never a trap
    }
    uint32_t sig = 0;
    int answers = 0;
    MvRows* r = tutBuildRows(base, &sig, &answers);
    mvSync(&g_tutCur, sig, r);
    int opt = -1;
    switch (mvKey(&g_tutCur, r, sym, mod, repeat, false, &opt)) {
        case MVK_TEXT:  mvSpeak(&g_tutCur, r, nullptr); return true;
        case MVK_CLOSE: tutCloseRow(base); return true;
        case MVK_OPTION:
            if (opt >= 0 && opt < answers && !cdClickAnswerUnderPopup(opt))
                postSpeech(axs(AXS_ACTION_FAILED));
            return true;
        case MVK_DONE:  return true;
        default:        return false;
    }
}

// ---- Tutorial popup dismissed -> hand the surface underneath back its voice ----
static bool g_tutOnScreen = false;
void checkTutorialDismissed(uintptr_t base) {
    bool on = g_tutSeenTick && (GetTickCount() - g_tutSeenTick) < TUT_GAP_MS;
    if (on == g_tutOnScreen) return;
    g_tutOnScreen = on;
    if (on) return;                      // just appeared: it speaks for itself
    g_tutSelf = 0;

    axModalClosed(base, "tutorial");
}
