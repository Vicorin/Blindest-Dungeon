// town/exchange.cpp -- the ninth TOWN slice

#include <cstdint>
#include <cstring>
#include <cstdio>
#include "internal.h"
// ---- TOWN: the HEIRLOOM EXCHANGE (TownUI::Panel::HeirloomExchangeDisplay) ----
static const int       EX_PANEL_INDEX = 0;         // TogglePanel case 0 = the Heirloom Exchange
static const uintptr_t EX_PANEL_OFF   = 0x45b8;
static const uintptr_t EX_OPEN_OFF    = 0x4610;
static const uintptr_t EX_SEL_SRC_OFF = 0x84;      // panel+: selected SOURCE heirloom id (hash)
static const uintptr_t EX_QTY_OFF     = 0x88;      // panel+: offer QUANTITY (int; NOT a target id)
static const uint32_t  EX_ID_QTY_UP   = 0x68657570u + 1;  // offer-quantity increase arrow element
static const uint32_t  EX_ID_QTY_DN   = 0x6865646eu + 1;  // offer-quantity decrease arrow element
static const uint32_t  EX_ID_CELL_BASE= 0x68656366u;      // result cell element base (+ cellIdx)

static bool  g_exActive   = false;                 // the exchange is the surface (mirrors the flag)
static bool  g_exProbed   = false;
static int   g_exCol      = 0;                     // 0 = "Your heirlooms", 1 = "You can get"
static int   g_exRow      = 0;                     // row cursor within the current column
static bool  g_exPicking  = false;                 // the quantity picker owns Up/Down/Enter
static bool  g_exPickHint = false;                 // the full picker instruction has been spoken
                                                   //   this OPENING (reset on the open edge)

static DWORD g_exQtyWatchUntil = 0;                // 0 = idle
static int   g_exQtyBefore     = 0;

static DWORD    g_exTradeWatchUntil = 0;           // 0 = idle
static uint32_t g_exTradeSrcId = 0, g_exTradeTgtId = 0;
static int      g_exTradeSrcBefore = 0, g_exTradeTgtBefore = 0;

bool axIsExchange() { return g_exActive; }   // non-static: the kAxContexts row (dllmain.cpp)

static uintptr_t exPanel(uintptr_t base) {
    uintptr_t root = resTownRoot(base);
    if (!root) return 0;
    uintptr_t panel = root + EX_PANEL_OFF;
    uintptr_t vft = 0;
    if (!safeReadPtr(panel, &vft) || vft != base + EX_VFT_RVA) return 0;
    return panel;
}

bool exIsOpen(uintptr_t base) {
    uintptr_t root = resTownRoot(base);
    if (!root) return false;
    uint32_t f = 0;
    return safeReadU32(root + EX_OPEN_OFF, &f) && f != 0;
}

static const char* exCurrencyName(uint32_t idHash) {
    const ResCurrency* c = resCurrencyFind(idHash);
    return c ? c->id : "?";
}

// ---- Exchange data model ----

static bool exRates(uintptr_t base, uintptr_t* begOut, int* nOut) {
    *begOut = 0; *nOut = 0;
    uintptr_t block = 0;
    if (!safeReadPtr(base + EX_RATES_RVA, &block) || block <= 0x10000) return false;
    uintptr_t beg = 0, end = 0;
    if (!safeReadPtr(block, &beg) || !safeReadPtr(block + 8, &end) || end < beg) return false;
    uintptr_t span = end - beg;
    if (span % 0x10) return false;
    int n = (int)(span / 0x10);
    if (n <= 0 || n > 64) return false;
    *begOut = beg; *nOut = n;
    return true;
}

