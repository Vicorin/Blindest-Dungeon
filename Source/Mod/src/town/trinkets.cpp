// town/trinkets.cpp -- the eighth TOWN slice

#include <cstdint>
#include <cstring>
#include <cstdio>
#include "internal.h"
// ---- TOWN: the TRINKET (realm) INVENTORY (AX_REALMINV) ----
static const uintptr_t RI_PANEL_OFF       = 0x4058;
static const uintptr_t RI_OPEN_OFF        = 0x40b0;
static const uintptr_t RI_HERO_OFF        = 0x4578;
static const uintptr_t RI_SYS_OFF         = 0xe88;
static const uintptr_t RI_SYS_OFF_CIRCUS  = 0xef0;     // campaign+: the Butcher's Circus variant
static const uintptr_t RI_SYS_COUNT_OFF   = 0x24;      // System+: occupied-stack count (the game's own)
static const uintptr_t RI_ASK_SELL_OFF    = 0x514;
static const uintptr_t RI_SORTIDX_OFF     = 0x518;     // panel+: int, index of the sort last applied
static const uintptr_t RI_RARITY_GATE_OFF = 0x53c;     // panel+: char, rarity sort offered only while 0
static const uint32_t  RI_ELEM_UNEQUIP    = 0x756e6571;// 'uneq' — the unequip-all button element
static const uint32_t  RI_ELEM_SORT_BASE  = 0x73727420;// 'srt ' + index — the sort buttons

typedef void (*RiSellDlgFn)(void* capture, uint32_t slot);

static const int RI_MAX_ACTIONS = 4;                   // unequip-all + up to 3 sorts
struct RiAction { int sortIdx; const char* locKey; const char* logName; };  // sortIdx -1 = unequip

bool         g_riActive = false;
static int   g_riSlot   = 0;                           // cursor: 0..slots-1 grid, then action rows

static DWORD g_riSellWatchStart = 0;                   // 0 = idle
static bool  g_riSellDlgSeen = false;
static int   g_riSellSlot = -1;
static int   g_riSellGoldBefore = 0;
static char  g_riSellName[256];

// Unequip watcher: same shape (its confirm is also the game's).
static DWORD g_riUneqWatchStart = 0;                   // 0 = idle
static bool  g_riUneqDlgSeen = false;
static int   g_riUneqCountBefore = 0;

static DWORD g_riSortWatchUntil = 0;                   // 0 = idle
static int   g_riSortWantIdx = -1;
static char  g_riSortName[128];

uintptr_t    g_riEquipForHero = 0;       // pending equip: the hero (0 = none pending)
int          g_riEquipForSlot = -1;      // ... and which of their two trinket slots
char         g_riEquipName[256];         // the trinket being dragged, for announcements
DWORD        g_riEquipWatchUntil = 0;    // 0 = idle
int          g_riEquipCountBefore = -1;  // occupied realm count before the drag
char         g_riEquipHeroName[80];
static uint32_t g_riEquipIdHash = 0;
bool         g_riEquipCloseSheet = false;
static DWORD g_riSheetCloseUntil = 0;    // 0 = idle; watching the sheet actually go away

static char  g_riBackLine[MAILBOX_SZ];   // the held equip line; [0] == 0 = nothing held
static int   g_riBackSlot = -1;          // ... and the sheet trinket slot to land back on
static DWORD g_riPanelCloseUntil = 0;    // 0 = idle; watching the grid actually go away

bool axIsRealmInv() { return g_riActive && g_trkPhase != 3; }

// The live panel, identity-checked behind the town gate. 0 => not available.
static uintptr_t riPanel(uintptr_t base) {
    uintptr_t root = resTownRoot(base);
    if (!root) return 0;
    uintptr_t panel = root + RI_PANEL_OFF;
    uintptr_t vft = 0;
    if (!safeReadPtr(panel, &vft) || vft != base + RI_VFT_RVA) return 0;
    return panel;
}

bool riIsOpen(uintptr_t base) {
    uintptr_t root = resTownRoot(base);
    if (!root) return false;
    uint32_t f = 0;
    return safeReadU32(root + RI_OPEN_OFF, &f) && f != 0;
}

uintptr_t riSystem(uintptr_t base) {
    uintptr_t campaign = 0;
    if (!safeReadPtr(base + RES_CAMPAIGN_RVA, &campaign) || campaign <= 0x10000) return 0;
    uint8_t circus = 0;
    safeReadU8(base + RI_CIRCUS_FLAG_RVA, &circus);
    return campaign + (circus ? RI_SYS_OFF_CIRCUS : RI_SYS_OFF);
}

// ---- THE BUTCHER'S CIRCUS LOCK ----
static const uintptr_t ITEM_LOCK_PRESTIGE_BEG = 0xa0;  // ItemStack+: LockingCondition, the
static const uintptr_t ITEM_LOCK_PRESTIGE_END = 0xa8;  //   vector<uint32> of prestige levels
static const uintptr_t ITEM_LOCK_ACHIEVE_BEG  = 0xb8;  //   ...and the achievement vector
static const uintptr_t ITEM_LOCK_ACHIEVE_END  = 0xc0;  //   (stride 0x40; only emptiness is read)
static const uintptr_t ITEM_UNLOCK_MAP_OFF = 0x90;   // ItemStack+: std::map<uint32,int> _Myhead
static const uintptr_t UMAP_NODE_LEFT_OFF   = 0x00;
static const uintptr_t UMAP_NODE_PARENT_OFF = 0x08;
static const uintptr_t UMAP_NODE_RIGHT_OFF  = 0x10;
static const uintptr_t UMAP_NODE_ISNIL_OFF  = 0x19;
static const uintptr_t UMAP_NODE_KEY_OFF    = 0x1c;  // pair<const uint,int>: key ...
static const uintptr_t UMAP_NODE_VAL_OFF    = 0x20;  // ... and value, 4-byte aligned
static const int       UMAP_MAX_DEPTH       = 64;    // a tree over a handful of mode ids is

static bool g_riUnlockWarned = false;
static bool g_riFilterFallbackWarned = false;
static bool g_riCountsLogged = false;
static void riUnlockWarnReset() {
    g_riUnlockWarned = false; g_riFilterFallbackWarned = false; g_riCountsLogged = false;
}
static void riUnlockWarn(const char* what, uintptr_t item) {
    if (g_riUnlockWarned) return;
    g_riUnlockWarned = true;
    logLine("realminv: %s (item %p) -- showing it, and not warning again this session",
            what, (void*)item);
}

bool riInCircus(uintptr_t base) {   // exported (internal.h) -- the picker asks too
    uint8_t circus = 0;
    return safeReadU8(base + RI_CIRCUS_FLAG_RVA, &circus) && circus != 0;
}

static bool riItemHasNoLockingCondition(uintptr_t item) {
    uintptr_t pb = 0, pe = 0, ab = 0, ae = 0;
    if (!safeReadPtr(item + ITEM_LOCK_PRESTIGE_BEG, &pb) ||
        !safeReadPtr(item + ITEM_LOCK_PRESTIGE_END, &pe) ||
        !safeReadPtr(item + ITEM_LOCK_ACHIEVE_BEG,  &ab) ||
        !safeReadPtr(item + ITEM_LOCK_ACHIEVE_END,  &ae)) return true;
    return pe == pb && ae == ab;
}

