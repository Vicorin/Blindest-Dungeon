// input/pad.cpp -- the GAMEPAD READER

#include <windows.h>
#include <cstdio>
#include <cstring>
#include <cmath>
#include "internal.h"
#include "game/offsets.h"

// ---- THE TABLE'S SHAPE ----
static const int      PAD_MAX_PADS     = 4;     // the poll's handle array is 4 qwords wide
static const uint32_t PAD_PAD_STRIDE   = 300;   // bytes per pad (memset(.., 300) on detach)
static const uint32_t PAD_ENTRY_STRIDE = 12;
static const uint32_t PAD_STICK_OFF    = 0x10c; // + stick * 16: float x, float y
static const uint32_t PAD_TRIGGER_OFF  = 0xfc;  // + trigger * 8: float value, float delta

static const char* const kPadActionName[PAD_ACTION_COUNT] = {
    "A", "B", "X", "Y", "Start", "Back", "LB", "RB", "LS click", "RS click",
    "dpad left", "dpad right", "dpad up", "dpad down", "LT", "RT",
    "(16)", "Back (hold copy)", "(18)", "LB (hold copy)", "RB (hold copy)",
    "(21)", "(22)", "(23)", "(24)",
};

const char* padActionName(int action) {
    if (action < 0 || action >= PAD_ACTION_COUNT) return "?";
    return kPadActionName[action];
}

// ---- STATE ----
static uint8_t  g_padLastState[PAD_MAX_PADS][PAD_ACTION_COUNT];
static bool     g_padInit       = false;
static uint32_t g_padPressedBits  = 0;   // actions whose press edge is current (any pad)
static uint32_t g_padReleasedBits = 0;   // actions whose release edge is current (any pad)
static uint32_t g_padDownBits     = 0;   // actions in state 1 or 2 on any pad, this read
static int      g_padCount        = 0;   // pads the poll serviced on the last read
static float    g_padStickX[2]    = { 0.0f, 0.0f };
static float    g_padStickY[2]    = { 0.0f, 0.0f };
static bool     g_padDriving      = false;
static int      g_padLoggedCount  = -1;  // last pad count the log reported
static int      g_padLoggedStick  = -1;

// ---- THE RAW CAPTURE (filled by the detours, read by servicePad once they are installed) ----
typedef uint8_t (*PadGetButtonFn)(void* ctrl, int button);
typedef int16_t (*PadGetAxisFn)(void* ctrl, int axis);
static PadGetButtonFn g_origPadGetButton = nullptr;
static PadGetAxisFn   g_origPadGetAxis   = nullptr;
static bool     g_padHooked         = false;
static void*    g_rawHandle[PAD_MAX_PADS];
static uint32_t g_rawDown[PAD_MAX_PADS];             // PadAction bits currently down (SDL buttons only)
static int16_t  g_rawAxis[PAD_MAX_PADS][6];
static uint32_t g_rawPrevAll        = 0;
static uint32_t g_rawSeq            = 0;
static uint32_t g_rawSeqSeen        = 0xffffffffu;   // the sample servicePad last turned into edges
static uint32_t g_padClaimButtons   = 0;
static bool     g_padClaimLeftStick = false;         // ...and the left stick's two axes
static bool     g_padClaimTriggers  = false;         // ...and both trigger axes (= the LT/RT buttons)
static bool     g_padClaimRightStick = false;
static const int16_t PAD_TRIGGER_ON = 8192;

static const int8_t kSdlButtonToAction[15] = {
    PAD_A, PAD_B, PAD_X, PAD_Y, PAD_BACK, -1, PAD_START, PAD_LS_CLICK, PAD_RS_CLICK,
    PAD_LB, PAD_RB, PAD_DPAD_UP, PAD_DPAD_DOWN, PAD_DPAD_LEFT, PAD_DPAD_RIGHT,
};

