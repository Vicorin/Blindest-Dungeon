// dlc/butchers_circus/banner.cpp -- THE BANNER DESIGNER (the Circus's `banner_customization` building

#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <windows.h>
#include "internal.h"

// ---- identity ----

// ---- the panel (BannerCustomizationDisplay) ----
static const uintptr_t BD_P_SECTION_OFF   = 0x0a0;  // uint: the current section
static const uintptr_t BD_P_BLD_OFF       = 0x260;  // Building::BannerCustomization*
static const uintptr_t BD_P_SHOWN_BEG     = 0x268;  // vector<BannerPiece*> begin: the grid, in order
static const uintptr_t BD_P_SHOWN_END     = 0x270;
static const uintptr_t BD_P_PAGE_OFF      = 0x718;  // uint: pre-scroll page
static const uintptr_t BD_P_BUILT_OFF     = 0x71c;  // byte: SelectSection has built the grid
static const uintptr_t BD_P_TABFOCUS_OFF  = 0x71d;  // byte: controller "tabs focused"
static const uintptr_t BD_P_TIPS_OFF      = 0x71e;  // byte: tooltips allowed this frame
static const uintptr_t BD_P_INPUTMODE_OFF = 0x71f;  // byte: the input-mode snapshot

// ---- the building (Building::BannerCustomization) ----
static const uintptr_t BD_B_FLAG_C0_OFF   = 0x0c0;  // byte: must be 0
static const uintptr_t BD_B_FLAG_C1_OFF   = 0x0c1;  // byte: a request is in flight (must be 0)
static const uintptr_t BD_B_FLAG_C3_OFF   = 0x0c3;  // byte: catalog data present (must be set)
static const uintptr_t BD_S_WORKING_OFF   = 0x20;   // section+: uint working (preview) piece index
static const uintptr_t BD_S_APPLIED_OFF   = 0x24;   // section+: uint applied piece index
static const uintptr_t BD_PC_PRESTIGE_BEG = 0x08;   // piece+: vector<int> begin
static const uintptr_t BD_PC_PRESTIGE_END = 0x10;
static const uintptr_t BD_PC_ACH_BEG      = 0x20;   // piece+: vector<char[0x40]> begin
static const uintptr_t BD_PC_ACH_END      = 0x28;
static const uintptr_t BD_PC_ACH_STRIDE   = 0x40;
static const uintptr_t BD_PC_INDEX_OFF    = 0x5c;   // piece+: uint, its own index in the section
static const uintptr_t BD_PC_RARITY_OFF   = 0x8c;   // piece+: char[0x20] rarity name
static const uintptr_t BD_PC_SEEN_OFF     = 0xb4;   // piece+: byte, hovered once
static const int       BD_MAX_SECTIONS    = 16;     // 6 shipped; a guard, not a table
static const int       BD_MAX_PIECES      = 256;    // 26 is the biggest section on the dev's save
static const int       BD_MAX_ACH         = 8;      // conditions per piece; a guard

// ---- the cursor ----
enum { BD_PANE_TABS = 0, BD_PANE_GRID = 1 };
static int   g_bdPane        = BD_PANE_TABS;
static int   g_bdTabRow      = 0;
static int   g_bdGridIdx     = 0;      // index into the SHOWN vector
static int   g_bdLastSection = -1;     // to notice a section change made by neither watch
static bool  g_bdLoadPending = false;  // arrived before the grid was built
// the watches
static int   g_bdSwitchTo    = -1;     // a section-icon click is in flight, aimed here
static DWORD g_bdSwitchUntil = 0;
static DWORD g_bdRandomUntil = 0;      // a Random click is in flight
static DWORD g_bdApplyUntil  = 0;      // an Apply click is in flight
static bool  g_bdApplyExpectDialog = false;   // ... and the game will refuse it (locked preview)
static uint32_t g_bdSnapWorking[BD_MAX_SECTIONS];   // working indices before Random
static int   g_bdSnapCount   = 0;

bool bdIsPanel(uintptr_t base, uintptr_t panel) {
    uintptr_t vft = 0;
    if (!panel || !safeReadPtr(panel, &vft) || vft <= base) return false;
    return vft - base == BD_VFT_RVA;
}

bool bdIsOpen(uintptr_t base) {
    uintptr_t root = resTownRoot(base);
    uintptr_t panel = root ? bldOpenPanel(root) : 0;
    return bdIsPanel(base, panel);
}

void bdReset() {
    g_bdPane = BD_PANE_TABS; g_bdTabRow = 0; g_bdGridIdx = 0; g_bdLastSection = -1;
    g_bdLoadPending = false;
    g_bdSwitchTo = -1; g_bdSwitchUntil = 0; g_bdRandomUntil = 0; g_bdApplyUntil = 0;
    g_bdApplyExpectDialog = false; g_bdSnapCount = 0;
}

// ---- reading the building ----
static uintptr_t bdBuilding(uintptr_t base) {
    return bcrBuildingByHash(base, resHash("banner_customization"));
}

static bool bdFlagsGood(uintptr_t bld) {
    uint8_t c0 = 0xff, c1 = 0xff, c3 = 0;
    if (!bld) return false;
    safeReadU8(bld + BD_B_FLAG_C0_OFF, &c0); safeReadU8(bld + BD_B_FLAG_C1_OFF, &c1);
    safeReadU8(bld + BD_B_FLAG_C3_OFF, &c3);
    return c0 == 0 && c1 == 0 && c3 != 0;
}

static bool bdBuilt(uintptr_t panel) {
    uint8_t b = 0;
    return safeReadU8(panel + BD_P_BUILT_OFF, &b) && b != 0;
}

// The sections vector: count (-1 = unreadable) and begin.
static int bdSections(uintptr_t bld, uintptr_t* begOut) {
    if (!bld) return -1;
    uintptr_t sb = 0, se = 0;
    if (!safeReadPtr(bld + PB_BC_SECTIONS_BEG, &sb) || !safeReadPtr(bld + PB_BC_SECTIONS_END, &se)) return -1;
    if (sb == 0 && se == 0) { if (begOut) *begOut = 0; return 0; }
    if (sb <= 0x10000 || se < sb || (se - sb) % PB_BC_SECTION_STRIDE) return -1;
    int n = (int)((se - sb) / PB_BC_SECTION_STRIDE);
    if (n > BD_MAX_SECTIONS) n = BD_MAX_SECTIONS;
    if (begOut) *begOut = sb;
    return n;
}
static uintptr_t bdSectionAt(uintptr_t bld, int i) {
    uintptr_t beg = 0;
    int n = bdSections(bld, &beg);
    if (i < 0 || i >= n || !beg) return 0;
    return beg + (uintptr_t)i * PB_BC_SECTION_STRIDE;
}
static int bdSection(uintptr_t panel) {
    uint32_t s = 0;
    if (!safeReadU32(panel + BD_P_SECTION_OFF, &s) || s >= (uint32_t)BD_MAX_SECTIONS) return -1;
    return (int)s;
}

