// raid/loot.cpp -- THE LOOT WINDOW

#include <cstdint>
#include <cstring>
#include <cstdio>
#include "internal.h"

// ---- The LOOT overlay (post-battle spoils / curio + chest treasure / camping) ----
static const uintptr_t LOOT_SYSTEM_OFF  = 0x5240;
static const uintptr_t LOOT_COUNT_OFF   = 0x5264;
static const int       LOOT_MAX_ITEMS   = 128;      // sanity cap on the live-item list

static const int64_t   LOOT_BTN_TAKE_ALL = 0x79657320;   // "yes " — button 1, lambda_1 -> +0x220
static const int64_t   LOOT_BTN_CLOSE    = 0x6e6f2020;   // "no  " — button 2, lambda_2 -> +0x221

// ---- Taking ONE item ----
static const uintptr_t LOOT_ROOT_OFF       = 0x5140;
static const uintptr_t SLOTIFACE_ITEM_OFF  = 0x30;
static const uintptr_t LOOT_POPUP_OFF   = 0x5238;   // raid root+: ptr -> popup descriptor (was 0x51c8)
static const uintptr_t LOOT_POPUP_TOKEN_OFF = 0x00; // descriptor+: ptr -> title token C-string
static const uintptr_t LOOT_POPUP_TYPE_OFF  = 0x08;

// ---- The LOOT overlay: reader, cursor and appearance watch ----
bool         g_lootActive = false;
bool                g_lootAside  = false;
static int          g_lootSlot   = -1;      // cursor item index; -1 = not placed yet
// The loot list, or false when there is no raid / no loot window.
static bool lootItemVector(uintptr_t base, uintptr_t* begOut, int* slotsOut) {
    uintptr_t root = 0;
    if (!safeReadPtr(base + MAP_ROOT_RVA, &root) || root <= 0x10000) return false;
    return invItemVectorAt(root + LOOT_SYSTEM_OFF, begOut, slotsOut);
}

static int lootLiveItems(uintptr_t base, int* slotOut, int maxOut) {
    uintptr_t beg = 0; int slots = 0;
    if (!lootItemVector(base, &beg, &slots)) return 0;

    int n = 0;
    for (int i = 0; i < slots && n < maxOut; i++) {
        uint32_t amount = 0;
        if (!safeReadU32(beg + (uintptr_t)i * ITEM_STRIDE + ITEM_AMOUNT_OFF, &amount)) continue;
        if ((int32_t)amount < 1) continue;             // the game's own empty test
        if (slotOut) slotOut[n] = i;
        n++;
    }
    return n;
}

static int lootItemCount(uintptr_t base) {
    return lootLiveItems(base, nullptr, LOOT_MAX_ITEMS);
}

static bool lootSlotText(uintptr_t base, int nth, char* out, int outsz) {
    uintptr_t root = 0;
    if (!safeReadPtr(base + MAP_ROOT_RVA, &root) || root <= 0x10000) return false;

    int slotIdx[LOOT_MAX_ITEMS];
    int live = lootLiveItems(base, slotIdx, LOOT_MAX_ITEMS);
    if (nth < 0 || nth >= live) return false;

    return invSystemSlotText(base, root + LOOT_SYSTEM_OFF, slotIdx[nth], axs(AXS_LOOT_NOUN_ITEM),
                             nth + 1, live, out, outsz);
}

static bool lootTakeItem(uintptr_t base, int nth) {
    uintptr_t root = 0;
    if (!safeReadPtr(base + MAP_ROOT_RVA, &root) || root <= 0x10000) return false;

    int slotIdx[LOOT_MAX_ITEMS];
    int live = lootLiveItems(base, slotIdx, LOOT_MAX_ITEMS);
    if (nth < 0 || nth >= live) return false;

    uintptr_t beg = 0; int pool = 0;
    if (!lootItemVector(base, &beg, &pool)) return false;
    uintptr_t item = beg + (uintptr_t)slotIdx[nth] * ITEM_STRIDE;

    uint8_t iface[0x40];
    memset(iface, 0, sizeof iface);
    *(uintptr_t*)(iface + SLOTIFACE_ITEM_OFF) = item;

    uint8_t ctx[0x40];
    memset(ctx, 0, sizeof ctx);
    *(int32_t*)  (ctx + 0x10) = nth;
    *(uintptr_t*)(ctx + 0x20) = root + LOOT_ROOT_OFF;
    *(uintptr_t*)(ctx + 0x28) = (uintptr_t)iface;

    typedef void (*LootTakeOneFn)(void*);
    LootTakeOneFn fn = (LootTakeOneFn)(base + LOOT_TAKE_ONE_RVA);
    logLine("loot: calling take-one worker for live item %d (pool slot %d, ItemStack=0x%llx)",
            nth + 1, slotIdx[nth], (unsigned long long)item);
    __try { fn(ctx); }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        logLine("loot: take-one worker FAULTED");
        return false;
    }
    return true;
}

