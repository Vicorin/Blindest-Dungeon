// raid/dungeonview.cpp -- THE DUNGEON VIEW

#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cmath>
#include "internal.h"

// ---- ROOM VIEW: the party roster ----

static const uintptr_t RAID_ENEMY_BEG_OFF = 0x48d0;  // raid root+: vector<Actor*> begin (was 0x4868)
static const uintptr_t RAID_ENEMY_END_OFF = 0x48d8;  // raid root+: vector<Actor*> end   (was 0x4870)

// ---- STAGE 3: the room's PROPS (curios, obstacles) and its DOORS ----
static const uintptr_t RD_DUNGEONVIEW_OFF = 0x3130;  // RaidDisplay+: Panel_DungeonView* (was 0x2ca0)
static const uintptr_t DV_PROPS_BEG_OFF   = 0x780;
static const uintptr_t DV_PROPS_END_OFF   = 0x788;
static const int       PROP_KIND_TRAP       = 2;     // the kind CanInteractWithTrap asserts on

// ---- TRAPS ----
static const uintptr_t PROP_TRAP_SEEN_OFF   = 0x123; // Prop+: byte, the revealed flag -- the popup's

static const uintptr_t OE_TRAP_HERO_IDX_OFF = 0xd80;
static const uintptr_t OE_TRAP_DELIBERATE_OFF = 0xd84;
static const uintptr_t MONSTER_CLASS_OFF  = 0x13f8;  // Monster+: MonsterClass* (was 0x1378)

// ---- The MONSTER HOVER TOOLTIP (FUN_1406ee670) ----
static const char* const MT_HP_KEY    = "monster_tooltip_hp_format";
static const char* const MT_PROT_KEY  = "monster_tooltip_prot_format";
static const char* const MT_DODGE_KEY = "monster_tooltip_dodge_format";
static const char* const MT_SPEED_KEY = "monster_tooltip_speed_format";
static const char* const MT_SKILLS_KEY = "monster_panel_skills";
static const char* const MT_RESIST_KEY      = "monster_panel_resistances";
static const uintptr_t MONSTER_TYPE_BEG_OFF = 0x598;
static const uintptr_t MONSTER_TYPE_END_OFF = 0x5a0;
static const uintptr_t MONSTER_TYPE_STRIDE  = 0x44;
static const char* const MT_TYPE_KEY_FMT = "enemy_type_name_%s";
static const char* const MT_TYPE_SEP_KEY = "enemy_type_name_seperator";

static const uintptr_t MONSTERCLASS_SKILLVEC_OFF     = 0xba8;
static const uintptr_t MONSTERCLASS_SKILLVEC_END_OFF = 0xbb0;
static const int RV_TIP_SKILL_MAX = 12;

static const int RV_TIP_MAX_LINES = 30;   // a dungeon-view-only budget

// ---- ROOM VIEW (R) ----

int  g_rvTipLine = 0;
int g_rvTipCol = 0;
static bool g_rvDumped  = false;           // the one-shot dump has fired this session
static bool g_rvPropDumped = false;

// The party, in the raid root's own vector order.
int rvPartyList(uintptr_t base, uintptr_t* out, int maxOut) {
    uintptr_t root = 0;
    if (!safeReadPtr(base + MAP_ROOT_RVA, &root) || root <= 0x10000) return 0;
    uintptr_t beg = 0, end = 0;
    if (!safeReadPtr(root + RAID_PARTY_BEG_OFF, &beg) ||
        !safeReadPtr(root + RAID_PARTY_END_OFF, &end)) {
        logLine("roomview: party vector unreadable");
        return 0;
    }
    if (beg <= 0x10000 || end < beg) { logLine("roomview: party vector empty or bad"); return 0; }
    int64_t n = (int64_t)((end - beg) / 8);
    if (n <= 0) return 0;
    if (n > RV_MAX_MEMBERS) {
        logLine("roomview: party vector claims %lld members, capping", (long long)n);
        n = RV_MAX_MEMBERS;
    }
    int got = 0;
    for (int64_t i = 0; i < n && got < maxOut; i++) {
        uintptr_t hero = 0;
        if (!safeReadPtr(beg + (uintptr_t)i * 8, &hero) || hero <= 0x10000) {
            logLine("roomview: party slot %lld unreadable", (long long)i);
            continue;
        }
        out[got++] = hero;
    }
    return got;
}

int rvEnemyList(uintptr_t base, uintptr_t* out, int maxOut) {
    uintptr_t root = 0;
    if (!safeReadPtr(base + MAP_ROOT_RVA, &root) || root <= 0x10000) return 0;
    uintptr_t beg = 0, end = 0;
    if (!safeReadPtr(root + RAID_ENEMY_BEG_OFF, &beg) ||
        !safeReadPtr(root + RAID_ENEMY_END_OFF, &end)) {
        logLine("roomview: enemy vector unreadable");
        return 0;
    }
    if (beg == 0 && end == 0) return 0;                 // no fight on: an empty side, not a fault
    if (beg <= 0x10000 || end < beg) { logLine("roomview: enemy vector bad"); return 0; }
    int64_t n = (int64_t)((end - beg) / 8);
    if (n <= 0) return 0;
    if (n > RV_MAX_ENEMIES) {
        logLine("roomview: enemy vector claims %lld actors, capping", (long long)n);
        n = RV_MAX_ENEMIES;
    }
    int got = 0;
    for (int64_t i = 0; i < n && got < maxOut; i++) {
        uintptr_t mon = 0;
        if (!safeReadPtr(beg + (uintptr_t)i * 8, &mon) || mon <= 0x10000) {
            logLine("roomview: enemy slot %lld unreadable", (long long)i);
            continue;
        }
        out[got++] = mon;
    }
    return got;
}

bool rvMonsterName(uintptr_t base, uintptr_t mon, char* out, int outsz) {
    out[0] = 0;
    uintptr_t cobj = 0;
    if (!safeReadPtr(mon + MONSTER_CLASS_OFF, &cobj) || cobj <= 0x10000) return false;

    uint8_t latched = 0;
    if (safeReadU8(cobj + HEROCLASS_DISP_LATCH_OFF, &latched) && latched) {
        if (safeReadCStr(cobj + HEROCLASS_DISP_OFF, out, outsz) && abPlausibleName(out)) return true;
        out[0] = 0;
    }
    // Not latched yet -> resolve the display key ourselves, as the latch write would.
    char id[64] = { 0 };
    if (!safeReadCStr(cobj + HEROCLASS_ID_OFF, id, sizeof id) || !abPlausibleName(id)) return false;
    bool identifier = true;
    for (const char* p = id; *p; p++)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
              (*p >= '0' && *p <= '9') || *p == '_')) { identifier = false; break; }
    if (identifier) {
        char key[96];
        _snprintf(key, sizeof key, "str_monstername_%s", id);
        key[sizeof key - 1] = 0;
        if (resolveKey(base, key, out, outsz) && out[0] && abPlausibleName(out)) return true;
        logLine("roomview: \"%s\" did not resolve — speaking the raw class id", key);
        out[0] = 0;
    }
    // The raw class id at least means something, and never silence.
    _snprintf(out, outsz, "%s", id);
    out[outsz - 1] = 0;
    return true;
}

static void rvPosFrag(int slot, int slotEnd, int total, char* out, int outsz) {
    if (slotEnd > slot) _snprintf(out, outsz, axs(AXS_RV_POSITIONS_RANGE_OF_FMT), slot, slotEnd, total);
    else                _snprintf(out, outsz, axs(AXS_RV_POSITION_OF_FMT), slot, total);
    out[outsz - 1] = 0;
}

void rvPosPhrase(int slot, int slotEnd, bool enemy, char* out, int outsz) {
    if (slotEnd > slot) {
        if (enemy) _snprintf(out, outsz, axs(AXS_RV_ENEMY_POSITIONS_FMT), slot, slotEnd);
        else       _snprintf(out, outsz, axs(AXS_RV_POSITIONS_FMT), slot, slotEnd);
    } else {
        if (enemy) _snprintf(out, outsz, axs(AXS_RV_ENEMY_POSITION_FMT), slot);
        else       _snprintf(out, outsz, axs(AXS_RV_POSITION_FMT), slot);
    }
    out[outsz - 1] = 0;
}

void rvAppendComma(char* out, int outsz, const char* frag) {
    if (!frag || !frag[0]) return;
    if (out[0]) strncat(out, ", ", outsz - strlen(out) - 1);
    strncat(out, frag, outsz - strlen(out) - 1);
}

// ---- SURPRISE: which side was ambushed ----
int rvSurprisedSide(uintptr_t base, uint32_t* roundOut) {
    if (roundOut) *roundOut = 0;
    uintptr_t root = 0;
    if (!safeReadPtr(base + MAP_ROOT_RVA, &root) || root <= 0x10000) return -1;   // not in a raid
    uint32_t side = 0, round = 0;
    if (!safeReadU32(root + BATTLE_OBJ_OFF + BATTLE_SURPRISE_OFF, &side) ||
        !safeReadU32(root + BATTLE_OBJ_OFF + BATTLE_ROUND_OFF,    &round)) {
        logLine("surprise: side/round unreadable off raidRoot=%p", (void*)root);
        return -1;
    }
    if (side > (uint32_t)SURPRISE_MONSTERS) {
        logLine("surprise: side flag reads %u — not a battle, ignoring", side);
        return -1;
    }
    if (roundOut) *roundOut = round;
    return (int)side;
}

static bool rvMonsterHasType(uintptr_t mon, const char* type);
static const int RV_TURNS_ACTIVE = -2;   // it is this actor's turn right now
static int rvActorTurnsLeft(uintptr_t base, uintptr_t mon) {
    uintptr_t root = 0;
    if (!safeReadPtr(base + MAP_ROOT_RVA, &root) || root <= 0x10000) return -1;
    uint32_t state = 0;
    if (!safeReadU32(root + RAID_IN_COMBAT_OFF, &state) || state == 0) return -1;

    if (rvMonsterHasType(mon, "corpse")) return -1;

    uintptr_t battle = root + BATTLE_OBJ_OFF, beg = 0, end = 0;
    uint32_t idx = 0;
    if (!safeReadPtr(battle + BATTLE_TURNVEC_BEG_OFF, &beg) ||
        !safeReadPtr(battle + BATTLE_TURNVEC_END_OFF, &end) ||
        !safeReadU32(battle + BATTLE_TURN_IDX_OFF, &idx)) return -1;
    if (beg <= 0x10000 || end < beg) return -1;
    uintptr_t n = (end - beg) / 0x18;
    if (n > 64) return -1;   // a torn read must not become a long walk

    if ((uintptr_t)idx < n && (int)state >= BS_TURN_START) {
        uintptr_t cur = 0;
        if (safeReadPtr(beg + (uintptr_t)idx * 0x18, &cur) && cur == mon)
            return RV_TURNS_ACTIVE;
    }

    int count = 0;
    for (uintptr_t i = idx; i < n; i++) {
        uintptr_t actor = 0;
        uint8_t   dead  = 0;
        if (!safeReadPtr(beg + i * 0x18, &actor)) return -1;
        if (actor != mon) continue;
        if (!safeReadU8(beg + i * 0x18 + BATTLE_TURNENTRY_DEAD_OFF, &dead) || dead) continue;
        if (i == idx && (int)state >= BS_TURN_START) continue;
        count++;
    }
    return count;
}

static bool rvTurnsClause(uintptr_t base, uintptr_t actor, char* frag, int fragsz) {
    int t = rvActorTurnsLeft(base, actor);
    if (t == RV_TURNS_ACTIVE) { _snprintf(frag, fragsz, "%s", axs(AXS_RV_TURN_ACTIVE)); frag[fragsz - 1] = 0; return frag[0] != 0; }
    if (t == 0)               { _snprintf(frag, fragsz, "%s", axs(AXS_RV_ALREADY_ACTED)); frag[fragsz - 1] = 0; return frag[0] != 0; }
    if (t > 0) return abTipInt(base, TURNS_FMT_KEY, t, frag, fragsz);
    return false;
}

static bool rvArenaContestant(uintptr_t base, uintptr_t mon) {
    if (!mon || !pitInMatch(base)) return false;
    char dead[96];
    return !rvCorpseWord(base, mon, dead, sizeof dead);
}
static bool rvArenaStressShown(uintptr_t base, uintptr_t mon) {
    return rvArenaContestant(base, mon);
}

static bool rvMonsterRowText(uintptr_t base, uintptr_t mon, int slot, int slotEnd, int total,
                             bool enemy, char* out, int outsz) {
    (void)total;
    out[0] = 0;
    if (!rvMonsterName(base, mon, out, outsz)) {
        logLine("roomview: no class name for monster=%p (slot %d, enemy=%d)",
                (void*)mon, slot, (int)enemy);
        return false;
    }

    char frag[160];
    if (abHeroHealth(base, mon, frag, sizeof frag)) rvAppendComma(out, outsz, frag);
    else logLine("roomview: health unreadable for monster=%p", (void*)mon);

    if (rvArenaStressShown(base, mon)) {
        if (abHeroStressCur(base, mon, frag, sizeof frag)) rvAppendComma(out, outsz, frag);
        else logLine("roomview: arena stress unreadable for contestant=%p", (void*)mon);
    }

    if (rvTurnsClause(base, mon, frag, sizeof frag)) rvAppendComma(out, outsz, frag);
    strncat(out, ".", outsz - strlen(out) - 1);

    rvPosPhrase(slot, slotEnd, enemy, frag, sizeof frag);
    abAppendFrag(out, outsz, frag);
    return true;
}

// ---- A FALLEN CONTESTANT: the Butcher's Circus's own kind of dead ----
static const char* const RV_CORPSE_WORD_KEY = "enemy_type_name_corpse";

static bool rvClassIdAt(uintptr_t actor, uintptr_t off, uintptr_t idOff, char* out, int outsz) {
    out[0] = 0;
    uintptr_t cls = 0;
    if (!safeReadPtr(actor + off, &cls) || cls <= 0x10000) return false;
    if (!safeReadCStr(cls + idOff, out, outsz) || !abPlausibleName(out)) { out[0] = 0; return false; }
    return true;
}

static bool rvActorClassId(uintptr_t actor, char* out, int outsz) {
    out[0] = 0;
    uintptr_t cls = abActorClass(actor);
    if (cls && safeReadCStr(cls + ACTOR_CLASS_VID_OFF, out, outsz) && abPlausibleName(out)) return true;
    out[0] = 0;
    if (rvClassIdAt(actor, ACTOR_HEROCLASS_OFF, HEROCLASS_ID_OFF, out, outsz)) return true;
    return rvClassIdAt(actor, MONSTER_CLASS_OFF, HEROCLASS_ID_OFF, out, outsz);
}

static const int RV_TIP_TYPE_MAX = 8;
static bool rvMonsterHasType(uintptr_t mon, const char* type) {
    uintptr_t cls = 0;
    if (!safeReadPtr(mon + MONSTER_CLASS_OFF, &cls) || cls <= 0x10000) return false;
    uintptr_t beg = 0, end = 0;
    if (!safeReadPtr(cls + MONSTER_TYPE_BEG_OFF, &beg) ||
        !safeReadPtr(cls + MONSTER_TYPE_END_OFF, &end)) return false;
    if (beg <= 0x10000 || end < beg) return false;
    int64_t n = (int64_t)((end - beg) / MONSTER_TYPE_STRIDE);
    if (n <= 0 || n > RV_TIP_TYPE_MAX) return false;
    for (int64_t i = 0; i < n; i++) {
        char id[MONSTER_TYPE_STRIDE + 1];
        if (safeReadCStr(beg + (uintptr_t)i * MONSTER_TYPE_STRIDE, id, sizeof id) &&
            strcmp(id, type) == 0) return true;
    }
    return false;
}

bool rvCorpseWord(uintptr_t base, uintptr_t actor, char* out, int outsz) {
    out[0] = 0;
    char id[64];
    bool body = rvActorClassId(actor, id, sizeof id) && strcmp(id, "corpse") == 0;
    if (!body) {
        if (spActorIsHero(base, actor) || !rvMonsterHasType(actor, "corpse")) return false;
    }
    if (resolveKey(base, RV_CORPSE_WORD_KEY, out, outsz) && out[0]) return true;
    logLine("roomview: \"%s\" did not resolve -- the corpse row speaks the class id \"%s\"",
            RV_CORPSE_WORD_KEY, id);
    _snprintf(out, outsz, "%s", id);
    out[outsz - 1] = 0;
    return true;
}

