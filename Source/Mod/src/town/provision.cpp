// town/provision.cpp -- the fifth TOWN slice

#include <cstdint>
#include <cstring>
#include <cstdio>
#include "internal.h"
// ---- TOWN: the PROVISIONING screen (AX_PROVISION) — embark rounds 2-3 ----
static const uintptr_t PROV_PANEL_OFF      = 0x3770;
static const uintptr_t PROV_STORE_HOLDER   = 0x80;     // panel+: -> holder; +0x08 -> store System
static const uintptr_t PROV_BAG_HOLDER     = 0x88;     // panel+: -> holder; +0x08 -> bag System
static const uint32_t  PROV_OWNER_STORE    = 0x73746f72u; // 'stor'
static const uint32_t  PROV_OWNER_BAG      = 0x70726f76u; // 'prov'
static const uint32_t  PROV_INV_ID_BASE    = 0x696e7620u; // 'inv ' — the family both grids use

static bool  g_provActive    = false;
static int   g_provSection   = 0;                      // 0 = store, 1 = supplies (the bag),
static int   g_provSlot[2]   = { 0, 0 };               // per-section cursor
static bool  g_provDumped    = false;
static DWORD g_provProbeAt   = 0;

static DWORD g_provTxWatchUntil = 0;                   // 0 = idle
static bool  g_provTxWasStore   = false;
static int   g_provTxGoldBefore = 0;
static int   g_provTxShardBefore = 0;                  // the second wallet (Color of Madness)
static int   g_provTxPriceGold = 0;
static int   g_provTxPriceShard = 0;
static bool  g_provTxPriceKnown = false;
static int   g_provTxBagBefore  = 0;                   // total item count across the bag
static char  g_provTxName[128];

bool axIsProvision() { return g_provActive; }

static uintptr_t provPanel(uintptr_t root) { return root + PROV_PANEL_OFF; }

// A grid's Inventory::System, or 0. Section 0 = store, 1 = bag.
static uintptr_t provSystem(uintptr_t base, int section) {
    uintptr_t root = resTownRoot(base);
    if (!root) return 0;
    uintptr_t holder = 0, sys = 0;
    uintptr_t off = (section == 0) ? PROV_STORE_HOLDER : PROV_BAG_HOLDER;
    if (!safeReadPtr(provPanel(root) + off, &holder) || holder <= 0x10000) return 0;
    if (!safeReadPtr(holder + 0x08, &sys) || sys <= 0x10000) return 0;
    return sys;
}

static int provBagTotal(uintptr_t base) {
    uintptr_t sys = provSystem(base, 1);
    uintptr_t beg = 0; int slots = 0;
    if (!sys || !invItemVectorAt(sys, &beg, &slots)) return -1;
    int total = 0;
    for (int i = 0; i < slots && i < 64; i++) {
        uint32_t amount = 0;
        if (safeReadU32(beg + (uintptr_t)i * ITEM_STRIDE + ITEM_AMOUNT_OFF, &amount) &&
            (int32_t)amount > 0) total += (int)amount;
    }
    return total;
}

int provSlotElemCount(uintptr_t base, uint32_t ownerTag, int64_t* firstId) {
    if (firstId) *firstId = 0;
    uintptr_t begin = 0, end = 0;
    if (!safeReadPtr(base + VEC_BEGIN_RVA, &begin) ||
        !safeReadPtr(base + VEC_END_RVA, &end) || !begin || end <= begin) return 0;
    uintptr_t count = (end - begin) / ELEM_STRIDE;
    if (count > 512) count = 512;
    int n = 0;
    bool any = false;
    uint64_t minId = 0;
    for (uintptr_t i = 0; i < count; i++) {
        uintptr_t elem = begin + i * ELEM_STRIDE;
        int64_t id = 0, owner = 0;
        if (!safeReadI64(elem + ELEM_ID_OFF, &id)) continue;
        if (!safeReadI64(elem + ELEM_OWNER_OFF, &owner)) continue;
        if ((uint32_t)(uint64_t)owner != ownerTag) continue;
        uint64_t uid = (uint64_t)id;
        if (!any || uid < minId) { minId = uid; any = true; }
        n++;
    }
    if (firstId) *firstId = (int64_t)minId;
    return n;
}

int64_t provSlotElem(uintptr_t base, uint32_t ownerTag, int slot) {
    uintptr_t begin = 0, end = 0;
    if (!safeReadPtr(base + VEC_BEGIN_RVA, &begin) ||
        !safeReadPtr(base + VEC_END_RVA, &end) || !begin || end <= begin) return 0;
    uintptr_t count = (end - begin) / ELEM_STRIDE;
    if (count > 512) count = 512;
    bool any = false;
    uint64_t minId = 0;
    for (uintptr_t i = 0; i < count; i++) {
        uintptr_t elem = begin + i * ELEM_STRIDE;
        int64_t id = 0; int64_t owner = 0;
        if (!safeReadI64(elem + ELEM_ID_OFF, &id)) continue;
        if (!safeReadI64(elem + ELEM_OWNER_OFF, &owner)) continue;
        if ((uint32_t)(uint64_t)owner != ownerTag) continue;
        uint64_t uid = (uint64_t)id;
        if (!any || uid < minId) { minId = uid; any = true; }
    }
    if (!any) return 0;
    uint64_t want = minId + (uint64_t)slot;
    for (uintptr_t i = 0; i < count; i++) {
        uintptr_t elem = begin + i * ELEM_STRIDE;
        int64_t id = 0; int64_t owner = 0;
        if (!safeReadI64(elem + ELEM_ID_OFF, &id)) continue;
        if (!safeReadI64(elem + ELEM_OWNER_OFF, &owner)) continue;
        if ((uint32_t)(uint64_t)owner == ownerTag && (uint64_t)id == want) return id;
    }
    return 0;
}

static const int PROV_SECTION_INFO = 2;
static const char* provSectionName(int s) {
    return s == 0 ? "Store" : s == 1 ? "Supplies" : "Quest info";
}
static const char* provSectionSpoken(int s) {
    return axs(s == 0 ? AXS_PROV_SECTION_STORE : s == 1 ? AXS_PROV_SECTION_BAG : AXS_PROV_SECTION_INFO);
}

