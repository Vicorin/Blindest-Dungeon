// THE BUTCHER'S CIRCUS -- THE RING, "Choose Your Contestants".

#include <cstdint>
#include <cstring>
#include <cstdio>
#include "internal.h"

// ---- the panel, and the two vectors on it ----
static const uintptr_t RING_PANEL_OFF      = 0x4e08;  // townRoot+: PartySetupDisplay, INLINE
                                                      // (= aggregate+0x3660, aggregate = +0x17a8)
static const int       RING_LAYER          = 2;       // TM_LAYER_OFF's value for The Ring
static const uintptr_t RING_LIST_BEG_OFF   = 0x78;    // panel+: contestant widget vector begin
static const uintptr_t RING_LIST_END_OFF   = 0x80;    // panel+: ... end
static const uintptr_t RING_HELD_OFF       = 0x728;   // panel+: the picked-up row's index; "none"
                                                      // is index == count, not -1 and not 0
static const uintptr_t RING_BUSY_OFF       = 0x280;   // panel+: int, every handler wants 0
static const uintptr_t RING_LOCKED_OFF     = 0x730;   // panel+: byte, the lineup is locked
static const uintptr_t RING_IFACE_DLC_OFF  = 0x238;   // row iface+: byte, needs a DLC you lack
static const uintptr_t RING_IFACE_IN_OFF   = 0x239;   // row iface+: byte, in the lineup
static const uintptr_t RING_ROSTER_OFF     = 0x728;   // Campaign+: the CIRCUS Roster::System
static const int       RING_MAX_ROWS       = 32;      // the class roster is ~17; more = a bad read
static const uint32_t  RING_ROW_ELEM_BASE  = 0x6e7073; // contestant row i ('spn'+i, owner 'spna')
static const uintptr_t RING_MM_PANEL_OFF   = 0x4d18;  // townRoot+: MatchmakingDisplay, INLINE
static const int       RING_MM_LAYER       = 3;       // TM_LAYER_OFF's value for matchmaking
static const uintptr_t RING_MM_STATE_OFF   = 0xd8;    // panel+: byte, str_matchmaking_state_%d

static const char* RING_TITLE_KEY = "str_party_setup_heroes_title";

struct RingRow {
    uintptr_t widget;   // the contestant's list widget
    uintptr_t iface;    // widget+0x158
    uintptr_t hero;
    bool      inLineup; // iface+0x239
    bool      dlcLocked;
};
static RingRow g_ringRows[RING_MAX_ROWS];
static int     g_ringRowCount = 0;      // cells walked (including empty ones)

static bool g_ringActive   = false;     // AX_RING: the slots own the keys
static bool g_ringListOpen = false;     // AX_RINGLIST: the contestant list is up over them
static int  g_ringSlot     = 0;
static int  g_ringRow      = 0;         // cursor over the contestant list
static int  g_ringForSlot  = 0;         // the slot the list was opened for
static uintptr_t g_ringHoldHero = 0;    // Space pickup: the hero riding the cursor
static char      g_ringHoldName[80];
static DWORD     g_ringSheetWatchUntil = 0;
static int       g_ringSheetFrom = 0;   // 1 = a slot opened it, 2 = a list row did
static bool      g_ringDumped  = false;
static bool      g_mmActive    = false; // the matchmaking screen (layer 3) is up
static int       g_mmState     = -1;    // the last state byte spoken, -1 = nothing yet
static bool      g_mmLoading   = false;

// ---- the toolbar: Leagues and Practice Battle ----
static const uint32_t  RING_TB_LEAGUES_ELEM  = 0x726e6b64; // 'dknr' (prints as "rnkd" in the dumps)
static const uint32_t  RING_TB_PRACTICE_ELEM = 0x63627474; // 'ttbc'
static const uintptr_t RING_RANKDISP_OFF       = 0x47e8;
static const uintptr_t RING_RANKDISP_SHOWN_OFF = 0x58;   // panel+: tween int, 0 hidden, 1 appearing,
                                                         // 4 fully shown; "open" = != 0
static const uintptr_t RING_RANKDISP_BLD_OFF   = 0x80;   // panel+: the `rankings` Building*
static const uintptr_t RING_LEAGUES_BEG_OFF    = 0x200;  // building+: vector<League> begin
static const uintptr_t RING_LEAGUES_END_OFF    = 0x208;  // building+: ... end
static const uintptr_t RING_LEAGUE_STRIDE      = 0x28;   // +0x00 id char[0x20] (-> str_league_<id>),
static const uintptr_t RING_LEAGUE_TIERS_OFF   = 0x20;   // +0x20 its tier count, +0x24 a colour id
static const int       RING_LEAGUES_MAX        = 8;      // 5 shipped; more = a bad read
static const char*     RING_LEAGUES_TITLE_KEY  = "str_rank_display_title";   // what the popup draws
static const DWORD     RING_TB_WATCH_MS        = 1500;

static int   g_ringTbWatch      = 0;
                                         // 3 Leagues closing (L on the open popup)
static DWORD g_ringTbUntil      = 0;
static bool  g_ringLeaguesOpen  = false;
static bool  g_ringTbGateLogged = false;
static void  ringServiceToolbar(uintptr_t base);

bool axIsRing()     { return g_ringActive && !g_ringListOpen; }
bool axIsRingList() { return g_ringActive &&  g_ringListOpen; }

// ---- reading the screen ----

uintptr_t ringPanel(uintptr_t base) {
    uintptr_t root = resTownRoot(base);
    if (!root) return 0;
    uint32_t layer = 0xffffffffu;
    if (!safeReadU32(root + TM_LAYER_OFF, &layer) || (int)layer != RING_LAYER) return 0;
    uintptr_t panel = root + RING_PANEL_OFF, vft = 0;
    if (!safeReadPtr(panel, &vft) || vft <= base) return 0;
    if (vft - base != RING_PANEL_VFT_RVA) {
        static bool moaned = false;
        if (!moaned) {
            moaned = true;
            logLine("ring: layer 2 but townRoot+0x%llx has vftable 0x%llx, not the "
                    "PartySetupDisplay's 0x%llx -- the panel offset is wrong for this build",
                    (unsigned long long)RING_PANEL_OFF,
                    (unsigned long long)(vft - base), (unsigned long long)RING_PANEL_VFT_RVA);
        }
        return 0;
    }
    return panel;
}

static uintptr_t ringMmPanel(uintptr_t base) {
    uintptr_t root = resTownRoot(base);
    if (!root) return 0;
    uint32_t layer = 0xffffffffu;
    if (!safeReadU32(root + TM_LAYER_OFF, &layer) || (int)layer != RING_MM_LAYER) return 0;
    uintptr_t panel = root + RING_MM_PANEL_OFF, vft = 0;
    if (!safeReadPtr(panel, &vft) || vft <= base) return 0;
    if (vft - base != RING_MM_VFT_RVA) {
        static bool moaned = false;
        if (!moaned) {
            moaned = true;
            logLine("ring: layer 3 but townRoot+0x%llx has vftable 0x%llx, not the "
                    "MatchmakingDisplay's 0x%llx",
                    (unsigned long long)RING_MM_PANEL_OFF,
                    (unsigned long long)(vft - base), (unsigned long long)RING_MM_VFT_RVA);
        }
        return 0;
    }
    return panel;
}

bool circusScreenUp(uintptr_t base) { return ringPanel(base) != 0 || ringMmPanel(base) != 0; }

static uintptr_t ringRosterSystem(uintptr_t base) {
    uintptr_t campaign = 0;
    uint8_t circus = 0;
    if (!safeReadPtr(base + RES_CAMPAIGN_RVA, &campaign) || campaign <= 0x10000) return 0;
    safeReadU8(base + RI_CIRCUS_FLAG_RVA, &circus);
    return circus ? campaign + RING_ROSTER_OFF : campaign;
}

static uintptr_t ringEntryOfHero(uintptr_t base, uintptr_t hero) {
    if (!hero) return 0;
    uintptr_t sys = ringRosterSystem(base), beg = 0, end = 0;
    if (!sys) return 0;
    if (!safeReadPtr(sys + PTY_ENTRIES_BEG_OFF, &beg) ||
        !safeReadPtr(sys + PTY_ENTRIES_END_OFF, &end) || !beg || end <= beg) return 0;
    int n = (int)((end - beg) / 8);
    if (n < 0 || n > 256) return 0;
    for (int i = 0; i < n; i++) {
        uintptr_t e = 0;
        if (!safeReadPtr(beg + (uintptr_t)i * 8, &e) || e <= 0x10000) continue;
        if (e + PTY_ENTRY_HERO_OFF == hero) return e;
    }
    return 0;
}

