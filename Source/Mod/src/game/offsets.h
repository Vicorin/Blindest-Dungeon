// game/offsets.h — reverse-engineered RVAs, struct offsets and game-function typedefs.

#pragma once
#include <cstdint>

// ---- THE BINARY EVERY VALUE BELOW BELONGS TO ----
static const uint32_t GAME_PE_TIMESTAMP   = 0x6ae4c882;
static const uint32_t GAME_PE_SIZEOFIMAGE = 0x2fc1000;

// ---- THE DUAL TABLE: Steam AND DRM-free (GOG), one release, two binaries ----
static const uint32_t GAME_PE_TIMESTAMP_DRMFREE   = 0x6ae4cbcf;
static const uint32_t GAME_PE_SIZEOFIMAGE_DRMFREE = 0x2f98000;

// ---- THE THIRD COLUMN: the DRM-free build GOG actually ships ----
static const uint32_t GAME_PE_TIMESTAMP_DRMFREE_PREV   = 0x6ae4d1a0;
static const uint32_t GAME_PE_SIZEOFIMAGE_DRMFREE_PREV = 0x2f98000;

enum AxGameBuild { AX_BUILD_UNKNOWN = -1, AX_BUILD_STEAM = 0, AX_BUILD_DRMFREE = 1, AX_BUILD_DRMFREE_PREV = 2 };
AxGameBuild axGameBuild();
const char* axGameBuildName();
inline bool axIsDrmFree() {
    AxGameBuild b = axGameBuild();
    return b == AX_BUILD_DRMFREE || b == AX_BUILD_DRMFREE_PREV;
}
inline uintptr_t axRva(uintptr_t steam, uintptr_t drmfree, uintptr_t drmfreePrev) {
    switch (axGameBuild()) {
        case AX_BUILD_DRMFREE:      return drmfree;
        case AX_BUILD_DRMFREE_PREV: return drmfreePrev;
        default:                    return steam;
    }
}

// ---- Reverse-engineered offsets ----
inline const uintptr_t FOCUS_ID_RVA = axRva(0x1096f70, 0x1073f70, 0x1071f70);
inline const uintptr_t VEC_BEGIN_RVA = axRva(0x2c38c48, 0x2c11708, 0x2c12a58);
inline const uintptr_t VEC_END_RVA = axRva(0x2c38c50, 0x2c11710, 0x2c12a60);
static const uintptr_t ELEM_STRIDE      = 0x108;      // bytes per FocusElement
static const uintptr_t ELEM_ID_OFF      = 0x00;       // int64 id (matches FOCUS_ID)
static const uintptr_t ELEM_OWNER_OFF   = 0x08;       // ptr to owning widget
static const uintptr_t ELEM_POS_OFF       = 0x10;     // vec2 position (x,y floats)
static const uintptr_t ELEM_SIZE_OFF      = 0x18;     // vec2 size (w,h floats)
static const uintptr_t ELEM_FOCUSABLE_OFF = 0x24;     // byte: 1 = focusable
static const uintptr_t ELEM_SKIP_OFF      = 0x26;     // byte: nav skips if set

// ---- TextBoxWidget ----
inline const uintptr_t TBW_VFTABLE_RVA = axRva(0xe68328, 0xe4b0d0, 0xe47fb0);
static const uintptr_t TBW_TEXT_OFF     = 0x198;      // inline UTF-8 text buffer (<=512)
static const uintptr_t TBW_TEXT_PTR_OFF = 0x398;      // fallback string pointer
// ---- Widget-tree layout (the WidgetCell shape; SharedUI layout primitives) ----
static const uintptr_t TL_KIDS_BEG_OFF     = 0x150;     // layout widget+: vector<Cell*> begin
static const uintptr_t TL_KIDS_END_OFF     = 0x158;     // layout widget+: vector<Cell*> end
static const uintptr_t TL_CELL_CONTENT_OFF = 0x150;     // cell+: its single content widget
static const uintptr_t TL_ATTACH_BEG_OFF   = 0x90;      // any widget+: attached-children begin
static const uintptr_t TL_ATTACH_END_OFF   = 0x98;      // any widget+: attached-children end
inline const uintptr_t RDATA_BEGIN_RVA = axRva(0xc72080, 0xc57080, 0xc53030);
inline const uintptr_t RDATA_END_RVA = axRva(0x108c000, 0x1069000, 0x1067000);

// ---- Surface ROOT globals shared across modules ----
inline const uintptr_t MAP_ROOT_RVA = axRva(0x118e2c8, 0x1168658, 0x1168100);
inline const uintptr_t RES_CAMPAIGN_RVA = axRva(0x118e138, 0x11684c8, 0x1167f78);
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
inline const uintptr_t TM_ARENA_DLC_RVA = axRva(0x2a6d4ba, 0x2a45f7a, 0x2a472ca);
static const uintptr_t TM_CAMP_DISTRICTS_UNLOCKED = 0x11f0; // Campaign+: char, Cornerstones fired
static const uintptr_t TM_CAMP_DISTRICTS_ENABLED  = 0x11f1; // Campaign+: char, feature on this save
inline const uintptr_t TM_DDIS_OVERRIDE_RVA = axRva(0x2a6e70b, 0x2a471cb, 0x2a4851b);

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
inline const uintptr_t PROV_EVTREG_RVA = axRva(0x118e0c0, 0x1168450, 0x1167f00);
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

inline const uintptr_t RESOLVER_RVA = axRva(0x440e60, 0x42f040, 0x431eb0);
typedef void* (*ResolveFn)(void* out, const char* key);

// ---- Vtable-hook targets (patched by core/hooks.cpp; some offsets also read by their ----
inline const uintptr_t MENU_VFTABLE_RVA = axRva(0xee3f10, 0xec65b0, 0xec2d80);
static const int       MENU_DRAW_SLOT   = 23;
inline const uintptr_t TUT_VFTABLE_RVA = axRva(0xee5700, 0xec7dd8, 0xec45a8);
static const int       TUT_SHOW_SLOT   = 23;
inline const uintptr_t TUT_LAYOUT_RVA = axRva(0x2bfe5c8, 0x2bd7088, 0x2bd83d8);
static const uintptr_t TUT_LAYOUT_CLOSE_OFF = 0x24;    // close_button_offset, relative to base_pos
inline const uintptr_t JP_VFT_RVA = axRva(0xee37b0, 0xec5ec0, 0xec2690);
static const int       JP_SHOW_SLOT     = 23;
static const uintptr_t JP_STATE_OFF     = 0x58;      // int UI::Panel::Base lifecycle: 0 out, 1..4 up
static const uintptr_t JP_PAGE_OFF      = 0x410;     // uint: page index == the loc-key number
inline const uintptr_t LTO_VFTABLE_RVA = axRva(0xed3e28, 0xeb62b0, 0xeb2cd0);
static const int       LTO_RENDER_SLOT = 5;
inline const uintptr_t TE_VFTABLE_RVA = axRva(0xec2020, 0xea46e8, 0xea0ed8);
static const int       TE_SHOW_SLOT    = 23;
inline const uintptr_t PANEL_BANNER_VFTABLE_RVA = axRva(0xec92f0, 0xeaba00, 0xea81d0);
static const int       PANEL_UPDATE_SLOT        = 2;        // same vtable slot as Panel_Map's
inline const uintptr_t POPUP_PROC_RVA = axRva(0x777080, 0x7624f0, 0x7686f0);
inline const uintptr_t POPUP_CALLSITE_RVA = axRva(0x75c0bc, 0x74762c, 0x74d72c);

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
static const uint32_t  SDL_EVT_TEXTEDITING  = 0x302;  // SDL_TEXTEDITING (an IME composition in progress)
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
inline const uintptr_t INPUT_MODE_RVA = axRva(0x118d514, 0x11678c4, 0x116736c);