// ---- Prices + free-grant hints ----
static const uintptr_t PROV_CLS_GOLD_OFF    = 0x9c;     // ItemClass+: purchase gold value (was 0x90)
static const uintptr_t PROV_CLS_SHARD_OFF   = 0xa0;
static const uintptr_t PROV_CLS_EVTFLAG_OFF = 0x156;    // ItemClass+: events-may-reprice flag (was 0x146)
static const uintptr_t PROV_ITEM_TAGMAP_OFF = 0x90;     // ItemStack+: map<tag hash, granted count>
static const uintptr_t PROV_ENTRY_STATE_OFF = 0x1540;   // Roster::Entry+: 1 = in party (was 0x14c0)
static const uint32_t  PROV_EVTDATA_COST_TYPE = 10;     // record+0x00: provision cost change

// ---- THE SECOND CURRENCY ----

struct ProvPrice { int gold; int shard; };

static bool provPriceOf(uintptr_t base, uintptr_t item, ProvPrice* priceOut) {
    priceOut->gold = 0;
    priceOut->shard = 0;
    uintptr_t cls = bldTrinketDef(base, item);
    if (!cls) return false;
    uint32_t v = 0, vs = 0;
    if (!safeReadU32(cls + PROV_CLS_GOLD_OFF, &v) || (int32_t)v < 0 || v > 10000000) return false;
    if (!safeReadU32(cls + PROV_CLS_SHARD_OFF, &vs) || (int32_t)vs < 0 || vs > 10000000) vs = 0;
    float mult = 1.0f;
    uint8_t evtFlag = 0;
    if (safeReadU8(cls + PROV_CLS_EVTFLAG_OFF, &evtFlag) && evtFlag) {
        uintptr_t camp = 0;
        uint32_t active = 0;
        if (safeReadPtr(base + RES_CAMPAIGN_RVA, &camp) && camp > 0x10000 &&
            safeReadU32(camp + PROV_CAMP_EVENT_OFF, &active) && active) {
            uint32_t typeHash = 0;
            safeReadU32(item + ITEM_TYPEHASH_OFF, &typeHash);
            uintptr_t reg = 0, eb = 0, ee = 0;
            if (safeReadPtr(base + PROV_EVTREG_RVA, &reg) && reg > 0x10000 &&
                safeReadPtr(reg + PROV_EVTREG_BEG_OFF, &eb) &&
                safeReadPtr(reg + PROV_EVTREG_END_OFF, &ee) && eb && ee > eb &&
                (ee - eb) % PROV_EVT_STRIDE == 0) {
                int nev = (int)((ee - eb) / PROV_EVT_STRIDE);
                if (nev > 256) nev = 256;
                for (int i = 0; i < nev; i++) {
                    uintptr_t evt = eb + (uintptr_t)i * PROV_EVT_STRIDE;
                    uint32_t idh = 0;
                    if (!safeReadU32(evt + PROV_EVT_IDHASH_OFF, &idh) || idh != active) continue;
                    uintptr_t db = 0, de = 0;
                    if (safeReadPtr(evt + PROV_EVT_DATA_BEG, &db) &&
                        safeReadPtr(evt + PROV_EVT_DATA_END, &de) && db && de > db &&
                        (de - db) % PROV_EVTDATA_STRIDE == 0) {
                        int nd = (int)((de - db) / PROV_EVTDATA_STRIDE);
                        if (nd > 64) nd = 64;
                        for (int d = 0; d < nd; d++) {
                            uintptr_t rec = db + (uintptr_t)d * PROV_EVTDATA_STRIDE;
                            uint32_t rt = 0, rh = 0;
                            float ra = 0.f;
                            if (!safeReadU32(rec, &rt) || rt != PROV_EVTDATA_COST_TYPE) continue;
                            if (!safeReadU32(rec + PROV_EVTDATA_HASH, &rh) || rh != typeHash) continue;
                            if (abReadF32(rec + PROV_EVTDATA_AMT, &ra)) mult += ra;
                        }
                    }
                    break;                           // the game stops at the matched event too
                }
            }
        }
    }
    int p = (int)((float)(int32_t)v * mult);         // the game's own truncation
    if (p < 0) p = 0;
    int ps = (int)((float)(int32_t)vs * mult);
    if (ps < 0) ps = 0;
    priceOut->gold  = p;
    priceOut->shard = ps;
    return true;
}

static void provMoneyWords(uintptr_t base, const ProvPrice& p, char* out, int outsz) {
    out[0] = 0;
    if (p.gold <= 0 && p.shard <= 0) return;
    char goldName[96] = {0}, shardName[96] = {0};
    if (p.gold  > 0) resCurrencyTitleById(base, "gold",  "provision", goldName,  sizeof goldName);
    if (p.shard > 0) resCurrencyTitleById(base, "shard", "provision", shardName, sizeof shardName);
    if (p.gold > 0 && p.shard > 0)
        _snprintf(out, outsz, axs(AXS_PROV_MONEY_BOTH_FMT), p.gold, goldName, p.shard, shardName);
    else if (p.gold > 0)
        _snprintf(out, outsz, axs(AXS_PROV_MONEY_GOLD_FMT), p.gold, goldName);
    else
        _snprintf(out, outsz, axs(AXS_PROV_MONEY_OTHER_FMT), p.shard, shardName);
    out[outsz - 1] = 0;
}

static int provFreeTagged(uintptr_t item) {
    uintptr_t head = 0;
    if (!safeReadPtr(item + PROV_ITEM_TAGMAP_OFF, &head) || head <= 0x10000) return 0;
    uintptr_t root = 0;
    if (!safeReadPtr(head + RBNODE_PARENT_OFF, &root) || root <= 0x10000) return 0;
    uintptr_t stack[8];
    uintptr_t cur = root;
    int sp = 0, total = 0, seen = 0;
    while (seen < 16) {
        uint8_t nil = 1;
        if (cur > 0x10000 && cur != head && safeReadU8(cur + RBNODE_ISNIL_OFF, &nil) && !nil) {
            if (sp >= 8) break;
            stack[sp++] = cur;
            if (!safeReadPtr(cur + RBNODE_LEFT_OFF, &cur)) break;
            continue;
        }
        if (sp == 0) break;
        cur = stack[--sp];
        int32_t val = 0;
        if (safeReadU32(cur + 0x20, (uint32_t*)&val) && val > 0 && val < 1000) total += val;
        seen++;
        if (!safeReadPtr(cur + RBNODE_RIGHT_OFF, &cur)) break;
    }
    return total;
}