static int riItemUnlockMapFlag(uintptr_t base, uintptr_t item) {
    uint32_t want = 0;
    if (!item || !safeReadU32(base + RI_UNLOCK_KEY_RVA, &want)) return -1;
    uintptr_t head = 0, node = 0;
    if (!safeReadPtr(item + ITEM_UNLOCK_MAP_OFF, &head) || head <= 0x10000 ||
        !safeReadPtr(head + UMAP_NODE_PARENT_OFF, &node) || node <= 0x10000) return -1;
    uintptr_t cand = head;
    for (int d = 0; d < UMAP_MAX_DEPTH; d++) {
        uint8_t nil = 1;
        uint32_t key = 0;
        uintptr_t next = 0;
        if (!safeReadU8(node + UMAP_NODE_ISNIL_OFF, &nil)) return -1;
        if (nil) {                                    // lower_bound landed: cand is the answer
            uint8_t candNil = 1; uint32_t candKey = 0, val = 0;
            if (cand == head) return 0;               // every key is smaller: absent
            if (!safeReadU8(cand + UMAP_NODE_ISNIL_OFF, &candNil)) return -1;
            if (candNil) return 0;
            if (!safeReadU32(cand + UMAP_NODE_KEY_OFF, &candKey)) return -1;
            if (candKey != want) return 0;            // absent (the accessor treats it as 0)
            if (!safeReadU32(cand + UMAP_NODE_VAL_OFF, &val)) return -1;
            return (int32_t)val != 0 ? 1 : 0;
        }
        if (!safeReadU32(node + UMAP_NODE_KEY_OFF, &key)) return -1;
        if (key < want) { if (!safeReadPtr(node + UMAP_NODE_RIGHT_OFF, &next)) return -1; }
        else            { cand = node;
                          if (!safeReadPtr(node + UMAP_NODE_LEFT_OFF, &next)) return -1; }
        if (next <= 0x10000) return -1;
        node = next;
    }
    return -1;                                        // deeper than any real tree: garbage
}

bool riItemUnlocked(uintptr_t base, uintptr_t item) {   // exported (internal.h)
    if (!item || !riInCircus(base)) return true;
    int flag = riItemUnlockMapFlag(base, item);
    if (flag < 0) {
        riUnlockWarn("the unlock map is unreadable", item);
        return true;
    }
    return flag != 0;
}

static void riDumpStacks(uintptr_t base) {
    if (!axDebugLogEnabled()) return;
    uintptr_t sys = riSystem(base), beg = 0;
    int slots = 0;
    if (!sys || !invItemVectorAt(sys, &beg, &slots)) { logLine("realminv-dump: no system"); return; }
    uint32_t sysCount = 0;
    safeReadU32(sys + RI_SYS_COUNT_OFF, &sysCount);
    logLine("realminv-dump: sys=%p vector=%d slots, System+0x24=%u, circus=%d",
            (void*)sys, slots, sysCount, riInCircus(base) ? 1 : 0);
    int shown = 0;
    for (int i = 0; i < slots; i++) {
        uintptr_t item = beg + (uintptr_t)i * ITEM_STRIDE;
        uint32_t amount = 0;
        if (!safeReadU32(item + ITEM_AMOUNT_OFF, &amount) || (int32_t)amount <= 0) continue;
        uintptr_t pb = 0, pe = 0, ab = 0, ae = 0;
        bool condRead = safeReadPtr(item + ITEM_LOCK_PRESTIGE_BEG, &pb) &&
                        safeReadPtr(item + ITEM_LOCK_PRESTIGE_END, &pe) &&
                        safeReadPtr(item + ITEM_LOCK_ACHIEVE_BEG,  &ab) &&
                        safeReadPtr(item + ITEM_LOCK_ACHIEVE_END,  &ae);
        char type[64] = {0}, itemId[64] = {0}, key[192] = {0}, name[256] = {0};
        invItemName(base, item, type, itemId, key, name);
        uintptr_t rec = bldTrinketRecord(base, item);
        char req[192] = {0};
        if (rec) bldTrinketClassReq(base, rec, req, sizeof req, false);
        bool owned = riItemUnlocked(base, item);
        if (owned) shown++;
        logLine("realminv-dump: [%3d] %-28s amt=%u cond=%s(p %d,a %d) map=%d rec=%p req=\"%s\" %s",
                i, itemId[0] ? itemId : "(no id)", amount,
                condRead ? "ok" : "UNREADABLE",
                (int)(pe - pb), (int)(ae - ab),
                riItemUnlockMapFlag(base, item),      // 1 flagged / 0 not / -1 unreadable
                (void*)rec, req, owned ? "<= SHOWN" : "");
    }
    logLine("realminv-dump: %d of %d occupied stacks shown", shown, slots);
}

static int riCols(uintptr_t base) {
    uint32_t cols = 0;
    if (!safeReadU32(base + RI_COLS_RVA, &cols) || cols < 1 || cols > 16) return 7;
    return (int)cols;
}

int riOccupiedSlots(uintptr_t base, int* out, int max) {
    uintptr_t sys = riSystem(base), beg = 0; int slots = 0;
    if (!sys || !invItemVectorAt(sys, &beg, &slots)) return 0;
    int occupied = 0, n = 0, noCond = 0;
    bool circus = riInCircus(base);
    for (int i = 0; i < slots; i++) {
        uintptr_t item = beg + (uintptr_t)i * ITEM_STRIDE;
        uint32_t amount = 0;
        if (!safeReadU32(item + ITEM_AMOUNT_OFF, &amount)) continue;
        if ((int32_t)amount <= 0) continue;
        occupied++;
        if (circus && riItemHasNoLockingCondition(item)) noCond++;
        if (n >= max) continue;
        if (!riItemUnlocked(base, item)) continue;
        out[n++] = i;
    }
    if (circus && !g_riCountsLogged) {
        g_riCountsLogged = true;
        logLine("realminv: circus stacks=%d, shown by unlock-map=%d, "
                "copies without a locking condition=%d", occupied, n, noCond);
    }
    if (n == 0 && occupied > 0) {
        if (!g_riFilterFallbackWarned) {
            g_riFilterFallbackWarned = true;
            logLine("realminv: the circus unlock filter hid ALL %d stacks -- ignoring it, and not "
                    "warning again this session", occupied);
        }
        for (int i = 0; i < slots && n < max; i++) {
            uint32_t amount = 0;
            if (!safeReadU32(beg + (uintptr_t)i * ITEM_STRIDE + ITEM_AMOUNT_OFF, &amount)) continue;
            if ((int32_t)amount > 0) out[n++] = i;
        }
    }
    return n;
}

static int riSlotCount(uintptr_t base) {
    int slots[RI_MAX_TRINKETS];
    return riOccupiedSlots(base, slots, RI_MAX_TRINKETS);
}

static int riActions(uintptr_t base, uintptr_t panel, RiAction* out) {
    int n = 0;
    out[n].sortIdx = -1; out[n].locKey = "str_unequip_all_trinkets"; out[n].logName = "unequip-all"; n++;
    int sortPos = 0;
    out[n].sortIdx = sortPos++; out[n].locKey = "str_sort_trinkets_by_class"; out[n].logName = "sort-class"; n++;
    uint8_t rarityGate = 1;
    if (panel && safeReadU8(panel + RI_RARITY_GATE_OFF, &rarityGate) && rarityGate == 0) {
        out[n].sortIdx = sortPos++; out[n].locKey = "str_sort_trinkets_by_rarity"; out[n].logName = "sort-rarity"; n++;
    }
    char cfg[64] = { 0 };
    if (resolveKey(base, "variable_sort_trinkets_alphabetically_enabled", cfg, sizeof cfg) &&
        (cfg[0] == 't' || cfg[0] == 'T' || cfg[0] == '1' || cfg[0] == 'y' || cfg[0] == 'Y')) {
        out[n].sortIdx = sortPos++; out[n].locKey = "str_sort_trinkets_alphabetically"; out[n].logName = "sort-alpha"; n++;
    }
    return n;
}

uintptr_t bldTrinketRecord(uintptr_t base, uintptr_t item);
bool bldTrinketRarity(uintptr_t base, uintptr_t rec, char* out, int outsz, bool probe);
void bldTrinketClassReq(uintptr_t base, uintptr_t rec, char* out, int outsz, bool probe);
int  bldTrinketEffects(uintptr_t base, uintptr_t item, char* out, int outsz, bool probe);
bool bldTrinketFitsHero(uintptr_t base, uintptr_t item, uintptr_t hero, bool* sure);

