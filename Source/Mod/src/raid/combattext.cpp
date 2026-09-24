// raid/combattext.cpp -- WHAT THE RAID SAYS ON SCREEN

#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cmath>
#include "internal.h"

// ---- SURPRISE — which SIDE was ambushed ----
static const char* const SURPRISE_KEY      = "surprise_announcement";

// ---- LIVE COMBAT TEXT + THE COMBAT LOG (.) ----
static const uintptr_t RD_POPUP_MAP_OFF  = 0x3158;
static const uintptr_t RD_POPUP_MAP_SIZE = 0x3160;  // RaidDisplay+: the map's _Mysize (was 0x2cd0)
// MSVC _Tree_node layout, the same shape abEffMapFind already walks.
static const uintptr_t TN_LEFT_OFF   = 0x00;
static const uintptr_t TN_RIGHT_OFF  = 0x10;
static const uintptr_t TN_ISNIL_OFF  = 0x19;
static const uintptr_t TN_KEY_OFF    = 0x20;   // Actor*
static const uintptr_t TN_VAL_OFF    = 0x28;   // TextProp*
static const uintptr_t TEXTPROP_STR_OFF = 0x34;
static const int       TEXTPROP_STR_MAX = 0x80;

static const int CL_MAX_LIVE = 24;
static const int CL_TREE_CAP = 256;

static const int CLOG_MAX      = 150;

// ---- the popup TYPE, and why it takes a second source ----
static const uintptr_t RD_POPUP_Q_BEGIN_OFF = 0x3168;
static const uintptr_t RD_POPUP_Q_END_OFF   = 0x3170;   // (was 0x2ce0; the vector triple after begin)
static const int       CL_Q_STRIDE          = 200;    // 0xc8
static const uintptr_t CLQ_ACTOR_OFF = 0x00;
static const uintptr_t CLQ_STR_OFF   = 0x08;
static const uintptr_t CLQ_NUM_OFF   = 0x4c;
static const uintptr_t CLQ_TYPE_OFF  = 0x58;
static const int       POPUP_TYPE_STRIDE    = 0xdc;
static const uintptr_t PT_NAME_OFF      = 0x00;
static const uintptr_t PT_HAS_STRING_OFF = 0x44;
static const uintptr_t PT_HAS_NUMBER_OFF = 0x45;
static const int       POPUP_TYPE_MAX   = 64;   // 42 ship; the cap is a sanity bound on a read int

static const int CL_HINT_MAX = 32;
struct ClHint {
    uintptr_t actor;
    int       type;
    int       num;
    char      str[96];
    DWORD     tick;
};
static ClHint g_clHints[CL_HINT_MAX];
static int    g_clHintN = 0;

static bool clTypeName(uintptr_t base, int type, char* out, int outsz) {
    out[0] = 0;
    if (type < 0 || type >= POPUP_TYPE_MAX) return false;
    uintptr_t rec = base + POPUP_TYPE_TABLE_RVA + (uintptr_t)type * POPUP_TYPE_STRIDE;
    return safeReadCStr(rec + PT_NAME_OFF, out, outsz) && out[0];
}

static bool clRenderExpected(uintptr_t base, int type, const char* str, int num,
                             char* out, int outsz) {
    out[0] = 0;
    if (type < 0 || type >= POPUP_TYPE_MAX) return false;
    uintptr_t rec = base + POPUP_TYPE_TABLE_RVA + (uintptr_t)type * POPUP_TYPE_STRIDE;
    uint8_t hasStr = 0, hasNum = 0;
    if (!safeReadU8(rec + PT_HAS_STRING_OFF, &hasStr)) return false;
    if (!safeReadU8(rec + PT_HAS_NUMBER_OFF, &hasNum)) return false;

    char raw[CLOG_LINE_MAX];
    if (hasStr && str && str[0]) {
        if (hasNum) _snprintf(raw, sizeof raw, "%s\n%d", str, num);
        else        _snprintf(raw, sizeof raw, "%s", str);
    } else if (hasNum) {
        _snprintf(raw, sizeof raw, "%d", num);
    } else return false;
    raw[sizeof raw - 1] = 0;

    // Same flattening the prop text goes through, so the two are comparable.
    char flat[CLOG_LINE_MAX];
    int o = 0;
    for (int i = 0; raw[i] && o < (int)sizeof flat - 2; i++) {
        if (raw[i] == '\n' || raw[i] == '\r') {
            if (o > 0 && flat[o - 1] != ' ') { flat[o++] = '.'; flat[o++] = ' '; }
        } else flat[o++] = raw[i];
    }
    flat[o] = 0;
    stripMarkup(flat, out, outsz);
    return out[0] != 0;
}

void clCaptureEntry(uintptr_t rd, unsigned int idx) {
    uintptr_t begin = 0, end = 0;
    if (!safeReadPtr(rd + RD_POPUP_Q_BEGIN_OFF, &begin) ||
        !safeReadPtr(rd + RD_POPUP_Q_END_OFF, &end)) return;
    if (begin == 0 || end <= begin) return;
    if ((uintptr_t)idx >= (end - begin) / CL_Q_STRIDE) return;   // index out of the live vector

    uintptr_t e = begin + (uintptr_t)idx * CL_Q_STRIDE;
    uintptr_t actor = 0; uint32_t type = 0; int32_t num = 0;
    if (!safeReadPtr(e + CLQ_ACTOR_OFF, &actor) || !actor) return;
    if (!safeReadU32(e + CLQ_TYPE_OFF, &type)) return;
    safeReadU32(e + CLQ_NUM_OFF, (uint32_t*)&num);
    char str[96];
    if (!safeReadCStr(e + CLQ_STR_OFF, str, sizeof str)) str[0] = 0;

    int slot = -1;
    for (int j = 0; j < g_clHintN; j++)
        if (g_clHints[j].actor == actor && g_clHints[j].type == (int)type) { slot = j; break; }
    if (slot < 0) {
        if (g_clHintN < CL_HINT_MAX) slot = g_clHintN++;
        else {
            slot = 0;
            for (int j = 1; j < g_clHintN; j++)
                if (g_clHints[j].tick < g_clHints[slot].tick) slot = j;
        }
    }
    g_clHints[slot].actor = actor;
    g_clHints[slot].type  = (int)type;
    g_clHints[slot].num   = num;
    strncpy(g_clHints[slot].str, str, sizeof g_clHints[slot].str - 1);
    g_clHints[slot].str[sizeof g_clHints[slot].str - 1] = 0;
    g_clHints[slot].tick  = GetTickCount();
}

