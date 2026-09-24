// raid/quest.cpp -- THE QUEST TRACKER ZONE, THE RETREAT CONTROL AND THE QUEST-COMPLETE POPUP

#include <cstdint>
#include <cstring>
#include <cstdio>
#include "internal.h"

// ---- QUEST TRACKER ZONE (the raid HUD's goals section) ----

static const uintptr_t QT_QUEST_OFF        = 0x52c0;  // raidRoot+ -> Quest* (was 0x5250)
static const uintptr_t QT_GOALVEC_BEG      = 0x120;   // Quest+0x120/+0x128: vector<uint32> goal ids
static const uintptr_t QT_GOALVEC_END      = 0x128;
static const uintptr_t QT_QUEST_ID_OFF     = 0x40;    // Quest+0x40: matched against a log entry
static const uintptr_t QT_QUEST_REGROUP_OFF = 0x104;  // Quest+0x104: byte gating the regroup wording
static const uintptr_t QT_QUEST_LEVEL_OFF   = 0x118;
static const int       QT_MAX_GOALS        = 12;      // sanity cap on a length read from memory

typedef char* (*QtGoalDescFn)(char* out, uint32_t goalId);
static const int       QT_GOALDESC_BUF     = 0x100;   // the describer's OWN fixed size — not ours

// The abandon-vs-finish test, mirroring ShowRetreatButton's choice of tooltip key.
static const uintptr_t QT_COMBAT_OFF       = 0x4b20;
static const uintptr_t QT_LOG_DONE_OFF     = 0x3f4;

static const uint32_t  QT_CREST_ID         = 0x716e666fu;  // "qnfo" — the crest: return to town
static const uint32_t  QT_PANELTAB_ID      = 0x72746162u;  // "rtab" — the HUD panel tab. NOT ours.
static const uint32_t  QT_RETREAT_ID       = 0x72747274u;  // "rtrt" — the real retreat button
static const uint32_t  QT_HAMLET_ID        = 0x79657320u;  // "yes " — Return to Hamlet
static const uint32_t  QT_CONTINUE_ID      = 0x6e6f2020u;  // "no  " — Continue Raid

// ---- The panel's own answer to "is the quest over?" ----
static const uintptr_t QT_PANEL_STATE_OFF = 0x148;   // QuestInfo+: 0 = normal, 1..3 = complete
static const uintptr_t QT_PANEL_SEAL_OFF  = 0x14c;
static uintptr_t qtFindPanel(uintptr_t base, int* routeOut, uintptr_t* offOut);

static int  g_qtCursor = -1;
static int  g_qtRoomReturn = -1;

static uintptr_t g_qcCompletedRoot = 0;

bool qtElemOnScreen(uint32_t id) {
    uintptr_t begin = 0, end = 0;
    if (!safeReadPtr(g_base + VEC_BEGIN_RVA, &begin) ||
        !safeReadPtr(g_base + VEC_END_RVA, &end) || !begin || end <= begin) return false;
    uintptr_t count = (end - begin) / ELEM_STRIDE;
    if (count > 512) count = 512;                    // a wild length means we read the wrong thing
    for (uintptr_t i = 0; i < count; i++) {
        int64_t eid = 0;
        if (!safeReadI64(begin + i * ELEM_STRIDE + ELEM_ID_OFF, &eid)) continue;
        if ((uint32_t)(uint64_t)eid == id) return true;
    }
    return false;
}

// The live Quest, or 0 outside a raid.
static uintptr_t qtQuest(uintptr_t base) {
    uintptr_t root = mapRoot(base);
    if (!root) return 0;
    uintptr_t q = 0;
    if (!safeReadPtr(root + QT_QUEST_OFF, &q) || q <= 0x10000) return 0;
    return q;
}

int qtQuestLevel(uintptr_t base) {
    uintptr_t q = qtQuest(base);
    if (!q) return -1;
    uint32_t lvl = 0;
    if (!safeReadU32(q + QT_QUEST_LEVEL_OFF, &lvl)) {
        logLine("quest: level unreadable at Quest+0x%x", (unsigned)QT_QUEST_LEVEL_OFF);
        return -1;
    }
    if (lvl > 32u) { logLine("quest: level reads %u -- refused", lvl); return -1; }
    return (int)lvl;
}

static int qtGoalIds(uintptr_t base, uint32_t* out, int max) {
    uintptr_t q = qtQuest(base);
    if (!q) return 0;
    uintptr_t b = 0, e = 0;
    if (!safeReadPtr(q + QT_GOALVEC_BEG, &b) || !safeReadPtr(q + QT_GOALVEC_END, &e)) return 0;
    if (!b || e < b) return 0;
    uintptr_t span = e - b;
    if (span % 4u != 0) { logLine("quest: goal vector span %llu not a multiple of 4 — refused",
                                  (unsigned long long)span); return 0; }
    int n = (int)(span / 4u);
    if (n < 0 || n > QT_MAX_GOALS) { logLine("quest: goal count %d out of range — refused", n); return 0; }
    if (n > max) n = max;
    for (int i = 0; i < n; i++)
        if (!safeReadU32(b + (uintptr_t)i * 4u, &out[i])) return i;
    return n;
}

// One goal, as the game's own localized sentence.
bool qtGoalText(uintptr_t base, uint32_t goalId, char* out, int outsz) {
    char buf[QT_GOALDESC_BUF];
    memset(buf, 0, sizeof buf);
    QtGoalDescFn fn = (QtGoalDescFn)(base + QT_GOALDESC_RVA);
    __try { fn(buf, goalId); }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        logLine("quest: goal describer FAULTED on id=%u", goalId);
        return false;
    }
    buf[QT_GOALDESC_BUF - 1] = 0;
    if (!buf[0]) return false;
    stripMarkup(buf, out, outsz);
    return out[0] != 0;
}

