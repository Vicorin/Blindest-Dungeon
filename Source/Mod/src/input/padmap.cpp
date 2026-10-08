// input/padmap.cpp -- the PAD -> KEY TRANSLATOR

#include <windows.h>
#include <cstdio>
#include <cstring>
#include <cmath>
#include "internal.h"
#include "game/offsets.h"

// ---- TIMING ----
static const DWORD PM_NAV_DELAY_MS  = 320;   // held KMF_REPEAT input: first repeat after this
static const DWORD PM_NAV_REPEAT_MS = 110;
static const DWORD PM_TAP_MS        = 450;   // a modifier let go within this, unchorded = a tap
static const DWORD PM_HOLD_MS       = 450;   // an armed input held this long = its hold binding
static const int   PM_SHIFT_HOLD_FRAMES = 6;
static const float PM_STICK_DEAD    = 0.40f; // a stick this far from centre (of 1.0) is a push

// ---- STATE ----
static bool     g_pmActive   = false;        // claims are set this frame
static uint32_t g_pmSeqSeen  = 0xffffffffu;
static bool     g_pmDown[PI_COUNT];
static bool     g_pmPrev[PI_COUNT];          // ...and the previous sample's
static bool     g_pmPressed[PI_COUNT];       // this sample's edges
static bool     g_pmReleased[PI_COUNT];
static DWORD    g_pmDownSince[PI_COUNT];     // press tick per input (the tap / hold clocks)
static bool     g_pmChorded[PI_COUNT];
static bool     g_pmIsMod[PI_COUNT];
struct PmArm { bool armed; bool fired; int pressFn, pressSlot, holdFn, holdSlot; };
static PmArm    g_pmArm[PI_COUNT];
// The one auto-repeating function (a held direction).
static int      g_pmRepFn = -1, g_pmRepSlot = 0;
static uint8_t  g_pmRepIn = PI_NONE;
static uint32_t g_pmRepSym = 0;
static uint16_t g_pmRepMod = 0;
static DWORD    g_pmRepNext = 0;
static uint32_t g_pmHoldSym  = 0, g_pmHoldScan = 0;
static uint8_t  g_pmHoldIn   = PI_NONE;
static bool     g_pmHoldRelDue = false;      // a KEYUP could not be queued: retry every pass
static bool     g_pmLoggedOff = false;
static bool     g_pmSupForced = false;
static int      g_fnBack = -2, g_fnStack = -2;

bool padMapActive() { return g_pmActive; }

static const char* const kPiName[PI_COUNT] = {
    "A", "B", "X", "Y", "Start", "Back", "LB", "RB", "LS click", "RS click",
    "dpad left", "dpad right", "dpad up", "dpad down", "LT", "RT",
    "right stick left", "right stick right", "right stick up", "right stick down",
};
static const char* pmInName(uint8_t in) { return in < PI_COUNT ? kPiName[in] : "?"; }

static bool pmKey(uintptr_t base, const char* from, uint32_t sym, uint16_t mod, uint8_t rep,
                  bool pass, const char* what) {
    bool claimed = axRoutePadKey(base, SDL_EVT_KEYDOWN, sym, mod, rep, pass);
    if (!rep && axDebugLogEnabled())
        logLine("padmap: %s -> %s -> %s", from, what,
                claimed ? "claimed" : pass ? "unclaimed, handed to the game" : "unclaimed, dropped");
    return claimed;
}

static void pmDrop(const char* from, const char* why) {
    if (axDebugLogEnabled()) logLine("padmap: %s -> dropped (%s)", from, why);
}

// ---- THE HELD FORWARD (the walk), and why its release is the safety-critical half ----
static void pmHoldRelease(const char* why) {
    if (!g_pmHoldSym) return;
    if (!enqueueSynthKey(SDL_EVT_KEYUP, g_pmHoldScan, g_pmHoldSym, 0)) {
        g_pmHoldRelDue = true;                            // the queue was full: try again next pass
        logLine("padmap: held '%c' release could not be queued, retrying", (char)g_pmHoldSym);
        return;
    }
    if (axDebugLogEnabled()) logLine("padmap: held '%c' released (%s)", (char)g_pmHoldSym, why);
    g_pmHoldSym = 0; g_pmHoldScan = 0; g_pmHoldIn = PI_NONE; g_pmHoldRelDue = false;
}

