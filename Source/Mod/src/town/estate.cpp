// town/estate.cpp -- the first TOWN slice

#include <cstdint>
#include <cstring>
#include <cstdio>
#include "internal.h"

// ---- TOWN: the ACTIVITY LOG (the hamlet's first voiced surface) ----
static const uintptr_t TL_PANEL_OFF        = 0x17a8;
static const uintptr_t TL_OPEN_OFF         = 0x58;      // panel+: int, != 0 = shown (game's own;
                                                        // really a tween state: 1..3 in/up, 0 out)
static const uintptr_t TL_MAIN_OFF         = 0x80;      // panel+: "main_widget" layout ptr
static const uintptr_t TL_GOALS_OFF        = 0x88;      // panel+: the GOALS column root (its own
static const uintptr_t TL_COLUMN_OFF       = 0x98;      // panel+: "activity_log_column" layout ptr
static const char* const TL_WEEK_TITLE_KEY = "str_week";
static const char* const TL_GOAL_HEAD_KEYS[] = {
    "str_caretaker_goals_heading",                   // "Caretaker Goals"
    "str_caretaker_goals_roster_goals_heading",      // "Roster Goals"
    "str_caretaker_goals_quest_goals_heading",       // "Quest Goals"
};
static const int TL_GOAL_HEADS = 3;
static const int       TL_CLOSE_ACTIVITYLOG= 2;         // the ClosePanel enum for this panel
static const int       TL_MAX_ROWSN        = 96;        // cap on collected text rows
static const int       TL_WALK_DEPTH_MAX   = 10;        // tree depth cap

static bool g_tlActive = false;   // mirror of the open gate, for edge detection
static int  g_tlRow    = 0;       // row cursor

static uintptr_t tlPanel(uintptr_t base) {
    uintptr_t root = 0;
    if (!safeReadPtr(base + TL_ROOT_RVA, &root) || root <= 0x10000) return 0;
    uintptr_t panel = root + TL_PANEL_OFF;
    uintptr_t vft = 0;
    if (!safeReadPtr(panel, &vft) || vft != base + TL_VFT_RVA) return 0;
    return panel;
}

static bool tlIsOpen(uintptr_t base) {
    uintptr_t r = 0;
    if (safeReadPtr(base + MAP_ROOT_RVA, &r) && r > 0x10000) return false;   // in a raid
    if (rrDisplay(base) != 0) return false;                                  // results screen up
    uintptr_t panel = tlPanel(base);
    if (!panel) return false;
    uint32_t open = 0;
    if (!safeReadU32(panel + TL_OPEN_OFF, &open)) return false;
    return open != 0;
}
bool axIsTownLog() { return g_tlActive; }

bool tlLooksLikeWidget(uintptr_t base, uintptr_t p) {
    if (p <= 0x10000) return false;
    uintptr_t vft = 0;
    if (!safeReadPtr(p, &vft)) return false;
    return vft >= base + RDATA_BEGIN_RVA && vft <= base + RDATA_END_RVA;
}

void tlGather(uintptr_t base, uintptr_t node, int depth, int* budget,
              char frags[][TL_ROW_MAX], int* nfrags, bool probe) {
    if (!node || depth > TL_WALK_DEPTH_MAX || *budget <= 0 || *nfrags >= TL_FRAGS_MAX) return;
    (*budget)--;
    uintptr_t vft = 0;
    safeReadPtr(node, &vft);

    if (vft == base + TBW_VFTABLE_RVA) {
        char raw[512], clean[TL_ROW_MAX];
        if (readTbwText(node, raw, sizeof raw)) {
            stripMarkup(raw, clean, sizeof clean);
            if (clean[0]) {
                strncpy(frags[*nfrags], clean, TL_ROW_MAX - 1);
                frags[*nfrags][TL_ROW_MAX - 1] = 0;
                if (probe) logLine("townlog probe: %*stext tbw=%p \"%.200s\"",
                                   depth * 2, "", (void*)node, clean);
                (*nfrags)++;
            }
        }
        // fall through: a TBW can carry attachments too
    }

    // layout shape: a children vector of cells
    uintptr_t beg = 0, end = 0;
    bool asVector = false;
    if (safeReadPtr(node + TL_KIDS_BEG_OFF, &beg) && safeReadPtr(node + TL_KIDS_END_OFF, &end) &&
        end >= beg && ((end - beg) >> 3) <= (uintptr_t)TL_WALK_KIDS_MAX) {
        int kids = (int)((end - beg) >> 3);
        asVector = kids > 0;
        for (int i = 0; i < kids && asVector; i++) {
            uintptr_t kid = 0;
            if (!safeReadPtr(beg + (uintptr_t)i * 8, &kid) || !tlLooksLikeWidget(base, kid))
                asVector = false;
        }
        if (asVector) {
            if (probe) logLine("townlog probe: %*snode=%p vft=+0x%llx cells=%d",
                               depth * 2, "", (void*)node,
                               (unsigned long long)(vft - base), kids);
            for (int i = 0; i < kids; i++) {
                uintptr_t kid = 0;
                if (safeReadPtr(beg + (uintptr_t)i * 8, &kid))
                    tlGather(base, kid, depth + 1, budget, frags, nfrags, probe);
            }
        }
    }
    if (!asVector && vft != base + TBW_VFTABLE_RVA) {
        // cell shape: one content widget
        uintptr_t content = 0;
        if (safeReadPtr(node + TL_CELL_CONTENT_OFF, &content) && tlLooksLikeWidget(base, content)) {
            if (probe) logLine("townlog probe: %*scell=%p -> %p",
                               depth * 2, "", (void*)node, (void*)content);
            tlGather(base, content, depth + 1, budget, frags, nfrags, probe);
        }
    }

    // free attachments, on every widget shape
    uintptr_t ab = 0, ae = 0;
    if (safeReadPtr(node + TL_ATTACH_BEG_OFF, &ab) && safeReadPtr(node + TL_ATTACH_END_OFF, &ae) &&
        ae > ab && ((ae - ab) >> 3) <= (uintptr_t)TL_WALK_KIDS_MAX) {
        int n = (int)((ae - ab) >> 3);
        bool ok = true;
        for (int i = 0; i < n && ok; i++) {
            uintptr_t kid = 0;
            if (!safeReadPtr(ab + (uintptr_t)i * 8, &kid) || !tlLooksLikeWidget(base, kid))
                ok = false;
        }
        if (ok) {
            if (probe) logLine("townlog probe: %*snode=%p vft=+0x%llx attached=%d",
                               depth * 2, "", (void*)node,
                               (unsigned long long)(vft - base), n);
            for (int i = 0; i < n; i++) {
                uintptr_t kid = 0;
                if (safeReadPtr(ab + (uintptr_t)i * 8, &kid))
                    tlGather(base, kid, depth + 1, budget, frags, nfrags, probe);
            }
        }
    }
}

