// core/speech.cpp — the speech side of the two-thread design

#include <windows.h>
#include <cstdio>
#include <cstring>
#include <cstdarg>
#include <cstdlib>
#include "prism.h"
#include "internal.h"

// ---- THE DEBUG LOG: one file, one switch, OFF by default ----
static CRITICAL_SECTION g_logCs;
static CRITICAL_SECTION g_speechCfgCs;

// A settings.ini string value: strip the line end and clamp into `out`.
static void iniCopyValue(char* out, int outsz, const char* p) {
    int n = 0;
    while (p[n] && p[n] != '\r' && p[n] != '\n' && n < outsz - 1) { out[n] = p[n]; n++; }
    out[n] = 0;
}
struct AxDebugLogState {
    char path[MAX_PATH];
    char ini[MAX_PATH];
    volatile bool enabled;
    volatile bool readSubtitles;
    volatile bool readSubtitlesExplicit;
    volatile int  barks;
    bool barksExplicit;
    volatile bool checkUpdates;
    int  speechHandler;
    char prismBackend[64];
    char sapiVoice[128];
    int  sapiRate;
    int  sapiVolume;
    AxDebugLogState() : enabled(false), readSubtitles(true), readSubtitlesExplicit(false),
                        barks(AX_BARKS_ON), barksExplicit(false), checkUpdates(true),
                        speechHandler(AX_SPEECH_AUTO), sapiRate(-1), sapiVolume(-1) {
        InitializeCriticalSection(&g_logCs);
        InitializeCriticalSection(&g_speechCfgCs);
        prismBackend[0] = 0;
        sapiVoice[0] = 0;
        path[0] = 0;
        ini[0] = 0;
        char exe[MAX_PATH] = "";
        if (GetModuleFileNameA(nullptr, exe, sizeof exe) > 0) {
            char* slash = strrchr(exe, '\\');
            if (slash) {
                *slash = 0;
                _snprintf(path, sizeof path, "%s\\ddaccess-debug.log", exe);
                path[sizeof path - 1] = 0;
            }
        }
        const char* local = getenv("LOCALAPPDATA");
        if (local && *local) {
            char dir[MAX_PATH];
            _snprintf(dir, sizeof dir, "%s\\DarkestAccess", local);
            dir[sizeof dir - 1] = 0;
            CreateDirectoryA(dir, nullptr);   // harmless if it already exists
            _snprintf(ini, sizeof ini, "%s\\settings.ini", dir);
            ini[sizeof ini - 1] = 0;
            FILE* f = fopen(ini, "r");
            if (f) {
                char line[256];
                while (fgets(line, sizeof line, f)) {
                    char* p = line;
                    while (*p == ' ' || *p == '\t') p++;
                    if (strncmp(p, "debug_log=", 10) == 0)           enabled      = (p[10] == '1');
                    else if (strncmp(p, "read_subtitles=", 15) == 0) {
                        if (p[15] == '0')      { readSubtitles = false; readSubtitlesExplicit = true; }
                        else if (p[15] == '2') { readSubtitles = true;  readSubtitlesExplicit = true; }
                    }
                    else if (strncmp(p, "barks=", 6) == 0) {
                        int v = p[6] - '0';
                        if (v >= 0 && v < AX_BARKS_MODES) { barks = v; barksExplicit = true; }
                    }
                    else if (strncmp(p, "read_town_barks=", 16) == 0) {
                        if (!barksExplicit) barks = (p[16] == '1') ? AX_BARKS_ON : AX_BARKS_DUNGEON;
                    }
                    else if (strncmp(p, "check_updates=", 14) == 0)   checkUpdates  = (p[14] != '0');
                    else if (strncmp(p, "speech_handler=", 15) == 0) {
                        int v = p[15] - '0';
                        if (v >= 0 && v < AX_SPEECH_MODES) speechHandler = v;
                    }
                    else if (strncmp(p, "prism_backend=", 14) == 0) iniCopyValue(prismBackend, sizeof prismBackend, p + 14);
                    else if (strncmp(p, "sapi_voice=", 11) == 0)    iniCopyValue(sapiVoice, sizeof sapiVoice, p + 11);
                    else if (strncmp(p, "sapi_rate=", 10) == 0) {
                        int v = atoi(p + 10);
                        if (v >= 0 && v <= 100) sapiRate = v;
                    }
                    else if (strncmp(p, "sapi_volume=", 12) == 0) {
                        int v = atoi(p + 12);
                        if (v >= 0 && v <= 100) sapiVolume = v;
                    }
                }
                fclose(f);
            }
        }
    }
};
static AxDebugLogState& axDebugState() { static AxDebugLogState s; return s; }

static const long  LOG_CAP_BYTES = 64 * 1024 * 1024;