static bool riTrinketRowText(uintptr_t base, uintptr_t sys, int slot, int pos, int total,
                             char* out, int outsz) {
    uintptr_t beg = 0; int slots = 0;
    if (!invItemVectorAt(sys, &beg, &slots) || slot < 0 || slot >= slots) return false;
    uintptr_t item = beg + (uintptr_t)slot * ITEM_STRIDE;
    uint32_t amount = 0;
    safeReadU32(item + ITEM_AMOUNT_OFF, &amount);
    char type[64] = {0}, itemId[64] = {0}, key[192] = {0}, name[256] = {0};
    invItemName(base, item, type, itemId, key, name);
    char rarity[96] = {0}, clsreq[256] = {0}, charges[384] = {0}, trig[1536] = {0};
    uintptr_t rec = bldTrinketRecord(base, item);
    if (rec) {
        bldTrinketRarity(base, rec, rarity, sizeof rarity, false);
        bldTrinketClassReq(base, rec, clsreq, sizeof clsreq, false);
        bldTrinketCharges(base, item, rec, charges, sizeof charges, false);
        bldTrinketTriggers(base, rec, trig, sizeof trig, false);
    }
    char fx[768] = {0};
    bldTrinketEffects(base, item, fx, sizeof fx, false);
    char desckey[192], desc[512];
    _snprintf(desckey, sizeof desckey, "%s%s%s", INV_DESC_PREFIX, type, itemId);
    desckey[sizeof desckey - 1] = 0;
    bool hasDesc = resolveKey(base, desckey, desc, sizeof desc) && desc[0];
    char head[1120];
    if (amount > 1)
        _snprintf(head, sizeof head, "%s, %u%s%s%s%s%s%s", name, amount,
                  rarity[0] ? ". " : "", rarity, clsreq[0] ? ". " : "", clsreq,
                  charges[0] ? ". " : "", charges);
    else
        _snprintf(head, sizeof head, "%s%s%s%s%s%s%s", name,
                  rarity[0] ? ". " : "", rarity, clsreq[0] ? ". " : "", clsreq,
                  charges[0] ? ". " : "", charges);
    head[sizeof head - 1] = 0;
    char posline[64];
    _snprintf(posline, sizeof posline, axs(AXS_RI_TRINKET_N_OF_M), pos, total);
    posline[sizeof posline - 1] = 0;
    _snprintf(out, outsz, "%s%s%s%s%s. %s%s%s",
              head, fx[0] ? ". " : "", fx,
              trig[0] ? ". " : "", trig,
              posline,
              hasDesc ? " " : "", hasDesc ? desc : "");
    out[outsz - 1] = 0;
    return true;
}

static int riActionCount(uintptr_t base) {
    RiAction acts[RI_MAX_ACTIONS];
    return riActions(base, riPanel(base), acts);
}

static bool riRowText(uintptr_t base, int index, char* out, int outsz) {
    RiAction acts[RI_MAX_ACTIONS];
    int nActs = riActions(base, riPanel(base), acts);
    if (index >= 0 && index < nActs) {                 // the top button row
        char label[192] = { 0 };
        if (!resolveKey(base, acts[index].locKey, label, sizeof label))
            strncpy(label, acts[index].logName, sizeof label - 1);   // never silent on a miss
        abStripMarkup(label);
        _snprintf(out, outsz, axs(AXS_RI_BUTTON_N_OF_M), label, index + 1, nActs);
        out[outsz - 1] = 0;
        return true;
    }
    int occ[RI_MAX_TRINKETS];
    int slots = riOccupiedSlots(base, occ, RI_MAX_TRINKETS);
    int g = index - nActs;                             // position within the table
    if (g < 0 || g >= slots) return false;
    uintptr_t sys = riSystem(base);
    return sys && riTrinketRowText(base, sys, occ[g], g + 1, slots, out, outsz);
}

void riSpeakRow(uintptr_t base, const char* prefix) {
    char row[MAILBOX_SZ]; row[0] = 0;
    if (!riRowText(base, g_riSlot, row, sizeof row)) {
        logLine("realminv: row %d did not read", g_riSlot);
        if (prefix && prefix[0]) postSpeech(prefix);
        return;
    }
    if (prefix && prefix[0]) {
        char utter[MAILBOX_SZ];
        _snprintf(utter, sizeof utter, "%s%s", prefix, row);
        utter[sizeof utter - 1] = 0;
        postSpeech(utter);
    } else {
        postSpeech(row);
    }
}

void riReannounce(uintptr_t base) {
    char pre[128];
    _snprintf(pre, sizeof pre, "%s ", axs(AXS_RI_TITLE));
    pre[sizeof pre - 1] = 0;
    riSpeakRow(base, pre);
}

bool routeRealmInvToggle(uintptr_t base, uint8_t repeat) {
    uintptr_t root = resTownRoot(base);
    if (!root) return false;                           // not town -> the key is not ours
    if (repeat) return true;
    bool wasOpen = riIsOpen(base);
    bool ok = true;
    __try { ((RiToggleFn)(base + RI_TOGGLE_RVA))(); }
    __except (EXCEPTION_EXECUTE_HANDLER) { ok = false; }
    logLine("realminv: toggle called (ok=%d, was %s, circus=%d)",
            ok ? 1 : 0, wasOpen ? "open" : "closed", riInCircus(base) ? 1 : 0);
    return true;
}

static void riSell(uintptr_t base, uintptr_t panel, int slot) {
    if (g_riSellWatchStart) return;                    // one sale in flight at a time
    uint8_t circus = 0;
    safeReadU8(base + RI_CIRCUS_FLAG_RVA, &circus);
    if (circus) {                                      // the game draws no sell path in the Circus
        postSpeech(axs(AXS_RI_NO_SELLING));
        return;
    }
    uintptr_t sys = riSystem(base), beg = 0; int slots = 0;
    if (!sys || !invItemVectorAt(sys, &beg, &slots) || slot < 0 || slot >= slots) return;
    uint32_t amount = 0;
    safeReadU32(beg + (uintptr_t)slot * ITEM_STRIDE + ITEM_AMOUNT_OFF, &amount);
    if ((int32_t)amount < 1) {
        postSpeech(axs(AXS_RI_NOTHING_TO_SELL));
        return;
    }
    char type[64], itemId[64], key[192];
    g_riSellName[0] = 0;
    invItemName(base, beg + (uintptr_t)slot * ITEM_STRIDE, type, itemId, key, g_riSellName);

    g_riSellGoldBefore = 0;
    resWalletAmount(base, resHash("gold"), &g_riSellGoldBefore);

    uintptr_t capture[2] = { panel, sys };
    bool ok = true;
    __try { ((RiSellDlgFn)(base + RI_SELLDLG_RVA))(capture, (uint32_t)slot); }
    __except (EXCEPTION_EXECUTE_HANDLER) { ok = false; }
    uint8_t ask = 1;
    safeReadU8(panel + RI_ASK_SELL_OFF, &ask);
    logLine("realminv: sell slot=%d \"%s\" ok=%d ask=%d gold=%d",
            slot, g_riSellName, ok ? 1 : 0, ask, g_riSellGoldBefore);
    if (!ok) {
        postSpeech(axs(AXS_RI_SALE_DIDNT_HAPPEN));
        return;
    }
    g_riSellWatchStart = GetTickCount();
    g_riSellDlgSeen = false;
    g_riSellSlot = slot;
}