static bool tlHasText(uintptr_t base, uintptr_t node, int depth, int* budget) {
    if (!node || depth > TL_WALK_DEPTH_MAX || *budget <= 0) return false;
    (*budget)--;
    uintptr_t vft = 0;
    safeReadPtr(node, &vft);
    if (vft == base + TBW_VFTABLE_RVA) {
        char raw[64];
        if (readTbwText(node, raw, sizeof raw) && raw[0]) return true;
    }
    uintptr_t beg = 0, end = 0;
    if (safeReadPtr(node + TL_KIDS_BEG_OFF, &beg) && safeReadPtr(node + TL_KIDS_END_OFF, &end) &&
        end > beg && ((end - beg) >> 3) <= (uintptr_t)TL_WALK_KIDS_MAX) {
        int kids = (int)((end - beg) >> 3);
        bool shaped = true;
        for (int i = 0; i < kids && shaped; i++) {
            uintptr_t kid = 0;
            if (!safeReadPtr(beg + (uintptr_t)i * 8, &kid) || !tlLooksLikeWidget(base, kid))
                shaped = false;
        }
        if (shaped)
            for (int i = 0; i < kids; i++) {
                uintptr_t kid = 0;
                if (safeReadPtr(beg + (uintptr_t)i * 8, &kid) &&
                    tlHasText(base, kid, depth + 1, budget)) return true;
            }
        if (shaped) { /* vector shape handled */ }
        else {
            uintptr_t content = 0;
            if (safeReadPtr(node + TL_CELL_CONTENT_OFF, &content) &&
                tlLooksLikeWidget(base, content) &&
                tlHasText(base, content, depth + 1, budget)) return true;
        }
    } else {
        uintptr_t content = 0;
        if (safeReadPtr(node + TL_CELL_CONTENT_OFF, &content) &&
            tlLooksLikeWidget(base, content) &&
            tlHasText(base, content, depth + 1, budget)) return true;
    }
    uintptr_t ab = 0, ae = 0;
    if (safeReadPtr(node + TL_ATTACH_BEG_OFF, &ab) && safeReadPtr(node + TL_ATTACH_END_OFF, &ae) &&
        ae > ab && ((ae - ab) >> 3) <= (uintptr_t)TL_WALK_KIDS_MAX) {
        int n = (int)((ae - ab) >> 3);
        for (int i = 0; i < n; i++) {
            uintptr_t kid = 0;
            if (safeReadPtr(ab + (uintptr_t)i * 8, &kid) && tlLooksLikeWidget(base, kid) &&
                tlHasText(base, kid, depth + 1, budget)) return true;
        }
    }
    return false;
}

static uintptr_t tlFindListNode(uintptr_t base, uintptr_t root, bool probe) {
    uintptr_t node = root;
    for (int d = 0; d < 8 && node; d++) {
        uintptr_t vft = 0;
        safeReadPtr(node, &vft);
        if (vft == base + TBW_VFTABLE_RVA) return node;   // degenerate: a single text
        uintptr_t beg = 0, end = 0;
        bool shaped = false;
        int kids = 0;
        if (safeReadPtr(node + TL_KIDS_BEG_OFF, &beg) &&
            safeReadPtr(node + TL_KIDS_END_OFF, &end) &&
            end > beg && ((end - beg) >> 3) <= (uintptr_t)TL_WALK_KIDS_MAX) {
            kids = (int)((end - beg) >> 3);
            shaped = true;
            for (int i = 0; i < kids && shaped; i++) {
                uintptr_t kid = 0;
                if (!safeReadPtr(beg + (uintptr_t)i * 8, &kid) || !tlLooksLikeWidget(base, kid))
                    shaped = false;
            }
        }
        if (shaped) {
            int nonEmpty = 0;
            uintptr_t only = 0;
            for (int i = 0; i < kids; i++) {
                uintptr_t kid = 0;
                int budget = 128;
                if (safeReadPtr(beg + (uintptr_t)i * 8, &kid) &&
                    tlHasText(base, kid, 0, &budget)) { nonEmpty++; only = kid; }
            }
            if (probe) logLine("townlog probe: unwrap d%d node=%p cells=%d nonempty=%d",
                               d, (void*)node, kids, nonEmpty);
            if (nonEmpty > 1 || nonEmpty == 0) return node;   // the list (or nothing readable)
            node = only;                                      // one branch: unwrap it
            continue;
        }
        // cell shape: one content widget
        uintptr_t content = 0;
        if (safeReadPtr(node + TL_CELL_CONTENT_OFF, &content) && tlLooksLikeWidget(base, content)) {
            node = content;
            continue;
        }
        // a lone attachment
        uintptr_t ab = 0, ae = 0;
        if (safeReadPtr(node + TL_ATTACH_BEG_OFF, &ab) &&
            safeReadPtr(node + TL_ATTACH_END_OFF, &ae) && (ae - ab) == 8) {
            uintptr_t kid = 0;
            if (safeReadPtr(ab, &kid) && tlLooksLikeWidget(base, kid)) { node = kid; continue; }
        }
        return node;
    }
    return node;
}

static bool tlWeekFmt(uintptr_t base, char* pre, int presz, char* post, int postsz) {
    char raw[256], fmt[256];
    if (!resolveKey(base, TL_WEEK_TITLE_KEY, raw, sizeof raw)) return false;
    stripMarkup(raw, fmt, sizeof fmt);
    const char* d = strstr(fmt, "%d");
    if (!d) return false;
    int plen = (int)(d - fmt);
    if (plen >= presz) plen = presz - 1;
    memcpy(pre, fmt, plen); pre[plen] = 0;
    strncpy(post, d + 2, postsz - 1); post[postsz - 1] = 0;
    return true;
}

static int tlWeekOfText(const char* text, const char* pre, const char* post) {
    size_t np = strlen(pre), ns = strlen(post), nt = strlen(text);
    if (nt <= np + ns || _strnicmp(text, pre, np) != 0) return -1;
    if (ns && _stricmp(text + nt - ns, post) != 0) return -1;
    int week = 0, digits = 0;
    for (size_t i = np; i < nt - ns; i++) {
        if (text[i] < '0' || text[i] > '9') return -1;
        week = week * 10 + (text[i] - '0');
        digits++;
    }
    return digits ? week : -1;
}

static char g_tlRows[TL_MAX_ROWSN][TL_ROW_MAX];   // live-collect scratch, main thread only
static char g_tlRowLbl[TL_MAX_ROWSN][96];         // per row: its section label, the game's words
                                                  // (a "Week N" / "Caretaker Goals" heading)
