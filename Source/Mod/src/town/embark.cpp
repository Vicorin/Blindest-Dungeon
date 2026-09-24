// town/embark.cpp -- the fourth TOWN slice

#include <cstdint>
#include <cstring>
#include <cstdio>
#include "internal.h"
// ---- TOWN: EMBARK / RAID PLANNING (AX_EMBARK) ----
static const uintptr_t EMB_QS_STATE_OFF   = 0x34d0;
static const uintptr_t EMB_PROV_STATE_OFF = 0x37c8;
static const uint32_t  EMB_FWRD_ID        = 0x66777264u; // 'fwrd' — the nav-bar forward button
static const uint32_t  EMB_MARKER_BASE    = 0x717374u;   // quest marker element id = base + quest index

static const uintptr_t EMB_QVEC_BEG       = 0x1230;
static const uintptr_t EMB_QVEC_END       = 0x1238;
static const uintptr_t EMB_QSEL_IDX       = 0x1248;   // Campaign+: int, the SELECTED quest index
static const uintptr_t EMB_QSPECIAL_IDX   = 0x1220;   // Campaign+: int, out-of-vector special index...
static const uintptr_t EMB_QSPECIAL_REC   = 0x12c0;   // ...whose record sits inline here (was 0x1270)
static const uintptr_t EMB_Q_STRIDE       = 0x2f0;    // (was 0x2a0)
static const uintptr_t EMB_Q_DUNGEONDEF   = 0x110;
static const uintptr_t EMB_Q_DIFF         = 0x118;    // quest+: int difficulty
static const uintptr_t EMB_Q_LEN          = 0x11c;    // quest+: int length
static const uintptr_t EMB_Q_GOALS_BEG    = 0x120;
static const uintptr_t EMB_Q_GOALS_END    = 0x128;

// ---- Round 4: the two facts the screen shows and the mod did not ----
static const uintptr_t EMB_QS_PANEL_OFF   = 0x3478;
static const uintptr_t EMB_QS_PARTYNAME   = 0xf0;     // panel+: PartyName record*, 0 = no combo
static const int       EMB_PN_ID_MAX      = 64;       // rec+0x00: the library id, inline c-string

static const uintptr_t EMB_PROG_SYS_OFF   = 0x1680;   // Campaign+: Progression::System (was 0x15e0)
static const uintptr_t EMB_PROG_XPMAP_OFF = 0x60;     // Progression::System+: id hash -> xp map
static const int       EMB_MASTERY_MAX    = 32;       // sanity ceiling; the table ships 8 levels

typedef char* (*EmbNameIdFn)(char* out , uintptr_t quest);
typedef char* (*EmbSpecificsFn)(char* out , uintptr_t quest);
typedef char* (*EmbFwdLabelFn)(uintptr_t townRoot, char* out );

static const int EMB_MAX_ROWS  = 24;                   // a week offers ~2-13 quests; never a hard trust
static const int EMB_TIP_MAX   = 20;                   // row info + goals + description + rewards

static bool  g_embActive       = false;                // the quest list context is live
static bool  g_embProvOpen     = false;                // provision-layer edge tracking
static int   g_embCol          = 0;                    // cursor: which LOCATION column
static int   g_embRow          = 0;                    // cursor: which quest inside that column
static bool  g_embDumped       = false;
static DWORD g_embProbeAt      = 0;
static bool  g_embKeyLogged    = false;                // kbActionForKey answer for E, once
static int   g_embTipLine      = 0;                    // Ctrl+Up/Down buffer cursor (0 = row info)
static uintptr_t g_embPnRec    = 0;                    // the party-combo record last seen on screen
static bool  g_embPnSeeded     = false;                // ...and whether it has been read even once

DWORD g_embFwdWatchUntil = 0;                          // 0 = idle
static bool  g_embFwdQsBefore   = false;
static bool  g_embFwdProvBefore = false;
static bool  g_embFwdDlgSeen    = false;
static int   g_embFwdLayerBefore = -1;
static char  g_embFwdLabel[64];                        // the button's resolved label at click time

// Enter's selection watch: announce from the OBSERVED selected-index change.
static DWORD g_embSelWatchUntil = 0;                   // 0 = idle
static int   g_embSelWant       = -1;
static char  g_embSelName[EMB_NAME_MAX];

bool axIsEmbark() { return g_embActive; }

static uintptr_t embCampaign(uintptr_t base) {
    uintptr_t c = 0;
    if (!safeReadPtr(base + RES_CAMPAIGN_RVA, &c) || c <= 0x10000) return 0;
    return c;
}

bool embQsOpen(uintptr_t root) {
    uint32_t v = 0;
    if (!safeReadU32(root + EMB_QS_STATE_OFF, &v)) return false;
    return v != 0 && v != 5 && v != 6 && v != 7;
}

bool embProvIsOpen(uintptr_t root) {
    uint32_t v = 0;
    return safeReadU32(root + EMB_PROV_STATE_OFF, &v) && v != 0;
}

static int embQuestVec(uintptr_t camp, uintptr_t* begOut) {
    uintptr_t beg = 0, end = 0;
    if (!safeReadPtr(camp + EMB_QVEC_BEG, &beg) || !safeReadPtr(camp + EMB_QVEC_END, &end) ||
        !beg || end < beg) return 0;
    uintptr_t span = end - beg;
    if (span % EMB_Q_STRIDE != 0) {
        logLine("embark: quest vector span 0x%llx not a stride multiple — refused",
                (unsigned long long)span);
        return 0;
    }
    int n = (int)(span / EMB_Q_STRIDE);
    if (n < 0 || n > EMB_MAX_ROWS) { logLine("embark: quest count %d out of range — refused", n); return 0; }
    if (begOut) *begOut = beg;
    return n;
}

struct EmbRow { int qIdx; uintptr_t quest; };
static int embCollect(uintptr_t base, EmbRow* rows, int max) {
    uintptr_t camp = embCampaign(base);
    if (!camp) return 0;
    uintptr_t beg = 0;
    int n = embQuestVec(camp, &beg);
    int cnt = 0;
    for (int i = 0; i < n && cnt < max; i++) {
        rows[cnt].qIdx  = i;
        rows[cnt].quest = beg + (uintptr_t)i * EMB_Q_STRIDE;
        cnt++;
    }
    int32_t special = -1;
    safeReadU32(camp + EMB_QSPECIAL_IDX, (uint32_t*)&special);
    if (special >= n && cnt < max) {
        // Sanity: the inline record must read like a quest before it becomes a row.
        uint32_t d = 0, l = 0;
        uintptr_t rec = camp + EMB_QSPECIAL_REC;
        if (safeReadU32(rec + EMB_Q_DIFF, &d) && safeReadU32(rec + EMB_Q_LEN, &l) &&
            d <= 20 && l <= 20) {
            rows[cnt].qIdx  = special;
            rows[cnt].quest = rec;
            cnt++;
        } else {
            logLine("embark: special idx %d armed but the inline record fails sanity (d=%u l=%u)",
                    special, d, l);
        }
    }
    return cnt;
}

static bool embNameId(uintptr_t base, uintptr_t quest, char* out, int outsz) {
    char buf[0x100];
    memset(buf, 0, sizeof buf);
    EmbNameIdFn fn = (EmbNameIdFn)(base + EMB_NAMEID_RVA);
    __try { fn(buf, quest); }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        logLine("embark: name-id fn FAULTED on quest %p", (void*)quest);
        return false;
    }
    buf[sizeof buf - 1] = 0;
    if (!buf[0]) return false;
    strncpy(out, buf, outsz - 1);
    out[outsz - 1] = 0;
    return true;
}

