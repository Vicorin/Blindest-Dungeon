// settings/keymap.cpp -- the mod's REMAPPABLE COMMAND TABLE and the key-translation layer.

#include <windows.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include "internal.h"

// ---- The table ----
static const KmEntry kEntries[] = {
    // ---- DUNGEON (in a raid) ----
    { "dungeon.step_forward", KMR_DUNGEON, { SDLK_d, KM_SHIFT },            AXS_KMD_STEP_FWD },
    { "dungeon.step_back",    KMR_DUNGEON, { SDLK_a, KM_SHIFT },            AXS_KMD_STEP_BACK },
    { "dungeon.skill_1",      KMR_DUNGEON, { SDLK_1, 0 },                   AXS_KMD_SKILL1 },
    { "dungeon.skill_2",      KMR_DUNGEON, { SDLK_1 + 1, 0 },               AXS_KMD_SKILL2 },
    { "dungeon.skill_3",      KMR_DUNGEON, { SDLK_1 + 2, 0 },               AXS_KMD_SKILL3 },
    { "dungeon.skill_4",      KMR_DUNGEON, { SDLK_4, 0 },                   AXS_KMD_SKILL4 },
    { "dungeon.skill_5",      KMR_DUNGEON, { SDLK_5, 0 },                   AXS_KMD_SKILL5 },
    { "dungeon.torch_lower",  KMR_DUNGEON, { SDLK_t, KM_SHIFT },            AXS_KMD_TORCH_LOWER },
    { "dungeon.torch_snuff",  KMR_DUNGEON, { SDLK_t, KM_SHIFT | KM_CTRL },  AXS_KMD_TORCH_SNUFF },
    { "dungeon.panel_map",    KMR_DUNGEON, { SDLK_m, 0 },                   AXS_KMD_PANEL_MAP },
    { "dungeon.panel_bag",    KMR_DUNGEON, { SDLK_i, 0 },                   AXS_KMD_PANEL_BAG },
    { "dungeon.dungeon_view", KMR_DUNGEON, { SDLK_r, 0 },                   AXS_KMD_DUNGEON_VIEW },
    { "dungeon.quest_goals",  KMR_DUNGEON, { SDLK_g, 0 },                   AXS_KMD_QUEST_GOALS },
    { "dungeon.action_bar",   KMR_DUNGEON, { SDLK_BACKQUOTE, 0 },           AXS_KMD_ACTION_BAR },
    { "dungeon.combat_log",   KMR_DUNGEON, { SDLK_PERIOD, 0 },              AXS_KMD_COMBAT_LOG },
    { "dungeon.light_meter",  KMR_DUNGEON, { SDLK_l, 0 },                   AXS_KMD_LIGHT },
    { "dungeon.confirm",      KMR_DUNGEON, { SDLK_RETURN, 0 },              AXS_KMD_CONFIRM },
    { "dungeon.character_sheet", KMR_DUNGEON, { SDLK_c, 0 },                 AXS_KMD_CHAR_SHEET },
    { "dungeon.bag_take",     KMR_DUNGEON, { SDLK_SPACE, 0 },               AXS_KMD_BAG_TAKE },
    { "dungeon.bag_all",      KMR_DUNGEON, { SDLK_SPACE, KM_CTRL },         AXS_KMD_BAG_ALL },
    { "dungeon.bag_discard",  KMR_DUNGEON, { SDLK_DELETE, 0 },              AXS_KMD_BAG_DISCARD },
    { "dungeon.nav_up",       KMR_DUNGEON, { SDLK_UP, 0 },                  AXS_KMD_NAV_UP },
    { "dungeon.nav_down",     KMR_DUNGEON, { SDLK_DOWN, 0 },                AXS_KMD_NAV_DOWN },
    { "dungeon.nav_left",     KMR_DUNGEON, { SDLK_LEFT, 0 },                AXS_KMD_NAV_LEFT },
    { "dungeon.nav_right",    KMR_DUNGEON, { SDLK_RIGHT, 0 },               AXS_KMD_NAV_RIGHT },
    { "dungeon.tip_back",     KMR_DUNGEON, { SDLK_UP, KM_CTRL },            AXS_KMD_TIP_BACK },
    { "dungeon.tip_fwd",      KMR_DUNGEON, { SDLK_DOWN, KM_CTRL },          AXS_KMD_TIP_FWD },
    { "dungeon.tile_left",    KMR_DUNGEON, { SDLK_LEFT, KM_CTRL },          AXS_KMD_TILE_LEFT },
    { "dungeon.tile_right",   KMR_DUNGEON, { SDLK_RIGHT, KM_CTRL },         AXS_KMD_TILE_RIGHT },
    { "dungeon.map_party",    KMR_DUNGEON, { SDLK_HOME, 0 },                AXS_KMD_MAP_PARTY },
    // ---- TOWN (the hamlet) ----
    { "town.resources",       KMR_TOWN, { SDLK_r, 0 },                      AXS_KMT_RESOURCES },
    { "town.activity_log",    KMR_TOWN, { SDLK_PERIOD, 0 },                 AXS_KMT_LOG },
    { "town.forward",         KMR_TOWN, { SDLK_e, 0 },                      AXS_KMT_FORWARD },
    { "town.trinket_inv",     KMR_TOWN, { SDLK_i, 0 },                      AXS_KMT_TRINKETS },
    { "town.hero_list",       KMR_TOWN, { SDLK_TAB, 0 },                    AXS_KMT_HEROES },
    { "town.hero_list_back",  KMR_TOWN, { SDLK_TAB, KM_SHIFT },             AXS_KMT_HEROES_BACK },
    { "town.jump_abbey",      KMR_TOWN, { SDLK_a, 0 },                      AXS_KMT_JUMP_ABBEY },
    { "town.jump_smith",      KMR_TOWN, { SDLK_b, 0 },                      AXS_KMT_JUMP_SMITH },
    { "town.jump_coach",      KMR_TOWN, { SDLK_c, 0 },                      AXS_KMT_JUMP_COACH },
    { "town.jump_districts",  KMR_TOWN, { SDLK_d, 0 },                      AXS_KMT_JUMP_DISTRICTS },
    { "town.jump_guild",      KMR_TOWN, { SDLK_l, 0 },                      AXS_KMT_JUMP_GUILD },
    { "town.jump_memoirs",    KMR_TOWN, { SDLK_m, 0 },                      AXS_KMT_JUMP_MEMOIRS },
    { "town.jump_sanitarium", KMR_TOWN, { SDLK_s, 0 },                      AXS_KMT_JUMP_SANITARIUM },
    { "town.jump_tavern",     KMR_TOWN, { SDLK_t, 0 },                      AXS_KMT_JUMP_TAVERN },
    { "town.jump_survivalist",KMR_TOWN, { SDLK_v, 0 },                      AXS_KMT_JUMP_SURVIVALIST },
    { "town.jump_wagon",      KMR_TOWN, { SDLK_w, 0 },                      AXS_KMT_JUMP_WAGON },
    { "town.jump_graveyard",  KMR_TOWN, { SDLK_y, 0 },                      AXS_KMT_JUMP_GRAVEYARD },
    { "town.jump_circus",     KMR_TOWN, { SDLK_z, 0 },                      AXS_KMT_JUMP_CIRCUS },
    { "town.strip_prev",      KMR_TOWN, { SDLK_PAGEUP, 0 },                 AXS_KMT_STRIP_PREV },
    { "town.strip_next",      KMR_TOWN, { SDLK_PAGEDOWN, 0 },               AXS_KMT_STRIP_NEXT },
    { "town.upgrades",        KMR_TOWN, { SDLK_u, 0 },                      AXS_KMT_UPGRADES },
    { "town.sheet_slot_1",    KMR_TOWN, { SDLK_1, 0 },                      AXS_KMT_SHEET_SLOT1 },
    { "town.sheet_slot_2",    KMR_TOWN, { SDLK_1 + 1, 0 },                  AXS_KMT_SHEET_SLOT2 },
    { "town.sheet_slot_3",    KMR_TOWN, { SDLK_1 + 2, 0 },                  AXS_KMT_SHEET_SLOT3 },
    { "town.sheet_slot_4",    KMR_TOWN, { SDLK_4, 0 },                      AXS_KMT_SHEET_SLOT4 },
    { "town.confirm",         KMR_TOWN, { SDLK_RETURN, 0 },                 AXS_KMT_CONFIRM },
    { "town.confirm_stack",   KMR_TOWN, { SDLK_RETURN, KM_SHIFT },          AXS_KMT_CONFIRM_STACK },
    { "town.reorder",         KMR_TOWN, { SDLK_SPACE, 0 },                  AXS_KMT_REORDER },
    { "town.slot_clear",      KMR_TOWN, { SDLK_DELETE, 0 },                 AXS_KMT_SLOT_CLEAR },
    { "town.nav_up",          KMR_TOWN, { SDLK_UP, 0 },                     AXS_KMT_NAV_UP },
    { "town.nav_down",        KMR_TOWN, { SDLK_DOWN, 0 },                   AXS_KMT_NAV_DOWN },
    { "town.nav_left",        KMR_TOWN, { SDLK_LEFT, 0 },                   AXS_KMT_NAV_LEFT },
    { "town.nav_right",       KMR_TOWN, { SDLK_RIGHT, 0 },                  AXS_KMT_NAV_RIGHT },
    { "town.tip_back",        KMR_TOWN, { SDLK_UP, KM_CTRL },               AXS_KMT_TIP_BACK },
    { "town.tip_fwd",         KMR_TOWN, { SDLK_DOWN, KM_CTRL },             AXS_KMT_TIP_FWD },
    { "town.rank_now",        KMR_TOWN, { SDLK_LEFT, KM_CTRL },             AXS_KMT_RANK_NOW },
    { "town.rank_next",       KMR_TOWN, { SDLK_RIGHT, KM_CTRL },            AXS_KMT_RANK_NEXT },
};
static const int KM_COUNT = (int)(sizeof kEntries / sizeof kEntries[0]);

