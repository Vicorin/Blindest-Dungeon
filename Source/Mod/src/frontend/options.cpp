// frontend/options.cpp -- the third frontend slice

#include <cstdint>
#include <cstring>
#include <cstdio>
#include "internal.h"

// ---- Options menu (immediate-mode; UI::Panel::Menu) ----
static const uintptr_t MENU_CURMATCH_OFF = 0x600;     // int: current category match value
static const uintptr_t MENU_CURCAT_OFF   = 0x604;     // int: current category index
static const uintptr_t MENU_SELIDX_OFF   = 0x3d4;     // uint: selected visible-row index (was 0x3e4)
static const uintptr_t MENU_PENDING_OFF  = 0x3d0;
static const uintptr_t OPT_DESC_STRIDE   = 0x64;      // per option
static const uintptr_t OPT_CAT_STRIDE    = 0x4c;      // per category
static const uintptr_t OPT_CAT_MATCH_OFF = 0x40;      // int: category match value (== menu+0x600)
static const uintptr_t OPT_DESC_NAME_OFF = 0x00;      // inline C-string
static const uintptr_t OPT_DESC_GROUP_OFF= 0x44;      // int: category-group index
static const uintptr_t OPT_DESC_TYPE_OFF = 0x54;      // int: value display type (0=enum,1/2/3=numeric)
static const uintptr_t OPT_VAL_SECTION   = 0x30;      // value entries start at (descIdx+0x30)
static const uintptr_t OPT_VAL_ENTRY_STR = 0x60;      // per-entry stride; entry = {int* begin; int* end}
static const uintptr_t OPT_LANG_STRIDE   = 0x200;     // per language entry
static const uintptr_t OPT_LANG_NAME_OFF = 0x000;     // internal name within a language entry
static const uintptr_t OPT_LANG_DISP_OFF = 0x100;     // display name within a language entry
static const uint32_t  OPT_CAT_ID_BASE   = 0x6f6367;  // category tab id = base + catIdx ("?co")
static const uintptr_t OPT_DESC_PLAIN_OFF= 0x4c;      // int: 0 = plain checkbox row. This is BOTH the
typedef void (*OptToggleCbFn)(void* capture);

static const long OPT_ADJUST_WINDOW_MS = 400;

char                 g_optValueOnly[128]= { 0 };

static int           g_optAdjustRow     = -1;
static int64_t       g_optAdjustAnchor  = 0;     // the row id we re-read the value from
static DWORD         g_optAdjustUntil   = 0;     // give up watching at this tick
static DWORD         g_optAdjustNextAt  = 0;
static char          g_optAdjustBefore[128] = { 0 }; // the value as it read BEFORE the click

// ---- Options screen (front-end, reachable from title or in-game pause menu) ----
bool isOptionId(uint32_t cc) {
    uint32_t hi = cc & 0xFFFFFF00u;
    return hi == 0x006f6300u || hi == 0x006f7000u || hi == 0x6f706c00u;
}

volatile void* g_optMenu = nullptr;

// Does this look like real text rather than a stray pointer / binary field?
static bool looksPrintable(const char* s) {
    if (!s[0]) return false;
    for (int i = 0; s[i] && i < 8; i++)
        if ((unsigned char)s[i] < 0x20 || (unsigned char)s[i] > 0x7e) return false;
    return true;
}

static bool readNameField(uintptr_t entry, uintptr_t off, char* out, int outsz) {
    if (safeReadCStr(entry + off, out, outsz) && looksPrintable(out) && out[1]) return true;
    uintptr_t p = 0;
    if (safeReadPtr(entry + off, &p) && p > 0x10000 &&
        safeReadCStr(p, out, outsz) && looksPrintable(out) && out[1]) return true;
    return false;
}

