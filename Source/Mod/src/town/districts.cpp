// town/districts.cpp -- the third TOWN slice

#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cmath>
#include "internal.h"
// ---- TOWN: THE DISTRICT BUILDINGS SCREEN (AX_DISTRICT) — round 1: the reader ----
static const uintptr_t DST_PANEL_OFF   = 0x2a00;   // townRoot+: DistrictDisplay, INLINE (was 0x2a38)
static const uintptr_t DST_SHOWN_OFF   = 0x58;     // panel+: open flag, the town-wide convention
static const uintptr_t DST_STATE_OFF   = 0x80;     // panel+: pass 68/69's panel-internal tween
static const uintptr_t DST_VEC_BEG     = 0x128;    // panel+: vector<BuildingEntry*> begin (was 0x130)
static const uintptr_t DST_VEC_END     = 0x130;    // panel+: vector end                   (was 0x138)
static const uintptr_t DST_ENT_DATA    = 0x00;     // entry+: DistrictBuilding*
static const uintptr_t DST_ENT_STRIP_IDX = 0x10;   // entry+: int, the card's strip index — the
static const int64_t   DST_CHK_ID_BASE = 0x63686b30; // 'chk0'
static const uintptr_t DST_B_DEF       = 0x00;     // building+: Definition*
static const uintptr_t DST_B_BUFF_BEG  = 0x08;     // building+: vector<DistrictBuff*> begin
static const uintptr_t DST_B_BUFF_END  = 0x10;     // building+: vector end
static const uintptr_t DST_B_BUILT     = 0x20;     // building+: char, non-zero = already built
static const uintptr_t DST_C_ID        = 0x00;     // definition+: the id string, inline chars
static const uintptr_t DST_C_COST_BEG  = 0x160;    // definition+: vector<CurrencyCost> begin
static const uintptr_t DST_C_COST_END  = 0x168;    // definition+: vector end
static const uintptr_t DST_COST_STRIDE = 0x48;     // ... identical to RES_WALLET_STRIDE
static const uintptr_t DST_COST_AMOUNT = 0x00;     // cost+: int amount
static const uintptr_t DST_COST_HASH   = 0x44;     // cost+: gameHash(currency id)
// ---- THE STRIP SCROLLS, AND AN OFF-WINDOW BUTTON CANNOT BE CLICKED ----
static const uintptr_t DST_SCROLL_CUR_OFF = 0x154;  // panel+: float, the eased scroll (READ ONLY)
static const uintptr_t DST_SCROLL_TGT_OFF = 0x15c;  // panel+: float, the target -- the one we write
static const uintptr_t DST_SCROLL_ON_OFF  = 0x164;  // panel+: char, 0 = the strip does not scroll
static const uintptr_t DST_SCROLL_MIN_OFF = 0x14c;  // panel+: float, the left stop
static const uintptr_t DST_SCROLL_MAX_OFF = 0x150;  // panel+: float, the strip's full width
static const uintptr_t DST_SCROLL_VIS_OFF = 0x158;  // panel+: float, the visible span Update writes
static const uintptr_t DST_LAYOUT_WIN_POS  = 0x08;
static const uintptr_t DST_LAYOUT_WIN_SIZE = 0x10;
static const float     DST_WIN_INSET       = 30.0f;
static const float     DST_STILL_PX        = 1.0f;
static const uintptr_t DST_BUFF_DESCRIBE_SLOT = 0x18;
typedef void (*DstBuffDescribeFn)(uintptr_t buff, const char* keyPrefix, char* out, int outsz);

static const int DST_MAX_ROWS  = 24;
static const int DST_MAX_COSTS = 6;
static const int DST_MAX_BUFFS = 6;
                                        // miasmal_orchard, conservatory_of_steel)
static const int DST_NAME_MAX  = 96;

