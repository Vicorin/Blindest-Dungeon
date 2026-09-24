// town/map.cpp -- the second TOWN slice

#include <cstdint>
#include <cstring>
#include <cstdio>
#include "internal.h"

// ---- TOWN MAP BUILDINGS (hamlet overview) — Phase-1 read-only probe ----

static int  g_tmbProbePass = 0;
static bool g_tmbPrevBare  = false;  // edge detector for the gate

static bool tmbGateSkips(int g) { return g <= 11 && ((TMB_GATE_SKIP >> g) & 1) != 0; }

uintptr_t tmbOwner(uintptr_t root) {
    uintptr_t owner = 0;
    if (!safeReadPtr(root + TMB_OWNER_OFF, &owner) || owner <= 0x10000) return 0;
    return owner;
}

int tmbGroup(uintptr_t owner, int g, uintptr_t* beg) {
    uintptr_t vec = owner + (uintptr_t)g * TMB_GROUP_STRIDE, b = 0, e = 0;
    *beg = 0;
    if (!safeReadPtr(vec, &b) || !safeReadPtr(vec + 8, &e) || e < b || (e - b) % 8) return 0;
    int n = (int)((e - b) / 8);
    if (n < 0 || n > 64) return 0;
    *beg = b;
    return n;
}

bool tmbBareMap(uintptr_t root) {
    uintptr_t owner = tmbOwner(root);
    if (!owner) return false;
    for (int g = 0; g < TMB_GROUP_COUNT; g++) {
        if (tmbGateSkips(g)) continue;
        uintptr_t beg = 0;
        int n = tmbGroup(owner, g, &beg);
        for (int i = 0; i < n; i++) {
            uintptr_t panel = 0;
            if (!safeReadPtr(beg + (uintptr_t)i * 8, &panel) || panel <= 0x10000) continue;
            uint32_t shown = 0;
            if (safeReadU32(panel + TMB_SHOWN_OFF, &shown) && shown != 0) return false;
        }
    }
    return true;
}

static int tmbCountPanels(uintptr_t root) {
    uintptr_t owner = tmbOwner(root);
    if (!owner) return 0;
    int total = 0;
    for (int g = 0; g < TMB_GROUP_COUNT; g++) {
        uintptr_t beg = 0;
        total += tmbGroup(owner, g, &beg);
    }
    return total;
}

bool tmbClassName(uintptr_t base, uintptr_t vft, char* out, int outsz) {
    out[0] = 0;
    if (vft <= base || vft - base > 0x3000000) return false;
    uintptr_t col = 0;
    if (!safeReadPtr(vft - 8, &col) || col <= base || col - base > 0x3000000) return false;
    uint32_t sig = 0, tdRva = 0;
    if (!safeReadU32(col, &sig) || sig != 1) return false;
    if (!safeReadU32(col + 0x0c, &tdRva) || !tdRva || tdRva > 0x3000000) return false;
    return safeReadCStr(base + tdRva + 0x10, out, outsz) && out[0] == '.';
}

static int tmbTextLen(const char* s) {
    int i = 0, ascii = 0;
    for (; s[i]; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c < 0x20) return -1;
        if (c <= 0x7e) ascii++;
    }
    return ascii >= 3 ? i : -1;
}

static void tmbScanStrings(int pass, uintptr_t obj, uintptr_t lo, uintptr_t hi, int maxHits,
                           const char* tag) {
    int hits = 0;
    for (uintptr_t off = lo; off < hi && hits < maxHits; off += 8) {
        char s[120];
        int len;
        if (safeReadCStr(obj + off, s, sizeof s) && (len = tmbTextLen(s)) >= 4) {
            logLine("townmap probe(%d):   %s +0x%llx inline \"%s\"",
                    pass, tag, (unsigned long long)off, s);
            hits++;
            off += ((uintptr_t)len / 8) * 8;   // don't re-log the tail of the same run
            continue;
        }
        uintptr_t p = 0;
        if (safeReadPtr(obj + off, &p) && p > 0x10000 &&
            safeReadCStr(p, s, sizeof s) && tmbTextLen(s) >= 3) {
            logLine("townmap probe(%d):   %s +0x%llx -> \"%s\"",
                    pass, tag, (unsigned long long)off, s);
            hits++;
        }
    }
}

void tmbIdChars(uint32_t v, char out[10]) {
    out[0] = 0;
    char c[4] = { (char)(v & 0xff), (char)((v >> 8) & 0xff),
                  (char)((v >> 16) & 0xff), (char)(v >> 24) };
    for (int i = 0; i < 4; i++)
        if ((unsigned char)c[i] < 0x20 || (unsigned char)c[i] > 0x7e) return;
    _snprintf(out, 10, " '%c%c%c%c'", c[0], c[1], c[2], c[3]);
}

