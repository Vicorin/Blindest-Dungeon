// internal.h — the one shared header of the multi-TU split.

#pragma once
#include <windows.h>
#include "game/offsets.h"
#include "core/axstrings.h"

static const int MAILBOX_SZ = 2048;
static const int SPEECH_QUEUE_MAX = 24;

// ---- What a line IS, which decides what may destroy it ----
enum SpeechKind {
    SPK_AUTO    = -1,
    SPK_NAV     = 0,
    SPK_EVENT   = 1,
    SPK_AMBIENT = 2,
    SPK_CHATTER = 3
};

// ---- The mod owns the queue; the reader gets one line at a time ----
static const DWORD SPEAK_MS_LATENCY  = 400;       // handover -> first audible word
static const DWORD SPEAK_MS_PER_CHAR = 50;        // ~20 chars/s
static const DWORD SPEAK_MS_FLOOR    = 300;       // on top of the latency, never less than this
static const DWORD SPEAK_MS_CAP      = 15000;

// ---- THE BARK CARRY ----
static const int   BARK_CARRY_MAX = 3;
extern char g_barkCarry[BARK_CARRY_MAX][MAILBOX_SZ];   // guarded by g_cs, oldest first
extern int  g_barkCarryN;

// ---- THE EVENT MERGE ----
static const int   EVENT_MERGE_MAX_LINES = 4;     // total lines in one utterance, not extras
static const int   EVENT_MERGE_MAX_CHARS = 240;

// ---- Speech module (core/speech.cpp) ----
extern CRITICAL_SECTION g_cs;
extern char g_speechQ[SPEECH_QUEUE_MAX][MAILBOX_SZ];
extern SpeechKind g_sqKind[SPEECH_QUEUE_MAX];     // per line: nav (droppable) or event (never)
extern int  g_sqHead;
extern int  g_sqCount;
extern volatile bool g_hasPending;
extern DWORD      g_speakUntil;                   // estimated end of the line the reader is speaking
extern SpeechKind g_lastSpokenKind;               // what that line was (only events are protected)
extern bool       g_holdLogged;                   // one log line per hold, not per 20 ms tick
extern char       g_lastNavLine[MAILBOX_SZ];      // last NAV line + when it was posted: the
extern DWORD      g_lastNavTick;                  // held-arrow release re-read (dllmain.cpp)
extern volatile bool g_silenceReq;
void axReleaseReadHold(const char* who);

const char* speechKindName(SpeechKind k);
// ---- The speech library ----
bool speakUtf8(const char* utf8, bool interrupt, int* outErr = nullptr);
bool        axSpeechInit();
void        axSpeechShutdown();
bool        axSpeechSilence();
int         axSpeechIsSpeaking();
const char* axSpeechReaderName();
bool        axSpeechHasSpeech();
const char* axSpeechErrName(int prismError); // PrismError int -> loggable text
// ---- The debug log (core/speech.cpp) ----
void logLine(const char* fmt, ...);
void logDump(const char* fmt, ...);
bool        axDebugLogEnabled();
const char* axDebugLogPath();          // "" if the exe directory could not be resolved
bool        axSetDebugLog(bool on);
// ---- Speech settings (core/speech.cpp, same settings.ini) ----
bool axReadSubtitles();
void axSetReadSubtitles(bool on);
void axSubtitlesLangDefault(const char* lang);  // settle that default; called once from
enum { AX_BARKS_ON = 0, AX_BARKS_DUNGEON = 1, AX_BARKS_TOWN = 2, AX_BARKS_OFF = 3, AX_BARKS_MODES = 4 };
int  axBarksMode();                    // default AX_BARKS_ON
void axSetBarksMode(int mode);         // out of range is refused (logged)
bool axReadTownBarks();                // mode is On or Only in town
bool axReadDungeonBarks();             // mode is On or Only in dungeons
bool axCheckUpdates();
void axSetCheckUpdates(bool on);
// ---- Speech OUTPUT settings ----
enum { AX_SPEECH_AUTO = 0, AX_SPEECH_PRISM = 1, AX_SPEECH_SAPI = 2, AX_SPEECH_MODES = 3 };
enum { AX_SPCFG_SAPI = 1, AX_SPCFG_BACKEND = 2 };   // request kinds; a pending heavier one wins
static const int AX_SAPI_VOICES_MAX = 64;
int  axSpeechHandler();                              // default AX_SPEECH_AUTO
void axSetSpeechHandler(int mode, const char* okFmt, const char* failFmt);
void axPrismBackendSetting(char* out, int outsz);    // registry name, "" = Auto
void axSetPrismBackend(const char* name, const char* okFmt, const char* failFmt);
void axSapiVoiceSetting(char* out, int outsz);       // voice display name, "" = SAPI's default
void axSetSapiVoice(const char* name);
int  axSapiRateSetting();                            // 0..100, -1 = never set (SAPI's own stands)
int  axSapiVolumeSetting();
void axSetSapiRate(int pct);
void axSetSapiVolume(int pct);
int         axPrismBackendCount();
const char* axPrismBackendName(int i);
bool axSapiLive();
int  axSapiVoiceCount();
void axSapiVoiceName(int i, char* out, int outsz);
int  axSapiLiveVoice();                              // index into the list, -1 unknown
int  axSapiLiveRate();                               // 0..100 as SAPI reports it, -1 unknown
int  axSapiLiveVolume();
void axSpeechServiceConfig();                        // SPEECH THREAD, once per tick before the drain
void postSpeech(const char* text, bool interrupt = true, SpeechKind kind = SPK_AUTO);
void flushSpeech(const char* why);
void stripMarkup(const char* in, char* out, int outsz);

// ---- The game's own subtitles (core/subtitles.cpp) ----
void serviceSubtitles(uintptr_t base);
bool subMovieTrackUp(uintptr_t base);           // a cinematic's subtitle track is live (LOG-ONLY

// ---- Game-read module (core/gameread.cpp) ----
bool safeReadPtr(uintptr_t addr, uintptr_t* out);
bool safeReadI64(uintptr_t addr, int64_t* out);
bool safeReadU32(uintptr_t addr, uint32_t* out);
bool safeReadU8(uintptr_t addr, uint8_t* out);
bool safeReadCStr(uintptr_t addr, char* dst, int max);
bool safeReadBlock(uintptr_t addr, void* dst, int n);
bool safeWriteU8(uintptr_t addr, uint8_t v);
bool safeWriteU32(uintptr_t addr, uint32_t v);
bool safeWriteU64(uintptr_t addr, uint64_t v);
int sehReport(const char* what, EXCEPTION_POINTERS* xp);
bool readTbwText(uintptr_t tbw, char* out, int outsz);
bool extractWidgetText(uintptr_t widget, uintptr_t tbwVtbl,
                       char* out, int outsz, char* via, int viasz);
uintptr_t findOwnerByFocusId(uintptr_t base, int64_t id);
uintptr_t focusElementById(uintptr_t base, int64_t id);
bool isNoFocus(int64_t id);
void idToAscii(int64_t id, char* out);
long focusVectorCount(uintptr_t base);
bool resolveKey(uintptr_t base, const char* key, char* out, int outsz);

// ---- Hook/bootstrap module (core/hooks.cpp) ----
typedef int (*PollFn)(void*);
extern uintptr_t* g_iatSlot;               // address of the IAT entry we patched
extern PollFn     g_origPoll;              // original SDL_PollEvent
extern volatile bool g_enabled;            // master on/off (set once the hook is live)
extern uintptr_t     g_base;
extern uintptr_t     g_tbwVtbl;
extern volatile bool g_textInputActive;    // SDL text input on (Start/Stop hooks)
extern volatile void* g_optMenu;           // captured by the options-menu vtable hook
extern volatile void* g_raidDisplay;       // captured by the Panel_Banner hook (RaidDisplay&)
extern uintptr_t g_jpObj;                  // journal popup the hook last saw drawn
extern DWORD     g_jpSeenTick;             // when it last drew (backstop for "gone")
extern uintptr_t g_ltoObj;                 // light-meter overlay the hook last saw render
extern DWORD     g_ltoSeenTick;            // when it last rendered ("meter is on screen")
int  OurPoll(void* ev);
void announceTutorialPopup(uintptr_t self);
void clCaptureEntry(uintptr_t rd, unsigned int idx);
void clScanQueue(uintptr_t base, uintptr_t rd);        //   the call-site patch is unchanged
int  feNamingSub(uintptr_t display);

// ---- Front-end module (frontend/pause.cpp): THE IN-GAME PAUSE MENU ----
void checkPauseOpen(uintptr_t base);
extern bool g_axOptionsUp;

// ---- Front-end module (frontend/tutorial.cpp): THE TUTORIAL POPUPS ----
bool tutPopupActive();
bool altControllerGlyphs(uintptr_t base);     // PlayStation-style prompts? (frontend/dialog.cpp;
void tutReannounce(uintptr_t base);           // AX_TUTORIAL's ',' re-read
void checkTutorialDismissed(uintptr_t base);  // the gap watch -> axModalClosed
bool routeTutorialKey(uintptr_t base, uint32_t sym, uint16_t mod, uint8_t repeat);

// ---- Front-end module (frontend/naming.cpp): THE TEXT-FIELD MIRROR ----
void beginNaming(uintptr_t display);          // the box appeared: seed the mirror and cue it
void announceCommittedName(uintptr_t base);   // after Enter: what the game actually stored
void handleInputEvent(uintptr_t base, void* ev);  // the SDL side, live only during a session
extern DWORD g_lastKeyTick;                   // last keystroke, for the self-healing close
extern DWORD g_commitCheckAt;                 // when to read the committed name back (0 = idle)
extern bool  g_reopenBlocked;

// ---- Front-end module (frontend/loading.cpp) ----
void checkPreamble(uintptr_t base, uintptr_t display);
void checkLoadingScreen(uintptr_t base);
void lsReannounce(uintptr_t base);        // AX_LOADING's re-introduce hook (the / key, and a
                                          //   modal closing over it) — title + tip, read live
extern volatile bool g_preambleHold;
extern DWORD         g_preambleHoldUntil;
extern volatile bool g_lsSuppressFocus;   // a loading screen is up -> frameCheck stays quiet
extern volatile bool g_titlePrefixPending;
extern const char* const kTitleScreenName;

// ---- Frontend module (frontend/sharedui.cpp): the shared-UI overlay family ----
void checkGlossary(uintptr_t base);
void checkHelp(uintptr_t base);
void checkControls(uintptr_t base);
void checkJournal(uintptr_t base);
void serviceStatueJournalWatch();
bool axIsGlossary();
bool axIsHelp();
bool axIsControls();
bool axIsJournal();
bool routeGlossaryKey(uintptr_t base, uint32_t sym, uint16_t mod, uint8_t repeat);
bool routeHelpKey(uintptr_t base, uint32_t sym, uint16_t mod, uint8_t repeat);
bool routeControlsKey(uintptr_t base, uint32_t sym, uint16_t mod, uint8_t repeat);
bool routeJournalKey(uintptr_t base, uint32_t sym, uint16_t mod, uint8_t repeat);
void glReannounce(uintptr_t base);
void hpReannounce(uintptr_t base);
void cpReannounce(uintptr_t base);
void jpReannounce(uintptr_t base);
extern bool g_jpActive;
extern DWORD g_stJrnWatchUntil;

