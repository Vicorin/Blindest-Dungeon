// frontend/sharedui.cpp -- the second frontend slice

#include <cstdint>
#include <cstring>
#include <cstdio>
#include "internal.h"

// ---- BASE MENU: the GLOSSARY (terms + definitions) ----
static const uintptr_t GL_STATE_OFF     = 0x90;       // int tween state: 0 out, 1..3 in/up
static const uintptr_t GL_MEDIA_OFF     = 0x88;       // "media_column" vertical layout (the rows)
static const int GL_MAX_ROWS = 128;                   // campaign 64, +arena; generous cap
static const int GL_ROW_MAX  = 512;                   // a definition can be a sentence or two
static const int GL_TEXTS_MAX = 4;

static char g_glTerm[GL_MAX_ROWS][GL_ROW_MAX];        // per row: the term (left column)
static char g_glDef [GL_MAX_ROWS][GL_ROW_MAX];        // per row: the definition (right column)
static int  g_glRow = 0;                              // row cursor
static int  g_glCol = 0;                              // 0 = term, 1 = definition
static bool g_glActive    = false;                    // mirror of the open gate, for edge detection
static bool g_glAnnounced = false;                    // the entry line was spoken for this open
static uint32_t g_glOpenedTick = 0;                   // when the open edge was seen (build-wait)

static uintptr_t glObj(uintptr_t base) {
    uintptr_t obj = 0;
    if (!safeReadPtr(base + GL_SINGLETON_RVA, &obj) || obj <= 0x10000) return 0;
    uintptr_t vft = 0;
    if (!safeReadPtr(obj, &vft) || vft != base + GL_VFT_RVA) return 0;
    return obj;
}

static bool glIsOpen(uintptr_t base) {
    uintptr_t obj = glObj(base);
    if (!obj) return false;
    uint32_t st = 0;
    if (!safeReadU32(obj + GL_STATE_OFF, &st)) return false;
    return st != 0;                                    // appearing / shown / fading out
}
bool axIsGlossary() { return g_glActive; }

static void glGatherTexts(uintptr_t base, uintptr_t node, int depth, int* budget,
                          char texts[][GL_ROW_MAX], int* n, int max) {
    if (!node || depth > 8 || *budget <= 0 || *n >= max) return;
    (*budget)--;
    uintptr_t vft = 0;
    safeReadPtr(node, &vft);
    if (vft == base + TBW_VFTABLE_RVA) {
        char raw[GL_ROW_MAX], clean[GL_ROW_MAX];
        if (readTbwText(node, raw, sizeof raw)) {
            stripMarkup(raw, clean, sizeof clean);
            if (clean[0] && *n < max) {
                strncpy(texts[*n], clean, GL_ROW_MAX - 1);
                texts[*n][GL_ROW_MAX - 1] = 0;
                (*n)++;
            }
        }
    }
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
        if (asVector)
            for (int i = 0; i < kids && *n < max; i++) {
                uintptr_t kid = 0;
                if (safeReadPtr(beg + (uintptr_t)i * 8, &kid))
                    glGatherTexts(base, kid, depth + 1, budget, texts, n, max);
            }
    }
    if (!asVector && vft != base + TBW_VFTABLE_RVA) {
        uintptr_t content = 0;
        if (safeReadPtr(node + TL_CELL_CONTENT_OFF, &content) && tlLooksLikeWidget(base, content))
            glGatherTexts(base, content, depth + 1, budget, texts, n, max);
    }
    uintptr_t ab = 0, ae = 0;
    if (safeReadPtr(node + TL_ATTACH_BEG_OFF, &ab) && safeReadPtr(node + TL_ATTACH_END_OFF, &ae) &&
        ae > ab && ((ae - ab) >> 3) <= (uintptr_t)TL_WALK_KIDS_MAX) {
        int cnt = (int)((ae - ab) >> 3);
        for (int i = 0; i < cnt && *n < max; i++) {
            uintptr_t kid = 0;
            if (safeReadPtr(ab + (uintptr_t)i * 8, &kid) && tlLooksLikeWidget(base, kid))
                glGatherTexts(base, kid, depth + 1, budget, texts, n, max);
        }
    }
}

static int glCollect(uintptr_t base, uintptr_t obj, bool probe) {
    uintptr_t media = 0;
    if (!safeReadPtr(obj + GL_MEDIA_OFF, &media) || !tlLooksLikeWidget(base, media)) {
        if (probe) logLine("glossary probe: no media column at obj+0x%llx",
                           (unsigned long long)GL_MEDIA_OFF);
        return 0;
    }
    uintptr_t beg = 0, end = 0;
    if (!safeReadPtr(media + TL_KIDS_BEG_OFF, &beg) || !safeReadPtr(media + TL_KIDS_END_OFF, &end) ||
        end < beg) return 0;
    int cells = (int)((end - beg) >> 3);
    if (cells > TL_WALK_KIDS_MAX) cells = TL_WALK_KIDS_MAX;
    int count = 0, budget = TL_WALK_NODES_MAX;
    for (int c = 0; c < cells && count < GL_MAX_ROWS; c++) {
        uintptr_t cell = 0;
        if (!safeReadPtr(beg + (uintptr_t)c * 8, &cell) || !tlLooksLikeWidget(base, cell)) continue;
        char texts[GL_TEXTS_MAX][GL_ROW_MAX];
        int nt = 0;
        glGatherTexts(base, cell, 1, &budget, texts, &nt, 2);   // at most the two columns
        if (nt == 0) continue;                                  // separator / pure image cell
        strncpy(g_glTerm[count], texts[0], GL_ROW_MAX - 1);
        g_glTerm[count][GL_ROW_MAX - 1] = 0;
        for (int e = (int)strlen(g_glTerm[count]) - 1;
             e >= 0 && (g_glTerm[count][e] == ':' || g_glTerm[count][e] == ' '); e--)
            g_glTerm[count][e] = 0;
        if (nt >= 2) {
            strncpy(g_glDef[count], texts[1], GL_ROW_MAX - 1);
            g_glDef[count][GL_ROW_MAX - 1] = 0;
        } else {
            g_glDef[count][0] = 0;
        }
        if (probe) logLine("glossary probe: row[%d] term=\"%.80s\" def=\"%.200s\"",
                           count, g_glTerm[count], g_glDef[count]);
        count++;
    }
    if (probe) logLine("glossary probe: %d rows", count);
    return count;
}

