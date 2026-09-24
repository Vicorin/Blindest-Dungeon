// dlc/butchers_circus/prizebox.cpp -- THE PRIZE BOX.

#include <cstdint>
#include <cstring>
#include <cstdio>
#include <windows.h>
#include "internal.h"

// ---- identity ----

// ---- the panel (PrizeBoothDisplay) ----
static const uintptr_t PB_BOOTH_OFF       = 0x260;  // Building::PrizeBooth* (vslot 33 stores it)
static const uintptr_t PB_CELLVEC_BEG     = 0x3b8;  // vector<Widget*> begin -- the ONE outer cell
static const uintptr_t PB_CELLVEC_END     = 0x3c0;  // ... end
static const uintptr_t PB_SCROLLSPEED_OFF = 0x6ec;  // float (rewards_layout.scroll_speed)
static const uintptr_t PB_SEL_LEVEL_OFF   = 0x6f8;  // uint: the game's selected level (1-based)
static const uintptr_t PB_SEL_IDX_OFF     = 0x6fc;  // uint: ... and reward index (inspect mode)
static const uintptr_t PB_BUILT_OFF       = 0x700;  // byte: the list has been built
static const uintptr_t PB_CTRLMODE_OFF    = 0x701;  // byte: controller-mode snapshot
static const uintptr_t PB_INSPECT_OFF     = 0x702;  // byte: controller inspect mode
static const uintptr_t PB_CELL_EXTENT_OFF = 0xb4;   // float: content extent
static const uintptr_t PB_CELL_TARGET_OFF = 0xb8;   // float: the scroll offset being eased to
static const uintptr_t PB_CELL_VIEW_OFF   = 0xbc;   // float: the viewport
static const uintptr_t PB_CELL_SCROLL_OFF = 0xc0;   // float: the live scroll offset
static const uintptr_t PB_CELL_SCROLLBAR_OFF = 0xf8;// Widget*: the scrollbar
static const uintptr_t PB_CELL_CONTENT_OFF= 0x150;  // Widget*: the row container

// ---- the building (Building::PrizeBooth) ----
static const uintptr_t PB_BOOTH_LEVEL_OFF = 0x120;
static const uintptr_t PB_BOOTH_PTS_OFF   = 0x124;  // int: prestige points
static const uintptr_t PB_BOOTH_NEXT_OFF  = 0x128;  // int: points at the next level
static const uintptr_t PB_BOOTH_PREV_OFF  = 0x12c;  // int: points at the previous level
static const uintptr_t PB_BOOTH_FLAGS_OFF = 0xa8;
static const uintptr_t PB_BOOTH_DATA_OFF  = 0x168;  // ptr: ... whose +0xc1 == 0 && +0xc3 != 0
static const uintptr_t PB_DATA_C1_OFF     = 0xc1;
static const uintptr_t PB_DATA_C3_OFF     = 0xc3;
static const int       PB_MAX_LEVELS      = 128;    // 69 shipped; a guard, not a table
static const int       PB_MAX_PER_LEVEL   = 8;      // a pair at most, so far

// ---- state ----
struct PbLevel { int level; uintptr_t rec; int nTrink; int nBanner; };
static int  g_pbRow       = 0;      // cursor: index into the rows-with-rewards list
static int  g_pbReward    = -1;     // -1 = on the level row; 0.. = a reward inside it
static bool g_pbRowInit   = false;
static bool g_pbWaitBuild = false;
static bool g_pbBuiltSeen = false;  // last observed +0x700
static DWORD g_pbOpenUntil = 0;
                                    // the game's ConfirmDialog appearing (the reader speaks it)

bool pbIsPanel(uintptr_t base, uintptr_t panel) {
    uintptr_t vft = 0;
    if (!panel || !safeReadPtr(panel, &vft) || vft <= base) return false;
    return vft - base == PB_VFT_RVA;
}

void pbReset() {
    g_pbRow = 0; g_pbReward = -1; g_pbRowInit = false; g_pbWaitBuild = false; g_pbBuiltSeen = false;
    g_pbOpenUntil = 0;
}

static uintptr_t pbBooth(uintptr_t base) {
    return bcrBuildingByHash(base, resHash("prize_booth"));
}

static bool pbBuilt(uintptr_t panel) {
    uint8_t b = 0;
    return safeReadU8(panel + PB_BUILT_OFF, &b) && b != 0;
}

// The four prestige ints. false = unreadable (the caller says so, never guesses).
static bool pbPrestige(uintptr_t base, int* level, int* pts, int* next, int* prev) {
    uintptr_t booth = pbBooth(base);
    uint32_t l = 0, p = 0, n = 0, v = 0;
    if (!booth || !safeReadU32(booth + PB_BOOTH_LEVEL_OFF, &l) ||
        !safeReadU32(booth + PB_BOOTH_PTS_OFF, &p) || !safeReadU32(booth + PB_BOOTH_NEXT_OFF, &n) ||
        !safeReadU32(booth + PB_BOOTH_PREV_OFF, &v)) return false;
    if (l > 1000) return false;                       // an int the game zeroes; huge = a bad read
    *level = (int)l; *pts = (int)p; *next = (int)n; *prev = (int)v;
    return true;
}