// ---- Frontend module (frontend/options.cpp): the OPTIONS screen ----
bool isOptionId(uint32_t cc);
int  optRowPos(uint32_t cc);
bool resolveOptions(uintptr_t base, uintptr_t tbwVtbl, int64_t id, char* text, int textsz);
int  readOptionValue(uintptr_t base, int descIdx, int32_t* out, int maxN);
int64_t optRowAnchorId(int pos);
bool optIsAnchorId(uint32_t cc);
bool optPageActive();
uint32_t optPageId();
bool optListStep(int dir);
                                  //   geometric walk is the fallback (page has no rows)
bool optTipStep(bool forward);
                                  //   the row, wrap); false = no row answers, chord not ours
bool optAdjustRow(bool increase);
bool optRowToggle();
void serviceOptAdjust();
bool optSavedLanguage(uintptr_t base, char* out, int outsz);  // the APPLIED game language's
extern char g_optValueOnly[128];

// ---- Frontend module (frontend/dialog.cpp): the CONFIRMDIALOG family ----
bool resolveModeDialog(uintptr_t base, uintptr_t display, int64_t id,
                       bool announceQuestion, char* out, int outsz);
bool resolveGenericConfirm(uintptr_t base, int64_t id, bool firstEntry,
                           char* out, int outsz);
bool confirmDialogOpen(uintptr_t base);
int  confirmDialogAnswerCount(uintptr_t base); // how many answers the live dialog has; 0 = none
uintptr_t confirmDialogEntry(uintptr_t base);
                                               // layer needs the identity, not just the existence.
void checkConfirmDialog(uintptr_t base);
void cdReannounce(uintptr_t base);        // AX_DIALOG's re-introduce hook (the / key, and a
                                          //   modal closing over it) — question + answers
bool axIsDialog();
bool routeDialogKey(uintptr_t base, uint32_t sym, uint16_t mod, uint8_t repeat);
extern const char* kActFailKey;
extern const char* kActFailQuirkKey;
extern char g_bldActWhy[512];

// ---- Input module (input/synth.cpp): SYNTHESISED MOUSE + KEYBOARD ----
void designToWindow(float dx, float dy, float* wx, float* wy);
void writeCursorGlobals(float designX, float designY);
void enqueueSynth(uint32_t type, int button, int state, uint32_t frameDelay);
void enqueueSynthWheel(int clicks);             // SDL_MOUSEWHEEL: +clicks = up, -clicks = down
void enqueueSynthAt(uint32_t type, int button, int state, uint8_t held,
                    float x, float y, uint32_t frameDelay);
bool synthDragPoints(float sx, float sy, float tx, float ty, const char* what);
bool synthDragElements(int64_t srcId, int64_t dstId, const char* what);  // ...between two elements
bool emitSynth(void* ev);                       // OurPoll: hand the game a due synthetic event
void advanceSynthFrame();                       // OurPoll: pump ended -> release next frame's
bool clickQueued();                             // a press is already pending -> don't re-arm
void enqueueSynthKey(uint32_t type, uint32_t scancode, uint32_t sym, uint16_t mod);
bool emitSynthKey(void* ev);                    // OurPoll: same, for the key queue
// ---- Input module (input/layout.cpp): the KEYBOARD LAYOUT FOLD, 2026-08-16 ----
bool klFold(uintptr_t base, uint32_t* sym, uint32_t scan);  // true = *sym rewritten to a canonical
uint32_t klScancodeForKey(uint32_t sym);        // where this keycode lives NOW (0 = nowhere)
uint32_t klKeyForScancode(uint32_t scan);       // ...and back: what that position prints (0 = n/a)
bool klKeyName(uint32_t sym, char* out, int outsz);  // SDL's own name for a key, UTF-8. The F10
void klRefresh();                               // rebuild if the input language changed (lazy)
void moveCursorTo(float fx, float fy);          // place the virtual cursor (queues the hover)
void parkCursor(const char* why);               // move it OFF-SCREEN for a transition: nothing
bool elemPos(uintptr_t elem, float* x, float* y);      // FocusElement rect: top-left
bool elemCenter(uintptr_t elem, float* fx, float* fy); // ... and its centre
uintptr_t feGetElementById(int64_t id);         // the game's own lookup, SEH-guarded (0 = none)
bool feElemOnScreen(int64_t id);                // the game's own "drawn this frame" lookup
uintptr_t feFindElementByFamily(uintptr_t base, uint32_t family, uint32_t ownerTag, int64_t* idOut,
                                uint32_t span = 0);
int feCollectFamilyByX(uintptr_t base, uint32_t family, uint32_t ownerTag,
                       uintptr_t* out, float* xOut, int maxOut);
void frontEndClickCursor();                     // click WHERE THE CURSOR IS (no target re-derive)
bool frontEndClickElementId(int64_t id);        // move onto a specific element, then click it
void feDumpFocusVector(const char* why);
extern bool g_cursorValid;                      // cursor has been placed on this screen
void serviceInputProbe(uintptr_t base);
void logJoyDeviceEvent(void* ev);
void logInputSnapshot(uintptr_t base, const char* why);
void diagDumpFocusElements(uintptr_t base, const char* why);
bool readInputEnableStack(uintptr_t base, int* gateOut, int* sizeOut);

// ---- Frontend module (frontend/display.cpp): FRONTENDDISPLAY + the NAVIGATOR ----
struct FourCcLabel { uint32_t code; const char* key; AxStrId fallback; };
const FourCcLabel* lookupFrontEnd(uint32_t code);   // title-screen FourCC -> loc key + fallback
bool resolveSaveSlot(uintptr_t display, int64_t id, char* out, int outsz);
uintptr_t frontEndDisplay(uintptr_t base);      // live FrontEndDisplay, or 0 (front end not up)
int  frontEndState(uintptr_t display);          // front-end sub-screen state; -1 on failure
void frontEndFocusMove(int dir);
int64_t feCurrentFocusId();                     // the game's live focus id, else the tracked one
extern int64_t g_feFocusId;                     // the navigator's tracked position
void checkSlotReturn(uintptr_t base);
void feReannounce(uintptr_t base);              // AX_TITLE / AX_PAUSE re-introduce hook: a modal
void serviceFrontEndLanding(uintptr_t base);    // a front-end screen appeared -> land on its first
                                                //   item and say it, instead of waiting for an arrow
void checkNewGameContent(uintptr_t base);       // the new-game DLC/mods content step
void checkSaveScrollProbe(uintptr_t base);
void serviceSaveScrollFollow(uintptr_t base);   // the eased scroll settled -> park the cursor
bool routeFrontEndKey(uintptr_t base, uint32_t sym, uint16_t mod, uint8_t repeat);

// ---- The mod's CONTEXT vocabulary ----
enum AxContext {
    AX_NONE = 0,
    AX_TITLE,
    AX_NAMING,
    AX_DIALOG,
    AX_PAUSE,      // the in-game pause menu overlay has focus
    AX_LOADING,    // a loading screen is up (owns announcer)
    AX_MAP,
    AX_INV,
    AX_ACTIONS,
    AX_ROOM,
    AX_QUEST,
    AX_QUESTDONE,
    AX_RAIDFINISH,
                   //   until the results / a loading screen / a cutscene ends it (raid/quest.cpp)
    AX_LOOT,
    AX_EVENT,
    AX_MEAL,
    AX_CAMPTARGET,
    AX_TARGET,
    AX_ITEMUSE,
    AX_CHARSHEET,
    AX_LOG,
    AX_RESULTS,
    AX_TOWNLOG,
    AX_RESOURCES,
    AX_EXCHANGE,
    AX_TRKPICK,
    AX_REALMINV,
    AX_TOWNMAP,
    AX_BLDG,
    AX_DISTRICT,
    AX_PARTY,
    AX_ROSTER,
    AX_EMBARK,
    AX_PROVISION,
    AX_GLOSSARY,
    AX_HELP,
    AX_CONTROLS,
    AX_JOURNAL,
    AX_INGAME,
    AX_SETTINGS,
    AX_TUTORIAL,
    AX_TOWNEVENT,
    AX_RING,
                   //   ring.cpp) -- the root view of that layer, like AX_BLDG in town (OWNS it)
    AX_RINGLIST,
};
AxContext currentAxContext();                   // the active context (defined with the table)
bool axModalClosed(uintptr_t base, const char* who);

// ---- Helpers still DEFINED in dllmain.cpp that surface modules call ----
bool axDecodeArrow(uint32_t sym, uint16_t mod, uint8_t repeat, bool wantCols,
                   int* dRow, int* dCol);       // shared list-nav: arrow -> row/col step
bool axNavHoldRepeat(uint8_t repeat);           // held-arrow gate (typeahead): true = swallow
bool axStepCursor(int* row, int count, int delta);   // shared list-nav: clamp, hard stops
bool axDecodeJump(uint32_t sym, uint16_t mod, uint8_t repeat, int* jump);
static const int AX_JUMP = 1 << 20;             // a delta larger than any list: fed to
                                                // axStepCursor, whose clamp IS the jump landing
void fourcc(uint32_t id, char* out);            // focus id -> 4-char tag, for log lines
void resolveLabel(uintptr_t base, uintptr_t tbwVtbl, int64_t id,
                  char* text, int textsz);      // the specific-else-generic label chain
extern int64_t g_axSpokenId;                    // shared spoken-dedup: last announced id...
extern char    g_axLastSpoken[512];             //   ...and its text (frameCheck's pair)
void abStripMarkup(char* s);
float u32AsFloatM(uint32_t u);
extern volatile bool g_inputIsController;
bool kbActionForKey(uintptr_t base, int keycode,
                    char* nameOut, int outsz);
extern volatile bool g_naming;
struct PauseLabel { uint32_t code; const char* key; AxStrId fallback; };
                                                                          //  fallback is an entry id)
const PauseLabel* lookupPause(uint32_t code);
extern bool          g_inPauseMenu;             // last focused item was a pause-menu button
extern volatile bool g_pauseOpen;               // the pause menu's elements are in the vector
uintptr_t rrDisplay(uintptr_t base);            // raid-results display global; 0 = screen not up
uintptr_t csTownSheetPanel(uintptr_t base);
bool invItemVectorAt(uintptr_t system, uintptr_t* begOut, int* slotsOut);  // System -> stacks
bool invItemName(uintptr_t base, uintptr_t item,
                 char type[64], char itemId[64], char key[192], char name[256]);