static int ringCollect(uintptr_t base, bool probe) {
    g_ringRowCount = 0;
    uintptr_t panel = ringPanel(base);
    if (!panel) return 0;
    uintptr_t beg = 0, end = 0;
    if (!safeReadPtr(panel + RING_LIST_BEG_OFF, &beg) ||
        !safeReadPtr(panel + RING_LIST_END_OFF, &end) || !beg || end <= beg || (end - beg) % 8)
        return 0;
    int n = (int)((end - beg) / 8);
    if (n <= 0 || n > RING_MAX_ROWS) {
        logLine("ring: contestant vector holds %d cells -- refusing to walk it", n);
        return 0;
    }
    for (int i = 0; i < n; i++) {
        RingRow* r = &g_ringRows[i];
        uint8_t inl = 0, dlc = 0;
        r->widget = r->iface = r->hero = 0;
        r->inLineup = r->dlcLocked = false;
        if (!safeReadPtr(beg + (uintptr_t)i * 8, &r->widget) || r->widget <= 0x10000) continue;
        if (!safeReadPtr(r->widget + PTY_SLOT_IFACE_OFF, &r->iface) || r->iface <= 0x10000) {
            r->widget = 0;
            continue;
        }
        safeReadPtr(r->iface + PTY_IFACE_HERO_OFF, &r->hero);
        if (safeReadU8(r->iface + RING_IFACE_IN_OFF,  &inl)) r->inLineup  = inl != 0;
        if (safeReadU8(r->iface + RING_IFACE_DLC_OFF, &dlc)) r->dlcLocked = dlc != 0;
    }
    g_ringRowCount = n;
    if (probe) {
        uint32_t held = 0, busy = 0;
        uint8_t  locked = 0;
        safeReadU32(panel + RING_HELD_OFF, &held);
        safeReadU32(panel + RING_BUSY_OFF, &busy);
        safeReadU8(panel + RING_LOCKED_OFF, &locked);
        logLine("ring probe: panel=%p rows=%d held=%u(none=%d) busy=%u locked=%u",
                (void*)panel, n, held, n, busy, locked);
        for (int i = 0; i < n; i++) {
            const RingRow* r = &g_ringRows[i];
            char name[80] = {0};
            if (r->hero) safeReadCStr(r->hero + HERO_NAME_OFF, name, sizeof name);
            logLine("ring probe: row %d widget=%p hero=%p \"%s\" inLineup=%d dlcLocked=%d",
                    i, (void*)r->widget, (void*)r->hero, name, r->inLineup ? 1 : 0,
                    r->dlcLocked ? 1 : 0);
        }
        uintptr_t iface[PTY_MAX_SLOTS], hero[PTY_MAX_SLOTS];
        int slots = ptySlotStrip(base, iface, hero, PTY_MAX_SLOTS);
        logLine("ring probe: slot strip = %d slots", slots);
        for (int i = 0; i < slots; i++)
            logLine("ring probe: slot %d iface=%p hero=%p", i, (void*)iface[i], (void*)hero[i]);
        feDumpFocusVector("The Ring, arrival frame");
    }
    return n;
}

static int ringRowOfHero(uintptr_t base, uintptr_t hero) {
    if (!hero) return -1;
    int n = ringCollect(base, false);          // always a fresh read: the panel rebuilds the
    for (int i = 0; i < n; i++)
        if (g_ringRows[i].hero == hero) return i;  // move heroes between two calls of this
    return -1;
}

// ---- the four slots ----
static int ringRankOfSlot(int slot, int n) { return n - slot; }

static int ringLandingSlot(int n) { return n > 0 ? n - 1 : 0; }

static void ringSlotLine(uintptr_t base, uintptr_t hero, int rank, int total,
                         char* out, int outsz) {
    char rankln[96];
    _snprintf(rankln, sizeof rankln, axs(AXS_RNG_RANK_FMT), rank, total);
    rankln[sizeof rankln - 1] = 0;
    if (!hero) {
        _snprintf(out, outsz, "%s %s", axs(AXS_RNG_EMPTY_SLOT), rankln);
        out[outsz - 1] = 0;
        return;
    }
    ptyHeroFragBare(base, hero, out, outsz);
    char frag[160];
    _snprintf(frag, sizeof frag, ". %s", rankln);
    frag[sizeof frag - 1] = 0;
    strncat(out, frag, outsz - strlen(out) - 1);
}

static void ringSpeakSlot(uintptr_t base, const char* prefix) {
    uintptr_t iface[PTY_MAX_SLOTS], hero[PTY_MAX_SLOTS];
    int n = ptySlotStrip(base, iface, hero, PTY_MAX_SLOTS);
    if (n <= 0) { postSpeech(axs(AXS_RNG_NO_LINEUP)); return; }
    axStepCursor(&g_ringSlot, n, 0);
    char card[512], utter[MAILBOX_SZ];
    ringSlotLine(base, hero[g_ringSlot], ringRankOfSlot(g_ringSlot, n), n, card, sizeof card);
    _snprintf(utter, sizeof utter, "%s%s", prefix ? prefix : "", card);
    utter[sizeof utter - 1] = 0;
    postSpeech(utter);
}

static bool ringOpponentName(uintptr_t base, char* out, size_t n);

static const char* ringTitle(uintptr_t base, char* buf, int bufsz) {
    char title[160];
    if (resolveKey(base, RING_TITLE_KEY, title, sizeof title) && title[0])
        _snprintf(buf, bufsz, "%s. ", title);
    else
        _snprintf(buf, bufsz, "%s ", axs(AXS_RNG_TITLE_FALLBACK));
    buf[bufsz - 1] = 0;
    char who[80];
    if (ringOpponentName(base, who, sizeof who) && who[0]) {
        int used = (int)strlen(buf);
        if (used < bufsz - 1) {
            _snprintf(buf + used, bufsz - used, axs(AXS_RNG_FACING_FMT), who);
            buf[bufsz - 1] = 0;
            int end = (int)strlen(buf);
            if (end && end < bufsz - 1 && buf[end - 1] != ' ') { buf[end] = ' '; buf[end + 1] = 0; }
        }
    }
    return buf;
}

// ---- the contestant list ----

static void ringRowLine(uintptr_t base, const RingRow* r, int idx, int total,
                        char* out, int outsz) {
    if (!r->hero) {
        _snprintf(out, outsz, "%s", axs(AXS_RNG_EMPTY_ROW));
        out[outsz - 1] = 0;
        return;
    }
    ptyHeroFragBare(base, r->hero, out, outsz);
    char posln[96], tail[352];
    _snprintf(posln, sizeof posln, axs(AXS_RNG_ROW_N_OF_M), idx + 1, total);
    posln[sizeof posln - 1] = 0;
    _snprintf(tail, sizeof tail, ".%s%s%s%s %s",
              r->dlcLocked ? " " : "", r->dlcLocked ? axs(AXS_RNG_NEEDS_DLC) : "",
              r->inLineup  ? " " : "", r->inLineup  ? axs(AXS_RNG_IN_LINEUP)  : "",
              posln);
    tail[sizeof tail - 1] = 0;
    strncat(out, tail, outsz - strlen(out) - 1);
}

static void ringSpeakRow(uintptr_t base, const char* prefix) {
    int n = ringCollect(base, false);
    if (n <= 0) { postSpeech(axs(AXS_RNG_NO_CONTESTANTS)); return; }
    axStepCursor(&g_ringRow, n, 0);
    char card[512], utter[MAILBOX_SZ];
    ringRowLine(base, &g_ringRows[g_ringRow], g_ringRow, n, card, sizeof card);
    _snprintf(utter, sizeof utter, "%s%s", prefix ? prefix : "", card);
    utter[sizeof utter - 1] = 0;
    postSpeech(utter);
}

// ---- calling the game ----
typedef void (*RingRowFn)(uintptr_t panel, uintptr_t hero, uint32_t index);

static bool ringSehInspect(uintptr_t base, uintptr_t panel, uintptr_t hero, int index) {
    __try { ((RingRowFn)(base + RING_INSPECT_RVA))(panel, hero, (uint32_t)index); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        logLine("ring: the contestant inspect faulted (row %d)", index);
        return false;
    }
}

typedef char (*RingPutdownFn)(uintptr_t panel, char skipDrop, char skipTrinketClose);

static bool ringSehPutdown(uintptr_t base, uintptr_t panel) {
    __try { return ((RingPutdownFn)(base + RING_PUTDOWN_RVA))(panel, 0, 1) != 0; }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        logLine("ring: the put-down faulted");
        return false;
    }
}

