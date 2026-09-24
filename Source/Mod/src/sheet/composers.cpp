// sheet/composers.cpp -- THE CHARACTER-SHEET COMPOSERS

#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cmath>
#include "internal.h"

// ---- CHARACTER SHEET (the C overlay) — static RE, session 38 ----

// ---- THE SHEET'S SECTIONS ----
static const uint32_t  CS_STAT_MASK       = 0x4e;    // the literal for showCombatVariant == 0
static const uint32_t  CS_STAT_BASE_MASK  = 0x4c;    // = 0xcd & CS_STAT_MASK
static const uintptr_t STAT_MASK_OFF      = 0x28;    // stat block+: uint32, the block's own mask

static const uintptr_t EQUIP_LEVEL_BYTE_OFF  = 0x110;   // byte; the game prints (byte - 0x2f), so an
                                                        // ascii digit '0'..'4' -> level 1..5
static const int       EQUIP_LEVEL_BIAS      = 0x2f;    // the game's own literal, not our arithmetic

static const uintptr_t CS_RESIST_STRIDE    = 0xb8;
static const uintptr_t CS_RESIST_MONSTER_OFF = 0xb0;

// ---- QUIRKS (character sheet) ----

static const uintptr_t QUIRK_IS_POSITIVE_OFF = 0x50;
static const uintptr_t QUIRK_IS_DISEASE_OFF  = 0x51;

static const uintptr_t QUIRK_BUFF_BEGIN_OFF = 0xa8;   // Quirk::Class+: -> first buff record
static const uintptr_t QUIRK_BUFF_END_OFF   = 0xb0;   // Quirk::Class+: -> one past the last
static const uintptr_t QUIRK_BUFF_STRIDE    = 0x1d0;  // = sizeof(Buff) (was 0x154)
static const int       QUIRK_BUFF_MAX       = 32;     // sanity cap on the per-quirk buff walk

static const uintptr_t QUIRK_TAG_BEGIN_OFF = 0x110;  // Quirk::Class+: -> first tag hash
static const uintptr_t QUIRK_TAG_END_OFF   = 0x118;  // Quirk::Class+: -> one past the last
static const uintptr_t QUIRK_TAG_STRIDE    = 4;      // one uint32 hash per tag
static const int       QUIRK_TAG_MAX       = 32;     // sanity cap on the per-quirk tag walk
static const char* const QUIRK_TAG_SINGLETON = "singleton";

static const uintptr_t QUIRK_ENTRY_REPLACES_OFF = 0x24;
static const uintptr_t QUIRK_ENTRY_REPLVIEW_OFF = 0x28;  // entry+: bool, the replaced mark has been seen
static const uintptr_t QUIRK_ENTRY_ISNEW_OFF    = 0x29;
static const uintptr_t QUIRK_ENTRY_LOCKED_OFF   = 0x2a;  // entry+: bool, locked (pos) / severe (neg)
static const int       QUIRK_REGISTRY_MAX       = 4096;  // sanity cap: 213 shipped + DLC + mod room

static const uintptr_t CS_QUIRK_DESC_EXTRA = 0;

static const int       CS_QUIRK_DESC_MODE = 0;

static const uintptr_t ACTOR_HEALTH_BLOCKS_OFF = 0x11cc;  // Hero+: uint, "health_damage_blocks" (was 0x114c;
static const uint32_t  ACTOR_HEALTH_BLOCKS_MAX = 99;

static const uintptr_t HEROCLASS_CANSELECT_OFF = 0x11e4;

// ---- CAMPING skills ----
static const uintptr_t CAMP_SKILL_ID_OFF   = 0x08;    // SkillClass+: inline C-string id ("encourage")
static const uintptr_t CAMP_SKILL_LEVEL_STRIDE = 0x78;
static const uintptr_t CAMP_SKILL_LEVEL_OFF     = 0x4c;  // always 0 in shipped data
static const uintptr_t CAMP_SKILL_COST_OFF      = 0x50;  // camp time, 1..5

// ---- CAMPING skill EFFECTS (the tooltip's third block, FUN_1407bb5d0) ----
static const uintptr_t CAMP_EFF_SELGROUP_OFF     = 0x40;  // uint: the game's std::map key, so also its ORDER
static const uintptr_t CAMP_EFF_REQ_BEG_OFF      = 0x48;  // vector of inline ids ("afflicted"), stride 0x44
static const uintptr_t CAMP_EFF_REQ_END_OFF      = 0x50;
static const uintptr_t CAMP_EFF_REQ_STRIDE       = 0x44;
static const uintptr_t CAMP_EFF_TYPE_OFF         = 0x60;  // inline id ("stress_heal_amount")
static const uintptr_t CAMP_EFF_TYPEENUM_OFF     = 0xa0;  // int: what the renderer actually branches on
static const uintptr_t CAMP_EFF_SUBTYPE_OFF      = 0xa4;  // inline id ("S", "campingACCBuff")
static const uintptr_t CAMP_EFF_BUFFKEY_OFF      = 0xe4;  // uint: the buff registry key (buff type only)
static const uintptr_t CAMP_EFF_AMOUNT_OFF       = 0xe8;
static const uintptr_t CAMP_EFF_CHANCE_OFF       = 0xf0;  // float, 0..1

static const uintptr_t CAMP_BUFFREG_ALT_OFF  = 0x10;      // added to P's VALUE when the flag is set
static const uintptr_t CAMP_BUFFREG_KEY_OFF   = 0x20;     // (was 0x1c)
static const uintptr_t CAMP_BUFFREG_VALUE_OFF = 0x28;     // (was 0x20)
static const uintptr_t CAMP_BUFF_REC_AMOUNT_OFF = 0x48;   // where the effect's amount is written in

// ---- The CHARACTER STATUS PANEL: an actor's ACTIVE buffs ----
static const uintptr_t ACTOR_BUFFS_BEGIN_OFF = 0x460;  // (was 0x408)
static const uintptr_t ACTOR_BUFFS_END_OFF   = 0x468;  // (was 0x410)
static const uintptr_t ACTOR_BUFF_ROUNDS_OFF = 0x54;   // int, rounds remaining (was 0x50)

static const uintptr_t BUFFREC_SOURCEDATA_OFF = 0x58;
                                                        //       buffs (runtime-proven); NOT a quirk id
static const uintptr_t BUFFREC_SRCGUID_OFF    = 0x13c;
static const uintptr_t BUFFREC_SRCCLASS_OFF   = 0x140;
static const uintptr_t BUFFREC_GUARD_GUID_OFF = 0x144;  // int   "guarded_guid" (guard relation partner)

static const char* const BSRC_QUIRK_NAME = "bsrc_quirk";

static const int       BUFF_SRCTYPE_STRIDE     = 0x4c;
static const uintptr_t BUFF_SRCTYPE_CAMP_OFF   = 0x45;       // byte: source survives to camp
static const int       BUFF_STATTYPE_STRIDE    = 0x58;
static const uintptr_t BUFF_STATTYPE_CAMP_OFF  = 5;          // byte: stat shows the camp phrasing
static const int       BUFF_DURTYPE_STRIDE     = 0x48;
static const int       BUFF_TYPE_TABLE_MAX     = 64;         // bound on any enum read from a record

static const uintptr_t BUFF_STATTYPE_HIGHERGOOD_OFF = 0;

static const int STAT_TYPE_TAG         = 0x01;   // marked
static const int STAT_TYPE_COMBAT_ADD  = 0x02;   // combat_stat_add — flat bonuses
static const int STAT_TYPE_COMBAT_MULT = 0x03;   // combat_stat_multiply — percent bonuses
static const int STAT_TYPE_RESISTANCE  = 0x0b;
static const int STAT_TYPE_POISON_DOT  = 0x04;   // blight
static const int STAT_TYPE_BLEED_DOT   = 0x05;
static const int STAT_TYPE_BURN_DOT    = 0x58;
static const int STAT_TYPE_RIPOSTE     = 0x1a;
static const int STAT_TYPE_GUARDED     = 0x1b;
static const int STAT_TYPE_STRESS_DOT  = 0x22;
static const int STAT_TYPE_SHUFFLE_DOT = 0x23;
static const int STAT_TYPE_HEAL_DOT    = 0x24;

static const uintptr_t ACTOR_STUN_OFF    = 0x1020;  // float, > 0 while stunned (was 0xfa4)
static const uintptr_t ACTOR_IMMOB_OFF   = 0x1024;  // byte                     (was 0xfa8)
static const uintptr_t ACTOR_DAZE_OFF    = 0x129c;  // float, rounds of daze remaining (was 0x121c)

static const uintptr_t TRAIT_REG_ALT_OFF  = 0x18;
static const int       TRAIT_REC_STRIDE   = 0x100;
static const uintptr_t TRAIT_REC_HASH_OFF = 0x40;
static const int       TRAIT_REG_MAX      = 64;     // sanity cap on the registry walk

static const uintptr_t HEROCLASS_MODEVEC_OFF     = 0xb38;   // ActorClass+: vector<ActorMode> begin
static const uintptr_t HEROCLASS_MODEVEC_END_OFF = 0xb40;   // ...end
static const int       MODE_REC_STRIDE           = 0x90;    // record: char id[0x20] @ +0, hash @ +0x20
static const int       MODE_TABLE_MAX            = 16;      // sanity cap; the shipped classes declare 2

static const uintptr_t QUIRK_HASH_OFF = 0x40;

// ---- CHARACTER SHEET: the SECTIONS ----

enum CsStatFmt {
    CS_FMT_CEIL,
    CS_FMT_ACCMOD,
    CS_FMT_SCALED,  // v * 100           -> "%.0f"    DEF
    CS_FMT_PCT1,    // v * 100           -> "%.1f%%"  CRIT
    CS_FMT_PCT0,    // roundf(v * 100)   -> "%.0f%%"  PROT
    CS_FMT_DMG,     // a low-high pair   -> "%d-%d"   DMG
    CS_FMT_RAW,     // v                 -> "%.0f"    SPD
};

struct CsStatRow {
    const char* key;
    uintptr_t   off;    // the stat block on the Hero
    CsStatFmt   fmt;
};

static const CsStatRow kCsStatRows[] = {
    { "str_ui_MAXHP",   CS_STAT_MAXHP_OFF, CS_FMT_CEIL   },
    { "str_ui_ATT_MOD", CS_STAT_ATT_OFF,   CS_FMT_ACCMOD },
    { "str_ui_DEF",     CS_STAT_DEF_OFF,   CS_FMT_SCALED },
    { "str_ui_CRIT",    CS_STAT_CRIT_OFF,  CS_FMT_PCT1   },
    { "str_ui_PROT",    CS_STAT_PROT_OFF,  CS_FMT_PCT0   },
    { "str_ui_DMG",     CS_STAT_DMGLO_OFF, CS_FMT_DMG    },
    { "str_ui_SPD",     CS_STAT_SPD_OFF,   CS_FMT_RAW    },
};
static const int kCsStatRowCount = (int)(sizeof kCsStatRows / sizeof kCsStatRows[0]);

bool csStatOwnMask(uintptr_t hero, uintptr_t off, float* out) {
    uint32_t mask = 0;
    if (!safeReadU32(hero + off + STAT_MASK_OFF, &mask)) return false;
    return abStatValue(hero + off, mask, true, out);
}

static bool csStatValueText(uintptr_t hero, const CsStatRow* row, char* out, int outsz) {
    float v = 0.0f;
    if (row->fmt == CS_FMT_DMG) {
        float lo = 0.0f, hi = 0.0f;
        if (!csStatOwnMask(hero, CS_STAT_DMGLO_OFF, &lo)) return false;
        if (!csStatOwnMask(hero, CS_STAT_DMGHI_OFF, &hi)) return false;
        _snprintf(out, outsz, "%d-%d", (int)ceilf(lo), (int)ceilf(hi));
        out[outsz - 1] = 0;
        return true;
    }
    bool clamp = (row->fmt != CS_FMT_ACCMOD);
    if (!abStatValue(hero + row->off, CS_STAT_MASK, clamp, &v)) return false;

    switch (row->fmt) {
        case CS_FMT_CEIL:   _snprintf(out, outsz, "%.0f",   ceilf(v));                     break;
        case CS_FMT_ACCMOD: _snprintf(out, outsz, "%+.0f",  v * CS_PERCENT_SCALE);         break;
        case CS_FMT_SCALED: _snprintf(out, outsz, "%.0f",   v * CS_PERCENT_SCALE);         break;
        case CS_FMT_PCT1:   _snprintf(out, outsz, "%.1f%%", v * CS_PERCENT_SCALE);         break;
        case CS_FMT_PCT0:   _snprintf(out, outsz, "%.0f%%", roundf(v * CS_PERCENT_SCALE)); break;
        default:            _snprintf(out, outsz, "%.0f",   v);                            break;
    }
    out[outsz - 1] = 0;
    return true;
}

void csTrimLabel(char* s) {
    int n = (int)strlen(s);
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == ':')) s[--n] = 0;
}

bool abTipNumFirst(uintptr_t base, const char* key, int a, int b, char* out, int outsz) {
    out[0] = 0;
    char fmt[256];
    if (!resolveKey(base, key, fmt, sizeof fmt) || !fmt[0]) return false;
    if (!abFormatMatches(fmt, "dd")) {
        logLine("numfirst: \"%s\" fmt mismatch \"%s\"", key, fmt);
        return false;
    }
    const char* pct = strchr(fmt, '%');
    if (!pct) return false;                     // abFormatMatches already proves there is one

    char label[96];
    int n = (int)(pct - fmt);
    if (n > (int)sizeof label - 1) n = (int)sizeof label - 1;
    memcpy(label, fmt, (size_t)n);
    label[n] = 0;
    abStripMarkup(label);
    csTrimLabel(label);

    char nums[128];
    _snprintf(nums, sizeof nums, pct, a, b);
    nums[sizeof nums - 1] = 0;
    abStripMarkup(nums);
    if (!nums[0]) return false;

    if (label[0]) _snprintf(out, outsz, "%s %s", nums, label);
    else          _snprintf(out, outsz, "%s", nums);
    out[outsz - 1] = 0;
    return out[0] != 0;
}

static const char* abFirstConversion(const char* fmt) {
    for (const char* p = fmt; *p; p++) {
        if (*p != '%') continue;
        if (p[1] == '%') { p++; continue; }
        return p;
    }
    return NULL;
}

bool abTipValueOnly(uintptr_t base, const char* key, int a, char* out, int outsz) {
    out[0] = 0;
    char fmt[256];
    if (!resolveKey(base, key, fmt, sizeof fmt) || !fmt[0]) return false;
    if (!abFormatMatches(fmt, "dd")) {
        logLine("valueonly: \"%s\" fmt mismatch \"%s\"", key, fmt);
        return false;
    }
    const char* pct = abFirstConversion(fmt);
    if (!pct) return false;                     // abFormatMatches already proves there is one

    char label[96];
    int n = (int)(pct - fmt);
    if (n > (int)sizeof label - 1) n = (int)sizeof label - 1;
    memcpy(label, fmt, (size_t)n);
    label[n] = 0;
    abStripMarkup(label);
    csTrimLabel(label);

    const char* endc = pct + 1;
    while (*endc && !strchr("diouxX", *endc)) endc++;
    if (!*endc) return false;
    size_t keep = (size_t)(endc - pct) + 1;
    char one[64];
    if (keep >= sizeof one) return false;       // an absurd width field: refuse, never guess
    memcpy(one, pct, keep);
    one[keep] = 0;

    char num[64];
    _snprintf(num, sizeof num, one, a);
    num[sizeof num - 1] = 0;
    abStripMarkup(num);
    if (!num[0]) return false;

    if (label[0]) _snprintf(out, outsz, "%s %s", num, label);
    else          _snprintf(out, outsz, "%s", num);
    out[outsz - 1] = 0;
    return out[0] != 0;
}

int csStatCount(uintptr_t base) { (void)base; return kCsStatRowCount; }

static bool csStatRowFor(uintptr_t base, uintptr_t hero, int i, char* out, int outsz) {
    if (i < 0 || i >= kCsStatRowCount) return false;
    if (!hero) return false;
    const CsStatRow* row = &kCsStatRows[i];

    char label[128];
    if (!resolveKey(base, row->key, label, sizeof label)) {
        logLine("charsheet stat: \"%s\" did not resolve", row->key);
        return false;
    }
    abStripMarkup(label);
    csTrimLabel(label);

    char val[64];
    if (!csStatValueText(hero, row, val, sizeof val)) {
        logLine("charsheet stat: \"%s\" value unreadable", row->key);
        _snprintf(out, outsz, "%s", label);
        out[outsz - 1] = 0;
        return true;
    }
    _snprintf(out, outsz, "%s: %s", label, val);
    out[outsz - 1] = 0;
    return true;
}

bool csStatRowText(uintptr_t base, int i, char* out, int outsz) {
    return csStatRowFor(base, abSelectedHero(base), i, out, outsz);
}

// ---- resistances ----

typedef float (*CsResistFn)(void* actor, int index);

bool csResistValue(uintptr_t base, uintptr_t hero, int i, float* out) {
    CsResistFn fn = reinterpret_cast<CsResistFn>(base + CS_RESIST_VALUE_RVA);
    __try { *out = fn(reinterpret_cast<void*>(hero), i); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    return true;
}

static bool csResistRawId(uintptr_t base, int i, char* out, int outsz) {
    uintptr_t rec = base + CS_RESIST_TABLE_RVA + (uintptr_t)i * CS_RESIST_STRIDE;
    if (!safeReadCStr(rec, out, outsz) || !out[0]) {
        logLine("charsheet resist: record %d has no name (table not filled yet?)", i);
        return false;
    }
    for (const char* p = out; *p; p++)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
              (*p >= '0' && *p <= '9') || *p == '_')) {
            logLine("charsheet resist: record %d name \"%s\" is not an identifier", i, out);
            return false;
        }
    return true;
}

// The resistance's localized name, resolved from the raw id.
static bool csResistName(uintptr_t base, int i, char* out, int outsz) {
    char raw[64];
    if (!csResistRawId(base, i, raw, sizeof raw)) return false;
    char key[96];
    _snprintf(key, sizeof key, "resistance_name_%s", raw);
    key[sizeof key - 1] = 0;
    if (!resolveKey(base, key, out, outsz)) {
        logLine("charsheet resist: \"%s\" did not resolve", key);
        return false;
    }
    abStripMarkup(out);
    return true;
}

int csResistCount(uintptr_t base) { (void)base; return CS_RESIST_COUNT; }

bool csResistVisibleFor(uintptr_t base, bool isMonster, int i) {
    if (!isMonster) return true;                       // heroes render all eight, ungated

    uintptr_t rec = base + CS_RESIST_TABLE_RVA + (uintptr_t)i * CS_RESIST_STRIDE;
    uint8_t show = 0;
    if (!safeReadU8(rec + CS_RESIST_MONSTER_OFF, &show)) {
        logLine("charsheet resist: monster flag unreadable for record %d, showing it", i);
        return true;
    }
    return show != 0;
}