static void dumpOptionTables(uintptr_t base) {
    logLine("optdump: category table @0x%llx stride 0x%x",
            (unsigned long long)(base + OPT_CAT_ARR_RVA), (unsigned)OPT_CAT_STRIDE);
    for (int c = 0; c < 8; c++) {
        uintptr_t e = base + OPT_CAT_ARR_RVA + (uintptr_t)c * OPT_CAT_STRIDE;
        for (uintptr_t off = 0; off <= 0x48; off += 4) {
            char s[64];
            if (readNameField(e, off, s, sizeof s) && s[2])
                logLine("  optcat[%d]+0x%llx = \"%s\"", c, (unsigned long long)off, s);
        }
    }
    logLine("optdump: descriptor array @0x%llx stride 0x%x",
            (unsigned long long)(base + OPT_DESC_ARR_RVA), (unsigned)OPT_DESC_STRIDE);
    for (int d = 0; d < OPT_DESC_COUNT; d++) {
        uintptr_t e = base + OPT_DESC_ARR_RVA + (uintptr_t)d * OPT_DESC_STRIDE;
        char best[64] = { 0 };
        for (uintptr_t off = 0; off <= 0x5c; off += 4) {
            char s[64];
            if (readNameField(e, off, s, sizeof s) && s[2])
                logLine("  optdesc[%d]+0x%llx = \"%s\"", d, (unsigned long long)off, s);
        }
        uint32_t grp = 0, typ = 0;
        safeReadU32(e + 0x44, &grp); safeReadU32(e + 0x54, &typ);
        logLine("  optdesc[%d] i44=%u i54=%u", d, grp, typ);
    }
}

static const uintptr_t OPT_VAL_STR_SECTION = 0x1218;  // byte offset of the value STRING section
static bool readOptionValueStr(uintptr_t base, int descIdx, char* out, int outsz) {
    out[0] = 0;
    if (descIdx < 0) return false;
    uintptr_t arr = 0;
    if (!safeReadPtr(base + OPT_VALARR_PTR_RVA, &arr) || arr <= 0x10000) return false;
    uintptr_t s = arr + (uintptr_t)descIdx * OPT_VAL_ENTRY_STR + OPT_VAL_STR_SECTION;
    if (!safeReadCStr(s, out, outsz) || !looksPrintable(out)) { out[0] = 0; return false; }
    return true;
}

static const uintptr_t OPT_VAL_STR_SAVED = 0x18;   // byte offset of the SAVED string section
bool optSavedLanguage(uintptr_t base, char* out, int outsz) {
    out[0] = 0;
    static int langDesc = -1;
    if (langDesc < 0) {
        for (int d = 0; d < OPT_DESC_COUNT; d++) {
            char name[64] = { 0 };
            if (safeReadCStr(base + OPT_DESC_ARR_RVA + (uintptr_t)d * OPT_DESC_STRIDE
                             + OPT_DESC_NAME_OFF, name, sizeof name) &&
                strcmp(name, "language") == 0) { langDesc = d; break; }
        }
        if (langDesc < 0) return false;
    }
    uintptr_t arr = 0;
    if (!safeReadPtr(base + OPT_VALARR_PTR_RVA, &arr) || arr <= 0x10000) return false;
    if (safeReadCStr(arr + (uintptr_t)langDesc * OPT_VAL_ENTRY_STR + OPT_VAL_STR_SAVED,
                     out, outsz) && looksPrintable(out)) return true;
    return readOptionValueStr(base, langDesc, out, outsz);
}

int readOptionValue(uintptr_t base, int descIdx, int32_t* out, int maxN) {
    uintptr_t arr = 0;
    if (!safeReadPtr(base + OPT_VALARR_PTR_RVA, &arr) || arr <= 0x10000) return 0;
    uintptr_t vec = arr + (uintptr_t)(descIdx + (int)OPT_VAL_SECTION) * OPT_VAL_ENTRY_STR;
    uintptr_t begin = 0, end = 0;
    if (!safeReadPtr(vec, &begin) || !safeReadPtr(vec + 8, &end)) return 0;
    if (begin <= 0x10000 || end < begin || (end - begin) > 0x1000) return 0;
    long cnt = (long)((end - begin) / 4);
    int n = 0;
    for (long i = 0; i < cnt && n < maxN; i++) {
        uint32_t v = 0;
        if (safeReadU32(begin + (uintptr_t)i * 4, &v)) out[n++] = (int32_t)v;
    }
    return n;
}