bool axDebugLogEnabled() { return axDebugState().enabled; }
const char* axDebugLogPath() { return axDebugState().path; }

static void axSaveIni() {
    AxDebugLogState& s = axDebugState();
    if (!s.ini[0]) return;
    FILE* f = fopen(s.ini, "w");
    if (!f) return;
    fprintf(f, "# DarkestAccess mod settings (the F10 menu writes this file).\n");
    fprintf(f, "debug_log=%d\n", s.enabled ? 1 : 0);
    if (s.readSubtitlesExplicit)
        fprintf(f, "read_subtitles=%d\n", s.readSubtitles ? 2 : 0);
    fprintf(f, "barks=%d\n", (int)s.barks);
    fprintf(f, "check_updates=%d\n", s.checkUpdates ? 1 : 0);
    EnterCriticalSection(&g_speechCfgCs);
    fprintf(f, "speech_handler=%d\n", s.speechHandler);
    fprintf(f, "prism_backend=%s\n", s.prismBackend);
    fprintf(f, "sapi_voice=%s\n", s.sapiVoice);
    if (s.sapiRate   >= 0) fprintf(f, "sapi_rate=%d\n",   s.sapiRate);
    if (s.sapiVolume >= 0) fprintf(f, "sapi_volume=%d\n", s.sapiVolume);
    LeaveCriticalSection(&g_speechCfgCs);
    fclose(f);
}

// ---- Speech settings ----
bool axReadSubtitles() { return axDebugState().readSubtitles; }
void axSetReadSubtitles(bool on) {
    axDebugState().readSubtitles = on;
    axDebugState().readSubtitlesExplicit = true;   // a menu toggle IS a choice; the language
                                                   // default never overrides it again
    axSaveIni();
}

void axSubtitlesLangDefault(const char* lang) {
    AxDebugLogState& s = axDebugState();
    if (s.readSubtitlesExplicit) return;
    s.readSubtitles = (strcmp(lang, "english") != 0);
    logLine("subtitles: no explicit setting saved; language \"%s\" defaults reading %s",
            lang, s.readSubtitles ? "on" : "off");
}
int  axBarksMode() { return axDebugState().barks; }
void axSetBarksMode(int mode) {
    if (mode < 0 || mode >= AX_BARKS_MODES) { logLine("barks: mode %d refused", mode); return; }
    axDebugState().barks = mode;
    axDebugState().barksExplicit = true;
    axSaveIni();
}
bool axReadTownBarks()    { int m = axBarksMode(); return m == AX_BARKS_ON || m == AX_BARKS_TOWN; }
bool axReadDungeonBarks() { int m = axBarksMode(); return m == AX_BARKS_ON || m == AX_BARKS_DUNGEON; }
bool axCheckUpdates() { return axDebugState().checkUpdates; }
void axSetCheckUpdates(bool on) {
    axDebugState().checkUpdates = on;
    axSaveIni();
}

// ---- Speech OUTPUT settings ----
static volatile int g_speechCfgReq = 0;           // AX_SPCFG_* pending, 0 = none
static char g_speechCfgOkFmt[256]   = "";         // "%s" = the backend now speaking
static char g_speechCfgFailFmt[256] = "";         // "%s %s" = wanted, then what speaks instead

static void axSpeechRequest(int kind, const char* okFmt, const char* failFmt) {
    EnterCriticalSection(&g_speechCfgCs);
    if (kind > g_speechCfgReq) g_speechCfgReq = kind;
    _snprintf(g_speechCfgOkFmt,   sizeof g_speechCfgOkFmt,   "%s", okFmt   ? okFmt   : "");
    _snprintf(g_speechCfgFailFmt, sizeof g_speechCfgFailFmt, "%s", failFmt ? failFmt : "");
    g_speechCfgOkFmt[sizeof g_speechCfgOkFmt - 1] = 0;
    g_speechCfgFailFmt[sizeof g_speechCfgFailFmt - 1] = 0;
    LeaveCriticalSection(&g_speechCfgCs);
}

