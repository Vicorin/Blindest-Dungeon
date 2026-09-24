// dlc/butchers_circus/matchresults.cpp -- THE POST-MATCH RESULTS SCREEN (the arena skin of
// RaidResultsDisplay).

#include <cstdint>
#include <cstring>
#include <cstdio>
#include "internal.h"

// ---- the results snapshot's arena fields (Campaign::RaidFinishResults) ----
static const uintptr_t BCR_RES_WON_OFF    = 0x2f0;  // char: nonzero = the match was won
static const uintptr_t BCR_RES_ARENA_OFF  = 0x3b6;  // char: nonzero = THIS is an arena result
static const uintptr_t BCR_RES_ALLY_BEG   = 0x430;
static const uintptr_t BCR_RES_ALLY_END   = 0x438;
static const uintptr_t BCR_RES_ENEMY_BEG  = 0x450;
static const uintptr_t BCR_RES_ENEMY_END  = 0x458;
static const uintptr_t BCR_REC_STRIDE     = 0x28;
static const uintptr_t BCR_REC_SHOWN_OFF  = 0x05;

// ---- the display-object snapshot the drawers read ----
static const uintptr_t BCR_D_PTS_OLD_OFF  = 0x1d10;  // int: prestige points before the match
static const uintptr_t BCR_D_PTS_NEW_OFF  = 0x1d14;  // int: ... after; -1 UNTIL THE DATA ARRIVES
static const uintptr_t BCR_D_NEXT_NEW_OFF = 0x1d1c;  // int: points needed for the next level (new)
static const uintptr_t BCR_D_LVL_OLD_OFF  = 0x1d28;  // int: prestige level before
static const uintptr_t BCR_D_LVL_NEW_OFF  = 0x1d2c;  // int: ... after
static const uintptr_t BCR_D_MAXED_OFF    = 0x1d38;  // bool: the ladder's top
static const uintptr_t BCR_D_LEAGUE_OFF   = 0x1be8;  // char[0x20]: league display name (verbatim
                                                     // from the rankings building; drawn unresolved)
static const uintptr_t BCR_D_TIER_OFF     = 0x1c08;  // char[0x20]: "I" / "II" / "III"
static const uintptr_t BCR_D_RANKKEY_OFF  = 0x1d08;  // char*: the game's own chosen
static const uintptr_t BCR_D_SK_OLD_CUR_OFF = 0x1af8; // int: skulls lit BEFORE the match
static const uintptr_t BCR_D_SK_OLD_ADV_OFF = 0x1afc; // int: ... of how many (steps to advance)
static const uintptr_t BCR_D_SK_NEW_CUR_OFF = 0x1b00; // int: skulls lit AFTER (PlayFab's answer)
static const uintptr_t BCR_D_SK_NEW_ADV_OFF = 0x1b04; // int: ... of how many
static const int       BCR_STR_INLINE_CAP = 0x20;

// ---- the prize booth's reward-level records ----
static const int       BCR_MAX_LEVELS_UP   = 8;      // per-match level-up cap on the walk
static const int       BCR_MAX_REWARDS     = 16;     // spoken-reward cap

static const int64_t   BCR_BTN_FWRD        = 0x66777264; // 'fwrd' -- the campaign screen's id family

enum { BCR_ROW_OUTCOME = 0, BCR_ROW_PRESTIGE, BCR_ROW_NEXT, BCR_ROW_RANK, BCR_ROW_STEPS,
       BCR_ROW_REWARDS, BCR_ROW__FIXED };

static bool g_bcrAnnounced = false;   // the entry line was spoken
static bool g_bcrDataSeen  = false;
static int  g_bcrLastState = -1;      // last state seen, to announce page CHANGES
static int  g_bcrRow       = 0;       // the row cursor

void bcrReset() {                     // screen gone -- results.cpp's watch calls this
    g_bcrAnnounced = false; g_bcrDataSeen = false; g_bcrLastState = -1; g_bcrRow = 0;
}

