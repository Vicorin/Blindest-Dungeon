// THE BUTCHER'S CIRCUS — the DUELING GROUNDS.

#include <cstdint>
#include <cstring>
#include <cstdio>
#include <windows.h>
#include "internal.h"

// ---- identity ----

// ---- the fields ----
static const uintptr_t DG_LISTCELL_BEG = 0x3b0; // vector<Widget*> begin — the ONE outer list cell
static const uintptr_t DG_LISTCELL_END = 0x3b8;
static const uintptr_t DG_IDS_BEG    = 0x6f0;   // vector<uint64> begin — friends' steam ids
static const uintptr_t DG_IDS_END    = 0x6f8;   // ... end
static const uintptr_t DG_SEL_OFF    = 0x708;   // uint — selected friend index
static const uintptr_t DG_SECTION_OFF= 0x70c;   // int  — selected section (mode 1)
static const uintptr_t DG_MODE_OFF   = 0x710;   // int  — 0 = friend list, else the Steam-code pane
static const uintptr_t DG_ID_INVITE  = 0x718;   // uint64 — "Invite Opponent"
static const uintptr_t DG_ID_LOBBY   = 0x720;   // uint64 — "Lobby"
static const uintptr_t DG_ID_USER    = 0x728;   // uint64 — "Your Steam ID"
static const uintptr_t DG_MESSAGE_OFF= 0x7f8;   // char*  — the status line on screen, or null
static const uintptr_t DG_REFRESH_OFF= 0x804;   // float  — friends-list refresh interval
static const uintptr_t DG_COUNTDOWN_OFF = 0x808;// float  — ... and its countdown
static const uintptr_t DG_OFFLINE_OFF= 0x80c;   // char   — the friends list is UNAVAILABLE (1=bad)
static const uintptr_t DG_OFFMSG_OFF = 0x80d;   // char   — ...and its message has been raised once
static const uintptr_t DG_EDITING_OFF= 0x80f;   // char   — a section's id field is being edited

static const uint32_t  DG_ELEM_MODESWITCH = 0x6d737762;   // "mswb"

// ---- the ENVELOPE, round 3b ----
static const uintptr_t IW_NAME_OFF    = 0x190;  // char[0x20] — the friend's name, copied from the
static const uintptr_t IW_STEAMID_OFF = 0x1b0;  // uint64 — the friend's steam id. The identity the
static const uintptr_t MP_CONNECTING_OFF = 0xc0;   // uint64 — the steam id being connected to
static const uintptr_t MP_SENTMAP_OFF    = 0x1930; // ptr to the _Myhead of a std::set<CSteamID> of
static const uintptr_t MP_NODE_LEFT_OFF   = 0x00;
static const uintptr_t MP_NODE_PARENT_OFF = 0x08;
static const uintptr_t MP_NODE_RIGHT_OFF  = 0x10;
static const uintptr_t MP_NODE_ISNIL_OFF  = 0x19;
static const uintptr_t MP_NODE_KEY_OFF    = 0x1a;
static const int       MP_TREE_MAX_DEPTH  = 64;    // a red-black tree over a friends list is
                                                   //   shallow; this only bounds a corrupt read

struct DgSection { const char* title; const char* empty; uintptr_t idOff; bool edit; };
static const DgSection DG_SECTIONS[] = {
    { "str_direct_challenge_opponent_invite", "str_direct_challenge_opponent_invite_empty", DG_ID_INVITE, true  },
    { "str_direct_challenge_opponent_lobby",  "str_direct_challenge_opponent_lobby_empty",  DG_ID_LOBBY,  true  },
    { "str_direct_challenge_user",            nullptr,                                      DG_ID_USER,   false },
};
static const int DG_SECTION_COUNT = (int)(sizeof DG_SECTIONS / sizeof DG_SECTIONS[0]);

// ---- the Steam-code pane's LIVE TEXT FIELD, round 4 ----
static const uintptr_t DG_EDIT_DESC_OFF  = 0x730; // the descriptor (its +0x00 is the buffer ptr)
static const uintptr_t DG_EDIT_MAX_OFF   = 0x758; // desc+0x28  int — 0x14, the character limit
static const uintptr_t DG_EDIT_COUNT_OFF = 0x75c; // desc+0x2c  int — the caret, in CHARACTERS
static const uintptr_t DG_EDIT_DONE_OFF  = 0x761; // desc+0x31  byte — the field has finished
static const uintptr_t DG_EDIT_CANCEL_OFF= 0x762; // desc+0x32  byte — ...and it was cancelled
static const uintptr_t DG_EDIT_BUF_OFF   = 0x778;
                                                  //   per keystroke, by the field widget itself.

bool dgIsPanel(uintptr_t base, uintptr_t panel) {
    if (axGameBuild() == AX_BUILD_DRMFREE) return false;
    uintptr_t vft = 0;
    if (!panel || !safeReadPtr(panel, &vft) || vft <= base) return false;
    return vft - base == DG_VFT_RVA;
}

static int dgMode(uintptr_t panel) {
    uint32_t m = 0;
    if (!safeReadU32(panel + DG_MODE_OFF, &m)) return -1;
    if (m > 8) return -1;
    return m == 0 ? 0 : 1;
}

static int dgFriendCount(uintptr_t panel) {
    uintptr_t beg = 0, end = 0;
    if (!safeReadPtr(panel + DG_IDS_BEG, &beg) || !safeReadPtr(panel + DG_IDS_END, &end)) return -1;
    if (!beg && !end) return 0;                          // never built / cleared: an empty list
    if (!beg || end < beg || (end - beg) % 8) return -1;
    uintptr_t n = (end - beg) / 8;
    if (n > 4096) return -1;                             // a Steam friends list, not a bad read
    return (int)n;
}

static bool dgFriendsOffline(uintptr_t panel, bool* sure) {
    uint8_t bad = 0;
    bool ok = safeReadU8(panel + DG_OFFLINE_OFF, &bad);
    if (sure) *sure = ok;
    return ok ? bad != 0 : false;    // an unreadable byte never invents an outage
}