static KmChord g_cur[sizeof kEntries / sizeof kEntries[0]];
static bool    g_blank[sizeof kEntries / sizeof kEntries[0]];
static KmChord g_snap[sizeof kEntries / sizeof kEntries[0]];
static bool    g_snapBlank[sizeof kEntries / sizeof kEntries[0]];
static bool    g_kmLoaded = false;
static bool    g_kmAnyRemapped = false;

int kmCount() { return KM_COUNT; }
const KmEntry* kmAt(int i) { return (i >= 0 && i < KM_COUNT) ? &kEntries[i] : nullptr; }
bool kmIsBlank(int i) { return (i >= 0 && i < KM_COUNT) ? g_blank[i] : false; }
KmChord kmCurrent(int i) {
    if (i < 0 || i >= KM_COUNT) { KmChord z = { 0, 0 }; return z; }
    return g_cur[i];
}

static bool chordEq(KmChord a, KmChord b) { return a.sym == b.sym && a.mods == b.mods; }

static void kmRecountRemapped() {
    g_kmAnyRemapped = false;
    for (int i = 0; i < KM_COUNT; i++)
        if (g_blank[i] || !chordEq(g_cur[i], kEntries[i].def)) { g_kmAnyRemapped = true; return; }
}

// ---- Chord <-> text ----
struct KmNamedKey { uint32_t sym; const char* tok; AxStrId spoken; };
static const KmNamedKey kNamedKeys[] = {
    { SDLK_RETURN,    "enter",     AXS_KEY_ENTER },
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

void kmEnsureLoaded() {
    if (g_kmLoaded) return;
    g_kmLoaded = true;
    for (int i = 0; i < KM_COUNT; i++) { g_cur[i] = kEntries[i].def; g_blank[i] = false; }
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
        const char* id = p;
        const char* val = eq + 1;
        int idx = -1;
        for (int i = 0; i < KM_COUNT; i++)
            if (strcmp(id, kEntries[i].id) == 0) { idx = i; break; }
        if (idx < 0) { bad++; logLine("keymap: unknown id \"%s\" -- skipped", id); continue; }
        if (strcmp(val, "unbound") == 0) { g_blank[idx] = true; applied++; continue; }
        KmChord c;
        if (!kmChordFromToken(val, &c)) { bad++; logLine("keymap: bad chord \"%s\" for %s -- default kept", val, id); continue; }
        for (int j = 0; j < KM_COUNT; j++)
            if (j != idx && kEntries[j].region == kEntries[idx].region &&
                !g_blank[j] && chordEq(g_cur[j], c)) g_blank[j] = true;
        g_cur[idx] = c;
        g_blank[idx] = false;
        applied++;
    }
    fclose(f);
    kmRecountRemapped();
    logLine("keymap: %s -- %d line(s) applied, %d skipped, remapped=%d", path, applied, bad, (int)g_kmAnyRemapped);
}

