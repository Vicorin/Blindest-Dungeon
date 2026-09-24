// raid/light.cpp -- THE LIGHT METER

#include <cstdint>
#include <cstring>
#include <cstdio>
#include "internal.h"

// ---- Light meter (torch) ----
static const uintptr_t TORCH_VALUE_OFF     = 0x5f14;
static const uintptr_t TORCH_RULES_OFF     = 0x5d08;
static const uintptr_t TORCH_RULES_HASTAB  = 0xc9;
static const uintptr_t TORCH_RULES_TABLE   = 0x80;      // that table, inside the rules record (unchanged)

static const uintptr_t RT_RANGE_BEGIN = 0x00;   // -> {float lo; float hi;}[], stride 8
static const uintptr_t RT_RANGE_END   = 0x08;
static const uintptr_t RT_DATA_BEGIN  = 0x18;   // -> bracket[], stride 0x30
static const uintptr_t RT_DATA_END    = 0x20;
static const uintptr_t RT_MODE        = 0x30;   // byte: 0 => lo <= t < hi, else lo < t <= hi
static const int       RT_DATA_STRIDE = 0x30;
static const long      RT_MAX_ENTRIES = 64;     // sanity bound: the meter has a handful of levels

// A bracket's 12 floats, named for the label each one drives.
static const uintptr_t BR_STRESS_A   = 0x00, BR_STRESS_B  = 0x04;
static const uintptr_t BR_MONSTER_A  = 0x08, BR_MONSTER_B = 0x0c;
static const uintptr_t BR_MON_CRIT   = 0x10;
static const uintptr_t BR_LOOT_A     = 0x14, BR_LOOT_B    = 0x18;
static const uintptr_t BR_PLR_CRIT   = 0x1c;
static const uintptr_t BR_PLR_DEF    = 0x20;
static const uintptr_t BR_PLR_SCOUT  = 0x24;
static const uintptr_t BR_MON_SURPR  = 0x28;
static const uintptr_t BR_HERO_SURPR = 0x2c;

struct LightEffects {
    int stress, monster, monCrit, heroSurpr, loot, plrCrit, plrDef, plrScout, monSurpr;
};

static bool torchValue(uintptr_t root, float* out) {
    return abReadF32(root + TORCH_VALUE_OFF, out);
}

static uintptr_t torchTable(uintptr_t base, uintptr_t root) {
    uintptr_t rules = 0;
    uint8_t over = 0;
    if (safeReadPtr(root + TORCH_RULES_OFF, &rules) && rules > 0x10000 &&
        safeReadU8(rules + TORCH_RULES_HASTAB, &over) && over)
        return rules + TORCH_RULES_TABLE;
    return base + TORCH_TABLE_RVA;
}

static int torchLevel(uintptr_t table, float t) {
    uintptr_t b = 0, e = 0;
    if (!safeReadPtr(table + RT_RANGE_BEGIN, &b) || !safeReadPtr(table + RT_RANGE_END, &e)) return -1;
    if (b <= 0x10000 || e < b) return -1;
    uint8_t mode = 0;
    safeReadU8(table + RT_MODE, &mode);
    long n = (long)((e - b) / 8);
    if (n <= 0 || n > RT_MAX_ENTRIES) return -1;
    for (long i = 0; i < n; i++) {
        float lo = 0, hi = 0;
        if (!abReadF32(b + i * 8, &lo) || !abReadF32(b + i * 8 + 4, &hi)) return -1;
        bool hit = mode ? (lo < t && t <= hi) : (lo <= t && t < hi);
        if (hit) return (int)i;
    }
    return -1;
}