static bool rvHeroRowText(uintptr_t base, uintptr_t hero, int slot, int slotEnd, int total,
                          bool enemy, char* out, int outsz) {
    (void)total;
    out[0] = 0;
    char name[80], cls[80];
    if (!abHeroNameClassOf(base, hero, name, sizeof name, cls, sizeof cls) || !(name[0] || cls[0])) {
        logLine("roomview: no label for hero=%p (slot %d, enemy=%d)", (void*)hero, slot, (int)enemy);
        return false;
    }
    if (enemy && name[0] && cls[0]) _snprintf(out, outsz, "%s, %s", name, cls);
    else                            _snprintf(out, outsz, "%s", name[0] ? name : cls);
    out[outsz - 1] = 0;

    char dead[96];
    if (rvCorpseWord(base, hero, dead, sizeof dead)) {
        char frag[160];
        if (name[0]) _snprintf(out, outsz, "%s, %s", name, dead);
        else         _snprintf(out, outsz, "%s", dead);
        out[outsz - 1] = 0;
        if (abHeroHealth(base, hero, frag, sizeof frag)) rvAppendComma(out, outsz, frag);
        strncat(out, ".", outsz - strlen(out) - 1);
        rvPosPhrase(slot, slotEnd, enemy, frag, sizeof frag);
        abAppendFrag(out, outsz, frag);
        return true;
    }

    if (!enemy) {
        char trap[96];
        rvTrapDisarmSuffix(base, hero, trap, sizeof trap);
        if (trap[0]) rvAppendComma(out, outsz, trap);
    }

    char frag[160];
    if (abHeroHealth(base, hero, frag, sizeof frag)) rvAppendComma(out, outsz, frag);
    else logLine("roomview: health unreadable for hero=%p", (void*)hero);
    if (abHeroStressCur(base, hero, frag, sizeof frag)) rvAppendComma(out, outsz, frag);
    else logLine("roomview: stress unreadable for hero=%p", (void*)hero);

    if (rvTurnsClause(base, hero, frag, sizeof frag)) rvAppendComma(out, outsz, frag);
    strncat(out, ".", outsz - strlen(out) - 1);

    rvPosPhrase(slot, slotEnd, enemy, frag, sizeof frag);
    abAppendFrag(out, outsz, frag);

    char form[64];
    if (spModeName(base, hero, form, sizeof form))
        abAppendFrag(out, outsz, form);

    if (!enemy) {
        char pit[64];
        pitRowSuffix(base, hero, pit, sizeof pit);
        if (pit[0]) abAppendFrag(out, outsz, pit);
    }

    return true;
}

// ---- THE HERO TOOLTIP ----

static void spEndSentence(char* s, int sz) {
    size_t n = strlen(s);
    if (n && s[n - 1] != '.' && n + 1 < (size_t)sz) { s[n] = '.'; s[n + 1] = 0; }
}

static void spAddBucket(uintptr_t base, uintptr_t hero, int bucket, char* out, int outsz) {
    char frag[AB_TIP_LINE_SZ];
    if (!spConditionsInto(base, hero, bucket, frag, sizeof frag) || !frag[0]) return;
    size_t len = strlen(frag);
    if (len && frag[len - 1] == '.') frag[len - 1] = 0;
    abAppend(out, outsz, frag);
}

static bool spHeroTitleLine(uintptr_t base, uintptr_t hero, int slot, int slotEnd, bool enemy,
                            char* out, int outsz) {
    out[0] = 0;
    char name[80], cls[80];
    if (!abHeroNameClassOf(base, hero, name, sizeof name, cls, sizeof cls)) return false;
    if (!name[0] && !cls[0]) return false;

    char deadWord[96];
    if (rvCorpseWord(base, hero, deadWord, sizeof deadWord)) {
        char dpos[64];
        dpos[0] = 0;
        if (slot >= 1) {                             // the same guard and the same de-capitalisation
            rvPosPhrase(slot, slotEnd, enemy, dpos, sizeof dpos);
            if (dpos[0] >= 'A' && dpos[0] <= 'Z') dpos[0] = (char)(dpos[0] - 'A' + 'a');
        }
        if (name[0] && dpos[0]) _snprintf(out, outsz, "%s, %s, %s.", name, deadWord, dpos);
        else if (name[0])       _snprintf(out, outsz, "%s, %s.", name, deadWord);
        else if (dpos[0])       _snprintf(out, outsz, "%s, %s.", deadWord, dpos);
        else                    _snprintf(out, outsz, "%s.", deadWord);
        out[outsz - 1] = 0;
        return true;
    }

    char rank[128] = { 0 };
    int level = csResolveLevel(base, hero);
    if (level < 0) {
        logLine("herotip: rank unreadable for hero=%p", (void*)hero);
    } else {
        char key[32];
        _snprintf(key, sizeof key, "str_resolve_%d", level);
        key[sizeof key - 1] = 0;
        if (!abTipPlain(base, key, rank, sizeof rank)) {
            logLine("herotip: \"%s\" did not resolve", key);
            rank[0] = 0;
        }
    }

    char title[224];
    if (rank[0] && cls[0]) _snprintf(title, sizeof title, "%s %s", rank, cls);
    else                   _snprintf(title, sizeof title, "%s", rank[0] ? rank : cls);
    title[sizeof title - 1] = 0;

    char pos[64];
    pos[0] = 0;
    if (slot >= 1) {
        rvPosPhrase(slot, slotEnd, enemy, pos, sizeof pos);
        if (pos[0] >= 'A' && pos[0] <= 'Z') pos[0] = (char)(pos[0] - 'A' + 'a');
    }

    const char* who = name[0] ? name : title;
    const char* rest = (name[0] && title[0]) ? title : NULL;
    if (rest && pos[0])      _snprintf(out, outsz, "%s, %s, %s.", who, rest, pos);
    else if (rest)           _snprintf(out, outsz, "%s, %s.", who, rest);
    else if (pos[0])         _snprintf(out, outsz, "%s, %s.", who, pos);
    else                     _snprintf(out, outsz, "%s.", who);
    out[outsz - 1] = 0;
    return out[0] != 0;
}

int spHeroTipLines(uintptr_t base, uintptr_t hero, int slot, int slotEnd, bool withTitle,
                          bool enemy, char lines[][AB_TIP_LINE_SZ], int n, int maxLines) {
    if (!hero) return n;

    char deadWord[96];
    if (rvCorpseWord(base, hero, deadWord, sizeof deadWord)) {
        char title[AB_TIP_LINE_SZ];
        if (withTitle && n < maxLines &&
            spHeroTitleLine(base, hero, slot, slotEnd, enemy, title, sizeof title)) {
            _snprintf(lines[n], AB_TIP_LINE_SZ, "%s", title);
            lines[n][AB_TIP_LINE_SZ - 1] = 0; n++;
        }
        return n;
    }

    spDumpStatusOnce(base, hero);

    char buf[AB_TIP_LINE_SZ], frag[AB_TIP_LINE_SZ];

    // 0 — identity.
    if (withTitle && n < maxLines && spHeroTitleLine(base, hero, slot, slotEnd, enemy, buf, sizeof buf)) {
        _snprintf(lines[n], AB_TIP_LINE_SZ, "%s", buf);
        lines[n][AB_TIP_LINE_SZ - 1] = 0; n++;
    }

    if (n < maxLines) {
        buf[0] = 0;
        if (abHeroHealth(base, hero, frag, sizeof frag)) rvAppendComma(buf, sizeof buf, frag);
        else logLine("herotip: health unreadable for hero=%p", (void*)hero);
        spAddBucket(base, hero, SP_COND_HEALTH, buf, sizeof buf);
        spEndSentence(buf, sizeof buf);
        if (spIncomingModsFrag(base, hero, "hp_heal_received_percent", frag, sizeof frag))
            abAppendFrag(buf, sizeof buf, frag);
        const char* line = buf;
        while (*line == ' ') line++;
        if (*line) { _snprintf(lines[n], AB_TIP_LINE_SZ, "%s", line);
                     lines[n][AB_TIP_LINE_SZ - 1] = 0; n++; }
    }

    if (n < maxLines) {
        buf[0] = 0;
        if (abHeroStress(base, hero, frag, sizeof frag)) rvAppendComma(buf, sizeof buf, frag);
        else logLine("herotip: stress unreadable for hero=%p", (void*)hero);
        {
            uint32_t th = 0;
            char tname[128];
            if (safeReadU32(hero + ACTOR_TRAIT_OFF, &th) && th &&
                spTraitName(base, th, false, tname, sizeof tname))
                rvAppendComma(buf, sizeof buf, tname);
            else if (safeReadU32(hero + ACTOR_VIRTUE_OFF, &th) && th &&
                     spTraitName(base, th, true, tname, sizeof tname))
                rvAppendComma(buf, sizeof buf, tname);
        }
        spAddBucket(base, hero, SP_COND_STRESS, buf, sizeof buf);
        spEndSentence(buf, sizeof buf);
        if (spIncomingModsFrag(base, hero, "stress_dmg_received_percent", frag, sizeof frag))
            abAppendFrag(buf, sizeof buf, frag);
        if (spIncomingModsFrag(base, hero, "stress_heal_received_percent", frag, sizeof frag))
            abAppendFrag(buf, sizeof buf, frag);
        const char* line = buf;
        while (*line == ' ') line++;
        if (*line) { _snprintf(lines[n], AB_TIP_LINE_SZ, "%s", line);
                     lines[n][AB_TIP_LINE_SZ - 1] = 0; n++; }
    }

    n = spConditionLines(base, hero, SP_COND_BLEED | SP_COND_BLIGHT | SP_COND_HORROR,
                         lines, n, maxLines);
    n = spConditionLines(base, hero, SP_COND_NEG | SP_COND_POS, lines, n, maxLines);

    n = spUnvoicedBuffLines(base, hero, false, lines, n, maxLines);

    int stats = csStatCount(base);
    for (int i = 0; i < stats && n < maxLines; i++) {
        if (!csStatTipLineFor(base, hero, i, buf, sizeof buf) || !buf[0]) {
            logLine("herotip: stat row %d unreadable for hero=%p", i, (void*)hero);
            continue;
        }
        _snprintf(lines[n], AB_TIP_LINE_SZ, "%s", buf);
        lines[n][AB_TIP_LINE_SZ - 1] = 0; n++;
    }

    if (n < maxLines) {
        _snprintf(lines[n], AB_TIP_LINE_SZ, "%s", axs(AXS_RV_RESIST_HINT));
        lines[n][AB_TIP_LINE_SZ - 1] = 0; n++;
    }
    if (n < maxLines) {
        _snprintf(lines[n], AB_TIP_LINE_SZ, "%s", axs(AXS_RV_ITEMS_HINT));
        lines[n][AB_TIP_LINE_SZ - 1] = 0; n++;
    }
    return n;
}

// ---- The RESISTANCE PANEL — the hero tooltip's second column ----
int spHeroResistLines(uintptr_t base, uintptr_t hero, int slot, int slotEnd, bool enemy,
                      char lines[][AB_TIP_LINE_SZ], int maxLines) {
    if (!hero) return 0;
    char deadWord[96];
    if (rvCorpseWord(base, hero, deadWord, sizeof deadWord)) return 0;
    int n = 0;
    if (n < maxLines &&
        spHeroTitleLine(base, hero, slot, slotEnd, enemy, lines[n], AB_TIP_LINE_SZ) &&
        lines[n][0]) n++;
    int body = 0;
    int resists = csResistCount(base);
    for (int i = 0; i < resists && n < maxLines; i++) {
        if (!csResistTipLineFor(base, hero, i, lines[n], AB_TIP_LINE_SZ) || !lines[n][0]) {
            logLine("herotip: resist row %d unreadable for hero=%p", i, (void*)hero);
            continue;
        }
        n++; body++;
    }
    return body > 0 ? n : 0;
}

// ---- The ITEMS PANEL — the hero tooltip's third column, LEFT of the main buffer ----
int spHeroItemLines(uintptr_t base, uintptr_t hero, int slot, int slotEnd, bool enemy,
                    char lines[][AB_TIP_LINE_SZ], int maxLines) {
    if (!hero) return 0;
    char deadWord[96];
    if (rvCorpseWord(base, hero, deadWord, sizeof deadWord)) return 0;
    int n = 0;
    if (n < maxLines &&
        spHeroTitleLine(base, hero, slot, slotEnd, enemy, lines[n], AB_TIP_LINE_SZ) &&
        lines[n][0]) n++;
    int body = 0;
    for (int i = 0; i < 2 + TRINKET_SLOT_COUNT && n < maxLines; i++) {
        if (!csItemsPanelRow(base, hero, i, lines[n], AB_TIP_LINE_SZ) || !lines[n][0]) {
            logLine("herotip: items row %d unreadable for hero=%p", i, (void*)hero);
            continue;
        }
        n++; body++;
    }
    if (body <= 0) return 0;
    if (n < maxLines) {
        _snprintf(lines[n], AB_TIP_LINE_SZ, "%s", axs(AXS_RV_ITEMS_BACK_HINT));
        lines[n][AB_TIP_LINE_SZ - 1] = 0; n++;
    }
    return n;
}

bool spResistPanelHead(uintptr_t base, char* out, int outsz) {
    if (!resolveKey(base, "character_title_resistances", out, outsz) || !out[0]) {
        logLine("herotip: \"character_title_resistances\" did not resolve — AXS fallback");
        _snprintf(out, outsz, "%s", axs(AXS_RV_RESIST_PANEL));
        out[outsz - 1] = 0;
    }
    abStripMarkup(out);
    csTrimLabel(out);
    return out[0] != 0;
}

// ---- The room as ONE flat list ----

int rvRankFromGame(uintptr_t actor, uint32_t* rawOut, int* endOut) {
    uint32_t mask = 0;
    if (rawOut) *rawOut = 0;
    if (endOut) *endOut = 0;
    if (!actor || !safeReadU32(actor + ACTOR_RANK_MASK_OFF, &mask)) return 0;
    if (rawOut) *rawOut = mask;
    mask &= SKILL_RANK_BITS;                 // ranks 1..4 only; higher bits are not rank data
    if (!mask) return 0;
    int lo = 0, hi = 0;
    for (int b = 0; b < 4; b++) if (mask & (1u << b)) { if (!lo) lo = b + 1; hi = b + 1; }
    if (endOut) *endOut = hi;
    return lo;
}

static int rvRankFootprint(const uintptr_t* actors, int count) {
    int used = 0;
    for (int i = 0; i < count; i++) {
        int end = 0;
        int lo = rvRankFromGame(actors[i], nullptr, &end);
        used += (lo && end >= lo) ? (end - lo + 1) : 1;
    }
    return used;
}

static int rvSlotFallback(bool enemy, int idx, int total) {
    return enemy ? idx + 1 : total - idx;
}

static void rvNoteRankChange(uintptr_t actor, uint32_t raw, int rank, bool enemy, int idx) {
    struct Seen { uintptr_t actor; uint32_t raw; };
    static Seen seen[RV_MAX_ROWS] = { { 0, 0 } };
    for (int i = 0; i < RV_MAX_ROWS; i++) {
        if (seen[i].actor == actor) {
            if (seen[i].raw != raw) {
                logLine("rv-rank CHANGED: actor=%p mask 0x%x -> 0x%x (rank %d) %s idx=%d",
                        (void*)actor, seen[i].raw, raw, rank, enemy ? "enemy" : "party", idx);
                seen[i].raw = raw;
            }
            return;
        }
    }
    for (int i = 0; i < RV_MAX_ROWS; i++) {
        if (!seen[i].actor) {
            seen[i].actor = actor; seen[i].raw = raw;
            logLine("rv-rank: actor=%p mask 0x%x -> rank %d (%s idx=%d)",
                    (void*)actor, raw, rank, enemy ? "enemy" : "party", idx);
            return;
        }
    }
}

static int rvSlotNumber(uintptr_t actor, bool enemy, int idx, int total, int* endOut) {
    uint32_t raw = 0;
    int end = 0;
    int rank = rvRankFromGame(actor, &raw, &end);
    if (!rank) {
        int guess = rvSlotFallback(enemy, idx, total);
        if (endOut) *endOut = guess;
        logLine("rv-rank: actor=%p mask 0x%x unreadable/empty -> falling back to %d (%s idx=%d/%d)",
                (void*)actor, raw, guess, enemy ? "enemy" : "party", idx, total);
        return guess;
    }
    if (endOut) *endOut = (end > rank) ? end : rank;
    rvNoteRankChange(actor, raw, rank, enemy, idx);
    if (end > rank)
        logLine("rv-rank: actor=%p mask 0x%x spans ranks %d-%d (%s idx=%d)",
                (void*)actor, raw, rank, end, enemy ? "enemy" : "party", idx);
    return rank;
}

