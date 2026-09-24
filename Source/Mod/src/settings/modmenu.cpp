// settings/modmenu.cpp -- the F10 MOD SETTINGS MENU.

#include <cstdio>
#include <cstring>
#include "internal.h"

bool g_smActive = false;

static const int SM_LEVEL_ANN   = 3;
static const int SM_LEVEL_SPEECH = 4;
static const int SM_LEVEL_SAPI  = 5;
static const int SM_LEVEL_LISTS = SM_LEVEL_ANN;
static int g_smLevel = 0;
static int g_smCat = 0;
static int g_smSub = 0;            // 0 Town, 1 Dungeon
static int g_smIdx = 0;            // cursor within the region list
static int g_smAnnIdx = 0;         // cursor within the Announcements list
static int g_smSpIdx = 0;          // cursor within the Speech list
static int g_smSapiIdx = 0;        // cursor within SAPI configuration
static int g_smMode = 0;
static int g_smSaveSel = 0;        // save question: 0 Yes, 1 No

static int g_smMap[128];
static int g_smMapN = 0;

static const int SM_CAT_ANNOUNCE = 0;
static const int SM_CAT_SPEECH   = 1;
static const int SM_CAT_CONTROLS = 2;
static const int SM_CAT_UPDATES  = 3;
static const int SM_CAT_DEBUG    = 4;   // inline toggle row at level 0
static const int SM_CATS = 5;
static const int SM_SUBS = 2;

// ---- The value lists (levels 3..5) ----
static const int SM_VALUES_MAX = 4;
struct SmValRow {
    AxStrId name;
    int     count;
    int   (*countFn)();                    // live value count, or null
    int   (*get)();
    void  (*set)(int);                     // null = submenu row
    int     child;                         // the level a submenu row opens
    AxStrId valueName[SM_VALUES_MAX];      // static value words (valueFn null)
    void  (*valueFn)(int i, char* out, int outsz);
    AxStrId outcome[SM_VALUES_MAX];        // static outcome lines (outcomeFn null)
    void  (*outcomeFn)(int i, char* out, int outsz);
};
struct SmValList {
    AxStrId header;                        // "%d settings." format spoken on entry
    const SmValRow* rows;
    int   n;
    int*  cursor;
    int   parent;                          // the level Escape returns to
};

static int  smSubsGet()      { return axReadSubtitles() ? 0 : 1; }
static void smSubsSet(int v) { axSetReadSubtitles(v == 0); }
static const SmValRow kSmAnn[] = {
    { AXS_SM_ANN_SUBTITLES, 2, nullptr, smSubsGet, smSubsSet, 0,
      { AXS_VALUE_ON, AXS_VALUE_OFF }, nullptr,
      { AXS_SM_ANN_SUBS_ON, AXS_SM_ANN_SUBS_OFF }, nullptr },
    { AXS_SM_ANN_BARKS, AX_BARKS_MODES, nullptr, axBarksMode, axSetBarksMode, 0,
      { AXS_VALUE_ON, AXS_SM_ANN_BARKS_V_DUNGEON, AXS_SM_ANN_BARKS_V_TOWN, AXS_VALUE_OFF }, nullptr,
      { AXS_SM_ANN_BARKS_ON, AXS_SM_ANN_BARKS_DUNGEON, AXS_SM_ANN_BARKS_TOWN, AXS_SM_ANN_BARKS_OFF }, nullptr },
};
static const int SM_ANN_N = (int)(sizeof kSmAnn / sizeof kSmAnn[0]);