static void embQuestName(uintptr_t base, uintptr_t quest, char* out, int outsz) {
    char id[0x100], key[0x140];
    out[0] = 0;
    if (!embNameId(base, quest, id, sizeof id)) { strncpy(out, axs(AXS_EMB_UNKNOWN_QUEST), outsz - 1); out[outsz-1] = 0; return; }
    _snprintf(key, sizeof key, "town_quest_name_%s", id);
    key[sizeof key - 1] = 0;
    char raw[EMB_NAME_MAX];
    if (resolveKey(base, key, raw, sizeof raw)) {
        stripMarkup(raw, out, outsz);
    } else {
        logLine("embark: no localization for \"%s\" — speaking the raw id", key);
        strncpy(out, id, outsz - 1);
        out[outsz - 1] = 0;
        for (char* p = out; *p; p++) if (*p == '_' || *p == '+') *p = ' ';
    }
}

static bool embDungeonId(uintptr_t quest, char* out, int outsz) {
    out[0] = 0;
    uintptr_t def = 0;
    char id[32];
    if (!safeReadPtr(quest + EMB_Q_DUNGEONDEF, &def) || def <= 0x10000) return false;
    if (!safeReadCStr(def, id, sizeof id) || !tmIdLooksValid(id)) return false;
    strncpy(out, id, outsz - 1);
    out[outsz - 1] = 0;
    return true;
}

static void embDungeonNameOf(uintptr_t base, const char* id, char* out, int outsz) {
    out[0] = 0;
    if (!id || !id[0]) return;
    char key[64], raw[EMB_NAME_MAX];
    _snprintf(key, sizeof key, "dungeon_name_%s", id);
    key[sizeof key - 1] = 0;
    if (resolveKey(base, key, raw, sizeof raw)) stripMarkup(raw, out, outsz);
    else logLine("embark: no localization for \"%s\"", key);
}

static void embDungeonName(uintptr_t base, uintptr_t quest, char* out, int outsz) {
    out[0] = 0;
    char id[32];
    if (!embDungeonId(quest, id, sizeof id)) return;
    embDungeonNameOf(base, id, out, outsz);
}

static uintptr_t embQsPanel(uintptr_t base) {
    uintptr_t root = resTownRoot(base);
    if (!root || !embQsOpen(root)) return 0;
    return root + EMB_QS_PANEL_OFF;
}

typedef unsigned int (*EmbMasteryFn)(uintptr_t progSystem, uint32_t dungeonIdHash);
static int embMasteryLevel(uintptr_t base, const char* dungeonId) {
    if (!dungeonId || !dungeonId[0]) return -1;
    uintptr_t camp = embCampaign(base);
    if (!camp) return -1;
    unsigned int lvl = 0;
    __try { lvl = ((EmbMasteryFn)(base + EMB_MASTERY_RVA))(camp + EMB_PROG_SYS_OFF, resHash(dungeonId)); }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        logLine("embark: the mastery getter FAULTED on dungeon \"%s\"", dungeonId);
        return -1;
    }
    if (lvl > (unsigned)EMB_MASTERY_MAX) {
        logLine("embark: mastery level %u for \"%s\" is out of range — refused", lvl, dungeonId);
        return -1;
    }
    return (int)lvl;
}

static bool embDungeonXpProbe(uintptr_t base, const char* dungeonId, uint32_t* xpOut) {
    uintptr_t camp = embCampaign(base);
    if (!camp || !dungeonId || !dungeonId[0]) return false;
    uint32_t want = resHash(dungeonId);
    uintptr_t head = 0;
    if (!safeReadPtr(camp + EMB_PROG_SYS_OFF + EMB_PROG_XPMAP_OFF, &head) || head <= 0x10000)
        return false;
    uintptr_t node = 0, best = head;
    if (!safeReadPtr(head + 0x08, &node)) return false;
    for (int guard = 0; guard < 64; guard++) {
        uint8_t nil = 1;
        if (!safeReadU8(node + 0x19, &nil) || nil) break;
        uint32_t key = 0;
        if (!safeReadU32(node + 0x1c, &key)) return false;
        uintptr_t next = 0;
        if (key < want) { if (!safeReadPtr(node + 0x10, &next)) return false; }
        else            { best = node; if (!safeReadPtr(node + 0x00, &next)) return false; }
        node = next;
    }
    if (best == head) return false;
    uint8_t nil = 1;
    uint32_t key = 0;
    if (!safeReadU8(best + 0x19, &nil) || nil) return false;
    if (!safeReadU32(best + 0x1c, &key) || key != want) return false;
    return safeReadU32(best + 0x20, xpOut);
}

static bool embPartyNameOf(uintptr_t base, uintptr_t rec, char* out, int outsz) {
    out[0] = 0;
    if (!rec) return false;
    char id[EMB_PN_ID_MAX];
    if (!safeReadCStr(rec, id, sizeof id) || !id[0]) return false;
    char key[EMB_PN_ID_MAX + 32], raw[EMB_NAME_MAX];
    _snprintf(key, sizeof key, "party_name_%s", id);
    key[sizeof key - 1] = 0;
    if (!resolveKey(base, key, raw, sizeof raw)) {
        logLine("embark: no localization for \"%s\" — the combo name is not spoken", key);
        return false;
    }
    stripMarkup(raw, out, outsz);
    return out[0] != 0;
}

static uintptr_t embPartyNameRec(uintptr_t base) {
    uintptr_t panel = embQsPanel(base), rec = 0;
    if (!panel) return 0;
    if (!safeReadPtr(panel + EMB_QS_PARTYNAME, &rec) || rec <= 0x10000) return 0;
    return rec;
}

bool embPartyName(uintptr_t base, char* out, int outsz) {
    return embPartyNameOf(base, embPartyNameRec(base), out, outsz);
}

static const uintptr_t EMB_PQ_LENGTH_ID    = 0x590;
static uintptr_t embPlotDef(uintptr_t base, uintptr_t quest);

static void embLengthSentence(uintptr_t base, uintptr_t quest, char* out, int outsz) {
    out[0] = 0;
    uint32_t len = 0;
    if (!safeReadU32(quest + EMB_Q_LEN, &len)) return;
    char key[96], raw[96], txt[64] = {0};
    _snprintf(key, sizeof key, "town_quest_length_%d", len);
    key[sizeof key - 1] = 0;
    if (uintptr_t def = embPlotDef(base, quest)) {
        char lid[48] = {0};
        bool ok = safeReadCStr(def + EMB_PQ_LENGTH_ID, lid, sizeof lid) && lid[0];
        for (const char* p = lid; ok && *p; p++) ok = (*p > 0x20 && *p < 0x7f);
        if (ok) {
            _snprintf(key, sizeof key, "town_quest_length_%s", lid);
            key[sizeof key - 1] = 0;
        }
    }
    if (resolveKey(base, key, raw, sizeof raw)) stripMarkup(raw, txt, sizeof txt);
    else {
        logLine("embark: no localization for \"%s\"", key);
        _snprintf(txt, sizeof txt, "%d", len);
    }
    if (!txt[0]) return;
    _snprintf(out, outsz, axs(AXS_EMB_LENGTH_FMT), txt);
    out[outsz - 1] = 0;
}