static bool ringSehToggle(uintptr_t base, uintptr_t panel, uintptr_t hero, int index) {
    __try { ((RingRowFn)(base + RING_TOGGLE_RVA))(panel, hero, (uint32_t)index); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        logLine("ring: the contestant toggle faulted (row %d)", index);
        return false;
    }
}

typedef void (*UiSoundFn)(const char* event);

static void ringSehSound(uintptr_t base, const char* event) {
    __try { ((UiSoundFn)(base + UI_PLAY_SOUND_RVA))(event); }
    __except (EXCEPTION_EXECUTE_HANDLER) { logLine("ring: playing \"%s\" faulted", event); }
}

static int ringSlotOfHero(uintptr_t base, uintptr_t hero, int* countOut) {
    uintptr_t iface[PTY_MAX_SLOTS], hero_[PTY_MAX_SLOTS];
    int n = ptySlotStrip(base, iface, hero_, PTY_MAX_SLOTS);
    if (countOut) *countOut = n;
    if (!hero) return -1;
    for (int i = 0; i < n; i++) if (hero_[i] == hero) return i;
    return -1;
}

static bool ringPlaceAtSlot(uintptr_t base, uintptr_t hero, int want, const char* what) {
    uintptr_t iface[PTY_MAX_SLOTS], now[PTY_MAX_SLOTS], wantArr[PTY_MAX_SLOTS];
    int n = ptySlotStrip(base, iface, now, PTY_MAX_SLOTS);
    if (n <= 0 || want < 0 || want >= n) return false;
    int at = -1;
    for (int i = 0; i < n; i++) if (now[i] == hero) at = i;
    if (at < 0) return false;
    if (at == want) return true;
    for (int i = 0; i < n; i++) {
        if (!iface[i]) {
            logLine("ring: slot %d has no interface -> refusing to rearrange", i);
            return false;
        }
        wantArr[i] = now[i];
    }
    uintptr_t t = wantArr[want]; wantArr[want] = wantArr[at]; wantArr[at] = t;
    return ptyWriteArrangement(base, iface, wantArr, n, what);
}

static bool ringRepairAdd(uintptr_t base, uintptr_t hero, int want, const char* who) {
    uintptr_t panel = ringPanel(base);
    const int row = ringRowOfHero(base, hero);
    if (!panel || row < 0) {
        logLine("ring: no repair possible for the refused add (panel=%p row=%d)", (void*)panel, row);
        return false;
    }
    logLine("ring: the add drag was refused -> repairing with the game's own row toggle (row %d)", row);
    if (!ringSehToggle(base, panel, hero, row)) return false;

    int n = 0;
    const int at = ringSlotOfHero(base, hero, &n);
    if (at < 0) {
        logLine("ring: the toggle did not add %s either -- the game is refusing this contestant",
                who ? who : "the hero");
        return false;
    }
    ringSehSound(base, "/ui/town/character_add");
    if (at != want) {
        char what[160];
        _snprintf(what, sizeof what, "add repair %s to rank %d",
                  who ? who : "the hero", ringRankOfSlot(want, n));
        what[sizeof what - 1] = 0;
        ringPlaceAtSlot(base, hero, want, what);
    }
    return true;
}

// ---- MEMBERSHIP IS A DRAG ----
enum RingDragKind { RING_DRAG_NONE = 0, RING_DRAG_MEMBER, RING_DRAG_MOVE };
static int       g_ringDragKind  = RING_DRAG_NONE;
static uintptr_t g_ringDragHero  = 0;    // 0 = no drag outstanding
static bool      g_ringDragWantIn = false; // MEMBER: true = we dragged them IN, false = OUT
static int       g_ringDragSlot  = 0;
static int       g_ringDragFrom  = -1;   // MOVE: the rank they left
static int       g_ringDragTotal = 0;    // slot count at fire time, for the rank arithmetic
static char      g_ringDragName[80];
static DWORD     g_ringDragUntil = 0;
static int       g_ringDragHeldSeen = -1;
static uintptr_t g_ringDragWantSrc  = 0;

static void ringDumpAddGuards(uintptr_t base, uintptr_t hero) {
    uintptr_t iface[PTY_MAX_SLOTS], now[PTY_MAX_SLOTS];
    const int n = ptySlotStrip(base, iface, now, PTY_MAX_SLOTS);
    for (int i = 0; i < n; i++) {
        if (!iface[i]) continue;
        uint8_t lock = 0, en = 1;
        uint32_t mask = 0;
        safeReadU8(iface[i] + 0x239, &lock);
        safeReadU8(iface[i] + 0x20,  &en);
        safeReadU32(iface[i] + 0x18, &mask);
        if (lock || !en)                          // nominal is lock 0, enabled 1
            logLine("ring guard: slot %d iface=%p after-fight lock=%d enabled=%d mask=0x%08x",
                    i, (void*)iface[i], (int)lock, (int)en, mask);
    }
    uintptr_t cls = 0;
    char dlc[40] = {0};
    if (safeReadPtr(hero + 0x12b0, &cls) && cls)
        safeReadCStr(cls + 0x1138, dlc, sizeof dlc);
    uint32_t mode = 0;
    safeReadU32(base + INPUT_MODE_RVA, &mode);
    if (dlc[0] || mode)                           // nominal is "" and 0 (keyboard/mouse)
        logLine("ring guard: hero=%p dlc=\"%s\" input mode=%d (0 kb/mouse, 1 controller)",
                (void*)hero, dlc, (int)mode);
}

static void ringDumpDragManager(uintptr_t base, const char* when) {
    uintptr_t mgr = 0;
    if (!safeReadPtr(base + UI_DRAGDROP_MGR_RVA, &mgr) || !mgr) return;

    uintptr_t obj = 0, target = 0, src = 0;
    uint8_t pressed = 0, begun = 0;
    safeReadPtr(mgr + 0x1b0, &obj);
    safeReadPtr(mgr + 0x18,  &target);
    safeReadPtr(mgr + 0x150, &src);
    safeReadU8(mgr + 0x22c, &pressed);     // the button is registered down
    safeReadU8(mgr + 0x148, &begun);       // frame C sets this -- "begin the drag"

    static uintptr_t lastObj, lastTarget, lastSrc;
    static uint8_t   lastPressed, lastBegun;
    if (obj == lastObj && target == lastTarget && src == lastSrc &&
        pressed == lastPressed && begun == lastBegun) return;
    lastObj = obj; lastTarget = target; lastSrc = src;
    lastPressed = pressed; lastBegun = begun;

    uint32_t hx = 0, hy = 0;
    safeReadU32(mgr + 0x1c0, &hx);
    safeReadU32(mgr + 0x1c4, &hy);
    logLine("ring drag-mgr (%s): dragobj=%p target=%p src=%p (want %p) hit=(%.0f,%.0f)"
            " down=%d begun=%d",
            when, (void*)obj, (void*)target, (void*)src, (void*)g_ringDragWantSrc,
            *reinterpret_cast<float*>(&hx), *reinterpret_cast<float*>(&hy),
            (int)pressed, (int)begun);
}

static bool ringDragMembership(uintptr_t base, uintptr_t hero, bool wantIn, int slot, int total,
                               const char* who) {
    int row = ringRowOfHero(base, hero);
    if (row < 0) {
        logLine("ring: hero %p is on no contestant row -> no drag possible", (void*)hero);
        return false;
    }
    const int64_t rowId  = (int64_t)(RING_ROW_ELEM_BASE + (uint32_t)row);
    const int64_t slotId = (int64_t)(PTY_SLOT_ELEM_BASE + (uint32_t)slot);
    char what[192];
    _snprintf(what, sizeof what, "ring %s %s rank %d",
              wantIn ? "add" : "remove", who, ringRankOfSlot(slot, total));
    what[sizeof what - 1] = 0;
    if (wantIn) ringDumpAddGuards(base, hero);   // before the gesture disturbs anything
    g_ringDragWantSrc = wantIn ? (g_ringRows[row].iface ? g_ringRows[row].iface + 0x78 : 0) : 0;
    if (!wantIn) {
        uintptr_t iface[PTY_MAX_SLOTS], now[PTY_MAX_SLOTS];
        const int n = ptySlotStrip(base, iface, now, PTY_MAX_SLOTS);
        if (slot >= 0 && slot < n && iface[slot]) g_ringDragWantSrc = iface[slot] + 0x78;
    }
    if (!(wantIn ? synthDragElements(rowId, slotId, what)
                 : synthDragElements(slotId, rowId, what)))
        return false;
    g_ringDragHeldSeen = -1;
    g_ringDragKind   = RING_DRAG_MEMBER;
    g_ringDragHero   = hero;
    g_ringDragWantIn = wantIn;
    g_ringDragSlot   = slot;
    g_ringDragFrom   = -1;
    g_ringDragTotal  = total;
    strncpy(g_ringDragName, who ? who : "", sizeof g_ringDragName - 1);
    g_ringDragName[sizeof g_ringDragName - 1] = 0;
    g_ringDragUntil  = GetTickCount() + 1200;   // the script itself spans ~200 ms; slack for the
    return true;                                // panel's own rebuild on the far side
}