bool csResistRowFor(uintptr_t base, uintptr_t hero, int i, char* out, int outsz) {
    if (i < 0 || i >= CS_RESIST_COUNT) return false;
    if (!hero) return false;

    char label[128];
    if (!csResistName(base, i, label, sizeof label)) return false;
    csTrimLabel(label);

    float v = 0.0f;
    if (!csResistValue(base, hero, i, &v)) {
        logLine("charsheet resist: value call faulted for index %d", i);
        _snprintf(out, outsz, "%s", label);
        out[outsz - 1] = 0;
        return true;
    }
    _snprintf(out, outsz, "%s: %.0f%%", label, v * CS_PERCENT_SCALE);
    out[outsz - 1] = 0;
    return true;
}

bool csResistRowText(uintptr_t base, int i, char* out, int outsz) {
    return csResistRowFor(base, abSelectedHero(base), i, out, outsz);
}

// ---- quirks ----

static int csQuirkCount(uintptr_t base) {
    uintptr_t hero = abSelectedHero(base);
    if (!hero) return 0;
    uintptr_t begin = 0, end = 0;
    if (!safeReadPtr(hero + HERO_QUIRK_BEGIN_OFF, &begin) ||
        !safeReadPtr(hero + HERO_QUIRK_END_OFF,   &end)) return 0;
    if (!begin || end < begin) return 0;

    uintptr_t span = end - begin;
    if (span % QUIRK_ENTRY_STRIDE != 0) {
        logLine("charsheet quirk: list span %llu is not a multiple of the 0x30 stride — refusing",
                (unsigned long long)span);
        return 0;
    }
    unsigned long long n = span / QUIRK_ENTRY_STRIDE;
    if (n > 64) {
        logLine("charsheet quirk: implausible count %llu — refusing", n);
        return 0;
    }
    return (int)n;
}

static uintptr_t csQuirkClassAt(uintptr_t base, int i) {
    if (i < 0 || i >= csQuirkCount(base)) return 0;
    uintptr_t hero = abSelectedHero(base);
    if (!hero) return 0;
    uintptr_t begin = 0, qc = 0;
    if (!safeReadPtr(hero + HERO_QUIRK_BEGIN_OFF, &begin) || !begin) return 0;
    if (!safeReadPtr(begin + (uintptr_t)i * QUIRK_ENTRY_STRIDE + QUIRK_ENTRY_CLASS_OFF, &qc)) return 0;
    return (qc > 0x10000) ? qc : 0;
}

static uintptr_t csQuirkEntryAt(uintptr_t base, int i) {
    if (i < 0 || i >= csQuirkCount(base)) return 0;
    uintptr_t hero = abSelectedHero(base);
    if (!hero) return 0;
    uintptr_t begin = 0;
    if (!safeReadPtr(hero + HERO_QUIRK_BEGIN_OFF, &begin) || !begin) return 0;
    return begin + (uintptr_t)i * QUIRK_ENTRY_STRIDE;
}

bool csQuirkId(uintptr_t quirk, char* out, int outsz) {
                                                          //  screen names a hero's new quirks)
    if (!safeReadCStr(quirk + QUIRK_ID_OFF, out, outsz) || !out[0]) return false;
    for (const char* p = out; *p; p++)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
              (*p >= '0' && *p <= '9') || *p == '_')) {
            logLine("charsheet quirk: id \"%s\" is not an identifier — refusing", out);
            return false;
        }
    return true;
}

// The quirk's localized display name.
bool csQuirkName(uintptr_t base, const char* id, char* out, int outsz) {
    char key[128];
    _snprintf(key, sizeof key, "str_quirk_name_%s", id);
    key[sizeof key - 1] = 0;
    if (!resolveKey(base, key, out, outsz)) {
        logLine("charsheet quirk: \"%s\" did not resolve", key);
        return false;
    }
    abStripMarkup(out);
    return true;
}

typedef char* (*CsQuirkDescFn)(char* out, void* quirk, uintptr_t extra, int mode);

bool csQuirkDescRaw(uintptr_t base, uintptr_t quirk, char* out, int outsz) {
    if (outsz < CS_QUIRK_DESC_BUF) return false;
    memset(out, 0, (size_t)outsz);
    CsQuirkDescFn fn = reinterpret_cast<CsQuirkDescFn>(base + CS_QUIRK_DESC_RVA);
    __try {
        fn(out, reinterpret_cast<void*>(quirk), CS_QUIRK_DESC_EXTRA, CS_QUIRK_DESC_MODE);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    out[outsz - 1] = 0;
    abStripMarkup(out);
    return true;
}

void csFlattenLines(char* s, int outsz) {
    char buf[MAILBOX_SZ];
    int o = 0;
    for (int i = 0; s[i] && o < (int)sizeof buf - 3; i++) {
        if (s[i] != '\n' && s[i] != '\r') { buf[o++] = s[i]; continue; }
        while (s[i + 1] == '\n' || s[i + 1] == '\r') i++;   // collapse a run of breaks
        if (!s[i + 1]) break;                                // a trailing break says nothing
        while (o > 0 && buf[o - 1] == ' ') o--;
        if (o > 0 && buf[o - 1] != '.' && buf[o - 1] != '!' && buf[o - 1] != '?') buf[o++] = '.';
        if (o > 0) buf[o++] = ' ';
    }
    while (o > 0 && buf[o - 1] == ' ') o--;
    buf[o] = 0;
    _snprintf(s, outsz, "%s", buf);
    s[outsz - 1] = 0;
}

static const char* csQuirkKind(uintptr_t quirk) {
    uint8_t positive = 0, disease = 0;
    if (!safeReadU8(quirk + QUIRK_IS_DISEASE_OFF,  &disease) ||
        !safeReadU8(quirk + QUIRK_IS_POSITIVE_OFF, &positive)) return nullptr;
    if (disease) return axs(AXS_CS_QUIRK_DISEASE);
    return positive ? axs(AXS_CS_QUIRK_POSITIVE) : axs(AXS_CS_QUIRK_NEGATIVE);
}

static bool csQuirkIsDisease(uintptr_t quirk) {
    uint8_t disease = 0;
    if (!safeReadU8(quirk + QUIRK_IS_DISEASE_OFF, &disease)) return false;
    return disease != 0;
}

static bool csQuirkHasTag(uintptr_t quirk, uint32_t want) {
    uintptr_t begin = 0, end = 0;
    if (!safeReadPtr(quirk + QUIRK_TAG_BEGIN_OFF, &begin) ||
        !safeReadPtr(quirk + QUIRK_TAG_END_OFF,   &end)) return false;
    if (!begin || end < begin) return false;    // no tags at all — 200 of the 213 shipped quirks

    uintptr_t span = end - begin;
    if (span % QUIRK_TAG_STRIDE != 0) {
        logLine("charsheet quirk: tag span %llu is not a multiple of the 4-byte stride — refusing",
                (unsigned long long)span);
        return false;
    }
    unsigned long long n = span / QUIRK_TAG_STRIDE;
    if (n > (unsigned long long)QUIRK_TAG_MAX) {   // the shipped maximum is 3
        logLine("charsheet quirk: implausible tag count %llu — refusing", n);
        return false;
    }

    for (unsigned long long i = 0; i < n; i++) {
        uint32_t tag = 0;
        if (!safeReadU32(begin + (uintptr_t)i * QUIRK_TAG_STRIDE, &tag)) return false;
        if (tag == want) return true;
    }
    return false;
}
static bool csQuirkIsSingleton(uintptr_t quirk) {
    return csQuirkHasTag(quirk, resHash(QUIRK_TAG_SINGLETON));
}

bool csHeroIsCursed(uintptr_t hero) {
    if (!hero) return false;
    uintptr_t qb = 0, qe = 0;
    if (!safeReadPtr(hero + HERO_QUIRK_BEGIN_OFF, &qb) ||
        !safeReadPtr(hero + HERO_QUIRK_END_OFF,   &qe)) return false;
    if (qb <= 0x10000 || qe < qb) return false;                 // no quirks at all
    uintptr_t span = qe - qb;
    if (span % QUIRK_ENTRY_STRIDE != 0 || span / QUIRK_ENTRY_STRIDE > 64) {
        logLine("cursed: hero %p quirk span %llu is not a sane entry list — refusing",
                (void*)hero, (unsigned long long)span);
        return false;
    }
    uint32_t want = resHash("vampire");
    for (uintptr_t e = qb; e < qe; e += QUIRK_ENTRY_STRIDE) {
        uintptr_t qc = 0;
        if (!safeReadPtr(e + QUIRK_ENTRY_CLASS_OFF, &qc) || qc <= 0x10000) continue;
        if (csQuirkHasTag(qc, want)) return true;
    }
    return false;
}

static const uintptr_t HERO_DD_QUESTS_OFF = 0x13dc;

bool csHeroIsNeverAgain(uintptr_t hero) {
    uint32_t n = 0;
    return hero && safeReadU32(hero + HERO_DD_QUESTS_OFF, &n) && n > 0;
}

void csNeverAgainWord(uintptr_t base, char* out, int outsz) {
    if (resolveKey(base, "menu_options_element_never_again", out, outsz) && out[0]) {
        abStripMarkup(out);
        return;
    }
    logLine("never again: \"menu_options_element_never_again\" did not resolve");
    _snprintf(out, outsz, "%s", axs(AXS_PTY_NEVER_AGAIN_FALLBACK));
    out[outsz - 1] = 0;
}

static const uintptr_t QUIRK_CLASS_ROSTER_LIMIT_OFF = 0x1b0;

uintptr_t csHeroRosterLimitQuirk(uintptr_t hero) {
    if (!hero) return 0;
    uintptr_t qb = 0, qe = 0;
    if (!safeReadPtr(hero + HERO_QUIRK_BEGIN_OFF, &qb) ||
        !safeReadPtr(hero + HERO_QUIRK_END_OFF,   &qe)) return 0;
    if (qb <= 0x10000 || qe < qb) return 0;
    uintptr_t span = qe - qb;
    if (span % QUIRK_ENTRY_STRIDE != 0 || span / QUIRK_ENTRY_STRIDE > 64) {
        logLine("roster limit: hero %p quirk span %llu is not a sane entry list — refusing",
                (void*)hero, (unsigned long long)span);
        return 0;
    }
    for (uintptr_t e = qb; e < qe; e += QUIRK_ENTRY_STRIDE) {
        uintptr_t qc = 0;
        uint32_t lim = 0;
        if (!safeReadPtr(e + QUIRK_ENTRY_CLASS_OFF, &qc) || qc <= 0x10000) continue;
        if (safeReadU32(qc + QUIRK_CLASS_ROSTER_LIMIT_OFF, &lim) && lim != 0) return qc;
    }
    return 0;
}

static bool csQuirkIdByHash(uintptr_t base, uint32_t hash, char* out, int outsz) {
    if (!hash) return false;
    uintptr_t reg = base + QUIRK_REGISTRY_RVA;
    uintptr_t begin = 0, end = 0;
    if (!safeReadPtr(reg, &begin) || !safeReadPtr(reg + 8, &end)) return false;
    if (begin <= 0x10000 || end < begin) return false;
    uintptr_t span = end - begin;
    if (span % QUIRK_REGISTRY_STRIDE != 0) {
        logLine("charsheet quirk: registry span %llu not a multiple of 0x1f8 — refusing",
                (unsigned long long)span);
        return false;
    }
    uintptr_t n = span / QUIRK_REGISTRY_STRIDE;
    if (n > (uintptr_t)QUIRK_REGISTRY_MAX) {
        logLine("charsheet quirk: implausible registry count %llu — refusing",
                (unsigned long long)n);
        return false;
    }
    uintptr_t hit = 0;
    for (uintptr_t i = 0; i < n; i++) {
        uintptr_t rec = begin + i * QUIRK_REGISTRY_STRIDE;
        uint32_t h = 0;
        if (!safeReadU32(rec + QUIRK_HASH_OFF, &h)) return false;
        if (h == hash) hit = rec;                    // last match, like the game's cmove
    }
    if (!hit) return false;
    return csQuirkId(hit, out, outsz);               // same identifier gate as every other id read
}

static bool csQuirkReplacedClause(uintptr_t base, uint32_t hash, char* out, int outsz) {
    char id[128], name[192], fmt[256];
    if (!csQuirkIdByHash(base, hash, id, sizeof id)) return false;
    if (!csQuirkName(base, id, name, sizeof name)) return false;
    csTrimLabel(name);
    if (!resolveKey(base, "str_quirk_replaces", fmt, sizeof fmt) || !fmt[0]) {
        logLine("charsheet quirk: \"str_quirk_replaces\" did not resolve");
        return false;
    }
    abStripMarkup(fmt);
    const char* pct = strstr(fmt, "%s");
    if (pct)
        _snprintf(out, outsz, "%.*s%s%s", (int)(pct - fmt), fmt, name, pct + 2);
    else
        _snprintf(out, outsz, "%s %s", fmt, name);   // a %s-less translation still names both halves
    out[outsz - 1] = 0;
    return out[0] != 0;
}

static int csQuirkFilteredIndex(uintptr_t base, int i, bool wantDisease) {
    if (i < 0) return -1;
    int n = csQuirkCount(base);
    int seen = 0;
    for (int k = 0; k < n; k++) {
        uintptr_t q = csQuirkClassAt(base, k);
        if (!q) continue;
        if (csQuirkIsDisease(q) != wantDisease) continue;
        if (seen == i) return k;
        seen++;
    }
    return -1;
}

static int csQuirkFilteredCount(uintptr_t base, bool wantDisease) {
    int n = csQuirkCount(base);
    int c = 0;
    for (int k = 0; k < n; k++) {
        uintptr_t q = csQuirkClassAt(base, k);
        if (q && csQuirkIsDisease(q) == wantDisease) c++;
    }
    return c;
}

int csQuirkSectionCount(uintptr_t base)   { return csQuirkFilteredCount(base, false); }   // (de-static'd
int csDiseaseSectionCount(uintptr_t base) { return csQuirkFilteredCount(base, true);  }

static bool csNeedsStop(const char* s) {
    size_t l = strlen(s);
    if (!l) return false;
    char c = s[l - 1];
    return c != '.' && c != '!' && c != '?' && c != ':';
}

static bool csQuirkRowCompose(uintptr_t base, uintptr_t quirk, uintptr_t entry, bool withKind,
                              bool* replSpoken, bool* newSpoken, char* out, int outsz) {
    if (replSpoken) *replSpoken = false;
    if (newSpoken)  *newSpoken  = false;
    if (!quirk) return false;

    char id[128];
    if (!csQuirkId(quirk, id, sizeof id)) return false;

    char name[192];
    if (!csQuirkName(base, id, name, sizeof name)) return false;  // the id is a token, not a label
    csTrimLabel(name);

    const char* kind = withKind ? csQuirkKind(quirk) : nullptr;
    if (withKind && !kind) logLine("charsheet quirk: \"%s\" polarity flags unreadable", id);

    // ---- the entry-level facts: NEW, REPLACED, LOCKED ----
    char pre[448] = { 0 };
    const char* lockWord = nullptr;
    if (entry) {
        uint8_t isnew = 0, viewed = 0, locked = 0;
        uint32_t repl = 0;
        safeReadU8(entry + QUIRK_ENTRY_ISNEW_OFF,    &isnew);
        safeReadU8(entry + QUIRK_ENTRY_REPLVIEW_OFF, &viewed);
        safeReadU8(entry + QUIRK_ENTRY_LOCKED_OFF,   &locked);
        safeReadU32(entry + QUIRK_ENTRY_REPLACES_OFF, &repl);

        char newbuf[128] = { 0 };
        if (isnew) {
            if (resolveKey(base, "str_quirk_new", newbuf, sizeof newbuf)) {
                abStripMarkup(newbuf);
                if (newSpoken && newbuf[0]) *newSpoken = true;
            } else {
                logLine("charsheet quirk: \"str_quirk_new\" did not resolve");
            }
        }
        char replbuf[288] = { 0 };
        if (repl && !viewed) {
            if (csQuirkReplacedClause(base, repl, replbuf, sizeof replbuf)) {
                if (replSpoken) *replSpoken = true;
            } else {
                // a hash the registry no longer holds (e.g. a removed mod quirk in an old save)
                logLine("charsheet quirk: \"%s\" replaces hash 0x%08x has no name — clause omitted",
                        id, repl);
            }
        }
        _snprintf(pre, sizeof pre, "%s%s%s%s",
                  newbuf, newbuf[0] ? (csNeedsStop(newbuf) ? ". " : " ") : "",
                  replbuf, replbuf[0] ? (csNeedsStop(replbuf) ? ". " : " ") : "");
        pre[sizeof pre - 1] = 0;

        if (locked) {
            uint8_t positive = 0;
            if (safeReadU8(quirk + QUIRK_IS_POSITIVE_OFF, &positive))
                lockWord = positive ? axs(AXS_CS_QUIRK_LOCKED) : axs(AXS_CS_QUIRK_SERIOUS);
            else
                logLine("charsheet quirk: \"%s\" locked but polarity unreadable — word omitted", id);
        }
    }

    char kindbuf[96] = { 0 };
    const char* kindOut = nullptr;
    if (kind && lockWord) { _snprintf(kindbuf, sizeof kindbuf, "%s, %s", kind, lockWord);
                            kindbuf[sizeof kindbuf - 1] = 0; kindOut = kindbuf; }
    else if (kind)          kindOut = kind;
    else if (lockWord)      kindOut = lockWord;

    char uniq[64] = {0};
    if (csQuirkIsSingleton(quirk))
        _snprintf(uniq, sizeof uniq, "%s%s.", kindOut ? ". " : " ", axs(AXS_CS_QUIRK_UNIQUE));

    char desc[CS_QUIRK_DESC_BUF + 8];
    bool gotDesc = csQuirkDescRaw(base, quirk, desc, sizeof desc);
    if (gotDesc) {
        abStripMarkup(desc);
        csFlattenLines(desc, sizeof desc);
    } else {
        logLine("charsheet quirk: \"%s\" description call faulted", id);
    }

    if (!gotDesc || !desc[0])
        logLine("charsheet quirk: \"%s\" produced no description text", id);

    bool haveDesc = gotDesc && desc[0];
    _snprintf(out, outsz, "%s%s.%s%s%s%s%s", pre, name,
              kindOut ? " " : "", kindOut ? kindOut : "", uniq,
              haveDesc ? (uniq[0] ? " " : ". ") : "", haveDesc ? desc : "");
    out[outsz - 1] = 0;
    return true;
}

bool csQuirkRowFrom(uintptr_t base, uintptr_t quirk, bool withKind, char* out, int outsz) {
    return csQuirkRowCompose(base, quirk, 0, withKind, nullptr, nullptr, out, outsz);
}

static bool csQuirkRowAt(uintptr_t base, int i, bool wantDisease, bool consume,
                         char* out, int outsz) {
    int k = csQuirkFilteredIndex(base, i, wantDisease);
    if (k < 0) return false;
    uintptr_t quirk = csQuirkClassAt(base, k);
    uintptr_t entry = csQuirkEntryAt(base, k);
    bool replSpoken = false, newSpoken = false;
    if (!csQuirkRowCompose(base, quirk, entry, !wantDisease, &replSpoken, &newSpoken, out, outsz))
        return false;
    if (consume && replSpoken && entry) {
        if (safeWriteU8(entry + QUIRK_ENTRY_REPLVIEW_OFF, 1))
            logLine("charsheet quirk: row %d replaced clause spoken — entry marked viewed", i);
        else
            logLine("charsheet quirk: row %d viewed write FAILED — clause will repeat", i);
    }
    if (consume && newSpoken && entry) {
        if (safeWriteU8(entry + QUIRK_ENTRY_ISNEW_OFF, 0))
            logLine("charsheet quirk: row %d new-quirk prefix spoken — glow cleared", i);
        else
            logLine("charsheet quirk: row %d is_new write FAILED — prefix will repeat", i);
    }
    return true;
}

bool csQuirkRowText(uintptr_t base, int i, char* out, int outsz) {
    return csQuirkRowAt(base, i, false, true, out, outsz);
}
bool csDiseaseRowText(uintptr_t base, int i, char* out, int outsz) {
    return csQuirkRowAt(base, i, true, true, out, outsz);
}

// ------------------------------------------------------------
// ------------------------------------------------------------

// lambda_1: `uint(HeroClass const*)` = (end - begin) / 4.
int csCampSkillCount(uintptr_t base) {
    uintptr_t cobj = abHeroClass(base);
    if (!cobj) return 0;
    uintptr_t beg = 0, end = 0;
    if (!safeReadPtr(cobj + HEROCLASS_CAMPVEC_OFF, &beg) ||
        !safeReadPtr(cobj + HEROCLASS_CAMPVEC_END_OFF, &end)) return 0;
    if (beg <= 0x10000 || end < beg) return 0;
    uintptr_t n = (end - beg) / 4;
    if (n > 32) return 0;
    return (int)n;
}

// The roster entry itself is the skill's id HASH, not a pointer and not an index.
static bool csCampSkillHash(uintptr_t base, int i, uint32_t* out) {
    uintptr_t cobj = abHeroClass(base);
    if (!cobj || i < 0 || i >= csCampSkillCount(base)) return false;
    uintptr_t beg = 0;
    if (!safeReadPtr(cobj + HEROCLASS_CAMPVEC_OFF, &beg) || beg <= 0x10000) return false;
    return safeReadU32(beg + (uintptr_t)i * 4, out);
}

static int csCampSkillLearnedRaw(uintptr_t base, int i) {
    uintptr_t hero = abSelectedHero(base);
    if (!hero || i < 0) return -1;
    uintptr_t beg = 0, end = 0;
    if (!safeReadPtr(hero + ACTOR_CAMP_KNOWN_OFF, &beg) ||
        !safeReadPtr(hero + ACTOR_CAMP_KNOWN_END_OFF, &end)) return -1;
    if (beg <= 0x10000 || end < beg) return -1;
    if ((uintptr_t)i >= (end - beg) / 4) return -1;
    int32_t v = -1;
    if (!safeReadU32(beg + (uintptr_t)i * 4, (uint32_t*)&v)) return -1;
    return v;
}
bool csCampSkillLearned(uintptr_t base, int i) { return csCampSkillLearnedRaw(base, i) >= 0; }

bool csCampSkillSelected(uintptr_t base, int i) {
    uintptr_t hero = abSelectedHero(base);
    if (!hero || i < 0) return false;
    uintptr_t beg = 0, end = 0;
    if (!safeReadPtr(hero + ACTOR_CAMP_SEL_BEG_OFF, &beg) ||
        !safeReadPtr(hero + ACTOR_CAMP_SEL_END_OFF, &end)) return false;
    if (beg <= 0x10000 || end < beg) return false;
    uintptr_t n = (end - beg) / 4;
    if (n > (uintptr_t)CAMP_SEL_MAX) {
        logLine("charsheet camskill: selected list has %u entries, cap is %d — refusing",
                (unsigned)n, CAMP_SEL_MAX);
        return false;
    }
    for (uintptr_t k = 0; k < n; k++) {
        uint32_t slot = 0;
        if (!safeReadU32(beg + k * 4, &slot)) continue;
        if ((int)slot == i) return true;
    }
    return false;
}

uintptr_t csCampSkillClass(uintptr_t base, int i) {
    uint32_t want = 0;
    if (!csCampSkillHash(base, i, &want)) return 0;
    uintptr_t vec = 0, beg = 0, end = 0;
    if (!safeReadPtr(base + CAMP_REGISTRY_RVA, &vec) || vec <= 0x10000) {
        logLine("charsheet camskill: registry global unreadable");
        return 0;
    }
    if (!safeReadPtr(vec, &beg) || !safeReadPtr(vec + 8, &end)) {
        logLine("charsheet camskill: registry vector at %p unreadable", (void*)vec);
        return 0;
    }
    if (beg <= 0x10000 || end < beg) {
        logLine("charsheet camskill: registry bounds bad (beg=%p end=%p)", (void*)beg, (void*)end);
        return 0;
    }
    uintptr_t n = (end - beg) / CAMP_REGISTRY_STRIDE;
    if (n > 512) {
        logLine("charsheet camskill: registry claims %u entries — refusing", (unsigned)n);
        return 0;
    }
    for (uintptr_t k = 0; k < n; k++) {
        uintptr_t cls = 0;
        if (!safeReadPtr(beg + k * CAMP_REGISTRY_STRIDE, &cls) || cls <= 0x10000) continue;
        uint32_t h = 0;
        if (!safeReadU32(cls + CAMP_SKILL_HASH_OFF, &h)) continue;
        if (h == want) return cls;
    }
    logLine("charsheet camskill: row %d hash 0x%08x not in the registry", i, want);
    return 0;
}

// The id is an inline C-string, like Quirk::Class's — not a std::string.
static bool csCampSkillId(uintptr_t cls, char* out, int outsz) {
    if (!safeReadCStr(cls + CAMP_SKILL_ID_OFF, out, outsz) || !out[0]) return false;
    for (const char* p = out; *p; p++)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
              (*p >= '0' && *p <= '9') || *p == '_')) {
            logLine("charsheet camskill: id \"%s\" is not an identifier — refusing", out);
            return false;
        }
    return true;
}

