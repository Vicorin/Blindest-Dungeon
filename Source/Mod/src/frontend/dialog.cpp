// frontend/dialog.cpp -- the fourth frontend slice

#include <cstdint>
#include <cstring>
#include <cstdio>
#include "internal.h"

// ---- Difficulty / game-mode ConfirmDialog (Radiant / Darkest / Stygian) ----

static const uintptr_t FE_MODE_ARRAY_OFF   = 0x2640;
static const uintptr_t MODE_STRIDE         = 0x548;      // bytes per mode entry
static const uintptr_t MODE_NAME_OFF       = 0x00;       // mode-name C-string (<=0x80)
static const char* kModeQuestionKey = "str_select_campaign";

// ---- Generic ConfirmDialog reading (any yes/no/confirm popup) ----
static const uintptr_t CD_ENTRIES_BEGIN_OFF = 0x00;     // System+0: entries vector begin
static const uintptr_t CD_ENTRIES_END_OFF   = 0x08;     // System+8: entries vector end
static const uintptr_t CD_ANSWERS_BEGIN_OFF = 0x208;    // Entry+0x208: answers vector begin
static const uintptr_t CD_ANSWERS_END_OFF   = 0x210;    // Entry+0x210: answers vector end
static const uintptr_t CD_ANSWER_STRIDE     = 0x148;    // bytes per answer
static const uintptr_t CD_ANSWER_TEXT_OFF   = 0x00;     // answer: display-text C-string (<=0x40)
static const uintptr_t CD_ANSWER_BTN_OFF    = 0x140;    // answer: int button-assignment code
                                                        // (ShowEntry checks it vs BUTTON_Primary2)
static const uintptr_t CD_QUESTION_OFF      = 0x2a0;
static const uintptr_t CD_QUESTION_TEXT_OFF = 0x08;     // Entry+0x08: question, plain C-string
static const uintptr_t CD_QUESTION_TEXT_MAX = 0x200;

// ---- The ONE dialog whose own text cannot be spoken ----
const char* kActFailKey      = "activity_assign_attempt_fail";
const char* kActFailQuirkKey = "activity_assign_attempt_fail_has_quirk_format";
char g_bldActWhy[512] = {0};

bool resolveModeDialog(uintptr_t base, uintptr_t display, int64_t id,
                              bool announceQuestion, char* out, int outsz) {
    uint32_t cc  = (uint32_t)(uint64_t)id;
    uint32_t idx = cc - CONFIRM_ANSWER_BASE;
    if (idx >= (uint32_t)CONFIRM_ANSWER_MAX) return false;   // not a dialog answer
    int i = (int)idx;

    uint32_t sub = 0xffffffffu;
    safeReadU32(display + FE_NAMING_SUB_OFF, &sub);
    if (sub != FE_NAMING_SUB_MODEDIALOG) {
        logLine("modedialog: sub-state %d != %d -> not the mode dialog, deferring to generic",
                (int)sub, FE_NAMING_SUB_MODEDIALOG);
        return false;
    }

    char modeName[128]; modeName[0] = 0;
    char desc[512];     desc[0] = 0;
    uintptr_t modesBase = 0;
    if (!safeReadPtr(display + FE_MODE_ARRAY_OFF, &modesBase) || modesBase <= 0x10000) return false;
    uintptr_t entry = modesBase + (uintptr_t)i * MODE_STRIDE;
    if (!safeReadCStr(entry + MODE_NAME_OFF, modeName, sizeof modeName) || !modeName[0]) return false;
    char key[192];
    _snprintf(key, sizeof key, "str_game_mode_select_%s_tooltip", modeName);
    key[sizeof key - 1] = 0;
    if (!resolveKey(base, key, desc, sizeof desc)) return false;   // not the game-mode dialog

    char labelBuf[128];
    char lkey[192];
    _snprintf(lkey, sizeof lkey, "fe_flow_mode_%s_confirm", modeName);
    lkey[sizeof lkey - 1] = 0;
    if (!resolveKey(base, lkey, labelBuf, sizeof labelBuf) || !labelBuf[0]) {
        logLine("modedialog: %s did not resolve -> raw mode name", lkey);
        strncpy(labelBuf, modeName, sizeof labelBuf - 1);
        labelBuf[sizeof labelBuf - 1] = 0;
    }
    const char* label = labelBuf;

    (void)announceQuestion;

    _snprintf(out, outsz, "%s. %s", label, desc);
    out[outsz - 1] = 0;
    logLine("modedialog id=0x%llx idx=%d label=\"%s\" mode=\"%s\" question=%d",
            (unsigned long long)id, i, label, modeName, announceQuestion ? 1 : 0);
    return true;
}