bool bcrIsArena(uintptr_t base) {
    uintptr_t results = rrResults(base);
    if (!results) return false;
    uint8_t arena = 0;
    return safeReadU8(results + BCR_RES_ARENA_OFF, &arena) && arena != 0;
}

static bool bcrWon(uintptr_t base) {
    uintptr_t results = rrResults(base);
    uint8_t won = 0;
    return results && safeReadU8(results + BCR_RES_WON_OFF, &won) && won != 0;
}

uintptr_t bcrBuildingByHash(uintptr_t base, uint32_t want) {
    uintptr_t camp = 0, head = 0, node = 0, best = 0;
    if (!safeReadPtr(base + RES_CAMPAIGN_RVA, &camp) || camp <= 0x10000) return 0;
    if (!safeReadPtr(camp + BCR_CAMP_BLDMAP_OFF, &head) || head <= 0x10000) return 0;
    if (!safeReadPtr(head + 8, &node) || node <= 0x10000) return 0;
    best = head;
    for (int guard = 0; guard < 64; guard++) {
        uint8_t isNil = 0;
        if (!safeReadU8(node + BCR_MAPNODE_ISNIL, &isNil) || isNil) break;
        uint32_t key = 0;
        if (!safeReadU32(node + BCR_MAPNODE_KEY, &key)) return 0;
        uintptr_t next = 0;
        if (key < want) {
            if (!safeReadPtr(node + BCR_MAPNODE_RIGHT, &next)) return 0;
        } else {
            best = node;
            if (!safeReadPtr(node + BCR_MAPNODE_LEFT, &next)) return 0;
        }
        if (next <= 0x10000) break;
        node = next;
    }
    if (best == head) return 0;
    uint8_t isNil = 0; uint32_t key = 0;
    if (!safeReadU8(best + BCR_MAPNODE_ISNIL, &isNil) || isNil) return 0;
    if (!safeReadU32(best + BCR_MAPNODE_KEY, &key) || key != want) return 0;
    uintptr_t val = 0;
    if (!safeReadPtr(best + BCR_MAPNODE_VALUE, &val) || val <= 0x10000) return 0;
    return val;
}

// ---- format-string guards ----
static bool bcrFmtOneInt(const char* fmt) {
    int pct = 0; bool ok = false;
    for (const char* p = fmt; *p; p++) {
        if (*p != '%') continue;
        pct++;
        const char* d = p + 1;
        if (*d == '+') d++;
        ok = (*d == 'd' || *d == 'u');
    }
    return pct == 1 && ok;
}
static bool bcrFmtTwoStr(const char* fmt) {
    int pct = 0, s = 0;
    for (const char* p = fmt; *p; p++)
        if (*p == '%') { pct++; if (p[1] == 's') s++; }
    return pct == 2 && s == 2;
}

static void bcrIntLine(uintptr_t base, const char* key, int value, char* out, int outsz) {
    char fmt[128] = { 0 };
    if (resolveKey(base, key, fmt, sizeof fmt) && fmt[0] && bcrFmtOneInt(fmt)) {
        abStripMarkup(fmt);
        _snprintf(out, outsz, fmt, value);
    } else {
        logLine("bcr: key \"%s\" missing or not a one-int format -- speaking the bare number", key);
        _snprintf(out, outsz, "%d", value);
    }
    out[outsz - 1] = 0;
}