bool csCampSkillName(uintptr_t base, uintptr_t cls, char* out, int outsz) {
    char id[64];
    if (!csCampSkillId(cls, id, sizeof id)) return false;
    char key[128];
    _snprintf(key, sizeof key, "camping_skill_name_%s", id);
    key[sizeof key - 1] = 0;
    if (!resolveKey(base, key, out, outsz)) {
        logLine("charsheet camskill: \"%s\" did not resolve", key);
        return false;
    }
    abStripMarkup(out);
    return true;
}

int csCampSkillCost(uintptr_t cls) {
    uint32_t c = 0;
    if (!cls || !safeReadU32(cls + CAMP_SKILL_COST_OFF, &c)) return 0;
    if (c == 0 || c > 12) return 0;
    return (int)c;
}

// ---- the effect lines (the tooltip's third block) ----

static uintptr_t csRbLowerBound(uintptr_t head, uint32_t key, uintptr_t keyOff) {
    uintptr_t node = 0;
    if (!safeReadPtr(head + RBNODE_PARENT_OFF, &node) || node <= 0x10000) return 0;   // the root

    uintptr_t bound = head;
    bool reachedLeaf = false;
    for (int guard = 0; guard < 64; guard++) {
        uint8_t nil = 1;
        if (!safeReadU8(node + RBNODE_ISNIL_OFF, &nil)) return 0;
        if (nil) { reachedLeaf = true; break; }
        uint32_t nk = 0;
        if (!safeReadU32(node + keyOff, &nk)) return 0;
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
    if (!reachedLeaf) { logLine("charsheet campeff: buff registry walk too deep — abandoned"); return 0; }
    if (bound == head) return 0;                        // every key was greater: not present

    uint8_t bnil = 1;
    uint32_t bk = 0;
    if (!safeReadU8(bound + RBNODE_ISNIL_OFF, &bnil) || bnil) return 0;
    if (!safeReadU32(bound + keyOff, &bk) || key < bk) return 0;   // lower_bound overshot
    return bound;
}

typedef unsigned char (*CsBuffHasDescFn)(void* buff);
typedef char*         (*CsBuffDescFn)(char* out, void* buff, uintptr_t extra, char withDuration);

bool csBuffDescByKey(uintptr_t base, uint32_t key, const uint32_t* amountBits,
                     char withDuration, char* out, int outsz) {
    if (outsz < CAMP_BUFF_DESC_BUF) return false;
    out[0] = 0;

    uint8_t altFlag = 0;
    safeReadU8(base + CAMP_BUFFREG_FLAG_RVA, &altFlag);
    uintptr_t reg = 0, head = 0;
    if (!safeReadPtr(base + CAMP_BUFFREG_RVA, &reg) || reg <= 0x10000) {
        logLine("buffdesc: buff registry global unreadable");
        return false;
    }
    if (!safeReadPtr(reg + (altFlag ? CAMP_BUFFREG_ALT_OFF : 0), &head) || head <= 0x10000) {
        logLine("buffdesc: buff map head unreadable (reg=%p alt=%d)", (void*)reg, (int)altFlag);
        return false;
    }
    uintptr_t node = csRbLowerBound(head, key, CAMP_BUFFREG_KEY_OFF);
    if (!node) { logLine("buffdesc: buff key 0x%08x not in the registry", key); return false; }

    unsigned char rec[CAMP_BUFF_REC_SZ];
    if (!safeReadBlock(node + CAMP_BUFFREG_VALUE_OFF, rec, CAMP_BUFF_REC_SZ)) {
        logLine("buffdesc: buff record at %p unreadable", (void*)(node + CAMP_BUFFREG_VALUE_OFF));
        return false;
    }
    if (amountBits) memcpy(rec + CAMP_BUFF_REC_AMOUNT_OFF, amountBits, 4);

    CsBuffHasDescFn hasDesc = reinterpret_cast<CsBuffHasDescFn>(base + CAMP_BUFF_HASDESC_RVA);
    CsBuffDescFn    desc    = reinterpret_cast<CsBuffDescFn>(base + CAMP_BUFF_DESC_RVA);
    memset(out, 0, (size_t)outsz);
    __try {
        if (!hasDesc(rec)) { out[0] = 0; return false; }   // the game's own gate: this buff draws nothing
        desc(out, rec, 0, withDuration);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        logLine("buffdesc: buff describer faulted on key 0x%08x", key);
        return false;
    }
    out[outsz - 1] = 0;
    abStripMarkup(out);
    csFlattenLines(out, outsz);
    return out[0] != 0;
}

static bool csCampBuffDesc(uintptr_t base, uintptr_t effect, char* out, int outsz) {
    uint32_t key = 0, amount = 0;
    if (!safeReadU32(effect + CAMP_EFF_BUFFKEY_OFF, &key)) return false;
    if (!safeReadU32(effect + CAMP_EFF_AMOUNT_OFF, &amount)) return false;
    return csBuffDescByKey(base, key, &amount, 1, out, outsz);
}

// ---- CHARACTER STATUS PANEL: the actor's active buffs / debuffs / conditions ----

int spBuffCount(uintptr_t actor, uintptr_t* beginOut) {
    if (beginOut) *beginOut = 0;
    if (!actor) return -1;
    uintptr_t begin = 0, end = 0;
    if (!safeReadPtr(actor + ACTOR_BUFFS_BEGIN_OFF, &begin)) return -1;
    if (!safeReadPtr(actor + ACTOR_BUFFS_END_OFF, &end)) return -1;
    if (begin == 0 && end == 0) return 0;                     // no buffs is normal, not a fault
    if (begin < 0x10000 || end < begin) return -1;
    uintptr_t span = end - begin;
    if (span % ACTOR_BUFF_STRIDE) return -1;                  // not this vector; refuse rather than guess
    uintptr_t n = span / ACTOR_BUFF_STRIDE;
    if (n > (uintptr_t)ACTOR_BUFF_MAX) return -1;
    if (beginOut) *beginOut = begin;
    return (int)n;
}

// ---- Attribution: which QUIRK a buff instance came from ----

static int spQuirkCountFor(uintptr_t hero) {
    if (!hero) return 0;
    uintptr_t begin = 0, end = 0;
    if (!safeReadPtr(hero + HERO_QUIRK_BEGIN_OFF, &begin) ||
        !safeReadPtr(hero + HERO_QUIRK_END_OFF,   &end)) return 0;
    if (!begin || end < begin) return 0;
    uintptr_t span = end - begin;
    if (span % QUIRK_ENTRY_STRIDE != 0) return 0;   // caught mid-move; refuse rather than guess
    uintptr_t n = span / QUIRK_ENTRY_STRIDE;
    return (n > 64) ? 0 : (int)n;
}

static uintptr_t spQuirkAtFor(uintptr_t hero, int i) {
    if (i < 0 || i >= spQuirkCountFor(hero)) return 0;
    uintptr_t begin = 0, qc = 0;
    if (!safeReadPtr(hero + HERO_QUIRK_BEGIN_OFF, &begin) || !begin) return 0;
    if (!safeReadPtr(begin + (uintptr_t)i * QUIRK_ENTRY_STRIDE + QUIRK_ENTRY_CLASS_OFF, &qc)) return 0;
    return (qc > 0x10000) ? qc : 0;
}

static bool spQuirkHasBuff(uintptr_t q, int32_t statType, float amount) {
    uintptr_t begin = 0, end = 0;
    if (!safeReadPtr(q + QUIRK_BUFF_BEGIN_OFF, &begin) ||
        !safeReadPtr(q + QUIRK_BUFF_END_OFF,   &end)) return false;
    if (!begin || end < begin) return false;
    uintptr_t span = end - begin;
    if (span % QUIRK_BUFF_STRIDE != 0) return false;
    uintptr_t cnt = span / QUIRK_BUFF_STRIDE;
    if (cnt > (uintptr_t)QUIRK_BUFF_MAX) return false;
    for (uintptr_t i = 0; i < cnt; i++) {
        uintptr_t e = begin + i * QUIRK_BUFF_STRIDE;
        int32_t est = 0; float eam = 0;
        if (!safeReadU32(e + BUFFREC_STAT_TYPE_OFF, (uint32_t*)&est)) continue;
        abReadF32(e + BUFFREC_AMOUNT_OFF, &eam);
        if (est == statType && fabsf(eam - amount) < 0.0001f) return true;
    }
    return false;
}

static bool spQuirkNameForRecord(uintptr_t base, uintptr_t hero, const unsigned char* rec,
                                 char* out, int outsz) {
    int32_t statType = *(int32_t*)(rec + BUFFREC_STAT_TYPE_OFF);
    float   amount   = *(float*)  (rec + BUFFREC_AMOUNT_OFF);
    int n = spQuirkCountFor(hero);
    for (int i = 0; i < n; i++) {
        uintptr_t q = spQuirkAtFor(hero, i);
        if (!q || !spQuirkHasBuff(q, statType, amount)) continue;
        char id[64];
        if (!csQuirkId(q, id, sizeof id)) return false;
        return csQuirkName(base, id, out, outsz);
    }
    return false;
}

static bool spBuffSourceNameIdx(uintptr_t base, uint32_t src, char* out, int outsz) {
    if (src >= (uint32_t)BUFF_TYPE_TABLE_MAX) return false;
    if (!safeReadCStr(base + BUFF_SRCTYPE_TABLE_RVA + (uintptr_t)src * BUFF_SRCTYPE_STRIDE,
                      out, outsz) || !out[0]) return false;
    return abPlausibleName(out);
}
static bool spBuffSourceName(uintptr_t base, const unsigned char* rec, char* out, int outsz) {
    return spBuffSourceNameIdx(base, *(uint32_t*)(rec + BUFFREC_SOURCE_OFF), out, outsz);
}

// ---- Is this record a BUFF or a DEBUFF? ----
static const char* const kSpBaseStatSubs[] = {
    "max_hp",            // str_ui_MAXHP
    "attack_rating",     // str_ui_ATT_MOD
    "defense_rating",    // str_ui_DEF
    "crit_chance",       // str_ui_CRIT
    "protection_rating", // str_ui_PROT
    "damage_low",        // str_ui_DMG, low half
    "damage_high",       // str_ui_DMG, high half
    "speed_rating",      // str_ui_SPD
};
static const int kSpBaseStatSubCount = (int)(sizeof kSpBaseStatSubs / sizeof kSpBaseStatSubs[0]);

static int spDirPolarity(uintptr_t base, uint32_t stat, float amount, const char* sub) {
    if (amount == 0.0f) return 0;                   // the lambda's own "neither" case
    if (stat >= (uint32_t)BUFF_TYPE_TABLE_MAX) return 0;
    uint8_t higherIsBetter = 0;
    if (!safeReadU8(base + BUFF_STATTYPE_TABLE_RVA + (uintptr_t)stat * BUFF_STATTYPE_STRIDE
                         + BUFF_STATTYPE_HIGHERGOOD_OFF, &higherIsBetter)) {
        logLine("status: direction flag unreadable for stat_type=%u — leaving \"%s\" unlabelled", stat, sub);
        return 0;
    }
    return (higherIsBetter ? (amount > 0.0f) : (amount < 0.0f)) ? 1 : -1;
}

int spBuffPolarity(uintptr_t base, const unsigned char* rec) {
    int32_t rounds = *(int32_t*)(rec + BUFFREC_ROUNDS_OFF);
    if (rounds < 0) return 0;                       // standing, not temporary

    char sub[0x41];
    memcpy(sub, rec + BUFFREC_STATSUB_OFF, sizeof sub - 1);
    sub[sizeof sub - 1] = 0;
    if (!abPlausibleName(sub)) return 0;
    int isBase = 0;
    for (int i = 0; i < kSpBaseStatSubCount; i++)
        if (strcmp(sub, kSpBaseStatSubs[i]) == 0) { isBase = 1; break; }
    if (!isBase) return 0;

    float amount = *(float*)(rec + BUFFREC_AMOUNT_OFF);
    uint32_t stat = *(uint32_t*)(rec + BUFFREC_STAT_TYPE_OFF);
    return spDirPolarity(base, stat, amount, sub);
}

static bool spBuffIsStandingCarrier(uintptr_t base, const unsigned char* rec) {
    int32_t rounds = *(int32_t*)(rec + BUFFREC_ROUNDS_OFF);
    if (rounds >= 0) return false;
    char src[0x44];
    if (!spBuffSourceName(base, rec, src, sizeof src)) return true;   // unreadable: refuse to claim
    static const char* const kStanding[] = {
        BSRC_QUIRK_NAME, "bsrc_disease", "bsrc_trinket", "bsrc_trinket_set",
        "bsrc_trinket_additional_effect",
    };
    for (int i = 0; i < (int)(sizeof kStanding / sizeof kStanding[0]); i++)
        if (strcmp(src, kStanding[i]) == 0) return true;
    return false;
}

static bool spCondHasOwnVoice(uint32_t stat) {
    static const int kOwnVoice[] = {
        0 /* status */,       STAT_TYPE_TAG,         STAT_TYPE_POISON_DOT,
        STAT_TYPE_BLEED_DOT,  STAT_TYPE_BURN_DOT,    STAT_TYPE_RIPOSTE,
        STAT_TYPE_GUARDED,    STAT_TYPE_STRESS_DOT,  STAT_TYPE_SHUFFLE_DOT,
        STAT_TYPE_HEAL_DOT,   STAT_TYPE_STEALTH,
    };
    for (int i = 0; i < (int)(sizeof kOwnVoice / sizeof kOwnVoice[0]); i++)
        if (stat == (uint32_t)kOwnVoice[i]) return true;
    return false;
}

int spBuffPolarityAny(uintptr_t base, const unsigned char* rec) {
    if (spBuffIsStandingCarrier(base, rec)) return 0;

    uint32_t stat = *(uint32_t*)(rec + BUFFREC_STAT_TYPE_OFF);
    if (spCondHasOwnVoice(stat)) return 0;

    char sub[0x41];                                  // diagnostics only in this form
    memcpy(sub, rec + BUFFREC_STATSUB_OFF, sizeof sub - 1);
    sub[sizeof sub - 1] = 0;

    float amount = *(float*)(rec + BUFFREC_AMOUNT_OFF);
    return spDirPolarity(base, stat, amount, sub);
}

bool spPolarityLabel(uintptr_t base, int polarity, char* out, int outsz) {
    out[0] = 0;
    if (polarity == 0) return false;
    if (!abTipPlain(base, polarity > 0 ? "str_ui_buff" : "str_ui_debuff", out, outsz)) {
        logLine("status: \"%s\" did not resolve — the line goes out unlabelled",
                polarity > 0 ? "str_ui_buff" : "str_ui_debuff");
        return false;
    }
    int n = (int)strlen(out);
    while (n > 0 && (out[n - 1] == ' ' || out[n - 1] == '!')) out[--n] = 0;
    csTrimLabel(out);
    return out[0] != 0;
}

bool spBuffTextFromRecord(uintptr_t base, uintptr_t actor, const unsigned char* rec,
                                 char* out, int outsz) {
    if (outsz < CAMP_BUFF_DESC_BUF) return false;             // the describer writes out[0xff]
    out[0] = 0;

    int32_t rounds  = *(int32_t*)(rec + BUFFREC_ROUNDS_OFF);
    uint32_t source = *(uint32_t*)(rec + BUFFREC_SOURCE_OFF);
    uint32_t stat   = *(uint32_t*)(rec + BUFFREC_STAT_TYPE_OFF);

    char withDur = 1;
    if (rounds == -1 && source < BUFF_TYPE_TABLE_MAX && stat < BUFF_TYPE_TABLE_MAX) {
        uint8_t campSrc = 0, campStat = 0;
        safeReadU8(base + BUFF_SRCTYPE_TABLE_RVA + (uintptr_t)source * BUFF_SRCTYPE_STRIDE
                        + BUFF_SRCTYPE_CAMP_OFF, &campSrc);
        safeReadU8(base + BUFF_STATTYPE_TABLE_RVA + (uintptr_t)stat * BUFF_STATTYPE_STRIDE
                        + BUFF_STATTYPE_CAMP_OFF, &campStat);
        if (!(campSrc && campStat)) withDur = 0;   // the branch that would have printed "(Quest)"
    }

    CsBuffHasDescFn hasDesc = reinterpret_cast<CsBuffHasDescFn>(base + CAMP_BUFF_HASDESC_RVA);
    CsBuffDescFn    desc    = reinterpret_cast<CsBuffDescFn>(base + CAMP_BUFF_DESC_RVA);
    char dbuf[CAMP_BUFF_DESC_BUF];
    memset(dbuf, 0, sizeof dbuf);
    unsigned char work[CAMP_BUFF_REC_SZ];
    memcpy(work, rec, CAMP_BUFF_REC_SZ);
    __try {
        if (!hasDesc(work)) { out[0] = 0; return false; }     // the game's own gate: draws no icon
        desc(dbuf, work, 0, withDur);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        logLine("status: buff describer FAULTED on a record of actor=%p", (void*)actor);
        return false;
    }
    dbuf[sizeof dbuf - 1] = 0;
    abStripMarkup(dbuf);
    csFlattenLines(dbuf, sizeof dbuf);
    if (!dbuf[0]) return false;

    char qname[128];
    char srcName[0x44];
    bool isQuirk = spBuffSourceName(base, rec, srcName, sizeof srcName) &&
                   strcmp(srcName, BSRC_QUIRK_NAME) == 0;
    bool named = isQuirk && spQuirkNameForRecord(base, actor, rec, qname, sizeof qname) && qname[0];

    char label[96];
    int pol = spBuffPolarity(base, rec);
    if (spPolarityLabel(base, pol, label, sizeof label))
        _snprintf(out, outsz, "%s: %s", label, dbuf);
    else if (named) _snprintf(out, outsz, "%s: %s", qname, dbuf);
    else            _snprintf(out, outsz, "%s", dbuf);
    out[outsz - 1] = 0;

    logLine("status: buff stat=%u src=%u(%s) rounds=%d withDur=%d named=%d pol=%d -> \"%s\"",
            stat, source, isQuirk ? srcName : "", rounds, (int)withDur, (int)named, pol, out);
    return out[0] != 0;
}

static bool spBuffText(uintptr_t base, uintptr_t actor, int index, char* out, int outsz) {
    if (outsz < CAMP_BUFF_DESC_BUF) return false;
    out[0] = 0;

    uintptr_t begin = 0;
    int n = spBuffCount(actor, &begin);
    if (n <= 0 || index < 0 || index >= n) return false;

    uintptr_t elem = begin + (uintptr_t)index * ACTOR_BUFF_STRIDE;
    unsigned char rec[CAMP_BUFF_REC_SZ];
    if (!safeReadBlock(elem + ACTOR_BUFF_RECORD_OFF, rec, CAMP_BUFF_REC_SZ)) {
        logLine("status: buff %d of %d at %p unreadable", index, n, (void*)elem);
        return false;
    }
    return spBuffTextFromRecord(base, actor, rec, out, outsz);
}

// ---- The CONDITIONS line ----
// ---- BUCKETS ----

// ---- CONTROLLED BURN (The Fire's Edge) ----
static const uintptr_t RAID_CBURN_BEG_OFF = 0x48a0;  // raidRoot+: controlled-burn vector begin
static const uintptr_t RAID_CBURN_END_OFF = 0x48a8;  // ...end
static const int       CBURN_STRIDE       = 0x18;
static const uintptr_t CBURN_AMOUNT_OFF   = 0x04;    // int burn applied per trigger
static const uintptr_t CBURN_ROUNDS_OFF   = 0x08;    // int rounds left; <= 0 means the game skips it
static const uintptr_t CBURN_ACTORID_OFF  = 0x0c;    // int: the actor standing in the burning rank
static const int       CBURN_MAX          = 64;      // sanity cap; four ranks a side is the reality

static const uintptr_t MONSTER_ID_OFF     = 0x12b0;

static bool spActorId(uintptr_t base, uintptr_t actor, uint32_t* out) {
    uintptr_t vft = 0;
    if (!actor || !safeReadPtr(actor, &vft) || vft <= 0x10000) return false;
    uintptr_t off = 0;
    if      (vft == base + HERO_VFT)    off = ACTOR_ID_OFF;
    else if (vft == base + MONSTER_VFT) off = MONSTER_ID_OFF;
    else {
        static uintptr_t s_lastVft = 0;
        if (vft != s_lastVft) {
            s_lastVft = vft;
            logLine("status: actor=%p has vftable +0x%llx — neither Hero (+0x%llx) nor Monster "
                    "(+0x%llx); cannot read its id",
                    (void*)actor, (unsigned long long)(vft - base),
                    (unsigned long long)HERO_VFT, (unsigned long long)MONSTER_VFT);
        }
        return false;
    }
    return safeReadU32(actor + off, out);
}

bool spActorIsHero(uintptr_t base, uintptr_t actor) {
    uintptr_t vft = 0;
    if (!actor || !safeReadPtr(actor, &vft) || vft <= 0x10000) return false;
    return vft == base + HERO_VFT;
}

static bool spControlledBurnInto(uintptr_t base, uintptr_t actor, char* out, int outsz) {
    out[0] = 0;
    uint32_t id = 0;
    if (!spActorId(base, actor, &id)) return false;

    uintptr_t root = 0;
    if (!safeReadPtr(base + MAP_ROOT_RVA, &root) || root <= 0x10000) return false;  // not in a raid
    uintptr_t beg = 0, end = 0;
    if (!safeReadPtr(root + RAID_CBURN_BEG_OFF, &beg) ||
        !safeReadPtr(root + RAID_CBURN_END_OFF, &end)) return false;
    if (beg == 0 && end == 0) return false;                       // empty vector: nothing is burning
    if (beg <= 0x10000 || end < beg) return false;
    uintptr_t count = (end - beg) / CBURN_STRIDE;
    if (count == 0) return false;
    if (count > (uintptr_t)CBURN_MAX) {
        logLine("status: controlled-burn vector at raidRoot+0x%llx holds %llu entries — out of "
                "range, ignoring", (unsigned long long)RAID_CBURN_BEG_OFF,
                (unsigned long long)count);
        return false;
    }

    for (uintptr_t i = 0; i < count; i++) {
        uintptr_t e = beg + i * CBURN_STRIDE;
        uint32_t owner = 0;
        int32_t rounds = 0, amount = 0;
        if (!safeReadU32(e + CBURN_ACTORID_OFF, &owner) || owner != id) continue;
        if (!safeReadU32(e + CBURN_ROUNDS_OFF, (uint32_t*)&rounds) || rounds <= 0) continue;
        safeReadU32(e + CBURN_AMOUNT_OFF, (uint32_t*)&amount);

        char frag[160];
        if (abTipPlain(base, "tray_icon_tooltip_controlled_burn_title", frag, sizeof frag))
            abAppend(out, outsz, frag);
        if (abTipInt(base, "tray_icon_tooltip_controlled_burn_turn_start", amount, frag, sizeof frag))
            abAppend(out, outsz, frag);
        if (abTipInt(base, "tray_icon_tooltip_controlled_burn_on_move", amount, frag, sizeof frag))
            abAppend(out, outsz, frag);
        if (abTipInt(base, "tray_icon_tooltip_controlled_burn_rounds", rounds, frag, sizeof frag))
            abAppend(out, outsz, frag);
        if (!out[0])
            logLine("status: controlled burn on actor id %u (%d burn, %d rounds) but not one of its "
                    "four tray keys resolved — it goes unsaid", owner, amount, rounds);
        return out[0] != 0;
    }
    return false;
}

static int spStatQuery(uintptr_t actor, int statType, float* sumOut, int* roundsOut) {
    if (sumOut) *sumOut = 0;
    if (roundsOut) *roundsOut = 0;
    uintptr_t begin = 0;
    int n = spBuffCount(actor, &begin);
    int hits = 0;
    for (int i = 0; i < n; i++) {
        uintptr_t rec = begin + (uintptr_t)i * ACTOR_BUFF_STRIDE + ACTOR_BUFF_RECORD_OFF;
        uint32_t st = 0;
        if (!safeReadU32(rec + BUFFREC_STAT_TYPE_OFF, &st) || (int)st != statType) continue;
        float a = 0; int32_t r = 0;
        abReadF32(rec + BUFFREC_AMOUNT_OFF, &a);
        safeReadU32(rec + BUFFREC_ROUNDS_OFF, (uint32_t*)&r);
        if (sumOut) *sumOut += a;
        if (roundsOut && r > *roundsOut) *roundsOut = r;
        hits++;
    }
    return hits;
}

// ---- THE STAT / RESISTANCE TOOLTIP BREAKDOWN ----

static const uintptr_t STAT_SRC_PTR_OFF     = 0x90;  // stat block +: ptr -> contribution record
static const uintptr_t STAT_SRC_WEAPON_OFF  = 0x08;  // float: the weapon's contribution
static const uintptr_t STAT_SRC_ARMOUR_OFF  = 0x0c;  // float: the armour's contribution

static const int CS_TIP_SRC_COUNT = 0x22;

static const float CS_TIP_EPS = 1e-6f;

static const uintptr_t CS_RESIST_LVLMULT_OFF = 0x94;  // float: per-level multiplier
static const uintptr_t CS_RESIST_LVLARR_BEG  = 0x98;  // float*: per-level bonus array begin
static const uintptr_t CS_RESIST_LVLARR_END  = 0xa0;  // float*: ... end

typedef char (*CsBuffRuleFn)(void* actor, void* rec, void* p3, void* p4, long long p5);
static bool csTipRuleActive(uintptr_t base, uintptr_t actor, uintptr_t recLive) {
    CsBuffRuleFn fn = reinterpret_cast<CsBuffRuleFn>(base + BUFF_RULE_GATE_RVA);
    char keep = 1;
    __try { keep = fn(reinterpret_cast<void*>(actor), reinterpret_cast<void*>(recLive), 0, 0, 0); }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        logLine("stattip: rule gate faulted on rec %llx — including it",
                (unsigned long long)recLive);
        return true;
    }
    return keep != 0;
}