static int glRowCount(uintptr_t base, uintptr_t* objOut) {
    uintptr_t obj = glObj(base);
    if (objOut) *objOut = obj;
    if (!obj) return 0;
    return glCollect(base, obj, false);
}

static void glSpeakCell(uintptr_t base, bool withTitle, bool sayColLabel, bool sayPos) {
    uintptr_t obj = 0;
    int n = glRowCount(base, &obj);
    char utter[MAILBOX_SZ];
    const char* title   = withTitle ? axs(AXS_GL_TITLE) : "";
    const char* titleSp = withTitle ? " " : "";
    if (n == 0) {
        _snprintf(utter, sizeof utter, "%s%s%s", title, titleSp, axs(AXS_GL_EMPTY));
        utter[sizeof utter - 1] = 0;
        postSpeech(utter);
        return;
    }
    if (g_glRow >= n) g_glRow = n - 1;
    if (g_glRow < 0)  g_glRow = 0;
    const char* cell = (g_glCol == 0) ? g_glTerm[g_glRow] : g_glDef[g_glRow];
    if (!cell[0]) cell = (g_glCol == 0) ? axs(AXS_GL_NO_TERM) : axs(AXS_GL_NO_DEF);
    char colLbl[64]; colLbl[0] = 0;
    if (sayColLabel) {
        _snprintf(colLbl, sizeof colLbl, " %s", g_glCol == 0 ? axs(AXS_GL_COL_TERM)
                                                             : axs(AXS_GL_COL_DEF));
        colLbl[sizeof colLbl - 1] = 0;
    }
    char pos[48]; pos[0] = 0;
    if (sayPos) {
        pos[0] = ' ';
        _snprintf(pos + 1, sizeof pos - 1, axs(AXS_POS_N_OF_M), g_glRow + 1, n);
        pos[sizeof pos - 1] = 0;
    }
    size_t cl = strlen(cell);
    char last = cl ? cell[cl - 1] : 0;
    bool punct = last == '.' || last == '!' || last == '?' || last == ':';
    _snprintf(utter, sizeof utter, "%s%s%s%s%s%s",
              title, titleSp, cell, punct ? "" : ".", colLbl, pos);
    utter[sizeof utter - 1] = 0;
    postSpeech(utter);
}

void glReannounce(uintptr_t base) {
    if (!g_glActive) return;
    logLine("glossary: re-announcing after a modal closed");
    glSpeakCell(base, true, true, true);
}

void checkGlossary(uintptr_t base) {
    bool open = glIsOpen(base);
    if (open && !g_glActive) {
        g_glActive = true;
        g_glAnnounced = false;
        g_glRow = 0;
        g_glCol = 0;
        g_glOpenedTick = GetTickCount();
        uintptr_t obj = glObj(base);
        logLine("glossary: opened, obj=%p", (void*)obj);
        if (obj) glCollect(base, obj, true);          // one-shot structure dump
    } else if (!open && g_glActive) {
        g_glActive = false;
        logLine("glossary: closed");
        if (!axModalClosed(base, "glossary")) postSpeech(axs(AXS_GL_CLOSED));
        return;
    }
    if (g_glActive && !g_glAnnounced) {
        uintptr_t obj = glObj(base);
        if (!obj) return;
        int n = glCollect(base, obj, false);
        if (n == 0 && GetTickCount() - g_glOpenedTick < 2000) return;   // build still running
        g_glAnnounced = true;
        logLine("glossary: announcing entry, %d rows", n);
        glSpeakCell(base, true, true, true);
    }
}

bool routeGlossaryKey(uintptr_t base, uint32_t sym, uint16_t mod, uint8_t repeat) {
    int dRow = 0, dCol = 0, jump = 0;
    if (axDecodeJump(sym, mod, repeat, &jump)) {      // Home/End: first/last row, column KEPT
        if (!jump) return true;                       // held jump: one landing per press
        dRow = jump;
    } else if (!axDecodeArrow(sym, mod, repeat, /*wantCols=*/true, &dRow, &dCol)) return false;
    if (!dRow && !dCol) return true;                  // throttled repeat: claimed, no step this tick
    uintptr_t obj = glObj(base);
    if (!obj) return true;
    int n = glCollect(base, obj, false);
    if (n == 0) { postSpeech(axs(AXS_GL_EMPTY)); return true; }
    axStepCursor(&g_glRow, n, 0);                     // re-clamp: the live tree can shrink rows

    if (dCol) {
        int want = (dCol > 0) ? 1 : 0;
        bool moved = want != g_glCol;
        g_glCol = want;                               // hard stop at the outer columns: re-read
        logLine("glossary nav column -> %s%s", g_glCol == 0 ? "term" : "definition",
                moved ? "" : " (already there)");
        glSpeakCell(base, false, true, false);
        return true;
    }

    int from = g_glRow;
    axStepCursor(&g_glRow, n, dRow);                  // hard stop: re-read the end row
    logLine("glossary nav row %d -> %d (of %d), col %d", from, g_glRow, n, g_glCol);
    glSpeakCell(base, false, false, true);
    return true;
}

static const uintptr_t HP_STATE_OFF     = 0x58;       // int lifecycle: 0 out, 1..4 up, 5..7 fading
static const uintptr_t HP_CTXIDX_OFF    = 0x80;       // int: which of the 17 topics (0..16)
static const int HP_CTX_COUNT = 17;
static const int HP_MAX_PARAS = 32;                   // paragraphs per topic (generous cap)
static const int HP_PARA_MAX  = 1024;                 // one paragraph