// ---- Building recruit displays: vftables + slot layout ----
inline const uintptr_t BLD_BHRD_VFT_RVA = axRva(0xec11e8, 0xea38b0, 0xea00b8);
inline const uintptr_t BLD_HRD_VFT_RVA = axRva(0xec0cb0, 0xea3378, 0xe9fb28);
inline const uintptr_t BLD_SHRD_VFT_RVA = axRva(0xec1098, 0xea3798, 0xe9ff48);
static const uintptr_t BLD_RCT_SLOTS_BEG  = 0x350;    // recruit display+: slot vector begin (was 0x348)
static const uintptr_t BLD_RCT_SLOTS_END  = 0x358;
static const uintptr_t BLD_RCT_SLOT_STRIDE= 0x10;
static const uintptr_t BLD_RCT_IFACE_OFF  = 0x158;    // slot widget+: -> slot interface (the same
static const uintptr_t BLD_RCT_IFACE_HERO = 0x28;     // iface+: Hero*  — shape as EmbarkParty's)

// ---- Hero / HeroClass / skill / quirk / equipment layout ----
static const uint32_t  INV_FOURCC_BASE    = 0x696e7620;    // "inv " — focus id = base + slotIndex
static const uintptr_t HERO_NAME_OFF      = 0x08;    // Hero+: inline C-string, <=0x40 ("hero_name")
static const uintptr_t HERO_TRINKET_SYSTEM_OFF = 0x1280;
static const uintptr_t ACTOR_HEROCLASS_OFF = 0x1320;
static const uintptr_t ACTOR_ID_OFF       = 0x137c;
inline const uintptr_t HERO_VFT = axRva(0xe8a288, 0xe6ca08, 0xe690d8);
inline const uintptr_t MONSTER_VFT = axRva(0xea9480, 0xe8bfe8, 0xe882c8);
static const float     CS_PERCENT_SCALE   = 100.0f;
static const uintptr_t HERO_WEAPON_LEVEL_OFF = 0x132c;
static const uintptr_t HERO_ARMOUR_LEVEL_OFF = 0x1330;
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
static const uintptr_t HERO_QUIRK_BEGIN_OFF  = 0x1380;
static const uintptr_t HERO_QUIRK_END_OFF    = 0x1388;
static const uintptr_t QUIRK_ENTRY_STRIDE    = 0x38;   // (was 0x30)
static const uintptr_t QUIRK_ENTRY_CLASS_OFF = 0x00;   // entry+: Quirk::Class const*
static const uintptr_t QUIRK_ID_OFF          = 0x00;   // Quirk::Class+: inline C-string id
inline const uintptr_t QUIRK_REGISTRY_RVA = axRva(0x118e108, 0x1168498, 0x1167f48);
static const uintptr_t QUIRK_REGISTRY_STRIDE = 0x220;
static const uintptr_t ACTOR_SEL_BEG_OFF  = 0x13c8;
static const uintptr_t ACTOR_SEL_END_OFF  = 0x13d0;
static const uintptr_t ACTOR_SLOTMAP_OFF  = 0x13e0;
static const uintptr_t ACTOR_SLOTMAP_END_OFF = 0x13e8;
static const uintptr_t HEROCLASS_SKILLVEC_OFF = 0xbd0;
static const uintptr_t HEROCLASS_SKILLVEC_END_OFF = 0xbd8; // HeroClass+: end of that outer vector
static const uintptr_t SKILL_STRIDE       = 0x510;
static const uintptr_t SKILL_ID_OFF       = 0x08;    // ActorCombatSkill+: inline C-string skill id
inline const uintptr_t RAID_SCREEN_RVA = axRva(0x118e570, 0x1168900, 0x1168308);
static const uintptr_t CS_PANEL_OPEN_OFF  = 0x58;
static const uintptr_t ACTOR_IS_MONSTER_OFF = 0x1306;
static const uintptr_t ACTOR_TRAIT_OFF    = 0x1250;
static const uintptr_t ACTOR_VIRTUE_OFF   = 0x1254;
static const uintptr_t HERO_RESOLVE_XP_OFF  = 0x1328;
inline const uintptr_t RESOLVE_TABLE_RVA = axRva(0x118e0b8, 0x1168448, 0x1167ef8);
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
static const uintptr_t SKILL_HP_RANGE_SET_OFF = 0x439;
static const uintptr_t SKILL_HP_RANGE_MIN_OFF = 0x43c;
static const uintptr_t SKILL_HP_RANGE_MAX_OFF = 0x440;
static const uintptr_t ACTOR_BATTLE_USES_MAP_OFF  = 0x1228;
static const uintptr_t ACTOR_BATTLE_USES_SIZE_OFF = 0x1230;
static const uintptr_t HEROCLASS_CAMPVEC_OFF     = 0x10f8;
static const uintptr_t HEROCLASS_CAMPVEC_END_OFF = 0x1100;
static const uintptr_t ACTOR_CAMP_KNOWN_OFF      = 0x13b0;
static const uintptr_t ACTOR_CAMP_KNOWN_END_OFF  = 0x13b8;
inline const uintptr_t CAMP_REGISTRY_RVA = axRva(0x118e0e8, 0x1168478, 0x1167f28);
static const uintptr_t CAMP_REGISTRY_STRIDE = 0x18;
static const uintptr_t CAMP_SKILL_HASH_OFF = 0x48;    // SkillClass+: hash of the id (h = h*0x35 + c)
inline const uintptr_t RI_CIRCUS_FLAG_RVA = axRva(0x2a6d4bb, 0x2a45f7b, 0x2a472cb);

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
static const uintptr_t ACTOR_STRESS_OFF      = 0x12fc;
inline const uintptr_t RI_TOGGLE_RVA = axRva(0x6801c0, 0x66bd10, 0x6718c0);
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
static const uintptr_t TILE_HDOOR_KIND_OFF      = 0x24; // tile+: int door kind — 2 = hidden door;
static const int       TILE_HDOOR_KIND_HIDDEN   = 2;
static const uintptr_t TILE_HDOOR_SEEN_OFF      = 0x44; // tile+: byte, the door has been FOUND
static const uintptr_t TILE_HDOOR_ACCESS_OFF    = 0x45; // tile+: byte, hidden_door_accessible (template)
inline const uintptr_t RD_INTERACT_HIDDEN_RVA = axRva(0x7745e0, 0x75fac0, 0x765c50);

inline const uintptr_t MAP_PROJ_SCALE_RVA = axRva(0x2be3920, 0x2bbc3e0, 0x2bbd730);
inline const uintptr_t MAP_PROJ_CONST_RVA = axRva(0xee625c, 0xec888c, 0xec513c);
inline const uintptr_t MAP_PROJ_PANX_RVA = axRva(0x2be3910, 0x2bbc3d0, 0x2bbd720);
inline const uintptr_t MAP_PROJ_PANY_RVA = axRva(0x2be3914, 0x2bbc3d4, 0x2bbd724);
inline const uintptr_t MAP_SCREEN_W_RVA = axRva(0xee6474, 0xec8a9c, 0xec533c);
inline const uintptr_t MAP_SCREEN_H_RVA = axRva(0xee6440, 0xec8a6c, 0xec530c);
                                                       // party-tile-index formula's offset constant (raidmap.cpp)
