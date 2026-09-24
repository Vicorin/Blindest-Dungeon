// raid/actionbar.cpp -- THE ACTION BAR AND THE TARGETING LAYER

#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cmath>
#include "internal.h"

// ---- In-raid ACTION BAR ----
static const uint32_t  SKILL_FOURCC_BASE  = 0x736b6c6c;
static const uint32_t  PASS_FOURCC        = 0x70617373;  // "pass" — skip turn
static int64_t g_tsSkillElemId = 0;
// ---- IS THE GAME ITSELF HOLDING THIS SKILL? ----
static bool g_tsArmed = false;
static bool g_tsAdoptArmed = false;
static uintptr_t g_tsSkill = 0;
static bool g_tsUserSel = false;
static const uint32_t  REORDER_FOURCC     = 0x727062;    // reorder party ("str_party_default_order")

// ---- Portrait: the id is keyed on the HERO'S GUID, not on a party slot ----
static const uint32_t  PORTRAIT_ID_BASE   = 0x72737469;  // 'rst'+0x69, + Hero::guarded_guid
static const uint32_t  PORTRAIT_FAMILY_LO = 0x72737400;  // the family floor (old mask's base)
static const uint32_t  PORTRAIT_FAMILY_HI = 0x72737400 + 0x10000;  // 65,536 heroes of headroom

static const char* const REORDER_LABEL_KEY = "str_party_default_order";

static const uintptr_t RAID_SEL_HERO_OFF  = 0x438;   // raid root+: Hero* currently selected
// ---- The TARGETING PREVIEW (same renderer, FUN_1406ee670) ----
static const char* const TP_HIT_KEY       = "str_ui_hero_to_hit";        // "Hero to Hit"
static const char* const TP_CRIT_KEY      = "str_ui_hero_crit";          // "Hero to Crit"
static const char* const TP_DMG_KEY       = "str_ui_hero_dmg";           // "Hero DMG"
static const char* const TP_DEATHBLOW_KEY = "str_ui_hero_to_deathblow";  // "Hero to Deathblow"
static const char* const TP_STRESS_KEY    = "str_ui_hero_stress";        // "Hero Stress"

static const uintptr_t ACTOR_ARM_PIERCE_OFF   = 0xc18;
static const uintptr_t ACTOR_DMG_RECV_OFF     = 0xbe0;
static const uintptr_t ACTOR_DEATHBLOW_OFF    = 0xcbc;
static const uintptr_t ACTOR_STRESS_DEALT_OFF = 0xecc;
static const uintptr_t ACTOR_STRESS_RECV_OFF  = 0xf2c;
static const uintptr_t ACTOR_VS_RANK_A_OFF    = 0x1010;
static const uintptr_t ACTOR_VS_RANK_B_OFF    = 0x1014;
static const uintptr_t ACTOR_CUR_SKILL_OFF    = 0x8e0;

// Skill fields, all bytes, all read as "does this skill bypass X".
static const uintptr_t SKILL_IGNORE_PROT_OFF  = 0x43c;
static const uintptr_t SKILL_IGN_DEATHBLW_OFF = 0x43f;
static const uintptr_t SKILL_EFFECTS_END_OFF  = 0x3d0;
static const uintptr_t EFFECT_STRESS_OFF      = 0x4c;   // Effect+: float, stress this effect deals

static const int       TP_STATE_CHOOSING_LO = 0x1c;
static const int       TP_STATE_CHOOSING_HI = 0x1e;
static const uintptr_t RAID_BATTLE_OVERRIDE_OFF = 0x4a83;
static const uintptr_t RAID_TURN_ACTOR_OFF      = 0x4968;

// Vtable slots used here, all on the target actor.
static const uintptr_t VT_ACTOR_RANK_INDEX  = 0x28;   // () -> int, the actor's rank index
static const uintptr_t VT_ACTOR_CLASS       = 0x30;   // () -> Class*, id as inline char[] at +0x48
static const uintptr_t VT_ACTOR_DEATHS_DOOR = 0x140;

static const float     TP_PERCENT_SCALE = 100.0f;
// ---- Skill TOOLTIP ----
static const uintptr_t SKILL_TYPE_OFF     = 0x50;    // .type C-string ("melee"/"ranged") -> "str_%s"
static const uintptr_t SKILL_MOVE_BACK_OFF= 0x35c;   // int .move back
static const uintptr_t SKILL_MOVE_FWD_OFF = 0x360;   // int .move forward
static const uintptr_t SKILL_HEAL_MIN_OFF = 0x364;   // int .heal min
static const uintptr_t SKILL_HEAL_MAX_OFF = 0x368;   // int .heal max
static const uintptr_t SKILL_ACC_OFF      = 0x380;   // float .atk (accuracy), stored as a fraction
static const uintptr_t SKILL_CRIT_OFF     = 0x384;   // float .crit, stored as a fraction
static const uintptr_t SKILL_DMG_OFF      = 0x41c;
static const uintptr_t SKILL_HEALFLAG_OFF = 0x90;    // byte: picks the composer's heal branch
                                                     // (heal skills show Heal x-y, not ACC/DMG/CRIT)
static const uintptr_t SKILL_RANDTGT_OFF  = 0x91;    // byte: random target
static const uintptr_t SKILL_NOINFO_OFF   = 0x3ac;
static const uintptr_t SKILL_PLAIN_OFF    = 0x4f8;
static const uintptr_t SKILL_IGN_PROT_OFF = 0x43c;
static const uintptr_t SKILL_IGN_GUARD_OFF= 0x43d;
static const uintptr_t SKILL_IGN_STLTH_OFF= 0x43e;
static const uintptr_t SKILL_IGN_DDOOR_OFF= 0x43f;
static const uintptr_t SKILL_REQ_BURNING_OFF  = 0x398;  // byte: the skill needs a BURNING target
static const uintptr_t SKILL_DMG_PER_BURN_OFF = 0x39c;  // float FRACTION: +DMG per burn stack
static const uintptr_t SKILL_COPY_BURN_OFF    = 0x3a0;  // byte: hits behind too, and copies the burn

// ---- Skill TARGETING (Enter on the action bar) ----
static const uint32_t  SKILL_AREA_BIT     = 0x100;

static const uintptr_t SKILL_ISMOVE_OFF   = 0x3ac;

static const uintptr_t SKILL_SELFONLY_OFF = 0x3ae;

static const uintptr_t SKILL_USER_SELECTED_OFF = 0x4a8;

// ---- SKIP TURN (the action bar's 'pass' button) ----

// ---- Skill ADDITIONAL EFFECTS (buffer line 3) ----
static const uintptr_t HEROCLASS_EFFDEF_BEG_OFF = 0xb38;  // HeroClass+: effect definition table begin
static const uintptr_t HEROCLASS_EFFDEF_END_OFF = 0xb40;  // HeroClass+: ... and end
static const uintptr_t EFFDEF_STRIDE      = 0x90;
static const uintptr_t EFFDEF_KEY_OFF     = 0x20;    // uint key into the skill's effect map
static const uintptr_t SKILL_EFFMAP_OFF   = 0x3e0;
static const uintptr_t SKILL_PERLINE_OFF  = 0x500;

static const uintptr_t EFF_DISPLAY_OFF    = 0x43c;
static const uintptr_t EFF_CHANCE_OFF     = 0x48;   // float, stored as a fraction
static const uintptr_t EFF_CATEGORY_OFF   = 0x44;   // int display type -> which header bucket
static const uintptr_t EFF_NOHEADER_OFF   = 0x2c8;
static const uintptr_t EFF_TORCH_OFF      = 0x198;
static const uintptr_t EFF_TORCH_NEG_OFF  = 0x19c;
static const uintptr_t EFF_STRESS_OFF     = 0x4c;   // float
static const uintptr_t EFF_STRESS_HEAL_OFF= 0xd4;   // float
static const uintptr_t EFF_STUN_OFF       = 0x50;   // float
static const uintptr_t EFF_UNSTUN_OFF     = 0x54;   // float
static const uintptr_t EFF_DAZE_OFF       = 0x58;   // float
static const uintptr_t EFF_UNDAZE_OFF     = 0x5c;   // float
static const uintptr_t EFF_TAG_OFF        = 0x60;   // int (mark)
static const uintptr_t EFF_UNTAG_OFF      = 0x64;   // int
static const uintptr_t EFF_HEAL_OFF       = 0xc8;   // float
static const uintptr_t EFF_HEAL_PCT_OFF   = 0xcc;   // float, fraction of max HP
static const uintptr_t EFF_CURE_POISON_OFF= 0xd8;   // int
static const uintptr_t EFF_CURE_BLEED_OFF = 0xdc;   // int
static const uintptr_t EFF_CURE_BURN_OFF  = 0xe0;   // int (The Fire's Edge) — and THIS is why cure
static const uintptr_t EFF_CURE_DISEASE_OFF = 0xe4;
static const uintptr_t EFF_DOT_POISON_OFF = 0x130;
static const uintptr_t EFF_DOT_BLEED_OFF  = 0x134;
static const uintptr_t EFF_DOT_BURN_OFF   = 0x138;  // int burn stacks applied (The Fire's Edge)
static const uintptr_t EFF_DOT_IRRESIST_OFF = 0x13c;  // byte .dot_irresistible: burn ignores resist
static const uintptr_t EFF_REQ_BURNING_OFF     = 0x13d;
static const uintptr_t EFF_REQ_NOT_BURNING_OFF = 0x13e;  // byte: ...only vs a target that is NOT burning
static const uintptr_t EFF_DOT_STRESS_OFF = 0x140;
static const uintptr_t EFF_CLEAR_DOT_STRESS_OFF = 0x144;
static const uintptr_t EFF_SHUFFLE_DOT_OFF= 0x14c;
static const uintptr_t EFF_HEAL_DOT_OFF   = 0x150;
static const uintptr_t EFF_DOT_DURATION_OFF = 0x154;
static const uintptr_t EFF_GUARD_OFF      = 0x158;
static const uintptr_t EFF_CLEAR_GUARDING_OFF = 0x15c;
static const uintptr_t EFF_CLEAR_GUARDED_OFF  = 0x160;
static const uintptr_t EFF_RIPOSTE_OFF    = 0x168;
static const uintptr_t EFF_CLEAR_VIRTUE_OFF = 0x17c;
static const uintptr_t EFF_MOVE_OFF       = 0x1a4;
static const uintptr_t EFF_SHUFFLE_OFF    = 0x1a8;
static const uintptr_t EFF_DISEASE_OFF    = 0x20f;
static const uintptr_t EFF_CBURN_OFF      = 0x2c0;  // int .controlled_burn_amount (The Fire's Edge)
static const uintptr_t EFF_HEALTH_BLOCKS_OFF = 0x2ec;
static const uintptr_t EFF_STEALTH_OFF    = 0x2f0;
static const uintptr_t EFF_UNSTEALTH_OFF  = 0x2f4;
static const uintptr_t EFF_GUARD_PERF_OFF = 0x360;
static const uintptr_t EFF_ACTOR_MODE_OFF = 0x36c;
static const uintptr_t EFF_HP_DAMAGE_OFF  = 0x3e0;
static const uintptr_t EFF_CURE_DEBUFF_OFF= 0x434;
static const uintptr_t EFF_REFRESH_SKILL_OFF = 0x4c5;
static const uintptr_t EFF_REQ_KILL_OFF   = 0x4c6;
// ---- The Fire's Edge: TRINKET effects ----
static const uintptr_t EFF_ON_HIT_OFF     = 0x2c9;
static const uintptr_t EFF_ON_MISS_OFF    = 0x2ca;  // byte .on_miss   -> appends "_miss"
static const uintptr_t EFF_IS_TRIGGER_OFF = 0x2ce;
static const uintptr_t EFF_TRIG_INC_LO_OFF = 0x2d0; // int  \ "Trigger Limit +%d-%d"; shown when either
static const uintptr_t EFF_TRIG_INC_HI_OFF = 0x2d4; // int  /  half is > 0
static const uintptr_t EFF_ACTIONS_OFF    = 0x430;
static const uintptr_t EFF_QUIRK_POS_PCT_OFF = 0x4c8;
static const uintptr_t EFF_QUIRK_NEG_OFF  = 0x4cc;  // byte (was 0x4c4 before 6aa18626)
static const uintptr_t EFF_GAIN_TRINKET_OFF = 0x50d;
static const uintptr_t EFF_GAIN_RND_TRINKET_OFF = 0x4cd; // byte (was 0x4c5 before 6aa18626)
static const uintptr_t EFF_TOWN_EVENT_OFF = 0x54d;
static const uintptr_t EFF_DESTROY_TRINKET_OFF = 0x54e; // byte (was 0x546 before 6aa18626)
static const uintptr_t EFF_IN_INVENTORY_OFF = 0x54f;

static const int AB_EFF_BUCKETS   = 29;   // (was 23)
static const char* const kEffBucketKey[AB_EFF_BUCKETS] = {
    nullptr, nullptr, nullptr,
    "effect_tooltip_target_group",
    "effect_tooltip_target_group_other",
    "effect_tooltip_target_enemy_group",
    "effect_tooltip_self",
    "effect_tooltip_party",
    "effect_tooltip_party_other",
    "effect_tooltip_target_buff",
    "effect_tooltip_target_debuff",
    "effect_tooltip_target_group_buff",
    "effect_tooltip_target_group_debuff",
    "effect_tooltip_target_group_other_buff",
    "effect_tooltip_target_group_other_debuff",
    "effect_tooltip_target_enemy_group_buff",
    "effect_tooltip_target_enemy_group_debuff",
    "effect_tooltip_self_buff",
    "effect_tooltip_self_debuff",
    "effect_tooltip_party_buff",
    "effect_tooltip_party_debuff",
    "effect_tooltip_party_other_buff",
    "effect_tooltip_party_other_debuff",
    "effect_tooltip_target_enemy_rank",        // [23] new in 24784538
    "effect_tooltip_target_enemy_rank_buff",   // [24]
    "effect_tooltip_target_enemy_rank_debuff", // [25]
    "effect_tooltip_this_trinket",             // [26]
    nullptr,                                   // [27] (the game stores a null here)
    "effect_tooltip_target_enemy_random",      // [28]
};
static const int AB_EFF_MAX_DEFS    = 128;  // sanity cap on the definition-table walk
static const int AB_EFF_MAX_PER_VEC = 32;   // sanity cap on one effect vector

// ---- Action-bar virtual buffer ----
// ---- In-raid ACTION BAR ----

static bool abReadElem(uintptr_t elem, ActionItem* out) {
    uint8_t focusable = 0, skip = 0;
    if (!safeReadU8(elem + ELEM_FOCUSABLE_OFF, &focusable) || !focusable) return false;
    if (safeReadU8(elem + ELEM_SKIP_OFF, &skip) && skip) return false;

    int64_t id = 0; uintptr_t owner = 0;
    if (!safeReadI64(elem + ELEM_ID_OFF, &id) || isNoFocus(id)) return false;
    safeReadPtr(elem + ELEM_OWNER_OFF, &owner);

    uint32_t px = 0, py = 0, sw = 0, sh = 0;
    if (!safeReadU32(elem + ELEM_POS_OFF,     &px) || !safeReadU32(elem + ELEM_POS_OFF + 4,  &py)) return false;
    if (!safeReadU32(elem + ELEM_SIZE_OFF,    &sw) || !safeReadU32(elem + ELEM_SIZE_OFF + 4, &sh)) return false;

    float x, y, w, h;
    memcpy(&x, &px, 4); memcpy(&y, &py, 4); memcpy(&w, &sw, 4); memcpy(&h, &sh, 4);
    out->id = id; out->owner = (int64_t)owner;
    out->cx = x + w * 0.5f; out->cy = y + h * 0.5f;
    out->kind = -1; out->skillIdx = -1;
    return true;
}

static uint32_t abPortraitIdFor(uintptr_t base) {
    uintptr_t hero = abSelectedHero(base);
    uint32_t  guid = 0;
    if (!hero || !safeReadU32(hero + ACTOR_ID_OFF, &guid)) return 0;
    return PORTRAIT_ID_BASE + guid;
}

static bool abClassify(ActionItem* it, uint32_t wantPortrait) {
    uint32_t id = (uint32_t)(uint64_t)it->id;
    if (wantPortrait && id == wantPortrait)          { it->kind = AB_PORTRAIT; return true; }
    if (id >= PORTRAIT_FAMILY_LO && id < PORTRAIT_FAMILY_HI)
                                                     { it->kind = AB_PORTRAIT; return true; }
    if (id == REORDER_FOURCC)                        { it->kind = AB_REORDER;  return true; }
    if (id == PASS_FOURCC)                           { it->kind = AB_PASS;     return true; }
    if (id == REST_FOURCC)                           { it->kind = AB_REST;     return true; }
    if ((uint32_t)(uint64_t)it->owner == SKILL_FOURCC_BASE && id != SKILL_FOURCC_BASE - 1) {
        it->kind = AB_SKILL;
        it->skillIdx = (int)(id - SKILL_FOURCC_BASE);
        return it->skillIdx >= 0 && it->skillIdx < AB_MAX_SKILLS;
    }
    return false;
}

int abBuildBar(uintptr_t base, ActionItem* items, int maxItems) {
    uintptr_t begin = 0, end = 0;
    if (!safeReadPtr(base + VEC_BEGIN_RVA, &begin) || !safeReadPtr(base + VEC_END_RVA, &end)) return 0;
    if (begin == 0 || end <= begin) return 0;
    uintptr_t count = (end - begin) / ELEM_STRIDE;
    if (count > 4096) count = 4096;

    ActionItem portrait, reorder, pass, rest, skills[AB_MAX_SKILLS];
    bool hasPortrait = false, hasReorder = false, hasPass = false, hasRest = false;
    bool exactPortrait = false;   // found by guid, not by the family range
    int nSkills = 0;

    const uint32_t wantPortrait = abPortraitIdFor(base);

    for (uintptr_t i = 0; i < count; i++) {
        ActionItem it;
        if (!abReadElem(begin + i * ELEM_STRIDE, &it)) continue;
        if (!abClassify(&it, wantPortrait)) continue;
        switch (it.kind) {
            case AB_PORTRAIT:
                if (exactPortrait) break;
                portrait = it; hasPortrait = true;
                exactPortrait = (wantPortrait && (uint32_t)(uint64_t)it.id == wantPortrait);
                break;
            case AB_REORDER:  reorder  = it; hasReorder  = true; break;
            case AB_PASS:     pass     = it; hasPass     = true; break;
            case AB_REST:     rest     = it; hasRest     = true; break;
            default:          if (nSkills < AB_MAX_SKILLS) skills[nSkills++] = it; break;
        }
    }
    if (nSkills == 0 && !hasRest) return 0;

    {
        static uint32_t s_lastWant = 0xffffffffu, s_lastGot = 0xffffffffu;
        uint32_t got = hasPortrait ? (uint32_t)(uint64_t)portrait.id : 0;
        if (got != s_lastGot || wantPortrait != s_lastWant) {
            s_lastGot = got; s_lastWant = wantPortrait;
            if (hasPortrait)
                logLine("actionbar portrait: id=0x%08x want=0x%08x by=%s%s",
                        got, wantPortrait, exactPortrait ? "guid" : "family",
                        (got & 0xffffff00u) != PORTRAIT_FAMILY_LO ? " (outside the old 8-bit mask)" : "");
            else
                logLine("actionbar portrait: MISSING (want=0x%08x, %d skills on the bar)",
                        wantPortrait, nSkills);
        }
    }

    // Action slots in skill-index order (which is also their left-to-right order).
    for (int i = 1; i < nSkills; i++) {
        ActionItem key = skills[i];
        int j = i - 1;
        while (j >= 0 && skills[j].skillIdx > key.skillIdx) { skills[j + 1] = skills[j]; j--; }
        skills[j + 1] = key;
    }

    int n = 0;
    if (hasPortrait && n < maxItems) items[n++] = portrait;
    for (int i = 0; i < nSkills && n < maxItems; i++) items[n++] = skills[i];
    if (hasReorder && n < maxItems) items[n++] = reorder;
    if (hasPass && n < maxItems) items[n++] = pass;
    if (hasRest && n < maxItems) items[n++] = rest;
    return n;
}

uintptr_t csTownSheetPanel(uintptr_t base);
uintptr_t g_ptySheetHero = 0;

uintptr_t abSelectedHero(uintptr_t base) {
    uintptr_t root = 0;
    if (safeReadPtr(base + MAP_ROOT_RVA, &root) && root > 0x10000) {
        uintptr_t hero = 0;
        if (!safeReadPtr(root + RAID_SEL_HERO_OFF, &hero) || hero <= 0x10000) return 0;
        return hero;
    }
    if (g_ptySheetHero && csTownSheetPanel(base)) return g_ptySheetHero;
    return 0;
}

bool abPlausibleName(const char* s) {
    if (!s || !s[0]) return false;
    for (const char* p = s; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (c < 32 || c == 127) return false;
    }
    return true;
}

// A given hero's HeroClass, or 0.
uintptr_t abHeroClassOf(uintptr_t hero) {
    if (!hero) return 0;
    uintptr_t cobj = 0;
    if (!safeReadPtr(hero + ACTOR_CLASS_OFF, &cobj) || cobj <= 0x10000) return 0;
    return cobj;
}

// The selected hero's HeroClass, or 0.
uintptr_t abHeroClass(uintptr_t base) {
    return abHeroClassOf(abSelectedHero(base));
}

bool abHeroNameClassOf(uintptr_t base, uintptr_t hero,
                       char* name, int namesz, char* cls, int clssz) {
    (void)base;
    name[0] = 0; cls[0] = 0;
    if (!hero) { logLine("actionbar: no hero to label"); return false; }

    if (!safeReadCStr(hero + HERO_NAME_OFF, name, namesz) || !abPlausibleName(name)) name[0] = 0;

    uintptr_t cobj = abHeroClassOf(hero);
    if (cobj) {
        uint8_t latched = 0;
        if (safeReadU8(cobj + HEROCLASS_DISP_LATCH_OFF, &latched) && latched) {
            if (!safeReadCStr(cobj + HEROCLASS_DISP_OFF, cls, clssz) || !abPlausibleName(cls)) cls[0] = 0;
        }
        // Not latched yet -> the raw class id is at least meaningful, and never silence.
        if (!cls[0]) {
            if (!safeReadCStr(cobj + HEROCLASS_ID_OFF, cls, clssz) || !abPlausibleName(cls)) cls[0] = 0;
        }
    }

    logLine("actionbar hero=%p class=%p name=\"%s\" class=\"%s\"", (void*)hero, (void*)cobj, name, cls);
    return name[0] || cls[0];
}

// The SELECTED hero's name and localized class.
bool abHeroNameClass(uintptr_t base, char* name, int namesz, char* cls, int clssz) {
    return abHeroNameClassOf(base, abSelectedHero(base), name, namesz, cls, clssz);
}

bool abHeroLabelOf(uintptr_t base, uintptr_t hero, char* out, int outsz) {
    char name[80], cls[80];
    if (!abHeroNameClassOf(base, hero, name, sizeof name, cls, sizeof cls)) return false;
    char dead[96];
    if (rvCorpseWord(base, hero, dead, sizeof dead)) {
        _snprintf(cls, sizeof cls, "%s", dead);
        cls[sizeof cls - 1] = 0;
    }
    if (name[0] && cls[0]) _snprintf(out, outsz, "%s, %s", name, cls);
    else                   _snprintf(out, outsz, "%s", name[0] ? name : cls);
    out[outsz - 1] = 0;
    return true;
}

bool abHeroLabel(uintptr_t base, char* out, int outsz) {
    return abHeroLabelOf(base, abSelectedHero(base), out, outsz);
}

