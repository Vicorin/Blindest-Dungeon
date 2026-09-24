// raid/inventory.cpp -- THE PARTY BAG

#include <cstdint>
#include <cstring>
#include <cstdio>
#include "internal.h"

// ---- In-raid INVENTORY (the party's loot/supply bag) ----
static const uintptr_t INV_SYSTEM_OFF     = 0x218;  // raid root+: embedded Inventory::System

static const int       INV_GRID_COLS      = 8;

uintptr_t        g_ruEquipHero  = 0;      // armed hero (0 = nothing armed)
static bool      g_ruLandPending = false;

bool invItemVectorAt(uintptr_t system, uintptr_t* begOut, int* slotsOut) {
    uintptr_t beg = 0, end = 0;
    if (!safeReadPtr(system + INV_ITEMS_BEG_OFF, &beg) ||
        !safeReadPtr(system + INV_ITEMS_END_OFF, &end) ||
        beg <= 0x10000 || end < beg) return false;

    uintptr_t span = end - beg;
    if (span % ITEM_STRIDE != 0) {
        static uintptr_t lastSys = 0, lastSpan = 0;
        if (system != lastSys || span != lastSpan) {
            lastSys = system; lastSpan = span;
            logLine("inventory: system %p has a RAGGED item vector -- span %llu is not a multiple "
                    "of the 0x%llx stride (begin=%p end=%p). Reading the truncated count %llu, "
                    "which is what the game's own loop walks; this is the shape the game's "
                    "GetItemFromSlotIndex assert fires on.",
                    (void*)system, (unsigned long long)span, (unsigned long long)ITEM_STRIDE,
                    (void*)beg, (void*)end, (unsigned long long)(span / ITEM_STRIDE));
        }
    }

    int slots = (int)(span / ITEM_STRIDE);
    if (slots <= 0 || slots > INV_MAX_SLOTS) return false;
    *begOut = beg; *slotsOut = slots;
    return true;
}

bool invItemVector(uintptr_t base, uintptr_t* begOut, int* slotsOut) {
    uintptr_t root = 0;
    if (!safeReadPtr(base + MAP_ROOT_RVA, &root) || root <= 0x10000) return false;
    return invItemVectorAt(root + INV_SYSTEM_OFF, begOut, slotsOut);
}

int invSlotCount(uintptr_t base) {
    uintptr_t beg = 0; int slots = 0;
    return invItemVector(base, &beg, &slots) ? slots : 0;
}

bool invItemIsTrinket(uintptr_t item);

bool invItemName(uintptr_t base, uintptr_t item,
                 char type[64], char itemId[64], char key[192], char name[256]) {
    type[0] = itemId[0] = key[0] = name[0] = 0;
    safeReadCStr(item + ITEM_TYPE_OFF, type, 64);
    safeReadCStr(item + ITEM_ID_OFF, itemId, 64);

    uint32_t typeHash = 0, noIdHash = 0;
    if (safeReadU32(item + ITEM_TYPEHASH_OFF, &typeHash) &&
        safeReadU32(base + INV_NOID_TYPEHASH_RVA, &noIdHash) &&
        typeHash == noIdHash) {
        itemId[0] = 0;
    }

    _snprintf(key, 192, "%s%s%s", INV_TITLE_PREFIX, type, itemId);
    key[191] = 0;

    if (resolveKey(base, key, name, 256)) {
        abStripMarkup(name);
        return true;
    }
    // Never go silent: speak the raw type/id rather than drop the item entirely.
    _snprintf(name, 256, "%s%s%s", type, itemId[0] ? " " : "", itemId);
    name[255] = 0;
    return false;
}

static void invAppendClause(char* out, int outsz, const char* clause) {
    if (!clause || !clause[0]) return;
    size_t n = strlen(out);
    if ((int)n + 3 >= outsz) return;
    _snprintf(out + n, outsz - (int)n, ". %s", clause);
    out[outsz - 1] = 0;
}

bool invSystemSlotText(uintptr_t base, uintptr_t system, int slot, const char* noun,
                       int posNum, int posTotal, char* out, int outsz,
                       const char* tag) {
    uintptr_t beg = 0; int slots = 0;
    if (!invItemVectorAt(system, &beg, &slots)) return false;
    if (slot < 0 || slot >= slots) return false;

    int slotNum = (posNum   > 0) ? posNum   : slot + 1;
    int slotTot = (posTotal > 0) ? posTotal : slots;
    uintptr_t item = beg + (uintptr_t)slot * ITEM_STRIDE;

    uint32_t amount = 0;
    if (!safeReadU32(item + ITEM_AMOUNT_OFF, &amount)) return false;

    if ((int32_t)amount < 1) {                        // the game's own empty test
        _snprintf(out, outsz, axs(AXS_INV_EMPTY_FMT), noun, slotNum, slotTot);
        out[outsz - 1] = 0;
        logLine("inv %s=%d/%d empty", noun, slotNum, slotTot);
        return true;
    }

    char type[64] = { 0 }, itemId[64] = { 0 }, key[192] = { 0 }, name[256] = { 0 };
    bool resolved = invItemName(base, item, type, itemId, key, name);

    char desckey[192], desc[512];
    _snprintf(desckey, sizeof desckey, "%s%s%s", INV_DESC_PREFIX, type, itemId);
    desckey[sizeof desckey - 1] = 0;
    bool hasDesc = resolveKey(base, desckey, desc, sizeof desc) && desc[0];

    char rarity[96] = { 0 }, clsreq[256] = { 0 }, fx[768] = { 0 };
    char charges[384] = { 0 }, trig[1536] = { 0 };
    uintptr_t trec = 0;
    int nfx = 0, nch = 0, ntr = 0;
    if (invItemIsTrinket(item)) {
        trec = bldTrinketRecord(base, item);
        if (trec) {
            bldTrinketRarity(base, trec, rarity, sizeof rarity, false);
            bldTrinketClassReq(base, trec, clsreq, sizeof clsreq, false);
            nch = bldTrinketCharges(base, item, trec, charges, sizeof charges, false);
            ntr = bldTrinketTriggers(base, trec, trig, sizeof trig, false);
        }
        nfx = bldTrinketEffects(base, item, fx, sizeof fx, false);
        logLine("inv trinket \"%s\" rec=%p rarity=\"%s\" class=\"%s\" fx=%d charges=%d triggers=%d",
                itemId, (void*)trec, rarity, clsreq, nfx, nch, ntr);
    }

    if (amount > 1) _snprintf(out, outsz, axs(AXS_INV_NAME_COUNT_FMT), name, (int)amount);
    else            _snprintf(out, outsz, "%s", name);
    out[outsz - 1] = 0;
    invAppendClause(out, outsz, tag);
    invAppendClause(out, outsz, rarity);
    invAppendClause(out, outsz, clsreq);
    invAppendClause(out, outsz, charges);
    invAppendClause(out, outsz, fx);
    invAppendClause(out, outsz, trig);
    char pos[256];
    _snprintf(pos, sizeof pos, axs(AXS_INV_POS_FMT), noun, slotNum, slotTot);
    pos[sizeof pos - 1] = 0;
    invAppendClause(out, outsz, pos);
    if (hasDesc) {
        size_t n = strlen(out);
        _snprintf(out + n, outsz - (int)n, " %s", desc);
        out[outsz - 1] = 0;
    }

    logLine("inv %s=%d/%d amount=%u type=\"%s\" id=\"%s\" key=\"%s\" desc=%s tag=%s -> \"%s\" (%s)",
            noun, slotNum, slotTot, amount, type, itemId, key, hasDesc ? "yes" : "MISS",
            tag && tag[0] ? tag : "-", out, resolved ? "resolved" : "fallback");
    return true;
}