inline const uintptr_t MAP_PAN2_X_RVA = axRva(0x2be3918, 0x2bbc3d8, 0x2bbd728);
inline const uintptr_t MAP_PAN2_Y_RVA = axRva(0x2be391c, 0x2bbc3dc, 0x2bbd72c);
inline const uintptr_t MAP_MS_OFFX = axRva(0x2c3a250, 0x2c12d08, 0x2c14060), MAP_MS_OFFY = axRva(0x2c3a254, 0x2c12d0c, 0x2c14064);
inline const uintptr_t MAP_MS_DIVX = axRva(0x2c3a258, 0x2c12d10, 0x2c14068), MAP_MS_DIVY = axRva(0x2c3a25c, 0x2c12d14, 0x2c1406c);
inline const uintptr_t MAP_MS_NUM = axRva(0xee62c0, 0xec88f0, 0xec51a0);
inline const uintptr_t MAP_MS_BNDX = axRva(0x2c3a260, 0x2c12d18, 0x2c14070), MAP_MS_BNDY = axRva(0x2c3a264, 0x2c12d1c, 0x2c14074);
inline const uintptr_t FOCUSED_WIDGET_RVA = axRva(0x1096f68, 0x1073f68, 0x1071f68);
inline const uintptr_t MOVE_FN_RVA = axRva(0x770080, 0x75b560, 0x7616f0);
inline const uintptr_t MOVE_GATE_RVA = axRva(0x42c290, 0x41a3c0, 0x41d2e0);
inline const uintptr_t MOVE_MGR_RVA = axRva(0x118e570, 0x1168900, 0x1168308);
inline const uintptr_t MAP_ZOOM2_RVA = axRva(0x2be3924, 0x2bbc3e4, 0x2bbd734);

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
static const uintptr_t MONSTER_SKILL_STRIDE = 0x500;    // (was 0x4f8; 6ae4c882 Skill +8) (was 0x4e8)
static const uintptr_t MONSTER_SKILL_ID_OFF = 0x08;     // inline char[0x40]
static const char* const MT_SKILL_KEY_FMT   = "str_monster_skill_%s";
// ---- THE SEEN-SKILLS STORE ----
inline const uintptr_t MONSTER_SKILL_SEEN_MAP_RVA = axRva(0x118e160, 0x11684f0, 0x1167f98);
static const uintptr_t ACTOR_CUR_HP_OFF      = 0x10a8;
static const uintptr_t ACTOR_RANK_MASK_OFF   = 0x1088;
static const uintptr_t ACTOR_SCREEN_X_OFF    = 0x48;   // float: WORLD x (camera-relative), both sides

static const uintptr_t ACTOR_GUID_VFT_OFF    = 0x28;   // vtable slot: uint32 guid
static const int       ACTOR_BUFF_STRIDE     = 0x418;
static const uintptr_t ACTOR_BUFF_RECORD_OFF = 0x08;   // the Buff the describer takes (was 0x04)
static const int       CAMP_BUFF_REC_SZ      = 0x410;
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
inline const uintptr_t PIT_ACTIVATE_HERO_RVA = axRva(0x5b1ad0, 0x59f4b0, 0x5a3070);
inline const uintptr_t PIT_CAN_ACTIVATE_RVA = axRva(0x5b4370, 0x5a1ca0, 0x5a5910);
inline const uintptr_t PIT_SELECT_HERO_RVA = axRva(0x5b1e90, 0x59f860, 0x5a3430);
static const uintptr_t BATTLE_ACTIVATED_OFF  = 0x1e9;     // Battle+: byte, a hero is activated
static const int BS_BEFORE_TURN_START = 0x11;
#define PIT_SELECT_HERO_KEY "str_ui_select_a_hero"
#define PIT_HOLD_BANNER_KEY "hold_to_activate_hero"
// ---- TELLING THE OPPONENT WHAT YOUR HERO DID ----
inline const uintptr_t MP_SEND_TURN_RVA = axRva(0x76c9f0, 0x757f00, 0x75e060);
static const uintptr_t ACTOR_CHOSEN_SKILL_OFF = 0x8e0;
static const uintptr_t ACTOR_TARGETS_OFF      = 0x8f0;   // {begin, end, cap}
inline const uintptr_t MP_VEC_ASSIGN_RVA = axRva(0x477120, 0x4652f0, 0x468170);
// ---- Battle::SetSingleTarget -- WHERE THE MISSING RNG DRAW LIVES ----
inline const uintptr_t SET_SINGLE_TARGET_RVA = axRva(0x5abae0, 0x599760, 0x59d080);
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
const uintptr_t ACTOR_CLASS_OFF    = 0x1320;
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
const uintptr_t ACTOR_MODE_ID_OFF  = 0x1248;
const uintptr_t HEROCLASS_MOVESKILL_OFF = 0xbe8;
const uint32_t  REST_FOURCC      = 0x72657374; // "rest" — the respite screen's finish button
inline const uintptr_t CAMP_BUFFREG_FLAG_RVA = axRva(0x2a6d4bb, 0x2a45f7b, 0x2a472cb);
inline const uintptr_t CAMP_BUFF_HASDESC_RVA = axRva(0x7c6290, 0x7b1710, 0x7b7910);
inline const uintptr_t EFF_BUFFLIST_BUILD_RVA = axRva(0x4c48d0, 0x4b2a40, 0x4b5920);
inline const uintptr_t EFF_BUFFLIST_FREE_RVA = axRva(0x4cc9a0, 0x4baab0, 0x4bd9f0);
inline const uintptr_t BUFF_RENDER_ONE_RVA = axRva(0x7c63b0, 0x7b1830, 0x7b7a30);
const uintptr_t EFF_BUFFLIST_STRIDE    = 0x410;
const int       EFF_BUFFLIST_MAX       = 32;       // sanity cap, same spirit as AB_EFF_MAX_PER_VEC
const uintptr_t EFF_SUMMON_BEG_OFF   = 0x228;
const uintptr_t EFF_SUMMON_END_OFF   = 0x230;      // (was 0x1b8; 6ae4c882 Effect +120) (was 0x1b0)
const uintptr_t EFF_SUMMON_STRIDE    = 0x14;       // the game steps piVar10 + 5 ints
const int       EFF_SUMMON_MAX       = 8;          // sanity cap on one effect's summon list
inline const uintptr_t MONCLASS_VEC_RVA = axRva(0x2c3c3c0, 0x2c14e30, 0x2c169d0);
inline const uintptr_t MONCLASS_VEC_ALT_RVA = axRva(0x2c3c3d8, 0x2c14e48, 0x2c169e8);
const uintptr_t MONCLASS_HASH_OFF    = 0xcc;       // int: the id hash the summon entry carries
const int       MONCLASS_MAX         = 4096;       // sanity cap on the registry walk
const uintptr_t SKILL_LEVEL_OFF    = 0x394;
const uintptr_t SKILL_VALIDMODES_OFF      = 0x3b8;
const uintptr_t SKILL_VALIDMODES_SIZE_OFF = 0x3c0;
const char* const SKILL_NAME_FMT   = "combat_skill_name_%s_%s";  // % (class id, skill id)
inline const uintptr_t MOVE_SKILL_RVA = axRva(0x5b9540, 0x5a6e70, 0x5aaae0);
inline const uintptr_t SKILL_PCT_MULT_RVA = axRva(0xee640c, 0xec8a38, 0xec52d8);
inline const uintptr_t EFF_ONE_RVA = axRva(0xee62c0, 0xec88f0, 0xec51a0);

// ---- Phase 3 round 24: what THE CAMP and dllmain.cpp both read ----
const uintptr_t ACTOR_CAMP_SEL_BEG_OFF    = 0x1398;
const uintptr_t ACTOR_CAMP_SEL_END_OFF    = 0x13a0;
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
const uintptr_t BUFFREC_DESCTIP_OFF   = 0x3c8;
const uintptr_t BUFFREC_SOURCE_OFF    = 0x54;   // int   "source" -- a source-TYPE enum, named live
const int       STAT_TYPE_STEALTH     = 0x2b;   // the one condition stat_type the combat text

