// town/events.cpp -- the eleventh TOWN slice

#include <cstdint>
#include <cstring>
#include <cstdio>
#include "internal.h"
// ---- Town-event popup (TownUI::Panel::TownEventDisplay) ----
static const uintptr_t TE_EVT_ID_OFF   = 0x00;
static const uintptr_t TE_EVT_TEXID_OFF   = 0x218;
static const uintptr_t TE_EVT_TOOLTIP_OFF = 0x298;
static const uintptr_t TE_FOCUS_HANDLES[3] = { 0x140, 0x150, 0x160 };   // (were 0x148, 0x158, 0x168)

// ---- Town-event popup: derive the active event id, resolve, speak ----
static bool townEventActiveId(uintptr_t base, char* idOut, size_t idsz,
                              uintptr_t* entryOut, uint32_t* hashOut) {
    if (entryOut) *entryOut = 0;
    if (hashOut)  *hashOut  = 0;
    if (!idOut || idsz == 0) return false;
    idOut[0] = 0;
    uintptr_t camp = 0;
    uint32_t active = 0;
    if (!safeReadPtr(base + RES_CAMPAIGN_RVA, &camp) || camp <= 0x10000) return false;
    if (!safeReadU32(camp + PROV_CAMP_EVENT_OFF, &active) || active == 0) return false;  // no event

    uintptr_t reg = 0, eb = 0, ee = 0;
    if (!safeReadPtr(base + PROV_EVTREG_RVA, &reg) || reg <= 0x10000) return false;
    if (!safeReadPtr(reg + PROV_EVTREG_BEG_OFF, &eb) ||
        !safeReadPtr(reg + PROV_EVTREG_END_OFF, &ee) || !eb || ee <= eb ||
        (ee - eb) % PROV_EVT_STRIDE != 0) return false;
    int nev = (int)((ee - eb) / PROV_EVT_STRIDE);
    if (nev > 256) nev = 256;
    for (int i = 0; i < nev; i++) {
        uintptr_t evt = eb + (uintptr_t)i * PROV_EVT_STRIDE;
        uint32_t idh = 0;
        if (!safeReadU32(evt + PROV_EVT_IDHASH_OFF, &idh) || idh != active) continue;
        char id[64];
        if (!safeReadCStr(evt + TE_EVT_ID_OFF, id, sizeof id) || !id[0]) {
            logLine("townevent: entry hash=0x%x has no id@+0x00", active);
            return false;
        }
        if (resHash(id) != active) {
            logLine("townevent: id=\"%s\" hash mismatch (got 0x%x want 0x%x)",
                    id, resHash(id), active);
            return false;
        }
        for (const char* p = id; *p; p++) {
            char c = *p;
            if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') || c == '_')) {
                logLine("townevent: bad id=\"%s\"", id);
                return false;
            }
        }
        strncpy(idOut, id, idsz - 1);
        idOut[idsz - 1] = 0;
        if (entryOut) *entryOut = evt;
        if (hashOut)  *hashOut  = active;
        return true;
    }
    logLine("townevent: active hash=0x%x not found in registry (%d entries)", active, nev);
    return false;
}

static bool townEventInfoLine(uintptr_t base, uint32_t hash, char* out, int outsz) {
    if (out && outsz) out[0] = 0;
    if (!hash) return false;
    typedef char* (__fastcall *TEInfoFn)(char*, int);
    TEInfoFn build = reinterpret_cast<TEInfoFn>(base + TE_INFO_RVA);
    char raw[512] = { 0 };
    __try { build(raw, (int)hash); }
    __except (EXCEPTION_EXECUTE_HANDLER) { logLine("townevent: info builder faulted"); return false; }
    raw[sizeof raw - 1] = 0;
    if (!raw[0]) return false;
    stripMarkup(raw, out, outsz);
    return out[0] != 0;
}

static bool townEventInteractionLabel(uintptr_t base, uintptr_t entry, char* out, int outsz) {
    if (out && outsz) out[0] = 0;
    if (!entry) return false;
    char key[96] = { 0 };
    if (!safeReadCStr(entry + TE_EVT_TOOLTIP_OFF, key, sizeof key) || !key[0]) return false;
    for (const char* p = key; *p; p++)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '_')) return false;
    char raw[512] = { 0 };
    if (!resolveKey(base, key, raw, sizeof raw)) return false;
    stripMarkup(raw, out, outsz);
    return out[0] != 0;
}

// ---- The popup's HERO SLOTS — the events that GRANT A HERO ----
static const uintptr_t TE_SYS_OFF         = 0x1808;   // Campaign+: the town-event system (was 0x1768)
static const uintptr_t TE_SYS_BONUS_BEG   = 0x38;     // sys+: vector<RecruitEntry> BY VALUE, begin
static const uintptr_t TE_SYS_BONUS_END   = 0x40;     // sys+: ... end
static const uintptr_t TE_RECRUIT_STRIDE  = 0x1540;
static const uintptr_t TE_RECRUIT_LIVE    = 0x1538;   // entry+: nonzero = still UNCLAIMED (was 0x14b8)
static const uintptr_t TE_SYS_DEAD_BEG    = 0x68;     // sys+: dead-hero grants ("From Beyond"),
static const uintptr_t TE_SYS_DEAD_END    = 0x70;     //       guid+flag pairs
static const uintptr_t TE_DEAD_STRIDE     = 0x08;
static const uintptr_t TE_DEAD_LIVE       = 0x04;     // pair+: nonzero = still unclaimed
static const uint32_t  TE_ELEM_SLOT_BASE  = 0x746568;

static int       g_teHeroRow   = 0;
static int       g_teHeroCount = 0;
static int       g_teRecArm    = 0;
static uintptr_t g_teRecHero   = 0;    // the hero the armed recruit holds
static int       g_teRecIndex  = -1;
static char      g_teRecName[64];      // spoken name, read once at arm time
static DWORD     g_teRecWatchUntil = 0;// outcome watch; 0 = idle
static int       g_teRecBefore = -1;   // roster entry count before the drop
static DWORD     g_teSheetWatchUntil = 0; // C's sheet-open watch; 0 = idle
static bool      g_teSheetResume = false;

static uintptr_t teSystem(uintptr_t base) {
    uintptr_t camp = 0;
    if (!safeReadPtr(base + RES_CAMPAIGN_RVA, &camp) || camp <= 0x10000) return 0;
    if (TE_SYS_OFF + 0x30 != PROV_CAMP_EVENT_OFF) {
        logLine("townevent: SYS OFFSET INVARIANT BROKEN (0x%llx + 0x30 != 0x%llx)",
                (unsigned long long)TE_SYS_OFF, (unsigned long long)PROV_CAMP_EVENT_OFF);
        return 0;
    }
    return camp + TE_SYS_OFF;
}