struct HelpCtx { const char* key; AxStrId label; };
static const HelpCtx kHelpCtx[HP_CTX_COUNT] = {
    { "town",                 AXS_HELPCTX_TOWN },
    { "town_stage_coach",     AXS_HELPCTX_STAGE_COACH },
    { "town_blacksmith",      AXS_HELPCTX_BLACKSMITH },
    { "town_guild",           AXS_HELPCTX_GUILD },
    { "town_camping_trainer", AXS_HELPCTX_CAMPING_TRAINER },
    { "town_tavern",          AXS_HELPCTX_TAVERN },
    { "town_abbey",           AXS_HELPCTX_ABBEY },
    { "town_sanitarium",      AXS_HELPCTX_SANITARIUM },
    { "town_nomad_wagon",     AXS_HELPCTX_NOMAD_WAGON },
    { "town_quest_select",    AXS_HELPCTX_QUEST_SELECT },
    { "town_provision",       AXS_HELPCTX_PROVISION },
    { "raid_room",            AXS_HELPCTX_RAID_ROOM },
    { "raid_hallway",         AXS_HELPCTX_RAID_HALLWAY },
    { "raid_combat",          AXS_HELPCTX_RAID_COMBAT },
    { "raid_camping",         AXS_HELPCTX_RAID_CAMPING },
    { "raid_wave_room",       AXS_HELPCTX_WAVE_ROOM },
    { "raid_wave_combat",     AXS_HELPCTX_WAVE_COMBAT },
};

static char g_hpPara[HP_MAX_PARAS][HP_PARA_MAX];   // the active topic's paragraphs, live-collected
static int  g_hpCount    = 0;                        // how many paragraphs
static int  g_hpRow      = 0;                        // paragraph cursor (for the Up/Down re-walk)
static bool g_hpPlaced   = false;                    // has a nav key landed on a paragraph yet?
static int  g_hpCtxIdx   = -1;                       // the topic those paragraphs were built for
static bool g_hpActive   = false;                    // mirror of the open gate, for edge detection
static bool g_hpAnnounced= false;                    // the entry read happened for this open
static uint32_t g_hpOpenedTick = 0;                  // when the open edge was seen (grace wait)

static uintptr_t hpObj(uintptr_t base) {
    uintptr_t obj = 0;
    if (!safeReadPtr(base + HP_SINGLETON_RVA, &obj) || obj <= 0x10000) return 0;
    uintptr_t vft = 0;
    if (!safeReadPtr(obj, &vft) || vft != base + HP_VFT_RVA) return 0;
    return obj;
}
static bool hpIsOpen(uintptr_t base) {
    uintptr_t obj = hpObj(base);
    if (!obj) return false;
    uint32_t st = 0;
    if (!safeReadU32(obj + HP_STATE_OFF, &st)) return false;
    return st >= 1 && st <= 4;                         // appearing / shown (5..7 = fading out)
}
bool axIsHelp() { return g_hpActive; }

static int hpCtxIndex(uintptr_t obj) {
    uint32_t idx = 0;
    if (!safeReadU32(obj + HP_CTXIDX_OFF, &idx)) return -1;
    if ((int)idx < 0 || (int)idx >= HP_CTX_COUNT) return -1;
    return (int)idx;
}

static int hpCollect(uintptr_t base, uintptr_t obj, bool probe) {
    g_hpCount = 0;
    int idx = hpCtxIndex(obj);
    if (idx < 0) {
        if (probe) logLine("help probe: context index at +0x%llx out of range",
                           (unsigned long long)HP_CTXIDX_OFF);
        g_hpCtxIdx = -1;
        return 0;
    }
    const char* ctxName = kHelpCtx[idx].key;
    int count = 0;
    for (int n = 0; n < HP_MAX_PARAS; n++) {
        char key[64];
        _snprintf(key, sizeof key, "str_help_%s_%d", ctxName, n);
        key[sizeof key - 1] = 0;
        char raw[HP_PARA_MAX];
        if (!resolveKey(base, key, raw, sizeof raw)) break;   // no more paragraphs for this topic
        stripMarkup(raw, g_hpPara[count], HP_PARA_MAX);
        if (!g_hpPara[count][0]) continue;                    // present-but-blank: skip, keep scanning
        if (probe) logLine("help probe: %s = \"%.180s\"", key, g_hpPara[count]);
        count++;
    }
    g_hpCount = count;
    g_hpCtxIdx = idx;
    if (probe) logLine("help probe: topic[%d]=\"%s\" -> %d paragraphs", idx, ctxName, count);
    return count;
}

static void hpBuildWhole(char* full, int sz) {
    const char* label = (g_hpCtxIdx >= 0 && g_hpCtxIdx < HP_CTX_COUNT) ? axs(kHelpCtx[g_hpCtxIdx].label) : "";
    _snprintf(full, sz, axs(AXS_HELP_TITLE_FMT), label);
    full[sz - 1] = 0;
    for (int i = 0; i < g_hpCount; i++) {
        int cur = (int)strlen(full);
        _snprintf(full + cur, sz - cur, " %s", g_hpPara[i]);
        full[sz - 1] = 0;
    }
}

static void hpSpeakWhole(uintptr_t base) {
    const char* label = (g_hpCtxIdx >= 0 && g_hpCtxIdx < HP_CTX_COUNT) ? axs(kHelpCtx[g_hpCtxIdx].label) : "";
    if (g_hpCount == 0) {
        char title[128], utter[256];
        _snprintf(title, sizeof title, axs(AXS_HELP_TITLE_FMT), label);
        title[sizeof title - 1] = 0;
        _snprintf(utter, sizeof utter, "%s %s", title, axs(AXS_HELP_EMPTY));
        utter[sizeof utter - 1] = 0;
        postSpeech(utter);
        return;
    }
    char full[MAILBOX_SZ];
    hpBuildWhole(full, sizeof full);
    logLine("help: reading whole topic \"%s\", %d paragraphs, %d chars",
            label, g_hpCount, (int)strlen(full));
    postSpeech(full);
}