static void riActivateAction(uintptr_t base, uintptr_t panel, const RiAction* act) {
    if (act->sortIdx < 0) {                            // unequip all
        if (g_riUneqWatchStart) return;
        uintptr_t sys = riSystem(base);
        g_riUneqCountBefore = 0;
        if (sys) safeReadU32(sys + RI_SYS_COUNT_OFF, (uint32_t*)&g_riUneqCountBefore);
        if (!frontEndClickElementId((int64_t)RI_ELEM_UNEQUIP)) {
            postSpeech(axs(AXS_NOT_AVAILABLE));
            return;
        }
        logLine("realminv: unequip-all clicked (count before=%d)", g_riUneqCountBefore);
        g_riUneqWatchStart = GetTickCount();
        g_riUneqDlgSeen = false;
        return;
    }
    if (g_riSortWatchUntil) return;
    char label[128] = { 0 };
    resolveKey(base, act->locKey, label, sizeof label);
    abStripMarkup(label);
    int before = -1;
    safeReadU32(panel + RI_SORTIDX_OFF, (uint32_t*)&before);
    if (!frontEndClickElementId((int64_t)(RI_ELEM_SORT_BASE + (uint32_t)act->sortIdx))) {
        postSpeech(axs(AXS_NOT_AVAILABLE));
        return;
    }
    logLine("realminv: %s clicked (idx=%d, was %d)", act->logName, act->sortIdx, before);
    strncpy(g_riSortName, label[0] ? label : act->logName, sizeof g_riSortName - 1);
    g_riSortName[sizeof g_riSortName - 1] = 0;
    if (before == act->sortIdx) {
        char utter[192];
        _snprintf(utter, sizeof utter, "%s.", g_riSortName);
        utter[sizeof utter - 1] = 0;
        postSpeech(utter);
        return;
    }
    g_riSortWatchUntil = GetTickCount() + 1500;
    g_riSortWantIdx = act->sortIdx;
}

// ---- the EQUIP flow: sheet slot -> this panel -> synthesized drag back onto the slot ----
static const uint32_t RI_GRID_OWNER_TAG   = 0x726c6d69;  // 'rlmi' — the grid cells' owner tag
static const uint32_t CS_ELEM_TRINKET_0   = 0x696e7632;  // "inv2" — sheet trinket slot 1
static const uint32_t CS_ELEM_TRINKET_1   = 0x696e7633;  // "inv3" — sheet trinket slot 2

uintptr_t csTownSheetPanel(uintptr_t base);       // defined with the sheet reader
static void riDumpGridFamily(uintptr_t base, int realSlot, int gridPos, int family, int64_t firstId) {
    static bool done = false;
    if (done || !axDebugLogEnabled()) return;
    done = true;
    logLine("rlmi-dump: family=%d firstId=0x%llx (asked realSlot=%d gridPos=%d)",
            family, (unsigned long long)firstId, realSlot, gridPos);
    for (int i = 0; i < family && i < 256; i++) {
        int64_t id = provSlotElem(base, RI_GRID_OWNER_TAG, i);
        uintptr_t el = id ? feGetElementById(id) : 0;
        float cx = 0, cy = 0;
        if (el && elemCenter(el, &cx, &cy))
            logLine("rlmi-dump:   [%3d] id=0x%llx centre=(%.0f,%.0f)",
                    i, (unsigned long long)id, cx, cy);
        else
            logLine("rlmi-dump:   [%3d] id=0x%llx (no element)", i, (unsigned long long)id);
    }
}

// ---- WHICH CELL HOLDS THIS TRINKET ----
static const uintptr_t RI_IFACE_PTR_OFF   = 0xc0;   // panel+: InventorySystemInterface*
static const uintptr_t IFACE_LIST_BEG_OFF = 0x10;   // iface+: m_InventorySlotInterfaceList begin
static const uintptr_t IFACE_LIST_END_OFF = 0x18;   // iface+: ... end
static const uintptr_t IFACE_SLOT_ITEM_OFF = 0x30;  // slot iface+: the ItemStack it is showing
static const int       RI_MAX_CELLS = 4096;         // sanity cap on the cell walk

// ---- DRAG-PICKUP PROBE ----
static const uintptr_t IFACE_HASITEM_OFF = 0x0c;    // slot iface+: byte, cell shows an item
static const uintptr_t IFACE_DRAGGED_OFF = 0xc0;    // slot iface+: byte, stack picked up
static bool g_riProbePickupSeen = false;            // one pickup log per armed watch

static bool riIfaceList(uintptr_t base, uintptr_t* begOut, long long* cellsOut) {
    uintptr_t panel = riPanel(base);
    if (!panel) return false;
    uintptr_t iface = 0, beg = 0, end = 0;
    if (!safeReadPtr(panel + RI_IFACE_PTR_OFF, &iface) || iface <= 0x10000) return false;
    if (!safeReadPtr(iface + IFACE_LIST_BEG_OFF, &beg) ||
        !safeReadPtr(iface + IFACE_LIST_END_OFF, &end) || !beg || end < beg) return false;
    long long cells = (long long)((end - beg) / 8);
    if (cells < 0) return false;
    if (cells > RI_MAX_CELLS) cells = RI_MAX_CELLS;
    *begOut = beg; *cellsOut = cells;
    return true;
}

static void riProbeDragPickup(uintptr_t base) {      // called from checkRealmInv while the watch runs
    if (g_riProbePickupSeen || !axDebugLogEnabled()) return;
    uintptr_t beg = 0; long long cells = 0;
    if (!riIfaceList(base, &beg, &cells)) return;
    for (long long i = 0; i < cells; i++) {
        uintptr_t slot = 0; uint8_t held = 0;
        if (!safeReadPtr(beg + (uintptr_t)i * 8, &slot) || slot <= 0x10000) continue;
        if (!safeReadU8(slot + IFACE_DRAGGED_OFF, &held) || !held) continue;
        uintptr_t stack = 0; uint32_t amt = 0;
        safeReadPtr(slot + IFACE_SLOT_ITEM_OFF, &stack);
        if (stack) safeReadU32(stack + ITEM_AMOUNT_OFF, &amt);
        char type[64] = {0}, itemId[64] = {0}, key[192] = {0}, name[256] = {0};
        if (stack) invItemName(base, stack, type, itemId, key, name);
        logLine("realminv-probe: PICKED UP iface[%lld] (stack=%p amt=%u id=\"%s\")",
                (long long)i, (void*)stack, amt, itemId[0] ? itemId : "(none)");
        g_riProbePickupSeen = true;
        return;
    }
}

static void riProbeCellPattern(uintptr_t base, int realSlot, uintptr_t wantItem) {
    if (!axDebugLogEnabled()) return;
    uintptr_t beg = 0; long long cells = 0;
    if (!riIfaceList(base, &beg, &cells)) return;
    char pat[161]; int n = 0;
    for (long long i = 0; i < cells && n < 160; i++, n++) {
        uintptr_t slot = 0; uint8_t has = 0;
        pat[n] = '?';
        if (safeReadPtr(beg + (uintptr_t)i * 8, &slot) && slot > 0x10000 &&
            safeReadU8(slot + IFACE_HASITEM_OFF, &has))
            pat[n] = has ? '1' : '0';
    }
    pat[n] = 0;
    logLine("realminv-probe: show-flags[0..%d]: %s", n - 1, pat);
    if (realSlot >= 0 && realSlot < (int)cells) {
        uintptr_t slot = 0, stack = 0; uint8_t has = 0, held = 0;
        safeReadPtr(beg + (uintptr_t)realSlot * 8, &slot);
        if (slot > 0x10000) {
            safeReadU8(slot + IFACE_HASITEM_OFF, &has);
            safeReadU8(slot + IFACE_DRAGGED_OFF, &held);
            safeReadPtr(slot + IFACE_SLOT_ITEM_OFF, &stack);
        }
        logLine("realminv-probe: target iface[%d]=%p show=%u held=%u stack=%p wantItem=%p (%s)",
                realSlot, (void*)slot, has, held, (void*)stack, (void*)wantItem,
                stack == wantItem ? "position-bound, as expected" : "STACK MISMATCH");
    }
}

// ---- EQUIP-DRAG WHEEL SERVO ----
static int64_t   g_riServoSrcId    = 0;      // 0 = idle
static DWORD     g_riServoDeadline = 0;
static float     g_riServoLastY    = 0;
static int       g_riServoStable   = 0;      // consecutive still frames (the ease has finished)
static int       g_riServoQuiet    = 0;      // wheels issued since the grid last moved
static const char* g_riServoWhich  = "";     // mapping label, for the drag log line
static int       g_riServoRealSlot = -1;
static uintptr_t g_riServoWantItem = 0;