// ---- The FREE-UPGRADE grant ----
static const uintptr_t TE_SYS_FREEUPG_OFF = 0x88;   // sys+: map<uint32 tagHash,int> free_upgrade_tags
static const uintptr_t TE_MAPN_LEFT   = 0x00;       // _Tree node: _Left
static const uintptr_t TE_MAPN_PARENT = 0x08;       //             _Parent (from _Myhead: the ROOT)
static const uintptr_t TE_MAPN_RIGHT  = 0x10;       //             _Right
static const uintptr_t TE_MAPN_ISNIL  = 0x19;       //             _Isnil (1 = the head sentinel)
static const uintptr_t TE_MAPN_KEY    = 0x1c;       //             the pair, 4-byte aligned
static const uintptr_t TE_MAPN_VAL    = 0x20;
static const int       TE_FREEUPG_MAX = 64;         // node cap; the shipped data ships four tags

static char g_teFreeSig[192] = { 0 };   // last logged map contents ("" = never seen one)

int townEventFreeUpgrades(uintptr_t base, const char* tag) {
    if (!tag || !tag[0]) return 0;
    uintptr_t sys = teSystem(base);
    if (!sys) return 0;
    uintptr_t head = 0;
    if (!safeReadPtr(sys + TE_SYS_FREEUPG_OFF, &head) || head <= 0x10000) return 0;
    uintptr_t root = 0;
    if (!safeReadPtr(head + TE_MAPN_PARENT, &root) || root <= 0x10000) return 0;
    uint8_t nil = 1;
    if (!safeReadU8(root + TE_MAPN_ISNIL, &nil) || nil) return 0;   // an empty map: root == _Myhead

    const uint32_t want = resHash(tag);
    uintptr_t stk[TE_FREEUPG_MAX];
    int sp = 0, seen = 0, found = 0;
    char sig[sizeof g_teFreeSig];
    int sigLen = 0;
    sig[0] = 0;
    stk[sp++] = root;
    while (sp > 0 && seen < TE_FREEUPG_MAX) {
        uintptr_t n = stk[--sp];
        seen++;
        uint32_t key = 0, val = 0;
        if (safeReadU32(n + TE_MAPN_KEY, &key) && safeReadU32(n + TE_MAPN_VAL, &val)) {
            if (sigLen < (int)sizeof sig - 24) {
                int w = _snprintf(sig + sigLen, sizeof sig - sigLen, "0x%08x=%d;", key, (int)val);
                sigLen = (w < 0) ? (int)sizeof sig - 1 : sigLen + w;
            }
            // The game itself skips a tag whose count has run out, so we do too.
            if (key == want && (int)val > 0) found = (int)val;
        }
        for (int side = 0; side < 2 && sp < TE_FREEUPG_MAX; side++) {
            uintptr_t kid = 0;
            uint8_t knil = 1;
            if (!safeReadPtr(n + (side ? TE_MAPN_RIGHT : TE_MAPN_LEFT), &kid) || kid <= 0x10000)
                continue;
            if (!safeReadU8(kid + TE_MAPN_ISNIL, &knil) || knil) continue;   // the nil sentinel
            stk[sp++] = kid;
        }
    }
    sig[sizeof sig - 1] = 0;
    if (strcmp(sig, g_teFreeSig) != 0) {
        strncpy(g_teFreeSig, sig, sizeof g_teFreeSig - 1);
        g_teFreeSig[sizeof g_teFreeSig - 1] = 0;
        logLine("townevent: free_upgrade_tags = %s (%d nodes; building=0x%08x weapon=0x%08x "
                "armour=0x%08x)", sig[0] ? sig : "(none)", seen,
                resHash("building"), resHash("weapon"), resHash("armour"));
    }
    return found;
}

static int teHeroCount(uintptr_t base) {
    uintptr_t sys = teSystem(base);
    if (!sys) return 0;
    int n = 0;
    uintptr_t b = 0, e = 0;
    if (safeReadPtr(sys + TE_SYS_BONUS_BEG, &b) && safeReadPtr(sys + TE_SYS_BONUS_END, &e) &&
        b > 0x10000 && e >= b && (e - b) % TE_RECRUIT_STRIDE == 0 &&
        (e - b) / TE_RECRUIT_STRIDE <= 32) {
        for (uintptr_t p = b; p < e; p += TE_RECRUIT_STRIDE) {
            uint8_t live = 0;
            if (safeReadU8(p + TE_RECRUIT_LIVE, &live) && live) n++;
        }
    }
    if (safeReadPtr(sys + TE_SYS_DEAD_BEG, &b) && safeReadPtr(sys + TE_SYS_DEAD_END, &e) &&
        b > 0x10000 && e >= b && (e - b) % TE_DEAD_STRIDE == 0 &&
        (e - b) / TE_DEAD_STRIDE <= 64) {
        for (uintptr_t p = b; p < e; p += TE_DEAD_STRIDE) {
            uint8_t live = 0;
            if (safeReadU8(p + TE_DEAD_LIVE, &live) && live) n++;
        }
    }
    return n;
}

typedef uintptr_t (*TEHeroAtFn)(uintptr_t, int);
static uintptr_t teHeroAt(uintptr_t base, int index) {
    uintptr_t sys = teSystem(base);
    if (!sys || index < 0) return 0;
    uintptr_t hero = 0;
    __try { hero = ((TEHeroAtFn)(base + TE_HERO_AT_RVA))(sys, index); }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        logLine("townevent: hero accessor faulted at index %d", index);
        return 0;
    }
    return (hero > 0x10000) ? hero : 0;
}