static bool g_tlRowDone[TL_MAX_ROWSN];            // per goals row: is the goal COMPLETED?
static int  g_tlCol     = 0;                      // 0 = the log column, 1 = the goals column
static int  g_tlRowGoal = 0;                      // the goals column's own cursor

static bool tlGoalCompleted(uintptr_t base, uintptr_t listCell) {
    uintptr_t row = 0;
    if (!safeReadPtr(listCell + TL_CELL_CONTENT_OFF, &row) || !tlLooksLikeWidget(base, row))
        return false;
    uintptr_t beg = 0, end = 0;
    if (!safeReadPtr(row + TL_KIDS_BEG_OFF, &beg) || !safeReadPtr(row + TL_KIDS_END_OFF, &end) ||
        end <= beg)
        return false;
    uintptr_t firstCell = 0, checkbox = 0;
    if (!safeReadPtr(beg, &firstCell) || !tlLooksLikeWidget(base, firstCell)) return false;
    if (!safeReadPtr(firstCell + TL_CELL_CONTENT_OFF, &checkbox) ||
        !tlLooksLikeWidget(base, checkbox))
        return false;
    uintptr_t ab = 0, ae = 0;
    if (safeReadPtr(checkbox + TL_ATTACH_BEG_OFF, &ab) &&
        safeReadPtr(checkbox + TL_ATTACH_END_OFF, &ae) && ae > ab &&
        ((ae - ab) >> 3) <= (uintptr_t)TL_WALK_KIDS_MAX)
        return true;
    return false;
}

static int tlCollectColumn(uintptr_t base, uintptr_t panel, int whichCol, bool probe) {
    uintptr_t col = 0;
    uintptr_t rootOff = (whichCol == 0) ? TL_COLUMN_OFF : TL_GOALS_OFF;
    if (!safeReadPtr(panel + rootOff, &col) || !tlLooksLikeWidget(base, col)) {
        if (probe) logLine("townlog probe: no column widget at panel+0x%llx",
                           (unsigned long long)rootOff);
        return 0;
    }
    // section-label matchers for this column
    char pre[128], post[128];
    bool haveWeekFmt = false;
    char heads[TL_GOAL_HEADS][96];
    int nHeads = 0;
    if (whichCol == 0) {
        haveWeekFmt = tlWeekFmt(base, pre, sizeof pre, post, sizeof post);
        if (probe) logLine("townlog probe: week fmt %s (\"%s\" + N + \"%s\")",
                           haveWeekFmt ? "resolved" : "UNAVAILABLE",
                           haveWeekFmt ? pre : "", haveWeekFmt ? post : "");
    } else {
        for (int i = 0; i < TL_GOAL_HEADS; i++) {
            char raw[192];
            if (resolveKey(base, TL_GOAL_HEAD_KEYS[i], raw, sizeof raw)) {
                stripMarkup(raw, heads[nHeads], sizeof heads[nHeads]);
                if (heads[nHeads][0]) nHeads++;
            }
        }
        if (probe) logLine("townlog probe: goals column, %d headings resolved", nHeads);
    }

    uintptr_t list = tlFindListNode(base, col, probe);
    if (!list) return 0;
    uintptr_t beg = 0, end = 0;
    if (!safeReadPtr(list + TL_KIDS_BEG_OFF, &beg) || !safeReadPtr(list + TL_KIDS_END_OFF, &end) ||
        end < beg) return 0;
    int cells = (int)((end - beg) >> 3);
    if (cells > TL_WALK_KIDS_MAX) cells = TL_WALK_KIDS_MAX;

    static char frags[TL_FRAGS_MAX][TL_ROW_MAX];
    char lbl[96] = { 0 };
    int count = 0, budget = TL_WALK_NODES_MAX;
    for (int c = 0; c < cells && count < TL_MAX_ROWSN; c++) {
        uintptr_t cell = 0;
        if (!safeReadPtr(beg + (uintptr_t)c * 8, &cell) || !tlLooksLikeWidget(base, cell))
            continue;
        int nf = 0;
        if (probe) logLine("townlog probe: -- col %d cell %d --", whichCol, c);
        tlGather(base, cell, 1, &budget, frags, &nf, probe);
        if (nf == 0) continue;                        // separator / pure image cell

        // drop any fragment contained in a longer one of the same entry
        bool drop[TL_FRAGS_MAX] = { false };
        for (int i = 0; i < nf; i++)
            for (int j = 0; j < nf && !drop[i]; j++)
                if (i != j && !drop[j] && strlen(frags[i]) < strlen(frags[j]) &&
                    strstr(frags[j], frags[i]) != nullptr)
                    drop[i] = true;

        char* row = g_tlRows[count];
        int o = 0;
        for (int i = 0; i < nf; i++) {
            if (drop[i]) continue;
            int len = (int)strlen(frags[i]);
            if (o > 0) {
                if (o + 2 >= TL_ROW_MAX - 1) break;
                bool punct = o > 0 && (row[o-1] == '.' || row[o-1] == '!' || row[o-1] == '?' ||
                                       row[o-1] == ':');
                row[o++] = punct ? ' ' : '.';
                if (!punct) { if (o < TL_ROW_MAX - 1) row[o++] = ' '; }
            }
            for (int k = 0; k < len && o < TL_ROW_MAX - 1; k++) row[o++] = frags[i][k];
        }
        row[o] = 0;
        if (!row[0]) continue;

        // a header cell becomes the section label, not a row
        bool isHeader = false;
        if (whichCol == 0 && haveWeekFmt && tlWeekOfText(row, pre, post) >= 0) isHeader = true;
        if (whichCol == 1)
            for (int i = 0; i < nHeads && !isHeader; i++)
                if (_stricmp(row, heads[i]) == 0) isHeader = true;
        if (isHeader) {
            strncpy(lbl, row, sizeof lbl - 1);
            lbl[sizeof lbl - 1] = 0;
            if (probe) logLine("townlog probe: col %d cell %d = header \"%s\"", whichCol, c, lbl);
            continue;
        }
        strncpy(g_tlRowLbl[count], lbl, sizeof g_tlRowLbl[count] - 1);
        g_tlRowLbl[count][sizeof g_tlRowLbl[count] - 1] = 0;
        g_tlRowDone[count] = (whichCol == 1) && tlGoalCompleted(base, cell);
        if (probe) logLine("townlog probe: col %d row[%d] lbl=\"%s\" done=%d \"%.200s\"",
                           whichCol, count, lbl, g_tlRowDone[count] ? 1 : 0, row);
        count++;
    }
    return count;
}

