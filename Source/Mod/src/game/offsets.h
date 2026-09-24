// game/offsets.h — reverse-engineered RVAs, struct offsets and game-function typedefs.

#pragma once
#include <cstdint>

// ---- THE BINARY EVERY VALUE BELOW BELONGS TO ----
static const uint32_t GAME_PE_TIMESTAMP   = 0x6ab13309;
static const uint32_t GAME_PE_SIZEOFIMAGE = 0x2fae000;

// ---- THE DUAL TABLE: Steam AND DRM-free (GOG), one release, two binaries ----
static const uint32_t GAME_PE_TIMESTAMP_DRMFREE   = 0x6ab13742;
static const uint32_t GAME_PE_SIZEOFIMAGE_DRMFREE = 0x2f86000;

enum AxGameBuild { AX_BUILD_UNKNOWN = -1, AX_BUILD_STEAM = 0, AX_BUILD_DRMFREE = 1 };
AxGameBuild axGameBuild();
const char* axGameBuildName();
inline uintptr_t axRva(uintptr_t steam, uintptr_t drmfree) {
    return axGameBuild() == AX_BUILD_DRMFREE ? drmfree : steam;
}

// ---- Reverse-engineered offsets ----
inline const uintptr_t FOCUS_ID_RVA = axRva(0x1086f70, 0x1063f70);
inline const uintptr_t VEC_BEGIN_RVA = axRva(0x2c27128, 0x2bffd28);
inline const uintptr_t VEC_END_RVA = axRva(0x2c27130, 0x2bffd30);
static const uintptr_t ELEM_STRIDE      = 0x108;      // bytes per FocusElement
static const uintptr_t ELEM_ID_OFF      = 0x00;       // int64 id (matches FOCUS_ID)
static const uintptr_t ELEM_OWNER_OFF   = 0x08;       // ptr to owning widget
static const uintptr_t ELEM_POS_OFF       = 0x10;     // vec2 position (x,y floats)
static const uintptr_t ELEM_SIZE_OFF      = 0x18;     // vec2 size (w,h floats)
static const uintptr_t ELEM_FOCUSABLE_OFF = 0x24;     // byte: 1 = focusable
static const uintptr_t ELEM_SKIP_OFF      = 0x26;     // byte: nav skips if set

// ---- TextBoxWidget ----
inline const uintptr_t TBW_VFTABLE_RVA = axRva(0xe5ab60, 0xe3d998);
static const uintptr_t TBW_TEXT_OFF     = 0x198;      // inline UTF-8 text buffer (<=512)
static const uintptr_t TBW_TEXT_PTR_OFF = 0x398;      // fallback string pointer
// ---- Widget-tree layout (the WidgetCell shape; SharedUI layout primitives) ----
static const uintptr_t TL_KIDS_BEG_OFF     = 0x150;     // layout widget+: vector<Cell*> begin
static const uintptr_t TL_KIDS_END_OFF     = 0x158;     // layout widget+: vector<Cell*> end
static const uintptr_t TL_CELL_CONTENT_OFF = 0x150;     // cell+: its single content widget
static const uintptr_t TL_ATTACH_BEG_OFF   = 0x90;      // any widget+: attached-children begin
static const uintptr_t TL_ATTACH_END_OFF   = 0x98;      // any widget+: attached-children end
inline const uintptr_t RDATA_BEGIN_RVA = axRva(0xc65068, 0xc4a068);
inline const uintptr_t RDATA_END_RVA = axRva(0x107c000, 0x1059000);

// ---- Surface ROOT globals shared across modules ----
inline const uintptr_t MAP_ROOT_RVA = axRva(0x117db48, 0x1158040);
inline const uintptr_t RES_CAMPAIGN_RVA = axRva(0x117d9b8, 0x1157eb8);
static const uintptr_t RAID_IN_COMBAT_OFF = 0x4b20;  // raid root+: int, 0 = not in combat (was 0x4ab0)

// ---- TOWN MAP layout ----
static const uintptr_t TMB_OWNER_OFF    = 0x1730;
static const int       TMB_GROUP_COUNT  = 16;       // vector<Panel*> headers at owner+0
static const uintptr_t TMB_GROUP_STRIDE = 0x18;
static const uint32_t  TMB_GATE_SKIP    = 0x805;    // gate skip mask, applied to indices <= 11 only
static const uintptr_t TMB_SHOWN_OFF    = 0x58;     // panel+: open/shown state (town-wide convention)
static const uintptr_t TM_BLD_VEC_BEG      = 0x5570; // townRoot + -> object vector begin (was 0x55f8)
static const uintptr_t TM_BLD_VEC_END      = 0x5578; // townRoot + -> object vector end   (was 0x5600)
static const uintptr_t TM_LAYER_OFF        = 0x5588;
static const uintptr_t TM_BLD_DEF_OFF      = 0x18;   // object -> TownDefinition::BuildingType
static const uintptr_t TM_DEF_ID_OFF       = 0x00;   // BuildingType -> char id[0x40]
static const uintptr_t TM_DEF_HASH_OFF     = 0x40;   // BuildingType -> gameHash(id)
static const uintptr_t TM_DEF_SCREEN_OFF   = 0x4d;   // BuildingType -> char: 0 = opens no screen
static const uintptr_t TM_BLD_UNLOCKED_OFF = 0x184;  // char: 0 = locked (click never arms)
static const uintptr_t TM_BLD_HIDDEN_OFF   = 0x171;  // char: set + locked = not drawn at all
inline const uintptr_t TM_ARENA_DLC_RVA = axRva(0x2a5c08a, 0x2a34c8a);
static const uintptr_t TM_CAMP_DISTRICTS_UNLOCKED = 0x11f0; // Campaign+: char, Cornerstones fired
static const uintptr_t TM_CAMP_DISTRICTS_ENABLED  = 0x11f1; // Campaign+: char, feature on this save
inline const uintptr_t TM_DDIS_OVERRIDE_RVA = axRva(0x2a5d25b, 0x2a35e5b);

// ---- ItemStack layout ----
static const uintptr_t ITEM_STRIDE        = 0x5a8;  // bytes per ItemStack (was 0xe0)
static const uintptr_t ITEM_AMOUNT_OFF    = 0x00;   // int32 stack amount (<1 = empty slot)
static const uintptr_t INV_ITEMS_BEG_OFF  = 0x08;   // System+: ItemStack vector begin
static const uintptr_t INV_ITEMS_END_OFF  = 0x10;   // System+: ItemStack vector end
static const uintptr_t ITEM_TYPE_OFF      = 0x04;   // inline C-string: item type
static const uintptr_t ITEM_TYPEHASH_OFF  = 0x44;   // int32 hash of the type string
static const uintptr_t ITEM_ID_OFF        = 0x48;   // inline C-string: item id
static const uintptr_t ITEM_IDHASH_OFF    = 0x88;   // int32 hash of the item id string
static const uintptr_t ITEM_TRIG_LEFT_OFF  = 0xd0;  // int32 triggers remaining
static const uintptr_t ITEM_QUEST_LEFT_OFF = 0xd4;  // int32 quest uses remaining

// ---- MSVC red-black tree node layout ----
static const uintptr_t RBNODE_LEFT_OFF    = 0x00;
static const uintptr_t RBNODE_PARENT_OFF  = 0x08;
static const uintptr_t RBNODE_RIGHT_OFF   = 0x10;
static const uintptr_t RBNODE_ISNIL_OFF   = 0x19;
static const uintptr_t RBNODE_KEY_OFF     = 0x20;
static const uintptr_t RBNODE_VALUE_OFF   = 0x28;   // the effects map's value: vector<Effect*>

// ---- HeroClass naming fields ----
static const uintptr_t HEROCLASS_ID_OFF   = 0x08;    // HeroClass+: class id C-string (the %s)
static const uintptr_t HEROCLASS_DISP_OFF = 0x89;    // HeroClass+: cached localized display name
static const uintptr_t HEROCLASS_DISP_LATCH_OFF = 0xdd;  // HeroClass+: byte, display name resolved

// ---- TOWN-EVENT registry layout ----
inline const uintptr_t PROV_EVTREG_RVA = axRva(0x117d940, 0x1157e48);
static const uintptr_t PROV_EVTREG_BEG_OFF  = 0x18;     // reg+: vector<Event> begin (BY VALUE)
static const uintptr_t PROV_EVTREG_END_OFF  = 0x20;     // reg+: vector end
static const uintptr_t PROV_EVT_STRIDE      = 0x2f8;    // (was 0x2f0)
static const uintptr_t PROV_EVT_IDHASH_OFF  = 0x40;     // event+: id hash
static const uintptr_t PROV_EVT_DATA_BEG    = 0x1a8;    // event+: data records begin (was 0x1a0)
static const uintptr_t PROV_EVT_DATA_END    = 0x1b0;    // event+: data records end   (was 0x1a8)
static const uintptr_t PROV_EVTDATA_STRIDE  = 0x4c;
static const uintptr_t PROV_EVTDATA_HASH    = 0x44;     // record+: the id hash it matches (item
                                                        // TYPE for provisions, ACTIVITY for costs)
static const uintptr_t PROV_EVTDATA_AMT     = 0x48;     // record+: float amount
static const uintptr_t PROV_CAMP_EVENT_OFF  = 0x1838;

inline const uintptr_t RESOLVER_RVA = axRva(0x43c770, 0x42b100);
typedef void* (*ResolveFn)(void* out, const char* key);