static const int INV_MAX_SLOTS = 16384;
bool qtElemOnScreen(uint32_t id);               // is this focus element registered right now?
bool qtGoalText(uintptr_t base, uint32_t goalId, char* out, int outsz);  // a quest goal's words
uintptr_t abHeroClassOf(uintptr_t hero);        // hero -> its HeroClass object (0 = unreadable)
bool abReadF32(uintptr_t addr, float* out);     // SEH-guarded float read (sanity-bounded)
int  csResolveLevel(uintptr_t base, uintptr_t hero);   // XP -> resolve level (the sheet's rank)
bool csQuirkName(uintptr_t base, const char* id, char* out, int outsz);
void csEquipStat(uintptr_t base, char* out, int outsz, const char* key, const char* value);
bool abHeroNameClassOf(uintptr_t base, uintptr_t hero,
                       char* name, int namesz, char* cls, int clssz);
static const int AB_TIP_MAX_LINES   = 30;
static const int AB_TIP_LINE_SZ     = 1024;
static const int CAMP_BUFF_DESC_BUF = 0x100;
int  abBuildSkillLines(uintptr_t base, uintptr_t skill, uintptr_t hero, const char* fallbackName,
                       char lines[AB_TIP_MAX_LINES][AB_TIP_LINE_SZ]);
bool csBuffDescByKey(uintptr_t base, uint32_t key, const uint32_t* amountBits,
                     char withDuration, char* out, int outsz);
static const int CS_QUIRK_DESC_BUF = 0x100;
bool csQuirkDescRaw(uintptr_t base, uintptr_t quirk, char* out, int outsz);
void csFlattenLines(char* s, int outsz);
int  csCampSkillCost(uintptr_t cls);
int  csCampEffectLines(uintptr_t base, uintptr_t cls,
                       char lines[AB_TIP_MAX_LINES][AB_TIP_LINE_SZ], int at);
int  csCampUsesLine(uintptr_t base, uintptr_t cls, uintptr_t hero,
                    char lines[AB_TIP_MAX_LINES][AB_TIP_LINE_SZ], int at);
uintptr_t abSelectedHero(uintptr_t base);       // the selected hero (town: the sheet's subject)
extern int       g_trkPhase;
extern int       g_trkItemSlot;                 // the chosen trinket's REAL realm-vector slot
extern char      g_trkItemName[256];
extern uintptr_t g_trkHero;                     // the hero chosen in the picker
extern uintptr_t g_trkEntry;                    // ... and their Roster::Entry (opens the sheet)
extern DWORD     g_trkSheetWatchUntil;
extern bool      g_trkToggleIssued;             // the panel re-open has been asked for once
bool csTrinketRowText(uintptr_t base, int i, char* out, int outsz);  // row i of the sheet's pair
bool csFocusTrinketSection(uintptr_t base);     // put the sheet cursor on the trinket section
bool csFocusTrinketSlotWith(uintptr_t base, int slot, const char* prefix);
uintptr_t mapRoot(uintptr_t base);              // ptr -> Raid/Map state; 0 outside a raid
extern volatile bool g_mapReview;               // map review layer active (map has focus)
extern volatile bool g_invActive;               // the inventory panel owns the corner right now
extern volatile bool g_iuActive;                // item-use aiming (Enter on a bag slot)
extern volatile bool g_abActive;
extern volatile bool g_rvActive;                // ...or the dungeon view, the bar's sibling layer
extern volatile bool g_qtActive;                // ...or the quest tracker zone, the third sibling
extern volatile bool g_tsActive;                // skill targeting entered from the bar
extern int           g_abCursor;                // index into the bar; -1 = unplaced
extern bool          g_campCovers;
extern volatile bool g_qcOpen;                  // the quest-complete popup is up
extern volatile bool g_csOpen;                  // the character sheet is up right now
extern volatile bool g_clogOpen;                // the combat log is being read
enum AbKind { AB_PORTRAIT = 0, AB_SKILL, AB_REORDER, AB_PASS, AB_REST };
struct ActionItem {
    int64_t  id;        // focus id
    int64_t  owner;     // owner id (elem+0x08)
    float    cx, cy;
    int      kind;      // AbKind
    int      skillIdx;
};
static const int AB_MAX_ITEMS   = 16;           // sanity cap on bar size (7 expected)
static const int RV_MAX_MEMBERS = 8;            // sanity cap on a length read out of game memory
int  abBuildBar(uintptr_t base, ActionItem* items, int maxItems);
int  abSlotCount(const ActionItem* items, int n);
void abSetActive(uintptr_t base, bool on);
void abSpeakLabel(uintptr_t base, const ActionItem* it, int slotCount,
                  const char* head = nullptr);  // the default lives HERE, not at the definition
void rvSetActive(uintptr_t base, bool on);
int  rvPartyList(uintptr_t base, uintptr_t* out, int maxOut);   // out sized RV_MAX_MEMBERS
void rvAnnounceEntry(uintptr_t base, bool withCounts);
void qtSetActive(uintptr_t base, bool on);
bool axIsNaming();                              // the estate/rename text field is live
bool axIsLoading();                             // a loading screen has the floor
bool axIsPause();                               // the pause menu or its options screens are up
static const int   CLOG_LINE_MAX = 320;
static const DWORD TUT_GAP_MS    = 500;
static const char* const INV_TITLE_PREFIX    = "str_inventory_title_";
static const char* const INV_DESC_PREFIX     = "str_inventory_description_";
static const char* const PROV_SHARD_NAME_KEY = "str_inventory_title_shard";

static const int       TL_WALK_KIDS_MAX    = 96;        // sanity cap on any children vector
static const int       TL_WALK_NODES_MAX   = 1024;      // total node budget per collect
static const int       TL_ROW_MAX          = 384;       // bytes per collected row / fragment
static const int       TL_FRAGS_MAX        = 16;        // fragments gathered per entry

// ---- Town module (town/estate.cpp): the ACTIVITY LOG + the RESOURCES BAR ----
void checkTownLog(uintptr_t base);
bool routeTownLogToggle(uintptr_t base, uint32_t sym, uint8_t repeat);
bool routeTownLogKey(uintptr_t base, uint32_t sym, uint16_t mod, uint8_t repeat);
bool axIsTownLog();
void tlReannounce(uintptr_t base);
bool tlLooksLikeWidget(uintptr_t base, uintptr_t p);   // widget-tree walk guard
void tlGather(uintptr_t base, uintptr_t node, int depth, int* budget,
              char frags[][TL_ROW_MAX], int* nfrags, bool probe); // the shared tree walker:
                                                       //   every TextBoxWidget text under node
uintptr_t resTownRoot(uintptr_t base);
struct ResCurrency { const char* id; const char* titleKey; int spoken; };
uint32_t resHash(const char* s);                       // the game's own string hash (h*0x35+c)
bool haPurchased(uintptr_t base, uint32_t hash, uint8_t code, uint32_t guid);
const ResCurrency* resCurrencyFind(uint32_t idHash);   // nullptr = neither table knows it
void resCurrencyTitle(uintptr_t base, uint32_t idHash, const char* who,
                      char* out, int outsz);           // localized name, id/hash fallback, logged
void resCurrencyTitleById(uintptr_t base, const char* id, const char* who,
                          char* out, int outsz);       // same, from the id string ("crest")
bool resWalletAmount(uintptr_t base, uint32_t idHash, int* amount);  // estate wallet lookup
void checkResources(uintptr_t base);
bool routeResourcesToggle(uintptr_t base, uint32_t sym, uint8_t repeat);
bool routeResourcesKey(uintptr_t base, uint32_t sym, uint16_t mod, uint8_t repeat);
bool axIsResources();
void resReannounce(uintptr_t base);
int  infEstateLevel(uintptr_t base, bool* shown, char* name, int nameSz);  // 0..3 or -1; name = "none".."high"
void infEstateText(uintptr_t base, char* out, int outsz);  // "Infestation: High" localized, "" = hidden
void serviceInfestation(uintptr_t base);                   // per-frame town watch: speak the level MOVING
extern bool g_resActive;
extern bool g_ptyActive;
extern bool g_rosActive;

// ---- Town module (town/map.cpp): the HAMLET MAP + the TOWN JUMP KEYS ----
void checkTownBuildings(uintptr_t base);
void checkTownMap(uintptr_t base);         // AX_TOWNMAP edges, the open watch, the jump chain
bool routeTownMapKey(uintptr_t base, uint32_t sym, uint16_t mod, uint8_t repeat);
bool axIsTownMap();
void tmReannounce(uintptr_t base);
bool tmbBareMap(uintptr_t root);           // the game's own all-panels-closed test
uintptr_t tmbOwner(uintptr_t root);        // townRoot -> the panel-group owner (16 vectors)
int  tmbGroup(uintptr_t owner, int g, uintptr_t* beg);  // one group's panel vector; count, *beg
bool tmbClassName(uintptr_t base, uintptr_t vft, char* out, int outsz);  // MSVC RTTI class name
void tmbIdChars(uint32_t v, char out[10]);
bool tmIdLooksValid(const char* s);         // short lowercase ASCII, the shipped-data id shape
static const int TM_MAX_ROWS = 24;
static const int TM_NAME_MAX = 96;
struct TmRow {
    char     id[68];
    char     name[TM_NAME_MAX];
    uint32_t elemId;
                                 // (the synthetic Districts row is the flat 'ddis' fourcc instead)
    bool     unlocked;
                                 // Districts row, the Campaign+0x11f0 / override pair)
    bool     district;
    bool     offSave;            // a district whose DLC is not enabled on this save
    bool     isNew;
};
extern TmRow g_tmRows[TM_MAX_ROWS];
extern int   g_tmRow;                      // the list cursor (a jump parks it on its target)
extern int   g_tmTipLine;
int  tmRows(uintptr_t base, bool probe);   // rebuild + count the rows -- always a live read
void tmOpenRow(uintptr_t base, const TmRow* r);  // the ONE open body: refusal, click, watch
int  tmCurrentLayer(uintptr_t root);       // the current eTownLayer; -1 = unreadable
static const uint32_t TMB_BLD_ID_BASE = 0x626c64;   // ChooseBuilding's element-id family base
static const uint32_t TM_DDIS_ELEM_ID = 0x64646973; // 'ddis', the estate-bar Districts button
struct TjKey { uint32_t sym; const char* id; };
extern const TjKey TJ_KEYS[];
extern const int   TJ_KEY_COUNT;
const char* tjTargetForKey(uint32_t sym);  // letter -> target id; nullptr = not a jump key
void tjTargetName(uintptr_t base, const char* id, char* out, int outsz);  // the naming ladder
extern DWORD g_tmOpenWatchUntil;           // an open click is in flight (0 = idle)
extern char  g_tmOpenName[TM_NAME_MAX];    // ...whose spoken name the watch reports with
extern bool  g_tmOpenIsDistrict;
extern bool  g_tmOpenIsDdis;               // ...and it was the Districts button
extern char  g_tjChainId[28];              // leave-then-open chain target id; empty = idle
extern DWORD g_tjChainUntil;               // give up waiting for the bare map
extern DWORD g_tjChainDue;
extern DWORD    g_bnSwitchUntil;           // a nav-strip switch click is in flight (0 = idle)
extern uint32_t g_bnSwitchHash;
extern char     g_bnSwitchName[TM_NAME_MAX];  // for the timeout line only

