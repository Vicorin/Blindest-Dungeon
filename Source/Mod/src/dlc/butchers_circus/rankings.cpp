// dlc/butchers_circus/rankings.cpp -- THE RANKING BOARD (the Circus's `rankings` building

#include <cstdint>
#include <cstring>
#include <cstdio>
#include <ctime>
#include <windows.h>
#include "internal.h"

// ---- identity ----

// ---- the panel (RankingsDisplay) ----
static const uintptr_t RB_P_SECTION_OFF   = 0x0a0;  // int: the current section (0/1)
static const uintptr_t RB_P_BLD_OFF       = 0x260;  // Building::Rankings*
static const uintptr_t RB_P_CELLVEC_BEG   = 0x3b8;  // vector<Widget*> begin -- the ONE outer cell
static const uintptr_t RB_P_CELLVEC_END   = 0x3c0;
static const uintptr_t RB_P_ARC_OFF       = 0x71c;  // float: the ring's arc
static const uintptr_t RB_P_SELROW_OFF    = 0x718;  // int: the controller's selected row
static const uintptr_t RB_P_BUILT_OFF     = 0x720;  // byte: the list has been built
static const uintptr_t RB_P_GAMEPAD_OFF   = 0x721;  // byte: a gamepad is live
static const uintptr_t RB_P_INPUTMODE_OFF = 0x722;  // byte: the input-mode snapshot
static const int       RB_SECTIONS        = 2;      // str_rankings_section_0 / _1; the game's cap

// ---- the building (Building::Rankings) ----
static const uintptr_t RB_B_DATA_IN_OFF   = 0x0a8;  // byte: rank data received
static const uintptr_t RB_B_HIST_BUSY_OFF = 0x0ac;  // byte: a match-history request is in flight
static const uintptr_t RB_B_NEWS_BUSY_OFF = 0x0ad;  // byte: a news request is in flight
static const uintptr_t RB_B_MATCHVEC_OFF  = 0x100;  // + section * 0x18: vector begin, +8 end
static const uintptr_t RB_B_MATCHVEC_STRIDE = 0x18;
static const uintptr_t RB_B_SEASON_END_OFF= 0x228;  // time_t
static const uintptr_t RB_B_LEAGUE_PREV   = 0x280;
static const uintptr_t RB_B_LEAGUE_CUR    = 0x2d0;
static const uintptr_t RB_B_LEAGUE_NEXT   = 0x320;
static const uintptr_t RB_B_LEAGUE_BEST   = 0x370;
static const uintptr_t RB_L_ID_OFF        = 0x00;   // league block: id char[0x20]
static const uintptr_t RB_L_NAME_OFF      = 0x20;   //   display-name char* (resolved text)
static const uintptr_t RB_L_TIER_OFF      = 0x44;   //   tier int 0..2 (III..I)
static const uintptr_t RB_L_STEPS_OFF     = 0x48;   //   steps_to_advance for that league
static const int       RB_L_ID_CAP        = 0x20;
static const uintptr_t RB_B_STEPS_TO_ADV_OFF = 0x410;
static const uintptr_t RB_B_CUR_STEPS_OFF = 0x414;
static const uintptr_t RB_B_TIERS_OFF     = 0x424;  // highest_number_of_tiers
static const uintptr_t RB_B_SEASON_OFF    = 0x428;  // current_season
static const uintptr_t RB_B_HAS_NEXT_OFF  = 0x484;  // byte: a next league exists

// ---- a match record (PlayFabAnalytics::API::MatchRecord) ----
static const uintptr_t RB_REC_STRIDE      = 0x80;
static const uintptr_t RB_REC_PLAYER_VEC  = 0x00;   // vector<HeroEntry> begin (+8 end)
static const uintptr_t RB_REC_OPP_VEC     = 0x18;
static const uintptr_t RB_REC_TIME_OFF    = 0x30;   // time_t
static const uintptr_t RB_REC_OPPNAME_OFF = 0x38;   // char[0x40]
static const int       RB_REC_OPPNAME_CAP = 0x40;
static const uintptr_t RB_REC_WON_OFF     = 0x78;   // byte: nonzero = won
static const uintptr_t RB_HERO_STRIDE     = 8;      // HeroEntry: +0 u32 class hash, +4 byte dead
static const uintptr_t RB_HERO_HASH_OFF   = 0;
static const uintptr_t RB_HERO_DEAD_OFF   = 4;
static const int       RB_MAX_MATCHES     = 64;     // a guard, not a table
static const int       RB_MAX_HEROES      = 8;      // four a side shipped; a guard
static const uintptr_t RB_MP_NAME_OFF     = 0x80;   // MP_API + 0x80: the Steam persona, inline

// ---- the row list ----
enum { RB_ROW_CURRENT = 0, RB_ROW_NEXT, RB_ROW_STEPS, RB_ROW_BEST, RB_ROW_SEASON, RB_ROW_SECTION,
       RB_ROW_HEADERS };                            // then RB_ROW_HEADERS + i = match i

// ---- state ----
static int   g_rbRow        = 0;      // the cursor
static bool  g_rbLoadPending= false;
static int   g_rbLastCount  = -1;
static int   g_rbSwitchTo   = -1;     // a tab click is in flight, aimed at this section
static DWORD g_rbSwitchUntil= 0;      // ... and its deadline

bool rbIsPanel(uintptr_t base, uintptr_t panel) {
    uintptr_t vft = 0;
    if (!panel || !safeReadPtr(panel, &vft) || vft <= base) return false;
    return vft - base == RB_VFT_RVA;
}