// ---- Speech (level 4) ----
static void smHandlerSet(int v) {
    axSetSpeechHandler(v, axs(AXS_SM_SP_SWITCHED_FMT), axs(AXS_SM_SP_FALLBACK_FMT));
}
static int  smBackendCount() { return 1 + axPrismBackendCount(); }
static int  smBackendGet() {
    char cur[64]; axPrismBackendSetting(cur, sizeof cur);
    if (!cur[0]) return 0;
    for (int i = 0; i < axPrismBackendCount(); i++)
        if (strcmp(axPrismBackendName(i), cur) == 0) return i + 1;
    return 0;
}
static void smBackendSet(int v) {
    axSetPrismBackend(v == 0 ? "" : axPrismBackendName(v - 1),
                      axs(AXS_SM_SP_SWITCHED_FMT), axs(AXS_SM_SP_FALLBACK_FMT));
}
static void smBackendValue(int i, char* out, int outsz) {
    _snprintf(out, outsz, "%s", i == 0 ? axs(AXS_VALUE_AUTO) : axPrismBackendName(i - 1));
    out[outsz - 1] = 0;
}
static void smBackendOutcome(int i, char* out, int outsz) {
    char main[192];
    if (i == 0) _snprintf(main, sizeof main, "%s", axs(AXS_SM_SP_BACKEND_AUTO));
    else        _snprintf(main, sizeof main, axs(AXS_SM_SP_BACKEND_SET_FMT), axPrismBackendName(i - 1));
    main[sizeof main - 1] = 0;
    if (axSpeechHandler() != AX_SPEECH_PRISM) _snprintf(out, outsz, "%s %s", main, axs(AXS_SM_SP_BACKEND_NOTE));
    else                                      _snprintf(out, outsz, "%s", main);
    out[outsz - 1] = 0;
}
static const SmValRow kSmSpeech[] = {
    { AXS_SM_SP_HANDLER, AX_SPEECH_MODES, nullptr, axSpeechHandler, smHandlerSet, 0,
      { AXS_VALUE_AUTO, AXS_SM_SP_V_PRISM, AXS_SM_SP_V_SAPI }, nullptr,
      { AXS_SM_SP_HANDLER_AUTO, AXS_SM_SP_HANDLER_PRISM, AXS_SM_SP_HANDLER_SAPI }, nullptr },
    { AXS_SM_SP_BACKEND, 0, smBackendCount, smBackendGet, smBackendSet, 0,
      { }, smBackendValue, { }, smBackendOutcome },
    { AXS_SM_SP_SAPI, 0, nullptr, nullptr, nullptr, SM_LEVEL_SAPI, { }, nullptr, { }, nullptr },
};
static const int SM_SPEECH_N = (int)(sizeof kSmSpeech / sizeof kSmSpeech[0]);

// ---- SAPI configuration (level 5) ----
static int smVoiceCount() { int n = axSapiVoiceCount(); return n > 0 ? n : 1; }
static int smVoiceGet() {
    int n = axSapiVoiceCount();
    if (n <= 0) return 0;
    char want[128]; axSapiVoiceSetting(want, sizeof want);
    if (want[0]) {
        char nm[128];
        for (int i = 0; i < n; i++) { axSapiVoiceName(i, nm, sizeof nm); if (strcmp(nm, want) == 0) return i; }
    }
    int live = axSapiLiveVoice();
    return (live >= 0 && live < n) ? live : 0;
}
static void smVoiceSet(int v) {
    if (axSapiVoiceCount() <= 0) return;        // nothing to choose from; the outcome says so
    char nm[128]; axSapiVoiceName(v, nm, sizeof nm);
    axSetSapiVoice(nm);
}
static void smVoiceValue(int i, char* out, int outsz) {
    if (axSapiVoiceCount() <= 0) {
        char want[128]; axSapiVoiceSetting(want, sizeof want);
        _snprintf(out, outsz, "%s", want[0] ? want : axs(AXS_SM_SP_V_DEFAULT));
    } else {
        axSapiVoiceName(i, out, outsz);
    }
    out[outsz - 1] = 0;
}
static void smSapiOutcome(int, char* out, int outsz) {
    _snprintf(out, outsz, "%s", axSapiLive() ? "" : axs(AXS_SM_SP_SAPI_NOTE));
    out[outsz - 1] = 0;
}
static int smPctIndex(int setting, int live, int dflt) {
    int pct = setting >= 0 ? setting : live >= 0 ? live : dflt;
    int i = (pct + 5) / 10;
    return i < 0 ? 0 : i > 10 ? 10 : i;
}
static int  smRateGet()      { return smPctIndex(axSapiRateSetting(),   axSapiLiveRate(),   50); }
static void smRateSet(int v) { axSetSapiRate(v * 10); }
static int  smVolGet()       { return smPctIndex(axSapiVolumeSetting(), axSapiLiveVolume(), 100); }
static void smVolSet(int v)  { axSetSapiVolume(v * 10); }
static void smPctValue(int i, char* out, int outsz) {
    _snprintf(out, outsz, axs(AXS_SM_SP_PCT_FMT), i * 10);
    out[outsz - 1] = 0;
}
static const SmValRow kSmSapi[] = {
    { AXS_SM_SP_VOICE,  0,  smVoiceCount, smVoiceGet, smVoiceSet, 0, { }, smVoiceValue, { }, smSapiOutcome },
    { AXS_SM_SP_RATE,   11, nullptr,      smRateGet,  smRateSet,  0, { }, smPctValue,   { }, smSapiOutcome },
    { AXS_SM_SP_VOLUME, 11, nullptr,      smVolGet,   smVolSet,   0, { }, smPctValue,   { }, smSapiOutcome },
};
static const int SM_SAPI_N = (int)(sizeof kSmSapi / sizeof kSmSapi[0]);

