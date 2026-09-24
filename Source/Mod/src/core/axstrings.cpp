// core/axstrings.cpp -- the mod's own localization layer, for the strings the GAME cannot say.

#include <cstring>
#include <cstdlib>
#include "internal.h"

// ---- The two tables ----
static const char* kAxEnglish[AXS__COUNT] = {
#define AXS(id, en) en,
#include "core/axstrings.inc"
#undef AXS
};
static const char* kAxKeyName[AXS__COUNT] = {
#define AXS(id, en) #id,
#include "core/axstrings.inc"
#undef AXS
};
struct AxEmbeddedLang { const char* name; const unsigned char* data; int size; };
#include "core/axlang_embedded.inc"
static char* g_axTr[AXS__COUNT] = { 0 };
static char g_axLang[64] = { 0 };

const char* axs(AxStrId id) {
    if (id < 0 || id >= AXS__COUNT) return "";      // impossible via the enum; belt-and-braces
    return g_axTr[id] ? g_axTr[id] : kAxEnglish[id];
}

const char* axsKeyName(AxStrId id) {
    if (id < 0 || id >= AXS__COUNT) return "";      // -1 is trkPickCancel's "silent" sentinel
    return kAxKeyName[id];
}

// ---- Format-specifier guard ----
static void axFmtSig(const char* s, char* sig, int sigsz) {
    int n = 0;
    for (const char* p = s; *p && n < sigsz - 1; p++) {
        if (*p != '%') continue;
        p++;
        if (*p == '%') continue;                    // "%%" is a literal percent, not a slot
        while (*p && strchr("-+ #.0123456789lhzjt", *p)) p++;
        if (!*p) break;                             // trailing lone '%': contributes nothing
        sig[n++] = *p;
    }
    sig[n] = 0;
}

// ---- Loading ----
static void axClearTable() {
    for (int i = 0; i < AXS__COUNT; i++) {
        free(g_axTr[i]);
        g_axTr[i] = nullptr;
    }
}

static void axLoadLanguage(const char* name) {
    axClearTable();
    if (!name[0] || _stricmp(name, "english") == 0) {
        logLine("axlang: \"%s\" -> built-in English", name);
        return;
    }
    const AxEmbeddedLang* lang = nullptr;
    for (int i = 0; i < kAxEmbeddedLangCount; i++)
        if (_stricmp(name, kAxEmbeddedLangs[i].name) == 0) { lang = &kAxEmbeddedLangs[i]; break; }
    if (!lang) {
        logLine("axlang: no embedded translation for \"%s\" -- staying English", name);
        return;
    }
    int loaded = 0, rejected = 0, unknown = 0;
    const char* data = (const char*)lang->data;
    const int size = lang->size;
    int pos = 0;
    char line[1024];
    bool first = true;
    while (pos < size) {
        int start = pos;
        while (pos < size && data[pos] != '\n') pos++;
        int lineLen = pos - start;
        if (pos < size) pos++;                       // step over the '\n' itself
        if (lineLen > (int)sizeof line - 1) lineLen = (int)sizeof line - 1;
        memcpy(line, data + start, lineLen);
        line[lineLen] = 0;
        char* p = line;
        if (first) {                                 // strip a UTF-8 BOM if the editor left one
            first = false;
            if ((unsigned char)p[0] == 0xEF && (unsigned char)p[1] == 0xBB &&
                (unsigned char)p[2] == 0xBF) p += 3;
        }
        // Trim the line ending and trailing whitespace.
        size_t len = strlen(p);
        while (len && (p[len-1] == '\n' || p[len-1] == '\r' ||
                       p[len-1] == ' '  || p[len-1] == '\t')) p[--len] = 0;
        while (*p == ' ' || *p == '\t') p++;
        if (!*p || *p == '#' || *p == ';') continue;
        char* eq = strchr(p, '=');
        if (!eq) continue;
        *eq = 0;
        char* key = p;
        char* val = eq + 1;
        char* ke = eq;
        while (ke > key && (ke[-1] == ' ' || ke[-1] == '\t')) *--ke = 0;
        while (*val == ' ' || *val == '\t') val++;
        if (!*val) continue;                         // an empty value would mean a silent line
        int idx = -1;
        for (int i = 0; i < AXS__COUNT; i++)
            if (strcmp(key, kAxKeyName[i]) == 0) { idx = i; break; }
        if (idx < 0) {
            unknown++;
            logLine("axlang: %s: unknown key \"%s\"", name, key);
            continue;
        }
        char sigEn[32], sigTr[32];
        axFmtSig(kAxEnglish[idx], sigEn, sizeof sigEn);
        axFmtSig(val, sigTr, sizeof sigTr);
        if (strcmp(sigEn, sigTr) != 0) {
            rejected++;
            logLine("axlang: %s: %s REJECTED, format \"%s\" != English \"%s\"",
                    name, key, sigTr, sigEn);
            continue;
        }
        free(g_axTr[idx]);
        g_axTr[idx] = _strdup(val);
        loaded++;
    }
    logLine("axlang: \"%s\" loaded %d/%d entries (%d rejected, %d unknown keys)",
            name, loaded, (int)AXS__COUNT, rejected, unknown);
}

// ---- The applied-language read: ONCE PER SESSION, then latched ----
void axLangService(uintptr_t base) {
    static bool latched = false;
    static DWORD nextAt = 0;
    if (latched) return;
    DWORD now = GetTickCount();
    if ((long)(now - nextAt) < 0) return;
    nextAt = now + 1000;
    char lang[64];
    if (!optSavedLanguage(base, lang, sizeof lang)) return;
    latched = true;
    logLine("axlang: session language \"%s\" (latched -- a mid-session change applies at "
            "next launch, like the game's own text)", lang);
    strncpy(g_axLang, lang, sizeof g_axLang - 1);
    g_axLang[sizeof g_axLang - 1] = 0;
    axLoadLanguage(lang);
    axSubtitlesLangDefault(lang);
}