void rbReset() {
    g_rbRow = 0; g_rbLoadPending = false; g_rbLastCount = -1; g_rbSwitchTo = -1; g_rbSwitchUntil = 0;
}

static uintptr_t rbBuilding(uintptr_t base) {
    return bcrBuildingByHash(base, resHash("rankings"));
}

static int rbSection(uintptr_t panel) {
    uint32_t s = 0;
    if (!safeReadU32(panel + RB_P_SECTION_OFF, &s) || s >= (uint32_t)RB_SECTIONS) return -1;
    return (int)s;
}

// The section's match vector: begin/end and the record count (-1 = unreadable).
static int rbMatchVec(uintptr_t bld, int section, uintptr_t* begOut) {
    if (!bld || section < 0 || section >= RB_SECTIONS) return -1;
    uintptr_t at = bld + RB_B_MATCHVEC_OFF + (uintptr_t)section * RB_B_MATCHVEC_STRIDE;
    uintptr_t beg = 0, end = 0;
    if (!safeReadPtr(at, &beg) || !safeReadPtr(at + 8, &end)) return -1;
    if (beg == 0 && end == 0) { if (begOut) *begOut = 0; return 0; }   // an empty vector reads live too
    if (beg <= 0x10000 || end < beg || (end - beg) % RB_REC_STRIDE) return -1;
    int n = (int)((end - beg) / RB_REC_STRIDE);
    if (n > RB_MAX_MATCHES) n = RB_MAX_MATCHES;
    if (begOut) *begOut = beg;
    return n;
}

// Record i as DRAWN: row 0 is the LAST record (BuildRows walks count-1 down to 0).
static uintptr_t rbRecordAt(uintptr_t beg, int n, int row) {
    if (!beg || row < 0 || row >= n) return 0;
    return beg + (uintptr_t)(n - 1 - row) * RB_REC_STRIDE;
}

// ---- text: the league blocks ----
static const char* rbTierWord(uint32_t tier) {
    switch (tier) { case 0: return "III"; case 1: return "II"; case 2: return "I"; }
    return nullptr;
}

static bool rbLeagueText(uintptr_t base, uintptr_t block, char* out, int outsz) {
    out[0] = 0;
    if (!block) return false;
    char name[128] = { 0 }, id[RB_L_ID_CAP + 1] = { 0 };
    uintptr_t namep = 0;
    uint32_t tier = 0xffffffff;
    safeReadCStr(block + RB_L_ID_OFF, id, sizeof id);
    if (safeReadPtr(block + RB_L_NAME_OFF, &namep) && namep > 0x10000) safeReadCStr(namep, name, sizeof name);
    if (!name[0] && id[0]) {
        char key[96];
        _snprintf(key, sizeof key, "str_league_%s", id);
        key[sizeof key - 1] = 0;
        if (!resolveKey(base, key, name, sizeof name) || !name[0]) {
            logLine("rankings: league display name null and \"%s\" did not resolve -- speaking the id", key);
            _snprintf(name, sizeof name, "%s", id);
        }
    }
    if (!name[0]) return false;
    abStripMarkup(name);
    safeReadU32(block + RB_L_TIER_OFF, &tier);
    const char* tw = rbTierWord(tier);
    if (!tw) logLine("rankings: league \"%s\" tier %u is outside 0..2", id, tier);
    _snprintf(out, outsz, "%s%s%s", name, tw ? " " : "", tw ? tw : "");
    out[outsz - 1] = 0;
    return true;
}

// "%s %s" guard: the two rank formats take exactly two strings.
static bool rbFmtTwoStr(const char* fmt) {
    int pct = 0, ok = 0;
    for (const char* p = fmt; *p; p++) if (*p == '%') { pct++; if (p[1] == 's') ok++; }
    return pct == 2 && ok == 2;
}
static bool rbFmtOneStr(const char* fmt) {
    int pct = 0, ok = 0;
    for (const char* p = fmt; *p; p++) if (*p == '%') { pct++; if (p[1] == 's') ok++; }
    return pct == 1 && ok == 1;
}
static bool rbFmtInts(const char* fmt, int want) {
    int pct = 0, ok = 0;
    for (const char* p = fmt; *p; p++) if (*p == '%') { pct++; if (p[1] == 'd' || p[1] == 'u') ok++; }
    return pct == want && ok == want;
}

static bool rbRankLine(uintptr_t base, const char* key, uintptr_t block, char* out, int outsz) {
    char name[128] = { 0 }, id[RB_L_ID_CAP + 1] = { 0 };
    uintptr_t namep = 0; uint32_t tier = 0xffffffff;
    if (!block) return false;
    safeReadCStr(block + RB_L_ID_OFF, id, sizeof id);
    if (safeReadPtr(block + RB_L_NAME_OFF, &namep) && namep > 0x10000) safeReadCStr(namep, name, sizeof name);
    if (!name[0] && id[0]) {
        char k[96];
        _snprintf(k, sizeof k, "str_league_%s", id); k[sizeof k - 1] = 0;
        if (!resolveKey(base, k, name, sizeof name) || !name[0]) _snprintf(name, sizeof name, "%s", id);
    }
    if (!name[0]) return false;
    abStripMarkup(name);
    safeReadU32(block + RB_L_TIER_OFF, &tier);
    const char* tw = rbTierWord(tier);
    char fmt[192] = { 0 };
    if (resolveKey(base, key, fmt, sizeof fmt) && fmt[0] && rbFmtTwoStr(fmt)) {
        abStripMarkup(fmt);
        _snprintf(out, outsz, fmt, name, tw ? tw : "");
    } else {
        logLine("rankings: \"%s\" missing or not a two-string format -- bare rank", key);
        _snprintf(out, outsz, "%s %s", name, tw ? tw : "");
    }
    out[outsz - 1] = 0;
    return true;
}