bool abModeAllowsSkill(uintptr_t skill, uint32_t modeId) {
    if (modeId == 0) return true;                                         // hero has no modes: never filter
    uintptr_t size = 0;
    if (!safeReadPtr(skill + SKILL_VALIDMODES_SIZE_OFF, &size)) return true;
    if (size == 0) return true;                                          // .valid_modes empty: valid everywhere
    uintptr_t head = 0;
    if (!safeReadPtr(skill + SKILL_VALIDMODES_OFF, &head) || head <= 0x10000) return true;
    uintptr_t node = 0;
    if (!safeReadPtr(head + 0x08, &node) || node <= 0x10000) return true; // head+0x08 = _Parent = root

    uintptr_t bound = head;                                              // lower_bound result (head = end/not-found)
    for (int guard = 0; guard < 64; guard++) {
        uint8_t isnil = 1;
        if (!safeReadU8(node + 0x19, &isnil)) return true;
        if (isnil) break;
        uint32_t key = 0;
        if (!safeReadU32(node + 0x1c, &key)) return true;
        uintptr_t next = 0;
        if (key < modeId) {
            if (!safeReadPtr(node + 0x10, &next)) return true;           // key too small: go right, bound unchanged
        } else {
            if (!safeReadPtr(node + 0x00, &next)) return true;           // key >= target: go left...
            bound = node;                                               // ...and this node bounds the search
        }
        if (next <= 0x10000) return true;
        node = next;
    }
    if (bound == head) return false;                                    // every key was smaller: not in the set
    uint8_t bnil = 1; uint32_t bkey = 0;
    if (!safeReadU8(bound + 0x19, &bnil) || bnil) return false;
    if (!safeReadU32(bound + 0x1c, &bkey)) return false;
    return modeId >= bkey;                                              // bkey is >= modeId by lower_bound, so == modeId
}

int abSkillList(uintptr_t base, uintptr_t* skills, int maxSkills) {
    uintptr_t hero = abSelectedHero(base), cobj = abHeroClass(base);
    if (!hero || !cobj) return 0;

    uintptr_t selBeg = 0, selEnd = 0, slotMap = 0, vecArr = 0;
    if (!safeReadPtr(hero + ACTOR_SEL_BEG_OFF, &selBeg) ||
        !safeReadPtr(hero + ACTOR_SEL_END_OFF, &selEnd) ||
        !safeReadPtr(hero + ACTOR_SLOTMAP_OFF, &slotMap) ||
        !safeReadPtr(cobj + HEROCLASS_SKILLVEC_OFF, &vecArr)) return 0;
    if (selBeg <= 0x10000 || selEnd < selBeg || slotMap <= 0x10000 || vecArr <= 0x10000) return 0;

    uint32_t modeId = 0;
    safeReadU32(hero + ACTOR_MODE_ID_OFF, &modeId);

    int n = 0, dropped = 0;
    uintptr_t nSel = (selEnd - selBeg) / 4;            // vector<uint> of equipped slot indices
    if (nSel > (uintptr_t)maxSkills) nSel = maxSkills;
    for (uintptr_t i = 0; i < nSel && n < maxSkills; i++) {
        uint32_t slot = 0;
        if (!safeReadU32(selBeg + i * 4, &slot) || slot > 64) continue;
        uint32_t idx = 0;
        if (!safeReadU32(slotMap + (uintptr_t)slot * 4, &idx)) continue;
        if ((int32_t)idx < 0) continue;                // -1 = nothing equipped in this slot
        uintptr_t inner = 0;
        if (!safeReadPtr(vecArr + (uintptr_t)slot * 0x18, &inner) || inner <= 0x10000) continue;
        uintptr_t skill = inner + (uintptr_t)idx * SKILL_STRIDE;
        if (!abModeAllowsSkill(skill, modeId)) { dropped++; continue; }
        skills[n++] = skill;
    }
    // The trailing slot: the generic move skill.
    if (n < maxSkills) skills[n++] = cobj + HEROCLASS_MOVESKILL_OFF;

    if (modeId != 0) {
        static uintptr_t s_lastHero = 0; static uint32_t s_lastMode = 0; static int s_lastN = -1;
        if (hero != s_lastHero || modeId != s_lastMode || n != s_lastN) {
            s_lastHero = hero; s_lastMode = modeId; s_lastN = n;
            logLine("actionbar mode=0x%08x: dropped %d out-of-form skill(s) -> %d slots (incl. move)",
                    modeId, dropped, n);
        }
    }
    return n;
}

static bool abIsColourCode(const char* p) {
    for (int i = 0; i < 6; i++) {
        unsigned char c = (unsigned char)p[i];
        if (c < 'A' || c > 0x80) return false;
    }
    return true;
}

void abStripMarkup(char* s) {
    char* w = s;
    for (char* r = s; *r; ) {
        if (*r == '{') {
            const char* e = strchr(r, '}');
            if (!e) break;                      // unterminated: stop, keep what we already have
            r = (char*)e + 1;
            continue;
        }
        if (r[0] == '<' && r[1] == 'c' && r[2] == '>' && abIsColourCode(r + 3)) { r += 9; continue; }
        if (r[0] == '<' && r[1] == '/' && r[2] == 'c' && r[3] == '>')           { r += 4; continue; }
        *w++ = *r++;
    }
    *w = 0;
}

bool abFormatMatches(const char* fmt, const char* expect) {
    int i = 0;
    for (const char* p = fmt; *p; p++) {
        if (*p != '%') continue;
        p++;
        if (*p == '%') continue;                       // "%%" is a literal, not a conversion
        while (*p && !strchr("diouxXeEfgGcsp", *p)) p++;   // skip flags/width/precision
        if (!*p) return false;
        char c = *p;
        if (c == 'e' || c == 'E' || c == 'g' || c == 'G') c = 'f';   // all take a double
        if (expect[i] == 0 || c != expect[i]) return false;
        i++;
    }
    return expect[i] == 0;
}

bool abTipFloat(uintptr_t base, const char* key, float v, char* out, int outsz) {
    char fmt[256];
    if (!resolveKey(base, key, fmt, sizeof fmt) || !fmt[0]) return false;
    if (!abFormatMatches(fmt, "f")) { logLine("actionbar tip: \"%s\" fmt mismatch \"%s\"", key, fmt); return false; }
    _snprintf(out, outsz, fmt, (double)v);
    out[outsz - 1] = 0; abStripMarkup(out);
    return out[0] != 0;
}
bool abTipInt(uintptr_t base, const char* key, int v, char* out, int outsz) {
    char fmt[256];
    if (!resolveKey(base, key, fmt, sizeof fmt) || !fmt[0]) return false;
    if (!abFormatMatches(fmt, "d")) { logLine("actionbar tip: \"%s\" fmt mismatch \"%s\"", key, fmt); return false; }
    _snprintf(out, outsz, fmt, v);
    out[outsz - 1] = 0; abStripMarkup(out);
    return out[0] != 0;
}
bool abTipInt2(uintptr_t base, const char* key, int a, int b, char* out, int outsz) {
    char fmt[256];
    if (!resolveKey(base, key, fmt, sizeof fmt) || !fmt[0]) return false;
    if (!abFormatMatches(fmt, "dd")) { logLine("actionbar tip: \"%s\" fmt mismatch \"%s\"", key, fmt); return false; }
    _snprintf(out, outsz, fmt, a, b);
    out[outsz - 1] = 0; abStripMarkup(out);
    return out[0] != 0;
}

bool abTipIntStr(uintptr_t base, const char* key, int a, const char* b, char* out, int outsz) {
    char fmt[256];
    if (!resolveKey(base, key, fmt, sizeof fmt) || !fmt[0]) return false;
    if (!abFormatMatches(fmt, "ds")) { logLine("actionbar tip: \"%s\" fmt mismatch \"%s\"", key, fmt); return false; }
    _snprintf(out, outsz, fmt, a, b);
    out[outsz - 1] = 0; abStripMarkup(out);
    return out[0] != 0;
}

// A plain (unformatted) tooltip key.
bool abTipPlain(uintptr_t base, const char* key, char* out, int outsz) {
    if (!resolveKey(base, key, out, outsz) || !out[0]) return false;
    abStripMarkup(out);
    return out[0] != 0;
}

void abAppend(char* dst, int dstsz, const char* piece) {
    if (!piece || !piece[0]) return;
    size_t n = strlen(dst);
    if (n && n + 2 < (size_t)dstsz) { dst[n++] = '.'; dst[n++] = ' '; dst[n] = 0; }
    _snprintf(dst + n, dstsz - (int)n, "%s", piece);
    dst[dstsz - 1] = 0;
}

static void abSkillTypeText(uintptr_t base, uintptr_t skill, char* out, int outsz) {
    out[0] = 0;
    char buf[256];
    uint8_t randTgt = 0;
    safeReadU8(skill + SKILL_RANDTGT_OFF, &randTgt);
    if (randTgt && abTipPlain(base, "skill_random_target", buf, sizeof buf)) abAppend(out, outsz, buf);
    char type[64] = { 0 };
    if (safeReadCStr(skill + SKILL_TYPE_OFF, type, sizeof type) && abPlausibleName(type)) {
        char key[96];
        _snprintf(key, sizeof key, "str_%s", type);
        key[sizeof key - 1] = 0;
        if (abTipPlain(base, key, buf, sizeof buf)) abAppend(out, outsz, buf);
    }
}

static void abRankText(uint32_t mask, char* out, int outsz) {
    if ((mask & SKILL_RANK_BITS) == SKILL_RANK_BITS) { _snprintf(out, outsz, "%s", axs(AXS_AB_RANK_ANY)); out[outsz-1]=0; return; }
    char list[64] = { 0 };
    int n = 0;
    for (int i = 0; i < 4; i++) {
        if (!(mask & (1u << i))) continue;
        char piece[8];
        _snprintf(piece, sizeof piece, "%s%d", n ? ", " : "", i + 1);
        strncat(list, piece, sizeof list - strlen(list) - 1);
        n++;
    }
    if (!n) { _snprintf(out, outsz, "%s", axs(AXS_AB_RANK_NONE)); out[outsz-1]=0; return; }
    _snprintf(out, outsz, axs(n > 1 ? AXS_AB_RANK_MANY_FMT : AXS_AB_RANK_ONE_FMT), list);
    out[outsz - 1] = 0;
}

static void abTargetRankText(uint32_t mask, char* out, int outsz) {
    bool area = (mask & SKILL_AREA_BIT) != 0;
    int ranks[4]; int n = 0;
    for (int i = 0; i < 4; i++)
        if (mask & (1u << i)) ranks[n++] = i + 1;
    if (n == 0) { _snprintf(out, outsz, "%s", axs(AXS_AB_RANK_NONE)); out[outsz-1]=0; return; }
    if (n == 4) {
        _snprintf(out, outsz, "%s", axs(area ? AXS_AB_TGT_RANKS_ALL : AXS_AB_RANK_ANY));
        out[outsz-1]=0; return;
    }
    if (n == 1) { _snprintf(out, outsz, axs(AXS_AB_TGT_RANK_ONE_FMT), ranks[0]); out[outsz-1]=0; return; }
    if (!area) {
        if (n == 2) _snprintf(out, outsz, axs(AXS_AB_TGT_RANK_OR2_FMT), ranks[0], ranks[1]);
        else        _snprintf(out, outsz, axs(AXS_AB_TGT_RANK_OR3_FMT), ranks[0], ranks[1], ranks[2]);
        out[outsz-1]=0; return;
    }
    char list[64] = { 0 };
    for (int i = 0; i < n; i++) {
        char piece[8];
        _snprintf(piece, sizeof piece, "%s%d", i ? ", " : "", ranks[i]);
        strncat(list, piece, sizeof list - strlen(list) - 1);
    }
    _snprintf(out, outsz, axs(AXS_AB_TGT_RANKS_LIST_FMT), list);
    out[outsz - 1] = 0;
}

static void abSkillRanksText(uintptr_t base, uintptr_t skill, char* out, int outsz) {
    (void)base;
    out[0] = 0;
    uint32_t launch = 0, target = 0;
    if (!safeReadU32(skill + SKILL_LAUNCH_MASK_OFF, &launch)) return;
    if (!safeReadU32(skill + SKILL_TGTMASK_OFF, &target)) return;
    char a[80], b[80];
    abRankText(launch, a, sizeof a);
    abTargetRankText(target, b, sizeof b);
    _snprintf(out, outsz, axs(AXS_AB_USED_FROM_FMT), a, b);
    out[outsz - 1] = 0;
}

// ---- Skill grey-out: is the selected hero out of position to use this skill? ----
static bool skillOutOfPosition(uintptr_t skill, uintptr_t perf) {
    if (!skill || !perf) return false;
    uint8_t isMove = 0;
    if (safeReadU8(skill + SKILL_ISMOVE_OFF, &isMove) && isMove) return false;
    uint32_t launch = 0, rank = 0;
    if (!safeReadU32(skill + SKILL_LAUNCH_MASK_OFF, &launch) || !launch) return false;
    if (!safeReadU32(perf  + ACTOR_RANK_MASK_OFF,   &rank)   || !rank)   return false;
    return (launch & rank) == 0;
}

static void skillLaunchPositions(uint32_t launch, char* out, int outsz) {
    char list[64] = { 0 };
    int n = 0;
    for (int i = 0; i < 4; i++) {
        if (!(launch & (1u << i))) continue;
        char piece[8];
        _snprintf(piece, sizeof piece, "%s%d", n ? ", " : "", i + 1);
        strncat(list, piece, sizeof list - strlen(list) - 1);
        n++;
    }
    if (!n) { _snprintf(out, outsz, "%s", axs(AXS_AB_ANOTHER_POSITION)); out[outsz - 1] = 0; return; }
    _snprintf(out, outsz, axs(n > 1 ? AXS_AB_POSITION_MANY_FMT : AXS_AB_POSITION_ONE_FMT), list);
    out[outsz - 1] = 0;
}

// ---- Skill USE GATES: limited uses, and the health a skill demands of its own performer ----

static int abSkillBattleUsesSpent(uintptr_t hero, uintptr_t skill) {
    if (!hero || !skill) return 0;
    uint32_t want = 0;
    if (!safeReadU32(skill + SKILL_USE_KEY_OFF, &want)) return 0;
    uintptr_t size = 0;
    if (!safeReadPtr(hero + ACTOR_BATTLE_USES_SIZE_OFF, &size) || size == 0) return 0;
    uintptr_t head = 0;
    if (!safeReadPtr(hero + ACTOR_BATTLE_USES_MAP_OFF, &head) || head <= 0x10000) return 0;
    uintptr_t node = 0;
    if (!safeReadPtr(head + 0x08, &node) || node <= 0x10000) return 0;   // head+0x08 = _Parent = root

    uintptr_t bound = head;                                              // lower_bound (head = not found)
    for (int guard = 0; guard < 64; guard++) {
        uint8_t isnil = 1;
        if (!safeReadU8(node + 0x19, &isnil)) return 0;
        if (isnil) break;
        uint32_t key = 0;
        if (!safeReadU32(node + 0x1c, &key)) return 0;
        uintptr_t next = 0;
        if (key < want) { if (!safeReadPtr(node + 0x10, &next)) return 0; }        // go right
        else            { if (!safeReadPtr(node + 0x00, &next)) return 0; bound = node; }  // go left
        if (next <= 0x10000) return 0;
        node = next;
    }
    if (bound == head) return 0;                                         // every key was smaller
    uint8_t bnil = 1; uint32_t bkey = 0;
    if (!safeReadU8(bound + 0x19, &bnil) || bnil) return 0;
    if (!safeReadU32(bound + 0x1c, &bkey) || bkey != want) return 0;     // lower_bound overshot: absent
    int32_t spent = 0;
    if (!safeReadU32(bound + 0x20, (uint32_t*)&spent) || spent < 0) return 0;
    return spent;
}

static bool abSkillBattleUses(uintptr_t skill, uintptr_t hero, int* limit, int* left) {
    *limit = 0; *left = -1;
    int32_t lim = 0;
    if (!safeReadU32(skill + SKILL_PER_BATTLE_LIMIT_OFF, (uint32_t*)&lim) || lim <= 0 || lim > 99)
        return false;                                    // 0 = unlimited; >99 = a torn read, not data
    *limit = lim;
    if (hero) {
        int rem = lim - abSkillBattleUsesSpent(hero, skill);
        *left = rem < 0 ? 0 : rem;
    }
    return true;
}

static bool abSkillHpReqText(uintptr_t base, uintptr_t skill, char* out, int outsz) {
    out[0] = 0;
    uint8_t declared = 0;
    if (!safeReadU8(skill + SKILL_HP_RANGE_SET_OFF, &declared) || !declared) return false;
    float lo = 0.0f, hi = 0.0f;
    if (!abReadF32(skill + SKILL_HP_RANGE_MIN_OFF, &lo)) return false;
    if (!abReadF32(skill + SKILL_HP_RANGE_MAX_OFF, &hi)) return false;
    if (!(lo >= 0.0f) || !(hi >= lo) || !(hi <= 1.0f)) return false;  // written so a NaN also fails
    return abTipInt2(base, "skill_performer_hp_range",
                     (int)(lo * 100.0f + 0.5f), (int)(hi * 100.0f + 0.5f), out, outsz);
}

static bool skillHpGateUnmet(uintptr_t skill, uintptr_t hero) {
    if (!skill || !hero) return false;
    uint8_t declared = 0;
    if (!safeReadU8(skill + SKILL_HP_RANGE_SET_OFF, &declared) || !declared) return false;
    float lo = 0.0f, hi = 0.0f, cur = 0.0f, max = 0.0f;
    uint32_t mask = 0;
    if (!abReadF32(skill + SKILL_HP_RANGE_MIN_OFF, &lo)) return false;
    if (!abReadF32(skill + SKILL_HP_RANGE_MAX_OFF, &hi)) return false;
    if (!(hi >= lo)) return false;
    if (!abReadF32(hero + ACTOR_CUR_HP_OFF, &cur)) return false;
    if (!safeReadU32(hero + ACTOR_HP_MASK_OFF, &mask)) return false;
    if (!abStatValue(hero + ACTOR_STAT_OFF, mask, true, &max) || max <= 0.0f) return false;
    float ratio = cur / max;
    return ratio < lo || ratio > hi;
}

static bool skillUsedUp(uintptr_t skill, uintptr_t hero) {
    if (!skill || !hero) return false;
    int limit = 0, left = -1;
    if (!abSkillBattleUses(skill, hero, &limit, &left)) return false;
    return left == 0;
}

static bool skillUnavailable(uintptr_t skill, uintptr_t perf) {
    return skillOutOfPosition(skill, perf) || skillUsedUp(skill, perf) ||
           skillHpGateUnmet(skill, perf);
}

static void abSkillLimitsText(uintptr_t base, uintptr_t skill, uintptr_t hero, char* out, int outsz) {
    out[0] = 0;
    char buf[256];

    int limit = 0, left = -1;
    if (abSkillBattleUses(skill, hero, &limit, &left)) {
        bool got = (left < 0)
            ? abTipInt(base, "skill_battle_limit_out_of_battle", limit, buf, sizeof buf)
            : abTipInt(base, "skill_battle_limit_in_battle",     left,  buf, sizeof buf);
        if (got) abAppend(out, outsz, buf);
    }

    if (abSkillHpReqText(base, skill, buf, sizeof buf)) abAppend(out, outsz, buf);
}

static void abSkillTooltip(uintptr_t base, uintptr_t skill, char* out, int outsz) {
    out[0] = 0;
    char buf[256];

    float mult = 100.0f;
    uint32_t m = 0;
    if (safeReadU32(base + SKILL_PCT_MULT_RVA, &m)) memcpy(&mult, &m, 4);

    uint8_t noInfo = 0, healBranch = 0, plain = 0;
    safeReadU8(skill + SKILL_NOINFO_OFF, &noInfo);
    safeReadU8(skill + SKILL_HEALFLAG_OFF, &healBranch);
    safeReadU8(skill + SKILL_PLAIN_OFF, &plain);

    int32_t mv = 0;
    if (safeReadU32(skill + SKILL_MOVE_BACK_OFF, (uint32_t*)&mv) && mv > 0 &&
        abTipInt(base, "str_skill_tooltip_back", mv, buf, sizeof buf)) abAppend(out, outsz, buf);
    if (safeReadU32(skill + SKILL_MOVE_FWD_OFF, (uint32_t*)&mv) && mv > 0 &&
        abTipInt(base, "str_skill_tooltip_forward", mv, buf, sizeof buf)) abAppend(out, outsz, buf);

    if (!noInfo && !healBranch && !plain) {
        float v = 0.0f; uint32_t raw = 0;
        if (safeReadU32(skill + SKILL_ACC_OFF, &raw)) { memcpy(&v, &raw, 4);
            if (v != 0.0f && abTipFloat(base, "str_skill_tooltip_acc_base", v * mult, buf, sizeof buf))
                abAppend(out, outsz, buf); }
        if (safeReadU32(skill + SKILL_DMG_OFF, &raw)) { memcpy(&v, &raw, 4);
            if (v != 0.0f && abTipFloat(base, "str_skill_tooltip_dmg_mod", v * mult, buf, sizeof buf))
                abAppend(out, outsz, buf); }
        if (safeReadU32(skill + SKILL_CRIT_OFF, &raw)) { memcpy(&v, &raw, 4);
            if (v != 0.0f && abTipFloat(base, "str_skill_tooltip_crit_mod", v * mult, buf, sizeof buf))
                abAppend(out, outsz, buf); }
    } else if (!noInfo && healBranch) {
        int32_t lo = 0, hi = 0;
        if (safeReadU32(skill + SKILL_HEAL_MIN_OFF, (uint32_t*)&lo) &&
            safeReadU32(skill + SKILL_HEAL_MAX_OFF, (uint32_t*)&hi) && hi > 0) {
            uint32_t tgt = 0; safeReadU32(skill + SKILL_TGTMASK_OFF, &tgt);
            const char* hk = (tgt & 0x100) ? "skill_party_heal_format" : "skill_heal_format";
            if (abTipInt2(base, hk, lo, hi, buf, sizeof buf)) abAppend(out, outsz, buf);
        }
    }

    struct { uintptr_t off; const char* key; } kFlagLines[] = {
        { SKILL_IGN_PROT_OFF,     "skill_ignore_protection" },
        { SKILL_IGN_GUARD_OFF,    "skill_ignore_guard"      },
        { SKILL_IGN_STLTH_OFF,    "skill_ignore_stealth"    },
        { SKILL_IGN_DDOOR_OFF,    "skill_ignore_deathsdoor" },
        { SKILL_REQ_BURNING_OFF,  "skill_requires_burning"  },  // "Requires Burning Target"
        { SKILL_COPY_BURN_OFF,    "skill_copy_burn_behind"  },  // hits behind, copies the burn across
    };
    for (int i = 0; i < (int)(sizeof kFlagLines / sizeof kFlagLines[0]); i++) {
        uint8_t f = 0;
        if (safeReadU8(skill + kFlagLines[i].off, &f) && f &&
            abTipPlain(base, kFlagLines[i].key, buf, sizeof buf)) abAppend(out, outsz, buf);
    }

    float perBurn = 0.0f;
    if (abReadF32(skill + SKILL_DMG_PER_BURN_OFF, &perBurn) && perBurn > 0.0f &&
        abTipInt(base, "str_skill_tooltip_dmg_per_burn_stack", (int)(perBurn * mult),
                 buf, sizeof buf))
        abAppend(out, outsz, buf);
}

// ---- Skill ADDITIONAL EFFECTS ----

bool abReadF32(uintptr_t addr, float* out) {
    uint32_t raw = 0;
    if (!safeReadU32(addr, &raw)) return false;
    memcpy(out, &raw, 4);
    return true;
}
static bool abReadF64(uintptr_t addr, double* out) {
    int64_t raw = 0;
    if (!safeReadI64(addr, &raw)) return false;
    memcpy(out, &raw, 8);
    return true;
}

bool abTipStrN(uintptr_t base, const char* key, const char* a, const char* b, const char* c,
                      char* out, int outsz) {
    out[0] = 0;
    char fmt[256];
    if (!resolveKey(base, key, fmt, sizeof fmt) || !fmt[0]) return false;
    const char* expect = c ? "sss" : (b ? "ss" : "s");
    if (!abFormatMatches(fmt, expect)) { logLine("actionbar eff: \"%s\" fmt mismatch \"%s\"", key, fmt); return false; }
    if (c)      _snprintf(out, outsz, fmt, a, b, c);
    else if (b) _snprintf(out, outsz, fmt, a, b);
    else        _snprintf(out, outsz, fmt, a);
    out[outsz - 1] = 0; abStripMarkup(out);
    return out[0] != 0;
}

void abEffGlueFallback(char* out, int outsz, const char* a, const char* b, const char* c) {
    out[0] = 0;
    abAppend(out, outsz, a);
    abAppend(out, outsz, b);
    abAppend(out, outsz, c);
}

