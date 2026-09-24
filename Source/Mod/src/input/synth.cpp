// input/synth.cpp -- the SYNTHESISED INPUT layer, and the first module of input/.

#include <cstdint>
#include <cstring>
#include <cstdio>
#include "internal.h"

// ---- Front-end activation via synthesised mouse input ----

bool elemPos(uintptr_t elem, float* x, float* y) {
    uint32_t xb = 0, yb = 0;
    if (!safeReadU32(elem + ELEM_POS_OFF, &xb))     return false;
    if (!safeReadU32(elem + ELEM_POS_OFF + 4, &yb)) return false;
    *x = u32AsFloatM(xb); *y = u32AsFloatM(yb);
    return true;
}

// ---- Synthesised mouse input (front-end virtual-cursor model) ----
struct SynthEvent { uint32_t type; uint8_t button, state, held; int32_t x, y; uint32_t frame;
                    DWORD dueTick; };
static SynthEvent g_synth[16];
static int        g_synthCount = 0;
static uint32_t   g_synthFrame = 0;                 // advances at each pump end
static float      g_cursorFx = 0.0f, g_cursorFy = 0.0f;  // current virtual cursor position
bool              g_cursorValid = false;            // set once we've placed it on this screen

static void writeSyntheticMouse(void* ev, uint32_t type, int button, int state, uint8_t held,
                                int32_t x, int32_t y) {
    uintptr_t e = (uintptr_t)ev;
    for (int i = 0; i < SDL_EVENT_SIZE; i += 8) safeWriteU64(e + i, 0);  // clear the SDL_Event union
    safeWriteU32(e + 0x00, type);
    if (button) {
        safeWriteU8(e + SDL_MOUSE_BUTTON_OFF, (uint8_t)button);
        safeWriteU8(e + SDL_MOUSE_STATE_OFF,  (uint8_t)state);
        safeWriteU8(e + SDL_MOUSE_CLICKS_OFF, 1);
    } else if (held) {
        safeWriteU32(e + SDL_MOUSE_BUTTON_OFF, held);
    }
    safeWriteU32(e + SDL_MOUSE_X_OFF, (uint32_t)x);
    safeWriteU32(e + SDL_MOUSE_Y_OFF, (uint32_t)y);
}

// ---- Design-space -> window-space conversion ----
void designToWindow(float dx, float dy, float* wx, float* wy) {
    *wx = dx; *wy = dy;                              // identity fallback: the always-worked case
    uint32_t nb = 0, dxb = 0, dyb = 0, oxb = 0, oyb = 0;
    if (!safeReadU32(g_base + VP_NUM_RVA,   &nb)  ||
        !safeReadU32(g_base + VP_DEN_X_RVA, &dxb) ||
        !safeReadU32(g_base + VP_DEN_Y_RVA, &dyb) ||
        !safeReadU32(g_base + VP_OFF_X_RVA, &oxb) ||
        !safeReadU32(g_base + VP_OFF_Y_RVA, &oyb)) return;
    float num  = u32AsFloatM(nb);
    float denx = u32AsFloatM(dxb), deny = u32AsFloatM(dyb);
    if (!(num > 0.0f) || !(denx > 0.0f) || !(deny > 0.0f) ||
        num > 32768.0f || denx > 32768.0f || deny > 32768.0f) return;
    *wx = dx * (denx / num) + u32AsFloatM(oxb);
    *wy = dy * (deny / num) + u32AsFloatM(oyb);
}

void writeCursorGlobals(float designX, float designY) {
    float wx = 0, wy = 0;
    designToWindow(designX, designY, &wx, &wy);
    safeWriteU32(g_base + FE_CURSOR_X_RVA, *reinterpret_cast<uint32_t*>(&wx));
    safeWriteU32(g_base + FE_CURSOR_Y_RVA, *reinterpret_cast<uint32_t*>(&wy));
}

void enqueueSynth(uint32_t type, int button, int state, uint32_t frameDelay) {
    if (g_synthCount >= (int)(sizeof g_synth / sizeof g_synth[0])) {
        logLine("synth: QUEUE FULL (%d) -- DROPPED a type=0x%x event", g_synthCount, type);
        return;                          // never silently: a lost release hangs a drag
    }
    SynthEvent& s = g_synth[g_synthCount++];
    s.type = type; s.button = (uint8_t)button; s.state = (uint8_t)state; s.held = 0;
    s.x = (int32_t)g_cursorFx; s.y = (int32_t)g_cursorFy; s.frame = g_synthFrame + frameDelay;
    s.dueTick = 0;
}