// ---- text: the duration formatter, copied from 0x499700 ----
static void rbDurationKey(uintptr_t base, const char* key, char* out, int outsz, int a, int b, int nargs) {
    char fmt[64] = { 0 };
    if (!resolveKey(base, key, fmt, sizeof fmt) || !fmt[0] || !rbFmtInts(fmt, nargs)) {
        logLine("rankings: \"%s\" missing or not a %d-int format", key, nargs);
        if (nargs == 2) _snprintf(out, outsz, "%d %d", a, b);
        else if (nargs == 1) _snprintf(out, outsz, "%d", a);
        else _snprintf(out, outsz, "%s", key);
        out[outsz - 1] = 0;
        return;
    }
    abStripMarkup(fmt);
    if (nargs == 2) _snprintf(out, outsz, fmt, a, b);
    else if (nargs == 1) _snprintf(out, outsz, fmt, a);
    else _snprintf(out, outsz, "%s", fmt);
    out[outsz - 1] = 0;
}
static void rbDuration(uintptr_t base, int64_t t, char* out, int outsz) {
    int64_t now = (int64_t)time(nullptr);
    int64_t diff = now - t;
    if (diff < 0) diff = -diff;
    if (diff > 0x7fffffffLL) {                     // the game's int cast goes negative -> Forever
        rbDurationKey(base, "str_rankings_time_infinite_format", out, outsz, 0, 0, 0);
        return;
    }
    int secs = (int)diff;
    int hours = secs / 3600, days = hours / 24, h24 = hours - days * 24;
    int mins = secs / 60, m60 = mins - (mins / 60) * 60;
    if (days > 0) {
        if (days <= 9 && h24) rbDurationKey(base, "str_rankings_time_dh_format", out, outsz, days, h24, 2);
        else                  rbDurationKey(base, "str_rankings_time_d_format", out, outsz, days, 0, 1);
    } else if (hours > 0) {
        if (m60) rbDurationKey(base, "str_rankings_time_hm_format", out, outsz, hours, m60, 2);
        else     rbDurationKey(base, "str_rankings_time_h_format", out, outsz, hours, 0, 1);
    } else if (m60) {
        rbDurationKey(base, "str_rankings_time_m_format", out, outsz, m60, 0, 1);
    } else {
        rbDurationKey(base, "str_rankings_time_s_format", out, outsz, secs - mins * 60, 0, 1);
    }
}

// ---- text: the header rows ----
static bool rbStepsText(uintptr_t bld, char* out, int outsz) {
    uint32_t cur = 0, total = 0;
    if (!bld || !safeReadU32(bld + RB_B_CUR_STEPS_OFF, &cur) || !safeReadU32(bld + RB_B_STEPS_TO_ADV_OFF, &total))
        return false;
    if (cur > 1000 || total > 1000) return false;   // ints the game zeroes; huge = a bad read
    _snprintf(out, outsz, axs(AXS_RB_STEPS_FMT), (int)cur, (int)total);
    out[outsz - 1] = 0;
    return true;
}

static bool rbSeasonText(uintptr_t base, uintptr_t bld, char* out, int outsz) {
    int64_t t = 0;
    if (!bld || !safeReadI64(bld + RB_B_SEASON_END_OFF, &t)) return false;
    char dur[64] = { 0 };
    rbDuration(base, t, dur, sizeof dur);
    char fmt[128] = { 0 };
    if (resolveKey(base, "str_rankings_stats_season_end_format", fmt, sizeof fmt) && fmt[0] && rbFmtOneStr(fmt)) {
        abStripMarkup(fmt);
        _snprintf(out, outsz, fmt, dur);
    } else {
        logLine("rankings: str_rankings_stats_season_end_format missing or not a one-string format");
        _snprintf(out, outsz, "%s", dur);
    }
    out[outsz - 1] = 0;
    return true;
}

static bool rbBestText(uintptr_t base, uintptr_t bld, char* out, int outsz) {
    char league[192];
    if (!bld || !rbLeagueText(base, bld + RB_B_LEAGUE_BEST, league, sizeof league)) return false;
    char title[128] = { 0 };
    if (!resolveKey(base, "str_rankings_stats_best_title", title, sizeof title) || !title[0]) {
        logLine("rankings: str_rankings_stats_best_title did not resolve");
        title[0] = 0;
    }
    abStripMarkup(title);
    _snprintf(out, outsz, "%s%s%s", title, title[0] ? " " : "", league);
    out[outsz - 1] = 0;
    return true;
}