static void embDifficultySentence(uintptr_t base, uintptr_t quest, char* out, int outsz) {
    out[0] = 0;
    uint32_t diff = 0;
    if (!safeReadU32(quest + EMB_Q_DIFF, &diff)) return;
    char key[64], raw[96], txt[64] = {0};
    _snprintf(key, sizeof key, "town_quest_difficulty_%d", diff);
    key[sizeof key - 1] = 0;
    if (resolveKey(base, key, raw, sizeof raw)) stripMarkup(raw, txt, sizeof txt);
    else {
        logLine("embark: no localization for \"%s\"", key);
        _snprintf(txt, sizeof txt, "%d", diff);
    }
    if (!txt[0]) return;
    _snprintf(out, outsz, axs(AXS_EMB_DIFFICULTY_FMT), txt);
    out[outsz - 1] = 0;
}

static void embSpecifics(uintptr_t base, uintptr_t quest, char* out, int outsz) {
    char lenS[128], diffS[128];
    embLengthSentence(base, quest, lenS, sizeof lenS);
    embDifficultySentence(base, quest, diffS, sizeof diffS);
    _snprintf(out, outsz, "%s%s%s", lenS, (lenS[0] && diffS[0]) ? " " : "", diffS);
    out[outsz - 1] = 0;
}

static uintptr_t embSelectedQuest(uintptr_t base) {
    uintptr_t camp = embCampaign(base);
    if (!camp) return 0;
    int32_t sel = -1;
    if (!safeReadU32(camp + EMB_QSEL_IDX, (uint32_t*)&sel)) return 0;
    uintptr_t beg = 0;
    int n = embQuestVec(camp, &beg);
    if (sel >= 0 && sel < n) return beg + (uintptr_t)sel * EMB_Q_STRIDE;
    int32_t special = -1;
    if (safeReadU32(camp + EMB_QSPECIAL_IDX, (uint32_t*)&special) && special == sel)
        return camp + EMB_QSPECIAL_REC;
    return 0;
}

bool embSelectedQuestFacts(uintptr_t base, char* dungeon, int dsz, char* lengthS, int lsz,
                           char* diffS, int dfsz) {
    dungeon[0] = lengthS[0] = diffS[0] = 0;
    uintptr_t quest = embSelectedQuest(base);
    if (!quest) return false;
    embDungeonName(base, quest, dungeon, dsz);
    embLengthSentence(base, quest, lengthS, lsz);
    embDifficultySentence(base, quest, diffS, dfsz);
    return true;
}

// ---- The list is COLUMNS, one per LOCATION ----
static const int EMB_MAX_COLS = EMB_MAX_ROWS;
struct EmbCols {
    int  n;                                    // locations in use
    char id[EMB_MAX_COLS][32];                 // the dungeon id — the grouping key
    int  cnt[EMB_MAX_COLS];                    // quests in this location
    int  row[EMB_MAX_COLS][EMB_MAX_ROWS];      // -> index into the flat EmbRow array
};

static void embGroup(const EmbRow* rows, int n, EmbCols* cols) {
    memset(cols, 0, sizeof *cols);
    for (int i = 0; i < n; i++) {
        char id[32];
        if (!embDungeonId(rows[i].quest, id, sizeof id)) id[0] = 0;   // no dungeon: its own column
        int c = -1;
        for (int j = 0; j < cols->n; j++)
            if (strcmp(cols->id[j], id) == 0) { c = j; break; }
        if (c < 0) {
            if (cols->n >= EMB_MAX_COLS) {
                logLine("embark: location table full — quest %d has no column", rows[i].qIdx);
                continue;
            }
            c = cols->n++;
            strncpy(cols->id[c], id, sizeof cols->id[c] - 1);
            cols->id[c][sizeof cols->id[c] - 1] = 0;
        }
        if (cols->cnt[c] < EMB_MAX_ROWS) cols->row[c][cols->cnt[c]++] = i;
    }
}

static void embColName(uintptr_t base, const EmbCols* cols, int c, char* out, int outsz) {
    out[0] = 0;
    if (c < 0 || c >= cols->n) return;
    if (!cols->id[c][0]) { strncpy(out, axs(AXS_EMB_UNKNOWN_LOCATION), outsz - 1); out[outsz - 1] = 0; return; }
    embDungeonNameOf(base, cols->id[c], out, outsz);
    if (!out[0]) { strncpy(out, cols->id[c], outsz - 1); out[outsz - 1] = 0; }  // key miss: raw id
}

static void embColLabel(uintptr_t base, const EmbCols* cols, int c, char* out, int outsz) {
    embColName(base, cols, c, out, outsz);
    if (!out[0] || c < 0 || c >= cols->n || !cols->id[c][0]) return;
    int lvl = embMasteryLevel(base, cols->id[c]);
    if (lvl < 0) return;
    char name[EMB_NAME_MAX];
    strncpy(name, out, sizeof name - 1);
    name[sizeof name - 1] = 0;
    _snprintf(out, outsz, axs(AXS_EMB_COL_MASTERY_FMT), name, lvl);
    out[outsz - 1] = 0;
}

// ---- The roaming miniboss (Color of Madness: the Thing from the Stars) ----
static const uintptr_t EMB_ROAM_SENTINEL = 0x240;  // manager+: unordered_map list sentinel ptr
static const uintptr_t EMB_ROAM_KEY      = 0x10;   // node+: key = DungeonClass* (id inline at +0x00)
static const uintptr_t EMB_ROAM_VEC      = 0x18;   // node+: vector<char[0x40]> begin (end at +0x20)
static const int       EMB_ROAM_ID_SZ    = 0x40;   // one roamer id slot ("thing"), inline

static void embRoamingSuffix(uintptr_t base, const char* dungeonId, char* out, int outsz) {
    out[0] = 0;
    if (!dungeonId || !dungeonId[0]) return;
    uintptr_t mgr = 0, sent = 0, node = 0;
    if (!safeReadPtr(base + EMB_ROAMMGR_RVA, &mgr) || mgr <= 0x10000) return;
    if (!safeReadPtr(mgr + EMB_ROAM_SENTINEL, &sent) || !sent) return;
    if (!safeReadPtr(sent, &node)) return;
    int used = 0;
    for (int guard = 0; node && node != sent && guard < 64; guard++) {
        uintptr_t key = 0;
        char id[32];
        if (!safeReadPtr(node + EMB_ROAM_KEY, &key)) return;
        if (key > 0x10000 && safeReadCStr(key, id, sizeof id) && strcmp(id, dungeonId) == 0) {
            uintptr_t vb = 0, ve = 0;
            if (!safeReadPtr(node + EMB_ROAM_VEC, &vb) ||
                !safeReadPtr(node + EMB_ROAM_VEC + 8, &ve) || !vb || ve <= vb) return;
            int cnt = (int)((ve - vb) / EMB_ROAM_ID_SZ);
            if (cnt > 8) cnt = 8;                    // one roamer ships; 8 covers any mod's worth
            for (int i = 0; i < cnt; i++) {
                char rid[EMB_ROAM_ID_SZ], rkey[96], raw[512], txt[512];
                if (!safeReadCStr(vb + (uintptr_t)i * EMB_ROAM_ID_SZ, rid, sizeof rid) || !rid[0])
                    continue;
                _snprintf(rkey, sizeof rkey, "str_roaming_tooltip_%s", rid);
                rkey[sizeof rkey - 1] = 0;
                if (!resolveKey(base, rkey, raw, sizeof raw)) {
                    logLine("embark: no localization for \"%s\" — roamer not spoken", rkey);
                    continue;                        // parity: the game's tooltip would be broken too
                }
                stripMarkup(raw, txt, sizeof txt);   // the shipped strings are two lines; fold them
                if (!txt[0]) continue;
                int n = _snprintf(out + used, outsz - used, "%s%s", used ? " " : "", txt);
                if (n < 0) { out[outsz - 1] = 0; return; }
                used += n;
            }
            return;
        }
        if (!safeReadPtr(node, &node)) return;
    }
}

