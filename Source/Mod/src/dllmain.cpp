// Darkest Dungeon accessibility — focus announcer (MAIN-THREAD HOOK build "A").

#include <windows.h>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cmath>
#include "internal.h"

static const uintptr_t POOL_RECORDS_OFF = 0x98;      // pool+0x98: records array base
static const uintptr_t POOL_REC_STRIDE  = 0x120;     // bytes per record
static const uintptr_t POOL_REC_GEN_OFF = 0x114;     // record: generation (validity)
static const uintptr_t POOL_REC_OBJ_OFF = 0x08;      // record: resolved object ptr

// ---- The LOOT overlay (post-battle spoils / curio + chest treasure / camping) ----

// ---- The EVENT overlay: the "scroll" with a list of CHOICES ----

// ---- In-raid ACTION BAR ----

// ---- ROOM VIEW: the party + enemy rosters, the room's PROPS, DOORS and TRAPS ----

// ---- Keyboard bindings (the GATE CHECK table) ----
static const uintptr_t KEYBIND_STRIDE     = 0x48;
static const uintptr_t KEYBIND_NAME_OFF   = 0x00;    // char[0x40]
static const uintptr_t KEYBIND_KEYCODE_OFF= 0x44;    // int SDL keycode, 0 = unbound
static const int       KEYBIND_MAX        = 64;      // walk cap; the table's length is not known

int              g_trkPhase     = 0;
int              g_trkItemSlot  = -1;    // the chosen trinket's REAL realm-vector slot
char             g_trkItemName[256];
uintptr_t        g_trkHero      = 0;     // the hero chosen in the picker
uintptr_t        g_trkEntry     = 0;
DWORD            g_trkSheetWatchUntil = 0;
bool             g_trkToggleIssued = false;  // the panel re-open has been asked for once

// ---- A skill effect's STAT BUFFS/DEBUFFS: the game's own list builder ----

// ---- A skill effect's SUMMONS ("Summons: Skeleton Soldier") — pure reads ----

// ---- SKIP TURN + Skill ADDITIONAL EFFECTS + the action-bar virtual buffer ----

volatile void* g_raidDisplay = nullptr;

// ---- Tunables ----
static const int  FRAME_GATE_MS = 40;  // process focus at most this often (debounce base)
static const int  SETTLE        = 2;   // gated frames the id must hold before we speak
static const long OPT_RESCAN_WINDOW_MS = 400;

// ---- Shared state ----
volatile bool g_enabled = false;   // master on/off (set once the hook is live)
uintptr_t     g_base    = 0;
uintptr_t     g_tbwVtbl = 0;
static bool          g_inDialog = false;  // last spoken focus was a ConfirmDialog answer
bool                 g_inPauseMenu = false; // last focused item was a pause-menu button

const char* const    kTitleScreenName    = "Darkest Dungeon";
volatile bool        g_titlePrefixPending = false;

volatile bool        g_naming          = false; // the name field is open + being edited
char                 g_typed[64]       = { 0 }; // reconstructed box contents (seed + keystrokes)
char                 g_lastName[64]    = { 0 }; // last contents we spoke (de-dup)
static DWORD         g_lastActionTick   = 0;
volatile bool        g_inputIsController = true;
DWORD                g_commitCheckAt   = 0;
volatile bool g_textInputActive = false; // SDL text input on (Start/Stop hooks)
DWORD                g_lastKeyTick      = 0;    // last naming keystroke — keeps the session alive
bool                 g_reopenBlocked    = false; // set on Enter/Escape close; blocks re-open until

// ------------------------------------------------------------

static bool routeInputEvent(uintptr_t base, void* ev);

// ---- Shared list-navigation helpers ----

// ---- HELD-ARROW NAVIGATION ----
static const DWORD AX_HELD_STEP_MS = 150;

static uint32_t g_axEvScan    = 0;
static uint32_t g_axHoldScan  = 0;
static DWORD    g_axHoldPress = 0;  // tick when this hold was armed
static DWORD    g_axHoldStep  = 0;  // tick of the last granted step (press or repeat)
static bool     g_axHoldBurst = false;
                                    // cancelled speech, so the release owes a re-read

bool axDecodeArrow(uint32_t sym, uint16_t mod, uint8_t repeat, bool wantCols,
                          int* dRow, int* dCol) {
    *dRow = 0; *dCol = 0;
    if (mod & (KMOD_LALT | KMOD_RALT | KMOD_LCTRL | KMOD_RCTRL)) return false;
    bool isRow = (sym == SDLK_UP   || sym == SDLK_DOWN);
    bool isCol = (sym == SDLK_LEFT || sym == SDLK_RIGHT);
    if (!isRow && !isCol) return false;
    if (isCol && !wantCols) return false;
    if (axNavHoldRepeat(repeat)) return true;      // the throttle: claimed, no step this repeat
    if (isRow) *dRow = (sym == SDLK_DOWN)  ? 1 : -1;
    else       *dCol = (sym == SDLK_RIGHT) ? 1 : -1;
    return true;
}

bool axNavHoldRepeat(uint8_t repeat) {
    DWORD now = GetTickCount();
    if (!repeat) {
        // A fresh press arms the hold tracking on this physical key, and always steps.
        g_axHoldScan  = g_axEvScan;
        g_axHoldPress = now;
        g_axHoldStep  = now;
        g_axHoldBurst = false;
        return false;
    }
    if (g_axHoldScan != g_axEvScan) {
        g_axHoldScan  = g_axEvScan;
        g_axHoldPress = now;
        g_axHoldStep  = now - AX_HELD_STEP_MS;
    }
    g_axHoldBurst = true;                  // even a throttled-away repeat cancelled speech
    if ((DWORD)(now - g_axHoldStep) < AX_HELD_STEP_MS)
        return true;                       // the throttle: claimed, no step this repeat
    g_axHoldStep = now;
    return false;
}

bool axStepCursor(int* row, int count, int delta) {
    if (count <= 0) { *row = 0; return false; }
    int from = *row;
    int target = from + delta;
    if (target < 0)      target = 0;               // hard stop at the top
    if (target >= count) target = count - 1;       // hard stop: the caller re-reads the end row
    *row = target;
    return target != from;
}