static void rbSectionName(uintptr_t base, int section, char* out, int outsz) {
    char key[64];
    _snprintf(key, sizeof key, "str_rankings_section_%u", (unsigned)section);
    key[sizeof key - 1] = 0;
    if (!resolveKey(base, key, out, outsz) || !out[0]) {
        logLine("rankings: \"%s\" did not resolve", key);
        _snprintf(out, outsz, "%d", section + 1);
    }
    out[outsz - 1] = 0;
    abStripMarkup(out);
}
static int rbListState(uintptr_t bld, int section, int* countOut) {
    int n = rbMatchVec(bld, section, nullptr);
    if (countOut) *countOut = n;
    if (n > 0) return 2;
    uint8_t dataIn = 0, busy = 0;
    safeReadU8(bld + RB_B_DATA_IN_OFF, &dataIn);
    safeReadU8(bld + RB_B_HIST_BUSY_OFF, &busy);
    if (n < 0 || !dataIn || busy) return 0;
    return 1;
}
static bool rbSectionText(uintptr_t base, uintptr_t bld, int section, char* out, int outsz) {
    if (section < 0) return false;
    char name[128];
    rbSectionName(base, section, name, sizeof name);
    int n = 0;
    int st = rbListState(bld, section, &n);
    char head[192], tail[128];
    _snprintf(head, sizeof head, axs(AXS_RB_SECTION_FMT), name, section + 1, RB_SECTIONS);
    head[sizeof head - 1] = 0;
    if (st == 2)      _snprintf(tail, sizeof tail, axs(AXS_RB_MATCHES_FMT), n);
    else if (st == 1) _snprintf(tail, sizeof tail, "%s", axs(AXS_RB_NO_MATCHES));
    else              _snprintf(tail, sizeof tail, "%s", axs(AXS_RB_LOADING));
    tail[sizeof tail - 1] = 0;
    _snprintf(out, outsz, "%s %s", head, tail);
    out[outsz - 1] = 0;
    return true;
}

// ---- text: one match row ----
static void rbHeroList(uintptr_t base, uintptr_t vec, char* out, int outsz) {
    out[0] = 0;
    uintptr_t beg = 0, end = 0;
    if (!safeReadPtr(vec, &beg) || !safeReadPtr(vec + 8, &end) || beg <= 0x10000 || end < beg ||
        (end - beg) % RB_HERO_STRIDE) {
        _snprintf(out, outsz, "%s", axs(AXS_RB_NO_HEROES));
        out[outsz - 1] = 0;
        return;
    }
    int n = (int)((end - beg) / RB_HERO_STRIDE);
    if (n > RB_MAX_HEROES) n = RB_MAX_HEROES;
    if (n == 0) { _snprintf(out, outsz, "%s", axs(AXS_RB_NO_HEROES)); out[outsz - 1] = 0; return; }
    for (int i = 0; i < n; i++) {
        uintptr_t e = beg + (uintptr_t)i * RB_HERO_STRIDE;
        uint32_t hash = 0; uint8_t dead = 0;
        safeReadU32(e + RB_HERO_HASH_OFF, &hash);
        safeReadU8(e + RB_HERO_DEAD_OFF, &dead);
        char name[96] = { 0 };
        if (!bldClassNameByHash(base, hash, name, sizeof name) || !name[0]) {
            logLine("rankings: hero class hash 0x%08x named by no registry", hash);
            _snprintf(name, sizeof name, "%s", axs(AXS_RB_HERO_UNKNOWN));
        }
        char piece[128];
        if (dead) _snprintf(piece, sizeof piece, axs(AXS_RB_HERO_DEAD_FMT), name);
        else      _snprintf(piece, sizeof piece, "%s", name);
        piece[sizeof piece - 1] = 0;
        size_t len = strlen(out);
        _snprintf(out + len, outsz - (int)len, "%s%s", len ? ", " : "", piece);
        out[outsz - 1] = 0;
    }
}

static void rbPlayerName(uintptr_t base, char* out, int outsz) {
    out[0] = 0;
    uint8_t alt = 1;
    uintptr_t api = 0;
    if (MP_API_RVA && safeReadU8(base + MP_ALT_GATE_RVA, &alt) && !alt &&
        safeReadPtr(base + MP_API_RVA, &api) && api > 0x10000)
        safeReadCStr(api + RB_MP_NAME_OFF, out, outsz < 64 ? outsz : 64);
    if (!out[0] && (!resolveKey(base, "str_match_player_name", out, outsz) || !out[0])) {
        logLine("rankings: no persona name and str_match_player_name did not resolve");
        _snprintf(out, outsz, "%s", axs(AXS_RB_YOU_FALLBACK));
    }
    out[outsz - 1] = 0;
    abStripMarkup(out);
}

static void rbOutcomeWord(uintptr_t base, bool won, char* out, int outsz) {
    const char* key = won ? "raid_results_quest_result_was_completed"
                          : "raid_results_quest_result_was_not_completed_defeat";
    if (!resolveKey(base, key, out, outsz) || !out[0]) {
        logLine("rankings: outcome key \"%s\" did not resolve", key);
        _snprintf(out, outsz, "%s", axs(won ? AXS_BCR_WON_FALLBACK : AXS_BCR_LOST_FALLBACK));
    }
    out[outsz - 1] = 0;
    abStripMarkup(out);
}