static int padHandleIndex(void* h) {
    for (int i = 0; i < PAD_MAX_PADS; i++) if (g_rawHandle[i] == h) return i;
    for (int i = 0; i < PAD_MAX_PADS; i++) if (!g_rawHandle[i]) { g_rawHandle[i] = h; return i; }
    return PAD_MAX_PADS - 1;
}

// ---- THE PAD IS SILENT WHILE THE MOD'S MOUSE IS TALKING ----
static DWORD g_padMuteUntil = 0;                      // 0 = not muted
void padMuteGame(DWORD ms) {
    DWORD t = GetTickCount() + ms;
    if (!g_padMuteUntil || (long)(t - g_padMuteUntil) > 0) g_padMuteUntil = t;
}
static bool padMuted() {
    if (!g_padMuteUntil) return false;
    if ((long)(GetTickCount() - g_padMuteUntil) < 0) return true;
    g_padMuteUntil = 0;
    return false;
}

// ---- THE DETOURS ----
uint8_t OurPadGetButton(void* ctrl, int button) {
    uint8_t raw = g_origPadGetButton ? g_origPadGetButton(ctrl, button) : 0;
    if (button < 0 || button >= 15) return raw;
    int a = kSdlButtonToAction[button];
    if (a < 0) return raw;
    int p = padHandleIndex(ctrl);
    if (raw) g_rawDown[p] |= 1u << a; else g_rawDown[p] &= ~(1u << a);
    g_rawSeq++;
    if (padMuted()) return 0;
    return (g_padClaimButtons & (1u << a)) ? 0 : raw;
}

int16_t OurPadGetAxis(void* ctrl, int axis) {
    int16_t raw = g_origPadGetAxis ? g_origPadGetAxis(ctrl, axis) : 0;
    if (axis < 0 || axis >= 6) return raw;
    int p = padHandleIndex(ctrl);
    g_rawAxis[p][axis] = raw;
    g_rawSeq++;
    if (padMuted()) return 0;
    if ((axis == 0 || axis == 1) && g_padClaimLeftStick)  return 0;
    if ((axis == 2 || axis == 3) && g_padClaimRightStick) return 0;
    if ((axis == 4 || axis == 5) && g_padClaimTriggers)   return 0;
    return raw;
}

void padSetOriginals(void* origGetButton, void* origGetAxis) {
    g_origPadGetButton = (PadGetButtonFn)origGetButton;
    g_origPadGetAxis   = (PadGetAxisFn)origGetAxis;
    g_padHooked = g_origPadGetButton != nullptr && g_origPadGetAxis != nullptr;
}
bool padHooksInstalled() { return g_padHooked; }
uint32_t padSampleSeq()  { return g_rawSeqSeen; }
void padSetClaim(uint32_t actionBits, bool leftStick, bool triggers, bool rightStick) {
    g_padClaimButtons    = actionBits;
    g_padClaimLeftStick  = leftStick;
    g_padClaimTriggers   = triggers;
    g_padClaimRightStick = rightStick;
}
bool padClaimActive() {
    return g_padHooked && (g_padClaimButtons || g_padClaimLeftStick || g_padClaimTriggers || g_padClaimRightStick);
}

static float u32f(uint32_t v) { float f; memcpy(&f, &v, 4); return f; }

static int padStickOctant(float x, float y) {
    if (x * x + y * y < 0.25f) return -1;
    double a = atan2((double)y, (double)x);          // -pi..pi
    int oct = (int)floor((a + 3.14159265 / 8.0) / (3.14159265 / 4.0));
    return ((oct % 8) + 8) % 8;
}

bool padIsDriving() { return g_padDriving; }
int  padCount()     { return g_padCount; }
bool padPressed(int action)  { return action >= 0 && action < 32 && (g_padPressedBits  & (1u << action)) != 0; }
bool padReleased(int action) { return action >= 0 && action < 32 && (g_padReleasedBits & (1u << action)) != 0; }
bool padDown(int action)     { return action >= 0 && action < 32 && (g_padDownBits     & (1u << action)) != 0; }
void padStick(int stick, float* x, float* y) {
    int s = (stick == 1) ? 1 : 0;
    if (x) *x = g_padStickX[s];
    if (y) *y = g_padStickY[s];
}

