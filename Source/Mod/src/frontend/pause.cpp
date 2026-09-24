// frontend/pause.cpp -- THE IN-GAME PAUSE MENU

#include <cstdint>
#include <cstring>
#include <cstdio>
#include "internal.h"
// ---- In-game pause / game menu (Esc during town or dungeon) ----
static const PauseLabel kPauseLabels[] = {
    { 0x6f656c, "menu_base_element_return_to_game", AXS_PM_RETURN_TO_GAME },       // "leo"
    { 0x6f656d, "menu_base_element_view_help",      AXS_PM_HELP },                 // "meo"
    { 0x6f656e, "menu_base_element_view_controls",  AXS_PM_CONTROLS },             // "neo"
    { 0x6f656f, "menu_base_element_view_glossary",  AXS_PM_GLOSSARY },             // "oeo"
    { 0x6f6570, "menu_base_element_view_options",   AXS_PM_OPTIONS },              // "peo"
    { 0x6f6571, "menu_base_element_view_credits",   AXS_PM_CREDITS },              // "qeo"
    { 0x6f6572, "menu_base_element_watch_intro",    AXS_PM_WATCH_INTRO },// "reo"
    { 0x6f6573, "menu_base_element_exit_campaign",  AXS_PM_EXIT_CAMPAIGN },    // "seo"
    { 0x6f6574, "menu_base_element_exit_game",      AXS_PM_EXIT_GAME },      // "teo"
};
const PauseLabel* lookupPause(uint32_t code) {
    for (const auto& e : kPauseLabels) if (e.code == code) return &e;
    return nullptr;
}

volatile bool        g_pauseOpen = false;
bool                 g_axOptionsUp = false;
static uintptr_t     g_pauseHeldDlg = 0;
static uintptr_t     g_pauseDlgUnderMenu = 0;
void checkPauseOpen(uintptr_t base) {
    uintptr_t begin = 0, end = 0;
    bool pauseSeen = false, optSeen = false;
    if (safeReadPtr(base + VEC_BEGIN_RVA, &begin) &&
        safeReadPtr(base + VEC_END_RVA, &end) && begin && end > begin) {
        uintptr_t count = (end - begin) / ELEM_STRIDE;
        if (count > 4096) count = 4096;
        for (uintptr_t i = 0; i < count && !(pauseSeen && optSeen); i++) {
            int64_t eid = 0;
            if (!safeReadI64(begin + i * ELEM_STRIDE + ELEM_ID_OFF, &eid)) continue;
            if (eid < 0) continue;
            uint32_t cc = (uint32_t)(uint64_t)eid;
            if (!pauseSeen && lookupPause(cc) != nullptr) {
                pauseSeen = true;
                if (!g_pauseOpen) {
                    logLine("pausemenu: OPEN (element 0x%llx present)",
                            (unsigned long long)eid);
                }
                g_pauseOpen = true;
            }
            if (!optSeen && isOptionId(cc)) optSeen = true;
        }
    }
    bool optNow = optSeen && frontEndDisplay(base) == 0;
    if (optNow != g_axOptionsUp)
        logLine("pausemenu: in-game options screen %s", optNow ? "OPEN" : "closed");
    g_axOptionsUp = optNow;

    if (pauseSeen) {
        g_pauseHeldDlg = 0;                          // menu up: nothing is being stood aside for
        g_pauseDlgUnderMenu = confirmDialogEntry(base);
        return;
    }
    if (g_pauseOpen) logLine("pausemenu: closed");
    g_pauseOpen = false;

    if (g_inPauseMenu) {
        uintptr_t dlg = confirmDialogEntry(base);
        if (!dlg) {
            logLine("pausemenu: clearing g_inPauseMenu (menu closed, no dialog standing in for it)");
            g_inPauseMenu = false;
            g_pauseHeldDlg = 0;
        } else if (!g_pauseHeldDlg) {
            if (dlg == g_pauseDlgUnderMenu) {
                logLine("pausemenu: dialog 0x%llx predates the menu's close -- it is the game's, "
                        "clearing g_inPauseMenu so AX_DIALOG can answer it",
                        (unsigned long long)dlg);
                g_inPauseMenu = false;
            } else {
                g_pauseHeldDlg = dlg;
                logLine("pausemenu: menu closed with dialog 0x%llx standing -- holding "
                        "g_inPauseMenu for that dialog only", (unsigned long long)dlg);
            }
        } else if (dlg != g_pauseHeldDlg) {
            logLine("pausemenu: dialog 0x%llx is not the one the menu left behind (0x%llx) -- "
                    "clearing g_inPauseMenu", (unsigned long long)dlg,
                    (unsigned long long)g_pauseHeldDlg);
            g_inPauseMenu = false;
            g_pauseHeldDlg = 0;
        }
    } else {
        g_pauseHeldDlg = 0;
    }
}
