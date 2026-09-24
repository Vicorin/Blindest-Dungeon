// raid/results.cpp -- THE RAID RESULTS SCREEN

#include <cstdint>
#include <cstring>
#include <cstdio>
#include "internal.h"

// ---- RAID RESULTS SCREEN (the end-of-run summary) ----
static const uintptr_t RR_RESULTS_OFF     = 0x1758;
static const uintptr_t RR_STATE_OFF       = 0x1768;
                                                       // 3/9 = circus variants
static const uintptr_t RR_REVEAL_MASK_OFF = 0x1938;
static const uintptr_t RR_QUEST_INV_OFF   = 0x2f8;
static const uintptr_t RR_PARTY_INV_OFF   = 0x3c8;
                                                       // heirlooms, split by item type when drawn
static const uintptr_t RR_HREC_VEC_BEG    = 0x430;
static const uintptr_t RR_HREC_VEC_END    = 0x438;     // results+: vector end (was 0x3e8)
static const uintptr_t RR_HREC_STRIDE     = 0x28;
static const uintptr_t RR_HREC_ROSTER_ID  = 0x00;      // record+: int32 roster id
static const uintptr_t RR_HREC_PRE_XP     = 0x08;      // record+: int32 XP at raid START
static const uintptr_t RR_HREC_QUIRK_BEG  = 0x10;
static const uintptr_t RR_HREC_QUIRK_END  = 0x18;      // record+: vector end
static const uintptr_t RR_PROFILE_HERO_BEG= 0x20;      // profile+: roster-entry vector begin
static const uintptr_t RR_PROFILE_HERO_END= 0x28;      // profile+: vector end
static const uintptr_t RR_ENTRY_HERO_OFF  = 0x08;      // entry+: the Hero object, inline
static const uintptr_t RR_ENTRY_STATUS_OFF= 0x1540;    // entry+: int status; 3 = dead (dead (was 0x14c0;
static const uintptr_t RR_ITEM_DEF_OFF    = 0x598;
static const uintptr_t RR_ITEMDEF_VALUE_OFF = 0xa4;    // Inventory::Item+: int32 gold value (was 0x98)
static const int64_t   RR_BTN_FWRD        = 0x66777264; // 'fwrd' — Next / Return to Town
static const int64_t   RR_BTN_BACK        = 0x6261636b; // 'back' — the game-wide back id (logged only)
static const uint32_t  RR_MASK_ID_BASE    = 0x6d61736b; // 'mask' + N — un-revealed quirk click zones
static const int       RR_MAX_HEROES      = 8;          // the party is 4; a hard cap on the walk
static const int       RR_MAX_ITEMS       = 64;         // per-grid cap on the occupied-slot walk
static const int       RR_MAX_QUIRKS      = 16;         // per-hero cap on the new-quirk walk

static bool     g_rrActive        = false;
static bool     g_rrAnnounced     = false;  // the entry line was spoken (waits for state != 0)
static int      g_rrLastState     = -1;     // last page state seen, to announce page CHANGES
static int      g_rrRow           = 0;      // page-1 cursor
static int      g_rrHero          = 0;      // page-2 hero cursor
static int      g_rrHeroRow       = 0;      // page-2 row cursor within the hero
static bool     g_rrRevealPending = false;  // Enter clicked 'fwrd' to reveal quirks; watching
static uint32_t g_rrRevealMaskAt  = 0;      // the reveal bitmask when the click was queued
static uint32_t g_rrRevealDeadline= 0;      // give up watching after this tick
static uint32_t g_rrAppearTick    = 0;
static uint32_t g_rrNextOutcomeTry= 0;      // throttle for the pre-announce outcome polling

uintptr_t rrDisplay(uintptr_t base) {
    uintptr_t d = 0;
    if (!safeReadPtr(base + RR_DISPLAY_RVA, &d) || d <= 0x10000) return 0;
    return d;
}

uintptr_t rrResults(uintptr_t base) {
    uintptr_t d = rrDisplay(base);
    if (!d) return 0;
    uintptr_t r = 0;
    if (!safeReadPtr(d + RR_RESULTS_OFF, &r) || r <= 0x10000) return 0;
    return r;
}

int rrState(uintptr_t base) {
    uintptr_t d = rrDisplay(base);
    if (!d) return -1;
    uint32_t s = 0;
    if (!safeReadU32(d + RR_STATE_OFF, &s)) return -1;
    return (int)s;
}

static bool rrOutcomeText(uintptr_t base, char* out, int outsz) {
    char key[0x80] = { 0 };
    if (!safeReadCStr(base + RR_OUTCOME_KEY_RVA, key, sizeof key) || !key[0]) return false;
    if (strncmp(key, "raid_results", 12) != 0) {
        logLine("results: outcome buffer holds \"%s\" — not an outcome key, refusing", key);
        return false;
    }
    for (const char* p = key; *p; p++)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '_')) {
            logLine("results: outcome key \"%s\" is not an identifier — refusing", key);
            return false;
        }
    if (!resolveKey(base, key, out, outsz)) {
        logLine("results: outcome key \"%s\" did not resolve", key);
        return false;
    }
    abStripMarkup(out);
    logLine("results: outcome key \"%s\" -> \"%s\"", key, out);
    return true;
}

static int rrSystemItems(uintptr_t system, uintptr_t* out, int maxOut) {
    uintptr_t beg = 0; int slots = 0;
    if (!invItemVectorAt(system, &beg, &slots)) return 0;
    int n = 0;
    for (int i = 0; i < slots && n < maxOut; i++) {
        uintptr_t item = beg + (uintptr_t)i * ITEM_STRIDE;
        uint32_t amount = 0;
        if (!safeReadU32(item + ITEM_AMOUNT_OFF, &amount)) continue;
        if ((int32_t)amount < 1) continue;             // the game's own empty test
        if (out) out[n] = item;
        n++;
    }
    return n;
}