void clScanQueue(uintptr_t base, uintptr_t rd) {
    (void)base;
    uintptr_t begin = 0, end = 0;
    if (!safeReadPtr(rd + RD_POPUP_Q_BEGIN_OFF, &begin) ||
        !safeReadPtr(rd + RD_POPUP_Q_END_OFF, &end)) return;
    if (begin == 0 || end <= begin) return;
    uintptr_t n = (end - begin) / CL_Q_STRIDE;
    if (n > 64) n = 64;

    DWORD now = GetTickCount();
    for (uintptr_t i = 0; i < n; i++) {
        uintptr_t e = begin + i * CL_Q_STRIDE;
        uintptr_t actor = 0; uint32_t type = 0; int32_t num = 0;
        if (!safeReadPtr(e + CLQ_ACTOR_OFF, &actor) || !actor) continue;
        if (!safeReadU32(e + CLQ_TYPE_OFF, &type)) continue;
        safeReadU32(e + CLQ_NUM_OFF, (uint32_t*)&num);
        char str[96];
        if (!safeReadCStr(e + CLQ_STR_OFF, str, sizeof str)) str[0] = 0;

        int slot = -1;
        for (int j = 0; j < g_clHintN; j++)
            if (g_clHints[j].actor == actor && g_clHints[j].type == (int)type) { slot = j; break; }
        if (slot < 0) {
            if (g_clHintN < CL_HINT_MAX) slot = g_clHintN++;
            else {
                slot = 0;
                for (int j = 1; j < g_clHintN; j++)
                    if (g_clHints[j].tick < g_clHints[slot].tick) slot = j;
            }
        }
        g_clHints[slot].actor = actor;
        g_clHints[slot].type  = (int)type;
        g_clHints[slot].num   = num;
        strncpy(g_clHints[slot].str, str, sizeof g_clHints[slot].str - 1);
        g_clHints[slot].str[sizeof g_clHints[slot].str - 1] = 0;
        g_clHints[slot].tick  = now;
    }
}

static int clTypeForPopup(uintptr_t base, uintptr_t actor, const char* propText) {
    int best = -1; DWORD bestTick = 0;
    int forActor = 0;
    char firstExpect[CLOG_LINE_MAX]; firstExpect[0] = 0;
    int  firstType = -1;

    for (int j = 0; j < g_clHintN; j++) {
        if (g_clHints[j].actor != actor) continue;
        forActor++;
        char expect[CLOG_LINE_MAX];
        if (!clRenderExpected(base, g_clHints[j].type, g_clHints[j].str, g_clHints[j].num,
                              expect, sizeof expect)) continue;
        if (!firstExpect[0]) {
            strncpy(firstExpect, expect, sizeof firstExpect - 1);
            firstExpect[sizeof firstExpect - 1] = 0;
            firstType = g_clHints[j].type;
        }
        if (strcmp(expect, propText) != 0) continue;      // could have disagreed, and did
        if (best < 0 || g_clHints[j].tick >= bestTick) { best = g_clHints[j].type; bestTick = g_clHints[j].tick; }
    }

    if (best < 0) {
        if (forActor == 0)
            logLine("combattext hint: NONE for actor=%p (text=\"%s\") — never sampled while queued",
                    (void*)actor, propText);
        else
            logLine("combattext hint: %d for actor=%p but none matched; first was type=%d expect=\"%s\" vs prop=\"%s\"",
                    forActor, (void*)actor, firstType, firstExpect, propText);
    }
    return best;
}

// ---- phrasing ----
struct ClPhrase { const char* type; AxStrId fmt; };   // fmt takes (who, number)
static const ClPhrase kClPhrases[] = {
    { "damage",            AXS_CT_DAMAGE_FMT },
    { "crit_damage",       AXS_CT_CRIT_DAMAGE_FMT },
    { "no_damage",         AXS_CT_NO_DAMAGE_FMT },
    { "hero_heal",         AXS_CT_HEAL_FMT },
    { "monster_heal",      AXS_CT_HEAL_FMT },
    { "hero_heal_crit",    AXS_CT_HEAL_CRIT_FMT },
    { "monster_heal_crit", AXS_CT_HEAL_CRIT_FMT },
    { "hp_heal_dot",       AXS_CT_HEAL_FMT },
    { "hp_heal_dot_crit",  AXS_CT_HEAL_CRIT_FMT },
    { "hp_heal_dot_onset", AXS_CT_HEAL_FMT },
    { "stress_damage",     AXS_CT_STRESS_FMT },
    { "stress_dot",        AXS_CT_STRESS_FMT },
    { "stress_reduce",     AXS_CT_STRESS_LESS_FMT },
};
static const int kClPhraseCount = (int)(sizeof kClPhrases / sizeof kClPhrases[0]);

static const char* clPhraseFor(const char* typeName) {
    for (int i = 0; i < kClPhraseCount; i++)
        if (strcmp(kClPhrases[i].type, typeName) == 0) return axs(kClPhrases[i].fmt);
    return nullptr;
}

// ---- the scrollback ----
static char g_clogLines[CLOG_MAX][CLOG_LINE_MAX];
static int  g_clogHead  = 0;    // index of the OLDEST line
static int  g_clogCount = 0;
volatile bool g_clogOpen = false;
static int  g_clogCursor = 0;   // 0 = newest; counts BACKWARDS into history

static const char* clogAt(int fromNewest) {
    if (fromNewest < 0 || fromNewest >= g_clogCount) return nullptr;
    int idx = (g_clogHead + g_clogCount - 1 - fromNewest) % CLOG_MAX;
    return g_clogLines[idx];
}

static void clogAdd(const char* line) {
    if (!line || !line[0]) return;
    int slot = (g_clogHead + g_clogCount) % CLOG_MAX;
    strncpy(g_clogLines[slot], line, CLOG_LINE_MAX - 1);
    g_clogLines[slot][CLOG_LINE_MAX - 1] = 0;
    if (g_clogCount < CLOG_MAX) g_clogCount++;
    else g_clogHead = (g_clogHead + 1) % CLOG_MAX;   // ring full: the oldest falls off
}

// ---- sentence punctuation, once ----
static bool clEndsSentence(const char* s) {
    size_t n = strlen(s);
    while (n && s[n - 1] == ' ') n--;
    if (!n) return false;
    char c = s[n - 1];
    return c == '.' || c == '!' || c == '?';
}

static void clEnsureDot(char* line, int linesz) {
    size_t n = strlen(line);
    while (n && line[n - 1] == ' ') line[--n] = 0;
    if (!n || clEndsSentence(line)) return;
    if ((int)n + 1 < linesz) { line[n] = '.'; line[n + 1] = 0; }
}