static void pmHoldBegin(uintptr_t base, const char* from, uint8_t in, KmChord c) {
    if (g_pmHoldSym) { pmDrop(from, "a held key's release is still pending"); return; }
    uint32_t scan = klScancodeForKey(c.sym);
    if (!scan) { pmDrop(from, "the layout has no scancode for the held key"); return; }
    uint16_t mod = 0;
    if (c.mods & KM_SHIFT) mod |= KMOD_LSHIFT;
    if (c.mods & KM_CTRL)  mod |= KMOD_LCTRL;
    if (axRoutePadKey(base, SDL_EVT_KEYDOWN, c.sym, mod, 0, false)) {
        if (axDebugLogEnabled()) logLine("padmap: %s -> '%c' -> claimed", from, (char)c.sym);
        return;
    }
    if (!enqueueSynthKey(SDL_EVT_KEYDOWN, scan, c.sym, mod)) { pmDrop(from, "synth-key queue full"); return; }
    g_pmHoldSym = c.sym; g_pmHoldScan = scan; g_pmHoldIn = in;
    if (axDebugLogEnabled()) logLine("padmap: %s -> holding '%c' (scan=%u)", from, (char)c.sym, scan);
}

// ---- WHICH BINDINGS APPLY WHERE THE PLAYER STANDS ----
struct PmPlace { KmRegion region; AxContext ctx; };
static bool pmApplies(uintptr_t base, const PmPlace& P, int f, bool forClaim, bool* useDefault) {
    const KmFunc* F = kmAt(f);
    *useDefault = false;
    if (P.region == KMR_NONE) {
        if (F->flags & KMF_GLOBAL) { if (g_textInputActive) return false; return true; }
        if (F->group != KMG_GENERAL) return false;
        *useDefault = true;
        return forClaim || F->tag == KMX_ANY || kmTagActive(base, P.ctx, F->tag);
    }
    if (!(F->flags & KMF_GLOBAL)) {
        uint8_t bit = (P.region == KMR_TOWN) ? KMS_TOWN : KMS_DUNGEON;
        if (!(F->scope & bit)) return false;
    } else if (g_textInputActive) return false;
    return forClaim || F->tag == KMX_ANY || kmTagActive(base, P.ctx, F->tag);
}
static PadBind pmBind(int f, int s, bool useDefault) {
    if (useDefault) return kmAt(f)->defPad[s];
    PadBind z = { PI_NONE, PI_NONE, 0 };
    return kmPadIsBlank(f, s) ? z : kmPadCurrent(f, s);
}

static int pmRank(const KmFunc* F) {
    int r = 0;
    if (F->tag != KMX_ANY) r += 2;
    if (F->scope != KMS_BOTH) r += 1;
    return r;
}
static void pmResolve(uintptr_t base, const PmPlace& P, uint8_t in, uint8_t mod,
                      int* pressFn, int* pressSlot, int* holdFn, int* holdSlot) {
    *pressFn = *holdFn = -1; *pressSlot = *holdSlot = 0;
    int pressRank = -1, holdRank = -1;
    for (int f = 0; f < kmCount(); f++) {
        bool useDef = false;
        if (!pmApplies(base, P, f, false, &useDef)) continue;
        const KmFunc* F = kmAt(f);
        int rank = pmRank(F);
        for (int s = 0; s < F->slots; s++) {
            PadBind b = pmBind(f, s, useDef);
            if (b.in != in || b.mod != mod) continue;
            if (b.hold) { if (rank > holdRank)  { *holdFn = f;  *holdSlot = s;  holdRank = rank; } }
            else        { if (rank > pressRank) { *pressFn = f; *pressSlot = s; pressRank = rank; } }
        }
    }
}

