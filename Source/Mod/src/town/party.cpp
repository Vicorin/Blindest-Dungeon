// town/party.cpp -- the seventh TOWN slice

#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cmath>
#include "internal.h"
// ---- TOWN: the EMBARK PARTY LINEUP (AX_PARTY) ----
static const uintptr_t PTY_ENTRY_STATE_OFF   = 0x1540;   // Roster::Entry+: roster.status (was 0x14c0)
static const uintptr_t PTY_ENTRY_ORDER_OFF   = 0x1548;
static const uintptr_t PTY_ENTRY_DIRTY_OFF   = 0x15bc;
static const uintptr_t PTY_HERO_GUID_OFF     = 0x130c;
static const uintptr_t PTY_IFACE_SETHERO_VOFF= 0x30;     // interface vftable: SetHero(Hero*)
static const uintptr_t PTY_SHEET_PANEL_OFF   = 0x38c0;
static const uintptr_t PTY_SHEET_STATE_OFF   = 0x3918;
static const uintptr_t PTY_EMBARK_PANEL_OFF  = 0x3640;
static const uintptr_t PTY_SLOT_RESTRICT_OFF = 0x7a;     // slot widget+: byte, 1 = this hero is
static const int PTY_SLOT_TOTAL = 4;                     // the campaign party size

struct PtyRow {
    uintptr_t entry;       // the Roster::Entry, 0 = empty position
    uintptr_t hero;        // entry+0x08, 0 = empty position
    uintptr_t iface;
    bool      barred;
};
static PtyRow g_ptyRows[PTY_MAX_SLOTS];
static int    g_ptyCount = 0;      // rows spoken (>= party size, empties padded to 4)
static int    g_ptyParty = 0;      // how many rows actually hold heroes
static bool   g_ptyFromStrip = false;
static int       g_ptyRow      = 0;      // row cursor (position order)
static uintptr_t g_ptyHoldHero = 0;      // Space picked this hero up; 0 = nothing held
static char      g_ptyHoldName[80];
static bool      g_ptyDumped   = false;
static DWORD     g_ptySheetWatchUntil = 0; // C's open-watch deadline; 0 = idle
static bool      g_ptyResume   = false;
static int       g_ptySheetFrom = 0;     // who launched the sheet: 1 = lineup, 2 = roster
static bool      g_rosOnBar    = false;
static int       g_rosBarCol   = 0;      // 0 = capacity, 1..N = the sort buttons
int              g_rosPickSlot = -1;     // >= 0: the roster is picking FOR that lineup slot
static bool      g_rosPickAct  = false;  // the roster is picking for an ACTIVITY SLOT
static uintptr_t g_rosHoldEntry = 0;
static char      g_rosHoldName[80];      // ... its name, read once at pick-up time

static void rosEnterFromParty(uintptr_t base, uintptr_t hero, int pickSlot);
static void rosResumeFromSheet(uintptr_t base);
static bool rosSehSetState(uintptr_t base, uintptr_t system, uintptr_t entry, int state);
void ptyLeave(uintptr_t base, const char* why);
// The trinket pick's own resume (from = 3), defined with that flow further down.
static void trkPickSheetClosed(uintptr_t base);

bool axIsParty() { return g_ptyActive; }

uintptr_t csTownSheetPanel(uintptr_t base) {
    uintptr_t root = resTownRoot(base);
    if (!root) return 0;
    uint32_t state = 0;
    if (!safeReadU32(root + PTY_SHEET_STATE_OFF, &state) || state == 0) return 0;
    return root + PTY_SHEET_PANEL_OFF;
}

typedef unsigned char (*PtyPanelHideFn)(uintptr_t panel, void* onHidden, unsigned char flag);
bool csCloseTownSheet(uintptr_t base) {
    uintptr_t panel = csTownSheetPanel(base);
    if (!panel) return false;
    unsigned long long onHidden[16];              // the empty std::function, +0x38 = 0
    memset(onHidden, 0, sizeof onHidden);
    bool ok = true;
    __try { ((PtyPanelHideFn)(base + PTY_PANEL_HIDE_RVA))(panel, onHidden, 0); }
    __except (EXCEPTION_EXECUTE_HANDLER) { ok = false; }
    logLine("charsheet: town sheet Hide issued (panel=%p) -> %s", (void*)panel,
            ok ? "called" : "FAULTED");
    if (!ok) return false;
    g_ptyResume = false;
    g_ptySheetFrom = 0;
    return true;
}

int ptySlotStrip(uintptr_t base, uintptr_t* ifaceOut, uintptr_t* heroOut, int maxOut);

static uintptr_t ptyEntryOfHero(uintptr_t base, uintptr_t hero) {
    if (!hero) return 0;
    uintptr_t campaign = 0, beg = 0, end = 0;
    if (!safeReadPtr(base + RES_CAMPAIGN_RVA, &campaign) || campaign <= 0x10000) return 0;
    if (!safeReadPtr(campaign + PTY_ENTRIES_BEG_OFF, &beg) ||
        !safeReadPtr(campaign + PTY_ENTRIES_END_OFF, &end) || !beg || end <= beg) return 0;
    int n = (int)((end - beg) / 8);
    if (n < 0 || n > 256) return 0;
    for (int i = 0; i < n; i++) {
        uintptr_t e = 0;
        if (!safeReadPtr(beg + (uintptr_t)i * 8, &e) || e <= 0x10000) continue;
        if (e + PTY_ENTRY_HERO_OFF == hero) return e;
    }
    return 0;
}

static int ptyCollect(uintptr_t base, bool probe) {
    g_ptyCount = 0;
    g_ptyParty = 0;
    g_ptyFromStrip = false;
    if (!resTownRoot(base)) return 0;

    // ---- source 1: the live widget strip ----
    {
        uintptr_t iface[PTY_MAX_SLOTS], hero[PTY_MAX_SLOTS];
        int n = ptySlotStrip(base, iface, hero, PTY_MAX_SLOTS);
        if (n > 0) {
            uintptr_t beg = 0, root = resTownRoot(base);
            safeReadPtr(root + PTY_SLOTS_BEG_OFF, &beg);
            for (int i = 0; i < n; i++) {
                g_ptyRows[i].entry  = hero[i] ? ptyEntryOfHero(base, hero[i]) : 0;
                g_ptyRows[i].hero   = hero[i];
                g_ptyRows[i].iface  = iface[i];
                g_ptyRows[i].barred = false;
                uintptr_t slotW = 0;
                uint8_t   bar   = 0;
                if (beg && safeReadPtr(beg + (uintptr_t)i * 8, &slotW) && slotW > 0x10000 &&
                    safeReadU8(slotW + PTY_SLOT_RESTRICT_OFF, &bar))
                    g_ptyRows[i].barred = (bar != 0);
                if (hero[i]) g_ptyParty++;
            }
            g_ptyCount = n;
            g_ptyFromStrip = true;
            if (probe) {
                logLine("party probe: reading the LIVE EMBARK STRIP, %d slots, %d filled",
                        n, g_ptyParty);
                for (int i = 0; i < n; i++) {
                    char name[80] = {0};
                    if (g_ptyRows[i].hero)
                        safeReadCStr(g_ptyRows[i].hero + HERO_NAME_OFF, name, sizeof name);
                    float sx = -1.f, sy = -1.f;
                    uintptr_t el = feGetElementById((int64_t)(PTY_SLOT_ELEM_BASE + (uint32_t)i));
                    if (el) elemCenter(el, &sx, &sy);
                    logLine("party probe: slot %d = position %d iface=%p hero=%p entry=%p \"%s\" "
                            "barred=%d elem=0x%x %s screenX=%.0f screenY=%.0f",
                            i, n - i, (void*)iface[i], (void*)hero[i], (void*)g_ptyRows[i].entry,
                            g_ptyRows[i].hero ? name : "(empty)", g_ptyRows[i].barred ? 1 : 0,
                            PTY_SLOT_ELEM_BASE + (uint32_t)i, el ? "found" : "NOT ON SCREEN", sx, sy);
                }
            }
            return g_ptyCount;
        }
    }

    // ---- source 2: the roster entries ----
    uintptr_t campaign = 0;
    if (!safeReadPtr(base + RES_CAMPAIGN_RVA, &campaign) || campaign <= 0x10000) return 0;
    uintptr_t beg = 0, end = 0;
    if (!safeReadPtr(campaign + PTY_ENTRIES_BEG_OFF, &beg) ||
        !safeReadPtr(campaign + PTY_ENTRIES_END_OFF, &end) || !beg || end <= beg) return 0;
    int n = (int)((end - beg) / 8);
    if (n < 0 || n > 256) {
        logLine("party: roster entry count %d is implausible -> refusing the read", n);
        return 0;
    }
    float rowTs[PTY_MAX_SLOTS];
    for (int i = 0; i < n; i++) {
        uintptr_t entry = 0;
        uint32_t  state = 0;
        float     ts = 0.f;
        if (!safeReadPtr(beg + (uintptr_t)i * 8, &entry) || entry <= 0x10000) continue;
        if (!safeReadU32(entry + PTY_ENTRY_STATE_OFF, &state) || state != 1) continue;
        abReadF32(entry + PTY_ENTRY_ORDER_OFF, &ts);
        if (g_ptyParty >= PTY_MAX_SLOTS) {
            logLine("party: more than %d in-party entries -> extra dropped", PTY_MAX_SLOTS);
            break;
        }
        // insertion sort ascending by timestamp — the game's own order rule
        int at = g_ptyParty;
        while (at > 0 && ts < rowTs[at - 1]) {
            g_ptyRows[at] = g_ptyRows[at - 1];
            rowTs[at] = rowTs[at - 1];
            at--;
        }
        g_ptyRows[at].entry  = entry;
        g_ptyRows[at].hero   = entry + PTY_ENTRY_HERO_OFF;
        g_ptyRows[at].iface  = 0;                // no widgets on this surface
        g_ptyRows[at].barred = false;            // and no restriction byte to read either
        rowTs[at] = ts;
        g_ptyParty++;
    }
    for (int i = 0, j = g_ptyParty - 1; i < j; i++, j--) {
        PtyRow tr = g_ptyRows[i]; g_ptyRows[i] = g_ptyRows[j]; g_ptyRows[j] = tr;
        float tf = rowTs[i];      rowTs[i]     = rowTs[j];     rowTs[j]     = tf;
    }
    g_ptyCount = g_ptyParty > PTY_SLOT_TOTAL ? g_ptyParty : PTY_SLOT_TOTAL;
    for (int r = g_ptyParty; r < g_ptyCount; r++) {
        g_ptyRows[r].entry = 0; g_ptyRows[r].hero = 0;
        g_ptyRows[r].iface = 0; g_ptyRows[r].barred = false;
    }
    if (probe) {
        logLine("party probe: %d roster entries, %d in party", n, g_ptyParty);
        for (int r = 0; r < g_ptyParty; r++) {
            char name[80] = {0};
            uint32_t entryGuid = 0, heroGuid = 0;
            safeReadCStr(g_ptyRows[r].hero + HERO_NAME_OFF, name, sizeof name);
            safeReadU32(g_ptyRows[r].entry, &entryGuid);
            safeReadU32(g_ptyRows[r].hero + PTY_HERO_GUID_OFF, &heroGuid);
            logLine("party probe: pos %d entry=%p hero=%p \"%s\" guid=%u/%u ts=%.1f",
                    r + 1, (void*)g_ptyRows[r].entry, (void*)g_ptyRows[r].hero, name,
                    entryGuid, heroGuid, rowTs[r]);
        }
        uintptr_t root = resTownRoot(base), wbeg = 0, wend = 0;
        if (root) {
            safeReadPtr(root + PTY_SLOTS_BEG_OFF, &wbeg);
            safeReadPtr(root + PTY_SLOTS_END_OFF, &wend);
            logLine("party probe: embark slot widgets = %d",
                    (wbeg && wend > wbeg) ? (int)((wend - wbeg) / 8) : 0);
        }
    }
    return g_ptyCount;
}

int ptySlotStrip(uintptr_t base, uintptr_t* ifaceOut, uintptr_t* heroOut, int maxOut) {
    uintptr_t root = resTownRoot(base);
    if (!root) return 0;
    uintptr_t beg = 0, end = 0;
    if (!safeReadPtr(root + PTY_SLOTS_BEG_OFF, &beg) ||
        !safeReadPtr(root + PTY_SLOTS_END_OFF, &end) || !beg || end <= beg) return 0;
    int n = (int)((end - beg) / 8);
    if (n <= 0 || n > PTY_MAX_SLOTS) return 0;
    if (n > maxOut) n = maxOut;
    for (int i = 0; i < n; i++) {
        uintptr_t slotW = 0, iface = 0, h = 0;
        ifaceOut[i] = 0;
        heroOut[i]  = 0;
        if (!safeReadPtr(beg + (uintptr_t)i * 8, &slotW) || slotW <= 0x10000) continue;
        if (!safeReadPtr(slotW + PTY_SLOT_IFACE_OFF, &iface) || iface <= 0x10000) continue;
        safeReadPtr(iface + PTY_IFACE_HERO_OFF, &h);
        ifaceOut[i] = iface;
        heroOut[i]  = h;
    }
    return n;
}
static int ptyPositionIfaces(uintptr_t base, uintptr_t* out, int n) {
    if (!g_ptyFromStrip) return 0;
    for (int i = 0; i < n; i++) {
        if (i >= g_ptyCount || !g_ptyRows[i].iface) {
            logLine("party: slot %d (position %d) has no strip interface -> refusing to write",
                    i, g_ptyCount - i);
            return -1;
        }
        out[i] = g_ptyRows[i].iface;
    }
    return n;
}