static bool lootOpeningLine(uintptr_t base, char* out, int outsz) {
    int count = lootItemCount(base);
    if (count <= 0) return false;

    int at = (g_lootSlot >= 0 && g_lootSlot < count) ? g_lootSlot : 0;
    char item[MAILBOX_SZ] = { 0 };
    if (lootSlotText(base, at, item, sizeof item))
        _snprintf(out, outsz, axs(AXS_LOOT_HEAD_FMT), count, item);
    else
        _snprintf(out, outsz, axs(AXS_LOOT_HEAD_BARE_FMT), count);
    out[outsz - 1] = 0;
    return true;
}

void checkLootOverlay(uintptr_t base) {
    int count = lootItemCount(base);
    bool up = (count > 0);

    if (up == g_lootActive) return;                 // no change
    g_lootActive = up;
    g_lootAside = false;

    if (!up) {                                      // window closed: forget the cursor
        g_lootSlot = -1;
        logLine("loot: window closed");
        return;
    }

    g_lootSlot = 0;                                 // opens on the first item

    uintptr_t root = 0, desc = 0, tokp = 0;
    char token[128] = { 0 };
    int32_t type = -1;
    if (safeReadPtr(base + MAP_ROOT_RVA, &root) && root > 0x10000 &&
        safeReadPtr(root + LOOT_POPUP_OFF, &desc) && desc > 0x10000) {
        if (safeReadPtr(desc + LOOT_POPUP_TOKEN_OFF, &tokp) && tokp > 0x10000)
            safeReadCStr(tokp, token, sizeof token);
        safeReadU32(desc + LOOT_POPUP_TYPE_OFF, (uint32_t*)&type);
    }
    int egate = -1, esize = -1;
    readInputEnableStack(base, &egate, &esize);
    logLine("loot: window opened, %d items, token=\"%s\" type=%d [input stack %d]",
            count, token, type, esize);

    char utter[MAILBOX_SZ];
    if (lootOpeningLine(base, utter, sizeof utter)) postSpeech(utter);
}

void lootReannounce(uintptr_t base) {
    char utter[MAILBOX_SZ];
    if (!lootOpeningLine(base, utter, sizeof utter)) return;
    logLine("loot: re-announcing after a modal closed");
    postSpeech(utter);
}

void lootStepAside(const char* why) {
    if (!g_lootActive || g_lootAside) return;
    g_lootAside = true;
    logLine("loot: stepped aside for %s (window stays open; Escape returns)", why);
}

// ---- The context predicate (its kAxContexts row stays with the table) ----
bool axIsLoot()    { return g_lootActive && !g_lootAside; }

void lootReturn(uintptr_t base, const char* why) {
    if (g_rvActive) rvSetActive(base, false);
    if (g_abActive) abSetActive(base, false);
    if (g_qtActive) qtSetActive(base, false);
    if (g_tsActive) { tsSetActive(false); logLine("targeting: abandoned (back to the loot window)"); }
    iuAbandon("back to the loot window");
    g_lootAside = false;
    logLine("loot: back to the window via %s", why);
    lootReannounce(base);
}

