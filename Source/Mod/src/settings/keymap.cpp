// settings/keymap.cpp -- the mod's REMAPPABLE FUNCTION TABLE and the input-translation layer.

#include <windows.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include "internal.h"

// ---- Table shorthands ----
#define K(sym, mods)      { (uint32_t)(sym), (uint8_t)(mods) }
#define K0                { 0u, 0u }
#define P(in)             { (uint8_t)(in), PI_NONE, 0 }
#define PM(mod, in)       { (uint8_t)(in), (uint8_t)(mod), 0 }
#define PH(in)            { (uint8_t)(in), PI_NONE, 1 }
#define PMH(mod, in)      { (uint8_t)(in), (uint8_t)(mod), 1 }   // chorded AND held (LT+B held = snuff)
#define P0                { PI_NONE, PI_NONE, 0 }
#define NONAME            AXS__COUNT
static const uint32_t SDLK_KEY_1 = 0x31;

#define F1(id, grp, scope, tag, flags, name, key, pad) \
    { id, grp, scope, tag, flags, name, 1, { nullptr }, { NONAME }, { key }, { pad } }
// Two-slot entry (back/forward, previous/next, ...).
#define F2(id, grp, scope, tag, flags, name, t0, n0, k0, p0, t1, n1, k1, p1) \
    { id, grp, scope, tag, flags, name, 2, { t0, t1 }, { n0, n1 }, { k0, k1 }, { p0, p1 } }
// Four-slot entry (the four directions).
#define F4(id, grp, scope, tag, flags, name, kU, kD, kL, kR, pU, pD, pL, pR) \
    { id, grp, scope, tag, flags, name, 4, { "up", "down", "left", "right" }, \
      { AXS_SM_SLOT_UP, AXS_SM_SLOT_DOWN, AXS_SM_SLOT_LEFT, AXS_SM_SLOT_RIGHT }, \
      { kU, kD, kL, kR }, { pU, pD, pL, pR } }
#define F4S(id, grp, scope, tag, flags, name, t0, t1, t2, t3, n0, n1, n2, n3, k0, k1, k2, k3, p0, p1, p2, p3) \
    { id, grp, scope, tag, flags, name, 4, { t0, t1, t2, t3 }, { n0, n1, n2, n3 }, \
      { k0, k1, k2, k3 }, { p0, p1, p2, p3 } }