static bool rrItemIsHeirloom(uintptr_t item) {
    char type[64] = { 0 };
    safeReadCStr(item + ITEM_TYPE_OFF, type, sizeof type);
    return strcmp(type, "heirloom") == 0;
}

static long long rrTreasureTotal(uintptr_t base, uintptr_t system) {
    uintptr_t items[RR_MAX_ITEMS];
    int n = rrSystemItems(system, items, RR_MAX_ITEMS);
    uint32_t goldHash = 0;
    safeReadU32(base + RR_GOLD_TYPEHASH_RVA, &goldHash);
    long long total = 0;
    for (int i = 0; i < n; i++) {
        uint32_t amount = 0, typeHash = 0;
        safeReadU32(items[i] + ITEM_AMOUNT_OFF, &amount);
        safeReadU32(items[i] + ITEM_TYPEHASH_OFF, &typeHash);
        if (goldHash && typeHash == goldHash) {
            total += (long long)(int32_t)amount;
            logLine("results gold: item %d is gold, amount %d", i + 1, (int32_t)amount);
            continue;
        }
        uintptr_t def = 0;
        if (!safeReadPtr(items[i] + RR_ITEM_DEF_OFF, &def) || def <= 0x10000) {
            logLine("results gold: item %d has no cached definition — value skipped", i + 1);
            continue;
        }
        uint32_t value = 0;
        safeReadU32(def + RR_ITEMDEF_VALUE_OFF, &value);
        total += (long long)(int32_t)value * (int32_t)amount;
        logLine("results gold: item %d value %d x %d", i + 1, (int32_t)value, (int32_t)amount);
    }
    if (total < 0) total = 0;
    if (total > 2000000000LL) total = 2000000000LL;     // the game's own cap
    return total;
}

static void rrSectionTitle(uintptr_t base, const char* locKey, const char* fallback,
                           char* out, int outsz) {
    if (resolveKey(base, locKey, out, outsz) && out[0]) { abStripMarkup(out); return; }
    _snprintf(out, outsz, "%s", fallback);
    out[outsz - 1] = 0;
}

static void rrItemRowText(uintptr_t base, uintptr_t item, int pos, int total,
                          char* out, int outsz) {
    char type[64], itemId[64], key[192], name[256];
    invItemName(base, item, type, itemId, key, name);
    uint32_t amount = 0;
    safeReadU32(item + ITEM_AMOUNT_OFF, &amount);

    char rarity[96] = { 0 }, clsreq[256] = { 0 }, fx[768] = { 0 };
    char charges[384] = { 0 }, trig[1536] = { 0 };
    bool trinket = invItemIsTrinket(item);
    if (trinket) {
        uintptr_t rec = bldTrinketRecord(base, item);
        if (rec) {
            bldTrinketRarity(base, rec, rarity, sizeof rarity, false);
            bldTrinketClassReq(base, rec, clsreq, sizeof clsreq, false);
            bldTrinketCharges(base, item, rec, charges, sizeof charges, false);
            bldTrinketTriggers(base, rec, trig, sizeof trig, false);
        }
        bldTrinketEffects(base, item, fx, sizeof fx, false);
    }

    char head[384];
    if (amount > 1) _snprintf(head, sizeof head, "%s, %u", name, amount);
    else            _snprintf(head, sizeof head, "%s", name);
    head[sizeof head - 1] = 0;

    char posText[96];
    _snprintf(posText, sizeof posText, axs(AXS_INV_POS_FMT), axs(AXS_LOOT_NOUN_ITEM), pos, total);
    posText[sizeof posText - 1] = 0;
    _snprintf(out, outsz, "%s%s%s%s%s%s%s%s%s%s%s. %s",
              head, rarity[0] ? ". " : "", rarity, clsreq[0] ? ". " : "", clsreq,
              charges[0] ? ". " : "", charges,
              fx[0] ? ". " : "", fx,
              trig[0] ? ". " : "", trig,
              posText);
    out[outsz - 1] = 0;

    if (trinket)
        logLine("results trinket: \"%s\" rarity=\"%s\" class=\"%s\" effects=%s charges=\"%s\""
                " triggers=%s", name, rarity, clsreq, fx[0] ? "yes" : "NONE READ", charges,
                trig[0] ? "yes" : "none");
}

// ---- Page 1: one flat list — outcome, quest rewards, treasure (+ total), heirlooms ----
static int rrPage1Layout(uintptr_t base, int* questN, int* treasN, int* heirN, int* goldRow) {
    *questN = *treasN = *heirN = 0; *goldRow = 0;
    uintptr_t results = rrResults(base);
    if (!results) return 1;                            // the outcome row always exists
    uintptr_t items[RR_MAX_ITEMS];
    *questN = rrSystemItems(results + RR_QUEST_INV_OFF, nullptr, RR_MAX_ITEMS);
    int pn = rrSystemItems(results + RR_PARTY_INV_OFF, items, RR_MAX_ITEMS);
    for (int i = 0; i < pn; i++)
        (rrItemIsHeirloom(items[i]) ? *heirN : *treasN)++;
    *goldRow = (pn > 0) ? 1 : 0;
    return 1 + *questN + *treasN + *goldRow + *heirN;
}

static int rrPage1RowSection(uintptr_t base, int row) {
    int questN, treasN, heirN, goldRow;
    rrPage1Layout(base, &questN, &treasN, &heirN, &goldRow);
    if (row <= 0) return 0;
    int r = row - 1;
    if (r < questN) return 1;
    r -= questN;
    if (r < treasN + goldRow) return 2;
    return 3;
}

static void rrPage1SectionName(uintptr_t base, int section, char* out, int outsz) {
    out[0] = 0;
    if (section == 1)
        rrSectionTitle(base, "raid_results_quest_inventory_title", axs(AXS_RR_SECT_QUEST),
                       out, outsz);
    else if (section == 2)
        rrSectionTitle(base, "raid_results_party_gold_inventory_title", axs(AXS_RR_SECT_TREASURE),
                       out, outsz);
    else if (section == 3)
        rrSectionTitle(base, "raid_results_party_heirloom_inventory_title",
                       axs(AXS_RR_SECT_HEIRLOOMS), out, outsz);
}

