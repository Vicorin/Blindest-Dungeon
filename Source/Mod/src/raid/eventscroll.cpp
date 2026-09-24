// raid/eventscroll.cpp -- THE EVENT SCROLL

#include <cstdint>
#include <cstring>
#include <cstdio>
#include "internal.h"

// ---- The EVENT overlay: the "scroll" with a list of CHOICES ----

static const uintptr_t OE_TITLE_OFF     = 0x2a0;   // char[0x200] localized title   (curio skin)
static const uintptr_t OE_CONTENT_OFF   = 0x4a0;   // char[0x200] localized flavour (curio skin)
static const uintptr_t OE_QUESTITEM_OFF = 0xd6a;

static const uintptr_t PT_ITEMSLOT_FLAG = 0x64b;   // the drag area row is drawn
static const uintptr_t PT_INVESTIGATE_FLAG = 0x64c;// the investigate row is drawn
static const uintptr_t PT_PASS_FLAG     = 0x64d;   // the ignore row is drawn
static const uintptr_t PT_ANCESTOR_FLAG = 0x648;   // door prop: 1 => the ancestor skin

// ---- Activating a choice ----

static const uintptr_t OE_EAT_COST_CAP_OFF = 0x10;     // capture+: float, the food cost
static const uintptr_t ACTOR_HUNGER_OFF    = 0xbc4;    // Actor+: float, this hero's food need (was 0xb64)
static const uintptr_t RAID_HEROES_BEG_OFF = 0x08;     // raid root+: vector<Actor*> begin
static const uintptr_t RAID_HEROES_END_OFF = 0x10;     // raid root+: vector<Actor*> end

// ---- The rows ARE focus elements after all ----
static const int64_t   OE_BTN_PRIMARY   = 0x79657320;  // "yes " — investigate / clear / speak / eat
static const int64_t   OE_BTN_SECONDARY = 0x6e6f2020;  // "no  " — ignore / starve

// ---- Using an ITEM on the curio ----

// ---- The REAL drag-onto-the-curio path ----
static const uintptr_t OE_SLOT_ITEM_OFF = 0x7c0;   // Overlay_Event+: the parked ItemStack copy
static const uintptr_t OE_SLOT_FLAG_OFF = 0xd68;

// ---- Announcing what a sighted player can see ----
static const char* const OE_NO_EFFECT_KEY = "str_curio_item_had_no_effect";

// ---- Using an item on an OBSTACLE (rubble, locked door/chest, sack) ----
static const size_t    OE_OBSTACLE_CTX_BYTES      = 0x8 + 0x5a8; // {oe*, ItemStack copy}
static const uintptr_t OE_CTX_OFF             = 0xd78;
static const uintptr_t OE_ANIM_OFF            = 0xdb0;
static const uintptr_t OE_PREVSTATE_OFF       = 0xd8c;
static const uintptr_t PROP_CLEAR_ITEM_VOFF   = 0x30;     // DoorProp vftable[6]: clear-with-item virtual

static const int OE_MAX_ROWS = 4;

// ---- The EVENT overlay: reader, cursor and appearance watch ----
enum EvSkin { EV_NONE = 0, EV_CURIO, EV_OBSTACLE, EV_ANCESTOR, EV_HUNGER };

struct EvRow {
    char      name[128];
    char      desc[512];      // the sentence under it: "Search the tent..."
    uintptr_t actionRva;
    int64_t   focusId;
    bool      isItemSlot;
    float     capFloat;       // extra capture value; only hunger's Eat uses it
    bool      enabled;        // false = the game draws this row greyed out
};

bool g_evActive = false;
static int  g_evRow    = 0;      // cursor row
static int  g_evSkin   = EV_NONE;
static bool g_evPickItem = false;

// The live Overlay_Event, or 0 when there is no raid HUD.
static uintptr_t evOverlay() {
    uintptr_t rd = (uintptr_t)g_raidDisplay;
    if (rd <= 0x10000) return 0;
    return rd + RD_EVENT_OFF;
}

static int32_t evState() {
    uintptr_t oe = evOverlay();
    uint32_t st = 0;
    if (!oe || !safeReadU32(oe + OE_STATE_OFF, &st)) return 0;
    return (int32_t)st;
}

static int evSkinNow(uintptr_t base, uintptr_t* propOut, uintptr_t* propTypeOut) {
    if (propOut) *propOut = 0;
    if (propTypeOut) *propTypeOut = 0;
    uintptr_t oe = evOverlay();
    if (!oe) return EV_NONE;

    uintptr_t prop = 0, pt = 0;
    if (safeReadPtr(oe + OE_CURIO_PROP_OFF, &prop) && prop > 0x10000) {
        safeReadPtr(prop + PROP_TYPE_OFF, &pt);
        if (propOut) *propOut = prop;
        if (propTypeOut) *propTypeOut = pt;
        return EV_CURIO;
    }
    if (!safeReadPtr(oe + OE_OTHER_PROP_OFF, &prop) || prop <= 0x10000)
        return EV_HUNGER;                      // both prop slots empty: the hunger prompt

    uint32_t ptype = 0;
    if (!safeReadU32(prop + PROP_TYPE_ENUM_OFF, &ptype)) return EV_NONE;
    if (ptype != 3) return EV_NONE;            // 2 = trap: the game's own disarm flow, not ours
    if (!safeReadPtr(prop + PROP_TYPE_OFF, &pt) || pt <= 0x10000) return EV_NONE;
    if (propOut) *propOut = prop;
    if (propTypeOut) *propTypeOut = pt;

    uint8_t anc = 0;
    safeReadU8(pt + PT_ANCESTOR_FLAG, &anc);
    return anc == 1 ? EV_ANCESTOR : EV_OBSTACLE;
}