bool dgArrivalText(uintptr_t base, uintptr_t panel, char* out, int outsz) {
    out[0] = 0;
    if (!dgIsPanel(base, panel)) return false;
    int mode = dgMode(panel);
    if (mode < 0) {
        logLine("dueling: mode field at +0x%llx unreadable -> saying nothing about the mode",
                (unsigned long long)DG_MODE_OFF);
        return false;
    }

    const char* modeName = axs(mode == 0 ? AXS_DG_MODE_FRIENDS : AXS_DG_MODE_CODE);

    if (mode == 0) {
        bool sure = false;
        bool offline = dgFriendsOffline(panel, &sure);
        if (sure && offline) {
            char msg[320];
            if (resolveKey(base, "str_friend_list_state_offline", msg, sizeof msg) && msg[0]) {
                abStripMarkup(msg);
                _snprintf(out, outsz, "%s. %s", modeName, msg);
                out[outsz - 1] = 0;
                return true;
            }
            logLine("dueling: str_friend_list_state_offline did not resolve");
            _snprintf(out, outsz, "%s. %s", modeName, axs(AXS_DG_OFFLINE_FALLBACK));
            out[outsz - 1] = 0;
            return true;
        }
        int n = dgFriendCount(panel);
        if (n < 0) {
            _snprintf(out, outsz, "%s.", modeName);
            out[outsz - 1] = 0;
            return true;
        }
        char cnt[64];
        _snprintf(cnt, sizeof cnt, axs(AXS_DG_FRIENDS_N), n);
        cnt[sizeof cnt - 1] = 0;
        _snprintf(out, outsz, "%s. %s", modeName, cnt);
        out[outsz - 1] = 0;
        return true;
    }

    uint32_t sec = 0;
    char title[192];
    title[0] = 0;
    if (safeReadU32(panel + DG_SECTION_OFF, &sec) && (int)sec < DG_SECTION_COUNT) {
        if (resolveKey(base, DG_SECTIONS[sec].title, title, sizeof title) && title[0])
            abStripMarkup(title);
        else
            logLine("dueling: \"%s\" did not resolve", DG_SECTIONS[sec].title);
    }
    if (title[0]) _snprintf(out, outsz, "%s. %s.", modeName, title);
    else          _snprintf(out, outsz, "%s.", modeName);
    out[outsz - 1] = 0;
    return true;
}

static const int DG_ROW_TEXTS_MAX = 4;
static void dgGatherTexts(uintptr_t base, uintptr_t widget, int depth, int* budget,
                          char texts[DG_ROW_TEXTS_MAX][256], int* n) {
    if (!widget || widget <= 0x10000 || depth > 6 || *n >= DG_ROW_TEXTS_MAX) return;
    if (*budget <= 0) return;
    (*budget)--;
    if (!tlLooksLikeWidget(base, widget)) return;

    uintptr_t vft = 0;
    if (safeReadPtr(widget, &vft) && vft == base + TBW_VFTABLE_RVA) {
        char t[256];
        if (readTbwText(widget, t, sizeof t) && t[0]) {
            abStripMarkup(t);
            if (t[0]) {
                strncpy(texts[*n], t, 255);
                texts[*n][255] = 0;
                (*n)++;
                return;                       // a text box has no children worth walking
            }
        }
    }
    uintptr_t beg = 0, end = 0;
    if (safeReadPtr(widget + TL_ATTACH_BEG_OFF, &beg) &&
        safeReadPtr(widget + TL_ATTACH_END_OFF, &end) &&
        beg && end > beg && !((end - beg) % 8)) {
        uintptr_t cnt = (end - beg) / 8;
        if (cnt <= TL_WALK_KIDS_MAX)
            for (uintptr_t i = 0; i < cnt && *n < DG_ROW_TEXTS_MAX; i++) {
                uintptr_t kid = 0;
                if (safeReadPtr(beg + i * 8, &kid)) dgGatherTexts(base, kid, depth + 1, budget, texts, n);
            }
    }
    beg = end = 0;
    if (safeReadPtr(widget + TL_KIDS_BEG_OFF, &beg) &&
        safeReadPtr(widget + TL_KIDS_END_OFF, &end) &&
        beg && end > beg && !((end - beg) % 8)) {
        uintptr_t cnt = (end - beg) / 8;
        if (cnt <= TL_WALK_KIDS_MAX)
            for (uintptr_t i = 0; i < cnt && *n < DG_ROW_TEXTS_MAX; i++) {
                uintptr_t cell = 0, content = 0;
                if (!safeReadPtr(beg + i * 8, &cell) || cell <= 0x10000) continue;
                if (safeReadPtr(cell + TL_CELL_CONTENT_OFF, &content) && content > 0x10000)
                    dgGatherTexts(base, content, depth + 1, budget, texts, n);
                dgGatherTexts(base, cell, depth + 1, budget, texts, n);
            }
    }
}

static uintptr_t dgRowLayout(uintptr_t base, uintptr_t panel) {
    uintptr_t cb = 0, ce = 0, cell = 0, layout = 0;
    if (!safeReadPtr(panel + DG_LISTCELL_BEG, &cb) || !safeReadPtr(panel + DG_LISTCELL_END, &ce))
        return 0;
    if (!cb || ce <= cb) return 0;                       // the list has never been built
    if (!safeReadPtr(cb, &cell) || cell <= 0x10000) return 0;
    if (!safeReadPtr(cell + TL_CELL_CONTENT_OFF, &layout) || layout <= 0x10000) return 0;
    if (!tlLooksLikeWidget(base, layout)) return 0;
    return layout;
}

static uintptr_t dgRowWidget(uintptr_t base, uintptr_t panel, int i) {
    uintptr_t layout = dgRowLayout(base, panel);
    if (!layout) return 0;
    uintptr_t beg = 0, end = 0;
    if (!safeReadPtr(layout + TL_KIDS_BEG_OFF, &beg) ||
        !safeReadPtr(layout + TL_KIDS_END_OFF, &end)) return 0;
    if (!beg || end <= beg || (end - beg) % 8) return 0;
    long n = (long)((end - beg) / 8);
    if (i < 0 || i >= n) return 0;
    uintptr_t cell = 0, row = 0;
    if (!safeReadPtr(beg + (uintptr_t)i * 8, &cell) || cell <= 0x10000) return 0;
    if (!safeReadPtr(cell + TL_CELL_CONTENT_OFF, &row) || row <= 0x10000) return 0;
    return row;
}

enum DgSkin { DG_SKIN_NONE = 0, DG_SKIN_SEND, DG_SKIN_CONNECTING, DG_SKIN_SENT };
static int  dgSkinOf(uintptr_t base, uintptr_t rowWidget, uintptr_t* inviteOut, uint64_t* idOut);
static bool dgSkinWord(uintptr_t base, int skin, char* out, int outsz);

