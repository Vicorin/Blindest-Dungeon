// THE BUTCHER'S CIRCUS -- THE PIT

#include <windows.h>
#include <cstdio>
#include <cstring>
#include "internal.h"
#include "game/offsets.h"

// ---- is this a Circus match? ----
static const uintptr_t PIT_QUEST_OFF = 0x52c0;   // raid root+ -> Quest*  (was 0x5250)
static const char      PIT_ARENA_ID[] = "arena_mp";

static uintptr_t pitBattle(uintptr_t root) { return root ? root + BATTLE_OBJ_OFF : 0; }

bool pitInMatch(uintptr_t base) {
    uintptr_t root = mapRoot(base);
    if (!root) return false;
    uintptr_t quest = 0;
    if (!safeReadPtr(root + PIT_QUEST_OFF, &quest) || !quest) return false;
    char id[32];
    if (!safeReadCStr(quest, id, sizeof id) || !id[0]) return false;
    return strcmp(id, PIT_ARENA_ID) == 0;
}

// ---- the game's own two calls ----
typedef char (__fastcall *PitGateFn)(uintptr_t battle, uintptr_t hero);
typedef char (__fastcall *PitActivateFn)(uintptr_t battle, uintptr_t hero);
typedef void (__fastcall *PitSelectFn)(uintptr_t battle, uintptr_t hero);

bool pitCanActivate(uintptr_t base, uintptr_t hero) {
    uintptr_t root = mapRoot(base);
    if (!root || !hero || !pitInMatch(base)) return false;
    __try {
        return ((PitGateFn)(base + PIT_CAN_ACTIVATE_RVA))(pitBattle(root), hero) != 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        logLine("pit: the activation gate faulted for hero=%p", (void*)hero);
        return false;
                               // ActivateHero on a battle we could not ask about.
    }
}

static bool pitSehActivate(uintptr_t base, uintptr_t battle, uintptr_t hero) {
    __try { ((PitActivateFn)(base + PIT_ACTIVATE_HERO_RVA))(battle, hero); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        logLine("pit: Battle::ActivateHero faulted for hero=%p", (void*)hero);
        return false;
    }
}

static bool pitSehSelect(uintptr_t base, uintptr_t battle, uintptr_t hero) {
    __try { ((PitSelectFn)(base + PIT_SELECT_HERO_RVA))(battle, hero); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        logLine("pit: the selection writer faulted for hero=%p", (void*)hero);
        return false;
    }
}

static bool pitSomeoneActing(uintptr_t root) {
    uint8_t b = 0;
    if (!root || !safeReadU8(pitBattle(root) + BATTLE_ACTIVATED_OFF, &b)) return false;
    return b != 0;
}

bool pitPickOpen(uintptr_t base) {
    if (!pitInMatch(base)) return false;
    uintptr_t party[RV_MAX_MEMBERS];
    int n = rvPartyList(base, party, RV_MAX_MEMBERS);
    for (int i = 0; i < n; i++)
        if (pitCanActivate(base, party[i])) return true;
    return false;
}

static bool pitHeroName(uintptr_t base, uintptr_t hero, char* out, int outsz) {
    out[0] = 0;
    if (!abHeroLabelOf(base, hero, out, outsz) || !out[0]) return false;
    return true;
}

// ---- the row marker ----
void pitRowSuffix(uintptr_t base, uintptr_t hero, char* out, int outsz) {
    out[0] = 0;
    if (!hero || !pitInMatch(base)) return;
    uintptr_t root = mapRoot(base);
    if (pitSomeoneActing(root)) return;            // the pick is over; every row would say it
    if (pitCanActivate(base, hero)) return;        // the common case, and it stays silent
    _snprintf(out, outsz, "%s", axs(AXS_PIT_ROW_CANT_ACT));
    out[outsz - 1] = 0;
}

// ---- ENTER: activate the hero under the cursor ----
static uintptr_t g_pitPendHero  = 0;      // whose activation we are waiting to land
static char      g_pitPendName[96] = {0};
static DWORD     g_pitPendUntil = 0;      // 0 = idle
static const DWORD PIT_BAR_WATCH_MS = 4000;   // the skip-turn watch's measured budget: the
                                              // lock-in has an animation in front of it