static bool lightEffects(uintptr_t table, int index, LightEffects* fx) {
    memset(fx, 0, sizeof *fx);
    uintptr_t b = 0, e = 0;
    if (!safeReadPtr(table + RT_DATA_BEGIN, &b) || !safeReadPtr(table + RT_DATA_END, &e)) return false;
    if (b <= 0x10000 || e < b) return false;
    long n = (long)((e - b) / RT_DATA_STRIDE);
    if (n <= 0 || n > RT_MAX_ENTRIES) return false;
    if (index < 0 || index >= n) return false;

    // Pass 1 — every bracket, darkest first. Previous values start at zero.
    float pDef = 0, pScout = 0, pSurpr = 0;
    for (long k = 0; k < n; k++) {
        uintptr_t br = b + (n - k - 1) * RT_DATA_STRIDE;
        float def = 0, scout = 0, surpr = 0;
        abReadF32(br + BR_PLR_DEF,   &def);
        abReadF32(br + BR_PLR_SCOUT, &scout);
        abReadF32(br + BR_MON_SURPR, &surpr);
        if (pDef   < def)   fx->plrDef++;
        if (pScout < scout) fx->plrScout++;
        if (pSurpr < surpr) fx->monSurpr++;
        pDef = def; pScout = scout; pSurpr = surpr;
    }

    // Pass 2 — brackets 0..index, i.e. radiant down to where we actually are.
    float pStressA = 0, pStressB = 0, pMonA = 0, pMonB = 0, pMonCrit = 0;
    float pLootA = 0, pLootB = 0, pPlrCrit = 0, pHeroSurpr = 0;
    float qDef = 0, qScout = 0, qSurpr = 0;
    for (long i = 0; i <= index; i++) {
        uintptr_t br = b + i * RT_DATA_STRIDE;
        float sA = 0, sB = 0, mA = 0, mB = 0, mc = 0, lA = 0, lB = 0, pc = 0;
        float df = 0, sc = 0, ms = 0, hs = 0;
        abReadF32(br + BR_STRESS_A,  &sA); abReadF32(br + BR_STRESS_B,  &sB);
        abReadF32(br + BR_MONSTER_A, &mA); abReadF32(br + BR_MONSTER_B, &mB);
        abReadF32(br + BR_MON_CRIT,  &mc);
        abReadF32(br + BR_LOOT_A,    &lA); abReadF32(br + BR_LOOT_B,    &lB);
        abReadF32(br + BR_PLR_CRIT,  &pc);
        abReadF32(br + BR_PLR_DEF,   &df); abReadF32(br + BR_PLR_SCOUT,  &sc);
        abReadF32(br + BR_MON_SURPR, &ms); abReadF32(br + BR_HERO_SURPR, &hs);
        if (pStressA   < sA || pStressB < sB) fx->stress++;
        if (pMonA      < mA || pMonB    < mB) fx->monster++;
        if (pMonCrit   < mc)                  fx->monCrit++;
        if (pLootA     < lA || pLootB   < lB) fx->loot++;
        if (pPlrCrit   < pc)                  fx->plrCrit++;
        if (pHeroSurpr < hs)                  fx->heroSurpr++;
        if (df < qDef)   fx->plrDef--;
        if (sc < qScout) fx->plrScout--;
        if (ms < qSurpr) fx->monSurpr--;
        pStressA = sA; pStressB = sB; pMonA = mA; pMonB = mB; pMonCrit = mc;
        pLootA = lA; pLootB = lB; pPlrCrit = pc; pHeroSurpr = hs;
        qDef = df; qScout = sc; qSurpr = ms;
    }
    return true;
}

static bool lightTitle(uintptr_t base, int index, char* out, int outsz) {
    char fmt[0x40];
    if (!safeReadCStr(base + TORCH_TITLE_FMT_RVA, fmt, sizeof fmt) || !fmt[0] ||
        !abFormatMatches(fmt, "d")) {
        logLine("light: titleIdFormat unusable (\"%s\") -> using the shipped format",
                fmt[0] ? fmt : "");
        strcpy(fmt, "str_darkness_title_%d");
    }
    char key[96];
    _snprintf(key, sizeof key, fmt, index);
    key[sizeof key - 1] = 0;
    if (!resolveKey(base, key, out, outsz)) { logLine("light: no title for key \"%s\"", key); return false; }
    abStripMarkup(out);
    return true;
}

static void lightAddEffect(uintptr_t base, const char* key, int n, char* out, int outsz) {
    if (n <= 0) return;
    char label[128];
    if (!resolveKey(base, key, label, sizeof label)) { logLine("light: no label for \"%s\"", key); return; }
    abStripMarkup(label);
    char line[160];
    line[0] = ' ';
    _snprintf(line + 1, sizeof line - 1, axs(AXS_LIGHT_EFFECT_FMT), label, n);
    line[sizeof line - 1] = 0;
    size_t used = strlen(out);
    if (used + 1 < (size_t)outsz) strncat(out, line, outsz - used - 1);
}