// ---- The table ----
static const KmFunc kFuncs[] = {
    // ---- GENERAL (both worlds) ----
    F4("general.move_focus",  KMG_GENERAL, KMS_BOTH, KMX_ANY, KMF_REPEAT, AXS_FN_MOVE_FOCUS,
       K(SDLK_UP, 0), K(SDLK_DOWN, 0), K(SDLK_LEFT, 0), K(SDLK_RIGHT, 0),
       P(PI_DPAD_UP), P(PI_DPAD_DOWN), P(PI_DPAD_LEFT), P(PI_DPAD_RIGHT)),
    F1("general.activate",    KMG_GENERAL, KMS_BOTH, KMX_ANY, KMF_FWD, AXS_FN_ACTIVATE,
       K(SDLK_RETURN, 0), P(PI_A)),
    F1("general.back",        KMG_GENERAL, KMS_BOTH, KMX_ANY, KMF_FWD, AXS_FN_BACK,
       K(SDLK_ESCAPE, 0), P(PI_B)),
    F1("general.next_area",   KMG_GENERAL, KMS_BOTH, KMX_ANY, KMF_FWD, AXS_FN_NEXT_AREA,
       K(SDLK_TAB, 0), P(PI_RT)),
    F1("general.prev_area",   KMG_GENERAL, KMS_BOTH, KMX_ANY, KMF_FWD, AXS_FN_PREV_AREA,
       K(SDLK_TAB, KM_SHIFT), P(PI_LT)),
    F2("general.tooltips",    KMG_GENERAL, KMS_BOTH, KMX_ANY, KMF_REPEAT, AXS_FN_TOOLTIPS,
       "back", AXS_SM_SLOT_BACK, K(SDLK_UP, KM_CTRL),   PM(PI_LT, PI_DPAD_UP),
       "fwd",  AXS_SM_SLOT_FWD,  K(SDLK_DOWN, KM_CTRL), PM(PI_LT, PI_DPAD_DOWN)),
    F2("general.tooltip_panel", KMG_GENERAL, KMS_BOTH, KMX_ANY, KMF_REPEAT, AXS_FN_TOOLTIP_PANEL,
       "prev", AXS_SM_SLOT_PREV, K(SDLK_LEFT, KM_CTRL),  PM(PI_LT, PI_DPAD_LEFT),
       "next", AXS_SM_SLOT_NEXT, K(SDLK_RIGHT, KM_CTRL), PM(PI_LT, PI_DPAD_RIGHT)),
    F1("general.say_again",   KMG_GENERAL, KMS_BOTH, KMX_ANY, KMF_GLOBAL, AXS_FN_SAY_AGAIN,
       K(SDLK_COMMA, 0), PM(PI_LT, PI_X)),
    F1("general.char_sheet",  KMG_GENERAL, KMS_BOTH, KMX_ANY, KMF_FWD, AXS_FN_CHAR_SHEET,
       K(SDLK_c, 0), P(PI_X)),
    F1("general.settings",    KMG_GENERAL, KMS_BOTH, KMX_ANY, KMF_GLOBAL, AXS_FN_SETTINGS,
       K(SDLK_F10, 0), P(PI_BACK)),
    // ---- GENERAL -> quick assign skills on the character sheet (submenu) ----
    F1("general.sheet_slot_1", KMG_GENERAL_SHEET, KMS_BOTH, KMX_SHEET, 0, AXS_FN_SHEET_SLOT1, K(SDLK_KEY_1, 0),     P0),
    F1("general.sheet_slot_2", KMG_GENERAL_SHEET, KMS_BOTH, KMX_SHEET, 0, AXS_FN_SHEET_SLOT2, K(SDLK_KEY_1 + 1, 0), P0),
    F1("general.sheet_slot_3", KMG_GENERAL_SHEET, KMS_BOTH, KMX_SHEET, 0, AXS_FN_SHEET_SLOT3, K(SDLK_KEY_1 + 2, 0), P0),
    F1("general.sheet_slot_4", KMG_GENERAL_SHEET, KMS_BOTH, KMX_SHEET, 0, AXS_FN_SHEET_SLOT4, K(SDLK_KEY_1 + 3, 0), P0),

    // ---- HAMLET (town) ----
    F1("hamlet.activity_log", KMG_HAMLET, KMS_TOWN, KMX_ANY, 0, AXS_FN_ACTIVITY_LOG, K(SDLK_PERIOD, 0),        P(PI_RB)),
    F1("hamlet.town_event",   KMG_HAMLET, KMS_TOWN, KMX_ANY, 0, AXS_FN_TOWN_EVENT,   K(SDLK_PERIOD, KM_SHIFT), P(PI_LB)),
    F1("hamlet.forward",      KMG_HAMLET, KMS_TOWN, KMX_ANY, 0, AXS_FN_FORWARD,      K(SDLK_e, 0),             P(PI_RS_CLICK)),
    F1("hamlet.resources",    KMG_HAMLET, KMS_TOWN, KMX_ANY, 0, AXS_FN_RESOURCES,    K(SDLK_r, 0),             P0),
    F1("hamlet.upgrades",     KMG_HAMLET, KMS_TOWN, KMX_BLDG, 0, AXS_FN_UPGRADES,    K(SDLK_u, 0),             P(PI_LT)),
    F1("hamlet.sell_trinket", KMG_HAMLET, KMS_TOWN, KMX_REALMINV, 0, AXS_FN_SELL_TRINKET, K(SDLK_RETURN, KM_SHIFT), P(PI_X)),
    F1("hamlet.stack",        KMG_HAMLET, KMS_TOWN, KMX_PROVISION, 0, AXS_FN_STACK,  K(SDLK_RETURN, KM_SHIFT), P(PI_X)),
    F1("hamlet.reorder",      KMG_HAMLET, KMS_TOWN, KMX_ANY, 0, AXS_FN_REORDER,      K(SDLK_SPACE, 0),         P(PI_LS_CLICK)),
    // The mod's other town claims the dev's list did not name, kept remappable:
    F1("hamlet.slot_clear",   KMG_HAMLET, KMS_TOWN, KMX_ANY, 0, AXS_FN_SLOT_CLEAR,   K(SDLK_DELETE, 0),        P0),
    F1("hamlet.trinket_inv",  KMG_HAMLET, KMS_TOWN, KMX_ANY, 0, AXS_FN_TRINKET_INV,  K(SDLK_i, 0),             P(PI_Y)),
    F2("hamlet.strip",        KMG_HAMLET, KMS_TOWN, KMX_BLDG, 0, AXS_FN_STRIP,
       "prev", AXS_SM_SLOT_PREV, K(SDLK_PAGEUP, 0),   PM(PI_LT, PI_LB),
       "next", AXS_SM_SLOT_NEXT, K(SDLK_PAGEDOWN, 0), PM(PI_LT, PI_RB)),
    // ---- HAMLET -> building hotkeys (submenu) ----
    F1("hamlet.jump_abbey",      KMG_HAMLET_BLDG, KMS_TOWN, KMX_ANY, 0, AXS_FN_JUMP_ABBEY,      K(SDLK_a, 0), P0),
    F1("hamlet.jump_smith",      KMG_HAMLET_BLDG, KMS_TOWN, KMX_ANY, 0, AXS_FN_JUMP_SMITH,      K(SDLK_b, 0), P0),
    F1("hamlet.jump_coach",      KMG_HAMLET_BLDG, KMS_TOWN, KMX_ANY, 0, AXS_FN_JUMP_COACH,      K(SDLK_c, 0), P0),
    F1("hamlet.jump_districts",  KMG_HAMLET_BLDG, KMS_TOWN, KMX_ANY, 0, AXS_FN_JUMP_DISTRICTS,  K(SDLK_d, 0), P0),
    F1("hamlet.jump_guild",      KMG_HAMLET_BLDG, KMS_TOWN, KMX_ANY, 0, AXS_FN_JUMP_GUILD,      K(SDLK_l, 0), P0),
    F1("hamlet.jump_memoirs",    KMG_HAMLET_BLDG, KMS_TOWN, KMX_ANY, 0, AXS_FN_JUMP_MEMOIRS,    K(SDLK_m, 0), P0),
    F1("hamlet.jump_sanitarium", KMG_HAMLET_BLDG, KMS_TOWN, KMX_ANY, 0, AXS_FN_JUMP_SANITARIUM, K(SDLK_s, 0), P0),
    F1("hamlet.jump_tavern",     KMG_HAMLET_BLDG, KMS_TOWN, KMX_ANY, 0, AXS_FN_JUMP_TAVERN,     K(SDLK_t, 0), P0),
    F1("hamlet.jump_survivalist",KMG_HAMLET_BLDG, KMS_TOWN, KMX_ANY, 0, AXS_FN_JUMP_SURVIVALIST,K(SDLK_v, 0), P0),
    F1("hamlet.jump_wagon",      KMG_HAMLET_BLDG, KMS_TOWN, KMX_ANY, 0, AXS_FN_JUMP_WAGON,      K(SDLK_w, 0), P0),
    F1("hamlet.jump_graveyard",  KMG_HAMLET_BLDG, KMS_TOWN, KMX_ANY, 0, AXS_FN_JUMP_GRAVEYARD,  K(SDLK_y, 0), P0),
    F1("hamlet.jump_circus",     KMG_HAMLET_BLDG, KMS_TOWN, KMX_ANY, 0, AXS_FN_JUMP_CIRCUS,     K(SDLK_z, 0), P0),

    // ---- DUNGEON (a raid) ----
    F2("dungeon.panels",      KMG_DUNGEON, KMS_DUNGEON, KMX_ANY, 0, AXS_FN_PANELS,
       "map", AXS_SM_SLOT_MAP, K(SDLK_m, 0), P(PI_LT),
       "bag", AXS_SM_SLOT_BAG, K(SDLK_i, 0), P(PI_RT)),
    F2("dungeon.hero_select", KMG_DUNGEON, KMS_DUNGEON, KMX_ANY, KMF_FWD, AXS_FN_HERO_SELECT,
       "prev", AXS_SM_SLOT_PREV, K(SDLK_q, 0), P(PI_LB),
       "next", AXS_SM_SLOT_NEXT, K(SDLK_e, 0), P(PI_RB)),
    F1("dungeon.move_hero",   KMG_DUNGEON, KMS_DUNGEON, KMX_PARTYROW, 0, AXS_FN_MOVE_HERO,   K(SDLK_RETURN, 0),        P(PI_A)),
    F1("dungeon.party_order", KMG_DUNGEON, KMS_DUNGEON, KMX_PARTYROW, 0, AXS_FN_PARTY_ORDER, K(SDLK_RETURN, KM_SHIFT), PH(PI_A)),
    F2("dungeon.walk",        KMG_DUNGEON, KMS_DUNGEON, KMX_ANY, KMF_FWD | KMF_HELD, AXS_FN_WALK,
       "back", AXS_SM_SLOT_BACK, K(SDLK_a, 0), P(PI_RS_LEFT),
       "fwd",  AXS_SM_SLOT_FWD,  K(SDLK_d, 0), P(PI_RS_RIGHT)),
    F2("dungeon.step",        KMG_DUNGEON, KMS_DUNGEON, KMX_ANY, 0, AXS_FN_STEP,
       "back", AXS_SM_SLOT_BACK, K(SDLK_a, KM_SHIFT), PM(PI_LT, PI_RS_LEFT),
       "fwd",  AXS_SM_SLOT_FWD,  K(SDLK_d, KM_SHIFT), PM(PI_LT, PI_RS_RIGHT)),
    F1("dungeon.interact",    KMG_DUNGEON, KMS_DUNGEON, KMX_ANY, KMF_FWD, AXS_FN_INTERACT, K(SDLK_w, 0), PM(PI_LT, PI_A)),
    F1("dungeon.torch_light", KMG_DUNGEON, KMS_DUNGEON, KMX_ANY, KMF_FWD, AXS_FN_TORCH_LIGHT, K(SDLK_t, 0),                   PM(PI_LT, PI_Y)),
    F1("dungeon.torch_dim",   KMG_DUNGEON, KMS_DUNGEON, KMX_ANY, 0,       AXS_FN_TORCH_DIM,   K(SDLK_t, KM_SHIFT),            PM(PI_LT, PI_B)),
    F1("dungeon.torch_snuff", KMG_DUNGEON, KMS_DUNGEON, KMX_ANY, 0,       AXS_FN_TORCH_SNUFF, K(SDLK_t, KM_SHIFT | KM_CTRL),  PMH(PI_LT, PI_B)),
    F1("dungeon.bag_discard", KMG_DUNGEON, KMS_DUNGEON, KMX_BAG, 0, AXS_FN_BAG_DISCARD,  K(SDLK_DELETE, 0),       PH(PI_Y)),
    F1("dungeon.bag_move",    KMG_DUNGEON, KMS_DUNGEON, KMX_BAG, 0, AXS_FN_BAG_MOVE,     K(SDLK_SPACE, 0),        P(PI_LS_CLICK)),
    F4("dungeon.tiles",       KMG_DUNGEON, KMS_DUNGEON, KMX_MAP, KMF_REPEAT, AXS_FN_TILES,
       K(SDLK_UP, KM_CTRL), K(SDLK_DOWN, KM_CTRL), K(SDLK_LEFT, KM_CTRL), K(SDLK_RIGHT, KM_CTRL),
       PM(PI_LT, PI_DPAD_UP), PM(PI_LT, PI_DPAD_DOWN), PM(PI_LT, PI_DPAD_LEFT), PM(PI_LT, PI_DPAD_RIGHT)),
    F1("dungeon.quest_button",KMG_DUNGEON, KMS_DUNGEON, KMX_ANY, 0, AXS_FN_QUEST_BUTTON, K(SDLK_ESCAPE, KM_SHIFT), P(PI_RS_CLICK)),
    // The mod's other raid claims the dev's list did not name, kept remappable:
    F1("dungeon.loot_take_all",KMG_DUNGEON, KMS_DUNGEON, KMX_LOOT, KMF_FWD, AXS_FN_LOOT_TAKE_ALL, K(SDLK_SPACE, 0), P(PI_Y)),
    F1("dungeon.map_party",   KMG_DUNGEON, KMS_DUNGEON, KMX_MAP, 0, AXS_FN_MAP_PARTY,    K(SDLK_HOME, 0),      P(PI_LS_CLICK)),
    F1("dungeon.dungeon_view",KMG_DUNGEON, KMS_DUNGEON, KMX_ANY, 0, AXS_FN_DUNGEON_VIEW, K(SDLK_r, 0),         P0),
    F1("dungeon.action_bar",  KMG_DUNGEON, KMS_DUNGEON, KMX_ANY, 0, AXS_FN_ACTION_BAR,   K(SDLK_BACKQUOTE, 0), P0),
    F1("dungeon.quest_goals", KMG_DUNGEON, KMS_DUNGEON, KMX_ANY, 0, AXS_FN_QUEST_GOALS,  K(SDLK_g, 0),         P0),
    F1("dungeon.combat_log",  KMG_DUNGEON, KMS_DUNGEON, KMX_ANY, 0, AXS_FN_COMBAT_LOG,   K(SDLK_PERIOD, 0),    PM(PI_LT, PI_RB)),
    F1("dungeon.light_meter", KMG_DUNGEON, KMS_DUNGEON, KMX_ANY, 0, AXS_FN_LIGHT_METER,  K(SDLK_l, 0),         PM(PI_LT, PI_LB)),
    F4S("dungeon.read_party_rank", KMG_DUNGEON, KMS_DUNGEON, KMX_ANY, 0, AXS_FN_READ_PARTY_RANK,
        "rank1", "rank2", "rank3", "rank4",
        AXS_SM_SLOT_RANK_1, AXS_SM_SLOT_RANK_2, AXS_SM_SLOT_RANK_3, AXS_SM_SLOT_RANK_4,
        K(SDLK_KEY_1, KM_ALT), K(SDLK_KEY_1 + 1, KM_ALT), K(SDLK_KEY_1 + 2, KM_ALT), K(SDLK_KEY_1 + 3, KM_ALT),
        P0, P0, P0, P0),
    F4S("dungeon.read_enemy_rank", KMG_DUNGEON, KMS_DUNGEON, KMX_ANY, 0, AXS_FN_READ_ENEMY_RANK,
        "rank1", "rank2", "rank3", "rank4",
        AXS_SM_SLOT_RANK_1, AXS_SM_SLOT_RANK_2, AXS_SM_SLOT_RANK_3, AXS_SM_SLOT_RANK_4,
        K(SDLK_KEY_1 + 4, KM_ALT), K(SDLK_KEY_1 + 5, KM_ALT), K(SDLK_KEY_1 + 6, KM_ALT), K(SDLK_KEY_1 + 7, KM_ALT),
        P0, P0, P0, P0),
    // ---- DUNGEON -> skill hotkeys (submenu). The game's own keys since 2026-08-29 (the mod ----
    F1("dungeon.skill_1",    KMG_DUNGEON_SKILLS, KMS_DUNGEON, KMX_ANY, KMF_FWD, AXS_FN_SKILL1,    K(SDLK_KEY_1, 0),     P0),
    F1("dungeon.skill_2",    KMG_DUNGEON_SKILLS, KMS_DUNGEON, KMX_ANY, KMF_FWD, AXS_FN_SKILL2,    K(SDLK_KEY_1 + 1, 0), P0),
    F1("dungeon.skill_3",    KMG_DUNGEON_SKILLS, KMS_DUNGEON, KMX_ANY, KMF_FWD, AXS_FN_SKILL3,    K(SDLK_KEY_1 + 2, 0), P0),
    F1("dungeon.skill_4",    KMG_DUNGEON_SKILLS, KMS_DUNGEON, KMX_ANY, KMF_FWD, AXS_FN_SKILL4,    K(SDLK_KEY_1 + 3, 0), P0),
    F1("dungeon.skill_move", KMG_DUNGEON_SKILLS, KMS_DUNGEON, KMX_ANY, KMF_FWD, AXS_FN_SKILL_MOVE,K(SDLK_KEY_1 + 4, 0), P0),

    // ---- DLC (town) ----
    F2("dlc.jeweler",         KMG_DLC, KMS_TOWN, KMX_WAGON, 0, AXS_FN_JEWELER,
       "to",   AXS_SM_SLOT_TO_JEWELER,   K(SDLK_RIGHT, 0), P(PI_DPAD_RIGHT),
       "back", AXS_SM_SLOT_FROM_JEWELER, K(SDLK_LEFT, 0),  P(PI_DPAD_LEFT)),
    F2("dlc.shard_mercs",     KMG_DLC, KMS_TOWN, KMX_COACH, 0, AXS_FN_SHARD_MERCS,
       "to",   AXS_SM_SLOT_TO_SHARDS,   K(SDLK_RIGHT, 0), P(PI_DPAD_RIGHT),
       "back", AXS_SM_SLOT_FROM_SHARDS, K(SDLK_LEFT, 0),  P(PI_DPAD_LEFT)),
    F1("dlc.ring_fight",      KMG_DLC, KMS_TOWN, KMX_RING, 0, AXS_FN_RING_FIGHT,    K(SDLK_e, 0), P(PI_RS_CLICK)),
    F1("dlc.ring_practice",   KMG_DLC, KMS_TOWN, KMX_RING, 0, AXS_FN_RING_PRACTICE, K(SDLK_p, 0), P(PI_LB)),
    F1("dlc.ring_leagues",    KMG_DLC, KMS_TOWN, KMX_RING, 0, AXS_FN_RING_LEAGUES,  K(SDLK_l, 0), P(PI_RB)),
};
static const int KM_COUNT = (int)(sizeof kFuncs / sizeof kFuncs[0]);