static void dgFriendRowText(uintptr_t base, uintptr_t panel, int i, int n, char* out, int outsz) {
    char texts[DG_ROW_TEXTS_MAX][256];
    int got = 0, budget = TL_WALK_NODES_MAX;
    uintptr_t row = dgRowWidget(base, panel, i);
    if (row) dgGatherTexts(base, row, 0, &budget, texts, &got);

    char lead[192];
    dgSkinWord(base, dgSkinOf(base, row, nullptr, nullptr), lead, sizeof lead);

    if (got <= 0) {
        logLine("dueling: row %d of %d has no readable text (widget=%p)", i + 1, n, (void*)row);
        char bare[192];
        _snprintf(bare, sizeof bare, axs(AXS_DG_FRIEND_UNREADABLE), i + 1, n);
        bare[sizeof bare - 1] = 0;
        _snprintf(out, outsz, "%s%s", lead, bare);
        out[outsz - 1] = 0;
        return;
    }
    char joined[640];
    joined[0] = 0;
    for (int k = 0; k < got; k++) {
        size_t len = strlen(joined);
        if ((int)len >= (int)sizeof joined - 4) break;
        _snprintf(joined + len, (int)(sizeof joined) - (int)len, "%s%s", k ? ". " : "", texts[k]);
    }
    joined[sizeof joined - 1] = 0;
    char pos[64];
    _snprintf(pos, sizeof pos, axs(AXS_DG_FRIEND_N_OF_M), i + 1, n);
    pos[sizeof pos - 1] = 0;
    _snprintf(out, outsz, "%s%s. %s", lead, joined, pos);
    out[outsz - 1] = 0;
}

static void dgSectionRowText(uintptr_t base, uintptr_t panel, int i, char* out, int outsz) {
    if (i < 0) i = 0;
    if (i >= DG_SECTION_COUNT) i = DG_SECTION_COUNT - 1;
    const DgSection* s = &DG_SECTIONS[i];
    char title[192];
    title[0] = 0;
    if (resolveKey(base, s->title, title, sizeof title) && title[0]) abStripMarkup(title);
    else logLine("dueling: \"%s\" did not resolve", s->title);

    uint64_t id = 0;
    bool haveId = safeReadBlock(panel + s->idOff, &id, 8);
    char value[192];
    value[0] = 0;
    if (haveId && id) {
        _snprintf(value, sizeof value, "%llu", (unsigned long long)id);
    } else if (s->empty) {
        if (resolveKey(base, s->empty, value, sizeof value) && value[0]) abStripMarkup(value);
        else logLine("dueling: \"%s\" did not resolve", s->empty);
    }
    if (!value[0]) strncpy(value, axs(AXS_VALUE_EMPTY), sizeof value - 1);
    value[sizeof value - 1] = 0;

    char pos[64];
    _snprintf(pos, sizeof pos, axs(AXS_DG_FIELD_N_OF_M), i + 1, DG_SECTION_COUNT);
    pos[sizeof pos - 1] = 0;
    _snprintf(out, outsz, "%s. %s. %s",
              title[0] ? title : axs(AXS_DG_FIELD), value, pos);
    out[outsz - 1] = 0;
}

// ---- the cursors and the mode-switch watch ----
static int   g_dgFriendRow = 0;      // cursor over the friends list (mode 0)
static int   g_dgField     = 0;      // cursor over the three id fields (mode 1)
static DWORD g_dgSwitchUntil = 0;    // mode-switch watch; 0 = idle
static int   g_dgSwitchFrom  = -1;   // the mode we clicked away FROM
static DWORD g_dgInviteUntil = 0;
static bool  g_dgEditing     = false;
static char  g_dgEditLast[192] = {0};
static DWORD g_dgEditUntil   = 0;    // "the box should have opened by now" watch
static DWORD g_dgCancelUntil = 0;
static uint64_t g_dgCancelId = 0;
static int   g_dgCancelFrom  = 0;

void dgReset() {
    g_dgFriendRow = 0;
    g_dgField     = 0;
    g_dgSwitchUntil = 0;
    g_dgSwitchFrom  = -1;
    g_dgInviteUntil = 0;
    g_dgCancelUntil = 0;
    g_dgCancelId    = 0;
    g_dgCancelFrom  = 0;
    g_dgEditing     = false;
    g_dgEditLast[0] = 0;
    g_dgEditUntil   = 0;
}

static void dgSpeakCursor(uintptr_t base, uintptr_t panel, const char* prefix) {
    char row[MAILBOX_SZ];
    row[0] = 0;
    int mode = dgMode(panel);
    if (mode == 1) {
        if (g_dgField < 0) g_dgField = 0;
        if (g_dgField >= DG_SECTION_COUNT) g_dgField = DG_SECTION_COUNT - 1;
        dgSectionRowText(base, panel, g_dgField, row, sizeof row);
    } else {
        int n = dgFriendCount(panel);
        if (n < 0) {
            _snprintf(row, sizeof row, "%s", axs(AXS_DG_LIST_UNREADABLE));
        } else if (n == 0) {
            bool sure = false;
            char msg[320];
            if (dgFriendsOffline(panel, &sure) && sure &&
                resolveKey(base, "str_friend_list_state_offline", msg, sizeof msg) && msg[0]) {
                abStripMarkup(msg);
                _snprintf(row, sizeof row, "%s %s", axs(AXS_DG_NO_FRIENDS), msg);
            } else {
                _snprintf(row, sizeof row, "%s", axs(AXS_DG_NO_FRIENDS));
            }
        } else {
            if (g_dgFriendRow < 0) g_dgFriendRow = 0;
            if (g_dgFriendRow >= n) g_dgFriendRow = n - 1;
            dgFriendRowText(base, panel, g_dgFriendRow, n, row, sizeof row);
        }
    }
    row[sizeof row - 1] = 0;
    char utter[MAILBOX_SZ];
    _snprintf(utter, sizeof utter, "%s%s", prefix ? prefix : "", row);
    utter[sizeof utter - 1] = 0;
    postSpeech(utter);
}

// ---- why this is a CALL and not a click, which is the opposite of this file's other action ----
// ---- the three skins: Enter does whatever the click under the cursor would do ----
// ---- and the one hard rule this surface has ----