static bool rbMatchText(uintptr_t base, uintptr_t rec, int row, int n, char* out, int outsz) {
    if (!rec) return false;
    uint8_t won = 0; int64_t t = 0;
    safeReadU8(rec + RB_REC_WON_OFF, &won);
    safeReadI64(rec + RB_REC_TIME_OFF, &t);
    char outcome[96], dur[64] = { 0 }, ago[128] = { 0 };
    rbOutcomeWord(base, won != 0, outcome, sizeof outcome);
    rbDuration(base, t, dur, sizeof dur);
    char fmt[96] = { 0 };
    if (resolveKey(base, "str_rankings_time_ago_format", fmt, sizeof fmt) && fmt[0] && rbFmtOneStr(fmt)) {
        abStripMarkup(fmt);
        _snprintf(ago, sizeof ago, fmt, dur);
    } else {
        logLine("rankings: str_rankings_time_ago_format missing or not a one-string format");
        _snprintf(ago, sizeof ago, "%s", dur);
    }
    ago[sizeof ago - 1] = 0;
    char you[96], opp[RB_REC_OPPNAME_CAP + 1] = { 0 }, mine[512], theirs[512], vs[32] = { 0 };
    rbPlayerName(base, you, sizeof you);
    safeReadCStr(rec + RB_REC_OPPNAME_OFF, opp, sizeof opp);
    if (!opp[0] && (!resolveKey(base, "str_match_opponent_name", opp, sizeof opp) || !opp[0]))
        _snprintf(opp, sizeof opp, "%s", axs(AXS_RB_OPPONENT_FALLBACK));
    opp[sizeof opp - 1] = 0;
    abStripMarkup(opp);
    rbHeroList(base, rec + RB_REC_PLAYER_VEC, mine, sizeof mine);
    rbHeroList(base, rec + RB_REC_OPP_VEC, theirs, sizeof theirs);
    if (!resolveKey(base, "str_rankings_vs", vs, sizeof vs) || !vs[0]) _snprintf(vs, sizeof vs, "%s", axs(AXS_RB_VS_FALLBACK));
    vs[sizeof vs - 1] = 0;
    abStripMarkup(vs);
    char pos[64];
    _snprintf(pos, sizeof pos, axs(AXS_RB_MATCH_N_OF_M), row + 1, n);
    pos[sizeof pos - 1] = 0;
    _snprintf(out, outsz, "%s. %s. %s: %s. %s %s: %s. %s", outcome, ago, you, mine, vs, opp, theirs, pos);
    out[outsz - 1] = 0;
    return true;
}

// ---- the cursor ----
static int rbRowCount(uintptr_t bld, int section) {
    int n = rbMatchVec(bld, section, nullptr);
    return RB_ROW_HEADERS + (n > 0 ? n : 0);
}

static bool rbRowText(uintptr_t base, uintptr_t panel, int row, char* out, int outsz) {
    uintptr_t bld = rbBuilding(base);
    int section = rbSection(panel);
    if (!bld) return false;
    switch (row) {
    case RB_ROW_CURRENT: return rbRankLine(base, "str_rankings_current_rank_format", bld + RB_B_LEAGUE_CUR, out, outsz);
    case RB_ROW_NEXT: {
        uint8_t hasNext = 0;
        safeReadU8(bld + RB_B_HAS_NEXT_OFF, &hasNext);
        if (!hasNext) { _snprintf(out, outsz, "%s", axs(AXS_RB_TOP_RANK)); out[outsz - 1] = 0; return true; }
        return rbRankLine(base, "str_rankings_next_rank_format", bld + RB_B_LEAGUE_NEXT, out, outsz);
    }
    case RB_ROW_STEPS:   return rbStepsText(bld, out, outsz);
    case RB_ROW_BEST:    return rbBestText(base, bld, out, outsz);
    case RB_ROW_SEASON:  return rbSeasonText(base, bld, out, outsz);
    case RB_ROW_SECTION: return rbSectionText(base, bld, section, out, outsz);
    default: {
        uintptr_t beg = 0;
        int n = rbMatchVec(bld, section, &beg);
        int i = row - RB_ROW_HEADERS;
        if (n <= 0 || i < 0 || i >= n) return false;
        return rbMatchText(base, rbRecordAt(beg, n, i), i, n, out, outsz);
    }
    }
}

static void rbSpeakCursor(uintptr_t base, uintptr_t panel, const char* prefix) {
    uintptr_t bld = rbBuilding(base);
    int section = rbSection(panel);
    int total = rbRowCount(bld, section);
    if (g_rbRow >= total) g_rbRow = total - 1;
    if (g_rbRow < 0) g_rbRow = 0;
    char body[MAILBOX_SZ];
    if (!bld || section < 0) {
        logLine("rankings: building=%p section=%d -- unreadable", (void*)bld, section);
        _snprintf(body, sizeof body, "%s", axs(AXS_RB_UNREADABLE));
    } else if (!rbRowText(base, panel, g_rbRow, body, sizeof body)) {
        logLine("rankings: row %d of %d unreadable", g_rbRow, total);
        _snprintf(body, sizeof body, "%s", axs(AXS_RB_ROW_UNREADABLE));
    }
    body[sizeof body - 1] = 0;
    char utter[MAILBOX_SZ];
    _snprintf(utter, sizeof utter, "%s%s", prefix ? prefix : "", body);
    utter[sizeof utter - 1] = 0;
    postSpeech(utter);
}

bool rbSpeakArrival(uintptr_t base, uintptr_t panel, const char* prefix) {
    if (!rbIsPanel(base, panel)) return false;
    uintptr_t bld = rbBuilding(base);
    int section = rbSection(panel);
    if (!bld || section < 0) {
        logLine("rankings: arrival with building=%p section=%d", (void*)bld, section);
        char utter[MAILBOX_SZ];
        _snprintf(utter, sizeof utter, "%s%s", prefix ? prefix : "", axs(AXS_RB_UNREADABLE));
        utter[sizeof utter - 1] = 0;
        postSpeech(utter);
        return true;
    }
    int n = 0;
    int st = rbListState(bld, section, &n);
    g_rbLoadPending = (st == 0);
    g_rbLastCount = n;
    char sec[384] = { 0 };
    rbSectionText(base, bld, section, sec, sizeof sec);
    char pfx[MAILBOX_SZ];
    _snprintf(pfx, sizeof pfx, "%s", prefix ? prefix : "");
    pfx[sizeof pfx - 1] = 0;
    char row0[512] = { 0 };
    if (!rbRowText(base, panel, RB_ROW_CURRENT, row0, sizeof row0)) {
        logLine("rankings: current-rank row unreadable on arrival");
        _snprintf(row0, sizeof row0, "%s", axs(AXS_RB_ROW_UNREADABLE));
    }
    char utter[MAILBOX_SZ];
    _snprintf(utter, sizeof utter, "%s%s. %s", pfx, row0, sec);   // the full stop is the pause
    utter[sizeof utter - 1] = 0;                                    // between rank and section
    postSpeech(utter);
    return true;
}