// A section's pieces vector: count (-1 = unreadable) and begin.
static int bdPieces(uintptr_t section, uintptr_t* begOut) {
    if (!section) return -1;
    uintptr_t pb = 0, pe = 0;
    if (!safeReadPtr(section + PB_BC_PIECES_BEG, &pb) || !safeReadPtr(section + PB_BC_PIECES_END, &pe)) return -1;
    if (pb == 0 && pe == 0) { if (begOut) *begOut = 0; return 0; }
    if (pb <= 0x10000 || pe < pb || (pe - pb) % PB_BC_PIECE_STRIDE) return -1;
    int n = (int)((pe - pb) / PB_BC_PIECE_STRIDE);
    if (n > BD_MAX_PIECES) n = BD_MAX_PIECES;
    if (begOut) *begOut = pb;
    return n;
}
// The piece a section's +0x20 / +0x24 index names (0 = none).
static uintptr_t bdPieceByIndex(uintptr_t section, uint32_t idx) {
    uintptr_t beg = 0;
    int n = bdPieces(section, &beg);
    if (n <= 0 || idx >= (uint32_t)n) return 0;
    return beg + (uintptr_t)idx * PB_BC_PIECE_STRIDE;
}
static uint32_t bdWorking(uintptr_t section) { uint32_t v = 0xffffffff; if (section) safeReadU32(section + BD_S_WORKING_OFF, &v); return v; }
static uint32_t bdApplied(uintptr_t section) { uint32_t v = 0xffffffff; if (section) safeReadU32(section + BD_S_APPLIED_OFF, &v); return v; }
static bool bdPieceUnlocked(uintptr_t piece) { uint8_t u = 0; return piece && safeReadU8(piece + PB_BC_PIECE_UNLOCKED, &u) && u != 0; }

static int bdShown(uintptr_t panel, uintptr_t* out, int maxOut) {
    uintptr_t b = 0, e = 0;
    if (!safeReadPtr(panel + BD_P_SHOWN_BEG, &b) || !safeReadPtr(panel + BD_P_SHOWN_END, &e)) return -1;
    if (b == 0 && e == 0) return 0;
    if (b <= 0x10000 || e < b || (e - b) % 8) return -1;
    int n = (int)((e - b) / 8);
    if (n > maxOut) n = maxOut;
    for (int i = 0; i < n; i++) {
        uintptr_t p = 0;
        if (!safeReadPtr(b + (uintptr_t)i * 8, &p) || p <= 0x10000) return -1;
        out[i] = p;
    }
    return n;
}
static int bdPerRow(uintptr_t base) {
    uint32_t v = 0;
    if (safeReadU32(base + BD_PIECES_PER_ROW_RVA, &v) && v >= 1 && v <= 16) return (int)v;
    logLine("banner: pieces_per_row global read %u -- using 7", v);
    return 7;
}

// "Unsaved changes" and "a locked piece is previewed", the game's own two tests.
static void bdChangeState(uintptr_t bld, bool* changedOut, bool* lockedPreviewOut) {
    bool changed = false, lockedPrev = false;
    uintptr_t beg = 0;
    int n = bdSections(bld, &beg);
    for (int i = 0; i < n; i++) {
        uintptr_t s = beg + (uintptr_t)i * PB_BC_SECTION_STRIDE;
        uint32_t w = bdWorking(s), a = bdApplied(s);
        uintptr_t wp = bdPieceByIndex(s, w);
        bool unlocked = bdPieceUnlocked(wp);
        if (!unlocked && wp) lockedPrev = true;
        if (w != a && unlocked) changed = true;
    }
    if (changedOut) *changedOut = changed;
    if (lockedPreviewOut) *lockedPreviewOut = lockedPrev;
}

// ---- text ----
static bool bdFmtOneStr(const char* fmt) {
    int pct = 0, ok = 0;
    for (const char* p = fmt; *p; p++) if (*p == '%') { pct++; if (p[1] == 's') ok++; }
    return pct == 1 && ok == 1;
}
static bool bdFmtOneInt(const char* fmt) {
    int pct = 0, ok = 0;
    for (const char* p = fmt; *p; p++) if (*p == '%') { pct++; if (p[1] == 'd' || p[1] == 'u') ok++; }
    return pct == 1 && ok == 1;
}
static void bdAppend(char* out, int outsz, const char* piece, const char* sep) {
    if (!piece || !piece[0]) return;
    size_t len = strlen(out);
    _snprintf(out + len, outsz - (int)len, "%s%s", len ? sep : "", piece);
    out[outsz - 1] = 0;
}

static void bdSectionName(uintptr_t section, char* out, int outsz) {
    out[0] = 0;
    if (!section || !safeReadCStr(section + PB_BC_SECTION_NAME, out, outsz < 0x11 ? outsz : 0x11)) return;
    out[outsz - 1] = 0;
    for (char* p = out; *p; p++) if (*p == '_') *p = ' ';
    if (out[0] >= 'a' && out[0] <= 'z') out[0] = (char)(out[0] - 'a' + 'A');   // a row label: "Crest"
}

static bool bdPieceName(uintptr_t base, uintptr_t piece, char* out, int outsz) {
    char name[0x21] = { 0 };
    if (!piece || !safeReadCStr(piece + PB_BC_PIECE_NAME, name, sizeof name) || !name[0]) return false;
    char key[128];
    _snprintf(key, sizeof key, "banner_piece_%s", name); key[sizeof key - 1] = 0;
    if (!resolveKey(base, key, out, outsz) || !out[0]) {
        logLine("banner: \"%s\" did not resolve -- speaking the raw name", key);
        _snprintf(out, outsz, "%s", name);
    }
    out[outsz - 1] = 0;
    abStripMarkup(out);
    return true;
}
static void bdPieceRarity(uintptr_t base, uintptr_t piece, char* out, int outsz) {
    out[0] = 0;
    char r[0x21] = { 0 };
    if (!piece || !safeReadCStr(piece + BD_PC_RARITY_OFF, r, sizeof r) || !r[0]) return;
    char key[128];
    _snprintf(key, sizeof key, "banner_piece_rarity_%s", r); key[sizeof key - 1] = 0;
    if (!resolveKey(base, key, out, outsz) || !out[0]) {
        logLine("banner: \"%s\" did not resolve -- speaking the raw rarity", key);
        _snprintf(out, outsz, "%s", r);
    }
    out[outsz - 1] = 0;
    abStripMarkup(out);
}