static int exRegistryIds(uintptr_t base, uint32_t* ids, int maxIds) {
    uintptr_t block = 0;
    if (!safeReadPtr(base + EX_REG_RVA, &block) || block <= 0x10000) return 0;
    uintptr_t beg = 0, end = 0;
    if (!safeReadPtr(block, &beg) || !safeReadPtr(block + 8, &end) || end < beg) return 0;
    int n = (int)((end - beg) / 8);
    if (n <= 0 || n > 32) return 0;
    int out = 0;
    for (int i = 0; i < n && out < maxIds; i++) {
        uintptr_t def = 0;
        if (!safeReadPtr(beg + (uintptr_t)i * 8, &def) || def <= 0x10000) continue;
        uint32_t id = 0;
        if (!safeReadU32(def + 0x40, &id)) continue;
        ids[out++] = id;
    }
    return out;
}

static bool exRateFor(uintptr_t base, uint32_t srcId, uint32_t tgtId, int* srcBatch, int* tgtBatch) {
    uintptr_t beg = 0; int n = 0;
    if (!exRates(base, &beg, &n)) return false;
    for (int i = 0; i < n; i++) {
        uint32_t s = 0, t = 0; int sb = 0, tb = 0;
        safeReadU32(beg + (uintptr_t)i * 0x10 + 0x0, &s);
        safeReadU32(beg + (uintptr_t)i * 0x10 + 0x8, &t);
        if (s != srcId || t != tgtId) continue;
        safeReadU32(beg + (uintptr_t)i * 0x10 + 0x4, (uint32_t*)&sb);
        safeReadU32(beg + (uintptr_t)i * 0x10 + 0xc, (uint32_t*)&tb);
        if (sb <= 0) return false;
        *srcBatch = sb; *tgtBatch = tb;
        return true;
    }
    return false;
}

static bool exIsRateSource(uintptr_t base, uint32_t id) {
    uintptr_t beg = 0; int n = 0;
    if (!exRates(base, &beg, &n)) return false;
    for (int i = 0; i < n; i++) {
        uint32_t s = 0;
        if (safeReadU32(beg + (uintptr_t)i * 0x10, &s) && s == id) return true;
    }
    return false;
}
// ... and as a rate TARGET? (The game's own cell-emit filter.)
static bool exIsRateTarget(uintptr_t base, uint32_t id) {
    uintptr_t beg = 0; int n = 0;
    if (!exRates(base, &beg, &n)) return false;
    for (int i = 0; i < n; i++) {
        uint32_t t = 0;
        if (safeReadU32(beg + (uintptr_t)i * 0x10 + 8, &t) && t == id) return true;
    }
    return false;
}

static void exTypeTitle(uintptr_t base, uint32_t idHash, char* out, int outsz) {
    resCurrencyTitle(base, idHash, "exchange", out, outsz);
}

struct ExRow {
    uint32_t id;         // currency id hash
    int      held;       // wallet count
    int      receive;
    int      cost;
    int      cellIdx;
};
static ExRow g_exLeft[8];  static int g_exLeftN  = 0;
static ExRow g_exRight[8]; static int g_exRightN = 0;

static void exCollect(uintptr_t base, uintptr_t panel) {
    g_exLeftN = 0; g_exRightN = 0;
    uint32_t src = 0, qty = 0;
    safeReadU32(panel + EX_SEL_SRC_OFF, &src);
    safeReadU32(panel + EX_QTY_OFF, &qty);
    uint32_t ids[16];
    int n = exRegistryIds(base, ids, 16);
    int cellIdx = 0;
    for (int i = 0; i < n; i++) {
        uint32_t id = ids[i];
        // Left column: everything offerable.
        if (exIsRateSource(base, id) && g_exLeftN < 8) {
            ExRow* r = &g_exLeft[g_exLeftN++];
            r->id = id; r->receive = 0; r->cost = 0; r->cellIdx = -1;
            r->held = 0; resWalletAmount(base, id, &r->held);
        }
        if (id != src && exIsRateTarget(base, id) && g_exRightN < 8) {
            ExRow* r = &g_exRight[g_exRightN++];
            r->id = id; r->cellIdx = cellIdx++;
            r->held = 0; resWalletAmount(base, id, &r->held);
            int sb = 0, tb = 0;
            if (exRateFor(base, src, id, &sb, &tb) && sb > 0) {
                int batches = (int)qty / sb;               // the cell body's own math
                r->cost    = batches * sb;
                r->receive = batches * tb;
            } else { r->cost = 0; r->receive = 0; }
        }
    }
}

