// COLOR OF MADNESS — the Farmstead's WAVE raid (the Endless Harvest).

#include "internal.h"
#include "game/offsets.h"

#include <stdio.h>
#include <string.h>

static const uintptr_t COM_MAP_ROOT_RVA   = MAP_ROOT_RVA;  // ptr -> Raid/Map state (0 outside a raid)
static const uintptr_t COM_RAID_QUEST_OFF = 0x52c0;    // raidRoot+: Quest* (was 0x5250)
static const uintptr_t COM_QUEST_KIND_OFF = 0x108;     // Quest+: kind record (its id is at +0x40)
static const uintptr_t COM_KIND_ID_OFF    = 0x40;      // kind+: the id hash

static const uintptr_t COM_WAVELOGIC_OFF  = 0x5f38;    // raidRoot+: Wave::WaveLogic* (was 0x5ec8)

static const uintptr_t WL_SHARD_MULT_OFF   = 0x0c0;  // "shardRewardMultiplier"  (float)
static const uintptr_t WL_KILL_COUNT_OFF   = 0x0c4;  // "killCount"      — the whole raid
static const uintptr_t WL_AREA_KILLS_OFF   = 0x0c8;
static const uintptr_t WL_CURR_ROOM_OFF    = 0x21c;  // "currRoom"       — index into the 2-room list
static const uintptr_t WL_WAVE_STATE_OFF   = 0x264;  // "currWaveState"
static const uintptr_t WL_NEXT_ACTION_OFF  = 0x268;  // "nextWaveActionType" — 0 mash, 1 boss, 2 curio
static const uintptr_t WL_WAVES_DONE_OFF   = 0x26c;  // "wavesCompleted"
static const uintptr_t WL_LAST_REWARD_OFF  = 0x2d8;  // "lastWaveGroupRewarded"
static const uintptr_t WL_BOSS_SPAWNED_OFF = 0x318;  // "bossSpawned"
static const uintptr_t WL_CURIO_SPAWN_OFF  = 0x2f8;  // "curioSpawned"
static const uintptr_t WL_CAN_SKIP_OFF     = 0x219;  // "canSkipCurio"
static const uintptr_t WL_TRANSITION_OFF   = 0x374;  // "transitionToNewArea" — THE trigger
static const uintptr_t WL_TRANS_TIMER_OFF  = 0x378;  // the delay it is armed with, always 0.33 s

static const uint32_t  WL_STATE_AWAIT_MOVE = 2;

static const uint32_t  WL_TRANS_DELAY_BITS = 0x3ea8f5c3;

// ---- THE KILL METER (the "wave progression" strip) ----
// ---- THE WAVE'S OWN KILL TARGET — `spawnsUntilCurio`, +0x2fc ----
static const uintptr_t WL_KILL_TARGET_OFF  = 0x2fc;  // "spawnsUntilCurio" — kills to end THIS wave

static const uintptr_t WL_GROUPS_BEG_OFF   = 0x270;  // vector<vector<WaveInfo>> begin
static const uintptr_t WL_GROUPS_END_OFF   = 0x278;  // ... end
static const uintptr_t WL_GROUP_STRIDE     = 0x18;   // one std::vector = {begin, end, cap}
static const uintptr_t WL_WAVEINFO_STRIDE  = 0x8c;   // one WaveInfo, as the widget steps it

static const int       COM_TIER_LAST        = 3;     // the widget's own clamp: min(banked, 3)
#define COM_TIER_KEY_PREFIX "str_wave_progression_reward_tier"
static const char* const kComTierKeys[4] = {
    "str_wave_progression_reward_tier_small_tooltip",
    "str_wave_progression_reward_tier_medium_tooltip",
    "str_wave_progression_reward_tier_large_tooltip",
    "str_wave_progression_reward_tier_wonderous_tooltip",
};

static const uintptr_t COM_QUEST_THRESH_BEG = 0x2c8;
static const uintptr_t COM_QUEST_THRESH_END = 0x2d0;  // Quest+: end                          (was 0x280)
static const uintptr_t COM_THRESH_STRIDE    = 0x170;  // one ThresholdReward                  (was 0x120)