static const char* kOptBackSaveKey     = "menu_options_back_confirm_dialog_answer_save";
static const char* kOptBackQuestionKey = "menu_options_back_confirm_dialog_question";

static bool optBackTitle(uintptr_t base, uintptr_t aBegin, int count,
                         char* out, int outsz) {
    if (count < 1) return false;
    char a0[128]; a0[0] = 0;
    if (!safeReadCStr(aBegin + CD_ANSWER_TEXT_OFF, a0, sizeof a0) || !a0[0]) return false;
    char save[128];
    if (!resolveKey(base, kOptBackSaveKey, save, sizeof save)) return false;
    if (_stricmp(a0, save) != 0) return false;   // answer[0] isn't SAVE -> not this dialog
    if (!resolveKey(base, kOptBackQuestionKey, out, outsz)) {
        strncpy(out, axs(AXS_DLG_SAVE_CHANGES_FALLBACK), outsz - 1); out[outsz - 1] = 0;  // fallback
    }
    return true;
}

static bool confirmDialogQuestion(uintptr_t base, uintptr_t entry, char* out, int outsz);

bool resolveGenericConfirm(uintptr_t base, int64_t id, bool firstEntry,
                                  char* out, int outsz) {
    uint32_t idx = (uint32_t)(uint64_t)id - CONFIRM_ANSWER_BASE;
    if (idx >= (uint32_t)CONFIRM_ANSWER_MAX) return false;
    int i = (int)idx;

    uintptr_t sys = 0, entry = 0, eEnd = 0, aBegin = 0, aEnd = 0;
    if (!safeReadPtr(base + CD_SYSTEM_PTR_RVA, &sys) || sys <= 0x10000) return false;
    if (!safeReadPtr(sys + CD_ENTRIES_BEGIN_OFF, &entry) || entry <= 0x10000) return false;
    if (!safeReadPtr(sys + CD_ENTRIES_END_OFF, &eEnd) || eEnd <= entry) return false;  // no entries
    if (!safeReadPtr(entry + CD_ANSWERS_BEGIN_OFF, &aBegin) || aBegin <= 0x10000) return false;
    if (!safeReadPtr(entry + CD_ANSWERS_END_OFF, &aEnd) || aEnd <= aBegin) return false;
    int count = (int)((aEnd - aBegin) / CD_ANSWER_STRIDE);
    if (i < 0 || i >= count) return false;

    char raw[128]; raw[0] = 0;
    uintptr_t ans = aBegin + (uintptr_t)i * CD_ANSWER_STRIDE;
    if (!safeReadCStr(ans + CD_ANSWER_TEXT_OFF, raw, sizeof raw) || !raw[0]) return false;

    char label[512];
    if (resolveKey(base, raw, label, sizeof label))
        { /* label holds the resolved text */ }
    else
        { strncpy(label, raw, sizeof label - 1); label[sizeof label - 1] = 0; }

    (void)firstEntry;

    _snprintf(out, outsz, "%s", label);
    out[outsz - 1] = 0;

    logLine("confirm id=0x%llx idx=%d count=%d raw=\"%s\" text=\"%s\"",
            (unsigned long long)id, i, count, raw, out);
    return true;
}

static const int OPT_ALT_GLYPHS_IDX = 5;
bool altControllerGlyphs(uintptr_t base) {
    int32_t v[2] = { 0, 0 };
    int n = readOptionValue(base, OPT_ALT_GLYPHS_IDX, v, 2);
    return n > 0 && v[0] > 0;
}