// Speak the paragraph at g_hpRow with its position (the Up/Down re-walk).
static void hpSpeakPara(uintptr_t base) {
    if (g_hpCount == 0) { postSpeech(axs(AXS_HELP_EMPTY)); return; }
    if (g_hpRow < 0) g_hpRow = 0;
    if (g_hpRow >= g_hpCount) g_hpRow = g_hpCount - 1;
    const char* p = g_hpPara[g_hpRow];
    size_t l = strlen(p);
    char last = l ? p[l - 1] : 0;
    bool punct = last == '.' || last == '!' || last == '?' || last == ':';
    char posn[64];
    _snprintf(posn, sizeof posn, axs(AXS_HELP_PARA_N_OF_M), g_hpRow + 1, g_hpCount);
    posn[sizeof posn - 1] = 0;
    char utter[MAILBOX_SZ];
    _snprintf(utter, sizeof utter, "%s%s %s", p, punct ? "" : ".", posn);
    utter[sizeof utter - 1] = 0;
    postSpeech(utter);
}

void hpReannounce(uintptr_t base) {
    if (!g_hpActive) return;
    logLine("help: re-announcing after a modal closed");
    uintptr_t obj = hpObj(base);
    if (obj) hpCollect(base, obj, false);
    g_hpPlaced = false;
    g_hpRow = 0;
    hpSpeakWhole(base);
}

void checkHelp(uintptr_t base) {
    bool open = hpIsOpen(base);
    if (open && !g_hpActive) {
        g_hpActive = true;
        g_hpAnnounced = false;
        g_hpPlaced = false;
        g_hpRow = 0;
        g_hpOpenedTick = GetTickCount();
        uintptr_t obj = hpObj(base);
        uint32_t st = 0; if (obj) safeReadU32(obj + HP_STATE_OFF, &st);
        logLine("help: opened, obj=%p state=%u", (void*)obj, st);
        if (obj) hpCollect(base, obj, true);           // one-shot topic dump
    } else if (!open && g_hpActive) {
        g_hpActive = false;
        logLine("help: closed");
        if (!axModalClosed(base, "help")) postSpeech(axs(AXS_HELP_CLOSED));
        return;
    }
    if (g_hpActive && !g_hpAnnounced) {
        uintptr_t obj = hpObj(base);
        if (!obj) return;
        int n = hpCollect(base, obj, false);
        if (n == 0 && GetTickCount() - g_hpOpenedTick < 1500) return;   // context index may lag a frame
        g_hpAnnounced = true;
        logLine("help: announcing topic, %d paragraphs", n);
        hpSpeakWhole(base);
    }
}

bool routeHelpKey(uintptr_t base, uint32_t sym, uint16_t mod, uint8_t repeat) {
    int dRow = 0, dCol = 0, jump = 0;
    bool isJump = axDecodeJump(sym, mod, repeat, &jump);   // Home/End: first/last paragraph
    if (isJump) {
        if (!jump) return true;                        // held jump: one landing per press
        dRow = jump;
    } else if (!axDecodeArrow(sym, mod, repeat, /*wantCols=*/false, &dRow, &dCol)) return false;
    if (!dRow) return true;                            // throttled repeat: claimed, no step this tick
    uintptr_t obj = hpObj(base);
    if (!obj) return true;
    int liveIdx = hpCtxIndex(obj);
    if (liveIdx != g_hpCtxIdx) hpCollect(base, obj, false);   // topic changed under us: re-read live
    if (g_hpCount == 0) { postSpeech(axs(AXS_HELP_EMPTY)); return true; }
    if (!g_hpPlaced) {                                  // first arrow lands on paragraph 1
        g_hpPlaced = true;
        g_hpRow = 0;
        if (!isJump) {                                  // a jump falls through: End must be able
            hpSpeakPara(base);                          // to land on the LAST paragraph from
            return true;                                // an unplaced cursor, Home re-reads 0
        }
    }
    int from = g_hpRow;
    axStepCursor(&g_hpRow, g_hpCount, dRow);            // hard stop: re-read the end paragraph
    logLine("help nav paragraph %d -> %d (of %d)", from, g_hpRow, g_hpCount);
    hpSpeakPara(base);
    return true;
}

// ---- Controls page (SharedUI::ControlsPanel) ----
static const uintptr_t CP_STATE_OFF = 0x58;   // int lifecycle: 0 out, 1..4 up, 5..7 fading
static const int CP_MAX_LINES = 48;           // element rows (shipped: 12; generous)
static const int CP_LINE_MAX  = 256;          // one line ("H - Contextual help for any screen")
static const int CP_MAX_CATS  = 8;            // category scan bound (shipped: 3)
static const int CP_MAX_ELEMS = 24;           // per-category element scan bound (shipped max: 9)

static char g_cpCatName[CP_MAX_CATS][CP_LINE_MAX]; // section headers, spoken on section crossings
static char g_cpLine[CP_MAX_LINES][CP_LINE_MAX];   // the element rows, live-collected on open
static signed char g_cpLineCat[CP_MAX_LINES];      // which section each row belongs to
static int  g_cpCount     = 0;                   // how many rows
static int  g_cpRow       = 0;                   // row cursor (for the Up/Down re-walk)
static int  g_cpSpokenCat = -1;                  // section of the last row spoken (-1 = none yet,
                                                 //  so the next row announces its section)
static bool g_cpPlaced    = false;               // has a nav key landed on a row yet?
static bool g_cpActive    = false;               // mirror of the open gate, for edge detection
static bool g_cpAnnounced = false;               // the entry read happened for this open
static uint32_t g_cpOpenedTick = 0;              // when the open edge was seen (grace wait)