typedef unsigned long long (*ComThresholdFn)(uintptr_t);

typedef char (*ComCanAdvanceFn)(uintptr_t);

uintptr_t comRaidRoot(uintptr_t base) {
    uintptr_t root = 0;
    if (!safeReadPtr(base + COM_MAP_ROOT_RVA, &root) || root <= 0x10000) return 0;
    return root;
}

static uint32_t comQuestKindId(uintptr_t base) {
    uintptr_t root = comRaidRoot(base);
    if (!root) return 0;
    uintptr_t q = 0, kind = 0;
    if (!safeReadPtr(root + COM_RAID_QUEST_OFF, &q) || q <= 0x10000) return 0;
    if (!safeReadPtr(q + COM_QUEST_KIND_OFF, &kind) || kind <= 0x10000) return 0;
    uint32_t id = 0;
    if (!safeReadU32(kind + COM_KIND_ID_OFF, &id)) return 0;
    return id;
}

bool comIsWaveRaid(uintptr_t base) {
    uint32_t got = comQuestKindId(base);
    if (!got) return false;
    uint32_t want = 0;
    if (!safeReadU32(base + COM_WAVE_QID_RVA, &want) || !want) return false;
    return got == want;
}

uintptr_t comWaveLogic(uintptr_t base) {
    if (!comIsWaveRaid(base)) return 0;
    uintptr_t root = comRaidRoot(base);
    if (!root) return 0;
    uintptr_t wl = 0;
    if (!safeReadPtr(root + COM_WAVELOGIC_OFF, &wl) || wl <= 0x10000) return 0;
    return wl;
}

bool comWaveThresholds(uintptr_t base, int* reachedOut, int* totalOut) {
    if (reachedOut) *reachedOut = 0;
    if (totalOut)   *totalOut   = 0;
    uintptr_t wl = comWaveLogic(base);
    uintptr_t root = comRaidRoot(base);
    if (!wl || !root) return false;

    uintptr_t q = 0, tb = 0, te = 0;
    if (!safeReadPtr(root + COM_RAID_QUEST_OFF, &q) || q <= 0x10000) return false;
    if (!safeReadPtr(q + COM_QUEST_THRESH_BEG, &tb) ||
        !safeReadPtr(q + COM_QUEST_THRESH_END, &te) || !tb || te < tb) return false;
    uintptr_t span = te - tb;
    if (span % COM_THRESH_STRIDE) return false;
    int total = (int)(span / COM_THRESH_STRIDE);
    if (total < 0 || total > 64) return false;

    unsigned long long reached = 0;
    ComThresholdFn fn = (ComThresholdFn)(base + COM_THRESHOLD_FN_RVA);
    __try { reached = fn(wl); }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        logLine("com wave: the threshold fn FAULTED — reward tier not reported");
        return false;
    }
    int r = (int)reached;
    if (r < 0) r = 0;
    if (r > total) r = total;      // ShowRetreatButton clamps the same way
    if (reachedOut) *reachedOut = r;
    if (totalOut)   *totalOut   = total;
    return true;
}

static bool comWaveKills(uintptr_t wl, int* doneOut, int* targetOut) {
    if (doneOut)   *doneOut   = -1;
    if (targetOut) *targetOut = -1;
    if (!wl) return false;
    uint32_t done = 0, target = 0;
    if (!safeReadU32(wl + WL_AREA_KILLS_OFF,  &done))   return false;
    if (!safeReadU32(wl + WL_KILL_TARGET_OFF, &target)) return false;
    if ((int)target <= 0 || (int)target > 1000) {
        logLine("com wave: spawnsUntilCurio reads %d — no wave target yet, kill fraction dropped",
                (int)target);
        return false;
    }
    if ((int)done < 0 || (int)done > 100000) return false;
    if (doneOut)   *doneOut   = (int)done;
    if (targetOut) *targetOut = (int)target;
    return true;
}