struct ProvGrant { const char* clsId; const char* itemId; };
static const ProvGrant kProvGrants[] = {
    { "houndmaster",   "dog_treats"      },
    { "plague_doctor", "antivenom"       },
    { "grave_robber",  "shovel"          },
    { "arbalest",      "bandage"         },
    { "crusader",      "holy_water"      },
    { "leper",         "medicinal_herbs" },
    { "jester",        "medicinal_herbs" },
    { "antiquarian",   "skeleton_key"    },
};
static void provFreeHint(uintptr_t base, const char* rawItemId, int freeCount,
                         char* out, int outsz) {
    out[0] = 0;
    if (freeCount <= 0) return;
    // The granting classes present in the party, with localized names when latched.
    char clsNames[192] = {0};
    int written = 0;
    uintptr_t camp = 0;
    if (rawItemId[0] &&
        safeReadPtr(base + RES_CAMPAIGN_RVA, &camp) && camp > 0x10000) {
        uintptr_t beg = 0, end = 0;
        if (safeReadPtr(camp + 0x20, &beg) && safeReadPtr(camp + 0x28, &end) &&
            beg && end > beg && (end - beg) / 8 <= 256) {
            int n = (int)((end - beg) / 8);
            const char* seenCls[4] = { 0, 0, 0, 0 };
            for (int i = 0; i < n && written < 4; i++) {
                uintptr_t entry = 0;
                uint32_t state = 0;
                if (!safeReadPtr(beg + (uintptr_t)i * 8, &entry) || entry <= 0x10000) continue;
                if (!safeReadU32(entry + PROV_ENTRY_STATE_OFF, &state) || state != 1) continue;   // in party
                uintptr_t cobj = abHeroClassOf(entry + 0x08);
                if (!cobj) continue;
                char cid[64] = {0};
                if (!safeReadCStr(cobj + HEROCLASS_ID_OFF, cid, sizeof cid) || !cid[0]) continue;
                bool grants = false;
                for (int g = 0; g < (int)(sizeof kProvGrants / sizeof kProvGrants[0]); g++)
                    if (strcmp(kProvGrants[g].clsId, cid) == 0 &&
                        strcmp(kProvGrants[g].itemId, rawItemId) == 0) { grants = true; break; }
                if (!grants) continue;
                bool dup = false;                    // one mention per class, not per hero
                for (int s = 0; s < written; s++)
                    if (seenCls[s] && strcmp(seenCls[s], cid) == 0) dup = true;
                if (dup) continue;
                char disp[96] = {0};
                uint8_t latched = 0;
                if (safeReadU8(cobj + HEROCLASS_DISP_LATCH_OFF, &latched) && latched)
                    safeReadCStr(cobj + HEROCLASS_DISP_OFF, disp, sizeof disp);
                if (!disp[0]) strncpy(disp, cid, sizeof disp - 1);
                disp[sizeof disp - 1] = 0;
                size_t l = strlen(clsNames);
                if (written)
                    _snprintf(clsNames + l, sizeof clsNames - l, " %s %s",
                              axs(AXS_PROV_WORD_AND_THE), disp);
                else
                    _snprintf(clsNames + l, sizeof clsNames - l, "%s", disp);
                clsNames[sizeof clsNames - 1] = 0;
                static char cidKeep[4][64];          // seenCls needs storage past the loop turn
                strncpy(cidKeep[written], cid, 63);
                cidKeep[written][63] = 0;
                seenCls[written] = cidKeep[written];
                written++;
            }
        }
    }
    if (written)
        _snprintf(out, outsz, axs(AXS_PROV_FREE_FROM_CLASSES_FMT), freeCount, clsNames);
    else
        _snprintf(out, outsz, axs(AXS_PROV_FREE_EXPEDITION_FMT), freeCount);
    out[outsz - 1] = 0;
}

static bool provSlotText(uintptr_t base, uintptr_t sys, int section, int slot,
                         char* out, int outsz) {
    uintptr_t beg = 0; int slots = 0;
    if (!invItemVectorAt(sys, &beg, &slots)) return false;
    if (slot < 0 || slot >= slots) return false;
    uintptr_t item = beg + (uintptr_t)slot * ITEM_STRIDE;
    uint32_t amount = 0;
    if (!safeReadU32(item + ITEM_AMOUNT_OFF, &amount)) return false;
    char posS[64];
    _snprintf(posS, sizeof posS, axs(AXS_ITEM_N_OF_M), slot + 1, slots);
    posS[sizeof posS - 1] = 0;
    if ((int32_t)amount < 1) {
        _snprintf(out, outsz, "%s %s", axs(AXS_PROV_ROW_EMPTY), posS);
        out[outsz - 1] = 0;
        return true;
    }
    char type[64], itemId[64], key[192], name[256];
    invItemName(base, item, type, itemId, key, name);
    char rawId[64] = {0};
    safeReadCStr(item + ITEM_ID_OFF, rawId, sizeof rawId);

    ProvPrice price = { 0, 0 };
    bool havePrice = provPriceOf(base, item, &price);
    if (!havePrice)
        logLine("provision: no ItemClass record for \"%s %s\" -> row reads without a price",
                type, rawId);

    char money[256] = {0};
    if (section == 0) {
        if (havePrice) {
            char words[160], sent[224];
            provMoneyWords(base, price, words, sizeof words);
            if (!words[0]) _snprintf(money, sizeof money, " %s", axs(AXS_PROV_FREE));
            else {
                _snprintf(sent, sizeof sent,
                          axs(amount > 1 ? AXS_PROV_COSTS_EACH_FMT : AXS_PROV_COSTS_FMT), words);
                sent[sizeof sent - 1] = 0;
                _snprintf(money, sizeof money, " %s", sent);
            }
        }
    } else {
        int freeCnt = provFreeTagged(item);
        if (freeCnt > (int)amount) freeCnt = (int)amount;
        int sellable = (int)amount - freeCnt;
        char words[160] = {0};
        if (havePrice && sellable > 0) provMoneyWords(base, price, words, sizeof words);
        if (words[0]) {
            char sent[224];
            _snprintf(sent, sizeof sent,
                      axs(sellable > 1 ? AXS_PROV_SELLS_EACH_FMT : AXS_PROV_SELLS_FMT), words);
            sent[sizeof sent - 1] = 0;
            _snprintf(money, sizeof money, " %s", sent);
        }
        if (freeCnt > 0) {
            char hint[192];
            provFreeHint(base, rawId, freeCnt, hint, sizeof hint);
            size_t l = strlen(money);
            _snprintf(money + l, sizeof money - l, " %s", hint);
        }
        money[sizeof money - 1] = 0;
    }

    char desckey[192], desc[512];
    _snprintf(desckey, sizeof desckey, "%s%s%s", INV_DESC_PREFIX, type, itemId);
    desckey[sizeof desckey - 1] = 0;
    bool hasDesc = resolveKey(base, desckey, desc, sizeof desc) && desc[0];

    if (amount > 1) _snprintf(out, outsz, "%s, %u.%s %s%s%s",
                              name, amount, money, posS,
                              hasDesc ? " " : "", hasDesc ? desc : "");
    else            _snprintf(out, outsz, "%s.%s %s%s%s",
                              name, money, posS,
                              hasDesc ? " " : "", hasDesc ? desc : "");
    out[outsz - 1] = 0;
    return true;
}