bool axDecodeJump(uint32_t sym, uint16_t mod, uint8_t repeat, int* jump) {
    *jump = 0;
    if (sym != SDLK_HOME && sym != SDLK_END) return false;
    if (mod & (KMOD_LALT | KMOD_RALT | KMOD_LCTRL | KMOD_RCTRL)) return false;
    if (repeat) return true;                   // held jump: claimed, nowhere further to go
    *jump = (sym == SDLK_END) ? AX_JUMP : -AX_JUMP;
    return true;
}

// ---- The LOOT overlay: reader, cursor and appearance watch ----
bool         g_campCovers = false;

// ---- The EVENT overlay: reader, cursor and appearance watch ----

void resolveLabel(uintptr_t base, uintptr_t tbwVtbl, int64_t id,
                  char* text, int textsz) {
    bool wasInDialog = g_inDialog;
    g_inDialog = false;
    text[0] = 0;
    char idasc[9]; idToAscii(id, idasc);
    long vcount = focusVectorCount(base);

    {
        const PauseLabel* pe = lookupPause((uint32_t)(uint64_t)id);
        if (pe) {
            g_inPauseMenu = true;
            // Localized label via the game's string table; English fallback on a miss.
            char label[256];
            bool resolved = resolveKey(base, pe->key, label, sizeof label);
            if (!resolved) { strncpy(label, axs(pe->fallback), sizeof label - 1); label[sizeof label - 1] = 0; }
            _snprintf(text, textsz, "%s", label);
            text[textsz - 1] = 0;
            logLine("pausemenu id=0x%llx ascii=\"%s\" key=%s text=\"%s\" (%s)",
                    (unsigned long long)id, idasc, pe->key, text,
                    resolved ? "resolved" : "fallback");
            return;
        }
        g_inPauseMenu = false;
    }

    if (resolveInventory(base, id, text, textsz)) return;

    uintptr_t owner = findOwnerByFocusId(base, id);
    if (owner != 0) {
        char via[32]; via[0] = 0;
        bool got = extractWidgetText(owner, tbwVtbl, text, textsz, via, sizeof via);
        if (!got) { strncpy(text, "unlabeled", textsz - 1); text[textsz - 1] = 0; }
        logLine("ingame id=0x%llx ascii=\"%s\" vcount=%ld owner=0x%llx via=%s text=\"%s\"",
                (unsigned long long)id, idasc, vcount, (unsigned long long)owner,
                got ? via : "(none)", text);
        return;
    }

    if (resolveOptions(base, tbwVtbl, id, text, textsz)) return;

    uintptr_t display = frontEndDisplay(base);
    int feState = (display != 0) ? frontEndState(display) : -1;

    {
        uint32_t cc = (uint32_t)(uint64_t)id;
        if (cc - CONFIRM_ANSWER_BASE < (uint32_t)CONFIRM_ANSWER_MAX) {
            if (display != 0 &&
                resolveModeDialog(base, display, id, !wasInDialog, text, textsz)) {
                g_inDialog = true;
                return;
            }
            if (resolveGenericConfirm(base, id, !wasInDialog, text, textsz)) {
                g_inDialog = true;
                return;
            }
        }
    }

    if (display != 0 && feState == FE_STATE_SAVESLOTS) {
        if (resolveSaveSlot(display, id, text, textsz)) {
            logLine("saveslot id=0x%llx ascii=\"%s\" state=%d text=\"%s\"",
                    (unsigned long long)id, idasc, feState, text);
            return;
        }
    }

    uint32_t cc = (uint32_t)(uint64_t)id;
    const FourCcLabel* e = lookupFrontEnd(cc);
    if (e) {
        if (resolveKey(base, e->key, text, textsz)) {
            logLine("frontend id=0x%llx ascii=\"%s\" state=%d key=%s text=\"%s\" (resolved)",
                    (unsigned long long)id, idasc, feState, e->key, text);
        } else {
            strncpy(text, axs(e->fallback), textsz - 1); text[textsz - 1] = 0;
            logLine("frontend id=0x%llx ascii=\"%s\" state=%d key=%s text=\"%s\" (fallback)",
                    (unsigned long long)id, idasc, feState, e->key, text);
        }
    } else {
        strncpy(text, "unlabeled button", textsz - 1); text[textsz - 1] = 0;
        logLine("frontend id=0x%llx ascii=\"%s\" state=%d UNKNOWN",
                (unsigned long long)id, idasc, feState);
    }
}

static bool isUserActionEvent(uint32_t t) {
    return t == SDL_EVT_KEYDOWN || t == SDL_EVT_MOUSEBUTTONDOWN ||
           t == SDL_EVT_JOYBUTTONDOWN || t == SDL_EVT_CONTROLLERBUTTONDOWN;
}

int64_t g_axSpokenId = 0x7fffffffffffffffLL;
char    g_axLastSpoken[512] = { 0 };

// ---- Per-frame focus check (runs on the game's MAIN thread) ----
static void frameCheck() {
    // Debounce state — single-threaded (only the main thread ever runs this).
    static DWORD   lastRun   = 0;
    static int64_t pendingId = 0x7fffffffffffffffLL;
    static int     stable    = 0;

    if (axOwnsAnnouncer(currentAxContext())) return;

    if (g_preambleHold) {
        if ((long)(GetTickCount() - g_preambleHoldUntil) >= 0) g_preambleHold = false;
        else return;
    }

    DWORD now = GetTickCount();
    if (now - lastRun < (DWORD)FRAME_GATE_MS) return;   // gate: honour the debounce cadence
    lastRun = now;

    int64_t id = 0;
    if (!safeReadI64(g_base + FOCUS_ID_RVA, &id)) return;

    if (id != pendingId) { pendingId = id; stable = 0; return; }
    if (stable < SETTLE) { stable++; return; }
    if (isNoFocus(id))   return;

    if (id == g_axSpokenId) {
        if (isOptionId((uint32_t)id) &&
            (long)(now - g_lastActionTick) <= OPT_RESCAN_WINDOW_MS) {
            char vtext[512];
            resolveLabel(g_base, g_tbwVtbl, id, vtext, sizeof vtext);   // refreshes g_optValueOnly
            if (vtext[0] && strcmp(vtext, g_axLastSpoken) != 0) {
                // Speak only the value ("Low" / "79" / "english"); fall back to the whole line.
                postSpeech(g_optValueOnly[0] ? g_optValueOnly : vtext);
                strncpy(g_axLastSpoken, vtext, sizeof g_axLastSpoken - 1);
                g_axLastSpoken[sizeof g_axLastSpoken - 1] = 0;
            }
        }
        return;
    }

    char text[512];
    resolveLabel(g_base, g_tbwVtbl, id, text, sizeof text);
    g_axSpokenId = id;

    if (g_titlePrefixPending) {
        g_titlePrefixPending = false;
        uintptr_t disp = frontEndDisplay(g_base);
        if (disp && frontEndState(disp) == FE_STATE_TITLE && text[0]) {
            char combined[600];
            _snprintf(combined, sizeof combined, "%s. %s", kTitleScreenName, text);
            combined[sizeof combined - 1] = 0;
            postSpeech(combined);
            strncpy(g_axLastSpoken, text, sizeof g_axLastSpoken - 1);
            g_axLastSpoken[sizeof g_axLastSpoken - 1] = 0;
            logLine("title screen announced: \"%s\"", combined);
            return;
        }
    }

    if (text[0] && strcmp(text, g_axLastSpoken) == 0) return;   // same words: stay quiet
    postSpeech(text);
    strncpy(g_axLastSpoken, text, sizeof g_axLastSpoken - 1);
    g_axLastSpoken[sizeof g_axLastSpoken - 1] = 0;
}

