// town/barks.cpp -- the tenth TOWN slice

#include <cstdint>
#include <cstring>
#include <cstdio>
#include "internal.h"
// ---- TOWN BARKS — the hamlet's live text ----
static const uintptr_t TBK_TEXT_OFF    = 0x000;  // bark+: char[0x80], already localized
static const int       TBK_TEXT_MAX    = 0x80;
static const uintptr_t TBK_ELAPSED_OFF = 0x128;  // bark+: float; < 0 while staggered   (was 0x130)
static const uintptr_t TBK_DUR_OFF     = 0x12c;  // bark+: float, the balloon's lifetime (was 0x134)
static const uintptr_t TBK_ACTIVE_OFF  = 0x130;  // bark+: u8, a bark is assigned        (was 0x138)
static const uintptr_t TBK_STARTED_OFF = 0x131;  // bark+: u8, the tick has drawn it     (was 0x139)

static const uintptr_t TBK_IN_ROSTERELEM = 0x358;  // RosterElement+: the component (was 0x360)
static const uintptr_t TBK_IN_HEROSLOT   = 0x1b0;  // HeroSlot+:      the component

static const int TBK_MAX_SITES = 48;

struct TbkSite {
    uintptr_t bark;      // the component
    uintptr_t hero;
    uint32_t  guid;
    const char* where;   // log only
};

static int tbkCollect(uintptr_t base, uintptr_t root, TbkSite* out, int max) {
    int n = 0;

    {
        uintptr_t rl = root + PTY_ROSTERLIST_OFF, beg = 0, end = 0;
        if (safeReadPtr(rl + PTY_RL_ROWS_BEG_OFF, &beg) &&
            safeReadPtr(rl + PTY_RL_ROWS_END_OFF, &end) && beg && end > beg) {
            int rows = (int)((end - beg) / 0x40);
            if (rows > ROS_MAX_ROWS) rows = ROS_MAX_ROWS;
            for (int i = 0; i < rows && n < max; i++) {
                uintptr_t re = 0, entry = 0;
                if (!safeReadPtr(beg + (uintptr_t)i * 0x40, &re) || re <= 0x10000) continue;
                safeReadPtr(re + PTY_RL_ROW_ENTRY_OFF, &entry);
                out[n].bark  = re + TBK_IN_ROSTERELEM;
                out[n].hero  = (entry > 0x10000) ? entry + PTY_ENTRY_HERO_OFF : 0;
                out[n].guid  = 0;
                out[n].where = "roster";
                n++;
            }
        }
    }

    {
        uintptr_t beg = 0, end = 0;
        if (safeReadPtr(root + PTY_SLOTS_BEG_OFF, &beg) &&
            safeReadPtr(root + PTY_SLOTS_END_OFF, &end) && beg && end > beg && (end - beg) % 8 == 0) {
            int slots = (int)((end - beg) / 8);
            if (slots > 8) slots = 8;
            for (int i = 0; i < slots && n < max; i++) {
                uintptr_t slotW = 0, iface = 0, hero = 0;
                if (!safeReadPtr(beg + (uintptr_t)i * 8, &slotW) || slotW <= 0x10000) continue;
                if (safeReadPtr(slotW + PTY_SLOT_IFACE_OFF, &iface) && iface > 0x10000)
                    safeReadPtr(iface + BLD_RCT_IFACE_HERO, &hero);
                out[n].bark  = slotW + TBK_IN_HEROSLOT;
                out[n].hero  = hero > 0x10000 ? hero : 0;
                out[n].guid  = 0;
                out[n].where = "lineup";
                n++;
            }
        }
    }

    uintptr_t panel = bldOpenPanel(root);
    if (panel) {
        uintptr_t items[16];
        int in = bldItemDisplays(panel, items, 16);
        uintptr_t disps[8];
        int dn = 0;
        for (int i = 0; i < in && dn < 8; i++)
            if (bldRecruitKindOf(base, items[i]) != BLD_RCT_NONE) disps[dn++] = items[i];
        if (dn == 0) {
            static const uintptr_t kRecruitVfts[] = { BLD_BHRD_VFT_RVA, BLD_HRD_VFT_RVA,
                                                      BLD_SHRD_VFT_RVA };
            uintptr_t disp = bldChildByVft(base, panel, kRecruitVfts, 3);
            if (disp) disps[dn++] = disp;
        }
        for (int d = 0; d < dn && n < max; d++) {
            uintptr_t disp = disps[d], sb = 0, se = 0;
            if (!safeReadPtr(disp + BLD_RCT_SLOTS_BEG, &sb) ||
                !safeReadPtr(disp + BLD_RCT_SLOTS_END, &se) || !sb || se <= sb ||
                (se - sb) % BLD_RCT_SLOT_STRIDE) continue;
            int slots = (int)((se - sb) / BLD_RCT_SLOT_STRIDE);
            if (slots > 16) slots = 16;
            for (int i = 0; i < slots && n < max; i++) {
                uintptr_t widget = 0, iface = 0, hero = 0;
                if (!safeReadPtr(sb + (uintptr_t)i * BLD_RCT_SLOT_STRIDE, &widget) ||
                    widget <= 0x10000) continue;
                if (safeReadPtr(widget + BLD_RCT_IFACE_OFF, &iface) && iface > 0x10000)
                    safeReadPtr(iface + BLD_RCT_IFACE_HERO, &hero);
                out[n].bark  = widget + TBK_IN_HEROSLOT;
                out[n].hero  = hero > 0x10000 ? hero : 0;
                out[n].guid  = 0;
                out[n].where = "recruit";
                n++;
            }
        }
        BldActRow rows[48];
        int an = bldActRows(base, panel, rows, 48);
        for (int i = 0; i < an && n < max; i++) {
            if (rows[i].trtDrop >= 0) continue;              // a dropdown, not a slot of its own
            if (rows[i].slotw <= 0x10000) continue;
            uintptr_t bark = rows[i].slotw + TBK_IN_HEROSLOT;
            bool dup = false;
            for (int j = 0; j < n; j++) if (out[j].bark == bark) { dup = true; break; }
            if (dup) continue;
            out[n].bark  = bark;
            out[n].hero  = rows[i].pendingHero;
            out[n].guid  = rows[i].pendingHero ? 0 : rows[i].committedGuid;
            out[n].where = "activity";
            n++;
        }
    }
    return n;
}