void pitReset() {
    g_pitPendHero = 0; g_pitPendName[0] = 0; g_pitPendUntil = 0;
}

static void pitSayFailed(const char* name) {
    char line[192];
    _snprintf(line, sizeof line, axs(AXS_PIT_ACTIVATE_FAILED_FMT), name);
    line[sizeof line - 1] = 0;
    postSpeech(line);
}

bool pitActivateHero(uintptr_t base, uintptr_t hero) {
    if (!pitInMatch(base) || !hero) return false;      // not our fight -- the game's key
    uintptr_t root = mapRoot(base);

    char name[96];
    if (!pitHeroName(base, hero, name, sizeof name)) {
        _snprintf(name, sizeof name, "%s", axs(AXS_RV_LABEL_PARTY_MEMBER));
        name[sizeof name - 1] = 0;
    }

    if (!pitCanActivate(base, hero)) {
        if (pitSomeoneActing(root)) {
            char who[96];
            uintptr_t turn = abCurrentTurnActor(root);
            if (turn && pitHeroName(base, turn, who, sizeof who)) {
                char line[192];
                _snprintf(line, sizeof line, axs(AXS_PIT_ALREADY_ACTING_FMT), who);
                line[sizeof line - 1] = 0;
                postSpeech(line);
            } else {
                postSpeech(axs(AXS_PIT_ALREADY_ACTING));
            }
            logLine("pit: Enter refused, a hero is already acting (turn=%p)", (void*)turn);
        } else {
            char line[192];
            _snprintf(line, sizeof line, axs(AXS_PIT_CANT_ACT_FMT), name);
            line[sizeof line - 1] = 0;
            postSpeech(line);
            logLine("pit: Enter refused by the game's gate, hero=%p \"%s\"", (void*)hero, name);
        }
        return true;
    }

    uint8_t before = 0;
    safeReadU8(pitBattle(root) + BATTLE_ACTIVATED_OFF, &before);

    if (!pitSehActivate(base, pitBattle(root), hero)) {
        pitSayFailed(name);
        return true;
    }

    uint8_t after = 0;
    safeReadU8(pitBattle(root) + BATTLE_ACTIVATED_OFF, &after);
    if (after == 0 || after == before) {
        logLine("pit: ActivateHero ran but the battle did not move (byte %u -> %u), hero=%p",
                (unsigned)before, (unsigned)after, (void*)hero);
        pitSayFailed(name);
        return true;
    }

    logLine("pit: activated hero=%p \"%s\" (byte %u -> %u) -- waiting for the bar",
            (void*)hero, name, (unsigned)before, (unsigned)after);
    g_pitPendHero  = hero;
    _snprintf(g_pitPendName, sizeof g_pitPendName, "%s", name);
    g_pitPendName[sizeof g_pitPendName - 1] = 0;
    g_pitPendUntil = GetTickCount() + PIT_BAR_WATCH_MS;
    if (!g_pitPendUntil) g_pitPendUntil = 1;   // 0 is this watch's "idle", so never store it
    return true;
}

// ---- COMMITTING A TURN THROUGH THE GAME'S OWN STATE MACHINE ----
// ---- ⚠⚠ FINDING 4: PARKING THE TARGET VECTOR SKIPS THE DRAW AFTER ALL ----
typedef void (__fastcall *PitVecAssignFn)(uintptr_t dst, const void* srcBegin, size_t count);
typedef void (__fastcall *PitSetSingleTargetFn)(uintptr_t battle, uintptr_t skill,
                                                uintptr_t performer, uintptr_t target);

static bool pitSehVecAssign(uintptr_t base, uintptr_t dst, const void* src, size_t n) {
    __try { ((PitVecAssignFn)(base + MP_VEC_ASSIGN_RVA))(dst, src, n); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        logLine("pit: the target-vector assign faulted (dst=%p n=%d)", (void*)dst, (int)n);
        return false;
    }
}