// ---- Event-driven ConfirmDialog reader (focus-less button-prompt popups) ----
static int confirmDialogAnswers(uintptr_t base, uintptr_t* entryOut, uintptr_t* aBeginOut) {
    if (entryOut)  *entryOut  = 0;
    if (aBeginOut) *aBeginOut = 0;
    uintptr_t sys = 0, entry = 0, eEnd = 0, aBegin = 0, aEnd = 0;
    if (!safeReadPtr(base + CD_SYSTEM_PTR_RVA, &sys) || sys <= 0x10000) return 0;
    if (!safeReadPtr(sys + CD_ENTRIES_BEGIN_OFF, &entry) || entry <= 0x10000) return 0;
    if (!safeReadPtr(sys + CD_ENTRIES_END_OFF, &eEnd) || eEnd <= entry) return 0;
    if (!safeReadPtr(entry + CD_ANSWERS_BEGIN_OFF, &aBegin) || aBegin <= 0x10000) return 0;
    if (!safeReadPtr(entry + CD_ANSWERS_END_OFF, &aEnd) || aEnd <= aBegin) return 0;
    uintptr_t span = aEnd - aBegin;
    if (span > (uintptr_t)CD_ANSWER_STRIDE * 16) return 0;      // wild length = wrong read
    if (entryOut)  *entryOut  = entry;
    if (aBeginOut) *aBeginOut = aBegin;
    return (int)(span / CD_ANSWER_STRIDE);
}
bool confirmDialogOpen(uintptr_t base) { return confirmDialogAnswers(base, nullptr, nullptr) > 0; }
int confirmDialogAnswerCount(uintptr_t base) { return confirmDialogAnswers(base, nullptr, nullptr); }

uintptr_t confirmDialogEntry(uintptr_t base) {
    uintptr_t entry = 0;
    return confirmDialogAnswers(base, &entry, nullptr) > 0 ? entry : 0;
}

static bool cdCleanText(char* raw, char* out, int outsz) {
    abStripMarkup(raw);                            // remove `<c>..</c>` / `{...}` runtime markup
    int n = 0;
    for (; raw[n]; n++) {
        unsigned char c = (unsigned char)raw[n];
        if (c < 0x20 && c != '\n' && c != '\t') return false;
    }
    if (n < 2) return false;
    strncpy(out, raw, outsz - 1);
    out[outsz - 1] = 0;
    return true;
}

static bool cdReadStdString(uintptr_t addr, char* out, int outsz) {
    uintptr_t cap = 0, size = 0;
    if (!safeReadPtr(addr + 0x18, &cap) || !safeReadPtr(addr + 0x10, &size)) return false;
    if (size == 0 || size > (uintptr_t)CD_QUESTION_TEXT_MAX) return false;
    uintptr_t dataAddr = addr;
    if (cap >= 0x10) { if (!safeReadPtr(addr, &dataAddr) || dataAddr <= 0x10000) return false; }
    return safeReadCStr(dataAddr, out, outsz) && out[0] != 0;
}

static bool confirmDialogQuestion(uintptr_t base, uintptr_t entry, char* out, int outsz) {
    if (out && outsz) out[0] = 0;
    if (!entry || !out || outsz <= 1) return false;

    char raw[CD_QUESTION_TEXT_MAX + 4];
    // 1. the pinned plain-text slot (standard dialogs).
    raw[0] = 0;
    if (safeReadCStr(entry + CD_QUESTION_TEXT_OFF, raw, sizeof raw) &&
        cdCleanText(raw, out, outsz)) return true;
    raw[0] = 0;
    if (cdReadStdString(entry + CD_QUESTION_TEXT_OFF, raw, sizeof raw) &&
        cdCleanText(raw, out, outsz)) { logLine("cd-question: read as std::string @+0x08"); return true; }
    // 3. a std::string at the Entry head.
    raw[0] = 0;
    if (cdReadStdString(entry, raw, sizeof raw) &&
        cdCleanText(raw, out, outsz)) { logLine("cd-question: read as std::string @+0x00"); return true; }
    (void)base;
    return false;
}