struct DstCost { uint32_t hash; int amount; int have; };
struct DstRow {
    uintptr_t entry;                    // the BuildingEntry*
    uintptr_t building;                 // its DistrictBuilding*
    uintptr_t def;                      // its Definition*
    char      id[64];                   // the data id ("bank"), for logs and key building
    char      name[DST_NAME_MAX];
    bool      built;
    bool      afford;                   // every cost line covered by the wallet
    int       stripIdx;
    int       costCount;
    DstCost   costs[DST_MAX_COSTS];
};
static DstRow g_dstRows[DST_MAX_ROWS];
static int    g_dstCount   = 0;
static bool   g_dstActive  = false;     // maintained by checkDistricts (edge-announced)
static int    g_dstRow     = 0;         // the mod's cursor
static int    g_dstTipLine = 0;         // Ctrl+Up/Down position in the effects buffer
static int    g_dstProbes  = 0;         // dumps fired this session (capped)
static DWORD     g_dstBuyUntil    = 0;      // deadline; 0 = no purchase in flight
static bool      g_dstBuySawDlg   = false;  // the confirm dialog was seen open
static uintptr_t g_dstBuyBuilding = 0;      // whose DST_B_BUILT byte is the outcome
static char      g_dstBuyName[DST_NAME_MAX]; // spoken name for the ack
static DWORD     g_dstScrollUntil = 0;      // deadline; 0 = nothing waiting on the scroll
static int64_t   g_dstBuyElem     = 0;      // the 'chk0'+idx element the purchase will click
static float     g_dstScrollLastX = 0;
static bool      g_dstScrollHaveX = false;

bool axIsDistrict() { return g_dstActive; }
                                              // Enter watch reads it (declared in internal.h)

static uintptr_t g_dstFoundPanel = 0;   // fallback's discovery, per town root
static uintptr_t g_dstFoundRoot  = 0;   // ...and the root it was found under
static int       g_dstScanLogs   = 2;   // budget for the fallback's log lines
static uintptr_t dstPanel(uintptr_t base) {
    uintptr_t root = resTownRoot(base);
    if (!root) return 0;
    uintptr_t want = base + DST_VFT_RVA;
    uintptr_t panel = root + DST_PANEL_OFF, vft = 0;
    if (safeReadPtr(panel, &vft) && vft == want) return panel;
    if (g_dstFoundRoot == root && g_dstFoundPanel &&
        safeReadPtr(g_dstFoundPanel, &vft) && vft == want) return g_dstFoundPanel;
    uintptr_t owner = tmbOwner(root);
    if (!owner) return 0;
    for (int g = 0; g < TMB_GROUP_COUNT; g++) {
        uintptr_t beg = 0;
        int n = tmbGroup(owner, g, &beg);
        for (int i = 0; i < n; i++) {
            uintptr_t p = 0;
            if (!safeReadPtr(beg + (uintptr_t)i * 8, &p) || p <= 0x10000) continue;
            if (!safeReadPtr(p, &vft) || vft != want) continue;
            g_dstFoundPanel = p;
            g_dstFoundRoot  = root;
            if (g_dstScanLogs > 0) {
                g_dstScanLogs--;
                logLine("districts: DST_PANEL_OFF 0x%llx did not validate; found the panel by "
                        "vftable in group %d at root+0x%llx -- update the constant",
                        (unsigned long long)DST_PANEL_OFF, g,
                        (unsigned long long)(p - root));
            }
            return p;
        }
    }
    return 0;
}

static bool dstIsOpen(uintptr_t base) {
    uintptr_t panel = dstPanel(base);
    if (!panel) return false;
    uint32_t st = 0;
    if (!safeReadU32(panel + DST_SHOWN_OFF, &st)) return false;
    return st != 0;
}