static void tlProbe(uintptr_t base, uintptr_t panel) {
    uintptr_t ctrl = 0, root = 0;
    safeReadPtr(base + TL_ROOT_RVA, &root);
    safeReadPtr(base + TL_CTRL_RVA, &ctrl);
    uint32_t c10 = 0, c0c = 0;
    if (ctrl > 0x10000) { safeReadU32(ctrl + 0x10, &c10); safeReadU32(ctrl + 0x0c, &c0c); }
    logLine("townlog probe: root=%p panel=%p ctrl=%p ctrl+0x0c=%d ctrl+0x10=%d",
            (void*)root, (void*)panel, (void*)ctrl, (int)c0c, (int)c10);
    int n = tlCollectColumn(base, panel, 0, true);
    logLine("townlog probe: log column, %d rows", n);
    n = tlCollectColumn(base, panel, 1, true);
    logLine("townlog probe: goals column, %d rows", n);
}

// The active column's cursor.
static int* tlCursor() { return g_tlCol == 0 ? &g_tlRow : &g_tlRowGoal; }

static void tlTitle(uintptr_t base, char* out, int outsz) {
    if (resolveKey(base, "town_name_activity_log", out, outsz) && out[0]) return;
    strncpy(out, axs(AXS_TL_TITLE_FALLBACK), outsz - 1);
    out[outsz - 1] = 0;
}

static void tlSpeakRow(uintptr_t base, bool sayTitle, bool sayLbl) {
    uintptr_t panel = tlPanel(base);
    if (!panel) return;
    int n = tlCollectColumn(base, panel, g_tlCol, false);
    char pfx[112] = { 0 };
    if (sayTitle) {
        char title[96];
        tlTitle(base, title, sizeof title);
        _snprintf(pfx, sizeof pfx, "%s. ", title);
        pfx[sizeof pfx - 1] = 0;
    }
    char utter[MAILBOX_SZ];
    if (n == 0) {
        _snprintf(utter, sizeof utter, "%s%s", pfx,
                  g_tlCol == 0 ? axs(AXS_TL_EMPTY) : axs(AXS_TL_NO_GOALS));
        utter[sizeof utter - 1] = 0;
        postSpeech(utter);
        return;
    }
    int* cur = tlCursor();
    if (*cur >= n) *cur = n - 1;
    if (*cur < 0)  *cur = 0;
    char lbl[112] = { 0 };
    if (sayLbl && g_tlRowLbl[*cur][0])
        _snprintf(lbl, sizeof lbl, "%s. ", g_tlRowLbl[*cur]);
    const char* row = g_tlRows[*cur];
    size_t rl = strlen(row);
    char last = rl ? row[rl - 1] : 0;
    bool punct = last == '.' || last == '!' || last == '?' || last == ':';
    char done[64] = { 0 };
    if (g_tlRowDone[*cur])
        _snprintf(done, sizeof done, " %s", axs(AXS_TL_GOAL_DONE));
    char pos[48];
    pos[0] = ' ';
    _snprintf(pos + 1, sizeof pos - 1, axs(AXS_POS_N_OF_M), *cur + 1, n);
    pos[sizeof pos - 1] = 0;
    _snprintf(utter, sizeof utter, "%s%s%s%s%s%s",
              pfx, lbl, row, punct ? "" : ".", done, pos);
    utter[sizeof utter - 1] = 0;
    postSpeech(utter);
}

void tlReannounce(uintptr_t base) {
    if (!g_tlActive) return;
    logLine("townlog: re-announcing after a modal closed");
    tlSpeakRow(base, true, true);
}

static bool     g_tlAnnounced  = false;   // the entry line was spoken for this open
static uint32_t g_tlOpenedTick = 0;       // when the open edge was seen (for the build-wait)

void checkTownLog(uintptr_t base) {
    bool open = tlIsOpen(base);
    if (open && !g_tlActive) {
        g_tlActive = true;
        g_tlAnnounced = false;
        g_tlRow = 0;
        g_tlRowGoal = 0;
        g_tlCol = 0;                  // always land in the log column
        g_tlOpenedTick = GetTickCount();
        uintptr_t panel = tlPanel(base);
        logLine("townlog: opened, panel=%p", (void*)panel);
        if (panel && axDebugLogEnabled()) tlProbe(base, panel);   // diagnostic: log-gated
    } else if (!open && g_tlActive) {
        g_tlActive = false;
        logLine("townlog: closed");
        char title[96], utter[160];
        tlTitle(base, title, sizeof title);
        _snprintf(utter, sizeof utter, axs(AXS_TL_CLOSED_FMT), title);
        utter[sizeof utter - 1] = 0;
        postSpeech(utter);
        return;
    }
    if (g_tlActive && !g_tlAnnounced) {
        uintptr_t panel = tlPanel(base);
        if (!panel) return;
        int n = tlCollectColumn(base, panel, 0, false);
        if (n == 0 && GetTickCount() - g_tlOpenedTick < 2000) return;   // build still running
        g_tlAnnounced = true;
        logLine("townlog: announcing entry, %d rows", n);
        tlSpeakRow(base, true, true);
    }
}

bool routeTownLogToggle(uintptr_t base, uint32_t sym, uint8_t repeat) {
    if (sym != SDLK_PERIOD) return false;
    if (repeat) return true;                   // a held key must not flap the panel
    uintptr_t root = 0;
    if (!safeReadPtr(base + TL_ROOT_RVA, &root) || root <= 0x10000 || !tlPanel(base))
        return false;                          // not in town: the key stays the game's
    typedef void (__fastcall *TlShowFn)(uintptr_t townRoot);
    typedef void (__fastcall *TlCloseFn)(uintptr_t townRoot, int which);
    if (tlIsOpen(base)) {
        logLine("townlog: period -> ClosePanel(activity log)");
        reinterpret_cast<TlCloseFn>(base + TL_CLOSE_RVA)(root, TL_CLOSE_ACTIVITYLOG);
    } else {
        uint32_t f1 = 0, f2 = 0, f3 = 0, f4 = 0, f5 = 0;
        uintptr_t heapPanel = 0;
        safeReadU32(root + 0x46c8, &f1);
        safeReadU32(root + 0x2a58, &f2);
        safeReadU32(root + 0x4840, &f3);
        safeReadU32(root + 0x4968, &f4);
        if (safeReadPtr(base + GL_SINGLETON_RVA, &heapPanel) && heapPanel > 0x10000)
            safeReadU32(heapPanel + TL_OPEN_OFF, &f5);
        if (f1 || f2 || f3 || f4 || f5) {
            logLine("townlog: period refused, another town panel is open (%u %u %u %u %u)",
                    f1, f2, f3, f4, f5);
            return false;
        }
        logLine("townlog: period -> ShowActivityLog");
        reinterpret_cast<TlShowFn>(base + TL_SHOW_RVA)(root);
    }
    return true;
}