// Fire a RANK MOVE: slot -> slot, both ends of the same 'hqst' family.
static bool ringDragMove(uintptr_t base, uintptr_t hero, int fromSlot, int toSlot, int total,
                         const char* who) {
    char what[192];
    _snprintf(what, sizeof what, "ring move %s rank %d->%d", who,
              ringRankOfSlot(fromSlot, total), ringRankOfSlot(toSlot, total));
    what[sizeof what - 1] = 0;
    if (!synthDragElements((int64_t)(PTY_SLOT_ELEM_BASE + (uint32_t)fromSlot),
                           (int64_t)(PTY_SLOT_ELEM_BASE + (uint32_t)toSlot), what))
        return false;
    g_ringDragHeldSeen = -1;
    g_ringDragKind   = RING_DRAG_MOVE;
    g_ringDragHero   = hero;
    g_ringDragWantIn = true;                    // a move never changes membership
    g_ringDragSlot   = toSlot;
    g_ringDragFrom   = fromSlot;
    g_ringDragTotal  = total;
    strncpy(g_ringDragName, who ? who : "", sizeof g_ringDragName - 1);
    g_ringDragName[sizeof g_ringDragName - 1] = 0;
    g_ringDragUntil  = GetTickCount() + 1200;
    return true;
}

static void ringServiceDrag(uintptr_t base) {
    if (!g_ringDragKind) return;

    {
        uintptr_t p = ringPanel(base);
        uint32_t heldNow = 0;
        if (p && safeReadU32(p + RING_HELD_OFF, &heldNow) && (int)heldNow != g_ringDragHeldSeen) {
            g_ringDragHeldSeen = (int)heldNow;
            logLine("ring: mid-drag the panel holds row %d (none = %d)",
                    (int)heldNow, g_ringRowCount);
        }
        ringDumpDragManager(base, "mid-drag");   // the frames our five events are spread over
    }

    int n = 0;
    const int at   = ringSlotOfHero(base, g_ringDragHero, &n);
    const bool done = (g_ringDragKind == RING_DRAG_MOVE) ? (at == g_ringDragSlot)
                                                         : ((at >= 0) == g_ringDragWantIn);
    if (!done && GetTickCount() <= g_ringDragUntil) return;      // still settling

    char utter[MAILBOX_SZ];
    const char* who = g_ringDragName[0] ? g_ringDragName : axs(AXS_THE_HERO);
    if (n <= 0) n = g_ringDragTotal;

    if (g_ringDragKind == RING_DRAG_MOVE) {
        if (!done) {
            char what[160];
            _snprintf(what, sizeof what, "move repair %s to rank %d", who,
                      ringRankOfSlot(g_ringDragSlot, n));
            what[sizeof what - 1] = 0;
            logLine("ring: move drag NOT observed -> repairing with the direct write");
            ringPlaceAtSlot(base, g_ringDragHero, g_ringDragSlot, what);
        }
        const int landed = ringSlotOfHero(base, g_ringDragHero, &n);
        if (landed >= 0) {
            g_ringSlot = landed;                 // the cursor follows the contestant they moved
            _snprintf(utter, sizeof utter, axs(AXS_RNG_AT_RANK_FMT),
                      who, ringRankOfSlot(landed, n));
        } else {
            _snprintf(utter, sizeof utter, "%s", axs(AXS_ACTION_FAILED));
        }
    } else if (done && g_ringDragWantIn) {
        if (at != g_ringDragSlot) {
            char what[160];
            _snprintf(what, sizeof what, "place %s at rank %d", who,
                      ringRankOfSlot(g_ringDragSlot, n));
            what[sizeof what - 1] = 0;
            ringPlaceAtSlot(base, g_ringDragHero, g_ringDragSlot, what);
        }
        const int landed = ringSlotOfHero(base, g_ringDragHero, &n);
        if (landed >= 0) _snprintf(utter, sizeof utter, axs(AXS_RNG_JOINED_RANK_FMT),
                                   who, ringRankOfSlot(landed, n));
        else             _snprintf(utter, sizeof utter, axs(AXS_RNG_JOINED_FMT), who);
    } else if (done) {
        _snprintf(utter, sizeof utter, axs(AXS_RNG_LEFT_RANK_FMT),
                  who, ringRankOfSlot(g_ringDragSlot, n));
    } else if (g_ringDragWantIn) {
        const bool fixed = ringRepairAdd(base, g_ringDragHero, g_ringDragSlot, who);
        const int landed = fixed ? ringSlotOfHero(base, g_ringDragHero, &n) : -1;
        if (landed >= 0) _snprintf(utter, sizeof utter, axs(AXS_RNG_JOINED_RANK_FMT),
                                   who, ringRankOfSlot(landed, n));
        else             _snprintf(utter, sizeof utter, axs(AXS_RNG_CANT_JOIN_FMT), who);
    } else {
        _snprintf(utter, sizeof utter, axs(AXS_RNG_COULDNT_LEAVE_FMT), who);
    }
    utter[sizeof utter - 1] = 0;

    uintptr_t panel = ringPanel(base);
    uint32_t held = 0;
    if (panel && safeReadU32(panel + RING_HELD_OFF, &held) && (int)held != g_ringRowCount) {
        const bool put = ringSehPutdown(base, panel);
        uint32_t after = held;
        safeReadU32(panel + RING_HELD_OFF, &after);
        logLine("ring: the drag left row %u held (none = %d) -> put-down %s, now %u",
                held, g_ringRowCount, put ? "accepted" : "DECLINED", after);
    }
    logLine("ring: drag %s %s -> %s (slot now %d)",
            g_ringDragKind == RING_DRAG_MOVE ? "move" : g_ringDragWantIn ? "add" : "remove",
            who, done ? "observed" : "NOT OBSERVED", at);

    g_ringDragKind = RING_DRAG_NONE;
    g_ringDragHero = 0;
    g_ringDragUntil = 0;
    postSpeech(utter);
}

// ---- Enter in the list: the player's three outcomes in one place ----
static void ringAssign(uintptr_t base, int row, int slot) {
    uintptr_t panel = ringPanel(base);
    if (!panel) return;
    if (row < 0 || row >= g_ringRowCount) return;
    const uintptr_t hero      = g_ringRows[row].hero;
    const bool      dlcLocked = g_ringRows[row].dlcLocked;
    char utter[MAILBOX_SZ];

    if (!hero) { postSpeech(axs(AXS_RNG_EMPTY_ROW)); return; }

    char name[80] = {0}, cls[80] = {0};
    abHeroNameClassOf(base, hero, name, sizeof name, cls, sizeof cls);
    const char* who = name[0] ? name : axs(AXS_THE_HERO);

    if (dlcLocked) {
        _snprintf(utter, sizeof utter, axs(AXS_RNG_DLC_LOCKED_FMT), who);
        utter[sizeof utter - 1] = 0;
        postSpeech(utter);
        return;
    }

    int n = 0;
    int at = ringSlotOfHero(base, hero, &n);
    if (n <= 0) { postSpeech(axs(AXS_RNG_NO_LINEUP)); return; }
    if (slot < 0 || slot >= n) slot = ringLandingSlot(n);

    uintptr_t iface[PTY_MAX_SLOTS], now[PTY_MAX_SLOTS];
    ptySlotStrip(base, iface, now, PTY_MAX_SLOTS);
    uintptr_t occupant = now[slot];

    // ---- the SAME contestant: clear the slot ----
    if (at == slot) {
        logLine("ring: Enter on the slot's own contestant -> drag %s out of rank %d",
                who, ringRankOfSlot(slot, n));
        if (!ringDragMembership(base, hero, false, slot, n, who)) {
            _snprintf(utter, sizeof utter, axs(AXS_RNG_COULDNT_LEAVE_FMT), who);
            utter[sizeof utter - 1] = 0;
            postSpeech(utter);
        }
        return;                                  // ringServiceDrag speaks the outcome
    }

    // ---- a contestant already in ANOTHER slot: move them here ----
    if (at >= 0) {
        logLine("ring: drag %s rank %d -> %d", who,
                ringRankOfSlot(at, n), ringRankOfSlot(slot, n));
        if (!ringDragMove(base, hero, at, slot, n, who)) postSpeech(axs(AXS_ACTION_FAILED));
        return;                                  // ringServiceDrag speaks the outcome
    }

    // ---- not in the lineup: they take the slot ----
    logLine("ring: drag %s onto rank %d (occupant %p)",
            who, ringRankOfSlot(slot, n), (void*)occupant);
    if (!ringDragMembership(base, hero, true, slot, n, who)) {
        _snprintf(utter, sizeof utter, axs(AXS_RNG_CANT_JOIN_FMT), who);
        utter[sizeof utter - 1] = 0;
        postSpeech(utter);
    }
    // ringServiceDrag speaks the outcome, from the strip, on a later frame.
}