// ---- Ambient light: the Courtyard's bloodlight, the Farmstead's miasma and wave lights ----
static const uintptr_t TSET_SLOTS_OFF    = 0x5cd0;   // raid root: TorchSettingsData*[4] (was 0x5c60)
static const int       TSET_SLOT_COUNT   = 4;
static const uintptr_t TSET_ID_OFF       = 0x80;     // char[0x80]: the entry's `id` field
static const uintptr_t TSET_BURNDOWN_OFF = 0x101;    // byte: burn_down_enabled
static const uintptr_t TSET_HEROBUFF_OFF = 0x108;    // vector<char[0x80]> begin/end: hero_buff_list
static const uintptr_t TSET_MONBUFF_OFF  = 0x120;    // vector<char[0x80]>: monster_buff_list
static const uintptr_t TSET_OVERLAYS_OFF = 0x168;    // vector<char[0x80]>: overlays
static const int       TSET_STR_STRIDE   = 0x80;     // the vectors' element size (inline strings)
static const int       TSET_MAX_IDS      = 16;       // sanity bound; the shipped lists hold 1..3

static void lightCat(char* out, int outsz, const char* frag) {
    size_t used = strlen(out);
    if (used + 1 < (size_t)outsz) strncat(out, frag, outsz - used - 1);
}

static uintptr_t lightSettings(uintptr_t root) {
    uintptr_t best = 0;
    for (int i = 0; i < TSET_SLOT_COUNT; i++) {
        uintptr_t p = 0;
        if (safeReadPtr(root + TSET_SLOTS_OFF + (uintptr_t)i * 8, &p) && p > 0x10000) best = p;
    }
    return best;
}

static bool lightHasOverlay(uintptr_t ts, const char* overlayId) {
    uintptr_t b = 0, e = 0;
    if (!safeReadPtr(ts + TSET_OVERLAYS_OFF, &b) ||
        !safeReadPtr(ts + TSET_OVERLAYS_OFF + 8, &e) || b <= 0x10000 || e < b) return false;
    long n = (long)((e - b) / TSET_STR_STRIDE);
    if (n <= 0 || n > TSET_MAX_IDS) return false;
    for (long i = 0; i < n; i++) {
        char id[TSET_STR_STRIDE];
        if (safeReadCStr(b + i * TSET_STR_STRIDE, id, sizeof id) &&
            strcmp(id, overlayId) == 0) return true;
    }
    return false;
}

static void lightAmbientBuffs(uintptr_t base, uintptr_t ts, uintptr_t vecOff,
                              const char* headerKey, char* out, int outsz) {
    uintptr_t b = 0, e = 0;
    if (!safeReadPtr(ts + vecOff, &b) || !safeReadPtr(ts + vecOff + 8, &e) ||
        b <= 0x10000 || e < b) return;
    long n = (long)((e - b) / TSET_STR_STRIDE);
    if (n <= 0 || n > TSET_MAX_IDS) return;

    char hdr[128];
    if (headerKey) {
        if (resolveKey(base, headerKey, hdr, sizeof hdr)) {
            abStripMarkup(hdr);
            char frag[144];
            _snprintf(frag, sizeof frag, " %s:", hdr);   // the join and the colon are punctuation only
            frag[sizeof frag - 1] = 0;
            lightCat(out, outsz, frag);
        } else {
            logLine("light: ambient header \"%s\" did not resolve", headerKey);
        }
    }
    for (long i = 0; i < n; i++) {
        char id[TSET_STR_STRIDE];
        if (!safeReadCStr(b + i * TSET_STR_STRIDE, id, sizeof id) || !id[0]) continue;
        char line[CAMP_BUFF_DESC_BUF];
        if (!csBuffDescByKey(base, resHash(id), nullptr, 0, line, sizeof line)) {
            logLine("light: ambient buff \"%s\" did not describe", id);
            continue;
        }
        char frag[CAMP_BUFF_DESC_BUF + 8];
        _snprintf(frag, sizeof frag, " %s.", line);
        frag[sizeof frag - 1] = 0;
        lightCat(out, outsz, frag);
    }
}