static void exRowText(uintptr_t base, uintptr_t panel, char* out, int outsz) {
    out[0] = 0;
    uint32_t src = 0, qty = 0;
    safeReadU32(panel + EX_SEL_SRC_OFF, &src);
    safeReadU32(panel + EX_QTY_OFF, &qty);
    char name[96], srcName[96];
    if (g_exCol == 0) {
        if (g_exRow >= g_exLeftN) g_exRow = g_exLeftN ? g_exLeftN - 1 : 0;
        if (g_exLeftN == 0) { strncpy(out, "No heirlooms.", outsz - 1); out[outsz-1] = 0; return; }
        ExRow* r = &g_exLeft[g_exRow];
        exTypeTitle(base, r->id, name, sizeof name);
        if (r->id == src)
            _snprintf(out, outsz, axs(AXS_EX_HELD_SEL_FMT), name, r->held, qty);
        else
            _snprintf(out, outsz, axs(AXS_EX_HELD_FMT), name, r->held);
    } else {
        if (g_exRow >= g_exRightN) g_exRow = g_exRightN ? g_exRightN - 1 : 0;
        if (g_exRightN == 0) { strncpy(out, "Nothing to trade for.", outsz - 1); out[outsz-1] = 0; return; }
        ExRow* r = &g_exRight[g_exRow];
        exTypeTitle(base, r->id, name, sizeof name);
        exTypeTitle(base, src, srcName, sizeof srcName);
        if (r->receive > 0)
            _snprintf(out, outsz, axs(AXS_EX_GET_FMT), name, r->receive, r->cost, srcName);
        else
            _snprintf(out, outsz, axs(AXS_EX_NOTHING_FMT), name);
    }
    out[outsz - 1] = 0;
}

static void exSpeakRow(uintptr_t base, const char* prefix, bool withHeader) {
    uintptr_t panel = exPanel(base);
    if (!panel) return;
    exCollect(base, panel);
    int n = (g_exCol == 0) ? g_exLeftN : g_exRightN;
    char row[256];
    exRowText(base, panel, row, sizeof row);
    char utter[MAILBOX_SZ];
    char header[128] = { 0 };
    if (withHeader)
        _snprintf(header, sizeof header, "%s ",
                  axs(g_exCol == 0 ? AXS_EX_COL_YOURS : AXS_EX_COL_GET));
    if (n > 0) {
        char pos[64];
        _snprintf(pos, sizeof pos, axs(AXS_POS_N_OF_M), g_exRow + 1, n);
        pos[sizeof pos - 1] = 0;
        _snprintf(utter, sizeof utter, "%s%s%s %s", prefix ? prefix : "", header, row, pos);
    } else {
        _snprintf(utter, sizeof utter, "%s%s%s", prefix ? prefix : "", header, row);
    }
    utter[sizeof utter - 1] = 0;
    postSpeech(utter);
}

static void exSpeakPicker(uintptr_t base, uintptr_t panel);

void exReannounce(uintptr_t base) {
    if (!g_exActive) return;
    logLine("exchange: re-announcing after a modal closed (picking=%d)", g_exPicking ? 1 : 0);
    if (g_exPicking) {
        uintptr_t panel = exPanel(base);
        if (panel) { exSpeakPicker(base, panel); return; }
        g_exPicking = false;
    }
    exSpeakRow(base, "Heirloom exchange. ", true);
}