// ---- what is currently floating on screen ----
struct ClPopup {
    uintptr_t actor;
    uintptr_t prop;
    uint32_t  hash;
    char      text[CLOG_LINE_MAX];
};
static ClPopup g_clSeen[CL_MAX_LIVE];
static int     g_clSeenN = 0;

static uint32_t clHash(const char* s) {
    uint32_t h = 2166136261u;                      // FNV-1a
    for (; *s; s++) { h ^= (uint8_t)*s; h *= 16777619u; }
    return h;
}

static bool clPropText(uintptr_t prop, char* out, int outsz) {
    char raw[TEXTPROP_STR_MAX + 1];
    if (!safeReadCStr(prop + TEXTPROP_STR_OFF, raw, sizeof raw) || !raw[0]) return false;
    char flat[TEXTPROP_STR_MAX + 1];
    int o = 0;
    for (int i = 0; raw[i] && o < (int)sizeof flat - 2; i++) {
        if (raw[i] == '\n' || raw[i] == '\r') {
            if (o > 0 && flat[o - 1] != ' ') { flat[o++] = '.'; flat[o++] = ' '; }
        } else flat[o++] = raw[i];
    }
    flat[o] = 0;
    stripMarkup(flat, out, outsz);
    return out[0] != 0;
}

static bool clActorName(uintptr_t base, uintptr_t actor, char* out, int outsz) {
    out[0] = 0;
    if (!actor) return false;

    uintptr_t party[RV_MAX_MEMBERS];
    int np = rvPartyList(base, party, RV_MAX_MEMBERS);
    for (int i = 0; i < np; i++) {
        if (party[i] != actor) continue;
        char cls[96];
        // Name only — the class is in the dungeon view and would pad every single popup.
        if (abHeroNameClassOf(base, actor, out, outsz, cls, sizeof cls) && out[0]) return true;
        return false;
    }
    uintptr_t foes[RV_MAX_ENEMIES];
    int ne = rvEnemyList(base, foes, RV_MAX_ENEMIES);
    for (int i = 0; i < ne; i++) {
        if (foes[i] == actor) return rvMonsterName(base, actor, out, outsz) && out[0];
    }
    return false;
}

static int clCollectLive(uintptr_t rd, ClPopup* out, int maxOut) {
    uintptr_t head = 0;
    if (!safeReadPtr(rd + RD_POPUP_MAP_OFF, &head) || head < 0x10000) return 0;
    // _Myhead->_Parent is the root; _Myhead itself is the nil sentinel.
    uintptr_t root = 0;
    if (!safeReadPtr(head + 0x08, &root) || root < 0x10000) return 0;
    uint8_t rootNil = 1;
    if (!safeReadU8(root + TN_ISNIL_OFF, &rootNil) || rootNil) return 0;   // empty map

    uintptr_t stack[64];
    int sp = 0, n = 0, visits = 0;
    stack[sp++] = root;
    while (sp > 0 && n < maxOut && visits < CL_TREE_CAP) {
        uintptr_t node = stack[--sp];
        visits++;
        uint8_t nil = 1;
        if (!safeReadU8(node + TN_ISNIL_OFF, &nil) || nil) continue;

        uintptr_t actor = 0, prop = 0;
        if (safeReadPtr(node + TN_KEY_OFF, &actor) && safeReadPtr(node + TN_VAL_OFF, &prop) &&
            prop >= 0x10000) {
            char text[CLOG_LINE_MAX];
            if (clPropText(prop, text, sizeof text)) {
                out[n].actor = actor;
                out[n].prop  = prop;
                out[n].hash  = clHash(text);
                strncpy(out[n].text, text, CLOG_LINE_MAX - 1);
                out[n].text[CLOG_LINE_MAX - 1] = 0;
                n++;
            }
        }
        uintptr_t l = 0, r = 0;
        if (sp < 62) {
            if (safeReadPtr(node + TN_LEFT_OFF,  &l) && l >= 0x10000) stack[sp++] = l;
            if (safeReadPtr(node + TN_RIGHT_OFF, &r) && r >= 0x10000) stack[sp++] = r;
        }
    }
    if (visits >= CL_TREE_CAP) logLine("combattext: tree visit cap hit (%d) — map not walked fully", visits);
    return n;
}

// ---- WHAT THE "Buff!" / "Debuff!" POPUP ACTUALLY WAS ----
static const int CL_BUFFSNAP_ACTORS = RV_MAX_MEMBERS + RV_MAX_ENEMIES;
static const int CL_BUFFNEW_MAX     = 12;
static const DWORD CL_BUFFNEW_MS    = 6000;

struct ClBuffSnap {
    uintptr_t actor;
    DWORD     tick;
    int       n;
    uint32_t  fp[ACTOR_BUFF_MAX];
    bool      stealth;
};
static ClBuffSnap g_clBuffSnap[CL_BUFFSNAP_ACTORS];
static int        g_clBuffSnapN = 0;

struct ClBuffNew {
    uintptr_t     actor;                       // 0 = free
    DWORD         tick;
    int           polarity;                    // +1 buff, -1 debuff, 0 neither
    bool          spoken;
    unsigned char rec[CAMP_BUFF_REC_SZ];
};
static ClBuffNew g_clBuffNew[CL_BUFFNEW_MAX];

static uint32_t clBuffFingerprint(const unsigned char* rec) {
    uint32_t h = 2166136261u;                                        // FNV-1a, as clHash
    const unsigned char* fields[] = { rec + BUFFREC_STAT_TYPE_OFF, rec + BUFFREC_AMOUNT_OFF,
                                      rec + BUFFREC_DURTYPE_OFF,   rec + BUFFREC_SOURCE_OFF };
    for (int f = 0; f < 4; f++)
        for (int b = 0; b < 4; b++) { h ^= fields[f][b]; h *= 16777619u; }
    for (int i = 0; i < 0x40 && rec[BUFFREC_STATSUB_OFF + i]; i++) {
        h ^= rec[BUFFREC_STATSUB_OFF + i]; h *= 16777619u;
    }
    return h;
}