// ---- Bringing a purchase button into reach ----
static bool dstWindowX(uintptr_t base, float* x0, float* x1) {
    uint32_t px = 0, sx = 0;
    if (!safeReadU32(base + DST_LAYOUT_RVA + DST_LAYOUT_WIN_POS,  &px) ||
        !safeReadU32(base + DST_LAYOUT_RVA + DST_LAYOUT_WIN_SIZE, &sx)) return false;
    float p = u32AsFloatM(px), w = u32AsFloatM(sx);
    if (!(w > 1.0f) || !(p > -10000.0f) || !(p < 10000.0f)) return false;   // never bound -> refuse
    *x0 = p; *x1 = p + w;
    return true;
}

static bool dstElemReachable(uintptr_t base, uintptr_t elem) {
    float cx = 0, cy = 0, x0 = 0, x1 = 0;
    if (!elem || !elemCenter(elem, &cx, &cy)) return false;
    if (!dstWindowX(base, &x0, &x1)) return true;    // window unreadable: never BLOCK on our doubt
    return cx >= x0 + DST_WIN_INSET && cx <= x1 - DST_WIN_INSET;
}

static bool dstScrollElemIntoView(uintptr_t base, uintptr_t elem) {
    uintptr_t panel = dstPanel(base);
    float cx = 0, cy = 0, x0 = 0, x1 = 0;
    uint32_t curBits = 0;
    uint8_t  canScroll = 0;
    if (!panel || !elem || !elemCenter(elem, &cx, &cy)) return false;
    if (!dstWindowX(base, &x0, &x1)) return false;
    if (!safeReadU8(panel + DST_SCROLL_ON_OFF, &canScroll) || !canScroll) return false;
    if (!safeReadU32(panel + DST_SCROLL_CUR_OFF, &curBits)) return false;
    float cur  = u32AsFloatM(curBits);
    float want = cx + cur - (x0 + x1) * 0.5f;
    uint32_t loBits = 0, hiBits = 0, visBits = 0;
    if (safeReadU32(panel + DST_SCROLL_MIN_OFF, &loBits) &&
        safeReadU32(panel + DST_SCROLL_MAX_OFF, &hiBits) &&
        safeReadU32(panel + DST_SCROLL_VIS_OFF, &visBits)) {
        float lo = u32AsFloatM(loBits), hi = u32AsFloatM(hiBits) - u32AsFloatM(visBits);
        if (hi < lo) hi = lo;                        // degenerate: everything fits, do not scroll
        if (want < lo) want = lo;
        if (want > hi) want = hi;
    }
    uint32_t bits = 0;
    memcpy(&bits, &want, sizeof bits);
    if (!safeWriteU32(panel + DST_SCROLL_TGT_OFF, bits)) return false;
    safeWriteU32(panel + DST_SCROLL_CUR_OFF, bits);
    logLine("districts: button at x=%.0f is outside the window %.0f..%.0f -- scroll %.0f, jumping "
            "to %.0f", cx, x0, x1, cur, want);
    return true;
}

static void dstFireBuyClick() {
    if (frontEndClickElementId(g_dstBuyElem)) {
        g_dstBuyUntil  = GetTickCount() + 1500;
        g_dstBuySawDlg = false;
        // Silent on purpose: the confirm dialog announces itself the moment it opens.
    } else {
        postSpeech(axs(AXS_ACTION_FAILED));      // click machinery busy; nothing was risked
    }
}

static void dstCurrencyName(uintptr_t base, uint32_t hash, char* out, int outsz) {
    resCurrencyTitle(base, hash, "districts", out, outsz);
}