static bool pitSehSetSingleTarget(uintptr_t base, uintptr_t battle, uintptr_t skill,
                                  uintptr_t perf, uintptr_t target) {
    __try {
        ((PitSetSingleTargetFn)(base + SET_SINGLE_TARGET_RVA))(battle, skill, perf, target);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        logLine("pit: SetSingleTarget faulted (battle=%p skill=%p perf=%p target=%p)",
                (void*)battle, (void*)skill, (void*)perf, (void*)target);
        return false;
    }
}

static const uintptr_t PIT_BATTLE_CHOICE_PARKED_OFF = 0x1eb;

bool pitCommitViaMachine(uintptr_t base, uintptr_t perf, uintptr_t skill,
                         const uintptr_t* targets, int nTargets) {
    if (!pitInMatch(base)) return false;
    if (!perf || !skill) {
        logLine("pit: cannot park the turn -- performer=%p skill=%p", (void*)perf, (void*)skill);
        return false;
    }
    uintptr_t root = mapRoot(base);
    uintptr_t battle = pitBattle(root);

    uint32_t state = 0;
    safeReadU32(root + RAID_IN_COMBAT_OFF, &state);
    if (state != 0x1d && state != 0x1e) {    // bs_turn_select_skill / _select_targets only
        logLine("pit: refusing the park at battle state 0x%x (0x1c would eat the targets)", state);
        return false;
    }

    // The same two writes the game's own committers make before raising the flag.
    if (!safeWriteU64(perf + ACTOR_CHOSEN_SKILL_OFF, (uint64_t)skill)) {
        logLine("pit: could not write the chosen skill to %p+0x%x", (void*)perf,
                (unsigned)ACTOR_CHOSEN_SKILL_OFF);
        return false;
    }
    bool viaSetter = false;
    if (nTargets == 1 && targets && targets[0] &&
        pitSehSetSingleTarget(base, battle, skill, perf, targets[0])) {
        viaSetter = true;
    } else if (!pitSehVecAssign(base, perf + ACTOR_TARGETS_OFF, targets, (size_t)nTargets)) {
        return false;
    }

    uintptr_t backSkill = 0, tBegin = 0, tEnd = 0;
    safeReadPtr(perf + ACTOR_CHOSEN_SKILL_OFF, &backSkill);
    safeReadPtr(perf + ACTOR_TARGETS_OFF, &tBegin);
    safeReadPtr(perf + ACTOR_TARGETS_OFF + 8, &tEnd);
    int backCount = (tEnd >= tBegin) ? (int)((tEnd - tBegin) / sizeof(uintptr_t)) : -1;
    if (backSkill != skill || backCount != nTargets) {
        if (viaSetter && nTargets == 1) {
            logLine("pit: SetSingleTarget left %d target(s), not %d -- repairing with the vector "
                    "assign (this turn will NOT have made the draw)", backCount, nTargets);
            if (!pitSehVecAssign(base, perf + ACTOR_TARGETS_OFF, targets, (size_t)nTargets)) return false;
            viaSetter = false;
            safeReadPtr(perf + ACTOR_TARGETS_OFF, &tBegin);
            safeReadPtr(perf + ACTOR_TARGETS_OFF + 8, &tEnd);
            backCount = (tEnd >= tBegin) ? (int)((tEnd - tBegin) / sizeof(uintptr_t)) : -1;
        }
        if (backSkill != skill || backCount != nTargets) {
            logLine("pit: the park did not take on the actor (skill %p -> %p, targets %d -> %d)",
                    (void*)skill, (void*)backSkill, nTargets, backCount);
            return false;
        }
    }

    if (viaSetter && nTargets == 1) {
        uintptr_t landed = 0;
        safeReadPtr(tBegin, &landed);
        if (landed && landed != targets[0])
            logLine("pit: the game's scramble roll redirected the attack, %p -> %p",
                    (void*)targets[0], (void*)landed);
    }

    if (!safeWriteU8(battle + PIT_BATTLE_CHOICE_PARKED_OFF, 1)) {
        logLine("pit: could not raise the commit flag at battle+0x%x",
                (unsigned)PIT_BATTLE_CHOICE_PARKED_OFF);
        return false;
    }

    uint32_t mask = 0;
    safeReadU32(perf + ACTOR_RANK_MASK_OFF, &mask);
    logLine("pit: turn parked for the machine -- performer=%p (rank mask 0x%x) skill=%p targets=%d "
            "[state 0x%x] via %s", (void*)perf, mask, (void*)skill, nTargets, state,
            viaSetter ? "SetSingleTarget (the draw was made)" : "the vector assign (NO draw)");
    return true;
}