// ---- the character sheet ----
static void ringOpenSheet(uintptr_t base, uintptr_t hero, int from) {
    uintptr_t panel = ringPanel(base);
    if (!panel || !hero) { postSpeech(axs(AXS_RNG_EMPTY_ROW)); return; }
    if (csTownSheetPanel(base)) { postSpeech(axs(AXS_SHEET_ALREADY_OPEN)); return; }
    int row = ringRowOfHero(base, hero);
    if (row < 0) {
        logLine("ring: hero %p has no contestant row -> no sheet", (void*)hero);
        postSpeech(axs(AXS_SHEET_DIDNT_OPEN));
        return;
    }
    logLine("ring: C -> PartySetupDisplay inspect(row=%d hero=%p)", row, (void*)hero);
    if (!ringSehInspect(base, panel, hero, row)) { postSpeech(axs(AXS_SHEET_DIDNT_OPEN)); return; }
    g_ptySheetHero = hero;                 // abSelectedHero hands this to the sheet reader
    g_ringSheetFrom = from;
    g_ringSheetWatchUntil = GetTickCount() + 1500;
}

// ---- what the TRINKET PICKER borrows ----

int ringContestants(uintptr_t base, uintptr_t* heroes, char* inLineup, int max) {
    int n = ringCollect(base, false), out = 0;
    for (int i = 0; i < n && out < max; i++) {
        if (!g_ringRows[i].hero) continue;
        heroes[out] = g_ringRows[i].hero;
        if (inLineup) inLineup[out] = g_ringRows[i].inLineup ? 1 : 0;
        out++;
    }
    return out;
}

bool ringOpenSheetForPick(uintptr_t base, uintptr_t hero) {
    g_ringSheetWatchUntil = 0;
    ringOpenSheet(base, hero, 3);
    return g_ringSheetWatchUntil != 0;
}

// ---- entering, leaving, and the per-frame edges ----

static void ringEnter(uintptr_t base) {
    uintptr_t iface[PTY_MAX_SLOTS], hero[PTY_MAX_SLOTS];
    int n = ptySlotStrip(base, iface, hero, PTY_MAX_SLOTS);
    g_ringActive   = true;
    g_ringListOpen = false;
    g_ringHoldHero = 0;
    g_ringSlot     = ringLandingSlot(n);
    g_ringRow      = 0;
    logLine("ring: active (%d slots)", n);
    if (axDebugLogEnabled() && !g_ringDumped) { g_ringDumped = true; ringCollect(base, true); }
    char head[224];
    ringSpeakSlot(base, ringTitle(base, head, sizeof head));
}

void ringLeave(uintptr_t base, const char* why) {
    if (!g_ringActive) return;
    (void)base;
    g_ringActive   = false;
    g_ringListOpen = false;
    g_ringHoldHero = 0;
    g_ringSheetWatchUntil = 0;
    g_ringSheetFrom = 0;                   // a real exit is not a sheet detour
    logLine("ring: stood down (%s)", why ? why : "?");
}

void ringReannounce(uintptr_t base) { ringReannounceWith(base, nullptr); }

void ringReannounceWith(uintptr_t base, const char* prefix) {
    if (!g_ringActive) return;
    if (g_ringListOpen) { ringSpeakRow(base, prefix); return; }
    char head[224], both[416];
    const char* title = ringTitle(base, head, sizeof head);
    if (prefix && *prefix) {
        _snprintf(both, sizeof both, "%s%s", prefix, title ? title : "");
        both[sizeof both - 1] = 0;
        title = both;
    }
    ringSpeakSlot(base, title);
}

// ---- the matchmaking screen the Fight button leads to ----
static void ringMmSpeakState(uintptr_t base, int state) {
    char key[64], line[256], utter[MAILBOX_SZ];
    _snprintf(key, sizeof key, "str_matchmaking_state_%d", state);
    key[sizeof key - 1] = 0;
    if (resolveKey(base, key, line, sizeof line) && line[0]) {
        _snprintf(utter, sizeof utter, "%s", line);
    } else {
        logLine("ring: matchmaking state %d has no string (\"%s\")", state, key);
        _snprintf(utter, sizeof utter, axs(AXS_RNG_MM_UNKNOWN_FMT), state);
    }
    utter[sizeof utter - 1] = 0;
    postSpeech(utter);
}

// ---- "A MATCH IS LOADING": the one second in which the town is a ghost ----
static const int   MM_STATE_FOUND   = 3;      // str_matchmaking_state_3 = "Match found!"
static const DWORD MM_LOADING_MAX_MS = 30000;
static DWORD       g_mmLoadingUntil  = 0;

bool circusMatchLoading() { return g_mmLoading; }

static void ringServiceMatchmaking(uintptr_t base) {
    if (g_mmLoading) {
        uintptr_t raid = 0;
        bool arrived = safeReadPtr(base + MAP_ROOT_RVA, &raid) && raid > 0x10000;
        if (arrived || GetTickCount() > g_mmLoadingUntil) {
            g_mmLoading = false;
            logLine("ring: the match-loading hold is released (%s)",
                    arrived ? "the raid root is up" : "TIMED OUT -- the match never arrived");
        }
    }

    uintptr_t panel = ringMmPanel(base);
    if (!panel) {
        if (g_mmActive) {
            g_mmActive = false;
            if (g_mmState >= MM_STATE_FOUND) {
                g_mmLoading      = true;
                g_mmLoadingUntil = GetTickCount() + MM_LOADING_MAX_MS;
                logLine("ring: matchmaking screen stood down at state %d -- a match is loading, the "
                        "town stands aside", g_mmState);
            } else {
                logLine("ring: matchmaking screen stood down at state %d (no match found)", g_mmState);
            }
            g_mmState = -1;
        }
        return;
    }
    uint8_t state = 0;
    if (!safeReadU8(panel + RING_MM_STATE_OFF, &state)) return;
    if (!g_mmActive) {
        g_mmActive = true;
        g_mmState  = (int)state;
        logLine("ring: matchmaking screen up, state %u", state);
        ringMmSpeakState(base, (int)state);
        return;
    }
    if ((int)state != g_mmState) {
        logLine("ring: matchmaking state %d -> %u", g_mmState, state);
        g_mmState = (int)state;
        ringMmSpeakState(base, (int)state);
    }
}

// ---- E on The Ring: the forward action has to be CALLED, not clicked ----
typedef void (__fastcall *RingProgressFwdFn)(uintptr_t);

bool ringForwardAction(uintptr_t base, const char* label) {
    uintptr_t root = resTownRoot(base);
    if (!root) return false;
    uint32_t layer = 0;
    if (!safeReadU32(root + TM_LAYER_OFF, &layer) || (int)layer != RING_LAYER) return false;

    uintptr_t vft = 0;
    if (!safeReadPtr(root, &vft) || vft != base + TOWNDISP_VFT_RVA) {
        logLine("ring: forward -- townRoot vftable is 0x%llx, not TownDisplay's (+0x%llx); "
                "falling back to the click",
                (unsigned long long)(vft > base ? vft - base : vft),
                (unsigned long long)TOWNDISP_VFT_RVA);
        return false;
    }
    uintptr_t fn = 0;
    if (!safeReadPtr(vft + (uintptr_t)TD_PROGRESS_FWD_SLOT * sizeof(uintptr_t), &fn) ||
        fn != base + TD_PROGRESS_FWD_RVA) {
        logLine("ring: forward -- vftable slot %d holds +0x%llx, not ProgressForwardInternal "
                "(+0x%llx); falling back to the click", TD_PROGRESS_FWD_SLOT,
                (unsigned long long)(fn > base ? fn - base : fn),
                (unsigned long long)TD_PROGRESS_FWD_RVA);
        return false;
    }

    uint64_t opp = 0;
    uint8_t  ready = 0;
    safeReadPtr(base + RING_CHALLENGE_ID_RVA, (uintptr_t*)&opp);
    safeReadU8(base + RING_CHALLENGE_STATE_RVA, &ready);
    logLine("ring: E -> ProgressForwardInternal (label \"%s\", challenge=%llu ready=%u)",
            label ? label : "?", (unsigned long long)opp, ready);
    __try { ((RingProgressFwdFn)fn)(root); }
    __except (sehReport("TownDisplay::ProgressForwardInternal", GetExceptionInformation())) {
        return false;                      // the click fallback is still better than nothing
    }
    return true;
}