static void bdPieceConditions(uintptr_t base, uintptr_t piece, bool locked, char* out, int outsz) {
    out[0] = 0;
    if (!piece) return;
    // prestige
    uintptr_t pb = 0, pe = 0;
    if (safeReadPtr(piece + BD_PC_PRESTIGE_BEG, &pb) && safeReadPtr(piece + BD_PC_PRESTIGE_END, &pe) &&
        pb > 0x10000 && pe >= pb && (pe - pb) % 4 == 0 && (pe - pb) / 4 <= 8) {
        char fmt[192] = { 0 };
        const char* key = locked ? "reward_locked_prestige_format" : "reward_earned_prestige_format";
        bool haveFmt = resolveKey(base, key, fmt, sizeof fmt) && fmt[0] && bdFmtOneInt(fmt);
        if (haveFmt) abStripMarkup(fmt);
        for (uintptr_t p = pb; p < pe; p += 4) {
            uint32_t v = 0;
            if (!safeReadU32(p, &v) || v == 0) continue;
            char line[256];
            if (haveFmt) _snprintf(line, sizeof line, fmt, (unsigned)v);
            else { logLine("banner: \"%s\" missing or not a one-int format", key); _snprintf(line, sizeof line, "%u", (unsigned)v); }
            line[sizeof line - 1] = 0;
            bdAppend(out, outsz, line, ". ");
        }
    }
    // achievements
    uintptr_t ab = 0, ae = 0;
    if (safeReadPtr(piece + BD_PC_ACH_BEG, &ab) && safeReadPtr(piece + BD_PC_ACH_END, &ae) &&
        ab > 0x10000 && ae >= ab && (ae - ab) % BD_PC_ACH_STRIDE == 0 && (ae - ab) / BD_PC_ACH_STRIDE <= (uintptr_t)BD_MAX_ACH) {
        char fmt[192] = { 0 };
        const char* key = locked ? "reward_locked_achievement_format" : "reward_earned_achievement_format";
        bool haveFmt = resolveKey(base, key, fmt, sizeof fmt) && fmt[0] && bdFmtOneStr(fmt);
        if (haveFmt) abStripMarkup(fmt);
        const char* achFmt = locked ? "achievement_%s" : "achievement_%s_completed";
        for (uintptr_t a = ab; a < ae; a += BD_PC_ACH_STRIDE) {
            char id[0x41] = { 0 };
            if (!safeReadCStr(a, id, sizeof id) || !id[0]) continue;
            char achKey[160], achText[256] = { 0 };
            const char* season = strstr(id, "season_");
            if (season) {
                const char* num = season + 7;
                int n = atoi(num);
                const char* rest = strchr(num, '_');
                char re[0x48];
                _snprintf(re, sizeof re, "season_x%s", rest ? rest : ""); re[sizeof re - 1] = 0;
                _snprintf(achKey, sizeof achKey, achFmt, re); achKey[sizeof achKey - 1] = 0;
                char raw[256] = { 0 };
                if (resolveKey(base, achKey, raw, sizeof raw) && raw[0]) {
                    abStripMarkup(raw);
                    if (bdFmtOneInt(raw)) _snprintf(achText, sizeof achText, raw, n);
                    else _snprintf(achText, sizeof achText, "%s", raw);
                }
            } else {
                _snprintf(achKey, sizeof achKey, achFmt, id); achKey[sizeof achKey - 1] = 0;
                if (resolveKey(base, achKey, achText, sizeof achText) && achText[0]) abStripMarkup(achText);
            }
            achText[sizeof achText - 1] = 0;
            if (!achText[0]) { logLine("banner: \"%s\" did not resolve -- speaking the id", achKey); _snprintf(achText, sizeof achText, "%s", id); for (char* p = achText; *p; p++) if (*p == '_') *p = ' '; }
            char line[320];
            if (haveFmt) _snprintf(line, sizeof line, fmt, achText);
            else { logLine("banner: \"%s\" missing or not a one-string format", key); _snprintf(line, sizeof line, "%s", achText); }
            line[sizeof line - 1] = 0;
            bdAppend(out, outsz, line, ". ");
        }
    }
}

static bool bdPieceText(uintptr_t base, uintptr_t section, uintptr_t piece, int i, int n, char* out, int outsz) {
    char name[192];
    if (!bdPieceName(base, piece, name, sizeof name)) return false;
    bool unlocked = bdPieceUnlocked(piece);
    char rarity[96], cond[768];
    bdPieceRarity(base, piece, rarity, sizeof rarity);
    bdPieceConditions(base, piece, !unlocked, cond, sizeof cond);
    uint32_t idx = 0xffffffff;
    safeReadU32(piece + BD_PC_INDEX_OFF, &idx);
    bool isWorking = idx == bdWorking(section), isApplied = idx == bdApplied(section);
    out[0] = 0;
    bdAppend(out, outsz, name, " ");
    if (rarity[0]) { bdAppend(out, outsz, ".", ""); bdAppend(out, outsz, rarity, " "); }
    if (cond[0])   { bdAppend(out, outsz, ".", ""); bdAppend(out, outsz, cond, " "); }
    bdAppend(out, outsz, ".", "");
    if (isWorking) bdAppend(out, outsz, axs(AXS_BD_PIECE_SELECTED), " ");
    if (isApplied) bdAppend(out, outsz, axs(AXS_BD_PIECE_APPLIED), " ");
    if (!unlocked) bdAppend(out, outsz, axs(AXS_BD_PIECE_LOCKED), " ");
    char pos[64];
    _snprintf(pos, sizeof pos, axs(AXS_BD_PIECE_N_OF_M), i + 1, n); pos[sizeof pos - 1] = 0;
    bdAppend(out, outsz, pos, " ");
    bdAppend(out, outsz, axs(unlocked ? AXS_BD_ENTER_SELECT : AXS_PB_ENTER_PREVIEW), " ");
    return true;
}