bool comCanAdvance(uintptr_t base) {
    uintptr_t wl = comWaveLogic(base);           // already gated on "this is a wave raid"
    if (!wl) return false;
    uint32_t state = 0;
    if (!safeReadU32(wl + WL_WAVE_STATE_OFF, &state) || state != WL_STATE_AWAIT_MOVE) return false;

    char ok = 0;
    ComCanAdvanceFn fn = (ComCanAdvanceFn)(base + COM_CAN_ADVANCE_FN_RVA);
    __try { ok = fn(wl); }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        logLine("com wave: the may-advance predicate FAULTED — the way on is not offered");
        return false;
    }
    return ok != 0;
}

bool comAdvanceRoom(uintptr_t base) {
    uintptr_t wl = comWaveLogic(base);
    if (!wl) return false;
    if (!comCanAdvance(base)) {
        logLine("com wave: advance refused — the game's own predicate says no");
        return false;
    }
    uint8_t already = 0;
    if (safeReadU8(wl + WL_TRANSITION_OFF, &already) && already) {
        logLine("com wave: advance ignored — a transition is already under way");
        return false;
    }
    if (!safeWriteU32(wl + WL_TRANS_TIMER_OFF, WL_TRANS_DELAY_BITS)) {
        logLine("com wave: could not arm the transition delay at WaveLogic+0x%llx",
                (unsigned long long)WL_TRANS_TIMER_OFF);
        return false;
    }
    if (!safeWriteU8(wl + WL_TRANSITION_OFF, 1)) {
        logLine("com wave: could not set transitionToNewArea at WaveLogic+0x%llx",
                (unsigned long long)WL_TRANSITION_OFF);
        return false;
    }
    logLine("com wave: transitionToNewArea armed (WaveLogic=%p)", (void*)wl);
    return true;
}

void comWaveMoveSnapshot(uintptr_t base, ComMoveSnapshot* out) {
    if (!out) return;
    ComMoveSnapshot s = ComMoveSnapshot();
    uintptr_t wl = comWaveLogic(base);
    if (!wl) { *out = s; return; }
    s.valid = true;
    safeReadU8 (wl + WL_TRANSITION_OFF,  &s.transition);
    safeReadU8 (wl + WL_CURIO_SPAWN_OFF, &s.curioSpawned);
    safeReadU32(wl + WL_NEXT_ACTION_OFF, &s.nextAction);
    safeReadU32(wl + WL_CURR_ROOM_OFF,   &s.currRoom);
    *out = s;
}

bool comWaveMoveHappened(const ComMoveSnapshot* before, const ComMoveSnapshot* now) {
    if (!before || !now || !before->valid) return false;
    if (!now->valid) return true;
    return before->transition   != now->transition
        || before->curioSpawned != now->curioSpawned
        || before->nextAction   != now->nextAction
        || before->currRoom     != now->currRoom;
}

bool comAdvanceLabel(uintptr_t base, char* out, int outsz) {
    if (!out || outsz <= 0) return false;
    out[0] = 0;
    char raw[256];
    if (resolveKey(base, "str_skip_curio_overlay_text", raw, sizeof raw) && raw[0]) {
        char clean[256];
        stripMarkup(raw, clean, sizeof clean);
        if (clean[0]) {
            _snprintf(out, outsz, "%s", clean);
            out[outsz - 1] = 0;
            return true;
        }
    }
    logLine("com wave: str_skip_curio_overlay_text did not resolve — using the mod's own wording");
    _snprintf(out, outsz, "%s", axs(AXS_WAVE_ADVANCE_FALLBACK));
    out[outsz - 1] = 0;
    return true;
}

bool comWaveRetreatKey(uintptr_t base, char* out, int outsz) {
    if (!out || outsz <= 0) return false;
    out[0] = 0;
    if (!comIsWaveRaid(base)) return false;
    int reached = 0, total = 0;
    if (comWaveThresholds(base, &reached, &total) && reached - 1 >= 0)
        _snprintf(out, outsz, "retreat_wave_tooltip_%d", reached - 1);
    else
        _snprintf(out, outsz, "retreat_wave_tooltip");
    out[outsz - 1] = 0;
    return true;
}

// ---- comWaveStatus, RETIRED 2026-08-12 ----