int  axSpeechHandler() { return axDebugState().speechHandler; }
void axSetSpeechHandler(int mode, const char* okFmt, const char* failFmt) {
    if (mode < 0 || mode >= AX_SPEECH_MODES) { logLine("speech: handler %d refused", mode); return; }
    EnterCriticalSection(&g_speechCfgCs);
    axDebugState().speechHandler = mode;
    LeaveCriticalSection(&g_speechCfgCs);
    axSaveIni();
    axSpeechRequest(AX_SPCFG_BACKEND, okFmt, failFmt);
}
void axPrismBackendSetting(char* out, int outsz) {
    EnterCriticalSection(&g_speechCfgCs);
    _snprintf(out, outsz, "%s", axDebugState().prismBackend);
    LeaveCriticalSection(&g_speechCfgCs);
    out[outsz - 1] = 0;
}
void axSetPrismBackend(const char* name, const char* okFmt, const char* failFmt) {
    AxDebugLogState& s = axDebugState();
    EnterCriticalSection(&g_speechCfgCs);
    _snprintf(s.prismBackend, sizeof s.prismBackend, "%s", name ? name : "");
    s.prismBackend[sizeof s.prismBackend - 1] = 0;
    LeaveCriticalSection(&g_speechCfgCs);
    axSaveIni();
    if (s.speechHandler == AX_SPEECH_PRISM) axSpeechRequest(AX_SPCFG_BACKEND, okFmt, failFmt);
}
void axSapiVoiceSetting(char* out, int outsz) {
    EnterCriticalSection(&g_speechCfgCs);
    _snprintf(out, outsz, "%s", axDebugState().sapiVoice);
    LeaveCriticalSection(&g_speechCfgCs);
    out[outsz - 1] = 0;
}
void axSetSapiVoice(const char* name) {
    AxDebugLogState& s = axDebugState();
    EnterCriticalSection(&g_speechCfgCs);
    _snprintf(s.sapiVoice, sizeof s.sapiVoice, "%s", name ? name : "");
    s.sapiVoice[sizeof s.sapiVoice - 1] = 0;
    LeaveCriticalSection(&g_speechCfgCs);
    axSaveIni();
    axSpeechRequest(AX_SPCFG_SAPI, nullptr, nullptr);
}
int  axSapiRateSetting()   { return axDebugState().sapiRate; }
int  axSapiVolumeSetting() { return axDebugState().sapiVolume; }
void axSetSapiRate(int pct) {
    if (pct < 0 || pct > 100) { logLine("speech: sapi rate %d refused", pct); return; }
    EnterCriticalSection(&g_speechCfgCs);
    axDebugState().sapiRate = pct;
    LeaveCriticalSection(&g_speechCfgCs);
    axSaveIni();
    axSpeechRequest(AX_SPCFG_SAPI, nullptr, nullptr);
}
void axSetSapiVolume(int pct) {
    if (pct < 0 || pct > 100) { logLine("speech: sapi volume %d refused", pct); return; }
    EnterCriticalSection(&g_speechCfgCs);
    axDebugState().sapiVolume = pct;
    LeaveCriticalSection(&g_speechCfgCs);
    axSaveIni();
    axSpeechRequest(AX_SPCFG_SAPI, nullptr, nullptr);
}

static bool appendLogLine(const char* path, long cap, const char* line);
static void axCloseLogFile();
static void axTruncateLogFile(const char* path);

bool axSetDebugLog(bool on) {
    AxDebugLogState& s = axDebugState();
    if (on) {
        axTruncateLogFile(s.path);
        char head[160];
        _snprintf(head, sizeof head, "=== ddaccess debug log ON (build %s %s) ===", __DATE__, __TIME__);
        head[sizeof head - 1] = 0;
        if (!appendLogLine(s.path, LOG_CAP_BYTES, head)) { s.enabled = false; axSaveIni(); return false; }
        s.enabled = true;
    } else {
        if (s.enabled) appendLogLine(s.path, LOG_CAP_BYTES, "=== ddaccess debug log OFF ===");
        s.enabled = false;
        axCloseLogFile();
    }
    axSaveIni();
    return true;
}

CRITICAL_SECTION g_cs;

const char* speechKindName(SpeechKind k) {
    return k == SPK_EVENT   ? "event"
         : k == SPK_AMBIENT ? "ambient"
         : k == SPK_CHATTER ? "chatter"
         : "nav";
}

char g_speechQ[SPEECH_QUEUE_MAX][MAILBOX_SZ];
SpeechKind g_sqKind[SPEECH_QUEUE_MAX];     // per line: nav (droppable) or event (never)
int  g_sqHead = 0;
int  g_sqCount = 0;
volatile bool g_hasPending = false;

char g_barkCarry[BARK_CARRY_MAX][MAILBOX_SZ];
int  g_barkCarryN = 0;

DWORD      g_speakUntil = 0;               // estimated end of the line the reader is speaking
SpeechKind g_lastSpokenKind = SPK_NAV;     // what that line was (only events are protected)
bool       g_holdLogged = false;           // one log line per hold, not per 20 ms tick

char  g_lastNavLine[MAILBOX_SZ] = "";
DWORD g_lastNavTick = 0;

// ---- Speech + logging ----
// ---- PRISM, the speech library ----
static PrismContext* g_prismCtx     = nullptr;
static PrismBackend* g_prismBackend = nullptr;

// ---- The registry's backend NAMES, for the F10 "Prism backend" row ----
static const int AX_PRISM_NAMES_MAX = 24;
static char g_prismNames[AX_PRISM_NAMES_MAX][32];
static int  g_prismNameCount = 0;

