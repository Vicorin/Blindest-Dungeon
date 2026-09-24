// DarkestAccess launcher ----------------------- One double-clickable file that starts Darkest Dungeon
// and loads the accessibility mod (ddaccess.dll) for the user — no terminal, no typing.

#include <windows.h>
#include <tlhelp32.h>
#include <shellapi.h>
#include <winhttp.h>
#include <string>
#include <vector>
#include <cstdio>
#include "../shared/gamedir.h" // game-dir detection, shared with the installer

// ---- config ----
static const wchar_t* kSteamUrl  = L"steam://rungameid/262060"; // Darkest Dungeon 1
static const wchar_t* kExeName   = L"Darkest.exe";
static const wchar_t* kDllName   = L"ddaccess.dll";
static const wchar_t* kPrismName = L"prism.dll";

static const int kWaitProcessSec = 180; // max wait for the game process to appear
static const int kWaitWindowSec  = 120; // max wait for its main window after that
static const int kSettleSec      = 12;  // let the menu load before injecting

static const wchar_t* kReleasesHost  = L"api.github.com";
static const wchar_t* kReleasesPath  = L"/repos/Vicorin/Blindest-Dungeon/releases/latest";
static const wchar_t* kReleasesUrlEnv = L"BLINDEST_DUNGEON_RELEASES_URL";

// ---- PRISM (opt) ----
#include "prism.h"

static HMODULE       g_prism     = nullptr;
static PrismContext* g_prCtx     = nullptr;
static PrismBackend* g_prBackend = nullptr;
static decltype(&prism_backend_output) g_prOutput   = nullptr;
static decltype(&prism_backend_free)   g_prFree     = nullptr;
static decltype(&prism_shutdown)       g_prShutdown = nullptr;

static std::wstring  g_updateMsg;

static void speechInit(const std::wstring& gameDir) {
    // Prefer the prism.dll that already sits next to Darkest.exe.
    std::wstring full = gameDir + L"\\" + kPrismName;
    g_prism = LoadLibraryW(full.c_str());
    if (!g_prism) g_prism = LoadLibraryW(kPrismName); // fall back to normal search
    if (!g_prism) return;

    auto cfgInit = reinterpret_cast<decltype(&prism_config_init)>(
                       GetProcAddress(g_prism, "prism_config_init"));
    auto ctxInit = reinterpret_cast<decltype(&prism_init)>(
                       GetProcAddress(g_prism, "prism_init"));
    auto best    = reinterpret_cast<decltype(&prism_registry_create_best)>(
                       GetProcAddress(g_prism, "prism_registry_create_best"));
    auto bInit   = reinterpret_cast<decltype(&prism_backend_initialize)>(
                       GetProcAddress(g_prism, "prism_backend_initialize"));
    g_prOutput   = reinterpret_cast<decltype(&prism_backend_output)>(
                       GetProcAddress(g_prism, "prism_backend_output"));
    g_prFree     = reinterpret_cast<decltype(&prism_backend_free)>(
                       GetProcAddress(g_prism, "prism_backend_free"));
    g_prShutdown = reinterpret_cast<decltype(&prism_shutdown)>(
                       GetProcAddress(g_prism, "prism_shutdown"));
    if (!cfgInit || !ctxInit || !best || !bInit || !g_prOutput) return;

    PrismConfig cfg = cfgInit();
    g_prCtx = ctxInit(&cfg);
    if (!g_prCtx) return;
    g_prBackend = best(g_prCtx);
    if (!g_prBackend) return;
    PrismError e = bInit(g_prBackend);
    if (e != PRISM_OK && e != PRISM_ERROR_ALREADY_INITIALIZED) {
        if (g_prFree) g_prFree(g_prBackend);
        g_prBackend = nullptr;
    }
}

static void say(const wchar_t* msg) {
    if (!g_prBackend || !g_prOutput) return;
    // PRISM takes UTF-8; the launcher's wide literals are converted at this one edge.
    char utf8[2048];
    if (WideCharToMultiByte(CP_UTF8, 0, msg, -1, utf8, sizeof utf8, nullptr, nullptr) <= 0)
        return;
    g_prOutput(g_prBackend, utf8, false); // false = don't interrupt, queue it
}

static void speechShutdown() {
    if (g_prBackend && g_prFree)     g_prFree(g_prBackend);
    if (g_prCtx     && g_prShutdown) g_prShutdown(g_prCtx);
    g_prBackend = nullptr; g_prCtx = nullptr;
    if (g_prism) { FreeLibrary(g_prism); g_prism = nullptr; }
}