static const DWORD SYNTH_DRAG_STAGE_MS = 40;

void enqueueSynthWheel(int clicks) {
    if (g_synthCount >= (int)(sizeof g_synth / sizeof g_synth[0])) {
        logLine("synth: QUEUE FULL (%d) -- DROPPED a wheel event (%d)", g_synthCount, clicks);
        return;
    }
    SynthEvent& s = g_synth[g_synthCount++];
    s.type = SDL_EVT_MOUSEWHEEL; s.button = 0; s.state = 0; s.held = 0;
    s.x = 0; s.y = clicks; s.frame = g_synthFrame;   // due immediately
    s.dueTick = 0;
}

void enqueueSynthAt(uint32_t type, int button, int state, uint8_t held,
                           float x, float y, uint32_t frameDelay) {
    if (g_synthCount >= (int)(sizeof g_synth / sizeof g_synth[0])) {
        logLine("synth: QUEUE FULL (%d) -- DROPPED drag stage %u (type=0x%x)",
                g_synthCount, frameDelay, type);
        return;                          // never silently: a lost release hangs a drag
    }
    SynthEvent& s = g_synth[g_synthCount++];
    s.type = type; s.button = (uint8_t)button; s.state = (uint8_t)state; s.held = held;
    s.x = (int32_t)x; s.y = (int32_t)y; s.frame = g_synthFrame + frameDelay;
    s.dueTick = GetTickCount() + frameDelay * SYNTH_DRAG_STAGE_MS;
}

bool emitSynth(void* ev) {
    if (g_synthCount == 0 || g_synth[0].frame > g_synthFrame) return false;
    if (g_synth[0].dueTick && (int32_t)(GetTickCount() - g_synth[0].dueTick) < 0) return false;
    SynthEvent s = g_synth[0];
    for (int i = 1; i < g_synthCount; i++) g_synth[i - 1] = g_synth[i];   // pop front
    g_synthCount--;
    if (s.type == SDL_EVT_MOUSEWHEEL) {
        uintptr_t e = (uintptr_t)ev;
        for (int i = 0; i < SDL_EVENT_SIZE; i += 8) safeWriteU64(e + i, 0);
        safeWriteU32(e + 0x00, s.type);
        safeWriteU32(e + SDL_WHEEL_X_OFF, (uint32_t)s.x);
        safeWriteU32(e + SDL_WHEEL_Y_OFF, (uint32_t)s.y);
        uint8_t inWin = 1;
        if (safeReadU8(g_base + MOUSE_INWINDOW_RVA, &inWin) && inWin == 0) {
            logLine("synth emit: d9607d was 0 (mouse-outside-window) — forcing 1");
            safeWriteU8(g_base + MOUSE_INWINDOW_RVA, 1);
        }
        logLine("synth emit type=0x%x wheel=(%d,%d)", s.type, (int)s.x, (int)s.y);
        return true;
    }
    g_cursorFx = (float)s.x; g_cursorFy = (float)s.y;
    writeCursorGlobals(g_cursorFx, g_cursorFy);
    float wx = 0, wy = 0;
    designToWindow((float)s.x, (float)s.y, &wx, &wy);
    writeSyntheticMouse(ev, s.type, s.button, s.state, s.held,
                        (int32_t)(wx + 0.5f), (int32_t)(wy + 0.5f));
    uint8_t inWin = 1;
    if (safeReadU8(g_base + MOUSE_INWINDOW_RVA, &inWin) && inWin == 0) {
        logLine("synth emit: d9607d was 0 (mouse-outside-window) — forcing 1");
        safeWriteU8(g_base + MOUSE_INWINDOW_RVA, 1);
    }
    uint8_t sup = 0;
    if (safeReadU8(g_base + MOUSE_SUPPRESS_RVA, &sup) && sup != 0) {
        logLine("synth emit: d9607a was %d (mouse suppressed) — forcing 0", (int)sup);
        safeWriteU8(g_base + MOUSE_SUPPRESS_RVA, 0);
    }
    logLine("synth emit type=0x%x state=%d design=(%d,%d) win=(%.0f,%.0f)",
            s.type, (int)s.state, (int)s.x, (int)s.y, wx, wy);
    return true;
}
void advanceSynthFrame() { g_synthFrame++; }
bool clickQueued() {                          // a press is already pending -> don't re-arm
    for (int i = 0; i < g_synthCount; i++)
        if (g_synth[i].type == SDL_EVT_MOUSEBUTTONUP) return true;
    return false;
}