int axPrismBackendCount() { return g_prismNameCount; }
const char* axPrismBackendName(int i) {
    return (i >= 0 && i < g_prismNameCount) ? g_prismNames[i] : "";
}

// ---- The SAPI snapshot, for the F10 "SAPI configuration" rows ----
struct AxSapiSnapshot {
    bool live;
    int  voiceCount;
    char voiceName[AX_SAPI_VOICES_MAX][128];
    int  voiceIdx;              // the voice SAPI reports as current, -1 unknown
    int  ratePct;
    int  volumePct;             // 0..100, -1 unknown
};
static AxSapiSnapshot g_sapi = { false, 0, {{0}}, -1, -1, -1 };

bool axSapiLive() {
    EnterCriticalSection(&g_speechCfgCs); bool v = g_sapi.live; LeaveCriticalSection(&g_speechCfgCs);
    return v;
}
int axSapiVoiceCount() {
    EnterCriticalSection(&g_speechCfgCs); int v = g_sapi.live ? g_sapi.voiceCount : 0; LeaveCriticalSection(&g_speechCfgCs);
    return v;
}
void axSapiVoiceName(int i, char* out, int outsz) {
    EnterCriticalSection(&g_speechCfgCs);
    _snprintf(out, outsz, "%s", (i >= 0 && i < g_sapi.voiceCount) ? g_sapi.voiceName[i] : "");
    LeaveCriticalSection(&g_speechCfgCs);
    out[outsz - 1] = 0;
}
int axSapiLiveVoice()  { EnterCriticalSection(&g_speechCfgCs); int v = g_sapi.voiceIdx;  LeaveCriticalSection(&g_speechCfgCs); return v; }
int axSapiLiveRate()   { EnterCriticalSection(&g_speechCfgCs); int v = g_sapi.ratePct;   LeaveCriticalSection(&g_speechCfgCs); return v; }
int axSapiLiveVolume() { EnterCriticalSection(&g_speechCfgCs); int v = g_sapi.volumePct; LeaveCriticalSection(&g_speechCfgCs); return v; }

static bool prismBackendIsSapi() {
    return g_prismBackend && strcmp(prism_backend_name(g_prismBackend), "SAPI") == 0;
}

static void prismApplySapiSettings() {
    AxDebugLogState& s = axDebugState();
    if (!prismBackendIsSapi()) {
        EnterCriticalSection(&g_speechCfgCs);
        g_sapi.live = false; g_sapi.voiceCount = 0; g_sapi.voiceIdx = -1;
        g_sapi.ratePct = -1; g_sapi.volumePct = -1;
        LeaveCriticalSection(&g_speechCfgCs);
        return;
    }
    uint64_t f = prism_backend_get_features(g_prismBackend);
    char wantVoice[128]; int wantRate, wantVol;
    EnterCriticalSection(&g_speechCfgCs);
    _snprintf(wantVoice, sizeof wantVoice, "%s", s.sapiVoice); wantVoice[sizeof wantVoice - 1] = 0;
    wantRate = s.sapiRate; wantVol = s.sapiVolume;
    LeaveCriticalSection(&g_speechCfgCs);

    AxSapiSnapshot snap; snap.live = true; snap.voiceCount = 0; snap.voiceIdx = -1;
    snap.ratePct = -1; snap.volumePct = -1;
    int wantIdx = -1;
    if ((f & PRISM_BACKEND_SUPPORTS_COUNT_VOICES) && (f & PRISM_BACKEND_SUPPORTS_GET_VOICE_NAME)) {
        size_t n = 0;
        if (prism_backend_count_voices(g_prismBackend, &n) == PRISM_OK) {
            if ((int)n > AX_SAPI_VOICES_MAX) {
                logLine("speech: sapi lists %d voices, menu shows the first %d", (int)n, AX_SAPI_VOICES_MAX);
                n = AX_SAPI_VOICES_MAX;
            }
            for (size_t i = 0; i < n; i++) {
                const char* nm = nullptr;
                if (prism_backend_get_voice_name(g_prismBackend, i, &nm) != PRISM_OK || !nm) continue;
                _snprintf(snap.voiceName[snap.voiceCount], sizeof snap.voiceName[0], "%s", nm);
                snap.voiceName[snap.voiceCount][sizeof snap.voiceName[0] - 1] = 0;
                if (wantVoice[0] && strcmp(nm, wantVoice) == 0) wantIdx = snap.voiceCount;
                snap.voiceCount++;
            }
        }
    }
    if (wantVoice[0] && wantIdx < 0)
        logLine("speech: sapi voice \"%s\" is not installed (%d listed); SAPI keeps its current voice",
                wantVoice, snap.voiceCount);
    if (wantIdx >= 0 && (f & PRISM_BACKEND_SUPPORTS_SET_VOICE)) {
        PrismError e = prism_backend_set_voice(g_prismBackend, (size_t)wantIdx);
        if (e != PRISM_OK) logLine("speech: sapi set_voice(%d \"%s\") failed: %s", wantIdx, wantVoice, prism_error_string(e));
    }
    if (wantRate >= 0 && (f & PRISM_BACKEND_SUPPORTS_SET_RATE)) {
        PrismError e = prism_backend_set_rate(g_prismBackend, (float)wantRate / 100.0f);
        if (e != PRISM_OK) logLine("speech: sapi set_rate(%d%%) failed: %s", wantRate, prism_error_string(e));
    }
    if (wantVol >= 0 && (f & PRISM_BACKEND_SUPPORTS_SET_VOLUME)) {
        PrismError e = prism_backend_set_volume(g_prismBackend, (float)wantVol / 100.0f);
        if (e != PRISM_OK) logLine("speech: sapi set_volume(%d%%) failed: %s", wantVol, prism_error_string(e));
    }
    // Read back what SAPI is actually using.
    if (f & PRISM_BACKEND_SUPPORTS_GET_VOICE) {
        size_t cur = 0;
        if (prism_backend_get_voice(g_prismBackend, &cur) == PRISM_OK && (int)cur < snap.voiceCount)
            snap.voiceIdx = (int)cur;
    }
    float v = 0.0f;
    if ((f & PRISM_BACKEND_SUPPORTS_GET_RATE) && prism_backend_get_rate(g_prismBackend, &v) == PRISM_OK)
        snap.ratePct = (int)(v * 100.0f + 0.5f);
    if ((f & PRISM_BACKEND_SUPPORTS_GET_VOLUME) && prism_backend_get_volume(g_prismBackend, &v) == PRISM_OK)
        snap.volumePct = (int)(v * 100.0f + 0.5f);
    logLine("speech: sapi live: %d voices, voice=%d (\"%s\"), rate=%d%%, volume=%d%%",
            snap.voiceCount, snap.voiceIdx,
            snap.voiceIdx >= 0 ? snap.voiceName[snap.voiceIdx] : "?", snap.ratePct, snap.volumePct);
    EnterCriticalSection(&g_speechCfgCs);
    g_sapi = snap;
    LeaveCriticalSection(&g_speechCfgCs);
}