// ---- dllmain.cpp ----
inline const uintptr_t POOL_PTR_RVA = axRva(0xd9edf0, 0x0, 0x0);
inline const uintptr_t KEYBIND_TABLE_RVA = axRva(0x2a80580, 0x2a59040, 0x2a5a390);
// ---- core\subtitles.cpp ----
inline const uintptr_t DARKEST_APP_RVA = axRva(0x2a6cf70, 0x2a45a30, 0x2a46d80);
// ---- dlc\butchers_circus\prizebox.cpp ----
inline const uintptr_t PB_VFT_RVA = axRva(0xeb85b0, 0xe9acb8, 0xe97488);
inline const uintptr_t PB_OPEN_BANNER_RVA = axRva(0x665f80, 0x651af0, 0x657680);
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
inline const uintptr_t RB_VFT_RVA = axRva(0xeb8860, 0xe9b008, 0xe97700);
static const uint32_t  RB_TAB_ELEM_ID = 0x727369;
// ---- dlc\butchers_circus\banner.cpp: THE BANNER DESIGNER ----
inline const uintptr_t BD_VFT_RVA = axRva(0xeb4768, 0xe96eb8, 0xe93400);
static const uint32_t  BD_ELEM_ID = 0x627369;
inline const uintptr_t BD_STEP_RVA = axRva(0x643140, 0x630af0, 0x634840);
inline const uintptr_t BD_PIECES_PER_ROW_RVA = axRva(0x2b8f35c, 0x2b67e1c, 0x2b6916c);
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
inline const uintptr_t DG_VFT_RVA = axRva(0xeb4e88, 0xe975d0, 0xe93d50);
inline const uintptr_t IW_VFT_RVA = axRva(0xeb50c8, 0x0, 0x0);
inline const uintptr_t IW_SEND_RVA = axRva(0x826980, 0x0, 0x0);
inline const uintptr_t MP_API_RVA = axRva(0x118d7c0, 0x0, 0x0);
inline const uintptr_t MP_ALT_GATE_RVA = axRva(0x2a6e1af, 0x2a46c6f, 0x2a47fbf);
inline const uintptr_t MP_DISCONNECT_RVA = axRva(0x363f40, 0x0, 0x0);
inline const uintptr_t MP_SENT_ERASE_RVA = axRva(0x365380, 0x0, 0x0);
inline const uintptr_t DG_EDIT_TOGGLE_RVA = axRva(0x6466f0, 0x6335a0, 0x637df0);
// ---- dlc\butchers_circus\ring.cpp ----
inline const uintptr_t RING_PANEL_VFT_RVA = axRva(0xec36a0, 0xea5cc0, 0xea24b0);
inline const uintptr_t RING_TOGGLE_RVA = axRva(0x655060, 0x640c90, 0x646760);
inline const uintptr_t RING_INSPECT_RVA = axRva(0x654f20, 0x640b50, 0x646620);
inline const uintptr_t RANKDISP_VFT_RVA = axRva(0xec3e98, 0xea6560, 0xea2dc0);
inline const uintptr_t RING_PUTDOWN_RVA = axRva(0x654940, 0x640570, 0x646040);
inline const uintptr_t UI_PLAY_SOUND_RVA = axRva(0x2312d0, 0x22f010, 0x2311e0);
inline const uintptr_t UI_DRAGDROP_MGR_RVA = axRva(0x118d518, 0x11678c8, 0x1167370);
inline const uintptr_t RING_MM_VFT_RVA = axRva(0xeb56f8, 0xe97d88, 0xe945c0);
inline const uintptr_t TOWNDISP_VFT_RVA = axRva(0xec37a8, 0xea5e70, 0xea25b8);
static const int       TD_PROGRESS_FWD_SLOT = 20;      // ...::ProgressForwardInternal
inline const uintptr_t TD_PROGRESS_FWD_RVA = axRva(0x6e2150, 0x6cd7e0, 0x6d37c0);
// ---- the DIRECT-CHALLENGE (lobby) ready handshake ----
inline const uintptr_t RING_CHALLENGE_ID_RVA = axRva(0x2a6d5e0, 0x2a460a0, 0x2a473f0);
                                                  // 0 = no direct challenge (solo / ranked queue)
inline const uintptr_t RING_CHALLENGE_STATE_RVA = axRva(0x2a6cf70, 0x2a45a30, 0x2a46d80);
                                                  // Same address as DARKEST_APP_RVA (+0x00).