static bool evPropString(uintptr_t propType, uintptr_t off, char* out, int outsz) {
    out[0] = 0;
    if (!propType) return false;
    return safeReadCStr(propType + off, out, outsz) && out[0];
}

static void evHeadFor(uintptr_t base, int skin, uintptr_t prop, uintptr_t propType,
                      char* title, int titlesz, char* flavour, int flavoursz) {
    title[0] = 0; flavour[0] = 0;
    char raw[600];

    if (skin == EV_CURIO) {
        uintptr_t oe = evOverlay();
        if (!oe) return;
        if (safeReadCStr(oe + OE_TITLE_OFF, raw, sizeof raw))   stripMarkup(raw, title, titlesz);
        if (safeReadCStr(oe + OE_CONTENT_OFF, raw, sizeof raw)) stripMarkup(raw, flavour, flavoursz);
        return;
    }

    if (skin == EV_HUNGER) {
        if (resolveKey(base, "str_ui_hunger_title", raw, sizeof raw))   stripMarkup(raw, title, titlesz);
        if (resolveKey(base, "str_ui_hunger_content", raw, sizeof raw)) stripMarkup(raw, flavour, flavoursz);
        return;
    }

    char id[96] = { 0 };
    if (!safeReadCStr(prop + PROP_INLINE_NAME_OFF, id, sizeof id) || !id[0]) return;
    char key[192];
    _snprintf(key, sizeof key, "str_obstacle_%s_title", id);       key[sizeof key - 1] = 0;
    if (resolveKey(base, key, raw, sizeof raw)) stripMarkup(raw, title, titlesz);
    _snprintf(key, sizeof key, "str_obstacle_%s_description", id); key[sizeof key - 1] = 0;
    if (resolveKey(base, key, raw, sizeof raw)) stripMarkup(raw, flavour, flavoursz);

    if (!title[0]) {
        propPrettyName(id, title, titlesz);
        logLine("event: no str_obstacle_%s_title in this language -> prettified \"%s\"", id, title);
    }
}

static EvRow* evAddRow(uintptr_t base, EvRow* rows, int* n, const char* nameKey,
                       const char* descKey, uintptr_t actionRva, int64_t focusId,
                       float capFloat) {
    if (*n >= OE_MAX_ROWS) return nullptr;
    EvRow* r = &rows[*n];
    memset(r, 0, sizeof *r);
    r->actionRva = actionRva;
    r->focusId   = focusId;
    r->capFloat  = capFloat;
    r->enabled   = true;      // the default; only hunger's Eat ever clears it

    char raw[600];
    if (nameKey && resolveKey(base, nameKey, raw, sizeof raw)) stripMarkup(raw, r->name, sizeof r->name);
    if (descKey && resolveKey(base, descKey, raw, sizeof raw)) stripMarkup(raw, r->desc, sizeof r->desc);
    (*n)++;
    return r;
}

static float evFoodCost(uintptr_t base) {
    uintptr_t root = 0, beg = 0, end = 0;
    if (!safeReadPtr(base + MAP_ROOT_RVA, &root) || root <= 0x10000) return 0.0f;
    if (!safeReadPtr(root + RAID_HEROES_BEG_OFF, &beg)) return 0.0f;
    if (!safeReadPtr(root + RAID_HEROES_END_OFF, &end)) return 0.0f;
    if (beg == 0 || end < beg || (end - beg) > 0x200) return 0.0f;

    uint32_t kbits = 0;
    if (!safeReadU32(base + HUNGER_ROUND_RVA, &kbits)) return 0.0f;
    float k = 0.0f; memcpy(&k, &kbits, sizeof k);

    float total = 0.0f;
    for (uintptr_t p = beg; p + 8 <= end; p += 8) {
        uintptr_t actor = 0;
        if (!safeReadPtr(p, &actor) || actor <= 0x10000) continue;
        uint32_t bits = 0;
        if (!safeReadU32(actor + ACTOR_HUNGER_OFF, &bits)) continue;
        float need = 0.0f; memcpy(&need, &bits, sizeof need);
        need += k;
        if (need > 0.0f) total += need;
    }
    return total;
}

static int evFoodHeld(uintptr_t base) {
    uintptr_t beg = 0; int slots = 0;
    if (!invItemVector(base, &beg, &slots)) return 0;
    uint32_t foodHash = 0;
    if (!safeReadU32(base + FOOD_TYPEHASH_RVA, &foodHash)) return 0;

    int held = 0;
    for (int i = 0; i < slots; i++) {
        uintptr_t item = beg + (uintptr_t)i * ITEM_STRIDE;
        uint32_t amount = 0, hash = 0;
        if (!safeReadU32(item + ITEM_AMOUNT_OFF, &amount)) continue;
        if ((int32_t)amount < 1) continue;
        if (!safeReadU32(item + ITEM_TYPEHASH_OFF, &hash)) continue;
        if (hash == foodHash) held += (int)amount;
    }
    return held;
}