static bool rrPage1RowText(uintptr_t base, int row, char* out, int outsz) {
    int questN, treasN, heirN, goldRow;
    int total = rrPage1Layout(base, &questN, &treasN, &heirN, &goldRow);
    if (row < 0 || row >= total) return false;

    if (row == 0) {                                    // the outcome line
        if (!rrOutcomeText(base, out, outsz))
            _snprintf(out, outsz, "%s", axs(AXS_RR_OUTCOME_FALLBACK));
        out[outsz - 1] = 0;
        return true;
    }

    uintptr_t results = rrResults(base);
    if (!results) return false;
    int r = row - 1;

    if (r < questN) {                                  // quest rewards
        uintptr_t items[RR_MAX_ITEMS];
        int n = rrSystemItems(results + RR_QUEST_INV_OFF, items, RR_MAX_ITEMS);
        if (r >= n) return false;
        rrItemRowText(base, items[r], r + 1, n, out, outsz);
        return true;
    }
    r -= questN;

    uintptr_t items[RR_MAX_ITEMS];
    int pn = rrSystemItems(results + RR_PARTY_INV_OFF, items, RR_MAX_ITEMS);
    uintptr_t treas[RR_MAX_ITEMS], heir[RR_MAX_ITEMS];
    int tn = 0, hn = 0;
    for (int i = 0; i < pn; i++) {
        if (rrItemIsHeirloom(items[i])) heir[hn++] = items[i];
        else                            treas[tn++] = items[i];
    }

    if (r < tn) {                                      // collected treasure
        rrItemRowText(base, treas[r], r + 1, tn, out, outsz);
        return true;
    }
    r -= tn;

    if (goldRow && r == 0) {                           // the treasure total
        long long gold = rrTreasureTotal(base, results + RR_PARTY_INV_OFF);
        char goldName[64];
        resCurrencyTitleById(base, "gold", "raidresults", goldName, sizeof goldName);
        _snprintf(out, outsz, axs(AXS_RR_TREASURE_TOTAL_FMT), gold, goldName);
        out[outsz - 1] = 0;
        return true;
    }
    r -= goldRow;

    if (r < hn) {                                      // collected heirlooms
        rrItemRowText(base, heir[r], r + 1, hn, out, outsz);
        return true;
    }
    return false;
}

// ---- Page 2: per-hero records — name/rank, XP gained, new quirks ----
static int rrHeroCount(uintptr_t base) {
    uintptr_t results = rrResults(base);
    if (!results) return 0;
    uintptr_t beg = 0, end = 0;
    if (!safeReadPtr(results + RR_HREC_VEC_BEG, &beg) ||
        !safeReadPtr(results + RR_HREC_VEC_END, &end) || beg <= 0x10000 || end < beg) return 0;
    uintptr_t span = end - beg;
    if (span % RR_HREC_STRIDE != 0) {
        logLine("results: hero vector span %llu not a multiple of 0x28 — refusing",
                (unsigned long long)span);
        return 0;
    }
    int n = (int)(span / RR_HREC_STRIDE);
    if (n > RR_MAX_HEROES) { logLine("results: implausible hero count %d — refusing", n); return 0; }
    return n;
}

static uintptr_t rrHeroRecAt(uintptr_t base, int i) {
    if (i < 0 || i >= rrHeroCount(base)) return 0;
    uintptr_t results = rrResults(base);
    uintptr_t beg = 0;
    if (!results || !safeReadPtr(results + RR_HREC_VEC_BEG, &beg) || beg <= 0x10000) return 0;
    return beg + (uintptr_t)i * RR_HREC_STRIDE;
}

static uintptr_t rrRosterEntryById(uintptr_t base, int rosterId) {
    typedef uintptr_t (*RrProfileFn)();
    uintptr_t profile = 0;
    __try { profile = ((RrProfileFn)(base + RR_PROFILE_GET_RVA))(); }
    __except (EXCEPTION_EXECUTE_HANDLER) { profile = 0; }
    if (profile <= 0x10000) { logLine("results: profile getter returned nothing"); return 0; }

    uintptr_t beg = 0, end = 0;
    if (!safeReadPtr(profile + RR_PROFILE_HERO_BEG, &beg) ||
        !safeReadPtr(profile + RR_PROFILE_HERO_END, &end) || beg <= 0x10000 || end < beg) return 0;
    uintptr_t count = (end - beg) / 8;
    if (count > 64) { logLine("results: implausible roster size %llu", (unsigned long long)count); return 0; }
    for (uintptr_t i = 0; i < count; i++) {
        uintptr_t entry = 0;
        if (!safeReadPtr(beg + i * 8, &entry) || entry <= 0x10000) continue;
        uint32_t id = 0;
        if (safeReadU32(entry + 0x00, &id) && (int)id == rosterId) return entry;
    }
    logLine("results: roster id %d not found among %llu entries", rosterId,
            (unsigned long long)count);
    return 0;
}

// The live Hero for a roster id (0 when the roster lookup fails).
static uintptr_t rrLiveHeroById(uintptr_t base, int rosterId) {
    uintptr_t entry = rrRosterEntryById(base, rosterId);
    return entry ? entry + RR_ENTRY_HERO_OFF : 0;
}