static const float RI_BAND_TOP     = 380.0f;
static const float RI_BAND_BOT     = 730.0f;
static const float RI_WHEEL_PARK_Y = 560.0f;  // mid-band: always over the grid viewport

static bool riFireEquipDrag(uintptr_t base, float sx, float sy, const char* which) {
    uint32_t targetId = g_riEquipForSlot == 0 ? CS_ELEM_TRINKET_0 : CS_ELEM_TRINKET_1;
    uintptr_t dst = feGetElementById((int64_t)targetId);
    float tx = 0, ty = 0;
    if (!dst || !elemCenter(dst, &tx, &ty)) {
        logLine("realminv: equip drag, sheet slot element 0x%x not on screen", targetId);
        return false;
    }
    if (!synthDragPoints(sx, sy, tx, ty, "realminv equip")) return false;
    logLine("realminv: equip drag %s cell (%.0f,%.0f) -> sheet slot %d (%.0f,%.0f)",
            which, sx, sy, g_riEquipForSlot + 1, tx, ty);
    g_riProbePickupSeen = false;
    riProbeCellPattern(base, g_riServoRealSlot, g_riServoWantItem);
    return true;
}

bool riStartEquipDrag(uintptr_t base, int realSlot, int gridPos) {
    if (clickQueued()) return false;
    int64_t firstId = 0;
    int family = provSlotElemCount(base, RI_GRID_OWNER_TAG, &firstId);
    int shown = riSlotCount(base);
    uintptr_t sysv = riSystem(base), vbeg = 0; int rawSlots = 0;
    if (sysv) invItemVectorAt(sysv, &vbeg, &rawSlots);
    riDumpGridFamily(base, realSlot, gridPos, family, firstId);
    uintptr_t wantItem = (sysv && realSlot >= 0 && realSlot < rawSlots)
                       ? vbeg + (uintptr_t)realSlot * ITEM_STRIDE : 0;
    g_riEquipIdHash = 0;
    if (wantItem) safeReadU32(wantItem + ITEM_IDHASH_OFF, &g_riEquipIdHash);
    logLine("realminv: 'rlmi' family=%d firstId=0x%llx shown=%d rawSlots=%d -> cell = raw slot %d "
            "(gridPos=%d)", family, (unsigned long long)firstId, shown, rawSlots, realSlot, gridPos);
    if (family <= realSlot) {
        logLine("realminv: the 'rlmi' family (%d) has no cell for slot %d -- refusing to drag",
                family, realSlot);
        return false;
    }
    int64_t srcId = provSlotElem(base, RI_GRID_OWNER_TAG, realSlot);
    const char* which = "raw slot";
    if (!srcId) {
        logLine("realminv: 'rlmi' member %d did not resolve -- refusing to drag", realSlot);
        return false;
    }
    uintptr_t src = srcId ? feGetElementById(srcId) : 0;
    float sx = 0, sy = 0;
    if (!src || !elemCenter(src, &sx, &sy)) {
        logLine("realminv: equip drag, no 'rlmi' grid element (slot %d / pos %d, srcId=0x%llx)",
                realSlot, gridPos, (unsigned long long)srcId);
        return false;
    }
    g_riServoRealSlot = realSlot;
    g_riServoWantItem = wantItem;
    if (sy > RI_BAND_BOT || sy < RI_BAND_TOP) {
        g_riServoSrcId    = srcId;
        g_riServoDeadline = GetTickCount() + 6000;
        g_riServoLastY    = sy;
        g_riServoStable   = 0;
        g_riServoQuiet    = 0;
        g_riServoWhich    = which;
        moveCursorTo(sx, RI_WHEEL_PARK_Y);     // the wheel goes to the panel under the cursor
        logLine("realminv: cell (%.0f,%.0f) is outside the interactive band [%.0f..%.0f] -- "
                "wheel servo engaged (cursor parked at %.0f,%.0f)", sx, sy, RI_BAND_TOP,
                RI_BAND_BOT, sx, RI_WHEEL_PARK_Y);
        return true;                           // the caller arms the watch; the servo extends it
    }
    return riFireEquipDrag(base, sx, sy, which);
}

static void riServiceEquipServo(uintptr_t base) {
    if (!g_riServoSrcId) return;
    DWORD now = GetTickCount();
    uintptr_t src = feGetElementById(g_riServoSrcId);
    float sx = 0, sy = 0;
    if (!src || !elemCenter(src, &sx, &sy) || now > g_riServoDeadline) {
        logLine("realminv: wheel servo gave up (%s) -- the drag never fired",
                src ? "timed out" : "grid element gone");
        g_riServoSrcId = 0;
        g_riEquipWatchUntil = now;             // the watch speaks the failure next frame
        return;
    }
    g_riEquipWatchUntil = now + 2000;          // hold the outcome watch open while scrolling
    bool moved = (sy < g_riServoLastY - 1.0f) || (sy > g_riServoLastY + 1.0f);
    g_riServoLastY = sy;
    if (moved) {                               // the ease is still playing out: watch, don't act
        g_riServoStable = 0;
        g_riServoQuiet  = 0;                   // the wheel demonstrably works
        return;
    }
    if (++g_riServoStable < 3) return;         // just stopped: let it prove it stays stopped
    if (sy >= RI_BAND_TOP && sy <= RI_BAND_BOT) {
        g_riServoSrcId = 0;
        logLine("realminv: wheel servo settled in band (y=%.0f, %d still frames) -- firing "
                "the drag", sy, g_riServoStable);
        if (riFireEquipDrag(base, sx, sy, g_riServoWhich))
            g_riEquipWatchUntil = now + 2000;  // a fresh outcome window for the drag itself
        else
            g_riEquipWatchUntil = now;         // no drop target: speak the failure
        return;
    }
    if (g_riServoQuiet >= 6) {                 // six wheels from a still grid, zero movement
        logLine("realminv: wheel servo inert (y stuck at %.0f after %d wheels) -- the grid "
                "ignores the wheel; giving up", sy, g_riServoQuiet);
        g_riServoSrcId = 0;
        g_riEquipWatchUntil = now;
        return;
    }
    enqueueSynthWheel(sy > RI_BAND_BOT ? -1 : 1);   // SDL: negative wheels the content down
    g_riServoQuiet++;
    g_riServoStable = 0;                       // a step is in flight; require fresh stillness
}

// ---- HOVER TRACKING ----
static int64_t g_riHoverSrcId  = 0;
static float   g_riHoverLastY  = 0;
static int     g_riHoverStable = 0;      // consecutive still frames
static int     g_riHoverQuiet  = 0;
static bool    g_riHoverParked = false;  // cursor sent to the wheel park this cycle
static bool    g_riHoverWarped = false;  // cursor delivered onto the settled cell

static void riHoverRetarget(uintptr_t base, int realSlot) {
    g_riHoverSrcId  = realSlot >= 0 ? provSlotElem(base, RI_GRID_OWNER_TAG, realSlot) : 0;
    g_riHoverLastY  = -1.0e6f;
    g_riHoverStable = 0;
    g_riHoverQuiet  = 0;
    g_riHoverParked = false;
    g_riHoverWarped = false;
}

static void riHoverFromCursor(uintptr_t base, int nActs, const int* occ, int slots) {
    int g = g_riSlot - nActs;
    riHoverRetarget(base, (g >= 0 && g < slots) ? occ[g] : -1);
}