static void ptyHeroFragEx(uintptr_t base, uintptr_t hero, char* out, int outsz,
                          bool withStress, bool withGear) {
    char name[80] = {0}, cls[80] = {0};
    abHeroNameClassOf(base, hero, name, sizeof name, cls, sizeof cls);

    char rank[96] = {0};
    int level = csResolveLevel(base, hero);
    if (level >= 0) {
        char key[32];
        _snprintf(key, sizeof key, "str_resolve_%d", level);
        key[sizeof key - 1] = 0;
        if (!resolveKey(base, key, rank, sizeof rank)) rank[0] = 0;
    }

    char title[192] = {0};
    if (rank[0] && cls[0])      _snprintf(title, sizeof title, "%s %s", rank, cls);
    else if (rank[0] || cls[0]) _snprintf(title, sizeof title, "%s", rank[0] ? rank : cls);
    title[sizeof title - 1] = 0;

    char item[128], frag[256];
    out[0] = 0;
    _snprintf(out, outsz, "%s", name[0] ? name : axs(AXS_HERO_UNREADABLE));
    out[outsz - 1] = 0;
    if (title[0]) {
        _snprintf(frag, sizeof frag, ", %s", title);
        frag[sizeof frag - 1] = 0;
        strncat(out, frag, outsz - strlen(out) - 1);
    }
    float stress = 0.f;
    if (withStress && abReadF32(hero + ACTOR_STRESS_OFF, &stress) &&
        stress >= 0.f && stress <= 1000.f) {
        _snprintf(item, sizeof item, axs(AXS_PTY_STRESS_FMT), (int)ceilf(stress));
        item[sizeof item - 1] = 0;
        _snprintf(frag, sizeof frag, ", %s", item);
        frag[sizeof frag - 1] = 0;
        strncat(out, frag, outsz - strlen(out) - 1);
    }
    if (csHeroIsCursed(hero)) {
        _snprintf(frag, sizeof frag, ", %s", axs(AXS_PTY_CURSED));
        frag[sizeof frag - 1] = 0;
        strncat(out, frag, outsz - strlen(out) - 1);
    }
    if (csHeroIsNeverAgain(hero)) {
        char word[96] = {0};
        csNeverAgainWord(base, word, sizeof word);
        _snprintf(frag, sizeof frag, ", %s", word);
        frag[sizeof frag - 1] = 0;
        strncat(out, frag, outsz - strlen(out) - 1);
    }
    if (uintptr_t rlq = csHeroRosterLimitQuirk(hero)) {
        char id[64] = {0}, word[96] = {0};
        if (!(csQuirkId(rlq, id, sizeof id) && csQuirkName(base, id, word, sizeof word) && word[0])) {
            _snprintf(word, sizeof word, "%s", axs(AXS_PTY_ROSTER_LIMIT_FALLBACK));
            word[sizeof word - 1] = 0;
        }
        _snprintf(frag, sizeof frag, ", %s", word);
        frag[sizeof frag - 1] = 0;
        strncat(out, frag, outsz - strlen(out) - 1);
    }
    if (!withGear) return;
    uint32_t wi = 0, ai = 0;
    if (safeReadU32(hero + HERO_WEAPON_LEVEL_OFF, &wi) && wi > 0 && wi <= 16) {
        _snprintf(item, sizeof item, axs(AXS_PTY_WEAPON_TIER_FMT), wi);
        item[sizeof item - 1] = 0;
        _snprintf(frag, sizeof frag, ", %s", item);
        frag[sizeof frag - 1] = 0;
        strncat(out, frag, outsz - strlen(out) - 1);
    }
    if (safeReadU32(hero + HERO_ARMOUR_LEVEL_OFF, &ai) && ai > 0 && ai <= 16) {
        _snprintf(item, sizeof item, axs(AXS_PTY_ARMOR_TIER_FMT), ai);
        item[sizeof item - 1] = 0;
        _snprintf(frag, sizeof frag, ", %s", item);
        frag[sizeof frag - 1] = 0;
        strncat(out, frag, outsz - strlen(out) - 1);
    }
}

void ptyHeroFrag(uintptr_t base, uintptr_t hero, char* out, int outsz) {
    ptyHeroFragEx(base, hero, out, outsz, true, true);
}

void ptyHeroFragBare(uintptr_t base, uintptr_t hero, char* out, int outsz) {
    ptyHeroFragEx(base, hero, out, outsz, false, false);
}

static int ptyPositionOfRow(int row) { return g_ptyCount - row; }

static void ptyCardLine(uintptr_t base, const PtyRow* r, int pos, int total, char* out, int outsz) {
    char posln[96];
    _snprintf(posln, sizeof posln, axs(AXS_PTY_POSITION_FMT), pos, total);
    posln[sizeof posln - 1] = 0;
    if (!r->hero) {
        _snprintf(out, outsz, "%s %s", axs(AXS_PTY_EMPTY_SLOT), posln);
        out[outsz - 1] = 0;
        return;
    }
    ptyHeroFrag(base, r->hero, out, outsz);
    char frag[224];
    if (r->barred) _snprintf(frag, sizeof frag, ". %s %s", axs(AXS_PTY_NOT_ALLOWED), posln);
    else           _snprintf(frag, sizeof frag, ". %s", posln);
    frag[sizeof frag - 1] = 0;
    strncat(out, frag, outsz - strlen(out) - 1);
}

static const char* ptyHeader(uintptr_t base, char* buf, int bufsz) {
    char name[EMB_NAME_MAX], head[224];
    if (embPartyName(base, name, sizeof name))
        _snprintf(head, sizeof head, axs(AXS_PTY_HEADER_NAME_FMT), name);
    else
        _snprintf(head, sizeof head, "%s", axs(AXS_PTY_HEADER));
    head[sizeof head - 1] = 0;
    _snprintf(buf, bufsz, "%s ", head);
    buf[bufsz - 1] = 0;
    return buf;
}

static void ptySpeakRow(uintptr_t base, const char* prefix) {
    int n = ptyCollect(base, false);
    if (n <= 0) { postSpeech(axs(AXS_PTY_NO_LINEUP)); return; }
    axStepCursor(&g_ptyRow, n, 0);           // re-clamp against the live lineup
    char card[512];
    ptyCardLine(base, &g_ptyRows[g_ptyRow], ptyPositionOfRow(g_ptyRow), n, card, sizeof card);
    char utter[MAILBOX_SZ];
    _snprintf(utter, sizeof utter, "%s%s", prefix ? prefix : "", card);
    utter[sizeof utter - 1] = 0;
    postSpeech(utter);
}

void ptyReannounce(uintptr_t base) {
    if (!g_ptyActive) return;
    logLine("party: re-announcing after a modal closed");
    char hdr[192];
    ptySpeakRow(base, ptyHeader(base, hdr, sizeof hdr));
}