// ---- the row texts ----
static bool bcrPrestigeData(uintptr_t base, int* ptsOld, int* ptsNew, int* nextNew,
                            int* lvlOld, int* lvlNew, bool* maxed) {
    uintptr_t d = rrDisplay(base);
    if (!d) return false;
    uint32_t po = 0, pn = 0, nn = 0, lo = 0, ln = 0; uint8_t mx = 0;
    if (!safeReadU32(d + BCR_D_PTS_OLD_OFF, &po) || !safeReadU32(d + BCR_D_PTS_NEW_OFF, &pn) ||
        !safeReadU32(d + BCR_D_NEXT_NEW_OFF, &nn) || !safeReadU32(d + BCR_D_LVL_OLD_OFF, &lo) ||
        !safeReadU32(d + BCR_D_LVL_NEW_OFF, &ln)) return false;
    safeReadU8(d + BCR_D_MAXED_OFF, &mx);
    *ptsOld = (int)po; *ptsNew = (int)pn; *nextNew = (int)nn;
    *lvlOld = (int)lo; *lvlNew = (int)ln; *maxed = mx != 0;
    return true;
}

static bool bcrHasData(uintptr_t base) {
    int po, pn, nn, lo, ln; bool mx;
    return bcrPrestigeData(base, &po, &pn, &nn, &lo, &ln, &mx) && pn != -1;
}

static void bcrOutcomeText(uintptr_t base, char* out, int outsz) {
    const char* key = bcrWon(base) ? "raid_results_quest_result_was_completed"
                                   : "raid_results_quest_result_was_not_completed_defeat";
    if (!resolveKey(base, key, out, outsz) || !out[0]) {
        logLine("bcr: outcome key \"%s\" did not resolve", key);
        _snprintf(out, outsz, "%s", axs(bcrWon(base) ? AXS_BCR_WON_FALLBACK : AXS_BCR_LOST_FALLBACK));
    }
    abStripMarkup(out);
    out[outsz - 1] = 0;
}

// Row 1: "+N pts. Lvl L." (+ "Prestige level up!" when the level moved).
static void bcrPrestigeText(uintptr_t base, char* out, int outsz) {
    int po, pn, nn, lo, ln; bool mx;
    if (!bcrPrestigeData(base, &po, &pn, &nn, &lo, &ln, &mx) || pn == -1) {
        _snprintf(out, outsz, "%s", axs(AXS_BCR_WAITING));
        out[outsz - 1] = 0;
        return;
    }
    char pts[128], lvl[128];
    bcrIntLine(base, "str_results_prestige_points_format", pn - po, pts, sizeof pts);
    bcrIntLine(base, "str_results_prestige_reward_level_format", ln, lvl, sizeof lvl);
    _snprintf(out, outsz, "%s. %s.%s%s", pts, lvl,
              ln > lo ? " " : "", ln > lo ? axs(AXS_BCR_LEVEL_UP) : "");
    out[outsz - 1] = 0;
}

static void bcrNextLevelText(uintptr_t base, char* out, int outsz) {
    int po, pn, nn, lo, ln; bool mx;
    if (!bcrPrestigeData(base, &po, &pn, &nn, &lo, &ln, &mx) || pn == -1) {
        _snprintf(out, outsz, "%s", axs(AXS_BCR_WAITING));
        out[outsz - 1] = 0;
        return;
    }
    if (mx) {
        if (resolveKey(base, "str_results_prestige_reward_max_level", out, outsz) && out[0]) {
            abStripMarkup(out); out[outsz - 1] = 0; return;
        }
    }
    bcrIntLine(base, "str_results_prestige_reward_next_level_format", nn - pn, out, outsz);
}