static int pbLevels(uintptr_t base, PbLevel* out, int max) {
    uintptr_t booth = pbBooth(base);
    if (!booth) return -1;
    uintptr_t beg = 0, end = 0;
    if (!safeReadPtr(booth + BCR_PB_LVLVEC_BEG, &beg) || !safeReadPtr(booth + BCR_PB_LVLVEC_END, &end) ||
        beg <= 0x10000 || end < beg || (end - beg) % BCR_PB_LVLREC_STRIDE) return -1;
    int levels = (int)((end - beg) / BCR_PB_LVLREC_STRIDE);
    if (levels > PB_MAX_LEVELS) levels = PB_MAX_LEVELS;
    int n = 0;
    for (int i = 0; i < levels && n < max; i++) {
        uintptr_t rec = beg + (uintptr_t)i * BCR_PB_LVLREC_STRIDE;
        uintptr_t tb = 0, te = 0, bb = 0, be = 0;
        safeReadPtr(rec + BCR_RC_TRINK_BEG, &tb);  safeReadPtr(rec + BCR_RC_TRINK_END, &te);
        safeReadPtr(rec + BCR_RC_BANNER_BEG, &bb); safeReadPtr(rec + BCR_RC_BANNER_END, &be);
        int nt = (tb > 0x10000 && te >= tb) ? (int)((te - tb) / BCR_RC_TRINK_STRIDE) : 0;
        int nb = (bb > 0x10000 && be >= bb) ? (int)((be - bb) / BCR_RC_BANNER_STRIDE) : 0;
        if (nt > PB_MAX_PER_LEVEL) nt = PB_MAX_PER_LEVEL;
        if (nb > PB_MAX_PER_LEVEL) nb = PB_MAX_PER_LEVEL;
        if (nt + nb == 0) continue;                   // the game builds no row for it either
        out[n].level = i + 1; out[n].rec = rec; out[n].nTrink = nt; out[n].nBanner = nb;
        n++;
    }
    return n;
}

// ---- text ----
static bool pbFmtTwoInt(const char* fmt) {
    int pct = 0, ok = 0;
    for (const char* p = fmt; *p; p++)
        if (*p == '%') { pct++; if (p[1] == 'u' || p[1] == 'd') ok++; }
    return pct == 2 && ok == 2;
}
static bool pbFmtOneInt(const char* fmt) {
    int pct = 0, ok = 0;
    for (const char* p = fmt; *p; p++)
        if (*p == '%') { pct++; if (p[1] == 'u' || p[1] == 'd') ok++; }
    return pct == 1 && ok == 1;
}

static void pbFlatten(char* s) {
    for (char* p = s; *p; p++) if (*p == '\n' || *p == '\r' || *p == '\t') *p = ' ';
    // collapse doubled spaces the newline left behind
    char* w = s;
    for (char* p = s; *p; p++) { if (*p == ' ' && w > s && w[-1] == ' ') continue; *w++ = *p; }
    *w = 0;
}

static void pbLevelWord(uintptr_t base, int level, char* out, int outsz) {
    char fmt[128] = { 0 };
    if (resolveKey(base, "str_ui_reward_level", fmt, sizeof fmt) && fmt[0] && pbFmtOneInt(fmt)) {
        abStripMarkup(fmt);
        _snprintf(out, outsz, fmt, level);
    } else {
        logLine("prizebox: str_ui_reward_level missing or not a one-int format -- bare number");
        _snprintf(out, outsz, "%d", level);
    }
    out[outsz - 1] = 0;
}

static bool pbPrestigeLine(uintptr_t base, char* out, int outsz) {
    int level, pts, next, prev;
    if (!pbPrestige(base, &level, &pts, &next, &prev)) return false;
    char lvl[128];
    pbLevelWord(base, level, lvl, sizeof lvl);
    int have = pts - prev, need = next - prev;
    char info[256] = { 0 };
    if (have == need) {
        if (!resolveKey(base, "str_prize_booth_prestige_info_max", info, sizeof info) || !info[0]) {
            logLine("prizebox: str_prize_booth_prestige_info_max did not resolve");
            info[0] = 0;
        }
    } else {
        char fmt[192] = { 0 };
        if (resolveKey(base, "str_prize_booth_prestige_info_format", fmt, sizeof fmt) && fmt[0] &&
            pbFmtTwoInt(fmt)) {
            abStripMarkup(fmt);
            _snprintf(info, sizeof info, fmt, have, need);
        } else {
            logLine("prizebox: str_prize_booth_prestige_info_format missing or not a two-int format");
            _snprintf(info, sizeof info, "%d / %d", have, need);
        }
    }
    info[sizeof info - 1] = 0;
    abStripMarkup(info);
    pbFlatten(info);
    _snprintf(out, outsz, "%s. %s", lvl, info);
    out[outsz - 1] = 0;
    return true;
}