// ---- The raid LAYER-STATE family ----
volatile bool g_abActive = false;
volatile bool g_rvActive = false;
int  g_rvCursor = -1;
volatile bool g_qtActive = false;
int g_abRoomReturn = -1;
volatile bool g_tsActive = false;
int  g_abCursor = -1;
int  g_abTipLine = 0;
bool g_abDumped = false;            // the one-shot diagnostic dump has been written

// ---- routeSkillHotkey IS GONE ----

bool routePanelJump(uintptr_t base, bool toMap, uint8_t repeat) {
    if (repeat) return true;
    uintptr_t rd = (uintptr_t)g_raidDisplay;
    if (rd <= 0x10000) return false;           // banner hasn't ticked yet; not ours to claim

    int target = toMap ? 0 : 2;
    uintptr_t p = 0;
    if (!safeReadPtr(rd + RD_PANEL_ARR_OFF + (uintptr_t)target * 8, &p) || p <= 0x10000) {
        logLine("paneljump: no panel at index %d", target);
        return false;
    }

    if (g_rvActive) rvSetActive(base, false);
    if (g_abActive) abSetActive(base, false);
    if (g_qtActive) qtSetActive(base, false);
    if (g_tsActive) { tsSetActive(false); logLine("targeting: abandoned (panel jump)"); }
    iuAbandon("panel jump");
    lootStepAside(toMap ? "M (map panel)" : "I (inventory panel)");

    uint32_t idx = 0;
    safeReadU32(rd + RD_ACTIVE_IDX_OFF, &idx);
    if ((int)idx == target) {
        if (toMap) {
            uintptr_t root = mapRoot(base);
            if (root) openMapReview(base, root);
            return true;
        }
        char buf[MAILBOX_SZ];
        if (g_invSlot >= 0 && invSlotText(base, g_invSlot, buf, sizeof buf)) postSpeech(buf);
        else postSpeech(axs(AXS_INV_TITLE));
        return true;
    }

    logLine("paneljump: selector %d -> %d (%s)", (int)idx, target, toMap ? "map" : "inventory");
    safeWriteU32(rd + RD_ACTIVE_IDX_OFF, (uint32_t)target);
    return true;
}

// ---- One line per raid: the formation the game actually dealt ----
static bool g_formLogged = false;

static void serviceFormationLog(uintptr_t base) {
    if (!g_raidDisplay) { g_formLogged = false; return; }   // out of a raid: arm for the next one
    if (g_formLogged) return;
    char line[512];
    roFormationText(base, line, sizeof line);
    if (!line[0]) return;                                   // no party to read yet — try next frame
    g_formLogged = true;
    logLine("raid formation: %s", line);
}

// ---- Keyboard bindings: is a key the GAME's? ----
bool kbActionForKey(uintptr_t base, int keycode, char* nameOut, int outsz) {
    if (nameOut && outsz) nameOut[0] = 0;
    if (!keycode) return false;
    uintptr_t tbl = base + KEYBIND_TABLE_RVA;
    for (int i = 0; i < KEYBIND_MAX; i++) {
        uintptr_t rec = tbl + (uintptr_t)i * KEYBIND_STRIDE;
        char name[0x44];
        if (!safeReadCStr(rec + KEYBIND_NAME_OFF, name, sizeof name)) break;
        if (!abPlausibleName(name)) break;          // ran off the end of the table
        uint32_t code = 0;
        if (!safeReadU32(rec + KEYBIND_KEYCODE_OFF, &code)) continue;
        if ((int32_t)code != keycode) continue;
        if (nameOut && outsz) { strncpy(nameOut, name, outsz - 1); nameOut[outsz - 1] = 0; }
        return true;
    }
    return false;
}

static void kbDumpTable(uintptr_t base) {
    static bool done = false;
    if (done) return;
    done = true;

    uintptr_t tbl = base + KEYBIND_TABLE_RVA;
    int n = 0;
    for (int i = 0; i < KEYBIND_MAX; i++) {
        uintptr_t rec = tbl + (uintptr_t)i * KEYBIND_STRIDE;
        char name[0x44];
        if (!safeReadCStr(rec + KEYBIND_NAME_OFF, name, sizeof name)) break;
        if (!abPlausibleName(name)) break;
        uint32_t code = 0;
        safeReadU32(rec + KEYBIND_KEYCODE_OFF, &code);
        char shown = (code >= 0x20 && code < 0x7f) ? (char)code : '?';
        logLine("keybind[%d] \"%s\" -> keycode %u (0x%x) '%c'%s",
                i, name, code, code, shown, code ? "" : "  <- UNBOUND");
        n++;
    }

    char owner[0x44];
    logLine("keybind: 'm' -> %s", kbActionForKey(base, 'm', owner, sizeof owner) ? owner : "NOT BOUND");
    logLine("keybind: 'i' -> %s", kbActionForKey(base, 'i', owner, sizeof owner) ? owner : "NOT BOUND");
    logLine("keybind: %d entries read before the walk stopped", n);
}