static bool lightStressBracket(uintptr_t base, uintptr_t root, float* chanceOut, float* dmgOut) {
    float t = 0;
    if (!torchValue(root, &t)) return false;
    uintptr_t table = torchTable(base, root);
    int idx = torchLevel(table, t);
    if (idx < 0) return false;
    uintptr_t b = 0, e = 0;
    if (!safeReadPtr(table + RT_DATA_BEGIN, &b) || !safeReadPtr(table + RT_DATA_END, &e)) return false;
    if (b <= 0x10000 || e < b) return false;
    long n = (long)((e - b) / RT_DATA_STRIDE);
    if (n <= 0 || n > RT_MAX_ENTRIES || idx >= n) return false;
    uintptr_t br = b + (uintptr_t)idx * RT_DATA_STRIDE;
    if (!abReadF32(br + BR_STRESS_A, chanceOut) || !abReadF32(br + BR_STRESS_B, dmgOut)) return false;
    return *chanceOut > 0.0f || *dmgOut > 0.0f;
}

// The ambient readout, or false if this raid's light is an ordinary torch.
static bool lightAmbient(uintptr_t base, uintptr_t root, bool withEffects, char* out, int outsz) {
    uintptr_t ts = lightSettings(root);
    if (!ts) return false;

    char id[TSET_STR_STRIDE];
    id[0] = 0;
    safeReadCStr(ts + TSET_ID_OFF, id, sizeof id);

    char title[160];
    title[0] = 0;
    bool blood = false;
    if (id[0]) {
        char key[160];
        _snprintf(key, sizeof key, "str_%s_torch_tooltip_title", id);   // the overlay's own format
        key[sizeof key - 1] = 0;
        if (!resolveKey(base, key, title, sizeof title)) {
            logLine("light: settings id \"%s\" but no \"%s\" — treating as an ordinary torch", id, key);
            title[0] = 0;
        }
    }
    if (!title[0] && lightHasOverlay(ts, "blood_torch")) {
        if (resolveKey(base, "str_blood_torch_tooltip_title", title, sizeof title)) blood = true;
        else logLine("light: blood_torch overlay but its title key did not resolve");
    }
    if (!title[0]) return false;
    abStripMarkup(title);

    _snprintf(out, outsz, "%s.", title);
    out[outsz - 1] = 0;

    uint8_t burn = 0;
    if (safeReadU8(ts + TSET_BURNDOWN_OFF, &burn) && burn) {
        float t = 0;
        if (torchValue(root, &t)) {
            char frag[64];
            frag[0] = ' ';
            _snprintf(frag + 1, sizeof frag - 1, axs(AXS_LIGHT_PCT_FMT), (int)t);
            frag[sizeof frag - 1] = 0;
            lightCat(out, outsz, frag);
        }
    }

    if (withEffects) {
        if (blood) {
            char label[128];
            if (resolveKey(base, "str_blood_torch_stress", label, sizeof label)) {
                abStripMarkup(label);
                char frag[192];
                frag[0] = ' ';
                float chance = 0, dmg = 0;
                if (lightStressBracket(base, root, &chance, &dmg)) {
                    if ((int)chance == (int)dmg)
                        _snprintf(frag + 1, sizeof frag - 1, axs(AXS_LIGHT_AMBIENT_STAT_FMT),
                                  label, (int)chance);
                    else
                        _snprintf(frag + 1, sizeof frag - 1, axs(AXS_LIGHT_AMBIENT_STAT2_FMT),
                                  label, (int)chance, (int)dmg);
                } else {
                    // No readable number: the label alone, exactly what the game shows.
                    logLine("light: blood stress bracket would not read — label only");
                    _snprintf(frag + 1, sizeof frag - 1, "%s.", label);
                }
                frag[sizeof frag - 1] = 0;
                lightCat(out, outsz, frag);
            } else {
                logLine("light: blood line \"str_blood_torch_stress\" did not resolve");
            }

            size_t before = strlen(out);
            lightAmbientBuffs(base, ts, TSET_HEROBUFF_OFF, nullptr, out, outsz);
            if (strlen(out) == before) {
                if (resolveKey(base, "str_blood_torch_bleed_resist", label, sizeof label)) {
                    abStripMarkup(label);
                    char frag[144];
                    _snprintf(frag, sizeof frag, " %s.", label);
                    frag[sizeof frag - 1] = 0;
                    lightCat(out, outsz, frag);
                } else {
                    logLine("light: blood line \"str_blood_torch_bleed_resist\" did not resolve");
                }
            }
        } else {
            lightAmbientBuffs(base, ts, TSET_HEROBUFF_OFF,
                              "str_torch_overlay_hero_buffs_header", out, outsz);
            lightAmbientBuffs(base, ts, TSET_MONBUFF_OFF,
                              "str_torch_overlay_monster_buffs_header", out, outsz);
        }
    }
    return true;
}

