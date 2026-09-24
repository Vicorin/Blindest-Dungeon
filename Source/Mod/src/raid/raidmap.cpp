// raid/raidmap.cpp -- THE IN-RAID MAP

#include <cstdint>
#include <cstring>
#include <cstdio>
#include "internal.h"

// ---- In-raid MAP: the scouting overlay latch ----
static const uintptr_t PANEL_SCOUT_SHOW_OFF = 0xafd;
static const char* const SCOUT_WORD_KEY = "str_scouting";

static volatile void* g_mapPanel = nullptr;
enum MapReq { MR_NONE = 0, MR_TOGGLE, MR_MOVE, MR_HOME };
static volatile int   g_mapReq    = MR_NONE;
static volatile int   g_moveDir   = 0;      // 0=up 1=down 2=left 3=right
static volatile bool  g_moveArea  = false;
volatile bool  g_mapReview = false;
static volatile bool  g_mapSuspended = false;
static int            g_mrArea    = 0;       // cursor: index into the area vector
static int            g_mrTile    = 0;       // cursor: tile index within the current area

static volatile bool  g_moveActivateReq = false;  // Enter pressed in the map layer
static bool     g_movePending      = false;       // the move fn was called; awaiting the result
static uint32_t g_movePendDeadline = 0;           // GetTickCount() ms deadline for the result window
static int32_t  g_movePendPrevArea = 0;           // party current-area id at call time
static float    g_movePendPrevX    = 0.0f;
static float    g_movePendPrevY    = 0.0f;
static char     g_movePendLabel[96] = {0};        // "Room A" etc., for the "Moving to ..." line

static const bool     UP_IS_POSITIVE_Y = false;

struct ContentLabel { const char* key; const char* en; };
static const ContentLabel kAreaContent[] = {
    { "str_map_ac_nothing_tooltip",          "empty" },            // 0  (no shipped string)
    { "str_map_ac_battle_tooltip",           "Battle" },           // 1
    { "str_map_ac_ambush_tooltip",           "Ambush" },           // 2
    { "str_map_ac_trap_tooltip",             "Trap" },             // 3
    { "str_map_ac_obstacle_tooltip",         "Obstacle" },         // 4
    { "str_map_ac_happening_tooltip",        "Happening" },        // 5  (no shipped string)
    { "str_map_ac_guarded_curio_tooltip",    "Guarded Curio" },    // 6  <- was "Curio"
    { "str_map_ac_curio_tooltip",            "Curio" },            // 7  <- was "Guarded Curio"
    { "str_map_ac_hunger_tooltip",           "Hunger" },           // 8  (no shipped string; and the
    { "str_map_ac_treasure_tooltip",         "Treasure" },         // 9  (no shipped string)
    { "str_map_ac_guarded_treasure_tooltip", "Guarded Treasure" }, // 10
    { "str_map_ac_ambush_curio_tooltip",     "Ambush Curio" },     // 11
    { "str_map_ac_ambush_treasure_tooltip",  "Ambush Treasure" },  // 12
    { "str_map_ac_hidden_door_tooltip",      "Secret Door" },      // 13
    { "str_map_ac_prisoner_tooltip",         "Prisoner" },         // 14  (no shipped string)
};

// ---- HUNGER IS NEVER ON THE MAP ----
static const int AREA_CONTENT_HUNGER = 8;
static inline int32_t mapVisibleContent(int32_t content) {
    return content == AREA_CONTENT_HUNGER ? 0 : content;
}

void mapContentLabel(uintptr_t base, int content, char* out, int outsz) {   // exported (internal.h)
    if (content == 0) { strncpy(out, axs(AXS_MAP_CONTENT_EMPTY), outsz - 1); out[outsz - 1] = 0; return; }
    if (content < 0 || content >= (int)(sizeof kAreaContent / sizeof kAreaContent[0])) {
        _snprintf(out, outsz, axs(AXS_MAP_CONTENT_N_FMT), content); out[outsz - 1] = 0; return;
    }
    const ContentLabel& c = kAreaContent[content];
    if (resolveKey(base, c.key, out, outsz) && out[0]) return;      // localized, if present
    strncpy(out, c.en, outsz - 1); out[outsz - 1] = 0;             // English fallback
}

bool tileContentsVisible(uintptr_t area, int tileIdx) {
    if (!area || tileIdx < 0) return false;
    uintptr_t tb = 0;
    if (!safeReadPtr(area + AREA_TILES_BEG, &tb) || !tb) return false;
    uint32_t k = 0;
    if (!safeReadU32(tb + (uintptr_t)tileIdx * TILE_STRIDE + TILE_KNOWLEDGE_OFF, &k)) return false;
    return k > 1;                       // the game's own test, ScoutAreaData::Initialize
}

static const int VW_MAX_AREAS = 96;
struct VisitedObsArea { uintptr_t area; uint32_t bits; };
static VisitedObsArea g_vwObs[VW_MAX_AREAS];
static int g_vwObsCount = 0;

static bool vwObserved(uintptr_t area, int tileIdx) {
    if (tileIdx < 0 || tileIdx >= 32) return false;
    for (int i = 0; i < g_vwObsCount; i++)
        if (g_vwObs[i].area == area) return (g_vwObs[i].bits >> tileIdx) & 1;
    return false;
}

static bool tileVisited(uintptr_t area, int tileIdx) {
    if (!area || tileIdx < 0) return false;
    uintptr_t tb = 0;
    if (!safeReadPtr(area + AREA_TILES_BEG, &tb) || !tb) return false;
    uint32_t k = 0;
    if (!safeReadU32(tb + (uintptr_t)tileIdx * TILE_STRIDE + TILE_KNOWLEDGE_OFF, &k)) return false;
    if (k != 3) return false;
    int32_t kind = -1, content = 0;
    if (safeReadU32(area + AREA_KIND_OFF, (uint32_t*)&kind) && kind == 1 &&
        safeReadU32(tb + (uintptr_t)tileIdx * TILE_STRIDE + TILE_CONTENT_OFF, (uint32_t*)&content) &&
        (content == AREA_CONTENT_OBSTACLE || content == AREA_CONTENT_CURIO))
        return vwObserved(area, tileIdx);
    return true;
}

static bool areaFullyVisited(uintptr_t area) {
    long n = mapAreaTiles(area);
    if (n <= 0) return false;
    for (long t = 0; t < n; t++) if (!tileVisited(area, (int)t)) return false;
    return true;
}

// Decode a 4-char little-endian FourCC area id (out >= 5 chars).
void fourcc(uint32_t id, char* out) {
    out[0] = (char)(id & 0xff);        out[1] = (char)((id >> 8) & 0xff);
    out[2] = (char)((id >> 16) & 0xff); out[3] = (char)((id >> 24) & 0xff); out[4] = 0;
    for (int i = 0; i < 4; i++) { char c = out[i]; if (c < 32 || c > 126) out[i] = '?'; }
}
static bool isSecretAreaId(uint32_t id) {
    char cc[5]; fourcc(id, cc);
    return cc[0] == 's' && cc[1] == 'e' && cc[2] == 'c';
}

void mapAreaLabel(uint32_t id, int kind, char* out, int outsz) {            // exported (internal.h)
    char cc[5]; fourcc(id, cc);
    if (kind == 0 && cc[0] == 'r' && cc[1] == 'o' && cc[2] == 'o')
        _snprintf(out, outsz, axs(AXS_MAP_ROOM_FMT), cc[3]);
    else if (kind == 0 && isSecretAreaId(id))
        _snprintf(out, outsz, "%s", axs(AXS_MAP_SECRET_ROOM));
    else
        _snprintf(out, outsz, axs(kind == 1 ? AXS_MAP_HALLWAY_FMT : AXS_MAP_AREA_FMT), cc);
    out[outsz - 1] = 0;
}

float u32AsFloatM(uint32_t u) { float f; memcpy(&f, &u, sizeof f); return f; }
static bool  floatsClose(uint32_t ar, uint32_t br) {
    float d = u32AsFloatM(ar) - u32AsFloatM(br); if (d < 0) d = -d; return d < 0.6f;
}

// ---- Map graph accessors ----
uintptr_t mapRoot(uintptr_t base) {
    uintptr_t r = 0;
    if (!safeReadPtr(base + MAP_ROOT_RVA, &r) || r <= 0x10000) return 0;
    return r;
}
static long mapAreaCount(uintptr_t root) {
    uintptr_t b = 0, e = 0;
    safeReadPtr(root + MAP_AREAVEC_BEG, &b);
    safeReadPtr(root + MAP_AREAVEC_END, &e);
    if (!b || e <= b) return 0;
    long n = (long)((e - b) / 8);
    return (n < 0 || n > 100000) ? 0 : n;
}
static uintptr_t mapAreaAt(uintptr_t root, long i) {
    uintptr_t b = 0;
    if (!safeReadPtr(root + MAP_AREAVEC_BEG, &b) || !b) return 0;
    uintptr_t a = 0;
    if (safeReadPtr(b + (uintptr_t)i * 8, &a) && a > 0x10000) return a;
    return 0;
}
uintptr_t mapAreaById(uintptr_t root, uint32_t id) {                        // exported (internal.h)
    long n = mapAreaCount(root);
    for (long i = 0; i < n; i++) {
        uintptr_t a = mapAreaAt(root, i); if (!a) continue;
        uint32_t aid = 0;
        if (safeReadU32(a + AREA_ID_OFF, &aid) && aid == id) return a;
    }
    return 0;
}
long mapAreaTiles(uintptr_t area) {                                         // exported (internal.h)
    uintptr_t tb = 0, te = 0;
    safeReadPtr(area + AREA_TILES_BEG, &tb);
    safeReadPtr(area + AREA_TILES_END, &te);
    return (tb && te > tb) ? (long)((te - tb) / TILE_STRIDE) : 0;
}

static long mapAreaIndexById(uintptr_t root, uint32_t id) {
    long n = mapAreaCount(root);
    for (long i = 0; i < n; i++) {
        uintptr_t a = mapAreaAt(root, i); if (!a) continue;
        uint32_t aid = 0;
        if (safeReadU32(a + AREA_ID_OFF, &aid) && aid == id) return i;
    }
    return -1;
}

// Read a tile's grid xy. Returns false on a bad tile.
static bool tileXY(uintptr_t root, long ai, int ti, float* x, float* y) {
    uintptr_t area = mapAreaAt(root, ai); if (!area) return false;
    uintptr_t tb = 0; if (!safeReadPtr(area + AREA_TILES_BEG, &tb) || !tb) return false;
    long tiles = mapAreaTiles(area); if (ti < 0 || ti >= tiles) return false;
    uintptr_t tile = tb + (uintptr_t)ti * TILE_STRIDE;
    uint32_t xr = 0, yr = 0;
    safeReadU32(tile + TILE_X_OFF, &xr); safeReadU32(tile + TILE_Y_OFF, &yr);
    *x = u32AsFloatM(xr); *y = u32AsFloatM(yr); return true;
}
static int areaNearestTileP(uintptr_t area, float px, float py) {
    if (!area) return 0;
    uintptr_t tb = 0; if (!safeReadPtr(area + AREA_TILES_BEG, &tb) || !tb) return 0;
    long n = mapAreaTiles(area); if (n <= 0) return 0;
    int best = 0; float bestd = 1e30f;
    for (long t = 0; t < n; t++) {
        uintptr_t tile = tb + (uintptr_t)t * TILE_STRIDE;
        uint32_t xr = 0, yr = 0;
        safeReadU32(tile + TILE_X_OFF, &xr); safeReadU32(tile + TILE_Y_OFF, &yr);
        float dx = u32AsFloatM(xr) - px, dy = u32AsFloatM(yr) - py;
        float d = dx * dx + dy * dy;
        if (d < bestd) { bestd = d; best = (int)t; }
    }
    return best;
}
static int areaNearestTile(uintptr_t root, long ai, float px, float py) {
    return areaNearestTileP(mapAreaAt(root, ai), px, py);
}

static bool corridorEnds(uintptr_t area, long tiles, uint32_t* roomAt0, uint32_t* roomAtEnd) {
    *roomAt0 = 0; *roomAtEnd = 0;
    if (tiles <= 0) return false;
    uintptr_t tb = 0;
    if (!safeReadPtr(area + AREA_TILES_BEG, &tb) || !tb) return false;
    for (int e = 0; e < 2; e++) {
        uintptr_t tile = tb + (uintptr_t)(e ? tiles - 1 : 0) * TILE_STRIDE;
        int32_t ty = -1; uint32_t nb = 0;
        safeReadU32(tile + TILE_TYPE_OFF, (uint32_t*)&ty);
        safeReadU32(tile + TILE_NEIGH_ID_OFF, &nb);
        if (ty == 2 && nb && nb != FOURCC_NONE) *(e ? roomAtEnd : roomAt0) = nb;
    }
    return *roomAt0 || *roomAtEnd;
}

// ---- Secret rooms: the hidden door in a hallway tile ----
static bool tileHiddenDoor(uintptr_t tile, uint32_t* destId) {
    uint32_t nb = 0; int32_t hk = -1;
    if (!safeReadU32(tile + TILE_NEIGH_ID_OFF, &nb) || !nb || nb == FOURCC_NONE) return false;
    if (!safeReadU32(tile + TILE_HDOOR_KIND_OFF, (uint32_t*)&hk) || hk != TILE_HDOOR_KIND_HIDDEN)
        return false;
    uint8_t seen = 0, access = 0;
    safeReadU8(tile + TILE_HDOOR_SEEN_OFF, &seen);
    safeReadU8(tile + TILE_HDOOR_ACCESS_OFF, &access);
    if (!seen && !access) return false;              // the door exists but may not be used yet
    if (destId) *destId = nb;
    return true;
}

