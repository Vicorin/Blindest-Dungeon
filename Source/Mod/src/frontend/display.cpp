// frontend/display.cpp -- the fifth frontend slice

#include <cstdint>
#include <cstring>
#include <cstdio>
#include "internal.h"

// ---- Title / front-end screen (FourCC focus backend) ----
static const FourCcLabel kFrontEndLabels[] = {
    { 0x73747274, "str_start_screen_button",        AXS_FE_FB_CAMPAIGN },      // "trts" (confirmed)
    { 0x73677362, "str_start_screen_circus_button", AXS_FE_FB_CIRCUS },        // "bsgs" (provisional)
    { 0x6d6c6c74, "mailing_list_title",             AXS_FE_FB_MAILING_LIST },  // "tllm"
    { 0x62646c63, "front_end_controls_element_dlc", AXS_FE_FB_ADD_DLC },       // "cldb" (provisional)
};

static const uint32_t kFrontEndSkipIds[] = {
    0x6d62636b,
    0x73746e73,
};
static bool isFrontEndSkipId(int64_t id) {
    uint32_t cc = (uint32_t)(uint64_t)id;
    for (uint32_t s : kFrontEndSkipIds) if (s == cc) return true;
    return false;
}
const FourCcLabel* lookupFrontEnd(uint32_t code) {
    for (const auto& e : kFrontEndLabels) if (e.code == code) return &e;
    return nullptr;
}

// ---- Campaign-select / save-slot screen (front-end "state 7") ----
static const uintptr_t FE_STATE_OFF       = 0x1f18;    // int sub-screen state (7 = save slots)
static const uintptr_t FE_SLOTARR_OFF     = 0x1768;    // ptr to save-slot array base (was 0x1798)
static const uintptr_t SLOT_STRIDE        = 0xc10;     // bytes per SaveSlot
static const uintptr_t SLOT_OCCUPIED_OFF  = 0x144;     // byte: !=0 => campaign in use ("begin")
static const uintptr_t SLOT_ALT_OFF       = 0x145;     // byte: !=0 => has data, uses "load"
static const int       SLOT_CAMPAIGN_COUNT = 9;        // offsets 1..9 are the campaign slots
static const uint32_t  SLOT_FOURCC_BASE   = 0x73677362; // focus id = base + offset, offset in 0..9
static const int       SLOT_COUNT         = 10;         // offsets 0..9 (offset 0 = Circus, at top)
static const int       SLOT_CIRCUS_INDEX  = 0;

static const uintptr_t SLOT_LOCATION_OFF     = 0x040;   // last known location / region
static const uintptr_t SLOT_WEEK_OFF         = 0x140;   // int32 week number
static const uintptr_t SLOT_MODE_NAME_OFF    = 0xa48;   // mode/difficulty name string
static const uintptr_t SLOT_ESTATE_DISP_OFF  = 0xac8;   // estate name (when occupied)
static const uintptr_t SLOT_TIMESTAMP_OFF    = 0xb48;   // save date/time string

// ---- Per-slot DLC state text + the Enable/View DLC button ----
static const uint32_t  SLOT_DLC_BTN_FOURCC    = 0x646c6374; // 'tcld' + slot array index (0..8)
static const uintptr_t SLOT_DLCVEC_BEGIN_OFF  = 0x790;  // SaveSlot: installed-DLC records begin
static const uintptr_t SLOT_DLCVEC_END_OFF    = 0x798;  // SaveSlot: end (stride 0xc0)

// ---- The slot's own NEW GAME / DELETE actions (called, not clicked) ----

static const uint32_t  DELETE_BTN_FOURCC_BASE = 0x6e736274;  // "tbsn" + slotIndex (array index 0..8)
                                                             // (kept for reference; no longer clicked)

// ---- THE SAVE LIST SCROLLS, AND THE MOD CAN DRIVE IT ----
static const uintptr_t FE_SCROLL_MIN_OFF     = 0x242c;
static const uintptr_t FE_SCROLL_MAX_OFF     = 0x2430;  // float: max offset (game-computed) (was 0x2428)
static const uintptr_t FE_SCROLL_CUR_OFF     = 0x2434;
static const uintptr_t FE_SCROLL_TARGET_OFF  = 0x243c;
static const uintptr_t FE_SCROLL_ENABLED_OFF = 0x2444;

static const float SAVE_ROW_VIS_TOP = 574.0f;   // first fully-visible row position
static const float SAVE_ROW_VIS_BOT = 848.0f;   // last fully-INTERACTIVE row position

// Return the live FrontEndDisplay pointer, or 0 if the front-end isn't up.
uintptr_t frontEndDisplay(uintptr_t base) {
    uintptr_t disp = 0;
    if (!safeReadPtr(base + FE_DISPLAY_PTR_RVA, &disp)) return 0;
    return (disp > 0x10000) ? disp : 0;
}

int frontEndState(uintptr_t display) {
    uint32_t s = 0;
    if (!safeReadU32(display + FE_STATE_OFF, &s)) return -1;
    return (int)s;
}

static void difficultyLabel(uintptr_t base, const char* mode, char* out, int outsz) {
    out[0] = 0;
    if (!mode || !mode[0]) return;
    char key[160];
    _snprintf(key, sizeof key, "fe_flow_mode_%s_confirm", mode);
    key[sizeof key - 1] = 0;
    if (resolveKey(base, key, out, outsz) && out[0]) return;
    logLine("saveslot: %s did not resolve -> raw mode name", key);
    strncpy(out, mode, outsz - 1);
    out[outsz - 1] = 0;
}

static bool contentRegistryHasEntries(uintptr_t base) {
    uintptr_t reg = 0, b = 0, e = 0;
    if (!safeReadPtr(base + FE_CONTENT_REG_RVA, &reg) || reg <= 0x10000) return false;
    if (!safeReadPtr(reg + 0x08, &b) || !safeReadPtr(reg + 0x10, &e)) return false;
    return b > 0x10000 && e > b;
}

static bool slotDlcStateText(uintptr_t slot, char* out, int outsz) {
    out[0] = 0;
    if (!contentRegistryHasEntries(g_base)) return false;
    uintptr_t b = 0, e = 0;
    if (!safeReadPtr(slot + SLOT_DLCVEC_BEGIN_OFF, &b) ||
        !safeReadPtr(slot + SLOT_DLCVEC_END_OFF, &e)) return false;
    bool installed = (b > 0x10000 && e > b);
    const char* key = installed ? "str_dlc_installed" : "str_dlc_disabled";
    if (!resolveKey(g_base, key, out, outsz) || !out[0]) {
        logLine("saveslot: %s did not resolve -> DLC state omitted", key);
        out[0] = 0;
        return false;
    }
    return true;
}

static void appendField(char* out, int outsz, const char* piece) {
    if (!piece || !piece[0]) return;
    size_t len = strlen(out);
    if ((int)len >= outsz - 1) return;
    _snprintf(out + len, outsz - (int)len, ", %s", piece);
    out[outsz - 1] = 0;
}

static void appendSentence(char* out, int outsz, const char* s) {
    if (!s || !s[0]) return;
    size_t len = strlen(out);
    if ((int)len >= outsz - 1) return;
    _snprintf(out + len, outsz - (int)len, (len && out[len - 1] == '.') ? " %s" : ". %s", s);
    out[outsz - 1] = 0;
}

bool resolveSaveSlot(uintptr_t display, int64_t id, char* out, int outsz) {
    uint32_t cc = (uint32_t)(uint64_t)id;
    uint32_t off = cc - SLOT_FOURCC_BASE;
    if (off >= (uint32_t)SLOT_COUNT) return false;
    int offset = (int)off;

    if (offset == SLOT_CIRCUS_INDEX) {
        char name[128], act[128];
        if (!resolveKey(g_base, "str_start_screen_circus_button", name, sizeof name) || !name[0]) {
            strncpy(name, axs(AXS_FE_FB_CIRCUS), sizeof name - 1);
            name[sizeof name - 1] = 0;
            logLine("saveslot: str_start_screen_circus_button did not resolve -> fallback");
        }
        if (!resolveKey(g_base, "front_end_controls_element_load_circus", act, sizeof act) || !act[0]) {
            strncpy(act, axs(AXS_FE_FB_ENTER_CIRCUS), sizeof act - 1);
            act[sizeof act - 1] = 0;
            logLine("saveslot: front_end_controls_element_load_circus did not resolve -> fallback");
        }
        _snprintf(out, outsz, "%s, %s", name, act);
        out[outsz - 1] = 0;
        return true;
    }

    int slotNum = offset;                 // offset 1 = "Slot 1", etc.
    int arrayIdx = offset - 1;            // campaign save-array element for this slot

    uintptr_t slotBase = 0;
    if (!safeReadPtr(display + FE_SLOTARR_OFF, &slotBase) || slotBase <= 0x10000) {
        // Can't reach the slot data — fall back to a bare, honest label.
        _snprintf(out, outsz, axs(AXS_SLOT_N), slotNum);
        out[outsz - 1] = 0;
        return true;
    }
    uintptr_t slot = slotBase + (uintptr_t)arrayIdx * SLOT_STRIDE;

    // Read every display field (best-effort; each guarded).
    uint8_t occ = 0, alt = 0;
    safeReadU8(slot + SLOT_OCCUPIED_OFF, &occ);
    safeReadU8(slot + SLOT_ALT_OFF, &alt);

    char location[0x84] = { 0 }, estate[0x84] = { 0 };
    char mode[0x84] = { 0 }, timestamp[0x84] = { 0 };
    safeReadCStr(slot + SLOT_LOCATION_OFF,    location,  sizeof location);
    safeReadCStr(slot + SLOT_ESTATE_DISP_OFF, estate,    sizeof estate);
    safeReadCStr(slot + SLOT_MODE_NAME_OFF,   mode,      sizeof mode);
    safeReadCStr(slot + SLOT_TIMESTAMP_OFF,   timestamp, sizeof timestamp);

    int32_t week = 0;
    safeReadU32(slot + SLOT_WEEK_OFF, (uint32_t*)&week);

    // Diagnostics: raw dump of every field so any wording can still be re-checked.
    logLine("slotfields idx=%d occ=%d alt=%d week=%d loc=\"%s\" estate=\"%s\" "
            "mode=\"%s\" ts=\"%s\"",
            arrayIdx, occ, alt, week, location, estate, mode, timestamp);

    if (occ == 0) {
        _snprintf(out, outsz, axs(AXS_SLOT_N_EMPTY), slotNum);
        out[outsz - 1] = 0;
    } else if (location[0] == '\0') {
        // Occupied but no raid yet (no last-known location) — a freshly named estate.
        _snprintf(out, outsz, axs(AXS_SLOT_N_NAMED), slotNum, estate[0] ? estate : axs(AXS_SLOT_UNNAMED));
        out[outsz - 1] = 0;
    } else {
        // Active campaign: estate, week, difficulty, timestamp.
        _snprintf(out, outsz, axs(AXS_SLOT_N_ACTIVE), slotNum, estate[0] ? estate : axs(AXS_SLOT_UNNAMED));
        out[outsz - 1] = 0;
        char weekBuf[64] = { 0 };
        _snprintf(weekBuf, sizeof weekBuf, axs(AXS_SLOT_WEEK_N), week);
        weekBuf[sizeof weekBuf - 1] = 0;
        char diff[128];
        difficultyLabel(g_base, mode, diff, sizeof diff);
        appendField(out, outsz, diff);
        appendField(out, outsz, weekBuf);
        appendField(out, outsz, timestamp);
    }
    if (occ != 0) {
        char dlcState[96];
        if (slotDlcStateText(slot, dlcState, sizeof dlcState))
            appendField(out, outsz, dlcState);
    }
    if (occ != 0) {
        appendSentence(out, outsz, axs(location[0] ? AXS_SLOT_HINT_LOAD : AXS_NM_SEAL_HELP));
        if (contentRegistryHasEntries(g_base))
            appendSentence(out, outsz, axs(AXS_SLOT_HINT_DLC));
        appendSentence(out, outsz, axs(AXS_SLOT_HINT_DELETE));
    }
    return true;
}