// ---- the section switch: a CLICK on the game's own tab icon ----
static bool rbClickTab(uintptr_t base, int target) {
    uintptr_t els[8];
    int n = feCollectFamilyByX(base, RB_TAB_ELEM_ID, 0, els, nullptr, 8);
    if (n != RB_SECTIONS) {
        logLine("rankings: expected %d 'rsi' tab elements on screen, found %d", RB_SECTIONS, n);
        diagDumpFocusElements(base, "rankings-tabs");
        return false;
    }
    float cx[8], cy[8];
    for (int i = 0; i < n; i++) {
        if (!elemCenter(els[i], &cx[i], &cy[i])) { logLine("rankings: tab element %d has no rect", i); return false; }
    }
    // ascending Y: index 0 = the top tab = section 0
    for (int i = 1; i < n; i++) {
        int k = i;
        while (k > 0 && cy[k - 1] > cy[k]) {
            float tx = cx[k], ty = cy[k]; uintptr_t te = els[k];
            cx[k] = cx[k - 1]; cy[k] = cy[k - 1]; els[k] = els[k - 1];
            cx[k - 1] = tx; cy[k - 1] = ty; els[k - 1] = te; k--;
        }
    }
    if (clickQueued()) { logLine("rankings: tab click refused, a click is already queued"); return false; }
    logLine("rankings: clicking tab %d at (%.0f,%.0f) (tabs at y=%.0f / %.0f)", target,
            cx[target], cy[target], cy[0], cy[1]);
    moveCursorTo(cx[target], cy[target]);                      // pins the globals + hover (frame 0)
    enqueueSynth(SDL_EVT_MOUSEBUTTONDOWN, SDL_BUTTON_LEFT, 1, 1);
    enqueueSynth(SDL_EVT_MOUSEBUTTONUP,   SDL_BUTTON_LEFT, 0, 2);
    return true;
}

// ---- keys ----
bool rbRouteKey(uintptr_t base, uintptr_t panel, uint32_t sym, uint8_t repeat) {
    if (!rbIsPanel(base, panel)) return false;
    const bool nav = sym == SDLK_UP || sym == SDLK_DOWN || sym == SDLK_LEFT || sym == SDLK_RIGHT ||
                     sym == SDLK_HOME || sym == SDLK_END || sym == SDLK_RETURN || sym == SDLK_KP_ENTER;
    if (!nav) return false;                          // Escape and the rest stay the game's
    uintptr_t bld = rbBuilding(base);
    int section = rbSection(panel);
    if (!bld || section < 0) {
        if (repeat) return true;
        logLine("rankings: key with building=%p section=%d", (void*)bld, section);
        postSpeech(axs(AXS_RB_UNREADABLE));
        return true;
    }
    int total = rbRowCount(bld, section);

    // Home/End: the first / last row.
    {
        int jump = 0;
        if (axDecodeJump(sym, 0, repeat, &jump)) {
            if (!jump) return true;                  // held jump: one landing per press
            axStepCursor(&g_rbRow, total, jump);
            rbSpeakCursor(base, panel, nullptr);
            return true;
        }
    }
    if (sym == SDLK_UP || sym == SDLK_DOWN) {
        if (axNavHoldRepeat(repeat)) return true;    // throttled repeat: claimed, no step
        axStepCursor(&g_rbRow, total, sym == SDLK_DOWN ? 1 : -1);   // hard stops re-read
        rbSpeakCursor(base, panel, nullptr);
        return true;
    }
    if (sym == SDLK_LEFT || sym == SDLK_RIGHT) {
        if (repeat) return true;                     // an action, one per press
        if (g_rbSwitchUntil || clickQueued()) { postSpeech(axs(AXS_STILL_SWITCHING)); return true; }
        int target = section + (sym == SDLK_RIGHT ? 1 : -1);
        if (target < 0 || target >= RB_SECTIONS) {   // the ends hard-stop and re-read the section
            g_rbRow = RB_ROW_SECTION;
            rbSpeakCursor(base, panel, nullptr);
            return true;
        }
        if (!rbClickTab(base, target)) { postSpeech(axs(AXS_RB_SECTION_STUCK)); return true; }
        g_rbSwitchTo = target;
        g_rbSwitchUntil = GetTickCount() + 1500;     // rbService announces the landing
        return true;
    }
    if (sym == SDLK_RETURN || sym == SDLK_KP_ENTER) {
        if (repeat) return true;
        rbSpeakCursor(base, panel, nullptr);         // nothing on this screen clicks a row
        return true;
    }
    return false;
}