static uintptr_t dgFindInvite(uintptr_t base, uintptr_t widget, int depth, int* budget) {
    if (!widget || widget <= 0x10000 || depth > 6) return 0;
    if (*budget <= 0) return 0;
    (*budget)--;
    if (!tlLooksLikeWidget(base, widget)) return 0;

    uintptr_t vft = 0;
    if (safeReadPtr(widget, &vft) && vft > base && vft - base == IW_VFT_RVA) return widget;

    uintptr_t beg = 0, end = 0;
    if (safeReadPtr(widget + TL_ATTACH_BEG_OFF, &beg) &&
        safeReadPtr(widget + TL_ATTACH_END_OFF, &end) &&
        beg && end > beg && !((end - beg) % 8)) {
        uintptr_t cnt = (end - beg) / 8;
        if (cnt <= TL_WALK_KIDS_MAX)
            for (uintptr_t i = 0; i < cnt; i++) {
                uintptr_t kid = 0, hit = 0;
                if (safeReadPtr(beg + i * 8, &kid) &&
                    (hit = dgFindInvite(base, kid, depth + 1, budget)) != 0) return hit;
            }
    }
    beg = end = 0;
    if (safeReadPtr(widget + TL_KIDS_BEG_OFF, &beg) &&
        safeReadPtr(widget + TL_KIDS_END_OFF, &end) &&
        beg && end > beg && !((end - beg) % 8)) {
        uintptr_t cnt = (end - beg) / 8;
        if (cnt <= TL_WALK_KIDS_MAX)
            for (uintptr_t i = 0; i < cnt; i++) {
                uintptr_t cell = 0, content = 0, hit = 0;
                if (!safeReadPtr(beg + i * 8, &cell) || cell <= 0x10000) continue;
                if (safeReadPtr(cell + TL_CELL_CONTENT_OFF, &content) && content > 0x10000 &&
                    (hit = dgFindInvite(base, content, depth + 1, budget)) != 0) return hit;
                if ((hit = dgFindInvite(base, cell, depth + 1, budget)) != 0) return hit;
            }
    }
    return 0;
}

static uintptr_t dgMpApi(uintptr_t base) {
    if (!MP_API_RVA) return 0;
    uint8_t alt = 1;
    if (!safeReadU8(base + MP_ALT_GATE_RVA, &alt) || alt != 0) return 0;
    uintptr_t api = 0;
    if (!safeReadPtr(base + MP_API_RVA, &api) || api <= 0x10000) return 0;
    return api;
}

// Skin 1: the game is already connecting to this friend.
static bool dgConnectingTo(uintptr_t base, uint64_t steamId) {
    uintptr_t api = dgMpApi(base);
    if (!api || !steamId) return false;
    uint64_t cur = 0;
    if (!safeReadBlock(api + MP_CONNECTING_OFF, &cur, 8)) return false;
    return cur == steamId;
}

static bool dgInviteAlreadySent(uintptr_t base, uint64_t steamId) {
    uintptr_t api = dgMpApi(base);
    if (!api || !steamId) return false;
    uintptr_t head = 0;
    if (!safeReadPtr(api + MP_SENTMAP_OFF, &head) || head <= 0x10000) return false;
    uintptr_t node = 0;
    if (!safeReadPtr(head + MP_NODE_PARENT_OFF, &node) || node <= 0x10000) return false;

    uintptr_t bound = head;                       // lower_bound, starting at end()
    bool reachedLeaf = false;
    for (int step = 0; step < MP_TREE_MAX_DEPTH; step++) {
        uint8_t isNil = 1;
        if (!safeReadU8(node + MP_NODE_ISNIL_OFF, &isNil)) return false;
        if (isNil) { reachedLeaf = true; break; }
        uint64_t key = 0;
        if (!safeReadBlock(node + MP_NODE_KEY_OFF, &key, 8)) return false;
        uintptr_t next = 0;
        if (key < steamId) {                      // too small: go right, keep the old candidate
            if (!safeReadPtr(node + MP_NODE_RIGHT_OFF, &next)) return false;
        } else {                                  // this node is the new candidate; go left
            bound = node;
            if (!safeReadPtr(node + MP_NODE_LEFT_OFF, &next)) return false;
        }
        if (next <= 0x10000) return false;
        node = next;
    }
    if (!reachedLeaf) {
        logLine("dueling: sent-invite set did not terminate in %d steps — treating as not sent",
                MP_TREE_MAX_DEPTH);
        return false;
    }
    if (bound == head) return false;              // nothing in the set is >= this id
    uint8_t isNil = 1;
    uint64_t key = 0;
    if (!safeReadU8(bound + MP_NODE_ISNIL_OFF, &isNil) || isNil) return false;
    if (!safeReadBlock(bound + MP_NODE_KEY_OFF, &key, 8)) return false;
    return key == steamId;
}

static int dgSkinOf(uintptr_t base, uintptr_t rowWidget, uintptr_t* inviteOut, uint64_t* idOut) {
    if (inviteOut) *inviteOut = 0;
    if (idOut) *idOut = 0;
    if (!rowWidget) return DG_SKIN_NONE;
    int budget = TL_WALK_NODES_MAX;
    uintptr_t iw = dgFindInvite(base, rowWidget, 0, &budget);
    if (!iw) return DG_SKIN_NONE;                 // no envelope: offline, or busy in The Ring
    uint64_t id = 0;
    if (!safeReadBlock(iw + IW_STEAMID_OFF, &id, 8) || !id) {
        logLine("dueling: envelope %p has no readable steam id", (void*)iw);
        return DG_SKIN_NONE;
    }
    if (inviteOut) *inviteOut = iw;
    if (idOut) *idOut = id;
    if (dgConnectingTo(base, id)) return DG_SKIN_CONNECTING;
    if (dgInviteAlreadySent(base, id)) return DG_SKIN_SENT;
    return DG_SKIN_SEND;
}

static bool dgSkinWord(uintptr_t base, int skin, char* out, int outsz) {
    if (outsz > 0) out[0] = 0;
    const char* key = (skin == DG_SKIN_CONNECTING) ? "str_connecting"
                    : (skin == DG_SKIN_SENT)       ? "str_invite_sent"
                    : nullptr;
    if (!key) return false;
    char msg[160];
    if (!resolveKey(base, key, msg, sizeof msg) || !msg[0]) {
        logLine("dueling: \"%s\" did not resolve — the row leads with its name instead", key);
        return false;
    }
    abStripMarkup(msg);
    for (int k = (int)strlen(msg) - 1; k >= 0 && (msg[k] == '.' || msg[k] == ' '); k--) msg[k] = 0;
    if (!msg[0]) return false;
    _snprintf(out, outsz, "%s. ", msg);
    out[outsz - 1] = 0;
    return true;
}