static int qtQuestComplete(uintptr_t base) {
    uintptr_t root = mapRoot(base);
    if (!root) return -1;
    uint32_t inBattle = 0;
    if (!safeReadU32(root + QT_COMBAT_OFF, &inBattle)) {
        logLine("quest: complete? battle flag unreadable"); return -1;
    }
    if (inBattle != 0) return 0;

    uintptr_t q = qtQuest(base);
    if (!q) return -1;
    uint8_t gate = 0;
    if (!safeReadU8(q + QT_QUEST_REGROUP_OFF, &gate)) {
        logLine("quest: complete? Quest+0x104 unreadable"); return -1;
    }
    if (!gate) { logLine("quest: complete? Quest+0x104 == 0 — no regroup wording for this quest"); return 0; }

    uint32_t qid = 0;
    if (!safeReadU32(q + QT_QUEST_ID_OFF, &qid)) {
        logLine("quest: complete? Quest+0x40 unreadable"); return -1;
    }
    uintptr_t owner = 0, b = 0, e = 0;
    if (!safeReadPtr(base + QT_LOGVEC_RVA, &owner) || owner <= 0x10000 ||
        !safeReadPtr(owner + QT_LOGVEC_BEG, &b) || !safeReadPtr(owner + QT_LOGVEC_END, &e) ||
        !b || e < b) {
        logLine("quest: complete? log vector unreadable"); return -1;
    }
    uintptr_t span = e - b;
    if (span % QT_LOG_STRIDE != 0) {
        logLine("quest: complete? log span %llu not a multiple of 0x%llx — refused",
                (unsigned long long)span, (unsigned long long)QT_LOG_STRIDE);
        return -1;
    }
    int n = (int)(span / QT_LOG_STRIDE);
    if (n < 0 || n > QT_LOG_MAX) {
        logLine("quest: complete? log count %d out of range — refused", n); return -1;
    }
    uintptr_t hit = 0;
    for (int i = 0; i < n; i++) {
        uintptr_t ent = b + (uintptr_t)i * QT_LOG_STRIDE;
        uint32_t eid = 0;
        if (!safeReadU32(ent + QT_LOG_QUESTID_OFF, &eid)) continue;
        if (eid == qid) hit = ent;
    }
    if (!hit) { logLine("quest: complete? no log entry for quest id %u of %d", qid, n); return -1; }
    uint8_t done = 0;
    if (!safeReadU8(hit + QT_LOG_DONE_OFF, &done)) {
        logLine("quest: complete? entry+0x3a4 unreadable"); return -1;
    }
    if (!done) logLine("quest: complete? log entry for quest %u has +0x3a4 == 0", qid);
    return done ? 1 : 0;
}

// ---- THE RETREAT CONTROL: the game's own gates, and the game's own handler ----
static const uintptr_t QT_QUEST_KIND_OFF      = 0x108;
static const uintptr_t QT_PANEL_LOADED_OFF    = 0x130;
static const uintptr_t QT_RAID_SUBMODE_OFF    = 0x5118;     // raidRoot+: != 0 = a sub-mode runs (was 0x50a8)
static const uintptr_t QT_RAID_HUD_OFF        = 0x563c;     // raidRoot+: 0 = no HUD (was 0x55cc)
static const uintptr_t QT_LOG_RETREATOK_OFF   = 0x3e9;      // logEntry+: 0 = no retreat here (was 0x399)
static const uintptr_t QT_LOG_ASKFIRST_OFF    = 0x3ea;      // logEntry+: ask before fleeing (was 0x39a)

static uintptr_t qtLogEntry(uintptr_t base) {
    uintptr_t q = qtQuest(base);
    if (!q) return 0;
    uint8_t gate = 0;
    if (!safeReadU8(q + QT_QUEST_REGROUP_OFF, &gate) || !gate) return 0;
    uint32_t qid = 0;
    if (!safeReadU32(q + QT_QUEST_ID_OFF, &qid)) return 0;
    uintptr_t owner = 0, b = 0, e = 0;
    if (!safeReadPtr(base + QT_LOGVEC_RVA, &owner) || owner <= 0x10000 ||
        !safeReadPtr(owner + QT_LOGVEC_BEG, &b) || !safeReadPtr(owner + QT_LOGVEC_END, &e) ||
        !b || e < b) return 0;
    uintptr_t span = e - b;
    if (span % QT_LOG_STRIDE != 0) return 0;
    int n = (int)(span / QT_LOG_STRIDE);
    if (n < 0 || n > QT_LOG_MAX) return 0;
    uintptr_t hit = 0;
    for (int i = 0; i < n; i++) {
        uintptr_t ent = b + (uintptr_t)i * QT_LOG_STRIDE;
        uint32_t eid = 0;
        if (!safeReadU32(ent + QT_LOG_QUESTID_OFF, &eid)) continue;
        if (eid == qid) hit = ent;
    }
    return hit;
}

static bool qtIsWaveQuest(uintptr_t base) {
    uintptr_t q = qtQuest(base);
    if (!q) return false;
    uintptr_t kind = 0;
    if (!safeReadPtr(q + QT_QUEST_KIND_OFF, &kind) || kind <= 0x10000) return false;
    uint32_t got = 0, want = 0;
    if (!safeReadU32(kind + QT_QUEST_ID_OFF, &got)) return false;
    if (!safeReadU32(base + QT_WAVE_QID_RVA, &want)) return false;
    return got == want;
}

// In a battle right now?
static bool qtInBattle(uintptr_t base) {
    uintptr_t root = mapRoot(base);
    uint32_t f = 0;
    return root && safeReadU32(root + QT_COMBAT_OFF, &f) && f != 0;
}

static bool qtRetreatAvailable(uintptr_t base, const char** why) {
    const char* dummy = nullptr;
    if (!why) why = &dummy;
    *why = nullptr;

    uintptr_t root = mapRoot(base);
    if (!root)  { *why = "not in a raid"; return false; }
    uintptr_t q = qtQuest(base);
    if (!q)     { *why = "no quest"; return false; }

    if (g_qcReturning) { *why = "the return to the Hamlet is under way"; return false; }

    // 1. A specific quest may never be abandoned.
    uint8_t inLog = 0;
    uint32_t qid = 0, noAbandon = 0;
    if (safeReadU8(q + QT_QUEST_REGROUP_OFF, &inLog) && inLog &&
        safeReadU32(q + QT_QUEST_ID_OFF, &qid) &&
        safeReadU32(base + QT_NOABANDON_QID_RVA, &noAbandon) && qid == noAbandon) {
        *why = "this quest cannot be abandoned"; return false;
    }
    if (!qtInBattle(base)) {
        uintptr_t panel = qtFindPanel(base, nullptr, nullptr);
        uint8_t loaded = 0;
        if (!panel || !safeReadU8(panel + QT_PANEL_LOADED_OFF, &loaded) || !loaded) {
            *why = "the quest panel is not ready"; return false;
        }
    }
    // 3. The quest-log entry can withhold the button.
    uintptr_t ent = qtLogEntry(base);
    if (ent) {
        uint8_t ok = 0;
        if (!safeReadU8(ent + QT_LOG_RETREATOK_OFF, &ok) || !ok) {
            *why = "the quest log withholds it"; return false;
        }
    }
    // 4./5. A raid sub-mode is running, or the HUD is not up.
    uint32_t sub = 0; uint8_t hud = 0;
    if (!safeReadU32(root + QT_RAID_SUBMODE_OFF, &sub) || sub != 0) {
        *why = "the raid is busy"; return false;
    }
    if (!safeReadU8(root + QT_RAID_HUD_OFF, &hud) || !hud) {
        *why = "the raid HUD is down"; return false;
    }
    return true;
}