static void clBuffRemember(uintptr_t base, uintptr_t actor, const unsigned char* rec) {
    int slot = -1;
    for (int i = 0; i < CL_BUFFNEW_MAX; i++)
        if (!g_clBuffNew[i].actor) { slot = i; break; }
    if (slot < 0) {
        for (int i = 0; i < CL_BUFFNEW_MAX; i++)
            if (g_clBuffNew[i].spoken &&
                (slot < 0 || g_clBuffNew[i].tick < g_clBuffNew[slot].tick)) slot = i;
        if (slot < 0) {
            slot = 0;
            for (int i = 1; i < CL_BUFFNEW_MAX; i++)
                if (g_clBuffNew[i].tick < g_clBuffNew[slot].tick) slot = i;
        }
    }
    g_clBuffNew[slot].actor    = actor;
    g_clBuffNew[slot].tick     = GetTickCount();
    g_clBuffNew[slot].spoken   = false;
    g_clBuffNew[slot].polarity = spBuffPolarityAny(base, rec);
    memcpy(g_clBuffNew[slot].rec, rec, CAMP_BUFF_REC_SZ);
    logLine("combatbuff: actor=%p gained stat=%u sub=\"%.32s\" amount=%.4f rounds=%d pol=%d",
            (void*)actor, *(const uint32_t*)(rec + BUFFREC_STAT_TYPE_OFF),
            (const char*)(rec + BUFFREC_STATSUB_OFF),
            (double)*(const float*)(rec + BUFFREC_AMOUNT_OFF),
            *(const int32_t*)(rec + BUFFREC_ROUNDS_OFF), g_clBuffNew[slot].polarity);
}

static void clAnnounceStealth(uintptr_t base, uintptr_t actor, bool gained) {
    char what[256];
    if (gained) {
        if (!spDurTitleLine(base, actor, STAT_TYPE_STEALTH, "tray_icon_tooltip_stealth_title_%s",
                            what, sizeof what)) {
            logLine("stealth: actor=%p GAINED it but the title would not build — nothing spoken",
                    (void*)actor);
            return;
        }
    } else {
        float hp = 0.0f;
        if (abReadF32(actor + ACTOR_CUR_HP_OFF, &hp) && hp <= 0.0f) {
            logLine("stealth: actor=%p lost it at hp=%.2f — reading that as death, not de-stealth",
                    (void*)actor, (double)hp);
            return;
        }
        if (!abTipPlain(base, "effect_unstealth_format", what, sizeof what) || !what[0]) {
            logLine("stealth: actor=%p LOST it but \"effect_unstealth_format\" did not resolve",
                    (void*)actor);
            return;
        }
    }

    char who[160], line[CLOG_LINE_MAX];
    if (clActorName(base, actor, who, sizeof who)) _snprintf(line, sizeof line, "%s, %s", who, what);
    else                                           _snprintf(line, sizeof line, "%s", what);
    line[sizeof line - 1] = 0;
    clEnsureDot(line, sizeof line);

    clogAdd(line);
    logLine("stealth: actor=%p %s -> \"%s\"", (void*)actor, gained ? "GAINED" : "LOST", line);
    if (!g_clogOpen) postSpeech(line, false);
}

// Diff one actor's buff vector against the previous poll and remember what is new.
static void clBuffScanActor(uintptr_t base, uintptr_t actor) {
    uintptr_t begin = 0;
    int n = spBuffCount(actor, &begin);
    if (n < 0) return;

    ClBuffSnap* snap = nullptr;
    for (int i = 0; i < g_clBuffSnapN; i++)
        if (g_clBuffSnap[i].actor == actor) { snap = &g_clBuffSnap[i]; break; }
    bool firstSight = (snap == nullptr);
    if (firstSight) {
        if (g_clBuffSnapN < CL_BUFFSNAP_ACTORS) {
            snap = &g_clBuffSnap[g_clBuffSnapN++];
        } else {
            snap = &g_clBuffSnap[0];
            for (int i = 1; i < g_clBuffSnapN; i++)
                if (g_clBuffSnap[i].tick < snap->tick) snap = &g_clBuffSnap[i];
            logLine("combatbuff: snapshot table full, recycling actor=%p for actor=%p",
                    (void*)snap->actor, (void*)actor);
        }
        snap->actor = actor;
        snap->n = 0;
        snap->stealth = false;
    }
    snap->tick = GetTickCount();

    uint32_t now[ACTOR_BUFF_MAX];
    int nowN = 0;
    bool stealthNow = false;
    for (int i = 0; i < n && nowN < ACTOR_BUFF_MAX; i++) {
        unsigned char rec[CAMP_BUFF_REC_SZ];
        if (!safeReadBlock(begin + (uintptr_t)i * ACTOR_BUFF_STRIDE + ACTOR_BUFF_RECORD_OFF,
                           rec, CAMP_BUFF_REC_SZ)) continue;
        uint32_t fp = clBuffFingerprint(rec);
        now[nowN++] = fp;
        if (*(const uint32_t*)(rec + BUFFREC_STAT_TYPE_OFF) == (uint32_t)STAT_TYPE_STEALTH)
            stealthNow = true;

        if (firstSight) continue;
        int seenNow = 0;
        for (int k = 0; k < nowN - 1; k++) if (now[k] == fp) seenNow++;
        int had = 0;
        for (int k = 0; k < snap->n; k++) if (snap->fp[k] == fp) had++;
        if (seenNow < had) continue;            // this occurrence was already there last poll
        clBuffRemember(base, actor, rec);
    }

    bool wasStealthed = firstSight ? false : snap->stealth;
    if (stealthNow != wasStealthed) clAnnounceStealth(base, actor, stealthNow);
    snap->stealth = stealthNow;

    snap->n = nowN;
    for (int i = 0; i < nowN; i++) snap->fp[i] = now[i];
}

static void clBuffScan(uintptr_t base, uintptr_t root) {
    (void)root;
    uintptr_t party[RV_MAX_MEMBERS];
    int np = rvPartyList(base, party, RV_MAX_MEMBERS);
    for (int i = 0; i < np; i++) clBuffScanActor(base, party[i]);
    uintptr_t foes[RV_MAX_ENEMIES];
    int ne = rvEnemyList(base, foes, RV_MAX_ENEMIES);
    for (int i = 0; i < ne; i++) clBuffScanActor(base, foes[i]);
}