// ---- Town module (town/districts.cpp): the DISTRICT BUILDINGS SCREEN ----
void checkDistricts(uintptr_t base);       // AX_DISTRICT edges + the purchase outcome watch
bool routeDistrictKey(uintptr_t base, uint32_t sym, uint16_t mod, uint8_t repeat);
bool axIsDistrict();
void dstReannounce(uintptr_t base);

// ---- Town module (town/embark.cpp): EMBARK / RAID PLANNING ----
void checkEmbark(uintptr_t base);          // AX_EMBARK edges + the E / selection watches
bool routeEmbarkKey(uintptr_t base, uint32_t sym, uint16_t mod, uint8_t repeat);
bool routeEmbarkForward(uintptr_t base, uint8_t repeat);
extern DWORD g_embFwdWatchUntil;           // nonzero = the forward click actually went out --
bool axIsEmbark();
void embReannounce(uintptr_t base);
bool embProvIsOpen(uintptr_t root);        // the provisioning panel's own open test
bool embQsOpen(uintptr_t root);
static const int EMB_NAME_MAX = 128;
bool embPartyName(uintptr_t base, char* out, int outsz);  // the party-combo name, if on screen
bool embSelectedQuestFacts(uintptr_t base, char* dungeon, int dsz, char* lengthS, int lsz,
                           char* diffS, int dfsz);

// ---- Town module (town/provision.cpp): the PROVISIONING screen ----
void checkProvision(uintptr_t base);
bool routeProvisionKey(uintptr_t base, uint32_t sym, uint16_t mod, uint8_t repeat);
bool axIsProvision();
void provReannounce(uintptr_t base);
bool provTabToInfo(uintptr_t base);
bool provShiftTabToGrid(uintptr_t base);
void provLandOnInfo();
bool routeTownShiftTab(uintptr_t base, uint8_t repeat);   // town/party.cpp
int64_t provSlotElem(uintptr_t base, uint32_t ownerTag, int slot);
int provSlotElemCount(uintptr_t base, uint32_t ownerTag, int64_t* firstId);

// ---- Town module (town/buildings.cpp): the BUILDING SCREENS ----
void checkBuilding(uintptr_t base);
bool routeBldKey(uintptr_t base, uint32_t sym, uint16_t mod, uint8_t repeat);
bool axIsBld();
void bldReannounce(uintptr_t base);
bool routeTownJumpKey(uintptr_t base, const char* id, bool fromBuilding, uint8_t repeat);
uintptr_t bldOpenPanel(uintptr_t root);         // the OPEN building's interior panel (0 = none)
extern int  g_bldRecDrag;
extern bool g_qtOpen;                      // a sanitarium dropdown is open
// ---- The ACTIVITY PICK, roster edition ----
bool bldActPickCommitFromRoster(uintptr_t base, uintptr_t entry, uintptr_t rowWidget,
                                uintptr_t hero);   // Enter on an idle hero: the game's own
                                                   // pick lambda + the outcome watch
void bldActPickCancelFromRoster(uintptr_t base);   // Escape/Tab: drop the session, speak
                                                   // "Cancelled." + the slot row underneath
void bldActPickRowSuffix(uintptr_t base, uintptr_t hero, char* out, int outsz);
bool bldActPickBusy();                             // the pick's outcome watch is in flight:
bool bldActivityNameFromPtr(uintptr_t base, uintptr_t activity, char* out, int outsz);
uintptr_t bldTrinketRecord(uintptr_t base, uintptr_t item);   // item -> its trinket def-registry rec
bool bldTrinketIdByHash(uintptr_t base, uint32_t idHash, char* out, int outsz);
bool bldClassNameByHash(uintptr_t base, uint32_t want, char* out, int outsz);
void bldItemClassProbe(uintptr_t base, uint32_t wantIdHash);   // debug-log dump of the registry walk
bool bldTrinketRarity(uintptr_t base, uintptr_t rec, char* out, int outsz, bool probe);
void bldTrinketClassReq(uintptr_t base, uintptr_t rec, char* out, int outsz, bool probe);
int  bldTrinketEffects(uintptr_t base, uintptr_t item, char* out, int outsz, bool probe);
uintptr_t bldTrinketRecordByHash(uintptr_t base, uint32_t idHash);
int  bldTrinketEffectsRec(uintptr_t base, uintptr_t rec, char* out, int outsz, bool probe);
int  bldTrinketCharges(uintptr_t base, uintptr_t item, uintptr_t rec, char* out, int outsz, bool probe);
int  bldTrinketTriggers(uintptr_t base, uintptr_t rec, char* out, int outsz, bool probe);
bool bldTrinketTriggerLine(uintptr_t base, uintptr_t item, uintptr_t rec, char* out, int outsz);
bool bldTrinketRecName(uintptr_t base, uintptr_t rec, char* out, int outsz);
bool bldTrinketExhaustTransform(uintptr_t base, uintptr_t rec, uint32_t intoIdHash,
                                char* nameOut, int nameOutSz);
uintptr_t bldTrinketDef(uintptr_t base, uintptr_t item);   // item -> its ItemClass record (0 = none)
bool bldTrinketFitsHero(uintptr_t base, uintptr_t item, uintptr_t hero, bool* sure);
void bldTrinketWhyNotFit(uintptr_t base, uintptr_t item, uintptr_t hero);
bool bldSehRosterCounts(uintptr_t base, int* count, int* max);  // campaign roster have/cap
int  bldRosterEntryCount(uintptr_t base);                       // entry count alone (-1 = unreadable)
int  bldItemDisplays(uintptr_t panel, uintptr_t* out, int max);
int  bldRecruitKindOf(uintptr_t base, uintptr_t disp);          // BLD_RCT_* kind of this display
uintptr_t bldChildByVft(uintptr_t base, uintptr_t panel, const uintptr_t* rvas, int nrva);
enum { BLD_RCT_NONE = 0, BLD_RCT_BASE = 1, BLD_RCT_SHARD = 2 };
struct BldActRow {
    uintptr_t disp;
    uintptr_t activity;      // Building::Activity*
    uintptr_t slotw;         // the HeroSlot widget
    uintptr_t pendingHero;   // iface+0x28 when not committed (0 otherwise)
    uint32_t  committedGuid; // slot record heroGuid (0 = not committed)
    uint32_t  slotElem;      // the slot's own element id (HeroSlot+0x88)
    int32_t   occupant;
    int       slotIdx;       // ordinal within the activity
    int       slotCount;     // slots in this activity
    int       actOrd;
    char      actId[32];     // the activity id string ("meditation")
    bool      isQuirkTreat;
    int       trtDrop;
};
int  bldActRows(uintptr_t base, uintptr_t panel, BldActRow* out, int max);
uintptr_t bldActHeroByGuid(uintptr_t base, uint32_t guid);      // roster scan; fine once per edge
bool bldActTakeCommitFollowup(uint32_t guid, char* out, int outsz);
static const uint32_t BLD_ROSTER_ELEM_FAMILY = 0x72736200;  // 'rsb' + <hero key>
static const uint32_t BLD_ROSTER_OWNER_TAG   = 0x726c6520;  // 'rle ' -- owner of the row widgets

// ---- Town module (town/party.cpp): TOWN HERO MANAGEMENT ----
void checkParty(uintptr_t base);
void checkPartyStrip(uintptr_t base);
void checkReturnToTown(uintptr_t base);
void serviceTrkPick(uintptr_t base);
bool routePartyKey(uintptr_t base, uint32_t sym, uint16_t mod, uint8_t repeat);
bool axIsParty();
void ptyReannounce(uintptr_t base);
bool routeRosterKey(uintptr_t base, uint32_t sym, uint16_t mod, uint8_t repeat);
bool axIsRoster();
void rosReannounce(uintptr_t base);
bool routeTrkPickKey(uintptr_t base, uint32_t sym, uint16_t mod, uint8_t repeat);
bool axIsTrkPick();
void trkReannounce(uintptr_t base);
bool routeTownTab(uintptr_t base, uint32_t sym, uint8_t repeat);  // Tab in town: roster in/out
void ptyLeave(uintptr_t base, const char* why);   // stand the lineup down (embark's forward watch)
void rosLeave(uintptr_t base, const char* why);   // stand the roster down (same site)
extern int g_rosPickSlot;               // >= 0: the roster is picking FOR that lineup slot
bool rosEnterForActivityPick(uintptr_t base, const char* header);  // buildings' slot Enter
bool rosActivityPickLive();             // the mode is armed. Deliberately TRUE through the
void rosSpeakRowPrefixed(uintptr_t base, const char* prefix);      // the watch's "Not
                                        // placed." re-grounding speaks the live roster row
void trkPickBegin(uintptr_t base, int realSlot, const char* name);  // realm inventory's Enter
void trkPickCancel(uintptr_t base, AxStrId why);                    // ... and its close edge.
void trkCommitSlot(uintptr_t base, int slot);
bool csCloseTownSheet(uintptr_t base);
                                        // teardown calls it; named cs* but the party area owned it)
static const int ROS_MAX_ROWS = 64;
                                        // until the barks slice moves) sizes its walk with it
extern uintptr_t g_ptySheetHero;
                                   // the party area owns it, the recruit preview borrows it
static const int PTY_MAX_SLOTS = 8;     // slot-strip headroom, never a trust -- sizes the
static const uint32_t PTY_SLOT_ELEM_BASE  = 0x717370;   // 'psq' + slot
static const uint32_t PTY_SLOT_ELEM_OWNER = 0x68717374;
int  ptySlotStrip(uintptr_t base, uintptr_t* ifaceOut, uintptr_t* heroOut, int maxOut);
bool ptyWriteArrangement(uintptr_t base, const uintptr_t* iface, const uintptr_t* want,
                         int n, const char* what);
void ptyHeroFrag(uintptr_t base, uintptr_t hero, char* out, int outsz);  // "Name, class, level"
void ptyHeroFragBare(uintptr_t base, uintptr_t hero, char* out, int outsz);
                                   // hero card fragment; the recruit rows + hero picker read it

// ---- Town module (town/trinkets.cpp): the TRINKET (realm) INVENTORY ----
void checkRealmInv(uintptr_t base);
bool routeRealmInvKey(uintptr_t base, uint32_t sym, uint16_t mod, uint8_t repeat);
bool axIsRealmInv();
void riReannounce(uintptr_t base);
bool routeRealmInvToggle(uintptr_t base, uint8_t repeat);  // I in town: the game's own toggle body
void riRequestEquipFor(uintptr_t base, uintptr_t hero, int slot);  // the sheet's Enter-on-empty-
                                        // slot handler (dllmain.cpp): arm the pending equip + open