static bool qtRetreatAsksFirst(uintptr_t base) {
    if (qtIsWaveQuest(base) || !qtInBattle(base)) return true;
    uintptr_t ent = qtLogEntry(base);
    uint8_t ask = 0;
    return ent && safeReadU8(ent + QT_LOG_ASKFIRST_OFF, &ask) && ask != 0;
}

static bool qtRetreatActivate(uintptr_t base) {
    if (confirmDialogOpen(base)) {
        logLine("quest retreat: a confirm dialog is already up — not raising a second one");
        return true;                      // the dialog layer owns the answer from here
    }
    uintptr_t panel = qtFindPanel(base, nullptr, nullptr);
    uintptr_t root  = mapRoot(base);
    uintptr_t screen = 0;
    if (!panel || !root) {
        logLine("quest retreat: panel=%p root=%p — refusing to call",
                (void*)panel, (void*)root);
        return false;
    }
    if (!safeReadPtr(base + RAID_SCREEN_RVA, &screen) || screen <= 0x10000) {
        logLine("quest retreat: no raid screen — refusing to call");
        return false;
    }
    uintptr_t cap[5];
    cap[0] = panel;
    cap[1] = root;
    cap[2] = qtLogEntry(base);
    cap[3] = screen;
    cap[4] = base + QT_RETREAT_CAP4_RVA;

    typedef void (*RetreatFn)(uintptr_t*);
    RetreatFn fn = (RetreatFn)(base + QT_RETREAT_CONFIRM_RVA);
    logLine("quest retreat: calling 0x%llx panel=%p root=%p entry=%p screen=%p",
            (unsigned long long)QT_RETREAT_CONFIRM_RVA, (void*)cap[0], (void*)cap[1],
            (void*)cap[2], (void*)cap[3]);
    __try { fn(cap); }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        logLine("quest retreat: the call FAULTED");
        return false;
    }
    return true;
}

static DWORD g_qtRetreatWatchUntil = 0;
void serviceQuestRetreatWatch() {
    if (!g_qtRetreatWatchUntil) return;
    if (confirmDialogOpen(g_base)) {          // the dialog reader has it from here
        logLine("quest retreat: the confirm dialog is up");
        g_qtRetreatWatchUntil = 0;
        return;
    }
    if ((long)(GetTickCount() - g_qtRetreatWatchUntil) < 0) return;
    g_qtRetreatWatchUntil = 0;
    logLine("⚠ quest retreat: no confirm dialog within the window");
    postSpeech(axs(AXS_NOTHING_HAPPENED));
}

static bool qtSealUp(uintptr_t base) {
    uintptr_t panel = qtFindPanel(base, nullptr, nullptr);
    if (!panel) return false;
    uint32_t seal = 0;
    if (!safeReadU32(panel + QT_PANEL_SEAL_OFF, &seal)) return false;
    return seal == 2;
}

// ---- THE ONE CONTEXT-CHOSEN CONTROL ----
static bool qtQuestWon(uintptr_t base) {
    uintptr_t root = mapRoot(base);
    return root && root == g_qcCompletedRoot;
}

enum QtBtn { QTB_NONE = 0, QTB_FLEE, QTB_ABANDON, QTB_REGROUP, QTB_FINISH };

static QtBtn qtCurrentButton(uintptr_t base) {
    uintptr_t root = mapRoot(base);
    if (!root) return QTB_NONE;

    bool battle = qtInBattle(base);
    bool crest  = qtElemOnScreen(QT_CREST_ID);
    bool seal   = qtSealUp(base);
    const char* why = nullptr;
    bool retreat = qtRetreatAvailable(base, &why);

    QtBtn pick;
    if (battle)      pick = retreat ? QTB_FLEE : QTB_NONE;
    else if (crest)  pick = QTB_FINISH;
    else if (retreat) pick = (qtQuestComplete(base) == 1 || qtQuestWon(base))
                                 ? QTB_REGROUP : QTB_ABANDON;
    else             pick = QTB_NONE;

    static int s_lastKey = -1;
    bool won = qtQuestWon(base);
    int key = ((int)pick << 5) | (won ? 16 : 0) | (battle ? 8 : 0) | (retreat ? 4 : 0) |
              (crest ? 2 : 0) | (seal ? 1 : 0);
    if (key != s_lastKey) {
        s_lastKey = key;
        logLine("quest button -> %s (battle=%d retreat=%d%s%s crest=%d seal=%d rtrt=%d won=%d)",
                pick == QTB_FLEE    ? "flee"    : pick == QTB_FINISH  ? "finish" :
                pick == QTB_REGROUP ? "regroup" : pick == QTB_ABANDON ? "abandon" : "none",
                battle ? 1 : 0, retreat ? 1 : 0, why ? " — " : "", why ? why : "",
                crest ? 1 : 0, seal ? 1 : 0, qtElemOnScreen(QT_RETREAT_ID) ? 1 : 0, won ? 1 : 0);
    }
    return pick;
}

// Is there a button row at all? The zone offers exactly one control, or none.
static bool qtHasButton(uintptr_t base) { return qtCurrentButton(base) != QTB_NONE; }

static void qtSentence(const char* in, char* out, int outsz) {
    if (!out || outsz <= 0) return;
    _snprintf(out, outsz, "%s", in ? in : "");
    out[outsz - 1] = 0;
    size_t n = strlen(out);
    if (n == 0) return;
    char last = out[n - 1];
    if (last == '.' || last == '!' || last == '?' || last == ':') return;
    if ((int)n + 1 < outsz) { out[n] = '.'; out[n + 1] = 0; }
}