static void cdDumpEntry(uintptr_t base) {
    static int   dumps = 0;
    static uint32_t lastKey = 0;
    if (!axDebugLogEnabled()) return;
    uintptr_t sys = 0, begin = 0, end = 0;
    if (!safeReadPtr(base + CD_SYSTEM_PTR_RVA, &sys) || sys <= 0x10000) return;
    if (!safeReadPtr(sys + CD_ENTRIES_BEGIN_OFF, &begin) || begin <= 0x10000) return;
    if (!safeReadPtr(sys + CD_ENTRIES_END_OFF, &end) || end <= begin) return;   // no entries
    uint32_t key = 2166136261u;
    for (int i = 0; i < 0x30; i++) { uint8_t b = 0; safeReadU8(begin + (uintptr_t)i, &b); key = (key ^ b) * 16777619u; }
    if (key == lastKey) return;                 // same dialog still up: don't re-dump
    lastKey = key;
    if (dumps >= 8) return;
    dumps++;
    logLine("cd-dump: system=0x%llx entries begin=0x%llx end=0x%llx span=%lld",
            (unsigned long long)sys, (unsigned long long)begin, (unsigned long long)end,
            (long long)(end > begin ? end - begin : 0));
    for (int row = 0; row < 6; row++) {
        char hex[80]; char asc[24]; int hp = 0;
        for (int i = 0; i < 16; i++) {
            uint8_t b = 0;
            safeReadU8(begin + (uintptr_t)(row * 16 + i), &b);
            hp += _snprintf(hex + hp, sizeof hex - hp, "%02x ", b);
            asc[i] = (b >= 0x20 && b < 0x7f) ? (char)b : '.';
        }
        asc[16] = 0;
        logLine("cd-dump +0x%02x: %s |%s|", row * 16, hex, asc);
    }
}

// ---- The whole readout, composed from LIVE state ----
static bool cdComposeLine(uintptr_t base, char* out, int outsz) {
    if (!out || outsz <= 0) return false;
    out[0] = 0;

    uintptr_t entry = 0, aBegin = 0;
    int count = confirmDialogAnswers(base, &entry, &aBegin);
    if (count <= 0) return false;

    char labels[4][128]; int btn[4] = { -1, -1, -1, -1 }; int got = 0;
    for (int i = 0; i < count && got < 4; i++) {
        char raw[128]; raw[0] = 0;
        uintptr_t ans = aBegin + (uintptr_t)i * CD_ANSWER_STRIDE;
        if (!safeReadCStr(ans + CD_ANSWER_TEXT_OFF, raw, sizeof raw) || !raw[0]) continue;
        if (!resolveKey(base, raw, labels[got], sizeof labels[got])) {
            strncpy(labels[got], raw, sizeof labels[got] - 1);
            labels[got][sizeof labels[got] - 1] = 0;
        }
        uint32_t b = 0xffffffffu; safeReadU32(ans + CD_ANSWER_BTN_OFF, &b); btn[got] = (int)b;
        got++;
    }
    if (got == 0) return false;

    char title[600];
    char actHdr[256];
    char qraw[CD_QUESTION_TEXT_MAX + 4]; qraw[0] = 0;
    safeReadCStr(entry + CD_QUESTION_TEXT_OFF, qraw, sizeof qraw);
    bool actFail = g_bldActWhy[0] &&
                   resolveKey(base, kActFailKey, actHdr, sizeof actHdr) && actHdr[0] &&
                   strncmp(qraw, actHdr, strlen(actHdr)) == 0;
    if (actFail) {
        _snprintf(out, outsz, "%s ", g_bldActWhy);
        out[outsz - 1] = 0;
        logLine("confirmpopup: activity-assign rejection — speaking the composed reason");
    } else if (confirmDialogQuestion(base, entry, title, sizeof title) ||
               optBackTitle(base, aBegin, count, title, sizeof title)) {
        _snprintf(out, outsz, "%s ", title);
        out[outsz - 1] = 0;
    }
    bool ctrl = g_inputIsController;
    bool altGlyphs = ctrl && altControllerGlyphs(base);   // PlayStation-style prompts
    const char* accept = axs(!ctrl ? AXS_KEY_ENTER  : (altGlyphs ? AXS_BTN_CROSS  : AXS_BTN_A));
    const char* cancel = axs(!ctrl ? AXS_KEY_ESCAPE : (altGlyphs ? AXS_BTN_CIRCLE : AXS_BTN_B));
    for (int i = 0; i < got; i++) {
        char seg[224];
        if (got == 2) {
            const char* g = (i == 0) ? accept : cancel;
            _snprintf(seg, sizeof seg, "%s%s, %s.", i ? " " : "", g, labels[i]);
        } else if (i == 0) {
            _snprintf(seg, sizeof seg, "%s", labels[i]);
        } else if (i == got - 1) {
            _snprintf(seg, sizeof seg, ", %s %s", axs(AXS_WORD_OR), labels[i]);
        } else {
            _snprintf(seg, sizeof seg, ", %s", labels[i]);
        }
        seg[sizeof seg - 1] = 0;
        strncat(out, seg, (size_t)outsz - strlen(out) - 1);
    }

    logLine("confirmpopup dev=%s alt=%d entry=0x%llx count=%d got=%d btn0=%d btn1=%d text=\"%s\"",
            ctrl ? "ctrl" : "kbd", altGlyphs ? 1 : 0,
            (unsigned long long)entry, count, got, btn[0], btn[1], out);
    return out[0] != 0;
}