static const SmValList kSmLists[] = {
    { AXS_SM_ANN_HEADER,  kSmAnn,    SM_ANN_N,    &g_smAnnIdx,  0 },               // level 3
    { AXS_SM_SP_HEADER,   kSmSpeech, SM_SPEECH_N, &g_smSpIdx,   0 },               // level 4
    { AXS_SM_SP_SAPI_HEADER, kSmSapi, SM_SAPI_N,  &g_smSapiIdx, SM_LEVEL_SPEECH }, // level 5
};
static const int SM_LISTS = (int)(sizeof kSmLists / sizeof kSmLists[0]);
static bool smIsList(int level) { return level >= SM_LEVEL_LISTS && level < SM_LEVEL_LISTS + SM_LISTS; }
static const SmValList& smList() { return kSmLists[g_smLevel - SM_LEVEL_LISTS]; }

bool axIsSettings() { return g_smActive; }

static AxStrId smCatName(int i) {
    return i == SM_CAT_ANNOUNCE ? AXS_SM_CAT_ANNOUNCE
         : i == SM_CAT_SPEECH   ? AXS_SM_CAT_SPEECH
         : i == SM_CAT_CONTROLS ? AXS_SM_CAT_CONTROLS
         : i == SM_CAT_UPDATES  ? AXS_SM_CAT_UPDATES
         : i == SM_CAT_DEBUG    ? AXS_SM_CAT_DEBUGLOG : AXS_SM_CAT_VERBOSITY;
}

static void smToggleLabel(AxStrId name, bool on, char* out, int outsz) {
    _snprintf(out, outsz, "%s: %s", axs(name), axs(on ? AXS_VALUE_ON : AXS_VALUE_OFF));
    out[outsz - 1] = 0;
}

static void smCatLabel(int i, char* out, int outsz) {
    if      (i == SM_CAT_DEBUG)   smToggleLabel(smCatName(i), axDebugLogEnabled(), out, outsz);
    else if (i == SM_CAT_UPDATES) smToggleLabel(smCatName(i), axCheckUpdates(),    out, outsz);
    else { _snprintf(out, outsz, "%s", axs(smCatName(i))); out[outsz - 1] = 0; }
}

static int smRowCount(const SmValRow& r) {
    int n = r.countFn ? r.countFn() : r.count;
    return n < 1 ? 1 : n;
}
static int smRowValue(const SmValRow& r) {
    if (!r.get) return 0;
    int v = r.get();
    int n = smRowCount(r);
    if (v < 0 || v >= n) { logLine("settings: row \"%s\" value %d out of range %d", axs(r.name), v, n); v = 0; }
    return v;
}
static void smRowValueText(const SmValRow& r, int i, char* out, int outsz) {
    if (r.valueFn) r.valueFn(i, out, outsz);
    else { _snprintf(out, outsz, "%s", axs(r.valueName[i])); out[outsz - 1] = 0; }
}
static void smRowOutcomeText(const SmValRow& r, int i, char* out, int outsz) {
    if (r.outcomeFn) r.outcomeFn(i, out, outsz);
    else { _snprintf(out, outsz, "%s", axs(r.outcome[i])); out[outsz - 1] = 0; }
}
// "<name>: <value>" for a value row; a submenu row is just its name.
static void smRowLabel(const SmValRow& r, char* out, int outsz) {
    if (!r.set) { _snprintf(out, outsz, "%s", axs(r.name)); out[outsz - 1] = 0; return; }
    char val[160];
    smRowValueText(r, smRowValue(r), val, sizeof val);
    _snprintf(out, outsz, "%s: %s", axs(r.name), val);
    out[outsz - 1] = 0;
}
static AxStrId smSubName(int i) { return i == 0 ? AXS_SM_SUB_TOWN : AXS_SM_SUB_DUNGEON; }
static KmRegion smRegion() { return g_smSub == 0 ? KMR_TOWN : KMR_DUNGEON; }