// ---- Context resolver ----

// Is this context up? Evaluated in table order; the first true one wins.
bool axIsNaming()  { return g_naming; }
bool axIsLoading() { return g_lsSuppressFocus; }

bool axIsPause()   { return g_pauseOpen || g_axOptionsUp; }
static bool axIsActions() { return g_abActive && mapRoot(g_base); }
static bool axIsRoom()    { return g_rvActive && mapRoot(g_base); }
static bool axIsQuest()   { return g_qtActive && mapRoot(g_base); }
static bool axIsQuestDone() { return g_qcOpen; }
static bool axIsTarget()  { return g_tsActive && mapRoot(g_base); }
static bool axIsItemUse() { return g_iuActive && g_invActive; }
static bool axIsCharSheet() { return g_csOpen && !g_ruEquipHero; }
static bool axIsMap()     { return g_mapReview; }
static bool axIsInv()     { return g_invActive; }
static bool axIsLog()     { return g_clogOpen && mapRoot(g_base) != 0; }
static bool axIsInGame()  { return mapRoot(g_base) != 0; }
static bool axIsTitle()   { return frontEndDisplay(g_base) != 0; }

static bool axIsResults() { return rrDisplay(g_base) != 0; }

typedef bool (*AxActiveFn)();
typedef bool (*AxKeyFn)(uintptr_t base, uint32_t sym, uint16_t mod, uint8_t repeat);
typedef void (*AxReannounceFn)(uintptr_t base);

struct AxContextDef {
    AxContext   ctx;
    AxActiveFn  active;         // is it up? — table order decides who wins
    AxKeyFn     keys;           // nullptr = claims no keys
    bool        ownsAnnouncer;
    const char* name;           // for the log
    AxReannounceFn reannounce;
    bool isMessage;
};

static const AxContextDef kAxContexts[] = {
    { AX_SETTINGS, axIsSettings, routeSettingsKey, true, "settings", smReannounce },
    // modal layers: the game's focus sits underneath and must not be voiced
    { AX_NAMING,  axIsNaming,  nullptr,          true,  "naming"    },
    { AX_LOADING, axIsLoading, nullptr,          true,  "loading",  lsReannounce, true },
    { AX_TUTORIAL, tutPopupActive, routeTutorialKey, true, "tutorial", tutReannounce, true },
    { AX_GLOSSARY, axIsGlossary, routeGlossaryKey, true, "glossary", glReannounce },
    { AX_HELP, axIsHelp, routeHelpKey, true, "help", hpReannounce, true },
    { AX_CONTROLS, axIsControls, routeControlsKey, true, "controls", cpReannounce, true },
    { AX_PAUSE,   axIsPause,   routeFrontEndKey, false, "pause",    feReannounce },
    { AX_DIALOG,  axIsDialog,  routeDialogKey,   false, "dialog",   cdReannounce, true },
    { AX_JOURNAL, axIsJournal, routeJournalKey,  true,  "journal",  jpReannounce, true },
    { AX_TOWNEVENT, axIsTownEvent, nullptr, true, "townevent", teReannounce, true },
    { AX_RESULTS, axIsResults, routeResultsKey,  true,  "results",  rrReannounce },
    { AX_TOWNLOG, axIsTownLog, routeTownLogKey,  true,  "townlog",  tlReannounce },
    { AX_EXCHANGE, axIsExchange, routeExchangeKey, true, "exchange", exReannounce },
    { AX_TRKPICK,  axIsTrkPick,  routeTrkPickKey,  true, "trinketpick", trkReannounce },
    { AX_REALMINV, axIsRealmInv, routeRealmInvKey, true, "realminv", riReannounce },
    { AX_RINGLIST, axIsRingList, routeRingListKey, true, "ringlist", ringReannounce },
    { AX_RING,     axIsRing,     routeRingKey,     true, "ring",     ringReannounce },
    { AX_ROSTER, axIsRoster, routeRosterKey, true, "roster", rosReannounce },
    { AX_PARTY, axIsParty, routePartyKey, true, "party", ptyReannounce },
    { AX_PROVISION, axIsProvision, routeProvisionKey, true, "provision", provReannounce },
    { AX_EMBARK, axIsEmbark, routeEmbarkKey, true, "embark", embReannounce },
    { AX_RESOURCES, axIsResources, routeResourcesKey, true, "resources", resReannounce },
    { AX_BLDG, axIsBld, routeBldKey, true, "building", bldReannounce },
    { AX_DISTRICT, axIsDistrict, routeDistrictKey, true, "districts", dstReannounce },
    { AX_TOWNMAP, axIsTownMap, routeTownMapKey, true, "townmap", tmReannounce },
    { AX_RAIDFINISH, axIsRaidFinish, routeRaidFinishKey, true, "raidfinish" },
    { AX_LOOT,    axIsLoot,    routeLootKey,     true,  "loot",     lootReannounce },
    { AX_EVENT,   axIsEvent,   routeEventKey,    true,  "event",    evReannounce, true },
    { AX_MEAL,    axIsMeal,    routeMealKey,     true,  "meal",     mealReannounce },
    { AX_CAMPTARGET, axIsCampTarget, routeCampTargetKey, true, "camptarget" },
    { AX_CHARSHEET, axIsCharSheet, routeCharSheetKey, true, "charsheet" },
    { AX_LOG,     axIsLog,     routeLogKey,      true,  "combatlog" },
    { AX_ITEMUSE, axIsItemUse, routeItemUseKey,  false, "itemuse"   },
    { AX_TARGET,  axIsTarget,  routeTargetKey,   false, "target"    },
    { AX_QUESTDONE, axIsQuestDone, routeQuestDoneKey, true, "questdone", qcReannounce, true },
    { AX_QUEST,   axIsQuest,   routeQuestKey,    false, "quest"     },
    { AX_ROOM,    axIsRoom,    routeRoomKey,     false, "room"      },
    { AX_ACTIONS, axIsActions, routeActionKey,   false, "actions"   },
    { AX_MAP,     axIsMap,     routeMapKey,      true,  "map"       },
    { AX_INV,     axIsInv,     routeInvKey,      false, "inventory" },
    { AX_INGAME,  axIsInGame,  nullptr,          false, "ingame"    },
    // front-end, and the catch-all
    { AX_TITLE,   axIsTitle,   routeFrontEndKey, false, "title",    feReannounce },
};
static const int kAxContextCount = (int)(sizeof kAxContexts / sizeof kAxContexts[0]);