static void qtButtonText(uintptr_t base, char* out, int outsz) {
    const char* key;
    const char* eng;
    char waveKey[64] = {0};
    QtBtn pick = qtCurrentButton(base);
    if ((pick == QTB_FLEE || pick == QTB_ABANDON || pick == QTB_REGROUP) &&
        comWaveRetreatKey(base, waveKey, sizeof waveKey) && waveKey[0]) {
        char raw[256];
        if (resolveKey(base, waveKey, raw, sizeof raw)) {
            char clean[256];
            stripMarkup(raw, clean, sizeof clean);
            if (clean[0]) { qtSentence(clean, out, outsz); return; }
        }
        logLine("quest: wave key \"%s\" did not resolve — falling back to the raid wording", waveKey);
    }
    switch (pick) {
        case QTB_FLEE:    key = "retreat_combat_tooltip";       eng = axs(AXS_QZ_BTN_FLEE);    break;
        case QTB_FINISH:  key = "str_return_to_hamlet_tooltip"; eng = axs(AXS_QZ_BTN_FINISH);  break;
        case QTB_REGROUP: key = "str_return_to_hamlet_tooltip"; eng = axs(AXS_QZ_BTN_REGROUP); break;
        case QTB_ABANDON: key = "retreat_raid_tooltip";         eng = axs(AXS_QZ_BTN_ABANDON); break;
        default:
            _snprintf(out, outsz, "%s", axs(AXS_NOT_AVAILABLE));
            out[outsz - 1] = 0;
            return;
    }
    char raw[256];
    if (resolveKey(base, key, raw, sizeof raw)) {
        char clean[256];
        stripMarkup(raw, clean, sizeof clean);
        if (clean[0]) { qtSentence(clean, out, outsz); return; }
    }
    qtSentence(eng, out, outsz);
    logLine("quest: button key \"%s\" did not resolve — using \"%s\"", key, eng);
}

static bool qtWaveRow(uintptr_t base, char* out, int outsz) {
    return comKillMeter(base, out, outsz);
}

static int qtRowCount(uintptr_t base, int* goalsOut) {
    uint32_t ids[QT_MAX_GOALS];
    int ng = qtGoalIds(base, ids, QT_MAX_GOALS);
    if (goalsOut) *goalsOut = ng;
    char wave[384];
    return ng + (qtWaveRow(base, wave, sizeof wave) ? 1 : 0) + (qtHasButton(base) ? 1 : 0);
}

static bool qtRowText(uintptr_t base, int row, char* out, int outsz) {
    uint32_t ids[QT_MAX_GOALS];
    int ng = qtGoalIds(base, ids, QT_MAX_GOALS);
    if (row < 0) return false;
    if (row < ng) {
        char goal[320];
        if (!qtGoalText(base, ids[row], goal, sizeof goal)) {
            if (ng > 1) { char ord[96];
                          _snprintf(ord, sizeof ord, axs(AXS_QZ_GOAL_N_FMT), row + 1, ng);
                          ord[sizeof ord - 1] = 0;
                          _snprintf(out, outsz, "%s %s", ord, axs(AXS_QZ_NOT_READABLE)); }
            else        _snprintf(out, outsz, "%s", axs(AXS_QZ_GOAL_UNREADABLE));
            out[outsz - 1] = 0;
            logLine("quest: goal %d/%d id=%u did not describe", row + 1, ng, ids[row]);
            return true;
        }
        if (ng > 1) { char ord[96];
                      _snprintf(ord, sizeof ord, axs(AXS_QZ_GOAL_N_FMT), row + 1, ng);
                      ord[sizeof ord - 1] = 0;
                      _snprintf(out, outsz, "%s %s", ord, goal); }
        else        _snprintf(out, outsz, "%s", goal);
        out[outsz - 1] = 0;
        return true;
    }
    char wave[384];
    bool haveWave = qtWaveRow(base, wave, sizeof wave);
    if (haveWave && row == ng) { _snprintf(out, outsz, "%s", wave); out[outsz - 1] = 0; return true; }
    int btnRow = ng + (haveWave ? 1 : 0);
    if (row == btnRow && qtHasButton(base)) { qtButtonText(base, out, outsz); return true; }
    return false;
}

static void qtSpeakRow(uintptr_t base, int row, const char* head = nullptr) {
    char out[512];
    if (!qtRowText(base, row, out, sizeof out)) {
        postSpeech(head && head[0] ? head : axs(AXS_QZ_NOTHING_HERE));
        return;
    }
    int ng = 0; int n = qtRowCount(base, &ng);
    logLine("quest row %d/%d (%s) -> \"%s\"", row, n,
            row < ng ? "goal" : (row == n - 1 && qtHasButton(base)) ? "button" : "wave", out);
    if (head && head[0]) {
        char full[768];
        _snprintf(full, sizeof full, "%s %s", head, out);
        full[sizeof full - 1] = 0;
        postSpeech(full);
        return;
    }
    postSpeech(out);
}

void qtSetActive(uintptr_t base, bool on) {
    (void)base;
    g_qtActive = on;
    g_qtCursor = on ? 0 : -1;
    logLine("quest zone %s", on ? "active" : "inactive");
}

bool qtEnterFromRoom(uintptr_t base) {
    int ng = 0;
    int n = qtRowCount(base, &ng);
    if (n <= 0) {
        // Say so rather than half-entering a zone with nothing in it.
        logLine("quest: nothing to show (goals=%d) — staying in the dungeon view", ng);
        postSpeech(axs(AXS_QZ_NO_QUEST_INFO));
        return true;
    }
    g_qtRoomReturn = g_rvCursor;         // remember the row, before rvSetActive re-seeds it
    rvSetActive(base, false);
    qtSetActive(base, true);
    char msg[256];
    if (ng > 1) _snprintf(msg, sizeof msg, axs(AXS_QZ_HEAD_GOALS_FMT), ng);
    else        _snprintf(msg, sizeof msg, "%s", axs(AXS_QZ_HEAD_QUEST));
    msg[sizeof msg - 1] = 0;
    logLine("quest enter: goals=%d rows=%d", ng, n);
    qtSpeakRow(base, 0, msg);
    return true;
}