static int evBuildRows(uintptr_t base, EvRow* rows, int* skinOut,
                       char* title, int titlesz, char* flavour, int flavoursz) {
    title[0] = 0; flavour[0] = 0;
    uintptr_t prop = 0, pt = 0;
    int skin = evSkinNow(base, &prop, &pt);
    if (skinOut) *skinOut = skin;
    if (skin == EV_NONE) return 0;

    evHeadFor(base, skin, prop, pt, title, titlesz, flavour, flavoursz);

    int n = 0;
    if (skin == EV_CURIO) {
        char cat[64] = { 0 }, ui[96] = { 0 };
        evPropString(pt, PT_CATEGORY_OFF, cat, sizeof cat);
        evPropString(pt, PT_UINAME_OFF,   ui,  sizeof ui);

        uint8_t investigate = 0, itemslot = 0, pass = 0, hasQuestItem = 0;
        safeReadU8(pt + PT_INVESTIGATE_FLAG, &investigate);
        safeReadU8(pt + PT_ITEMSLOT_FLAG,    &itemslot);
        safeReadU8(pt + PT_PASS_FLAG,        &pass);
        safeReadU8(evOverlay() + OE_QUESTITEM_OFF, &hasQuestItem);

        if (investigate) {
            char key[256] = { 0 };
            if (cat[0] && ui[0])
                _snprintf(key, sizeof key, "str_%s_tooltip_investigate_%s%s", cat, ui,
                          hasQuestItem ? "_with_quest_item" : "");
            key[sizeof key - 1] = 0;
            evAddRow(base, rows, &n, "str_curio_tooltip_investigate_controller",
                     key[0] ? key : nullptr, OE_ACT_INVESTIGATE, OE_BTN_PRIMARY, 0.0f);
        }
        if (itemslot) {
            EvRow* r = evAddRow(base, rows, &n, nullptr,
                                hasQuestItem ? "curio_tooltip_quest_item_slot"
                                             : "curio_tooltip_item_slot",
                                0, 0, 0.0f);
            if (r) {
                r->isItemSlot = true;
                if (!r->name[0]) {
                    _snprintf(r->name, sizeof r->name, "%s", axs(AXS_EV_USE_AN_ITEM));
                    r->name[sizeof r->name - 1] = 0;
                }
            }
        }
        if (pass)
            evAddRow(base, rows, &n, "curio_tooltip_pass_controller", nullptr,
                     OE_ACT_CURIO_PASS, OE_BTN_SECONDARY, 0.0f);
        return n;
    }

    if (skin == EV_OBSTACLE) {
        evAddRow(base, rows, &n, "obstacle_tooltip_clear_by_hand_controller",
                 "obstacle_tooltip_clear_by_hand", OE_ACT_CLEAR, OE_BTN_PRIMARY, 0.0f);
        EvRow* slot = evAddRow(base, rows, &n, nullptr, nullptr, 0, 0, 0.0f);
        if (slot) {
            slot->isItemSlot = true;
            _snprintf(slot->name, sizeof slot->name, "%s", axs(AXS_EV_USE_AN_ITEM));
            slot->name[sizeof slot->name - 1] = 0;
        }
        evAddRow(base, rows, &n, "curio_tooltip_pass_controller", nullptr,
                 OE_ACT_OBST_PASS, OE_BTN_SECONDARY, 0.0f);
        return n;
    }

    if (skin == EV_ANCESTOR) {
        // ShowAncestorObstacle builds exactly one row — there is no way to decline it.
        evAddRow(base, rows, &n, "obstacle_tooltip_ancestor_talk_controller",
                 "obstacle_tooltip_ancestor_talk", OE_ACT_ANCESTOR, OE_BTN_PRIMARY, 0.0f);
        return n;
    }

    {
        float cost = evFoodCost(base);
        int   held = evFoodHeld(base);
        EvRow* eat = evAddRow(base, rows, &n, "str_ui_hunger_choice_eat_controller", nullptr,
                              OE_ACT_EAT, OE_BTN_PRIMARY, cost);
        if (eat) {
            eat->enabled = (cost <= (float)held);
            _snprintf(eat->desc, sizeof eat->desc, axs(AXS_EV_EAT_COST_FMT),
                      (int)cost, held);
            eat->desc[sizeof eat->desc - 1] = 0;
        }
        EvRow* starve = evAddRow(base, rows, &n, "str_ui_hunger_choice_starve_controller",
                                 nullptr, OE_ACT_STARVE, OE_BTN_SECONDARY, 0.0f);
        if (starve) {
            _snprintf(starve->desc, sizeof starve->desc, "%s", axs(AXS_EV_STARVE_DESC));
            starve->desc[sizeof starve->desc - 1] = 0;
        }
    }
    return n;
}

// "Investigate. Search the tent..." — one row, with its position in the list.
static void evRowLine(const EvRow* r, int idx, int total, char* out, int outsz) {
    const char* name = r->name[0] ? r->name : axs(AXS_EV_CHOICE_FALLBACK);
    char pos[48];
    _snprintf(pos, sizeof pos, axs(AXS_POS_N_OF_M), idx + 1, total);
    pos[sizeof pos - 1] = 0;
    const char* dead = r->enabled ? "" : axs(AXS_EV_UNAVAILABLE);
    if (r->desc[0] && !r->enabled)
        _snprintf(out, outsz, "%s. %s %s %s", name, r->desc, dead, pos);
    else if (r->desc[0])
        _snprintf(out, outsz, "%s. %s. %s", name, r->desc, pos);
    else if (!r->enabled)
        _snprintf(out, outsz, "%s. %s %s", name, dead, pos);
    else
        _snprintf(out, outsz, "%s. %s", name, pos);
    out[outsz - 1] = 0;
}

static bool evOpeningLine(uintptr_t base, char* out, int outsz) {
    EvRow rows[OE_MAX_ROWS];
    char title[256], flavour[600];
    int skin = EV_NONE;
    int n = evBuildRows(base, rows, &skin, title, sizeof title, flavour, sizeof flavour);
    if (n <= 0) return false;

    int at = (g_evRow >= 0 && g_evRow < n) ? g_evRow : 0;
    char row[MAILBOX_SZ];
    evRowLine(&rows[at], at, n, row, sizeof row);

    if (flavour[0])
        _snprintf(out, outsz, "%s. %s. %s", title[0] ? title : axs(AXS_EV_TITLE_FALLBACK),
                  flavour, row);
    else
        _snprintf(out, outsz, "%s. %s", title[0] ? title : axs(AXS_EV_TITLE_FALLBACK), row);
    out[outsz - 1] = 0;
    return true;
}