// ---- Vtable-hook targets (patched by core/hooks.cpp; some offsets also read by their ----
inline const uintptr_t MENU_VFTABLE_RVA = axRva(0xed53d0, 0xeb7c68);
static const int       MENU_DRAW_SLOT   = 23;
inline const uintptr_t TUT_VFTABLE_RVA = axRva(0xed6b90, 0xeb9450);
static const int       TUT_SHOW_SLOT   = 23;
inline const uintptr_t TUT_LAYOUT_RVA = axRva(0x2becaa8, 0x2bc56a8);
static const uintptr_t TUT_LAYOUT_CLOSE_OFF = 0x24;    // close_button_offset, relative to base_pos
inline const uintptr_t JP_VFT_RVA = axRva(0xed4c70, 0xeb7540);
static const int       JP_SHOW_SLOT     = 23;
static const uintptr_t JP_STATE_OFF     = 0x58;      // int UI::Panel::Base lifecycle: 0 out, 1..4 up
static const uintptr_t JP_PAGE_OFF      = 0x410;     // uint: page index == the loc-key number
inline const uintptr_t LTO_VFTABLE_RVA = axRva(0xec55b8, 0xea7e88);
static const int       LTO_RENDER_SLOT = 5;
inline const uintptr_t TE_VFTABLE_RVA = axRva(0xeb38a0, 0xe960b0);
static const int       TE_SHOW_SLOT    = 23;
inline const uintptr_t PANEL_BANNER_VFTABLE_RVA = axRva(0xebaa98, 0xe9d350);
static const int       PANEL_UPDATE_SLOT        = 2;        // same vtable slot as Panel_Map's
inline const uintptr_t POPUP_PROC_RVA = axRva(0x76abd0, 0x756770);
inline const uintptr_t POPUP_CALLSITE_RVA = axRva(0x74fc4c, 0x73b91c);

// ---- Front-end flow states ----
static const int FE_STATE_PREAMBLE = 4;   // States::PREAMBLE (verify via festate log)
static const int FE_STATE_TITLE    = 5;   // States::TITLE — main menu (Campaign/Options/
static const int FE_STATE_SAVESLOTS = 7;  // States: the campaign-select / save-slot screen

// ---- New-campaign flow sub-state (FrontEndDisplay+0x1db0) ----
static const uintptr_t FE_NAMING_SUB_OFF = 0x1e20; // int: new-campaign flow sub-state.
static const int FE_NAMING_SUB_NAMING = 2;          // sub-state while the box is live
static const uint32_t FE_NAMING_SUB_MODEDIALOG = 1; // sub-state while the DIFFICULTY dialog is up.

// ---- ConfirmDialog answer focus ids ----
static const uint32_t  CONFIRM_ANSWER_BASE = 0x636e6661; // "afnc"; + answer index
static const int       CONFIRM_ANSWER_MAX  = 8;          // guard: at most 8 answers
static const uint32_t  UI_CLOSE_ELEM_ID    = 0x74636c73; // 'slct'

// ---- SDL2 event ABI ----
static const uint32_t  SDL_EVT_MOUSEMOTION   = 0x400;
static const uint32_t  SDL_EVT_MOUSEBUTTONUP = 0x402;
static const uint32_t  SDL_EVT_MOUSEWHEEL    = 0x403;
static const uintptr_t SDL_WHEEL_X_OFF       = 0x10; // SDL_MouseWheelEvent.x — CLICK DELTA, not a
static const uintptr_t SDL_WHEEL_Y_OFF       = 0x14; // coordinate (positive y = away from the user
                                                      // = scroll UP); bypasses designToWindow.
static const uintptr_t SDL_MOUSE_BUTTON_OFF  = 0x10; // SDL_MouseButtonEvent.button (Uint8)
static const uintptr_t SDL_MOUSE_STATE_OFF   = 0x11; // .state (1 pressed / 0 released)
static const uintptr_t SDL_MOUSE_CLICKS_OFF  = 0x12; // .clicks (Uint8)
static const uintptr_t SDL_MOUSE_X_OFF       = 0x14; // .x (Sint32) — motion & button share this
static const uintptr_t SDL_MOUSE_Y_OFF       = 0x18; // .y (Sint32)
static const int       SDL_BUTTON_LEFT       = 1;    // left mouse button = the game's confirm
static const int       SDL_BUTTON_RIGHT      = 3;    // SDL_BUTTON_RIGHT -- the game's cancel/back
static const int       SDL_EVENT_SIZE        = 56;   // sizeof(SDL_Event) in SDL2 (zero before filling)
static const uint32_t  SDL_EVT_KEYDOWN      = 0x300;  // SDL_KEYDOWN
static const uint32_t  SDL_EVT_KEYUP        = 0x301;  // SDL_KEYUP — the release half of a held key
static const uint32_t  SDL_EVT_TEXTINPUT    = 0x303;  // SDL_TEXTINPUT
static const uint32_t  SDL_EVT_MOUSEBUTTONDOWN      = 0x401;
static const uint32_t  SDL_EVT_JOYBUTTONDOWN        = 0x603;
static const uint32_t  SDL_EVT_CONTROLLERBUTTONDOWN = 0x651;
static const uint32_t  SDL_EVT_JOYAXISMOTION          = 0x600;
static const uint32_t  SDL_EVT_JOYDEVICEADDED         = 0x605;
static const uint32_t  SDL_EVT_JOYDEVICEREMOVED       = 0x606;
static const uint32_t  SDL_EVT_CONTROLLERAXISMOTION   = 0x650;
static const uint32_t  SDL_EVT_CONTROLLERDEVICEADDED  = 0x653;
static const uint32_t  SDL_EVT_CONTROLLERDEVICEREMOVED= 0x654;
static const uintptr_t SDL_TEXTINPUT_TEXT   = 0x0C;   // SDL_TextInputEvent.text[32] (UTF-8)
static const uintptr_t SDL_KEY_STATE_OFF    = 0x0C;
static const uintptr_t SDL_KEY_REPEAT_OFF   = 0x0D;
static const uintptr_t SDL_KEY_SCAN_OFF     = 0x10;   // SDL_KeyboardEvent.keysym.scancode (keysym+0x00).
static const uintptr_t SDL_KEY_SYM_OFF      = 0x14;   // SDL_KeyboardEvent.keysym.sym  (keysym+0x04)
static const uintptr_t SDL_KEY_MOD_OFF      = 0x18;
inline const uintptr_t INPUT_MODE_RVA = axRva(0x117cd94, 0x11572d8);

// ---- Building recruit displays: vftables + slot layout ----
inline const uintptr_t BLD_BHRD_VFT_RVA = axRva(0xeb2830, 0xe950f0);
inline const uintptr_t BLD_HRD_VFT_RVA = axRva(0xeb2410, 0xe94d08);
inline const uintptr_t BLD_SHRD_VFT_RVA = axRva(0xeb2968, 0xe95240);
static const uintptr_t BLD_RCT_SLOTS_BEG  = 0x350;    // recruit display+: slot vector begin (was 0x348)
static const uintptr_t BLD_RCT_SLOTS_END  = 0x358;
static const uintptr_t BLD_RCT_SLOT_STRIDE= 0x10;
static const uintptr_t BLD_RCT_IFACE_OFF  = 0x158;    // slot widget+: -> slot interface (the same
static const uintptr_t BLD_RCT_IFACE_HERO = 0x28;     // iface+: Hero*  — shape as EmbarkParty's)