static void riServiceHoverTrack(uintptr_t base) {
    if (!g_riHoverSrcId) return;
    if (g_riServoSrcId || g_riEquipWatchUntil || clickQueued()) return;
    uintptr_t el = feGetElementById(g_riHoverSrcId);
    float sx = 0, sy = 0;
    if (!el || !elemCenter(el, &sx, &sy)) return;      // not registered this frame; retry next
    bool moved = (sy < g_riHoverLastY - 1.0f) || (sy > g_riHoverLastY + 1.0f);
    g_riHoverLastY = sy;
    if (moved) { g_riHoverStable = 0; g_riHoverQuiet = 0; return; }
    if (++g_riHoverStable < 2) return;                 // let the ease finish before acting
    if (sy >= RI_BAND_TOP && sy <= RI_BAND_BOT) {
        if (!g_riHoverWarped) {
            g_riHoverWarped = true;
            moveCursorTo(sx, sy);                      // hover-enter: the game's own cue fires
        }
        return;
    }
    if (g_riHoverQuiet >= 6) return;                   // the grid ignores the wheel: stay quiet
    if (!g_riHoverParked) {
        g_riHoverParked = true;
        moveCursorTo(sx, RI_WHEEL_PARK_Y);             // the wheel goes to the hovered panel
    }
    enqueueSynthWheel(sy > RI_BAND_BOT ? -1 : 1);
    g_riHoverQuiet++;
    g_riHoverStable = 0;
}

bool routeRealmInvKey(uintptr_t base, uint32_t sym, uint16_t mod, uint8_t repeat) {
    if (mod & (KMOD_LALT | KMOD_RALT | KMOD_LCTRL | KMOD_RCTRL)) return false;
    bool up    = (sym == SDLK_UP),   down  = (sym == SDLK_DOWN);
    bool left  = (sym == SDLK_LEFT), right = (sym == SDLK_RIGHT);
    bool enter = (sym == SDLK_RETURN || sym == SDLK_KP_ENTER);
    int jump = 0;
    bool isJump = axDecodeJump(sym, mod, repeat, &jump);   // Home/End: first/last of the REGION
    if (!up && !down && !left && !right && !enter && !isJump) return false;
    if (isJump && !jump) return true;                  // held jump: one landing per press
    if (enter) { if (repeat) return true; }
    else if (!isJump && axNavHoldRepeat(repeat)) return true;  // throttled repeat: claimed, no step

    uintptr_t panel = riPanel(base);
    if (!panel) return true;

    int nActs = riActionCount(base);
    int occ[RI_MAX_TRINKETS];
    int slots = riOccupiedSlots(base, occ, RI_MAX_TRINKETS);
    int total = nActs + slots;
    if (total <= 0) { logLine("realminv: nothing to navigate"); return true; }
    axStepCursor(&g_riSlot, total, 0);                 // the list shrank under the cursor
    int cols = riCols(base);

    if (isJump) {
        if (g_riSlot < nActs) g_riSlot = (jump > 0) ? nActs - 1 : 0;
        else                  g_riSlot = (jump > 0) ? total - 1 : nActs;
        riHoverFromCursor(base, nActs, occ, slots);    // keep the game's grid on the focus
        riSpeakRow(base, nullptr);
        return true;
    }

    if (enter) {
        if (g_riSlot < nActs) {                        // a top button
            if (mod & (KMOD_LSHIFT | KMOD_RSHIFT)) { riSpeakRow(base, nullptr); return true; }
            RiAction acts[RI_MAX_ACTIONS];
            riActions(base, panel, acts);
            riActivateAction(base, panel, &acts[g_riSlot]);
        } else {                                       // a trinket in the table
            int g = g_riSlot - nActs;
            if (g < 0 || g >= slots) { riSpeakRow(base, nullptr); return true; }
            if (mod & (KMOD_LSHIFT | KMOD_RSHIFT)) {   // Shift+Enter sells
                riSell(base, panel, occ[g]);
                return true;
            }
            if (g_riEquipForHero && g_riEquipForSlot >= 0) {
                if (g_riEquipWatchUntil) { postSpeech(axs(AXS_TRK_STILL_EQUIPPING)); return true; }
                // The pending target must still be there: the sheet open, on the same hero.
                if (!csTownSheetPanel(base) || abSelectedHero(base) != g_riEquipForHero) {
                    logLine("realminv: pending equip stale (sheet=%p hero=%p vs %p) -> cleared",
                            (void*)csTownSheetPanel(base), (void*)abSelectedHero(base),
                            (void*)g_riEquipForHero);
                    g_riEquipForHero = 0; g_riEquipForSlot = -1; g_riEquipCloseSheet = false;
                    postSpeech(axs(AXS_RI_SHEET_GONE));
                    return true;
                }
                uintptr_t sys = riSystem(base), beg = 0; int total2 = 0;
                if (!sys || !invItemVectorAt(sys, &beg, &total2) || occ[g] >= total2) return true;
                char type[64], itemId[64], key[192];
                g_riEquipName[0] = 0;
                invItemName(base, beg + (uintptr_t)occ[g] * ITEM_STRIDE, type, itemId, key,
                            g_riEquipName);
                g_riEquipHeroName[0] = 0;
                safeReadCStr(g_riEquipForHero + HERO_NAME_OFF, g_riEquipHeroName,
                             sizeof g_riEquipHeroName);
                g_riEquipCountBefore = slots;
                if (!riStartEquipDrag(base, occ[g], g)) {
                    postSpeech(axs(AXS_TRK_CANT_EQUIP_NOW));
                    return true;
                }
                g_riEquipWatchUntil = GetTickCount() + 2000;  // checkRealmInv pays the outcome
                return true;
            }
            uintptr_t sys = riSystem(base), beg = 0; int total2 = 0;
            if (!sys || !invItemVectorAt(sys, &beg, &total2) || occ[g] >= total2) {
                riSpeakRow(base, nullptr);
                return true;
            }
            char type[64], itemId[64], key[192], name[256];
            invItemName(base, beg + (uintptr_t)occ[g] * ITEM_STRIDE, type, itemId, key, name);
            trkPickBegin(base, occ[g], name[0] ? name : axs(AXS_TRK_THE_TRINKET));
        }
        return true;
    }

    int target = g_riSlot;
    if (g_riSlot < nActs) {                             // in the button row
        if (left)  target = g_riSlot > 0 ? g_riSlot - 1 : 0;               // hard stop
        if (right) target = g_riSlot < nActs - 1 ? g_riSlot + 1 : nActs - 1; // hard stop
        if (up)    target = g_riSlot;
        if (down)  target = slots > 0 ? nActs + (g_riSlot < slots ? g_riSlot : slots - 1)
                                      : g_riSlot;                            // into the table
    } else {                                           // in the trinket table
        int g = g_riSlot - nActs;
        if (left)  target = nActs + (g > 0 ? g - 1 : 0);                    // reading order, hard stop
        if (right) target = nActs + (g < slots - 1 ? g + 1 : slots - 1);    // hard stop
        if (up) {
            if (g < cols) target = nActs > 0 ? (g < nActs ? g : nActs - 1) : g_riSlot;  // into buttons
            else          target = nActs + (g - cols);
        }
        if (down) {
            int t = g + cols;
            if (t >= slots) t = slots - 1;             // bottom row: hard stop
            target = nActs + t;
        }
    }
    if (target < 0) target = 0;
    if (target >= total) target = total - 1;
    logLine("realminv nav %d -> %d (buttons=%d trinkets=%d cols=%d)",
            g_riSlot, target, nActs, slots, cols);
    g_riSlot = target;
    riHoverFromCursor(base, nActs, occ, slots);        // keep the game's grid on the focus
    riSpeakRow(base, nullptr);
    return true;
}

static bool riAskClose(uintptr_t base) {
    if (!riIsOpen(base)) return false;
    bool ok = true;
    __try { ((RiToggleFn)(base + RI_TOGGLE_RVA))(); }
    __except (EXCEPTION_EXECUTE_HANDLER) { ok = false; }
    logLine("realminv: close toggle issued -> %s", ok ? "called" : "FAULTED");
    return ok;
}

