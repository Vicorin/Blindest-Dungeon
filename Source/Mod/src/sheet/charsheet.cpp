// sheet/charsheet.cpp -- the CHARACTER SHEET module

#include <cstdint>
#include <cstring>
#include <cstdio>
#include "internal.h"

// ---- constants that moved WITH the frame (this module is their only reader; the RE ----
static const uintptr_t CS_LIST_OWNER_OFF  = 0x1730;
static const uintptr_t CS_LIST_BEGIN_OFF  = 0x18;      // list owner+: vector begin (ptr entries)
static const uintptr_t CS_LIST_END_OFF    = 0x20;      // list owner+: vector end
static const uintptr_t CS_PANEL_STRIDE    = 8;         // the entries are pointers
static const int       CS_MAX_PANELS      = 32;        // sanity cap on the panel-vector walk
static const uintptr_t HEROCLASS_MAXSELECT_OFF = 0x11e8; // HeroClass+: uint, max selected combat
                                                       // skills -- where the familiar 4 comes from (was 0x11d8)

// ---- CHARACTER SHEET: is it open, and what is on it? ----

static uintptr_t csOpenPanel(uintptr_t base) {
    uintptr_t screen = 0, owner = 0, begin = 0, end = 0;
    if (!safeReadPtr(base + RAID_SCREEN_RVA, &screen) || screen <= 0x10000) return 0;
    if (!safeReadPtr(screen + CS_LIST_OWNER_OFF, &owner) || owner <= 0x10000) return 0;
    if (!safeReadPtr(owner + CS_LIST_BEGIN_OFF, &begin) || begin <= 0x10000) return 0;
    if (!safeReadPtr(owner + CS_LIST_END_OFF, &end) || end <= begin) return 0;

    uintptr_t count = (end - begin) / CS_PANEL_STRIDE;
    if (count > CS_MAX_PANELS) count = CS_MAX_PANELS;   // a wild length means we read the wrong thing
    for (uintptr_t i = 0; i < count; i++) {
        uintptr_t panel = 0;
        if (!safeReadPtr(begin + i * CS_PANEL_STRIDE, &panel) || panel <= 0x10000) continue;
        uint32_t open = 0;
        if (safeReadU32(panel + CS_PANEL_OPEN_OFF, &open) && open != 0) return panel;
    }
    return 0;
}

volatile bool g_csOpen   = false;
static bool          g_csDumped = false;  // the one-shot dump has run this session

static int g_csSection = 0;
static int g_csRow     = -1;
static int g_csTipLine = 0;

static DWORD g_csSkillWatchUntil = 0;
static int   g_csSkillWatchIdx   = -1;
static bool  g_csSkillWantEquipped = false;
static char  g_csSkillWatchName[128];
static bool  g_csSkillWatchCamp = false;