// ---- Hero / HeroClass / skill / quirk / equipment layout ----
static const uint32_t  INV_FOURCC_BASE    = 0x696e7620;    // "inv " — focus id = base + slotIndex
static const uintptr_t HERO_NAME_OFF      = 0x08;    // Hero+: inline C-string, <=0x40 ("hero_name")
static const uintptr_t HERO_TRINKET_SYSTEM_OFF = 0x1210;  // Hero+: embedded Inventory::System (was 0x1190)
static const uintptr_t ACTOR_HEROCLASS_OFF = 0x12b0; // actor+: HeroClass* (was 0x1230)
static const uintptr_t ACTOR_ID_OFF       = 0x130c;
inline const uintptr_t HERO_VFT = axRva(0xe7c638, 0xe5efa0);
inline const uintptr_t MONSTER_VFT = axRva(0xe9b020, 0xe7d978);
static const float     CS_PERCENT_SCALE   = 100.0f;
static const uintptr_t HERO_WEAPON_LEVEL_OFF = 0x12bc;
static const uintptr_t HERO_ARMOUR_LEVEL_OFF = 0x12c0;
static const uintptr_t HEROCLASS_WEAPON_VEC  = 0xba0;   // HeroClass+: ptr to weapon array
static const uintptr_t HEROCLASS_ARMOUR_VEC  = 0xbb8;   // HeroClass+: ptr to armour array (3000 dec)
static const uintptr_t EQUIP_WEAPON_STRIDE   = 0x130;
static const uintptr_t EQUIP_ARMOUR_STRIDE   = 0x128;
static const uintptr_t EQUIP_NAME_OFF        = 0x40;    // inline C string: the item's name/id
static const uintptr_t EQUIP_SLOT_OFF        = 0x114;   // int: 0 = weapon, 1 = armour
static const uintptr_t EQUIP_W_DMG_LO_OFF    = 0x11c;   // int   str_stat_base_damage, low
static const uintptr_t EQUIP_W_DMG_HI_OFF    = 0x120;   // int   str_stat_base_damage, high
static const uintptr_t EQUIP_W_CRIT_OFF      = 0x124;   // float str_stat_base_crit (x CS_PERCENT_SCALE)
static const uintptr_t EQUIP_W_SPD_OFF       = 0x128;   // int   str_stat_base_speed
static const uintptr_t EQUIP_A_DEF_OFF       = 0x118;   // float str_stat_base_defense (x scale)
static const uintptr_t EQUIP_A_HP_OFF        = 0x120;   // int   str_stat_base_health_points
static const uintptr_t HERO_QUIRK_BEGIN_OFF  = 0x1310; // Hero+: -> first entry        (was 0x1290)
static const uintptr_t HERO_QUIRK_END_OFF    = 0x1318; // Hero+: -> one past the last  (was 0x1298)
static const uintptr_t QUIRK_ENTRY_STRIDE    = 0x38;   // (was 0x30)
static const uintptr_t QUIRK_ENTRY_CLASS_OFF = 0x00;   // entry+: Quirk::Class const*
static const uintptr_t QUIRK_ID_OFF          = 0x00;   // Quirk::Class+: inline C-string id
inline const uintptr_t QUIRK_REGISTRY_RVA = axRva(0x117d988, 0x1157e90);
static const uintptr_t QUIRK_REGISTRY_STRIDE = 0x1f8;
static const uintptr_t ACTOR_SEL_BEG_OFF  = 0x1358;
static const uintptr_t ACTOR_SEL_END_OFF  = 0x1360;
static const uintptr_t ACTOR_SLOTMAP_OFF  = 0x1370;
static const uintptr_t ACTOR_SLOTMAP_END_OFF = 0x1378;
static const uintptr_t HEROCLASS_SKILLVEC_OFF = 0xbd0;
static const uintptr_t HEROCLASS_SKILLVEC_END_OFF = 0xbd8; // HeroClass+: end of that outer vector
static const uintptr_t SKILL_STRIDE       = 0x508;   // bytes per ActorCombatSkill (was 0x4f8)
static const uintptr_t SKILL_ID_OFF       = 0x08;    // ActorCombatSkill+: inline C-string skill id
inline const uintptr_t RAID_SCREEN_RVA = axRva(0x117ddf0, 0x1158248);
static const uintptr_t CS_PANEL_OPEN_OFF  = 0x58;
static const uintptr_t ACTOR_IS_MONSTER_OFF = 0x1296; // actor+: char, 0 = hero          (was 0x1216)
static const uintptr_t ACTOR_TRAIT_OFF    = 0x11e0;
static const uintptr_t ACTOR_VIRTUE_OFF   = 0x11e4;
static const uintptr_t HERO_RESOLVE_XP_OFF  = 0x12b8; // Hero+: uint32 "resolve_xp"        (was 0x1238)
inline const uintptr_t RESOLVE_TABLE_RVA = axRva(0x117d938, 0x1157e40);
static const uintptr_t RESOLVE_VEC_BEGIN    = 0x00;
static const uintptr_t RESOLVE_VEC_END      = 0x08;
static const int       RESOLVE_MAX_LEVELS   = 16;     // sanity cap (the ladder ships 7: 0..6)
static const int       EQUIP_SLOT_COUNT     = 2;      // weapon + armour; the game draws exactly two
static const int       TRINKET_SLOT_COUNT   = 2;
static const uintptr_t SKILL_LAUNCH_MASK_OFF = 0x3a4; // uint launch-rank bitmask (.launch)   (was 0x398)
static const uintptr_t SKILL_TGTMASK_OFF  = 0x3a8;
static const uintptr_t SKILL_NONATTACK_OFF = 0x90;    // != 0 -> no preview (heals, buffs, moves)
// ---- Skill USE GATES: the two things that make a drawn button refuse the click ----
static const uintptr_t SKILL_USE_KEY_OFF   = 0x4c;
static const uintptr_t SKILL_PER_TURN_LIMIT_OFF   = 0x94;
static const uintptr_t SKILL_PER_BATTLE_LIMIT_OFF = 0x98;
static const uintptr_t SKILL_HP_RANGE_SET_OFF = 0x431;
static const uintptr_t SKILL_HP_RANGE_MIN_OFF = 0x434;
static const uintptr_t SKILL_HP_RANGE_MAX_OFF = 0x438;
static const uintptr_t ACTOR_BATTLE_USES_MAP_OFF  = 0x11b8; // Hero+: _Myhead ptr  (was 0x1138)
static const uintptr_t ACTOR_BATTLE_USES_SIZE_OFF = 0x11c0; // Hero+: that map's element count
static const uintptr_t HEROCLASS_CAMPVEC_OFF     = 0x10f0;
static const uintptr_t HEROCLASS_CAMPVEC_END_OFF = 0x10f8;
static const uintptr_t ACTOR_CAMP_KNOWN_OFF      = 0x1340;
static const uintptr_t ACTOR_CAMP_KNOWN_END_OFF  = 0x1348;
inline const uintptr_t CAMP_REGISTRY_RVA = axRva(0x117d968, 0x1157e70);
static const uintptr_t CAMP_REGISTRY_STRIDE = 0x18;
static const uintptr_t CAMP_SKILL_HASH_OFF = 0x48;    // SkillClass+: hash of the id (h = h*0x35 + c)
inline const uintptr_t RI_CIRCUS_FLAG_RVA = axRva(0x2a5c08b, 0x2a34c8b);

// ---- Roster list / embark strip layout ----
static const uintptr_t PTY_ROSTERLIST_OFF    = 0x2f48;
static const uintptr_t PTY_RL_ROWS_BEG_OFF   = 0xa0;     // rosterList+: row records begin
static const uintptr_t PTY_RL_ROWS_END_OFF   = 0xa8;     // rosterList+: ... end (stride 0x40)
static const uintptr_t PTY_RL_ROW_ENTRY_OFF  = 0x90;     // row widget -> Roster::Entry*
static const uintptr_t PTY_ENTRY_HERO_OFF    = 0x08;     // Roster::Entry+: the Hero, embedded inline
static const uintptr_t PTY_ENTRIES_BEG_OFF   = 0x20;     // Roster::System+: vector<Roster::Entry*> begin
static const uintptr_t PTY_ENTRIES_END_OFF   = 0x28;     // Roster::System+: ... end
static const uintptr_t PTY_IFACE_HERO_OFF    = 0x28;     // slot interface -> Hero* (0 = empty)
static const uintptr_t PTY_SLOTS_BEG_OFF     = 0x36c8;
static const uintptr_t PTY_SLOTS_END_OFF     = 0x36d0;
static const uintptr_t PTY_SLOT_IFACE_OFF    = 0x158;    // slot widget -> slot interface
static const uintptr_t ACTOR_STRESS_OFF      = 0x128c;   // Hero+: float, current stress (was 0x120c)
inline const uintptr_t RI_TOGGLE_RVA = axRva(0x673ec0, 0x660170);
typedef void (*RiToggleFn)(void);

// ---- The IN-RAID MAP's area graph, its projection and the party move fn ----
// ---- In-raid MAP: area graph layout ----
static const uintptr_t MAP_AREAVEC_BEG  = 0x740;     // map+: vector<Area*> begin (was 0x6f0)
static const uintptr_t MAP_AREAVEC_END  = 0x748;     // map+: vector<Area*> end   (was 0x6f8)
static const uintptr_t MAP_CUR_AREA_ID  = 0x758;
static const uintptr_t MAP_CUR_AREA_PTR = 0x55d0;    // map+: Area* current party area (was 0x5560)
static const uintptr_t PANEL_MAP_HOVER_ID = 0xad4;
static const uintptr_t MAP_PARTY_X_OFF  = 0x24;      // map+: float party x
static const uintptr_t MAP_PARTY_Y_OFF  = 0x28;      // map+: float party y
static const uintptr_t MAP_PARTY_POS_OFF = 0x294;    // map+: float position along the area
static const uintptr_t AREA_TILES_BEG   = 0x00;      // area+: Tile* begin
static const uintptr_t AREA_TILES_END   = 0x08;      // area+: Tile* end
static const uintptr_t AREA_ID_OFF      = 0x18;      // area+: int area id (uint key)
static const uintptr_t AREA_KIND_OFF    = 0x1c;      // area+: int kind (0=room, 1=corridor)
static const uintptr_t AREA_KNOWLEDGE_OFF = 0x110;
static const uintptr_t AREA_EXITS_OFF   = 0x28;      // area+: first Door slot
static const uintptr_t AREA_EXIT_STRIDE = 0x10;      // 16 bytes per Door
static const int       AREA_EXIT_SLOTS  = 8;         // 8 fixed slots (+0x28 .. +0xa8)
static const uintptr_t DOOR_DEST_OFF    = 0x00;      // door+: int destination area id (FourCC)
static const uintptr_t DOOR_TILE_OFF    = 0x04;      // door+: int tile index within that area
static const uintptr_t TILE_STRIDE      = 0xc8;      // 200 bytes per Tile
static const uintptr_t TILE_TYPE_OFF    = 0x00;      // tile+: int type (1 normal/2 junction/3 door)
static const uintptr_t TILE_SUB_OFF     = 0x04;      // tile+: int sub/direction candidate
static const uintptr_t TILE_OWNER_ID_OFF= 0x14;      // tile+: int owner area id
static const uintptr_t TILE_NEIGH_ID_OFF= 0x18;      // tile+: int neighbor area id (junctions)
static const uintptr_t TILE_X_OFF       = 0x28;      // tile+: float grid x
static const uintptr_t TILE_Y_OFF       = 0x2c;      // tile+: float grid y
static const uintptr_t TILE_KNOWLEDGE_OFF = 0x9c;    // tile+: int TileKnowledge (0/1/2/3, monotone)
static const uintptr_t TILE_CONTENT_OFF = 0xa0;      // tile+: int content enum (AreaContent)
static const uint32_t  FOURCC_NONE      = 0x656E6F6E; // "none" — empty exit / non-junction neighbour

static const uintptr_t TILE_HDOOR_DEST_TILE_OFF = 0x1c; // tile+: int tile index inside the secret room
static const uintptr_t TILE_HDOOR_KIND_OFF      = 0x24; // tile+: int door kind — 2 = hidden door
static const int       TILE_HDOOR_KIND_HIDDEN   = 2;
static const uintptr_t TILE_HDOOR_SEEN_OFF      = 0x44; // tile+: byte, the door has been FOUND
static const uintptr_t TILE_HDOOR_ACCESS_OFF    = 0x45; // tile+: byte, hidden_door_accessible (template)
inline const uintptr_t RD_INTERACT_HIDDEN_RVA = axRva(0x768140, 0x753d50);

inline const uintptr_t MAP_PROJ_SCALE_RVA = axRva(0x2bd1e00, 0x2baaa00);
inline const uintptr_t MAP_PROJ_CONST_RVA = axRva(0xed7724, 0xeb9f04);
inline const uintptr_t MAP_PROJ_PANX_RVA = axRva(0x2bd1df0, 0x2baa9f0);
inline const uintptr_t MAP_PROJ_PANY_RVA = axRva(0x2bd1df4, 0x2baa9f4);
inline const uintptr_t MAP_SCREEN_W_RVA = axRva(0xed793c, 0xeba114);
inline const uintptr_t MAP_SCREEN_H_RVA = axRva(0xed7908, 0xeba0e4);
                                                       // party-tile-index formula's offset constant (raidmap.cpp)