bool riIsOpen(uintptr_t base);                  // the realm trinket panel is up
uintptr_t riSystem(uintptr_t base);             // the realm Inventory::System (circus-aware)
int  riOccupiedSlots(uintptr_t base, int* out, int max);  // occupied slot indices, in grid order
bool riInCircus(uintptr_t base);        // the Butcher's Circus mode flag, read live
bool riItemUnlocked(uintptr_t base, uintptr_t item);
void riSpeakRow(uintptr_t base, const char* prefix);      // speak the realm grid's focused row
bool riStartEquipDrag(uintptr_t base, int realSlot, int gridPos);  // the one equip mechanism
static const int RI_MAX_TRINKETS = 2048;
extern bool  g_riActive;                        // mirrors the realm panel's shown flag
extern uintptr_t g_riEquipForHero;              // pending equip: the hero (0 = none pending)
extern int   g_riEquipForSlot;                  // ... and which of their two trinket slots
extern char  g_riEquipName[256];                // the trinket being dragged, for announcements
extern DWORD g_riEquipWatchUntil;               // 0 = idle
extern int   g_riEquipCountBefore;              // occupied realm count before the drag
extern char  g_riEquipHeroName[80];
extern bool  g_riEquipCloseSheet;               // trinket-first flow: drop the sheet after

// ---- Town module (town/exchange.cpp): the HEIRLOOM EXCHANGE ----
void checkExchange(uintptr_t base);
bool routeExchangeKey(uintptr_t base, uint32_t sym, uint16_t mod, uint8_t repeat);
bool axIsExchange();                    // the exchange is the live surface
void exReannounce(uintptr_t base);
void exOpen(uintptr_t base);
                                        //   estate bar's Trade row (town/estate.cpp) calls it
bool exIsOpen(uintptr_t base);          // the exchange's shown flag -- town/trinkets.cpp's
                                        //   open-edge overlap diagnostic reads it

// ---- Town module (town/barks.cpp): TOWN BARKS ----
void serviceTownBark(uintptr_t base);

// ---- Town module (town/events.cpp): the TOWN-EVENT POPUP ----
void announceTownEventPopup(uintptr_t base, uintptr_t self);  // the slot-23 detour's callee
void checkTownEventDismissed(uintptr_t base); // OurPoll: popup left -> release hold, hand back
bool townEventPopupActive();
bool axIsTownEvent();
bool townEventEnterClaims();            // popup up AND the event has an interaction
bool routeTownEventEnter(uintptr_t base);     // pre-dispatch: click the interaction button
bool townEventHeroKeysClaim();
bool routeTownEventHeroKey(uintptr_t base, uint32_t sym, uint16_t mod, uint8_t repeat);
void teReannounce(uintptr_t base);
int townEventFreeUpgrades(uintptr_t base, const char* tag);

// ---- Raid module (raid/resting.cpp): THE RESTING POINT ----
void checkRestingPoint(uintptr_t base); // the debt latch: track edges, pay when audible
bool axCoversRestingPoint();            // the layers that sit ON TOP of the resting point

// ---- Raid module (raid/map.cpp): THE IN-RAID MAP ----
void checkOverlayFocus(uintptr_t base);    // is a modal (pause / dialog) holding the focus?
void checkMapFocus(uintptr_t base);        // THE panel gate: map + inventory edges, off ONE
                                           //   selector read, so the two can never disagree
void servicePanelHandoff(uintptr_t base);
void serviceScoutWatch(uintptr_t base);
void serviceScoutDiag(uintptr_t base);
void serviceVisitedWatch(uintptr_t base);
void serviceSecretReveal(uintptr_t base);
void serviceAreaCross(uintptr_t base);     // "Room A, Curio" on any arrival in a room that no
void serviceMapReq(uintptr_t base);        // the cursor's queued area / tile step
void checkMapPan(uintptr_t base);          // keep the focused tile centred on screen
void serviceMapMove(uintptr_t base);
void serviceTileStep(uintptr_t base);
bool routeMapKey(uintptr_t base, uint32_t sym, uint16_t mod, uint8_t repeat);
void openMapReview(uintptr_t base, uintptr_t root);  // announce the layer + reset the cursor to
                                           //   the party (the M key's re-orientation press)
void armPanelHandoff(const char* why);
bool twBeginStep(uintptr_t base, uint32_t sym, uint32_t scan);  // Shift+D / Shift+A
bool rvPropReach(uintptr_t base, uintptr_t prop, bool* inReachOut, int* dirOut, float* dxOut);
int  rvRoomProps(uintptr_t base, uintptr_t* out, int max);
bool rvPropIsTrap(uintptr_t prop);
bool rvPropActive(uintptr_t prop);
bool rvPropName(uintptr_t base, uintptr_t prop, char* out, int outsz);
void twInterrupt(uint32_t bySym);
bool sehPartyTile(uintptr_t base, uintptr_t root, int* out);  // the party's tile index within
extern bool g_twStepping;
extern uint32_t g_twSym;                   // ...and the keycode it is holding
extern uint32_t g_twScan;
uintptr_t mapAreaById(uintptr_t root, uint32_t id);   // linear scan of the area vector
long mapAreaTiles(uintptr_t area);                    // its tile count
bool tileContentsVisible(uintptr_t area, int tileIdx);// is that tile scouted? (knowledge > 1 --
void mapAreaLabel(uint32_t id, int kind, char* out, int outsz);        // from the id alone
void mapAreaLabelById(uintptr_t root, uint32_t id, char* out, int outsz);  // ...via the graph
void mapContentLabel(uintptr_t base, int content, char* out, int outsz);   // AreaContent -> word
int  areaFindHiddenDoor(uintptr_t area, uint32_t* destId, uintptr_t* tileOut);  // -1 = none.
extern int g_invSlot;                      // the bag's cursor slot; -1 = not placed yet
void ruDisarmEquip(const char* why);
bool axOwnsAnnouncer(AxContext c);         // does this context drive its own readout?
extern bool g_ihHeld;                      // a bag stack is picked up (Space rearrange)
void ihRelease(const char* why);

// ---- Raid module (raid/dungeonview.cpp): THE DUNGEON VIEW ----
static const int RV_MAX_ENEMIES = 12;
static const int RV_MAX_PROPS   = 16;   // sanity cap on a length read out of game memory
static const int RV_MAX_DOORS   = AREA_EXIT_SLOTS;   // the Area's 8 fixed door slots
static const int RV_MAX_ROWS    = RV_MAX_MEMBERS + RV_MAX_ENEMIES +
                                  RV_MAX_PROPS   + RV_MAX_DOORS;
enum RvKind {
    RV_ACTOR   = 0,   // a hero or a monster -- `actor` is set
    RV_PROP    = 1,   // a curio / obstacle -- `obj` is the Prop*
    RV_DOOR    = 2,
    RV_ADVANCE = 3,
    RV_HIDDEN_DOOR = 4,
};
struct RvEntry {
    uintptr_t actor;
    int       kind;
    uintptr_t obj;      // non-actor rows only: the Prop* or the Door slot
    uint32_t  destId;
    bool      enemy;    // false = party member, true = far side
    int       idx;
    int       total;
                        // are in it -- a large monster fills two). Prop/door rows: the group size.
    int       slot;
    int       slotEnd;
};

// The per-frame pair (OurPoll) and the two key doors (the kAxContexts row, and R).
void serviceAdvanceWatch(uintptr_t base);   // did "move on" actually advance the wave raid?
void serviceAdvanceOffer(uintptr_t base);   // ...and arm/disarm the row that offers it
bool routeRoomKey(uintptr_t base, uint32_t sym, uint16_t mod, uint8_t repeat);
bool routeRoomToggle(uintptr_t base, uint32_t sym, uint8_t repeat);   // R: open / re-orient /
bool rvEnterFromBar(uintptr_t base);       // the action bar's Up: step back into the view
bool abEnterFromRoom(uintptr_t base, const char* head = nullptr);

// The readers the rest of the raid HUD borrows.
int  rvEnemyList(uintptr_t base, uintptr_t* out, int maxOut);   // out sized RV_MAX_ENEMIES
bool rvMonsterName(uintptr_t base, uintptr_t mon,               // the display name: latch, else
                   char* out, int outsz);                       // resolve str_monstername_<id>
bool rvCorpseWord(uintptr_t base, uintptr_t actor, char* out, int outsz);
int  rvBuildRoom(uintptr_t base, RvEntry* out, int maxOut);     // the room as ONE flat list
int  rvSurprisedSide(uintptr_t base, uint32_t* roundOut);       // which SIDE was ambushed
int  rvSkillCount(uintptr_t mon, uintptr_t* begOut);            // the MonsterClass skill vector
int  rvRankFromGame(uintptr_t actor, uint32_t* rawOut = nullptr, int* endOut = nullptr);
void rvPosPhrase(int slot, int slotEnd, bool enemy, char* out, int outsz);  // "position 2" / a span
void rvPosForEntry(const RvEntry& e, char* out, int outsz);     // ...the same, for a built row
void rvAppendComma(char* out, int outsz, const char* frag);     // ", frag" with the sentence stop
void rvTrapDisarmSuffix(uintptr_t base, uintptr_t hero, char* out, int outsz);
void rvSpeakTipLine(uintptr_t base, int tipDir);                // THE shared Ctrl+Up/Down reader
extern int g_rvTipLine;
bool rvTipPanelSwitch(uintptr_t base, int dir, bool repeat);
extern int g_rvTipCol;
int  spHeroTipLines(uintptr_t base, uintptr_t hero, int slot, int slotEnd, bool withTitle,
                    bool enemy, char lines[][AB_TIP_LINE_SZ], int n, int maxLines);
void propPrettyName(const char* id, char* out, int outsz);

void abAppend(char* dst, int dstsz, const char* piece);
void abAppendFrag(char* out, int outsz, const char* frag);
bool abPlausibleName(const char* s);                 // reject a garbage read before speaking it
bool abFormatMatches(const char* fmt, const char* expect);
bool abTipPlain(uintptr_t base, const char* key, char* out, int outsz);
bool abTipFloat(uintptr_t base, const char* key, float v, char* out, int outsz);
bool abHeroLabelOf(uintptr_t base, uintptr_t hero, char* out, int outsz);
uintptr_t abActorClass(uintptr_t actor);
static const uintptr_t ACTOR_CLASS_VID_OFF = 0x48;   // ...its inline class-id string
bool abHeroHealth(uintptr_t base, uintptr_t hero, char* out, int outsz);
bool abHeroStress(uintptr_t base, uintptr_t hero, char* out, int outsz);     // "40/200 Stress"
bool abHeroStressCur(uintptr_t base, uintptr_t hero, char* out, int outsz);  // "40 Stress" -- the
void abDumpBar(uintptr_t base, const ActionItem* items, int n);   // the stitch's one-shot dump
static const int AB_EFF_BUCKET_SZ = 320;
void abEffRenderVector(uintptr_t base, uintptr_t skill, uintptr_t vecAddr, bool partyHeal,
                       char* out, int outsz);
void abEffTrinketGroup(uintptr_t base, uintptr_t vecAddr, const char* groupStem, bool hitFlag,
                       bool partyHeal, char* out, int outsz);