static void bcrRankText(uintptr_t base, char* out, int outsz) {
    uintptr_t d = rrDisplay(base);
    char league[BCR_STR_INLINE_CAP + 1] = { 0 }, tier[BCR_STR_INLINE_CAP + 1] = { 0 };
    if (d) {
        safeReadCStr(d + BCR_D_LEAGUE_OFF, league, sizeof league);
        safeReadCStr(d + BCR_D_TIER_OFF, tier, sizeof tier);
    }
    if (!league[0]) {
        _snprintf(out, outsz, "%s", axs(AXS_BCR_WAITING));
        out[outsz - 1] = 0;
        return;
    }
    char head[128] = { 0 };
    uintptr_t keyPtr = 0;
    if (safeReadPtr(d + BCR_D_RANKKEY_OFF, &keyPtr) && keyPtr > 0x10000) {
        char key[64] = { 0 };
        if (safeReadCStr(keyPtr, key, sizeof key) &&
            strncmp(key, "str_results_rank_", 17) == 0 && strlen(key) < 32) {
            if (resolveKey(base, key, head, sizeof head)) abStripMarkup(head);
            else head[0] = 0;
        }
    }
    char fmt[128] = { 0 }, title[192];
    if (resolveKey(base, "str_results_rank_title_format", fmt, sizeof fmt) && fmt[0] &&
        bcrFmtTwoStr(fmt)) {
        abStripMarkup(fmt);
        _snprintf(title, sizeof title, fmt, league, tier);
    } else {
        _snprintf(title, sizeof title, "%s %s", league, tier);
    }
    title[sizeof title - 1] = 0;
    if (head[0]) _snprintf(out, outsz, "%s %s.", head, title);
    else         _snprintf(out, outsz, "%s.", title);
    out[outsz - 1] = 0;
}

static void bcrStepsText(uintptr_t base, char* out, int outsz) {
    uintptr_t d = rrDisplay(base);
    uint32_t oc = 0, oa = 0, nc = 0, na = 0;
    if (!d || !bcrHasData(base) ||
        !safeReadU32(d + BCR_D_SK_OLD_CUR_OFF, &oc) || !safeReadU32(d + BCR_D_SK_OLD_ADV_OFF, &oa) ||
        !safeReadU32(d + BCR_D_SK_NEW_CUR_OFF, &nc) || !safeReadU32(d + BCR_D_SK_NEW_ADV_OFF, &na)) {
        _snprintf(out, outsz, "%s", axs(AXS_BCR_WAITING));
        out[outsz - 1] = 0;
        return;
    }
    if (nc == oc && na == oa) _snprintf(out, outsz, axs(AXS_BCR_STEPS_SAME_FMT), (int)nc, (int)na);
    else _snprintf(out, outsz, axs(AXS_BCR_STEPS_CHANGED_FMT), (int)nc, (int)na, (int)oc, (int)oa);
    out[outsz - 1] = 0;
}

bool bcrTrinketName(uintptr_t base, uint32_t idHash, char* out, int outsz) {
    char id[64] = { 0 };
    if (!bldTrinketIdByHash(base, idHash, id, sizeof id)) {
        logLine("bcr: no ItemClass record for trinket hash 0x%x", idHash);
        return false;
    }
    char key[128];
    _snprintf(key, sizeof key, "str_inventory_title_trinket%s", id);
    key[sizeof key - 1] = 0;
    if (!resolveKey(base, key, out, outsz) || !out[0]) {
        logLine("bcr: trinket title key \"%s\" did not resolve", key);
        _snprintf(out, outsz, "%s", id);
    }
    abStripMarkup(out);
    out[outsz - 1] = 0;
    return true;
}

bool bcrBannerName(uintptr_t base, uintptr_t entry, char* out, int outsz) {
    char name[0x21] = { 0 };
    if (!safeReadCStr(entry + BCR_RC_BANNER_NAME, name, sizeof name) || !name[0]) return false;
    char key[128];
    _snprintf(key, sizeof key, "banner_piece_%s", name);
    key[sizeof key - 1] = 0;
    if (!resolveKey(base, key, out, outsz) || !out[0]) {
        logLine("bcr: banner key \"%s\" did not resolve -- speaking the raw name", key);
        _snprintf(out, outsz, "%s", name);
    }
    abStripMarkup(out);
    out[outsz - 1] = 0;
    return true;
}