inline const uintptr_t MAP_PAN2_X_RVA = axRva(0x2bd1df8, 0x2baa9f8);
inline const uintptr_t MAP_PAN2_Y_RVA = axRva(0x2bd1dfc, 0x2baa9fc);
inline const uintptr_t MAP_MS_OFFX = axRva(0x2c28730, 0x2c01328), MAP_MS_OFFY = axRva(0x2c28734, 0x2c0132c);
inline const uintptr_t MAP_MS_DIVX = axRva(0x2c28738, 0x2c01330), MAP_MS_DIVY = axRva(0x2c2873c, 0x2c01334);
inline const uintptr_t MAP_MS_NUM = axRva(0xed7788, 0xeb9f68);
inline const uintptr_t MAP_MS_BNDX = axRva(0x2c28740, 0x2c01338), MAP_MS_BNDY = axRva(0x2c28744, 0x2c0133c);
inline const uintptr_t FOCUSED_WIDGET_RVA = axRva(0x1086f68, 0x1063f68);
inline const uintptr_t MOVE_FN_RVA = axRva(0x763be0, 0x74f7f0);
inline const uintptr_t MOVE_GATE_RVA = axRva(0x427a80, 0x416360);
inline const uintptr_t MOVE_MGR_RVA = axRva(0x117ddf0, 0x1158248);
inline const uintptr_t MAP_ZOOM2_RVA = axRva(0x2bd1e04, 0x2baaa04);

static const int AREA_CONTENT_TRAP = 3;         // needed by name (the trap prop)
static const int AREA_CONTENT_OBSTACLE = 4;     // needed by name (the visited gate: the arrival
static const int AREA_CONTENT_CURIO = 7;        //   handler stamps knowledge 3 on the NEXT tile
static const int AREA_CONTENT_HIDDEN_DOOR = 13; // needed by name (the secret door's own word)

// ---- Which panel owns the shared corner (map / inventory / skills) ----
static const uintptr_t RD_PANEL_ARR_OFF  = 0x3140;
static const uintptr_t RD_ACTIVE_IDX_OFF = 0x1768;
static const int       RD_PANEL_COUNT    = 3;       // the game's own bound check is "< 3"

// ---- Phase 3 round 22: what the DUNGEON VIEW and dllmain.cpp both read ----
static const uintptr_t RAID_PARTY_BEG_OFF = 0x08;    // raid root+: vector<Hero*> begin
static const uintptr_t RAID_PARTY_END_OFF = 0x10;    // raid root+: vector<Hero*> end
static const uintptr_t PROP_TYPE_OFF           = 0xc8;  // Prop+: PropType* (Prop::GetPropTypeData)
static const uintptr_t PROP_INTERACT_TABLE_OFF = 0xd0;  // Prop+: interaction table ptr
static const uintptr_t PROP_TYPE_ENUM_OFF      = 0x0c;  // Prop+: int32 kind (0/1 curio, 2 trap, 3 door)
static const uintptr_t PROP_INLINE_NAME_OFF    = 0x125; // Prop+: inline C-string id
static const uintptr_t MONSTER_SKILL_STRIDE = 0x4f8;    // (was 0x4e8)
static const uintptr_t MONSTER_SKILL_ID_OFF = 0x08;     // inline char[0x40]
static const char* const MT_SKILL_KEY_FMT   = "str_monster_skill_%s";
// ---- THE SEEN-SKILLS STORE ----
inline const uintptr_t MONSTER_SKILL_SEEN_MAP_RVA = axRva(0x117d9e0, 0x1157ed8);
static const uintptr_t ACTOR_CUR_HP_OFF      = 0x1038; // Hero+: float, current health (was 0xfbc)
static const uintptr_t ACTOR_RANK_MASK_OFF   = 0x1018;
static const uintptr_t ACTOR_SCREEN_X_OFF    = 0x48;   // float: WORLD x (camera-relative), both sides

static const uintptr_t ACTOR_GUID_VFT_OFF    = 0x28;   // vtable slot: uint32 guid
static const int       ACTOR_BUFF_STRIDE     = 0x1d8;  // (was 0x158)
static const uintptr_t ACTOR_BUFF_RECORD_OFF = 0x08;   // the Buff the describer takes (was 0x04)
static const int       CAMP_BUFF_REC_SZ      = 0x1d0;  // the copy's byte count = sizeof(Buff) (was 0x158)
static const uintptr_t BATTLE_OBJ_OFF      = 0x4898;  // raid root+: the Battle object (was 0x4848)
static const uintptr_t BATTLE_SURPRISE_OFF = 0x12c;   // Battle+: int tri-state  (was 0x114)
static const uintptr_t BATTLE_ROUND_OFF    = 0xa8;
static const int SURPRISE_NONE = 0, SURPRISE_PARTY = 1, SURPRISE_MONSTERS = 2;
static const uintptr_t BATTLE_TURNVEC_BEG_OFF = 0x20;  // Battle+: turn-order vector begin (was 0x08)
static const uintptr_t BATTLE_TURNVEC_END_OFF = 0x28;  // Battle+: ... end                 (was 0x10)
static const uintptr_t BATTLE_TURN_IDX_OFF    = 0xa0;  // Battle+: uint current turn index  (was 0x88)
static const uintptr_t BATTLE_TURNENTRY_DEAD_OFF = 0x0c;
static const int BS_TURN_START = 0x1c;

// ---- THE BUTCHER'S CIRCUS PIT: choosing WHICH hero acts (dlc/butchers_circus/pit.cpp) ----
inline const uintptr_t PIT_ACTIVATE_HERO_RVA = axRva(0x5a7450, 0x595720);
inline const uintptr_t PIT_CAN_ACTIVATE_RVA = axRva(0x5a9cd0, 0x597ef0);
inline const uintptr_t PIT_SELECT_HERO_RVA = axRva(0x5a7810, 0x595ad0);
static const uintptr_t BATTLE_ACTIVATED_OFF  = 0x1e9;     // Battle+: byte, a hero is activated
static const int BS_BEFORE_TURN_START = 0x11;
#define PIT_SELECT_HERO_KEY "str_ui_select_a_hero"
#define PIT_HOLD_BANNER_KEY "hold_to_activate_hero"
// ---- TELLING THE OPPONENT WHAT YOUR HERO DID ----
inline const uintptr_t MP_SEND_TURN_RVA = axRva(0x7604f0, 0x74c140);
static const uintptr_t ACTOR_CHOSEN_SKILL_OFF = 0x8e0;
static const uintptr_t ACTOR_TARGETS_OFF      = 0x8f0;   // {begin, end, cap}
inline const uintptr_t MP_VEC_ASSIGN_RVA = axRva(0x4729a0, 0x461320);
// ---- Battle::SetSingleTarget -- WHERE THE MISSING RNG DRAW LIVES ----
inline const uintptr_t SET_SINGLE_TARGET_RVA = axRva(0x5a14d0, 0x58f940);
static const int BS_TURN_CHOOSING_LO = 0x1c;
static const int BS_TURN_CHOOSING_HI = 0x1e;
static const int BS_TURN_DO_SKILL    = 0x20;
static const uintptr_t CS_STAT_MAXHP_OFF = 0x768;   // (was 0x708)
static const uintptr_t CS_STAT_DEF_OFF   = 0x560;   // (was 0x500)
static const uintptr_t CS_STAT_PROT_OFF  = 0x5c8;   // (was 0x568)
static const uintptr_t CS_STAT_SPD_OFF   = 0x630;   // (was 0x5d0)
static const int       CS_RESIST_COUNT   = 8;
static const uintptr_t SKILL_EFFECTS_OFF = 0x3c8;   // ActorCombatSkill+: vector<Effect*> (was 0x3b8)
static const uint32_t  SKILL_RANK_BITS   = 0xf;
static const uintptr_t RD_EVENT_OFF      = 0x20b8;
static const uintptr_t OE_STATE_OFF      = 0xd88;
static const int32_t   OE_STATE_LIVE     = 2;
static const uintptr_t OE_CURIO_PROP_OFF = 0x6a0;
static const uintptr_t OE_OTHER_PROP_OFF = 0xd70;
static const uintptr_t PT_CATEGORY_OFF = 0x108;     // inline C-string, e.g. "curio"
static const uintptr_t PT_UINAME_OFF   = 0x4c8;     // inline C-string, e.g. "travellers_tent"