bool pitBattleAcceptsCommand(uintptr_t base, uint32_t* stateOut) {
    if (stateOut) *stateOut = 0;
    if (!pitInMatch(base)) return true;
    uintptr_t root = mapRoot(base);
    uint32_t state = 0;
    if (!root || !safeReadU32(root + RAID_IN_COMBAT_OFF, &state)) return true;  // never on a bad read
    if (stateOut) *stateOut = state;
    return (int)state >= BS_TURN_CHOOSING_LO && (int)state <= BS_TURN_CHOOSING_HI;
}

// ---- C: read a hero while the pick is open ----
bool pitInspectHero(uintptr_t base, uintptr_t hero) {
    if (!hero || !pitPickOpen(base)) return false;     // not this surface -- leave the key alone
    uintptr_t root = mapRoot(base);
    if (!pitSehSelect(base, pitBattle(root), hero)) return true;   // it faulted; the key was ours
    logLine("pit: C -> selection moved to hero=%p, opening the sheet", (void*)hero);
    if (!abToggleCharSheet(base)) {
        logLine("pit: ToggleCharacterDisplay faulted");
        postSpeech(axs(AXS_AB_SHEET_DIDNT_OPEN));
    }
    return true;
}

// ---- the per-frame watch ----
static bool g_pitPickWasOpen = false;

void servicePit(uintptr_t base) {
    if (!pitInMatch(base)) {
        g_pitPickWasOpen = false;
        if (g_pitPendUntil) logLine("pit: the match ended with an activation still pending");
        pitReset();
        return;
    }
    uintptr_t root = mapRoot(base);
    DWORD now = GetTickCount();

    if (g_pitPendUntil) {
        uintptr_t turn = abCurrentTurnActor(root);
        uintptr_t sel  = abSelectedHero(base);
        if (!g_rvActive) {
            char line[192];
            _snprintf(line, sizeof line, axs(AXS_PIT_ACTING_FMT), g_pitPendName);
            line[sizeof line - 1] = 0;
            logLine("pit: the player left the dungeon view -- not stepping into the bar");
            pitReset();
            postSpeech(line);
        } else if (turn == g_pitPendHero && sel == g_pitPendHero) {
            char head[192];
            _snprintf(head, sizeof head, axs(AXS_PIT_ACTING_HEAD_FMT), g_pitPendName);
            head[sizeof head - 1] = 0;
            logLine("pit: the bar is the activated hero's -- stepping into it");
            pitReset();
            abEnterFromRoom(base, head);     // speaks the head AND the first slot, in one line
        } else if ((int)(now - g_pitPendUntil) >= 0) {
            logLine("pit: the bar never became the activated hero's (turn=%p sel=%p want=%p)",
                    (void*)turn, (void*)sel, (void*)g_pitPendHero);
            char line[192];
            _snprintf(line, sizeof line, axs(AXS_PIT_ACTING_FMT), g_pitPendName);
            line[sizeof line - 1] = 0;
            postSpeech(line);
            pitReset();
        }
        return;
    }

    bool open = pitPickOpen(base);
    if (open && !g_pitPickWasOpen) {
        char caption[160], line[224];
        if (!resolveKey(base, PIT_SELECT_HERO_KEY, caption, sizeof caption) || !caption[0]) {
            logLine("pit: \"%s\" did not resolve -- using the mod's own line", PIT_SELECT_HERO_KEY);
            _snprintf(line, sizeof line, "%s", axs(AXS_PIT_PICK_OPEN_FALLBACK));
            line[sizeof line - 1] = 0;
        } else {
            abStripMarkup(caption);
            _snprintf(line, sizeof line, axs(AXS_RV_PRESS_ENTER_FMT), caption);
            line[sizeof line - 1] = 0;
        }
        logLine("pit: the pick is open -> \"%s\"", line);
        postSpeech(line, false);
    }
    g_pitPickWasOpen = open;
}
