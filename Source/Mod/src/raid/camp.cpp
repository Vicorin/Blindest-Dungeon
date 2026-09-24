// raid/camp.cpp -- THE CAMP

#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cmath>
#include "internal.h"

// ---- THE CAMP ITSELF ----
static const uintptr_t RAID_CAMP_PHASE_OFF  = 0x5118;
static const uintptr_t RAID_CAMP_POINTS_OFF = 0x511c;
static const uintptr_t RAID_CAMP_USED_OFF   = 0x5120;
static const int CAMP_PHASE_MEAL    = 3;
static const int CAMP_PHASE_RESPITE = 6;
static const uintptr_t ACTOR_CAMP_ID_OFF    = 0x130c;
static const uintptr_t ACTOR_CAMP_ARMED_OFF = 0x13ac;
                                                        // (-1 = none). Camp start writes -1 per hero.

// ---- PERFORMING a camping skill ----
static const uintptr_t RAID_CAMP_PENDING_OFF = 0x5100;
static const uintptr_t RAID_CAMP_ARG3_OFF    = 0x5140;
static const char CAMP_SEL_INDIVIDUAL[] = "individual";
static const uintptr_t CAMPUSED_KEY_HERO_OFF  = 0x1c;
static const uintptr_t CAMPUSED_KEY_SKILL_OFF = 0x20;
static const uintptr_t CAMPUSED_COUNT_OFF     = 0x28;

static const uintptr_t MEAL_KEY_OFF     = 0x1c;       // int: the option index, this map's key
static const uintptr_t MEAL_RATIONS_OFF = 0x20;       // float: 0, 0.5, 1.0, 2.0
static const uintptr_t MEAL_HEALING_OFF = 0x24;       // float: -0.2, 0, 0.1, 0.25 (x100 = percent)
static const uintptr_t MEAL_STRESS_OFF  = 0x28;       // float: 15, 0, 0, -10
static const uint32_t  MEAL_FOURCC_BASE = 0x6d656c20; // "mel " + index — the option's focus id
static const uintptr_t ACTOR_RATION_OFF = 0xbc4;

// ---- MAY A CAMP START HERE? ----
typedef char (*CampCanStartFn)(uintptr_t raidDisplay);
bool campCanStartHere(uintptr_t base) {
    uintptr_t rd = 0;
    if (!safeReadPtr(base + RAID_SCREEN_RVA, &rd) || rd <= 0x10000) {
        logLine("camp: can-start gate has no raid screen -> refusing");
        return false;
    }
    char ok = 0;
    __try { ok = ((CampCanStartFn)(base + CAMP_CAN_START_RVA))(rd); }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        logLine("camp: can-start gate FAULTED -> refusing");
        return false;
    }
    if (!ok) {
        uint32_t state = 0;
        safeReadU32(rd + 0x40b0, &state);
        logLine("camp: the game's can-start gate says no (raid state %u, phase %d)",
                state, campPhase(base));
    }
    return ok != 0;
}

// ---- THE CAMP: state readers ----

int campPhase(uintptr_t base) {
    uintptr_t root = mapRoot(base);
    if (!root) return 0;
    int32_t v = 0;
    if (!safeReadU32(root + RAID_CAMP_PHASE_OFF, (uint32_t*)&v)) return 0;
    if (v < 0 || v > 9) return 0;
    return (int)v;
}
bool campActive(uintptr_t base)      { return campPhase(base) != 0; }
static bool campInMealPhase(uintptr_t base) { return campPhase(base) == CAMP_PHASE_MEAL; }
bool campInSkillPhase(uintptr_t base){ return campPhase(base) == CAMP_PHASE_RESPITE; }

int campPoints(uintptr_t base) {
    uintptr_t root = mapRoot(base);
    if (!root) return -1;
    int32_t v = 0;
    if (!safeReadU32(root + RAID_CAMP_POINTS_OFF, (uint32_t*)&v)) return -1;
    if (v < 0 || v > 999) return -1;
    return (int)v;
}

uint32_t campSkillUsedCount(uintptr_t base, uintptr_t hero, uintptr_t cls) {
    uintptr_t root = mapRoot(base);
    if (!root || !hero || !cls) return 0;
    uint32_t heroId = 0, skillHash = 0;
    if (!safeReadU32(hero + ACTOR_CAMP_ID_OFF, &heroId)) return 0;
    if (!safeReadU32(cls + CAMP_SKILL_HASH_OFF, &skillHash)) return 0;

    uintptr_t head = 0;
    if (!safeReadPtr(root + RAID_CAMP_USED_OFF, &head) || head <= 0x10000) return 0;
    uintptr_t node = 0;
    if (!safeReadPtr(head + RBNODE_PARENT_OFF, &node) || node <= 0x10000) return 0;

    for (int guard = 0; guard < 64; guard++) {
        uint8_t nil = 1;
        if (!safeReadU8(node + RBNODE_ISNIL_OFF, &nil) || nil) return 0;
        uint32_t nh = 0, ns = 0;
        if (!safeReadU32(node + CAMPUSED_KEY_HERO_OFF, &nh) ||
            !safeReadU32(node + CAMPUSED_KEY_SKILL_OFF, &ns)) return 0;
        if (nh == heroId && ns == skillHash) {
            uint32_t used = 0;
            safeReadU32(node + CAMPUSED_COUNT_OFF, &used);
            return used;
        }
        bool goRight = (nh < heroId) || (nh == heroId && ns < skillHash);
        uintptr_t next = 0;
        if (!safeReadPtr(node + (goRight ? RBNODE_RIGHT_OFF : RBNODE_LEFT_OFF), &next)) return 0;
        if (next <= 0x10000) return 0;
        node = next;
    }
    logLine("camp: used-map walk too deep — treating as unused");
    return 0;
}