// ---- Phase 3 round 23: what the ACTION BAR and dllmain.cpp both read ----
const uintptr_t CS_STAT_ATT_OFF    = 0x698;   // str_ui_ATT_MOD          (was 0x638)
const uintptr_t CS_STAT_CRIT_OFF   = 0x700;   // str_ui_CRIT             (was 0x6a0)
const uintptr_t CS_STAT_DMGLO_OFF  = 0x490;   // str_ui_DMG, low end     (was 0x430)
const uintptr_t CS_STAT_DMGHI_OFF  = 0x4f8;   // str_ui_DMG, high end    (was 0x498)
const int CS_RESIST_DEATHBLOW_IDX = 6;
const int CS_RESIST_TRAP_IDX      = 7;
const uintptr_t ACTOR_CLASS_OFF    = 0x12b0;  // Hero+: HeroClass*       (was 0x1230)
const uintptr_t ACTOR_STAT_OFF     = 0x768;
const uintptr_t ACTOR_HP_MASK_OFF  = 0x790;
const float     ACTOR_MAX_STRESS   = 200.0f;
const uintptr_t STAT_FLAT_BEG_OFF  = 0x30;    // stat+: float* begin, the flat contributions
const uintptr_t STAT_FLAT_END_OFF  = 0x38;    // stat+: float* end
const uintptr_t STAT_MULT_BEG_OFF  = 0x48;    // stat+: float* begin, the percent contributions
const uintptr_t STAT_MULT_END_OFF  = 0x50;    // stat+: float* end
const uintptr_t STAT_MIN_OFF       = 0x60;    // stat+: float, clamp low
const uintptr_t STAT_MAX_OFF       = 0x64;    // stat+: float, clamp high
const int       STAT_MAX_ENTRIES   = 128;     // sanity cap on a vector length read from memory
const char* const HEALTH_FMT_KEY = "tray_status_bar_tooltip_health_format";
const char* const STRESS_FMT_KEY = "tray_status_bar_tooltip_stress_format";
const char* const TURNS_FMT_KEY  = "tray_status_bar_tooltip_turns_remaining_format";
const uintptr_t ACTOR_MODE_ID_OFF  = 0x11d8;
const uintptr_t HEROCLASS_MOVESKILL_OFF = 0xbe8;
const uint32_t  REST_FOURCC      = 0x72657374; // "rest" — the respite screen's finish button
inline const uintptr_t CAMP_BUFFREG_FLAG_RVA = axRva(0x2a5c08b, 0x2a34c8b);
inline const uintptr_t CAMP_BUFF_HASDESC_RVA = axRva(0x7b9e50, 0x7a5a00);
inline const uintptr_t EFF_BUFFLIST_BUILD_RVA = axRva(0x4bbde0, 0x4aa700);
inline const uintptr_t EFF_BUFFLIST_FREE_RVA = axRva(0x4c36f0, 0x4b1fe0);
inline const uintptr_t BUFF_RENDER_ONE_RVA = axRva(0x7b9f70, 0x7a5b20);
const uintptr_t EFF_BUFFLIST_STRIDE    = 0x1d0;
const int       EFF_BUFFLIST_MAX       = 32;       // sanity cap, same spirit as AB_EFF_MAX_PER_VEC
const uintptr_t EFF_SUMMON_BEG_OFF   = 0x1b0;      // vector begin; end at +0x1b8 (was 0x1a8)
const uintptr_t EFF_SUMMON_END_OFF   = 0x1b8;      //                            (was 0x1b0)
const uintptr_t EFF_SUMMON_STRIDE    = 0x14;       // the game steps piVar10 + 5 ints
const int       EFF_SUMMON_MAX       = 8;          // sanity cap on one effect's summon list
inline const uintptr_t MONCLASS_VEC_RVA = axRva(0x2c2a870, 0x2c03410);
inline const uintptr_t MONCLASS_VEC_ALT_RVA = axRva(0x2c2a888, 0x2c03428);
const uintptr_t MONCLASS_HASH_OFF    = 0xcc;       // int: the id hash the summon entry carries
const int       MONCLASS_MAX         = 4096;       // sanity cap on the registry walk
const uintptr_t SKILL_LEVEL_OFF    = 0x394;
const uintptr_t SKILL_VALIDMODES_OFF      = 0x3b8;
const uintptr_t SKILL_VALIDMODES_SIZE_OFF = 0x3c0;
const char* const SKILL_NAME_FMT   = "combat_skill_name_%s_%s";  // % (class id, skill id)
inline const uintptr_t MOVE_SKILL_RVA = axRva(0x5af000, 0x59d090);
inline const uintptr_t SKILL_PCT_MULT_RVA = axRva(0xed78d4, 0xeba0b0);
inline const uintptr_t EFF_ONE_RVA = axRva(0xed7788, 0xeb9f68);

// ---- Phase 3 round 24: what THE CAMP and dllmain.cpp both read ----
const uintptr_t ACTOR_CAMP_SEL_BEG_OFF    = 0x1328;
const uintptr_t ACTOR_CAMP_SEL_END_OFF    = 0x1330;
const int       CAMP_SEL_MAX              = 4;      // lambda_5's own test: (end-beg)/4 < 4
const uintptr_t CAMP_SKILL_USE_LIMIT_OFF = 0x54;  // always 1 in shipped data
const uintptr_t CAMP_SKILL_EFFVEC_OFF     = 0x58;  // SkillClass+: first effect
const uintptr_t CAMP_SKILL_EFFVEC_END_OFF = 0x60;  // SkillClass+: one past the last
const uintptr_t CAMP_EFF_STRIDE           = 0xf8;
const uintptr_t CAMP_EFF_SELECTION_OFF    = 0x00;  // inline id ("individual"), the header's stem

// ---- Phase 3 round 27: what the STATUS PANEL and the LIVE COMBAT TEXT both read ----
const int       ACTOR_BUFF_MAX        = 64;     // sanity cap on a corrupt/garbage buff vector
const uintptr_t BUFFREC_STAT_TYPE_OFF = 0x00;   // int   "stat_type"
const uintptr_t BUFFREC_AMOUNT_OFF    = 0x48;   // float "amount"
const uintptr_t BUFFREC_ROUNDS_OFF    = 0x4c;
const uintptr_t BUFFREC_DURTYPE_OFF   = 0x50;   // int   "duration_type"
const uintptr_t BUFFREC_STATSUB_OFF   = 0x04;   // string "stat_sub_type" (e.g. "unholy")
const uintptr_t BUFFREC_SOURCE_OFF    = 0x54;   // int   "source" -- a source-TYPE enum, named live
const int       STAT_TYPE_STEALTH     = 0x2b;   // the one condition stat_type the combat text

// ---- dllmain.cpp ----
inline const uintptr_t POOL_PTR_RVA = axRva(0xd9edf0, 0x0);
inline const uintptr_t KEYBIND_TABLE_RVA = axRva(0x2a6f0d0, 0x2a47cd0);
// ---- core\subtitles.cpp ----
inline const uintptr_t DARKEST_APP_RVA = axRva(0x2a5bb40, 0x2a34740);
// ---- dlc\butchers_circus\prizebox.cpp ----
inline const uintptr_t PB_VFT_RVA = axRva(0xea9d08, 0xe8c5d8);
inline const uintptr_t PB_OPEN_BANNER_RVA = axRva(0x659c80, 0x645f50);
static const uintptr_t PB_BC_SECTIONS_BEG  = 0xa8;
static const uintptr_t PB_BC_SECTIONS_END  = 0xb0;
static const uintptr_t PB_BC_SECTION_STRIDE= 0x48;
static const uintptr_t PB_BC_SECTION_NAME  = 0x10;
static const uintptr_t PB_BC_PIECES_BEG    = 0x30;   // section+
static const uintptr_t PB_BC_PIECES_END    = 0x38;
static const uintptr_t PB_BC_PIECE_STRIDE  = 0xb8;
static const uintptr_t PB_BC_PIECE_NAME    = 0x38;   // piece+
static const uintptr_t PB_BC_PIECE_UNLOCKED= 0x00;   // piece+ byte
// ---- dlc\butchers_circus\rankings.cpp: THE RANKING BOARD ----
inline const uintptr_t RB_VFT_RVA = axRva(0xeaa0c8, 0xe8c998);
static const uint32_t  RB_TAB_ELEM_ID = 0x727369;
// ---- dlc\butchers_circus\banner.cpp: THE BANNER DESIGNER ----
inline const uintptr_t BD_VFT_RVA = axRva(0xea5ee0, 0xe88868);
static const uint32_t  BD_ELEM_ID = 0x627369;
inline const uintptr_t BD_STEP_RVA = axRva(0x636f90, 0x6250a0);
inline const uintptr_t BD_PIECES_PER_ROW_RVA = axRva(0x2b7d83c, 0x2b5643c);
// ---- dlc\butchers_circus\matchresults.cpp + prizebox.cpp: the PRIZE-BOOTH RECORDS ----
static const uintptr_t BCR_CAMP_BLDMAP_OFF = 0x11f8;
static const uintptr_t BCR_MAPNODE_LEFT    = 0x00;
static const uintptr_t BCR_MAPNODE_RIGHT   = 0x10;
static const uintptr_t BCR_MAPNODE_ISNIL   = 0x19;
static const uintptr_t BCR_MAPNODE_KEY     = 0x20;
static const uintptr_t BCR_MAPNODE_VALUE   = 0x28;
static const uintptr_t BCR_PB_LVLVEC_BEG   = 0x170;
static const uintptr_t BCR_PB_LVLVEC_END   = 0x178;
static const uintptr_t BCR_PB_LVLREC_STRIDE= 0x30;
static const uintptr_t BCR_RC_TRINK_BEG    = 0x00;   // record+: trinket vector begin
static const uintptr_t BCR_RC_TRINK_END    = 0x08;
static const uintptr_t BCR_RC_TRINK_STRIDE = 0x48;
static const uintptr_t BCR_RC_TRINK_HASH   = 0x40;   // entry+: the trinket's id HASH
static const uintptr_t BCR_RC_BANNER_BEG   = 0x18;   // record+: banner-part vector begin
static const uintptr_t BCR_RC_BANNER_END   = 0x20;
static const uintptr_t BCR_RC_BANNER_STRIDE= 0x30;
static const uintptr_t BCR_RC_BANNER_NAME  = 0x00;   // entry+: char[0x20] part name
static const uintptr_t BCR_RC_BANNER_SECTION = 0x20; // entry+: char[0x10] section name
// ---- dlc\butchers_circus\dueling.cpp ----
inline const uintptr_t DG_VFT_RVA = axRva(0xea6398, 0xe88f80);
inline const uintptr_t IW_VFT_RVA = axRva(0xea65d8, 0x0);
inline const uintptr_t IW_SEND_RVA = axRva(0x819b20, 0x0);
inline const uintptr_t MP_API_RVA = axRva(0x117d040, 0x0);
inline const uintptr_t MP_ALT_GATE_RVA = axRva(0x2a5cd7f, 0x2a3597f);
inline const uintptr_t MP_DISCONNECT_RVA = axRva(0x3625d0, 0x0);
inline const uintptr_t MP_SENT_ERASE_RVA = axRva(0x363a10, 0x0);
inline const uintptr_t DG_EDIT_TOGGLE_RVA = axRva(0x63a540, 0x627b50);
// ---- dlc\butchers_circus\ring.cpp ----
inline const uintptr_t RING_PANEL_VFT_RVA = axRva(0xeb4d60, 0xe97750);
inline const uintptr_t RING_TOGGLE_RVA = axRva(0x648d60, 0x6350f0);
inline const uintptr_t RING_INSPECT_RVA = axRva(0x648c20, 0x634fb0);
inline const uintptr_t RANKDISP_VFT_RVA = axRva(0xeb5638, 0xe97f60);
inline const uintptr_t RING_PUTDOWN_RVA = axRva(0x648640, 0x6349d0);
inline const uintptr_t UI_PLAY_SOUND_RVA = axRva(0x22fac0, 0x22d800);
inline const uintptr_t UI_DRAGDROP_MGR_RVA = axRva(0x117cd98, 0x11572e0);
inline const uintptr_t RING_MM_VFT_RVA = axRva(0xea6e68, 0xe89780);
inline const uintptr_t TOWNDISP_VFT_RVA = axRva(0xeb4e68, 0xe97688);
static const int       TD_PROGRESS_FWD_SLOT = 20;      // ...::ProgressForwardInternal
inline const uintptr_t TD_PROGRESS_FWD_RVA = axRva(0x6d5d30, 0x6c1b20);
// ---- the DIRECT-CHALLENGE (lobby) ready handshake ----
inline const uintptr_t RING_CHALLENGE_ID_RVA = axRva(0x2a5c1b0, 0x2a34db0);
                                                  // 0 = no direct challenge (solo / ranked queue)