// ---- Old save-file ids -> the function/slot that holds them now ----
struct KmAlias { const char* oldId; const char* id; const char* slot; };
static const KmAlias kAliases[] = {
    { "dungeon.step_forward", "dungeon.step", "fwd" }, { "dungeon.step_back", "dungeon.step", "back" },
    { "dungeon.skill_1", "dungeon.skill_1", nullptr }, { "dungeon.skill_2", "dungeon.skill_2", nullptr },
    { "dungeon.skill_3", "dungeon.skill_3", nullptr }, { "dungeon.skill_4", "dungeon.skill_4", nullptr },
    { "dungeon.skill_5", "dungeon.skill_move", nullptr },
    { "dungeon.torch_lower", "dungeon.torch_dim", nullptr }, { "dungeon.torch_snuff", "dungeon.torch_snuff", nullptr },
    { "dungeon.panel_map", "dungeon.panels", "map" }, { "dungeon.panel_bag", "dungeon.panels", "bag" },
    { "dungeon.dungeon_view", "dungeon.dungeon_view", nullptr }, { "dungeon.quest_goals", "dungeon.quest_goals", nullptr },
    { "dungeon.action_bar", "dungeon.action_bar", nullptr }, { "dungeon.combat_log", "dungeon.combat_log", nullptr },
    { "dungeon.light_meter", "dungeon.light_meter", nullptr },
    { "dungeon.confirm", "general.activate", nullptr }, { "dungeon.party_reset", "dungeon.party_order", nullptr },
    { "dungeon.character_sheet", "general.char_sheet", nullptr },
    { "dungeon.bag_take", "dungeon.bag_move", nullptr }, { "dungeon.bag_all", "dungeon.bag_move", nullptr },
    { "dungeon.bag_drop_all", "dungeon.bag_move", nullptr },
    { "dungeon.bag_discard", "dungeon.bag_discard", nullptr },
    { "dungeon.nav_up", "general.move_focus", "up" }, { "dungeon.nav_down", "general.move_focus", "down" },
    { "dungeon.nav_left", "general.move_focus", "left" }, { "dungeon.nav_right", "general.move_focus", "right" },
    { "dungeon.tip_back", "general.tooltips", "back" }, { "dungeon.tip_fwd", "general.tooltips", "fwd" },
    { "dungeon.tile_left", "dungeon.tiles", "left" }, { "dungeon.tile_right", "dungeon.tiles", "right" },
    { "dungeon.map_party", "dungeon.map_party", nullptr },
    { "town.resources", "hamlet.resources", nullptr }, { "town.activity_log", "hamlet.activity_log", nullptr },
    { "town.event_popup", "hamlet.town_event", nullptr }, { "town.forward", "hamlet.forward", nullptr },
    { "town.trinket_inv", "hamlet.trinket_inv", nullptr },
    { "town.hero_list", "general.next_area", nullptr }, { "town.hero_list_back", "general.prev_area", nullptr },
    { "town.jump_abbey", "hamlet.jump_abbey", nullptr }, { "town.jump_smith", "hamlet.jump_smith", nullptr },
    { "town.jump_coach", "hamlet.jump_coach", nullptr }, { "town.jump_districts", "hamlet.jump_districts", nullptr },
    { "town.jump_guild", "hamlet.jump_guild", nullptr }, { "town.jump_memoirs", "hamlet.jump_memoirs", nullptr },
    { "town.jump_sanitarium", "hamlet.jump_sanitarium", nullptr }, { "town.jump_tavern", "hamlet.jump_tavern", nullptr },
    { "town.jump_survivalist", "hamlet.jump_survivalist", nullptr }, { "town.jump_wagon", "hamlet.jump_wagon", nullptr },
    { "town.jump_graveyard", "hamlet.jump_graveyard", nullptr }, { "town.jump_circus", "hamlet.jump_circus", nullptr },
    { "town.strip_prev", "hamlet.strip", "prev" }, { "town.strip_next", "hamlet.strip", "next" },
    { "town.upgrades", "hamlet.upgrades", nullptr },
    { "town.sheet_slot_1", "general.sheet_slot_1", nullptr }, { "town.sheet_slot_2", "general.sheet_slot_2", nullptr },
    { "town.sheet_slot_3", "general.sheet_slot_3", nullptr }, { "town.sheet_slot_4", "general.sheet_slot_4", nullptr },
    { "town.confirm", "general.activate", nullptr }, { "town.confirm_stack", "hamlet.stack", nullptr },
    { "town.reorder", "hamlet.reorder", nullptr }, { "town.slot_clear", "hamlet.slot_clear", nullptr },
    { "town.nav_up", "general.move_focus", "up" }, { "town.nav_down", "general.move_focus", "down" },
    { "town.nav_left", "general.move_focus", "left" }, { "town.nav_right", "general.move_focus", "right" },
    { "town.tip_back", "general.tooltips", "back" }, { "town.tip_fwd", "general.tooltips", "fwd" },
    { "town.rank_now", "general.tooltip_panel", "prev" }, { "town.rank_next", "general.tooltip_panel", "next" },
    { "hamlet.rank_detail.now", "general.tooltip_panel", "prev" }, { "hamlet.rank_detail.next", "general.tooltip_panel", "next" },
    { "dungeon.open_door", "general.activate", nullptr },
};
static const int KM_ALIAS_COUNT = (int)(sizeof kAliases / sizeof kAliases[0]);