void checkEventOverlay(uintptr_t base) {
    int skin = EV_NONE;
    bool up = false;
    if (evState() == OE_STATE_LIVE) {
        uintptr_t prop = 0, pt = 0;
        skin = evSkinNow(base, &prop, &pt);
        up = (skin != EV_NONE);
    }

    if (up && g_evActive && skin != g_evSkin) {
        g_evSkin = skin;
        g_evRow  = 0;
        g_evPickItem = false;           // a different scroll: the old pick is meaningless
        char utter[MAILBOX_SZ];
        if (evOpeningLine(base, utter, sizeof utter)) postSpeech(utter);
        return;
    }

    if (up == g_evActive) return;
    g_evActive = up;

    if (!up) {
        g_evRow  = 0;
        g_evSkin = EV_NONE;
        g_evPickItem = false;
        logLine("event: scroll closed");
        return;
    }

    g_evSkin = skin;
    g_evRow  = 0;
    g_evPickItem = false;

    EvRow rows[OE_MAX_ROWS];
    char title[256], flavour[600];
    int dummy = EV_NONE;
    int n = evBuildRows(base, rows, &dummy, title, sizeof title, flavour, sizeof flavour);
    logLine("event: scroll opened, skin=%d rows=%d title=\"%s\"", skin, n, title);
    for (int i = 0; i < n; i++)
        logLine("event:   row %d \"%s\" desc=\"%s\" action=0x%llx", i, rows[i].name, rows[i].desc,
                (unsigned long long)rows[i].actionRva);

    char utter[MAILBOX_SZ];
    if (evOpeningLine(base, utter, sizeof utter)) postSpeech(utter);
}

// ---- WHAT THE CURIO DID: the result list at Overlay_Event+0x98 ----
static const uintptr_t OE_RESULTS_BEG_OFF = 0x98;    // Overlay_Event+: vector<Result> begin
static const uintptr_t OE_RESULTS_END_OFF = 0xa0;    // Overlay_Event+: end
static const uintptr_t OE_RESULT_STRIDE   = 0x170;   // from the apply's push cursor (+0x2e qwords)
static const uintptr_t OE_RESULT_TEXT_OFF = 0x000;   // record+: char[0x100], the sentence
static const uintptr_t OE_RESULT_TAG_OFF  = 0x100;   // record+: char[0x40], short; logged only
static const int       OE_MAX_RESULTS     = 8;
static const uintptr_t OE_APPLIED_FLAG    = 0xe56;

static bool evResultTextOk(const char* s) {
    if (!s || !s[0]) return false;
    int n = (int)strlen(s);
    if (n < 2 || n > 250) return false;
    int letters = 0;
    for (int i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c < 0x20 && c != '\n' && c != '\t') return false;   // control byte => not text
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c >= 0x80) letters++;
    }
    if (letters < 2) return false;
    if (s[0] == '[' && s[1] == '<') return false;                // "[<key>]" miss sentinel -- the
    if (strstr(s, "error_not_localized")) return false;
    if (strstr(s, "str_")) return false;                         // an unresolved key echo
    return true;
}

static void evDumpResultRecord(uintptr_t rec) {
    if (!axDebugLogEnabled()) return;                // diagnostic: nothing runs with the log off
    for (uintptr_t off = 0; off < 0x120; off += 0x10) {
        unsigned char b[0x10];
        char hex[64], asc[24];
        int ho = 0;
        for (int i = 0; i < 0x10; i++) {
            if (!safeReadU8(rec + off + i, &b[i])) b[i] = 0;
            ho += _snprintf(hex + ho, sizeof hex - ho, "%02x ", b[i]);
            asc[i] = (b[i] >= 32 && b[i] < 127) ? (char)b[i] : '.';
        }
        asc[0x10] = 0;
        logDump("curiores +0x%03x: %s |%s|", (unsigned)off, hex, asc);
    }
}

static char g_evLastResult[OE_MAX_RESULTS][256];
static int  g_evLastResultCount = 0;
static bool g_evResultDumped    = false;

static void evDumpResolveMoment(uintptr_t oe) {
    uintptr_t rb = 0, re = 0, curio = 0;
    uint8_t applied = 0;
    int32_t st = -1, prevSt = -1;
    safeReadPtr(oe + OE_RESULTS_BEG_OFF, &rb);
    safeReadPtr(oe + OE_RESULTS_END_OFF, &re);
    safeReadPtr(oe + OE_CURIO_PROP_OFF, &curio);
    safeReadU8 (oe + OE_APPLIED_FLAG, &applied);
    safeReadU32(oe + OE_STATE_OFF, reinterpret_cast<uint32_t*>(&st));
    safeReadU32(oe + OE_PREVSTATE_OFF, reinterpret_cast<uint32_t*>(&prevSt));
    char title[300] = {0}, content[600] = {0};
    safeReadCStr(oe + OE_TITLE_OFF,   title,   sizeof title);
    safeReadCStr(oe + OE_CONTENT_OFF, content, sizeof content);
    logDump("curiores RESOLVE: oe=%p state=%d prev=%d applied=%u curioProp=%p",
            (void*)oe, st, prevSt, applied, (void*)curio);
    logDump("curiores RESOLVE: results beg=%p end=%p span=%lld",
            (void*)rb, (void*)re, (long long)(re - rb));
    logDump("curiores RESOLVE: title=\"%s\"", title);
    logDump("curiores RESOLVE: content=\"%s\"", content);
    if (rb && re > rb) evDumpResultRecord(rb);
}