static int clAppendBuffDetail(uintptr_t base, uintptr_t actor, int want, char* line, int linesz,
                              bool* alreadyToldOut) {
    DWORD now = GetTickCount();
    int named = 0;
    if (alreadyToldOut) *alreadyToldOut = false;
    for (int i = 0; i < CL_BUFFNEW_MAX; i++) {
        if (g_clBuffNew[i].actor != actor) continue;
        if (now - g_clBuffNew[i].tick > CL_BUFFNEW_MS) { g_clBuffNew[i].actor = 0; continue; }
        if (g_clBuffNew[i].polarity != want) continue;
        if (g_clBuffNew[i].spoken) {
            if (alreadyToldOut) *alreadyToldOut = true;
            continue;
        }
        char desc[CAMP_BUFF_DESC_BUF];
        if (!spBuffTextFromRecord(base, actor, g_clBuffNew[i].rec, desc, sizeof desc) || !desc[0]) {
            g_clBuffNew[i].actor = 0;
            continue;
        }
        {
            char lbl[96];
            const char* body = desc;
            if (spPolarityLabel(base, want, lbl, sizeof lbl)) {
                size_t ll = strlen(lbl);
                if (strncmp(desc, lbl, ll) == 0 && desc[ll] == ':')
                    { body = desc + ll + 1; while (*body == ' ') body++; }
            }
            if (body[0]) {
                size_t at = strlen(line);
                _snprintf(line + at, linesz - (int)at, " %s", body);
                line[linesz - 1] = 0;
                clEnsureDot(line, linesz);
                named++;
            }
        }
        g_clBuffNew[i].spoken = true;           // claimed either way: it had its chance. It STAYS
                                                // until expiry so a duplicate popup reads as told.
    }
    return named;
}

// ---- THE FORCED-MOVE WATCH ----
struct ClRankSnap { uintptr_t actor; int lo; int hi; };
static ClRankSnap g_clRankParty[RV_MAX_MEMBERS];
static int        g_clRankPartyN = -1;                 // -1 = no baseline yet
static ClRankSnap g_clRankFoes[RV_MAX_ENEMIES];
static int        g_clRankFoesN  = -1;

static void clRankBaseline(const uintptr_t* now, int n, ClRankSnap* snap, int* snapN) {
    for (int i = 0; i < n; i++) {
        snap[i].actor = now[i];
        snap[i].hi = 0;
        snap[i].lo = rvRankFromGame(now[i], nullptr, &snap[i].hi);
    }
    *snapN = n;
}

static void clRankAnnounce(const char* line) {
    clogAdd(line);
    if (!g_clogOpen) postSpeech(line, false);
}

static void clRankScanSide(uintptr_t base, const uintptr_t* now, int n,
                           ClRankSnap* snap, int* snapN, bool party,
                           uintptr_t turnActor, const uintptr_t* tgt, int nTgt) {
    if (*snapN < 0) { clRankBaseline(now, n, snap, snapN); return; }

    bool sameSet = (n == *snapN);
    for (int i = 0; sameSet && i < n; i++) {
        bool found = false;
        for (int j = 0; j < *snapN; j++) if (snap[j].actor == now[i]) { found = true; break; }
        sameSet = found;
    }
    if (!sameSet) {
        logLine("movewatch: %s membership changed (%d -> %d) — resnapshot, not a move",
                party ? "party" : "enemy", *snapN, n);
        clRankBaseline(now, n, snap, snapN);
        return;
    }

    struct { uintptr_t actor; int lo, hi; } mv[(RV_MAX_ENEMIES > RV_MAX_MEMBERS) ? RV_MAX_ENEMIES : RV_MAX_MEMBERS];
    int movers = 0;
    for (int i = 0; i < n; i++) {
        int hi = 0, lo = rvRankFromGame(now[i], nullptr, &hi);
        if (!lo) continue;
        for (int j = 0; j < *snapN; j++) {
            if (snap[j].actor != now[i]) continue;
            if (snap[j].lo != lo || snap[j].hi != hi) {
                if (snap[j].lo) {
                    mv[movers].actor = now[i]; mv[movers].lo = lo; mv[movers].hi = hi;
                    movers++;
                }
                snap[j].lo = lo; snap[j].hi = hi;
            }
            break;
        }
    }
    if (!movers) return;

    if (party && abMoveWatchArmed()) {
        logLine("movewatch: %d party mover(s) while the player's move watch is armed — theirs to announce",
                movers);
        return;
    }

    int said = 0;
    for (int i = 0; i < movers; i++) {
        bool targeted = (mv[i].actor == turnActor) || abWasRecentCommitTarget(mv[i].actor);
        for (int j = 0; !targeted && j < nTgt; j++) targeted = (tgt[j] == mv[i].actor);
        if (!targeted) {
            logLine("movewatch: actor=%p moved to %d..%d but was not the action's target — not announced",
                    (void*)mv[i].actor, mv[i].lo, mv[i].hi);
            mv[i].actor = 0;
        } else {
            said++;
        }
    }
    if (!said) return;

    if (said >= 3) {
        const char* line = axs(party ? AXS_CT_PARTY_SHUFFLED : AXS_CT_ENEMIES_SHUFFLED);
        logLine("movewatch: %s side, %d targeted movers -> \"%s\"", party ? "party" : "enemy", said, line);
        clRankAnnounce(line);
        return;
    }
    for (int i = 0; i < movers; i++) {
        if (!mv[i].actor) continue;
        char who[160], line[CLOG_LINE_MAX];
        if (!clActorName(base, mv[i].actor, who, sizeof who) || !who[0]) {
            logLine("movewatch: actor=%p moved to %d..%d but could not be named — nothing spoken",
                    (void*)mv[i].actor, mv[i].lo, mv[i].hi);
            continue;
        }
        if (mv[i].hi > mv[i].lo)
            _snprintf(line, sizeof line, axs(AXS_CT_MOVED_TO_RANKS_FMT), who, mv[i].lo, mv[i].hi);
        else
            _snprintf(line, sizeof line, axs(AXS_CT_MOVED_TO_RANK_FMT), who, mv[i].lo);
        line[sizeof line - 1] = 0;
        logLine("movewatch: actor=%p -> \"%s\"", (void*)mv[i].actor, line);
        clRankAnnounce(line);
    }
}

static void clRankScan(uintptr_t base, uintptr_t root) {
    if (!root || !abBattleLive(root)) { g_clRankPartyN = -1; g_clRankFoesN = -1; return; }

    uintptr_t turnActor = abCurrentTurnActor(root);
    uintptr_t tgt[8];
    int nTgt = 0;
    if (turnActor) {
        uintptr_t beg = 0, end = 0;
        if (safeReadPtr(turnActor + ACTOR_TARGETS_OFF, &beg) &&
            safeReadPtr(turnActor + ACTOR_TARGETS_OFF + 8, &end) && beg && end > beg) {
            int cnt = (int)((end - beg) / 8);
            if (cnt > 8) cnt = 8;
            for (int i = 0; i < cnt; i++) {
                uintptr_t a = 0;
                if (safeReadPtr(beg + (uintptr_t)i * 8, &a) && a) tgt[nTgt++] = a;
            }
        }
    }

    uintptr_t party[RV_MAX_MEMBERS];
    int np = rvPartyList(base, party, RV_MAX_MEMBERS);
    uintptr_t foes[RV_MAX_ENEMIES];
    int ne = rvEnemyList(base, foes, RV_MAX_ENEMIES);
    clRankScanSide(base, party, np, g_clRankParty, &g_clRankPartyN, true,  turnActor, tgt, nTgt);
    clRankScanSide(base, foes,  ne, g_clRankFoes,  &g_clRankFoesN,  false, turnActor, tgt, nTgt);
}