extern bool g_abDumped;                    // the bar's one-shot diagnostic has fired
extern int  g_abRoomReturn;
extern int  g_rvCursor;                    // ...and the view's own cursor, which the bar's `
enum SpCondBucket {
    SP_COND_HEALTH = 1,
    SP_COND_STRESS = 2,
    SP_COND_NEG    = 4,
    SP_COND_POS    = 8,   // stealth, guard, riposte, aegis
    SP_COND_BLEED  = 16,
    SP_COND_BLIGHT = 32,
    SP_COND_HORROR = 64,
    SP_COND_TRAIT  = 128,
    SP_COND_ALL    = SP_COND_HEALTH | SP_COND_STRESS | SP_COND_NEG | SP_COND_POS |
                     SP_COND_BLEED  | SP_COND_BLIGHT | SP_COND_HORROR | SP_COND_TRAIT,
};
int  spBuffCount(uintptr_t actor, uintptr_t* beginOut);
int  csPartyScoutLines(uintptr_t base, const uintptr_t* heroes, int nHeroes,
                       char lines[AB_TIP_MAX_LINES][AB_TIP_LINE_SZ]);
int  spBuffPolarity(uintptr_t base, const unsigned char* rec);   // buff, debuff or neither (base stats only)
int  spBuffPolarityAny(uintptr_t base, const unsigned char* rec);// ...any stat type, by the game's own
                                                                 // direction byte; the combat popup's matcher
bool spBuffTextFromRecord(uintptr_t base, uintptr_t actor, const unsigned char* rec,
                          char* out, int outsz);
bool spConditionsInto(uintptr_t base, uintptr_t actor, int want, char* out, int outsz);
int  spConditionLines(uintptr_t base, uintptr_t actor, int want,
                      char lines[][AB_TIP_LINE_SZ], int n, int maxLines);
int  spUnvoicedBuffLines(uintptr_t base, uintptr_t actor, bool monster,
                         char lines[][AB_TIP_LINE_SZ], int n, int maxLines);
int  spAppendStatusLines(uintptr_t base, uintptr_t actor,
                         char lines[][AB_TIP_LINE_SZ], int n, int maxLines);
void spDumpStatusOnce(uintptr_t base, uintptr_t actor);
bool spActorIsHero(uintptr_t base, uintptr_t actor);
bool spPolarityLabel(uintptr_t base, int polarity, char* out, int outsz);
bool spDurTitleLine(uintptr_t base, uintptr_t actor, int statType, const char* keyFmt,
                    char* out, int outsz);
bool spModeName(uintptr_t base, uintptr_t actor, char* out, int outsz);
bool csStatOwnMask(uintptr_t hero, uintptr_t off, float* out);
bool csResistVisibleFor(uintptr_t base, bool isMonster, int i);
bool csResistRowFor(uintptr_t base, uintptr_t hero, int i, char* out, int outsz);
int  campPhase(uintptr_t base);
bool campActive(uintptr_t base);
int  campPoints(uintptr_t base);
bool campPointsText(uintptr_t base, char* out, int outsz);
// Up out of the dungeon view: the quest zone's own entry.
bool qtEnterFromRoom(uintptr_t base);
bool routeQuestToggle(uintptr_t base, uint32_t sym, uint8_t repeat);
void tsSetActive(bool on);
void iuAbandon(const char* why);
void lootStepAside(const char* why);

// ---- Raid module (raid/actionbar.cpp): THE ACTION BAR + SKILL TARGETING ----
static const int AB_MAX_SKILLS = 8;        // sanity cap on the equipped-skill walk

void serviceHeroWatch(uintptr_t base);     // the selected hero changed -- say who, and why
void serviceSkipTurn(uintptr_t base);      // did the pass button actually end the turn?
void serviceReorderWatch(uintptr_t base);
void serviceMoveWatch(uintptr_t base);
                                           //   reposition -- never from the call returning
bool routeActionKey(uintptr_t base, uint32_t sym, uint16_t mod, uint8_t repeat);
bool routeActionToggle(uintptr_t base, uint32_t sym, uint8_t repeat);   // ` : open / close
bool routeTargetKey(uintptr_t base, uint32_t sym, uint16_t mod, uint8_t repeat);
bool abActivate(uintptr_t base, const ActionItem* it, int slotCount, int abCursor);  // Enter
uintptr_t abCurrentTurnActor(uintptr_t root);   // whose turn the battle says it is
bool abBattleLive(uintptr_t root);               // is a battle actually running (raid root's
bool abMoveWatchArmed();
bool abWasRecentCommitTarget(uintptr_t actor);   // was this actor the explicit target of the
bool abToggleCharSheet(uintptr_t base);          // DungeonController::ToggleCharacterDisplay --
                                                //   argument-less: it shows the SELECTED hero
bool abHeroLabel(uintptr_t base, char* out, int outsz);      // the SELECTED hero, named
int  abSkillList(uintptr_t base, uintptr_t* skills, int maxSkills);   // the equipped-skill walk
bool abSkillName(uintptr_t base, uintptr_t skill, char* out, int outsz);
bool abModeAllowsSkill(uintptr_t skill, uint32_t modeId);
void roFormationText(uintptr_t base, char* out, int outsz);  // "Position 1, Reynauld. ..."
bool abTipInt(uintptr_t base, const char* key, int v, char* out, int outsz);
bool abTipInt2(uintptr_t base, const char* key, int a, int b, char* out, int outsz);
bool abTipIntStr(uintptr_t base, const char* key, int a, const char* b, char* out, int outsz);
bool abTipStrN(uintptr_t base, const char* key, const char* a, const char* b, const char* c,
               char* out, int outsz);
void abEffGlueFallback(char* out, int outsz, const char* a, const char* b, const char* c);
bool abStatValue(uintptr_t stat, uint32_t mask, bool clamp, float* out);

uintptr_t csCampSkillClass(uintptr_t base, int i);              // the camping skill behind a slot
bool csResistValue(uintptr_t base, uintptr_t hero, int i, float* out);   // the preview's resists
bool abTipNumFirst(uintptr_t base, const char* key, int a, int b, char* out, int outsz);
bool abTipValueOnly(uintptr_t base, const char* key, int a, char* out, int outsz);
extern int g_abTipLine;

// ---- Raid module (raid/camp.cpp): THE CAMP ----
int  campPhase(uintptr_t base);            // 0 = no camp; 1..9 = the step it is on
bool campActive(uintptr_t base);
bool campCanStartHere(uintptr_t base);
bool campInSkillPhase(uintptr_t base);
int  campPoints(uintptr_t base);           // time left to spend
uint32_t campSkillUsedCount(uintptr_t base, uintptr_t hero, uintptr_t cls);
bool campPointsText(uintptr_t base, char* out, int outsz);
bool campBarLabel(uintptr_t base, int barIdx, char* out, int outsz);
int  campBarLines(uintptr_t base, int barIdx, char lines[AB_TIP_MAX_LINES][AB_TIP_LINE_SZ]);
bool campUseSkill(uintptr_t base, const ActionItem* it);        // Enter on one of them
bool axIsCampTarget();
bool routeCampTargetKey(uintptr_t base, uint32_t sym, uint16_t mod, uint8_t repeat);
bool axIsMeal();
bool routeMealKey(uintptr_t base, uint32_t sym, uint16_t mod, uint8_t repeat);
void mealReannounce(uintptr_t base);
void serviceCamp(uintptr_t base);          // THE watcher -- the camp has no other signal
bool csCampSkillName(uintptr_t base, uintptr_t cls, char* out, int outsz);
bool invItemVector(uintptr_t base, uintptr_t* begOut, int* slotsOut);

// ---- Raid module (raid/loot.cpp): THE LOOT WINDOW ----
extern bool g_lootActive;
extern bool g_lootAside;
void lootStepAside(const char* why);
void lootReturn(uintptr_t base, const char* why);   // the other half: Escape, from wherever
void checkLootOverlay(uintptr_t base);
void lootReannounce(uintptr_t base);       // the kAxContexts row's re-introduce hook
bool axIsLoot();
bool routeLootKey(uintptr_t base, uint32_t sym, uint16_t mod, uint8_t repeat);

// ---- Raid module (raid/eventscroll.cpp): THE EVENT SCROLL ----
extern bool g_evActive;
void checkEventOverlay(uintptr_t base);
void checkCurioResult(uintptr_t base);
void evReannounce(uintptr_t base);         // the kAxContexts row's re-introduce hook
bool axIsEvent();
bool routeEventKey(uintptr_t base, uint32_t sym, uint16_t mod, uint8_t repeat);
const char* invNavTag(uintptr_t base, int slot);

// ---- Raid module (raid/inventory.cpp): THE PARTY BAG ----
int  invSlotCount(uintptr_t base);
bool invSlotText(uintptr_t base, int slot, char* out, int outsz, const char* tag = nullptr);
bool invSystemSlotText(uintptr_t base, uintptr_t system, int slot, const char* noun,
                       int posNum, int posTotal, char* out, int outsz,
                       const char* tag = nullptr);
bool invItemIsTrinket(uintptr_t item);     // the game's own type-string discriminator
void serviceTrinketTrigger(uintptr_t base);
bool resolveInventory(uintptr_t base, int64_t id, char* out, int outsz);
bool routeInvKey(uintptr_t base, uint32_t sym, uint16_t mod, uint8_t repeat);
bool routeItemUseKey(uintptr_t base, uint32_t sym, uint16_t mod, uint8_t repeat);
void serviceRaidTrinket(uintptr_t base);
void serviceRaidTrinketConfirm(uintptr_t base);
void serviceItemTurn(uintptr_t base);      // combat: refresh the one-item-per-turn allowance
void serviceDiscardWatch(uintptr_t base);
extern uintptr_t g_ruEquipHero;

bool routePanelJump(uintptr_t base, bool toMap, uint8_t repeat);

// ---- Raid module (raid/light.cpp): THE LIGHT METER ----
void speakMeter(uintptr_t base);              // L -- the light: darkness meter, or the quest's
                                              // named AMBIENCE (bloodlight, the farm's miasma)
void armTorchWatch(uintptr_t base);
void serviceTorchWatch(uintptr_t base);       // ...and echo the new level once the value moves
void requestTorchDouse(uintptr_t base, bool snuff);   // Shift+T reduce / Ctrl+Shift+T snuff
void serviceSnuffWatch(uintptr_t base);
void serviceLightWatch(uintptr_t base);       // a threshold crossing, darker only

// ---- Raid module (raid/combattext.cpp): WHAT THE RAID SAYS ON SCREEN ----
void serviceCombatText(uintptr_t base);       // the popup prop map -> speak + record
void serviceAnnouncement(uintptr_t base);     // the banner vector the game's timeline publishes
void serviceBark(uintptr_t base);             // the front of the raid event ring
void clogSetOpen(bool on);                    // the raid teardown drops the log's hold
bool routeLogKey(uintptr_t base, uint32_t sym, uint16_t mod, uint8_t repeat);   // AX_COMBATLOG
bool routeLogToggle(uintptr_t base, uint32_t sym, uint8_t repeat);              // the '\' key