// ---- Crimson Court INVITATIONS: the region's count and a quest's cost ----
static const uintptr_t EMB_Q_ISPLOT        = 0x104;
static const uintptr_t EMB_Q_ID            = 0x40;   // quest+: int quest id (== the definition's +0x40)
static const uintptr_t EMB_PQ_CONSUME      = 0x58;   // plot def+: char, `consume_on_attempt`
static const uintptr_t EMB_PQ_DEPSYS       = 0x60;
static const uintptr_t EMB_PQ_PROGRESSION  = 0x3e0;
static const uintptr_t EMB_CAMP_ESTATE_INV = 0xf58;  // Campaign+: the ESTATE Inventory::System
static const int       EMB_DEP_MAX_STACKS  = 16;     // a requirement ships one stack; a mod's worth

static uintptr_t embPlotDef(uintptr_t base, uintptr_t quest) {
    uint8_t isPlot = 0;
    if (!safeReadU8(quest + EMB_Q_ISPLOT, &isPlot) || !isPlot) return 0;
    uint32_t qid = 0;
    if (!safeReadU32(quest + EMB_Q_ID, &qid)) return 0;
    uintptr_t owner = 0, b = 0, e = 0;
    if (!safeReadPtr(base + QT_LOGVEC_RVA, &owner) || owner <= 0x10000 ||
        !safeReadPtr(owner + QT_LOGVEC_BEG, &b) || !safeReadPtr(owner + QT_LOGVEC_END, &e) ||
        !b || e < b) return 0;
    uintptr_t span = e - b;
    if (span % QT_LOG_STRIDE != 0) {
        logLine("embark: plot-quest library span 0x%llx not a stride multiple — refused",
                (unsigned long long)span);
        return 0;
    }
    int n = (int)(span / QT_LOG_STRIDE);
    if (n < 0 || n > QT_LOG_MAX) { logLine("embark: plot-quest library count %d out of range — refused", n); return 0; }
    uintptr_t hit = 0;
    for (int i = 0; i < n; i++) {
        uintptr_t def = b + (uintptr_t)i * QT_LOG_STRIDE;
        uint32_t did = 0;
        if (safeReadU32(def + QT_LOG_QUESTID_OFF, &did) && did == qid) hit = def;
    }
    return hit;
}

static bool embOccupiedStacks(uintptr_t system, uintptr_t* out, int max, int* nOut) {
    *nOut = 0;
    uintptr_t b = 0, e = 0;
    if (!safeReadPtr(system + INV_ITEMS_BEG_OFF, &b) || !safeReadPtr(system + INV_ITEMS_END_OFF, &e))
        return false;
    if (!b || e <= b) return true;                       // nothing allocated: zero stacks
    uintptr_t beg = 0; int slots = 0;
    if (!invItemVectorAt(system, &beg, &slots)) return false;
    for (int i = 0; i < slots && *nOut < max; i++) {
        uintptr_t item = beg + (uintptr_t)i * ITEM_STRIDE;
        int32_t amt = 0;
        if (!safeReadU32(item + ITEM_AMOUNT_OFF, (uint32_t*)&amt)) return false;
        if (amt < 1) continue;                            // the game's own empty test
        out[(*nOut)++] = item;
    }
    return true;
}

static bool embSumKind(uintptr_t req, const uintptr_t* items, int n, long long* sum) {
    uint32_t th = 0, ih = 0;
    if (!safeReadU32(req + ITEM_TYPEHASH_OFF, &th) || !safeReadU32(req + ITEM_IDHASH_OFF, &ih)) return false;
    *sum = 0;
    for (int i = 0; i < n; i++) {
        uint32_t t = 0, d = 0; int32_t amt = 0;
        if (!safeReadU32(items[i] + ITEM_TYPEHASH_OFF, &t) || !safeReadU32(items[i] + ITEM_IDHASH_OFF, &d) ||
            !safeReadU32(items[i] + ITEM_AMOUNT_OFF, (uint32_t*)&amt)) return false;
        if (t == th && d == ih) *sum += amt;
    }
    return true;
}

static bool embDependencyCount(uintptr_t base, uintptr_t def, int* countOut, bool probe) {
    uintptr_t camp = embCampaign(base);
    if (!camp) return false;
    uintptr_t req[EMB_DEP_MAX_STACKS], est[EMB_DEP_MAX_STACKS * 4];
    int nreq = 0, nest = 0;
    if (!embOccupiedStacks(def + EMB_PQ_DEPSYS, req, EMB_DEP_MAX_STACKS, &nreq)) {
        logLine("embark: requirement vector of plot def %p unreadable", (void*)def);
        return false;
    }
    if (!embOccupiedStacks(camp + EMB_CAMP_ESTATE_INV, est, EMB_DEP_MAX_STACKS * 4, &nest)) {
        logLine("embark: estate inventory (Campaign+0x%llx) unreadable", (unsigned long long)EMB_CAMP_ESTATE_INV);
        return false;
    }
    if (probe) {
        for (int i = 0; i < nest; i++) {
            char type[64], id[64], key[192], name[256]; int32_t amt = 0;
            invItemName(base, est[i], type, id, key, name);
            safeReadU32(est[i] + ITEM_AMOUNT_OFF, (uint32_t*)&amt);
            logLine("embark probe: estate stack %d: %s%s%s x%d -> \"%s\"", i, type, id[0] ? " " : "", id, amt, name);
        }
    }
    int result = -1;                                     // -1 = nothing required so far
    for (int i = 0; i < nreq; i++) {
        long long need = 0, have = 0;
        if (!embSumKind(req[i], req, nreq, &need) || !embSumKind(req[i], est, nest, &have)) return false;
        if (probe) {
            char type[64], id[64], key[192], name[256];
            invItemName(base, req[i], type, id, key, name);
            logLine("embark probe: requirement stack %d: %s%s%s need=%lld have=%lld -> \"%s\"",
                    i, type, id[0] ? " " : "", id, need, have, name);
        }
        if (need <= 0) continue;                         // cannot happen: req[i] is one of them
        int times = (have < need) ? 0 : (int)(have / need);
        if (result < 0 || times < result) result = times;
        if (result == 0) break;                          // the game returns 0 the moment a stack is short
    }
    *countOut = (result < 0) ? 1 : result;               // nothing required = payable once
    return true;
}

static bool embColInvites(uintptr_t base, const EmbRow* rows, const EmbCols* cols, int c,
                          int* countOut, bool probe) {
    if (c < 0 || c >= cols->n) return false;
    uintptr_t camp = embCampaign(base);
    if (!camp) return false;
    uintptr_t def = 0;
    for (int i = 0; i < cols->cnt[c]; i++) {
        const EmbRow* r = &rows[cols->row[c][i]];
        if (r->quest == camp + EMB_QSPECIAL_REC) continue; // parity: the drawer walks the vector only
        uintptr_t d = embPlotDef(base, r->quest);
        uint8_t prog = 0;
        if (d && safeReadU8(d + EMB_PQ_PROGRESSION, &prog) && prog) def = d;   // last wins
    }
    if (!def) return false;
    uint8_t consume = 0;
    if (!safeReadU8(def + EMB_PQ_CONSUME, &consume) || !consume) return false;
    return embDependencyCount(base, def, countOut, probe);
}

