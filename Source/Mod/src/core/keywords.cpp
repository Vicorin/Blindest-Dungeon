// core/keywords.cpp -- KEYWORD DEFINITIONS

#include <windows.h>
#include <cstdio>
#include <cstring>
#include "internal.h"

static const int KW_MAX_TERMS  = 128;
static const int KW_TERM_SZ    = 96;
static const int KW_DEF_SZ     = 512;   // GL_ROW_MAX: a definition is a sentence or two
static const int KW_MAX_HITS   = 32;
static const int KW_BLOCK      = 1000;  // the game's id blocks (1.., 1001.., 2001.., ...)
static const int KW_GAP        = 10;    // misses in a row that end a block
static const int KW_EMPTY_END  = 3;     // empty blocks in a row that end the scan
static const int KW_MAX_BLOCKS = 20;
static const int KW_CIRCUS_LO  = 2000;
static const int KW_CIRCUS_HI  = 3000;
static const int kKwExcluded[] = { 2004, 2005 };

struct KwTerm {
    int  id;
    char term[KW_TERM_SZ];      // as the game writes it, the trailing ':' trimmed
    int  matchLen;
    char def[KW_DEF_SZ];
};
static KwTerm g_kwTerms[KW_MAX_TERMS];
static int    g_kwCount  = -1;          // -1 = never built this session
static bool   g_kwCircus = false;       // the Circus flag the table was built under

static void kwTrimEnd(char* s, const char* drop) {
    size_t n = strlen(s);
    while (n > 0 && strchr(drop, s[n - 1])) s[--n] = 0;
}

static void kwBuild(uintptr_t base) {
    g_kwCount  = 0;
    g_kwCircus = riInCircus(base);
    int emptyBlocks = 0;
    for (int block = 0; block < KW_MAX_BLOCKS && emptyBlocks < KW_EMPTY_END; block++) {
        int  misses = 0;
        bool any    = false;
        for (int id = block * KW_BLOCK; id < (block + 1) * KW_BLOCK && misses < KW_GAP; id++) {
            char key[64], raw[KW_DEF_SZ];
            _snprintf(key, sizeof key, "str_glossary_term_%d", id);
            key[sizeof key - 1] = 0;
            if (!resolveKey(base, key, raw, sizeof raw)) { misses++; continue; }
            misses = 0;
            any    = true;
            if (id >= KW_CIRCUS_LO && id < KW_CIRCUS_HI && !g_kwCircus) continue;
            bool excluded = false;
            for (int x = 0; x < (int)(sizeof kKwExcluded / sizeof kKwExcluded[0]); x++)
                if (kKwExcluded[x] == id) excluded = true;
            if (excluded) continue;
            if (g_kwCount >= KW_MAX_TERMS) {
                logLine("keywords: term %d would be the %dth, over the cap -- not cached", id, g_kwCount + 1);
                continue;
            }
            char term[KW_TERM_SZ];
            stripMarkup(raw, term, sizeof term);
            kwTrimEnd(term, ": \t");
            _snprintf(key, sizeof key, "str_glossary_term_definition_%d", id);
            key[sizeof key - 1] = 0;
            if (!resolveKey(base, key, raw, sizeof raw)) {
                logLine("keywords: term %d \"%s\" has no definition -- skipped", id, term);
                continue;
            }
            char def[KW_DEF_SZ];
            stripMarkup(raw, def, sizeof def);
            kwTrimEnd(def, " \t");
            if (!term[0] || !def[0]) continue;
            KwTerm& t = g_kwTerms[g_kwCount++];
            t.id = id;
            strncpy(t.term, term, sizeof t.term - 1); t.term[sizeof t.term - 1] = 0;
            strncpy(t.def,  def,  sizeof t.def  - 1); t.def [sizeof t.def  - 1] = 0;
            t.matchLen = (int)strlen(t.term);
            const char* paren = strstr(t.term, " (");
            if (paren && paren > t.term && t.term[t.matchLen - 1] == ')') {
                int m = (int)(paren - t.term);
                while (m > 0 && t.term[m - 1] == ' ') m--;
                if (m > 0) t.matchLen = m;
            }
        }
        emptyBlocks = any ? 0 : emptyBlocks + 1;
    }
    logLine("keywords: %d glossary terms cached from the resolver (circus=%d)", g_kwCount, g_kwCircus ? 1 : 0);
}

static void kwEnsure() {
    if (g_kwCount < 0 || riInCircus(g_base) != g_kwCircus) kwBuild(g_base);
}

// ---- THE SCAN ----
static size_t kwMatchAt(const unsigned char* s, size_t i, const unsigned char* sp, size_t n) {
    size_t m = axWordMatchAt(s, i, sp, n);
    if (m) return m;
    for (size_t k = 0; k < n; k++)                   // the spelling itself, case-folded like the helper
        if (axAsciiLower(s[i + k]) != axAsciiLower(sp[k])) return 0;   // a NUL in s mismatches
    unsigned char a = axAsciiLower(s[i + n]);
    if (a == 's' && !axWordCp(s[i + n + 1])) return n + 1;
    if (a == 'e' && axAsciiLower(s[i + n + 1]) == 's' && !axWordCp(s[i + n + 2])) return n + 2;
    return 0;
}