struct TbkSeen { uintptr_t bark; uint8_t shown; uint8_t active; };
static TbkSeen g_tbkSeen[TBK_MAX_SITES];
static int     g_tbkSeenN = 0;
static int     g_tbkProbedN = -1;

static void tbkReset(const char* why) {
    if (g_tbkSeenN) logLine("townbark: forgetting %d speaker(s) (%s)", g_tbkSeenN, why);
    g_tbkSeenN   = 0;
    g_tbkProbedN = -1;
}

static float tbkFloat(uintptr_t addr) {
    uint32_t raw = 0;
    return safeReadU32(addr, &raw) ? u32AsFloatM(raw) : 0.0f;
}

void serviceTownBark(uintptr_t base) {
    if (!axReadTownBarks()) { tbkReset("barks switched off"); return; }

    uintptr_t root = resTownRoot(base);
    if (!root) { tbkReset("left town"); return; }

    TbkSite sites[TBK_MAX_SITES];
    int n = tbkCollect(base, root, sites, TBK_MAX_SITES);

    if (n != g_tbkProbedN && n > 0) {
        g_tbkProbedN = n;
        logLine("townbark probe: watching %d speaker(s)", n);
        for (int i = 0; i < n; i++) {
            uint8_t a = 0, s = 0;
            char who[80] = {0};
            safeReadU8(sites[i].bark + TBK_ACTIVE_OFF, &a);
            safeReadU8(sites[i].bark + TBK_STARTED_OFF, &s);
            float el = tbkFloat(sites[i].bark + TBK_ELAPSED_OFF);
            float du = tbkFloat(sites[i].bark + TBK_DUR_OFF);
            uintptr_t h = sites[i].hero;
            if (!h && sites[i].guid) h = bldActHeroByGuid(base, sites[i].guid);
            if (h) safeReadCStr(h + HERO_NAME_OFF, who, sizeof who);
            logLine("townbark probe:   %-8s bark=%p hero=\"%s\" active=%u started=%u elapsed=%.2f dur=%.2f",
                    sites[i].where, (void*)sites[i].bark, who, a, s, el, du);
        }
    }

    TbkSeen next[TBK_MAX_SITES];
    int nn = 0;

    for (int i = 0; i < n && nn < TBK_MAX_SITES; i++) {
        uint8_t active = 0, started = 0;
        if (!safeReadU8(sites[i].bark + TBK_ACTIVE_OFF, &active)) continue;
        if (!safeReadU8(sites[i].bark + TBK_STARTED_OFF, &started)) continue;
        uint8_t shown = (active && started) ? 1 : 0;

        int prev = -1;
        for (int j = 0; j < g_tbkSeenN; j++) if (g_tbkSeen[j].bark == sites[i].bark) { prev = j; break; }

        next[nn].bark   = sites[i].bark;
        next[nn].shown  = shown;
        next[nn].active = active ? 1 : 0;
        nn++;

        if (prev < 0) continue;                       // first sighting: record, never speak

        if (active && !g_tbkSeen[prev].active) {
            float el = tbkFloat(sites[i].bark + TBK_ELAPSED_OFF);
            logLine("townbark: %s assigned a bark (elapsed=%.2f, %s)", sites[i].where, el,
                    started ? "already started" : el < 0 ? "waiting its turn" : "starts next tick");
        }

        if (!shown || g_tbkSeen[prev].shown) continue; // no rising edge on "it is on screen"

        char raw[TBK_TEXT_MAX + 1];
        if (!safeReadCStr(sites[i].bark + TBK_TEXT_OFF, raw, sizeof raw) || !raw[0]) {
            logLine("townbark: %s balloon is up with an empty text field", sites[i].where);
            continue;
        }
        char flat[TBK_TEXT_MAX + 1];
        int o = 0;
        for (int k = 0; raw[k] && o < (int)sizeof flat - 2; k++) {
            if (raw[k] == '\n' || raw[k] == '\r') { if (o > 0 && flat[o - 1] != ' ') flat[o++] = ' '; }
            else flat[o++] = raw[k];
        }
        flat[o] = 0;
        char text[CLOG_LINE_MAX];
        stripMarkup(flat, text, sizeof text);
        if (!text[0]) continue;

        uintptr_t hero = sites[i].hero;
        if (!hero && sites[i].guid) hero = bldActHeroByGuid(base, sites[i].guid);
        char who[80] = {0};
        if (hero) safeReadCStr(hero + HERO_NAME_OFF, who, sizeof who);

        char line[MAILBOX_SZ];
        if (who[0]) {
            _snprintf(line, sizeof line, "%s: %s", who, text);
            line[sizeof line - 1] = 0;
        } else {
            strncpy(line, text, sizeof line - 1);
            line[sizeof line - 1] = 0;
        }

        logLine("townbark: %s bark -> \"%s\"", sites[i].where, line);
        if (sites[i].guid) {
            char follow[MAILBOX_SZ];
            if (bldActTakeCommitFollowup(sites[i].guid, follow, sizeof follow)) {
                char both[MAILBOX_SZ];
                char lastc = line[0] ? line[strlen(line) - 1] : 0;
                bool punct = lastc == '.' || lastc == '!' || lastc == '?' || lastc == '"';
                _snprintf(both, sizeof both, punct ? "%s %s" : "%s. %s", line, follow);
                both[sizeof both - 1] = 0;
                logLine("townbark: bark carries the parked commit announcement");
                postSpeech(both);
                continue;
            }
        }
        postSpeech(line, false, SPK_CHATTER);
    }

    for (int i = 0; i < nn; i++) g_tbkSeen[i] = next[i];
    g_tbkSeenN = nn;
}