// ---- THE GROUP CONTEXT (The Fire's Edge) ----
static const char* g_abEffGroupStem = nullptr;
static bool        g_abEffGroupHit  = false;

static void abEffAdd(uintptr_t base, uintptr_t skill, uintptr_t effect,
                     char buckets[AB_EFF_BUCKETS][AB_EFF_BUCKET_SZ], int bucket,
                     const char* mainText, const char* extra, bool baseChance, bool condensed) {
    if (bucket < 0 || bucket >= AB_EFF_BUCKETS) return;
    if (!mainText || !mainText[0]) return;

    char chanceStr[128] = { 0 };
    float chance = 0.0f, one = 1.0f, mult = 100.0f;
    abReadF32(effect + EFF_CHANCE_OFF, &chance);
    abReadF32(base + EFF_ONE_RVA, &one);
    abReadF32(base + SKILL_PCT_MULT_RVA, &mult);
    uint8_t healFlag = 0;
    safeReadU8(skill + SKILL_HEALFLAG_OFF, &healFlag);
    if (chance < one || (baseChance && !healFlag)) {
        const char* ck = baseChance ? "effect_base_chance_format" : "effect_chance_format";
        abTipInt(base, ck, (int)roundf(chance * mult), chanceStr, sizeof chanceStr);
    }

    char line[AB_EFF_BUCKET_SZ];
    if (!extra || !extra[0]) {
        if (!abTipStrN(base, "effect_tooltip_format", mainText, chanceStr, nullptr, line, sizeof line))
            abEffGlueFallback(line, sizeof line, mainText, chanceStr, nullptr);
    } else if (!chanceStr[0]) {
        if (!abTipStrN(base, "effect_tooltip_format", mainText, extra, nullptr, line, sizeof line))
            abEffGlueFallback(line, sizeof line, mainText, extra, nullptr);
    } else {
        uint32_t perLine = 0;
        safeReadU32(skill + SKILL_PERLINE_OFF, &perLine);
        const char* k = (perLine != 0 || condensed)
            ? "effect_tooltip_additional_description_condensed_format"
            : "effect_tooltip_additional_description_format";
        if (!abTipStrN(base, k, mainText, chanceStr, extra, line, sizeof line))
            abEffGlueFallback(line, sizeof line, mainText, chanceStr, extra);
    }

    // ---- THE REQUIREMENT WRAPPERS ----
    char wrapped[AB_EFF_BUCKET_SZ];
    uint8_t reqBurning = 0, reqNotBurning = 0, reqKill = 0;
    safeReadU8(effect + EFF_REQ_BURNING_OFF,     &reqBurning);
    safeReadU8(effect + EFF_REQ_NOT_BURNING_OFF, &reqNotBurning);
    if (reqBurning || reqNotBurning) {
        const char* rk = reqBurning ? "effect_tooltip_requires_burning_target_format"
                                    : "effect_tooltip_requires_not_burning_target_format";
        if (abTipStrN(base, rk, line, nullptr, nullptr, wrapped, sizeof wrapped) && wrapped[0])
            _snprintf(line, sizeof line, "%s", wrapped);
        else
            logLine("actionbar eff: \"%s\" did not resolve — \"%s\" goes out unqualified", rk, line);
    }
    if (safeReadU8(effect + EFF_REQ_KILL_OFF, &reqKill) && reqKill) {
        if (abTipStrN(base, "effect_tooltip_requires_kill_target_format", line, nullptr, nullptr,
                      wrapped, sizeof wrapped) && wrapped[0])
            _snprintf(line, sizeof line, "%s", wrapped);
        else
            logLine("actionbar eff: \"effect_tooltip_requires_kill_target_format\" did not "
                    "resolve — \"%s\" goes out unqualified", line);
    }
    line[sizeof line - 1] = 0;

    // ---- THE TRINKET GROUP HEADER ----
    if (g_abEffGroupStem && g_abEffGroupStem[0]) {
        char gkey[192], ghdr[192];
        strncpy(gkey, g_abEffGroupStem, sizeof gkey - 1);
        gkey[sizeof gkey - 1] = 0;
        uint8_t onHit = 0, onMiss = 0, isTrig = 0;
        if (g_abEffGroupHit) {
            if (safeReadU8(effect + EFF_ON_HIT_OFF,  &onHit)  && onHit)
                strncat(gkey, "_hit",  sizeof gkey - strlen(gkey) - 1);
            if (safeReadU8(effect + EFF_ON_MISS_OFF, &onMiss) && onMiss)
                strncat(gkey, "_miss", sizeof gkey - strlen(gkey) - 1);
        }
        if (safeReadU8(effect + EFF_IS_TRIGGER_OFF, &isTrig) && isTrig)
            strncat(gkey, "_trigger_effect", sizeof gkey - strlen(gkey) - 1);

        char headed[AB_EFF_BUCKET_SZ];
        if (abTipPlain(base, gkey, ghdr, sizeof ghdr) && ghdr[0]) {
            _snprintf(headed, sizeof headed, "%s %s", ghdr, line);
            headed[sizeof headed - 1] = 0;
            _snprintf(line, sizeof line, "%s", headed);
        } else {
            logLine("actionbar eff: trinket group key \"%s\" did not resolve — \"%s\" goes out "
                    "without its event header", gkey, line);
        }
    }
    uint8_t inInv = 0;
    if (safeReadU8(effect + EFF_IN_INVENTORY_OFF, &inInv) && inInv) {
        char inv[192], headed[AB_EFF_BUCKET_SZ];
        if (abTipPlain(base, "effect_is_in_inventory", inv, sizeof inv) && inv[0]) {
            _snprintf(headed, sizeof headed, "%s %s", inv, line);
            headed[sizeof headed - 1] = 0;
            _snprintf(line, sizeof line, "%s", headed);
        }
    }
    line[sizeof line - 1] = 0;

    abAppend(buckets[bucket], AB_EFF_BUCKET_SZ, line);
}

// Add a plain (unformatted) key, the shape most of the effect fields use.
static void abEffAddKey(uintptr_t base, uintptr_t skill, uintptr_t effect,
                        char buckets[AB_EFF_BUCKETS][AB_EFF_BUCKET_SZ], int bucket,
                        const char* key, bool baseChance) {
    char t[192];
    if (abTipPlain(base, key, t, sizeof t))
        abEffAdd(base, skill, effect, buckets, bucket, t, nullptr, baseChance, false);
}

void csFlattenLines(char* s, int outsz);

struct AbBuffVec { uintptr_t beg, end, cap; };
typedef void  (*AbBuffListBuildFn)(AbBuffVec* outVec, uintptr_t effect);
typedef unsigned char (*AbBuffGateFn)(const void* rec);
typedef char* (*AbBuffRenderOneFn)(char* out, const void* rec, long long alt);
typedef void  (*AbBuffListFreeFn)(AbBuffVec* vec);

static void abEffBuffLines(uintptr_t base, uintptr_t skill, uintptr_t effect,
                           char buckets[AB_EFF_BUCKETS][AB_EFF_BUCKET_SZ], int bucket) {
    AbBuffVec vec = { 0, 0, 0 };
    AbBuffListBuildFn build = reinterpret_cast<AbBuffListBuildFn>(base + EFF_BUFFLIST_BUILD_RVA);
    AbBuffGateFn      gate  = reinterpret_cast<AbBuffGateFn>(base + CAMP_BUFF_HASDESC_RVA);
    AbBuffRenderOneFn rend  = reinterpret_cast<AbBuffRenderOneFn>(base + BUFF_RENDER_ONE_RVA);
    AbBuffListFreeFn  destroy = reinterpret_cast<AbBuffListFreeFn>(base + EFF_BUFFLIST_FREE_RVA);

    bool built = false;
    __try { build(&vec, effect); built = true; }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        logLine("actionbar eff: buff-list builder faulted on effect=%p", (void*)effect);
    }

    if (built && vec.beg > 0x10000 && vec.end >= vec.beg) {
        uintptr_t span = vec.end - vec.beg;
        if (span % EFF_BUFFLIST_STRIDE) {
            logLine("actionbar eff: buff list span 0x%llx is not whole entries — skipped",
                    (unsigned long long)span);
        } else {
            int n = (int)(span / EFF_BUFFLIST_STRIDE);
            if (n > EFF_BUFFLIST_MAX) {
                logLine("actionbar eff: buff list claims %d entries — implausible, skipped", n);
                n = 0;
            }
            for (int i = 0; i < n; i++) {
                const void* rec = (const void*)(vec.beg + (uintptr_t)i * EFF_BUFFLIST_STRIDE);
                char line[CAMP_BUFF_DESC_BUF];
                line[0] = 0;
                bool draws = false, faulted = false;
                __try {
                    if (gate(rec)) { rend(line, rec, 0); draws = true; }
                } __except (EXCEPTION_EXECUTE_HANDLER) { faulted = true; }
                if (faulted) {
                    logLine("actionbar eff: buff renderer faulted on entry %d of %d", i, n);
                    continue;                          // the rest of the list is still worth trying
                }
                if (!draws || !line[0]) continue;      // gated off: the game draws nothing either
                line[sizeof line - 1] = 0;
                abStripMarkup(line);
                csFlattenLines(line, sizeof line);
                if (line[0])
                    abEffAdd(base, skill, effect, buckets, bucket, line, nullptr,
                             bucket != 6 && bucket != 2, false);
            }
        }
    }

    __try { destroy(&vec); }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        logLine("actionbar eff: buff-list destroy faulted (beg=%p)", (void*)vec.beg);
    }
}

static bool abSummonNameForHash(uintptr_t base, uint32_t hash, char* out, int outsz) {
    out[0] = 0;
    uint8_t altFlag = 0;
    safeReadU8(base + CAMP_BUFFREG_FLAG_RVA, &altFlag);
    uintptr_t vecRva = altFlag ? MONCLASS_VEC_ALT_RVA : MONCLASS_VEC_RVA;

    uintptr_t beg = 0, end = 0;
    if (!safeReadPtr(base + vecRva, &beg) || !safeReadPtr(base + vecRva + 8, &end)) return false;
    if (beg <= 0x10000 || end < beg) return false;
    uintptr_t n = (end - beg) / sizeof(uintptr_t);
    if (n > (uintptr_t)MONCLASS_MAX) {
        logLine("actionbar eff: monster class vector claims %llu entries — implausible, skipped",
                (unsigned long long)n);
        return false;
    }
    for (uintptr_t i = 0; i < n; i++) {
        uintptr_t cls = 0;
        if (!safeReadPtr(beg + i * sizeof(uintptr_t), &cls) || cls <= 0x10000) continue;
        uint32_t h = 0;
        if (!safeReadU32(cls + MONCLASS_HASH_OFF, &h) || h != hash) continue;

        uint8_t latched = 0;
        if (safeReadU8(cls + HEROCLASS_DISP_LATCH_OFF, &latched) && latched &&
            safeReadCStr(cls + HEROCLASS_DISP_OFF, out, outsz) && out[0]) return true;
        out[0] = 0;
        if (safeReadCStr(cls + HEROCLASS_ID_OFF, out, outsz) && abPlausibleName(out)) return true;
        out[0] = 0;
        return false;
    }
    return false;
}

static void abEffOne(uintptr_t base, uintptr_t skill, uintptr_t effect,
                     char buckets[AB_EFF_BUCKETS][AB_EFF_BUCKET_SZ], bool partyHeal) {
    uint8_t display = 0;
    if (!safeReadU8(effect + EFF_DISPLAY_OFF, &display) || !display) return;   // the game's own gate

    char t[192], extra[192];
    float f = 0.0f, f2 = 0.0f;
    int32_t i32 = 0, i2 = 0;

    bool haveTorch = false;
    if (abReadF32(effect + EFF_TORCH_OFF, &f) && f > 0.0f) haveTorch = true;
    else if (abReadF32(effect + EFF_TORCH_NEG_OFF, &f2) && f2 > 0.0f) { f = -f2; haveTorch = true; }
    if (haveTorch && abTipInt(base, "effect_tooltip_torch_format", (int)f, t, sizeof t))
        abEffAdd(base, skill, effect, buckets, 1, t, nullptr, false, false);

    // The category -> which header the effect is filed under.
    int bucket = 1;
    uint8_t noHeader = 0;
    safeReadU8(effect + EFF_NOHEADER_OFF, &noHeader);
    if (noHeader) {
        bucket = 2;
    } else if (safeReadU32(effect + EFF_CATEGORY_OFF, (uint32_t*)&i32)) {
        switch (i32) {
            case 0: bucket = 6; break;   // self
            case 1: bucket = 0; break;
            case 2: bucket = 7; break;   // party
            case 3: bucket = 8; break;   // party, other
            case 4: bucket = 3; break;   // target group
            case 5: bucket = 4; break;   // target group, other
            case 6: bucket = 5; break;   // target enemy group
            case 7: bucket = 1; break;
            default:
                logLine("actionbar eff: no display type for category %d -> unheaded bucket", i32);
                bucket = 1; break;
        }
    }

    abEffBuffLines(base, skill, effect, buckets, bucket);

    // Stress: dealt, or healed — the game shows whichever is set, dealt first.
    if (abReadF32(effect + EFF_STRESS_OFF, &f)) {
        float heal = 0.0f;
        abReadF32(effect + EFF_STRESS_HEAL_OFF, &heal);
        if (f > 0.0f) {
            if (abTipInt(base, "effect_tooltip_stress_format", (int)f, t, sizeof t))
                abEffAdd(base, skill, effect, buckets, bucket, t, nullptr, false, false);
        } else if (heal > 0.0f) {
            if (abTipInt(base, "effect_tooltip_stress_heal_format", (int)heal, t, sizeof t))
                abEffAdd(base, skill, effect, buckets, bucket, t, nullptr, false, false);
        }
    }

    if (abReadF32(effect + EFF_STUN_OFF, &f) && f > 0.0f)
        abEffAddKey(base, skill, effect, buckets, bucket, "effect_tooltip_stun", true);
    if (abReadF32(effect + EFF_UNSTUN_OFF, &f) && f > 0.0f)
        abEffAddKey(base, skill, effect, buckets, bucket, "effect_tooltip_unstun", false);

    if (abReadF32(effect + EFF_DAZE_OFF, &f) && f > 0.0f) {
        extra[0] = 0;
        abTipInt(base, "effect_tooltip_daze_format", (int)f, extra, sizeof extra);
        if (abTipPlain(base, "effect_tooltip_daze", t, sizeof t))
            abEffAdd(base, skill, effect, buckets, bucket, t, extra, true, true);
    }
    if (abReadF32(effect + EFF_UNDAZE_OFF, &f) && f > 0.0f)
        abEffAddKey(base, skill, effect, buckets, bucket, "effect_tooltip_undaze", false);

    if (safeReadU32(effect + EFF_TAG_OFF, (uint32_t*)&i32) && i32 > 0)
        abEffAddKey(base, skill, effect, buckets, bucket, "effect_tooltip_tag", false);

    if (safeReadU32(effect + EFF_GUARD_OFF, (uint32_t*)&i32) && i32 > 0) {
        uint8_t performer = 0;
        safeReadU8(effect + EFF_GUARD_PERF_OFF, &performer);
        abEffAddKey(base, skill, effect, buckets, bucket,
                    performer ? "effect_tooltip_guard_performer" : "effect_tooltip_guard", false);
    }
    if (safeReadU32(effect + EFF_CLEAR_GUARDED_OFF, (uint32_t*)&i32) && i32 > 0)
        abEffAddKey(base, skill, effect, buckets, bucket, "effect_clear_guarded", false);
    if (safeReadU32(effect + EFF_CLEAR_GUARDING_OFF, (uint32_t*)&i32) && i32 > 0)
        abEffAddKey(base, skill, effect, buckets, bucket, "effect_clear_guarding", false);
    if (safeReadU32(effect + EFF_RIPOSTE_OFF, (uint32_t*)&i32) && i32 > 0)
        abEffAddKey(base, skill, effect, buckets, bucket, "effect_tooltip_riposte", false);
    if (safeReadU32(effect + EFF_UNTAG_OFF, (uint32_t*)&i32) && i32 > 0)
        abEffAddKey(base, skill, effect, buckets, bucket, "effect_tooltip_untag", false);

    // Cure: poison and bleed share one line when both are cured.
    safeReadU32(effect + EFF_CURE_POISON_OFF, (uint32_t*)&i32);
    safeReadU32(effect + EFF_CURE_BLEED_OFF, (uint32_t*)&i2);
    if (i32 > 0)      abEffAddKey(base, skill, effect, buckets, bucket,
                                  i2 > 0 ? "effect_tooltip_cure" : "effect_tooltip_cure_poison", false);
    else if (i2 > 0)  abEffAddKey(base, skill, effect, buckets, bucket, "effect_tooltip_cure_bleed", false);

    if (safeReadU32(effect + EFF_CURE_BURN_OFF, (uint32_t*)&i32) && i32 > 0)
        abEffAddKey(base, skill, effect, buckets, bucket, "effect_tooltip_cure_burn", false);
    if (safeReadU32(effect + EFF_CURE_DISEASE_OFF, (uint32_t*)&i32) && i32 > 0)
        abEffAddKey(base, skill, effect, buckets, bucket, "effect_tooltip_cure_disease", false);

    int32_t dur = 0;
    safeReadU32(effect + EFF_DOT_DURATION_OFF, (uint32_t*)&dur);

    if (safeReadU32(effect + EFF_DOT_POISON_OFF, (uint32_t*)&i32) && i32 > 0) {
        extra[0] = 0;
        abTipInt2(base, "effect_tooltip_dot_format", i32, dur, extra, sizeof extra);
        if (abTipPlain(base, "effect_tooltip_dot_poison", t, sizeof t))
            abEffAdd(base, skill, effect, buckets, bucket, t, extra, true, false);
    }
    if (safeReadU32(effect + EFF_DOT_BLEED_OFF, (uint32_t*)&i32) && i32 > 0) {
        extra[0] = 0;
        abTipInt2(base, "effect_tooltip_dot_format", i32, dur, extra, sizeof extra);
        if (abTipPlain(base, "effect_tooltip_dot_bleed", t, sizeof t))
            abEffAdd(base, skill, effect, buckets, bucket, t, extra, true, false);
    }
    if (safeReadU32(effect + EFF_DOT_BURN_OFF, (uint32_t*)&i32) && i32 > 0) {
        uint8_t irresistible = 0;
        safeReadU8(effect + EFF_DOT_IRRESIST_OFF, &irresistible);
        extra[0] = 0;
        abTipInt(base, "effect_tooltip_dot_burn_format", i32, extra, sizeof extra);
        if (abTipPlain(base, "effect_tooltip_dot_burn", t, sizeof t))
            abEffAdd(base, skill, effect, buckets, bucket, t, extra, irresistible == 0, false);
    }

    if (safeReadU32(effect + EFF_CBURN_OFF, (uint32_t*)&i32) && i32 > 0) {
        abEffAddKey(base, skill, effect, buckets, bucket, "effect_tooltip_controlled_burn", false);
        if (abTipInt(base, "effect_tooltip_controlled_burn_trigger", i32, t, sizeof t))
            abEffAdd(base, skill, effect, buckets, bucket, t, nullptr, false, false);
    }

    // Stress-over-time has no separate label: the formatted amount IS the line.
    if (safeReadU32(effect + EFF_DOT_STRESS_OFF, (uint32_t*)&i32) && i32 > 0 &&
        abTipInt2(base, "effect_tooltip_stress_dot_format", i32, dur, t, sizeof t))
        abEffAdd(base, skill, effect, buckets, bucket, t, nullptr, true, false);
    if (safeReadU32(effect + EFF_CLEAR_DOT_STRESS_OFF, (uint32_t*)&i32) && i32 > 0)
        abEffAddKey(base, skill, effect, buckets, bucket, "effect_tooltip_clear_stress_dot", false);
    if (safeReadU32(effect + EFF_CURE_DEBUFF_OFF, (uint32_t*)&i32) && i32 != 0)
        abEffAddKey(base, skill, effect, buckets, bucket, "effect_tooltip_cure_debuff", false);
    if (safeReadU32(effect + EFF_SHUFFLE_DOT_OFF, (uint32_t*)&i32) && i32 > 0 &&
        abTipInt(base, "effect_tooltip_shuffle_dot_format", dur, t, sizeof t))
        abEffAdd(base, skill, effect, buckets, bucket, t, nullptr, true, false);
    if (safeReadU32(effect + EFF_HEAL_DOT_OFF, (uint32_t*)&i32) && i32 > 0 &&
        abTipInt2(base, partyHeal ? "effect_tooltip_hp_heal_party_dot_format"
                                  : "effect_tooltip_hp_heal_dot_format", i32, dur, t, sizeof t))
        abEffAdd(base, skill, effect, buckets, bucket, t, nullptr, true, false);

    if (safeReadU32(effect + EFF_SHUFFLE_OFF, (uint32_t*)&i32)) {
        if (i32 == 1) abEffAddKey(base, skill, effect, buckets, bucket, "effect_tooltip_shuffle_single", false);
        if (i32 == 2) abEffAddKey(base, skill, effect, buckets, bucket, "effect_tooltip_shuffle_group", false);
    }

    uint8_t b8 = 0;
    if (safeReadU8(effect + EFF_DISEASE_OFF, &b8) && b8)
        abEffAddKey(base, skill, effect, buckets, bucket, "effect_tooltip_disease", true);

    {
        uintptr_t sbeg = 0, send = 0;
        if (safeReadPtr(effect + EFF_SUMMON_BEG_OFF, &sbeg) &&
            safeReadPtr(effect + EFF_SUMMON_END_OFF, &send) &&
            sbeg > 0x10000 && send > sbeg) {
            uintptr_t span = send - sbeg;
            int ns = (span % EFF_SUMMON_STRIDE) ? 0 : (int)(span / EFF_SUMMON_STRIDE);
            if (ns > EFF_SUMMON_MAX) {
                logLine("actionbar eff: summon list claims %d entries — implausible, skipped", ns);
                ns = 0;
            }
            char joined[256];
            joined[0] = 0;
            for (int si = 0; si < ns; si++) {
                uint32_t hash = 0;
                if (!safeReadU32(sbeg + (uintptr_t)si * EFF_SUMMON_STRIDE, &hash)) continue;
                char mname[0x41];
                if (!abSummonNameForHash(base, hash, mname, sizeof mname)) {
                    logLine("actionbar eff: summon hash 0x%08x matched no monster class", hash);
                    continue;
                }
                abStripMarkup(mname);
                if (!mname[0]) continue;
                if (joined[0]) strncat(joined, "/", sizeof joined - strlen(joined) - 1);
                strncat(joined, mname, sizeof joined - strlen(joined) - 1);
            }
            if (joined[0] &&
                abTipStrN(base, "effect_tooltip_summon_format", joined, nullptr, nullptr, t, sizeof t))
                abEffAdd(base, skill, effect, buckets, bucket, t, nullptr, false, false);
        }
    }

    if (abReadF32(effect + EFF_HEAL_OFF, &f) && f > 0.0f &&
        abTipInt(base, "effect_tooltip_heal_format", (int)f, t, sizeof t))
        abEffAdd(base, skill, effect, buckets, bucket, t, nullptr, false, false);
    if (abReadF32(effect + EFF_HEAL_PCT_OFF, &f) && f > 0.0f) {
        double pct = 100.0;
        abReadF64(base + EFF_PCT_D_RVA, &pct);
        if (abTipInt(base, "effect_tooltip_heal_percent_format", (int)((double)f * pct), t, sizeof t))
            abEffAdd(base, skill, effect, buckets, bucket, t, nullptr, false, false);
    }

    // One signed field: positive moves the target back, negative moves it forward.
    if (safeReadU32(effect + EFF_MOVE_OFF, (uint32_t*)&i32) && i32 != 0) {
        const char* mk = i32 > 0 ? "effect_tooltip_move_backward" : "effect_tooltip_move_forward";
        if (abTipInt(base, mk, i32 > 0 ? i32 : -i32, t, sizeof t))
            abEffAdd(base, skill, effect, buckets, bucket, t, nullptr, true, false);
    }

    // Stance / transform ("Set mode: Berserk").
    char modeId[64] = { 0 };
    if (safeReadCStr(effect + EFF_ACTOR_MODE_OFF, modeId, sizeof modeId) && modeId[0] &&
        abPlausibleName(modeId)) {
        char mkey[96];
        _snprintf(mkey, sizeof mkey, "actor_mode_name_%s", modeId);
        mkey[sizeof mkey - 1] = 0;
        char modeName[128];
        if (abTipPlain(base, mkey, modeName, sizeof modeName) &&
            abTipStrN(base, "effect_tooltip_set_actor_mode_format", modeName, nullptr, nullptr, t, sizeof t))
            abEffAdd(base, skill, effect, buckets, bucket, t, nullptr, false, false);
    }

    if (safeReadU32(effect + EFF_HEALTH_BLOCKS_OFF, (uint32_t*)&i32) && i32 != 0 &&
        abTipInt(base, "effect_health_damage_blocks_format", i32, t, sizeof t))
        abEffAdd(base, skill, effect, buckets, bucket, t, nullptr, false, false);

    if (safeReadU32(effect + EFF_STEALTH_OFF, (uint32_t*)&i32) && i32 != 0) {
        if (bucket == 0) bucket = 9;
        if (abTipInt(base, "effect_stealth_format", dur, t, sizeof t))
            abEffAdd(base, skill, effect, buckets, bucket, t, nullptr, false, false);
    }
    if (safeReadU32(effect + EFF_UNSTEALTH_OFF, (uint32_t*)&i32) && i32 != 0)
        abEffAddKey(base, skill, effect, buckets, bucket, "effect_unstealth_format", false);

    if (abReadF32(effect + EFF_HP_DAMAGE_OFF, &f) && f > 0.0f &&
        abTipInt(base, "effect_health_damage_format", (int)f, t, sizeof t))
        abEffAdd(base, skill, effect, buckets, bucket, t, nullptr, false, false);

    if (safeReadU32(effect + EFF_CLEAR_VIRTUE_OFF, (uint32_t*)&i32) && i32 > 0)
        abEffAddKey(base, skill, effect, buckets, bucket, "effect_clear_virtue", false);
    if (safeReadU8(effect + EFF_REFRESH_SKILL_OFF, &b8) && b8)
        abEffAddKey(base, skill, effect, buckets, bucket, "effect_refresh_skill_uses", false);

    // ---- The Fire's Edge: the effect kinds the DLC added ----
    if (safeReadU32(effect + EFF_ACTIONS_OFF, (uint32_t*)&i32) && i32 != 0 &&
        abTipInt(base, "effect_tooltip_actions_format", i32, t, sizeof t))
        abEffAdd(base, skill, effect, buckets, bucket, t, nullptr, false, false);

    int trigLo = 0, trigHi = 0;
    safeReadU32(effect + EFF_TRIG_INC_LO_OFF, (uint32_t*)&trigLo);
    safeReadU32(effect + EFF_TRIG_INC_HI_OFF, (uint32_t*)&trigHi);
    if ((trigLo > 0 || trigHi > 0) &&
        abTipInt2(base, "effect_trigger_limit_increase", trigLo, trigHi, t, sizeof t))
        abEffAdd(base, skill, effect, buckets, bucket, t, nullptr, false, false);

    if (safeReadU32(effect + EFF_QUIRK_POS_PCT_OFF, (uint32_t*)&i32) && i32 >= 0 &&
        abTipInt(base, "effect_gain_random_quirk_positive_percentage", i32, t, sizeof t))
        abEffAdd(base, skill, effect, buckets, bucket, t, nullptr, false, false);
    if (safeReadU8(effect + EFF_QUIRK_NEG_OFF, &b8) && b8)
        abEffAddKey(base, skill, effect, buckets, bucket, "effect_gain_random_quirk_negative", false);
    if (safeReadU8(effect + EFF_GAIN_RND_TRINKET_OFF, &b8) && b8)
        abEffAddKey(base, skill, effect, buckets, bucket, "effect_gain_random_trinket", false);

    char gainId[72];
    gainId[0] = 0;
    if (safeReadCStr(effect + EFF_GAIN_TRINKET_OFF, gainId, sizeof gainId) && gainId[0]) {
        char nkey[192], gname[192];
        _snprintf(nkey, sizeof nkey, "str_inventory_title_%s%s", "trinket", gainId);
        nkey[sizeof nkey - 1] = 0;
        if (abTipPlain(base, nkey, gname, sizeof gname) && gname[0] &&
            abTipStrN(base, "effect_gain_trinket", gname, nullptr, nullptr, t, sizeof t))
            abEffAdd(base, skill, effect, buckets, bucket, t, nullptr, false, false);
        else
            logLine("actionbar eff: gain-trinket \"%s\" (key \"%s\") did not resolve — line omitted",
                    gainId, nkey);
    }

    if (safeReadU8(effect + EFF_TOWN_EVENT_OFF, &b8) && b8 == 1)   // == 1, the game's own test
        abEffAddKey(base, skill, effect, buckets, bucket, "effect_guaranteed_town_event", false);
    if (safeReadU8(effect + EFF_DESTROY_TRINKET_OFF, &b8) && b8)
        abEffAddKey(base, skill, effect, buckets, bucket, "effect_destroy_trinket", false);
}