static void exProbe(uintptr_t base, uintptr_t panel) {
    uint32_t shown = 0, src = 0, qty = 0;
    uintptr_t root = resTownRoot(base);
    if (root) safeReadU32(root + EX_OPEN_OFF, &shown);
    safeReadU32(panel + EX_SEL_SRC_OFF, &src);
    safeReadU32(panel + EX_QTY_OFF, &qty);
    logLine("exchange probe: panel=%p shown=%u src=%u(%s) qty=%u",
            (void*)panel, shown, src, exCurrencyName(src), qty);
    uintptr_t beg = 0; int n = 0;
    if (exRates(base, &beg, &n)) {
        for (int i = 0; i < n; i++) {
            uint32_t s = 0, t = 0, sb = 0, tb = 0;
            safeReadU32(beg + (uintptr_t)i*0x10 + 0x0, &s);
            safeReadU32(beg + (uintptr_t)i*0x10 + 0x4, &sb);
            safeReadU32(beg + (uintptr_t)i*0x10 + 0x8, &t);
            safeReadU32(beg + (uintptr_t)i*0x10 + 0xc, &tb);
            logLine("exchange probe: rate[%d] %s x%u -> %s x%u",
                    i, exCurrencyName(s), sb, exCurrencyName(t), tb);
        }
    } else logLine("exchange probe: RATES NOT READABLE");
    exCollect(base, panel);
    for (int i = 0; i < g_exLeftN; i++)
        logLine("exchange probe: left[%d] id=%u(%s) held=%d", i,
                g_exLeft[i].id, exCurrencyName(g_exLeft[i].id), g_exLeft[i].held);
    for (int i = 0; i < g_exRightN; i++)
        logLine("exchange probe: right[%d] id=%u(%s) held=%d cell=%d receive=%d cost=%d", i,
                g_exRight[i].id, exCurrencyName(g_exRight[i].id), g_exRight[i].held,
                g_exRight[i].cellIdx, g_exRight[i].receive, g_exRight[i].cost);
    // Do the game's per-frame FocusElements resolve by id right now?
    struct { const char* what; uint32_t id; } elems[] = {
        { "qty-up",  EX_ID_QTY_UP }, { "qty-down", EX_ID_QTY_DN },
        { "cell0", EX_ID_CELL_BASE + 0 }, { "cell1", EX_ID_CELL_BASE + 1 },
        { "cell2", EX_ID_CELL_BASE + 2 },
    };
    for (int i = 0; i < 5; i++) {
        uintptr_t e = focusElementById(base, (int64_t)elems[i].id);
        logLine("exchange probe: element %s id=0x%x -> %p", elems[i].what, elems[i].id, (void*)e);
    }
    logLine("exchange probe: end");
}

void exOpen(uintptr_t base) {
    uintptr_t root = resTownRoot(base);
    if (!root) { postSpeech(axs(AXS_EX_NOT_AVAILABLE)); return; }
    if (exIsOpen(base)) { logLine("exchange: Trade pressed but already open"); return; }
    typedef void (__fastcall *ExToggleFn)(uintptr_t townRoot, int index);
    logLine("exchange: Trade -> TogglePanel(root=%p, %d)", (void*)root, EX_PANEL_INDEX);
    __try { reinterpret_cast<ExToggleFn>(base + EX_TOGGLE_RVA)(root, EX_PANEL_INDEX); }
    __except (EXCEPTION_EXECUTE_HANDLER) { logLine("exchange: TogglePanel threw"); }
}

static void exSpeakPicker(uintptr_t base, uintptr_t panel) {
    uint32_t src = 0, qty = 0;
    safeReadU32(panel + EX_SEL_SRC_OFF, &src);
    safeReadU32(panel + EX_QTY_OFF, &qty);
    char name[96], utter[MAILBOX_SZ];
    exTypeTitle(base, src, name, sizeof name);
    if (!g_exPickHint) {
        g_exPickHint = true;
        _snprintf(utter, sizeof utter, axs(AXS_EX_OFFERING_HINT_FMT), qty, name);
    } else {
        _snprintf(utter, sizeof utter, axs(AXS_EX_OFFERING_SET_FMT), qty, name);
    }
    utter[sizeof utter - 1] = 0;
    postSpeech(utter);
}