// ---- STAGE 3: PROPS (curios, obstacles) ----
static const char* const kCurioUiFor[][2] = {
    { "unlocked_strongbox", "unlocked_strongbox" },
    { "locked_strongbox", "heirloom_chest" },
    { "goal_strongbox", "heirloom_chest" },
    { "heirloom_chest", "heirloom_chest" },
    { "eldritch_altar", "eldritch_altar" },
    { "altar_of_light", "altar_of_light" },
    { "stack_of_books", "stack_of_books" },
    { "pile_of_bones", "pile_of_bones" },
    { "discarded_pack", "discarded_pack" },
    { "sconce", "sconce" },
    { "crate", "crate" },
    { "sack", "sack" },
    { "iron_maiden", "iron_maiden" },
    { "suit_of_armor", "suit_of_armor" },
    { "dinner_cart", "dinner_cart" },
    { "decorative_urn", "decorative_urn" },
    { "locked_display_cabinet", "locked_display_cabinet" },
    { "confession_booth", "confession_booth" },
    { "holy_fountain", "holy_fountain" },
    { "bookshelf", "bookshelf" },
    { "alchemy_table", "alchemy_table" },
    { "knife_rack", "knife_rack" },
    { "sarcophagus", "sarcophagus" },
    { "mummified_remains", "mummified_remains" },
    { "sacrificial_stone", "sacrificial_stone" },
    { "skull_altar", "skull_altar" },
    { "occult_scrawlings", "occult_scrawlings" },
    { "moonshine_barrel", "moonshine_barrel" },
    { "locked_sarcophagus", "locked_sarcophagus" },
    { "makeshift_dining_table", "makeshift_dining_table" },
    { "pile_of_scrolls", "pile_of_scrolls" },
    { "ancient_coffin", "ancient_coffin" },
    { "old_tree", "old_tree" },
    { "beast_carcass", "beast_carcass" },
    { "pristine_fountain", "pristine_fountain" },
    { "travellers_tent", "travellers_tent" },
    { "cosmic_spiderweb", "cosmic_spiderweb" },
    { "lost_luggage", "lost_luggage" },
    { "shallow_grave", "shallow_grave" },
    { "troubling_effigy", "troubling_effigy" },
    { "travellers_tent_tutorial", "travellers_tent" },
    { "bandits_trapped_chest", "bandits_trapped_chest" },
    { "shamblers_altar", "shamblers_altar" },
    { "shipment_crates", "shipment_crates" },
    { "protective_ward", "protective_ward" },
    { "barnacle_crusted_chest", "barnacle_crusted_chest" },
    { "brackish_tidepool", "brackish_tidepool" },
    { "fish_idol", "fish_idol" },
    { "giant_oyster", "giant_oyster" },
    { "giant_fish_carcass", "giant_fish_carcass" },
    { "ships_figurehead", "ships_figurehead" },
    { "eerie_coral", "eerie_coral" },
    { "bas_relief", "bas_relief" },
    { "secret_stash", "secret_stash" },
    { "ancestors_knapsack", "ancestors_knapsack" },
    { "tutorial_shovel", "unlocked_strongbox" },
    { "tutorial_key", "discarded_pack" },
    { "tutorial_holy", "sack" },
    { "tutorial_food", "discarded_pack" },
    { "incursion_knapsack", "ancestors_knapsack" },
    { "thanks_chest", "unlocked_strongbox" },
    { "open_grave", "open_grave" },
};
static const int kCurioUiCount = (int)(sizeof kCurioUiFor / sizeof kCurioUiFor[0]);

static const char* const kObstacleNames[] = {
    "thorny_thicket", "rubble", "shipwreck", "town_rubble", "ancestor",
};
static const int kObstacleCount = (int)(sizeof kObstacleNames / sizeof kObstacleNames[0]);

static bool propNameIsKnown(const char* name) {
    for (int i = 0; i < kCurioUiCount; i++) if (strcmp(kCurioUiFor[i][0], name) == 0) return true;
    for (int i = 0; i < kObstacleCount; i++) if (strcmp(kObstacleNames[i], name) == 0) return true;
    return false;
}

static const int PROPTYPE_SCAN_MAX = 0x200;
static int g_propNameOff = -1;

static bool propReadIdAt(uintptr_t at, char* out, int outsz) {
    char raw[80];
    if (!safeReadCStr(at, raw, sizeof raw) || !raw[0]) return false;
    int n = (int)strlen(raw);
    if (n < 3 || n > 63) return false;
    for (int i = 0; i < n; i++) {
        char c = raw[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) return false;
    }
    _snprintf(out, outsz, "%s", raw); out[outsz - 1] = 0;
    return true;
}

static int propFindNameOffset(uintptr_t propType) {
    if (g_propNameOff != -1) return g_propNameOff;
    for (int off = 0; off + 4 < PROPTYPE_SCAN_MAX; off += 4) {
        char cand[80];
        if (!propReadIdAt(propType + off, cand, sizeof cand)) continue;
        if (!propNameIsKnown(cand)) continue;
        g_propNameOff = off;
        logLine("prop: PropType name offset = +0x%x (matched shipped id \"%s\")", off, cand);
        return off;
    }
    g_propNameOff = -2;
    logLine("prop: PropType name offset NOT FOUND — no known prop id in the first 0x%x bytes. "
            "Props will not be named. See the prop-dump lines below.", PROPTYPE_SCAN_MAX);
    return -2;
}

static bool propIdOf(uintptr_t prop, char* out, int outsz) {
    out[0] = 0;
    uintptr_t pt = 0;
    if (!safeReadPtr(prop + PROP_TYPE_OFF, &pt) || pt <= 0x10000) return false;
    int off = propFindNameOffset(pt);
    if (off < 0) return false;
    return propReadIdAt(pt + off, out, outsz);
}

static const char* propUiNameFor(const char* id) {
    for (int i = 0; i < kCurioUiCount; i++)
        if (strcmp(kCurioUiFor[i][0], id) == 0) return kCurioUiFor[i][1];
    return nullptr;
}

void propPrettyName(const char* id, char* out, int outsz) {
    int o = 0;
    for (int i = 0; id[i] && o < outsz - 1; i++) {
        char c = id[i];
        if (c == '_') c = ' ';
        else if (i == 0 && c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
        out[o++] = c;
    }
    out[o] = 0;
}

static bool propIsTrap(uintptr_t prop) {
    int32_t kind = -1;
    if (!prop || !safeReadU32(prop + PROP_TYPE_ENUM_OFF, reinterpret_cast<uint32_t*>(&kind)))
        return false;
    return kind == PROP_KIND_TRAP;
}

// ---- The PropType's OWN naming pair — the DLC fix ----
static bool propTypeKeyName(uintptr_t base, uintptr_t prop,
                            char* name, int nameSz, char* desc, int descSz) {
    uintptr_t pt = 0;
    if (!safeReadPtr(prop + PROP_TYPE_OFF, &pt) || pt <= 0x10000) return false;
    char cat[64] = {0}, ui[96] = {0};
    if (!safeReadCStr(pt + PT_CATEGORY_OFF, cat, sizeof cat) || !cat[0]) return false;
    if (!safeReadCStr(pt + PT_UINAME_OFF,   ui,  sizeof ui)  || !ui[0])  return false;
    for (const char* p = cat; *p; ++p)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '_')) return false;
    for (const char* p = ui; *p; ++p)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '_')) return false;

    char key[224];
    _snprintf(key, sizeof key, "str_%s_title_%s", cat, ui);   key[sizeof key - 1] = 0;
    if (!resolveKey(base, key, name, nameSz) || !name[0]) return false;   // false => empty, always
    abStripMarkup(name);
    _snprintf(key, sizeof key, "str_%s_content_%s", cat, ui); key[sizeof key - 1] = 0;
    if (resolveKey(base, key, desc, descSz) && desc[0]) abStripMarkup(desc);
    return name[0] != 0;
}

static bool propTextFor(uintptr_t base, uintptr_t prop,
                        char* name, int nameSz, char* desc, int descSz) {
    name[0] = desc[0] = 0;

    if (propIsTrap(prop)) {
        mapContentLabel(base, AREA_CONTENT_TRAP, name, nameSz);
        return name[0] != 0;
    }

    if (propTypeKeyName(base, prop, name, nameSz, desc, descSz)) return true;

    char key[160];
    char inlineId[96] = {0};
    if (safeReadCStr(prop + PROP_INLINE_NAME_OFF, inlineId, sizeof inlineId) && inlineId[0]) {
        _snprintf(key, sizeof key, "str_obstacle_%s_title", inlineId);
        if (resolveKey(base, key, name, nameSz) && name[0]) {
            abStripMarkup(name);
            _snprintf(key, sizeof key, "str_obstacle_%s_description", inlineId);
            if (resolveKey(base, key, desc, descSz) && desc[0]) abStripMarkup(desc);
            return true;
        }
    }

    char id[80];
    if (!propIdOf(prop, id, sizeof id)) {
        if (!inlineId[0]) return false;
        propPrettyName(inlineId, name, nameSz);
        static char s_lastInline[96] = { 0 };
        if (strcmp(s_lastInline, inlineId) != 0) {
            _snprintf(s_lastInline, sizeof s_lastInline, "%s", inlineId);
            s_lastInline[sizeof s_lastInline - 1] = 0;
            logLine("prop: no PropType title and no id scan for inline \"%s\" -> prettified \"%s\"",
                    inlineId, name);
        }
        return name[0] != 0;
    }

    const char* ui = propUiNameFor(id);
    if (ui) {
        _snprintf(key, sizeof key, "str_curio_title_%s", ui);
        if (resolveKey(base, key, name, nameSz) && name[0]) abStripMarkup(name);
        _snprintf(key, sizeof key, "str_curio_content_%s", ui);
        if (resolveKey(base, key, desc, descSz) && desc[0]) abStripMarkup(desc);
    }
    if (!name[0]) {
        _snprintf(key, sizeof key, "str_obstacle_%s_title", id);
        if (resolveKey(base, key, name, nameSz) && name[0]) abStripMarkup(name);
        _snprintf(key, sizeof key, "str_obstacle_%s_description", id);
        if (resolveKey(base, key, desc, descSz) && desc[0]) abStripMarkup(desc);
    }
    if (!name[0]) {
        propPrettyName(ui ? ui : id, name, nameSz);
        static char s_lastPretty[80] = { 0 };
        if (strcmp(s_lastPretty, id) != 0) {
            _snprintf(s_lastPretty, sizeof s_lastPretty, "%s", id);
            s_lastPretty[sizeof s_lastPretty - 1] = 0;
            logLine("prop: no string-table title for id=\"%s\" (ui=\"%s\") -> prettified \"%s\"",
                    id, ui ? ui : "(not in csv)", name);
        }
    }
    return name[0] != 0;
}

static int rvPropList(uintptr_t base, uintptr_t* out, int maxOut) {
    uintptr_t rd = (uintptr_t)g_raidDisplay;
    if (!rd) return 0;
    (void)base;

    uintptr_t dv = 0;
    if (!safeReadPtr(rd + RD_DUNGEONVIEW_OFF, &dv) || dv <= 0x10000) {
        logLine("roomview: no Panel_DungeonView at RaidDisplay+0x%x", (unsigned)RD_DUNGEONVIEW_OFF);
        return 0;
    }
    uintptr_t beg = 0, end = 0;
    if (!safeReadPtr(dv + DV_PROPS_BEG_OFF, &beg) || !safeReadPtr(dv + DV_PROPS_END_OFF, &end))
        return 0;
    if (!beg || end < beg) return 0;

    int64_t n = (int64_t)((end - beg) / 8);
    if (n <= 0) return 0;
    if (n > RV_MAX_PROPS) {
        logLine("roomview: prop vector claims %lld entries, capping at %d", (long long)n, RV_MAX_PROPS);
        n = RV_MAX_PROPS;
    }

    int got = 0;
    for (int64_t i = 0; i < n && got < maxOut; i++) {
        uintptr_t p = 0;
        if (safeReadPtr(beg + (uintptr_t)i * 8, &p) && p > 0x10000) out[got++] = p;
    }
    return got;
}

// ---- THE TRAP DISARM CHANCE ----
static bool rvTrapDisarmChance(uintptr_t base, uintptr_t hero, float* out) {
    if (!hero || !out) return false;

    float resist = 0.0f;
    if (!csResistValue(base, hero, CS_RESIST_TRAP_IDX, &resist)) {
        logLine("trapchance: resistance call faulted for hero=%p", (void*)hero);
        return false;
    }

    uint32_t bonusBits = 0;
    if (!safeReadU32(base + TRAP_SCOUT_BONUS_RVA, &bonusBits)) {
        logLine("trapchance: trap_scout_disarm_bonus unreadable");
        return false;
    }
    const float bonus = u32AsFloatM(bonusBits);

    float penalty = 0.0f;
    uintptr_t tblBeg = 0, tblEnd = 0;
    if (!safeReadPtr(base + TRAP_DIFF_BASE_BEG_RVA, &tblBeg) ||
        !safeReadPtr(base + TRAP_DIFF_BASE_END_RVA, &tblEnd)) {
        logLine("trapchance: difficulty_trap_base vector unreadable");
        return false;
    }
    if (tblBeg && tblEnd >= tblBeg) {
        uint64_t count = (uint64_t)(tblEnd - tblBeg) / 4u;
        int lvl = qtQuestLevel(base);
        if (lvl >= 0 && count && (uint64_t)lvl < count) {
            uint32_t bits = 0;
            if (!safeReadU32(tblBeg + (uintptr_t)lvl * 4u, &bits)) {
                logLine("trapchance: difficulty_trap_base[%d] unreadable", lvl);
                return false;
            }
            penalty = u32AsFloatM(bits);
        }
    }

    *out = resist + bonus - penalty;
    return true;
}

// ---- WHEN the clause is heard: the game's own INTERACTION BOX ----
static bool rvReadF32(uintptr_t addr, float* out) {
    uint32_t bits = 0;
    if (!safeReadU32(addr, &bits)) return false;
    *out = u32AsFloatM(bits);
    return true;
}

static const uintptr_t ACTOR_BOX_X_OFF   = 0xa64;   // Actor+: float, the lead hero's x
static const uintptr_t ACTOR_BOX_Y_OFF   = 0xa68;
static const uintptr_t ACTOR_BOX_W_OFF   = 0xa70;   // Actor+: float, scaled by the interaction width
static const uintptr_t PROP_BOX_MINX_OFF = 0x0fc;   // Prop+: float, the prop's own box
static const uintptr_t PROP_BOX_MAXX_OFF = 0x100;
static const uintptr_t PROP_BOX_MINY_OFF = 0x104;
static const uintptr_t PROP_BOX_MAXY_OFF = 0x108;

static bool rvTrapInReach(uintptr_t base) {
    uintptr_t props[RV_MAX_PROPS];
    int n = rvPropList(base, props, RV_MAX_PROPS);
    if (n <= 0) return false;

    uintptr_t root = mapRoot(base);
    if (!root) return false;

    uintptr_t area = 0;
    if (!safeReadPtr(root + MAP_CUR_AREA_PTR, &area) || !area) return false;
    int partyTile = -1;
    if (!sehPartyTile(base, root, &partyTile) || partyTile < 0) {
        logLine("trapchance: party tile unreadable -- clause suppressed");
        return false;
    }
    const int ahead = partyTile + 1;
    if (ahead >= (int)mapAreaTiles(area)) return false;      // the game's own bound
    if (!tileContentsVisible(area, ahead)) return false;     // still fogged: say nothing

    uintptr_t beg = 0, end = 0, lead = 0;
    if (!safeReadPtr(root + RAID_PARTY_BEG_OFF, &beg) ||
        !safeReadPtr(root + RAID_PARTY_END_OFF, &end) || beg <= 0x10000 || end <= beg) return false;
    if (!safeReadPtr(beg, &lead) || lead <= 0x10000) return false;   // party[0], the game's own pick

    float ax = 0, ay = 0, aw = 0, pos = 0, boxW = 0, boxH = 0;
    if (!rvReadF32(lead + ACTOR_BOX_X_OFF, &ax) ||
        !rvReadF32(lead + ACTOR_BOX_Y_OFF, &ay) ||
        !rvReadF32(lead + ACTOR_BOX_W_OFF, &aw) ||
        !rvReadF32(root + MAP_PARTY_POS_OFF, &pos) ||
        !rvReadF32(base + REACH_BOX_WIDTH_RVA, &boxW) ||
        !rvReadF32(base + REACH_BOX_HEIGHT_RVA, &boxH)) {
        logLine("trapchance: interaction box unreadable -- clause suppressed");
        return false;
    }

    float x2 = boxW * aw + pos;
    float loX = ax, hiX = x2;
    if (hiX < loX) { loX = x2; hiX = ax; }
    const float loY = ay, hiY = ay + boxH;

    for (int i = 0; i < n; i++) {
        // (b) the filter lambda, both terms, read here rather than assumed of the vector.
        if (!propIsTrap(props[i])) continue;
        uint8_t seen = 0;
        if (!safeReadU8(props[i] + PROP_TRAP_SEEN_OFF, &seen) || !seen) continue;
        // (c) the box.
        float pminx = 0, pmaxx = 0, pminy = 0, pmaxy = 0;
        if (!rvReadF32(props[i] + PROP_BOX_MINX_OFF, &pminx) ||
            !rvReadF32(props[i] + PROP_BOX_MAXX_OFF, &pmaxx) ||
            !rvReadF32(props[i] + PROP_BOX_MINY_OFF, &pminy) ||
            !rvReadF32(props[i] + PROP_BOX_MAXY_OFF, &pmaxy)) continue;
        if (pminx <= hiX && loX <= pmaxx && pminy <= hiY && loY <= pmaxy) return true;
    }
    return false;
}

// ---- THE CURIO REACH GATE, and what the tile step borrows from this file ----
static const uintptr_t PROP_POS_X_OFF        = 0x0e0;   // Prop+: float, the prop's x in the travel space
static const uintptr_t PROP_POS_Y_OFF        = 0x0e4;   // Prop+: float, its y
static const uintptr_t PROP_RECT_MINX_OFF    = 0x10c;   // Prop+: the rect the lead hero's point may lie in
static const uintptr_t PROP_RECT_MAXX_OFF    = 0x110;
static const uintptr_t PROP_RECT_MINY_OFF    = 0x114;
static const uintptr_t PROP_RECT_MAXY_OFF    = 0x118;
static const uintptr_t PROP_ACTIVE_OFF       = 0x010;   // Prop+: bool, the byte the gate tests last
static const uintptr_t MAP_INTERACT_LOCK_OFF = 0x4b20;