// Down out of the zone, back to the dungeon view it was entered from.
static void qtBackToRoom(uintptr_t base) {
    qtSetActive(base, false);
    uintptr_t party[RV_MAX_MEMBERS];
    if (rvPartyList(base, party, RV_MAX_MEMBERS) <= 0) {
        logLine("quest: leaving the zone but the dungeon view has nothing to return to");
        postSpeech(axs(AXS_QZ_CLOSED));
        return;
    }
    rvSetActive(base, true);
    int want = g_qtRoomReturn;
    g_qtRoomReturn = -1;
    if (want >= 0) g_rvCursor = want;
    rvAnnounceEntry(base, false);
}

bool routeQuestToggle(uintptr_t base, uint32_t sym, uint8_t repeat) {
    if (sym != SDLK_g) return false;
    if (repeat) return true;                   // held key must not flap the context
    if (g_qtActive) {
        qtSpeakRow(base, g_qtCursor < 0 ? 0 : g_qtCursor);
        if (g_qtCursor < 0) g_qtCursor = 0;
        return true;
    }
    if (g_abActive) abSetActive(base, false);
    iuAbandon("quest goals hotkey");
    return qtEnterFromRoom(base);
}

bool routeQuestKey(uintptr_t base, uint32_t sym, uint16_t mod, uint8_t repeat) {   // (de-static'd
    if (mod & (KMOD_LALT | KMOD_RALT)) return false;
    if (mod & (KMOD_LCTRL | KMOD_RCTRL)) return false;   // no buffer here; don't claim what we can't use

    if (sym == SDLK_TAB) {
        if (repeat) return false;
        logLine("quest: Tab -> closing the zone, panel toggle passes through");
        qtSetActive(base, false);
        armPanelHandoff("the quest zone");
        return false;
    }

    if (sym == SDLK_DOWN) {
        if (repeat) return true;
        qtBackToRoom(base);
        return true;
    }
    if (sym == SDLK_UP) {
        if (repeat) return true;
        qtSpeakRow(base, g_qtCursor < 0 ? 0 : g_qtCursor);
        if (g_qtCursor < 0) g_qtCursor = 0;
        return true;
    }

    if (sym == SDLK_RETURN || sym == SDLK_KP_ENTER) {
        if (repeat) return true;
        int ng = 0;
        int n = qtRowCount(base, &ng);
        if (n <= 0 || g_qtCursor < 0) { postSpeech(axs(AXS_NO_ROW_SELECTED)); return true; }
        if (g_qtCursor >= n) g_qtCursor = n - 1;
        char waveRow[384];   // existence test only, but sized with the others
        int btnRow = ng + (qtWaveRow(base, waveRow, sizeof waveRow) ? 1 : 0);
        if (g_qtCursor < btnRow) {
            qtSpeakRow(base, g_qtCursor);
            return true;
        }
        QtBtn btn = qtCurrentButton(base);
        if (btn == QTB_NONE || g_qtCursor != btnRow) { postSpeech(axs(AXS_NOT_AVAILABLE)); return true; }

        if (btn == QTB_FINISH) {
            if (!frontEndClickElementId((int64_t)QT_CREST_ID)) {
                logLine("quest: click refused for the crest 0x%08x (not on screen)", QT_CREST_ID);
                postSpeech(axs(AXS_NOT_AVAILABLE));
            }
            return true;
        }

        bool asks = qtRetreatAsksFirst(base);
        if (!qtRetreatActivate(base)) { postSpeech(axs(AXS_NOTHING_HAPPENED)); return true; }
        if (asks) g_qtRetreatWatchUntil = GetTickCount() + 1500;
        else      logLine("quest retreat: no dialog expected here — the battle banners report it");
        return true;
    }

    {
        int jump = 0;
        if (axDecodeJump(sym, mod, repeat, &jump)) {
            if (!jump) return true;              // held jump: one landing per press
            int ngj = 0;
            int nj = qtRowCount(base, &ngj);
            if (nj <= 0) { logLine("quest: nothing to navigate"); postSpeech(axs(AXS_QZ_NOTHING_HERE)); return true; }
            g_qtCursor = (jump > 0) ? nj - 1 : 0;
            qtSpeakRow(base, g_qtCursor);
            return true;
        }
    }

    int dir;
    switch (sym) {
        case SDLK_LEFT:  dir = -1; break;
        case SDLK_RIGHT: dir =  1; break;
        default:         return false;   // not ours -> let the game have it
    }
    if (axNavHoldRepeat(repeat)) return true;    // throttled repeat: claimed, no step

    int ng = 0;
    int n = qtRowCount(base, &ng);
    if (n <= 0) { logLine("quest: nothing to navigate"); postSpeech(axs(AXS_QZ_NOTHING_HERE)); return true; }
    if (g_qtCursor >= n) g_qtCursor = n - 1;

    if (g_qtCursor < 0) g_qtCursor = 0;
    else axStepCursor(&g_qtCursor, n, dir);  // hard stop: clamp, then re-read the row we sat on
    qtSpeakRow(base, g_qtCursor);
    return true;
}

// ---- THE QUEST-COMPLETE POPUP ----
volatile bool g_qcOpen = false;
static int  g_qcCursor = 0;

// ---- THE RETURN-IN-FLIGHT BLOCK ----
volatile bool    g_qcReturning    = false;
static DWORD     g_qcReturnSince  = 0;
static uintptr_t g_qcReturnRoot   = 0;
static DWORD     g_qcReturnSaidAt = 0;
static bool      g_qcReturnMovie  = false;
static const DWORD QC_RETURN_CAP_MS = 240000;

static void qcReturnBegin(uintptr_t base) {
    if (g_qcReturning) return;
    g_qcReturning    = true;
    g_qcReturnSince  = GetTickCount();
    g_qcReturnRoot   = mapRoot(base);
    g_qcReturnSaidAt = 0;
    g_qcReturnMovie  = subMovieTrackUp(base);
    logLine("questdone: Return to Hamlet observed taken -> holding every raid key until the raid "
            "is gone (movie track %s)", g_qcReturnMovie ? "up" : "down");
    postSpeech(axs(AXS_QC_RETURNING), false);
}

static void qcReturnEnd(const char* why, bool stalled) {
    if (!g_qcReturning) return;
    g_qcReturning = false;
    logLine("questdone: return block released after %u ms (%s)",
            (unsigned)(GetTickCount() - g_qcReturnSince), why);
    if (stalled) postSpeech(axs(AXS_QC_RETURN_STALLED));
}