inline const uintptr_t RING_NOT_STEAM_RVA = axRva(0x2a6e1af, 0x2a46c6f, 0x2a47fbf);
inline const uintptr_t STEAM_FRIENDS_CTX_RVA = axRva(0x10afe28, 0x0, 0x0);
// ---- dlc\color_of_madness\wave.cpp ----
inline const uintptr_t COM_WAVE_QID_RVA = axRva(0x2628598, 0x2602ef8, 0x2602b88);
inline const uintptr_t COM_TIER_KEY_TAB_RVA = axRva(0xdf6130, 0xdda818, 0xdd5f90);
inline const uintptr_t COM_THRESHOLD_FN_RVA = axRva(0x50d180, 0x4fb1b0, 0x4fe1d0);
inline const uintptr_t COM_CAN_ADVANCE_FN_RVA = axRva(0x50d010, 0x4fb040, 0x4fe060);
// ---- frontend\dialog.cpp ----
inline const uintptr_t CD_SYSTEM_PTR_RVA = axRva(0x118e5d0, 0x1168968, 0x11683e8);
// ---- frontend\display.cpp ----
inline const uintptr_t FE_DISPLAY_PTR_RVA = axRva(0x118e030, 0x11683c0, 0x1167e80);
inline const uintptr_t FE_NEWGAME_RVA = axRva(0x41b330, 0x40a9c0, 0x40d920);
inline const uintptr_t FE_DELETE_RVA = axRva(0x413d70, 0x403570, 0x406520);
inline const uintptr_t FE_CONTENT_REG_RVA = axRva(0x118dfe8, 0x1168378, 0x1167e38);
// ---- frontend\loading.cpp ----
inline const uintptr_t LS_PTR_RVA = axRva(0x118e5e8, 0x1168980, 0x11683f8);
// ---- frontend\naming.cpp ----
inline const uintptr_t COMMIT_NAME_RVA = axRva(0x2a6e1c0, 0x2a46c80, 0x2a47fd0);
// ---- frontend\options.cpp ----
inline const uintptr_t OPT_DESC_ARR_RVA = axRva(0x2a73570, 0x2a4c030, 0x2a4d380);
inline const uintptr_t OPT_CAT_ARR_RVA = axRva(0x2a733a0, 0x2a4be60, 0x2a4d1b0);
inline const uintptr_t OPT_VALARR_PTR_RVA = axRva(0x118e008, 0x1168398, 0x1167e58);
inline const uintptr_t OPT_LANG_BEGIN_RVA = axRva(0x2a993e0, 0x2a71ea0, 0x2a731f0);
inline const uintptr_t OPT_LANG_END_RVA = axRva(0x2a993e8, 0x2a71ea8, 0x2a731f8);
inline const uintptr_t OPT_LANG_CUR_RVA = axRva(0x2a99450, 0x2a71f10, 0x2a73260);
static const int       OPT_DESC_COUNT    = 48;
inline const uintptr_t OPT_ROWVIS_FN_RVA = axRva(0x8101b0, 0x7fb6f0, 0x801830);
inline const uintptr_t OPT_TOGGLE_CB_RVA = axRva(0x813560, 0x7feaa0, 0x804be0);
// ---- frontend\sharedui.cpp ----
inline const uintptr_t GL_SINGLETON_RVA = axRva(0x118e5c8, 0x1168960, 0x11683e0);
inline const uintptr_t GL_VFT_RVA = axRva(0xed7d78, 0xeba448, 0xeb6c18);
inline const uintptr_t HP_SINGLETON_RVA = axRva(0x118e5c0, 0x1168958, 0x11683d8);
inline const uintptr_t HP_VFT_RVA = axRva(0xed7af0, 0xeba1c0, 0xeb69c8);
inline const uintptr_t CP_SINGLETON_RVA = axRva(0x118e5b0, 0x1168950, 0x11683d0);
inline const uintptr_t CP_VFT_RVA = axRva(0xed7910, 0xeb9fd8, 0xeb67b0);
inline const uintptr_t JP_SYSCLASS_RVA = axRva(0x118e118, 0x11684a8, 0x1167f58);
// ---- frontend\credits.cpp ----
inline const uintptr_t CR_VFT_RVA = axRva(0xee2eb0, 0xec55c0, 0xec1d88);
static const int       CR_SHOW_SLOT = 23;
// ---- input\synth.cpp ----
inline const uintptr_t FE_CURSOR_X_RVA = axRva(0x118d528, 0x11678d8, 0x1167380);
inline const uintptr_t FE_CURSOR_Y_RVA = axRva(0x118d52c, 0x11678dc, 0x1167384);
inline const uintptr_t CTRL_CONNECTED_RVA = axRva(0x1190a3d, 0x11ece85, 0x116a6b9);
inline const uintptr_t INPUT_GATE_RVA = axRva(0x118d510, 0x11678c0, 0x1167368);
// ---- THE GAMEPAD ACTION TABLE ----
inline const uintptr_t PAD_ACTION_TABLE_RVA = axRva(0x2622f90, 0x25fd320, 0x25fcda0);
inline const uintptr_t MOUSE_SUPPRESS_RVA = axRva(0x118525a, 0x115f60a, 0x115f0ba);
inline const uintptr_t MOUSE_SUPPRESS_B_RVA = axRva(0x118525b, 0x115f60b, 0x115f0bb);
inline const uintptr_t MOUSE_INWINDOW_RVA = axRva(0x118525d, 0x115f60d, 0x115f0bd);
inline const uintptr_t INPUT_ENABLE_VEC_RVA = axRva(0x2c39540, 0x2c12000, 0x2c13350);
inline const uintptr_t INPUT_ENABLE_SIZE_RVA = axRva(0x2c39558, 0x2c12018, 0x2c13368);
inline const uintptr_t VP_OFF_X_RVA = axRva(0x2c3a250, 0x2c12d08, 0x2c14060);
inline const uintptr_t VP_OFF_Y_RVA = axRva(0x2c3a254, 0x2c12d0c, 0x2c14064);
inline const uintptr_t VP_DEN_X_RVA = axRva(0x2c3a258, 0x2c12d10, 0x2c14068);
inline const uintptr_t VP_DEN_Y_RVA = axRva(0x2c3a25c, 0x2c12d14, 0x2c1406c);
inline const uintptr_t VP_BOUND_X_RVA = axRva(0x2c3a260, 0x2c12d18, 0x2c14070);
inline const uintptr_t VP_BOUND_Y_RVA = axRva(0x2c3a264, 0x2c12d1c, 0x2c14074);
inline const uintptr_t VP_NUM_RVA = axRva(0xee62c0, 0xec88f0, 0xec51a0);
// ---- raid\actionbar.cpp ----
inline const uintptr_t TP_CUTOFF_MISS_RVA = axRva(0x2add2f8, 0x2ab5db8, 0x2ab7108);
inline const uintptr_t TP_CUTOFF_HIT_RVA = axRva(0x2add2fc, 0x2ab5dbc, 0x2ab710c);
inline const uintptr_t TP_DEATHBLOW_RVA = axRva(0x2add5a0, 0x2ab6060, 0x2ab73b0);
inline const uintptr_t BUFF_UPKEEP_RVA = axRva(0x480540, 0x46e710, 0x471590);
inline const uintptr_t TARGET_VALID_RVA = axRva(0x489a60, 0x477c30, 0x47aab0);
inline const uintptr_t TARGET_ANY_RVA = axRva(0x4898b0, 0x477a80, 0x47a900);
inline const uintptr_t SKILL_USE_GATES_RVA = axRva(0x5b23d0, 0x59fda0, 0x5a3970);
inline const uintptr_t DO_SKILL_RVA = axRva(0x5b9b50, 0x5a7480, 0x5ab0f0);
inline const uintptr_t EFF_PCT_D_RVA = axRva(0xee6370, 0xec89a0, 0xec5250);
inline const uintptr_t TOGGLE_CHARSHEET_RVA = axRva(0x42c1a0, 0x41a2d0, 0x41d1f0);
// ---- raid\camp.cpp ----
inline const uintptr_t CAMP_PERFORM_RVA = axRva(0x5c7420, 0x5b4d50, 0x5b89c0);
inline const uintptr_t MEAL_TABLE_RVA = axRva(0x2add2c8, 0x2ab5d88, 0x2ab70d8);
inline const uintptr_t MEAL_COUNT_RVA = axRva(0x2add2d0, 0x2ab5d90, 0x2ab70e0);
inline const uintptr_t CAMP_CAN_START_RVA = axRva(0x771d90, 0x75d270, 0x763400);
// ---- raid\combattext.cpp ----
inline const uintptr_t POPUP_TYPE_TABLE_RVA = axRva(0x2be83f0, 0x2bc0eb0, 0x2bc2200);
// ---- raid\dungeonview.cpp ----
inline const uintptr_t RD_INTERACT_TRAP_RVA = axRva(0x774410, 0x75f8f0, 0x765a80);
inline const uintptr_t RD_INTERACT_PROP_RVA = axRva(0x7747c0, 0x75fca0, 0x765e30);
inline const uintptr_t TRAP_SCOUT_BONUS_RVA = axRva(0x2add5f0, 0x2ab60b0, 0x2ab7400);
inline const uintptr_t TRAP_DIFF_BASE_BEG_RVA = axRva(0x2add868, 0x2ab6328, 0x2ab7678);
inline const uintptr_t TRAP_DIFF_BASE_END_RVA = axRva(0x2add870, 0x2ab6330, 0x2ab7680);
inline const uintptr_t REACH_BOX_WIDTH_RVA = axRva(0x2add5b8, 0x2ab6078, 0x2ab73c8);
inline const uintptr_t REACH_BOX_HEIGHT_RVA = axRva(0x2add5bc, 0x2ab607c, 0x2ab73cc);
// ---- raid\eventscroll.cpp ----
inline const uintptr_t OE_ACT_INVESTIGATE = axRva(0x714360, 0x6ff8a0, 0x7059d0);
inline const uintptr_t OE_ACT_CURIO_PASS = axRva(0x714270, 0x6ff7b0, 0x7058e0);
inline const uintptr_t OE_ACT_CLEAR = axRva(0x714770, 0x6ffcb0, 0x705de0);
inline const uintptr_t OE_ACT_OBST_PASS = axRva(0x714540, 0x6ffa80, 0x705bb0);
inline const uintptr_t OE_ACT_ANCESTOR = axRva(0x714500, 0x6ffa40, 0x705b70);
inline const uintptr_t OE_ACT_EAT = axRva(0x713bf0, 0x6ff130, 0x705260);
inline const uintptr_t OE_ACT_STARVE = axRva(0x713b80, 0x6ff0c0, 0x7051f0);
inline const uintptr_t HUNGER_ROUND_RVA = axRva(0xee62c0, 0xec88f0, 0xec51a0);
inline const uintptr_t FOOD_TYPEHASH_RVA = axRva(0x2a87b08, 0x2a61f58, 0x2a61e98);
inline const uintptr_t OE_USE_ITEM_RVA = axRva(0x738480, 0x7239c0, 0x729af0);
inline const uintptr_t ITEM_ASSIGN_RVA = axRva(0x3f5fc0, 0x3e5a10, 0x3e8ed0);
inline const uintptr_t ITEM_CLEAR_RVA = axRva(0x5de910, 0x5cc240, 0x5cfeb0);
inline const uintptr_t OE_INTERACT_LOOKUP_RVA = axRva(0x4b12e0, 0x49f450, 0x4a2330);
inline const uintptr_t OE_OBSTACLE_CLEAR_BODY_RVA = axRva(0x70ff50, 0x6fb490, 0x7015c0);
inline const uintptr_t CURIO_TRACKER_RVA = axRva(0x118e150, 0x11684e0, 0x1167f88);
inline const uintptr_t CURIO_TRACKER_QUERY_RVA = axRva(0x560ea0, 0x54edf0, 0x551ef0);
inline const uintptr_t CURIO_TRACKER_GATE_RVA = axRva(0x428fe0, 0x417110, 0x41a030);
inline const uintptr_t QUEST_ITEM_TYPEHASH_RVA = axRva(0x2a89494, 0x2a638a4, 0x2a639b4);
// ---- raid\inventory.cpp ----
inline const uintptr_t INV_NOID_TYPEHASH_RVA = axRva(0x2a91148, 0x2a6b4f8, 0x2a6b4d8);
inline const uintptr_t DUNGEON_CTRL_RVA = axRva(0x118e058, 0x11683e8, 0x1167ea0);
inline const uintptr_t HERO_USE_ITEM_RVA = axRva(0x429750, 0x417880, 0x41a7a0);
inline const uintptr_t USE_ITEM_PICK_RVA = axRva(0x42ad80, 0x418eb0, 0x41bdd0);
inline const uintptr_t INV_DISCARD_RVA = axRva(0x736cb0, 0x7221f0, 0x728320);
inline const uintptr_t INV_POUR_RVA = axRva(0x5e1460, 0x5ced90, 0x5d2a00);
inline const uintptr_t INV_SETIDENT_RVA = axRva(0x5df170, 0x5ccaa0, 0x5d0710);
// ---- raid\light.cpp ----
inline const uintptr_t TORCH_TABLE_RVA = axRva(0x2add520, 0x2ab5fe0, 0x2ab7330);
inline const uintptr_t TORCH_TITLE_FMT_RVA = axRva(0x10ab31c, 0x108810c, 0x108631c);
// ---- raid\loot.cpp ----
inline const uintptr_t LOOT_TAKE_ONE_RVA = axRva(0x7171d0, 0x702710, 0x708840);
// ---- raid\quest.cpp ----
inline const uintptr_t QT_GOALDESC_RVA = axRva(0x815e90, 0x8013d0, 0x807480);
inline const uintptr_t QT_LOGVEC_RVA = axRva(0x118e0b0, 0x1168440, 0x1167ef0);
static const uintptr_t QT_LOGVEC_BEG       = 0x48;      // owner+: definition vector begin
static const uintptr_t QT_LOGVEC_END       = 0x50;      // owner+: definition vector end
static const uintptr_t QT_LOG_STRIDE       = 0x780;     // (was 0x730)
static const uintptr_t QT_LOG_QUESTID_OFF  = 0x40;      // def+0x40 == Quest+0x40 identifies the entry
static const int       QT_LOG_MAX          = 256;       // sanity cap on the definition walk
inline const uintptr_t QT_RETREAT_CONFIRM_RVA = axRva(0x74c080, 0x7375f0, 0x73d6f0);
inline const uintptr_t QT_RETREAT_CAP4_RVA = axRva(0x10ab4a0, 0x1088290, 0x10864a0);
inline const uintptr_t QT_NOABANDON_QID_RVA = axRva(0x2a71238, 0x2a4e214, 0x2a4b6b8);
inline const uintptr_t QT_WAVE_QID_RVA = axRva(0x2628598, 0x2602ef8, 0x2602b88);
inline const uintptr_t QT_PANEL_VFTABLE_RVA = axRva(0xecc678, 0xeaf390, 0xeab408);
// ---- raid\raidmap.cpp ----
inline const uintptr_t PARTY_TILE_K_OFFSET_RVA = axRva(0xee6440, 0xec8a6c, 0xec530c);
inline const uintptr_t PARTY_TILE_K_SCALE_RVA = axRva(0xee61f4, 0xec8824, 0xec50d4);
inline const uintptr_t KEY_IS_DOWN_RVA = axRva(0x28b7c0, 0x289050, 0x28b6d0);
inline const uintptr_t KEY_STATE_ARRAY_RVA = axRva(0x2c39670, 0x2c12130, 0x2c13480);
// ---- raid\results.cpp ----
inline const uintptr_t RR_DISPLAY_RVA = axRva(0x118e578, 0x1168910, 0x11683a0);
                                                       // app-state 0xb, NULLED leaving — the gate