static int campSelRosterIndex(uintptr_t hero, int i) {
    if (!hero || i < 0) return -1;
    uintptr_t beg = 0, end = 0;
    if (!safeReadPtr(hero + ACTOR_CAMP_SEL_BEG_OFF, &beg) ||
        !safeReadPtr(hero + ACTOR_CAMP_SEL_END_OFF, &end)) return -1;
    if (beg <= 0x10000 || end < beg) return -1;
    uintptr_t n = (end - beg) / 4;
    if (n > (uintptr_t)CAMP_SEL_MAX) return -1;    // the struct's own cap; rules.json ships three
    if ((uintptr_t)i >= n) return -1;
    uint32_t idx = 0;
    if (!safeReadU32(beg + (uintptr_t)i * 4, &idx)) return -1;
    return (int)idx;
}

// ---- THE CAMP: the ACTION BAR's camping-skill buttons ----

static uintptr_t campBarSkillClass(uintptr_t base, int barIdx) {
    uintptr_t hero = abSelectedHero(base);
    int roster = campSelRosterIndex(hero, barIdx);
    if (roster < 0) return 0;
    return csCampSkillClass(base, roster);
}

static const char* campBarRefusal(uintptr_t base, uintptr_t hero, uintptr_t cls, int cost) {
    uint32_t limit = 0;
    if (safeReadU32(cls + CAMP_SKILL_USE_LIMIT_OFF, &limit) && limit > 0 &&
        campSkillUsedCount(base, hero, cls) >= limit)
        return "Already used";
    int have = campPoints(base);
    if (have >= 0 && cost > 0 && cost > have) return "Not enough time";
    return nullptr;
}

bool campBarLabel(uintptr_t base, int barIdx, char* out, int outsz) {
    uintptr_t cls = campBarSkillClass(base, barIdx);
    if (!cls) return false;
    char name[256];
    if (!csCampSkillName(base, cls, name, sizeof name)) return false;

    int cost = csCampSkillCost(cls);
    if (cost > 0) _snprintf(out, outsz, axs(AXS_CAMP_COSTS_FMT), name, cost);
    else          _snprintf(out, outsz, "%s.", name);
    out[outsz - 1] = 0;

    const char* no = campBarRefusal(base, abSelectedHero(base), cls, cost);
    if (no) {
        size_t len = strlen(out);
        _snprintf(out + len, outsz - len, " %s.", no);
        out[outsz - 1] = 0;
    }
    return true;
}

int campBarLines(uintptr_t base, int barIdx, char lines[AB_TIP_MAX_LINES][AB_TIP_LINE_SZ]) {
    for (int k = 0; k < AB_TIP_MAX_LINES; k++) lines[k][0] = 0;
    uintptr_t cls = campBarSkillClass(base, barIdx);
    if (!cls) return 0;
    if (!campBarLabel(base, barIdx, lines[0], AB_TIP_LINE_SZ)) return 0;
    int n = 1 + csCampEffectLines(base, cls, lines, 1);
    n += csCampUsesLine(base, cls, abSelectedHero(base), lines, n);
    return n;
}

static char    g_campUseName[256] = { 0 };
static DWORD   g_campUseDeadline  = 0;
static void campArmUseWatch(const char* name) {
    _snprintf(g_campUseName, sizeof g_campUseName, "%s", name ? name : "");
    g_campUseName[sizeof g_campUseName - 1] = 0;
    g_campUseDeadline = GetTickCount() + 2500;   // generous: the game plays an animation first
}

// ---- THE CAMP: performing a skill, and the target pick it may need ----

typedef void (*CampPerformFn)(uintptr_t pending, uintptr_t partyVec, uintptr_t arg3,
                              uint32_t performerIdx, uint32_t skillIdx, uint32_t targetIdx);