static void smBuildMap() {
    g_smMapN = 0;
    for (int i = 0; i < kmCount(); i++) {
        if (kmAt(i)->region != smRegion()) continue;
        if (g_smMapN >= (int)(sizeof g_smMap / sizeof g_smMap[0])) { logLine("settings: map CLIPPED"); break; }
        g_smMap[g_smMapN++] = i;
    }
}

// ---- Announcing ----
static void smRowLine(char* out, int outsz, const char* name, int idx, int count) {
    // "<name>. <i> of <n>." -- POS_N_OF_M carries the localized "i of n" clause.
    char pos[48];
    _snprintf(pos, sizeof pos, axs(AXS_POS_N_OF_M), idx + 1, count);
    pos[sizeof pos - 1] = 0;
    _snprintf(out, outsz, "%s. %s", name, pos);
    out[outsz - 1] = 0;
}

// The command row: "<chord or Unassigned>. <what it does> <i> of <n>."
static void smEntryLine(char* out, int outsz) {
    int gi = g_smMap[g_smIdx];
    char key[96];
    if (kmIsBlank(gi)) _snprintf(key, sizeof key, "%s", axs(AXS_SM_UNASSIGNED));
    else               kmChordName(kmCurrent(gi), key, sizeof key);
    key[sizeof key - 1] = 0;
    char pos[48];
    _snprintf(pos, sizeof pos, axs(AXS_POS_N_OF_M), g_smIdx + 1, g_smMapN);
    pos[sizeof pos - 1] = 0;
    _snprintf(out, outsz, "%s. %s %s", key, axs(kmAt(gi)->desc), pos);
    out[outsz - 1] = 0;
}

// The current value-list row as "<name>: <value>. <i> of <n>."
static void smListRowLine(char* out, int outsz) {
    const SmValList& L = smList();
    char label[224];
    smRowLabel(L.rows[*L.cursor], label, sizeof label);
    smRowLine(out, outsz, label, *L.cursor, L.n);
}

static void smAnnounceLevel(bool withTitle) {
    char row[640];
    char cat[160];
    if (g_smLevel == 0)      { smCatLabel(g_smCat, cat, sizeof cat);
                               smRowLine(row, sizeof row, cat, g_smCat, SM_CATS); }
    else if (g_smLevel == 1) smRowLine(row, sizeof row, axs(smSubName(g_smSub)), g_smSub, SM_SUBS);
    else if (smIsList(g_smLevel)) smListRowLine(row, sizeof row);
    else                     smEntryLine(row, sizeof row);
    if (withTitle) {
        char buf[768];
        _snprintf(buf, sizeof buf, "%s %s", axs(AXS_SM_TITLE), row);
        buf[sizeof buf - 1] = 0;
        postSpeech(buf);
    } else {
        postSpeech(row);
    }
}

static void smAnnounceSaveQ() {
    char buf[256];
    _snprintf(buf, sizeof buf, "%s %s", axs(AXS_SM_SAVEQ),
              axs(g_smSaveSel == 0 ? AXS_SM_YES : AXS_SM_NO));
    buf[sizeof buf - 1] = 0;
    postSpeech(buf);
}

static void smAnnounceWarn() {
    char buf[256];
    _snprintf(buf, sizeof buf, "%s %s", axs(AXS_SM_WARN_UNASSIGNED), axs(AXS_SM_OK));
    buf[sizeof buf - 1] = 0;
    postSpeech(buf);
}