int optRowPos(uint32_t cc) {
    if ((cc & 0xFFFFFF00u) == 0x006f7000u) return (int)(cc - 0x6f7074u);
    uint32_t low = cc & 0xffu;                     // "?lpo" family
    return (low >= 0x75u) ? (int)(cc - 0x6f706c75u) : (int)(cc - 0x6f706c64u);
}

typedef uint8_t (*OptRowVisFn)(uintptr_t menu, uint32_t descIdx);
static bool optRowVisible(uintptr_t base, uintptr_t menu, int descIdx) {
    OptRowVisFn fn = (OptRowVisFn)(base + OPT_ROWVIS_FN_RVA);
    __try {
        return fn(menu, (uint32_t)descIdx) != 0;
    } __except (sehReport("optRowVisible", GetExceptionInformation())) {
        return false;
    }
}

static int optDescForPos(uintptr_t base, int pos) {
    if (pos < 0) return -1;
    uintptr_t menu = (uintptr_t)g_optMenu, mvt = 0;
    if (!menu || !safeReadPtr(menu, &mvt) || mvt != base + MENU_VFTABLE_RVA) return -1;
    int seen = 0;
    for (int d = 0; d < OPT_DESC_COUNT; d++) {
        if (!optRowVisible(base, menu, d)) continue;
        if (seen == pos) return d;
        seen++;
    }
    return -1;
}