// ---- the DIRECT-CHALLENGE LOBBY: who you are facing, and the two-player ready handshake ----
static int  g_ringReady     = -1;       // the last ready byte spoken, -1 = not watching
static char g_ringOpponent[80];

static bool ringOpponentName(uintptr_t base, char* out, size_t n) {
    out[0] = 0;
    uint64_t id = 0;
    if (!safeReadPtr(base + RING_CHALLENGE_ID_RVA, (uintptr_t*)&id) || !id) return false;

    uint8_t notSteam = 0;
    safeReadU8(base + RING_NOT_STEAM_RVA, &notSteam);
    if (!notSteam) {
        HMODULE steam = GetModuleHandleA("steam_api64.dll");
        typedef void* (__cdecl *CtxInitFn)(void*);
        CtxInitFn ctxInit = steam ? (CtxInitFn)GetProcAddress(steam, "SteamInternal_ContextInit")
                                  : nullptr;
        if (ctxInit) {
            const char* name = nullptr;
            __try {
                void*     ctx   = ctxInit((void*)(base + STEAM_FRIENDS_CTX_RVA));
                uintptr_t iface = 0, vft = 0, fn = 0;
                if (ctx && safeReadPtr((uintptr_t)ctx, &iface) && iface &&
                    safeReadPtr(iface, &vft) && vft &&
                    safeReadPtr(vft + 0x38, &fn) && fn) {
                    typedef const char* (__fastcall *PersonaFn)(uintptr_t, uint64_t);
                    name = ((PersonaFn)fn)(iface, id);
                }
            } __except (sehReport("ISteamFriends::GetFriendPersonaName", GetExceptionInformation())) {
                name = nullptr;
            }
            if (name && safeReadCStr((uintptr_t)name, out, (int)n) && out[0]) return true;
        }
        logLine("ring: no Steam persona name for opponent %llu", (unsigned long long)id);
    }
    char raw[64];
    if (resolveKey(base, "str_match_opponent_name", raw, sizeof raw) && raw[0]) {
        stripMarkup(raw, out, (int)n);
        return out[0] != 0;
    }
    return false;
}

// Called from checkRing every frame while the Ring panel is up.
static void ringServiceChallenge(uintptr_t base) {
    uint64_t id = 0;
    if (!safeReadPtr(base + RING_CHALLENGE_ID_RVA, (uintptr_t*)&id) || !id) {
        if (g_ringReady >= 0) {                  // the lobby ended (or we were never in one)
            g_ringReady = -1;
            g_ringOpponent[0] = 0;
            logLine("ring: no direct challenge standing; ready watch stood down");
        }
        return;
    }
    uint8_t state = 0;
    if (!safeReadU8(base + RING_CHALLENGE_STATE_RVA, &state)) return;

    if (g_ringReady < 0) {                       // first frame in this lobby: adopt, don't speak.
        g_ringReady = (int)state;                // The arrival line already named the opponent,
        logLine("ring: lobby ready watch armed at %u", state);
        return;                                  // CHANGE is not news.
    }
    if ((int)state == g_ringReady) return;

    int from = g_ringReady;
    g_ringReady = (int)state;
    logLine("ring: lobby ready state %d -> %u", from, state);

    if (g_embFwdWatchUntil) {
        g_embFwdWatchUntil = 0;
        logLine("ring: the ready byte moved -- that is the forward button's outcome");
    }

    char who[80];
    if (!ringOpponentName(base, who, sizeof who) || !who[0])
        strncpy(who, g_ringOpponent[0] ? g_ringOpponent : "", sizeof who - 1);
    who[sizeof who - 1] = 0;
    if (who[0]) { strncpy(g_ringOpponent, who, sizeof g_ringOpponent - 1);
                  g_ringOpponent[sizeof g_ringOpponent - 1] = 0; }

    char utter[MAILBOX_SZ];
    utter[0] = 0;
    switch (state) {
        case 1:                                  // they readied
            if (who[0]) _snprintf(utter, sizeof utter, axs(AXS_RNG_THEY_READY_FMT), who);
            else        _snprintf(utter, sizeof utter, "%s", axs(AXS_RNG_THEY_READY));
            break;
        case 2:                                  // we readied
            _snprintf(utter, sizeof utter, "%s", axs(AXS_RNG_YOU_READY));
            break;
        case 3:                                  // both -- the match is loading
            _snprintf(utter, sizeof utter, "%s", axs(AXS_RNG_BOTH_READY));
            break;
        case 0:
            if (from == 2)      _snprintf(utter, sizeof utter, "%s", axs(AXS_RNG_YOU_UNREADY));
            else if (who[0])    _snprintf(utter, sizeof utter, axs(AXS_RNG_THEY_UNREADY_FMT), who);
            else                _snprintf(utter, sizeof utter, "%s", axs(AXS_RNG_THEY_UNREADY));
            break;
        default:
            logLine("ring: unknown lobby ready state %u", state);
            _snprintf(utter, sizeof utter, axs(AXS_RNG_READY_UNKNOWN_FMT), (int)state);
            break;
    }
    utter[sizeof utter - 1] = 0;
    if (utter[0]) postSpeech(utter);
}

void checkRing(uintptr_t base) {
    uintptr_t panel = ringPanel(base);
    bool sheet = csTownSheetPanel(base) != 0;

    if (g_ringSheetWatchUntil) {
        if (sheet) {
            g_ringSheetWatchUntil = 0;
        } else if (GetTickCount() > g_ringSheetWatchUntil) {
            g_ringSheetWatchUntil = 0;
            logLine("ring: the character sheet never opened");
            postSpeech(axs(AXS_SHEET_DIDNT_OPEN));
        }
    }

    if (panel) ringServiceDrag(base);            // a membership drag's outcome, from the strip
    ringServiceMatchmaking(base);                // layer 3 is the Fight button's destination
    if (panel) ringServiceChallenge(base);
    if (panel) ringServiceToolbar(base);         // L / P outcomes + the Leagues popup's edges

    if (!panel) {                                // the layer is gone: nothing to come back to
        g_ringSheetFrom = 0;
        g_ringTbWatch = 0; g_ringLeaguesOpen = false;   // no popup to come back to either

        g_ringReady = -1;                        // a fresh lobby re-arms; never speak a stale edge
        g_ringOpponent[0] = 0;
        if (g_ringActive) ringLeave(base, "the screen closed");
        return;
    }
    if (sheet) {                                 // a DETOUR, not an exit -- the sheet reader owns
        if (g_ringActive) {                      // the keys and the cursors wait where they are
            g_ringActive = false;
            logLine("ring: stood down for the character sheet");
        }
        return;
    }
    if (g_ringActive) return;

    if (g_ringSheetFrom) {
        int from = g_ringSheetFrom;
        g_ringSheetFrom = 0;
        g_ptySheetHero  = 0;
        g_ringActive    = true;
        logLine("ring: back from the character sheet (%s)",
                from == 3 ? "trinket pick" : from == 2 ? "list" : "slot");
        if (from == 3) return;
        if (g_ringListOpen) ringSpeakRow(base, nullptr);
        else                ringSpeakSlot(base, nullptr);
        return;
    }

    {
        uintptr_t iface[PTY_MAX_SLOTS], hero[PTY_MAX_SLOTS];
        if (ptySlotStrip(base, iface, hero, PTY_MAX_SLOTS) <= 0) return;
    }
    ringEnter(base);
}

// ---- the toolbar ----
static uintptr_t ringRankDisplay(uintptr_t base) {
    uintptr_t root = resTownRoot(base);
    if (!root) return 0;
    uintptr_t panel = root + RING_RANKDISP_OFF, vft = 0;
    if (!safeReadPtr(panel, &vft) || vft <= base) return 0;
    if (vft - base != RANKDISP_VFT_RVA) {
        static bool moaned = false;
        if (!moaned) {
            moaned = true;
            logLine("ring: townRoot+0x%llx has vftable 0x%llx, not the RankDisplay's 0x%llx -- the "
                    "Leagues panel offset is wrong for this build",
                    (unsigned long long)RING_RANKDISP_OFF, (unsigned long long)(vft - base),
                    (unsigned long long)RANKDISP_VFT_RVA);
        }
        return 0;
    }
    return panel;
}