static void smToggleDebugLog() {
    bool wantOn = !axDebugLogEnabled();
    char outcome[MAX_PATH + 160];
    if (wantOn) {
        if (axSetDebugLog(true)) {
            _snprintf(outcome, sizeof outcome, axs(AXS_SM_DEBUGLOG_ON_FMT), axDebugLogPath());
            logLine("settings: debug log switched ON by the player");
        } else {
            _snprintf(outcome, sizeof outcome, axs(AXS_SM_DEBUGLOG_FAIL_FMT), axDebugLogPath());
        }
    } else {
        logLine("settings: debug log switched OFF by the player");
        axSetDebugLog(false);
        _snprintf(outcome, sizeof outcome, "%s", axs(AXS_SM_DEBUGLOG_OFF));
    }
    outcome[sizeof outcome - 1] = 0;
    char cat[160], row[640], buf[1024];
    smCatLabel(g_smCat, cat, sizeof cat);
    smRowLine(row, sizeof row, cat, g_smCat, SM_CATS);
    _snprintf(buf, sizeof buf, "%s %s", outcome, row);
    buf[sizeof buf - 1] = 0;
    postSpeech(buf);
}

static void smToggleUpdates() {
    axSetCheckUpdates(!axCheckUpdates());
    bool now = axCheckUpdates();
    logLine("settings: update check switched %s by the player", now ? "ON" : "OFF");
    char cat[160], row[640], buf[1024];
    smCatLabel(g_smCat, cat, sizeof cat);
    smRowLine(row, sizeof row, cat, g_smCat, SM_CATS);
    _snprintf(buf, sizeof buf, "%s %s", axs(now ? AXS_SM_UPDATES_ON : AXS_SM_UPDATES_OFF), row);
    buf[sizeof buf - 1] = 0;
    postSpeech(buf);
}

static void smOpenList(int level) {
    g_smLevel = level;
    const SmValList& L = smList();
    *L.cursor = 0;
    char head[128], row[640], buf[768];
    _snprintf(head, sizeof head, axs(L.header), L.n);
    head[sizeof head - 1] = 0;
    smListRowLine(row, sizeof row);
    _snprintf(buf, sizeof buf, "%s %s", head, row);
    buf[sizeof buf - 1] = 0;
    postSpeech(buf);
}

static void smStepValue(int delta, bool wrap) {
    const SmValList& L = smList();
    const SmValRow& r = L.rows[*L.cursor];
    if (!r.set) {
        if (wrap) smOpenList(r.child);            // Enter on a submenu row
        return;
    }
    int count = smRowCount(r);
    int cur = smRowValue(r);
    int want = cur + delta;
    if (wrap) want = ((want % count) + count) % count;
    else {
        if (want < 0) want = 0;
        if (want >= count) want = count - 1;
        if (want == cur) { smAnnounceLevel(false); return; }   // at the end: re-read, change nothing
    }
    r.set(want);
    int now = smRowValue(r);
    logLine("settings: level %d row %d \"%s\" -> value %d (asked %d)", g_smLevel, *L.cursor, axs(r.name), now, want);
    char outcome[320], row[640], buf[1024];
    smRowOutcomeText(r, now, outcome, sizeof outcome);
    smListRowLine(row, sizeof row);
    if (outcome[0]) _snprintf(buf, sizeof buf, "%s %s", outcome, row);
    else            _snprintf(buf, sizeof buf, "%s", row);
    buf[sizeof buf - 1] = 0;
    postSpeech(buf);
}

void smReannounce(uintptr_t base) {
    (void)base;
    if (!g_smActive) return;
    if (g_smMode == 1) {
        char buf[640];
        _snprintf(buf, sizeof buf, axs(AXS_SM_CAPTURE_FMT), axs(kmAt(g_smMap[g_smIdx])->desc));
        buf[sizeof buf - 1] = 0;
        postSpeech(buf);
    }
    else if (g_smMode == 2) smAnnounceWarn();
    else if (g_smMode == 3) smAnnounceSaveQ();
    else smAnnounceLevel(true);
}

// ---- Open / close ----
void smOpen(uintptr_t base) {
    if (g_smActive) return;
    kmEnsureLoaded();
    kmSnapshot();
    logLine("settings: opened (ctx underneath: %d)", (int)currentAxContext());
    g_smActive = true;
    g_smLevel = 0; g_smCat = 0; g_smMode = 0;
    static bool checked = false;
    if (!checked) {
        checked = true;
        char who[64] = "";
        bool owned = kbActionForKey(base, (int)SDLK_F10, who, sizeof who);
        logLine("settings: F10 game-binding check: %s", owned ? who : "not bound");
    }
    smAnnounceLevel(true);
}

static void smClose(uintptr_t base, AxStrId outcome) {
    g_smActive = false;
    g_smMode = 0;
    logLine("settings: closed");
    axModalClosed(base, "settings");
    postSpeech(axs(outcome), /*interrupt=*/false, SPK_EVENT);
}