void abEffRenderVector(uintptr_t base, uintptr_t skill, uintptr_t vecAddr, bool partyHeal,
                              char* out, int outsz) {
    out[0] = 0;
    uintptr_t beg = 0, end = 0;
    if (!safeReadPtr(vecAddr, &beg) || !safeReadPtr(vecAddr + 8, &end)) return;
    if (!beg || end < beg) return;
    size_t n = (end - beg) / sizeof(uintptr_t);
    if (n == 0) return;
    if (n > AB_EFF_MAX_PER_VEC) {
        logLine("actionbar eff: vector claims %zu effects — implausible, skipped", n);
        return;
    }

    char buckets[AB_EFF_BUCKETS][AB_EFF_BUCKET_SZ];
    for (int i = 0; i < AB_EFF_BUCKETS; i++) buckets[i][0] = 0;

    for (size_t i = 0; i < n; i++) {
        uintptr_t eff = 0;
        if (!safeReadPtr(beg + i * sizeof(uintptr_t), &eff) || eff <= 0x10000) continue;
        abEffOne(base, skill, eff, buckets, partyHeal);
    }

    for (int i = 0; i < AB_EFF_BUCKETS; i++) {
        if (!buckets[i][0]) continue;
        char hdr[192];
        if (kEffBucketKey[i] && abTipPlain(base, kEffBucketKey[i], hdr, sizeof hdr))
            abAppend(out, outsz, hdr);
        abAppend(out, outsz, buckets[i]);
    }
}

void abEffTrinketGroup(uintptr_t base, uintptr_t vecAddr, const char* groupStem, bool hitFlag,
                       bool partyHeal, char* out, int outsz) {
    g_abEffGroupStem = groupStem;
    g_abEffGroupHit  = hitFlag;
    abEffRenderVector(base, 0, vecAddr, partyHeal, out, outsz);
    g_abEffGroupStem = nullptr;
    g_abEffGroupHit  = false;
}

static uintptr_t abEffMapFind(uintptr_t skill, uint32_t key) {
    uintptr_t head = 0;
    if (!safeReadPtr(skill + SKILL_EFFMAP_OFF, &head) || head <= 0x10000) return 0;
    uintptr_t node = 0;
    if (!safeReadPtr(head + RBNODE_PARENT_OFF, &node) || node <= 0x10000) return 0;   // the root

    uintptr_t bound = head;
    bool reachedLeaf = false;
    for (int guard = 0; guard < 64; guard++) {
        uint8_t nil = 1;
        if (!safeReadU8(node + RBNODE_ISNIL_OFF, &nil)) return 0;
        if (nil) { reachedLeaf = true; break; }
        uint32_t nk = 0;
        if (!safeReadU32(node + RBNODE_KEY_OFF, &nk)) return 0;
        uintptr_t next = 0;
        if (nk < key) {
            if (!safeReadPtr(node + RBNODE_RIGHT_OFF, &next)) return 0;
        } else {
            bound = node;
            if (!safeReadPtr(node + RBNODE_LEFT_OFF, &next)) return 0;
        }
        if (next <= 0x10000) return 0;
        node = next;
    }
    if (!reachedLeaf) { logLine("actionbar eff: effect map walk too deep — abandoned"); return 0; }
    if (bound == head) return 0;                       // every key was greater: not present

    uint8_t bnil = 1;
    uint32_t bk = 0;
    if (!safeReadU8(bound + RBNODE_ISNIL_OFF, &bnil) || bnil) return 0;
    if (!safeReadU32(bound + RBNODE_KEY_OFF, &bk) || key < bk) return 0;   // lower_bound overshot
    return bound + RBNODE_VALUE_OFF;
}

static void abSkillEffects(uintptr_t base, uintptr_t skill, char* out, int outsz) {
    out[0] = 0;
    char buf[AB_EFF_BUCKET_SZ * 2];

    uint32_t tgt = 0;
    safeReadU32(skill + SKILL_TGTMASK_OFF, &tgt);
    abEffRenderVector(base, skill, skill + SKILL_EFFECTS_OFF, (tgt & 0x100) != 0, buf, sizeof buf);
    if (buf[0]) abAppend(out, outsz, buf);

    uintptr_t cobj = abHeroClass(base);
    if (!cobj) return;
    uintptr_t dbeg = 0, dend = 0;
    if (!safeReadPtr(cobj + HEROCLASS_EFFDEF_BEG_OFF, &dbeg) ||
        !safeReadPtr(cobj + HEROCLASS_EFFDEF_END_OFF, &dend)) return;
    if (!dbeg || dend < dbeg) return;
    size_t ndef = (dend - dbeg) / EFFDEF_STRIDE;
    if (ndef > AB_EFF_MAX_DEFS) {
        logLine("actionbar eff: class claims %zu effect defs — implausible, skipped", ndef);
        return;
    }
    for (size_t i = 0; i < ndef; i++) {
        uint32_t key = 0;
        if (!safeReadU32(dbeg + i * EFFDEF_STRIDE + EFFDEF_KEY_OFF, &key)) continue;
        uintptr_t vec = abEffMapFind(skill, key);
        if (!vec) continue;
        abEffRenderVector(base, skill, vec, false, buf, sizeof buf);
        if (buf[0]) abAppend(out, outsz, buf);
    }
}

static uintptr_t abSkillAt(uintptr_t base, int skillIdx, int slotCount) {
    uintptr_t skills[AB_MAX_SKILLS];
    int n = abSkillList(base, skills, AB_MAX_SKILLS);
    if (n != slotCount) {
        logLine("actionbar skill: walk gave %d skills but %d slots drawn -> generic labels", n, slotCount);
        return 0;
    }
    if (skillIdx < 0 || skillIdx >= n) return 0;
    return skills[skillIdx];
}

bool abSkillName(uintptr_t base, uintptr_t skill, char* out, int outsz) {
    uintptr_t cobj = abHeroClass(base);
    char classId[64] = { 0 }, skillId[64] = { 0 };
    if (!skill || !cobj) return false;
    if (!safeReadCStr(cobj + HEROCLASS_ID_OFF, classId, sizeof classId) ||
        !abPlausibleName(classId)) return false;
    if (!safeReadCStr(skill + SKILL_ID_OFF, skillId, sizeof skillId) ||
        !abPlausibleName(skillId)) return false;

    char key[192];
    _snprintf(key, sizeof key, SKILL_NAME_FMT, classId, skillId);
    key[sizeof key - 1] = 0;
    bool ok = resolveKey(base, key, out, outsz) && out[0];
    if (!ok) {
        _snprintf(out, outsz, "%s", skillId);
        out[outsz - 1] = 0;
    }
    abStripMarkup(out);
    logLine("actionbar skill id=\"%s\" key=\"%s\" -> %s", skillId, key, ok ? out : "MISS");
    return true;
}

void abSkillTitled(uintptr_t base, uintptr_t skill, const char* fallbackName,
                   char* out, int outsz, bool withLevel) {
    char name[256];
    if (!abSkillName(base, skill, name, sizeof name)) {
        _snprintf(name, sizeof name, "%s", fallbackName);
        name[sizeof name - 1] = 0;
    }
    if (!withLevel) {
        _snprintf(out, outsz, "%s", name);
        out[outsz - 1] = 0;
        return;
    }

    uint8_t isMove = 0;
    bool moveSkill = skill && safeReadU8(skill + SKILL_ISMOVE_OFF, &isMove) && isMove != 0;

    int32_t lvl = 0;
    if (!moveSkill && safeReadU32(skill + SKILL_LEVEL_OFF, (uint32_t*)&lvl) && lvl >= 0 && lvl < 16) {
        char fmt[128];
        if (resolveKey(base, "combat_skill_name_level_format", fmt, sizeof fmt) &&
            abFormatMatches(fmt, "sd")) {
            _snprintf(out, outsz, fmt, name, lvl);
            out[outsz - 1] = 0;
            abStripMarkup(out);
        } else {
            _snprintf(out, outsz, "%s %d", name, lvl);
        }
    } else {
        _snprintf(out, outsz, "%s", name);      // omit the level, never fake it
    }
    out[outsz - 1] = 0;
}

int abBuildSkillLines(uintptr_t base, uintptr_t skill, uintptr_t hero, const char* fallbackName,
                      char lines[AB_TIP_MAX_LINES][AB_TIP_LINE_SZ]) {
    // Line 0 — name, level, type.
    char titled[320];
    abSkillTitled(base, skill, fallbackName, titled, sizeof titled);

    char type[256];
    abSkillTypeText(base, skill, type, sizeof type);
    if (type[0]) _snprintf(lines[0], AB_TIP_LINE_SZ, "%s. %s.", titled, type);
    else         _snprintf(lines[0], AB_TIP_LINE_SZ, "%s.", titled);
    lines[0][AB_TIP_LINE_SZ - 1] = 0;

    int n = 1;
    char buf[AB_TIP_LINE_SZ];

    // Line 1 — ranks.
    abSkillRanksText(base, skill, buf, sizeof buf);
    if (buf[0]) { _snprintf(lines[n], AB_TIP_LINE_SZ, "%s.", buf); lines[n][AB_TIP_LINE_SZ-1]=0; n++; }

    abSkillLimitsText(base, skill, hero, buf, sizeof buf);
    if (buf[0]) { _snprintf(lines[n], AB_TIP_LINE_SZ, "%s.", buf); lines[n][AB_TIP_LINE_SZ-1]=0; n++; }

    // Line 3 — the stat block.
    abSkillTooltip(base, skill, buf, sizeof buf);
    if (buf[0]) { _snprintf(lines[n], AB_TIP_LINE_SZ, "%s.", buf); lines[n][AB_TIP_LINE_SZ-1]=0; n++; }

    abSkillEffects(base, skill, buf, sizeof buf);
    if (buf[0]) { _snprintf(lines[n], AB_TIP_LINE_SZ, "%s.", buf); lines[n][AB_TIP_LINE_SZ-1]=0; n++; }
    return n;
}

static void abItemText(uintptr_t base, const ActionItem* it, int slotCount, char* out, int outsz);
static int abPortraitLines(uintptr_t base, char lines[][AB_TIP_LINE_SZ], int n);

static int abBuildLines(uintptr_t base, const ActionItem* it, int slotCount,
                        char lines[AB_TIP_MAX_LINES][AB_TIP_LINE_SZ]) {
    for (int i = 0; i < AB_TIP_MAX_LINES; i++) lines[i][0] = 0;

    if (it->kind != AB_SKILL) {
        abItemText(base, it, slotCount, lines[0], AB_TIP_LINE_SZ);
        if (it->kind == AB_PORTRAIT) return abPortraitLines(base, lines, 1);
        return 1;
    }

    if (campInSkillPhase(base)) {
        int nc = campBarLines(base, it->skillIdx, lines);
        if (nc > 0) return nc;
        _snprintf(lines[0], AB_TIP_LINE_SZ, axs(AXS_AB_CAMP_SKILL_N_FMT), it->skillIdx + 1);
        return 1;
    }

    uintptr_t skill = abSkillAt(base, it->skillIdx, slotCount);
    if (!skill) {
        _snprintf(lines[0], AB_TIP_LINE_SZ, axs(AXS_AB_ACTION_N_DOT_FMT), it->skillIdx + 1);
        return 1;
    }

    uintptr_t perf = abSelectedHero(base);
    char fallback[64];
    _snprintf(fallback, sizeof fallback, axs(AXS_AB_ACTION_N_FMT), it->skillIdx + 1);
    int nl = abBuildSkillLines(base, skill, perf, fallback, lines);

    if (nl > 0 && skillUnavailable(skill, perf))
        { strncat(lines[0], " ", AB_TIP_LINE_SZ - strlen(lines[0]) - 1); strncat(lines[0], axs(AXS_AB_UNAVAILABLE), AB_TIP_LINE_SZ - strlen(lines[0]) - 1); }
    return nl;
}

static void abBuildLabel(uintptr_t base, const ActionItem* it, int slotCount, char* out, int outsz) {
    if (it->kind != AB_SKILL) {
        abItemText(base, it, slotCount, out, outsz);
        return;
    }
    if (campInSkillPhase(base)) {
        if (!campBarLabel(base, it->skillIdx, out, outsz)) {
            _snprintf(out, outsz, axs(AXS_AB_CAMP_SKILL_N_FMT), it->skillIdx + 1);
            out[outsz - 1] = 0;
        }
        return;
    }
    uintptr_t skill = abSkillAt(base, it->skillIdx, slotCount);
    if (!skill) {
        _snprintf(out, outsz, axs(AXS_AB_ACTION_N_DOT_FMT), it->skillIdx + 1);
        out[outsz - 1] = 0;
        return;
    }
    char fallback[64], titled[320], type[256];
    _snprintf(fallback, sizeof fallback, axs(AXS_AB_ACTION_N_FMT), it->skillIdx + 1);
    abSkillTitled(base, skill, fallback, titled, sizeof titled, false);   // no level on the label
    abSkillTypeText(base, skill, type, sizeof type);
    if (type[0]) _snprintf(out, outsz, "%s. %s.", titled, type);
    else         _snprintf(out, outsz, "%s.", titled);
    out[outsz - 1] = 0;
    if (skillUnavailable(skill, abSelectedHero(base)))
        { strncat(out, " ", outsz - strlen(out) - 1); strncat(out, axs(AXS_AB_UNAVAILABLE), outsz - strlen(out) - 1); }
}

static int g_abTipCol = 0;

static void abSpeakTipLine(uintptr_t base, const ActionItem* it, int slotCount, int line,
                           const char* head = nullptr) {
    char lines[AB_TIP_MAX_LINES][AB_TIP_LINE_SZ];
    int n = abBuildLines(base, it, slotCount, lines);
    if (n <= 0) { if (head && head[0]) postSpeech(head); return; }
    if (line < 0) line = n - 1;
    if (line >= n) line = 0;
    g_abTipLine = line;
    logLine("actionbar tipline %d/%d -> \"%s\"", line, n, lines[line]);
    if (head && head[0]) {
        char full[AB_TIP_LINE_SZ + 128];
        _snprintf(full, sizeof full, "%s %s", head, lines[line]);
        full[sizeof full - 1] = 0;
        postSpeech(full);
        return;
    }
    postSpeech(lines[line]);
}

void abSpeakLabel(uintptr_t base, const ActionItem* it, int slotCount,
                  const char* head) {
    char label[AB_TIP_LINE_SZ];
    abBuildLabel(base, it, slotCount, label, sizeof label);
    g_abTipLine = 0;
    g_abTipCol  = 0;
    logLine("actionbar label -> \"%s\"", label);
    if (head && head[0]) {
        char full[AB_TIP_LINE_SZ + 128];
        _snprintf(full, sizeof full, "%s %s", head, label);
        full[sizeof full - 1] = 0;
        postSpeech(full);
        return;
    }
    postSpeech(label);
}

static void abItemText(uintptr_t base, const ActionItem* it, int slotCount, char* out, int outsz) {
    char what[512] = { 0 };

    switch (it->kind) {
        case AB_PASS:
            _snprintf(what, sizeof what, "%s", axs(AXS_AB_LABEL_SKIP_TURN));
            break;
        case AB_REORDER:
            if (!resolveKey(base, REORDER_LABEL_KEY, what, sizeof what) || !what[0])
                _snprintf(what, sizeof what, "%s", axs(AXS_AB_LABEL_REORDER));
            break;
        case AB_REST: {
            char label[128];
            if (!abTipPlain(base, "camping_respite_rest", label, sizeof label) || !label[0])
                _snprintf(label, sizeof label, "%s", axs(AXS_AB_LABEL_REST));
            int pts = campPoints(base);
            if (pts > 0) _snprintf(what, sizeof what, axs(AXS_AB_REST_ENDS_TIME_FMT), label, pts);
            else         _snprintf(what, sizeof what, axs(AXS_AB_REST_ENDS_FMT), label);
            break;
        }
        case AB_PORTRAIT: {
            if (!abHeroLabel(base, what, sizeof what)) _snprintf(what, sizeof what, "%s", axs(AXS_AB_LABEL_HERO));
            char form[64];
            uintptr_t hero = abSelectedHero(base);
            if (spModeName(base, hero, form, sizeof form))
                rvAppendComma(what, sizeof what, form);
            char frag[160];
            bool isCorpse = false;
            { char dead[96]; isCorpse = rvCorpseWord(base, hero, dead, sizeof dead); }
            if (abHeroHealth(base, hero, frag, sizeof frag)) rvAppendComma(what, sizeof what, frag);
            else logLine("actionbar portrait: health unreadable for hero=%p", (void*)hero);
            if (!isCorpse) {
                if (abHeroStressCur(base, hero, frag, sizeof frag)) rvAppendComma(what, sizeof what, frag);
                else logLine("actionbar portrait: stress unreadable for hero=%p", (void*)hero);
            }
            break;
        }
        default:
            _snprintf(what, sizeof what, axs(AXS_AB_ACTION_N_FMT), it->skillIdx + 1);
            break;
    }
    what[sizeof what - 1] = 0;

    _snprintf(out, outsz, "%s.", what);
    out[outsz - 1] = 0;
}

void abDumpBar(uintptr_t base, const ActionItem* items, int n) {
    uintptr_t root = 0; uint32_t combat = 0;
    safeReadPtr(base + MAP_ROOT_RVA, &root);
    if (root) safeReadU32(root + RAID_IN_COMBAT_OFF, &combat);
    logLine("actionbar DUMP items=%d combat=%u hero=%p", n, combat, (void*)abSelectedHero(base));
    for (int i = 0; i < n; i++) {
        char ida[16], owa[16];
        idToAscii(items[i].id, ida); idToAscii(items[i].owner, owa);
        logLine("  ab[%d] kind=%d skill=%d id=0x%llx '%s' owner=0x%llx '%s' ctr=(%.0f,%.0f)",
                i, items[i].kind, items[i].skillIdx, (unsigned long long)items[i].id, ida,
                (unsigned long long)items[i].owner, owa, items[i].cx, items[i].cy);
    }
}

int abSlotCount(const ActionItem* items, int n) {
    int c = 0;
    for (int i = 0; i < n; i++) if (items[i].kind == AB_SKILL) c++;
    return c;
}

void abSetActive(uintptr_t base, bool on) {
    g_abActive = on;
    g_abCursor = on ? 0 : -1;
    logLine("actionbar %s", on ? "active" : "inactive");
}

static bool tsBegin(uintptr_t base, uintptr_t skill, const char* skillName, int abCursor);
static void serviceUserSelectedTarget(uintptr_t base);   // the monster-turn aim, with tsBegin
static const uint32_t MV_WATCH_MS       = 1500;  // the direct call: the swap is immediate
static const uint32_t MV_CLICK_WATCH_MS = 3000;  // the clicked gesture: queued stamps + animation
static void mvSnapshot(uintptr_t base, const char* mover, int destSlot,
                       uint32_t waitMs = MV_WATCH_MS);