static KmChord g_curKey[sizeof kFuncs / sizeof kFuncs[0]][KM_SLOTS_MAX];
static bool    g_blankKey[sizeof kFuncs / sizeof kFuncs[0]][KM_SLOTS_MAX];
static PadBind g_curPad[sizeof kFuncs / sizeof kFuncs[0]][KM_SLOTS_MAX];
static bool    g_blankPad[sizeof kFuncs / sizeof kFuncs[0]][KM_SLOTS_MAX];
static KmChord g_snapKey[sizeof kFuncs / sizeof kFuncs[0]][KM_SLOTS_MAX];
static bool    g_snapBlankKey[sizeof kFuncs / sizeof kFuncs[0]][KM_SLOTS_MAX];
static PadBind g_snapPad[sizeof kFuncs / sizeof kFuncs[0]][KM_SLOTS_MAX];
static bool    g_snapBlankPad[sizeof kFuncs / sizeof kFuncs[0]][KM_SLOTS_MAX];
static bool    g_kmLoaded = false;
static bool    g_kmAnyRemapped = false;

int kmCount() { return KM_COUNT; }
const KmFunc* kmAt(int i) { return (i >= 0 && i < KM_COUNT) ? &kFuncs[i] : nullptr; }
int kmFind(const char* id) {
    for (int i = 0; i < KM_COUNT; i++) if (strcmp(kFuncs[i].id, id) == 0) return i;
    return -1;
}
static bool kmOk(int f, int s) { return f >= 0 && f < KM_COUNT && s >= 0 && s < kFuncs[f].slots; }
bool kmKeyIsBlank(int f, int s) { return kmOk(f, s) ? g_blankKey[f][s] : false; }
bool kmPadIsBlank(int f, int s) { return kmOk(f, s) ? g_blankPad[f][s] : true; }
KmChord kmKeyCurrent(int f, int s) { KmChord z = { 0, 0 }; return kmOk(f, s) ? g_curKey[f][s] : z; }
PadBind kmPadCurrent(int f, int s) { PadBind z = { PI_NONE, PI_NONE, 0 }; return kmOk(f, s) ? g_curPad[f][s] : z; }

static bool chordEq(KmChord a, KmChord b) { return a.sym == b.sym && a.mods == b.mods; }
bool kmPadEq(PadBind a, PadBind b) { return a.in == b.in && a.mod == b.mod && a.hold == b.hold; }
static bool padIsNone(PadBind b) { return b.in == PI_NONE; }

static void kmRecountRemapped() {
    g_kmAnyRemapped = false;
    for (int f = 0; f < KM_COUNT; f++)
        for (int s = 0; s < kFuncs[f].slots; s++)
            if (g_blankKey[f][s] || !chordEq(g_curKey[f][s], kFuncs[f].defKey[s])) { g_kmAnyRemapped = true; return; }
}

static bool kmOverlap(int f, int g) {
    const KmFunc& a = kFuncs[f]; const KmFunc& b = kFuncs[g];
    bool scopes = (a.flags & KMF_GLOBAL) || (b.flags & KMF_GLOBAL) || (a.scope & b.scope) != 0;
    return scopes && a.tag == b.tag;
}

// ---- Chord <-> text ----
struct KmNamedKey { uint32_t sym; const char* tok; AxStrId spoken; };
static const KmNamedKey kNamedKeys[] = {
    { SDLK_RETURN,    "enter",     AXS_KEY_ENTER },
    { SDLK_ESCAPE,    "escape",    AXS_KEY_ESCAPE },
    { SDLK_TAB,       "tab",       AXS_KN_TAB },
    { SDLK_SPACE,     "space",     AXS_KN_SPACE },
    { SDLK_BACKSPACE, "backspace", AXS_KN_BACKSPACE },
    { SDLK_DELETE,    "delete",    AXS_KN_DELETE },
    { SDLK_BACKSLASH, "backslash", AXS_KN_BACKSLASH },
    { SDLK_PERIOD,    "period",    AXS_KN_PERIOD },
    { SDLK_BACKQUOTE, "backquote", AXS_KN_BACKQUOTE },
    { SDLK_SLASH,     "slash",     AXS_KN_SLASH },
    { SDLK_COMMA,     "comma",     AXS_KN_COMMA },
    { SDLK_MINUS,     "minus",     AXS_KN_MINUS },
    { SDLK_EQUALS,    "equals",    AXS_KN_PLUS },      // the '='/'+' key; spoken by its shifted face
    { SDLK_UP,        "up",        AXS_KN_UP },
    { SDLK_DOWN,      "down",      AXS_KN_DOWN },
    { SDLK_LEFT,      "left",      AXS_KN_LEFT },
    { SDLK_RIGHT,     "right",     AXS_KN_RIGHT },
    { SDLK_PAGEUP,    "pageup",    AXS_KN_PAGEUP },
    { SDLK_PAGEDOWN,  "pagedown",  AXS_KN_PAGEDOWN },
    { 0x4000004Au,    "home",      AXS_KN_HOME },
    { 0x4000004Du,    "end",       AXS_KN_END },
    { 0x40000049u,    "insert",    AXS_KN_INSERT },
};
static const int KM_NAMED_COUNT = (int)(sizeof kNamedKeys / sizeof kNamedKeys[0]);

// F1..F12 and the numpad are ranges, handled arithmetically rather than listed.
static const uint32_t SDLK_F1_       = 0x4000003Au;   // scancode 58
static const uint32_t SDLK_F12_      = 0x40000045u;
static const uint32_t SDLK_KP_DIV_   = 0x40000054u;   // divide..period = scancodes 84..99
static const uint32_t SDLK_KP_MUL_   = 0x40000055u;
static const uint32_t SDLK_KP_MINUS_ = 0x40000056u;
static const uint32_t SDLK_KP_PLUS_  = 0x40000057u;
static const uint32_t SDLK_KP_ENTER_ = 0x40000058u;
static const uint32_t SDLK_KP_1_     = 0x40000059u;   // KP_1..KP_9 then KP_0, then period
static const uint32_t SDLK_KP_0_     = 0x40000062u;
static const uint32_t SDLK_KP_DOT_   = 0x40000063u;