static void tlKeyGateCheck(uintptr_t base) {
    static bool done = false;
    if (done) return;
    done = true;
    char owner[0x44];
    if (kbActionForKey(base, (int)SDLK_UP, owner, sizeof owner))
        logLine("townlog GATE: Up is bound to the game action \"%s\"", owner);
    else
        logLine("townlog GATE: Up is not in the keyboard binding table");
    if (kbActionForKey(base, (int)SDLK_DOWN, owner, sizeof owner))
        logLine("townlog GATE: Down is bound to the game action \"%s\"", owner);
    else
        logLine("townlog GATE: Down is not in the keyboard binding table");
}

bool routeTownLogKey(uintptr_t base, uint32_t sym, uint16_t mod, uint8_t repeat) {
    int dRow = 0, dCol = 0, jump = 0;
    if (axDecodeJump(sym, mod, repeat, &jump)) {   // Home/End: first/last row, column KEPT
        if (!jump) return true;                    // held jump: one landing per press
        dRow = jump;
    } else if (!axDecodeArrow(sym, mod, repeat, /*wantCols=*/true, &dRow, &dCol)) return false;
    if (!dRow && !dCol) return true;         // throttled repeat: claimed, no step this tick
    tlKeyGateCheck(base);
    uintptr_t panel = tlPanel(base);
    if (!panel) return true;

    if (dCol) {
        int want = (dCol > 0) ? 1 : 0;
        bool moved = want != g_tlCol;
        g_tlCol = want;                       // hard stop at the outer columns: re-announce
        logLine("townlog nav column -> %s%s", g_tlCol == 0 ? "log" : "goals",
                moved ? "" : " (already there)");
        tlSpeakRow(base, g_tlCol == 0, true);
        return true;
    }

    int n = tlCollectColumn(base, panel, g_tlCol, false);
    if (n == 0) {
        postSpeech(g_tlCol == 0 ? axs(AXS_TL_EMPTY) : axs(AXS_TL_NO_GOALS));
        return true;
    }
    int* cur = tlCursor();
    axStepCursor(cur, n, 0);                            // re-clamp: the live column can shrink
    int from = *cur;
    axStepCursor(cur, n, dRow);                         // hard stop: re-read the end row
    bool crossed = strcmp(g_tlRowLbl[*cur], g_tlRowLbl[from]) != 0;
    logLine("townlog nav col %d row %d -> %d (of %d)%s", g_tlCol, from, *cur, n,
            crossed ? " [section]" : "");
    tlSpeakRow(base, false, crossed);
    return true;
}

// ---- TOWN: the RESOURCES BAR (TownUI::Panel::EstateSummary) ----
static const uintptr_t RES_WALLET_OFF   = 0xe70;     // campaign+: vector<CurrencyAmount> begin (end +8)
static const uintptr_t RES_WALLET_STRIDE= 0x48;      // amount int@+0x00, id-hash int@+0x44
static const int RES_MAX_ROWS = 8;                   // gold + 4 heirlooms + flagged DLC rows
static const uintptr_t RES_DEF_ID_OFF  = 0x40;       // CurrencyDef+: id hash (== wallet entry +0x44)
static const uintptr_t RES_DEF_BAR_OFF = 0x45;       // CurrencyDef+: char, drawn on the estate bar
static const int RES_ROW_MAX  = 160;

bool g_resActive = false;                            // mod-owned, set by R, cleared on leaving town
bool g_ptyActive = false;
bool g_rosActive = false;
static int  g_resRow    = 0;                         // row cursor
static char g_resRows[RES_MAX_ROWS][RES_ROW_MAX];    // the rendered rows, rebuilt live on every read
static bool g_resIsTrade[RES_MAX_ROWS];              // this row is the Trade Heirlooms action
static int  g_resCount  = 0;
static bool g_resDumped  = false;

bool axIsResources() { return g_resActive; }

void exOpen(uintptr_t base);

bool tmbBareMap(uintptr_t root);

uintptr_t bldOpenPanel(uintptr_t root);

uintptr_t resTownRoot(uintptr_t base) {
    uintptr_t r = 0;
    if (safeReadPtr(base + MAP_ROOT_RVA, &r) && r > 0x10000) return 0;   // in a raid
    if (rrDisplay(base) != 0) return 0;                                  // results screen up
    uintptr_t root = 0;
    if (!safeReadPtr(base + TL_ROOT_RVA, &root) || root <= 0x10000) return 0;
    uintptr_t vft = 0;                                                   // TownDisplay identity
    if (!safeReadPtr(root + TL_PANEL_OFF, &vft) || vft != base + TL_VFT_RVA) return 0;
    return root;
}

static const ResCurrency kResCurrencies[] = {
    { "gold",     "str_inventory_title_gold",            AXS_GOLD },
    { "bust",     "str_inventory_title_heirloombust",    AXS__COUNT },
    { "portrait", "str_inventory_title_heirloomportrait", AXS__COUNT },
    { "deed",     "str_inventory_title_heirloomdeed",    AXS__COUNT },
    { "crest",    "str_inventory_title_heirloomcrest",   AXS__COUNT },
};
static const ResCurrency kOffBarCurrencies[] = {
    { "blueprint", "str_inventory_title_heirloomblueprint", AXS__COUNT },  // districts, 13 languages
    { "shard",     "str_inventory_title_shard", AXS_SHARD_FALLBACK },      // CoM, 13 languages
    { "urn",       "str_inventory_title_heirloomurn", AXS__COUNT },        // a type, never on the bar
    { "memory",    "str_inventory_title_heirloommemory", AXS__COUNT },     // CoM, a district cost
};
static const int kOffBarCurrencyCount = (int)(sizeof kOffBarCurrencies / sizeof kOffBarCurrencies[0]);
static const int kResCurrencyCount = (int)(sizeof kResCurrencies / sizeof kResCurrencies[0]);

uint32_t resHash(const char* s) {
    uint32_t h = 0;
    for (; *s; ++s) h = h * 0x35u + (uint8_t)(*s);
    return h;
}

const ResCurrency* resCurrencyFind(uint32_t idHash) {
    for (int i = 0; i < kResCurrencyCount; i++)
        if (resHash(kResCurrencies[i].id) == idHash) return &kResCurrencies[i];
    for (int i = 0; i < kOffBarCurrencyCount; i++)
        if (resHash(kOffBarCurrencies[i].id) == idHash) return &kOffBarCurrencies[i];
    return nullptr;
}