static int dgRowOfId(uintptr_t panel, uint64_t steamId) {
    if (!steamId) return -1;
    uintptr_t ib = 0, ie = 0;
    if (!safeReadPtr(panel + DG_IDS_BEG, &ib) || !safeReadPtr(panel + DG_IDS_END, &ie)) return -1;
    if (!ib || ie <= ib || ((ie - ib) % 8)) return -1;
    long n = (long)((ie - ib) / 8);
    if (n > 4096) return -1;
    for (long i = 0; i < n; i++) {
        uint64_t id = 0;
        if (safeReadBlock(ib + (uintptr_t)i * 8, &id, 8) && id == steamId) return (int)i;
    }
    return -1;
}

static bool dgCallSendInvite(uintptr_t base, uintptr_t invite) {
    if (!IW_SEND_RVA) { logLine("dueling: OnSendInvite is absent on this build"); return false; }
    typedef void (*IwSendInviteFn)(uintptr_t);
    IwSendInviteFn fn = (IwSendInviteFn)(base + IW_SEND_RVA);
    __try { fn(invite); }
    __except (EXCEPTION_EXECUTE_HANDLER) { logLine("dueling: OnSendInvite FAULTED"); return false; }
    return true;
}
static bool dgCallDisconnect(uintptr_t base) {
    uintptr_t api = dgMpApi(base);
    if (!api) { logLine("dueling: cancel connect — no live MultiplayerAPI"); return false; }
    if (!MP_DISCONNECT_RVA) { logLine("dueling: Disconnect is absent on this build"); return false; }
    typedef void (*MpDisconnectFn)(uintptr_t);
    MpDisconnectFn fn = (MpDisconnectFn)(base + MP_DISCONNECT_RVA);
    __try { fn(api); }
    __except (EXCEPTION_EXECUTE_HANDLER) { logLine("dueling: Disconnect FAULTED"); return false; }
    return true;
}
static bool dgCallForgetInvite(uintptr_t base, uint64_t steamId) {
    uintptr_t api = dgMpApi(base);
    if (!api || !steamId) { logLine("dueling: cancel invite — no live MultiplayerAPI"); return false; }
    if (!MP_SENT_ERASE_RVA) { logLine("dueling: forget-invite is absent on this build"); return false; }
    uint64_t key[4] = { steamId, 0, 0, 0 };
    typedef void (*MpForgetInviteFn)(uintptr_t, uint64_t*);
    MpForgetInviteFn fn = (MpForgetInviteFn)(base + MP_SENT_ERASE_RVA);
    __try { fn(api + MP_SENTMAP_OFF, key); }
    __except (EXCEPTION_EXECUTE_HANDLER) { logLine("dueling: forget-invite FAULTED"); return false; }
    return true;
}

// Enter on a friends-list row: do whatever clicking that row's envelope would do.
static void dgActivate(uintptr_t base, uintptr_t panel) {
    int n = dgFriendCount(panel);
    if (n <= 0) { dgSpeakCursor(base, panel, nullptr); return; }   // says WHY the list is empty
    if (g_dgFriendRow < 0) g_dgFriendRow = 0;
    if (g_dgFriendRow >= n) g_dgFriendRow = n - 1;

    uintptr_t ib = 0;
    uint64_t wantId = 0;
    if (!safeReadPtr(panel + DG_IDS_BEG, &ib) || !ib ||
        !safeReadBlock(ib + (uintptr_t)g_dgFriendRow * 8, &wantId, 8) || !wantId) {
        logLine("dueling: activate — row %d has no readable steam id", g_dgFriendRow + 1);
        postSpeech(axs(AXS_ACTION_FAILED));
        return;
    }

    uintptr_t rowWidget = dgRowWidget(base, panel, g_dgFriendRow);
    uintptr_t invite = 0;
    uint64_t  haveId = 0;
    int skin = dgSkinOf(base, rowWidget, &invite, &haveId);
    if (skin == DG_SKIN_NONE) {
        logLine("dueling: activate — row %d of %d (steamId=%llu) has no envelope (row widget=%p); "
                "nothing to do", g_dgFriendRow + 1, n, (unsigned long long)wantId, (void*)rowWidget);
        return;
    }

    if (haveId != wantId) {
        logLine("dueling: activate REFUSED — row %d says steamId=%llu but its envelope says %llu "
                "(the 5-second rebuild reordered the list)",
                g_dgFriendRow + 1, (unsigned long long)wantId, (unsigned long long)haveId);
        postSpeech(axs(AXS_DG_ROW_MOVED));
        return;
    }

    char who[64];
    who[0] = 0;
    safeReadCStr(invite + IW_NAME_OFF, who, sizeof who);

    if (skin == DG_SKIN_SEND) {
        logLine("dueling: send — OnSendInvite on envelope %p (row %d of %d, steamId=%llu, name=\"%s\")",
                (void*)invite, g_dgFriendRow + 1, n, (unsigned long long)haveId, who);
        if (!dgCallSendInvite(base, invite)) { postSpeech(axs(AXS_ACTION_FAILED)); return; }
        g_dgInviteUntil = GetTickCount() + 1500;
        return;
    }

    bool ok = (skin == DG_SKIN_CONNECTING) ? dgCallDisconnect(base)
                                           : dgCallForgetInvite(base, haveId);
    logLine("dueling: cancel (%s) for row %d of %d, steamId=%llu, name=\"%s\" -> call %s",
            skin == DG_SKIN_CONNECTING ? "connecting" : "invite sent",
            g_dgFriendRow + 1, n, (unsigned long long)haveId, who, ok ? "made" : "REFUSED");
    if (!ok) { postSpeech(axs(AXS_ACTION_FAILED)); return; }
    g_dgCancelId    = haveId;
    g_dgCancelFrom  = skin;
    g_dgCancelUntil = GetTickCount() + 1500;
}

// ---- one function is the whole door ----
// ---- ⚠ WHILE THE BOX IS OPEN THE MOD CLAIMS NOTHING ----
// ---- and the echo is a READ, not a reconstruction ----

// Is the panel's text field open right now?
static bool dgIsEditing(uintptr_t panel) {
    uint8_t f = 0;
    return panel && safeReadU8(panel + DG_EDITING_OFF, &f) && f != 0;
}