// Centre of an element's on-screen rect (pos +0x10, size +0x18).
bool elemCenter(uintptr_t elem, float* fx, float* fy) {
    float x, y; if (!elemPos(elem, &x, &y)) return false;
    uint32_t wb = 0, hb = 0;
    safeReadU32(elem + ELEM_SIZE_OFF,     &wb);
    safeReadU32(elem + ELEM_SIZE_OFF + 4, &hb);
    *fx = x + u32AsFloatM(wb) * 0.5f;
    *fy = y + u32AsFloatM(hb) * 0.5f;
    return true;
}

// ---- THE DRAG GESTURE, in one place ----
bool synthDragPoints(float sx, float sy, float tx, float ty, const char* what) {
    if (clickQueued()) {                       // another gesture is already in flight
        logLine("synthdrag: %s REFUSED, a click is already queued", what ? what : "drag");
        return false;
    }
    moveCursorTo(sx, sy);                                            // hover the source (frame 0)
    enqueueSynthAt(SDL_EVT_MOUSEBUTTONDOWN, SDL_BUTTON_LEFT, 1, 0, sx, sy, 1);
    enqueueSynthAt(SDL_EVT_MOUSEMOTION, 0, 0, 1, sx, sy, 2);         // dwell: let the source latch
    enqueueSynthAt(SDL_EVT_MOUSEMOTION, 0, 0, 1, sx, sy, 3);         // dwell: frame B
    enqueueSynthAt(SDL_EVT_MOUSEMOTION, 0, 0, 1, sx, sy, 4);         // dwell: frame C, drag begins
    enqueueSynthAt(SDL_EVT_MOUSEMOTION, 0, 0, 1, (sx + tx) * 0.5f, (sy + ty) * 0.5f, 5);
    enqueueSynthAt(SDL_EVT_MOUSEMOTION, 0, 0, 1, tx, ty, 6);
    enqueueSynthAt(SDL_EVT_MOUSEMOTION, 0, 0, 1, tx, ty, 7);         // let the drag-over settle
    enqueueSynthAt(SDL_EVT_MOUSEBUTTONUP, SDL_BUTTON_LEFT, 0, 0, tx, ty, 8);
    logLine("synthdrag: %s (%.0f,%.0f) -> (%.0f,%.0f)", what ? what : "drag", sx, sy, tx, ty);
    return true;
}

bool synthDragElements(int64_t srcId, int64_t dstId, const char* what) {
    uintptr_t src = feGetElementById(srcId), dst = feGetElementById(dstId);
    float sx = 0, sy = 0, tx = 0, ty = 0;
    if (!src || !elemCenter(src, &sx, &sy)) {
        logLine("synthdrag: %s -- source element 0x%llx is not on screen",
                what ? what : "drag", (unsigned long long)srcId);
        return false;
    }
    if (!dst || !elemCenter(dst, &tx, &ty)) {
        logLine("synthdrag: %s -- target element 0x%llx is not on screen",
                what ? what : "drag", (unsigned long long)dstId);
        return false;
    }
    return synthDragPoints(sx, sy, tx, ty, what);
}

void moveCursorTo(float fx, float fy) {
    g_cursorFx = fx; g_cursorFy = fy; g_cursorValid = true;
    writeCursorGlobals(fx, fy);
    enqueueSynth(SDL_EVT_MOUSEMOTION, 0, 0, 0);
}

// ---- PARKING: get the cursor OUT OF THE WAY for a transition ----
static const float CURSOR_PARK_X = -1000.0f, CURSOR_PARK_Y = -1000.0f;   // design space; off-screen
void parkCursor(const char* why) {
    if (g_synthCount > 0) {
        logLine("cursor park REFUSED (%s): %d synthetic event(s) in flight", why ? why : "", g_synthCount);
        return;
    }
    g_cursorFx = CURSOR_PARK_X; g_cursorFy = CURSOR_PARK_Y; g_cursorValid = false;
    writeCursorGlobals(CURSOR_PARK_X, CURSOR_PARK_Y);
    enqueueSynth(SDL_EVT_MOUSEMOTION, 0, 0, 0);
    logLine("cursor parked (%s)", why ? why : "");
}

bool feElemOnScreen(int64_t id) {
    return focusElementById(g_base, id) != 0;
}