static bool sehCampPerform(uintptr_t base, uintptr_t root, int performerIdx, int skillIdx,
                           int targetIdx) {
    __try {
        ((CampPerformFn)(base + CAMP_PERFORM_RVA))(
            root + RAID_CAMP_PENDING_OFF, root + RAID_PARTY_BEG_OFF, root + RAID_CAMP_ARG3_OFF,
            (uint32_t)performerIdx, (uint32_t)skillIdx, (uint32_t)targetIdx);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

static int campPartyVec(uintptr_t base, uintptr_t* out, int maxOut) {
    uintptr_t root = mapRoot(base);
    if (!root) return 0;
    uintptr_t beg = 0, end = 0;
    if (!safeReadPtr(root + RAID_PARTY_BEG_OFF, &beg) ||
        !safeReadPtr(root + RAID_PARTY_END_OFF, &end)) return 0;
    if (beg <= 0x10000 || end < beg) return 0;
    int n = (int)((end - beg) / 8);
    if (n <= 0 || n > RV_MAX_MEMBERS) return 0;
    if (n > maxOut) n = maxOut;
    for (int i = 0; i < n; i++) {
        uintptr_t hero = 0;
        if (!safeReadPtr(beg + (uintptr_t)i * 8, &hero) || hero <= 0x10000) hero = 0;
        out[i] = hero;
    }
    return n;
}

static int campPartyIndexOf(uintptr_t base, uintptr_t hero) {
    uintptr_t party[RV_MAX_MEMBERS];
    int n = campPartyVec(base, party, RV_MAX_MEMBERS);
    for (int i = 0; i < n; i++) if (party[i] == hero) return i;
    return -1;
}

static bool campSkillNeedsTarget(uintptr_t cls) {
    uintptr_t beg = 0, end = 0;
    if (!cls || !safeReadPtr(cls + CAMP_SKILL_EFFVEC_OFF, &beg) ||
        !safeReadPtr(cls + CAMP_SKILL_EFFVEC_END_OFF, &end)) return false;
    if (beg <= 0x10000 || end < beg) return false;
    uintptr_t n = (end - beg) / CAMP_EFF_STRIDE;
    if (n > 32) { logLine("camp: skill claims %u effects — refusing", (unsigned)n); return false; }
    for (uintptr_t e = 0; e < n; e++) {
        char sel[64] = { 0 };
        if (!safeReadCStr(beg + e * CAMP_EFF_STRIDE + CAMP_EFF_SELECTION_OFF, sel, sizeof sel))
            continue;
        if (strcmp(sel, CAMP_SEL_INDIVIDUAL) == 0) return true;
    }
    return false;
}

static volatile bool g_ctActive = false;
static int       g_ctCursor   = -1;      // -1 = unplaced, the picker model everywhere else
static int       g_ctSkillIdx = -1;      // the bar slot being spent
static int64_t   g_ctSkillElemId = 0;
static uintptr_t g_ctCls      = 0;
static char      g_ctName[256] = { 0 };

static void ctAbandon(const char* why) {
    if (!g_ctActive) return;
    g_ctActive = false;
    g_ctCursor = -1;
    logLine("camp target: abandoned (%s)", why ? why : "");
}

static void ctLandBar(uintptr_t base, int skillIdx) {
    ActionItem items[AB_MAX_ITEMS];
    int n = abBuildBar(base, items, AB_MAX_ITEMS);
    if (n <= 0) {
        logLine("camp target: no bar to return to after the commit");
        return;
    }
    abSetActive(base, true);                    // re-seeds the cursor, so the search runs after it
    for (int i = 0; i < n; i++)
        if (items[i].kind == AB_SKILL && items[i].skillIdx == skillIdx) { g_abCursor = i; break; }
    if (g_abCursor < 0 || g_abCursor >= n) g_abCursor = 0;   // the bar may have changed shape
}

// ---- THE RESPITE'S HERO ELEMENTS ----
static const uint32_t CAMP_HERO_ELEM_BASE = 0x6865726f;    // 'hero' + rank, 0-based

static int campHeroRank(uintptr_t hero) {
    uint32_t mask = 0;
    if (!hero || !safeReadU32(hero + ACTOR_RANK_MASK_OFF, &mask) || !mask) return -1;
    int r = 0;
    while (!((mask >> r) & 1)) r++;
    return r;
}
static int64_t campHeroElemId(uintptr_t hero) {
    int r = campHeroRank(hero);
    return r < 0 ? 0 : (int64_t)(CAMP_HERO_ELEM_BASE + (uint32_t)r);
}

static int ctTargetList(uintptr_t base, uintptr_t* heroes, int* vecIdx, int maxOut) {
    uintptr_t party[RV_MAX_MEMBERS];
    int n = campPartyVec(base, party, RV_MAX_MEMBERS);
    uintptr_t perf = abSelectedHero(base);
    int got = 0;
    for (int i = n - 1; i >= 0 && got < maxOut; i--) {
        if (!party[i]) continue;                    // a hole is skipped from the LIST, not the index
        if (party[i] == perf) continue;
        heroes[got] = party[i];
        vecIdx[got] = i;
        got++;
    }
    return got;
}

static void ctSpeakRow(uintptr_t base, uintptr_t* heroes, int n, int idx, const char* head) {
    char out[MAILBOX_SZ]; out[0] = 0;
    char frag[160];
    if (head && head[0]) _snprintf(out, sizeof out, "%s ", head);
    size_t len = strlen(out);
    if (abHeroLabelOf(base, heroes[idx], frag, sizeof frag))
        _snprintf(out + len, sizeof out - len, "%s", frag);
    else
        _snprintf(out + len, sizeof out - len, axs(AXS_CAMP_HERO_N_FMT), idx + 1);
    out[sizeof out - 1] = 0;
    if (abHeroHealth(base, heroes[idx], frag, sizeof frag)) {
        len = strlen(out);
        _snprintf(out + len, sizeof out - len, ". %s", frag);
    }
    if (abHeroStress(base, heroes[idx], frag, sizeof frag)) {
        len = strlen(out);
        _snprintf(out + len, sizeof out - len, ". %s", frag);
    }
    len = strlen(out);
    { _snprintf(out + len, sizeof out - len, ". "); len = strlen(out);
      _snprintf(out + len, sizeof out - len, axs(AXS_CAMP_TARGET_N_OF_M_FMT), idx + 1, n); }
    out[sizeof out - 1] = 0;
    postSpeech(out);
}

static bool ctClickCommit(uintptr_t base, uintptr_t perf, uintptr_t target, const char* who) {
    if (!g_ctSkillElemId) return false;
    if (clickQueued()) {
        logLine("camp target: click-commit not attempted, a click is already queued");
        return false;
    }
    int64_t heroId = campHeroElemId(target);
    uintptr_t heroEl = heroId ? feGetElementById(heroId) : 0;
    float tx = 0, ty = 0;
    if (!heroEl || !elemCenter(heroEl, &tx, &ty)) {
        logLine("camp target: click-commit not attempted, hero element 0x%llx (%s) is not on screen",
                (unsigned long long)heroId, who);
        return false;
    }
    int32_t armed = -1;
    safeReadU32(perf + ACTOR_CAMP_ARMED_OFF, (uint32_t*)&armed);
    if (armed == g_ctSkillIdx) {
        moveCursorTo(tx, ty);
        enqueueSynthAt(SDL_EVT_MOUSEBUTTONDOWN, SDL_BUTTON_LEFT, 1, 0, tx, ty, 1);
        enqueueSynthAt(SDL_EVT_MOUSEBUTTONUP,   SDL_BUTTON_LEFT, 0, 0, tx, ty, 2);
        logLine("camp target: slot %d is ALREADY armed -- one click on %s elem 0x%llx (%.0f,%.0f)",
                g_ctSkillIdx, who, (unsigned long long)heroId, tx, ty);
        return true;
    }
    uintptr_t skillEl = feGetElementById(g_ctSkillElemId);
    float sx = 0, sy = 0;
    if (!skillEl || !elemCenter(skillEl, &sx, &sy)) {
        logLine("camp target: click-commit not attempted, skill element 0x%llx is not on screen",
                (unsigned long long)g_ctSkillElemId);
        return false;
    }
    moveCursorTo(sx, sy);
    enqueueSynthAt(SDL_EVT_MOUSEBUTTONDOWN, SDL_BUTTON_LEFT, 1, 0, sx, sy, 1);
    enqueueSynthAt(SDL_EVT_MOUSEBUTTONUP,   SDL_BUTTON_LEFT, 0, 0, sx, sy, 2);
    enqueueSynthAt(SDL_EVT_MOUSEMOTION,     0, 0, 0, tx, ty, 4);   // hover the hero, then click
    enqueueSynthAt(SDL_EVT_MOUSEBUTTONDOWN, SDL_BUTTON_LEFT, 1, 0, tx, ty, 5);
    enqueueSynthAt(SDL_EVT_MOUSEBUTTONUP,   SDL_BUTTON_LEFT, 0, 0, tx, ty, 6);
    logLine("camp target: click-commit skill elem 0x%llx (%.0f,%.0f) -> %s elem 0x%llx (%.0f,%.0f)",
            (unsigned long long)g_ctSkillElemId, sx, sy, who, (unsigned long long)heroId, tx, ty);
    return true;
}

static void ctCommit(uintptr_t base, uintptr_t target, int targetVecIdx) {
    uintptr_t root = mapRoot(base);
    uintptr_t perf = abSelectedHero(base);
    int perfIdx = campPartyIndexOf(base, perf);
    if (!root || perfIdx < 0 || targetVecIdx < 0 || target == perf) {
        int back = g_ctSkillIdx;
        ctAbandon("the party moved under the aim");
        ctLandBar(base, back);                  // a refusal must not strand them in the bag either
        postSpeech(axs(AXS_CAMP_CANT_USE_SKILL));
        return;
    }
    char who[160];
    if (!abHeroLabelOf(base, target, who, sizeof who)) _snprintf(who, sizeof who, "%s", axs(AXS_THE_HERO_LC));
    who[sizeof who - 1] = 0;
    char full[320];
    _snprintf(full, sizeof full, axs(AXS_CAMP_USED_ON_FMT), g_ctName, who);
    full[sizeof full - 1] = 0;

    if (ctClickCommit(base, perf, target, who)) {
        int back = g_ctSkillIdx;
        ctAbandon("committed by click");
        ctLandBar(base, back);                  // back on the bar, on the skill just spent
        campArmUseWatch(full);
        return;
    }

    logLine("camp target: \"%s\" perf=%d skill=%d target=%d (%s), %d points before -- DIRECT CALL",
            g_ctName, perfIdx, g_ctSkillIdx, targetVecIdx, who, campPoints(base));
    bool called = sehCampPerform(base, root, perfIdx, g_ctSkillIdx, targetVecIdx);
    if (perf) safeWriteU32(perf + ACTOR_CAMP_ARMED_OFF, 0xffffffffu);
    int back = g_ctSkillIdx;                    // captured before anything can reset it
    ctAbandon("committed");
    ctLandBar(base, back);                      // back on the bar, on the skill just spent
    if (!called) {
        logLine("camp target: the perform call faulted");
        postSpeech(axs(AXS_CAMP_CANT_USE_SKILL));
        return;
    }
    campArmUseWatch(full);
}

static bool ctBegin(uintptr_t base, uintptr_t cls, int skillIdx, int64_t skillElemId,
                    const char* name) {
    uintptr_t heroes[RV_MAX_MEMBERS];
    int vidx[RV_MAX_MEMBERS];
    int n = ctTargetList(base, heroes, vidx, RV_MAX_MEMBERS);
    if (n <= 0) {
        logLine("camp target: no party to choose from");
        postSpeech(axs(AXS_CAMP_NO_ONE));
        return true;
    }
    g_ctCls      = cls;
    g_ctSkillIdx = skillIdx;
    g_ctSkillElemId = skillElemId;
    g_ctCursor   = -1;
    _snprintf(g_ctName, sizeof g_ctName, "%s", name ? name : axs(AXS_CAMP_SKILL_FALLBACK));
    g_ctName[sizeof g_ctName - 1] = 0;
    if (g_abActive) abSetActive(base, false);   // the bar and the picker both want Left/Right
    g_ctActive = true;

    char head[320];
    _snprintf(head, sizeof head, axs(AXS_CAMP_CHOOSE_TARGET_FMT), g_ctName, n);
    head[sizeof head - 1] = 0;
    logLine("camp target: begin \"%s\" slot %d targets=%d", g_ctName, skillIdx, n);
    postSpeech(head);
    return true;
}

bool routeCampTargetKey(uintptr_t base, uint32_t sym, uint16_t mod, uint8_t repeat) {
    if (mod & (KMOD_LALT | KMOD_RALT | KMOD_LCTRL | KMOD_RCTRL)) return false;

    if (sym == SDLK_ESCAPE) {
        if (repeat) return true;
        int back = g_ctSkillIdx;
        ctAbandon("cancelled");
        ActionItem items[AB_MAX_ITEMS];
        int n = abBuildBar(base, items, AB_MAX_ITEMS);
        if (n > 0) {
            abSetActive(base, true);            // re-seeds the cursor, so the search runs after it
            for (int i = 0; i < n; i++)
                if (items[i].kind == AB_SKILL && items[i].skillIdx == back) { g_abCursor = i; break; }
            if (g_abCursor < 0 || g_abCursor >= n) g_abCursor = 0;   // the bar may have changed shape
            abSpeakLabel(base, &items[g_abCursor], abSlotCount(items, n), "Cancelled.");
        } else {
            postSpeech(axs(AXS_CANCELLED));
        }
        return true;
    }

    int jump = 0;
    if (axDecodeJump(sym, mod, repeat, &jump)) {
        if (!jump) return true;                  // held jump: one landing per press
        uintptr_t jheroes[RV_MAX_MEMBERS];
        int jvidx[RV_MAX_MEMBERS];
        int jn = ctTargetList(base, jheroes, jvidx, RV_MAX_MEMBERS);
        if (jn <= 0) {
            ctAbandon("targets vanished");
            postSpeech(axs(AXS_CAMP_NO_ONE));
            return true;
        }
        g_ctCursor = (jump > 0) ? jn - 1 : 0;
        ctSpeakRow(base, jheroes, jn, g_ctCursor, nullptr);
        return true;
    }

    int dir = 0;
    bool confirm = false;
    switch (sym) {
        case SDLK_LEFT:   dir = -1; break;
        case SDLK_RIGHT:  dir =  1; break;
        case SDLK_RETURN:
        case SDLK_KP_ENTER: confirm = true; break;
        default: return false;
    }
    if (repeat) return true;

    uintptr_t heroes[RV_MAX_MEMBERS];
    int vidx[RV_MAX_MEMBERS];
    int n = ctTargetList(base, heroes, vidx, RV_MAX_MEMBERS);
    if (n <= 0) {
        ctAbandon("targets vanished");
        postSpeech(axs(AXS_CAMP_NO_ONE));
        return true;
    }
    if (g_ctCursor >= n) g_ctCursor = n - 1;

    if (confirm) {
        if (g_ctCursor < 0) { postSpeech(axs(AXS_CAMP_NO_TARGET_SELECTED)); return true; }
        ctCommit(base, heroes[g_ctCursor], vidx[g_ctCursor]);
        return true;
    }
    if (g_ctCursor < 0) g_ctCursor = 0;         // the first press lands rather than steps
    else axStepCursor(&g_ctCursor, n, dir);     // hard stops re-read, the list rule
    ctSpeakRow(base, heroes, n, g_ctCursor, nullptr);
    return true;
}

bool axIsCampTarget() { return g_ctActive; }

bool campUseSkill(uintptr_t base, const ActionItem* it) {
    uintptr_t hero = abSelectedHero(base);
    uintptr_t cls  = campBarSkillClass(base, it->skillIdx);
    char name[256];
    if (!cls || !csCampSkillName(base, cls, name, sizeof name))
        _snprintf(name, sizeof name, axs(AXS_CAMP_SKILL_N_FMT), it->skillIdx + 1);
    name[sizeof name - 1] = 0;

    if (cls) {
        const char* no = campBarRefusal(base, hero, cls, csCampSkillCost(cls));
        if (no) {
            logLine("camp: \"%s\" refused — %s", name, no);
            char msg[320];
            _snprintf(msg, sizeof msg, "%s. %s.", name, no);
            msg[sizeof msg - 1] = 0;
            postSpeech(msg);
            return true;
        }
    }

    if (campSkillNeedsTarget(cls)) {
        logLine("camp: \"%s\" needs an individual target -> picker", name);
        return ctBegin(base, cls, it->skillIdx, it->id, name);
    }

    uintptr_t root = mapRoot(base);
    int perfIdx = campPartyIndexOf(base, hero);
    if (!root || perfIdx < 0) {
        logLine("camp: \"%s\" — performer not in the party vector", name);
        postSpeech(axs(AXS_CAMP_CANT_USE_SKILL));
        return true;
    }
    if (!clickQueued() && frontEndClickElementId(it->id)) {
        logLine("camp: \"%s\" committed by CLICKING slot %d (elem 0x%llx), %d points before",
                name, it->skillIdx, (unsigned long long)it->id, campPoints(base));
        campArmUseWatch(name);
        return true;
    }
    logLine("camp: \"%s\" button not clickable this frame -> DIRECT CALL (perf=%d slot=%d, %d points before)",
            name, perfIdx, it->skillIdx, campPoints(base));
    if (!sehCampPerform(base, root, perfIdx, it->skillIdx, 0)) {
        logLine("camp: the perform call faulted");
        postSpeech(axs(AXS_CAMP_CANT_USE_SKILL));
        return true;
    }
    campArmUseWatch(name);
    return true;
}

// ---- THE CAMP: the MEAL screen ----

static volatile bool g_mealActive = false;
static int  g_mealCursor = -1;               // -1 = unplaced, the target-list model

static int mealCount(uintptr_t base) {
    int32_t n = 0;
    if (!safeReadU32(base + MEAL_COUNT_RVA, (uint32_t*)&n)) return 0;
    if (n < 1 || n > 16) return 0;           // vanilla ships four; a wild count is a bad read
    return (int)n;
}

static uintptr_t mealNode(uintptr_t base, int i) {
    uintptr_t head = 0;
    if (!safeReadPtr(base + MEAL_TABLE_RVA, &head) || head <= 0x10000) return 0;
    uintptr_t node = 0;
    if (!safeReadPtr(head + RBNODE_PARENT_OFF, &node) || node <= 0x10000) return 0;
    for (int guard = 0; guard < 64; guard++) {
        uint8_t nil = 1;
        if (!safeReadU8(node + RBNODE_ISNIL_OFF, &nil) || nil) return 0;
        int32_t key = 0;
        if (!safeReadU32(node + MEAL_KEY_OFF, (uint32_t*)&key)) return 0;
        if (key == i) return node;
        uintptr_t next = 0;
        if (!safeReadPtr(node + (key < i ? RBNODE_RIGHT_OFF : RBNODE_LEFT_OFF), &next)) return 0;
        if (next <= 0x10000) return 0;
        node = next;
    }
    logLine("camp meal: option map walk too deep — abandoned");
    return 0;
}

static bool mealFloat(uintptr_t node, uintptr_t off, float* out) {
    uint32_t raw = 0;
    if (!node || !safeReadU32(node + off, &raw)) return false;
    memcpy(out, &raw, 4);
    return true;
}

static float mealPartyRations(uintptr_t base) {
    uintptr_t party[RV_MAX_MEMBERS];
    int n = rvPartyList(base, party, RV_MAX_MEMBERS);
    float total = 0.0f;
    for (int i = 0; i < n; i++) {
        uint32_t raw = 0;
        float v = 0.0f;
        if (safeReadU32(party[i] + ACTOR_RATION_OFF, &raw)) memcpy(&v, &raw, 4);
        v += 1.0f;                                   // DAT_140b28d44
        if (v > 0.0f) total += v;                    // the game clamps each member at zero
    }
    return total;
}

static int mealFoodOwned(uintptr_t base) {
    uintptr_t beg = 0;
    int slots = 0;
    if (!invItemVector(base, &beg, &slots)) return -1;
    int total = 0;
    for (int i = 0; i < slots; i++) {
        uintptr_t item = beg + (uintptr_t)i * ITEM_STRIDE;
        int32_t amount = 0;
        if (!safeReadU32(item + ITEM_AMOUNT_OFF, (uint32_t*)&amount) || amount < 1) continue;
        char type[64] = { 0 };
        if (!safeReadCStr(item + ITEM_TYPE_OFF, type, sizeof type)) continue;
        if (strcmp(type, "provision") == 0) total += amount;
    }
    return total;
}

static int mealFoodNeeded(uintptr_t base, uintptr_t node) {
    float rations = 0.0f;
    if (!mealFloat(node, MEAL_RATIONS_OFF, &rations)) return -1;
    float need = floorf(mealPartyRations(base) * rations + 0.5f);
    if (need < 0.0f || need > 9999.0f) return -1;
    return (int)need;
}

static bool mealRowText(uintptr_t base, int i, char* out, int outsz) {
    int n = mealCount(base);
    uintptr_t node = mealNode(base, i);
    if (!node) return false;

    char name[128], key[64];
    _snprintf(key, sizeof key, "str_meal_title_%d", i);
    key[sizeof key - 1] = 0;
    if (!abTipPlain(base, key, name, sizeof name) || !name[0]) {
        logLine("camp meal: \"%s\" did not resolve", key);
        _snprintf(name, sizeof name, axs(AXS_OPTION_N), i + 1);
    }
    _snprintf(out, outsz, "%s", name);
    out[outsz - 1] = 0;

    int need = mealFoodNeeded(base, node);
    int have = mealFoodOwned(base);
    if (need == 0) {
        abAppend(out, outsz, "No food");
    } else if (need > 0) {
        char frag[96];
        _snprintf(frag, sizeof frag, axs(AXS_CAMP_FOOD_FMT), need);
        abAppend(out, outsz, frag);
        if (have >= 0 && have < need) {
            // The game's own wording for this, not ours — it draws it under the option.
            char no[128];
            if (abTipPlain(base, "str_meal_not_enough_provisions", no, sizeof no) && no[0]) {
                csTrimLabel(no);                     // it ships wrapped in brackets
                abAppend(out, outsz, no);
            } else {
                abAppend(out, outsz, "Not enough food");
            }
        }
    }

    float heal = 0.0f, stress = 0.0f;
    char frag[160];
    if (mealFloat(node, MEAL_HEALING_OFF, &heal) && heal != 0.0f) {
        int pct = (int)(heal * 100.0f);              // DAT_140b28e7c
        if (abTipInt(base, "str_meal_heal_format", pct, frag, sizeof frag)) abAppend(out, outsz, frag);
    }
    if (mealFloat(node, MEAL_STRESS_OFF, &stress) && stress != 0.0f) {
        if (abTipInt(base, "str_meal_stress_format", (int)stress, frag, sizeof frag))
            abAppend(out, outsz, frag);
    }

    if (n > 0) {
        _snprintf(frag, sizeof frag, axs(AXS_CAMP_OPTION_N_OF_M_FMT), i + 1, n);
        abAppend(out, outsz, frag);
    }
    size_t len = strlen(out);
    if (len && out[len - 1] != '.' && len + 1 < (size_t)outsz) { out[len] = '.'; out[len + 1] = 0; }
    return true;
}

static void mealSpeakRow(uintptr_t base, int i, const char* head = nullptr) {
    char row[MAILBOX_SZ];
    if (!mealRowText(base, i, row, sizeof row)) {
        logLine("camp meal: option %d produced no row", i);
        postSpeech(head && head[0] ? head : axs(AXS_CAMP_MEAL_UNREADABLE));
        return;
    }
    logLine("camp meal: row %d -> \"%s\"", i, row);
    if (head && head[0]) {
        char full[MAILBOX_SZ];
        _snprintf(full, sizeof full, "%s %s", head, row);   // one utterance: a separate head is
        full[sizeof full - 1] = 0;                          // built, queued and then destroyed
        postSpeech(full);
        return;
    }
    postSpeech(row);
}

static void mealSetActive(uintptr_t base, bool on) {
    if (g_mealActive == on) return;
    g_mealActive = on;
    if (!on) { g_mealCursor = -1; logLine("camp meal: closed"); return; }
    g_mealCursor = 0;
    logLine("camp meal: opened, %d options", mealCount(base));
}

bool routeMealKey(uintptr_t base, uint32_t sym, uint16_t mod, uint8_t repeat) {
    if (mod & (KMOD_LALT | KMOD_RALT | KMOD_LCTRL | KMOD_RCTRL)) return false;

    {
        int jump = 0;
        if (axDecodeJump(sym, mod, repeat, &jump)) {
            if (!jump) return true;              // held jump: one landing per press
            int n = mealCount(base);
            if (n <= 0) { postSpeech(axs(AXS_CAMP_MEAL_NONE)); return true; }
            axStepCursor(&g_mealCursor, n, jump);   // covers the -1 unplaced cursor: count > 0
                                                    // clamps it into range either way
            mealSpeakRow(base, g_mealCursor);
            return true;
        }
    }

    int dir = 0;
    bool confirm = false;
    switch (sym) {
        case SDLK_LEFT:   dir = -1; break;
        case SDLK_RIGHT:  dir =  1; break;
        case SDLK_RETURN:
        case SDLK_KP_ENTER: confirm = true; break;
        default: return false;
    }
    if (confirm) { if (repeat) return true; }
    else if (axNavHoldRepeat(repeat)) return true;   // throttled repeat: claimed, no step

    int n = mealCount(base);
    if (n <= 0) { postSpeech(axs(AXS_CAMP_MEAL_NONE)); return true; }
    if (g_mealCursor >= n) g_mealCursor = n - 1;
    if (g_mealCursor < 0)  g_mealCursor = 0;

    if (confirm) {
        uintptr_t node = mealNode(base, g_mealCursor);
        int need = node ? mealFoodNeeded(base, node) : -1;
        int have = mealFoodOwned(base);
        if (need > 0 && have >= 0 && have < need) {
            logLine("camp meal: option %d refused — needs %d food, party has %d",
                    g_mealCursor, need, have);
            char msg[200];
            _snprintf(msg, sizeof msg, axs(AXS_CAMP_NOT_ENOUGH_FOOD_FMT), need, have);
            msg[sizeof msg - 1] = 0;
            postSpeech(msg);
            return true;
        }
        int64_t id = (int64_t)(uint32_t)(MEAL_FOURCC_BASE + (uint32_t)g_mealCursor);
        logLine("camp meal: choosing option %d (element 0x%llx)", g_mealCursor,
                (unsigned long long)id);
        if (!frontEndClickElementId(id)) {
            postSpeech(axs(AXS_CAMP_MEAL_FAILED));
            return true;
        }
        return true;
    }

    axStepCursor(&g_mealCursor, n, dir);
    mealSpeakRow(base, g_mealCursor);
    return true;
}

bool axIsMeal() { return g_mealActive; }

void mealReannounce(uintptr_t base) {
    if (!g_mealActive) return;
    int i = g_mealCursor < 0 ? 0 : g_mealCursor;
    char head[160];
    if (!abTipPlain(base, "str_ui_meal_title", head, sizeof head) || !head[0])
        _snprintf(head, sizeof head, "%s", axs(AXS_CAMP_MEAL_TITLE));
    head[sizeof head - 1] = 0;
    size_t len = strlen(head);
    _snprintf(head + len, sizeof head - len, ".");
    head[sizeof head - 1] = 0;
    logLine("camp meal: re-announcing after a modal closed");
    mealSpeakRow(base, i, head);
}

// ---- THE CAMP: the per-frame watcher ----
static int g_campPhaseSeen  = 0;
static int g_campPointsSeen = -1;

bool campPointsText(uintptr_t base, char* out, int outsz) {
    int pts = campPoints(base);
    if (pts < 0) return false;
    _snprintf(out, outsz, axs(AXS_CAMP_POINTS_FMT), pts);
    out[outsz - 1] = 0;
    return true;
}

static void campEnterSkillBar(uintptr_t base, const char* head) {
    ActionItem items[AB_MAX_ITEMS];
    int n = abBuildBar(base, items, AB_MAX_ITEMS);
    if (n <= 0) {
        logLine("camp: respite phase but no bar on screen");
        if (head && head[0]) postSpeech(head);
        return;
    }
    if (g_rvActive) rvSetActive(base, false);
    if (g_qtActive) qtSetActive(base, false);
    if (g_tsActive) { tsSetActive(false); logLine("targeting: abandoned (camp opened)"); }
    iuAbandon("camp opened");
    abSetActive(base, true);
    g_abCursor = 0;
    abSpeakLabel(base, &items[0], abSlotCount(items, n), head);
}

void serviceCamp(uintptr_t base) {
    int phase = campPhase(base);

    if (phase == g_campPhaseSeen) {
        if (phase == CAMP_PHASE_RESPITE) {
            int pts = campPoints(base);
            if (pts >= 0 && pts != g_campPointsSeen) {
                logLine("camp: respite points %d -> %d", g_campPointsSeen, pts);
                g_campPointsSeen = pts;
                char msg[MAILBOX_SZ], pt[96];
                campPointsText(base, pt, sizeof pt);
                if (g_campUseDeadline && g_campUseName[0])
                    _snprintf(msg, sizeof msg, axs(AXS_CAMP_USED_LEFT_FMT), g_campUseName, pt);
                else
                    _snprintf(msg, sizeof msg, axs(AXS_CAMP_LEFT_FMT), pt);
                msg[sizeof msg - 1] = 0;
                g_campUseDeadline = 0;
                g_campUseName[0]  = 0;
                postSpeech(msg);
            } else if (g_campUseDeadline && GetTickCount() > g_campUseDeadline) {
                logLine("camp: \"%s\" pressed but respite points never moved (still %d)",
                        g_campUseName, campPoints(base));
                char msg[MAILBOX_SZ];
                _snprintf(msg, sizeof msg, axs(AXS_CAMP_NOTHING_YET_FMT),
                          g_campUseName[0] ? g_campUseName : "That camping skill");
                msg[sizeof msg - 1] = 0;
                g_campUseDeadline = 0;
                g_campUseName[0]  = 0;
                postSpeech(msg);
            }
        }
        return;
    }

    logLine("camp: phase %d -> %d (points %d)", g_campPhaseSeen, phase, campPoints(base));
    int prev = g_campPhaseSeen;
    g_campPhaseSeen = phase;
    g_campCovers    = (phase != 0);

    if (phase != CAMP_PHASE_MEAL && g_mealActive) mealSetActive(base, false);

    if (phase != CAMP_PHASE_RESPITE) {
        g_campUseDeadline = 0;
        g_campUseName[0]  = 0;
        if (g_ctActive) ctAbandon("the respite phase ended");
    }

    if (phase == 0) {
        g_campPointsSeen = -1;
        return;
    }

    if (phase == CAMP_PHASE_MEAL) {
        mealSetActive(base, true);
        char head[160];
        if (!abTipPlain(base, "str_ui_meal_title", head, sizeof head) || !head[0])
            _snprintf(head, sizeof head, "%s", axs(AXS_CAMP_MEAL_TITLE));
        size_t len = strlen(head);
        { _snprintf(head + len, sizeof head - len, ". "); _snprintf(head + strlen(head), sizeof head - strlen(head), "%s", axs(AXS_CAMP_MEAL_CHOOSE)); }
        head[sizeof head - 1] = 0;
        mealSpeakRow(base, 0, head);          // header + row 0 in ONE utterance
        return;
    }

    if (phase == CAMP_PHASE_RESPITE) {
        g_campPointsSeen = campPoints(base);
        char head[192], pts[96];
        if (campPointsText(base, pts, sizeof pts)) _snprintf(head, sizeof head, axs(AXS_CAMP_HEAD_FMT), pts);
        else                                       _snprintf(head, sizeof head, "%s", axs(AXS_CAMP_HEAD_BARE));
        head[sizeof head - 1] = 0;
        campEnterSkillBar(base, head);
        return;
    }

    if (phase == 7 && prev == CAMP_PHASE_RESPITE) postSpeech(axs(AXS_CAMP_RESTING));
}