static void qcServiceReturn(uintptr_t base) {
    if (!g_qcReturning) return;
    if (rrDisplay(base))  { qcReturnEnd("results screen up", false); return; }
    if (axIsLoading())    { qcReturnEnd("loading screen has the floor", false); return; }
    uintptr_t root = mapRoot(base);
    if (!root)                  { qcReturnEnd("raid root gone", false); return; }
    if (root != g_qcReturnRoot) { qcReturnEnd("a different raid", false); return; }
    bool movie = subMovieTrackUp(base);
    if (movie != g_qcReturnMovie) {
        g_qcReturnMovie = movie;
        logLine("questdone: movie track %s during the return (+%u ms)",
                movie ? "UP" : "down", (unsigned)(GetTickCount() - g_qcReturnSince));
    }
    if (GetTickCount() - g_qcReturnSince > QC_RETURN_CAP_MS)
        qcReturnEnd("cap expired -- the raid never left", true);
}

bool axIsRaidFinish() {
    return g_qcReturning && mapRoot(g_base) != 0 && rrDisplay(g_base) == 0;
}
bool routeRaidFinishKey(uintptr_t base, uint32_t sym, uint16_t mod, uint8_t repeat) {
    (void)base; (void)sym;
    if (mod & (KMOD_LALT | KMOD_RALT | KMOD_LCTRL | KMOD_RCTRL)) return false;  // Alt+F4 etc.
    if (repeat) return true;
    DWORD now = GetTickCount();
    if (now - g_qcReturnSaidAt >= 1500) {     // one line per press, not per key bounce
        g_qcReturnSaidAt = now;
        postSpeech(axs(AXS_QC_RETURN_WAIT));
    }
    return true;
}

bool qcPresent() {
    uintptr_t base = g_base;
    if (!mapRoot(base)) return false;
    uintptr_t panel = qtFindPanel(base, nullptr, nullptr);
    if (!panel) return false;
    uint32_t state = 0;
    if (!safeReadU32(panel + QT_PANEL_STATE_OFF, &state)) return false;
    return state >= 1 && state <= 3;
}

// ---- The probe that will re-enable the popup, correctly ----
static uintptr_t qcElem(uint32_t id) {
    return focusElementById(g_base, (int64_t)id);
}
static uintptr_t qcOwnerOf(uint32_t id) {
    uintptr_t e = qcElem(id), owner = 0;
    if (!e) return 0;
    if (!safeReadPtr(e + ELEM_OWNER_OFF, &owner)) return 0;
    return owner;
}

// ---- Round 2: find the QuestInfo PANEL, and read its own state ----
static const uintptr_t QT_PANEL_EXPECTED_OFF = 0x3590;    // raidScreen+: the embedded QuestInfo (was 0x3100)
static const uintptr_t QT_SCAN_BYTES        = 0x4800;     // fallback sweep, past the object's end
static const int       QT_SCAN_MAX_HITS     = 4;

static bool qtIsPanel(uintptr_t base, uintptr_t cand) {
    uintptr_t vt = 0;
    if (!cand || cand <= 0x10000) return false;
    if (!safeReadPtr(cand, &vt)) return false;
    return vt == base + QT_PANEL_VFTABLE_RVA;
}

static uintptr_t qtFindPanel(uintptr_t base, int* routeOut, uintptr_t* offOut) {
    if (routeOut) *routeOut = 0;
    if (offOut)   *offOut   = 0;
    uintptr_t screen = 0;
    if (!safeReadPtr(base + RAID_SCREEN_RVA, &screen) || screen <= 0x10000) return 0;

    if (qtIsPanel(base, screen + QT_PANEL_EXPECTED_OFF)) {
        if (routeOut) *routeOut = 1;
        if (offOut)   *offOut   = QT_PANEL_EXPECTED_OFF;
        return screen + QT_PANEL_EXPECTED_OFF;
    }

    static uintptr_t s_off = (uintptr_t)-1;          // -1 = never scanned, 0 = scanned, absent
    static uintptr_t s_scannedScreen = 0;
    if (s_scannedScreen != screen) {                 // new screen -> the latch means nothing
        s_off = (uintptr_t)-1;
        s_scannedScreen = screen;
    }
    if (s_off == 0) return 0;                        // already swept this screen; it is not here
    if (s_off != (uintptr_t)-1) {
        if (qtIsPanel(base, screen + s_off)) {
            if (routeOut) *routeOut = 2;
            if (offOut)   *offOut   = s_off;
            return screen + s_off;
        }
        s_off = (uintptr_t)-1;                       // stale -> fall through and rescan once
    }
    int hits = 0;
    uintptr_t firstOff = 0;
    for (uintptr_t o = 0; o + 8 <= QT_SCAN_BYTES; o += 8) {
        if (!qtIsPanel(base, screen + o)) continue;
        if (!hits) firstOff = o;
        if (++hits >= QT_SCAN_MAX_HITS) break;
    }
    if (hits) {
        logLine("questpanel: sweep found %d embedded QuestInfo(s), using raidscreen+0x%llx",
                hits, (unsigned long long)firstOff);
        s_off = firstOff;
        if (routeOut) *routeOut = 2;
        if (offOut)   *offOut   = firstOff;
        return screen + firstOff;
    }
    logLine("questpanel: NOT FOUND on this raid screen (%p) — neither the panel list nor a "
            "0x%llx-byte member sweep holds RaidUI::Panel::QuestInfo::vftable",
            (void*)screen, (unsigned long long)QT_SCAN_BYTES);
    s_off = 0;                                       // do not sweep this screen again
    return 0;
}

// ---- Why the button row never appeared: dump what is ACTUALLY registered ----

// ---- The two quest diagnostics, now OFF ----
static const bool QT_DIAGNOSTICS = false;