static int comBankedRaw(uintptr_t base, uintptr_t wl) {
    if (!wl) return -1;
    unsigned long long reached = 0;
    ComThresholdFn fn = (ComThresholdFn)(base + COM_THRESHOLD_FN_RVA);
    __try { reached = fn(wl); }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        logLine("com meter: the threshold fn FAULTED — no group position");
        return -1;
    }
    if (reached > 0x7fffffffULL) return -1;
    return (int)reached;
}

static bool comWaveGroupPos(uintptr_t base, int* groupOut, int* doneOut, int* sizeOut) {
    if (groupOut) *groupOut = -1;
    if (doneOut)  *doneOut  = -1;
    if (sizeOut)  *sizeOut  = -1;
    uintptr_t wl = comWaveLogic(base);
    if (!wl) return false;

    uintptr_t gb = 0, ge = 0;
    if (!safeReadPtr(wl + WL_GROUPS_BEG_OFF, &gb) ||
        !safeReadPtr(wl + WL_GROUPS_END_OFF, &ge) || !gb || ge < gb) return false;
    uintptr_t span = ge - gb;
    if (span % WL_GROUP_STRIDE) return false;
    int groups = (int)(span / WL_GROUP_STRIDE);
    if (groups <= 0 || groups > 4096) return false;

    uint32_t done = 0;
    if (!safeReadU32(wl + WL_WAVES_DONE_OFF, &done)) return false;

    long long rem = (long long)done;
    for (int i = 0; i < groups; i++) {
        uintptr_t ib = 0, ie = 0;
        uintptr_t g = gb + (uintptr_t)i * WL_GROUP_STRIDE;
        if (!safeReadPtr(g, &ib) || !safeReadPtr(g + 8, &ie) || !ib || ie < ib) return false;
        uintptr_t s = ie - ib;
        if (s % WL_WAVEINFO_STRIDE) return false;
        int size = (int)(s / WL_WAVEINFO_STRIDE);
        if (size <= 0) return false;
        if (rem < size) {
            if (groupOut) *groupOut = i;
            if (doneOut)  *doneOut  = (int)rem;
            if (sizeOut)  *sizeOut  = size;
            return true;
        }
        rem -= size;
    }
    logLine("com meter: wavesCompleted=%u outruns the %d wave group(s) — no bar position",
            done, groups);
    return false;
}

static bool comRewardTierWord(uintptr_t base, int banked, char* out, int outsz, int* tierOut) {
    if (!out || outsz <= 0) return false;
    out[0] = 0;
    int t = banked < 0 ? 0 : banked;
    if (t > COM_TIER_LAST) t = COM_TIER_LAST;      // the widget's own clamp
    if (tierOut) *tierOut = t;

    char key[128];
    key[0] = 0;
    uintptr_t p = 0;
    if (safeReadPtr(base + COM_TIER_KEY_TAB_RVA + (uintptr_t)t * 8, &p) && p > 0x10000 &&
        safeReadCStr(p, key, sizeof key) &&
        strncmp(key, COM_TIER_KEY_PREFIX, sizeof COM_TIER_KEY_PREFIX - 1) == 0) {
        // the game's own key, live
    } else {
        logLine("com meter: tier key table unreadable at RVA 0x%llx+%d — using the shipped key",
                (unsigned long long)COM_TIER_KEY_TAB_RVA, t);
        _snprintf(key, sizeof key, "%s", kComTierKeys[t]);
        key[sizeof key - 1] = 0;
    }

    char raw[256];
    if (!resolveKey(base, key, raw, sizeof raw) || !raw[0]) {
        logLine("com meter: tier key \"%s\" did not resolve", key);
        return false;
    }
    stripMarkup(raw, out, outsz);
    return out[0] != 0;
}