// The active row, or nullptr when nothing special is up (AX_NONE).
static const AxContextDef* currentAxDef() {
    for (int i = 0; i < kAxContextCount; i++)
        if (kAxContexts[i].active()) return &kAxContexts[i];
    return nullptr;
}

// ---- A MODAL THE PLAYER WAS READING HAS GONE AWAY ----
bool axModalClosed(uintptr_t base, const char* who) {
    if (g_speakUntil) {
        logLine("%s closed: releasing the remaining speech hold", who);
        g_speakUntil = 0;
    }
    const AxContextDef* d = currentAxDef();
    if (!d || !d->reannounce) {
        logLine("%s closed; context \"%s\" does not re-announce", who, d ? d->name : "none");
        return false;
    }
    logLine("%s closed -> handing focus back to \"%s\"", who, d->name);
    d->reannounce(base);
    return true;
}

AxContext currentAxContext() {
    const AxContextDef* d = currentAxDef();
    return d ? d->ctx : AX_NONE;
}

bool axOwnsAnnouncer(AxContext c) {
    for (int i = 0; i < kAxContextCount; i++)
        if (kAxContexts[i].ctx == c) return kAxContexts[i].ownsAnnouncer;
    return false;
}

static void traceAxContext() {
    static AxContext last = (AxContext)-1;
    const AxContextDef* d = currentAxDef();
    AxContext c = d ? d->ctx : AX_NONE;
    if (c != last) { logLine("axcontext -> %s", d ? d->name : "none"); last = c; }
}

// ---- Per-context keyboard handlers ----

// ---- Input router ----

static bool g_ctrlDown[2] = { false, false };

static void axSayAgain(uintptr_t base);