static int pbRewardAt(const PbLevel* lv, int idx, uintptr_t* entry) {
    if (idx < 0) return 0;
    if (idx < lv->nTrink) {
        uintptr_t tb = 0;
        if (!safeReadPtr(lv->rec + BCR_RC_TRINK_BEG, &tb) || tb <= 0x10000) return 0;
        *entry = tb + (uintptr_t)idx * BCR_RC_TRINK_STRIDE;
        return 1;
    }
    idx -= lv->nTrink;
    if (idx < lv->nBanner) {
        uintptr_t bb = 0;
        if (!safeReadPtr(lv->rec + BCR_RC_BANNER_BEG, &bb) || bb <= 0x10000) return 0;
        *entry = bb + (uintptr_t)idx * BCR_RC_BANNER_STRIDE;
        return 2;
    }
    return 0;
}

// A reward's NAME only (the row list).
static bool pbRewardName(uintptr_t base, const PbLevel* lv, int idx, char* out, int outsz) {
    uintptr_t e = 0;
    int kind = pbRewardAt(lv, idx, &e);
    if (kind == 1) {
        uint32_t hash = 0;
        if (!safeReadU32(e + BCR_RC_TRINK_HASH, &hash) || !hash) return false;
        return bcrTrinketName(base, hash, out, outsz);
    }
    if (kind == 2) return bcrBannerName(base, e, out, outsz);
    return false;
}

// The level ROW: "Prestige Level N, earned: A, B. Current level. Level i of n."
static bool pbRowText(uintptr_t base, const PbLevel* rows, int n, int row, char* out, int outsz) {
    if (row < 0 || row >= n) return false;
    const PbLevel* lv = &rows[row];
    int cur, pts, next, prev;
    bool haveCur = pbPrestige(base, &cur, &pts, &next, &prev);
    bool locked = haveCur && cur < lv->level;         // the renderer's own test
    char lvl[128];
    pbLevelWord(base, lv->level, lvl, sizeof lvl);
    char names[1024] = { 0 };
    int total = lv->nTrink + lv->nBanner;
    for (int i = 0; i < total; i++) {
        char nm[192];
        if (!pbRewardName(base, lv, i, nm, sizeof nm)) {
            logLine("prizebox: level %d reward %d unreadable", lv->level, i);
            continue;
        }
        size_t len = strlen(names);
        _snprintf(names + len, sizeof names - len, "%s%s", len ? ", " : "", nm);
        names[sizeof names - 1] = 0;
    }
    char body[1280];
    if (names[0]) _snprintf(body, sizeof body, axs(AXS_PB_ROW_FMT), lvl,
                            axs(locked ? AXS_PB_LOCKED : AXS_PB_EARNED), names);
    else          _snprintf(body, sizeof body, "%s. %s", lvl, axs(AXS_PB_NO_REWARDS));
    body[sizeof body - 1] = 0;
    char pos[96];
    _snprintf(pos, sizeof pos, axs(AXS_PB_LEVEL_N_OF_M), row + 1, n);
    pos[sizeof pos - 1] = 0;
    _snprintf(out, outsz, "%s%s%s %s", body,
              (haveCur && cur == lv->level) ? " " : "",
              (haveCur && cur == lv->level) ? axs(AXS_PB_CURRENT_LEVEL) : "", pos);
    out[outsz - 1] = 0;
    return true;
}

static bool pbRewardText(uintptr_t base, const PbLevel* lv, int idx, char* out, int outsz) {
    uintptr_t e = 0;
    int kind = pbRewardAt(lv, idx, &e);
    if (!kind) return false;
    return pbRewardCard(base, kind, e, idx, lv->nTrink + lv->nBanner, true, out, outsz);
}