static PrismBackend* prismTryBackend(PrismBackend* b, const char* what, PrismError* err) {
    if (!b) { *err = PRISM_ERROR_BACKEND_NOT_AVAILABLE; logLine("speech: %s: nothing to create", what); return nullptr; }
    PrismError e = prism_backend_initialize(b);
    if (e != PRISM_OK && e != PRISM_ERROR_ALREADY_INITIALIZED) {
        logLine("speech: %s: backend '%s' failed to initialize: %s", what, prism_backend_name(b), prism_error_string(e));
        prism_backend_free(b);
        *err = e;
        return nullptr;
    }
    *err = PRISM_OK;
    return b;
}

static bool prismBringUp(char* wanted, int wantedsz, bool* fellBack) {
    AxDebugLogState& s = axDebugState();
    int handler; char backend[64];
    EnterCriticalSection(&g_speechCfgCs);
    handler = s.speechHandler;
    _snprintf(backend, sizeof backend, "%s", s.prismBackend); backend[sizeof backend - 1] = 0;
    LeaveCriticalSection(&g_speechCfgCs);

    if (g_prismBackend) { prism_backend_free(g_prismBackend); g_prismBackend = nullptr; }
    *fellBack = false;
    wanted[0] = 0;
    PrismError e = PRISM_OK;
    if (handler == AX_SPEECH_SAPI) {
        _snprintf(wanted, wantedsz, "SAPI"); wanted[wantedsz - 1] = 0;
        g_prismBackend = prismTryBackend(prism_registry_create(g_prismCtx, PRISM_BACKEND_SAPI), "forced SAPI", &e);
    } else if (handler == AX_SPEECH_PRISM && backend[0]) {
        _snprintf(wanted, wantedsz, "%s", backend); wanted[wantedsz - 1] = 0;
        PrismBackendId id = prism_registry_id(g_prismCtx, backend);
        if (id == PRISM_BACKEND_INVALID) {
            logLine("speech: prism backend \"%s\" is not in this PRISM's registry", backend);
        } else {
            g_prismBackend = prismTryBackend(prism_registry_create(g_prismCtx, id), "chosen backend", &e);
        }
    }
    if (!g_prismBackend) {
        if (wanted[0]) *fellBack = true;
        g_prismBackend = prismTryBackend(prism_registry_create_best(g_prismCtx), "best backend", &e);
    }
    if (!g_prismBackend) {
        logLine("speech: NO backend could be started -- no speech");
        prismApplySapiSettings();   // clears the snapshot
        return false;
    }
    logLine("speech: prism %s up, backend=%s features=0x%llx (handler=%d wanted=\"%s\" fell_back=%d)",
            prism_version_string(), prism_backend_name(g_prismBackend),
            (unsigned long long)prism_backend_get_features(g_prismBackend),
            handler, wanted, *fellBack ? 1 : 0);
    prismApplySapiSettings();
    return true;
}

