// core/subtitles.cpp -- THE GAME'S OWN SUBTITLES, READ ALOUD.

#include <cstdio>
#include <cstring>
#include "internal.h"
#include "game/offsets.h"

static const uintptr_t SUB_TEXT_OFF    = 0x193c;
static const uintptr_t SUB_TEXT_MAX    = 0x200;   // the game's own strncpy_s bound
static const uintptr_t SUB_VISIBLE_OFF = 0x1bd8;

static char s_subLast[MAILBOX_SZ];

static bool s_subLogged = false;

static bool subIsMissSentinel(const char* s) {
    return (s[0] == '[' && s[1] == '<') || strstr(s, "error_not_localized") != nullptr;
}

static const uintptr_t SUB_MOVIE_OFF = 0x1cf0;
bool subMovieTrackUp(uintptr_t base) {
    if (!base) return false;
    uintptr_t movie = 0;
    return safeReadPtr(base + DARKEST_APP_RVA + SUB_MOVIE_OFF, &movie) && movie > 0x10000;
}

void serviceSubtitles(uintptr_t base) {
    if (!base) return;
    if (!axReadSubtitles()) { s_subLast[0] = 0; return; }

    uintptr_t app = base + DARKEST_APP_RVA;

    uint8_t visible = 0;
    if (!safeReadU8(app + SUB_VISIBLE_OFF, &visible)) return;
    if (!visible) { s_subLast[0] = 0; return; }      // gone -> re-arm; the buffer keeps stale text

    char raw[SUB_TEXT_MAX + 1];
    if (!safeReadCStr(app + SUB_TEXT_OFF, raw, (int)sizeof raw)) return;
    if (!raw[0] || subIsMissSentinel(raw)) return;

    char line[MAILBOX_SZ];
    stripMarkup(raw, line, sizeof line);
    if (!line[0]) return;
    if (strcmp(line, s_subLast) == 0) return;        // same chunk still up: nothing new to say

    strncpy(s_subLast, line, sizeof s_subLast - 1);
    s_subLast[sizeof s_subLast - 1] = 0;

    if (!s_subLogged) {
        s_subLogged = true;
        uintptr_t movie = 0;
        safeReadPtr(app + SUB_MOVIE_OFF, &movie);    // diagnostic only: narration vs cinematic
        logLine("subtitles: first line read (movie track %s): \"%s\"",
                movie ? "present" : "none", line);
    }

    postSpeech(line, false, SPK_EVENT);
}