static bool bdTabText(uintptr_t base, uintptr_t panel, uintptr_t bld, int row, char* out, int outsz) {
    uintptr_t beg = 0;
    int ns = bdSections(bld, &beg);
    if (ns <= 0) return false;
    out[0] = 0;
    if (row < ns) {
        uintptr_t s = beg + (uintptr_t)row * PB_BC_SECTION_STRIDE;
        char sname[32], pname[192] = { 0 }, line[256];
        bdSectionName(s, sname, sizeof sname);
        if (row == bdSection(panel)) bdAppend(out, outsz, axs(AXS_BD_TAB_CURRENT), " ");
        uint32_t w = bdWorking(s), a = bdApplied(s);
        if (!bdPieceName(base, bdPieceByIndex(s, w), pname, sizeof pname))
            _snprintf(pname, sizeof pname, "%s", axs(AXS_BD_PIECE_UNKNOWN));
        _snprintf(line, sizeof line, axs(AXS_BD_BANNER_LINE_FMT), sname, pname); line[sizeof line - 1] = 0;
        bdAppend(out, outsz, line, " ");
        if (w != a) bdAppend(out, outsz, axs(AXS_BD_TAB_CHANGED), " ");
        _snprintf(line, sizeof line, axs(AXS_BD_TAB_SECTION_FMT), row + 1, ns); line[sizeof line - 1] = 0;
        bdAppend(out, outsz, line, " ");
        return true;
    }
    if (row == ns) { _snprintf(out, outsz, "%s", axs(AXS_BD_ROW_RANDOM)); out[outsz - 1] = 0; return true; }
    if (row == ns + 1) {
        char label[128] = { 0 };
        if (!resolveKey(base, "str_banner_customization_save", label, sizeof label) || !label[0]) {
            logLine("banner: str_banner_customization_save did not resolve");
            _snprintf(label, sizeof label, "%s", axs(AXS_BD_APPLY_FALLBACK));
        }
        label[sizeof label - 1] = 0;
        abStripMarkup(label);
        char line[256];
        _snprintf(line, sizeof line, axs(AXS_BD_ROW_APPLY_FMT), label); line[sizeof line - 1] = 0;
        bdAppend(out, outsz, line, " ");
        bool changed = false, lockedPrev = false;
        bdChangeState(bld, &changed, &lockedPrev);
        bdAppend(out, outsz, axs(changed ? AXS_BD_UNSAVED : AXS_BD_NO_CHANGES), " ");
        if (lockedPrev) bdAppend(out, outsz, axs(AXS_BD_LOCKED_PREVIEW), " ");
        return true;
    }
    return false;
}

static void bdBannerText(uintptr_t base, uintptr_t bld, char* out, int outsz) {
    uintptr_t beg = 0;
    int ns = bdSections(bld, &beg);
    _snprintf(out, outsz, "%s", axs(AXS_BD_BANNER_HEAD)); out[outsz - 1] = 0;
    for (int i = 0; i < ns; i++) {
        uintptr_t s = beg + (uintptr_t)i * PB_BC_SECTION_STRIDE;
        char sname[32], pname[192] = { 0 }, line[320];
        bdSectionName(s, sname, sizeof sname);
        uint32_t w = bdWorking(s), a = bdApplied(s);
        if (!bdPieceName(base, bdPieceByIndex(s, w), pname, sizeof pname))
            _snprintf(pname, sizeof pname, "%s", axs(AXS_BD_PIECE_UNKNOWN));
        _snprintf(line, sizeof line, axs(AXS_BD_BANNER_LINE_FMT), sname, pname); line[sizeof line - 1] = 0;
        bdAppend(out, outsz, line, " ");
        if (w != a) bdAppend(out, outsz, axs(AXS_BD_TAB_CHANGED), " ");
    }
}

// ---- speaking the cursor ----
static int bdTabCount(uintptr_t bld) { int ns = bdSections(bld, nullptr); return ns > 0 ? ns + 2 : 0; }

static void bdSpeakTab(uintptr_t base, uintptr_t panel, const char* prefix) {
    uintptr_t bld = bdBuilding(base);
    int total = bdTabCount(bld);
    if (total <= 0) { postSpeech(axs(AXS_BD_UNREADABLE)); return; }
    axStepCursor(&g_bdTabRow, total, 0);
    char body[MAILBOX_SZ];
    if (!bdTabText(base, panel, bld, g_bdTabRow, body, sizeof body)) {
        logLine("banner: tab row %d of %d unreadable", g_bdTabRow, total);
        _snprintf(body, sizeof body, "%s", axs(AXS_BD_ROW_UNREADABLE));
    }
    body[sizeof body - 1] = 0;
    char utter[MAILBOX_SZ];
    _snprintf(utter, sizeof utter, "%s%s", prefix ? prefix : "", body); utter[sizeof utter - 1] = 0;
    postSpeech(utter);
}

static void bdSpeakGrid(uintptr_t base, uintptr_t panel, const char* prefix) {
    uintptr_t bld = bdBuilding(base);
    uintptr_t section = bdSectionAt(bld, bdSection(panel));
    uintptr_t shown[BD_MAX_PIECES];
    int n = bdShown(panel, shown, BD_MAX_PIECES);
    char utter[MAILBOX_SZ];
    if (!section || n < 0) {
        logLine("banner: grid unreadable (section=%p shown=%d)", (void*)section, n);
        _snprintf(utter, sizeof utter, "%s%s", prefix ? prefix : "", axs(AXS_BD_UNREADABLE));
        utter[sizeof utter - 1] = 0; postSpeech(utter); return;
    }
    if (n == 0) {
        _snprintf(utter, sizeof utter, "%s%s", prefix ? prefix : "", axs(AXS_BD_NO_PIECES));
        utter[sizeof utter - 1] = 0; postSpeech(utter); return;
    }
    axStepCursor(&g_bdGridIdx, n, 0);
    char body[MAILBOX_SZ];
    if (!bdPieceText(base, section, shown[g_bdGridIdx], g_bdGridIdx, n, body, sizeof body)) {
        logLine("banner: piece %d of %d unreadable", g_bdGridIdx, n);
        _snprintf(body, sizeof body, "%s", axs(AXS_BD_ROW_UNREADABLE));
    }
    body[sizeof body - 1] = 0;
    _snprintf(utter, sizeof utter, "%s%s", prefix ? prefix : "", body); utter[sizeof utter - 1] = 0;
    postSpeech(utter);
}

static void bdSpeakCursor(uintptr_t base, uintptr_t panel, const char* prefix) {
    if (g_bdPane == BD_PANE_GRID) bdSpeakGrid(base, panel, prefix);
    else bdSpeakTab(base, panel, prefix);
}

// The shown index of the section's working piece (-1 = not shown).
static int bdShownIndexOfWorking(uintptr_t panel, uintptr_t section) {
    uintptr_t shown[BD_MAX_PIECES];
    int n = bdShown(panel, shown, BD_MAX_PIECES);
    uint32_t w = bdWorking(section);
    for (int i = 0; i < n; i++) {
        uint32_t idx = 0xffffffff;
        if (safeReadU32(shown[i] + BD_PC_INDEX_OFF, &idx) && idx == w) return i;
    }
    return -1;
}