static void csTipSumBySource(uintptr_t base, uintptr_t actor, int statType, const char* sub,
                             float sums[CS_TIP_SRC_COUNT], int counts[CS_TIP_SRC_COUNT]) {
    for (int s = 0; s < CS_TIP_SRC_COUNT; s++) { sums[s] = 0.0f; counts[s] = 0; }
    uintptr_t begin = 0;
    int n = spBuffCount(actor, &begin);
    for (int i = 0; i < n; i++) {
        uintptr_t live = begin + (uintptr_t)i * ACTOR_BUFF_STRIDE + ACTOR_BUFF_RECORD_OFF;
        unsigned char rec[CAMP_BUFF_REC_SZ];
        if (!safeReadBlock(live, rec, CAMP_BUFF_REC_SZ)) continue;
        if (*(int32_t*)(rec + BUFFREC_STAT_TYPE_OFF) != statType) continue;
        if (sub) {
            char rsub[0x41];
            memcpy(rsub, rec + BUFFREC_STATSUB_OFF, sizeof rsub - 1);
            rsub[sizeof rsub - 1] = 0;
            if (strcmp(rsub, sub) != 0) continue;
        }
        uint32_t src = *(uint32_t*)(rec + BUFFREC_SOURCE_OFF);
        if (src >= (uint32_t)CS_TIP_SRC_COUNT) continue;
        if (!csTipRuleActive(base, actor, live)) continue;
        sums[src] += *(float*)(rec + BUFFREC_AMOUNT_OFF);
        counts[src]++;
    }
}

static bool csTipValueLine(uintptr_t base, const char* key, float v, int dec, bool pct,
                           bool sign, bool suppressZero, char* out, int outsz) {
    if (suppressZero && v > -CS_TIP_EPS && v < CS_TIP_EPS) return false;
    char label[128];
    if (!resolveKey(base, key, label, sizeof label)) {
        logLine("stattip: \"%s\" did not resolve", key);
        return false;
    }
    abStripMarkup(label);
    if (dec == 0) v = roundf(v);
    _snprintf(out, outsz, sign ? "%+.*f%s %s" : "%.*f%s %s", dec, v, pct ? "%" : "", label);
    out[outsz - 1] = 0;
    return true;
}

static int csTipBuffLines(uintptr_t base, const float sums[CS_TIP_SRC_COUNT],
                          const int counts[CS_TIP_SRC_COUNT], float scale, bool pct, int dec,
                          bool suppressZero, char lines[AB_TIP_MAX_LINES][AB_TIP_LINE_SZ], int at) {
    int wrote = 0;
    for (int s = 0; s < CS_TIP_SRC_COUNT && at + wrote < AB_TIP_MAX_LINES; s++) {
        if (counts[s] == 0) continue;
        char nm[0x48];
        if (!spBuffSourceNameIdx(base, (uint32_t)s, nm, sizeof nm)) continue;
        char key[96];
        _snprintf(key, sizeof key, "buff_%s", nm);
        key[sizeof key - 1] = 0;
        if (csTipValueLine(base, key, sums[s] * scale, dec, pct, true, suppressZero,
                           lines[at + wrote], AB_TIP_LINE_SZ))
            wrote++;
    }
    return wrote;
}

// ---- THE PARTY SCOUTING CHANCE ----
static const int STAT_TYPE_SCOUTING = 0x13;

static bool csFmtOneFloat(const char* f) {
    int convs = 0;
    for (const char* p = f; *p; p++) {
        if (*p != '%') continue;
        if (p[1] == '%') { p++; continue; }
        const char* q = p + 1;
        while (*q && strchr("-+ #0123456789.", *q)) q++;
        if (!*q || !strchr("fFeEgG", *q)) return false;
        convs++;
        p = q;
    }
    return convs == 1;
}

int csPartyScoutLines(uintptr_t base, const uintptr_t* heroes, int nHeroes,
                      char lines[AB_TIP_MAX_LINES][AB_TIP_LINE_SZ]) {
    float baseChance = 0.0f;
    if (!abReadF32(base + SCOUT_BASE_RVA, &baseChance)) {
        logLine("scouting: the base (SCOUT_BASE_RVA) did not read -- no line");
        return 0;
    }
    float sums[CS_TIP_SRC_COUNT]; int counts[CS_TIP_SRC_COUNT];
    for (int s = 0; s < CS_TIP_SRC_COUNT; s++) { sums[s] = 0.0f; counts[s] = 0; }
    float total = baseChance;
    for (int h = 0; h < nHeroes; h++) {
        if (!heroes[h]) continue;
        float hs[CS_TIP_SRC_COUNT]; int hc[CS_TIP_SRC_COUNT];
        csTipSumBySource(base, heroes[h], STAT_TYPE_SCOUTING, nullptr, hs, hc);
        for (int s = 0; s < CS_TIP_SRC_COUNT; s++) {
            sums[s] += hs[s]; counts[s] += hc[s]; total += hs[s];
        }
    }

    int n = 0;
    char fmt[128];
    if (resolveKey(base, "str_stat_scouting_chance", fmt, sizeof fmt) && fmt[0]) {
        abStripMarkup(fmt);
        if (csFmtOneFloat(fmt)) _snprintf(lines[0], AB_TIP_LINE_SZ, fmt, (double)(total * 100.0f));
        else {
            logLine("scouting: \"str_stat_scouting_chance\" = \"%s\" is not a one-float format", fmt);
            fmt[0] = 0;
        }
    } else {
        logLine("scouting: \"str_stat_scouting_chance\" did not resolve");
        fmt[0] = 0;
    }
    if (!fmt[0]) {
        char word[64];
        if (!resolveKey(base, "str_scouting", word, sizeof word)) word[0] = 0;
        abStripMarkup(word);
        _snprintf(lines[0], AB_TIP_LINE_SZ, "%s%s%.1f%%", word, word[0] ? " " : "", total * 100.0f);
    }
    lines[0][AB_TIP_LINE_SZ - 1] = 0;
    n = 1;
    if (csTipValueLine(base, "str_stat_base_scouting_chance", baseChance * 100.0f, 0, true, false,
                       false, lines[n], AB_TIP_LINE_SZ))
        n++;
    n += csTipBuffLines(base, sums, counts, 100.0f, true, 0, true, lines, n);
    logLine("scouting: %d heroes, base %.3f, total %.3f, %d lines", nHeroes, baseChance, total, n);
    return n;
}

