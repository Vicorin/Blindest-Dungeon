// town/buildings.cpp -- the sixth TOWN slice

#include <cstdint>
#include <cstring>
#include <cstdio>
#include "internal.h"
// ---- TOWN: the BUILDING SCREENS (AX_BLDG) — round 1: skeleton + probe ----
static const uintptr_t BLD_VEC_BEG      = 0x5548;
static const uintptr_t BLD_VEC_END      = 0x5550;
static const uintptr_t BLD_ID_STR_OFF   = 0xa4;     // panel+: building id chars (map-pass layout)
static const uintptr_t BLD_ID_HASH_OFF  = 0xe4;     // panel+: gameHash(id) (state predicates)
static const uintptr_t BLD_MODE_OFF     = 0xf8;     // panel+: 0 = building screen, 1 = upgrades
static const uintptr_t BLD_TREE_VEC_BEG = 0x130;    // panel+: vector<UpgradeTreeDisplay*> begin
static const uintptr_t BLD_TREE_VEC_END = 0x138;    // panel+: vector end
static const uintptr_t BLD_TREE_ELEM_OFF = 0x228;   // tree+: its element-id field ('bdup' family)
static const uint32_t  BLD_ELEM_UPGRADE = 0x6e6d6669;
static const uint32_t  BLD_ELEM_BACK    = 0x6261636e;

static bool     g_bldActive = false;     // maintained by checkBuilding (edge-announced)
static char     g_bldId[28];             // the open building's data id ("abbey", ...)
static char     g_bldName[TM_NAME_MAX];  // its localized name (town_name_<id>)
static uint32_t g_bldMode = 0;           // last observed +0xf8, for edge announcements
static int      g_bldProbes = 0;
static DWORD    g_bldProbeDue = 0;
static char     g_bldProbeWhy[12];
static DWORD    g_bldWatchUntil = 0;     // U/Escape outcome watch; 0 = idle
static uint32_t g_bldWatchWant = 0;

bool axIsBld() { return g_bldActive; }

uintptr_t bldOpenPanel(uintptr_t root) {
    uintptr_t beg = 0, end = 0;
    if (!safeReadPtr(root + BLD_VEC_BEG, &beg) || !safeReadPtr(root + BLD_VEC_END, &end) ||
        !beg || end <= beg || (end - beg) % 8) return 0;
    uintptr_t count = (end - beg) / 8;
    if (count > 32) return 0;                        // the roster is ~11; a huge count = bad read
    for (uintptr_t i = 0; i < count; i++) {
        uintptr_t panel = 0;
        uint32_t  shown = 0;
        if (!safeReadPtr(beg + i * 8, &panel) || panel <= 0x10000) continue;
        if (safeReadU32(panel + TMB_SHOWN_OFF, &shown) && shown != 0) return panel;
    }
    return 0;
}

static int bldTrees(uintptr_t panel, uintptr_t* out, int max) {
    uintptr_t beg = 0, end = 0;
    if (!safeReadPtr(panel + BLD_TREE_VEC_BEG, &beg) || !safeReadPtr(panel + BLD_TREE_VEC_END, &end) ||
        !beg || end <= beg || (end - beg) % 8) return 0;
    int count = (int)((end - beg) / 8);
    if (count > 8) return 0;                         // 1..3 shipped; more = a bad read, trust nothing
    int n = 0;
    for (int i = 0; i < count && n < max; i++) {
        uintptr_t t = 0;
        if (safeReadPtr(beg + (uintptr_t)i * 8, &t) && t > 0x10000) out[n++] = t;
    }
    return n;
}

// ---- ROUND 2: the upgrade view's track rows ----
struct BldStepCost { const char* cur1; int amt1; const char* cur2; int amt2; };
struct BldTreeDef  { const char* id; int steps; BldStepCost cost[6]; };
static const BldTreeDef kBldTrees[] = {
    { "abbey.meditation", 6, { { "bust", 4, "crest", 5 }, { "bust", 8, "crest", 10 }, { "bust", 11, "crest", 14 }, { "bust", 15, "crest", 19 }, { "bust", 19, "crest", 24 }, { "bust", 23, "crest", 29 } } },
    { "abbey.prayer", 6, { { "bust", 4, "crest", 5 }, { "bust", 8, "crest", 10 }, { "bust", 11, "crest", 14 }, { "bust", 15, "crest", 19 }, { "bust", 19, "crest", 24 }, { "bust", 23, "crest", 29 } } },
    { "abbey.flagellation", 6, { { "bust", 4, "crest", 5 }, { "bust", 8, "crest", 10 }, { "bust", 11, "crest", 14 }, { "bust", 15, "crest", 19 }, { "bust", 19, "crest", 24 }, { "bust", 23, "crest", 29 } } },
    { "blacksmith.weapon", 4, { { "deed", 8, "crest", 8 }, { "deed", 20, "crest", 21 }, { "deed", 32, "crest", 35 }, { "deed", 44, "crest", 49 }, { "", 0, "", 0 }, { "", 0, "", 0 } } },
    { "blacksmith.armour", 4, { { "deed", 8, "crest", 8 }, { "deed", 20, "crest", 21 }, { "deed", 32, "crest", 35 }, { "deed", 44, "crest", 49 }, { "", 0, "", 0 }, { "", 0, "", 0 } } },
    { "blacksmith.cost", 5, { { "deed", 4, "crest", 4 }, { "deed", 9, "crest", 10 }, { "deed", 14, "crest", 15 }, { "deed", 18, "crest", 20 }, { "deed", 24, "crest", 26 }, { "", 0, "", 0 } } },
    { "camping_trainer.cost", 5, { { "crest", 15, "", 0 }, { "crest", 35, "", 0 }, { "crest", 54, "", 0 }, { "crest", 72, "", 0 }, { "crest", 92, "", 0 }, { "", 0, "", 0 } } },
    { "guild.skill_levels", 4, { { "portrait", 6, "crest", 14 }, { "portrait", 15, "crest", 38 }, { "portrait", 24, "crest", 60 }, { "portrait", 33, "crest", 84 }, { "", 0, "", 0 }, { "", 0, "", 0 } } },
    { "guild.cost", 5, { { "portrait", 2, "crest", 6 }, { "portrait", 5, "crest", 14 }, { "portrait", 8, "crest", 21 }, { "portrait", 11, "crest", 29 }, { "portrait", 14, "crest", 36 }, { "", 0, "", 0 } } },
    { "nomad_wagon.numitems", 4, { { "crest", 10, "", 0 }, { "crest", 26, "", 0 }, { "crest", 42, "", 0 }, { "crest", 58, "", 0 }, { "", 0, "", 0 }, { "", 0, "", 0 } } },
    { "nomad_wagon.cost", 5, { { "crest", 8, "", 0 }, { "crest", 18, "", 0 }, { "crest", 28, "", 0 }, { "crest", 38, "", 0 }, { "crest", 46, "", 0 }, { "", 0, "", 0 } } },
    { "sanitarium.cost", 5, { { "bust", 3, "crest", 3 }, { "bust", 8, "crest", 5 }, { "bust", 12, "crest", 8 }, { "bust", 16, "crest", 10 }, { "bust", 21, "crest", 13 }, { "", 0, "", 0 } } },
    { "sanitarium.disease_quirk_cost", 5, { { "bust", 3, "crest", 3 }, { "bust", 8, "crest", 5 }, { "bust", 12, "crest", 8 }, { "bust", 16, "crest", 10 }, { "bust", 21, "crest", 13 }, { "", 0, "", 0 } } },
    { "sanitarium.slots", 4, { { "bust", 10, "crest", 6 }, { "bust", 20, "crest", 13 }, { "bust", 30, "crest", 19 }, { "bust", 60, "crest", 38 }, { "", 0, "", 0 }, { "", 0, "", 0 } } },
    { "stage_coach.numrecruits", 5, { { "deed", 3, "crest", 4 }, { "deed", 8, "crest", 10 }, { "deed", 16, "crest", 15 }, { "deed", 20, "crest", 20 }, { "deed", 25, "crest", 26 }, { "", 0, "", 0 } } },
    { "stage_coach.rostersize", 5, { { "deed", 3, "crest", 4 }, { "deed", 8, "crest", 10 }, { "deed", 16, "crest", 15 }, { "deed", 20, "crest", 20 }, { "deed", 25, "crest", 26 }, { "", 0, "", 0 } } },
    { "stage_coach.upgraded_recruits", 3, { { "bust", 9, "crest", 12 }, { "bust", 12, "crest", 16 }, { "bust", 15, "crest", 20 }, { "", 0, "", 0 }, { "", 0, "", 0 }, { "", 0, "", 0 } } },
    { "tavern.bar", 6, { { "portrait", 2, "crest", 5 }, { "portrait", 4, "crest", 10 }, { "portrait", 6, "crest", 14 }, { "portrait", 8, "crest", 19 }, { "portrait", 10, "crest", 24 }, { "portrait", 11, "crest", 29 } } },
    { "tavern.gambling", 6, { { "portrait", 2, "crest", 5 }, { "portrait", 4, "crest", 10 }, { "portrait", 6, "crest", 14 }, { "portrait", 8, "crest", 19 }, { "portrait", 10, "crest", 24 }, { "portrait", 11, "crest", 29 } } },
    { "tavern.brothel", 6, { { "portrait", 2, "crest", 5 }, { "portrait", 4, "crest", 10 }, { "portrait", 6, "crest", 14 }, { "portrait", 8, "crest", 19 }, { "portrait", 10, "crest", 24 }, { "portrait", 11, "crest", 29 } } },
};
static const int kBldTreeCount = (int)(sizeof kBldTrees / sizeof kBldTrees[0]);
static uint32_t g_bldTreeHashes[sizeof kBldTrees / sizeof kBldTrees[0]];   // resHash(id), lazy
static bool     g_bldTreeHashed = false;

struct BldTreeFx { int cur[7]; int nxt[6]; };
#define BFX_NONE (-1)
static const BldTreeFx kBldTreeFx[] = {   // parallel to kBldTrees
    { // abbey.meditation
      { AXS_BFX_HEAL45_1SLOT_1000, AXS_BFX_HEAL56_1SLOT_1000, AXS_BFX_HEAL56_1SLOT_850, AXS_BFX_HEAL56_2SLOT_850, AXS_BFX_HEAL70_2SLOT_850, AXS_BFX_HEAL70_2SLOT_700, AXS_BFX_HEAL70_3SLOT_700 },
      { AXS_BFX_HEALS56, AXS_BFX_VISIT850, AXS_BFX_2SLOTS, AXS_BFX_HEALS70, AXS_BFX_VISIT700, AXS_BFX_3SLOTS } },
    { // abbey.prayer
      { AXS_BFX_HEAL55_1SLOT_1250, AXS_BFX_HEAL69_1SLOT_1250, AXS_BFX_HEAL69_1SLOT_1050, AXS_BFX_HEAL69_2SLOT_1050, AXS_BFX_HEAL86_2SLOT_1050, AXS_BFX_HEAL86_2SLOT_900, AXS_BFX_HEAL86_3SLOT_900 },
      { AXS_BFX_HEALS69, AXS_BFX_VISIT1050, AXS_BFX_2SLOTS, AXS_BFX_HEALS86, AXS_BFX_VISIT900, AXS_BFX_3SLOTS } },
    { // abbey.flagellation
      { AXS_BFX_HEAL65_1SLOT_1500, AXS_BFX_HEAL81_1SLOT_1500, AXS_BFX_HEAL81_1SLOT_1300, AXS_BFX_HEAL81_2SLOT_1300, AXS_BFX_HEAL100_2SLOT_1300, AXS_BFX_HEAL100_2SLOT_1100, AXS_BFX_HEAL100_3SLOT_1100 },
      { AXS_BFX_HEALS81, AXS_BFX_VISIT1300, AXS_BFX_2SLOTS, AXS_BFX_HEALS100, AXS_BFX_VISIT1100, AXS_BFX_3SLOTS } },
    { // blacksmith.weapon
      { AXS_BFX_WEAP_TIER1, AXS_BFX_WEAP_TIER2, AXS_BFX_WEAP_TIER3, AXS_BFX_WEAP_TIER4, AXS_BFX_WEAP_TIER5, BFX_NONE, BFX_NONE },
      { AXS_BFX_WEAP_UNLOCK2, AXS_BFX_WEAP_UNLOCK3, AXS_BFX_WEAP_UNLOCK4, AXS_BFX_WEAP_UNLOCK5, BFX_NONE, BFX_NONE } },
    { // blacksmith.armour
      { AXS_BFX_ARM_TIER1, AXS_BFX_ARM_TIER2, AXS_BFX_ARM_TIER3, AXS_BFX_ARM_TIER4, AXS_BFX_ARM_TIER5, BFX_NONE, BFX_NONE },
      { AXS_BFX_ARM_UNLOCK2, AXS_BFX_ARM_UNLOCK3, AXS_BFX_ARM_UNLOCK4, AXS_BFX_ARM_UNLOCK5, BFX_NONE, BFX_NONE } },
    { // blacksmith.cost
      { AXS_BFX_EQUIP_FULL, AXS_BFX_EQUIP_M10, AXS_BFX_EQUIP_M20, AXS_BFX_EQUIP_M30, AXS_BFX_EQUIP_M40, AXS_BFX_EQUIP_M50, BFX_NONE },
      { AXS_BFX_EQUIP_M10, AXS_BFX_EQUIP_M20, AXS_BFX_EQUIP_M30, AXS_BFX_EQUIP_M40, AXS_BFX_EQUIP_M50, BFX_NONE } },
    { // camping_trainer.cost
      { AXS_BFX_CAMP_FULL, AXS_BFX_CAMP_M10, AXS_BFX_CAMP_M20, AXS_BFX_CAMP_M30, AXS_BFX_CAMP_M40, AXS_BFX_CAMP_M50, BFX_NONE },
      { AXS_BFX_CAMP_M10, AXS_BFX_CAMP_M20, AXS_BFX_CAMP_M30, AXS_BFX_CAMP_M40, AXS_BFX_CAMP_M50, BFX_NONE } },
    { // guild.skill_levels
      { AXS_BFX_SKILL_RANK1, AXS_BFX_SKILL_RANK2, AXS_BFX_SKILL_RANK3, AXS_BFX_SKILL_RANK4, AXS_BFX_SKILL_RANK5, BFX_NONE, BFX_NONE },
      { AXS_BFX_SKILL_UNLOCK2, AXS_BFX_SKILL_UNLOCK3, AXS_BFX_SKILL_UNLOCK4, AXS_BFX_SKILL_UNLOCK5, BFX_NONE, BFX_NONE } },
    { // guild.cost
      { AXS_BFX_GUILD_FULL, AXS_BFX_GUILD_M10, AXS_BFX_GUILD_M20, AXS_BFX_GUILD_M30, AXS_BFX_GUILD_M40, AXS_BFX_GUILD_M50, BFX_NONE },
      { AXS_BFX_GUILD_M10, AXS_BFX_GUILD_M20, AXS_BFX_GUILD_M30, AXS_BFX_GUILD_M40, AXS_BFX_GUILD_M50, BFX_NONE } },
    { // nomad_wagon.numitems
      { AXS_BFX_TRINK_2, AXS_BFX_TRINK_4, AXS_BFX_TRINK_6, AXS_BFX_TRINK_8, AXS_BFX_TRINK_12, BFX_NONE, BFX_NONE },
      { AXS_BFX_TRINK_4, AXS_BFX_TRINK_6, AXS_BFX_TRINK_8, AXS_BFX_TRINK_12, BFX_NONE, BFX_NONE } },
    { // nomad_wagon.cost
      { AXS_BFX_TRINK_FULL, AXS_BFX_TRINK_M10, AXS_BFX_TRINK_M20, AXS_BFX_TRINK_M30, AXS_BFX_TRINK_M40, AXS_BFX_TRINK_M50, BFX_NONE },
      { AXS_BFX_TRINK_M10, AXS_BFX_TRINK_M20, AXS_BFX_TRINK_M30, AXS_BFX_TRINK_M40, AXS_BFX_TRINK_M50, BFX_NONE } },
    { // sanitarium.cost
      { AXS_BFX_QUIRK_FULL, AXS_BFX_QUIRK_M10, AXS_BFX_QUIRK_M20, AXS_BFX_QUIRK_M30, AXS_BFX_QUIRK_M40, AXS_BFX_QUIRK_M50, BFX_NONE },
      { AXS_BFX_QUIRK_M10, AXS_BFX_QUIRK_M20, AXS_BFX_QUIRK_M30, AXS_BFX_QUIRK_M40, AXS_BFX_QUIRK_M50, BFX_NONE } },
    { // sanitarium.disease_quirk_cost
      { AXS_BFX_DIS_750_33, AXS_BFX_DIS_650_33, AXS_BFX_DIS_650_67, AXS_BFX_DIS_550_67, AXS_BFX_DIS_550_ALL, AXS_BFX_DIS_450_ALL, BFX_NONE },
      { AXS_BFX_DIS_650, AXS_BFX_DIS_67PCT, AXS_BFX_DIS_550, AXS_BFX_DIS_CURESALL, AXS_BFX_DIS_450, BFX_NONE } },
    { // sanitarium.slots
      { AXS_BFX_SLOTS_Q1_D1, AXS_BFX_SLOTS_Q1_D2, AXS_BFX_SLOTS_Q2_D2, AXS_BFX_SLOTS_Q2_D3, AXS_BFX_SLOTS_Q3_D3, BFX_NONE, BFX_NONE },
      { AXS_BFX_SLOTS_D2, AXS_BFX_SLOTS_Q2, AXS_BFX_SLOTS_D3, AXS_BFX_SLOTS_Q3, BFX_NONE, BFX_NONE } },
    { // stage_coach.numrecruits
      { AXS_BFX_RECRUITS_2, AXS_BFX_RECRUITS_3, AXS_BFX_RECRUITS_4, AXS_BFX_RECRUITS_5, AXS_BFX_RECRUITS_6, AXS_BFX_RECRUITS_7, BFX_NONE },
      { AXS_BFX_RECRUITS_3, AXS_BFX_RECRUITS_4, AXS_BFX_RECRUITS_5, AXS_BFX_RECRUITS_6, AXS_BFX_RECRUITS_7, BFX_NONE } },
    { // stage_coach.rostersize
      { AXS_BFX_ROSTER_9, AXS_BFX_ROSTER_12, AXS_BFX_ROSTER_16, AXS_BFX_ROSTER_20, AXS_BFX_ROSTER_24, AXS_BFX_ROSTER_28, BFX_NONE },
      { AXS_BFX_ROSTER_12, AXS_BFX_ROSTER_16, AXS_BFX_ROSTER_20, AXS_BFX_ROSTER_24, AXS_BFX_ROSTER_28, BFX_NONE } },
    { // stage_coach.upgraded_recruits
      { AXS_BFX_RECLVL_0, AXS_BFX_RECLVL_1, AXS_BFX_RECLVL_2, AXS_BFX_RECLVL_3, BFX_NONE, BFX_NONE, BFX_NONE },
      { AXS_BFX_RECLVL_1, AXS_BFX_RECLVL_2, AXS_BFX_RECLVL_3, BFX_NONE, BFX_NONE, BFX_NONE } },
    { // tavern.bar
      { AXS_BFX_HEAL45_1SLOT_1000, AXS_BFX_HEAL56_1SLOT_1000, AXS_BFX_HEAL56_1SLOT_850, AXS_BFX_HEAL56_2SLOT_850, AXS_BFX_HEAL70_2SLOT_850, AXS_BFX_HEAL70_2SLOT_700, AXS_BFX_HEAL70_3SLOT_700 },
      { AXS_BFX_HEALS56, AXS_BFX_VISIT850, AXS_BFX_2SLOTS, AXS_BFX_HEALS70, AXS_BFX_VISIT700, AXS_BFX_3SLOTS } },
    { // tavern.gambling
      { AXS_BFX_HEAL55_1SLOT_1250, AXS_BFX_HEAL69_1SLOT_1250, AXS_BFX_HEAL69_1SLOT_1050, AXS_BFX_HEAL69_2SLOT_1050, AXS_BFX_HEAL86_2SLOT_1050, AXS_BFX_HEAL86_2SLOT_900, AXS_BFX_HEAL86_3SLOT_900 },
      { AXS_BFX_HEALS69, AXS_BFX_VISIT1050, AXS_BFX_2SLOTS, AXS_BFX_HEALS86, AXS_BFX_VISIT900, AXS_BFX_3SLOTS } },
    {
      { AXS_BFX_HEAL65_1SLOT_1500, AXS_BFX_HEAL81_1SLOT_1500, AXS_BFX_HEAL81_1SLOT_1300, AXS_BFX_HEAL81_2SLOT_1300, AXS_BFX_HEAL100_2SLOT_1300, AXS_BFX_HEAL100_2SLOT_1110, AXS_BFX_HEAL100_3SLOT_1110 },
      { AXS_BFX_HEALS81, AXS_BFX_VISIT1300, AXS_BFX_2SLOTS, AXS_BFX_HEALS100, AXS_BFX_VISIT1110, AXS_BFX_3SLOTS } },
};

static const uintptr_t BLD_UTD_HASH_OFF   = 0x90;   // UpgradeTreeDisplay+: tree id hash (ctor)
static const uintptr_t BLD_UTD_URDVEC_OFF = 0xa0;   // UTD+: vector<URD*> (ctor zeroes 0xa0..0xb0);
static const uintptr_t BLD_URD_HASH_OFF   = 0x78;   // URD+: tree hash (int)
static const uintptr_t BLD_URD_CODE_OFF   = 0x7c;   // URD+: requirement code char ('a'..)
static const uintptr_t BLD_URD_ARMED_OFF  = 0xc8;   // URD+: int, 0 = click is the locked no-op
static const uint32_t  BLD_STEP_ID_TAG    = 0x75706772; // 'upgr': step elem = hash + tag + code
static const uintptr_t BLD_REG_STRIDE     = 0x70;
static const uintptr_t BLD_REG_HASH_OFF   = 0x40;   // record+: gameHash(tree id)
static const uintptr_t BLD_BLDG_PURCH_OFF = 0xa0;   // Building+: purchased upgrade count
static const uintptr_t BLD_BLDG_TOTAL_OFF = 0xa4;   // Building+: total upgrade count

static uintptr_t haTreeRecord(uintptr_t base, uint32_t hash);
static const uintptr_t HA_REG_INSTANCED_FWD = 0x44;   // registry rec+: byte, tree is per-hero
static const int BLD_TRACK_STEPS_MAX = 8;
struct BldTrack {
    uintptr_t utd;                // the tree panel
    uint32_t  hash;               // its tree id hash
    const BldTreeDef* def;
    const BldTreeFx*  fx;         // its effect text (parallel table; null with def)
    char      id[40];             // tree id string (registry, else the def, else "")
    char      name[TM_NAME_MAX];
    int       stepsSeen;          // URD panels resolved
    int       armedIdx;
    int       bought;             // purchased steps, from the save-backed map
    int       nextIdx;
    int       state[BLD_TRACK_STEPS_MAX];
};
static BldTrack g_bldTracks[8];
static int      g_bldTrackCount = 0;
static int      g_bldUpRow = 0;             // track cursor; reset on each view open
static DWORD    g_bldBuyWatchUntil = 0;     // Enter's purchase watch; 0 = idle
static int      g_bldBuyBefore = -1;        // kept only for the log line
static uint32_t g_bldBuyHash = 0;
static uint8_t  g_bldBuyCode = 0;
static char     g_bldBuyName[TM_NAME_MAX];  // for the outcome sentence
static int      g_bldBuyStep = 0, g_bldBuyTotal = 0;
static int g_bldBuyFx = -1;

// The Building object for an id hash (the +0x55f8 vector walk tmLockState proved).
static uintptr_t bldBuildingObj(uintptr_t root, uint32_t idHash) {
    uintptr_t beg = 0, end = 0;
    if (!safeReadPtr(root + TM_BLD_VEC_BEG, &beg) || !safeReadPtr(root + TM_BLD_VEC_END, &end) ||
        !beg || end <= beg) return 0;
    uintptr_t count = (end - beg) / 8;
    if (count > 64) count = 64;
    for (uintptr_t i = 0; i < count; i++) {
        uintptr_t b = 0, def = 0;
        uint32_t  h = 0;
        if (!safeReadPtr(beg + i * 8, &b) || b <= 0x10000) continue;
        if (!safeReadPtr(b + TM_BLD_DEF_OFF, &def) || def <= 0x10000) continue;
        if (safeReadU32(def + TM_DEF_HASH_OFF, &h) && h == idHash) return b;
    }
    return 0;
}

// A tree id looks like "stage_coach.numrecruits": lowercase, underscores, one dot.
static bool bldTreeIdLooksValid(const char* s) {
    int n = 0;
    for (; s[n]; n++) {
        char c = s[n];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '.')) return false;
        if (n >= 38) return false;
    }
    return n >= 4;
}

static bool bldRegistryId(uintptr_t base, uint32_t hash, char* out, int outsz) {
    uintptr_t beg = 0, end = 0;
    if (!safeReadPtr(base + BLD_REG_BEGIN_RVA, &beg) || !safeReadPtr(base + BLD_REG_END_RVA, &end) ||
        !beg || end <= beg) return false;
    uintptr_t count = (end - beg) / BLD_REG_STRIDE;
    if (count > 512) return false;
    for (uintptr_t i = 0; i < count; i++) {
        uintptr_t rec = beg + i * BLD_REG_STRIDE;
        uint32_t h = 0;
        if (!safeReadU32(rec + BLD_REG_HASH_OFF, &h) || h != hash) continue;
        return safeReadCStr(rec, out, outsz) && bldTreeIdLooksValid(out);
    }
    return false;
}

static void bldCostText(uintptr_t base, const BldStepCost* c, char* out, int outsz) {
    out[0] = 0;
    if (!c || !c->cur1[0]) return;
    char t1[48], t2[48];
    resCurrencyTitleById(base, c->cur1, "building", t1, sizeof t1);
    if (c->cur2[0]) {
        resCurrencyTitleById(base, c->cur2, "building", t2, sizeof t2);
        _snprintf(out, outsz, axs(AXS_BLD_COST_PAIR_FMT), c->amt1, t1, c->amt2, t2);
    } else {
        _snprintf(out, outsz, "%d %s", c->amt1, t1);
    }
    out[outsz - 1] = 0;
}

static void bldCurrencyWord(uintptr_t base, uint32_t curHash, char* out, int outsz) {
    out[0] = 0;
    if (curHash && resCurrencyFind(curHash)) {
        resCurrencyTitle(base, curHash, "bldact", out, outsz);
        if (out[0]) return;
    }
    _snprintf(out, outsz, "%s", axs(AXS_BLD_OF_A_CURRENCY));
    out[outsz - 1] = 0;
    if (curHash) logLine("bldact: cost currency hash %u is in no table -- "
                         "add it to kOffBarCurrencies", curHash);
}

static int bldCollectTracks(uintptr_t base, uintptr_t root, uintptr_t panel, bool probe) {
    g_bldTrackCount = 0;
    if (!g_bldTreeHashed) {
        for (int i = 0; i < kBldTreeCount; i++) g_bldTreeHashes[i] = resHash(kBldTrees[i].id);
        g_bldTreeHashed = true;
    }
    uintptr_t trees[8];
    int nt = bldTrees(panel, trees, 8);
    for (int i = 0; i < nt && g_bldTrackCount < 8; i++) {
        BldTrack t;
        memset(&t, 0, sizeof t);
        t.utd = trees[i];
        t.armedIdx = -1;
        t.nextIdx = -1;
        if (!safeReadU32(trees[i] + BLD_UTD_HASH_OFF, &t.hash)) continue;
        for (int d = 0; d < kBldTreeCount; d++)
            if (g_bldTreeHashes[d] == t.hash) { t.def = &kBldTrees[d]; t.fx = &kBldTreeFx[d]; break; }
        if (!bldRegistryId(base, t.hash, t.id, sizeof t.id) && t.def) {
            strncpy(t.id, t.def->id, sizeof t.id - 1);
            t.id[sizeof t.id - 1] = 0;
        }
        if (t.id[0]) {
            char key[64];
            _snprintf(key, sizeof key, "upgrade_tree_name_%s", t.id);
            key[sizeof key - 1] = 0;
            if (!resolveKey(base, key, t.name, sizeof t.name)) {
                if (probe) logLine("bldup rows: no localization for \"%s\"", key);
                const char* dot = strchr(t.id, '.');
                strncpy(t.name, dot ? dot + 1 : t.id, sizeof t.name - 1);
                t.name[sizeof t.name - 1] = 0;
                for (char* p = t.name; *p; p++) if (*p == '_') *p = ' ';
            }
        } else {
            _snprintf(t.name, sizeof t.name, axs(AXS_BLD_TRACK_N_FMT), g_bldTrackCount + 1);
            t.name[sizeof t.name - 1] = 0;
        }

        uintptr_t ub = 0, ue = 0;
        if (safeReadPtr(trees[i] + BLD_UTD_URDVEC_OFF, &ub) &&
            safeReadPtr(trees[i] + BLD_UTD_URDVEC_OFF + 8, &ue) && ub && ue > ub &&
            ue - ub <= 0x2000) {
            static const uintptr_t kStrides[] = { 0x180, 0x188, 0x190, 0x178, 0x1a0, 0x170, 8 };
            uintptr_t stride = 0;
            for (int c = 0; c < (int)(sizeof kStrides / sizeof kStrides[0]) && !stride; c++) {
                uintptr_t st = kStrides[c];
                if ((ue - ub) % st) continue;
                uintptr_t cnt = (ue - ub) / st;
                if (cnt < 1 || cnt > 8) continue;
                bool all = true;
                for (uintptr_t s = 0; s < cnt && all; s++) {
                    uintptr_t obj = ub + s * st, vft = 0;
                    if (st == 8 && (!safeReadPtr(obj, &obj) || obj <= 0x10000)) { all = false; break; }
                    if (!safeReadPtr(obj, &vft) || vft != base + BLD_URD_VFT_RVA) all = false;
                }
                if (all) stride = st;
            }
            if (!stride) {
                if (probe) logLine("bldup rows: track %d URD vector %p..%p (0x%llx bytes) matches "
                                   "no stride", g_bldTrackCount, (void*)ub, (void*)ue,
                                   (unsigned long long)(ue - ub));
            } else {
                int n = (int)((ue - ub) / stride);
                for (int s = 0; s < n; s++) {
                    uintptr_t urd = ub + (uintptr_t)s * stride;
                    if (stride == 8 && (!safeReadPtr(urd, &urd) || urd <= 0x10000)) continue;
                    uint32_t uhash = 0, armed = 0;
                    uint8_t  code = 0;
                    if (!safeReadU32(urd + BLD_URD_HASH_OFF, &uhash) || uhash != t.hash) continue;
                    safeReadU8(urd + BLD_URD_CODE_OFF, &code);
                    safeReadU32(urd + BLD_URD_ARMED_OFF, &armed);
                    t.stepsSeen++;
                    if (code >= 'a' && code <= 'z') {
                        int idx = code - 'a';
                        if (armed != 0 && (t.armedIdx < 0 || idx < t.armedIdx)) t.armedIdx = idx;
                        if (idx < BLD_TRACK_STEPS_MAX) t.state[idx] = (int)armed;
                    }
                    if (probe) {
                        uint32_t f80 = 0, f84 = 0;   // unnamed neighbours: purchased-flag hunt
                        safeReadU32(urd + 0x80, &f80);
                        safeReadU32(urd + 0x84, &f84);
                        logLine("bldup rows: track %d step '%c' urd=%p stride=0x%llx armed=%u "
                                "f80=0x%x f84=0x%x", g_bldTrackCount, code, (void*)urd,
                                (unsigned long long)stride, armed, f80, f84);
                    }
                }
            }
        }
        {
            uintptr_t rec = haTreeRecord(base, t.hash);
            uint8_t instanced = 0;
            if (rec) safeReadU8(rec + HA_REG_INSTANCED_FWD, &instanced);
            int total = t.def ? t.def->steps : t.stepsSeen;
            if (total > BLD_TRACK_STEPS_MAX) total = BLD_TRACK_STEPS_MAX;
            if (instanced && probe)
                logLine("bldup rows: track \"%s\" is flagged per-hero — unexpected for a building",
                        t.id);
            for (int k = 0; k < total; k++) {
                if (!haPurchased(base, t.hash, (uint8_t)('a' + k), 0)) break;  // sequential chain
                t.bought++;
            }
            t.nextIdx = (t.bought < total) ? t.bought : -1;
        }
        if (probe) logLine("bldup rows: track %d hash=0x%x id=\"%s\" def=%s steps=%d armed=%d "
                           "bought=%d next=%d \"%s\"",
                           g_bldTrackCount, t.hash, t.id, t.def ? "yes" : "NO",
                           t.stepsSeen, t.armedIdx, t.bought, t.nextIdx, t.name);
        g_bldTracks[g_bldTrackCount++] = t;
    }
    return g_bldTrackCount;
}

static void bldTrackRowText(uintptr_t base, int i, char* out, int outsz) {
    const BldTrack* t = &g_bldTracks[i];
    int total = t->def ? t->def->steps : t->stepsSeen;
    char pos[64];                                     // the trailing "Track i of n." sentence
    _snprintf(pos, sizeof pos, axs(AXS_BLD_TRACK_POS_FMT), i + 1, g_bldTrackCount);
    pos[sizeof pos - 1] = 0;
    if (t->nextIdx >= 0) {
        int k = t->nextIdx;                           // ranks bought == the next step's index
        bool buyable = (k < BLD_TRACK_STEPS_MAX) && t->state[k] != 0;
        bool freeUp = buyable && townEventFreeUpgrades(base, "building") > 0;
        char cost[96];
        cost[0] = 0;
        if (!freeUp && t->def && k < t->def->steps)
            bldCostText(base, &t->def->cost[k], cost, sizeof cost);
        const char* curFx  = (t->fx && k <= 6 && t->fx->cur[k] >= 0) ? axs((AxStrId)t->fx->cur[k]) : "";
        const char* nextFx = (t->fx && k <  6 && t->fx->nxt[k] >= 0) ? axs((AxStrId)t->fx->nxt[k]) : "";
        char next[224];                               // "costs X, effect" / "costs X" / "effect"
        if (freeUp && nextFx[0])       _snprintf(next, sizeof next, axs(AXS_BLD_NEXT_FREE_FX_FMT), nextFx);
        else if (freeUp)               _snprintf(next, sizeof next, "%s", axs(AXS_BLD_NEXT_FREE));
        else if (cost[0] && nextFx[0]) _snprintf(next, sizeof next, axs(AXS_BLD_NEXT_COSTS_FX_FMT), cost, nextFx);
        else if (cost[0])              _snprintf(next, sizeof next, axs(AXS_BLD_NEXT_COSTS_FMT), cost);
        else if (nextFx[0])            _snprintf(next, sizeof next, "%s", nextFx);
        else                           _snprintf(next, sizeof next, "%s", axs(AXS_BLD_NEXT_AVAILABLE));
        next[sizeof next - 1] = 0;
        if (!buyable) {
            bool crossTree = t->id[0] && strcmp(t->id, "stage_coach.upgraded_recruits") == 0;
            size_t at = strlen(next);
            _snprintf(next + at, (int)(sizeof next - at), " %s", axs(crossTree
                      ? AXS_BLD_NOT_AVAILABLE_CROSS
                      : AXS_BLD_NOT_AVAILABLE_YET));
            next[sizeof next - 1] = 0;
        }
        char head[224];                               // "<name>, <k> of <total>[, <curFx>]"
        if (curFx[0]) _snprintf(head, sizeof head, axs(AXS_BLD_ROW_PROGRESS_FX_FMT),
                                t->name, k, total, curFx);
        else          _snprintf(head, sizeof head, axs(AXS_BLD_ROW_PROGRESS_FMT),
                                t->name, k, total);
        head[sizeof head - 1] = 0;
        char nextS[288];
        _snprintf(nextS, sizeof nextS, axs(AXS_BLD_NEXT_RANK_FMT), next);
        nextS[sizeof nextS - 1] = 0;
        _snprintf(out, outsz, "%s. %s %s", head, nextS, pos);
    } else if (t->id[0] && strcmp(t->id, "stage_coach.upgraded_recruits") == 0) {
        char head[224];
        _snprintf(head, sizeof head, axs(AXS_BLD_ROW_LOCKED_CROSS_FMT), t->name);
        head[sizeof head - 1] = 0;
        _snprintf(out, outsz, "%s %s", head, pos);
    } else if (t->stepsSeen > 0) {
        const char* curFx = (t->fx && total <= 6 && t->fx->cur[total] >= 0)
                            ? axs((AxStrId)t->fx->cur[total]) : "";
        char head[224];
        if (curFx[0]) _snprintf(head, sizeof head, axs(AXS_BLD_ROW_FULL_FX_FMT), t->name, curFx);
        else          _snprintf(head, sizeof head, axs(AXS_BLD_ROW_FULL_FMT), t->name);
        head[sizeof head - 1] = 0;
        _snprintf(out, outsz, "%s %s", head, pos);
    } else {
        // Steps unreadable (the collect log says why) — never invent state.
        _snprintf(out, outsz, "%s. %s", t->name, pos);
    }
    out[outsz - 1] = 0;
}

static void bldSpeakTrackRow(uintptr_t base, uintptr_t root, uintptr_t panel, const char* prefix) {
    int n = bldCollectTracks(base, root, panel, false);
    if (n <= 0) { postSpeech(axs(AXS_BLD_NO_TRACKS)); return; }
    if (g_bldUpRow >= n) g_bldUpRow = n - 1;
    if (g_bldUpRow < 0)  g_bldUpRow = 0;
    char row[320], utter[MAILBOX_SZ];
    bldTrackRowText(base, g_bldUpRow, row, sizeof row);
    _snprintf(utter, sizeof utter, "%s%s", prefix ? prefix : "", row);
    utter[sizeof utter - 1] = 0;
    postSpeech(utter);
}

static void bldProbe(uintptr_t base, uintptr_t root, uintptr_t panel, const char* why) {
    uintptr_t tracked[96];
    int ntracked = 0;
    uint32_t shown = 0, hash = 0, mode = 0;
    safeReadU32(panel + TMB_SHOWN_OFF,   &shown);
    safeReadU32(panel + BLD_ID_HASH_OFF, &hash);
    safeReadU32(panel + BLD_MODE_OFF,    &mode);
    uintptr_t vft = 0;
    safeReadPtr(panel, &vft);
    char cls[96];
    tmbClassName(base, vft, cls, sizeof cls);
    logLine("bld probe(%s): panel=%p %s shown=%u idHash=0x%x mode=%u id=\"%s\"",
            why, (void*)panel, cls[0] ? cls : "(no rtti)", shown, hash, mode, g_bldId);
    if (ntracked < 96) tracked[ntracked++] = panel;

    uintptr_t trees[8];
    int nt = bldTrees(panel, trees, 8);
    logLine("bld probe(%s): %d upgrade trees", why, nt);
    for (int i = 0; i < nt; i++) {
        uint32_t tshown = 0, telem = 0;
        safeReadU32(trees[i] + TMB_SHOWN_OFF,    &tshown);
        safeReadU32(trees[i] + BLD_TREE_ELEM_OFF, &telem);
        char chars[10];
        tmbIdChars(telem, chars);
        logLine("bld probe(%s): tree[%d]=%p shown=%u elemField=0x%x%s",
                why, i, (void*)trees[i], tshown, telem, chars);
        if (ntracked < 96) tracked[ntracked++] = trees[i];
    }

    for (int cv = 0; cv < 2; cv++) {
        uintptr_t coff = cv ? 0x118 : 0x100;
        uintptr_t cb = 0, ce = 0;
        if (!safeReadPtr(panel + coff, &cb) || !safeReadPtr(panel + coff + 8, &ce) ||
            ce < cb || (ce - cb) % 8) continue;
        int cn = (int)((ce - cb) / 8);
        if (cn <= 0 || cn > 48) continue;
        for (int c = 0; c < cn; c++) {
            uintptr_t kid = 0, kvft = 0;
            if (!safeReadPtr(cb + (uintptr_t)c * 8, &kid) || kid <= 0x10000) continue;
            if (!safeReadPtr(kid, &kvft)) continue;
            uint32_t kshown = 0xffffffff;
            safeReadU32(kid + TMB_SHOWN_OFF, &kshown);
            char kcls[96];
            tmbClassName(base, kvft, kcls, sizeof kcls);
            logLine("bld probe(%s): child %c[%d]=%p vft=+0x%llx %s shown=%u",
                    why, cv ? 'B' : 'A', c, (void*)kid,
                    (unsigned long long)(kvft > base ? kvft - base : kvft),
                    kcls[0] ? kcls : "(no rtti)", kshown);
            if (ntracked < 96) tracked[ntracked++] = kid;
        }
    }

    uintptr_t fb = 0, fe = 0;
    if (safeReadPtr(base + VEC_BEGIN_RVA, &fb) && safeReadPtr(base + VEC_END_RVA, &fe) &&
        fb && fe > fb) {
        uintptr_t count = (fe - fb) / ELEM_STRIDE;
        if (count > 512) count = 512;
        logLine("bld probe(%s): focus vector, %llu elements", why, (unsigned long long)count);
        for (uintptr_t i = 0; i < count; i++) {
            uintptr_t elem = fb + i * ELEM_STRIDE;
            int64_t id = 0;
            uintptr_t eowner = 0;
            if (!safeReadI64(elem + ELEM_ID_OFF, &id) || isNoFocus(id)) continue;
            safeReadPtr(elem + ELEM_OWNER_OFF, &eowner);
            bool mine = false;
            for (int t = 0; t < ntracked; t++)
                if (tracked[t] == eowner) { mine = true; break; }
            uint32_t xb = 0, yb = 0, wb = 0, hb = 0;
            safeReadU32(elem + ELEM_POS_OFF,      &xb);
            safeReadU32(elem + ELEM_POS_OFF + 4,  &yb);
            safeReadU32(elem + ELEM_SIZE_OFF,     &wb);
            safeReadU32(elem + ELEM_SIZE_OFF + 4, &hb);
            char chars[10];
            tmbIdChars((uint32_t)(uint64_t)id, chars);
            uint32_t relUpgr = (uint32_t)(uint64_t)id - 0x75706772;   // the 'upgr' step family
            logLine("bld probe(%s): elem id=0x%llx%s owner=%p pos=(%.0f,%.0f) size=(%.0f,%.0f)%s%s",
                    why, (unsigned long long)id, chars, (void*)eowner,
                    u32AsFloatM(xb), u32AsFloatM(yb), u32AsFloatM(wb), u32AsFloatM(hb),
                    mine ? " <-- building-owned" : "",
                    relUpgr < 64 ? " <-- 'upgr' step family" : "");
        }
    } else {
        logLine("bld probe(%s): focus vector empty", why);
    }
    logLine("bld probe(%s): end", why);
}

// ---- ROUND 3: the building screen's own OPTION ROWS (mode 0), first two families ----
static const uintptr_t BLD_CHILDA_BEG_OFF = 0x100;    // panel+: child panel vector A begin
static const uintptr_t BLD_CHILDA_END_OFF = 0x108;    // panel+: ... end
static const uintptr_t BLD_TSD_STORE_OFF  = 0x80;     // TSD+: -> the store data object
static const uintptr_t BLD_STORE_SYS_OFF  = 0x68;     // store+: embedded Inventory::System
// ---- THE JEWELER (Color of Madness), RE 2026-07-30 — re-notes/jeweler{,2,3,4,5}-output.txt ----
static const uintptr_t BLD_ITEMS_BEG_OFF  = 0x260;
static const uintptr_t BLD_ITEMS_END_OFF  = 0x268;    // ... end (was 0x270)
static const uintptr_t BLD_ITEM_INDEX_OFF = 0x2b8;    // ... the SHOWING item display's index (was 0x2c0)
static const uintptr_t BLD_SWAP_ANIM_OFF  = 0x2a8;
static const uintptr_t BLD_CTSD_SCROLL_CUR  = 0xa0;     // CTSD+: current offset (eased)
static const uintptr_t BLD_CTSD_SCROLL_TGT  = 0xa8;     // CTSD+: target offset (what slot 25 sets)
static const uintptr_t BLD_SHARD_VALUE_OFF  = 0xa0;
static const uintptr_t BLD_TDEF_HASH_OFF  = 0x40;     // rec+: TYPE hash (== item+0x44)
static const uintptr_t BLD_TDEF_IDSTR_OFF = 0x44;     // rec+: the ID STRING itself (the circus
static const uintptr_t BLD_TDEF_ID_OFF    = 0x84;     // rec+: ID hash (== item+0x88)
static const uintptr_t BLD_TDEF_VALUE_OFF = 0x9c;
static const uintptr_t BLD_ITEM_CLASS_OFF = 0x598;
static const uintptr_t BLD_ITEM_TRECORD_OFF = 0x5a0;
static const uintptr_t BLD_TREC_STRIDE    = 0x428;
static const uintptr_t BLD_TREC_ID_STR_OFF = 0x00;     // char[0x40] the trinket's own id ("jar_of_ash")
static const uintptr_t BLD_TREC_HASH_OFF  = 0x40;      // trinket id hash (== item+0x88)
static const uintptr_t BLD_TREC_BUFFS_BEG = 0x48;      // vector<u32 buff key> begin
static const uintptr_t BLD_TREC_BUFFS_END = 0x50;      // ... end
static const uintptr_t BLD_TREC_CLSREQ_BEG = 0x278;    // vector<u32 class-id hash> begin (was 0x1b8)
static const uintptr_t BLD_TREC_CLSREQ_END = 0x280;    // ... end (hero_class_requirements) (was 0x1c0)
static const uintptr_t BLD_TREC_RARITY_OFF = 0x324;
static const uintptr_t BLD_TREC_RARITY_ID  = 0x2e4;    // ... and its ID STRING, char[0x40] (was 0x224)
static const uintptr_t BLD_CLASS_IDHASH_OFF= 0xd4;      // HeroClass+: gameHash(class id)
static const uint32_t  BLD_ELEM_RCT_BASE  = 0x737467; // recruit slot element family

static int   g_bldRow = 0;                // mode-0 row cursor
static bool  g_bldKeepRow = false;        // survive one stand-down (the sheet detour)
static bool  g_bldResume = false;
static DWORD g_bldSheetWatchUntil = 0;    // recruit Enter's sheet-open watch; 0 = idle
static DWORD g_bldShopWatchUntil = 0;     // store Enter's purchase watch; 0 = idle
static int   g_bldShopGoldBefore = 0;     // the paying wallet before the buy click
static bool  g_bldShopComet = false;
static DWORD g_bldSwapWatchUntil = 0;     // Left/Right's column-switch watch; 0 = idle
static uint32_t g_bldSwapIndexBefore = 0; // the showing-column index before the switch
static int   g_bldShopCountBefore = 0;    // occupied store slots before the buy click
static char  g_bldShopName[128];          // the item being bought, for the outcome line

uintptr_t bldChildByVft(uintptr_t base, uintptr_t panel, const uintptr_t* rvas, int nrva) {
    uintptr_t cb = 0, ce = 0;
    if (!safeReadPtr(panel + BLD_CHILDA_BEG_OFF, &cb) ||
        !safeReadPtr(panel + BLD_CHILDA_END_OFF, &ce) || !cb || ce <= cb || (ce - cb) % 8) return 0;
    int cn = (int)((ce - cb) / 8);
    if (cn > 48) return 0;
    for (int c = 0; c < cn; c++) {
        uintptr_t kid = 0, vft = 0;
        if (!safeReadPtr(cb + (uintptr_t)c * 8, &kid) || kid <= 0x10000) continue;
        if (!safeReadPtr(kid, &vft) || vft <= base) continue;
        for (int r = 0; r < nrva; r++)
            if (vft - base == rvas[r]) return kid;
    }
    return 0;
}

int bldItemDisplays(uintptr_t panel, uintptr_t* out, int max) {
    uintptr_t ib = 0, ie = 0;
    if (!safeReadPtr(panel + BLD_ITEMS_BEG_OFF, &ib) ||
        !safeReadPtr(panel + BLD_ITEMS_END_OFF, &ie) || !ib || ie < ib || (ie - ib) % 8) return 0;
    int n = (int)((ie - ib) / 8);
    if (n < 0 || n > 16) return 0;                 // a wagon has 2, an activity wing 3
    int w = 0;
    for (int i = 0; i < n && w < max; i++) {
        uintptr_t kid = 0;
        if (!safeReadPtr(ib + (uintptr_t)i * 8, &kid) || kid <= 0x10000) continue;
        out[w++] = kid;
    }
    return w;
}

enum { BLD_STORE_NONE = 0, BLD_STORE_GOLD = 1, BLD_STORE_COMET = 2 };
static int bldStoreKindOf(uintptr_t base, uintptr_t disp) {
    uintptr_t vft = 0;
    if (!disp || !safeReadPtr(disp, &vft) || vft <= base) return BLD_STORE_NONE;
    if (vft - base == BLD_TSD_VFT_RVA)  return BLD_STORE_GOLD;
    if (vft - base == BLD_CTSD_VFT_RVA) return BLD_STORE_COMET;
    return BLD_STORE_NONE;
}

static uintptr_t bldStorePanel(uintptr_t base, uintptr_t panel, int* kindOut) {
    if (kindOut) *kindOut = BLD_STORE_NONE;
    uintptr_t items[16];
    int n = bldItemDisplays(panel, items, 16);
    if (n > 0) {
        uint32_t idx = 0;
        safeReadU32(panel + BLD_ITEM_INDEX_OFF, &idx);
        if ((int)idx >= 0 && (int)idx < n) {
            int kind = bldStoreKindOf(base, items[idx]);
            if (kind != BLD_STORE_NONE) {
                if (kindOut) *kindOut = kind;
                return items[idx];
            }
            return 0;
        }
    }
    static const uintptr_t kStoreVfts[] = { BLD_TSD_VFT_RVA, BLD_CTSD_VFT_RVA };
    uintptr_t disp = bldChildByVft(base, panel, kStoreVfts, 2);
    if (disp && kindOut) *kindOut = bldStoreKindOf(base, disp);
    return disp;
}

int bldRecruitKindOf(uintptr_t base, uintptr_t disp) {
    uintptr_t vft = 0;
    if (!disp || !safeReadPtr(disp, &vft) || vft <= base) return BLD_RCT_NONE;
    uintptr_t rva = vft - base;
    if (rva == BLD_SHRD_VFT_RVA) return BLD_RCT_SHARD;
    if (rva == BLD_HRD_VFT_RVA || rva == BLD_BHRD_VFT_RVA) return BLD_RCT_BASE;
    return BLD_RCT_NONE;
}

static uintptr_t bldRecruitDisplay(uintptr_t base, uintptr_t panel, int* kindOut) {
    if (kindOut) *kindOut = BLD_RCT_NONE;
    uintptr_t items[16];
    int n = bldItemDisplays(panel, items, 16);
    if (n > 0) {
        uint32_t idx = 0;
        safeReadU32(panel + BLD_ITEM_INDEX_OFF, &idx);
        if ((int)idx >= 0 && (int)idx < n) {
            int kind = bldRecruitKindOf(base, items[idx]);
            if (kind != BLD_RCT_NONE) {
                if (kindOut) *kindOut = kind;
                return items[idx];
            }
            return 0;
        }
    }
    static const uintptr_t kRecruitVfts[] = { BLD_BHRD_VFT_RVA, BLD_HRD_VFT_RVA, BLD_SHRD_VFT_RVA };
    uintptr_t disp = bldChildByVft(base, panel, kRecruitVfts, 3);
    if (disp && kindOut) *kindOut = bldRecruitKindOf(base, disp);
    return disp;
}

static uintptr_t bldStoreSystemKind(uintptr_t base, uintptr_t panel, int* kindOut) {
    int kind = BLD_STORE_NONE;
    uintptr_t tsd = bldStorePanel(base, panel, &kind);
    if (kindOut) *kindOut = kind;
    if (!tsd) return 0;
    uintptr_t store = 0;
    if (!safeReadPtr(tsd + BLD_TSD_STORE_OFF, &store) || store <= 0x10000) return 0;
    uintptr_t sys = store + BLD_STORE_SYS_OFF;
    uintptr_t beg = 0; int slots = 0;
    if (!invItemVectorAt(sys, &beg, &slots)) return 0;
    return sys;
}
static uintptr_t bldStoreSystem(uintptr_t base, uintptr_t panel) {
    return bldStoreSystemKind(base, panel, nullptr);
}

static uintptr_t bldCometPanelOf(uintptr_t base, uintptr_t panel) {
    uintptr_t items[16];
    int n = bldItemDisplays(panel, items, 16);
    for (int i = 0; i < n; i++)
        if (bldStoreKindOf(base, items[i]) == BLD_STORE_COMET) return items[i];
    return 0;
}

static int bldStoreRows(uintptr_t sys, int* slotIdx, int max) {
    uintptr_t beg = 0; int slots = 0, n = 0;
    if (!invItemVectorAt(sys, &beg, &slots)) return 0;
    for (int i = 0; i < slots && n < max; i++) {
        uint32_t amount = 0;
        if (!safeReadU32(beg + (uintptr_t)i * ITEM_STRIDE + ITEM_AMOUNT_OFF, &amount)) continue;
        if ((int32_t)amount > 0) slotIdx[n++] = i;
    }
    return n;
}

// ---- THE COLUMNS: a building's second pool ----
typedef void (*BldSwitchFn)(void* closure);
struct BldSwitchClosure { uintptr_t vft; uintptr_t panel; };
static bool bldSehSwitchMerchant(uintptr_t base, uintptr_t panel) {
    BldSwitchClosure c;
    c.vft = 0;
    c.panel = panel;
    __try { ((BldSwitchFn)(base + BLD_SWITCH_FN_RVA))(&c); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

static int bldColumnCount(uintptr_t panel, int* showingOut) {
    if (showingOut) *showingOut = 0;
    uintptr_t items[16];
    int n = bldItemDisplays(panel, items, 16);
    if (n <= 0) return 0;
    uint32_t idx = 0;
    safeReadU32(panel + BLD_ITEM_INDEX_OFF, &idx);
    if ((int)idx < 0 || (int)idx >= n) idx = 0;
    if (showingOut) *showingOut = (int)idx;
    return n;
}

static void bldColumnName(uintptr_t base, uintptr_t panel, int col, char* out, int outsz) {
    out[0] = 0;
    uintptr_t items[16];
    int n = bldItemDisplays(panel, items, 16);
    const char* key = nullptr;
    const char* english = nullptr;
    if (col >= 0 && col < n) {
        if (bldStoreKindOf(base, items[col]) == BLD_STORE_COMET) {
            key = "tutorial_popup_shard_nomad_wagon_title";
            english = axs(AXS_JW_JEWELER_FALLBACK);
        } else if (bldRecruitKindOf(base, items[col]) == BLD_RCT_SHARD) {
            key = "tutorial_popup_shard_stage_coach_title";
            english = axs(AXS_JW_SHARDMERC_FALLBACK);
        }
    }
    if (key) {
        char raw[160];
        if (resolveKey(base, key, raw, sizeof raw) && raw[0]) stripMarkup(raw, out, outsz);
        if (!out[0]) {
            _snprintf(out, outsz, "%s", english);
            logLine("bldcols: no %s — fallback \"%s\"", key, english);
        }
        out[outsz - 1] = 0;
        return;
    }
    _snprintf(out, outsz, "%s", g_bldName[0] ? g_bldName : axs(AXS_BLD_THIS_BUILDING));
    out[outsz - 1] = 0;
}

static bool bldColumnSwitchTo(uintptr_t base, uintptr_t panel, int target) {
    uintptr_t items[16];
    int n = bldItemDisplays(panel, items, 16);
    uint32_t idx = 0;
    safeReadU32(panel + BLD_ITEM_INDEX_OFF, &idx);
    uintptr_t swapAnim = 0;
    safeReadPtr(panel + BLD_SWAP_ANIM_OFF, &swapAnim);
    if (n < 2 || (int)idx >= n || target < 0 || target >= n || !swapAnim) {
        logLine("bldcols: no switch here (displays=%d index=%u target=%d swapAnim=%p)",
                n, idx, target, (void*)swapAnim);
        return false;
    }
    int steps = (target - (int)idx + n) % n;
    if (steps <= 0) return false;
    if (steps > 1)
        logLine("bldcols: %d columns — reaching %d from %u takes %d presses of the game's button",
                n, target, idx, steps);
    for (int s = 0; s < steps; s++) {
        if (!bldSehSwitchMerchant(base, panel)) {
            logLine("bldcols: switch body faulted (panel=%p step %d of %d)", (void*)panel, s + 1, steps);
            return false;
        }
    }
    g_bldSwapIndexBefore = idx;
    g_bldSwapWatchUntil  = GetTickCount() + 1500;
    logLine("bldcols: switch requested, column %u -> %d of %d", idx, target, n);
    return true;
}

// ---- The Jeweler's SCROLL ----
typedef void (*BldShowRowFn)(uintptr_t panel, uintptr_t elem);
static bool bldSehShowRow(uintptr_t base, uintptr_t panel, uintptr_t elem) {
    __try { ((BldShowRowFn)(base + BLD_CTSD_SHOWROW_RVA))(panel, elem); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
static void bldCometScrollToRow(uintptr_t base, uintptr_t bldPanel, int row) {
    int kind = BLD_STORE_NONE;
    uintptr_t store = bldStorePanel(base, bldPanel, &kind);
    if (!store || kind != BLD_STORE_COMET) return;     // only the comet panel scrolls
    uintptr_t elem = feGetElementById((int64_t)(uint32_t)(INV_FOURCC_BASE + (uint32_t)row));
    if (!elem) return;                                 // not drawn this frame — nothing to aim at
    float before = 0, after = 0;
    uint32_t raw = 0;
    if (safeReadU32(store + BLD_CTSD_SCROLL_TGT, &raw)) before = u32AsFloatM(raw);
    if (!bldSehShowRow(base, store, elem)) {
        logLine("jeweler: scroll-into-view faulted (store=%p row=%d)", (void*)store, row);
        return;
    }
    if (safeReadU32(store + BLD_CTSD_SCROLL_TGT, &raw)) after = u32AsFloatM(raw);
    uint32_t cur = 0;
    safeReadU32(store + BLD_CTSD_SCROLL_CUR, &cur);
    logLine("jeweler: scroll row %d -> target %.1f (was %.1f, current %.1f)",
            row, after, before, u32AsFloatM(cur));
}

struct BldRecruit { int slot; uintptr_t hero; };
static int bldRecruitRows(uintptr_t base, uintptr_t panel, BldRecruit* out, int max,
                          uintptr_t* dispOut) {
    uintptr_t disp = bldRecruitDisplay(base, panel, nullptr);
    if (dispOut) *dispOut = disp;
    if (!disp) return 0;
    uintptr_t sb = 0, se = 0;
    if (!safeReadPtr(disp + BLD_RCT_SLOTS_BEG, &sb) ||
        !safeReadPtr(disp + BLD_RCT_SLOTS_END, &se) || !sb || se <= sb ||
        (se - sb) % BLD_RCT_SLOT_STRIDE) return 0;
    int slots = (int)((se - sb) / BLD_RCT_SLOT_STRIDE);
    if (slots > 16) return 0;
    int n = 0;
    for (int i = 0; i < slots && n < max; i++) {
        uintptr_t widget = 0, iface = 0, hero = 0;
        if (!safeReadPtr(sb + (uintptr_t)i * BLD_RCT_SLOT_STRIDE, &widget) || widget <= 0x10000)
            continue;
        if (!safeReadPtr(widget + BLD_RCT_IFACE_OFF, &iface) || iface <= 0x10000) continue;
        if (!safeReadPtr(iface + BLD_RCT_IFACE_HERO, &hero) || hero <= 0x10000) continue;
        out[n].slot = i;
        out[n].hero = hero;
        n++;
    }
    return n;
}

// ---- STATUE (Ancestor's Memoirs) — the media browser (passes 49-51) ----
static const uintptr_t ST_CAT_VEC_BEG = 0x288;
static const uintptr_t ST_CAT_VEC_END = 0x290;
static const uintptr_t ST_CAT_STRIDE  = 0x968;   // one category
static const uintptr_t ST_CAT_ID_OFF  = 0x20;    // cat+: id string (0x80)
static const uintptr_t ST_CAT_ENT_BEG = 0x928;   // cat+: vector<Entry> begin
static const uintptr_t ST_CAT_ENT_END = 0x930;   // cat+: ... end
static const uintptr_t ST_ENT_STRIDE  = 0x158;   // one entry
static const uintptr_t ST_ENT_REC_BEG = 0x140;   // entry+: vector<MediaRecord> begin
static const uintptr_t ST_ENT_REC_END = 0x148;   // entry+: ... end
static const uintptr_t ST_REC_STRIDE  = 0x450;   // one media record
static const uintptr_t ST_REC_TEXT_OFF = 0x44;   // record+: localized display text (0x200)
static const uintptr_t ST_REC_TYPE_OFF = 0x248;  // record+: int type
static const uintptr_t ST_REC_LOCK_OFF = 0x44c;  // record+: char (1 unlocked / 0 locked)
static const uint32_t  ST_ELEM_BASE    = 0x6d646961; // media button element id base

// True when the OPEN building is the statue (its own panel vftable, not a child).
static bool bldIsStatue(uintptr_t base, uintptr_t panel) {
    uintptr_t vft = 0;
    if (!panel || !safeReadPtr(panel, &vft) || vft <= base) return false;
    return vft - base == BLD_STATUE_VFT_RVA;
}

static bool bldHasActivityRows(uintptr_t base, uintptr_t panel);
static bool bldIsGraveyard(uintptr_t base, uintptr_t panel);
static int  haSkinOf(uintptr_t base, uintptr_t panel);
static int bldRowKindOf(uintptr_t base, uintptr_t panel, uintptr_t* sysOut,
                        int* storeKindOut = nullptr) {
    if (sysOut) *sysOut = 0;
    if (storeKindOut) *storeKindOut = BLD_STORE_NONE;
    if (bldIsStatue(base, panel)) return 4;           // the Ancestor's Memoirs (its own reader)
    if (bldIsGraveyard(base, panel)) return 5;        // the graveyard's dead-hero memorial list
    if (haSkinOf(base, panel) >= 0) return 6;         // Blacksmith / Guild / Camping Trainer
    int storeKind = BLD_STORE_NONE;
    uintptr_t sys = bldStoreSystemKind(base, panel, &storeKind);
    if (sysOut) *sysOut = sys;
    if (storeKindOut) *storeKindOut = storeKind;
    if (sys) return 1;
    if (bldRecruitDisplay(base, panel, nullptr)) return 2;      // the SHOWING pool (base or shard)
    if (bldHasActivityRows(base, panel)) return 3;
    return 0;
}

static bool bldItemClassVector(uintptr_t base, uintptr_t* beg, uintptr_t* end) {
    uintptr_t obj = 0;
    if (!safeReadPtr(base + BLD_TRINKDB_PTR_RVA, &obj) || obj <= 0x10000) return false;
    if (!safeReadPtr(obj, beg) || !safeReadPtr(obj + 8, end) || *beg <= 0x10000 || *end < *beg ||
        (*end - *beg) % 8 || (*end - *beg) / 8 > 4096) return false;
    return true;
}

uintptr_t bldTrinketDef(uintptr_t base, uintptr_t item) {
    uint32_t typeHash = 0, idHash = 0;
    if (!safeReadU32(item + ITEM_TYPEHASH_OFF, &typeHash) ||
        !safeReadU32(item + ITEM_IDHASH_OFF, &idHash)) return 0;
    uintptr_t cached = 0;
    uint32_t th = 0, ih = 0;
    if (safeReadPtr(item + BLD_ITEM_CLASS_OFF, &cached) && cached > 0x10000 &&
        safeReadU32(cached + BLD_TDEF_HASH_OFF, &th) && th == typeHash &&
        safeReadU32(cached + BLD_TDEF_ID_OFF, &ih) && ih == idHash) return cached;
    uintptr_t beg = 0, end = 0;
    if (!bldItemClassVector(base, &beg, &end)) return 0;
    uintptr_t count = (end - beg) / 8;
    for (uintptr_t i = 0; i < count; i++) {
        uintptr_t def = 0;
        if (!safeReadPtr(beg + i * 8, &def) || def <= 0x10000) continue;
        if (!safeReadU32(def + BLD_TDEF_HASH_OFF, &th) || th != typeHash) continue;
        if (!safeReadU32(def + BLD_TDEF_ID_OFF, &ih) || ih != idHash) continue;
        return def;
    }
    return 0;
}

static const uintptr_t BLD_TDEF_TYPESTR_OFF = 0x00;   // rec+: char[0x40] the TYPE string
bool bldTrinketIdByHash(uintptr_t base, uint32_t idHash, char* out, int outsz) {
    if (!idHash) return false;
    uintptr_t beg = 0, end = 0;
    if (!bldItemClassVector(base, &beg, &end)) {
        logLine("bldrows: ItemClass registry unreadable (ptr global 0x%llx)", (unsigned long long)BLD_TRINKDB_PTR_RVA);
        return false;
    }
    uintptr_t count = (end - beg) / 8;
    char fallback[64] = { 0 };
    for (uintptr_t i = 0; i < count; i++) {
        uintptr_t def = 0;
        uint32_t ih = 0, th = 0;
        if (!safeReadPtr(beg + i * 8, &def) || def <= 0x10000) continue;
        if (!safeReadU32(def + BLD_TDEF_ID_OFF, &ih) || ih != idHash) continue;
        char type[0x41] = { 0 }, id[64] = { 0 };
        if (!safeReadCStr(def + BLD_TDEF_IDSTR_OFF, id, sizeof id) || !id[0]) continue;
        if (resHash(id) != idHash) {
            logLine("bldrows: id string \"%s\" at rec %p does not hash back to 0x%x — skipping",
                    id, (void*)def, idHash);
            continue;
        }
        safeReadCStr(def + BLD_TDEF_TYPESTR_OFF, type, sizeof type);
        safeReadU32(def + BLD_TDEF_HASH_OFF, &th);
        if (strcmp(type, "trinket") == 0 || th == resHash("trinket")) {
            _snprintf(out, outsz, "%s", id);
            out[outsz - 1] = 0;
            return true;
        }
        if (!fallback[0]) {
            _snprintf(fallback, sizeof fallback, "%s", id);
            logLine("bldrows: rec %p id \"%s\" hashes back but type reads \"%s\" / 0x%08x "
                    "(resHash(\"trinket\") = 0x%08x) -- taken unless a typed match follows",
                    (void*)def, id, type, th, resHash("trinket"));
        }
    }
    if (fallback[0]) {
        _snprintf(out, outsz, "%s", fallback);
        out[outsz - 1] = 0;
        return true;
    }
    return false;
}

void bldItemClassProbe(uintptr_t base, uint32_t wantIdHash) {
    if (!axDebugLogEnabled()) return;
    uintptr_t obj = 0, beg = 0, end = 0;
    safeReadPtr(base + BLD_TRINKDB_PTR_RVA, &obj);
    if (!bldItemClassVector(base, &beg, &end)) {
        logLine("itemclass probe: registry object %p (global 0x%llx) unreadable or absurd",
                (void*)obj, (unsigned long long)BLD_TRINKDB_PTR_RVA);
        return;
    }
    uintptr_t count = (end - beg) / 8;
    logLine("itemclass probe: registry object %p -> [%p,%p) -> %llu records; resHash(\"trinket\")=0x%08x, "
            "looking for id hash 0x%08x", (void*)obj, (void*)beg, (void*)end, (unsigned long long)count,
            resHash("trinket"), wantIdHash);
    int shown = 0;
    for (uintptr_t i = 0; i < count && i < 4096; i++) {
        uintptr_t def = 0;
        if (!safeReadPtr(beg + i * 8, &def) || def <= 0x10000) continue;
        uint32_t th = 0, ih = 0;
        char type[0x41] = { 0 }, id[0x41] = { 0 };
        safeReadU32(def + BLD_TDEF_HASH_OFF, &th);
        safeReadU32(def + BLD_TDEF_ID_OFF, &ih);
        safeReadCStr(def + BLD_TDEF_TYPESTR_OFF, type, sizeof type);
        safeReadCStr(def + BLD_TDEF_IDSTR_OFF, id, sizeof id);
        bool hit = ih == wantIdHash;
        if (shown < 3 || hit) {
            logLine("itemclass probe:   [%llu] rec=%p type(+0x00)=\"%s\" typeHash(+0x40)=0x%08x "
                    "(resHash of it = 0x%08x) id(+0x44)=\"%s\" idHash(+0x84)=0x%08x (resHash = 0x%08x)%s",
                    (unsigned long long)i, (void*)def, type, th, resHash(type), id, ih, resHash(id),
                    hit ? "   <-- THE ONE WANTED" : "");
            shown++;
        }
    }
}

static bool bldTrinketBaseValue(uintptr_t base, uintptr_t item, const char* itemId,
                                bool shard, int* valOut) {
    uintptr_t def = bldTrinketDef(base, item);
    if (!def) {
        logLine("bldrows: no def record for trinket \"%s\" (type/id hash match)", itemId);
        return false;
    }
    uint32_t v = 0;
    if (!safeReadU32(def + (shard ? BLD_SHARD_VALUE_OFF : BLD_TDEF_VALUE_OFF), &v)) return false;
    if ((int32_t)v <= 0 || v > 10000000) {
        logLine("bldrows: def %p for \"%s\" has implausible %s value %d",
                (void*)def, itemId, shard ? "shard" : "gold", (int32_t)v);
        return false;
    }
    *valOut = (int)v;
    return true;
}

typedef int (*BldPriceFn)(int baseValue, uint32_t key);
static bool bldSehPrice(uintptr_t base, int baseValue, uint32_t key, int* out) {
    __try { *out = ((BldPriceFn)(base + BLD_PRICE_FN_RVA))(baseValue, key); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

static bool bldTrinketPriceOf(uintptr_t base, uintptr_t item, const char* itemId,
                              bool shard, int* priceOut) {
    int baseVal = 0;
    if (!bldTrinketBaseValue(base, item, itemId, shard, &baseVal)) return false;
    uint32_t key = 0;
    if (!safeReadU32(base + (shard ? BLD_SHARD_PRICE_KEY_RVA : BLD_PRICE_KEY_RVA), &key))
        return false;
    int price = 0;
    if (!bldSehPrice(base, baseVal, key, &price)) {
        logLine("bldrows: price fn faulted (base=%d key=0x%x %s)",
                baseVal, key, shard ? "shard" : "gold");
        return false;
    }
    if (price <= 0 || price > baseVal * 2) {
        logLine("bldrows: implausible %s price %d (base=%d key=0x%x) -> omitted",
                shard ? "shard" : "gold", price, baseVal, key);
        return false;
    }
    *priceOut = price;
    return true;
}

uintptr_t bldTrinketRecordByHash(uintptr_t base, uint32_t idHash) {
    if (!idHash) return 0;
    uintptr_t beg = 0, end = 0;
    if (!safeReadPtr(base + BLD_TRECDB_BEG_RVA, &beg) ||
        !safeReadPtr(base + BLD_TRECDB_END_RVA, &end) || !beg || end <= beg ||
        (end - beg) % BLD_TREC_STRIDE) return 0;
    uintptr_t count = (end - beg) / BLD_TREC_STRIDE;
    if (count > 2048) return 0;
    for (uintptr_t i = 0; i < count; i++) {
        uintptr_t r = beg + i * BLD_TREC_STRIDE;
        uint32_t h = 0;
        if (safeReadU32(r + BLD_TREC_HASH_OFF, &h) && h == idHash) return r;
    }
    return 0;
}

uintptr_t bldTrinketRecord(uintptr_t base, uintptr_t item) {
    uint32_t idHash = 0, h = 0;
    if (!safeReadU32(item + ITEM_IDHASH_OFF, &idHash) || !idHash) return 0;
    uintptr_t rec = 0;
    if (safeReadPtr(item + BLD_ITEM_TRECORD_OFF, &rec) && rec > 0x10000 &&
        safeReadU32(rec + BLD_TREC_HASH_OFF, &h) && h == idHash) return rec;
    return bldTrinketRecordByHash(base, idHash);
}

bool bldTrinketRarity(uintptr_t base, uintptr_t rec, char* out, int outsz, bool probe) {
    out[0] = 0;
    uint32_t want = 0;
    if (!safeReadU32(rec + BLD_TREC_RARITY_OFF, &want) || !want) return false;
    char id[72];
    id[0] = 0;
    if (!safeReadCStr(rec + BLD_TREC_RARITY_ID, id, sizeof id) || !id[0]) {
        if (probe) logLine("bldrows: rarity id string unreadable at rec+0x%llx (hash 0x%08x)",
                           (unsigned long long)BLD_TREC_RARITY_ID, want);
        return false;
    }
    if (resHash(id) != want) {
        logLine("bldrows: rarity id \"%s\" hashes 0x%08x but the record says 0x%08x"
                " — rec+0x%llx is not the rarity id, omitting",
                id, resHash(id), want, (unsigned long long)BLD_TREC_RARITY_ID);
        return false;
    }
    char key[96];
    _snprintf(key, sizeof key, "trinket_rarity_%s", id);
    key[sizeof key - 1] = 0;
    if (!resolveKey(base, key, out, outsz) || !out[0]) {
        logLine("bldrows: \"%s\" did not resolve — falling back to the raw rarity id", key);
        strncpy(out, id, outsz - 1);
        out[outsz - 1] = 0;
        for (char* p = out; *p; p++) if (*p == '_') *p = ' ';
    }
    abStripMarkup(out);
    if (probe) logLine("bldrows: rarity id=\"%s\" (hash 0x%08x verified) -> \"%s\"", id, want, out);
    return true;
}

bool bldClassNameByHash(uintptr_t base, uint32_t want, char* out, int outsz) {
    out[0] = 0;
    if (!want) return false;
    uint8_t alt = 0;
    safeReadU8(base + RI_CIRCUS_FLAG_RVA, &alt);
    const uintptr_t dbs[2] = { base + (alt ? BLD_CLASSDB_ALT_RVA : BLD_CLASSDB_RVA),
                               base + (alt ? BLD_CLASSDB_RVA : BLD_CLASSDB_ALT_RVA) };
    for (int d = 0; d < 2; d++) {
        uintptr_t cb = 0, ce = 0;
        if (!safeReadPtr(dbs[d], &cb) || !safeReadPtr(dbs[d] + 8, &ce) || !cb || ce <= cb) continue;
        int nc = (int)((ce - cb) / 8);
        if (nc <= 0 || nc > 128) continue;
        for (int c = 0; c < nc; c++) {
            uintptr_t cobj = 0;
            uint32_t h = 0;
            if (!safeReadPtr(cb + (uintptr_t)c * 8, &cobj) || cobj <= 0x10000) continue;
            if (!safeReadU32(cobj + BLD_CLASS_IDHASH_OFF, &h) || h != want) continue;
            uint8_t latched = 0;
            if (safeReadU8(cobj + HEROCLASS_DISP_LATCH_OFF, &latched) && latched)
                safeReadCStr(cobj + HEROCLASS_DISP_OFF, out, outsz);
            if (!out[0]) safeReadCStr(cobj + HEROCLASS_ID_OFF, out, outsz);
            if (out[0]) return true;
        }
    }
    static const char* const kClassIds[] = {
        "abomination", "antiquarian", "arbalest", "bounty_hunter", "crusader", "flagellant",
        "grave_robber", "hellion", "highwayman", "houndmaster", "jester", "leper", "man_at_arms",
        "musketeer", "occultist", "plague_doctor", "shieldbreaker", "vestal",
    };
    for (int i = 0; i < (int)(sizeof kClassIds / sizeof kClassIds[0]); i++) {
        if (resHash(kClassIds[i]) != want) continue;
        static uint32_t noted[8];
        static int      nnoted = 0;
        bool seen = false;
        for (int k = 0; k < nnoted; k++) if (noted[k] == want) { seen = true; break; }
        if (!seen && nnoted < 8) {
            noted[nnoted++] = want;
            logLine("classdb: hash 0x%08x is in NO registry but IS \"%s\" -- named from the "
                    "shipped id list", want, kClassIds[i]);
        }
        char key[96];
        _snprintf(key, sizeof key, "hero_class_name_%s", kClassIds[i]);
        key[sizeof key - 1] = 0;
        if (resolveKey(base, key, out, outsz) && out[0]) return true;
        logLine("classdb: \"%s\" resolved no name -- speaking the raw id", key);
        _snprintf(out, outsz, "%s", kClassIds[i]);
        out[outsz - 1] = 0;
        return true;
    }
    return false;
}

static bool g_bldClassDbDumped = false;
static void bldDumpClassRegistries(uintptr_t base, uint32_t want) {
    if (!axDebugLogEnabled() || g_bldClassDbDumped) return;   // the registries do not change
    g_bldClassDbDumped = true;                                // within a session: once is enough,
    uint8_t alt = 0;
    safeReadU8(base + RI_CIRCUS_FLAG_RVA, &alt);
    logLine("classdb: dumping both registries (wanted 0x%08x, circus=%d)", want, alt ? 1 : 0);
    const uintptr_t rvas[2] = { BLD_CLASSDB_RVA, BLD_CLASSDB_ALT_RVA };
    const char*     names[2] = { "campaign", "circus" };
    for (int d = 0; d < 2; d++) {
        uintptr_t cb = 0, ce = 0;
        if (!safeReadPtr(base + rvas[d], &cb) || !safeReadPtr(base + rvas[d] + 8, &ce) ||
            !cb || ce <= cb) {
            logLine("classdb: %s registry unreadable", names[d]);
            continue;
        }
        int nc = (int)((ce - cb) / 8);
        logLine("classdb: %s registry holds %d classes", names[d], nc);
        if (nc <= 0 || nc > 128) continue;
        for (int c = 0; c < nc; c++) {
            uintptr_t cobj = 0;
            uint32_t h = 0;
            char id[96] = {0}, disp[96] = {0};
            uint8_t latched = 0;
            if (!safeReadPtr(cb + (uintptr_t)c * 8, &cobj) || cobj <= 0x10000) {
                logLine("classdb:   %s[%d] = (unreadable)", names[d], c);
                continue;
            }
            safeReadU32(cobj + BLD_CLASS_IDHASH_OFF, &h);
            safeReadCStr(cobj + HEROCLASS_ID_OFF, id, sizeof id);
            if (safeReadU8(cobj + HEROCLASS_DISP_LATCH_OFF, &latched) && latched)
                safeReadCStr(cobj + HEROCLASS_DISP_OFF, disp, sizeof disp);
            logLine("classdb:   %s[%d] hash=0x%08x id=\"%s\" disp=\"%s\"%s",
                    names[d], c, h, id, disp, h == want ? "   <== THE WANTED ONE" : "");
        }
    }
}

void bldTrinketWhyNotFit(uintptr_t base, uintptr_t item, uintptr_t hero) {
    if (!axDebugLogEnabled()) return;
    char type[64] = {0}, itemId[64] = {0}, key[192] = {0}, name[256] = {0};
    invItemName(base, item, type, itemId, key, name);
    uintptr_t cls = 0;
    uint32_t mine = 0;
    char myId[96] = {0}, myDisp[96] = {0};
    uint8_t latched = 0;
    safeReadPtr(hero + ACTOR_HEROCLASS_OFF, &cls);
    if (cls > 0x10000) {
        safeReadU32(cls + BLD_CLASS_IDHASH_OFF, &mine);
        safeReadCStr(cls + HEROCLASS_ID_OFF, myId, sizeof myId);
        if (safeReadU8(cls + HEROCLASS_DISP_LATCH_OFF, &latched) && latched)
            safeReadCStr(cls + HEROCLASS_DISP_OFF, myDisp, sizeof myDisp);
    }
    logLine("whynotfit: \"%s\" (%s%s) refused for hero class %p hash=0x%08x id=\"%s\" disp=\"%s\"",
            name, type, itemId, (void*)cls, mine, myId, myDisp);
    uintptr_t rec = bldTrinketRecord(base, item);
    if (!rec) { logLine("whynotfit:   no def record -- the refusal cannot be grounded"); return; }
    uintptr_t kb = 0, ke = 0;
    if (!safeReadPtr(rec + BLD_TREC_CLSREQ_BEG, &kb) ||
        !safeReadPtr(rec + BLD_TREC_CLSREQ_END, &ke) || !kb || ke <= kb || (ke - kb) % 4) {
        logLine("whynotfit:   rec=%p class-req vector unreadable", (void*)rec);
        return;
    }
    int n = (int)((ke - kb) / 4);
    logLine("whynotfit:   rec=%p requires %d class(es)", (void*)rec, n);
    for (int i = 0; i < n && i < 8; i++) {
        uint32_t want = 0;
        char nm[96] = {0};
        if (!safeReadU32(kb + (uintptr_t)i * 4, &want)) continue;
        bldClassNameByHash(base, want, nm, sizeof nm);
        logLine("whynotfit:     [%d] 0x%08x -> \"%s\"%s", i, want, nm,
                want == mine ? "   <== MATCHES THE HERO, so the refusal is WRONG" : "");
    }
    bldDumpClassRegistries(base, 0);
}

static uint32_t g_bldUnknownClass[8];
static int      g_bldUnknownClassCount = 0;
static void bldNoteUnknownClass(uint32_t want) {
    for (int i = 0; i < g_bldUnknownClassCount; i++) if (g_bldUnknownClass[i] == want) return;
    if (g_bldUnknownClassCount >= 8) return;
    g_bldUnknownClass[g_bldUnknownClassCount++] = want;
    logLine("bldrows: class-req hash 0x%08x is in NEITHER class registry -- naming it \"%s\"",
            want, axs(AXS_BLD_CLASS_UNNAMED));
}

static void bldNoteUnknownClassAt(uintptr_t base, uint32_t want) {
    bool first = (g_bldUnknownClassCount == 0);
    int before = g_bldUnknownClassCount;
    bldNoteUnknownClass(want);
    if (first && g_bldUnknownClassCount != before) bldDumpClassRegistries(base, want);
}

void bldTrinketClassReq(uintptr_t base, uintptr_t rec, char* out, int outsz, bool probe) {
    out[0] = 0;
    uintptr_t kb = 0, ke = 0;
    if (!safeReadPtr(rec + BLD_TREC_CLSREQ_BEG, &kb) ||
        !safeReadPtr(rec + BLD_TREC_CLSREQ_END, &ke) || !kb || ke <= kb || (ke - kb) % 4) return;
    int n = (int)((ke - kb) / 4);
    if (n > 8) {
        if (probe) logLine("bldrows: implausible class-req count %d", n);
        return;
    }
    int written = 0, unnamed = 0;
    for (int i = 0; i < n; i++) {
        uint32_t want = 0;
        if (!safeReadU32(kb + (uintptr_t)i * 4, &want)) continue;
        char cls[96];
        if (!bldClassNameByHash(base, want, cls, sizeof cls)) {
            bldNoteUnknownClassAt(base, want);
            unnamed++;
        }
        if (probe) logLine("bldrows: class req[%d]=0x%08x -> \"%s\"", i, want, cls);
        if (!cls[0]) continue;
        size_t len = strlen(out);
        if (written) _snprintf(out + len, outsz - (int)len, " %s %s", axs(AXS_WORD_OR), cls);
        else         _snprintf(out + len, outsz - (int)len, "%s", cls);
        out[outsz - 1] = 0;
        written++;
    }
    if (unnamed) {
        size_t len = strlen(out);               // needs to know a restriction is there, not how
        if (written) _snprintf(out + len, outsz - (int)len, " %s %s",   // many ways it is phrased
                               axs(AXS_WORD_OR), axs(AXS_BLD_CLASS_UNNAMED));
        else         _snprintf(out + len, outsz - (int)len, "%s", axs(AXS_BLD_CLASS_UNNAMED));
        out[outsz - 1] = 0;
        written++;
    }
    if (written) {
        char list[256];
        strncpy(list, out, sizeof list - 1);
        list[sizeof list - 1] = 0;
        _snprintf(out, outsz, axs(AXS_BLD_CLASSES_ONLY_FMT), list);
        out[outsz - 1] = 0;
    }
}

bool bldTrinketFitsHero(uintptr_t base, uintptr_t item, uintptr_t hero, bool* sure) {
    if (sure) *sure = false;
    uintptr_t rec = bldTrinketRecord(base, item);
    if (!rec) return true;
    uintptr_t kb = 0, ke = 0;
    if (!safeReadPtr(rec + BLD_TREC_CLSREQ_BEG, &kb) ||
        !safeReadPtr(rec + BLD_TREC_CLSREQ_END, &ke)) return true;
    if (!kb || ke <= kb) { if (sure) *sure = true; return true; }   // no requirement: any class
    if ((ke - kb) % 4) return true;
    int n = (int)((ke - kb) / 4);
    if (n <= 0 || n > 8) return true;                               // implausible: let the game rule
    uintptr_t cls = 0;
    uint32_t mine = 0;
    if (!safeReadPtr(hero + ACTOR_HEROCLASS_OFF, &cls) || cls <= 0x10000 ||
        !safeReadU32(cls + BLD_CLASS_IDHASH_OFF, &mine) || !mine) return true;
    for (int i = 0; i < n; i++) {
        uint32_t want = 0;
        if (!safeReadU32(kb + (uintptr_t)i * 4, &want)) return true;  // partial read: don't judge
        if (want == mine) { if (sure) *sure = true; return true; }
    }
    if (sure) *sure = true;
    return false;
}

int bldTrinketEffects(uintptr_t base, uintptr_t item, char* out, int outsz, bool probe) {
    out[0] = 0;
    uintptr_t rec = bldTrinketRecord(base, item);
    if (!rec) {
        if (probe) logLine("bldrows: no trinket record for item %p", (void*)item);
        return 0;
    }
    return bldTrinketEffectsRec(base, rec, out, outsz, probe);
}

int bldTrinketEffectsRec(uintptr_t base, uintptr_t rec, char* out, int outsz, bool probe) {
    out[0] = 0;
    if (!rec) return 0;
    uintptr_t kb = 0, ke = 0;
    if (!safeReadPtr(rec + BLD_TREC_BUFFS_BEG, &kb) ||
        !safeReadPtr(rec + BLD_TREC_BUFFS_END, &ke) || !kb || ke < kb || (ke - kb) % 4) {
        if (probe) logLine("bldrows: trinket record %p buff vector unreadable", (void*)rec);
        return 0;
    }
    int nkeys = (int)((ke - kb) / 4);
    if (nkeys > 12) {
        if (probe) logLine("bldrows: trinket record %p implausible buff count %d", (void*)rec, nkeys);
        return 0;
    }
    int written = 0;
    for (int i = 0; i < nkeys; i++) {
        uint32_t key = 0;
        if (!safeReadU32(kb + (uintptr_t)i * 4, &key)) continue;
        char line[CAMP_BUFF_DESC_BUF];
        bool ok = csBuffDescByKey(base, key, nullptr, 0, line, sizeof line);
        if (probe) logLine("bldrows: trinket buff key[%d]=0x%08x -> %s\"%s\"",
                           i, key, ok ? "" : "MISS ", ok ? line : "");
        if (!ok || !line[0]) continue;
        size_t n = strlen(out);
        if ((int)n >= outsz - 4) break;
        _snprintf(out + n, outsz - (int)n, "%s%s", written ? ". " : "", line);
        out[outsz - 1] = 0;
        written++;
    }
    return written;
}

static const uintptr_t BLD_TREC_TRIGLIM_OFF   = 0x334;  // int, -1 = no trigger limit
static const uintptr_t BLD_TREC_DESTROYEXH_OFF= 0x339;  // bool destroy_on_triggers_exhausted
static const uintptr_t BLD_TREC_BLOCKTRIG_OFF = 0x33a;  // bool blocks_other_slot_trigger_expend
static const uintptr_t BLD_TREC_BLOCKQUEST_OFF= 0x33b;  // bool blocks_other_slot_quest_uses_expend
static const uintptr_t BLD_TREC_QUESTUSES_OFF = 0x33c;  // int, -1 = no quest uses
static const uintptr_t BLD_TREC_XFORM_QUEST_OFF = 0x344;  // char[0x40] transform_on_quest_complete
static const uintptr_t BLD_TREC_XFORM_KILLED_OFF= 0x388;
static const uintptr_t BLD_TREC_XFORM_EXH_OFF   = 0x3c8;
static const uintptr_t BLD_TREC_QUESTXP_OFF   = 0x408;
static const int       BLD_TRK_GROUPS      = 22;
static const uintptr_t BLD_TRK_GROUP_BASE  = 4 * 0x18;
static const uintptr_t BLD_TRK_GROUP_STRIDE= 0x18;
// ...and the table that NAMES them (BLD_TRKGRP_TABLE_RVA).
static const uintptr_t BLD_TRKGRP_STRIDE   = 0x48;
static const uintptr_t BLD_TRKGRP_HASH_OFF = 0x40;   // gameHash(name) — the read's own proof
static const uintptr_t BLD_TRKGRP_HIT_OFF  = 0x46;   // byte: this group can tell a hit from a miss
static const uintptr_t BLD_TRKGRP_PARTY_OFF= 0x47;   // byte: party-heal wording (only friendly_skill)

static bool bldTrinketCount(uintptr_t base, const char* key, int left, int max, char* out, int outsz) {
    out[0] = 0;
    char sentence[160];
    if (!abTipInt(base, key, left, sentence, sizeof sentence) || !sentence[0]) {
        logLine("bldrows: \"%s\" did not resolve — the count is omitted rather than invented", key);
        return false;
    }
    size_t n = strlen(sentence);
    while (n && (sentence[n - 1] == '.' || sentence[n - 1] == ' ')) sentence[--n] = 0;
    _snprintf(out, outsz, axs(AXS_TRK_COUNT_OF_MAX_FMT), sentence, max);
    out[outsz - 1] = 0;
    return true;
}

int bldTrinketCharges(uintptr_t base, uintptr_t item, uintptr_t rec, char* out, int outsz, bool probe) {
    out[0] = 0;
    if (!rec) return 0;
    int written = 0;
    struct { uintptr_t recOff; uintptr_t itemOff; const char* key; } kCounts[] = {
        { BLD_TREC_TRIGLIM_OFF,   ITEM_TRIG_LEFT_OFF,  "trinket_trigger_limit" },
        { BLD_TREC_QUESTUSES_OFF, ITEM_QUEST_LEFT_OFF, "trinket_quest_uses" },
    };
    for (int i = 0; i < 2; i++) {
        int32_t max = -1;
        if (!safeReadU32(rec + kCounts[i].recOff, (uint32_t*)&max) || max < 0) continue;  // no mechanic
        int32_t left = -1;
        if (!item || !safeReadU32(item + kCounts[i].itemOff, (uint32_t*)&left) || left < 0) {
            logLine("bldrows: trinket has %s max %d but its live count at item+0x%llx did not read"
                    " — omitting rather than reporting the maximum",
                    kCounts[i].key, max, (unsigned long long)kCounts[i].itemOff);
            continue;
        }
        char line[192];
        if (!bldTrinketCount(base, kCounts[i].key, left, max, line, sizeof line)) continue;
        size_t n = strlen(out);
        if ((int)n >= outsz - 4) break;
        _snprintf(out + n, outsz - (int)n, "%s%s", written ? " " : "", line);
        out[outsz - 1] = 0;
        written++;
        if (probe) logLine("bldrows: trinket %s %d of %d", kCounts[i].key, left, max);
    }
    struct { uintptr_t off; const char* key; } kFlags[] = {
        { BLD_TREC_BLOCKTRIG_OFF,  "trinket_blocks_other_slot_trigger_expend" },
        { BLD_TREC_BLOCKQUEST_OFF, "trinket_blocks_other_slot_quest_uses_expend" },
        { BLD_TREC_DESTROYEXH_OFF, "trinket_destroy_on_triggers_exhausted" },
    };
    for (int i = 0; i < 3; i++) {
        uint8_t on = 0;
        if (!safeReadU8(rec + kFlags[i].off, &on) || !on) continue;
        char line[192];
        if (!abTipPlain(base, kFlags[i].key, line, sizeof line) || !line[0]) {
            logLine("bldrows: \"%s\" did not resolve — flag omitted", kFlags[i].key);
            continue;
        }
        size_t n = strlen(out);
        if ((int)n >= outsz - 4) break;
        _snprintf(out + n, outsz - (int)n, "%s%s.", written ? " " : "", line);
        out[outsz - 1] = 0;
        written++;
    }
    return written;
}

static bool bldTrinketTransform(uintptr_t base, uintptr_t rec, uintptr_t off, const char* fmtKey,
                                char* out, int outsz) {
    out[0] = 0;
    char id[72];
    id[0] = 0;
    if (!safeReadCStr(rec + off, id, sizeof id) || !id[0]) return false;   // the game's strnlen gate
    char nkey[192], name[192];
    _snprintf(nkey, sizeof nkey, "%s%s%s", INV_TITLE_PREFIX, "trinket", id);
    nkey[sizeof nkey - 1] = 0;
    if (!abTipPlain(base, nkey, name, sizeof name) || !name[0]) {
        logLine("bldrows: transform target \"%s\" (key \"%s\") did not resolve — line omitted",
                id, nkey);
        return false;
    }
    if (!abTipStrN(base, fmtKey, name, nullptr, nullptr, out, outsz) || !out[0]) {
        logLine("bldrows: \"%s\" did not resolve — the transform into \"%s\" is not spoken",
                fmtKey, name);
        return false;
    }
    abStripMarkup(out);
    return true;
}

int bldTrinketTriggers(uintptr_t base, uintptr_t rec, char* out, int outsz, bool probe) {
    out[0] = 0;
    if (!rec) return 0;
    int written = 0;

    for (int i = 0; i < BLD_TRK_GROUPS; i++) {
        uintptr_t vec = rec + BLD_TRK_GROUP_BASE + (uintptr_t)i * BLD_TRK_GROUP_STRIDE;
        uintptr_t vb = 0, ve = 0;
        if (!safeReadPtr(vec, &vb) || !safeReadPtr(vec + 8, &ve)) continue;
        if (!vb || ve <= vb || (ve - vb) % 8) continue;                 // empty group: the common case

        uintptr_t ent = base + BLD_TRKGRP_TABLE_RVA + (uintptr_t)i * BLD_TRKGRP_STRIDE;
        char gname[72];
        gname[0] = 0;
        uint32_t want = 0;
        if (!safeReadCStr(ent, gname, sizeof gname) || !gname[0] ||
            !safeReadU32(ent + BLD_TRKGRP_HASH_OFF, &want) || resHash(gname) != want) {
            logLine("bldrows: trinket effect group %d has effects but its name did not verify"
                    " (read \"%s\", hash 0x%08x vs 0x%08x) — group skipped",
                    i, gname, resHash(gname), want);
            continue;
        }
        uint8_t hit = 0, party = 0;
        safeReadU8(ent + BLD_TRKGRP_HIT_OFF, &hit);
        safeReadU8(ent + BLD_TRKGRP_PARTY_OFF, &party);

        char stem[192], fx[AB_EFF_BUCKET_SZ * 2];
        _snprintf(stem, sizeof stem, "trinket_%s_additional_effects", gname);
        stem[sizeof stem - 1] = 0;
        abEffTrinketGroup(base, vec, stem, hit != 0, party != 0, fx, sizeof fx);
        if (probe) logLine("bldrows: trinket group %d \"%s\" (%d effects, hit=%d party=%d) -> \"%s\"",
                           i, gname, (int)((ve - vb) / 8), hit, party, fx);
        if (!fx[0]) continue;
        size_t n = strlen(out);
        if ((int)n >= outsz - 4) break;
        _snprintf(out + n, outsz - (int)n, "%s%s", written ? " " : "", fx);
        out[outsz - 1] = 0;
        written++;
    }

    int32_t xp = 0;
    if (safeReadU32(rec + BLD_TREC_QUESTXP_OFF, (uint32_t*)&xp) && xp > 0) {
        char line[192];
        if (abTipInt(base, "str_trinket_quest_complete_resolve_xp", xp, line, sizeof line) && line[0]) {
            size_t n = strlen(out);
            if ((int)n < outsz - 4) {
                _snprintf(out + n, outsz - (int)n, "%s%s.", written ? " " : "", line);
                out[outsz - 1] = 0;
                written++;
            }
        }
    }

    struct { uintptr_t off; const char* key; } kXf[] = {
        { BLD_TREC_XFORM_QUEST_OFF,  "str_trinket_transform_on_quest_complete" },
        { BLD_TREC_XFORM_KILLED_OFF, "str_trinket_transform_on_wearer_killed" },
        { BLD_TREC_XFORM_EXH_OFF,    "str_trinket_transform_on_trigger_limit_exhausted" },
    };
    for (int i = 0; i < 3; i++) {
        char line[256];
        if (!bldTrinketTransform(base, rec, kXf[i].off, kXf[i].key, line, sizeof line)) continue;
        size_t n = strlen(out);
        if ((int)n >= outsz - 4) break;
        _snprintf(out + n, outsz - (int)n, "%s%s.", written ? " " : "", line);
        out[outsz - 1] = 0;
        written++;
    }
    return written;
}

// ---- the record questions the raid's trigger watcher asks ----

bool bldTrinketRecName(uintptr_t base, uintptr_t rec, char* out, int outsz) {
    out[0] = 0;
    if (!rec) return false;
    char id[72];
    id[0] = 0;
    if (!safeReadCStr(rec + BLD_TREC_ID_STR_OFF, id, sizeof id) || !id[0]) return false;
    uint32_t want = 0;
    if (!safeReadU32(rec + BLD_TREC_HASH_OFF, &want) || resHash(id) != want) {
        logLine("bldrows: record id \"%s\" hashes 0x%08x but the record says 0x%08x — not naming it",
                id, resHash(id), want);
        return false;
    }
    char nkey[192];
    _snprintf(nkey, sizeof nkey, "%s%s%s", INV_TITLE_PREFIX, "trinket", id);
    nkey[sizeof nkey - 1] = 0;
    if (abTipPlain(base, nkey, out, outsz) && out[0]) return true;
    strncpy(out, id, outsz - 1);
    out[outsz - 1] = 0;
    for (char* p = out; *p; p++) if (*p == '_') *p = ' ';
    return true;
}

bool bldTrinketTriggerLine(uintptr_t base, uintptr_t item, uintptr_t rec, char* out, int outsz) {
    out[0] = 0;
    if (!rec || !item) return false;
    int32_t max = -1, left = -1;
    if (!safeReadU32(rec + BLD_TREC_TRIGLIM_OFF, (uint32_t*)&max) || max < 0) return false;
    if (!safeReadU32(item + ITEM_TRIG_LEFT_OFF, (uint32_t*)&left) || left < 0) return false;
    return bldTrinketCount(base, "trinket_trigger_limit", left, max, out, outsz);
}

bool bldTrinketExhaustTransform(uintptr_t base, uintptr_t rec, uint32_t intoIdHash,
                                char* nameOut, int nameOutSz) {
    if (nameOut && nameOutSz > 0) nameOut[0] = 0;
    if (!rec || !intoIdHash) return false;
    char id[72];
    id[0] = 0;
    if (!safeReadCStr(rec + BLD_TREC_XFORM_EXH_OFF, id, sizeof id) || !id[0]) return false;
    if (resHash(id) != intoIdHash) return false;
    if (!nameOut || nameOutSz <= 0) return true;
    char nkey[192];
    _snprintf(nkey, sizeof nkey, "%s%s%s", INV_TITLE_PREFIX, "trinket", id);
    nkey[sizeof nkey - 1] = 0;
    if (!abTipPlain(base, nkey, nameOut, nameOutSz) || !nameOut[0]) {
        logLine("bldrows: transform target \"%s\" did not resolve — the trinket is named by its id",
                id);
        strncpy(nameOut, id, nameOutSz - 1);
        nameOut[nameOutSz - 1] = 0;
        for (char* p = nameOut; *p; p++) if (*p == '_') *p = ' ';
    }
    return true;
}

static void bldStoreCurrencyName(uintptr_t base, int kind, char* out, int outsz) {
    resCurrencyTitleById(base, kind == BLD_STORE_COMET ? "shard" : "gold",
                         "bldrows", out, outsz);
}

static void bldJewelerPurseLine(uintptr_t base, uintptr_t panel, int col, char* out, int outsz) {
    out[0] = 0;
    uintptr_t items[16];
    int n = bldItemDisplays(panel, items, 16);
    if (col < 0 || col >= n) return;
    if (bldStoreKindOf(base, items[col]) != BLD_STORE_COMET) return;
    int have = 0;
    resWalletAmount(base, resHash("shard"), &have);   // absent wallet entry -> 0, still spoken
    char cur[96];
    bldStoreCurrencyName(base, BLD_STORE_COMET, cur, sizeof cur);
    _snprintf(out, outsz, axs(AXS_JW_PURSE_FMT), have, cur);
    out[outsz - 1] = 0;
}

static bool bldStoreRowText(uintptr_t base, uintptr_t sys, int kind, int slot, int pos, int total,
                            char* out, int outsz, char* nameOut, int nameOutSz, int* priceOut) {
    uintptr_t beg = 0; int slots = 0;
    if (!invItemVectorAt(sys, &beg, &slots) || slot < 0 || slot >= slots) return false;
    uintptr_t item = beg + (uintptr_t)slot * ITEM_STRIDE;
    char type[64], itemId[64], key[192], name[256];
    invItemName(base, item, type, itemId, key, name);
    if (nameOut && nameOutSz > 0) {
        strncpy(nameOut, name, nameOutSz - 1);
        nameOut[nameOutSz - 1] = 0;
    }
    int price = 0;
    bool havePrice = bldTrinketPriceOf(base, item, itemId, kind == BLD_STORE_COMET, &price);
    if (priceOut) *priceOut = havePrice ? price : -1;
    char currency[96];
    bldStoreCurrencyName(base, kind, currency, sizeof currency);
    char rarity[96], clsreq[256], charges[384], trig[1536];
    rarity[0] = clsreq[0] = charges[0] = trig[0] = 0;
    uintptr_t rec = bldTrinketRecord(base, item);
    if (rec) {
        bldTrinketRarity(base, rec, rarity, sizeof rarity, false);
        bldTrinketClassReq(base, rec, clsreq, sizeof clsreq, false);
        bldTrinketCharges(base, item, rec, charges, sizeof charges, false);
        bldTrinketTriggers(base, rec, trig, sizeof trig, false);
    }
    char fx[768];
    bldTrinketEffects(base, item, fx, sizeof fx, false);
    char desckey[192], desc[512];
    _snprintf(desckey, sizeof desckey, "%s%s%s", INV_DESC_PREFIX, type, itemId);
    desckey[sizeof desckey - 1] = 0;
    bool hasDesc = resolveKey(base, desckey, desc, sizeof desc) && desc[0];
    char head[1024];
    _snprintf(head, sizeof head, "%s%s%s%s%s%s%s", name,
              rarity[0] ? ". " : "", rarity,
              clsreq[0] ? ". " : "", clsreq,
              charges[0] ? ". " : "", charges);
    head[sizeof head - 1] = 0;
    char itemPos[64];
    _snprintf(itemPos, sizeof itemPos, axs(AXS_ITEM_N_OF_M), pos, total);
    itemPos[sizeof itemPos - 1] = 0;
    if (havePrice) {
        char priceS[128];                             // period-less: the ". " joins supply it
        _snprintf(priceS, sizeof priceS, axs(AXS_SHOP_COSTS_FMT), price, currency);
        priceS[sizeof priceS - 1] = 0;
        _snprintf(out, outsz, "%s. %s%s%s%s%s. %s%s%s",
                  head, priceS,
                  fx[0] ? ". " : "", fx,
                  trig[0] ? ". " : "", trig,
                  itemPos,
                  hasDesc ? " " : "", hasDesc ? desc : "");
    } else {
        _snprintf(out, outsz, "%s%s%s%s%s. %s%s%s",
                  head, fx[0] ? ". " : "", fx,
                  trig[0] ? ". " : "", trig,
                  itemPos,
                  hasDesc ? " " : "", hasDesc ? desc : "");
    }
    out[outsz - 1] = 0;
    return true;
}

static void bldActProbe(uintptr_t base, uintptr_t panel);
static void bldGraveProbe(uintptr_t base, uintptr_t panel);
static void haProbe(uintptr_t base, uintptr_t panel);
static void bldRowsProbe(uintptr_t base, uintptr_t panel) {
    if (!axDebugLogEnabled()) return;                // diagnostic: nothing runs with the log off
    uintptr_t sys = 0;
    int storeKind = BLD_STORE_NONE;
    int kind = bldRowKindOf(base, panel, &sys, &storeKind);
    logLine("bldrows probe: \"%s\" kind=%d sys=%p storeKind=%d", g_bldId, kind, (void*)sys, storeKind);
    if (dgIsPanel(base, panel)) dgProbe(base, panel);
    if (pbIsPanel(base, panel)) pbProbe(base, panel);
    if (rbIsPanel(base, panel)) rbProbe(base, panel);
    if (bdIsPanel(base, panel)) bdProbe(base, panel);
    {
        uintptr_t items[16];
        int items_n = bldItemDisplays(panel, items, 16);
        uint32_t idx = 0xffffffffu;
        safeReadU32(panel + BLD_ITEM_INDEX_OFF, &idx);
        uintptr_t swapAnim = 0;
        safeReadPtr(panel + BLD_SWAP_ANIM_OFF, &swapAnim);
        logLine("bldcols probe: %d item display(s) = column(s), showing index %u, swapAnim=%p",
                items_n, idx, (void*)swapAnim);
        for (int i = 0; i < items_n; i++) {
            uintptr_t vft = 0;
            safeReadPtr(items[i], &vft);
            char who[128];
            bldColumnName(base, panel, i, who, sizeof who);
            logLine("bldcols probe:   [%d] %p vft=+0x%llx store=%d recruit=%d \"%s\"%s",
                    i, (void*)items[i], (unsigned long long)(vft > base ? vft - base : 0),
                    bldStoreKindOf(base, items[i]), bldRecruitKindOf(base, items[i]), who,
                    (uint32_t)i == idx ? "  <- SHOWING" : "");
        }
        uintptr_t comet = bldCometPanelOf(base, panel);
        if (comet) {
            uint32_t cur = 0, tgt = 0;
            safeReadU32(comet + BLD_CTSD_SCROLL_CUR, &cur);
            safeReadU32(comet + BLD_CTSD_SCROLL_TGT, &tgt);
            logLine("bldcols probe:   comet panel %p scroll current=%.1f target=%.1f",
                    (void*)comet, u32AsFloatM(cur), u32AsFloatM(tgt));
        }
    }
    if (kind == 1) {
        int slots[64];
        int n = bldStoreRows(sys, slots, 64);
        for (int i = 0; i < n; i++) {
            uintptr_t beg = 0; int total = 0;
            if (!invItemVectorAt(sys, &beg, &total)) break;
            uintptr_t item = beg + (uintptr_t)slots[i] * ITEM_STRIDE;
            char type[64], itemId[64], key[192], name[256];
            invItemName(base, item, type, itemId, key, name);
            int price = 0;
            bool hp = bldTrinketPriceOf(base, item, itemId, storeKind == BLD_STORE_COMET, &price);
            uintptr_t elem = feGetElementById((int64_t)(uint32_t)(INV_FOURCC_BASE + (uint32_t)i));
            float ex = 0, ey = 0;
            bool haveXY = elem && elemCenter(elem, &ex, &ey);
            logLine("bldrows probe: row %d slot %d id=\"%s\" name=\"%s\" price=%s%d elem=0x%x"
                    " at %s(%.0f,%.0f)",
                    i, slots[i], itemId, name, hp ? "" : "MISS ", hp ? price : 0,
                    (uint32_t)(INV_FOURCC_BASE + (uint32_t)i),
                    haveXY ? "" : "UNREGISTERED ", ex, ey);
            char fx[768];
            int nfx = bldTrinketEffects(base, item, fx, sizeof fx, true);
            logLine("bldrows probe: row %d effects=%d \"%s\"", i, nfx, fx);
            uintptr_t rec = bldTrinketRecord(base, item);
            char rarity[96] = {0}, clsreq[256] = {0};
            if (rec) {
                bldTrinketRarity(base, rec, rarity, sizeof rarity, true);
                bldTrinketClassReq(base, rec, clsreq, sizeof clsreq, true);
            }
            logLine("bldrows probe: row %d rec=%p rarity=\"%s\" classreq=\"%s\"",
                    i, (void*)rec, rarity, clsreq);
            if (rec) {
                char charges[384] = {0}, trig[1536] = {0};
                int nch = bldTrinketCharges(base, item, rec, charges, sizeof charges, true);
                int ntr = bldTrinketTriggers(base, rec, trig, sizeof trig, true);
                logLine("bldrows probe: row %d charges=%d \"%s\" triggers=%d \"%s\"",
                        i, nch, charges, ntr, trig);
            }
        }
    } else if (kind == 2) {
        BldRecruit rows[16];
        uintptr_t disp = 0;
        int rctKind = BLD_RCT_NONE;
        bldRecruitDisplay(base, panel, &rctKind);
        int n = bldRecruitRows(base, panel, rows, 16, &disp);
        logLine("bldrows probe: showing pool %p kind=%d (%s)", (void*)disp, rctKind,
                rctKind == BLD_RCT_SHARD ? "shard mercenaries" : "the building's own");
        for (int i = 0; i < n; i++) {
            char name[80] = {0};
            safeReadCStr(rows[i].hero + HERO_NAME_OFF, name, sizeof name);
            uint32_t elemId = (uint32_t)(BLD_ELEM_RCT_BASE + (uint32_t)rows[i].slot);
            uintptr_t elem = feGetElementById((int64_t)elemId);
            float ex = 0, ey = 0;
            bool haveXY = elem && elemCenter(elem, &ex, &ey);
            logLine("bldrows probe: row %d slot %d hero=%p \"%s\" elem=0x%x at %s(%.0f,%.0f)",
                    i, rows[i].slot, (void*)rows[i].hero, name, elemId,
                    haveXY ? "" : "UNREGISTERED ", ex, ey);
        }
    } else if (kind == 3) {
        bldActProbe(base, panel);
    } else if (kind == 5) {
        bldGraveProbe(base, panel);                   // the graveyard's dead-hero elements + text
    } else if (kind == 6) {
        haProbe(base, panel);
    }
}

typedef void (*BldInspectFn)(void* closure, uintptr_t heroArg, uint32_t* slotIdx);
static bool bldSehInspectRecruit(uintptr_t base, uintptr_t disp, uintptr_t hero, uint32_t slotIdx) {
    struct { uintptr_t vft; uintptr_t display; } closure;
    closure.vft = base + BLD_RCT_INSPECT_VFT;
    closure.display = disp;
    __try {
        ((BldInspectFn)(base + BLD_RCT_INSPECT_RVA))(&closure, hero, &slotIdx);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

static bool bldActFocusedRowText(uintptr_t base, uintptr_t panel, char* row, int rowsz,
                                 bool withWing);
// ---- The statue row model: one flat list of UNLOCKED media, cinematics first ----
struct StRow {
    int       type;       // 0 video / 1 narration / 2 journal page
    int       buildIndex;
    uintptr_t rec;
    char      text[224];  // record+0x44, markup stripped
};

static StRow g_stRows[256];
static int   g_stRowCount = 0;
static int   g_stRow = 0;          // the single row cursor

static const char* stTypeWord(int type) {           // SPOKEN words (axs); the logs use the int
    switch (type) {
        case 0: return axs(AXS_ST_TYPE_CINEMATIC);
        case 1: return axs(AXS_ST_TYPE_NARRATION);
        case 2: return axs(AXS_ST_TYPE_JOURNAL);
    }
    return axs(AXS_ST_TYPE_ENTRY);
}

static int stBuildModel(uintptr_t base, uintptr_t panel) {
    (void)base;
    g_stRowCount = 0;
    uintptr_t cb = 0, ce = 0;
    if (!safeReadPtr(panel + ST_CAT_VEC_BEG, &cb) || !safeReadPtr(panel + ST_CAT_VEC_END, &ce) ||
        !cb || ce <= cb || (ce - cb) % ST_CAT_STRIDE) return 0;
    int ncat = (int)((ce - cb) / ST_CAT_STRIDE);
    if (ncat <= 0 || ncat > 32) return 0;
    int buildIdx = 0;
    for (int ci = 0; ci < ncat; ci++) {
        uintptr_t cat = cb + (uintptr_t)ci * ST_CAT_STRIDE;
        uintptr_t eb = 0, ee = 0;
        if (!safeReadPtr(cat + ST_CAT_ENT_BEG, &eb) || !safeReadPtr(cat + ST_CAT_ENT_END, &ee) ||
            !eb || ee <= eb || (ee - eb) % ST_ENT_STRIDE) continue;
        int nent = (int)((ee - eb) / ST_ENT_STRIDE);
        if (nent <= 0 || nent > 256) continue;
        for (int ei = 0; ei < nent; ei++) {
            uintptr_t ent = eb + (uintptr_t)ei * ST_ENT_STRIDE;
            uintptr_t rb = 0, re = 0;
            if (!safeReadPtr(ent + ST_ENT_REC_BEG, &rb) || !safeReadPtr(ent + ST_ENT_REC_END, &re) ||
                !rb || re <= rb || (re - rb) % ST_REC_STRIDE) continue;
            int nrec = (int)((re - rb) / ST_REC_STRIDE);
            if (nrec <= 0 || nrec > 256) continue;
            for (int ri = 0; ri < nrec; ri++, buildIdx++) {
                uintptr_t rec = rb + (uintptr_t)ri * ST_REC_STRIDE;
                uint8_t lock = 0;
                safeReadU8(rec + ST_REC_LOCK_OFF, &lock);
                if (lock == 0) continue;               // hidden: not unlocked yet (build index still ++)
                if (g_stRowCount >= 256) continue;
                uint32_t type = 0;
                safeReadU32(rec + ST_REC_TYPE_OFF, &type);
                char raw[224] = {0};
                safeReadCStr(rec + ST_REC_TEXT_OFF, raw, sizeof raw);
                StRow* r = &g_stRows[g_stRowCount];
                r->type = (int)type;
                r->buildIndex = buildIdx;
                r->rec = rec;
                stripMarkup(raw, r->text, sizeof r->text);
                if (!r->text[0]) { strncpy(r->text, axs(AXS_ST_UNTITLED), sizeof r->text - 1); r->text[sizeof r->text - 1] = 0; }
                g_stRowCount++;
            }
        }
    }
    for (int i = 1; i < g_stRowCount; i++) {
        StRow key = g_stRows[i];
        int j = i - 1;
        while (j >= 0 && g_stRows[j].type > key.type) { g_stRows[j + 1] = g_stRows[j]; j--; }
        g_stRows[j + 1] = key;
    }
    return g_stRowCount;
}

// One row line: "<title>. <Cinematic/Narration/Journal page>. Item i of N."
static void stComposeRow(int rowIdx, char* out, int outsz) {
    const StRow* r = &g_stRows[rowIdx];
    char itemPos[64];
    _snprintf(itemPos, sizeof itemPos, axs(AXS_ITEM_N_OF_M), rowIdx + 1, g_stRowCount);
    itemPos[sizeof itemPos - 1] = 0;
    _snprintf(out, outsz, "%s. %s. %s", r->text, stTypeWord(r->type), itemPos);
    out[outsz - 1] = 0;
}

static bool stSpeakFocused(uintptr_t base, uintptr_t panel, const char* prefix) {
    int n = stBuildModel(base, panel);
    if (n <= 0) return false;
    if (g_stRow >= n) g_stRow = n - 1;
    if (g_stRow < 0)  g_stRow = 0;
    char row[MAILBOX_SZ];
    stComposeRow(g_stRow, row, sizeof row);
    char utter[MAILBOX_SZ];
    _snprintf(utter, sizeof utter, "%s%s", prefix ? prefix : "", row);
    utter[sizeof utter - 1] = 0;
    postSpeech(utter);
    return true;
}

typedef void (*StatueDispatchFn)(uintptr_t display, uintptr_t record);

static bool stActivateRow(uintptr_t base, uintptr_t panel, int rowIdx) {
    if (rowIdx < 0 || rowIdx >= g_stRowCount) return false;
    uintptr_t rec = g_stRows[rowIdx].rec;
    if (!panel || rec <= 0x10000) {
        logLine("⚠ statue: row %d has no record (panel=%p rec=%p)",
                rowIdx, (void*)panel, (void*)rec);
        return false;
    }
    logLine("statue: activating row %d (type %d buildIndex %d rec=%p) \"%s\"",
            rowIdx, g_stRows[rowIdx].type, g_stRows[rowIdx].buildIndex, (void*)rec,
            g_stRows[rowIdx].text);
    __try {
        ((StatueDispatchFn)(base + ST_DISPATCH_RVA))(panel, rec);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        logLine("⚠ statue: the dispatch faulted for row %d", rowIdx);
        return false;
    }
    return true;
}

DWORD g_stJrnWatchUntil = 0;                  // 0 = no journal open is pending
static const DWORD ST_JRN_WAIT_MS = 1500;

// ---- The GRAVEYARD (the Ancestor's memorial to the fallen) ----
static const uintptr_t GY_ROWS_OFF       = 0x290;

static bool bldIsGraveyard(uintptr_t base, uintptr_t panel) {
    uintptr_t vft = 0;
    if (!panel || !safeReadPtr(panel, &vft) || vft <= base) return false;
    return vft - base == BLD_GRAVE_VFT_RVA;
}

struct GyRow {
    int64_t elemId;
    char    text[MAILBOX_SZ];   // joined memorial text (name. week. story.)
};
static GyRow g_gyRows[128];
static int   g_gyRowCount = 0;
static int   g_gyRow = 0;

static bool gyEntryText(uintptr_t base, uintptr_t widget, char* out, int outsz) {
    char frags[TL_FRAGS_MAX][TL_ROW_MAX];
    int nfrags = 0, budget = 4000;
    tlGather(base, widget, 0, &budget, frags, &nfrags, false);
    out[0] = 0;
    int used = 0;
    for (int i = 0; i < nfrags && used < outsz - 1; i++) {
        if (!frags[i][0]) continue;
        // strip a trailing '.' the fragment may already carry, then re-add exactly one.
        char frag[TL_ROW_MAX];
        strncpy(frag, frags[i], sizeof frag - 1);
        frag[sizeof frag - 1] = 0;
        int fl = (int)strlen(frag);
        while (fl > 0 && (frag[fl - 1] == '.' || frag[fl - 1] == ' ')) frag[--fl] = 0;
        if (!frag[0]) continue;
        int n = _snprintf(out + used, outsz - used, "%s%s.", used ? " " : "", frag);
        if (n <= 0) break;
        used += n;
    }
    out[outsz - 1] = 0;
    return out[0] != 0;
}

static int gyBuildModel(uintptr_t base, uintptr_t panel) {
    g_gyRowCount = 0;
    uintptr_t container = 0;
    if (!panel || !safeReadPtr(panel + GY_ROWS_OFF, &container) || !tlLooksLikeWidget(base, container))
        return 0;
    uintptr_t beg = 0, end = 0;
    if (!safeReadPtr(container + TL_KIDS_BEG_OFF, &beg) ||
        !safeReadPtr(container + TL_KIDS_END_OFF, &end) || end < beg) return 0;
    int cells = (int)((end - beg) >> 3);
    if (cells > TL_WALK_KIDS_MAX) cells = TL_WALK_KIDS_MAX;
    for (int c = 0; c < cells && g_gyRowCount < 128; c++) {
        uintptr_t cell = 0;
        if (!safeReadPtr(beg + (uintptr_t)c * 8, &cell) || !tlLooksLikeWidget(base, cell)) continue;
        char text[MAILBOX_SZ];
        if (!gyEntryText(base, cell, text, sizeof text)) continue;    // separator / image-only cell
        g_gyRows[g_gyRowCount].elemId = (int64_t)cell;
        strncpy(g_gyRows[g_gyRowCount].text, text, MAILBOX_SZ - 1);
        g_gyRows[g_gyRowCount].text[MAILBOX_SZ - 1] = 0;
        g_gyRowCount++;
    }
    return g_gyRowCount;
}

static void gyComposeRow(int rowIdx, char* out, int outsz) {
    char pos[64];
    _snprintf(pos, sizeof pos, axs(AXS_GY_GRAVE_POS_FMT), rowIdx + 1, g_gyRowCount);
    pos[sizeof pos - 1] = 0;
    _snprintf(out, outsz, "%s %s", g_gyRows[rowIdx].text, pos);
    out[outsz - 1] = 0;
}

static bool gySpeakFocused(uintptr_t base, uintptr_t panel, const char* prefix) {
    int n = gyBuildModel(base, panel);
    if (n <= 0) return false;
    if (g_gyRow >= n) g_gyRow = n - 1;
    if (g_gyRow < 0)  g_gyRow = 0;
    char row[MAILBOX_SZ], utter[MAILBOX_SZ];
    gyComposeRow(g_gyRow, row, sizeof row);
    _snprintf(utter, sizeof utter, "%s%s", prefix ? prefix : "", row);
    utter[sizeof utter - 1] = 0;
    postSpeech(utter);
    return true;
}

static void bldGraveProbe(uintptr_t base, uintptr_t panel) {
    uintptr_t container = 0;
    bool okc = safeReadPtr(panel + GY_ROWS_OFF, &container);
    int n = gyBuildModel(base, panel);
    logLine("graveyard probe: panel=%p container(+0x%llx)=%p ok=%d -> %d memorial rows",
            (void*)panel, (unsigned long long)GY_ROWS_OFF, (void*)container, okc ? 1 : 0, n);
    for (int i = 0; i < n; i++)
        logLine("graveyard probe: row %d cell=0x%llx \"%.240s\"",
                i, (unsigned long long)g_gyRows[i].elemId, g_gyRows[i].text);
    if (!okc) return;
    if (!tlLooksLikeWidget(base, container)) {                        // offset wrong? show the head
        uintptr_t q[6] = { 0 };
        for (int i = 0; i < 6; i++) safeReadPtr(container + (uintptr_t)i * 8, &q[i]);
        logLine("graveyard probe: container not a widget; raw qwords %p %p %p %p %p %p",
                (void*)q[0], (void*)q[1], (void*)q[2], (void*)q[3], (void*)q[4], (void*)q[5]);
        return;
    }
    uintptr_t beg = 0, end = 0;
    if (!safeReadPtr(container + TL_KIDS_BEG_OFF, &beg) ||
        !safeReadPtr(container + TL_KIDS_END_OFF, &end) || end < beg) return;
    int cells = (int)((end - beg) >> 3);
    logLine("graveyard probe: container holds %d cell(s)", cells);
    static char frags[TL_FRAGS_MAX][TL_ROW_MAX];
    for (int c = 0; c < cells && c < 3; c++) {                        // first few trees = enough
        uintptr_t cell = 0;
        if (!safeReadPtr(beg + (uintptr_t)c * 8, &cell)) continue;
        int nf = 0, budget = TL_WALK_NODES_MAX;
        logLine("graveyard probe: -- cell %d @%p --", c, (void*)cell);
        tlGather(base, cell, 1, &budget, frags, &nf, true);
    }
}

static bool haSpeakCell(uintptr_t base, uintptr_t panel, const char* prefix, bool sayHero);
static bool bldSpeakOptionRow(uintptr_t base, uintptr_t panel, const char* prefix) {
    uintptr_t sys = 0;
    int storeKind = BLD_STORE_NONE;
    int kind = bldRowKindOf(base, panel, &sys, &storeKind);
    if (kind == 0) return false;
    if (kind == 6) return haSpeakCell(base, panel, prefix, true);  // the hero/upgrade table
    if (kind == 5) return gySpeakFocused(base, panel, prefix);   // the graveyard's dead-hero list
    if (kind == 4) return stSpeakFocused(base, panel, prefix);   // the statue's single media list
    char row[MAILBOX_SZ];
    row[0] = 0;
    if (kind == 3) {
        if (!bldActFocusedRowText(base, panel, row, sizeof row, true))
            _snprintf(row, sizeof row, "%s", axs(AXS_ACT_NONE_READ));
    } else if (kind == 1) {
        int slots[64];
        int n = bldStoreRows(sys, slots, 64);
        axStepCursor(&g_bldRow, n, 0);       // re-clamp against the live rows
        if (n <= 0) {
            _snprintf(row, sizeof row, "%s", axs(AXS_SHOP_NOTHING));
        } else if (!bldStoreRowText(base, sys, storeKind, slots[g_bldRow], g_bldRow + 1, n,
                                    row, sizeof row, nullptr, 0, nullptr)) {
            _snprintf(row, sizeof row, "%s", axs(AXS_SHOP_UNREADABLE));
        }
        if (n > 0) bldCometScrollToRow(base, panel, g_bldRow);
    } else {
        BldRecruit rows[16];
        int n = bldRecruitRows(base, panel, rows, 16, nullptr);
        axStepCursor(&g_bldRow, n, 0);       // re-clamp against the live rows
        if (n <= 0) {
            _snprintf(row, sizeof row, "%s", axs(AXS_RCT_NONE));
        } else {
            char card[512], pos[64];
            ptyHeroFrag(base, rows[g_bldRow].hero, card, sizeof card);
            _snprintf(pos, sizeof pos, axs(AXS_RCT_POS_FMT), g_bldRow + 1, n);
            pos[sizeof pos - 1] = 0;
            _snprintf(row, sizeof row, "%s. %s", card, pos);
        }
    }
    row[sizeof row - 1] = 0;
    char utter[MAILBOX_SZ];
    _snprintf(utter, sizeof utter, "%s%s", prefix ? prefix : "", row);
    utter[sizeof utter - 1] = 0;
    postSpeech(utter);
    return true;
}

static int  bldContagionLevel(uintptr_t base, uintptr_t panel);
static bool bldIsActivityBuildingId(const char* id);
static void bldContagionText(uintptr_t base, int level, char* out, int outsz);

static void bldSpeakHere(uintptr_t base) {
    char utter[MAILBOX_SZ];
    uintptr_t root = resTownRoot(base);
    uintptr_t panel = root ? bldOpenPanel(root) : 0;
    if (g_bldMode == 1) {
        if (panel) {
            char head[128], prefix[160];
            _snprintf(head, sizeof head, axs(AXS_BLD_UPGRADES_FMT), g_bldName);
            head[sizeof head - 1] = 0;
            _snprintf(prefix, sizeof prefix, "%s ", head);
            prefix[sizeof prefix - 1] = 0;
            bldSpeakTrackRow(base, root, panel, prefix);
            return;
        }
        _snprintf(utter, sizeof utter, axs(AXS_BLD_UPGRADES_FMT), g_bldName);
    } else {
        if (panel) {
            int showing = 0;
            int cols = bldColumnCount(panel, &showing);
            char merch[256];
            merch[0] = 0;
            if (cols > 1 && showing > 0) {
                char here[128];
                bldColumnName(base, panel, showing, here, sizeof here);
                char purse[128];
                bldJewelerPurseLine(base, panel, showing, purse, sizeof purse);
                if (purse[0]) _snprintf(merch, sizeof merch, "%s. %s ", here, purse);
                else          _snprintf(merch, sizeof merch, "%s. ", here);
                merch[sizeof merch - 1] = 0;
            }
            char cg[256];
            cg[0] = 0;
            if (bldIsActivityBuildingId(g_bldId)) {
                int lv = bldContagionLevel(base, panel);
                if (lv > 0) {
                    bldContagionText(base, lv, cg, sizeof cg);
                    if (cg[0]) logLine("bldact: arrival contagion level %d (\"%s\")", lv, g_bldId);
                }
            }
            char head[160], prefix[768];
            _snprintf(head, sizeof head, axs(AXS_BLD_SCREEN_FMT), g_bldName);
            head[sizeof head - 1] = 0;
            _snprintf(prefix, sizeof prefix, "%s %s%s%s", head, cg, cg[0] ? " " : "", merch);
            prefix[sizeof prefix - 1] = 0;
            if (bldSpeakOptionRow(base, panel, prefix)) return;

            if (dgSpeakArrival(base, panel, prefix)) return;
            if (pbSpeakArrival(base, panel, prefix)) return;
            if (rbSpeakArrival(base, panel, prefix)) return;
            if (bdSpeakArrival(base, panel, prefix)) return;
        }
        _snprintf(utter, sizeof utter, axs(AXS_BLD_SCREEN_FMT), g_bldName);
    }
    utter[sizeof utter - 1] = 0;
    postSpeech(utter);
}

void bldReannounce(uintptr_t base) {
    if (!g_bldActive) return;
    logLine("building: re-announcing after a modal closed");
    bldSpeakHere(base);
}

// ---- ROUND 4: the recruit ACTION — a synthesized DRAG ----
int              g_bldRecDrag = 0;
static uintptr_t g_bldRecHero = 0;            // the recruit the virtual button holds
static int       g_bldRecSlot = -1;           // its coach slot ordinal (element = base + slot)
static char      g_bldRecName[64];            // spoken name, read once at focus time
static DWORD     g_bldRecWatchUntil = 0;      // outcome watch; 0 = idle
static int       g_bldRecEntriesBefore = -1;  // roster entry-vector count before the drop

typedef int (*RctIntFn)(void);
bool bldSehRosterCounts(uintptr_t base, int* count, int* max) {
    __try {
        *count = ((RctIntFn)(base + RCT_COUNT_RVA))();
        *max   = ((RctIntFn)(base + RCT_MAX_RVA))();
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

int bldRosterEntryCount(uintptr_t base) {
    uintptr_t campaign = 0, beg = 0, end = 0;
    if (!safeReadPtr(base + RES_CAMPAIGN_RVA, &campaign) || campaign <= 0x10000) return -1;
    if (!safeReadPtr(campaign + 0x20, &beg) || !safeReadPtr(campaign + 0x28, &end)) return -1;
    if (end < beg || end - beg > 0x8000) return -1;
    return (int)((end - beg) >> 3);
}

static bool bldStartRecruitDrag(uintptr_t base, int slotOrdinal) {
    if (clickQueued()) {                              // another click is already in flight
        logLine("bldrows: recruit drag REFUSED, a click is already queued");
        logInputSnapshot(base, "recruit-clickqueued");
        return false;
    }
    uintptr_t src = feGetElementById((int64_t)(BLD_ELEM_RCT_BASE + (uint32_t)slotOrdinal));
    float sx = 0, sy = 0;
    if (!src || !elemCenter(src, &sx, &sy)) {
        logLine("bldrows: recruit drag, slot element 0x%x not on screen",
                BLD_ELEM_RCT_BASE + (uint32_t)slotOrdinal);
        logInputSnapshot(base, "recruit-noslot");
        diagDumpFocusElements(base, "recruit-noslot");
        return false;
    }
    float tx = 0, ty = 0;
    int64_t dropId = 0;
    uintptr_t drop = feFindElementByFamily(base, BLD_ROSTER_ELEM_FAMILY,
                                           BLD_ROSTER_OWNER_TAG, &dropId);
    bool found = drop && elemCenter(drop, &tx, &ty);
    if (!found) {
        logLine("bldrows: recruit drag, no roster sidebar element on screen");
        logInputSnapshot(base, "recruit-noroster");
        diagDumpFocusElements(base, "recruit-noroster");
        return false;
    }
    if (axDebugLogEnabled()) logInputSnapshot(base, "recruit-drag-start");
    if (!synthDragPoints(sx, sy, tx, ty, "coach recruit")) return false;
    logLine("bldrows: recruit drag slot %d (%.0f,%.0f) -> roster 0x%llx (%.0f,%.0f)",
            slotOrdinal, sx, sy, (unsigned long long)dropId, tx, ty);
    return true;
}

// ---- ROUND 5: the ACTIVITY WINGS (Abbey / Tavern) ----
static const uintptr_t BLD_ICD_ITEMS_BEG    = 0x260;
static const uintptr_t BLD_ICD_ITEMS_END    = 0x268;     // ... end (vector<ItemDisplay*>) (was 0x270)
static const uintptr_t BLD_ACTD_ACT_OFF     = 0x80;      // activity display+: Building::Activity*
static const uintptr_t BLD_ACTD_SLOTS_BEG   = 0x90;      // activity display+: HeroSlot* vector begin
static const uintptr_t BLD_ACTD_SLOTS_END   = 0x98;      // ... end
static const uintptr_t BLD_ACT_ID_OFF       = 0x20;      // activity+: id string, inline ("meditation")
static const uintptr_t BLD_ACT_SLOTREC_BEG  = 0x78;      // activity+: slot records begin, stride 0x18:
static const uintptr_t BLD_ACT_SLOTREC_END  = 0x80;      //   {heroGuid @0, taken @4, cookie @8,
static const uintptr_t BLD_ACT_SLOTREC_STRIDE = 0x18;    //    occupantType @0x10, committed @0x14}
static const uintptr_t BLD_SLOT_ELEM_OFF    = 0x88;      // HeroSlot+: its FocusElement id
static const uintptr_t BLD_SLOT_OCCUPANT_OFF= 0x2f0;
                                                         //   a town event can park the caretaker)
static const uint32_t  BLD_ELEM_CNCL_TAG    = 0x636e636c;// committed slot's cancel: actBase+tag+idx
static const uint32_t  BLD_ELEM_CNFM_ID     = 0x636e666d;// the confirm checkmark's id constant as
// ---- WHY a hero is refused an activity slot ----
static const uintptr_t ACT_REQ_QUIRKS_BEG    = 0x50;     // NotHaveQuirks+: blocked-quirk id vector
static const uintptr_t ACT_REQ_QUIRKS_END    = 0x58;     //   begin/end — confirmed twice: the
static const uintptr_t ACT_REQ_QUIRK_STRIDE  = 0x40;     // ... entries are inline char ids
static const uintptr_t ACT_LEDGER_OFF       = 0xe50;     // Campaign+: the currency ledger the game
static const uintptr_t ACT_RL_OFF           = 0x2f48;
static const uintptr_t ACT_RL_ROWS_BEG      = 0xa0;
static const uintptr_t ACT_RL_ROWS_END      = 0xa8;
static const uintptr_t ACT_RL_ROW_ENTRY     = 0x90;
static const uintptr_t ACT_ENTRY_HERO_OFF   = 0x08;
static const uintptr_t ACT_ENTRY_STATE_OFF  = 0x1540;

// ---- ROUND 9: the SANITARIUM (QuirkTreatmentActivityDisplay) ----
static const uintptr_t QT_SLOTDATA_BEG       = 0x190;    // activity+: vector<TreatmentSlotData>
static const uintptr_t QT_SLOTDATA_END       = 0x198;
static const uintptr_t QT_SLOTDATA_STRIDE    = 0x1e0;    // = 3 x TreatmentListData
static const uintptr_t QT_LIST_STRIDE        = 0xa0;
static const uintptr_t QT_LIST_SEL_OFF       = 0x18;     // list+: the chosen entry (a copy)
static const uintptr_t QT_ENTRY_STRIDE       = 0x88;
static const uintptr_t QT_ENTRY_ID_MAX       = 0x80;     // entry+0x00: the quirk id, inline
static const uintptr_t QT_ENTRY_LOCKED_OFF   = 0x80;     // entry+: the hero already locked this one
static const uintptr_t QT_ENTRY_CANLOCK_OFF  = 0x81;     // entry+: mode 1 (lock) is offered
static const uintptr_t QT_ENTRY_CANREM_OFF   = 0x82;     // entry+: mode 2 (remove) is offered
static const uintptr_t QT_ENTRY_MODE_OFF     = 0x84;     // entry+: 0 none / 1 lock / 2 remove
static const uintptr_t QT_HAS_LIST_OFF       = 0x23c;    // activity+ type: 1 = this list is drawn
static const uintptr_t QT_COSTTBL_OFF        = 0x1c0;    // activity+ type*0x18: the cost table
static const uintptr_t QT_COSTTBL_STRIDE     = 0x18;
static const uintptr_t QT_COSTTBL_PERMNEG    = 0x208;    // activity+: locked-negative cost table
static const uintptr_t QT_COSTREC_STRIDE     = 0x50;     // {uint treeHash, byte code} + CurrencyCost
static const uintptr_t QT_COSTREC_CODE_OFF   = 0x04;     // record+: 0 = no prerequisite
static const uintptr_t QT_COSTREC_COST_OFF   = 0x08;     // record+: the CurrencyCost
static const uintptr_t QT_COST_AMOUNT_OFF    = 0x00;     // cost+: int amount
static const uintptr_t QT_COST_HASH_OFF      = 0x44;     // cost+: currency type hash
static const uintptr_t QT_ACT_IDHASH_OFF     = 0x60;     // activity+: its own id hash
static const uint32_t  QT_ELEM_BASE          = 0x71726b; // row element = base + type*0x20 + index
static const uint32_t  QT_EVTDATA_ACT_COST   = 2;        // town-event record type: activity cost
static const int       QT_MODE_LOCK          = 1;
static const int       QT_MODE_REMOVE        = 2;

typedef uint8_t (*ActBoolFn)(uintptr_t);
typedef bool    (*ActSlotBoolFn)(uintptr_t, uint32_t);
static bool bldSehActFlag(uintptr_t base, uintptr_t rva, uintptr_t activity, bool fallback) {
    __try { return ((ActBoolFn)(base + rva))(activity) != 0; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return fallback; }
}
static bool bldSehActSlotLocked(uintptr_t base, uintptr_t activity, uint32_t slot) {
    __try { return ((ActSlotBoolFn)(base + ACT_SLOTLOCKED_RVA))(activity, slot); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

typedef void  (*ActCostFn)(uintptr_t activity, void* outVec, int guid, uint32_t slot);
typedef uint8_t (*ActAffordFn)(uintptr_t ledger, void* costVec);
typedef void  (*ActVecFreeFn)(void* vec);
static bool bldSehActHeroCost(uintptr_t base, uintptr_t activity, int guid, uint32_t slot,
                              int* amountOut, uint32_t* curHashOut, bool* affordOut) {
    struct { uintptr_t beg, end, cap; } vec = { 0, 0, 0 };
    uintptr_t campaign = 0;
    if (!safeReadPtr(base + RES_CAMPAIGN_RVA, &campaign) || campaign <= 0x10000) return false;
    __try {
        ((ActCostFn)(base + ACT_COSTBYID_RVA))(activity, &vec, guid, slot);
        int total = 0; uint32_t cur = 0;
        for (uintptr_t p = vec.beg; p + 0x48 <= vec.end; p += 0x48) {
            total += *(int32_t*)p;
            cur = *(uint32_t*)(p + 0x44);
        }
        *amountOut = total;
        *curHashOut = cur;
        *affordOut = ((ActAffordFn)(base + ACT_CANAFFORD_RVA))(campaign + ACT_LEDGER_OFF, &vec) != 0;
        ((ActVecFreeFn)(base + ACT_COSTFREE_RVA))(&vec);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

typedef void* (*ActCanHeroFn)(uintptr_t activity, void* out, int guid);
static bool bldSehActCanHero(uintptr_t base, uintptr_t activity, int guid,
                             bool* affordOut, bool* eligibleOut) {
    struct {
        uint8_t afford; uint8_t pad[7];
        uintptr_t beg, end, cap;
        uint8_t hasRestrict, inRestrict; uint8_t pad2[6];
    } out;
    memset(&out, 0, sizeof out);
    __try {
        ((ActCanHeroFn)(base + ACT_CANHERO_RVA))(activity, &out, guid);
        *affordOut = out.afford != 0;
        bool reqOk = out.beg == out.end;
        bool restrictOk = !out.hasRestrict || out.inRestrict;
        *eligibleOut = reqOk && restrictOk;
        ((ActVecFreeFn)(base + ACT_REQFREE_RVA))(&out.beg);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// ---- WHY the pick will be refused: the offending quirk(s), by name ----
static bool bldSehActFailQuirkIds(uintptr_t base, uintptr_t activity, int guid, uintptr_t hero,
                                  char ids[][64], int maxIds, int* nOut) {
    struct {
        uint8_t afford; uint8_t pad[7];
        uintptr_t beg, end, cap;
        uint8_t hasRestrict, inRestrict; uint8_t pad2[6];
    } out;
    memset(&out, 0, sizeof out);
    *nOut = 0;

    uintptr_t reqs[16];
    int nReq = 0;
    __try {
        ((ActCanHeroFn)(base + ACT_CANHERO_RVA))(activity, &out, guid);
        for (uintptr_t p = out.beg; p + 8 <= out.end && nReq < 16; p += 8) {
            uintptr_t req = *(uintptr_t*)p;
            if (req > 0x10000) reqs[nReq++] = req;
        }
        ((ActVecFreeFn)(base + ACT_REQFREE_RVA))(&out.beg);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }

    __try {
        uintptr_t hqBeg = *(uintptr_t*)(hero + HERO_QUIRK_BEGIN_OFF);
        uintptr_t hqEnd = *(uintptr_t*)(hero + HERO_QUIRK_END_OFF);
        for (int i = 0; i < nReq && *nOut < maxIds; i++) {
            uintptr_t vft = *(uintptr_t*)reqs[i];
            if (vft <= base || (vft - base) != ACT_REQ_NOTQUIRKS_VFT) continue;   // not the quirk rule
            uintptr_t qBeg = *(uintptr_t*)(reqs[i] + ACT_REQ_QUIRKS_BEG);
            uintptr_t qEnd = *(uintptr_t*)(reqs[i] + ACT_REQ_QUIRKS_END);
            if (qBeg <= 0x10000 || qEnd <= qBeg) continue;
            if ((qEnd - qBeg) > ACT_REQ_QUIRK_STRIDE * 64) continue;              // wild span
            for (uintptr_t b = qBeg; b + ACT_REQ_QUIRK_STRIDE <= qEnd && *nOut < maxIds;
                 b += ACT_REQ_QUIRK_STRIDE) {
                const char* blocked = (const char*)b;
                if (!blocked[0]) continue;
                // Does this hero actually carry it? Only then is it the reason.
                for (uintptr_t h = hqBeg; h + QUIRK_ENTRY_STRIDE <= hqEnd && *nOut < maxIds;
                     h += QUIRK_ENTRY_STRIDE) {
                    uintptr_t qc = *(uintptr_t*)(h + QUIRK_ENTRY_CLASS_OFF);
                    if (qc <= 0x10000) continue;
                    const char* had = (const char*)(qc + QUIRK_ID_OFF);
                    if (strncmp(had, blocked, 63) != 0) continue;
                    bool seen = false;                       // two rules can block the same quirk
                    for (int k = 0; k < *nOut; k++)
                        if (strncmp(ids[k], had, 63) == 0) { seen = true; break; }
                    if (!seen) {
                        strncpy(ids[*nOut], had, 63);
                        ids[*nOut][63] = 0;
                        (*nOut)++;
                    }
                    break;
                }
            }
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { *nOut = 0; return false; }
}

static bool bldActWhyQuirkText(uintptr_t base, uintptr_t activity, int guid, uintptr_t hero,
                               char* out, int outsz) {
    if (!out || outsz <= 1) return false;
    out[0] = 0;
    if (!activity || !hero) return false;

    char ids[8][64];
    int n = 0;
    if (!bldSehActFailQuirkIds(base, activity, guid, hero, ids, 8, &n)) {
        logLine("bldact why: eligibility call faulted (act=%p guid=%d)", (void*)activity, guid);
        return false;
    }
    if (n <= 0) return false;                    // refused for some other reason — say nothing here

    char header[256] = {0}, fmt[256] = {0};
    if (!resolveKey(base, kActFailKey, header, sizeof header)) return false;
    if (!resolveKey(base, kActFailQuirkKey, fmt, sizeof fmt)) return false;
    abStripMarkup(header);
    abStripMarkup(fmt);
    int specs = 0;
    bool wellFormed = true;
    for (const char* p = fmt; *p; p++) {
        if (*p != '%') continue;
        if (p[1] == '%') { p++; continue; }          // an escaped percent, harmless
        if (p[1] == 's') { specs++; p++; continue; }
        wellFormed = false;
        break;
    }
    if (!wellFormed || specs != 1) {
        logLine("bldact why: \"%s\" is not a single-%%s format (\"%s\") — not substituting",
                kActFailQuirkKey, fmt);
        return false;
    }

    _snprintf(out, outsz, "%s", header);
    out[outsz - 1] = 0;
    int said = 0;
    for (int i = 0; i < n; i++) {
        char name[128];
        if (!csQuirkName(base, ids[i], name, sizeof name) || !name[0]) {
            logLine("bldact why: quirk \"%s\" has no name — omitted", ids[i]);
            continue;
        }
        char line[320];
        _snprintf(line, sizeof line, fmt, name);   // the substitution the game skipped
        line[sizeof line - 1] = 0;
        char seg[336];
        _snprintf(seg, sizeof seg, " %s.", line);
        seg[sizeof seg - 1] = 0;
        strncat(out, seg, outsz - strlen(out) - 1);
        said++;
    }
    if (!said) { out[0] = 0; return false; }
    logLine("bldact why: composed \"%s\" (%d quirk(s) of %d named)", out, said, n);
    return true;
}

static void bldActPickDump(uintptr_t base, uintptr_t slotw, uintptr_t activity, uintptr_t panel,
                           int slotIdx, uintptr_t rowWidget) {
    if (!axDebugLogEnabled()) return;                // diagnostic: nothing runs with the log off
    uintptr_t entry = 0, iface = 0, owner = 0, vbeg = 0, vend = 0, actVft = 0, slotVft = 0;
    uint32_t guid = 0, state = 0;
    char who[64] = {0}, ownerId[64] = {0};
    safeReadPtr(rowWidget + ACT_RL_ROW_ENTRY, &entry);
    if (entry > 0x10000) {
        safeReadU32(entry, &guid);
        safeReadU32(entry + ACT_ENTRY_STATE_OFF, &state);
        safeReadCStr(entry + ACT_ENTRY_HERO_OFF + HERO_NAME_OFF, who, sizeof who);
    }
    safeReadPtr(slotw, &slotVft);
    safeReadPtr(slotw + 0x158, &iface);
    safeReadPtr(activity, &actVft);
    safeReadPtr(activity + 0x18, &owner);
    if (owner > 0x10000) safeReadCStr(owner + 0x20, ownerId, sizeof ownerId);
    safeReadPtr(panel + BLD_ICD_ITEMS_BEG, &vbeg);
    safeReadPtr(panel + BLD_ICD_ITEMS_END, &vend);
    bool afford = false, eligible = false;
    bool verdict = entry > 0x10000 &&
                   bldSehActCanHero(base, activity, (int)guid, &afford, &eligible);
    logDump("bldact pick dump: hero \"%s\" guid=%u state=%u entry=%p row=%p", who, guid, state,
            (void*)entry, (void*)rowWidget);
    logDump("bldact pick dump: verdict=%s afford=%d eligible=%d -> the body will take the %s branch",
            verdict ? "read" : "FAULTED", (int)afford, (int)eligible,
            !verdict ? "unknown" : eligible ? "ACCEPT" : "REFUSE");
    logDump("bldact pick dump: slot %d slotw=%p vft=+0x%llx iface=%p", slotIdx, (void*)slotw,
            (unsigned long long)(slotVft > base ? slotVft - base : 0), (void*)iface);
    logDump("bldact pick dump: act=%p vft=+0x%llx owner=%p id=\"%s\"", (void*)activity,
            (unsigned long long)(actVft > base ? actVft - base : 0), (void*)owner, ownerId);
    logDump("bldact pick dump: panel=%p items=%p..%p (%lld wing display(s))", (void*)panel,
            (void*)vbeg, (void*)vend,
            (long long)(vbeg && vend >= vbeg && (vend - vbeg) % 8 == 0 ? (vend - vbeg) / 8 : -1));
    logLine("bldact: pick dump written (hero \"%s\", %s branch)", who,
            !verdict ? "unknown" : eligible ? "ACCEPT" : "REFUSE");
}

typedef void (*ActPickFn)(void* closure, uintptr_t rosterElemWidget);
static bool bldSehActPick(uintptr_t base, uintptr_t slotw, uintptr_t activity, uintptr_t panel,
                          int slotIdx, uintptr_t rowWidget) {
    struct { uintptr_t slotw, act, panel; int64_t idx; } c = {
        slotw, activity, panel, (int64_t)slotIdx };
    bldActPickDump(base, slotw, activity, panel, slotIdx, rowWidget);
    __try { ((ActPickFn)(base + ACT_PICK_RVA))(&c, rowWidget); return true; }
    __except (sehReport("bldact pick", GetExceptionInformation())) { return false; }
}
typedef void (*ActClosureFn)(void* closure);
static bool bldSehActConfirm(uintptr_t base, uintptr_t disp, uintptr_t hero, uintptr_t slotw,
                             int slotIdx, uintptr_t activity) {
    struct { uintptr_t disp, hero, slotw; int64_t idx; uintptr_t act; } c = {
        disp, hero, slotw, (int64_t)slotIdx, activity };
    __try { ((ActClosureFn)(base + ACT_CONFIRM_RVA))(&c); return true; }
    __except (sehReport("bldact confirm", GetExceptionInformation())) { return false; }
}
static bool bldSehActUncommit(uintptr_t base, uintptr_t disp, uintptr_t hero, uintptr_t slotw,
                              int slotIdx, uintptr_t activity) {
    struct { uintptr_t disp, hero, slotw; int64_t idx; uintptr_t act; } c = {
        disp, hero, slotw, (int64_t)slotIdx, activity };
    __try { ((ActClosureFn)(base + ACT_UNCOMMIT_RVA))(&c); return true; }
    __except (sehReport("bldact uncommit", GetExceptionInformation())) { return false; }
}

// ---- ROUND 9 helpers: the treatment lists ----

static uintptr_t qtList(uintptr_t base, uintptr_t activity, int slot, int type) {
    (void)base;
    if (slot < 0 || type < 0 || type > 2) return 0;
    uintptr_t beg = 0, end = 0;
    if (!safeReadPtr(activity + QT_SLOTDATA_BEG, &beg) ||
        !safeReadPtr(activity + QT_SLOTDATA_END, &end) || !beg || end <= beg) return 0;
    uintptr_t slots = (end - beg) / QT_SLOTDATA_STRIDE;
    if ((uintptr_t)slot >= slots || slots > 16) return 0;
    return beg + (uintptr_t)slot * QT_SLOTDATA_STRIDE + (uintptr_t)type * QT_LIST_STRIDE;
}

static bool qtHasList(uintptr_t activity, int type) {
    uint8_t f = 0;
    if (type < 0 || type > 2) return false;
    return safeReadU8(activity + QT_HAS_LIST_OFF + (uintptr_t)type, &f) && f != 0;
}

static int qtListCount(uintptr_t list) {
    uintptr_t beg = 0, end = 0;
    if (!list || !safeReadPtr(list, &beg) || !safeReadPtr(list + 8, &end)) return 0;
    if (!beg || end < beg || (end - beg) % QT_ENTRY_STRIDE) return 0;
    uintptr_t n = (end - beg) / QT_ENTRY_STRIDE;
    return n > 64 ? 64 : (int)n;
}

static uintptr_t qtEntryAt(uintptr_t list, int i) {
    uintptr_t beg = 0;
    if (i < 0 || i >= qtListCount(list) || !safeReadPtr(list, &beg) || !beg) return 0;
    return beg + (uintptr_t)i * QT_ENTRY_STRIDE;
}

static bool qtChosen(uintptr_t list, char* idOut, int idSz, int* modeOut) {
    if (idOut && idSz) idOut[0] = 0;
    if (modeOut) *modeOut = 0;
    if (!list) return false;
    char id[QT_ENTRY_ID_MAX + 1] = {0};
    if (!safeReadCStr(list + QT_LIST_SEL_OFF, id, sizeof id) || !id[0]) return false;
    if (idOut) { strncpy(idOut, id, idSz - 1); idOut[idSz - 1] = 0; }
    if (modeOut) {
        int n = qtListCount(list);
        for (int i = 0; i < n; i++) {
            uintptr_t e = qtEntryAt(list, i);
            char eid[QT_ENTRY_ID_MAX + 1] = {0};
            if (!e || !safeReadCStr(e, eid, sizeof eid) || strcmp(eid, id) != 0) continue;
            uint32_t m = 0;
            if (safeReadU32(e + QT_ENTRY_MODE_OFF, &m)) *modeOut = (int)m;
            break;
        }
    }
    return true;
}

static bool qtSlotHasChoice(uintptr_t base, uintptr_t activity, int slot) {
    for (int t = 0; t < 3; t++)
        if (qtChosen(qtList(base, activity, slot, t), nullptr, 0, nullptr)) return true;
    return false;
}

static uintptr_t qtQuirkClass(uintptr_t hero, const char* id) {
    uintptr_t beg = 0, end = 0;
    if (!hero || !id || !id[0]) return 0;
    if (!safeReadPtr(hero + HERO_QUIRK_BEGIN_OFF, &beg) ||
        !safeReadPtr(hero + HERO_QUIRK_END_OFF, &end) || !beg || end <= beg) return 0;
    if ((end - beg) % QUIRK_ENTRY_STRIDE) return 0;
    uintptr_t n = (end - beg) / QUIRK_ENTRY_STRIDE;
    if (n > 64) n = 64;
    for (uintptr_t i = 0; i < n; i++) {
        uintptr_t qc = 0;
        char qid[96] = {0};
        if (!safeReadPtr(beg + i * QUIRK_ENTRY_STRIDE + QUIRK_ENTRY_CLASS_OFF, &qc) ||
            qc <= 0x10000) continue;
        if (safeReadCStr(qc + QUIRK_ID_OFF, qid, sizeof qid) && strcmp(qid, id) == 0) return qc;
    }
    return 0;
}

static float qtCostFactor(uintptr_t base, uintptr_t hero, uintptr_t activity) {
    float mult = 1.0f;
    uintptr_t obj = 0, beg = 0, end = 0;
    int level = csResolveLevel(base, hero);
    if (level >= 0 && safeReadPtr(base + QT_ACTMULT_RVA, &obj) && obj > 0x10000 &&
        safeReadPtr(obj + 0x28, &beg) && safeReadPtr(obj + 0x30, &end) && beg && end > beg) {
        uintptr_t count = (end - beg) / 4;
        if ((uintptr_t)level < count && count <= 32) {
            uint32_t bits = 0;
            if (safeReadU32(beg + (uintptr_t)level * 4, &bits)) memcpy(&mult, &bits, sizeof mult);
        }
    }
    if (!(mult > 0.0f) || mult > 100.0f) {
        logLine("sanitarium: cost multiplier unreadable (level=%d) — using 1.0", level);
        mult = 1.0f;
    }
    uintptr_t camp = 0;
    uint32_t active = 0, actHash = 0;
    if (safeReadPtr(base + RES_CAMPAIGN_RVA, &camp) && camp > 0x10000 &&
        safeReadU32(camp + PROV_CAMP_EVENT_OFF, &active) && active &&
        safeReadU32(activity + QT_ACT_IDHASH_OFF, &actHash)) {
        uintptr_t reg = 0, eb = 0, ee = 0;
        if (safeReadPtr(base + PROV_EVTREG_RVA, &reg) && reg > 0x10000 &&
            safeReadPtr(reg + PROV_EVTREG_BEG_OFF, &eb) &&
            safeReadPtr(reg + PROV_EVTREG_END_OFF, &ee) && eb && ee > eb &&
            (ee - eb) % PROV_EVT_STRIDE == 0) {
            int nev = (int)((ee - eb) / PROV_EVT_STRIDE);
            if (nev > 256) nev = 256;
            for (int i = 0; i < nev; i++) {
                uintptr_t evt = eb + (uintptr_t)i * PROV_EVT_STRIDE;
                uint32_t idh = 0;
                if (!safeReadU32(evt + PROV_EVT_IDHASH_OFF, &idh) || idh != active) continue;
                uintptr_t db = 0, de = 0;
                if (!safeReadPtr(evt + PROV_EVT_DATA_BEG, &db) ||
                    !safeReadPtr(evt + PROV_EVT_DATA_END, &de) || !db || de <= db) break;
                int nrec = (int)((de - db) / PROV_EVTDATA_STRIDE);
                if (nrec > 64) nrec = 64;
                for (int r = 0; r < nrec; r++) {
                    uintptr_t rec = db + (uintptr_t)r * PROV_EVTDATA_STRIDE;
                    uint32_t kind = 0, who = 0, bits = 0;
                    float d = 0.0f;
                    if (!safeReadU32(rec, &kind) || kind != QT_EVTDATA_ACT_COST) continue;
                    if (!safeReadU32(rec + PROV_EVTDATA_HASH, &who) || who != actHash) continue;
                    if (!safeReadU32(rec + PROV_EVTDATA_AMT, &bits)) continue;
                    memcpy(&d, &bits, sizeof d);
                    if (d > -10.0f && d < 10.0f) mult += d;
                }
                break;
            }
        }
    }
    return mult;
}

static bool qtRowCost(uintptr_t base, uintptr_t activity, uintptr_t hero, int type, bool locked,
                      int* amountOut, uint32_t* curOut) {
    uintptr_t tbl = (type == 1 && locked) ? activity + QT_COSTTBL_PERMNEG
                                          : activity + QT_COSTTBL_OFF +
                                            (uintptr_t)type * QT_COSTTBL_STRIDE;
    uintptr_t beg = 0, end = 0;
    if (!safeReadPtr(tbl, &beg) || !safeReadPtr(tbl + 8, &end) || !beg || end <= beg) return false;
    if ((end - beg) % QT_COSTREC_STRIDE) return false;
    int n = (int)((end - beg) / QT_COSTREC_STRIDE);
    if (n > 16) return false;
    uintptr_t best = 0;
    for (int i = 0; i < n; i++) {
        uintptr_t rec = beg + (uintptr_t)i * QT_COSTREC_STRIDE;
        uint32_t hash = 0;
        uint8_t code = 0;
        if (!safeReadU32(rec, &hash) || !safeReadU8(rec + QT_COSTREC_CODE_OFF, &code)) break;
        if (code && !haPurchased(base, hash, code, 0)) break;   // the first unbought step ends it
        best = rec;
    }
    if (!best) return false;
    uint32_t amt = 0, cur = 0;
    if (!safeReadU32(best + QT_COSTREC_COST_OFF + QT_COST_AMOUNT_OFF, &amt)) return false;
    safeReadU32(best + QT_COSTREC_COST_OFF + QT_COST_HASH_OFF, &cur);
    if ((int32_t)amt < 0 || amt > 10000000) return false;
    float f = qtCostFactor(base, hero, activity);
    *amountOut = (int)((float)amt * f + 0.5f);
    if (curOut) *curOut = cur;
    return true;
}

typedef void (*QtClickFn)(void* closure);
static bool qtSehClick(uintptr_t base, uintptr_t activity, int type, int slot, int idx) {
    struct { uintptr_t vft; uintptr_t act; int32_t type, slot, idx, pad; } c = {
        0, activity, (int32_t)type, (int32_t)slot, (int32_t)idx, 0 };
    __try { ((QtClickFn)(base + QT_CLICK_RVA))(&c); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
static bool qtClickRow(uintptr_t base, uintptr_t panel, uintptr_t activity, int type,
                       int slot, int idx) {
    (void)panel;
    return qtSehClick(base, activity, type, slot, idx);
}

static bool qtSetChoice(uintptr_t base, uintptr_t panel, uintptr_t activity, int slot, int type,
                        int idx, const char* id, int wantMode) {
    for (int attempt = 0; attempt < 5; attempt++) {
        uintptr_t list = qtList(base, activity, slot, type);
        char chosen[QT_ENTRY_ID_MAX + 1] = {0};
        int mode = 0;
        bool any = qtChosen(list, chosen, sizeof chosen, &mode);
        bool isThis = any && strcmp(chosen, id) == 0;
        if (wantMode == 0 ? !isThis : (isThis && mode == wantMode)) return true;
        if (!qtClickRow(base, panel, activity, type, slot, idx)) return false;
    }
    logLine("sanitarium: could not set \"%s\" (list %d, slot %d) to mode %d after 5 presses",
            id, type, slot, wantMode);
    return false;
}

// ---- ROUND 9.1: the two DROPDOWNS ----
struct QtCand {
    int      type;
    int      idx;        // index within that list
    uint8_t  locked;
    bool     chosen;
    char     id[QT_ENTRY_ID_MAX + 1];
};

static int qtCandidates(uintptr_t base, uintptr_t activity, int slot, int mode,
                        QtCand* out, int max) {
    int n = 0;
    for (int t = 0; t < 3 && n < max; t++) {
        if (!qtHasList(activity, t)) continue;
        uintptr_t list = qtList(base, activity, slot, t);
        int cnt = qtListCount(list);
        char chosenId[QT_ENTRY_ID_MAX + 1] = {0};
        int chosenMode = 0;
        qtChosen(list, chosenId, sizeof chosenId, &chosenMode);
        for (int e = 0; e < cnt && n < max; e++) {
            uintptr_t entry = qtEntryAt(list, e);
            uint8_t gate = 0;
            if (!entry) continue;
            if (!safeReadU8(entry + (mode == QT_MODE_LOCK ? QT_ENTRY_CANLOCK_OFF
                                                          : QT_ENTRY_CANREM_OFF), &gate) || !gate)
                continue;
            QtCand* c = &out[n];
            memset(c, 0, sizeof *c);
            if (!safeReadCStr(entry, c->id, sizeof c->id) || !c->id[0]) continue;
            c->type = t;
            c->idx = e;
            safeReadU8(entry + QT_ENTRY_LOCKED_OFF, &c->locked);
            c->chosen = (chosenMode == mode && strcmp(chosenId, c->id) == 0);
            n++;
        }
    }
    return n;
}

static bool qtDropExists(uintptr_t base, uintptr_t activity, int slot, int mode) {
    QtCand c[64];
    return qtCandidates(base, activity, slot, mode, c, 64) > 0;
}

static const char* qtDropWord(int mode)   { return mode == QT_MODE_LOCK ? "Lock" : "Remove"; }
static const char* qtDropSpoken(int mode) {
    return axs(mode == QT_MODE_LOCK ? AXS_QT_WORD_LOCK : AXS_QT_WORD_REMOVE);
}

static void qtDropState(uintptr_t base, uintptr_t activity, uintptr_t hero, int slot, int mode,
                        char* names, int namesSz, int* totalOut, uint32_t* curOut) {
    names[0] = 0;
    if (totalOut) *totalOut = 0;
    QtCand cands[64];
    int n = qtCandidates(base, activity, slot, mode, cands, 64);
    for (int i = 0; i < n; i++) {
        if (!cands[i].chosen) continue;
        char nm[160];
        if (!csQuirkName(base, cands[i].id, nm, sizeof nm) || !nm[0]) {
            strncpy(nm, cands[i].id, sizeof nm - 1);
            nm[sizeof nm - 1] = 0;
        }
        char joined[MAILBOX_SZ];
        _snprintf(joined, sizeof joined, "%s%s%s%s%s", names, names[0] ? " " : "",
                  names[0] ? axs(AXS_WORD_AND) : "", names[0] ? " " : "", nm);
        joined[sizeof joined - 1] = 0;
        _snprintf(names, namesSz, "%s", joined);
        names[namesSz - 1] = 0;
        int amt = 0;
        uint32_t cur = 0;
        if (totalOut && qtRowCost(base, activity, hero, cands[i].type, cands[i].locked != 0,
                                  &amt, &cur)) {
            *totalOut += amt;
            if (curOut) *curOut = cur;
        }
    }
}

// One dropdown row, collapsed: "Lock. Select." or "Remove. Off Guard. 1500 gold."
static void qtDropRowText(uintptr_t base, const BldActRow* r, char* out, int outsz) {
    char names[MAILBOX_SZ];
    int total = 0;
    uint32_t cur = 0;
    qtDropState(base, r->activity, r->pendingHero, r->slotIdx, r->trtDrop,
                names, sizeof names, &total, &cur);
    if (!names[0]) {
        _snprintf(out, outsz, axs(AXS_QT_ROW_SELECT_FMT), qtDropSpoken(r->trtDrop));
    } else if (total > 0) {
        char curName[48];
        bldCurrencyWord(base, cur, curName, sizeof curName);
        _snprintf(out, outsz, axs(AXS_QT_ROW_CHOSEN_COST_FMT), qtDropSpoken(r->trtDrop), names,
                  total, curName);
    } else {
        _snprintf(out, outsz, axs(AXS_QT_ROW_CHOSEN_FMT), qtDropSpoken(r->trtDrop), names);
    }
    out[outsz - 1] = 0;
}

static bool qtDropClear(uintptr_t base, uintptr_t panel, uintptr_t activity, int slot, int mode) {
    QtCand cands[64];
    int n = qtCandidates(base, activity, slot, mode, cands, 64);
    bool ok = true;
    for (int i = 0; i < n; i++)
        if (cands[i].chosen &&
            !qtSetChoice(base, panel, activity, slot, cands[i].type, cands[i].idx, cands[i].id, 0))
            ok = false;
    return ok;
}

int bldActRows(uintptr_t base, uintptr_t panel, BldActRow* out, int max) {
    uintptr_t ib = 0, ie = 0;
    if (!safeReadPtr(panel + BLD_ICD_ITEMS_BEG, &ib) ||
        !safeReadPtr(panel + BLD_ICD_ITEMS_END, &ie) || !ib || ie <= ib || (ie - ib) % 8) return 0;
    int items = (int)((ie - ib) / 8);
    if (items > 16) return 0;
    int n = 0, actOrd = 0;
    for (int d = 0; d < items; d++) {
        uintptr_t disp = 0, vft = 0;
        if (!safeReadPtr(ib + (uintptr_t)d * 8, &disp) || disp <= 0x10000) continue;
        if (!safeReadPtr(disp, &vft)) continue;
        bool quirkTreat = (vft - base == BLD_QTAD_VFT_RVA);
        if (vft - base != BLD_HSRD_VFT_RVA && !quirkTreat) continue;
        uintptr_t activity = 0, sb = 0, se = 0;
        if (!safeReadPtr(disp + BLD_ACTD_ACT_OFF, &activity) || activity <= 0x10000) continue;
        if (!safeReadPtr(disp + BLD_ACTD_SLOTS_BEG, &sb) ||
            !safeReadPtr(disp + BLD_ACTD_SLOTS_END, &se) || !sb || se < sb || (se - sb) % 8)
            continue;
        int slots = (int)((se - sb) / 8);
        if (slots > 8) slots = 8;
        char actId[32] = {0};
        safeReadCStr(activity + BLD_ACT_ID_OFF, actId, sizeof actId);
        uintptr_t rb = 0, re2 = 0;
        safeReadPtr(activity + BLD_ACT_SLOTREC_BEG, &rb);
        safeReadPtr(activity + BLD_ACT_SLOTREC_END, &re2);
        for (int s = 0; s < slots && n < max; s++) {
            uintptr_t slotw = 0, iface = 0, pend = 0;
            if (!safeReadPtr(sb + (uintptr_t)s * 8, &slotw) || slotw <= 0x10000) continue;
            BldActRow* r = &out[n];
            memset(r, 0, sizeof *r);
            r->disp = disp; r->activity = activity; r->slotw = slotw;
            r->slotIdx = s; r->slotCount = slots; r->actOrd = actOrd;
            r->isQuirkTreat = quirkTreat;
            r->trtDrop = -1;
            strncpy(r->actId, actId, sizeof r->actId - 1);
            uint32_t elem = 0;
            if (safeReadU32(slotw + BLD_SLOT_ELEM_OFF, &elem)) r->slotElem = elem;
            uint32_t occ = 0;
            r->occupant = -1;
            if (safeReadU32(slotw + BLD_SLOT_OCCUPANT_OFF, &occ)) r->occupant = (int32_t)occ;
            uintptr_t rec = rb + (uintptr_t)s * BLD_ACT_SLOTREC_STRIDE;
            if (rb && rec + BLD_ACT_SLOTREC_STRIDE <= re2) {
                uint32_t guid = 0;
                if (safeReadU32(rec, &guid)) r->committedGuid = guid;
            }
            if (safeReadPtr(slotw + BLD_RCT_IFACE_OFF, &iface) && iface > 0x10000 &&
                safeReadPtr(iface + BLD_RCT_IFACE_HERO, &pend) && pend > 0x10000 &&
                !r->committedGuid)
                r->pendingHero = pend;
            uintptr_t slotHero = r->pendingHero;
            BldActRow slotRow = *r;
            n++;
            if (!quirkTreat || !slotHero) continue;
            static const int kDrops[2] = { QT_MODE_LOCK, QT_MODE_REMOVE };
            for (int d = 0; d < 2 && n < max; d++) {
                if (!qtDropExists(base, activity, s, kDrops[d])) continue;
                BldActRow* q = &out[n];
                *q = slotRow;                      // the slot's identity, then the row's own part
                q->trtDrop = kDrops[d];
                n++;
            }
        }
        actOrd++;
    }
    return n;
}

static bool bldHasActivityRows(uintptr_t base, uintptr_t panel) {
    BldActRow rows[48];
    return bldActRows(base, panel, rows, 48) > 0;
}

// A roster entry (and its Hero) by guid — Campaign+0x20, pure reads.
uintptr_t bldActHeroByGuid(uintptr_t base, uint32_t guid) {
    uintptr_t campaign = 0, beg = 0, end = 0;
    if (!safeReadPtr(base + RES_CAMPAIGN_RVA, &campaign) || campaign <= 0x10000) return 0;
    if (!safeReadPtr(campaign + 0x20, &beg) || !safeReadPtr(campaign + 0x28, &end) ||
        !beg || end <= beg || end - beg > 0x8000) return 0;
    for (uintptr_t p = beg; p < end; p += 8) {
        uintptr_t entry = 0; uint32_t g = 0;
        if (!safeReadPtr(p, &entry) || entry <= 0x10000) continue;
        if (safeReadU32(entry, &g) && g == guid) return entry + ACT_ENTRY_HERO_OFF;
    }
    return 0;
}

// The activity's display name / description, through the game's own keys.
static void bldActName(uintptr_t base, const char* actId, char* out, int outsz) {
    char key[64];
    _snprintf(key, sizeof key, "town_activity_name_%s", actId);
    key[sizeof key - 1] = 0;
    if (!resolveKey(base, key, out, outsz)) {
        strncpy(out, actId, outsz - 1);
        out[outsz - 1] = 0;
        for (char* p = out; *p; p++) if (*p == '_') *p = ' ';
    }
}

bool bldActivityNameFromPtr(uintptr_t base, uintptr_t activity, char* out, int outsz) {
    out[0] = 0;
    char actId[32] = {0};
    if (activity <= 0x10000) return false;
    if (!safeReadCStr(activity + BLD_ACT_ID_OFF, actId, sizeof actId) || !actId[0]) return false;
    bldActName(base, actId, out, outsz);
    return out[0] != 0;
}

static void qtActionText(uintptr_t base, int mode, const char* quirkName, char* out, int outsz) {
    char fmt[160];
    const char* key = (mode == QT_MODE_LOCK) ? "str_sanitarium_lock_quirk_format"
                                             : "str_sanitarium_remove_quirk_format";
    if (resolveKey(base, key, fmt, sizeof fmt) && strstr(fmt, "%s")) {
        _snprintf(out, outsz, fmt, quirkName);
    } else {
        _snprintf(out, outsz, axs(mode == QT_MODE_LOCK ? AXS_QT_LOCK_FMT : AXS_QT_REMOVE_FMT),
                  quirkName);
    }
    out[outsz - 1] = 0;
}

static const char* qtListWord(int type) {
    return axs(type == 0 ? AXS_QT_TYPE_POSITIVE
                         : (type == 1 ? AXS_QT_TYPE_NEGATIVE : AXS_QT_TYPE_DISEASE));
}

static void qtChoiceSummary(uintptr_t base, const BldActRow* r, char* out, int outsz) {
    out[0] = 0;
    for (int t = 0; t < 3; t++) {
        uintptr_t list = qtList(base, r->activity, r->slotIdx, t);
        char id[QT_ENTRY_ID_MAX + 1] = {0};
        int mode = 0;
        if (!qtChosen(list, id, sizeof id, &mode) || !mode) continue;
        char name[160];
        if (!csQuirkName(base, id, name, sizeof name) || !name[0]) {
            strncpy(name, id, sizeof name - 1);
            name[sizeof name - 1] = 0;
        }
        char action[224];
        qtActionText(base, mode, name, action, sizeof action);
        char joined[MAILBOX_SZ];
        if (out[0]) _snprintf(joined, sizeof joined, "%s %s %s", out, axs(AXS_WORD_AND), action);
        else        _snprintf(joined, sizeof joined, "%s", action);
        joined[sizeof joined - 1] = 0;
        _snprintf(out, outsz, "%s", joined);
        out[outsz - 1] = 0;
    }
}

// ---- CRIMSON COURT: the contagion warning ----
typedef int (*ActContagionFn)(uintptr_t panel);
static int bldSehContagionLevel(uintptr_t base, uintptr_t panel) {
    __try { return ((ActContagionFn)(base + ACT_CONTAGION_LEVEL_RVA))(panel); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
}
static int bldContagionLevel(uintptr_t base, uintptr_t panel) {
    uintptr_t tip = 0;
    if (!panel || !safeReadPtr(panel + ACT_CONTAGION_TIP_OFF, &tip) || tip <= 0x10000) return -1;
    int lv = bldSehContagionLevel(base, panel);
    if (lv < 0 || lv > 3) {
        logLine("bldact: contagion getter returned %d (panel=%p) — not spoken", lv, (void*)panel);
        return -1;
    }
    return lv;
}

static void bldContagionWord(uintptr_t base, int level, char* out, int outsz) {
    out[0] = 0;
    if (level < 0 || level > 3) return;
    static const char* kLevelKey[4] = { "infestation_building_contagion_none",
                                        "infestation_building_contagion_low",
                                        "infestation_building_contagion_medium",
                                        "infestation_building_contagion_high" };
    char word[96];
    if (!resolveKey(base, kLevelKey[level], word, sizeof word) || !word[0]) return;
    abStripMarkup(word);
    char* w = word;
    while (*w == ':') w++;                          // "::High::" -> "High"
    size_t wl = strlen(w);
    while (wl && w[wl - 1] == ':') w[--wl] = 0;
    _snprintf(out, outsz, "%s", w);
    out[outsz - 1] = 0;
}

static void bldContagionText(uintptr_t base, int level, char* out, int outsz) {
    out[0] = 0;
    if (level <= 0) return;
    char head[224], word[96];
    if (!resolveKey(base, "infestation_building_contagion_description", head, sizeof head) ||
        !head[0]) return;                          // no key, no sentence — never a guess
    abStripMarkup(head);
    bldContagionWord(base, level, word, sizeof word);
    if (!word[0]) return;
    _snprintf(out, outsz, axs(AXS_ACT_CONTAGION_FMT), head, word);
    out[outsz - 1] = 0;
}

static bool bldIsActivityBuildingId(const char* id) {
    return id && (!strcmp(id, "abbey") || !strcmp(id, "tavern") || !strcmp(id, "sanitarium"));
}

static int bldActWingCount(const BldActRow* rows, int n) {
    int c = 0;
    for (int i = 0; i < n; i++) if (rows[i].actOrd + 1 > c) c = rows[i].actOrd + 1;
    return c;
}

static void bldActRowText(uintptr_t base, uintptr_t panel, const BldActRow* r, int actCount,
                          bool withWing, char* out, int outsz) {
    char wing[576];
    wing[0] = 0;
    if (withWing) {
        char nm[96];
        bldActName(base, r->actId, nm, sizeof nm);
        char head[192];
        if (actCount > 1) {
            _snprintf(head, sizeof head, axs(AXS_ACT_WING_POS_FMT), nm, r->actOrd + 1, actCount);
        } else {
            _snprintf(head, sizeof head, "%s.", nm);
        }
        head[sizeof head - 1] = 0;
        _snprintf(wing, sizeof wing, "%s ", head);
        wing[sizeof wing - 1] = 0;
    }
    if (r->trtDrop >= 0) {
        char drop[MAILBOX_SZ];
        qtDropRowText(base, r, drop, sizeof drop);
        _snprintf(out, outsz, "%s%s", wing, drop);
        out[outsz - 1] = 0;
        return;
    }
    char state[320];
    state[0] = 0;
    if (r->occupant >= 0) {
        _snprintf(state, sizeof state, "%s", axs(AXS_ACT_CARETAKER_HERE));
    } else if (r->committedGuid) {
        char who[80] = {0};
        uintptr_t hero = bldActHeroByGuid(base, r->committedGuid);
        if (hero) safeReadCStr(hero + HERO_NAME_OFF, who, sizeof who);
        _snprintf(state, sizeof state, axs(AXS_ACT_STAYING_FMT),
                  who[0] ? who : axs(AXS_ACT_A_HERO));
    } else if (r->pendingHero) {
        char who[80] = {0};
        safeReadCStr(r->pendingHero + HERO_NAME_OFF, who, sizeof who);
        if (r->isQuirkTreat) {
            char chosen[MAILBOX_SZ];
            qtChoiceSummary(base, r, chosen, sizeof chosen);
            if (!chosen[0]) {
                _snprintf(state, sizeof state, axs(AXS_QT_NOTHING_CHOSEN_FMT),
                          who[0] ? who : axs(AXS_ACT_A_HERO));
            } else {
                char costFrag[80] = {0};
                uint32_t guid = 0;
                int amount = 0;
                uint32_t cur = 0;
                bool afford = true;
                safeReadU32(r->pendingHero + ACTOR_ID_OFF, &guid);
                if (bldSehActHeroCost(base, r->activity, (int)guid, (uint32_t)r->slotIdx,
                                      &amount, &cur, &afford) && amount > 0) {
                    char money[64], curName[48];
                    bldCurrencyWord(base, cur, curName, sizeof curName);
                    _snprintf(money, sizeof money, axs(AXS_BLD_COST_SHORT_FMT), amount, curName);
                    money[sizeof money - 1] = 0;
                    if (afford) _snprintf(costFrag, sizeof costFrag, " %s", money);
                    else        _snprintf(costFrag, sizeof costFrag, " %s %s", money,
                                          axs(AXS_BLD_CANT_AFFORD_IT));
                }
                _snprintf(state, sizeof state, axs(AXS_QT_PENDING_FMT),
                          who[0] ? who : axs(AXS_ACT_A_HERO), chosen, costFrag);
            }
        } else {
            char costFrag[80] = {0};
            uint32_t guid = 0;
            int amount = 0;
            uint32_t cur = 0;
            bool afford = true;
            safeReadU32(r->pendingHero + ACTOR_ID_OFF, &guid);
            if (bldSehActHeroCost(base, r->activity, (int)guid, (uint32_t)r->slotIdx,
                                  &amount, &cur, &afford) && amount > 0) {
                char money[64], curName[48];
                bldCurrencyWord(base, cur, curName, sizeof curName);
                _snprintf(money, sizeof money, axs(AXS_BLD_COST_SHORT_FMT), amount, curName);
                money[sizeof money - 1] = 0;
                if (afford) _snprintf(costFrag, sizeof costFrag, " %s", money);
                else        _snprintf(costFrag, sizeof costFrag, " %s %s", money,
                                      axs(AXS_BLD_CANT_AFFORD_IT));
            }
            _snprintf(state, sizeof state, axs(AXS_ACT_NOT_CONFIRMED_COST_FMT),
                      who[0] ? who : axs(AXS_ACT_A_HERO), costFrag);
        }
    } else if (bldSehActSlotLocked(base, r->activity, (uint32_t)r->slotIdx)) {
        _snprintf(state, sizeof state, "%s", axs(AXS_ACT_SLOT_LOCKED));
    } else if (bldSehActFlag(base, ACT_EVTLOCKED_RVA, r->activity, false)) {
        _snprintf(state, sizeof state, "%s", axs(AXS_ACT_CLOSED_WEEK));
    } else {
        bool free_ = !bldSehActFlag(base, ACT_COSTSMONEY_RVA, r->activity, true);
        _snprintf(state, sizeof state, "%s",
                  axs(free_ ? AXS_ACT_EMPTY_FREE_CHOOSE : AXS_ACT_EMPTY_CHOOSE));
    }
    state[sizeof state - 1] = 0;
    char slotPos[64];
    _snprintf(slotPos, sizeof slotPos, axs(AXS_ACT_SLOT_POS_FMT), r->slotIdx + 1, r->slotCount);
    slotPos[sizeof slotPos - 1] = 0;
    _snprintf(out, outsz, "%s%s %s", wing, slotPos, state);
    out[outsz - 1] = 0;
}

static void bldActProbe(uintptr_t base, uintptr_t panel) {
    BldActRow rows[48];
    int n = bldActRows(base, panel, rows, 48);
    logLine("bldact probe: %d rows", n);
    for (int i = 0; i < n; i++) {
        const BldActRow* r = &rows[i];
        if (r->trtDrop >= 0) {
            QtCand cands[64];
            int cn = qtCandidates(base, r->activity, r->slotIdx, r->trtDrop, cands, 64);
            char names[MAILBOX_SZ];
            int total = 0;
            uint32_t cur = 0;
            qtDropState(base, r->activity, r->pendingHero, r->slotIdx, r->trtDrop,
                        names, sizeof names, &total, &cur);
            logLine("bldact probe: row %d DROPDOWN \"%s\" act=\"%s\" slot %d: %d candidates, "
                    "holding \"%s\" for %d", i, qtDropWord(r->trtDrop), r->actId,
                    r->slotIdx + 1, cn, names, total);
            for (int c = 0; c < cn; c++) {
                int amt = -1;
                uint32_t cu = 0;
                qtRowCost(base, r->activity, r->pendingHero, cands[c].type,
                          cands[c].locked != 0, &amt, &cu);
                logLine("bldact probe:   cand %d id=\"%s\" list=%d idx=%d locked=%d chosen=%d "
                        "cost=%d", c, cands[c].id, cands[c].type, cands[c].idx,
                        cands[c].locked, (int)cands[c].chosen, amt);
            }
            continue;
        }
        uint32_t cancelElem = r->slotElem - (uint32_t)r->slotIdx + BLD_ELEM_CNCL_TAG
                              + (uint32_t)r->slotIdx;
        char who[64] = {0};
        if (r->pendingHero) safeReadCStr(r->pendingHero + HERO_NAME_OFF, who, sizeof who);
        logLine("bldact probe: row %d act=\"%s\" (%d) slot %d/%d elem=0x%x(%s) cancel=0x%x(%s) "
                "committed=%u pending=%p\"%s\" occupant=%d locked=%d evtlocked=%d costsmoney=%d",
                i, r->actId, r->actOrd, r->slotIdx + 1, r->slotCount,
                r->slotElem, feGetElementById((int64_t)r->slotElem) ? "live" : "absent",
                cancelElem, feGetElementById((int64_t)cancelElem) ? "live" : "absent",
                r->committedGuid, (void*)r->pendingHero, who, r->occupant,
                (int)bldSehActSlotLocked(base, r->activity, (uint32_t)r->slotIdx),
                (int)bldSehActFlag(base, ACT_EVTLOCKED_RVA, r->activity, false),
                (int)bldSehActFlag(base, ACT_COSTSMONEY_RVA, r->activity, true));
    }
    logLine("bldact probe: cnfm plain elem %s",
            feGetElementById((int64_t)BLD_ELEM_CNFM_ID) ? "live" : "absent");
}

static bool bldActFocusedRowText(uintptr_t base, uintptr_t panel, char* row, int rowsz,
                                 bool withWing) {
    BldActRow rows[48];
    int n = bldActRows(base, panel, rows, 48);
    if (n <= 0) return false;
    axStepCursor(&g_bldRow, n, 0);           // re-clamp against the live rows
    bldActRowText(base, panel, &rows[g_bldRow], bldActWingCount(rows, n), withWing, row, rowsz);
    return true;
}

static void bldActMove(uintptr_t base, uintptr_t panel, int dCol, int dRow) {
    BldActRow rows[48];
    int n = bldActRows(base, panel, rows, 48);
    if (n <= 0) { postSpeech(axs(AXS_ACT_NONE_READ)); return; }
    axStepCursor(&g_bldRow, n, 0);           // re-clamp against the live rows
    int wing = rows[g_bldRow].actOrd, slot = rows[g_bldRow].slotIdx;
    if (dRow) {
        int i = g_bldRow + dRow;
        if (i >= 0 && i < n && rows[i].actOrd == wing) g_bldRow = i;
    } else if (dCol) {
        int best = -1;
        for (int i = 0; i < n; i++) {
            if (rows[i].actOrd != wing + dCol || rows[i].trtDrop >= 0) continue;
            if (best < 0) { best = i; continue; }
            int have = rows[best].slotIdx, cand = rows[i].slotIdx;
            bool better = (have > slot) ? (cand < have) : (cand <= slot && cand > have);
            if (better) best = i;
        }
        if (best >= 0) g_bldRow = best;
    }
    char row[MAILBOX_SZ];
    if (!bldActFocusedRowText(base, panel, row, sizeof row, dCol != 0))
        _snprintf(row, sizeof row, "%s", axs(AXS_ACT_NONE_READ));
    row[sizeof row - 1] = 0;
    postSpeech(row);
}

// ---- ROUND 9.1: the OPEN dropdown (a mod layer over the building, like the recruit button) ----
bool             g_qtOpen = false;
static int       g_qtOpenMode = 0;          // QT_MODE_LOCK / QT_MODE_REMOVE
static int       g_qtOpenRow = 0;           // 0 = the placeholder, 1.. = candidates
static uintptr_t g_qtOpenActivity = 0;
static uintptr_t g_qtOpenHero = 0;
static int       g_qtOpenSlot = -1;

static void qtOpenRowText(uintptr_t base, int row, char* out, int outsz) {
    QtCand cands[64];
    int n = qtCandidates(base, g_qtOpenActivity, g_qtOpenSlot, g_qtOpenMode, cands, 64);
    if (row <= 0 || row > n) {
        _snprintf(out, outsz, axs(AXS_QT_PLACEHOLDER_POS_FMT), n + 1);
        out[outsz - 1] = 0;
        return;
    }
    const QtCand* c = &cands[row - 1];
    char name[160];
    if (!csQuirkName(base, c->id, name, sizeof name) || !name[0]) {
        strncpy(name, c->id, sizeof name - 1);
        name[sizeof name - 1] = 0;
        for (char* p = name; *p; p++) if (*p == '_') *p = ' ';
    }
    char priceFrag[80] = {0};
    int amount = 0;
    uint32_t cur = 0;
    if (qtRowCost(base, g_qtOpenActivity, g_qtOpenHero, c->type, c->locked != 0, &amount, &cur)) {
        char money[64], curName[48];
        bldCurrencyWord(base, cur, curName, sizeof curName);
        _snprintf(money, sizeof money, axs(AXS_BLD_COST_SHORT_FMT), amount, curName);
        money[sizeof money - 1] = 0;
        _snprintf(priceFrag, sizeof priceFrag, " %s", money);
    }
    char lockFrag[32] = {0};
    if (c->locked && c->type == 1) _snprintf(lockFrag, sizeof lockFrag, " %s", axs(AXS_QT_LOCKED_IN));
    char chosenFrag[32] = {0};
    if (c->chosen) _snprintf(chosenFrag, sizeof chosenFrag, " %s", axs(AXS_QT_CHOSEN));
    char pos[48];
    _snprintf(pos, sizeof pos, axs(AXS_POS_N_OF_M), row + 1, n + 1);
    pos[sizeof pos - 1] = 0;
    _snprintf(out, outsz, "%s.%s%s%s %s. %s", name, priceFrag, lockFrag,
              chosenFrag, qtListWord(c->type), pos);
    out[outsz - 1] = 0;
}

static void qtSpeakOpenRow(uintptr_t base, const char* prefix) {
    QtCand cands[64];
    int n = qtCandidates(base, g_qtOpenActivity, g_qtOpenSlot, g_qtOpenMode, cands, 64);
    if (g_qtOpenRow > n) g_qtOpenRow = n;          // hard stops, placeholder included
    if (g_qtOpenRow < 0) g_qtOpenRow = 0;
    char row[MAILBOX_SZ], utter[MAILBOX_SZ];
    qtOpenRowText(base, g_qtOpenRow, row, sizeof row);
    _snprintf(utter, sizeof utter, "%s%s", prefix ? prefix : "", row);
    utter[sizeof utter - 1] = 0;
    postSpeech(utter);
}

// ---- the ACTIVITY PICK session (the hero choice itself happens ON THE ROSTER) ----
static uintptr_t g_bldActPickActivity = 0;  // the slot being filled; 0 = no session armed
static uintptr_t g_bldActPickSlotW = 0;
static int       g_bldActPickSlot = -1;
static char      g_bldActPickActId[32];
// One in-flight outcome watch for the whole activity family:
static int       g_bldActWatch = 0;         // 0 idle, 1 pick, 2 confirm, 3 uncommit, 4 unpend
static DWORD     g_bldActWatchUntil = 0;
static uintptr_t g_bldActWatchAct = 0;
static int       g_bldActWatchSlot = -1;
static uintptr_t g_bldActWatchHero = 0;     // the hero the watch expects (pick/confirm)
static int       g_bldActGoldBefore = 0;
static int       g_bldActLvlBefore = -1;
static bool      g_bldActSawDialog = false;
static char      g_bldActWatchWho[64];
static char      g_bldActWatchWhat[96];
static char      g_bldActDeferUtter[MAILBOX_SZ];
static uint32_t  g_bldActDeferGuid  = 0;    // the committed hero; 0 = nothing parked
static DWORD     g_bldActDeferUntil = 0;

bool bldActTakeCommitFollowup(uint32_t guid, char* out, int outsz) {
    if (!g_bldActDeferGuid || !guid || guid != g_bldActDeferGuid) return false;
    strncpy(out, g_bldActDeferUtter, (size_t)outsz - 1);
    out[outsz - 1] = 0;
    g_bldActDeferGuid = 0;
    return true;
}

static int bldActIdleCount(uintptr_t base) {
    uintptr_t root = resTownRoot(base);
    if (!root) return 0;
    uintptr_t beg = 0, end = 0;
    if (!safeReadPtr(root + ACT_RL_OFF + ACT_RL_ROWS_BEG, &beg) ||
        !safeReadPtr(root + ACT_RL_OFF + ACT_RL_ROWS_END, &end) || !beg || end <= beg) return 0;
    int rows = (int)((end - beg) / 0x40);
    if (rows > 64) rows = 64;
    int n = 0;
    for (int i = 0; i < rows; i++) {
        uintptr_t rowW = 0, entry = 0;
        if (!safeReadPtr(beg + (uintptr_t)i * 0x40, &rowW) || rowW <= 0x10000) continue;
        if (!safeReadPtr(rowW + ACT_RL_ROW_ENTRY, &entry) || entry <= 0x10000) continue;
        uint32_t state = 0;
        if (!safeReadU32(entry + ACT_ENTRY_STATE_OFF, &state) || state != 0) continue;
        n++;
    }
    return n;
}

void bldActPickRowSuffix(uintptr_t base, uintptr_t hero, char* out, int outsz) {
    out[0] = 0;
    if (!g_bldActPickActivity || !hero) return;
    char costFrag[96] = {0}, eligFrag[96] = {0};
    uint32_t guid = 0;
    safeReadU32(hero + ACTOR_ID_OFF, &guid);             // the roster guid (== Entry+0x00)
    int amount = 0; uint32_t cur = 0; bool afford = true;
    bool freeNow = !bldSehActFlag(base, ACT_COSTSMONEY_RVA, g_bldActPickActivity, true);
    if (freeNow) {
        _snprintf(costFrag, sizeof costFrag, " %s", axs(AXS_ACT_FREE_WEEK));
    } else if (bldSehActHeroCost(base, g_bldActPickActivity, (int)guid,
                                 (uint32_t)g_bldActPickSlot, &amount, &cur, &afford)) {
        if (amount > 0) {
            char money[96], curName[48];
            bldCurrencyWord(base, cur, curName, sizeof curName);
            _snprintf(money, sizeof money, axs(AXS_BLD_COSTS_AMOUNT_FMT), amount, curName);
            money[sizeof money - 1] = 0;
            if (afford) _snprintf(costFrag, sizeof costFrag, " %s", money);
            else        _snprintf(costFrag, sizeof costFrag, " %s %s", money,
                                  axs(AXS_BLD_CANT_AFFORD_IT));
        }
    } else {
        logLine("bldact: cost getter faulted (act=%p guid=%u)",
                (void*)g_bldActPickActivity, guid);
    }
    bool canAfford = true, eligible = true;
    if (bldSehActCanHero(base, g_bldActPickActivity, (int)guid, &canAfford, &eligible)) {
        if (!eligible)
            _snprintf(eligFrag, sizeof eligFrag, " %s", axs(AXS_ACT_NOT_ELIGIBLE));
    }
    _snprintf(out, outsz, "%s%s", costFrag, eligFrag);
    out[outsz - 1] = 0;
}

bool bldActPickBusy() {
    return g_bldActPickActivity != 0 && g_bldActWatch != 0;
}

static void bldActPickSessionDrop(const char* why) {
    if (!g_bldActPickActivity) return;
    logLine("bldact: pick session dropped (%s)", why);
    g_bldActPickActivity = 0;
    g_bldActPickSlotW = 0;
    g_bldActPickSlot = -1;
    g_bldActPickActId[0] = 0;
}

bool bldActPickCommitFromRoster(uintptr_t base, uintptr_t entry, uintptr_t rowWidget,
                                uintptr_t hero) {
    (void)entry;                                          // the lambda re-reads it off the widget
    uintptr_t root = resTownRoot(base);
    uintptr_t panel = root ? bldOpenPanel(root) : 0;
    if (!panel || !g_bldActPickActivity) {                // the building vanished under the pick
        logLine("bldact: commit with no %s -- cancelling the pick",
                panel ? "session" : "panel");
        bldActPickSessionDrop("commit found no building under it");
        rosLeave(base, "activity pick lost its building");
        postSpeech(axs(AXS_BLD_DIDNT_HAPPEN));
        return true;
    }
    char who[64] = {0};
    safeReadCStr(hero + HERO_NAME_OFF, who, sizeof who);
    g_bldActWhy[0] = 0;
    {
        uint32_t pguid = 0;
        safeReadU32(hero + ACTOR_ID_OFF, &pguid);
        bldActWhyQuirkText(base, g_bldActPickActivity, (int)pguid, hero,
                           g_bldActWhy, sizeof g_bldActWhy);
    }
    if (!bldSehActPick(base, g_bldActPickSlotW, g_bldActPickActivity, panel,
                       g_bldActPickSlot, rowWidget)) {
        logLine("bldact: pick body faulted (act=%p slot=%d row=%p)",
                (void*)g_bldActPickActivity, g_bldActPickSlot, (void*)rowWidget);
        postSpeech(axs(AXS_BLD_DIDNT_HAPPEN));
        return true;
    }
    g_bldActWatch = 1;
    g_bldActWatchUntil = GetTickCount() + 1500;
    g_bldActWatchAct = g_bldActPickActivity;
    g_bldActWatchSlot = g_bldActPickSlot;
    g_bldActWatchHero = hero;
    g_bldActSawDialog = false;
    g_bldActLvlBefore = bldContagionLevel(base, panel);
    strncpy(g_bldActWatchWho, who[0] ? who : axs(AXS_THE_HERO), sizeof g_bldActWatchWho - 1);
    g_bldActWatchWho[sizeof g_bldActWatchWho - 1] = 0;
    bldActName(base, g_bldActPickActId, g_bldActWatchWhat, sizeof g_bldActWatchWhat);
    logLine("bldact: pick called, hero \"%s\" -> \"%s\" slot %d",
            who, g_bldActPickActId, g_bldActPickSlot);
    return true;
}

void bldActPickCancelFromRoster(uintptr_t base) {
    bldActPickSessionDrop("cancelled from the roster");
    uintptr_t root = resTownRoot(base);
    uintptr_t panel = root ? bldOpenPanel(root) : 0;
    char pfx[64];
    _snprintf(pfx, sizeof pfx, "%s ", axs(AXS_CANCELLED));
    pfx[sizeof pfx - 1] = 0;
    if (!panel || !bldSpeakOptionRow(base, panel, pfx)) postSpeech(axs(AXS_CANCELLED));
}

static bool bldActSlotState(uintptr_t base, uintptr_t panel, uintptr_t activity, int slotIdx,
                            uint32_t* committedOut, uintptr_t* pendingOut) {
    BldActRow rows[48];
    int n = bldActRows(base, panel, rows, 48);
    for (int i = 0; i < n; i++) {
        if (rows[i].activity == activity && rows[i].slotIdx == slotIdx) {
            *committedOut = rows[i].committedGuid;
            *pendingOut = rows[i].pendingHero;
            return true;
        }
    }
    return false;
}

// The row index of a given slot, so an outcome can land the cursor back on it.
static int bldActRowIndexOf(uintptr_t base, uintptr_t panel, uintptr_t activity, int slotIdx) {
    BldActRow rows[48];
    int n = bldActRows(base, panel, rows, 48);
    for (int i = 0; i < n; i++)
        if (rows[i].activity == activity && rows[i].slotIdx == slotIdx) return i;
    return -1;
}

// ---- ROUND 8: the HERO-ACTION family — Blacksmith / Guild / Camping Trainer ----
static const uintptr_t HA_TREES_BEG      = 0x270;
static const uintptr_t HA_TREES_END      = 0x278;   // panel+: ... end (was 0x280)
static const uintptr_t HA_TREES2_BEG     = 0x288;
static const uintptr_t HA_TREES2_END     = 0x290;
static const uintptr_t HA_FAC_MAP_OFF    = 0x11f8;  // Campaign+: map<building id hash, Facility*>
static const uintptr_t HA_FAC_HERO_OFF   = 0x128;   // Facility+: selected hero guid (0 = none)
static const uintptr_t HA_URD_STRIDE     = 0x180;   // UTD+0xa0 vector: inline URDs
static const uintptr_t HA_URD_GUID_OFF   = 0x80;    // URD+: the hero guid this step belongs to
static const uintptr_t HA_REG_INSTANCED  = 0x44;    // registry rec+: byte, tree is per-hero
static const uintptr_t HA_REG_TAGS_BEG   = 0x48;    // registry rec+: vector<uint tag hash> begin
static const uintptr_t HA_UTD_CFG_OFF    = 0x98;    // UTD+: the ctor's 4th arg — a per-screen config
static const uintptr_t HA_CFG_ICONBUYS   = 0x1c;    // config+: int, the tree ICON is a purchase
                                                    // (the icon's own click body reads exactly this)
static const uintptr_t HA_REG_REQMAP     = 0x60;    // registry rec+: map<code byte, Requirement>
static const uintptr_t HA_REQ_COST_BEG   = 0x08;    // requirement+: vector<CurrencyCost> begin
static const uintptr_t HA_REQ_COST_END   = 0x10;    // requirement+: ... end
static const uintptr_t HA_REQ_PRQ_BEG    = 0x20;    // requirement+: vector<{hash, code}> begin
static const uintptr_t HA_REQ_PRQ_END    = 0x28;    // requirement+: ... end
static const uintptr_t HA_REQ_RESOLVE    = 0x38;    // requirement+: int required resolve level
static const uintptr_t HA_COST_STRIDE    = 0x48;
static const uintptr_t HA_COST_AMOUNT    = 0x00;    // cost+: int amount
static const uintptr_t HA_COST_TYPE      = 0x04;    // cost+: char type[0x40] ("gold", "crest"...)
static const int       HA_MAX_STEPS      = 8;       // shipped max is 5
static const int       HA_MAX_TREES      = 16;      // guild = 7, camping trainer up to ~8
static const int       HA_MAX_HEROES     = 64;

// Which skin is open. -1 = this building is not a HeroActionDisplay at all.
static int haSkinOf(uintptr_t base, uintptr_t panel) {
    uintptr_t vft = 0;
    if (!panel || !safeReadPtr(panel, &vft) || vft <= base) return -1;
    uintptr_t rva = vft - base;
    if (rva == HA_BSMITH_VFT_RVA) return 0;
    if (rva == HA_GUILD_VFT_RVA)  return 1;
    if (rva == HA_CAMPT_VFT_RVA)  return 2;
    return -1;
}

static uintptr_t haMapRoot(uintptr_t mapHead) {
    uintptr_t root = 0;
    if (!safeReadPtr(mapHead + 8, &root) || root <= 0x10000) return 0;
    return root;
}

static uintptr_t haFacility(uintptr_t base, uintptr_t panel) {
    uint32_t want = 0;
    uintptr_t campaign = 0, head = 0;
    if (!safeReadU32(panel + BLD_ID_HASH_OFF, &want)) return 0;
    if (!safeReadPtr(base + RES_CAMPAIGN_RVA, &campaign) || campaign <= 0x10000) return 0;
    if (!safeReadPtr(campaign + HA_FAC_MAP_OFF, &head) || head <= 0x10000) return 0;
    uintptr_t node = haMapRoot(head), best = 0;
    for (int i = 0; i < 64 && node > 0x10000; i++) {
        uint8_t nil = 1;
        uint32_t key = 0;
        if (!safeReadU8(node + 0x19, &nil) || nil) break;
        if (!safeReadU32(node + 0x20, &key)) return 0;
        if (key < want) { if (!safeReadPtr(node + 0x10, &node)) return 0; }
        else            { best = node; if (!safeReadPtr(node, &node)) return 0; }
    }
    uint32_t bkey = 0;
    uintptr_t fac = 0;
    if (!best || !safeReadU32(best + 0x20, &bkey) || bkey != want) return 0;
    if (!safeReadPtr(best + 0x28, &fac) || fac <= 0x10000) return 0;
    return fac;
}

static uint32_t haSelectedGuid(uintptr_t base, uintptr_t panel) {
    uintptr_t fac = haFacility(base, panel);
    uint32_t guid = 0;
    if (!fac || !safeReadU32(fac + HA_FAC_HERO_OFF, &guid)) return 0;
    return guid;
}

static uintptr_t haTreeRecord(uintptr_t base, uint32_t hash) {
    uintptr_t beg = 0, end = 0;
    if (!safeReadPtr(base + BLD_REG_BEGIN_RVA, &beg) || !safeReadPtr(base + BLD_REG_END_RVA, &end) ||
        !beg || end <= beg) return 0;
    uintptr_t count = (end - beg) / BLD_REG_STRIDE;
    if (count > 4096) return 0;
    for (uintptr_t i = 0; i < count; i++) {
        uintptr_t rec = beg + i * BLD_REG_STRIDE;
        uint32_t h = 0;
        if (safeReadU32(rec + BLD_REG_HASH_OFF, &h) && h == hash) return rec;
    }
    return 0;
}

static uintptr_t haRequirement(uintptr_t base, uint32_t hash, uint8_t code) {
    uintptr_t rec = haTreeRecord(base, hash);
    uintptr_t head = 0;
    if (!rec || !safeReadPtr(rec + HA_REG_REQMAP, &head) || head <= 0x10000) return 0;
    uintptr_t node = haMapRoot(head), best = 0;
    for (int i = 0; i < 64 && node > 0x10000; i++) {
        uint8_t nil = 1, key = 0;
        if (!safeReadU8(node + 0x19, &nil) || nil) break;
        if (!safeReadU8(node + 0x20, &key)) return 0;
        if (key < code) { if (!safeReadPtr(node + 0x10, &node)) return 0; }
        else            { best = node; if (!safeReadPtr(node, &node)) return 0; }
    }
    uint8_t bkey = 0;
    if (!best || !safeReadU8(best + 0x20, &bkey) || bkey != code) return 0;
    return best + 0x28;
}

static int haRegSteps(uintptr_t base, uint32_t hash, uint8_t* out, int max) {
    static const char kCodes[] = "0123456789abcdefghij";
    int n = 0;
    for (int i = 0; kCodes[i] && n < max; i++)
        if (haRequirement(base, hash, (uint8_t)kCodes[i])) out[n++] = (uint8_t)kCodes[i];
    return n;
}

bool haPurchased(uintptr_t base, uint32_t hash, uint8_t code, uint32_t guid) {
    uint32_t all = 0;
    if (safeReadU32(base + HA_ALLBOUGHT_RVA, &all) && (all & 0xff)) return true;
    uintptr_t head = 0;
    if (!safeReadPtr(base + HA_PURCH_MAP_RVA, &head) || head <= 0x10000) return false;
    uintptr_t node = haMapRoot(head), best = 0;
    for (int i = 0; i < 128 && node > 0x10000; i++) {
        uint8_t nil = 1, ncode = 0;
        uint32_t nhash = 0, nguid = 0;
        if (!safeReadU8(node + 0x19, &nil) || nil) break;
        if (!safeReadU32(node + 0x1c, &nhash) || !safeReadU8(node + 0x20, &ncode) ||
            !safeReadU32(node + 0x24, &nguid)) return false;
        bool less = nhash < hash ||
                    (nhash == hash && (ncode < code ||
                                       (ncode == code && nguid < guid)));
        if (less) { if (!safeReadPtr(node + 0x10, &node)) return false; }
        else      { best = node; if (!safeReadPtr(node, &node)) return false; }
    }
    if (!best) return false;
    uint8_t bcode = 0, bought = 0;
    uint32_t bhash = 0, bguid = 0;
    if (!safeReadU32(best + 0x1c, &bhash) || !safeReadU8(best + 0x20, &bcode) ||
        !safeReadU32(best + 0x24, &bguid)) return false;
    if (bhash != hash || bcode != code || bguid != guid) return false;
    return safeReadU8(best + 0x28, &bought) && bought != 0;
}

static const uintptr_t HA_FMAP_KEY_OFF = 0x1c;   // node+: uint key   (4-byte-aligned pair)
static const uintptr_t HA_FMAP_VAL_OFF = 0x20;   // node+: float value
static float haFloatMapAt(uintptr_t base, uintptr_t mapRva, uint32_t key) {
    uintptr_t head = 0;
    if (!safeReadPtr(base + mapRva, &head) || head <= 0x10000) return 0.f;
    uintptr_t node = haMapRoot(head), best = 0;
    for (int i = 0; i < 64 && node > 0x10000; i++) {
        uint8_t nil = 1;
        uint32_t k = 0;
        if (!safeReadU8(node + 0x19, &nil) || nil) break;
        if (!safeReadU32(node + HA_FMAP_KEY_OFF, &k)) return 0.f;
        if (k < key) { if (!safeReadPtr(node + 0x10, &node)) return 0.f; }
        else         { best = node; if (!safeReadPtr(node, &node)) return 0.f; }
    }
    uint32_t bk = 0, bits = 0;
    if (!best || !safeReadU32(best + HA_FMAP_KEY_OFF, &bk) || bk != key) return 0.f;
    if (!safeReadU32(best + HA_FMAP_VAL_OFF, &bits)) return 0.f;
    float f;
    memcpy(&f, &bits, sizeof f);
    return f;
}

static float haDiscountFactor(uintptr_t base, uint32_t treeHash) {
    uintptr_t rec = haTreeRecord(base, treeHash);
    uintptr_t tb = 0, te = 0;
    if (!rec || !safeReadPtr(rec + HA_REG_TAGS_BEG, &tb) ||
        !safeReadPtr(rec + HA_REG_TAGS_BEG + 8, &te) || !tb || te < tb || (te - tb) % 4) return 0.f;
    int n = (int)((te - tb) / 4);
    if (n < 0 || n > 32) return 0.f;
    float f = 0.f;
    for (int i = 0; i < n; i++) {
        uint32_t tag = 0;
        if (!safeReadU32(tb + (uintptr_t)i * 4, &tag)) continue;
        f += haFloatMapAt(base, HA_DISC1_RVA, tag);
        f += haFloatMapAt(base, HA_DISC2_RVA, tag);
        f += haFloatMapAt(base, HA_DISC3_RVA, tag);
    }
    if (!(f > 0.f) || f > 0.95f) {                 // NaN, negative or absurd: no discount, and say so
        if (f != 0.f) logLine("heroaction: discount factor %.3f for tree 0x%x is implausible "
                              "— treating the listed price as exact", f, treeHash);
        return 0.f;
    }
    static uint32_t lastTree = 0;
    static float    lastF    = 0.f;
    if (treeHash != lastTree || f != lastF) {
        lastTree = treeHash; lastF = f;
        logLine("heroaction: tree 0x%x is discounted by %.3f (%d%% off)",
                treeHash, f, (int)(f * 100.f + 0.5f));
    }
    return f;
}

static int haStepPrice(uintptr_t base, uint32_t treeHash, int amount) {
    if (amount <= 0) return amount;
    float f = haDiscountFactor(base, treeHash);
    if (f <= 0.f) return amount;
    int paid = amount - (int)((float)amount * f);
    return paid < 0 ? 0 : paid;
}

// ---- what a column IS, and the hero's LIVE state in it ----
enum HaKind { HA_OTHER = 0, HA_WEAPON, HA_ARMOUR, HA_COMBAT, HA_CAMP };

static int haCombatIndexOf(uintptr_t base, uintptr_t heroClass, const char* treeId) {
    const char* want = strchr(treeId, '.');
    if (!heroClass || !want || !want[1]) return -1;
    want++;
    uintptr_t beg = 0, end = 0;
    if (!safeReadPtr(heroClass + HEROCLASS_SKILLVEC_OFF, &beg) ||
        !safeReadPtr(heroClass + HEROCLASS_SKILLVEC_END_OFF, &end) ||
        beg <= 0x10000 || end < beg) return -1;
    int n = (int)((end - beg) / 0x18);
    if (n < 0 || n > 32) return -1;
    for (int i = 0; i < n; i++) {
        uintptr_t inner = 0;
        char id[64] = {0};
        if (!safeReadPtr(beg + (uintptr_t)i * 0x18, &inner) || inner <= 0x10000) continue;
        if (!safeReadCStr(inner + SKILL_ID_OFF, id, sizeof id) || !id[0]) continue;
        if (strcmp(id, want) == 0) return i;
    }
    return -1;
}

static int haCombatLevelOf(uintptr_t base, uintptr_t hero, uintptr_t heroClass, int idx) {
    return csComSkillLevelOf(base, hero, heroClass, idx);
}

static bool haCombatEquippedOf(uintptr_t base, uintptr_t hero, int idx) {
    if (!hero || idx < 0) return false;
    uintptr_t beg = 0, end = 0;
    if (!safeReadPtr(hero + ACTOR_SEL_BEG_OFF, &beg) ||
        !safeReadPtr(hero + ACTOR_SEL_END_OFF, &end) || beg <= 0x10000 || end < beg) return false;
    uintptr_t n = (end - beg) / 4;
    if (n > 32) return false;
    for (uintptr_t k = 0; k < n; k++) {
        uint32_t slot = 0;
        if (safeReadU32(beg + k * 4, &slot) && (int)slot == idx) return true;
    }
    return false;
}

static uintptr_t haCombatRecordOf(uintptr_t base, uintptr_t heroClass, int idx, int level) {
    if (!heroClass || idx < 0 || level < 0 || level > 15) return 0;
    uintptr_t beg = 0, inner = 0;
    if (!safeReadPtr(heroClass + HEROCLASS_SKILLVEC_OFF, &beg) || beg <= 0x10000) return 0;
    if (!safeReadPtr(beg + (uintptr_t)idx * 0x18, &inner) || inner <= 0x10000) return 0;
    return inner + (uintptr_t)level * SKILL_STRIDE;
}

static int haCampIndexOf(uintptr_t base, uintptr_t heroClass, const char* treeId) {
    const char* want = strchr(treeId, '.');
    if (!heroClass || !want || !want[1]) return -1;
    uint32_t wantHash = resHash(want + 1);
    uintptr_t beg = 0, end = 0;
    if (!safeReadPtr(heroClass + HEROCLASS_CAMPVEC_OFF, &beg) ||
        !safeReadPtr(heroClass + HEROCLASS_CAMPVEC_END_OFF, &end) ||
        beg <= 0x10000 || end < beg) return -1;
    int n = (int)((end - beg) / 4);
    if (n < 0 || n > 32) return -1;
    for (int i = 0; i < n; i++) {
        uint32_t h = 0;
        if (safeReadU32(beg + (uintptr_t)i * 4, &h) && h == wantHash) return i;
    }
    return -1;
}

static bool haCampLearnedOf(uintptr_t base, uintptr_t hero, int idx) {
    if (!hero || idx < 0) return false;
    uintptr_t beg = 0, end = 0;
    if (!safeReadPtr(hero + ACTOR_CAMP_KNOWN_OFF, &beg) ||
        !safeReadPtr(hero + ACTOR_CAMP_KNOWN_END_OFF, &end) || beg <= 0x10000 || end < beg)
        return false;
    if ((uintptr_t)idx >= (end - beg) / 4) return false;
    int32_t v = -1;
    if (!safeReadU32(beg + (uintptr_t)idx * 4, (uint32_t*)&v)) return false;
    return v >= 0;
}

static uintptr_t haCampClassOf(uintptr_t base, const char* treeId) {
    const char* want = strchr(treeId, '.');
    if (!want || !want[1]) return 0;
    uint32_t wantHash = resHash(want + 1);
    uintptr_t vec = 0, beg = 0, end = 0;
    if (!safeReadPtr(base + CAMP_REGISTRY_RVA, &vec) || vec <= 0x10000) return 0;
    if (!safeReadPtr(vec, &beg) || !safeReadPtr(vec + 8, &end) || beg <= 0x10000 || end < beg)
        return 0;
    uintptr_t count = (end - beg) / CAMP_REGISTRY_STRIDE;
    if (count > 512) return 0;
    for (uintptr_t i = 0; i < count; i++) {
        uintptr_t cls = 0;
        uint32_t h = 0;
        if (!safeReadPtr(beg + i * CAMP_REGISTRY_STRIDE, &cls) || cls <= 0x10000) continue;
        if (safeReadU32(cls + CAMP_SKILL_HASH_OFF, &h) && h == wantHash) return cls;
    }
    return 0;
}

struct HaStep {
    uint8_t code;
    bool    purchased;
    bool    armed;          // URD+0xc8 != 0: the game will act on the click
    bool    armedKnown;
    int     amount;         // the first currency's amount (-1 = unreadable)
    char    cur[40];        // its type id ("gold", "crest", ...)
    int     amount2;
    char    cur2[40];
    int     resolveReq;     // required resolve level (-1 = none/unreadable)
};
struct HaTree {
    uintptr_t utd;
    uint32_t  hash;
    char      id[48];               // "crusader.weapon"
    char      name[TM_NAME_MAX];    // localized upgrade_tree_name_<id> ("Greatsword")
    int       steps;
    int       urdSeen;              // how many of them the panel actually drew
    int       bought;               // purchased steps (from the save-backed map)
    int       next;                 // index of the step to buy next, -1 = nothing left
    int       kind;                 // HaKind
    bool      iconBuys;
    int64_t   iconElem;             // ...and this is that element's id
    int       skillIdx;             // combat/camping roster index (-1 = not that kind)
    int       level;
    bool      selected;             // combat/camping: taken into a raid
    HaStep    step[HA_MAX_STEPS];
};

static HaTree g_haTrees[HA_MAX_TREES];
static uint32_t g_haProbedGuid = 0;      // the hero whose columns were last logged
static uint32_t g_haSeenGuid = 0;        // ...and the one the previous read saw
static int      g_haColProbes = 0;       // capped, like every other one-shot dump
static int haTrees(uintptr_t base, uintptr_t panel, uintptr_t hero, bool probe) {
    HaTree* out = g_haTrees;
    const int max = HA_MAX_TREES;
    uintptr_t heroClass = 0;
    if (hero) safeReadPtr(hero + ACTOR_HEROCLASS_OFF, &heroClass);
    uint32_t probeGuid = 0;
    if (hero) safeReadU32(hero + ACTOR_ID_OFF, &probeGuid);
    if (!probe && probeGuid) {
        if (probeGuid != g_haSeenGuid) {
            g_haSeenGuid = probeGuid;                 // the pick frame: only remember it
        } else if (probeGuid != g_haProbedGuid && g_haColProbes < 12) {
            probe = true;
            g_haProbedGuid = probeGuid;
            g_haColProbes++;
        }
    }
    uintptr_t utds[HA_MAX_TREES];
    int total = 0;
    const int vecs = (haSkinOf(base, panel) == 2) ? 2 : 1;
    for (int v = 0; v < vecs; v++) {
        uintptr_t beg = 0, end = 0;
        uintptr_t at = panel + (v == 0 ? HA_TREES_BEG : HA_TREES2_BEG);
        if (!safeReadPtr(at, &beg) || !safeReadPtr(at + 8, &end) ||
            !beg || end < beg || (end - beg) % 8) {
            if (v == 0) return 0;
            logLine("heroaction: camping trainer shared-skill vector unreadable (beg=%p end=%p)",
                    (void*)beg, (void*)end);
            continue;
        }
        int count = (int)((end - beg) / 8);
        int got = 0;
        for (int i = 0; i < count && total < HA_MAX_TREES; i++) {
            uintptr_t utd = 0;
            if (!safeReadPtr(beg + (uintptr_t)i * 8, &utd) || utd <= 0x10000) continue;
            utds[total++] = utd;
            got++;
        }
        if (probe && v == 1)
            logLine("heroaction: camping trainer shared-skill grid (panel+0x288) holds %d tree(s), "
                    "%d readable", count, got);
    }
    int n = 0;
    for (int i = 0; i < total && n < max; i++) {
        uintptr_t utd = utds[i];
        HaTree t;
        memset(&t, 0, sizeof t);
        t.utd = utd;
        t.next = -1;
        t.skillIdx = -1;
        t.level = -1;
        if (!safeReadU32(utd + BLD_UTD_HASH_OFF, &t.hash) || !t.hash) continue;
        if (!bldRegistryId(base, t.hash, t.id, sizeof t.id)) t.id[0] = 0;
        if (t.id[0]) {
            char key[80];
            _snprintf(key, sizeof key, "upgrade_tree_name_%s", t.id);
            key[sizeof key - 1] = 0;
            if (!resolveKey(base, key, t.name, sizeof t.name) || !t.name[0]) {
                const char* dot = strchr(t.id, '.');
                strncpy(t.name, dot ? dot + 1 : t.id, sizeof t.name - 1);
                t.name[sizeof t.name - 1] = 0;
                for (char* p = t.name; *p; p++) if (*p == '_') *p = ' ';
            }
            abStripMarkup(t.name);
        } else {
            _snprintf(t.name, sizeof t.name, axs(AXS_OPTION_N), n + 1);
        }

        uintptr_t rec = haTreeRecord(base, t.hash);
        uint8_t instanced = 0;
        if (rec) safeReadU8(rec + HA_REG_INSTANCED, &instanced);
        uint32_t heroGuid = 0;
        if (hero) safeReadU32(hero + ACTOR_ID_OFF, &heroGuid);

        uint32_t iconField = 0;
        safeReadU32(utd + BLD_TREE_ELEM_OFF, &iconField);
        t.iconElem = (int64_t)(t.hash + iconField);
        uintptr_t cfg = 0;
        uint32_t iconBuys = 0;
        if (safeReadPtr(utd + HA_UTD_CFG_OFF, &cfg) && cfg > 0x10000)
            safeReadU32(cfg + HA_CFG_ICONBUYS, &iconBuys);
        t.iconBuys = iconBuys != 0;

        uint8_t codes[HA_MAX_STEPS];
        t.steps = haRegSteps(base, t.hash, codes, HA_MAX_STEPS);
        for (int k = 0; k < t.steps; k++) {
            HaStep* st = &t.step[k];
            memset(st, 0, sizeof *st);
            st->code = codes[k];
            st->armedKnown = false;
            st->amount = st->amount2 = -1;
            st->resolveReq = -1;
            st->purchased = haPurchased(base, t.hash, codes[k], instanced ? heroGuid : 0);
            if (st->purchased) t.bought++;
            else if (t.next < 0) t.next = k;

            uintptr_t req = haRequirement(base, t.hash, codes[k]);
            if (!req) continue;
            uint32_t lvl = 0;
            if (safeReadU32(req + HA_REQ_RESOLVE, &lvl) && lvl < 32) st->resolveReq = (int)lvl;
            uintptr_t cb = 0, ce = 0;
            if (safeReadPtr(req + HA_REQ_COST_BEG, &cb) &&
                safeReadPtr(req + HA_REQ_COST_END, &ce) && cb && ce > cb &&
                (ce - cb) % HA_COST_STRIDE == 0) {
                int nc = (int)((ce - cb) / HA_COST_STRIDE);
                for (int c = 0, kept = 0; c < nc && kept < 2; c++) {
                    uintptr_t cost = cb + (uintptr_t)c * HA_COST_STRIDE;
                    uint32_t amt = 0;
                    char type[40] = {0};
                    if (!safeReadU32(cost + HA_COST_AMOUNT, &amt)) continue;
                    if (!safeReadCStr(cost + HA_COST_TYPE, type, sizeof type)) continue;
                    if ((int)amt <= 0 || !type[0]) continue;        // "gold: 0" rows exist
                    if (kept == 0) {
                        st->amount = (int)amt;
                        strncpy(st->cur, type, sizeof st->cur - 1);
                    } else {
                        st->amount2 = (int)amt;
                        strncpy(st->cur2, type, sizeof st->cur2 - 1);
                    }
                    kept++;
                }
            }
        }

        // The live flags from whatever the panel drew, matched by code.
        uintptr_t ub = 0, ue = 0;
        if (safeReadPtr(utd + BLD_UTD_URDVEC_OFF, &ub) &&
            safeReadPtr(utd + BLD_UTD_URDVEC_OFF + 8, &ue) && ub && ue > ub &&
            (ue - ub) % HA_URD_STRIDE == 0) {
            int ns = (int)((ue - ub) / HA_URD_STRIDE);
            for (int u = 0; u < ns; u++) {
                uintptr_t urd = ub + (uintptr_t)u * HA_URD_STRIDE, vft = 0;
                uint32_t uhash = 0, armed = 0;
                uint8_t code = 0;
                if (!safeReadPtr(urd, &vft) || vft != base + BLD_URD_VFT_RVA) continue;
                if (!safeReadU32(urd + BLD_URD_HASH_OFF, &uhash) || uhash != t.hash) continue;
                if (!safeReadU8(urd + BLD_URD_CODE_OFF, &code)) continue;
                safeReadU32(urd + BLD_URD_ARMED_OFF, &armed);
                t.urdSeen++;
                for (int k = 0; k < t.steps; k++)
                    if (t.step[k].code == code) {
                        t.step[k].armed = armed != 0;
                        t.step[k].armedKnown = true;
                        break;
                    }
            }
        }
        if (probe) {
            uint32_t utdShown = 0xffffffff, panelShown = 0xffffffff;
            safeReadU32(utd + TMB_SHOWN_OFF, &utdShown);
            safeReadU32(panel + TMB_SHOWN_OFF, &panelShown);
            uintptr_t vb = 0, ve = 0;
            safeReadPtr(utd + BLD_UTD_URDVEC_OFF, &vb);
            safeReadPtr(utd + BLD_UTD_URDVEC_OFF + 8, &ve);
            logLine("heroaction: tree \"%s\" utd=%p shown=%u (panel shown=%u) urdvec=%p..%p "
                    "(%lld bytes = %lld x 0x180)", t.id, (void*)utd, utdShown, panelShown,
                    (void*)vb, (void*)ve, (long long)(ve - vb),
                    (long long)((ve > vb) ? (ve - vb) / HA_URD_STRIDE : 0));
            for (int k = 0; k < t.steps; k++) {
                int64_t elem = (int64_t)(t.hash + BLD_STEP_ID_TAG + (uint32_t)t.step[k].code);
                logLine("heroaction: tree \"%s\" step '%c' purchased=%d armed=%s cost=%d %s "
                        "resolve=%d elem=0x%llx live=%d", t.id, t.step[k].code ? t.step[k].code : '?',
                        t.step[k].purchased ? 1 : 0,
                        t.step[k].armedKnown ? (t.step[k].armed ? "1" : "0") : "unknown",
                        t.step[k].amount, t.step[k].cur, t.step[k].resolveReq,
                        (unsigned long long)elem, feGetElementById(elem) ? 1 : 0);
            }
            if (t.urdSeen != t.steps) {
                logLine("heroaction: tree \"%s\" icon elem=0x%llx buys-first-step=%d live=%d",
                        t.id, (unsigned long long)t.iconElem, t.iconBuys ? 1 : 0,
                        feGetElementById(t.iconElem) ? 1 : 0);
                uint32_t elemField = 0;
                safeReadU32(utd + BLD_TREE_ELEM_OFF, &elemField);
                logLine("heroaction: tree \"%s\" defines %d step(s) but the panel drew %d; "
                        "treeElemField=0x%x live(field)=%d live(hash)=%d live(hash+field)=%d",
                        t.id, t.steps, t.urdSeen, elemField,
                        feGetElementById((int64_t)elemField) ? 1 : 0,
                        feGetElementById((int64_t)t.hash) ? 1 : 0,
                        feGetElementById((int64_t)(t.hash + elemField)) ? 1 : 0);
            }
        }

        if (strstr(t.id, ".weapon"))      t.kind = HA_WEAPON;
        else if (strstr(t.id, ".armour")) t.kind = HA_ARMOUR;
        else if (heroClass) {
            int ci = haCombatIndexOf(base, heroClass, t.id);
            if (ci >= 0) { t.kind = HA_COMBAT; t.skillIdx = ci; }
            else {
                int cm = haCampIndexOf(base, heroClass, t.id);
                if (cm >= 0) { t.kind = HA_CAMP; t.skillIdx = cm; }
            }
        }

        if (t.kind == HA_CAMP) {
            uintptr_t ccls = haCampClassOf(base, t.id);
            char cname[TM_NAME_MAX];
            if (ccls && csCampSkillName(base, ccls, cname, sizeof cname) && cname[0]) {
                strncpy(t.name, cname, sizeof t.name - 1);
                t.name[sizeof t.name - 1] = 0;
            } else {
                logLine("heroaction: camping tree \"%s\" has no localized name (cls=%p) -- "
                        "falling back to \"%s\"", t.id, (void*)ccls, t.name);
            }
        }

        int liveNext = -1;                       // the step the hero's own state says comes next
        switch (t.kind) {
            case HA_WEAPON:
            case HA_ARMOUR: {
                uint32_t worn = 0;
                if (hero && safeReadU32(hero + (t.kind == HA_WEAPON ? HERO_WEAPON_LEVEL_OFF
                                                                    : HERO_ARMOUR_LEVEL_OFF),
                                        &worn) && (int)worn >= 0 && worn <= 16) {
                    t.level = (int)worn;         // tier 0 = the starting item
                    liveNext = (int)worn;        // step index N takes tier N -> N+1
                }
                break;
            }
            case HA_COMBAT: {
                t.level = haCombatLevelOf(base, hero, heroClass, t.skillIdx);
                t.selected = haCombatEquippedOf(base, hero, t.skillIdx);
                liveNext = (t.level < 0) ? 0 : t.level + 1;   // step '0' is LEARNING the skill
                break;
            }
            case HA_CAMP: {
                bool learned = haCampLearnedOf(base, hero, t.skillIdx);
                t.level = learned ? 0 : -1;
                liveNext = learned ? -1 : 0;
                break;
            }
            default: break;
        }
        if (liveNext >= t.steps) liveNext = -1;                // nothing left on this track
        if (liveNext != t.next && probe)
            logLine("heroaction: \"%s\" purchase map says next=%d, the hero's own state says %d "
                    "(level=%d) — using the hero", t.id, t.next, liveNext, t.level);
        if (t.kind != HA_OTHER) t.next = liveNext;             // the hero's field wins

        if (probe)
            logLine("heroaction: column %d \"%s\" hash=0x%x kind=%d idx=%d level=%d selected=%d "
                    "steps=%d bought=%d next=%d name=\"%s\"", n + 1, t.id, t.hash, t.kind,
                    t.skillIdx, t.level, t.selected ? 1 : 0, t.steps, t.bought, t.next, t.name);
        out[n++] = t;
    }
    return n;
}

struct HaHero { uintptr_t entry, hero, rowWidget; uint32_t guid; };
static HaHero g_haHeroes[HA_MAX_HEROES];
static int haHeroRows(uintptr_t base) {
    HaHero* out = g_haHeroes;
    const int max = HA_MAX_HEROES;
    uintptr_t root = resTownRoot(base);
    if (!root) return 0;
    uintptr_t beg = 0, end = 0;
    if (!safeReadPtr(root + ACT_RL_OFF + ACT_RL_ROWS_BEG, &beg) ||
        !safeReadPtr(root + ACT_RL_OFF + ACT_RL_ROWS_END, &end) || !beg || end <= beg) return 0;
    int rows = (int)((end - beg) / 0x40);
    if (rows > HA_MAX_HEROES) rows = HA_MAX_HEROES;
    int n = 0;
    for (int i = 0; i < rows && n < max; i++) {
        uintptr_t rowW = 0, entry = 0;
        uint32_t state = 0, guid = 0;
        if (!safeReadPtr(beg + (uintptr_t)i * 0x40, &rowW) || rowW <= 0x10000) continue;
        if (!safeReadPtr(rowW + ACT_RL_ROW_ENTRY, &entry) || entry <= 0x10000) continue;
        if (!safeReadU32(entry + ACT_ENTRY_STATE_OFF, &state) || (state != 0 && state != 1)) continue;
        if (!safeReadU32(entry, &guid)) continue;
        out[n].entry = entry;
        out[n].hero = entry + ACT_ENTRY_HERO_OFF;
        out[n].rowWidget = rowW;
        out[n].guid = guid;
        n++;
    }
    return n;
}

typedef void (*HaPickFn)(void* closure, uintptr_t* rosterRow);
static bool haSehPick(uintptr_t base, uintptr_t panel, uintptr_t rowWidget) {
    struct { uintptr_t vft; uintptr_t panel; } c;
    c.vft = base + HA_PICK_VFT_RVA;
    c.panel = panel;
    uintptr_t arg = rowWidget;
    __try { ((HaPickFn)(base + HA_PICK_RVA))(&c, &arg); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

static int  g_haRow = 0;                // the hero row cursor
static int  g_haCol = 0;
static DWORD g_haBuyWatchUntil = 0;     // Enter's purchase watch; 0 = idle
static uint32_t g_haBuyHash = 0;        // the tree it was spent on
static uint8_t  g_haBuyCode = 0;
static uint32_t g_haBuyGuid = 0;
static char  g_haBuyName[TM_NAME_MAX];
static int   g_haBuyGoldBefore = 0;

static const char* haFreeTagOf(const HaTree* t) {
    if (!t) return nullptr;
    if (t->kind == HA_WEAPON) return "weapon";
    if (t->kind == HA_ARMOUR) return "armour";
    return nullptr;
}

static void haCostText(uintptr_t base, uint32_t treeHash, const HaStep* st, char* out, int outsz) {
    out[0] = 0;
    if (!st || st->amount <= 0) return;
    int amt1 = haStepPrice(base, treeHash, st->amount);
    int amt2 = haStepPrice(base, treeHash, st->amount2);
    char t1[48], t2[48];
    resCurrencyTitleById(base, st->cur, "heroupgrade", t1, sizeof t1);
    if (st->amount2 > 0 && st->cur2[0]) {
        resCurrencyTitleById(base, st->cur2, "heroupgrade", t2, sizeof t2);
        _snprintf(out, outsz, axs(AXS_BLD_COST_PAIR_FMT), amt1, t1, amt2, t2);
    } else {
        _snprintf(out, outsz, "%d %s", amt1, t1);
    }
    out[outsz - 1] = 0;
}

static bool haLockReason(uintptr_t base, uintptr_t hero, const HaTree* t, const HaStep* st,
                         char* out, int outsz) {
    out[0] = 0;
    if (st->resolveReq > 0) {
        int level = csResolveLevel(base, hero);
        if (level >= 0 && level < st->resolveReq) {
            _snprintf(out, outsz, axs(AXS_HA_NEED_RESOLVE_FMT), st->resolveReq, level);
            out[outsz - 1] = 0;
            return true;
        }
    }
    uintptr_t req = haRequirement(base, t->hash, st->code);
    uintptr_t pb = 0, pe = 0;
    if (!req || !safeReadPtr(req + HA_REQ_PRQ_BEG, &pb) ||
        !safeReadPtr(req + HA_REQ_PRQ_END, &pe) || !pb || pe <= pb || (pe - pb) % 8) return false;
    int np = (int)((pe - pb) / 8);
    if (np > 16) return false;
    uint32_t guid = 0;
    safeReadU32(hero + ACTOR_ID_OFF, &guid);
    for (int i = 0; i < np; i++) {
        uint32_t phash = 0;
        uint8_t  pcode = 0, instanced = 0;
        if (!safeReadU32(pb + (uintptr_t)i * 8, &phash)) continue;
        if (!safeReadU8(pb + (uintptr_t)i * 8 + 4, &pcode)) continue;
        uintptr_t prec = haTreeRecord(base, phash);
        if (prec) safeReadU8(prec + HA_REG_INSTANCED, &instanced);
        if (haPurchased(base, phash, pcode, instanced ? guid : 0)) continue;
        char pid[48], pname[TM_NAME_MAX], key[80];
        if (!bldRegistryId(base, phash, pid, sizeof pid)) {
            _snprintf(out, outsz, "%s", axs(AXS_HA_NEEDS_UNNAMED));
            out[outsz - 1] = 0;
            return true;
        }
        _snprintf(key, sizeof key, "upgrade_tree_name_%s", pid);
        key[sizeof key - 1] = 0;
        if (!resolveKey(base, key, pname, sizeof pname) || !pname[0]) {
            const char* dot = strchr(pid, '.');
            strncpy(pname, dot ? dot + 1 : pid, sizeof pname - 1);
            pname[sizeof pname - 1] = 0;
            for (char* p = pname; *p; p++) if (*p == '_') *p = ' ';
        }
        abStripMarkup(pname);
        _snprintf(out, outsz, axs(AXS_HA_NEEDS_UPGRADE_FMT), pname);
        out[outsz - 1] = 0;
        return true;
    }
    return false;
}

static bool haStepRefused(uintptr_t base, uintptr_t hero, const HaTree* t, const HaStep* st,
                          char* reason, int rsz) {
    reason[0] = 0;
    if (!t || !st) return false;
    bool why  = haLockReason(base, hero, t, st, reason, rsz);
    bool flag = st->armedKnown && !st->armed;
    if (flag != why) {
        static uint32_t lastHash = 0;
        static uint8_t  lastCode = 0;
        static int      lastPair = -1;
        int pair = (flag ? 2 : 0) | (why ? 1 : 0);
        if (lastHash != t->hash || lastCode != st->code || lastPair != pair) {
            lastHash = t->hash; lastCode = st->code; lastPair = pair;
            logLine("heroaction: tree \"%s\" step '%c' — the panel's flag says %s, the data says "
                    "%s%s — going with the data", t->id, st->code ? st->code : '?',
                    flag ? "locked" : "buyable", why ? "locked: " : "buyable", why ? reason : "");
        }
    }
    if (flag && why) return true;
    reason[0] = 0;
    return false;
}

static void haColumnHead(const HaTree* t, char* head, int outsz) {
    switch (t->kind) {
        case HA_WEAPON:
        case HA_ARMOUR:
            _snprintf(head, outsz,
                      axs(t->kind == HA_WEAPON ? AXS_HA_COL_WEAPON_FMT : AXS_HA_COL_ARMOUR_FMT),
                      t->name, t->level < 0 ? 0 : t->level, t->steps);
            break;
        case HA_COMBAT:
            if (t->level < 0)
                _snprintf(head, outsz, axs(AXS_HA_COL_NOTLEARNED_FMT), t->name);
            else if (t->selected)
                _snprintf(head, outsz, axs(AXS_HA_COL_SELECTED_RANK_FMT),
                          t->name, t->level, t->steps - 1);
            else
                _snprintf(head, outsz, axs(AXS_HA_COL_RANK_FMT),
                          t->name, t->level, t->steps - 1);
            break;
        case HA_CAMP:
            _snprintf(head, outsz,
                      axs(t->level >= 0 ? AXS_HA_COL_LEARNED_FMT : AXS_HA_COL_NOTLEARNED_FMT),
                      t->name);
            break;
        default:
            _snprintf(head, outsz, axs(AXS_HA_COL_BOUGHT_FMT), t->name, t->bought, t->steps);
            break;
    }
    head[outsz - 1] = 0;
}

static void haColumnText(uintptr_t base, uintptr_t hero, const HaTree* t,
                         int col, int ncols, char* out, int outsz) {
    char cost[128], lock[192], head[320];
    cost[0] = lock[0] = 0;
    bool refused = false;
    const HaStep* nx = (t->next >= 0 && t->next < t->steps) ? &t->step[t->next] : nullptr;
    const char* freeTag = haFreeTagOf(t);
    bool freeUp = freeTag && townEventFreeUpgrades(base, freeTag) > 0;
    if (nx) {
        if (!freeUp) haCostText(base, t->hash, nx, cost, sizeof cost);
        refused = haStepRefused(base, hero, t, nx, lock, sizeof lock);
    }
    haColumnHead(t, head, sizeof head);

    char next[320];
    if (!nx) {
        _snprintf(next, sizeof next, " %s", axs(AXS_HA_NOTHING_LEFT));
    } else if (refused) {
        char willCost[160];
        willCost[0] = 0;
        if (cost[0]) _snprintf(willCost, sizeof willCost, axs(AXS_HA_WILL_COST_FMT), cost);
        willCost[sizeof willCost - 1] = 0;
        _snprintf(next, sizeof next, " %s%s%s%s%s", axs(AXS_HA_COL_CANT_YET),
                  lock[0] ? " " : "", lock,
                  willCost[0] ? " " : "", willCost);
    } else {
        bool tier  = (t->kind == HA_WEAPON || t->kind == HA_ARMOUR);
        bool learn = (t->kind == HA_CAMP) || (t->level < 0);
        if (cost[0]) {
            char sentence[288];
            _snprintf(sentence, sizeof sentence,
                      axs(tier ? AXS_HA_NEXT_TIER_COSTS_FMT
                               : learn ? AXS_HA_LEARN_COSTS_FMT : AXS_HA_NEXT_RANK_COSTS_FMT),
                      cost);
            sentence[sizeof sentence - 1] = 0;
            _snprintf(next, sizeof next, " %s", sentence);
        } else {
            _snprintf(next, sizeof next, " %s",
                      axs(tier ? AXS_HA_NEXT_TIER_FREE
                               : learn ? AXS_HA_LEARN_FREE : AXS_HA_NEXT_RANK_FREE));
        }
    }
    next[sizeof next - 1] = 0;

    char colPos[64];
    _snprintf(colPos, sizeof colPos, axs(AXS_HA_COL_POS_FMT), col + 1, ncols);
    colPos[sizeof colPos - 1] = 0;
    _snprintf(out, outsz, "%s%s %s", head, next, colPos);
    out[outsz - 1] = 0;
}

static void haHeroLine(uintptr_t base, uintptr_t hero, char* out, int outsz) {
    char name[80] = {0}, cls[80] = {0};
    abHeroNameClassOf(base, hero, name, sizeof name, cls, sizeof cls);
    int level = csResolveLevel(base, hero);
    const char* who = name[0] ? name : axs(AXS_HERO_UNREADABLE);
    if (level >= 0 && cls[0])      _snprintf(out, outsz, axs(AXS_HA_HERO_LVL_CLS_FMT),
                                             who, level, cls);
    else if (level >= 0)           _snprintf(out, outsz, axs(AXS_HA_HERO_LVL_FMT), who, level);
    else if (cls[0])               _snprintf(out, outsz, axs(AXS_HA_HERO_CLS_FMT), who, cls);
    else                           _snprintf(out, outsz, "%s", who);
    out[outsz - 1] = 0;
}

static bool haSpeakCell(uintptr_t base, uintptr_t panel, const char* prefix, bool sayHero) {
    int skin = haSkinOf(base, panel);
    if (skin < 0) return false;
    int nh = haHeroRows(base);
    char row[MAILBOX_SZ], utter[MAILBOX_SZ];
    if (nh <= 0) {
        _snprintf(row, sizeof row, "%s", axs(AXS_HA_NO_HERO));
    } else {
        if (g_haRow >= nh) g_haRow = nh - 1;
        if (g_haRow < 0)   g_haRow = 0;
        const HaHero* h = &g_haHeroes[g_haRow];
        if (g_haCol == 0) {
            char line[256], pos[64];
            haHeroLine(base, h->hero, line, sizeof line);
            _snprintf(pos, sizeof pos, axs(AXS_HA_HERO_POS_FMT), g_haRow + 1, nh);
            pos[sizeof pos - 1] = 0;
            _snprintf(row, sizeof row, "%s. %s", line, pos);
        } else {
            int nt = (haSelectedGuid(base, panel) == h->guid)
                     ? haTrees(base, panel, h->hero, false) : 0;
            if (nt <= 0) {
                _snprintf(row, sizeof row, "%s", axs(AXS_HA_NOTHING_FOR_HERO));
                g_haCol = 0;
            } else {
                if (g_haCol > nt) g_haCol = nt;
                char cell[MAILBOX_SZ];
                haColumnText(base, h->hero, &g_haTrees[g_haCol - 1], g_haCol - 1, nt,
                             cell, sizeof cell);
                if (sayHero) {
                    char who[80] = {0};
                    safeReadCStr(h->hero + HERO_NAME_OFF, who, sizeof who);
                    _snprintf(row, sizeof row, "%s%s%s",
                              who[0] ? who : "", who[0] ? ". " : "", cell);
                } else {
                    _snprintf(row, sizeof row, "%s", cell);
                }
            }
        }
    }
    row[sizeof row - 1] = 0;
    _snprintf(utter, sizeof utter, "%s%s", prefix ? prefix : "", row);
    utter[sizeof utter - 1] = 0;
    postSpeech(utter);
    return true;
}

static bool haSpeakCellPfx(uintptr_t base, uintptr_t panel, AxStrId pfxId, bool sayHero) {
    char pfx[96];
    _snprintf(pfx, sizeof pfx, "%s ", axs(pfxId));
    pfx[sizeof pfx - 1] = 0;
    return haSpeakCell(base, panel, pfx, sayHero);
}

// ---- the column's TOOLTIP: two buffer columns, "now" and "if you buy it" ----
#define HA_TIP_COLS 2
static char g_haTip[HA_TIP_COLS][AB_TIP_MAX_LINES][AB_TIP_LINE_SZ];
static int  g_haTipCount[HA_TIP_COLS];
static int  g_haTipCol = 0;                 // which buffer column
static int  g_haTipLine = -1;
static uint32_t g_haTipTree = 0;
static int  g_haTipRow = -1;

static int haEquipTipLines(uintptr_t base, uintptr_t hero, int kind, int tier,
                           char lines[AB_TIP_MAX_LINES][AB_TIP_LINE_SZ], int at) {
    uintptr_t cls = 0;
    if (!hero || !safeReadPtr(hero + ACTOR_HEROCLASS_OFF, &cls) || cls <= 0x10000) return 0;
    bool weapon = (kind == HA_WEAPON);
    uintptr_t arr = 0;
    if (!safeReadPtr(cls + (weapon ? HEROCLASS_WEAPON_VEC : HEROCLASS_ARMOUR_VEC), &arr) ||
        arr <= 0x10000) return 0;
    if (tier < 0 || tier > 16) return 0;
    uintptr_t rec = arr + (uintptr_t)tier * (weapon ? EQUIP_WEAPON_STRIDE : EQUIP_ARMOUR_STRIDE);
    uint32_t slot = 0;
    if (!safeReadU32(rec + EQUIP_SLOT_OFF, &slot) || slot != (uint32_t)(weapon ? 0 : 1)) {
        logLine("heroaction tip: tier %d record's slot field reads %u — refusing", tier, slot);
        return 0;
    }
    char raw[128] = {0}, name[256] = {0};
    if (!safeReadCStr(rec + EQUIP_NAME_OFF, raw, sizeof raw) || !raw[0]) return 0;
    if (!resolveKey(base, raw, name, sizeof name) || !name[0]) {
        strncpy(name, raw, sizeof name - 1);
        name[sizeof name - 1] = 0;
    }
    abStripMarkup(name);
    _snprintf(lines[at], AB_TIP_LINE_SZ, "%s. Tier %d.", name, tier);
    lines[at][AB_TIP_LINE_SZ - 1] = 0;

    char stats[AB_TIP_LINE_SZ], v[96];
    stats[0] = 0;
    if (weapon) {
        uint32_t lo = 0, hi = 0, spd = 0, critBits = 0;
        if (safeReadU32(rec + EQUIP_W_DMG_LO_OFF, &lo) &&
            safeReadU32(rec + EQUIP_W_DMG_HI_OFF, &hi)) {
            _snprintf(v, sizeof v, "%d - %d", (int)lo, (int)hi);
            csEquipStat(base, stats, sizeof stats, "str_stat_base_damage", v);
        }
        if (safeReadU32(rec + EQUIP_W_CRIT_OFF, &critBits)) {
            float crit; memcpy(&crit, &critBits, sizeof crit);
            _snprintf(v, sizeof v, "%.1f%%", crit * CS_PERCENT_SCALE);
            csEquipStat(base, stats, sizeof stats, "str_stat_base_crit", v);
        }
        if (safeReadU32(rec + EQUIP_W_SPD_OFF, &spd)) {
            float s; memcpy(&s, &spd, sizeof s);
            _snprintf(v, sizeof v, "%.0f", s);
            csEquipStat(base, stats, sizeof stats, "str_stat_base_speed", v);
        }
    } else {
        uint32_t defBits = 0, hpBits = 0;
        if (safeReadU32(rec + EQUIP_A_DEF_OFF, &defBits)) {
            float def; memcpy(&def, &defBits, sizeof def);
            _snprintf(v, sizeof v, "%.0f%%", def * CS_PERCENT_SCALE);
            csEquipStat(base, stats, sizeof stats, "str_stat_base_defense", v);
        }
        if (safeReadU32(rec + EQUIP_A_HP_OFF, &hpBits)) {
            float hp; memcpy(&hp, &hpBits, sizeof hp);      // FLOAT too
            _snprintf(v, sizeof v, "%.0f", hp);
            csEquipStat(base, stats, sizeof stats, "str_stat_base_health_points", v);
        }
    }
    if (!stats[0]) return 1;
    const char* p = stats;
    while (*p == ' ') p++;
    _snprintf(lines[at + 1], AB_TIP_LINE_SZ, "%s", p);
    lines[at + 1][AB_TIP_LINE_SZ - 1] = 0;
    return 2;
}

static bool haBuildTip(uintptr_t base, uintptr_t hero, const HaTree* t) {
    for (int c = 0; c < HA_TIP_COLS; c++) {
        g_haTipCount[c] = 0;
        for (int i = 0; i < AB_TIP_MAX_LINES; i++) g_haTip[c][i][0] = 0;
    }
    uintptr_t cls = 0;
    if (hero) safeReadPtr(hero + ACTOR_HEROCLASS_OFF, &cls);
    const HaStep* nx = (t->next >= 0 && t->next < t->steps) ? &t->step[t->next] : nullptr;

    if (t->kind == HA_WEAPON || t->kind == HA_ARMOUR) {
        int tier = t->level < 0 ? 0 : t->level;
        g_haTipCount[0] = haEquipTipLines(base, hero, t->kind, tier, g_haTip[0], 0);
        if (nx) g_haTipCount[1] = haEquipTipLines(base, hero, t->kind, tier + 1, g_haTip[1], 0);
    } else if (t->kind == HA_COMBAT && cls) {
        int shown = (t->level < 0) ? 0 : t->level;
        uintptr_t rec = haCombatRecordOf(base, cls, t->skillIdx, shown);
        if (rec) g_haTipCount[0] = abBuildSkillLines(base, rec, 0, t->name, g_haTip[0]);
        if (t->level < 0 && g_haTipCount[0] > 0) {
            // Say it is a preview, without losing the line the builder wrote.
            char first[AB_TIP_LINE_SZ];
            _snprintf(first, sizeof first, axs(AXS_HA_TIP_NOTLEARNED_FMT), g_haTip[0][0]);
            first[sizeof first - 1] = 0;
            strncpy(g_haTip[0][0], first, AB_TIP_LINE_SZ - 1);
            g_haTip[0][0][AB_TIP_LINE_SZ - 1] = 0;
        }
        if (nx) {
            if (t->level < 0) {                      // the next step IS learning it: say the deal
                char cost[128];
                haCostText(base, t->hash, nx, cost, sizeof cost);
                if (cost[0]) _snprintf(g_haTip[1][0], AB_TIP_LINE_SZ,
                                       axs(AXS_HA_LEARN_DEAL_FMT), cost);
                else         _snprintf(g_haTip[1][0], AB_TIP_LINE_SZ,
                                       "%s", axs(AXS_HA_LEARN_DEAL_FREE));
                g_haTipCount[1] = 1;
            } else {
                uintptr_t up = haCombatRecordOf(base, cls, t->skillIdx, t->level + 1);
                if (up) g_haTipCount[1] = abBuildSkillLines(base, up, 0, t->name, g_haTip[1]);
            }
        }
    } else if (t->kind == HA_CAMP) {
        uintptr_t ccls = haCampClassOf(base, t->id);
        if (ccls) {
            int cost = csCampSkillCost(ccls);
            if (cost > 0)
                _snprintf(g_haTip[0][0], AB_TIP_LINE_SZ, axs(AXS_HA_CAMP_COST_FMT), t->name, cost);
            else
                _snprintf(g_haTip[0][0], AB_TIP_LINE_SZ, "%s.", t->name);
            g_haTipCount[0] = 1 + csCampEffectLines(base, ccls, g_haTip[0], 1);
            g_haTipCount[0] += csCampUsesLine(base, ccls, 0, g_haTip[0], g_haTipCount[0]);
        }
    }

    if (g_haTipCount[0] == 0) {
        _snprintf(g_haTip[0][0], AB_TIP_LINE_SZ, "%s", axs(AXS_NO_MORE_INFO));
        g_haTipCount[0] = 1;
    }
    if (g_haTipCount[1] == 0) {
        char cost[128];
        cost[0] = 0;
        const char* tipFreeTag = haFreeTagOf(t);
        if (!(tipFreeTag && townEventFreeUpgrades(base, tipFreeTag) > 0))
            haCostText(base, t->hash, nx, cost, sizeof cost);
        if (!nx)            _snprintf(g_haTip[1][0], AB_TIP_LINE_SZ, "%s", axs(AXS_HA_NOTHING_LEFT));
        else if (cost[0])   _snprintf(g_haTip[1][0], AB_TIP_LINE_SZ,
                                      axs(AXS_HA_NEXT_STEP_COSTS_FMT), cost);
        else                _snprintf(g_haTip[1][0], AB_TIP_LINE_SZ, "%s", axs(AXS_NO_MORE_INFO));
        g_haTipCount[1] = 1;
    }
    g_haTipTree = t->hash;
    g_haTipRow = g_haRow;
    return true;
}

static int g_haDumps = 0;
static void haDumpElements(uintptr_t base, uintptr_t panel, const char* why) {
    if (g_haDumps >= 4) return;
    g_haDumps++;
    uint32_t panelShown = 0xffffffff, mode = 0;
    safeReadU32(panel + TMB_SHOWN_OFF, &panelShown);
    safeReadU32(panel + BLD_MODE_OFF, &mode);
    uintptr_t tb = 0, te = 0;
    safeReadPtr(panel + HA_TREES_BEG, &tb);
    safeReadPtr(panel + HA_TREES_END, &te);
    logLine("heroaction dump(%s): panel=%p shown=%u mode=%u trees=%lld selected guid=%u",
            why, (void*)panel, panelShown, mode,
            (long long)((te > tb) ? (te - tb) / 8 : 0), haSelectedGuid(base, panel));
    for (int i = 0; i < HA_MAX_TREES && tb + (uintptr_t)i * 8 < te; i++) {
        uintptr_t utd = 0;
        uint32_t shown = 0xffffffff, hash = 0;
        if (!safeReadPtr(tb + (uintptr_t)i * 8, &utd) || utd <= 0x10000) continue;
        safeReadU32(utd + TMB_SHOWN_OFF, &shown);
        safeReadU32(utd + BLD_UTD_HASH_OFF, &hash);
        logLine("heroaction dump(%s): tree[%d]=%p shown=%u hash=0x%x", why, i, (void*)utd, shown, hash);
    }
    if (haSkinOf(base, panel) == 2) {
        uintptr_t sb = 0, se = 0;
        safeReadPtr(panel + HA_TREES2_BEG, &sb);
        safeReadPtr(panel + HA_TREES2_END, &se);
        logLine("heroaction dump(%s): shared-skill grid trees=%lld", why,
                (long long)((se > sb) ? (se - sb) / 8 : 0));
        for (int i = 0; i < HA_MAX_TREES && sb + (uintptr_t)i * 8 < se; i++) {
            uintptr_t utd = 0;
            uint32_t shown = 0xffffffff, hash = 0;
            if (!safeReadPtr(sb + (uintptr_t)i * 8, &utd) || utd <= 0x10000) continue;
            safeReadU32(utd + TMB_SHOWN_OFF, &shown);
            safeReadU32(utd + BLD_UTD_HASH_OFF, &hash);
            logLine("heroaction dump(%s): shared tree[%d]=%p shown=%u hash=0x%x", why, i,
                    (void*)utd, shown, hash);
        }
    }
    uintptr_t fb = 0, fe = 0;
    if (!safeReadPtr(base + VEC_BEGIN_RVA, &fb) || !safeReadPtr(base + VEC_END_RVA, &fe) ||
        !fb || fe <= fb) { logLine("heroaction dump(%s): no focus vector", why); return; }
    uintptr_t count = (fe - fb) / ELEM_STRIDE;
    if (count > 512) count = 512;
    logLine("heroaction dump(%s): focus vector, %llu elements", why, (unsigned long long)count);
    for (uintptr_t i = 0; i < count; i++) {
        uintptr_t elem = fb + i * ELEM_STRIDE, eowner = 0;
        int64_t id = 0;
        if (!safeReadI64(elem + ELEM_ID_OFF, &id) || isNoFocus(id)) continue;
        safeReadPtr(elem + ELEM_OWNER_OFF, &eowner);
        uint32_t xb = 0, yb = 0, wb = 0, hb = 0;
        safeReadU32(elem + ELEM_POS_OFF,      &xb);
        safeReadU32(elem + ELEM_POS_OFF + 4,  &yb);
        safeReadU32(elem + ELEM_SIZE_OFF,     &wb);
        safeReadU32(elem + ELEM_SIZE_OFF + 4, &hb);
        char chars[10];
        tmbIdChars((uint32_t)(uint64_t)id, chars);
        logLine("heroaction dump(%s): elem id=0x%llx%s owner=%p pos=(%.0f,%.0f) size=(%.0f,%.0f)",
                why, (unsigned long long)id, chars, (void*)eowner,
                u32AsFloatM(xb), u32AsFloatM(yb), u32AsFloatM(wb), u32AsFloatM(hb));
    }
    logLine("heroaction dump(%s): end", why);
}

static bool haShowFocusedHero(uintptr_t base, uintptr_t panel, const HaHero* h) {
    if (haSelectedGuid(base, panel) == h->guid) return true;
    if (!haSehPick(base, panel, h->rowWidget)) {
        logLine("heroaction: pick body faulted (row=%p guid=%u)", (void*)h->rowWidget, h->guid);
        return false;
    }
    uint32_t now = haSelectedGuid(base, panel);
    logLine("heroaction: picked guid=%u -> facility says %u", h->guid, now);
    return now == h->guid;
}

static void haProbe(uintptr_t base, uintptr_t panel) {
    int skin = haSkinOf(base, panel);
    uintptr_t fac = haFacility(base, panel);
    uint32_t sel = haSelectedGuid(base, panel);
    int nh = haHeroRows(base);
    logLine("heroaction probe: \"%s\" skin=%d facility=%p selected guid=%u heroes=%d spare=%d",
            g_bldId, skin, (void*)fac, sel, nh, 0);
    for (int i = 0; i < nh && i < 8; i++) {
        char who[80] = {0};
        safeReadCStr(g_haHeroes[i].hero + HERO_NAME_OFF, who, sizeof who);
        logLine("heroaction probe: hero %d guid=%u \"%s\" row=%p", i, g_haHeroes[i].guid, who,
                (void*)g_haHeroes[i].rowWidget);
    }
    uintptr_t selHero = 0;
    for (int i = 0; i < nh; i++) if (g_haHeroes[i].guid == sel) selHero = g_haHeroes[i].hero;
    int nt = haTrees(base, panel, selHero, true);
    logLine("heroaction probe: %d option column(s) built for the selected hero (%p)",
            nt, (void*)selHero);
}

// ---- the BUILDING-NAV STRIP (the game's own switch-buildings sidebar) + the jump actions ----
static const uint32_t  BN_STRIP_ID_SPAN  = 0x40;      // strip ids live in [TMB_BLD_ID_BASE, +span)
static const uintptr_t BN_NODE_LEFT_OFF  = 0x00;      // map node: left child
static const uintptr_t BN_NODE_RIGHT_OFF = 0x10;      // map node: right child
static const uintptr_t BN_NODE_ISNIL_OFF = 0x19;      // map node: the sentinel byte
static const uintptr_t BN_NODE_HASH_OFF  = 0x5c;      // map node: the building id hash (the key)
static const uintptr_t BN_NODE_SLOT_OFF  = 0x70;      // map node: the strip slot index (the value)

static bool bnLayoutSlot(uintptr_t base, uint32_t hash, int* out) {
    uintptr_t head = 0, node = 0, best = 0;
    if (!safeReadPtr(base + BN_LAYOUT_MAP_RVA, &head) || head <= 0x10000) return false;
    if (!safeReadPtr(head + 0x08, &node) || node <= 0x10000) return false;   // head->parent = root
    for (int guard = 0; guard < 64; guard++) {
        uint8_t nil = 1;
        if (!safeReadU8(node + BN_NODE_ISNIL_OFF, &nil) || nil) break;
        uint32_t k = 0;
        if (!safeReadU32(node + BN_NODE_HASH_OFF, &k)) return false;
        if (k < hash) {
            if (!safeReadPtr(node + BN_NODE_RIGHT_OFF, &node) || node <= 0x10000) return false;
        } else {
            best = node;
            if (!safeReadPtr(node + BN_NODE_LEFT_OFF, &node) || node <= 0x10000) return false;
        }
    }
    if (!best) return false;
    uint32_t k = 0, slot = 0;
    if (!safeReadU32(best + BN_NODE_HASH_OFF, &k) || k != hash) return false;
    if (!safeReadU32(best + BN_NODE_SLOT_OFF, &slot) || slot > 63) return false;
    *out = (int)slot;
    return true;
}

static const int BN_MAX = 24;
struct BnRow {
    char     id[68];     // the building's data id
    uint32_t hash;
    int      slot;       // the layout map's strip slot (the sort key)
    int64_t  elemId;
    float    y;          // its drawn Y, for the log
};
static bool g_bnDumped     = false;   // one-shot zip log per session
static int  g_bnWarnBudget = 6;      // a refused zip dumps both sides; cap the pages

static int bnCollect(uintptr_t base, uintptr_t root, BnRow* rows, int max) {
    uintptr_t beg = 0, end = 0;
    if (!safeReadPtr(root + TM_BLD_VEC_BEG, &beg) || !safeReadPtr(root + TM_BLD_VEC_END, &end) ||
        !beg || end <= beg || (end - beg) % 8) return -1;
    uintptr_t n = (end - beg) / 8;
    if (n > 64) return -1;
    int cn = 0;
    for (uintptr_t i = 0; i < n; i++) {
        uintptr_t obj = 0, def = 0;
        if (!safeReadPtr(beg + i * 8, &obj) || obj <= 0x10000) continue;
        if (!safeReadPtr(obj + TM_BLD_DEF_OFF, &def) || def <= 0x10000) continue;
        BnRow r;
        if (!safeReadCStr(def + TM_DEF_ID_OFF, r.id, sizeof r.id) || !tmIdLooksValid(r.id))
            continue;
        uint8_t screen = 0, unlocked = 0;
        safeReadU8(def + TM_DEF_SCREEN_OFF, &screen);
        safeReadU8(obj + TM_BLD_UNLOCKED_OFF, &unlocked);
        if (!screen || !unlocked) continue;          // no screen / locked = not a strip element
        if (!safeReadU32(def + TM_DEF_HASH_OFF, &r.hash) || r.hash != resHash(r.id)) {
            if (g_bnWarnBudget > 0) {
                g_bnWarnBudget--;
                logLine("townjump strip: \"%s\" hash mismatch -> zip refused", r.id);
            }
            return -1;
        }
        if (!bnLayoutSlot(base, r.hash, &r.slot)) {
            if (g_bnWarnBudget > 0) {
                g_bnWarnBudget--;
                logLine("townjump strip: \"%s\" (0x%x) has no layout-map slot -> zip refused",
                        r.id, r.hash);
            }
            return -1;
        }
        r.elemId = 0;
        r.y = 0;
        if (cn >= max) return -1;                    // more rows than any real strip: bad read
        int at = cn;
        while (at > 0 && r.slot < rows[at - 1].slot) { rows[at] = rows[at - 1]; at--; }
        rows[at] = r;
        cn++;
    }
    // 2) the live strip elements, sorted by drawn Y
    struct { int64_t id; float x, y; } elems[BN_MAX];
    int en = 0;
    uintptr_t fb = 0, fe = 0;
    if (!safeReadPtr(base + VEC_BEGIN_RVA, &fb) || !safeReadPtr(base + VEC_END_RVA, &fe) ||
        !fb || fe <= fb) return -1;
    uintptr_t count = (fe - fb) / ELEM_STRIDE;
    if (count > 512) count = 512;
    for (uintptr_t i = 0; i < count; i++) {
        uintptr_t elem = fb + i * ELEM_STRIDE;
        int64_t id = 0;
        if (!safeReadI64(elem + ELEM_ID_OFF, &id) || isNoFocus(id)) continue;
        if (id < (int64_t)TMB_BLD_ID_BASE || id >= (int64_t)(TMB_BLD_ID_BASE + BN_STRIP_ID_SPAN))
            continue;
        uint32_t xb = 0, yb = 0;
        safeReadU32(elem + ELEM_POS_OFF,     &xb);
        safeReadU32(elem + ELEM_POS_OFF + 4, &yb);
        if (en >= BN_MAX) return -1;
        float x = u32AsFloatM(xb), y = u32AsFloatM(yb);
        int at = en;
        while (at > 0 && y < elems[at - 1].y) { elems[at] = elems[at - 1]; at--; }
        elems[at].id = id;
        elems[at].x = x;
        elems[at].y = y;
        en++;
    }
    // 3) the zip — refused out loud when the two sides disagree
    if (en != cn) {
        if (g_bnWarnBudget > 0) {
            g_bnWarnBudget--;
            logLine("townjump strip: %d candidates vs %d elements -> zip refused", cn, en);
            for (int i = 0; i < cn; i++)
                logLine("townjump strip:   cand[%d] slot=%d id=\"%s\" hash=0x%x",
                        i, rows[i].slot, rows[i].id, rows[i].hash);
            for (int i = 0; i < en; i++)
                logLine("townjump strip:   elem[%d] id=0x%llx pos=(%.0f,%.0f)",
                        i, (unsigned long long)elems[i].id, elems[i].x, elems[i].y);
        }
        return -1;
    }
    for (int i = 0; i < cn; i++) {
        rows[i].elemId = elems[i].id;
        rows[i].y      = elems[i].y;
    }
    if (!g_bnDumped && cn > 0) {
        g_bnDumped = true;                           // the model the letters trust, on record
        for (int i = 0; i < cn; i++)
            logLine("townjump strip: [%d] slot=%d id=\"%s\" elem=0x%llx y=%.0f",
                    i, rows[i].slot, rows[i].id, (unsigned long long)rows[i].elemId, rows[i].y);
    }
    return cn;
}

static bool g_tjKbLogged = false;
static void tjLogBindingsOnce(uintptr_t base) {
    if (g_tjKbLogged) return;
    g_tjKbLogged = true;
    char owner[96];
    for (int i = 0; i < TJ_KEY_COUNT; i++)
        logLine("townjump: key '%c' -> %s", (char)TJ_KEYS[i].sym,
                kbActionForKey(base, (int)TJ_KEYS[i].sym, owner, sizeof owner)
                    ? owner : "not bound in the game table");
    logLine("townjump: PageUp -> %s",
            kbActionForKey(base, (int)SDLK_PAGEUP, owner, sizeof owner)
                ? owner : "not bound in the game table");
    logLine("townjump: PageDown -> %s",
            kbActionForKey(base, (int)SDLK_PAGEDOWN, owner, sizeof owner)
                ? owner : "not bound in the game table");
}

static bool tjChainStart(uintptr_t base, const char* id, const char* why) {
    if (g_tjChainId[0] || g_tmOpenWatchUntil || g_bnSwitchUntil) {
        postSpeech(axs(AXS_TJ_STILL_OPENING));
        return true;
    }
    if (!frontEndClickElementId((int64_t)BLD_ELEM_BACK)) {
        logLine("townjump: chain(%s) -- the back button would not click", why);
        postSpeech(axs(AXS_ACTION_FAILED));
        return true;
    }
    strncpy(g_tjChainId, id, sizeof g_tjChainId - 1);
    g_tjChainId[sizeof g_tjChainId - 1] = 0;
    g_tjChainUntil = GetTickCount() + 2500;
    g_tjChainDue   = 0;
    logLine("townjump: chain(%s) -> left the building, the map will open \"%s\"", why, id);
    return true;
}

static bool tjStripCycle(uintptr_t base, uintptr_t root, uintptr_t panel, bool forward) {
    tjLogBindingsOnce(base);
    BnRow rows[BN_MAX];
    int n = bnCollect(base, root, rows, BN_MAX);
    if (n <= 0) {
        if (n == 0) logLine("townjump: the strip model is empty");
        postSpeech(axs(AXS_TJ_NO_STRIP));            // bnCollect already logged the refusal
        return true;
    }
    uint32_t hereHash = 0;
    if (!safeReadU32(panel + BLD_ID_HASH_OFF, &hereHash) || !hereHash)
        hereHash = resHash(g_bldId);
    int idx = -1;
    for (int i = 0; i < n; i++) if (rows[i].hash == hereHash) { idx = i; break; }
    if (idx < 0) {
        logLine("townjump: the open building 0x%x (\"%s\") is not on the strip model",
                hereHash, g_bldId);
        postSpeech(axs(AXS_TJ_NO_STRIP));
        return true;
    }
    if (n == 1) { postSpeech(axs(AXS_TJ_NO_OTHERS)); return true; }
    if (g_bnSwitchUntil || g_tjChainId[0]) { postSpeech(axs(AXS_TJ_STILL_OPENING)); return true; }
    const BnRow* t = &rows[(idx + (forward ? 1 : n - 1)) % n];
    char name[TM_NAME_MAX];
    tjTargetName(base, t->id, name, sizeof name);
    if (!frontEndClickElementId(t->elemId)) {
        char utter[TM_NAME_MAX + 48];
        logLine("townjump: strip element 0x%llx (\"%s\") would not click",
                (unsigned long long)t->elemId, t->id);
        _snprintf(utter, sizeof utter, axs(AXS_TJ_CANT_OPEN), name);
        utter[sizeof utter - 1] = 0;
        postSpeech(utter);
        return true;
    }
    g_bnSwitchHash = t->hash;
    strncpy(g_bnSwitchName, name, sizeof g_bnSwitchName - 1);
    g_bnSwitchName[sizeof g_bnSwitchName - 1] = 0;
    g_bnSwitchUntil = GetTickCount() + 2500;
    logLine("townjump: strip %s -> \"%s\" (elem 0x%llx, slot %d)", forward ? "next" : "previous",
            t->id, (unsigned long long)t->elemId, t->slot);
    return true;
}

static bool tjJumpFromBuilding(uintptr_t base, uintptr_t root, const char* id, uint8_t repeat) {
    uintptr_t panel = bldOpenPanel(root);
    if (!panel) return false;
    if (g_bldMode != 0) return false;                // the upgrade view keeps its own keys
    if (strcmp(id, g_bldId) == 0) {
        if (strcmp(id, "stage_coach") == 0) return false;
        if (repeat) return true;
        char name[TM_NAME_MAX], utter[TM_NAME_MAX + 32];
        tjTargetName(base, id, name, sizeof name);
        _snprintf(utter, sizeof utter, axs(AXS_TJ_ALREADY_OPEN), name);
        utter[sizeof utter - 1] = 0;
        postSpeech(utter);
        return true;
    }
    if (repeat) return true;
    tjLogBindingsOnce(base);
    char name[TM_NAME_MAX], utter[TM_NAME_MAX + 48];
    tjTargetName(base, id, name, sizeof name);

    if (strcmp(id, "circus") == 0) {
        // The travel tent is a MAP object; its element exists only on the bare map.
        if (tmCurrentLayer(root) == 1) { postSpeech(axs(AXS_TJ_ALREADY_THERE)); return true; }
        return tjChainStart(base, id, "circus travel");
    }

    if (strcmp(id, "district") == 0) {
        uintptr_t camp = 0;
        uint8_t enabled = 0, unlocked = 0, ovr = 0;
        if (!safeReadPtr(base + RES_CAMPAIGN_RVA, &camp) || camp <= 0x10000 ||
            !safeReadU8(camp + TM_CAMP_DISTRICTS_ENABLED, &enabled) || !enabled) {
            _snprintf(utter, sizeof utter, axs(AXS_TJ_NOT_HERE), name);
            utter[sizeof utter - 1] = 0;
            postSpeech(utter);
            return true;
        }
        safeReadU8(camp + TM_CAMP_DISTRICTS_UNLOCKED, &unlocked);
        safeReadU8(base + TM_DDIS_OVERRIDE_RVA, &ovr);
        if (!unlocked && !ovr) {
            _snprintf(utter, sizeof utter, axs(AXS_TJ_LOCKED), name);
            utter[sizeof utter - 1] = 0;
            postSpeech(utter);
            return true;
        }
        if (g_tmOpenWatchUntil || g_tjChainId[0]) { postSpeech(axs(AXS_TJ_STILL_OPENING)); return true; }
        if (frontEndClickElementId((int64_t)TM_DDIS_ELEM_ID)) {
            strncpy(g_tmOpenName, name, sizeof g_tmOpenName - 1);
            g_tmOpenName[sizeof g_tmOpenName - 1] = 0;
            g_tmOpenIsDistrict = false;
            g_tmOpenIsDdis     = true;               // checkTownMap watches axIsDistrict()
            g_tmOpenWatchUntil = GetTickCount() + 2000;
            logLine("townjump: clicked the estate bar's 'ddis' from \"%s\"", g_bldId);
            return true;
        }
        logLine("townjump: 'ddis' is not clickable inside \"%s\" -> chaining", g_bldId);
        return tjChainStart(base, id, "districts");
    }

    BnRow rows[BN_MAX];
    int n = bnCollect(base, root, rows, BN_MAX);
    uint32_t hash = resHash(id);
    if (n > 0) {
        for (int i = 0; i < n; i++) {
            if (rows[i].hash != hash) continue;
            if (g_bnSwitchUntil || g_tjChainId[0]) { postSpeech(axs(AXS_TJ_STILL_OPENING)); return true; }
            if (!frontEndClickElementId(rows[i].elemId)) {
                logLine("townjump: strip element 0x%llx (\"%s\") would not click -> chaining",
                        (unsigned long long)rows[i].elemId, id);
                return tjChainStart(base, id, "strip click failed");
            }
            g_bnSwitchHash = hash;
            strncpy(g_bnSwitchName, name, sizeof g_bnSwitchName - 1);
            g_bnSwitchName[sizeof g_bnSwitchName - 1] = 0;
            g_bnSwitchUntil = GetTickCount() + 2500;
            logLine("townjump: jump \"%s\" -> \"%s\" (strip elem 0x%llx)", g_bldId, id,
                    (unsigned long long)rows[i].elemId);
            return true;
        }
    }
    uintptr_t obj = bldBuildingObj(root, hash);
    if (obj) {
        uint8_t unlocked = 0;
        if (safeReadU8(obj + TM_BLD_UNLOCKED_OFF, &unlocked) && !unlocked) {
            _snprintf(utter, sizeof utter, axs(AXS_TJ_LOCKED), name);
            utter[sizeof utter - 1] = 0;
            postSpeech(utter);
            return true;
        }
        return tjChainStart(base, id, "no strip row");
    }
    _snprintf(utter, sizeof utter, axs(AXS_TJ_NOT_HERE), name);
    utter[sizeof utter - 1] = 0;
    postSpeech(utter);
    return true;
}

static bool tjJumpFromMap(uintptr_t base, uintptr_t root, const char* id, uint8_t repeat) {
    if (repeat) return true;
    tjLogBindingsOnce(base);
    if (strcmp(id, "circus") == 0 && tmCurrentLayer(root) == 1) {
        postSpeech(axs(AXS_TJ_ALREADY_THERE));
        return true;
    }
    int n = tmRows(base, false);
    for (int i = 0; i < n; i++) {
        if (strcmp(g_tmRows[i].id, id) != 0) continue;
        g_tmRow     = i;                             // the cursor follows the jump
        g_tmTipLine = 0;
        tmOpenRow(base, &g_tmRows[i]);
        return true;
    }
    char name[TM_NAME_MAX], utter[TM_NAME_MAX + 32];
    tjTargetName(base, id, name, sizeof name);
    logLine("townjump: \"%s\" is not on this map", id);
    _snprintf(utter, sizeof utter, axs(AXS_TJ_NOT_HERE), name);
    utter[sizeof utter - 1] = 0;
    postSpeech(utter);
    return true;
}

bool routeTownJumpKey(uintptr_t base, const char* id, bool fromBuilding, uint8_t repeat) {
    uintptr_t root = resTownRoot(base);
    if (!root) return false;
    if (fromBuilding) return tjJumpFromBuilding(base, root, id, repeat);
    return tjJumpFromMap(base, root, id, repeat);
}

bool routeBldKey(uintptr_t base, uint32_t sym, uint16_t mod, uint8_t repeat) {
    if (dgEditFieldOpen(base)) return false;

    if (g_bldRecDrag != 0) {
        uintptr_t root = resTownRoot(base);
        uintptr_t panel = root ? bldOpenPanel(root) : 0;
        if (!panel) { g_bldRecDrag = 0; return false; }   // the building vanished under it
        if (repeat) return true;
        if (g_bldRecDrag == 2 || g_bldRecWatchUntil) {    // the drag is flying: outcome pending
            if (sym == SDLK_RETURN || sym == SDLK_KP_ENTER || sym == SDLK_ESCAPE)
                postSpeech(axs(AXS_RCT_STILL_RECRUITING));
            return true;
        }
        if (sym == SDLK_ESCAPE) {
            g_bldRecDrag = 0;
            logLine("bldrows: recruit cancelled (slot %d)", g_bldRecSlot);
            {
                char pfx[64];
                _snprintf(pfx, sizeof pfx, "%s ", axs(AXS_CANCELLED));
                pfx[sizeof pfx - 1] = 0;
                if (!bldSpeakOptionRow(base, panel, pfx)) postSpeech(axs(AXS_CANCELLED));
            }
            return true;
        }
        if (sym == SDLK_RETURN || sym == SDLK_KP_ENTER) {
            g_bldRecEntriesBefore = bldRosterEntryCount(base);
            if (!bldStartRecruitDrag(base, g_bldRecSlot)) {
                g_bldRecDrag = 0;
                char pfx[96];
                _snprintf(pfx, sizeof pfx, "%s ", axs(AXS_RCT_CANT_NOW));
                pfx[sizeof pfx - 1] = 0;
                if (!bldSpeakOptionRow(base, panel, pfx))
                    postSpeech(axs(AXS_RCT_CANT_NOW));
                return true;
            }
            g_bldRecDrag = 2;
            g_bldRecWatchUntil = GetTickCount() + 2000;   // checkBuilding announces the outcome
            return true;
        }
        return true;                                      // every other key is blocked
    }

    if (g_qtOpen) {
        uintptr_t root = resTownRoot(base);
        uintptr_t panel = root ? bldOpenPanel(root) : 0;
        if (!panel) { g_qtOpen = false; return false; }   // the building vanished under it
        if ((sym == SDLK_UP || sym == SDLK_DOWN) &&
            !(mod & (KMOD_LCTRL | KMOD_RCTRL | KMOD_LALT | KMOD_RALT))) {
            if (axNavHoldRepeat(repeat)) return true;     // throttled repeat: claimed, no step
        } else if (repeat) return true;
        if (mod & (KMOD_LCTRL | KMOD_RCTRL)) {
            if (!(mod & (KMOD_LALT | KMOD_RALT)) && (sym == SDLK_UP || sym == SDLK_DOWN)) {
                QtCand cands[64];
                int cn = qtCandidates(base, g_qtOpenActivity, g_qtOpenSlot, g_qtOpenMode,
                                      cands, 64);
                char desc[CS_QUIRK_DESC_BUF];
                uintptr_t qc = 0;
                if (g_qtOpenRow > 0 && g_qtOpenRow <= cn)
                    qc = qtQuirkClass(g_qtOpenHero, cands[g_qtOpenRow - 1].id);
                if (qc && csQuirkDescRaw(base, qc, desc, sizeof desc) && desc[0]) {
                    csFlattenLines(desc, sizeof desc);
                    postSpeech(desc);
                } else {
                    postSpeech(axs(AXS_NO_MORE_INFO));
                }
            }
            return true;
        }
        if (sym == SDLK_ESCAPE) {
            g_qtOpen = false;
            {
                char pfx[64];
                _snprintf(pfx, sizeof pfx, "%s ", axs(AXS_QT_CLOSED));
                pfx[sizeof pfx - 1] = 0;
                if (!bldSpeakOptionRow(base, panel, pfx)) postSpeech(axs(AXS_QT_CLOSED));
            }
            return true;
        }
        {
            int jump = 0;
            if (axDecodeJump(sym, mod, repeat, &jump)) {   // Home/End: placeholder / last candidate
                if (!jump) return true;                    // held jump: one landing per press
                g_qtOpenRow = (jump > 0) ? AX_JUMP : 0;    // the speaker clamps (hard stops)
                qtSpeakOpenRow(base, nullptr);
                return true;
            }
        }
        if (sym == SDLK_UP || sym == SDLK_DOWN) {
            g_qtOpenRow += (sym == SDLK_DOWN) ? 1 : -1;    // the speaker clamps (hard stops)
            qtSpeakOpenRow(base, nullptr);
            return true;
        }
        if (sym == SDLK_RETURN || sym == SDLK_KP_ENTER) {
            QtCand cands[64];
            int cn = qtCandidates(base, g_qtOpenActivity, g_qtOpenSlot, g_qtOpenMode, cands, 64);
            if (g_qtOpenRow > cn) g_qtOpenRow = cn;
            if (g_qtOpenRow < 0)  g_qtOpenRow = 0;
            int otherMode = (g_qtOpenMode == QT_MODE_LOCK) ? QT_MODE_REMOVE : QT_MODE_LOCK;
            char otherBefore[MAILBOX_SZ];
            qtDropState(base, g_qtOpenActivity, g_qtOpenHero, g_qtOpenSlot, otherMode,
                        otherBefore, sizeof otherBefore, nullptr, nullptr);

            if (g_qtOpenRow == 0) {                        // the placeholder: clear this dropdown
                if (!qtDropClear(base, panel, g_qtOpenActivity, g_qtOpenSlot, g_qtOpenMode)) {
                    postSpeech(axs(AXS_BLD_DIDNT_HAPPEN));
                    return true;
                }
                g_qtOpen = false;
                logLine("sanitarium: dropdown \"%s\" cleared (slot %d)",
                        qtDropWord(g_qtOpenMode), g_qtOpenSlot);
                if (!bldSpeakOptionRow(base, panel, nullptr)) postSpeech(axs(AXS_QT_SELECT));
                return true;
            }
            const QtCand* c = &cands[g_qtOpenRow - 1];
            char name[160];
            if (!csQuirkName(base, c->id, name, sizeof name) || !name[0]) {
                strncpy(name, c->id, sizeof name - 1);
                name[sizeof name - 1] = 0;
            }
            int want = c->chosen ? 0 : g_qtOpenMode;
            if (!qtSetChoice(base, panel, g_qtOpenActivity, g_qtOpenSlot, c->type, c->idx,
                             c->id, want)) {
                postSpeech(axs(AXS_BLD_DIDNT_HAPPEN));
                return true;
            }
            logLine("sanitarium: dropdown \"%s\" -> \"%s\" (list %d idx %d, slot %d) mode %d",
                    qtDropWord(g_qtOpenMode), c->id, c->type, c->idx, g_qtOpenSlot, want);
            if (!want) {                                   // un-chosen: stay in the list
                char utter[MAILBOX_SZ];
                _snprintf(utter, sizeof utter, axs(AXS_QT_NOT_CHOSEN_FMT), name);
                utter[sizeof utter - 1] = 0;
                postSpeech(utter);
                return true;
            }
            char otherAfter[MAILBOX_SZ];
            qtDropState(base, g_qtOpenActivity, g_qtOpenHero, g_qtOpenSlot, otherMode,
                        otherAfter, sizeof otherAfter, nullptr, nullptr);
            char displaced[MAILBOX_SZ] = {0};
            if (strcmp(otherBefore, otherAfter) != 0) {
                char s[MAILBOX_SZ];
                if (!otherAfter[0])
                    _snprintf(s, sizeof s, axs(AXS_QT_DISPLACED_CLEARED_FMT),
                              otherBefore, qtDropSpoken(otherMode));
                else
                    _snprintf(s, sizeof s, axs(AXS_QT_DISPLACED_NOW_FMT),
                              qtDropSpoken(otherMode), otherAfter);
                s[sizeof s - 1] = 0;
                _snprintf(displaced, sizeof displaced, " %s", s);
            }
            char costFrag[96] = {0};
            uint32_t guid = 0, cur = 0;
            int amount = 0;
            bool afford = true;
            safeReadU32(g_qtOpenHero + ACTOR_ID_OFF, &guid);
            if (bldSehActHeroCost(base, g_qtOpenActivity, (int)guid, (uint32_t)g_qtOpenSlot,
                                  &amount, &cur, &afford) && amount > 0) {
                char total[80], curName[48];
                bldCurrencyWord(base, cur, curName, sizeof curName);
                _snprintf(total, sizeof total, axs(AXS_QT_TOTAL_COST_FMT), amount, curName);
                total[sizeof total - 1] = 0;
                if (afford) _snprintf(costFrag, sizeof costFrag, " %s", total);
                else        _snprintf(costFrag, sizeof costFrag, " %s %s", total,
                                      axs(AXS_BLD_CANT_AFFORD_IT));
                int mine = 0;
                if (qtRowCost(base, g_qtOpenActivity, g_qtOpenHero, c->type, c->locked != 0,
                              &mine, nullptr) && mine != amount)
                    logLine("sanitarium: price check — this quirk reads %d, the game's slot total "
                            "is %d (other choices may be included)", mine, amount);
            }
            g_qtOpen = false;
            char utter[MAILBOX_SZ];
            _snprintf(utter, sizeof utter, axs(AXS_QT_CHOSE_FMT), qtDropSpoken(g_qtOpenMode), name,
                      costFrag, displaced);
            utter[sizeof utter - 1] = 0;
            postSpeech(utter);
            return true;
        }
        return true;                                      // every other key is blocked
    }

    if ((mod & (KMOD_LCTRL | KMOD_RCTRL)) && !(mod & (KMOD_LALT | KMOD_RALT)) &&
        (sym == SDLK_UP || sym == SDLK_DOWN || sym == SDLK_LEFT || sym == SDLK_RIGHT) &&
        g_bldMode == 0) {
        uintptr_t root = resTownRoot(base);
        uintptr_t panel = root ? bldOpenPanel(root) : 0;
        if (!panel || bldRowKindOf(base, panel, nullptr) != 6) {
            if (sym == SDLK_LEFT || sym == SDLK_RIGHT) return false;   // not ours here
        } else {
            if (repeat) return true;
            if (g_haCol == 0) {
                if (sym == SDLK_LEFT || sym == SDLK_RIGHT) {
                    postSpeech(axs(AXS_HA_MOVE_RIGHT_FIRST));
                    return true;
                }
                int nh = haHeroRows(base);
                if (nh <= 0) { postSpeech(axs(AXS_HA_NO_HERO)); return true; }
                if (g_haRow >= nh) g_haRow = nh - 1;
                if (g_haRow < 0)   g_haRow = 0;
                static const char* kHaSkinId[3] = { "blacksmith", "guild", "camping_trainer" };
                int skin = haSkinOf(base, panel);      // kind 6 => >= 0, but never assume
                uintptr_t cobj = abHeroClassOf(g_haHeroes[g_haRow].hero);
                char cid[64] = {0};
                if (skin >= 0 && cobj) safeReadCStr(cobj + HEROCLASS_ID_OFF, cid, sizeof cid);
                if (cid[0]) {
                    char key[128], raw[MAILBOX_SZ], desc[MAILBOX_SZ];
                    _snprintf(key, sizeof key, "action_verbose_body_%s_%s",
                              kHaSkinId[skin], cid);
                    key[sizeof key - 1] = 0;
                    if (resolveKey(base, key, raw, sizeof raw) && raw[0]) {
                        stripMarkup(raw, desc, sizeof desc);
                        if (desc[0]) { postSpeech(desc); return true; }
                    }
                    // A modded class may ship no blurb; fall through to "No more information."
                    logLine("ha: class blurb miss \"%s\"", key);
                }
                postSpeech(axs(AXS_NO_MORE_INFO));
                return true;
            }
            int nh = haHeroRows(base);
            if (nh <= 0) { postSpeech(axs(AXS_HA_NO_HERO)); return true; }
            if (g_haRow >= nh) g_haRow = nh - 1;
            if (g_haRow < 0)   g_haRow = 0;
            const HaHero* h = &g_haHeroes[g_haRow];
            int nt = (haSelectedGuid(base, panel) == h->guid)
                     ? haTrees(base, panel, h->hero, false) : 0;
            if (nt <= 0) { postSpeech(axs(AXS_HA_NOTHING_FOR_HERO)); return true; }
            if (g_haCol > nt) g_haCol = nt;
            const HaTree* t = &g_haTrees[g_haCol - 1];
            if (t->hash != g_haTipTree || g_haRow != g_haTipRow) {   // a new cell: rebuild, restart
                haBuildTip(base, h->hero, t);
                g_haTipCol = 0;
                g_haTipLine = -1;
            }
            char utter[MAILBOX_SZ];
            if (sym == SDLK_LEFT || sym == SDLK_RIGHT) {
                int want = g_haTipCol + (sym == SDLK_RIGHT ? 1 : -1);
                if (want < 0) want = 0;
                if (want >= HA_TIP_COLS) want = HA_TIP_COLS - 1;   // hard stops, like every buffer
                g_haTipCol = want;
                g_haTipLine = 0;
                const char* head = axs(
                    (g_haTipCol == 0)                                    ? AXS_HA_HEAD_NOW :
                    (t->kind == HA_WEAPON || t->kind == HA_ARMOUR)        ? AXS_HA_HEAD_NEXT_TIER :
                    (t->kind == HA_COMBAT && t->level >= 0)              ? AXS_HA_HEAD_NEXT_RANK
                                                                         : AXS_HA_HEAD_IF_BUY);
                _snprintf(utter, sizeof utter, "%s. %s", head, g_haTip[g_haTipCol][0]);
            } else {
                int n = g_haTipCount[g_haTipCol];
                if (n <= 0) { postSpeech(axs(AXS_NO_MORE_INFO)); return true; }
                if (g_haTipLine < 0) g_haTipLine = 0;              // the first press lands on line 1
                else {
                    g_haTipLine += (sym == SDLK_DOWN) ? 1 : -1;
                    if (g_haTipLine < 0)  g_haTipLine = n - 1;     // wraps, like the action bar's
                    if (g_haTipLine >= n) g_haTipLine = 0;
                }
                _snprintf(utter, sizeof utter, "%s", g_haTip[g_haTipCol][g_haTipLine]);
            }
            utter[sizeof utter - 1] = 0;
            postSpeech(utter);
            return true;
        }
    }

    if ((mod & (KMOD_LCTRL | KMOD_RCTRL)) && !(mod & (KMOD_LALT | KMOD_RALT)) &&
        (sym == SDLK_UP || sym == SDLK_DOWN) && g_bldMode == 0) {
        uintptr_t root = resTownRoot(base);
        uintptr_t panel = root ? bldOpenPanel(root) : 0;
        if (!panel || bldRowKindOf(base, panel, nullptr) != 3) return false;
        if (repeat) return true;
        BldActRow rows[48];
        int n = bldActRows(base, panel, rows, 48);
        if (n <= 0) return false;
        axStepCursor(&g_bldRow, n, 0);       // re-clamp against the live rows
        char key[64], desc[MAILBOX_SZ];
        _snprintf(key, sizeof key, "town_activity_description_%s", rows[g_bldRow].actId);
        key[sizeof key - 1] = 0;
        if (resolveKey(base, key, desc, sizeof desc) && desc[0]) postSpeech(desc);
        else postSpeech(axs(AXS_NO_MORE_INFO));
        return true;
    }

    if (g_bldMode == 0) {
        uintptr_t bdRoot = resTownRoot(base);
        uintptr_t bdPanel = bdRoot ? bldOpenPanel(bdRoot) : 0;
        if (bdPanel && bdIsPanel(base, bdPanel) && bdRouteKey(base, bdPanel, sym, mod, repeat)) return true;
    }

    if (mod & (KMOD_LALT | KMOD_RALT | KMOD_LCTRL | KMOD_RCTRL)) return false;
    uintptr_t root = resTownRoot(base);
    uintptr_t panel = root ? bldOpenPanel(root) : 0;
    if (!panel) return false;

    if (g_bldMode == 0 && (sym == SDLK_PAGEUP || sym == SDLK_PAGEDOWN)) {
        if (repeat) return true;
        return tjStripCycle(base, root, panel, sym == SDLK_PAGEDOWN);
    }

    if (g_bldMode == 1 && (sym == SDLK_UP || sym == SDLK_DOWN)) {
        if (axNavHoldRepeat(repeat)) return true;
        int n = bldCollectTracks(base, root, panel, false);
        if (n <= 0) { postSpeech(axs(AXS_BLD_NO_TRACKS)); return true; }
        axStepCursor(&g_bldUpRow, n, 0);                             // re-clamp: live tracks
        axStepCursor(&g_bldUpRow, n, sym == SDLK_DOWN ? 1 : -1);     // hard stop: re-read the end
        bldSpeakTrackRow(base, root, panel, nullptr);
        return true;
    }

    if (g_bldMode == 1) {
        int jump = 0;
        if (axDecodeJump(sym, mod, repeat, &jump)) {     // Home/End: first/last upgrade track
            if (!jump) return true;                      // held jump: one landing per press
            int n = bldCollectTracks(base, root, panel, false);
            if (n <= 0) { postSpeech(axs(AXS_BLD_NO_TRACKS)); return true; }
            axStepCursor(&g_bldUpRow, n, jump);          // the clamp lands it; ends re-read
            bldSpeakTrackRow(base, root, panel, nullptr);
            return true;
        }
    }

    if (g_bldMode == 1 && (sym == SDLK_RETURN || sym == SDLK_KP_ENTER)) {
        if (repeat) return true;
        if (g_bldBuyWatchUntil) { postSpeech(axs(AXS_BLD_STILL_BUYING)); return true; }
        int n = bldCollectTracks(base, root, panel, false);
        if (n <= 0) { postSpeech(axs(AXS_BLD_NO_TRACKS)); return true; }
        axStepCursor(&g_bldUpRow, n, 0);              // re-clamp before acting on the row
        const BldTrack* t = &g_bldTracks[g_bldUpRow];
        if (t->nextIdx < 0) {
            postSpeech(axs(AXS_BLD_TRACK_FULL));
            return true;
        }
        if (t->nextIdx >= BLD_TRACK_STEPS_MAX || t->state[t->nextIdx] == 0) {
            postSpeech(axs(AXS_BLD_TRACK_NOTHING_NOW));
            return true;
        }
        if (t->def && t->nextIdx < t->def->steps &&
            townEventFreeUpgrades(base, "building") <= 0) {
            const BldStepCost* c = &t->def->cost[t->nextIdx];
            int have1 = 0, have2 = 0;               // an absent wallet entry counts as zero
            if (c->cur1[0]) resWalletAmount(base, resHash(c->cur1), &have1);
            if (c->cur2[0]) resWalletAmount(base, resHash(c->cur2), &have2);
            bool short1 = c->cur1[0] && have1 < c->amt1;
            bool short2 = c->cur2[0] && have2 < c->amt2;
            if (short1 || short2) {
                char cost[96], utter[224];
                bldCostText(base, c, cost, sizeof cost);
                _snprintf(utter, sizeof utter, axs(AXS_BLD_CANT_AFFORD_COSTS_FMT), cost);
                utter[sizeof utter - 1] = 0;
                postSpeech(utter);
                return true;
            }
        }
        int64_t elem = (int64_t)(t->hash + BLD_STEP_ID_TAG + (uint32_t)('a' + t->nextIdx));
        if (!frontEndClickElementId(elem)) {
            logLine("building: buy click, step element 0x%llx not on screen (track \"%s\")",
                    (unsigned long long)elem, t->id);
            postSpeech(axs(AXS_BLD_CANT_BUY_NOW));
            return true;
        }
        g_bldBuyHash = t->hash;                       // the step the watch is waiting on
        g_bldBuyCode = (uint8_t)('a' + t->nextIdx);
        g_bldBuyBefore = -1;
        strncpy(g_bldBuyName, t->name, sizeof g_bldBuyName - 1);
        g_bldBuyName[sizeof g_bldBuyName - 1] = 0;
        g_bldBuyStep  = t->nextIdx + 1;
        g_bldBuyTotal = t->def ? t->def->steps : t->stepsSeen;
        g_bldBuyFx    = (t->fx && t->nextIdx < 6) ? t->fx->nxt[t->nextIdx] : -1;
        g_bldBuyWatchUntil = GetTickCount() + 1500;   // checkBuilding announces the outcome
        return true;
    }

    if (g_bldMode == 0 && (sym == SDLK_UP || sym == SDLK_DOWN ||
                           sym == SDLK_RETURN || sym == SDLK_KP_ENTER) &&
        bldRowKindOf(base, panel, nullptr) == 4) {
        if (sym == SDLK_UP || sym == SDLK_DOWN) {
            if (axNavHoldRepeat(repeat)) return true;    // throttled repeat: claimed, no step
        } else if (repeat) return true;
        int n = stBuildModel(base, panel);
        if (n <= 0) { postSpeech(axs(AXS_ST_NOTHING_YET)); return true; }
        axStepCursor(&g_stRow, n, 0);                       // re-clamp against the live model

        if (sym == SDLK_UP || sym == SDLK_DOWN) {
            axStepCursor(&g_stRow, n, sym == SDLK_DOWN ? 1 : -1);   // hard stop: re-read the end
            stSpeakFocused(base, panel, nullptr);
            return true;
        }
        const StRow* r = &g_stRows[g_stRow];
        int type = r->type;
        char text[224];
        strncpy(text, r->text, sizeof text - 1); text[sizeof text - 1] = 0;
        if (!stActivateRow(base, panel, g_stRow)) { postSpeech(axs(AXS_ST_CANT_PLAY)); return true; }
        if (type == 2) {
            g_stJrnWatchUntil = GetTickCount() + ST_JRN_WAIT_MS;
            logLine("statue: journal page requested, watching %lu ms for the popup", ST_JRN_WAIT_MS);
            return true;
        }
        char utter[MAILBOX_SZ];
        if (type == 0) _snprintf(utter, sizeof utter, axs(AXS_ST_PLAYING_FMT), text);
        else           _snprintf(utter, sizeof utter, axs(AXS_ST_PLAYING_NARRATION_FMT), text);
        utter[sizeof utter - 1] = 0;
        postSpeech(utter);
        return true;
    }

    if (g_bldMode == 0 && (sym == SDLK_UP || sym == SDLK_DOWN ||
                           sym == SDLK_RETURN || sym == SDLK_KP_ENTER) &&
        bldRowKindOf(base, panel, nullptr) == 5) {
        if (sym == SDLK_UP || sym == SDLK_DOWN) {
            if (axNavHoldRepeat(repeat)) return true;    // throttled repeat: claimed, no step
        } else if (repeat) return true;
        int n = gyBuildModel(base, panel);
        if (n <= 0) { postSpeech(axs(AXS_GY_NO_DEAD)); return true; }
        axStepCursor(&g_gyRow, n, 0);                       // re-clamp against the live model
        if (sym == SDLK_UP || sym == SDLK_DOWN)
            axStepCursor(&g_gyRow, n, sym == SDLK_DOWN ? 1 : -1);   // hard stop: re-read the end
        gySpeakFocused(base, panel, nullptr);               // Enter re-reads the focused memorial
        return true;
    }

    if (g_bldMode == 0 && (sym == SDLK_UP || sym == SDLK_DOWN || sym == SDLK_LEFT ||
                           sym == SDLK_RIGHT || sym == SDLK_RETURN || sym == SDLK_KP_ENTER) &&
        bldRowKindOf(base, panel, nullptr) == 6) {
        if (sym != SDLK_RETURN && sym != SDLK_KP_ENTER) {
            if (axNavHoldRepeat(repeat)) return true;    // throttled repeat: claimed, no step
        } else if (repeat) return true;
        int nh = haHeroRows(base);
        if (nh <= 0) { postSpeech(axs(AXS_HA_NO_HERO)); return true; }
        axStepCursor(&g_haRow, nh, 0);                   // re-clamp against the live hero rows

        if (sym == SDLK_UP || sym == SDLK_DOWN) {
            axStepCursor(&g_haRow, nh, sym == SDLK_DOWN ? 1 : -1);  // hard stop: re-read the end
            if (g_haCol > 0 && !haShowFocusedHero(base, panel, &g_haHeroes[g_haRow])) {
                g_haCol = 0;
                haSpeakCellPfx(base, panel, AXS_HA_CANT_WORK_HERO, true);
                return true;
            }
            haSpeakCell(base, panel, nullptr, true);
            return true;
        }

        if (sym == SDLK_LEFT || sym == SDLK_RIGHT) {
            if (sym == SDLK_LEFT) {
                if (g_haCol == 0) { haSpeakCell(base, panel, nullptr, false); return true; }
                g_haCol--;
                haSpeakCell(base, panel, nullptr, false);
                return true;
            }
            // Rightwards: the option columns need this hero on the screen first.
            if (!haShowFocusedHero(base, panel, &g_haHeroes[g_haRow])) {
                haSpeakCellPfx(base, panel, AXS_HA_CANT_WORK_HERO, true);
                return true;
            }
            int nt = haTrees(base, panel, g_haHeroes[g_haRow].hero, false);
            if (nt <= 0) { postSpeech(axs(AXS_HA_NOTHING_FOR_HERO)); return true; }
            if (g_haCol < nt) g_haCol++;                 // hard stop: re-read the last column
            haSpeakCell(base, panel, nullptr, false);
            return true;
        }

        const HaHero* h = &g_haHeroes[g_haRow];
        if (g_haCol == 0) {
            if (!haShowFocusedHero(base, panel, h)) {
                haSpeakCellPfx(base, panel, AXS_HA_CANT_WORK_HERO, true);
                return true;
            }
            haSpeakCellPfx(base, panel, AXS_HA_WORKING_ON, true);
            return true;
        }
        if (!haShowFocusedHero(base, panel, h)) {
            g_haCol = 0;
            haSpeakCellPfx(base, panel, AXS_HA_CANT_WORK_HERO, true);
            return true;
        }
        int nt = haTrees(base, panel, h->hero, false);
        if (nt <= 0) { g_haCol = 0; postSpeech(axs(AXS_HA_NOTHING_FOR_HERO)); return true; }
        if (g_haCol > nt) g_haCol = nt;
        const HaTree* t = &g_haTrees[g_haCol - 1];
        if (t->next < 0) { postSpeech(axs(AXS_HA_NOTHING_LEFT_HERE)); return true; }
        const HaStep* st = &t->step[t->next];
        char lock[192];
        if (haStepRefused(base, h->hero, t, st, lock, sizeof lock)) {
            char utter[MAILBOX_SZ];
            if (lock[0]) _snprintf(utter, sizeof utter, "%s %s", axs(AXS_HA_CANT_BUY_YET), lock);
            else         _snprintf(utter, sizeof utter, "%s", axs(AXS_HA_CANT_BUY_YET));
            utter[sizeof utter - 1] = 0;
            postSpeech(utter);
            return true;
        }
        const char* buyFreeTag = haFreeTagOf(t);
        bool buyFree = buyFreeTag && townEventFreeUpgrades(base, buyFreeTag) > 0;
        int gold = 0;
        if (st->amount > 0 && !buyFree) {
            int have1 = 0, have2 = 0;
            int want1 = haStepPrice(base, t->hash, st->amount);
            int want2 = haStepPrice(base, t->hash, st->amount2);
            resWalletAmount(base, resHash(st->cur), &have1);
            if (st->amount2 > 0 && st->cur2[0]) resWalletAmount(base, resHash(st->cur2), &have2);
            if (have1 < want1 || (st->amount2 > 0 && have2 < want2)) {
                char cost[128], utter[MAILBOX_SZ];
                haCostText(base, t->hash, st, cost, sizeof cost);
                _snprintf(utter, sizeof utter, axs(AXS_BLD_CANT_AFFORD_COSTS_FMT), cost);
                utter[sizeof utter - 1] = 0;
                postSpeech(utter);
                return true;
            }
        }
        resWalletAmount(base, resHash("gold"), &gold);
        int64_t elem = (int64_t)(t->hash + BLD_STEP_ID_TAG + (uint32_t)st->code);
        bool viaIcon = false;
        if (!feGetElementById(elem) && t->next == 0 && feGetElementById(t->iconElem)) {
            elem = t->iconElem;
            viaIcon = true;
        }
        if (!frontEndClickElementId(elem)) {
            logLine("heroaction: buy click, no element for tree \"%s\" code '%c' "
                    "(step elem 0x%llx, icon 0x%llx, iconBuys=%d)", t->id, st->code ? st->code : '?',
                    (unsigned long long)(t->hash + BLD_STEP_ID_TAG + (uint32_t)st->code),
                    (unsigned long long)t->iconElem, t->iconBuys ? 1 : 0);
            haDumpElements(base, panel, "buy failed");
            postSpeech(axs(AXS_BLD_CANT_BUY_NOW));
            return true;
        }
        if (viaIcon)
            logLine("heroaction: bought via the tree icon (no box for step '%c')",
                    st->code ? st->code : '?');
        g_haBuyHash = t->hash;
        g_haBuyCode = st->code;
        safeReadU32(h->hero + ACTOR_ID_OFF, &g_haBuyGuid);
        {
            uintptr_t rec = haTreeRecord(base, t->hash);
            uint8_t instanced = 0;
            if (rec) safeReadU8(rec + HA_REG_INSTANCED, &instanced);
            if (!instanced) g_haBuyGuid = 0;
        }
        strncpy(g_haBuyName, t->name, sizeof g_haBuyName - 1);
        g_haBuyName[sizeof g_haBuyName - 1] = 0;
        g_haBuyGoldBefore = gold;
        g_haBuyWatchUntil = GetTickCount() + 1500;   // checkBuilding announces the outcome
        logLine("heroaction: buy clicked tree \"%s\" code '%c' elem=0x%llx guid=%u gold=%d",
                t->id, st->code ? st->code : '?', (unsigned long long)elem, g_haBuyGuid, gold);
        return true;
    }

    if (g_bldMode == 0 && (sym == SDLK_UP || sym == SDLK_DOWN ||
                           sym == SDLK_LEFT || sym == SDLK_RIGHT) &&
        bldRowKindOf(base, panel, nullptr) == 3) {
        if (axNavHoldRepeat(repeat)) return true;
        bldActMove(base, panel,
                   (sym == SDLK_RIGHT) ? 1 : (sym == SDLK_LEFT ? -1 : 0),
                   (sym == SDLK_DOWN)  ? 1 : (sym == SDLK_UP   ? -1 : 0));
        return true;
    }

    if (g_bldMode == 0 && dgIsPanel(base, panel) && dgRouteKey(base, panel, sym, repeat)) return true;
    if (g_bldMode == 0 && pbIsPanel(base, panel) && pbRouteKey(base, panel, sym, repeat)) return true;
    if (g_bldMode == 0 && rbIsPanel(base, panel) && rbRouteKey(base, panel, sym, repeat)) return true;

    if (g_bldMode == 0) {
        int jump = 0;
        if (axDecodeJump(sym, mod, repeat, &jump)) {
            if (!jump) return true;                      // held jump: one landing per press
            int kind = bldRowKindOf(base, panel, nullptr);
            if (kind == 4) {                             // statue: the media list
                int n = stBuildModel(base, panel);
                if (n <= 0) { postSpeech(axs(AXS_ST_NOTHING_YET)); return true; }
                axStepCursor(&g_stRow, n, jump);
                stSpeakFocused(base, panel, nullptr);
                return true;
            }
            if (kind == 5) {                             // graveyard: the dead-hero list
                int n = gyBuildModel(base, panel);
                if (n <= 0) { postSpeech(axs(AXS_GY_NO_DEAD)); return true; }
                axStepCursor(&g_gyRow, n, jump);
                gySpeakFocused(base, panel, nullptr);
                return true;
            }
            if (kind == 6) {                             // hero-action table: the ROW axis,
                int nh = haHeroRows(base);               // the column is kept (user decision)
                if (nh <= 0) { postSpeech(axs(AXS_HA_NO_HERO)); return true; }
                axStepCursor(&g_haRow, nh, jump);
                if (g_haCol > 0 && !haShowFocusedHero(base, panel, &g_haHeroes[g_haRow])) {
                    g_haCol = 0;
                    haSpeakCellPfx(base, panel, AXS_HA_CANT_WORK_HERO, true);
                    return true;
                }
                haSpeakCell(base, panel, nullptr, true);
                return true;
            }
            if (kind == 3) {                             // activity wings: first/last slot row of
                BldActRow rows[48];                      // the CURRENT wing — the stitch is never
                int n = bldActRows(base, panel, rows, 48);   // crossed (the roster-toolbar rule)
                if (n <= 0) { postSpeech(axs(AXS_ACT_NONE_READ)); return true; }
                axStepCursor(&g_bldRow, n, 0);           // re-clamp against the live rows
                int wing = rows[g_bldRow].actOrd;
                if (jump > 0) { while (g_bldRow + 1 < n  && rows[g_bldRow + 1].actOrd == wing) g_bldRow++; }
                else          { while (g_bldRow - 1 >= 0 && rows[g_bldRow - 1].actOrd == wing) g_bldRow--; }
                char row[MAILBOX_SZ];
                if (!bldActFocusedRowText(base, panel, row, sizeof row, false))
                    _snprintf(row, sizeof row, "%s", axs(AXS_ACT_NONE_READ));
                row[sizeof row - 1] = 0;
                postSpeech(row);
                return true;
            }
            if (kind != 0) {                             // the row families (store, coach, ...):
                g_bldRow = (jump > 0) ? AX_JUMP : 0;     // the speaker re-clamps to the live count
                if (!bldSpeakOptionRow(base, panel, nullptr)) bldSpeakHere(base);
                return true;
            }
            return false;                                // no list here: the keys stay the game's
        }
    }

    if (g_bldMode == 0 && (sym == SDLK_LEFT || sym == SDLK_RIGHT)) {
        int rowKind = bldRowKindOf(base, panel, nullptr);
        if (rowKind != 1 && rowKind != 2) return false;      // not a pooled building: stay silent
        int showing = 0;
        int cols = bldColumnCount(panel, &showing);
        if (cols < 2) return false;                          // no second column YET: stay silent
        if (repeat) return true;
        if (g_bldSwapWatchUntil) { postSpeech(axs(AXS_STILL_SWITCHING)); return true; }
        int target = showing + (sym == SDLK_RIGHT ? 1 : -1);
        char who[128];
        if (target < 0 || target >= cols) {
            bldColumnName(base, panel, showing, who, sizeof who);
            char prefix[160];
            _snprintf(prefix, sizeof prefix, "%s. ", who);
            prefix[sizeof prefix - 1] = 0;
            if (!bldSpeakOptionRow(base, panel, prefix)) postSpeech(who);
            return true;
        }
        if (!bldColumnSwitchTo(base, panel, target)) {
            bldColumnName(base, panel, target, who, sizeof who);
            char utter[192];
            _snprintf(utter, sizeof utter, axs(AXS_COL_CANT_REACH_FMT), who);
            utter[sizeof utter - 1] = 0;
            postSpeech(utter);
        }
        return true;
    }

    if (g_bldMode == 0 && (sym == SDLK_UP || sym == SDLK_DOWN)) {
        if (bldRowKindOf(base, panel, nullptr) == 0) return false;
        if (axNavHoldRepeat(repeat)) return true;    // throttled repeat: claimed, no step
        int delta = (sym == SDLK_DOWN) ? 1 : -1;
        g_bldRow += delta;
        if (g_bldRow < 0) g_bldRow = 0;
        if (!bldSpeakOptionRow(base, panel, nullptr)) bldSpeakHere(base);
        return true;
    }

    if (g_bldMode == 0 && (sym == SDLK_RETURN || sym == SDLK_KP_ENTER)) {
        uintptr_t sys = 0;
        int storeKind = BLD_STORE_NONE;
        int kind = bldRowKindOf(base, panel, &sys, &storeKind);
        if (kind == 0) return false;
        if (repeat) return true;

        if (kind == 1) {                              // the store: Enter buys the focused trinket
            if (g_bldShopWatchUntil) { postSpeech(axs(AXS_BLD_STILL_BUYING)); return true; }
            int slots[64];
            int n = bldStoreRows(sys, slots, 64);
            if (n <= 0) { postSpeech(axs(AXS_SHOP_NOTHING)); return true; }
            axStepCursor(&g_bldRow, n, 0);   // re-clamp against the live rows
            char row[MAILBOX_SZ], name[128];
            int price = -1;
            name[0] = 0;
            bldStoreRowText(base, sys, storeKind, slots[g_bldRow], g_bldRow + 1, n,
                            row, sizeof row, name, sizeof name, &price);
            bool comet = (storeKind == BLD_STORE_COMET);
            char currency[96];
            bldStoreCurrencyName(base, storeKind, currency, sizeof currency);
            int purse = 0;
            resWalletAmount(base, resHash(comet ? "shard" : "gold"), &purse);
            if (price > 0 && purse < price) {
                char utter[224];
                _snprintf(utter, sizeof utter, axs(AXS_BLD_CANT_AFFORD_HAVE_FMT),
                          price, currency, purse);
                utter[sizeof utter - 1] = 0;
                postSpeech(utter);
                return true;
            }
            int64_t elem = (int64_t)(uint32_t)(INV_FOURCC_BASE + (uint32_t)g_bldRow);
            if (comet) bldCometScrollToRow(base, panel, g_bldRow);
            if (!frontEndClickElementId(elem)) {
                logLine("bldrows: buy click, element 0x%llx not on screen (row %d of %d)",
                        (unsigned long long)elem, g_bldRow + 1, n);
                postSpeech(axs(AXS_BLD_CANT_BUY_NOW));
                return true;
            }
            strncpy(g_bldShopName, name[0] ? name : axs(AXS_SHOP_THE_ITEM), sizeof g_bldShopName - 1);
            g_bldShopName[sizeof g_bldShopName - 1] = 0;
            g_bldShopGoldBefore  = purse;
            g_bldShopCountBefore = n;
            g_bldShopComet       = comet;                  // which wallet the watch must watch
            g_bldShopWatchUntil  = GetTickCount() + 1500;  // checkBuilding announces the outcome
            return true;
        }

        if (kind == 3) {                              // an activity slot: act by its live state
            if (g_bldActWatch) { postSpeech(axs(AXS_STILL_WORKING)); return true; }
            BldActRow rows[48];
            int n = bldActRows(base, panel, rows, 48);
            if (n <= 0) { postSpeech(axs(AXS_ACT_NONE_READ)); return true; }
            axStepCursor(&g_bldRow, n, 0);   // re-clamp against the live rows
            const BldActRow* r = &rows[g_bldRow];
            char actName[96];
            bldActName(base, r->actId, actName, sizeof actName);

            if (r->trtDrop >= 0) {
                QtCand cands[64];
                int cn = qtCandidates(base, r->activity, r->slotIdx, r->trtDrop, cands, 64);
                if (cn <= 0) { postSpeech(axs(AXS_QT_NOTHING_TO_CHOOSE)); return true; }
                g_qtOpen = true;
                g_qtOpenMode = r->trtDrop;
                g_qtOpenActivity = r->activity;
                g_qtOpenHero = r->pendingHero;
                g_qtOpenSlot = r->slotIdx;
                g_qtOpenRow = 0;
                for (int c = 0; c < cn; c++) if (cands[c].chosen) { g_qtOpenRow = c + 1; break; }
                char head[128], prefix[160];
                _snprintf(head, sizeof head, axs(AXS_QT_N_TO_CHOOSE_FMT),
                          qtDropSpoken(r->trtDrop), cn);
                head[sizeof head - 1] = 0;
                _snprintf(prefix, sizeof prefix, "%s ", head);
                prefix[sizeof prefix - 1] = 0;
                logLine("sanitarium: dropdown \"%s\" open, slot %d, %d candidates",
                        qtDropWord(r->trtDrop), r->slotIdx, cn);
                qtSpeakOpenRow(base, prefix);
                return true;
            }

            if (r->occupant >= 0) {                   // a town event parked the caretaker here
                postSpeech(axs(AXS_ACT_CARETAKER_HERE_SLOT));
                return true;
            }
            if (r->committedGuid) {                   // committed: the game's own cancel flow,
                uintptr_t hero = bldActHeroByGuid(base, r->committedGuid);
                char who[64] = {0};
                if (hero) safeReadCStr(hero + HERO_NAME_OFF, who, sizeof who);
                int64_t cancelElem = (int64_t)(uint32_t)
                    (r->slotElem - (uint32_t)r->slotIdx + BLD_ELEM_CNCL_TAG + (uint32_t)r->slotIdx);
                bool sent = frontEndClickElementId(cancelElem);
                if (!sent) {
                    logLine("bldact: cancel element 0x%llx not on screen, calling the body",
                            (unsigned long long)cancelElem);
                    sent = hero && bldSehActUncommit(base, r->disp, hero, r->slotw,
                                                     r->slotIdx, r->activity);
                }
                if (!sent) { postSpeech(axs(AXS_BLD_CANT_DO_NOW)); return true; }
                g_bldActWatch = 3;
                g_bldActWatchUntil = GetTickCount() + 2000;
                g_bldActWatchAct = r->activity;
                g_bldActWatchSlot = r->slotIdx;
                g_bldActWatchHero = hero;
                g_bldActSawDialog = false;
                resWalletAmount(base, resHash("gold"), &g_bldActGoldBefore);
                g_bldActLvlBefore = bldContagionLevel(base, panel);
                strncpy(g_bldActWatchWho, who[0] ? who : axs(AXS_THE_HERO), sizeof g_bldActWatchWho - 1);
                g_bldActWatchWho[sizeof g_bldActWatchWho - 1] = 0;
                strncpy(g_bldActWatchWhat, actName, sizeof g_bldActWatchWhat - 1);
                g_bldActWatchWhat[sizeof g_bldActWatchWhat - 1] = 0;
                return true;
            }
            if (r->pendingHero) {                     // pending: confirm = the payment, so the
                char who[64] = {0};                   // cost is prechecked OUT LOUD first (the
                safeReadCStr(r->pendingHero + HERO_NAME_OFF, who, sizeof who);
                uint32_t guid = 0;                    // game's own refusal is just a sound)
                safeReadU32(r->pendingHero + ACTOR_ID_OFF, &guid);
                if (r->isQuirkTreat && !qtSlotHasChoice(base, r->activity, r->slotIdx)) {
                    postSpeech(axs(AXS_QT_CHOOSE_TREAT_FIRST));
                    return true;
                }
                int amount = 0; uint32_t cur = 0; bool afford = true;
                bool costsMoney = bldSehActFlag(base, ACT_COSTSMONEY_RVA, r->activity, true);
                if (costsMoney &&
                    bldSehActHeroCost(base, r->activity, (int)guid, (uint32_t)r->slotIdx,
                                      &amount, &cur, &afford) && !afford) {
                    char utter[192], curName[48];
                    bldCurrencyWord(base, cur, curName, sizeof curName);
                    _snprintf(utter, sizeof utter, axs(AXS_BLD_CANT_AFFORD_AMOUNT_FMT),
                              amount, curName);
                    utter[sizeof utter - 1] = 0;
                    postSpeech(utter);
                    return true;
                }
                int64_t plain = (int64_t)BLD_ELEM_CNFM_ID;
                int64_t composed = (int64_t)(uint32_t)
                    (r->slotElem - (uint32_t)r->slotIdx + BLD_ELEM_CNFM_ID + (uint32_t)r->slotIdx);
                bool sent = false;
                if (feGetElementById(plain))         sent = frontEndClickElementId(plain);
                else if (feGetElementById(composed)) sent = frontEndClickElementId(composed);
                if (!sent) {
                    logLine("bldact: no confirm element (0x%llx / 0x%llx), calling the body",
                            (unsigned long long)plain, (unsigned long long)composed);
                    sent = bldSehActConfirm(base, r->disp, r->pendingHero, r->slotw,
                                            r->slotIdx, r->activity);
                }
                if (!sent) { postSpeech(axs(AXS_BLD_CANT_DO_NOW)); return true; }
                g_bldActWatch = 2;
                g_bldActWatchUntil = GetTickCount() + 2000;
                g_bldActWatchAct = r->activity;
                g_bldActWatchSlot = r->slotIdx;
                g_bldActWatchHero = r->pendingHero;
                g_bldActSawDialog = false;
                resWalletAmount(base, resHash("gold"), &g_bldActGoldBefore);
                g_bldActLvlBefore = bldContagionLevel(base, panel);
                strncpy(g_bldActWatchWho, who[0] ? who : axs(AXS_THE_HERO), sizeof g_bldActWatchWho - 1);
                g_bldActWatchWho[sizeof g_bldActWatchWho - 1] = 0;
                strncpy(g_bldActWatchWhat, actName, sizeof g_bldActWatchWhat - 1);
                g_bldActWatchWhat[sizeof g_bldActWatchWhat - 1] = 0;
                return true;
            }
            if (bldSehActSlotLocked(base, r->activity, (uint32_t)r->slotIdx)) {
                char key[64], why[MAILBOX_SZ];
                _snprintf(key, sizeof key, "str_hero_slot_locked_%s", r->actId);
                key[sizeof key - 1] = 0;
                if (resolveKey(base, key, why, sizeof why) && why[0]) postSpeech(why);
                else postSpeech(axs(AXS_ACT_SLOT_LOCKED));
                return true;
            }
            if (bldSehActFlag(base, ACT_EVTLOCKED_RVA, r->activity, false)) {
                postSpeech(axs(AXS_ACT_CLOSED_WEEK));
                return true;
            }
            g_bldActPickActivity = r->activity;
            g_bldActPickSlotW = r->slotw;
            g_bldActPickSlot = r->slotIdx;
            strncpy(g_bldActPickActId, r->actId, sizeof g_bldActPickActId - 1);
            g_bldActPickActId[sizeof g_bldActPickActId - 1] = 0;
            int idleN = bldActIdleCount(base);
            char head[144];
            _snprintf(head, sizeof head, axs(AXS_ACT_CHOOSE_HERO_FMT), actName, idleN);
            head[sizeof head - 1] = 0;
            if (!rosEnterForActivityPick(base, head)) {
                // The roster surface could not open (no rows readable) — it said so itself.
                bldActPickSessionDrop("roster would not open");
                return true;
            }
            logLine("bldact: roster pick open, \"%s\" slot %d, %d idle",
                    r->actId, r->slotIdx, idleN);
            return true;
        }

        BldRecruit rows[16];
        int rn = bldRecruitRows(base, panel, rows, 16, nullptr);
        if (rn <= 0) { postSpeech(axs(AXS_RCT_NONE)); return true; }
        if (g_bldRow >= rn) g_bldRow = rn - 1;
        if (g_bldRow < 0)  g_bldRow = 0;
        int have = 0, cap = 0;
        if (!bldSehRosterCounts(base, &have, &cap))
            logLine("bldrows: roster count/max getters faulted (log only)");
        g_bldRecHero = rows[g_bldRow].hero;
        g_bldRecSlot = rows[g_bldRow].slot;
        g_bldRecName[0] = 0;
        safeReadCStr(g_bldRecHero + HERO_NAME_OFF, g_bldRecName, sizeof g_bldRecName);
        g_bldRecDrag = 1;
        logLine("bldrows: recruit pending, slot %d hero=%p \"%s\" (roster %d of %d)",
                g_bldRecSlot, (void*)g_bldRecHero, g_bldRecName, have, cap);
        char utter[224];
        _snprintf(utter, sizeof utter, axs(AXS_RCT_ADD_FMT),
                  g_bldRecName[0] ? g_bldRecName : axs(AXS_RCT_THIS_HERO));
        utter[sizeof utter - 1] = 0;
        postSpeech(utter);
        return true;
    }

    if (g_bldMode == 0 && sym == SDLK_BACKSPACE) {
        if (bldRowKindOf(base, panel, nullptr) != 3) return false;
        if (repeat) return true;
        if (g_bldActWatch) { postSpeech(axs(AXS_STILL_WORKING)); return true; }
        BldActRow arows[48];
        int an = bldActRows(base, panel, arows, 48);
        if (an <= 0) return false;
        if (g_bldRow >= an) g_bldRow = an - 1;
        if (g_bldRow < 0)  g_bldRow = 0;
        const BldActRow* r = &arows[g_bldRow];
        if (r->trtDrop >= 0) {
            char names[MAILBOX_SZ];
            qtDropState(base, r->activity, r->pendingHero, r->slotIdx, r->trtDrop,
                        names, sizeof names, nullptr, nullptr);
            if (!names[0]) { postSpeech(axs(AXS_QT_NOTHING_CHOSEN_THERE)); return true; }
            if (!qtDropClear(base, panel, r->activity, r->slotIdx, r->trtDrop)) {
                postSpeech(axs(AXS_BLD_DIDNT_HAPPEN));
                return true;
            }
            char utter[MAILBOX_SZ];
            _snprintf(utter, sizeof utter, axs(AXS_QT_ROW_SELECT_FMT), qtDropSpoken(r->trtDrop));
            utter[sizeof utter - 1] = 0;
            postSpeech(utter);
            return true;
        }
        if (!r->pendingHero) {
            postSpeech(axs(r->committedGuid ? AXS_ACT_ALREADY_STAYING
                                            : AXS_ACT_NOTHING_TAKE_OUT));
            return true;
        }
        char who[64] = {0};
        safeReadCStr(r->pendingHero + HERO_NAME_OFF, who, sizeof who);
        if (!frontEndClickElementId((int64_t)r->slotElem)) {
            logLine("bldact: slot element 0x%x not on screen for the pending remove", r->slotElem);
            postSpeech(axs(AXS_BLD_CANT_DO_NOW));
            return true;
        }
        g_bldActWatch = 4;
        g_bldActWatchUntil = GetTickCount() + 1500;
        g_bldActWatchAct = r->activity;
        g_bldActWatchSlot = r->slotIdx;
        g_bldActWatchHero = r->pendingHero;
        g_bldActSawDialog = false;
        g_bldActLvlBefore = bldContagionLevel(base, panel);
        strncpy(g_bldActWatchWho, who[0] ? who : axs(AXS_THE_HERO), sizeof g_bldActWatchWho - 1);
        g_bldActWatchWho[sizeof g_bldActWatchWho - 1] = 0;
        bldActName(base, r->actId, g_bldActWatchWhat, sizeof g_bldActWatchWhat);
        return true;
    }

    if (g_bldMode == 0 && sym == SDLK_c) {
        uintptr_t disp = 0;
        BldRecruit rows[16];
        int n = bldRecruitRows(base, panel, rows, 16, &disp);
        if (!disp) return false;                      // not a recruit building: C stays the game's
        if (repeat) return true;
        if (csTownSheetPanel(base)) { postSpeech(axs(AXS_SHEET_ALREADY_OPEN)); return true; }
        if (g_bldSheetWatchUntil)   { postSpeech(axs(AXS_STILL_WORKING)); return true; }
        if (n <= 0) { postSpeech(axs(AXS_RCT_NONE)); return true; }
        axStepCursor(&g_bldRow, n, 0);       // re-clamp against the live rows
        if (!bldSehInspectRecruit(base, disp, rows[g_bldRow].hero, (uint32_t)rows[g_bldRow].slot)) {
            logLine("bldrows: recruit inspect body faulted (disp=%p slot=%d)",
                    (void*)disp, rows[g_bldRow].slot);
            postSpeech(axs(AXS_SHEET_DIDNT_OPEN));
            return true;
        }
        g_ptySheetHero = rows[g_bldRow].hero;         // the sheet reader's hero source
        g_bldSheetWatchUntil = GetTickCount() + 1500; // checkBuilding pays the observed flip
        return true;
    }

    if (sym == SDLK_u || (sym == SDLK_ESCAPE && g_bldMode == 1)) {
        if (repeat) return true;
        if (g_bldWatchUntil) { postSpeech(axs(AXS_STILL_WORKING)); return true; }
        if (!feGetElementById((int64_t)BLD_ELEM_UPGRADE)) {
            logLine("building: no upgrade-crest element on \"%s\" -> no upgrades", g_bldId);
            postSpeech(axs(AXS_BLD_NO_UPGRADES));
            return true;
        }
        if (frontEndClickElementId((int64_t)BLD_ELEM_UPGRADE)) {
            g_bldWatchWant  = g_bldMode == 1 ? 0 : 1;
            g_bldWatchUntil = GetTickCount() + 1500;  // checkBuilding announces the outcome
        } else {
            postSpeech(axs(AXS_BLD_UPGRADE_BTN_DIDNT));
        }
        return true;
    }

    return false;
}

void checkBuilding(uintptr_t base) {
    uintptr_t root = resTownRoot(base);
    uintptr_t panel = root ? bldOpenPanel(root) : 0;
    uint32_t  mode = 0;
    if (panel) safeReadU32(panel + BLD_MODE_OFF, &mode);
    bool sheetOpen = csTownSheetPanel(base) != 0;

    if (g_bldSwapWatchUntil) {
        uint32_t idx = 0;
        bool haveIdx = panel && safeReadU32(panel + BLD_ITEM_INDEX_OFF, &idx);
        if (!panel || !haveIdx) {
            g_bldSwapWatchUntil = 0;
            logLine("bldcols: switch watch ended, the building is gone");
        } else if (idx != g_bldSwapIndexBefore) {
            g_bldSwapWatchUntil = 0;
            char who[128];
            bldColumnName(base, panel, (int)idx, who, sizeof who);
            logLine("bldcols: switch observed, column %u -> %u (%s)",
                    g_bldSwapIndexBefore, idx, who);
            g_bldRow = 0;                       // a different column is a different list
            bldRowsProbe(base, panel);
            char purse[128];
            bldJewelerPurseLine(base, panel, (int)idx, purse, sizeof purse);
            char prefix[336];
            if (purse[0]) _snprintf(prefix, sizeof prefix, "%s. %s ", who, purse);
            else          _snprintf(prefix, sizeof prefix, "%s. ", who);
            prefix[sizeof prefix - 1] = 0;
            if (!bldSpeakOptionRow(base, panel, prefix)) postSpeech(prefix);
        } else if (GetTickCount() > g_bldSwapWatchUntil) {
            g_bldSwapWatchUntil = 0;
            logLine("bldcols: switch watch timed out (column still %u)", g_bldSwapIndexBefore);
            postSpeech(axs(AXS_COL_DIDNT_CHANGE));
        }
    }

    if (g_bldShopWatchUntil) {
        uintptr_t sys = panel ? bldStoreSystem(base, panel) : 0;
        if (!panel || !sys) {
            g_bldShopWatchUntil = 0;
            logLine("bldrows: buy watch ended, the store surface is gone");
        } else {
            int slots[64];
            int nowCount = bldStoreRows(sys, slots, 64);
            int purseNow = 0;
            resWalletAmount(base, resHash(g_bldShopComet ? "shard" : "gold"), &purseNow);
            if (nowCount < g_bldShopCountBefore || purseNow < g_bldShopGoldBefore) {
                g_bldShopWatchUntil = 0;
                int paid = g_bldShopGoldBefore - purseNow;
                char currency[96];
                bldStoreCurrencyName(base, g_bldShopComet ? BLD_STORE_COMET : BLD_STORE_GOLD,
                                     currency, sizeof currency);
                char utter[MAILBOX_SZ];
                if (paid > 0)
                    _snprintf(utter, sizeof utter, axs(AXS_SHOP_BOUGHT_FMT),
                              g_bldShopName, paid, currency, purseNow, currency, nowCount);
                else
                    _snprintf(utter, sizeof utter, axs(AXS_SHOP_BOUGHT_FREE_FMT),
                              g_bldShopName, nowCount);
                utter[sizeof utter - 1] = 0;
                postSpeech(utter);
                logLine("bldrows: purchase observed (count %d -> %d, %s %d -> %d)",
                        g_bldShopCountBefore, nowCount, g_bldShopComet ? "shard" : "gold",
                        g_bldShopGoldBefore, purseNow);
                if (g_bldRow >= nowCount) g_bldRow = nowCount > 0 ? nowCount - 1 : 0;
            } else if (confirmDialogOpen(base)) {
                g_bldShopWatchUntil = GetTickCount() + 1500;   // the game is asking; wait it out
            } else if (GetTickCount() > g_bldShopWatchUntil) {
                g_bldShopWatchUntil = 0;
                logLine("bldrows: buy watch timed out (count=%d gold=%d)",
                        g_bldShopCountBefore, g_bldShopGoldBefore);
                postSpeech(axs(AXS_BLD_PURCHASE_DIDNT));
            }
        }
    }

    if (g_bldActWatch) {
        if (!panel) {
            g_bldActWatch = 0;
            bldActPickSessionDrop("watch ended, the building is gone");
            if (rosActivityPickLive())               // the roster was mid-pick over it: stand it
                rosLeave(base, "building gone under the activity pick");   // down too; the town
            logLine("bldact: watch ended, the building is gone");          // map edge announces
        } else {
            uint32_t committed = 0; uintptr_t pending = 0;
            bool stateOk = bldActSlotState(base, panel, g_bldActWatchAct, g_bldActWatchSlot,
                                           &committed, &pending);
            bool dialog = confirmDialogOpen(base);
            if (dialog) g_bldActSawDialog = true;
            uint32_t wantGuid = 0;
            if (g_bldActWatchHero) safeReadU32(g_bldActWatchHero + ACTOR_ID_OFF, &wantGuid);
            char utter[MAILBOX_SZ];
            utter[0] = 0;
            bool done = false;
            if (g_bldActWatch == 1 && stateOk && pending && pending == g_bldActWatchHero) {
                int idx = bldActRowIndexOf(base, panel, g_bldActWatchAct, g_bldActWatchSlot);
                if (idx >= 0) g_bldRow = idx;
                bldActPickSessionDrop("pick landed");
                if (rosActivityPickLive()) rosLeave(base, "activity pick committed");
                _snprintf(utter, sizeof utter, axs(AXS_ACT_PLACED_FMT),
                          g_bldActWatchWho, g_bldActWatchWhat);
                done = true;
            } else if (g_bldActWatch == 2 && stateOk && committed && committed == wantGuid) {
                int goldNow = 0;
                resWalletAmount(base, resHash("gold"), &goldNow);
                int paid = g_bldActGoldBefore - goldNow;
                char goldName[64];
                resCurrencyTitleById(base, "gold", "bldact", goldName, sizeof goldName);
                if (paid > 0)
                    _snprintf(utter, sizeof utter, axs(AXS_ACT_COMMITTED_PAID_FMT),
                              g_bldActWatchWho, g_bldActWatchWhat, paid, goldName);
                else
                    _snprintf(utter, sizeof utter, axs(AXS_ACT_COMMITTED_FMT),
                              g_bldActWatchWho, g_bldActWatchWhat);
                done = true;
            } else if (g_bldActWatch == 3 && stateOk && !committed && !pending) {
                int goldNow = 0;
                resWalletAmount(base, resHash("gold"), &goldNow);
                int back = goldNow - g_bldActGoldBefore;
                char goldName[64];
                resCurrencyTitleById(base, "gold", "bldact", goldName, sizeof goldName);
                if (back > 0)
                    _snprintf(utter, sizeof utter, axs(AXS_ACT_TAKEN_OUT_REFUND_FMT),
                              g_bldActWatchWho, g_bldActWatchWhat, back, goldName);
                else
                    _snprintf(utter, sizeof utter, axs(AXS_ACT_TAKEN_OUT_OF_FMT),
                              g_bldActWatchWho, g_bldActWatchWhat);
                done = true;
            } else if (g_bldActWatch == 4 && stateOk && !pending) {
                _snprintf(utter, sizeof utter, axs(AXS_ACT_TAKEN_OUT_FMT), g_bldActWatchWho);
                done = true;
            }
            if (done) {
                int kindDone = g_bldActWatch;
                g_bldActWatch = 0;
                utter[sizeof utter - 1] = 0;
                logLine("bldact: watch %d paid (committed=%u pending=%p)",
                        kindDone, committed, (void*)pending);
                {
                    int lvlNow = bldContagionLevel(base, panel);
                    if (utter[0] && lvlNow >= 0 && g_bldActLvlBefore >= 0 &&
                        lvlNow != g_bldActLvlBefore) {
                        char word[96], cgline[144];
                        bldContagionWord(base, lvlNow, word, sizeof word);
                        if (word[0]) {
                            _snprintf(cgline, sizeof cgline,
                                      axs(lvlNow > g_bldActLvlBefore ? AXS_ACT_CONTAGION_UP_FMT
                                                                     : AXS_ACT_CONTAGION_DOWN_FMT),
                                      word);
                            cgline[sizeof cgline - 1] = 0;
                            size_t ul = strlen(utter);   // an AXS entry cannot LEAD with a space
                            _snprintf(utter + ul, sizeof utter - ul, " %s", cgline);
                            utter[sizeof utter - 1] = 0;
                            logLine("bldact: contagion %d -> %d rides the watch-%d line",
                                    g_bldActLvlBefore, lvlNow, kindDone);
                        }
                    }
                    g_bldActLvlBefore = -1;
                }
                if (utter[0]) {
                    if (kindDone == 2 && axReadTownBarks()) {
                        if (g_bldActDeferGuid) postSpeech(g_bldActDeferUtter); // never lose a parked one
                        strncpy(g_bldActDeferUtter, utter, sizeof g_bldActDeferUtter - 1);
                        g_bldActDeferUtter[sizeof g_bldActDeferUtter - 1] = 0;
                        g_bldActDeferGuid  = committed;
                        g_bldActDeferUntil = GetTickCount() + 3000; // one full balloon lifetime
                        logLine("bldact: committed announcement parked behind the bark (guid=%u)",
                                committed);
                    } else {
                        postSpeech(utter);
                    }
                }
            } else if (dialog) {
                g_bldActWatchUntil = GetTickCount() + 1500;    // the game is asking; wait it out
            } else if (GetTickCount() > g_bldActWatchUntil) {
                int kindTo = g_bldActWatch;
                g_bldActWatch = 0;
                logLine("bldact: watch %d timed out (sawDialog=%d committed=%u pending=%p)",
                        kindTo, (int)g_bldActSawDialog, committed, (void*)pending);
                if (kindTo == 1 && g_bldActSawDialog) {
                    char pfx[64];
                    _snprintf(pfx, sizeof pfx, "%s ", axs(AXS_ACT_NOT_PLACED));
                    pfx[sizeof pfx - 1] = 0;
                    if (rosActivityPickLive()) rosSpeakRowPrefixed(base, pfx);
                    else postSpeech(axs(AXS_ACT_NOT_PLACED));   // pick gone some other way:
                                                                // still never say nothing
                } else if (kindTo == 3 && g_bldActSawDialog) {
                } else {
                    postSpeech(axs(AXS_BLD_DIDNT_HAPPEN));
                }
            }
        }
    }

    if (g_bldActDeferGuid && GetTickCount() > g_bldActDeferUntil) {
        logLine("bldact: no bark collected the parked announcement; speaking it alone (guid=%u)",
                g_bldActDeferGuid);
        g_bldActDeferGuid = 0;
        postSpeech(g_bldActDeferUtter);
    }

    if (g_bldSheetWatchUntil) {
        if (sheetOpen) {
            g_bldSheetWatchUntil = 0;
            g_bldResume = true;
            g_bldKeepRow = true;
            logLine("building: recruit sheet open observed -> stood down, resume armed");
            if (g_bldProbes < 8) {                    // the recruit sheet's own elements are
                g_bldProbeDue = GetTickCount() + 400;
                strncpy(g_bldProbeWhy, "rsheet", sizeof g_bldProbeWhy - 1);
                g_bldProbeWhy[sizeof g_bldProbeWhy - 1] = 0;
            }
        } else if (!panel) {
            g_bldSheetWatchUntil = 0;
            g_ptySheetHero = 0;
            logLine("building: sheet watch ended by the building closing");
        } else if (GetTickCount() > g_bldSheetWatchUntil) {
            g_bldSheetWatchUntil = 0;
            g_ptySheetHero = 0;
            logLine("building: the recruit-sheet watch timed out");
            logInputSnapshot(base, "recruit-sheet-timeout");
            postSpeech(axs(AXS_SHEET_DIDNT_OPEN));
        }
    }

    if (g_bldRecWatchUntil) {
        int nowCount = bldRosterEntryCount(base);
        if (!panel) {
            g_bldRecWatchUntil = 0;
            g_bldRecDrag = 0;
            logLine("bldrows: recruit watch ended by the building closing");
        } else if (g_bldRecEntriesBefore >= 0 && nowCount > g_bldRecEntriesBefore) {
            g_bldRecWatchUntil = 0;
            g_bldRecDrag = 0;
            int have = 0, cap = 0;
            char head[192], prefix[224];
            if (bldSehRosterCounts(base, &have, &cap) && cap > 0)
                _snprintf(head, sizeof head, axs(AXS_RCT_JOINED_COUNT_FMT),
                          g_bldRecName[0] ? g_bldRecName : axs(AXS_THE_HERO), have, cap);
            else
                _snprintf(head, sizeof head, axs(AXS_RCT_JOINED_FMT),
                          g_bldRecName[0] ? g_bldRecName : axs(AXS_THE_HERO));
            head[sizeof head - 1] = 0;
            _snprintf(prefix, sizeof prefix, "%s ", head);
            prefix[sizeof prefix - 1] = 0;
            logLine("bldrows: recruit observed (entries %d -> %d)", g_bldRecEntriesBefore, nowCount);
            if (!bldSpeakOptionRow(base, panel, prefix)) postSpeech(prefix);
        } else if (confirmDialogOpen(base)) {
            g_bldRecWatchUntil = GetTickCount() + 1500;   // the game is asking; wait it out
        } else if (GetTickCount() > g_bldRecWatchUntil) {
            g_bldRecWatchUntil = 0;
            g_bldRecDrag = 0;
            logLine("bldrows: recruit watch timed out (entries before=%d now=%d)",
                    g_bldRecEntriesBefore, nowCount);
            logInputSnapshot(base, "recruit-watch-timeout");
            diagDumpFocusElements(base, "recruit-watch-timeout");
            int have = 0, cap = 0;
            if (bldSehRosterCounts(base, &have, &cap) && cap > 0 && have >= cap) {
                char why[192], utter[MAILBOX_SZ];
                _snprintf(why, sizeof why, axs(AXS_RCT_ROSTER_FULL_FMT), have, cap);
                why[sizeof why - 1] = 0;
                _snprintf(utter, sizeof utter, "%s %s", axs(AXS_RCT_DIDNT_HAPPEN), why);
                utter[sizeof utter - 1] = 0;
                logLine("bldrows: ... and the roster reads full (%d of %d)", have, cap);
                postSpeech(utter);
            } else {
                postSpeech(axs(AXS_RCT_DIDNT_HAPPEN));
            }
        }
    }

    if (!panel) {
        g_bldResume  = false;
        g_bldKeepRow = false;
        g_bldRecDrag = 0;                             // a held virtual button dies with its screen
        if (g_bldActPickActivity) {                   // ... and so does an activity-pick session
            bldActPickSessionDrop("no building panel");
            if (rosActivityPickLive())                // the roster was mid-pick: stand it down;
                rosLeave(base, "building gone under the activity pick");  // the town map edge
        }                                             // announces the landing
        g_qtOpen = false;                             // ... and an open treatment dropdown
    }

    if (g_bldBuyWatchUntil) {
        uint32_t keyGuid = 0;
        uintptr_t rec = haTreeRecord(base, g_bldBuyHash);
        uint8_t instanced = 0;
        if (rec) safeReadU8(rec + HA_REG_INSTANCED, &instanced);
        if (!panel) {
            g_bldBuyWatchUntil = 0;                   // the building closed under the watch
            logLine("building: buy watch ended by the building closing");
        } else if (g_bldBuyHash && !instanced &&
                   haPurchased(base, g_bldBuyHash, g_bldBuyCode, keyGuid)) {
            g_bldBuyWatchUntil = 0;
            char utter[MAILBOX_SZ];
            if (g_bldBuyFx >= 0)
                _snprintf(utter, sizeof utter, axs(AXS_BLD_BOUGHT_RANK_FX_FMT),
                          g_bldBuyName, g_bldBuyStep, g_bldBuyTotal, axs((AxStrId)g_bldBuyFx));
            else
                _snprintf(utter, sizeof utter, axs(AXS_BLD_BOUGHT_RANK_FMT),
                          g_bldBuyName, g_bldBuyStep, g_bldBuyTotal);
            utter[sizeof utter - 1] = 0;
            postSpeech(utter);
            logLine("building: purchase observed (tree 0x%x step '%c')",
                    g_bldBuyHash, g_bldBuyCode ? g_bldBuyCode : '?');
        } else if (confirmDialogOpen(base)) {
            g_bldBuyWatchUntil = GetTickCount() + 1500;   // the game is asking; wait it out
        } else if (GetTickCount() > g_bldBuyWatchUntil) {
            g_bldBuyWatchUntil = 0;
            logLine("building: buy watch timed out (tree 0x%x step '%c' instanced=%u)",
                    g_bldBuyHash, g_bldBuyCode ? g_bldBuyCode : '?', instanced);
            postSpeech(axs(AXS_BLD_NOT_BOUGHT_MAYBE_BROKE));
        }
    }

    if (g_haBuyWatchUntil) {
        if (haPurchased(base, g_haBuyHash, g_haBuyCode, g_haBuyGuid)) {
            g_haBuyWatchUntil = 0;
            int goldNow = 0;
            resWalletAmount(base, resHash("gold"), &goldNow);
            int paid = g_haBuyGoldBefore - goldNow;
            char utter[MAILBOX_SZ], head[320];
            head[0] = 0;
            if (panel && haSkinOf(base, panel) >= 0) {   // re-read the column's new state
                int nh = haHeroRows(base);
                if (nh > 0 && g_haRow >= 0 && g_haRow < nh) {
                    int nt = haTrees(base, panel, g_haHeroes[g_haRow].hero, false);
                    if (nt > 0 && g_haCol >= 1 && g_haCol <= nt)
                        haColumnHead(&g_haTrees[g_haCol - 1], head, sizeof head);
                }
            }
            g_haTipTree = 0;                             // the tooltip describes the old state
            char goldName[64];
            resCurrencyTitleById(base, "gold", "heroaction", goldName, sizeof goldName);
            if (paid > 0) _snprintf(utter, sizeof utter, axs(AXS_HA_BOUGHT_PAID_FMT),
                                    paid, goldName, head);
            else          _snprintf(utter, sizeof utter, axs(AXS_HA_BOUGHT_FMT), g_haBuyName, head);
            utter[sizeof utter - 1] = 0;
            postSpeech(utter);
            logLine("heroaction: purchase observed (tree 0x%x code '%c' guid=%u, gold %d -> %d)",
                    g_haBuyHash, g_haBuyCode ? g_haBuyCode : '?', g_haBuyGuid,
                    g_haBuyGoldBefore, goldNow);
        } else if (confirmDialogOpen(base)) {
            g_haBuyWatchUntil = GetTickCount() + 1500;   // the game is asking; wait it out
        } else if (GetTickCount() > g_haBuyWatchUntil) {
            g_haBuyWatchUntil = 0;
            logLine("heroaction: buy watch timed out (tree 0x%x code '%c' guid=%u)",
                    g_haBuyHash, g_haBuyCode ? g_haBuyCode : '?', g_haBuyGuid);
            postSpeech(axs(AXS_BLD_PURCHASE_DIDNT));
        }
    }

    dgService(base, panel);
    pbService(base, panel);
    rbService(base, panel);
    bdService(base, panel);

    if (g_bnSwitchUntil) {
        uint32_t nowHash = 0;
        if (panel) safeReadU32(panel + BLD_ID_HASH_OFF, &nowHash);
        if (panel && nowHash == g_bnSwitchHash) {
            g_bnSwitchUntil = 0;
        } else if (GetTickCount() > g_bnSwitchUntil) {
            g_bnSwitchUntil = 0;
            logLine("townjump: strip click on \"%s\" (0x%x) made no observed switch",
                    g_bnSwitchName, g_bnSwitchHash);
            postSpeech(axs(AXS_TJ_NO_SWITCH));
        }
    }

    // The click watches first, on the same frame the flip lands.
    if (g_bldWatchUntil) {
        if (!panel) {
            logLine("building: watch(want=%u) ended by the building closing", g_bldWatchWant);
            g_bldWatchUntil = 0;
        } else if (mode == g_bldWatchWant) {
            g_bldWatchUntil = 0;
        } else if (GetTickCount() > g_bldWatchUntil) {
            g_bldWatchUntil = 0;
            logLine("building: watch(want=%u) timed out at mode=%u", g_bldWatchWant, mode);
            postSpeech(axs(g_bldWatchWant == 1 ? AXS_BLD_UPVIEW_NO_OPEN
                                               : AXS_BLD_UPVIEW_NO_CLOSE));
        }
    }

    bool on = panel != 0 && !circusScreenUp(base) &&
              (g_bldActPickActivity != 0 ||
               (!g_resActive && !g_ptyActive && !g_rosActive && !sheetOpen));
    if (on && !g_bldActive) {
        g_bldActive = true;
        g_bldMode = mode;
        if (!safeReadCStr(panel + BLD_ID_STR_OFF, g_bldId, sizeof g_bldId) ||
            !tmIdLooksValid(g_bldId)) {
            logLine("building: no plausible id at +0xa4 (panel=%p)", (void*)panel);
            strncpy(g_bldId, "building", sizeof g_bldId - 1);
            g_bldId[sizeof g_bldId - 1] = 0;
        }
        char key[64];
        _snprintf(key, sizeof key, "town_name_%s", g_bldId);
        key[sizeof key - 1] = 0;
        if (!resolveKey(base, key, g_bldName, sizeof g_bldName)) {
            logLine("building: no localization for \"%s\" -> speaking the raw id", key);
            strncpy(g_bldName, g_bldId, sizeof g_bldName - 1);
            g_bldName[sizeof g_bldName - 1] = 0;
            for (char* p = g_bldName; *p; p++) if (*p == '_') *p = ' ';
        }
        logLine("building: active, \"%s\" mode=%u", g_bldId, mode);
        if (mode == 1) g_bldUpRow = 0;               // re-entered straight into the upgrade view
        if (!g_bldKeepRow) g_bldRow = 0;             // fresh visit lands on row 1; a sheet detour
        if (!g_bldKeepRow) g_stRow = 0;              // and the statue on its first entry
        if (!g_bldKeepRow) g_gyRow = 0;              // and the graveyard on its first entry
        if (!g_bldKeepRow) dgReset();                // and the Dueling Grounds' two cursors
        if (!g_bldKeepRow) pbReset();                // and the Prize Box's grid cursor
        if (!g_bldKeepRow) rbReset();                // and the Ranking Board's row cursor + watches
        if (!g_bldKeepRow) bdReset();                // and the Banner Designer's two panes + watches
        if (!g_bldKeepRow) {                         // the hero table lands on the hero the game
            g_haCol = 0;                             // is already showing, if it is showing one
            g_haRow = 0;
            uint32_t sel = haSelectedGuid(base, panel);
            if (sel) {
                int nh = haHeroRows(base);
                for (int i = 0; i < nh; i++)
                    if (g_haHeroes[i].guid == sel) { g_haRow = i; break; }
            }
        }
        g_bldKeepRow = false;
        g_bldResume = false;
        g_bldRecDrag = 0;                            // no virtual button survives an entry edge
        bldActPickSessionDrop("building entry edge"); // nor an activity-pick session (the `on`
                                                     // hold means one can only be here stale)
        g_qtOpen = false;                            // nor an open treatment dropdown
        if (mode == 0) bldRowsProbe(base, panel);    // one-shot row-resolution log per open
        bldSpeakHere(base);
        if (g_bldProbes < 8) {
            g_bldProbeDue = GetTickCount() + 400;
            strncpy(g_bldProbeWhy, "open", sizeof g_bldProbeWhy - 1);
            g_bldProbeWhy[sizeof g_bldProbeWhy - 1] = 0;
        }
    } else if (!on && g_bldActive) {
        g_bldActive = false;
        if (!g_bldResume) g_bldProbeDue = 0;          // the surface it described is gone
        if (g_bldRecDrag == 1) g_bldRecDrag = 0;      // a mouse-opened surface stole the keys; a
                                                      // flying drag (2) is left for its watch
        bldActPickSessionDrop("building stood down"); // a pick session dies with its surface
        g_bldSwapWatchUntil = 0;
        logLine("building: stood down");
    } else if (g_bldActive && panel) {
        char nowId[28] = {0};
        bool switched = safeReadCStr(panel + BLD_ID_STR_OFF, nowId, sizeof nowId) &&
                        tmIdLooksValid(nowId) && strcmp(nowId, g_bldId) != 0;
        if (switched) {
            logLine("building: direct switch \"%s\" -> \"%s\"", g_bldId, nowId);
            g_bldActive  = false;
            g_bldProbeDue = 0;
            if (g_bldRecDrag == 1) g_bldRecDrag = 0;
            bldActPickSessionDrop("direct building switch");
            g_qtOpen     = false;
            g_bldSwapWatchUntil = 0;                 // the old building's merchant watch
            g_bldWatchUntil     = 0;                 // ... and its upgrade-view watch
            g_bldResume  = false;
            g_bldKeepRow = false;
        } else if (mode != g_bldMode) {
            logLine("building: mode %u -> %u", g_bldMode, mode);
            g_bldMode = mode;
            if (mode == 1) {
                g_bldUpRow = 0;                           // a fresh view lands on the first track
                bldCollectTracks(base, root, panel, true); // the per-open row/step state log
            }
            bldSpeakHere(base);
            if (g_bldProbes < 8) {
                g_bldProbeDue = GetTickCount() + 400;
                strncpy(g_bldProbeWhy, mode == 1 ? "upgrades" : "back", sizeof g_bldProbeWhy - 1);
                g_bldProbeWhy[sizeof g_bldProbeWhy - 1] = 0;
            }
        }
    }

    if (g_bldProbeDue && GetTickCount() >= g_bldProbeDue) {
        g_bldProbeDue = 0;
        if (!axDebugLogEnabled()) {  }
        else if (panel && (g_bldActive || g_bldResume)) { g_bldProbes++; bldProbe(base, root, panel, g_bldProbeWhy); }
        else logLine("building: pending probe(%s) dropped, surface gone", g_bldProbeWhy);
    }
}