inline const uintptr_t RING_CHALLENGE_STATE_RVA = axRva(0x2a5bb40, 0x2a34740);
                                                  // Same address as DARKEST_APP_RVA (+0x00).
inline const uintptr_t RING_NOT_STEAM_RVA = axRva(0x2a5cd7f, 0x2a3597f);
inline const uintptr_t STEAM_FRIENDS_CTX_RVA = axRva(0x109fd28, 0x0);
// ---- dlc\color_of_madness\wave.cpp ----
inline const uintptr_t COM_WAVE_QID_RVA = axRva(0x26186e4, 0x25f3134);
inline const uintptr_t COM_TIER_KEY_TAB_RVA = axRva(0xde8d00, 0xdcd2e8);
inline const uintptr_t COM_THRESHOLD_FN_RVA = axRva(0x503650, 0x4f1e70);
inline const uintptr_t COM_CAN_ADVANCE_FN_RVA = axRva(0x5034e0, 0x4f1d00);
// ---- frontend\dialog.cpp ----
inline const uintptr_t CD_SYSTEM_PTR_RVA = axRva(0x117de50, 0x1158328);
// ---- frontend\display.cpp ----
inline const uintptr_t FE_DISPLAY_PTR_RVA = axRva(0x117d8b0, 0x1157dc0);
inline const uintptr_t FE_NEWGAME_RVA = axRva(0x416bc0, 0x406550);
inline const uintptr_t FE_DELETE_RVA = axRva(0x40f660, 0x3ff140);
inline const uintptr_t FE_CONTENT_REG_RVA = axRva(0x117d868, 0x1157d80);
// ---- frontend\loading.cpp ----
inline const uintptr_t LS_PTR_RVA = axRva(0x117de68, 0x1158338);
// ---- frontend\naming.cpp ----
inline const uintptr_t COMMIT_NAME_RVA = axRva(0x2a5cd90, 0x2a35990);
// ---- frontend\options.cpp ----
inline const uintptr_t OPT_DESC_ARR_RVA = axRva(0x2a620c0, 0x2a3acc0);
inline const uintptr_t OPT_CAT_ARR_RVA = axRva(0x2a61ef0, 0x2a3aaf0);
inline const uintptr_t OPT_VALARR_PTR_RVA = axRva(0x117d888, 0x1157da0);
inline const uintptr_t OPT_LANG_BEGIN_RVA = axRva(0x2a87f30, 0x2a60b30);
inline const uintptr_t OPT_LANG_END_RVA = axRva(0x2a87f38, 0x2a60b38);
inline const uintptr_t OPT_LANG_CUR_RVA = axRva(0x2a87fa0, 0x2a60ba0);
static const int       OPT_DESC_COUNT    = 48;
inline const uintptr_t OPT_ROWVIS_FN_RVA = axRva(0x8032d0, 0x7eef40);
inline const uintptr_t OPT_TOGGLE_CB_RVA = axRva(0x8066d0, 0x7f2340);
// ---- frontend\sharedui.cpp ----
inline const uintptr_t GL_SINGLETON_RVA = axRva(0x117de48, 0x1158320);
inline const uintptr_t GL_VFT_RVA = axRva(0xec94f8, 0xeabdd8);
inline const uintptr_t HP_SINGLETON_RVA = axRva(0x117de40, 0x1158318);
inline const uintptr_t HP_VFT_RVA = axRva(0xec9270, 0xeabb50);
inline const uintptr_t CP_SINGLETON_RVA = axRva(0x117de38, 0x1158310);
inline const uintptr_t CP_VFT_RVA = axRva(0xec8f30, 0xeab930);
inline const uintptr_t JP_SYSCLASS_RVA = axRva(0x117d998, 0x1157ea0);
// ---- input\synth.cpp ----
inline const uintptr_t FE_CURSOR_X_RVA = axRva(0x117cda8, 0x11572dc);
inline const uintptr_t FE_CURSOR_Y_RVA = axRva(0x117cdac, 0x11572f0);
inline const uintptr_t CTRL_CONNECTED_RVA = axRva(0x1180129, 0x115a5f9);
inline const uintptr_t INPUT_GATE_RVA = axRva(0x117cd90, 0x115729c);
inline const uintptr_t MOUSE_SUPPRESS_RVA = axRva(0x1174ada, 0x114f02a);
inline const uintptr_t MOUSE_SUPPRESS_B_RVA = axRva(0x1174adb, 0x114f02b);
inline const uintptr_t MOUSE_INWINDOW_RVA = axRva(0x1174add, 0x114f02d);
inline const uintptr_t INPUT_ENABLE_VEC_RVA = axRva(0x2c27a20, 0x2c00620);
inline const uintptr_t INPUT_ENABLE_SIZE_RVA = axRva(0x2c27a38, 0x2c00638);
inline const uintptr_t VP_OFF_X_RVA = axRva(0x2c28730, 0x2c01328);
inline const uintptr_t VP_OFF_Y_RVA = axRva(0x2c28734, 0x2c0132c);
inline const uintptr_t VP_DEN_X_RVA = axRva(0x2c28738, 0x2c01330);
inline const uintptr_t VP_DEN_Y_RVA = axRva(0x2c2873c, 0x2c01334);
inline const uintptr_t VP_BOUND_X_RVA = axRva(0x2c28740, 0x2c01338);
inline const uintptr_t VP_BOUND_Y_RVA = axRva(0x2c28744, 0x2c0133c);
inline const uintptr_t VP_NUM_RVA = axRva(0xed7788, 0xeb9f68);
// ---- raid\actionbar.cpp ----
inline const uintptr_t TP_CUTOFF_MISS_RVA = axRva(0x2acb8f8, 0x2aa44f8);
inline const uintptr_t TP_CUTOFF_HIT_RVA = axRva(0x2acb8fc, 0x2aa44fc);
inline const uintptr_t TP_DEATHBLOW_RVA = axRva(0x2acbba0, 0x2aa47a0);
inline const uintptr_t BUFF_UPKEEP_RVA = axRva(0x47b510, 0x469e90);
inline const uintptr_t TARGET_VALID_RVA = axRva(0x484430, 0x472db0);
inline const uintptr_t TARGET_ANY_RVA = axRva(0x484280, 0x472c00);
inline const uintptr_t DO_SKILL_RVA = axRva(0x5af430, 0x59d4c0);
inline const uintptr_t EFF_PCT_D_RVA = axRva(0xed7838, 0xeba018);
inline const uintptr_t TOGGLE_CHARSHEET_RVA = axRva(0x427990, 0x416270);
// ---- raid\camp.cpp ----
inline const uintptr_t CAMP_PERFORM_RVA = axRva(0x5bce30, 0x5aaec0);
inline const uintptr_t MEAL_TABLE_RVA = axRva(0x2acb8c8, 0x2aa44c8);
inline const uintptr_t MEAL_COUNT_RVA = axRva(0x2acb8d0, 0x2aa44d0);
inline const uintptr_t CAMP_CAN_START_RVA = axRva(0x7658f0, 0x751500);
// ---- raid\combattext.cpp ----
inline const uintptr_t POPUP_TYPE_TABLE_RVA = axRva(0x2bd68d0, 0x2baf4d0);
// ---- raid\dungeonview.cpp ----
inline const uintptr_t RD_INTERACT_TRAP_RVA = axRva(0x767f70, 0x753b80);
inline const uintptr_t RD_INTERACT_PROP_RVA = axRva(0x768320, 0x753f30);
inline const uintptr_t TRAP_SCOUT_BONUS_RVA = axRva(0x2acbbf0, 0x2aa47f0);
inline const uintptr_t TRAP_DIFF_BASE_BEG_RVA = axRva(0x2acbe68, 0x2aa4a68);
inline const uintptr_t TRAP_DIFF_BASE_END_RVA = axRva(0x2acbe70, 0x2aa4a70);
inline const uintptr_t REACH_BOX_WIDTH_RVA = axRva(0x2acbbb8, 0x2aa47b8);
inline const uintptr_t REACH_BOX_HEIGHT_RVA = axRva(0x2acbbbc, 0x2aa47bc);
// ---- raid\eventscroll.cpp ----
inline const uintptr_t OE_ACT_INVESTIGATE = axRva(0x707ef0, 0x6f3b90);
inline const uintptr_t OE_ACT_CURIO_PASS = axRva(0x707e00, 0x6f3aa0);
inline const uintptr_t OE_ACT_CLEAR = axRva(0x708300, 0x6f3fa0);
inline const uintptr_t OE_ACT_OBST_PASS = axRva(0x7080d0, 0x6f3d70);
inline const uintptr_t OE_ACT_ANCESTOR = axRva(0x708090, 0x6f3d30);
inline const uintptr_t OE_ACT_EAT = axRva(0x707780, 0x6f3420);
inline const uintptr_t OE_ACT_STARVE = axRva(0x707710, 0x6f33b0);
inline const uintptr_t HUNGER_ROUND_RVA = axRva(0xed7788, 0xeb9f68);
inline const uintptr_t FOOD_TYPEHASH_RVA = axRva(0x2a77a34, 0x2a520a4);
inline const uintptr_t OE_USE_ITEM_RVA = axRva(0x72c010, 0x717cb0);
inline const uintptr_t ITEM_ASSIGN_RVA = axRva(0x3f1f80, 0x3e1cb0);
inline const uintptr_t ITEM_CLEAR_RVA = axRva(0x5d2ed0, 0x5c0f60);
inline const uintptr_t OE_INTERACT_LOOKUP_RVA = axRva(0x4aa880, 0x4991a0);
inline const uintptr_t OE_OBSTACLE_CLEAR_BODY_RVA = axRva(0x703ae0, 0x6ef780);
inline const uintptr_t CURIO_TRACKER_RVA = axRva(0x117d9d0, 0x1157ed0);
inline const uintptr_t CURIO_TRACKER_QUERY_RVA = axRva(0x5572c0, 0x545a00);
inline const uintptr_t CURIO_TRACKER_GATE_RVA = axRva(0x4247d0, 0x4130b0);
inline const uintptr_t QUEST_ITEM_TYPEHASH_RVA = axRva(0x2a794a8, 0x2a539b8);
// ---- raid\inventory.cpp ----
inline const uintptr_t INV_NOID_TYPEHASH_RVA = axRva(0x2a80f38, 0x2a5b608);
inline const uintptr_t DUNGEON_CTRL_RVA = axRva(0x117d8d8, 0x1157de8);
inline const uintptr_t HERO_USE_ITEM_RVA = axRva(0x424f40, 0x413820);
inline const uintptr_t USE_ITEM_PICK_RVA = axRva(0x426570, 0x414e50);
inline const uintptr_t INV_DISCARD_RVA = axRva(0x72a840, 0x7164e0);
inline const uintptr_t INV_POUR_RVA = axRva(0x5d5a20, 0x5c3ab0);
inline const uintptr_t INV_SETIDENT_RVA = axRva(0x5d3730, 0x5c17c0);
// ---- raid\light.cpp ----
inline const uintptr_t TORCH_TABLE_RVA = axRva(0x2acbb20, 0x2aa4720);
inline const uintptr_t TORCH_TITLE_FMT_RVA = axRva(0x109b2bc, 0x10780ac);
// ---- raid\loot.cpp ----
inline const uintptr_t LOOT_TAKE_ONE_RVA = axRva(0x70ad60, 0x6f6a00);
// ---- raid\quest.cpp ----
inline const uintptr_t QT_GOALDESC_RVA = axRva(0x808fa0, 0x7f4c10);
inline const uintptr_t QT_LOGVEC_RVA = axRva(0x117d930, 0x1157e38);
static const uintptr_t QT_LOGVEC_BEG       = 0x48;      // owner+: definition vector begin
static const uintptr_t QT_LOGVEC_END       = 0x50;      // owner+: definition vector end
static const uintptr_t QT_LOG_STRIDE       = 0x780;     // (was 0x730)
static const uintptr_t QT_LOG_QUESTID_OFF  = 0x40;      // def+0x40 == Quest+0x40 identifies the entry
static const int       QT_LOG_MAX          = 256;       // sanity cap on the definition walk
inline const uintptr_t QT_RETREAT_CONFIRM_RVA = axRva(0x73fc10, 0x72b8e0);
inline const uintptr_t QT_RETREAT_CAP4_RVA = axRva(0x109b440, 0x1078230);
inline const uintptr_t QT_NOABANDON_QID_RVA = axRva(0x2a63bc8, 0x2a3e098);
inline const uintptr_t QT_WAVE_QID_RVA = axRva(0x26186e4, 0x25f3134);
inline const uintptr_t QT_PANEL_VFTABLE_RVA = axRva(0xebe428, 0xea0598);
// ---- raid\raidmap.cpp ----
inline const uintptr_t PARTY_TILE_K_OFFSET_RVA = axRva(0xed7908, 0xeba0e4);
inline const uintptr_t PARTY_TILE_K_SCALE_RVA = axRva(0xed76bc, 0xeb9e9c);
inline const uintptr_t KEY_IS_DOWN_RVA = axRva(0x289bb0, 0x287440);
inline const uintptr_t KEY_STATE_ARRAY_RVA = axRva(0x2c27b50, 0x2c00750);
// ---- raid\results.cpp ----
inline const uintptr_t RR_DISPLAY_RVA = axRva(0x117ddf8, 0x11582e0);
                                                       // app-state 0xb, NULLED leaving — the gate