static void exSelectSource(uintptr_t base, uintptr_t panel) {
    if (g_exRow >= g_exLeftN) return;
    ExRow* r = &g_exLeft[g_exRow];
    uint32_t src = 0;
    safeReadU32(panel + EX_SEL_SRC_OFF, &src);
    if (r->id != src) {
        if (!exIsRateSource(base, r->id)) {
            postSpeech(axs(AXS_EX_CANT_OFFER)); return;  // never act outside the game's own domain
        }
        typedef int (__fastcall *ExDefQtyFn)(uintptr_t panel);
        safeWriteU32(panel + EX_SEL_SRC_OFF, r->id);
        int defQty = 0;
        __try { defQty = ((ExDefQtyFn)(base + EX_DEFQTY_RVA))(panel); }
        __except (EXCEPTION_EXECUTE_HANDLER) { defQty = 0; logLine("exchange: default-qty fn threw"); }
        safeWriteU32(panel + EX_QTY_OFF, (uint32_t)defQty);
        logLine("exchange: source -> %s, offer %d", exCurrencyName(r->id), defQty);
    } else {
        logLine("exchange: re-opening the picker on the current source %s", exCurrencyName(src));
    }
    g_exPicking = true;
    g_exQtyWatchUntil = 0;
    exSpeakPicker(base, panel);
}

static void exConfirmOffer(uintptr_t base, uintptr_t panel) {
    g_exPicking = false;
    g_exQtyWatchUntil = 0;
    uint32_t src = 0, qty = 0;
    safeReadU32(panel + EX_SEL_SRC_OFF, &src);
    safeReadU32(panel + EX_QTY_OFF, &qty);
    char name[96], prefix[160];
    exTypeTitle(base, src, name, sizeof name);
    char offer[160];
    _snprintf(offer, sizeof offer, axs(AXS_EX_OFFERING_FMT), qty, name);
    offer[sizeof offer - 1] = 0;
    _snprintf(prefix, sizeof prefix, "%s ", offer);
    prefix[sizeof prefix - 1] = 0;
    g_exCol = 1;
    g_exRow = 0;
    logLine("exchange: offer confirmed at %u %s -> get column", qty, exCurrencyName(src));
    exSpeakRow(base, prefix, true);
}

static void exAdjustOffer(uintptr_t base, uintptr_t panel, bool increase) {
    uint32_t before = 0;
    safeReadU32(panel + EX_QTY_OFF, &before);
    if (!frontEndClickElementId((int64_t)(increase ? EX_ID_QTY_UP : EX_ID_QTY_DN))) {
        postSpeech(axs(increase ? AXS_EX_OFFER_MAX : AXS_EX_OFFER_MIN));
        return;
    }
    g_exQtyBefore = (int)before;                     // announce on the OBSERVED change
    g_exQtyWatchUntil = GetTickCount() + 800;
}

static void exTrade(uintptr_t base, uintptr_t panel) {
    if (g_exRow >= g_exRightN) return;
    ExRow* r = &g_exRight[g_exRow];
    if (g_exTradeWatchUntil) { postSpeech(axs(AXS_EX_TRADE_IN_PROGRESS)); return; }
    if (r->receive <= 0) {
        postSpeech(axs(AXS_EX_NOTHING_TO_RECEIVE));
        return;
    }
    uint32_t src = 0;
    safeReadU32(panel + EX_SEL_SRC_OFF, &src);
    int srcHeld = 0, tgtHeld = 0;
    resWalletAmount(base, src, &srcHeld);
    resWalletAmount(base, r->id, &tgtHeld);
    if (!frontEndClickElementId((int64_t)(EX_ID_CELL_BASE + (uint32_t)r->cellIdx))) {
        // The affordability gate lives in whether the game drew the element at all.
        postSpeech(axs(AXS_EX_TRADE_UNAVAILABLE));
        logLine("exchange: cell %d element missing (receive=%d cost=%d held=%d)",
                r->cellIdx, r->receive, r->cost, srcHeld);
        return;
    }
    g_exTradeSrcId = src;          g_exTradeTgtId = r->id;
    g_exTradeSrcBefore = srcHeld;  g_exTradeTgtBefore = tgtHeld;
    g_exTradeWatchUntil = GetTickCount() + 1500;
    logLine("exchange: trade clicked, cell=%d give %d %s for %d %s",
            r->cellIdx, r->cost, exCurrencyName(src), r->receive, exCurrencyName(r->id));
}