// ---- helpers ----
static std::wstring launcherDir() {
    wchar_t buf[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    std::wstring p(buf, n);
    size_t slash = p.find_last_of(L"\\/");
    return (slash == std::wstring::npos) ? L"." : p.substr(0, slash);
}

static bool fileExists(const std::wstring& p) {
    return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES;
}

// Resolve where ddaccess.dll lives: next to the launcher first, else the game dir.
static std::wstring resolveDll(const std::wstring& gameDir) {
    std::wstring here = launcherDir() + L"\\" + kDllName;
    if (fileExists(here)) return here;
    std::wstring inGame = gameDir + L"\\" + kDllName;
    if (fileExists(inGame)) return inGame;
    return L""; // not found
}

static DWORD findPid(const wchar_t* name) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W pe; pe.dwSize = sizeof(pe);
    DWORD pid = 0;
    if (Process32FirstW(snap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, name) == 0) { pid = pe.th32ProcessID; break; }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return pid;
}

static bool moduleLoaded(DWORD pid, const wchar_t* moduleName) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (snap == INVALID_HANDLE_VALUE) return false;
    MODULEENTRY32W me; me.dwSize = sizeof(me);
    bool found = false;
    if (Module32FirstW(snap, &me)) {
        do {
            if (_wcsicmp(me.szModule, moduleName) == 0) { found = true; break; }
        } while (Module32NextW(snap, &me));
    }
    CloseHandle(snap);
    return found;
}

// Wait until the process shows a visible top-level window.
struct WndSearch { DWORD pid; HWND hwnd; };
static BOOL CALLBACK enumProc(HWND hwnd, LPARAM lp) {
    auto* s = reinterpret_cast<WndSearch*>(lp);
    DWORD wpid = 0;
    GetWindowThreadProcessId(hwnd, &wpid);
    if (wpid == s->pid && IsWindowVisible(hwnd) && GetWindow(hwnd, GW_OWNER) == nullptr) {
        RECT r;
        if (GetWindowRect(hwnd, &r) && (r.right - r.left) > 100 && (r.bottom - r.top) > 100) {
            s->hwnd = hwnd;
            return FALSE; // stop
        }
    }
    return TRUE;
}
static bool hasMainWindow(DWORD pid) {
    WndSearch s{ pid, nullptr };
    EnumWindows(enumProc, reinterpret_cast<LPARAM>(&s));
    return s.hwnd != nullptr;
}

static void fail(const wchar_t* msg) {
    say(msg);
    MessageBoxW(nullptr, msg, L"Darkest Dungeon Accessibility", MB_OK | MB_ICONWARNING);
}