static bool csTipSrcField(uintptr_t hero, uintptr_t blockOff, uintptr_t fld, float* out) {
    uintptr_t rec = 0;
    if (!safeReadPtr(hero + blockOff + STAT_SRC_PTR_OFF, &rec) || rec <= 0x10000) {
        logLine("stattip: stat block +%llx has no source record", (unsigned long long)blockOff);
        return false;
    }
    return abReadF32(rec + fld, out);
}

static int csStatTipBody(uintptr_t base, uintptr_t hero, int i,
                         char lines[AB_TIP_MAX_LINES][AB_TIP_LINE_SZ], int at) {
    int  n = 0;
    float v = 0.0f;
    float sums[CS_TIP_SRC_COUNT]; int counts[CS_TIP_SRC_COUNT];
    switch (i) {
    case 0:
        if (csTipSrcField(hero, CS_STAT_MAXHP_OFF, STAT_SRC_ARMOUR_OFF, &v) &&
            csTipValueLine(base, "str_stat_base_armour", v, 0, false, false, false,
                           lines[at + n], AB_TIP_LINE_SZ)) n++;
        csTipSumBySource(base, hero, STAT_TYPE_COMBAT_MULT, "max_hp", sums, counts);
        n += csTipBuffLines(base, sums, counts, CS_PERCENT_SCALE, true, 0, true, lines, at + n);
        break;
    case 1:
        if (csTipSrcField(hero, CS_STAT_ATT_OFF, STAT_SRC_WEAPON_OFF, &v) &&
            csTipValueLine(base, "str_stat_additional_weapon", v * CS_PERCENT_SCALE, 0, false,
                           true, true, lines[at + n], AB_TIP_LINE_SZ)) n++;
        csTipSumBySource(base, hero, STAT_TYPE_COMBAT_ADD, "attack_rating", sums, counts);
        n += csTipBuffLines(base, sums, counts, CS_PERCENT_SCALE, false, 0, true, lines, at + n);
        break;
    case 2:
        if (csTipSrcField(hero, CS_STAT_DEF_OFF, STAT_SRC_ARMOUR_OFF, &v) &&
            csTipValueLine(base, "str_stat_base_armour", v * CS_PERCENT_SCALE, 0, false, false,
                           false, lines[at + n], AB_TIP_LINE_SZ)) n++;
        csTipSumBySource(base, hero, STAT_TYPE_COMBAT_ADD, "defense_rating", sums, counts);
        n += csTipBuffLines(base, sums, counts, CS_PERCENT_SCALE, false, 0, true, lines, at + n);
        break;
    case 3:
        if (csTipSrcField(hero, CS_STAT_CRIT_OFF, STAT_SRC_WEAPON_OFF, &v) &&
            csTipValueLine(base, "str_stat_base_weapon", v * CS_PERCENT_SCALE, 1, true, false,
                           false, lines[at + n], AB_TIP_LINE_SZ)) n++;
        csTipSumBySource(base, hero, STAT_TYPE_COMBAT_ADD, "crit_chance", sums, counts);
        n += csTipBuffLines(base, sums, counts, CS_PERCENT_SCALE, true, 1, true, lines, at + n);
        break;
    case 4:
        csTipSumBySource(base, hero, STAT_TYPE_COMBAT_ADD, "protection_rating", sums, counts);
        n += csTipBuffLines(base, sums, counts, CS_PERCENT_SCALE, true, 0, true, lines, at + n);
        break;
    case 5: {
        float lo = 0.0f, hi = 0.0f;
        if (csTipSrcField(hero, CS_STAT_DMGLO_OFF, STAT_SRC_WEAPON_OFF, &lo) &&
            csTipSrcField(hero, CS_STAT_DMGHI_OFF, STAT_SRC_WEAPON_OFF, &hi)) {
            char label[128];
            if (resolveKey(base, "str_stat_base_weapon", label, sizeof label)) {
                abStripMarkup(label);
                _snprintf(lines[at + n], AB_TIP_LINE_SZ, "%.0f - %.0f %s",
                          roundf(lo), roundf(hi), label);
                lines[at + n][AB_TIP_LINE_SZ - 1] = 0;
                n++;
            }
        }
        csTipSumBySource(base, hero, STAT_TYPE_COMBAT_ADD, "damage_low", sums, counts);
        n += csTipBuffLines(base, sums, counts, 1.0f, false, 0, true, lines, at + n);
        csTipSumBySource(base, hero, STAT_TYPE_COMBAT_MULT, "damage_low", sums, counts);
        n += csTipBuffLines(base, sums, counts, CS_PERCENT_SCALE, true, 0, true, lines, at + n);
        break;
    }
    case 6:
        if (csTipSrcField(hero, CS_STAT_SPD_OFF, STAT_SRC_WEAPON_OFF, &v) &&
            csTipValueLine(base, "str_stat_base_weapon", v, 0, false, false, false,
                           lines[at + n], AB_TIP_LINE_SZ)) n++;
        if (csTipSrcField(hero, CS_STAT_SPD_OFF, STAT_SRC_ARMOUR_OFF, &v) &&
            csTipValueLine(base, "str_stat_additional_armour", v, 0, false, true, true,
                           lines[at + n], AB_TIP_LINE_SZ)) n++;
        csTipSumBySource(base, hero, STAT_TYPE_COMBAT_ADD, "speed_rating", sums, counts);
        n += csTipBuffLines(base, sums, counts, 1.0f, false, 0, true, lines, at + n);
        break;
    }
    return n;
}

int csStatTipLines(uintptr_t base, int i, char lines[AB_TIP_MAX_LINES][AB_TIP_LINE_SZ]) {
    for (int k = 0; k < AB_TIP_MAX_LINES; k++) lines[k][0] = 0;
    uintptr_t hero = abSelectedHero(base);
    if (!hero) return 0;
    if (!csStatRowText(base, i, lines[0], AB_TIP_LINE_SZ)) return 0;
    return 1 + csStatTipBody(base, hero, i, lines, 1);
}

// ---- THE BUFFED / DEBUFFED WORD ----
static bool csStatBuffState(uintptr_t hero, int i, int* out) {
    *out = 0;
    if (!hero || i < 0 || i >= kCsStatRowCount) return false;

    const CsStatRow* row = &kCsStatRows[i];
    uintptr_t block = hero + row->off;
    bool clamp = (row->fmt != CS_FMT_ACCMOD);

    float withBuffs = 0.0f, without = 0.0f;
    if (!abStatValue(block, CS_STAT_MASK,      clamp, &withBuffs) ||
        !abStatValue(block, CS_STAT_BASE_MASK, clamp, &without)) {
        logLine("statpolarity: \"%s\" would not evaluate on hero=%p", row->key, (void*)hero);
        return false;
    }
    float delta = withBuffs - without;
    if (delta > 0.0f) *out =  1;
    else if (delta < 0.0f) *out = -1;
    return true;
}

static void csAppendComma(char* out, int outsz, const char* frag) {
    if (!frag || !frag[0]) return;
    size_t n = strlen(out);
    if (n && n + 2 < (size_t)outsz) { out[n++] = ','; out[n++] = ' '; out[n] = 0; }
    _snprintf(out + n, outsz - (int)n, "%s", frag);
    out[outsz - 1] = 0;
}

bool csStatTipLineFor(uintptr_t base, uintptr_t hero, int i, char* out, int outsz) {
    if (outsz > 0) out[0] = 0;
    if (!hero || i < 0 || i >= kCsStatRowCount || outsz <= 0) return false;

    char row[AB_TIP_LINE_SZ];
    if (!csStatRowFor(base, hero, i, row, sizeof row)) return false;
    _snprintf(out, outsz, "%s", row);
    out[outsz - 1] = 0;
    csStatTipDecorate(base, hero, i, out, outsz);
    return out[0] != 0;
}

void csStatTipDecorate(uintptr_t base, uintptr_t actor, int i, char* line, int linesz) {
    if (!actor || i < 0 || i >= kCsStatRowCount || linesz <= 0 || !line[0]) return;

    int state = 0;
    if (csStatBuffState(actor, i, &state) && state) {
        char dec[AB_TIP_LINE_SZ];
        _snprintf(dec, sizeof dec,
                  axs(state > 0 ? AXS_CS_STAT_BUFFED_FMT : AXS_CS_STAT_DEBUFFED_FMT), line);
        dec[sizeof dec - 1] = 0;
        _snprintf(line, linesz, "%s", dec);
        line[linesz - 1] = 0;
    }
    size_t len = strlen(line);
    if (len && line[len - 1] != '.' && len + 1 < (size_t)linesz) { line[len] = '.'; line[len + 1] = 0; }

    char tip[AB_TIP_MAX_LINES][AB_TIP_LINE_SZ];
    for (int k = 0; k < AB_TIP_MAX_LINES; k++) tip[k][0] = 0;
    int n = csStatTipBody(base, actor, i, tip, 0);
    char list[AB_TIP_LINE_SZ];
    list[0] = 0;
    for (int k = 0; k < n && k < AB_TIP_MAX_LINES; k++) csAppendComma(list, sizeof list, tip[k]);
    if (list[0]) abAppendFrag(line, linesz, list);
}

int csStatRowIndexForOff(uintptr_t off) {
    for (int i = 0; i < kCsStatRowCount; i++)
        if (kCsStatRows[i].off == off) return i;
    return -1;
}

// ---- INCOMING MODIFIERS — "Healing received: +20% Quirk" for the health/stress lines ----
static const uintptr_t BUFF_STATTYPE_NAME_BACKOFF = 0x50;

static bool spStatTypeName(uintptr_t base, int idx, char* out, int outsz) {
    if (idx < 0 || idx >= BUFF_TYPE_TABLE_MAX) return false;
    if (!safeReadCStr(base + BUFF_STATTYPE_TABLE_RVA - BUFF_STATTYPE_NAME_BACKOFF
                           + (uintptr_t)idx * BUFF_STATTYPE_STRIDE, out, outsz) || !out[0])
        return false;
    return abPlausibleName(out);
}

static int spStatTypeIndexByName(uintptr_t base, const char* name) {
    for (int i = 0; i < BUFF_TYPE_TABLE_MAX; i++) {
        char nm[0x41];
        if (!spStatTypeName(base, i, nm, sizeof nm)) continue;
        if (strcmp(nm, name) == 0) return i;
    }
    return -1;
}

static bool spIncomingLabel(uintptr_t base, const char* typeName, char* out, int outsz) {
    out[0] = 0;
    char key[96];
    _snprintf(key, sizeof key, "buff_stat_tooltip_%s", typeName);
    key[sizeof key - 1] = 0;
    char fmt[256];
    if (!resolveKey(base, key, fmt, sizeof fmt) || !fmt[0]) {
        logLine("incoming: \"%s\" did not resolve — the group stays unspoken", key);
        return false;
    }
    abStripMarkup(fmt);
    const char* pct = abFirstConversion(fmt);
    char head[128], tail[128];
    head[0] = tail[0] = 0;
    if (!pct) {
        strncpy(head, fmt, sizeof head - 1); head[sizeof head - 1] = 0;
    } else {
        int hn = (int)(pct - fmt);
        if (hn > (int)sizeof head - 1) hn = (int)sizeof head - 1;
        memcpy(head, fmt, (size_t)hn); head[hn] = 0;
        const char* e = pct + 1;
        while (*e && !((*e >= 'a' && *e <= 'z') || (*e >= 'A' && *e <= 'Z'))) e++;
        if (*e) e++;                                  // past the conversion letter
        if (e[0] == '%' && e[1] == '%') e += 2;       // the literal '%' riding "%+d%%"
        strncpy(tail, e, sizeof tail - 1); tail[sizeof tail - 1] = 0;
    }
    char* pick = tail[0] ? tail : head;
    while (*pick == ' ' || *pick == ':') pick++;
    csTrimLabel(pick);
    if (!pick[0] || abFirstConversion(pick)) {        // empty, or a second conversion survived
        logLine("incoming: \"%s\" -> \"%s\" leaves no clean label — the group stays unspoken",
                key, fmt);
        return false;
    }
    _snprintf(out, outsz, "%s", pick);
    out[outsz - 1] = 0;
    return true;
}

bool spIncomingModsFrag(uintptr_t base, uintptr_t hero, const char* typeName,
                        char* out, int outsz) {
    if (outsz > 0) out[0] = 0;
    if (!hero || outsz <= 0) return false;
    int idx = spStatTypeIndexByName(base, typeName);
    if (idx < 0) {
        logLine("incoming: stat type \"%s\" not in the name table — the group stays unspoken",
                typeName);
        return false;
    }
    float sums[CS_TIP_SRC_COUNT]; int counts[CS_TIP_SRC_COUNT];
    csTipSumBySource(base, hero, idx, NULL, sums, counts);
    char tip[AB_TIP_MAX_LINES][AB_TIP_LINE_SZ];
    for (int k = 0; k < AB_TIP_MAX_LINES; k++) tip[k][0] = 0;
    int n = csTipBuffLines(base, sums, counts, CS_PERCENT_SCALE, true, 0, true, tip, 0);
    if (n <= 0) return false;
    char label[128];
    if (!spIncomingLabel(base, typeName, label, sizeof label)) return false;
    char list[AB_TIP_LINE_SZ];
    list[0] = 0;
    for (int k = 0; k < n && k < AB_TIP_MAX_LINES; k++) csAppendComma(list, sizeof list, tip[k]);
    if (!list[0]) return false;
    _snprintf(out, outsz, "%s: %s", label, list);
    out[outsz - 1] = 0;
    return true;
}

static int csResistTipBody(uintptr_t base, uintptr_t hero, int i,
                           char lines[AB_TIP_MAX_LINES][AB_TIP_LINE_SZ], int at, bool full) {
    int n = 0;

    if (full) {
        uintptr_t vft = 0, fn = 0, obj = 0;
        if (safeReadPtr(hero, &vft) && vft > 0x10000 &&
            safeReadPtr(vft + 0x30, &fn) && fn > 0x10000) {
            typedef uintptr_t (*CsGetObjFn)(void* actor);
            __try { obj = reinterpret_cast<CsGetObjFn>(fn)(reinterpret_cast<void*>(hero)); }
            __except (EXCEPTION_EXECUTE_HANDLER) { obj = 0; }
        }
        float v = 0.0f;
        if (obj > 0x10000 && abReadF32(obj + 0xb10 + (uintptr_t)i * 4, &v)) {
            if (csTipValueLine(base, "resistance_base", v * CS_PERCENT_SCALE, 0, true, false, false,
                               lines[at + n], AB_TIP_LINE_SZ)) n++;
        } else {
            logLine("stattip: resist %d base unreadable (obj=%llx)", i, (unsigned long long)obj);
        }
    }

    int level = full ? csResolveLevel(base, hero) : 0;
    if (level > 0) {
        uintptr_t rec = base + CS_RESIST_TABLE_RVA + (uintptr_t)i * CS_RESIST_STRIDE;
        float mult = 0.0f, bonus = 0.0f;
        if (abReadF32(rec + CS_RESIST_LVLMULT_OFF, &mult)) bonus = (float)level * mult;
        uintptr_t ab = 0, ae = 0;
        if (safeReadPtr(rec + CS_RESIST_LVLARR_BEG, &ab) &&
            safeReadPtr(rec + CS_RESIST_LVLARR_END, &ae) && ab > 0x10000 && ae >= ab) {
            uintptr_t cnt = (ae - ab) / 4;
            float x = 0.0f;
            if ((uintptr_t)level < cnt && abReadF32(ab + (uintptr_t)level * 4, &x)) bonus += x;
        }
        if (bonus > 0.0f &&
            csTipValueLine(base, "resistance_resolve_level", bonus * CS_PERCENT_SCALE, 0, true,
                           true, false, lines[at + n], AB_TIP_LINE_SZ)) n++;
    }

    char sub[64];
    if (csResistRawId(base, i, sub, sizeof sub)) {
        float sums[CS_TIP_SRC_COUNT]; int counts[CS_TIP_SRC_COUNT];
        csTipSumBySource(base, hero, STAT_TYPE_RESISTANCE, sub, sums, counts);
        n += csTipBuffLines(base, sums, counts, CS_PERCENT_SCALE, true, 0, false, lines, at + n);
    }

    if (full && i == CS_RESIST_TRAP_IDX && n < AB_TIP_MAX_LINES - at) {
        float total = 0.0f, dbonus = 0.0f;
        if (csResistValue(base, hero, CS_RESIST_TRAP_IDX, &total) &&
            abReadF32(base + TRAP_SCOUT_BONUS_RVA, &dbonus)) {
            if (abTipFloat(base, "resistance_trap_disarm_format",
                           (total + dbonus) * CS_PERCENT_SCALE, lines[at + n], AB_TIP_LINE_SZ)) n++;
        }
    }
    return n;
}

int csResistTipLines(uintptr_t base, int i, char lines[AB_TIP_MAX_LINES][AB_TIP_LINE_SZ]) {
    for (int k = 0; k < AB_TIP_MAX_LINES; k++) lines[k][0] = 0;
    uintptr_t hero = abSelectedHero(base);
    if (!hero) return 0;
    if (!csResistRowText(base, i, lines[0], AB_TIP_LINE_SZ)) return 0;
    return 1 + csResistTipBody(base, hero, i, lines, 1, true);
}