void kmSaveFile() {
    char path[MAX_PATH];
    kmFilePath(path, sizeof path);
    if (!path[0]) { logLine("keymap: save skipped -- no LOCALAPPDATA"); return; }
    FILE* f = fopen(path, "w");
    if (!f) { logLine("keymap: save FAILED opening %s", path); return; }
    fprintf(f, "# DarkestAccess key remaps -- one line per control that is off its default.\n");
    fprintf(f, "# <id>=<chord>  (chord: [ctrl+][shift+][alt+]<key>)  or  <id>=unbound\n");
    fprintf(f, "# <key> is a name (a, 7, enter, up, f5, kp3) or pos:<scancode> for a key with no\n");
    fprintf(f, "# ASCII face -- a position, so it follows the same physical key across layouts.\n");
    int written = 0;
    for (int i = 0; i < KM_COUNT; i++) {
        if (g_blank[i]) { fprintf(f, "%s=unbound\n", kEntries[i].id); written++; continue; }
        if (chordEq(g_cur[i], kEntries[i].def)) continue;
        char tok[96];
        kmChordToken(g_cur[i], tok, sizeof tok);
        fprintf(f, "%s=%s\n", kEntries[i].id, tok);
        written++;
    }
    fclose(f);
    logLine("keymap: saved %d remap(s) -> %s", written, path);
}