static bool tileHiddenDoorShown(uintptr_t area, int tileIdx, uint32_t* destId) {
    uintptr_t tb = 0;
    if (!safeReadPtr(area + AREA_TILES_BEG, &tb) || !tb) return false;
    if (!tileContentsVisible(area, tileIdx)) return false;   // not scouted: as silent as the screen
    uint8_t found = 0;
    if (!safeReadU8(tb + (uintptr_t)tileIdx * TILE_STRIDE + TILE_HDOOR_SEEN_OFF, &found) || !found)
        return false;                                        // scouted past, but never FOUND
    return tileHiddenDoor(tb + (uintptr_t)tileIdx * TILE_STRIDE, destId);
}

// ---- ⚠ THE DOOR HAS A SECOND NAME, AND IT WAS NEVER GATED ----
static int32_t mapVisibleTileContent(uintptr_t area, int tileIdx, int32_t content) {
    if (content == AREA_CONTENT_HIDDEN_DOOR && !tileHiddenDoorShown(area, tileIdx, nullptr))
        return 0;
    return mapVisibleContent(content);          // hunger reads as empty too
}

int areaFindHiddenDoor(uintptr_t area, uint32_t* destId, uintptr_t* tileOut) {  // exported (internal.h)
    uintptr_t tb = 0;
    if (!safeReadPtr(area + AREA_TILES_BEG, &tb) || !tb) return -1;
    long tiles = mapAreaTiles(area);
    for (long t = 0; t < tiles; t++) {
        if (tileHiddenDoorShown(area, (int)t, destId)) {
            if (tileOut) *tileOut = tb + (uintptr_t)t * TILE_STRIDE;
            return (int)t;
        }
    }
    return -1;
}

static void mapAreaLabelEx(uintptr_t root, uintptr_t area, char* out, int outsz) {
    (void)root;
    uint32_t id = 0; int32_t kind = -1;
    safeReadU32(area + AREA_ID_OFF, &id);
    safeReadU32(area + AREA_KIND_OFF, (uint32_t*)&kind);
    if (kind != 1) { mapAreaLabel(id, kind, out, outsz); return; }
    uint32_t r0 = 0, r1 = 0;
    if (corridorEnds(area, mapAreaTiles(area), &r0, &r1) && r0 && r1) {
        char l0[32], l1[32];
        mapAreaLabel(r0, 0, l0, sizeof l0);
        mapAreaLabel(r1, 0, l1, sizeof l1);
        _snprintf(out, outsz, axs(AXS_MAP_HALLWAY_BETWEEN_FMT), l0, l1);
        out[outsz - 1] = 0;
        return;
    }
    mapAreaLabel(id, 1, out, outsz);            // ends unreadable: the raw "Hallway corX"
}
void mapAreaLabelById(uintptr_t root, uint32_t id, char* out, int outsz) {  // exported (internal.h)
    uintptr_t a = mapAreaById(root, id);
    if (a) { mapAreaLabelEx(root, a, out, outsz); return; }
    char cc[5]; fourcc(id, cc);
    if (cc[0] == 'r' && cc[1] == 'o' && cc[2] == 'o') _snprintf(out, outsz, axs(AXS_MAP_ROOM_FMT), cc[3]);
    else if (isSecretAreaId(id)) _snprintf(out, outsz, "%s", axs(AXS_MAP_SECRET_ROOM));
    else                                              _snprintf(out, outsz, axs(AXS_MAP_AREA_FMT), cc);
    out[outsz - 1] = 0;
}

static const char* dirName(int dir) {
    switch (dir) {
        case 0:  return axs(AXS_MAP_DIR_UP);
        case 1:  return axs(AXS_MAP_DIR_DOWN);
        case 2:  return axs(AXS_MAP_DIR_LEFT);
        default: return axs(AXS_MAP_DIR_RIGHT);
    }
}
// Classify an xy delta into an arrow direction by its dominant axis.
static int deltaToDir(float dx, float dy) {
    float ax = dx < 0 ? -dx : dx, ay = dy < 0 ? -dy : dy;
    if (ay >= ax) return ((dy > 0) == UP_IS_POSITIVE_Y) ? 0 : 1;
    return dx < 0 ? 2 : 3;
}

static void mapFindParty(uintptr_t root, int* outArea, int* outTile) {
    *outArea = 0; *outTile = 0;
    uint32_t pxr = 0, pyr = 0;
    safeReadU32(root + MAP_PARTY_X_OFF, &pxr);
    safeReadU32(root + MAP_PARTY_Y_OFF, &pyr);
    float px = u32AsFloatM(pxr), py = u32AsFloatM(pyr);

    uintptr_t live = 0;
    if (safeReadPtr(root + MAP_CUR_AREA_PTR, &live) && live > 0x10000) {
        long n = mapAreaCount(root);
        for (long i = 0; i < n; i++) {
            if (mapAreaAt(root, i) != live) continue;
            *outArea = (int)i;
            *outTile = areaNearestTile(root, i, px, py);
            return;
        }
    }
    long n = mapAreaCount(root);
    long bestA = 0; int bestT = 0; float bestd = 1e30f;
    for (long i = 0; i < n; i++) {
        uintptr_t a = mapAreaAt(root, i); if (!a) continue;
        uintptr_t tb = 0, te = 0;
        safeReadPtr(a + AREA_TILES_BEG, &tb);
        safeReadPtr(a + AREA_TILES_END, &te);
        long tiles = (tb && te > tb) ? (long)((te - tb) / TILE_STRIDE) : 0;
        for (long t = 0; t < tiles; t++) {
            uintptr_t tile = tb + (uintptr_t)t * TILE_STRIDE;
            uint32_t xr = 0, yr = 0;
            safeReadU32(tile + TILE_X_OFF, &xr);
            safeReadU32(tile + TILE_Y_OFF, &yr);
            float dx = u32AsFloatM(xr) - px, dy = u32AsFloatM(yr) - py;
            float d = dx * dx + dy * dy;
            if (d < bestd) { bestd = d; bestA = i; bestT = (int)t; }
        }
    }
    *outArea = (int)bestA; *outTile = bestT;
}

static bool mapAreaHasParty(uintptr_t root, uintptr_t area) {
    uintptr_t live = 0;
    if (safeReadPtr(root + MAP_CUR_AREA_PTR, &live) && live > 0x10000)
        return live == area;
    uint32_t pxr = 0, pyr = 0;
    safeReadU32(root + MAP_PARTY_X_OFF, &pxr);
    safeReadU32(root + MAP_PARTY_Y_OFF, &pyr);
    long tiles = mapAreaTiles(area);
    uintptr_t tb = 0; safeReadPtr(area + AREA_TILES_BEG, &tb);
    for (long t = 0; t < tiles; t++) {
        uintptr_t tile = tb + (uintptr_t)t * TILE_STRIDE;
        uint32_t xr = 0, yr = 0;
        safeReadU32(tile + TILE_X_OFF, &xr);
        safeReadU32(tile + TILE_Y_OFF, &yr);
        if (floatsClose(pxr, xr) && floatsClose(pyr, yr)) return true;
    }
    return false;
}

// ---- Graph (connection-based) exits ----
struct MapExit {
    int      dir;         // 0..3 arrow direction out of the room
    long     corIdx;
    long     corTiles;    // its tile count
    int      mouthTile;   // hallway tile on THIS room's side
    int      farTile;     // hallway tile on the far side
    uint32_t farRoomId;   // room on the far side (0 if unreadable)
    long     farRoomIdx;
};

static int roomExits(uintptr_t root, long roomIdx, MapExit* out, int maxOut) {
    uintptr_t room = mapAreaAt(root, roomIdx);
    if (!room) return 0;
    uint32_t roomId = 0;
    safeReadU32(room + AREA_ID_OFF, &roomId);
    float rx = 0, ry = 0; tileXY(root, roomIdx, 0, &rx, &ry);

    int got = 0;
    for (int s = 0; s < AREA_EXIT_SLOTS && got < maxOut; s++) {
        uint32_t dest = 0;
        if (!safeReadU32(room + AREA_EXITS_OFF + (uintptr_t)s * AREA_EXIT_STRIDE, &dest)) continue;
        if (!dest || dest == FOURCC_NONE) continue;
        long di = mapAreaIndexById(root, dest);
        if (di < 0) continue;                       // not in the known-area vector (yet)
        uintptr_t da = mapAreaAt(root, di); if (!da) continue;
        int32_t dkind = -1;
        safeReadU32(da + AREA_KIND_OFF, (uint32_t*)&dkind);

        MapExit ex; ex.dir = -1; ex.corIdx = -1; ex.corTiles = 0;
        ex.mouthTile = 0; ex.farTile = 0; ex.farRoomId = 0; ex.farRoomIdx = -1;

        if (dkind != 1) {                           // a door straight onto another room
            float ax = 0, ay = 0;
            if (!tileXY(root, di, 0, &ax, &ay)) continue;
            if (ax == rx && ay == ry) continue;
            ex.dir = deltaToDir(ax - rx, ay - ry);
            ex.farRoomId = dest; ex.farRoomIdx = di;
        } else {
            long tiles = mapAreaTiles(da);
            if (tiles <= 0) continue;
            uint32_t r0 = 0, r1 = 0;
            corridorEnds(da, tiles, &r0, &r1);
            int nearT, farT; uint32_t farId;
            bool hidden = false;
            if (r0 == roomId)      { nearT = 0;               farT = (int)tiles - 1; farId = r1; }
            else if (r1 == roomId) { nearT = (int)tiles - 1;  farT = 0;              farId = r0; }
            else {
                uintptr_t tb2 = 0;
                safeReadPtr(da + AREA_TILES_BEG, &tb2);
                int ht = -1;
                for (long t = 0; tb2 && t < tiles && ht < 0; t++) {
                    uint32_t nb2 = 0;
                    if (tileHiddenDoor(tb2 + (uintptr_t)t * TILE_STRIDE, &nb2) && nb2 == roomId)
                        ht = (int)t;
                }
                if (ht < 0) continue;               // genuinely unrelated hallway
                nearT = ht; farT = ht; farId = dest;
                hidden = true;
            }
            float nx = 0, ny = 0;
            if (!tileXY(root, di, nearT, &nx, &ny)) continue;
            float ddx, ddy;
            if (hidden) {
                // Perpendicular step: from the room's tile to the door tile, not along the hall.
                ddx = nx - rx; ddy = ny - ry;
            } else {
                // Direction = along the hallway from its mouth inward (hallways are axis-aligned).
                int inward = (nearT == 0) ? ((int)tiles > 1 ? 1 : 0) : (int)tiles - 2;
                float ix = 0, iy = 0;
                if (!tileXY(root, di, inward, &ix, &iy)) continue;
                ddx = ix - nx; ddy = iy - ny;
                if (ddx == 0 && ddy == 0) { ddx = nx - rx; ddy = ny - ry; }   // 1-tile hallway
            }
            if (ddx == 0 && ddy == 0) continue;
            ex.dir = deltaToDir(ddx, ddy);
            ex.corIdx = di; ex.corTiles = tiles;
            ex.mouthTile = nearT; ex.farTile = farT;
            ex.farRoomId = farId;
            ex.farRoomIdx = farId ? mapAreaIndexById(root, farId) : -1;
        }
        out[got++] = ex;
    }
    for (int i = 1; i < got; i++)
        for (int j = i; j > 0 && out[j].dir < out[j - 1].dir; j--) {
            MapExit tmp = out[j]; out[j] = out[j - 1]; out[j - 1] = tmp;
        }
    return got;
}

static int corridorDirToEnd(uintptr_t root, long corIdx, int ti, int endTile, long tiles) {
    int from = ti, to = endTile;
    if (from == to) {                       // at the end: direction from the inward neighbour
        from = (endTile == 0) ? ((int)tiles > 1 ? 1 : 0) : (int)tiles - 2;
        if (from < 0) from = 0;
    }
    if (from == to) return -1;
    float fx = 0, fy = 0, tx = 0, ty = 0;
    if (!tileXY(root, corIdx, from, &fx, &fy)) return -1;
    if (!tileXY(root, corIdx, to, &tx, &ty)) return -1;
    if (fx == tx && fy == ty) return -1;
    return deltaToDir(tx - fx, ty - fy);
}

// ---- Announcement builders ----
static void mapDescribeArea(uintptr_t base, uintptr_t root, long i, char* out, int outsz) {
    uintptr_t area = mapAreaAt(root, i);
    if (!area) { _snprintf(out, outsz, axs(AXS_MAP_AREA_UNREADABLE_FMT), i + 1); out[outsz - 1] = 0; return; }
    int32_t kind = -1;
    safeReadU32(area + AREA_KIND_OFF, (uint32_t*)&kind);
    long tiles = mapAreaTiles(area);
    char label[96]; mapAreaLabelEx(root, area, label, sizeof label);

    int o = _snprintf(out, outsz, "%s", label);
    bool here = mapAreaHasParty(root, area);
    if (here) o += _snprintf(out + o, outsz - o, ", %s", axs(AXS_MAP_YOU_ARE_HERE));

    if (kind == 0) {   // room = 1 tile
        uintptr_t tb = 0; safeReadPtr(area + AREA_TILES_BEG, &tb);
        int32_t content = -1;
        if (tb) safeReadU32(tb + TILE_CONTENT_OFF, (uint32_t*)&content);
        content = mapVisibleTileContent(area, 0, content);   // hunger, and an unfound secret door,
                                                             // read as empty — as they look on screen
        if (tb && tileContentsVisible(area, 0)) {
            char cl[128]; mapContentLabel(base, content, cl, sizeof cl);
            o += _snprintf(out + o, outsz - o, ", %s", cl);
        } else {
            o += _snprintf(out + o, outsz - o, ", %s", axs(AXS_MAP_NOT_SCOUTED));
        }
        if (!here && tileVisited(area, 0)) o += _snprintf(out + o, outsz - o, ", %s", axs(AXS_MAP_VISITED));
        MapExit exits[AREA_EXIT_SLOTS];
        int n = roomExits(root, i, exits, AREA_EXIT_SLOTS);
        char ex[256]; int eo = 0; ex[0] = 0; int nex = 0;
        for (int s = 0; s < n; s++) {
            char rl[48];
            if (exits[s].farRoomId) mapAreaLabelById(root, exits[s].farRoomId, rl, sizeof rl);
            else { _snprintf(rl, sizeof rl, "%s", axs(AXS_MAP_UNMAPPED_ROOM)); rl[sizeof rl - 1] = 0; }
            eo += _snprintf(ex + eo, sizeof ex - eo, "%s", nex ? ", " : "");
            eo += _snprintf(ex + eo, sizeof ex - eo, axs(AXS_MAP_EXIT_TO_FMT),
                            dirName(exits[s].dir), rl);
            nex++;
        }
        if (nex) { o += _snprintf(out + o, outsz - o, ". "); o += _snprintf(out + o, outsz - o, axs(AXS_MAP_EXITS_FMT), ex); }
        else   { o += _snprintf(out + o, outsz - o, ". %s", axs(AXS_MAP_NO_EXITS)); }
    } else {           // corridor
        { o += _snprintf(out + o, outsz - o, ", "); o += _snprintf(out + o, outsz - o, axs(AXS_MAP_TILES_FMT), tiles); }
        if (areaFullyVisited(area)) o += _snprintf(out + o, outsz - o, ", %s", axs(AXS_MAP_VISITED));
    }
    out[outsz - 1] = 0;
}