void frontEndClickCursor() {
    if (clickQueued()) {                                // a click is already in flight
        logLine("fe-click at cursor REFUSED: a click is already queued (n=%d)", g_synthCount);
        return;
    }
    if (!g_cursorValid) {
        int64_t gameId = 0; safeReadI64(g_base + FOCUS_ID_RVA, &gameId);
        int64_t id = !isNoFocus(gameId) ? gameId : g_feFocusId;
        uintptr_t elem = 0;
        if (!isNoFocus(id)) elem = focusElementById(g_base, id);
        float tx = 0, ty = 0;
        if (!elem || !elemCenter(elem, &tx, &ty)) {
            logLine("fe-click: no cursor and no focus to activate");
            return;
        }
        g_cursorFx = tx; g_cursorFy = ty; g_cursorValid = true;
    }
    writeCursorGlobals(g_cursorFx, g_cursorFy);
    enqueueSynth(SDL_EVT_MOUSEMOTION,     0,               0, 0);
    enqueueSynth(SDL_EVT_MOUSEBUTTONDOWN, SDL_BUTTON_LEFT, 1, 0);
    enqueueSynth(SDL_EVT_MOUSEBUTTONUP,   SDL_BUTTON_LEFT, 0, 1);
    logLine("fe-click at cursor (%.0f,%.0f)", g_cursorFx, g_cursorFy);
}

uintptr_t feGetElementById(int64_t id) {
    return focusElementById(g_base, id);
}

static const uint32_t FE_FAMILY_SPAN_DEFAULT = 0x1000;

uintptr_t feFindElementByFamily(uintptr_t base, uint32_t family, uint32_t ownerTag, int64_t* idOut,
                                uint32_t span) {
    if (span == 0) span = FE_FAMILY_SPAN_DEFAULT;
    uintptr_t begin = 0, end = 0;
    if (!safeReadPtr(base + VEC_BEGIN_RVA, &begin) ||
        !safeReadPtr(base + VEC_END_RVA, &end) || !begin || end <= begin) return 0;
    uintptr_t n = (end - begin) / ELEM_STRIDE;
    if (n > 4096) n = 4096;
    for (uintptr_t i = 0; i < n; i++) {
        uintptr_t el = begin + i * ELEM_STRIDE;
        int64_t id = 0; uintptr_t owner = 0;
        if (!safeReadI64(el + ELEM_ID_OFF, &id) || isNoFocus(id)) continue;
        safeReadPtr(el + ELEM_OWNER_OFF, &owner);
        uint32_t cc = (uint32_t)(uint64_t)id;
        uint32_t lo = family & 0xFFFFFF00u;
        if (!(cc >= lo && cc - lo < span) &&
            !(ownerTag && (uint32_t)owner == ownerTag)) continue;
        float cx = 0, cy = 0;
        if (!elemCenter(el, &cx, &cy)) continue;   // registered but no readable rect -- keep looking
        if (idOut) *idOut = id;
        return el;
    }
    return 0;
}

int feCollectFamilyByX(uintptr_t base, uint32_t family, uint32_t ownerTag,
                       uintptr_t* out, float* xOut, int maxOut) {
    uintptr_t begin = 0, end = 0;
    int n = 0;
    if (!out || maxOut <= 0) return 0;
    if (!safeReadPtr(base + VEC_BEGIN_RVA, &begin) ||
        !safeReadPtr(base + VEC_END_RVA, &end) || !begin || end <= begin) return 0;
    uintptr_t cnt = (end - begin) / ELEM_STRIDE;
    if (cnt > 4096) cnt = 4096;
    float xs[64];
    if (maxOut > 64) maxOut = 64;
    for (uintptr_t i = 0; i < cnt && n < maxOut; i++) {
        uintptr_t el = begin + i * ELEM_STRIDE;
        int64_t id = 0; uintptr_t owner = 0;
        if (!safeReadI64(el + ELEM_ID_OFF, &id) || isNoFocus(id)) continue;
        safeReadPtr(el + ELEM_OWNER_OFF, &owner);
        uint32_t cc = (uint32_t)(uint64_t)id;
        uint32_t lo = family & 0xFFFFFF00u;
        const bool byOwner = ownerTag && (uint32_t)owner == ownerTag;
        if (!byOwner && !(cc >= lo && cc - lo < FE_FAMILY_SPAN_DEFAULT)) continue;
        float cx = 0, cy = 0;
        if (!elemCenter(el, &cx, &cy)) continue;
        int at = n++;                                   // insertion sort, descending x
        while (at > 0 && xs[at - 1] < cx) { xs[at] = xs[at - 1]; out[at] = out[at - 1]; at--; }
        xs[at] = cx; out[at] = el;
    }
    if (xOut) for (int i = 0; i < n; i++) xOut[i] = xs[i];
    return n;
}