static void tmbProbe(uintptr_t base, uintptr_t root, int pass) {
    uintptr_t tracked[160];
    int ntracked = 0;
    uintptr_t owner = tmbOwner(root);
    logLine("townmap probe(%d): start root=%p owner=%p", pass, (void*)root, (void*)owner);
    if (!owner) { logLine("townmap probe(%d): end (no owner)", pass); return; }

    for (int g = 0; g < TMB_GROUP_COUNT; g++) {
        uintptr_t beg = 0;
        int n = tmbGroup(owner, g, &beg);
        logLine("townmap probe(%d): group %d%s: %d panels",
                pass, g, tmbGateSkips(g) ? " (gate-skipped)" : "", n);
        for (int i = 0; i < n; i++) {
            uintptr_t panel = 0;
            if (!safeReadPtr(beg + (uintptr_t)i * 8, &panel) || panel <= 0x10000) continue;
            uintptr_t vft = 0;
            safeReadPtr(panel, &vft);
            uint32_t shown = 0xffffffff;
            safeReadU32(panel + TMB_SHOWN_OFF, &shown);
            char cls[96];
            tmbClassName(base, vft, cls, sizeof cls);
            char tag[24];
            _snprintf(tag, sizeof tag, "g%d[%d]", g, i);
            tag[sizeof tag - 1] = 0;
            logLine("townmap probe(%d): %s panel=%p vft=+0x%llx %s shown=%u",
                    pass, tag, (void*)panel,
                    (unsigned long long)(vft > base ? vft - base : vft),
                    cls[0] ? cls : "(no rtti)", shown);
            if (ntracked < 160) tracked[ntracked++] = panel;
            tmbScanStrings(pass, panel, 0x8, 0x300, 12, tag);

            for (int cv = 0; cv < 2; cv++) {
                uintptr_t coff = cv ? 0x118 : 0x100;
                uintptr_t cb = 0, ce = 0;
                if (!safeReadPtr(panel + coff, &cb) || !safeReadPtr(panel + coff + 8, &ce) ||
                    ce < cb || (ce - cb) % 8) continue;
                int cn = (int)((ce - cb) / 8);
                if (cn <= 0 || cn > 32) continue;
                for (int c = 0; c < cn; c++) {
                    uintptr_t kid = 0, kvft = 0;
                    if (!safeReadPtr(cb + (uintptr_t)c * 8, &kid) || kid <= 0x10000) continue;
                    if (!safeReadPtr(kid, &kvft)) continue;
                    uint32_t kshown = 0xffffffff;
                    safeReadU32(kid + TMB_SHOWN_OFF, &kshown);
                    char kcls[96];
                    tmbClassName(base, kvft, kcls, sizeof kcls);
                    char ktag[32];
                    _snprintf(ktag, sizeof ktag, "g%d[%d].%c[%d]", g, i, cv ? 'B' : 'A', c);
                    ktag[sizeof ktag - 1] = 0;
                    logLine("townmap probe(%d): %s kid=%p vft=+0x%llx %s shown=%u",
                            pass, ktag, (void*)kid,
                            (unsigned long long)(kvft > base ? kvft - base : kvft),
                            kcls[0] ? kcls : "(no rtti)", kshown);
                    if (ntracked < 160) tracked[ntracked++] = kid;
                    tmbScanStrings(pass, kid, 0x8, 0x180, 8, ktag);
                }
            }
        }
    }

    uintptr_t fb = 0, fe = 0;
    if (safeReadPtr(base + VEC_BEGIN_RVA, &fb) && safeReadPtr(base + VEC_END_RVA, &fe) &&
        fb && fe > fb) {
        uintptr_t count = (fe - fb) / ELEM_STRIDE;
        if (count > 512) count = 512;
        logLine("townmap probe(%d): focus vector, %llu elements", pass, (unsigned long long)count);
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
            safeReadU32(elem + ELEM_POS_OFF, &xb);
            safeReadU32(elem + ELEM_POS_OFF + 4, &yb);
            safeReadU32(elem + ELEM_SIZE_OFF, &wb);
            safeReadU32(elem + ELEM_SIZE_OFF + 4, &hb);
            char chars[10];
            tmbIdChars((uint32_t)(uint64_t)id, chars);
            uint32_t rel = (uint32_t)(uint64_t)id - TMB_BLD_ID_BASE;
            logLine("townmap probe(%d): elem id=0x%llx%s owner=%p pos=(%.0f,%.0f) "
                    "size=(%.0f,%.0f)%s%s",
                    pass, (unsigned long long)id, chars, (void*)eowner,
                    u32AsFloatM(xb), u32AsFloatM(yb), u32AsFloatM(wb), u32AsFloatM(hb),
                    mine ? " <-- town panel" : "", rel < 64 ? " <-- 'bld' family" : "");
        }
    } else {
        logLine("townmap probe(%d): focus vector empty", pass);
    }
    logLine("townmap probe(%d): end", pass);
}

void checkTownBuildings(uintptr_t base) {
    uintptr_t root = resTownRoot(base);
    if (!root) { g_tmbPrevBare = false; return; }
    bool bare = tmbBareMap(root);
    if (bare != g_tmbPrevBare) {
        g_tmbPrevBare = bare;
        logLine("townmap: bare-map gate -> %d", bare ? 1 : 0);
        if (axDebugLogEnabled() && bare && g_tmbProbePass == 1) {   // diagnostic: log-gated
            g_tmbProbePass = 2;
            tmbProbe(base, root, 2);
        }
    }
    if (axDebugLogEnabled() && bare && g_tmbProbePass == 0 && tmbCountPanels(root) > 0) {
        g_tmbProbePass = 1;
        tmbProbe(base, root, 1);
    }
}