bool resolveOptions(uintptr_t base, uintptr_t /*tbwVtbl*/, int64_t id,
                           char* text, int textsz) {
    uint32_t cc = (uint32_t)(uint64_t)id;
    if (!isOptionId(cc)) return false;
    char idasc[9]; idToAscii(id, idasc);

    static bool dumped = false;
    if (!dumped) { dumped = true; dumpOptionTables(base); }

    // Validate the captured menu object (its vtable must be the Menu vftable).
    uintptr_t menu = (uintptr_t)g_optMenu, mvt = 0;
    bool menuOk = menu && safeReadPtr(menu, &mvt) && mvt == base + MENU_VFTABLE_RVA;
    uint32_t curMatch = 0, curCat = 0, selIdx = 0;
    if (menuOk) {
        safeReadU32(menu + MENU_CURMATCH_OFF, &curMatch);
        safeReadU32(menu + MENU_CURCAT_OFF,   &curCat);
        safeReadU32(menu + MENU_SELIDX_OFF,   &selIdx);
    }

    text[0] = 0;
    uint32_t hi = cc & 0xFFFFFF00u;

    if (hi == 0x006f6300u) {
        int catIdx = (int)(cc - OPT_CAT_ID_BASE);
        char name[64] = { 0 };
        bool gotName = (catIdx >= 0 && catIdx < 8) &&
                       readNameField(base + OPT_CAT_ARR_RVA + (uintptr_t)catIdx * OPT_CAT_STRIDE,
                                     0, name, sizeof name);
        char label[256] = { 0 };
        bool resolved = false;
        if (gotName) {
            char key[96]; _snprintf(key, sizeof key, "options_category_%s", name); key[95] = 0;
            resolved = resolveKey(base, key, label, sizeof label);
        }
        if (resolved) _snprintf(text, textsz, "%s", label);
        else          _snprintf(text, textsz, axs(AXS_OPT_FALLBACK_CATEGORY_N), catIdx + 1);
        text[textsz - 1] = 0;
        logLine("optcat id=0x%llx ascii=\"%s\" catIdx=%d name=\"%s\" text=\"%s\" (%s)",
                (unsigned long long)id, idasc, catIdx, name, text,
                resolved ? "resolved" : "fallback");
        return true;
    }

    int pos = optRowPos(cc);
    int chosen = optDescForPos(base, pos);

    char name[64] = { 0 }; uint32_t vtype = 0;
    if (chosen >= 0) {
        uintptr_t e = base + OPT_DESC_ARR_RVA + (uintptr_t)chosen * OPT_DESC_STRIDE;
        safeReadCStr(e + OPT_DESC_NAME_OFF, name, sizeof name);
        safeReadU32(e + OPT_DESC_TYPE_OFF, &vtype);
    }
    char label[256] = { 0 }; bool resolved = false;
    if (name[0]) {
        char key[96]; _snprintf(key, sizeof key, "menu_options_element_%s", name); key[95] = 0;
        resolved = resolveKey(base, key, label, sizeof label);
    }
    if (resolved) { strncpy(text, label, textsz - 1); text[textsz - 1] = 0; }
    else          { strncpy(text, axs(AXS_OPT_FALLBACK_ROW), textsz - 1); text[textsz - 1] = 0; }

    int32_t vals[4];
    int nv = (chosen >= 0) ? readOptionValue(base, chosen, vals, 4) : 0;
    char valstr[128] = { 0 };
    if (resolved && (nv > 0 || vtype == 0)) {
        if (vtype == 0) {                            // enum: the value is a STRING, not an int
            if (strcmp(name, "language") != 0) {
                char ev[64];
                if (readOptionValueStr(base, chosen, ev, sizeof ev)) {
                    char key[96] = { 0 };
                    if (strcmp(name, "subtitles") == 0)
                        _snprintf(key, sizeof key, "options_value_subtitles_%s", ev);
                    else if (strcmp(name, "town_events") == 0 || strcmp(name, "never_again") == 0)
                        _snprintf(key, sizeof key, "options_value_%s", ev);
                    if (key[0]) {
                        key[sizeof key - 1] = 0;
                        if (!resolveKey(base, key, valstr, sizeof valstr) || !valstr[0]) {
                            strncpy(valstr, ev, sizeof valstr - 1);
                            valstr[sizeof valstr - 1] = 0;
                        }
                    }
                }
            } else if (strcmp(name, "language") == 0) {
                char curName[128] = { 0 };
                if (readOptionValueStr(base, chosen, curName, sizeof curName) && curName[0]) {
                    uintptr_t lb = 0, le = 0;
                    bool matched = false;
                    if (safeReadPtr(base + OPT_LANG_BEGIN_RVA, &lb) &&
                        safeReadPtr(base + OPT_LANG_END_RVA,   &le) && lb > 0x10000 && le > lb) {
                        long lc = (long)((le - lb) / OPT_LANG_STRIDE);
                        for (long i = 0; i < lc && i < 64; i++) {
                            char entName[128] = { 0 };
                            if (safeReadCStr(lb + (uintptr_t)i * OPT_LANG_STRIDE + OPT_LANG_NAME_OFF,
                                             entName, sizeof entName) &&
                                _stricmp(entName, curName) == 0) {
                                safeReadCStr(lb + (uintptr_t)i * OPT_LANG_STRIDE + OPT_LANG_DISP_OFF,
                                             valstr, sizeof valstr);
                                matched = true;
                                break;
                            }
                        }
                    }
                    if (!matched || !valstr[0]) {   // fall back to the internal name (e.g. "english")
                        strncpy(valstr, curName, sizeof valstr - 1);
                        valstr[sizeof valstr - 1] = 0;
                    }
                }
                static char lastLang[128] = { 0 };
                if (strcmp(valstr, lastLang) != 0) {
                    strncpy(lastLang, valstr, sizeof lastLang - 1); lastLang[sizeof lastLang - 1] = 0;
                    logLine("optlang cur=\"%s\" -> \"%s\"", curName, valstr);
                }
            }
        } else if (nv == 1 && vtype == 1 && (vals[0] == 0 || vals[0] == 1)) {
            strncpy(valstr, axs(vals[0] ? AXS_VALUE_ON : AXS_VALUE_OFF), sizeof valstr - 1);
        } else {
            for (int i = 0; i < nv; i++) {
                int shown = vals[i] + (vtype == 2 ? 1 : 0);
                char num[16]; _snprintf(num, sizeof num, "%d", shown);
                int cur = (int)strlen(valstr);
                _snprintf(valstr + cur, sizeof valstr - cur, "%s%s", cur ? " " : "", num);
            }
        }
        if (valstr[0]) {
            int cur = (int)strlen(text);
            _snprintf(text + cur, textsz - cur, ", %s", valstr);
            text[textsz - 1] = 0;
        }
    }
    strncpy(g_optValueOnly, valstr, sizeof g_optValueOnly - 1);
    g_optValueOnly[sizeof g_optValueOnly - 1] = 0;

    static char lastLogged[512] = { 0 };
    if (strcmp(text, lastLogged) != 0) {
        strncpy(lastLogged, text, sizeof lastLogged - 1);
        lastLogged[sizeof lastLogged - 1] = 0;
        char evLog[64] = { 0 };
        if (vtype == 0) readOptionValueStr(base, chosen, evLog, sizeof evLog);
        logLine("optrow id=0x%llx ascii=\"%s\" pos=%d chosen=%d name=\"%s\" vtype=%u nv=%d "
                "v0=%d v1=%d enum=\"%s\" curCat=%u text=\"%s\" (%s)",
                (unsigned long long)id, idasc, pos, chosen, name, vtype, nv,
                nv > 0 ? vals[0] : -999, nv > 1 ? vals[1] : -999, evLog, curCat, text,
                resolved ? "resolved" : name[0] ? "unresolved" : "nomap");
    }
    return true;
}