void qtDumpFocusIds(uintptr_t base) {
    if (!QT_DIAGNOSTICS) return;
    if (!mapRoot(base)) return;
    uintptr_t begin = 0, end = 0;
    if (!safeReadPtr(base + VEC_BEGIN_RVA, &begin) ||
        !safeReadPtr(base + VEC_END_RVA, &end) || !begin || end <= begin) return;
    uintptr_t count = (end - begin) / ELEM_STRIDE;
    if (count > 512) count = 512;

    uint64_t h = 1469598103934665603ull;
    for (uintptr_t i = 0; i < count; i++) {
        int64_t id = 0;
        if (!safeReadI64(begin + i * ELEM_STRIDE + ELEM_ID_OFF, &id)) continue;
        h ^= (uint64_t)id; h *= 1099511628211ull;
    }
    static uint64_t s_lastHash = 0;
    if (h == s_lastHash) return;
    s_lastHash = h;

    bool sawRetreat = false;
    char line[512];
    int used = 0, inLine = 0, chunk = 0;
    used = _snprintf(line, sizeof line, "focusdump[%d] (%d elems):", chunk, (int)count);
    for (uintptr_t i = 0; i < count; i++) {
        int64_t id = 0;
        if (!safeReadI64(begin + i * ELEM_STRIDE + ELEM_ID_OFF, &id)) continue;
        uint32_t cc = (uint32_t)(uint64_t)id;
        if (cc == QT_RETREAT_ID) sawRetreat = true;
        char t[5]; fourcc(cc, t);
        float x = 0, y = 0; elemPos(begin + i * ELEM_STRIDE, &x, &y);
        int n = _snprintf(line + used, (int)sizeof line - used,
                          " [0x%08x '%s' @%.0f,%.0f]", cc, t, x, y);
        if (n < 0 || used + n >= (int)sizeof line - 8) {
            logLine("%s", line);                       // flush and start the next chunk
            chunk++;
            used = _snprintf(line, sizeof line, "focusdump[%d]:", chunk);
            n = _snprintf(line + used, (int)sizeof line - used,
                          " [0x%08x '%s' @%.0f,%.0f]", cc, t, x, y);
            inLine = 0;
        }
        if (n > 0) used += n;
        inLine++;
    }
    if (inLine || chunk == 0) logLine("%s", line);
    logLine("focusdump: rtrt(0x%08x) present=%d  <- 1 only while the quest log is hovered",
            QT_RETREAT_ID, sawRetreat ? 1 : 0);
}

void qcProbe(uintptr_t base) {
    if (!QT_DIAGNOSTICS) return;
    if (!mapRoot(base)) return;

    int route = 0; uintptr_t off = 0;
    uintptr_t panel = qtFindPanel(base, &route, &off);
    static uintptr_t s_announcedFor = 0;
    uintptr_t root = mapRoot(base);
    if (s_announcedFor != root) {
        s_announcedFor = root;
        logLine("questpanel: raid entered — panel=%p route=%d off=0x%llx (%s)",
                (void*)panel, route, (unsigned long long)off,
                panel ? "FOUND" : "NOT FOUND, the state gate cannot be built this way");
    }

    uintptr_t yes = qcElem(QT_HAMLET_ID);
    uintptr_t no  = qcElem(QT_CONTINUE_ID);

    uint32_t f144 = 0, f148 = 0, f14c = 0, f150 = 0, f164 = 0;
    uint8_t  b145 = 0;
    if (panel) {
        safeReadU32(panel + 0x144, &f144);
        safeReadU8 (panel + 0x145, &b145);
        safeReadU32(panel + 0x148, &f148);
        safeReadU32(panel + 0x14c, &f14c);
        safeReadU32(panel + 0x150, &f150);
        safeReadU32(panel + 0x164, &f164);
    }
    int ctx = (int)currentAxContext();

    static uint32_t s[6] = { 0xffffffffu, 0xffffffffu, 0xffffffffu, 0xffffffffu, 0xffffffffu, 0xffffffffu };
    static int s_ctx = -1, s_route = -1;
    if (s[0] == f144 && s[1] == (uint32_t)b145 && s[2] == f148 && s[3] == f14c &&
        s[4] == f150 && s[5] == f164 && s_ctx == ctx && s_route == route) return;
    s[0] = f144; s[1] = b145; s[2] = f148; s[3] = f14c; s[4] = f150; s[5] = f164;
    s_ctx = ctx; s_route = route;

    // ctx is the enum value: 9=room 10=quest 11=questdone 12=loot 13=event 17=ingame.
    logLine("qcprobe2: yes=%d no=%d | panel=%p route=%d off=0x%llx | "
            "+144=%u +145=%u +148=%u +14c=%u +150=%u +164=0x%08x | ctx=%d",
            yes ? 1 : 0, no ? 1 : 0, (void*)panel, route, (unsigned long long)off,
            f144, (unsigned)b145, f148, f14c, f150, f164, ctx);
}

// Label one choice from the game's own text, with a plain-English last resort.
static void qcChoiceText(uintptr_t base, int idx, char* out, int outsz) {
    const char* key = idx == 0 ? "str_return_to_hamlet_tooltip" : "str_continue_raid_tooltip";
    const char* eng = axs(idx == 0 ? AXS_QC_CHOICE_HAMLET : AXS_QC_CHOICE_CONTINUE);
    char raw[256];
    if (resolveKey(base, key, raw, sizeof raw)) {
        char clean[256];
        stripMarkup(raw, clean, sizeof clean);
        if (clean[0]) { _snprintf(out, outsz, "%s.", clean); out[outsz - 1] = 0; return; }
    }
    _snprintf(out, outsz, "%s.", eng);
    out[outsz - 1] = 0;
}

static void qcSpeakChoice(uintptr_t base) {
    char out[320];
    qcChoiceText(base, g_qcCursor, out, sizeof out);
    logLine("questdone choice %d -> \"%s\"", g_qcCursor, out);
    postSpeech(out);
}

static void qcComposeLine(uintptr_t base, char* out, int outsz) {
    char a[320], b[320];
    qcChoiceText(base, 0, a, sizeof a);
    qcChoiceText(base, 1, b, sizeof b);
    char alt[384];
    _snprintf(alt, sizeof alt, axs(AXS_QC_OR_FMT), b);
    alt[sizeof alt - 1] = 0;
    _snprintf(out, outsz, "%s %s %s", axs(AXS_QC_COMPLETE_HEAD), a, alt);
    out[outsz - 1] = 0;
}

void qcReannounce(uintptr_t base) {
    char msg[768];
    qcComposeLine(base, msg, sizeof msg);
    logLine("questdone: re-announcing on request");
    postSpeech(msg);
}