static bool isSaveSlotId(int64_t id) {
    uint32_t off = (uint32_t)(uint64_t)id - SLOT_FOURCC_BASE;
    return off < (uint32_t)SLOT_COUNT;
}

static uintptr_t saveSlotScreen(uintptr_t base) {
    uintptr_t disp = frontEndDisplay(base);
    if (!disp) return 0;
    return (frontEndState(disp) == FE_STATE_SAVESLOTS) ? disp : 0;
}

static bool campaignSlotFromId(int64_t id, int* arrayIdx, int* slotNum) {
    uint32_t off = (uint32_t)(uint64_t)id - SLOT_FOURCC_BASE;
    if (off == 0 || off >= (uint32_t)SLOT_COUNT) return false;   // 0 = Circus
    if (arrayIdx) *arrayIdx = (int)off - 1;
    if (slotNum)  *slotNum  = (int)off;
    return true;
}

static bool saveSlotOccupied(uintptr_t display, int arrayIdx, bool* okOut, bool* inUseOut = nullptr) {
    if (okOut) *okOut = false;
    if (inUseOut) *inUseOut = false;
    uintptr_t slotBase = 0;
    if (!safeReadPtr(display + FE_SLOTARR_OFF, &slotBase) || slotBase <= 0x10000) return false;
    uintptr_t slot = slotBase + (uintptr_t)arrayIdx * SLOT_STRIDE;
    uint8_t occ = 0, alt = 0;
    if (!safeReadU8(slot + SLOT_OCCUPIED_OFF, &occ)) return false;
    safeReadU8(slot + SLOT_ALT_OFF, &alt);
    if (okOut) *okOut = true;
    if (inUseOut) *inUseOut = (occ != 0);
    return occ != 0 || alt != 0;
}