// ---- Options category page: ONE VERTICAL LIST ----
static const uint32_t OPT_ROW_ID_BASE = 0x6f7074u;    // centre element  — plain row
static const uint32_t OPT_DEC_ID_BASE = 0x6f706c64u;  // 'd'ecrease      — adjustable row
static const uint32_t OPT_INC_ID_BASE = 0x6f706c75u;  // 'u'p / increase — adjustable row

static bool optIsListId(uint32_t cc)     { return (cc & 0xFFFFFF00u) == 0x6f706c00u; }
static bool optIsIncreaseId(uint32_t cc) { return optIsListId(cc) && (cc & 0xffu) >= 0x75u; }
static bool optIsCategoryId(uint32_t cc) { return (cc & 0xFFFFFF00u) == 0x006f6300u; }

int64_t optRowAnchorId(int pos) {
    if (pos < 0 || pos > 255) return 0;
    int64_t centre = (int64_t)(OPT_ROW_ID_BASE + (uint32_t)pos);
    if (feElemOnScreen(centre)) return centre;
    int64_t dec = (int64_t)(OPT_DEC_ID_BASE + (uint32_t)pos);
    if (feElemOnScreen(dec)) return dec;
    return 0;
}

bool optIsAnchorId(uint32_t cc) { return isOptionId(cc) && !optIsIncreaseId(cc); }

bool optPageActive() { return optPageId() != 0; }

uint32_t optPageId() {
    uintptr_t menu = (uintptr_t)g_optMenu, mvt = 0;
    if (!menu || !safeReadPtr(menu, &mvt) || mvt != g_base + MENU_VFTABLE_RVA) return 0;
    uint32_t curMatch = 0;
    if (!safeReadU32(menu + MENU_CURMATCH_OFF, &curMatch)) return 0;
    return curMatch;
}