bool csResistTipLineFor(uintptr_t base, uintptr_t hero, int i, char* out, int outsz) {
    if (outsz > 0) out[0] = 0;
    if (!hero || i < 0 || i >= CS_RESIST_COUNT || outsz <= 0) return false;

    char row[AB_TIP_LINE_SZ];
    if (!csResistRowFor(base, hero, i, row, sizeof row)) return false;

    float net = 0.0f;
    char raw[64];
    if (csResistRawId(base, i, raw, sizeof raw)) {
        float sums[CS_TIP_SRC_COUNT]; int counts[CS_TIP_SRC_COUNT];
        csTipSumBySource(base, hero, STAT_TYPE_RESISTANCE, raw, sums, counts);
        for (int s = 0; s < CS_TIP_SRC_COUNT; s++) net += sums[s];
    }
    if (net > CS_TIP_EPS || net < -CS_TIP_EPS)
        _snprintf(out, outsz, axs(net > 0.0f ? AXS_CS_STAT_BUFFED_FMT : AXS_CS_STAT_DEBUFFED_FMT),
                  row);
    else
        _snprintf(out, outsz, "%s", row);
    out[outsz - 1] = 0;
    size_t len = strlen(out);
    if (len && out[len - 1] != '.' && len + 1 < (size_t)outsz) { out[len] = '.'; out[len + 1] = 0; }

    char tip[AB_TIP_MAX_LINES][AB_TIP_LINE_SZ];
    for (int k = 0; k < AB_TIP_MAX_LINES; k++) tip[k][0] = 0;
    int n = csResistTipBody(base, hero, i, tip, 0, false);   // panel: modifiers only, no base line
    char list[AB_TIP_LINE_SZ];
    list[0] = 0;
    for (int k = 0; k < n && k < AB_TIP_MAX_LINES; k++) csAppendComma(list, sizeof list, tip[k]);
    if (list[0]) abAppendFrag(out, outsz, list);
    return out[0] != 0;
}

static bool spDurTypeName(uintptr_t base, int idx, char* out, int outsz) {
    if (idx < 0 || idx >= BUFF_TYPE_TABLE_MAX) return false;
    if (!safeReadCStr(base + BUFF_DURTYPE_TABLE_RVA + (uintptr_t)idx * BUFF_DURTYPE_STRIDE,
                      out, outsz) || !out[0]) return false;
    for (const char* p = out; *p; p++)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
              (*p >= '0' && *p <= '9') || *p == '_')) return false;
    return true;
}

bool spTraitName(uintptr_t base, uint32_t hash, bool virtue, char* out, int outsz) {
    if (!hash) return false;
    uint8_t alt = 0;
    safeReadU8(base + CAMP_BUFFREG_FLAG_RVA, &alt);
    uintptr_t reg = 0, begin = 0, end = 0;
    if (!safeReadPtr(base + TRAIT_REG_RVA, &reg) || reg <= 0x10000) return false;
    uintptr_t pair = reg + (alt ? TRAIT_REG_ALT_OFF : 0);
    if (!safeReadPtr(pair, &begin) || !safeReadPtr(pair + 8, &end)) return false;
    if (begin <= 0x10000 || end < begin) return false;
    uintptr_t count = (end - begin) / TRAIT_REC_STRIDE;
    if (count > (uintptr_t)TRAIT_REG_MAX) return false;
    for (uintptr_t i = 0; i < count; i++) {
        uintptr_t recAddr = begin + i * TRAIT_REC_STRIDE;
        uint32_t h = 0;
        if (!safeReadU32(recAddr + TRAIT_REC_HASH_OFF, &h) || h != hash) continue;
        char id[64];
        if (!safeReadCStr(recAddr, id, sizeof id) || !id[0]) return false;
        for (const char* p = id; *p; p++)
            if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
                  (*p >= '0' && *p <= '9') || *p == '_')) return false;
        char key[96];
        _snprintf(key, sizeof key, virtue ? "str_virtue_name_%s" : "str_affliction_name_%s", id);
        key[sizeof key - 1] = 0;
        if (!resolveKey(base, key, out, outsz) || !out[0]) return false;
        abStripMarkup(out);
        return out[0] != 0;
    }
    return false;
}

// ---- THE HERO'S CURRENT MODE (transform / stance) ----
bool spModeName(uintptr_t base, uintptr_t actor, char* out, int outsz) {
    if (out && outsz > 0) out[0] = 0;
    if (!actor || !out || outsz <= 0) return false;

    uint32_t modeId = 0;
    if (!safeReadU32(actor + ACTOR_MODE_ID_OFF, &modeId) || modeId == 0)
        return false;

    uintptr_t cls = 0;
    if (!safeReadPtr(actor + ACTOR_HEROCLASS_OFF, &cls) || cls <= 0x10000) return false;
    uintptr_t begin = 0, end = 0;
    if (!safeReadPtr(cls + HEROCLASS_MODEVEC_OFF, &begin) ||
        !safeReadPtr(cls + HEROCLASS_MODEVEC_END_OFF, &end)) return false;
    if (begin <= 0x10000 || end < begin) return false;
    uintptr_t count = (end - begin) / MODE_REC_STRIDE;
    if (count == 0 || count > (uintptr_t)MODE_TABLE_MAX) {
        logLine("mode: class=%p declares %llu modes — out of range, not naming mode 0x%08x",
                (void*)cls, (unsigned long long)count, modeId);
        return false;
    }

    for (uintptr_t i = 0; i < count; i++) {
        char id[64];
        if (!safeReadCStr(begin + i * MODE_REC_STRIDE, id, sizeof id) || !id[0]) continue;
        bool clean = true;
        for (const char* p = id; *p; p++)
            if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
                  (*p >= '0' && *p <= '9') || *p == '_')) { clean = false; break; }
        if (!clean || resHash(id) != modeId) continue;

        char key[96];
        _snprintf(key, sizeof key, "actor_mode_name_%s", id);
        key[sizeof key - 1] = 0;
        if (!resolveKey(base, key, out, outsz) || !out[0]) {
            logLine("mode: id \"%s\" matched 0x%08x but \"%s\" did not resolve", id, modeId, key);
            out[0] = 0;
            return false;
        }
        abStripMarkup(out);
        return out[0] != 0;
    }

    static uintptr_t s_lastCls = 0; static uint32_t s_lastMode = 0;
    if (cls != s_lastCls || modeId != s_lastMode) {
        s_lastCls = cls; s_lastMode = modeId;
        logLine("mode: class=%p mode 0x%08x not found in %llu record(s):",
                (void*)cls, modeId, (unsigned long long)count);
        for (uintptr_t i = 0; i < count; i++) {
            char id[64];
            if (!safeReadCStr(begin + i * MODE_REC_STRIDE, id, sizeof id)) { id[0] = 0; }
            logLine("  mode[%llu] \"%s\" -> 0x%08x", (unsigned long long)i, id,
                    id[0] ? resHash(id) : 0u);
        }
    }
    return false;
}

bool spDurTitleLine(uintptr_t base, uintptr_t actor, int statType, const char* keyFmt,
                    char* out, int outsz) {
                                              //  its stealth line from this same shape)
    uintptr_t begin = 0;
    int n = spBuffCount(actor, &begin), hits = 0, permanent = 0, maxR = 0;
    for (int i = 0; i < n; i++) {
        uintptr_t rec = begin + (uintptr_t)i * ACTOR_BUFF_STRIDE + ACTOR_BUFF_RECORD_OFF;
        uint32_t st = 0, dt = 0; int32_t r = 0;
        if (!safeReadU32(rec + BUFFREC_STAT_TYPE_OFF, &st) || (int)st != statType) continue;
        hits++;
        if (safeReadU32(rec + BUFFREC_DURTYPE_OFF, &dt) && dt == 0) permanent = 1;
        if (safeReadU32(rec + BUFFREC_ROUNDS_OFF, (uint32_t*)&r) && r > maxR) maxR = r;
    }
    if (hits <= 0) return false;

    char dn[80];
    if (!spDurTypeName(base, permanent ? 0 : 9, dn, sizeof dn)) {
        logLine("status: actor=%p HAS stat_type %d but the duration-type name did not read — "
                "\"%s\" cannot be built, so the condition goes unspoken", (void*)actor, statType, keyFmt);
        return false;
    }
    char key[128];
    _snprintf(key, sizeof key, keyFmt, dn);
    key[sizeof key - 1] = 0;
    if (abTipInt(base, key, maxR, out, outsz)) return true;
    logLine("status: actor=%p HAS stat_type %d but \"%s\" did not resolve", (void*)actor, statType, key);
    return false;
}

// ---- The condition WALK and its two output shapes ----
struct SpCondSink {
    char* out; int outsz;                 // packed mode when lines == NULL
    char (*lines)[AB_TIP_LINE_SZ];        // line mode
    int n, maxLines;
};
static void spCondEmit(SpCondSink* s, const char* frag) {
    if (!frag[0]) return;
    if (!s->lines) { abAppend(s->out, s->outsz, frag); return; }
    if (s->n >= s->maxLines) return;
    _snprintf(s->lines[s->n], AB_TIP_LINE_SZ, "%s", frag);
    s->lines[s->n][AB_TIP_LINE_SZ - 1] = 0;
    size_t len = strlen(s->lines[s->n]);
    if (len && s->lines[s->n][len - 1] != '.' && len + 1 < (size_t)AB_TIP_LINE_SZ) {
        s->lines[s->n][len] = '.'; s->lines[s->n][len + 1] = 0;
    }
    s->n++;
}

static void spCondWalk(uintptr_t base, uintptr_t actor, int want, SpCondSink* s) {
    char frag[256];
    float f = 0; int rounds = 0;

    if ((want & SP_COND_HEALTH) &&
        abReadF32(actor + ACTOR_CUR_HP_OFF, &f) && f <= 0.0f &&
        abTipPlain(base, "tray_icon_tooltip_deathsdoor_title", frag, sizeof frag))
        spCondEmit(s, frag);

    if ((want & SP_COND_NEG) && abReadF32(actor + ACTOR_STUN_OFF, &f) && f > 0.0f) {
        if (!abTipPlain(base, "str_ui_stunned", frag, sizeof frag)) {
            logLine("status: \"str_ui_stunned\" did not resolve — falling back to resistance_name_stun");
            if (!abTipPlain(base, "resistance_name_stun", frag, sizeof frag)) frag[0] = 0;
        }
        if (frag[0]) spCondEmit(s, frag);
    }

    if ((want & SP_COND_NEG) &&
        abReadF32(actor + ACTOR_DAZE_OFF, &f) && f > 0.0f &&
        abTipInt(base, "tray_icon_tooltip_daze_format", (int)f, frag, sizeof frag))
        spCondEmit(s, frag);

    uint8_t b = 0;
    if ((want & SP_COND_NEG) &&
        safeReadU8(actor + ACTOR_IMMOB_OFF, &b) && b &&
        abTipPlain(base, "tray_icon_tooltip_immobilized", frag, sizeof frag))
        spCondEmit(s, frag);

    struct { int stat; int bucket; const char* key; const char* nameKey; } dots[] = {
        { STAT_TYPE_BLEED_DOT,   SP_COND_BLEED,  "tray_icon_tooltip_bleed_format",       "resistance_name_bleed"    },
        { STAT_TYPE_POISON_DOT,  SP_COND_BLIGHT, "tray_icon_tooltip_poison_format",      "resistance_name_poison"   },
        { STAT_TYPE_STRESS_DOT,  SP_COND_HORROR, "tray_icon_tooltip_stress_dot_format",  "str_ui_stress_dot"        },
        { STAT_TYPE_SHUFFLE_DOT, SP_COND_NEG,    "tray_icon_tooltip_shuffle_dot_format", "str_ui_shuffle_dot"       },
        { STAT_TYPE_HEAL_DOT,    SP_COND_HEALTH, "tray_icon_tooltip_hp_heal_dot_format", "str_ui_hp_heal_dot_onset" },
    };
    for (int i = 0; i < (int)(sizeof dots / sizeof dots[0]); i++) {
        if (!(want & dots[i].bucket)) continue;
        float sum = 0;
        if (spStatQuery(actor, dots[i].stat, &sum, &rounds) <= 0) continue;
        if (!abTipInt2(base, dots[i].key, (int)sum, rounds, frag, sizeof frag)) continue;
        char name[96];
        if (abTipPlain(base, dots[i].nameKey, name, sizeof name) && name[0]) {
            char labelled[256];
            _snprintf(labelled, sizeof labelled, "%s: %s", name, frag);
            labelled[sizeof labelled - 1] = 0;
            spCondEmit(s, labelled);
        } else {
            logLine("status: DOT stat=%d name key \"%s\" did not resolve, format -> \"%s\"",
                    dots[i].stat, dots[i].nameKey, frag);
            spCondEmit(s, frag);
        }
    }

    if (want & SP_COND_HEALTH) {
        float burn = 0;
        if (spStatQuery(actor, STAT_TYPE_BURN_DOT, &burn, NULL) > 0) {
            if (abTipInt(base, "tray_icon_tooltip_burn_format", (int)burn, frag, sizeof frag))
                spCondEmit(s, frag);
            else
                logLine("status: burning for %d but \"tray_icon_tooltip_burn_format\" did not "
                        "resolve — the burn goes unsaid", (int)burn);
        }
    }

    if ((want & SP_COND_NEG) && spControlledBurnInto(base, actor, frag, sizeof frag))
        spCondEmit(s, frag);

    if ((want & SP_COND_NEG) &&
        spDurTitleLine(base, actor, STAT_TYPE_TAG, "tray_icon_tooltip_tag_title_%s", frag, sizeof frag))
        spCondEmit(s, frag);

    if ((want & SP_COND_POS) &&
        spDurTitleLine(base, actor, STAT_TYPE_STEALTH, "tray_icon_tooltip_stealth_title_%s",
                       frag, sizeof frag))
        spCondEmit(s, frag);

    if ((want & SP_COND_POS) && spStatQuery(actor, STAT_TYPE_RIPOSTE, NULL, &rounds) > 0) {
        bool got = (rounds < 1)
            ? abTipPlain(base, "tray_icon_tooltip_riposte_no_duration", frag, sizeof frag)
            : abTipInt(base, "tray_icon_tooltip_riposte", rounds, frag, sizeof frag);
        if (got) spCondEmit(s, frag);
    }

    if (want & SP_COND_POS) {
        uint32_t blocks = 0;
        if (safeReadU32(actor + ACTOR_HEALTH_BLOCKS_OFF, &blocks) && blocks > 0 &&
            blocks <= ACTOR_HEALTH_BLOCKS_MAX &&
            abTipInt(base, "tray_icon_tooltip_health_damage_blocks_format", (int)blocks,
                     frag, sizeof frag)) {
            char name[96];
            if (abTipPlain(base, "str_ui_health_damage_block_onset", name, sizeof name) && name[0]) {
                char labelled[256];
                _snprintf(labelled, sizeof labelled, "%s: %s", name, frag);
                labelled[sizeof labelled - 1] = 0;
                spCondEmit(s, labelled);
            } else {
                logLine("status: \"str_ui_health_damage_block_onset\" did not resolve — "
                        "the block count goes out unnamed");
                spCondEmit(s, frag);
            }
        }
    }

    if (want & SP_COND_POS) {
        uintptr_t begin = 0;
        int n = spBuffCount(actor, &begin);
        for (int i = 0; i < n; i++) {
            uintptr_t rec = begin + (uintptr_t)i * ACTOR_BUFF_STRIDE + ACTOR_BUFF_RECORD_OFF;
            uint32_t st = 0, gid = 0, own = 0;
            if (!safeReadU32(rec + BUFFREC_STAT_TYPE_OFF, &st) || (int)st != STAT_TYPE_GUARDED) continue;
            safeReadU32(rec + BUFFREC_GUARD_GUID_OFF, &gid);
            safeReadU32(actor + ACTOR_ID_OFF, &own);
            logLine("status: guard relation on actor=%p own_id=%u guarded_guid=%u", (void*)actor, own, gid);
            if (!abTipPlain(base, "tray_icon_tooltip_guard_title", frag, sizeof frag)) break;
            uintptr_t party[RV_MAX_MEMBERS];
            int np = rvPartyList(base, party, RV_MAX_MEMBERS);
            for (int k = 0; k < np; k++) {
                uint32_t pid = 0;
                if (party[k] != actor && safeReadU32(party[k] + ACTOR_ID_OFF, &pid) && pid == gid) {
                    char pname[80], pcls[80];
                    if (abHeroNameClassOf(base, party[k], pname, sizeof pname, pcls, sizeof pcls) && pname[0]) {
                        size_t len = strlen(frag);
                        _snprintf(frag + len, sizeof frag - len, ": %s", pname);
                        frag[sizeof frag - 1] = 0;
                    }
                    break;
                }
            }
            spCondEmit(s, frag);
            break;
        }
    }

    uint32_t hash = 0;
    if ((want & SP_COND_TRAIT) &&
        safeReadU32(actor + ACTOR_TRAIT_OFF, &hash) && hash &&
        spTraitName(base, hash, false, frag, sizeof frag))
        spCondEmit(s, frag);
    if ((want & SP_COND_TRAIT) &&
        safeReadU32(actor + ACTOR_VIRTUE_OFF, &hash) && hash &&
        spTraitName(base, hash, true, frag, sizeof frag))
        spCondEmit(s, frag);
}

bool spConditionsInto(uintptr_t base, uintptr_t actor, int want, char* out, int outsz) {
    out[0] = 0;
    if (!actor || !want) return false;
    SpCondSink s = { out, outsz, NULL, 0, 0 };
    spCondWalk(base, actor, want, &s);
    if (out[0]) {
        size_t len = strlen(out);
        if (len + 1 < (size_t)outsz) { out[len] = '.'; out[len + 1] = 0; }
        logLine("status: conditions actor=%p want=%d -> \"%s\"", (void*)actor, want, out);
    }
    return out[0] != 0;
}

int spConditionLines(uintptr_t base, uintptr_t actor, int want,
                     char lines[][AB_TIP_LINE_SZ], int n, int maxLines) {
    if (!actor || !want || n >= maxLines) return n;
    SpCondSink s = { NULL, 0, lines, n, maxLines };
    spCondWalk(base, actor, want, &s);
    if (s.n > n)
        logLine("status: condition lines actor=%p want=%d added=%d", (void*)actor, want, s.n - n);
    return s.n;
}

static bool spConditionsLine(uintptr_t base, uintptr_t actor, char* out, int outsz) {
    return spConditionsInto(base, actor, SP_COND_ALL, out, outsz);
}