typedef void (*PtySetHeroFn)(uintptr_t iface, uintptr_t hero);
static bool ptySehSetHero(uintptr_t iface, uintptr_t hero) {
    __try {
        uintptr_t vft = *(uintptr_t*)iface;
        PtySetHeroFn fn = *(PtySetHeroFn*)(vft + PTY_IFACE_SETHERO_VOFF);
        fn(iface, hero);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

typedef void (*PtyShowSheetFn)(uintptr_t rowRecord);
static bool ptySehShowSheet(uintptr_t base, uintptr_t rowRecord) {
    __try { ((PtyShowSheetFn)(base + PTY_RL_SHOWSHEET_RVA))(rowRecord); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

static void ptyOpenSheet(uintptr_t base, uintptr_t entry, uintptr_t hero) {
    uintptr_t root = resTownRoot(base);
    if (!root) return;
    if (csTownSheetPanel(base)) { postSpeech(axs(AXS_SHEET_ALREADY_OPEN)); return; }
    uintptr_t rl = root + PTY_ROSTERLIST_OFF;
    uintptr_t beg = 0, end = 0, record = 0;
    if (safeReadPtr(rl + PTY_RL_ROWS_BEG_OFF, &beg) &&
        safeReadPtr(rl + PTY_RL_ROWS_END_OFF, &end) && beg && end > beg) {
        int n = (int)((end - beg) / 0x40);
        if (n > 128) n = 128;
        for (int i = 0; i < n; i++) {
            uintptr_t rec = beg + (uintptr_t)i * 0x40;
            uintptr_t rowW = 0, rowEntry = 0;
            if (!safeReadPtr(rec, &rowW) || rowW <= 0x10000) continue;
            if (!safeReadPtr(rowW + PTY_RL_ROW_ENTRY_OFF, &rowEntry)) continue;
            if (rowEntry == entry) { record = rec; break; }
        }
    }
    if (!record) {
        logLine("party: entry %p not found in the roster list rows -> no sheet", (void*)entry);
        postSpeech(axs(AXS_SHEET_DIDNT_OPEN));
        return;
    }
    logLine("party: C -> RosterList::ShowCharacterDisplay(record=%p hero=%p)", (void*)record, (void*)hero);
    if (!ptySehShowSheet(base, record)) {
        logLine("party: RosterList::ShowCharacterDisplay faulted");
        postSpeech(axs(AXS_SHEET_DIDNT_OPEN));
        return;
    }
    g_ptySheetHero = hero;                       // abSelectedHero hands this to the sheet reader
    g_ptySheetWatchUntil = GetTickCount() + 1500;
}

bool ptyWriteArrangement(uintptr_t base, const uintptr_t* iface, const uintptr_t* want,
                         int n, const char* what) {
    for (int i = 0; i < n; i++) ptySehSetHero(iface[i], 0);
    for (int i = 0; i < n; i++) if (want[i]) ptySehSetHero(iface[i], want[i]);
    bool ok = true;
    for (int i = 0; i < n; i++) {
        uintptr_t now = 0;
        safeReadPtr(iface[i] + PTY_IFACE_HERO_OFF, &now);
        if (now != want[i]) {
            logLine("party: %s — position %d reads %p, wanted %p",
                    what, i + 1, (void*)now, (void*)want[i]);
            ok = false;
        }
    }
    logLine("party: %s over %d slots -> %s", what, n, ok ? "observed" : "FAILED");
    return ok;
}

static void ptyReorderDrag(uintptr_t base, int fromSlot, int toSlot, const char* who);

static void ptyCommitReorder(uintptr_t base, int targetRow) {
    int n = g_ptyCount;
    int srcRow = -1;
    for (int i = 0; i < n; i++) if (g_ptyRows[i].hero == g_ptyHoldHero) { srcRow = i; break; }
    if (srcRow < 0) {
        // dismissed, moved by the mouse, or the town rebuilt underneath us
        logLine("party: held hero %p is no longer in the lineup -> reorder cancelled",
                (void*)g_ptyHoldHero);
        g_ptyHoldHero = 0;
        postSpeech(axs(AXS_PTY_REORDER_CANCELLED));
        return;
    }
    char utter[256];
    g_ptyHoldHero = 0;
    if (targetRow == srcRow) {                   // dropped where they already stand
        g_ptyRow = targetRow;
        _snprintf(utter, sizeof utter, axs(AXS_PTY_AT_POSITION_FMT), g_ptyHoldName, ptyPositionOfRow(targetRow));
        utter[sizeof utter - 1] = 0;
        postSpeech(utter);
        return;
    }

    ptyReorderDrag(base, srcRow, targetRow, g_ptyHoldName);
}

// ---- Delete: take the focused slot's hero out of the party, BY DRAGGING them out ----
enum PtyDragKind { PTY_DRAG_NONE = 0, PTY_DRAG_REMOVE, PTY_DRAG_REORDER };
static int       g_ptyDragKind  = PTY_DRAG_NONE;
static uintptr_t g_ptyDragHero  = 0;     // the hero being removed / moved
static uintptr_t g_ptyDragEntry = 0;     // remove: their roster entry
static int       g_ptyDragPos   = 0;     // spoken position (remove: where they left)
static int       g_ptyDragToSlot = 0;    // reorder: the slot asked for
static uintptr_t g_ptyDragWant[PTY_MAX_SLOTS];   // reorder: the arrangement asked for
static int       g_ptyDragWantN = 0;
static char      g_ptyDragName[80];
static DWORD     g_ptyDragUntil = 0;

static void ptyRemoveFromSlot(uintptr_t base, int row) {
    int n = ptyCollect(base, false);
    if (row < 0 || row >= n) return;
    const PtyRow* r = &g_ptyRows[row];
    if (!r->hero) { postSpeech(axs(AXS_PTY_POS_ALREADY_EMPTY)); return; }
    char name[80] = {0}, cls[80] = {0};
    abHeroNameClassOf(base, r->hero, name, sizeof name, cls, sizeof cls);
    const char* who = name[0] ? name : axs(AXS_THE_HERO);
    if (!r->iface) {
        logLine("party: remove at position %d has no strip interface -> refusing",
                ptyPositionOfRow(row));
        postSpeech(axs(AXS_ACTION_FAILED));
        return;
    }
    int64_t dropId = 0;
    uintptr_t drop = feFindElementByFamily(base, BLD_ROSTER_ELEM_FAMILY,
                                           BLD_ROSTER_OWNER_TAG, &dropId);
    float tx = 0, ty = 0;
    if (!drop || !elemCenter(drop, &tx, &ty)) {
        logLine("party: remove %s -- no roster row on screen to drag onto", who);
        diagDumpFocusElements(base, "party-remove-noroster");
        postSpeech(axs(AXS_ACTION_FAILED));
        return;
    }
    uintptr_t src = feGetElementById((int64_t)(PTY_SLOT_ELEM_BASE + (uint32_t)row));
    float sx = 0, sy = 0;
    if (!src || !elemCenter(src, &sx, &sy)) {
        logLine("party: remove %s -- slot element 0x%x is not on screen",
                who, PTY_SLOT_ELEM_BASE + (uint32_t)row);
        diagDumpFocusElements(base, "party-remove-noslot");
        postSpeech(axs(AXS_ACTION_FAILED));
        return;
    }
    char what[160];
    _snprintf(what, sizeof what, "party remove %s from position %d", who, ptyPositionOfRow(row));
    what[sizeof what - 1] = 0;
    if (!synthDragPoints(sx, sy, tx, ty, what)) { postSpeech(axs(AXS_ACTION_FAILED)); return; }
    logLine("party: remove drag %s position %d -> roster row 0x%llx",
            who, ptyPositionOfRow(row), (unsigned long long)dropId);
    g_ptyDragKind  = PTY_DRAG_REMOVE;
    g_ptyDragHero  = r->hero;
    g_ptyDragEntry = r->entry;
    g_ptyDragPos   = ptyPositionOfRow(row);
    strncpy(g_ptyDragName, who, sizeof g_ptyDragName - 1);
    g_ptyDragName[sizeof g_ptyDragName - 1] = 0;
    g_ptyDragUntil = GetTickCount() + 1200;
}

// ---- REARRANGING is a drag too ----
static void ptyReorderDrag(uintptr_t base, int fromSlot, int toSlot, const char* who) {
    uintptr_t iface[PTY_MAX_SLOTS], now[PTY_MAX_SLOTS];
    int n = ptySlotStrip(base, iface, now, PTY_MAX_SLOTS);
    if (n <= 0 || fromSlot < 0 || fromSlot >= n || toSlot < 0 || toSlot >= n) {
        postSpeech(axs(AXS_MOVE_DIDNT_HAPPEN));
        return;
    }
    uintptr_t moved = now[fromSlot];
    if (!moved) { postSpeech(axs(AXS_PTY_EMPTY_SLOT)); return; }
    char what[160];
    _snprintf(what, sizeof what, "party move %s position %d->%d", who,
              ptyPositionOfRow(fromSlot), ptyPositionOfRow(toSlot));
    what[sizeof what - 1] = 0;
    if (!synthDragElements((int64_t)(PTY_SLOT_ELEM_BASE + (uint32_t)fromSlot),
                           (int64_t)(PTY_SLOT_ELEM_BASE + (uint32_t)toSlot), what)) {
        postSpeech(axs(AXS_MOVE_DIDNT_HAPPEN));
        return;
    }
    for (int i = 0; i < n; i++) g_ptyDragWant[i] = now[i];
    uintptr_t t = g_ptyDragWant[toSlot];
    g_ptyDragWant[toSlot]   = g_ptyDragWant[fromSlot];
    g_ptyDragWant[fromSlot] = t;
    g_ptyDragWantN  = n;
    g_ptyDragKind   = PTY_DRAG_REORDER;
    g_ptyDragHero   = moved;
    g_ptyDragEntry  = 0;
    g_ptyDragToSlot = toSlot;
    g_ptyDragPos    = ptyPositionOfRow(toSlot);
    strncpy(g_ptyDragName, who ? who : "", sizeof g_ptyDragName - 1);
    g_ptyDragName[sizeof g_ptyDragName - 1] = 0;
    g_ptyDragUntil  = GetTickCount() + 1200;
}

static void ptyServiceSlotDrag(uintptr_t base) {
    if (!g_ptyDragKind) return;
    uintptr_t iface[PTY_MAX_SLOTS], hero[PTY_MAX_SLOTS];
    int n = ptySlotStrip(base, iface, hero, PTY_MAX_SLOTS);
    char utter[192];

    if (g_ptyDragKind == PTY_DRAG_REMOVE) {
        bool onStrip = false;
        for (int i = 0; i < n; i++) if (hero[i] == g_ptyDragHero) onStrip = true;
        uint32_t state = 1;
        if (g_ptyDragEntry) safeReadU32(g_ptyDragEntry + PTY_ENTRY_STATE_OFF, &state);
        const bool gone = !onStrip && state != 1;
        if (!gone && GetTickCount() <= g_ptyDragUntil) return;
        logLine("party: remove drag %s -> onStrip=%d state=%u = %s",
                g_ptyDragName, onStrip ? 1 : 0, state, gone ? "observed" : "NOT OBSERVED");
        if (gone) _snprintf(utter, sizeof utter, axs(AXS_PTY_LEFT_POS_EMPTY_FMT),
                            g_ptyDragName, g_ptyDragPos);
        else      _snprintf(utter, sizeof utter, axs(AXS_PTY_COULDNT_LEAVE_FMT), g_ptyDragName);
    } else {
        const bool landed = (g_ptyDragToSlot < n && hero[g_ptyDragToSlot] == g_ptyDragHero);
        if (!landed && GetTickCount() <= g_ptyDragUntil) return;
        if (!landed) {
            logLine("party: move drag %s NOT observed -> repairing with the direct write",
                    g_ptyDragName);
            if (n == g_ptyDragWantN)
                ptyWriteArrangement(base, iface, g_ptyDragWant, n, "reorder repair");
            n = ptySlotStrip(base, iface, hero, PTY_MAX_SLOTS);
        } else {
            logLine("party: move drag %s -> observed at position %d",
                    g_ptyDragName, ptyPositionOfRow(g_ptyDragToSlot));
        }
        const bool ok = (g_ptyDragToSlot < n && hero[g_ptyDragToSlot] == g_ptyDragHero);
        if (ok) {
            g_ptyRow = g_ptyDragToSlot;
            _snprintf(utter, sizeof utter, axs(AXS_PTY_AT_POSITION_FMT),
                      g_ptyDragName, ptyPositionOfRow(g_ptyDragToSlot));
        } else {
            _snprintf(utter, sizeof utter, "%s", axs(AXS_MOVE_DIDNT_HAPPEN));
        }
    }
    utter[sizeof utter - 1] = 0;
    g_ptyDragKind  = PTY_DRAG_NONE;
    g_ptyDragHero  = 0;
    g_ptyDragEntry = 0;
    g_ptyDragUntil = 0;
    postSpeech(utter);
}

// ---- TOWN: the party is DISSOLVED on the way home from a raid AND on every save load ----
typedef void (*PtyClearFn)(uintptr_t embarkPanel);

static bool g_ptyRaidSeen     = false;
static bool g_ptyFrontEndSeen = false;
                                        // surface -- town coming up now means a save LOAD

static int ptyInPartyCount(uintptr_t base) {
    uintptr_t campaign = 0, beg = 0, end = 0;
    if (!safeReadPtr(base + RES_CAMPAIGN_RVA, &campaign) || campaign <= 0x10000) return -1;
    if (!safeReadPtr(campaign + PTY_ENTRIES_BEG_OFF, &beg) ||
        !safeReadPtr(campaign + PTY_ENTRIES_END_OFF, &end) || !beg || end <= beg) return -1;
    int n = (int)((end - beg) / 8);
    if (n < 0 || n > 256) return -1;
    int k = 0;
    for (int i = 0; i < n; i++) {
        uintptr_t e = 0;
        uint32_t  st = 0;
        if (!safeReadPtr(beg + (uintptr_t)i * 8, &e) || e <= 0x10000) continue;
        if (safeReadU32(e + PTY_ENTRY_STATE_OFF, &st) && st == 1) k++;
    }
    return k;
}

void checkReturnToTown(uintptr_t base) {
    uintptr_t raid = 0;
    if (safeReadPtr(base + MAP_ROOT_RVA, &raid) && raid > 0x10000) {
        if (!g_ptyRaidSeen) logLine("party: a raid is live -> the return-to-town dissolve is armed");
        g_ptyRaidSeen = true;
        return;
    }
    if (frontEndDisplay(base)) {                 // main menu / save slots: the next town is a LOAD
        if (!g_ptyFrontEndSeen)
            logLine("party: the front end is up -> the load-edge dissolve is armed%s",
                    g_ptyRaidSeen ? " (raid edge handed over)" : "");
        g_ptyRaidSeen     = false;               // whatever town comes next, it is a load
        g_ptyFrontEndSeen = true;
        return;
    }
    if (!g_ptyRaidSeen && !g_ptyFrontEndSeen) return;
    uintptr_t root = resTownRoot(base);
    if (!root) return;                           // results screen / loading: town is not up yet
    const char* why = g_ptyRaidSeen ? "back from a raid" : "save loaded";
    g_ptyRaidSeen     = false;
    g_ptyFrontEndSeen = false;

    int before = ptyInPartyCount(base);
    if (before < 0) {
        logLine("party: %s, but the roster would not read -> dissolve skipped", why);
        return;
    }
    if (before == 0) {
        logLine("party: %s with an empty party -> nothing to dissolve", why);
        return;
    }
    bool called = true;
    __try { ((PtyClearFn)(base + PTY_CLEAR_RVA))(root + PTY_EMBARK_PANEL_OFF); }
    __except (EXCEPTION_EXECUTE_HANDLER) { called = false; }
    int after = ptyInPartyCount(base);
    logLine("party: %s -> clear-party %s, in the party %d -> %d%s",
            why, called ? "called" : "FAULTED", before, after,
            (called && after == 0) ? "" : "   <-- the party did NOT dissolve");
}

void checkParty(uintptr_t base) {
    uintptr_t root = resTownRoot(base);
    if (!root) {
        if (g_ptyActive || g_rosActive) logLine("party: town gone, stood down");
        g_ptyActive = false;
        g_rosActive = false;
        g_ptyHoldHero = 0;
        g_rosHoldEntry = 0;
        g_ptyRow = 0;                            // the next visit starts at the first slot
        g_rosPickSlot = -1;
        g_rosPickAct = false;                    // (buildings' session dies with its panel,
                                                 //  which cannot outlive the town either)
        g_rosOnBar = false;
        g_rosBarCol = 0;
        g_ptySheetWatchUntil = 0;
        g_ptyResume = false;
        g_ptySheetHero = 0;
        g_ptySheetFrom = 0;
        g_ptyDragKind = PTY_DRAG_NONE;           // a slot gesture cannot outlive the town
        g_ptyDragHero = 0;
        g_ptyDragUntil = 0;
        return;
    }
    ptyServiceSlotDrag(base);                    // Delete / a rank move, paid from the strip
    bool sheetOpen = csTownSheetPanel(base) != 0;

    if (g_ptySheetWatchUntil) {
        if (sheetOpen) {                         // the observed flip IS the outcome; the sheet
            g_ptySheetWatchUntil = 0;            // reader announces the title line itself
            g_ptyActive = false;
            g_rosActive = false;
            g_ptyHoldHero = 0;
            g_rosHoldEntry = 0;
            g_ptyResume = true;
            logLine("party: sheet open observed -> stood down, resume armed (from=%d)", g_ptySheetFrom);
        } else if (GetTickCount() > g_ptySheetWatchUntil) {
            g_ptySheetWatchUntil = 0;
            g_ptySheetHero = 0;
            g_ptySheetFrom = 0;
            logLine("party: the sheet-open watch timed out");
            postSpeech(axs(AXS_SHEET_DIDNT_OPEN));
        }
    }

    if ((g_ptyActive || g_rosActive) && sheetOpen && !g_ptyResume) {
        g_ptyActive = false;
        g_rosActive = false;
        g_ptyHoldHero = 0;
        g_rosHoldEntry = 0;
        logLine("party: character sheet took the surface, stood down");
    }

    if (!sheetOpen && g_ptySheetHero && !g_ptySheetWatchUntil) g_ptySheetHero = 0;

    if (g_ptyResume && !sheetOpen) {
        g_ptyResume = false;
        int from = g_ptySheetFrom;
        g_ptySheetFrom = 0;
        if (from == 3) {
            logLine("party: sheet closed -> the trinket pick owns the resume");
            trkPickSheetClosed(base);
        } else if (from == 2) {
            logLine("party: sheet closed -> back to the roster");
            rosResumeFromSheet(base);
        } else {
            g_ptyActive = true;
            logLine("party: sheet closed -> back to the party lineup");
            char hdr[192];
            ptySpeakRow(base, ptyHeader(base, hdr, sizeof hdr));
        }
    }
}

void checkPartyStrip(uintptr_t base) {
    if (!g_ptyActive) return;
    uintptr_t iface[PTY_MAX_SLOTS], hero[PTY_MAX_SLOTS];
    if (ptySlotStrip(base, iface, hero, PTY_MAX_SLOTS) > 0) return;
    ptyLeave(base, "the embark strip is gone");
}

static bool ptyEnter(uintptr_t base) {
    if (!resTownRoot(base)) return false;
    int n = ptyCollect(base, !g_ptyDumped);      // log every slot once per session
    g_ptyDumped = true;
    if (n <= 0 || !g_ptyFromStrip) {
        logLine("party: Tab, but the embark strip is not live -> no lineup here");
        return false;
    }
    g_ptyActive = true;
    g_resActive = false;                         // one town layer at a time
    g_rosActive = false;
    g_ptyRow = 0;
    g_ptyHoldHero = 0;
    logLine("party: lineup active, %d slots, %d filled", n, g_ptyParty);
    char hdr[192];
    ptySpeakRow(base, ptyHeader(base, hdr, sizeof hdr));
    return true;
}

void ptyLeave(uintptr_t base, const char* why) {
    g_ptyActive = false;
    g_ptyHoldHero = 0;
    logLine("party: %s -> stood down to the surface underneath", why);
}

bool routePartyKey(uintptr_t base, uint32_t sym, uint16_t mod, uint8_t repeat) {
    if (mod & (KMOD_LALT | KMOD_RALT | KMOD_LCTRL | KMOD_RCTRL)) return false;

    if (sym == SDLK_ESCAPE) {
        if (g_ptyHoldHero) {                     // one layer per press: first the pickup...
            if (!repeat) {
                g_ptyHoldHero = 0;
                postSpeech(axs(AXS_PTY_REORDER_CANCELLED));
            }
            return true;
        }
        if (repeat) return true;                 // ...then the lineup itself. Landing = the quest
        ptyLeave(base, "Escape");                // list, which re-announces on its own edge.
        return true;
    }

    if (sym == SDLK_c) {
        if (repeat) return true;
        int n = ptyCollect(base, false);
        if (n <= 0) { postSpeech(axs(AXS_PTY_NO_LINEUP)); return true; }
        axStepCursor(&g_ptyRow, n, 0);       // re-clamp against the live lineup
        if (!g_ptyRows[g_ptyRow].hero) { postSpeech(axs(AXS_PTY_EMPTY_SLOT)); return true; }
        g_ptySheetFrom = 1;                      // resume lands back on the lineup
        ptyOpenSheet(base, g_ptyRows[g_ptyRow].entry, g_ptyRows[g_ptyRow].hero);
        return true;
    }

    if (sym == SDLK_RETURN || sym == SDLK_KP_ENTER) {
        if (repeat) return true;
        int n = ptyCollect(base, false);
        if (n <= 0) { postSpeech(axs(AXS_PTY_NO_LINEUP)); return true; }
        axStepCursor(&g_ptyRow, n, 0);       // re-clamp against the live lineup
        rosEnterFromParty(base, g_ptyRows[g_ptyRow].hero, g_ptyRow);
        return true;
    }

    if (sym == SDLK_DELETE || sym == SDLK_BACKSPACE) {
        if (repeat) return true;
        if (g_ptyHoldHero) { postSpeech(axs(AXS_PTY_FINISH_REORDER_FIRST)); return true; }
        int n = ptyCollect(base, false);
        if (n <= 0) { postSpeech(axs(AXS_PTY_NO_LINEUP)); return true; }
        axStepCursor(&g_ptyRow, n, 0);       // re-clamp against the live lineup
        ptyRemoveFromSlot(base, g_ptyRow);
        return true;
    }

    if (sym == SDLK_SPACE) {
        if (repeat) return true;
        int n = ptyCollect(base, false);
        if (n <= 0) { postSpeech(axs(AXS_PTY_NO_LINEUP)); return true; }
        axStepCursor(&g_ptyRow, n, 0);       // re-clamp against the live lineup
        if (!g_ptyHoldHero) {                    // pick up
            const PtyRow* r = &g_ptyRows[g_ptyRow];
            if (!r->hero) { postSpeech(axs(AXS_PTY_EMPTY_SLOT)); return true; }
            char name[80] = {0}, cls[80] = {0};
            abHeroNameClassOf(base, r->hero, name, sizeof name, cls, sizeof cls);
            strncpy(g_ptyHoldName, name[0] ? name : axs(AXS_PTY_HELD_HERO_FALLBACK), sizeof g_ptyHoldName - 1);
            g_ptyHoldName[sizeof g_ptyHoldName - 1] = 0;
            g_ptyHoldHero = r->hero;
            char utter[128];
            _snprintf(utter, sizeof utter, axs(AXS_PTY_REORDERING_FMT), g_ptyHoldName);
            utter[sizeof utter - 1] = 0;
            postSpeech(utter);
        } else {                                 // place
            ptyCommitReorder(base, g_ptyRow);
        }
        return true;
    }

    int jump = 0;
    if (axDecodeJump(sym, mod, repeat, &jump)) {     // Home/End: first/last lineup slot
        if (!jump) return true;                      // held jump: one landing per press
        int n = ptyCollect(base, false);
        if (n <= 0) { postSpeech(axs(AXS_PTY_NO_LINEUP)); return true; }
        axStepCursor(&g_ptyRow, n, jump);            // the clamp lands it; ends re-read
        ptySpeakRow(base, nullptr);
        return true;
    }

    if (sym == SDLK_LEFT || sym == SDLK_RIGHT) {
        if (axNavHoldRepeat(repeat)) return true;
        int n = ptyCollect(base, false);
        if (n <= 0) { postSpeech(axs(AXS_PTY_NO_LINEUP)); return true; }
        axStepCursor(&g_ptyRow, n, 0);       // re-clamp against the live lineup
        axStepCursor(&g_ptyRow, n, sym == SDLK_RIGHT ? 1 : -1);   // hard stop: re-read the end
        ptySpeakRow(base, nullptr);
        return true;
    }

    return false;
}

// ---- TOWN: the ROSTER LIST (all heroes, AX_ROSTER) ----
static const uintptr_t ROS_ENTRY_BUILDING_OFF = 0x1558;
static const uintptr_t ROS_ENTRY_MISSING_OFF  = 0x1598;
static const uintptr_t ROS_ENTRY_ACTIVITY_OFF = 0x1550; // Roster::Entry+: Activity* while state == 2
static const uintptr_t ROS_REORDER_GATE_OFF   = 0x5c;
static const uintptr_t ROS_SORT_TYPE_OFF      = 0x1f4;    // rosterList+: int, the current eSortType
static const uintptr_t ROS_SORT_DIR_OFF       = 0x1f8;    // rosterList+: int, 0/1 = which direction

struct RosSort {
    int         type;
    uintptr_t   vftRva;        // ShowSortByX::lambda_1 impl vftable
    const char* locKey;
    const char* logName;       // LOG only (logs never localize)
    AxStrId     fbWord;
};
static const RosSort kRosSorts[] = {
    { 0, ROS_SORT_LEVEL_VFT,    "str_sort_roster_by_level",    "level",    AXS_ROS_SORT_LEVEL    },
    { 1, ROS_SORT_STRESS_VFT,   "str_sort_roster_by_stress",   "stress",   AXS_ROS_SORT_STRESS   },
    { 2, ROS_SORT_CLASS_VFT,    "str_sort_roster_by_class",    "class",    AXS_ROS_SORT_CLASS    },
    { 3, ROS_SORT_BUILDING_VFT, "str_sort_roster_by_building", "activity", AXS_ROS_SORT_ACTIVITY },
};
static const int ROS_SORT_COUNT = (int)(sizeof kRosSorts / sizeof kRosSorts[0]);
static const int ROS_BAR_ITEMS  = 1 + ROS_SORT_COUNT;   // capacity, then the sorts

struct RosRow {
    uintptr_t entry;       // the Roster::Entry
    uintptr_t hero;        // entry+0x08
    uintptr_t rowW;
};
static RosRow g_rosRows[ROS_MAX_ROWS];
static int    g_rosCount  = 0;
static int    g_rosRow    = 0;
static bool   g_rosDumped = false;

bool axIsRoster() { return g_rosActive; }

static int rosCollect(uintptr_t base, bool probe) {
    g_rosCount = 0;
    uintptr_t root = resTownRoot(base);
    if (!root) return 0;
    uintptr_t rl = root + PTY_ROSTERLIST_OFF;
    uintptr_t beg = 0, end = 0;
    if (!safeReadPtr(rl + PTY_RL_ROWS_BEG_OFF, &beg) ||
        !safeReadPtr(rl + PTY_RL_ROWS_END_OFF, &end) || !beg || end <= beg) return 0;
    int n = (int)((end - beg) / 0x40);
    if (n > ROS_MAX_ROWS) n = ROS_MAX_ROWS;
    for (int i = 0; i < n; i++) {
        uintptr_t rec = beg + (uintptr_t)i * 0x40;
        uintptr_t rowW = 0, entry = 0;
        if (!safeReadPtr(rec, &rowW) || rowW <= 0x10000) continue;
        if (!safeReadPtr(rowW + PTY_RL_ROW_ENTRY_OFF, &entry) || entry <= 0x10000) continue;
        g_rosRows[g_rosCount].entry = entry;
        g_rosRows[g_rosCount].hero  = entry + PTY_ENTRY_HERO_OFF;
        g_rosRows[g_rosCount].rowW  = rowW;
        if (probe) {
            char name[80] = {0}, bld[48] = {0};
            uint32_t state = 0, missing = 0;
            safeReadCStr(g_rosRows[g_rosCount].hero + HERO_NAME_OFF, name, sizeof name);
            safeReadU32(entry + PTY_ENTRY_STATE_OFF, &state);
            safeReadCStr(entry + ROS_ENTRY_BUILDING_OFF, bld, sizeof bld);
            safeReadU32(entry + ROS_ENTRY_MISSING_OFF, &missing);
            logLine("roster probe: row %d entry=%p \"%s\" state=%u building=\"%s\" missing=%u",
                    g_rosCount, (void*)entry, name, state, bld, missing);
        }
        g_rosCount++;
    }
    return g_rosCount;
}

static void rosStatusFrag(uintptr_t base, uintptr_t entry, char* out, int outsz) {
    out[0] = 0;
    uint32_t state = 0;
    if (!safeReadU32(entry + PTY_ENTRY_STATE_OFF, &state)) {
        _snprintf(out, outsz, " %s", axs(AXS_ROS_ACTIVITY_UNREADABLE));
        out[outsz - 1] = 0;
        logLine("roster: entry %p state read faulted", (void*)entry);
        return;
    }
    switch (state) {
    case 0:
        _snprintf(out, outsz, " %s", axs(AXS_ROS_IDLE));
        break;
    case 1:
        _snprintf(out, outsz, " %s", axs(AXS_ROS_IN_PARTY));
        break;
    case 2: {                                  // committed to an activity this week: the
        uintptr_t act = 0;                     // game's own activity name, bare
        char name[96] = {0};
        if (safeReadPtr(entry + ROS_ENTRY_ACTIVITY_OFF, &act) &&
            bldActivityNameFromPtr(base, act, name, sizeof name)) {
            _snprintf(out, outsz, " %s.", name);
        } else {
            _snprintf(out, outsz, " %s", axs(AXS_ROS_IN_ACTIVITY));
            logLine("roster: entry %p state 2 but the Activity* (%p) would not name itself",
                    (void*)entry, (void*)act);
        }
        break;
    }
    case 3:
        _snprintf(out, outsz, " %s", axs(AXS_ROS_DEAD));
        break;
    case 4:
        _snprintf(out, outsz, " %s", axs(AXS_ROS_MISSING));
        break;
    default:
        _snprintf(out, outsz, " %s", axs(AXS_ROS_UNAVAILABLE));
        logLine("roster: entry %p has unknown state %u -- spoken as \"Unavailable\"",
                (void*)entry, state);
        break;
    }
    out[outsz - 1] = 0;
}

static uintptr_t rosPanel(uintptr_t base) {
    uintptr_t root = resTownRoot(base);
    return root ? root + PTY_ROSTERLIST_OFF : 0;
}

static void rosCapacityLine(uintptr_t base, char* out, int outsz) {
    int count = -1, max = -1;
    if (!bldSehRosterCounts(base, &count, &max) || count < 0 || max <= 0) {
        _snprintf(out, outsz, "%s", axs(AXS_ROS_CAP_UNREADABLE));
        out[outsz - 1] = 0;
        logLine("roster: the capacity getters faulted or read nonsense (%d/%d)", count, max);
        return;
    }
    if (count >= max) {
        char full[128];
        if (!(resolveKey(base, "str_roster_list_full", full, sizeof full) && full[0])) {
            _snprintf(full, sizeof full, "%s", axs(AXS_ROS_FULL_FALLBACK));
            full[sizeof full - 1] = 0;
        }
        _snprintf(out, outsz, axs(AXS_ROS_CAPACITY_FULL_FMT), count, max, full);
    } else {
        _snprintf(out, outsz, axs(AXS_ROS_CAPACITY_FMT), count, max);
    }
    out[outsz - 1] = 0;
}

static void rosSortLine(uintptr_t base, int idx, char* out, int outsz) {
    const RosSort* s = &kRosSorts[idx];
    char label[128] = {0};
    if (!resolveKey(base, s->locKey, label, sizeof label) || !label[0]) {
        logLine("roster: sort key \"%s\" did not resolve", s->locKey);
        _snprintf(label, sizeof label, axs(AXS_ROS_SORT_FALLBACK_FMT), axs(s->fbWord));
    }
    uint32_t cur = 0xffffffff;
    uintptr_t panel = rosPanel(base);
    if (panel) safeReadU32(panel + ROS_SORT_TYPE_OFF, &cur);
    if ((int)cur == s->type) _snprintf(out, outsz, "%s. %s", label, axs(AXS_ROS_CURRENT_SORT));
    else                     _snprintf(out, outsz, "%s.", label);
    out[outsz - 1] = 0;
}

static void rosSpeakBar(uintptr_t base, const char* prefix) {
    if (g_rosBarCol < 0) g_rosBarCol = 0;
    if (g_rosBarCol >= ROS_BAR_ITEMS) g_rosBarCol = ROS_BAR_ITEMS - 1;
    char item[256];
    if (g_rosBarCol == 0) rosCapacityLine(base, item, sizeof item);
    else                  rosSortLine(base, g_rosBarCol - 1, item, sizeof item);
    char utter[MAILBOX_SZ];
    _snprintf(utter, sizeof utter, "%s%s", prefix ? prefix : "", item);
    utter[sizeof utter - 1] = 0;
    postSpeech(utter);
}

static void rosSpeakRow(uintptr_t base, const char* prefix) {
    if (g_rosOnBar) { rosSpeakBar(base, prefix); return; }
    int n = rosCollect(base, false);
    if (n <= 0) { postSpeech(axs(AXS_ROS_NO_ROSTER)); return; }
    axStepCursor(&g_rosRow, n, 0);           // re-clamp against the live roster
    const RosRow* r = &g_rosRows[g_rosRow];
    char card[512], status[160], posln[64];
    ptyHeroFragEx(base, r->hero, card, sizeof card, true, false);
    rosStatusFrag(base, r->entry, status, sizeof status);
    char actFrag[192] = {0};
    if (g_rosPickAct) bldActPickRowSuffix(base, r->hero, actFrag, sizeof actFrag);
    _snprintf(posln, sizeof posln, axs(AXS_POS_N_OF_M), g_rosRow + 1, n);
    posln[sizeof posln - 1] = 0;
    char utter[MAILBOX_SZ];
    _snprintf(utter, sizeof utter, "%s%s.%s%s %s", prefix ? prefix : "",
              card, status, actFrag, posln);
    utter[sizeof utter - 1] = 0;
    postSpeech(utter);
}

void rosReannounce(uintptr_t base) {
    if (!g_rosActive) return;
    if (g_rosPickAct && bldActPickBusy()) {
        logLine("roster: modal closed mid-pick-watch -- leaving the announcement to the watch");
        return;
    }
    logLine("roster: re-announcing after a modal closed");
    char pfx[128];
    _snprintf(pfx, sizeof pfx, "%s ", axs((g_rosPickSlot >= 0 || g_rosPickAct)
                                          ? AXS_ROS_CHOOSE_HERO : AXS_ROS_TITLE));
    pfx[sizeof pfx - 1] = 0;
    rosSpeakRow(base, pfx);
}

static void rosEnterFromParty(uintptr_t base, uintptr_t hero, int pickSlot) {
    int n = rosCollect(base, !g_rosDumped);
    g_rosDumped = true;
    if (n <= 0) { postSpeech(axs(AXS_ROS_NO_ROSTER)); return; }
    g_rosActive = true;
    g_ptyActive = false;
    g_resActive = false;
    g_rosPickSlot = pickSlot;
    g_rosPickAct = false;                    // the two pick modes are exclusive by construction
    g_rosOnBar = false;
    g_rosBarCol = 0;
    g_rosRow = 0;
    if (hero) {
        for (int i = 0; i < n; i++) if (g_rosRows[i].hero == hero) { g_rosRow = i; break; }
    }
    logLine("roster: active, %d heroes, row %d, pickSlot=%d", n, g_rosRow, pickSlot);
    char head[256];
    if (pickSlot >= 0) {
        char line[192];
        _snprintf(line, sizeof line, axs(AXS_ROS_CHOOSE_FOR_POS_FMT), g_ptyCount - pickSlot);
        line[sizeof line - 1] = 0;
        _snprintf(head, sizeof head, "%s ", line);
    } else {
        char cap[192];
        rosCapacityLine(base, cap, sizeof cap);
        _snprintf(head, sizeof head, "%s ", cap);
    }
    head[sizeof head - 1] = 0;
    rosSpeakRow(base, head);
}

static bool rosEnterFromTown(uintptr_t base) {
    if (!resTownRoot(base)) return false;
    rosEnterFromParty(base, 0, -1);
    return g_rosActive;
}

void rosLeave(uintptr_t base, const char* why) {
    g_rosActive = false;
    g_rosPickSlot = -1;
    g_rosPickAct = false;
    g_rosHoldEntry = 0;                          // a held hero never survives leaving the surface
    logLine("roster: %s -> stood down to the surface underneath", why);
}

// ---- The ACTIVITY PICK doors (2026-09-01, player's design: "move focus to the roster and let ----
bool rosEnterForActivityPick(uintptr_t base, const char* header) {
    int n = rosCollect(base, !g_rosDumped);
    g_rosDumped = true;
    if (n <= 0) { postSpeech(axs(AXS_ROS_NO_ROSTER)); return false; }   // said something; the
    g_rosActive = true;                                                 // caller stays silent
    g_ptyActive = false;
    g_resActive = false;
    g_rosPickSlot = -1;
    g_rosPickAct = true;
    g_rosOnBar = false;
    g_rosBarCol = 0;
    g_rosRow = 0;
    logLine("roster: active for an ACTIVITY pick, %d heroes", n);
    char pfx[192];
    _snprintf(pfx, sizeof pfx, "%s ", header ? header : axs(AXS_ROS_CHOOSE_HERO));
    pfx[sizeof pfx - 1] = 0;
    rosSpeakRow(base, pfx);
    return true;
}

bool rosActivityPickLive() { return g_rosPickAct; }

void rosSpeakRowPrefixed(uintptr_t base, const char* prefix) {
    if (!g_rosActive) return;
    rosSpeakRow(base, prefix);
}

static void rosResumeFromSheet(uintptr_t base) {
    if (!resTownRoot(base)) return;
    g_rosActive = true;
    char pfx[128];
    _snprintf(pfx, sizeof pfx, "%s ", axs((g_rosPickSlot >= 0 || g_rosPickAct)
                                          ? AXS_ROS_CHOOSE_HERO : AXS_ROS_TITLE));
    pfx[sizeof pfx - 1] = 0;
    rosSpeakRow(base, pfx);
}

typedef void (*RosSortByFn)(uintptr_t list, uint32_t type, void* fn);
static bool rosSehSortBy(uintptr_t base, uintptr_t list, const RosSort* s) {
    __try {
        uint8_t fn[0x40];
        memset(fn, 0, sizeof fn);
        *(uintptr_t*)(fn + 0x00) = base + s->vftRva;      // the game's own comparator impl
        *(uintptr_t*)(fn + 0x08) = list;                  // ... which captures the list
        *(uintptr_t*)(fn + 0x38) = (uintptr_t)fn;         // _Ptr -> the inline impl
        ((RosSortByFn)(base + ROS_SORTBY_RVA))(list, (uint32_t)s->type, fn);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

static void rosApplySort(uintptr_t base, int idx) {
    const RosSort* s = &kRosSorts[idx];
    uintptr_t list = rosPanel(base);
    if (!list) { postSpeech(axs(AXS_ROS_NO_ROSTER)); return; }

    // The row order BEFORE, so the outcome is observed and not assumed.
    int n = rosCollect(base, false);
    uintptr_t before[ROS_MAX_ROWS];
    for (int i = 0; i < n; i++) before[i] = g_rosRows[i].entry;
    uintptr_t focus = (!g_rosOnBar && g_rosRow < n) ? g_rosRows[g_rosRow].entry : 0;

    uint32_t curType = 0xffffffff, dir = 0;
    safeReadU32(list + ROS_SORT_TYPE_OFF, &curType);
    if ((int)curType != s->type) safeWriteU32(list + ROS_SORT_DIR_OFF, 0);  // the click body's line 1
    bool called = rosSehSortBy(base, list, s);
    safeReadU32(list + ROS_SORT_DIR_OFF, &dir);
    safeWriteU32(list + ROS_SORT_DIR_OFF, (uint32_t)(((int)dir - 1) & 1));  // ... and its line 3

    int n2 = rosCollect(base, false);
    bool moved = (n2 != n);
    for (int i = 0; i < n2 && i < n && !moved; i++) if (g_rosRows[i].entry != before[i]) moved = true;
    uint32_t nowType = 0xffffffff;
    safeReadU32(list + ROS_SORT_TYPE_OFF, &nowType);
    logLine("roster: sort %s call=%d type %u -> %u, dir -> %u, order %s",
            s->logName, called ? 1 : 0, curType, nowType, dir,
            moved ? "changed" : "unchanged");

    if (!called || (int)nowType != s->type) {
        postSpeech(axs(AXS_ROS_SORT_DIDNT_HAPPEN));
        return;
    }
    // Keep the cursor on the hero it was on; the list moved underneath it.
    if (focus) for (int i = 0; i < n2; i++) if (g_rosRows[i].entry == focus) { g_rosRow = i; break; }

    char label[128] = {0};
    if (!resolveKey(base, s->locKey, label, sizeof label) || !label[0])
        _snprintf(label, sizeof label, axs(AXS_ROS_SORTED_BY_FALLBACK_FMT), axs(s->fbWord));
    char first[512] = {0}, firstAct[160] = {0};
    if (n2 > 0) {
        ptyHeroFragEx(base, g_rosRows[0].hero, first, sizeof first, true, false);
        rosStatusFrag(base, g_rosRows[0].entry, firstAct, sizeof firstAct);
    }
    char utter[MAILBOX_SZ];
    if (first[0]) _snprintf(utter, sizeof utter, axs(AXS_ROS_SORTED_FIRST_FMT), label, first, firstAct);
    else          _snprintf(utter, sizeof utter, "%s.", label);
    utter[sizeof utter - 1] = 0;
    postSpeech(utter);
}

typedef void (*RosSetStateFn)(uintptr_t system, uintptr_t entry, int state);
static bool rosSehSetState(uintptr_t base, uintptr_t system, uintptr_t entry, int state) {
    __try { ((RosSetStateFn)(base + ROS_SETSTATE_RVA))(system, entry, state); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

typedef void (*RosDropFn)(uintptr_t* panelPtr, uintptr_t rowWidget);
static bool rosSehDropIntoParty(uintptr_t base, uintptr_t panel, uintptr_t rowWidget) {
    __try {
        uintptr_t p = panel;
        ((RosDropFn)(base + PTY_DROP_RVA))(&p, rowWidget);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

static uintptr_t rosRowWidgetOf(uintptr_t base, uintptr_t entry) {
    uintptr_t rl = rosPanel(base), beg = 0, end = 0;
    if (!rl || !safeReadPtr(rl + PTY_RL_ROWS_BEG_OFF, &beg) ||
        !safeReadPtr(rl + PTY_RL_ROWS_END_OFF, &end) || !beg || end <= beg) return 0;
    int n = (int)((end - beg) / 0x40);
    if (n > ROS_MAX_ROWS) n = ROS_MAX_ROWS;
    for (int i = 0; i < n; i++) {
        uintptr_t rowW = 0, e = 0;
        if (!safeReadPtr(beg + (uintptr_t)i * 0x40, &rowW) || rowW <= 0x10000) continue;
        if (safeReadPtr(rowW + PTY_RL_ROW_ENTRY_OFF, &e) && e == entry) return rowW;
    }
    return 0;
}

static bool rosAssignToSlot(uintptr_t base, int row, int slot) {
    const RosRow* r = &g_rosRows[row];
    uintptr_t entry = r->entry, hero = r->hero, campaign = 0;
    char name[80] = {0}, cls[80] = {0};
    abHeroNameClassOf(base, hero, name, sizeof name, cls, sizeof cls);
    const char* who = name[0] ? name : axs(AXS_THE_HERO);
    if (!safeReadPtr(base + RES_CAMPAIGN_RVA, &campaign) || campaign <= 0x10000) return false;

    uint32_t before = 0;
    safeReadU32(entry + PTY_ENTRY_STATE_OFF, &before);
    char utter[256];

    // ---- already in the party ----
    if (before == 1) {
        uintptr_t sIface[PTY_MAX_SLOTS], sHero[PTY_MAX_SLOTS];
        int sn = ptySlotStrip(base, sIface, sHero, PTY_MAX_SLOTS);
        int at = -1;
        for (int i = 0; i < sn; i++) if (sHero[i] == hero) { at = i; break; }

        // In some OTHER slot: this is a MOVE to the slot the list was opened for.
        if (at >= 0 && at != slot) {
            logLine("roster: pick moves %s position %d -> %d",
                    who, ptyPositionOfRow(at), ptyPositionOfRow(slot));
            ptyReorderDrag(base, at, slot, who);
            return true;                         // ptyServiceSlotDrag speaks the outcome
        }
        // In THIS slot (or in the party but off the strip): take them out.
        if (at == slot) {
            logLine("roster: pick removes %s from position %d", who, ptyPositionOfRow(at));
            ptyRemoveFromSlot(base, at);
            return true;                         // ptyServiceSlotDrag speaks the outcome
        }
        bool called = rosSehSetState(base, campaign, entry, 0);
        uint32_t now = 1;
        safeReadU32(entry + PTY_ENTRY_STATE_OFF, &now);
        logLine("roster: pick removes %s (no slot on the strip) call=%d state %u -> %u",
                who, called ? 1 : 0, before, now);
        if (now != 1) _snprintf(utter, sizeof utter, axs(AXS_ROS_LEFT_PARTY_FMT), who);
        else          _snprintf(utter, sizeof utter, axs(AXS_PTY_COULDNT_LEAVE_FMT), who);
        utter[sizeof utter - 1] = 0;
        postSpeech(utter);
        return true;
    }

    // ---- not in the party: ADD, then PLACE ----
    uintptr_t root = resTownRoot(base);
    uintptr_t rowW = rosRowWidgetOf(base, entry);
    if (!root || !rowW) {
        logLine("roster: pick — entry %p has no row widget (root=%p) -> refusing",
                (void*)entry, (void*)root);
        postSpeech(axs(AXS_ACTION_FAILED));
        return false;
    }
    int n = ptyCollect(base, false);
    if (n > 0 && slot >= 0 && slot < n && g_ptyRows[slot].hero) {
        if (g_ptyRows[slot].iface) ptySehSetHero(g_ptyRows[slot].iface, 0);
        if (g_ptyRows[slot].entry) rosSehSetState(base, campaign, g_ptyRows[slot].entry, 0);
        logLine("roster: pick — position %d emptied to make room", g_ptyCount - slot);
    }
    bool called = rosSehDropIntoParty(base, root + PTY_EMBARK_PANEL_OFF, rowW);
    uint32_t now = 0;
    safeReadU32(entry + PTY_ENTRY_STATE_OFF, &now);
    logLine("roster: pick adds %s call=%d state %u -> %u (slot %d = position %d)",
            who, called ? 1 : 0, before, now, slot, g_ptyCount - slot);
    if (now != 1) {
        _snprintf(utter, sizeof utter, axs(AXS_ROS_CANT_JOIN_FMT), who);
        utter[sizeof utter - 1] = 0;
        postSpeech(utter);
        return true;
    }

    n = ptyCollect(base, false);
    int at = -1;
    for (int i = 0; i < n; i++) if (g_ptyRows[i].hero == hero) { at = i; break; }
    if (at >= 0 && slot >= 0 && slot < n && at != slot) {
        uintptr_t iface[PTY_MAX_SLOTS], want[PTY_MAX_SLOTS];
        if (ptyPositionIfaces(base, iface, n) == n) {
            for (int i = 0; i < n; i++) want[i] = g_ptyRows[i].hero;
            uintptr_t t = want[slot]; want[slot] = want[at]; want[at] = t;
            char what[128];
            _snprintf(what, sizeof what, "place %s position %d->%d", who,
                      ptyPositionOfRow(at), ptyPositionOfRow(slot));
            what[sizeof what - 1] = 0;
            ptyWriteArrangement(base, iface, want, n, what);
        }
    }
    // The OBSERVED position, whatever the arrangement write actually achieved.
    n = ptyCollect(base, false);
    int landed = -1;
    for (int i = 0; i < n; i++) if (g_ptyRows[i].hero == hero) { landed = i; break; }
    if (landed >= 0) {
        g_ptyRow = landed;
        _snprintf(utter, sizeof utter, axs(AXS_ROS_JOINED_POS_FMT),
                  who, ptyPositionOfRow(landed));
    } else {
        _snprintf(utter, sizeof utter, axs(AXS_ROS_JOINED_FMT), who);
    }
    utter[sizeof utter - 1] = 0;
    postSpeech(utter);
    return true;
}

// ---- TOWN: REARRANGING THE ROSTER BY HAND (Space picks up, Space drops) ----
static const uintptr_t ROS_REC_STRIDE   = 0x40;  // one row record
static const uintptr_t ROS_REC_ANIM_OFF = 0x20;  // record+: the drop zeroes this...
static const uintptr_t ROS_REC_TWEEN_OFF= 0x30;

static int rosRecIndexOf(uintptr_t base, uintptr_t entry, int* countOut) {
    if (countOut) *countOut = 0;
    uintptr_t rl = rosPanel(base), beg = 0, end = 0;
    if (!rl || !safeReadPtr(rl + PTY_RL_ROWS_BEG_OFF, &beg) ||
        !safeReadPtr(rl + PTY_RL_ROWS_END_OFF, &end) || !beg || end <= beg) return -1;
    int n = (int)((end - beg) / ROS_REC_STRIDE);
    if (n <= 0 || n > ROS_MAX_ROWS) return -1;
    if (countOut) *countOut = n;
    for (int i = 0; i < n; i++) {
        uintptr_t rowW = 0, e = 0;
        if (!safeReadPtr(beg + (uintptr_t)i * ROS_REC_STRIDE, &rowW) || rowW <= 0x10000) continue;
        if (safeReadPtr(rowW + PTY_RL_ROW_ENTRY_OFF, &e) && e == entry) return i;
    }
    return -1;
}

static bool rosSehRotateRecord(uintptr_t beg, int src, int dst) {
    __try {
        uint8_t rec[ROS_REC_STRIDE];
        uint8_t* v = (uint8_t*)beg;
        memcpy(rec, v + (size_t)src * ROS_REC_STRIDE, ROS_REC_STRIDE);
        if (dst > src) memmove(v + (size_t)src * ROS_REC_STRIDE,
                               v + (size_t)(src + 1) * ROS_REC_STRIDE,
                               (size_t)(dst - src) * ROS_REC_STRIDE);
        else           memmove(v + (size_t)(dst + 1) * ROS_REC_STRIDE,
                               v + (size_t)dst * ROS_REC_STRIDE,
                               (size_t)(src - dst) * ROS_REC_STRIDE);
        memcpy(v + (size_t)dst * ROS_REC_STRIDE, rec, ROS_REC_STRIDE);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

typedef void (*RosListOrderFn)(uintptr_t list);
static bool rosSehListOrder(uintptr_t base, uintptr_t list) {
    __try { ((RosListOrderFn)(base + ROS_LISTORDER_RVA))(list); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

static int rosCampaignIndexOf(uintptr_t base, uintptr_t entry) {
    uintptr_t campaign = 0, beg = 0, end = 0;
    if (!safeReadPtr(base + RES_CAMPAIGN_RVA, &campaign) || campaign <= 0x10000) return -1;
    if (!safeReadPtr(campaign + PTY_ENTRIES_BEG_OFF, &beg) ||
        !safeReadPtr(campaign + PTY_ENTRIES_END_OFF, &end) || !beg || end <= beg) return -1;
    int n = (int)((end - beg) >> 3);
    if (n <= 0 || n > 0x400) return -1;
    for (int i = 0; i < n; i++) {
        uintptr_t e = 0;
        if (safeReadPtr(beg + (uintptr_t)i * 8, &e) && e == entry) return i;
    }
    return -1;
}

static void rosCommitMove(uintptr_t base, int targetRow) {
    uintptr_t entry = g_rosHoldEntry;
    g_rosHoldEntry = 0;                          // one press, one attempt, whatever happens

    uintptr_t list = rosPanel(base);
    int n = rosCollect(base, false);
    if (!list || n <= 0) { postSpeech(axs(AXS_ROS_NO_ROSTER)); return; }
    if (targetRow < 0) targetRow = 0;
    if (targetRow >= n) targetRow = n - 1;

    int recCount = 0;
    int src = rosRecIndexOf(base, entry, &recCount);
    int dst = rosRecIndexOf(base, g_rosRows[targetRow].entry, nullptr);
    if (src < 0 || dst < 0) {
        logLine("roster: move, src rec %d dst rec %d (entry=%p target=%p) -> cancelled",
                src, dst, (void*)entry, (void*)g_rosRows[targetRow].entry);
        postSpeech(axs(AXS_ROS_MOVE_NO_ROW));
        return;
    }

    uint32_t gate = 0xffffffff;                  // m_DisableReorderCount -- logged, never a guard
    uintptr_t campaign = 0;
    if (safeReadPtr(base + RES_CAMPAIGN_RVA, &campaign) && campaign > 0x10000)
        safeReadU32(campaign + ROS_REORDER_GATE_OFF, &gate);
    int campBefore = rosCampaignIndexOf(base, entry);

    if (src == dst) {                            // dropped where they already stand
        g_rosRow = targetRow;
        char same[MAILBOX_SZ];
        _snprintf(same, sizeof same, axs(AXS_ROS_MOVED_TO_FMT), g_rosHoldName, targetRow + 1, n);
        same[sizeof same - 1] = 0;
        postSpeech(same);
        return;
    }

    uintptr_t beg = 0;
    if (!safeReadPtr(list + PTY_RL_ROWS_BEG_OFF, &beg) || !beg ||
        !rosSehRotateRecord(beg, src, dst)) {
        logLine("roster: move %s rec %d -> %d, the record rotate FAULTED", g_rosHoldName, src, dst);
        postSpeech(axs(AXS_MOVE_DIDNT_HAPPEN));
        return;
    }
    safeWriteU32(beg + (uintptr_t)dst * ROS_REC_STRIDE + ROS_REC_ANIM_OFF, 0);
    safeWriteU32(beg + (uintptr_t)dst * ROS_REC_STRIDE + ROS_REC_TWEEN_OFF, 0x3e800000);
    bool wrote = rosSehListOrder(base, list);    // ... and the write-through to the roster itself

    // OBSERVED outcome, from both sides: the panel's rows and the campaign's vector.
    int n2 = rosCollect(base, false);
    int nowRow = -1;
    for (int i = 0; i < n2; i++) if (g_rosRows[i].entry == entry) { nowRow = i; break; }
    int campAfter = rosCampaignIndexOf(base, entry);
    logLine("roster: move %s rec %d -> %d (row %d of %d), listorder call=%d, campaign idx %d -> %d, "
            "reorder gate=%d",
            g_rosHoldName, src, dst, nowRow + 1, n2, wrote ? 1 : 0, campBefore, campAfter, (int)gate);

    if (nowRow < 0) { postSpeech(axs(AXS_MOVE_DIDNT_HAPPEN)); return; }
    g_rosRow = nowRow;
    char utter[MAILBOX_SZ];
    _snprintf(utter, sizeof utter, axs(AXS_ROS_MOVED_TO_FMT), g_rosHoldName, nowRow + 1, n2);
    utter[sizeof utter - 1] = 0;
    postSpeech(utter);
}

bool routeRosterKey(uintptr_t base, uint32_t sym, uint16_t mod, uint8_t repeat) {
    if (mod & (KMOD_LALT | KMOD_RALT | KMOD_LCTRL | KMOD_RCTRL)) return false;

    if (sym == SDLK_ESCAPE) {
        if (g_rosHoldEntry) {                    // one layer per press: first the pickup...
            if (!repeat) {
                g_rosHoldEntry = 0;
                postSpeech(axs(AXS_ROS_MOVE_CANCELLED));
            }
            return true;
        }
        if (repeat) return true;
        if (g_rosPickAct) {                      // cancel the ACTIVITY pick, back to the slot row
            if (bldActPickBusy()) { postSpeech(axs(AXS_STILL_WORKING)); return true; }
            rosLeave(base, "Escape (activity pick cancelled)");
            bldActPickCancelFromRoster(base);    // drops the session, speaks "Cancelled." + row
            return true;
        }
        if (g_rosPickSlot >= 0) {                // cancel the pick, back to the slot it came from
            int slot = g_rosPickSlot;
            rosLeave(base, "Escape (pick cancelled)");
            g_ptyActive = true;
            g_ptyRow = slot;
            char hdr[192];
            ptySpeakRow(base, ptyHeader(base, hdr, sizeof hdr));
            return true;
        }
        rosLeave(base, "Escape");
        return true;
    }

    if (sym == SDLK_c) {
        if (repeat) return true;
        if (g_rosHoldEntry) { postSpeech(axs(AXS_ROS_FINISH_MOVE_FIRST)); return true; }
        if (g_rosOnBar) { postSpeech(axs(AXS_ROS_NO_HERO_HERE)); return true; }
        if (g_rosPickAct && bldActPickBusy()) { postSpeech(axs(AXS_STILL_WORKING)); return true; }
        int n = rosCollect(base, false);
        if (n <= 0) { postSpeech(axs(AXS_ROS_NO_ROSTER)); return true; }
        axStepCursor(&g_rosRow, n, 0);       // re-clamp against the live roster
        g_ptySheetFrom = 2;                      // resume lands back on the roster
        ptyOpenSheet(base, g_rosRows[g_rosRow].entry, g_rosRows[g_rosRow].hero);
        return true;
    }

    if (sym == SDLK_RETURN || sym == SDLK_KP_ENTER) {
        if (repeat) return true;
        if (g_rosHoldEntry) { postSpeech(axs(AXS_ROS_FINISH_MOVE_FIRST)); return true; }
        if (g_rosOnBar) {                        // the toolbar: capacity re-reads, a sort applies
            if (g_rosBarCol == 0) rosSpeakBar(base, nullptr);
            else                  rosApplySort(base, g_rosBarCol - 1);
            return true;
        }
        int n = rosCollect(base, false);
        if (n <= 0) { postSpeech(axs(AXS_ROS_NO_ROSTER)); return true; }
        axStepCursor(&g_rosRow, n, 0);       // re-clamp against the live roster
        if (g_rosPickAct) {                  // ACTIVITY pick: hand the row to the building
            if (bldActPickBusy()) { postSpeech(axs(AXS_STILL_WORKING)); return true; }
            const RosRow* r = &g_rosRows[g_rosRow];
            uint32_t state = 0;
            bool haveState = safeReadU32(r->entry + PTY_ENTRY_STATE_OFF, &state);
            if (!haveState || state != 0) {
                char name[80] = {0}, cls[80] = {0}, status[160] = {0};
                abHeroNameClassOf(base, r->hero, name, sizeof name, cls, sizeof cls);
                rosStatusFrag(base, r->entry, status, sizeof status);
                char utter[256];
                _snprintf(utter, sizeof utter, axs(AXS_ACT_PICK_BUSY_FMT),
                          name[0] ? name : axs(AXS_THE_HERO),
                          status[0] ? status + 1 : axs(AXS_ROS_UNAVAILABLE));
                utter[sizeof utter - 1] = 0;                 // status+1: past the join space
                postSpeech(utter);
                return true;
            }
            bldActPickCommitFromRoster(base, r->entry, r->rowW, r->hero);
            return true;                     // the outcome watch decides what is said next
        }
        if (g_rosPickSlot >= 0) {
            int slot = g_rosPickSlot;
            if (rosAssignToSlot(base, g_rosRow, slot)) {
                rosLeave(base, "pick committed");
                g_ptyActive = true;              // ... and back to the slot we came from
                g_ptyRow = slot;
            }
            return true;
        }
        g_ptySheetFrom = 2;                      // the hamlet: Enter is the sheet, like C
        ptyOpenSheet(base, g_rosRows[g_rosRow].entry, g_rosRows[g_rosRow].hero);
        return true;
    }

    if (sym == SDLK_SPACE) {
        if (repeat) return true;
        if (g_rosPickSlot >= 0 || g_rosPickAct) { postSpeech(axs(AXS_ROS_NO_HERO_HERE)); return true; }
        if (g_rosOnBar) { postSpeech(axs(AXS_ROS_NO_HERO_HERE)); return true; }
        int n = rosCollect(base, false);
        if (n <= 0) { postSpeech(axs(AXS_ROS_NO_ROSTER)); return true; }
        axStepCursor(&g_rosRow, n, 0);           // re-clamp against the live roster
        if (!g_rosHoldEntry) {                   // pick up
            const RosRow* r = &g_rosRows[g_rosRow];
            char name[80] = {0}, cls[80] = {0};
            abHeroNameClassOf(base, r->hero, name, sizeof name, cls, sizeof cls);
            strncpy(g_rosHoldName, name[0] ? name : axs(AXS_PTY_HELD_HERO_FALLBACK),
                    sizeof g_rosHoldName - 1);
            g_rosHoldName[sizeof g_rosHoldName - 1] = 0;
            g_rosHoldEntry = r->entry;
            logLine("roster: picked up %s (entry=%p) at row %d of %d",
                    g_rosHoldName, (void*)r->entry, g_rosRow + 1, n);
            char utter[128];
            _snprintf(utter, sizeof utter, axs(AXS_ROS_MOVING_FMT), g_rosHoldName);
            utter[sizeof utter - 1] = 0;
            postSpeech(utter);
        } else {                                 // place
            rosCommitMove(base, g_rosRow);
        }
        return true;
    }

    int jump = 0;
    if (axDecodeJump(sym, mod, repeat, &jump)) {
        if (!jump) return true;                      // held jump: one landing per press
        if (g_rosOnBar) {
            axStepCursor(&g_rosBarCol, ROS_BAR_ITEMS, jump);
            rosSpeakBar(base, nullptr);
            return true;
        }
        int n = rosCollect(base, false);
        if (n <= 0) { postSpeech(axs(AXS_ROS_NO_ROSTER)); return true; }
        axStepCursor(&g_rosRow, n, jump);            // the clamp lands it; ends re-read
        rosSpeakRow(base, nullptr);
        return true;
    }

    if ((sym == SDLK_LEFT || sym == SDLK_RIGHT) && g_rosOnBar) {
        if (axNavHoldRepeat(repeat)) return true;
        axStepCursor(&g_rosBarCol, ROS_BAR_ITEMS, sym == SDLK_RIGHT ? 1 : -1);  // hard stop
        rosSpeakBar(base, nullptr);
        return true;
    }

    if (sym == SDLK_UP || sym == SDLK_DOWN) {
        if (axNavHoldRepeat(repeat)) return true;
                                                     // even holding a hero, the DROP is Space
        int n = rosCollect(base, false);
        if (n <= 0) { postSpeech(axs(AXS_ROS_NO_ROSTER)); return true; }
        axStepCursor(&g_rosRow, n, 0);       // re-clamp against the live roster
        if (g_rosOnBar) {
            if (sym == SDLK_UP) { rosSpeakBar(base, nullptr); return true; }   // hard stop at the top
            g_rosOnBar = false;                  // down from the toolbar = the first hero
            rosSpeakRow(base, nullptr);
            return true;
        }
        if (sym == SDLK_UP && g_rosRow == 0) {   // up from the first hero = the toolbar
            g_rosOnBar = true;
            rosSpeakBar(base, nullptr);
            return true;
        }
        axStepCursor(&g_rosRow, n, sym == SDLK_DOWN ? 1 : -1);   // hard stop: re-read the end row
        rosSpeakRow(base, nullptr);
        return true;
    }

    return false;
}

bool routeTownTab(uintptr_t base, uint32_t sym, uint8_t repeat) {
    if (sym != SDLK_TAB) return false;
    if (repeat) return true;                     // held key must not flap the context
    if (!resTownRoot(base)) return false;        // not in town: Tab stays the game's

    if (g_ptyActive) { ptyLeave(base, "Tab"); return true; }
    if (g_rosActive) {
        if (g_rosPickAct) {                      // mid-ACTIVITY-pick: Tab cancels, like Escape
            if (bldActPickBusy()) { postSpeech(axs(AXS_STILL_WORKING)); return true; }
            rosLeave(base, "Tab (activity pick cancelled)");
            bldActPickCancelFromRoster(base);
            return true;
        }
        if (g_rosPickSlot >= 0) {                // mid-pick: Tab is the cancel, back to the slot
            int slot = g_rosPickSlot;
            rosLeave(base, "Tab (pick cancelled)");
            g_ptyActive = true;
            g_ptyRow = slot;
            char hdr[192];
            ptySpeakRow(base, ptyHeader(base, hdr, sizeof hdr));
            return true;
        }
        rosLeave(base, "Tab");
        return true;
    }
    if (axIsProvision() && provTabToInfo(base)) return true;
    if (ptyEnter(base)) return true;             // the embark screen has a lineup to enter
    return rosEnterFromTown(base);               // everywhere else in town: the roster
}

bool routeTownShiftTab(uintptr_t base, uint8_t repeat) {
    uintptr_t root = resTownRoot(base);
    if (!root) return false;                     // not in town: Shift+Tab stays the game's
    if (!embProvIsOpen(root)) return routeTownTab(base, SDLK_TAB, repeat);
    if (repeat) return true;                     // held key must not flap the context
    if (g_ptyActive) {
        ptyLeave(base, "Shift+Tab");
        provLandOnInfo();
        return true;
    }
    if (!axIsProvision()) return false;
    if (provShiftTabToGrid(base)) return true;   // quest info -> the grid
    if (ptyEnter(base)) return true;
    return provTabToInfo(base);                  // no lineup here: the panel is the other stop
}

// ---- TOWN: EQUIP A TRINKET, STARTING FROM THE TRINKET (AX_TRKPICK) ----
static const int TRK_MAX_ROWS = ROS_MAX_ROWS;
struct TrkRow {
    uintptr_t entry;
    uintptr_t hero;
    bool      inParty;
};
static TrkRow g_trkRows[TRK_MAX_ROWS];
static int    g_trkCount = 0;
static int    g_trkRow   = 0;

bool axIsTrkPick() { return g_trkPhase == 1 && g_riActive; }

static int trkCollect(uintptr_t base) {
    g_trkCount = 0;
    if (riInCircus(base)) {
        uintptr_t heroes[TRK_MAX_ROWS];
        char inLineup[TRK_MAX_ROWS];
        int n = ringContestants(base, heroes, inLineup, TRK_MAX_ROWS);
        for (int pass = 0; pass < 2; pass++)          // the four contestants first, same as the
            for (int i = 0; i < n && g_trkCount < TRK_MAX_ROWS; i++) {   // hamlet's party-first
                bool in = inLineup[i] != 0;                              // order
                if (in != (pass == 0)) continue;
                g_trkRows[g_trkCount].entry   = 0;
                g_trkRows[g_trkCount].hero    = heroes[i];
                g_trkRows[g_trkCount].inParty = in;
                g_trkCount++;
            }
        if (g_trkCount) return g_trkCount;
        logLine("trkpick: circus, but the contestant list is not up -- trying the roster list");
    }
    int n = rosCollect(base, false);
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < n && g_trkCount < TRK_MAX_ROWS; i++) {
            uint32_t state = 0;
            safeReadU32(g_rosRows[i].entry + PTY_ENTRY_STATE_OFF, &state);
            bool inParty = (state == 1);
            if (inParty != (pass == 0)) continue;
            g_trkRows[g_trkCount].entry   = g_rosRows[i].entry;
            g_trkRows[g_trkCount].hero    = g_rosRows[i].hero;
            g_trkRows[g_trkCount].inParty = inParty;
            g_trkCount++;
        }
    }
    return g_trkCount;
}

static uintptr_t trkItem(uintptr_t base) {
    uintptr_t sys = riSystem(base), beg = 0;
    int total = 0;
    if (!sys || !invItemVectorAt(sys, &beg, &total)) return 0;
    if (g_trkItemSlot < 0 || g_trkItemSlot >= total) return 0;
    uintptr_t item = beg + (uintptr_t)g_trkItemSlot * ITEM_STRIDE;
    int32_t amount = 0;
    safeReadU32(item + ITEM_AMOUNT_OFF, (uint32_t*)&amount);
    return amount > 0 ? item : 0;
}

static void trkFitFrag(uintptr_t base, uintptr_t item, uintptr_t hero, char* out, int outsz) {
    out[0] = 0;
    if (!item) return;
    bool sure = false;
    bool fits = bldTrinketFitsHero(base, item, hero, &sure);
    if (!sure) return;
    if (fits) { _snprintf(out, outsz, " %s", axs(AXS_TRK_CAN_WEAR)); out[outsz - 1] = 0; return; }
    char req[160];
    req[0] = 0;
    uintptr_t rec = bldTrinketRecord(base, item);
    if (rec) bldTrinketClassReq(base, rec, req, sizeof req, false);
    char line[224];
    if (req[0]) _snprintf(line, sizeof line, axs(AXS_TRK_CANT_WEAR_REQ_FMT), req);
    else        _snprintf(line, sizeof line, "%s", axs(AXS_TRK_CANT_WEAR));
    line[sizeof line - 1] = 0;
    _snprintf(out, outsz, " %s", line);
    out[outsz - 1] = 0;
}

static void trkSpeakRow(uintptr_t base, const char* prefix) {
    int n = trkCollect(base);
    if (n <= 0) { postSpeech(axs(AXS_ROS_NO_ROSTER)); return; }
    if (g_trkRow >= n) g_trkRow = n - 1;
    if (g_trkRow < 0)  g_trkRow = 0;
    const TrkRow* r = &g_trkRows[g_trkRow];
    char card[512], status[160], fit[224];
    if (r->entry) ptyHeroFrag    (base, r->hero, card, sizeof card);
    else          ptyHeroFragBare(base, r->hero, card, sizeof card);
    if (!r->entry) _snprintf(status, sizeof status, "%s%s",
                             r->inParty ? " " : "", r->inParty ? axs(AXS_RNG_IN_LINEUP) : "");
    else           rosStatusFrag(base, r->entry, status, sizeof status);
    status[sizeof status - 1] = 0;
    trkFitFrag(base, trkItem(base), r->hero, fit, sizeof fit);
    char posln[64];
    _snprintf(posln, sizeof posln, axs(AXS_POS_N_OF_M), g_trkRow + 1, n);
    posln[sizeof posln - 1] = 0;
    char utter[MAILBOX_SZ];
    _snprintf(utter, sizeof utter, "%s%s.%s%s %s", prefix ? prefix : "",
              card, status, fit, posln);
    utter[sizeof utter - 1] = 0;
    postSpeech(utter);
}

void trkPickCancel(uintptr_t base, AxStrId why) {
    if (!g_trkPhase) return;
    logLine("trkpick: cancelled at phase %d (%s)", g_trkPhase, why >= 0 ? axsKeyName(why) : "silent");
    g_trkPhase = 0;
    g_trkItemSlot = -1;
    g_trkHero = 0;
    g_trkEntry = 0;
    g_trkSheetWatchUntil = 0;
    g_trkToggleIssued = false;
    g_riEquipForHero = 0;                 // the shared pending state is ours while this runs
    g_riEquipForSlot = -1;
    g_riEquipCloseSheet = false;          // ... including "close the sheet once it lands"
    if (why >= 0) postSpeech(axs(why));
    (void)base;
}

void trkPickBegin(uintptr_t base, int realSlot, const char* name) {
    g_trkPhase    = 1;
    g_trkItemSlot = realSlot;
    g_trkRow      = 0;
    g_trkHero     = 0;
    g_trkEntry    = 0;
    g_trkToggleIssued = false;
    _snprintf(g_trkItemName, sizeof g_trkItemName, "%s", name ? name : axs(AXS_TRK_THE_TRINKET));
    g_trkItemName[sizeof g_trkItemName - 1] = 0;
    int n = trkCollect(base);
    logLine("trkpick: begin \"%s\" (realm slot %d), %d candidates", g_trkItemName, realSlot, n);
    if (n <= 0) {
        logLine("trkpick: the roster read back empty -> no picker");
        g_trkPhase = 0;
        g_trkItemSlot = -1;
        postSpeech(axs(AXS_TRK_NO_HEROES));
        return;
    }
    char line[320], prefix[336];
    _snprintf(line, sizeof line, axs(AXS_TRK_EQUIP_CHOOSE_FMT), g_trkItemName, n);
    line[sizeof line - 1] = 0;
    _snprintf(prefix, sizeof prefix, "%s ", line);
    prefix[sizeof prefix - 1] = 0;
    trkSpeakRow(base, prefix);
}

static void trkChooseHero(uintptr_t base) {
    int n = trkCollect(base);
    if (n <= 0) { postSpeech(axs(AXS_ROS_NO_ROSTER)); return; }
    if (g_trkRow >= n) g_trkRow = n - 1;
    if (g_trkRow < 0)  g_trkRow = 0;
    const TrkRow* r = &g_trkRows[g_trkRow];

    uintptr_t item = trkItem(base);
    if (!item) {
        trkPickCancel(base, AXS_TRK_TRINKET_GONE);
        return;
    }
    bool sure = false;
    if (!bldTrinketFitsHero(base, item, r->hero, &sure) && sure) {
        char req[160], who[80] = { 0 };
        req[0] = 0;
        uintptr_t rec = bldTrinketRecord(base, item);
        if (rec) bldTrinketClassReq(base, rec, req, sizeof req, false);
        safeReadCStr(r->hero + HERO_NAME_OFF, who, sizeof who);
        char utter[320];
        if (req[0]) _snprintf(utter, sizeof utter, axs(AXS_TRK_CANT_WEAR_WHO_REQ_FMT),
                              who[0] ? who : axs(AXS_TRK_THAT_HERO), g_trkItemName, req);
        else        _snprintf(utter, sizeof utter, axs(AXS_TRK_CANT_WEAR_WHO_FMT),
                              who[0] ? who : axs(AXS_TRK_THAT_HERO), g_trkItemName);
        utter[sizeof utter - 1] = 0;
        logLine("trkpick: \"%s\" refused for hero=%p (class requirement)",
                g_trkItemName, (void*)r->hero);
        bldTrinketWhyNotFit(base, item, r->hero);   // both sides of the comparison, once, here --
        postSpeech(utter);
        return;
    }

    g_trkHero  = r->hero;
    g_trkEntry = r->entry;

    uintptr_t open = csTownSheetPanel(base);
    if (open && abSelectedHero(base) == g_trkHero) {
        logLine("trkpick: the sheet is already on this hero -> straight to the slot pick");
        g_trkPhase = 2;
        g_trkSheetWatchUntil = GetTickCount() + 2000;
        return;                                     // serviceTrkPick lands it next frame
    }
    if (open) {
        trkPickCancel(base, AXS_TRK_OTHER_SHEET_OPEN);
        return;
    }
    char who[80] = { 0 };
    safeReadCStr(g_trkHero + HERO_NAME_OFF, who, sizeof who);
    logLine("trkpick: opening the sheet for %p \"%s\"%s", (void*)g_trkHero, who,
            g_trkEntry ? "" : " (circus)");
    g_trkPhase = 2;
    g_trkSheetWatchUntil = GetTickCount() + 2000;
    if (!g_trkEntry) {
        if (!ringOpenSheetForPick(base, g_trkHero))
            trkPickCancel(base, (AxStrId)-1);       // it has already said why; a second line here
        return;                                     // would talk over it
    }
    g_ptySheetFrom = 3;                             // this flow owns the resume, not the party area
    ptyOpenSheet(base, g_trkEntry, g_trkHero);
}

bool routeTrkPickKey(uintptr_t base, uint32_t sym, uint16_t mod, uint8_t repeat) {
    if (mod & (KMOD_LALT | KMOD_RALT | KMOD_LCTRL | KMOD_RCTRL)) return false;

    if (sym == SDLK_ESCAPE) {
        if (repeat) return true;
        trkPickCancel(base, (AxStrId)-1);
        char pfx[96];
        _snprintf(pfx, sizeof pfx, "%s ", axs(AXS_CANCELLED));
        pfx[sizeof pfx - 1] = 0;
        riSpeakRow(base, pfx);                       // back onto the grid, on the same trinket
        return true;
    }
    if (sym == SDLK_RETURN || sym == SDLK_KP_ENTER) {
        if (repeat) return true;
        trkChooseHero(base);
        return true;
    }
    int dRow = 0, dCol = 0, jump = 0;
    if (axDecodeJump(sym, mod, repeat, &jump)) {     // Home/End: first/last hero in the picker
        if (!jump) return true;                      // held jump: one landing per press
        dRow = jump;
    } else if (!axDecodeArrow(sym, mod, repeat, /*wantCols=*/false, &dRow, &dCol)) return false;
    if (!dRow) return true;                          // throttled repeat: claimed, no step this tick
    int n = trkCollect(base);
    if (n <= 0) { postSpeech(axs(AXS_ROS_NO_ROSTER)); return true; }
    axStepCursor(&g_trkRow, n, 0);                   // re-clamp against the live roster
    axStepCursor(&g_trkRow, n, dRow);                // hard stop: re-read the end row
    trkSpeakRow(base, nullptr);
    return true;
}

void trkReannounce(uintptr_t base) { trkSpeakRow(base, nullptr); }

void trkCommitSlot(uintptr_t base, int slot) {
    if (g_trkPhase != 3) return;
    if (g_riEquipWatchUntil) { postSpeech(axs(AXS_TRK_STILL_EQUIPPING)); return; }

    if (!riIsOpen(base) || !csTownSheetPanel(base)) {
        logLine("trkpick: commit with panel=%d sheet=%p -> cancelled",
                riIsOpen(base) ? 1 : 0, (void*)csTownSheetPanel(base));
        trkPickCancel(base, AXS_TRK_PANEL_CLOSED);
        return;
    }
    uintptr_t item = trkItem(base);
    if (!item) {
        trkPickCancel(base, AXS_TRK_TRINKET_GONE);
        return;
    }
    int occ[RI_MAX_TRINKETS];
    int slots = riOccupiedSlots(base, occ, RI_MAX_TRINKETS);
    int gridPos = -1;
    for (int i = 0; i < slots; i++) if (occ[i] == g_trkItemSlot) { gridPos = i; break; }
    if (gridPos < 0) {
        logLine("trkpick: realm slot %d is no longer occupied -> cancelled", g_trkItemSlot);
        trkPickCancel(base, AXS_TRK_TRINKET_GONE);
        return;
    }

    // Hand the shared pending state the values the drag and its watcher read.
    g_riEquipForHero = g_trkHero;
    g_riEquipForSlot = slot;
    g_riEquipCloseSheet = true;
    _snprintf(g_riEquipName, sizeof g_riEquipName, "%s", g_trkItemName);
    g_riEquipName[sizeof g_riEquipName - 1] = 0;
    g_riEquipHeroName[0] = 0;
    safeReadCStr(g_trkHero + HERO_NAME_OFF, g_riEquipHeroName, sizeof g_riEquipHeroName);
    g_riEquipCountBefore = slots;

    logLine("trkpick: commit \"%s\" (realm slot %d, grid pos %d) -> hero=%p slot %d",
            g_trkItemName, g_trkItemSlot, gridPos, (void*)g_trkHero, slot + 1);
    if (!riStartEquipDrag(base, g_trkItemSlot, gridPos)) {
        g_riEquipForHero = 0;
        g_riEquipForSlot = -1;
        g_riEquipCloseSheet = false;
        postSpeech(axs(AXS_TRK_CANT_EQUIP_NOW));
        return;
    }
    g_trkPhase = 0;
    g_trkItemSlot = -1;
    g_trkHero = 0;
    g_trkEntry = 0;
    g_trkToggleIssued = false;
    g_riEquipWatchUntil = GetTickCount() + 2000;
}

static void trkPickSheetClosed(uintptr_t base) {
    if (!g_trkPhase) return;
    logLine("trkpick: the sheet closed at phase %d", g_trkPhase);
    trkPickCancel(base, (AxStrId)-1);
    if (riIsOpen(base)) {
        char pfx[128];
        _snprintf(pfx, sizeof pfx, "%s ", axs(AXS_RI_TITLE));
        pfx[sizeof pfx - 1] = 0;
        riSpeakRow(base, pfx);
    } else {
        postSpeech(axs(AXS_TRK_EQUIP_CANCELLED));
    }
}

void serviceTrkPick(uintptr_t base) {
    if (g_trkPhase == 3) {
        if (!csTownSheetPanel(base)) trkPickSheetClosed(base);
        return;
    }
    if (g_trkPhase != 2) return;
    uintptr_t sheet = csTownSheetPanel(base);
    if (!sheet) {
        if (g_trkSheetWatchUntil && GetTickCount() > g_trkSheetWatchUntil) {
            logLine("trkpick: the sheet never opened for hero=%p", (void*)g_trkHero);
            trkPickCancel(base, AXS_SHEET_DIDNT_OPEN);
        }
        return;
    }
    if (abSelectedHero(base) != g_trkHero) {
        logLine("trkpick: the sheet opened on %p, not %p -> cancelled",
                (void*)abSelectedHero(base), (void*)g_trkHero);
        trkPickCancel(base, AXS_TRK_WRONG_SHEET);
        return;
    }
    if (!riIsOpen(base)) {
        if (!g_trkToggleIssued) {
            g_trkToggleIssued = true;
            bool ok = true;
            __try { ((RiToggleFn)(base + RI_TOGGLE_RVA))(); }
            __except (EXCEPTION_EXECUTE_HANDLER) { ok = false; }
            logLine("trkpick: re-open toggle issued behind the sheet (ok=%d)", ok ? 1 : 0);
            if (!ok) { trkPickCancel(base, AXS_TRK_PANEL_NO_REOPEN); return; }
            g_trkSheetWatchUntil = GetTickCount() + 2000;   // now a watch on the PANEL
            return;
        }
        if (g_trkSheetWatchUntil && GetTickCount() > g_trkSheetWatchUntil) {
            logLine("trkpick: the trinket panel never came back");
            trkPickCancel(base, AXS_TRK_PANEL_NO_REOPEN);
        }
        return;                                       // still opening: wait
    }
    if (!csFocusTrinketSection(base)) {
        logLine("trkpick: the sheet has no trinkets section -> cancelled");
        trkPickCancel(base, AXS_TRK_NO_SLOTS);
        return;
    }
    g_trkPhase = 3;
    g_trkSheetWatchUntil = 0;
    char who[80] = { 0 }, row[MAILBOX_SZ];
    safeReadCStr(g_trkHero + HERO_NAME_OFF, who, sizeof who);
    char line[320], prefix[336];
    _snprintf(line, sizeof line, axs(AXS_TRK_CHOOSE_SLOT_FMT),
              who[0] ? who : axs(AXS_THE_HERO), g_trkItemName);
    line[sizeof line - 1] = 0;
    _snprintf(prefix, sizeof prefix, "%s ", line);
    prefix[sizeof prefix - 1] = 0;
    logLine("trkpick: phase 3 — slot pick on %s's sheet", who);
    if (csTrinketRowText(base, 0, row, sizeof row)) {
        char utter[MAILBOX_SZ];
        _snprintf(utter, sizeof utter, "%s%s", prefix, row);
        utter[sizeof utter - 1] = 0;
        postSpeech(utter);                           // land ON something, in one utterance
    } else {
        postSpeech(prefix);
    }
}