bool rvPropReach(uintptr_t base, uintptr_t prop, bool* inReachOut, int* dirOut, float* dxOut) {
    *inReachOut = false; *dirOut = 0; if (dxOut) *dxOut = 0;
    uintptr_t root = mapRoot(base);
    if (!root || !prop) return false;
    uintptr_t beg = 0, end = 0, lead = 0;
    if (!safeReadPtr(root + RAID_PARTY_BEG_OFF, &beg) ||
        !safeReadPtr(root + RAID_PARTY_END_OFF, &end) || beg <= 0x10000 || end <= beg) return false;
    if (!safeReadPtr(beg, &lead) || lead <= 0x10000) return false;

    float ax = 0, ay = 0, aw = 0, pos = 0, boxW = 0, boxH = 0;
    if (!rvReadF32(lead + ACTOR_BOX_X_OFF, &ax) ||
        !rvReadF32(lead + ACTOR_BOX_Y_OFF, &ay) ||
        !rvReadF32(lead + ACTOR_BOX_W_OFF, &aw) ||
        !rvReadF32(root + MAP_PARTY_POS_OFF, &pos) ||
        !rvReadF32(base + REACH_BOX_WIDTH_RVA, &boxW) ||
        !rvReadF32(base + REACH_BOX_HEIGHT_RVA, &boxH)) return false;
    float px = 0, py = 0, rminx = 0, rmaxx = 0, rminy = 0, rmaxy = 0;
    if (!rvReadF32(prop + PROP_POS_X_OFF, &px) || !rvReadF32(prop + PROP_POS_Y_OFF, &py) ||
        !rvReadF32(prop + PROP_RECT_MINX_OFF, &rminx) || !rvReadF32(prop + PROP_RECT_MAXX_OFF, &rmaxx) ||
        !rvReadF32(prop + PROP_RECT_MINY_OFF, &rminy) || !rvReadF32(prop + PROP_RECT_MAXY_OFF, &rmaxy))
        return false;
    uint8_t active = 0; uint32_t lock = 0;
    if (!safeReadU8(prop + PROP_ACTIVE_OFF, &active)) return false;
    if (!safeReadU32(root + MAP_INTERACT_LOCK_OFF, &lock)) return false;

    float x2 = boxW * aw + pos;
    float lo = ax, hi = x2;
    if (hi < lo) { lo = x2; hi = ax; }
    const float ylo = ay, yhi = ay + boxH;

    bool inBand = (lo <= px && px <= hi && ylo <= py && py <= yhi);
    bool inRect = (rminx <= ax && ax <= rmaxx && rminy <= ay && ay <= rmaxy);
    *inReachOut = (lock == 0) && (inBand || inRect) && active != 0;
    if (px > hi)      { *dirOut = +1; if (dxOut) *dxOut = px - hi; }
    else if (px < lo) { *dirOut = -1; if (dxOut) *dxOut = lo - px; }
    return true;
}

int  rvRoomProps(uintptr_t base, uintptr_t* out, int max) { return rvPropList(base, out, max); }
bool rvPropIsTrap(uintptr_t prop)                          { return propIsTrap(prop); }
bool rvPropActive(uintptr_t prop) {
    uint8_t a = 0; return safeReadU8(prop + PROP_ACTIVE_OFF, &a) && a != 0;
}
bool rvPropName(uintptr_t base, uintptr_t prop, char* out, int outsz) {
    char desc[512];
    return propTextFor(base, prop, out, outsz, desc, sizeof desc) && out[0] != 0;
}

void rvTrapDisarmSuffix(uintptr_t base, uintptr_t hero, char* out, int outsz) {
    out[0] = 0;
    if (!hero || !rvTrapInReach(base)) return;
    float chance = 0.0f;
    if (!rvTrapDisarmChance(base, hero, &chance)) return;
    // *100 is the game's own scale factor at this call site (DAT_140ed2cfc = 100.0f).
    if (!abTipFloat(base, "resistance_trap_disarm_format", chance * 100.0f, out, outsz))
        out[0] = 0;
}

// ---- STAGE 3: DOORS ----
static uint32_t rvDoorLeadsTo(uintptr_t root, uintptr_t curArea, uint32_t curId, uintptr_t door) {
    uint32_t via = 0;
    if (!safeReadU32(door + DOOR_DEST_OFF, &via) || !via || via == FOURCC_NONE) return 0;
    (void)curArea;

    uintptr_t link = mapAreaById(root, via);
    if (!link) return via;
    int32_t lkind = -1;
    safeReadU32(link + AREA_KIND_OFF, reinterpret_cast<uint32_t*>(&lkind));
    if (lkind != 1) return via;            // not a corridor: it IS the destination

    uintptr_t tb = 0;
    safeReadPtr(link + AREA_TILES_BEG, &tb);
    long tiles = mapAreaTiles(link);
    if (!tb || tiles <= 0) return via;
    bool endsNameUs = false;
    uint32_t farEnd = 0;
    for (int e = 0; e < 2; e++) {
        uintptr_t tile = tb + (uintptr_t)(e ? tiles - 1 : 0) * TILE_STRIDE;
        int32_t ttype = -1; uint32_t tneigh = 0;
        safeReadU32(tile + TILE_TYPE_OFF, reinterpret_cast<uint32_t*>(&ttype));
        safeReadU32(tile + TILE_NEIGH_ID_OFF, &tneigh);
        if (ttype != 2 || !tneigh || tneigh == FOURCC_NONE) continue;
        if (tneigh == curId) endsNameUs = true;
        else                 farEnd = tneigh;
    }
    if (endsNameUs && farEnd) return farEnd;
    return via;
}

static int rvDoorList(uintptr_t base, uint32_t* destOut, uintptr_t* doorOut, int maxOut) {
    uintptr_t root = mapRoot(base);
    if (!root) return 0;
    uintptr_t curArea = 0;
    if (!safeReadPtr(root + MAP_CUR_AREA_PTR, &curArea) || !curArea) return 0;
    uint32_t curId = 0;
    safeReadU32(curArea + AREA_ID_OFF, &curId);

    int got = 0;
    int32_t akind = -1;
    safeReadU32(curArea + AREA_KIND_OFF, reinterpret_cast<uint32_t*>(&akind));
    if (akind == 1) {
        uintptr_t tb = 0;
        safeReadPtr(curArea + AREA_TILES_BEG, &tb);
        long tiles = mapAreaTiles(curArea);
        if (!tb || tiles <= 0) return 0;
        int partyTile = -1;                              // -1 = unreadable -> list every end door
        if (!sehPartyTile(base, root, &partyTile)) {
            partyTile = -1;
            logLine("rv-door: party tile read FAULTED - listing both corridor ends");
        }
        for (int e = 0; e < 2 && got < maxOut; e++) {
            if (e == 1 && tiles < 2) break;              // a one-tile hall: don't read tile 0 twice
            long ti = e ? tiles - 1 : 0;
            if (partyTile >= 0 && partyTile != ti) continue;   // not standing at this end door
            uintptr_t tile = tb + (uintptr_t)ti * TILE_STRIDE;
            int32_t ttype = -1; uint32_t tneigh = 0;
            safeReadU32(tile + TILE_TYPE_OFF, reinterpret_cast<uint32_t*>(&ttype));
            safeReadU32(tile + TILE_NEIGH_ID_OFF, &tneigh);
            if (ttype != 2 || !tneigh || tneigh == FOURCC_NONE) continue;
            destOut[got] = tneigh;
            doorOut[got] = tile;
            got++;
        }
        return got;
    }
    for (int s = 0; s < AREA_EXIT_SLOTS && got < maxOut; s++) {
        uintptr_t door = curArea + AREA_EXITS_OFF + (uintptr_t)s * AREA_EXIT_STRIDE;
        uint32_t dest = rvDoorLeadsTo(root, curArea, curId, door);
        if (!dest || dest == FOURCC_NONE) continue;
        destOut[got] = dest;
        doorOut[got] = door;
        got++;
    }
    return got;
}

static bool rvDoorRowText(uintptr_t base, uint32_t destId, int slot, int total, char* out, int outsz) {
    uintptr_t root = mapRoot(base);
    char label[64];
    if (root) mapAreaLabelById(root, destId, label, sizeof label);   // corridor -> endpoint naming
    else      mapAreaLabel(destId, 0, label, sizeof label);
    int o = _snprintf(out, outsz, axs(AXS_RV_EXIT_TO_FMT), label, slot, total);
    if (root && o > 0 && o < outsz) {
        uintptr_t curArea = 0;
        int32_t akind = -1;
        int partyTile = -1;
        if (safeReadPtr(root + MAP_CUR_AREA_PTR, &curArea) && curArea &&
            safeReadU32(curArea + AREA_KIND_OFF, reinterpret_cast<uint32_t*>(&akind)) &&
            akind == 1 && sehPartyTile(base, root, &partyTile)) {
            long tiles = mapAreaTiles(curArea);
            if (partyTile == 0 || (tiles > 0 && partyTile == (int)tiles - 1))
                _snprintf(out + o, outsz - o, " %s", axs(AXS_DOOR_PRESS_W));
        }
    }
    out[outsz - 1] = 0;
    return true;
}

static bool rvHiddenDoorRowText(uintptr_t base, uint32_t destId, char* out, int outsz) {
    char door[128];
    mapContentLabel(base, AREA_CONTENT_HIDDEN_DOOR, door, sizeof door);
    _snprintf(out, outsz, "%s. %s", door, axs(AXS_DOOR_PRESS_W));
    out[outsz - 1] = 0;
    (void)destId;
    return true;
}

static bool rvPropRowText(uintptr_t base, uintptr_t prop, int slot, int total, char* out, int outsz) {
    char name[160], desc[512];
    if (!propTextFor(base, prop, name, sizeof name, desc, sizeof desc)) return false;
    _snprintf(out, outsz, axs(AXS_RV_OBJECT_N_OF_M_FMT), name, slot, total);
    out[outsz - 1] = 0;
    return true;
}

// ---- Enter on a trap row: DISARM ----
static bool rvDisarmTrap(uintptr_t base, uintptr_t prop) {
    uintptr_t rd = (uintptr_t)g_raidDisplay;
    if (!rd) {
        logLine("trap: no RaidDisplay captured — cannot disarm");
        postSpeech(axs(AXS_RV_TRAP_UNREACHABLE));
        return true;
    }
    uintptr_t oe = rd + RD_EVENT_OFF;

    int32_t before = -1;
    safeReadU32(oe + OE_STATE_OFF, reinterpret_cast<uint32_t*>(&before));
    uint8_t seen = 0;
    safeReadU8(prop + PROP_TRAP_SEEN_OFF, &seen);
    logLine("trap: disarm requested prop=%p seen=%u overlay state before=%d",
            (void*)prop, seen, before);

    typedef char (*InteractTrapFn)(uintptr_t rd, uintptr_t prop);
    InteractTrapFn fn = reinterpret_cast<InteractTrapFn>(base + RD_INTERACT_TRAP_RVA);
    char ret = 0;
    __try { ret = fn(rd, prop); }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        logLine("⚠ trap: InteractWithTrap faulted");
        postSpeech(axs(AXS_RV_TRAP_UNREACHABLE));
        return true;
    }

    int32_t state = -1; uintptr_t armed = 0; int32_t hidx = -1; uint8_t deliberate = 0;
    safeReadU32(oe + OE_STATE_OFF, reinterpret_cast<uint32_t*>(&state));
    safeReadPtr(oe + OE_OTHER_PROP_OFF, &armed);
    safeReadU32(oe + OE_TRAP_HERO_IDX_OFF, reinterpret_cast<uint32_t*>(&hidx));
    safeReadU8 (oe + OE_TRAP_DELIBERATE_OFF, &deliberate);
    logLine("trap: InteractWithTrap returned %d — state %d -> %d, armed prop=%p, hero index=%d, "
            "deliberate=%u", (int)ret, before, state, (void*)armed, hidx, deliberate);

    if (state != OE_STATE_LIVE || armed != prop) {
        return true;
    }

    // Name the hero the GAME chose, from the index it just wrote.
    char who[160]; who[0] = 0;
    uintptr_t party[RV_MAX_MEMBERS];
    int np = rvPartyList(base, party, RV_MAX_MEMBERS);
    if (hidx >= 0 && hidx < np) abHeroLabelOf(base, party[hidx], who, sizeof who);

    char trap[160];
    mapContentLabel(base, AREA_CONTENT_TRAP, trap, sizeof trap);

    char utter[MAILBOX_SZ];
    if (who[0]) _snprintf(utter, sizeof utter, axs(AXS_RV_DISARMING_WHO_FMT), who, trap);
    else        _snprintf(utter, sizeof utter, axs(AXS_RV_DISARMING_FMT), trap);
    utter[sizeof utter - 1] = 0;
    postSpeech(utter);
    return true;
}

// ---- Enter on a CURIO row: INTERACT ----

static bool rvInteractProp(uintptr_t base, uintptr_t prop) {
    uintptr_t rd = (uintptr_t)g_raidDisplay;
    if (!rd) {
        logLine("curio: no RaidDisplay captured — cannot interact");
        postSpeech(axs(AXS_RV_CANT_USE_HERE));
        return true;
    }
    uintptr_t oe = rd + RD_EVENT_OFF;

    int32_t before = -1;
    uintptr_t curioBefore = 0, otherBefore = 0;
    safeReadU32(oe + OE_STATE_OFF, reinterpret_cast<uint32_t*>(&before));
    safeReadPtr(oe + OE_CURIO_PROP_OFF, &curioBefore);
    safeReadPtr(oe + OE_OTHER_PROP_OFF, &otherBefore);

    if (before != 0) {
        logLine("curio: overlay state is %d (not idle) — not interacting", before);
        postSpeech(axs(AXS_RV_SOMETHING_OPEN));
        return true;
    }

    char nm[160] = {0}, dsc[512] = {0};
    propTextFor(base, prop, nm, sizeof nm, dsc, sizeof dsc);
    logLine("curio: interact requested prop=%p (\"%s\") state before=%d", (void*)prop, nm, before);

    typedef char (*InteractPropFn)(uintptr_t rd, uintptr_t prop, int idx, char checkReach);
    InteractPropFn fn = reinterpret_cast<InteractPropFn>(base + RD_INTERACT_PROP_RVA);
    char ret = 0;
    __try { ret = fn(rd, prop, 0, 1); }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        logLine("⚠ curio: InteractWithProp faulted");
        postSpeech(axs(AXS_RV_CANT_USE_HERE));
        return true;
    }

    int32_t state = -1;
    uintptr_t curioNow = 0, otherNow = 0;
    safeReadU32(oe + OE_STATE_OFF, reinterpret_cast<uint32_t*>(&state));
    safeReadPtr(oe + OE_CURIO_PROP_OFF, &curioNow);
    safeReadPtr(oe + OE_OTHER_PROP_OFF, &otherNow);
    logLine("curio: InteractWithProp returned %d — state %d -> %d, curio prop %p -> %p, "
            "other prop %p -> %p", (int)ret, before, state,
            (void*)curioBefore, (void*)curioNow, (void*)otherBefore, (void*)otherNow);

    if (state != before && (curioNow == prop || otherNow == prop)) return true;

    return true;
}

// ---- The SECRET DOOR is deliberately NOT actionable from here ----
static const uint32_t RV_ADV_WATCH_MS = 4000;

static bool            g_rvAdvWatch = false;
static uint32_t        g_rvAdvDeadline = 0;
static ComMoveSnapshot g_rvAdvBefore;

static bool rvAdvanceRoom(uintptr_t base) {
    if (g_rvAdvWatch) {
        // The transition is already running. Saying nothing would read as a dead key.
        logLine("wayon: already moving — the press is ignored");
        postSpeech(axs(AXS_RV_ALREADY_MOVING));
        return true;
    }
    comWaveMoveSnapshot(base, &g_rvAdvBefore);
    if (!comAdvanceRoom(base)) {
        postSpeech(axs(AXS_RV_CANT_MOVE_ON));
        return true;
    }
    g_rvAdvWatch = true;
    g_rvAdvDeadline = GetTickCount() + RV_ADV_WATCH_MS;
    logLine("wayon: transition armed, watching for %u ms", RV_ADV_WATCH_MS);
    return true;
}

void serviceAdvanceWatch(uintptr_t base) {
    if (!g_rvAdvWatch) return;

    ComMoveSnapshot now;
    comWaveMoveSnapshot(base, &now);
    if (comWaveMoveHappened(&g_rvAdvBefore, &now)) {
        g_rvAdvWatch = false;
        logLine("wayon: the wave logic moved (transition %u->%u, curio %u->%u, action %u->%u, "
                "room %u->%u)",
                g_rvAdvBefore.transition, now.transition,
                g_rvAdvBefore.curioSpawned, now.curioSpawned,
                g_rvAdvBefore.nextAction, now.nextAction,
                g_rvAdvBefore.currRoom, now.currRoom);
        postSpeech(axs(AXS_RV_MOVING_ON));
        return;
    }
    if (GetTickCount() >= g_rvAdvDeadline) {
        g_rvAdvWatch = false;
        logLine("wayon: nothing changed within %u ms — the transition did not take",
                RV_ADV_WATCH_MS);
        comWaveProbe(base);
        postSpeech(axs(AXS_RV_DIDNT_MOVE));
    }
}

static bool g_rvAdvOffered = false;

static bool rvAdvanceRowText(uintptr_t base, char* out, int outsz);   // defined with the row texts