// ---- THE 2026-08-23 PLAYER REPORT: "Return to Hamlet" LEFT THE PLAYER IN THE DUNGEON ----
static void qcDumpChoices(const char* why) {
    if (!axDebugLogEnabled()) return;
    uintptr_t panel = qtFindPanel(g_base, nullptr, nullptr);
    uint32_t state = 0, answer = 0;
    if (panel) { safeReadU32(panel + QT_PANEL_STATE_OFF, &state);
                 safeReadU32(panel + QT_PANEL_SEAL_OFF,  &answer); }
    logLine("qcdump (%s): panel=%p +0x148=%u +0x14c=%u cursor=%d",
            why ? why : "?", (void*)panel, state, answer, g_qcCursor);

    uintptr_t begin = 0, end = 0;
    if (!safeReadPtr(g_base + VEC_BEGIN_RVA, &begin) ||
        !safeReadPtr(g_base + VEC_END_RVA, &end) || !begin || end <= begin) {
        logLine("qcdump (%s): the element vector is empty or unreadable", why ? why : "?");
        return;
    }
    uintptr_t n = (end - begin) / ELEM_STRIDE;
    if (n > 512) n = 512;
    int hits = 0;
    for (uintptr_t i = 0; i < n; i++) {
        uintptr_t e = begin + i * ELEM_STRIDE;
        int64_t id = 0;
        if (!safeReadI64(e + ELEM_ID_OFF, &id)) continue;
        uint32_t cc = (uint32_t)(uint64_t)id;
        if (cc != QT_HAMLET_ID && cc != QT_CONTINUE_ID) continue;
        hits++;
        float x = 0, y = 0; elemPos(e, &x, &y);
        uint32_t wb = 0, hb = 0;
        safeReadU32(e + ELEM_SIZE_OFF, &wb);
        safeReadU32(e + ELEM_SIZE_OFF + 4, &hb);
        uint8_t f = 0, sk = 0;
        safeReadU8(e + ELEM_FOCUSABLE_OFF, &f);
        safeReadU8(e + ELEM_SKIP_OFF, &sk);
        uintptr_t owner = 0; safeReadPtr(e + ELEM_OWNER_OFF, &owner);
        char tag[9]; idToAscii(id, tag);
        logLine("  qcdump fe[%llu] id=0x%08x \"%s\" pos=(%.0f,%.0f) size=(%.0fx%.0f) "
                "focusable=%u skip=%u owner=%p",
                (unsigned long long)i, cc, tag, x, y, u32AsFloatM(wb), u32AsFloatM(hb),
                f, sk, (void*)owner);
    }
    logLine("qcdump (%s): %d of %llu elements carry \"yes \" or \"no  \"",
            why ? why : "?", hits, (unsigned long long)n);
    feDumpFocusVector(why ? why : "questdone");
}

static DWORD    g_qcClickWatchUntil = 0;
static uint32_t g_qcWatchState = 0xffffffffu, g_qcWatchAnswer = 0xffffffffu;

static void qcServiceClickWatch() {
    if (!g_qcClickWatchUntil) return;
    uintptr_t panel = qtFindPanel(g_base, nullptr, nullptr);
    uint32_t state = 0, answer = 0;
    if (panel) { safeReadU32(panel + QT_PANEL_STATE_OFF, &state);
                 safeReadU32(panel + QT_PANEL_SEAL_OFF,  &answer); }
    if (state != g_qcWatchState || answer != g_qcWatchAnswer) {
        logLine("qcwatch: +0x148 %u -> %u, +0x14c %u -> %u%s",
                g_qcWatchState, state, g_qcWatchAnswer, answer,
                answer == 1 ? "  (Return to Hamlet took)" :
                answer == 2 ? "  (Continue Adventuring took)" : "");
        bool tookHamlet = (answer == 1 && g_qcWatchAnswer != 1);
        g_qcWatchState = state; g_qcWatchAnswer = answer;
        if (tookHamlet) qcReturnBegin(g_base);
    }
    if ((long)(GetTickCount() - g_qcClickWatchUntil) < 0) return;
    g_qcClickWatchUntil = 0;
    logLine("qcwatch: window closed with +0x148=%u +0x14c=%u", g_qcWatchState, g_qcWatchAnswer);
}

void checkQuestCompletePopup(uintptr_t base) {
    qcServiceClickWatch();
    qcServiceReturn(base);                       // the return-in-flight block's release edges
    bool up = qcPresent();
    if (up == g_qcOpen) return;
    g_qcOpen = up;
    if (!up) { logLine("questdone: closed"); qcDumpChoices("questdone closed"); return; }
    qcReturnEnd("a quest-complete popup opened", false);   // cannot be returning AND asked again
    g_qcCursor = 0;
    g_qcCompletedRoot = mapRoot(base);
    char msg[768];
    qcComposeLine(base, msg, sizeof msg);
    logLine("questdone: OPEN");
    qcDumpChoices("questdone opened");
    postSpeech(msg);
}

bool routeQuestDoneKey(uintptr_t base, uint32_t sym, uint16_t mod, uint8_t repeat) {
                             // de-static'd; the AX_QUESTDONE row stays with the context table too)
    if (mod & (KMOD_LALT | KMOD_RALT | KMOD_LCTRL | KMOD_RCTRL)) return false;
    if (sym == SDLK_LEFT || sym == SDLK_RIGHT) {
        if (repeat) return true;
        axStepCursor(&g_qcCursor, 2, sym == SDLK_RIGHT ? 1 : -1);   // hard stops, as everywhere else
        qcSpeakChoice(base);
        return true;
    }
    if (sym == SDLK_RETURN || sym == SDLK_KP_ENTER) {
        if (repeat) return true;
        uint32_t id = g_qcCursor == 0 ? QT_HAMLET_ID : QT_CONTINUE_ID;
        qcDumpChoices(g_qcCursor == 0 ? "before the Return-to-Hamlet click"
                                      : "before the Continue-Adventuring click");
        bool ok = frontEndClickElementId((int64_t)id);
        if (!ok) postSpeech(axs(AXS_NOT_AVAILABLE));
        if (ok) {
            uintptr_t panel = qtFindPanel(base, nullptr, nullptr);
            g_qcWatchState = g_qcWatchAnswer = 0;
            if (panel) { safeReadU32(panel + QT_PANEL_STATE_OFF, &g_qcWatchState);
                         safeReadU32(panel + QT_PANEL_SEAL_OFF,  &g_qcWatchAnswer); }
            g_qcClickWatchUntil = GetTickCount() + 3000;
        }
        return true;
    }
    return false;
}