static void provInfoSpeak(uintptr_t base, const char* prefix);

// Speak the cursor slot of the current section, prefix optional, one utterance.
static void provSpeakSlot(uintptr_t base, const char* prefix) {
    if (g_provSection == PROV_SECTION_INFO) { provInfoSpeak(base, prefix); return; }
    uintptr_t sys = provSystem(base, g_provSection);
    uintptr_t beg = 0; int slots = 0;
    if (!sys || !invItemVectorAt(sys, &beg, &slots)) {
        char utter[128];
        _snprintf(utter, sizeof utter, "%s%s. %s", prefix ? prefix : "",
                  provSectionSpoken(g_provSection), axs(AXS_PROV_NO_ITEMS));
        utter[sizeof utter - 1] = 0;
        postSpeech(utter);
        return;
    }
    int* cur = &g_provSlot[g_provSection];
    if (*cur >= slots) *cur = slots - 1;
    if (*cur < 0)      *cur = 0;
    char row[MAILBOX_SZ], utter[MAILBOX_SZ];
    if (!provSlotText(base, sys, g_provSection, *cur, row, sizeof row)) {
        _snprintf(row, sizeof row, axs(AXS_PROV_ITEM_UNREADABLE_FMT), *cur + 1, slots);
        row[sizeof row - 1] = 0;
    }
    _snprintf(utter, sizeof utter, "%s%s", prefix ? prefix : "", row);
    utter[sizeof utter - 1] = 0;
    postSpeech(utter);
}

void provReannounce(uintptr_t base) {
    if (!g_provActive) return;
    logLine("provision: re-announcing after a modal closed");
    char prefix[64];
    _snprintf(prefix, sizeof prefix, "%s %s. ", axs(AXS_PROV_TITLE),
              provSectionSpoken(g_provSection));
    prefix[sizeof prefix - 1] = 0;
    provSpeakSlot(base, prefix);
}

// ---- Selling ONE unit at a time ----
typedef void (*ProvSellOneFn)(void* env, uint32_t* visibleIndex);

static int provVisibleIndex(uintptr_t sys, int slot) {
    uintptr_t beg = 0; int slots = 0;
    if (!invItemVectorAt(sys, &beg, &slots)) return -1;
    if (slot < 0 || slot >= slots) return -1;
    int vis = 0;
    for (int i = 0; i < slots; i++) {
        uint32_t amount = 0;
        if (!safeReadU32(beg + (uintptr_t)i * ITEM_STRIDE + ITEM_AMOUNT_OFF, &amount)) return -1;
        if (i == slot) return ((int32_t)amount > 0) ? vis : -1;
        if ((int32_t)amount > 0) vis++;
    }
    return -1;
}