static void teLogHeroLists(uintptr_t base, const char* eventId) {
    uintptr_t sys = teSystem(base);
    if (!sys) return;
    int bonusN = 0, bonusLive = 0, deadN = 0, deadLive = 0;
    uintptr_t b = 0, e = 0;
    if (safeReadPtr(sys + TE_SYS_BONUS_BEG, &b) && safeReadPtr(sys + TE_SYS_BONUS_END, &e) &&
        b > 0x10000 && e >= b && (e - b) % TE_RECRUIT_STRIDE == 0 &&
        (e - b) / TE_RECRUIT_STRIDE <= 32) {
        for (uintptr_t p = b; p < e; p += TE_RECRUIT_STRIDE) {
            uint8_t live = 0;
            bonusN++;
            if (safeReadU8(p + TE_RECRUIT_LIVE, &live) && live) bonusLive++;
        }
    }
    if (safeReadPtr(sys + TE_SYS_DEAD_BEG, &b) && safeReadPtr(sys + TE_SYS_DEAD_END, &e) &&
        b > 0x10000 && e >= b && (e - b) % TE_DEAD_STRIDE == 0 &&
        (e - b) / TE_DEAD_STRIDE <= 64) {
        for (uintptr_t p = b; p < e; p += TE_DEAD_STRIDE) {
            uint8_t live = 0;
            deadN++;
            if (safeReadU8(p + TE_DEAD_LIVE, &live) && live) deadLive++;
        }
    }
    logLine("townevent SLOTS: event=%s bonus_hero_entries %d live of %d, dead grants %d live of %d",
            eventId, bonusLive, bonusN, deadLive, deadN);
    for (int i = 0; i < bonusLive + deadLive && i < 16; i++) {
        uintptr_t h = teHeroAt(base, i);
        char name[64] = { 0 };
        if (h) safeReadCStr(h + HERO_NAME_OFF, name, sizeof name);
        logLine("townevent SLOTS: index %d -> hero %p \"%s\"", i, (void*)h, name[0] ? name : "?");
    }
}

// ---- The event's PRICE (`event_cost`) ----
static const uintptr_t TE_SYS_COST_BEG  = 0x50;
static const uintptr_t TE_SYS_COST_END  = 0x58;     // sys+: ... end
static const uintptr_t TE_COST_STRIDE   = 0x48;     // == DST_COST_STRIDE / RES_WALLET_STRIDE
static const uintptr_t TE_COST_AMOUNT   = 0x00;     // cost+: int amount
static const uintptr_t TE_COST_HASH     = 0x44;     // cost+: gameHash(currency id)
static const uintptr_t TE_LEDGER_OFF    = 0xe50;

struct TeCost { uint32_t hash; int amount; int have; };

static int teReadCosts(uintptr_t base, TeCost* out, int max) {
    uintptr_t sys = teSystem(base);
    if (!sys) return 0;
    uintptr_t beg = 0, end = 0;
    if (!safeReadPtr(sys + TE_SYS_COST_BEG, &beg) || !safeReadPtr(sys + TE_SYS_COST_END, &end) ||
        !beg || end <= beg || (end - beg) % TE_COST_STRIDE) return 0;
    int n = (int)((end - beg) / TE_COST_STRIDE);
    if (n > 8) return 0;                                  // not this layout -> trust nothing
    int got = 0;
    for (int i = 0; i < n && got < max; i++) {
        uintptr_t rec = beg + (uintptr_t)i * TE_COST_STRIDE;
        uint32_t amt = 0, hash = 0;
        if (!safeReadU32(rec + TE_COST_AMOUNT, &amt) || !safeReadU32(rec + TE_COST_HASH, &hash))
            continue;
        if ((int32_t)amt <= 0) continue;
        out[got].hash = hash;
        out[got].amount = (int)amt;
        out[got].have = 0;
        resWalletAmount(base, hash, &out[got].have);      // absent = 0
        got++;
    }
    return got;
}