static void dgEditText(uintptr_t panel, char* out, int outsz) {
    char buf[192];
    buf[0] = 0;
    if (!safeReadCStr(panel + DG_EDIT_BUF_OFF, buf, sizeof buf) || !buf[0]) {
        _snprintf(out, outsz, "%s", axs(AXS_NM_BLANK));
    } else {
        _snprintf(out, outsz, "%s", buf);
    }
    out[outsz - 1] = 0;
}

bool dgEditFieldOpen(uintptr_t base) {
    uintptr_t root = resTownRoot(base);
    uintptr_t panel = root ? bldOpenPanel(root) : 0;
    return panel && dgIsPanel(base, panel) && dgIsEditing(panel);
}

// Enter on a Steam-code row: work the game's own edit door for that section.
static void dgToggleEdit(uintptr_t base, uintptr_t panel) {
    if (g_dgField < 0) g_dgField = 0;
    if (g_dgField >= DG_SECTION_COUNT) g_dgField = DG_SECTION_COUNT - 1;
    const DgSection* s = &DG_SECTIONS[g_dgField];
    if (!s->edit) {
        logLine("dueling: edit — section %d is not typable; nothing to do", g_dgField);
        return;
    }
    uint64_t id = 0;
    safeReadBlock(panel + s->idOff, &id, 8);       // the value the game seeds the box with
    logLine("dueling: edit — opening section %d (\"%s\", id=%llu)",
            g_dgField, s->title, (unsigned long long)id);

    typedef void (*DgEditToggleFn)(uintptr_t, int, uint64_t, char);
    DgEditToggleFn fn = (DgEditToggleFn)(base + DG_EDIT_TOGGLE_RVA);
    __try { fn(panel, g_dgField, id, 0); }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        logLine("dueling: edit toggle FAULTED");
        postSpeech(axs(AXS_ACTION_FAILED));
        return;
    }
    g_dgEditUntil = GetTickCount() + 1500;
}

static void dgServiceEdit(uintptr_t base, uintptr_t panel) {
    bool editing = panel && dgIsEditing(panel);

    if (editing != g_dgEditing) {
        g_dgEditing = editing;
        g_dgEditUntil = 0;
        if (editing) {
            int sec = 0;
            uint32_t s32 = 0;
            if (safeReadU32(panel + DG_SECTION_OFF, &s32) && (int)s32 >= 0 &&
                (int)s32 < DG_SECTION_COUNT) sec = (int)s32;
            g_dgField = sec;
            char title[192];
            title[0] = 0;
            if (!resolveKey(base, DG_SECTIONS[sec].title, title, sizeof title) || !title[0])
                _snprintf(title, sizeof title, "%s", axs(AXS_DG_FIELD));
            else abStripMarkup(title);
            dgEditText(panel, g_dgEditLast, sizeof g_dgEditLast);   // seeded: already in the line
            char line[MAILBOX_SZ];
            _snprintf(line, sizeof line, "%s. %s. %s.", title, g_dgEditLast, axs(AXS_NM_HELP));
            line[sizeof line - 1] = 0;
            postSpeech(line);
            logLine("dueling: edit OPEN section %d text=\"%s\"", sec, g_dgEditLast);
        } else {
            g_dgEditLast[0] = 0;
            logLine("dueling: edit CLOSE");
            if (panel) dgSpeakCursor(base, panel, nullptr);
        }
        return;
    }

    if (editing) {
        char now[192];
        dgEditText(panel, now, sizeof now);
        if (strcmp(now, g_dgEditLast) != 0) {
            strncpy(g_dgEditLast, now, sizeof g_dgEditLast - 1);
            g_dgEditLast[sizeof g_dgEditLast - 1] = 0;
            postSpeech(g_dgEditLast);
        }
        return;
    }

    if (g_dgEditUntil && (!panel || GetTickCount() > g_dgEditUntil)) {
        bool gone = !panel;
        g_dgEditUntil = 0;
        logLine(gone ? "dueling: edit watch ended by the screen closing"
                     : "dueling: edit watch timed out — the box never opened");
        if (!gone) postSpeech(axs(AXS_DIDNT_HAPPEN));
    }
}

void dgService(uintptr_t base, uintptr_t panel) {
    if (panel && !dgIsPanel(base, panel)) panel = 0;

    dgServiceEdit(base, panel);

    if (g_dgInviteUntil) {
        if (confirmDialogOpen(base)) {
            g_dgInviteUntil = 0;                 // the ConfirmDialog reader speaks the question
            logLine("dueling: invite confirm observed");
        } else if (!panel || !dgIsPanel(base, panel)) {
            g_dgInviteUntil = 0;
            logLine("dueling: invite watch ended by the screen closing");
        } else if (GetTickCount() > g_dgInviteUntil) {
            g_dgInviteUntil = 0;
            logLine("dueling: invite watch timed out — no confirm dialog appeared");
            postSpeech(axs(AXS_DIDNT_HAPPEN));
        }
    }

    if (g_dgCancelUntil) {
        if (!panel || !dgIsPanel(base, panel)) {
            g_dgCancelUntil = 0;
            logLine("dueling: cancel watch ended by the screen closing");
        } else {
            int at = dgRowOfId(panel, g_dgCancelId);
            int skin = (at >= 0) ? dgSkinOf(base, dgRowWidget(base, panel, at), nullptr, nullptr)
                                 : DG_SKIN_NONE;
            if (at < 0 || skin != g_dgCancelFrom) {
                g_dgCancelUntil = 0;
                logLine("dueling: cancel observed (steamId=%llu now row %d, skin %d -> %d)",
                        (unsigned long long)g_dgCancelId, at, g_dgCancelFrom, skin);
                dgSpeakCursor(base, panel, nullptr);
            } else if (GetTickCount() > g_dgCancelUntil) {
                g_dgCancelUntil = 0;
                logLine("dueling: cancel watch timed out, still skin %d", g_dgCancelFrom);
                postSpeech(axs(AXS_DIDNT_HAPPEN));
            }
        }
    }
    if (!g_dgSwitchUntil) return;
    if (!panel || !dgIsPanel(base, panel)) {       // the screen went away under the watch
        logLine("dueling: mode-switch watch ended by the screen closing");
        g_dgSwitchUntil = 0;
        return;
    }
    int mode = dgMode(panel);
    if (mode >= 0 && mode != g_dgSwitchFrom) {
        g_dgSwitchUntil = 0;
        if (mode == 0) g_dgFriendRow = 0; else g_dgField = 0;
        logLine("dueling: mode %d -> %d observed", g_dgSwitchFrom, mode);
        char prefix[96];
        _snprintf(prefix, sizeof prefix, "%s. ", axs(mode == 0 ? AXS_DG_MODE_FRIENDS : AXS_DG_MODE_CODE));
        prefix[sizeof prefix - 1] = 0;
        dgSpeakCursor(base, panel, prefix);
        return;
    }
    if (GetTickCount() > g_dgSwitchUntil) {
        g_dgSwitchUntil = 0;
        logLine("dueling: mode-switch watch timed out, still mode %d", mode);
        postSpeech(axs(AXS_DG_VIEW_UNCHANGED));
    }
}