bool pbRewardCard(uintptr_t base, int kind, uintptr_t e, int idx, int total, bool enterPreview,
                  char* out, int outsz) {
    if (kind != 1 && kind != 2) return false;
    char pos[96];
    _snprintf(pos, sizeof pos, axs(AXS_PB_REWARD_N_OF_M), idx + 1, total);
    pos[sizeof pos - 1] = 0;

    if (kind == 1) {
        uint32_t hash = 0;
        if (!safeReadU32(e + BCR_RC_TRINK_HASH, &hash) || !hash) return false;
        char name[192] = { 0 };
        if (!bcrTrinketName(base, hash, name, sizeof name)) return false;
        char rarity[96] = { 0 }, clsreq[256] = { 0 }, fx[768] = { 0 }, trig[1024] = { 0 };
        uintptr_t rec = bldTrinketRecordByHash(base, hash);
        if (rec) {
            bldTrinketRarity(base, rec, rarity, sizeof rarity, false);
            bldTrinketClassReq(base, rec, clsreq, sizeof clsreq, false);
            bldTrinketEffectsRec(base, rec, fx, sizeof fx, false);
            bldTrinketTriggers(base, rec, trig, sizeof trig, false);
        } else {
            logLine("prizebox: no trinket record for hash 0x%x (\"%s\") -- name and condition only",
                    hash, name);
        }
        char head[256];
        _snprintf(head, sizeof head, axs(AXS_PB_REWARD_FMT), name, axs(AXS_PB_KIND_TRINKET));
        head[sizeof head - 1] = 0;
        _snprintf(out, outsz, "%s%s%s%s%s%s%s%s%s %s", head,
                  rarity[0] ? " " : "", rarity, rarity[0] ? "." : "",
                  clsreq[0] ? " " : "", clsreq, clsreq[0] ? "." : "",
                  fx[0] ? " " : "", fx, pos);
        out[outsz - 1] = 0;
        if (trig[0]) {
            size_t len = strlen(out);
            _snprintf(out + len, outsz - (int)len, " %s", trig);
            out[outsz - 1] = 0;
        }
        return true;
    }
    char name[192] = { 0 };
    if (!bcrBannerName(base, e, name, sizeof name)) return false;
    char section[0x11] = { 0 };
    safeReadCStr(e + BCR_RC_BANNER_SECTION, section, sizeof section);
    for (char* p = section; *p; p++) if (*p == '_') *p = ' ';
    char head[256], sec[128] = { 0 };
    _snprintf(head, sizeof head, axs(AXS_PB_REWARD_FMT), name, axs(AXS_PB_KIND_BANNER));
    head[sizeof head - 1] = 0;
    if (section[0]) {
        _snprintf(sec, sizeof sec, axs(AXS_PB_BANNER_SECTION_FMT), section);
        sec[sizeof sec - 1] = 0;
    }
    _snprintf(out, outsz, "%s%s%s %s%s%s", head, sec[0] ? " " : "", sec, pos,
              enterPreview ? " " : "", enterPreview ? axs(AXS_PB_ENTER_PREVIEW) : "");
    out[outsz - 1] = 0;
    return true;
}

// ---- round 2: the banner piece's "View %s in banner designer?" ----
static bool pbFindBannerPiece(uintptr_t base, uintptr_t reward, uintptr_t* sectionOut,
                              uintptr_t* pieceOut, char* whyOut, int whySz) {
    whyOut[0] = 0;
    char want[0x21] = { 0 }, wantSec[0x11] = { 0 };
    safeReadCStr(reward + BCR_RC_BANNER_NAME, want, sizeof want);
    safeReadCStr(reward + BCR_RC_BANNER_SECTION, wantSec, sizeof wantSec);
    if (!want[0] || !wantSec[0]) { _snprintf(whyOut, whySz, "reward record has no name/section"); return false; }
    uintptr_t bc = bcrBuildingByHash(base, resHash("banner_customization"));
    if (!bc) { _snprintf(whyOut, whySz, "banner_customization not in the building map"); return false; }
    uintptr_t sb = 0, se = 0;
    if (!safeReadPtr(bc + PB_BC_SECTIONS_BEG, &sb) || !safeReadPtr(bc + PB_BC_SECTIONS_END, &se) ||
        sb <= 0x10000 || se < sb || (se - sb) / PB_BC_SECTION_STRIDE > 32) {
        _snprintf(whyOut, whySz, "sections vector unreadable"); return false;
    }
    for (uintptr_t s = sb; s < se; s += PB_BC_SECTION_STRIDE) {
        char sname[0x11] = { 0 };
        safeReadCStr(s + PB_BC_SECTION_NAME, sname, sizeof sname);
        if (strncmp(sname, wantSec, 0x10) != 0) continue;
        uintptr_t pb = 0, pe = 0;
        if (!safeReadPtr(s + PB_BC_PIECES_BEG, &pb) || !safeReadPtr(s + PB_BC_PIECES_END, &pe) ||
            pb <= 0x10000 || pe < pb || (pe - pb) / PB_BC_PIECE_STRIDE > 256) {
            _snprintf(whyOut, whySz, "section \"%s\" pieces vector unreadable", sname); return false;
        }
        for (uintptr_t p = pb; p < pe; p += PB_BC_PIECE_STRIDE) {
            char pname[0x21] = { 0 };
            safeReadCStr(p + PB_BC_PIECE_NAME, pname, sizeof pname);
            if (strncmp(pname, want, 0x20) != 0) continue;
            *sectionOut = s; *pieceOut = p;
            return true;
        }
        _snprintf(whyOut, whySz, "section \"%s\" has no piece \"%s\"", sname, want);
        return false;
    }
    _snprintf(whyOut, whySz, "no section \"%s\"", wantSec);
    return false;
}