// ---- The quest modifier: the Endless Harvest's "Reflections" ----
static const uintptr_t QMOD_ACTIVE_OFF   = 0x5f58;
static const uintptr_t QMOD_ID_OFF       = 0x00;    // char[0x40]: the modifier's id
static const uintptr_t QMOD_HEROBUFF_OFF = 0x90;    // vector<uint32> begin/end: hero buff hashes
static const uintptr_t QMOD_MONBUFF_OFF  = 0xa8;    // vector<uint32>: monster buff hashes

static void lightModifierBuffs(uintptr_t base, uintptr_t mod, uintptr_t vecOff,
                               const char* headerKey, char* out, int outsz) {
    uintptr_t b = 0, e = 0;
    if (!safeReadPtr(mod + vecOff, &b) || !safeReadPtr(mod + vecOff + 8, &e) ||
        b <= 0x10000 || e < b) return;
    long n = (long)((e - b) / 4);
    if (n <= 0 || n > TSET_MAX_IDS) return;

    char hdr[128];
    if (resolveKey(base, headerKey, hdr, sizeof hdr)) {
        abStripMarkup(hdr);
        char frag[144];
        _snprintf(frag, sizeof frag, " %s:", hdr);   // the join and the colon are punctuation only
        frag[sizeof frag - 1] = 0;
        lightCat(out, outsz, frag);
    } else {
        logLine("light: modifier header \"%s\" did not resolve", headerKey);
    }
    for (long i = 0; i < n; i++) {
        uint32_t h = 0;
        if (!safeReadU32(b + (uintptr_t)i * 4, &h) || !h) continue;
        char line[CAMP_BUFF_DESC_BUF];
        if (!csBuffDescByKey(base, h, nullptr, 0, line, sizeof line)) {
            logLine("light: modifier buff hash %08x did not describe", h);
            continue;
        }
        char frag[CAMP_BUFF_DESC_BUF + 8];
        _snprintf(frag, sizeof frag, " %s.", line);
        frag[sizeof frag - 1] = 0;
        lightCat(out, outsz, frag);
    }
}

static void lightQuestModifier(uintptr_t base, uintptr_t root, char* out, int outsz) {
    uintptr_t mod = 0;
    if (!safeReadPtr(root + QMOD_ACTIVE_OFF, &mod) || mod <= 0x10000) return;

    char id[0x40];
    id[0] = 0;
    if (!safeReadCStr(mod + QMOD_ID_OFF, id, sizeof id) || !id[0]) {
        logLine("light: active quest modifier %p has no readable id", (void*)mod);
        return;
    }
    char key[128];
    _snprintf(key, sizeof key, "quest_modifier_name_%s", id);   // the game's own format string
    key[sizeof key - 1] = 0;
    char name[160];
    if (!resolveKey(base, key, name, sizeof name)) {
        logLine("light: quest modifier \"%s\": \"%s\" did not resolve — block skipped", id, key);
        return;
    }
    abStripMarkup(name);
    char frag[176];
    _snprintf(frag, sizeof frag, " %s.", name);
    frag[sizeof frag - 1] = 0;
    lightCat(out, outsz, frag);

    lightModifierBuffs(base, mod, QMOD_HEROBUFF_OFF,
                       "str_quest_modifier_hero_buffs_header", out, outsz);
    lightModifierBuffs(base, mod, QMOD_MONBUFF_OFF,
                       "str_quest_modifier_monster_buffs_header", out, outsz);
}

