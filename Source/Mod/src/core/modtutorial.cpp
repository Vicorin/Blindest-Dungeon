// core/modtutorial.cpp -- MOD TUTORIALS

#include <windows.h>
#include <cstdio>
#include <cstring>
#include "internal.h"

// ---- The topics ----
static const int MT_MAX_PARAS = 12;
struct MtTopic {
    const char* id;
    bool (*here)(uintptr_t base);
    int paras;
    AxStrId para[MT_MAX_PARAS];
};

static bool mtHereTitle(uintptr_t base) {
    if (currentAxContext() != AX_TITLE) return false;
    uintptr_t disp = frontEndDisplay(base);
    if (!disp || frontEndState(disp) != FE_STATE_TITLE) return false;
    if (g_preambleHold) return false;
    if (confirmDialogOpen(base)) return false;
    return true;
}

static bool mtHereCampaign(uintptr_t base) {
    if (currentAxContext() != AX_TITLE) return false;
    uintptr_t disp = frontEndDisplay(base);
    if (!disp || frontEndState(disp) != FE_STATE_SAVESLOTS) return false;
    if (confirmDialogOpen(base)) return false;
    if (feContentPanelActive()) return false;
    return true;
}

static bool mtHereDungeon(uintptr_t base) {
    AxContext c = currentAxContext();
    if (c != AX_INGAME && c != AX_ROOM && c != AX_ACTIONS && c != AX_QUEST && c != AX_LOG && c != AX_INV) return false;
    if (!mapRoot(base)) return false;
    if (pitInMatch(base)) return false;
    return true;
}

// ---- The map's and the bag's LANDING latches ----
static uintptr_t g_mtRaidRoot = 0;      // the raid the latches belong to
static bool      g_mtLandMap  = false;
static bool      g_mtLandBag  = false;
void mtNoteLanding(bool map) {
    if (map) { if (!g_mtLandMap) logLine("modtutorial: the player landed on the map"); g_mtLandMap = true; }
    else     { if (!g_mtLandBag) logLine("modtutorial: the player landed on the bag"); g_mtLandBag = true; }
}
static void mtTrackRaid(uintptr_t base) {
    uintptr_t root = mapRoot(base);
    if (root != g_mtRaidRoot) { g_mtRaidRoot = root; g_mtLandMap = false; g_mtLandBag = false; }
}

static bool mtHereMap(uintptr_t base) {
    if (currentAxContext() != AX_MAP) return false;
    if (!mapRoot(base)) return false;
    if (pitInMatch(base)) return false;
    if (!axTutorialSeen("dungeon")) return false;
    if (!g_mtLandMap) return false;
    return true;
}

static bool mtHereBag(uintptr_t base) {
    if (currentAxContext() != AX_INV) return false;
    if (!mapRoot(base)) return false;
    if (pitInMatch(base)) return false;
    if (!axTutorialSeen("dungeon")) return false;
    if (!g_mtLandBag && g_invSlot < 0) return false;
    return true;
}

static bool mtHereCamp(uintptr_t base) {
    AxContext c = currentAxContext();
    if (campInMealPhase(base)) {
        if (c != AX_MEAL) return false;
    } else if (campInSkillPhase(base)) {
        if (c != AX_ACTIONS && c != AX_INGAME && c != AX_ROOM && c != AX_QUEST && c != AX_LOG &&
            c != AX_CAMPTARGET) return false;
    } else return false;
    if (!axTutorialSeen("dungeon")) return false;
    return true;
}

static bool mtHereSheet(uintptr_t base) {
    (void)base;
    return currentAxContext() == AX_CHARSHEET;
}

static bool mtHereHamlet(uintptr_t base) {
    AxContext c = currentAxContext();
    if (c != AX_TOWNMAP && c != AX_ROSTER && c != AX_RESOURCES && c != AX_TOWNMENU) return false;
    uintptr_t root = resTownRoot(base);
    if (!root) return false;
    if (!tmbBareMap(root)) return false;
    return true;
}

static bool mtHereEmbark(uintptr_t base) {
    AxContext c = currentAxContext();
    if (c != AX_EMBARK && c != AX_PARTY && c != AX_ROSTER && c != AX_TOWNMENU) return false;
    uintptr_t root = resTownRoot(base);
    if (!root) return false;
    if (!embQsOpen(root)) return false;
    if (embProvIsOpen(root)) return false;
    return true;
}