static bool cdAnswersOnScreen(int count);

static uintptr_t g_cdSeenEntry   = 0;
static uint32_t  g_cdSeenTag     = 0;
static DWORD     g_cdSeenAt      = 0;
static bool      g_cdSpoken      = false;
static bool      g_cdKeysWarned  = false;

static const DWORD CD_TEXT_GRACE_MS = 300;
static const DWORD CD_KEYS_GRACE_MS = 1200;

static void cdWarnNoKeyboard(const char* who) {
    if (g_cdKeysWarned) return;
    g_cdKeysWarned = true;
    logLine("confirmpopup: no answer element in the live focus registry (%s) -- correcting the "
            "keys the readout named", who);
    postSpeech(axs(AXS_DLG_NO_KEYBOARD), false);
}

static bool g_cdCancelOwed = false;

static bool cdDeferCancel(const char* where) {
    if ((DWORD)(GetTickCount() - g_cdSeenAt) >= CD_KEYS_GRACE_MS) return false;
    if (!g_cdCancelOwed)
        logLine("dialogkey: Escape claimed, cancel owed until the answer registers (%s)", where);
    g_cdCancelOwed = true;
    return true;
}

void checkConfirmDialog(uintptr_t base) {
    int64_t id = 0; safeReadI64(base + FOCUS_ID_RVA, &id);

    uintptr_t entry = 0;
    int count = confirmDialogAnswers(base, &entry, nullptr);

    if (count > 0) cdDumpEntry(base);

    (void)id;
    if (count <= 0) {
        g_cdSeenEntry = 0; g_cdSeenTag = 0; g_cdSpoken = false; g_cdKeysWarned = false;
        g_cdCancelOwed = false;
        return;
    }

    char qraw[CD_QUESTION_TEXT_MAX + 4]; qraw[0] = 0;
    safeReadCStr(entry + CD_QUESTION_TEXT_OFF, qraw, sizeof qraw);
    uint32_t tag = (uint32_t)count * 2654435761u;
    for (const char* p = qraw; *p; p++) tag = tag * 31u + (unsigned char)*p;

    DWORD now = GetTickCount();
    if (entry != g_cdSeenEntry || tag != g_cdSeenTag) {      // a dialog this has not met yet
        g_cdSeenEntry  = entry;
        g_cdSeenTag    = tag;
        g_cdSeenAt     = now;
        g_cdSpoken     = false;
        g_cdKeysWarned = false;
        g_cdCancelOwed = false;
    }

    if (g_cdCancelOwed) {
        if ((DWORD)(now - g_cdSeenAt) >= CD_KEYS_GRACE_MS) {
            g_cdCancelOwed = false;
            logLine("confirmpopup: deferred cancel expired -- the answer element never registered");
        } else if (count == 2 && !clickQueued() &&
                   feElemOnScreen((int64_t)(CONFIRM_ANSWER_BASE + 1)) &&
                   frontEndClickElementId((int64_t)(CONFIRM_ANSWER_BASE + 1))) {
            g_cdCancelOwed = false;
            logLine("confirmpopup: deferred cancel paid -- clicked answer 1 of %d", count);
        }
    }

    if (!g_cdSpoken) {
        char out[900];
        if (cdComposeLine(base, out, sizeof out)) {
            g_cdSpoken = true;
            postSpeech(out);
        } else if ((DWORD)(now - g_cdSeenAt) >= CD_TEXT_GRACE_MS) {
            g_cdSpoken = true;
            logLine("confirmpopup: entry=0x%llx count=%d -- not one answer label would read; "
                    "naming the dialog anyway", (unsigned long long)entry, count);
            postSpeech(axs(AXS_DLG_PRESENT_FALLBACK));
        }
        return;
    }

    if (!g_cdKeysWarned) {
        if (cdAnswersOnScreen(count))
            g_cdKeysWarned = true;                                   // the promise was good
        else if ((DWORD)(now - g_cdSeenAt) >= CD_KEYS_GRACE_MS)
            cdWarnNoKeyboard("no answer registered within the grace");
    }
}