static void mapDescribeTile(uintptr_t base, uintptr_t root, long ai, int ti, char* out, int outsz) {
    uintptr_t area = mapAreaAt(root, ai);
    if (!area) { _snprintf(out, outsz, "%s", axs(AXS_MAP_TILE_UNREADABLE)); out[outsz - 1] = 0; return; }
    uint32_t id = 0; int32_t kind = -1;
    safeReadU32(area + AREA_ID_OFF, &id);
    safeReadU32(area + AREA_KIND_OFF, (uint32_t*)&kind);
    long tiles = mapAreaTiles(area);
    if (tiles <= 0) { _snprintf(out, outsz, "%s", axs(AXS_MAP_NO_TILES)); out[outsz - 1] = 0; return; }
    if (ti < 0) ti = 0; if (ti >= tiles) ti = (int)tiles - 1;
    uintptr_t tb = 0; safeReadPtr(area + AREA_TILES_BEG, &tb);
    uintptr_t tile = tb + (uintptr_t)ti * TILE_STRIDE;

    int32_t ty = -1, content = -1, nb = -1;
    safeReadU32(tile + TILE_TYPE_OFF, (uint32_t*)&ty);
    safeReadU32(tile + TILE_CONTENT_OFF, (uint32_t*)&content);
    safeReadU32(tile + TILE_NEIGH_ID_OFF, (uint32_t*)&nb);
    content = mapVisibleTileContent(area, ti, content);  // hunger, and an unfound secret door, read
                                                         // as empty — as they look on screen

    int o = _snprintf(out, outsz, axs(AXS_MAP_TILE_N_OF_M_FMT), ti + 1, tiles);
    if (ty == 2 && nb && nb != (int32_t)FOURCC_NONE) {   // junction: area on the far side
        char nlabel[64]; mapAreaLabelById(root, (uint32_t)nb, nlabel, sizeof nlabel);
        { o += _snprintf(out + o, outsz - o, ", "); o += _snprintf(out + o, outsz - o, axs(AXS_MAP_DOOR_TO_FMT), nlabel); }
    } else {
        uint32_t sid = 0;
        if (tileHiddenDoorShown(area, ti, &sid)) {
            char nlabel[64]; mapAreaLabelById(root, sid, nlabel, sizeof nlabel);
            { o += _snprintf(out + o, outsz - o, ", "); o += _snprintf(out + o, outsz - o, axs(AXS_MAP_DOOR_TO_FMT), nlabel); }
        }
    }
    if (tileContentsVisible(area, ti)) {
        char cl[128]; mapContentLabel(base, content, cl, sizeof cl);
        o += _snprintf(out + o, outsz - o, ", %s", cl);
    } else {
        o += _snprintf(out + o, outsz - o, ", %s", axs(AXS_MAP_NOT_SCOUTED));
    }
    int pa = 0, pt = 0; mapFindParty(root, &pa, &pt);
    bool here = ((long)pa == ai && pt == ti);
    if (!here && tileVisited(area, ti))
        o += _snprintf(out + o, outsz - o, ", %s", axs(AXS_MAP_VISITED));
    if (here)
        o += _snprintf(out + o, outsz - o, ", %s", axs(AXS_MAP_PARTY_HERE));
    out[outsz - 1] = 0;

    uint32_t know = 0;
    safeReadU32(tile + TILE_KNOWLEDGE_OFF, &know);
    logLine("mapread area=%ld tile=%d id=%08x kind=%d type=%d content=%d know=%u vis=%d -> \"%s\"",
            ai, ti, id, kind, ty, content, know, know > 1 ? 1 : 0, out);
}

void openMapReview(uintptr_t base, uintptr_t root) {                        // exported (internal.h)
    g_mapReview = true;
    mapFindParty(root, &g_mrArea, &g_mrTile);
    char adesc[400], tdesc[400], msg[MAILBOX_SZ];
    mapDescribeArea(base, root, g_mrArea, adesc, sizeof adesc);
    mapDescribeTile(base, root, g_mrArea, g_mrTile, tdesc, sizeof tdesc);
    _snprintf(msg, sizeof msg,
              "Map. %s. %s. Arrow keys move between rooms, "
              "Control and arrows walk the hallways.",
              adesc, tdesc);
    msg[sizeof msg - 1] = 0;
    postSpeech(msg);
    logLine("mapreview open (focus) areas=%ld startArea=%d startTile=%d panel=0x%llx",
            mapAreaCount(root), g_mrArea, g_mrTile, (unsigned long long)(uintptr_t)g_mapPanel);
}

static volatile bool g_overlayFocus = false;
void checkOverlayFocus(uintptr_t base) {                                    // exported (OurPoll)
    int64_t id = 0;
    if (!safeReadI64(base + FOCUS_ID_RVA, &id) || id < 0) { g_overlayFocus = false; return; }
    uint32_t cc = (uint32_t)(uint64_t)id;
    g_overlayFocus = (lookupPause(cc) != nullptr) ||
                     (cc - CONFIRM_ANSWER_BASE < (uint32_t)CONFIRM_ANSWER_MAX);
}

static bool panelIsInventory(uintptr_t rd, uintptr_t active) {
    uintptr_t p = 0;
    return safeReadPtr(rd + RD_PANEL_ARR_OFF + 16, &p) && p && p == active;
}

static const char* activePanelName(uintptr_t rd, uintptr_t active) {
    uintptr_t p = 0;
    if (safeReadPtr(rd + RD_PANEL_ARR_OFF + 8,  &p) && p == active && p) return "Hero";
    if (panelIsInventory(rd, active)) return "Inventory";
    return nullptr;
}

// ---- Tab out of a mod layer: name where the player LANDED ----
static bool     g_panelHandoff     = false;
static int      g_panelHandoffFrom = -1;      // the selector index at the moment of the press
static DWORD    g_panelHandoffAt   = 0;
static const DWORD PANEL_HANDOFF_MS = 250;

static bool mapPanelIsActive(uintptr_t* rdOut, int* idxOut, uintptr_t* activeOut) {
    if (rdOut) *rdOut = 0;
    if (idxOut) *idxOut = -1;
    if (activeOut) *activeOut = 0;

    uintptr_t rd = (uintptr_t)g_raidDisplay;
    if (rd <= 0x10000) return false;                    // not in a raid / banner hasn't ticked yet
    if (rdOut) *rdOut = rd;

    uintptr_t mapPanel = 0;
    if (!safeReadPtr(rd + RD_PANEL_ARR_OFF, &mapPanel) || mapPanel <= 0x10000) return false;
    g_mapPanel = reinterpret_cast<void*>(mapPanel);

    uint32_t idx = 0;
    if (!safeReadU32(rd + RD_ACTIVE_IDX_OFF, &idx)) return false;
    if ((int)idx < 0 || (int)idx >= RD_PANEL_COUNT) return false;   // 4 = "none" at construction
    if (idxOut) *idxOut = (int)idx;

    uintptr_t active = 0;
    if (!safeReadPtr(rd + RD_PANEL_ARR_OFF + (uintptr_t)idx * 8, &active)) return false;
    if (activeOut) *activeOut = active;

    return active == mapPanel;
}

void checkMapFocus(uintptr_t base) {                                        // exported (OurPoll)
    uintptr_t root = mapRoot(base);
    if (!root) {                                  // raid over: close and drop the stale pointers
        if (g_mapReview) {
            g_mapReview = false;
            logLine("mapreview auto-closed (raid ended)");
        }
        g_mapPanel     = nullptr;
        g_raidDisplay  = nullptr;
        g_mapSuspended = false;
        g_invActive    = false;
        g_invSlot      = -1;
        g_iuActive     = false;
        ruDisarmEquip("raid ended");
        ihRelease("raid ended");
        return;
    }

    uintptr_t rd = 0, active = 0;
    int idx = -1;
    bool mapActive = mapPanelIsActive(&rd, &idx, &active);

    bool invActive = !mapActive && rd && active && panelIsInventory(rd, active);
    if (invActive != g_invActive) {
        g_invActive = invActive;
        if (invActive) g_invSlot = -1;                  // unplaced: first arrow reads slot 1
        g_iuActive = false;                             // an aim never survives the panel changing
        if (!invActive) ruDisarmEquip("the bag stopped being the active panel");
        if (!invActive) ihRelease("the bag stopped being the active panel");
        logLine("invnav %s (idx=%d)", invActive ? "active" : "inactive", idx);
    }

    static int lastIdx = -2;
    bool changed   = (idx != lastIdx);
    bool firstRead = (lastIdx < 0);        // -2 = never read, -1 = was outside a raid
    if (changed) {
        lastIdx = idx;
        logLine("panelsel idx=%d rd=0x%llx active=0x%llx -> mapActive=%d",
                idx, (unsigned long long)rd, (unsigned long long)active, mapActive ? 1 : 0);
    }

    if (g_overlayFocus) {
        if (g_mapReview) {
            g_mapReview    = false;
            g_mapSuspended = true;
            logLine("mapreview suspended (overlay has focus)");
        }
        return;
    }

    if (mapActive) {
        if (!g_mapReview) {
            if (g_mapSuspended) {           // coming back from the pause menu — resume quietly
                g_mapReview    = true;
                g_mapSuspended = false;
                logLine("mapreview resumed (overlay gone)");
            } else {
                openMapReview(base, root);
            }
        }
        return;
    }

    // Some other panel owns the corner.
    if (g_mapReview) {
        g_mapReview = false;
        logLine("mapreview auto-closed (another panel is active, idx=%d)", idx);
    }
    g_mapSuspended = false;

    if (changed && !firstRead && idx >= 0 && !axOwnsAnnouncer(currentAxContext())) {
        const char* pn = activePanelName(rd, active);
        if (pn) {
            postSpeech(pn);
            logLine("panel announced: \"%s\" (idx=%d)", pn, idx);
            g_panelHandoff = false;
        }
    } else if (changed && !firstRead && idx >= 0) {
        logLine("panel change idx=%d NOT announced — ctx %d owns the announcer", idx,
                (int)currentAxContext());
    }
}

void armPanelHandoff(const char* why) {                                     // exported (internal.h)
    uintptr_t rd = 0, active = 0;
    int idx = -1;
    mapPanelIsActive(&rd, &idx, &active);
    g_panelHandoff     = true;
    g_panelHandoffFrom = idx;
    g_panelHandoffAt   = GetTickCount();
    logLine("panel handoff armed by %s (idx was %d)", why, idx);
}

void servicePanelHandoff(uintptr_t base) {                                  // exported (OurPoll)
    (void)base;
    if (!g_panelHandoff) return;

    if (g_overlayFocus || axOwnsAnnouncer(currentAxContext())) {
        g_panelHandoff = false;
        logLine("panel handoff dropped — ctx %d owns the announcer", (int)currentAxContext());
        return;
    }

    uintptr_t rd = 0, active = 0;
    int idx = -1;
    bool mapActive = mapPanelIsActive(&rd, &idx, &active);
    if (!rd) { g_panelHandoff = false; return; }          // left the raid: nothing to name

    if (mapActive) {
        g_panelHandoff = false;
        logLine("panel handoff satisfied by the map layer");
        return;
    }

    if (idx == g_panelHandoffFrom && (DWORD)(GetTickCount() - g_panelHandoffAt) < PANEL_HANDOFF_MS)
        return;

    g_panelHandoff = false;
    const char* pn = activePanelName(rd, active);
    if (!pn) {
        logLine("panel handoff: no name for the active panel (idx=%d active=0x%llx)",
                idx, (unsigned long long)active);
        return;
    }
    logLine("panel handoff announced: \"%s\" (idx=%d, was %d)", pn, idx, g_panelHandoffFrom);
    postSpeech(pn);
}

// ---- Scouting: say the word the game is showing ----
static bool     g_scoutShown  = false;   // last seen state of the panel's latch
static uint32_t g_scoutSaidAt = 0;       // GetTickCount() of the last spoken "Scouting"
static const uint32_t SCOUT_SAY_GAP_MS = 1500;   // ignore a re-latch inside this window

void serviceScoutWatch(uintptr_t base) {                                    // exported (OurPoll)
    uintptr_t panel = (uintptr_t)g_mapPanel;
    if (!panel) { g_scoutShown = false; return; }   // out of a raid: arm for the next one

    uint8_t shown = 0;
    if (!safeReadU8(panel + PANEL_SCOUT_SHOW_OFF, &shown)) return;
    bool now = (shown != 0);
    if (now == g_scoutShown) return;
    g_scoutShown = now;
    if (!now) return;                               // the overlay ended — nothing to say

    uint32_t t = GetTickCount();
    if (g_scoutSaidAt && (uint32_t)(t - g_scoutSaidAt) < SCOUT_SAY_GAP_MS) {
        logLine("scout watch: latch rose again %ums after the last one — not repeated",
                (unsigned)(t - g_scoutSaidAt));
        return;
    }
    g_scoutSaidAt = t;

    char word[96];
    if (!resolveKey(base, SCOUT_WORD_KEY, word, sizeof word) || !word[0]) {
        strncpy(word, axs(AXS_MAP_SCOUTING), sizeof word - 1);      // the key stopped shipping — still say it
        word[sizeof word - 1] = 0;
        logLine("scout watch: %s did not resolve, using the English fallback", SCOUT_WORD_KEY);
    }
    logLine("scout watch: overlay latched -> \"%s\"", word);
    postSpeech(word, false);
}