void serviceCombatText(uintptr_t base) {
    uintptr_t rd = (uintptr_t)g_raidDisplay;
    if (!rd || !mapRoot(base)) {
        g_clSeenN = 0;
        g_clHintN = 0;
        g_clBuffSnapN = 0;
        g_clRankPartyN = -1;
        g_clRankFoesN  = -1;
        for (int i = 0; i < CL_BUFFNEW_MAX; i++) g_clBuffNew[i].actor = 0;
        return;
    }

    clScanQueue(base, rd);

    clBuffScan(base, mapRoot(base));

    ClPopup live[CL_MAX_LIVE];
    int n = clCollectLive(rd, live, CL_MAX_LIVE);

    for (int i = 0; i < n; i++) {
        bool known = false;
        for (int j = 0; j < g_clSeenN; j++) {
            if (g_clSeen[j].actor == live[i].actor && g_clSeen[j].prop == live[i].prop &&
                g_clSeen[j].hash == live[i].hash) { known = true; break; }
        }
        if (known) continue;

        char who[160];
        char line[CLOG_LINE_MAX];
        bool named = clActorName(base, live[i].actor, who, sizeof who);

        int  type = clTypeForPopup(base, live[i].actor, live[i].text);
        char tname[64]; tname[0] = 0;
        const char* fmt = nullptr;
        if (type >= 0 && clTypeName(base, type, tname, sizeof tname)) fmt = clPhraseFor(tname);

        int want = 0;
        if (tname[0])
            want = strcmp(tname, "buff") == 0 ? 1 : strcmp(tname, "debuff") == 0 ? -1 : 0;
        if (!want) {
            char word[96];
            if (abTipPlain(base, "str_ui_debuff", word, sizeof word) &&
                strcmp(live[i].text, word) == 0) want = -1;
            else if (abTipPlain(base, "str_ui_buff", word, sizeof word) &&
                     strcmp(live[i].text, word) == 0) want = 1;
        }
        char detail[CLOG_LINE_MAX];
        detail[0] = 0;
        int got = 0;
        bool alreadyTold = false;
        if (want) {
            got = clAppendBuffDetail(base, live[i].actor, want, detail, sizeof detail, &alreadyTold);
            if (!got && alreadyTold) {
                logLine("combatbuff: %s on actor=%p — its records were already spoken; suppressed",
                        want > 0 ? "buff" : "debuff", (void*)live[i].actor);
                continue;
            }
            if (!got)
                logLine("combatbuff: %s on actor=%p had nothing to name — the bare popup goes out",
                        want > 0 ? "buff" : "debuff", (void*)live[i].actor);
        }

        if (got && named) {
            _snprintf(line, sizeof line, "%s,%s", who, detail);   // detail starts with its own space
        } else if (got) {
            const char* d = detail;
            while (*d == ' ') d++;                 // unnamed actor: the effect stands alone
            strncpy(line, d, sizeof line - 1);
        } else if (fmt && named) {
            _snprintf(line, sizeof line, fmt, who, live[i].text);
        } else if (named) {
            _snprintf(line, sizeof line, "%s, %s", who, live[i].text);
        } else {
            strncpy(line, live[i].text, sizeof line - 1);
        }
        line[sizeof line - 1] = 0;
        clEnsureDot(line, sizeof line);

        clogAdd(line);
        logLine("combattext: actor=%p prop=%p type=%d(%s)%s -> \"%s\"",
                (void*)live[i].actor, (void*)live[i].prop, type,
                tname[0] ? tname : "?", fmt ? "" : " [no phrase]", line);

        if (!g_clogOpen) postSpeech(line, false);
    }

    g_clSeenN = n;
    for (int i = 0; i < n; i++) g_clSeen[i] = live[i];

    clRankScan(base, mapRoot(base));
}

// ---- CENTRE-SCREEN BANNERS (RaidDisplay::ShowAnnouncement) ----
static const uintptr_t RD_ANN_BEGIN_OFF = 0x40e0;
static const uintptr_t RD_ANN_END_OFF   = 0x40e8;
static const uintptr_t ANN_REC_STRIDE   = 0x98;    // one record, per the lambda's append
static const uintptr_t ANN_TEXT_OFF     = 0x00;    // record+: the display text
static const int       ANN_TEXT_MAX     = 0x80;    // char[128] — FixedLengthString<128>

static const int ANN_MAX_LIVE = 8;

struct AnnRec { uint32_t hash; char text[CLOG_LINE_MAX]; };

static AnnRec g_annSeen[ANN_MAX_LIVE];
static int    g_annSeenN = 0;

static bool annRecText(uintptr_t rec, char* out, int outsz) {
    char raw[ANN_TEXT_MAX + 1];
    if (!safeReadCStr(rec + ANN_TEXT_OFF, raw, sizeof raw) || !raw[0]) return false;
    char flat[ANN_TEXT_MAX + 1];
    int o = 0;
    for (int i = 0; raw[i] && o < (int)sizeof flat - 2; i++) {
        if (raw[i] == '\n' || raw[i] == '\r') {
            if (o > 0 && flat[o - 1] != ' ') { flat[o++] = '.'; flat[o++] = ' '; }
        } else flat[o++] = raw[i];
    }
    flat[o] = 0;
    stripMarkup(flat, out, outsz);
    return out[0] != 0;
}