bool routeExchangeKey(uintptr_t base, uint32_t sym, uint16_t mod, uint8_t repeat) {  // non-static:
                                                            // the kAxContexts row (dllmain.cpp)
    if (mod & (KMOD_LALT | KMOD_RALT | KMOD_LCTRL | KMOD_RCTRL)) return false;
    int dRow = 0, dCol = 0, jump = 0;
    bool isJump = axDecodeJump(sym, mod, repeat, &jump);
    bool arrow = !isJump && axDecodeArrow(sym, mod, repeat, /*wantCols=*/true, &dRow, &dCol);
    bool enter = (sym == SDLK_RETURN || sym == SDLK_KP_ENTER);
    bool retired = (sym == SDLK_SPACE);
    if (!arrow && !enter && !retired && !isJump) return false;
    if (isJump && !jump) return true;              // held jump: one landing per press
    if (repeat && !dRow && !dCol) return true;
    uintptr_t panel = exPanel(base);
    if (!panel) return true;
    exCollect(base, panel);

    if (retired) {
        postSpeech(axs(g_exPicking ? AXS_EX_HELP_PICKING : AXS_EX_HELP_IDLE));
        return true;
    }

    if (g_exPicking) {
        if (isJump) {
            uint32_t src = 0, qty = 0;
            safeReadU32(panel + EX_SEL_SRC_OFF, &src);
            safeReadU32(panel + EX_QTY_OFF, &qty);
            char name[96], offer[160];
            exTypeTitle(base, src, name, sizeof name);
            _snprintf(offer, sizeof offer, axs(AXS_EX_OFFERING_FMT), qty, name);
            offer[sizeof offer - 1] = 0;
            postSpeech(offer);
            return true;
        }
        if (dRow) { exAdjustOffer(base, panel, /*increase=*/dRow < 0); return true; }
        if (enter) { exConfirmOffer(base, panel); return true; }
        if (dCol) {
            g_exPicking = false;
            logLine("exchange: left the picker sideways");
        }
    }

    if (isJump) dRow = jump;                          // idle row walk: the clamp lands first/last

    if (dCol) {
        int want = (dCol > 0) ? 1 : 0;
        bool moved = want != g_exCol;
        g_exCol = want;                               // hard stop at the outer columns
        int n = (g_exCol == 0) ? g_exLeftN : g_exRightN;
        axStepCursor(&g_exRow, n, 0);                 // keep the row index where possible
        logLine("exchange nav column -> %s%s", g_exCol ? "get" : "own",
                moved ? "" : " (already there)");
        exSpeakRow(base, nullptr, true);              // the header names the column just entered
        return true;
    }
    if (dRow) {
        int n = (g_exCol == 0) ? g_exLeftN : g_exRightN;
        if (n <= 0) { exSpeakRow(base, nullptr, true); return true; }
        axStepCursor(&g_exRow, n, dRow);              // hard stop: re-read the end row
        exSpeakRow(base, nullptr, false);
        return true;
    }
    if (enter) {
        if (g_exCol == 0) exSelectSource(base, panel);
        else              exTrade(base, panel);
        return true;
    }
    return true;
}