void serviceAdvanceOffer(uintptr_t base) {
    bool now = comCanAdvance(base);
    if (now == g_rvAdvOffered) return;
    g_rvAdvOffered = now;
    if (!now) { logLine("wayon: the way on is no longer offered"); return; }

    if (g_rvActive && !axCoversRestingPoint() && !g_mapReview) {
        RvEntry rows[RV_MAX_ROWS];
        int n = rvBuildRoom(base, rows, RV_MAX_ROWS);
        for (int i = n - 1; i >= 0; i--) {          // the way-on row is appended last; scan back
            if (rows[i].kind != RV_ADVANCE) continue;
            char row[300];
            if (!rvAdvanceRowText(base, row, sizeof row)) break;
            g_rvCursor  = i;
            g_rvTipLine = 0;
            logLine("wayon: offered -> cursor moved to the way-on row (%d of %d) -> \"%s\"",
                    i + 1, n, row);
            postSpeech(row, false, SPK_EVENT);
            return;
        }
    }

    char label[256];
    if (!comAdvanceLabel(base, label, sizeof label) || !label[0]) return;
    char utter[MAILBOX_SZ];
    _snprintf(utter, sizeof utter, axs(AXS_RV_LAST_ROW_FMT), label);
    utter[sizeof utter - 1] = 0;
    logLine("wayon: offered -> \"%s\"", utter);
    postSpeech(utter, false, SPK_EVENT);
}

int rvBuildRoom(uintptr_t base, RvEntry* out, int maxOut) {
    uintptr_t party[RV_MAX_MEMBERS];
    uintptr_t foes[RV_MAX_ENEMIES];
    int np = rvPartyList(base, party, RV_MAX_MEMBERS);
    int ne = rvEnemyList(base, foes,  RV_MAX_ENEMIES);

    int npRanks = rvRankFootprint(party, np);
    int neRanks = rvRankFootprint(foes,  ne);
    if (npRanks != np || neRanks != ne)
        logLine("rv-rank: line width party %d bodies/%d ranks, enemies %d bodies/%d ranks",
                np, npRanks, ne, neRanks);

    int n = 0;
    for (int i = np - 1; i >= 0 && n < maxOut; i--) {
        out[n] = RvEntry();
        out[n].actor = party[i]; out[n].kind = RV_ACTOR;
        out[n].enemy = false; out[n].idx = i; out[n].total = npRanks;
        out[n].slot  = rvSlotNumber(party[i], false, i, np, &out[n].slotEnd);
        n++;
    }
    for (int i = 0; i < ne && n < maxOut; i++) {
        out[n] = RvEntry();
        out[n].actor = foes[i];  out[n].kind = RV_ACTOR;
        out[n].enemy = true;  out[n].idx = i; out[n].total = neRanks;
        out[n].slot  = rvSlotNumber(foes[i], true, i, ne, &out[n].slotEnd);
        n++;
    }

    uintptr_t props[RV_MAX_PROPS];
    int npr = rvPropList(base, props, RV_MAX_PROPS);
    for (int i = 0; i < npr && n < maxOut; i++) {
        out[n] = RvEntry();
        out[n].kind = RV_PROP; out[n].obj = props[i];
        out[n].idx = i; out[n].total = npr; out[n].slot = i + 1;
        n++;
    }

    if (!comIsWaveRaid(base)) {
        uint32_t dests[RV_MAX_DOORS];
        uintptr_t doors[RV_MAX_DOORS];
        int nd = rvDoorList(base, dests, doors, RV_MAX_DOORS);
        for (int i = 0; i < nd && n < maxOut; i++) {
            out[n] = RvEntry();
            out[n].kind = RV_DOOR; out[n].obj = doors[i]; out[n].destId = dests[i];
            out[n].idx = i; out[n].total = nd; out[n].slot = i + 1;
            n++;
        }

        uintptr_t root = mapRoot(base);
        uintptr_t curArea = 0;
        if (root) safeReadPtr(root + MAP_CUR_AREA_PTR, &curArea);
        int32_t curKind = -1;
        if (curArea) safeReadU32(curArea + AREA_KIND_OFF, reinterpret_cast<uint32_t*>(&curKind));
        if (curKind == 1 && n < maxOut) {
            uint32_t sid = 0; uintptr_t tileP = 0;
            int ti = areaFindHiddenDoor(curArea, &sid, &tileP);
            int partyTile = -1;                          // -1 = unreadable -> list it anyway
            if (ti >= 0 && !sehPartyTile(base, root, &partyTile)) {
                partyTile = -1;
                logLine("rv-secret: party tile read FAULTED - listing the secret door anyway");
            }
            if (ti >= 0 && partyTile >= 0 && partyTile != ti) {
                logLine("rv-secret: the hidden door is on tile %d, the party is on %d - not in reach",
                        ti, partyTile);
                ti = -1;
            }
            if (ti >= 0) {
                out[n] = RvEntry();
                out[n].kind = RV_HIDDEN_DOOR; out[n].obj = tileP; out[n].destId = sid;
                out[n].idx = ti; out[n].total = 1; out[n].slot = 1;
                n++;
            }
        }
    }

    if (n < maxOut && comCanAdvance(base)) {
        out[n] = RvEntry();
        out[n].kind = RV_ADVANCE;
        out[n].idx = 0; out[n].total = 1; out[n].slot = 1;
        n++;
    }
    return n;
}

static const char* rvGenericLabel(const RvEntry& e) {
    switch (e.kind) {
        case RV_PROP:        return axs(AXS_RV_LABEL_OBJECT);
        case RV_DOOR:        return axs(AXS_RV_LABEL_EXIT);
        case RV_ADVANCE:     return axs(AXS_RV_LABEL_MOVE_ON);
        case RV_HIDDEN_DOOR: return axs(AXS_SECRET_DOOR_LABEL);
        default:             return axs(e.enemy ? AXS_RV_LABEL_ENEMY : AXS_RV_LABEL_PARTY_MEMBER);
    }
}

static bool rvAdvanceRowText(uintptr_t base, char* out, int outsz) {
    char label[256];
    if (!comAdvanceLabel(base, label, sizeof label) || !label[0]) return false;
    _snprintf(out, outsz, axs(AXS_RV_PRESS_ENTER_FMT), label);
    out[outsz - 1] = 0;
    return true;
}

static bool rvWantsMonsterRow(uintptr_t base, const RvEntry& e) {
    if (e.kind != RV_ACTOR || !e.actor) return e.enemy;
    bool hero = spActorIsHero(base, e.actor);
    if (hero == e.enemy) {                       // the Circus cases; never true in a campaign raid
        static bool s_said[2] = { false, false };
        if (!s_said[e.enemy ? 1 : 0]) {
            s_said[e.enemy ? 1 : 0] = true;
            logLine("roomview: %s-side actor=%p is a %s -- switching describers from here on",
                    e.enemy ? "far" : "party", (void*)e.actor, hero ? "HERO" : "MONSTER");
        }
    }
    return !hero;
}

static bool rvRowText(uintptr_t base, const RvEntry& e, char* out, int outsz) {
    switch (e.kind) {
        case RV_PROP:        return rvPropRowText(base, e.obj,    e.slot, e.total, out, outsz);
        case RV_DOOR:        return rvDoorRowText(base, e.destId, e.slot, e.total, out, outsz);
        case RV_ADVANCE:     return rvAdvanceRowText(base, out, outsz);
        case RV_HIDDEN_DOOR: return rvHiddenDoorRowText(base, e.destId, out, outsz);
        default:             break;
    }
    if (rvWantsMonsterRow(base, e))
        return rvMonsterRowText(base, e.actor, e.slot, e.slotEnd, e.total, e.enemy, out, outsz);
    return rvHeroRowText(base, e.actor, e.slot, e.slotEnd, e.total, e.enemy, out, outsz);
}

void rvPosForEntry(const RvEntry& e, char* out, int outsz) {
    if (e.kind == RV_ACTOR) rvPosPhrase(e.slot, e.slotEnd, false, out, outsz);
    else                    rvPosFrag(e.slot, e.slotEnd, e.total, out, outsz);
}

// ---- the enemy's stats: the hover panel, as a Ctrl+Up/Down buffer ----

static bool rvMonsterTypes(uintptr_t base, uintptr_t mon, char* out, int outsz) {
    out[0] = 0;
    uintptr_t cls = 0;
    if (!safeReadPtr(mon + MONSTER_CLASS_OFF, &cls) || cls <= 0x10000) return false;

    uintptr_t beg = 0, end = 0;
    if (!safeReadPtr(cls + MONSTER_TYPE_BEG_OFF, &beg) ||
        !safeReadPtr(cls + MONSTER_TYPE_END_OFF, &end)) return false;
    if (beg <= 0x10000 || end < beg) return false;

    int64_t n = (int64_t)((end - beg) / MONSTER_TYPE_STRIDE);
    if (n <= 0) return false;
    if (n > RV_TIP_TYPE_MAX) {
        logLine("roomview type: claims %lld types, capping", (long long)n);
        n = RV_TIP_TYPE_MAX;
    }

    char sep[64];
    if (!resolveKey(base, MT_TYPE_SEP_KEY, sep, sizeof sep) || !sep[0]) strcpy(sep, ", ");
    else abStripMarkup(sep);

    int got = 0;
    for (int64_t i = 0; i < n; i++) {
        char id[MONSTER_TYPE_STRIDE + 1];
        if (!safeReadCStr(beg + (uintptr_t)i * MONSTER_TYPE_STRIDE, id, sizeof id) || !id[0]) continue;
        bool ok = true;
        for (const char* p = id; *p; p++)
            if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
                  (*p >= '0' && *p <= '9') || *p == '_')) { ok = false; break; }
        if (!ok) { logLine("roomview type: \"%s\" is not an identifier", id); continue; }

        char key[128], name[128];
        _snprintf(key, sizeof key, MT_TYPE_KEY_FMT, id);
        key[sizeof key - 1] = 0;
        if (!resolveKey(base, key, name, sizeof name) || !name[0]) {
            logLine("roomview type: \"%s\" did not resolve", key);
            continue;
        }
        abStripMarkup(name);
        if (got++) strncat(out, sep, outsz - strlen(out) - 1);
        strncat(out, name, outsz - strlen(out) - 1);
    }
    return got > 0;
}

// The enemy's skill list: how many, and the record for skill i.
int rvSkillCount(uintptr_t mon, uintptr_t* begOut) {
    if (begOut) *begOut = 0;
    uintptr_t cls = 0;
    if (!safeReadPtr(mon + MONSTER_CLASS_OFF, &cls) || cls <= 0x10000) return 0;

    uintptr_t beg = 0, end = 0;
    if (!safeReadPtr(cls + MONSTERCLASS_SKILLVEC_OFF, &beg) ||
        !safeReadPtr(cls + MONSTERCLASS_SKILLVEC_END_OFF, &end)) return 0;
    if (beg <= 0x10000 || end < beg) return 0;

    int64_t n = (int64_t)((end - beg) / MONSTER_SKILL_STRIDE);
    if (n <= 0) return 0;
    if (n > RV_TIP_SKILL_MAX) {
        logLine("roomview skills: claims %lld skills, capping", (long long)n);
        n = RV_TIP_SKILL_MAX;
    }
    if (begOut) *begOut = beg;
    return (int)n;
}

static bool rvSkillName(uintptr_t base, uintptr_t mon, int i, char* out, int outsz) {
    out[0] = 0;
    uintptr_t beg = 0;
    int n = rvSkillCount(mon, &beg);
    if (i < 0 || i >= n || !beg) return false;

    uintptr_t skill = beg + (uintptr_t)i * MONSTER_SKILL_STRIDE;

    char id[0x41];
    if (!safeReadCStr(skill + MONSTER_SKILL_ID_OFF, id, sizeof id) || !id[0]) return false;
    for (const char* p = id; *p; p++)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
              (*p >= '0' && *p <= '9') || *p == '_')) {
            logLine("roomview skills: \"%s\" is not an identifier", id);
            return false;
        }

    char key[128], name[160];
    _snprintf(key, sizeof key, MT_SKILL_KEY_FMT, id);
    key[sizeof key - 1] = 0;
    if (!resolveKey(base, key, name, sizeof name) || !name[0]) {
        logLine("roomview skills: \"%s\" did not resolve, using the id", key);
        _snprintf(name, sizeof name, "%s", id);
    }
    abStripMarkup(name);
    _snprintf(out, outsz, "%s", name);
    out[outsz - 1] = 0;
    return true;
}

// ---- WHICH SKILLS THE GAME'S TOOLTIP ACTUALLY DRAWS ----
static const uintptr_t MONSTER_SKILL_SHOWN_OFF = 0x4a9;

static uint32_t rvSkillIdHash(const char* id) {
    uint32_t h = 0;
    for (const unsigned char* p = (const unsigned char*)id; *p; p++) h = h * 0x35 + *p;
    return h;
}

static bool rvSkillSeen(uintptr_t base, const char* id) {
    static bool storeGrumbled = false;
    uintptr_t obj = 0, head = 0, node = 0;
    if (!safeReadPtr(base + MONSTER_SKILL_SEEN_MAP_RVA, &obj) || obj <= 0x10000) {
        if (!storeGrumbled) { logLine("roomview seen: store global unreadable (obj=%p)", (void*)obj);
                              storeGrumbled = true; }
        return false;
    }
    if (!safeReadPtr(obj, &head) || head <= 0x10000) {
        if (!storeGrumbled) { logLine("roomview seen: store at %p has no map head", (void*)obj);
                              storeGrumbled = true; }
        return false;
    }
    if (!safeReadPtr(head + 0x08, &node) || node <= 0x10000) return false;  // an empty map: root = head is fine,
    uint32_t want = rvSkillIdHash(id);
    uintptr_t bound = head;
    for (int guard = 0; guard < 100; guard++) {          // depth ~2·log2(N); 100 outlasts any real store
        uint8_t isnil = 1;
        if (!safeReadU8(node + 0x19, &isnil)) return false;
        if (isnil) break;
        uint32_t key = 0;
        if (!safeReadU32(node + 0x1c, &key)) return false;
        uintptr_t next = 0;
        if (key < want) {
            if (!safeReadPtr(node + 0x10, &next)) return false;   // too small: go right
        } else {
            if (!safeReadPtr(node + 0x00, &next)) return false;   // >= target: go left...
            bound = node;                                         // ...and this node bounds the search
        }
        if (next <= 0x10000) return false;
        node = next;
    }
    if (bound == head) return false;                     // every key smaller: not in the store
    uint8_t bnil = 1; uint32_t bkey = 0;
    if (!safeReadU8(bound + 0x19, &bnil) || bnil) return false;
    if (!safeReadU32(bound + 0x1c, &bkey)) return false;
    return bkey == want;                                 // lower_bound landed on it: seen
}

static bool rvSkillSlotShown(uintptr_t mon, uintptr_t skill) {
    uint8_t shown = 0;
    if (!safeReadU8(skill + MONSTER_SKILL_SHOWN_OFF, &shown) || !shown) return false;
    uintptr_t setSize = 0;
    if (!safeReadPtr(skill + SKILL_VALIDMODES_SIZE_OFF, &setSize)) return true;  // keep on a torn
    if (setSize == 0) return true;                                               // read, the bar's rule
    uint32_t mode = 0;
    if (!safeReadU32(mon + ACTOR_MODE_ID_OFF, &mode) || mode == 0) return false;
    return abModeAllowsSkill(skill, mode);
}

struct RvSkillSlot { bool seen; char name[160]; };
static int rvSkillSlotList(uintptr_t base, uintptr_t mon, RvSkillSlot* out, int maxOut) {
    uintptr_t beg = 0;
    int n = rvSkillCount(mon, &beg);
    int shown = 0;
    for (int i = 0; i < n && shown < maxOut; i++) {
        uintptr_t skill = beg + (uintptr_t)i * MONSTER_SKILL_STRIDE;
        if (!rvSkillSlotShown(mon, skill)) continue;
        RvSkillSlot& s = out[shown];
        s.seen = false; s.name[0] = 0;
        char id[0x41];
        if (safeReadCStr(skill + MONSTER_SKILL_ID_OFF, id, sizeof id) && id[0])
            s.seen = rvSkillSeen(base, id);
        if (s.seen && (!rvSkillName(base, mon, i, s.name, sizeof s.name) || !s.name[0])) {
            s.seen = false;
        }
        shown++;
    }
    return shown;
}

static bool rvTipStat(uintptr_t base, uintptr_t actor, uintptr_t off,
                      const char* key, float scale, char* out, int outsz) {
    float v = 0.0f;
    if (!csStatOwnMask(actor, off, &v)) return false;      // block's OWN mask at +0x28
    return abTipFloat(base, key, v * scale, out, outsz);
}

static bool rvMonsterHpFrag(uintptr_t base, uintptr_t actor, char* out, int outsz) {
    out[0] = 0;
    float cur = 0.0f, max = 0.0f;
    uint32_t curBits = 0;
    if (!safeReadU32(actor + ACTOR_CUR_HP_OFF, &curBits) ||
        (memcpy(&cur, &curBits, sizeof cur), false) ||
        !csStatOwnMask(actor, CS_STAT_MAXHP_OFF, &max)) {
        logLine("roomview tip: health unreadable for actor=%p", (void*)actor);
        return false;
    }
    if (cur < 0.0f) cur = 0.0f;
    char fmt[256];
    if (!resolveKey(base, MT_HP_KEY, fmt, sizeof fmt) || !fmt[0] ||
        !abFormatMatches(fmt, "ff")) {
        logLine("roomview tip: \"%s\" fmt mismatch/missing", MT_HP_KEY);
        return false;
    }
    _snprintf(out, outsz, fmt, (double)ceilf(cur), (double)ceilf(max));
    out[outsz - 1] = 0;
    abStripMarkup(out);
    return out[0] != 0;
}