static bool ringLeaguesShown(uintptr_t base) {
    uintptr_t panel = ringRankDisplay(base);
    uint32_t st = 0;
    return panel && safeReadU32(panel + RING_RANKDISP_SHOWN_OFF, &st) && st != 0;
}

static void ringSpeakLeagues(uintptr_t base, uintptr_t panel) {
    char title[128] = { 0 };
    if (!resolveKey(base, RING_LEAGUES_TITLE_KEY, title, sizeof title) || !title[0]) {
        logLine("ring: %s did not resolve -- speaking the fallback title", RING_LEAGUES_TITLE_KEY);
        _snprintf(title, sizeof title, "%s", axs(AXS_RNG_LEAGUES_FALLBACK));
        title[sizeof title - 1] = 0;
    }
    char names[400] = { 0 }, mixed[600] = { 0 };
    int n = 0; bool allThree = true;
    uintptr_t bld = 0, beg = 0, end = 0;
    if (panel && safeReadPtr(panel + RING_RANKDISP_BLD_OFF, &bld) && bld > 0x10000 &&
        safeReadPtr(bld + RING_LEAGUES_BEG_OFF, &beg) && safeReadPtr(bld + RING_LEAGUES_END_OFF, &end) &&
        beg > 0x10000 && end > beg && (end - beg) % RING_LEAGUE_STRIDE == 0 &&
        (end - beg) / RING_LEAGUE_STRIDE <= (uintptr_t)RING_LEAGUES_MAX) {
        for (uintptr_t e = beg; e < end; e += RING_LEAGUE_STRIDE) {
            char id[0x21] = { 0 }, key[64], name[96] = { 0 };
            uint32_t tiers = 0;
            if (!safeReadCStr(e, id, sizeof id) || !id[0]) { logLine("ring: league entry %d has no id", n); continue; }
            _snprintf(key, sizeof key, "str_league_%s", id); key[sizeof key - 1] = 0;
            if (!resolveKey(base, key, name, sizeof name) || !name[0]) {
                logLine("ring: \"%s\" did not resolve -- speaking the id", key);
                _snprintf(name, sizeof name, "%s", id); name[sizeof name - 1] = 0;
            }
            if (!safeReadU32(e + RING_LEAGUE_TIERS_OFF, &tiers)) tiers = 0;
            if (tiers != 3) allThree = false;
            logLine("ring: league %d id=\"%s\" name=\"%s\" tiers=%u", n, id, name, tiers);
            size_t len = strlen(names);
            _snprintf(names + len, sizeof names - len, "%s%s", n ? ", " : "", name);
            names[sizeof names - 1] = 0;
            char one[128];
            _snprintf(one, sizeof one, axs(AXS_RNG_LEAGUE_TIERS_FMT), name, tiers); one[sizeof one - 1] = 0;
            len = strlen(mixed);
            _snprintf(mixed + len, sizeof mixed - len, "%s%s", n ? " " : "", one);
            mixed[sizeof mixed - 1] = 0;
            n++;
        }
    } else {
        logLine("ring: the Leagues popup's building/vector is unreadable (panel=%llx bld=%llx beg=%llx end=%llx)",
                (unsigned long long)panel, (unsigned long long)bld, (unsigned long long)beg, (unsigned long long)end);
    }
    char utter[MAILBOX_SZ];
    if (n == 0)          _snprintf(utter, sizeof utter, axs(AXS_RNG_LEAGUES_EMPTY_FMT), title);
    else if (allThree)   _snprintf(utter, sizeof utter, axs(AXS_RNG_LEAGUES_ALL3_FMT), title, names);
    else                 _snprintf(utter, sizeof utter, axs(AXS_RNG_LEAGUES_MIXED_FMT), title, mixed);
    utter[sizeof utter - 1] = 0;
    postSpeech(utter);
}

static void ringLogToolbarGate(uintptr_t base) {
    if (g_ringTbGateLogged) return;
    g_ringTbGateLogged = true;
    char owner[0x44];
    if (kbActionForKey(base, (int)SDLK_l, owner, sizeof owner)) logLine("ring GATE: L is bound to the game action \"%s\"", owner);
    else logLine("ring GATE: L is not in the keyboard binding table");
    if (kbActionForKey(base, (int)SDLK_p, owner, sizeof owner)) logLine("ring GATE: P is bound to the game action \"%s\"", owner);
    else logLine("ring GATE: P is not in the keyboard binding table");
}

static void ringToolbarPress(uintptr_t base, uint32_t elem, int watch, const char* what) {
    if (g_ringTbWatch || clickQueued()) { postSpeech(axs(AXS_RNG_TB_BUSY)); return; }
    if (!feGetElementById((int64_t)elem)) {
        logLine("ring: the %s button (0x%x) is not on this frame", what, elem);
        diagDumpFocusElements(base, "ring-toolbar-missing");
        postSpeech(axs(AXS_RNG_TB_MISSING));
        return;
    }
    if (!frontEndClickElementId((int64_t)elem)) {   // hover then press, on separate frames
        logLine("ring: the click on the %s button (0x%x) could not be queued", what, elem);
        postSpeech(axs(AXS_ACTION_FAILED));
        return;
    }
    logLine("ring: clicked the %s button (0x%x)", what, elem);
    g_ringTbWatch = watch;
    g_ringTbUntil = GetTickCount() + RING_TB_WATCH_MS;
}

static void ringServiceToolbar(uintptr_t base) {
    bool shown = ringLeaguesShown(base);
    if (g_ringTbWatch) {
        DWORD now = GetTickCount();
        bool timedOut = now > g_ringTbUntil;
        switch (g_ringTbWatch) {
        case 1:                                          // Leagues opening
            if (shown) {
                g_ringTbWatch = 0; g_ringLeaguesOpen = true;
                logLine("ring: the Leagues window opened");
                ringSpeakLeagues(base, ringRankDisplay(base));
            } else if (timedOut) {
                g_ringTbWatch = 0;
                logLine("ring: the Leagues window never opened");
                logInputSnapshot(base, "ring-leagues-timeout");
                diagDumpFocusElements(base, "ring-leagues-timeout");
                postSpeech(axs(AXS_RNG_LEAGUES_DIDNT_OPEN));
            }
            break;
        case 2:                                          // the practice dialog
            if (axIsDialog()) {
                g_ringTbWatch = 0;
                logLine("ring: the practice dialog is up -- the dialog reader speaks it");
            } else if (timedOut) {
                g_ringTbWatch = 0;
                logLine("ring: the practice dialog never opened");
                logInputSnapshot(base, "ring-practice-timeout");
                diagDumpFocusElements(base, "ring-practice-timeout");
                postSpeech(axs(AXS_RNG_PRACTICE_DIDNT_OPEN));
            }
            break;
        case 3:                                          // Leagues closing (L on the open popup)
            if (!shown) g_ringTbWatch = 0;
            else if (timedOut) {
                g_ringTbWatch = 0;
                logLine("ring: the Leagues window did not close on the second click");
                postSpeech(axs(AXS_RNG_LEAGUES_STILL_OPEN));
            }
            break;
        default: g_ringTbWatch = 0; break;
        }
    }
    if (g_ringLeaguesOpen && !shown) {
        g_ringLeaguesOpen = false;
        logLine("ring: the Leagues window closed");
        postSpeech(axs(AXS_RNG_LEAGUES_CLOSED));
    } else if (!g_ringLeaguesOpen && shown) {          // opened by the mouse or a controller
        g_ringLeaguesOpen = true;
        logLine("ring: the Leagues window opened (not by L)");
        ringSpeakLeagues(base, ringRankDisplay(base));
    }
}