// ---- FIRING A FUNCTION ----
static void pmFire(uintptr_t base, const PmPlace& P, int f, int s, uint8_t in, const char* from) {
    const KmFunc* F = kmAt(f);
    if (!F) return;
    KmChord c = F->defKey[s];
    uint16_t mod = 0;
    if (c.mods & KM_SHIFT) mod |= KMOD_LSHIFT;
    if (c.mods & KM_CTRL)  mod |= KMOD_LCTRL;
    if (c.mods & KM_ALT)   mod |= KMOD_LALT;
    char what[96];
    { KmChord cc = c; kmChordName(cc, what, sizeof what); }
    bool pass = (F->flags & KMF_FWD) != 0;

    if (F->flags & KMF_HELD) {
        if (!(P.region == KMR_DUNGEON) || g_textInputActive) { pmDrop(from, "a held key is only for a raid"); return; }
        pmHoldBegin(base, from, in, c);
        return;
    }
    if (f == g_fnBack) {
        if (P.ctx == AX_MAP || (P.ctx == AX_INV && !g_ihHeld && !g_ruEquipHero)) {
            pmKey(base, from, SDLK_r, 0, 0, false, "R");
            return;
        }
    } else if (f == g_fnStack) {
        synthHoldShift(PM_SHIFT_HOLD_FRAMES);
    }
    pmKey(base, from, c.sym, mod, 0, pass, what);
    if (F->flags & KMF_REPEAT) {
        g_pmRepFn = f; g_pmRepSlot = s; g_pmRepIn = in; g_pmRepSym = c.sym; g_pmRepMod = mod;
        g_pmRepNext = GetTickCount() + PM_NAV_DELAY_MS;
    }
}

static void pmRepeatEnd(uintptr_t base) {
    if (g_pmRepFn < 0) return;
    axRoutePadKey(base, SDL_EVT_KEYUP, g_pmRepSym, g_pmRepMod, 0, false);
    g_pmRepFn = -1; g_pmRepIn = PI_NONE;
}

// ---- THE INPUT LEVELS: buttons, triggers, and the two sticks folded into directions ----
static void pmReadLevels() {
    for (int i = 0; i < 16; i++) g_pmDown[i] = padDown(i);
    float x = 0.0f, y = 0.0f;
    padStick(0, &x, &y);
    if (fabsf(x) >= PM_STICK_DEAD || fabsf(y) >= PM_STICK_DEAD) {
        if (fabsf(x) > fabsf(y)) g_pmDown[x > 0.0f ? PI_DPAD_RIGHT : PI_DPAD_LEFT] = true;
        else                     g_pmDown[y > 0.0f ? PI_DPAD_DOWN : PI_DPAD_UP] = true;
    }
    for (int i = PI_RS_LEFT; i <= PI_RS_DOWN; i++) g_pmDown[i] = false;
    padStick(1, &x, &y);
    if (fabsf(x) >= PM_STICK_DEAD || fabsf(y) >= PM_STICK_DEAD) {
        if (fabsf(x) > fabsf(y)) g_pmDown[x > 0.0f ? PI_RS_RIGHT : PI_RS_LEFT] = true;
        else                     g_pmDown[y > 0.0f ? PI_RS_DOWN : PI_RS_UP] = true;
    }
}

static void pmAllOff(uintptr_t base, const char* why) {
    if (g_pmActive) { padSetClaim(0, false, false, false); g_pmActive = false; }
    pmHoldRelease(why);
    pmRepeatEnd(base);
    memset(g_pmArm, 0, sizeof g_pmArm);
    memset(g_pmPrev, 0, sizeof g_pmPrev);
    memset(g_pmChorded, 0, sizeof g_pmChorded);
}