static bool pbCallOpenBanner(uintptr_t base, uintptr_t panel, uintptr_t section, uintptr_t piece) {
    typedef void (*PbOpenBannerFn)(uintptr_t, uintptr_t, uintptr_t);
    PbOpenBannerFn fn = (PbOpenBannerFn)(base + PB_OPEN_BANNER_RVA);
    __try { fn(panel, section, piece); }
    __except (EXCEPTION_EXECUTE_HANDLER) { logLine("prizebox: OpenBannerCustomization FAULTED"); return false; }
    return true;
}

static void pbOpenBannerPiece(uintptr_t base, uintptr_t panel, uintptr_t reward) {
    uintptr_t section = 0, piece = 0;
    char why[160], name[192] = { 0 };
    bcrBannerName(base, reward, name, sizeof name);
    if (!pbFindBannerPiece(base, reward, &section, &piece, why, sizeof why)) {
        logLine("prizebox: banner piece \"%s\" not resolvable in the banner designer's registry: %s", name, why);
        postSpeech(axs(AXS_PB_BANNER_NOT_FOUND));
        return;
    }
    uint8_t unlocked = 0xff;
    safeReadU8(piece + PB_BC_PIECE_UNLOCKED, &unlocked);
    logLine("prizebox: opening the banner designer prompt for \"%s\" (section=%p piece=%p unlocked=%u)",
            name, (void*)section, (void*)piece, unlocked);
    if (!pbCallOpenBanner(base, panel, section, piece)) { postSpeech(axs(AXS_ACTION_FAILED)); return; }
    g_pbOpenUntil = GetTickCount() + 1500;
}

// ---- speaking ----
static const int PB_PRESTIGE_ROW = 0;

// The prestige row's text.
static bool pbPrestigeRowText(uintptr_t base, const PbLevel* rows, int n, char* out, int outsz) {
    char pres[384] = { 0 };
    if (!pbPrestigeLine(base, pres, sizeof pres)) return false;
    int cur = 0, pts, next, prev, earned = 0;
    if (pbPrestige(base, &cur, &pts, &next, &prev))
        for (int i = 0; i < n; i++) if (rows[i].level <= cur) earned++;
    char counts[160];
    _snprintf(counts, sizeof counts, axs(AXS_PB_LEVELS_FMT), n, earned);
    counts[sizeof counts - 1] = 0;
    _snprintf(out, outsz, "%s %s", pres, counts);
    out[outsz - 1] = 0;
    return true;
}

static void pbPlaceCursor(uintptr_t base, const PbLevel* rows, int n) {
    if (n <= 0) { g_pbRow = PB_PRESTIGE_ROW; g_pbReward = -1; return; }
    if (!g_pbRowInit) {
        int cur, pts, next, prev;
        g_pbRow = 1;
        if (pbPrestige(base, &cur, &pts, &next, &prev)) {
            for (int i = 0; i < n; i++) if (rows[i].level <= cur) g_pbRow = i + 1;   // the highest earned
        }
        g_pbRowInit = true;
        g_pbReward = -1;
    }
    if (g_pbRow > n) g_pbRow = n;
    if (g_pbRow < 0) g_pbRow = 0;
    if (g_pbRow == PB_PRESTIGE_ROW) { g_pbReward = -1; return; }
    int total = rows[g_pbRow - 1].nTrink + rows[g_pbRow - 1].nBanner;
    if (g_pbReward >= total) g_pbReward = total - 1;
}

static void pbSpeakCursor(uintptr_t base, const char* prefix) {
    PbLevel rows[PB_MAX_LEVELS];
    int n = pbLevels(base, rows, PB_MAX_LEVELS);
    char body[MAILBOX_SZ];
    if (n < 0) {
        _snprintf(body, sizeof body, "%s", axs(AXS_PB_UNREADABLE));
    } else if (n == 0) {
        _snprintf(body, sizeof body, "%s", axs(AXS_PB_NO_LEVELS));
    } else {
        pbPlaceCursor(base, rows, n);
        bool ok;
        if (g_pbRow == PB_PRESTIGE_ROW) ok = pbPrestigeRowText(base, rows, n, body, sizeof body);
        else if (g_pbReward >= 0)      ok = pbRewardText(base, &rows[g_pbRow - 1], g_pbReward, body, sizeof body);
        else                           ok = pbRowText(base, rows, n, g_pbRow - 1, body, sizeof body);
        if (!ok) _snprintf(body, sizeof body, "%s", axs(AXS_PB_UNREADABLE));
    }
    body[sizeof body - 1] = 0;
    char utter[MAILBOX_SZ];
    _snprintf(utter, sizeof utter, "%s%s", prefix ? prefix : "", body);
    utter[sizeof utter - 1] = 0;
    postSpeech(utter);
}