static int dstReadCosts(uintptr_t base, uintptr_t def, DstCost* out, int max) {
    uintptr_t beg = 0, end = 0;
    if (!safeReadPtr(def + DST_C_COST_BEG, &beg) || !safeReadPtr(def + DST_C_COST_END, &end) ||
        !beg || end < beg) return 0;
    uintptr_t span = end - beg;
    if (span % DST_COST_STRIDE) return 0;                 // not this layout -> trust nothing
    int n = (int)(span / DST_COST_STRIDE);
    if (n < 0 || n > 32) return 0;
    int got = 0;
    for (int i = 0; i < n && got < max; i++) {
        uintptr_t rec = beg + (uintptr_t)i * DST_COST_STRIDE;
        uint32_t amt = 0, hash = 0;
        if (!safeReadU32(rec + DST_COST_AMOUNT, &amt) || !safeReadU32(rec + DST_COST_HASH, &hash))
            continue;
        if ((int)amt <= 0) continue;                      // the game draws no line for these
        out[got].hash   = hash;
        out[got].amount = (int)amt;
        out[got].have   = 0;
        resWalletAmount(base, hash, &out[got].have);      // absent = 0, which reads as unaffordable
        got++;
    }
    return got;
}

static bool dstRowBefore(const DstRow* a, const DstRow* b) {
    int ga = a->built ? 0 : (a->afford ? 1 : 2);
    int gb = b->built ? 0 : (b->afford ? 1 : 2);
    if (ga != gb) return ga < gb;
    return _stricmp(a->name, b->name) < 0;
}

static int dstCollect(uintptr_t base, bool probe) {
    g_dstCount = 0;
    uintptr_t panel = dstPanel(base);
    if (!panel) return 0;
    uintptr_t beg = 0, end = 0;
    if (!safeReadPtr(panel + DST_VEC_BEG, &beg) || !safeReadPtr(panel + DST_VEC_END, &end) ||
        !beg || end < beg || (end - beg) % 8) return 0;
    int n = (int)((end - beg) / 8);
    if (n < 0 || n > DST_MAX_ROWS) {
        logLine("districts: entry count %d out of range -- layout wrong, reading nothing", n);
        return 0;
    }
    for (int i = 0; i < n && g_dstCount < DST_MAX_ROWS; i++) {
        uintptr_t entry = 0, b = 0, def = 0;
        if (!safeReadPtr(beg + (uintptr_t)i * 8, &entry) || entry <= 0x10000) continue;
        if (!safeReadPtr(entry + DST_ENT_DATA, &b) || b <= 0x10000) continue;
        if (!safeReadPtr(b + DST_B_DEF, &def) || def <= 0x10000) continue;
        DstRow r;
        r.entry = entry; r.building = b; r.def = def;
        r.id[0] = 0;
        if (!safeReadCStr(def + DST_C_ID, r.id, sizeof r.id) || !r.id[0]) {
            logLine("districts: entry %d has no readable id -- skipped", i);
            continue;
        }
        uint8_t built = 0;
        safeReadU8(b + DST_B_BUILT, &built);
        r.built = built != 0;
        uint32_t sidx = 0;
        r.stripIdx = (safeReadU32(entry + DST_ENT_STRIP_IDX, &sidx) && sidx < 64)
                     ? (int)sidx : -1;               // -1 = no element id; Enter refuses safely
        char key[128];
        _snprintf(key, sizeof key, "str_%s_title", r.id);
        key[sizeof key - 1] = 0;
        r.name[0] = 0;
        if (!resolveKey(base, key, r.name, sizeof r.name) || !r.name[0]) {
            _snprintf(r.name, sizeof r.name, "%s", r.id);
            r.name[sizeof r.name - 1] = 0;
            logLine("districts: no %s string -- speaking the raw id", key);
        }
        r.costCount = dstReadCosts(base, def, r.costs, DST_MAX_COSTS);
        r.afford = r.costCount > 0;
        for (int c = 0; c < r.costCount; c++)
            if (r.costs[c].have < r.costs[c].amount) r.afford = false;
        if (probe)
            logLine("districts: strip entry %d id=\"%s\" name=\"%s\" built=%d costs=%d afford=%d "
                    "stripIdx=%d entry=%p b=%p def=%p", i, r.id, r.name, r.built ? 1 : 0,
                    r.costCount, r.afford ? 1 : 0, r.stripIdx, (void*)entry, (void*)b, (void*)def);
        int at = g_dstCount;
        while (at > 0 && dstRowBefore(&r, &g_dstRows[at - 1])) {
            g_dstRows[at] = g_dstRows[at - 1];
            at--;
        }
        g_dstRows[at] = r;
        g_dstCount++;
    }
    return g_dstCount;
}