bool bdSpeakArrival(uintptr_t base, uintptr_t panel, const char* prefix) {
    if (!bdIsPanel(base, panel)) return false;
    uintptr_t bld = bdBuilding(base);
    int section = bdSection(panel);
    char utter[MAILBOX_SZ];
    if (!bld || section < 0 || bdSections(bld, nullptr) <= 0) {
        logLine("banner: arrival with building=%p section=%d", (void*)bld, section);
        _snprintf(utter, sizeof utter, "%s%s", prefix ? prefix : "", axs(AXS_BD_UNREADABLE));
        utter[sizeof utter - 1] = 0; postSpeech(utter);
        return true;
    }
    g_bdPane = BD_PANE_TABS;
    g_bdTabRow = section;
    g_bdLastSection = section;
    g_bdGridIdx = 0;
    if (!bdBuilt(panel)) {
        g_bdLoadPending = true;
        logLine("banner: arrived before the grid was built (flags good=%d)", bdFlagsGood(bld) ? 1 : 0);
        _snprintf(utter, sizeof utter, "%s%s", prefix ? prefix : "", axs(AXS_BD_LOADING));
        utter[sizeof utter - 1] = 0; postSpeech(utter);
        return true;
    }
    int w = bdShownIndexOfWorking(panel, bdSectionAt(bld, section));
    if (w >= 0) g_bdGridIdx = w;
    bdSpeakTab(base, panel, prefix);
    return true;
}

// ---- the clicks: the game's own FocusElements ----
struct BnElems { uintptr_t section[BD_MAX_SECTIONS]; int nSection; uintptr_t random, apply; };
static bool bdCollectElems(uintptr_t base, int nSections, BnElems* out, bool loud) {
    uintptr_t els[BD_MAX_SECTIONS + 8]; float xs[BD_MAX_SECTIONS + 8], cx[BD_MAX_SECTIONS + 8], cy[BD_MAX_SECTIONS + 8];
    int n = feCollectFamilyByX(base, BD_ELEM_ID, 0, els, xs, BD_MAX_SECTIONS + 8);
    memset(out, 0, sizeof *out);
    if (n <= 0) { if (loud) { logLine("banner: no 'isb' elements on screen"); diagDumpFocusElements(base, "banner-elems"); } return false; }
    for (int i = 0; i < n; i++)
        if (!elemCenter(els[i], &cx[i], &cy[i])) { if (loud) logLine("banner: 'isb' element %d has no rect", i); return false; }
    for (int i = 1; i < n; i++) {                    // ascending Y
        int k = i;
        while (k > 0 && cy[k - 1] > cy[k]) {
            float tx = cx[k], ty = cy[k]; uintptr_t te = els[k];
            cx[k] = cx[k - 1]; cy[k] = cy[k - 1]; els[k] = els[k - 1];
            cx[k - 1] = tx; cy[k - 1] = ty; els[k - 1] = te; k--;
        }
    }
    if (n != nSections + 2) {
        if (loud) { logLine("banner: expected %d 'isb' elements (%d sections + 2 buttons), found %d", nSections + 2, nSections, n); diagDumpFocusElements(base, "banner-elems"); }
        if (n < nSections) return false;
    }
    out->nSection = nSections;
    for (int i = 0; i < nSections; i++) out->section[i] = els[i];
    if (n >= nSections + 2) {
        int a = nSections, b = nSections + 1;
        if (cx[a] <= cx[b]) { out->random = els[a]; out->apply = els[b]; }
        else                { out->random = els[b]; out->apply = els[a]; }
    }
    return true;
}

static bool bdClickElem(uintptr_t elem, const char* what) {
    float cx = 0, cy = 0;
    if (!elem || !elemCenter(elem, &cx, &cy)) { logLine("banner: %s element missing or without a rect", what); return false; }
    if (clickQueued()) { logLine("banner: %s click refused, a click is already queued", what); return false; }
    logLine("banner: clicking %s at (%.0f,%.0f)", what, cx, cy);
    moveCursorTo(cx, cy);                                       // pins the globals + hover (frame 0)
    enqueueSynth(SDL_EVT_MOUSEBUTTONDOWN, SDL_BUTTON_LEFT, 1, 1);
    enqueueSynth(SDL_EVT_MOUSEBUTTONUP,   SDL_BUTTON_LEFT, 0, 2);
    return true;
}

static bool bdBusy() { return g_bdSwitchUntil || g_bdRandomUntil || g_bdApplyUntil || clickQueued(); }

static void bdClickSection(uintptr_t base, uintptr_t panel, int target) {
    uintptr_t bld = bdBuilding(base);
    int ns = bdSections(bld, nullptr);
    if (target < 0 || target >= ns) return;
    if (target == bdSection(panel)) {                 // the game's SelectSection is a no-op here
        g_bdTabRow = target;
        bdSpeakTab(base, panel, nullptr);
        return;
    }
    if (bdBusy()) { postSpeech(axs(AXS_STILL_SWITCHING)); return; }
    BnElems e;
    if (!bdCollectElems(base, ns, &e, true) || !bdClickElem(e.section[target], "section icon")) {
        postSpeech(axs(AXS_BD_SECTION_STUCK)); return;
    }
    g_bdSwitchTo = target;
    g_bdSwitchUntil = GetTickCount() + 1500;          // bdService announces the landing
}

static void bdClickRandom(uintptr_t base, uintptr_t panel) {
    uintptr_t bld = bdBuilding(base);
    uintptr_t beg = 0;
    int ns = bdSections(bld, &beg);
    if (ns <= 0) { postSpeech(axs(AXS_BD_UNREADABLE)); return; }
    if (bdBusy()) { postSpeech(axs(AXS_STILL_SWITCHING)); return; }
    BnElems e;
    if (!bdCollectElems(base, ns, &e, true) || !bdClickElem(e.random, "Random")) { postSpeech(axs(AXS_BD_BUTTON_STUCK)); return; }
    g_bdSnapCount = ns;
    for (int i = 0; i < ns; i++) g_bdSnapWorking[i] = bdWorking(beg + (uintptr_t)i * PB_BC_SECTION_STRIDE);
    g_bdRandomUntil = GetTickCount() + 1500;
}

static void bdClickApply(uintptr_t base, uintptr_t panel) {
    uintptr_t bld = bdBuilding(base);
    int ns = bdSections(bld, nullptr);
    if (ns <= 0) { postSpeech(axs(AXS_BD_UNREADABLE)); return; }
    if (bdBusy()) { postSpeech(axs(AXS_STILL_SWITCHING)); return; }
    bool changed = false, lockedPrev = false;
    bdChangeState(bld, &changed, &lockedPrev);
    if (!changed && !lockedPrev) {                    // the game plays the save sound and stops
        postSpeech(axs(AXS_BD_NO_CHANGES));
        return;
    }
    BnElems e;
    if (!bdCollectElems(base, ns, &e, true) || !bdClickElem(e.apply, "Apply All")) { postSpeech(axs(AXS_BD_BUTTON_STUCK)); return; }
    g_bdApplyExpectDialog = lockedPrev;               // the game refuses with its notice
    g_bdApplyUntil = GetTickCount() + 2500;
}