// ---- TOWN: the MAP BUILDINGS list (AX_TOWNMAP) ----
TmRow g_tmRows[TM_MAX_ROWS];
static int   g_tmCount  = 0;
int          g_tmRow    = 0;
static bool  g_tmActive = false;     // maintained by checkTownMap (edge-announced)
static bool  g_tmDumped = false;     // the one-shot collect log has fired this session
static int   g_tmLayer  = -1;        // last seen town layer; -1 = not in town / unknown

DWORD g_tmOpenWatchUntil = 0;        // 0 = idle
char  g_tmOpenName[TM_NAME_MAX];
bool  g_tmOpenIsDistrict = false;
bool         g_tmOpenIsDdis     = false;
static int   g_tmWarnBudget = 8;           // collect runs per keypress: cap the "something is
                                           // wrong" lines so a bad read can't flood the log

bool axIsTownMap() { return g_tmActive; }

bool tmIdLooksValid(const char* s) {
    int n = 0;
    for (; s[n]; n++) {
        char c = s[n];
        if (!((c >= 'a' && c <= 'z') || c == '_')) return false;
        if (n >= 24) return false;
    }
    return n >= 3;
}

// ---- the Districts opener: an ESTATE-BAR button, not a map object (pass 75) ----

// ---- "(new)": the exclamation mark the game paints over a location you have never opened ----
static const uintptr_t TM_VFT_ALERT_OFF   = 0x20;      // vftable slot 4 — "wants an exclamation mark"
typedef uint8_t (*TmWantsAlertFn)(uintptr_t obj);
static bool g_tmAlertVftWarned = false;                // the identity check complains once, not per row