static void dstRowText(uintptr_t base, int i, char* out, int outsz) {
    out[0] = 0;
    if (i < 0 || i >= g_dstCount) return;
    const DstRow* r = &g_dstRows[i];
    if (r->built) {
        _snprintf(out, outsz, "%s. %s", r->name, axs(AXS_DST_ROW_BUILT));
        out[outsz - 1] = 0;
        return;
    }
    char costs[320]; costs[0] = 0;
    for (int c = 0; c < r->costCount; c++) {
        char word[64];
        dstCurrencyName(base, r->costs[c].hash, word, sizeof word);
        char unit[96];
        _snprintf(unit, sizeof unit, axs(AXS_DST_COST_FMT), r->costs[c].amount, word);
        unit[sizeof unit - 1] = 0;
        char one[128];
        _snprintf(one, sizeof one, "%s%s", c ? ", " : "", unit);
        one[sizeof one - 1] = 0;
        strncat(costs, one, sizeof costs - strlen(costs) - 1);
    }
    _snprintf(out, outsz, "%s. %s%s%s", r->name,
              r->afford ? axs(AXS_DST_ROW_NOT_BUILT) : axs(AXS_DST_ROW_CANT_AFFORD),
              costs[0] ? " " : "", costs);
    out[outsz - 1] = 0;
}

static int dstTipLines(uintptr_t base, int i, int line, char* out, int outsz) {
    out[0] = 0;
    if (i < 0 || i >= g_dstCount) return 0;
    const DstRow* r = &g_dstRows[i];
    char texts[DST_MAX_BUFFS][256];
    int  got = 0;
    uintptr_t beg = 0, end = 0;
    if (safeReadPtr(r->building + DST_B_BUFF_BEG, &beg) &&
        safeReadPtr(r->building + DST_B_BUFF_END, &end) &&
        beg && end >= beg && (end - beg) % 8 == 0) {
        int n = (int)((end - beg) / 8);
        if (n >= 0 && n <= 32) {
            char prefix[128];
            _snprintf(prefix, sizeof prefix, "str_%s_buff", r->id);
            prefix[sizeof prefix - 1] = 0;
            for (int k = 0; k < n && got < DST_MAX_BUFFS; k++) {
                uintptr_t buff = 0, vft = 0, fn = 0;
                if (!safeReadPtr(beg + (uintptr_t)k * 8, &buff) || buff <= 0x10000) continue;
                if (!safeReadPtr(buff, &vft) || vft <= 0x10000) continue;
                if (!safeReadPtr(vft + DST_BUFF_DESCRIBE_SLOT, &fn) || fn <= 0x10000) continue;
                char buf[256]; buf[0] = 0;
                ((DstBuffDescribeFn)fn)(buff, prefix, buf, (int)sizeof buf);
                abStripMarkup(buf);
                if (!buf[0]) continue;                     // this buff has no words; not an error
                _snprintf(texts[got], sizeof texts[got], "%s", buf);
                texts[got][sizeof texts[got] - 1] = 0;
                got++;
            }
        }
    }
    int total = 1 + got;
    if (line < 0) line = 0;
    if (line >= total) line = total - 1;
    if (line == 0) dstRowText(base, i, out, outsz);
    else { _snprintf(out, outsz, "%s", texts[line - 1]); out[outsz - 1] = 0; }
    if (total == 1 && line == 0) {
        strncat(out, " ", outsz - strlen(out) - 1);
        strncat(out, axs(AXS_DST_NO_EFFECTS), outsz - strlen(out) - 1);
    }
    return total;
}