inline const uintptr_t RR_OUTCOME_KEY_RVA = axRva(0x2bf2050, 0x2bcab10, 0x2bcbe60);
inline const uintptr_t RR_PROFILE_GET_RVA = axRva(0x556520, 0x544470, 0x547570);
inline const uintptr_t RR_GOLD_TYPEHASH_RVA = axRva(0x2a8c7c8, 0x2a66ec8, 0x2a66cf8);
// ---- sheet\charsheet.cpp ----
inline const uintptr_t CS_TRK_UNEQUIP_RVA = axRva(0x8050e0, 0x7f0650, 0x7f6760);
inline const uintptr_t CS_COMSKILL_TOGGLE_RVA = axRva(0x5d4950, 0x5c2280, 0x5c5ef0);
inline const uintptr_t CS_CAMPSKILL_TOGGLE_RVA = axRva(0x5d3dd0, 0x5c1700, 0x5c5370);
// ---- sheet\composers.cpp ----
inline const uintptr_t CS_RESIST_TABLE_RVA = axRva(0x2ae5330, 0x2abddf0, 0x2abf140);
inline const uintptr_t CS_RESIST_VALUE_RVA = axRva(0x47d440, 0x46b610, 0x46e490);
inline const uintptr_t CS_QUIRK_DESC_RVA = axRva(0x817f20, 0x803460, 0x809510);
inline const uintptr_t CAMP_ET_SKIP_RVA = axRva(0x2a903a8, 0x2a6a6e8, 0x2a6a738);
inline const uintptr_t CAMP_ET_PCT_RVA[2]  = { axRva(0x2a90588, 0x2a6ab08, 0x2a6aa78), axRva(0x2a90534, 0x2a6aaa4, 0x2a6aa14) };
inline const uintptr_t CAMP_ET_RAW_RVA[3]  = { axRva(0x2a90448, 0x2a6a7a8, 0x2a6a928), axRva(0x2a903f4, 0x2a6a744, 0x2a6a794), axRva(0x2a90494, 0x2a6a804, 0x2a6a974) };
inline const uintptr_t CAMP_ET_PLAIN_RVA[7] = { axRva(0x2a90348, 0x2a6a698, 0x2a6a6d8), axRva(0x2a90538, 0x2a6aaa8, 0x2a6aa18), axRva(0x2a90444, 0x2a6a7a4, 0x2a6a924), axRva(0x2a902a8, 0x2a6a638, 0x2a6a678),
                                                axRva(0x2a90344, 0x2a6a694, 0x2a6a6d4), axRva(0x2a903a4, 0x2a6a6e4, 0x2a6a734), axRva(0x2a904e4, 0x2a6a8e4, 0x2a6a9c4) }; // format, verbatim