static bool routeInputEvent(uintptr_t base, void* ev) {
    uint32_t type = 0;
    if (!safeReadU32((uintptr_t)ev, &type)) return false;

    if (type == SDL_EVT_KEYUP && g_twStepping) {
        uint32_t uscan = 0; safeReadU32((uintptr_t)ev + SDL_KEY_SCAN_OFF, &uscan);
        if (uscan && uscan == g_twScan) return true;
    }
    if (type == SDL_EVT_KEYUP) {
        uint32_t usym = 0; safeReadU32((uintptr_t)ev + SDL_KEY_SYM_OFF, &usym);
        if (usym == SDLK_LCTRL) g_ctrlDown[0] = false;
        if (usym == SDLK_RCTRL) g_ctrlDown[1] = false;
    }
    if (type == SDL_EVT_KEYUP && g_axHoldScan) {
        uint32_t uscan = 0; safeReadU32((uintptr_t)ev + SDL_KEY_SCAN_OFF, &uscan);
        if (uscan == g_axHoldScan) {
            if (g_axHoldBurst && g_lastNavLine[0]
                && (long)(g_lastNavTick - g_axHoldPress) >= 0
                && (DWORD)(GetTickCount() - g_lastNavTick) <= 1000) {
                logLine("navhold: release re-read \"%.60s\"", g_lastNavLine);
                postSpeech(g_lastNavLine);
            }
            g_axHoldScan = 0; g_axHoldBurst = false;
        }
    }
    if (type != SDL_EVT_KEYDOWN) return false;         // (text input etc. always passes)

    uint32_t sym = 0; safeReadU32((uintptr_t)ev + SDL_KEY_SYM_OFF, &sym);
    uint32_t m32 = 0; safeReadU32((uintptr_t)ev + SDL_KEY_MOD_OFF, &m32);
    uint8_t  rep = 0; safeReadU8((uintptr_t)ev + SDL_KEY_REPEAT_OFF, &rep);
    uint32_t scan = 0; safeReadU32((uintptr_t)ev + SDL_KEY_SCAN_OFF, &scan);
    uint16_t mod = (uint16_t)m32;

    if (g_axHoldScan && !rep && scan != g_axHoldScan
        && !(sym >= SDLK_LCTRL && sym <= SDLK_RGUI)) {
        g_axHoldScan = 0; g_axHoldBurst = false;
    }
    g_axEvScan = scan;

    // ---- The KEYBOARD LAYOUT FOLD (input/layout.cpp) ----
    klFold(base, &sym, scan);

    // ---- Ctrl = the player's silence key: drop the whole backlog ----
    int ci = (sym == SDLK_LCTRL) ? 0 : (sym == SDLK_RCTRL) ? 1 : -1;
    if (ci >= 0 && !g_ctrlDown[ci]) {
        g_ctrlDown[ci] = true;
        flushSpeech("player pressed Ctrl");
    }

    if (g_abActive && !mapRoot(base)) abSetActive(base, false);
    if (g_rvActive && !mapRoot(base)) rvSetActive(base, false);   // same, for the dungeon view
    if (g_iuActive && !mapRoot(base)) g_iuActive = false;         // and for a half-aimed item
    if (g_clogOpen && !mapRoot(base)) clogSetOpen(false);

    if (mapRoot(base)) kbDumpTable(base);

    const AxContextDef* d = currentAxDef();
    AxContext ctx = d ? d->ctx : AX_NONE;

    // ---- The mod settings menu (F10), and the key-remap translation ----
    if (ctx == AX_SETTINGS) return routeSettingsKey(base, sym, mod, rep);
    if (sym == SDLK_F10 && !g_textInputActive &&
        !(mod & (KMOD_LALT | KMOD_RALT | KMOD_LCTRL | KMOD_RCTRL |
                 KMOD_LSHIFT | KMOD_RSHIFT)) &&
        !(ctx == AX_BLDG && (g_bldRecDrag != 0 || g_qtOpen))) {
        if (!rep) {
            if (g_twStepping) twInterrupt(sym);
            smOpen(base);
        }
        return true;
    }
    if (kmRoute(base, ctx, &sym, &mod) == 2) return false;

    if ((sym == SDLK_a || sym == SDLK_d) && (mod & (KMOD_LSHIFT | KMOD_RSHIFT)) &&
        !(mod & (KMOD_LALT | KMOD_RALT | KMOD_LCTRL | KMOD_RCTRL)) && !g_textInputActive) {
        if (rep) return true;                          // auto-repeat: one press = one tile
        uint32_t liveScan = klScancodeForKey(sym);
        if (liveScan) scan = liveScan;
        return twBeginStep(base, sym, scan);
    }
    if (g_twStepping && !(sym >= 0x400000E0u && sym <= 0x400000E7u)) twInterrupt(sym);

    if (!tutPopupActive() &&
        townEventHeroKeysClaim() && routeTownEventHeroKey(base, sym, mod, rep)) return true;

    if ((sym == SDLK_RETURN || sym == SDLK_KP_ENTER) &&
        !(mod & (KMOD_LALT | KMOD_RALT | KMOD_LCTRL | KMOD_RCTRL)) &&
        !tutPopupActive() && townEventEnterClaims()) {
        if (rep) return true;                          // swallow auto-repeat, act once
        return routeTownEventEnter(base);
    }

    if (sym == SDLK_COMMA &&
        !(mod & (KMOD_LALT | KMOD_RALT | KMOD_LCTRL | KMOD_RCTRL |
                 KMOD_LSHIFT | KMOD_RSHIFT)) && !g_textInputActive) {
        if (rep) return true;                          // held: one readout per press, never a stutter
        axSayAgain(base);
        return true;
    }

    if (ctx == AX_BLDG && (g_bldRecDrag != 0 || g_qtOpen))
        return routeBldKey(base, sym, mod, rep);

    if (ctx == AX_BLDG && (sym == SDLK_r || sym == SDLK_b) && !g_textInputActive &&
        !(mod & (KMOD_LALT | KMOD_RALT | KMOD_LCTRL | KMOD_RCTRL)) && bdIsOpen(base))
        return routeBldKey(base, sym, mod, rep);

    if ((ctx == AX_TOWNMAP || ctx == AX_BLDG) && !g_textInputActive &&
        !(mod & (KMOD_LALT | KMOD_RALT | KMOD_LCTRL | KMOD_RCTRL))) {
        const char* jid = tjTargetForKey(sym);
        if (jid && routeTownJumpKey(base, jid, ctx == AX_BLDG, rep)) return true;
    }

    if (sym == SDLK_BACKQUOTE && !(mod & (KMOD_LALT | KMOD_RALT | KMOD_LCTRL | KMOD_RCTRL))) {
        if (ctx == AX_ACTIONS || ctx == AX_ROOM || ctx == AX_MAP || ctx == AX_INV ||
            ctx == AX_INGAME || ctx == AX_ITEMUSE)
            return routeActionToggle(base, sym, rep);
    }

    // ---- 1..5 ARE THE GAME'S OWN KEYS AND THE MOD NO LONGER TAKES THEM ----

    if (sym == SDLK_m && !(mod & (KMOD_LALT | KMOD_RALT | KMOD_LCTRL | KMOD_RCTRL))) {
        if (ctx == AX_ACTIONS || ctx == AX_ROOM || ctx == AX_MAP || ctx == AX_INV ||
            ctx == AX_INGAME || ctx == AX_QUEST || ctx == AX_ITEMUSE || ctx == AX_LOOT)
            return routePanelJump(base, true, rep);
    }

    if (sym == SDLK_r && !(mod & (KMOD_LALT | KMOD_RALT | KMOD_LCTRL | KMOD_RCTRL))) {
        // ---- The loot window RESTRICTS R ----
        if (g_lootActive) {
            if (ctx == AX_LOOT) {
                if (!rep) lootReannounce(base);
                return true;                       // claimed: one readout per press
            }
            if (ctx == AX_ROOM || ctx == AX_ACTIONS || ctx == AX_MAP || ctx == AX_INV ||
                ctx == AX_INGAME || ctx == AX_ITEMUSE || ctx == AX_QUEST) {
                if (rep) return true;              // held: one return per press, never a stutter
                lootReturn(base, "R");
                return true;
            }
        }
        if (ctx == AX_ROOM || ctx == AX_ACTIONS || ctx == AX_MAP || ctx == AX_INV ||
            ctx == AX_INGAME || ctx == AX_ITEMUSE)
            return routeRoomToggle(base, sym, rep);
        if (ctx == AX_RESOURCES || ctx == AX_PARTY || ctx == AX_ROSTER ||
            ctx == AX_TOWNMAP || ctx == AX_BLDG || ctx == AX_DISTRICT || ctx == AX_EMBARK ||
            ctx == AX_NONE)
            return routeResourcesToggle(base, sym, rep);
    }

    // ---- The loot window's step-aside, both directions ----
    if (sym == SDLK_TAB && ctx == AX_LOOT &&
        !(mod & (KMOD_LALT | KMOD_RALT | KMOD_LCTRL | KMOD_RCTRL |
                 KMOD_LSHIFT | KMOD_RSHIFT))) {
        lootStepAside("Tab");
        return false;                  // never claimed — the game owns the panel cycle
    }

    if (sym == SDLK_ESCAPE && g_lootActive && g_lootAside && !g_ruEquipHero && !g_ihHeld &&
        !(mod & (KMOD_LALT | KMOD_RALT | KMOD_LCTRL | KMOD_RCTRL))) {
        if (ctx == AX_INV || ctx == AX_MAP || ctx == AX_ROOM || ctx == AX_QUEST ||
            ctx == AX_ACTIONS || ctx == AX_INGAME) {
            if (rep) return true;                  // held: one return per press, never a stutter
            lootReturn(base, "Escape");
            return true;
        }
    }

    if (sym == SDLK_TAB && !(mod & (KMOD_LALT | KMOD_RALT | KMOD_LCTRL | KMOD_RCTRL |
                                    KMOD_LSHIFT | KMOD_RSHIFT))) {
        if (ctx == AX_PARTY || ctx == AX_ROSTER || ctx == AX_RESOURCES || ctx == AX_TOWNLOG ||
            ctx == AX_REALMINV || ctx == AX_EXCHANGE ||
            ctx == AX_TOWNMAP || ctx == AX_BLDG || ctx == AX_DISTRICT || ctx == AX_EMBARK ||
            ctx == AX_PROVISION || ctx == AX_NONE)
            return routeTownTab(base, sym, rep);
    }
    if (sym == SDLK_TAB && (mod & (KMOD_LSHIFT | KMOD_RSHIFT)) &&
        !(mod & (KMOD_LALT | KMOD_RALT | KMOD_LCTRL | KMOD_RCTRL))) {
        if (ctx == AX_PARTY || ctx == AX_ROSTER || ctx == AX_RESOURCES || ctx == AX_TOWNLOG ||
            ctx == AX_REALMINV || ctx == AX_EXCHANGE ||
            ctx == AX_TOWNMAP || ctx == AX_BLDG || ctx == AX_DISTRICT || ctx == AX_EMBARK ||
            ctx == AX_PROVISION || ctx == AX_NONE)
            return routeTownShiftTab(base, rep);
    }

    if (sym == SDLK_e && !(mod & (KMOD_LALT | KMOD_RALT | KMOD_LCTRL | KMOD_RCTRL))) {
        if ((ctx == AX_PARTY || ctx == AX_ROSTER) && g_rosPickSlot < 0 && !rosActivityPickLive()) {
            if (rep) return true;
            bool claimed = routeEmbarkForward(base, rep);
            if (claimed && g_embFwdWatchUntil) {
                if (g_ptyActive) ptyLeave(base, "E — handing over to the forward button");
                if (g_rosActive) rosLeave(base, "E — handing over to the forward button");
            }
            return claimed;
        }
        if (ctx == AX_TOWNMAP || ctx == AX_EMBARK || ctx == AX_PROVISION || ctx == AX_RING ||
            ctx == AX_NONE)
            return routeEmbarkForward(base, rep);
    }

    if (sym == SDLK_PERIOD && !(mod & (KMOD_LALT | KMOD_RALT | KMOD_LCTRL | KMOD_RCTRL))) {
        if (ctx == AX_LOG || ctx == AX_ROOM || ctx == AX_ACTIONS || ctx == AX_MAP ||
            ctx == AX_INV || ctx == AX_INGAME || ctx == AX_ITEMUSE)
            return routeLogToggle(base, sym, rep);
        if (ctx == AX_TOWNLOG || ctx == AX_TOWNMAP || ctx == AX_PARTY || ctx == AX_ROSTER ||
            ctx == AX_BLDG || ctx == AX_DISTRICT || ctx == AX_EMBARK || ctx == AX_NONE)
            return routeTownLogToggle(base, sym, rep);
    }

    if (sym == SDLK_l && !(mod & (KMOD_LALT | KMOD_RALT | KMOD_LCTRL | KMOD_RCTRL))) {
        if (ctx == AX_ACTIONS || ctx == AX_MAP || ctx == AX_INV || ctx == AX_INGAME ||
            ctx == AX_ROOM || ctx == AX_QUEST || ctx == AX_LOG || ctx == AX_ITEMUSE) {
            if (!rep) speakMeter(base);
            return true;                              // claimed: one readout per press
        }
    }

    if (sym == SDLK_t && (mod & (KMOD_LSHIFT | KMOD_RSHIFT)) &&
        !(mod & (KMOD_LALT | KMOD_RALT))) {
        if (ctx == AX_ACTIONS || ctx == AX_MAP || ctx == AX_INV || ctx == AX_INGAME ||
            ctx == AX_ROOM || ctx == AX_QUEST || ctx == AX_LOG || ctx == AX_ITEMUSE) {
            if (!rep) requestTorchDouse(base, (mod & (KMOD_LCTRL | KMOD_RCTRL)) != 0);
            return true;                          // claimed: one action per press, never repeating
        }
        return false;
    }

    if (sym == SDLK_t && !(mod & (KMOD_LALT | KMOD_RALT | KMOD_LCTRL | KMOD_RCTRL))) {
        if (!rep && (ctx == AX_ACTIONS || ctx == AX_MAP || ctx == AX_INV || ctx == AX_INGAME ||
                     ctx == AX_ROOM || ctx == AX_QUEST || ctx == AX_LOG || ctx == AX_ITEMUSE))
            armTorchWatch(base);
        return false;
    }

    if (sym == SDLK_i && !(mod & (KMOD_LALT | KMOD_RALT | KMOD_LCTRL | KMOD_RCTRL))) {
        if (ctx == AX_ACTIONS || ctx == AX_ROOM || ctx == AX_MAP || ctx == AX_INV ||
            ctx == AX_INGAME || ctx == AX_QUEST || ctx == AX_ITEMUSE || ctx == AX_LOOT)
            return routePanelJump(base, false, rep);
        if (ctx == AX_REALMINV || ctx == AX_TRKPICK || ctx == AX_EXCHANGE || ctx == AX_RESOURCES ||
            ctx == AX_TOWNMAP || ctx == AX_PARTY || ctx == AX_ROSTER ||
            ctx == AX_TOWNLOG || ctx == AX_BLDG || ctx == AX_DISTRICT || ctx == AX_EMBARK ||
            ctx == AX_RING || ctx == AX_RINGLIST ||
            ctx == AX_NONE)
            return routeRealmInvToggle(base, rep);
        return false;
    }

    if (sym == SDLK_g && !(mod & (KMOD_LALT | KMOD_RALT | KMOD_LCTRL | KMOD_RCTRL))) {
        if (!g_campCovers &&
            (ctx == AX_ACTIONS || ctx == AX_ROOM || ctx == AX_MAP || ctx == AX_INV ||
             ctx == AX_INGAME || ctx == AX_QUEST || ctx == AX_ITEMUSE))
            return routeQuestToggle(base, sym, rep);
    }

    if (d && d->keys) return d->keys(base, sym, mod, rep);
    return false;
}