// ---- THE ORDER, and why it changed ----
bool comKillMeter(uintptr_t base, char* out, int outsz) {
    if (!out || outsz <= 0) return false;
    out[0] = 0;
    uintptr_t wl = comWaveLogic(base);
    if (!wl) return false;

    uint32_t kills = 0, wavesDone = 0;
    bool haveKills = safeReadU32(wl + WL_KILL_COUNT_OFF, &kills);
    bool haveWave  = safeReadU32(wl + WL_WAVES_DONE_OFF, &wavesDone) && wavesDone < 100000;

    int killsDone = -1, killTarget = -1;
    bool haveFrac = comWaveKills(wl, &killsDone, &killTarget);

    int group = -1, waveDone = -1, groupSize = -1;
    bool havePos = comWaveGroupPos(base, &group, &waveDone, &groupSize);

    int banked = comBankedRaw(base, wl);
    if (havePos && banked >= 0 && group != banked)
        logLine("com meter: WARNING group walk says %d, the game's threshold leaf says %d - "
                "the bar position may be wrong", group, banked);

    char tier[160];
    int tierIdx = -1;
    bool haveTier = comRewardTierWord(base, banked, tier, sizeof tier, &tierIdx);

    int reached = 0, total = 0;
    bool haveThresh = comWaveThresholds(base, &reached, &total) && total > 0;

    int o = 0;
    #define COM_MADD(...) do {                                                    \
            if (o < outsz - 1) {                                                  \
                _snprintf(out + o, (size_t)(outsz - o), __VA_ARGS__);             \
                out[outsz - 1] = 0;                                               \
                o = (int)strlen(out);                                             \
            }                                                                     \
        } while (0)

    COM_MADD("Kill meter.");
    if (haveWave)  COM_MADD(" Wave %u.", wavesDone + 1);
    if (haveFrac)  COM_MADD(" %d of %d kills.", killsDone, killTarget);

    if (havePos) {
        int togo = groupSize - waveDone - 1;
        if (togo <= 0)      COM_MADD(" The next reward comes after this wave.");
        else if (togo == 1) COM_MADD(" 1 wave until the next reward.");
        else                COM_MADD(" %d waves until the next reward.", togo);
    }
    if (haveTier)  COM_MADD(" %s.", tier);
    if (haveKills) COM_MADD(" %u %s killed.", kills, kills == 1 ? "foe" : "foes");
    if (haveThresh) {
        if (reached > 0) COM_MADD(" %d of %d secured.", reached, total);
        else             COM_MADD(" None secured yet.");
    }
    #undef COM_MADD

    logLine("com meter: kills=%u wave=%u frac=%d/%d group=%d pos=%d/%d banked=%d tier=%d -> \"%s\"",
            kills, wavesDone + 1, killsDone, killTarget, group, waveDone, groupSize,
            banked, tierIdx, out);
    return out[0] != 0;
}