static bool mtHereProvision(uintptr_t base) {
    AxContext c = currentAxContext();
    if (c != AX_PROVISION && c != AX_PARTY && c != AX_ROSTER && c != AX_TOWNMENU) return false;
    uintptr_t root = resTownRoot(base);
    if (!root) return false;
    if (!embProvIsOpen(root)) return false;
    return true;
}

// ---- The Butcher's Circus ----
static bool mtCircusBuilding(uintptr_t base, uintptr_t* panelOut) {
    uintptr_t root = resTownRoot(base);
    uintptr_t panel = root ? bldOpenPanel(root) : 0;
    if (panelOut) *panelOut = panel;
    if (!panel) return false;
    return bdIsPanel(base, panel) || dgIsPanel(base, panel) || pbIsPanel(base, panel) ||
           rbIsPanel(base, panel);
}
static bool mtHereBuilding(uintptr_t base) {
    if (currentAxContext() != AX_BLDG) return false;
    if (!resTownRoot(base)) return false;
    if (mtCircusBuilding(base, nullptr)) return false;
    return true;
}

static bool mtHereBanner(uintptr_t base) {
    uintptr_t panel = 0;
    if (currentAxContext() != AX_BLDG) return false;
    return mtCircusBuilding(base, &panel) && bdIsPanel(base, panel);
}
static bool mtHereDueling(uintptr_t base) {
    uintptr_t panel = 0;
    if (currentAxContext() != AX_BLDG) return false;
    return mtCircusBuilding(base, &panel) && dgIsPanel(base, panel);
}
static bool mtHerePrizeBox(uintptr_t base) {
    uintptr_t panel = 0;
    if (currentAxContext() != AX_BLDG) return false;
    return mtCircusBuilding(base, &panel) && pbIsPanel(base, panel);
}
static bool mtHereRankings(uintptr_t base) {
    uintptr_t panel = 0;
    if (currentAxContext() != AX_BLDG) return false;
    return mtCircusBuilding(base, &panel) && rbIsPanel(base, panel);
}

static bool mtHereRing(uintptr_t base) {
    AxContext c = currentAxContext();
    if (c != AX_RING && c != AX_RINGLIST) return false;
    return ringPanel(base) != 0;
}

static const MtTopic kMtTopics[] = {
    { "title", mtHereTitle, 9,
      { AXS_MT_TITLE_P1, AXS_MT_TITLE_P2, AXS_MT_TITLE_HINTS, AXS_MT_TITLE_P3, AXS_MT_TITLE_P4,
        AXS_MT_TITLE_P5, AXS_MT_TITLE_P6, AXS_MT_TITLE_P7, AXS_MT_TITLE_P8 } },
    { "campaign", mtHereCampaign, 6,
      { AXS_MT_CAMP_P1, AXS_MT_CAMP_P2, AXS_MT_CAMP_P3, AXS_MT_CAMP_P4, AXS_MT_CAMP_P5,
        AXS_MT_CAMP_P6 } },
    { "bag", mtHereBag, 5,
      { AXS_MT_BAG_P1, AXS_MT_BAG_P2, AXS_MT_BAG_P3, AXS_MT_BAG_P4, AXS_MT_BAG_P5 } },
    { "camp", mtHereCamp, 3, { AXS_MT_CAMPING_P1, AXS_MT_CAMPING_P2, AXS_MT_CAMPING_P3 } },
    { "dungeon", mtHereDungeon, 12,
      { AXS_MT_DUNG_P1, AXS_MT_DUNG_P2, AXS_MT_DUNG_P3, AXS_MT_DUNG_P4, AXS_MT_DUNG_P5,
        AXS_MT_DUNG_P6, AXS_MT_DUNG_P7, AXS_MT_DUNG_P8, AXS_MT_DUNG_P9, AXS_MT_DUNG_P10,
        AXS_MT_DUNG_P11, AXS_MT_DUNG_P12 } },
    { "map", mtHereMap, 5,
      { AXS_MT_MAP_P1, AXS_MT_MAP_P2, AXS_MT_MAP_P3, AXS_MT_MAP_P4, AXS_MT_MAP_P5 } },
    { "sheet", mtHereSheet, 11,
      { AXS_MT_CS_P1, AXS_MT_CS_P2, AXS_MT_CS_P3, AXS_MT_CS_P4, AXS_MT_CS_P5, AXS_MT_CS_P6,
        AXS_MT_CS_P7, AXS_MT_CS_P8, AXS_MT_CS_P9, AXS_MT_CS_P10, AXS_MT_CS_P11 } },
    { "hamlet", mtHereHamlet, 9,
      { AXS_MT_HAM_P1, AXS_MT_HAM_P2, AXS_MT_HAM_P3, AXS_MT_HAM_P4, AXS_MT_HAM_P5,
        AXS_MT_HAM_P6, AXS_MT_HAM_P7, AXS_MT_HAM_P8, AXS_MT_HAM_P9 } },
    { "embark", mtHereEmbark, 5,
      { AXS_MT_EMB_P1, AXS_MT_EMB_P2, AXS_MT_EMB_P3, AXS_MT_EMB_P4, AXS_MT_EMB_P5 } },
    { "provision", mtHereProvision, 6,
      { AXS_MT_PROV_P1, AXS_MT_PROV_P2, AXS_MT_PROV_P3, AXS_MT_PROV_P4, AXS_MT_PROV_P5,
        AXS_MT_PROV_P6 } },
    { "building", mtHereBuilding, 1, { AXS_MT_BLDG_P1 } },
    { "banner", mtHereBanner, 5,
      { AXS_MT_BANNER_P1, AXS_MT_BANNER_P2, AXS_MT_BANNER_P3, AXS_MT_BANNER_P4,
        AXS_MT_BANNER_P5 } },
    { "dueling", mtHereDueling, 2, { AXS_MT_DUEL_P1, AXS_MT_DUEL_P2 } },
    { "prizebox", mtHerePrizeBox, 1, { AXS_MT_PRIZE_P1 } },
    { "rankings", mtHereRankings, 2, { AXS_MT_RANK_P1, AXS_MT_RANK_P2 } },
    { "ring", mtHereRing, 6,
      { AXS_MT_RING_P1, AXS_MT_RING_P2, AXS_MT_RING_P3, AXS_MT_RING_P4, AXS_MT_RING_P5,
        AXS_MT_RING_P6 } },
};
static const int MT_TOPICS = (int)(sizeof kMtTopics / sizeof kMtTopics[0]);