static bool rvStatsLine(uintptr_t base, uintptr_t actor, char* out, int outsz) {
    out[0] = 0;
    int got = 0;
    char frag[256];

    if (rvMonsterHpFrag(base, actor, frag, sizeof frag)) {
        strncat(out, frag, outsz - strlen(out) - 1); got++;
    }

    if (rvMonsterTypes(base, actor, frag, sizeof frag) && frag[0]) {
        if (got++) strncat(out, " ", outsz - strlen(out) - 1);
        strncat(out, frag, outsz - strlen(out) - 1);
        strncat(out, ".", outsz - strlen(out) - 1);
    } else logLine("roomview tip: no enemy type for actor=%p", (void*)actor);

    struct { uintptr_t off; const char* key; float scale; } rows[] = {
        { CS_STAT_PROT_OFF, MT_PROT_KEY,  CS_PERCENT_SCALE },
        { CS_STAT_DEF_OFF,  MT_DODGE_KEY, CS_PERCENT_SCALE },
        { CS_STAT_SPD_OFF,  MT_SPEED_KEY, 1.0f             },
    };
    for (int i = 0; i < (int)(sizeof rows / sizeof rows[0]); i++) {
        if (!rvTipStat(base, actor, rows[i].off, rows[i].key, rows[i].scale, frag, sizeof frag)) {
            logLine("roomview tip: \"%s\" unreadable for actor=%p", rows[i].key, (void*)actor);
            continue;
        }
        if (got++) strncat(out, " ", outsz - strlen(out) - 1);
        strncat(out, frag, outsz - strlen(out) - 1);
        strncat(out, ".", outsz - strlen(out) - 1);
    }
    return got > 0;
}

static bool rvResistLine(uintptr_t base, uintptr_t actor, bool isMonster, char* out, int outsz) {
    out[0] = 0;
    int got = 0, hidden = 0;
    for (int i = 0; i < CS_RESIST_COUNT; i++) {
        if (!csResistVisibleFor(base, isMonster, i)) { hidden++; continue; }
        char row[128];
        if (!csResistRowFor(base, actor, i, row, sizeof row)) {
            logLine("roomview resist: row %d unreadable for actor=%p", i, (void*)actor);
            continue;
        }
        if (got++) strncat(out, " ", outsz - strlen(out) - 1);
        strncat(out, row, outsz - strlen(out) - 1);
        strncat(out, ".", outsz - strlen(out) - 1);
    }
    logLine("roomview resist: actor=%p showed %d, gated out %d", (void*)actor, got, hidden);
    return got > 0;
}

// ---- the ENEMY identity line — "Bone Soldier, Unholy, enemy position 2." ----
static bool rvMonsterTitleLine(uintptr_t base, const RvEntry& e, char* out, int outsz) {
    out[0] = 0;
    char name[160], types[192];
    if (!rvMonsterName(base, e.actor, name, sizeof name)) name[0] = 0;
    if (!rvMonsterTypes(base, e.actor, types, sizeof types)) types[0] = 0;

    char pos[64];
    pos[0] = 0;
    if (e.slot >= 1) {
        rvPosPhrase(e.slot, e.slotEnd, e.enemy, pos, sizeof pos);
        if (pos[0] >= 'A' && pos[0] <= 'Z') pos[0] = (char)(pos[0] - 'A' + 'a');
    }

    const char* who  = name[0] ? name : types;
    const char* rest = (name[0] && types[0]) ? types : NULL;
    if (!who[0] && !pos[0]) return false;
    if (rest && pos[0])        _snprintf(out, outsz, "%s, %s, %s.", who, rest, pos);
    else if (rest)             _snprintf(out, outsz, "%s, %s.", who, rest);
    else if (who[0] && pos[0]) _snprintf(out, outsz, "%s, %s.", who, pos);
    else                       _snprintf(out, outsz, "%s.", who[0] ? who : pos);
    out[outsz - 1] = 0;
    return out[0] != 0;
}

static bool rvMonsterStatLine(uintptr_t base, uintptr_t actor, uintptr_t off, const char* key,
                              float scale, char* out, int outsz) {
    if (!rvTipStat(base, actor, off, key, scale, out, outsz) || !out[0]) return false;
    int row = csStatRowIndexForOff(off);
    if (row >= 0) csStatTipDecorate(base, actor, row, out, outsz);
    else logLine("roomview tip: no sheet row for block +0x%llx", (unsigned long long)off);
    return out[0] != 0;
}

// ---- THE ENEMY TOOLTIP, hero format ----
static int rvMonsterTipLines(uintptr_t base, const RvEntry& e,
                             char lines[][AB_TIP_LINE_SZ], int maxLines) {
    uintptr_t mon = e.actor;
    if (!mon) return 0;
    spDumpStatusOnce(base, mon);

    int n = 0;
    char buf[AB_TIP_LINE_SZ], frag[AB_TIP_LINE_SZ];

    // 0 — identity.
    if (n < maxLines && rvMonsterTitleLine(base, e, buf, sizeof buf)) {
        _snprintf(lines[n], AB_TIP_LINE_SZ, "%s", buf);
        lines[n][AB_TIP_LINE_SZ - 1] = 0; n++;
    }

    if (n < maxLines) {
        buf[0] = 0;
        if (rvMonsterHpFrag(base, mon, frag, sizeof frag)) rvAppendComma(buf, sizeof buf, frag);
        spAddBucket(base, mon, SP_COND_HEALTH, buf, sizeof buf);
        spEndSentence(buf, sizeof buf);
        if (spIncomingModsFrag(base, mon, "hp_heal_received_percent", frag, sizeof frag))
            abAppendFrag(buf, sizeof buf, frag);
        const char* line = buf;
        while (*line == ' ') line++;
        if (*line) { _snprintf(lines[n], AB_TIP_LINE_SZ, "%s", line);
                     lines[n][AB_TIP_LINE_SZ - 1] = 0; n++; }
    }

    n = spConditionLines(base, mon, SP_COND_BLEED | SP_COND_BLIGHT | SP_COND_HORROR,
                         lines, n, maxLines);
    n = spConditionLines(base, mon, SP_COND_NEG | SP_COND_POS, lines, n, maxLines);
    n = spUnvoicedBuffLines(base, mon, true, lines, n, maxLines);

    static const struct { uintptr_t off; const char* key; float scale; } kRows[] = {
        { CS_STAT_PROT_OFF, MT_PROT_KEY,  CS_PERCENT_SCALE },
        { CS_STAT_DEF_OFF,  MT_DODGE_KEY, CS_PERCENT_SCALE },
        { CS_STAT_SPD_OFF,  MT_SPEED_KEY, 1.0f             },
    };
    for (int i = 0; i < (int)(sizeof kRows / sizeof kRows[0]) && n < maxLines; i++) {
        if (!rvMonsterStatLine(base, mon, kRows[i].off, kRows[i].key, kRows[i].scale,
                               buf, sizeof buf)) {
            logLine("roomview tip: \"%s\" unreadable for actor=%p", kRows[i].key, (void*)mon);
            continue;
        }
        _snprintf(lines[n], AB_TIP_LINE_SZ, "%s", buf);
        lines[n][AB_TIP_LINE_SZ - 1] = 0; n++;
    }

    if (n < maxLines) {
        _snprintf(lines[n], AB_TIP_LINE_SZ, "%s", axs(AXS_RV_RESIST_HINT));
        lines[n][AB_TIP_LINE_SZ - 1] = 0; n++;
    }
    if (n < maxLines) {
        _snprintf(lines[n], AB_TIP_LINE_SZ, "%s",
                  axs(rvArenaContestant(base, mon) ? AXS_RV_LOADOUT_HINT : AXS_RV_SKILLS_HINT));
        lines[n][AB_TIP_LINE_SZ - 1] = 0; n++;
    }
    return n;
}

// ---- The enemy's RESISTANCE PANEL — Ctrl+Right, the hero panel's shape ----
static int rvMonsterResistPanelLines(uintptr_t base, const RvEntry& e,
                                     char lines[][AB_TIP_LINE_SZ], int maxLines) {
    if (!e.actor) return 0;
    int n = 0;
    if (n < maxLines && rvMonsterTitleLine(base, e, lines[n], AB_TIP_LINE_SZ) && lines[n][0]) n++;
    int body = 0;
    int resists = csResistCount(base);
    for (int i = 0; i < resists && n < maxLines; i++) {
        if (!csResistVisibleFor(base, true, i)) continue;
        if (!csResistTipLineFor(base, e.actor, i, lines[n], AB_TIP_LINE_SZ) || !lines[n][0]) {
            logLine("roomview resist: tip row %d unreadable for actor=%p", i, (void*)e.actor);
            continue;
        }
        n++; body++;
    }
    return body > 0 ? n : 0;
}

// ---- A CIRCUS OPPONENT'S LOADOUT PANEL — Ctrl+Left inside a match ----
static bool rvArenaSkillName(uintptr_t base, uintptr_t mon, uintptr_t skill,
                             char* out, int outsz) {
    out[0] = 0;
    char id[0x41];
    if (!safeReadCStr(skill + MONSTER_SKILL_ID_OFF, id, sizeof id) || !id[0]) return false;
    for (const char* p = id; *p; p++)                       // loaded data: the identifier guard
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
              (*p >= '0' && *p <= '9') || *p == '_')) {
            logLine("roomview loadout: skill id \"%s\" is not an identifier", id);
            return false;
        }
    char cls[64] = { 0 };
    uintptr_t clsObj = abActorClass(mon);
    if (clsObj) safeReadCStr(clsObj + ACTOR_CLASS_VID_OFF, cls, sizeof cls);
    char key[192], name[160];
    bool ok = false;
    if (abPlausibleName(cls)) {
        _snprintf(key, sizeof key, SKILL_NAME_FMT, cls, id);
        key[sizeof key - 1] = 0;
        ok = resolveKey(base, key, name, sizeof name) && name[0];
    }
    if (!ok) {
        _snprintf(key, sizeof key, MT_SKILL_KEY_FMT, id);
        key[sizeof key - 1] = 0;
        ok = resolveKey(base, key, name, sizeof name) && name[0];
        logLine("roomview loadout: hero key missed for class=\"%s\" skill=\"%s\"; monster key %s",
                cls, id, ok ? "resolved" : "missed too, speaking the id");
    }
    if (!ok) _snprintf(name, sizeof name, "%s", id);
    name[sizeof name - 1] = 0;
    abStripMarkup(name);
    _snprintf(out, outsz, "%s", name);
    out[outsz - 1] = 0;
    return out[0] != 0;
}

static int rvArenaLoadoutLines(uintptr_t base, const RvEntry& e,
                               char lines[][AB_TIP_LINE_SZ], int maxLines) {
    uintptr_t mon = e.actor;
    if (!mon) return 0;
    int n = 0;
    if (n < maxLines && rvMonsterTitleLine(base, e, lines[n], AB_TIP_LINE_SZ) && lines[n][0]) n++;
    int body = 0;

    for (int t = 0; t < TRINKET_SLOT_COUNT && n < maxLines; t++) {
        if (!csItemsPanelRow(base, mon, 2 + t, lines[n], AB_TIP_LINE_SZ) || !lines[n][0]) {
            logLine("roomview loadout: trinket row %d unreadable for contestant=%p", t, (void*)mon);
            continue;
        }
        n++; body++;
    }

    uintptr_t beg = 0;
    int ns = rvSkillCount(mon, &beg);
    uintptr_t picked[RV_TIP_SKILL_MAX];
    int np = 0;
    for (int i = 0; i < ns && np < RV_TIP_SKILL_MAX && beg; i++) {
        uintptr_t skill = beg + (uintptr_t)i * MONSTER_SKILL_STRIDE;
        char id[0x41] = { 0 };
        uint8_t shown = 0;
        safeReadCStr(skill + MONSTER_SKILL_ID_OFF, id, sizeof id);
        safeReadU8(skill + MONSTER_SKILL_SHOWN_OFF, &shown);
        logLine("roomview loadout: skill[%d] id=\"%s\" drawbyte=%u", i,
                abPlausibleName(id) ? id : "(implausible)", shown);
        if (strcmp(id, "move") == 0) continue;
        picked[np++] = skill;
    }
    if (np > 0) {
        char head[96];
        head[0] = 0;
        if (resolveKey(base, MT_SKILLS_KEY, head, sizeof head) && head[0]) {
            abStripMarkup(head);
            csTrimLabel(head);
        }
        if (!head[0]) {
            logLine("roomview loadout: \"%s\" did not resolve — AXS fallback", MT_SKILLS_KEY);
            _snprintf(head, sizeof head, "%s", axs(AXS_RV_SKILLS_PANEL));
            head[sizeof head - 1] = 0;
        }
        if (n < maxLines) {
            _snprintf(lines[n], AB_TIP_LINE_SZ, axs(AXS_RV_SKILLS_COUNT_FMT), head, np);
            lines[n][AB_TIP_LINE_SZ - 1] = 0; n++;
        }
        for (int i = 0; i < np && n < maxLines; i++) {
            if (!rvArenaSkillName(base, mon, picked[i], lines[n], AB_TIP_LINE_SZ) || !lines[n][0])
                continue;
                                                // says a slot existed, the campaign panel's rule
            spEndSentence(lines[n], AB_TIP_LINE_SZ);
            n++; body++;
        }
    } else {
        logLine("roomview loadout: no skills on contestant=%p (vector %d entries)", (void*)mon, ns);
    }

    if (body <= 0) return 0;
    if (n < maxLines) {
        _snprintf(lines[n], AB_TIP_LINE_SZ, "%s", axs(AXS_RV_ITEMS_BACK_HINT));
        lines[n][AB_TIP_LINE_SZ - 1] = 0; n++;
    }
    return n;
}

// ---- The enemy's SKILLS PANEL — Ctrl+Left, where a hero keeps items ----
static int rvMonsterSkillPanelLines(uintptr_t base, const RvEntry& e,
                                    char lines[][AB_TIP_LINE_SZ], int maxLines) {
    if (!e.actor) return 0;
    if (rvArenaContestant(base, e.actor)) return rvArenaLoadoutLines(base, e, lines, maxLines);
    RvSkillSlot slots[RV_TIP_SKILL_MAX];
    int ns = rvSkillSlotList(base, e.actor, slots, RV_TIP_SKILL_MAX);
    if (ns <= 0) return 0;
                                              // says "No more information."
    int n = 0;
    if (n < maxLines && rvMonsterTitleLine(base, e, lines[n], AB_TIP_LINE_SZ) && lines[n][0]) n++;

    char head[96];
    if (resolveKey(base, MT_SKILLS_KEY, head, sizeof head) && head[0]) {
        abStripMarkup(head);
        csTrimLabel(head);
    }
    if (!head[0]) {
        logLine("roomview skills: \"%s\" did not resolve — AXS fallback", MT_SKILLS_KEY);
        _snprintf(head, sizeof head, "%s", axs(AXS_RV_SKILLS_PANEL));
        head[sizeof head - 1] = 0;
    }
    if (n < maxLines) {
        _snprintf(lines[n], AB_TIP_LINE_SZ, axs(AXS_RV_SKILLS_COUNT_FMT), head, ns);
        lines[n][AB_TIP_LINE_SZ - 1] = 0; n++;
    }

    for (int i = 0; i < ns && n < maxLines; i++) {
        if (slots[i].seen && slots[i].name[0]) {
            _snprintf(lines[n], AB_TIP_LINE_SZ, "%s", slots[i].name);
            lines[n][AB_TIP_LINE_SZ - 1] = 0;
            spEndSentence(lines[n], AB_TIP_LINE_SZ);
        } else {
            _snprintf(lines[n], AB_TIP_LINE_SZ, "%s", axs(AXS_RV_SKILL_UNKNOWN));
            lines[n][AB_TIP_LINE_SZ - 1] = 0;
        }
        n++;
    }

    if (n < maxLines) {
        _snprintf(lines[n], AB_TIP_LINE_SZ, "%s", axs(AXS_RV_ITEMS_BACK_HINT));
        lines[n][AB_TIP_LINE_SZ - 1] = 0; n++;
    }
    return n;
}

static int rvBuildLines(uintptr_t base, const RvEntry& e,
                        char lines[RV_TIP_MAX_LINES][AB_TIP_LINE_SZ]) {
    char deadTip[96];
    bool wantsMonster = rvWantsMonsterRow(base, e);
    if (e.kind == RV_ACTOR && e.actor && !wantsMonster &&
        rvCorpseWord(base, e.actor, deadTip, sizeof deadTip)) {
        lines[0][0] = 0;
        if (!rvRowText(base, e, lines[0], AB_TIP_LINE_SZ)) {
            char pos[64];
            rvPosForEntry(e, pos, sizeof pos);
            _snprintf(lines[0], AB_TIP_LINE_SZ, "%s. %s.", deadTip, pos);
            lines[0][AB_TIP_LINE_SZ - 1] = 0;
        }
        return 1;
    }

    if (e.kind == RV_ACTOR && !wantsMonster) {
        int n = spHeroTipLines(base, e.actor, e.slot, e.slotEnd, true, e.enemy,
                               lines, 0, RV_TIP_MAX_LINES);
        if (n == 0) {
            logLine("roomview status: nothing readable for hero=%p", (void*)e.actor);
            if (!rvRowText(base, e, lines[0], AB_TIP_LINE_SZ)) {
                char pos[64];
                rvPosForEntry(e, pos, sizeof pos);
                _snprintf(lines[0], AB_TIP_LINE_SZ, "%s. %s.", rvGenericLabel(e), pos);
                lines[0][AB_TIP_LINE_SZ - 1] = 0;
            }
            n = 1;
        }
        return n;
    }

    lines[0][0] = 0;
    if (!rvRowText(base, e, lines[0], AB_TIP_LINE_SZ)) {
        char pos[64];
        rvPosForEntry(e, pos, sizeof pos);
        _snprintf(lines[0], AB_TIP_LINE_SZ, "%s. %s.", rvGenericLabel(e), pos);
        lines[0][AB_TIP_LINE_SZ - 1] = 0;
    }
    int n = 1;

    if (e.kind == RV_DOOR || e.kind == RV_ADVANCE) return n;
    if (e.kind == RV_PROP) {
        char name[160], desc[512];
        if (propTextFor(base, e.obj, name, sizeof name, desc, sizeof desc) && desc[0] &&
            n < RV_TIP_MAX_LINES) {
            _snprintf(lines[n], AB_TIP_LINE_SZ, "%s", desc);
            lines[n][AB_TIP_LINE_SZ - 1] = 0; n++;
        } else {
            logLine("roomview prop: no description for prop=%p", (void*)e.obj);
        }
        return n;
    }
    {
        int mn = rvMonsterTipLines(base, e, lines, RV_TIP_MAX_LINES);
        if (mn > 0) n = mn;
    }
    return n;
}