inline const uintptr_t RR_OUTCOME_KEY_RVA = axRva(0x2be0530, 0x2bb9130);
inline const uintptr_t RR_PROFILE_GET_RVA = axRva(0x54c950, 0x53b090);
inline const uintptr_t RR_GOLD_TYPEHASH_RVA = axRva(0x2a7c964, 0x2a56fd4);
// ---- sheet\charsheet.cpp ----
inline const uintptr_t CS_TRK_UNEQUIP_RVA = axRva(0x7f8270, 0x7e3f10);
inline const uintptr_t CS_COMSKILL_TOGGLE_RVA = axRva(0x5c9b10, 0x5b7ba0);
// ---- sheet\composers.cpp ----
inline const uintptr_t CS_RESIST_TABLE_RVA = axRva(0x2ad3810, 0x2aac410);
inline const uintptr_t CS_RESIST_VALUE_RVA = axRva(0x478600, 0x466f80);
inline const uintptr_t CS_QUIRK_DESC_RVA = axRva(0x80b0c0, 0x7f6d30);
inline const uintptr_t CAMP_ET_SKIP_RVA = axRva(0x2a801b4, 0x2a5a8b4);
inline const uintptr_t CAMP_ET_PCT_RVA[2]  = { axRva(0x2a804c8, 0x2a5aa98), axRva(0x2a80334, 0x2a5aa34) };
inline const uintptr_t CAMP_ET_RAW_RVA[3]  = { axRva(0x2a80218, 0x2a5a918), axRva(0x2a801b8, 0x2a5a8b8), axRva(0x2a80274, 0x2a5a974) };
inline const uintptr_t CAMP_ET_PLAIN_RVA[7] = { axRva(0x2a80154, 0x2a5a744), axRva(0x2a80338, 0x2a5aa38), axRva(0x2a80214, 0x2a5a914), axRva(0x2a800f4, 0x2a5a6f4),
                                                axRva(0x2a800f8, 0x2a5a6f8), axRva(0x2a80158, 0x2a5a748), axRva(0x2a802d4, 0x2a5a9d4) }; // format, verbatim
inline const uintptr_t CAMP_ET_SUFFIX_RVA = axRva(0x2a800f8, 0x2a5a6f8);
inline const uintptr_t CAMP_ET_ITEM_RVA = axRva(0x2a80278, 0x2a5a978);
inline const uintptr_t CAMP_ET_BUFF_RVA = axRva(0x2a802d8, 0x2a5a9d8);
inline const uintptr_t CAMP_BUFFREG_RVA = axRva(0x117d960, 0x1157e68);
inline const uintptr_t CAMP_BUFF_DESC_RVA = axRva(0x7ba490, 0x7a6040);
inline const uintptr_t BUFF_SRCTYPE_TABLE_RVA = axRva(0x2aa62a0, 0x2a7eea0);
inline const uintptr_t BUFF_STATTYPE_TABLE_RVA = axRva(0x2aa7030, 0x2a7fc30);
inline const uintptr_t BUFF_DURTYPE_TABLE_RVA = axRva(0x2aa6cc0, 0x2a7f8c0);
inline const uintptr_t SCOUT_BASE_RVA = axRva(0x2acbb94, 0x2aa4794);
inline const uintptr_t BUFF_RULE_GATE_RVA = axRva(0x475210, 0x463b90);
inline const uintptr_t TRAIT_REG_RVA = axRva(0x117d9a8, 0x1157eb0);
// ---- town\buildings.cpp ----
inline const uintptr_t BLD_URD_VFT_RVA = axRva(0xeb3b30, 0xe96420);
inline const uintptr_t BLD_REG_BEGIN_RVA = axRva(0x2c29fa0, 0x2c02b40);
inline const uintptr_t BLD_REG_END_RVA = axRva(0x2c29fa8, 0x2c02b48);
inline const uintptr_t BLD_TSD_VFT_RVA = axRva(0xeaed20, 0xe919d8);
inline const uintptr_t BLD_CTSD_VFT_RVA = axRva(0xeaf008, 0xe91688);
inline const uintptr_t BLD_SWITCH_FN_RVA = axRva(0x685da0, 0x672050);
inline const uintptr_t BLD_CTSD_SHOWROW_RVA = axRva(0x687a90, 0x673d40);
inline const uintptr_t BLD_SHARD_PRICE_KEY_RVA = axRva(0x2a6baf8, 0x2a46348);
inline const uintptr_t BLD_PRICE_FN_RVA = axRva(0x591560, 0x57fc90);
inline const uintptr_t BLD_PRICE_KEY_RVA = axRva(0x2a6ba34, 0x2a46234);
inline const uintptr_t BLD_TRINKDB_PTR_RVA = axRva(0x117d978, 0x1157e80);
inline const uintptr_t BLD_TRECDB_BEG_RVA = axRva(0x2c2aa20, 0x2c035c0);
inline const uintptr_t BLD_TRECDB_END_RVA = axRva(0x2c2aa28, 0x2c035c8);
inline const uintptr_t BLD_TRKGRP_TABLE_RVA = axRva(0x2acda90, 0x2aa6690);
inline const uintptr_t BLD_CLASSDB_RVA = axRva(0x2c2a7b8, 0x2c03358);
inline const uintptr_t BLD_CLASSDB_ALT_RVA = axRva(0x2c2a798, 0x2c03338);
inline const uintptr_t BLD_RCT_INSPECT_RVA = axRva(0x6b96e0, 0x6a5990);
inline const uintptr_t BLD_RCT_INSPECT_VFT = axRva(0xeb2330, 0xe94c60);
inline const uintptr_t BLD_STATUE_VFT_RVA = axRva(0xeb2eb8, 0xe95420);
inline const uintptr_t ST_DISPATCH_RVA = axRva(0x6bd6c0, 0x6a9970);
inline const uintptr_t BLD_GRAVE_VFT_RVA = axRva(0xeacc80, 0xe8f560);
inline const uintptr_t RCT_COUNT_RVA = axRva(0x5814b0, 0x56fbf0);
inline const uintptr_t RCT_MAX_RVA = axRva(0x5815a0, 0x56fce0);
inline const uintptr_t BLD_HSRD_VFT_RVA = axRva(0xeae588, 0xe90e40);
inline const uintptr_t ACT_SLOTLOCKED_RVA = axRva(0x52c790, 0x51af80);
inline const uintptr_t ACT_EVTLOCKED_RVA = axRva(0x52c6f0, 0x51aee0);
inline const uintptr_t ACT_COSTSMONEY_RVA = axRva(0x52c850, 0x51b040);
inline const uintptr_t ACT_COSTBYID_RVA = axRva(0x52c8e0, 0x51b0d0);
inline const uintptr_t ACT_CANAFFORD_RVA = axRva(0x55eb20, 0x54d260);
inline const uintptr_t ACT_COSTFREE_RVA = axRva(0x3a2560, 0x396200);
inline const uintptr_t ACT_CANHERO_RVA = axRva(0x52c240, 0x51aa30);
inline const uintptr_t ACT_CONTAGION_LEVEL_RVA = axRva(0x6328e0, 0x6209f0);
static const uintptr_t ACT_CONTAGION_TIP_OFF   = 0x2b0;     // ItemContainerDisplay+: the tooltip
inline const uintptr_t INF_SYS_RVA = axRva(0x117d920, 0x1157e28);
static const uintptr_t INF_GATE_OFF      = 0x30;      // sys+: char, the builder's first early-out
static const uintptr_t INF_LVLS_OFF      = 0x38;      // sys+: level-def vector begin (end at +8)
static const uintptr_t INF_ELEMS_OFF     = 0x50;      // sys+: sequence-element vector begin (end +8)
static const uintptr_t INF_LVL_STRIDE    = 0x78;
static const uintptr_t INF_ELEM_STRIDE   = 0x60;
                                                      //   show_ui char@+0x5c (the json field, 1:1)