static void kmKeyToken(uint32_t sym, char* out, int outsz) {
    for (int i = 0; i < KM_NAMED_COUNT; i++)
        if (kNamedKeys[i].sym == sym) { _snprintf(out, outsz, "%s", kNamedKeys[i].tok); out[outsz-1] = 0; return; }
    if ((sym >= 'a' && sym <= 'z') || (sym >= '0' && sym <= '9')) {
        _snprintf(out, outsz, "%c", (char)sym); out[outsz-1] = 0; return;
    }
    if (sym >= SDLK_F1_ && sym <= SDLK_F12_) {
        _snprintf(out, outsz, "f%d", (int)(sym - SDLK_F1_) + 1); out[outsz-1] = 0; return;
    }
    if (sym >= SDLK_KP_DIV_ && sym <= SDLK_KP_DOT_) {
        _snprintf(out, outsz, "kp%d", (int)(sym - SDLK_KP_DIV_)); out[outsz-1] = 0; return;
    }
    uint32_t scan = klScancodeForKey(sym);
    if (scan) { _snprintf(out, outsz, "pos:%u", scan); out[outsz-1] = 0; return; }
    _snprintf(out, outsz, "sym%u", sym); out[outsz-1] = 0;
}

static bool kmKeyFromToken(const char* tok, uint32_t* sym) {
    for (int i = 0; i < KM_NAMED_COUNT; i++)
        if (strcmp(tok, kNamedKeys[i].tok) == 0) { *sym = kNamedKeys[i].sym; return true; }
    size_t len = strlen(tok);
    if (len == 1 && ((tok[0] >= 'a' && tok[0] <= 'z') || (tok[0] >= '0' && tok[0] <= '9'))) {
        *sym = (uint32_t)tok[0]; return true;
    }
    int n = 0;
    if (sscanf(tok, "f%d", &n) == 1 && n >= 1 && n <= 12 && tok[0] == 'f') { *sym = SDLK_F1_ + (n - 1); return true; }
    if (sscanf(tok, "kp%d", &n) == 1 && n >= 0 && n <= 15) { *sym = SDLK_KP_DIV_ + n; return true; }
    unsigned u = 0;
    if (sscanf(tok, "pos:%u", &u) == 1) {
        uint32_t k = klKeyForScancode(u);
        if (!k) return false;
        *sym = k;
        return true;
    }
    if (sscanf(tok, "sym%u", &u) == 1) { *sym = u; return true; }
    return false;
}

static void kmKeySpoken(uint32_t sym, char* out, int outsz) {
    for (int i = 0; i < KM_NAMED_COUNT; i++)
        if (kNamedKeys[i].sym == sym) { _snprintf(out, outsz, "%s", axs(kNamedKeys[i].spoken)); out[outsz-1] = 0; return; }
    if (sym >= 'a' && sym <= 'z') { _snprintf(out, outsz, "%c", (char)(sym - 'a' + 'A')); out[outsz-1] = 0; return; }
    if (sym >= '0' && sym <= '9') { _snprintf(out, outsz, "%c", (char)sym); out[outsz-1] = 0; return; }
    if (sym >= SDLK_F1_ && sym <= SDLK_F12_) {
        _snprintf(out, outsz, axs(AXS_KN_F_FMT), (int)(sym - SDLK_F1_) + 1); out[outsz-1] = 0; return;
    }
    if (sym >= SDLK_KP_DIV_ && sym <= SDLK_KP_DOT_) {
        char inner[32] = "";
        if (sym >= SDLK_KP_1_ && sym <= SDLK_KP_0_) {
            int d = (sym == SDLK_KP_0_) ? 0 : (int)(sym - SDLK_KP_1_) + 1;
            _snprintf(inner, sizeof inner, "%d", d);
        } else if (sym == SDLK_KP_DIV_)   _snprintf(inner, sizeof inner, "%s", axs(AXS_KN_SLASH));
        else if (sym == SDLK_KP_MUL_)     _snprintf(inner, sizeof inner, "%s", axs(AXS_KN_STAR));
        else if (sym == SDLK_KP_MINUS_)   _snprintf(inner, sizeof inner, "%s", axs(AXS_KN_MINUS));
        else if (sym == SDLK_KP_PLUS_)    _snprintf(inner, sizeof inner, "%s", axs(AXS_KN_PLUS));
        else if (sym == SDLK_KP_ENTER_)   _snprintf(inner, sizeof inner, "%s", axs(AXS_KEY_ENTER));
        else if (sym == SDLK_KP_DOT_)     _snprintf(inner, sizeof inner, "%s", axs(AXS_KN_PERIOD));
        inner[sizeof inner - 1] = 0;
        if (inner[0]) { _snprintf(out, outsz, axs(AXS_KN_NUMPAD_FMT), inner); out[outsz-1] = 0; return; }
    }
    if (sym >= 0x21 && sym <= 0x7e) { _snprintf(out, outsz, "%c", (char)sym); out[outsz-1] = 0; return; }
    if (klKeyName(sym, out, outsz) && out[0]) return;
    _snprintf(out, outsz, axs(AXS_KN_KEY_FMT), (int)sym); out[outsz-1] = 0;
}

void kmChordName(KmChord c, char* out, int outsz) {
    char key[64];
    kmKeySpoken(c.sym, key, sizeof key);
    _snprintf(out, outsz, "%s%s%s%s%s%s%s",
              (c.mods & KM_CTRL)  ? axs(AXS_KN_CTRL)  : "", (c.mods & KM_CTRL)  ? " " : "",
              (c.mods & KM_SHIFT) ? axs(AXS_KN_SHIFT) : "", (c.mods & KM_SHIFT) ? " " : "",
              (c.mods & KM_ALT)   ? axs(AXS_KN_ALT)   : "", (c.mods & KM_ALT)   ? " " : "",
              key);
    out[outsz - 1] = 0;
}

// ---- Pad input <-> text ----
struct KmPadName { uint8_t in; const char* tok; AxStrId spoken; };
static const KmPadName kPadNames[PI_COUNT] = {
    { PI_A, "a", AXS_PI_A }, { PI_B, "b", AXS_PI_B }, { PI_X, "x", AXS_PI_X }, { PI_Y, "y", AXS_PI_Y },
    { PI_START, "start", AXS_PI_START }, { PI_BACK, "back", AXS_PI_BACK },
    { PI_LB, "lb", AXS_PI_LB }, { PI_RB, "rb", AXS_PI_RB },
    { PI_LS_CLICK, "ls", AXS_PI_LS_CLICK }, { PI_RS_CLICK, "rs", AXS_PI_RS_CLICK },
    { PI_DPAD_LEFT, "dpad_left", AXS_PI_DPAD_LEFT }, { PI_DPAD_RIGHT, "dpad_right", AXS_PI_DPAD_RIGHT },
    { PI_DPAD_UP, "dpad_up", AXS_PI_DPAD_UP }, { PI_DPAD_DOWN, "dpad_down", AXS_PI_DPAD_DOWN },
    { PI_LT, "lt", AXS_PI_LT }, { PI_RT, "rt", AXS_PI_RT },
    { PI_RS_LEFT, "rs_left", AXS_PI_RS_LEFT }, { PI_RS_RIGHT, "rs_right", AXS_PI_RS_RIGHT },
    { PI_RS_UP, "rs_up", AXS_PI_RS_UP }, { PI_RS_DOWN, "rs_down", AXS_PI_RS_DOWN },
};
static const char* kmPadInTok(uint8_t in) { return in < PI_COUNT ? kPadNames[in].tok : "?"; }
static bool kmPadInFromTok(const char* tok, uint8_t* in) {
    for (int i = 0; i < PI_COUNT; i++) if (strcmp(tok, kPadNames[i].tok) == 0) { *in = kPadNames[i].in; return true; }
    return false;
}
static void kmPadToken(PadBind b, char* out, int outsz) {
    if (padIsNone(b)) { _snprintf(out, outsz, "unbound"); out[outsz-1] = 0; return; }
    _snprintf(out, outsz, "%s%s%s%s", b.mod != PI_NONE ? kmPadInTok(b.mod) : "", b.mod != PI_NONE ? "+" : "",
              kmPadInTok(b.in), b.hold ? ":hold" : "");
    out[outsz - 1] = 0;
}
static bool kmPadFromToken(const char* s, PadBind* b) {
    char buf[64];
    strncpy(buf, s, sizeof buf - 1); buf[sizeof buf - 1] = 0;
    PadBind r = { PI_NONE, PI_NONE, 0 };
    char* colon = strchr(buf, ':');
    if (colon) { *colon = 0; if (strcmp(colon + 1, "hold") == 0) r.hold = 1; else return false; }
    char* plus = strchr(buf, '+');
    const char* main = buf;
    if (plus) { *plus = 0; if (!kmPadInFromTok(buf, &r.mod)) return false; main = plus + 1; }
    if (!kmPadInFromTok(main, &r.in)) return false;
    if (r.mod == r.in) return false;
    *b = r;
    return true;
}
void kmPadName(PadBind b, char* out, int outsz) {
    if (padIsNone(b) || b.in >= PI_COUNT) { _snprintf(out, outsz, "%s", axs(AXS_SM_UNASSIGNED)); out[outsz-1] = 0; return; }
    char core[128];
    if (b.mod != PI_NONE && b.mod < PI_COUNT)
        _snprintf(core, sizeof core, axs(AXS_PAD_CHORD_FMT), axs(kPadNames[b.mod].spoken), axs(kPadNames[b.in].spoken));
    else
        _snprintf(core, sizeof core, "%s", axs(kPadNames[b.in].spoken));
    core[sizeof core - 1] = 0;
    if (b.hold) _snprintf(out, outsz, axs(AXS_PAD_HOLD_FMT), core);
    else        _snprintf(out, outsz, "%s", core);
    out[outsz - 1] = 0;
}