void feDumpFocusVector(const char* why) {
    if (!axDebugLogEnabled()) return;
    uintptr_t begin = 0, end = 0;
    if (!safeReadPtr(g_base + VEC_BEGIN_RVA, &begin) ||
        !safeReadPtr(g_base + VEC_END_RVA, &end) || !begin || end <= begin) {
        logLine("fe-dump (%s): the element vector is empty or unreadable", why ? why : "?");
        return;
    }
    uintptr_t n = (end - begin) / ELEM_STRIDE;
    if (n > 4096) n = 4096;
    logLine("fe-dump (%s): %llu elements", why ? why : "?", (unsigned long long)n);
    for (uintptr_t i = 0; i < n && i < 64; i++) {
        uintptr_t e = begin + i * ELEM_STRIDE;
        int64_t id = 0; safeReadI64(e + ELEM_ID_OFF, &id);
        float x = 0, y = 0; elemPos(e, &x, &y);
        uint32_t wb = 0, hb = 0;
        safeReadU32(e + ELEM_SIZE_OFF, &wb);
        safeReadU32(e + ELEM_SIZE_OFF + 4, &hb);
        uint8_t f = 0, s = 0;
        safeReadU8(e + ELEM_FOCUSABLE_OFF, &f);
        safeReadU8(e + ELEM_SKIP_OFF, &s);
        uintptr_t owner = 0; safeReadPtr(e + ELEM_OWNER_OFF, &owner);
        char tag[9]; idToAscii(id, tag);
        logLine("  fe[%2llu] id=0x%llx \"%s\" pos=(%.0f,%.0f) size=(%.0fx%.0f) "
                "focusable=%u skip=%u owner=%p",
                (unsigned long long)i, (unsigned long long)id, tag, x, y,
                u32AsFloatM(wb), u32AsFloatM(hb), f, s, (void*)owner);
    }
}

bool frontEndClickElementId(int64_t id) {
    if (clickQueued()) {                                // a click is already in flight
        logLine("fe-click id=0x%llx REFUSED: a click is already queued (n=%d)",
                (unsigned long long)id, g_synthCount);
        return false;
    }
    uintptr_t elem = focusElementById(g_base, id);
    if (!elem) {
        logLine("fe-click id=0x%llx: no such element on screen", (unsigned long long)id);
        return false;
    }
    float tx = 0, ty = 0;
    if (!elemCenter(elem, &tx, &ty)) {
        logLine("fe-click id=0x%llx: no centre", (unsigned long long)id);
        return false;
    }
    moveCursorTo(tx, ty);                               // pins the globals + queues the hover (frame 0)
    enqueueSynth(SDL_EVT_MOUSEBUTTONDOWN, SDL_BUTTON_LEFT, 1, 1);
    enqueueSynth(SDL_EVT_MOUSEBUTTONUP,   SDL_BUTTON_LEFT, 0, 2);
    g_axSpokenId = id;
    g_axLastSpoken[0] = 0;
    logLine("fe-click id=0x%llx at (%.0f,%.0f)", (unsigned long long)id, tx, ty);
    return true;
}

// ---- Synthesised keyboard input ----
struct SynthKey { uint32_t type, scancode, sym; uint16_t mod; };
static SynthKey g_synthKey[4];
static int      g_synthKeyCount = 0;
static uint32_t g_synthKeyFrame = 0xffffffffu;   // the frame we last emitted on

void enqueueSynthKey(uint32_t type, uint32_t scancode, uint32_t sym, uint16_t mod) {
    if (g_synthKeyCount >= (int)(sizeof g_synthKey / sizeof g_synthKey[0])) {
        logLine("synthkey: QUEUE FULL — dropped type=0x%x scan=%u sym=0x%x", type, scancode, sym);
        return;
    }
    SynthKey& k = g_synthKey[g_synthKeyCount++];
    k.type = type; k.scancode = scancode; k.sym = sym; k.mod = mod;
}