inline const uintptr_t CAMP_ET_SUFFIX_RVA = axRva(0x2a90344, 0x2a6a694, 0x2a6a6d4);
inline const uintptr_t CAMP_ET_ITEM_RVA = axRva(0x2a90498, 0x2a6a808, 0x2a6a978);
inline const uintptr_t CAMP_ET_BUFF_RVA = axRva(0x2a904e8, 0x2a6a8e8, 0x2a6a9c8);
inline const uintptr_t CAMP_BUFFREG_RVA = axRva(0x118e0e0, 0x1168470, 0x1167f20);
inline const uintptr_t CAMP_BUFF_DESC_RVA = axRva(0x7c68d0, 0x7b1d50, 0x7b7f50);
inline const uintptr_t BUFF_SRCTYPE_TABLE_RVA = axRva(0x2ab7790, 0x2a90250, 0x2a915a0);
inline const uintptr_t BUFF_STATTYPE_TABLE_RVA = axRva(0x2ab8520, 0x2a90fe0, 0x2a92330);
inline const uintptr_t BUFF_DURTYPE_TABLE_RVA = axRva(0x2ab81b0, 0x2a90c70, 0x2a91fc0);
inline const uintptr_t SCOUT_BASE_RVA = axRva(0x2add594, 0x2ab6054, 0x2ab73a4);
inline const uintptr_t BUFF_RULE_GATE_RVA = axRva(0x479cd0, 0x467ea0, 0x46ad20);
inline const uintptr_t TRAIT_REG_RVA = axRva(0x118e128, 0x11684b8, 0x1167f68);
// ---- town\buildings.cpp ----
inline const uintptr_t BLD_URD_VFT_RVA = axRva(0xec2400, 0xea4a90, 0xea1280);
inline const uintptr_t BLD_REG_BEGIN_RVA = axRva(0x2c3baf0, 0x2c14560, 0x2c16100);
inline const uintptr_t BLD_REG_END_RVA = axRva(0x2c3baf8, 0x2c14568, 0x2c16108);
inline const uintptr_t BLD_TSD_VFT_RVA = axRva(0xebd620, 0xe9fc90, 0xe9c488);
inline const uintptr_t BLD_CTSD_VFT_RVA = axRva(0xebd898, 0xe9ff78, 0xe9c770);
inline const uintptr_t BLD_SWITCH_FN_RVA = axRva(0x692140, 0x67dc90, 0x683840);
inline const uintptr_t BLD_CTSD_SHOWROW_RVA = axRva(0x693e30, 0x67f980, 0x685530);
inline const uintptr_t BLD_SHARD_PRICE_KEY_RVA = axRva(0x2a7b4c8, 0x2a56334, 0x2a55948);
inline const uintptr_t BLD_PRICE_FN_RVA = axRva(0x59b540, 0x589480, 0x58c590);
inline const uintptr_t BLD_PRICE_KEY_RVA = axRva(0x2a7b404, 0x2a56228, 0x2a55884);
inline const uintptr_t BLD_TRINKDB_PTR_RVA = axRva(0x118e0f8, 0x1168488, 0x1167f38);
inline const uintptr_t BLD_TRECDB_BEG_RVA = axRva(0x2c3c570, 0x2c14fe0, 0x2c16b80);
inline const uintptr_t BLD_TRECDB_END_RVA = axRva(0x2c3c578, 0x2c14fe8, 0x2c16b88);
inline const uintptr_t TRK_SETMAP_HEAD_RVA = axRva(0x2adf458, 0x2ab7f18, 0x2ab9268);
inline const uintptr_t BLD_TRKGRP_TABLE_RVA = axRva(0x2adf490, 0x2ab7f50, 0x2ab92a0);
inline const uintptr_t BLD_CLASSDB_RVA = axRva(0x2c3c308, 0x2c14d78, 0x2c16918);
inline const uintptr_t BLD_CLASSDB_ALT_RVA = axRva(0x2c3c2e8, 0x2c14d58, 0x2c168f8);
inline const uintptr_t BLD_RCT_INSPECT_RVA = axRva(0x6c5a90, 0x6b15e0, 0x6b7190);
inline const uintptr_t BLD_RCT_INSPECT_VFT = axRva(0xec0c08, 0xea3298, 0xe9faf0);
inline const uintptr_t BLD_STATUE_VFT_RVA = axRva(0xec1750, 0xea3e18, 0xea0608);
inline const uintptr_t ST_DISPATCH_RVA = axRva(0x6c9a70, 0x6b55c0, 0x6bb170);
inline const uintptr_t BLD_GRAVE_VFT_RVA = axRva(0xebb5a8, 0xe9dc10, 0xe9a418);
inline const uintptr_t RCT_COUNT_RVA = axRva(0x58b1f0, 0x579140, 0x57c240);
inline const uintptr_t RCT_MAX_RVA = axRva(0x58b2e0, 0x579230, 0x57c330);
inline const uintptr_t BLD_HSRD_VFT_RVA = axRva(0xebce48, 0xe9f4b0, 0xe9bcb0);
inline const uintptr_t ACT_SLOTLOCKED_RVA = axRva(0x5363a0, 0x5243a0, 0x5273f0);
inline const uintptr_t ACT_EVTLOCKED_RVA = axRva(0x536300, 0x524300, 0x527350);
inline const uintptr_t ACT_COSTSMONEY_RVA = axRva(0x536460, 0x524460, 0x5274b0);
inline const uintptr_t ACT_COSTBYID_RVA = axRva(0x5364f0, 0x5244f0, 0x527540);
inline const uintptr_t ACT_CANAFFORD_RVA = axRva(0x568710, 0x556660, 0x559760);
inline const uintptr_t ACT_COSTFREE_RVA = axRva(0x3a5eb0, 0x3998a0, 0x39ad70);
inline const uintptr_t ACT_CANHERO_RVA = axRva(0x535e50, 0x523e50, 0x526ea0);
inline const uintptr_t ACT_CONTAGION_LEVEL_RVA = axRva(0x63ea90, 0x62c440, 0x630190);
static const uintptr_t ACT_CONTAGION_TIP_OFF   = 0x2b0;     // ItemContainerDisplay+: the tooltip
inline const uintptr_t INF_SYS_RVA = axRva(0x118e0a0, 0x1168430, 0x1167ee0);
static const uintptr_t INF_GATE_OFF      = 0x30;      // sys+: char, the builder's first early-out
static const uintptr_t INF_LVLS_OFF      = 0x38;      // sys+: level-def vector begin (end at +8)
static const uintptr_t INF_ELEMS_OFF     = 0x50;      // sys+: sequence-element vector begin (end +8)
static const uintptr_t INF_LVL_STRIDE    = 0x78;
static const uintptr_t INF_ELEM_STRIDE   = 0x60;
                                                      //   show_ui char@+0x5c (the json field, 1:1)
static const uintptr_t INF_CAMP_CUR_OFF  = 0x17d4;    // Campaign+: current sequence-element id hash
inline const uintptr_t ACT_REQFREE_RVA = axRva(0x233de0, 0x231b20, 0x233cf0);
inline const uintptr_t ACT_REQ_NOTQUIRKS_VFT = axRva(0xe9c458, 0xe7ebd0, 0xe7b2e0);
                                                         //   NotHaveQuirks::vftable (RTTI, pass 62)