void riRequestEquipFor(uintptr_t base, uintptr_t hero, int slot) {
    g_riEquipForHero = hero;
    g_riEquipForSlot = slot;
    g_riEquipCloseSheet = false;
    char who[80] = { 0 };
    safeReadCStr(hero + HERO_NAME_OFF, who, sizeof who);
    logLine("realminv: pending equip armed, hero=%p \"%s\" slot %d (panel %s)",
            (void*)hero, who, slot + 1, riIsOpen(base) ? "already open" : "opening");
    if (riIsOpen(base)) {
        char prefix[192], head[160];
        _snprintf(head, sizeof head, axs(AXS_RI_CHOOSE_FOR_FMT),
                  who[0] ? who : axs(AXS_THE_HERO_LC), slot + 1);
        head[sizeof head - 1] = 0;
        _snprintf(prefix, sizeof prefix, "%s ", head);
        prefix[sizeof prefix - 1] = 0;
        riSpeakRow(base, prefix);
        return;
    }
    bool ok = true;
    __try { ((RiToggleFn)(base + RI_TOGGLE_RVA))(); }
    __except (EXCEPTION_EXECUTE_HANDLER) { ok = false; }
    if (!ok) {
        g_riEquipForHero = 0; g_riEquipForSlot = -1; g_riEquipCloseSheet = false;
        logLine("realminv: equip open toggle faulted");
        postSpeech(axs(AXS_RI_DIDNT_OPEN));
    }
    // The open edge in checkRealmInv announces, with the pending-equip prefix.
}