static void bdSelectPiece(uintptr_t base, uintptr_t panel) {
    uintptr_t bld = bdBuilding(base);
    int sectionIdx = bdSection(panel);
    uintptr_t section = bdSectionAt(bld, sectionIdx);
    uintptr_t shown[BD_MAX_PIECES];
    int n = bdShown(panel, shown, BD_MAX_PIECES);
    if (!section || n <= 0) { postSpeech(axs(n == 0 ? AXS_BD_NO_PIECES : AXS_BD_UNREADABLE)); return; }
    if (bdBusy()) { postSpeech(axs(AXS_STILL_SWITCHING)); return; }
    axStepCursor(&g_bdGridIdx, n, 0);
    uintptr_t target = shown[g_bdGridIdx];
    uint32_t targetIdx = 0xffffffff;
    safeReadU32(target + BD_PC_INDEX_OFF, &targetIdx);
    char name[192] = { 0 };
    bdPieceName(base, target, name, sizeof name);
    bool unlocked = bdPieceUnlocked(target);
    if (targetIdx == bdWorking(section)) {            // already the working piece: say so
        char utter[MAILBOX_SZ];
        _snprintf(utter, sizeof utter, axs(unlocked ? AXS_BD_SELECTED_FMT : AXS_BD_PREVIEWING_FMT), name);
        utter[sizeof utter - 1] = 0; postSpeech(utter);
        return;
    }
    int cur = bdShownIndexOfWorking(panel, section);
    if (cur < 0) {
        logLine("banner: working piece %u of section %d is not in the shown vector -- no step", bdWorking(section), sectionIdx);
        postSpeech(axs(AXS_BD_SELECT_DIDNT));
        return;
    }
    int delta = g_bdGridIdx - cur;
    logLine("banner: selecting piece %d of %d (\"%s\", index %u, %s) from shown %d, delta %d",
            g_bdGridIdx + 1, n, name, targetIdx, unlocked ? "unlocked" : "locked", cur, delta);
    typedef void (*BnStepFn)(uintptr_t, int);
    BnStepFn fn = (BnStepFn)(base + BD_STEP_RVA);
    __try { fn(panel, delta); }
    __except (EXCEPTION_EXECUTE_HANDLER) { logLine("banner: the step function FAULTED"); postSpeech(axs(AXS_ACTION_FAILED)); return; }
    uint32_t after = bdWorking(section);
    if (after != targetIdx) {
        logLine("banner: step returned but working index is %u, not %u", after, targetIdx);
        postSpeech(axs(AXS_BD_SELECT_DIDNT));
        return;
    }
    char utter[MAILBOX_SZ];
    _snprintf(utter, sizeof utter, axs(unlocked ? AXS_BD_SELECTED_FMT : AXS_BD_PREVIEWING_FMT), name);
    utter[sizeof utter - 1] = 0;
    postSpeech(utter);
}

// ---- keys ----
bool bdRouteKey(uintptr_t base, uintptr_t panel, uint32_t sym, uint16_t mod, uint8_t repeat) {
    if (!bdIsPanel(base, panel)) return false;
    if (mod & (KMOD_LALT | KMOD_RALT)) return false;
    const bool ctrl = (mod & (KMOD_LCTRL | KMOD_RCTRL)) != 0;
    const bool enter = sym == SDLK_RETURN || sym == SDLK_KP_ENTER;
    if (ctrl && !enter) return false;                 // Ctrl+anything else stays the game's
    const bool nav = sym == SDLK_UP || sym == SDLK_DOWN || sym == SDLK_LEFT || sym == SDLK_RIGHT ||
                     sym == SDLK_HOME || sym == SDLK_END || enter || sym == SDLK_r || sym == SDLK_b;
    if (!nav) return false;                           // Escape and the rest stay the game's
    uintptr_t bld = bdBuilding(base);
    int section = bdSection(panel);
    int ns = bdSections(bld, nullptr);
    if (!bld || section < 0 || ns <= 0) {
        if (repeat) return true;
        logLine("banner: key with building=%p section=%d sections=%d", (void*)bld, section, ns);
        postSpeech(axs(AXS_BD_UNREADABLE));
        return true;
    }
    if (!bdBuilt(panel)) {                            // nothing to walk yet
        if (repeat) return true;
        g_bdLoadPending = true;
        postSpeech(axs(AXS_BD_LOADING));
        return true;
    }
    if (ctrl && enter) { if (!repeat) bdClickApply(base, panel); return true; }
    if (sym == SDLK_r) { if (!repeat) bdClickRandom(base, panel); return true; }
    if (sym == SDLK_b) {
        if (repeat) return true;
        char utter[MAILBOX_SZ];
        bdBannerText(base, bld, utter, sizeof utter);
        postSpeech(utter);
        return true;
    }
    uintptr_t shown[BD_MAX_PIECES];
    int n = bdShown(panel, shown, BD_MAX_PIECES);
    int perRow = bdPerRow(base);
    int tabs = bdTabCount(bld);

    // Home/End: first / last of the current pane.
    {
        int jump = 0;
        if (axDecodeJump(sym, 0, repeat, &jump)) {
            if (!jump) return true;                   // held jump: one landing per press
            if (g_bdPane == BD_PANE_GRID) { if (n > 0) axStepCursor(&g_bdGridIdx, n, jump); }
            else axStepCursor(&g_bdTabRow, tabs, jump);
            bdSpeakCursor(base, panel, nullptr);
            return true;
        }
    }
    if (sym == SDLK_UP || sym == SDLK_DOWN) {
        if (axNavHoldRepeat(repeat)) return true;     // throttled repeat: claimed, no step
        if (g_bdPane == BD_PANE_TABS) {
            axStepCursor(&g_bdTabRow, tabs, sym == SDLK_DOWN ? 1 : -1);   // hard stops re-read
        } else if (n > 0) {
            axStepCursor(&g_bdGridIdx, n, 0);
            int row = g_bdGridIdx / perRow, lastRow = (n - 1) / perRow;
            if (sym == SDLK_DOWN && row < lastRow) {
                g_bdGridIdx += perRow;
                if (g_bdGridIdx >= n) g_bdGridIdx = n - 1;   // a short last row: its last cell
            } else if (sym == SDLK_UP && row > 0) {
                g_bdGridIdx -= perRow;
            }                                          // else a hard stop: re-read
        }
        bdSpeakCursor(base, panel, nullptr);
        return true;
    }
    if (sym == SDLK_LEFT || sym == SDLK_RIGHT) {
        if (axNavHoldRepeat(repeat)) return true;
        if (g_bdPane == BD_PANE_TABS) {
            if (n <= 0) { postSpeech(axs(n == 0 ? AXS_BD_NO_PIECES : AXS_BD_UNREADABLE)); return true; }
            g_bdPane = BD_PANE_GRID;
            if (g_bdGridIdx < 0 || g_bdGridIdx >= n) {
                int w = bdShownIndexOfWorking(panel, bdSectionAt(bld, section));
                g_bdGridIdx = w >= 0 ? w : 0;
            }
            char pfx[64];
            _snprintf(pfx, sizeof pfx, "%s ", axs(AXS_BD_PANE_GRID)); pfx[sizeof pfx - 1] = 0;
            bdSpeakGrid(base, panel, pfx);
            return true;
        }
        if (n > 0) axStepCursor(&g_bdGridIdx, n, 0);
        int col = n > 0 ? g_bdGridIdx % perRow : 0;
        bool toTabs = n <= 0 ||
                      (sym == SDLK_LEFT && col == 0) ||
                      (sym == SDLK_RIGHT && (col == perRow - 1 || g_bdGridIdx + 1 >= n));
        if (toTabs) {
            g_bdPane = BD_PANE_TABS;
            g_bdTabRow = section;                     // the dev's rule: land on the current tab
            char pfx[64];
            _snprintf(pfx, sizeof pfx, "%s ", axs(AXS_BD_PANE_TABS)); pfx[sizeof pfx - 1] = 0;
            bdSpeakTab(base, panel, pfx);
            return true;
        }
        g_bdGridIdx += (sym == SDLK_RIGHT) ? 1 : -1;
        bdSpeakGrid(base, panel, nullptr);
        return true;
    }
    if (enter) {
        if (repeat) return true;
        if (g_bdPane == BD_PANE_GRID) { bdSelectPiece(base, panel); return true; }
        axStepCursor(&g_bdTabRow, tabs, 0);
        if (g_bdTabRow < ns)            bdClickSection(base, panel, g_bdTabRow);
        else if (g_bdTabRow == ns)      bdClickRandom(base, panel);
        else                            bdClickApply(base, panel);
        return true;
    }
    return false;
}