// ---- the keys ----
bool routeRingKey(uintptr_t base, uint32_t sym, uint16_t mod, uint8_t repeat) {
    if (mod & (KMOD_LALT | KMOD_RALT | KMOD_LCTRL | KMOD_RCTRL)) return false;

    uintptr_t iface[PTY_MAX_SLOTS], hero[PTY_MAX_SLOTS];

    if (sym == SDLK_ESCAPE) {
        if (!g_ringHoldHero) return false;       // the game's own way out of The Ring
        if (!repeat) {
            g_ringHoldHero = 0;
            postSpeech(axs(AXS_RNG_MOVE_CANCELLED));
        }
        return true;
    }

    if (sym == SDLK_l || sym == SDLK_p) {
        if (repeat) return true;
        ringLogToolbarGate(base);
        if (g_ringHoldHero) { postSpeech(axs(AXS_RNG_FINISH_MOVE_FIRST)); return true; }
        if (sym == SDLK_l) {
            ringToolbarPress(base, RING_TB_LEAGUES_ELEM, g_ringLeaguesOpen ? 3 : 1,
                             g_ringLeaguesOpen ? "Leagues (close)" : "Leagues");
        } else {
            if (g_ringLeaguesOpen) { postSpeech(axs(AXS_RNG_LEAGUES_OPEN_HINT)); return true; }
            ringToolbarPress(base, RING_TB_PRACTICE_ELEM, 2, "Practice Battle");
        }
        return true;
    }
    if (g_ringLeaguesOpen) {
        int jump = 0;
        bool ours = sym == SDLK_LEFT || sym == SDLK_RIGHT || sym == SDLK_c || sym == SDLK_SPACE ||
                    sym == SDLK_RETURN || sym == SDLK_KP_ENTER || axDecodeJump(sym, mod, repeat, &jump);
        if (!ours) return false;
        if (!repeat) postSpeech(axs(AXS_RNG_LEAGUES_OPEN_HINT));
        return true;
    }

    {
        int jump = 0;
        if (axDecodeJump(sym, mod, repeat, &jump)) {     // Home/End: first/last lineup slot
            if (!jump) return true;                      // held jump: one landing per press
            int n = ptySlotStrip(base, iface, hero, PTY_MAX_SLOTS);
            if (n <= 0) { postSpeech(axs(AXS_RNG_NO_LINEUP)); return true; }
            axStepCursor(&g_ringSlot, n, jump);          // the clamp lands it; ends re-read
            ringSpeakSlot(base, nullptr);
            return true;
        }
    }

    if (sym == SDLK_LEFT || sym == SDLK_RIGHT) {
        if (axNavHoldRepeat(repeat)) return true;
        int n = ptySlotStrip(base, iface, hero, PTY_MAX_SLOTS);
        if (n <= 0) { postSpeech(axs(AXS_RNG_NO_LINEUP)); return true; }
        axStepCursor(&g_ringSlot, n, 0);
        axStepCursor(&g_ringSlot, n, sym == SDLK_RIGHT ? 1 : -1);
        ringSpeakSlot(base, nullptr);
        return true;
    }

    if (sym == SDLK_c) {
        if (repeat) return true;
        int n = ptySlotStrip(base, iface, hero, PTY_MAX_SLOTS);
        if (n <= 0) { postSpeech(axs(AXS_RNG_NO_LINEUP)); return true; }
        axStepCursor(&g_ringSlot, n, 0);
        if (!hero[g_ringSlot]) { postSpeech(axs(AXS_RNG_EMPTY_SLOT)); return true; }
        ringOpenSheet(base, hero[g_ringSlot], 1);
        return true;
    }

    if (sym == SDLK_SPACE) {
        if (repeat) return true;
        int n = ptySlotStrip(base, iface, hero, PTY_MAX_SLOTS);
        if (n <= 0) { postSpeech(axs(AXS_RNG_NO_LINEUP)); return true; }
        axStepCursor(&g_ringSlot, n, 0);
        if (!g_ringHoldHero) {                   // pick up
            if (!hero[g_ringSlot]) { postSpeech(axs(AXS_RNG_EMPTY_SLOT)); return true; }
            char name[80] = {0}, cls[80] = {0};
            abHeroNameClassOf(base, hero[g_ringSlot], name, sizeof name, cls, sizeof cls);
            strncpy(g_ringHoldName, name[0] ? name : axs(AXS_RNG_HELD_FALLBACK),
                    sizeof g_ringHoldName - 1);
            g_ringHoldName[sizeof g_ringHoldName - 1] = 0;
            g_ringHoldHero = hero[g_ringSlot];
            char utter[128];
            _snprintf(utter, sizeof utter, axs(AXS_RNG_MOVING_FMT), g_ringHoldName);
            utter[sizeof utter - 1] = 0;
            postSpeech(utter);
            return true;
        }
        uintptr_t held = g_ringHoldHero;
        g_ringHoldHero = 0;
        int at = ringSlotOfHero(base, held, &n);
        if (at < 0) {
            logLine("ring: the held contestant is no longer in the lineup -> drop cancelled");
            postSpeech(axs(AXS_RNG_MOVE_CANCELLED));
            return true;
        }
        if (at == g_ringSlot) {                  // dropped where they already stand
            char utter[192];
            _snprintf(utter, sizeof utter, axs(AXS_RNG_AT_RANK_FMT),
                      g_ringHoldName, ringRankOfSlot(at, n));
            utter[sizeof utter - 1] = 0;
            postSpeech(utter);
            return true;
        }
        if (!ringDragMove(base, held, at, g_ringSlot, n, g_ringHoldName))
            postSpeech(axs(AXS_ACTION_FAILED));
        return true;                             // ringServiceDrag speaks the outcome
    }

    if (sym == SDLK_RETURN || sym == SDLK_KP_ENTER) {
        if (repeat) return true;
        if (g_ringHoldHero) { postSpeech(axs(AXS_RNG_FINISH_MOVE_FIRST)); return true; }
        int n = ptySlotStrip(base, iface, hero, PTY_MAX_SLOTS);
        if (n <= 0) { postSpeech(axs(AXS_RNG_NO_LINEUP)); return true; }
        axStepCursor(&g_ringSlot, n, 0);
        int rows = ringCollect(base, false);
        if (rows <= 0) { postSpeech(axs(AXS_RNG_NO_CONTESTANTS)); return true; }
        g_ringForSlot  = g_ringSlot;
        g_ringListOpen = true;
        int on = hero[g_ringSlot] ? ringRowOfHero(base, hero[g_ringSlot]) : -1;
        g_ringRow = on >= 0 ? on : 0;
        char head[224];
        _snprintf(head, sizeof head, axs(AXS_RNG_CHOOSE_FOR_RANK_FMT),
                  ringRankOfSlot(g_ringForSlot, n));
        head[sizeof head - 1] = 0;
        char prefix[240];
        _snprintf(prefix, sizeof prefix, "%s ", head);
        prefix[sizeof prefix - 1] = 0;
        ringSpeakRow(base, prefix);
        return true;
    }

    return false;
}

bool routeRingListKey(uintptr_t base, uint32_t sym, uint16_t mod, uint8_t repeat) {
    if (mod & (KMOD_LALT | KMOD_RALT | KMOD_LCTRL | KMOD_RCTRL)) return false;

    if (sym == SDLK_ESCAPE) {
        if (repeat) return true;
        g_ringListOpen = false;
        ringSpeakSlot(base, nullptr);
        return true;
    }

    {
        int jump = 0;
        if (axDecodeJump(sym, mod, repeat, &jump)) {     // Home/End: first/last contestant
            if (!jump) return true;                      // held jump: one landing per press
            int n = ringCollect(base, false);
            if (n <= 0) { postSpeech(axs(AXS_RNG_NO_CONTESTANTS)); return true; }
            axStepCursor(&g_ringRow, n, jump);           // the clamp lands it; ends re-read
            ringSpeakRow(base, nullptr);
            return true;
        }
    }

    if (sym == SDLK_UP || sym == SDLK_DOWN) {
        if (axNavHoldRepeat(repeat)) return true;
        int n = ringCollect(base, false);
        if (n <= 0) { postSpeech(axs(AXS_RNG_NO_CONTESTANTS)); return true; }
        axStepCursor(&g_ringRow, n, 0);
        axStepCursor(&g_ringRow, n, sym == SDLK_DOWN ? 1 : -1);
        ringSpeakRow(base, nullptr);
        return true;
    }

    if (sym == SDLK_c) {
        if (repeat) return true;
        int n = ringCollect(base, false);
        if (n <= 0) { postSpeech(axs(AXS_RNG_NO_CONTESTANTS)); return true; }
        axStepCursor(&g_ringRow, n, 0);
        if (!g_ringRows[g_ringRow].hero) { postSpeech(axs(AXS_RNG_EMPTY_ROW)); return true; }
        ringOpenSheet(base, g_ringRows[g_ringRow].hero, 2);
        return true;
    }

    if (sym == SDLK_RETURN || sym == SDLK_KP_ENTER) {
        if (repeat) return true;
        int n = ringCollect(base, false);
        if (n <= 0) { postSpeech(axs(AXS_RNG_NO_CONTESTANTS)); return true; }
        axStepCursor(&g_ringRow, n, 0);
        ringAssign(base, g_ringRow, g_ringForSlot);
        g_ringListOpen = false;
        return true;                       // the assign line has just described
    }

    return false;
}