inline const uintptr_t ACT_PICK_RVA = axRva(0x690e60, 0x67c9b0, 0x682560);
inline const uintptr_t ACT_CONFIRM_RVA = axRva(0x68b080, 0x676bd0, 0x67c780);
inline const uintptr_t ACT_UNCOMMIT_RVA = axRva(0x68b580, 0x6770d0, 0x67cc80);
inline const uintptr_t BLD_QTAD_VFT_RVA = axRva(0xebe8c0, 0xea0f80, 0xe9d738);
inline const uintptr_t QT_CLICK_RVA = axRva(0x6a3910, 0x68f460, 0x695010);
inline const uintptr_t QT_ACTMULT_RVA = axRva(0x118e090, 0x1168420, 0x1167ed0);
inline const uintptr_t HA_BSMITH_VFT_RVA = axRva(0xeb8c70, 0xe9b200, 0xe97b10);
inline const uintptr_t HA_GUILD_VFT_RVA = axRva(0xebb8d8, 0xe9df40, 0xe9a680);
inline const uintptr_t HA_CAMPT_VFT_RVA = axRva(0xeb9ae0, 0xe9c1e8, 0xe98980);
inline const uintptr_t HA_PICK_RVA = axRva(0x687c20, 0x673770, 0x679320);
inline const uintptr_t HA_PICK_VFT_RVA = axRva(0xebc1e8, 0xe9e888, 0xe9b098);
inline const uintptr_t HA_PURCH_MAP_RVA = axRva(0x2b66160, 0x2b3ec20, 0x2b3ff70);
inline const uintptr_t HA_ALLBOUGHT_RVA = axRva(0x2b661b0, 0x2b3ec70, 0x2b3ffc0);
inline const uintptr_t HA_DISC1_RVA = axRva(0x2b66170, 0x2b3ec30, 0x2b3ff80);
inline const uintptr_t HA_DISC2_RVA = axRva(0x2b66180, 0x2b3ec40, 0x2b3ff90);
inline const uintptr_t HA_DISC3_RVA = axRva(0x2b66190, 0x2b3ec50, 0x2b3ffa0);
inline const uintptr_t BN_LAYOUT_MAP_RVA = axRva(0x2b8d4f8, 0x2b65fb8, 0x2b67308);
// ---- town\districts.cpp ----
inline const uintptr_t DST_VFT_RVA = axRva(0xeba368, 0xe9c518, 0xe99240);
inline const uintptr_t DST_LAYOUT_RVA = axRva(0x2c3e3c0, 0x2c16e30, 0x2c189d0);
// ---- town\embark.cpp ----
inline const uintptr_t EMB_MASTERY_RVA = axRva(0x56eb60, 0x55cab0, 0x55fbb0);
inline const uintptr_t EMB_NAMEID_RVA = axRva(0x817530, 0x802a70, 0x808b20);
inline const uintptr_t EMB_SPECIFICS_RVA = axRva(0x815cf0, 0x801230, 0x8072e0);
inline const uintptr_t EMB_FWDLABEL_RVA = axRva(0x6e1f00, 0x6cd590, 0x6d3570);
inline const uintptr_t EMB_ROAMMGR_RVA = axRva(0x118e2c0, 0x1168650, 0x11680f8);
// ---- town\estate.cpp ----
inline const uintptr_t TL_ROOT_RVA = axRva(0x118e360, 0x11686f0, 0x1168190);
inline const uintptr_t TL_CTRL_RVA = axRva(0x118e060, 0x11683f0, 0x1167ea8);
inline const uintptr_t TL_VFT_RVA = axRva(0xeb7c68, 0xe9a3a8, 0xe96b08);
inline const uintptr_t TL_SHOW_RVA = axRva(0x6d8b50, 0x6c46a0, 0x6ca250);
inline const uintptr_t TL_CLOSE_RVA = axRva(0x6d8630, 0x6c4180, 0x6c9d30);
// ---- town\events.cpp ----
inline const uintptr_t TE_INFO_RVA = axRva(0x63e1d0, 0x62bb80, 0x62f8d0);
inline const uintptr_t TE_HERO_AT_RVA = axRva(0x596ed0, 0x584e10, 0x587f20);
inline const uintptr_t TE_INSPECT_RVA = axRva(0x6d2b70, 0x6be6c0, 0x6c4270);
inline const uintptr_t TE_INSPECT_VFT = axRva(0xec2218, 0xea48a8, 0xea1098);
static const uintptr_t TE_DISP_SLOTS_BEG  = 0x120;      // TownEventDisplay+: vector<slot widget*> begin
static const uintptr_t TE_DISP_SLOTS_END  = 0x128;
inline const uintptr_t TE_FOCUSMGR_RVA = axRva(0x118dfd8, 0x1168370, 0x1167e28);
inline const uintptr_t TE_POPUPPOS_A_RVA = axRva(0x2771fb8, 0x0, 0x0);
inline const uintptr_t TE_POPUPPOS_B_RVA = axRva(0x2771fc0, 0x0, 0x0);
inline const uintptr_t TE_UISCALE_RVA = axRva(0xee625c, 0xec888c, 0xec513c);
// ---- town\exchange.cpp ----
inline const uintptr_t EX_TOGGLE_RVA = axRva(0x6d8200, 0x6c3d50, 0x6c9900);
inline const uintptr_t EX_VFT_RVA = axRva(0xebbda0, 0xe9e440, 0xe9ac18);
inline const uintptr_t EX_RATES_RVA = axRva(0x118e098, 0x1168428, 0x1167ed8);
inline const uintptr_t EX_REG_RVA = axRva(0x118e090, 0x1168420, 0x1167ed0);
inline const uintptr_t EX_DEFQTY_RVA = axRva(0x686a90, 0x6725e0, 0x678190);
// ---- town\map.cpp ----
inline const uintptr_t TM_BLD_VFTABLE_RVA = axRva(0xeb9240, 0xe9b8d8, 0xe980e0);
// ---- town\party.cpp ----
inline const uintptr_t PTY_RL_SHOWSHEET_RVA = axRva(0x6b1460, 0x69cfb0, 0x6a2b60);
                                                         // body re-read: rows +0xa0/+0xa8 by 0x40, DAT_1411769f0)
inline const uintptr_t PTY_PANEL_HIDE_RVA = axRva(0x7f6680, 0x7e1b10, 0x7e7d00);
inline const uintptr_t PTY_DROP_RVA = axRva(0x67b670, 0x6671c0, 0x66cd70);
inline const uintptr_t PTY_CLEAR_RVA = axRva(0x67b420, 0x666f70, 0x66cb20);
inline const uintptr_t ROS_SETSTATE_RVA = axRva(0x58b500, 0x579450, 0x57c550);
inline const uintptr_t ROS_SORTBY_RVA = axRva(0x6b8830, 0x6a4380, 0x6a9f30);
inline const uintptr_t ROS_LISTORDER_RVA = axRva(0x6b45d0, 0x6a0120, 0x6a5cd0);
inline const uintptr_t ROS_SORT_LEVEL_VFT = axRva(0xec0278, 0xea2938, 0xe9eee0);
inline const uintptr_t ROS_SORT_STRESS_VFT = axRva(0xebfb90, 0xea2288, 0xe9e7f8);
inline const uintptr_t ROS_SORT_CLASS_VFT = axRva(0xebfea0, 0xea2560, 0xe9eb98);
inline const uintptr_t ROS_SORT_BUILDING_VFT = axRva(0xec05e8, 0xea2c98, 0xe9f230);
// ---- town\provision.cpp ----
inline const uintptr_t PROV_SELL_ONE_RVA = axRva(0x6978b0, 0x683400, 0x688fb0);
// ---- town\trinkets.cpp ----
inline const uintptr_t RI_VFT_RVA = axRva(0xebef28, 0xea15e8, 0xe9ddd8);
inline const uintptr_t RI_SELLDLG_RVA = axRva(0x6a7260, 0x692db0, 0x698960);
inline const uintptr_t RI_COLS_RVA = axRva(0x2b8e8ec, 0x2b673ac, 0x2b686fc);
inline const uintptr_t RI_UNLOCK_KEY_RVA = axRva(0x2a6fb24, 0x2a49f04, 0x2a49eb4);