static bool mapTryCrossHiddenDoor(uintptr_t base, uintptr_t root, uintptr_t cur, int dir,
                                  char* buf, int bufsz) {
    uintptr_t tb = 0;
    if (!safeReadPtr(cur + AREA_TILES_BEG, &tb) || !tb) return false;
    uint32_t sid = 0;
    if (!tileHiddenDoorShown(cur, g_mrTile, &sid)) return false;
    long si = mapAreaIndexById(root, sid);
    if (si < 0) return false;                       // linked room not in the area vector
    float tx = 0, ty = 0, sx = 0, sy = 0;
    if (!tileXY(root, g_mrArea, g_mrTile, &tx, &ty)) return false;
    if (!tileXY(root, (long)si, 0, &sx, &sy)) return false;
    if (sx == tx && sy == ty) return false;
    if (deltaToDir(sx - tx, sy - ty) != dir) return false;
    g_mrArea = (int)si;
    g_mrTile = 0;
    mapDescribeArea(base, root, g_mrArea, buf, bufsz);
    logLine("mapnav cross-hidden-door dir=%d -> area=%d", dir, g_mrArea);
    return true;
}

static bool g_mapHomeKbLogged = false;
static void mapHomeGateLogOnce(uintptr_t base) {
    if (g_mapHomeKbLogged) return;
    g_mapHomeKbLogged = true;
    char owner[96];
    logLine("mapnav: Home -> %s",
            kbActionForKey(base, (int)SDLK_HOME, owner, sizeof owner)
                ? owner : "not bound in the game table");
}

void serviceMapReq(uintptr_t base) {                                        // exported (OurPoll)
    int req = g_mapReq;
    if (req == MR_NONE) return;
    g_mapReq = MR_NONE;

    uintptr_t root = mapRoot(base);
    if (req != MR_MOVE && req != MR_HOME) return;
    if (!g_mapReview) return;                       // movement only acts while the map is active
    if (!root) { g_mapReview = false; return; }     // raid ended: close silently

    if (req == MR_HOME) {
        mapHomeGateLogOnce(base);
        mapFindParty(root, &g_mrArea, &g_mrTile);
        char adesc[440], tdesc[400], hbuf[900];
        mapDescribeArea(base, root, g_mrArea, adesc, sizeof adesc);
        mapDescribeTile(base, root, g_mrArea, g_mrTile, tdesc, sizeof tdesc);
        _snprintf(hbuf, sizeof hbuf, "%s. %s", adesc, tdesc);
        hbuf[sizeof hbuf - 1] = 0;
        postSpeech(hbuf);
        logLine("mapnav home -> area=%d tile=%d", g_mrArea, g_mrTile);
        return;
    }

    int dir = g_moveDir;
    char buf[600];

    uintptr_t cur = mapAreaAt(root, g_mrArea);
    if (!cur) { postSpeech(axs(AXS_MAP_NO_MAP)); return; }
    int32_t kind = -1;
    safeReadU32(cur + AREA_KIND_OFF, (uint32_t*)&kind);
    long tiles = mapAreaTiles(cur);

    if (g_moveArea) {
        long destRoomIdx = -1; uint32_t unresolvedId = 0;
        long viaTiles = 0; bool viaHall = false;
        int landTile = -1;
        if (kind == 0) {
            MapExit exits[AREA_EXIT_SLOTS];
            int n = roomExits(root, g_mrArea, exits, AREA_EXIT_SLOTS);
            for (int i = 0; i < n; i++) {
                if (exits[i].dir != dir) continue;
                if (exits[i].farRoomIdx < 0) { unresolvedId = exits[i].farRoomId; continue; }
                destRoomIdx = exits[i].farRoomIdx;
                viaHall  = exits[i].corIdx >= 0;
                viaTiles = exits[i].corTiles;
                if (exits[i].farRoomIdx == exits[i].corIdx) landTile = exits[i].mouthTile;
                break;
            }
        } else {
            if (mapTryCrossHiddenDoor(base, root, cur, dir, buf, sizeof buf)) {
                postSpeech(buf);
                return;
            }
            uint32_t r0 = 0, r1 = 0;
            corridorEnds(cur, tiles, &r0, &r1);
            const int      endT[2] = { 0, (int)tiles - 1 };
            const uint32_t endR[2] = { r0, r1 };
            for (int e = 0; e < 2; e++) {
                if (!endR[e]) continue;
                if (corridorDirToEnd(root, g_mrArea, g_mrTile, endT[e], tiles) != dir) continue;
                destRoomIdx = mapAreaIndexById(root, endR[e]);
                if (destRoomIdx < 0) unresolvedId = endR[e];
                break;
            }
        }
        if (destRoomIdx < 0) {
            postSpeech(axs(unresolvedId ? AXS_MAP_NOT_MAPPED : AXS_MAP_NO_EXIT_THAT_WAY));
            if (unresolvedId)
                logLine("mapnav room-step dir=%d: far room %08x not in the area vector",
                        dir, unresolvedId);
            return;
        }
        g_mrArea = (int)destRoomIdx;
        g_mrTile = (landTile >= 0) ? landTile : 0;
        char adesc[440];
        mapDescribeArea(base, root, g_mrArea, adesc, sizeof adesc);
        if (landTile >= 0) {
            char tdesc[400];
            mapDescribeTile(base, root, g_mrArea, g_mrTile, tdesc, sizeof tdesc);
            _snprintf(buf, sizeof buf, "%s. %s", adesc, tdesc);
        }
        else if (viaHall) { _snprintf(buf, sizeof buf, "%s. ", adesc);
                          _snprintf(buf + strlen(buf), sizeof buf - strlen(buf), axs(AXS_MAP_VIA_HALLWAY_FMT), viaTiles); }
        else              _snprintf(buf, sizeof buf, "%s.", adesc);
        buf[sizeof buf - 1] = 0;
        postSpeech(buf);
        logLine("mapnav room-step dir=%d -> area=%d tile=%d", dir, g_mrArea, g_mrTile);
        return;
    }

    if (kind == 0) {
        MapExit exits[AREA_EXIT_SLOTS];
        int n = roomExits(root, g_mrArea, exits, AREA_EXIT_SLOTS);
        const MapExit* pick = nullptr;
        for (int i = 0; i < n; i++)
            if (exits[i].dir == dir && exits[i].corIdx >= 0) { pick = &exits[i]; break; }
        if (!pick) { postSpeech(axs(AXS_MAP_NO_HALLWAY)); return; }
        g_mrArea = (int)pick->corIdx;
        g_mrTile = pick->mouthTile;
        char alabel[96], tdesc[400];
        mapAreaLabelEx(root, mapAreaAt(root, g_mrArea), alabel, sizeof alabel);
        mapDescribeTile(base, root, g_mrArea, g_mrTile, tdesc, sizeof tdesc);
        _snprintf(buf, sizeof buf, "%s. %s", alabel, tdesc);
        buf[sizeof buf - 1] = 0;
        postSpeech(buf);
        logLine("mapnav enter-hall dir=%d -> area=%d tile=%d", dir, g_mrArea, g_mrTile);
        return;
    }

    if (mapTryCrossHiddenDoor(base, root, cur, dir, buf, sizeof buf)) {
        postSpeech(buf);
        return;
    }
    uint32_t r0 = 0, r1 = 0;
    corridorEnds(cur, tiles, &r0, &r1);
    int toward = -1;                                     // end tile we are stepping toward
    if      (corridorDirToEnd(root, g_mrArea, g_mrTile, 0,               tiles) == dir) toward = 0;
    else if (corridorDirToEnd(root, g_mrArea, g_mrTile, (int)tiles - 1,  tiles) == dir) toward = (int)tiles - 1;
    if (toward < 0) { postSpeech(axs(AXS_MAP_HALLWAY_WRONG_WAY)); return; }

    if (g_mrTile == toward) {
        // Already at this end: cross into the room, if it is known.
        uint32_t roomId = (toward == 0) ? r0 : r1;
        long ri = roomId ? mapAreaIndexById(root, roomId) : -1;
        if (ri < 0) { postSpeech(axs(AXS_MAP_NO_EXIT_THAT_WAY)); return; }
        g_mrArea = (int)ri;
        g_mrTile = 0;
        mapDescribeArea(base, root, g_mrArea, buf, sizeof buf);
        postSpeech(buf);
        logLine("mapnav exit-hall dir=%d -> area=%d", dir, g_mrArea);
        return;
    }
    g_mrTile += (toward > g_mrTile) ? 1 : -1;
    mapDescribeTile(base, root, g_mrArea, g_mrTile, buf, sizeof buf);
    postSpeech(buf);
}

bool routeMapKey(uintptr_t base, uint32_t sym, uint16_t mod, uint8_t repeat) {  // exported (the table)
    (void)base;
    if (sym == SDLK_RETURN || sym == SDLK_KP_ENTER) {
        if (mod & (KMOD_LALT | KMOD_RALT)) return false;   // leave Alt+Enter to the game
        if (!repeat) g_moveActivateReq = true;             // serviced on the main thread
        return true;
    }
    if (sym == SDLK_HOME) {
        if (mod & (KMOD_LALT | KMOD_RALT)) return false;   // Alt+Home isn't ours
        if (!repeat) g_mapReq = MR_HOME;                   // serviced on the main thread
        return true;                                       // swallow the repeat too
    }
    int dir;
    switch (sym) {
        case SDLK_UP:    dir = 0; break;
        case SDLK_DOWN:  dir = 1; break;
        case SDLK_LEFT:  dir = 2; break;
        case SDLK_RIGHT: dir = 3; break;
        default:         return false;   // not an arrow -> let the game have it
    }
    if (mod & (KMOD_LALT | KMOD_RALT)) return false;   // Alt+arrow isn't ours
    if (repeat) return true;
    g_moveDir  = dir;
    g_moveArea = (mod & (KMOD_LCTRL | KMOD_RCTRL)) == 0;
    g_mapReq   = MR_MOVE;
    return true;
}

// ---- In-raid MAP: Enter-to-move (the game's own move fn, with the button's own objects) ----
static bool partyWorld(uintptr_t root, float* x, float* y) {
    uint32_t xb = 0, yb = 0;
    if (!safeReadU32(root + MAP_PARTY_X_OFF, &xb)) return false;
    if (!safeReadU32(root + MAP_PARTY_Y_OFF, &yb)) return false;
    *x = u32AsFloatM(xb); *y = u32AsFloatM(yb);
    return true;
}

static bool mapTileRender(uintptr_t base, uintptr_t root, long ai, int ti, float* rx, float* ry) {
    float tx = 0, ty = 0;
    if (!tileXY(root, ai, ti, &tx, &ty)) return false;
    uint32_t s = 0, c = 0;
    safeReadU32(base + MAP_PROJ_SCALE_RVA, &s); safeReadU32(base + MAP_PROJ_CONST_RVA, &c);
    float scale = u32AsFloatM(s), C = u32AsFloatM(c);
    *rx = scale * (tx + C);
    *ry = scale * (ty + C);
    return true;
}

static bool mapTilePixel(uintptr_t base, uintptr_t root, long ai, int ti, float* px, float* py) {
    float tx = 0, ty = 0;
    if (!tileXY(root, ai, ti, &tx, &ty)) return false;
    uint32_t s = 0, c = 0, panx = 0, pany = 0;
    safeReadU32(base + MAP_PROJ_SCALE_RVA, &s);  safeReadU32(base + MAP_PROJ_CONST_RVA, &c);
    safeReadU32(base + MAP_PROJ_PANX_RVA, &panx); safeReadU32(base + MAP_PROJ_PANY_RVA, &pany);
    float scale = u32AsFloatM(s), C = u32AsFloatM(c);
    *px = scale * (tx + C) + u32AsFloatM(panx);
    *py = scale * (ty + C) + u32AsFloatM(pany);
    return true;
}

static void fourccStr(int64_t id, char out[5]) {
    uint32_t v = (uint32_t)id;
    for (int i = 0; i < 4; i++) {
        char ch = (char)((v >> (8 * i)) & 0xff);
        out[i] = (ch >= 0x20 && ch < 0x7f) ? ch : '.';
    }
    out[4] = 0;
}