bool invSlotText(uintptr_t base, int slot, char* out, int outsz, const char* tag) {
    uintptr_t root = 0;
    if (!safeReadPtr(base + MAP_ROOT_RVA, &root) || root <= 0x10000) return false;
    return invSystemSlotText(base, root + INV_SYSTEM_OFF, slot, axs(AXS_INV_NOUN_SLOT),
                             0, 0, out, outsz, tag);
}

bool resolveInventory(uintptr_t base, int64_t id, char* out, int outsz) {
    uint32_t cc  = (uint32_t)(uint64_t)id;
    uint32_t off = cc - INV_FOURCC_BASE;
    if (off >= (uint32_t)INV_MAX_SLOTS) return false;
    return invSlotText(base, (int)off, out, outsz);
}

// ---- In-raid inventory: the mod-owned grid cursor ----
volatile bool g_invActive = false;
int                  g_invSlot   = -1;      // cursor slot index; -1 = not placed yet
volatile bool g_iuActive  = false;

// ---- Inventory ITEM USE (Enter) + DISCARD (Delete) ----
static const uintptr_t RD_PANEL_INV_OFF  = 0x3150;

typedef unsigned char (*HeroUseItemFn)(void*, void*, void*, uint32_t, int32_t);
typedef uintptr_t     (*UseItemPickFn)(void);
typedef void          (*InvDiscardFn)(void*, uint32_t);