void checkCurioResult(uintptr_t base) {
    (void)base;
    uintptr_t oe = evOverlay();
    if (!oe) { g_evLastResultCount = 0; return; }

    {
        static int32_t s_prevState = -1;
        static bool    s_dumped    = false;
        int32_t st = -1;
        safeReadU32(oe + OE_STATE_OFF, reinterpret_cast<uint32_t*>(&st));
        uintptr_t curio = 0;
        safeReadPtr(oe + OE_CURIO_PROP_OFF, &curio);
        if (!s_dumped && s_prevState == OE_STATE_LIVE && st != OE_STATE_LIVE && curio) {
            s_dumped = true;
            evDumpResolveMoment(oe);
        }
        s_prevState = st;
    }

    uintptr_t beg = 0, end = 0;
    if (!safeReadPtr(oe + OE_RESULTS_BEG_OFF, &beg) ||
        !safeReadPtr(oe + OE_RESULTS_END_OFF, &end) || !beg || end <= beg) {
        g_evLastResultCount = 0;      // the list is gone: the next one starts fresh
        return;
    }
    uintptr_t span = end - beg;
    if (span % OE_RESULT_STRIDE != 0) {
        static uintptr_t s_badSpan = 0;
        if (s_badSpan != span) {
            s_badSpan = span;
            logLine("curiores: span %llu is not a multiple of 0x%llx — stride is wrong, staying silent",
                    (unsigned long long)span, (unsigned long long)OE_RESULT_STRIDE);
            logDump("curiores: BAD STRIDE span=%llu beg=%p end=%p", (unsigned long long)span,
                    (void*)beg, (void*)end);
            evDumpResultRecord(beg);
        }
        return;
    }
    int n = (int)(span / OE_RESULT_STRIDE);
    if (n <= 0 || n > OE_MAX_RESULTS) {
        logLine("curiores: %d records is out of range — refused", n);
        return;
    }

    if (!g_evResultDumped) {
        g_evResultDumped = true;
        logDump("curiores: first list beg=%p end=%p records=%d stride=0x%llx",
                (void*)beg, (void*)end, n, (unsigned long long)OE_RESULT_STRIDE);
        evDumpResultRecord(beg);
    }

    for (int i = 0; i < n; i++) {
        uintptr_t rec = beg + (uintptr_t)i * OE_RESULT_STRIDE;
        char raw[300] = {0}, tag[80] = {0}, text[256] = {0};
        if (!safeReadCStr(rec + OE_RESULT_TEXT_OFF, raw, sizeof raw)) continue;
        safeReadCStr(rec + OE_RESULT_TAG_OFF, tag, sizeof tag);
        stripMarkup(raw, text, sizeof text);
        if (!evResultTextOk(text)) {
            static bool s_loggedBad = false;
            if (!s_loggedBad) {
                s_loggedBad = true;
                logLine("curiores: record %d has no readable text — offsets need correcting", i);
                logDump("curiores: UNREADABLE record %d at %p", i, (void*)rec);
                evDumpResultRecord(rec);
            }
            continue;
        }
        bool seen = false;
        for (int k = 0; k < g_evLastResultCount; k++)
            if (strcmp(g_evLastResult[k], text) == 0) { seen = true; break; }
        if (seen) continue;
        if (g_evLastResultCount < OE_MAX_RESULTS) {
            _snprintf(g_evLastResult[g_evLastResultCount], sizeof g_evLastResult[0], "%s", text);
            g_evLastResult[g_evLastResultCount][sizeof g_evLastResult[0] - 1] = 0;
            g_evLastResultCount++;
        }
        logLine("curiores: result %d/%d tag=\"%s\" -> \"%s\"", i + 1, n, tag, text);
        postSpeech(text, true, SPK_EVENT);
    }
}

void evReannounce(uintptr_t base) {
    char utter[MAILBOX_SZ];
    if (!evOpeningLine(base, utter, sizeof utter)) return;
    logLine("event: re-announcing after a modal closed");
    postSpeech(utter);
}

enum EvActResult { EVACT_FAILED = 0, EVACT_CLICKED, EVACT_CALLED };

static EvActResult evActivate(uintptr_t base, const EvRow* r) {
    uintptr_t oe = evOverlay();
    if (!oe) return EVACT_FAILED;

    if (evState() != OE_STATE_LIVE) {
        logLine("event: refusing to activate \"%s\" — state is %d, not %d",
                r->name, evState(), OE_STATE_LIVE);
        return EVACT_FAILED;
    }

    if (!r->enabled) {
        logLine("event: refusing to activate \"%s\" — the game has this row disabled", r->name);
        return EVACT_FAILED;
    }

    if (r->focusId && frontEndClickElementId(r->focusId)) {
        logLine("event: activating \"%s\" by CLICKING its element 0x%llx", r->name,
                (unsigned long long)r->focusId);
        return EVACT_CLICKED;
    }
    if (!r->actionRva) return EVACT_FAILED;

    uint8_t functor[0x20];
    memset(functor, 0, sizeof functor);
    *(uintptr_t*)(functor + 8) = oe;                              // the capture: `this`
    *(float*)(functor + OE_EAT_COST_CAP_OFF) = r->capFloat;       // hunger's Eat only

    typedef void (*EvActionFn)(void*);
    EvActionFn fn = (EvActionFn)(base + r->actionRva);
    logLine("event: activating \"%s\" via 0x%llx (cap=%.1f)", r->name,
            (unsigned long long)r->actionRva, r->capFloat);
    __try { fn(functor); }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        logLine("event: action for \"%s\" FAULTED", r->name);
        return EVACT_FAILED;
    }
    return EVACT_CALLED;
}