bool emitSynthKey(void* ev) {
    if (g_synthKeyCount == 0 || g_synthKeyFrame == g_synthFrame) return false;
    g_synthKeyFrame = g_synthFrame;
    SynthKey k = g_synthKey[0];
    for (int i = 1; i < g_synthKeyCount; i++) g_synthKey[i - 1] = g_synthKey[i];
    g_synthKeyCount--;
    uintptr_t e = (uintptr_t)ev;
    for (int i = 0; i < SDL_EVENT_SIZE; i += 8) safeWriteU64(e + i, 0);   // clear the union
    safeWriteU32(e + 0x00,               k.type);
    safeWriteU8 (e + SDL_KEY_STATE_OFF,  k.type == SDL_EVT_KEYDOWN ? 1 : 0);
    safeWriteU8 (e + SDL_KEY_REPEAT_OFF, 0);
    safeWriteU32(e + SDL_KEY_SCAN_OFF,   k.scancode);
    safeWriteU32(e + SDL_KEY_SYM_OFF,    k.sym);
    safeWriteU32(e + SDL_KEY_MOD_OFF,    k.mod);
    return true;
}

// ---- Input probe, NOW OFF ----
#define DBG_INPUT_PROBE (axDebugLogEnabled())
static int      g_probeCtrlConnected = -1;         // last logged value; -1 = not read yet
static uint32_t g_probeInputGate     = 0xdeadbeef;
static uint32_t g_probeInputMode     = 0xdeadbeef;
static int      g_probeSuppressA     = -1;         // DAT_140d9607a: mouse suppressed / gamepad cursor
static int      g_probeSuppressB     = -1;         // DAT_140d9607b: the OR-ed master bit
static int      g_probeInWindow      = -1;         // DAT_140d9607d: mouse inside the window
static int      g_probeEnableGate    = -1;
static DWORD    g_probeFightTick     = 0;          // rate limit: cursor-fight lines
static DWORD    g_probeAxisTick      = 0;          // rate limit: axis-motion lines

bool readInputEnableStack(uintptr_t base, int* gateOut, int* sizeOut) {
    uintptr_t words = 0; int64_t nbits = 0;
    if (!safeReadPtr(base + INPUT_ENABLE_VEC_RVA, &words)) return false;
    if (!safeReadI64(base + INPUT_ENABLE_SIZE_RVA, &nbits)) return false;
    *sizeOut = (int)nbits;
    if (nbits <= 0 || nbits > 4096 || words == 0) { *gateOut = 1; return true; }
    for (int64_t i = 0; i < nbits; i += 32) {
        uint32_t w = 0;
        if (!safeReadU32(words + (i / 32) * 4, &w)) return false;
        int64_t remain = nbits - i;
        uint32_t mask = (remain >= 32) ? 0xFFFFFFFFu : ((1u << (uint32_t)remain) - 1u);
        if ((w & mask) != mask) { *gateOut = 0; return true; }
    }
    *gateOut = 1;
    return true;
}

