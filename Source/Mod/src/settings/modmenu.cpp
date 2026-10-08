// settings/modmenu.cpp -- the F10 MOD SETTINGS MENU.

#include <windows.h>
#include <cstdio>
#include <cstring>
#include "internal.h"

bool g_smActive = false;

static const int SM_LEVEL_GROUPS = 1;
static const int SM_LEVEL_FUNCS  = 2;
static const int SM_LEVEL_ANN    = 3;
static const int SM_LEVEL_SPEECH = 4;
static const int SM_LEVEL_SAPI   = 5;
static const int SM_LEVEL_SUBFUNCS = 6;
static const int SM_LEVEL_BINDS  = 7;
static const int SM_LEVEL_LISTS  = SM_LEVEL_ANN;
static int g_smLevel = 0;
static int g_smCat = 0;
static int g_smGroup = 0;
static int g_smAnnIdx = 0;         // cursor within the Announcements list
static int g_smSpIdx = 0;          // cursor within the Speech list
static int g_smSapiIdx = 0;        // cursor within SAPI configuration
static int g_smMode = 0;
static int g_smSaveSel = 0;        // save question / reset question: 0 Yes, 1 No

static const int SM_ROWS_MAX = 96;
static int g_smRows[SM_ROWS_MAX];
static int g_smRowsN = 0;
static int g_smIdx = 0;            // cursor within the function list
static int g_smTopGroup = 0;       // the KmGroup level 2 lists
static int g_smSubGroup = 0;       // the KmGroup level 6 lists
static int g_smTopIdx = 0;         // level 2's cursor, kept while level 6 is open
static int g_smFn = -1;
static int g_smBind = 0;
static uint8_t g_smCapIn = PI_NONE, g_smCapMod = PI_NONE;
static DWORD   g_smCapSince = 0;
static const DWORD SM_PAD_HOLD_MS = 450;
static int g_smFnSettings = -2, g_smFnSayAgain = -2;