static uintptr_t cpObj(uintptr_t base) {
    uintptr_t obj = 0;
    if (!safeReadPtr(base + CP_SINGLETON_RVA, &obj) || obj <= 0x10000) return 0;
    uintptr_t vft = 0;
    if (!safeReadPtr(obj, &vft) || vft != base + CP_VFT_RVA) return 0;
    return obj;
}
static bool cpIsOpen(uintptr_t base) {
    uintptr_t obj = cpObj(base);
    if (!obj) return false;
    uint32_t st = 0;
    if (!safeReadU32(obj + CP_STATE_OFF, &st)) return false;
    return st >= 1 && st <= 4;                     // appearing / shown (5..7 = fading out)
}
bool axIsControls() { return g_cpActive; }

static int cpCollect(uintptr_t base, bool probe) {
    g_cpCount = 0;
    for (int c = 0; c < CP_MAX_CATS; c++) {
        char key[64];
        _snprintf(key, sizeof key, "menu_controls_category_%d", c);
        key[sizeof key - 1] = 0;
        char raw[CP_LINE_MAX];
        if (!resolveKey(base, key, raw, sizeof raw)) break;   // no more categories: done
        stripMarkup(raw, g_cpCatName[c], CP_LINE_MAX);
        if (probe && g_cpCatName[c][0])
            logLine("controls probe: %s = \"%.180s\"", key, g_cpCatName[c]);
        for (int n = 0; n < CP_MAX_ELEMS && g_cpCount < CP_MAX_LINES; n++) {
            _snprintf(key, sizeof key, "menu_controls_element_%d_%d", c, n);
            key[sizeof key - 1] = 0;
            if (!resolveKey(base, key, raw, sizeof raw)) break; // no more elements in this category
            stripMarkup(raw, g_cpLine[g_cpCount], CP_LINE_MAX);
            if (!g_cpLine[g_cpCount][0]) continue;             // present-but-blank: skip, keep scanning
            g_cpLineCat[g_cpCount] = (signed char)c;
            if (probe) logLine("controls probe: %s = \"%.180s\"", key, g_cpLine[g_cpCount]);
            g_cpCount++;
        }
    }
    if (probe) logLine("controls probe: %d rows", g_cpCount);
    return g_cpCount;
}

static void cpAppend(char* full, int sz, const char* text) {
    size_t l = strlen(text);
    char last = l ? text[l - 1] : 0;
    bool punct = last == '.' || last == '!' || last == '?' || last == ':';
    int cur = (int)strlen(full);
    _snprintf(full + cur, sz - cur, " %s%s", text, punct ? "" : ".");
    full[sz - 1] = 0;
}

static void cpSpeakWhole(uintptr_t base) {
    if (g_cpCount == 0) {
        char utter[128];
        _snprintf(utter, sizeof utter, "%s %s", axs(AXS_CTRLS_TITLE), axs(AXS_CTRLS_EMPTY));
        utter[sizeof utter - 1] = 0;
        postSpeech(utter);
        return;
    }
    char full[MAILBOX_SZ];
    _snprintf(full, sizeof full, "%s", axs(AXS_CTRLS_TITLE));
    full[sizeof full - 1] = 0;
    int cat = -1;
    for (int i = 0; i < g_cpCount; i++) {
        if (g_cpLineCat[i] != cat) {                       // section boundary: say the header once
            cat = g_cpLineCat[i];
            if (cat >= 0 && cat < CP_MAX_CATS && g_cpCatName[cat][0])
                cpAppend(full, sizeof full, g_cpCatName[cat]);
        }
        cpAppend(full, sizeof full, g_cpLine[i]);
    }
    g_cpSpokenCat = -1;
    logLine("controls: reading whole page, %d rows, %d chars", g_cpCount, (int)strlen(full));
    postSpeech(full);
}

static void cpSpeakLine(uintptr_t base) {
    if (g_cpCount == 0) { postSpeech(axs(AXS_CTRLS_EMPTY)); return; }
    if (g_cpRow < 0) g_cpRow = 0;
    if (g_cpRow >= g_cpCount) g_cpRow = g_cpCount - 1;
    int cat = g_cpLineCat[g_cpRow];
    char utter[MAILBOX_SZ];
    utter[0] = 0;
    if (cat != g_cpSpokenCat && cat >= 0 && cat < CP_MAX_CATS && g_cpCatName[cat][0])
        cpAppend(utter, sizeof utter, g_cpCatName[cat]);
    cpAppend(utter, sizeof utter, g_cpLine[g_cpRow]);
    g_cpSpokenCat = cat;
    postSpeech(utter[0] == ' ' ? utter + 1 : utter);   // cpAppend leads with a joiner space
}

void cpReannounce(uintptr_t base) {
    if (!g_cpActive) return;
    logLine("controls: re-announcing after a modal closed");
    cpCollect(base, false);
    g_cpPlaced = false;
    g_cpRow = 0;
    cpSpeakWhole(base);
}

void checkControls(uintptr_t base) {
    bool open = cpIsOpen(base);
    if (open && !g_cpActive) {
        g_cpActive = true;
        g_cpAnnounced = false;
        g_cpPlaced = false;
        g_cpRow = 0;
        g_cpSpokenCat = -1;
        g_cpOpenedTick = GetTickCount();
        uintptr_t obj = cpObj(base);
        uint32_t st = 0; if (obj) safeReadU32(obj + CP_STATE_OFF, &st);
        logLine("controls: opened, obj=%p state=%u", (void*)obj, st);
        cpCollect(base, true);                          // one-shot page dump
    } else if (!open && g_cpActive) {
        g_cpActive = false;
        logLine("controls: closed");
        if (!axModalClosed(base, "controls")) postSpeech(axs(AXS_CTRLS_CLOSED));
        return;
    }
    if (g_cpActive && !g_cpAnnounced) {
        int n = cpCollect(base, false);
        if (n == 0 && GetTickCount() - g_cpOpenedTick < 1500) return;  // tables may lag a frame
        g_cpAnnounced = true;
        logLine("controls: announcing page, %d lines", n);
        cpSpeakWhole(base);
    }
}