static int rrHeroNewQuirks(uintptr_t base, int heroIdx, uintptr_t* out, int maxOut) {
    uintptr_t rec = rrHeroRecAt(base, heroIdx);
    if (!rec) return 0;
    uintptr_t beg = 0, end = 0;
    if (!safeReadPtr(rec + RR_HREC_QUIRK_BEG, &beg) ||
        !safeReadPtr(rec + RR_HREC_QUIRK_END, &end) || beg <= 0x10000 || end < beg) return 0;
    uintptr_t count = (end - beg) / 8;
    if (count > (uintptr_t)RR_MAX_QUIRKS) {
        logLine("results: implausible quirk count %llu — refusing", (unsigned long long)count);
        return 0;
    }

    uint32_t rosterId = 0;
    safeReadU32(rec + RR_HREC_ROSTER_ID, &rosterId);
    uintptr_t hero = rrLiveHeroById(base, (int)rosterId);

    int n = 0;
    for (uintptr_t i = 0; i < count && n < maxOut; i++) {
        uintptr_t qc = 0;
        if (!safeReadPtr(beg + i * 8, &qc) || qc <= 0x10000) continue;
        if (hero) {                                    // the drawer's re-filter
            bool still = false;
            uintptr_t qb = 0, qe = 0;
            if (safeReadPtr(hero + HERO_QUIRK_BEGIN_OFF, &qb) &&
                safeReadPtr(hero + HERO_QUIRK_END_OFF, &qe) && qb > 0x10000 && qe >= qb &&
                (qe - qb) / QUIRK_ENTRY_STRIDE <= 64) {
                for (uintptr_t e = qb; e < qe; e += QUIRK_ENTRY_STRIDE) {
                    uintptr_t hq = 0;
                    if (safeReadPtr(e + QUIRK_ENTRY_CLASS_OFF, &hq) && hq == qc) { still = true; break; }
                }
            }
            if (!still) {
                logLine("results: hero %d new quirk %llu no longer held — dropped",
                        heroIdx + 1, (unsigned long long)i);
                continue;
            }
        }
        out[n++] = qc;
    }
    return n;
}

static bool rrRevealWord(uintptr_t base, uint32_t* out) {
    uintptr_t d = rrDisplay(base);
    if (!d) return false;
    uintptr_t words = 0;
    if (!safeReadPtr(d + RR_REVEAL_MASK_OFF, &words) || words <= 0x10000) return false;
    return safeReadU32(words, out);
}

// Has this hero's quirk column been revealed? The game's own bit, by hero index.
static bool rrHeroRevealed(uintptr_t base, int heroIdx) {
    uint32_t w = 0;
    if (!rrRevealWord(base, &w)) return false;
    return ((w >> (heroIdx & 31)) & 1) != 0;
}

static void rrHeroHeaderText(uintptr_t base, int heroIdx, char* out, int outsz) {
    int total = rrHeroCount(base);
    uintptr_t rec = rrHeroRecAt(base, heroIdx);
    uint32_t rosterId = 0;
    if (rec) safeReadU32(rec + RR_HREC_ROSTER_ID, &rosterId);
    uintptr_t hero = rec ? rrLiveHeroById(base, (int)rosterId) : 0;

    char label[224] = { 0 };
    if (hero) {
        char name[80], cls[80], rank[128] = { 0 };
        if (abHeroNameClassOf(base, hero, name, sizeof name, cls, sizeof cls)) {
            int level = csResolveLevel(base, hero);
            if (level >= 0) {
                char key[32];
                _snprintf(key, sizeof key, "str_resolve_%d", level);
                key[sizeof key - 1] = 0;
                if (!resolveKey(base, key, rank, sizeof rank)) rank[0] = 0;
            }
            char title[160];
            if (rank[0] && cls[0]) _snprintf(title, sizeof title, "%s %s", rank, cls);
            else                   _snprintf(title, sizeof title, "%s", rank[0] ? rank : cls);
            title[sizeof title - 1] = 0;
            if (name[0] && title[0]) _snprintf(label, sizeof label, "%s, %s", name, title);
            else                     _snprintf(label, sizeof label, "%s", name[0] ? name : title);
            label[sizeof label - 1] = 0;
        }
    }
    char pos[96];
    _snprintf(pos, sizeof pos, axs(AXS_INV_POS_FMT), axs(AXS_RR_NOUN_HERO), heroIdx + 1, total);
    pos[sizeof pos - 1] = 0;
    if (label[0]) _snprintf(out, outsz, "%s. %s", label, pos);
    else          _snprintf(out, outsz, "%s", pos);
    out[outsz - 1] = 0;
}

static int rrLevelOfXp(uintptr_t base, uint32_t xp) {
    uintptr_t tbl = 0, begin = 0, end = 0;
    if (!safeReadPtr(base + RESOLVE_TABLE_RVA, &tbl) || tbl <= 0x10000) return -1;
    if (!safeReadPtr(tbl + RESOLVE_VEC_BEGIN, &begin) || begin <= 0x10000) return -1;
    if (!safeReadPtr(tbl + RESOLVE_VEC_END, &end) || end <= begin) return -1;
    uintptr_t count = (end - begin) / 4;
    if (count == 0 || count > RESOLVE_MAX_LEVELS) return -1;
    int level = 0;
    for (uintptr_t i = 0; i < count; i++) {
        uint32_t th = 0;
        if (!safeReadU32(begin + i * 4, &th)) return -1;
        if (xp < th) break;
        level = (int)i;
    }
    return level;
}

static bool rrXpProgress(uintptr_t base, uint32_t xp, int* intoOut, int* spanOut, bool* maxOut) {
    uintptr_t tbl = 0, begin = 0, end = 0;
    if (!safeReadPtr(base + RESOLVE_TABLE_RVA, &tbl) || tbl <= 0x10000) return false;
    if (!safeReadPtr(tbl + RESOLVE_VEC_BEGIN, &begin) || begin <= 0x10000) return false;
    if (!safeReadPtr(tbl + RESOLVE_VEC_END, &end) || end <= begin) return false;
    uintptr_t count = (end - begin) / 4;
    if (count == 0 || count > RESOLVE_MAX_LEVELS) return false;

    uint32_t th[RESOLVE_MAX_LEVELS];
    int level = 0;
    for (uintptr_t i = 0; i < count; i++) {
        if (!safeReadU32(begin + i * 4, &th[i])) return false;
        if (xp >= th[i]) level = (int)i;
    }
    if (level >= (int)count - 1) { *maxOut = true; *intoOut = 0; *spanOut = 0; return true; }
    *maxOut = false;
    *intoOut = (int)(xp - th[level]);
    *spanOut = (int)(th[level + 1] - th[level]);
    return true;
}