static void smTryClose(uintptr_t base) {
    int gi = kmFirstBlank(KMR_NONE);
    if (gi >= 0) {
        g_smCat = SM_CAT_CONTROLS;
        g_smSub = (kmAt(gi)->region == KMR_TOWN) ? 0 : 1;
        smBuildMap();
        g_smIdx = 0;
        for (int i = 0; i < g_smMapN; i++) if (g_smMap[i] == gi) { g_smIdx = i; break; }
        g_smLevel = 2;
        g_smMode = 2;
        smAnnounceWarn();
        return;
    }
    if (kmDirty()) {
        g_smMode = 3;
        g_smSaveSel = 0;
        smAnnounceSaveQ();
        return;
    }
    smClose(base, AXS_SM_CLOSED);
}

// ---- Capture ----
static void smCaptureKey(uintptr_t base, uint32_t sym, uint16_t mod) {
    (void)base;
    // A modifier's own KEYDOWN is a chord being formed, not a choice.
    if (sym >= 0x400000E0u && sym <= 0x400000E7u) return;
    if (sym == SDLK_ESCAPE) {
        g_smMode = 0;
        char row[640], buf[768];
        smEntryLine(row, sizeof row);
        _snprintf(buf, sizeof buf, "%s %s", axs(AXS_CANCELLED), row);
        buf[sizeof buf - 1] = 0;
        postSpeech(buf);
        return;
    }
    if (sym == SDLK_F10) { postSpeech(axs(AXS_SM_MENUKEY_REFUSED)); return; }
    KmChord c = { sym, kmChordModsFromKmod(mod) };
    int gi = g_smMap[g_smIdx];
    int victim = kmAssign(gi, c);
    g_smMode = 0;
    char key[96], row[640], buf[1024];
    kmChordName(c, key, sizeof key);
    smEntryLine(row, sizeof row);
    if (victim >= 0) {
        char stolen[320];
        _snprintf(stolen, sizeof stolen, axs(AXS_SM_STOLEN_FMT), axs(kmAt(victim)->desc));
        stolen[sizeof stolen - 1] = 0;
        char assigned[128];
        _snprintf(assigned, sizeof assigned, axs(AXS_SM_ASSIGNED_FMT), key);
        assigned[sizeof assigned - 1] = 0;
        _snprintf(buf, sizeof buf, "%s %s %s", assigned, stolen, row);
    } else {
        char assigned[128];
        _snprintf(assigned, sizeof assigned, axs(AXS_SM_ASSIGNED_FMT), key);
        assigned[sizeof assigned - 1] = 0;
        _snprintf(buf, sizeof buf, "%s %s", assigned, row);
    }
    buf[sizeof buf - 1] = 0;
    postSpeech(buf);
}

// ---- The key handler ----
static void smStepLevelCursor(int delta) {
    if (g_smLevel == 0)           axStepCursor(&g_smCat, SM_CATS, delta);
    else if (g_smLevel == 1)      axStepCursor(&g_smSub, SM_SUBS, delta);
    else if (smIsList(g_smLevel)) { const SmValList& L = smList(); axStepCursor(L.cursor, L.n, delta); }
    else                          axStepCursor(&g_smIdx, g_smMapN, delta);
}