bool routeControlsKey(uintptr_t base, uint32_t sym, uint16_t mod, uint8_t repeat) {
    int dRow = 0, dCol = 0, jump = 0;
    bool isJump = axDecodeJump(sym, mod, repeat, &jump);   // Home/End: first/last line
    if (isJump) {
        if (!jump) return true;                        // held jump: one landing per press
        dRow = jump;
    } else if (!axDecodeArrow(sym, mod, repeat, /*wantCols=*/false, &dRow, &dCol)) return false;
    if (!dRow) return true;                            // throttled repeat: claimed, no step this tick
    if (g_cpCount == 0) cpCollect(base, false);        // grace-window arrow: try once more, live
    if (g_cpCount == 0) { postSpeech(axs(AXS_CTRLS_EMPTY)); return true; }
    if (!g_cpPlaced) {                                  // first arrow lands on line 1
        g_cpPlaced = true;
        g_cpRow = 0;
        if (!isJump) {                                  // a jump falls through: End must be able
            cpSpeakLine(base);                          // to land on the LAST line from an
            return true;                                // unplaced cursor, Home re-reads 0
        }
    }
    int from = g_cpRow;
    axStepCursor(&g_cpRow, g_cpCount, dRow);            // hard stop: re-read the end line
    logLine("controls nav line %d -> %d (of %d)", from, g_cpRow, g_cpCount);
    cpSpeakLine(base);
    return true;
}

// ---- Journal pages (UI::Panel::JournalPopup) ----
static const uintptr_t JP_NAV_OFF       = 0x414;     // char: 1 = the prev/next buttons are built
static const uintptr_t JP_SC_COUNT_OFF  = 0x00;      // uint: how many journal pages exist in total
static const uintptr_t JP_SC_TITLES_OFF = 0x08;      // -> record[] begin (titles), stride JP_REC_STRIDE
static const uintptr_t JP_SC_TEXTS_OFF  = 0x20;      // -> record[] begin (texts)
static const uintptr_t JP_REC_STRIDE    = 0x10;      // record: +0x00 char* text, +0x08 packed length
static const uint32_t  JP_ID_PREV       = 0x70727670; // 'prvp' — previous collected page
static const uint32_t  JP_ID_NEXT       = 0x6e657870; // 'nexp' — next collected page
static const uint32_t  JP_ID_CLOSE      = UI_CLOSE_ELEM_ID; // 'slct', the close button (Escape's click

static const int JP_MAX_ROWS = 40;                 // header + sentences; generous for a 730-char page
static const int JP_ROW_SZ   = 512;                // one sentence
static const int JP_TEXT_SZ  = 2048;               // the whole page text
static const int JP_MERGE_MIN = 40;                // a fragment shorter than this joins the next row

uintptr_t g_jpObj      = 0;
DWORD     g_jpSeenTick = 0;      // when the hook last fired (backstop for "gone")
bool      g_jpActive   = false;  // mirror of the open gate, for edge detection
static bool      g_jpAnnounced= false;  // has this appearance been read out?
static int       g_jpPage     = -1;     // the page index those rows were built for
static bool      g_jpNav      = false;  // does this appearance have prev/next buttons?
static char      g_jpRows[JP_MAX_ROWS][JP_ROW_SZ];
static int       g_jpCount    = 0;      // how many rows
static int       g_jpRow      = 0;      // row cursor for the Up/Down re-walk
static bool      g_jpPlaced   = false;  // has an arrow landed on a row yet?
static int       g_jpTotal    = 0;
static bool      g_jpDumped   = false;

static uintptr_t jpObj(uintptr_t base) {
    if (!g_jpObj || g_jpObj <= 0x10000) return 0;
    uintptr_t vft = 0;
    if (!safeReadPtr(g_jpObj, &vft) || vft != base + JP_VFT_RVA) return 0;
    return g_jpObj;
}
static const DWORD JP_GAP_MS = 500;                // no draw for this long = it has left the screen
static bool jpIsOpen(uintptr_t base) {
    uintptr_t obj = jpObj(base);
    if (!obj) return false;
    if (!g_jpSeenTick || (GetTickCount() - g_jpSeenTick) >= JP_GAP_MS) return false;
    uint32_t st = 0;
    if (!safeReadU32(obj + JP_STATE_OFF, &st)) return false;
    return st >= 1 && st <= 4;                     // appearing / shown (5..7 = fading out)
}
bool axIsJournal() { return g_jpActive; }

static bool jpTableStr(uintptr_t base, uintptr_t vecOff, int idx, char* out, int sz) {
    out[0] = 0;
    uintptr_t sc = 0;
    if (!safeReadPtr(base + JP_SYSCLASS_RVA, &sc) || sc <= 0x10000) return false;
    uint32_t total = 0;
    if (!safeReadU32(sc + JP_SC_COUNT_OFF, &total) || !total || total > 4096) return false;
    if (idx < 0 || (uint32_t)idx >= total) return false;
    uintptr_t begin = 0;
    if (!safeReadPtr(sc + vecOff, &begin) || begin <= 0x10000) return false;
    uintptr_t rec = begin + (uintptr_t)idx * JP_REC_STRIDE;
    uintptr_t str = 0;
    if (!safeReadPtr(rec, &str) || str <= 0x10000) return false;
    char raw[JP_TEXT_SZ];
    if (!safeReadCStr(str, raw, (int)sizeof raw) || !raw[0]) return false;
    stripMarkup(raw, out, sz);
    return out[0] != 0;
}

static int jpTotalPages(uintptr_t base) {
    uintptr_t sc = 0;
    if (!safeReadPtr(base + JP_SYSCLASS_RVA, &sc) || sc <= 0x10000) return 0;
    uint32_t total = 0;
    if (!safeReadU32(sc + JP_SC_COUNT_OFF, &total) || total > 4096) return 0;
    return (int)total;
}