bool axSpeechInit() {
    PrismConfig cfg = prism_config_init();
    g_prismCtx = prism_init(&cfg);
    if (!g_prismCtx) {
        logLine("speech: prism_init FAILED -- no speech this session");
        return false;
    }
    size_t n = prism_registry_count(g_prismCtx);
    g_prismNameCount = 0;
    for (size_t i = 0; i < n && g_prismNameCount < AX_PRISM_NAMES_MAX; i++) {
        const char* nm = prism_registry_name(g_prismCtx, prism_registry_id_at(g_prismCtx, i));
        if (!nm || !*nm) continue;
        _snprintf(g_prismNames[g_prismNameCount], sizeof g_prismNames[0], "%s", nm);
        g_prismNames[g_prismNameCount][sizeof g_prismNames[0] - 1] = 0;
        g_prismNameCount++;
    }
    logLine("speech: prism registry lists %d backends (%d kept)", (int)n, g_prismNameCount);

    char wanted[64]; bool fellBack = false;
    if (!prismBringUp(wanted, sizeof wanted, &fellBack)) {
        prism_shutdown(g_prismCtx); g_prismCtx = nullptr;
        return false;
    }
    return true;
}

void axSpeechServiceConfig() {
    int kind; char okFmt[256], failFmt[256];
    EnterCriticalSection(&g_speechCfgCs);
    kind = g_speechCfgReq;
    g_speechCfgReq = 0;
    memcpy(okFmt, g_speechCfgOkFmt, sizeof okFmt);
    memcpy(failFmt, g_speechCfgFailFmt, sizeof failFmt);
    LeaveCriticalSection(&g_speechCfgCs);
    if (!kind) return;
    if (!g_prismCtx) { logLine("speech: config request %d with no PRISM context -- ignored", kind); return; }

    if (kind == AX_SPCFG_SAPI) {
        // Parameters only: no restart. Nothing to say -- the menu already spoke the row.
        prismApplySapiSettings();
        return;
    }
    char wanted[64]; bool fellBack = false;
    bool up = prismBringUp(wanted, sizeof wanted, &fellBack);
    if (!up) return;
    const char* got = prism_backend_name(g_prismBackend);
    char line[512];
    if (fellBack && failFmt[0])      _snprintf(line, sizeof line, failFmt, wanted, got);
    else if (!fellBack && okFmt[0])  _snprintf(line, sizeof line, okFmt, got);
    else return;
    line[sizeof line - 1] = 0;
    postSpeech(line, /*interrupt=*/false, SPK_EVENT);
}

void axSpeechShutdown() {
    if (g_prismBackend) { prism_backend_free(g_prismBackend); g_prismBackend = nullptr; }
    if (g_prismCtx)     { prism_shutdown(g_prismCtx);         g_prismCtx     = nullptr; }
}

const char* axSpeechReaderName() {
    return g_prismBackend ? prism_backend_name(g_prismBackend) : nullptr;
}

// Does the backend claim a speech channel at all (Tolk_HasSpeech's question).
bool axSpeechHasSpeech() {
    if (!g_prismBackend) return false;
    uint64_t f = prism_backend_get_features(g_prismBackend);
    return (f & (PRISM_BACKEND_SUPPORTS_SPEAK | PRISM_BACKEND_SUPPORTS_OUTPUT)) != 0;
}

bool axSpeechSilence() {
    return g_prismBackend && prism_backend_stop(g_prismBackend) == PRISM_OK;
}

int axSpeechIsSpeaking() {
    if (!g_prismBackend) return -1;
    if (!(prism_backend_get_features(g_prismBackend) & PRISM_BACKEND_SUPPORTS_IS_SPEAKING))
        return -1;
    bool sp = false;
    if (prism_backend_is_speaking(g_prismBackend, &sp) != PRISM_OK) return -1;
    return sp ? 1 : 0;
}

const char* axSpeechErrName(int prismError) {
    return prism_error_string((PrismError)prismError);
}

bool speakUtf8(const char* utf8, bool interrupt, int* outErr) {
    if (!g_prismBackend) {
        if (outErr) *outErr = (int)PRISM_ERROR_NOT_INITIALIZED;
        return false;
    }
    PrismError e = prism_backend_output(g_prismBackend, utf8, interrupt);
    if (e == PRISM_ERROR_INVALID_UTF8) {
        char clean[MAILBOX_SZ]; size_t j = 0;
        for (const char* p = utf8; *p && j + 1 < sizeof clean; ++p)
            clean[j++] = ((unsigned char)*p < 0x80) ? *p : '?';
        clean[j] = 0;
        logLine("speech: line was not valid UTF-8, retrying with non-ASCII bytes replaced \"%.60s\"", clean);
        e = prism_backend_output(g_prismBackend, clean, interrupt);
    }
    if (outErr) *outErr = (int)e;
    return e == PRISM_OK;
}

