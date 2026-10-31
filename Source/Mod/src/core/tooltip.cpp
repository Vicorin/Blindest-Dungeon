// core/tooltip.cpp -- THE TOOLTIP CHANNEL.

#include <windows.h>
#include <cstdio>
#include <cstring>
#include "internal.h"

// ---- THE UNPROMPTED LANDING SPEAKS THE ROW BARE ----
static bool g_tipUnprompted = false;
void axTipSetUnprompted(bool on) { g_tipUnprompted = on; }
bool axTipWanted() { return axAutoTooltips() && !g_tipUnprompted; }

static bool tipEnded(const char* buf, size_t n) {
    if (n == 0) return true;
    char c = buf[n - 1];
    if (c == '.' || c == '?' || c == '!') return true;
    if (n >= 3) {
        const unsigned char* t = reinterpret_cast<const unsigned char*>(buf + n - 3);
        if (t[0] == 0xE3 && t[1] == 0x80 && t[2] == 0x82) return true;                       // 。
        if (t[0] == 0xEF && t[1] == 0xBC && (t[2] == 0x8E || t[2] == 0x9F || t[2] == 0x81)) return true;
    }
    return false;
}

static bool g_tipGluingKeywords = false;
void axTipGlue(char* buf, int bufsz, const char* line) {
    if (!buf || bufsz <= 0 || !line) return;
    while (*line == ' ' || *line == '\t') line++;        // a composer's leading join space
    if (!*line) return;
    size_t n = strlen(buf);
    while (n > 0 && (buf[n - 1] == ' ' || buf[n - 1] == '\t')) buf[--n] = 0;
    if ((int)n >= bufsz - 2) return;
    bool ended = tipEnded(buf, n);
    _snprintf(buf + n, (size_t)bufsz - n, "%s%s%s", ended ? "" : ".", n ? " " : "", line);
    buf[bufsz - 1] = 0;
    if (!g_tipGluingKeywords) axTipNoteKeywords(line);
}
void axTipGlueKeywordLine(char* buf, int bufsz, const char* line) {
    g_tipGluingKeywords = true;
    axTipGlue(buf, bufsz, line);
    g_tipGluingKeywords = false;
}

void axTipGlueLines(char* buf, int bufsz, const char* lines, int stride, int n, int from) {
    if (!lines || stride <= 0) return;
    for (int i = (from < 0 ? 0 : from); i < n; i++)
        axTipGlue(buf, bufsz, lines + (size_t)i * (size_t)stride);
}

void axTipGluePanel(char* buf, int bufsz, const char* head, const char* lines, int stride,
                    int n, int from) {
    if (from < 0) from = 0;
    if (n <= from) return;                              // an empty panel is not announced
    bool any = false;
    for (int i = from; i < n && !any; i++) {
        const char* l = lines + (size_t)i * (size_t)stride;
        while (*l == ' ') l++;
        any = *l != 0;
    }
    if (!any) return;
    if (head && head[0]) axTipGlue(buf, bufsz, head);
    axTipGlueLines(buf, bufsz, lines, stride, n, from);
}

bool axTipStep(int* cursor, int dir, const char* lines, int stride, int n) {
    if (!cursor) return true;
    if (!lines || stride <= 0) n = 0;
    int kw    = (n > 0) ? axTipKeywordCount(lines, stride, n, 1, nullptr) : 0;   // 1: line 0 is the row
    int total = n + kw;
    if (total <= 1) { *cursor = 0; postSpeech(axs(AXS_NO_MORE_INFO)); return true; }
    if (*cursor < 0 || *cursor >= total) *cursor = 0;
    const char* note = nullptr;
    *cursor = axTipWrapStep(*cursor, dir, total, &note);
    if (*cursor < n) {
        const char* l = lines + (size_t)*cursor * (size_t)stride;
        logLine("tooltip: line %d/%d -> \"%s\"", *cursor, total, l);
        axPostWrapped(note, l);
        return true;
    }
    char def[AB_TIP_LINE_SZ];
    if (!axTipKeywordLine(lines, stride, n, 1, nullptr, *cursor - n, def, sizeof def)) {   // the text changed under the cursor
        *cursor = 0;
        postSpeech(lines);
        return true;
    }
    logLine("tooltip: keyword line %d/%d -> \"%s\"", *cursor, total, def);
    axPostWrapped(note, def);
    return true;
}

// ---- THE WRAP NOTE ----
const char* axWrapNote(int from, int to, int dir) {
    if (from < 0 || to == from) return nullptr;
    if (dir > 0 && to < from) return axs(AXS_WRAP_TOP);
    if (dir < 0 && to > from) return axs(AXS_WRAP_BOTTOM);
    return nullptr;
}

int axTipWrapStep(int from, int dir, int total, const char** note) {
    if (note) *note = nullptr;
    if (total <= 0) return 0;
    if (from < 0 || from >= total) return 0;              // a first placement, never a wrap
    int step = dir < 0 ? -1 : 1;
    int to   = from + step;
    int mode = axTipWrapMode();
    if (mode == AX_TIPWRAP_OFF) {
        if (to < 0) to = 0;
        if (to >= total) to = total - 1;
        return to;
    }
    to = (to + total) % total;
    if (note && mode == AX_TIPWRAP_SPOKEN) *note = axWrapNote(from, to, step);
    return to;
}

void axPostWrapped(const char* note, const char* text) {
    if (!text) return;
    if (!note || !note[0]) { postSpeech(text); return; }
    char full[MAILBOX_SZ];
    _snprintf(full, sizeof full, "%s %s", note, text);
    full[sizeof full - 1] = 0;
    postSpeech(full);
}