bool routeSettingsKey(uintptr_t base, uint32_t sym, uint16_t mod, uint8_t repeat) {
    if (!g_smActive) return false;                    // belt-and-braces; the row's predicate gates
    bool enter = (sym == SDLK_RETURN || sym == SDLK_KP_ENTER || sym == 0x40000058u);

    if (g_smMode == 1) {                              // capture: the next key IS the answer
        if (!repeat) smCaptureKey(base, sym, mod);
        return true;
    }
    if (g_smMode == 2) {                              // "Commands still unassigned." -- OK
        if (!repeat && (enter || sym == SDLK_SPACE || sym == SDLK_ESCAPE)) {
            g_smMode = 0;
            smAnnounceLevel(false);                   // lands on the first blank row
        }
        return true;
    }
    if (g_smMode == 3) {                              // "Save changes?" -- Yes / No
        if (repeat) return true;
        if (sym == SDLK_LEFT || sym == SDLK_RIGHT || sym == SDLK_UP || sym == SDLK_DOWN) {
            g_smSaveSel ^= 1;
            postSpeech(axs(g_smSaveSel == 0 ? AXS_SM_YES : AXS_SM_NO));
        } else if (enter) {
            if (g_smSaveSel == 0) { kmSaveFile(); smClose(base, AXS_SM_SAVED); }
            else                  { kmRevert();   smClose(base, AXS_SM_DISCARDED); }
        } else if (sym == SDLK_ESCAPE) {
            g_smMode = 0;                             // changed their mind: back to the categories
            smAnnounceLevel(false);
        }
        return true;
    }

    // Browse.
    if (sym == SDLK_UP || sym == SDLK_DOWN) {
        smStepLevelCursor((sym == SDLK_DOWN) ? 1 : -1);
        smAnnounceLevel(false);                       // ends clamp and re-read: lists don't wrap
        return true;
    }
    {
        int jump = 0;
        if (axDecodeJump(sym, mod, repeat, &jump)) {
            if (!jump) return true;                   // held jump: one landing per press
            smStepLevelCursor(jump);
            smAnnounceLevel(false);                   // the clamp lands it; ends re-read
            return true;
        }
    }
    if (repeat) return true;
    if (sym == SDLK_COMMA) { smReannounce(base); return true; }  // the global say-again key
    if (sym == SDLK_LEFT || sym == SDLK_RIGHT) {
        if (smIsList(g_smLevel)) smStepValue(sym == SDLK_RIGHT ? 1 : -1, /*wrap=*/false);
        return true;
    }
    if (enter) {
        if (g_smLevel == 0 && g_smCat == SM_CAT_DEBUG) {
            smToggleDebugLog();
        } else if (g_smLevel == 0 && g_smCat == SM_CAT_UPDATES) {
            smToggleUpdates();
        } else if (g_smLevel == 0 && g_smCat == SM_CAT_ANNOUNCE) {
            smOpenList(SM_LEVEL_ANN);                 // Announcements
        } else if (g_smLevel == 0 && g_smCat == SM_CAT_SPEECH) {
            smOpenList(SM_LEVEL_SPEECH);              // Speech
        } else if (smIsList(g_smLevel)) {
            smStepValue(1, /*wrap=*/true);            // a value row cycles; a submenu row opens
        } else if (g_smLevel == 0) {                  // the one category left: SM_CAT_CONTROLS
            g_smLevel = 1;                            // Controls
            g_smSub = 0;
            smAnnounceLevel(false);
        } else if (g_smLevel == 1) {
            smBuildMap();
            g_smIdx = 0;
            g_smLevel = 2;
            char head[128], row[640], buf[768];
            _snprintf(head, sizeof head, axs(AXS_SM_LIST_HEADER_FMT), axs(smSubName(g_smSub)), g_smMapN);
            head[sizeof head - 1] = 0;
            smEntryLine(row, sizeof row);
            _snprintf(buf, sizeof buf, "%s %s", head, row);
            buf[sizeof buf - 1] = 0;
            postSpeech(buf);
        } else {
            g_smMode = 1;
            char buf[640];
            _snprintf(buf, sizeof buf, axs(AXS_SM_CAPTURE_FMT), axs(kmAt(g_smMap[g_smIdx])->desc));
            buf[sizeof buf - 1] = 0;
            postSpeech(buf);
        }
        return true;
    }
    if (sym == SDLK_ESCAPE) {
        if (smIsList(g_smLevel)) {                    // a list goes to its parent (0, or 4 for SAPI)
            g_smLevel = smList().parent;
            smAnnounceLevel(false);
        } else if (g_smLevel == 2) {
            int gi = kmFirstBlank(smRegion());
            if (gi >= 0) {
                g_smIdx = 0;
                for (int i = 0; i < g_smMapN; i++) if (g_smMap[i] == gi) { g_smIdx = i; break; }
                g_smMode = 2;
                smAnnounceWarn();
            } else {
                g_smLevel = 1;
                smAnnounceLevel(false);
            }
        } else if (g_smLevel == 1) {
            g_smLevel = 0;
            smAnnounceLevel(false);
        } else {
            smTryClose(base);
        }
        return true;
    }
    if (sym == SDLK_F10) { smTryClose(base); return true; }
    return true;                                      // modal: everything else is swallowed
}