// ---- Persistence ----
static void kmFilePath(char* out, int outsz) {
    out[0] = 0;
    const char* local = getenv("LOCALAPPDATA");
    if (!local || !*local) return;                    // no LOCALAPPDATA: no persistence (logged)
    char dir[MAX_PATH];
    _snprintf(dir, sizeof dir, "%s\\DarkestAccess", local);
    dir[sizeof dir - 1] = 0;
    CreateDirectoryA(dir, nullptr);                   // harmless if it already exists
    _snprintf(out, outsz, "%s\\keybinds.ini", dir);
    out[outsz - 1] = 0;
}

static void kmChordToken(KmChord c, char* out, int outsz) {
    char key[64];
    kmKeyToken(c.sym, key, sizeof key);
    _snprintf(out, outsz, "%s%s%s%s",
              (c.mods & KM_CTRL) ? "ctrl+" : "",
              (c.mods & KM_SHIFT) ? "shift+" : "",
              (c.mods & KM_ALT) ? "alt+" : "", key);
    out[outsz - 1] = 0;
}

static bool kmChordFromToken(const char* s, KmChord* c) {
    KmChord r = { 0, 0 };
    for (;;) {
        if (strncmp(s, "ctrl+", 5) == 0)       { r.mods |= KM_CTRL;  s += 5; }
        else if (strncmp(s, "shift+", 6) == 0) { r.mods |= KM_SHIFT; s += 6; }
        else if (strncmp(s, "alt+", 4) == 0)   { r.mods |= KM_ALT;   s += 4; }
        else break;
    }
    if (!kmKeyFromToken(s, &r.sym)) return false;
    *c = r;
    return true;
}

static void kmSlotId(int f, int s, char* out, int outsz) {
    if (kFuncs[f].slots == 1 || !kFuncs[f].slotTok[s]) _snprintf(out, outsz, "%s", kFuncs[f].id);
    else _snprintf(out, outsz, "%s.%s", kFuncs[f].id, kFuncs[f].slotTok[s]);
    out[outsz - 1] = 0;
}
static bool kmResolveId(const char* id, int* f, int* s) {
    for (int i = 0; i < KM_COUNT; i++) {
        size_t n = strlen(kFuncs[i].id);
        if (strncmp(id, kFuncs[i].id, n) != 0) continue;
        if (id[n] == 0) { if (kFuncs[i].slots != 1) return false; *f = i; *s = 0; return true; }
        if (id[n] != '.') continue;
        for (int k = 0; k < kFuncs[i].slots; k++)
            if (kFuncs[i].slotTok[k] && strcmp(id + n + 1, kFuncs[i].slotTok[k]) == 0) { *f = i; *s = k; return true; }
        return false;
    }
    for (int a = 0; a < KM_ALIAS_COUNT; a++) {
        if (strcmp(id, kAliases[a].oldId) != 0) continue;
        int fi = kmFind(kAliases[a].id);
        if (fi < 0) return false;
        if (!kAliases[a].slot) { *f = fi; *s = 0; return true; }
        for (int k = 0; k < kFuncs[fi].slots; k++)
            if (kFuncs[fi].slotTok[k] && strcmp(kAliases[a].slot, kFuncs[fi].slotTok[k]) == 0) { *f = fi; *s = k; return true; }
        return false;
    }
    return false;
}

static void kmSetDefaults() {
    for (int f = 0; f < KM_COUNT; f++)
        for (int s = 0; s < KM_SLOTS_MAX; s++) {
            g_curKey[f][s] = kFuncs[f].defKey[s]; g_blankKey[f][s] = false;
            g_curPad[f][s] = kFuncs[f].defPad[s]; g_blankPad[f][s] = padIsNone(kFuncs[f].defPad[s]);
        }
}

void kmEnsureLoaded() {
    if (g_kmLoaded) return;
    g_kmLoaded = true;
    kmSetDefaults();
    char path[MAX_PATH];
    kmFilePath(path, sizeof path);
    if (!path[0]) { logLine("keymap: no LOCALAPPDATA -- defaults only, nothing persists"); return; }
    FILE* f = fopen(path, "r");
    if (!f) { logLine("keymap: no %s -- defaults", path); kmRecountRemapped(); return; }
    char line[256];
    int applied = 0, bad = 0;
    while (fgets(line, sizeof line, f)) {
        char* p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (!*p || *p == '#' || *p == ';' || *p == '\n') continue;
        char* nl = strpbrk(p, "\r\n");
        if (nl) *nl = 0;
        char* eq = strchr(p, '=');
        if (!eq) continue;
        *eq = 0;
        char* id = p;
        const char* val = eq + 1;
        bool pad = false;
        size_t idl = strlen(id);
        if (idl > 4 && strcmp(id + idl - 4, ".pad") == 0) { pad = true; id[idl - 4] = 0; }
        int fi = -1, si = -1;
        if (!kmResolveId(id, &fi, &si)) { bad++; logLine("keymap: unknown id \"%s\" -- skipped", id); continue; }
        if (pad) {
            if (strcmp(val, "unbound") == 0) { g_blankPad[fi][si] = true; applied++; continue; }
            PadBind b;
            if (!kmPadFromToken(val, &b)) { bad++; logLine("keymap: bad pad input \"%s\" for %s -- default kept", val, id); continue; }
            for (int g = 0; g < KM_COUNT; g++) for (int k = 0; k < kFuncs[g].slots; k++)
                if ((g != fi || k != si) && kmOverlap(fi, g) && !g_blankPad[g][k] && kmPadEq(g_curPad[g][k], b)) g_blankPad[g][k] = true;
            g_curPad[fi][si] = b; g_blankPad[fi][si] = false;
            applied++;
            continue;
        }
        if (strcmp(val, "unbound") == 0) { g_blankKey[fi][si] = true; applied++; continue; }
        KmChord c;
        if (!kmChordFromToken(val, &c)) { bad++; logLine("keymap: bad chord \"%s\" for %s -- default kept", val, id); continue; }
        for (int g = 0; g < KM_COUNT; g++) for (int k = 0; k < kFuncs[g].slots; k++)
            if ((g != fi || k != si) && kmOverlap(fi, g) && !g_blankKey[g][k] && chordEq(g_curKey[g][k], c)) g_blankKey[g][k] = true;
        g_curKey[fi][si] = c; g_blankKey[fi][si] = false;
        applied++;
    }
    fclose(f);
    { int sf = kmFind("general.settings"); if (sf >= 0 && g_blankKey[sf][0]) { g_blankKey[sf][0] = false; g_curKey[sf][0] = kFuncs[sf].defKey[0]; logLine("keymap: the menu key was unbound in the file -- restored to its default"); } }
    kmRecountRemapped();
    logLine("keymap: %s -- %d line(s) applied, %d skipped, remapped=%d", path, applied, bad, (int)g_kmAnyRemapped);
}