static bool provSellOne(uintptr_t base, uintptr_t sys, int slot) {
    int vis = provVisibleIndex(sys, slot);
    if (vis < 0) {
        logLine("provision: sell-one slot=%d refused -- not an occupied slot", slot);
        return false;
    }
    uintptr_t env[2] = { 0, sys };            // { vftable (never read), the captured System }
    uint32_t idx = (uint32_t)vis;
    bool called = false;
    __try {
        ((ProvSellOneFn)(base + PROV_SELL_ONE_RVA))(env, &idx);
        called = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { called = false; }
    logLine("provision: sell-one slot=%d visible=%d sys=%p called=%d",
            slot, vis, (void*)sys, called ? 1 : 0);
    return called;
}

// ---- THE QUEST-INFO PANEL ----
static const uintptr_t PROV_CIRCUS_ROSTER_OFF  = 0x728;  // Campaign+: the circus Roster::System
static const uintptr_t PROV_INFEST_ICON_OFF    = 0x148;  // ProvisionSelect+: the wasting icon handle
                                                         // (PreChildrenShowInternal's param_1[0x29])
static const uintptr_t QUIRK_CLASS_EVO_DEATH_OFF = 0x13c; // Quirk::Class+: char evolution_causes_death
static const int       PROV_PARTY_MAX          = 4;      // AssembleParty(system, 4)
static const int       PROV_ROSTER_MAX         = 256;    // sanity cap on the entry walk

enum ProvInfoKind { PI_DUNGEON, PI_LENGTH, PI_DIFFICULTY, PI_SCOUTING, PI_WASTING, PI_KINDS };
static const int PROV_INFO_MAX = PI_KINDS;
static int g_provInfoRow = 0;                          // the panel's row cursor
static int g_provInfoTip = 0;                          // its tooltip-buffer cursor (0 = the row)
static int g_provGridBack = 0;                         // the grid Tab left for the panel: where
static DWORD g_provLandInfoUntil = 0;

static uintptr_t provRosterSystem(uintptr_t base) {
    uintptr_t camp = 0;
    if (!safeReadPtr(base + RES_CAMPAIGN_RVA, &camp) || camp <= 0x10000) return 0;
    uint8_t circus = 0;
    safeReadU8(base + CAMP_BUFFREG_FLAG_RVA, &circus);
    return circus ? camp + PROV_CIRCUS_ROSTER_OFF : camp;
}

static int provRosterEntries(uintptr_t base, uintptr_t* begOut) {
    uintptr_t sys = provRosterSystem(base), beg = 0, end = 0;
    if (!sys || !safeReadPtr(sys + PTY_ENTRIES_BEG_OFF, &beg) ||
        !safeReadPtr(sys + PTY_ENTRIES_END_OFF, &end) || !beg || end < beg) return -1;
    int n = (int)((end - beg) / 8);
    if (n > PROV_ROSTER_MAX) { logLine("provision info: roster count %d implausible -- refused", n); return -1; }
    *begOut = beg;
    return n;
}

static int provPartyHeroes(uintptr_t base, uintptr_t heroes[PROV_PARTY_MAX]) {
    uintptr_t beg = 0;
    int n = provRosterEntries(base, &beg), k = 0;
    for (int i = 0; i < n && k < PROV_PARTY_MAX; i++) {
        uintptr_t e = 0; uint32_t st = 0;
        if (!safeReadPtr(beg + (uintptr_t)i * 8, &e) || e <= 0x10000) continue;
        if (!safeReadU32(e + PROV_ENTRY_STATE_OFF, &st) || st != 1) continue;
        heroes[k++] = e + PTY_ENTRY_HERO_OFF;
    }
    return k;
}

static int provWastingCount(uintptr_t base) {
    uintptr_t beg = 0;
    int n = provRosterEntries(base, &beg);
    if (n < 0) return -1;
    int count = 0;
    for (int i = 0; i < n; i++) {
        uintptr_t e = 0; uint32_t st = 0;
        if (!safeReadPtr(beg + (uintptr_t)i * 8, &e) || e <= 0x10000) continue;
        if (!safeReadU32(e + PROV_ENTRY_STATE_OFF, &st) || (st & ~2u) != 0) continue;
        uintptr_t hero = e + PTY_ENTRY_HERO_OFF, qb = 0, qe = 0;
        if (!safeReadPtr(hero + HERO_QUIRK_BEGIN_OFF, &qb) ||
            !safeReadPtr(hero + HERO_QUIRK_END_OFF, &qe) || qb <= 0x10000 || qe < qb) continue;
        uintptr_t span = qe - qb;
        if (span % QUIRK_ENTRY_STRIDE != 0 || span / QUIRK_ENTRY_STRIDE > 64) continue;
        for (uintptr_t q = qb; q < qe; q += QUIRK_ENTRY_STRIDE) {
            uintptr_t cls = 0; uint8_t evo = 0;
            if (!safeReadPtr(q + QUIRK_ENTRY_CLASS_OFF, &cls) || cls <= 0x10000) continue;
            if (safeReadU8(cls + QUIRK_CLASS_EVO_DEATH_OFF, &evo) && evo) { count++; break; }
        }
    }
    return count;
}

// A resolved game string, markup stripped. false (logged) on a miss.
static bool provResolve(uintptr_t base, const char* key, char* out, int outsz) {
    char raw[AB_TIP_LINE_SZ];
    if (!resolveKey(base, key, raw, sizeof raw) || !raw[0]) {
        logLine("provision info: \"%s\" did not resolve", key);
        out[0] = 0;
        return false;
    }
    stripMarkup(raw, out, outsz);
    return out[0] != 0;
}

static bool provWastingShown(uintptr_t base) {
    bool shown = false;
    infEstateLevel(base, &shown, nullptr, 0);
    if (!shown) return false;
    uintptr_t root = resTownRoot(base);
    int64_t icon = 0;
    return root && safeReadI64(provPanel(root) + PROV_INFEST_ICON_OFF, &icon) && icon != 0;
}

static char g_provInfoText[PROV_INFO_MAX][AB_TIP_LINE_SZ];
static int  g_provInfoKind[PROV_INFO_MAX];
static int provInfoRows(uintptr_t base) {
    int n = 0;
    char dungeon[EMB_NAME_MAX], lenS[160], diffS[160];
    if (embSelectedQuestFacts(base, dungeon, sizeof dungeon, lenS, sizeof lenS, diffS, sizeof diffS)) {
        const char* src[3] = { dungeon, lenS, diffS };
        const int   kind[3] = { PI_DUNGEON, PI_LENGTH, PI_DIFFICULTY };
        for (int i = 0; i < 3; i++) {
            if (!src[i][0]) continue;
            strncpy(g_provInfoText[n], src[i], AB_TIP_LINE_SZ - 1);
            g_provInfoText[n][AB_TIP_LINE_SZ - 1] = 0;
            g_provInfoKind[n++] = kind[i];
        }
    } else {
        logLine("provision info: no selected quest");
    }
    {
        uintptr_t heroes[PROV_PARTY_MAX];
        int h = provPartyHeroes(base, heroes);
        static char tip[AB_TIP_MAX_LINES][AB_TIP_LINE_SZ];
        if (csPartyScoutLines(base, heroes, h, tip) > 0) {
            strncpy(g_provInfoText[n], tip[0], AB_TIP_LINE_SZ - 1);
            g_provInfoText[n][AB_TIP_LINE_SZ - 1] = 0;
            g_provInfoKind[n++] = PI_SCOUTING;
        }
    }
    if (provWastingShown(base)) {
        int count = provWastingCount(base);
        char title[256], num[32] = {0}, fmt[64];
        if (count >= 0 && provResolve(base, "infestation_quirk_evolution_death_hero_type", title, sizeof title)) {
            const char* pc = nullptr;
            if (provResolve(base, "infestation_quirk_evolution_death_hero_count_format", fmt, sizeof fmt) &&
                (pc = strchr(fmt, '%')) != nullptr && pc[1] == 'd' && !strchr(pc + 2, '%'))
                _snprintf(num, sizeof num, fmt, count);
            else
                _snprintf(num, sizeof num, "%d", count);
            num[sizeof num - 1] = 0;
            _snprintf(g_provInfoText[n], AB_TIP_LINE_SZ, "%s %s", title, num);
            g_provInfoText[n][AB_TIP_LINE_SZ - 1] = 0;
            g_provInfoKind[n++] = PI_WASTING;
        }
    }
    return n;
}

static char g_provInfoTipLines[AB_TIP_MAX_LINES][AB_TIP_LINE_SZ];
static int provInfoTips(uintptr_t base, int row) {
    int n = 0;
    strncpy(g_provInfoTipLines[n], g_provInfoText[row], AB_TIP_LINE_SZ - 1);
    g_provInfoTipLines[n++][AB_TIP_LINE_SZ - 1] = 0;
    if (g_provInfoKind[row] == PI_SCOUTING) {
        uintptr_t heroes[PROV_PARTY_MAX];
        int h = provPartyHeroes(base, heroes);
        static char tip[AB_TIP_MAX_LINES][AB_TIP_LINE_SZ];
        int t = csPartyScoutLines(base, heroes, h, tip);
        for (int i = 1; i < t && n < AB_TIP_MAX_LINES; i++) {
            strncpy(g_provInfoTipLines[n], tip[i], AB_TIP_LINE_SZ - 1);
            g_provInfoTipLines[n++][AB_TIP_LINE_SZ - 1] = 0;
        }
    } else if (g_provInfoKind[row] == PI_WASTING) {
        if (provWastingCount(base) > 0 &&
            provResolve(base, "infestation_quirk_evolution_death_hero_description",
                        g_provInfoTipLines[n], AB_TIP_LINE_SZ))
            n++;
    }
    return n;
}

static void provInfoSpeak(uintptr_t base, const char* prefix) {
    int n = provInfoRows(base);
    char utter[MAILBOX_SZ];
    if (n <= 0) {
        _snprintf(utter, sizeof utter, "%s%s", prefix ? prefix : "", axs(AXS_NO_MORE_INFO));
    } else {
        axStepCursor(&g_provInfoRow, n, 0);            // re-clamp against the live row count
        const char* t = g_provInfoText[g_provInfoRow];
        size_t tl = strlen(t);
        bool ends = tl && strchr(".!?", t[tl - 1]);
        char pos[48];
        _snprintf(pos, sizeof pos, axs(AXS_POS_N_OF_M), g_provInfoRow + 1, n);
        pos[sizeof pos - 1] = 0;
        _snprintf(utter, sizeof utter, "%s%s%s %s", prefix ? prefix : "", t, ends ? "" : ".", pos);
    }
    utter[sizeof utter - 1] = 0;
    postSpeech(utter);
}

bool provTabToInfo(uintptr_t base) {
    if (!g_provActive || g_provSection == PROV_SECTION_INFO) return false;
    g_provGridBack = g_provSection;
    g_provSection = PROV_SECTION_INFO;
    g_provInfoRow = 0;
    g_provInfoTip = 0;
    logLine("provision: Tab -> the quest-info panel");
    char prefix[64];
    _snprintf(prefix, sizeof prefix, "%s. ", provSectionSpoken(g_provSection));
    prefix[sizeof prefix - 1] = 0;
    provInfoSpeak(base, prefix);
    return true;
}

bool provShiftTabToGrid(uintptr_t base) {
    if (!g_provActive || g_provSection != PROV_SECTION_INFO) return false;
    g_provSection = (g_provGridBack == 1) ? 1 : 0;
    logLine("provision: Shift+Tab -> back to %s", provSectionName(g_provSection));
    char prefix[64];
    _snprintf(prefix, sizeof prefix, "%s. ", provSectionSpoken(g_provSection));
    prefix[sizeof prefix - 1] = 0;
    provSpeakSlot(base, prefix);
    return true;
}

void provLandOnInfo() { g_provLandInfoUntil = GetTickCount() + 1000; }

static bool routeProvInfoKey(uintptr_t base, uint32_t sym, uint16_t mod, uint8_t repeat) {
    bool ctrl = (mod & (KMOD_LCTRL | KMOD_RCTRL)) != 0;
    if (ctrl && (sym == SDLK_UP || sym == SDLK_DOWN)) {
        if (repeat) return true;
        int n = provInfoRows(base);
        if (n <= 0) { postSpeech(axs(AXS_NO_MORE_INFO)); return true; }
        axStepCursor(&g_provInfoRow, n, 0);
        int ln = provInfoTips(base, g_provInfoRow);
        if (ln <= 1) { postSpeech(axs(AXS_NO_MORE_INFO)); return true; }
        if (g_provInfoTip < 0 || g_provInfoTip >= ln) g_provInfoTip = 0;
        g_provInfoTip = (g_provInfoTip + ((sym == SDLK_UP) ? 1 : -1) + ln) % ln;
        postSpeech(g_provInfoTipLines[g_provInfoTip]);
        return true;
    }
    if (ctrl) return false;

    int dRow = 0, dCol = 0, jump = 0;
    bool isJump = axDecodeJump(sym, mod, repeat, &jump);
    if (isJump) {
        if (!jump) return true;
        dRow = jump;
    }
    if (isJump || axDecodeArrow(sym, mod, repeat, /*wantCols=*/true, &dRow, &dCol)) {
        if (!dRow && !dCol) return true;
        int n = provInfoRows(base);
        if (n > 0 && dRow) {
            axStepCursor(&g_provInfoRow, n, dRow);      // hard stop: re-read the end row
            g_provInfoTip = 0;
        }
        provInfoSpeak(base, nullptr);                   // Left/Right: one column, re-read
        return true;
    }
    if (sym == SDLK_RETURN || sym == SDLK_KP_ENTER) {
        if (!repeat) provInfoSpeak(base, nullptr);      // nothing to act on: say where we are
        return true;
    }
    return false;
}

bool routeProvisionKey(uintptr_t base, uint32_t sym, uint16_t mod, uint8_t repeat) {
    if (mod & (KMOD_LALT | KMOD_RALT)) return false;
    if (g_provSection == PROV_SECTION_INFO) return routeProvInfoKey(base, sym, mod, repeat);
    if (mod & (KMOD_LCTRL | KMOD_RCTRL)) return false;

    int dRow = 0, dCol = 0, jump = 0;
    bool isJump = axDecodeJump(sym, mod, repeat, &jump);   // Home/End: first/last slot of the
    if (isJump) {                                          // CURRENT section (store or bag)
        if (!jump) return true;                      // held jump: one landing per press
        dRow = jump;                                 // dCol stays 0: the slot clamp lands it
    }
    if (isJump || axDecodeArrow(sym, mod, repeat, /*wantCols=*/true, &dRow, &dCol)) {
        if (!dRow && !dCol) return true;             // throttled repeat: claimed, no step this tick
        if (dCol) {
            g_provSection = (g_provSection == 0) ? 1 : 0;
            char prefix[64];
            _snprintf(prefix, sizeof prefix, "%s. ", provSectionSpoken(g_provSection));
            prefix[sizeof prefix - 1] = 0;
            provSpeakSlot(base, prefix);
            return true;
        }
        uintptr_t sys = provSystem(base, g_provSection);
        uintptr_t beg = 0; int slots = 0;
        if (!sys || !invItemVectorAt(sys, &beg, &slots)) { postSpeech(axs(AXS_PROV_NO_ITEMS)); return true; }
        axStepCursor(&g_provSlot[g_provSection], slots, dRow);   // hard stop: re-read the end slot
        provSpeakSlot(base, nullptr);
        return true;
    }

    if (sym == SDLK_RETURN || sym == SDLK_KP_ENTER) {
        if (repeat) return true;
        if (g_provTxWatchUntil) { postSpeech(axs(AXS_STILL_WORKING)); return true; }
        uintptr_t sys = provSystem(base, g_provSection);
        uintptr_t beg = 0; int slots = 0;
        if (!sys || !invItemVectorAt(sys, &beg, &slots)) { postSpeech(axs(AXS_PROV_NO_ITEMS)); return true; }
        int* cur = &g_provSlot[g_provSection];
        if (*cur >= slots) *cur = slots - 1;
        if (*cur < 0)      *cur = 0;
        uintptr_t item = beg + (uintptr_t)*cur * ITEM_STRIDE;
        uint32_t amount = 0;
        safeReadU32(item + ITEM_AMOUNT_OFF, &amount);
        if ((int32_t)amount < 1 && g_provSection == 1) {
            postSpeech(axs(AXS_PROV_SLOT_EMPTY));
            return true;
        }
        char type[64], itemId[64], key[192], name[256];
        if (!invItemName(base, item, type, itemId, key, name)) {
            strncpy(name, axs(AXS_PROV_ITEM_FALLBACK), sizeof name - 1);
            name[sizeof name - 1] = 0;
        }
        bool sellOne = (g_provSection == 1) && !(mod & (KMOD_LSHIFT | KMOD_RSHIFT));
        g_provTxWasStore = (g_provSection == 0);
        g_provTxGoldBefore = 0;
        resWalletAmount(base, resHash("gold"), &g_provTxGoldBefore);
        g_provTxShardBefore = 0;
        resWalletAmount(base, resHash("shard"), &g_provTxShardBefore);
        g_provTxBagBefore = provBagTotal(base);
        g_provTxPriceKnown = false;
        g_provTxPriceGold = 0;
        g_provTxPriceShard = 0;
        {
            ProvPrice px;
            if (provPriceOf(base, item, &px)) {
                g_provTxPriceKnown = true;
                g_provTxPriceGold = px.gold;
                g_provTxPriceShard = px.shard;
            }
        }
        if (sellOne) {
            if (!provSellOne(base, sys, *cur)) {
                char utter[MAILBOX_SZ];
                _snprintf(utter, sizeof utter, axs(AXS_PROV_CANT_SELL_FMT), name);
                utter[sizeof utter - 1] = 0;
                postSpeech(utter);
                return true;
            }
        } else {
            int64_t elem = provSlotElem(base,
                                        g_provSection == 0 ? PROV_OWNER_STORE : PROV_OWNER_BAG, *cur);
            if (!elem || !frontEndClickElementId(elem)) {
                char utter[MAILBOX_SZ];
                _snprintf(utter, sizeof utter,
                          axs(g_provSection == 0 ? AXS_PROV_CANT_BUY_FMT : AXS_PROV_CANT_SELL_FMT),
                          name);
                utter[sizeof utter - 1] = 0;
                logLine("provision: slot %d of section %d has no clickable element", *cur, g_provSection);
                postSpeech(utter);
                return true;
            }
        }
        strncpy(g_provTxName, name, sizeof g_provTxName - 1);
        g_provTxName[sizeof g_provTxName - 1] = 0;
        g_provTxWatchUntil = GetTickCount() + 1500;   // checkProvision announces the outcome
        return true;
    }

    return false;
}

static void provProbe(uintptr_t base) {
    uintptr_t camp = 0;
    uint32_t activeEvt = 0;
    if (safeReadPtr(base + RES_CAMPAIGN_RVA, &camp) && camp > 0x10000)
        safeReadU32(camp + PROV_CAMP_EVENT_OFF, &activeEvt);
    logLine("prov probe: active town event hash=0x%08x", activeEvt);
    for (int s = 0; s < 2; s++) {
        uintptr_t sys = provSystem(base, s);
        uintptr_t beg = 0; int slots = 0;
        if (!sys || !invItemVectorAt(sys, &beg, &slots)) {
            logLine("prov probe: section %d (%s): no system", s, provSectionName(s));
            continue;
        }
        logLine("prov probe: section %d (%s): sys=%p %d slots", s, provSectionName(s), (void*)sys, slots);
        for (int i = 0; i < slots && i < 32; i++) {
            uintptr_t item = beg + (uintptr_t)i * ITEM_STRIDE;
            uint32_t amount = 0;
            char type[64] = {0}, itemId[64] = {0}, key[192], name[256] = {0};
            safeReadU32(item + ITEM_AMOUNT_OFF, &amount);
            invItemName(base, item, type, itemId, key, name);
            ProvPrice price = { 0, 0 };
            bool havePrice = provPriceOf(base, item, &price);
            int freeCnt = provFreeTagged(item);
            logLine("prov probe:   slot %d amount=%d type=\"%s\" id=\"%s\" price=%sgold %d shard %d free=%d",
                    i, (int32_t)amount, type, itemId, havePrice ? "" : "MISS ",
                    havePrice ? price.gold : 0, havePrice ? price.shard : 0, freeCnt);
        }
    }
    // The element families as the live table shows them.
    uintptr_t begin = 0, end = 0;
    if (safeReadPtr(base + VEC_BEGIN_RVA, &begin) && safeReadPtr(base + VEC_END_RVA, &end) &&
        begin && end > begin) {
        uintptr_t count = (end - begin) / ELEM_STRIDE;
        if (count > 512) count = 512;
        for (uintptr_t i = 0; i < count; i++) {
            uintptr_t elem = begin + i * ELEM_STRIDE;
            int64_t id = 0, owner = 0;
            if (!safeReadI64(elem + ELEM_ID_OFF, &id)) continue;
            safeReadI64(elem + ELEM_OWNER_OFF, &owner);
            uint32_t tag = (uint32_t)(uint64_t)owner;
            if (tag == PROV_OWNER_STORE || tag == PROV_OWNER_BAG)
                logLine("prov probe: elem id=0x%llx owner='%c%c%c%c'",
                        (unsigned long long)id,
                        (char)(tag >> 24), (char)(tag >> 16), (char)(tag >> 8), (char)tag);
        }
    }
}

void checkProvision(uintptr_t base) {
    uintptr_t root = resTownRoot(base);
    if (!root) {
        if (g_provActive) { g_provActive = false; logLine("provision: stood down (left town)"); }
        g_provTxWatchUntil = 0;                      // leaving town IS the outcome (the raid starts)
        g_provSection = 0;
        g_provSlot[0] = g_provSlot[1] = 0;
        return;
    }
    bool prov = embProvIsOpen(root);

    if (g_provProbeAt && GetTickCount() >= g_provProbeAt) {
        g_provProbeAt = 0;
        // Gated on the debug log so the latch is not spent while it is off.
        if (axDebugLogEnabled() && !g_provDumped && prov) { provProbe(base); g_provDumped = true; }
    }

    if (g_provTxWatchUntil) {
        int goldNow = g_provTxGoldBefore;
        resWalletAmount(base, resHash("gold"), &goldNow);
        int shardNow = g_provTxShardBefore;
        resWalletAmount(base, resHash("shard"), &shardNow);
        int bagNow = provBagTotal(base);
        bool goldMoved  = goldNow != g_provTxGoldBefore;
        bool shardMoved = shardNow != g_provTxShardBefore;
        bool bagMoved   = bagNow >= 0 && g_provTxBagBefore >= 0 && bagNow != g_provTxBagBefore;
        if (goldMoved || shardMoved || bagMoved) {
            char utter[MAILBOX_SZ];
            int delta = goldNow - g_provTxGoldBefore;
            int sdelta = shardNow - g_provTxShardBefore;
            char cur[96];
            int paid = 0, have = 0;
            if (delta != 0) {
                resCurrencyTitleById(base, "gold", "provision", cur, sizeof cur);
                paid = delta; have = goldNow;
            } else {
                resCurrencyTitleById(base, "shard", "provision", cur, sizeof cur);
                paid = sdelta; have = shardNow;
            }
            cur[sizeof cur - 1] = 0;
            if (g_provTxWasStore) {
                if (paid < 0) _snprintf(utter, sizeof utter, axs(AXS_PROV_BOUGHT_FMT),
                                        g_provTxName, -paid, cur, have, cur);
                else          _snprintf(utter, sizeof utter, axs(AXS_PROV_ADDED_FMT), g_provTxName);
            } else {
                if (paid > 0) _snprintf(utter, sizeof utter, axs(AXS_PROV_SOLD_FMT),
                                        g_provTxName, paid, cur, have, cur);
                else          _snprintf(utter, sizeof utter, axs(AXS_PROV_REMOVED_FMT), g_provTxName);
            }
            utter[sizeof utter - 1] = 0;
            logLine("provision: tx observed (gold %d->%d shard %d->%d bag %d->%d)",
                    g_provTxGoldBefore, goldNow, g_provTxShardBefore, shardNow,
                    g_provTxBagBefore, bagNow);
            g_provTxWatchUntil = 0;
            postSpeech(utter);
        } else if (GetTickCount() > g_provTxWatchUntil) {
            g_provTxWatchUntil = 0;
            if (axIsDialog()) {
                logLine("provision: no change -- a dialog is up, it speaks for itself");
            } else {
                int shortBy = 0;          // 0 = affordable / unknown, 1 = gold, 2 = shard
                if (g_provTxWasStore && g_provTxPriceKnown) {
                    if (g_provTxPriceGold > 0 && g_provTxGoldBefore < g_provTxPriceGold) shortBy = 1;
                    else if (g_provTxPriceShard > 0 && g_provTxShardBefore < g_provTxPriceShard) shortBy = 2;
                }
                if (shortBy) {
                    char cur[96];
                    int cost = 0, have = 0;
                    if (shortBy == 1) {
                        resCurrencyTitleById(base, "gold", "provision", cur, sizeof cur);
                        cost = g_provTxPriceGold; have = g_provTxGoldBefore;
                    } else {
                        resCurrencyTitleById(base, "shard", "provision", cur, sizeof cur);
                        cost = g_provTxPriceShard; have = g_provTxShardBefore;
                    }
                    cur[sizeof cur - 1] = 0;
                    char utter[MAILBOX_SZ];
                    _snprintf(utter, sizeof utter, axs(AXS_BLD_CANT_AFFORD_HAVE_FMT), cost, cur, have);
                    utter[sizeof utter - 1] = 0;
                    logLine("provision: no change -- cannot afford \"%s\" (costs %d, wallet %d, currency %s)",
                            g_provTxName, cost, have, shortBy == 1 ? "gold" : "shard");
                    postSpeech(utter);
                } else {
                    logLine("provision: click made no observable change "
                            "(store=%d priceKnown=%d gold=%d/%d shard=%d/%d)",
                            g_provTxWasStore ? 1 : 0, g_provTxPriceKnown ? 1 : 0,
                            g_provTxGoldBefore, g_provTxPriceGold,
                            g_provTxShardBefore, g_provTxPriceShard);
                    postSpeech(axs(AXS_NOTHING_HAPPENED));
                }
            }
        }
    }

    bool on = prov && !g_resActive && !g_ptyActive && !g_rosActive && !csTownSheetPanel(base);
    if (on && !g_provActive) {
        g_provActive = true;
        bool landInfo = g_provLandInfoUntil && GetTickCount() <= g_provLandInfoUntil;
        g_provLandInfoUntil = 0;
        if (landInfo) {
            g_provSection = PROV_SECTION_INFO;
            g_provInfoRow = 0;
            g_provInfoTip = 0;
            logLine("provision: active (Shift+Tab from the lineup -> the quest-info panel)");
            char pre[64];
            _snprintf(pre, sizeof pre, "%s. ", provSectionSpoken(g_provSection));
            pre[sizeof pre - 1] = 0;
            provInfoSpeak(base, pre);
            return;
        }
        g_provSection = 0;
        g_provSlot[0] = g_provSlot[1] = 0;
        if (!g_provDumped) g_provProbeAt = GetTickCount() + 400;   // past the open tween
        logLine("provision: active");
        char pre[96];
        _snprintf(pre, sizeof pre, "%s %s %s. ", axs(AXS_PROV_TITLE), axs(AXS_PROV_E_HINT),
                  provSectionSpoken(g_provSection));
        pre[sizeof pre - 1] = 0;
        provSpeakSlot(base, pre);
    } else if (!on && g_provActive) {
        g_provActive = false;
        logLine("provision: stood down");
    }
}