// The proven injection: write the DLL path into the target and LoadLibraryW it.
static bool inject(DWORD pid, const std::wstring& dllPath) {
    HANDLE hProc = OpenProcess(
        PROCESS_CREATE_THREAD | PROCESS_VM_OPERATION | PROCESS_VM_WRITE |
        PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, FALSE, pid);
    if (!hProc) return false;

    bool ok = false;
    SIZE_T sz = (dllPath.size() + 1) * sizeof(wchar_t);
    void* remote = VirtualAllocEx(hProc, nullptr, sz, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (remote && WriteProcessMemory(hProc, remote, dllPath.c_str(), sz, nullptr)) {
        HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
        auto loadLib = reinterpret_cast<LPTHREAD_START_ROUTINE>(GetProcAddress(k32, "LoadLibraryW"));
        HANDLE hThread = CreateRemoteThread(hProc, nullptr, 0, loadLib, remote, 0, nullptr);
        if (hThread) {
            WaitForSingleObject(hThread, INFINITE);
            DWORD exitCode = 0;
            GetExitCodeThread(hThread, &exitCode); // LoadLibraryW's HMODULE (low 32 bits); 0 = failed
            ok = (exitCode != 0);
            CloseHandle(hThread);
        }
    }
    if (remote) VirtualFreeEx(hProc, remote, 0, MEM_RELEASE);
    CloseHandle(hProc);
    return ok;
}

// ---- update check ----

static std::vector<int> parseVersion(const std::wstring& s) {
    std::vector<int> out;
    size_t i = 0;
    if (!s.empty() && (s[0] == L'v' || s[0] == L'V')) i = 1;
    int cur = 0;
    for (; i < s.size(); ++i) {
        wchar_t c = s[i];
        if (c >= L'0' && c <= L'9') { cur = cur * 10 + (c - L'0'); }
        else if (c == L'.')         { out.push_back(cur); cur = 0; }
        else break;
    }
    out.push_back(cur);
    return out;
}

// True if remote is strictly newer than local (missing components read as 0).
static bool isNewer(const std::wstring& remote, const std::wstring& local) {
    std::vector<int> r = parseVersion(remote), l = parseVersion(local);
    size_t n = (r.size() > l.size()) ? r.size() : l.size();
    for (size_t i = 0; i < n; ++i) {
        int rv = (i < r.size()) ? r[i] : 0;
        int lv = (i < l.size()) ? l[i] : 0;
        if (rv != lv) return rv > lv;
    }
    return false;
}

static std::string jsonString(const std::string& body, const char* key) {
    std::string needle = std::string("\"") + key + "\"";
    size_t p = body.find(needle);
    if (p == std::string::npos) return "";
    p = body.find(':', p + needle.size());
    if (p == std::string::npos) return "";
    ++p;
    while (p < body.size() && (body[p] == ' ' || body[p] == '\t' ||
                               body[p] == '\r' || body[p] == '\n')) ++p;
    if (p >= body.size() || body[p] != '"') return ""; // null or non-string
    ++p;
    std::string out;
    while (p < body.size() && body[p] != '"') {
        if (body[p] == '\\' && p + 1 < body.size()) ++p; // naive unescape (tags are plain)
        out += body[p++];
    }
    return out;
}

static bool httpsGet(const std::wstring& host, const std::wstring& path,
                     INTERNET_PORT port, std::string& body) {
    body.clear();
    // User-Agent is REQUIRED by the GitHub API -- without it the response is 403.
    HINTERNET hSession = WinHttpOpen(L"BlindestDungeonLauncher",
        WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) return false;
    // resolve, connect, send, receive -- 5s each so the worst case is bounded.
    WinHttpSetTimeouts(hSession, 5000, 5000, 5000, 5000);

    bool ok = false;
    HINTERNET hConnect = WinHttpConnect(hSession, host.c_str(), port, 0);
    if (hConnect) {
        DWORD flags = (port == INTERNET_DEFAULT_HTTPS_PORT) ? WINHTTP_FLAG_SECURE : 0;
        HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET", path.c_str(),
            nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
        if (hRequest) {
            WinHttpAddRequestHeaders(hRequest,
                L"Accept: application/vnd.github+json\r\n",
                (DWORD)-1, WINHTTP_ADDREQ_FLAG_ADD);
            if (WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                    WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
                WinHttpReceiveResponse(hRequest, nullptr)) {
                DWORD status = 0, sz = sizeof(status);
                WinHttpQueryHeaders(hRequest,
                    WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                    WINHTTP_HEADER_NAME_BY_INDEX, &status, &sz, WINHTTP_NO_HEADER_INDEX);
                if (status == 200) {
                    DWORD avail = 0;
                    do {
                        avail = 0;
                        if (!WinHttpQueryDataAvailable(hRequest, &avail)) break;
                        if (!avail) break;
                        std::string chunk(avail, '\0');
                        DWORD read = 0;
                        if (!WinHttpReadData(hRequest, &chunk[0], avail, &read)) break;
                        body.append(chunk.data(), read);
                        if (body.size() > 512 * 1024) break; // sanity cap; the tag is near the top
                    } while (avail > 0);
                    ok = true;
                }
            }
            WinHttpCloseHandle(hRequest);
        }
        WinHttpCloseHandle(hConnect);
    }
    WinHttpCloseHandle(hSession);
    return ok;
}

static std::wstring readInstalledVersion() {
    wchar_t path[MAX_PATH];
    if (!GetModuleFileNameW(nullptr, path, MAX_PATH)) return L"";
    DWORD handle = 0;
    DWORD sz = GetFileVersionInfoSizeW(path, &handle);
    if (!sz) return L"";
    std::vector<BYTE> buf(sz);
    if (!GetFileVersionInfoW(path, 0, sz, buf.data())) return L"";
    VS_FIXEDFILEINFO* ffi = nullptr; UINT len = 0;
    if (!VerQueryValueW(buf.data(), L"\\", reinterpret_cast<LPVOID*>(&ffi), &len) || !ffi)
        return L"";
    WORD a = HIWORD(ffi->dwFileVersionMS), b = LOWORD(ffi->dwFileVersionMS);
    WORD c = HIWORD(ffi->dwFileVersionLS), d = LOWORD(ffi->dwFileVersionLS);
    if (!a && !b && !c && !d) return L""; // unstamped dev build -> unknown
    wchar_t v[64];
    swprintf(v, 64, L"%u.%u.%u.%u", a, b, c, d);
    return v;
}

static bool updateCheckEnabled() {
    wchar_t local[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return true;
    std::wstring ini = std::wstring(local) + L"\\DarkestAccess\\settings.ini";
    FILE* f = _wfopen(ini.c_str(), L"r");
    if (!f) return true;
    bool on = true;
    char line[128];
    while (fgets(line, sizeof line, f)) {
        char* p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (strncmp(p, "check_updates=", 14) == 0) { on = (p[14] != '0'); break; }
    }
    fclose(f);
    return on;
}

static void checkForUpdate() {
    if (!updateCheckEnabled()) return;
    std::wstring local = readInstalledVersion();
    if (local.empty()) return; // unknown version -> don't guess, don't nag

    std::wstring host = kReleasesHost, path = kReleasesPath;
    INTERNET_PORT port = INTERNET_DEFAULT_HTTPS_PORT;

    // Optional full-URL override for testing.
    wchar_t envBuf[1024];
    DWORD n = GetEnvironmentVariableW(kReleasesUrlEnv, envBuf, 1024);
    if (n > 0 && n < 1024) {
        URL_COMPONENTS uc{};
        uc.dwStructSize = sizeof(uc);
        wchar_t hostBuf[256], pathBuf[1024];
        uc.lpszHostName = hostBuf; uc.dwHostNameLength = 256;
        uc.lpszUrlPath  = pathBuf; uc.dwUrlPathLength  = 1024;
        if (WinHttpCrackUrl(envBuf, n, 0, &uc)) {
            host = std::wstring(uc.lpszHostName, uc.dwHostNameLength);
            path = (uc.dwUrlPathLength > 0)
                 ? std::wstring(uc.lpszUrlPath, uc.dwUrlPathLength) : L"/";
            port = uc.nPort;
        }
    }

    std::string body;
    if (!httpsGet(host, path, port, body)) return; // offline / error -> stay silent
    std::string tag = jsonString(body, "tag_name");
    if (tag.empty()) return;

    std::wstring remote(tag.begin(), tag.end()); // tags are ASCII
    if (!isNewer(remote, local)) return;         // up to date

    g_updateMsg = L"An update is available: version " + remote +
                  L". You have version " + local +
                  L". Run the Blindest Dungeon installer to update.";
}

// ---- main ----
#ifndef BD_NO_MAIN
int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    std::wstring gameDir = launcherDir();
    if (!fileExists(gameDir + L"\\" + kExeName)) {
        gameDir = bdDetectGameDir(kExeName);
        if (gameDir.empty()) gameDir = kBdDefaultWin64;
    }

    speechInit(gameDir);

    std::wstring dllPath = resolveDll(gameDir);
    if (dllPath.empty()) {
        fail(L"Could not find the mod file ddaccess.dll. Put it next to this launcher or "
             L"next to Darkest.exe, then run this again.");
        speechShutdown();
        return 1;
    }

    // 1) Launch the game if it isn't already running.
    DWORD pid = findPid(kExeName);
    if (!pid) {
        say(L"Starting Darkest Dungeon.");
        bool viaSteam = fileExists(gameDir + L"\\steam_api64.dll");
        if (viaSteam) {
            ShellExecuteW(nullptr, L"open", kSteamUrl, nullptr, nullptr, SW_SHOWNORMAL);
        } else {
            std::wstring exe = gameDir + L"\\" + kExeName;
            ShellExecuteW(nullptr, L"open", exe.c_str(), nullptr, gameDir.c_str(), SW_SHOWNORMAL);
        }

        // 2) Wait for the process.
        for (int i = 0; i < kWaitProcessSec && !pid; ++i) {
            Sleep(1000);
            pid = findPid(kExeName);
        }
        if (!pid) {
            fail(viaSteam ? L"Darkest Dungeon did not start. Please make sure Steam is installed and try again."
                          : L"Darkest Dungeon did not start. Please start the game yourself, then run this launcher again.");
            speechShutdown();
            return 1;
        }
    } else {
        say(L"Darkest Dungeon is already running.");
    }

    checkForUpdate();

    // 3) Wait for the main window, then let the menu settle.
    for (int i = 0; i < kWaitWindowSec && !hasMainWindow(pid); ++i) {
        Sleep(1000);
        if (findPid(kExeName) == 0) { // crashed/closed during startup
            fail(L"Darkest Dungeon closed before the mod could load.");
            speechShutdown();
            return 1;
        }
    }
    say(L"Loading accessibility mod, please wait.");
    for (int i = 0; i < kSettleSec; ++i) Sleep(1000);

    // Refresh pid in case Steam relaunched a bootstrap process.
    pid = findPid(kExeName);
    if (!pid) {
        fail(L"Darkest Dungeon is no longer running.");
        speechShutdown();
        return 1;
    }

    // 4) Inject (skip if somehow already loaded).
    bool ok = moduleLoaded(pid, kDllName) || inject(pid, dllPath);
    if (ok) {
    } else {
        fail(L"The accessibility mod failed to load. The game is running normally without it. "
             L"Make sure prism.dll is next to Darkest.exe.");
    }

    if (!g_updateMsg.empty()) {
        if (ok) Sleep(4000);
        say(g_updateMsg.c_str());
    }

    DWORD flushMs = 1500;
    if (!g_updateMsg.empty()) {
        flushMs += (DWORD)g_updateMsg.size() * 70;  // ~14 chars/s covers a slow voice
        if (flushMs > 15000) flushMs = 15000;
    }
    Sleep(flushMs);
    speechShutdown();
    return ok ? 0 : 1;
}
#endif // BD_NO_MAIN