static void dstSpeakRow(uintptr_t base, const char* prefix) {
    char row[512];
    dstRowText(base, g_dstRow, row, sizeof row);
    char pos[48];
    _snprintf(pos, sizeof pos, axs(AXS_POS_N_OF_M), g_dstRow + 1, g_dstCount);
    pos[sizeof pos - 1] = 0;
    char line[640];
    _snprintf(line, sizeof line, "%s%s %s", prefix ? prefix : "", row, pos);
    line[sizeof line - 1] = 0;
    postSpeech(line);
}

static void dstTitleName(uintptr_t base, char* out, int outsz) {
    if (resolveKey(base, "town_name_district", out, outsz) && out[0]) return;
    strncpy(out, axs(AXS_DST_TITLE_FALLBACK), outsz - 1);
    out[outsz - 1] = 0;
}

void checkDistricts(uintptr_t base) {
    bool open = dstIsOpen(base);

    if (g_dstScrollUntil) {
        if (!open) {
            g_dstScrollUntil = 0;                    // screen went away; nothing to buy
            g_dstScrollHaveX = false;
        } else {
            uintptr_t elem = feGetElementById(g_dstBuyElem);
            float cx = 0, cy = 0;
            bool  have = elem && elemCenter(elem, &cx, &cy);
            bool  still = have && g_dstScrollHaveX && fabsf(cx - g_dstScrollLastX) < DST_STILL_PX;
            if (have) { g_dstScrollLastX = cx; g_dstScrollHaveX = true; }
            if (still && dstElemReachable(base, elem)) {
                logLine("districts: the strip settled with the button at x=%.0f -- clicking", cx);
                g_dstScrollUntil = 0; g_dstScrollHaveX = false;
                dstFireBuyClick();
            } else if (GetTickCount() > g_dstScrollUntil) {
                g_dstScrollUntil = 0; g_dstScrollHaveX = false;
                logLine("districts: the strip never settled with 0x%llx in the window (last x=%.0f,"
                        " readable=%d) -- clicking where it is",
                        (unsigned long long)g_dstBuyElem, cx, have ? 1 : 0);
                dstFireBuyClick();
            }
        }
    }

    if (g_dstBuyUntil) {
        if (!open) {
            g_dstBuyUntil = 0; g_dstBuySawDlg = false;   // screen went away; nothing to report on
        } else if (confirmDialogOpen(base)) {
            g_dstBuySawDlg = true;
            g_dstBuyUntil  = GetTickCount() + 3000;
        } else if (g_dstBuySawDlg) {
            g_dstBuyUntil = 0; g_dstBuySawDlg = false;
            uint8_t built = 0;
            if (safeReadU8(g_dstBuyBuilding + DST_B_BUILT, &built) && built) {
                char line[DST_NAME_MAX + 32];
                _snprintf(line, sizeof line, axs(AXS_DST_BUILT), g_dstBuyName);
                line[sizeof line - 1] = 0;
                postSpeech(line);
                logLine("districts: \"%s\" built (byte flipped after the dialog)", g_dstBuyName);
                dstCollect(base, false);
                for (int i = 0; i < g_dstCount; i++)
                    if (g_dstRows[i].building == g_dstBuyBuilding) { g_dstRow = i; break; }
                g_dstTipLine = 0;
            } else {
                logLine("districts: purchase dialog closed without the built byte flipping "
                        "(declined, or the wrong building watched)");
            }
        } else if (GetTickCount() > g_dstBuyUntil) {
            g_dstBuyUntil = 0;
            logLine("districts: purchase click on \"%s\" raised no confirm dialog", g_dstBuyName);
            postSpeech(axs(AXS_DST_BUY_NO_DIALOG));
        }
    }

    if (axIsLoading() || circusMatchLoading()) {
        if (open != g_dstActive)
            logLine("districts: open=%d while a loading screen owns the floor -- edge held",
                    open ? 1 : 0);
        return;
    }
    if (open == g_dstActive) return;
    g_dstActive = open;
    char name[64];
    if (!open) {
        g_dstRow = 0; g_dstTipLine = 0;
        dstTitleName(base, name, sizeof name);
        char line[96];                               // TL_CLOSED_FMT: the shared close-line template
        _snprintf(line, sizeof line, axs(AXS_TL_CLOSED_FMT), name);
        line[sizeof line - 1] = 0;
        postSpeech(line);
        return;
    }
    g_dstRow = 0; g_dstTipLine = 0;
    bool probe = g_dstProbes < 3;
    if (probe) g_dstProbes++;
    if (probe) {
        uintptr_t panel = dstPanel(base);
        uint32_t shown = 0, tween = 0;
        if (panel) {
            safeReadU32(panel + DST_SHOWN_OFF, &shown);
            safeReadU32(panel + DST_STATE_OFF, &tween);
            logLine("districts: open edge, panel=root+0x%llx shown(+0x58)=%u tween(+0x80)=%u",
                    (unsigned long long)(panel - resTownRoot(base)), shown, tween);
            float x0 = 0, x1 = 0;
            uint32_t curBits = 0, tgtBits = 0;
            uint8_t  canScroll = 0;
            bool haveWin = dstWindowX(base, &x0, &x1);
            safeReadU32(panel + DST_SCROLL_CUR_OFF, &curBits);
            safeReadU32(panel + DST_SCROLL_TGT_OFF, &tgtBits);
            safeReadU8(panel + DST_SCROLL_ON_OFF, &canScroll);
            logLine("districts: list window %s x %.0f..%.0f, scroll cur=%.0f tgt=%.0f scrollable=%u",
                    haveWin ? "read" : "UNREADABLE", x0, x1,
                    u32AsFloatM(curBits), u32AsFloatM(tgtBits), canScroll);
        }
    }
    int n = dstCollect(base, probe);
    dstTitleName(base, name, sizeof name);
    if (n <= 0) {
        logLine("districts: panel open but 0 rows collected -- layout check needed");
        char line[160];
        _snprintf(line, sizeof line, axs(AXS_DST_LIST_UNREADABLE_FMT), name);
        line[sizeof line - 1] = 0;
        postSpeech(line);
        return;
    }
    char title[128], prefix[240];
    _snprintf(title, sizeof title, axs(AXS_DST_TITLE_N_FMT), name, n);
    title[sizeof title - 1] = 0;
    int bp = 0;
    resWalletAmount(base, resHash("blueprint"), &bp);
    char cur[64], bpLine[96];
    dstCurrencyName(base, resHash("blueprint"), cur, sizeof cur);
    _snprintf(bpLine, sizeof bpLine, axs(AXS_DST_BLUEPRINTS_FMT), bp, cur);
    bpLine[sizeof bpLine - 1] = 0;
    _snprintf(prefix, sizeof prefix, "%s %s ", title, bpLine);  // trailing space at the call site
    prefix[sizeof prefix - 1] = 0;
    dstSpeakRow(base, prefix);
}