// ---- The page walk itself: BY ROW NUMBER, NOT BY GEOMETRY ----
bool optListStep(int dir) {
    if (dir != 0 && dir != 1) return false;           // vertical only; Left/Right adjust the value
    if (!optPageActive()) return false;               // the category LIST stays geometric

    int64_t cur = feCurrentFocusId();
    uint32_t cc = (uint32_t)(uint64_t)cur;
    int pos = -1;
    if (!isNoFocus(cur) && isOptionId(cc) && !optIsCategoryId(cc)) {
        int p = optRowPos(cc);
        if (p >= 0 && p <= 255 && optRowAnchorId(p) != 0) pos = p;
    }

    const int step = (dir == 1) ? 1 : -1;
    int target = -1;
    if (pos < 0) {
        for (int p = 0; p < 4 && target < 0; p++)
            if (optRowAnchorId(p) != 0) target = p;
        if (target < 0) return false;                 // no rows drawn -> caller's walk
    } else {
        for (int k = 1; k <= 4 && target < 0; k++) {
            int p = pos + step * k;
            if (p < 0) break;
            if (optRowAnchorId(p) != 0) target = p;
        }
        if (target < 0) {
            // The end of the list: re-read the row we are standing on, never silence.
            int64_t anchor = optRowAnchorId(pos);
            char text[512];
            resolveLabel(g_base, g_tbwVtbl, anchor, text, sizeof text);
            if (text[0]) {
                postSpeech(text);
                g_axSpokenId = anchor;
                strncpy(g_axLastSpoken, text, sizeof g_axLastSpoken - 1);
                g_axLastSpoken[sizeof g_axLastSpoken - 1] = 0;
            }
            logLine("optlist: pos=%d is the %s row -- hard stop, re-read", pos,
                    step > 0 ? "last" : "first");
            return true;
        }
    }

    int64_t anchor = optRowAnchorId(target);
    uintptr_t elem = feGetElementById(anchor);
    float tx = 0, ty = 0;
    if (!elem || !elemCenter(elem, &tx, &ty)) {
        logLine("optlist: anchor 0x%llx for pos %d has no centre",
                (unsigned long long)anchor, target);
        return false;
    }
    moveCursorTo(tx, ty);                              // the game's own hover does the focusing
    g_feFocusId = anchor;
    char text[512];
    resolveLabel(g_base, g_tbwVtbl, anchor, text, sizeof text);
    if (text[0]) {                                     // a move is explicit -> always speak
        postSpeech(text);
        g_axSpokenId = anchor;
        strncpy(g_axLastSpoken, text, sizeof g_axLastSpoken - 1);
        g_axLastSpoken[sizeof g_axLastSpoken - 1] = 0;
    }
    logLine("optlist: dir=%d pos=%d -> %d anchor=0x%llx cursor=(%.0f,%.0f)",
            dir, pos, target, (unsigned long long)anchor, tx, ty);
    return true;
}

// ---- the focused row's tooltip, as a Ctrl+Up/Down buffer ----
static int      g_optTipLine = 0;   // buffer cursor; 0 = the row itself
static int      g_optTipPos  = -1;  // the row position the cursor belongs to...
static uint32_t g_optTipPage = 0;
                                    //    page is another row, so the pair is the identity)

bool optTipStep(bool forward) {
    uint32_t page = optPageId();
    if (page == 0) return false;                     // the category list has no rows
    int64_t cur = feCurrentFocusId();
    uint32_t cc = (uint32_t)(uint64_t)cur;
    if (isNoFocus(cur) || !isOptionId(cc) || optIsCategoryId(cc)) return false;
    int pos = optRowPos(cc);
    if (pos < 0 || pos > 255) return false;
    int64_t anchor = optRowAnchorId(pos);
    if (!anchor) return false;                       // row not drawn this frame -> not ours

    if (pos != g_optTipPos || page != g_optTipPage) {
        g_optTipPos = pos; g_optTipPage = page;      // a new row: the buffer starts at line 0
        g_optTipLine = 0;
    }

    char row[512];
    resolveLabel(g_base, g_tbwVtbl, anchor, row, sizeof row);

    char tip[1024]; tip[0] = 0;
    int chosen = optDescForPos(g_base, pos);
    char name[64] = { 0 };
    if (chosen >= 0)
        safeReadCStr(g_base + OPT_DESC_ARR_RVA + (uintptr_t)chosen * OPT_DESC_STRIDE
                     + OPT_DESC_NAME_OFF, name, sizeof name);
    if (name[0]) {
        char key[96];
        _snprintf(key, sizeof key, "menu_options_element_tooltip_%s", name); key[95] = 0;
        if (resolveKey(g_base, key, tip, sizeof tip)) abStripMarkup(tip);
    }

    int total = tip[0] ? 2 : 1;
    if (total == 1) {
        char line[640];
        _snprintf(line, sizeof line, "%s %s", row, axs(AXS_OPT_NO_TOOLTIP));
        line[sizeof line - 1] = 0;
        postSpeech(line);
        g_optTipLine = 0;
        logLine("opttip: pos=%d desc=%d name=\"%s\" has no tooltip", pos, chosen, name);
        return true;
    }
    g_optTipLine = (g_optTipLine + (forward ? 1 : -1) + total) % total;
    postSpeech(g_optTipLine == 0 ? row : tip);
    logLine("opttip: pos=%d desc=%d name=\"%s\" line=%d/%d", pos, chosen, name,
            g_optTipLine, total);
    return true;
}