void comWaveProbe(uintptr_t base) {
    if (!axDebugLogEnabled()) return;                // diagnostic: nothing runs with the log off
    uintptr_t root = comRaidRoot(base);
    uintptr_t q = 0, kind = 0, wl = 0;
    uint32_t kindId = 0, wantKind = 0, questId = 0, noAbandonId = 0;
    if (root) safeReadPtr(root + COM_RAID_QUEST_OFF, &q);
    if (q) safeReadPtr(q + COM_QUEST_KIND_OFF, &kind);
    if (kind) safeReadU32(kind + COM_KIND_ID_OFF, &kindId);
    safeReadU32(base + COM_WAVE_QID_RVA, &wantKind);
    if (q) safeReadU32(q + 0x40, &questId);
    safeReadU32(base + QT_NOABANDON_QID_RVA, &noAbandonId);
    if (root) safeReadPtr(root + COM_WAVELOGIC_OFF, &wl);

    logDump("com wave probe: root=%p quest=%p kind=%p kindId=%u waveKindId=%u -> wave=%d",
            (void*)root, (void*)q, (void*)kind, kindId, wantKind, kindId && kindId == wantKind);
    logDump("com wave probe: questId=%u noAbandonQid=%u (equal => the game forbids abandoning)",
            questId, noAbandonId);

    // The five ShowRetreatButton gates, each with the value the game tests.
    uint8_t regroupByte = 0, hudUp = 0;
    uint32_t combat = 0, subMode = 0;
    if (q)    safeReadU8(q + 0x104, &regroupByte);
    if (root) safeReadU32(root + 0x4b20, &combat);
    if (root) safeReadU32(root + 0x5118, &subMode);
    if (root) safeReadU8(root + 0x563c, &hudUp);
    logDump("com wave probe: gates  Quest+0x104=%u  raid+0x4b20(combat)=%u  "
            "raid+0x5118(submode)=%u  raid+0x563c(hud)=%u",
            regroupByte, combat, subMode, hudUp);

    // The Quest's threshold table and the wave state.
    uintptr_t tb = 0, te = 0;
    if (q) { safeReadPtr(q + COM_QUEST_THRESH_BEG, &tb); safeReadPtr(q + COM_QUEST_THRESH_END, &te); }
    logDump("com wave probe: thresholds beg=%p end=%p span=%lld stride=0x%llx",
            (void*)tb, (void*)te, (long long)(te - tb), (unsigned long long)COM_THRESH_STRIDE);

    if (!wl) { logDump("com wave probe: no WaveLogic at raid+0x%llx",
                       (unsigned long long)COM_WAVELOGIC_OFF); return; }
    uint32_t kills = 0, area = 0, waves = 0, state = 0, next = 0, room = 0, lastRew = 0;
    uint8_t boss = 0, curio = 0;
    safeReadU32(wl + WL_KILL_COUNT_OFF,  &kills);
    safeReadU32(wl + WL_AREA_KILLS_OFF,  &area);
    safeReadU32(wl + WL_WAVES_DONE_OFF,  &waves);
    safeReadU32(wl + WL_WAVE_STATE_OFF,  &state);
    safeReadU32(wl + WL_NEXT_ACTION_OFF, &next);
    safeReadU32(wl + WL_CURR_ROOM_OFF,   &room);
    safeReadU32(wl + WL_LAST_REWARD_OFF, &lastRew);
    safeReadU8 (wl + WL_BOSS_SPAWNED_OFF, &boss);
    safeReadU8 (wl + WL_CURIO_SPAWN_OFF,  &curio);
    uint32_t multBits = 0; float mult = 0.f;
    if (safeReadU32(wl + WL_SHARD_MULT_OFF, &multBits)) memcpy(&mult, &multBits, sizeof mult);
    logDump("com wave probe: WaveLogic=%p killCount=%u areaKillCount=%u wavesCompleted=%u",
            (void*)wl, kills, area, waves);
    logDump("com wave probe: currWaveState=%u nextWaveActionType=%u currRoom=%u "
            "lastWaveGroupRewarded=%u bossSpawned=%u curioSpawned=%u shardMult=%.3f",
            state, next, room, lastRew, boss, curio, (double)mult);

    uint8_t canSkip = 0, trans = 0;
    uint32_t timerBits = 0; float timer = 0.f;
    safeReadU8 (wl + WL_CAN_SKIP_OFF,    &canSkip);
    safeReadU8 (wl + WL_TRANSITION_OFF,  &trans);
    if (safeReadU32(wl + WL_TRANS_TIMER_OFF, &timerBits)) memcpy(&timer, &timerBits, sizeof timer);
    logDump("com wave probe: canSkipCurio=%u transitionToNewArea=%u transTimer=%.3f "
            "-> mayAdvance=%d", canSkip, trans, (double)timer, comCanAdvance(base) ? 1 : 0);
    char adv[256];
    if (comAdvanceLabel(base, adv, sizeof adv))
        logDump("com wave probe: the way on reads \"%s\"", adv);

    int reached = 0, total = 0;
    bool okT = comWaveThresholds(base, &reached, &total);
    char key[64] = {0};
    comWaveRetreatKey(base, key, sizeof key);
    logDump("com wave probe: thresholds %s reached=%d of %d -> retreat key \"%s\"",
            okT ? "ok" : "UNREADABLE", reached, total, key);

    char raw[256], clean[256];
    if (key[0] && resolveKey(base, key, raw, sizeof raw)) {
        stripMarkup(raw, clean, sizeof clean);
        logDump("com wave probe: retreat tooltip reads \"%s\"", clean);
    } else if (key[0]) {
        logDump("com wave probe: retreat key \"%s\" DID NOT RESOLVE", key);
    }
    char status[384];
    if (comKillMeter(base, status, sizeof status)) logDump("com wave probe: meter -> \"%s\"", status);
}