void spDumpStatusOnce(uintptr_t base, uintptr_t actor) {
    static bool done = false;
    if (done || !actor) return;
    uintptr_t begin = 0;
    int n = spBuffCount(actor, &begin);
    if (n <= 0) return;                       // wait for an actor that has something to show
    done = true;

    logLine("status DUMP actor=%p instances=%d", (void*)actor, n);
    for (int i = 0; i < n; i++) {
        uintptr_t rec = begin + (uintptr_t)i * ACTOR_BUFF_STRIDE + ACTOR_BUFF_RECORD_OFF;
        uint32_t st = 0, src = 0, sd = 0, dt = 0, gg = 0, sg = 0, scl = 0; int32_t r = 0; float amt = 0;
        char sub[64] = "";
        safeReadU32(rec + BUFFREC_STAT_TYPE_OFF, &st);
        safeReadU32(rec + BUFFREC_SOURCE_OFF, &src);
        safeReadU32(rec + BUFFREC_SOURCEDATA_OFF, &sd);
        safeReadU32(rec + BUFFREC_DURTYPE_OFF, &dt);
        safeReadU32(rec + BUFFREC_ROUNDS_OFF, (uint32_t*)&r);
        safeReadU32(rec + BUFFREC_GUARD_GUID_OFF, &gg);
        safeReadU32(rec + BUFFREC_SRCGUID_OFF, &sg);
        safeReadU32(rec + BUFFREC_SRCCLASS_OFF, &scl);
        abReadF32(rec + BUFFREC_AMOUNT_OFF, &amt);
        safeReadCStr(rec + BUFFREC_STATSUB_OFF, sub, sizeof sub);
        if (!abPlausibleName(sub)) sub[0] = 0;
        char srcName[0x44] = "";
        if (src < BUFF_TYPE_TABLE_MAX)
            safeReadCStr(base + BUFF_SRCTYPE_TABLE_RVA + (uintptr_t)src * BUFF_SRCTYPE_STRIDE,
                         srcName, sizeof srcName);
        if (!abPlausibleName(srcName)) srcName[0] = 0;
        logLine("status DUMP [%d] stat=%u sub=\"%s\" amount=%.4f rounds=%d durtype=%u src=%u(%s) "
                "srcdata=0x%08x srcguid=0x%08x srcclass=0x%08x guard=%u",
                i, st, sub, (double)amt, r, dt, src, srcName, sd, sg, scl, gg);
    }
    int nq = spQuirkCountFor(actor);
    for (int i = 0; i < nq; i++) {
        uintptr_t q = spQuirkAtFor(actor, i);
        uint32_t h = 0; char id[64] = "?";
        if (q) { safeReadU32(q + QUIRK_HASH_OFF, &h); csQuirkId(q, id, sizeof id); }
        logLine("status DUMP quirk[%d] id=\"%s\" hash=0x%08x", i, id, h);
        uintptr_t qb = 0, qe = 0;
        if (q && safeReadPtr(q + QUIRK_BUFF_BEGIN_OFF, &qb) && safeReadPtr(q + QUIRK_BUFF_END_OFF, &qe) &&
            qb && qe >= qb && ((qe - qb) % QUIRK_BUFF_STRIDE) == 0) {
            uintptr_t cnt = (qe - qb) / QUIRK_BUFF_STRIDE;
            if (cnt > (uintptr_t)QUIRK_BUFF_MAX) cnt = QUIRK_BUFF_MAX;
            for (uintptr_t j = 0; j < cnt; j++) {
                uintptr_t e = qb + j * QUIRK_BUFF_STRIDE;
                uint32_t est = 0; float eam = 0; char esub[64] = "";
                safeReadU32(e + BUFFREC_STAT_TYPE_OFF, &est);
                abReadF32(e + BUFFREC_AMOUNT_OFF, &eam);
                safeReadCStr(e + BUFFREC_STATSUB_OFF, esub, sizeof esub);
                if (!abPlausibleName(esub)) esub[0] = 0;
                logLine("status DUMP   quirk[%d].buff[%llu] stat=%u sub=\"%s\" amount=%.4f",
                        i, (unsigned long long)j, est, esub, (double)eam);
            }
        }
    }
}

int spAppendStatusLines(uintptr_t base, uintptr_t actor,
                               char lines[][AB_TIP_LINE_SZ], int n, int maxLines) {
    spDumpStatusOnce(base, actor);

    char cond[AB_TIP_LINE_SZ];
    if (spConditionsLine(base, actor, cond, sizeof cond) && n < maxLines) {
        _snprintf(lines[n], AB_TIP_LINE_SZ, "%s", cond);
        lines[n][AB_TIP_LINE_SZ - 1] = 0;
        n++;
    }

    uintptr_t begin = 0;
    int count = spBuffCount(actor, &begin);
    if (count < 0) { logLine("status: actor=%p has no readable buff vector", (void*)actor); return n; }
    if (count == 0) return n;

    char buf[AB_TIP_LINE_SZ];
    int spoken = 0;
    for (int i = 0; i < count && n < maxLines; i++) {
        if (!spBuffText(base, actor, i, buf, sizeof buf) || !buf[0]) continue;   // gated off, or unreadable
        _snprintf(lines[n], AB_TIP_LINE_SZ, "%s", buf);
        lines[n][AB_TIP_LINE_SZ - 1] = 0;
        n++; spoken++;
    }
    if (count > 0 && spoken == 0)
        logLine("status: actor=%p has %d buffs but none describable", (void*)actor, count);
    return n;
}

// ---- THE UNVOICED-EFFECT LINES — the hero tooltip's leftovers ----
static bool spBuffIsQuirkSourced(uintptr_t base, const unsigned char* rec) {
    char src[0x44];
    if (!spBuffSourceName(base, rec, src, sizeof src)) return false;   // unreadable: speak it
    return strcmp(src, BSRC_QUIRK_NAME) == 0 || strcmp(src, "bsrc_disease") == 0;
}

static bool spDmgHighTwinInVector(uintptr_t begin, int count, int self,
                                  const unsigned char* rec) {
    char sub[0x41];
    memcpy(sub, rec + BUFFREC_STATSUB_OFF, sizeof sub - 1);
    sub[sizeof sub - 1] = 0;
    if (strcmp(sub, "damage_high") != 0) return false;
    uint32_t stat = *(uint32_t*)(rec + BUFFREC_STAT_TYPE_OFF);
    uint32_t src  = *(uint32_t*)(rec + BUFFREC_SOURCE_OFF);
    uint32_t amt  = *(uint32_t*)(rec + BUFFREC_AMOUNT_OFF);   // bit compare: same JSON, same bits
    int32_t  rnds = *(int32_t*)(rec + BUFFREC_ROUNDS_OFF);
    for (int j = 0; j < count; j++) {
        if (j == self) continue;
        uintptr_t o = begin + (uintptr_t)j * ACTOR_BUFF_STRIDE + ACTOR_BUFF_RECORD_OFF;
        uint32_t ost = 0, osrc = 0, oamt = 0; int32_t ornds = 0;
        char osub[0x41] = { 0 };
        if (!safeReadU32(o + BUFFREC_STAT_TYPE_OFF, &ost) || ost != stat) continue;
        if (!safeReadBlock(o + BUFFREC_STATSUB_OFF, osub, sizeof osub - 1)) continue;
        osub[sizeof osub - 1] = 0;
        if (strcmp(osub, "damage_low") != 0) continue;
        if (!safeReadU32(o + BUFFREC_AMOUNT_OFF, &oamt) || oamt != amt) continue;
        if (!safeReadU32(o + BUFFREC_SOURCE_OFF, &osrc) || osrc != src) continue;
        if (!safeReadU32(o + BUFFREC_ROUNDS_OFF, (uint32_t*)&ornds) || ornds != rnds) continue;
        return true;
    }
    return false;
}

static bool spResistSubShownForMonster(uintptr_t base, const char* sub) {
    for (int i = 0; i < CS_RESIST_COUNT; i++) {
        char raw[64];
        if (!csResistRawId(base, i, raw, sizeof raw)) continue;
        if (strcmp(raw, sub) == 0) return csResistVisibleFor(base, true, i);
    }
    return false;
}

static bool spRecCountedElsewhere(uintptr_t base, uintptr_t actor, uintptr_t live,
                                  const unsigned char* rec, bool monster,
                                  int healIdx, int sTakeIdx, int sHealIdx) {
    int32_t stat = *(int32_t*)(rec + BUFFREC_STAT_TYPE_OFF);
    bool pairShown = false;
    char sub[0x41];
    memcpy(sub, rec + BUFFREC_STATSUB_OFF, sizeof sub - 1);
    sub[sizeof sub - 1] = 0;
    if (monster) {
        if (stat == healIdx) {
            pairShown = true;                                  // rides the HP line's group
        } else if (stat == STAT_TYPE_RESISTANCE) {
            pairShown = spResistSubShownForMonster(base, sub); // only a drawn resistance row counts
        } else if (stat == STAT_TYPE_COMBAT_ADD) {
            pairShown = strcmp(sub, "protection_rating") == 0 ||
                        strcmp(sub, "defense_rating") == 0 ||
                        strcmp(sub, "speed_rating") == 0;      // the three visible stat rows
        }
    } else if (stat == STAT_TYPE_RESISTANCE ||
        stat == healIdx || stat == sTakeIdx || stat == sHealIdx) {
        pairShown = true;
    } else if (stat == STAT_TYPE_COMBAT_ADD || stat == STAT_TYPE_COMBAT_MULT) {
        const char* s = (strcmp(sub, "damage_high") == 0) ? "damage_low" : sub;
        if (stat == STAT_TYPE_COMBAT_MULT)
            pairShown = strcmp(s, "max_hp") == 0 || strcmp(s, "damage_low") == 0;
        else
            pairShown = strcmp(s, "attack_rating") == 0 || strcmp(s, "defense_rating") == 0 ||
                        strcmp(s, "crit_chance") == 0 || strcmp(s, "protection_rating") == 0 ||
                        strcmp(s, "damage_low") == 0 || strcmp(s, "speed_rating") == 0;
    }
    if (!pairShown) return false;
    uint32_t src = *(uint32_t*)(rec + BUFFREC_SOURCE_OFF);
    if (src >= (uint32_t)CS_TIP_SRC_COUNT) return false;   // the summer skips it — never spoken
    return csTipRuleActive(base, actor, live);             // counted only while the rule holds
}

int spUnvoicedBuffLines(uintptr_t base, uintptr_t actor, bool monster,
                        char lines[][AB_TIP_LINE_SZ], int n, int maxLines) {
    if (!actor || n >= maxLines) return n;
    uintptr_t begin = 0;
    int count = spBuffCount(actor, &begin);
    if (count < 0) {
        logLine("herotip: actor=%p has no readable buff vector", (void*)actor);
        return n;
    }
    if (count == 0) return n;

    int healIdx  = spStatTypeIndexByName(base, "hp_heal_received_percent");
    int sTakeIdx = spStatTypeIndexByName(base, "stress_dmg_received_percent");
    int sHealIdx = spStatTypeIndexByName(base, "stress_heal_received_percent");

    char buf[AB_TIP_LINE_SZ];
    for (int i = 0; i < count && n < maxLines; i++) {
        uintptr_t live = begin + (uintptr_t)i * ACTOR_BUFF_STRIDE + ACTOR_BUFF_RECORD_OFF;
        unsigned char rec[CAMP_BUFF_REC_SZ];
        if (!safeReadBlock(live, rec, CAMP_BUFF_REC_SZ)) {
            logLine("herotip: effect %d of %d at %llx unreadable", i, count,
                    (unsigned long long)live);
            continue;
        }
        uint32_t stat = *(uint32_t*)(rec + BUFFREC_STAT_TYPE_OFF);
        if (spCondHasOwnVoice(stat)) continue;              // a condition line already says it
        if (spBuffIsQuirkSourced(base, rec)) continue;      // the sheet's rows, not the tooltip's
        if (spRecCountedElsewhere(base, actor, live, rec, monster, healIdx, sTakeIdx, sHealIdx)) continue;
        if (spDmgHighTwinInVector(begin, count, i, rec)) continue;
        if (!spBuffTextFromRecord(base, actor, rec, buf, sizeof buf) || !buf[0]) continue;
        _snprintf(lines[n], AB_TIP_LINE_SZ, "%s", buf);
        lines[n][AB_TIP_LINE_SZ - 1] = 0;
        n++;
    }
    return n;
}

static bool csCampTypeIs(uintptr_t base, int32_t type, const uintptr_t* rvas, int n) {
    for (int i = 0; i < n; i++) {
        uint32_t v = 0;
        if (safeReadU32(base + rvas[i], &v) && (int32_t)v == type) return true;
    }
    return false;
}

static bool csCampEffKey(uintptr_t base, uintptr_t effect, int32_t type, char* out, int outsz) {
    char typeId[64];
    if (!safeReadCStr(effect + CAMP_EFF_TYPE_OFF, typeId, sizeof typeId) || !typeId[0]) return false;

    char subId[64] = { 0 };
    uint32_t suffixType = 0;
    if (safeReadU32(base + CAMP_ET_SUFFIX_RVA, &suffixType) && (int32_t)suffixType == type)
        safeReadCStr(effect + CAMP_EFF_SUBTYPE_OFF, subId, sizeof subId);

    if (subId[0]) _snprintf(out, outsz, "camping_skill_effect_%s_%s", typeId, subId);
    else          _snprintf(out, outsz, "camping_skill_effect_%s", typeId);
    out[outsz - 1] = 0;
    return true;
}

static bool csCampEffText(uintptr_t base, uintptr_t effect, char* out, int outsz) {
    out[0] = 0;
    int32_t type = 0;
    if (!safeReadU32(effect + CAMP_EFF_TYPEENUM_OFF, (uint32_t*)&type)) return false;

    if (csCampTypeIs(base, type, &CAMP_ET_BUFF_RVA, 1)) {
        char d[CAMP_BUFF_DESC_BUF];
        if (!csCampBuffDesc(base, effect, d, sizeof d)) return false;
        _snprintf(out, outsz, "%s", d);
        out[outsz - 1] = 0;
        return out[0] != 0;
    }

    char key[160];
    if (!csCampEffKey(base, effect, type, key, sizeof key)) return false;

    if (csCampTypeIs(base, type, CAMP_ET_PLAIN_RVA,
                     (int)(sizeof CAMP_ET_PLAIN_RVA / sizeof CAMP_ET_PLAIN_RVA[0])))
        return abTipPlain(base, key, out, outsz);

    bool percent = csCampTypeIs(base, type, CAMP_ET_PCT_RVA, 2);
    if (percent || csCampTypeIs(base, type, CAMP_ET_RAW_RVA, 3)) {
        float amount = 0.0f, mult = 100.0f;
        if (!abReadF32(effect + CAMP_EFF_AMOUNT_OFF, &amount)) return false;
        abReadF32(base + SKILL_PCT_MULT_RVA, &mult);
        return abTipInt(base, key, (int)(percent ? amount * mult : amount), out, outsz);
    }

    if (csCampTypeIs(base, type, &CAMP_ET_ITEM_RVA, 1)) {
        logLine("charsheet campeff: \"%s\" is the item type — not built, omitted", key);
        return false;
    }
    logLine("charsheet campeff: \"%s\" has no display type for enum %d — omitted", key, type);
    return false;
}

static bool csCampEffRequirements(uintptr_t base, uintptr_t effect, char* out, int outsz) {
    out[0] = 0;
    uintptr_t beg = 0, end = 0;
    if (!safeReadPtr(effect + CAMP_EFF_REQ_BEG_OFF, &beg) ||
        !safeReadPtr(effect + CAMP_EFF_REQ_END_OFF, &end)) return false;
    if (beg <= 0x10000 || end <= beg) return false;
    uintptr_t n = (end - beg) / CAMP_EFF_REQ_STRIDE;
    if (n > 16) { logLine("charsheet campeff: %u requirements — refusing", (unsigned)n); return false; }

    for (uintptr_t k = 0; k < n; k++) {
        char id[80];
        if (!safeReadCStr(beg + k * CAMP_EFF_REQ_STRIDE, id, sizeof id) || !id[0]) continue;
        char key[160], text[256];
        _snprintf(key, sizeof key, "camping_skill_requirement_%s", id);
        key[sizeof key - 1] = 0;
        if (!abTipPlain(base, key, text, sizeof text)) {
            logLine("charsheet campeff: \"%s\" did not resolve", key);
            continue;
        }
        if (out[0]) strncat(out, ", ", (size_t)(outsz - (int)strlen(out) - 1));
        strncat(out, text, (size_t)(outsz - (int)strlen(out) - 1));
    }
    return out[0] != 0;
}

static bool csCampEffLine(uintptr_t base, uintptr_t effect, char* out, int outsz) {
    char text[AB_TIP_LINE_SZ];
    if (!csCampEffText(base, effect, text, sizeof text)) return false;

    char reqs[256];
    if (csCampEffRequirements(base, effect, reqs, sizeof reqs)) {
        char wrapped[AB_TIP_LINE_SZ];
        if (!abTipStrN(base, "camping_skill_requirement_effect_format", reqs, text, nullptr,
                       wrapped, sizeof wrapped))
            abEffGlueFallback(wrapped, sizeof wrapped, reqs, text, nullptr);
        _snprintf(text, sizeof text, "%s", wrapped);
        text[sizeof text - 1] = 0;
    }

    float chance = 0.0f, one = 1.0f, mult = 100.0f;
    abReadF32(effect + CAMP_EFF_CHANCE_OFF, &chance);
    abReadF32(base + EFF_ONE_RVA, &one);
    abReadF32(base + SKILL_PCT_MULT_RVA, &mult);
    if (chance < one) {
        char wrapped[AB_TIP_LINE_SZ];
        if (abTipIntStr(base, "camping_skill_chance_effect_format", (int)(chance * mult), text,
                        wrapped, sizeof wrapped))
            _snprintf(text, sizeof text, "%s", wrapped);
        text[sizeof text - 1] = 0;
    }

    _snprintf(out, outsz, "%s", text);
    out[outsz - 1] = 0;
    return out[0] != 0;
}