// ---- Snapshot / revert / dirty (the menu's save-question machinery) ----
void kmSnapshot() {
    for (int i = 0; i < KM_COUNT; i++) { g_snap[i] = g_cur[i]; g_snapBlank[i] = g_blank[i]; }
}
void kmRevert() {
    for (int i = 0; i < KM_COUNT; i++) { g_cur[i] = g_snap[i]; g_blank[i] = g_snapBlank[i]; }
    kmRecountRemapped();
}
bool kmDirty() {
    for (int i = 0; i < KM_COUNT; i++)
        if (g_snapBlank[i] != g_blank[i] || !chordEq(g_snap[i], g_cur[i])) return true;
    return false;
}

// ---- Assign (the capture's commit) ----
int kmAssign(int i, KmChord c) {
    if (i < 0 || i >= KM_COUNT) return -1;
    int victim = -1;
    for (int j = 0; j < KM_COUNT; j++) {
        if (j == i || kEntries[j].region != kEntries[i].region) continue;
        if (!g_blank[j] && chordEq(g_cur[j], c)) { g_blank[j] = true; victim = j; break; }
    }
    g_cur[i] = c;
    g_blank[i] = false;
    kmRecountRemapped();
    char tok[96];
    kmChordToken(c, tok, sizeof tok);
    logLine("keymap: %s = %s%s%s", kEntries[i].id, tok,
            victim >= 0 ? " (cleared " : "", victim >= 0 ? kEntries[victim].id : "");
    return victim;
}

int kmFirstBlank(KmRegion r) {
    for (int i = 0; i < KM_COUNT; i++)
        if (g_blank[i] && (r == KMR_NONE || kEntries[i].region == r)) return i;
    return -1;
}

// ---- The translation layer ----
KmRegion kmRegionNow(uintptr_t base, AxContext ctx) {
    if (g_textInputActive) return KMR_NONE;
    switch (ctx) {
        case AX_NAMING: case AX_LOADING: case AX_GLOSSARY: case AX_HELP:
        case AX_PAUSE: case AX_DIALOG: case AX_JOURNAL: case AX_RESULTS:
        case AX_TITLE: case AX_SETTINGS: case AX_TUTORIAL: case AX_TOWNEVENT:
            return KMR_NONE;
        default: break;
    }
    if (mapRoot(base)) return KMR_DUNGEON;
    if (resTownRoot(base)) return KMR_TOWN;
    return KMR_NONE;
}

static uint8_t kmModsFromKmod(uint16_t mod) {
    uint8_t m = 0;
    if (mod & (KMOD_LSHIFT | KMOD_RSHIFT)) m |= KM_SHIFT;
    if (mod & (KMOD_LCTRL | KMOD_RCTRL))   m |= KM_CTRL;
    if (mod & (KMOD_LALT | KMOD_RALT))     m |= KM_ALT;
    return m;
}
uint8_t kmChordModsFromKmod(uint16_t mod) { return kmModsFromKmod(mod); }

int kmRoute(uintptr_t base, AxContext ctx, uint32_t* sym, uint16_t* mod) {
    kmEnsureLoaded();
    if (!g_kmAnyRemapped) return 0;                   // nobody remapped anything: zero cost
    KmRegion r = kmRegionNow(base, ctx);
    if (r == KMR_NONE) return 0;
    KmChord pressed = { *sym, kmModsFromKmod(*mod) };
    for (int i = 0; i < KM_COUNT; i++) {
        if (kEntries[i].region != r || g_blank[i]) continue;
        if (!chordEq(g_cur[i], pressed)) continue;
        if (chordEq(kEntries[i].def, pressed)) return 0;   // still on its default: nothing to do
        const uint16_t MODBITS = (uint16_t)(KMOD_LSHIFT | KMOD_RSHIFT | KMOD_LCTRL |
                                            KMOD_RCTRL | KMOD_LALT | KMOD_RALT);
        uint16_t m = (uint16_t)(*mod & ~MODBITS);
        if (kEntries[i].def.mods & KM_SHIFT) m |= KMOD_LSHIFT;
        if (kEntries[i].def.mods & KM_CTRL)  m |= KMOD_LCTRL;
        if (kEntries[i].def.mods & KM_ALT)   m |= KMOD_LALT;
        *sym = kEntries[i].def.sym;
        *mod = m;
        return 1;
    }
    // 2) Is it the canonical chord of an entry that no longer answers to it? Freed.
    for (int i = 0; i < KM_COUNT; i++) {
        if (kEntries[i].region != r) continue;
        if (!g_blank[i] && chordEq(g_cur[i], kEntries[i].def)) continue;   // still home
        if (chordEq(kEntries[i].def, pressed)) return 2;
    }
    return 0;
}