struct BcrReward { int kind; uintptr_t entry; };
static int bcrCollectRewards(uintptr_t base, BcrReward* out, int max) {
    int po, pn, nn, lo, ln; bool mx;
    if (!bcrPrestigeData(base, &po, &pn, &nn, &lo, &ln, &mx) || pn == -1 || ln <= lo) return 0;
    uintptr_t booth = bcrBuildingByHash(base, resHash("prize_booth"));
    if (!booth) { logLine("bcr: prize_booth not in the building map"); return 0; }
    uintptr_t beg = 0, end = 0;
    if (!safeReadPtr(booth + BCR_PB_LVLVEC_BEG, &beg) ||
        !safeReadPtr(booth + BCR_PB_LVLVEC_END, &end) || beg <= 0x10000 || end < beg) return 0;
    int levels = (int)((end - beg) / BCR_PB_LVLREC_STRIDE);
    int n = 0;
    for (int lvl = lo + 1; lvl <= ln && lvl - lo <= BCR_MAX_LEVELS_UP; lvl++) {
        if (lvl < 1 || lvl > levels) { logLine("bcr: level %d outside the %d reward records", lvl, levels); continue; }
        uintptr_t rec = beg + (uintptr_t)(lvl - 1) * BCR_PB_LVLREC_STRIDE;
        uintptr_t tb = 0, te = 0, bb = 0, be = 0;
        safeReadPtr(rec + BCR_RC_TRINK_BEG, &tb);  safeReadPtr(rec + BCR_RC_TRINK_END, &te);
        safeReadPtr(rec + BCR_RC_BANNER_BEG, &bb); safeReadPtr(rec + BCR_RC_BANNER_END, &be);
        if (tb > 0x10000 && te >= tb && (te - tb) / BCR_RC_TRINK_STRIDE <= 8) {
            for (uintptr_t e = tb; e < te && n < max; e += BCR_RC_TRINK_STRIDE) {
                uint32_t hash = 0;
                if (!safeReadU32(e + BCR_RC_TRINK_HASH, &hash) || !hash) continue;
                out[n].kind = 1; out[n].entry = e; n++;
            }
        }
        if (bb > 0x10000 && be >= bb && (be - bb) / BCR_RC_BANNER_STRIDE <= 8) {
            for (uintptr_t e = bb; e < be && n < max; e += BCR_RC_BANNER_STRIDE) {
                out[n].kind = 2; out[n].entry = e; n++;
            }
        }
    }
    return n;
}

static int bcrRewardNames(uintptr_t base, char* out, int outsz) {
    out[0] = 0;
    BcrReward rw[BCR_MAX_REWARDS];
    int n = bcrCollectRewards(base, rw, BCR_MAX_REWARDS), named = 0;
    for (int i = 0; i < n; i++) {
        char name[192];
        bool ok;
        if (rw[i].kind == 1) {
            uint32_t hash = 0;
            ok = safeReadU32(rw[i].entry + BCR_RC_TRINK_HASH, &hash) && hash &&
                 bcrTrinketName(base, hash, name, sizeof name);
        } else {
            ok = bcrBannerName(base, rw[i].entry, name, sizeof name);
        }
        if (!ok) continue;
        size_t len = strlen(out);
        _snprintf(out + len, outsz - (int)len, "%s%s.", len ? " " : "", name);
        out[outsz - 1] = 0;
        named++;
    }
    return named;
}

static bool bcrRewardRowText(uintptr_t base, int idx, char* out, int outsz) {
    BcrReward rw[BCR_MAX_REWARDS];
    int n = bcrCollectRewards(base, rw, BCR_MAX_REWARDS);
    if (idx < 0 || idx >= n) return false;
    return pbRewardCard(base, rw[idx].kind, rw[idx].entry, idx, n, false, out, outsz);
}

static int bcrRewardCount(uintptr_t base) {
    BcrReward rw[BCR_MAX_REWARDS];
    return bcrCollectRewards(base, rw, BCR_MAX_REWARDS);
}
static int bcrRowCount(uintptr_t base) { return BCR_ROW__FIXED + bcrRewardCount(base) + 1; }