// ---- the watches ----
void bdService(uintptr_t base, uintptr_t panel) {
    if (!bdIsPanel(base, panel)) {
        if (g_bdSwitchUntil || g_bdRandomUntil || g_bdApplyUntil) {
            g_bdSwitchUntil = 0; g_bdSwitchTo = -1; g_bdRandomUntil = 0; g_bdApplyUntil = 0;
            logLine("banner: watches ended by the screen closing");
        }
        return;
    }
    uintptr_t bld = bdBuilding(base);
    int section = bdSection(panel);
    if (!bld || section < 0) return;
    DWORD now = GetTickCount();
    if (g_bdSwitchUntil) {
        if (section == g_bdSwitchTo) {
            g_bdSwitchUntil = 0; g_bdSwitchTo = -1;
            g_bdLastSection = section;
            logLine("banner: section switched to %d", section);
            g_bdPane = BD_PANE_TABS;
            g_bdTabRow = section;
            int w = bdShownIndexOfWorking(panel, bdSectionAt(bld, section));
            g_bdGridIdx = w >= 0 ? w : 0;
            bdSpeakTab(base, panel, nullptr);
        } else if (now > g_bdSwitchUntil) {
            g_bdSwitchUntil = 0; g_bdSwitchTo = -1;
            logLine("banner: switch watch timed out -- section still %d", section);
            logInputSnapshot(base, "banner-tab-timeout");
            diagDumpFocusElements(base, "banner-tab-timeout");
            postSpeech(axs(AXS_BD_SECTION_UNCHANGED));
        }
        return;
    }
    if (g_bdRandomUntil) {
        uintptr_t beg = 0;
        int ns = bdSections(bld, &beg);
        bool changed = false;
        for (int i = 0; i < ns && i < g_bdSnapCount; i++)
            if (bdWorking(beg + (uintptr_t)i * PB_BC_SECTION_STRIDE) != g_bdSnapWorking[i]) changed = true;
        if (changed || now > g_bdRandomUntil) {
            g_bdRandomUntil = 0;
            logLine("banner: random %s", changed ? "changed the banner" : "watch timed out with no change");
            char body[MAILBOX_SZ], utter[MAILBOX_SZ];
            bdBannerText(base, bld, body, sizeof body);
            _snprintf(utter, sizeof utter, "%s %s", axs(changed ? AXS_BD_RANDOMIZED : AXS_BD_RANDOM_UNCHANGED), body);
            utter[sizeof utter - 1] = 0;
            postSpeech(utter);
            int w = bdShownIndexOfWorking(panel, bdSectionAt(bld, section));
            if (w >= 0) g_bdGridIdx = w;
        }
        return;
    }
    if (g_bdApplyUntil) {
        bool changed = false, lockedPrev = false;
        bdChangeState(bld, &changed, &lockedPrev);
        if (confirmDialogOpen(base)) {
            g_bdApplyUntil = 0;                       // the game's notice: the dialog reader's
            logLine("banner: apply raised a dialog (expected=%d)", g_bdApplyExpectDialog ? 1 : 0);
        } else if (!changed && !g_bdApplyExpectDialog) {
            g_bdApplyUntil = 0;
            logLine("banner: applied");
            char body[MAILBOX_SZ], utter[MAILBOX_SZ];
            bdBannerText(base, bld, body, sizeof body);
            _snprintf(utter, sizeof utter, "%s %s", axs(AXS_BD_APPLIED), body); utter[sizeof utter - 1] = 0;
            postSpeech(utter);
        } else if (now > g_bdApplyUntil) {
            g_bdApplyUntil = 0;
            logLine("banner: apply watch timed out (changed=%d lockedPreview=%d)", changed ? 1 : 0, lockedPrev ? 1 : 0);
            logInputSnapshot(base, "banner-apply-timeout");
            diagDumpFocusElements(base, "banner-apply-timeout");
            postSpeech(axs(AXS_BD_APPLY_DIDNT));
        }
        return;
    }
    if (g_bdLoadPending && bdBuilt(panel)) {
        g_bdLoadPending = false;
        logLine("banner: grid built after arrival (section %d)", section);
        g_bdPane = BD_PANE_TABS; g_bdTabRow = section; g_bdLastSection = section;
        int w = bdShownIndexOfWorking(panel, bdSectionAt(bld, section));
        g_bdGridIdx = w >= 0 ? w : 0;
        char pfx[96];
        _snprintf(pfx, sizeof pfx, "%s ", axs(AXS_BD_LOADED)); pfx[sizeof pfx - 1] = 0;
        bdSpeakTab(base, panel, pfx);
        return;
    }
    if (g_bdLastSection >= 0 && section != g_bdLastSection) {
        logLine("banner: section changed under the cursor (%d -> %d)", g_bdLastSection, section);
        g_bdLastSection = section;
        int w = bdShownIndexOfWorking(panel, bdSectionAt(bld, section));
        g_bdGridIdx = w >= 0 ? w : 0;
        if (g_bdPane == BD_PANE_TABS) g_bdTabRow = section;
    }
}