// ---- THE ARENA MIRROR PROBE ----
static const uintptr_t RV_PROBE_HEX_BASE = 0x1200;   // covers +0x12b0 class, +0x128c stress, +0x13f8
static const int       RV_PROBE_HEX_LEN  = 0x220;

static void rvProbeClass(uintptr_t base, const char* who, int i, const char* which,
                         uintptr_t actor, uintptr_t off) {
    uintptr_t cls = 0;
    if (!safeReadPtr(actor + off, &cls) || cls <= 0x10000) {
        logDump("arena %s[%d] %s (+0x%llx): no pointer (raw %p)",
                who, i, which, (unsigned long long)off, (void*)cls);
        return;
    }
    uintptr_t vft = 0;
    char rtti[192]; rtti[0] = 0;
    if (safeReadPtr(cls, &vft) && vft > 0x10000) tmbClassName(base, vft, rtti, sizeof rtti);
    char id[80] = {0}, id48[80] = {0}, disp[80] = {0};
    uint8_t latch = 0;
    safeReadCStr(cls + HEROCLASS_ID_OFF, id, sizeof id);
    safeReadCStr(cls + ACTOR_CLASS_VID_OFF, id48, sizeof id48);
    safeReadU8(cls + HEROCLASS_DISP_LATCH_OFF, &latch);
    safeReadCStr(cls + HEROCLASS_DISP_OFF, disp, sizeof disp);
    logDump("arena %s[%d] %s (+0x%llx) = %p vft=+0x%llx rtti=\"%s\" id@08=\"%s\" id@48=\"%s\" "
            "latch=%u disp=\"%s\"",
            who, i, which, (unsigned long long)off, (void*)cls,
            vft > base ? (unsigned long long)(vft - base) : 0ull,
            rtti[0] ? rtti : "(none)", abPlausibleName(id) ? id : "(implausible)",
            abPlausibleName(id48) ? id48 : "(implausible)",
            latch, abPlausibleName(disp) ? disp : "(implausible)");

    struct { uintptr_t beg, end; const char* name; uintptr_t stride; } vecs[] = {
        { cls + MONSTERCLASS_SKILLVEC_OFF, cls + MONSTERCLASS_SKILLVEC_END_OFF,
          "monster+0xba8", MONSTER_SKILL_STRIDE },
        { cls + HEROCLASS_SKILLVEC_OFF,    cls + HEROCLASS_SKILLVEC_END_OFF,
          "hero+0xbd0",    0x18 },
    };
    for (int v = 0; v < 2; v++) {
        uintptr_t b = 0, e = 0;
        if (!safeReadPtr(vecs[v].beg, &b) || !safeReadPtr(vecs[v].end, &e)) {
            logDump("arena %s[%d]   %s: unreadable", who, i, vecs[v].name);
            continue;
        }
        long long span = (b && e >= b) ? (long long)(e - b) : -1;
        logDump("arena %s[%d]   %s: beg=%p end=%p span=%lld -> %lld entries at stride 0x%llx",
                who, i, vecs[v].name, (void*)b, (void*)e, span,
                span >= 0 ? span / (long long)vecs[v].stride : -1,
                (unsigned long long)vecs[v].stride);
        if (v == 0 && span > 0 && b > 0x10000) {
            for (int s = 0; s < 2 && (long long)((uintptr_t)s * MONSTER_SKILL_STRIDE) < span; s++) {
                char sid[0x41] = {0};
                safeReadCStr(b + (uintptr_t)s * MONSTER_SKILL_STRIDE + MONSTER_SKILL_ID_OFF,
                             sid, sizeof sid);
                logDump("arena %s[%d]     skill[%d] id=\"%s\"", who, i, s,
                        abPlausibleName(sid) ? sid : "(implausible)");
            }
        }
    }
}

static void rvProbeActor(uintptr_t base, const char* who, int i, uintptr_t actor) {
    uintptr_t vft = 0;
    char rtti[192]; rtti[0] = 0;
    bool haveVft = safeReadPtr(actor, &vft) && vft > 0x10000;
    if (haveVft) tmbClassName(base, vft, rtti, sizeof rtti);
    logDump("arena %s[%d] actor=%p vft=+0x%llx rtti=\"%s\" | spActorIsHero=%d "
            "(HERO_VFT=+0x%llx MONSTER_VFT=+0x%llx)",
            who, i, (void*)actor,
            haveVft && vft > base ? (unsigned long long)(vft - base) : 0ull,
            rtti[0] ? rtti : "(none)", spActorIsHero(base, actor) ? 1 : 0,
            (unsigned long long)HERO_VFT, (unsigned long long)MONSTER_VFT);

    char pname[80] = {0};
    bool nameOk = safeReadCStr(actor + HERO_NAME_OFF, pname, sizeof pname) && abPlausibleName(pname);
    float stress = -1.0f, hp = -1.0f;
    bool stressOk = abReadF32(actor + ACTOR_STRESS_OFF, &stress);
    bool hpOk     = abReadF32(actor + ACTOR_CUR_HP_OFF, &hp);
    logDump("arena %s[%d]   name(+0x08)=\"%s\" plausible=%d | stress(+0x%llx)=%.3f read=%d "
            "inrange=%d | hp(+0x%llx)=%.3f read=%d inrange=%d",
            who, i, pname, nameOk ? 1 : 0,
            (unsigned long long)ACTOR_STRESS_OFF, stress, stressOk ? 1 : 0,
            (stressOk && stress >= 0.0f && stress <= ACTOR_MAX_STRESS) ? 1 : 0,
            (unsigned long long)ACTOR_CUR_HP_OFF, hp, hpOk ? 1 : 0,
            (hpOk && hp >= -1.0f && hp <= 1000.0f) ? 1 : 0);

    rvProbeClass(base, who, i, "heroclass", actor, ACTOR_HEROCLASS_OFF);
    rvProbeClass(base, who, i, "monsterclass", actor, MONSTER_CLASS_OFF);

    uintptr_t vcls = abActorClass(actor);
    char vrtti[192]; vrtti[0] = 0;
    uintptr_t vclsVft = 0;
    if (vcls && safeReadPtr(vcls, &vclsVft) && vclsVft > 0x10000)
        tmbClassName(base, vclsVft, vrtti, sizeof vrtti);
    char vid8[80] = {0}, vid48[80] = {0};
    if (vcls) { safeReadCStr(vcls + HEROCLASS_ID_OFF, vid8, sizeof vid8);
                safeReadCStr(vcls + ACTOR_CLASS_VID_OFF, vid48, sizeof vid48); }
    logDump("arena %s[%d]   VCALL class(+0x30) = %p vft=+0x%llx rtti=\"%s\" id@08=\"%s\" id@48=\"%s\"",
            who, i, (void*)vcls,
            vclsVft > base ? (unsigned long long)(vclsVft - base) : 0ull,
            vrtti[0] ? vrtti : "(none)",
            abPlausibleName(vid8) ? vid8 : "(implausible)",
            abPlausibleName(vid48) ? vid48 : "(implausible)");
    for (int o = 0; vcls && o < 0xE0; o += 32) {
        char line[128]; int w = 0;
        for (int k = 0; k < 32; k++) {
            uint8_t v = 0;
            if (!safeReadU8(vcls + (uintptr_t)(o + k), &v)) v = 0;
            w += _snprintf(line + w, sizeof line - w, "%02x", v);
        }
        logDump("arena %s[%d]   class+%04x: %s", who, i, o, line);
    }

    unsigned char buf[RV_PROBE_HEX_LEN];
    for (int o = 0; o < RV_PROBE_HEX_LEN; o += 32) {
        char line[128]; int w = 0;
        for (int k = 0; k < 32 && o + k < RV_PROBE_HEX_LEN; k++) {
            uint8_t v = 0;
            if (!safeReadU8(actor + RV_PROBE_HEX_BASE + (uintptr_t)(o + k), &v)) v = 0;
            buf[o + k] = v;
            w += _snprintf(line + w, sizeof line - w, "%02x", v);
        }
        logDump("arena %s[%d]   +%04llx: %s", who, i,
                (unsigned long long)(RV_PROBE_HEX_BASE + o), line);
    }
}

static bool g_rvArenaProbed = false;

static void rvProbeArena(uintptr_t base) {
    uintptr_t party[RV_MAX_MEMBERS], foes[RV_MAX_ENEMIES];
    int np = rvPartyList(base, party, RV_MAX_MEMBERS);
    int nf = rvEnemyList(base, foes, RV_MAX_ENEMIES);
    if (np <= 0 && nf <= 0) return;                 // nothing to measure yet; stay armed
    logDump("=== ARENA MIRROR PROBE: party=%d far=%d ===", np, nf);
    for (int i = 0; i < np; i++) rvProbeActor(base, "party", i, party[i]);
    for (int i = 0; i < nf; i++) rvProbeActor(base, "far",   i, foes[i]);
    logDump("=== ARENA MIRROR PROBE end ===");
    g_rvArenaProbed = true;
}

static void rvDumpRoom(uintptr_t base, const uintptr_t* party, int n) {
    char owner[0x44];
    if (kbActionForKey(base, SDLK_r, owner, sizeof owner))
        logLine("roomview GATE: 'r' is BOUND to the game action \"%s\" — claiming it takes it away", owner);
    else
        logLine("roomview GATE: 'r' is not in the keyboard binding table — safe to claim");

    uintptr_t sel = abSelectedHero(base);
    logLine("roomview DUMP party=%d selected=%p", n, (void*)sel);
    for (int i = 0; i < n; i++) {
        char name[80], cls[80];
        name[0] = cls[0] = 0;
        abHeroNameClassOf(base, party[i], name, sizeof name, cls, sizeof cls);
        logLine("rv-party[%d] hero=%p name=\"%s\" class=\"%s\"%s",
                i, (void*)party[i], name, cls, party[i] == sel ? "  <- SELECTED" : "");
    }

    uintptr_t foes[RV_MAX_ENEMIES];
    int ne = rvEnemyList(base, foes, RV_MAX_ENEMIES);
    logLine("roomview DUMP enemies=%d", ne);
    for (int i = 0; i < ne; i++) {
        char cls[80];
        uintptr_t cobj = 0;
        cls[0] = 0;
        safeReadPtr(foes[i] + MONSTER_CLASS_OFF, &cobj);
        rvMonsterName(base, foes[i], cls, sizeof cls);

        char types[192];
        if (!rvMonsterTypes(base, foes[i], types, sizeof types)) strcpy(types, "(none)");
        logLine("rv-enemy[%d] monster=%p class=%p name=\"%s\" types=\"%s\"",
                i, (void*)foes[i], (void*)cobj, cls, types);

        char tip[AB_TIP_LINE_SZ];
        if (rvStatsLine(base, foes[i], tip, sizeof tip)) logLine("rv-enemy[%d] stats -> \"%s\"", i, tip);
        else                                             logLine("rv-enemy[%d] stats UNREADABLE", i);
        if (rvResistLine(base, foes[i], true, tip, sizeof tip)) logLine("rv-enemy[%d] resist -> \"%s\"", i, tip);
        uintptr_t sbeg = 0;
        int ns = rvSkillCount(foes[i], &sbeg);
        logLine("rv-enemy[%d] skills=%d", i, ns);
        for (int s = 0; s < ns; s++) {
            uintptr_t skill = sbeg + (uintptr_t)s * MONSTER_SKILL_STRIDE;
            uint8_t shownB = 0;
            safeReadU8(skill + MONSTER_SKILL_SHOWN_OFF, &shownB);
            char id[0x41] = { 0 };
            safeReadCStr(skill + MONSTER_SKILL_ID_OFF, id, sizeof id);
            bool slot = rvSkillSlotShown(foes[i], skill);
            bool seen = id[0] ? rvSkillSeen(base, id) : false;
            if (rvSkillName(base, foes[i], s, tip, sizeof tip))
                 logLine("rv-enemy[%d] skill[%d] id=\"%s\" shownbyte=%u slot=%d seen=%d -> \"%s\"",
                         i, s, id, shownB, slot ? 1 : 0, seen ? 1 : 0, tip);
            else logLine("rv-enemy[%d] skill[%d] id=\"%s\" shownbyte=%u slot=%d seen=%d UNREADABLE",
                         i, s, id, shownB, slot ? 1 : 0, seen ? 1 : 0);
        }
    }

    int shown = 0, hid = 0;
    for (int i = 0; i < CS_RESIST_COUNT; i++) csResistVisibleFor(base, true, i) ? shown++ : hid++;
    logLine("rv-resist FLAGS: %d shown, %d hidden for monsters (expect 5 / 3)", shown, hid);
}

// ---- the stage 3 one-shot dump ----
static void rvDumpPropHex(uintptr_t propType) {
    for (int off = 0; off < 0xc0; off += 0x10) {
        unsigned char b[0x10];
        char hex[64], asc[24];
        int ho = 0;
        for (int i = 0; i < 0x10; i++) {
            if (!safeReadU8(propType + off + i, &b[i])) { b[i] = 0; }
            ho += _snprintf(hex + ho, sizeof hex - ho, "%02x ", b[i]);
            asc[i] = (b[i] >= 32 && b[i] < 127) ? (char)b[i] : '.';
        }
        asc[0x10] = 0;
        logLine("prop-dump +0x%02x: %s |%s|", off, hex, asc);
    }
}

static void rvDumpProps(uintptr_t base) {
    uintptr_t rd = (uintptr_t)g_raidDisplay;
    uintptr_t dv = 0;
    if (rd) safeReadPtr(rd + RD_DUNGEONVIEW_OFF, &dv);
    logLine("roomview DUMP stage3: RaidDisplay=%p Panel_DungeonView=%p", (void*)rd, (void*)dv);

    uintptr_t props[RV_MAX_PROPS];
    int npr = rvPropList(base, props, RV_MAX_PROPS);
    logLine("roomview DUMP props=%d", npr);
    for (int i = 0; i < npr; i++) {
        uintptr_t pt = 0;
        safeReadPtr(props[i] + PROP_TYPE_OFF, &pt);
        char id[80], name[160], desc[512];
        id[0] = 0;
        bool gotId = propIdOf(props[i], id, sizeof id);
        const char* ui = gotId ? propUiNameFor(id) : nullptr;
        if (!propTextFor(base, props[i], name, sizeof name, desc, sizeof desc)) { name[0] = desc[0] = 0; }
        logLine("rv-prop[%d] prop=%p type=%p id=\"%s\" ui=\"%s\" name=\"%s\"",
                i, (void*)props[i], (void*)pt, id, ui ? ui : "(not in csv)", name);
        logLine("rv-prop[%d] desc=\"%s\"", i, desc);
        if (i == 0 && pt > 0x10000) rvDumpPropHex(pt);
    }

    uint32_t dests[RV_MAX_DOORS];
    uintptr_t doors[RV_MAX_DOORS];
    int nd = rvDoorList(base, dests, doors, RV_MAX_DOORS);
    logLine("roomview DUMP doors=%d (rows suppressed: wave=%d)", nd, comIsWaveRaid(base) ? 1 : 0);
    for (int i = 0; i < nd; i++) {
        char row[160];
        rvDoorRowText(base, dests[i], i + 1, nd, row, sizeof row);
        logLine("rv-door[%d] door=%p dest='%.4s' -> \"%s\"",
                i, (void*)doors[i], (const char*)&dests[i], row);
    }
}

static void rvSpeakRow(uintptr_t base, const RvEntry* rows, int n, int cur, const char* head = nullptr) {
    if (cur < 0 || cur >= n) return;
    g_rvTipCol = 0;
    char out[512];
    if (!rvRowText(base, rows[cur], out, sizeof out)) {
        char pos[64];
        rvPosForEntry(rows[cur], pos, sizeof pos);
        _snprintf(out, sizeof out, "%s. %s.", rvGenericLabel(rows[cur]), pos);
        out[sizeof out - 1] = 0;
    }
    static const char* const kKindWord[] = { "actor", "prop", "door", "?" };  // 4th = out-of-range guard
    logLine("roomview row %d/%d (%s %s idx=%d slot=%d) -> \"%s\"",
            cur, n, kKindWord[rows[cur].kind & 3], rows[cur].enemy ? "enemy" : "party",
            rows[cur].idx, rows[cur].slot, out);
    if (head && head[0]) {
        char full[768];
        _snprintf(full, sizeof full, "%s %s", head, out);
        full[sizeof full - 1] = 0;
        postSpeech(full);
        return;
    }
    postSpeech(out);
}