// Rows for one hero: header, XP (or Dead), then one row per new quirk.
static int rrHeroRowCount(uintptr_t base, int heroIdx) {
    uintptr_t quirks[RR_MAX_QUIRKS];
    return 2 + rrHeroNewQuirks(base, heroIdx, quirks, RR_MAX_QUIRKS);
}

static bool rrHeroRowText(uintptr_t base, int heroIdx, int row, char* out, int outsz) {
    if (row == 0) { rrHeroHeaderText(base, heroIdx, out, outsz); return true; }

    uintptr_t rec = rrHeroRecAt(base, heroIdx);
    if (!rec) return false;
    uint32_t rosterId = 0;
    safeReadU32(rec + RR_HREC_ROSTER_ID, &rosterId);
    uintptr_t entry = rrRosterEntryById(base, (int)rosterId);
    uintptr_t hero  = entry ? entry + RR_ENTRY_HERO_OFF : 0;

    if (row == 1) {                                    // the XP line, or the dead notice
        uint32_t status = 0;
        if (entry) safeReadU32(entry + RR_ENTRY_STATUS_OFF, &status);
        logLine("results: hero %d roster=%d entry=%p status=%u", heroIdx + 1, (int)rosterId,
                (void*)entry, status);
        if (hero && status == 3) {
            _snprintf(out, outsz, "%s", axs(AXS_RR_DEAD_NO_XP));
            out[outsz - 1] = 0;
            return true;
        }
        if (!hero) {
            _snprintf(out, outsz, "%s", axs(AXS_RR_XP_UNKNOWN));
            out[outsz - 1] = 0;
            return true;
        }
        uint32_t preXp = 0, liveXp = 0;
        safeReadU32(rec + RR_HREC_PRE_XP, &preXp);
        safeReadU32(hero + HERO_RESOLVE_XP_OFF, &liveXp);
        int gained = (int)liveXp - (int)preXp;
        if (gained < 0) gained = 0;
        logLine("results: hero %d xp %u -> %u (gained %d)", heroIdx + 1, preXp, liveXp, gained);
        char fmt[128] = { 0 };
        bool fmtOk = resolveKey(base, "raid_results_hero_resolve_from_quest", fmt, sizeof fmt) && fmt[0];
        if (fmtOk) {                                   // "+%d Resolve XP" — exactly one %d, checked
            int pct = 0; const char* d = nullptr;
            for (const char* p = fmt; *p; p++)
                if (*p == '%') { pct++; d = p + 1; }
            fmtOk = (pct == 1 && d && *d == 'd');
        }
        if (fmtOk) { abStripMarkup(fmt); _snprintf(out, outsz, fmt, gained); }
        else       _snprintf(out, outsz, axs(AXS_RR_XP_GAINED_FMT), gained);
        out[outsz - 1] = 0;
        int lvPre = rrLevelOfXp(base, preXp), lvNow = rrLevelOfXp(base, liveXp);
        if (lvPre >= 0 && lvNow > lvPre) {
            size_t n = strlen(out);
            _snprintf(out + n, outsz - (int)n, " %s", axs(AXS_RR_LEVEL_UP));
            out[outsz - 1] = 0;
        }
        // The bar the game draws under that line, spoken as numbers.
        int into = 0, span = 0; bool maxed = false;
        if (rrXpProgress(base, liveXp, &into, &span, &maxed)) {
            size_t n = strlen(out);
            if (maxed) _snprintf(out + n, outsz - (int)n, " %s", axs(AXS_RR_RANK_MAXED));
            else       { char bar[160];
                         _snprintf(bar, sizeof bar, axs(AXS_RR_XP_TOWARD_FMT), into, span);
                         bar[sizeof bar - 1] = 0;
                         _snprintf(out + n, outsz - (int)n, " %s", bar); }
            out[outsz - 1] = 0;
        }
        return true;
    }

    uintptr_t quirks[RR_MAX_QUIRKS];
    int qn = rrHeroNewQuirks(base, heroIdx, quirks, RR_MAX_QUIRKS);
    int q = row - 2;
    if (q < 0 || q >= qn) return false;

    char tail[128];
    if (qn > 1) _snprintf(tail, sizeof tail, axs(AXS_RR_NEW_QUIRK_N_FMT), q + 1, qn);
    else        _snprintf(tail, sizeof tail, "%s", axs(AXS_RR_NEW_QUIRK));
    tail[sizeof tail - 1] = 0;

    if (!rrHeroRevealed(base, heroIdx)) {
        _snprintf(out, outsz, "%s %s", axs(AXS_RR_QUIRK_HIDDEN), tail);
        out[outsz - 1] = 0;
        return true;
    }
    char body[768] = { 0 };
    if (csQuirkRowFrom(base, quirks[q], true, body, sizeof body)) {
        if (qn > 1) {
            size_t bl = strlen(body);
            const char* gap = (bl && (body[bl - 1] == '.' || body[bl - 1] == '!' ||
                                      body[bl - 1] == '?')) ? " " : ". ";
            _snprintf(out, outsz, "%s%s%s", body, gap, tail);
        } else {
            _snprintf(out, outsz, "%s", body);
        }
    } else {
        _snprintf(out, outsz, "%s", tail);
    }
    out[outsz - 1] = 0;
    return true;
}

static void rrHeroQuirkNames(uintptr_t base, int heroIdx, char* out, int outsz) {
    out[0] = 0;
    uintptr_t quirks[RR_MAX_QUIRKS];
    int qn = rrHeroNewQuirks(base, heroIdx, quirks, RR_MAX_QUIRKS);
    for (int q = 0; q < qn; q++) {
        char id[64], name[128] = { 0 };
        if (csQuirkId(quirks[q], id, sizeof id)) csQuirkName(base, id, name, sizeof name);
        if (!name[0]) continue;
        size_t n = strlen(out);
        _snprintf(out + n, outsz - (int)n, "%s%s.", n ? " " : "", name);
        out[outsz - 1] = 0;
    }
}