// ---- the watches ----
void rbService(uintptr_t base, uintptr_t panel) {
    if (!rbIsPanel(base, panel)) {
        if (g_rbSwitchUntil) { g_rbSwitchUntil = 0; g_rbSwitchTo = -1; logLine("rankings: switch watch ended by the screen closing"); }
        return;
    }
    uintptr_t bld = rbBuilding(base);
    int section = rbSection(panel);
    if (g_rbSwitchUntil) {
        if (section == g_rbSwitchTo) {
            g_rbSwitchUntil = 0; g_rbSwitchTo = -1;
            logLine("rankings: section switched to %d", section);
            int n = 0;
            int st = rbListState(bld, section, &n);
            g_rbLoadPending = (st == 0);
            g_rbLastCount = n;
            g_rbRow = RB_ROW_SECTION;                // land on the section head, its count included
            rbSpeakCursor(base, panel, nullptr);
        } else if (GetTickCount() > g_rbSwitchUntil) {
            g_rbSwitchUntil = 0; g_rbSwitchTo = -1;
            logLine("rankings: switch watch timed out -- section still %d", section);
            logInputSnapshot(base, "rankings-tab-timeout");   // the gate state, for the next report
            diagDumpFocusElements(base, "rankings-tab-timeout");
            postSpeech(axs(AXS_RB_SECTION_UNCHANGED));
        }
        return;
    }
    if (!bld || section < 0) return;
    int n = 0;
    int st = rbListState(bld, section, &n);
    if (g_rbLoadPending && st != 0) {
        g_rbLoadPending = false;
        g_rbLastCount = n;
        logLine("rankings: match history landed after arrival (section %d, %d rows)", section, n);
        char sec[384] = { 0 };
        rbSectionText(base, bld, section, sec, sizeof sec);
        char utter[MAILBOX_SZ];
        _snprintf(utter, sizeof utter, "%s %s", axs(AXS_RB_LOADED), sec);
        utter[sizeof utter - 1] = 0;
        postSpeech(utter);
    } else if (!g_rbLoadPending && st == 2 && g_rbLastCount >= 0 && n != g_rbLastCount) {
        g_rbLastCount = n;
        logLine("rankings: match count changed while open (section %d, now %d)", section, n);
        char sec[384] = { 0 };
        rbSectionText(base, bld, section, sec, sizeof sec);
        postSpeech(sec);
    } else if (!g_rbLoadPending && st == 0 && g_rbLastCount > 0) {
        g_rbLoadPending = true;
        g_rbLastCount = 0;
        logLine("rankings: list reloading (section %d)", section);
    }
}