bool pbSpeakArrival(uintptr_t base, uintptr_t panel, const char* prefix) {
    if (!pbIsPanel(base, panel)) return false;
    if (!pbBuilt(panel)) {
        g_pbWaitBuild = true;
        g_pbBuiltSeen = false;
        char pres[384] = { 0 };
        if (!pbPrestigeLine(base, pres, sizeof pres)) {
            logLine("prizebox: prestige ints unreadable (booth=%p)", (void*)pbBooth(base));
            pres[0] = 0;
        }
        char utter[MAILBOX_SZ];
        _snprintf(utter, sizeof utter, "%s%s%s%s", prefix ? prefix : "", pres, pres[0] ? " " : "",
                  axs(AXS_PB_LOADING));
        utter[sizeof utter - 1] = 0;
        postSpeech(utter);
        logLine("prizebox: arrived before the list was built (+0x700 = 0) -- waiting for the edge");
        return true;
    }
    g_pbWaitBuild = false;
    g_pbBuiltSeen = true;
    pbSpeakCursor(base, prefix);
    return true;
}

void pbService(uintptr_t base, uintptr_t panel) {
    if (g_pbOpenUntil) {
        if (confirmDialogOpen(base)) {
            g_pbOpenUntil = 0;                       // the ConfirmDialog reader speaks the question
            logLine("prizebox: banner designer confirm observed");
        } else if (!panel || !pbIsPanel(base, panel)) {
            g_pbOpenUntil = 0;
            logLine("prizebox: banner prompt watch ended by the screen closing");
        } else if (GetTickCount() > g_pbOpenUntil) {
            g_pbOpenUntil = 0;
            logLine("prizebox: banner prompt watch timed out -- no confirm dialog appeared");
            postSpeech(axs(AXS_DIDNT_HAPPEN));
        }
    }
    if (!pbIsPanel(base, panel)) return;
    bool built = pbBuilt(panel);
    if (built && !g_pbBuiltSeen) {
        g_pbBuiltSeen = true;
        if (g_pbWaitBuild) {
            g_pbWaitBuild = false;
            logLine("prizebox: the list was built after arrival -- announcing");
            char prefix[128];
            _snprintf(prefix, sizeof prefix, "%s ", axs(AXS_PB_LOADED));
            prefix[sizeof prefix - 1] = 0;
            g_pbRowInit = false;                 // land on the current level, as a fresh arrival
            pbSpeakArrival(base, panel, prefix);
        }
    } else if (!built && g_pbBuiltSeen) {
        g_pbBuiltSeen = false;
        g_pbWaitBuild = true;
        logLine("prizebox: +0x700 dropped to 0 while open");
    }
}

// ---- keys ----
bool pbRouteKey(uintptr_t base, uintptr_t panel, uint32_t sym, uint8_t repeat) {
    if (!pbIsPanel(base, panel)) return false;
    const bool nav = sym == SDLK_UP || sym == SDLK_DOWN || sym == SDLK_LEFT || sym == SDLK_RIGHT ||
                     sym == SDLK_HOME || sym == SDLK_END || sym == SDLK_RETURN || sym == SDLK_KP_ENTER;
    if (!pbBuilt(panel)) {
        if (!nav) return false;
        if (repeat) return true;
        postSpeech(axs(AXS_PB_LOADING));
        return true;
    }
    PbLevel rows[PB_MAX_LEVELS];
    int n = pbLevels(base, rows, PB_MAX_LEVELS);

    if (sym == SDLK_ESCAPE) {
        if (g_pbReward < 0) return false;
        if (repeat) return true;
        g_pbReward = -1;
        pbSpeakCursor(base, nullptr);
        return true;
    }
    if (!nav) return false;
    if (n <= 0) {
        if (repeat) return true;
        postSpeech(axs(n < 0 ? AXS_PB_UNREADABLE : AXS_PB_NO_LEVELS));
        return true;
    }
    pbPlaceCursor(base, rows, n);

    {
        int jump = 0;
        if (axDecodeJump(sym, 0, repeat, &jump)) {
            if (!jump) return true;                  // held jump: one landing per press
            g_pbReward = -1;
            axStepCursor(&g_pbRow, n + 1, jump);
            pbSpeakCursor(base, nullptr);
            return true;
        }
    }
    if (sym == SDLK_UP || sym == SDLK_DOWN) {
        if (axNavHoldRepeat(repeat)) return true;    // throttled repeat: claimed, no step
        g_pbReward = -1;                             // a new row lands on the row itself
        axStepCursor(&g_pbRow, n + 1, sym == SDLK_DOWN ? 1 : -1);
        pbSpeakCursor(base, nullptr);
        return true;
    }
    if (g_pbRow == PB_PRESTIGE_ROW &&
        (sym == SDLK_RIGHT || sym == SDLK_LEFT || sym == SDLK_RETURN || sym == SDLK_KP_ENTER)) {
        if (repeat) return true;
        pbSpeakCursor(base, nullptr);
        return true;
    }
    const PbLevel* lv = &rows[g_pbRow - 1];
    if (sym == SDLK_RIGHT || sym == SDLK_LEFT) {
        if (axNavHoldRepeat(repeat)) return true;
        int total = lv->nTrink + lv->nBanner;
        if (sym == SDLK_RIGHT) {
            if (total <= 0) { postSpeech(axs(AXS_PB_NO_REWARDS)); return true; }
            if (g_pbReward < total - 1) g_pbReward++;   // the last reward is a hard stop, re-read
        } else {
            if (g_pbReward >= 0) g_pbReward--;          // -1 = the row again
        }
        pbSpeakCursor(base, nullptr);
        return true;
    }
    if (sym == SDLK_RETURN || sym == SDLK_KP_ENTER) {
        if (repeat) return true;
        if (g_pbReward < 0) {                        // Enter on the row = into its rewards
            int total = lv->nTrink + lv->nBanner;
            if (total <= 0) { postSpeech(axs(AXS_PB_NO_REWARDS)); return true; }
            g_pbReward = 0;
            pbSpeakCursor(base, nullptr);
            return true;
        }
        uintptr_t e = 0;
        int kind = pbRewardAt(lv, g_pbReward, &e);
        if (kind == 2) {
            if (g_pbOpenUntil) return true;
            pbOpenBannerPiece(base, panel, e);
            return true;
        }
        pbSpeakCursor(base, nullptr);
        return true;
    }
    return false;
}