static bool rrAnyHiddenQuirks(uintptr_t base) {
    int heroes = rrHeroCount(base);
    for (int h = 0; h < heroes; h++) {
        uintptr_t quirks[RR_MAX_QUIRKS];
        if (rrHeroNewQuirks(base, h, quirks, RR_MAX_QUIRKS) > 0 && !rrHeroRevealed(base, h))
            return true;
    }
    return false;
}

static void rrProbeElements(uintptr_t base, const char* why) {
    if (!axDebugLogEnabled()) return;                // diagnostic: nothing runs with the log off
    uintptr_t begin = 0, end = 0;
    if (!safeReadPtr(base + VEC_BEGIN_RVA, &begin) ||
        !safeReadPtr(base + VEC_END_RVA, &end) || !begin || end <= begin) {
        logLine("rr-probe(%s): focus vector empty", why);
        return;
    }
    uintptr_t count = (end - begin) / ELEM_STRIDE;
    if (count > 4096) count = 4096;
    logLine("rr-probe(%s): %llu elements", why, (unsigned long long)count);
    for (uintptr_t i = 0; i < count; i++) {
        uintptr_t elem = begin + i * ELEM_STRIDE;
        int64_t id = 0; uintptr_t owner = 0;
        if (!safeReadI64(elem + ELEM_ID_OFF, &id) || isNoFocus(id)) continue;
        safeReadPtr(elem + ELEM_OWNER_OFF, &owner);
        uint32_t xb = 0, yb = 0, wb = 0, hb = 0;
        safeReadU32(elem + ELEM_POS_OFF, &xb);  safeReadU32(elem + ELEM_POS_OFF + 4, &yb);
        safeReadU32(elem + ELEM_SIZE_OFF, &wb); safeReadU32(elem + ELEM_SIZE_OFF + 4, &hb);
        uint32_t cc = (uint32_t)(uint64_t)id;
        const char* tag = (cc == (uint32_t)RR_BTN_FWRD) ? " <-- fwrd" :
                          (cc == (uint32_t)RR_BTN_BACK) ? " <-- back" :
                          (cc - RR_MASK_ID_BASE < 32u)  ? " <-- mask" : "";
        logLine("rr-probe(%s): id=0x%llx owner=0x%llx pos=(%.0f,%.0f) size=(%.0f,%.0f)%s",
                why, (unsigned long long)id, (unsigned long long)owner,
                u32AsFloatM(xb), u32AsFloatM(yb), u32AsFloatM(wb), u32AsFloatM(hb), tag);
    }
}

static void rrHeadPrefix(AxStrId first, AxStrId second, char* out, int outsz) {
    if (second != AXS__COUNT) _snprintf(out, outsz, "%s %s ", axs(first), axs(second));
    else                      _snprintf(out, outsz, "%s ", axs(first));
    out[outsz - 1] = 0;
}

static void rrSpeakEntry(uintptr_t base, int state, const char* prefix) {
    char utter[MAILBOX_SZ]; utter[0] = 0;
    char row[MAILBOX_SZ] = { 0 };
    if (state == 2) {
        rrHeroRowText(base, g_rrHero, g_rrHeroRow, row, sizeof row);
        const char* hint = axs(rrAnyHiddenQuirks(base) ? AXS_RR_HINT_REVEAL : AXS_RR_HINT_RETURN);
        _snprintf(utter, sizeof utter, "%s%s %s", prefix, row, hint);
    } else {
        rrPage1RowText(base, g_rrRow, row, sizeof row);
        char sect[128] = { 0 };
        rrPage1SectionName(base, rrPage1RowSection(base, g_rrRow), sect, sizeof sect);
        int questN, treasN, heirN, goldRow;
        int total = rrPage1Layout(base, &questN, &treasN, &heirN, &goldRow);
        int rewards = total - 1 - goldRow;
        char counts[128];
        counts[0] = ' ';
        if (rewards > 0) _snprintf(counts + 1, sizeof counts - 1, axs(AXS_RR_REWARD_COUNT_FMT), rewards);
        else             _snprintf(counts + 1, sizeof counts - 1, "%s", axs(AXS_RR_NO_REWARDS));
        counts[sizeof counts - 1] = 0;
        _snprintf(utter, sizeof utter, "%s%s%s%s%s %s", prefix,
                  sect, sect[0] ? ". " : "", row, g_rrRow == 0 ? counts : "",
                  axs(AXS_RR_HINT_NEXT_PAGE));
    }
    utter[sizeof utter - 1] = 0;
    postSpeech(utter);
}

void rrReannounce(uintptr_t base) {

    int state = rrState(base);
    if (state < 0) return;
    if (bcrIsArena(base)) { bcrReannounce(base); return; }   // a Circus match's results
    logLine("results: re-announcing after a modal closed");
    char head[160];
    rrHeadPrefix(state == 2 ? AXS_RR_HEAD_HEROES : AXS_RR_HEAD_RESULTS, AXS__COUNT,
                 head, sizeof head);
    rrSpeakEntry(base, state, head);
}