int OurPoll(void* ev) {
    if (g_enabled && ev && emitSynth(ev)) return 1;
    if (g_enabled && ev && emitSynthKey(ev)) return 1;

    int r;
    for (;;) {
        r = g_origPoll ? g_origPoll(ev) : 0;
        if (!g_enabled || !r || !ev) break;          // hook off / queue empty: nothing to route
        logJoyDeviceEvent(ev);
        if (!routeInputEvent(g_base, ev)) break;     // not claimed -> hand it to the game
        // ---- A CLAIMED KEY RELEASES THE EVENT READ HOLD ----
        {
            uint32_t evType = 0;
            if (safeReadU32((uintptr_t)ev, &evType) && evType == SDL_EVT_KEYDOWN)
                axReleaseReadHold("claimed key");
        }
    }
    if (g_enabled && !r) advanceSynthFrame();          // pump ended -> release deferred events next frame
    if (g_enabled) {
        DWORD now = GetTickCount();
        traceAxContext();
        serviceInputProbe(g_base);

        serviceSubtitles(g_base);

        checkPreamble(g_base, frontEndDisplay(g_base));

        checkOverlayFocus(g_base);
        checkPauseOpen(g_base);
        checkMapFocus(g_base);
        servicePanelHandoff(g_base);
        serviceScoutDiag(g_base);
        serviceSecretReveal(g_base);
        serviceScoutWatch(g_base);
        serviceVisitedWatch(g_base);
        serviceAreaCross(g_base);
        serviceMapReq(g_base);
        checkMapPan(g_base);      // keep the focused tile centred
        serviceMapMove(g_base);
        serviceTileStep(g_base);
        serviceMoveWatch(g_base);
        serviceAdvanceWatch(g_base);
        serviceAdvanceOffer(g_base);
        serviceSkipTurn(g_base);
        serviceReorderWatch(g_base);
        serviceItemTurn(g_base);
        serviceCamp(g_base);
        serviceTorchWatch(g_base);
        serviceSnuffWatch(g_base);
        serviceLightWatch(g_base);
        serviceFormationLog(g_base);
        serviceDiscardWatch(g_base);
        serviceCharSheet(g_base);  // the character sheet (C) opened or closed
        serviceHeroWatch(g_base);
        serviceTrinketTrigger(g_base);
        serviceCombatText(g_base);
        serviceAnnouncement(g_base);
        serviceBark(g_base);
        servicePit(g_base);

        uintptr_t disp = frontEndDisplay(g_base);
        int sub = disp ? feNamingSub(disp) : -1;
        if (sub != FE_NAMING_SUB_NAMING) g_reopenBlocked = false;   // box gone -> re-arm for next time
        if (sub == FE_NAMING_SUB_NAMING && !g_naming && !g_reopenBlocked) {
            beginNaming(disp);
        } else if (g_naming && sub != FE_NAMING_SUB_NAMING && !g_textInputActive &&
                   (now - g_lastKeyTick) > 1500) {
            endNaming();
        }
        // After Enter committed a name, read + announce what the game stored.
        if (g_commitCheckAt && now >= g_commitCheckAt) {
            g_commitCheckAt = 0;
            announceCommittedName(g_base);
        }
        if (r && ev) handleInputEvent(g_base, ev);   // r==1 => ev is a real, filled event

        if (r && ev) {
            uint32_t et = 0;
            if (safeReadU32((uintptr_t)ev, &et) && isUserActionEvent(et)) {
                g_lastActionTick = now;
                if (et == SDL_EVT_JOYBUTTONDOWN || et == SDL_EVT_CONTROLLERBUTTONDOWN)
                    g_inputIsController = true;
                else if (et == SDL_EVT_KEYDOWN)
                    g_inputIsController = false;
                if (g_preambleHold) {
                    g_preambleHold = false;
                    logLine("preamble hold released by input type=0x%x", et);
                }
            }
        }
        checkConfirmDialog(g_base);   // focus-less button-prompt popups (id stays -1)
        checkSlotReturn(g_base);
        checkNewGameContent(g_base);
        checkSaveScrollProbe(g_base);
        serviceSaveScrollFollow(g_base);
        serviceFrontEndLanding(g_base);
        checkLoadingScreen(g_base);
        checkLootOverlay(g_base);     // loot window: appear + read
        checkEventOverlay(g_base);
        checkCurioResult(g_base);
        checkQuestCompletePopup(g_base);
        qcProbe(g_base);              // the popup's state gate, still watched
        checkRaidResults(g_base);
        checkReturnToTown(g_base);
        checkTownLog(g_base);
        checkResources(g_base);
        serviceInfestation(g_base);
                                      // throttled to 2 Hz; the front end clears its change latch)
        checkRing(g_base);
        checkTownBuildings(g_base);
        checkDistricts(g_base);
        checkTownMap(g_base);
        checkEmbark(g_base);
        checkProvision(g_base);
        checkBuilding(g_base);
        checkParty(g_base);
        checkPartyStrip(g_base);
        serviceTownBark(g_base);
        checkTownRename(g_base);
        checkExchange(g_base);
        checkRealmInv(g_base);
        serviceTrkPick(g_base);
        serviceRaidTrinket(g_base);
        serviceRaidTrinketConfirm(g_base);
        checkGlossary(g_base);
        checkHelp(g_base);
        checkControls(g_base);
        checkJournal(g_base);
        serviceStatueJournalWatch();
        serviceQuestRetreatWatch();
        qtDumpFocusIds(g_base);
        checkTutorialDismissed(g_base);
        checkTownEventDismissed(g_base); // ...and the town-event popup does the same
        checkRestingPoint(g_base);
        serviceOptAdjust();
        axLangService(g_base);
        frameCheck();
    }
    return r;
}