// ---- Raid module (raid/quest.cpp): THE QUEST ZONE, RETREAT, AND THE DONE POPUP ----
void serviceQuestRetreatWatch();
void checkQuestCompletePopup(uintptr_t base); // the popup's own open/close watch
void qtDumpFocusIds(uintptr_t base);          // one-shot zone diagnostic (off unless re-enabled)
void qcProbe(uintptr_t base);                 // one-shot popup diagnostic (likewise)
bool qcPresent();                             // is the quest-complete popup up? (axIsQuestDone)
void qcReannounce(uintptr_t base);            // AX_QUESTDONE's re-introduce hook
bool routeQuestKey(uintptr_t base, uint32_t sym, uint16_t mod, uint8_t repeat);
bool routeQuestDoneKey(uintptr_t base, uint32_t sym, uint16_t mod, uint8_t repeat);
extern volatile bool g_qcReturning;           // the block is up (read by the retreat gate too)
bool axIsRaidFinish();
bool routeRaidFinishKey(uintptr_t base, uint32_t sym, uint16_t mod, uint8_t repeat);
int  qtQuestLevel(uintptr_t base);            // the dungeon level the rules.json difficulty

// ---- Raid module (raid/results.cpp): THE RAID RESULTS SCREEN ----
void checkRaidResults(uintptr_t base);        // appearance, page change, and a reveal's outcome
bool routeResultsKey(uintptr_t base, uint32_t sym, uint16_t mod, uint8_t repeat);
void rrReannounce(uintptr_t base);            // AX_RESULTS' re-introduce hook (the / key)
uintptr_t rrResults(uintptr_t base);
int       rrState(uintptr_t base);            // the display's page-state int (-1 = screen gone)

// ---- DLC modules (src/dlc/<dlc>/) ----
bool      comIsWaveRaid(uintptr_t base);          // the live quest's kind is the wave kind
uintptr_t comWaveLogic(uintptr_t base);           // Wave::WaveLogic*, 0 outside a wave raid
uintptr_t comRaidRoot(uintptr_t base);            // the raid root, 0 outside a raid
bool      comWaveThresholds(uintptr_t base, int* reachedOut, int* totalOut);
bool      comWaveRetreatKey(uintptr_t base, char* out, int outsz);  // the game's own tooltip key
bool      comKillMeter(uintptr_t base, char* out, int outsz);
void      comWaveProbe(uintptr_t base);           // one-shot dump (debug log only)

struct ComMoveSnapshot {
    bool     valid;
    uint8_t  transition;      // WaveLogic "transitionToNewArea"
    uint8_t  curioSpawned;    // WaveLogic "curioSpawned"
    uint32_t nextAction;      // WaveLogic "nextWaveActionType"
    uint32_t currRoom;        // WaveLogic "currRoom"
    ComMoveSnapshot() : valid(false), transition(0), curioSpawned(0), nextAction(0), currRoom(0) {}
};
bool      comCanAdvance(uintptr_t base);
bool      comAdvanceRoom(uintptr_t base);
bool      comAdvanceLabel(uintptr_t base, char* out, int outsz);   // the overlay's own text
void      comWaveMoveSnapshot(uintptr_t base, ComMoveSnapshot* out);
bool      comWaveMoveHappened(const ComMoveSnapshot* before, const ComMoveSnapshot* now);

bool dgIsPanel(uintptr_t base, uintptr_t panel);                  // the vftable identity check
bool dgArrivalText(uintptr_t base, uintptr_t panel, char* out, int outsz);  // "<mode>. N friends."
bool dgSpeakArrival(uintptr_t base, uintptr_t panel, const char* prefix);   // ...+ the cursor row
bool dgRouteKey(uintptr_t base, uintptr_t panel, uint32_t sym, uint8_t repeat);
void dgService(uintptr_t base, uintptr_t panel);                  // finishes the mode-switch watch
void dgReset();                                                   // fresh visit: cursors to row 1
void dgProbe(uintptr_t base, uintptr_t panel);                    // one-shot dump (debug log only)
bool dgEditFieldOpen(uintptr_t base);

// ---- DLC module (dlc/butchers_circus/ring.cpp): THE RING ----
void checkRing(uintptr_t base);         // the layer's open/close edge + the sheet watch
bool routeRingKey(uintptr_t base, uint32_t sym, uint16_t mod, uint8_t repeat);
bool routeRingListKey(uintptr_t base, uint32_t sym, uint16_t mod, uint8_t repeat);
bool axIsRing();                        // the slots own the keys
bool axIsRingList();                    // ... the contestant list does
void ringReannounce(uintptr_t base);    // a modal that covered the screen closed
void ringReannounceWith(uintptr_t base, const char* prefix);  // ...and the same with the closing
                                        // surface's own acknowledgement in front, as ONE line
void ringLeave(uintptr_t base, const char* why);   // stand the whole surface down
uintptr_t ringPanel(uintptr_t base);    // the live PartySetupDisplay, 0 = not in The Ring
bool circusScreenUp(uintptr_t base);
bool circusMatchLoading();
bool ringForwardAction(uintptr_t base, const char* label);
int  ringContestants(uintptr_t base, uintptr_t* heroes, char* inLineup, int max);
bool ringOpenSheetForPick(uintptr_t base, uintptr_t hero);  // false = it did not go out, and the
                                        // reason has already been spoken -- do not add a line

// ---- DLC module (dlc/butchers_circus/pit.cpp): INSIDE THE MATCH ----
bool pitInMatch(uintptr_t base);        // the raid's Quest id is "arena_mp"
bool pitPickOpen(uintptr_t base);
bool pitCanActivate(uintptr_t base, uintptr_t hero);   // the game's OWN per-hero gate
bool pitActivateHero(uintptr_t base, uintptr_t hero);  // Enter: activate, then step into the bar
bool pitInspectHero(uintptr_t base, uintptr_t hero);   // C: move the selection, open the sheet
void pitRowSuffix(uintptr_t base, uintptr_t hero, char* out, int outsz);  // "Cannot act." or ""
void servicePit(uintptr_t base);
void pitReset();                        // drop a pending activation (leaving the match)
bool pitCommitViaMachine(uintptr_t base, uintptr_t perf, uintptr_t skill,
                         const uintptr_t* targets, int nTargets);
bool pitBattleAcceptsCommand(uintptr_t base, uint32_t* stateOut);

// ---- DLC module (dlc/butchers_circus/matchresults.cpp): THE POST-MATCH RESULTS ----
bool bcrIsArena(uintptr_t base);        // this results screen is a Circus match's
void bcrCheck(uintptr_t base);          // entry + data-arrival + page announcements
bool bcrRouteKey(uintptr_t base, uint32_t sym, uint16_t mod, uint8_t repeat);
void bcrReannounce(uintptr_t base);     // a modal that covered the screen closed
void bcrReset();                        // the screen left -- results.cpp's watch calls it
uintptr_t bcrBuildingByHash(uintptr_t base, uint32_t idHash);
bool bcrTrinketName(uintptr_t base, uint32_t idHash, char* out, int outsz);
bool bcrBannerName(uintptr_t base, uintptr_t entry, char* out, int outsz);

// ---- DLC module (dlc/butchers_circus/prizebox.cpp): THE PRIZE BOX ----
bool pbIsPanel(uintptr_t base, uintptr_t panel);                 // the vftable identity check
bool pbSpeakArrival(uintptr_t base, uintptr_t panel, const char* prefix);  // prestige + counts + row
bool pbRouteKey(uintptr_t base, uintptr_t panel, uint32_t sym, uint8_t repeat);
void pbService(uintptr_t base, uintptr_t panel);                 // the "list built" edge announce
void pbReset();                                                  // fresh visit: cursor to the current level
void pbProbe(uintptr_t base, uintptr_t panel);                   // one-shot dump (debug log only)
bool pbRewardCard(uintptr_t base, int kind, uintptr_t entry, int idx, int total, bool enterPreview,
                  char* out, int outsz);

// ---- DLC module (dlc/butchers_circus/rankings.cpp): THE RANKING BOARD ----
bool rbIsPanel(uintptr_t base, uintptr_t panel);                 // the vftable identity check
bool rbSpeakArrival(uintptr_t base, uintptr_t panel, const char* prefix);  // rank row 0 + section head
bool rbRouteKey(uintptr_t base, uintptr_t panel, uint32_t sym, uint8_t repeat);
void rbService(uintptr_t base, uintptr_t panel);                 // tab-click landing + the rows-landed edge
void rbReset();                                                  // fresh visit: cursor to row 0
void rbProbe(uintptr_t base, uintptr_t panel);                   // one-shot dump (debug log only)

// ---- DLC module (dlc/butchers_circus/banner.cpp): THE BANNER DESIGNER ----
bool bdIsPanel(uintptr_t base, uintptr_t panel);                 // the vftable identity check
bool bdIsOpen(uintptr_t base);                                   // ... on the OPEN building (the R/B carve-out)
bool bdSpeakArrival(uintptr_t base, uintptr_t panel, const char* prefix);  // the current section's tab row
bool bdRouteKey(uintptr_t base, uintptr_t panel, uint32_t sym, uint16_t mod, uint8_t repeat);
void bdService(uintptr_t base, uintptr_t panel);                 // the click watches + the built edge
void bdReset();                                                  // fresh visit: tab list, current section
void bdProbe(uintptr_t base, uintptr_t panel);                   // one-shot dump (debug log only)

// ---- SDL keycodes + modifier masks ----
static const uint32_t  SDLK_BACKSPACE       = 8;
static const uint32_t  SDLK_TAB             = 9;      // normally the game's panel toggle; the
                                                      // character sheet claims it for its sections