void checkRaidResults(uintptr_t base) {

    uintptr_t d = rrDisplay(base);
    if (!d) {
        if (g_rrActive) {
            g_rrActive = false; g_rrAnnounced = false; g_rrLastState = -1;
            g_rrRow = 0; g_rrHero = 0; g_rrHeroRow = 0; g_rrRevealPending = false;
            bcrReset();                            // the Circus module's slice of this screen
            logLine("results: screen gone");
        }
        return;
    }
    if (!g_rrActive) {
        g_rrActive = true; g_rrAnnounced = false; g_rrLastState = -1;
        g_rrRow = 0; g_rrHero = 0; g_rrHeroRow = 0; g_rrRevealPending = false;
        bcrReset();
        g_rrAppearTick = GetTickCount(); g_rrNextOutcomeTry = 0;
        logLine("results: screen appeared, display=%p results=%p state=%d arena=%d",
                (void*)d, (void*)rrResults(base), rrState(base), bcrIsArena(base) ? 1 : 0);
        rrProbeElements(base, "open");
    }

    int state = rrState(base);
    if (state < 0) return;

    if (bcrIsArena(base)) { bcrCheck(base); return; }

    if (!g_rrAnnounced) {
        uint32_t now = GetTickCount();
        if (axIsLoading()) { g_rrAppearTick = now; return; }   // hold while the loading screen has the floor
        if (now < g_rrNextOutcomeTry) return;                  // throttle the resolve attempts
        g_rrNextOutcomeTry = now + 200;
        char probe[256];
        bool haveOutcome = rrOutcomeText(base, probe, sizeof probe);
        if (state != 2 && !haveOutcome && now - g_rrAppearTick < 2500) return;
        g_rrAnnounced = true; g_rrLastState = state;
        logLine("results: announcing entry at state %d (outcome %s)", state,
                haveOutcome ? "resolved" : "FALLBACK");
        rrProbeElements(base, "entry");
        char head[160];
        rrHeadPrefix(AXS_RR_HEAD_RESULTS, state == 2 ? AXS_RR_HEAD_HEROES : AXS__COUNT,
                     head, sizeof head);
        rrSpeakEntry(base, state, head);
        return;
    }

    if (g_rrAnnounced && state != g_rrLastState) {
        int old = g_rrLastState;
        g_rrLastState = state;
        bool wasItems = (old == 0 || old == 1), isItems = (state == 0 || state == 1);
        char head[160];
        if (state == 2 && wasItems) {                  // Next: rewards -> heroes
            g_rrHero = 0; g_rrHeroRow = 0;
            logLine("results: page -> heroes");
            rrProbeElements(base, "heroes");
            rrHeadPrefix(AXS_RR_HEAD_HEROES, AXS__COUNT, head, sizeof head);
            rrSpeakEntry(base, state, head);
        } else if (isItems && old == 2) {              // Back: heroes -> rewards
            logLine("results: page -> rewards");
            rrHeadPrefix(AXS_RR_HEAD_REWARDS, AXS__COUNT, head, sizeof head);
            rrSpeakEntry(base, state, head);
        } else if (!(wasItems && isItems)) {           // circus states etc.: log, say nothing wrong
            logLine("results: state %d -> %d (unhandled pair)", old, state);
        }
    }

    if (g_rrRevealPending) {
        uint32_t word = 0;
        rrRevealWord(base, &word);
        if (word != g_rrRevealMaskAt) {
            g_rrRevealPending = false;
            logLine("results: reveal observed, flags 0x%x -> 0x%x", g_rrRevealMaskAt, word);
            rrProbeElements(base, "reveal");    // ground truth for the mask zones, once
            char utter[MAILBOX_SZ];
            _snprintf(utter, sizeof utter, "%s", axs(AXS_RR_QUIRKS_REVEALED));
            int heroes = rrHeroCount(base);
            for (int h = 0; h < heroes; h++) {
                char names[512];
                rrHeroQuirkNames(base, h, names, sizeof names);
                if (!names[0]) continue;
                uintptr_t rec = rrHeroRecAt(base, h);
                uint32_t rid = 0;
                if (rec) safeReadU32(rec + RR_HREC_ROSTER_ID, &rid);
                char who[80] = { 0 }, cls[80];
                uintptr_t hero = rec ? rrLiveHeroById(base, (int)rid) : 0;
                if (hero) abHeroNameClassOf(base, hero, who, sizeof who, cls, sizeof cls);
                size_t n = strlen(utter);
                if (who[0]) _snprintf(utter + n, sizeof utter - (int)n, " %s: %s", who, names);
                else        _snprintf(utter + n, sizeof utter - (int)n, " %s", names);
                utter[sizeof utter - 1] = 0;
            }
            postSpeech(utter);
        } else if (GetTickCount() > g_rrRevealDeadline) {
            g_rrRevealPending = false;
            logLine("results: reveal watch expired with flags unchanged (0x%x)", word);
        }
    }
}

static bool rrOrphanDialogCancel(uintptr_t base, const char* key) {
    int count = confirmDialogAnswerCount(base);
    if (count <= 0) return false;
    if (count != 2) {
        logLine("results: a %d-answer confirm dialog covers the screen -- no cancel role, %s left alone",
                count, key);
        return false;
    }
    int64_t cancel = (int64_t)(CONFIRM_ANSWER_BASE + 1u);
    if (!feElemOnScreen(cancel)) {
        logLine("results: a 2-answer confirm dialog is reported but its cancel is not on screen");
        return false;
    }
    if (!frontEndClickElementId(cancel)) {
        logLine("results: the orphan dialog's cancel refused the click");
        return false;
    }
    logLine("results: a leftover confirm dialog covers the screen -- %s clicked its cancel answer", key);
    postSpeech(axs(AXS_RR_STALE_DIALOG));
    return true;
}