static int   g_csTrkConfirmSlot = -1;      // armed unequip confirm; -1 = none
static char  g_csTrkConfirmName[256];
static DWORD g_csTrkWatchUntil = 0;        // the unequip outcome watch
static int   g_csTrkWatchSlot = -1;
static char  g_csTrkWatchName[256];
typedef void (*CsTrkUnequipFn)(void* closure, uint32_t* slot);
static bool csSehUnequipTrinket(uintptr_t base, uintptr_t hero, int slot) {
    struct { uintptr_t vft, pad, hero; } c = { 0, 0, hero };   // the lambda reads only +0x10
    uint32_t s = (uint32_t)slot;
    __try { ((CsTrkUnequipFn)(base + CS_TRK_UNEQUIP_RVA))(&c, &s); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

int csResolveLevel(uintptr_t base, uintptr_t hero) {
    uint8_t isMonster = 1;
    if (!safeReadU8(hero + ACTOR_IS_MONSTER_OFF, &isMonster) || isMonster) return -1;

    uint32_t xp = 0;
    if (!safeReadU32(hero + HERO_RESOLVE_XP_OFF, &xp)) return -1;

    uintptr_t tbl = 0, begin = 0, end = 0;
    if (!safeReadPtr(base + RESOLVE_TABLE_RVA, &tbl) || tbl <= 0x10000) return -1;
    if (!safeReadPtr(tbl + RESOLVE_VEC_BEGIN, &begin) || begin <= 0x10000) return -1;
    if (!safeReadPtr(tbl + RESOLVE_VEC_END, &end) || end <= begin) return -1;

    uintptr_t count = (end - begin) / 4;
    if (count == 0 || count > RESOLVE_MAX_LEVELS) {      // a wild length => we read the wrong object
        logLine("charsheet rank: threshold table length %llu is implausible", (unsigned long long)count);
        return -1;
    }
    int level = 0;
    for (uintptr_t i = 0; i < count; i++) {
        uint32_t th = 0;
        if (!safeReadU32(begin + i * 4, &th)) return -1;
        if (xp < th) break;
        level = (int)i;
    }
    static uint32_t lastXp = 0xffffffffu;
    static int      lastLevel = -2;
    if (xp != lastXp || level != lastLevel) {
        lastXp = xp; lastLevel = level;
        logLine("charsheet rank: xp=%u thresholds=%llu -> level %d", xp, (unsigned long long)count, level);
    }
    return level;
}

static bool csTitleLine(uintptr_t base, char* out, int outsz) {
    char name[80], cls[80];
    if (!abHeroNameClass(base, name, sizeof name, cls, sizeof cls)) return false;

    char rank[128] = { 0 };
    uintptr_t hero = abSelectedHero(base);

    char deadWord[96];
    if (hero && rvCorpseWord(base, hero, deadWord, sizeof deadWord)) {
        _snprintf(cls, sizeof cls, "%s", deadWord);
        cls[sizeof cls - 1] = 0;
        hero = 0;
    }

    if (hero) {
        int level = csResolveLevel(base, hero);
        if (level < 0) {
            logLine("charsheet: rank unreadable for hero=%p", (void*)hero);
        } else {
            char key[32];
            _snprintf(key, sizeof key, "str_resolve_%d", level);
            key[sizeof key - 1] = 0;
            if (!resolveKey(base, key, rank, sizeof rank)) {
                logLine("charsheet: \"%s\" did not resolve", key);
                rank[0] = 0;
            }
        }
    }

    // "<rank> <class>", collapsing to whichever half is readable.
    char title[224];
    if (rank[0] && cls[0]) _snprintf(title, sizeof title, "%s %s", rank, cls);
    else                   _snprintf(title, sizeof title, "%s", rank[0] ? rank : cls);
    title[sizeof title - 1] = 0;

    char resolveFrag[160] = { 0 };
    if (hero) {
        uint32_t th = 0;
        char tname[128] = { 0 };
        if (safeReadU32(hero + ACTOR_TRAIT_OFF, &th) && th &&
            spTraitName(base, th, false, tname, sizeof tname)) {
            resolveFrag[0] = ' ';
            _snprintf(resolveFrag + 1, sizeof resolveFrag - 1, axs(AXS_CS_AFFLICTED_FMT), tname);
        } else if (safeReadU32(hero + ACTOR_VIRTUE_OFF, &th) && th &&
                   spTraitName(base, th, true, tname, sizeof tname)) {
            resolveFrag[0] = ' ';
            _snprintf(resolveFrag + 1, sizeof resolveFrag - 1, axs(AXS_CS_VIRTUOUS_FMT), tname);
        }
        resolveFrag[sizeof resolveFrag - 1] = 0;
    }

    char naFrag[112] = { 0 };
    if (hero && csHeroIsNeverAgain(hero)) {
        char word[96] = { 0 };
        csNeverAgainWord(base, word, sizeof word);
        _snprintf(naFrag, sizeof naFrag, " %s.", word);
        naFrag[sizeof naFrag - 1] = 0;
    }

    if (name[0] && title[0]) _snprintf(out, outsz, "%s, %s.%s%s", name, title, naFrag, resolveFrag);
    else                     _snprintf(out, outsz, "%s.%s%s", name[0] ? name : title, naFrag, resolveFrag);
    out[outsz - 1] = 0;
    return true;
}

static void csSpeakTitle(uintptr_t base, const char* why) {
    char out[512];
    if (!csTitleLine(base, out, sizeof out)) {
        logLine("charsheet title (%s): no label", why);
        return;
    }
    logLine("charsheet title (%s): \"%s\"", why, out);
    postSpeech(out);
}

static bool      g_trActive         = false;   // the sheet's edit flag, edge-tracked
static uintptr_t g_trHero           = 0;       // the hero being renamed
static char      g_trOld[64];                  // their name when the box opened
static DWORD     g_trOpenWatchUntil = 0;       // Enter on the Rename row armed this watch

// ---- COMBAT SKILLS ----

int csComSkillCount(uintptr_t base) {
    uintptr_t cobj = abHeroClass(base);
    if (!cobj) return 0;
    uintptr_t beg = 0, end = 0;
    if (!safeReadPtr(cobj + HEROCLASS_SKILLVEC_OFF, &beg) ||
        !safeReadPtr(cobj + HEROCLASS_SKILLVEC_END_OFF, &end)) return 0;
    if (beg <= 0x10000 || end < beg) return 0;
    uintptr_t n = (end - beg) / 0x18;
    if (n > 32) return 0;
    return (int)n;
}

static int csComSkillLevelPurchased(uintptr_t base, uintptr_t hero, uintptr_t cobj, int i) {
    if (!hero || !cobj || i < 0) return -1;
    char cls[64] = {0}, sid[64] = {0};
    if (!safeReadCStr(cobj + HEROCLASS_ID_OFF, cls, sizeof cls) || !cls[0]) return -1;
    uintptr_t vecArr = 0, inner = 0, innerEnd = 0;
    if (!safeReadPtr(cobj + HEROCLASS_SKILLVEC_OFF, &vecArr) || vecArr <= 0x10000) return -1;
    if (!safeReadPtr(vecArr + (uintptr_t)i * 0x18, &inner) || inner <= 0x10000) return -1;
    if (!safeReadCStr(inner + SKILL_ID_OFF, sid, sizeof sid) || !sid[0]) return -1;
    uint32_t guid = 0;
    if (!safeReadU32(hero + ACTOR_ID_OFF, &guid)) return -1;

    char tree[136];
    _snprintf(tree, sizeof tree, "%s.%s", cls, sid);   // the game's own "%s.%s" over the same two ids
    tree[sizeof tree - 1] = 0;
    uint32_t h = resHash(tree);
    if (!haPurchased(base, h, (uint8_t)'0', guid)) return -1;   // '0' = learn: never bought

    int maxLvl = 0;
    if (safeReadPtr(vecArr + (uintptr_t)i * 0x18 + 8, &innerEnd) && innerEnd > inner)
        maxLvl = (int)((innerEnd - inner) / SKILL_STRIDE) - 1;
    if (maxLvl < 0 || maxLvl > 15) maxLvl = 0;

    int lvl = 0;
    for (int c = 0; c < maxLvl; c++) {
        if (!haPurchased(base, h, (uint8_t)('1' + c), guid)) break;
        lvl++;
    }
    return lvl;
}

static uintptr_t g_csLvlLogHero = 0;
static uint32_t  g_csLvlLogMask = 0;

int csComSkillLevelOf(uintptr_t base, uintptr_t hero, uintptr_t cobj, int i) {
    if (!hero || i < 0 || i >= CS_COMSKILL_MAX) return -1;
    uintptr_t beg = 0, end = 0;
    int32_t cached = -1;
    if (safeReadPtr(hero + ACTOR_SLOTMAP_OFF, &beg) &&
        safeReadPtr(hero + ACTOR_SLOTMAP_END_OFF, &end) &&
        beg > 0x10000 && end >= beg && (uintptr_t)i < (end - beg) / 4) {
        if (!safeReadU32(beg + (uintptr_t)i * 4, (uint32_t*)&cached)) cached = -1;
    }
    if (cached >= 0) return cached;                    // fresh cache = the number the grid draws

    int live = csComSkillLevelPurchased(base, hero, cobj, i);
    if (live >= 0) {
        if (hero != g_csLvlLogHero) { g_csLvlLogHero = hero; g_csLvlLogMask = 0; }
        if (!(g_csLvlLogMask & (1u << i))) {
            g_csLvlLogMask |= (1u << i);
            logLine("comskill %d: the hero's slot-map cache says not learned, the save-backed "
                    "purchase map says level %d -- using the save (the cache is only rebuilt when "
                    "a skill grid draws or a purchase lands)", i, live);
        }
    }
    return live;
}

int csComSkillLevel(uintptr_t base, int i) {
    return csComSkillLevelOf(base, abSelectedHero(base), abHeroClass(base), i);
}

uintptr_t csComSkillAt(uintptr_t base, int i) {
    uintptr_t cobj = abHeroClass(base);
    if (!cobj || i < 0 || i >= csComSkillCount(base)) return 0;
    uintptr_t vecArr = 0;
    if (!safeReadPtr(cobj + HEROCLASS_SKILLVEC_OFF, &vecArr) || vecArr <= 0x10000) return 0;
    uintptr_t inner = 0;
    if (!safeReadPtr(vecArr + (uintptr_t)i * 0x18, &inner) || inner <= 0x10000) return 0;
    int lvl = csComSkillLevel(base, i);
    if (lvl < 0) lvl = 0;                 // the game's own clamp
    if (lvl > 15) return 0;
    return inner + (uintptr_t)lvl * SKILL_STRIDE;
}

bool csComSkillEquipped(uintptr_t base, int i) {
    uintptr_t hero = abSelectedHero(base);
    if (!hero || i < 0) return false;
    uintptr_t beg = 0, end = 0;
    if (!safeReadPtr(hero + ACTOR_SEL_BEG_OFF, &beg) ||
        !safeReadPtr(hero + ACTOR_SEL_END_OFF, &end)) return false;
    if (beg <= 0x10000 || end < beg) return false;
    uintptr_t n = (end - beg) / 4;
    if (n > 32) return false;
    for (uintptr_t k = 0; k < n; k++) {
        uint32_t slot = 0;
        if (!safeReadU32(beg + k * 4, &slot)) continue;
        if ((int)slot == i) return true;
    }
    return false;
}

// ---- SLOTS: the equipped vector IS the action bar's order ----

static int csComSlotList(uintptr_t base, int slots[CS_COMSKILL_MAX]) {
    uintptr_t hero = abSelectedHero(base);
    if (!hero) return 0;
    uintptr_t beg = 0, end = 0;
    if (!safeReadPtr(hero + ACTOR_SEL_BEG_OFF, &beg) ||
        !safeReadPtr(hero + ACTOR_SEL_END_OFF, &end)) return 0;
    if (beg <= 0x10000 || end < beg) return 0;
    uintptr_t n = (end - beg) / 4;
    if (n > CS_COMSKILL_MAX) return 0;              // a hero has at most four; this is a torn read
    int k = 0;
    for (uintptr_t i = 0; i < n; i++) {
        uint32_t v = 0;
        if (!safeReadU32(beg + i * 4, &v)) return 0;
        slots[k++] = (int)v;
    }
    return k;
}

static int csComSkillSlot(uintptr_t base, int i) {
    if (i < 0) return 0;
    int slots[CS_COMSKILL_MAX];
    int n = csComSlotList(base, slots);
    for (int s = 0; s < n; s++) if (slots[s] == i) return s + 1;
    return 0;
}

static const int CS_MAX_EQUIPPED_FALLBACK = 4;
static int csComMaxSelected(uintptr_t base) {
    uintptr_t cobj = abHeroClass(base);
    uint32_t cap = 0;
    if (!cobj || !safeReadU32(cobj + HEROCLASS_MAXSELECT_OFF, &cap)) return CS_MAX_EQUIPPED_FALLBACK;
    if (cap == 0 || cap > CS_COMSKILL_MAX) return CS_MAX_EQUIPPED_FALLBACK;
    return (int)cap;
}

bool csComSkillLearned(uintptr_t base, int i) {
    return csComSkillLevel(base, i) >= 0 || csComSkillEquipped(base, i);
}

bool csComSkillRowText(uintptr_t base, int i, char* out, int outsz) {
    uintptr_t skill = csComSkillAt(base, i);
    if (!skill) {
        logLine("charsheet comskill: row %d did not resolve a skill", i);
        return false;
    }

    char fallback[64];
    _snprintf(fallback, sizeof fallback, axs(AXS_CS_SKILL_N_FMT), i + 1);
    char titled[320];
    abSkillTitled(base, skill, fallback, titled, sizeof titled, false);

    char state[32];
    int slot = csComSkillSlot(base, i);
    if      (!csComSkillLearned(base, i)) strncpy(state, axs(AXS_CS_SKILL_NOT_LEARNED), sizeof state - 1);
    else if (slot > 0)                    _snprintf(state, sizeof state, axs(AXS_CS_SKILL_SLOT_FMT), slot);
    else if (csComSkillEquipped(base, i)) strncpy(state, axs(AXS_CS_SKILL_SELECTED), sizeof state - 1);
    else                                  strncpy(state, axs(AXS_CS_SKILL_LEARNED), sizeof state - 1);
    state[sizeof state - 1] = 0;
    _snprintf(out, outsz, "%s. %s.", titled, state);
    out[outsz - 1] = 0;
    return true;
}

static int csComSkillTipLines(uintptr_t base, int i, char lines[AB_TIP_MAX_LINES][AB_TIP_LINE_SZ]) {
    uintptr_t skill = csComSkillAt(base, i);
    if (!skill) return 0;
    char fallback[64];
    _snprintf(fallback, sizeof fallback, axs(AXS_CS_SKILL_N_FMT), i + 1);
    return abBuildSkillLines(base, skill, 0, fallback, lines);
}

// ---- PREFERRED POSITION / TARGET PIPS ----

static bool csPipCounts(uintptr_t base, int launch[4], int support[4], int attack[4]) {
    for (int r = 0; r < 4; r++) { launch[r] = support[r] = attack[r] = 0; }
    int n = csComSkillCount(base);
    if (n <= 0) return false;
    bool any = false;
    for (int i = 0; i < n; i++) {
        if (!csComSkillEquipped(base, i)) continue;           // equipped only, exactly like the game
        uintptr_t skill = csComSkillAt(base, i);
        if (!skill) continue;
        uint8_t launchMask = 0, targetMask = 0, friendly = 0;
        if (!safeReadU8(skill + SKILL_LAUNCH_MASK_OFF, &launchMask)) continue;
        safeReadU8(skill + SKILL_TGTMASK_OFF,     &targetMask);
        safeReadU8(skill + SKILL_NONATTACK_OFF,   &friendly);
        any = true;
        for (int r = 0; r < 4; r++) {
            uint8_t bit = (uint8_t)(1 << r);
            if (launchMask & bit) { if (launch[r] < 4) launch[r]++; }       // any skill => position glow
            if (targetMask & bit) {
                if (friendly) { if (support[r] < 4) support[r]++; }         // heal/buff => green support
                else          { if (attack[r]  < 4) attack[r]++;  }         // attack   => red target
            }
        }
    }
    return any;
}

static bool csPipPositionRow(uintptr_t base, char* out, int outsz) {
    char hdr[128];
    if (!resolveKey(base, "character_title_position_pips", hdr, sizeof hdr)) {
        strncpy(hdr, axs(AXS_CS_PREF_POSITIONS), sizeof hdr - 1); hdr[sizeof hdr - 1] = 0;
    }
    abStripMarkup(hdr); csTrimLabel(hdr);

    int launch[4], support[4], attack[4];
    if (!csPipCounts(base, launch, support, attack)) {
        _snprintf(out, outsz, "%s. %s", hdr, axs(AXS_NOT_AVAILABLE)); out[outsz - 1] = 0; return true;
    }
    char body[MAILBOX_SZ]; body[0] = 0; bool anyslot = false;
    for (int r = 0; r <= 3; r++) {
        int rank = r + 1, L = launch[r], F = support[r];
        if (L == 0 && F == 0) continue;
        anyslot = true;
        char slot[96];
        // Number-neutral: "%d skill%s" was a plural hack no translator can decline.
        slot[0] = ' ';
        if (L > 0 && F > 0)
            _snprintf(slot + 1, sizeof slot - 1, axs(AXS_CS_POS_SKILLS_SUPPORT_FMT), rank, L);
        else if (L > 0)
            _snprintf(slot + 1, sizeof slot - 1, axs(AXS_CS_POS_SKILLS_FMT), rank, L);
        else
            _snprintf(slot + 1, sizeof slot - 1, axs(AXS_CS_POS_SUPPORT_FMT), rank);
        slot[sizeof slot - 1] = 0;
        strncat(body, slot, sizeof body - strlen(body) - 1);
    }
    _snprintf(out, outsz, "%s.%s%s", hdr, anyslot ? "" : " ", anyslot ? body : axs(AXS_CS_NONE));
    out[outsz - 1] = 0;
    return true;
}

static bool csPipTargetRow(uintptr_t base, char* out, int outsz) {
    char hdr[128];
    if (!resolveKey(base, "character_title_target_pips", hdr, sizeof hdr)) {
        strncpy(hdr, axs(AXS_CS_PREF_TARGETS), sizeof hdr - 1); hdr[sizeof hdr - 1] = 0;
    }
    abStripMarkup(hdr); csTrimLabel(hdr);

    int launch[4], support[4], attack[4];
    if (!csPipCounts(base, launch, support, attack)) {
        _snprintf(out, outsz, "%s. %s", hdr, axs(AXS_NOT_AVAILABLE)); out[outsz - 1] = 0; return true;
    }
    char body[MAILBOX_SZ]; body[0] = 0; bool anyslot = false;
    for (int r = 0; r <= 3; r++) {                    // rank 1 (leftmost) .. rank 4 (rightmost)
        int rank = r + 1, T = attack[r];
        if (T == 0) continue;
        anyslot = true;
        char slot[96];
        slot[0] = ' ';
        _snprintf(slot + 1, sizeof slot - 1, axs(AXS_CS_POS_TARGET_FMT), rank, T);
        slot[sizeof slot - 1] = 0;
        strncat(body, slot, sizeof body - strlen(body) - 1);
    }
    _snprintf(out, outsz, "%s.%s%s", hdr, anyslot ? "" : " ", anyslot ? body : axs(AXS_CS_NONE));
    out[outsz - 1] = 0;
    return true;
}

// ---- SORTING + THE HORIZONTAL STRIP ----
int csComSortedOrder(uintptr_t base, int order[CS_COMSKILL_MAX]) {
    int n = csComSkillCount(base);
    if (n <= 0) return 0;
    if (n > CS_COMSKILL_MAX) n = CS_COMSKILL_MAX;
    int k = 0;
    int slots[CS_COMSKILL_MAX];
    int ns = csComSlotList(base, slots);
    for (int s = 0; s < ns; s++) {                                  // equipped, in SLOT order
        int i = slots[s];
        if (i < 0 || i >= n) continue;                              // out-of-range entry: never mis-name a cell
        bool dup = false;
        for (int q = 0; q < k; q++) if (order[q] == i) dup = true;  // a duplicated entry gets one row
        if (!dup) order[k++] = i;
    }
    for (int i = 0; i < n; i++) {
        if (!csComSkillEquipped(base, i)) continue;
        bool have = false;
        for (int q = 0; q < k; q++) if (order[q] == i) have = true;
        if (!have) order[k++] = i;
    }
    for (int i = 0; i < n; i++) if (!csComSkillEquipped(base, i) &&  csComSkillLearned(base, i)) order[k++] = i; // Learned
    for (int i = 0; i < n; i++) if (!csComSkillEquipped(base, i) && !csComSkillLearned(base, i)) order[k++] = i; // Not learned
    return k;
}
static int csComColToSkill(uintptr_t base, int col) {
    int order[CS_COMSKILL_MAX];
    int cnt = csComSortedOrder(base, order);
    if (cnt <= 0 || col < 0 || col >= cnt) return -1;
    return order[col];
}
static int csComSkillToCol(uintptr_t base, int skillIdx) {
    int order[CS_COMSKILL_MAX];
    int cnt = csComSortedOrder(base, order);
    for (int c = 0; c < cnt; c++) if (order[c] == skillIdx) return c;
    return -1;
}

static const int CS_COM_PIP_ITEMS = 2;     // preferred positions, preferred targets
static bool csComSectionRow(uintptr_t base, int item, char* out, int outsz) {
    int n = csComSkillCount(base);
    if (n <= 0) return false;
    if (item == 0) return csPipPositionRow(base, out, outsz);
    if (item == 1) return csPipTargetRow(base, out, outsz);
    int order[CS_COMSKILL_MAX];
    int cnt = csComSortedOrder(base, order);
    if (cnt <= 0) return false;
    int col = item - CS_COM_PIP_ITEMS;
    if (col < 0) col = 0;
    if (col >= cnt) col = cnt - 1;
    return csComSkillRowText(base, order[col], out, outsz);
}
static int csComSectionCount(uintptr_t base) {
    int n = csComSkillCount(base);
    if (n <= 0) return n;
    int order[CS_COMSKILL_MAX];
    return CS_COM_PIP_ITEMS + csComSortedOrder(base, order);   // pips + the skills actually listed
}
static int csComSectionTip(uintptr_t base, int item, char lines[AB_TIP_MAX_LINES][AB_TIP_LINE_SZ]) {
    if (item < CS_COM_PIP_ITEMS) return 0;
    int skillIdx = csComColToSkill(base, item - CS_COM_PIP_ITEMS);
    if (skillIdx < 0) return 0;
    return csComSkillTipLines(base, skillIdx, lines);
}

static const uint32_t CS_COMSKILL_OWNER    = 0x636f6d73; // 'coms' — the combat-skill grid owner
static const uint32_t CS_COMSKILL_CLICK_TAG = 0x6274746f;// the click-cell element id tag (pass 49)

static int csComSkillEquippedCount(uintptr_t base) {
    int n = csComSkillCount(base), c = 0;
    for (int i = 0; i < n; i++) if (csComSkillEquipped(base, i)) c++;
    return c;
}

static int64_t csComCellId(int i) {
    return (int64_t)(uint32_t)(CS_COMSKILL_OWNER + CS_COMSKILL_CLICK_TAG + (uint32_t)i);
}

static const uint32_t CS_CAMSKILL_OWNER = 0x63616d73; // 'cams' -- the camping-skill grid owner
static int64_t csCampCellId(int i) {
    return (int64_t)(uint32_t)(CS_CAMSKILL_OWNER + CS_COMSKILL_CLICK_TAG + (uint32_t)i);
}

// ---- THE DESELECT PROBE ----
static const uintptr_t CS_PANEL_EDITABLE_OFF = 0x14d;   // CharacterDisplay+: the cell closure's gate
static const uintptr_t CS_TOWN_SHEET_STATE_OFF = 0x3918; // townRoot+: the sheet's show state (0 = down)
static const uintptr_t CS_HERO_BACKER_OFF = 0x13b8;     // Hero+: backer_hero (was 0x1338)
static void csCampProbe(uintptr_t base, const char* when) {
    uintptr_t panel = csTownSheetPanel(base);
    uintptr_t root  = resTownRoot(base);
    uintptr_t hero  = abSelectedHero(base);
    uint8_t editable = 0xff, editable2 = 0xff, backer = 0xff;
    uint32_t state = 0xffffffff;
    if (panel) { safeReadU8(panel + CS_PANEL_EDITABLE_OFF, &editable);
                 safeReadU8(panel + CS_PANEL_EDITABLE_OFF + 1, &editable2); }
    if (root)  safeReadU32(root + CS_TOWN_SHEET_STATE_OFF, &state);
    if (hero)  safeReadU8(hero + CS_HERO_BACKER_OFF, &backer);
    char list[128] = {0};
    uintptr_t beg = 0, end = 0;
    if (hero && safeReadPtr(hero + ACTOR_CAMP_SEL_BEG_OFF, &beg) &&
        safeReadPtr(hero + ACTOR_CAMP_SEL_END_OFF, &end) && beg > 0x10000 && end >= beg &&
        (end - beg) / 4 <= 8) {
        for (uintptr_t k = 0; k < (end - beg) / 4; k++) {
            uint32_t v = 0; safeReadU32(beg + k * 4, &v);
            char one[16]; _snprintf(one, sizeof one, "%s%u", k ? "," : "", v);
            strncat(list, one, sizeof list - strlen(list) - 1);
        }
    } else strncpy(list, "(unreadable)", sizeof list - 1);
    int learned = 0, n = csCampSkillCount(base);
    for (int k = 0; k < n; k++) if (csCampSkillLearned(base, k)) learned++;
    logLine("charsheet camskill probe(%s): panel=%p editable(+0x14d)=%u (+0x14e)=%u "
            "sheetState(root+0x3918)=%u hero=%p backer(+0x13b8)=%u learned=%d/%d selected=[%s] "
            "gameFocus=0x%llx",
            when, (void*)panel, editable, editable2, state, (void*)hero, backer, learned, n, list,
            (unsigned long long)feCurrentFocusId());
}

// ---- ASSIGN A SKILL TO AN ACTION-BAR SLOT ----
typedef uint8_t (*CsComToggleFn)(void* hero, uint32_t idx);
static bool csSehToggleSkill(uintptr_t base, uintptr_t hero, int i, bool camp) {
    if (!hero || i < 0) return false;
    uintptr_t fn = base + (camp ? CS_CAMPSKILL_TOGGLE_RVA : CS_COMSKILL_TOGGLE_RVA);
    __try { return ((CsComToggleFn)fn)((void*)hero, (uint32_t)i) != 0; }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        logLine("charsheet %s: toggle FAULTED (hero=%p idx=%d)", camp ? "camskill" : "comskill", (void*)hero, i);
        return false;
    }
}

struct CsSlotResult {
    int  landed;
    int  displaced;
    int  movedIdx;
    int  movedTo;       // that skill's new 1-based slot
};

static bool csComAssignSlot(uintptr_t base, int want, int slot, CsSlotResult* res, bool camp) {
    res->landed = 0; res->displaced = -1; res->movedIdx = -1; res->movedTo = 0;
    uintptr_t hero = abSelectedHero(base);
    if (!hero || want < 0) return false;
    int n = camp ? csCampSkillCount(base) : csComSkillCount(base);
    if (want >= n) return false;
    int cap = camp ? CAMP_SEL_MAX : csComMaxSelected(base);
    if (slot < 1 || slot > cap) return false;

    int cur[CS_COMSKILL_MAX], tgt[CS_COMSKILL_MAX];
    int L = camp ? csCampSelList(base, cur) : csComSlotList(base, cur);
    if (L > cap) return false;
    for (int s = 0; s < L; s++) if (cur[s] < 0 || cur[s] >= n) return false;   // torn entry: refuse

    int p = -1;                                            // where `want` is now, 0-based
    for (int s = 0; s < L; s++) if (cur[s] == want) p = s;
    int at = slot - 1;                                     // where it should go, 0-based
    int T = L;
    for (int s = 0; s < L; s++) tgt[s] = cur[s];

    if (p >= 0) {
        if (at >= L) at = L - 1;
        if (at == p) { res->landed = p + 1; return true; } // already there: nothing to do, and say so
        tgt[at] = want; tgt[p] = cur[at];                  // swap
        res->movedIdx = cur[at]; res->movedTo = p + 1;
    } else if (at < L) {
        tgt[at] = want;                                    // replace the occupant
        res->displaced = cur[at];
    } else if (L < cap) {
        at = L;                                            // append: the first free slot
        tgt[T++] = want;
    } else {
        return false;                                      // full and the slot is past the end: unreachable
    }

    int K = 0;                                             // first position where the plan differs
    while (K < L && K < T && cur[K] == tgt[K]) K++;
    logLine("charsheet slot: assign skill %d -> slot %d (len %d -> %d, rebuild from %d)",
            want, slot, L, T, K + 1);
    for (int s = L - 1; s >= K; s--)                       // erase the tail, back to front
        if (!csSehToggleSkill(base, hero, cur[s], camp))
            logLine("charsheet slot: ⚠ erase of skill %d refused", cur[s]);
    for (int s = K; s < T; s++)                            // push the new tail
        if (!csSehToggleSkill(base, hero, tgt[s], camp))
            logLine("charsheet slot: ⚠ push of skill %d refused", tgt[s]);

    // Read the outcome back out of the vector rather than trusting the plan.
    res->landed = camp ? csCampSkillSlot(base, want) : csComSkillSlot(base, want);
    if (res->movedIdx >= 0) res->movedTo = camp ? csCampSkillSlot(base, res->movedIdx) : csComSkillSlot(base, res->movedIdx);
    if (res->displaced >= 0 && (camp ? csCampSkillSelected(base, res->displaced)
                                     : csComSkillEquipped(base, res->displaced))) res->displaced = -1;
    int after[CS_COMSKILL_MAX];
    int A = camp ? csCampSelList(base, after) : csComSlotList(base, after);
    bool ok = (A == T);
    for (int s = 0; ok && s < A; s++) if (after[s] != tgt[s]) ok = false;
    if (!ok) logLine("charsheet slot: ⚠ vector does not match the plan (len %d, wanted %d)", A, T);
    return true;
}

// ---- the section table ----

// ------------------------------------------------------------
// ------------------------------------------------------------

static int csEquipCount(uintptr_t base) { (void)base; return EQUIP_SLOT_COUNT; }

static uintptr_t csEquipRecordFor(uintptr_t hero, int i) {
    if (i < 0 || i >= EQUIP_SLOT_COUNT) return 0;
    if (!hero) return 0;
    uintptr_t cls = 0;
    if (!safeReadPtr(hero + ACTOR_HEROCLASS_OFF, &cls) || cls <= 0x10000) return 0;

    uintptr_t vecOff   = (i == 0) ? HEROCLASS_WEAPON_VEC   : HEROCLASS_ARMOUR_VEC;
    uintptr_t stride   = (i == 0) ? EQUIP_WEAPON_STRIDE    : EQUIP_ARMOUR_STRIDE;
    uintptr_t levelOff = (i == 0) ? HERO_WEAPON_LEVEL_OFF  : HERO_ARMOUR_LEVEL_OFF;

    uintptr_t arr = 0;
    if (!safeReadPtr(cls + vecOff, &arr) || arr <= 0x10000) return 0;
    uint32_t level = 0;
    if (!safeReadU32(hero + levelOff, &level)) return 0;
    if ((int32_t)level < 0 || (int32_t)level > 16) {   // five tiers ship; 16 is a generous cap
        logLine("charsheet equip: slot %d implausible level %d — refusing", i, (int)level);
        return 0;
    }
    return arr + (uintptr_t)level * stride;
}

uintptr_t csEquipRecord(uintptr_t base, int i) {
    return csEquipRecordFor(abSelectedHero(base), i);
}

static int csEquipTierFor(uintptr_t hero, int i) {
    if (i < 0 || i >= EQUIP_SLOT_COUNT) return -1;
    if (!hero) return -1;
    uint32_t lvl = 0;
    uintptr_t off = (i == 0) ? HERO_WEAPON_LEVEL_OFF : HERO_ARMOUR_LEVEL_OFF;
    if (!safeReadU32(hero + off, &lvl)) return -1;
    if ((int32_t)lvl < 0 || (int32_t)lvl > 16) return -1;
    return (int)lvl;
}

// One stat line, using the game's OWN localized label key. Appended to `out`.
void csEquipStat(uintptr_t base, char* out, int outsz, const char* key, const char* value) {
    char label[128];
    if (!resolveKey(base, key, label, sizeof label)) {
        logLine("charsheet equip: label \"%s\" did not resolve — omitting", key);
        return;                       // omit, never fake: the health/stress rule
    }
    abStripMarkup(label);
    csTrimLabel(label);
    size_t n = strlen(out);
    _snprintf(out + n, outsz - (int)n, " %s: %s.", label, value);
    out[outsz - 1] = 0;
}

bool csEquipRowFor(uintptr_t base, uintptr_t hero, int i, char* out, int outsz) {
    uintptr_t rec = csEquipRecordFor(hero, i);
    if (!rec) return false;

    // The record's own discriminator, not our loop counter.
    uint32_t slot = 0;
    if (!safeReadU32(rec + EQUIP_SLOT_OFF, &slot)) return false;
    const char* what = (slot == 0) ? axs(AXS_CS_WEAPON) : (slot == 1) ? axs(AXS_CS_ARMOUR) : nullptr;
    if (!what) {
        logLine("charsheet equip: slot field reads %u (expected 0 or 1) — refusing", slot);
        return false;
    }

    char raw[128] = { 0 }, name[256] = { 0 };
    if (!safeReadCStr(rec + EQUIP_NAME_OFF, raw, sizeof raw) || !raw[0]) {
        logLine("charsheet equip: slot %u name string unreadable", slot);
        return false;
    }
    if (!resolveKey(base, raw, name, sizeof name) || !name[0]) {
        _snprintf(name, sizeof name, "%s", raw);
        name[sizeof name - 1] = 0;
    }
    abStripMarkup(name);

    _snprintf(out, outsz, "%s: %s.", what, name);
    out[outsz - 1] = 0;

    int tier = csEquipTierFor(hero, i);
    if (tier > 0) {
        size_t n = strlen(out);
        out[n] = ' ';
        _snprintf(out + n + 1, outsz - (int)n - 1, axs(AXS_CS_TIER_FMT), tier);
        out[outsz - 1] = 0;
    }

    char v[96];
    if (slot == 0) {
        uint32_t lo = 0, hi = 0, spd = 0, critBits = 0;
        if (safeReadU32(rec + EQUIP_W_DMG_LO_OFF, &lo) &&
            safeReadU32(rec + EQUIP_W_DMG_HI_OFF, &hi)) {
            _snprintf(v, sizeof v, "%d - %d", (int)lo, (int)hi);
            csEquipStat(base, out, outsz, "str_stat_base_damage", v);
        }
        if (safeReadU32(rec + EQUIP_W_CRIT_OFF, &critBits)) {
            float crit; memcpy(&crit, &critBits, sizeof crit);
            _snprintf(v, sizeof v, "%.1f%%", crit * CS_PERCENT_SCALE);
            csEquipStat(base, out, outsz, "str_stat_base_crit", v);
        }
        if (safeReadU32(rec + EQUIP_W_SPD_OFF, &spd)) {
            float s; memcpy(&s, &spd, sizeof s);
            _snprintf(v, sizeof v, "%.0f", s);
            csEquipStat(base, out, outsz, "str_stat_base_speed", v);
        }
    } else {
        uint32_t defBits = 0, hpBits = 0;
        if (safeReadU32(rec + EQUIP_A_DEF_OFF, &defBits)) {
            float def; memcpy(&def, &defBits, sizeof def);
            _snprintf(v, sizeof v, "%.0f%%", def * CS_PERCENT_SCALE);
            csEquipStat(base, out, outsz, "str_stat_base_defense", v);
        }
        if (safeReadU32(rec + EQUIP_A_HP_OFF, &hpBits)) {
            float hp; memcpy(&hp, &hpBits, sizeof hp);
            _snprintf(v, sizeof v, "%.0f", hp);
            csEquipStat(base, out, outsz, "str_stat_base_health_points", v);
        }
    }

    logLine("charsheet equip[%d] rec=%p slot=%u raw=\"%s\" -> \"%s\"",
            i, (void*)rec, slot, raw, out);
    return true;
}

bool csEquipRowText(uintptr_t base, int i, char* out, int outsz) {
    return csEquipRowFor(base, abSelectedHero(base), i, out, outsz);
}

// ------------------------------------------------------------
// ------------------------------------------------------------
uintptr_t bldTrinketRecord(uintptr_t base, uintptr_t item);
bool bldTrinketRarity(uintptr_t base, uintptr_t rec, char* out, int outsz, bool probe);
void bldTrinketClassReq(uintptr_t base, uintptr_t rec, char* out, int outsz, bool probe);
int  bldTrinketEffects(uintptr_t base, uintptr_t item, char* out, int outsz, bool probe);

static int csTrinketCount(uintptr_t base) { (void)base; return TRINKET_SLOT_COUNT; }

bool csTrinketRowText(uintptr_t base, int i, char* out, int outsz) {
    if (i < 0 || i >= TRINKET_SLOT_COUNT) return false;
    uintptr_t hero = abSelectedHero(base);
    if (!hero) return false;

    uintptr_t beg = 0; int slots = 0;
    if (!invItemVectorAt(hero + HERO_TRINKET_SYSTEM_OFF, &beg, &slots)) {
        logLine("charsheet trinket: hero item system unreadable");
        return false;
    }
    if (i >= slots) {
        logLine("charsheet trinket: slot %d beyond the hero's %d-slot system", i, slots);
        return false;
    }

    uintptr_t item = beg + (uintptr_t)i * ITEM_STRIDE;
    uint32_t amount = 0;
    if (!safeReadU32(item + ITEM_AMOUNT_OFF, &amount)) return false;

    bool town = csTownSheetPanel(base) != 0;
    if ((int32_t)amount < 1) {                     // the game's own empty test
        _snprintf(out, outsz, axs(AXS_CS_TRK_EMPTY_FMT), i + 1, TRINKET_SLOT_COUNT);
        out[outsz - 1] = 0;
        logLine("charsheet trinket[%d] empty", i);
        return true;
    }
    (void)town;

    char type[64] = { 0 }, itemId[64] = { 0 }, key[192] = { 0 }, name[256] = { 0 };
    bool resolved = invItemName(base, item, type, itemId, key, name);

    char slotline[96];
    _snprintf(slotline, sizeof slotline, axs(AXS_CS_TRK_SLOT_FMT), i + 1, TRINKET_SLOT_COUNT);
    slotline[sizeof slotline - 1] = 0;
    _snprintf(out, outsz, "%s. %s", name, slotline);
    out[outsz - 1] = 0;

    logLine("charsheet trinket[%d] item=%p type=\"%s\" id=\"%s\" key=\"%s\" -> \"%s\" (%s)",
            i, (void*)item, type, itemId, key, out, resolved ? "resolved" : "fallback");
    return true;
}

int csTrinketTipLines(uintptr_t base, int i, char lines[AB_TIP_MAX_LINES][AB_TIP_LINE_SZ]) {
    for (int k = 0; k < AB_TIP_MAX_LINES; k++) lines[k][0] = 0;
    if (!csTrinketRowText(base, i, lines[0], AB_TIP_LINE_SZ)) return 0;
    uintptr_t hero = abSelectedHero(base);
    uintptr_t beg = 0; int slots = 0;
    if (!hero || !invItemVectorAt(hero + HERO_TRINKET_SYSTEM_OFF, &beg, &slots) || i >= slots) return 1;
    uintptr_t item = beg + (uintptr_t)i * ITEM_STRIDE;
    uint32_t amount = 0;
    if (!safeReadU32(item + ITEM_AMOUNT_OFF, &amount) || (int32_t)amount < 1) return 1;
    InvCard c;
    invCardRead(base, item, &c);
    c.desc[0] = 0;
    return invCardLines(&c, lines, 1, AB_TIP_MAX_LINES);
}

void csTrinketAppendHint(uintptr_t base, int i, char* out, int outsz) {
    uintptr_t hero = abSelectedHero(base);
    uintptr_t beg = 0; int slots = 0;
    if (!hero || i < 0 || !invItemVectorAt(hero + HERO_TRINKET_SYSTEM_OFF, &beg, &slots) || i >= slots) return;
    uint32_t amount = 0;
    if (!safeReadU32(beg + (uintptr_t)i * ITEM_STRIDE + ITEM_AMOUNT_OFF, &amount)) return;
    bool town = csTownSheetPanel(base) != 0;
    bool filled = (int32_t)amount >= 1;
    axAppendHint(out, outsz, filled ? (town ? AXS_CS_TRK_HINT_UNEQUIP : AXS_CS_TRK_HINT_REPLACE)
                                    : (town ? AXS_CS_TRK_HINT_TOWN    : AXS_CS_TRK_HINT_RAID));
}

// ------------------------------------------------------------
bool csItemsPanelRow(uintptr_t base, uintptr_t hero, int i, char* out, int outsz) {
    if (outsz > 0) out[0] = 0;
    if (!hero || outsz <= 0) return false;
    if (i == 0 || i == 1) return csEquipRowFor(base, hero, i, out, outsz);
    if (i < 2 || i >= 2 + TRINKET_SLOT_COUNT) return false;
    int t = i - 2;

    char label[64];
    _snprintf(label, sizeof label, axs(AXS_RV_ITEMS_TRK_LABEL_FMT), t + 1);
    label[sizeof label - 1] = 0;

    uintptr_t beg = 0; int slots = 0;
    if (!invItemVectorAt(hero + HERO_TRINKET_SYSTEM_OFF, &beg, &slots)) {
        logLine("itemspanel: hero=%p item system unreadable", (void*)hero);
        return false;
    }
    if (t >= slots) {
        logLine("itemspanel: slot %d beyond the hero's %d-slot system", t, slots);
        return false;
    }
    uintptr_t item = beg + (uintptr_t)t * ITEM_STRIDE;
    uint32_t amount = 0;
    if (!safeReadU32(item + ITEM_AMOUNT_OFF, &amount)) return false;

    if ((int32_t)amount < 1) {                     // the game's own empty test
        _snprintf(out, outsz, axs(AXS_RV_ITEMS_TRK_EMPTY_FMT), t + 1);
        out[outsz - 1] = 0;
        return true;
    }

    char type[64] = { 0 }, itemId[64] = { 0 }, key[192] = { 0 }, name[256] = { 0 };
    invItemName(base, item, type, itemId, key, name);

    char rarity[96] = { 0 }, clsreq[256] = { 0 }, charges[384] = { 0 }, trig[1536] = { 0 };
    uintptr_t rec = bldTrinketRecord(base, item);
    if (rec) {
        bldTrinketRarity(base, rec, rarity, sizeof rarity, false);
        bldTrinketClassReq(base, rec, clsreq, sizeof clsreq, false);
        bldTrinketCharges(base, item, rec, charges, sizeof charges, false);
        bldTrinketTriggers(base, rec, trig, sizeof trig, false);
    }
    char fx[768] = { 0 };
    bldTrinketEffects(base, item, fx, sizeof fx, false);

    _snprintf(out, outsz, "%s: %s%s%s%s%s%s%s%s%s%s%s.",
              label, name,
              rarity[0]  ? ", " : "", rarity,
              clsreq[0]  ? ", " : "", clsreq,
              charges[0] ? ". " : "", charges,
              fx[0]      ? ". " : "", fx,
              trig[0]    ? ". " : "", trig);
    out[outsz - 1] = 0;
    return true;
}

// ------------------------------------------------------------
static const int64_t CS_MANAGE_RENAME_ID  = 0x686e6562;   // 'hneb' — the rename pencil
static const int64_t CS_MANAGE_DISMISS_ID = 0x68646d62;   // 'hdmb' — the dismiss button
static const int64_t CS_MANAGE_OUTFIT_ID  = 0x70616c65;   // 'pale' — the outfit (palette) icon
uintptr_t feGetElementById(int64_t id);

// ---- THE OUTFIT BUTTON ----
static const uintptr_t CS_HERO_OUTFIT_OFF         = 0xb68;  // Hero+: int colour_variation
static const uintptr_t CS_CLASS_OUTFITS_BEGIN_OFF = 0x5e0;  // HeroClass+: char[0x200]* begin
static const uintptr_t CS_CLASS_OUTFITS_END_OFF   = 0x5e8;  // HeroClass+: end
static const uintptr_t CS_CLASS_OUTFIT_STRIDE     = 0x200;

struct CsOutfitDescs { const char* classId; AxStrId desc[4]; };   // desc[k] = folder letter 'A'+k
static const CsOutfitDescs kCsOutfits[] = {
    { "abomination",   { AXS_OUTFIT_ABOMINATION_A,   AXS_OUTFIT_ABOMINATION_B,   AXS_OUTFIT_ABOMINATION_C,   AXS_OUTFIT_ABOMINATION_D   } },
    { "antiquarian",   { AXS_OUTFIT_ANTIQUARIAN_A,   AXS_OUTFIT_ANTIQUARIAN_B,   AXS_OUTFIT_ANTIQUARIAN_C,   AXS_OUTFIT_ANTIQUARIAN_D   } },
    { "arbalest",      { AXS_OUTFIT_ARBALEST_A,      AXS_OUTFIT_ARBALEST_B,      AXS_OUTFIT_ARBALEST_C,      AXS_OUTFIT_ARBALEST_D      } },
    { "bounty_hunter", { AXS_OUTFIT_BOUNTY_HUNTER_A, AXS_OUTFIT_BOUNTY_HUNTER_B, AXS_OUTFIT_BOUNTY_HUNTER_C, AXS_OUTFIT_BOUNTY_HUNTER_D } },
    { "crusader",      { AXS_OUTFIT_CRUSADER_A,      AXS_OUTFIT_CRUSADER_B,      AXS_OUTFIT_CRUSADER_C,      AXS_OUTFIT_CRUSADER_D      } },
    { "flagellant",    { AXS_OUTFIT_FLAGELLANT_A,    AXS_OUTFIT_FLAGELLANT_B,    AXS_OUTFIT_FLAGELLANT_C,    AXS_OUTFIT_FLAGELLANT_D    } },
    { "grave_robber",  { AXS_OUTFIT_GRAVE_ROBBER_A,  AXS_OUTFIT_GRAVE_ROBBER_B,  AXS_OUTFIT_GRAVE_ROBBER_C,  AXS_OUTFIT_GRAVE_ROBBER_D  } },
    { "hellion",       { AXS_OUTFIT_HELLION_A,       AXS_OUTFIT_HELLION_B,       AXS_OUTFIT_HELLION_C,       AXS_OUTFIT_HELLION_D       } },
    { "highwayman",    { AXS_OUTFIT_HIGHWAYMAN_A,    AXS_OUTFIT_HIGHWAYMAN_B,    AXS_OUTFIT_HIGHWAYMAN_C,    AXS_OUTFIT_HIGHWAYMAN_D    } },
    { "houndmaster",   { AXS_OUTFIT_HOUNDMASTER_A,   AXS_OUTFIT_HOUNDMASTER_B,   AXS_OUTFIT_HOUNDMASTER_C,   AXS_OUTFIT_HOUNDMASTER_D   } },
    { "jester",        { AXS_OUTFIT_JESTER_A,        AXS_OUTFIT_JESTER_B,        AXS_OUTFIT_JESTER_C,        AXS_OUTFIT_JESTER_D        } },
    { "leper",         { AXS_OUTFIT_LEPER_A,         AXS_OUTFIT_LEPER_B,         AXS_OUTFIT_LEPER_C,         AXS_OUTFIT_LEPER_D         } },
    { "man_at_arms",   { AXS_OUTFIT_MAN_AT_ARMS_A,   AXS_OUTFIT_MAN_AT_ARMS_B,   AXS_OUTFIT_MAN_AT_ARMS_C,   AXS_OUTFIT_MAN_AT_ARMS_D   } },
    { "musketeer",     { AXS_OUTFIT_MUSKETEER_A,     AXS_OUTFIT_MUSKETEER_B,     AXS_OUTFIT_MUSKETEER_C,     AXS_OUTFIT_MUSKETEER_D     } },
    { "occultist",     { AXS_OUTFIT_OCCULTIST_A,     AXS_OUTFIT_OCCULTIST_B,     AXS_OUTFIT_OCCULTIST_C,     AXS_OUTFIT_OCCULTIST_D     } },
    { "plague_doctor", { AXS_OUTFIT_PLAGUE_DOCTOR_A, AXS_OUTFIT_PLAGUE_DOCTOR_B, AXS_OUTFIT_PLAGUE_DOCTOR_C, AXS_OUTFIT_PLAGUE_DOCTOR_D } },
    { "shieldbreaker", { AXS_OUTFIT_SHIELDBREAKER_A, AXS_OUTFIT_SHIELDBREAKER_B, AXS_OUTFIT_SHIELDBREAKER_C, AXS_OUTFIT_SHIELDBREAKER_D } },
    { "vestal",        { AXS_OUTFIT_VESTAL_A,        AXS_OUTFIT_VESTAL_B,        AXS_OUTFIT_VESTAL_C,        AXS_OUTFIT_VESTAL_D        } },
};

static bool csOutfitRead(uintptr_t hero, int* idx, int* count, const char** desc) {
    *desc = "";
    uint32_t cv = 0;
    uintptr_t cls = 0, beg = 0, end = 0;
    if (!hero || !safeReadU32(hero + CS_HERO_OUTFIT_OFF, &cv)) return false;
    if (!safeReadPtr(hero + ACTOR_HEROCLASS_OFF, &cls) || cls <= 0x10000) return false;
    if (!safeReadPtr(cls + CS_CLASS_OUTFITS_BEGIN_OFF, &beg) ||
        !safeReadPtr(cls + CS_CLASS_OUTFITS_END_OFF, &end) || !beg || end <= beg) return false;
    uintptr_t n = (end - beg) / CS_CLASS_OUTFIT_STRIDE;
    if (n < 1 || n > 16 || (int32_t)cv < 0 || cv >= n) {
        logLine("charsheet outfit: implausible read cv=%d count=%llu", (int)cv, (unsigned long long)n);
        return false;
    }
    *idx = (int)cv;
    *count = (int)n;

    char path[CS_CLASS_OUTFIT_STRIDE], clsId[64];
    if (!safeReadCStr(beg + cv * CS_CLASS_OUTFIT_STRIDE, path, sizeof path)) path[0] = 0;
    if (!safeReadCStr(cls + HEROCLASS_ID_OFF, clsId, sizeof clsId)) clsId[0] = 0;
    int len = (int)strlen(path);
    while (len > 0 && (path[len - 1] == '/' || path[len - 1] == '\\')) path[--len] = 0;
    const char* us = strrchr(path, '_');
    char letter = (us && us[1] >= 'A' && us[1] <= 'Z' && us[2] == 0) ? us[1] : 0;
    if (!letter) {
        logLine("charsheet outfit: no folder letter in \"%s\" (class %s, cv %d)", path, clsId, (int)cv);
        return true;
    }
    for (const CsOutfitDescs& o : kCsOutfits) {
        if (strcmp(o.classId, clsId) != 0) continue;
        int k = letter - 'A';
        if (k >= 0 && k < 4) *desc = axs(o.desc[k]);
        break;
    }
    return true;
}

static void csOutfitRowName(uintptr_t base, const char* tooltip, char* out, int outsz) {
    int idx = 0, count = 0; const char* desc = "";
    if (!csOutfitRead(abSelectedHero(base), &idx, &count, &desc)) {
        strncpy(out, tooltip, outsz - 1); out[outsz - 1] = 0;
        return;
    }
    if (desc[0]) _snprintf(out, outsz, axs(AXS_CS_OUTFIT_ROW_FMT), tooltip, idx + 1, count, desc);
    else         _snprintf(out, outsz, axs(AXS_CS_OUTFIT_ROW_BARE_FMT), tooltip, idx + 1, count);
    out[outsz - 1] = 0;
}

static uintptr_t g_csOutfitWatchHero  = 0;
static int       g_csOutfitWatchFrom  = -1;
static DWORD     g_csOutfitWatchUntil = 0;
static bool      g_csOutfitWatchSaw   = false;

static void csOutfitArm(uintptr_t base) {
    uintptr_t hero = abSelectedHero(base);
    int idx = -1, count = 0; const char* desc = "";
    if (hero != g_csOutfitWatchHero || !g_csOutfitWatchUntil) {
        g_csOutfitWatchHero = hero;
        g_csOutfitWatchSaw  = false;
        g_csOutfitWatchFrom = csOutfitRead(hero, &idx, &count, &desc) ? idx : -1;
    }
    g_csOutfitWatchUntil = GetTickCount() + 1500;
}

static void csOutfitService(uintptr_t base) {
    if (!g_csOutfitWatchUntil) return;
    uintptr_t hero = abSelectedHero(base);
    if (hero != g_csOutfitWatchHero) { g_csOutfitWatchUntil = 0; return; }   // swapped away
    int idx = -1, count = 0; const char* desc = "";
    if (csOutfitRead(hero, &idx, &count, &desc) && idx != g_csOutfitWatchFrom) {
        g_csOutfitWatchFrom = idx;
        g_csOutfitWatchSaw  = true;
        char line[320];
        if (desc[0]) _snprintf(line, sizeof line, axs(AXS_CS_OUTFIT_NOW_FMT), idx + 1, count, desc);
        else         _snprintf(line, sizeof line, axs(AXS_CS_OUTFIT_NOW_BARE_FMT), idx + 1, count);
        line[sizeof line - 1] = 0;
        logLine("charsheet outfit: now %d of %d", idx + 1, count);
        postSpeech(line);
        return;
    }
    if (GetTickCount() > g_csOutfitWatchUntil) {
        g_csOutfitWatchUntil = 0;
        if (!g_csOutfitWatchSaw) {
            logLine("charsheet outfit: click produced no change in 1.5 s");
            postSpeech(axs(AXS_CS_DIDNT_CHANGE));
        }
    }
}

static int64_t g_csManageIds[3];
static char    g_csManageNames[3][256];
static int     g_csManageCnt = 0;

static int csManageCollect(uintptr_t base) {
    g_csManageCnt = 0;
    bool town = csTownSheetPanel(base) != 0;
    if (!town && !csOpenPanel(base)) return 0;     // no sheet: no Manage section
    static const struct { int64_t id; const char* key; const char* fallback; bool townOnly; } kBtns[3] = {
        { CS_MANAGE_RENAME_ID,  "character_panel_rename_tt",     "Rename",        true  },
        { CS_MANAGE_DISMISS_ID, "character_panel_dismiss_tt",    "Dismiss",       true  },
        { CS_MANAGE_OUTFIT_ID,  "character_panel_change_colour", "Switch Outfit", false },
    };
    for (int i = 0; i < 3; i++) {
        if (kBtns[i].townOnly && !town) continue;
        if (!feGetElementById(kBtns[i].id)) continue;
        g_csManageIds[g_csManageCnt] = kBtns[i].id;
        char* name = g_csManageNames[g_csManageCnt];
        if (!resolveKey(base, kBtns[i].key, name, sizeof g_csManageNames[0])) {
            strncpy(name, kBtns[i].fallback, sizeof g_csManageNames[0] - 1);
            name[sizeof g_csManageNames[0] - 1] = 0;
        }
        abStripMarkup(name);
        if (kBtns[i].id == CS_MANAGE_OUTFIT_ID) {
            char tip[96];
            strncpy(tip, name, sizeof tip - 1); tip[sizeof tip - 1] = 0;
            csOutfitRowName(base, tip, name, sizeof g_csManageNames[0]);
        }
        g_csManageCnt++;
    }
    return g_csManageCnt;
}

static int csManageCount(uintptr_t base) { return csManageCollect(base); }

static bool csManageRowText(uintptr_t base, int i, char* out, int outsz) {
    int n = csManageCollect(base);
    if (i < 0 || i >= n) return false;
    _snprintf(out, outsz, axs(AXS_CS_BUTTON_FMT), g_csManageNames[i]);
    out[outsz - 1] = 0;
    return true;
}

typedef int  (*CsCountFn)(uintptr_t base);
typedef bool (*CsRowFn)(uintptr_t base, int i, char* out, int outsz);
typedef int  (*CsTipFn)(uintptr_t base, int i, char lines[AB_TIP_MAX_LINES][AB_TIP_LINE_SZ]);

struct CsSectionDef {
    const char* titleKey;
                            // = section 0, the title line
    CsCountFn   count;
    CsRowFn     row;
    CsTipFn     tip;
    const char* name;
    bool        hideWhenEmpty;
    int         modTitle = -1;
                               // words of its own (the two quirk columns). -1 = use titleKey.
};

static int  csTitleCount(uintptr_t base) { (void)base; return 1; }
static bool csTitleRowText(uintptr_t base, int i, char* out, int outsz) {
    if (i != 0) return false;
    return csTitleLine(base, out, outsz);
}

static const CsSectionDef kCsSections[] = {
    { nullptr,                         csTitleCount,    csTitleRowText,    nullptr,             "hero"          },
    { nullptr, csQuirkPosSectionCount, csQuirkPosRowText, csQuirkPosTipLines, "positive quirks", false, AXS_CS_SECTION_POS_QUIRKS },
    { nullptr, csQuirkNegSectionCount, csQuirkNegRowText, csQuirkNegTipLines, "negative quirks", false, AXS_CS_SECTION_NEG_QUIRKS },
    { "character_title_base_stats",    csStatCount,     csStatRowText,     csStatTipLines,      "base stats"    },
    { "character_title_equipment",     csEquipCount,    csEquipRowText,    nullptr,             "equipment"     },
    { "character_title_trinkets",      csTrinketCount,  csTrinketRowText,  csTrinketTipLines,   "trinkets"      },
    { "character_title_combat_skills", csComSectionCount, csComSectionRow, csComSectionTip,  "combat skills" },
    { "character_title_camping_skills", csCampSkillCount, csCampSkillRowText, csCampSkillTipLines, "camping skills" },
    { "character_title_resistances",   csResistCount,   csResistRowText,   csResistTipLines,    "resistances"   },
    { "str_diseases",                  csDiseaseSectionCount, csDiseaseRowText, csDiseaseTipLines, "diseases"   },
    { "str_manage_section",            csManageCount,   csManageRowText,   nullptr,             "Manage", true  },
};
static const int kCsSectionCount = (int)(sizeof kCsSections / sizeof kCsSections[0]);

static void csAppendPos(char* out, size_t outsz, int idx, int n) {
    size_t len = strlen(out);
    while (len && out[len - 1] == ' ') out[--len] = 0;
    if (len && out[len - 1] != '.' && out[len - 1] != '!' && out[len - 1] != '?' &&
        len < outsz - 1) { out[len++] = '.'; out[len] = 0; }
    if (len && len < outsz - 1)  { out[len++] = ' '; out[len] = 0; }
    _snprintf(out + len, outsz - len, axs(AXS_POS_N_OF_M), idx + 1, n);
    out[outsz - 1] = 0;
}

static void csSectionHeading(uintptr_t base, const CsSectionDef* s, char* title, int titlesz) {
    if (s->modTitle >= 0) {          // the mod's own heading (the two quirk columns)
        strncpy(title, axs((AxStrId)s->modTitle), titlesz - 1);
        title[titlesz - 1] = 0;
    } else if (!resolveKey(base, s->titleKey, title, titlesz)) {
        logLine("charsheet section: \"%s\" did not resolve", s->titleKey);
        strncpy(title, s->name, titlesz - 1);   // the log name is English, but it is not silence
        title[titlesz - 1] = 0;
    }
    abStripMarkup(title);
    csTrimLabel(title);
}

static void csGridSkillTitle(uintptr_t base, bool camp, int i, char* out, int outsz) {
    char fallback[64];
    if (camp) {
        uintptr_t cls = csCampSkillClass(base, i);
        if (cls && csCampSkillName(base, cls, out, outsz)) return;
        _snprintf(out, outsz, axs(AXS_CS_CAMPSKILL_N_FMT), i + 1);
        out[outsz - 1] = 0;
        return;
    }
    _snprintf(fallback, sizeof fallback, axs(AXS_CS_SKILL_N_FMT), i + 1);
    fallback[sizeof fallback - 1] = 0;
    uintptr_t skill = csComSkillAt(base, i);
    if (skill) abSkillTitled(base, skill, fallback, out, outsz, false);
    else       { strncpy(out, fallback, outsz - 1); out[outsz - 1] = 0; }
}

static void csRowTypeWords(uintptr_t base, const CsSectionDef* s, int item, char* out, int outsz) {
    out[0] = 0;
    if (axKeywordsMode() == AX_KEYWORDS_OFF) return;
    if (!s->titleKey && s->modTitle < 0) return;
    char title[128];
    csSectionHeading(base, s, title, sizeof title);
    char word[128]; word[0] = 0;
    if (s->row == csResistRowText) {
        if (csResistName(base, item, word, sizeof word)) {
            csTrimLabel(word);
            _snprintf(out, outsz, "%s %s", word, title);     // name first: the own term wins
            out[outsz - 1] = 0;
            return;
        }
    }
    _snprintf(out, outsz, "%s", title);
    out[outsz - 1] = 0;
    if (s->row == csStatRowText) {
        if (!csStatName(base, item, word, sizeof word)) word[0] = 0;
    } else if (s->row == csEquipRowText) {
        uintptr_t rec = csEquipRecordFor(abSelectedHero(base), item);
        uint32_t slot = 0;
        if (rec && safeReadU32(rec + EQUIP_SLOT_OFF, &slot) && slot <= 1)
            axKeywordTerm(slot == 0 ? KW_TERM_WEAPON : KW_TERM_ARMOR, word, sizeof word);
    }
    if (word[0]) {
        size_t n = strlen(out);
        _snprintf(out + n, outsz - (int)n, ". %s", word);
        out[outsz - 1] = 0;
    }
}

static void csAppendTip(uintptr_t base, const CsSectionDef* s, int item, char* out, size_t outsz) {
    if (!axTipWanted()) return;
    char type[320];
    csRowTypeWords(base, s, item, type, sizeof type);
    axTipNoteKeywords(type);
    if (s->tip) {
        char lines[AB_TIP_MAX_LINES][AB_TIP_LINE_SZ];
        int n = s->tip(base, item, lines);
        axTipGlueLines(out, (int)outsz, &lines[0][0], AB_TIP_LINE_SZ, n, 1);
    }
    axTipGlueKeywords(out, (int)outsz);
}

static void csAppendRowHint(uintptr_t base, const CsSectionDef* s, int item, char* out, size_t outsz) {
    bool camp;
    if      (s->row == csComSectionRow)   camp = false;
    else if (s->row == csCampSkillRowText) camp = true;
    else if (s->row == csManageRowText) { axAppendHint(out, (int)outsz, AXS_ENTER_ACTIVATE_HINT); return; }
    else if (s->row == csTrinketRowText) { csTrinketAppendHint(base, item, out, (int)outsz); return; }
    else return;
    int i = item;
    if (!camp) {
        if (item < CS_COM_PIP_ITEMS) return;
        i = csComColToSkill(base, item - CS_COM_PIP_ITEMS);
    }
    if (i < 0) return;
    if (!(camp ? csCampSkillLearned(base, i) : csComSkillLearned(base, i))) return;
    bool in = camp ? csCampSkillSelected(base, i) : csComSkillEquipped(base, i);
    const char* fmt = axs(in ? AXS_CS_SKILL_DESELECT_HINT_FMT : AXS_CS_SKILL_SELECT_HINT_FMT);
    if (!fmt[0]) return;                                 // "" while Control hints is Off
    char hint[160];
    _snprintf(hint, sizeof hint, fmt, camp ? CAMP_SEL_MAX : csComMaxSelected(base));
    hint[sizeof hint - 1] = 0;
    axAppendHintText(out, (int)outsz, hint);
}
static void csSpeakSection(uintptr_t base) {
    if (g_csSection < 0 || g_csSection >= kCsSectionCount) g_csSection = 0;
    const CsSectionDef* s = &kCsSections[g_csSection];
    g_csRow = -1;
    g_csTipLine = 0;
    g_csTrkConfirmSlot = -1;

    if (!s->titleKey && s->modTitle < 0) {   // row 0: the title line IS the announcement
        csSpeakTitle(base, "section");
        return;
    }
    char title[128];
    csSectionHeading(base, s, title, sizeof title);

    char out[MAILBOX_SZ];
    _snprintf(out, sizeof out, "%s.", title);
    out[sizeof out - 1] = 0;

    int n = s->count(base);
    if (n > 0) {
        char item[MAILBOX_SZ];
        if (s->row(base, 0, item, sizeof item)) {
            csAppendTip(base, s, 0, item, sizeof item);
            csAppendPos(item, sizeof item, 0, n);
            csAppendRowHint(base, s, 0, item, sizeof item);   // the landing item's control hint
            size_t len = strlen(out);
            if (len && len < sizeof out - 1) { out[len++] = ' '; out[len] = 0; }
            _snprintf(out + len, sizeof out - len, "%s", item);
            out[sizeof out - 1] = 0;
            g_csRow = 0;
        } else {
            logLine("charsheet: section %d (%s) item 0 did not read on landing", g_csSection, s->name);
        }
    }
    logLine("charsheet section %d (%s) -> \"%s\"", g_csSection, s->name, out);
    postSpeech(out);
}

static int csTrinketSectionIdx() {
    for (int i = 0; i < kCsSectionCount; i++)
        if (kCsSections[i].row == csTrinketRowText) return i;
    return -1;
}

bool csFocusTrinketSection(uintptr_t base) {
    int i = csTrinketSectionIdx();
    if (i < 0 || kCsSections[i].count(base) <= 0) return false;
    g_csSection = i;
    g_csRow     = 0;
    g_csTipLine = 0;
    return true;
}

bool csFocusTrinketSlotWith(uintptr_t base, int slot, const char* prefix) {   // exported (internal.h)
    int sec = csTrinketSectionIdx();
    if (sec < 0) return false;
    int n = kCsSections[sec].count(base);
    if (n <= 0) return false;
    if (slot < 0)  slot = 0;
    if (slot >= n) slot = n - 1;

    char row[MAILBOX_SZ];
    if (!kCsSections[sec].row(base, slot, row, sizeof row)) {
        logLine("charsheet: trinket slot %d would not read on the hand-back", slot + 1);
        return false;
    }
    csAppendPos(row, sizeof row, slot, n);

    g_csSection        = sec;
    g_csRow            = slot;
    g_csTipLine        = 0;
    g_csTrkConfirmSlot = -1;             // a fresh landing arms no unequip confirm

    char out[MAILBOX_SZ];
    if (prefix && *prefix) _snprintf(out, sizeof out, "%s%s", prefix, row);
    else                   _snprintf(out, sizeof out, "%s", row);
    out[sizeof out - 1] = 0;
    logLine("charsheet: handed back onto trinket slot %d -> \"%s\"", slot + 1, out);
    postSpeech(out);
    return true;
}

static void csMoveSection(uintptr_t base, int dir) {
    for (int step = g_csSection + dir; step >= 0 && step < kCsSectionCount; step += dir) {
        const CsSectionDef* s = &kCsSections[step];
        if (s->hideWhenEmpty && s->count(base) <= 0) continue;
        g_csSection = step;
        break;
    }
    csSpeakSection(base);
}

static void csMoveRow(uintptr_t base, int step) {
    if (g_csSection < 0 || g_csSection >= kCsSectionCount) g_csSection = 0;
    const CsSectionDef* s = &kCsSections[g_csSection];
    int n = s->count(base);
    if (n <= 0) {
        logLine("charsheet: section %d (%s) has no rows", g_csSection, s->name);
        csSpeakSection(base);
        return;
    }

    int target;
    if (g_csRow < 0) {
        target = 0;
    } else {
        target = g_csRow + step;
        if (target < 0)  target = 0;      // hard stop at the start ...
        if (target >= n) target = n - 1;  // ... and at the end
    }

    char out[MAILBOX_SZ];
    if (!s->row(base, target, out, sizeof out)) {
        logLine("charsheet: section %d (%s) row %d did not read", g_csSection, s->name, target);
        return;
    }
    csAppendTip(base, s, target, out, sizeof out);
    csAppendPos(out, sizeof out, target, n);
    csAppendRowHint(base, s, target, out, sizeof out);   // the item's control hint, after the count

    logLine("charsheet row %d -> %d (of %d) -> \"%s\"", g_csRow, target, n, out);
    if (target != g_csRow) g_csTrkConfirmSlot = -1;   // moving disarms the unequip confirm
    g_csRow = target;
    g_csTipLine = 0;
    postSpeech(out);
}

// ---- THE CHARACTER SHEET'S FIND ENUMERATOR ----
static bool csFindLocate(uintptr_t base, int idx, int* secOut, int* itemOut, int* countOut) {
    int at = 0;
    for (int s = 0; s < kCsSectionCount; s++) {
        const CsSectionDef* d = &kCsSections[s];
        int n = d->count(base);
        if (d->hideWhenEmpty && n <= 0) continue;   // Up/Down skip it: not an element
        int here = n > 0 ? n : 1;                   // an empty row is one element, its heading
        if (idx < at + here) {
            *secOut = s; *itemOut = idx - at; *countOut = n;
            return true;
        }
        at += here;
    }
    return false;
}

int csFind(uintptr_t base, AxFindOp op, int idx, char* out, int outsz) {
    if (!g_csOpen) return 0;
    if (op == AXF_COUNT) {
        int total = 0;
        for (int s = 0; s < kCsSectionCount; s++) {
            const CsSectionDef* d = &kCsSections[s];
            int n = d->count(base);
            if (d->hideWhenEmpty && n <= 0) continue;
            total += n > 0 ? n : 1;
        }
        return total;
    }
    int sec = 0, item = 0, n = 0;
    if (idx < 0 || !csFindLocate(base, idx, &sec, &item, &n)) return 0;
    const CsSectionDef* s = &kCsSections[sec];
    if (op == AXF_LABEL) {
        if (!out || outsz <= 0) return 0;
        out[0] = 0;
        if (!s->titleKey && s->modTitle < 0) {      // row 0: the title line is the whole element
            return csTitleLine(base, out, outsz) ? 1 : 0;
        }
        if (n <= 0 || item == 0) {                   // the landing wording: heading, then item 1
            char title[128];
            csSectionHeading(base, s, title, sizeof title);
            _snprintf(out, outsz, "%s.", title);
            out[outsz - 1] = 0;
            if (n <= 0) return 1;
            char first[MAILBOX_SZ];
            if (s->row(base, 0, first, sizeof first)) {
                size_t len = strlen(out);
                if (len < (size_t)outsz - 1) _snprintf(out + len, outsz - len, " %s", first);
                out[outsz - 1] = 0;
            }
            return 1;
        }
        return s->row(base, item, out, outsz) ? 1 : 0;   // a later item: the item alone
    }
    if (op == AXF_GO) {
        g_csSection = sec;
        if (n <= 0 || item == 0) {
            logLine("charsheet: find landed on row %d (%s), item 0", sec, s->name);
            csSpeakSection(base);                    // places the cursor on item 1 and speaks it
            return 1;
        }
        g_csRow = item;                              // csMoveRow(0) re-reads and clamps it live
        g_csTipLine = 0;
        g_csTrkConfirmSlot = -1;                     // a fresh landing arms no unequip confirm
        logLine("charsheet: find landed on row %d (%s), item %d", sec, s->name, item);
        csMoveRow(base, 0);
        return 1;
    }
    return 0;
}

static void csSpeakTipLine(uintptr_t base, int tipDir) {
    const CsSectionDef* s = &kCsSections[g_csSection];
    if (g_csRow < 0) { postSpeech(axs(AXS_NO_ROW_SELECTED)); return; }

    char lines[AB_TIP_MAX_LINES][AB_TIP_LINE_SZ];
    int n = s->tip ? s->tip(base, g_csRow, lines) : (s->row(base, g_csRow, lines[0], AB_TIP_LINE_SZ) ? 1 : 0);
    char type[320];
    csRowTypeWords(base, s, g_csRow, type, sizeof type);
    int total = n + ((n > 0) ? axTipKeywordCount(&lines[0][0], AB_TIP_LINE_SZ, n, 1, type) : 0);   // 1: line 0 is the row
    if (total <= 1) { postSpeech(axs(AXS_NO_MORE_INFO)); return; }

    const char* note = nullptr;
    int line = axTipWrapStep(g_csTipLine, tipDir, total, &note);
    g_csTipLine = line;
    char def[AB_TIP_LINE_SZ];
    const char* text = lines[0];
    if (line < n) text = lines[line];
    else if (axTipKeywordLine(&lines[0][0], AB_TIP_LINE_SZ, n, 1, type, line - n, def, sizeof def)) text = def;
    logLine("charsheet tipline %d/%d (%s row %d) -> \"%s\"", line, total, s->name, g_csRow, text);
    axPostWrapped(note, text);
}

bool routeCharSheetKey(uintptr_t base, uint32_t sym, uint16_t mod, uint8_t repeat) {
    if (mod & (KMOD_LALT | KMOD_RALT)) return false;
    bool ctrl = (mod & (KMOD_LCTRL | KMOD_RCTRL)) != 0;

    if (sym == SDLK_TAB) {
        if (ctrl) return false;          // Ctrl+Tab is not ours
        if (repeat) return true;
        csMoveSection(base, (mod & (KMOD_LSHIFT | KMOD_RSHIFT)) ? -1 : 1);
        return true;
    }

    if (sym == SDLK_RETURN || sym == SDLK_KP_ENTER) {
        if (ctrl) return false;
        if (g_csSection < 0 || g_csSection >= kCsSectionCount) return false;

        if (kCsSections[g_csSection].row == csComSectionRow) {
            if (repeat) return true;
            int n = csComSkillCount(base);
            if (n <= 0) return false;
            if (g_csRow < 0) { csMoveRow(base, 1); return true; }   // land on item 1 + read it
            if (g_csRow < CS_COM_PIP_ITEMS) {
                csMoveRow(base, 0);
                return true;
            }
            if (g_csSkillWatchUntil) { postSpeech(axs(AXS_STILL_WORKING)); return true; }
            int i = csComColToSkill(base, g_csRow - CS_COM_PIP_ITEMS);
            if (i < 0) return true;
            bool equipped = csComSkillEquipped(base, i);
            if (!equipped && !csComSkillLearned(base, i)) {
                if (!frontEndClickElementId(csComCellId(i)))
                    logLine("charsheet comskill: locked cell 0x%llx not on screen (skill %d)",
                            (unsigned long long)csComCellId(i), i);
                postSpeech(axs(AXS_CS_SKILL_LOCKED));
                return true;
            }
            int cap = csComMaxSelected(base);
            if (!equipped && csComSkillEquippedCount(base) >= cap) {
                char full[256], hint[128];
                _snprintf(full, sizeof full, axs(AXS_CS_COM_CAP_FMT), cap);
                full[sizeof full - 1] = 0;
                const char* hfmt = axs(AXS_CS_COM_CAP_HINT_FMT);
                if (hfmt[0]) {
                    _snprintf(hint, sizeof hint, hfmt, cap); hint[sizeof hint - 1] = 0;
                    axAppendHintText(full, sizeof full, hint);
                }
                postSpeech(full);
                return true;
            }
            char fallback[64], titled[320];
            _snprintf(fallback, sizeof fallback, axs(AXS_CS_SKILL_N_FMT), i + 1);
            uintptr_t skill = csComSkillAt(base, i);
            if (skill) abSkillTitled(base, skill, fallback, titled, sizeof titled, false);
            else       { strncpy(titled, fallback, sizeof titled - 1); titled[sizeof titled - 1] = 0; }
            int64_t elem = csComCellId(i);
            if (!frontEndClickElementId(elem)) {
                logLine("charsheet comskill: cell element 0x%llx not on screen (skill %d, item %d)",
                        (unsigned long long)elem, i, g_csRow);
                postSpeech(axs(AXS_CS_CANT_CHANGE_NOW));
                return true;
            }
            g_csSkillWatchIdx = i;
            g_csSkillWatchCamp = false;
            g_csSkillWantEquipped = !equipped;
            strncpy(g_csSkillWatchName, titled, sizeof g_csSkillWatchName - 1);
            g_csSkillWatchName[sizeof g_csSkillWatchName - 1] = 0;
            g_csSkillWatchUntil = GetTickCount() + 1500;   // serviceCharSheet announces the outcome
            logLine("charsheet comskill: toggle skill %d item %d \"%s\" want=%s (elem 0x%llx)",
                    i, g_csRow, titled, g_csSkillWantEquipped ? "equipped" : "unequipped",
                    (unsigned long long)elem);
            return true;
        }

        if (kCsSections[g_csSection].row == csCampSkillRowText) {
            if (repeat) return true;
            int n = csCampSkillCount(base);
            if (n <= 0) return false;
            if (g_csRow < 0) { csMoveRow(base, 1); return true; }   // land on row 1 + read it
            if (g_csRow >= n) g_csRow = n - 1;
            if (g_csSkillWatchUntil) { postSpeech(axs(AXS_STILL_WORKING)); return true; }
            int i = g_csRow;
            bool selected = csCampSkillSelected(base, i);
            if (!selected && !csCampSkillLearned(base, i)) {
                if (!frontEndClickElementId(csCampCellId(i)))
                    logLine("charsheet camskill: locked cell 0x%llx not on screen (skill %d)",
                            (unsigned long long)csCampCellId(i), i);
                postSpeech(axs(AXS_CS_CAMP_LOCKED));
                return true;
            }
            if (!selected) {
                int have = 0;
                for (int k = 0; k < n; k++) if (csCampSkillSelected(base, k)) have++;
                if (have >= CAMP_SEL_MAX) {
                    char full[160];
                    _snprintf(full, sizeof full, axs(AXS_CS_CAMP_FULL_FMT), CAMP_SEL_MAX);
                    full[sizeof full - 1] = 0;
                    postSpeech(full);
                    return true;
                }
            }
            char name[256];
            uintptr_t cls = csCampSkillClass(base, i);
            if (!cls || !csCampSkillName(base, cls, name, sizeof name))
                _snprintf(name, sizeof name, axs(AXS_CS_CAMPSKILL_N_FMT), i + 1);
            name[sizeof name - 1] = 0;
            csCampProbe(base, "before click");
            int64_t elem = csCampCellId(i);
            if (!frontEndClickElementId(elem)) {
                logLine("charsheet camskill: cell element 0x%llx not on screen (skill %d)",
                        (unsigned long long)elem, i);
                postSpeech(axs(AXS_CS_CANT_CHANGE_NOW));
                return true;
            }
            g_csSkillWatchIdx = i;
            g_csSkillWatchCamp = true;
            g_csSkillWantEquipped = !selected;
            strncpy(g_csSkillWatchName, name, sizeof g_csSkillWatchName - 1);
            g_csSkillWatchName[sizeof g_csSkillWatchName - 1] = 0;
            g_csSkillWatchUntil = GetTickCount() + 1500;   // serviceCharSheet announces the outcome
            logLine("charsheet camskill: toggle skill %d \"%s\" want=%s (elem 0x%llx)",
                    i, name, g_csSkillWantEquipped ? "selected" : "deselected",
                    (unsigned long long)elem);
            return true;
        }

        if (kCsSections[g_csSection].row == csTrinketRowText && !csTownSheetPanel(base)) {
            if (repeat) return true;
            uintptr_t hero = abSelectedHero(base);
            if (!hero) return false;
            if (g_csRow < 0) { csMoveRow(base, 1); return true; }    // land + read first
            return ruBeginFromSheet(base, hero);
        }

        if (kCsSections[g_csSection].row == csTrinketRowText && csTownSheetPanel(base)) {
            if (repeat) return true;
            uintptr_t hero = abSelectedHero(base);
            if (!hero) return false;
            if (g_csRow < 0) { csMoveRow(base, 1); return true; }    // land + read first
            if (g_csRow >= TRINKET_SLOT_COUNT) g_csRow = TRINKET_SLOT_COUNT - 1;
            if (g_csTrkWatchUntil) { postSpeech(axs(AXS_STILL_WORKING)); return true; }
            int slot = g_csRow;

            if (g_trkPhase == 3 && hero == g_trkHero) {
                trkCommitSlot(base, slot);
                return true;
            }

            uintptr_t beg = 0; int slots = 0;
            uint32_t amount = 0;
            if (invItemVectorAt(hero + HERO_TRINKET_SYSTEM_OFF, &beg, &slots) && slot < slots)
                safeReadU32(beg + (uintptr_t)slot * ITEM_STRIDE + ITEM_AMOUNT_OFF, &amount);
            if ((int32_t)amount > 0) {                              // FILLED: two-step unequip
                char type[64], itemId[64], key[192], name[256];
                invItemName(base, beg + (uintptr_t)slot * ITEM_STRIDE, type, itemId, key, name);
                if (g_csTrkConfirmSlot != slot) {                   // first press: ask
                    g_csTrkConfirmSlot = slot;
                    strncpy(g_csTrkConfirmName, name, sizeof g_csTrkConfirmName - 1);
                    g_csTrkConfirmName[sizeof g_csTrkConfirmName - 1] = 0;
                    char utter[320];
                    _snprintf(utter, sizeof utter, axs(AXS_CS_TRK_UNEQUIP_CONFIRM_FMT), name);
                    utter[sizeof utter - 1] = 0;
                    axAppendHint(utter, sizeof utter, AXS_CS_TRK_UNEQUIP_CONFIRM_HINT);   // "" when Off
                    postSpeech(utter);
                    return true;
                }
                g_csTrkConfirmSlot = -1;                            // second press: do it
                if (!csSehUnequipTrinket(base, hero, slot)) {
                    logLine("charsheet trinket: unequip body faulted (hero=%p slot=%d)",
                            (void*)hero, slot);
                    postSpeech(axs(AXS_DIDNT_HAPPEN));
                    return true;
                }
                strncpy(g_csTrkWatchName, g_csTrkConfirmName, sizeof g_csTrkWatchName - 1);
                g_csTrkWatchName[sizeof g_csTrkWatchName - 1] = 0;
                g_csTrkWatchSlot = slot;
                g_csTrkWatchUntil = GetTickCount() + 1500;  // serviceCharSheet pays the outcome
                return true;
            }
            g_csTrkConfirmSlot = -1;                                // EMPTY: arm the equip flow
            riRequestEquipFor(base, hero, slot);
            return true;
        }

        if (kCsSections[g_csSection].row != csManageRowText) return false;
        if (repeat) return true;
        int n = csManageCollect(base);
        if (n <= 0) return false;                     // the buttons vanished; not ours after all
        if (g_csRow < 0) { csMoveRow(base, 1); return true; }  // land on button 1, read it first
        if (g_csRow >= n) g_csRow = n - 1;
        if (frontEndClickElementId(g_csManageIds[g_csRow])) {
            logLine("charsheet manage: clicked \"%s\" (0x%llx)",
                    g_csManageNames[g_csRow], (unsigned long long)g_csManageIds[g_csRow]);
            if (g_csManageIds[g_csRow] == CS_MANAGE_RENAME_ID)
                g_trOpenWatchUntil = GetTickCount() + 1500;  // checkTownRename pays this watch
            if (g_csManageIds[g_csRow] == CS_MANAGE_OUTFIT_ID)
                csOutfitArm(base);                          // serviceCharSheet pays this watch
        } else {
            logLine("charsheet manage: \"%s\" did not click", g_csManageNames[g_csRow]);
            postSpeech(axs(AXS_NOT_AVAILABLE));
        }
        return true;
    }

    if (sym >= SDLK_1 && sym <= SDLK_4 && !ctrl) {
        if (g_csSection < 0 || g_csSection >= kCsSectionCount) return false;
        bool camp;
        if      (kCsSections[g_csSection].row == csComSectionRow)   camp = false;
        else if (kCsSections[g_csSection].row == csCampSkillRowText) camp = true;
        else return false;                                   // digits stay the game's elsewhere
        if (!camp && g_csRow < CS_COM_PIP_ITEMS) return false;   // a pip item (or unplaced): not ours
        if (camp && g_csRow < 0) return false;                   // unplaced on the camping row
        if (repeat) return true;
        int slot = (int)(sym - SDLK_1) + 1;
        if (g_csSkillWatchUntil) { postSpeech(axs(AXS_STILL_WORKING)); return true; }
        int i = camp ? g_csRow : csComColToSkill(base, g_csRow - CS_COM_PIP_ITEMS);
        if (i < 0 || (camp && i >= csCampSkillCount(base))) return true;

        char titled[320];
        csGridSkillTitle(base, camp, i, titled, sizeof titled);

        if (!(camp ? csCampSkillLearned(base, i) : csComSkillLearned(base, i))) {
            postSpeech(axs(camp ? AXS_CS_CAMP_LOCKED : AXS_CS_SKILL_LOCKED));
            return true;
        }
        int64_t cell = camp ? csCampCellId(i) : csComCellId(i);
        if (!feElemOnScreen(cell)) {
            logLine("charsheet slot: cell 0x%llx not on screen — refusing to assign %s skill %d",
                    (unsigned long long)cell, camp ? "camping" : "combat", i);
            postSpeech(axs(AXS_CS_CANT_CHANGE_NOW));
            return true;
        }
        int cap = camp ? CAMP_SEL_MAX : csComMaxSelected(base);
        if (slot > cap) {
            char utter[96];
            _snprintf(utter, sizeof utter, axs(AXS_CS_ONLY_N_SLOTS_FMT), cap);
            utter[sizeof utter - 1] = 0;
            postSpeech(utter);
            return true;
        }

        int was = camp ? csCampSkillSlot(base, i) : csComSkillSlot(base, i);
        CsSlotResult r;
        if (!csComAssignSlot(base, i, slot, &r, camp)) {
            logLine("charsheet slot: assign of skill %d to slot %d refused", i, slot);
            postSpeech(axs(AXS_DIDNT_HAPPEN));
            return true;
        }
        if (r.landed <= 0) {                        // the vector says it is not equipped: say so
            logLine("charsheet slot: skill %d did not land in a slot", i);
            postSpeech(axs(AXS_DIDNT_HAPPEN));
            return true;
        }

        char utter[MAILBOX_SZ];
        if (was == r.landed && was == slot) {
            _snprintf(utter, sizeof utter, axs(AXS_CS_ALREADY_IN_SLOT_FMT), titled, r.landed);
        } else {
            _snprintf(utter, sizeof utter, axs(AXS_CS_SKILL_IN_SLOT_FMT), titled, r.landed);
            if (r.movedIdx >= 0 && r.movedTo > 0) {
                char other[320];
                csGridSkillTitle(base, camp, r.movedIdx, other, sizeof other);
                char frag[400];
                frag[0] = ' '; _snprintf(frag + 1, sizeof frag - 1, axs(AXS_CS_SKILL_IN_SLOT_FMT), other, r.movedTo);
                strncat(utter, frag, sizeof utter - strlen(utter) - 1);
            } else if (r.displaced >= 0) {
                char other[320];
                csGridSkillTitle(base, camp, r.displaced, other, sizeof other);
                char frag[400];
                frag[0] = ' '; _snprintf(frag + 1, sizeof frag - 1,
                                         axs(camp ? AXS_CS_CAMP_DESELECTED_FMT : AXS_CS_SKILL_UNEQUIPPED_FMT), other);
                strncat(utter, frag, sizeof utter - strlen(utter) - 1);
            }
            if (r.landed != slot)
                strncat(utter, " The slots fill in order.", sizeof utter - strlen(utter) - 1);
        }
        utter[sizeof utter - 1] = 0;
        postSpeech(utter);
        logLine("charsheet slot: %s skill %d \"%s\" was slot %d, asked %d, landed %d "
                "(displaced %d, swapped %d -> %d)",
                camp ? "camping" : "combat", i, titled, was, slot, r.landed, r.displaced, r.movedIdx, r.movedTo);
        if (!camp) {
            int col = csComSkillToCol(base, i);
            if (col >= 0) g_csRow = CS_COM_PIP_ITEMS + col;
        }
        g_csTipLine = 0;
        return true;
    }

    if ((sym == SDLK_LEFT || sym == SDLK_RIGHT) && !ctrl) {
        if (axNavHoldRepeat(repeat)) return true;
        csMoveRow(base, sym == SDLK_RIGHT ? 1 : -1);
        return true;
    }

    int jump = 0;
    if (axDecodeJump(sym, mod, repeat, &jump)) {
        if (!jump) return true;                      // held jump: one landing per press
        if (g_csRow < 0) g_csRow = 0;                // pre-place: csMoveRow's unplaced branch
        csMoveRow(base, jump);
        return true;
    }

    if (sym != SDLK_UP && sym != SDLK_DOWN) return false;

    if (ctrl) {
        if (g_csSection < 0 || g_csSection >= kCsSectionCount) return false;
        if (!kCsSections[g_csSection].tip) {
            char type[320];
            csRowTypeWords(base, &kCsSections[g_csSection], g_csRow, type, sizeof type);
            if (!type[0]) return false;
        }
        if (repeat) return true;
        csSpeakTipLine(base, sym == SDLK_UP ? 1 : -1);
        return true;
    }

    if (axNavHoldRepeat(repeat)) return true;
    csMoveSection(base, sym == SDLK_UP ? -1 : 1);
    return true;
}

static uintptr_t g_csLastHero = 0;

void serviceCharSheet(uintptr_t base) {
    uintptr_t panel = csOpenPanel(base);
    bool townSheet = false;
    if (!panel) {
        panel = csTownSheetPanel(base);
        townSheet = panel != 0;
    }
    bool open = panel != 0;

    if (open != g_csOpen) {
        g_csOpen = open;
        logLine("charsheet %s%s", open ? "OPEN" : "closed", townSheet ? " (town)" : "");
        if (open) {
            parkCursor(townSheet ? "character sheet opened (town)" : "character sheet opened");
            if (!g_csDumped && !townSheet) { csDumpSheet(base, panel); g_csDumped = true; }
            g_csLastHero = abSelectedHero(base);
            g_csSection  = 0;    // the sheet always opens at its title
            g_csRow      = -1;
            csSpeakTitle(base, "open");
        } else {
            g_csLastHero = 0;    // closed: the next open re-announces from scratch
            g_csSection  = 0;
            g_csRow      = -1;
            g_csSkillWatchUntil = 0;   // no equip watch outlives the sheet
            g_csTrkConfirmSlot  = -1;  // nor an armed unequip confirm
            g_csTrkWatchUntil   = 0;
            g_csOutfitWatchUntil = 0;  // nor an outfit switch
        }
        return;
    }
    if (!open) return;

    csOutfitService(base);

    if (g_csTrkWatchUntil) {
        uintptr_t h = abSelectedHero(base);
        uintptr_t beg = 0; int slots = 0;
        uint32_t amount = 1;
        if (h && invItemVectorAt(h + HERO_TRINKET_SYSTEM_OFF, &beg, &slots) &&
            g_csTrkWatchSlot >= 0 && g_csTrkWatchSlot < slots)
            safeReadU32(beg + (uintptr_t)g_csTrkWatchSlot * ITEM_STRIDE + ITEM_AMOUNT_OFF, &amount);
        if ((int32_t)amount < 1) {
            g_csTrkWatchUntil = 0;
            char utter[320];
            _snprintf(utter, sizeof utter, axs(AXS_CS_TRK_UNEQUIPPED_FMT),
                      g_csTrkWatchName[0] ? g_csTrkWatchName : axs(AXS_CS_TRK_FALLBACK));
            utter[sizeof utter - 1] = 0;
            logLine("charsheet trinket: unequip observed (slot %d empty)", g_csTrkWatchSlot);
            postSpeech(utter);
        } else if (GetTickCount() > g_csTrkWatchUntil) {
            g_csTrkWatchUntil = 0;
            logLine("charsheet trinket: unequip watch timed out (slot %d still filled)",
                    g_csTrkWatchSlot);
            postSpeech(axs(AXS_DIDNT_HAPPEN));
        }
    }

    if (g_csSkillWatchUntil) {
        bool now = g_csSkillWatchCamp ? csCampSkillSelected(base, g_csSkillWatchIdx)
                                      : csComSkillEquipped(base, g_csSkillWatchIdx);
        if (now == g_csSkillWantEquipped) {
            g_csSkillWatchUntil = 0;
            char utter[192];
            int slot = !now ? 0 : g_csSkillWatchCamp ? csCampSkillSlot(base, g_csSkillWatchIdx)
                                                     : csComSkillSlot(base, g_csSkillWatchIdx);
            if (slot > 0) _snprintf(utter, sizeof utter, axs(AXS_CS_SKILL_IN_SLOT_FMT),
                                    g_csSkillWatchName, slot);
            else if (g_csSkillWatchCamp)
                          _snprintf(utter, sizeof utter,
                                    axs(now ? AXS_CS_CAMP_SELECTED_FMT
                                            : AXS_CS_CAMP_DESELECTED_FMT),
                                    g_csSkillWatchName);
            else          _snprintf(utter, sizeof utter,
                                    axs(now ? AXS_CS_SKILL_EQUIPPED_FMT
                                            : AXS_CS_SKILL_UNEQUIPPED_FMT),
                                    g_csSkillWatchName);
            utter[sizeof utter - 1] = 0;
            postSpeech(utter);
            logLine("charsheet %s: observed %s of \"%s\"",
                    g_csSkillWatchCamp ? "camskill" : "comskill",
                    now ? "equip" : "unequip", g_csSkillWatchName);
            if (g_csSkillWatchCamp) csCampProbe(base, "after flip");
            if (!g_csSkillWatchCamp && g_csSection >= 0 && g_csSection < kCsSectionCount &&
                kCsSections[g_csSection].row == csComSectionRow) {
                int col = csComSkillToCol(base, g_csSkillWatchIdx);
                if (col >= 0) g_csRow = CS_COM_PIP_ITEMS + col;
            }
        } else if (GetTickCount() > g_csSkillWatchUntil) {
            g_csSkillWatchUntil = 0;
            logLine("charsheet %s: watch timed out for \"%s\" (want=%d)",
                    g_csSkillWatchCamp ? "camskill" : "comskill",
                    g_csSkillWatchName, (int)g_csSkillWantEquipped);
            if (g_csSkillWatchCamp) csCampProbe(base, "after timeout");
            postSpeech(axs(AXS_CS_DIDNT_CHANGE));
        }
    }

    // Sheet is open and stayed open: did Q/E (or a click) move the selected hero?
    uintptr_t hero = abSelectedHero(base);
    if (!hero || hero == g_csLastHero) return;
    bool wasSwap = g_csLastHero != 0;
    g_csLastHero = hero;
    if (!wasSwap) return;
    g_csSection = 0;
    g_csRow     = -1;
    g_csTrkConfirmSlot = -1;
    csSpeakTitle(base, "swap");
}

// ---- TOWN: the HERO RENAME session (the sheet's edit box) ----
static const uintptr_t CS_SHEET_EDIT_OFF = 0x14c;   // sheet panel + : byte, rename box is live

void checkTownRename(uintptr_t base) {
    uintptr_t panel = csTownSheetPanel(base);
    uint8_t editing = 0;
    if (panel) safeReadU8(panel + CS_SHEET_EDIT_OFF, &editing);
    bool on = panel != 0 && editing != 0;

    if (on && !g_trActive) {
        g_trActive = true;
        g_trOpenWatchUntil = 0;
        g_trHero = abSelectedHero(base);
        g_trOld[0] = 0;
        if (g_trHero) safeReadCStr(g_trHero + HERO_NAME_OFF, g_trOld, sizeof g_trOld);
        // open the shared naming session in hero mode, seeded with the current name
        g_naming = true;
        g_nameModeHero = true;
        strncpy(g_typed, g_trOld, sizeof g_typed - 1);
        g_typed[sizeof g_typed - 1] = 0;
        strncpy(g_lastName, g_typed, sizeof g_lastName - 1);   // the seed is spoken in the cue
        g_lastName[sizeof g_lastName - 1] = 0;
        char line[256];
        char head[192];
        _snprintf(head, sizeof head, axs(AXS_CS_RENAME_FMT),
                  g_trOld[0] ? g_trOld : axs(AXS_CS_RENAME_SUBJECT));
        head[sizeof head - 1] = 0;
        _snprintf(line, sizeof line, "%s", head);
        line[sizeof line - 1] = 0;
        axAppendHint(line, sizeof line, AXS_NM_HELP);   // "" while Control hints is Off
        postSpeech(line);
        logLine("townrename OPEN hero=%p seed=\"%s\"", (void*)g_trHero, g_trOld);
        return;
    }
    if (!on && g_trActive) {
        g_trActive = false;
        endNaming();
        char now[64] = {0};
        if (g_trHero) safeReadCStr(g_trHero + HERO_NAME_OFF, now, sizeof now);
        logLine("townrename CLOSE old=\"%s\" now=\"%s\"", g_trOld, now);
        char line[160];
        if (now[0] && strcmp(now, g_trOld) != 0) {
            _snprintf(line, sizeof line, axs(AXS_CS_RENAMED_FMT), now);
            line[sizeof line - 1] = 0;
            postSpeech(line);
        } else {
            postSpeech(axs(AXS_CS_RENAME_CANCELLED));
        }
        g_trHero = 0;
        return;
    }

    if (g_trOpenWatchUntil && !on && GetTickCount() > g_trOpenWatchUntil) {
        g_trOpenWatchUntil = 0;
        logLine("townrename: the rename click made no observable change");
        postSpeech(axs(AXS_CS_RENAME_NO_BOX));
    }
    if (on) g_trOpenWatchUntil = 0;
}

// ---- PAD FOCUS FOLLOWER ----
bool csFocusSync(uintptr_t base, int64_t id, uint32_t owner) {
    (void)owner;
    if (!g_csOpen) return false;
    uint32_t cc = (uint32_t)(uint64_t)id;
    uint32_t dc = cc - (CS_COMSKILL_OWNER + CS_COMSKILL_CLICK_TAG);
    uint32_t dk = cc - (CS_CAMSKILL_OWNER + CS_COMSKILL_CLICK_TAG);
    if (dc < (uint32_t)CS_COMSKILL_MAX) {
        int order[CS_COMSKILL_MAX];
        int cnt = csComSortedOrder(base, order);
        for (int col = 0; col < cnt; col++) {
            if (order[col] != (int)dc) continue;
            for (int s = 0; s < kCsSectionCount; s++) {
                if (kCsSections[s].row != csComSectionRow) continue;
                g_csSection = s;
                g_csRow = col + CS_COM_PIP_ITEMS;
                csMoveRow(base, 0);
                return true;
            }
        }
        return false;
    }
    if (dk < (uint32_t)csCampSkillCount(base)) {
        for (int s = 0; s < kCsSectionCount; s++) {
            if (kCsSections[s].row != csCampSkillRowText) continue;
            g_csSection = s;
            g_csRow = (int)dk;
            csMoveRow(base, 0);
            return true;
        }
    }
    return false;
}