// ---- The key handler ----
bool routeLootKey(uintptr_t base, uint32_t sym, uint16_t mod, uint8_t repeat) {
    if (mod & (KMOD_LALT | KMOD_RALT | KMOD_LCTRL | KMOD_RCTRL)) return false;

    int slots = lootItemCount(base);
    if (slots <= 0) return false;        // window is going away; let the game have the key

    if (sym == SDLK_ESCAPE) {
        if (repeat) return true;
        if (frontEndClickElementId(LOOT_BTN_CLOSE)) {
            logLine("loot: closing via the window's own close button 0x%llx",
                    (unsigned long long)LOOT_BTN_CLOSE);
        } else {
            logLine("loot: close button 0x%llx not on screen — letting the key through",
                    (unsigned long long)LOOT_BTN_CLOSE);
            return false;
        }
        return true;
    }

    if (sym == SDLK_RETURN || sym == SDLK_KP_ENTER) {
        if (repeat) return true;
        if (g_lootSlot < 0 || g_lootSlot >= slots) return true;

        // Name the item BEFORE taking it -- afterwards it may be gone entirely.
        uintptr_t root = 0, beg = 0; int pool = 0;
        char type[64] = {0}, itemId[64] = {0}, key[192] = {0}, name[256] = {0};
        int slotIdx[LOOT_MAX_ITEMS];
        int live = lootLiveItems(base, slotIdx, LOOT_MAX_ITEMS);
        if (safeReadPtr(base + MAP_ROOT_RVA, &root) && root > 0x10000 &&
            lootItemVector(base, &beg, &pool) && g_lootSlot < live)
            invItemName(base, beg + (uintptr_t)slotIdx[g_lootSlot] * ITEM_STRIDE,
                        type, itemId, key, name);

        if (!lootTakeItem(base, g_lootSlot)) {
            postSpeech(axs(AXS_LOOT_CANT_TAKE));
            logLine("loot: take item %d could not be attempted", g_lootSlot + 1);
            return true;
        }

        int after = lootItemCount(base);
        if (after >= slots) {
            logLine("loot: take item %d left the list at %d items — nothing moved",
                    g_lootSlot + 1, after);
            return true;
        }

        logLine("loot: took item %d (\"%s\"), %d -> %d items left", g_lootSlot + 1, name,
                slots, after);
        if (after <= 0) {
            postSpeech(name[0] ? name : axs(AXS_LOOT_TAKEN));   // window is closing; keep it short
            return true;
        }
        if (g_lootSlot >= after) g_lootSlot = after - 1;   // the list shrank under the cursor

        char now[MAILBOX_SZ];
        char utter[MAILBOX_SZ];
        if (lootSlotText(base, g_lootSlot, now, sizeof now))
            _snprintf(utter, sizeof utter, axs(AXS_LOOT_TOOK_FMT),
                      name[0] ? name : axs(AXS_LOOT_ITEM_FALLBACK), now);
        else
            _snprintf(utter, sizeof utter, axs(AXS_LOOT_TOOK_BARE_FMT),
                      name[0] ? name : axs(AXS_LOOT_ITEM_FALLBACK));
        utter[sizeof utter - 1] = 0;
        postSpeech(utter);
        return true;
    }

    {
        int jump = 0;
        if (axDecodeJump(sym, mod, repeat, &jump)) {
            if (!jump) return true;                    // held jump: one landing per press
            int jt = (jump > 0) ? slots - 1 : 0;
            char buf[MAILBOX_SZ];
            if (!lootSlotText(base, jt, buf, sizeof buf)) {
                logLine("lootnav: item %d did not read", jt);
                return true;
            }
            logLine("lootnav jump item %d -> %d (of %d)", g_lootSlot, jt, slots);
            g_lootSlot = jt;
            postSpeech(buf);
            return true;
        }
    }

    int dir;
    switch (sym) {
        case SDLK_UP:    case SDLK_LEFT:  dir = -1; break;
        case SDLK_DOWN:  case SDLK_RIGHT: dir = +1; break;
        default:         return false;   // not ours -> the game gets it (Space = take all)
    }
    if (axNavHoldRepeat(repeat)) return true;    // throttled repeat: claimed, no step

    if (g_lootSlot >= slots) g_lootSlot = slots - 1;   // the list shrank under the cursor

    int target;
    if (g_lootSlot < 0) {
        target = 0;
    } else {
        target = g_lootSlot + dir;
        if (target < 0 || target >= slots) target = g_lootSlot;   // hard stop: re-read this item
    }

    char buf[MAILBOX_SZ];
    if (!lootSlotText(base, target, buf, sizeof buf)) {
        logLine("lootnav: item %d did not read", target);
        return true;
    }
    logLine("lootnav dir=%+d item %d -> %d (of %d)", dir, g_lootSlot, target, slots);
    g_lootSlot = target;
    postSpeech(buf);
    return true;
}