bool routeResultsKey(uintptr_t base, uint32_t sym, uint16_t mod, uint8_t repeat) {
                     // 29: de-static'd; the AX_RESULTS row itself stays with the context table)
    if (mod & (KMOD_LALT | KMOD_RALT | KMOD_LCTRL | KMOD_RCTRL)) return false;
    int state = rrState(base);
    if (state < 0) return false;                       // screen is going away; the game's key
    if (bcrIsArena(base))                              // a Circus match: its own six-row list
        return bcrRouteKey(base, sym, mod, repeat);
    bool heroesPage = (state == 2);

    if (sym == SDLK_ESCAPE) {
        if (repeat) return true;
        if (!feElemOnScreen(RR_BTN_FWRD) && rrOrphanDialogCancel(base, "Escape")) return true;
        return false;
    }

    if (sym == SDLK_RETURN || sym == SDLK_KP_ENTER) {
        if (repeat) return true;
        if (!feElemOnScreen(RR_BTN_FWRD) && rrOrphanDialogCancel(base, "Enter")) return true;
        if (heroesPage && rrAnyHiddenQuirks(base)) {   // this click will reveal, not leave
            uint32_t word = 0;
            rrRevealWord(base, &word);
            g_rrRevealMaskAt = word;
            g_rrRevealDeadline = GetTickCount() + 3000;
            g_rrRevealPending = true;
        }
        if (!frontEndClickElementId(RR_BTN_FWRD)) {
            g_rrRevealPending = false;
            logLine("results: 'fwrd' not on screen — Enter did nothing");
            postSpeech(axs(AXS_RR_CANT_CONTINUE));
        } else {
            logLine("results: clicked 'fwrd' at state %d", state);
        }
        return true;
    }

    if (!heroesPage) {                                 // page 1: one flat list, all four arrows
        int jump = 0;
        if (axDecodeJump(sym, mod, repeat, &jump)) {
            if (!jump) return true;                  // held jump: one landing per press
            int questN, treasN, heirN, goldRow;
            int total = rrPage1Layout(base, &questN, &treasN, &heirN, &goldRow);
            if (total <= 0) return true;
            int target = (jump > 0) ? total - 1 : 0;
            char buf[MAILBOX_SZ];
            if (!rrPage1RowText(base, target, buf, sizeof buf)) {
                logLine("results: page-1 row %d did not read (of %d)", target, total);
                return true;
            }
            int fromSec = rrPage1RowSection(base, g_rrRow);
            int toSec   = rrPage1RowSection(base, target);
            char sect[128] = { 0 };
            if (toSec != fromSec) rrPage1SectionName(base, toSec, sect, sizeof sect);
            logLine("results jump row %d -> %d (of %d)", g_rrRow, target, total);
            g_rrRow = target;
            if (sect[0]) {
                char utter[MAILBOX_SZ];
                _snprintf(utter, sizeof utter, "%s. %s", sect, buf);
                utter[sizeof utter - 1] = 0;
                postSpeech(utter);
            } else {
                postSpeech(buf);
            }
            return true;
        }

        int dir;
        switch (sym) {
            case SDLK_UP:   case SDLK_LEFT:  dir = -1; break;
            case SDLK_DOWN: case SDLK_RIGHT: dir = +1; break;
            default: return false;
        }
        if (axNavHoldRepeat(repeat)) return true;
        int questN, treasN, heirN, goldRow;
        int total = rrPage1Layout(base, &questN, &treasN, &heirN, &goldRow);
        int target = g_rrRow + dir;
        if (target < 0 || target >= total) target = g_rrRow;   // hard stop: re-read this row
        char buf[MAILBOX_SZ];
        if (!rrPage1RowText(base, target, buf, sizeof buf)) {
            logLine("results: page-1 row %d did not read (of %d)", target, total);
            return true;
        }
        int fromSec = rrPage1RowSection(base, g_rrRow);
        int toSec   = rrPage1RowSection(base, target);
        char sect[128] = { 0 };
        if (toSec != fromSec) rrPage1SectionName(base, toSec, sect, sizeof sect);
        logLine("results nav dir=%+d row %d -> %d (of %d) sec %d -> %d", dir, g_rrRow, target,
                total, fromSec, toSec);
        g_rrRow = target;
        if (sect[0]) {
            char utter[MAILBOX_SZ];
            _snprintf(utter, sizeof utter, "%s. %s", sect, buf);
            utter[sizeof utter - 1] = 0;
            postSpeech(utter);
        } else {
            postSpeech(buf);
        }
        return true;
    }

    {
        int jump = 0;
        if (axDecodeJump(sym, mod, repeat, &jump)) {
            if (!jump) return true;                  // held jump: one landing per press
            int rows = rrHeroRowCount(base, g_rrHero);
            if (rows <= 0) return true;
            int target = (jump > 0) ? rows - 1 : 0;
            char buf[MAILBOX_SZ];
            if (!rrHeroRowText(base, g_rrHero, target, buf, sizeof buf)) {
                logLine("results: hero %d row %d did not read (of %d)", g_rrHero + 1, target, rows);
                return true;
            }
            logLine("results jump hero %d row %d -> %d (of %d)", g_rrHero + 1, g_rrHeroRow, target, rows);
            g_rrHeroRow = target;
            postSpeech(buf);
            return true;
        }
    }
    if (sym == SDLK_LEFT || sym == SDLK_RIGHT) {
        if (axNavHoldRepeat(repeat)) return true;
        int heroes = rrHeroCount(base);
        if (heroes <= 0) return true;
        int target = g_rrHero + (sym == SDLK_RIGHT ? 1 : -1);
        if (target < 0 || target >= heroes) target = g_rrHero;  // hard stop
        g_rrHero = target;
        g_rrHeroRow = 0;                               // land on the header, never unplaced
        char buf[MAILBOX_SZ];
        if (rrHeroRowText(base, g_rrHero, 0, buf, sizeof buf)) postSpeech(buf);
        logLine("results nav hero -> %d (of %d)", g_rrHero + 1, heroes);
        return true;
    }
    if (sym == SDLK_UP || sym == SDLK_DOWN) {
        if (axNavHoldRepeat(repeat)) return true;
        int rows = rrHeroRowCount(base, g_rrHero);
        int target = g_rrHeroRow + (sym == SDLK_DOWN ? 1 : -1);
        if (target < 0 || target >= rows) target = g_rrHeroRow; // hard stop: re-read this row
        char buf[MAILBOX_SZ];
        if (!rrHeroRowText(base, g_rrHero, target, buf, sizeof buf)) {
            logLine("results: hero %d row %d did not read (of %d)", g_rrHero + 1, target, rows);
            return true;
        }
        logLine("results nav hero %d row %d -> %d (of %d)", g_rrHero + 1, g_rrHeroRow, target, rows);
        g_rrHeroRow = target;
        postSpeech(buf);
        return true;
    }
    return false;                                      // everything else stays the game's
}