static int embQuestCost(uintptr_t base, uintptr_t quest) {
    uintptr_t def = embPlotDef(base, quest);
    if (!def) return 0;
    uint8_t consume = 0;
    if (!safeReadU8(def + EMB_PQ_CONSUME, &consume) || !consume) return 0;
    uintptr_t req[EMB_DEP_MAX_STACKS]; int nreq = 0;
    if (!embOccupiedStacks(def + EMB_PQ_DEPSYS, req, EMB_DEP_MAX_STACKS, &nreq)) return 0;
    long long total = 0;
    for (int i = 0; i < nreq; i++) {
        int32_t amt = 0;
        if (safeReadU32(req[i] + ITEM_AMOUNT_OFF, (uint32_t*)&amt) && amt > 0) total += amt;
    }
    return (total > 0x7fffffff) ? 0x7fffffff : (int)total;
}

// The cost as a whole sentence ("Costs 1 invitation."), or empty.
static void embRowCost(uintptr_t base, uintptr_t quest, char* out, int outsz) {
    out[0] = 0;
    int cost = embQuestCost(base, quest);
    if (cost <= 0) return;
    _snprintf(out, outsz, axs(cost == 1 ? AXS_EMB_ROW_COST_1_FMT : AXS_EMB_ROW_COST_N_FMT), cost);
    out[outsz - 1] = 0;
}

static const int EMB_TIP_LINE_SZ = 1536;

static int embRequirementLines(uintptr_t base, uintptr_t quest, char lines[][EMB_TIP_LINE_SZ], int max) {
    uintptr_t def = embPlotDef(base, quest);
    if (!def) return 0;
    uint8_t consume = 0;
    if (!safeReadU8(def + EMB_PQ_CONSUME, &consume) || !consume) return 0;
    uintptr_t req[EMB_DEP_MAX_STACKS]; int nreq = 0;
    if (!embOccupiedStacks(def + EMB_PQ_DEPSYS, req, EMB_DEP_MAX_STACKS, &nreq)) return 0;
    int n = 0;
    for (int i = 0; i < nreq && n < max; i++) {
        char type[64], id[64], key[192], name[256]; int32_t amt = 0;
        if (!invItemName(base, req[i], type, id, key, name))
            logLine("embark: requirement item key \"%s\" did not resolve — speaking the raw id", key);
        safeReadU32(req[i] + ITEM_AMOUNT_OFF, (uint32_t*)&amt);
        if (amt > 1) _snprintf(lines[n], EMB_TIP_LINE_SZ, axs(AXS_EMB_REQUIRES_N_FMT), name, (unsigned)amt);
        else         _snprintf(lines[n], EMB_TIP_LINE_SZ, axs(AXS_EMB_REQUIRES_FMT), name);
        lines[n][EMB_TIP_LINE_SZ - 1] = 0;
        n++;
    }
    return n;
}

static void embClamp(const EmbCols* cols) {
    if (cols->n <= 0) { g_embCol = 0; g_embRow = 0; return; }
    axStepCursor(&g_embCol, cols->n, 0);
    axStepCursor(&g_embRow, cols->cnt[g_embCol], 0);
}

static const EmbRow* embFocus(uintptr_t base, EmbRow* rows, EmbCols* cols) {
    int n = embCollect(base, rows, EMB_MAX_ROWS);
    embGroup(rows, n, cols);
    if (cols->n <= 0) return nullptr;
    embClamp(cols);
    if (cols->cnt[g_embCol] <= 0) return nullptr;
    return &rows[cols->row[g_embCol][g_embRow]];
}

static void embRowText(uintptr_t base, const EmbRow* r, int pos, int count, char* out, int outsz) {
    char name[EMB_NAME_MAX], specs[192], cost[96];
    embQuestName(base, r->quest, name, sizeof name);
    embSpecifics(base, r->quest, specs, sizeof specs);
    embRowCost(base, r->quest, cost, sizeof cost);
    int32_t sel = -1;
    uintptr_t camp = embCampaign(base);
    if (camp) safeReadU32(camp + EMB_QSEL_IDX, (uint32_t*)&sel);
    char posS[64];
    _snprintf(posS, sizeof posS, axs(AXS_EMB_ROW_POS_FMT), pos + 1, count);
    posS[sizeof posS - 1] = 0;
    _snprintf(out, outsz, "%s.%s%s%s%s%s%s %s",
              name,
              specs[0] ? " " : "", specs,          // embSpecifics self-terminates its sentences
              cost[0] ? " " : "", cost,            // a whole sentence, or empty
              sel == r->qIdx ? " " : "", sel == r->qIdx ? axs(AXS_EMB_ROW_SELECTED) : "",
              posS);
    out[outsz - 1] = 0;
}

static void embSpeakRow(uintptr_t base, const char* prefix, bool withCol) {
    EmbRow rows[EMB_MAX_ROWS];
    EmbCols cols;
    const EmbRow* r = embFocus(base, rows, &cols);
    if (!r) { postSpeech(axs(AXS_EMB_NO_QUESTS)); return; }
    char loc[EMB_NAME_MAX];
    loc[0] = 0;
    char inv[96], roam[512];
    inv[0] = 0;
    roam[0] = 0;
    if (withCol) {
        embColLabel(base, &cols, g_embCol, loc, sizeof loc);
        int invites = 0;
        if (loc[0] && embColInvites(base, rows, &cols, g_embCol, &invites, false)) {
            _snprintf(inv, sizeof inv, axs(AXS_EMB_COL_INVITES_FMT), invites);
            inv[sizeof inv - 1] = 0;
        }
        if (loc[0]) embRoamingSuffix(base, cols.id[g_embCol], roam, sizeof roam);
    }
    char row[MAILBOX_SZ], utter[MAILBOX_SZ];
    embRowText(base, r, g_embRow, cols.cnt[g_embCol], row, sizeof row);
    _snprintf(utter, sizeof utter, "%s%s%s%s%s%s%s%s%s", prefix ? prefix : "", prefix ? " " : "",
              loc, loc[0] ? ". " : "", inv, inv[0] ? " " : "", roam, roam[0] ? " " : "", row);
    utter[sizeof utter - 1] = 0;
    postSpeech(utter);
}

static void embTitle(uintptr_t base, char* out, int outsz) {
    char inf[224];
    infEstateText(base, inf, sizeof inf);
    if (inf[0]) _snprintf(out, outsz, "%s %s.", axs(AXS_EMB_TITLE), inf);
    else        _snprintf(out, outsz, "%s", axs(AXS_EMB_TITLE));
    out[outsz - 1] = 0;
}

void embReannounce(uintptr_t base) {
    if (!g_embActive) return;
    logLine("embark: re-announcing after a modal closed");
    char title[288];
    embTitle(base, title, sizeof title);
    embSpeakRow(base, title, true);
}