typedef uint8_t (*TeAffordFn)(uintptr_t ledger, uintptr_t costVec);
static int teCanAfford(uintptr_t base) {
    uintptr_t camp = 0, sys = teSystem(base);
    if (!sys || !safeReadPtr(base + RES_CAMPAIGN_RVA, &camp) || camp <= 0x10000) return -1;
    __try {
        return ((TeAffordFn)(base + ACT_CANAFFORD_RVA))(camp + TE_LEDGER_OFF,
                                                        sys + TE_SYS_COST_BEG) ? 1 : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
}

static void teCostLine(uintptr_t base, char* out, int outsz) {
    out[0] = 0;
    TeCost c[4];
    int n = teReadCosts(base, c, 4), len = 0;
    for (int i = 0; i < n && len < outsz - 1; i++) {
        char cur[96];
        resCurrencyTitle(base, c[i].hash, "townevent", cur, sizeof cur);
        int w = _snprintf(out + len, outsz - len, "%s" , len ? " " : "");
        len = (w < 0) ? outsz - 1 : len + w;
        w = _snprintf(out + len, outsz - len, axs(AXS_BLD_COSTS_AMOUNT_FMT), c[i].amount, cur);
        len = (w < 0) ? outsz - 1 : len + w;
    }
    out[outsz - 1] = 0;
}

static void teSpeakHeroRow(uintptr_t base, int row, int count, const char* prefix) {
    uintptr_t hero = teHeroAt(base, row);
    char card[512] = { 0 };
    if (hero) ptyHeroFrag(base, hero, card, sizeof card);
    char cost[160];
    teCostLine(base, cost, sizeof cost);
    char utter[MAILBOX_SZ];
    char pos[64];
    _snprintf(pos, sizeof pos, axs(AXS_POS_N_OF_M), row + 1, count);
    pos[sizeof pos - 1] = 0;
    _snprintf(utter, sizeof utter, "%s%s. %s%s%s",
              prefix ? prefix : "", card[0] ? card : axs(AXS_TE_A_HERO),
              cost, cost[0] ? " " : "", pos);
    utter[sizeof utter - 1] = 0;
    postSpeech(utter);
}

static bool teLogElemById(uintptr_t base, int64_t wantId, const char* label) {
    uintptr_t begin = 0, end = 0;
    if (!safeReadPtr(base + VEC_BEGIN_RVA, &begin) ||
        !safeReadPtr(base + VEC_END_RVA, &end) || !begin || end <= begin) return false;
    uintptr_t count = (end - begin) / ELEM_STRIDE;
    if (count > 4096) count = 4096;
    for (uintptr_t i = 0; i < count; i++) {
        uintptr_t elem = begin + i * ELEM_STRIDE;
        int64_t eid = 0;
        if (!safeReadI64(elem + ELEM_ID_OFF, &eid) || eid != wantId) continue;
        uintptr_t owner = 0;
        uint32_t xb = 0, yb = 0, wb = 0, hb = 0;
        safeReadPtr(elem + ELEM_OWNER_OFF, &owner);
        safeReadU32(elem + ELEM_POS_OFF, &xb);  safeReadU32(elem + ELEM_POS_OFF + 4, &yb);
        safeReadU32(elem + ELEM_SIZE_OFF, &wb); safeReadU32(elem + ELEM_SIZE_OFF + 4, &hb);
        logLine("townevent DUMP: %s id=0x%llx FOUND elem=0x%llx owner=0x%llx pos=(%.0f,%.0f) size=(%.0f,%.0f)",
                label, (unsigned long long)wantId, (unsigned long long)elem, (unsigned long long)owner,
                u32AsFloatM(xb), u32AsFloatM(yb), u32AsFloatM(wb), u32AsFloatM(hb));
        return true;
    }
    logLine("townevent DUMP: %s id=0x%llx NOT in focus vector", label, (unsigned long long)wantId);
    return false;
}

static char g_teDumpedId[64] = { 0 };
static void townEventDumpOnce(uintptr_t base, uintptr_t self, const char* id,
                              uintptr_t entry, uint32_t hash) {
    if (!id || strcmp(id, g_teDumpedId) == 0) return;   // once per distinct event

    int32_t hid[3] = { -1, -1, -1 };
    bool ready = false;
    if (self > 0x10000) {
        for (int i = 0; i < 3; i++) {
            uint32_t v = 0;
            if (safeReadU32(self + TE_FOCUS_HANDLES[i], &v)) hid[i] = (int32_t)v;
            if (hid[i] != -1 && hid[i] != -0x4d2 && hid[i] != 0) ready = true;
        }
    }
    if (!ready) return;                                 // handles not registered yet — wait a frame

    strncpy(g_teDumpedId, id, sizeof g_teDumpedId - 1);
    g_teDumpedId[sizeof g_teDumpedId - 1] = 0;

    char texId[96] = { 0 }, tipKey[96] = { 0 }, info[512] = { 0 };
    safeReadCStr(entry + TE_EVT_TEXID_OFF, texId, sizeof texId);
    safeReadCStr(entry + TE_EVT_TOOLTIP_OFF, tipKey, sizeof tipKey);
    townEventInfoLine(base, hash, info, sizeof info);
    logLine("townevent DUMP id=%s self=0x%llx entry=0x%llx hash=0x%x texId=\"%s\" tipKey=\"%s\" info=\"%s\"",
            id, (unsigned long long)self, (unsigned long long)entry, hash, texId, tipKey, info);

    const char* hn[3] = { "handle+0x148", "handle+0x158", "handle+0x168" };
    for (int i = 0; i < 3; i++) {
        uint32_t gen = 0; safeReadU32(self + TE_FOCUS_HANDLES[i] + 4, &gen);
        logLine("townevent DUMP: %s id=0x%x gen=0x%x", hn[i], (uint32_t)hid[i], gen);
        if (hid[i] != -1 && hid[i] != -0x4d2 && hid[i] != 0)
            teLogElemById(base, (int64_t)(uint32_t)hid[i], hn[i]);
    }

    uintptr_t begin = 0, end = 0;
    if (safeReadPtr(base + VEC_BEGIN_RVA, &begin) &&
        safeReadPtr(base + VEC_END_RVA, &end) && begin && end > begin) {
        uintptr_t count = (end - begin) / ELEM_STRIDE;
        if (count > 4096) count = 4096;
        logLine("townevent DUMP: %llu focus elements (id/owner/rect):", (unsigned long long)count);
        for (uintptr_t i = 0; i < count; i++) {
            uintptr_t elem = begin + i * ELEM_STRIDE;
            int64_t eid = 0; uintptr_t owner = 0;
            if (!safeReadI64(elem + ELEM_ID_OFF, &eid) || isNoFocus(eid)) continue;
            safeReadPtr(elem + ELEM_OWNER_OFF, &owner);
            uint32_t xb = 0, yb = 0, wb = 0, hb = 0;
            safeReadU32(elem + ELEM_POS_OFF, &xb);  safeReadU32(elem + ELEM_POS_OFF + 4, &yb);
            safeReadU32(elem + ELEM_SIZE_OFF, &wb); safeReadU32(elem + ELEM_SIZE_OFF + 4, &hb);
            uint32_t cc = (uint32_t)(uint64_t)eid;
            char fourcc[5] = { (char)(cc & 0xff), (char)((cc >> 8) & 0xff),
                               (char)((cc >> 16) & 0xff), (char)((cc >> 24) & 0xff), 0 };
            for (int k = 0; k < 4; k++) if (fourcc[k] < 32 || fourcc[k] > 126) fourcc[k] = '.';
            logLine("townevent DUMP: elem id=0x%llx (%s) owner=0x%llx pos=(%.0f,%.0f) size=(%.0f,%.0f)",
                    (unsigned long long)eid, fourcc, (unsigned long long)owner,
                    u32AsFloatM(xb), u32AsFloatM(yb), u32AsFloatM(wb), u32AsFloatM(hb));
        }
    }
}

static char  g_teSpokenId[64] = { 0 };   // event id last announced ("" = none)
static DWORD g_teSeenTick     = 0;       // tick of the most recent detour call
static bool      g_teInteractArmed = true;
static uintptr_t g_teSelf          = 0;      // live panel (for the +0x168 interaction handle)
static bool      g_teHasInteract   = false;
static char      g_teActLabel[256] = { 0 };
static const uintptr_t TE_INTERACT_HANDLE_OFF = 0x160;
static const uintptr_t TE_MGR_POOL_OFF   = 0x98;       // manager+ -> pool base ptr
static const uintptr_t TE_POOL_STRIDE    = 0x120;      // bytes per pool entry
static const uintptr_t TE_POOL_GEN_OFF   = 0x114;      // entry+ -> generation (validate == handle.gen)
static const uintptr_t TE_POOL_OBJ_OFF   = 0x08;       // entry+ -> the widget object ptr
static const uintptr_t TE_WIDGET_W_OFF   = 0x14;       // widget+ -> width  (uint, * uiScale)
static const uintptr_t TE_WIDGET_H_OFF   = 0x18;       // widget+ -> height (uint, * uiScale)

static uintptr_t teInteractWidget(uintptr_t base, uintptr_t self, uint32_t* wOut, uint32_t* hOut) {
    if (wOut) *wOut = 0; if (hOut) *hOut = 0;
    uint32_t idRaw = 0, gen = 0;
    if (!safeReadU32(self + TE_INTERACT_HANDLE_OFF, &idRaw) ||
        !safeReadU32(self + TE_INTERACT_HANDLE_OFF + 4, &gen)) return 0;
    int32_t id = (int32_t)idRaw;
    if (id < 0 || id == 0) return 0;
    uintptr_t mgr = 0, pool = 0;
    if (!safeReadPtr(base + TE_FOCUSMGR_RVA, &mgr) || mgr <= 0x10000) return 0;
    if (!safeReadPtr(mgr + TE_MGR_POOL_OFF, &pool) || pool <= 0x10000) return 0;
    uintptr_t entry = pool + (uintptr_t)(uint32_t)id * TE_POOL_STRIDE;
    uint32_t egen = 0;
    if (!safeReadU32(entry + TE_POOL_GEN_OFF, &egen) || egen != gen) return 0;  // stale handle
    uintptr_t widget = 0;
    if (!safeReadPtr(entry + TE_POOL_OBJ_OFF, &widget) || widget <= 0x10000) return 0;
    if (wOut) safeReadU32(widget + TE_WIDGET_W_OFF, wOut);
    if (hOut) safeReadU32(widget + TE_WIDGET_H_OFF, hOut);
    return widget;
}

bool townEventPopupActive() {
    return g_teSeenTick && (GetTickCount() - g_teSeenTick) < TUT_GAP_MS;
}
bool townEventEnterClaims() { return townEventPopupActive() && g_teHasInteract && !g_csOpen; }

static bool teClickAt(float fx, float fy) {
    if (clickQueued()) return false;
    moveCursorTo(fx, fy);                                        // pin cursor + queue hover (frame 0)
    enqueueSynth(SDL_EVT_MOUSEBUTTONDOWN, SDL_BUTTON_LEFT, 1, 1);
    enqueueSynth(SDL_EVT_MOUSEBUTTONUP,   SDL_BUTTON_LEFT, 0, 2);
    return true;
}

static const uint32_t TE_ELEM_ID_ET = 0x74652020u;   // '  et' — town-event element id/owner family

static uintptr_t teFindInteractElem(uintptr_t base, int64_t* idOut) {
    if (idOut) *idOut = 0;
    uintptr_t begin = 0, end = 0;
    if (!safeReadPtr(base + VEC_BEGIN_RVA, &begin) ||
        !safeReadPtr(base + VEC_END_RVA, &end) || !begin || end <= begin) return 0;
    uintptr_t n = (end - begin) / ELEM_STRIDE;
    if (n > 4096) n = 4096;
    for (uintptr_t i = 0; i < n; i++) {
        uintptr_t el = begin + i * ELEM_STRIDE;
        int64_t id = 0; uintptr_t owner = 0;
        if (!safeReadI64(el + ELEM_ID_OFF, &id) || isNoFocus(id)) continue;
        safeReadPtr(el + ELEM_OWNER_OFF, &owner);
        uint32_t cc = (uint32_t)(uint64_t)id;
        if (cc == TE_ELEM_ID_ET || (cc & 0xFFFFFF00u) == (TE_ELEM_ID_ET & 0xFFFFFF00u) ||
            (uint32_t)owner == TE_ELEM_ID_ET) {
            if (idOut) *idOut = id;
            return el;
        }
    }
    return 0;
}

// ---- Recruiting an offered hero ----
static bool teStartRecruitDrag(uintptr_t base, int index) {
    if (clickQueued()) {                              // another click is already in flight
        logLine("townevent: recruit drag REFUSED, a click is already queued");
        logInputSnapshot(base, "te-recruit-clickqueued");
        return false;
    }
    uint32_t elemId = TE_ELEM_SLOT_BASE + (uint32_t)index;
    uintptr_t src = feGetElementById((int64_t)elemId);
    float sx = 0, sy = 0;
    if (!src || !elemCenter(src, &sx, &sy)) {
        logLine("townevent: recruit drag, slot element 0x%x not on screen", elemId);
        logInputSnapshot(base, "te-recruit-noslot");
        diagDumpFocusElements(base, "te-recruit-noslot");
        return false;
    }
    float tx = 0, ty = 0;
    int64_t dropId = 0;
    uintptr_t drop = feFindElementByFamily(base, BLD_ROSTER_ELEM_FAMILY,
                                           BLD_ROSTER_OWNER_TAG, &dropId);
    bool found = drop && elemCenter(drop, &tx, &ty);
    if (!found) {
        logLine("townevent: recruit drag, no roster sidebar element on screen");
        logInputSnapshot(base, "te-recruit-noroster");
        diagDumpFocusElements(base, "te-recruit-noroster");
        return false;
    }
    if (!synthDragPoints(sx, sy, tx, ty, "townevent recruit")) return false;
    logLine("townevent: recruit drag slot %d elem 0x%x (%.0f,%.0f) -> roster 0x%llx (%.0f,%.0f)",
            index, elemId, sx, sy, (unsigned long long)dropId, tx, ty);
    return true;
}

static void teServiceRecruit(uintptr_t base) {
    if (!g_teRecWatchUntil) return;
    int now = bldRosterEntryCount(base);
    int offers = teHeroCount(base);
    bool grew = (g_teRecBefore >= 0 && now > g_teRecBefore);
    bool gone = (offers < g_teHeroCount);
    if (grew || gone) {
        g_teRecWatchUntil = 0;
        g_teRecArm = 0;
        g_teHeroCount = offers;
        if (g_teHeroRow >= offers) g_teHeroRow = offers > 0 ? offers - 1 : 0;
        char utter[MAILBOX_SZ];
        _snprintf(utter, sizeof utter, axs(AXS_TE_JOINED_FMT),
                  g_teRecName[0] ? g_teRecName : axs(AXS_THE_HERO), offers);
        utter[sizeof utter - 1] = 0;
        logLine("townevent: recruit CONFIRMED (roster %d -> %d, offers %d)",
                g_teRecBefore, now, offers);
        postSpeech(utter, true, SPK_EVENT);
        return;
    }
    if (GetTickCount() > g_teRecWatchUntil) {
        g_teRecWatchUntil = 0;
        g_teRecArm = 0;
        logLine("townevent: recruit watch expired (roster still %d, offers still %d)", now, offers);
        if (teCanAfford(base) == 0) {
            TeCost c[4];
            int nc = teReadCosts(base, c, 4);
            int k = 0;
            while (k < nc - 1 && c[k].have >= c[k].amount) k++;   // the first line that falls short
            char utter[MAILBOX_SZ];
            if (nc > 0) {
                char cur[96];
                resCurrencyTitle(base, c[k].hash, "townevent", cur, sizeof cur);
                _snprintf(utter, sizeof utter, axs(AXS_BLD_CANT_AFFORD_HAVE_FMT),
                          c[k].amount, cur, c[k].have);
                logLine("townevent: ... and the game says the price is not covered (%d x%08x, have %d)",
                        c[k].amount, c[k].hash, c[k].have);
            } else {
                _snprintf(utter, sizeof utter, "%s", axs(AXS_BLD_CANT_AFFORD_IT));
                logLine("townevent: ... and the game says the price is not covered (no cost lines read)");
            }
            utter[sizeof utter - 1] = 0;
            postSpeech(utter, true, SPK_EVENT);
            return;
        }
        int have = 0, cap = 0;
        if (bldSehRosterCounts(base, &have, &cap) && cap > 0 && have >= cap) {
            char why[192], utter[MAILBOX_SZ];
            _snprintf(why, sizeof why, axs(AXS_TE_ROSTER_FULL_FMT), have, cap);
            why[sizeof why - 1] = 0;
            _snprintf(utter, sizeof utter, "%s %s", axs(AXS_TE_NOT_RECRUITED), why);
            utter[sizeof utter - 1] = 0;
            logLine("townevent: ... and the roster reads full (%d of %d)", have, cap);
            postSpeech(utter, true, SPK_EVENT);
            return;
        }
        postSpeech(axs(AXS_TE_NOT_RECRUITED), true, SPK_EVENT);
    }
}

bool townEventHeroKeysClaim() { return townEventPopupActive() && g_teHeroCount > 0 && !g_csOpen; }

bool axIsTownEvent() { return townEventPopupActive() && !g_csOpen; }

// ---- C: the offered hero's preview sheet ----
typedef void (*TEInspectFn)(void* closure, uintptr_t* heroArg, uint32_t* slotOrdinal);
static bool teSehInspect(uintptr_t base, uintptr_t panel, uintptr_t sys, uintptr_t hero,
                         uint32_t slot) {
    struct { uintptr_t vft; uintptr_t panel; uintptr_t sys; } closure;
    closure.vft = base + TE_INSPECT_VFT;
    closure.panel = panel;
    closure.sys = sys;
    __try {
        ((TEInspectFn)(base + TE_INSPECT_RVA))(&closure, &hero, &slot);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

static int teSlotOfHero(uintptr_t panel, uintptr_t hero) {
    uintptr_t sb = 0, se = 0;
    if (!safeReadPtr(panel + TE_DISP_SLOTS_BEG, &sb) ||
        !safeReadPtr(panel + TE_DISP_SLOTS_END, &se) || !sb || se <= sb || (se - sb) % 8) return -1;
    int n = (int)((se - sb) / 8);
    if (n > 32) return -1;
    for (int i = 0; i < n; i++) {
        uintptr_t slot = 0, iface = 0, h = 0;
        if (!safeReadPtr(sb + (uintptr_t)i * 8, &slot) || slot <= 0x10000) continue;
        if (!safeReadPtr(slot + BLD_RCT_IFACE_OFF, &iface) || iface <= 0x10000) continue;
        if (safeReadPtr(iface + BLD_RCT_IFACE_HERO, &h) && h == hero) return i;
    }
    return -1;
}

static void teServiceSheet(uintptr_t base) {
    if (g_teSheetWatchUntil) {
        if (g_csOpen) {
            g_teSheetWatchUntil = 0;
            g_teSheetResume = true;
            logLine("townevent: preview sheet open observed -> resume armed");
        } else if (!townEventPopupActive() || GetTickCount() > g_teSheetWatchUntil) {
            g_teSheetWatchUntil = 0;
            g_ptySheetHero = 0;
            logLine("townevent: the preview-sheet watch timed out");
            postSpeech(axs(AXS_SHEET_DIDNT_OPEN));
        }
    }
    if (g_teSheetResume && !g_csOpen) {
        g_teSheetResume = false;
        if (!townEventPopupActive()) return;          // the popup went too: its close edge speaks
        int n = teHeroCount(base);
        g_teHeroCount = n;
        if (n <= 0) return;
        if (g_teHeroRow >= n) g_teHeroRow = n - 1;
        logLine("townevent: preview sheet closed -> back on offer %d", g_teHeroRow);
        teSpeakHeroRow(base, g_teHeroRow, n, nullptr);
    }
}

static void teReleaseReadHold(const char* key) {
    char who[96];
    _snprintf(who, sizeof who, "townevent: %s", key);
    who[sizeof who - 1] = 0;
    axReleaseReadHold(who);
}

bool routeTownEventHeroKey(uintptr_t base, uint32_t sym, uint16_t mod, uint8_t repeat) {
    int n = teHeroCount(base);                 // live, always — a claimed hero must not linger
    g_teHeroCount = n;
    if (n <= 0) { g_teRecArm = 0; return false; }
    if (g_teHeroRow >= n) g_teHeroRow = n - 1;
    if (g_teHeroRow < 0)  g_teHeroRow = 0;

    // A drag in flight owns every key until its outcome lands (the coach's rule).
    if (g_teRecArm == 2 || g_teRecWatchUntil) {
        if (sym == SDLK_RETURN || sym == SDLK_KP_ENTER || sym == SDLK_ESCAPE)
            postSpeech(axs(AXS_TE_STILL_RECRUITING));
        return true;
    }

    int dRow = 0, dCol = 0, jump = 0;
    bool isJump = axDecodeJump(sym, mod, repeat, &jump);   // Home/End: first/last offered hero
    if (isJump) {
        if (!jump) return true;                // held jump: one landing per press
        dRow = jump;
    }
    if (isJump || axDecodeArrow(sym, mod, repeat, false, &dRow, &dCol)) {
        if (dRow == 0) return true;
        teReleaseReadHold(isJump ? "a jump" : "an arrow");
        g_teRecArm = 0;                        // moving off the armed hero disarms it
        axStepCursor(&g_teHeroRow, n, dRow);   // hard stops, no wrap — the row re-reads
        teSpeakHeroRow(base, g_teHeroRow, n, nullptr);
        return true;
    }

    if (sym == SDLK_c && !(mod & (KMOD_LALT | KMOD_RALT | KMOD_LCTRL | KMOD_RCTRL))) {
        if (repeat) return true;
        teReleaseReadHold("C");
        if (csTownSheetPanel(base)) { postSpeech(axs(AXS_SHEET_ALREADY_OPEN)); return true; }
        if (g_teSheetWatchUntil)    { postSpeech(axs(AXS_STILL_WORKING)); return true; }
        uintptr_t panel = g_teSelf, vft = 0, sys = teSystem(base);
        uintptr_t hero = teHeroAt(base, g_teHeroRow);
        bool isPanel = panel && safeReadPtr(panel, &vft) && vft == base + TE_VFTABLE_RVA;
        int slot = (isPanel && hero) ? teSlotOfHero(panel, hero) : -1;
        if (!isPanel || !sys || !hero || slot < 0) {
            logLine("townevent: inspect refused (panel=%p isPanel=%d sys=%p hero=%p slot=%d)",
                    (void*)panel, isPanel, (void*)sys, (void*)hero, slot);
            postSpeech(axs(AXS_TE_CANT_DO_NOW));
            return true;
        }
        if (!teSehInspect(base, panel, sys, hero, (uint32_t)slot)) {
            logLine("townevent: inspect body faulted (panel=%p slot=%d)", (void*)panel, slot);
            postSpeech(axs(AXS_SHEET_DIDNT_OPEN));
            return true;
        }
        logLine("townevent: inspect offer %d -> slot %d hero=%p", g_teHeroRow, slot, (void*)hero);
        g_ptySheetHero = hero;                          // the sheet reader's hero source
        g_teSheetWatchUntil = GetTickCount() + 1500;    // teServiceSheet pays the observed flip
        return true;
    }

    if (sym == SDLK_ESCAPE && g_teRecArm == 1) {
        if (repeat) return true;
        teReleaseReadHold("Escape");
        g_teRecArm = 0;
        logLine("townevent: recruit cancelled (slot %d)", g_teRecIndex);
        teSpeakHeroRow(base, g_teHeroRow, n, "Cancelled. ");
        return true;
    }

    if (sym == SDLK_RETURN || sym == SDLK_KP_ENTER) {
        if (repeat) return true;
        if (mod & (KMOD_LALT | KMOD_RALT | KMOD_LCTRL | KMOD_RCTRL)) return false;
        teReleaseReadHold("Enter");
        if (g_teRecArm == 1) {                              // second Enter: perform the drag
            g_teRecBefore = bldRosterEntryCount(base);
            if (!teStartRecruitDrag(base, g_teRecIndex)) {
                g_teRecArm = 0;
                postSpeech(axs(AXS_TE_RECRUIT_CANT_NOW));
                return true;
            }
            g_teRecArm = 2;
            g_teRecWatchUntil = GetTickCount() + 2000;       // teServiceRecruit announces the outcome
            return true;
        }
        int have = 0, cap = 0;
        if (!bldSehRosterCounts(base, &have, &cap))
            logLine("townevent: roster count/max getters faulted (log only)");
        uintptr_t hero = teHeroAt(base, g_teHeroRow);
        if (!hero) { postSpeech(axs(AXS_TE_HERO_UNREADABLE)); return true; }
        g_teRecHero  = hero;
        g_teRecIndex = g_teHeroRow;
        g_teRecName[0] = 0;
        safeReadCStr(hero + HERO_NAME_OFF, g_teRecName, sizeof g_teRecName);
        g_teRecArm = 1;
        logLine("townevent: recruit pending, slot %d hero=%p \"%s\" (roster %d of %d)",
                g_teRecIndex, (void*)hero, g_teRecName, have, cap);
        char utter[224];
        _snprintf(utter, sizeof utter, axs(AXS_TE_ADD_PROMPT_FMT),
                  g_teRecName[0] ? g_teRecName : axs(AXS_TE_THIS_HERO));
        utter[sizeof utter - 1] = 0;
        postSpeech(utter);
        return true;
    }
    return false;
}

bool routeTownEventEnter(uintptr_t base) {
    if (!g_teSelf || !g_teHasInteract) return false;             // no interaction -> leave Enter alone
    teReleaseReadHold("the interaction Enter");                  // same hold, same reason as the hero keys

    int64_t id = 0;
    uintptr_t el = teFindInteractElem(base, &id);
    if (el) {
        float cx = 0, cy = 0;
        if (elemCenter(el, &cx, &cy) && teClickAt(cx, cy)) {
            g_axSpokenId = id;                                   // the hover parks here; don't re-read it
            g_axLastSpoken[0] = 0;
            logLine("townevent enter: clicking '  et' elem id=0x%llx at (%.0f,%.0f)",
                    (unsigned long long)id, cx, cy);
            char line[300];
            _snprintf(line, sizeof line, "%s.", g_teActLabel[0] ? g_teActLabel : "Interacting");
            line[sizeof line - 1] = 0;
            postSpeech(line, true, SPK_EVENT);
            return true;
        }
        logLine("townevent enter: elem id=0x%llx found but no centre/click", (unsigned long long)id);
    }

    uintptr_t begin = 0, end = 0;
    if (safeReadPtr(base + VEC_BEGIN_RVA, &begin) &&
        safeReadPtr(base + VEC_END_RVA, &end) && begin && end > begin) {
        uintptr_t n = (end - begin) / ELEM_STRIDE;
        if (n > 4096) n = 4096;
        logLine("townevent enter: '  et' not found; %llu elements at input time:", (unsigned long long)n);
        for (uintptr_t i = 0; i < n; i++) {
            uintptr_t e2 = begin + i * ELEM_STRIDE;
            int64_t eid = 0; uintptr_t owner = 0;
            if (!safeReadI64(e2 + ELEM_ID_OFF, &eid) || isNoFocus(eid)) continue;
            safeReadPtr(e2 + ELEM_OWNER_OFF, &owner);
            uint32_t xb = 0, yb = 0, wb = 0, hb = 0;
            safeReadU32(e2 + ELEM_POS_OFF, &xb);  safeReadU32(e2 + ELEM_POS_OFF + 4, &yb);
            safeReadU32(e2 + ELEM_SIZE_OFF, &wb); safeReadU32(e2 + ELEM_SIZE_OFF + 4, &hb);
            uint32_t cc = (uint32_t)(uint64_t)eid;
            char fourcc[5] = { (char)(cc & 0xff), (char)((cc >> 8) & 0xff),
                               (char)((cc >> 16) & 0xff), (char)((cc >> 24) & 0xff), 0 };
            for (int k = 0; k < 4; k++) if (fourcc[k] < 32 || fourcc[k] > 126) fourcc[k] = '.';
            logLine("townevent enter: elem id=0x%llx (%s) owner=0x%llx pos=(%.0f,%.0f) size=(%.0f,%.0f)",
                    (unsigned long long)eid, fourcc, (unsigned long long)owner,
                    u32AsFloatM(xb), u32AsFloatM(yb), u32AsFloatM(wb), u32AsFloatM(hb));
        }
    }
    postSpeech(axs(AXS_TE_NO_BUTTON), true, SPK_EVENT);
    return true;                                                 // Enter belongs to the popup while it is up
}

static bool teComposeLine(uintptr_t base, uintptr_t self, char* out, int outsz) {
    if (!out || outsz <= 0) return false;
    out[0] = 0;

    char id[64];
    uintptr_t entry = 0;
    uint32_t hash = 0;
    if (!townEventActiveId(base, id, sizeof id, &entry, &hash)) return false;

    char keyT[96], keyD[96], rawT[512], rawD[1536];
    _snprintf(keyT, sizeof keyT, "town_event_title_%s", id);
    _snprintf(keyD, sizeof keyD, "town_event_description_%s", id);
    bool haveT = resolveKey(base, keyT, rawT, sizeof rawT);
    bool haveD = resolveKey(base, keyD, rawD, sizeof rawD);

    char title[512] = { 0 }, desc[1536] = { 0 };
    if (haveT) stripMarkup(rawT, title, sizeof title);
    if (haveD) stripMarkup(rawD, desc,  sizeof desc);

    char info[512] = { 0 };
    bool haveI = townEventInfoLine(base, hash, info, sizeof info);

    char act[512] = { 0 };
    bool haveA = townEventInteractionLabel(base, entry, act, sizeof act);
    if (self) g_teSelf = self;
    g_teHasInteract = haveA;
    if (haveA) { strncpy(g_teActLabel, act, sizeof g_teActLabel - 1); g_teActLabel[sizeof g_teActLabel - 1] = 0; }
    else g_teActLabel[0] = 0;

    int heroes = teHeroCount(base);
    g_teHeroCount = heroes;
    if (heroes > 0) teLogHeroLists(base, id);
    if (heroes <= 0) { g_teHeroRow = 0; g_teRecArm = 0; }
    else if (g_teHeroRow >= heroes) g_teHeroRow = heroes - 1;

    if (!haveT && !haveD && !haveI && heroes <= 0) {
        _snprintf(out, outsz, "%s", axs(AXS_TE_TITLE));
        out[outsz - 1] = 0;
        logLine("townevent id=%s UNRESOLVED (all keys missed)", id);
        return true;
    }
    int n = _snprintf(out, outsz, "%s", axs(AXS_TE_TITLE));
    if (n < 0 || n >= outsz) n = outsz - 1;
    if (haveT && n < outsz - 1) {
        int w = _snprintf(out + n, outsz - n, " %s.", title);
        n = (w < 0) ? outsz - 1 : n + w;
    }
    if (haveD && n < outsz - 1) {
        int w = _snprintf(out + n, outsz - n, " %s", desc);
        n = (w < 0) ? outsz - 1 : n + w;
    }
    if (haveI && n < outsz - 1) {
        int w = _snprintf(out + n, outsz - n, " %s", info);
        n = (w < 0) ? outsz - 1 : n + w;
    }
    if (haveA && n < outsz - 1) {
        char armed[256];
        _snprintf(armed, sizeof armed, axs(AXS_TE_PRESS_ENTER_TO_FMT), act);
        armed[sizeof armed - 1] = 0;
        int w = g_teInteractArmed
                    ? _snprintf(out + n, outsz - n, " %s", armed)
                    : _snprintf(out + n, outsz - n, " %s.", act);
        n = (w < 0) ? outsz - 1 : n + w;
    }
    if (heroes > 0 && n < outsz - 1) {
        uintptr_t h0 = teHeroAt(base, 0);
        char card[512] = { 0 };
        if (h0) ptyHeroFrag(base, h0, card, sizeof card);
        char cost[160];
        teCostLine(base, cost, sizeof cost);
        if (cost[0]) {
            int wc = _snprintf(out + n, outsz - n, " %s", cost);
            n = (wc < 0) ? outsz - 1 : n + wc;
        }
        char offered[768];
        _snprintf(offered, sizeof offered, axs(AXS_TE_OFFERED_FMT),
                  heroes, card[0] ? card : axs(AXS_TE_A_HERO));
        offered[sizeof offered - 1] = 0;
        int w = _snprintf(out + n, outsz - n, " %s", offered);
        n = (w < 0) ? outsz - 1 : n + w;
    }
    out[outsz - 1] = 0;
    logLine("townevent id=%s haveT=%d haveD=%d haveI=%d haveA=%d heroes=%d text=\"%s\"",
            id, haveT, haveD, haveI, haveA, heroes, out);
    if (heroes > 0 && haveA)
        logLine("townevent: id=%s has BOTH an interaction and %d hero offer(s) — Enter is the "
                "recruit; the interaction is unreachable on this event", id, heroes);
    return true;
}

void announceTownEventPopup(uintptr_t base, uintptr_t self) {
    DWORD now = GetTickCount();
    DWORD gap = now - g_teSeenTick;
    g_teSeenTick = now;
    if (!g_enabled) return;

    char id[64];
    uintptr_t entry = 0;
    uint32_t hash = 0;
    if (!townEventActiveId(base, id, sizeof id, &entry, &hash)) return;  // nothing verified -> silent

    teServiceRecruit(base);

    townEventDumpOnce(base, self, id, entry, hash);

    if (strcmp(id, g_teSpokenId) == 0 && gap < TUT_GAP_MS) return;
    strncpy(g_teSpokenId, id, sizeof g_teSpokenId - 1);
    g_teSpokenId[sizeof g_teSpokenId - 1] = 0;

    char utter[MAILBOX_SZ];
    if (!teComposeLine(base, self, utter, sizeof utter)) return;
    postSpeech(utter, true, SPK_EVENT);
}

void teReannounce(uintptr_t base) {
    char utter[MAILBOX_SZ];
    if (!teComposeLine(base, 0, utter, sizeof utter)) return;
    logLine("townevent: '/' re-reading");
    postSpeech(utter);
}

// ---- The popup left the screen -> release the hold, hand the player back ----
static bool g_teOnScreen = false;
void checkTownEventDismissed(uintptr_t base) {
    teServiceSheet(base);
    bool on = townEventPopupActive();
    if (on == g_teOnScreen) return;
    g_teOnScreen = on;
    if (on) return;                      // just appeared: it speaks for itself
    g_teSelf = 0;
    axModalClosed(base, "townevent");
}