// ---- naming the actor a banner belongs to ----
static bool annTextEq(const char* a, const char* b) {
    while (*a == ' ') a++;
    while (*b == ' ') b++;
    for (;;) {
        char ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca = (char)(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z') cb = (char)(cb - 'A' + 'a');
        // Trailing spaces and a trailing '.'/'!' are formatting, not identity.
        if (!ca || !cb) {
            while (*a == ' ' || *a == '.' || *a == '!') a++;
            while (*b == ' ' || *b == '.' || *b == '!') b++;
            return !*a && !*b;
        }
        if (ca != cb) return false;
        a++; b++;
    }
}

static bool annActorOwnsSkill(uintptr_t base, uintptr_t actor, const char* text) {
    if (!actor || !text || !text[0]) return false;

    uintptr_t beg = 0;
    int n = rvSkillCount(actor, &beg);
    for (int i = 0; i < n && beg; i++) {
        uintptr_t skill = beg + (uintptr_t)i * MONSTER_SKILL_STRIDE;
        char id[0x41];
        if (!safeReadCStr(skill + MONSTER_SKILL_ID_OFF, id, sizeof id) || !id[0]) continue;
        bool ident = true;
        for (const char* p = id; *p; p++)
            if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
                  (*p >= '0' && *p <= '9') || *p == '_')) { ident = false; break; }
        if (!ident) continue;

        char key[128], name[160];
        _snprintf(key, sizeof key, MT_SKILL_KEY_FMT, id);
        key[sizeof key - 1] = 0;
        if (!resolveKey(base, key, name, sizeof name) || !name[0]) continue;
        abStripMarkup(name);
        if (annTextEq(name, text)) return true;
    }

    if (actor == abSelectedHero(base)) {
        uintptr_t skills[AB_MAX_SKILLS];
        int ns = abSkillList(base, skills, AB_MAX_SKILLS);
        for (int i = 0; i < ns; i++) {
            char name[160];
            if (!abSkillName(base, skills[i], name, sizeof name) || !name[0]) continue;
            if (annTextEq(name, text)) return true;
        }
    }
    return false;
}

// ---- naming the SIDE a surprise banner belongs to ----
static const char* annSurpriseLabel(uintptr_t base, const char* text) {
    char word[128];
    if (!abTipPlain(base, SURPRISE_KEY, word, sizeof word) || !word[0]) return nullptr;
    if (!annTextEq(word, text)) return nullptr;              // some other banner — not ours

    int side = rvSurprisedSide(base, nullptr);
    if (side == SURPRISE_PARTY)    return axs(AXS_CT_SIDE_PARTY);
    if (side == SURPRISE_MONSTERS) return axs(AXS_CT_SIDE_ENEMIES);
    logLine("announce: surprise banner is up but the side flag reads %d — spoken bare", side);
    return nullptr;
}

// ---- the one banner the mod raises ITSELF, and must not speak ----
static bool annPitHoldBanner(uintptr_t base, const char* text) {
    if (!text || !text[0] || !pitInMatch(base)) return false;
    char want[160];
    if (!resolveKey(base, PIT_HOLD_BANNER_KEY, want, sizeof want) || !want[0]) return false;
    abStripMarkup(want);
    return annTextEq(want, text);
}

void serviceAnnouncement(uintptr_t base) {
    uintptr_t rd = (uintptr_t)g_raidDisplay;
    if (!rd || !mapRoot(base)) { g_annSeenN = 0; return; }   // out of a raid: forget the live set

    uintptr_t begin = 0, end = 0;
    if (!safeReadPtr(rd + RD_ANN_BEGIN_OFF, &begin) ||
        !safeReadPtr(rd + RD_ANN_END_OFF,   &end)) { g_annSeenN = 0; return; }
    if (begin < 0x10000 || end < begin) { g_annSeenN = 0; return; }

    uintptr_t span = end - begin;
    int n = (int)(span / ANN_REC_STRIDE);
    if (n <= 0) { g_annSeenN = 0; return; }                  // nothing on screen — the Hide case
    if (n > ANN_MAX_LIVE) {
        logLine("announce: %d records live (cap %d) — reading the first %d", n, ANN_MAX_LIVE, ANN_MAX_LIVE);
        n = ANN_MAX_LIVE;
    }

    AnnRec live[ANN_MAX_LIVE];
    int m = 0;
    for (int i = 0; i < n; i++) {
        char text[CLOG_LINE_MAX];
        if (!annRecText(begin + (uintptr_t)i * ANN_REC_STRIDE, text, sizeof text)) continue;
        live[m].hash = clHash(text);
        strncpy(live[m].text, text, CLOG_LINE_MAX - 1);
        live[m].text[CLOG_LINE_MAX - 1] = 0;
        m++;
    }

    uintptr_t root  = mapRoot(base);
    uintptr_t actor = root ? abCurrentTurnActor(root) : 0;

    for (int i = 0; i < m; i++) {
        // Same slot AND same words = the banner we already spoke, still up.
        if (i < g_annSeenN && g_annSeen[i].hash == live[i].hash) continue;

        char line[CLOG_LINE_MAX];
        char who[160];
        const char* why = "";

        const char* side = annSurpriseLabel(base, live[i].text);
        if (side) {
            _snprintf(line, sizeof line, "%s: %s", side, live[i].text);
            line[sizeof line - 1] = 0;
            why = " [surprise side]";
        } else {
            if (!actor)                                        why = " [no turn actor]";
            else if (!annActorOwnsSkill(base, actor, live[i].text)) why = " [not this actor's skill]";
            else if (!clActorName(base, actor, who, sizeof who) || !who[0]) why = " [actor unnamed]";

            if (!*why) {
                _snprintf(line, sizeof line, "%s: %s", who, live[i].text);
                line[sizeof line - 1] = 0;
            } else {
                strncpy(line, live[i].text, CLOG_LINE_MAX - 1);
                line[CLOG_LINE_MAX - 1] = 0;
            }
        }

        clogAdd(line);
        logLine("announce: slot %d/%d actor=%p%s -> \"%s\"", i + 1, m, (void*)actor, why, line);

        if (annPitHoldBanner(base, live[i].text)) {
            logLine("announce: suppressed the pit's hold-to-activate instruction");
            continue;
        }

        if (!g_clogOpen) postSpeech(line, false);
    }

    g_annSeenN = m;
    for (int i = 0; i < m; i++) g_annSeen[i] = live[i];
}

// ---- BARKS — the speech balloons over heroes and monsters ----
static const uintptr_t RD_BARK_ARR_OFF   = 0x3188;
static const uintptr_t RD_BARK_CAP_OFF   = 0x3190;
static const uintptr_t RD_BARK_HEAD_OFF  = 0x3198;
static const uintptr_t RD_BARK_COUNT_OFF = 0x31a0;

static const uintptr_t BARK_ACTOR_OFF   = 0x00;   // record+: Actor* — the speaker. 0 = not a bark.
static const uintptr_t BARK_TEXT_OFF    = 0x08;   // record+: char[0x80], already localized
static const int       BARK_TEXT_MAX    = 0x80;
static const uintptr_t BARK_ID_OFF      = 0x90;   // record+: globally unique, incrementing
static const uintptr_t BARK_STARTED_OFF = 0xa9;   // record+: driver sets this the frame it draws

static uint64_t g_barkLastId = 0;