void kmSaveFile() {
    char path[MAX_PATH];
    kmFilePath(path, sizeof path);
    if (!path[0]) { logLine("keymap: save skipped -- no LOCALAPPDATA"); return; }
    FILE* f = fopen(path, "w");
    if (!f) { logLine("keymap: save FAILED opening %s", path); return; }
    fprintf(f, "# DarkestAccess control remaps -- one line per binding that is off its default.\n");
    fprintf(f, "# <function>[.<slot>]=<chord>  (chord: [ctrl+][shift+][alt+]<key>)  or  =unbound\n");
    fprintf(f, "# <key> is a name (a, 7, enter, up, f5, kp3) or pos:<scancode> for a key with no\n");
    fprintf(f, "# ASCII face -- a position, so it follows the same physical key across layouts.\n");
    fprintf(f, "# <function>[.<slot>].pad=<input>  (input: [<modifier>+]<button>[:hold], buttons a b x y\n");
    fprintf(f, "# start back lb rb ls rs lt rt dpad_up dpad_down dpad_left dpad_right rs_up rs_down\n");
    fprintf(f, "# rs_left rs_right)  or  .pad=unbound\n");
    int written = 0;
    char sid[96], tok[96];
    for (int fi = 0; fi < KM_COUNT; fi++) for (int s = 0; s < kFuncs[fi].slots; s++) {
        kmSlotId(fi, s, sid, sizeof sid);
        if (g_blankKey[fi][s]) { fprintf(f, "%s=unbound\n", sid); written++; }
        else if (!chordEq(g_curKey[fi][s], kFuncs[fi].defKey[s])) {
            kmChordToken(g_curKey[fi][s], tok, sizeof tok);
            fprintf(f, "%s=%s\n", sid, tok); written++;
        }
        bool defNone = padIsNone(kFuncs[fi].defPad[s]);
        if (g_blankPad[fi][s]) { if (!defNone) { fprintf(f, "%s.pad=unbound\n", sid); written++; } }
        else if (defNone || !kmPadEq(g_curPad[fi][s], kFuncs[fi].defPad[s])) {
            kmPadToken(g_curPad[fi][s], tok, sizeof tok);
            fprintf(f, "%s.pad=%s\n", sid, tok); written++;
        }
    }
    fclose(f);
    logLine("keymap: saved %d remap(s) -> %s", written, path);
}

// ---- Snapshot / revert / dirty (the menu's save-question machinery) ----
void kmSnapshot() {
    memcpy(g_snapKey, g_curKey, sizeof g_curKey); memcpy(g_snapBlankKey, g_blankKey, sizeof g_blankKey);
    memcpy(g_snapPad, g_curPad, sizeof g_curPad); memcpy(g_snapBlankPad, g_blankPad, sizeof g_blankPad);
}
void kmRevert() {
    memcpy(g_curKey, g_snapKey, sizeof g_curKey); memcpy(g_blankKey, g_snapBlankKey, sizeof g_blankKey);
    memcpy(g_curPad, g_snapPad, sizeof g_curPad); memcpy(g_blankPad, g_snapBlankPad, sizeof g_blankPad);
    kmRecountRemapped();
}
bool kmDirty() {
    for (int f = 0; f < KM_COUNT; f++) for (int s = 0; s < kFuncs[f].slots; s++) {
        if (g_snapBlankKey[f][s] != g_blankKey[f][s] || !chordEq(g_snapKey[f][s], g_curKey[f][s])) return true;
        if (g_snapBlankPad[f][s] != g_blankPad[f][s] || !kmPadEq(g_snapPad[f][s], g_curPad[f][s])) return true;
    }
    return false;
}
void kmResetDefaults() {
    kmSetDefaults();
    kmRecountRemapped();
    kmSaveFile();
    kmSnapshot();
    logLine("keymap: all controls reset to defaults");
}

// ---- Assign / clear / restore (the capture's commit) ----
int kmAssignKey(int f, int s, KmChord c, int* victimSlot) {
    if (victimSlot) *victimSlot = -1;
    if (!kmOk(f, s)) return -1;
    int victim = -1;
    for (int g = 0; g < KM_COUNT && victim < 0; g++) {
        if (g != f && !kmOverlap(f, g)) continue;
        for (int k = 0; k < kFuncs[g].slots; k++) {
            if (g == f && k == s) continue;
            if (!g_blankKey[g][k] && chordEq(g_curKey[g][k], c)) { g_blankKey[g][k] = true; victim = g; if (victimSlot) *victimSlot = k; break; }
        }
    }
    g_curKey[f][s] = c; g_blankKey[f][s] = false;
    kmRecountRemapped();
    char tok[96], sid[96];
    kmChordToken(c, tok, sizeof tok); kmSlotId(f, s, sid, sizeof sid);
    logLine("keymap: %s = %s%s%s", sid, tok, victim >= 0 ? " (cleared " : "", victim >= 0 ? kFuncs[victim].id : "");
    return victim;
}
int kmAssignPad(int f, int s, PadBind b, int* victimSlot) {
    if (victimSlot) *victimSlot = -1;
    if (!kmOk(f, s) || padIsNone(b)) return -1;
    int victim = -1;
    for (int g = 0; g < KM_COUNT && victim < 0; g++) {
        if (g != f && !kmOverlap(f, g)) continue;
        for (int k = 0; k < kFuncs[g].slots; k++) {
            if (g == f && k == s) continue;
            if (!g_blankPad[g][k] && kmPadEq(g_curPad[g][k], b)) { g_blankPad[g][k] = true; victim = g; if (victimSlot) *victimSlot = k; break; }
        }
    }
    g_curPad[f][s] = b; g_blankPad[f][s] = false;
    char tok[96], sid[96];
    kmPadToken(b, tok, sizeof tok); kmSlotId(f, s, sid, sizeof sid);
    logLine("keymap: %s.pad = %s%s%s", sid, tok, victim >= 0 ? " (cleared " : "", victim >= 0 ? kFuncs[victim].id : "");
    return victim;
}
void kmClearKey(int f, int s) { if (kmOk(f, s)) { g_blankKey[f][s] = true; kmRecountRemapped(); } }
void kmClearPad(int f, int s) { if (kmOk(f, s)) g_blankPad[f][s] = true; }
void kmRestoreKey(int f, int s) {
    if (!kmOk(f, s)) return;
    int vs = -1; (void)kmAssignKey(f, s, kFuncs[f].defKey[s], &vs);
}
void kmRestorePad(int f, int s) {
    if (!kmOk(f, s)) return;
    if (padIsNone(kFuncs[f].defPad[s])) { g_blankPad[f][s] = true; return; }
    int vs = -1; (void)kmAssignPad(f, s, kFuncs[f].defPad[s], &vs);
}

bool kmFirstBlankKey(int* f, int* s) {
    for (int i = 0; i < KM_COUNT; i++) for (int k = 0; k < kFuncs[i].slots; k++)
        if (g_blankKey[i][k]) { *f = i; *s = k; return true; }
    return false;
}

bool kmIsCurrentKey(int f, uint32_t sym, uint16_t mod) {
    if (f < 0 || f >= KM_COUNT) return false;
    KmChord p = { sym, kmChordModsFromKmod(mod) };
    for (int s = 0; s < kFuncs[f].slots; s++) if (!g_blankKey[f][s] && chordEq(g_curKey[f][s], p)) return true;
    return false;
}

// ---- The translation layer ----
KmRegion kmRegionNow(uintptr_t base, AxContext ctx) {
    if (g_textInputActive) return KMR_NONE;
    switch (ctx) {
        case AX_NAMING: case AX_LOADING: case AX_GLOSSARY: case AX_HELP:
        case AX_PAUSE: case AX_DIALOG: case AX_JOURNAL: case AX_RESULTS:
        case AX_TITLE: case AX_SETTINGS: case AX_TUTORIAL: case AX_TOWNEVENT:
        case AX_CONTROLS:
            return KMR_NONE;
        default: break;
    }
    if (mapRoot(base)) return KMR_DUNGEON;
    if (resTownRoot(base)) return KMR_TOWN;
    return KMR_NONE;
}