void checkExchange(uintptr_t base) {
    bool open = exIsOpen(base);
    if (open && !g_exActive) {
        g_exActive = true;
        g_exCol = 0;
        g_exRow = 0;
        g_exPicking = false;
        g_exPickHint = false;
        g_exQtyWatchUntil = 0;
        g_exTradeWatchUntil = 0;
        uintptr_t panel = exPanel(base);
        logLine("exchange: opened, panel=%p", (void*)panel);
        if (panel) {
            exCollect(base, panel);
            uint32_t src = 0;
            safeReadU32(panel + EX_SEL_SRC_OFF, &src);
            for (int i = 0; i < g_exLeftN; i++)
                if (g_exLeft[i].id == src) { g_exRow = i; break; }
            exSpeakRow(base, "Heirloom exchange. ", true);   // land ON something, in one utterance
        } else {
            postSpeech(axs(AXS_EX_TITLE));
        }
        return;
    }
    if (!open && g_exActive) {
        g_exActive = false;
        g_exPicking = false;
        g_exQtyWatchUntil = 0;
        g_exTradeWatchUntil = 0;
        logLine("exchange: closed");
        postSpeech(axs(AXS_EX_CLOSED));
        return;
    }
    if (!g_exActive) return;
    uintptr_t panel = exPanel(base);
    if (!panel) return;

    if (axDebugLogEnabled() && !g_exProbed) {        // diagnostic: log-gated, latch unspent when off
        uintptr_t root = resTownRoot(base);
        uint32_t shown = 0;
        if (root && safeReadU32(root + EX_OPEN_OFF, &shown) && shown == 4) {
            g_exProbed = true;
            exProbe(base, panel);
        }
    }

    // Offer watcher: speak the quantity the game actually landed on.
    if (g_exQtyWatchUntil) {
        uint32_t now = 0;
        safeReadU32(panel + EX_QTY_OFF, &now);
        if ((int)now != g_exQtyBefore) {
            g_exQtyWatchUntil = 0;
            uint32_t src = 0;
            safeReadU32(panel + EX_SEL_SRC_OFF, &src);
            char name[96], utter[192];
            exTypeTitle(base, src, name, sizeof name);
            _snprintf(utter, sizeof utter, axs(AXS_EX_OFFERING_FMT), now, name);
            utter[sizeof utter - 1] = 0;
            postSpeech(utter);
        } else if (GetTickCount() > g_exQtyWatchUntil) {
            g_exQtyWatchUntil = 0;
            logLine("exchange: offer click made no change (before=%d)", g_exQtyBefore);
            postSpeech(axs(AXS_EX_OFFER_UNCHANGED));
        }
    }

    // Trade watcher: the wallet moving is the only evidence the trade happened.
    if (g_exTradeWatchUntil) {
        int srcNow = 0, tgtNow = 0;
        resWalletAmount(base, g_exTradeSrcId, &srcNow);
        resWalletAmount(base, g_exTradeTgtId, &tgtNow);
        if (srcNow != g_exTradeSrcBefore || tgtNow != g_exTradeTgtBefore) {
            g_exTradeWatchUntil = 0;
            char srcName[96], tgtName[96], utter[MAILBOX_SZ];
            exTypeTitle(base, g_exTradeSrcId, srcName, sizeof srcName);
            exTypeTitle(base, g_exTradeTgtId, tgtName, sizeof tgtName);
            _snprintf(utter, sizeof utter, axs(AXS_EX_TRADED_FMT),
                      g_exTradeSrcBefore - srcNow, srcName, tgtNow - g_exTradeTgtBefore, tgtName,
                      srcNow, srcName, tgtNow, tgtName);
            utter[sizeof utter - 1] = 0;
            logLine("exchange: trade observed, %s %d->%d, %s %d->%d",
                    exCurrencyName(g_exTradeSrcId), g_exTradeSrcBefore, srcNow,
                    exCurrencyName(g_exTradeTgtId), g_exTradeTgtBefore, tgtNow);
            postSpeech(utter);
        } else if (GetTickCount() > g_exTradeWatchUntil) {
            g_exTradeWatchUntil = 0;
            logLine("exchange: trade click produced no wallet change");
            postSpeech(axs(AXS_EX_TRADE_DIDNT_HAPPEN));
        }
    }
}