// ---- ',' — SAY THE MESSAGE ON SCREEN AGAIN ----
static void commaKeyGateCheck(uintptr_t base) {
    static bool done = false;
    if (done) return;
    done = true;
    char owner[0x44];
    if (kbActionForKey(base, (int)SDLK_COMMA, owner, sizeof owner))
        logLine("sayagain GATE: ',' is BOUND to the game action \"%s\" — claiming it takes it away", owner);
    else
        logLine("sayagain GATE: ',' is not in the keyboard binding table — safe to claim");
}

static void axSayAgain(uintptr_t base) {
    commaKeyGateCheck(base);

    if (g_speakUntil) {
        logLine("sayagain: releasing the remaining speech hold");
        g_speakUntil = 0;
    }

    if (tutPopupActive())     { tutReannounce(base); return; }
    if (axIsTownEvent())      { teReannounce(base); return; }   // not under C's preview sheet

    for (int i = 0; i < kAxContextCount; i++) {
        const AxContextDef* d = &kAxContexts[i];
        if (!d->isMessage || !d->reannounce || !d->active()) continue;
        logLine("sayagain: re-reading \"%s\"", d->name);
        d->reannounce(base);
        return;
    }

    const AxContextDef* cur = currentAxDef();
    logLine("sayagain: no message on screen (context \"%s\")", cur ? cur->name : "none");
    postSpeech(axs(AXS_SAY_AGAIN_NOTHING));
}