static const uintptr_t EMB_Q_REWARDS = 0x160;          // quest+: embedded Inventory::System
static int embTipLines(uintptr_t base, uintptr_t quest, char lines[][EMB_TIP_LINE_SZ], int max) {
    int n = 0;
    char id[0x100], key[0x140], raw[512];
    if (n < max) {
        char name[EMB_NAME_MAX], dungeon[EMB_NAME_MAX], specs[192], cost[96];
        embQuestName(base, quest, name, sizeof name);
        embDungeonName(base, quest, dungeon, sizeof dungeon);
        embSpecifics(base, quest, specs, sizeof specs);
        embRowCost(base, quest, cost, sizeof cost);
        _snprintf(lines[n], EMB_TIP_LINE_SZ, "%s.%s%s%s%s%s%s%s",
                  name, dungeon[0] ? " " : "", dungeon, dungeon[0] ? "." : "",
                  specs[0] ? " " : "", specs, cost[0] ? " " : "", cost);
        lines[n][EMB_TIP_LINE_SZ - 1] = 0;
        n++;
    }
    // Goals next — the first thing a fresh Ctrl+Up reaches.
    uintptr_t gb = 0, ge = 0;
    if (safeReadPtr(quest + EMB_Q_GOALS_BEG, &gb) && safeReadPtr(quest + EMB_Q_GOALS_END, &ge) &&
        gb && ge > gb && (ge - gb) % 4u == 0) {
        int ng = (int)((ge - gb) / 4u);
        if (ng > 8) ng = 8;
        for (int i = 0; i < ng && n < max; i++) {
            uint32_t gid = 0;
            if (!safeReadU32(gb + (uintptr_t)i * 4u, &gid)) break;
            if (qtGoalText(base, gid, lines[n], EMB_TIP_LINE_SZ)) n++;
        }
    }
    if (n < max && embNameId(base, quest, id, sizeof id)) {
        _snprintf(key, sizeof key, "town_quest_description_%s", id);
        key[sizeof key - 1] = 0;
        if (resolveKey(base, key, raw, sizeof raw)) {
            stripMarkup(raw, lines[n], EMB_TIP_LINE_SZ);
            if (lines[n][0]) n++;
        }
    }
    uintptr_t rb = 0; int rslots = 0;
    if (invItemVectorAt(quest + EMB_Q_REWARDS, &rb, &rslots)) {
        for (int i = 0; i < rslots && n < max; i++) {
            uintptr_t item = rb + (uintptr_t)i * ITEM_STRIDE;
            uint32_t amount = 0;
            if (!safeReadU32(item + ITEM_AMOUNT_OFF, &amount) || (int32_t)amount < 1) continue;
            char type[64], itemId[64], ikey[192], name[256];
            if (!invItemName(base, item, type, itemId, ikey, name)) continue;
            char head[320];
            if (amount > 1) _snprintf(head, sizeof head, axs(AXS_EMB_REWARD_N_FMT), name, amount);
            else            _snprintf(head, sizeof head, axs(AXS_EMB_REWARD_FMT), name);
            head[sizeof head - 1] = 0;
            if (strcmp(type, "trinket") == 0) {
                char rarity[96] = {0}, clsreq[256] = {0}, fx[768] = {0};
                char charges[384] = {0}, trig[1024] = {0};
                uintptr_t rec = bldTrinketRecord(base, item);
                if (rec) {
                    bldTrinketRarity(base, rec, rarity, sizeof rarity, false);
                    bldTrinketClassReq(base, rec, clsreq, sizeof clsreq, false);
                    bldTrinketCharges(base, item, rec, charges, sizeof charges, false);
                    bldTrinketTriggers(base, rec, trig, sizeof trig, false);
                }
                bldTrinketEffects(base, item, fx, sizeof fx, false);
                _snprintf(lines[n], EMB_TIP_LINE_SZ, "%s%s%s%s%s%s%s%s%s%s%s%s%s%s",
                          head,
                          rarity[0] ? " " : "", rarity, rarity[0] ? "." : "",
                          clsreq[0] ? " " : "", clsreq, clsreq[0] ? "." : "",
                          charges[0] ? " " : "", charges,
                          fx[0] ? " " : "", fx, fx[0] ? "." : "",
                          trig[0] ? " " : "", trig);
            } else {
                _snprintf(lines[n], EMB_TIP_LINE_SZ, "%s", head);
            }
            lines[n][EMB_TIP_LINE_SZ - 1] = 0;
            n++;
        }
    }
    if (n < max) n += embRequirementLines(base, quest, lines + n, max - n);
    return n;
}

bool routeEmbarkKey(uintptr_t base, uint32_t sym, uint16_t mod, uint8_t repeat) {
    if (mod & (KMOD_LALT | KMOD_RALT)) return false;
    bool ctrl = (mod & (KMOD_LCTRL | KMOD_RCTRL)) != 0;

    if (ctrl && (sym == SDLK_UP || sym == SDLK_DOWN)) {
        if (repeat) return true;
        EmbRow rows[EMB_MAX_ROWS];
        EmbCols cols;
        const EmbRow* focus = embFocus(base, rows, &cols);
        if (!focus) { postSpeech(axs(AXS_EMB_NO_QUESTS)); return true; }
        static char lines[EMB_TIP_MAX][EMB_TIP_LINE_SZ];
        int ln = embTipLines(base, focus->quest, lines, EMB_TIP_MAX);
        if (ln <= 1) { postSpeech(axs(AXS_NO_MORE_INFO)); return true; }
        if (g_embTipLine < 0 || g_embTipLine >= ln) g_embTipLine = 0;
        g_embTipLine = (g_embTipLine + ((sym == SDLK_UP) ? 1 : -1) + ln) % ln;
        postSpeech(lines[g_embTipLine]);
        return true;
    }
    if (ctrl) return false;

    int dRow = 0, dCol = 0, jump = 0;
    bool isJump = axDecodeJump(sym, mod, repeat, &jump);   // Home/End: first/last quest of the
    if (isJump) {                                          // CURRENT location (column kept)
        if (!jump) return true;                      // held jump: one landing per press
        dRow = jump;                                 // dCol stays 0: the row clamp is the landing
    }
    if (isJump || axDecodeArrow(sym, mod, repeat, /*wantCols=*/true, &dRow, &dCol)) {
        if (!dRow && !dCol) return true;             // throttled repeat: claimed, no step this tick
        EmbRow rows[EMB_MAX_ROWS];
        EmbCols cols;
        int n = embCollect(base, rows, EMB_MAX_ROWS);
        embGroup(rows, n, &cols);
        if (cols.n <= 0) { postSpeech(axs(AXS_EMB_NO_QUESTS)); return true; }
        embClamp(&cols);                             // re-clamp against the live grouping

        if (dCol) {
            static bool colGateLogged = false;
            if (!colGateLogged) {
                colGateLogged = true;
                char owner[0x44];
                if (kbActionForKey(base, (int)SDLK_LEFT, owner, sizeof owner))
                    logLine("embark GATE: Left is bound to the game action \"%s\"", owner);
                else
                    logLine("embark GATE: Left is not in the keyboard binding table");
                if (kbActionForKey(base, (int)SDLK_RIGHT, owner, sizeof owner))
                    logLine("embark GATE: Right is bound to the game action \"%s\"", owner);
                else
                    logLine("embark GATE: Right is not in the keyboard binding table");
            }
            int from = g_embCol;
            if (axStepCursor(&g_embCol, cols.n, dCol)) {
                g_embRow     = 0;
                g_embTipLine = 0;
            }
            logLine("embark nav location %d -> %d (of %d), %d quests",
                    from, g_embCol, cols.n, cols.cnt[g_embCol]);
            embSpeakRow(base, nullptr, true);        // the location name leads the utterance
            return true;
        }

        int cnt = cols.cnt[g_embCol];
        if (axStepCursor(&g_embRow, cnt, dRow))      // hard stop: re-read the end row
            g_embTipLine = 0;                        // a new row re-places the cursor on line 0
        embSpeakRow(base, nullptr, false);
        return true;
    }

    if (sym == SDLK_RETURN || sym == SDLK_KP_ENTER) {
        if (repeat) return true;
        EmbRow rows[EMB_MAX_ROWS];
        EmbCols cols;
        const EmbRow* r = embFocus(base, rows, &cols);
        if (!r) return true;
        char name[EMB_NAME_MAX];
        embQuestName(base, r->quest, name, sizeof name);
        int32_t sel = -1;
        uintptr_t camp = embCampaign(base);
        if (camp) safeReadU32(camp + EMB_QSEL_IDX, (uint32_t*)&sel);
        char utter[MAILBOX_SZ];
        if (sel == r->qIdx) {
            _snprintf(utter, sizeof utter, axs(AXS_EMB_ALREADY_SELECTED_FMT), name);
            utter[sizeof utter - 1] = 0;
            postSpeech(utter);
            return true;
        }
        if (g_embSelWatchUntil) { postSpeech(axs(AXS_EMB_STILL_SELECTING)); return true; }
        if (frontEndClickElementId((int64_t)(EMB_MARKER_BASE + (uint32_t)r->qIdx))) {
            g_embSelWant = r->qIdx;
            strncpy(g_embSelName, name, sizeof g_embSelName - 1);
            g_embSelName[sizeof g_embSelName - 1] = 0;
            g_embSelWatchUntil = GetTickCount() + 1500;   // checkEmbark announces the outcome
        } else {
            _snprintf(utter, sizeof utter, axs(AXS_EMB_CANT_SELECT_FMT), name);
            utter[sizeof utter - 1] = 0;
            postSpeech(utter);
        }
        return true;
    }

    return false;
}