void rvSetActive(uintptr_t base, bool on) {
    (void)base;
    g_rvActive = on;
    g_rvCursor = on ? 0 : -1;
    g_rvTipLine = 0;
    g_rvTipCol  = 0;
    logLine("roomview %s", on ? "active" : "inactive");
}

void rvSpeakTipLine(uintptr_t base, int tipDir) {
    RvEntry rows[RV_MAX_ROWS];
    int n = rvBuildRoom(base, rows, RV_MAX_ROWS);
    if (n <= 0 || g_rvCursor < 0) { postSpeech(axs(AXS_NO_ROW_SELECTED)); return; }
    if (g_rvCursor >= n) g_rvCursor = n - 1;

    if (g_rvTipCol != 0) {
        const RvEntry& e = rows[g_rvCursor];
        char rl[RV_TIP_MAX_LINES][AB_TIP_LINE_SZ];
        int nr = 0;
        if (e.kind == RV_ACTOR && e.actor) {
            if (rvWantsMonsterRow(base, e))
                nr = (g_rvTipCol == 1)
                         ? rvMonsterResistPanelLines(base, e, rl, RV_TIP_MAX_LINES)
                         : rvMonsterSkillPanelLines(base, e, rl, RV_TIP_MAX_LINES);
            else
                nr = (g_rvTipCol == 1)
                         ? spHeroResistLines(base, e.actor, e.slot, e.slotEnd, e.enemy,
                                             rl, RV_TIP_MAX_LINES)
                         : spHeroItemLines(base, e.actor, e.slot, e.slotEnd, e.enemy,
                                           rl, RV_TIP_MAX_LINES);
        }
        if (nr > 0) {
            int line = (g_rvTipLine + tipDir + nr) % nr;   // wraps, like the main buffer
            g_rvTipLine = line;
            logLine("roomview %s tipline %d/%d (row %d) -> \"%s\"",
                    g_rvTipCol == 1 ? "resist" : "left-panel", line, nr, g_rvCursor, rl[line]);
            postSpeech(rl[line]);
            return;
        }
        g_rvTipCol = 0;
    }

    char lines[RV_TIP_MAX_LINES][AB_TIP_LINE_SZ];
    int nl = rvBuildLines(base, rows[g_rvCursor], lines);
    if (nl <= 1) { postSpeech(axs(AXS_NO_MORE_INFO)); return; }

    int line = (g_rvTipLine + tipDir + nl) % nl;   // wraps, so neither direction dead-ends
    g_rvTipLine = line;
    logLine("roomview tipline %d/%d (row %d) -> \"%s\"", line, nl, g_rvCursor, lines[line]);
    postSpeech(lines[line]);
}

bool rvTipPanelSwitch(uintptr_t base, int dir, bool repeat) {
    RvEntry rows[RV_MAX_ROWS];
    int n = rvBuildRoom(base, rows, RV_MAX_ROWS);
    if (n <= 0 || g_rvCursor < 0) return false;
    if (g_rvCursor >= n) g_rvCursor = n - 1;
    const RvEntry& e = rows[g_rvCursor];
    char deadWord[96];
    if (e.kind != RV_ACTOR || !e.actor) return false;
    bool monster = rvWantsMonsterRow(base, e);
    bool arena   = monster && rvArenaContestant(base, e.actor);   // the left panel is the LOADOUT
    if (!monster && rvCorpseWord(base, e.actor, deadWord, sizeof deadWord)) return false;
    if (repeat) return true;                    // claimed key: one switch per press

    int want = g_rvTipCol + dir;
    if (want < -1) want = -1;                   // hard stops, like every mod buffer
    if (want > 1)  want = 1;
    g_rvTipCol  = want;
    g_rvTipLine = 0;
    if (want != 0) {
        char rl[RV_TIP_MAX_LINES][AB_TIP_LINE_SZ];
        int nr = 0;
        if (monster)
            nr = (want == 1) ? rvMonsterResistPanelLines(base, e, rl, RV_TIP_MAX_LINES)
                             : rvMonsterSkillPanelLines(base, e, rl, RV_TIP_MAX_LINES);
        else
            nr = (want == 1) ? spHeroResistLines(base, e.actor, e.slot, e.slotEnd, e.enemy,
                                                 rl, RV_TIP_MAX_LINES)
                             : spHeroItemLines(base, e.actor, e.slot, e.slotEnd, e.enemy,
                                               rl, RV_TIP_MAX_LINES);
        if (nr <= 0) {
            g_rvTipCol = 0;
            postSpeech(axs(AXS_NO_MORE_INFO));
            return true;
        }
        char head[128];
        head[0] = 0;
        if (want == 1) {
            if (monster) {
                if (!resolveKey(base, MT_RESIST_KEY, head, sizeof head) || !head[0]) {
                    logLine("roomview tip: \"%s\" did not resolve — AXS fallback", MT_RESIST_KEY);
                    _snprintf(head, sizeof head, "%s", axs(AXS_RV_RESIST_PANEL));
                }
            } else spResistPanelHead(base, head, sizeof head);
        } else {
            if (monster && arena) {
                _snprintf(head, sizeof head, "%s", axs(AXS_RV_LOADOUT_PANEL));
            } else if (monster) {
                if (!resolveKey(base, MT_SKILLS_KEY, head, sizeof head) || !head[0]) {
                    logLine("roomview tip: \"%s\" did not resolve — AXS fallback", MT_SKILLS_KEY);
                    _snprintf(head, sizeof head, "%s", axs(AXS_RV_SKILLS_PANEL));
                }
            } else _snprintf(head, sizeof head, "%s", axs(AXS_RV_ITEMS_PANEL));
        }
        head[sizeof head - 1] = 0;
        abStripMarkup(head);
        csTrimLabel(head);
        char full[AB_TIP_LINE_SZ + 140];
        _snprintf(full, sizeof full, "%s. %s", head, rl[0]);
        full[sizeof full - 1] = 0;
        logLine("roomview tip panel -> %s%s (%d lines)", monster ? "enemy " : "",
                want == 1 ? "resistances" : (monster ? (arena ? "loadout" : "skills") : "items"), nr);
        postSpeech(full);
    } else {
        char lines[RV_TIP_MAX_LINES][AB_TIP_LINE_SZ];
        int nl = rvBuildLines(base, e, lines);
        logLine("roomview tip panel -> main");
        if (nl > 0 && lines[0][0]) postSpeech(lines[0]);
        else                       postSpeech(axs(AXS_NO_MORE_INFO));
    }
    return true;
}

// ---- Entering the dungeon view: ALWAYS header + row ----
void rvAnnounceEntry(uintptr_t base, bool withCounts) {
    char head[256];
    if (withCounts && campActive(base)) {
        uintptr_t party[RV_MAX_MEMBERS];
        int np = rvPartyList(base, party, RV_MAX_MEMBERS);
        char pts[96];
        if (campPointsText(base, pts, sizeof pts))
            _snprintf(head, sizeof head, axs(AXS_RV_HEAD_CAMP_FMT), np, pts);
        else
            _snprintf(head, sizeof head, axs(AXS_RV_HEAD_CAMP_BARE_FMT), np);
        head[sizeof head - 1] = 0;
        logLine("roomview enter: camp phase %d, party=%d, points=%d",
                campPhase(base), np, campPoints(base));
        RvEntry crows[RV_MAX_ROWS];
        int cn = rvBuildRoom(base, crows, RV_MAX_ROWS);
        if (cn <= 0) { postSpeech(head); return; }
        if (g_rvCursor < 0)  g_rvCursor = 0;
        if (g_rvCursor >= cn) g_rvCursor = cn - 1;
        g_rvTipLine = 0;
        rvSpeakRow(base, crows, cn, g_rvCursor, head);
        return;
    }
    if (withCounts) {
        uintptr_t party[RV_MAX_MEMBERS];
        uintptr_t foes[RV_MAX_ENEMIES];
        uintptr_t props[RV_MAX_PROPS];
        uint32_t  dests[RV_MAX_DOORS];
        uintptr_t doors[RV_MAX_DOORS];
        int np  = rvPartyList(base, party, RV_MAX_MEMBERS);
        int ne  = rvEnemyList(base, foes,  RV_MAX_ENEMIES);
        int npr = rvPropList (base, props, RV_MAX_PROPS);
        bool wave = comIsWaveRaid(base);
        int nd  = wave ? 0 : rvDoorList(base, dests, doors, RV_MAX_DOORS);
        bool wayOn = comCanAdvance(base);
        int ho = wave
                   ? _snprintf(head, sizeof head, axs(AXS_RV_HEAD_WAVE_FMT), np, ne, npr)
                   : _snprintf(head, sizeof head, axs(AXS_RV_HEAD_FMT), np, ne, npr, nd);
        if (ho < 0 || ho >= (int)sizeof head) ho = (int)sizeof head - 1;
        if (wayOn) _snprintf(head + ho, sizeof head - ho, " %s", axs(AXS_RV_WAY_ON_OPEN));
        logLine("roomview enter: party=%d enemies=%d props=%d doors=%d wave=%d wayon=%d",
                np, ne, npr, nd, wave ? 1 : 0, wayOn ? 1 : 0);
    } else {
        _snprintf(head, sizeof head, "%s", axs(AXS_RV_HEAD));
    }
    head[sizeof head - 1] = 0;

    RvEntry rows[RV_MAX_ROWS];
    int n = rvBuildRoom(base, rows, RV_MAX_ROWS);
    if (n <= 0) {
        logLine("roomview enter: no rows to place the cursor on");
        postSpeech(head);
        return;
    }
    if (withCounts) {
        for (int i = n - 1; i >= 0; i--) {       // the way-on row is appended last; scan back
            if (rows[i].kind != RV_ADVANCE) continue;
            if (g_rvCursor != i)
                logLine("roomview enter: the way on is offered -> cursor to row %d of %d", i + 1, n);
            g_rvCursor = i;
            break;
        }
    }
    if (g_rvCursor < 0)  g_rvCursor = 0;         // land ON something, never on nothing
    if (g_rvCursor >= n) g_rvCursor = n - 1;     // a remembered row that no longer exists clamps
    g_rvTipLine = 0;
    rvSpeakRow(base, rows, n, g_rvCursor, head);
}

// ---- The dungeon view <-> action bar stitch ----

bool abEnterFromRoom(uintptr_t base, const char* head) {
    ActionItem items[AB_MAX_ITEMS];
    int n = abBuildBar(base, items, AB_MAX_ITEMS);
    if (n <= 0) {
        logLine("actionbar: no bar on screen — staying in the dungeon view");
        postSpeech(axs(AXS_RV_NO_ACTIONS));
        return true;
    }
    if (!g_abDumped) { abDumpBar(base, items, n); g_abDumped = true; }
    g_abRoomReturn = g_rvCursor;         // remember the row, before rvSetActive re-seeds it
    rvSetActive(base, false);
    abSetActive(base, true);
    logLine("stitch: dungeon view -> action bar (items=%d, return row=%d)", n, g_abRoomReturn);
    abSpeakLabel(base, &items[0], abSlotCount(items, n), head ? head : "Actions.");
    return true;
}

bool rvEnterFromBar(uintptr_t base) {
    uintptr_t party[RV_MAX_MEMBERS];
    if (rvPartyList(base, party, RV_MAX_MEMBERS) <= 0) {
        // The dungeon view would have nothing to show. Do not pretend it opened.
        logLine("stitch: action bar -> dungeon view, but there is no party to show");
        postSpeech(axs(AXS_RV_NOTHING_HERE));
        return true;
    }
    abSetActive(base, false);
    rvSetActive(base, true);

    int want = g_abRoomReturn;
    g_abRoomReturn = -1;
    if (want >= 0) g_rvCursor = want;
    logLine("stitch: action bar -> dungeon view, return row=%d", want);
    rvAnnounceEntry(base, false);
    return true;
}

bool routeRoomKey(uintptr_t base, uint32_t sym, uint16_t mod, uint8_t repeat) {
    if (mod & (KMOD_LALT | KMOD_RALT)) return false;

    if (sym == SDLK_TAB) {
        if (mod & (KMOD_LCTRL | KMOD_RCTRL)) return false;   // Ctrl+Tab is not ours
        if (repeat) return false;                            // the game's key: repeats are its own too
        logLine("roomview: Tab -> closing the view, panel toggle passes through to the game");
        rvSetActive(base, false);
        armPanelHandoff("the dungeon view");
        return false;
    }

    if (mod & (KMOD_LCTRL | KMOD_RCTRL)) {
        if (sym == SDLK_LEFT || sym == SDLK_RIGHT)
            return rvTipPanelSwitch(base, sym == SDLK_RIGHT ? 1 : -1, repeat);
        if (sym != SDLK_UP && sym != SDLK_DOWN) return false;
        if (repeat) return true;
        rvSpeakTipLine(base, sym == SDLK_UP ? 1 : -1);
        return true;
    }

    if (sym == SDLK_UP) {
        if (repeat) return true;         // held key must not flap the context
        return qtEnterFromRoom(base);
    }
    if (sym == SDLK_DOWN) {
        if (repeat) return true;
        return abEnterFromRoom(base);
    }

    if (sym == SDLK_c) {
        if (repeat) return true;
        RvEntry rows[RV_MAX_ROWS];
        int n = rvBuildRoom(base, rows, RV_MAX_ROWS);
        if (n <= 0 || g_rvCursor < 0 || g_rvCursor >= n) return false;
        const RvEntry& e = rows[g_rvCursor];
        if (e.kind != RV_ACTOR || e.enemy) return false;
        return pitInspectHero(base, e.actor);
    }

    if (sym == SDLK_RETURN || sym == SDLK_KP_ENTER) {
        if (repeat) return true;                 // one attempt per press; never leak a held key
        RvEntry rows[RV_MAX_ROWS];
        int n = rvBuildRoom(base, rows, RV_MAX_ROWS);
        if (n <= 0 || g_rvCursor < 0 || g_rvCursor >= n) return false;
        const RvEntry& e = rows[g_rvCursor];
        if (e.kind == RV_ADVANCE) return rvAdvanceRoom(base);
        if (e.kind == RV_ACTOR && !e.enemy && pitActivateHero(base, e.actor)) return true;
        if (e.kind != RV_PROP) return false;                         // not ours — the game's key
        if (propIsTrap(e.obj)) return rvDisarmTrap(base, e.obj);
        return rvInteractProp(base, e.obj);
    }

    {
        int jump = 0;
        if (axDecodeJump(sym, mod, repeat, &jump)) {
            if (!jump) return true;              // held jump: one landing per press
            RvEntry rows[RV_MAX_ROWS];
            int n = rvBuildRoom(base, rows, RV_MAX_ROWS);
            if (n <= 0) {
                logLine("roomview: nothing to navigate");
                postSpeech(axs(AXS_RV_NOTHING_HERE));
                return true;
            }
            g_rvCursor = (jump > 0) ? n - 1 : 0;
            g_rvTipLine = 0;                     // a new row rewinds its buffer
            rvSpeakRow(base, rows, n, g_rvCursor);
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

    RvEntry rows[RV_MAX_ROWS];
    int n = rvBuildRoom(base, rows, RV_MAX_ROWS);
    if (n <= 0) {
        logLine("roomview: nothing to navigate");
        postSpeech(axs(AXS_RV_NOTHING_HERE));
        return true;
    }
    if (g_rvCursor >= n) g_rvCursor = n - 1;

    if (g_rvCursor < 0) g_rvCursor = 0;
    else axStepCursor(&g_rvCursor, n, dir);    // hard stop: clamp, then re-read the row we sat on

    g_rvTipLine = 0;
    rvSpeakRow(base, rows, n, g_rvCursor);
    return true;
}

bool routeRoomToggle(uintptr_t base, uint32_t sym, uint8_t repeat) {
    if (sym != SDLK_r) return false;
    if (repeat) return true;                   // held key must not flap the context

    int keepRow = g_rvActive ? g_rvCursor : -1;
    if (g_qtActive) qtSetActive(base, false);

    uintptr_t party[RV_MAX_MEMBERS];
    int np = rvPartyList(base, party, RV_MAX_MEMBERS);
    if (np <= 0) {
        logLine("roomview: no party found, not entering");
        postSpeech(axs(AXS_RV_NOTHING_HERE));
        return true;
    }
    if (!g_rvDumped) {
        rvDumpRoom(base, party, np);
        uintptr_t seen[RV_MAX_ENEMIES];
        if (rvEnemyList(base, seen, RV_MAX_ENEMIES) > 0) g_rvDumped = true;
    }
    if (!g_rvPropDumped) {
        rvDumpProps(base);
        uintptr_t seen[RV_MAX_PROPS];
        if (rvPropList(base, seen, RV_MAX_PROPS) > 0) g_rvPropDumped = true;
    }
    static bool s_waveDumped = false;
    if (axDebugLogEnabled() && !s_waveDumped && comIsWaveRaid(base)) { comWaveProbe(base); s_waveDumped = true; }

    if (axDebugLogEnabled() && !g_rvArenaProbed && pitInMatch(base)) rvProbeArena(base);

    if (g_abActive) abSetActive(base, false);
    if (g_tsActive) { tsSetActive(false); logLine("targeting: abandoned (dungeon view opened)"); }
    iuAbandon("dungeon view opened");

    rvSetActive(base, true);
    if (keepRow >= 0) g_rvCursor = keepRow;   // a re-announce does not move you

    rvAnnounceEntry(base, true);
    return true;
}