static void optArmWatch(int pos, int64_t anchor, const char* beforeVal) {
    g_optAdjustRow    = pos;
    g_optAdjustAnchor = anchor;
    g_optAdjustUntil  = GetTickCount() + (DWORD)OPT_ADJUST_WINDOW_MS;
    g_optAdjustNextAt = GetTickCount();
    strncpy(g_optAdjustBefore, beforeVal, sizeof g_optAdjustBefore - 1);
    g_optAdjustBefore[sizeof g_optAdjustBefore - 1] = 0;
}

bool optAdjustRow(bool increase) {
    int64_t cur = feCurrentFocusId();
    uint32_t cc = (uint32_t)(uint64_t)cur;
    if (isNoFocus(cur) || !isOptionId(cc) || optIsCategoryId(cc)) {
        logLine("optadjust: nothing adjustable focused (id=0x%llx)", (unsigned long long)cur);
        return false;
    }
    int pos = optRowPos(cc);
    int64_t want = (int64_t)((increase ? OPT_INC_ID_BASE : OPT_DEC_ID_BASE) + (uint32_t)pos);
    if (!feElemOnScreen(want)) {
        char text[512];
        resolveLabel(g_base, g_tbwVtbl, cur, text, sizeof text);
        if (text[0]) postSpeech(text);
        logLine("optadjust: row pos=%d has no %s button", pos,
                increase ? "increase" : "decrease");
        return false;
    }
    int64_t anchor = optRowAnchorId(pos);
    if (!anchor) anchor = cur;

    char before[512];
    resolveLabel(g_base, g_tbwVtbl, anchor, before, sizeof before);   // refreshes g_optValueOnly
    char beforeVal[128];
    strncpy(beforeVal, g_optValueOnly[0] ? g_optValueOnly : before, sizeof beforeVal - 1);
    beforeVal[sizeof beforeVal - 1] = 0;

    if (!frontEndClickElementId(want)) return false;

    g_feFocusId = anchor;

    optArmWatch(pos, anchor, beforeVal);

    uintptr_t menu = (uintptr_t)g_optMenu;
    uint8_t p0 = 0, p1 = 0;
    safeReadU8(menu + MENU_PENDING_OFF, &p0); safeReadU8(menu + MENU_PENDING_OFF + 1, &p1);
    logLine("optadjust: row pos=%d %s -> clicked 0x%llx (anchor 0x%llx) before=\"%s\" pending=%u/%u",
            pos, increase ? "increase" : "decrease",
            (unsigned long long)want, (unsigned long long)anchor, beforeVal, p0, p1);
    return true;
}