static void bcrRewardsText(uintptr_t base, char* out, int outsz) {
    if (!bcrHasData(base)) {
        _snprintf(out, outsz, "%s", axs(AXS_BCR_WAITING));
        out[outsz - 1] = 0;
        return;
    }
    char names[MAILBOX_SZ];
    int n = bcrRewardNames(base, names, sizeof names);
    char title[128] = { 0 };
    if (!resolveKey(base, "str_results_prestige_rewards", title, sizeof title) || !title[0])
        _snprintf(title, sizeof title, "%s", axs(AXS_BCR_REWARDS_FALLBACK));
    abStripMarkup(title);
    title[sizeof title - 1] = 0;
    if (n > 0) _snprintf(out, outsz, "%s %s", title, names);
    else       _snprintf(out, outsz, "%s", axs(AXS_BCR_NO_REWARDS));
    out[outsz - 1] = 0;
}

static void bcrContinueText(uintptr_t base, int state, char* out, int outsz) {
    if (state == 9 && g_bcrDataSeen) {
        char cont[128] = { 0 };
        if (resolveKey(base, "str_results_continue", cont, sizeof cont) && cont[0])
            abStripMarkup(cont);
        else cont[0] = 0;
        _snprintf(out, outsz, "%s%s%s", cont, cont[0] ? " " : "", axs(AXS_BCR_HINT_CONTINUE));
    } else {
        _snprintf(out, outsz, "%s", axs(AXS_BCR_WAITING));
    }
    out[outsz - 1] = 0;
}

static bool bcrRowText(uintptr_t base, int row, char* out, int outsz) {
    switch (row) {
        case BCR_ROW_OUTCOME:  bcrOutcomeText(base, out, outsz);  return true;
        case BCR_ROW_PRESTIGE: bcrPrestigeText(base, out, outsz); return true;
        case BCR_ROW_NEXT:     bcrNextLevelText(base, out, outsz);return true;
        case BCR_ROW_RANK:     bcrRankText(base, out, outsz);     return true;
        case BCR_ROW_STEPS:    bcrStepsText(base, out, outsz);    return true;
        case BCR_ROW_REWARDS:  bcrRewardsText(base, out, outsz);  return true;
    }
    if (row < 0) return false;
    int rewards = bcrRewardCount(base);
    if (row < BCR_ROW__FIXED + rewards) return bcrRewardRowText(base, row - BCR_ROW__FIXED, out, outsz);
    if (row == BCR_ROW__FIXED + rewards) { bcrContinueText(base, rrState(base), out, outsz); return true; }
    return false;
}

void bcrReannounce(uintptr_t base) {
    char row[MAILBOX_SZ];
    if (!bcrRowText(base, g_bcrRow, row, sizeof row)) row[0] = 0;
    char outcome[128] = { 0 };
    if (g_bcrRow != BCR_ROW_OUTCOME) bcrOutcomeText(base, outcome, sizeof outcome);
    char utter[MAILBOX_SZ];
    _snprintf(utter, sizeof utter, "%s %s%s%s", axs(AXS_BCR_HEAD),
              outcome, outcome[0] ? " " : "", row);
    utter[sizeof utter - 1] = 0;
    postSpeech(utter);
    logLine("bcr: re-announcing after a modal closed (row %d)", g_bcrRow);
}