// ---- THE PER-FRAME TRANSLATION ----
void servicePadMap(uintptr_t base) {
    if (!padHooksInstalled() || !g_enabled) {
        pmAllOff(base, "translator off");
        if (!g_pmLoggedOff && axDebugLogEnabled()) {
            logLine("padmap: off (pad detours not installed) -- the pad stays the game's");
            g_pmLoggedOff = true;
        }
        return;
    }
    kmEnsureLoaded();
    if (g_fnBack == -2)  g_fnBack  = kmFind("general.back");
    if (g_fnStack == -2) g_fnStack = kmFind("hamlet.stack");

    DWORD now = GetTickCount();
    PmPlace P;
    P.ctx = currentAxContext();
    P.region = kmRegionNow(base, P.ctx);
    bool raid = mapRoot(base) != 0;

    // ---- A new sample? Derive this frame's edges once; later OurPoll passes see none. ----
    uint32_t seq = padSampleSeq();
    bool fresh = seq != g_pmSeqSeen;
    if (fresh) {
        g_pmSeqSeen = seq;
        pmReadLevels();
        for (int i = 0; i < PI_COUNT; i++) {
            g_pmPressed[i]  = g_pmDown[i] && !g_pmPrev[i];
            g_pmReleased[i] = !g_pmDown[i] && g_pmPrev[i];
            if (g_pmPressed[i]) { g_pmDownSince[i] = now; g_pmChorded[i] = false; }
            g_pmPrev[i] = g_pmDown[i];
        }
    } else {
        memset(g_pmPressed, 0, sizeof g_pmPressed);
        memset(g_pmReleased, 0, sizeof g_pmReleased);
    }

    // ---- THE MENU'S PAD CAPTURE: the whole pad is the menu's, nothing is translated ----
    if (smPadCaptureActive()) {
        padSetClaim(0xffffu, true, true, true);
        g_pmActive = true;
        pmHoldRelease("the menu is capturing");
        pmRepeatEnd(base);
        memset(g_pmArm, 0, sizeof g_pmArm);
        if (fresh) smPadCaptureFeed(g_pmDown, g_pmPressed, g_pmReleased);
        return;
    }

    // ---- THE CLAIM POLICY for the game's NEXT poll ----
    uint32_t claim = 0;
    bool claimRs = false;
    memset(g_pmIsMod, 0, sizeof g_pmIsMod);
    for (int f = 0; f < kmCount(); f++) {
        bool useDef = false;
        if (!pmApplies(base, P, f, true, &useDef)) continue;
        const KmFunc* F = kmAt(f);
        for (int s = 0; s < F->slots; s++) {
            PadBind b = pmBind(f, s, useDef);
            if (b.in == PI_NONE) continue;
            if (b.in < 16) claim |= 1u << b.in; else claimRs = true;
            if (b.mod != PI_NONE) { if (b.mod < 16) claim |= 1u << b.mod; else claimRs = true; g_pmIsMod[b.mod] = true; }
        }
    }
    if (raid && g_campCovers) claim &= ~(1u << PI_Y);               // the camp's Y is the game's
    bool claimDpad = (claim & ((1u << PI_DPAD_UP) | (1u << PI_DPAD_DOWN) | (1u << PI_DPAD_LEFT) | (1u << PI_DPAD_RIGHT))) != 0;
    bool claimTrig = (claim & ((1u << PI_LT) | (1u << PI_RT))) != 0;
    padSetClaim(claim, claimDpad, claimTrig, claimRs);
    if (!g_pmActive) {
        g_pmActive = true;
        if (axDebugLogEnabled()) logLine("padmap: on -- the pad drives the mod's focus (table-driven)");
    }

    // ---- THE GAME IS A MOUSE PLAYER WHILE THE PAD DRIVES THE MOD ----
    {
        uint8_t sup = 0;
        if (safeReadU8(base + MOUSE_SUPPRESS_RVA, &sup) && sup != 0) {
            safeWriteU8(base + MOUSE_SUPPRESS_RVA, 0);
            if (!g_pmSupForced && axDebugLogEnabled())
                logLine("padmap: the game's 'last input was a controller' byte was 1 -- forced 0 (the mod is its mouse)");
            g_pmSupForced = true;
        } else g_pmSupForced = false;
    }

    // ---- THE HELD FORWARD'S RELEASE CONDITIONS (level-driven, every pass) ----
    if (g_pmHoldRelDue) pmHoldRelease("retry");
    if (g_pmHoldSym) {
        bool modDown = false;
        for (int i = 0; i < PI_COUNT; i++) if (g_pmIsMod[i] && g_pmDown[i]) modDown = true;
        if (!raid)                  pmHoldRelease("not in a raid");
        else if (g_textInputActive) pmHoldRelease("a text field is typing");
        else if (g_twStepping)      pmHoldRelease("a tile step is in flight");
        else if (modDown)           pmHoldRelease("a modifier is held");
        else if (g_pmHoldIn < PI_COUNT && !g_pmDown[g_pmHoldIn]) pmHoldRelease("input released");
    }

    // ---- TIME-DRIVEN: armed holds past their threshold, and the one auto-repeat ----
    for (int i = 0; i < PI_COUNT; i++) {
        if (!g_pmArm[i].armed || g_pmArm[i].fired || !g_pmDown[i]) continue;
        if ((DWORD)(now - g_pmDownSince[i]) < PM_HOLD_MS) continue;
        g_pmArm[i].fired = true;
        char from[48]; _snprintf(from, sizeof from, "%s held", pmInName((uint8_t)i)); from[sizeof from - 1] = 0;
        pmFire(base, P, g_pmArm[i].holdFn, g_pmArm[i].holdSlot, (uint8_t)i, from);
    }
    if (g_pmRepFn >= 0) {
        if (g_pmRepIn >= PI_COUNT || !g_pmDown[g_pmRepIn]) pmRepeatEnd(base);
        else if ((long)(now - g_pmRepNext) >= 0) {
            axRoutePadKey(base, SDL_EVT_KEYDOWN, g_pmRepSym, g_pmRepMod, 1, false);
            g_pmRepNext = now + PM_NAV_REPEAT_MS;
        }
    }

    if (!fresh) return;

    // ---- THE EDGES. Releases first, so a direction change lets the old key go before the new ----
    for (int i = 0; i < PI_COUNT; i++) {
        if (!g_pmReleased[i]) continue;
        uint8_t in = (uint8_t)i;
        if (g_pmArm[i].armed) {
            if (!g_pmArm[i].fired && g_pmArm[i].pressFn >= 0) {
                char from[48]; _snprintf(from, sizeof from, "%s tap", pmInName(in)); from[sizeof from - 1] = 0;
                pmFire(base, P, g_pmArm[i].pressFn, g_pmArm[i].pressSlot, in, from);
            }
            g_pmArm[i].armed = false; g_pmArm[i].fired = false;
        }
        if (g_pmIsMod[i]) {
            if (!g_pmChorded[i] && (DWORD)(now - g_pmDownSince[i]) <= PM_TAP_MS) {
                int pf, ps, hf, hs;
                pmResolve(base, P, in, PI_NONE, &pf, &ps, &hf, &hs);
                char from[48]; _snprintf(from, sizeof from, "%s tap", pmInName(in)); from[sizeof from - 1] = 0;
                if (pf >= 0) pmFire(base, P, pf, ps, in, from);
                else if (in < 16 && (claim & (1u << in))) pmDrop(from, "no meaning here");
            }
            g_pmChorded[i] = false;
        }
        if (g_pmRepIn == in) pmRepeatEnd(base);
        if (g_pmHoldIn == in) pmHoldRelease("input released");
    }

    for (int i = 0; i < PI_COUNT; i++) {
        if (!g_pmPressed[i]) continue;
        uint8_t in = (uint8_t)i;
        bool claimed = (in < 16) ? (claim & (1u << in)) != 0 : claimRs;
        if (g_pmIsMod[i]) continue;
        uint8_t mod = PI_NONE;
        for (int m = 0; m < PI_COUNT; m++) if (g_pmIsMod[m] && g_pmDown[m] && m != i) { mod = (uint8_t)m; break; }
        if (mod != PI_NONE) g_pmChorded[mod] = true;
        int pf, ps, hf, hs;
        pmResolve(base, P, in, mod, &pf, &ps, &hf, &hs);
        char from[64];
        if (mod != PI_NONE) _snprintf(from, sizeof from, "%s+%s", pmInName(mod), pmInName(in));
        else                _snprintf(from, sizeof from, "%s", pmInName(in));
        from[sizeof from - 1] = 0;
        if (hf >= 0) {
            g_pmArm[i].armed = true; g_pmArm[i].fired = false;
            g_pmArm[i].pressFn = pf; g_pmArm[i].pressSlot = ps; g_pmArm[i].holdFn = hf; g_pmArm[i].holdSlot = hs;
            continue;
        }
        if (pf >= 0) { pmFire(base, P, pf, ps, in, from); continue; }
        if (claimed) pmDrop(from, "no meaning here");
        // else: unbound in scope -- the game has it
    }
}