typedef void (*ToggleSheetFn)();
bool abToggleCharSheet(uintptr_t base) {
    __try { ((ToggleSheetFn)(base + TOGGLE_CHARSHEET_RVA))(); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

uintptr_t abCurrentTurnActor(uintptr_t root) {
    uintptr_t battle = root + BATTLE_OBJ_OFF, beg = 0, end = 0, actor = 0;
    uint32_t  idx = 0;
    if (!safeReadPtr(battle + BATTLE_TURNVEC_BEG_OFF, &beg) || !safeReadPtr(battle + BATTLE_TURNVEC_END_OFF, &end)) return 0;
    if (!safeReadU32(battle + BATTLE_TURN_IDX_OFF, &idx) || !beg || end <= beg)                return 0;
    if ((uintptr_t)idx >= (uintptr_t)(end - beg) / 0x18)                        return 0;
    safeReadPtr(beg + (uintptr_t)idx * 0x18, &actor);
    return actor;
}

bool abBattleLive(uintptr_t root) {
    uint32_t state = 0;
    if (!root || !safeReadU32(root + RAID_IN_COMBAT_OFF, &state)) return true;   // never on a bad read
    return state != 0;
}

static bool abHeroMayAct(uintptr_t root, uintptr_t hero, const char* what) {
    if (!root || !hero) return true;                 // nothing to judge; never refuse on a bad read
    uint32_t state = 0;
    if (!safeReadU32(root + RAID_IN_COMBAT_OFF, &state) || state == 0) return true;  // no battle
    uintptr_t turn = abCurrentTurnActor(root);
    if (!turn || turn == hero) return true;
    logLine("turngate: refused \"%s\" — the turn belongs to %p, not %p (battle state 0x%x)",
            what ? what : "action", (void*)turn, (void*)hero, state);
    return false;
}

static volatile bool g_stPending  = false;
static uint32_t      g_stDeadline = 0;
static uintptr_t     g_stPrevActor = 0;   // whose turn it was when we clicked
static uintptr_t     g_stPrevHero  = 0;   // which hero the bar showed when we clicked
static const uint32_t ST_WATCH_MS = 4000;

static bool abSkipTurn(uintptr_t base, const ActionItem* it) {
    uintptr_t root = mapRoot(base);
    if (!root || !it) {
        logLine("skipturn: root=%p it=%p — not in a live raid view", (void*)root, (const void*)it);
        postSpeech(axs(AXS_AB_CANT_SKIP));
        return true;
    }
    if (it->cx <= 0.0f || it->cy <= 0.0f) {
        logLine("skipturn: pass button has no on-screen position (%.0f,%.0f)", it->cx, it->cy);
        postSpeech(axs(AXS_AB_CANT_SKIP));
        return true;
    }
    uint32_t combat = 0;
    safeReadU32(root + RAID_IN_COMBAT_OFF, &combat);
    logLine("skipturn: clicking pass at (%.0f,%.0f) combat=%u", it->cx, it->cy, combat);

    moveCursorTo(it->cx, it->cy);
    enqueueSynth(SDL_EVT_MOUSEBUTTONDOWN, SDL_BUTTON_LEFT, 1, 1);
    enqueueSynth(SDL_EVT_MOUSEBUTTONUP,   SDL_BUTTON_LEFT, 0, 2);

    g_stPrevActor = abCurrentTurnActor(root);
    g_stPrevHero  = abSelectedHero(base);
    g_stDeadline  = GetTickCount() + ST_WATCH_MS;
    g_stPending   = true;
    return true;
}

void serviceSkipTurn(uintptr_t base) {
    if (!g_stPending) return;
    uintptr_t root = mapRoot(base);
    if (!root) { g_stPending = false; return; }        // left the raid; nothing to report

    uintptr_t now  = abCurrentTurnActor(root);
    uintptr_t hero = abSelectedHero(base);
    bool turnMoved = (now  && g_stPrevActor && now  != g_stPrevActor);
    bool heroMoved = (hero && g_stPrevHero  && hero != g_stPrevHero);
    if (turnMoved || heroMoved) {
        g_stPending = false;
        logLine("skipturn: took after %ums (turn %p->%p, hero %p->%p)",
                ST_WATCH_MS - (g_stDeadline - GetTickCount()),
                (void*)g_stPrevActor, (void*)now, (void*)g_stPrevHero, (void*)hero);
        postSpeech(axs(AXS_AB_TURN_SKIPPED));
        return;
    }
    if (GetTickCount() >= g_stDeadline) {
        g_stPending = false;
        logLine("skipturn: no turn/hero change within %ums — outcome UNKNOWN, saying nothing "
                "(actor %p, hero %p)", ST_WATCH_MS, (void*)now, (void*)hero);
    }
}

// ---- REORDER PARTY — the bar's "Default Party order" button ----
static const uint32_t RO_WATCH_MS = 1500;

static bool      g_roWatch    = false;
static uint32_t  g_roDeadline = 0;
static int       g_roN        = 0;
static uintptr_t g_roPtr[RV_MAX_MEMBERS];

void roFormationText(uintptr_t base, char* out, int outsz) {
    out[0] = 0;
    uintptr_t party[RV_MAX_MEMBERS];
    int n = rvPartyList(base, party, RV_MAX_MEMBERS);
    if (n <= 0) return;

    uintptr_t byRank[5] = { 0, 0, 0, 0, 0 };          // index 1..4 = the four ranks
    bool ranksUsable = true;
    for (int i = 0; i < n; i++) {
        int rank = rvRankFromGame(party[i], nullptr, nullptr);
        if (rank < 1 || rank > 4 || byRank[rank]) { ranksUsable = false; break; }
        byRank[rank] = party[i];
    }

    char line[512]; line[0] = 0;
    for (int i = 1; i <= (ranksUsable ? 4 : n); i++) {
        uintptr_t hero = ranksUsable ? byRank[i] : party[i - 1];
        if (!hero) continue;
        char name[80], cls[80];
        if (!abHeroNameClassOf(base, hero, name, sizeof name, cls, sizeof cls)) continue;
        char deadWord[96];
        char labelled[176];
        if (rvCorpseWord(base, hero, deadWord, sizeof deadWord)) {
            if (name[0]) _snprintf(labelled, sizeof labelled, "%s, %s", name, deadWord);
            else         _snprintf(labelled, sizeof labelled, "%s", deadWord);
            labelled[sizeof labelled - 1] = 0;
            strncpy(name, labelled, sizeof name - 1);
            name[sizeof name - 1] = 0;
            cls[0] = 0;
        }
        const char* who = name[0] ? name : cls;
        if (!who[0]) continue;
        char piece[160];
        if (ranksUsable) { _snprintf(piece, sizeof piece, "%s", line[0] ? " " : "");
                           _snprintf(piece + strlen(piece), sizeof piece - strlen(piece),
                                     axs(AXS_AB_POSITION_WHO_FMT), i, who); }
        else             _snprintf(piece, sizeof piece, "%s%s.",              line[0] ? " " : "",    who);
        piece[sizeof piece - 1] = 0;
        strncat(line, piece, sizeof line - strlen(line) - 1);
    }
    if (!ranksUsable)
        logLine("reorder: a rank mask would not read -> naming the party without positions");

    _snprintf(out, outsz, "%s", line);
    out[outsz - 1] = 0;
}

static bool abReorderParty(uintptr_t base, const ActionItem* it) {
    uintptr_t root = mapRoot(base);
    if (!root || !it) {
        logLine("reorder: root=%p it=%p — not in a live raid view", (void*)root, (const void*)it);
        postSpeech(axs(AXS_AB_CANT_REORDER));
        return true;
    }
    if (it->cx <= 0.0f || it->cy <= 0.0f) {
        logLine("reorder: button has no on-screen position (%.0f,%.0f)", it->cx, it->cy);
        postSpeech(axs(AXS_AB_CANT_REORDER));
        return true;
    }

    uint32_t combat = 0;
    safeReadU32(root + RAID_IN_COMBAT_OFF, &combat);
    logLine("reorder: clicking the default-order button at (%.0f,%.0f) combat=%u",
            it->cx, it->cy, combat);

    moveCursorTo(it->cx, it->cy);
    enqueueSynth(SDL_EVT_MOUSEBUTTONDOWN, SDL_BUTTON_LEFT, 1, 1);
    enqueueSynth(SDL_EVT_MOUSEBUTTONUP,   SDL_BUTTON_LEFT, 0, 2);

    uintptr_t party[RV_MAX_MEMBERS];
    g_roN = rvPartyList(base, party, RV_MAX_MEMBERS);
    for (int i = 0; i < g_roN; i++) g_roPtr[i] = party[i];
    g_roDeadline = GetTickCount() + RO_WATCH_MS;
    g_roWatch    = true;
    logLine("reorder-watch: armed, %d party members", g_roN);
    return true;
}

void serviceReorderWatch(uintptr_t base) {
    if (!g_roWatch) return;

    uintptr_t party[RV_MAX_MEMBERS];
    int n = rvPartyList(base, party, RV_MAX_MEMBERS);

    if (n <= 0) {
        g_roWatch = false;
        logLine("reorder-watch: party vector no longer readable -> standing down, saying nothing");
        return;
    }

    bool changed = (n != g_roN);
    for (int i = 0; !changed && i < n; i++) changed = (party[i] != g_roPtr[i]);
    bool timedOut = GetTickCount() >= g_roDeadline;
    if (!changed && !timedOut) return;

    g_roWatch = false;
    char formation[512];
    roFormationText(base, formation, sizeof formation);

    char msg[640];
    _snprintf(msg, sizeof msg, "%s%s%s",
              axs(changed ? AXS_AB_ORDER_DEFAULT : AXS_AB_ORDER_UNCHANGED),
              formation[0] ? " " : "", formation);
    msg[sizeof msg - 1] = 0;
    logLine("reorder-watch: party order %s within %ums -> \"%s\"",
            changed ? "changed" : "did NOT change", RO_WATCH_MS, msg);
    postSpeech(msg);
}

bool abActivate(uintptr_t base, const ActionItem* it, int slotCount, int abCursor) {
    switch (it->kind) {
        case AB_SKILL: {
            if (campInSkillPhase(base)) return campUseSkill(base, it);
            uintptr_t skill = abSkillAt(base, it->skillIdx, slotCount);
            char name[160];
            if (!skill || !abSkillName(base, skill, name, sizeof name)) {
                logLine("actionbar activate: slot %d has no readable skill", it->skillIdx);
                postSpeech(axs(AXS_AB_CANT_USE_NOW));
                return true;
            }
            logLine("actionbar activate: skill %d \"%s\" (elem 0x%llx)", it->skillIdx, name,
                    (unsigned long long)it->id);
            g_tsSkillElemId = it->id;        // the button a friendly commit will CLICK
            return tsBegin(base, skill, name, abCursor);
        }
        case AB_PORTRAIT:
            logLine("actionbar activate: portrait -> ToggleCharacterDisplay");
            if (!abToggleCharSheet(base)) {
                logLine("actionbar activate: ToggleCharacterDisplay faulted");
                postSpeech(axs(AXS_AB_SHEET_DIDNT_OPEN));
            }
            return true;
        case AB_PASS:
            logLine("actionbar activate: skip turn");
            return abSkipTurn(base, it);
        case AB_REORDER:
            logLine("actionbar activate: reorder party (default order)");
            return abReorderParty(base, it);
        case AB_REST:
            logLine("actionbar activate: REST (end the camp), %d points left", campPoints(base));
            if (!frontEndClickElementId(it->id)) {
                postSpeech(axs(AXS_AB_COULDNT_REST));
                return true;
            }
            return true;
        default:
            postSpeech(axs(AXS_AB_NO_ACTION));
            return true;
    }
}

bool routeActionKey(uintptr_t base, uint32_t sym, uint16_t mod, uint8_t repeat) {
    if (mod & (KMOD_LALT | KMOD_RALT)) return false;
    bool ctrl = (mod & (KMOD_LCTRL | KMOD_RCTRL)) != 0;

    if (sym == SDLK_TAB) {
        if (ctrl) return false;                              // Ctrl+Tab is not ours
        if (repeat) return false;                            // the game's key: repeats are its own too
        logLine("actionbar: Tab -> closing the bar, panel toggle passes through to the game");
        abSetActive(base, false);
        armPanelHandoff("the action bar");
        return false;
    }

    if (sym == SDLK_UP && !ctrl) {
        if (repeat) return true;         // held key must not flap the context
        return rvEnterFromBar(base);
    }

    {
        int jump = 0;
        if (axDecodeJump(sym, mod, repeat, &jump)) {
            if (!jump) return true;          // held jump: one landing per press
            ActionItem items[AB_MAX_ITEMS];
            int n = abBuildBar(base, items, AB_MAX_ITEMS);
            if (n <= 0) { logLine("actionbar: nothing to navigate"); return true; }
            int target = (jump > 0) ? n - 1 : 0;
            logLine("actionbar jump %d -> %d (of %d)", g_abCursor, target, n);
            g_abCursor = target;
            abSpeakLabel(base, &items[target], abSlotCount(items, n));
            return true;
        }
    }

    int dir = 0, tipDir = 0, panelDir = 0;
    bool activate = false;
    switch (sym) {
        case SDLK_LEFT:  if (ctrl) { panelDir = -1; break; } dir = -1; break;
        case SDLK_RIGHT: if (ctrl) { panelDir =  1; break; } dir =  1; break;
        case SDLK_RETURN:
        case SDLK_KP_ENTER: if (ctrl) return false; activate = true; break;
        case SDLK_UP:    tipDir =  1; break;
        case SDLK_DOWN:  if (!ctrl) return false; tipDir = -1; break;
        default:         return false;   // not ours -> let the game have it
    }
    if (repeat && !panelDir) return true;      // one move per press, but never leak to the game

    ActionItem items[AB_MAX_ITEMS];
    int n = abBuildBar(base, items, AB_MAX_ITEMS);
    if (n <= 0) {
        if (panelDir) return false;            // no bar: Ctrl+arrows were never ours here
        logLine("actionbar: nothing to navigate");
        return true;
    }
    if (g_abCursor >= n) g_abCursor = n - 1;   // the bar shrank under the cursor

    if (panelDir) {
        if (g_abCursor < 0 || items[g_abCursor].kind != AB_PORTRAIT) return false;
        if (repeat) return true;               // claimed key: one switch per press
        int want = g_abTipCol + panelDir;
        if (want < -1) want = -1;              // three columns now: items / main / resistances
        if (want > 1)  want = 1;
        g_abTipCol  = want;
        g_abTipLine = 0;
        if (want != 0) {
            uintptr_t hero = abSelectedHero(base);
            int slotEnd = 0;                        // the rank, as abPortraitLines asks for it:
            int slot = rvRankFromGame(hero, nullptr, &slotEnd);   // the panel's identity row
            char rl[AB_TIP_MAX_LINES][AB_TIP_LINE_SZ];
            int nr = (want == 1)
                         ? spHeroResistLines(base, hero, slot, slotEnd, false, rl, AB_TIP_MAX_LINES)
                         : spHeroItemLines(base, hero, slot, slotEnd, false, rl, AB_TIP_MAX_LINES);
            if (nr <= 0) {
                g_abTipCol = 0;
                postSpeech(axs(AXS_NO_MORE_INFO));
                return true;
            }
            char head[128];
            if (want == 1) spResistPanelHead(base, head, sizeof head);
            else { _snprintf(head, sizeof head, "%s", axs(AXS_RV_ITEMS_PANEL)); head[sizeof head - 1] = 0; }
            char full[AB_TIP_LINE_SZ + 140];
            _snprintf(full, sizeof full, "%s. %s", head, rl[0]);
            full[sizeof full - 1] = 0;
            logLine("actionbar tip panel -> %s (%d lines)", want == 1 ? "resistances" : "items", nr);
            postSpeech(full);
        } else {
            logLine("actionbar tip panel -> main");
            abSpeakTipLine(base, &items[g_abCursor], abSlotCount(items, n), 0);
        }
        return true;
    }

    if (activate) {
        if (g_abCursor < 0) { postSpeech(axs(AXS_AB_NO_ACTION_SELECTED)); return true; }
        return abActivate(base, &items[g_abCursor], abSlotCount(items, n), g_abCursor);
    }

    if (tipDir) {
        if (g_abCursor < 0) { postSpeech(axs(AXS_AB_NO_ACTION_SELECTED)); return true; }
        if (g_abTipCol != 0 && items[g_abCursor].kind == AB_PORTRAIT) {
            uintptr_t hero = abSelectedHero(base);
            int slotEnd = 0;
            int slot = rvRankFromGame(hero, nullptr, &slotEnd);
            char rl[AB_TIP_MAX_LINES][AB_TIP_LINE_SZ];
            int nr = (g_abTipCol == 1)
                         ? spHeroResistLines(base, hero, slot, slotEnd, false, rl, AB_TIP_MAX_LINES)
                         : spHeroItemLines(base, hero, slot, slotEnd, false, rl, AB_TIP_MAX_LINES);
            if (nr <= 0) { postSpeech(axs(AXS_NO_MORE_INFO)); return true; }
            int line = (g_abTipLine + tipDir + nr) % nr;
            g_abTipLine = line;
            logLine("actionbar %s tipline %d/%d -> \"%s\"",
                    g_abTipCol == 1 ? "resist" : "items", line, nr, rl[line]);
            postSpeech(rl[line]);
            return true;
        }
        g_abTipCol = 0;
        char lines[AB_TIP_MAX_LINES][AB_TIP_LINE_SZ];
        int nl = abBuildLines(base, &items[g_abCursor], abSlotCount(items, n), lines);
        if (nl <= 1) { postSpeech(axs(AXS_NO_MORE_INFO)); return true; }
        abSpeakTipLine(base, &items[g_abCursor], abSlotCount(items, n),
                       (g_abTipLine + tipDir + nl) % nl);
        return true;
    }

    int target;
    if (g_abCursor < 0) target = 0;
    else                target = (g_abCursor + dir + n) % n;   // wrap at both ends

    logLine("actionbar dir=%d %d -> %d (of %d)", dir, g_abCursor, target, n);
    g_abCursor = target;
    abSpeakLabel(base, &items[target], abSlotCount(items, n));
    return true;
}

bool routeActionToggle(uintptr_t base, uint32_t sym, uint8_t repeat) {
    if (sym != SDLK_BACKQUOTE) return false;
    if (repeat) return true;                   // held key must not flap the context

    int keepItem = g_abActive ? g_abCursor : -1;
    int rvRow    = g_rvActive ? g_rvCursor : -1;

    ActionItem items[AB_MAX_ITEMS];
    int n = abBuildBar(base, items, AB_MAX_ITEMS);
    if (n <= 0) {
        logLine("actionbar: no bar on screen, not entering");
        postSpeech(axs(AXS_AB_NO_ACTIONS_HERE));
        return true;
    }
    if (!g_abDumped) { abDumpBar(base, items, n); g_abDumped = true; }
    if (g_rvActive) rvSetActive(base, false);
    if (g_qtActive) qtSetActive(base, false);
    if (g_tsActive) { tsSetActive(false); logLine("targeting: abandoned (action bar reopened)"); }
    iuAbandon("action bar opened");
    abSetActive(base, true);
    if (keepItem >= 0) {
        g_abCursor = keepItem < n ? keepItem : n - 1;
    } else {
        g_abRoomReturn = rvRow;
    }
    abSpeakLabel(base, &items[g_abCursor], abSlotCount(items, n), "Actions.");
    return true;
}
// ---- Selected hero: announce the swap ----

static bool abStatSum(uintptr_t begPtr, uintptr_t endPtr, uint32_t mask, float* out) {
    uintptr_t beg = 0, end = 0;
    if (!safeReadPtr(begPtr, &beg) || !safeReadPtr(endPtr, &end)) return false;
    if (!beg || end < beg) return false;
    int64_t n = (int64_t)((end - beg) >> 2);
    if (n < 0) return false;
    if (n > STAT_MAX_ENTRIES) { logLine("herostat: vector claims %lld entries, capping", (long long)n); n = STAT_MAX_ENTRIES; }
    float sum = 0.0f;
    for (int64_t i = 0; i < n; i++) {
        if (!((mask >> (i & 31)) & 1)) continue;
        float f = 0.0f;
        if (!abReadF32(beg + (uintptr_t)i * 4, &f)) return false;
        sum += f;
    }
    *out = sum;
    return true;
}

bool abStatValue(uintptr_t stat, uint32_t mask, bool clamp, float* out) {
    float flat = 0.0f, mult = 0.0f;
    if (!abStatSum(stat + STAT_FLAT_BEG_OFF, stat + STAT_FLAT_END_OFF, mask, &flat)) return false;
    if (!abStatSum(stat + STAT_MULT_BEG_OFF, stat + STAT_MULT_END_OFF, mask, &mult)) return false;
    float v = mult * flat + flat;
    if (clamp) {
        float lo = 0.0f, hi = 0.0f;
        if (!abReadF32(stat + STAT_MIN_OFF, &lo) || !abReadF32(stat + STAT_MAX_OFF, &hi)) return false;
        if (v < lo) v = lo;
        else if (v > hi) v = hi;
    }
    *out = v;
    return true;
}

bool abHeroHealth(uintptr_t base, uintptr_t hero, char* out, int outsz) {
    float cur = 0.0f, max = 0.0f;
    uint32_t mask = 0;
    if (!abReadF32(hero + ACTOR_CUR_HP_OFF, &cur)) return false;
    if (!safeReadU32(hero + ACTOR_HP_MASK_OFF, &mask)) return false;
    if (!abStatValue(hero + ACTOR_STAT_OFF, mask, true, &max)) return false;
    return abTipNumFirst(base, HEALTH_FMT_KEY, (int)ceilf(cur), (int)ceilf(max), out, outsz);
}

bool abHeroStress(uintptr_t base, uintptr_t hero, char* out, int outsz) {
    float cur = 0.0f;
    if (!abReadF32(hero + ACTOR_STRESS_OFF, &cur)) return false;
    return abTipNumFirst(base, STRESS_FMT_KEY, (int)ceilf(cur), (int)ceilf(ACTOR_MAX_STRESS),
                         out, outsz);
}

bool abHeroStressCur(uintptr_t base, uintptr_t hero, char* out, int outsz) {
    float cur = 0.0f;
    if (!abReadF32(hero + ACTOR_STRESS_OFF, &cur)) return false;
    return abTipValueOnly(base, STRESS_FMT_KEY, (int)ceilf(cur), out, outsz);
}

static uintptr_t g_lastSelHero = 0;

// Append " <frag>." if the fragment read.
void abAppendFrag(char* out, int outsz, const char* frag) {
    strncat(out, " ", outsz - strlen(out) - 1);
    strncat(out, frag, outsz - strlen(out) - 1);
    strncat(out, ".", outsz - strlen(out) - 1);
}

static int abPortraitLines(uintptr_t base, char lines[][AB_TIP_LINE_SZ], int n) {
    uintptr_t hero = abSelectedHero(base);
    if (!hero) { logLine("status portrait: no selected hero"); return n; }

    int slotEnd = 0;
    int slot = rvRankFromGame(hero, nullptr, &slotEnd);

    int before = n;
    n = spHeroTipLines(base, hero, slot, slotEnd, false, false, lines, n, AB_TIP_MAX_LINES);
    if (n == before) logLine("status portrait: nothing to say about hero=%p", (void*)hero);
    return n;
}

// ---- a new hero's turn brings the player back to the action bar ----
static uintptr_t g_hwLastHandoff = 0;
static uintptr_t g_hwTurnActor   = 0;    // the combatant last seen holding the turn

static uintptr_t hwTurnHandoff(uintptr_t base, uintptr_t hero) {
    uintptr_t root = mapRoot(base);
    uint32_t combat = 0;
    if (!root || !safeReadU32(root + RAID_IN_COMBAT_OFF, &combat) || combat == 0) {
        g_hwLastHandoff = 0;
        g_hwTurnActor   = 0;             // for the last one of this one
        return 0;
    }
    uintptr_t now = abCurrentTurnActor(root);
    if (now != 0 && now != g_hwTurnActor) {          // a real new combatant has the turn
        g_hwTurnActor = now;
        if (g_hwLastHandoff != now) g_hwLastHandoff = 0;   // the old handoff is stale
    }
    if (now != hero)             return 0;
    if (hero == g_hwLastHandoff) return 0;
    return hero;
}

// ---- ARMING-EDGE PROBE: what moves when the GAME arms a skill? ----
static uintptr_t g_axLastArmedSkill = 0;
static uint32_t  g_axLastArmState   = 0xffffffffu;

// ---- ⚠⚠ THE MOD'S OWN ARMING CLICK IS NOT A PLAYER PRESS ----
static const uint32_t TS_SELF_ARM_MS = 2000;   // the gesture spans ~350 ms; this is the backstop
static uintptr_t      g_tsSelfArmSkill = 0;
static uint32_t       g_tsSelfArmUntil = 0;

// ---- THE GAME ARMED A SKILL: OPEN THE TARGET LIST ----
static void serviceGameArmedSkill(uintptr_t base, uintptr_t hero) {
    uintptr_t armed = 0;
    if (!safeReadPtr(hero + ACTOR_CUR_SKILL_OFF, &armed)) return;

    // ---- THE SWALLOWED-EDGE PROBE ----
    if (armed == g_axLastArmedSkill) {                  // act on the EDGE, never the steady state
        if (armed && axDebugLogEnabled()) {
            uint32_t st = 0;
            safeReadU32(mapRoot(base) + RAID_IN_COMBAT_OFF, &st);
            if (st != g_axLastArmState) {
                g_axLastArmState = st;
                logLine("armed-watch: state -> 0x%x with the pointer UNCHANGED (armed=%p hero=%p "
                        "tsActive=%d) -- an arming press of THIS SAME skill is invisible to the "
                        "pointer edge", st, (void*)armed, (void*)hero, (int)g_tsActive);
            }
        }
        return;
    }
    g_axLastArmedSkill = armed;

    uint32_t state = 0;
    safeReadU32(mapRoot(base) + RAID_IN_COMBAT_OFF, &state);
    g_axLastArmState = state;
    char name[160] = "";
    if (armed && !abSkillName(base, armed, name, sizeof name)) name[0] = 0;
    logLine("armed-watch: hero=%p armed=%p \"%s\" [battle state 0x%x] tsActive=%d",
            (void*)hero, (void*)armed, armed ? name : "(cleared)", state, (int)g_tsActive);

    if (!armed) return;                                  // the turn resolved and cleared it

    if (armed == g_tsSelfArmSkill && GetTickCount() <= g_tsSelfArmUntil) {
        g_tsSelfArmSkill = 0;
        g_tsSelfArmUntil = 0;
        logLine("armed-watch: \"%s\" was armed by the mod's OWN commit click -- not a player press, "
                "no target list", name);
        return;
    }
    if (state != 0x1e) return;                           // 0x1d here is Actor_DoSkill COMMITTING
    if (g_tsActive && g_tsUserSel) {
        logLine("armed-watch: a hero-skill edge during a monster's user-selected aim -- ignored");
        return;
    }

    if (g_tsActive && armed == g_tsSkill) return;        // our own aim, already open and announced
    if (g_tsActive) {
        logLine("armed-watch: the GAME re-armed mid-aim (%p -> %p) -- reopening for \"%s\"",
                (void*)g_tsSkill, (void*)armed, name);
        tsSetActive(false);
    }

    ActionItem items[AB_MAX_ITEMS];
    const int n = abBuildBar(base, items, AB_MAX_ITEMS);
    const int slots = abSlotCount(items, n);
    int cursor = -1;
    g_tsSkillElemId = 0;
    for (int i = 0; i < n; i++) {
        if (items[i].kind != AB_SKILL) continue;
        if (abSkillAt(base, items[i].skillIdx, slots) != armed) continue;
        g_tsSkillElemId = items[i].id;
        cursor = i;
        break;
    }
    logLine("armed-watch: the GAME armed \"%s\" -- opening the target list (bar item %d, elem "
            "0x%llx)", name, cursor, (unsigned long long)g_tsSkillElemId);

    if (g_abActive) abSetActive(base, false);
    g_tsAdoptArmed = true;
    tsBegin(base, armed, name, cursor);
}

void serviceHeroWatch(uintptr_t base) {
    serviceUserSelectedTarget(base);

    uintptr_t hero = abSelectedHero(base);
    if (!hero) { g_lastSelHero = 0; g_hwLastHandoff = 0; g_hwTurnActor = 0; return; }

    serviceGameArmedSkill(base, hero);

    uintptr_t handoff = hwTurnHandoff(base, hero);
    if (handoff) g_hwLastHandoff = handoff;

    bool selChanged = (hero != g_lastSelHero);
    uintptr_t prev  = g_lastSelHero;
    g_lastSelHero = hero;
    if (!selChanged && !handoff) return;   // nothing new this frame
    if (!prev) return;
    AxContext c = currentAxContext();
    if (c == AX_LOADING || c == AX_NAMING || c == AX_CHARSHEET) return;

    char out[512];
    if (!abHeroLabel(base, out, sizeof out)) { logLine("heroswap: no label for hero=%p", (void*)hero); return; }
    strncat(out, ".", sizeof out - strlen(out) - 1);

    char dead[96];
    bool isCorpse = rvCorpseWord(base, hero, dead, sizeof dead);
    char frag[160];

    if (!isCorpse) {
        rvTrapDisarmSuffix(base, hero, frag, sizeof frag);
        if (frag[0]) abAppendFrag(out, sizeof out, frag);
    }

    if (abHeroHealth(base, hero, frag, sizeof frag)) abAppendFrag(out, sizeof out, frag);
    else logLine("heroswap: health unreadable for hero=%p", (void*)hero);
    if (!isCorpse) {
        if (abHeroStress(base, hero, frag, sizeof frag)) abAppendFrag(out, sizeof out, frag);
        else logLine("heroswap: stress unreadable for hero=%p", (void*)hero);
    }

    if (handoff && !g_tsActive && !g_iuActive && !axCoversRestingPoint()) {
        ActionItem items[AB_MAX_ITEMS];
        int n = abBuildBar(base, items, AB_MAX_ITEMS);
        if (n > 0) {
            if (g_rvActive) rvSetActive(base, false);
            if (g_qtActive) qtSetActive(base, false);
            abSetActive(base, true);
            logLine("heroswap: %p -> %p is the TURN — taking the action bar -> \"%s\"",
                    (void*)prev, (void*)hero, out);
            const bool onPortrait = items[g_abCursor].kind == AB_PORTRAIT;
            abSpeakLabel(base, &items[g_abCursor], abSlotCount(items, n), onPortrait ? nullptr : out);
            return;
        }
        logLine("heroswap: %p -> %p is the turn, but the bar does not read yet — not moving",
                (void*)prev, (void*)hero);
    }

    logLine("heroswap: %p -> %p -> \"%s\"", (void*)prev, (void*)hero, out);
    postSpeech(out);
}
// ---- SKILL TARGETING (Enter on the action bar) ----
static int  g_tsCursor   = -1;               // placed at entry by tsBegin (first enemy for an
static uintptr_t g_tsPerformer = 0;          // the hero who will act (latched at entry)
static int  g_tsReturnCursor = -1;           // action-bar cursor to restore on cancel
static char g_tsSkillName[160] = {0};        // for the entry/commit announcements
static const uint32_t TS_HERO_ELEM_FAMILY = 0x6865726f;  // 'hero' + index, owner the same tag
static const uint32_t TS_HERO_ELEM_OWNER  = 0x6865726f;
static const uint32_t TS_STATBAR_ELEM_FAMILY = 0x73626100;  // 'sba' + an ACTOR-keyed low byte
static const uint32_t TS_STATBAR_ELEM_OWNER  = 0x73747362;  // 'stbs'
static bool g_tsMove = false;                // the skill being aimed is the generic move skill
static char g_tsMoverName[160] = {0};        // who is moving — "Moving <this> to position N"

static uintptr_t g_tsCommitOne    = 0;       // the one target actor the commit is aimed at
static uintptr_t g_tsCommitVec[3] = {0};     // std::vector<Actor*> {begin, end, cap} over it

typedef char (*TargetValidFn)(void*, void*, void*);
typedef char (*TargetAnyFn)(void*, void*);
typedef void (*DoSkillFn)(void*, void*, void*, void*, unsigned char);
typedef void (*MoveSkillFn)(void*, void*, int);

static bool sehMoveSkill(uintptr_t base, uintptr_t battle, uintptr_t perf, int destIdx) {
    __try {
        ((MoveSkillFn)(base + MOVE_SKILL_RVA))(
            reinterpret_cast<void*>(battle), reinterpret_cast<void*>(perf), destIdx);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

static bool sehIsValidTarget(uintptr_t base, uintptr_t skill, uintptr_t perf, uintptr_t cand) {
    char ok = 0;
    __try {
        ok = ((TargetValidFn)(base + TARGET_VALID_RVA))(
                 reinterpret_cast<void*>(skill), reinterpret_cast<void*>(perf),
                 reinterpret_cast<void*>(cand));
    } __except (EXCEPTION_EXECUTE_HANDLER) { ok = 0; }
    return ok != 0;
}
static bool sehHasAnyTarget(uintptr_t base, uintptr_t skill, uintptr_t perf) {
    char ok = 0;
    __try {
        ok = ((TargetAnyFn)(base + TARGET_ANY_RVA))(
                 reinterpret_cast<void*>(skill), reinterpret_cast<void*>(perf));
    } __except (EXCEPTION_EXECUTE_HANDLER) { ok = 0; }
    return ok != 0;
}
static bool sehDoSkill(uintptr_t base, uintptr_t battle, uintptr_t perf, uintptr_t skill,
                       void* targets) {
    __try {
        ((DoSkillFn)(base + DO_SKILL_RVA))(
            reinterpret_cast<void*>(battle), reinterpret_cast<void*>(perf),
            reinterpret_cast<void*>(skill), targets, 1);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

static int tsBuildTargets(uintptr_t base, RvEntry* out, int maxOut) {
    if (!g_tsSkill || !g_tsPerformer) return 0;
    uint8_t nonAttack = 0, selfOnly = 0, perfIsMonster = 0;
    if (!safeReadU8(g_tsSkill + SKILL_NONATTACK_OFF, &nonAttack))
        logLine("targeting: class byte unreadable on skill %p — side gate assumes attack",
                (void*)g_tsSkill);
    safeReadU8(g_tsSkill + SKILL_SELFONLY_OFF, &selfOnly);
    safeReadU8(g_tsPerformer + ACTOR_IS_MONSTER_OFF, &perfIsMonster);
    const bool farSideOnly = (nonAttack == 0) && (selfOnly == 0);
    const bool farIsEnemy  = (perfIsMonster == 0);   // a hero's far side is the enemies
    RvEntry all[RV_MAX_ROWS];
    int n = rvBuildRoom(base, all, RV_MAX_ROWS);
    int got = 0;
    for (int i = 0; i < n && got < maxOut; i++) {
        if (!all[i].actor) continue;
        if (farSideOnly && all[i].enemy != farIsEnemy) continue;
        if (sehIsValidTarget(base, g_tsSkill, g_tsPerformer, all[i].actor)) out[got++] = all[i];
    }
    return got;
}

static bool tsSameSide(const RvEntry& e) { return !e.enemy; }

// ---- ⚠⚠ A STAGED ATTACK CLICK WAS BUILT AND REMOVED THE SAME DAY ----

// ---- COMMIT A PARTY-SIDE CHOICE THE WAY A PLAYER DOES: two clicks ----
static bool tsClickPartyPoint(uintptr_t base, int rank, float* tx, float* ty) {
    uintptr_t heroes[8];
    float hx[8];
    const int n = feCollectFamilyByX(base, TS_HERO_ELEM_FAMILY, TS_HERO_ELEM_OWNER,
                                     heroes, hx, 8);
    if (rank < 1 || rank > n) {
        logLine("targeting: no hero element for rank %d (%d on screen)", rank, n);
        return false;
    }
    if (!elemCenter(heroes[rank - 1], tx, ty)) {
        logLine("targeting: rank %d hero element has no rect", rank);
        return false;
    }
    return true;
}

// ---- THE FAR SIDE'S CLICK POINT: the actor's OWN stat bar ----
static const uint32_t TS_BAR_ID_BASE = 0x73626172;   // 'sbar' + the actor's guid

typedef uint32_t (__fastcall *ActorGuidFn)(uintptr_t actor);

static bool tsActorGuid(uintptr_t actor, uint32_t* out) {
    uintptr_t vft = 0;
    if (!actor || !safeReadPtr(actor, &vft) || !vft) return false;
    uintptr_t fn = 0;
    if (!safeReadPtr(vft + ACTOR_GUID_VFT_OFF, &fn) || !fn) return false;
    __try { *out = ((ActorGuidFn)fn)(actor); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        logLine("targeting: the guid getter faulted on actor %p", (void*)actor);
        return false;
    }
}

static bool tsBarAnchorOffset(uintptr_t base, float* out) {
    uintptr_t heroes[8];
    float hx[8];
    const int hn = feCollectFamilyByX(base, TS_HERO_ELEM_FAMILY, TS_HERO_ELEM_OWNER, heroes, hx, 8);
    if (hn <= 0) return false;

    RvEntry rows[RV_MAX_ROWS];
    const int n = rvBuildRoom(base, rows, RV_MAX_ROWS);
    float sum = 0;
    int got = 0;
    for (int i = 0; i < n; i++) {
        if (rows[i].enemy || !rows[i].actor) continue;
        if (rows[i].slot < 1 || rows[i].slot > hn) continue;
        uint32_t guid = 0;
        if (!tsActorGuid(rows[i].actor, &guid)) continue;
        uintptr_t bar = feGetElementById((int64_t)(TS_BAR_ID_BASE + guid));
        float bpx = 0, bpy = 0, cx = 0, cy = 0;
        if (!bar || !elemPos(bar, &bpx, &bpy)) continue;
        if (!elemCenter(heroes[rows[i].slot - 1], &cx, &cy)) continue;
        sum += cx - bpx;
        got++;
    }
    if (got <= 0) return false;
    *out = sum / (float)got;
    logLine("targeting: bar anchor offset = %.1f px, calibrated on %d hero(es) this frame", *out, got);
    return true;
}

static bool tsEnemyClickPoint(uintptr_t base, uintptr_t actor, float* tx, float* ty) {
    uint32_t guid = 0;
    if (!tsActorGuid(actor, &guid)) return false;
    const int64_t barId = (int64_t)(TS_BAR_ID_BASE + guid);
    uintptr_t bar = feGetElementById(barId);
    if (!bar) {
        logLine("targeting: no stat bar for actor %p (guid %u, id 0x%llx) -- not clicking",
                (void*)actor, guid, (unsigned long long)barId);
        return false;
    }
    float bpx = 0, bpy = 0;
    if (!elemPos(bar, &bpx, &bpy)) {
        logLine("targeting: stat bar 0x%llx has no rect", (unsigned long long)barId);
        return false;
    }
    float anchor = 0;
    if (!tsBarAnchorOffset(base, &anchor)) {
        logLine("targeting: could not calibrate the bar anchor this frame -- not clicking");
        return false;
    }

    // The sprite band, read live off the party's own boxes this frame.
    uintptr_t heroes[8];
    float hx[8];
    const int hn = feCollectFamilyByX(base, TS_HERO_ELEM_FAMILY, TS_HERO_ELEM_OWNER, heroes, hx, 8);
    float band = 0;
    if (hn > 0) {
        float px = 0, py = 0;
        uint32_t hb = 0;
        if (elemPos(heroes[0], &px, &py) && safeReadU32(heroes[0] + ELEM_SIZE_OFF + 4, &hb))
            band = py + u32AsFloatM(hb) * 0.75f;
    }
    if (band <= 0) {
        logLine("targeting: no hero box to take the sprite band from (%d on screen) -- not clicking",
                hn);
        return false;
    }
    *tx = bpx + anchor;
    *ty = band;
    logLine("targeting: enemy point actor=%p guid=%u bar 0x%llx pos=(%.1f,%.1f) +anchor %.1f -> "
            "click (%.1f,%.1f) [band from hero box]", (void*)actor, guid,
            (unsigned long long)barId, bpx, bpy, anchor, *tx, *ty);
    return true;
}

static bool tsClickTargetPoint(uintptr_t base, float tx, float ty, const char* what) {
    if (!g_tsSkillElemId) return false;
    if (clickQueued()) {
        logLine("targeting: click-commit not attempted, a click is already queued");
        return false;
    }
    uintptr_t skillEl = feGetElementById(g_tsSkillElemId);
    float sx = 0, sy = 0;
    if (!skillEl || !elemCenter(skillEl, &sx, &sy)) {
        logLine("targeting: click-commit not attempted, skill element 0x%llx is not on screen",
                (unsigned long long)g_tsSkillElemId);
        return false;
    }
    moveCursorTo(sx, sy);
    enqueueSynthAt(SDL_EVT_MOUSEBUTTONDOWN, SDL_BUTTON_LEFT, 1, 0, sx, sy, 1);
    enqueueSynthAt(SDL_EVT_MOUSEBUTTONUP,   SDL_BUTTON_LEFT, 0, 0, sx, sy, 2);
    enqueueSynthAt(SDL_EVT_MOUSEMOTION,     0, 0, 0, tx, ty, 4);   // hover the target, then click
    enqueueSynthAt(SDL_EVT_MOUSEBUTTONDOWN, SDL_BUTTON_LEFT, 1, 0, tx, ty, 5);
    enqueueSynthAt(SDL_EVT_MOUSEBUTTONUP,   SDL_BUTTON_LEFT, 0, 0, tx, ty, 6);
    g_tsSelfArmSkill = g_tsSkill;
    g_tsSelfArmUntil = GetTickCount() + TS_SELF_ARM_MS;
    logLine("targeting: click-commit skill elem 0x%llx (%.0f,%.0f) -> %s (%.0f,%.0f) "
            "[own-arm latched for skill %p]",
            (unsigned long long)g_tsSkillElemId, sx, sy, what, tx, ty, (void*)g_tsSelfArmSkill);
    return true;
}

// ---- IS THE GAME **OBSERVABLY** HOLDING THIS SKILL RIGHT NOW? ----
static bool tsGameHoldsSkill(uintptr_t base, uintptr_t hero, uintptr_t skill, const char* why) {
    if (!hero || !skill) return false;
    uintptr_t held = 0;
    if (!safeReadPtr(hero + ACTOR_CUR_SKILL_OFF, &held)) return false;
    uint32_t state = 0;
    safeReadU32(mapRoot(base) + RAID_IN_COMBAT_OFF, &state);
    const bool ok = (held == skill) && ((int)state == TP_STATE_CHOOSING_HI);
    if (!ok)
        logLine("targeting: %s -- the game is NOT holding this skill (held=%p want=%p, "
                "battle state 0x%x)", why, (void*)held, (void*)skill, state);
    return ok;
}

// ---- ARM THE SKILL THE WAY THE GAME'S OWN 1..4 DOES: click its button ----
static bool tsArmSkill(uintptr_t base, int64_t skillElemId) {
    if (!skillElemId) return false;
    if (clickQueued()) {
        logLine("targeting: not arming, a click is already queued");
        return false;
    }
    uintptr_t el = feGetElementById(skillElemId);
    float sx = 0, sy = 0;
    if (!el || !elemCenter(el, &sx, &sy)) {
        logLine("targeting: not arming, skill element 0x%llx is not on screen",
                (unsigned long long)skillElemId);
        return false;
    }
    moveCursorTo(sx, sy);
    enqueueSynthAt(SDL_EVT_MOUSEBUTTONDOWN, SDL_BUTTON_LEFT, 1, 0, sx, sy, 1);
    enqueueSynthAt(SDL_EVT_MOUSEBUTTONUP,   SDL_BUTTON_LEFT, 0, 0, sx, sy, 2);
    logLine("targeting: arming -- clicked skill elem 0x%llx at (%.0f,%.0f)",
            (unsigned long long)skillElemId, sx, sy);
    return true;
}

static bool tsClickArmedTarget(uintptr_t base, float tx, float ty, const char* what) {
    if (clickQueued()) {
        logLine("targeting: armed commit not attempted, a click is already queued");
        return false;
    }
    moveCursorTo(tx, ty);                    // normally a no-op: the cursor is already here
    enqueueSynthAt(SDL_EVT_MOUSEBUTTONDOWN, SDL_BUTTON_LEFT, 1, 0, tx, ty, 1);
    enqueueSynthAt(SDL_EVT_MOUSEBUTTONUP,   SDL_BUTTON_LEFT, 0, 0, tx, ty, 2);
    logLine("targeting: ARMED commit -- one click on %s (%.0f,%.0f)", what, tx, ty);
    return true;
}

static bool tsRowClickPoint(uintptr_t base, const RvEntry& row, float* tx, float* ty) {
    if (!row.actor) return false;
    return row.enemy ? tsEnemyClickPoint(base, row.actor, tx, ty)
                     : tsClickPartyPoint(base, row.slot, tx, ty);
}

static bool tsClickPartyTarget(uintptr_t base, int rank) {
    float tx = 0, ty = 0;
    if (!tsClickPartyPoint(base, rank, &tx, &ty)) return false;
    char what[48];
    _snprintf(what, sizeof what, "rank %d", rank);
    what[sizeof what - 1] = 0;
    return tsClickTargetPoint(base, tx, ty, what);
}

static bool tsClickEnemyTarget(uintptr_t base, uintptr_t actor, int slot) {
    float tx = 0, ty = 0;
    if (!tsEnemyClickPoint(base, actor, &tx, &ty)) return false;
    char what[48];
    _snprintf(what, sizeof what, "enemy slot %d", slot);
    what[sizeof what - 1] = 0;
    return tsClickTargetPoint(base, tx, ty, what);
}

static int tsAreaMembers(uintptr_t base, const RvEntry& focus, RvEntry* out, int maxOut) {
    uint32_t mask = 0;
    if (!safeReadU32(g_tsSkill + SKILL_TGTMASK_OFF, &mask)) return 0;
    if (!(mask & SKILL_AREA_BIT)) return 0;

    RvEntry all[RV_MAX_ROWS];
    int n = rvBuildRoom(base, all, RV_MAX_ROWS);

    bool friendly = tsSameSide(focus);
    int lo = 1 << 30, hi = -1;
    if (friendly) {
        for (int i = 0; i < n; i++) {
            if (all[i].enemy != focus.enemy || !all[i].actor) continue;
            if (!sehIsValidTarget(base, g_tsSkill, g_tsPerformer, all[i].actor)) continue;
            if (all[i].idx < lo) lo = all[i].idx;
            if (all[i].idx > hi) hi = all[i].idx;
        }
    }

    int got = 0;
    for (int i = 0; i < n && got < maxOut; i++) {
        if (all[i].enemy != focus.enemy || !all[i].actor) continue;
        bool hit = friendly ? (all[i].idx >= lo && all[i].idx <= hi)
                            : sehIsValidTarget(base, g_tsSkill, g_tsPerformer, all[i].actor);
        if (hit) out[got++] = all[i];
    }
    return got;
}

// ---- THE TARGETING PREVIEW ----

typedef int  (*ActorIntFn)(void*);
typedef void* (*ActorPtrFn)(void*);
typedef char (*ActorBoolFn)(void*);
typedef void (*BuffUpkeepFn)(void*, void*, void*, void*);

static bool tpVCallInt(uintptr_t actor, uintptr_t slot, int* out) {
    __try {
        uintptr_t vt = *reinterpret_cast<uintptr_t*>(actor);
        *out = (*reinterpret_cast<ActorIntFn*>(vt + slot))(reinterpret_cast<void*>(actor));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
static bool tpVCallPtr(uintptr_t actor, uintptr_t slot, uintptr_t* out) {
    __try {
        uintptr_t vt = *reinterpret_cast<uintptr_t*>(actor);
        *out = reinterpret_cast<uintptr_t>(
                   (*reinterpret_cast<ActorPtrFn*>(vt + slot))(reinterpret_cast<void*>(actor)));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
uintptr_t abActorClass(uintptr_t actor) {
    uintptr_t cls = 0;
    if (!actor || !tpVCallPtr(actor, VT_ACTOR_CLASS, &cls)) return 0;
    return cls > 0x10000 ? cls : 0;
}

static bool tpVCallBool(uintptr_t actor, uintptr_t slot, bool* out) {
    __try {
        uintptr_t vt = *reinterpret_cast<uintptr_t*>(actor);
        *out = (*reinterpret_cast<ActorBoolFn*>(vt + slot))(reinterpret_cast<void*>(actor)) != 0;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

static bool tpBuffUpkeep(uintptr_t base, uintptr_t self, uintptr_t attacker,
                         uintptr_t defender, uintptr_t skill) {
    __try {
        ((BuffUpkeepFn)(base + BUFF_UPKEEP_RVA))(
            reinterpret_cast<void*>(self), reinterpret_cast<void*>(attacker),
            reinterpret_cast<void*>(defender), reinterpret_cast<void*>(skill));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

static bool tpPrimeMatchup(uintptr_t base, uintptr_t hero, uintptr_t target, uintptr_t skill) {
    int heroRank = 0, tgtRank = 0;
    if (!tpVCallInt(hero, VT_ACTOR_RANK_INDEX, &heroRank)) return false;
    if (!tpVCallInt(target, VT_ACTOR_RANK_INDEX, &tgtRank)) return false;

    if (!tpBuffUpkeep(base, hero, 0, target, skill)) return false;
    if (!safeWriteU32(hero + ACTOR_VS_RANK_A_OFF, 0)) return false;
    if (!safeWriteU32(hero + ACTOR_VS_RANK_B_OFF, (uint32_t)tgtRank)) return false;

    if (!tpBuffUpkeep(base, target, hero, 0, skill)) return false;
    if (!safeWriteU32(target + ACTOR_VS_RANK_A_OFF, (uint32_t)heroRank)) return false;
    if (!safeWriteU32(target + ACTOR_VS_RANK_B_OFF, 0)) return false;
    return true;
}

static bool tpPreviewShown(uintptr_t base, uintptr_t hero, uintptr_t skill) {
    uintptr_t root = mapRoot(base);
    if (!root) return false;
    if (g_tsUserSel) return false;

    uint8_t nonAttack = 0;
    if (!safeReadU8(skill + SKILL_NONATTACK_OFF, &nonAttack) || nonAttack != 0) return false;

    uint32_t state = 0;
    uint8_t  over  = 0;
    bool stateOk = safeReadU32(root + RAID_IN_COMBAT_OFF, &state) &&
                   (int)state >= TP_STATE_CHOOSING_LO && (int)state <= TP_STATE_CHOOSING_HI;
    if (!stateOk) {
        if (!safeReadU8(root + RAID_BATTLE_OVERRIDE_OFF, &over) || over == 0) return false;
    }

    uintptr_t turn = 0;
    if (!safeReadPtr(root + RAID_TURN_ACTOR_OFF, &turn) || turn != hero) return false;
    return true;
}

static bool tpHitChance(uintptr_t base, uintptr_t hero, uintptr_t target, float* out) {
    float acc = 0.0f, dodge = 0.0f, lo = 0.0f, hi = 0.0f;
    if (!csStatOwnMask(target, CS_STAT_DEF_OFF, &dodge)) return false;
    if (!csStatOwnMask(hero, CS_STAT_ATT_OFF, &acc)) return false;
    if (!abReadF32(base + TP_CUTOFF_MISS_RVA, &lo)) return false;
    if (!abReadF32(base + TP_CUTOFF_HIT_RVA, &hi)) return false;
    float v = acc - dodge;
    *out = (lo > v) ? lo : (hi < v ? hi : v);
    return true;
}

static bool tpDamageRange(uintptr_t base, uintptr_t hero, uintptr_t target, uintptr_t skill,
                          int* lo, int* hi) {
    float prot = 0.0f;
    uint8_t ignoreProt = 0;
    bool skipProt = skill && safeReadU8(skill + SKILL_IGNORE_PROT_OFF, &ignoreProt) && ignoreProt != 0;
    if (!skipProt) {
        float tgtProt = 0.0f, pierce = 0.0f;
        if (!csStatOwnMask(target, CS_STAT_PROT_OFF, &tgtProt)) return false;
        if (!abReadF32(hero + ACTOR_ARM_PIERCE_OFF, &pierce)) return false;
        prot = tgtProt - pierce;
        if (prot < 0.0f) prot = 0.0f;
    }

    float dLo = 0.0f, dHi = 0.0f;
    if (!csStatOwnMask(hero, CS_STAT_DMGLO_OFF, &dLo)) return false;
    if (!csStatOwnMask(hero, CS_STAT_DMGHI_OFF, &dHi)) return false;
    dLo = ceilf(dLo);
    dHi = ceilf(dHi);

    float cutLo = roundf(prot * dLo), cutHi = roundf(prot * dHi);
    if (prot > 0.0f) {                       // a PROT'd hit always loses at least one point
        if (cutLo < 1.0f) cutLo = 1.0f;
        if (cutHi < 1.0f) cutHi = 1.0f;
    }
    float recv = 0.0f;
    if (!abReadF32(target + ACTOR_DMG_RECV_OFF, &recv)) return false;

    float remLo = dLo - cutLo; if (remLo < 0.0f) remLo = 0.0f;
    float remHi = dHi - cutHi; if (remHi < 0.0f) remHi = 0.0f;
    *lo = (int)roundf((float)(int)remLo * (recv + 1.0f));
    *hi = (int)roundf((float)(int)remHi * (recv + 1.0f));
    return true;
}

static bool tpDeathblowChance(uintptr_t base, uintptr_t hero, uintptr_t target, uintptr_t skill,
                              float* out) {
    float chance = 0.0f, resist = 0.0f;
    if (!abReadF32(base + TP_DEATHBLOW_RVA, &chance)) return false;
    if (!csResistValue(base, target, CS_RESIST_DEATHBLOW_IDX, &resist)) return false;

    uint8_t ignore = 0;
    if (skill && safeReadU8(skill + SKILL_IGN_DEATHBLW_OFF, &ignore) && ignore != 0) {
        chance += resist;
    } else {
        float mod = 0.0f;
        if (!abReadF32(hero + ACTOR_DEATHBLOW_OFF, &mod)) return false;
        chance += mod;
    }
    *out = chance - resist;
    return true;
}

static bool tpStress(uintptr_t hero, uintptr_t target, uintptr_t skill, bool isMonster, int* out) {
    if (!isMonster) return false;

    uintptr_t cls = 0;
    if (tpVCallPtr(target, VT_ACTOR_CLASS, &cls) && cls) {
        char id[32];
        if (safeReadCStr(cls + 0x48, id, sizeof id) && strcmp(id, "corpse") == 0) return false;
    }

    uintptr_t beg = 0, end = 0;
    if (!safeReadPtr(skill + SKILL_EFFECTS_OFF, &beg)) return false;
    if (!safeReadPtr(skill + SKILL_EFFECTS_END_OFF, &end)) return false;

    float total = 0.0f;
    int guard = 0;
    for (uintptr_t p = beg; p && p != end && guard < 64; p += sizeof(uintptr_t), guard++) {
        uintptr_t eff = 0;
        if (!safeReadPtr(p, &eff) || !eff) continue;
        float s = 0.0f;
        if (abReadF32(eff + EFFECT_STRESS_OFF, &s)) total += s;
    }
    if (total <= 0.0f) return false;          // no stress is not a number worth speaking

    float dealt = 0.0f, recv = 0.0f;
    if (!abReadF32(hero + ACTOR_STRESS_DEALT_OFF, &dealt)) return false;
    if (!abReadF32(target + ACTOR_STRESS_RECV_OFF, &recv)) return false;
    *out = (int)roundf(total * (dealt + 1.0f + recv));
    return *out != 0;
}

static void tpPhrase(char* clause, int clausesz, const char* fmt, ...) {
    char frag[96];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf(frag, sizeof frag, fmt, ap);
    va_end(ap);
    frag[sizeof frag - 1] = 0;
    if (!frag[0]) return;
    if (clause[0]) strncat(clause, ", ", clausesz - strlen(clause) - 1);
    strncat(clause, frag, clausesz - strlen(clause) - 1);
}

static bool tpBuildPreview(uintptr_t base, uintptr_t target, bool isMonster,
                           char* clause, int clausesz) {
    clause[0] = 0;
    if (!g_tsSkill || !g_tsPerformer || !target) return false;
    const uintptr_t hero = g_tsPerformer, skill = g_tsSkill;

    if (!tpPreviewShown(base, hero, skill)) {
        logLine("targetinfo: the game would not show a preview here — staying quiet");
        return false;
    }
    uintptr_t prevSkill = 0;
    bool skillSwapped = safeReadPtr(hero + ACTOR_CUR_SKILL_OFF, &prevSkill) &&
                        safeWriteU64(hero + ACTOR_CUR_SKILL_OFF, (uint64_t)skill);
    if (!skillSwapped) {
        logLine("targetinfo: could not set the selected skill — hit chance will read low");
    }

    bool primed = tpPrimeMatchup(base, hero, target, skill);
    if (!primed) {
        if (skillSwapped) safeWriteU64(hero + ACTOR_CUR_SKILL_OFF, (uint64_t)prevSkill);
        logLine("targetinfo: matchup priming failed for target=%p — omitting the preview",
                (void*)target);
        return false;
    }

    int got = 0;

    float hit = 0.0f;
    if (tpHitChance(base, hero, target, &hit)) {
        tpPhrase(clause, clausesz, "%.0f%% to hit", (double)(hit * TP_PERCENT_SCALE));
        got++;
    } else logLine("targetinfo: hit chance unreadable");
    {
        float dacc = 0.0f, ddod = 0.0f, dlo = 0.0f, dhi = 0.0f;
        csStatOwnMask(hero, CS_STAT_ATT_OFF, &dacc);
        csStatOwnMask(target, CS_STAT_DEF_OFF, &ddod);
        abReadF32(base + TP_CUTOFF_MISS_RVA, &dlo);
        abReadF32(base + TP_CUTOFF_HIT_RVA, &dhi);
        logLine("targetinfo hit: acc=%.4f dodge=%.4f -> %.4f, cutoffs [%.4f..%.4f], skill=%p prev=%p",
                dacc, ddod, hit, dlo, dhi, (void*)skill, (void*)prevSkill);
    }

    bool atDeathsDoor = false;
    if (!tpVCallBool(target, VT_ACTOR_DEATHS_DOOR, &atDeathsDoor)) atDeathsDoor = false;
    if (atDeathsDoor) {
        float db = 0.0f;
        if (tpDeathblowChance(base, hero, target, skill, &db)) {
            tpPhrase(clause, clausesz, "%.0f%% deathblow", (double)(db * TP_PERCENT_SCALE));
            got++;
        } else logLine("targetinfo: deathblow chance unreadable");
    } else {
        int lo = 0, hi = 0;
        if (tpDamageRange(base, hero, target, skill, &lo, &hi)) {
            tpPhrase(clause, clausesz, "%d-%d damage", lo, hi);
            got++;
        } else logLine("targetinfo: damage range unreadable");
    }

    float crit = 0.0f;
    if (csStatOwnMask(hero, CS_STAT_CRIT_OFF, &crit)) {
        tpPhrase(clause, clausesz, "%.1f%% crit", (double)(crit * TP_PERCENT_SCALE));
        got++;
    } else logLine("targetinfo: crit chance unreadable");

    int stress = 0;
    if (tpStress(hero, target, skill, isMonster, &stress)) {
        tpPhrase(clause, clausesz, "%d stress", stress);
        got++;
    }

    if (skillSwapped) safeWriteU64(hero + ACTOR_CUR_SKILL_OFF, (uint64_t)prevSkill);

    logLine("targetinfo: target=%p rows=%d deathsdoor=%d", (void*)target, got, atDeathsDoor ? 1 : 0);
    return got > 0;
}

static bool tsShortName(uintptr_t base, const RvEntry& e, char* out, int outsz) {
    out[0] = 0;
    if (rvCorpseWord(base, e.actor, out, outsz)) return true;
    if (e.enemy) return rvMonsterName(base, e.actor, out, outsz);
    char cls[96];
    return abHeroNameClassOf(base, e.actor, out, outsz, cls, sizeof cls);
}

static bool tsIsMoveSkill(uintptr_t skill) {
    uint8_t b = 0;
    return skill && safeReadU8(skill + SKILL_ISMOVE_OFF, &b) && b != 0;
}

static bool tsMoveRowText(uintptr_t base, const RvEntry& e, char* out, int outsz) {
    char who[256];
    if (!abHeroLabelOf(base, e.actor, who, sizeof who) || !who[0]) return false;
    _snprintf(out, outsz, axs(AXS_TS_ROW_POSITION_FMT), e.slot, who);
    out[outsz - 1] = 0;
    return true;
}

static bool tsTargetRowText(uintptr_t base, const RvEntry& e, char* out, int outsz) {
    out[0] = 0;
    if (e.kind != RV_ACTOR || !e.actor) return false;

    char name[160], cls[160], dead[96];
    bool isCorpse = rvCorpseWord(base, e.actor, dead, sizeof dead);
    if (isCorpse) {
        name[0] = 0;
        if (spActorIsHero(base, e.actor))
            abHeroNameClassOf(base, e.actor, name, sizeof name, cls, sizeof cls);
        if (name[0]) { strncat(name, ", ", sizeof name - strlen(name) - 1);
                       strncat(name, dead, sizeof name - strlen(name) - 1); }
        else         _snprintf(name, sizeof name, "%s", dead);
    } else if (!spActorIsHero(base, e.actor)) {
        if (!rvMonsterName(base, e.actor, name, sizeof name) || !name[0]) return false;
    } else {
        if (!abHeroNameClassOf(base, e.actor, name, sizeof name, cls, sizeof cls)) return false;
        if (!name[0]) {
            if (!cls[0]) return false;
            _snprintf(name, sizeof name, "%s", cls);
        }
    }
    _snprintf(out, outsz, "%s", name);
    out[outsz - 1] = 0;

    char clause[512];
    if (tpBuildPreview(base, e.actor, e.enemy, clause, sizeof clause) && clause[0])
        rvAppendComma(out, outsz, clause);

    char frag[160];
    if (abHeroHealth(base, e.actor, frag, sizeof frag)) rvAppendComma(out, outsz, frag);
    else logLine("target row: health unreadable for actor=%p", (void*)e.actor);
    bool hasStress = !isCorpse && (spActorIsHero(base, e.actor) || pitInMatch(base));
    if (hasStress && abHeroStress(base, e.actor, frag, sizeof frag))
        rvAppendComma(out, outsz, frag);
    strncat(out, ".", outsz - strlen(out) - 1);

    rvPosPhrase(e.slot, e.slotEnd, e.enemy, frag, sizeof frag);
    abAppendFrag(out, outsz, frag);
    return true;
}

static bool tsComposeRow(uintptr_t base, const RvEntry* rows, int n, int cur, char* out, int outsz) {
    if (cur < 0 || cur >= n) return false;
    if (g_tsMove && tsMoveRowText(base, rows[cur], out, outsz)) {
        logLine("target row %d/%d (move -> position %d) -> \"%s\"", cur, n, rows[cur].slot, out);
        return true;
    }
    if (!tsTargetRowText(base, rows[cur], out, outsz)) {
        char pos[64];
        rvPosForEntry(rows[cur], pos, sizeof pos);
        _snprintf(out, outsz, "%s. %s.", axs(rows[cur].enemy ? AXS_RV_LABEL_ENEMY : AXS_RV_LABEL_PARTY_MEMBER), pos);
        out[outsz - 1] = 0;
    }

    RvEntry area[RV_MAX_ROWS];
    int na = tsAreaMembers(base, rows[cur], area, RV_MAX_ROWS);
    // One member is not an area worth announcing — it is the row just spoken.
    if (na > 1) {
        char list[512]; list[0] = 0;
        for (int i = 0; i < na; i++) {
            char nm[160];
            if (!tsShortName(base, area[i], nm, sizeof nm)) continue;
            if (list[0]) strncat(list, ", ", sizeof list - strlen(list) - 1);
            strncat(list, nm, sizeof list - strlen(list) - 1);
        }
        if (list[0]) {
            char frag[600];
            _snprintf(frag, sizeof frag, axs(AXS_TS_HITS_FMT), na, list);
            frag[sizeof frag - 1] = 0;
            abAppendFrag(out, outsz, frag);
        }
    }

    logLine("target row %d/%d (%s idx=%d slot=%d) area=%d -> \"%s\"",
            cur, n, rows[cur].enemy ? "enemy" : "party", rows[cur].idx, rows[cur].slot, na, out);
    return true;
}

// ---- THE REAL CURSOR FOLLOWS THE SPOKEN ONE ----
static void tsFollowCursor(uintptr_t base, const RvEntry* rows, int n, int cur) {
    if (cur < 0 || cur >= n || clickQueued()) return;
    float tx = 0, ty = 0;
    if (!tsRowClickPoint(base, rows[cur], &tx, &ty)) return;
    moveCursorTo(tx, ty);
}

static void tsSpeakRow(uintptr_t base, const RvEntry* rows, int n, int cur) {
    char out[768];
    if (tsComposeRow(base, rows, n, cur, out, sizeof out)) postSpeech(out);
    tsFollowCursor(base, rows, n, cur);
}

static int tsRoomIndexOf(uintptr_t base, const RvEntry& e) {
    RvEntry all[RV_MAX_ROWS];
    int n = rvBuildRoom(base, all, RV_MAX_ROWS);
    if (!e.actor) return -1;
    for (int i = 0; i < n; i++) if (all[i].actor && all[i].actor == e.actor) return i;
    return -1;
}

void tsSetActive(bool on) {
    g_tsActive = on;
    g_tsCursor = -1;
    g_tsArmed = false;
    g_rvCursor  = -1;
    g_rvTipLine = 0;
    g_rvTipCol  = 0;
    if (!on) {
        g_tsSkill = 0; g_tsPerformer = 0; g_tsSkillName[0] = 0;
        g_tsMove = false; g_tsMoverName[0] = 0;
        g_tsUserSel = false;
    }
    logLine("targeting %s", on ? "active" : "inactive");
}

static void tsCloseToBar(uintptr_t base) {
    int back = g_tsReturnCursor;
    tsSetActive(false);
    abSetActive(base, true);
    if (back >= 0) g_abCursor = back;
}

// ---- CALIBRATION PROBE: the actor's screen X against geometry the mod ALREADY trusts ----
static int g_tsCalibDumps = 0;
static const int TS_CALIB_MAX_DUMPS = 24;

static void tsDumpActorX(uintptr_t base) {
    if (!axDebugLogEnabled()) return;                 // nothing diagnostic runs while logging is off
    if (g_tsCalibDumps >= TS_CALIB_MAX_DUMPS) return;
    g_tsCalibDumps++;

    RvEntry rows[RV_MAX_ROWS];
    const int n = rvBuildRoom(base, rows, RV_MAX_ROWS);

    uintptr_t heroes[8];
    float hx[8];
    const int hn = feCollectFamilyByX(base, TS_HERO_ELEM_FAMILY, TS_HERO_ELEM_OWNER, heroes, hx, 8);

    logLine("actorx: ---- calibration %d/%d -- %d room row(s), %d hero element(s) ----",
            g_tsCalibDumps, TS_CALIB_MAX_DUMPS, n, hn);

    for (int i = 0; i < hn; i++) {
        float px = 0, py = 0, cx = 0, cy = 0;
        const bool gotP = elemPos(heroes[i], &px, &py);
        const bool gotC = elemCenter(heroes[i], &cx, &cy);
        logLine("actorx:   heroElem[%d] (geometry says rank %d) pos=(%.1f,%.1f) ctr=(%.1f,%.1f) "
                "read=%d%d", i, i + 1, px, py, cx, cy, (int)gotP, (int)gotC);
    }

    for (int i = 0; i < n; i++) {
        if (!rows[i].actor) continue;
        uint32_t bits = 0;
        if (!safeReadU32(rows[i].actor + ACTOR_SCREEN_X_OFF, &bits)) {
            logLine("actorx:   %-5s slot %d actor=%p -- X UNREADABLE",
                    rows[i].enemy ? "enemy" : "hero", rows[i].slot, (void*)rows[i].actor);
            continue;
        }
        const float ax = u32AsFloatM(bits);
        uint32_t mask = 0;
        safeReadU32(rows[i].actor + ACTOR_RANK_MASK_OFF, &mask);

        if (!rows[i].enemy && rows[i].slot >= 1 && rows[i].slot <= hn) {
            float px = 0, py = 0, cx = 0, cy = 0;
            elemPos(heroes[rows[i].slot - 1], &px, &py);
            elemCenter(heroes[rows[i].slot - 1], &cx, &cy);
            logLine("actorx:   hero  slot %d rank=0x%x actorX=%.1f | elemPos.x=%.1f d=%+.1f | "
                    "elemCtr=(%.1f,%.1f) d=%+.1f", rows[i].slot, mask, ax, px, ax - px, cx, cy,
                    ax - cx);
        } else {
            logLine("actorx:   %-5s slot %d rank=0x%x actorX=%.1f%s",
                    rows[i].enemy ? "enemy" : "hero", rows[i].slot, mask, ax,
                    rows[i].enemy ? "  (no element -- this is what a click would use)" : "");
        }
    }

    // ---- THE CLOSING MEASUREMENT: the far side's OWN screen geometry ----
    uintptr_t bars[16];
    float bx[16];
    const int bn = feCollectFamilyByX(base, TS_STATBAR_ELEM_FAMILY, TS_STATBAR_ELEM_OWNER,
                                      bars, bx, 16);
    logLine("actorx:   -- %d stat bar(s) by owner 'stbs' (living actors only) --", bn);
    for (int i = 0; i < bn; i++) {
        int64_t id = 0;
        safeReadI64(bars[i] + ELEM_ID_OFF, &id);
        float px = 0, py = 0, cx = 0, cy = 0;
        elemPos(bars[i], &px, &py);
        elemCenter(bars[i], &cx, &cy);
        uint32_t wb = 0, hb = 0;
        safeReadU32(bars[i] + ELEM_SIZE_OFF,     &wb);
        safeReadU32(bars[i] + ELEM_SIZE_OFF + 4, &hb);
        logLine("actorx:     bar[%d] id=0x%llx pos=(%.1f,%.1f) ctr=(%.1f,%.1f) size=(%.1fx%.1f) %s",
                i, (unsigned long long)id, px, py, cx, cy,
                u32AsFloatM(wb), u32AsFloatM(hb), cx > 960.0f ? "ENEMY side" : "party side");
    }
}

static bool tsBegin(uintptr_t base, uintptr_t skill, const char* skillName, int abCursor) {
    const bool adoptArmed = g_tsAdoptArmed;
    g_tsAdoptArmed = false;

    uintptr_t perf = abSelectedHero(base);
    if (!skill || !perf) {
        logLine("targeting: no skill (%p) or performer (%p)", (void*)skill, (void*)perf);
        postSpeech(axs(AXS_AB_CANT_USE_NOW));
        return true;
    }
    const bool isMove = tsIsMoveSkill(skill);

    if (!isMove && !abBattleLive(mapRoot(base))) {
        logLine("targeting: refused \"%s\" — not in combat (battle state 0)",
                skillName ? skillName : "?");
        postSpeech(axs(AXS_TS_ONLY_IN_COMBAT));
        return true;
    }
    if (!abHeroMayAct(mapRoot(base), perf, skillName)) {
        postSpeech(axs(AXS_TS_NOT_YOUR_TURN));
        return true;
    }
    if (skillOutOfPosition(skill, perf)) {
        uint32_t launch = 0;
        char where[80] = "another position";
        if (safeReadU32(skill + SKILL_LAUNCH_MASK_OFF, &launch) && launch)
            skillLaunchPositions(launch, where, sizeof where);
        char msg[160];
        _snprintf(msg, sizeof msg, axs(AXS_TS_MUST_BE_IN_FMT), where);
        msg[sizeof msg - 1] = 0;
        logLine("targeting: \"%s\" out of position (launch=0x%x) -> %s",
                skillName ? skillName : "?", launch, where);
        postSpeech(msg);
        return true;
    }
    bool spent = skillUsedUp(skill, perf), hpGate = skillHpGateUnmet(skill, perf);
    if (spent || hpGate) {
        char why[256], msg[320];
        abSkillLimitsText(base, skill, perf, why, sizeof why);
        if (why[0]) _snprintf(msg, sizeof msg, axs(AXS_TS_CANT_USE_REASON_FMT), why);
        else        _snprintf(msg, sizeof msg, "%s", axs(AXS_AB_CANT_USE_NOW));
        msg[sizeof msg - 1] = 0;
        logLine("targeting: \"%s\" refused — %s", skillName ? skillName : "?",
                spent ? "no per-battle uses left" : "performer hp outside the required range");
        postSpeech(msg);
        return true;
    }
    if (!sehHasAnyTarget(base, skill, perf)) {
        logLine("targeting: game reports no legal target for \"%s\"", skillName ? skillName : "?");
        postSpeech(axs(AXS_TS_NO_VALID_TARGETS));
        return true;
    }

    g_tsSkill = skill;
    g_tsPerformer = perf;
    _snprintf(g_tsSkillName, sizeof g_tsSkillName, "%s", skillName ? skillName : axs(AXS_TS_SKILL_FALLBACK));
    g_tsSkillName[sizeof g_tsSkillName - 1] = 0;
    g_tsReturnCursor = abCursor;

    RvEntry rows[RV_MAX_ROWS];
    int n = tsBuildTargets(base, rows, RV_MAX_ROWS);
    if (n <= 0) {
        logLine("targeting: HasAnyTarget said yes but no row passed IsValidTarget — check the call");
        postSpeech(axs(AXS_TS_NO_VALID_TARGETS));
        g_tsSkill = 0; g_tsPerformer = 0;
        return true;
    }

    if (g_abActive) abSetActive(base, false);   // the bar and the target list both own Left/Right
    tsSetActive(true);
    if (!isMove) {
        if (adoptArmed || tsGameHoldsSkill(base, perf, skill, "arming")) {
            g_tsArmed = true;
            logLine("targeting: the GAME is already holding \"%s\" -- adopting, not clicking",
                    skillName ? skillName : "?");
        } else if (!g_tsArmed) {
            g_tsArmed = tsArmSkill(base, g_tsSkillElemId);
        }
    }
    g_tsMove = isMove;
    if (isMove && !abHeroLabelOf(base, perf, g_tsMoverName, sizeof g_tsMoverName)) g_tsMoverName[0] = 0;

    int start = -1;
    if (!isMove) {
        bool attack = false;
        for (int i = 0; i < n; i++) if (rows[i].enemy) { attack = true; break; }
        for (int i = 0; i < n; i++) {
            if (rows[i].enemy != attack) continue;
            if (start < 0 || rows[i].slot < rows[start].slot) start = i;
        }
    }

    logLine("targeting: begin \"%s\"%s perf=%p targets=%d start=%d", g_tsSkillName,
            isMove ? " (MOVE)" : "", (void*)perf, n, start);

    tsDumpActorX(base);

    char msg[1100];
    _snprintf(msg, sizeof msg, axs(isMove ? AXS_TS_CHOOSE_POSITION_FMT : AXS_TS_CHOOSE_TARGET_FMT),
              g_tsSkillName, n);
    msg[sizeof msg - 1] = 0;
    if (start >= 0) {
        g_tsCursor = start;                          // after tsSetActive, which resets it to -1
        g_rvCursor = tsRoomIndexOf(base, rows[start]); // point the Ctrl+Up/Down tooltip reader here
        char row[768];
        if (tsComposeRow(base, rows, n, start, row, sizeof row)) abAppendFrag(msg, sizeof msg, row);
    }
    postSpeech(msg);
    return true;
}

// ---- A MONSTER'S TURN THAT THE PLAYER AIMS: "Come Unto Your Maker" ----
static bool tsUserSelPending(uintptr_t base, uintptr_t* monster, uintptr_t* skill) {
    uintptr_t root = mapRoot(base);
    if (!root) return false;
    uint32_t state = 0;
    uint8_t  over  = 0;
    const bool stateOk = safeReadU32(root + RAID_IN_COMBAT_OFF, &state) &&
                         (int)state >= TP_STATE_CHOOSING_LO && (int)state <= TP_STATE_CHOOSING_HI;
    if (!stateOk && (!safeReadU8(root + RAID_BATTLE_OVERRIDE_OFF, &over) || over == 0)) return false;

    uintptr_t turn = 0, held = 0;
    uint8_t isMonster = 0, friendly = 0, userSel = 0;
    if (!safeReadPtr(root + RAID_TURN_ACTOR_OFF, &turn) || !turn) return false;
    if (!safeReadU8(turn + ACTOR_IS_MONSTER_OFF, &isMonster) || !isMonster) return false;
    if (!safeReadPtr(turn + ACTOR_CUR_SKILL_OFF, &held) || !held) return false;
    if (!safeReadU8(held + SKILL_NONATTACK_OFF, &friendly) || friendly) return false;
    if (!safeReadU8(held + SKILL_USER_SELECTED_OFF, &userSel) || !userSel) return false;

    uintptr_t tBegin = 0, tEnd = 0;
    if (!safeReadPtr(turn + ACTOR_TARGETS_OFF, &tBegin) ||
        !safeReadPtr(turn + ACTOR_TARGETS_OFF + 8, &tEnd) || tBegin != tEnd) return false;

    *monster = turn;
    *skill   = held;
    return true;
}

static void tsUserSelHeader(uintptr_t base, int n, char* out, int outsz) {
    _snprintf(out, outsz, axs(AXS_TS_CHOOSE_TARGET_FMT), g_tsSkillName, n);
    out[outsz - 1] = 0;
}

static bool tsBeginUserSelected(uintptr_t base, uintptr_t monster, uintptr_t skill) {
    char id[64] = "", key[128], name[160] = "";
    if (safeReadCStr(skill + SKILL_ID_OFF, id, sizeof id) && id[0]) {
        _snprintf(key, sizeof key, MT_SKILL_KEY_FMT, id);
        key[sizeof key - 1] = 0;
        if (!resolveKey(base, key, name, sizeof name) || !name[0])
            _snprintf(name, sizeof name, "%s", id);   // the raw id beats a bare "Skill"
        name[sizeof name - 1] = 0;
        abStripMarkup(name);
    }

    const int back = g_abActive ? g_abCursor : -1;
    if (g_tsActive) tsSetActive(false);
    g_tsSkill     = skill;
    g_tsPerformer = monster;
    g_tsUserSel   = true;
    _snprintf(g_tsSkillName, sizeof g_tsSkillName, "%s", name[0] ? name : axs(AXS_TS_SKILL_FALLBACK));
    g_tsSkillName[sizeof g_tsSkillName - 1] = 0;

    RvEntry rows[RV_MAX_ROWS];
    const int n = tsBuildTargets(base, rows, RV_MAX_ROWS);
    if (n <= 0) {
        logLine("usersel: \"%s\" (%s) is waiting on the player but NO party row passed "
                "IsValidTarget", g_tsSkillName, id);
        g_tsSkill = 0; g_tsPerformer = 0; g_tsUserSel = false; g_tsSkillName[0] = 0;
        postSpeech(axs(AXS_TS_NO_VALID_TARGETS));
        return false;
    }

    if (g_abActive) abSetActive(base, false);   // the bar and the target list both own Left/Right
    tsSetActive(true);
    g_tsArmed        = true;                    // the GAME holds the skill: Enter is one click
    g_tsSkillElemId  = 0;                       // and there is no button to fall back on
    g_tsReturnCursor = back;

    int start = 0;                              // the lowest rank, as every other aim starts
    for (int i = 1; i < n; i++) if (rows[i].slot < rows[start].slot) start = i;
    g_tsCursor = start;
    g_rvCursor = tsRoomIndexOf(base, rows[start]);

    logLine("usersel: the GAME is waiting on the player for \"%s\" (%s) -- monster=%p skill=%p, "
            "%d party target(s)", g_tsSkillName, id, (void*)monster, (void*)skill, n);

    char msg[1100], row[768];
    tsUserSelHeader(base, n, msg, sizeof msg);
    if (tsComposeRow(base, rows, n, start, row, sizeof row)) abAppendFrag(msg, sizeof msg, row);
    postSpeech(msg);
    tsFollowCursor(base, rows, n, start);
    return true;
}

static const uint32_t US_SETTLE_MS = 300;
static uintptr_t g_usSeen    = 0;     // the monster seen pending, and since when
static uint32_t  g_usSeenAt  = 0;
static uintptr_t g_usOffered = 0;
static uintptr_t g_usDone    = 0;
static uint32_t  g_usClickedAt = 0;
static const uint32_t US_CLICK_TAKE_MS = 2000;   // the click lands in ~80 ms

static void serviceUserSelectedTarget(uintptr_t base) {
    uintptr_t monster = 0, skill = 0;
    if (!tsUserSelPending(base, &monster, &skill)) {
        if (g_usOffered || g_usDone)
            logLine("usersel: the choice is no longer pending (offered=%p done=%p)",
                    (void*)g_usOffered, (void*)g_usDone);
        g_usSeen = 0;
        g_usOffered = 0;
        g_usDone = 0;
        g_usClickedAt = 0;
        if (g_tsActive && g_tsUserSel) {
            logLine("usersel: closing the list -- the game no longer waits on it");
            tsCloseToBar(base);
        }
        return;
    }
    if (g_usSeen != monster) { g_usSeen = monster; g_usSeenAt = GetTickCount(); return; }
    if (GetTickCount() - g_usSeenAt < US_SETTLE_MS) return;
    if (g_usDone == monster) {                            // clicked, or refused once already
        if (!g_usClickedAt || GetTickCount() - g_usClickedAt < US_CLICK_TAKE_MS) return;
        logLine("usersel: still pending %u ms after our click -- it did not take, re-offering",
                (unsigned)(GetTickCount() - g_usClickedAt));
        g_usDone = 0;
        g_usOffered = 0;
        g_usClickedAt = 0;
    }
    if (g_tsActive && g_tsUserSel && g_tsPerformer == monster && g_tsSkill == skill) return;
    if (g_usOffered == monster && !g_abActive) return;
    if (clickQueued()) return;                            // never aim under a click in flight
    g_usOffered = monster;
    if (!tsBeginUserSelected(base, monster, skill)) g_usDone = monster;
}

static void tsConfirmUserSelected(uintptr_t base, const RvEntry& row) {
    uintptr_t monster = 0, skill = 0;
    if (!tsUserSelPending(base, &monster, &skill) || monster != g_tsPerformer || skill != g_tsSkill) {
        logLine("usersel: Enter, but the game is no longer waiting on \"%s\"", g_tsSkillName);
        tsCloseToBar(base);
        postSpeech(axs(AXS_AB_CANT_USE_NOW));
        return;
    }
    float tx = 0, ty = 0;
    char what[48];
    _snprintf(what, sizeof what, "hero slot %d", row.slot);
    what[sizeof what - 1] = 0;
    if (row.enemy || !tsRowClickPoint(base, row, &tx, &ty) ||
        !tsClickArmedTarget(base, tx, ty, what)) {
        logLine("usersel: the click on %s was NOT attempted -- the list stays open", what);
        postSpeech(axs(AXS_AB_CANT_USE_NOW));
        return;
    }
    g_usDone = monster;
    g_usClickedAt = GetTickCount();
    logLine("usersel: \"%s\" -> %s committed by CLICKING -- the game owns the turn from here",
            g_tsSkillName, what);
    tsCloseToBar(base);
}

static uintptr_t g_abCommitTgtActor = 0;
static uint32_t  g_abCommitTgtTick  = 0;
static const uint32_t AB_COMMIT_TGT_MS = 5000;   // outlives the longest skill presentation
bool abWasRecentCommitTarget(uintptr_t actor) {
    return actor && actor == g_abCommitTgtActor &&
           (GetTickCount() - g_abCommitTgtTick) <= AB_COMMIT_TGT_MS;
}

static void tsConfirm(uintptr_t base, const RvEntry& row) {
    uintptr_t root = mapRoot(base);
    if (!root) { postSpeech(axs(AXS_AB_CANT_USE_NOW)); tsSetActive(false); return; }

    char name[160];
    if (!tsShortName(base, row, name, sizeof name)) _snprintf(name, sizeof name, "%s", axs(AXS_TS_TARGET_FALLBACK));
    name[sizeof name - 1] = 0;

    if (!g_tsMove && !abBattleLive(root)) {
        logLine("targeting: refused commit \"%s\" — no battle running (state 0)", g_tsSkillName);
        tsCloseToBar(base);
        postSpeech(axs(AXS_TS_ONLY_IN_COMBAT));
        return;
    }

    if (!abHeroMayAct(root, g_tsPerformer, g_tsSkillName)) {
        tsCloseToBar(base);
        postSpeech(axs(AXS_TS_NOT_YOUR_TURN));
        return;
    }

    uint32_t pitState = 0;
    if (!pitBattleAcceptsCommand(base, &pitState)) {
        logLine("targeting: refused \"%s\" — the match is not accepting commands (battle state 0x%x)",
                g_tsSkillName, pitState);
        tsCloseToBar(base);
        postSpeech(axs(AXS_TS_MATCH_WAITING));
        return;
    }

    g_tsCommitOne    = row.actor;
    g_tsCommitVec[0] = (uintptr_t)&g_tsCommitOne;
    g_tsCommitVec[1] = (uintptr_t)(&g_tsCommitOne + 1);
    g_tsCommitVec[2] = (uintptr_t)(&g_tsCommitOne + 1);

    uint32_t bstate = 0;
    safeReadU32(root + RAID_IN_COMBAT_OFF, &bstate);
    int egate = -1, esize = -1;
    readInputEnableStack(base, &egate, &esize);
    logLine("targeting: commit \"%s\" -> actor=%p (%s slot %d) [battle state 0x%x, input stack %d]",
            g_tsSkillName, (void*)row.actor, row.enemy ? "enemy" : "party", row.slot, bstate, esize);

    g_abCommitTgtActor = row.actor;
    g_abCommitTgtTick  = GetTickCount();

    const bool isMove = g_tsMove;
    char mover[160];
    _snprintf(mover, sizeof mover, "%s", g_tsMoverName[0] ? g_tsMoverName : axs(AXS_AB_HERO_FALLBACK));
    mover[sizeof mover - 1] = 0;
    const int destSlot = row.slot;

    bool ok;
    if (isMove) {
        int rank = rvRankFromGame(row.actor);
        if (rank <= 0) {
            logLine("targeting: move has no readable rank mask on target %p — refusing",
                    (void*)row.actor);
            tsCloseToBar(base);
            postSpeech(axs(AXS_TS_CANT_MOVE_THERE));
            return;
        }
        int destIdx = rank - 1;

        // ---- ⚠⚠ Actor_MoveSkill BURNS THE TURN IN AN ARENA MATCH ----
        if (tsClickPartyTarget(base, row.slot)) {
            mvSnapshot(base, mover, destSlot, MV_CLICK_WATCH_MS);
            logLine("targeting: MOVE committed by CLICKING -> position %d", row.slot);
            tsCloseToBar(base);
            return;
        }

        if (pitInMatch(base)) {
            logLine("targeting: MOVE refused — position %d could not be clicked, and "
                    "Actor_MoveSkill would spend the turn as a pass in a match", row.slot);
            tsCloseToBar(base);
            postSpeech(axs(AXS_TS_CANT_MOVE_THERE));
            return;
        }

        logLine("targeting: MOVE via Actor_MoveSkill(battle, %p, %d) [target rank %d]",
                (void*)g_tsPerformer, destIdx, rank);
        mvSnapshot(base, mover, destSlot);
        ok = sehMoveSkill(base, root + BATTLE_OBJ_OFF, g_tsPerformer, destIdx);
        if (!ok) logLine("targeting: Actor_MoveSkill faulted");
        tsCloseToBar(base);
        return;
    }

    // ---- A FRIENDLY TARGET IS CLICKED, NOT CALLED ----
    // ---- AND NOW THE FAR SIDE IS CLICKED TOO ----
    bool clicked = false;
    if (!isMove && g_tsArmed &&
        tsGameHoldsSkill(base, g_tsPerformer, g_tsSkill, "armed commit")) {
        float tx = 0, ty = 0;
        char what[48];
        _snprintf(what, sizeof what, "%s slot %d", row.enemy ? "enemy" : "hero", row.slot);
        what[sizeof what - 1] = 0;
        clicked = tsRowClickPoint(base, row, &tx, &ty) && tsClickArmedTarget(base, tx, ty, what);
    }
    if (!clicked) {
        clicked = isMove ? false
                : row.enemy ? tsClickEnemyTarget(base, row.actor, row.slot)
                            : tsClickPartyTarget(base, row.slot);
    }
    if (clicked) {
        logLine("targeting: \"%s\" committed by CLICKING (%s) -- the game owns the turn "
                "from here", g_tsSkillName, row.enemy ? "enemy" : "friendly");
        tsCloseToBar(base);
        return;
    }

    if (pitInMatch(base)) {
        if (!pitCommitViaMachine(base, g_tsPerformer, g_tsSkill, &g_tsCommitOne, 1)) {
            logLine("targeting: refused \"%s\" — the turn could not be handed to the machine",
                    g_tsSkillName);
            tsCloseToBar(base);
            postSpeech(axs(AXS_TS_TURN_NOT_SENT));
            return;
        }
        ok = true;
    } else {
        ok = sehDoSkill(base, root + BATTLE_OBJ_OFF, g_tsPerformer, g_tsSkill, g_tsCommitVec);
        if (!ok) logLine("targeting: Actor_DoSkill faulted");
    }

    char msg[320];
    _snprintf(msg, sizeof msg, axs(ok ? AXS_TS_USED_ON_FMT : AXS_TS_COULDNT_USE_FMT), g_tsSkillName, name);
    msg[sizeof msg - 1] = 0;
    tsCloseToBar(base);
    postSpeech(msg);
}

bool routeTargetKey(uintptr_t base, uint32_t sym, uint16_t mod, uint8_t repeat) {
    if (mod & (KMOD_LALT | KMOD_RALT)) return false;

    if (mod & (KMOD_LCTRL | KMOD_RCTRL)) {
        if (sym == SDLK_LEFT || sym == SDLK_RIGHT)
            return rvTipPanelSwitch(base, sym == SDLK_RIGHT ? 1 : -1, repeat);
        if (sym != SDLK_UP && sym != SDLK_DOWN) return false;
        if (repeat) return true;
        rvSpeakTipLine(base, sym == SDLK_UP ? 1 : -1);
        return true;
    }

    if (sym == SDLK_ESCAPE) {
        if (repeat) return true;
        if (g_tsUserSel) {
            RvEntry rows[RV_MAX_ROWS];
            const int n = tsBuildTargets(base, rows, RV_MAX_ROWS);
            char msg[1100], row[768];
            tsUserSelHeader(base, n, msg, sizeof msg);
            if (g_tsCursor >= 0 && g_tsCursor < n &&
                tsComposeRow(base, rows, n, g_tsCursor, row, sizeof row))
                abAppendFrag(msg, sizeof msg, row);
            logLine("usersel: Escape -- the choice cannot be declined, re-reading the prompt");
            postSpeech(msg);
            return true;
        }
        int back = g_tsReturnCursor;
        if (g_tsArmed && !clickQueued()) {
            float cx = 0, cy = 0;
            bool got = false;
            RvEntry rows[RV_MAX_ROWS];
            const int n = tsBuildTargets(base, rows, RV_MAX_ROWS);
            if (g_tsCursor >= 0 && g_tsCursor < n)
                got = tsRowClickPoint(base, rows[g_tsCursor], &cx, &cy);
            if (!got) {
                uintptr_t el = g_tsSkillElemId ? feGetElementById(g_tsSkillElemId) : 0;
                got = el && elemCenter(el, &cx, &cy);
            }
            if (got) {
                enqueueSynthAt(SDL_EVT_MOUSEBUTTONDOWN, SDL_BUTTON_RIGHT, 1, 0, cx, cy, 1);
                enqueueSynthAt(SDL_EVT_MOUSEBUTTONUP,   SDL_BUTTON_RIGHT, 0, 0, cx, cy, 2);
                logLine("targeting: cancelling an ARMED skill -- right-click at (%.0f,%.0f)", cx, cy);
            } else {
                logLine("targeting: cancelling an ARMED skill but no point to right-click");
            }
        }
        tsCloseToBar(base);
        logLine("targeting: cancelled, back to action bar item %d", back);
        postSpeech(axs(AXS_CANCELLED));
        return true;
    }

    {
        int jump = 0;
        if (axDecodeJump(sym, mod, repeat, &jump)) {
            if (!jump) return true;              // held jump: one landing per press
            RvEntry rows[RV_MAX_ROWS];
            int n = tsBuildTargets(base, rows, RV_MAX_ROWS);
            if (n <= 0) {
                logLine("targeting: no targets left, closing");
                tsCloseToBar(base);
                postSpeech(axs(AXS_TS_NO_VALID_TARGETS));
                return true;
            }
            g_tsCursor = (jump > 0) ? n - 1 : 0;
            g_rvTipLine = 0;                     // a new row rewinds the shared tooltip buffer
            g_rvTipCol  = 0;
            g_rvCursor  = tsRoomIndexOf(base, rows[g_tsCursor]);   // aim Ctrl+Up/Down here too
            tsSpeakRow(base, rows, n, g_tsCursor);
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
    if (repeat) return true;

    RvEntry rows[RV_MAX_ROWS];
    int n = tsBuildTargets(base, rows, RV_MAX_ROWS);
    if (n <= 0) {
        logLine("targeting: no targets left, closing");
        tsCloseToBar(base);
        postSpeech(axs(AXS_TS_NO_VALID_TARGETS));
        return true;
    }
    if (g_tsCursor >= n) g_tsCursor = n - 1;

    if (confirm) {
        if (g_tsCursor < 0) { postSpeech(axs(AXS_TS_NO_TARGET_SELECTED)); return true; }
        if (g_tsUserSel) tsConfirmUserSelected(base, rows[g_tsCursor]);
        else             tsConfirm(base, rows[g_tsCursor]);
        return true;
    }

    if (g_tsCursor < 0) g_tsCursor = 0;       // unplaced (a MOVE starts so): first press lands
    else axStepCursor(&g_tsCursor, n, dir);
    g_rvTipLine = 0;                      // a new row rewinds the shared tooltip buffer
    g_rvTipCol  = 0;
    g_rvCursor  = tsRoomIndexOf(base, rows[g_tsCursor]);
    tsSpeakRow(base, rows, n, g_tsCursor);
    return true;
}
// ---- MOVE OUTCOME WATCH ----
static bool      g_mvWatch = false;
static uint32_t  g_mvDeadline = 0;
static uint32_t  g_mvWaited = 0;
static int       g_mvN = 0;
static uintptr_t g_mvPtr[RV_MAX_MEMBERS];
static char      g_mvMover[160] = {0};   // who is moving, latched at the commit
static int       g_mvDestSlot = 0;       // the rank they were sent to
bool abMoveWatchArmed() { return g_mvWatch; }

static void mvSnapshot(uintptr_t base, const char* mover, int destSlot, uint32_t waitMs) {
    uintptr_t party[RV_MAX_MEMBERS];
    g_mvN = rvPartyList(base, party, RV_MAX_MEMBERS);
    for (int i = 0; i < g_mvN; i++) g_mvPtr[i] = party[i];
    _snprintf(g_mvMover, sizeof g_mvMover, "%s", mover && mover[0] ? mover : axs(AXS_AB_HERO_FALLBACK));
    g_mvMover[sizeof g_mvMover - 1] = 0;
    g_mvDestSlot = destSlot;
    g_mvWatch = true;
    g_mvWaited = waitMs;
    g_mvDeadline = GetTickCount() + waitMs;
    logLine("move-watch: armed, %d party members, expecting \"%s\" -> position %d (within %ums)",
            g_mvN, g_mvMover, destSlot, waitMs);
}

void serviceMoveWatch(uintptr_t base) {
    if (!g_mvWatch) return;

    uintptr_t party[RV_MAX_MEMBERS];
    int n = rvPartyList(base, party, RV_MAX_MEMBERS);

    bool moved = (n != g_mvN);
    for (int i = 0; !moved && i < n; i++) moved = (party[i] != g_mvPtr[i]);

    if (moved) {
        g_mvWatch = false;
        char msg[320];
        _snprintf(msg, sizeof msg, axs(AXS_AB_MOVING_TO_POS_FMT), g_mvMover, g_mvDestSlot);
        msg[sizeof msg - 1] = 0;
        logLine("move-watch: party order changed -> \"%s\"", msg);
        postSpeech(msg);
        return;
    }

    if (GetTickCount() >= g_mvDeadline) {
        g_mvWatch = false;
        logLine("move-watch: no reposition within %ums — reporting the failure", g_mvWaited);
        postSpeech(axs(AXS_MOVE_DIDNT_HAPPEN));
    }
}