void serviceBark(uintptr_t base) {
    uintptr_t rd = (uintptr_t)g_raidDisplay;
    if (!rd || !mapRoot(base)) { g_barkLastId = 0; return; }

    int64_t count = 0, cap = 0, head = 0;
    uintptr_t arr = 0;
    if (!safeReadI64(rd + RD_BARK_COUNT_OFF, &count) || count <= 0) return;
    if (!safeReadI64(rd + RD_BARK_CAP_OFF, &cap) || cap <= 0) return;
    if ((cap & (cap - 1)) != 0) { logLine("bark: ring capacity %lld is not a power of two", (long long)cap); return; }
    if (!safeReadI64(rd + RD_BARK_HEAD_OFF, &head)) return;
    if (!safeReadPtr(rd + RD_BARK_ARR_OFF, &arr) || arr < 0x10000) return;

    uintptr_t front = 0;
    if (!safeReadPtr(arr + (uintptr_t)((head & (cap - 1)) * 8), &front) || front < 0x10000) return;

    uint8_t started = 0;
    if (!safeReadU8(front + BARK_STARTED_OFF, &started) || !started) return;   // queued, not shown

    uint64_t id = 0;
    if (!safeReadI64(front + BARK_ID_OFF, (int64_t*)&id) || !id) return;
    if (id == g_barkLastId) return;
    g_barkLastId = id;

    uintptr_t actor = 0;
    if (!safeReadPtr(front + BARK_ACTOR_OFF, &actor) || actor < 0x10000) return;

    char raw[BARK_TEXT_MAX + 1];
    if (!safeReadCStr(front + BARK_TEXT_OFF, raw, sizeof raw) || !raw[0]) return;

    char flat[BARK_TEXT_MAX + 1];
    int o = 0;
    for (int i = 0; raw[i] && o < (int)sizeof flat - 2; i++) {
        if (raw[i] == '\n' || raw[i] == '\r') {
            if (o > 0 && flat[o - 1] != ' ') { flat[o++] = ' '; }
        } else flat[o++] = raw[i];
    }
    flat[o] = 0;

    char text[CLOG_LINE_MAX];
    stripMarkup(flat, text, sizeof text);
    if (!text[0]) return;

    char line[CLOG_LINE_MAX];
    char who[160];
    if (clActorName(base, actor, who, sizeof who) && who[0]) {
        _snprintf(line, sizeof line, "%s: %s", who, text);
        line[sizeof line - 1] = 0;
    } else {
        strncpy(line, text, CLOG_LINE_MAX - 1);
        line[CLOG_LINE_MAX - 1] = 0;
        logLine("bark: actor=%p not in either roster — speaking unnamed", (void*)actor);
    }

    clogAdd(line);
    logLine("bark: id=%llu actor=%p -> \"%s\"", (unsigned long long)id, (void*)actor, line);

    if (!axReadDungeonBarks()) { logLine("bark: not spoken (switched off), logged"); return; }
    if (!g_clogOpen) postSpeech(line, false);
}

// ---- the log view ----
static void clogSpeakCursor() {
    const char* s = clogAt(g_clogCursor);
    if (!s) { postSpeech(axs(AXS_CLOG_EMPTY)); return; }
    char pos[64];
    _snprintf(pos, sizeof pos, axs(AXS_POS_N_OF_M), g_clogCursor + 1, g_clogCount);
    pos[sizeof pos - 1] = 0;
    char msg[CLOG_LINE_MAX + 128];
    _snprintf(msg, sizeof msg, clEndsSentence(s) ? "%s %s" : "%s. %s", s, pos);
    msg[sizeof msg - 1] = 0;
    postSpeech(msg);
}

void clogSetOpen(bool on) {
    g_clogOpen = on;
    if (on) g_clogCursor = 0;   // always open on the NEWEST line
    logLine("combatlog: %s (%d lines)", on ? "opened" : "closed", g_clogCount);
}

bool routeLogKey(uintptr_t base, uint32_t sym, uint16_t mod, uint8_t repeat) {   // (de-static'd
    (void)base; (void)mod;
    if (sym == SDLK_ESCAPE) {
        if (!repeat) { clogSetOpen(false); postSpeech(axs(AXS_CLOG_CLOSED)); }
        return true;
    }
    {
        int jump = 0;
        if (axDecodeJump(sym, mod, repeat, &jump)) {
            if (!jump) return true;                         // held jump: one landing per press
            if (g_clogCount == 0) { postSpeech(axs(AXS_CLOG_EMPTY)); return true; }
            axStepCursor(&g_clogCursor, g_clogCount, -jump);
            clogSpeakCursor();
            return true;
        }
    }
    if (sym != SDLK_UP && sym != SDLK_DOWN) return false;   // everything else stays the game's
    if (axNavHoldRepeat(repeat)) return true;               // throttled repeat: claimed, no step

    if (g_clogCount == 0) { postSpeech(axs(AXS_CLOG_EMPTY)); return true; }
    axStepCursor(&g_clogCursor, g_clogCount, sym == SDLK_UP ? 1 : -1);   // Up = older = +1
    clogSpeakCursor();
    return true;
}

static void clogKeyGateCheck(uintptr_t base) {
    static bool done = false;
    if (done) return;
    done = true;
    char owner[0x44];
    if (kbActionForKey(base, (int)SDLK_PERIOD, owner, sizeof owner))
        logLine("combatlog GATE: '.' is BOUND to the game action \"%s\" — claiming it takes it away", owner);
    else
        logLine("combatlog GATE: '.' is not in the keyboard binding table — safe to claim");
}

bool routeLogToggle(uintptr_t base, uint32_t sym, uint8_t repeat) {
                                             // is a GLOBAL key of routeInputEvent's pre-dispatch)
    if (sym != SDLK_PERIOD) return false;
    if (repeat) return true;                   // a held key must not flap the context

    clogKeyGateCheck(base);

    if (g_clogOpen) { clogSetOpen(false); postSpeech(axs(AXS_CLOG_CLOSED)); return true; }

    clogSetOpen(true);
    if (g_clogCount == 0) { postSpeech(axs(AXS_CLOG_OPEN_EMPTY)); return true; }
    char head[128], pos[64];
    _snprintf(head, sizeof head, axs(AXS_CLOG_OPEN_FMT), g_clogCount);
    head[sizeof head - 1] = 0;
    _snprintf(pos, sizeof pos, axs(AXS_POS_N_OF_M), 1, g_clogCount);
    pos[sizeof pos - 1] = 0;
    char msg[CLOG_LINE_MAX + 256];
    const char* s = clogAt(0);
    _snprintf(msg, sizeof msg, (s && clEndsSentence(s)) ? "%s %s %s" : "%s %s. %s",
              head, s ? s : "", pos);
    msg[sizeof msg - 1] = 0;
    postSpeech(msg);
    return true;
}