static const int SM_CAT_ANNOUNCE = 0;
static const int SM_CAT_SPEECH   = 1;
static const int SM_CAT_CONTROLS = 2;
static const int SM_CAT_UPDATES  = 3;
static const int SM_CAT_DEBUG    = 4;   // inline toggle row at level 0
static const int SM_CATS = 5;
// Level 1: the four groups in the dev's order, then the reset row.
static const int SM_GROUP_ROWS = 5;
static const int SM_GROUP_RESET = 4;
static const KmGroup kSmTopGroups[4] = { KMG_GENERAL, KMG_HAMLET, KMG_DUNGEON, KMG_DLC };

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
static int  smPosGet()       { return axPositionCounts() ? 0 : 1; }
static void smPosSet(int v)  { axSetPositionCounts(v == 0); }
static const SmValRow kSmAnn[] = {
    { AXS_SM_ANN_SUBTITLES, 2, nullptr, smSubsGet, smSubsSet, 0,
      { AXS_VALUE_ON, AXS_VALUE_OFF }, nullptr,
      { AXS_SM_ANN_SUBS_ON, AXS_SM_ANN_SUBS_OFF }, nullptr },
    { AXS_SM_ANN_BARKS, AX_BARKS_MODES, nullptr, axBarksMode, axSetBarksMode, 0,
      { AXS_VALUE_ON, AXS_SM_ANN_BARKS_V_DUNGEON, AXS_SM_ANN_BARKS_V_TOWN, AXS_VALUE_OFF }, nullptr,
      { AXS_SM_ANN_BARKS_ON, AXS_SM_ANN_BARKS_DUNGEON, AXS_SM_ANN_BARKS_TOWN, AXS_SM_ANN_BARKS_OFF }, nullptr },
    { AXS_SM_ANN_POSITIONS, 2, nullptr, smPosGet, smPosSet, 0,
      { AXS_VALUE_ON, AXS_VALUE_OFF }, nullptr,
      { AXS_SM_ANN_POS_ON, AXS_SM_ANN_POS_OFF }, nullptr },
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
static bool smIsFuncs(int level) { return level == SM_LEVEL_FUNCS || level == SM_LEVEL_SUBFUNCS; }

bool axIsSettings() { return g_smActive; }

static AxStrId smCatName(int i) {
    return i == SM_CAT_ANNOUNCE ? AXS_SM_CAT_ANNOUNCE
         : i == SM_CAT_SPEECH   ? AXS_SM_CAT_SPEECH
         : i == SM_CAT_CONTROLS ? AXS_SM_CAT_CONTROLS
         : i == SM_CAT_UPDATES  ? AXS_SM_CAT_UPDATES
         : i == SM_CAT_DEBUG    ? AXS_SM_CAT_DEBUGLOG : AXS_SM_CAT_VERBOSITY;
}
static AxStrId smGroupName(KmGroup g) {
    switch (g) {
        case KMG_GENERAL:        return AXS_SM_GRP_GENERAL;
        case KMG_GENERAL_SHEET:  return AXS_SM_GRP_SHEET_SUB;
        case KMG_HAMLET:         return AXS_SM_GRP_HAMLET;
        case KMG_HAMLET_BLDG:    return AXS_SM_GRP_BLDG_SUB;
        case KMG_DUNGEON:        return AXS_SM_GRP_DUNGEON;
        case KMG_DUNGEON_SKILLS: return AXS_SM_GRP_SKILLS_SUB;
        case KMG_DLC:            return AXS_SM_GRP_DLC;
        default:                 return AXS_SM_GRP_GENERAL;
    }
}
static KmGroup smSubOf(KmGroup top, const char** afterId) {
    *afterId = nullptr;
    switch (top) {
        case KMG_GENERAL: *afterId = "general.char_sheet"; return KMG_GENERAL_SHEET;
        case KMG_HAMLET:  return KMG_HAMLET_BLDG;
        case KMG_DUNGEON: return KMG_DUNGEON_SKILLS;
        default:          return KMG__COUNT;                // no submenu
    }
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

// ---- The function lists ----
static void smBuildRows(KmGroup g) {
    g_smRowsN = 0;
    const char* afterId = nullptr;
    KmGroup sub = smSubOf(g, &afterId);
    bool subPlaced = (sub == KMG__COUNT);
    for (int i = 0; i < kmCount(); i++) {
        if (kmAt(i)->group != g) continue;
        if (g_smRowsN >= SM_ROWS_MAX) { logLine("settings: rows CLIPPED"); break; }
        g_smRows[g_smRowsN++] = i;
        if (!subPlaced && afterId && strcmp(kmAt(i)->id, afterId) == 0 && g_smRowsN < SM_ROWS_MAX) {
            g_smRows[g_smRowsN++] = -1 - (int)sub; subPlaced = true;
        }
    }
    if (!subPlaced && g_smRowsN < SM_ROWS_MAX) g_smRows[g_smRowsN++] = -1 - (int)sub;
}
static bool smRowIsSub(int row) { return row < 0; }
static KmGroup smRowSub(int row) { return (KmGroup)(-1 - row); }

// ---- Summaries: a function's bindings in one breath ----
static void smKeySummary(int f, char* out, int outsz) {
    const KmFunc* F = kmAt(f);
    out[0] = 0;
    if (F->slots == 4) {
        static const uint32_t kArrows[4] = { SDLK_UP, SDLK_DOWN, SDLK_LEFT, SDLK_RIGHT };
        bool allArrows = true; uint8_t mods = 0;
        for (int s = 0; s < 4; s++) {
            if (kmKeyIsBlank(f, s)) { allArrows = false; break; }
            KmChord c = kmKeyCurrent(f, s);
            if (c.sym != kArrows[s]) { allArrows = false; break; }
            if (s == 0) mods = c.mods; else if (c.mods != mods) { allArrows = false; break; }
        }
        if (allArrows) {
            if (!mods) { _snprintf(out, outsz, "%s", axs(AXS_SM_SUM_ARROWS)); out[outsz - 1] = 0; return; }
            char m[96];
            _snprintf(m, sizeof m, "%s%s%s%s%s",
                      (mods & KM_CTRL) ? axs(AXS_KN_CTRL) : "", (mods & KM_CTRL) && (mods & (KM_SHIFT | KM_ALT)) ? " " : "",
                      (mods & KM_SHIFT) ? axs(AXS_KN_SHIFT) : "", (mods & KM_SHIFT) && (mods & KM_ALT) ? " " : "",
                      (mods & KM_ALT) ? axs(AXS_KN_ALT) : "");
            m[sizeof m - 1] = 0;
            _snprintf(out, outsz, axs(AXS_SM_SUM_MOD_ARROWS_FMT), m); out[outsz - 1] = 0;
            return;
        }
    }
    for (int s = 0; s < F->slots; s++) {
        char one[96];
        if (kmKeyIsBlank(f, s)) _snprintf(one, sizeof one, "%s", axs(AXS_SM_UNASSIGNED));
        else kmChordName(kmKeyCurrent(f, s), one, sizeof one);
        one[sizeof one - 1] = 0;
        size_t len = strlen(out);
        _snprintf(out + len, outsz - len, "%s%s", s ? ", " : "", one);
        out[outsz - 1] = 0;
    }
}
static void smPadSummary(int f, char* out, int outsz) {
    const KmFunc* F = kmAt(f);
    out[0] = 0;
    if (F->slots == 4) {
        static const uint8_t kDpad[4] = { PI_DPAD_UP, PI_DPAD_DOWN, PI_DPAD_LEFT, PI_DPAD_RIGHT };
        static const uint8_t kRs[4]   = { PI_RS_UP, PI_RS_DOWN, PI_RS_LEFT, PI_RS_RIGHT };
        for (int fam = 0; fam < 2; fam++) {
            const uint8_t* want = fam == 0 ? kDpad : kRs;
            bool all = true; uint8_t mod = PI_NONE;
            for (int s = 0; s < 4; s++) {
                if (kmPadIsBlank(f, s)) { all = false; break; }
                PadBind b = kmPadCurrent(f, s);
                if (b.in != want[s] || b.hold) { all = false; break; }
                if (s == 0) mod = b.mod; else if (b.mod != mod) { all = false; break; }
            }
            if (!all) continue;
            const char* fam_name = axs(fam == 0 ? AXS_SM_SUM_DPAD : AXS_SM_SUM_RSTICK);
            if (mod == PI_NONE) _snprintf(out, outsz, "%s", fam_name);
            else {
                char modName[64]; PadBind mb = { mod, PI_NONE, 0 }; kmPadName(mb, modName, sizeof modName);
                _snprintf(out, outsz, axs(AXS_PAD_CHORD_FMT), modName, fam_name);
            }
            out[outsz - 1] = 0;
            return;
        }
    }
    for (int s = 0; s < F->slots; s++) {
        char one[128];
        if (kmPadIsBlank(f, s)) _snprintf(one, sizeof one, "%s", axs(AXS_SM_UNASSIGNED));
        else kmPadName(kmPadCurrent(f, s), one, sizeof one);
        one[sizeof one - 1] = 0;
        size_t len = strlen(out);
        _snprintf(out + len, outsz - len, "%s%s", s ? ", " : "", one);
        out[outsz - 1] = 0;
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

static void smGroupLine(char* out, int outsz) {
    AxStrId nm = g_smGroup == SM_GROUP_RESET ? AXS_SM_GRP_RESET : smGroupName(kSmTopGroups[g_smGroup]);
    smRowLine(out, outsz, axs(nm), g_smGroup, SM_GROUP_ROWS);
}

static void smFuncLine(char* out, int outsz) {
    if (g_smRowsN <= 0) { _snprintf(out, outsz, "%s", axs(AXS_NO_ROW_SELECTED)); out[outsz - 1] = 0; return; }
    int row = g_smRows[g_smIdx];
    char label[640];
    if (smRowIsSub(row)) {
        _snprintf(label, sizeof label, "%s. %s", axs(smGroupName(smRowSub(row))), axs(AXS_SM_SUBMENU));
    } else {
        char ks[256], ps[320];
        smKeySummary(row, ks, sizeof ks);
        smPadSummary(row, ps, sizeof ps);
        _snprintf(label, sizeof label, axs(AXS_SM_ROW_FMT), axs(kmAt(row)->name), ks, ps);
    }
    label[sizeof label - 1] = 0;
    smRowLine(out, outsz, label, g_smIdx, g_smRowsN);
}

static bool smBindIsPad(int bind) { return g_smFn >= 0 && bind >= kmAt(g_smFn)->slots; }
static int  smBindSlot(int bind) { const KmFunc* F = kmAt(g_smFn); return bind < F->slots ? bind : bind - F->slots; }
static int  smBindCount() { return g_smFn >= 0 ? 2 * kmAt(g_smFn)->slots : 0; }
static void smBindLabel(int bind, char* out, int outsz) {
    const KmFunc* F = kmAt(g_smFn);
    bool pad = smBindIsPad(bind);
    int s = smBindSlot(bind);
    char val[128];
    if (pad) { if (kmPadIsBlank(g_smFn, s)) _snprintf(val, sizeof val, "%s", axs(AXS_SM_UNASSIGNED)); else kmPadName(kmPadCurrent(g_smFn, s), val, sizeof val); }
    else     { if (kmKeyIsBlank(g_smFn, s)) _snprintf(val, sizeof val, "%s", axs(AXS_SM_UNASSIGNED)); else kmChordName(kmKeyCurrent(g_smFn, s), val, sizeof val); }
    val[sizeof val - 1] = 0;
    const char* dev = axs(pad ? AXS_SM_DEV_CONTROLLER : AXS_SM_DEV_KEYBOARD);
    if (F->slots > 1 && F->slotName[s] != AXS__COUNT) _snprintf(out, outsz, axs(AXS_SM_BIND_ROW_SLOT_FMT), dev, axs(F->slotName[s]), val);
    else _snprintf(out, outsz, axs(AXS_SM_BIND_ROW_FMT), dev, val);
    out[outsz - 1] = 0;
}
static void smBindLine(char* out, int outsz) {
    if (g_smFn < 0) { _snprintf(out, outsz, "%s", axs(AXS_NO_ROW_SELECTED)); out[outsz - 1] = 0; return; }
    char label[320];
    smBindLabel(g_smBind, label, sizeof label);
    smRowLine(out, outsz, label, g_smBind, smBindCount());
}
// "<function> <slot>" -- what a capture prompt and a theft warning name.
static void smFuncSlotName(int f, int s, char* out, int outsz) {
    const KmFunc* F = kmAt(f);
    if (F->slots > 1 && F->slotName[s] != AXS__COUNT) _snprintf(out, outsz, "%s, %s", axs(F->name), axs(F->slotName[s]));
    else _snprintf(out, outsz, "%s", axs(F->name));
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
    char row[1024];
    char cat[160];
    if (g_smLevel == 0)      { smCatLabel(g_smCat, cat, sizeof cat);
                               smRowLine(row, sizeof row, cat, g_smCat, SM_CATS); }
    else if (g_smLevel == SM_LEVEL_GROUPS) smGroupLine(row, sizeof row);
    else if (smIsList(g_smLevel)) smListRowLine(row, sizeof row);
    else if (smIsFuncs(g_smLevel)) smFuncLine(row, sizeof row);
    else                     smBindLine(row, sizeof row);
    if (withTitle) {
        char buf[1200];
        _snprintf(buf, sizeof buf, "%s %s", axs(AXS_SM_TITLE), row);
        buf[sizeof buf - 1] = 0;
        postSpeech(buf);
    } else {
        postSpeech(row);
    }
}

static void smAnnounceYesNo(AxStrId question) {
    char buf[256];
    _snprintf(buf, sizeof buf, "%s %s", axs(question), axs(g_smSaveSel == 0 ? AXS_SM_YES : AXS_SM_NO));
    buf[sizeof buf - 1] = 0;
    postSpeech(buf);
}

static void smAnnounceWarn() {
    char buf[256];
    _snprintf(buf, sizeof buf, "%s %s", axs(AXS_SM_WARN_UNASSIGNED), axs(AXS_SM_OK));
    buf[sizeof buf - 1] = 0;
    postSpeech(buf);
}

static void smAnnounceCapture() {
    char what[256], buf[640];
    smFuncSlotName(g_smFn, smBindSlot(g_smBind), what, sizeof what);
    _snprintf(buf, sizeof buf, axs(g_smMode == 4 ? AXS_SM_CAPTURE_PAD_FMT : AXS_SM_CAPTURE_FMT), what);
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

// Open a group's function list (level 2 or 6): header + row 0 in one utterance.
static void smOpenFuncs(int level, KmGroup g, int cursor) {
    g_smLevel = level;
    smBuildRows(g);
    g_smIdx = (cursor >= 0 && cursor < g_smRowsN) ? cursor : 0;
    char head[160], row[1024], buf[1200];
    _snprintf(head, sizeof head, axs(AXS_SM_LIST_HEADER_FMT), axs(smGroupName(g)), g_smRowsN);
    head[sizeof head - 1] = 0;
    smFuncLine(row, sizeof row);
    _snprintf(buf, sizeof buf, "%s %s", head, row);
    buf[sizeof buf - 1] = 0;
    postSpeech(buf);
}

// Open a function's binding rows (level 7).
static void smOpenBinds(int f, int bind) {
    g_smLevel = SM_LEVEL_BINDS;
    g_smFn = f;
    g_smBind = (bind >= 0 && bind < smBindCount()) ? bind : 0;
    char head[320], row[640], buf[1024];
    _snprintf(head, sizeof head, axs(AXS_SM_BIND_HEADER_FMT), axs(kmAt(f)->name), smBindCount());
    head[sizeof head - 1] = 0;
    smBindLine(row, sizeof row);
    _snprintf(buf, sizeof buf, "%s %s", head, row);
    buf[sizeof buf - 1] = 0;
    postSpeech(buf);
}

static void smParkOn(int f, int s) {
    const KmFunc* F = kmAt(f);
    KmGroup g = F->group;
    bool isSub = (g == KMG_GENERAL_SHEET || g == KMG_HAMLET_BLDG || g == KMG_DUNGEON_SKILLS);
    KmGroup top = g == KMG_GENERAL_SHEET ? KMG_GENERAL : g == KMG_HAMLET_BLDG ? KMG_HAMLET :
                  g == KMG_DUNGEON_SKILLS ? KMG_DUNGEON : g;
    g_smCat = SM_CAT_CONTROLS;
    for (int i = 0; i < 4; i++) if (kSmTopGroups[i] == top) g_smGroup = i;
    g_smTopGroup = (int)top;
    smBuildRows(top);
    g_smTopIdx = 0;
    for (int i = 0; i < g_smRowsN; i++) {
        if (isSub ? (smRowIsSub(g_smRows[i]) && smRowSub(g_smRows[i]) == g) : g_smRows[i] == f) { g_smTopIdx = i; break; }
    }
    if (isSub) {
        g_smSubGroup = (int)g;
        smBuildRows(g);
        g_smIdx = 0;
        for (int i = 0; i < g_smRowsN; i++) if (g_smRows[i] == f) { g_smIdx = i; break; }
    } else g_smIdx = g_smTopIdx;
    g_smLevel = SM_LEVEL_BINDS;
    g_smFn = f;
    g_smBind = s;
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
    if (g_smMode == 1 || g_smMode == 4) smAnnounceCapture();
    else if (g_smMode == 2) smAnnounceWarn();
    else if (g_smMode == 3) smAnnounceYesNo(AXS_SM_SAVEQ);
    else if (g_smMode == 5) smAnnounceYesNo(AXS_SM_RESETQ);
    else smAnnounceLevel(true);
}

// ---- Open / close ----
void smOpen(uintptr_t base) {
    if (g_smActive) return;
    kmEnsureLoaded();
    kmSnapshot();
    if (g_smFnSettings == -2) g_smFnSettings = kmFind("general.settings");
    if (g_smFnSayAgain == -2) g_smFnSayAgain = kmFind("general.say_again");
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
    int bf = -1, bs = -1;
    if (kmFirstBlankKey(&bf, &bs)) {
        smParkOn(bf, bs);
        g_smMode = 2;
        smAnnounceWarn();
        return;
    }
    if (kmDirty()) {
        g_smMode = 3;
        g_smSaveSel = 0;
        smAnnounceYesNo(AXS_SM_SAVEQ);
        return;
    }
    smClose(base, AXS_SM_CLOSED);
}

// ---- The assignment's one utterance: "<binding> assigned." [+ the theft warning] + the row ----
static void smSpeakAssigned(const char* bindingName, int victim, int victimSlot) {
    char row[640], buf[1024], assigned[192];
    smBindLine(row, sizeof row);
    _snprintf(assigned, sizeof assigned, axs(AXS_SM_ASSIGNED_FMT), bindingName);
    assigned[sizeof assigned - 1] = 0;
    if (victim >= 0) {
        char who[256], stolen[400];
        smFuncSlotName(victim, victimSlot < 0 ? 0 : victimSlot, who, sizeof who);
        _snprintf(stolen, sizeof stolen, axs(AXS_SM_STOLEN_FMT), who);
        stolen[sizeof stolen - 1] = 0;
        _snprintf(buf, sizeof buf, "%s %s %s", assigned, stolen, row);
    } else {
        _snprintf(buf, sizeof buf, "%s %s", assigned, row);
    }
    buf[sizeof buf - 1] = 0;
    postSpeech(buf);
}

// ---- Keyboard capture (mode 1) ----
static void smCaptureKey(uintptr_t base, uint32_t sym, uint16_t mod) {
    (void)base;
    // A modifier's own KEYDOWN is a chord being formed, not a choice.
    if (sym >= 0x400000E0u && sym <= 0x400000E7u) return;
    if (sym == SDLK_ESCAPE) {
        g_smMode = 0;
        char row[640], buf[768];
        smBindLine(row, sizeof row);
        _snprintf(buf, sizeof buf, "%s %s", axs(AXS_CANCELLED), row);
        buf[sizeof buf - 1] = 0;
        postSpeech(buf);
        return;
    }
    if (g_smFn != g_smFnSettings && g_smFnSettings >= 0 && kmIsCurrentKey(g_smFnSettings, sym, mod)) {
        char key[96], buf[256];
        kmChordName(kmKeyCurrent(g_smFnSettings, 0), key, sizeof key);
        _snprintf(buf, sizeof buf, axs(AXS_SM_MENUKEY_REFUSED), key);
        buf[sizeof buf - 1] = 0;
        postSpeech(buf);
        return;
    }
    KmChord c = { sym, kmChordModsFromKmod(mod) };
    int s = smBindSlot(g_smBind);
    int vs = -1;
    int victim = kmAssignKey(g_smFn, s, c, &vs);
    g_smMode = 0;
    char key[96];
    kmChordName(c, key, sizeof key);
    smSpeakAssigned(key, victim, vs);
}

// ---- Controller capture (mode 4): fed by the pad translator once per pad sample ----
bool smPadCaptureActive() { return g_smActive && g_smMode == 4; }

static void smPadCommit(PadBind b) {
    int s = smBindSlot(g_smBind);
    int vs = -1;
    int victim = kmAssignPad(g_smFn, s, b, &vs);
    g_smMode = 0;
    g_smCapIn = PI_NONE; g_smCapMod = PI_NONE;
    char name[128];
    kmPadName(b, name, sizeof name);
    smSpeakAssigned(name, victim, vs);
}

void smPadCaptureFeed(const bool* down, const bool* pressed, const bool* released) {
    if (!smPadCaptureActive()) return;
    DWORD now = GetTickCount();
    for (int i = 0; i < PI_COUNT; i++) {
        if (!pressed[i]) continue;
        if (g_smCapIn != PI_NONE && down[g_smCapIn]) g_smCapMod = g_smCapIn;
        g_smCapIn = (uint8_t)i;
        g_smCapSince = now;
    }
    if (g_smCapIn == PI_NONE) return;
    if (g_smCapMod != PI_NONE && !down[g_smCapMod]) g_smCapMod = PI_NONE;   // the modifier let go first: not a chord
    if (released[g_smCapIn]) {
        PadBind b = { g_smCapIn, g_smCapMod, 0 };
        smPadCommit(b);
        return;
    }
    if (down[g_smCapIn] && (DWORD)(now - g_smCapSince) >= SM_PAD_HOLD_MS) {
        PadBind b = { g_smCapIn, g_smCapMod, 1 };
        smPadCommit(b);
    }
}

// ---- The reset button (mode 5) ----
static void smResetConfirmed(uintptr_t base) {
    (void)base;
    kmResetDefaults();
    g_smMode = 0;
    char row[640], buf[1024];
    smGroupLine(row, sizeof row);
    _snprintf(buf, sizeof buf, "%s %s", axs(AXS_SM_RESET_DONE), row);
    buf[sizeof buf - 1] = 0;
    postSpeech(buf);
}

// ---- The key handler ----
static void smStepLevelCursor(int delta) {
    if (g_smLevel == 0)                     axStepCursor(&g_smCat, SM_CATS, delta);
    else if (g_smLevel == SM_LEVEL_GROUPS)  axStepCursor(&g_smGroup, SM_GROUP_ROWS, delta);
    else if (smIsList(g_smLevel))           { const SmValList& L = smList(); axStepCursor(L.cursor, L.n, delta); }
    else if (smIsFuncs(g_smLevel))          axStepCursor(&g_smIdx, g_smRowsN, delta);
    else                                    { int n = smBindCount(); axStepCursor(&g_smBind, n, delta); }
}

bool routeSettingsKey(uintptr_t base, uint32_t sym, uint16_t mod, uint8_t repeat) {
    if (!g_smActive) return false;                    // belt-and-braces; the row's predicate gates
    bool enter = (sym == SDLK_RETURN || sym == SDLK_KP_ENTER || sym == 0x40000058u);
    bool menuKey = g_smFnSettings >= 0 ? kmIsCurrentKey(g_smFnSettings, sym, mod) : (sym == SDLK_F10);
    bool sayAgain = g_smFnSayAgain >= 0 ? kmIsCurrentKey(g_smFnSayAgain, sym, mod) : (sym == SDLK_COMMA);

    if (g_smMode == 1) {                              // key capture: the next key IS the answer
        if (!repeat) smCaptureKey(base, sym, mod);
        return true;
    }
    if (g_smMode == 4) {                              // pad capture: only Escape means anything here
        if (!repeat && sym == SDLK_ESCAPE) {
            g_smMode = 0; g_smCapIn = PI_NONE; g_smCapMod = PI_NONE;
            char row[640], buf[768];
            smBindLine(row, sizeof row);
            _snprintf(buf, sizeof buf, "%s %s", axs(AXS_CANCELLED), row);
            buf[sizeof buf - 1] = 0;
            postSpeech(buf);
        } else if (!repeat && sayAgain) smAnnounceCapture();
        return true;
    }
    if (g_smMode == 2) {                              // "Commands still unassigned." -- OK
        if (!repeat && (enter || sym == SDLK_SPACE || sym == SDLK_ESCAPE)) {
            g_smMode = 0;
            smAnnounceLevel(false);                   // lands on the blank binding row
        }
        return true;
    }
    if (g_smMode == 3 || g_smMode == 5) {             // "Save changes?" / "Reset ... ?" -- Yes / No
        if (repeat) return true;
        AxStrId q = g_smMode == 3 ? AXS_SM_SAVEQ : AXS_SM_RESETQ;
        if (sym == SDLK_LEFT || sym == SDLK_RIGHT || sym == SDLK_UP || sym == SDLK_DOWN) {
            g_smSaveSel ^= 1;
            postSpeech(axs(g_smSaveSel == 0 ? AXS_SM_YES : AXS_SM_NO));
        } else if (enter) {
            if (g_smMode == 3) {
                if (g_smSaveSel == 0) { kmSaveFile(); smClose(base, AXS_SM_SAVED); }
                else                  { kmRevert();   smClose(base, AXS_SM_DISCARDED); }
            } else {
                if (g_smSaveSel == 0) smResetConfirmed(base);
                else { g_smMode = 0; smAnnounceLevel(false); }
            }
        } else if (sym == SDLK_ESCAPE) {
            g_smMode = 0;                             // changed their mind: back to where they were
            smAnnounceLevel(false);
        } else if (sayAgain) smAnnounceYesNo(q);
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
    if (sayAgain) { smReannounce(base); return true; }   // the global say-again key, as bound
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
            g_smLevel = SM_LEVEL_GROUPS;
            g_smGroup = 0;
            char head[128], row[640], buf[768];
            _snprintf(head, sizeof head, axs(AXS_SM_GROUPS_HEADER_FMT), SM_GROUP_ROWS);
            head[sizeof head - 1] = 0;
            smGroupLine(row, sizeof row);
            _snprintf(buf, sizeof buf, "%s %s", head, row);
            buf[sizeof buf - 1] = 0;
            postSpeech(buf);
        } else if (g_smLevel == SM_LEVEL_GROUPS) {
            if (g_smGroup == SM_GROUP_RESET) {        // the reset button: ask first (the dev's spec)
                g_smMode = 5; g_smSaveSel = 1;        // lands on No: a reset is the deliberate answer
                smAnnounceYesNo(AXS_SM_RESETQ);
            } else {
                g_smTopGroup = (int)kSmTopGroups[g_smGroup];
                smOpenFuncs(SM_LEVEL_FUNCS, (KmGroup)g_smTopGroup, 0);
            }
        } else if (smIsFuncs(g_smLevel)) {
            if (g_smRowsN <= 0) { smAnnounceLevel(false); return true; }
            int row = g_smRows[g_smIdx];
            if (smRowIsSub(row)) {                    // a submenu row: its own function list
                g_smTopIdx = g_smIdx;
                g_smSubGroup = (int)smRowSub(row);
                smOpenFuncs(SM_LEVEL_SUBFUNCS, (KmGroup)g_smSubGroup, 0);
            } else {
                smOpenBinds(row, 0);
            }
        } else {                                      // a binding row: capture
            g_smMode = smBindIsPad(g_smBind) ? 4 : 1;
            g_smCapIn = PI_NONE; g_smCapMod = PI_NONE;
            smAnnounceCapture();
        }
        return true;
    }
    if (g_smLevel == SM_LEVEL_BINDS && (sym == SDLK_DELETE || sym == SDLK_BACKSPACE)) {
        int s = smBindSlot(g_smBind);
        bool pad = smBindIsPad(g_smBind);
        char row[640], buf[1024];
        if (sym == SDLK_DELETE) {
            if (!pad && g_smFn == g_smFnSettings) { postSpeech(axs(AXS_SM_MENUKEY_KEEP)); return true; }
            if (pad) kmClearPad(g_smFn, s); else kmClearKey(g_smFn, s);
            smBindLine(row, sizeof row);
            _snprintf(buf, sizeof buf, "%s %s", axs(AXS_SM_CLEARED), row);
        } else {
            if (pad) kmRestorePad(g_smFn, s); else kmRestoreKey(g_smFn, s);
            char val[128];
            if (pad) { if (kmPadIsBlank(g_smFn, s)) _snprintf(val, sizeof val, "%s", axs(AXS_SM_UNASSIGNED)); else kmPadName(kmPadCurrent(g_smFn, s), val, sizeof val); }
            else kmChordName(kmKeyCurrent(g_smFn, s), val, sizeof val);
            val[sizeof val - 1] = 0;
            char restored[256];
            _snprintf(restored, sizeof restored, axs(AXS_SM_RESTORED_FMT), val);
            restored[sizeof restored - 1] = 0;
            smBindLine(row, sizeof row);
            _snprintf(buf, sizeof buf, "%s %s", restored, row);
        }
        buf[sizeof buf - 1] = 0;
        postSpeech(buf);
        return true;
    }
    if (sym == SDLK_ESCAPE) {
        if (smIsList(g_smLevel)) {                    // a list goes to its parent (0, or 4 for SAPI)
            g_smLevel = smList().parent;
            smAnnounceLevel(false);
        } else if (g_smLevel == SM_LEVEL_BINDS) {
            const KmFunc* F = kmAt(g_smFn);
            int blank = -1;
            for (int s = 0; s < F->slots; s++) if (kmKeyIsBlank(g_smFn, s)) { blank = s; break; }
            if (blank >= 0) { g_smBind = blank; g_smMode = 2; smAnnounceWarn(); return true; }
            bool fromSub = (F->group == KMG_GENERAL_SHEET || F->group == KMG_HAMLET_BLDG || F->group == KMG_DUNGEON_SKILLS);
            if (fromSub) { g_smLevel = SM_LEVEL_SUBFUNCS; smBuildRows((KmGroup)g_smSubGroup); }
            else         { g_smLevel = SM_LEVEL_FUNCS;    smBuildRows((KmGroup)g_smTopGroup); }
            if (g_smIdx >= g_smRowsN) g_smIdx = 0;
            smAnnounceLevel(false);
        } else if (g_smLevel == SM_LEVEL_SUBFUNCS) {
            smBuildRows((KmGroup)g_smTopGroup);
            g_smIdx = (g_smTopIdx < g_smRowsN) ? g_smTopIdx : 0;
            g_smLevel = SM_LEVEL_FUNCS;
            smAnnounceLevel(false);
        } else if (g_smLevel == SM_LEVEL_FUNCS) {
            g_smLevel = SM_LEVEL_GROUPS;
            smAnnounceLevel(false);
        } else if (g_smLevel == SM_LEVEL_GROUPS) {
            g_smLevel = 0;
            smAnnounceLevel(false);
        } else {
            smTryClose(base);
        }
        return true;
    }
    if (menuKey) { smTryClose(base); return true; }
    return true;                                      // modal: everything else is swallowed
}