// ---- the probe ----
void bdProbe(uintptr_t base, uintptr_t panel) {
    if (!panel) { logDump("banner probe: no panel"); return; }
    uintptr_t vft = 0;
    safeReadPtr(panel, &vft);
    logDump("banner probe: panel=%p vft=%p (expect base+0x%llx = %p) match=%d",
            (void*)panel, (void*)vft, (unsigned long long)BD_VFT_RVA, (void*)(base + BD_VFT_RVA), bdIsPanel(base, panel) ? 1 : 0);
    if (!bdIsPanel(base, panel)) { logDump("banner probe: NOT the banner designer panel -> nothing else is trustworthy here"); return; }
    uintptr_t bldPanel = 0, bldMap = bdBuilding(base);
    safeReadPtr(panel + BD_P_BLD_OFF, &bldPanel);
    logDump("banner probe: building via panel+0x260=%p, via Campaign map=%p %s", (void*)bldPanel, (void*)bldMap,
            bldPanel == bldMap ? "(agree)" : "<-- DISAGREE: +0x260 is not the building, or the map walk is off");
    uint32_t section = 0xffffffff, page = 0xffffffff;
    uint8_t built = 0xff, tabFocus = 0xff, tips = 0xff, inputMode = 0xff;
    safeReadU32(panel + BD_P_SECTION_OFF, &section); safeReadU32(panel + BD_P_PAGE_OFF, &page);
    safeReadU8(panel + BD_P_BUILT_OFF, &built); safeReadU8(panel + BD_P_TABFOCUS_OFF, &tabFocus);
    safeReadU8(panel + BD_P_TIPS_OFF, &tips); safeReadU8(panel + BD_P_INPUTMODE_OFF, &inputMode);
    logDump("banner probe: section(+0xa0)=%u page(+0x718)=%u built(+0x71c)=%u tabFocus(+0x71d)=%u tips(+0x71e)=%u inputMode(+0x71f)=%u pieces_per_row(global)=%d",
            section, page, built, tabFocus, tips, inputMode, bdPerRow(base));
    if (bldMap) {
        uint8_t c0 = 0xff, c1 = 0xff, c2 = 0xff, c3 = 0xff;
        safeReadU8(bldMap + 0xc0, &c0); safeReadU8(bldMap + 0xc1, &c1); safeReadU8(bldMap + 0xc2, &c2); safeReadU8(bldMap + 0xc3, &c3);
        uintptr_t beg = 0;
        int ns = bdSections(bldMap, &beg);
        logDump("banner probe: flags(+0xc0..+0xc3)=%u,%u,%u,%u good=%d sections(+0xa8/+0xb0)=%d begin=%p",
                c0, c1, c2, c3, bdFlagsGood(bldMap) ? 1 : 0, ns, (void*)beg);
        for (int i = 0; i < ns; i++) {
            uintptr_t s = beg + (uintptr_t)i * PB_BC_SECTION_STRIDE;
            char sname[32], wname[192] = { 0 }, aname[192] = { 0 };
            bdSectionName(s, sname, sizeof sname);
            uint32_t w = bdWorking(s), a = bdApplied(s), o = 0xffffffff;
            safeReadU32(s + 0x28, &o);
            int np = bdPieces(s, nullptr);
            bdPieceName(base, bdPieceByIndex(s, w), wname, sizeof wname);
            bdPieceName(base, bdPieceByIndex(s, a), aname, sizeof aname);
            logDump("banner probe:   section %d \"%s\" working(+0x20)=%u \"%s\" applied(+0x24)=%u \"%s\" +0x28=%u pieces=%d",
                    i, sname, w, wname, a, aname, o, np);
        }
    }
    uintptr_t shown[BD_MAX_PIECES];
    int n = bdShown(panel, shown, BD_MAX_PIECES);
    logDump("banner probe: shown vector(+0x268/+0x270) -> %d pieces", n);
    uintptr_t curSection = bldMap ? bdSectionAt(bldMap, (int)section) : 0;
    for (int i = 0; i < n && i < 30; i++) {
        uint32_t idx = 0xffffffff; uint8_t unlocked = 0xff, seen = 0xff; char name[0x21] = { 0 }, rar[0x21] = { 0 }, text[MAILBOX_SZ] = { 0 };
        safeReadU32(shown[i] + BD_PC_INDEX_OFF, &idx); safeReadU8(shown[i] + PB_BC_PIECE_UNLOCKED, &unlocked);
        safeReadU8(shown[i] + BD_PC_SEEN_OFF, &seen); safeReadCStr(shown[i] + PB_BC_PIECE_NAME, name, sizeof name);
        safeReadCStr(shown[i] + BD_PC_RARITY_OFF, rar, sizeof rar);
        if (curSection) bdPieceText(base, curSection, shown[i], i, n, text, sizeof text);
        logDump("banner probe:   shown %d piece=%p index(+0x5c)=%u unlocked=%u seen(+0xb4)=%u name=\"%s\" rarity=\"%s\" -> \"%s\"",
                i, (void*)shown[i], idx, unlocked, seen, name, rar, text);
    }
    uintptr_t els[BD_MAX_SECTIONS + 8]; float xs[BD_MAX_SECTIONS + 8];
    int ne = feCollectFamilyByX(base, BD_ELEM_ID, 0, els, xs, BD_MAX_SECTIONS + 8);
    logDump("banner probe: 'isb' (0x%x) family elements on screen: %d (expect sections + 2)", BD_ELEM_ID, ne);
    for (int i = 0; i < ne; i++) { float cx = 0, cy = 0; elemCenter(els[i], &cx, &cy); logDump("banner probe:   element %d centre=(%.0f,%.0f)", i, cx, cy); }
    feDumpFocusVector("banner designer open");
}