// ---- the probe ----
void rbProbe(uintptr_t base, uintptr_t panel) {
    if (!panel) { logDump("rankings probe: no panel"); return; }
    uintptr_t vft = 0;
    safeReadPtr(panel, &vft);
    logDump("rankings probe: panel=%p vft=%p (expect base+0x%llx = %p) match=%d",
            (void*)panel, (void*)vft, (unsigned long long)RB_VFT_RVA, (void*)(base + RB_VFT_RVA),
            rbIsPanel(base, panel) ? 1 : 0);
    if (!rbIsPanel(base, panel)) {
        logDump("rankings probe: NOT the ranking board panel -> nothing else is trustworthy here");
        return;
    }
    uintptr_t bldPanel = 0, bldMap = rbBuilding(base);
    safeReadPtr(panel + RB_P_BLD_OFF, &bldPanel);
    logDump("rankings probe: building via panel+0x260=%p, via Campaign map=%p %s",
            (void*)bldPanel, (void*)bldMap,
            bldPanel == bldMap ? "(agree)" : "<-- DISAGREE: +0x260 is not the building, or the map walk is off");
    uint32_t section = 0xffffffff, selRow = 0xffffffff, arc = 0;
    uint8_t built = 0xff, gamepad = 0xff, inputMode = 0xff;
    safeReadU32(panel + RB_P_SECTION_OFF, &section); safeReadU32(panel + RB_P_SELROW_OFF, &selRow);
    safeReadU32(panel + RB_P_ARC_OFF, &arc);
    safeReadU8(panel + RB_P_BUILT_OFF, &built); safeReadU8(panel + RB_P_GAMEPAD_OFF, &gamepad);
    safeReadU8(panel + RB_P_INPUTMODE_OFF, &inputMode);
    logDump("rankings probe: section(+0xa0)=%u selRow(+0x718)=%u arc(+0x71c)=%.3f built(+0x720)=%u "
            "gamepad(+0x721)=%u inputMode(+0x722)=%u", section, selRow, u32AsFloatM(arc), built, gamepad, inputMode);
    if (bldMap) {
        uint8_t f[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
        for (int i = 0; i < 6; i++) safeReadU8(bldMap + RB_B_DATA_IN_OFF + i, &f[i]);
        uint32_t adv = 0, cur = 0, tiers = 0, season = 0; uint8_t hasNext = 0xff; int64_t seasonEnd = 0;
        safeReadU32(bldMap + RB_B_STEPS_TO_ADV_OFF, &adv); safeReadU32(bldMap + RB_B_CUR_STEPS_OFF, &cur);
        safeReadU32(bldMap + RB_B_TIERS_OFF, &tiers); safeReadU32(bldMap + RB_B_SEASON_OFF, &season);
        safeReadU8(bldMap + RB_B_HAS_NEXT_OFF, &hasNext); safeReadI64(bldMap + RB_B_SEASON_END_OFF, &seasonEnd);
        char dur[64] = { 0 };
        rbDuration(base, seasonEnd, dur, sizeof dur);
        logDump("rankings probe: flags(+0xa8..+0xad)=%u,%u,%u,%u,%u,%u steps_to_advance(+0x410)=%u "
                "current_steps(+0x414)=%u tiers(+0x424)=%u season(+0x428)=%u hasNext(+0x484)=%u "
                "seasonEnd(+0x228)=%lld -> \"%s\" (now %lld)",
                f[0], f[1], f[2], f[3], f[4], f[5], adv, cur, tiers, season, hasNext,
                (long long)seasonEnd, dur, (long long)time(nullptr));
        static const struct { const char* name; uintptr_t off; } blocks[4] = {
            { "prev", RB_B_LEAGUE_PREV }, { "current", RB_B_LEAGUE_CUR },
            { "next", RB_B_LEAGUE_NEXT }, { "best", RB_B_LEAGUE_BEST } };
        for (int b = 0; b < 4; b++) {
            uintptr_t blk = bldMap + blocks[b].off, namep = 0;
            char id[RB_L_ID_CAP + 1] = { 0 }, name[96] = { 0 }, text[192] = { 0 };
            uint32_t tier = 0xffffffff, steps = 0xffffffff;
            safeReadCStr(blk + RB_L_ID_OFF, id, sizeof id);
            if (safeReadPtr(blk + RB_L_NAME_OFF, &namep) && namep > 0x10000) safeReadCStr(namep, name, sizeof name);
            safeReadU32(blk + RB_L_TIER_OFF, &tier); safeReadU32(blk + RB_L_STEPS_OFF, &steps);
            rbLeagueText(base, blk, text, sizeof text);
            logDump("rankings probe:   %-7s block +0x%03llx id=\"%s\" name(+0x20)=%p \"%s\" tier(+0x44)=%u steps(+0x48)=%u -> \"%s\"",
                    blocks[b].name, (unsigned long long)blocks[b].off, id, (void*)namep, name, tier, steps, text);
        }
        for (int s = 0; s < RB_SECTIONS; s++) {
            uintptr_t at = bldMap + RB_B_MATCHVEC_OFF + (uintptr_t)s * RB_B_MATCHVEC_STRIDE, vb = 0, ve = 0, beg = 0;
            safeReadPtr(at, &vb); safeReadPtr(at + 8, &ve);
            int n = rbMatchVec(bldMap, s, &beg);
            logDump("rankings probe: section %d vector(+0x%llx)=[%p,%p) -> %d records (stride 0x80)",
                    s, (unsigned long long)(RB_B_MATCHVEC_OFF + s * RB_B_MATCHVEC_STRIDE), (void*)vb, (void*)ve, n);
            for (int i = 0; i < n && i < 6; i++) {
                uintptr_t rec = rbRecordAt(beg, n, i);
                uint8_t won = 0xff; int64_t t = 0; char opp[RB_REC_OPPNAME_CAP + 1] = { 0 };
                safeReadU8(rec + RB_REC_WON_OFF, &won); safeReadI64(rec + RB_REC_TIME_OFF, &t);
                safeReadCStr(rec + RB_REC_OPPNAME_OFF, opp, sizeof opp);
                uintptr_t pb = 0, pe = 0, ob = 0, oe = 0;
                safeReadPtr(rec + RB_REC_PLAYER_VEC, &pb); safeReadPtr(rec + RB_REC_PLAYER_VEC + 8, &pe);
                safeReadPtr(rec + RB_REC_OPP_VEC, &ob); safeReadPtr(rec + RB_REC_OPP_VEC + 8, &oe);
                char text[MAILBOX_SZ] = { 0 };
                rbMatchText(base, rec, i, n, text, sizeof text);
                logDump("rankings probe:   row %d rec=%p won(+0x78)=%u time(+0x30)=%lld opp(+0x38)=\"%s\" "
                        "heroes=[%p,%p) (%ld) vs [%p,%p) (%ld) -> \"%s\"",
                        i, (void*)rec, won, (long long)t, opp, (void*)pb, (void*)pe,
                        (pb && pe >= pb) ? (long)((pe - pb) / RB_HERO_STRIDE) : -1L,
                        (void*)ob, (void*)oe, (ob && oe >= ob) ? (long)((oe - ob) / RB_HERO_STRIDE) : -1L, text);
                if (i == 0 && pb && pe > pb) {
                    for (uintptr_t e = pb; e < pe && e < pb + 8 * RB_HERO_STRIDE; e += RB_HERO_STRIDE) {
                        uint32_t h = 0; uint8_t dead = 0xff; char nm[96] = { 0 };
                        safeReadU32(e + RB_HERO_HASH_OFF, &h); safeReadU8(e + RB_HERO_DEAD_OFF, &dead);
                        bldClassNameByHash(base, h, nm, sizeof nm);
                        logDump("rankings probe:     hero entry hash=0x%08x dead=%u -> \"%s\"", h, dead, nm);
                    }
                }
            }
        }
    }
    uintptr_t cb = 0, ce = 0;
    safeReadPtr(panel + RB_P_CELLVEC_BEG, &cb); safeReadPtr(panel + RB_P_CELLVEC_END, &ce);
    logDump("rankings probe: cellVec(+0x3b8..+0x3c0)=[%p,%p) -> %ld (expect 1: the outer cell)",
            (void*)cb, (void*)ce, (cb && ce >= cb && !((ce - cb) % 8)) ? (long)((ce - cb) / 8) : -1L);
    // The tabs and the refresh button: what they registered as, and where.
    uintptr_t els[8]; float xs[8];
    int nt = feCollectFamilyByX(base, RB_TAB_ELEM_ID, 0, els, xs, 8);
    logDump("rankings probe: 'rsi' (0x%x) family elements on screen: %d", RB_TAB_ELEM_ID, nt);
    for (int i = 0; i < nt; i++) {
        float cx = 0, cy = 0; elemCenter(els[i], &cx, &cy);
        logDump("rankings probe:   tab element %d centre=(%.0f,%.0f)", i, cx, cy);
    }
    feDumpFocusVector("ranking board open");
}