static void jpBuildRows(const char* header, const char* text) {
    for (int i = 0; i < JP_MAX_ROWS; i++) g_jpRows[i][0] = 0;
    _snprintf(g_jpRows[0], JP_ROW_SZ, "%s", header);
    g_jpRows[0][JP_ROW_SZ - 1] = 0;
    g_jpCount = 1;

    int used = 0;                                   // chars in the row being built
    const char* p = text;
    while (*p && g_jpCount < JP_MAX_ROWS) {
        char c = *p++;
        if (used < JP_ROW_SZ - 1) { g_jpRows[g_jpCount][used++] = c; g_jpRows[g_jpCount][used] = 0; }
        bool term = (c == '.' || c == '!' || c == '?');
        if (!term) continue;
        while (*p == '.' || *p == '!' || *p == '?' || *p == '"' || *p == '\'') {   // "..." / .!" runs
            if (used < JP_ROW_SZ - 1) { g_jpRows[g_jpCount][used++] = *p; g_jpRows[g_jpCount][used] = 0; }
            p++;
        }
        if (*p && *p != ' ' && *p != '\t' && *p != '\n') continue;                 // "3.5" — not an end
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
        if (used < JP_MERGE_MIN) {                                                 // too short to stand
            if (used < JP_ROW_SZ - 1) { g_jpRows[g_jpCount][used++] = ' '; g_jpRows[g_jpCount][used] = 0; }
            continue;
        }
        g_jpCount++;
        used = 0;
    }
    // A trailing chunk with no terminator is still a row.
    if (g_jpCount < JP_MAX_ROWS && used > 0) g_jpCount++;
}

static bool jpCollect(uintptr_t base, uintptr_t obj, bool probe) {
    uint32_t idx = 0;
    if (!safeReadU32(obj + JP_PAGE_OFF, &idx)) {
        if (probe) logLine("journal probe: no page index at obj+0x%llx", (unsigned long long)JP_PAGE_OFF);
        return false;
    }
    uint8_t nav = 0;
    safeReadU8(obj + JP_NAV_OFF, &nav);
    g_jpNav   = nav != 0;
    g_jpPage  = (int)idx;
    g_jpTotal = jpTotalPages(base);

    char title[JP_ROW_SZ] = { 0 }, text[JP_TEXT_SZ] = { 0 };
    bool haveT = jpTableStr(base, JP_SC_TITLES_OFF, (int)idx, title, sizeof title);
    bool haveX = jpTableStr(base, JP_SC_TEXTS_OFF,  (int)idx, text,  sizeof text);

    char header[JP_ROW_SZ];
    if (haveT && g_jpTotal > 0)
        _snprintf(header, sizeof header, axs(AXS_JP_HDR_TITLE_POS), title, (int)idx + 1, g_jpTotal);
    else if (haveT)
        _snprintf(header, sizeof header, axs(AXS_JP_HDR_TITLE), title);
    else if (g_jpTotal > 0)
        _snprintf(header, sizeof header, axs(AXS_JP_HDR_POS), (int)idx + 1, g_jpTotal);
    else
        _snprintf(header, sizeof header, "%s", axs(AXS_JP_HDR));
    header[sizeof header - 1] = 0;

    jpBuildRows(header, haveX ? text : "");
    if (probe)
        logLine("journal probe: page=%u of %d nav=%d haveTitle=%d haveText=%d chars=%d rows=%d "
                "title=\"%.90s\"", idx, g_jpTotal, (int)nav, (int)haveT, (int)haveX,
                (int)strlen(text), g_jpCount, title);
    if (!haveT && !haveX)
        logLine("⚠ journal: page %u read NOTHING from the table (sysclass=0x%llx)",
                idx, (unsigned long long)JP_SYSCLASS_RVA);
    return true;
}

static void jpDumpFocusIds(uintptr_t base) {
    uintptr_t begin = 0, end = 0;
    if (!safeReadPtr(base + VEC_BEGIN_RVA, &begin) ||
        !safeReadPtr(base + VEC_END_RVA, &end) || !begin || end <= begin) {
        logLine("journal probe: no focus element vector");
        return;
    }
    uintptr_t count = (end - begin) / ELEM_STRIDE;
    if (count > 256) count = 256;
    char line[512];
    int used = _snprintf(line, sizeof line, "journal probe: %d focus elems:", (int)count);
    for (uintptr_t i = 0; i < count; i++) {
        int64_t id = 0;
        if (!safeReadI64(begin + i * ELEM_STRIDE + ELEM_ID_OFF, &id)) continue;
        uint32_t cc = (uint32_t)(uint64_t)id;
        char t[5]; fourcc(cc, t);
        int n = _snprintf(line + used, (int)sizeof line - used, " [0x%08x '%s']", cc, t);
        if (n < 0 || used + n >= (int)sizeof line - 8) {
            logLine("%s", line);
            used = _snprintf(line, sizeof line, "journal probe (cont):");
            n = _snprintf(line + used, (int)sizeof line - used, " [0x%08x '%s']", cc, t);
        }
        if (n > 0) used += n;
    }
    logLine("%s", line);
    logLine("journal probe: prev(0x%08x)=%s next(0x%08x)=%s close(0x%08x)=%s",
            JP_ID_PREV,  feGetElementById((int64_t)JP_ID_PREV)  ? "on screen" : "absent",
            JP_ID_NEXT,  feGetElementById((int64_t)JP_ID_NEXT)  ? "on screen" : "absent",
            JP_ID_CLOSE, feGetElementById((int64_t)JP_ID_CLOSE) ? "on screen" : "absent");
}

static void jpSpeakWhole(bool turn) {
    char full[MAILBOX_SZ];
    _snprintf(full, sizeof full, "%s", g_jpRows[0]);
    full[sizeof full - 1] = 0;
    for (int i = 1; i < g_jpCount; i++) {
        int cur = (int)strlen(full);
        _snprintf(full + cur, (int)sizeof full - cur, " %s", g_jpRows[i]);
        full[sizeof full - 1] = 0;
    }
    if (g_jpCount <= 1) {
        int cur = (int)strlen(full);
        _snprintf(full + cur, (int)sizeof full - cur, " %s", axs(AXS_JP_NO_TEXT));
        full[sizeof full - 1] = 0;
    }
    logLine("journal: reading page %d, %d rows, %d chars", g_jpPage, g_jpCount, (int)strlen(full));
    postSpeech(full, true, turn ? SPK_NAV : SPK_EVENT);
}