bool kmTagActive(uintptr_t base, AxContext ctx, KmTag tag) {
    switch (tag) {
        case KMX_ANY:       return true;
        case KMX_PARTYROW:  return ctx == AX_ROOM && rvCursorOnParty(base);
        case KMX_DOORROW:   return ctx == AX_ROOM && rvCursorOnDoor(base);
        case KMX_BAG:       return ctx == AX_INV;
        case KMX_LOOT:      return ctx == AX_LOOT;
        case KMX_MAP:       return ctx == AX_MAP;
        case KMX_SHEET:     return ctx == AX_CHARSHEET;
        case KMX_REALMINV:  return ctx == AX_REALMINV;
        case KMX_PROVISION: return ctx == AX_PROVISION;
        case KMX_RING:      return ctx == AX_RING;
        case KMX_BLDG:      return ctx == AX_BLDG;
        case KMX_WAGON:     return ctx == AX_BLDG && bldColumnsLiveFor(base, "nomad_wagon");
        case KMX_COACH:     return ctx == AX_BLDG && bldColumnsLiveFor(base, "stage_coach");
    }
    return false;
}

static bool kmApplies(uintptr_t base, AxContext ctx, KmRegion r, int f) {
    const KmFunc& F = kFuncs[f];
    if (F.flags & KMF_GLOBAL) { if (g_textInputActive) return false; }
    else {
        if (r == KMR_NONE) return false;
        uint8_t bit = (r == KMR_TOWN) ? KMS_TOWN : KMS_DUNGEON;
        if (!(F.scope & bit)) return false;
    }
    return F.tag == KMX_ANY || kmTagActive(base, ctx, F.tag);
}

static uint8_t kmModsFromKmod(uint16_t mod) {
    uint8_t m = 0;
    if (mod & (KMOD_LSHIFT | KMOD_RSHIFT)) m |= KM_SHIFT;
    if (mod & (KMOD_LCTRL | KMOD_RCTRL))   m |= KM_CTRL;
    if (mod & (KMOD_LALT | KMOD_RALT))     m |= KM_ALT;
    return m;
}
uint8_t kmChordModsFromKmod(uint16_t mod) { return kmModsFromKmod(mod); }

static bool g_kmBypass = false;                      // axRoutePadKey's canonical chords pass untouched
void kmSetBypass(bool on) { g_kmBypass = on; }
int kmRoute(uintptr_t base, AxContext ctx, uint32_t* sym, uint16_t* mod, int* fnOut, int* slotOut) {
    kmEnsureLoaded();
    if (fnOut) *fnOut = -1;
    if (slotOut) *slotOut = -1;
    if (!g_kmAnyRemapped || g_kmBypass) return 0;
    KmRegion r = kmRegionNow(base, ctx);
    KmChord pressed = { *sym, kmModsFromKmod(*mod) };
    for (int pass = 0; pass < 2; pass++) {
        for (int f = 0; f < KM_COUNT; f++) {
            bool specific = kFuncs[f].tag != KMX_ANY;
            if (specific != (pass == 0)) continue;
            if (!kmApplies(base, ctx, r, f)) continue;
            for (int s = 0; s < kFuncs[f].slots; s++) {
                if (g_blankKey[f][s] || !chordEq(g_curKey[f][s], pressed)) continue;
                if (chordEq(kFuncs[f].defKey[s], pressed)) return 0;   // still on its default: nothing to do
                const uint16_t MODBITS = (uint16_t)(KMOD_LSHIFT | KMOD_RSHIFT | KMOD_LCTRL |
                                                    KMOD_RCTRL | KMOD_LALT | KMOD_RALT);
                uint16_t m = (uint16_t)(*mod & ~MODBITS);
                if (kFuncs[f].defKey[s].mods & KM_SHIFT) m |= KMOD_LSHIFT;
                if (kFuncs[f].defKey[s].mods & KM_CTRL)  m |= KMOD_LCTRL;
                if (kFuncs[f].defKey[s].mods & KM_ALT)   m |= KMOD_LALT;
                *sym = kFuncs[f].defKey[s].sym;
                *mod = m;
                if (fnOut) *fnOut = f;
                if (slotOut) *slotOut = s;
                return 1;
            }
        }
    }
    bool freed = false;
    for (int f = 0; f < KM_COUNT; f++) {
        if (!kmApplies(base, ctx, r, f)) continue;
        for (int s = 0; s < kFuncs[f].slots; s++) {
            if (!chordEq(kFuncs[f].defKey[s], pressed)) continue;
            if (!g_blankKey[f][s] && chordEq(g_curKey[f][s], kFuncs[f].defKey[s])) return 0;   // still home
            freed = true;
        }
    }
    return freed ? 2 : 0;
}

// ---- Forwarding a remapped GAME key (KMF_FWD), the keyboard side ----
static int      g_fwdFn = -1, g_fwdSlot = -1;
static uint32_t g_fwdHeldPlayerScan = 0;
static uint32_t g_fwdHeldSym = 0, g_fwdHeldScan = 0;   // ...and the canonical key it holds
static bool     g_fwdHeldRelDue = false;

static uint32_t g_fwdNoteScan = 0;
void kmForwardNote(int fn, int slot, uint32_t scan) { g_fwdFn = fn; g_fwdSlot = slot; g_fwdNoteScan = scan; }

static void kmForwardReleaseHeld(const char* why) {
    if (!g_fwdHeldSym) return;
    if (!enqueueSynthKey(SDL_EVT_KEYUP, g_fwdHeldScan, g_fwdHeldSym, 0)) {
        g_fwdHeldRelDue = true;
        logLine("keymap: forwarded '%c' release could not be queued, retrying", (char)g_fwdHeldSym);
        return;
    }
    logLine("keymap: forwarded held key '%c' released (%s)", (char)g_fwdHeldSym, why);
    g_fwdHeldSym = 0; g_fwdHeldScan = 0; g_fwdHeldPlayerScan = 0; g_fwdHeldRelDue = false;
}

bool kmForwardUnclaimed(uintptr_t base, bool repeat) {
    (void)base;
    if (g_fwdHeldRelDue) kmForwardReleaseHeld("retry");
    int f = g_fwdFn, s = g_fwdSlot;
    g_fwdFn = -1; g_fwdSlot = -1;
    if (f < 0 || s < 0 || !(kFuncs[f].flags & KMF_FWD)) return false;
    if (repeat) return true;
    KmChord c = kFuncs[f].defKey[s];
    uint32_t scan = klScancodeForKey(c.sym);
    if (!scan) { logLine("keymap: cannot forward %s -- the layout has no scancode for its key", kFuncs[f].id); return false; }
    uint16_t mod = 0;
    if (c.mods & KM_SHIFT) mod |= KMOD_LSHIFT;
    if (c.mods & KM_CTRL)  mod |= KMOD_LCTRL;
    if (c.mods & KM_ALT)   mod |= KMOD_LALT;
    if (kFuncs[f].flags & KMF_HELD) {
        if (g_fwdHeldSym) { if (g_fwdHeldSym == c.sym) return true; kmForwardReleaseHeld("another held key"); }
        if (!enqueueSynthKey(SDL_EVT_KEYDOWN, scan, c.sym, mod)) { logLine("keymap: forward of %s dropped (queue full)", kFuncs[f].id); return true; }
        g_fwdHeldSym = c.sym; g_fwdHeldScan = scan; g_fwdHeldPlayerScan = g_fwdNoteScan;
        logLine("keymap: %s -> holding the game's '%c'", kFuncs[f].id, (char)c.sym);
        return true;
    }
    enqueueSynthKey(SDL_EVT_KEYDOWN, scan, c.sym, mod);
    enqueueSynthKey(SDL_EVT_KEYUP,   scan, c.sym, mod);
    logLine("keymap: %s -> forwarded to the game", kFuncs[f].id);
    return true;
}

bool kmForwardKeyUp(uint32_t scan) {
    if (!g_fwdHeldSym || !scan || scan != g_fwdHeldPlayerScan) return false;
    kmForwardReleaseHeld("key released");
    return true;
}

void kmForwardService(uintptr_t base) {
    if (!g_fwdHeldSym) return;
    if (g_fwdHeldRelDue) { kmForwardReleaseHeld("retry"); return; }
    if (!mapRoot(base)) kmForwardReleaseHeld("not in a raid");
    else if (g_textInputActive) kmForwardReleaseHeld("a text field is typing");
}