static void speakLight(uintptr_t base, bool withEffects) {
    uintptr_t root = mapRoot(base);
    if (!root) return;

    {
        char amb[MAILBOX_SZ];
        if (lightAmbient(base, root, withEffects, amb, sizeof amb)) {
            if (withEffects) lightQuestModifier(base, root, amb, sizeof amb);
            logLine("light: ambient -> \"%s\"", amb);
            postSpeech(amb);
            return;
        }
    }

    float t = 0;
    if (!torchValue(root, &t)) { logLine("light: torch value unreadable"); return; }
    uintptr_t table = torchTable(base, root);
    int idx = torchLevel(table, t);

    char out[512];
    _snprintf(out, sizeof out, axs(AXS_LIGHT_PCT_FMT), (int)t);   // the meter's own truncation to int
    out[sizeof out - 1] = 0;

    char title[128];
    if (idx >= 0 && lightTitle(base, idx, title, sizeof title)) {
        char frag[160];
        _snprintf(frag, sizeof frag, " %s.", title);
        frag[sizeof frag - 1] = 0;
        strncat(out, frag, sizeof out - strlen(out) - 1);
    }

    if (withEffects && idx >= 0) {
        LightEffects fx;
        if (lightEffects(table, idx, &fx)) {
            // The meter's own order.
            lightAddEffect(base, "str_darkness_stress",            fx.stress,    out, sizeof out);
            lightAddEffect(base, "str_darkness_monster",           fx.monster,   out, sizeof out);
            lightAddEffect(base, "str_darkness_monster_crit",      fx.monCrit,   out, sizeof out);
            lightAddEffect(base, "str_darkness_heroesSurprised",   fx.heroSurpr, out, sizeof out);
            lightAddEffect(base, "str_darkness_loot",              fx.loot,      out, sizeof out);
            lightAddEffect(base, "str_darkness_player_crit",       fx.plrCrit,   out, sizeof out);
            lightAddEffect(base, "str_darkness_player_def",        fx.plrDef,    out, sizeof out);
            lightAddEffect(base, "str_darkness_player_scout",      fx.plrScout,  out, sizeof out);
            lightAddEffect(base, "str_darkness_monstersSurprised", fx.monSurpr,  out, sizeof out);
        } else {
            logLine("light: effects unreadable (table=%p idx=%d)", (void*)table, idx);
        }
    }
    if (withEffects) lightQuestModifier(base, root, out, sizeof out);
    logLine("light: %.2f level=%d -> \"%s\"", (double)t, idx, out);
    postSpeech(out);
}

void speakMeter(uintptr_t base) {
    speakLight(base, true);
}

static bool  g_torchWatch = false;
static float g_torchWatchFrom = 0;
static DWORD g_torchWatchDeadline = 0;

void armTorchWatch(uintptr_t base) {
    uintptr_t root = mapRoot(base);
    float t = 0;
    if (!root || !torchValue(root, &t)) return;
    g_torchWatch = true;
    g_torchWatchFrom = t;
    g_torchWatchDeadline = GetTickCount() + 1500;
}

void serviceTorchWatch(uintptr_t base) {
    if (!g_torchWatch) return;
    uintptr_t root = mapRoot(base);
    float t = 0;
    if (!root || !torchValue(root, &t)) { g_torchWatch = false; return; }
    float d = t - g_torchWatchFrom;
    if (d < 0) d = -d;
    if (d > 0.01f) { g_torchWatch = false; speakLight(base, false); return; }
    if ((int32_t)(GetTickCount() - g_torchWatchDeadline) >= 0) {
        g_torchWatch = false;
        logLine("light: torch key changed nothing within the window");
    }
}

// ---- Dousing the torch: reduce a step / snuff it out ----
static const uintptr_t LTO_REDUCE_OFF  = 0xcc;      // byte: pending "reduce one step"  (shift+click)
static const uintptr_t LTO_SNUFF_OFF   = 0xcd;      // byte: pending "snuff to zero"    (ctrl+shift)
static const DWORD     LTO_GAP_MS      = 500;       // the meter draws every frame; 500 ms = gone

uintptr_t g_ltoObj      = 0;
DWORD     g_ltoSeenTick = 0;

static uintptr_t lightOverlay(uintptr_t base) {
    if (!g_ltoObj || g_ltoObj <= 0x10000) return 0;
    if (!g_ltoSeenTick || (GetTickCount() - g_ltoSeenTick) >= LTO_GAP_MS) return 0;
    uintptr_t vft = 0;
    if (!safeReadPtr(g_ltoObj, &vft) || vft != base + LTO_VFTABLE_RVA) return 0;
    return g_ltoObj;
}

static bool  g_snuffWatch = false;
static float g_snuffFrom = 0;
static bool  g_snuffWasSnuff = false;
static DWORD g_snuffDeadline = 0;