static const uintptr_t INF_CAMP_CUR_OFF  = 0x17d4;    // Campaign+: current sequence-element id hash
inline const uintptr_t ACT_REQFREE_RVA = axRva(0x2325a0, 0x2302e0);
inline const uintptr_t ACT_REQ_NOTQUIRKS_VFT = axRva(0xe8dc98, 0xe70628);
                                                         //   NotHaveQuirks::vftable (RTTI, pass 62)
inline const uintptr_t ACT_PICK_RVA = axRva(0x684ac0, 0x670d70);
inline const uintptr_t ACT_CONFIRM_RVA = axRva(0x67ece0, 0x66af90);
inline const uintptr_t ACT_UNCOMMIT_RVA = axRva(0x67f1e0, 0x66b490);
inline const uintptr_t BLD_QTAD_VFT_RVA = axRva(0xeb0028, 0xe92918);
inline const uintptr_t QT_CLICK_RVA = axRva(0x697560, 0x683810);
inline const uintptr_t QT_ACTMULT_RVA = axRva(0x117d910, 0x1157e18);
inline const uintptr_t HA_BSMITH_VFT_RVA = axRva(0xeaa3c0, 0xe8cc98);
inline const uintptr_t HA_GUILD_VFT_RVA = axRva(0xead028, 0xe8f8d8);
inline const uintptr_t HA_CAMPT_VFT_RVA = axRva(0xeab298, 0xe8db78);
inline const uintptr_t HA_PICK_RVA = axRva(0x67b880, 0x667b30);
inline const uintptr_t HA_PICK_VFT_RVA = axRva(0xead900, 0xe90220);
inline const uintptr_t HA_PURCH_MAP_RVA = axRva(0x2b54640, 0x2b2d240);
inline const uintptr_t HA_ALLBOUGHT_RVA = axRva(0x2b54690, 0x2b2d290);
inline const uintptr_t HA_DISC1_RVA = axRva(0x2b54650, 0x2b2d250);
inline const uintptr_t HA_DISC2_RVA = axRva(0x2b54660, 0x2b2d260);
inline const uintptr_t HA_DISC3_RVA = axRva(0x2b54670, 0x2b2d270);
inline const uintptr_t BN_LAYOUT_MAP_RVA = axRva(0x2b7b9d8, 0x2b545d8);
// ---- town\districts.cpp ----
inline const uintptr_t DST_VFT_RVA = axRva(0xeabab0, 0xe8e3c8);
inline const uintptr_t DST_LAYOUT_RVA = axRva(0x2c2c870, 0x2c05410);
// ---- town\embark.cpp ----
inline const uintptr_t EMB_MASTERY_RVA = axRva(0x564f70, 0x5536b0);
inline const uintptr_t EMB_NAMEID_RVA = axRva(0x80a650, 0x7f62c0);
inline const uintptr_t EMB_SPECIFICS_RVA = axRva(0x808e00, 0x7f4a70);
inline const uintptr_t EMB_FWDLABEL_RVA = axRva(0x6d5ae0, 0x6c18d0);
inline const uintptr_t EMB_ROAMMGR_RVA = axRva(0x117db40, 0x1158038);
// ---- town\estate.cpp ----
inline const uintptr_t TL_ROOT_RVA = axRva(0x117dbe0, 0x11580d0);
inline const uintptr_t TL_CTRL_RVA = axRva(0x117d8e0, 0x1157df0);
inline const uintptr_t TL_VFT_RVA = axRva(0xea9470, 0xe8b248);
inline const uintptr_t TL_SHOW_RVA = axRva(0x6cc730, 0x6b89e0);
inline const uintptr_t TL_CLOSE_RVA = axRva(0x6cc210, 0x6b84c0);
// ---- town\events.cpp ----
inline const uintptr_t TE_INFO_RVA = axRva(0x632020, 0x620130);
inline const uintptr_t TE_HERO_AT_RVA = axRva(0x58d190, 0x57b8c0);
inline const uintptr_t TE_INSPECT_RVA = axRva(0x6c6750, 0x6b2a00);
inline const uintptr_t TE_INSPECT_VFT = axRva(0xeb35c8, 0xe96270);
static const uintptr_t TE_DISP_SLOTS_BEG  = 0x120;      // TownEventDisplay+: vector<slot widget*> begin
static const uintptr_t TE_DISP_SLOTS_END  = 0x128;
inline const uintptr_t TE_FOCUSMGR_RVA = axRva(0x117d858, 0x1157d78);
inline const uintptr_t TE_POPUPPOS_A_RVA = axRva(0x2771fb8, 0x0);
inline const uintptr_t TE_POPUPPOS_B_RVA = axRva(0x2771fc0, 0x0);
inline const uintptr_t TE_UISCALE_RVA = axRva(0xed7724, 0xeb9f04);
// ---- town\exchange.cpp ----
inline const uintptr_t EX_TOGGLE_RVA = axRva(0x6cbde0, 0x6b8090);
inline const uintptr_t EX_VFT_RVA = axRva(0xead4f0, 0xe8fdd8);
inline const uintptr_t EX_RATES_RVA = axRva(0x117d918, 0x1157e20);
inline const uintptr_t EX_REG_RVA = axRva(0x117d910, 0x1157e18);
inline const uintptr_t EX_DEFQTY_RVA = axRva(0x67a6f0, 0x6669a0);
// ---- town\map.cpp ----
inline const uintptr_t TM_BLD_VFTABLE_RVA = axRva(0xeaa760, 0xe8d268);
// ---- town\party.cpp ----
inline const uintptr_t PTY_RL_SHOWSHEET_RVA = axRva(0x6a50b0, 0x691360);
                                                         // body re-read: rows +0xa0/+0xa8 by 0x40, DAT_1411769f0)
inline const uintptr_t PTY_PANEL_HIDE_RVA = axRva(0x7e9dc0, 0x7d5980);
inline const uintptr_t PTY_DROP_RVA = axRva(0x66f370, 0x65b620);
inline const uintptr_t PTY_CLEAR_RVA = axRva(0x66f120, 0x65b3d0);
inline const uintptr_t ROS_SETSTATE_RVA = axRva(0x5817c0, 0x56ff00);
inline const uintptr_t ROS_SORTBY_RVA = axRva(0x6ac480, 0x698730);
inline const uintptr_t ROS_LISTORDER_RVA = axRva(0x6a8220, 0x6944d0);
inline const uintptr_t ROS_SORT_LEVEL_VFT = axRva(0xeb19d8, 0xe942c8);
inline const uintptr_t ROS_SORT_STRESS_VFT = axRva(0xeb12f0, 0xe93be0);
inline const uintptr_t ROS_SORT_CLASS_VFT = axRva(0xeb1600, 0xe93f90);
inline const uintptr_t ROS_SORT_BUILDING_VFT = axRva(0xeb1d48, 0xe94638);
// ---- town\provision.cpp ----
inline const uintptr_t PROV_SELL_ONE_RVA = axRva(0x68b510, 0x6777c0);
// ---- town\trinkets.cpp ----
inline const uintptr_t RI_VFT_RVA = axRva(0xeb0690, 0xe92f80);
inline const uintptr_t RI_SELLDLG_RVA = axRva(0x69aeb0, 0x687160);
inline const uintptr_t RI_COLS_RVA = axRva(0x2b7cdcc, 0x2b559cc);
inline const uintptr_t RI_UNLOCK_KEY_RVA = axRva(0x2a5f8b8, 0x2a3c884);