void checkRealmInv(uintptr_t base) {
    bool open = riIsOpen(base);
    if (open && !g_riActive) {
        g_riActive = true;
        g_riSellWatchStart = 0;
        g_riUneqWatchStart = 0;
        g_riSortWatchUntil = 0;
        g_riEquipWatchUntil = 0;
        g_riSheetCloseUntil = 0;
        g_riPanelCloseUntil = 0;                       // nor a hand-back the last panel never paid
        g_riBackLine[0] = 0;
        g_riBackSlot = -1;
        g_riServoSrcId = 0;                            // no servo survives a close/open edge
        g_riHoverSrcId = 0;
        if (g_trkPhase == 2 || g_trkPhase == 3) {
            logLine("realminv: opened behind the sheet for the trinket pick (phase %d) — silent",
                    g_trkPhase);
            return;
        }
        riUnlockWarnReset();
        riDumpStacks(base);
        g_riSlot = riActionCount(base);
        uintptr_t sys = riSystem(base);
        int count = riSlotCount(base);
        int rawCount = 0;
        if (sys) safeReadU32(sys + RI_SYS_COUNT_OFF, (uint32_t*)&rawCount);
        if (rawCount != count)
            logLine("realminv: %d stacks occupied, %d owned (circus lock hid %d)",
                    rawCount, count, rawCount - count);
        if (exIsOpen(base)) logLine("realminv: OVERLAP — exchange flag is also open");
        char prefix[192], head[160], cnt[64];
        _snprintf(cnt, sizeof cnt, axs(AXS_RI_COUNT_FMT), count);
        cnt[sizeof cnt - 1] = 0;
        if (g_riEquipForHero && g_riEquipForSlot >= 0) {
            // Opened by a sheet trinket slot: the header says what this pick is FOR.
            char who[80] = { 0 };
            safeReadCStr(g_riEquipForHero + HERO_NAME_OFF, who, sizeof who);
            _snprintf(head, sizeof head, axs(AXS_RI_CHOOSE_FOR_FMT),
                      who[0] ? who : axs(AXS_THE_HERO_LC), g_riEquipForSlot + 1);
        } else {
            strncpy(head, axs(AXS_RI_TITLE), sizeof head - 1);
        }
        head[sizeof head - 1] = 0;
        _snprintf(prefix, sizeof prefix, "%s %s ", head, cnt);
        prefix[sizeof prefix - 1] = 0;
        uintptr_t dbeg = 0, dend = 0; long vlen = -1;
        if (sys && safeReadPtr(sys + INV_ITEMS_BEG_OFF, &dbeg) &&
            safeReadPtr(sys + INV_ITEMS_END_OFF, &dend) && dend >= dbeg)
            vlen = (long)((dend - dbeg) / ITEM_STRIDE);
        logLine("realminv: opened, header=%d occupied=%d rawVectorLen=%ld (cap=%d)",
                count, riSlotCount(base), vlen, INV_MAX_SLOTS);
        {
            char hits[256]; int hp = 0; int found = 0;
            for (int i = 0; i < 12 && hp < 200; i++) {
                int64_t id = provSlotElem(base, RI_GRID_OWNER_TAG, i);
                if (!id) break;
                hp += _snprintf(hits + hp, sizeof hits - hp, "0x%llx ", (unsigned long long)id);
                found++;
            }
            hits[hp] = 0;
            logLine("realminv: 'rlmi' grid slots resolved: %d [%s]; sheet slots inv2=%s inv3=%s",
                    found, hits,
                    feGetElementById((int64_t)CS_ELEM_TRINKET_0) ? "live" : "absent",
                    feGetElementById((int64_t)CS_ELEM_TRINKET_1) ? "live" : "absent");
        }
        riSpeakRow(base, prefix);                      // land ON something, in one utterance
        return;
    }
    if (!open && g_riActive) {
        g_riActive = false;
        g_riSellWatchStart = 0;
        g_riUneqWatchStart = 0;
        g_riSortWatchUntil = 0;
        g_riEquipWatchUntil = 0;
        g_riSheetCloseUntil = 0;
        g_riServoSrcId = 0;                            // a pending scroll dies with the panel
        g_riHoverSrcId = 0;
        g_riEquipForHero = 0;                          // a pending equip dies with the panel
        g_riEquipForSlot = -1;
        g_riEquipCloseSheet = false;                   // ... and so does its "close the sheet after"
        if (g_trkPhase == 2) {
            logLine("realminv: closed by the sheet opener during the handoff (phase 2) — silent");
            return;
        }
        if (g_trkPhase) {
            logLine("realminv: closed during the trinket pick (phase %d) -> cancelled", g_trkPhase);
            trkPickCancel(base, (AxStrId)-1);
        }
        logLine("realminv: closed");
        if (g_riBackLine[0]) {
            char line[MAILBOX_SZ], pfx[MAILBOX_SZ];
            strncpy(line, g_riBackLine, sizeof line - 1);
            line[sizeof line - 1] = 0;
            int slot = g_riBackSlot;
            g_riBackLine[0] = 0;                       // consumed either way: never speak it twice
            g_riBackSlot = -1;
            g_riPanelCloseUntil = 0;
            _snprintf(pfx, sizeof pfx, "%s ", line);
            pfx[sizeof pfx - 1] = 0;
            if (csTownSheetPanel(base) && csFocusTrinketSlotWith(base, slot, pfx)) return;
            logLine("realminv: the sheet was gone on the hand-back -- the equip line goes out alone");
            postSpeech(line);
            return;
        }
        if (axIsRing() || axIsRingList()) {
            char pfx[96];
            _snprintf(pfx, sizeof pfx, "%s ", axs(AXS_RI_CLOSED));
            pfx[sizeof pfx - 1] = 0;
            ringReannounceWith(base, pfx);
            return;
        }
        postSpeech(axs(AXS_RI_CLOSED));
        return;
    }
    if (!g_riActive) return;

    riServiceEquipServo(base);                         // a pending scroll-into-view, if any
    riServiceHoverTrack(base);                         // keep the grid under the keyboard focus

    if (g_riEquipWatchUntil) {
        riProbeDragPickup(base);                       // did the game pick ANYTHING up, and where
        bool landed = false;
        uintptr_t hbeg = 0; int hslots = 0;
        if (g_riEquipForHero &&
            invItemVectorAt(g_riEquipForHero + HERO_TRINKET_SYSTEM_OFF, &hbeg, &hslots) &&
            g_riEquipForSlot >= 0 && g_riEquipForSlot < hslots) {
            uint32_t amt = 0;
            safeReadU32(hbeg + (uintptr_t)g_riEquipForSlot * ITEM_STRIDE + ITEM_AMOUNT_OFF, &amt);
            landed = (int32_t)amt > 0;
        }
        int occ[RI_MAX_TRINKETS];
        int nowCount = riOccupiedSlots(base, occ, RI_MAX_TRINKETS);
        if (landed) {
            uintptr_t landedItem = hbeg + (uintptr_t)g_riEquipForSlot * ITEM_STRIDE;
            uint32_t landedHash = 0;
            char ltype[64] = {0}, lid[64] = {0}, lkey[192] = {0}, lname[256] = {0};
            safeReadU32(landedItem + ITEM_IDHASH_OFF, &landedHash);
            invItemName(base, landedItem, ltype, lid, lkey, lname);
            bool mismatch = g_riEquipIdHash && landedHash && landedHash != g_riEquipIdHash;
            const char* spoken = lname[0] ? lname
                                          : (g_riEquipName[0] ? g_riEquipName
                                                              : axs(AXS_TRK_THE_TRINKET));
            char utter[MAILBOX_SZ];
            _snprintf(utter, sizeof utter, axs(AXS_RI_EQUIPPED_FMT),
                      spoken,
                      g_riEquipHeroName[0] ? g_riEquipHeroName : axs(AXS_THE_HERO_LC),
                      g_riEquipForSlot + 1);
            utter[sizeof utter - 1] = 0;
            if (mismatch)
                logLine("realminv: ⚠ WRONG TRINKET LANDED -- asked for \"%s\" (hash 0x%08x), got "
                        "\"%s\" (id \"%s\", hash 0x%08x). The 'rlmi' cell mapping is wrong.",
                        g_riEquipName, g_riEquipIdHash, lname, lid, landedHash);
            logLine("realminv: equip observed (hero slot filled; realm count %d -> %d)",
                    g_riEquipCountBefore, nowCount);
            g_riEquipWatchUntil = 0;
            int backSlot = g_riEquipForSlot;           // ... which the hand-back lands on
            g_riEquipForHero = 0;                      // done: the pending target is consumed
            g_riEquipForSlot = -1;
            if (g_riSlot >= riActionCount(base) + nowCount)          // the list shrank under us
                g_riSlot = riActionCount(base) + (nowCount > 0 ? nowCount - 1 : 0);
            if (g_riEquipCloseSheet) {
                g_riEquipCloseSheet = false;
                if (csCloseTownSheet(base)) g_riSheetCloseUntil = GetTickCount() + 2000;
                postSpeech(utter);
            } else if (riAskClose(base)) {
                strncpy(g_riBackLine, utter, sizeof g_riBackLine - 1);
                g_riBackLine[sizeof g_riBackLine - 1] = 0;
                g_riBackSlot = backSlot;
                g_riPanelCloseUntil = GetTickCount() + 2000;
            } else {
                postSpeech(utter);                     // the close never went out: say it here
            }
        } else if (GetTickCount() > g_riEquipWatchUntil) {
            g_riEquipWatchUntil = 0;                   // pending stays armed for another try
            logLine("realminv: equip watch timed out (realm count %d -> %d)",
                    g_riEquipCountBefore, nowCount);
            char utter[MAILBOX_SZ];
            _snprintf(utter, sizeof utter, axs(AXS_RI_CANT_TAKE_FMT),
                      g_riEquipHeroName[0] ? g_riEquipHeroName : axs(AXS_THE_HERO),
                      g_riEquipName[0] ? g_riEquipName : axs(AXS_RI_THAT_TRINKET));
            utter[sizeof utter - 1] = 0;
            postSpeech(utter);
        }
    }

    if (g_riSheetCloseUntil) {
        if (!csTownSheetPanel(base)) {
            g_riSheetCloseUntil = 0;
            logLine("realminv: the character sheet closed after the equip");
        } else if (GetTickCount() > g_riSheetCloseUntil) {
            g_riSheetCloseUntil = 0;
            logLine("realminv: the character sheet did NOT close after the equip (state still up)");
            postSpeech(axs(AXS_RI_SHEET_STILL_OPEN));
        }
    }

    if (g_riPanelCloseUntil && GetTickCount() > g_riPanelCloseUntil) {
        g_riPanelCloseUntil = 0;
        logLine("realminv: the grid did NOT close after the sheet-first equip (still up) -- "
                "the equip line goes out here instead of with the hand-back");
        if (g_riBackLine[0]) postSpeech(g_riBackLine);
        g_riBackLine[0] = 0;
        g_riBackSlot = -1;
    }

    if (g_riSellWatchStart) {
        int goldNow = 0;
        resWalletAmount(base, resHash("gold"), &goldNow);
        uintptr_t sys = riSystem(base), beg = 0; int slots = 0;
        uint32_t amount = 1;
        if (sys && invItemVectorAt(sys, &beg, &slots) && g_riSellSlot < slots)
            safeReadU32(beg + (uintptr_t)g_riSellSlot * ITEM_STRIDE + ITEM_AMOUNT_OFF, &amount);
        if (goldNow > g_riSellGoldBefore || (int32_t)amount < 1) {
            char utter[MAILBOX_SZ], goldName[64];
            resCurrencyTitleById(base, "gold", "realminv", goldName, sizeof goldName);
            _snprintf(utter, sizeof utter, axs(AXS_RI_SOLD_FMT),
                      g_riSellName, goldNow - g_riSellGoldBefore, goldName,
                      goldNow, goldName);
            utter[sizeof utter - 1] = 0;
            logLine("realminv: sale observed, gold %d -> %d", g_riSellGoldBefore, goldNow);
            g_riSellWatchStart = 0;
            postSpeech(utter);
        } else if (axIsDialog()) {
            g_riSellDlgSeen = true;
        } else if (g_riSellDlgSeen) {
            logLine("realminv: sell dialog closed without a sale");
            g_riSellWatchStart = 0;
            postSpeech(axs(AXS_RI_NOT_SOLD));
        } else if (GetTickCount() > g_riSellWatchStart + 1500) {
            logLine("realminv: sell produced neither dialog nor sale");
            g_riSellWatchStart = 0;
            postSpeech(axs(AXS_RI_SALE_DIDNT_HAPPEN));
        }
    }

    if (g_riUneqWatchStart) {
        uintptr_t sys = riSystem(base);
        int countNow = g_riUneqCountBefore;
        if (sys) safeReadU32(sys + RI_SYS_COUNT_OFF, (uint32_t*)&countNow);
        if (countNow > g_riUneqCountBefore) {
            char utter[192];
            _snprintf(utter, sizeof utter, axs(AXS_RI_RETURNED_FMT),
                      countNow - g_riUneqCountBefore);
            utter[sizeof utter - 1] = 0;
            logLine("realminv: unequip observed, count %d -> %d", g_riUneqCountBefore, countNow);
            g_riUneqWatchStart = 0;
            postSpeech(utter);
        } else if (axIsDialog()) {
            g_riUneqDlgSeen = true;
        } else if (g_riUneqDlgSeen) {
            logLine("realminv: unequip dialog closed without a change");
            g_riUneqWatchStart = 0;
            postSpeech(axs(AXS_RI_NOTHING_RETURNED));
        } else if (GetTickCount() > g_riUneqWatchStart + 1500) {
            logLine("realminv: unequip click produced no dialog");
            g_riUneqWatchStart = 0;
            postSpeech(axs(AXS_DIDNT_HAPPEN));
        }
    }

    // Sort watcher: the click body writes the applied button index to panel+0x518.
    if (g_riSortWatchUntil) {
        uintptr_t panel = riPanel(base);
        int idxNow = -1;
        if (panel) safeReadU32(panel + RI_SORTIDX_OFF, (uint32_t*)&idxNow);
        if (idxNow == g_riSortWantIdx) {
            char utter[192];
            _snprintf(utter, sizeof utter, "%s.", g_riSortName);
            utter[sizeof utter - 1] = 0;
            logLine("realminv: sort observed, idx=%d", idxNow);
            g_riSortWatchUntil = 0;
            postSpeech(utter);
        } else if (GetTickCount() > g_riSortWatchUntil) {
            logLine("realminv: sort click made no change (want=%d, is=%d)",
                    g_riSortWantIdx, idxNow);
            g_riSortWatchUntil = 0;
            postSpeech(axs(AXS_RI_SORT_DIDNT_HAPPEN));
        }
    }
}