static bool sehHeroUseItem(uintptr_t base, uintptr_t hero, uintptr_t sys, uint32_t slot,
                           bool* okOut) {
    uintptr_t ctrl = 0;
    if (!safeReadPtr(base + DUNGEON_CTRL_RVA, &ctrl) || ctrl <= 0x10000) return false;
    __try {
        unsigned char r = ((HeroUseItemFn)(base + HERO_USE_ITEM_RVA))(
            reinterpret_cast<void*>(ctrl), reinterpret_cast<void*>(hero),
            reinterpret_cast<void*>(sys), slot, 1);
        if (okOut) *okOut = r != 0;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

static uintptr_t sehUseItemPick(uintptr_t base) {
    __try {
        return ((UseItemPickFn)(base + USE_ITEM_PICK_RVA))();
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

static bool sehInvDiscard(uintptr_t base, uintptr_t panel, uintptr_t sys, uint32_t slot) {
    uintptr_t cap[2] = { panel, sys };
    __try {
        ((InvDiscardFn)(base + INV_DISCARD_RVA))(cap, slot);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

static int  g_iuSlot = -1;             // the bag slot being aimed
static char g_iuName[160] = {0};       // the item's localized name, captured at entry
static int  g_iuCursor = -1;           // hero cursor; -1 = unplaced (the ts model)

// ---- TRINKETS IN A RAID ----
static bool      g_iuIsTrinket = false;
static bool      g_iuIsCamp    = false;

void ruDisarmEquip(const char* why) {
    if (!g_ruEquipHero && !g_ruLandPending) return;
    logLine("trinket: raid equip disarmed (%s)", why ? why : "unspecified");
    g_ruEquipHero   = 0;
    g_ruLandPending = false;
}

bool invItemIsTrinket(uintptr_t item) {
    char type[64] = { 0 };
    if (!safeReadCStr(item + ITEM_TYPE_OFF, type, sizeof type)) return false;
    return strcmp(type, "trinket") == 0;
}

static bool invItemIsCampSupply(uintptr_t item) {
    char type[64] = { 0 }, id[64] = { 0 };
    if (!safeReadCStr(item + ITEM_TYPE_OFF, type, sizeof type)) return false;
    if (!safeReadCStr(item + ITEM_ID_OFF, id, sizeof id)) return false;
    return strcmp(type, "supply") == 0 && strcmp(id, "firewood") == 0;
}

static bool ruFightingNow(uintptr_t base) {
    uintptr_t root = mapRoot(base);
    uint32_t combat = 0;
    if (!root) return false;
    safeReadU32(root + RAID_IN_COMBAT_OFF, &combat);
    return combat != 0;
}

static void ruSnapTrinkets(uintptr_t base, uintptr_t hero, char names[TRINKET_SLOT_COUNT][160]) {
    for (int i = 0; i < TRINKET_SLOT_COUNT; i++) names[i][0] = 0;
    uintptr_t beg = 0;
    int slots = 0;
    if (!hero || !invItemVectorAt(hero + HERO_TRINKET_SYSTEM_OFF, &beg, &slots)) return;
    for (int i = 0; i < TRINKET_SLOT_COUNT && i < slots; i++) {
        uintptr_t item = beg + (uintptr_t)i * ITEM_STRIDE;
        int32_t amount = 0;
        safeReadU32(item + ITEM_AMOUNT_OFF, (uint32_t*)&amount);
        if (amount < 1) continue;
        char type[64], id[64], key[192], full[256];
        invItemName(base, item, type, id, key, full);
        _snprintf(names[i], 160, "%s", full);
        names[i][159] = 0;
    }
}

// ---- THE EQUIP THE GAME STOPS TO ASK ABOUT ----
static const DWORD RU_ASK_GRACE_MS  = 400;
static const DWORD RU_ASK_SETTLE_MS = 500;
static const DWORD RU_ASK_MAX_MS    = 180000;   // a question left standing this long is let go of

static uintptr_t g_ruAskHero = 0;               // 0 = no deferred verdict pending
static char      g_ruAskName[160];              // the trinket, captured before the aim was dropped
static char      g_ruAskWho[160];               // ... and the hero's spoken label
static char      g_ruAskBefore[TRINKET_SLOT_COUNT][160];
static DWORD     g_ruAskStart     = 0;
static DWORD     g_ruAskGoneAt    = 0;          // when the question left the screen (0 = still up)
static bool      g_ruAskSawDialog = false;

static int ruEquipLanded(uintptr_t base, uintptr_t hero, const char* name,
                         char before[TRINKET_SLOT_COUNT][160],
                         char displacedName[160], const char* logWhy) {
    char after[TRINKET_SLOT_COUNT][160];
    ruSnapTrinkets(base, hero, after);
    int landed = -1, displaced = -1;
    bool afterUsed[TRINKET_SLOT_COUNT] = { false };
    for (int i = 0; i < TRINKET_SLOT_COUNT; i++)
        if (strcmp(after[i], name) == 0 && strcmp(before[i], name) != 0) landed = i;
    for (int i = 0; i < TRINKET_SLOT_COUNT; i++) {
        if (!before[i][0]) continue;
        bool still = false;
        for (int j = 0; j < TRINKET_SLOT_COUNT; j++) {
            if (afterUsed[j] || strcmp(before[i], after[j]) != 0) continue;
            afterUsed[j] = true;
            still = true;
            break;
        }
        if (!still) displaced = i;
    }
    displacedName[0] = 0;
    if (displaced >= 0) {
        _snprintf(displacedName, 160, "%s", before[displaced]);
        displacedName[159] = 0;
    }
    if (logWhy)
        logLine("trinket: slots before [\"%s\",\"%s\"] after [\"%s\",\"%s\"] landed=%d displaced=%d (%s)",
                before[0], before[1], after[0], after[1], landed, displaced, logWhy);
    return landed;
}

static void ruSpeakEquipLanded(const char* who, const char* name, int landed,
                               const char* displacedName) {
    char utter[800];
    if (displacedName && displacedName[0])
        _snprintf(utter, sizeof utter, axs(AXS_INV_EQUIPPED_DISPLACED_FMT),
                  name, who, landed + 1, displacedName);
    else
        _snprintf(utter, sizeof utter, axs(AXS_INV_EQUIPPED_FMT), name, who, landed + 1);
    utter[sizeof utter - 1] = 0;
    postSpeech(utter);
}

static void ruSpeakEquipRefused(const char* who, const char* name) {
    char utter[800];
    _snprintf(utter, sizeof utter, axs(AXS_INV_COULDNT_WEAR_FMT), who, name);
    utter[sizeof utter - 1] = 0;
    postSpeech(utter);
}

static void ruAskClear(const char* why) {
    if (!g_ruAskHero) return;
    if (why) logLine("trinket: deferred equip verdict dropped (%s)", why);
    g_ruAskHero      = 0;
    g_ruAskGoneAt    = 0;
    g_ruAskSawDialog = false;
}

static void ruAskArm(uintptr_t hero, const char* who, const char* name,
                     char before[TRINKET_SLOT_COUNT][160]) {
    g_ruAskHero = hero;
    _snprintf(g_ruAskName, sizeof g_ruAskName, "%s", name ? name : "");
    g_ruAskName[sizeof g_ruAskName - 1] = 0;
    _snprintf(g_ruAskWho, sizeof g_ruAskWho, "%s", who ? who : "");
    g_ruAskWho[sizeof g_ruAskWho - 1] = 0;
    for (int i = 0; i < TRINKET_SLOT_COUNT; i++) {
        _snprintf(g_ruAskBefore[i], sizeof g_ruAskBefore[i], "%s", before[i]);
        g_ruAskBefore[i][sizeof g_ruAskBefore[i] - 1] = 0;
    }
    g_ruAskStart     = GetTickCount();
    g_ruAskGoneAt    = 0;
    g_ruAskSawDialog = false;
    logLine("trinket: \"%s\" did not land on the call -- deferring the verdict", g_ruAskName);
}

// Per frame: pay the deferred verdict, once there is something to pay it from.
void serviceRaidTrinketConfirm(uintptr_t base) {   // OurPoll, beside serviceRaidTrinket
    if (!g_ruAskHero) return;
    if (!mapRoot(base)) { ruAskClear("the raid ended"); return; }   // the slots went with it

    DWORD now = GetTickCount();
    if ((DWORD)(now - g_ruAskStart) > RU_ASK_MAX_MS) {
        ruAskClear("the question outlived the watch");
        return;
    }
    if (confirmDialogOpen(base)) {
        g_ruAskSawDialog = true;
        g_ruAskGoneAt    = 0;
        return;
    }

    char displacedName[160];
    if (!g_ruAskSawDialog) {
        if ((DWORD)(now - g_ruAskStart) < RU_ASK_GRACE_MS) return;   // a question may still be coming
        int landed = ruEquipLanded(base, g_ruAskHero, g_ruAskName, g_ruAskBefore, displacedName,
                                   "no question came");
        if (landed >= 0) ruSpeakEquipLanded(g_ruAskWho, g_ruAskName, landed, displacedName);
        else             ruSpeakEquipRefused(g_ruAskWho, g_ruAskName);
        ruAskClear(nullptr);
        return;
    }

    if (!g_ruAskGoneAt) g_ruAskGoneAt = now ? now : 1;
    int landed = ruEquipLanded(base, g_ruAskHero, g_ruAskName, g_ruAskBefore, displacedName, nullptr);
    if (landed >= 0) {
        logLine("trinket: \"%s\" landed in slot %d after the question was answered",
                g_ruAskName, landed + 1);
        ruSpeakEquipLanded(g_ruAskWho, g_ruAskName, landed, displacedName);
        ruAskClear(nullptr);
        return;
    }
    if ((DWORD)(now - g_ruAskGoneAt) < RU_ASK_SETTLE_MS) return;
    logLine("trinket: \"%s\" never reached the hero after the question closed -- the refusal stands",
            g_ruAskName);
    ruSpeakEquipRefused(g_ruAskWho, g_ruAskName);
    ruAskClear(nullptr);
}

static int ruFirstTrinketSlot(uintptr_t base) {
    uintptr_t beg = 0;
    int slots = 0;
    if (!invItemVector(base, &beg, &slots)) return -1;
    for (int i = 0; i < slots; i++) {
        uintptr_t item = beg + (uintptr_t)i * ITEM_STRIDE;
        int32_t amount = 0;
        safeReadU32(item + ITEM_AMOUNT_OFF, (uint32_t*)&amount);
        if (amount > 0 && invItemIsTrinket(item)) return i;
    }
    return -1;
}

bool ruBeginFromSheet(uintptr_t base, uintptr_t hero) {
    if (ruFightingNow(base)) {
        logLine("trinket: sheet slot refused — in combat");
        postSpeech(axs(AXS_INV_NO_TRINKET_IN_FIGHT));
        return true;
    }
    if (!hero) return true;
    int first = ruFirstTrinketSlot(base);
    if (first < 0) {
        logLine("trinket: sheet slot -> no trinkets in the bag");
        postSpeech(axs(AXS_INV_NO_TRINKETS_IN_BAG));
        return true;
    }
    char who[80] = { 0 };
    safeReadCStr(hero + HERO_NAME_OFF, who, sizeof who);
    g_ruEquipHero   = hero;
    g_ruLandPending = true;
    logLine("trinket: armed %p \"%s\" from the sheet, first bag trinket = slot %d",
            (void*)hero, who, first);
    if (!routePanelJump(base, false, 0)) {
        ruDisarmEquip("the panel jump was refused");
        postSpeech(axs(AXS_INV_DIDNT_OPEN));
        return true;
    }
    return true;
}

void serviceRaidTrinket(uintptr_t base) {
    if (!g_ruLandPending) return;
    if (!g_invActive) return;                       // the corner has not switched yet
    int first = ruFirstTrinketSlot(base);
    if (first < 0) {
        ruDisarmEquip("the bag holds no trinket");
        postSpeech(axs(AXS_INV_NO_TRINKETS_IN_BAG));
        return;
    }
    g_ruLandPending = false;
    g_invSlot = first;
    char who[80] = { 0 }, row[MAILBOX_SZ];
    safeReadCStr(g_ruEquipHero + HERO_NAME_OFF, who, sizeof who);
    char prefix[512];
    _snprintf(prefix, sizeof prefix, axs(AXS_INV_CHOOSE_TRINKET_FMT),
              who[0] ? who : axs(AXS_THE_HERO_LC));
    prefix[sizeof prefix - 1] = 0;
    logLine("trinket: landed on bag slot %d for \"%s\"", first, who);
    if (invSlotText(base, first, row, sizeof row)) {
        char utter[MAILBOX_SZ];
        _snprintf(utter, sizeof utter, "%s %s", prefix, row);
        utter[sizeof utter - 1] = 0;
        postSpeech(utter);                          // land ON something, in one utterance
    } else {
        postSpeech(prefix);
    }
}

static uintptr_t g_iuTurnActor    = 0;
static bool      g_iuUsedThisTurn = false;

// The focused, occupied slot — the one thing both Enter and Delete start from.
static bool iuFocusedItem(uintptr_t base, uintptr_t* itemOut, char* name, int namesz) {
    uintptr_t beg = 0; int slots = 0;
    if (!invItemVector(base, &beg, &slots) || slots <= 0) { postSpeech(axs(AXS_INV_BAG_EMPTY)); return false; }
    if (g_invSlot < 0 || g_invSlot >= slots) { postSpeech(axs(AXS_NO_ITEM_SELECTED)); return false; }
    uintptr_t item = beg + (uintptr_t)g_invSlot * ITEM_STRIDE;
    int32_t amount = 0;
    safeReadU32(item + ITEM_AMOUNT_OFF, (uint32_t*)&amount);
    if (amount < 1) { postSpeech(axs(AXS_INV_SLOT_EMPTY)); return false; }
    char type[64], id[64], key[192], full[256];
    invItemName(base, item, type, id, key, full);
    _snprintf(name, namesz, "%s", full[0] ? full : axs(AXS_INV_ITEM_FALLBACK));
    name[namesz - 1] = 0;
    if (itemOut) *itemOut = item;
    return true;
}

static int iuTargetList(uintptr_t base, uintptr_t* out, int maxOut) {
    uintptr_t root = mapRoot(base);
    if (!root) return 0;
    uint32_t combat = 0;
    safeReadU32(root + RAID_IN_COMBAT_OFF, &combat);
    if (combat != 0) {
        uintptr_t hero = sehUseItemPick(base);
        if (hero <= 0x10000) return 0;
        out[0] = hero;
        return 1;
    }
    int n = rvPartyList(base, out, maxOut);
    for (int i = 0, j = n - 1; i < j; i++, j--) {
        uintptr_t t = out[i]; out[i] = out[j]; out[j] = t;
    }
    return n;
}

static void iuCommit(uintptr_t base, uintptr_t hero);   // an armed trinket commits without a picker

static bool iuBegin(uintptr_t base) {
    char name[160];
    uintptr_t item = 0;
    if (!iuFocusedItem(base, &item, name, sizeof name)) return true;

    g_iuIsTrinket = item && invItemIsTrinket(item);
    g_iuIsCamp    = item && invItemIsCampSupply(item);
    if (g_iuIsTrinket) {
        if (ruFightingNow(base)) {
            logLine("trinket: \"%s\" refused — in combat", name);
            postSpeech(axs(AXS_INV_NO_TRINKET_IN_FIGHT));
            return true;
        }
        if (g_ruEquipHero) {
            uintptr_t hero = g_ruEquipHero;
            g_ruEquipHero = 0;
            _snprintf(g_iuName, sizeof g_iuName, "%s", name);
            g_iuName[sizeof g_iuName - 1] = 0;
            g_iuSlot = g_invSlot;
            logLine("trinket: \"%s\" -> armed hero %p (no picker)", name, (void*)hero);
            iuCommit(base, hero);
            return true;
        }
    }

    if (g_iuIsCamp && !campCanStartHere(base)) {
        logLine("itemuse: \"%s\" refused before the prompt -- the game's can-camp gate says no", name);
        postSpeech(axs(AXS_INV_NO_CAMP_HERE));
        return true;
    }

    uintptr_t root   = mapRoot(base);
    uint32_t  combat = 0;
    if (root) safeReadU32(root + RAID_IN_COMBAT_OFF, &combat);
    if (combat != 0 && g_iuUsedThisTurn) {
        logLine("itemuse: blocked \"%s\" — item already used this turn (actor %p)",
                name, (void*)g_iuTurnActor);
        postSpeech(axs(AXS_INV_ALREADY_USED_ITEM));
        return true;
    }

    uintptr_t heroes[RV_MAX_MEMBERS];
    int n = iuTargetList(base, heroes, RV_MAX_MEMBERS);
    if (n <= 0) {
        logLine("itemuse: no legal target for \"%s\"", name);
        postSpeech(axs(AXS_INV_NO_ONE_CAN_USE));
        return true;
    }

    _snprintf(g_iuName, sizeof g_iuName, "%s", name);
    g_iuName[sizeof g_iuName - 1] = 0;
    g_iuSlot   = g_invSlot;
    g_iuCursor = -1;
    if (g_tsActive) { tsSetActive(false); logLine("targeting: abandoned (item use opened)"); }
    g_iuActive = true;

    char msg[640];
    if (g_iuIsCamp) {
        g_iuCursor = 0;
        _snprintf(msg, sizeof msg, "%s", axs(AXS_INV_MAKE_CAMP_PROMPT));
    } else {
        _snprintf(msg, sizeof msg, axs(g_iuIsTrinket ? AXS_INV_EQUIP_CHOOSE_FMT
                                                     : AXS_INV_USE_CHOOSE_FMT), name, n);
    }
    msg[sizeof msg - 1] = 0;
    logLine("itemuse: begin \"%s\" slot %d targets=%d trinket=%d camp=%d",
            name, g_iuSlot, n, g_iuIsTrinket ? 1 : 0, g_iuIsCamp ? 1 : 0);
    postSpeech(msg);
    return true;
}

void iuAbandon(const char* why) {
    if (!g_iuActive) return;
    g_iuActive = false;
    logLine("itemuse: abandoned (%s)", why);
}

static void iuSpeakRow(uintptr_t base, uintptr_t* heroes, int n, int idx) {
    char out[MAILBOX_SZ]; out[0] = 0;
    char frag[160];
    if (abHeroLabelOf(base, heroes[idx], frag, sizeof frag)) _snprintf(out, sizeof out, "%s", frag);
    else _snprintf(out, sizeof out, axs(AXS_INV_HERO_N_FMT), idx + 1);
    out[sizeof out - 1] = 0;
    size_t len;
    if (abHeroHealth(base, heroes[idx], frag, sizeof frag)) {
        len = strlen(out);
        _snprintf(out + len, sizeof out - len, ". %s", frag);
    }
    if (abHeroStress(base, heroes[idx], frag, sizeof frag)) {
        len = strlen(out);
        _snprintf(out + len, sizeof out - len, ". %s", frag);
    }
    if (g_iuIsTrinket) {
        uintptr_t beg = 0;
        int slots = 0;
        if (invItemVector(base, &beg, &slots) && g_iuSlot >= 0 && g_iuSlot < slots) {
            uintptr_t item = beg + (uintptr_t)g_iuSlot * ITEM_STRIDE;
            bool sure = false;
            bool fits = bldTrinketFitsHero(base, item, heroes[idx], &sure);
            if (sure && !fits) {
                char req[160];
                req[0] = 0;
                uintptr_t rec = bldTrinketRecord(base, item);
                if (rec) bldTrinketClassReq(base, rec, req, sizeof req, false);
                // Whole clauses with the ". " join in code, not entries that carry punctuation.
                char why[512];
                if (req[0]) _snprintf(why, sizeof why, axs(AXS_INV_CANT_WEAR_WHY_FMT), req);
                else        _snprintf(why, sizeof why, "%s", axs(AXS_INV_CANT_WEAR));
                why[sizeof why - 1] = 0;
                len = strlen(out);
                _snprintf(out + len, sizeof out - len, ". %s", why);
            }
        }
    }
    char tpos[256];
    _snprintf(tpos, sizeof tpos, axs(AXS_INV_TARGET_N_OF_M), idx + 1, n);
    tpos[sizeof tpos - 1] = 0;
    len = strlen(out);
    _snprintf(out + len, sizeof out - len, ". %s", tpos);
    out[sizeof out - 1] = 0;
    postSpeech(out);
}

static void iuCommit(uintptr_t base, uintptr_t hero) {
    uintptr_t root = mapRoot(base);
    if (!root) { iuAbandon("raid gone"); return; }

    char who[160];
    if (!abHeroLabelOf(base, hero, who, sizeof who)) _snprintf(who, sizeof who, "%s", axs(AXS_THE_HERO_LC));
    who[sizeof who - 1] = 0;

    uintptr_t beg = 0; int slots = 0;
    if (!invItemVector(base, &beg, &slots) || g_iuSlot < 0 || g_iuSlot >= slots) {
        iuAbandon("bag unreadable at commit");
        postSpeech(axs(AXS_INV_ITEM_GONE));
        return;
    }
    uintptr_t item = beg + (uintptr_t)g_iuSlot * ITEM_STRIDE;
    uint32_t before = 0;
    safeReadU32(item + ITEM_AMOUNT_OFF, &before);
    if ((int32_t)before < 1) {
        iuAbandon("slot emptied under the aim");
        postSpeech(axs(AXS_INV_ITEM_GONE));
        return;
    }

    if (g_iuIsTrinket && ruFightingNow(base)) {
        iuAbandon("combat started");
        logLine("trinket: commit \"%s\" refused — combat started under the aim", g_iuName);
        postSpeech(axs(AXS_INV_NO_TRINKET_IN_FIGHT));
        return;
    }
    if (g_iuIsCamp && !campCanStartHere(base)) {
        iuAbandon("can-camp gate closed under the aim");
        logLine("itemuse: firewood commit refused -- the game's can-camp gate says no now");
        postSpeech(axs(AXS_INV_NO_CAMP_HERE));
        return;
    }
    char trkBefore[TRINKET_SLOT_COUNT][160];
    if (g_iuIsTrinket) ruSnapTrinkets(base, hero, trkBefore);

    bool gameOk = false;
    bool called = sehHeroUseItem(base, hero, root + INV_SYSTEM_OFF, (uint32_t)g_iuSlot, &gameOk);
    uint32_t after = before;
    safeReadU32(item + ITEM_AMOUNT_OFF, &after);
    logLine("itemuse: commit \"%s\" -> %s (called=%d ok=%d amount %u -> %u trinket=%d)",
            g_iuName, who, called ? 1 : 0, gameOk ? 1 : 0, before, after, g_iuIsTrinket ? 1 : 0);

    if (g_iuIsTrinket) {
        char nm[160];
        _snprintf(nm, sizeof nm, "%s", g_iuName);
        nm[sizeof nm - 1] = 0;
        iuAbandon("committed");                    // the aim is spent whichever way this goes
        char displacedName[160];
        int landed = ruEquipLanded(base, hero, nm, trkBefore, displacedName, "commit");
        if (landed >= 0) {
            ruSpeakEquipLanded(who, nm, landed, displacedName);
            return;
        }
        ruAskArm(hero, who, nm, trkBefore);
        return;
    }

    char msg[800];
    if (after < before) {
        uint32_t combat = 0;
        safeReadU32(root + RAID_IN_COMBAT_OFF, &combat);
        if (combat != 0) {
            g_iuUsedThisTurn = true;
            logLine("itemuse: turn's free item spent (actor %p)", (void*)g_iuTurnActor);
        }
        if (g_iuIsCamp && campPhase(base) == 0) {
            logLine("itemuse: firewood consumed but the camp phase is still 0 -- no camp started");
            _snprintf(msg, sizeof msg, "%s", axs(AXS_INV_FIREWOOD_LOST));
        } else if (g_iuIsCamp)
            _snprintf(msg, sizeof msg, "%s", axs(AXS_INV_MAKING_CAMP));
        else if ((int32_t)after > 0)
            _snprintf(msg, sizeof msg, axs(AXS_INV_USED_LEFT_FMT), g_iuName, who, after);
        else
            _snprintf(msg, sizeof msg, axs(AXS_INV_USED_NONE_LEFT_FMT), g_iuName, who);
    } else {
        if (g_iuIsCamp) _snprintf(msg, sizeof msg, "%s", axs(AXS_INV_CANT_CAMP_HERE));
        else            _snprintf(msg, sizeof msg, axs(AXS_INV_COULDNT_USE_FMT), g_iuName, who);
    }
    msg[sizeof msg - 1] = 0;
    iuAbandon("committed");
    postSpeech(msg);
}

void serviceItemTurn(uintptr_t base) {
    uintptr_t root   = mapRoot(base);
    uint32_t  combat = 0;
    if (root) safeReadU32(root + RAID_IN_COMBAT_OFF, &combat);
    if (!root || combat == 0) {                     // not fighting -> no per-turn limit at all
        if (g_iuTurnActor || g_iuUsedThisTurn) { g_iuTurnActor = 0; g_iuUsedThisTurn = false; }
        return;
    }
    uintptr_t now = abCurrentTurnActor(root);
    if (now != 0 && now != g_iuTurnActor) {         // a real new combatant has the turn
        if (g_iuUsedThisTurn)
            logLine("itemturn: turn %p -> %p, per-turn item refreshed",
                    (void*)g_iuTurnActor, (void*)now);
        g_iuTurnActor    = now;
        g_iuUsedThisTurn = false;
    }
}

bool routeItemUseKey(uintptr_t base, uint32_t sym, uint16_t mod, uint8_t repeat) {
    if (mod & (KMOD_LALT | KMOD_RALT | KMOD_LCTRL | KMOD_RCTRL)) return false;

    if (sym == SDLK_ESCAPE) {
        if (repeat) return true;
        iuAbandon("cancelled");
        postSpeech(axs(AXS_CANCELLED));
        return true;
    }

    int dir = 0;
    bool confirm = false;
    switch (sym) {
        case SDLK_LEFT:   dir = -1; break;
        case SDLK_RIGHT:  dir =  1; break;
        case SDLK_RETURN:
        case SDLK_KP_ENTER: confirm = true; break;
        default: return false;
    }
    if (repeat) return true;

    uintptr_t heroes[RV_MAX_MEMBERS];
    int n = iuTargetList(base, heroes, RV_MAX_MEMBERS);
    if (n <= 0) {
        iuAbandon("targets vanished");
        postSpeech(axs(AXS_INV_NO_ONE_CAN_USE));
        return true;
    }
    if (g_iuCursor >= n) g_iuCursor = n - 1;

    if (g_iuIsCamp) {
        if (confirm) { iuCommit(base, heroes[0]); return true; }
        postSpeech(axs(AXS_INV_MAKE_CAMP_PROMPT));
        return true;
    }

    if (confirm) {
        if (g_iuCursor < 0) { postSpeech(axs(AXS_INV_NO_TARGET_SELECTED)); return true; }
        iuCommit(base, heroes[g_iuCursor]);
        return true;
    }

    if (g_iuCursor < 0) g_iuCursor = 0;       // first press lands
    else axStepCursor(&g_iuCursor, n, dir);   // hard stops re-read — the target list's rule
    iuSpeakRow(base, heroes, n, g_iuCursor);
    return true;
}

// ---- Delete: discard the focused stack, through the game's own confirm ----
static bool      g_idPending  = false;    // a discard's outcome is being watched
static bool      g_idSawDialog = false;   // the game's confirm (or refusal) dialog appeared
static int       g_idSlot     = -1;
static uint32_t  g_idBefore   = 0;
static char      g_idName[160] = {0};
static DWORD     g_idDeadline = 0;

static bool invDiscardBegin(uintptr_t base) {
    char name[160];
    uintptr_t item = 0;
    if (!iuFocusedItem(base, &item, name, sizeof name)) return true;

    uintptr_t root = mapRoot(base);
    uintptr_t rd = (uintptr_t)g_raidDisplay;
    uintptr_t panel = 0;
    if (!root || rd <= 0x10000 || !safeReadPtr(rd + RD_PANEL_INV_OFF, &panel) ||
        panel <= 0x10000) {
        logLine("discard: no panel to go through (rd=%p)", (void*)rd);
        postSpeech(axs(AXS_INV_COULDNT_DISCARD));
        return true;
    }

    uint32_t before = 0;
    safeReadU32(item + ITEM_AMOUNT_OFF, &before);
    _snprintf(g_idName, sizeof g_idName, "%s", name);
    g_idName[sizeof g_idName - 1] = 0;
    g_idSlot      = g_invSlot;
    g_idBefore    = before;
    g_idSawDialog = false;
    g_idDeadline  = GetTickCount() + 5000;

    uint32_t prevMode = 0;
    safeReadU32(base + INPUT_MODE_RVA, &prevMode);
    safeWriteU32(base + INPUT_MODE_RVA, 1);
    bool called = sehInvDiscard(base, panel, root + INV_SYSTEM_OFF, (uint32_t)g_invSlot);
    safeWriteU32(base + INPUT_MODE_RVA, prevMode);

    logLine("discard: \"%s\" slot %d amount %u -> lambda_3 %s (mode %u restored)",
            g_idName, g_idSlot, before, called ? "called" : "FAULTED", prevMode);
    if (!called) { postSpeech(axs(AXS_INV_COULDNT_DISCARD)); return true; }
    g_idPending = true;
    return true;
}

void serviceDiscardWatch(uintptr_t base) {
    if (!g_idPending) return;
    uintptr_t root = mapRoot(base);
    if (!root) { g_idPending = false; return; }

    bool dlg = confirmDialogOpen(base);
    if (dlg) { g_idSawDialog = true; return; }

    uintptr_t beg = 0; int slots = 0;
    uint32_t now = g_idBefore;
    if (invItemVector(base, &beg, &slots) && g_idSlot >= 0 && g_idSlot < slots)
        safeReadU32(beg + (uintptr_t)g_idSlot * ITEM_STRIDE + ITEM_AMOUNT_OFF, &now);

    if (now < g_idBefore) {
        g_idPending = false;
        char msg[700];
        _snprintf(msg, sizeof msg, axs(AXS_INV_DISCARDED_FMT), g_idName);
        msg[sizeof msg - 1] = 0;
        logLine("discard: \"%s\" gone (%u -> %u)", g_idName, g_idBefore, now);
        postSpeech(msg);
        return;
    }
    if (g_idSawDialog) {
        g_idPending = false;
        logLine("discard: dialog closed, \"%s\" kept", g_idName);
        return;
    }
    if (GetTickCount() >= g_idDeadline) {
        g_idPending = false;
        logLine("discard: no dialog and no change within 5s — reporting failure");
        postSpeech(axs(AXS_INV_DISCARD_DIDNT_HAPPEN));
    }
}

// ---- REARRANGE: Space picks a stack up, drops one on the focused slot; Ctrl+Space drops all ----

typedef int  (*InvPourFn)(void*, void*, void*, int);
typedef void (*InvSetIdentFn)(void*, const char*, const char*, int, uint32_t, int, unsigned char, void*, void*);
static const size_t INV_SETIDENT_P9_BYTES = 0x4c8;

bool             g_ihHeld = false;
static int       g_ihSrc  = -1;        // the slot it lives in (the stack never leaves it)
static uint32_t  g_ihTypeHash = 0;
static uint32_t  g_ihIdHash   = 0;
static char      g_ihName[160] = {0};  // localized name at pickup, for the outcome lines

void ihRelease(const char* why) {
    if (!g_ihHeld) return;
    g_ihHeld = false;
    g_ihSrc  = -1;
    logLine("rearrange: hold released (%s)", why ? why : "unspecified");
}

static bool sehInvPour(uintptr_t base, uintptr_t sys, uintptr_t dst, uintptr_t src,
                       int count, int* pouredOut) {
    __try {
        int p = ((InvPourFn)(base + INV_POUR_RVA))(
            reinterpret_cast<void*>(sys), reinterpret_cast<void*>(dst),
            reinterpret_cast<void*>(src), count);
        if (pouredOut) *pouredOut = p;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

static bool sehInvSetIdent(uintptr_t base, uintptr_t dst, uintptr_t srcRec) {
    uintptr_t     emptyTriple[3] = {0, 0, 0};
    static unsigned char zeroBlock[INV_SETIDENT_P9_BYTES];   // static: 1.2 KB, keep it off the stack
    memset(zeroBlock, 0, sizeof zeroBlock);
    __try {
        ((InvSetIdentFn)(base + INV_SETIDENT_RVA))(
            reinterpret_cast<void*>(dst),
            reinterpret_cast<const char*>(srcRec + ITEM_TYPE_OFF),
            reinterpret_cast<const char*>(srcRec + ITEM_ID_OFF), 0,
            0xffffffffu, -1, 0, emptyTriple, zeroBlock);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

static bool sehInvSwap(uintptr_t a, uintptr_t b) {
    unsigned char tmp[ITEM_STRIDE];
    __try {
        memcpy(tmp, reinterpret_cast<void*>(a), ITEM_STRIDE);
        memcpy(reinterpret_cast<void*>(a), reinterpret_cast<void*>(b), ITEM_STRIDE);
        memcpy(reinterpret_cast<void*>(b), tmp, ITEM_STRIDE);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

static bool ihPickUp(uintptr_t base) {
    char name[160];
    uintptr_t item = 0;
    if (!iuFocusedItem(base, &item, name, sizeof name)) return true;   // it spoke the refusal
    uint32_t amount = 0;
    safeReadU32(item + ITEM_AMOUNT_OFF, &amount);
    safeReadU32(item + ITEM_TYPEHASH_OFF, &g_ihTypeHash);
    safeReadU32(item + ITEM_IDHASH_OFF, &g_ihIdHash);
    _snprintf(g_ihName, sizeof g_ihName, "%s", name);
    g_ihName[sizeof g_ihName - 1] = 0;
    g_ihSrc  = g_invSlot;
    g_ihHeld = true;
    char act[96]; act[0] = 0;
    bool bound = kbActionForKey(base, SDLK_SPACE, act, sizeof act);
    logLine("rearrange: picked up slot %d \"%s\" x%u (game's Space action: %s)",
            g_ihSrc, g_ihName, amount, bound ? act : "none");
    char msg[512];
    _snprintf(msg, sizeof msg, axs(AXS_INV_PICKED_UP_FMT), g_ihName, (int)amount);
    msg[sizeof msg - 1] = 0;
    postSpeech(msg);
    return true;
}

static bool ihDrop(uintptr_t base, bool all) {
    uintptr_t beg = 0; int slots = 0;
    uintptr_t root = mapRoot(base);
    if (!root || !invItemVector(base, &beg, &slots)) {
        ihRelease("bag unreadable at drop");
        postSpeech(axs(AXS_INV_HELD_GONE));
        return true;
    }
    uint32_t srcAmt = 0, th = 0, ih = 0;
    uintptr_t src = beg + (uintptr_t)g_ihSrc * ITEM_STRIDE;
    if (g_ihSrc >= 0 && g_ihSrc < slots) {
        safeReadU32(src + ITEM_AMOUNT_OFF, &srcAmt);
        safeReadU32(src + ITEM_TYPEHASH_OFF, &th);
        safeReadU32(src + ITEM_IDHASH_OFF, &ih);
    }
    if (g_ihSrc < 0 || g_ihSrc >= slots || (int32_t)srcAmt < 1 ||
        th != g_ihTypeHash || ih != g_ihIdHash) {
        logLine("rearrange: held slot %d no longer matches (amt=%d th=%08x/%08x ih=%08x/%08x)",
                g_ihSrc, (int32_t)srcAmt, th, g_ihTypeHash, ih, g_ihIdHash);
        ihRelease("held item gone");
        postSpeech(axs(AXS_INV_HELD_GONE));
        return true;
    }
    int destIdx = g_invSlot;
    if (destIdx < 0 || destIdx >= slots) { postSpeech(axs(AXS_NO_ITEM_SELECTED)); return true; }
    if (destIdx == g_ihSrc)              { postSpeech(axs(AXS_INV_SAME_SLOT));    return true; }

    uintptr_t dst = beg + (uintptr_t)destIdx * ITEM_STRIDE;
    uint32_t dstBefore = 0, dth = 0, dih = 0;
    safeReadU32(dst + ITEM_AMOUNT_OFF, &dstBefore);
    safeReadU32(dst + ITEM_TYPEHASH_OFF, &dth);
    safeReadU32(dst + ITEM_IDHASH_OFF, &dih);
    bool dstEmpty = (int32_t)dstBefore < 1;
    bool sameKind = !dstEmpty && dth == th && dih == ih;
    char msg[700];

    if (!dstEmpty && !sameKind) {
        char t[64], i2[64], k[192], other[256];
        invItemName(base, dst, t, i2, k, other);
        if (!all) {
            _snprintf(msg, sizeof msg, axs(AXS_INV_OTHER_ITEM_FMT), other);
            msg[sizeof msg - 1] = 0;
            postSpeech(msg);
            return true;
        }
        bool ok = sehInvSwap(src, dst);
        uint32_t nth = 0;
        safeReadU32(dst + ITEM_TYPEHASH_OFF, &nth);   // observe: the held identity moved here
        logLine("rearrange: swap slot %d <-> %d %s (dst typeHash now %08x, held %08x)",
                g_ihSrc, destIdx, ok ? "done" : "FAULTED", nth, g_ihTypeHash);
        if (!ok || nth != g_ihTypeHash) { postSpeech(axs(AXS_ACTION_FAILED)); return true; }
        _snprintf(msg, sizeof msg, axs(AXS_INV_SWAPPED_FMT), g_ihName, other);
        msg[sizeof msg - 1] = 0;
        ihRelease("swapped");
        postSpeech(msg);
        return true;
    }

    if (dstEmpty && all) {
        bool ok = sehInvSwap(src, dst);
        uint32_t moved = 0;
        safeReadU32(dst + ITEM_AMOUNT_OFF, &moved);   // observe: the amount travelled
        logLine("rearrange: move slot %d -> %d %s (dst amount now %u of %u)",
                g_ihSrc, destIdx, ok ? "done" : "FAULTED", moved, srcAmt);
        if (!ok || moved != srcAmt) { postSpeech(axs(AXS_ACTION_FAILED)); return true; }
        _snprintf(msg, sizeof msg, axs(AXS_INV_MOVED_FMT), g_ihName, destIdx + 1);
        msg[sizeof msg - 1] = 0;
        ihRelease("moved");
        postSpeech(msg);
        return true;
    }

    if (dstEmpty) {
        if (!sehInvSetIdent(base, dst, src)) {
            logLine("rearrange: set-identity FAULTED (slot %d)", destIdx);
            postSpeech(axs(AXS_ACTION_FAILED));
            return true;
        }
    }

    int want = all ? (int)srcAmt : 1;
    int poured = 0;
    bool called = sehInvPour(base, root + INV_SYSTEM_OFF, dst, src, want, &poured);
    uint32_t dstAfter = dstBefore, srcAfter = srcAmt;
    safeReadU32(dst + ITEM_AMOUNT_OFF, &dstAfter);    // observe both ends; the return is a log line
    safeReadU32(src + ITEM_AMOUNT_OFF, &srcAfter);
    int gained = (int)dstAfter - (dstEmpty ? 0 : (int)dstBefore);
    logLine("rearrange: pour %d of \"%s\" slot %d -> %d: called=%d ret=%d dst %u -> %u src %u -> %u",
            want, g_ihName, g_ihSrc, destIdx, called ? 1 : 0, poured,
            dstBefore, dstAfter, srcAmt, srcAfter);
    if (!called)     { postSpeech(axs(AXS_ACTION_FAILED));  return true; }
    if (gained <= 0) { postSpeech(axs(AXS_INV_STACK_FULL)); return true; }

    if ((int32_t)srcAfter < 1) {
        _snprintf(msg, sizeof msg, axs(AXS_INV_DROPPED_ALL_FMT), g_ihName, (int)dstAfter);
        msg[sizeof msg - 1] = 0;
        ihRelease("dropped all");
        postSpeech(msg);
        return true;
    }
    if (all) _snprintf(msg, sizeof msg, axs(AXS_INV_DROPPED_PART_FMT), gained, (int)srcAfter);
    else     _snprintf(msg, sizeof msg, axs(AXS_INV_DROPPED_ONE_FMT), (int)dstAfter, (int)srcAfter);
    msg[sizeof msg - 1] = 0;
    postSpeech(msg);
    return true;
}

bool routeInvKey(uintptr_t base, uint32_t sym, uint16_t mod, uint8_t repeat) {
    if (sym == SDLK_SPACE && !(mod & (KMOD_LALT | KMOD_RALT))) {
        if (repeat) return true;
        bool ctrl = (mod & (KMOD_LCTRL | KMOD_RCTRL)) != 0;
        if (!g_ihHeld) {
            if (ctrl) { postSpeech(axs(AXS_INV_NOTHING_HELD)); return true; }
            return ihPickUp(base);
        }
        return ihDrop(base, ctrl);
    }
    if (!(mod & (KMOD_LALT | KMOD_RALT | KMOD_LCTRL | KMOD_RCTRL))) {
        if (sym == SDLK_RETURN || sym == SDLK_KP_ENTER) {
            if (repeat) return true;
            return iuBegin(base);
        }
        if (sym == SDLK_DELETE) {
            if (repeat) return true;
            return invDiscardBegin(base);
        }
        if (sym == SDLK_ESCAPE && g_ihHeld) {
            if (repeat) return true;
            ihRelease("Escape");
            postSpeech(axs(AXS_CANCELLED));
            return true;
        }
        if (sym == SDLK_ESCAPE && g_ruEquipHero) {
            if (repeat) return true;
            ruDisarmEquip("Escape");
            postSpeech(axs(AXS_CANCELLED));
            return true;
        }
    }
    {
        int jump = 0;
        if (axDecodeJump(sym, mod, repeat, &jump)) {
            if (!jump) return true;                    // held jump: one landing per press
            int slots = invSlotCount(base);
            if (slots <= 0) { logLine("invnav: no slots to navigate"); return true; }
            int jt = (jump > 0) ? slots - 1 : 0;
            char buf[MAILBOX_SZ];
            if (!invSlotText(base, jt, buf, sizeof buf, invNavTag(base, jt))) {
                logLine("invnav: slot %d did not read", jt);
                return true;
            }
            logLine("invnav jump slot %d -> %d (of %d)", g_invSlot, jt, slots);
            g_invSlot = jt;
            postSpeech(buf);
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
    if (mod & (KMOD_LALT | KMOD_RALT | KMOD_LCTRL | KMOD_RCTRL)) return false;
    if (axNavHoldRepeat(repeat)) return true;    // throttled repeat: claimed, no step

    int slots = invSlotCount(base);
    if (slots <= 0) {                    // panel is up but the bag did not read back
        logLine("invnav: no slots to navigate");
        return true;
    }
    if (g_invSlot >= slots) g_invSlot = slots - 1;    // the bag shrank under the cursor

    int target;
    if (g_invSlot < 0) {
        target = 0;
    } else {
        int rows = (slots + INV_GRID_COLS - 1) / INV_GRID_COLS;
        int row  = g_invSlot / INV_GRID_COLS;
        int col  = g_invSlot % INV_GRID_COLS;
        switch (dir) {
            case 0: row--; break;
            case 1: row++; break;
            case 2: col--; break;
            default: col++; break;
        }
        target = row * INV_GRID_COLS + col;
        // Off the table, or off the ragged end of a partly-filled last row.
        if (row < 0 || row >= rows || col < 0 || col >= INV_GRID_COLS || target >= slots) {
            postSpeech(axs(AXS_INV_EDGE));
            return true;
        }
    }

    char buf[MAILBOX_SZ];
    if (!invSlotText(base, target, buf, sizeof buf, invNavTag(base, target))) {
        logLine("invnav: slot %d did not read", target);
        return true;
    }
    logLine("invnav dir=%d slot %d -> %d (of %d)", dir, g_invSlot, target, slots);
    g_invSlot = target;
    postSpeech(buf);
    return true;
}

static const int TRK_WATCH_MAX = 24;

struct TrkWatch {
    uintptr_t sys;
    int       slot;
    uintptr_t item;
    uintptr_t rec;
    uint32_t  idHash;   // which trinket is in the slot
    int       left;     // triggers remaining
};
static TrkWatch g_trkWatch[TRK_WATCH_MAX];
static int      g_trkWatchN = 0;
static bool     g_trkArmed  = false;

static int trkCollect(uintptr_t base, uintptr_t sys, TrkWatch* out, int max) {
    uintptr_t beg = 0;
    int slots = 0;
    if (max <= 0 || !invItemVectorAt(sys, &beg, &slots)) return 0;
    int n = 0;
    for (int i = 0; i < slots && n < max; i++) {
        uintptr_t item = beg + (uintptr_t)i * ITEM_STRIDE;
        int32_t amount = 0;
        if (!safeReadU32(item + ITEM_AMOUNT_OFF, (uint32_t*)&amount) || amount < 1) continue;
        int32_t left = -1;
        if (!safeReadU32(item + ITEM_TRIG_LEFT_OFF, (uint32_t*)&left) || left < 0) continue;
        if (!invItemIsTrinket(item)) continue;
        uint32_t idh = 0;
        if (!safeReadU32(item + ITEM_IDHASH_OFF, &idh) || !idh) continue;
        out[n].sys    = sys;
        out[n].slot   = i;
        out[n].item   = item;
        out[n].rec    = bldTrinketRecord(base, item);
        out[n].idHash = idh;
        out[n].left   = left;
        n++;
    }
    return n;
}

static const TrkWatch* trkFind(const TrkWatch* tbl, int n, uintptr_t sys, int slot) {
    for (int i = 0; i < n; i++)
        if (tbl[i].sys == sys && tbl[i].slot == slot) return &tbl[i];
    return nullptr;
}

// "Warm Scarf triggered. Trigger Limit: 7 of 8."
static void trkAnnounceSpend(uintptr_t base, const TrkWatch& w, int was) {
    char type[64], itemId[64], key[192], name[256];
    if (!invItemName(base, w.item, type, itemId, key, name) || !name[0]) {
        logLine("trktrigger: slot %d spent a trigger (%d -> %d) but did not name -- not spoken",
                w.slot, was, w.left);
        return;
    }
    char count[192], line[MAILBOX_SZ];
    if (bldTrinketTriggerLine(base, w.item, w.rec, count, sizeof count) && count[0])
        _snprintf(line, sizeof line, axs(AXS_TRK_TRIGGERED_FMT), name, count);
    else
        _snprintf(line, sizeof line, axs(AXS_TRK_TRIGGERED_BARE_FMT), name);
    line[sizeof line - 1] = 0;
    logLine("trktrigger: \"%s\" %d -> %d -> \"%s\"", name, was, w.left, line);
    postSpeech(line, false, SPK_EVENT);
}

static void trkAnnounceTransform(uintptr_t base, const TrkWatch& was, const TrkWatch& now) {
    char intoName[192];
    if (!bldTrinketExhaustTransform(base, was.rec, now.idHash, intoName, sizeof intoName)) return;
    char fromName[256];
    if (!bldTrinketRecName(base, was.rec, fromName, sizeof fromName) || !fromName[0]) return;
    char line[MAILBOX_SZ];
    _snprintf(line, sizeof line, axs(AXS_TRK_TRANSFORMED_FMT), fromName, intoName);
    line[sizeof line - 1] = 0;
    logLine("trktrigger: transform \"%s\" -> \"%s\"", fromName, intoName);
    postSpeech(line, false, SPK_EVENT);
}

void serviceTrinketTrigger(uintptr_t base) {
    uintptr_t root = 0;
    if (!safeReadPtr(base + MAP_ROOT_RVA, &root) || root <= 0x10000) {
        g_trkWatchN = 0;
        g_trkArmed  = false;
        return;
    }

    TrkWatch now[TRK_WATCH_MAX];
    int n = 0;
    uintptr_t heroes[RV_MAX_MEMBERS];
    int nh = rvPartyList(base, heroes, RV_MAX_MEMBERS);
    for (int h = 0; h < nh && n < TRK_WATCH_MAX; h++) {
        if (!heroes[h]) continue;
        n += trkCollect(base, heroes[h] + HERO_TRINKET_SYSTEM_OFF, now + n, TRK_WATCH_MAX - n);
    }
    if (n < TRK_WATCH_MAX)
        n += trkCollect(base, root + INV_SYSTEM_OFF, now + n, TRK_WATCH_MAX - n);

    if (g_trkArmed) {
        for (int i = 0; i < n; i++) {
            const TrkWatch* was = trkFind(g_trkWatch, g_trkWatchN, now[i].sys, now[i].slot);
            if (!was) continue;
            if (was->idHash == now[i].idHash) {
                if (now[i].left < was->left) trkAnnounceSpend(base, now[i], was->left);
            } else if (was->left == 0) {
                trkAnnounceTransform(base, *was, now[i]);
            }
        }
    }

    for (int i = 0; i < n; i++) g_trkWatch[i] = now[i];
    g_trkWatchN = n;
    g_trkArmed  = true;
}