void bcrCheck(uintptr_t base) {
    int state = rrState(base);
    if (state < 0) return;

    if (!g_bcrAnnounced) {
        if (axIsLoading()) return;                    // the loading reader owns this moment
        g_bcrAnnounced = true;
        g_bcrLastState = state;
        char outcome[128];
        bcrOutcomeText(base, outcome, sizeof outcome);
        char utter[MAILBOX_SZ];
        char pres[256];
        bcrPrestigeText(base, pres, sizeof pres);
        _snprintf(utter, sizeof utter, "%s %s %s", axs(AXS_BCR_HEAD), outcome, pres);
        utter[sizeof utter - 1] = 0;
        postSpeech(utter);
        g_bcrDataSeen = bcrHasData(base);
        logLine("bcr: entry announced (state %d, won=%d, data=%d)",
                state, bcrWon(base) ? 1 : 0, g_bcrDataSeen ? 1 : 0);
        return;
    }

    if (!g_bcrDataSeen && bcrHasData(base)) {
        g_bcrDataSeen = true;
        char pres[256], next[256], steps[256];
        bcrPrestigeText(base, pres, sizeof pres);
        bcrNextLevelText(base, next, sizeof next);
        bcrStepsText(base, steps, sizeof steps);
        char utter[MAILBOX_SZ];
        _snprintf(utter, sizeof utter, "%s %s %s", pres, next, steps);
        utter[sizeof utter - 1] = 0;
        postSpeech(utter);
        logLine("bcr: prestige data landed at state %d", rrState(base));
    }

    if (state != g_bcrLastState) {
        int old = g_bcrLastState;
        g_bcrLastState = state;
        logLine("bcr: state %d -> %d", old, state);
        char utter[MAILBOX_SZ];
        if (state == 6) {                              // the NEW RANK page (+ the steps it sits on)
            char rank[MAILBOX_SZ], steps[256];
            bcrRankText(base, rank, sizeof rank);
            bcrStepsText(base, steps, sizeof steps);
            _snprintf(utter, sizeof utter, "%s %s", rank, steps);
            utter[sizeof utter - 1] = 0;
            postSpeech(utter);
        } else if (state == 8) {                       // the reward reveal
            char rewards[MAILBOX_SZ];
            bcrRewardsText(base, rewards, sizeof rewards);
            postSpeech(rewards);
        } else if (state == 9 && old >= 5) {           // settled after the sequence
            bcrContinueText(base, state, utter, sizeof utter);
            postSpeech(utter);
        }
    }
}

bool bcrRouteKey(uintptr_t base, uint32_t sym, uint16_t mod, uint8_t repeat) {
    if (mod & (KMOD_LALT | KMOD_RALT | KMOD_LCTRL | KMOD_RCTRL)) return false;
    int state = rrState(base);
    if (state < 0) return false;
    const int rows = bcrRowCount(base);
    if (g_bcrRow >= rows) g_bcrRow = rows - 1;
    if (g_bcrRow < 0) g_bcrRow = 0;

    if (sym == SDLK_RETURN || sym == SDLK_KP_ENTER) {
        if (repeat) return true;
        if (!frontEndClickElementId(BCR_BTN_FWRD)) {
            logLine("bcr: 'fwrd' not on screen at state %d -- Enter did nothing", state);
            postSpeech(axs(AXS_RR_CANT_CONTINUE));
        } else {
            logLine("bcr: clicked 'fwrd' at state %d", state);
        }
        return true;
    }

    int jump = 0;
    if (axDecodeJump(sym, mod, repeat, &jump)) {
        if (!jump) return true;                        // held jump: one landing per press
        int target = (jump > 0) ? rows - 1 : 0;
        char buf[MAILBOX_SZ];
        if (!bcrRowText(base, target, buf, sizeof buf)) return true;
        logLine("bcr: jump row %d -> %d", g_bcrRow, target);
        g_bcrRow = target;
        postSpeech(buf);
        return true;
    }

    int dir;
    switch (sym) {
        case SDLK_UP:   case SDLK_LEFT:  dir = -1; break;
        case SDLK_DOWN: case SDLK_RIGHT: dir = +1; break;
        default: return false;
    }
    if (axNavHoldRepeat(repeat)) return true;          // held-arrow walk: pure nav
    int target = g_bcrRow + dir;
    if (target < 0 || target >= rows) target = g_bcrRow;   // hard stop: re-read
    char buf[MAILBOX_SZ];
    if (!bcrRowText(base, target, buf, sizeof buf)) {
        logLine("bcr: row %d did not read", target);
        return true;
    }
    logLine("bcr: nav row %d -> %d", g_bcrRow, target);
    g_bcrRow = target;
    postSpeech(buf);
    return true;
}