// ---- State ----
bool g_mtActive = false;
static int  g_mtTopic = -1;               // index into kMtTopics
static int  g_mtRow = 0;
static int  g_mtFnSayAgain = -2, g_mtFnTutorial = -2, g_mtFnSettings = -2, g_mtFnFind = -2;
static int  g_mtFnHints = -2, g_mtFnTips = -2;

bool axIsModTut() { return g_mtActive; }

static int mtRows()        { return g_mtTopic >= 0 ? kMtTopics[g_mtTopic].paras + 2 : 0; }
static int mtCheckboxRow() { return g_mtTopic >= 0 ? kMtTopics[g_mtTopic].paras : -1; }
static int mtOkRow()       { return g_mtTopic >= 0 ? kMtTopics[g_mtTopic].paras + 1 : -1; }

static void mtCheckboxLine(char* out, int outsz) {
    _snprintf(out, outsz, axs(AXS_MT_OPTOUT_FMT),
              axModTutorials() ? axs(AXS_VALUE_UNCHECKED) : axs(AXS_VALUE_CHECKED));
    out[outsz - 1] = 0;
}

// One row's text, without its position count.
static void mtRowLine(int row, char* out, int outsz) {
    out[0] = 0;
    if (g_mtTopic < 0) return;
    const MtTopic& t = kMtTopics[g_mtTopic];
    if (row >= 0 && row < t.paras)   { _snprintf(out, outsz, "%s", axs(t.para[row])); }
    else if (row == mtCheckboxRow()) { mtCheckboxLine(out, outsz); axAppendHint(out, outsz, AXS_ENTER_ACTIVATE_HINT); return; }
    else if (row == mtOkRow())       { _snprintf(out, outsz, "%s", axs(AXS_MT_OK)); out[outsz - 1] = 0;
                                       axAppendHint(out, outsz, AXS_ENTER_ACTIVATE_HINT); return; }
    out[outsz - 1] = 0;
}

static void mtSpeakRow(const char* prefix) {
    char line[1024];
    mtRowLine(g_mtRow, line, sizeof line);
    char posln[64];
    _snprintf(posln, sizeof posln, axs(AXS_POS_N_OF_M), g_mtRow + 1, mtRows());
    posln[sizeof posln - 1] = 0;
    char utter[MAILBOX_SZ];
    _snprintf(utter, sizeof utter, "%s%s%s %s", prefix ? prefix : "", (prefix && prefix[0]) ? " " : "",
              line, posln);
    utter[sizeof utter - 1] = 0;
    postSpeech(utter);
}