static bool tmIsNew(uintptr_t base, uintptr_t obj, uintptr_t def, bool unlocked) {
    if (!unlocked || !def) return false;
    uintptr_t vft = 0, fn = 0;
    if (!safeReadPtr(obj, &vft)) return false;
    if (vft != base + TM_BLD_VFTABLE_RVA) {
        if (!g_tmAlertVftWarned) {
            g_tmAlertVftWarned = true;
            logLine("townmap: map object vftable 0x%llx is not Panel::Buildings (expected +0x%llx)"
                    " -> no \"(new)\" marks", (unsigned long long)(vft - base),
                    (unsigned long long)TM_BLD_VFTABLE_RVA);
        }
        return false;
    }
    if (!safeReadPtr(vft + TM_VFT_ALERT_OFF, &fn) || fn <= 0x10000) return false;
    uint8_t wants = 0;
    __try { wants = ((TmWantsAlertFn)fn)(obj); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    return wants != 0;
}

int tmCurrentLayer(uintptr_t root) {
    uint32_t layer = 0;
    if (!root || !safeReadU32(root + TM_LAYER_OFF, &layer)) return -1;
    if (layer > 8) return -1;                        // eTownLayer is 0..4; more = a bad read
    return (int)layer;
}

static bool tmArenaDlcEnabled(uintptr_t base) {
    uint8_t on = 1;
    if (!safeReadU8(base + TM_ARENA_DLC_RVA, &on)) return true;
    return on != 0;
}

struct TmDistrict { const char* id; int layer; AxStrId fallback; };
static const TmDistrict TM_DISTRICTS[] = {
    { "hamlet", 0, AXS_TM_FB_HAMLET },   // the estate map
    { "circus", 1, AXS_TM_FB_CIRCUS },   // the Circus grounds (arena_mp DLC)
    { "fight",  2, AXS_TM_FB_RING   },   // the circus pit -> matchmaking
};
static const int TM_DISTRICT_COUNT = (int)(sizeof TM_DISTRICTS / sizeof TM_DISTRICTS[0]);

// Which district this id opens, or -1 for an ordinary building.
static int tmDistrictIndex(const char* id) {
    for (int i = 0; i < TM_DISTRICT_COUNT; i++)
        if (strcmp(id, TM_DISTRICTS[i].id) == 0) return i;
    return -1;
}

static void tmLayerName(uintptr_t base, int layer, char* out, int outsz) {
    out[0] = 0;
    for (int i = 0; i < TM_DISTRICT_COUNT; i++) {
        if (TM_DISTRICTS[i].layer != layer) continue;
        char key[64];
        _snprintf(key, sizeof key, "town_name_%s", TM_DISTRICTS[i].id);
        key[sizeof key - 1] = 0;
        if (!resolveKey(base, key, out, outsz) || !out[0]) {
            strncpy(out, axs(TM_DISTRICTS[i].fallback), outsz - 1);
            out[outsz - 1] = 0;
        }
        return;
    }
    _snprintf(out, outsz, axs(AXS_TM_FB_DISTRICT_N), layer);   // a layer the table doesn't know: say which
    out[outsz - 1] = 0;
}

static bool tmRowBefore(const TmRow* a, const TmRow* b) {
    bool aBack = !a->unlocked || a->offSave, bBack = !b->unlocked || b->offSave;
    if (aBack != bBack) return !aBack;
    return _stricmp(a->name, b->name) < 0;
}

static int tmCollect(uintptr_t base, bool probe) {
    g_tmCount = 0;
    uintptr_t root = resTownRoot(base);
    if (!root) return 0;
    uintptr_t beg = 0, end = 0;
    if (!safeReadPtr(root + TM_BLD_VEC_BEG, &beg) || !safeReadPtr(root + TM_BLD_VEC_END, &end) ||
        !beg || end < beg || (end - beg) % 8) {
        if (probe) logLine("townmap rows: the layer object vector is unreadable");
        return 0;
    }
    uintptr_t n = (end - beg) / 8;
    if (n > 64) {                                    // a layer holds ~11; more = a bad read
        if (g_tmWarnBudget > 0) {
            g_tmWarnBudget--;
            logLine("townmap rows: implausible object count %llu -> no rows",
                    (unsigned long long)n);
        }
        return 0;
    }
    bool arena = tmArenaDlcEnabled(base);
    if (probe) logLine("townmap rows: layer vector holds %llu objects", (unsigned long long)n);
    for (uintptr_t i = 0; i < n && g_tmCount < TM_MAX_ROWS; i++) {
        uintptr_t obj = 0, def = 0;
        if (!safeReadPtr(beg + i * 8, &obj) || obj <= 0x10000) {
            if (probe) logLine("townmap rows: object %llu is null/unreadable -> skipped",
                               (unsigned long long)i);
            continue;
        }
        if (!safeReadPtr(obj + TM_BLD_DEF_OFF, &def) || def <= 0x10000) {
            if (probe) logLine("townmap rows: object %llu has no BuildingType -> not a location",
                               (unsigned long long)i);
            continue;
        }

        TmRow row;
        row.district = false;
        row.offSave  = false;
        row.isNew    = false;
        if (!safeReadCStr(def + TM_DEF_ID_OFF, row.id, sizeof row.id) || !tmIdLooksValid(row.id)) {
            if (probe) logLine("townmap rows: object %llu has no plausible id -> skipped",
                               (unsigned long long)i);
            continue;
        }
        uint8_t unlocked = 0, hidden = 0, screen = 0;
        bool lockRead = safeReadU8(obj + TM_BLD_UNLOCKED_OFF, &unlocked);
        safeReadU8(obj + TM_BLD_HIDDEN_OFF, &hidden);
        safeReadU8(def + TM_DEF_SCREEN_OFF, &screen);
        if (!lockRead) {                             // never guess "Locked" off a failed read
            if (g_tmWarnBudget > 0) {
                g_tmWarnBudget--;
                logLine("townmap rows: \"%s\" lock state unreadable -> treating as unlocked",
                        row.id);
            }
            unlocked = 1;
        }
        if (hidden && !unlocked) {
            if (probe) logLine("townmap rows: \"%s\" is hidden while locked -> skipped", row.id);
            continue;                                // the game doesn't draw it; neither do we
        }
        row.unlocked = unlocked != 0;

        int di = tmDistrictIndex(row.id);
        row.district = di >= 0;
        if (di >= 0 && TM_DISTRICTS[di].layer == 1 && !arena)
            row.offSave = true;                      // the Circus is not enabled on this save

        row.isNew = tmIsNew(base, obj, def, row.unlocked);

        uint32_t idHash = 0;
        if (!safeReadU32(def + TM_DEF_HASH_OFF, &idHash) || idHash != resHash(row.id)) {
            if (g_tmWarnBudget > 0) {
                g_tmWarnBudget--;
                logLine("townmap rows: \"%s\" hash mismatch (game 0x%x, mine 0x%x) -> skipped",
                        row.id, idHash, resHash(row.id));
            }
            continue;
        }
        row.elemId = idHash + TMB_BLD_ID_BASE;

        char key[64];
        _snprintf(key, sizeof key, "town_name_%s", row.id);
        key[sizeof key - 1] = 0;
        if (!resolveKey(base, key, row.name, sizeof row.name) || !row.name[0]) {
            if (di >= 0) {                           // a district always has a spoken name
                strncpy(row.name, axs(TM_DISTRICTS[di].fallback), sizeof row.name - 1);
                row.name[sizeof row.name - 1] = 0;
            } else {
                strncpy(row.name, row.id, sizeof row.name - 1);
                row.name[sizeof row.name - 1] = 0;
                for (char* p = row.name; *p; p++) if (*p == '_') *p = ' ';
            }
            if (probe) logLine("townmap rows: no localization for \"%s\" -> speaking \"%s\"",
                               key, row.name);
        }
        if (probe) logLine("townmap rows: [%llu] id=\"%s\" elem=0x%x unlocked=%d screen=%u "
                           "district=%d offsave=%d new=%d name=\"%s\"",
                           (unsigned long long)i, row.id, row.elemId, row.unlocked ? 1 : 0,
                           screen, row.district ? 1 : 0, row.offSave ? 1 : 0, row.isNew ? 1 : 0,
                           row.name);

        // insertion sort: the list is tiny and the order is part of the feature
        int at = g_tmCount;
        while (at > 0 && tmRowBefore(&row, &g_tmRows[at - 1])) {
            g_tmRows[at] = g_tmRows[at - 1];
            at--;
        }
        g_tmRows[at] = row;
        g_tmCount++;
    }

    uintptr_t camp = 0;
    uint8_t dEnabled = 0;
    if (tmCurrentLayer(root) == 0 && g_tmCount < TM_MAX_ROWS &&
        safeReadPtr(base + RES_CAMPAIGN_RVA, &camp) && camp > 0x10000 &&
        safeReadU8(camp + TM_CAMP_DISTRICTS_ENABLED, &dEnabled) && dEnabled) {
        TmRow row;
        row.district = false;                        // opens a screen, not a layer
        row.offSave  = false;
        row.isNew    = false;                        // no map object, so no novelty predicate to ask
        strncpy(row.id, "district", sizeof row.id - 1);
        row.id[sizeof row.id - 1] = 0;
        uint8_t dUnlocked = 0, dOverride = 0;
        safeReadU8(camp + TM_CAMP_DISTRICTS_UNLOCKED, &dUnlocked);
        safeReadU8(base + TM_DDIS_OVERRIDE_RVA, &dOverride);
        row.unlocked = dUnlocked != 0 || dOverride != 0;   // the game's own OR, mirrored
        row.elemId   = TM_DDIS_ELEM_ID;
        if (!resolveKey(base, "town_name_district", row.name, sizeof row.name) || !row.name[0]) {
            strncpy(row.name, "district", sizeof row.name - 1);
            row.name[sizeof row.name - 1] = 0;
            if (probe) logLine("townmap rows: no localization for \"town_name_district\" -> "
                               "speaking the raw id");
        }
        if (probe) logLine("townmap rows: [ddis] estate-bar districts button unlocked=%d name=\"%s\"",
                           row.unlocked ? 1 : 0, row.name);
        g_tmRows[g_tmCount++] = row;                 // appended AFTER the sorted rows: always last
    }
    return g_tmCount;
}

static int tmCollectFromPanels(uintptr_t base) {
    g_tmCount = 0;
    uintptr_t root = resTownRoot(base);
    if (!root) return 0;
    uintptr_t owner = tmbOwner(root);
    if (!owner) return 0;
    uintptr_t beg = 0;
    int n = tmbGroup(owner, 3, &beg);
    for (int i = 0; i < n && g_tmCount < TM_MAX_ROWS; i++) {
        uintptr_t panel = 0, vft = 0;
        if (!safeReadPtr(beg + (uintptr_t)i * 8, &panel) || panel <= 0x10000) continue;
        safeReadPtr(panel, &vft);
        char cls[96];
        tmbClassName(base, vft, cls, sizeof cls);
        if (strstr(cls, "EmptyBuilding")) continue;      // map furniture, not a building

        TmRow row;
        row.district = false;
        row.offSave  = false;
        row.unlocked = true;
        row.isNew    = false;                            // no map object here, so no predicate to ask
        if (!safeReadCStr(panel + 0xa4, row.id, sizeof row.id) || !tmIdLooksValid(row.id)) continue;
        row.elemId = resHash(row.id) + TMB_BLD_ID_BASE;
        char key[64];
        _snprintf(key, sizeof key, "town_name_%s", row.id);
        key[sizeof key - 1] = 0;
        if (!resolveKey(base, key, row.name, sizeof row.name) || !row.name[0]) {
            strncpy(row.name, row.id, sizeof row.name - 1);
            row.name[sizeof row.name - 1] = 0;
            for (char* p = row.name; *p; p++) if (*p == '_') *p = ' ';
        }
        int at = g_tmCount;
        while (at > 0 && tmRowBefore(&row, &g_tmRows[at - 1])) {
            g_tmRows[at] = g_tmRows[at - 1];
            at--;
        }
        g_tmRows[at] = row;
        g_tmCount++;
    }
    if (g_tmCount)
        logLine("townmap: FELL BACK to the panel roster -> %d rows (no district rows, and rows "
                "from other districts are possible)", g_tmCount);
    return g_tmCount;
}

// The rows, from the layer vector, with the old panel walk as a last resort.
int tmRows(uintptr_t base, bool probe) {
    int n = tmCollect(base, probe);
    if (n > 0) return n;
    return tmCollectFromPanels(base);
}

static void tmRowText(const TmRow* r, char* out, int outsz) {
    const char* state = !r->unlocked ? axs(AXS_TM_ROW_LOCKED)
                      : (r->offSave  ? axs(AXS_TM_ROW_OFF_SAVE) : nullptr);
    _snprintf(out, outsz, r->isNew ? axs(AXS_TM_ROW_NEW_FMT) : "%s.", r->name);
    out[outsz - 1] = 0;
    int len = (int)strlen(out);
    if (state && len < outsz - 1) {
        _snprintf(out + len, outsz - len, " %s", state);
        out[outsz - 1] = 0;
    }
}

static void tmSpeakRow(uintptr_t base, const char* prefix) {
    int n = tmRows(base, false);
    if (n <= 0) { postSpeech(axs(AXS_TM_NO_LOCATIONS)); return; }
    if (g_tmRow >= n) g_tmRow = n - 1;
    if (g_tmRow < 0)  g_tmRow = 0;
    char row[TM_NAME_MAX + 64];
    tmRowText(&g_tmRows[g_tmRow], row, sizeof row);
    char pos[48];
    _snprintf(pos, sizeof pos, axs(AXS_POS_N_OF_M), g_tmRow + 1, n);
    pos[sizeof pos - 1] = 0;
    char utter[MAILBOX_SZ];
    _snprintf(utter, sizeof utter, "%s%s %s", prefix ? prefix : "", row, pos);
    utter[sizeof utter - 1] = 0;
    postSpeech(utter);
}

// ---- the location's hover tooltip, as a Ctrl+Up/Down buffer ----
static const int TM_TIP_MAX     = 4;
static const int TM_TIP_LINE_SZ = 512;
int g_tmTipLine = 0;                        // buffer cursor; 0 = the row itself

static int tmTipLines(uintptr_t base, const TmRow* r, char lines[][TM_TIP_LINE_SZ], int max) {
    int n = 0;
    char key[128], raw[TM_TIP_LINE_SZ];
    if (n < max) {
        tmRowText(r, lines[n], TM_TIP_LINE_SZ);
        n++;
    }
    if (n < max) {
        if (r->unlocked) {
            _snprintf(key, sizeof key, "str_%s_summary", r->id);
            key[sizeof key - 1] = 0;
        } else if (r->elemId == TM_DDIS_ELEM_ID) {
            strncpy(key, "str_districts_locked_tooltip", sizeof key - 1);
            key[sizeof key - 1] = 0;
        } else {
            strncpy(key, "str_locked_building_summary", sizeof key - 1);
            key[sizeof key - 1] = 0;
        }
        if (resolveKey(base, key, raw, sizeof raw) && raw[0]) {
            stripMarkup(raw, lines[n], TM_TIP_LINE_SZ);
            if (lines[n][0]) n++;
        } else {
            logLine("townmap tip: no localization for \"%s\" -> line skipped", key);
        }
    }
    if (n < max) {
        _snprintf(key, sizeof key, "building_verbose_%s", r->id);
        key[sizeof key - 1] = 0;
        if (resolveKey(base, key, raw, sizeof raw) && raw[0]) {
            stripMarkup(raw, lines[n], TM_TIP_LINE_SZ);
            if (lines[n][0]) n++;
        }
    }
    return n;
}

static void tmSurfacePrefix(uintptr_t base, uintptr_t root, char* out, int outsz) {
    int layer = tmCurrentLayer(root);
    if (layer <= 0) {                                // 0 = the hamlet; -1 = unreadable, same words
        _snprintf(out, outsz, "%s ", axs(AXS_TM_TITLE_HAMLET));   // trailing space at the call site
        out[outsz - 1] = 0;
        return;
    }
    char name[TM_NAME_MAX], title[TM_NAME_MAX + 32];
    tmLayerName(base, layer, name, sizeof name);
    _snprintf(title, sizeof title, axs(AXS_TM_TITLE_FMT), name);
    title[sizeof title - 1] = 0;
    _snprintf(out, outsz, "%s ", title);
    out[outsz - 1] = 0;
}

void tmReannounce(uintptr_t base) {
    if (!g_tmActive) return;
    logLine("townmap: re-announcing after a modal closed");
    char prefix[TM_NAME_MAX + 16];
    tmSurfacePrefix(base, resTownRoot(base), prefix, sizeof prefix);
    tmSpeakRow(base, prefix);
}

void tmOpenRow(uintptr_t base, const TmRow* r) {
    char utter[192];
    if (!r->unlocked) {
        _snprintf(utter, sizeof utter, axs(AXS_TJ_LOCKED), r->name);
        utter[sizeof utter - 1] = 0;
        postSpeech(utter);
        return;
    }
    if (g_tmOpenWatchUntil) { postSpeech(axs(AXS_TJ_STILL_OPENING)); return; }
    if (frontEndClickElementId((int64_t)r->elemId)) {
        strncpy(g_tmOpenName, r->name, sizeof g_tmOpenName - 1);
        g_tmOpenName[sizeof g_tmOpenName - 1] = 0;
        g_tmOpenIsDistrict = r->district;
        g_tmOpenIsDdis     = r->elemId == TM_DDIS_ELEM_ID;
        g_tmOpenWatchUntil = GetTickCount() + (r->district ? 6000 : 1500);
        logLine("townmap: open \"%s\"%s -> clicked element 0x%x", r->id,
                r->district ? " (district)" : "", r->elemId);
    } else {
        logInputSnapshot(base, "townmap-cant-open");
        diagDumpFocusElements(base, "townmap-cant-open");
        _snprintf(utter, sizeof utter, axs(AXS_TJ_CANT_OPEN), r->name);
        utter[sizeof utter - 1] = 0;
        postSpeech(utter);
    }
}

// ---- the TOWN JUMP KEYS: one letter, one location ----
const TjKey TJ_KEYS[] = {
    { SDLK_a, "abbey"           },
    { SDLK_b, "blacksmith"      },
    { SDLK_c, "stage_coach"     },
    { SDLK_d, "district"        },   // the estate bar's Districts screen ('ddis')
    { SDLK_l, "guild"           },
    { SDLK_m, "statue"          },   // the Ancestor's Memoirs
    { SDLK_s, "sanitarium"      },
    { SDLK_t, "tavern"          },
    { SDLK_v, "camping_trainer" },   // the Survivalist
    { SDLK_w, "nomad_wagon"     },
    { SDLK_y, "graveyard"       },
    { SDLK_z, "circus"          },   // travel to the Butcher's Circus grounds
};
const int TJ_KEY_COUNT = (int)(sizeof TJ_KEYS / sizeof TJ_KEYS[0]);

const char* tjTargetForKey(uint32_t sym) {
    for (int i = 0; i < TJ_KEY_COUNT; i++) if (TJ_KEYS[i].sym == sym) return TJ_KEYS[i].id;
    return nullptr;
}

void tjTargetName(uintptr_t base, const char* id, char* out, int outsz) {
    char key[64];
    _snprintf(key, sizeof key, "town_name_%s", id);
    key[sizeof key - 1] = 0;
    if (resolveKey(base, key, out, outsz) && out[0]) return;
    int di = tmDistrictIndex(id);
    if (di >= 0) {
        strncpy(out, axs(TM_DISTRICTS[di].fallback), outsz - 1);
        out[outsz - 1] = 0;
        return;
    }
    strncpy(out, id, outsz - 1);
    out[outsz - 1] = 0;
    for (char* p = out; *p; p++) if (*p == '_') *p = ' ';
}

char  g_tjChainId[28] = {0};          // the target row id; empty = idle
DWORD g_tjChainUntil  = 0;            // give up waiting for the bare map
DWORD g_tjChainDue    = 0;

DWORD    g_bnSwitchUntil = 0;
uint32_t g_bnSwitchHash  = 0;
char     g_bnSwitchName[TM_NAME_MAX] = {0};

static void tmHoverRow(const TmRow* r) {
    if (!r || !r->elemId) return;
    uintptr_t elem = feGetElementById((int64_t)r->elemId);
    float fx = 0, fy = 0;
    if (elem && elemCenter(elem, &fx, &fy)) moveCursorTo(fx, fy);
}

bool routeTownMapKey(uintptr_t base, uint32_t sym, uint16_t mod, uint8_t repeat) {
    if (mod & (KMOD_LALT | KMOD_RALT)) return false;
    bool ctrl = (mod & (KMOD_LCTRL | KMOD_RCTRL)) != 0;

    if (ctrl && (sym == SDLK_UP || sym == SDLK_DOWN)) {
        if (repeat) return true;
        int n = tmRows(base, false);
        if (n <= 0) { postSpeech(axs(AXS_TM_NO_LOCATIONS)); return true; }
        if (g_tmRow >= n) g_tmRow = n - 1;
        if (g_tmRow < 0)  g_tmRow = 0;
        static char lines[TM_TIP_MAX][TM_TIP_LINE_SZ];
        int ln = tmTipLines(base, &g_tmRows[g_tmRow], lines, TM_TIP_MAX);
        if (ln <= 1) { postSpeech(axs(AXS_NO_MORE_INFO)); return true; }
        if (g_tmTipLine < 0 || g_tmTipLine >= ln) g_tmTipLine = 0;
        g_tmTipLine = (g_tmTipLine + ((sym == SDLK_UP) ? 1 : -1) + ln) % ln;
        postSpeech(lines[g_tmTipLine]);
        return true;
    }
    if (ctrl) return false;

    if (sym == SDLK_RETURN || sym == SDLK_KP_ENTER) {
        if (repeat) return true;
        int n = tmRows(base, false);
        if (n <= 0) return true;
        if (g_tmRow >= n) g_tmRow = n - 1;
        if (g_tmRow < 0)  g_tmRow = 0;
        tmOpenRow(base, &g_tmRows[g_tmRow]);   // the shared open body (jump letters use it too)
        return true;
    }

    int dRow = 0, dCol = 0, jump = 0;
    if (axDecodeJump(sym, mod, repeat, &jump)) {     // Home/End: first/last location of the layer
        if (!jump) return true;                      // held jump: one landing per press
        dRow = jump;
    } else if (!axDecodeArrow(sym, mod, repeat, /*wantCols=*/false, &dRow, &dCol)) return false;
    if (!dRow) return true;                          // throttled repeat: claimed, no step this tick
    int n = tmRows(base, false);
    if (n <= 0) { postSpeech(axs(AXS_TM_NO_LOCATIONS)); return true; }
    axStepCursor(&g_tmRow, n, 0);                    // re-clamp: rows rebuild per layer
    if (axStepCursor(&g_tmRow, n, dRow))             // hard stop: re-read the end row
        g_tmTipLine = 0;                             // moved: the next Ctrl+Up starts from this row
    tmHoverRow(&g_tmRows[g_tmRow]);                  // the game's own focus cue follows the cursor
    tmSpeakRow(base, nullptr);
    return true;
}

void checkTownMap(uintptr_t base) {
    uintptr_t root = resTownRoot(base);
    int layer = tmCurrentLayer(root);

    bool travelled = false;
    if (root && layer >= 0 && layer != g_tmLayer) {
        travelled = g_tmLayer >= 0;                  // -1 = first sight of town, not a journey
        logLine("townmap: town layer %d -> %d%s", g_tmLayer, layer,
                travelled ? "" : " (first read)");
        g_tmLayer   = layer;
        g_tmRow     = 0;
        g_tmTipLine = 0;                             // a new map: the buffer belongs to a new row
        if (travelled && g_tmOpenWatchUntil && g_tmOpenIsDistrict) {
            g_tmOpenWatchUntil = 0;                  // that was our click; it worked
            g_tmOpenIsDistrict = false;
        }
    }
    if (!root) g_tmLayer = -1;

    if (g_tmOpenWatchUntil) {
        if (!root) {
            g_tmOpenWatchUntil = 0;                  // town itself went away; nothing to report
            g_tmOpenIsDdis     = false;
        } else if (g_tmOpenIsDdis) {
            if (axIsDistrict()) {
                g_tmOpenWatchUntil = 0;
                g_tmOpenIsDdis     = false;
                logLine("townmap: \"%s\" opened -> the districts context announces", g_tmOpenName);
            } else if (GetTickCount() > g_tmOpenWatchUntil) {
                g_tmOpenWatchUntil = 0;
                g_tmOpenIsDdis     = false;
                logLine("townmap: districts click on \"%s\" made no observable change", g_tmOpenName);
                postSpeech(axs(AXS_TM_DISTRICTS_NO_OPEN));
            }
        } else if (g_tmOpenIsDistrict) {
            if (confirmDialogOpen(base)) {
                g_tmOpenWatchUntil = GetTickCount() + 3000;
            } else if (GetTickCount() > g_tmOpenWatchUntil) {
                g_tmOpenWatchUntil = 0;
                g_tmOpenIsDistrict = false;
                logLine("townmap: district click on \"%s\" did not change the layer", g_tmOpenName);
                postSpeech(axs(AXS_TM_NO_TRAVEL));
            }
        } else if (!tmbBareMap(root)) {
            g_tmOpenWatchUntil = 0;
            if (bldOpenPanel(root)) {
                logLine("townmap: \"%s\" opened -> the building context announces", g_tmOpenName);
            } else {
                char utter[128];
                _snprintf(utter, sizeof utter, axs(AXS_TM_OPENED_FMT), g_tmOpenName);
                utter[sizeof utter - 1] = 0;
                postSpeech(utter);
            }
        } else if (GetTickCount() > g_tmOpenWatchUntil) {
            g_tmOpenWatchUntil = 0;
            logLine("townmap: open click on \"%s\" made no observable change", g_tmOpenName);
            logInputSnapshot(base, "townmap-open-timeout");
            diagDumpFocusElements(base, "townmap-open-timeout");
            postSpeech(axs(AXS_TM_BLD_NO_OPEN));
        }
    }

    if (g_tjChainId[0]) {
        if (!root) {
            g_tjChainId[0] = 0;                       // town itself went away; nothing to report
            g_tjChainDue   = 0;
        } else if (tmbBareMap(root) && !g_resActive) {
            if (!g_tjChainDue) {
                g_tjChainDue = GetTickCount() + 300;
            } else if (GetTickCount() >= g_tjChainDue) {
                char id[28];
                strncpy(id, g_tjChainId, sizeof id - 1);
                id[sizeof id - 1] = 0;
                g_tjChainId[0] = 0;
                g_tjChainDue   = 0;
                int n = tmRows(base, false);
                int found = -1;
                for (int i = 0; i < n; i++)
                    if (strcmp(g_tmRows[i].id, id) == 0) { found = i; break; }
                if (found >= 0) {
                    g_tmRow     = found;
                    g_tmTipLine = 0;
                    logLine("townjump: chain landed on the map -> opening \"%s\"", id);
                    tmOpenRow(base, &g_tmRows[found]);
                } else {
                    char name[TM_NAME_MAX], utter[TM_NAME_MAX + 32];
                    tjTargetName(base, id, name, sizeof name);
                    logLine("townjump: chain target \"%s\" is not on this map", id);
                    _snprintf(utter, sizeof utter, axs(AXS_TJ_NOT_HERE), name);
                    utter[sizeof utter - 1] = 0;
                    postSpeech(utter);
                }
            }
        } else if (GetTickCount() > g_tjChainUntil) {
            logLine("townjump: chain to \"%s\" timed out -- the bare map never came back",
                    g_tjChainId);
            g_tjChainId[0] = 0;
            g_tjChainDue   = 0;
            postSpeech(axs(AXS_TJ_NO_MAP));
        }
    }

    bool on = false;
    if (root && !g_resActive && !g_ptyActive && !g_rosActive && !circusScreenUp(base) &&
        !axIsLoading() && !circusMatchLoading() && tmbBareMap(root)) {
        uintptr_t beg = 0;
        uintptr_t owner = tmbOwner(root);
        on = owner && tmbGroup(owner, 3, &beg) > 0;  // a still-constructing town has no rows yet
    }
    char prefix[TM_NAME_MAX + 16];
    if (on && !g_tmActive) {
        g_tmActive  = true;
        g_tmTipLine = 0;
        int n = tmRows(base, !g_tmDumped);        // log the roster once per session
        g_tmDumped = true;
        logLine("townmap: active, layer %d, %d locations, row %d", layer, n, g_tmRow);
        if (!g_tjChainId[0] && !g_bnSwitchUntil) {
            tmSurfacePrefix(base, root, prefix, sizeof prefix);
            tmSpeakRow(base, prefix);                // land ON something, in one utterance
        } else {
            logLine("townmap: entry announce held (a town jump is in flight)");
        }
    } else if (!on && g_tmActive) {
        g_tmActive = false;
        logLine("townmap: stood down");
    } else if (on && travelled) {
        int n = tmRows(base, true);               // a new district's roster is worth a log
        char name[TM_NAME_MAX];
        tmLayerName(base, layer, name, sizeof name);
        logLine("townmap: arrived in layer %d (\"%s\"), %d locations", layer, name, n);
        _snprintf(prefix, sizeof prefix, "%s. ", name);
        prefix[sizeof prefix - 1] = 0;
        tmSpeakRow(base, prefix);
    }
    if (!root) { g_tmRow = 0; g_tmTipLine = 0; }     // left town: the next visit starts at the top
}