int csCampEffectLines(uintptr_t base, uintptr_t cls,
                      char lines[AB_TIP_MAX_LINES][AB_TIP_LINE_SZ], int at) {
    int line = at;
    uintptr_t beg = 0, end = 0;
    if (!safeReadPtr(cls + CAMP_SKILL_EFFVEC_OFF, &beg) ||
        !safeReadPtr(cls + CAMP_SKILL_EFFVEC_END_OFF, &end)) {
        logLine("charsheet campeff: skill %p effect vector unreadable", (void*)cls);
        return 0;
    }
    if (beg <= 0x10000 || end < beg) {
        logLine("charsheet campeff: skill %p effect bounds bad (beg=%p end=%p)", (void*)cls, (void*)beg, (void*)end);
        return 0;
    }
    int n = (int)((end - beg) / CAMP_EFF_STRIDE);
    if (n < 0 || n > 32) {
        logLine("charsheet campeff: skill %p claims %d effects — refusing", (void*)cls, n);
        return 0;
    }
    logLine("charsheet campeff: skill %p has %d effects", (void*)cls, n);

    uint32_t groups[32];
    int ngroups = 0;
    for (int e = 0; e < n; e++) {
        uint32_t g = 0;
        if (!safeReadU32(beg + (uintptr_t)e * CAMP_EFF_STRIDE + CAMP_EFF_SELGROUP_OFF, &g)) continue;
        int at = 0;
        while (at < ngroups && groups[at] < g) at++;
        if (at < ngroups && groups[at] == g) continue;
        for (int m = ngroups; m > at; m--) groups[m] = groups[m - 1];
        groups[at] = g;
        ngroups++;
    }

    for (int gi = 0; gi < ngroups; gi++) {
        char buf[AB_TIP_LINE_SZ];
        buf[0] = 0;
        bool headed = false;
        for (int e = 0; e < n; e++) {
            uintptr_t eff = beg + (uintptr_t)e * CAMP_EFF_STRIDE;
            uint32_t g = 0;
            if (!safeReadU32(eff + CAMP_EFF_SELGROUP_OFF, &g) || g != groups[gi]) continue;

            int32_t type = 0;
            if (safeReadU32(eff + CAMP_EFF_TYPEENUM_OFF, (uint32_t*)&type) &&
                csCampTypeIs(base, type, &CAMP_ET_SKIP_RVA, 1)) continue;

            if (!headed) {                          // the group title, from the group's first effect
                char selId[64], key[160], title[192];
                if (safeReadCStr(eff + CAMP_EFF_SELECTION_OFF, selId, sizeof selId) && selId[0]) {
                    _snprintf(key, sizeof key, "camping_skill_selection_%s", selId);
                    key[sizeof key - 1] = 0;
                    if (abTipPlain(base, key, title, sizeof title)) {
                        csTrimLabel(title);         // the titles ship with a trailing colon
                        abAppend(buf, sizeof buf, title);
                    } else {
                        logLine("charsheet campeff: \"%s\" did not resolve", key);
                    }
                }
                headed = true;                      // one attempt per group, resolved or not
            }

            char text[AB_TIP_LINE_SZ];
            if (csCampEffLine(base, eff, text, sizeof text)) abAppend(buf, sizeof buf, text);
            else logLine("charsheet campeff: skill %p effect %d produced nothing", (void*)cls, e);
        }
        if (!buf[0]) continue;
        if (line < AB_TIP_MAX_LINES) {              // a group of its own
            _snprintf(lines[line], AB_TIP_LINE_SZ, "%s.", buf);
            lines[line][AB_TIP_LINE_SZ - 1] = 0;
            line++;
        } else {                                    // out of lines: fold in rather than drop
            abAppend(lines[AB_TIP_MAX_LINES - 1], AB_TIP_LINE_SZ, buf);
        }
    }
    for (int k = at; k < line; k++) logLine("charsheet campeff: tipline %d -> \"%s\"", k, lines[k]);
    return line - at;
}

// ---- the USES REMAINING line (the tooltip's FOURTH and last block) ----
int csCampUsesLine(uintptr_t base, uintptr_t cls, uintptr_t hero,
                   char lines[AB_TIP_MAX_LINES][AB_TIP_LINE_SZ], int at) {
    uint32_t limit = 0;
    if (!cls || !safeReadU32(cls + CAMP_SKILL_USE_LIMIT_OFF, &limit)) return 0;
    if (limit == 0 || limit > 99) {
        logLine("charsheet campeff: skill %p use_limit %u is not credible -- no uses line",
                (void*)cls, limit);
        return 0;
    }
    if (limit <= 1) return 0;
    uint32_t used = campSkillUsedCount(base, hero, cls);
    int remaining = (int)limit - (int)used;
    if (remaining < 0) remaining = 0;
    char text[AB_TIP_LINE_SZ];
    if (!abTipInt(base, "camping_skill_uses_remaining_format", remaining, text, sizeof text)) {
        logLine("charsheet campeff: camping_skill_uses_remaining_format did not resolve");
        return 0;
    }
    if (at < AB_TIP_MAX_LINES) {             // a line of its own, last, as the game draws it
        _snprintf(lines[at], AB_TIP_LINE_SZ, "%s.", text);
        lines[at][AB_TIP_LINE_SZ - 1] = 0;
        logLine("charsheet campeff: tipline %d -> \"%s\"", at, lines[at]);
        return 1;
    }
    abAppend(lines[AB_TIP_MAX_LINES - 1], AB_TIP_LINE_SZ, text);   // out of lines: fold in rather than drop
    return 0;
}

int csCampSkillTipLines(uintptr_t base, int i, char lines[AB_TIP_MAX_LINES][AB_TIP_LINE_SZ]) {
    for (int k = 0; k < AB_TIP_MAX_LINES; k++) lines[k][0] = 0;
    uintptr_t cls = csCampSkillClass(base, i);
    if (!cls) return 0;
    if (!csCampSkillRowText(base, i, lines[0], AB_TIP_LINE_SZ)) return 0;
    int n = 1 + csCampEffectLines(base, cls, lines, 1);
    n += csCampUsesLine(base, cls, abSelectedHero(base), lines, n);
    return n;
}

bool csCampSkillRowText(uintptr_t base, int i, char* out, int outsz) {
    uintptr_t cls = csCampSkillClass(base, i);
    if (!cls) {
        logLine("charsheet camskill: row %d did not resolve a skill class", i);
        return false;
    }
    char name[256];
    if (!csCampSkillName(base, cls, name, sizeof name))
        _snprintf(name, sizeof name, axs(AXS_CS_CAMPSKILL_N_FMT), i + 1);

    const char* state = !csCampSkillLearned(base, i) ? axs(AXS_CS_SKILL_NOT_LEARNED)
                      : csCampSkillSelected(base, i) ? axs(AXS_CS_SKILL_SELECTED)
                                                     : axs(AXS_CS_SKILL_LEARNED);
    int  cost = csCampSkillCost(cls);
    char costLine[128] = { 0 };
    if (cost > 0) _snprintf(costLine, sizeof costLine, axs(AXS_CS_CAMP_COST_FMT), cost);
    else          logLine("charsheet camskill: row %d cost unreadable — omitted", i);
    costLine[sizeof costLine - 1] = 0;
    _snprintf(out, outsz, "%s. %s.%s%s", name, state, costLine[0] ? " " : "", costLine);
    out[outsz - 1] = 0;
    return true;
}

void csDumpSheet(uintptr_t base, uintptr_t panel) {
    uintptr_t screen = 0, root = mapRoot(base), hero = abSelectedHero(base);
    safeReadPtr(base + RAID_SCREEN_RVA, &screen);

    uint32_t open = 0;
    safeReadU32(panel + CS_PANEL_OPEN_OFF, &open);

    logLine("charsheet DUMP panel=%p open=%u screen=%p root=%p same=%d hero=%p",
            (void*)panel, open, (void*)screen, (void*)root, screen == root, (void*)hero);

    if (hero) {
        uintptr_t cls = 0;
        uint8_t   isMonster = 1;
        safeReadPtr(hero + ACTOR_HEROCLASS_OFF, &cls);
        safeReadU8(hero + ACTOR_IS_MONSTER_OFF, &isMonster);
        char label[256];
        bool gotLabel = abHeroLabel(base, label, sizeof label);
        logLine("  charsheet hero: class=%p isMonster=%u label=\"%s\"",
                (void*)cls, isMonster, gotLabel ? label : "(unreadable)");

        for (int i = 0; i < CS_RESIST_COUNT; i++) {
            char raw[64] = {0};
            uintptr_t rec = base + CS_RESIST_TABLE_RVA + (uintptr_t)i * CS_RESIST_STRIDE;
            bool gotRaw = safeReadCStr(rec, raw, sizeof raw) && raw[0];
            float v = 0.0f;
            bool gotVal = csResistValue(base, hero, i, &v);
            char row[MAILBOX_SZ];
            bool gotRow = csResistRowText(base, i, row, sizeof row);
            logLine("  cs-resist[%d] raw=\"%s\" value=%s%.4f row=%s%s%s", i,
                    gotRaw ? raw : "(empty)", gotVal ? "" : "(faulted) ", v,
                    gotRow ? "\"" : "(unreadable)", gotRow ? row : "", gotRow ? "\"" : "");
        }
        for (int i = 0; i < kCsStatRowCount; i++) {
            char row[MAILBOX_SZ];
            bool gotRow = csStatRowText(base, i, row, sizeof row);
            logLine("  cs-stat[%d] key=\"%s\" row=%s%s%s", i, kCsStatRows[i].key,
                    gotRow ? "\"" : "(unreadable)", gotRow ? row : "", gotRow ? "\"" : "");
        }

        int nq = csQuirkCount(base);
        logLine("  cs-quirk COUNT=%d", nq);
        for (int i = 0; i < nq; i++) {
            uintptr_t quirk = csQuirkClassAt(base, i);
            char id[128] = {0};
            bool gotId = quirk && csQuirkId(quirk, id, sizeof id);
            const char* kind = quirk ? csQuirkKind(quirk) : nullptr;

            char row[MAILBOX_SZ];
            bool gotRow = csQuirkRowAt(base, i, false, false, row, sizeof row);
            logLine("  cs-quirk[%d] rec=%p id=\"%s\" kind=%s row=%s%s%s", i, (void*)quirk,
                    gotId ? id : "(unreadable)", kind ? kind : "(unreadable)",
                    gotRow ? "\"" : "(unreadable)", gotRow ? row : "", gotRow ? "\"" : "");
        }

        int ncs = csComSkillCount(base);
        {
            uintptr_t shero = abSelectedHero(base), sbeg = 0, send = 0;
            if (shero && safeReadPtr(shero + ACTOR_SLOTMAP_OFF, &sbeg) &&
                safeReadPtr(shero + ACTOR_SLOTMAP_END_OFF, &send) && sbeg && send >= sbeg)
                logLine("  cs-comskill COUNT=%d slotmap=[%p,%p) entries=%d", ncs,
                        (void*)sbeg, (void*)send, (int)((send - sbeg) / 4));
            else
                logLine("  cs-comskill COUNT=%d slotmap=(unreadable)", ncs);
        }
        for (int i = 0; i < ncs; i++) {
            uintptr_t skill = csComSkillAt(base, i);
            char id[64] = {0};
            bool gotId = skill && safeReadCStr(skill + SKILL_ID_OFF, id, sizeof id) && id[0];
            char row[MAILBOX_SZ];
            bool gotRow = csComSkillRowText(base, i, row, sizeof row);
            logLine("  cs-comskill[%d] rec=%p id=\"%s\" lvl=%d learned=%d equipped=%d row=%s%s%s", i,
                    (void*)skill, gotId ? id : "(unreadable)", csComSkillLevel(base, i),
                    csComSkillLearned(base, i) ? 1 : 0, csComSkillEquipped(base, i) ? 1 : 0,
                    gotRow ? "\"" : "(unreadable)", gotRow ? row : "", gotRow ? "\"" : "");
        }
        int cord[CS_COMSKILL_MAX];
        int ccnt = csComSortedOrder(base, cord);
        char ordbuf[128]; ordbuf[0] = 0;
        for (int c = 0; c < ccnt; c++) {
            char one[16]; _snprintf(one, sizeof one, "%s%d", c ? "," : "", cord[c]);
            strncat(ordbuf, one, sizeof ordbuf - strlen(ordbuf) - 1);
        }
        logLine("  cs-comskill display order (skill idx) = [%s]", ordbuf);

        int ncamp = csCampSkillCount(base);
        logLine("  cs-camskill COUNT=%d", ncamp);
        for (int i = 0; i < ncamp; i++) {
            uintptr_t cls = csCampSkillClass(base, i);
            uint32_t hash = 0; csCampSkillHash(base, i, &hash);
            char id[64] = {0};
            bool gotId = cls && csCampSkillId(cls, id, sizeof id);
            uint32_t a = 0, b = 0, c = 0;
            if (cls) {
                safeReadU32(cls + CAMP_SKILL_LEVEL_OFF,     &a);
                safeReadU32(cls + CAMP_SKILL_COST_OFF,      &b);
                safeReadU32(cls + CAMP_SKILL_USE_LIMIT_OFF, &c);
            }
            char row[MAILBOX_SZ];
            bool gotRow = csCampSkillRowText(base, i, row, sizeof row);
            logLine("  cs-camskill[%d] cls=%p hash=0x%08x id=\"%s\" learned=%d sel=%d "
                    "level(+0x4c)=%u cost(+0x50)=%u use_limit(+0x54)=%u row=%s%s%s",
                    i, (void*)cls, hash, gotId ? id : "(unreadable)",
                    csCampSkillLearnedRaw(base, i), csCampSkillSelected(base, i) ? 1 : 0,
                    a, b, c,
                    gotRow ? "\"" : "(unreadable)", gotRow ? row : "", gotRow ? "\"" : "");

            uintptr_t ebeg = 0, eend = 0;
            if (cls && safeReadPtr(cls + CAMP_SKILL_EFFVEC_OFF, &ebeg) &&
                       safeReadPtr(cls + CAMP_SKILL_EFFVEC_END_OFF, &eend) &&
                ebeg > 0x10000 && eend >= ebeg) {
                int ne = (int)((eend - ebeg) / CAMP_EFF_STRIDE);
                for (int e = 0; e < ne && e < 32; e++) {
                    uintptr_t eff = ebeg + (uintptr_t)e * CAMP_EFF_STRIDE;
                    char sel[64] = {0}, ty[64] = {0}, sub[64] = {0};
                    safeReadCStr(eff + CAMP_EFF_SELECTION_OFF, sel, sizeof sel);
                    safeReadCStr(eff + CAMP_EFF_TYPE_OFF,      ty,  sizeof ty);
                    safeReadCStr(eff + CAMP_EFF_SUBTYPE_OFF,   sub, sizeof sub);
                    uint32_t grp = 0, tenum = 0, bkey = 0;
                    float amt = 0.0f, chc = 0.0f;
                    safeReadU32(eff + CAMP_EFF_SELGROUP_OFF, &grp);
                    safeReadU32(eff + CAMP_EFF_TYPEENUM_OFF, &tenum);
                    safeReadU32(eff + CAMP_EFF_BUFFKEY_OFF,  &bkey);
                    abReadF32(eff + CAMP_EFF_AMOUNT_OFF, &amt);
                    abReadF32(eff + CAMP_EFF_CHANCE_OFF, &chc);
                    char text[AB_TIP_LINE_SZ];
                    bool gotText = csCampEffLine(base, eff, text, sizeof text);
                    logLine("    cs-campeff[%d.%d] sel=\"%s\"(grp=%u) type=\"%s\"(enum=%d) sub=\"%s\" "
                            "amount=%.4f chance=%.4f buffkey=0x%08x -> %s%s%s",
                            i, e, sel, grp, ty, (int)tenum, sub, amt, chc, bkey,
                            gotText ? "\"" : "(nothing)", gotText ? text : "", gotText ? "\"" : "");
                }
            }
        }

        for (int i = 0; i < EQUIP_SLOT_COUNT; i++) {
            uintptr_t rec = csEquipRecord(base, i);
            uint32_t slot = 0xffffffff, lvl = 0;
            char raw[128] = {0};
            if (rec) {
                safeReadU32(rec + EQUIP_SLOT_OFF, &slot);
                safeReadU32(rec + EQUIP_LEVEL_BYTE_OFF, &lvl);
                safeReadCStr(rec + EQUIP_NAME_OFF, raw, sizeof raw);
            }
            char row[MAILBOX_SZ];
            bool gotRow = csEquipRowText(base, i, row, sizeof row);
            logLine("  cs-equip[%d] rec=%p slot=%u lvlbyte=0x%02x raw=\"%s\" row=%s%s%s", i,
                    (void*)rec, slot, (unsigned)(lvl & 0xff), raw,
                    gotRow ? "\"" : "(unreadable)", gotRow ? row : "", gotRow ? "\"" : "");
        }

        uintptr_t hero = abSelectedHero(base);
        uintptr_t tbeg = 0; int tslots = 0;
        if (hero && invItemVectorAt(hero + HERO_TRINKET_SYSTEM_OFF, &tbeg, &tslots)) {
            logLine("  cs-trinket SYSTEM=%p SLOTS=%d", (void*)(hero + HERO_TRINKET_SYSTEM_OFF), tslots);
            for (int i = 0; i < tslots; i++) {
                uintptr_t item = tbeg + (uintptr_t)i * ITEM_STRIDE;
                uint32_t amount = 0;
                safeReadU32(item + ITEM_AMOUNT_OFF, &amount);
                char type[64] = {0}, id[64] = {0};
                safeReadCStr(item + ITEM_TYPE_OFF, type, sizeof type);
                safeReadCStr(item + ITEM_ID_OFF, id, sizeof id);
                logLine("  cs-trinket[%d] item=%p amount=%d type=\"%s\" id=\"%s\"",
                        i, (void*)item, (int)amount, type, id);
            }
        } else {
            logLine("  cs-trinket SYSTEM unreadable (hero=%p)", (void*)hero);
        }
    }

    // Every focus element on screen while the sheet is up. Deliberately unfiltered.
    uintptr_t begin = 0, end = 0;
    if (!safeReadPtr(base + VEC_BEGIN_RVA, &begin) || !safeReadPtr(base + VEC_END_RVA, &end) ||
        begin == 0 || end <= begin) {
        logLine("  charsheet: focus vector unreadable");
        return;
    }
    uintptr_t count = (end - begin) / ELEM_STRIDE;
    if (count > 4096) count = 4096;
    logLine("  charsheet: %llu focus elements", (unsigned long long)count);

    for (uintptr_t i = 0; i < count; i++) {
        uintptr_t elem = begin + i * ELEM_STRIDE;
        int64_t   id = 0;
        uintptr_t owner = 0;
        uint8_t   focusable = 0;
        safeReadU8(elem + ELEM_FOCUSABLE_OFF, &focusable);
        if (!safeReadI64(elem + ELEM_ID_OFF, &id) || isNoFocus(id)) continue;
        safeReadPtr(elem + ELEM_OWNER_OFF, &owner);

        uint32_t px = 0, py = 0, sw = 0, sh = 0;
        safeReadU32(elem + ELEM_POS_OFF,      &px); safeReadU32(elem + ELEM_POS_OFF + 4,  &py);
        safeReadU32(elem + ELEM_SIZE_OFF,     &sw); safeReadU32(elem + ELEM_SIZE_OFF + 4, &sh);
        float x, y, w, h;
        memcpy(&x, &px, 4); memcpy(&y, &py, 4); memcpy(&w, &sw, 4); memcpy(&h, &sh, 4);

        char ida[16], owa[16];
        idToAscii(id, ida); idToAscii((int64_t)owner, owa);

        char text[256] = {0}, via[32] = {0};
        bool gotText = g_tbwVtbl && owner > 0x10000 &&
                       extractWidgetText(owner, g_tbwVtbl, text, sizeof text, via, sizeof via);

        logLine("  cs[%03llu] id=0x%llx '%s' owner=0x%llx '%s' foc=%u pos=(%.0f,%.0f) size=(%.0fx%.0f) text=%s%s%s",
                (unsigned long long)i, (unsigned long long)id, ida,
                (unsigned long long)owner, owa, focusable, x, y, w, h,
                gotText ? "\"" : "(none)", gotText ? text : "", gotText ? "\"" : "");
        if (gotText) logLine("           via=%s", via);
    }
}
