// raid/resting.cpp -- the FIRST RAID slice

#include <cstdint>
#include <cstring>
#include <cstdio>
#include "internal.h"
// ---- THE RESTING POINT ----

bool axCoversRestingPoint() {
    return g_lootActive || g_evActive || g_qcOpen || g_csOpen || g_clogOpen || g_jpActive ||
           g_campCovers;
}

static bool      g_restPending = false;   // a landing on the resting point is owed
static bool      g_restCovered = false;   // something was on top as of last frame
static bool      g_restCombat  = false;   // a battle was live as of last frame
static bool      g_restSwitch  = false;
static uintptr_t g_restRoot    = 0;       // which raid we are tracking

static bool restLandActionBar(uintptr_t base) {
    ActionItem items[AB_MAX_ITEMS];
    int n = abBuildBar(base, items, AB_MAX_ITEMS);
    if (n <= 0) return false;
    if (g_rvActive) rvSetActive(base, false);
    if (g_qtActive) qtSetActive(base, false);
    abSetActive(base, true);
    abSpeakLabel(base, &items[g_abCursor], abSlotCount(items, n), "Actions.");
    return true;
}

void checkRestingPoint(uintptr_t base) {
    uintptr_t root = mapRoot(base);
    if (!root) {
        g_restRoot = 0; g_restCovered = false; g_restPending = false; g_restSwitch = false;
        return;
    }

    uint32_t combatState = 0;                 // a state ENUM, nonzero while a battle is up
    safeReadU32(root + RAID_IN_COMBAT_OFF, &combatState);
    bool combat = combatState != 0;

    if (root != g_restRoot) {                 // a raid just began (or we changed raids)
        g_restRoot    = root;
        g_restCovered = axCoversRestingPoint();
        g_restCombat  = combat;
        g_restPending = true;
        g_restSwitch  = false;
        logLine("resting point: raid root=%p — a landing is owed (combat=%d)",
                (void*)root, combat ? 1 : 0);
    }

    if (combat != g_restCombat) {
        g_restCombat = combat;
        bool inOldRoot = combat ? g_rvActive : g_abActive;
        bool elsewhere = g_qtActive || (combat ? g_abActive : g_rvActive) ||
                         (!g_rvActive && !g_abActive && (g_mapReview || g_invActive));
        if (inOldRoot || !elsewhere) {
            g_restPending = true;
            g_restSwitch  = inOldRoot;
            logLine("resting point: combat %s — %s", combat ? "started" : "ended",
                    inOldRoot ? "the root moves under the player" : "a landing is owed");
        }
    }

    bool covered = axCoversRestingPoint();
    if (covered != g_restCovered) {
        g_restCovered = covered;
        if (!covered) {
            g_restPending = true;
            logLine("resting point: the covering layer closed — a landing is owed");
        }
    }
    if (!g_restPending) return;

    if (covered || axIsLoading() || axIsNaming() || axIsPause() || axIsDialog()) return;
    if (g_tsActive || g_iuActive) return;

    bool wantBar = g_restCombat;
    bool inDest  = wantBar ? g_abActive : g_rvActive;
    bool inOther = wantBar ? g_rvActive : g_abActive;
    if (inDest || g_qtActive || (inOther && !g_restSwitch)) {
        g_restPending = false; g_restSwitch = false;
        logLine("resting point: the player is already in a mod layer — debt cancelled");
        return;
    }

    if (wantBar) {
        // The bar is not readable yet (combat's first frames). KEEP the debt.
        if (!restLandActionBar(base)) return;
        g_restPending = false; g_restSwitch = false;
        logLine("resting point: landing in the action bar (combat)");
        return;
    }

    uintptr_t party[RV_MAX_MEMBERS];
    if (rvPartyList(base, party, RV_MAX_MEMBERS) <= 0) return;

    g_restPending = false; g_restSwitch = false;
    if (g_abActive) abSetActive(base, false);   // a root switch out of combat leaves the bar
    rvSetActive(base, true);
    logLine("resting point: landing in the dungeon view");
    rvAnnounceEntry(base, true);
}