void serviceInputProbe(uintptr_t base) {
    if (!DBG_INPUT_PROBE) return;
    uint8_t con = 0;
    if (safeReadU8(base + CTRL_CONNECTED_RVA, &con) && (int)con != g_probeCtrlConnected) {
        logLine("ctrlprobe: controller-connected %d -> %d", g_probeCtrlConnected, (int)con);
        g_probeCtrlConnected = (int)con;
    }
    uint32_t v = 0;
    if (safeReadU32(base + INPUT_GATE_RVA, &v) && v != g_probeInputGate) {
        logLine("ctrlprobe: input gate d9e330 0x%x -> 0x%x", g_probeInputGate, v);
        g_probeInputGate = v;
    }
    if (safeReadU32(base + INPUT_MODE_RVA, &v) && v != g_probeInputMode) {
        logLine("ctrlprobe: input mode d9e334 0x%x -> 0x%x", g_probeInputMode, v);
        g_probeInputMode = v;
    }
    uint8_t b = 0;
    if (safeReadU8(base + MOUSE_SUPPRESS_RVA, &b) && (int)b != g_probeSuppressA) {
        logLine("ctrlprobe: d9607a (mouse suppressed) %d -> %d", g_probeSuppressA, (int)b);
        g_probeSuppressA = (int)b;
    }
    if (safeReadU8(base + MOUSE_SUPPRESS_B_RVA, &b) && (int)b != g_probeSuppressB) {
        logLine("ctrlprobe: d9607b (master bit) %d -> %d", g_probeSuppressB, (int)b);
        g_probeSuppressB = (int)b;
    }
    if (safeReadU8(base + MOUSE_INWINDOW_RVA, &b) && (int)b != g_probeInWindow) {
        logLine("ctrlprobe: d9607d (mouse in window) %d -> %d", g_probeInWindow, (int)b);
        g_probeInWindow = (int)b;
    }
    int egate = 0, esize = 0;
    if (readInputEnableStack(base, &egate, &esize)) {
        int packed = (egate & 1) | (esize << 1);
        if (packed != g_probeEnableGate) {
            logLine("ctrlprobe: input-enable stack gate=%s size=%d",
                    egate ? "OPEN" : "BLOCKED", esize);
            g_probeEnableGate = packed;
        }
    }
    uint32_t vdx = 0, vdy = 0;
    if (safeReadU32(base + VP_DEN_X_RVA, &vdx) && safeReadU32(base + VP_DEN_Y_RVA, &vdy)) {
        int packed = (int)((vdx >> 8) ^ (vdy >> 7));
        static int lastVp = -1;
        if (packed != lastVp) {
            lastVp = packed;
            uint32_t vn = 0, vox = 0, voy = 0, vbx = 0, vby = 0;
            safeReadU32(base + VP_NUM_RVA, &vn);
            safeReadU32(base + VP_OFF_X_RVA, &vox); safeReadU32(base + VP_OFF_Y_RVA, &voy);
            safeReadU32(base + VP_BOUND_X_RVA, &vbx); safeReadU32(base + VP_BOUND_Y_RVA, &vby);
            logLine("ctrlprobe: viewport num=%.0f den=(%.0f,%.0f) off=(%.0f,%.0f) bounds=(%.0f,%.0f)",
                    u32AsFloatM(vn), u32AsFloatM(vdx), u32AsFloatM(vdy),
                    u32AsFloatM(vox), u32AsFloatM(voy), u32AsFloatM(vbx), u32AsFloatM(vby));
        }
    }
    if (g_cursorValid) {
        uint32_t xb = 0, yb = 0;
        if (safeReadU32(base + FE_CURSOR_X_RVA, &xb) && safeReadU32(base + FE_CURSOR_Y_RVA, &yb)) {
            float gx = u32AsFloatM(xb), gy = u32AsFloatM(yb);
            float pwx = 0, pwy = 0;
            designToWindow(g_cursorFx, g_cursorFy, &pwx, &pwy);
            float dx = gx - pwx, dy = gy - pwy;
            if (dx < 0) dx = -dx; if (dy < 0) dy = -dy;
            DWORD now = GetTickCount();
            if ((dx > 0.5f || dy > 0.5f) && (now - g_probeFightTick) > 1000) {
                g_probeFightTick = now;
                logLine("ctrlprobe: cursor fight — game holds (%.0f,%.0f), mod pinned (%.0f,%.0f) "
                        "[design (%.0f,%.0f)]",
                        gx, gy, pwx, pwy, g_cursorFx, g_cursorFy);
            }
        }
    }
}

void logJoyDeviceEvent(void* ev) {
    if (!DBG_INPUT_PROBE) return;
    uint32_t t = 0;
    if (!safeReadU32((uintptr_t)ev, &t)) return;
    if (t == SDL_EVT_JOYDEVICEADDED || t == SDL_EVT_JOYDEVICEREMOVED ||
        t == SDL_EVT_CONTROLLERDEVICEADDED || t == SDL_EVT_CONTROLLERDEVICEREMOVED) {
        logLine("joydev: device event 0x%x (%s)", t,
                (t == SDL_EVT_JOYDEVICEADDED || t == SDL_EVT_CONTROLLERDEVICEADDED)
                    ? "added" : "removed");
    } else if (t == SDL_EVT_JOYBUTTONDOWN || t == SDL_EVT_CONTROLLERBUTTONDOWN) {
        logLine("joydev: button down 0x%x", t);
    } else if (t == SDL_EVT_JOYAXISMOTION || t == SDL_EVT_CONTROLLERAXISMOTION) {
        static uint32_t axisCount = 0;
        axisCount++;
        DWORD now = GetTickCount();
        if (now - g_probeAxisTick > 1000) {
            logLine("joydev: axis motion 0x%x — %u event(s) in the last %lu ms", t, axisCount,
                    (unsigned long)(g_probeAxisTick ? now - g_probeAxisTick : 0));
            g_probeAxisTick = now;
            axisCount = 0;
        }
    }
}

// ---- Click-failure diagnostics ----