static const uint32_t  SDLK_RETURN          = 13;
static const uint32_t  SDLK_ESCAPE          = 27;
static const uint32_t  SDLK_BACKQUOTE       = 0x60;   // '`' — toggles the in-raid action bar
static const uint32_t  SDLK_a               = 0x61;   // 'a' — the GAME's Move Left. Shift+A = step
static const uint32_t  SDLK_d               = 0x64;   // 'd' — the GAME's Move Right. Shift+D = step
static const uint32_t  SDLK_l               = 0x6c;   // 'l' — speaks the in-raid light meter
static const uint32_t  SDLK_r               = 0x72;   // 'r' — toggles the in-raid dungeon view
static const uint32_t  SDLK_t               = 0x74;   // 't' — the GAME's torch key; only echoed
static const uint32_t  SDLK_i               = 0x69;
static const uint32_t  SDLK_m               = 0x6d;   // 'm' — in a raid: jump to the map panel
static const uint32_t  SDLK_1               = 0x31;   // '1'..'5' — the game's own skill shortcuts,
static const uint32_t  SDLK_4               = 0x34;   //   rewired into the accessible targeting flow;
static const uint32_t  SDLK_5               = 0x35;   //   1..4 also assign a slot on the char sheet
static const uint32_t  SDLK_p               = 0x70;   // 'p' — no longer claimed (the town party
static const uint32_t  SDLK_c               = 0x63;   // 'c' — in the party area: open the hero's sheet
static const uint32_t  SDLK_u               = 0x75;   // 'u' — in a building: the upgrade button
static const uint32_t  SDLK_j               = 0x6a;   // 'j' — no longer claimed (the Jeweler it
static const uint32_t  SDLK_e               = 0x65;
static const uint32_t  SDLK_g               = 0x67;   // 'g' — in a raid: jump to the quest goals zone
static const uint32_t  SDLK_PERIOD          = 0x2e;   // '.' — toggles the combat log (raid) / activity
static const uint32_t  SDLK_BACKSLASH       = 0x5c;   // '\' — no longer claimed (was the log toggle
static const uint32_t  SDLK_COMMA           = 0x2c;   // ',' — says the message on screen again.
static const uint32_t  SDLK_SLASH           = 0x2f;   // '/' — no longer claimed (was say-again
static const uint32_t  SDLK_KP_DIVIDE       = 0x40000054;
static const uint32_t  SDLK_KP_ENTER        = 0x4000000D;
static const uint32_t  SDLK_SPACE           = 0x20;   // Space — retired from the heirloom exchange
static const uint32_t  SDLK_MINUS           = 0x2d;   // '-' — NOT a mod key. Defined only because

// ---- Sheet module (sheet/charsheet.cpp): the CHARACTER SHEET frame + the HERO RENAME ----
bool routeCharSheetKey(uintptr_t base, uint32_t sym, uint16_t mod, uint8_t repeat);
void serviceCharSheet(uintptr_t base);          // open/close + swap + outcome watches
void checkTownRename(uintptr_t base);           // the sheet's edit box: edges + outcome read
#define CS_COMSKILL_MAX 32
int  csComSkillCount(uintptr_t base);           // the class roster length (~7)
int  csComSkillLevel(uintptr_t base, int i);
int  csComSkillLevelOf(uintptr_t base, uintptr_t hero, uintptr_t heroClass, int i);
uintptr_t csComSkillAt(uintptr_t base, int i);  // roster index -> ActorCombatSkill record
bool csComSkillEquipped(uintptr_t base, int i); // is roster index i in the equipped vector?
bool csComSkillLearned(uintptr_t base, int i);  // level >= 0 (the sheet's "Not learned" test)
bool csComSkillRowText(uintptr_t base, int i, char* out, int outsz);
int  csComSortedOrder(uintptr_t base, int order[CS_COMSKILL_MAX]);  // Selected -> Learned -> Not learned
uintptr_t csEquipRecord(uintptr_t base, int i); // slot 0 weapon / 1 armour -> equipment record
bool csEquipRowText(uintptr_t base, int i, char* out, int outsz);
void csTrimLabel(char* s);                      // strip a label's trailing ':' / whitespace
int  csStatCount(uintptr_t base);               // the base-stats section
bool csStatRowText(uintptr_t base, int i, char* out, int outsz);
int  csResistCount(uintptr_t base);             // the resistances section
bool csResistRowText(uintptr_t base, int i, char* out, int outsz);
int  csStatTipLines(uintptr_t base, int i, char lines[AB_TIP_MAX_LINES][AB_TIP_LINE_SZ]);
int  csResistTipLines(uintptr_t base, int i, char lines[AB_TIP_MAX_LINES][AB_TIP_LINE_SZ]);
bool csStatTipLineFor(uintptr_t base, uintptr_t hero, int i, char* out, int outsz);
void csStatTipDecorate(uintptr_t base, uintptr_t actor, int i, char* line, int linesz);
int  csStatRowIndexForOff(uintptr_t off);
bool csResistTipLineFor(uintptr_t base, uintptr_t hero, int i, char* out, int outsz);
int  spHeroResistLines(uintptr_t base, uintptr_t hero, int slot, int slotEnd, bool enemy,
                       char lines[][AB_TIP_LINE_SZ], int maxLines);
bool spResistPanelHead(uintptr_t base, char* out, int outsz);   // the panel's spoken heading
int  spHeroItemLines(uintptr_t base, uintptr_t hero, int slot, int slotEnd, bool enemy,
                     char lines[][AB_TIP_LINE_SZ], int maxLines);
bool csItemsPanelRow(uintptr_t base, uintptr_t hero, int i, char* out, int outsz);
bool csEquipRowFor(uintptr_t base, uintptr_t hero, int i, char* out, int outsz);
bool spIncomingModsFrag(uintptr_t base, uintptr_t hero, const char* typeName, char* out, int outsz);
int  csQuirkSectionCount(uintptr_t base);       // quirks (diseases filtered OUT)
bool csQuirkRowText(uintptr_t base, int i, char* out, int outsz);
int  csDiseaseSectionCount(uintptr_t base);     // diseases (the same list, filter reversed)
bool csDiseaseRowText(uintptr_t base, int i, char* out, int outsz);
bool csHeroIsCursed(uintptr_t hero);
bool      csHeroIsNeverAgain(uintptr_t hero);
void      csNeverAgainWord(uintptr_t base, char* out, int outsz);
uintptr_t csHeroRosterLimitQuirk(uintptr_t hero);
bool csQuirkId(uintptr_t quirk, char* out, int outsz);          // the record's inline id ("tough")
bool csQuirkRowFrom(uintptr_t base, uintptr_t quirk, bool withKind, char* out, int outsz);
int  csCampSkillCount(uintptr_t base);          // the camping-skills section
bool csCampSkillRowText(uintptr_t base, int i, char* out, int outsz);
int  csCampSkillTipLines(uintptr_t base, int i, char lines[AB_TIP_MAX_LINES][AB_TIP_LINE_SZ]);
bool csCampSkillLearned(uintptr_t base, int i);    // Hero's learned map at the roster index >= 0
bool csCampSkillSelected(uintptr_t base, int i);   // roster index i is in the hero's selected list
void csDumpSheet(uintptr_t base, uintptr_t panel);
bool spTraitName(uintptr_t base, uint32_t hash, bool virtue, char* out, int outsz);  // affliction/virtue name
uintptr_t abHeroClass(uintptr_t base);          // the selected hero's HeroClass (0 = unreadable)
bool abHeroNameClass(uintptr_t base, char* name, int namesz, char* cls, int clssz);
void abSkillTitled(uintptr_t base, uintptr_t skill, const char* fallbackName,
                   char* out, int outsz,
                   bool withLevel = true);      // the default lives HERE, not at the definition
bool ruBeginFromSheet(uintptr_t base, uintptr_t hero);      // raid sheet Enter-on-trinket -> the bag
extern char g_typed[64];                        // reconstructed box contents (seed + keystrokes)
extern char g_lastName[64];                     // last contents spoken (de-dup)
extern bool g_nameModeHero;                     // while set, Enter/Escape do NOT end the session
void endNaming();                               // close the mirror session (idempotent)
static const uint32_t  SDLK_EQUALS          = 0x3d;   // '=' / '+' — NOT a mod key either; same
static const uint32_t  SDLK_DELETE          = 0x7f;   // Delete — deletes the focused save slot
static const uint32_t  SDLK_b               = 0x62;   // 'b' — town jump: the Blacksmith
static const uint32_t  SDLK_s               = 0x73;   // 's' — town jump: the Sanitarium
static const uint32_t  SDLK_v               = 0x76;   // 'v' — town jump: the Survivalist
static const uint32_t  SDLK_w               = 0x77;   // 'w' — town jump: the Nomad Wagon
static const uint32_t  SDLK_y               = 0x79;   // 'y' — town jump: the Graveyard
static const uint32_t  SDLK_z               = 0x7a;   // 'z' — town jump: the Butcher's Circus
static const uint32_t  SDLK_LCTRL           = 0x400000E0;
static const uint32_t  SDLK_RCTRL           = 0x400000E4;
static const uint32_t  SDLK_RGUI            = 0x400000E7;   // top of the modifier range: the

static const uint32_t  SDLK_HOME            = 0x4000004A;
static const uint32_t  SDLK_END             = 0x4000004D;
static const uint32_t  SDLK_PAGEUP          = 0x4000004B;
static const uint32_t  SDLK_PAGEDOWN        = 0x4000004E;
static const uint32_t  SDLK_RIGHT           = 0x4000004F;
static const uint32_t  SDLK_LEFT            = 0x40000050;
static const uint32_t  SDLK_DOWN            = 0x40000051;
static const uint32_t  SDLK_UP              = 0x40000052;
// SDL key modifiers (keysym.mod bitmask).
static const uint16_t  KMOD_LSHIFT          = 0x0001;
static const uint16_t  KMOD_RSHIFT          = 0x0002;
static const uint16_t  KMOD_LCTRL           = 0x0040;
static const uint16_t  KMOD_RCTRL           = 0x0080;
static const uint16_t  KMOD_LALT            = 0x0100;
static const uint16_t  KMOD_RALT            = 0x0200;

// ---- Settings module (settings/keymap.cpp + settings/modmenu.cpp), 2026-08-14 ----
static const uint32_t  SDLK_F10             = 0x40000043;   // scancode 67 | SDLK_SCANCODE_MASK
enum KmRegion { KMR_NONE = -1, KMR_TOWN = 0, KMR_DUNGEON = 1 };
static const uint8_t KM_SHIFT = 1;
static const uint8_t KM_CTRL  = 2;
static const uint8_t KM_ALT   = 4;
struct KmChord { uint32_t sym; uint8_t mods; };
struct KmEntry {
    const char* id;        // stable save-file slug ("dungeon.step_forward")
    KmRegion    region;    // whose bindings this belongs to
    KmChord     def;
    AxStrId     desc;
};
// keymap.cpp -- the table, the translation, the persistence:
int  kmCount();
const KmEntry* kmAt(int i);
bool kmIsBlank(int i);
KmChord kmCurrent(int i);
int  kmAssign(int i, KmChord c);
int  kmFirstBlank(KmRegion r);
bool kmDirty();                          // current differs from the open-time snapshot?
void kmSnapshot();                       // menu open: remember the last-saved state
void kmRevert();                         // "No" at the save question: back to the snapshot
void kmSaveFile();
void kmEnsureLoaded();                   // lazy one-time load of that file (main thread)
KmRegion kmRegionNow(uintptr_t base, AxContext ctx);
uint8_t kmChordModsFromKmod(uint16_t mod);
int  kmRoute(uintptr_t base, AxContext ctx, uint32_t* sym, uint16_t* mod);  // 0 pass / 1 translated / 2 freed
void kmChordName(KmChord c, char* out, int outsz);   // the spoken chord ("Control Shift T")
// modmenu.cpp -- the surface:
extern bool g_smActive;                  // the menu is up (= the AX_SETTINGS predicate)
bool axIsSettings();
bool routeSettingsKey(uintptr_t base, uint32_t sym, uint16_t mod, uint8_t repeat);
void smOpen(uintptr_t base);
void smReannounce(uintptr_t base);