// ---- The held-key swallow, and why no amount of mod code fixes it ----

static FILE* g_logFile = nullptr;
static long  g_logSize = 0;

static bool appendLogLine(const char* path, long cap, const char* line) {
    if (!path || !path[0]) return false;
    EnterCriticalSection(&g_logCs);
    if (!g_logFile) {
        g_logFile = fopen(path, "a");
        if (!g_logFile) { LeaveCriticalSection(&g_logCs); return false; }
        fseek(g_logFile, 0, SEEK_END);
        g_logSize = ftell(g_logFile);
        if (g_logSize < 0) g_logSize = 0;
    }
    if (g_logSize > cap) {
        fclose(g_logFile);
        g_logFile = fopen(path, "w");
        if (!g_logFile) { LeaveCriticalSection(&g_logCs); return false; }
        g_logSize = 0;
    }
    int n = fprintf(g_logFile, "[%10lu] %s\n", (unsigned long)GetTickCount(), line);
    if (n > 0) g_logSize += n;
    fflush(g_logFile);
    LeaveCriticalSection(&g_logCs);
    return true;
}

static void axCloseLogFile() {
    EnterCriticalSection(&g_logCs);
    if (g_logFile) { fclose(g_logFile); g_logFile = nullptr; }
    LeaveCriticalSection(&g_logCs);
}

static void axTruncateLogFile(const char* path) {
    if (!path || !path[0]) return;
    EnterCriticalSection(&g_logCs);
    if (g_logFile) { fclose(g_logFile); g_logFile = nullptr; }
    FILE* f = fopen(path, "w");
    if (f) fclose(f);
    g_logSize = 0;
    LeaveCriticalSection(&g_logCs);
}

void logLine(const char* fmt, ...) {
    AxDebugLogState& s = axDebugState();
    if (!s.enabled) return;
    char line[2048];
    va_list ap; va_start(ap, fmt);
    _vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    line[sizeof line - 1] = 0;
    appendLogLine(s.path, LOG_CAP_BYTES, line);
}

// ---- One-shot PROBE dumps ----
void logDump(const char* fmt, ...) {
    AxDebugLogState& s = axDebugState();
    if (!s.enabled) return;
    char line[2048];
    va_list ap; va_start(ap, fmt);
    _vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    line[sizeof line - 1] = 0;
    appendLogLine(s.path, LOG_CAP_BYTES, line);
}