// ---- the probe ----
void pbProbe(uintptr_t base, uintptr_t panel) {
    if (!panel) { logDump("prizebox probe: no panel"); return; }
    uintptr_t vft = 0;
    safeReadPtr(panel, &vft);
    logDump("prizebox probe: panel=%p vft=%p (expect base+0x%llx = %p) match=%d",
            (void*)panel, (void*)vft, (unsigned long long)PB_VFT_RVA, (void*)(base + PB_VFT_RVA),
            pbIsPanel(base, panel) ? 1 : 0);
    if (!pbIsPanel(base, panel)) {
        logDump("prizebox probe: NOT the prize box panel -> nothing else is trustworthy here");
        return;
    }
    uintptr_t boothPanel = 0, boothMap = pbBooth(base);
    safeReadPtr(panel + PB_BOOTH_OFF, &boothPanel);
    logDump("prizebox probe: booth via panel+0x260=%p, via Campaign map=%p %s",
            (void*)boothPanel, (void*)boothMap,
            boothPanel == boothMap ? "(agree)" : "<-- DISAGREE: +0x260 is not the booth, or the map walk is off");
    if (boothMap) {
        uint32_t lvl = 0, pts = 0, next = 0, prev = 0;
        uint8_t f[4] = { 0xff, 0xff, 0xff, 0xff };
        uintptr_t data = 0; uint8_t c1 = 0xff, c3 = 0xff;
        safeReadU32(boothMap + PB_BOOTH_LEVEL_OFF, &lvl); safeReadU32(boothMap + PB_BOOTH_PTS_OFF, &pts);
        safeReadU32(boothMap + PB_BOOTH_NEXT_OFF, &next); safeReadU32(boothMap + PB_BOOTH_PREV_OFF, &prev);
        for (int i = 0; i < 4; i++) safeReadU8(boothMap + PB_BOOTH_FLAGS_OFF + i, &f[i]);
        if (safeReadPtr(boothMap + PB_BOOTH_DATA_OFF, &data) && data > 0x10000) {
            safeReadU8(data + PB_DATA_C1_OFF, &c1); safeReadU8(data + PB_DATA_C3_OFF, &c3);
        }
        logDump("prizebox probe: booth level(+0x120)=%u pts(+0x124)=%u next(+0x128)=%u prev(+0x12c)=%u "
                "-> have %d / need %d; flags(+0xa8..ab)=%u,%u,%u,%u data(+0x168)=%p c1=%u c3=%u "
                "(gate: a8&&a9&&!aa&&!ab&&data&&c1==0&&c3!=0)",
                lvl, pts, next, prev, (int)pts - (int)prev, (int)next - (int)prev,
                f[0], f[1], f[2], f[3], (void*)data, c1, c3);
    }
    uint32_t selLvl = 0, selIdx = 0, speed = 0;
    uint8_t built = 0xff, ctrl = 0xff, inspect = 0xff;
    safeReadU32(panel + PB_SEL_LEVEL_OFF, &selLvl); safeReadU32(panel + PB_SEL_IDX_OFF, &selIdx);
    safeReadU32(panel + PB_SCROLLSPEED_OFF, &speed);
    safeReadU8(panel + PB_BUILT_OFF, &built); safeReadU8(panel + PB_CTRLMODE_OFF, &ctrl);
    safeReadU8(panel + PB_INSPECT_OFF, &inspect);
    logDump("prizebox probe: selLevel(+0x6f8)=%u selIdx(+0x6fc)=%u scrollSpeed(+0x6ec)=%.1f "
            "built(+0x700)=%u ctrlMode(+0x701)=%u inspect(+0x702)=%u",
            selLvl, selIdx, u32AsFloatM(speed), built, ctrl, inspect);

    PbLevel rows[PB_MAX_LEVELS];
    int n = pbLevels(base, rows, PB_MAX_LEVELS);
    uintptr_t vb = 0, ve = 0;
    if (boothMap) { safeReadPtr(boothMap + BCR_PB_LVLVEC_BEG, &vb); safeReadPtr(boothMap + BCR_PB_LVLVEC_END, &ve); }
    logDump("prizebox probe: level vector(+0x170..+0x178)=[%p,%p) -> %ld records, %d with rewards",
            (void*)vb, (void*)ve, (vb && ve >= vb) ? (long)((ve - vb) / BCR_PB_LVLREC_STRIDE) : -1L, n);
    if (n > 0 && rows[0].nTrink > 0) {
        uintptr_t e = 0; uint32_t hash = 0;
        if (pbRewardAt(&rows[0], 0, &e) == 1 && safeReadU32(e + BCR_RC_TRINK_HASH, &hash))
            bldItemClassProbe(base, hash);
    }
    for (int i = 0; i < n && i < 12; i++) {
        char names[512] = { 0 };
        int total = rows[i].nTrink + rows[i].nBanner;
        for (int k = 0; k < total; k++) {
            char nm[160];
            if (!pbRewardName(base, &rows[i], k, nm, sizeof nm)) _snprintf(nm, sizeof nm, "(unreadable)");
            size_t len = strlen(names);
            _snprintf(names + len, sizeof names - len, "%s%s", len ? ", " : "", nm);
        }
        logDump("prizebox probe:   level %d rec=%p trinkets=%d banners=%d: %s",
                rows[i].level, (void*)rows[i].rec, rows[i].nTrink, rows[i].nBanner, names);
    }

    // The scroll cell and the pitch.
    uintptr_t cb = 0, ce = 0, cell = 0;
    bool cellsOk = safeReadPtr(panel + PB_CELLVEC_BEG, &cb) && safeReadPtr(panel + PB_CELLVEC_END, &ce);
    long cellN = (cellsOk && cb && ce >= cb && !((ce - cb) % 8)) ? (long)((ce - cb) / 8) : -1;
    logDump("prizebox probe: cellVec(+0x3b8..+0x3c0)=[%p,%p) -> %ld (expect 1: the outer cell)",
            (void*)cb, (void*)ce, cellN);
    if (cellN >= 1 && safeReadPtr(cb, &cell) && cell > 0x10000) {
        uint32_t ext = 0, tgt = 0, view = 0, scr = 0;
        uintptr_t bar = 0, content = 0;
        safeReadU32(cell + PB_CELL_EXTENT_OFF, &ext); safeReadU32(cell + PB_CELL_TARGET_OFF, &tgt);
        safeReadU32(cell + PB_CELL_VIEW_OFF, &view);  safeReadU32(cell + PB_CELL_SCROLL_OFF, &scr);
        safeReadPtr(cell + PB_CELL_SCROLLBAR_OFF, &bar); safeReadPtr(cell + PB_CELL_CONTENT_OFF, &content);
        float extent = u32AsFloatM(ext);
        long recs = (vb && ve >= vb) ? (long)((ve - vb) / BCR_PB_LVLREC_STRIDE) : 0;
        logDump("prizebox probe: cell=%p extent(+0xb4)=%.1f target(+0xb8)=%.1f view(+0xbc)=%.1f "
                "scroll(+0xc0)=%.1f scrollbar(+0xf8)=%p content(+0x150)=%p looksLikeWidget=%d "
                "-> pitch = extent/rows = %.1f (rows=%d) or extent/records = %.1f (records=%ld)",
                (void*)cell, extent, u32AsFloatM(tgt), u32AsFloatM(view), u32AsFloatM(scr),
                (void*)bar, (void*)content,
                (content > 0x10000 && tlLooksLikeWidget(base, content)) ? 1 : 0,
                n > 0 ? extent / n : 0.f, n, recs > 0 ? extent / recs : 0.f, recs);
    }
    feDumpFocusVector("prize box open");
}