// ---- FIND (Ctrl+F) over the dialog ----
int mtFind(uintptr_t base, AxFindOp op, int idx, char* out, int outsz) {
    (void)base;
    if (!g_mtActive || g_mtTopic < 0) return 0;
    int n = mtRows();
    if (op == AXF_COUNT) return n;
    if (idx < 0 || idx >= n) return 0;
    if (op == AXF_LABEL) {
        if (!out || outsz <= 0) return 0;
        const MtTopic& t = kMtTopics[g_mtTopic];
        if (idx < t.paras)                _snprintf(out, outsz, "%s", axs(t.para[idx]));
        else if (idx == mtCheckboxRow())  mtCheckboxLine(out, outsz);
        else                              _snprintf(out, outsz, "%s", axs(AXS_MT_OK));
        out[outsz - 1] = 0;
        return 1;
    }
    if (op == AXF_GO) {
        g_mtRow = idx;
        logLine("modtutorial: find landed on row %d (of %d)", idx, n);
        mtSpeakRow(nullptr);
        return 1;
    }
    return 0;
}

void mtReannounce(uintptr_t base) {
    (void)base;
    if (!g_mtActive || g_mtTopic < 0) return;
    const MtTopic& t = kMtTopics[g_mtTopic];
    char utter[MAILBOX_SZ];
    utter[0] = 0;
    int cur = 0;
    for (int i = 0; i < t.paras; i++) {
        _snprintf(utter + cur, sizeof utter - cur, "%s%s", cur ? " " : "", axs(t.para[i]));
        utter[sizeof utter - 1] = 0;
        cur = (int)strlen(utter);
    }
    char line[512];
    mtCheckboxLine(line, sizeof line);
    _snprintf(utter + cur, sizeof utter - cur, " %s", line);
    utter[sizeof utter - 1] = 0;
    cur = (int)strlen(utter);
    _snprintf(utter + cur, sizeof utter - cur, " %s", axs(AXS_MT_OK));
    utter[sizeof utter - 1] = 0;
    if ((int)strlen(utter) >= MAILBOX_SZ - 1)
        logLine("modtutorial: \"%s\" fills the mailbox (%d bytes) -- the end is cut", t.id, MAILBOX_SZ);
    postSpeech(utter);
}

// ---- Open / close ----
static void mtOpen(uintptr_t base, int topic, const char* why) {
    if (g_mtActive || topic < 0 || topic >= MT_TOPICS) return;
    g_mtTopic = topic;
    g_mtRow = 0;
    g_mtActive = true;
    if (!axTutorialSeen(kMtTopics[topic].id)) axMarkTutorialSeen(kMtTopics[topic].id);
    logLine("modtutorial: opened \"%s\" (%s; ctx underneath %d)", kMtTopics[topic].id, why,
            (int)currentAxContext());
    mtReannounce(base);
}

static void mtClose(uintptr_t base, const char* why) {
    g_mtActive = false;
    logLine("modtutorial: closed \"%s\" (%s)", g_mtTopic >= 0 ? kMtTopics[g_mtTopic].id : "?", why);
    g_mtTopic = -1;
    if (!axModalClosed(base, "modtutorial")) postSpeech(axs(AXS_MT_CLOSED));
}

static void mtToggleOptOut() {
    bool show = !axModTutorials();
    axSetModTutorials(show);
    logLine("modtutorial: mod tutorials %s (the dialog's checkbox)", show ? "on" : "off");
    char line[1024];
    mtCheckboxLine(line, sizeof line);
    if (!show) axAppendHint(line, sizeof line, AXS_MT_OPTOUT_HINT);
    char posln[64];
    _snprintf(posln, sizeof posln, axs(AXS_POS_N_OF_M), g_mtRow + 1, mtRows());
    posln[sizeof posln - 1] = 0;
    char utter[MAILBOX_SZ];
    _snprintf(utter, sizeof utter, "%s %s", line, posln);
    utter[sizeof utter - 1] = 0;
    postSpeech(utter);
}

void mtService(uintptr_t base) {
    mtTrackRaid(base);
    if (g_mtActive || !axModTutorials()) return;
    for (int i = 0; i < MT_TOPICS; i++) {
        if (axTutorialSeen(kMtTopics[i].id)) continue;
        if (!kMtTopics[i].here(base)) continue;
        mtOpen(base, i, "auto");
        return;
    }
}