void postSpeech(const char* text, bool interrupt, SpeechKind kind) {
    if (kind == SPK_AUTO) kind = interrupt ? SPK_NAV : SPK_EVENT;
    if (kind == SPK_NAV) {
        if (text != g_lastNavLine) {
            strncpy(g_lastNavLine, text, MAILBOX_SZ - 1);
            g_lastNavLine[MAILBOX_SZ - 1] = 0;
        }
        g_lastNavTick = GetTickCount();
    }
    EnterCriticalSection(&g_cs);
    if (kind == SPK_CHATTER) {
        if (g_barkCarryN >= BARK_CARRY_MAX) {
            logLine("speech: bark carry full (%d), dropping the oldest \"%.60s\"",
                    BARK_CARRY_MAX, g_barkCarry[0]);
            memmove(g_barkCarry[0], g_barkCarry[1], (size_t)(BARK_CARRY_MAX - 1) * MAILBOX_SZ);
            g_barkCarryN--;
        }
        strncpy(g_barkCarry[g_barkCarryN], text, MAILBOX_SZ - 1);
        g_barkCarry[g_barkCarryN][MAILBOX_SZ - 1] = 0;
        g_barkCarryN++;
        LeaveCriticalSection(&g_cs);
        return;
    }
    if (kind == SPK_NAV) {
        int nk = 0, dropped = 0;
        for (int i = 0; i < g_sqCount; i++) {
            int r = (g_sqHead + i) % SPEECH_QUEUE_MAX;
            if (g_sqKind[r] != SPK_EVENT) { dropped++; continue; }
            int w = (g_sqHead + nk) % SPEECH_QUEUE_MAX;
            if (w != r) {
                memcpy(g_speechQ[w], g_speechQ[r], MAILBOX_SZ);
                g_sqKind[w]      = g_sqKind[r];
            }
            nk++;
        }
        g_sqCount = nk;
        if (dropped) logLine("speech: nav line dropped %d stale nav/ambient line(s), kept %d event line(s)",
                             dropped, nk);
    } else if (kind == SPK_AMBIENT) {
        int nk = 0, dropped = 0;
        for (int i = 0; i < g_sqCount; i++) {
            int r = (g_sqHead + i) % SPEECH_QUEUE_MAX;
            if (g_sqKind[r] == kind) { dropped++; continue; }
            int w = (g_sqHead + nk) % SPEECH_QUEUE_MAX;
            if (w != r) {
                memcpy(g_speechQ[w], g_speechQ[r], MAILBOX_SZ);
                g_sqKind[w]      = g_sqKind[r];
            }
            nk++;
        }
        g_sqCount = nk;
        if (dropped) logLine("speech: %s line replaced %d unspoken %s line(s)",
                             speechKindName(kind), dropped, speechKindName(kind));
    }
    if (g_sqCount >= SPEECH_QUEUE_MAX) {
        int victim = -1;
        for (int pass = 0; pass < 3 && victim < 0; pass++) {
            SpeechKind want = pass == 0 ? SPK_CHATTER : pass == 1 ? SPK_AMBIENT : SPK_NAV;
            for (int i = 0; i < g_sqCount; i++) {
                int s = (g_sqHead + i) % SPEECH_QUEUE_MAX;
                if (g_sqKind[s] == want) { victim = i; break; }
            }
        }
        if (victim < 0) {
            logLine("speech: queue full (%d) and ALL EVENT — dropping oldest event \"%s\"",
                    SPEECH_QUEUE_MAX, g_speechQ[g_sqHead]);
            victim = 0;
        } else {
            logLine("speech: queue full (%d), dropping oldest %s line", SPEECH_QUEUE_MAX,
                    speechKindName(g_sqKind[(g_sqHead + victim) % SPEECH_QUEUE_MAX]));
        }
        // Close the gap so the ring stays contiguous.
        for (int i = victim; i > 0; i--) {
            int dst = (g_sqHead + i) % SPEECH_QUEUE_MAX;
            int src = (g_sqHead + i - 1) % SPEECH_QUEUE_MAX;
            memcpy(g_speechQ[dst], g_speechQ[src], MAILBOX_SZ);
            g_sqKind[dst]      = g_sqKind[src];
        }
        g_sqHead = (g_sqHead + 1) % SPEECH_QUEUE_MAX;
        g_sqCount--;
    }
    int slot = (g_sqHead + g_sqCount) % SPEECH_QUEUE_MAX;
    strncpy(g_speechQ[slot], text, MAILBOX_SZ - 1);
    g_speechQ[slot][MAILBOX_SZ - 1] = 0;
    g_sqKind[slot]      = kind;
    g_sqCount++;
    g_hasPending = true;
    LeaveCriticalSection(&g_cs);
}

volatile bool g_silenceReq = false;        // -> the speech thread, which owns Tolk calls
void flushSpeech(const char* why) {
    EnterCriticalSection(&g_cs);
    int had = g_sqCount + g_barkCarryN;
    g_sqCount = 0;
    g_sqHead  = 0;                                // empty ring: any head is valid
    g_hasPending = false;
    g_barkCarryN = 0;                             // parked barks too: silence means silence
    LeaveCriticalSection(&g_cs);

    g_speakUntil     = 0;
    g_lastSpokenKind = SPK_NAV;

    g_silenceReq = true;

    logLine("speech: SILENCE (%s) — dropped %d queued line(s)", why, had);
}

void axReleaseReadHold(const char* who) {
    if (!g_speakUntil || g_lastSpokenKind != SPK_EVENT) return;   // no event hold in effect
    logLine("speech: %s releases the remaining speech hold", who);
    g_speakUntil = 0;
}

void stripMarkup(const char* in, char* out, int outsz) {
    int o = 0; bool pendingSpace = false; bool any = false;
    const char* p = in;
    while (*p && o < outsz - 1) {
        if (p[0] == '<' && p[1] == 'c' && p[2] == '>') {   // colour span open
            p += 3;
            for (int k = 0; k < 6 && *p; k++) p++;          // skip the 6-byte colour code
            continue;
        }
        if (p[0] == '<') {                                  // "</c>" or any other tag
            while (*p && *p != '>') p++;
            if (*p) p++;
            continue;
        }
        if (p[0] == '{') {                                  // legacy "{...}" tag
            while (*p && *p != '}') p++;
            if (*p) p++;
            continue;
        }
        char c = *p++;
        if (c == ' ' || c == '\n' || c == '\r' || c == '\t') {
            if (any) pendingSpace = true;                   // collapse runs, drop leading
            continue;
        }
        if (pendingSpace) { out[o++] = ' '; pendingSpace = false; if (o >= outsz - 1) break; }
        out[o++] = c; any = true;
    }
    out[o] = 0;
}