// ---- THE PER-FRAME READ, DETOUR EDITION ----
static void padServiceRaw(uint32_t count, bool dbg) {
    if (count == 0) {                                     // the game closed its handles: forget ours
        memset(g_rawHandle, 0, sizeof g_rawHandle);
        memset(g_rawDown, 0, sizeof g_rawDown);
        g_rawPrevAll = 0;
        memset(g_rawAxis, 0, sizeof g_rawAxis);
        g_padPressedBits = g_padReleasedBits = g_padDownBits = 0;
        g_padStickX[0] = g_padStickY[0] = g_padStickX[1] = g_padStickY[1] = 0.0f;
        return;
    }
    if (g_rawSeq == g_rawSeqSeen) return;                 // same frame, another pump pass: edges stand
    g_rawSeqSeen = g_rawSeq;
    // ---- ONE PRESS, HOWEVER MANY HANDLES REPORT IT ----
    uint32_t downAll = 0, byPad[PAD_MAX_PADS] = { 0, 0, 0, 0 };
    for (int p = 0; p < PAD_MAX_PADS; p++) {
        if (!g_rawHandle[p]) continue;
        uint32_t d = g_rawDown[p];
        if (g_rawAxis[p][4] >= PAD_TRIGGER_ON) d |= 1u << PAD_LT;
        if (g_rawAxis[p][5] >= PAD_TRIGGER_ON) d |= 1u << PAD_RT;
        byPad[p] = d;
        downAll |= d;
    }
    uint32_t newly = downAll & ~g_rawPrevAll, gone = g_rawPrevAll & ~downAll;
    g_rawPrevAll = downAll;
    if (newly) g_inputIsController = true;                // the glyph choice learns about the pad here
    if (dbg) {
        for (int a = 0; a < 16; a++) {
            uint32_t mask = 0;
            for (int p = 0; p < PAD_MAX_PADS; p++) if (byPad[p] & (1u << a)) mask |= 1u << p;
            if (newly & (1u << a)) logLine("pad: %s pressed (raw, pads 0x%x)", padActionName(a), mask);
            if (gone  & (1u << a)) logLine("pad: %s released (raw)", padActionName(a));
        }
    }
    g_padPressedBits = newly; g_padReleasedBits = gone; g_padDownBits = downAll;
    for (int p = 0; p < PAD_MAX_PADS; p++) {
        if (!g_rawHandle[p]) continue;
        g_padStickX[0] = g_rawAxis[p][0] / 32767.0f; g_padStickY[0] = g_rawAxis[p][1] / 32767.0f;
        g_padStickX[1] = g_rawAxis[p][2] / 32767.0f; g_padStickY[1] = g_rawAxis[p][3] / 32767.0f;
        break;
    }
    int oct = padStickOctant(g_padStickX[0], g_padStickY[0]);
    if (oct != g_padLoggedStick) {
        static const char* const kOct[8] = { "right", "down-right", "down", "down-left",
                                             "left", "up-left", "up", "up-right" };
        if (dbg) logLine("pad: left stick %s (%.2f, %.2f)", oct < 0 ? "centred" : kOct[oct],
                         g_padStickX[0], g_padStickY[0]);
        g_padLoggedStick = oct;
    }
}