static int kwScanText(const char* text, int* hits, int nHits) {
    const unsigned char* s = reinterpret_cast<const unsigned char*>(text);
    for (size_t i = 0; s[i]; ) {
        int    best    = -1;
        size_t bestLen = 0;
        if (!axWordBefore(s, i)) {
            for (int t = 0; t < g_kwCount; t++) {
                size_t m = kwMatchAt(s, i, reinterpret_cast<const unsigned char*>(g_kwTerms[t].term),
                                     (size_t)g_kwTerms[t].matchLen);
                if (m > bestLen) { bestLen = m; best = t; }
            }
        }
        if (best < 0) { i++; continue; }
        bool seen = false;
        for (int h = 0; h < nHits && !seen; h++) seen = (hits[h] == best);
        if (!seen && nHits < KW_MAX_HITS) hits[nHits++] = best;
        i += bestLen;
    }
    return nHits;
}

static int kwHitsForLines(const char* lines, int stride, int n, int from, const char* extra, int* hits) {
    kwEnsure();
    int nh = 0;
    if (extra && extra[0]) nh = kwScanText(extra, hits, nh);
    for (int i = (from < 0 ? 0 : from); i < n; i++)
        nh = kwScanText(lines + (size_t)i * (size_t)stride, hits, nh);
    return nh;
}

bool axKeywordTerm(int id, char* out, int outsz) {
    if (!out || outsz <= 0) return false;
    out[0] = 0;
    kwEnsure();
    for (int t = 0; t < g_kwCount; t++)
        if (g_kwTerms[t].id == id) {
            _snprintf(out, outsz, "%.*s", g_kwTerms[t].matchLen, g_kwTerms[t].term);
            out[outsz - 1] = 0;
            return out[0] != 0;
        }
    return false;
}

// Where a definition's first sentence ends: . ! ? then a space or the end, or 。！？.
static size_t kwFirstSentenceEnd(const char* d) {
    const unsigned char* s = reinterpret_cast<const unsigned char*>(d);
    for (size_t i = 0; s[i]; i++) {
        if ((s[i] == '.' || s[i] == '!' || s[i] == '?') && (s[i + 1] == ' ' || !s[i + 1])) return i + 1;
        if (s[i] == 0xE3 && s[i + 1] == 0x80 && s[i + 2] == 0x82) return i + 3;
        if (s[i] == 0xEF && s[i + 1] == 0xBC && (s[i + 2] == 0x81 || s[i + 2] == 0x9F)) return i + 3;
    }
    return strlen(d);
}

static void kwLine(int t, char* out, int outsz) {
    // An abbreviation's own entry explains it ("HP: Short for "health points". ..."): the term
    // and that first sentence are fenced from Expand abbreviations, or the line would say
    // "health points: Short for health points". The rest of every definition is expanded.
    const KwTerm& k = g_kwTerms[t];
    char term[KW_TERM_SZ + 4], def[KW_DEF_SZ + 4];
    _snprintf(term, sizeof term, "%c%s%c", AX_NOEXPAND_OPEN, k.term, AX_NOEXPAND_CLOSE);
    term[sizeof term - 1] = 0;
    if (axIsAbbreviation(k.term)) {
        size_t e = kwFirstSentenceEnd(k.def);
        _snprintf(def, sizeof def, "%c%.*s%c%s", AX_NOEXPAND_OPEN, (int)e, k.def, AX_NOEXPAND_CLOSE, k.def + e);
    } else {
        _snprintf(def, sizeof def, "%s", k.def);
    }
    def[sizeof def - 1] = 0;
    _snprintf(out, outsz, axs(AXS_KW_DEF_FMT), term, def);
    out[outsz - 1] = 0;
}

int axTipKeywordCount(const char* lines, int stride, int n, int from, const char* extra) {
    if (axKeywordsMode() == AX_KEYWORDS_OFF || !lines || stride <= 0 || n <= 0) return 0;
    int hits[KW_MAX_HITS];
    return kwHitsForLines(lines, stride, n, from, extra, hits);
}

bool axTipKeywordLine(const char* lines, int stride, int n, int from, const char* extra, int k,
                      char* out, int outsz) {
    if (!out || outsz <= 0) return false;
    out[0] = 0;
    if (axKeywordsMode() == AX_KEYWORDS_OFF || !lines || stride <= 0 || n <= 0 || k < 0) return false;
    int hits[KW_MAX_HITS];
    int nh = kwHitsForLines(lines, stride, n, from, extra, hits);
    if (k >= nh) return false;
    kwLine(hits[k], out, outsz);
    return true;
}

// ---- THE LANDING'S PENDING SET ----
static int g_kwPend[KW_MAX_HITS];
static int g_kwPendN = 0;

void axTipNoteKeywords(const char* text) {
    if (!text || !text[0]) return;
    if (axKeywordsMode() != AX_KEYWORDS_SPOKEN) return;   // the buffer half scans on its own
    if (g_fdComposingLabel) return;                       // a label's composer may note and never flush
    kwEnsure();
    g_kwPendN = kwScanText(text, g_kwPend, g_kwPendN);
}

void axTipGlueKeywords(char* buf, int bufsz) {
    int nh = g_kwPendN;
    g_kwPendN = 0;
    if (!buf || bufsz <= 0 || nh <= 0) return;
    if (axKeywordsMode() != AX_KEYWORDS_SPOKEN) return;
    if (g_fdComposingLabel) return;                 // a search label never carries a definition
    for (int h = 0; h < nh; h++) {
        char line[KW_TERM_SZ + KW_DEF_SZ + 12];
        kwLine(g_kwPend[h], line, sizeof line);
        axTipGlueKeywordLine(buf, bufsz, line);     // the glue that notes nothing back
    }
    logLine("keywords: %d definition(s) glued onto the landing", nh);
}