bool routeDistrictKey(uintptr_t base, uint32_t sym, uint16_t mod, uint8_t repeat) {
    if (g_dstCount <= 0) return false;
    bool ctrl = (mod & (KMOD_LCTRL | KMOD_RCTRL)) != 0;
    if (ctrl && (sym == SDLK_UP || sym == SDLK_DOWN)) {
        char probe[512];
        int total = dstTipLines(base, g_dstRow, 0, probe, sizeof probe);
        int next  = g_dstTipLine + (sym == SDLK_UP ? 1 : -1);
        if (next < 0) next = total - 1;
        if (next >= total) next = 0;
        g_dstTipLine = next;
        char line[640];
        dstTipLines(base, g_dstRow, g_dstTipLine, line, sizeof line);
        postSpeech(line);
        return true;
    }
    int jump = 0;
    if (axDecodeJump(sym, mod, repeat, &jump)) {     // Home/End: first/last district card
        if (!jump) return true;                      // held jump: one landing per press
        dstCollect(base, false);                     // live list, same rule as the arrows
        if (g_dstCount <= 0) return true;
        axStepCursor(&g_dstRow, g_dstCount, jump);   // the clamp lands it; ends re-read
        g_dstTipLine = 0;
        dstSpeakRow(base, nullptr);
        return true;
    }
    if (sym == SDLK_UP || sym == SDLK_DOWN) {
        if (axNavHoldRepeat(repeat)) return true;    // throttled repeat: claimed, no step
        dstCollect(base, false);
        if (g_dstCount <= 0) return true;
        axStepCursor(&g_dstRow, g_dstCount, sym == SDLK_DOWN ? 1 : -1);
        g_dstTipLine = 0;
        dstSpeakRow(base, nullptr);
        return true;
    }
    if (sym == SDLK_RETURN || sym == SDLK_KP_ENTER) {
        if (repeat) return true;
        if (g_dstBuyUntil || g_dstScrollUntil) return true;   // a purchase is already in flight
        dstCollect(base, false);
        if (g_dstRow < 0 || g_dstRow >= g_dstCount) return true;
        const DstRow* r = &g_dstRows[g_dstRow];
        char line[512];
        if (r->built) {
            _snprintf(line, sizeof line, axs(AXS_DST_ALREADY_BUILT_FMT), r->name);
            line[sizeof line - 1] = 0;
            postSpeech(line);
            return true;
        }
        if (!r->afford) {
            char word[64]; word[0] = 0; int missing = 0;
            for (int c = 0; c < r->costCount; c++)
                if (r->costs[c].have < r->costs[c].amount) {
                    if (!word[0]) { dstCurrencyName(base, r->costs[c].hash, word, sizeof word);
                                    missing = r->costs[c].amount - r->costs[c].have; }
                }
            if (word[0]) _snprintf(line, sizeof line, axs(AXS_DST_NEED_MORE_FMT), missing, word);
            else         _snprintf(line, sizeof line, axs(AXS_DST_CANT_AFFORD_FMT), r->name);
            line[sizeof line - 1] = 0;
            postSpeech(line);
            return true;
        }
        int64_t elemId = DST_CHK_ID_BASE + r->stripIdx;
        uintptr_t elem = (r->stripIdx >= 0) ? feGetElementById(elemId) : 0;
        logLine("districts: Enter on \"%s\" built=0 afford=1 stripIdx=%d elem=0x%llx offered=%d",
                r->id, r->stripIdx, (unsigned long long)elemId, elem ? 1 : 0);
        if (!elem) {
            postSpeech(axs(AXS_DST_NOT_OFFERED));
            return true;
        }
        g_dstBuyElem     = elemId;
        g_dstBuyBuilding = r->building;
        strncpy(g_dstBuyName, r->name, sizeof g_dstBuyName - 1);
        g_dstBuyName[sizeof g_dstBuyName - 1] = 0;
        if (dstElemReachable(base, elem)) {
            dstFireBuyClick();
        } else if (dstScrollElemIntoView(base, elem)) {
            g_dstScrollUntil = GetTickCount() + 3000;
            g_dstScrollHaveX = false;         // no previous frame to settle against
        } else {
            logLine("districts: \"%s\" is out of the window and the strip will not scroll -- "
                    "clicking anyway", r->id);
            dstFireBuyClick();
        }
        return true;
    }
    return false;
}

void dstReannounce(uintptr_t base) {
    if (!g_dstActive) return;
    dstCollect(base, false);
    char name[64], prefix[96];
    dstTitleName(base, name, sizeof name);
    _snprintf(prefix, sizeof prefix, "%s. ", name);  // a name join, no words -- stays in code
    prefix[sizeof prefix - 1] = 0;
    dstSpeakRow(base, prefix);
}