void cdReannounce(uintptr_t base) {
    char out[900];
    if (!cdComposeLine(base, out, sizeof out)) return;
    logLine("confirmpopup: re-announcing on request");
    postSpeech(out);
}

bool axIsDialog() {
    if (!confirmDialogOpen(g_base)) return false;
    if (rrDisplay(g_base) != 0) return false;
    if (g_pauseOpen) return false;
    if (g_inPauseMenu) {
        if (frontEndDisplay(g_base) != 0) return false;
        int count = confirmDialogAnswers(g_base, nullptr, nullptr);
        return cdAnswersOnScreen(count);
    }
    return true;
}

// ---- ConfirmDialog keys: Enter confirms, Escape cancels ----
static bool cdAnswersOnScreen(int count) {
    if (count > CONFIRM_ANSWER_MAX) count = CONFIRM_ANSWER_MAX;
    for (int i = 0; i < count; i++)
        if (feElemOnScreen((int64_t)(CONFIRM_ANSWER_BASE + (uint32_t)i))) return true;
    return false;
}

bool routeDialogKey(uintptr_t base, uint32_t sym, uint16_t mod, uint8_t repeat) {
    (void)base;
    if (mod & (KMOD_LALT | KMOD_RALT)) return false;

    int count = confirmDialogAnswers(g_base, nullptr, nullptr);
    if (count <= 0) return false;                       // no dialog -> nothing of ours

    static bool s_phantomLogged = false;
    if (!cdAnswersOnScreen(count)) {
        if (!s_phantomLogged) {
            s_phantomLogged = true;
            logLine("dialogkey: %d answers reported, none on screen -- passing keys through "
                    "(see CD_ENTRIES_END_OFF)", count);
            feDumpFocusVector("confirm dialog reported, no answer element on screen");
        }
        if (sym == SDLK_ESCAPE && count == 2 && cdDeferCancel("no answer registered yet"))
            return true;
        if (sym == SDLK_RETURN || sym == SDLK_KP_ENTER || sym == SDLK_ESCAPE ||
            sym == SDLK_UP || sym == SDLK_DOWN || sym == SDLK_LEFT || sym == SDLK_RIGHT)
            cdWarnNoKeyboard("a key the readout named was pressed");
        return false;
    }
    s_phantomLogged = false;

    switch (sym) {
        case SDLK_UP: case SDLK_DOWN: case SDLK_LEFT: case SDLK_RIGHT: {
            if (repeat) return true;
            frontEndFocusMove(sym == SDLK_UP ? 0 : sym == SDLK_DOWN ? 1 : sym == SDLK_LEFT ? 2 : 3);
            return true;
        }
        default: break;
    }

    int want = -1;
    if (sym == SDLK_RETURN || sym == SDLK_KP_ENTER) {
        // The focused answer if focus is on one; else the accept role (answer 0).
        int64_t id = feCurrentFocusId();
        uint32_t idx = (uint32_t)(uint64_t)id - CONFIRM_ANSWER_BASE;
        want = (idx < (uint32_t)count) ? (int)idx : 0;
    } else if (sym == SDLK_ESCAPE) {
        if (count != 2) return false;
        want = 1;
    } else {
        return false;                                   // not ours
    }
    if (repeat) return true;                            // one activation per physical press

    if (clickQueued()) {
        logLine("dialogkey: a click is already in flight -- swallowing the second press");
        return true;
    }

    int64_t target = (int64_t)(CONFIRM_ANSWER_BASE + (uint32_t)want);
    if (!feElemOnScreen(target)) {
        if (sym == SDLK_ESCAPE && cdDeferCancel("cancel answer not registered yet"))
            return true;
        logLine("dialogkey: answer %d of %d is not on screen -- handing %s to the game",
                want, count, sym == SDLK_ESCAPE ? "Escape" : "Enter");
        return false;
    }

    bool ok = frontEndClickElementId(target);
    logLine("dialogkey: %s -> answer %d of %d -> %s",
            sym == SDLK_ESCAPE ? "Escape" : "Enter", want, count, ok ? "clicked" : "NO ELEMENT");
    if (!ok) {
        logLine("dialogkey: the click did not land -- handing %s to the game instead",
                sym == SDLK_ESCAPE ? "Escape" : "Enter");
        return false;
    }
    return true;
}