bool dgRouteKey(uintptr_t base, uintptr_t panel, uint32_t sym, uint8_t repeat) {
    if (!dgIsPanel(base, panel)) return false;

    if (dgIsEditing(panel)) return false;

    if (sym == SDLK_RETURN || sym == SDLK_KP_ENTER) {
        int mode = dgMode(panel);
        if (mode < 0) return false;              // unreadable: claim nothing, guess nothing
        if (repeat) return true;                 // one action per press, never per key-repeat
        if (g_dgSwitchUntil) { postSpeech(axs(AXS_STILL_SWITCHING)); return true; }
        if (mode == 1) {                         // the Steam-code pane's own edit control
            if (g_dgEditUntil) return true;      // the box is already on its way up
            dgToggleEdit(base, panel);
            return true;
        }
        if (g_dgInviteUntil || g_dgCancelUntil) return true;
        dgActivate(base, panel);
        return true;
    }

    if (sym == SDLK_LEFT || sym == SDLK_RIGHT) {
        if (repeat) return true;
        if (g_dgSwitchUntil) { postSpeech(axs(AXS_STILL_SWITCHING)); return true; }
        int mode = dgMode(panel);
        if (mode < 0) {
            logLine("dueling: mode unreadable -> refusing to switch");
            postSpeech(axs(AXS_DG_VIEW_STUCK));
            return true;
        }
        if (!frontEndClickElementId((int64_t)(uint32_t)DG_ELEM_MODESWITCH)) {
            logLine("dueling: mode-switch element 0x%08x is not on screen", DG_ELEM_MODESWITCH);
            postSpeech(axs(AXS_DG_VIEW_STUCK));
            return true;
        }
        g_dgSwitchFrom  = mode;
        g_dgSwitchUntil = GetTickCount() + 1500;   // dgService announces the landing
        return true;
    }

    {
        int jump = 0;
        if (axDecodeJump(sym, 0, repeat, &jump)) {
            if (!jump) return true;                  // held jump: one landing per press
            if (g_dgSwitchUntil) { postSpeech(axs(AXS_STILL_SWITCHING)); return true; }
            int mode = dgMode(panel);
            if (mode == 1) {
                axStepCursor(&g_dgField, DG_SECTION_COUNT, jump);
            } else {
                int n = dgFriendCount(panel);
                if (n > 0) axStepCursor(&g_dgFriendRow, n, jump);
                // n == 0 falls through: dgSpeakCursor says why the list is empty
            }
            dgSpeakCursor(base, panel, nullptr);
            return true;
        }
    }

    if (sym == SDLK_UP || sym == SDLK_DOWN) {
        if (axNavHoldRepeat(repeat)) return true;    // throttled repeat: claimed, no step
        if (g_dgSwitchUntil) { postSpeech(axs(AXS_STILL_SWITCHING)); return true; }
        int delta = (sym == SDLK_DOWN) ? 1 : -1;
        int mode = dgMode(panel);
        if (mode == 1) {
            axStepCursor(&g_dgField, DG_SECTION_COUNT, delta);
        } else {
            int n = dgFriendCount(panel);
            if (n > 0) axStepCursor(&g_dgFriendRow, n, delta);
        }
        dgSpeakCursor(base, panel, nullptr);
        return true;
    }
    return false;
}

bool dgSpeakArrival(uintptr_t base, uintptr_t panel, const char* prefix) {
    char head[384];
    if (!dgArrivalText(base, panel, head, sizeof head)) return false;
    char full[512];
    _snprintf(full, sizeof full, "%s%s ", prefix ? prefix : "", head);
    full[sizeof full - 1] = 0;
    dgSpeakCursor(base, panel, full);
    return true;
}