void resCurrencyTitle(uintptr_t base, uint32_t idHash, const char* who, char* out, int outsz) {
    out[0] = 0;
    const ResCurrency* c = resCurrencyFind(idHash);
    if (c) {
        char raw[128];
        if (resolveKey(base, c->titleKey, raw, sizeof raw) && raw[0]) {
            stripMarkup(raw, out, outsz);
            if (out[0]) return;
        }
        if (c->spoken != AXS__COUNT) _snprintf(out, outsz, "%s", axs((AxStrId)c->spoken));
        else                         _snprintf(out, outsz, "%s", c->id);
        out[outsz - 1] = 0;
        logLine("%s: currency \"%s\" has no %s string -- fell back to \"%s\"",
                who, c->id, c->titleKey, out);
        return;
    }
    _snprintf(out, outsz, "%u", idHash);
    out[outsz - 1] = 0;
    logLine("%s: UNKNOWN currency hash %u -- add it to kOffBarCurrencies", who, idHash);
}

void resCurrencyTitleById(uintptr_t base, const char* id, const char* who, char* out, int outsz) {
    out[0] = 0;
    if (!id || !id[0]) return;
    resCurrencyTitle(base, resHash(id), who, out, outsz);
}

bool resWalletAmount(uintptr_t base, uint32_t idHash, int* amount) {
    *amount = 0;
    uintptr_t campaign = 0;
    if (!safeReadPtr(base + RES_CAMPAIGN_RVA, &campaign) || campaign <= 0x10000) return false;
    uintptr_t beg = 0, end = 0;
    if (!safeReadPtr(campaign + RES_WALLET_OFF, &beg) ||
        !safeReadPtr(campaign + RES_WALLET_OFF + 8, &end) || end < beg) return false;
    uintptr_t span = end - beg;
    if (span % RES_WALLET_STRIDE) return false;
    int n = (int)(span / RES_WALLET_STRIDE);
    if (n < 0 || n > 64) return false;
    for (int i = 0; i < n; i++) {
        uintptr_t e = beg + (uintptr_t)i * RES_WALLET_STRIDE;
        uint32_t wid = 0;
        if (safeReadU32(e + 0x44, &wid) && wid == idHash) {
            uint32_t amt = 0;
            safeReadU32(e + 0x00, &amt);
            *amount = (int)amt;
            return true;
        }
    }
    return false;
}

static int resCollect(uintptr_t base, bool probe) {
    g_resCount = 0;
    for (int c = 0; c < kResCurrencyCount && g_resCount < RES_MAX_ROWS - 1; c++) {
        uint32_t idHash = resHash(kResCurrencies[c].id);
        int amount = 0;
        bool found = resWalletAmount(base, idHash, &amount);   // absent -> 0, still shown

        char disp[96] = {0};
        resCurrencyTitle(base, idHash, "resources", disp, sizeof disp);

        _snprintf(g_resRows[g_resCount], RES_ROW_MAX, axs(AXS_RES_ROW_AMOUNT), disp, amount);
        g_resRows[g_resCount][RES_ROW_MAX - 1] = 0;
        g_resIsTrade[g_resCount] = false;
        if (probe) logLine("resources probe: %s id-hash=%u found=%d amount=%d -> \"%s\"",
                           kResCurrencies[c].id, idHash, found ? 1 : 0, amount, g_resRows[g_resCount]);
        g_resCount++;
    }
    {
        uintptr_t block = 0, beg = 0, end = 0;
        if (safeReadPtr(base + EX_REG_RVA, &block) && block > 0x10000 &&
            safeReadPtr(block, &beg) && safeReadPtr(block + 8, &end) && end >= beg) {
            int n = (int)((end - beg) / 8);
            if (n > 0 && n <= 32) {
                for (int i = 0; i < n && g_resCount < RES_MAX_ROWS - 1; i++) {
                    uintptr_t def = 0;
                    if (!safeReadPtr(beg + (uintptr_t)i * 8, &def) || def <= 0x10000) continue;
                    uint32_t id = 0;
                    uint8_t  onBar = 0;
                    if (!safeReadU32(def + RES_DEF_ID_OFF, &id)) continue;
                    safeReadU8(def + RES_DEF_BAR_OFF, &onBar);
                    if (probe) logLine("resources probe: registry def %d id-hash=%u bar-flag=%d",
                                       i, id, (int)onBar);
                    if (!onBar) continue;
                    bool known = false;                     // already one of the fixed five?
                    for (int c = 0; c < kResCurrencyCount && !known; c++)
                        known = resHash(kResCurrencies[c].id) == id;
                    if (known) continue;
                    int amount = 0;
                    resWalletAmount(base, id, &amount);     // absent -> 0, still shown
                    char disp[96];
                    resCurrencyTitle(base, id, "resources", disp, sizeof disp);
                    _snprintf(g_resRows[g_resCount], RES_ROW_MAX, axs(AXS_RES_ROW_AMOUNT),
                              disp, amount);
                    g_resRows[g_resCount][RES_ROW_MAX - 1] = 0;
                    g_resIsTrade[g_resCount] = false;
                    if (probe) logLine("resources probe: registry row \"%s\" (id-hash=%u)",
                                       g_resRows[g_resCount], id);
                    g_resCount++;
                }
            }
        }
    }
    char trade[96];
    if (!resolveKey(base, "town_name_heirloom_exchange", trade, sizeof trade)) {
        strncpy(trade, axs(AXS_RES_TRADE_FALLBACK), sizeof trade - 1);
        trade[sizeof trade - 1] = 0;
    }
    strncpy(g_resRows[g_resCount], trade, RES_ROW_MAX - 1);
    g_resRows[g_resCount][RES_ROW_MAX - 1] = 0;
    g_resIsTrade[g_resCount] = true;
    g_resCount++;
    return g_resCount;
}

static void resSpeakRow(uintptr_t base, const char* prefix) {
    int n = resCollect(base, false);
    if (n <= 0) { postSpeech(axs(AXS_RES_NONE)); return; }
    if (g_resRow >= n) g_resRow = n - 1;
    if (g_resRow < 0)  g_resRow = 0;
    char pos[48];
    pos[0] = ' ';
    _snprintf(pos + 1, sizeof pos - 1, axs(AXS_POS_N_OF_M), g_resRow + 1, n);
    pos[sizeof pos - 1] = 0;
    char utter[MAILBOX_SZ];
    _snprintf(utter, sizeof utter, "%s%s%s.%s", prefix ? prefix : "", prefix ? " " : "",
              g_resRows[g_resRow], pos);
    utter[sizeof utter - 1] = 0;
    postSpeech(utter);
}

void resReannounce(uintptr_t base) {
    if (!g_resActive) return;
    logLine("resources: re-announcing after a modal closed");
    resSpeakRow(base, axs(AXS_RES_TITLE));
}