typedef char (*MoveGateFn)();
typedef void (*MoveDoFn)(void*, int*, void*, unsigned char, unsigned char);
static bool sehMoveGate(uintptr_t base) {
    char ok = 0;
    __try { ok = ((MoveGateFn)(base + MOVE_GATE_RVA))(); }
    __except (EXCEPTION_EXECUTE_HANDLER) { ok = 0; }
    return ok != 0;
}
static bool sehMoveCall(uintptr_t base, void* mgr, int* door, void* destArea) {
    __try { ((MoveDoFn)(base + MOVE_FN_RVA))(mgr, door, destArea, 1, 0); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

static uintptr_t mapDoorToward(uintptr_t root, uintptr_t curArea, uint32_t destId) {
    for (int s = 0; s < AREA_EXIT_SLOTS; s++) {
        uintptr_t door = curArea + AREA_EXITS_OFF + (uintptr_t)s * AREA_EXIT_STRIDE;
        uint32_t via = 0;
        if (!safeReadU32(door + DOOR_DEST_OFF, &via) || !via || via == FOURCC_NONE) continue;
        if (via == destId) return door;                     // door straight into the target area

        uintptr_t link = mapAreaById(root, via); if (!link) continue;
        int32_t lkind = -1; safeReadU32(link + AREA_KIND_OFF, reinterpret_cast<uint32_t*>(&lkind));
        if (lkind != 1) continue;                           // only corridors bridge two rooms
        uintptr_t tb = 0; safeReadPtr(link + AREA_TILES_BEG, &tb);
        long tiles = mapAreaTiles(link);
        if (!tb || tiles <= 0) continue;
        // Only a corridor's two END tiles can be junctions onto a room.
        for (int e = 0; e < 2; e++) {
            uintptr_t tile = tb + (uintptr_t)(e ? tiles - 1 : 0) * TILE_STRIDE;
            int32_t ttype = -1; uint32_t tneigh = 0;
            safeReadU32(tile + TILE_TYPE_OFF, reinterpret_cast<uint32_t*>(&ttype));
            safeReadU32(tile + TILE_NEIGH_ID_OFF, &tneigh);
            if (ttype == 2 && tneigh == destId) return door;
        }
    }
    return 0;
}

static void attemptRoomMove(uintptr_t base, uintptr_t root) {
    if (g_movePending) return;

    if (comIsWaveRaid(base)) {
        logLine("map-move: wave raid — the map's move-to-room is refused; the way on is the "
                "dungeon view's last row");
        postSpeech(axs(AXS_MAP_FARMSTEAD_NO_MOVE));
        return;
    }

    uintptr_t area = mapAreaAt(root, g_mrArea);
    if (!area) { postSpeech(axs(AXS_MAP_CANT_MOVE)); return; }
    uint32_t id = 0; int32_t kind = -1;
    safeReadU32(area + AREA_ID_OFF, &id);
    safeReadU32(area + AREA_KIND_OFF, reinterpret_cast<uint32_t*>(&kind));

    uintptr_t curArea = 0;
    if (!safeReadPtr(root + MAP_CUR_AREA_PTR, &curArea) || !curArea) {
        logLine("map-move: no current area -> can't move there");
        postSpeech(axs(AXS_MAP_CANT_MOVE)); return;
    }
    uint32_t curId = 0; int32_t curKind = -1;
    safeReadU32(curArea + AREA_ID_OFF, &curId);
    safeReadU32(curArea + AREA_KIND_OFF, reinterpret_cast<uint32_t*>(&curKind));
    if (kind != 0 || id == curId) { postSpeech(axs(AXS_MAP_CANT_MOVE)); return; }

    mapAreaLabel(id, kind, g_movePendLabel, sizeof g_movePendLabel);

    uintptr_t door = (curKind == 0) ? mapDoorToward(root, curArea, id) : 0;
    if (!door) {
        if (isSecretAreaId(id)) {
            logLine("map-move: '%.4s' is a secret room - no map-move into one exists; "
                    "entry is the hallway's door tile", &id);
            postSpeech(axs(AXS_SECRET_ENTER_FROM_HALL));
            return;
        }
        logLine("map-move: no door from '%.4s' (kind=%d) to '%.4s' -> can't move there",
                &curId, curKind, &id);
        postSpeech(axs(AXS_MAP_CANT_MOVE)); return;
    }

    uintptr_t mgr = 0;
    if (!safeReadPtr(base + MOVE_MGR_RVA, &mgr) || !mgr) {
        logLine("map-move: no move manager -> can't move there");
        postSpeech(axs(AXS_MAP_CANT_MOVE)); return;
    }
    if (!sehMoveGate(base)) {
        logLine("map-move: gate refused (\"%s\")", g_movePendLabel);
        postSpeech(axs(AXS_MAP_CANT_MOVE)); return;
    }

    // Snapshot party state so serviceMapMove reports the outcome.
    float pwx = 0, pwy = 0; partyWorld(root, &pwx, &pwy);
    g_movePendPrevArea = (int32_t)curId; g_movePendPrevX = pwx; g_movePendPrevY = pwy;
    g_movePending = true;
    g_movePendDeadline = GetTickCount() + 1500;

    uint32_t dvia = 0, dtile = 0;
    safeReadU32(door + DOOR_DEST_OFF, &dvia); safeReadU32(door + DOOR_TILE_OFF, &dtile);
    logLine("map-move: '%.4s' -> '%.4s' via door {'%.4s', tile %u}", &curId, &id, &dvia, dtile);
    if (!sehMoveCall(base, reinterpret_cast<void*>(mgr), reinterpret_cast<int*>(door),
                     reinterpret_cast<void*>(area))) {
        logLine("map-move: move call faulted");
        g_movePending = false;
        postSpeech(axs(AXS_MAP_CANT_MOVE));
    }
}

static const uintptr_t PANEL_PAN_BUSY_OFF = 0xafc;
static bool g_panStoodAside = false;                 // edge memory for the two log lines
void checkMapPan(uintptr_t base) {                                          // exported (OurPoll)
    if (!g_mapReview) return;
    uintptr_t panel = (uintptr_t)g_mapPanel;
    uint8_t busy = 0;
    if (panel && safeReadU8(panel + PANEL_PAN_BUSY_OFF, &busy) && busy) {
        if (!g_panStoodAside) {
            g_panStoodAside = true;
            logLine("map pan: the panel is panning to the party -- standing aside until it arrives");
        }
        return;
    }
    if (g_panStoodAside) {
        g_panStoodAside = false;
        logLine("map pan: the panel's pan arrived -- centering the cursor tile again");
    }
    uintptr_t root = mapRoot(base); if (!root) return;
    float rx = 0, ry = 0;
    if (!mapTileRender(base, root, g_mrArea, g_mrTile, &rx, &ry)) return;
    safeWriteU32(base + MAP_PAN2_X_RVA, *reinterpret_cast<uint32_t*>(&rx));
    safeWriteU32(base + MAP_PAN2_Y_RVA, *reinterpret_cast<uint32_t*>(&ry));
}

void serviceMapMove(uintptr_t base) {                                       // exported (OurPoll)
    if (g_moveActivateReq) {
        g_moveActivateReq = false;
        uintptr_t root = mapRoot(base);
        if (g_mapReview && root) attemptRoomMove(base, root);
    }
    if (!g_movePending) return;

    uintptr_t root = mapRoot(base);
    bool moved = false;
    if (root) {
        uintptr_t ca = 0; int32_t curA = 0;
        if (safeReadPtr(root + MAP_CUR_AREA_PTR, &ca) && ca)
            safeReadU32(ca + AREA_ID_OFF, reinterpret_cast<uint32_t*>(&curA));
        float x = 0, y = 0; partyWorld(root, &x, &y);
        float dx = x - g_movePendPrevX; if (dx < 0) dx = -dx;
        float dy = y - g_movePendPrevY; if (dy < 0) dy = -dy;
        if (curA != g_movePendPrevArea || dx > 0.01f || dy > 0.01f) moved = true;
    } else {
        moved = true;
    }

    if (moved) {
        char buf[128];
        _snprintf(buf, sizeof buf, axs(AXS_MAP_MOVING_TO_FMT), g_movePendLabel);
        buf[sizeof buf - 1] = 0;
        postSpeech(buf);
        g_movePending = false;
        logLine("map-move: party started moving -> \"%s\"", g_movePendLabel);
    } else if ((int32_t)(GetTickCount() - g_movePendDeadline) >= 0) {
        postSpeech(axs(AXS_MAP_CANT_MOVE));
        g_movePending = false;
        logLine("map-move: no movement within window -> can't move there");
    }
}

// ---- TILE-STEP MOVEMENT — Shift+D forward, Shift+A back ----
static const uint32_t  TW_RELEASE_CHECK_MS = 250;       // by then the queued KEYUP must have landed
static const uint32_t  TW_STUCK_MS         = 1000;      // the party's position has not changed for this
static const uint32_t  TW_HARD_MS          = 15000;     // absolute cap: never hold a key longer than this
static const uint32_t  TW_TRAP_ACK_MS      = 10000;     // "press again" window for walking onto a
                                                        // warned trap; after this the warning re-arms
static const float     TW_TILE_AIM         = 0.12f;     // WHERE IN THE DESTINATION TILE A PLAIN STEP
static const float     TW_TRAP_GAP         = 0.12f;     // a trap approach parks this fraction of a tile
static const uint32_t  TW_STOP_SLACK_MS    = 20;        // until a coast has been measured, stop this
                                                        // many milliseconds of travel early

typedef char (*KeyIsDownFn)(unsigned int);

bool sehPartyTile(uintptr_t base, uintptr_t root, int* out) {
    __try {
        uintptr_t area = *(uintptr_t*)(root + MAP_CUR_AREA_PTR);
        if (!area) { *out = 0; return true; }
        float pos   = *(float*)(root + MAP_PARTY_POS_OFF);       // the position scalar (unchanged)
        float kOff  = *(float*)(base + PARTY_TILE_K_OFFSET_RVA);
        float kScl  = *(float*)(base + PARTY_TILE_K_SCALE_RVA);
        int idx = (int)((pos + kOff) * kScl);
        if (idx < 0) idx = 0;
        else {
            uintptr_t tb = *(uintptr_t*)(area + AREA_TILES_BEG), te = *(uintptr_t*)(area + AREA_TILES_END);
            int count = (te > tb) ? (int)((te - tb) / TILE_STRIDE) : 0;
            if (idx >= count) idx = count - 1;
            if (idx < 0) idx = 0;                                   // an area with no tiles: the game would
        }                                                           // read tile -1; clamp instead
        *out = idx;
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
static bool sehKeyIsDown(uintptr_t base, uint32_t keycode, bool* out) {
    __try { *out = ((KeyIsDownFn)(base + KEY_IS_DOWN_RVA))((unsigned int)keycode) != 0; return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// ---- SCOUT DIAGNOSTIC: WHAT is in the panel's scouting queue when it fills ----
static const uintptr_t PM_QUEUE_A_OFF   = 0x268;   // Panel_Map+: FogOfWar pending queue A (scouting)
static const uintptr_t PM_QUEUE_B_OFF   = 0x2a0;   // Panel_Map+: pending queue B (silent)
static const uintptr_t DQ_MAP_OFF       = 0x08;    // std::deque+: block map
static const uintptr_t DQ_MAPSIZE_OFF   = 0x10;    // std::deque+: block count (power of two)
static const uintptr_t DQ_FRONT_OFF     = 0x18;    // std::deque+: index of the front element
static const uintptr_t DQ_SIZE_OFF      = 0x20;    // std::deque+: element count
static const uintptr_t AREA_REVERSED_OFF_DIAG = 0x188;   // area+: byte, tile array reversed
static const uint32_t  SCAN_LSHIFT = 225, SCAN_RSHIFT = 229;   // SDL scancodes (fixed, not layout)
static uintptr_t g_sdPanel = 0;
static uint64_t  g_sdPrevA = 0, g_sdPrevB = 0;

static bool sdReadU64(uintptr_t addr, uint64_t* out) {
    int64_t v = 0;
    if (!safeReadI64(addr, &v)) return false;
    *out = (uint64_t)v; return true;
}

static bool sdEntry(uintptr_t dq, uint64_t i, char* out, int outsz) {
    uintptr_t map = 0; uint64_t mapsz = 0, front = 0;
    if (!safeReadPtr(dq + DQ_MAP_OFF, &map) || !map) return false;
    if (!sdReadU64(dq + DQ_MAPSIZE_OFF, &mapsz) || !mapsz || (mapsz & (mapsz - 1))) return false;
    if (!sdReadU64(dq + DQ_FRONT_OFF, &front)) return false;
    uintptr_t el = 0;
    if (!safeReadPtr(map + ((front + i) & (mapsz - 1)) * 8, &el) || !el) return false;
    uint32_t type = 0xffff, tile = 0xffff, id = 0, know = 0xffff, content = 0xffff;
    int32_t kind = -1;
    safeReadU32(el, &type); safeReadU32(el + 0x10, &tile);
    uintptr_t sad = 0, area = 0;
    if (safeReadPtr(el + 8, &sad) && sad && safeReadPtr(sad, &area) && area) {
        safeReadU32(area + AREA_ID_OFF, &id);
        safeReadU32(area + AREA_KIND_OFF, (uint32_t*)&kind);
        uint8_t rev = 0; safeReadU8(area + AREA_REVERSED_OFF_DIAG, &rev);
        long n = mapAreaTiles(area);
        long ti = rev ? (n - 1 - (long)tile) : (long)tile;
        uintptr_t tb = 0;
        if (ti >= 0 && ti < n && safeReadPtr(area + AREA_TILES_BEG, &tb) && tb) {
            safeReadU32(tb + (uintptr_t)ti * TILE_STRIDE + TILE_KNOWLEDGE_OFF, &know);
            safeReadU32(tb + (uintptr_t)ti * TILE_STRIDE + TILE_CONTENT_OFF, &content);
        }
    }
    _snprintf(out, outsz, "type=%u area='%.4s' kind=%d tile=%u know=%u content=%u",
              type, (const char*)&id, kind, tile, know, content);
    out[outsz - 1] = 0;
    return true;
}

void serviceScoutDiag(uintptr_t base) {                                     // exported (OurPoll)
    if (!axDebugLogEnabled()) return;                // diagnostic: nothing runs with the log off
    uintptr_t panel = (uintptr_t)g_mapPanel;
    if (!panel) { g_sdPanel = 0; g_sdPrevA = g_sdPrevB = 0; return; }
    if (panel != g_sdPanel) { g_sdPanel = panel; g_sdPrevA = g_sdPrevB = 0; }

    uint32_t stR = 0, stL = 0, stQ = 0, stS = 0;
    safeReadU32(base + KEY_STATE_ARRAY_RVA + SCAN_RSHIFT * 4, &stR);
    safeReadU32(base + KEY_STATE_ARRAY_RVA + SCAN_LSHIFT * 4, &stL);
    uint32_t scQ = klScancodeForKey(0x71), scS = klScancodeForKey(0x73);
    if (scQ && scQ < 256) safeReadU32(base + KEY_STATE_ARRAY_RVA + scQ * 4, &stQ);
    if (scS && scS < 256) safeReadU32(base + KEY_STATE_ARRAY_RVA + scS * 4, &stS);
    bool rightShiftHeld = (stR == 1 || stR == 2);
    if (rightShiftHeld && (stQ == 2 || stS == 2))
        logLine("scoutdiag: DEVELOPER SCOUT CHORD -- Right Shift held and '%c' pressed this frame "
                "(the game scouts on this while the map panel is ticking)", stQ == 2 ? 'q' : 's');

    uint64_t a = 0, b = 0;
    if (!sdReadU64(panel + PM_QUEUE_A_OFF + DQ_SIZE_OFF, &a) ||
        !sdReadU64(panel + PM_QUEUE_B_OFF + DQ_SIZE_OFF, &b)) return;
    if (a > 4096 || b > 4096) return;                // mid-construction garbage, not a queue
    if (a == g_sdPrevA && b == g_sdPrevB) return;

    uintptr_t rd = (uintptr_t)g_raidDisplay;
    uint32_t state = 0xffff, idx = 0xffff;
    if (rd > 0x10000) { safeReadU32(rd + 0x40b0, &state); safeReadU32(rd + RD_ACTIVE_IDX_OFF, &idx); }
    uintptr_t root = mapRoot(base), live = 0; uint32_t pid = 0;
    if (root && safeReadPtr(root + MAP_CUR_AREA_PTR, &live) && live > 0x10000)
        safeReadU32(live + AREA_ID_OFF, &pid);
    uint8_t latch = 0; safeReadU8(panel + PANEL_SCOUT_SHOW_OFF, &latch);
    logLine("scoutdiag: queue A %llu -> %llu, queue B %llu -> %llu | panel idx=%u raidstate=%u "
            "party='%.4s' latch=%u | keys RSHIFT=%u LSHIFT=%u q=%u s=%u",
            (unsigned long long)g_sdPrevA, (unsigned long long)a,
            (unsigned long long)g_sdPrevB, (unsigned long long)b,
            idx, state, (const char*)&pid, latch, stR, stL, stQ, stS);
    char line[200];
    if (a > g_sdPrevA) {
        for (uint64_t i = g_sdPrevA; i < a && i < g_sdPrevA + 16; i++)
            if (sdEntry(panel + PM_QUEUE_A_OFF, i, line, sizeof line))
                logLine("scoutdiag:   A[%llu] %s   <- SCOUTING queue: this plays the sound",
                        (unsigned long long)i, line);
    }
    if (b > g_sdPrevB) {
        for (uint64_t i = g_sdPrevB; i < b && i < g_sdPrevB + 16; i++)
            if (sdEntry(panel + PM_QUEUE_B_OFF, i, line, sizeof line))
                logLine("scoutdiag:   B[%llu] %s   (silent queue)", (unsigned long long)i, line);
    }
    g_sdPrevA = a; g_sdPrevB = b;
}

// ---- The step ----
bool            g_twStepping    = false;   // a mod-driven step is in flight (exported: the
                                           //   router asks it before every other key)
uint32_t        g_twSym       = 0;
uint32_t        g_twScan      = 0;
static int      g_twStartTile = -1;      // tile index when the step began
static uint32_t g_twStartArea = 0;       // area id when the step began
static bool     g_twVerified  = false;   // the game was OBSERVED to have the key down
static uint32_t g_twRelSym    = 0;       // a release we queued and are still verifying
static uint32_t g_twRelScan   = 0;
static uint32_t g_twRelDeadline = 0;

// ---- WHERE A STEP STOPS: the game's own 1-D travel coordinate ----
static bool twPosModel(uintptr_t base, uintptr_t root, float* pos, float* kOff, float* kScl) {
    uint32_t p = 0, o = 0, s = 0;
    if (!safeReadU32(root + MAP_PARTY_POS_OFF, &p))       return false;
    if (!safeReadU32(base + PARTY_TILE_K_OFFSET_RVA, &o)) return false;
    if (!safeReadU32(base + PARTY_TILE_K_SCALE_RVA, &s))  return false;
    *pos = u32AsFloatM(p); *kOff = u32AsFloatM(o); *kScl = u32AsFloatM(s);
    return *kScl > 1e-6f && *kScl < 1e6f;
}
static float twPosAt(float kOff, float kScl, int tile, float frac) {
    return ((float)tile + frac) / kScl - kOff;
}

static uint32_t g_twTrapAckArea = 0;
static int      g_twTrapAckTile = -1;
static uint32_t g_twTrapAckSym  = 0;
static uint32_t g_twTrapAckUntil = 0;
                                         // silently authorise a walk onto spikes

static bool     g_twTrapShort    = false;  // the in-flight step is a trap approach
static uint32_t g_twTrapTileArea = 0;      // where the approach stopped short...
static int      g_twTrapTileIdx  = -1;
static uint32_t g_twTrapTileSym  = 0;

// ---- The AIM: where this step is trying to end up, in the travel coordinate above ----
static uintptr_t g_twBase       = 0;
                                           //   read the position without being handed one
static int      g_twDir         = 0;       // +1 for 'd' (pos rises), -1 for 'a'
static int      g_twDestTile    = -1;      // the tile the aim point is in (-1 when unaimed)
static bool     g_twAimed       = false;
static float    g_twTarget      = 0;       // the aim point
static float    g_twTileW       = 0;       // one tile, in the same units
static float    g_twStartPos    = 0;       // position when the key went down...
static uint32_t g_twStartTick   = 0;
static float    g_twSpeed       = 0;       //   the walk applies a constant delta per frame
static float    g_twLastPos     = 0;       // the no-progress watch: the last position seen...
static uint32_t g_twMoveTick    = 0;       // ...and the last tick it CHANGED on
static uint32_t g_twHardDeadline = 0;
static uintptr_t g_twPropAim    = 0;

static bool     g_twCoastValid  = false;
static float    g_twCoast       = 0;
static bool     g_twMeasure     = false;   // a release is pending measurement...
static float    g_twRelPos      = 0;       // ...from here...
static uint32_t g_twRelArea     = 0;       // ...in this area (pos is per-area; an area change
                                           //    resets it and the sample would be nonsense)

static uint32_t twAreaId(uintptr_t root) {
    uintptr_t area = 0;
    if (!safeReadPtr(root + MAP_CUR_AREA_PTR, &area) || area <= 0x10000) return 0;
    uint32_t id = 0; safeReadU32(area + AREA_ID_OFF, &id);
    return id;
}

static void twClearStep() {
    g_twStepping = false; g_twVerified = false; g_twTrapShort = false;
    g_twSym = 0; g_twScan = 0; g_twStartTile = -1; g_twStartArea = 0;
    g_twAimed = false; g_twDir = 0; g_twDestTile = -1; g_twTarget = 0;
    g_twSpeed = 0; g_twMoveTick = 0; g_twHardDeadline = 0; g_twPropAim = 0;
}

static void twRelease(const char* why) {
    if (!g_twStepping) return;
    enqueueSynthKey(SDL_EVT_KEYUP, g_twScan, g_twSym, 0);
    g_twRelSym = g_twSym; g_twRelScan = g_twScan;
    g_twRelDeadline = GetTickCount() + TW_RELEASE_CHECK_MS;
    g_twMeasure = false;
    if (g_twBase) {
        uintptr_t root = mapRoot(g_twBase);
        float pos = 0, kOff = 0, kScl = 0;
        if (root && twPosModel(g_twBase, root, &pos, &kOff, &kScl)) {
            g_twRelPos = pos; g_twRelArea = twAreaId(root); g_twMeasure = true;
        }
    }
    logLine("tilestep: released '%c' (scan=%u) - %s", (char)g_twSym, g_twScan, why ? why : "");
    twClearStep();
}

void twInterrupt(uint32_t bySym) {                                          // exported (the router)
    if (!g_twStepping) return;
    if (bySym == g_twSym) {
        logLine("tilestep: player took '%c' over by hand - dropping the step, key stays down",
                (char)g_twSym);
        twClearStep();
        return;
    }
    twRelease("interrupted by another key");
}

static void twTileLine(uintptr_t base, uintptr_t root, uintptr_t area, int tile,
                       char* out, int outsz) {
    long tiles = mapAreaTiles(area);
    uintptr_t tb = 0; safeReadPtr(area + AREA_TILES_BEG, &tb);
    int o = _snprintf(out, outsz, axs(AXS_MAP_TILE_N_OF_M_FMT), tile + 1, tiles > 0 ? tiles : 1);
    if (tb && tile >= 0 && (tiles <= 0 || tile < tiles)) {
        uintptr_t t = tb + (uintptr_t)tile * TILE_STRIDE;
        int32_t ty = -1, content = -1, nb = -1;
        safeReadU32(t + TILE_TYPE_OFF,     (uint32_t*)&ty);
        safeReadU32(t + TILE_CONTENT_OFF,  (uint32_t*)&content);
        safeReadU32(t + TILE_NEIGH_ID_OFF, (uint32_t*)&nb);
        content = mapVisibleTileContent(area, tile, content);
        if (ty == 2 && nb && nb != (int32_t)FOURCC_NONE) {      // junction: what is through it
            char nl[64]; mapAreaLabelById(root, (uint32_t)nb, nl, sizeof nl);
            { o += _snprintf(out + o, outsz - o, ", "); o += _snprintf(out + o, outsz - o, axs(AXS_MAP_DOOR_TO_FMT), nl); }
        } else {
            uint32_t sid = 0;                        // hidden door: same clause, once SHOWN
            if (tileHiddenDoorShown(area, tile, &sid)) {
                char nl[64]; mapAreaLabelById(root, sid, nl, sizeof nl);
                { o += _snprintf(out + o, outsz - o, ", "); o += _snprintf(out + o, outsz - o, axs(AXS_MAP_DOOR_TO_FMT), nl); }
            }
        }
        if (content > 0) {                                      // 0 = nothing; say nothing of it
            char cl[128]; mapContentLabel(base, content, cl, sizeof cl);
            o += _snprintf(out + o, outsz - o, ", %s", cl);
        }
    }
    out[outsz - 1] = 0;
}

static void twRoomLine(uintptr_t base, uintptr_t root, uintptr_t area, char* out, int outsz) {
    int o = 0;
    { char alabel[96]; mapAreaLabelEx(root, area, alabel, sizeof alabel);
      o = _snprintf(out, outsz, "%s", alabel); }
    uintptr_t tb = 0; safeReadPtr(area + AREA_TILES_BEG, &tb);
    if (tb) {
        int32_t content = -1;
        safeReadU32(tb + TILE_CONTENT_OFF, (uint32_t*)&content);
        content = mapVisibleTileContent(area, 0, content);
        if (content > 0) {
            char cl[128]; mapContentLabel(base, content, cl, sizeof cl);
            o += _snprintf(out + o, outsz - o, ", %s", cl);
        }
    }
    out[outsz - 1] = 0;
}

static bool areaIsRoom(uintptr_t area) {
    int32_t kind = -1;
    return safeReadU32(area + AREA_KIND_OFF, (uint32_t*)&kind) && kind == 0;
}

static void twAnnounceArrival(uintptr_t base, uintptr_t root, int tile, bool newArea) {
    uintptr_t area = 0;
    char msg[MAILBOX_SZ];
    if (!safeReadPtr(root + MAP_CUR_AREA_PTR, &area) || area <= 0x10000) {
        _snprintf(msg, sizeof msg, axs(AXS_MAP_TILE_N_FMT), tile + 1);
    } else if (areaIsRoom(area)) {
        char rdesc[400]; twRoomLine(base, root, area, rdesc, sizeof rdesc);
        _snprintf(msg, sizeof msg, "%s.", rdesc);
    } else {
        char tdesc[400]; twTileLine(base, root, area, tile, tdesc, sizeof tdesc);
        if (newArea) {
            char alabel[96]; mapAreaLabelEx(root, area, alabel, sizeof alabel);
            _snprintf(msg, sizeof msg, "%s. %s.", alabel, tdesc);
        } else {
            _snprintf(msg, sizeof msg, "%s.", tdesc);
        }
    }
    msg[sizeof msg - 1] = 0;
    logLine("tilestep: arrived tile=%d newArea=%d -> \"%s\"", tile, newArea ? 1 : 0, msg);
    postSpeech(msg);
}

static void twAnnounceStuck(uintptr_t base, uintptr_t root, bool keyWasHeld) {
    uint32_t combat = 0;
    safeReadU32(root + RAID_IN_COMBAT_OFF, &combat);
    const char* why = combat      ? "Not while fighting."
                    : !keyWasHeld ? "The game didn't take the movement key."
                                  : "The party didn't move.";
    logLine("tilestep: no movement in %ums (combat=%u keyHeld=%d) -> \"%s\"",
            TW_STUCK_MS, combat, keyWasHeld ? 1 : 0, why);
    postSpeech(why);
    (void)base;
}

bool twBeginStep(uintptr_t base, uint32_t sym, uint32_t scan) {             // exported (the router)
    uintptr_t root = mapRoot(base);
    if (!root) return false;                      // not in a raid -> the key is not ours

    if (g_twStepping) {                             // a step is already running -> BRAKE
        twRelease("second press — braking");
        postSpeech(axs(AXS_MAP_STOPPED));
        return true;
    }
    if (!scan) {                                  // no scancode on the event: nothing to inject
        logLine("tilestep: event carried no scancode (sym=0x%x) - passing the key through", sym);
        return false;
    }
    int tile = -1;
    if (!sehPartyTile(base, root, &tile)) {
        logLine("tilestep: party tile read FAULTED - refusing the step");
        postSpeech(axs(AXS_MAP_NO_PARTY_POS));
        return true;
    }

    // ---- Pre-step gates. Both ----
    bool trapApproach = false;
    {
        uintptr_t area = 0;
        uintptr_t tb   = 0;
        long tiles = 0;
        if (safeReadPtr(root + MAP_CUR_AREA_PTR, &area) && area > 0x10000) {
            tiles = mapAreaTiles(area);
            safeReadPtr(area + AREA_TILES_BEG, &tb);
        }
        int next = tile + (sym == SDLK_d ? 1 : -1);   // 'd' is always the higher index (the game
                                                      // reverses the array to the travel direction)
        if (tb && tiles > 0 && (next < 0 || next >= (int)tiles)) {
            int32_t akind = -1; safeReadU32(area + AREA_KIND_OFF, (uint32_t*)&akind);
            uintptr_t t = tb + (uintptr_t)tile * TILE_STRIDE;
            int32_t ty = -1; uint32_t nb = 0;
            safeReadU32(t + TILE_TYPE_OFF,     (uint32_t*)&ty);
            safeReadU32(t + TILE_NEIGH_ID_OFF, &nb);
            if (akind == 1 && ty == 2 && nb && nb != FOURCC_NONE) {
                char nl[64]; mapAreaLabelById(root, nb, nl, sizeof nl);
                char msg[160];
                _snprintf(msg, sizeof msg, axs(AXS_MAP_DOOR_AT_END_FMT), nl);
                msg[sizeof msg - 1] = 0;
                logLine("tilestep: refused '%c' past the door - tile %d of %ld is the %s end",
                        (char)sym, tile + 1, tiles, next < 0 ? "low" : "high");
                postSpeech(msg);
                return true;
            }
        }
        if (tb && tile >= 0 && tile < (int)tiles) {
            int32_t curContent = -1;
            safeReadU32(tb + (uintptr_t)tile * TILE_STRIDE + TILE_CONTENT_OFF, (uint32_t*)&curContent);
            uint32_t aid = twAreaId(root);
            if (curContent == AREA_CONTENT_TRAP && tileContentsVisible(area, tile) &&
                g_twTrapTileArea == aid && g_twTrapTileIdx == tile && g_twTrapTileSym == sym) {
                bool acked = g_twTrapAckSym == sym && g_twTrapAckTile == tile &&
                             g_twTrapAckArea == aid &&
                             (long)(GetTickCount() - g_twTrapAckUntil) < 0;
                if (!acked) {
                    g_twTrapAckArea = aid; g_twTrapAckTile = tile; g_twTrapAckSym = sym;
                    g_twTrapAckUntil = GetTickCount() + TW_TRAP_ACK_MS;
                    logLine("tilestep: refused '%c' - crossing the revealed trap on tile %d of %ld (warned)",
                            (char)sym, tile + 1, tiles);
                    postSpeech(axs(AXS_MAP_TRAP_AHEAD));
                    return true;
                }
                logLine("tilestep: trap warning overridden - walking across tile %d of %ld",
                        tile + 1, tiles);
            }
        }
        if (tb && next >= 0 && next < (int)tiles) {
            int32_t content = -1;
            safeReadU32(tb + (uintptr_t)next * TILE_STRIDE + TILE_CONTENT_OFF, (uint32_t*)&content);
            if (content == AREA_CONTENT_TRAP && tileContentsVisible(area, next)) {
                uint32_t aid = twAreaId(root);
                if (g_twTrapTileArea == aid && g_twTrapTileIdx == tile && g_twTrapTileSym == sym) {
                    bool acked = g_twTrapAckSym == sym && g_twTrapAckTile == tile &&
                                 g_twTrapAckArea == aid &&
                                 (long)(GetTickCount() - g_twTrapAckUntil) < 0;
                    if (!acked) {
                        g_twTrapAckArea = aid; g_twTrapAckTile = tile; g_twTrapAckSym = sym;
                        g_twTrapAckUntil = GetTickCount() + TW_TRAP_ACK_MS;
                        logLine("tilestep: refused '%c' - at the threshold of the trap on tile %d of %ld (warned)",
                                (char)sym, next + 1, tiles);
                        postSpeech(axs(AXS_MAP_TRAP_AHEAD));
                        return true;
                    }
                    logLine("tilestep: trap warning overridden - crossing onto tile %d of %ld",
                            next + 1, tiles);
                } else {
                    trapApproach = true;
                    logLine("tilestep: '%c' approaches the revealed trap on tile %d of %ld - will "
                            "park short of the boundary onto it", (char)sym, next + 1, tiles);
                }
            }
        }
        g_twTrapAckArea = 0; g_twTrapAckTile = -1; g_twTrapAckSym = 0; g_twTrapAckUntil = 0;
    }

    // ---- AIM THE STEP (revision 6) ----
    uint32_t aid  = twAreaId(root);
    int      dir  = (sym == SDLK_d) ? 1 : -1;    // 'd' is always the higher tile index
    int      dest = tile + dir;
    long     areaTiles = 0;
    {
        uintptr_t a = 0;
        if (safeReadPtr(root + MAP_CUR_AREA_PTR, &a) && a > 0x10000) areaTiles = mapAreaTiles(a);
    }

    float pos = 0, kOff = 0, kScl = 0, tileW = 0, target = 0;
    bool  havePos = twPosModel(base, root, &pos, &kOff, &kScl);
    bool  aimed   = false;
    if (havePos) {
        tileW = 1.0f / kScl; if (tileW < 0) tileW = -tileW;
        if (trapApproach) {
            float bnd = twPosAt(kOff, kScl, dir > 0 ? tile + 1 : tile, 0.0f);
            target = bnd - (float)dir * TW_TRAP_GAP * tileW;
            aimed  = true;
        } else if (dest >= 0 && (areaTiles <= 0 || dest < (int)areaTiles)) {
            target = twPosAt(kOff, kScl, dest, TW_TILE_AIM);
            aimed  = true;
        }
    }

    // ---- THE ROOM-PROP AIM ----
    uintptr_t propAim = 0;
    if (!aimed && !trapApproach && havePos) {
        uintptr_t a = 0; int32_t akind = -1;
        if (safeReadPtr(root + MAP_CUR_AREA_PTR, &a) && a > 0x10000)
            safeReadU32(a + AREA_KIND_OFF, (uint32_t*)&akind);
        if (akind == 0) {
            uintptr_t props[RV_MAX_PROPS];
            int n = rvRoomProps(base, props, RV_MAX_PROPS);
            float bestDx = 0; int seen = 0, thatWay = 0;
            for (int i = 0; i < n; i++) {
                if (rvPropIsTrap(props[i]) || !rvPropActive(props[i])) continue;
                bool in = false; int pdir = 0; float dx = 0;
                if (!rvPropReach(base, props[i], &in, &pdir, &dx)) continue;
                seen++;
                if (in || pdir != dir) continue;
                thatWay++;
                if (!propAim || dx < bestDx) { propAim = props[i]; bestDx = dx; }
            }
            char nm[160] = {0};
            if (propAim) rvPropName(base, propAim, nm, sizeof nm);
            logLine("tilestep: room step '%c' - %d prop(s) readable, %d out of reach that way%s%s%s",
                    (char)sym, seen, thatWay, propAim ? " -> aiming at \"" : "", nm,
                    propAim ? "\"" : "");
        }
    }

    if (trapApproach && !aimed) {
        g_twTrapAckArea  = aid; g_twTrapAckTile = tile; g_twTrapAckSym  = sym;
        g_twTrapAckUntil = GetTickCount() + TW_TRAP_ACK_MS;
        g_twTrapTileArea = aid; g_twTrapTileIdx = tile; g_twTrapTileSym = sym;
        logLine("tilestep: refused '%c' - the position model would not read, so this trap approach "
                "has no provable stop (press again to cross anyway)", (char)sym);
        postSpeech(axs(AXS_MAP_TRAP_AHEAD));
        return true;
    }

    if (aimed && (float)dir * (pos - target) >= 0.0f) {
        logLine("tilestep: '%c' is already at or past its aim point (pos=%.4f target=%.4f) - "
                "nothing to walk", (char)sym, pos, target);
        if (trapApproach) {
            g_twTrapTileArea = aid; g_twTrapTileIdx = tile; g_twTrapTileSym = sym;
            postSpeech(axs(AXS_MAP_TRAP_AHEAD));
        } else {
            twAnnounceArrival(base, root, tile, false);
        }
        return true;
    }

    g_twBase         = base;
    g_twSym          = sym; g_twScan = scan;
    g_twStartTile    = tile;
    g_twStartArea    = aid;
    g_twDir          = dir;
    g_twDestTile     = aimed ? dest : -1;
    g_twAimed        = aimed;
    g_twTarget       = target;
    g_twPropAim      = propAim;
    g_twTileW        = tileW;
    g_twStartPos     = pos;
    g_twLastPos      = pos;
    g_twSpeed        = 0;
    g_twStartTick    = GetTickCount();
    g_twMoveTick     = g_twStartTick;
    g_twHardDeadline = g_twStartTick + TW_HARD_MS;
    g_twStepping     = true; g_twVerified = false;
    g_twTrapShort    = trapApproach;
    enqueueSynthKey(SDL_EVT_KEYDOWN, scan, sym, 0);
    if (aimed)
        logLine("tilestep: holding '%c' (scan=%u) area=%08x tile=%d pos=%.4f -> aim %.4f "
                "(tileW=%.4f, dest tile %d)%s", (char)sym, scan, aid, tile, pos, target, tileW,
                dest, trapApproach ? " (trap approach)" : "");
    else if (propAim)
        logLine("tilestep: holding '%c' (scan=%u) area=%08x pos=%.4f -> until prop %p is in reach",
                (char)sym, scan, aid, pos, (void*)propAim);
    else
        logLine("tilestep: holding '%c' (scan=%u) area=%08x tile=%d - NO aim point (destination "
                "tile %d is outside this area's %ld); stopping on the tile or area index instead",
                (char)sym, scan, aid, tile, dest, areaTiles);
    return true;
}

void serviceTileStep(uintptr_t base) {                                      // exported (OurPoll)
    if (g_twRelSym && (long)(GetTickCount() - g_twRelDeadline) >= 0) {
        bool down = false;
        if (sehKeyIsDown(base, g_twRelSym, &down) && down) {
            safeWriteU32(base + KEY_STATE_ARRAY_RVA + (uintptr_t)g_twRelScan * 4, 3);
            logLine("tilestep: WARNING the KEYUP did not take for scan=%u - forced the release",
                    g_twRelScan);
        }
        if (g_twMeasure) {
            uintptr_t mroot = mapRoot(base);
            float mpos = 0, mkOff = 0, mkScl = 0;
            if (mroot && twAreaId(mroot) == g_twRelArea &&
                twPosModel(base, mroot, &mpos, &mkOff, &mkScl)) {
                float coast = mpos - g_twRelPos; if (coast < 0) coast = -coast;
                float w = 1.0f / mkScl; if (w < 0) w = -w;
                if (coast <= 0.5f * w) {
                    g_twCoast = g_twCoastValid ? (g_twCoast * 0.5f + coast * 0.5f) : coast;
                    g_twCoastValid = true;
                    logLine("tilestep: coast after release = %.4f (%.1f%% of a tile), running %.4f",
                            coast, 100.0f * coast / w, g_twCoast);
                }
            }
            g_twMeasure = false;
        }
        g_twRelSym = 0; g_twRelScan = 0;
    }

    if (!g_twStepping) return;

    uintptr_t root = mapRoot(base);
    if (!root) { twRelease("the raid ended"); return; }   // silent: the screen is gone

    if (!g_twVerified) {
        bool down = false;
        if (sehKeyIsDown(base, g_twSym, &down) && down) {
            g_twVerified = true;
            logLine("tilestep: the game reports '%c' held - the walk is running", (char)g_twSym);
        }
    }

    int tile = -1;
    if (!sehPartyTile(base, root, &tile)) return;
    uint32_t area = twAreaId(root);
    uint32_t now  = GetTickCount();

    if (area != g_twStartArea) {
        twRelease("crossed into a new area");
        g_twTrapTileArea = 0; g_twTrapTileIdx = -1; g_twTrapTileSym = 0;
        twAnnounceArrival(base, root, tile, true);
        return;
    }

    float pos = 0, kOff = 0, kScl = 0;
    bool  havePos = twPosModel(base, root, &pos, &kOff, &kScl);
    if (havePos) {
        if (pos != g_twLastPos) { g_twLastPos = pos; g_twMoveTick = now; }
        uint32_t elapsed = now - g_twStartTick;
        if (elapsed >= 48) g_twSpeed = (pos - g_twStartPos) / (float)elapsed;
    }

    if (g_twPropAim) {
        bool in = false; int pdir = 0; float dx = 0;
        if (rvPropReach(base, g_twPropAim, &in, &pdir, &dx) && in) {
            uintptr_t prop = g_twPropAim;
            char nm[160] = {0};
            if (!rvPropName(base, prop, nm, sizeof nm))
                _snprintf(nm, sizeof nm, "%s", axs(AXS_RV_LABEL_OBJECT));
            nm[sizeof nm - 1] = 0;
            logLine("tilestep: stop - prop %p (\"%s\") is in reach at pos=%.4f", (void*)prop, nm, pos);
            twRelease("the object is in reach");
            g_twTrapTileArea = 0; g_twTrapTileIdx = -1; g_twTrapTileSym = 0;
            char msg[220];
            _snprintf(msg, sizeof msg, axs(AXS_MAP_PROP_IN_REACH_FMT), nm);
            msg[sizeof msg - 1] = 0;
            postSpeech(msg);
            return;
        }
    }

    if (havePos && g_twAimed) {
        float stopDist = g_twCoastValid ? g_twCoast * 1.25f : 0.0f;
        float speedAbs = g_twSpeed < 0 ? -g_twSpeed : g_twSpeed;
        float bySpeed  = speedAbs * (float)TW_STOP_SLACK_MS;
        if (bySpeed > stopDist) stopDist = bySpeed;
        if (g_twTileW > 0 && stopDist > 0.35f * g_twTileW) stopDist = 0.35f * g_twTileW;
        if (!g_twTrapShort && g_twTileW > 0 && stopDist > TW_TILE_AIM * g_twTileW)
            stopDist = TW_TILE_AIM * g_twTileW;
        float predicted = pos + (float)g_twDir * stopDist;
        if ((float)g_twDir * (predicted - g_twTarget) >= 0.0f) {
            bool     wasTrap = g_twTrapShort;
            uint32_t stepSym = g_twSym;
            logLine("tilestep: stop pos=%.4f predicted=%.4f target=%.4f (stopDist=%.4f "
                    "speed=%.6f/ms tileW=%.4f) tile=%d%s",
                    pos, predicted, g_twTarget, stopDist, g_twSpeed, g_twTileW, tile,
                    wasTrap ? " [trap approach]" : "");
            twRelease(wasTrap ? "parked short of the trap boundary" : "reached the aim point");
            if (wasTrap) {
                g_twTrapTileArea = area; g_twTrapTileIdx = tile; g_twTrapTileSym = stepSym;
                postSpeech(axs(AXS_MAP_TRAP_AHEAD));
            } else {
                g_twTrapTileArea = 0; g_twTrapTileIdx = -1; g_twTrapTileSym = 0;
                twAnnounceArrival(base, root, tile, false);
            }
            return;
        }
    }

    if (tile != g_twStartTile) {
        bool overshotTrap = g_twTrapShort;
        bool stopHere = overshotTrap || !g_twAimed
                        || (g_twDir > 0 ? tile > g_twDestTile : tile < g_twDestTile);
        if (stopHere) {
            uint32_t stepSym = g_twSym;
            if (overshotTrap)
                logLine("trapstop: OVERSHOT - the index flipped onto the trap tile before the aim "
                        "point was reached (posRead=%d aimed=%d target=%.4f last=%.4f)",
                        havePos ? 1 : 0, g_twAimed ? 1 : 0, g_twTarget, g_twLastPos);
            twRelease(overshotTrap ? "overshot onto the trap tile" : "reached the next tile");
            if (overshotTrap) {
                g_twTrapTileArea = area; g_twTrapTileIdx = tile; g_twTrapTileSym = stepSym;
            } else {
                g_twTrapTileArea = 0; g_twTrapTileIdx = -1; g_twTrapTileSym = 0;
            }
            twAnnounceArrival(base, root, tile, false);
            return;
        }
    }

    if ((long)(now - g_twMoveTick) >= (long)TW_STUCK_MS ||
        (long)(now - g_twHardDeadline) >= 0) {
        bool     verified = g_twVerified;
        bool     moved    = (tile != g_twStartTile) || (havePos && pos != g_twStartPos);
        bool     wasTrap  = g_twTrapShort;
        uint32_t stepSym  = g_twSym;
        logLine("tilestep: no further movement for %ums (moved=%d aimed=%d pos=%.4f target=%.4f "
                "tile=%d start=%d)", now - g_twMoveTick, moved ? 1 : 0, g_twAimed ? 1 : 0,
                g_twLastPos, g_twTarget, tile, g_twStartTile);
        twRelease(moved ? "the walk stopped before the aim point" : "the party never moved");
        if (moved && wasTrap) {
            g_twTrapTileArea = area; g_twTrapTileIdx = tile; g_twTrapTileSym = stepSym;
            postSpeech(axs(AXS_MAP_TRAP_AHEAD));
        } else if (moved) {
            g_twTrapTileArea = 0; g_twTrapTileIdx = -1; g_twTrapTileSym = 0;
            twAnnounceArrival(base, root, tile, false);
        } else {
            twAnnounceStuck(base, root, verified);
        }
    }
}

// ---- SECRET ROOM transitions are deliberately NOT announced ----
// ---- ...AND THE NARROW HALF OF IT IS BACK, 2026-08-23 ----
// ---- ...AND WIDENED TO EVERY ROOM, 2026-09-16 (dev's design) ----
static uint32_t g_scAreaId = 0;          // the party's area id as of the last frame

void serviceAreaCross(uintptr_t base) {                                     // exported (OurPoll)
    uintptr_t root = mapRoot(base);
    if (!root) { g_scAreaId = 0; return; }          // out of a raid: arm for the next one
    uint32_t now = twAreaId(root);
    if (!now || now == g_scAreaId) return;
    uint32_t was = g_scAreaId;
    g_scAreaId = now;                               // ALWAYS update, even when we stay silent —
                                                    // a skipped edge must not fire on the next one
    if (!was) return;                               // first sighting of the raid: no crossing yet
    if (g_movePending || g_twStepping) return;      // those own their own arrival line
    uintptr_t area = 0;
    if (!safeReadPtr(root + MAP_CUR_AREA_PTR, &area) || area <= 0x10000) return;
    if (!areaIsRoom(area)) return;                  // a hallway entered by hand: the game's silence
    char line[400]; twRoomLine(base, root, area, line, sizeof line);
    char msg[MAILBOX_SZ];
    _snprintf(msg, sizeof msg, "%s.", line); msg[sizeof msg - 1] = 0;
    logLine("area cross: '%.4s' -> '%.4s' - room arrival -> \"%s\"", &was, &now, msg);
    postSpeech(msg);
}

// ---- A SECRET ROOM WAS FOUND: the reveal edge, and WHERE ----
static const int SR_MAX_DOORS = 8;
struct SecretDoorWatch {
    uint32_t areaId;
    long     areaIdx;
    int      tileIdx;
    bool     shown;       // last seen answer of tileHiddenDoorShown
};
static SecretDoorWatch g_srDoors[SR_MAX_DOORS];
static int       g_srCount    = 0;
static uintptr_t g_srSeedBeg  = 0;       // the area vector this seeding was taken from...
static long      g_srSeedN    = 0;

static bool tileIsHiddenDoorSlot(uintptr_t tile) {
    uint32_t nb = 0; int32_t hk = -1, content = -1;
    if (!safeReadU32(tile + TILE_NEIGH_ID_OFF, &nb) || !nb || nb == FOURCC_NONE) return false;
    if (!safeReadU32(tile + TILE_HDOOR_KIND_OFF, (uint32_t*)&hk) || hk != TILE_HDOOR_KIND_HIDDEN)
        return false;
    if (!safeReadU32(tile + TILE_CONTENT_OFF, (uint32_t*)&content) ||
        content != AREA_CONTENT_HIDDEN_DOOR) return false;
    return true;
}

static void srSeed(uintptr_t root, uintptr_t beg, long n) {
    g_srCount = 0; g_srSeedBeg = beg; g_srSeedN = n;
    for (long i = 0; i < n && g_srCount < SR_MAX_DOORS; i++) {
        uintptr_t area = mapAreaAt(root, i); if (!area) continue;
        int32_t kind = -1;
        safeReadU32(area + AREA_KIND_OFF, (uint32_t*)&kind);
        if (kind != 1) continue;                    // only a hallway ever carries one
        uintptr_t tb = 0;
        if (!safeReadPtr(area + AREA_TILES_BEG, &tb) || !tb) continue;
        long tiles = mapAreaTiles(area);
        for (long t = 0; t < tiles && g_srCount < SR_MAX_DOORS; t++) {
            if (!tileIsHiddenDoorSlot(tb + (uintptr_t)t * TILE_STRIDE)) continue;
            SecretDoorWatch& d = g_srDoors[g_srCount++];
            d.areaIdx = i; d.tileIdx = (int)t; d.areaId = 0;
            safeReadU32(area + AREA_ID_OFF, &d.areaId);
            d.shown = tileHiddenDoorShown(area, (int)t, nullptr);
            logLine("secret watch: door slot at tile %ld of '%.4s' (shown=%d)",
                    t, &d.areaId, d.shown ? 1 : 0);
        }
    }
    logLine("secret watch: seeded on a new map (%ld areas) - %d hidden door(s)", n, g_srCount);
}

void serviceSecretReveal(uintptr_t base) {                                  // exported (OurPoll)
    uintptr_t root = mapRoot(base);
    if (!root) { g_srCount = 0; g_srSeedBeg = 0; g_srSeedN = 0; return; }

    uintptr_t beg = 0, end = 0;
    if (!safeReadPtr(root + MAP_AREAVEC_BEG, &beg) || !beg) return;
    if (!safeReadPtr(root + MAP_AREAVEC_END, &end) || end <= beg) return;
    long n = (long)((end - beg) / 8);
    if (n < 0 || n > 100000) return;
    if (beg != g_srSeedBeg || n != g_srSeedN) srSeed(root, beg, n);
    if (!g_srCount) return;                         // this map has no secret room: nothing to poll

    for (int i = 0; i < g_srCount; i++) {
        SecretDoorWatch& d = g_srDoors[i];
        if (d.shown) continue;
        uintptr_t area = mapAreaAt(root, d.areaIdx); if (!area) continue;
        uint32_t id = 0;
        if (!safeReadU32(area + AREA_ID_OFF, &id) || id != d.areaId) continue;  // vector drifted
        uintptr_t tb = 0;
        if (!safeReadPtr(area + AREA_TILES_BEG, &tb) || !tb) continue;
        if (!tileIsHiddenDoorSlot(tb + (uintptr_t)d.tileIdx * TILE_STRIDE)) {
            long tiles = mapAreaTiles(area);
            long found = -1;
            for (long t = 0; t < tiles; t++)
                if (tileIsHiddenDoorSlot(tb + (uintptr_t)t * TILE_STRIDE)) { found = t; break; }
            if (found < 0) continue;
            logLine("secret watch: door slot moved tile %d -> %ld in '%.4s' - the seed saw a mid-load map",
                    d.tileIdx, found, &d.areaId);
            d.tileIdx = (int)found;
        }
        bool shown = tileHiddenDoorShown(area, d.tileIdx, nullptr);
        if (!shown) continue;
        d.shown = true;                             // latch: this door speaks exactly once per map
        char hall[96]; mapAreaLabelEx(root, area, hall, sizeof hall);
        uint32_t t = GetTickCount();
        bool saidAlready = g_scoutSaidAt && (uint32_t)(t - g_scoutSaidAt) < SCOUT_SAY_GAP_MS;
        char msg[MAILBOX_SZ];
        if (saidAlready) {
            _snprintf(msg, sizeof msg, axs(AXS_SCOUT_SECRET_AT_FMT),
                      axs(AXS_MAP_SECRET_ROOM), hall);
        } else {
            char word[96];
            if (!resolveKey(base, SCOUT_WORD_KEY, word, sizeof word) || !word[0]) {
                strncpy(word, axs(AXS_MAP_SCOUTING), sizeof word - 1);
                word[sizeof word - 1] = 0;
            }
            _snprintf(msg, sizeof msg, axs(AXS_SCOUT_SECRET_FOUND_FMT),
                      word, axs(AXS_MAP_SECRET_ROOM), hall);
        }
        msg[sizeof msg - 1] = 0;
        g_scoutSaidAt = t;                          // swallow the plain "Scouting" still to come
        logLine("secret watch: revealed on tile %d of '%.4s' -> \"%s\"", d.tileIdx, &id, msg);
        postSpeech(msg, false);
    }
}

// ---- THE VISITED OBSERVER: which tiles has the MOD seen the party on ----
static uintptr_t g_vwSeedBeg = 0;      // area vector begin the set was built against...
static long      g_vwSeedN   = 0;
static uintptr_t g_vwLastArea = 0;
static uint32_t  g_vwLastX = 0, g_vwLastY = 0;
static bool      g_vwOverflowSaid = false;

static void vwMark(uintptr_t area, int tileIdx) {
    if (!area || tileIdx < 0) return;
    if (tileIdx >= 32) {
        if (!g_vwOverflowSaid) { g_vwOverflowSaid = true;
            logLine("visited watch: tile %d >= 32 -- not recording (conservative)", tileIdx); }
        return;
    }
    for (int i = 0; i < g_vwObsCount; i++)
        if (g_vwObs[i].area == area) { g_vwObs[i].bits |= 1u << tileIdx; return; }
    if (g_vwObsCount >= VW_MAX_AREAS) {
        if (!g_vwOverflowSaid) { g_vwOverflowSaid = true;
            logLine("visited watch: observed-set full (%d areas) -- not recording (conservative)",
                    VW_MAX_AREAS); }
        return;
    }
    g_vwObs[g_vwObsCount].area = area;
    g_vwObs[g_vwObsCount].bits = 1u << tileIdx;
    g_vwObsCount++;
}

void serviceVisitedWatch(uintptr_t base) {                                  // exported (OurPoll)
    uintptr_t root = mapRoot(base);
    if (!root) {                                    // out of a raid: empty set, armed for the next
        g_vwObsCount = 0; g_vwSeedBeg = 0; g_vwSeedN = 0;
        g_vwLastArea = 0; g_vwOverflowSaid = false;
        return;
    }
    uintptr_t beg = 0, end = 0;
    if (!safeReadPtr(root + MAP_AREAVEC_BEG, &beg) || !beg) return;
    if (!safeReadPtr(root + MAP_AREAVEC_END, &end) || end <= beg) return;
    long n = (long)((end - beg) / 8);
    if (n < 0 || n > 100000) return;
    if (beg != g_vwSeedBeg || n != g_vwSeedN) {     // a new map: forget the old one's observations
        g_vwObsCount = 0; g_vwSeedBeg = beg; g_vwSeedN = n;
        g_vwLastArea = 0; g_vwOverflowSaid = false;
        logLine("visited watch: reset on a new map (%ld areas)", n);
    }
    uintptr_t live = 0;
    if (!safeReadPtr(root + MAP_CUR_AREA_PTR, &live) || live <= 0x10000) return;
    uint32_t pxr = 0, pyr = 0;
    if (!safeReadU32(root + MAP_PARTY_X_OFF, &pxr)) return;
    if (!safeReadU32(root + MAP_PARTY_Y_OFF, &pyr)) return;
    if (live == g_vwLastArea && pxr == g_vwLastX && pyr == g_vwLastY) return;   // nothing moved
    g_vwLastArea = live; g_vwLastX = pxr; g_vwLastY = pyr;
    vwMark(live, areaNearestTileP(live, u32AsFloatM(pxr), u32AsFloatM(pyr)));
}