static void jpSpeakRow() {
    if (g_jpRow < 0) g_jpRow = 0;
    if (g_jpRow >= g_jpCount) g_jpRow = g_jpCount - 1;
    postSpeech(g_jpRows[g_jpRow]);
}

void jpReannounce(uintptr_t base) {
    if (!g_jpActive) return;
    uintptr_t obj = jpObj(base);
    if (!obj) return;
    logLine("journal: re-announcing after a modal closed");
    jpCollect(base, obj, false);
    g_jpPlaced = false;
    g_jpRow = 0;
    jpSpeakWhole(/*turn=*/false);
}

void checkJournal(uintptr_t base) {
    bool open = jpIsOpen(base);
    if (open && !g_jpActive) {
        g_jpActive    = true;
        g_jpAnnounced = false;
        g_jpPlaced    = false;
        g_jpDumped    = false;
        g_jpRow       = 0;
        g_jpPage      = -1;
        uintptr_t obj = jpObj(base);
        uint32_t st = 0; if (obj) safeReadU32(obj + JP_STATE_OFF, &st);
        logLine("journal: opened, obj=%p state=%u", (void*)obj, st);
    } else if (!open && g_jpActive) {
        g_jpActive = false;
        logLine("journal: closed");
        if (!axModalClosed(base, "journal")) postSpeech(axs(AXS_JP_CLOSED));
        return;
    }
    if (!g_jpActive) return;

    uintptr_t obj = jpObj(base);
    if (!obj) return;
    if (!g_jpDumped) { g_jpDumped = true; jpDumpFocusIds(base); }

    uint32_t live = 0;
    if (!safeReadU32(obj + JP_PAGE_OFF, &live)) return;
    if (!g_jpAnnounced || (int)live != g_jpPage) {
        bool turn = g_jpAnnounced;                 // a page TURN, not the first read
        if (!jpCollect(base, obj, !turn)) return;
        g_jpAnnounced = true;
        g_jpPlaced    = false;
        g_jpRow       = 0;
        if (turn) logLine("journal: page turned -> %d", g_jpPage);
        jpSpeakWhole(turn);
    }
}

void serviceStatueJournalWatch() {
    if (!g_stJrnWatchUntil) return;
    if (g_jpActive) {                              // it opened: the reader has it from here
        logLine("statue: the journal popup opened — watch cleared");
        g_stJrnWatchUntil = 0;
        return;
    }
    if ((int)(GetTickCount() - g_stJrnWatchUntil) < 0) return;   // still waiting
    g_stJrnWatchUntil = 0;
    logLine("⚠ statue: the journal popup never opened");
    postSpeech(axs(AXS_JP_DIDNT_OPEN));
}

static void jpReleaseReadHold(const char* key) {
    char who[96];
    _snprintf(who, sizeof who, "journal: %s", key);
    who[sizeof who - 1] = 0;
    axReleaseReadHold(who);
}

bool routeJournalKey(uintptr_t base, uint32_t sym, uint16_t mod, uint8_t repeat) {
    if (mod & (KMOD_LALT | KMOD_RALT | KMOD_LCTRL | KMOD_RCTRL)) return false;

    if (sym == SDLK_ESCAPE) {
        if (repeat) return true;
        jpReleaseReadHold("Escape");
        if (feGetElementById((int64_t)JP_ID_CLOSE) && frontEndClickElementId((int64_t)JP_ID_CLOSE)) {
            logLine("journal: Escape -> clicked the close button");
            return true;
        }
        logLine("journal: Escape -> no close button on screen, passing it to the game");
        return false;
    }

    if (sym == SDLK_LEFT || sym == SDLK_RIGHT) {
        if (repeat) return true;                   // claimed, one turn per press
        jpReleaseReadHold("a page-turn key");
        bool next = (sym == SDLK_RIGHT);
        if (!g_jpNav) {                            // a raid pickup: this page stands alone
            postSpeech(axs(AXS_JP_STANDALONE));
            return true;
        }
        uint32_t id = next ? JP_ID_NEXT : JP_ID_PREV;
        if (!feGetElementById((int64_t)id)) {
            postSpeech(next ? axs(AXS_JP_NO_LATER) : axs(AXS_JP_NO_EARLIER));
            return true;
        }
        if (!frontEndClickElementId((int64_t)id)) {
            logLine("⚠ journal: the %s button would not click", next ? "next" : "previous");
            postSpeech(axs(AXS_JP_NO_TURN));
        }
        return true;                               // the readout rides the observed index change
    }

    int dRow = 0, dCol = 0, jump = 0;
    bool isJump = axDecodeJump(sym, mod, repeat, &jump);   // Home/End: first/last row
    if (isJump) {
        if (!jump) return true;                    // held jump: one landing per press
        dRow = jump;
    } else if (!axDecodeArrow(sym, mod, repeat, /*wantCols=*/false, &dRow, &dCol)) return false;
    if (!dRow) return true;                        // throttled repeat: claimed, no step this tick
    jpReleaseReadHold(isJump ? "a jump" : "an arrow");
    if (g_jpCount <= 0) { postSpeech(axs(AXS_JP_NOTHING_TO_READ)); return true; }
    if (!g_jpPlaced) {                             // the whole page was just read: land on the header
        g_jpPlaced = true;
        g_jpRow = 0;
        if (!isJump) {                             // a jump falls through so End can land on the
            jpSpeakRow();                          // LAST row from an unplaced cursor
            return true;
        }
    }
    int from = g_jpRow;
    axStepCursor(&g_jpRow, g_jpCount, dRow);       // hard stop: re-read the last row
    logLine("journal nav row %d -> %d (of %d)", from, g_jpRow, g_jpCount);
    jpSpeakRow();
    return true;
}