void checkResources(uintptr_t base) {
    if (g_resActive && !resTownRoot(base)) {
        g_resActive = false;
        logLine("resources: town gone, stood down");
    }
}

bool routeResourcesToggle(uintptr_t base, uint32_t sym, uint8_t repeat) {
    if (sym != SDLK_r) return false;
    if (repeat) return true;                     // held key must not flap the context
    if (!resTownRoot(base)) return false;        // not in town: R stays the game's
    if (!g_resActive) {
        g_resActive = true;
        g_ptyActive = false;                     // one town layer at a time (P re-enters the party)
        g_rosActive = false;
        g_resRow = 0;                            // a fresh entry lands on the first row
        int n = resCollect(base, !g_resDumped);  // log each currency lookup once per session
        g_resDumped = true;
        logLine("resources: active, %d rows", n);
    }
    resSpeakRow(base, axs(AXS_RES_TITLE));       // a re-orient keeps the row and re-reads it
    return true;
}

bool routeResourcesKey(uintptr_t base, uint32_t sym, uint16_t mod, uint8_t repeat) {
    if (mod & (KMOD_LALT | KMOD_RALT | KMOD_LCTRL | KMOD_RCTRL)) return false;

    if (sym == SDLK_ESCAPE) {
        uintptr_t root = resTownRoot(base);
        if (!root || (!tmbBareMap(root) && !bldOpenPanel(root) &&
                      !embQsOpen(root) && !embProvIsOpen(root))) return false;
        if (repeat) return true;
        g_resActive = false;
        logLine("resources: Escape -> stand down to the surface underneath");
        return true;
    }

    if (sym == SDLK_RETURN || sym == SDLK_KP_ENTER) {
        if (repeat) return true;
        int n = resCollect(base, false);
        if (n <= 0) return true;
        if (g_resRow < 0)  g_resRow = 0;
        if (g_resRow >= n) g_resRow = n - 1;
        if (g_resIsTrade[g_resRow]) {
            logLine("resources: Enter on Trade Heirlooms -> open the exchange");
            exOpen(base);
        } else {
            resSpeakRow(base, nullptr);          // re-read; value rows have no action
        }
        return true;
    }

    int dRow = 0, dCol = 0, jump = 0;
    if (axDecodeJump(sym, mod, repeat, &jump)) {  // Home/End: gold / the Trade action
        if (!jump) return true;                   // held jump: one landing per press
        dRow = jump;
    } else if (!axDecodeArrow(sym, mod, repeat, /*wantCols=*/true, &dRow, &dCol)) return false;
    if (!dRow && !dCol) return true;             // throttled repeat: claimed, no step this tick
    int n = resCollect(base, false);
    if (n <= 0) { postSpeech(axs(AXS_RES_NONE)); return true; }
    axStepCursor(&g_resRow, n, 0);               // re-clamp: the live count can change
    axStepCursor(&g_resRow, n, dRow + dCol);     // hard stop: re-read the end row
    resSpeakRow(base, nullptr);
    return true;
}

// ---- TOWN: the ESTATE-WIDE INFESTATION level (Crimson Court's hamlet banner) ----

static char s_infWhy[200];
static const char* const kInfElemIds[] = {          // the 16 shipped sequence elements, so the
    "infestation_1_0", "infestation_1_1", "infestation_1_2", "infestation_1_3", "infestation_1_4",
    "infestation_2_0", "infestation_2_1", "infestation_2_2", "infestation_2_3", "infestation_2_4",
    "infestation_3_0", "infestation_3_1", "infestation_3_2", "infestation_3_3", "infestation_3_4",
    "infestation_4" };                              // dump can NAME a hash it sees
static const char* infElemName(uint32_t h) {
    for (int i = 0; i < (int)(sizeof kInfElemIds / sizeof kInfElemIds[0]); i++)
        if (resHash(kInfElemIds[i]) == h) return kInfElemIds[i];
    return "?";
}

int infEstateLevel(uintptr_t base, bool* shown, char* name, int nameSz) {
    if (shown) *shown = false;
    if (name && nameSz > 0) name[0] = 0;
    s_infWhy[0] = 0;
    uintptr_t sys = 0;
    if (!safeReadPtr(base + INF_SYS_RVA, &sys) || sys <= 0x10000) {
        _snprintf(s_infWhy, sizeof s_infWhy, "system global unreadable (sys=%p)", (void*)sys); return -1;
    }
    uint8_t gate = 0;
    if (!safeReadU8(sys + INF_GATE_OFF, &gate) || !gate) {
        _snprintf(s_infWhy, sizeof s_infWhy, "gate byte sys+0x30 = %u (sys=%p)", gate, (void*)sys); return -1;
    }
    uintptr_t camp = 0;
    if (!safeReadPtr(base + RES_CAMPAIGN_RVA, &camp) || camp <= 0x10000) {
        _snprintf(s_infWhy, sizeof s_infWhy, "campaign global unreadable (camp=%p)", (void*)camp); return -1;
    }
    uint32_t cur = 0;
    if (!safeReadU32(camp + INF_CAMP_CUR_OFF, &cur)) {
        _snprintf(s_infWhy, sizeof s_infWhy, "camp+0x17d4 unreadable (camp=%p)", (void*)camp); return -1;
    }

    uintptr_t eb = 0, ee = 0;
    if (!safeReadPtr(sys + INF_ELEMS_OFF, &eb) || !safeReadPtr(sys + INF_ELEMS_OFF + 8, &ee) ||
        eb <= 0x10000 || ee < eb) {
        _snprintf(s_infWhy, sizeof s_infWhy, "element vector unreadable (beg=%p end=%p)", (void*)eb, (void*)ee); return -1;
    }
    int en = (int)((ee - eb) / INF_ELEM_STRIDE);
    if (en <= 0 || en > 64) {                        // the json ships 16 elements; 64 = corruption stop
        _snprintf(s_infWhy, sizeof s_infWhy, "element count %d (span 0x%llx)", en, (unsigned long long)(ee - eb)); return -1;
    }
    uintptr_t elem = 0;
    for (int i = 0; i < en; i++) {
        uint32_t id = 0;
        if (safeReadU32(eb + (uintptr_t)i * INF_ELEM_STRIDE + 0x40, &id) && id == cur)
            elem = eb + (uintptr_t)i * INF_ELEM_STRIDE;
    }
    if (!elem) {
        uint32_t id0 = 0, idN = 0;
        safeReadU32(eb + 0x40, &id0);
        safeReadU32(eb + (uintptr_t)(en - 1) * INF_ELEM_STRIDE + 0x40, &idN);
        _snprintf(s_infWhy, sizeof s_infWhy,
                  "no element matches camp+0x17d4 = %u (%s); %d elements, [0]+0x40 = %u (%s), [%d]+0x40 = %u (%s)",
                  cur, infElemName(cur), en, id0, infElemName(id0), en - 1, idN, infElemName(idN));
        return -1;
    }
    uint8_t sh = 0;
    safeReadU8(elem + 0x5c, &sh);
    if (shown) *shown = sh != 0;
    uint32_t lvlId = 0;
    if (!safeReadU32(elem + 0x50, &lvlId)) {
        _snprintf(s_infWhy, sizeof s_infWhy, "elem+0x50 unreadable (elem=%p)", (void*)elem); return -1;
    }

    uintptr_t lb = 0, le = 0;
    if (!safeReadPtr(sys + INF_LVLS_OFF, &lb) || !safeReadPtr(sys + INF_LVLS_OFF + 8, &le) ||
        lb <= 0x10000 || le < lb) {
        _snprintf(s_infWhy, sizeof s_infWhy, "level-def vector unreadable (beg=%p end=%p)", (void*)lb, (void*)le); return -1;
    }
    int ln = (int)((le - lb) / INF_LVL_STRIDE);
    if (ln <= 0 || ln > 16) {                        // the json ships 4 levels
        _snprintf(s_infWhy, sizeof s_infWhy, "level-def count %d (span 0x%llx)", ln, (unsigned long long)(le - lb)); return -1;
    }
    int ordinal = -1;
    for (int i = 0; i < ln; i++) {
        uint32_t id = 0;
        if (safeReadU32(lb + (uintptr_t)i * INF_LVL_STRIDE + 0x40, &id) && id == lvlId) {
            ordinal = i;
            if (name) {
                uintptr_t def = lb + (uintptr_t)i * INF_LVL_STRIDE;
                if (!safeReadCStr(def, name, nameSz) || !name[0] || strlen(name) > 15 ||
                    resHash(name) != id) {
                    _snprintf(s_infWhy, sizeof s_infWhy,
                              "level def %d matched but def+0x00 \"%.16s\" does not hash to %u",
                              i, name, id);
                    name[0] = 0;
                }
            }
        }
    }
    if (ordinal < 0) {
        uint32_t id0 = 0;
        safeReadU32(lb + 0x40, &id0);
        _snprintf(s_infWhy, sizeof s_infWhy,
                  "no level def matches elem+0x50 = %u (%s shown=%u); %d defs, [0]+0x40 = %u",
                  lvlId, infElemName(cur), sh, ln, id0);
    } else if (name && !name[0]) {
    } else {
        _snprintf(s_infWhy, sizeof s_infWhy, "element %s level %d \"%s\" shown=%u",
                  infElemName(cur), ordinal, name ? name : "", sh);
    }
    s_infWhy[sizeof s_infWhy - 1] = 0;
    return ordinal;
}