bool routeEmbarkForward(uintptr_t base, uint8_t repeat) {
    uintptr_t root = resTownRoot(base);
    if (!root) return false;                         // not in town: E stays the game's
    if (repeat) return true;
    if (!g_embKeyLogged) {                           // the gate check, logged with the game's data
        char act[0x44];
        if (kbActionForKey(base, (int)SDLK_e, act, sizeof act))
            logLine("embark: game binds E to \"%s\" (table walk; can under-report)", act);
        else
            logLine("embark: E not found in the game's binding table");
        g_embKeyLogged = true;
    }
    if (g_embFwdWatchUntil) { postSpeech(axs(AXS_STILL_WORKING)); return true; }

    // The button's current label, from the game's own picker (a pure read + strncpy).
    strncpy(g_embFwdLabel, axs(AXS_EMB_FWD_FALLBACK), sizeof g_embFwdLabel - 1);
    char key[0x40];
    key[0] = 0;
    EmbFwdLabelFn fn = (EmbFwdLabelFn)(base + EMB_FWDLABEL_RVA);
    __try { fn(root, key); }
    __except (EXCEPTION_EXECUTE_HANDLER) { key[0] = 0; }
    key[sizeof key - 1] = 0;
    if (key[0]) {
        char raw[64];
        if (resolveKey(base, key, raw, sizeof raw)) {
            stripMarkup(raw, g_embFwdLabel, sizeof g_embFwdLabel);
        } else {
            logLine("embark: forward label key \"%s\" did not resolve", key);
        }
    }

    if (ringForwardAction(base, g_embFwdLabel)) {
        g_embFwdQsBefore   = embQsOpen(root);
        g_embFwdProvBefore = embProvIsOpen(root);
        g_embFwdDlgSeen    = false;
        g_embFwdLayerBefore = -1;
        {   uint32_t layer = 0;
            if (safeReadU32(root + TM_LAYER_OFF, &layer)) g_embFwdLayerBefore = (int)layer;   }
        g_embFwdWatchUntil = GetTickCount() + 2000;
        return true;
    }
    feDumpFocusVector("E: about to click the forward button");
    if (!frontEndClickElementId((int64_t)EMB_FWRD_ID)) {
        char utter[128];
        _snprintf(utter, sizeof utter, axs(AXS_EMB_FWD_UNAVAILABLE_FMT), g_embFwdLabel);
        utter[sizeof utter - 1] = 0;
        postSpeech(utter);
        return true;
    }
    g_embFwdQsBefore   = embQsOpen(root);
    g_embFwdProvBefore = embProvIsOpen(root);
    g_embFwdDlgSeen    = false;
    g_embFwdLayerBefore = -1;
    {   uint32_t layer = 0;
        if (safeReadU32(root + TM_LAYER_OFF, &layer)) g_embFwdLayerBefore = (int)layer;   }
    g_embFwdWatchUntil = GetTickCount() + 2000;
    logLine("embark: E clicked 'fwrd' (label \"%s\", qs=%d prov=%d layer=%d)",
            g_embFwdLabel, g_embFwdQsBefore ? 1 : 0, g_embFwdProvBefore ? 1 : 0,
            g_embFwdLayerBefore);
    return true;
}

static void embProbe(uintptr_t base, uintptr_t root) {
    uintptr_t camp = embCampaign(base);
    int32_t sel = -1, special = -1;
    if (camp) {
        safeReadU32(camp + EMB_QSEL_IDX, (uint32_t*)&sel);
        safeReadU32(camp + EMB_QSPECIAL_IDX, (uint32_t*)&special);
    }
    uint32_t qsState = 0;
    safeReadU32(root + EMB_QS_STATE_OFF, &qsState);
    logLine("embark probe: qsState=%u sel=%d special=%d camp=%p", qsState, sel, special, (void*)camp);
    EmbRow rows[EMB_MAX_ROWS];
    EmbCols cols;
    int n = embCollect(base, rows, EMB_MAX_ROWS);
    embGroup(rows, n, &cols);
    for (int c = 0; c < cols.n; c++) {
        char list[256];
        int  used = 0;
        list[0] = 0;
        for (int i = 0; i < cols.cnt[c] && used < (int)sizeof list - 8; i++)
            used += _snprintf(list + used, sizeof list - used, "%s%d",
                              i ? "," : "", rows[cols.row[c][i]].qIdx);
        list[sizeof list - 1] = 0;
        uint32_t xp = 0;
        bool haveXp = embDungeonXpProbe(base, cols.id[c], &xp);
        logLine("embark probe: column %d dungeon=\"%s\" %d quests (vector idx %s) "
                "hash=%u xp=%s%u mastery=%d",
                c, cols.id[c], cols.cnt[c], list,
                cols.id[c][0] ? resHash(cols.id[c]) : 0,
                haveXp ? "" : "(absent)", xp, embMasteryLevel(base, cols.id[c]));
        int invites = 0;
        bool drawn = embColInvites(base, rows, &cols, c, &invites, true);
        logLine("embark probe: column %d invitations: %s%d", c, drawn ? "" : "(no icon) ", invites);
    }
    uintptr_t pn = embPartyNameRec(base);
    char pnName[EMB_NAME_MAX];
    pnName[0] = 0;
    if (pn) embPartyNameOf(base, pn, pnName, sizeof pnName);
    logLine("embark probe: party combo rec=%p name=\"%s\"", (void*)pn, pnName);
    for (int i = 0; i < n; i++) {
        char id[0x100] = {0}, dungeon[32] = {0};
        uint32_t d = 0, l = 0;
        embNameId(base, rows[i].quest, id, sizeof id);
        uintptr_t def = 0;
        if (safeReadPtr(rows[i].quest + EMB_Q_DUNGEONDEF, &def) && def > 0x10000)
            safeReadCStr(def, dungeon, sizeof dungeon);
        safeReadU32(rows[i].quest + EMB_Q_DIFF, &d);
        safeReadU32(rows[i].quest + EMB_Q_LEN, &l);
        logLine("embark probe: row %d qIdx=%d id=\"%s\" dungeon=\"%s\" len=%u diff=%u elem(0x%x)=%s",
                i, rows[i].qIdx, id, dungeon, l, d,
                EMB_MARKER_BASE + (uint32_t)rows[i].qIdx,
                qtElemOnScreen(EMB_MARKER_BASE + (uint32_t)rows[i].qIdx) ? "onscreen" : "ABSENT");
        uintptr_t pdef = embPlotDef(base, rows[i].quest);
        if (pdef) {
            uint8_t consume = 0, prog = 0, repeat = 0;
            safeReadU8(pdef + EMB_PQ_CONSUME, &consume);
            safeReadU8(pdef + EMB_PQ_PROGRESSION, &prog);
            safeReadU8(pdef + EMB_PQ_PROGRESSION + 1, &repeat);
            logLine("embark probe: row %d plot def=%p consume_on_attempt=%u is_progression=%u "
                    "is_repeatable=%u cost=%d", i, (void*)pdef, consume, prog, repeat,
                    embQuestCost(base, rows[i].quest));
        }
    }
}