static bool evObstacleAccepts(uintptr_t base, uintptr_t prop, uint32_t typeHash, uint32_t idHash) {
    if (!prop) return false;
    uintptr_t table = 0;
    if (!safeReadPtr(prop + PROP_INTERACT_TABLE_OFF, &table) || table <= 0x10000) return false;
    typedef int (*LookupFn)(uintptr_t, uintptr_t, uint32_t, uint32_t);
    LookupFn fn = (LookupFn)(base + OE_INTERACT_LOOKUP_RVA);
    int idx = -1;
    __try { idx = fn(table, prop + PROP_INLINE_NAME_OFF, typeHash, idHash); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    return idx >= 0;
}

static bool evItemWorks(uintptr_t base, uintptr_t item) {
    uintptr_t oe = evOverlay();
    if (!oe || !item) return false;
    uint32_t typeHash = 0, idHash = 0;
    if (!safeReadU32(item + ITEM_TYPEHASH_OFF, &typeHash)) return false;
    if (!safeReadU32(item + ITEM_IDHASH_OFF,   &idHash))   return false;

    uintptr_t prop = 0, pt = 0;
    if (evSkinNow(base, &prop, &pt) == EV_OBSTACLE)
        return evObstacleAccepts(base, prop, typeHash, idHash);

    uintptr_t cprop = 0, table = 0; uint32_t st = 0;
    if (!safeReadPtr(oe + OE_CURIO_PROP_OFF, &cprop) || cprop <= 0x10000) return false;
    if (!safeReadU32(oe + OE_STATE_OFF, &st) || (st != 1 && st != 2)) return false;
    if (!safeReadPtr(cprop + PROP_INTERACT_TABLE_OFF, &table) || table <= 0x10000) return false;
    typedef int (*LookupFn)(uintptr_t, uintptr_t, uint32_t, uint32_t);
    int idx = -1;
    __try { idx = ((LookupFn)(base + OE_INTERACT_LOOKUP_RVA))(table, cprop + PROP_INLINE_NAME_OFF, typeHash, idHash); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    return idx >= 0;
}

// ---- THE CURIO TRACKER -- what the bag's item icons say ----
static const uintptr_t PROP_NAME_ID_OFF = 0x168;   // Prop+: u32 hashed prop name, the tracker's key
                                                   // (also what ApplyItem's miss path files under)

static bool evTrackerId(uintptr_t base, uintptr_t item, char* out, int outsz) {
    out[0] = 0;
    uintptr_t oe = evOverlay();
    if (!oe || !item) return false;
    uintptr_t cprop = 0;
    if (!safeReadPtr(oe + OE_CURIO_PROP_OFF, &cprop) || cprop <= 0x10000) return false;
    uint32_t typeHash = 0, idHash = 0, questHash = 0, nameId = 0;
    if (!safeReadU32(item + ITEM_TYPEHASH_OFF, &typeHash)) return false;
    if (!safeReadU32(item + ITEM_IDHASH_OFF,   &idHash))   return false;
    if (!safeReadU32(base + QUEST_ITEM_TYPEHASH_RVA, &questHash) || typeHash == questHash) return false;

    typedef uint8_t (*GateFn)(uintptr_t, uint32_t, uint32_t);
    uint8_t gate = 0;
    __try { gate = ((GateFn)(base + CURIO_TRACKER_GATE_RVA))(0, typeHash, idHash); }
    __except (EXCEPTION_EXECUTE_HANDLER) { logLine("tracker: gate faulted"); return false; }
    if (!gate) return false;

    uint8_t questCurio = 0;
    safeReadU8(oe + OE_QUESTITEM_OFF, &questCurio);
    if (questCurio) {
        _snprintf(out, outsz, "%s", "no_effect");
        out[outsz - 1] = 0;
        return true;
    }

    uintptr_t tracker = 0;
    if (!safeReadPtr(base + CURIO_TRACKER_RVA, &tracker) || tracker <= 0x10000) return false;
    if (!safeReadU32(cprop + PROP_NAME_ID_OFF, &nameId)) return false;
    typedef uintptr_t (*QueryFn)(uintptr_t, uint32_t, uint32_t, uint32_t);
    uintptr_t row = 0;
    __try { row = ((QueryFn)(base + CURIO_TRACKER_QUERY_RVA))(tracker, nameId, typeHash, idHash); }
    __except (EXCEPTION_EXECUTE_HANDLER) { logLine("tracker: query faulted"); return false; }
    if (!row) {
        _snprintf(out, outsz, "%s", "unkown");
        out[outsz - 1] = 0;
        return true;
    }
    if (!safeReadCStr(row, out, outsz < 0x40 ? outsz : 0x40)) return false;
    return out[0] != 0;
}

static const char* evTrackerWords(const char* id) {
    static const struct { const char* id; AxStrId s; } kTrk[] = {
        { "unkown",      AXS_EV_TRK_UNKNOWN     }, { "no_effect",   AXS_EV_TRK_NO_EFFECT   },
        { "nothing",     AXS_EV_TRK_NOTHING     }, { "loot",        AXS_EV_TRK_LOOT        },
        { "stress",      AXS_EV_TRK_STRESS      }, { "heal_stress", AXS_EV_TRK_HEAL_STRESS },
        { "heal_gen",    AXS_EV_TRK_HEAL        }, { "buff",        AXS_EV_TRK_BUFF        },
        { "debuff",      AXS_EV_TRK_DEBUFF      }, { "quirk_pos",   AXS_EV_TRK_QUIRK_POS   },
        { "quirk_neg",   AXS_EV_TRK_QUIRK_NEG   }, { "purge_neg",   AXS_EV_TRK_PURGE_NEG   },
        { "purge_pos",   AXS_EV_TRK_PURGE_POS   }, { "summon",      AXS_EV_TRK_SUMMON      },
        { "torch_up",    AXS_EV_TRK_TORCH_UP    },
    };
    for (const auto& t : kTrk)
        if (strcmp(id, t.id) == 0) return axs(t.s);
    logLine("tracker: no words for tracker id \"%s\"", id);
    return nullptr;
}

const char* invNavTag(uintptr_t base, int slot) {
    if (!g_evPickItem) return nullptr;
    uintptr_t beg = 0; int pool = 0;
    if (!invItemVector(base, &beg, &pool) || slot < 0 || slot >= pool) return nullptr;
    char id[0x40];
    if (!evTrackerId(base, beg + (uintptr_t)slot * ITEM_STRIDE, id, sizeof id)) return nullptr;
    return evTrackerWords(id);
}

static bool evDropItem(uintptr_t base, int slot) {
    uintptr_t oe = evOverlay();
    if (!oe) return false;
    if (evState() != OE_STATE_LIVE) {
        logLine("event: refusing to drop an item — state is %d, not %d", evState(), OE_STATE_LIVE);
        return false;
    }
    uintptr_t beg = 0; int slots = 0;
    if (!invItemVector(base, &beg, &slots) || slot < 0 || slot >= slots) return false;
    uintptr_t item = beg + (uintptr_t)slot * ITEM_STRIDE;

    uint32_t amount = 0;
    if (!safeReadU32(item + ITEM_AMOUNT_OFF, &amount) || (int32_t)amount < 1) return false;

    typedef void (*ItemAssignFn)(uintptr_t, uintptr_t);
    typedef void (*ItemClearFn)(uintptr_t);
    ItemAssignFn assign = (ItemAssignFn)(base + ITEM_ASSIGN_RVA);
    ItemClearFn  clear  = (ItemClearFn) (base + ITEM_CLEAR_RVA);

    logLine("event: dropping bag slot %d (amount %d) onto the curio slot", slot, (int)amount);
    __try {
        assign(oe + OE_SLOT_ITEM_OFF, item);              // park a copy for ShowCurio
        safeWriteU8(oe + OE_SLOT_FLAG_OFF, 1);            // "an item is waiting"
        // Consume one from the bag, exactly as the receiver does to its source.
        safeWriteU32(item + ITEM_AMOUNT_OFF, amount - 1);
        if ((int32_t)(amount - 1) < 1) clear(item);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        logLine("event: drop FAULTED for slot %d", slot);
        return false;
    }
    return true;
}

static int evClearObstacle(uintptr_t base, int slot) {
    uintptr_t oe = evOverlay();
    if (!oe) return -1;
    if (evState() != OE_STATE_LIVE) {
        logLine("event: refusing obstacle clear — state is %d, not %d", evState(), OE_STATE_LIVE);
        return -1;
    }
    uintptr_t prop = 0, pt = 0;
    if (evSkinNow(base, &prop, &pt) != EV_OBSTACLE || !prop) return -1;

    uintptr_t beg = 0; int slots = 0;
    if (!invItemVector(base, &beg, &slots) || slot < 0 || slot >= slots) return -1;
    uintptr_t item = beg + (uintptr_t)slot * ITEM_STRIDE;

    uint32_t amount = 0, typeHash = 0, idHash = 0;
    if (!safeReadU32(item + ITEM_AMOUNT_OFF, &amount) || (int32_t)amount < 1) return -1;
    if (!safeReadU32(item + ITEM_TYPEHASH_OFF, &typeHash)) return -1;
    if (!safeReadU32(item + ITEM_IDHASH_OFF,   &idHash))   return -1;

    if (!evObstacleAccepts(base, prop, typeHash, idHash)) return 0;

    uintptr_t ctx = 0;
    safeReadPtr(oe + OE_CTX_OFF, &ctx);

    logLine("event: clearing obstacle with slot %d (type=%08x id=%08x amount=%u)",
            slot, typeHash, idHash, amount);

    (void)ctx;
    static unsigned char ctxBuf[OE_OBSTACLE_CTX_BYTES];   // static: 1.5 KB, keep it off the stack
    memset(ctxBuf, 0, sizeof ctxBuf);
    __try {
        *(uintptr_t*)ctxBuf = oe;
        memcpy(ctxBuf + 8, reinterpret_cast<const void*>(item), 0x98);   // amount, type, hash, id, idhash
        ((void (*)(void*))(base + OE_OBSTACLE_CLEAR_BODY_RVA))(ctxBuf);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        logLine("event: obstacle clear FAULTED for slot %d", slot);
        return -1;
    }
    return 1;
}

// ---- The context predicate (its kAxContexts row stays with the table) ----
bool axIsEvent()   { return g_evActive; }

// ---- The key handler ----
bool routeEventKey(uintptr_t base, uint32_t sym, uint16_t mod, uint8_t repeat) {
    if (mod & (KMOD_LALT | KMOD_RALT | KMOD_LCTRL | KMOD_RCTRL)) return false;

    EvRow rows[OE_MAX_ROWS];
    char title[256], flavour[600];
    int skin = EV_NONE;
    int n = evBuildRows(base, rows, &skin, title, sizeof title, flavour, sizeof flavour);
    if (n <= 0) return false;

    axStepCursor(&g_evRow, n, 0);        // the row set changed under the cursor: re-clamp

    // ---- ITEM-PICKING MODE ----
    if (g_evPickItem) {
        if (sym == SDLK_ESCAPE) {
            if (repeat) return true;
            g_evPickItem = false;
            logLine("event: item pick cancelled");
            char buf[MAILBOX_SZ];
            evRowLine(&rows[g_evRow], g_evRow, n, buf, sizeof buf);
            char utter[MAILBOX_SZ];
            _snprintf(utter, sizeof utter, "%s %s", axs(AXS_CANCELLED), buf);
            utter[sizeof utter - 1] = 0;
            postSpeech(utter);
            return true;
        }

        if (sym == SDLK_RETURN || sym == SDLK_KP_ENTER) {
            if (repeat) return true;
            int slots = invSlotCount(base);
            if (g_invSlot < 0 || g_invSlot >= slots) {
                postSpeech(axs(AXS_NO_ITEM_SELECTED));
                return true;
            }

            // Name the item BEFORE it is used — afterwards the stack may be gone entirely.
            uintptr_t beg = 0; int pool = 0;
            uintptr_t stack = 0;
            if (invItemVector(base, &beg, &pool) && g_invSlot < pool)
                stack = beg + (uintptr_t)g_invSlot * ITEM_STRIDE;
            char itp[64], iidb[64], ikey[192], iname[256];
            iname[0] = 0;
            if (stack) invItemName(base, stack, itp, iidb, ikey, iname);
            const char* shown = iname[0] ? iname : axs(AXS_EV_THE_ITEM);

            if (skin == EV_OBSTACLE) {
                int r = evClearObstacle(base, g_invSlot);
                g_evPickItem = false;
                char utter[MAILBOX_SZ];
                if (r == 1)
                    _snprintf(utter, sizeof utter, axs(AXS_EV_ITEM_USED_FMT), shown);
                                                     // the cleared-obstacle banner follows
                else if (r == 0)
                    _snprintf(utter, sizeof utter, axs(AXS_EV_ITEM_WONT_WORK_FMT), shown);
                                                     // nothing consumed
                else
                    _snprintf(utter, sizeof utter, "%s", axs(AXS_EV_ITEM_UNUSABLE));
                utter[sizeof utter - 1] = 0;
                postSpeech(utter);
                return true;
            }

            bool works = stack && evItemWorks(base, stack);
            uint32_t beforeAmount = 0;
            if (stack) safeReadU32(stack + ITEM_AMOUNT_OFF, &beforeAmount);

            if (!evDropItem(base, g_invSlot)) {
                postSpeech(axs(AXS_EV_ITEM_UNUSABLE));
                return true;
            }

            uint32_t afterAmount = 0;
            if (stack) safeReadU32(stack + ITEM_AMOUNT_OFF, &afterAmount);
            logLine("event: dropped slot %d (\"%s\"), accepted=%d, amount %u -> %u",
                    g_invSlot, shown, works ? 1 : 0, beforeAmount, afterAmount);
            g_evPickItem = false;

            if (!works) {
                char raw[400], msg[400];
                if (resolveKey(base, OE_NO_EFFECT_KEY, raw, sizeof raw) && raw[0])
                    stripMarkup(raw, msg, sizeof msg);
                else
                    _snprintf(msg, sizeof msg, "%s", axs(AXS_EV_NO_EFFECT_FALLBACK));
                postSpeech(msg);
            }
            return true;
        }

        return routeInvKey(base, sym, mod, repeat);
    }

    // ENTER: run the choice under the cursor.
    if (sym == SDLK_RETURN || sym == SDLK_KP_ENTER) {
        if (repeat) return true;
        const EvRow* r = &rows[g_evRow];

        if (r->isItemSlot) {
            int slots = invSlotCount(base);
            if (slots <= 0) {
                postSpeech(axs(AXS_EV_BAG_EMPTY));
                return true;
            }
            g_evPickItem = true;
            if (g_invSlot < 0 || g_invSlot >= slots) g_invSlot = 0;
            logLine("event: handing over to the inventory to pick an item (%d slots)", slots);

            char item[MAILBOX_SZ] = { 0 };
            char utter[MAILBOX_SZ];
            if (invSlotText(base, g_invSlot, item, sizeof item, invNavTag(base, g_invSlot)))
                _snprintf(utter, sizeof utter, axs(AXS_EV_CHOOSE_ITEM_FMT), item);
            else
                _snprintf(utter, sizeof utter, "%s", axs(AXS_EV_CHOOSE_ITEM));
            utter[sizeof utter - 1] = 0;
            postSpeech(utter);
            return true;
        }

        if (!r->enabled) {
            char msg[200];
            if (skin == EV_HUNGER)
                _snprintf(msg, sizeof msg, axs(AXS_EV_NOT_ENOUGH_FOOD_FMT),
                          (int)r->capFloat, evFoodHeld(base));
            else
                _snprintf(msg, sizeof msg, "%s", axs(AXS_EV_UNAVAILABLE));
            msg[sizeof msg - 1] = 0;
            logLine("event: \"%s\" is disabled — refused", r->name);
            postSpeech(msg);
            return true;
        }

        int32_t before = evState();
        EvActResult res = evActivate(base, r);
        if (res == EVACT_FAILED) {
            postSpeech(axs(AXS_EV_CHOICE_FAILED));
            return true;
        }
        if (res == EVACT_CLICKED) return true;

        int32_t after = evState();
        if (after == before) {
            logLine("event: activating \"%s\" left the state at %d — nothing happened",
                    r->name, after);
            postSpeech(axs(AXS_EV_CHOICE_FAILED));
            return true;
        }
        logLine("event: \"%s\" taken, state %d -> %d", r->name, before, after);
        return true;
    }

    {
        int jump = 0;
        if (axDecodeJump(sym, mod, repeat, &jump)) {
            if (!jump) return true;              // held jump: one landing per press
            int from = g_evRow;
            axStepCursor(&g_evRow, n, jump);     // the clamp lands it; ends re-read
            char buf[MAILBOX_SZ];
            evRowLine(&rows[g_evRow], g_evRow, n, buf, sizeof buf);
            logLine("eventnav jump row %d -> %d (of %d)", from, g_evRow, n);
            postSpeech(buf);
            return true;
        }
    }

    int dir;
    switch (sym) {
        case SDLK_UP:    case SDLK_LEFT:  dir = -1; break;
        case SDLK_DOWN:  case SDLK_RIGHT: dir = +1; break;
        default:         return false;   // not ours -> the game gets it
    }
    if (axNavHoldRepeat(repeat)) return true;    // throttled repeat: claimed, no step

    int from = g_evRow;
    axStepCursor(&g_evRow, n, dir);      // hard stop: re-read this row
    char buf[MAILBOX_SZ];
    evRowLine(&rows[g_evRow], g_evRow, n, buf, sizeof buf);
    logLine("eventnav dir=%+d row %d -> %d (of %d)", dir, from, g_evRow, n);
    postSpeech(buf);
    return true;
}