void requestTorchDouse(uintptr_t base, bool snuff) {
    const char* what = snuff ? "snuff" : "reduce";
    uintptr_t ov = lightOverlay(base);
    if (!ov) {
        logLine("snuff: %s requested but no LightTorchOverlay is live (obj=%p tick=%lu)",
                what, (void*)g_ltoObj, (unsigned long)g_ltoSeenTick);
        postSpeech(axs(AXS_LIGHT_NO_METER));
        return;
    }
    uintptr_t root = mapRoot(base);
    float t = 0;
    if (!root || !torchValue(root, &t)) { logLine("snuff: torch value unreadable"); return; }

    if (!safeWriteU8(ov + (snuff ? LTO_SNUFF_OFF : LTO_REDUCE_OFF), 1)) {
        logLine("⚠ snuff: could not set the %s flag on overlay %p", what, (void*)ov);
        return;
    }
    g_snuffWatch    = true;
    g_snuffFrom     = t;
    g_snuffWasSnuff = snuff;
    g_snuffDeadline = GetTickCount() + 1500;
    logLine("snuff: %s requested, overlay=%p torch=%.2f", what, (void*)ov, (double)t);
}

static int   g_lightLevel = -1;   // the bracket last seen
static float g_lightLast  = 0.0f; // the torch value on the frame we last looked

void serviceSnuffWatch(uintptr_t base) {
    if (!g_snuffWatch) return;
    uintptr_t root = mapRoot(base);
    float t = 0;
    if (!root || !torchValue(root, &t)) { g_snuffWatch = false; return; }

    float d = t - g_snuffFrom;
    if (d < 0) d = -d;
    if (d > 0.01f) {
        g_snuffWatch = false;
        int idx = torchLevel(torchTable(base, root), t);
        char title[128];
        char out[256];
        if (idx >= 0 && lightTitle(base, idx, title, sizeof title))
            _snprintf(out, sizeof out, axs(g_snuffWasSnuff ? AXS_LIGHT_SNUFFED_LEVEL_FMT
                                                           : AXS_LIGHT_REDUCED_LEVEL_FMT),
                      title, (int)t);
        else
            _snprintf(out, sizeof out, axs(g_snuffWasSnuff ? AXS_LIGHT_SNUFFED_PCT_FMT
                                                           : AXS_LIGHT_REDUCED_PCT_FMT),
                      (int)t);
        out[sizeof out - 1] = 0;
        g_lightLevel = idx;
        g_lightLast  = t;
        logLine("snuff: %s done, %.2f -> %.2f level=%d -> \"%s\"",
                g_snuffWasSnuff ? "snuff" : "reduce", (double)g_snuffFrom, (double)t, idx, out);
        postSpeech(out);
        return;
    }
    if ((int32_t)(GetTickCount() - g_snuffDeadline) >= 0) {
        g_snuffWatch = false;
        const char* msg = (t <= 0.01f) ? axs(AXS_LIGHT_ALREADY_OUT)
                                       : axs(AXS_LIGHT_NO_CHANGE);
        logLine("snuff: %s changed nothing within the window (torch=%.2f) -> \"%s\"",
                g_snuffWasSnuff ? "snuff" : "reduce", (double)t, msg);
        postSpeech(msg);
    }
}

// ---- Light level: announce a threshold crossing ----

void serviceLightWatch(uintptr_t base) {
    uintptr_t root = mapRoot(base);
    if (!g_raidDisplay || !root) { g_lightLevel = -1; return; }   // out of a raid: re-seed on the way in

    float t = 0;
    if (!torchValue(root, &t)) return;
    int idx = torchLevel(torchTable(base, root), t);
    if (idx < 0) return;

    if (g_lightLevel < 0) {
        g_lightLevel = idx;                 // entering a dungeon must not announce a "change".
        g_lightLast  = t;
        logLine("light watch: seeded at %.2f level=%d", (double)t, idx);
        return;
    }
    if (g_snuffWatch) { g_lightLevel = idx; g_lightLast = t; return; }
    if (idx == g_lightLevel) { g_lightLast = t; return; }   // same level; track the drift inside it

    bool darker = (t < g_lightLast);
    int  was    = g_lightLevel;
    g_lightLevel = idx;
    g_lightLast  = t;
    if (!darker) { logLine("light watch: level %d -> %d, brightening — not announced", was, idx); return; }

    char title[128];
    char out[256];
    if (lightTitle(base, idx, title, sizeof title))
        _snprintf(out, sizeof out, axs(AXS_LIGHT_DARKER_LEVEL_FMT), title, (int)t);  // the meter's truncation
    else
        _snprintf(out, sizeof out, axs(AXS_LIGHT_DARKER_PCT_FMT), (int)t);           // no name: still say it dipped
    out[sizeof out - 1] = 0;
    logLine("light watch: level %d -> %d at %.2f -> \"%s\"", was, idx, (double)t, out);
    postSpeech(out, false);
}