void mtOpenF1(uintptr_t base) {
    if (g_mtActive) { mtReannounce(base); return; }
    static bool checked = false;
    if (!checked) {
        checked = true;
        char who[64] = "";
        bool owned = kbActionForKey(base, (int)SDLK_F1, who, sizeof who);
        logLine("modtutorial: F1 game-binding check: %s", owned ? who : "not bound");
    }
    for (int i = 0; i < MT_TOPICS; i++) {
        if (!kMtTopics[i].here(base)) continue;
        mtOpen(base, i, "F1");
        return;
    }
    logLine("modtutorial: F1 with no topic for context %d", (int)currentAxContext());
    postSpeech(axs(AXS_MT_NONE_HERE));
}

// ---- Keys ----
bool routeModTutKey(uintptr_t base, uint32_t sym, uint16_t mod, uint8_t repeat) {
    if (!g_mtActive) return false;
    if (g_mtFnSayAgain == -2) g_mtFnSayAgain = kmFind("general.say_again");
    if (g_mtFnTutorial == -2) g_mtFnTutorial = kmFind("general.mod_tutorial");
    if (g_mtFnSettings == -2) g_mtFnSettings = kmFind("general.settings");
    if (g_mtFnFind == -2)     g_mtFnFind     = kmFind("general.find");
    if (g_mtFnHints == -2)    g_mtFnHints    = kmFind("general.control_hints");
    if (g_mtFnTips == -2)     g_mtFnTips     = kmFind("general.auto_tooltips");
    const uint16_t CTRL = (uint16_t)(KMOD_LCTRL | KMOD_RCTRL);
    const uint16_t ALT  = (uint16_t)(KMOD_LALT | KMOD_RALT);
    const uint16_t SHIFT = (uint16_t)(KMOD_LSHIFT | KMOD_RSHIFT);
    bool bare = !(mod & (CTRL | ALT | SHIFT));
    bool menuKey = g_mtFnSettings >= 0 ? kmIsCurrentKey(g_mtFnSettings, sym, mod) : (sym == SDLK_F10 && bare);
    bool findKey = g_mtFnFind >= 0     ? kmIsCurrentKey(g_mtFnFind, sym, mod)     : (sym == SDLK_f && (mod & CTRL) != 0);
    bool hintKey = g_mtFnHints >= 0    ? kmIsCurrentKey(g_mtFnHints, sym, mod)    : (sym == SDLK_F2 && bare);
    bool tipsKey = g_mtFnTips >= 0     ? kmIsCurrentKey(g_mtFnTips, sym, mod)     : (sym == SDLK_F3 && bare);
    if (menuKey || findKey || hintKey || tipsKey) return false;   // the router's own blocks take these
    bool sayAgain = g_mtFnSayAgain >= 0 ? kmIsCurrentKey(g_mtFnSayAgain, sym, mod) : (sym == SDLK_COMMA);
    bool tutKey   = g_mtFnTutorial >= 0 ? kmIsCurrentKey(g_mtFnTutorial, sym, mod) : (sym == SDLK_F1 && bare);
    if (sayAgain || tutKey) { if (!repeat) mtReannounce(base); return true; }
    if (sym == SDLK_ESCAPE) { if (!repeat) mtClose(base, "Escape"); return true; }
    if (sym == SDLK_RETURN || sym == SDLK_KP_ENTER || sym == SDLK_SPACE) {
        if (repeat) return true;
        if (g_mtRow == mtOkRow()) {
            if (sym == SDLK_SPACE) { mtSpeakRow(nullptr); return true; }   // Space is the box's key, not OK's
            mtClose(base, "OK");
        }
        else if (g_mtRow == mtCheckboxRow()) mtToggleOptOut();
        else mtSpeakRow(nullptr);                    // a paragraph: Enter re-reads it
        return true;
    }
    int dRow = 0, dCol = 0;
    if (axDecodeArrow(sym, mod, repeat, false, &dRow, &dCol)) {
        if (!dRow) return true;                      // throttled auto-repeat: claimed, no step
        axStepCursor(&g_mtRow, mtRows(), dRow);      // hard stop: the end row re-reads
        mtSpeakRow(nullptr);
        return true;
    }
    int jump = 0;
    if (axDecodeJump(sym, mod, repeat, &jump)) {
        if (!jump) return true;
        axStepCursor(&g_mtRow, mtRows(), jump);
        mtSpeakRow(nullptr);
        return true;
    }
    return true;                                     // every other key: the dialog swallows it
}