void infEstateText(uintptr_t base, char* out, int outsz) {
    out[0] = 0;
    bool shown = false;
    char name[64];
    int lv = infEstateLevel(base, &shown, name, sizeof name);
    if (lv < 0 || !shown || !name[0]) {
        logLine("infestation: banner phrase empty -- %s", s_infWhy[0] ? s_infWhy : "(no reason recorded)");
        return;
    }
    char key[96];
    _snprintf(key, sizeof key, "infestation_level_description_%s", name);
    key[sizeof key - 1] = 0;
    char txt[224];
    if (!resolveKey(base, key, txt, sizeof txt) || !txt[0]) {   // no key, no phrase — no guess
        logLine("infestation: banner phrase empty -- key \"%s\" did not resolve (%s)", key, s_infWhy);
        return;
    }
    logLine("infestation: banner phrase -- %s", s_infWhy);
    abStripMarkup(txt);
    _snprintf(out, outsz, "%s", txt);
    out[outsz - 1] = 0;
}

static void infLevelWord(uintptr_t base, const char* name, char* out, int outsz) {
    out[0] = 0;
    char key[96];
    _snprintf(key, sizeof key, "infestation_building_contagion_%s", name);
    key[sizeof key - 1] = 0;
    char word[96];
    if (!resolveKey(base, key, word, sizeof word) || !word[0]) return;
    abStripMarkup(word);
    char* w = word;
    while (*w == ':') w++;                           // "::High::" -> "High"
    size_t wl = strlen(w);
    while (wl && w[wl - 1] == ':') w[--wl] = 0;
    _snprintf(out, outsz, "%s", w);
    out[outsz - 1] = 0;
}

static int       g_infLastLvl  = -2;
static uintptr_t g_infLastCamp = 0;
static uint32_t  g_infNextAt   = 0;
static uintptr_t g_infLoggedRoot = 0;                // the town visit the sample line was logged for
void serviceInfestation(uintptr_t base) {
    if (frontEndDisplay(base)) {
        if (g_infLastLvl != -2) logLine("infestation: front end up, latch cleared");
        g_infLastLvl = -2;
        g_infLastCamp = 0;
        g_infLoggedRoot = 0;
        return;
    }
    uintptr_t town = resTownRoot(base);
    if (!town) { g_infLoggedRoot = 0; return; }      // observed, like the banner, in town only
    uint32_t now = GetTickCount();
    if (now < g_infNextAt) return;
    g_infNextAt = now + 500;
    bool shown = false;
    char name[64];
    int lv = infEstateLevel(base, &shown, name, sizeof name);
    if (town != g_infLoggedRoot) {
        g_infLoggedRoot = town;
        logLine("infestation: town sample -> level %d -- %s", lv, s_infWhy[0] ? s_infWhy : "(no reason recorded)");
    }
    if (lv < 0) return;                              // no CC / no state: latch untouched
    uintptr_t camp = 0;
    safeReadPtr(base + RES_CAMPAIGN_RVA, &camp);
    if (g_infLastLvl < 0 || camp != g_infLastCamp) { // first sample or another save: silent latch
        g_infLastCamp = camp;
        g_infLastLvl = lv;
        return;
    }
    if (lv == g_infLastLvl) return;
    int old = g_infLastLvl;
    g_infLastLvl = lv;
    logLine("infestation: level %d -> %d (shown=%d, \"%s\")", old, lv, shown ? 1 : 0, name);
    if (!shown) return;                              // the game shows nothing; neither do we
    char word[96];
    infLevelWord(base, name, word, sizeof word);
    char utter[MAILBOX_SZ];
    if (word[0]) {
        _snprintf(utter, sizeof utter, axs(lv > old ? AXS_INF_UP_FMT : AXS_INF_DOWN_FMT), word);
    } else {
        infEstateText(base, utter, sizeof utter);
        if (!utter[0]) return;
    }
    utter[sizeof utter - 1] = 0;
    postSpeech(utter);
}