// Per-frame: layer edges + the two outcome watches.
void checkEmbark(uintptr_t base) {
    uintptr_t root = resTownRoot(base);
    if (!root) {
        if (g_embActive) { g_embActive = false; logLine("embark: stood down (left town)"); }
        g_embProvOpen = false;
        g_embFwdWatchUntil = 0;                      // leaving town IS the outcome (loading speaks)
        g_embSelWatchUntil = 0;
        g_embCol = 0;
        g_embRow = 0;
        g_embTipLine = 0;
        g_embPnRec = 0;
        g_embPnSeeded = false;
        return;
    }
    bool qs   = embQsOpen(root);
    bool prov = embProvIsOpen(root);

    if (qs) {
        uintptr_t rec = embPartyNameRec(base);
        if (!g_embPnSeeded) {
            g_embPnSeeded = true;
            g_embPnRec    = rec;
        } else if (rec != g_embPnRec) {
            g_embPnRec = rec;
            char name[EMB_NAME_MAX], utter[MAILBOX_SZ];
            if (rec && embPartyNameOf(base, rec, name, sizeof name)) {
                _snprintf(utter, sizeof utter, "%s.", name);
                utter[sizeof utter - 1] = 0;
                logLine("embark: party combo formed -> \"%s\" (rec=%p)", name, (void*)rec);
                postSpeech(utter);
            } else if (rec) {
                logLine("embark: party combo record %p has no readable name", (void*)rec);
            }
        }
    } else if (g_embPnSeeded) {
        g_embPnSeeded = false;
        g_embPnRec    = 0;
    }

    if (g_embProbeAt && GetTickCount() >= g_embProbeAt) {
        g_embProbeAt = 0;
        // Gated on the debug log so the latch is not spent while it is off.
        if (axDebugLogEnabled() && !g_embDumped && qs) { embProbe(base, root); g_embDumped = true; }
    }

    if (g_embFwdWatchUntil) {
        int layerNow = -1;
        {   uint32_t l = 0;
            if (safeReadU32(root + TM_LAYER_OFF, &l)) layerNow = (int)l;   }
        if (qs != g_embFwdQsBefore || prov != g_embFwdProvBefore ||
            (g_embFwdLayerBefore >= 0 && layerNow >= 0 && layerNow != g_embFwdLayerBefore)) {
            g_embFwdWatchUntil = 0;                  // the new layer announces itself: the
            logLine("embark: forward outcome observed (qs %d->%d prov %d->%d layer %d->%d)",
                    g_embFwdQsBefore ? 1 : 0, qs ? 1 : 0, g_embFwdProvBefore ? 1 : 0, prov ? 1 : 0,
                    g_embFwdLayerBefore, layerNow);
        } else if (axIsDialog()) {
            g_embFwdDlgSeen = true;                  // the dialog reader is speaking; wait it out
        } else if (g_embFwdDlgSeen) {
            g_embFwdWatchUntil = 0;
            logLine("embark: forward dialog closed without a layer change");
            postSpeech(axs(AXS_CANCELLED));
        } else if (GetTickCount() > g_embFwdWatchUntil) {
            g_embFwdWatchUntil = 0;
            char utter[128];
            _snprintf(utter, sizeof utter, axs(AXS_EMB_FWD_NO_RESPONSE_FMT), g_embFwdLabel);
            utter[sizeof utter - 1] = 0;
            logLine("embark: forward click made no observable change");
            postSpeech(utter);
        }
    }

    // Enter's outcome: the campaign's selected index moving to the clicked quest.
    if (g_embSelWatchUntil) {
        uintptr_t camp = embCampaign(base);
        int32_t sel = -1;
        if (camp) safeReadU32(camp + EMB_QSEL_IDX, (uint32_t*)&sel);
        if (sel == g_embSelWant) {
            g_embSelWatchUntil = 0;
            char utter[MAILBOX_SZ];
            _snprintf(utter, sizeof utter, axs(AXS_EMB_SELECTED_FMT), g_embSelName);
            utter[sizeof utter - 1] = 0;
            logLine("embark: selection observed -> %d", sel);
            postSpeech(utter);
        } else if (GetTickCount() > g_embSelWatchUntil) {
            g_embSelWatchUntil = 0;
            logLine("embark: marker click made no observable change (sel=%d want=%d)",
                    sel, g_embSelWant);
            postSpeech(axs(AXS_EMB_SEL_FAILED));
        }
    }

    g_embProvOpen = prov;

    bool on = qs && !prov && !g_resActive && !g_ptyActive && !g_rosActive && !csTownSheetPanel(base);
    if (on && !g_embActive) {
        g_embActive = true;
        uintptr_t camp = embCampaign(base);
        int32_t sel = -1;
        if (camp) safeReadU32(camp + EMB_QSEL_IDX, (uint32_t*)&sel);
        EmbRow rows[EMB_MAX_ROWS];
        EmbCols cols;
        int n = embCollect(base, rows, EMB_MAX_ROWS);
        embGroup(rows, n, &cols);
        g_embCol = 0;
        g_embRow = 0;
        for (int c = 0; c < cols.n; c++)
            for (int i = 0; i < cols.cnt[c]; i++)
                if (rows[cols.row[c][i]].qIdx == sel) { g_embCol = c; g_embRow = i; c = cols.n; break; }
        g_embTipLine = 0;
        if (!g_embDumped) g_embProbeAt = GetTickCount() + 400;
        logLine("embark: active, %d quests in %d locations, cursor %d/%d",
                n, cols.n, g_embCol, g_embRow);
        char title[288];                              // header + the infestation banner (embTitle)
        embTitle(base, title, sizeof title);
        embSpeakRow(base, title, true);               // one utterance: header + location + row
    } else if (!on && g_embActive) {
        g_embActive = false;
        logLine("embark: stood down (qs=%d prov=%d)", qs ? 1 : 0, prov ? 1 : 0);
    }
}