void logInputSnapshot(uintptr_t base, const char* why) {
    uint8_t con = 0xff, supA = 0xff, supB = 0xff, inWin = 0xff;
    uint32_t gate = 0xdeadbeef, mode = 0xdeadbeef;
    safeReadU8(base + CTRL_CONNECTED_RVA, &con);
    safeReadU8(base + MOUSE_SUPPRESS_RVA, &supA);
    safeReadU8(base + MOUSE_SUPPRESS_B_RVA, &supB);
    safeReadU8(base + MOUSE_INWINDOW_RVA, &inWin);
    safeReadU32(base + INPUT_GATE_RVA, &gate);
    safeReadU32(base + INPUT_MODE_RVA, &mode);
    int egate = -1, esize = -1;
    readInputEnableStack(base, &egate, &esize);
    logLine("axdiag[%s]: ctrl-connected=%d input-mode=0x%x gate=0x%x suppress=%d/%d "
            "in-window=%d enable-stack=%s(%d bits)",
            why, (int)con, mode, gate, (int)supA, (int)supB, (int)inWin,
            egate == 1 ? "OPEN" : egate == 0 ? "BLOCKED" : "unreadable", esize);
    uint32_t vn = 0, vdx = 0, vdy = 0, vox = 0, voy = 0;
    safeReadU32(base + VP_NUM_RVA, &vn);
    safeReadU32(base + VP_DEN_X_RVA, &vdx);  safeReadU32(base + VP_DEN_Y_RVA, &vdy);
    safeReadU32(base + VP_OFF_X_RVA, &vox);  safeReadU32(base + VP_OFF_Y_RVA, &voy);
    logLine("axdiag[%s]: viewport num=%.0f den=(%.0f,%.0f) off=(%.0f,%.0f) "
            "(den = the real window size)",
            why, u32AsFloatM(vn), u32AsFloatM(vdx), u32AsFloatM(vdy),
            u32AsFloatM(vox), u32AsFloatM(voy));
    char q[160]; int p = 0;
    for (int i = 0; i < g_synthCount && p < (int)sizeof q - 24; i++) {
        int w = _snprintf(q + p, sizeof q - p, " [%d]t=0x%x f=%u",
                          i, g_synth[i].type, g_synth[i].frame);
        if (w <= 0) break;              // truncated: stop rather than walk p backwards
        p += w;
    }
    q[p >= 0 && p < (int)sizeof q ? p : (int)sizeof q - 1] = 0;
    logLine("axdiag[%s]: synthq n=%d frame-now=%u%s cursor=%s(%.0f,%.0f)",
            why, g_synthCount, g_synthFrame, g_synthCount ? q : " (empty)",
            g_cursorValid ? "valid" : "UNPLACED", g_cursorFx, g_cursorFy);
}

void diagDumpFocusElements(uintptr_t base, const char* why) {
    if (!axDebugLogEnabled()) return;
    static DWORD lastDump = 0;
    DWORD now = GetTickCount();
    if (lastDump && now - lastDump < 2000) return;
    lastDump = now;
    uintptr_t begin = 0, end = 0;
    if (!safeReadPtr(base + VEC_BEGIN_RVA, &begin) ||
        !safeReadPtr(base + VEC_END_RVA, &end) || !begin || end < begin) {
        logLine("axdiag[%s]: element vector unreadable (begin=%p end=%p)",
                why, (void*)begin, (void*)end);
        return;
    }
    int n = (int)((end - begin) / ELEM_STRIDE);
    int shown = n > 300 ? 300 : n;
    logLine("axdiag[%s]: element dump, %d element(s) follows", why, n);
    logDump("==== axdiag element dump (%s): %d element(s)%s ====",
            why, n, n > shown ? " (first 300)" : "");
    for (int i = 0; i < shown; i++) {
        uintptr_t el = begin + (uintptr_t)i * ELEM_STRIDE;
        int64_t id = 0; uintptr_t owner = 0;
        safeReadI64(el + ELEM_ID_OFF, &id);
        safeReadPtr(el + ELEM_OWNER_OFF, &owner);
        float x = 0, y = 0, cx = 0, cy = 0;
        elemPos(el, &x, &y);
        elemCenter(el, &cx, &cy);
        char tag[5];
        for (int b = 0; b < 4; b++) {
            char c = (char)((id >> (b * 8)) & 0xff);
            tag[b] = (c >= 0x20 && c < 0x7f) ? c : '.';
        }
        tag[4] = 0;
        logDump("  [%3d] id=0x%llx \"%s\" owner=%p pos=(%.0f,%.0f) centre=(%.0f,%.0f)",
                i, (unsigned long long)id, tag, (void*)owner, x, y, cx, cy);
    }
}