// ---- THE PER-FRAME READ ----
void servicePad(uintptr_t base) {
    if (!g_padInit) { memset(g_padLastState, 0xff, sizeof g_padLastState); g_padInit = true; }
    const bool dbg = axDebugLogEnabled();

    uint32_t mode = 0; uint8_t suppress = 0;
    safeReadU32(base + INPUT_MODE_RVA, &mode);
    safeReadU8(base + MOUSE_SUPPRESS_RVA, &suppress);
    bool driving = (mode == 1) || (mode == 0 && suppress != 0);
    if (driving != g_padDriving) {
        g_padDriving = driving;
        if (dbg) logLine("pad: %s driving (mode=%u suppress=%u)", driving ? "pad is" : "pad stopped", mode, suppress);
    }

    uint32_t count = 0;
    safeReadU32(base + INPUT_GATE_RVA, &count);
    if (count > (uint32_t)PAD_MAX_PADS) count = PAD_MAX_PADS;   // the handle array is 4 wide
    g_padCount = (int)count;
    if ((int)count != g_padLoggedCount) {
        if (dbg) logLine("pad: %u pad(s) attached", count);
        g_padLoggedCount = (int)count;
        if (count == 0) memset(g_padLastState, 0xff, sizeof g_padLastState);
    }

    if (g_padHooked) { padServiceRaw(count, dbg); return; }

    uint32_t pressed = 0, released = 0, down = 0;
    for (int a = 0; a < PAD_ACTION_COUNT; a++) {
        bool any1 = false, any2 = false, any3 = false;
        for (uint32_t p = 0; p < count; p++) {
            uintptr_t e = base + PAD_ACTION_TABLE_RVA + p * PAD_PAD_STRIDE + (uintptr_t)a * PAD_ENTRY_STRIDE;
            uint32_t st = 0; uint8_t blocked = 0;
            if (!safeReadU32(e, &st) || !safeReadU8(e + 8, &blocked)) continue;
            if (blocked) st = 0;                       // the accessors' rule: blocked reads as up
            if (st == 1) any1 = true; else if (st == 2) any2 = true; else if (st == 3) any3 = true;
            // (st > 3 = a torn or foreign value: never trusted, counts as up)
        }
        uint32_t st = any2 ? 2 : any1 ? 1 : any3 ? 3 : 0;
        uint8_t prev = g_padLastState[0][a];
        if (st == 1 && prev != 1) {
            pressed |= 1u << a;
            if (dbg) logLine("pad: %s pressed", padActionName(a));
            g_inputIsController = true;
        } else if (st == 3 && prev != 3) {
            released |= 1u << a;
            if (dbg) logLine("pad: %s released", padActionName(a));
        } else if (st == 1 && prev == 1) {
            pressed |= 1u << a;                        // same frame, another OurPoll pass: still current
        } else if (st == 3 && prev == 3) {
            released |= 1u << a;
        }
        if (st == 1 || st == 2) down |= 1u << a;
        g_padLastState[0][a] = (uint8_t)st;
    }
    for (uint32_t p = 0; p < count; p++) {
        uintptr_t padBase = base + PAD_ACTION_TABLE_RVA + p * PAD_PAD_STRIDE;
        if (p == 0) {
            for (int s = 0; s < 2; s++) {
                uint32_t xb = 0, yb = 0;
                uintptr_t so = padBase + PAD_STICK_OFF + (uintptr_t)s * 16;
                if (safeReadU32(so, &xb) && safeReadU32(so + 4, &yb)) {
                    g_padStickX[s] = u32f(xb); g_padStickY[s] = u32f(yb);
                }
            }
            int oct = padStickOctant(g_padStickX[0], g_padStickY[0]);
            if (oct != g_padLoggedStick) {
                static const char* const kOct[8] = { "right", "down-right", "down", "down-left",
                                                     "left", "up-left", "up", "up-right" };
                if (dbg) logLine("pad: left stick %s (%.2f, %.2f)", oct < 0 ? "centred" : kOct[oct],
                                 g_padStickX[0], g_padStickY[0]);
                g_padLoggedStick = oct;
            }
        }
    }
    g_padPressedBits = pressed; g_padReleasedBits = released; g_padDownBits = down;
}