typedef void (*SlotActionFn)(void*, unsigned int);
static bool sehSlotAction(uintptr_t base, uintptr_t rva, uintptr_t display, int arrayIdx) {
    __try { ((SlotActionFn)(base + rva))((void*)display, (unsigned int)arrayIdx); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// ---- Front-end / menu keyboard navigation ----

int64_t g_feFocusId   = -1;

static bool saveScrollFollowTo(uintptr_t base, int64_t id, float elemY);
static void saveScrollFollowCancel(const char* why);
static int     g_feScreenKey = -1;

struct FePick {
    uintptr_t elem;      // 0 = nothing to move to
    int64_t   id;
    int64_t   curId;     // where we started from (for the log line)
    bool      haveCur;
    bool      optList;
};

static FePick feFindCandidate(int dir, bool forceSeed) {
    FePick p = { 0, 0, -1, false, false };
    uintptr_t begin = 0, end = 0;
    if (!safeReadPtr(g_base + VEC_BEGIN_RVA, &begin) ||
        !safeReadPtr(g_base + VEC_END_RVA, &end) || begin == 0 || end <= begin) {
        logLine("fe-move dir=%d: focus vector empty", dir);
        return p;
    }
    uintptr_t count = (end - begin) / ELEM_STRIDE;
    if (count > 4096) count = 4096;

    uintptr_t disp = frontEndDisplay(g_base);
    int st = disp ? frontEndState(disp) : -1;
    int screenKey = ((int)currentAxContext() << 8) | (st & 0xff);
    if (screenKey != g_feScreenKey) { g_feScreenKey = screenKey; g_feFocusId = -1; g_cursorValid = false; }

    float dirx = 0.0f, diry = 0.0f;
    switch (dir) { case 0: diry = -1.0f; break; case 1: diry = 1.0f; break;
                   case 2: dirx = -1.0f; break; case 3: dirx = 1.0f; break; }

    int64_t gameId = 0; safeReadI64(g_base + FOCUS_ID_RVA, &gameId);
    int64_t curId = !isNoFocus(gameId) ? gameId : g_feFocusId;
    if (forceSeed) curId = -1;

    const bool pauseModal = g_pauseOpen;
    if (pauseModal && !isNoFocus(curId) && lookupPause((uint32_t)(uint64_t)curId) == nullptr &&
        !isOptionId((uint32_t)(uint64_t)curId))
        curId = -1;

    const bool dialogModal = confirmDialogOpen(g_base);
    if (dialogModal && !isNoFocus(curId) &&
        (uint32_t)((uint32_t)(uint64_t)curId - CONFIRM_ANSWER_BASE) >= (uint32_t)CONFIRM_ANSWER_MAX)
        curId = -1;                                  // seeded off-dialog -> restart on an answer

    const bool slotList = (disp != 0 && st == FE_STATE_SAVESLOTS && !dialogModal);
    if (slotList && !isNoFocus(curId) && !isSaveSlotId(curId))
        curId = -1;                                  // seeded onto a button -> restart at the top

    const bool optList = (!dialogModal && optPageActive());
    if (optList && !isNoFocus(curId)) {
        uint32_t cc = (uint32_t)(uint64_t)curId;
        if (!isOptionId(cc))            curId = -1;
        else if (!optIsAnchorId(cc)) {
            int64_t a = optRowAnchorId(optRowPos(cc));
            curId = a ? a : -1;
        }
    }

    p.optList = optList;

    bool haveCur = false; float cx = 0.0f, cy = 0.0f;
    if (!isNoFocus(curId)) {
        for (uintptr_t i = 0; i < count; i++) {
            uintptr_t e = begin + i * ELEM_STRIDE;
            int64_t eid = 0;
            if (safeReadI64(e + ELEM_ID_OFF, &eid) && eid == curId) {
                haveCur = elemPos(e, &cx, &cy);
                break;
            }
        }
    }

    uintptr_t best = 0; float bestScore = 0.0f; int64_t bestId = 0;
    for (uintptr_t i = 0; i < count; i++) {
        uintptr_t e = begin + i * ELEM_STRIDE;
        uint8_t f = 0, s = 0;
        safeReadU8(e + ELEM_FOCUSABLE_OFF, &f);
        safeReadU8(e + ELEM_SKIP_OFF, &s);
        if (!f || s) continue;
        int64_t eid = 0; safeReadI64(e + ELEM_ID_OFF, &eid);
        if (haveCur && eid == curId) continue;          // don't pick ourselves
        if (isFrontEndSkipId(eid)) continue;            // confirmed off-screen / cancel affordance
        if (pauseModal && lookupPause((uint32_t)(uint64_t)eid) == nullptr &&
            !isOptionId((uint32_t)(uint64_t)eid))
            continue;                                   // modal: the menu and its options screens
        if (dialogModal &&
            (uint32_t)((uint32_t)(uint64_t)eid - CONFIRM_ANSWER_BASE) >= (uint32_t)CONFIRM_ANSWER_MAX)
            continue;                                   // modal: the dialog's own answers only
        if (slotList && !isSaveSlotId(eid)) continue;   // save screen: slots only, never the buttons
        if (optList && !optIsAnchorId((uint32_t)(uint64_t)eid)) continue; // options: one stop per row

        float ex, ey; if (!elemPos(e, &ex, &ey)) continue;

        float score;
        if (haveCur) {
            float dx = ex - cx, dy = ey - cy;
            float along = dx * dirx + dy * diry;         // + = in the pressed direction
            if (along <= 0.5f) continue;                 // behind/beside us -> skip
            float perp = dx * -diry + dy * dirx;
            if (perp < 0.0f) perp = -perp;
            if (perp > along * 2.0f) continue;           // far more sideways than forward -> stray
            score = along + 3.0f * perp;
        } else {
            score = ey * 1000.0f + ex;                   // no focus yet -> top-most, then left-most
        }
        if (best == 0 || score < bestScore) { best = e; bestScore = score; bestId = eid; }
    }

    if (best == 0) {
        logLine("fe-move dir=%d: no candidate (haveCur=%d cur=%.0f,%.0f count=%lu)",
                dir, (int)haveCur, cx, cy, (unsigned long)count);
        return p;
    }
    p.elem = best; p.id = bestId; p.curId = curId; p.haveCur = haveCur;
    return p;
}

static const char* feHeaderFor(bool withSurface);
static bool feCommitTo(int dir, const FePick& p, bool withSurface) {
    uintptr_t best = p.elem; int64_t bestId = p.id;
    float tx = 0, ty = 0;
    if (!elemCenter(best, &tx, &ty)) {
        logLine("fe-move dir=%d: no centre for 0x%llx", dir, (unsigned long long)bestId);
        return false;
    }
    float rowX = 0, rowY = 0;
    bool scrolling = elemPos(best, &rowX, &rowY) &&
                     saveScrollFollowTo(g_base, bestId, rowY);
    if (!scrolling) {
        saveScrollFollowCancel("overtaken by a newer move");
        moveCursorTo(tx, ty);
    }
    g_feFocusId = bestId;                                // remember it as our current position
    logLine("fe-move dir=%d old=0x%llx new=0x%llx cursor=(%.0f,%.0f)%s%s",
            dir, (unsigned long long)p.curId, (unsigned long long)bestId, tx, ty,
            p.haveCur ? "" : " [seed]", scrolling ? " [scrolling, cursor follows]" : "");
    char text[512];
    resolveLabel(g_base, g_tbwVtbl, bestId, text, sizeof text);
    if (text[0]) {                                       // a move is explicit -> always speak
        const char* prefix = feHeaderFor(withSurface);
        char line[640];
        if (prefix && prefix[0]) {
            size_t n = strlen(prefix);
            _snprintf(line, sizeof line, prefix[n - 1] == '.' ? "%s %s" : "%s. %s", prefix, text);
            line[sizeof line - 1] = 0;
        } else {
            strncpy(line, text, sizeof line - 1); line[sizeof line - 1] = 0;
        }
        postSpeech(line);
        g_axSpokenId = bestId;
        strncpy(g_axLastSpoken, text, sizeof g_axLastSpoken - 1);
        g_axLastSpoken[sizeof g_axLastSpoken - 1] = 0;
    }
    return true;
}

static void feLandingCancel(const char* why);
static void feLandingClaim(const char* why);

static const char* feHeaderFor(bool withSurface) {
    uintptr_t disp = frontEndDisplay(g_base);
    if (g_titlePrefixPending && disp && frontEndState(disp) == FE_STATE_TITLE) {
        g_titlePrefixPending = false;
        return kTitleScreenName;
    }
    if (withSurface && g_pauseOpen && !g_axOptionsUp) return axs(AXS_PM_HEAD);
    return nullptr;
}

static bool feFocusStep(int dir, bool forceSeed, bool withSurface) {
    FePick p = feFindCandidate(dir, forceSeed);

    if (!forceSeed && p.optList && (dir == 0 || dir == 1) && optListStep(dir)) {
        feLandingCancel("options row walk");
        return true;
    }
    if (!p.elem) return false;
    if (!feCommitTo(dir, p, withSurface)) return false;
    feLandingCancel("focus moved");
    return true;
}

void frontEndFocusMove(int dir) { feFocusStep(dir, false, false); }

void feReannounce(uintptr_t base) {
    (void)base;
    FePick p = { 0, 0, -1, false, false };
    if (!isNoFocus(g_feFocusId)) {
        uintptr_t elem = feGetElementById(g_feFocusId);
        if (elem) { p.elem = elem; p.id = g_feFocusId; p.curId = g_feFocusId; p.haveCur = true; }
    }
    if (!p.elem) p = feFindCandidate(1, true);
    if (!p.elem) {
        const char* head = feHeaderFor(true);
        logLine("fe-reannounce: no item to hand back to%s", head ? " -- header alone" : "");
        if (head && head[0]) postSpeech(head);
        return;
    }
    feCommitTo(1, p, true);
}

int64_t feCurrentFocusId() {
    int64_t gameId = 0; safeReadI64(g_base + FOCUS_ID_RVA, &gameId);
    return !isNoFocus(gameId) ? gameId : g_feFocusId;
}

// ---- Save-slot screen: Enter on an empty slot, Delete on a full one ----
enum SlotActionResult {
    SLOT_ACTION_NA,      // not applicable here — caller should fall through
    SLOT_ACTION_DONE,    // the action was called
    SLOT_ACTION_REFUSED
};

static bool  g_ngcActive     = false;
static int   g_ngcSlotIdx    = -1;
static DWORD g_ngcArmedTick  = 0;      // GetTickCount at the New Game press; 0 = disarmed
static int   g_ngcViewArm    = 0;
static bool  g_ngcViewMode   = false;

// ---- Save-slot return watch ----
static bool  g_srwArmed    = false;
static int   g_srwSlotNum  = 0;
static bool  g_srwSeenBusy = false;
static DWORD g_srwArmedAt  = 0;
static DWORD g_srwFireAt   = 0;      // flow ended -> settle delay before landing

static void armSlotReturnWatch(int slotNum) {
    g_srwArmed = true; g_srwSlotNum = slotNum;
    g_srwSeenBusy = false;
    g_srwArmedAt = GetTickCount(); g_srwFireAt = 0;
}

static SlotActionResult saveSlotAction(uintptr_t base, bool wantOccupied) {
    uintptr_t disp = saveSlotScreen(base);
    if (!disp) return SLOT_ACTION_NA;              // not the save screen -> not our key

    if (confirmDialogOpen(base)) return SLOT_ACTION_NA;

    int64_t id = feCurrentFocusId();
    int idx = 0, slotNum = 0;
    if (!campaignSlotFromId(id, &idx, &slotNum)) {
        // The Circus row, or focus not on a slot at all. Neither action exists there.
        logLine("slotaction: focus 0x%llx is not a campaign slot", (unsigned long long)id);
        return SLOT_ACTION_NA;
    }

    bool readOk = false, inUse = false;
    bool occupied = saveSlotOccupied(disp, idx, &readOk, &inUse);
    if (!readOk) {
        logLine("slotaction: slot %d occupancy unreadable — refusing", slotNum);
        postSpeech(axs(AXS_SLOT_CANT_READ));
        return SLOT_ACTION_REFUSED;
    }

    char say[96];
    bool ok;
    if (wantOccupied) {
        if (!occupied) {
            logLine("slotaction: slot %d is empty — no delete there", slotNum);
            return SLOT_ACTION_REFUSED;            // caller says "That slot is empty."
        }
        _snprintf(say, sizeof say, axs(AXS_SLOT_DELETE_N), slotNum);
        say[sizeof say - 1] = 0;
        postSpeech(say);
        ok = sehSlotAction(base, FE_DELETE_RVA, disp, idx);
        logLine("slotaction: delete slot %d (idx=%d) -> %s", slotNum, idx, ok ? "called" : "FAULTED");
        if (!ok) postSpeech(axs(AXS_ACTION_FAILED));
    } else {
        _snprintf(say, sizeof say, axs(inUse ? AXS_SLOT_LOADING_N : AXS_SLOT_NEWGAME_N), slotNum);
        say[sizeof say - 1] = 0;
        postSpeech(say);
        ok = sehSlotAction(base, FE_NEWGAME_RVA, disp, idx);
        logLine("slotaction: %s slot %d (idx=%d) -> %s", inUse ? "begin" : "new game",
                slotNum, idx, ok ? "called" : "FAULTED");
        if (!ok) postSpeech(axs(AXS_ACTION_FAILED));
        if (ok && !inUse) { g_ngcArmedTick = GetTickCount(); g_ngcViewArm = 0; }
    }
    if (ok) armSlotReturnWatch(slotNum);
    return SLOT_ACTION_DONE;
}

// ---- Shift+Enter: the focused slot's Enable/View DLC button ----
static SlotActionResult slotDlcAction(uintptr_t base) {
    uintptr_t disp = saveSlotScreen(base);
    if (!disp) return SLOT_ACTION_NA;              // not the save screen -> not our chord
    if (confirmDialogOpen(base)) return SLOT_ACTION_NA;   // modal -- same gate as saveSlotAction
    if (g_ngcActive) return SLOT_ACTION_NA;        // the panel is already up -- nothing to open

    int64_t id = feCurrentFocusId();
    int idx = 0, slotNum = 0;
    if (!campaignSlotFromId(id, &idx, &slotNum)) {
        logLine("slotdlc: focus 0x%llx is not a campaign slot", (unsigned long long)id);
        return SLOT_ACTION_NA;
    }
    int64_t btnId = (int64_t)(SLOT_DLC_BTN_FOURCC + (uint32_t)idx);
    if (!feGetElementById(btnId)) {
        logLine("slotdlc: slot %d has no 'tcld' element on screen", slotNum);
        return SLOT_ACTION_REFUSED;
    }
    char label[160];
    if (!resolveKey(base, "str_dlc_show_panel_tooltip", label, sizeof label) || !label[0]) {
        logLine("slotdlc: str_dlc_show_panel_tooltip did not resolve -> fallback");
        strncpy(label, axs(AXS_DLC_BTN_FALLBACK), sizeof label - 1);
        label[sizeof label - 1] = 0;
    }
    postSpeech(label);
    bool ok = frontEndClickElementId(btnId);
    logLine("slotdlc: slot %d (idx=%d) 'tcld' 0x%llx -> %s", slotNum, idx,
            (unsigned long long)btnId, ok ? "clicked" : "CLICK REFUSED");
    if (!ok) { postSpeech(axs(AXS_ACTION_FAILED)); return SLOT_ACTION_DONE; }
    g_ngcArmedTick = GetTickCount();               // the content-panel watch announces the window
    g_ngcViewArm   = slotNum;                      // ...as the VIEW flavour, for this slot
    return SLOT_ACTION_DONE;
}

static void saveSlotDeleteGateCheck(uintptr_t base) {
    static bool done = false;
    if (done) return;
    done = true;
    char owner[0x44];
    if (kbActionForKey(base, (int)SDLK_DELETE, owner, sizeof owner))
        logLine("slotaction GATE: Delete is BOUND to the game action \"%s\" — claiming it takes it away", owner);
    else
        logLine("slotaction GATE: Delete is not in the keyboard binding table — safe to claim");
}

// ---- SCROLLING THE SAVE LIST TO FOLLOW THE CURSOR ----
static bool  g_ssfArmed   = false;
static int64_t g_ssfId    = 0;       // the slot element we are scrolling to
static DWORD g_ssfUntil   = 0;
static int   g_ssfSkipPolls = 0;

static void saveScrollFollowCancel(const char* why) {
    if (!g_ssfArmed) return;
    logLine("savescroll: follow for 0x%llx cancelled (%s)", (unsigned long long)g_ssfId, why);
    g_ssfArmed = false;
}

static bool saveScrollRead(uintptr_t disp, float* cur, float* target, bool* enabled) {
    uint32_t cb = 0, tb = 0; uint8_t en = 0;
    if (!safeReadU32(disp + FE_SCROLL_CUR_OFF, &cb))    return false;
    if (!safeReadU32(disp + FE_SCROLL_TARGET_OFF, &tb)) return false;
    safeReadU8(disp + FE_SCROLL_ENABLED_OFF, &en);
    float c = u32AsFloatM(cb), t = u32AsFloatM(tb);
    if (!(c > -8000.0f && c < 8000.0f) || !(t > -8000.0f && t < 8000.0f)) return false;
    *cur = c; *target = t; *enabled = (en != 0);
    return true;
}

static bool saveScrollToShow(uintptr_t disp, float elemY) {
    float cur = 0, target = 0; bool enabled = false;
    if (!saveScrollRead(disp, &cur, &target, &enabled)) {
        logLine("savescroll: block unreadable — not scrolling");
        return false;
    }
    if (!enabled) {                         // the update would force both back to 0 anyway
        logLine("savescroll: scrolling disabled (enabled byte == 0) — not scrolling");
        return false;
    }
    float want = elemY;
    if (elemY > SAVE_ROW_VIS_BOT)      want = SAVE_ROW_VIS_BOT;
    else if (elemY < SAVE_ROW_VIS_TOP) want = SAVE_ROW_VIS_TOP;
    else return false;                      // already inside the band — nothing to do
    float newTarget = cur + (elemY - want);
    bool snap = false;
    uint32_t mnb = 0, mxb = 0;
    if (safeReadU32(disp + FE_SCROLL_MIN_OFF, &mnb) && safeReadU32(disp + FE_SCROLL_MAX_OFF, &mxb)) {
        float mn = u32AsFloatM(mnb), mx = u32AsFloatM(mxb);
        if (mn <= mx && mn > -8000.0f && mx < 8000.0f) {
            if (newTarget < mn) newTarget = mn;
            if (newTarget > mx) newTarget = mx;
            snap = true;
        }
    }
    safeWriteU32(disp + FE_SCROLL_TARGET_OFF, *reinterpret_cast<uint32_t*>(&newTarget));
    if (snap)
        safeWriteU32(disp + FE_SCROLL_CUR_OFF, *reinterpret_cast<uint32_t*>(&newTarget));
    logLine("savescroll: row y=%.0f -> want %.0f; target %.1f -> %.1f (cur %.1f)%s",
            elemY, want, target, newTarget, cur,
            snap ? " [snapped]" : " [eased -- bounds unreadable]");
    return true;
}

static const DWORD SSF_TIMEOUT_MS = 900;    // ease is fast; this is only a backstop
void serviceSaveScrollFollow(uintptr_t base) {
    if (!g_ssfArmed) return;
    uintptr_t disp = saveSlotScreen(base);
    if (!disp) { g_ssfArmed = false; return; }          // left the screen -> nothing to land on
    if (g_ssfSkipPolls > 0) { g_ssfSkipPolls--; return; }

    float cur = 0, target = 0; bool enabled = false;
    bool settled = true;
    if (saveScrollRead(disp, &cur, &target, &enabled)) {
        float d = cur - target; if (d < 0) d = -d;
        settled = (d < 1.0f);
    }
    bool expired = ((int32_t)(GetTickCount() - g_ssfUntil) >= 0);
    if (!settled && !expired) return;

    g_ssfArmed = false;
    uintptr_t elem = feGetElementById(g_ssfId);
    float tx = 0, ty = 0;
    if (elem && elemCenter(elem, &tx, &ty)) {
        moveCursorTo(tx, ty);                            // the motion event the hover needs
        logLine("savescroll: follow placed cursor on 0x%llx at (%.0f,%.0f)%s",
                (unsigned long long)g_ssfId, tx, ty, expired ? " [deadline]" : "");
    } else {
        logLine("savescroll: follow FAILED — element 0x%llx not on screen",
                (unsigned long long)g_ssfId);
    }
}

static const float SSF_PARK_X = 8.0f, SSF_PARK_Y = 8.0f;   // design space; inert corner
static bool saveScrollFollowTo(uintptr_t base, int64_t id, float elemY) {
    uintptr_t disp = saveSlotScreen(base);
    if (!disp || !isSaveSlotId(id)) return false;
    if (!saveScrollToShow(disp, elemY)) return false;
    moveCursorTo(SSF_PARK_X, SSF_PARK_Y);
    g_ssfArmed = true; g_ssfId = id; g_ssfUntil = GetTickCount() + SSF_TIMEOUT_MS;
    g_ssfSkipPolls = 2;
    return true;
}

// ---- SCROLL PROBE: what is actually ON the campaign screen? ----
static DWORD g_sspArmedAt = 0;      // save screen seen open at this tick; 0 = idle
static bool  g_sspDumped  = false;  // this visit already dumped
static const DWORD SSP_SETTLE_MS = 1200;

void checkSaveScrollProbe(uintptr_t base) {
    if (!axDebugLogEnabled()) return;                // diagnostic: nothing runs with the log off
    uintptr_t disp = saveSlotScreen(base);
    if (!disp) { g_sspArmedAt = 0; g_sspDumped = false; return; }   // left the screen -> re-arm
    if (g_sspDumped) return;
    DWORD now = GetTickCount();
    if (!g_sspArmedAt) { g_sspArmedAt = now; return; }
    if (now - g_sspArmedAt < SSP_SETTLE_MS) return;
    g_sspDumped = true;

    uintptr_t begin = 0; long count = focusVectorCount(base);
    safeReadPtr(base + VEC_BEGIN_RVA, &begin);
    logLine("savescroll: DUMP follows (%ld elements)", count);
    logDump("================ savescroll DUMP ================");
    logDump("%ld focus elements, settled %ums after the screen opened.", count, SSP_SETTLE_MS);
    logDump("Known families: slots 0x%x+0..9, delete buttons 0x%x+0..8.",
            SLOT_FOURCC_BASE, DELETE_BTN_FOURCC_BASE);
    logDump("Anything marked ?CANDIDATE is unaccounted for -- the scrollbar arrows, if they are");
    logDump("focus elements at all, are among those. Design space is 1920x1080.");
    for (long i = 0; i < count && i < 64 && begin; i++) {
        uintptr_t e = begin + (uintptr_t)i * ELEM_STRIDE;
        int64_t id = 0; safeReadI64(e + ELEM_ID_OFF, &id);
        float x = 0, y = 0; elemPos(e, &x, &y);
        uint32_t wb = 0, hb = 0;
        safeReadU32(e + ELEM_SIZE_OFF, &wb); safeReadU32(e + ELEM_SIZE_OFF + 4, &hb);
        uint8_t f = 0, s = 0;
        safeReadU8(e + ELEM_FOCUSABLE_OFF, &f); safeReadU8(e + ELEM_SKIP_OFF, &s);
        char tag[9]; idToAscii(id, tag);
        const char* known = isSaveSlotId(id) ? "SLOT"
                          : ((uint32_t)((uint32_t)(uint64_t)id - DELETE_BTN_FOURCC_BASE) < 9u ? "delete"
                          : "?CANDIDATE");
        logDump("  fe[%2ld] id=0x%llx \"%s\" %-10s pos=(%.0f,%.0f) size=(%.0fx%.0f) focusable=%u skip=%u%s",
                i, (unsigned long long)id, tag, known, x, y,
                u32AsFloatM(wb), u32AsFloatM(hb), f, s,
                (y > 1080.0f || y < 0.0f) ? "  <-- OFF the 1080 design space" : "");
    }
    {
        float cur = 0, target = 0; bool enabled = false;
        uint32_t mn = 0, mx = 0;
        safeReadU32(disp + FE_SCROLL_MIN_OFF, &mn);
        safeReadU32(disp + FE_SCROLL_MAX_OFF, &mx);
        bool ok = saveScrollRead(disp, &cur, &target, &enabled);
        logDump("scroll block: readable=%d enabled=%d cur=%.2f target=%.2f min=%.2f max=%.2f",
                (int)ok, (int)enabled, cur, target, u32AsFloatM(mn), u32AsFloatM(mx));
        logDump("  (model: write TARGET only; the game clamps it and eases cur toward it)");
    }
    logDump("display=0x%llx state=%d -- raw window, nonzero words only:",
            (unsigned long long)disp, frontEndState(disp));
    for (uintptr_t off = FE_SLOTARR_OFF - 0x18; off <= FE_SLOTARR_OFF + 0x58; off += 4) {
        uint32_t v = 0;
        if (safeReadU32(disp + off, &v) && v)
            logDump("  disp+0x%llx = 0x%08x (%d, %.3f)", (unsigned long long)off, v, (int)v,
                    u32AsFloatM(v));
    }
    logDump("================ end savescroll DUMP ============");
}

static const DWORD SRW_OPEN_TIMEOUT_MS = 2000;  // flow never appeared -> give up
static const DWORD SRW_SETTLE_MS       = 500;   // flow ended -> let the outcome land first
void checkSlotReturn(uintptr_t base) {
    if (!g_srwArmed) return;
    DWORD now = GetTickCount();
    uintptr_t fe = frontEndDisplay(base);
    if (!fe) {
        g_srwArmed = false;
        logLine("slotreturn: front end gone — disarmed");
        return;
    }
    int sub = feNamingSub(fe);
    bool busy = confirmDialogOpen(base) || g_naming || g_ngcActive ||
                sub == (int)FE_NAMING_SUB_MODEDIALOG || sub == FE_NAMING_SUB_NAMING;
    if (busy) {
        g_srwSeenBusy = true; g_srwFireAt = 0;
        return;
    }

    if (!g_srwSeenBusy) {
        if (now - g_srwArmedAt > SRW_OPEN_TIMEOUT_MS) {
            g_srwArmed = false;
            logLine("slotreturn: no dialog/naming flow within %ums — disarmed", SRW_OPEN_TIMEOUT_MS);
        }
        return;
    }

    if (frontEndState(fe) != FE_STATE_SAVESLOTS) {     // flow ended somewhere else
        g_srwArmed = false;
        logLine("slotreturn: flow ended off the save-slot screen (state=%d) — disarmed",
                frontEndState(fe));
        return;
    }
    if (!g_srwFireAt) { g_srwFireAt = now + SRW_SETTLE_MS; return; }
    if ((int32_t)(now - g_srwFireAt) < 0) return;
    g_srwArmed = false;

    int64_t id = (int64_t)(SLOT_FOURCC_BASE + (uint32_t)g_srwSlotNum);
    uintptr_t elem = feGetElementById(id);
    float tx = 0, ty = 0, rowX = 0, rowY = 0;
    bool placed = (elem != 0 && elemCenter(elem, &tx, &ty));
    if (placed) {
        bool scrolling = elemPos(elem, &rowX, &rowY) &&
                         saveScrollFollowTo(base, id, rowY);
        if (!scrolling) {
            saveScrollFollowCancel("overtaken by the slot-return landing");
            moveCursorTo(tx, ty);
        }
        g_feFocusId = id;
        g_feScreenKey = ((int)currentAxContext() << 8) | (FE_STATE_SAVESLOTS & 0xff);
    }
    char text[512];
    resolveLabel(base, g_tbwVtbl, id, text, sizeof text);
    logLine("slotreturn: slot %d elem=%s text=\"%s\"", g_srwSlotNum,
            placed ? "placed" : "MISSING", text);
    if (text[0]) {
        postSpeech(text);
        g_axSpokenId = id;
        strncpy(g_axLastSpoken, text, sizeof g_axLastSpoken - 1);
        g_axLastSpoken[sizeof g_axLastSpoken - 1] = 0;
    }
    feLandingClaim("slot return landed");
}

// ---- EVERY FRONT-END SCREEN LANDS ON ITS FIRST ITEM ----
static const DWORD FEL_MIN_MS     = 120;
static const DWORD FEL_TICK_MS    = 60;    // gap between the two picks that have to agree
static const DWORD FEL_TIMEOUT_MS = 2500;
static const float FEL_STILL_PX   = 0.5f;

static bool     g_felArmed    = false;
static DWORD    g_felArmedAt  = 0;
static DWORD    g_felNextAt   = 0;
static int64_t  g_felLastPick = 0;         // the previous tick's answer (0 = none yet)
static float    g_felLastX    = 0.0f;      // ... and where its rect was on that tick
static float    g_felLastY    = 0.0f;
static int      g_felCtx      = -1;
static int      g_felState    = -999;
static uint32_t g_felPage     = 0;
static bool     g_felBlocked  = true;
static bool     g_felClaimed  = false;

static void feLandingCancel(const char* why) {
    if (!g_felArmed) return;
    g_felArmed = false;
    logLine("fe-land: cancelled (%s)", why);
}

static void feLandingClaim(const char* why) {
    feLandingCancel(why);                  // in case one IS armed (harmless no-op otherwise)
    g_felClaimed = true;
    logLine("fe-land: claimed (%s) -- next screen edge records without arming", why);
}

void serviceFrontEndLanding(uintptr_t base) {
    AxContext ctx = currentAxContext();
    bool landable = (ctx == AX_TITLE || ctx == AX_PAUSE);
    uintptr_t disp = frontEndDisplay(base);
    int st = disp ? frontEndState(disp) : -1;
    bool blocked = !landable || st == FE_STATE_PREAMBLE ||
                   confirmDialogOpen(base) || g_ngcActive || g_srwArmed;
    uint32_t page = landable ? optPageId() : 0;

    DWORD now = GetTickCount();
    if (blocked != g_felBlocked || (int)ctx != g_felCtx || st != g_felState || page != g_felPage) {
        g_felCtx = (int)ctx; g_felState = st; g_felPage = page; g_felBlocked = blocked;
        bool claimed = g_felClaimed; g_felClaimed = false;
        if (blocked) {
            feLandingCancel("screen not landable");
        } else if (claimed) {
            logLine("fe-land: edge claimed by its own surface (ctx=%s state=%d page=0x%x) -- not arming",
                    ctx == AX_PAUSE ? "pause" : "title", st, page);
        } else {
            g_felArmed = true; g_felArmedAt = now; g_felNextAt = now + FEL_MIN_MS;
            g_felLastPick = 0;
            logLine("fe-land: armed (ctx=%s state=%d page=0x%x)",
                    ctx == AX_PAUSE ? "pause" : "title", st, page);
            parkCursor("front-end screen edge");
        }
    }
    if (!g_felArmed) return;

    int64_t live = 0; safeReadI64(base + FOCUS_ID_RVA, &live);
    if (!isNoFocus(live)) {
        g_felArmed = false;
        logLine("fe-land: 0x%llx already focused -- disarmed", (unsigned long long)live);
        return;
    }
    if (g_preambleHold) { g_felArmedAt = now; g_felNextAt = now + FEL_MIN_MS; return; }
    if ((int32_t)(now - g_felNextAt) < 0) return;
    g_felNextAt = now + FEL_TICK_MS;

    FePick p = feFindCandidate(1, true);            // speculative: what WOULD we land on?
    float px = 0.0f, py = 0.0f;
    bool havePos = p.elem && elemPos(p.elem, &px, &py);
    if (p.elem && p.id == g_felLastPick) {
        float dx = px - g_felLastX; if (dx < 0) dx = -dx;
        float dy = py - g_felLastY; if (dy < 0) dy = -dy;
        if (!havePos || (dx < FEL_STILL_PX && dy < FEL_STILL_PX)) {
            g_felArmed = false;                     // before the commit: it cancels us anyway
            logLine("fe-land: landing on 0x%llx at (%.0f,%.0f)", (unsigned long long)p.id, px, py);
            feCommitTo(1, p, true);
            return;
        }
        logLine("fe-land: 0x%llx agreed but still moving (%.0f,%.0f -> %.0f,%.0f) -- waiting",
                (unsigned long long)p.id, g_felLastX, g_felLastY, px, py);
    }
    g_felLastPick = p.elem ? p.id : 0;
    g_felLastX = px; g_felLastY = py;

    if (now - g_felArmedAt >= FEL_TIMEOUT_MS) {
        g_felArmed = false;
        if (p.elem) {
            logLine("fe-land: never settled within %ums -- landing on 0x%llx anyway",
                    FEL_TIMEOUT_MS, (unsigned long long)p.id);
            feCommitTo(1, p, true);
            return;
        }
        const char* head = feHeaderFor(true);
        logLine("fe-land: nothing to land on within %ums -- giving up%s",
                FEL_TIMEOUT_MS, head ? ", header alone" : "");
        if (head && head[0]) postSpeech(head);
    }
}

// ---- New-game DLC / mods content-selection step ----
static const uint32_t  NGC_DLC_BASE          = 0x646c6374;  // slot summary "DLC" indicator id (reference)
static const uint32_t  NGC_UGC_BASE          = 0x75676374;  // slot summary "mods" indicator id (reference)
static const uint32_t  NGC_AGC_BASE          = 0x61676320;
static const uint32_t  NGC_OK_ID             = 0x66777264;
static const uintptr_t FE_NEWGAME_SLOTIDX_OFF = 0x1dd4;     // display: slot array index new-game acted on
static const uintptr_t FE_NAMING_FOLLOW_OFF   = 0x1dd0;     // display: slot idx the LETTER is open on, -1 idle
static const uintptr_t FE_CONTENT_FLAG_OFF    = 0x1764;     // display: 1 while the content step is live.
static const uintptr_t FE_AGC_VEC_BEGIN_OFF   = 0x23d0;     // display: vector<DLCEntry*> begin ptr
static const uintptr_t FE_AGC_VEC_END_OFF     = 0x23d8;     // display: vector<DLCEntry*> end ptr
static const uintptr_t NGC_ENT_CBID_OFF       = 0x220;
static const uintptr_t NGC_ENT_DLCID_OFF      = 0x240;      // DLCEntry: DLC id C-string ("crimson_court", ...)
static const uintptr_t NGC_ENT_ENABLED_OFF    = 0x228;      // DLCEntry: enabled/checked state — WRITTEN by the
static const uintptr_t NGC_ENT_CHECKED_OFF    = 0x306;      // DLCEntry: checkbox DISPLAY byte — set only at
                                                            //   (re)build, so it lags a toggle (diagnostic only)

// ---- The content panel as a navigable list (checkboxes + OK) ----
static int   g_ngcCursor  = 0;
static int   g_ngcTogIdx  = -1;
static DWORD g_ngcTogTick = 0;    // when that toggle was clicked
static uint8_t g_ngcTogB228 = 0, g_ngcTogB305 = 0, g_ngcTogB306 = 0;  // entry bytes snapshotted at the click
static const DWORD NGC_SETTLE_MS        = 450;
static const DWORD NGC_TOGGLE_SETTLE_MS = 350;

static const float NGC_ROW_VIS_TOP = 331.0f;
static const float NGC_CLICK_MAX_Y = 850.0f;
static int g_ngcSwapIdx    = -1;
static int g_ngcSwapAnchor = -1;

// The wheel servo's state (one servo at a time; a new arrow move retargets it).
static bool  g_ngcWhlActive   = false;
static int   g_ngcWhlRow      = -1;     // logical row being brought into the band
static DWORD g_ngcWhlDeadline = 0;
static float g_ngcWhlLastY    = -1.0f;
static int   g_ngcWhlStall    = 0;      // consecutive pumps without movement
static int   g_ngcWhlDir      = 0;
static const DWORD NGC_WHEEL_TIMEOUT_MS  = 900;
static const int   NGC_WHEEL_STALL_PUMPS = 10;

// How many DLC checkboxes the panel is showing (the DLCEntry vector length).
static int ngcCount(uintptr_t disp) {
    uintptr_t b = 0, e = 0;
    if (!safeReadPtr(disp + FE_AGC_VEC_BEGIN_OFF, &b) || !safeReadPtr(disp + FE_AGC_VEC_END_OFF, &e))
        return 0;
    if (b <= 0x10000 || e <= b || (e - b) > 0x400) return 0;
    return (int)((e - b) / 8);
}
static uintptr_t ngcEntry(uintptr_t disp, int i) {
    if (g_ngcSwapIdx >= 0) {
        if (i == g_ngcSwapIdx)       i = g_ngcSwapAnchor;
        else if (i == g_ngcSwapAnchor) i = g_ngcSwapIdx;
    }
    uintptr_t b = 0;
    if (i < 0 || !safeReadPtr(disp + FE_AGC_VEC_BEGIN_OFF, &b) || b <= 0x10000) return 0;
    uintptr_t ent = 0;
    if (!safeReadPtr(b + (uintptr_t)i * 8, &ent) || ent <= 0x10000) return 0;
    return ent;
}

static bool ngcSwapEntries(uintptr_t disp, int a, int b) {
    uintptr_t vb = 0;
    if (!safeReadPtr(disp + FE_AGC_VEC_BEGIN_OFF, &vb) || vb <= 0x10000) return false;
    uintptr_t pa = 0, pb = 0;
    if (!safeReadPtr(vb + (uintptr_t)a * 8, &pa) || pa <= 0x10000) return false;
    if (!safeReadPtr(vb + (uintptr_t)b * 8, &pb) || pb <= 0x10000) return false;
    if (!safeWriteU64(vb + (uintptr_t)a * 8, (uint64_t)pb)) return false;
    if (!safeWriteU64(vb + (uintptr_t)b * 8, (uint64_t)pa)) {
        safeWriteU64(vb + (uintptr_t)a * 8, (uint64_t)pa);   // roll back -- never leave a dup
        return false;
    }
    return true;
}

static int64_t ngcCheckboxId(uintptr_t disp, int i);
static void    ngcHoverItem(uintptr_t disp, int cursor);

static bool ngcRowY(uintptr_t disp, int row, float* outY) {
    float x = 0;
    uintptr_t elem = feGetElementById(ngcCheckboxId(disp, row));
    return elem != 0 && elemPos(elem, &x, outY);
}

static int ngcAnchorRow(uintptr_t disp, int n) {
    for (int i = 0; i < n; i++) {
        float y = 0;
        if (ngcRowY(disp, i, &y) && y >= NGC_ROW_VIS_TOP && y <= NGC_CLICK_MAX_Y) return i;
    }
    return -1;
}

static bool ngcBoxPoint(uintptr_t disp, int row, float* bx, float* by) {
    float x = 0, y = 0;
    uintptr_t elem = feGetElementById(ngcCheckboxId(disp, row));
    if (!elem || !elemPos(elem, &x, &y)) return false;
    uint32_t hb = 0; float h = 40.0f;
    if (safeReadU32(elem + ELEM_SIZE_OFF + 4, &hb)) {
        float hf = u32AsFloatM(hb);
        if (hf >= 1.0f && hf <= 4000.0f) h = hf;
    }
    *bx = x + h * 0.5f; *by = y + h * 0.5f;
    return true;
}

static void ngcClickAt(float bx, float by) {
    moveCursorTo(bx, by);
    enqueueSynth(SDL_EVT_MOUSEBUTTONDOWN, SDL_BUTTON_LEFT, 1, 2);
    enqueueSynth(SDL_EVT_MOUSEBUTTONUP,   SDL_BUTTON_LEFT, 0, 3);
}

// ---- The wheel servo ----
static void ngcWheelStart(int row, float rowY) {
    g_ngcWhlActive = true; g_ngcWhlRow = row;
    g_ngcWhlDeadline = GetTickCount() + NGC_WHEEL_TIMEOUT_MS;
    g_ngcWhlLastY = rowY; g_ngcWhlStall = 0;
    g_ngcWhlDir = (rowY > NGC_CLICK_MAX_Y) ? -1 : 1;
    enqueueSynthWheel(g_ngcWhlDir);
    logLine("ngcwheel: row %d y=%.0f -> wheeling %s", row, rowY, g_ngcWhlDir < 0 ? "down" : "up");
}

static void ngcWheelService(uintptr_t disp) {
    if (!g_ngcWhlActive) return;
    if (g_ngcWhlRow < 0 || g_ngcWhlRow >= ngcCount(disp)) { g_ngcWhlActive = false; return; }
    float ry = 0;
    if (!ngcRowY(disp, g_ngcWhlRow, &ry)) {
        if (++g_ngcWhlStall >= NGC_WHEEL_STALL_PUMPS) { g_ngcWhlActive = false; }
        return;
    }
    if (ry >= NGC_ROW_VIS_TOP && ry <= NGC_CLICK_MAX_Y) {
        g_ngcWhlActive = false;
        ngcHoverItem(disp, g_ngcWhlRow);           // arrived -- park the game's hover on it
        logLine("ngcwheel: row %d in view at y=%.0f -- hover parked", g_ngcWhlRow, ry);
        return;
    }
    bool moved = (ry - g_ngcWhlLastY > 0.5f) || (g_ngcWhlLastY - ry > 0.5f);
    if (moved) {
        bool wrongWay = (g_ngcWhlDir < 0) ? (ry > g_ngcWhlLastY) : (ry < g_ngcWhlLastY);
        if (wrongWay) {
            g_ngcWhlDir = -g_ngcWhlDir;
            logLine("ngcwheel: row moved the wrong way -- flipping wheel sign to %+d", g_ngcWhlDir);
        }
        g_ngcWhlStall = 0;
    } else {
        g_ngcWhlStall++;
    }
    g_ngcWhlLastY = ry;
    if (g_ngcWhlStall >= NGC_WHEEL_STALL_PUMPS ||
        (int32_t)(GetTickCount() - g_ngcWhlDeadline) >= 0) {
        g_ngcWhlActive = false;
        ngcHoverItem(disp, g_ngcWhlRow);           // park anyway; Enter's clip-borrow covers it
        logLine("ngcwheel: gave up on row %d at y=%.0f (%s) -- the panel did not respond to the "
                "wheel; the clip-borrow covers the click", g_ngcWhlRow, ry,
                g_ngcWhlStall >= NGC_WHEEL_STALL_PUMPS ? "stalled" : "deadline");
        return;
    }
    enqueueSynthWheel(g_ngcWhlDir);                // one step per pump until it lands
}
static int64_t ngcCheckboxId(uintptr_t disp, int i) {
    uintptr_t ent = ngcEntry(disp, i);
    int64_t id = 0;
    if (ent && safeReadI64(ent + NGC_ENT_CBID_OFF, &id) && id) return id;
    return (int64_t)(NGC_AGC_BASE + (uint32_t)i);       // the id family is sequential (fallback)
}
static bool ngcChecked(uintptr_t disp, int i) {
    uintptr_t ent = ngcEntry(disp, i);
    uint8_t c = 0;
    return ent && safeReadU8(ent + NGC_ENT_ENABLED_OFF, &c) && c != 0;
}
static void ngcName(uintptr_t base, uintptr_t disp, int i, char* out, int outsz) {
    out[0] = 0;
    uintptr_t ent = ngcEntry(disp, i);
    char dlc[96]; dlc[0] = 0;
    if (ent) safeReadCStr(ent + NGC_ENT_DLCID_OFF, dlc, sizeof dlc);
    if (!dlc[0]) { _snprintf(out, outsz, axs(AXS_OPTION_N), i + 1); out[outsz - 1] = 0; return; }
    char key[128];
    _snprintf(key, sizeof key, "str_dlc_title_%s", dlc); key[sizeof key - 1] = 0;
    if (resolveKey(base, key, out, outsz) && out[0]) return;
    int n = 0;                                          // de-slug: "crimson_court" -> "Crimson court"
    for (const char* p = dlc; *p && n < outsz - 1; p++, n++) out[n] = (*p == '_') ? ' ' : *p;
    out[n] = 0;
    if (out[0] >= 'a' && out[0] <= 'z') out[0] = (char)(out[0] - 32);
}
static bool ngcPanelDrawn(uintptr_t disp) {
    int n = disp ? ngcCount(disp) : 0;
    for (int i = 0; i < n; i++)
        if (feGetElementById(ngcCheckboxId(disp, i))) return true;
    for (uint32_t i = 0; i < (uint32_t)SLOT_CAMPAIGN_COUNT; i++)   // no readable entry vector
        if (feGetElementById((int64_t)(NGC_AGC_BASE + i))) return true;
    return false;
}
static void ngcSpeakItem(uintptr_t base, uintptr_t disp, int cursor) {
    char utter[256];
    if (cursor >= ngcCount(disp)) {
        _snprintf(utter, sizeof utter, "%s", axs(AXS_NGC_OK_ROW));
    } else {
        char name[176]; ngcName(base, disp, cursor, name, sizeof name);
        _snprintf(utter, sizeof utter, axs(AXS_NGC_ITEM_STATE), name,
                  axs(ngcChecked(disp, cursor) ? AXS_VALUE_CHECKED : AXS_VALUE_UNCHECKED));
    }
    utter[sizeof utter - 1] = 0;
    postSpeech(utter);
}
static void ngcHoverItem(uintptr_t disp, int cursor) {
    int n = ngcCount(disp);
    int64_t id = (cursor >= n) ? (int64_t)NGC_OK_ID : ngcCheckboxId(disp, cursor);
    uintptr_t elem = feGetElementById(id);
    if (!elem) return;
    float x = 0, y = 0;
    if (!elemPos(elem, &x, &y)) return;
    uint32_t wb = 0, hb = 0;
    safeReadU32(elem + ELEM_SIZE_OFF, &wb);
    safeReadU32(elem + ELEM_SIZE_OFF + 4, &hb);
    float w = u32AsFloatM(wb), h = u32AsFloatM(hb);
    if (h < 1.0f || h > 4000.0f) h = 40.0f;
    float cx = (cursor >= n) ? x + (w > 1.0f && w < 4000.0f ? w : h) * 0.5f   // OK: row centre
                             : x + h * 0.5f;                                    // checkbox: left box
    float cy = y + h * 0.5f;
    moveCursorTo(cx, cy);
    logLine("ngc hover: cursor %d id=0x%llx -> (%.0f,%.0f)", cursor, (unsigned long long)id, cx, cy);
}

static void ngcDump(uintptr_t base, uintptr_t disp) {
    if (!axDebugLogEnabled()) return;                // diagnostic: nothing runs with the log off
    int n = ngcCount(disp);
    logLine("newgame-content DUMP: %d options, okId=0x%x", n, NGC_OK_ID);
    for (int i = 0; i < n && i < 16; i++) {
        char dlc[96]; dlc[0] = 0;
        uintptr_t ent = ngcEntry(disp, i);
        if (ent) safeReadCStr(ent + NGC_ENT_DLCID_OFF, dlc, sizeof dlc);
        char name[176]; ngcName(base, disp, i, name, sizeof name);
        float rx = 0, ry = 0;
        uintptr_t elem = feGetElementById(ngcCheckboxId(disp, i));
        bool haveY = (elem != 0 && elemPos(elem, &rx, &ry));
        logLine("  ngc[%d] cbId=0x%llx dlc=\"%s\" checked=%d y=%.0f%s name=\"%s\"",
                i, (unsigned long long)ngcCheckboxId(disp, i), dlc, (int)ngcChecked(disp, i),
                haveY ? ry : -1.0f,
                (haveY && ry > NGC_CLICK_MAX_Y) ? " <-- beyond the click clip" : "", name);
    }
    {
        uintptr_t begin = 0; long count = focusVectorCount(base);
        safeReadPtr(base + VEC_BEGIN_RVA, &begin);
        logLine("newgame-content: focus vector while the panel is up (%ld elements):", count);
        for (long i = 0; i < count && i < 48 && begin; i++) {
            uintptr_t e = begin + (uintptr_t)i * ELEM_STRIDE;
            int64_t id = 0; safeReadI64(e + ELEM_ID_OFF, &id);
            float x = 0, y = 0; elemPos(e, &x, &y);
            uint32_t wb = 0, hb = 0;
            safeReadU32(e + ELEM_SIZE_OFF, &wb); safeReadU32(e + ELEM_SIZE_OFF + 4, &hb);
            uint8_t f = 0, s = 0;
            safeReadU8(e + ELEM_FOCUSABLE_OFF, &f); safeReadU8(e + ELEM_SKIP_OFF, &s);
            char tag[9]; idToAscii(id, tag);
            logLine("  fe[%2ld] id=0x%llx \"%s\" pos=(%.0f,%.0f) size=(%.0fx%.0f) focusable=%u skip=%u",
                    i, (unsigned long long)id, tag, x, y, u32AsFloatM(wb), u32AsFloatM(hb), f, s);
        }
    }
}

// ---- PANEL SCROLL PROBE ----
static const uintptr_t NGC_PRB_LO = 0x2068, NGC_PRB_HI = 0x2600;
static uint32_t g_ngcPrbSnap[(NGC_PRB_HI - NGC_PRB_LO) / 4];
static bool  g_ngcPrbHave = false;   // snapshot valid (reset at panel open/close)
static DWORD g_ngcPrbNext = 0;       // next pass (throttle)
static DWORD g_ngcPrbBeat = 0;       // next heartbeat line
static float g_ngcPrbY0 = -1.0f;

static void ngcScrollProbe(uintptr_t base, uintptr_t disp) {
    if (!axDebugLogEnabled()) return;
    DWORD now = GetTickCount();
    if ((int32_t)(now - g_ngcPrbNext) < 0) return;
    g_ngcPrbNext = now + 250;

    int n = ngcCount(disp);
    float x = 0, y0 = -1.0f, yN = -1.0f;
    uintptr_t e0 = feGetElementById(ngcCheckboxId(disp, 0));
    uintptr_t eN = (n > 1) ? feGetElementById(ngcCheckboxId(disp, n - 1)) : 0;
    if (e0) elemPos(e0, &x, &y0);
    if (eN) elemPos(eN, &x, &yN);

    // Change-only sweep. The first pass only snapshots; later passes collect diffs.
    char diffs[640]; int dn = 0, shown = 0, changedWords = 0;
    diffs[0] = 0;
    for (uintptr_t off = NGC_PRB_LO; off < NGC_PRB_HI; off += 4) {
        uint32_t v = 0;
        if (!safeReadU32(disp + off, &v)) continue;
        uint32_t idx = (uint32_t)((off - NGC_PRB_LO) / 4);
        if (g_ngcPrbHave && g_ngcPrbSnap[idx] != v) {
            changedWords++;
            if (shown < 14) {
                int m = _snprintf(diffs + dn, (int)(sizeof diffs) - dn, " +0x%x:%08x->%08x(%.1f)",
                                  (unsigned int)off, g_ngcPrbSnap[idx], v, u32AsFloatM(v));
                if (m > 0) dn += m; else break;
                shown++;
            }
        }
        g_ngcPrbSnap[idx] = v;
    }
    diffs[sizeof diffs - 1] = 0;

    bool moved = g_ngcPrbHave && y0 >= 0 && g_ngcPrbY0 >= 0 &&
                 (y0 - g_ngcPrbY0 > 0.5f || g_ngcPrbY0 - y0 > 0.5f);
    bool first = !g_ngcPrbHave;
    g_ngcPrbHave = true;
    g_ngcPrbY0 = y0;

    if (first || moved || (int32_t)(now - g_ngcPrbBeat) >= 0) {
        g_ngcPrbBeat = now + 1000;
        logLine("ngcprobe:%s row0=%.0f rowN=%.0f (%d words changed)%s%s",
                first ? " BASELINE" : moved ? " ROWS MOVED" : "", y0, yN, changedWords,
                (moved && diffs[0]) ? " --" : "", moved ? diffs : "");
    }
}

// ---- THE LETTER/SEAL WAIT THAT WASN'T ----

void checkNewGameContent(uintptr_t base) {
    uintptr_t disp = saveSlotScreen(base);
    if (!disp) {
        if (g_ngcActive) { g_ngcActive = false; g_ngcSlotIdx = -1; g_ngcTogIdx = -1; logLine("newgame-content: closed (off screen)"); }
        g_ngcSwapIdx = -1; g_ngcSwapAnchor = -1; g_ngcWhlActive = false;
        g_ngcViewMode = false; g_ngcViewArm = 0;
        g_ngcArmedTick = 0;
        return;
    }
    if (g_ngcArmedTick && GetTickCount() - g_ngcArmedTick >= NGC_SETTLE_MS) {
        g_ngcArmedTick = 0;
        if (!g_ngcActive && !confirmDialogOpen(base) && ngcPanelDrawn(disp)) {
            uint32_t idx = 0; safeReadU32(disp + FE_NEWGAME_SLOTIDX_OFF, &idx);
            g_ngcSlotIdx = (idx < (uint32_t)SLOT_CAMPAIGN_COUNT) ? (int)idx : 0;
            g_ngcViewMode = (g_ngcViewArm != 0); g_ngcViewArm = 0;
            g_ngcActive = true; g_ngcCursor = 0; g_ngcTogIdx = -1;
            g_ngcSwapIdx = -1; g_ngcSwapAnchor = -1; g_ngcWhlActive = false;
            g_ngcPrbHave = false;
            logLine("newgame-content: content panel up, slot idx %d%s (sub=%d)", g_ngcSlotIdx,
                    g_ngcViewMode ? " [view/enable]" : "", feNamingSub(disp));
            ngcDump(base, disp);
            ngcHoverItem(disp, 0);                     // park the game's hover on the first checkbox
            int nc = ngcCount(disp);
            char name[176]; ngcName(base, disp, 0, name, sizeof name);
            char row0[224], intro[448];
            _snprintf(row0, sizeof row0, axs(AXS_NGC_ITEM_STATE), name,
                      axs(ngcChecked(disp, 0) ? AXS_VALUE_CHECKED : AXS_VALUE_UNCHECKED));
            row0[sizeof row0 - 1] = 0;
            char head[288];
            _snprintf(head, sizeof head,
                      axs(g_ngcViewMode ? AXS_DLCVIEW_INTRO_N : AXS_NGC_INTRO_N), nc);
            head[sizeof head - 1] = 0;
            _snprintf(intro, sizeof intro, "%s %s", head, row0);
            intro[sizeof intro - 1] = 0;
            postSpeech(intro);
        }
    }
    if (!g_ngcActive) return;
    if (!g_ngcViewMode) {
        int sub = feNamingSub(disp);
        if (sub == (int)FE_NAMING_SUB_MODEDIALOG || sub == FE_NAMING_SUB_NAMING) {
            g_ngcActive = false; g_ngcSlotIdx = -1; g_ngcTogIdx = -1;
            g_ngcSwapIdx = -1; g_ngcSwapAnchor = -1;   // markers only -- the entries are the panel's
            g_ngcWhlActive = false;
            logLine("newgame-content: flow advanced past the panel (sub=%d) -> standing down", sub);
            return;
        }
    }
    uint32_t ngcFlag = 0;
    bool haveFlag = safeReadU32(disp + FE_CONTENT_FLAG_OFF, &ngcFlag) != 0;
    bool stillUp  = haveFlag ? (ngcFlag == 1)
                             : (confirmDialogOpen(base) || ngcPanelDrawn(disp));
    if (!stillUp) {

        bool wasView = g_ngcViewMode;
        int  slotNum = g_ngcSlotIdx + 1;               // array idx 0..8 -> spoken slot 1..9
        g_ngcActive = false; g_ngcViewMode = false; g_ngcSlotIdx = -1; g_ngcTogIdx = -1;
        g_ngcSwapIdx = -1; g_ngcSwapAnchor = -1;
        g_ngcWhlActive = false;
        logLine("newgame-content: panel closed%s (flag=%s, drawn=%d, dialog=%d)",
                wasView ? " (view)" : "", haveFlag ? (ngcFlag == 1 ? "1" : "0") : "unreadable",
                ngcPanelDrawn(disp) ? 1 : 0, confirmDialogOpen(base) ? 1 : 0);
        if (wasView && slotNum >= 1 && slotNum <= SLOT_CAMPAIGN_COUNT) {
            armSlotReturnWatch(slotNum);
            g_srwSeenBusy = true;
        }
        return;
    }
    if (g_ngcTogIdx >= 0 && !confirmDialogOpen(base) &&
        GetTickCount() - g_ngcTogTick >= NGC_TOGGLE_SETTLE_MS) {
        int idx = g_ngcTogIdx; g_ngcTogIdx = -1;
        if (g_ngcSwapIdx >= 0) {
            bool restored = ngcSwapEntries(disp, g_ngcSwapIdx, g_ngcSwapAnchor);
            logLine("newgame-content: clip-borrow restore %d<->%d -> %s",
                    g_ngcSwapIdx, g_ngcSwapAnchor, restored ? "ok" : "FAILED");
            g_ngcSwapIdx = -1; g_ngcSwapAnchor = -1;
        }
        if (idx < ngcCount(disp)) {
            uintptr_t ent = ngcEntry(disp, idx);
            uint8_t a228 = 0, a305 = 0, a306 = 0;
            if (ent) { safeReadU8(ent + NGC_ENT_ENABLED_OFF, &a228);
                       safeReadU8(ent + 0x305, &a305);
                       safeReadU8(ent + NGC_ENT_CHECKED_OFF, &a306); }
            logLine("newgame-content: toggle %d read-back  +0x228 %u->%u  +0x305 %u->%u  +0x306 %u->%u",
                    idx, g_ngcTogB228, a228, g_ngcTogB305, a305, g_ngcTogB306, a306);
            ngcHoverItem(disp, g_ngcCursor);
            char name[176]; ngcName(base, disp, idx, name, sizeof name);
            char utter[256];
            _snprintf(utter, sizeof utter, axs(AXS_NGC_ITEM_STATE), name,
                      axs(ngcChecked(disp, idx) ? AXS_VALUE_CHECKED : AXS_VALUE_UNCHECKED));
            utter[sizeof utter - 1] = 0;
            postSpeech(utter);
        }
    }
    // The wheel servo: one step per pump toward the row the cursor is waiting on.
    ngcWheelService(disp);
    ngcScrollProbe(base, disp);
}

bool routeFrontEndKey(uintptr_t base, uint32_t sym, uint16_t mod, uint8_t repeat) {
    (void)base;
    if (mod & (KMOD_LALT | KMOD_RALT)) return false;   // leave Alt+key to the game

    if (g_ngcActive && !confirmDialogOpen(g_base)) {
        int jump = 0;
        if (axDecodeJump(sym, mod, repeat, &jump)) {
            if (!jump) return true;                    // held jump: one landing per press
            uintptr_t disp = saveSlotScreen(g_base);
            if (!disp) return true;
            int n = ngcCount(disp);
            int last = g_ngcViewMode ? (n > 0 ? n - 1 : 0) : n;   // new game: n = the OK row
            int nc = (jump > 0) ? last : 0;
            g_ngcCursor = nc;
            float ry = 0;
            if (nc < n && ngcRowY(disp, nc, &ry) &&
                (ry > NGC_CLICK_MAX_Y || ry < NGC_ROW_VIS_TOP)) {
                ngcWheelStart(nc, ry);
            } else {
                g_ngcWhlActive = false;                // an in-band stop cancels any servo
                ngcHoverItem(disp, g_ngcCursor);
            }
            ngcSpeakItem(g_base, disp, g_ngcCursor);
            return true;
        }
    }

    if (g_ngcActive && !confirmDialogOpen(g_base) &&
        (sym == SDLK_UP || sym == SDLK_DOWN || sym == SDLK_RETURN || sym == SDLK_KP_ENTER)) {
        uintptr_t disp = saveSlotScreen(g_base);
        if (disp) {
            int n = ngcCount(disp);
            if (sym == SDLK_UP || sym == SDLK_DOWN) {
                if (axNavHoldRepeat(repeat)) return true;
                int nc = g_ngcCursor + (sym == SDLK_DOWN ? 1 : -1);
                int last = g_ngcViewMode ? (n > 0 ? n - 1 : 0) : n;   // view: no OK row
                if (nc < 0) nc = 0;
                if (nc > last) nc = last;              // new game: n = the OK row, the last stop
                g_ngcCursor = nc;
                float ry = 0;
                if (nc < n && ngcRowY(disp, nc, &ry) &&
                    (ry > NGC_CLICK_MAX_Y || ry < NGC_ROW_VIS_TOP)) {
                    ngcWheelStart(nc, ry);
                } else {
                    g_ngcWhlActive = false;            // an in-band stop cancels any servo
                    ngcHoverItem(disp, g_ngcCursor);   // move the game's hover onto it
                }
                ngcSpeakItem(g_base, disp, g_ngcCursor);
                return true;
            }
            if (repeat) return true;                   // Enter: one action per press
            if (!g_ngcViewMode && g_ngcCursor >= n) {  // OK: confirm the content, advance to difficulty
                bool ok = frontEndClickElementId((int64_t)NGC_OK_ID);
                logLine("newgame-content: OK -> %s", ok ? "clicked" : "NO OK ELEMENT");
                if (!ok) postSpeech(axs(AXS_NGC_NO_OK));
            } else if (g_ngcCursor < n) {              // a checkbox: toggle it by clicking
                if (g_ngcTogIdx >= 0 || clickQueued()) {
                    logLine("newgame-content: toggle refused, one still settling (tog=%d)", g_ngcTogIdx);
                    return true;
                }
                g_ngcWhlActive = false;                // Enter overrides a scroll still in flight
                uintptr_t ent = ngcEntry(disp, g_ngcCursor);   // snapshot state bytes for the read-back diff
                g_ngcTogB228 = g_ngcTogB305 = g_ngcTogB306 = 0;
                if (ent) { safeReadU8(ent + NGC_ENT_ENABLED_OFF, &g_ngcTogB228);
                           safeReadU8(ent + 0x305, &g_ngcTogB305);
                           safeReadU8(ent + NGC_ENT_CHECKED_OFF, &g_ngcTogB306); }
                float rowY = 0, bx = 0, by = 0;
                bool haveY   = ngcRowY(disp, g_ngcCursor, &rowY);
                bool outside = haveY && (rowY > NGC_CLICK_MAX_Y || rowY < NGC_ROW_VIS_TOP);
                if (outside) {
                    int anchor = ngcAnchorRow(disp, n);
                    if (anchor >= 0 && anchor != g_ngcCursor && ngcBoxPoint(disp, anchor, &bx, &by) &&
                        ngcSwapEntries(disp, g_ngcCursor, anchor)) {
                        g_ngcSwapIdx = g_ngcCursor; g_ngcSwapAnchor = anchor;
                        logLine("newgame-content: option %d outside the view (y=%.0f) -> "
                                "borrowing row %d at (%.0f,%.0f)", g_ngcCursor, rowY, anchor, bx, by);
                        ngcClickAt(bx, by);
                    } else if (ngcBoxPoint(disp, g_ngcCursor, &bx, &by)) {
                        logLine("newgame-content: clip borrow unavailable for option %d (anchor=%d)",
                                g_ngcCursor, anchor);
                        ngcClickAt(bx, by);
                    }
                } else if (ngcBoxPoint(disp, g_ngcCursor, &bx, &by)) {
                    ngcClickAt(bx, by);
                } else {
                    logLine("newgame-content: option %d has no live rect -- click refused", g_ngcCursor);
                }
                logLine("newgame-content: toggle option %d id=0x%llx (pre 228=%u 305=%u 306=%u)",
                        g_ngcCursor, (unsigned long long)ngcCheckboxId(disp, g_ngcCursor),
                        g_ngcTogB228, g_ngcTogB305, g_ngcTogB306);
                g_ngcTogIdx = g_ngcCursor; g_ngcTogTick = GetTickCount();
            }
            return true;
        }
    }

    if (sym == SDLK_DELETE) {
        if (repeat) return true;                       // one prompt per physical press
        saveSlotDeleteGateCheck(g_base);               // one-time: is Delete the game's key?
        SlotActionResult r = saveSlotAction(g_base, true);
        if (r == SLOT_ACTION_REFUSED) postSpeech(axs(AXS_SLOT_EMPTY_NO_DELETE));
        return r != SLOT_ACTION_NA;                    // only ours on a slot; else the game's
    }

    if ((sym == SDLK_RETURN || sym == SDLK_KP_ENTER) && (mod & (KMOD_LSHIFT | KMOD_RSHIFT))) {
        if (repeat) return true;
        SlotActionResult r = slotDlcAction(g_base);
        if (r == SLOT_ACTION_REFUSED) postSpeech(axs(AXS_DLC_BTN_NONE));
        if (r != SLOT_ACTION_NA) return true;
    }

    if (sym == SDLK_RETURN || sym == SDLK_KP_ENTER) {
        if (repeat) return false;
        if (!confirmDialogOpen(g_base) && optRowToggle()) return true;
        if (saveSlotAction(g_base, false) == SLOT_ACTION_DONE) return true;
        frontEndClickCursor();                         // click whatever the cursor is on
        return false;
    }

    if ((sym == SDLK_UP || sym == SDLK_DOWN) && (mod & (KMOD_LCTRL | KMOD_RCTRL)) &&
        !confirmDialogOpen(g_base) && optPageActive()) {
        if (repeat) return true;                       // one line per physical press
        if (optTipStep(sym == SDLK_UP)) return true;
    }

    if ((sym == SDLK_LEFT || sym == SDLK_RIGHT) &&
        !confirmDialogOpen(g_base) && optPageActive()) {
        if (axNavHoldRepeat(repeat)) return true;      // throttled repeat: claimed, no step
        optAdjustRow(sym == SDLK_RIGHT);
        return true;
    }

    {
        int jump = 0;
        if (!confirmDialogOpen(g_base) && axDecodeJump(sym, mod, repeat, &jump)) {
            if (!jump) return true;                    // held jump: one landing per press
            int jdir = (jump > 0) ? 1 : 0;             // the vertical axis: Down-most / Up-most
            if (saveSlotScreen(g_base) && !g_ngcActive) {
                int64_t id = (int64_t)(SLOT_FOURCC_BASE +
                                       (uint32_t)(jump > 0 ? SLOT_COUNT - 1 : 0));
                uintptr_t elem = feGetElementById(id);
                if (elem) {
                    FePick p = { elem, id, g_feFocusId, !isNoFocus(g_feFocusId), false };
                    feCommitTo(jdir, p, false);
                    feLandingCancel("focus moved");
                    g_feScreenKey = ((int)currentAxContext() << 8) | (FE_STATE_SAVESLOTS & 0xff);
                    logLine("frontend jump %s: direct to slot end 0x%llx",
                            jump > 0 ? "end" : "home", (unsigned long long)id);
                    return true;
                }
                logLine("frontend jump: slot end 0x%llx missing -- falling back to the walk",
                        (unsigned long long)id);
            }
            int steps = 0;
            while (steps < 32 && feFocusStep(jdir, false, false)) steps++;
            logLine("frontend jump %s: %d step(s)", jump > 0 ? "end" : "home", steps);
            return true;
        }
    }

    int dir;
    switch (sym) {
        case SDLK_UP:    dir = 0; break;
        case SDLK_DOWN:  dir = 1; break;
        case SDLK_LEFT:  dir = 2; break;
        case SDLK_RIGHT: dir = 3; break;
        default:         return false;   // not an arrow -> let the game have it
    }
    if (axNavHoldRepeat(repeat)) return true;           // throttled repeat: claimed, no step
    frontEndFocusMove(dir);
    return true;
}