// ---- the probe ----
void dgProbe(uintptr_t base, uintptr_t panel) {
    if (!panel) { logDump("dueling probe: no panel"); return; }
    uintptr_t vft = 0;
    safeReadPtr(panel, &vft);
    logDump("dueling probe: panel=%p vft=%p (expect base+0x%llx = %p) match=%d",
            (void*)panel, (void*)vft, (unsigned long long)DG_VFT_RVA, (void*)(base + DG_VFT_RVA),
            dgIsPanel(base, panel) ? 1 : 0);
    if (!dgIsPanel(base, panel)) {
        logDump("dueling probe: NOT the dueling grounds panel -> nothing else is trustworthy here");
        return;
    }

    uint32_t mode = 0xffffffff, sec = 0xffffffff, sel = 0xffffffff;
    safeReadU32(panel + DG_MODE_OFF, &mode);
    safeReadU32(panel + DG_SECTION_OFF, &sec);
    safeReadU32(panel + DG_SEL_OFF, &sel);
    uint8_t offline = 0xff, offMsg = 0xff, editing = 0xff;
    safeReadU8(panel + DG_OFFLINE_OFF, &offline);
    safeReadU8(panel + DG_OFFMSG_OFF, &offMsg);
    safeReadU8(panel + DG_EDITING_OFF, &editing);
    uint32_t refresh = 0, countdown = 0;
    safeReadU32(panel + DG_REFRESH_OFF, &refresh);
    safeReadU32(panel + DG_COUNTDOWN_OFF, &countdown);
    logDump("dueling probe: mode(+0x710)=%d section(+0x70c)=%d selected(+0x708)=%u "
            "friendsOFFLINE(+0x80c)=%u [1=unavailable] msgShown(+0x80d)=%u editing(+0x80f)=%u "
            "refresh(+0x804)=%.2f countdown(+0x808)=%.2f",
            (int)mode, (int)sec, sel, offline, offMsg, editing,
            u32AsFloatM(refresh), u32AsFloatM(countdown));

    uintptr_t bufPtr = 0;
    uint32_t editMax = 0, editCount = 0;
    uint8_t editDone = 0xff, editCancel = 0xff;
    char editBuf[192];
    editBuf[0] = 0;
    safeReadPtr(panel + DG_EDIT_DESC_OFF, &bufPtr);
    safeReadU32(panel + DG_EDIT_MAX_OFF, &editMax);
    safeReadU32(panel + DG_EDIT_COUNT_OFF, &editCount);
    safeReadU8(panel + DG_EDIT_DONE_OFF, &editDone);
    safeReadU8(panel + DG_EDIT_CANCEL_OFF, &editCancel);
    safeReadCStr(panel + DG_EDIT_BUF_OFF, editBuf, sizeof editBuf);
    logDump("dueling probe: edit desc(+0x730)=%p (expect &panel+0x778 = %p) max(+0x758)=%u "
            "caret(+0x75c)=%u done(+0x761)=%u cancelled(+0x762)=%u buffer(+0x778)=\"%s\"",
            (void*)bufPtr, (void*)(panel + DG_EDIT_BUF_OFF), editMax, editCount,
            editDone, editCancel, editBuf);

    uint64_t inviteId = 0, lobbyId = 0, userId = 0;
    safeReadBlock(panel + DG_ID_INVITE, &inviteId, 8);
    safeReadBlock(panel + DG_ID_LOBBY,  &lobbyId,  8);
    safeReadBlock(panel + DG_ID_USER,   &userId,   8);
    logDump("dueling probe: ids invite(+0x718)=%llu lobby(+0x720)=%llu user(+0x728)=%llu",
            (unsigned long long)inviteId, (unsigned long long)lobbyId, (unsigned long long)userId);

    uintptr_t msg = 0;
    char msgText[256];
    msgText[0] = 0;
    if (safeReadPtr(panel + DG_MESSAGE_OFF, &msg) && msg > 0x10000)
        safeReadCStr(msg, msgText, sizeof msgText);
    logDump("dueling probe: message(+0x7f8)=%p \"%s\"", (void*)msg, msgText);

    uintptr_t ib = 0, ie = 0;
    bool idsOk = safeReadPtr(panel + DG_IDS_BEG, &ib) && safeReadPtr(panel + DG_IDS_END, &ie);
    long idN = (idsOk && ib && ie >= ib && !((ie - ib) % 8)) ? (long)((ie - ib) / 8) : -1;
    logDump("dueling probe: ids(+0x6f0..0x6f8)=[%p,%p) -> %ld friend(s)", (void*)ib, (void*)ie, idN);

    uintptr_t cb = 0, ce = 0;
    bool cellsOk = safeReadPtr(panel + DG_LISTCELL_BEG, &cb) &&
                   safeReadPtr(panel + DG_LISTCELL_END, &ce);
    long cellN = (cellsOk && cb && ce >= cb && !((ce - cb) % 8)) ? (long)((ce - cb) / 8) : -1;
    logDump("dueling probe: listCell(+0x3b0..0x3b8)=[%p,%p) -> %ld (expect 1: the outer cell)",
            (void*)cb, (void*)ce, cellN);

    uintptr_t outerCell = 0, layout = 0, rb = 0, re = 0;
    long rowN = -1;
    if (cellN >= 1 && safeReadPtr(cb, &outerCell) && outerCell > 0x10000) {
        safeReadPtr(outerCell + TL_CELL_CONTENT_OFF, &layout);
        logDump("dueling probe: outerCell=%p -> content(+0x%llx)=%p looksLikeWidget=%d",
                (void*)outerCell, (unsigned long long)TL_CELL_CONTENT_OFF, (void*)layout,
                (layout > 0x10000 && tlLooksLikeWidget(base, layout)) ? 1 : 0);
        if (layout > 0x10000 &&
            safeReadPtr(layout + TL_KIDS_BEG_OFF, &rb) && safeReadPtr(layout + TL_KIDS_END_OFF, &re) &&
            rb && re >= rb && !((re - rb) % 8))
            rowN = (long)((re - rb) / 8);
        logDump("dueling probe: rowLayout=%p kids(+0x%llx..+0x%llx)=[%p,%p) -> %ld row cell(s)%s",
                (void*)layout, (unsigned long long)TL_KIDS_BEG_OFF,
                (unsigned long long)TL_KIDS_END_OFF, (void*)rb, (void*)re, rowN,
                (idN >= 0 && rowN >= 0 && idN != rowN)
                    ? "   <-- MISMATCH with the id count: the chain is still wrong" : "");
    }

    for (long i = 0; i < idN && i < 24; i++) {
        uint64_t sid = 0;
        safeReadBlock(ib + (uintptr_t)i * 8, &sid, 8);
        uintptr_t rowCell = 0, rowWidget = 0;
        char text[256], via[64];
        text[0] = via[0] = 0;
        if (rowN > i && safeReadPtr(rb + (uintptr_t)i * 8, &rowCell) && rowCell > 0x10000) {
            safeReadPtr(rowCell + TL_CELL_CONTENT_OFF, &rowWidget);
            if (rowWidget > 0x10000)
                extractWidgetText(rowWidget, base + TBW_VFTABLE_RVA, text, sizeof text,
                                  via, sizeof via);
        }
        logDump("dueling probe:   row %ld: steamId=%llu cell=%p widget=%p text=\"%s\" via=%s%s",
                i, (unsigned long long)sid, (void*)rowCell, (void*)rowWidget, text,
                via[0] ? via : "(none)", ((uint32_t)i == sel) ? "  <- selected" : "");
    }
    if (idN > 24) logDump("dueling probe:   ... %ld more row(s)", idN - 24);

    for (int s = 0; s < DG_SECTION_COUNT; s++) {
        char t[192];
        bool ok = resolveKey(base, DG_SECTIONS[s].title, t, sizeof t) && t[0];
        logDump("dueling probe: section %d \"%s\" -> %s\"%s\"",
                s, DG_SECTIONS[s].title, ok ? "" : "MISS ", ok ? t : "");
    }
    if ((int)mode <= 8) {
        char key[64], t[192];
        _snprintf(key, sizeof key, "str_direct_challenge_switch_mode_%d", (int)mode);
        key[sizeof key - 1] = 0;
        bool ok = resolveKey(base, key, t, sizeof t) && t[0];
        logDump("dueling probe: switch button (element 0x%08x \"mswb\") label key \"%s\" -> %s\"%s\""
                "  [this names where it GOES, not where you are]",
                DG_ELEM_MODESWITCH, key, ok ? "" : "MISS ", ok ? t : "");
    }
}