// ---- Enter on a plain CHECKBOX row: CALL the game's own activation ----
bool optRowToggle() {
    if (!optPageActive()) return false;
    int64_t cur = feCurrentFocusId();
    uint32_t cc = (uint32_t)(uint64_t)cur;
    if (isNoFocus(cur) || !isOptionId(cc) || optIsCategoryId(cc)) return false;
    int pos = optRowPos(cc);
    if (pos < 0 || pos > 255) return false;
    int64_t centre = (int64_t)(OPT_ROW_ID_BASE + (uint32_t)pos);
    if (!feElemOnScreen(centre)) return false;                       // not a plain row
    if (feElemOnScreen((int64_t)(OPT_DEC_ID_BASE + (uint32_t)pos))) return false; // adjustable

    int chosen = optDescForPos(g_base, pos);
    if (chosen < 0) { logLine("opttoggle: pos=%d has no descriptor", pos); return false; }
    uintptr_t e = g_base + OPT_DESC_ARR_RVA + (uintptr_t)chosen * OPT_DESC_STRIDE;
    uint32_t vtype = 0, plain = 1;
    safeReadU32(e + OPT_DESC_TYPE_OFF, &vtype);
    safeReadU32(e + OPT_DESC_PLAIN_OFF, &plain);
    int32_t vals[2];
    int nv = readOptionValue(g_base, chosen, vals, 2);
    if (plain != 0 || vtype != 1 || nv != 1 || (vals[0] != 0 && vals[0] != 1)) {
        logLine("opttoggle: pos=%d desc=%d not a checkbox (plain=%u vtype=%u nv=%d v0=%d)",
                pos, chosen, plain, vtype, nv, nv > 0 ? vals[0] : -999);
        return false;
    }
    uintptr_t menu = (uintptr_t)g_optMenu, mvt = 0, arr = 0;
    if (!menu || !safeReadPtr(menu, &mvt) || mvt != g_base + MENU_VFTABLE_RVA) return false;
    if (!safeReadPtr(g_base + OPT_VALARR_PTR_RVA, &arr) || arr <= 0x10000) return false;

    char before[512];
    resolveLabel(g_base, g_tbwVtbl, cur, before, sizeof before);     // refreshes g_optValueOnly
    char beforeVal[128];
    strncpy(beforeVal, g_optValueOnly[0] ? g_optValueOnly : before, sizeof beforeVal - 1);
    beforeVal[sizeof beforeVal - 1] = 0;

    struct { void* vft; uintptr_t menu; uintptr_t arr; int32_t idx; int32_t pad; } cap =
        { nullptr, menu, arr, chosen, 0 };
    OptToggleCbFn fn = (OptToggleCbFn)(g_base + OPT_TOGGLE_CB_RVA);
    __try {
        fn(&cap);
    } __except (sehReport("optRowToggle", GetExceptionInformation())) {
        return true;
    }

    optArmWatch(pos, cur, beforeVal);

    uint8_t p0 = 0, p1 = 0;                          // the game's own pending-changes pair
    safeReadU8(menu + MENU_PENDING_OFF, &p0); safeReadU8(menu + MENU_PENDING_OFF + 1, &p1);
    logLine("opttoggle: pos=%d desc=%d called toggle, before=\"%s\" pending=%u/%u",
            pos, chosen, beforeVal, p0, p1);
    return true;
}

void serviceOptAdjust() {
    if (g_optAdjustRow < 0) return;
    DWORD now = GetTickCount();
    if ((long)(now - g_optAdjustNextAt) < 0) return;
    g_optAdjustNextAt = now + 30;                    // throttle: ~30 ms between re-reads

    char text[512];
    resolveLabel(g_base, g_tbwVtbl, g_optAdjustAnchor, text, sizeof text); // refreshes g_optValueOnly
    const char* val = g_optValueOnly[0] ? g_optValueOnly : text;

    bool changed = (val[0] != 0) && strcmp(val, g_optAdjustBefore) != 0;
    bool expired = ((long)(now - g_optAdjustUntil) >= 0);
    if (!changed && !expired) return;                // still waiting for the click to land

    if (val[0]) {
        postSpeech(val);
        g_axSpokenId = g_optAdjustAnchor;
        strncpy(g_axLastSpoken, text, sizeof g_axLastSpoken - 1);
        g_axLastSpoken[sizeof g_axLastSpoken - 1] = 0;
    }
    logLine("optadjust: row pos=%d %s \"%s\" -> \"%s\"", g_optAdjustRow,
            changed ? "changed" : "NO CHANGE within window", g_optAdjustBefore, val);
    g_optAdjustRow = -1;
}
