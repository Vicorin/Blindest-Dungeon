// Blindest Dungeon install manager --------------------------------- A single self-contained GUI
// program that installs, updates, repairs, and uninstalls the accessibility mod by talking to the
// project's GitHub Releases.

#include <windows.h>
#include <winhttp.h>
#include <tlhelp32.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <shldisp.h>
#include <exdisp.h>
#include <shlwapi.h>
#include <shlguid.h>
#include <string>
#include <vector>
#include <cstdio>
#include "../shared/gamedir.h" // game-dir detection, shared with the launcher

// ---- config ----
static const wchar_t* kTitle         = L"Blindest Dungeon Installer";
static const wchar_t* kExeName       = L"Darkest.exe";
static const wchar_t* kLegacyManifestName = L"ddaccess-install.json";
static const wchar_t* kManifestRes   = L"DDACCESS_MANIFEST"; // RCDATA resource in ddaccess.dll
static const wchar_t* kModDll        = L"ddaccess.dll"; // marker for an unmanaged install
static const wchar_t* kLauncherName  = L"DarkestAccess.exe"; // the mod's one-click launcher

static const wchar_t* kReleasesHost  = L"api.github.com";
static const wchar_t* kReleasesPath  = L"/repos/Vicorin/Blindest-Dungeon/releases/latest";
static const wchar_t* kReleasesUrlEnv = L"BLINDEST_DUNGEON_RELEASES_URL"; // full-URL override (testing)

static void say(const std::wstring& s);

static bool fileExists(const std::wstring& p) {
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

static std::vector<int> parseVersion(const std::wstring& s) {
    std::vector<int> v; size_t i = 0;
    if (!s.empty() && (s[0] == L'v' || s[0] == L'V')) i = 1;
    int cur = 0;
    for (; i < s.size(); ++i) { wchar_t c = s[i];
        if (c >= L'0' && c <= L'9') cur = cur * 10 + (c - L'0');
        else if (c == L'.') { v.push_back(cur); cur = 0; }
        else break; }
    v.push_back(cur); return v;
}
static bool isNewer(const std::wstring& remote, const std::wstring& local) {
    std::vector<int> r = parseVersion(remote), l = parseVersion(local);
    size_t n = (r.size() > l.size()) ? r.size() : l.size();
    for (size_t i = 0; i < n; ++i) { int rv = (i<r.size())?r[i]:0, lv = (i<l.size())?l[i]:0;
        if (rv != lv) return rv > lv; }
    return false;
}

static std::string jsonString(const std::string& body, const char* key, size_t from = 0) {
    std::string needle = std::string("\"") + key + "\"";
    size_t p = body.find(needle, from);
    if (p == std::string::npos) return "";
    p = body.find(':', p + needle.size());
    if (p == std::string::npos) return "";
    ++p;
    while (p < body.size() && (body[p]==' '||body[p]=='\t'||body[p]=='\r'||body[p]=='\n')) ++p;
    if (p >= body.size() || body[p] != '"') return "";
    ++p; std::string o;
    while (p < body.size() && body[p] != '"') { if (body[p]=='\\'&&p+1<body.size())++p; o += body[p++]; }
    return o;
}
// First browser_download_url ending in ".zip".
static std::string findZipAssetUrl(const std::string& body) {
    size_t pos = 0;
    for (;;) {
        size_t p = body.find("\"browser_download_url\"", pos);
        if (p == std::string::npos) return "";
        std::string url = jsonString(body, "browser_download_url", p);
        if (url.size() >= 4 && url.compare(url.size()-4, 4, ".zip") == 0) return url;
        pos = p + 22;
    }
}

static bool httpFetch(const std::wstring& host, const std::wstring& path,
                      INTERNET_PORT port, std::string* body, FILE* file) {
    if (body) body->clear();
    HINTERNET hS = WinHttpOpen(L"BlindestDungeonInstaller",
        WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hS) return false;
    WinHttpSetTimeouts(hS, 8000, 8000, 8000, 30000);

    bool ok = false;
    HINTERNET hC = WinHttpConnect(hS, host.c_str(), port, 0);
    if (hC) {
        DWORD flags = (port == INTERNET_DEFAULT_HTTPS_PORT) ? WINHTTP_FLAG_SECURE : 0;
        HINTERNET hR = WinHttpOpenRequest(hC, L"GET", path.c_str(), nullptr,
            WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
        if (hR) {
            WinHttpAddRequestHeaders(hR, L"Accept: application/vnd.github+json\r\n",
                (DWORD)-1, WINHTTP_ADDREQ_FLAG_ADD);
            if (WinHttpSendRequest(hR, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                    WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
                WinHttpReceiveResponse(hR, nullptr)) {
                DWORD status = 0, sz = sizeof(status);
                WinHttpQueryHeaders(hR, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                    WINHTTP_HEADER_NAME_BY_INDEX, &status, &sz, WINHTTP_NO_HEADER_INDEX);
                if (status == 200) {
                    ok = true;
                    DWORD avail = 0;
                    do {
                        avail = 0;
                        if (!WinHttpQueryDataAvailable(hR, &avail)) { ok = false; break; }
                        if (!avail) break;
                        std::string chunk(avail, '\0');
                        DWORD read = 0;
                        if (!WinHttpReadData(hR, &chunk[0], avail, &read)) { ok = false; break; }
                        if (body) body->append(chunk.data(), read);
                        if (file) fwrite(chunk.data(), 1, read, file);
                    } while (avail > 0);
                }
            }
            WinHttpCloseHandle(hR);
        }
        WinHttpCloseHandle(hC);
    }
    WinHttpCloseHandle(hS);
    return ok;
}

// Fetch the latest-release JSON, honouring the URL override.
static bool fetchReleaseJson(std::string& body) {
    std::wstring host = kReleasesHost, path = kReleasesPath;
    INTERNET_PORT port = INTERNET_DEFAULT_HTTPS_PORT;
    wchar_t env[1024];
    DWORD n = GetEnvironmentVariableW(kReleasesUrlEnv, env, 1024);
    if (n > 0 && n < 1024) {
        URL_COMPONENTS uc{}; uc.dwStructSize = sizeof(uc);
        wchar_t hb[256], pb[1024];
        uc.lpszHostName = hb; uc.dwHostNameLength = 256;
        uc.lpszUrlPath  = pb; uc.dwUrlPathLength  = 1024;
        if (WinHttpCrackUrl(env, n, 0, &uc)) {
            host = std::wstring(uc.lpszHostName, uc.dwHostNameLength);
            path = (uc.dwUrlPathLength>0) ? std::wstring(uc.lpszUrlPath, uc.dwUrlPathLength) : L"/";
            port = uc.nPort;
        }
    }
    return httpFetch(host, path, port, &body, nullptr);
}

// Download a full URL (any host) to a file.
static bool downloadUrl(const std::string& url, const std::wstring& destPath) {
    std::wstring wurl(url.begin(), url.end()); // URLs are ASCII
    URL_COMPONENTS uc{}; uc.dwStructSize = sizeof(uc);
    wchar_t hb[256], pb[2048];
    uc.lpszHostName = hb; uc.dwHostNameLength = 256;
    uc.lpszUrlPath  = pb; uc.dwUrlPathLength  = 2048;
    if (!WinHttpCrackUrl(wurl.c_str(), (DWORD)wurl.size(), 0, &uc)) return false;
    std::wstring host(uc.lpszHostName, uc.dwHostNameLength);
    std::wstring path = (uc.dwUrlPathLength>0) ? std::wstring(uc.lpszUrlPath, uc.dwUrlPathLength) : L"/";
    if (uc.lpszExtraInfo && uc.dwExtraInfoLength) path.append(uc.lpszExtraInfo, uc.dwExtraInfoLength);

    FILE* f = _wfopen(destPath.c_str(), L"wb");
    if (!f) return false;
    bool ok = httpFetch(host, path, uc.nPort, nullptr, f);
    fclose(f);
    if (!ok) DeleteFileW(destPath.c_str());
    return ok;
}

struct Manifest {
    std::wstring version;
    std::vector<std::wstring> files; // paths relative to the game dir
};

static std::wstring fromUtf8(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}
static bool parseManifestJson(const std::string& j, Manifest& m) {
    m.version = fromUtf8(jsonString(j, "version"));
    m.files.clear();
    // Extract the "files": [ "a", "b", ... ] array.
    size_t p = j.find("\"files\"");
    if (p != std::string::npos) {
        size_t lb = j.find('[', p), rb = j.find(']', lb);
        if (lb != std::string::npos && rb != std::string::npos) {
            size_t q = lb;
            while (true) {
                size_t s = j.find('"', q + 1);
                if (s == std::string::npos || s > rb) break;
                size_t e = s + 1; std::string val;
                while (e < rb && j[e] != '"') { if (j[e]=='\\'&&e+1<rb)++e; val += j[e++]; }
                m.files.push_back(fromUtf8(val));
                q = e + 1;
            }
        }
    }
    return !m.version.empty();
}

static bool readManifestResource(const std::wstring& dllPath, std::string& out) {
    out.clear();
    HMODULE h = LoadLibraryExW(dllPath.c_str(), nullptr,
                               LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE);
    if (!h) return false;
    if (HRSRC r = FindResourceW(h, kManifestRes, RT_RCDATA)) {
        DWORD sz = SizeofResource(h, r);
        if (HGLOBAL g = LoadResource(h, r)) {
            if (const char* p = (const char*)LockResource(g))
                out.assign(p, sz);
        }
    }
    FreeLibrary(h);
    return !out.empty();
}

static bool readManifest(const std::wstring& gameDir, Manifest& m) {
    std::string j;
    if (readManifestResource(gameDir + L"\\" + kModDll, j) && parseManifestJson(j, m))
        return true;
    // Legacy fallback: a pre-v0.10 install recorded a loose JSON file instead.
    FILE* f = _wfopen((gameDir + L"\\" + kLegacyManifestName).c_str(), L"rb");
    if (!f) return false;
    j.clear(); char b[8192]; size_t r;
    while ((r = fread(b, 1, sizeof(b), f)) > 0) j.append(b, r);
    fclose(f);
    return parseManifestJson(j, m);
}

static bool runHidden(const std::wstring& cmdline) {
    std::vector<wchar_t> cl(cmdline.begin(), cmdline.end()); cl.push_back(0);
    STARTUPINFOW si{}; si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW; si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, cl.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) return false;
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1; GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
    return code == 0;
}

static void rmTree(const std::wstring& dir) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            std::wstring name = fd.cFileName;
            if (name == L"." || name == L"..") continue;
            std::wstring full = dir + L"\\" + name;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) rmTree(full);
            else DeleteFileW(full.c_str());
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryW(dir.c_str());
}

static bool copyTreeInto(const std::wstring& srcDir, const std::wstring& dstDir,
                         const std::wstring& relRoot, std::vector<std::wstring>& written) {
    CreateDirectoryW(dstDir.c_str(), nullptr);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((srcDir + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return false;
    bool ok = true;
    do {
        std::wstring name = fd.cFileName;
        if (name == L"." || name == L"..") continue;
        std::wstring s = srcDir + L"\\" + name;
        std::wstring d = dstDir + L"\\" + name;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (!copyTreeInto(s, d, relRoot, written)) ok = false;
        } else {
            if (CopyFileW(s.c_str(), d.c_str(), FALSE)) {
                // relative path = d minus relRoot + backslash
                std::wstring rel = d.substr(relRoot.size() + 1);
                written.push_back(rel);
            } else { ok = false; }
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return ok;
}

// The zip wraps everything in a single "<Name>-v<ver>" folder; return that folder.
static std::wstring singleSubfolder(const std::wstring& dir) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    std::wstring found;
    int count = 0;
    if (h != INVALID_HANDLE_VALUE) {
        do {
            std::wstring name = fd.cFileName;
            if (name == L"." || name == L"..") continue;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) { found = dir + L"\\" + name; ++count; }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    return (count == 1) ? found : dir; // if not a single wrapper, deploy dir itself
}

static bool gameRunning() {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W pe; pe.dwSize = sizeof(pe);
    bool found = false;
    if (Process32FirstW(snap, &pe)) do {
        if (_wcsicmp(pe.szExeFile, kExeName) == 0) { found = true; break; }
    } while (Process32NextW(snap, &pe));
    CloseHandle(snap);
    return found;
}

static void doInstall(const std::wstring& gameDir) {
    if (gameRunning()) {
        say(L"Darkest Dungeon is running. Please close the game first, then try again.");
        return;
    }

    say(L"Checking GitHub for the latest release...");
    std::string json;
    if (!fetchReleaseJson(json)) { say(L"Could not reach GitHub. Check your internet connection."); return; }
    std::wstring tag = fromUtf8(jsonString(json, "tag_name"));
    std::string  url = findZipAssetUrl(json);
    if (tag.empty() || url.empty()) { say(L"The latest release has no downloadable package."); return; }

    // Asset file name (last path segment).
    std::string assetName = url.substr(url.find_last_of('/') + 1);
    say(L"Latest release is version " + tag + L". Downloading " + fromUtf8(assetName) + L"...");

    wchar_t tmp[MAX_PATH]; GetTempPathW(MAX_PATH, tmp);
    std::wstring work = std::wstring(tmp) + L"blindest_install";
    rmTree(work); CreateDirectoryW(work.c_str(), nullptr);
    std::wstring zipPath = work + L"\\" + fromUtf8(assetName);

    if (!downloadUrl(url, zipPath)) { say(L"Download failed."); rmTree(work); return; }
    say(L"Download complete. Extracting...");

    std::wstring extractDir = work + L"\\x";
    CreateDirectoryW(extractDir.c_str(), nullptr);
    // bsdtar (C:\Windows\System32\tar.exe) extracts zip natively.
    std::wstring cmd = L"tar.exe -xf \"" + zipPath + L"\" -C \"" + extractDir + L"\"";
    if (!runHidden(cmd)) { say(L"Extraction failed (tar.exe)."); rmTree(work); return; }

    Manifest old;
    bool hadOld = readManifest(gameDir, old);

    std::wstring payload = singleSubfolder(extractDir);
    std::vector<std::wstring> written;
    if (!copyTreeInto(payload, gameDir, gameDir, written) || written.empty()) {
        say(L"Copying files into the game folder failed. Are you running as administrator?");
        rmTree(work); return;
    }

    if (hadOld) {
        for (const std::wstring& rel : old.files) {
            bool still = false;
            for (const std::wstring& w : written)
                if (_wcsicmp(rel.c_str(), w.c_str()) == 0) { still = true; break; }
            if (!still) DeleteFileW((gameDir + L"\\" + rel).c_str());
        }
        RemoveDirectoryW((gameDir + L"\\ddaccess-lang").c_str());
    }
    DeleteFileW((gameDir + L"\\" + kLegacyManifestName).c_str());

    rmTree(work);

    Manifest fresh; std::string freshJson;
    if (readManifestResource(gameDir + L"\\" + kModDll, freshJson) &&
        parseManifestJson(freshJson, fresh)) {
        say(L"Install complete. Version " + fresh.version + L" is now installed. Press Play "
            L"to start the game; the mod speaks at the main menu.");
    } else {
        say(L"Install complete (version " + tag + L"), but this build of ddaccess.dll has no "
            L"embedded install record, so the installer will treat it as a manual install.");
    }
}

static void doUninstall(const std::wstring& gameDir) {
    if (gameRunning()) { say(L"Darkest Dungeon is running. Please close it first."); return; }
    Manifest m;
    if (!readManifest(gameDir, m)) { say(L"Nothing to uninstall: no install manifest found."); return; }

    int removed = 0;
    for (const std::wstring& rel : m.files) {
        std::wstring full = gameDir + L"\\" + rel;
        if (DeleteFileW(full.c_str())) ++removed;
    }
    // Remove the mod's language subfolder if it is now empty.
    RemoveDirectoryW((gameDir + L"\\ddaccess-lang").c_str());
    DeleteFileW((gameDir + L"\\" + kLegacyManifestName).c_str());
    say(L"Uninstalled: removed " + std::to_wstring(removed) + L" file(s). The game is back to vanilla.");
}

#ifndef BDI_NO_MAIN

enum {
    IDC_PATH      = 100,
    IDC_PRIMARY   = 101,   // label switches: Install / Reinstall / Update
    IDC_UNINSTALL = 103,
    IDC_BROWSE    = 104,
    IDC_STATUS    = 105,
    IDC_LOG       = 106,
    IDC_PLAY      = 107,
};

// Worker actions. One worker at a time; the buttons are disabled while it runs.
enum { ACT_REFRESH = 0, ACT_INSTALL = 1, ACT_UNINSTALL = 2 };

#define WM_APP_SAY      (WM_APP + 1)
#define WM_APP_DONE     (WM_APP + 2)
#define WM_APP_ANNOUNCE (WM_APP + 3)

static HWND  g_hwnd    = nullptr;
static HWND  g_path    = nullptr;
static HWND  g_status  = nullptr;
static HWND  g_log     = nullptr;
static HWND  g_primary = nullptr;
static HWND  g_uninst  = nullptr;
static HWND  g_play    = nullptr;
static HWND  g_browse  = nullptr;
static HWND  g_exit    = nullptr;
static HFONT g_font    = nullptr;
static bool  g_busy    = false;

static std::wstring g_gameDir;
static std::wstring g_latest;       // latest release tag, empty = unknown/offline
static bool         g_probed = false;

#ifdef BDI_TRACE
void traceCmd(const char* what, unsigned lo, unsigned hi);
#endif

static void say(const std::wstring& s) {
    if (!g_hwnd) return;
    if (GetCurrentThreadId() != GetWindowThreadProcessId(g_hwnd, nullptr)) {
        PostMessageW(g_hwnd, WM_APP_SAY, 0, (LPARAM)new std::wstring(s));
        return;
    }
    // Status field shows the newest message; the log keeps them all reviewable.
    SetWindowTextW(g_status, s.c_str());
    int len = GetWindowTextLengthW(g_log);
    SendMessageW(g_log, EM_SETSEL, len, len);
    std::wstring line = s + L"\r\n";
    SendMessageW(g_log, EM_REPLACESEL, FALSE, (LPARAM)line.c_str());
}

static void announceStatus() {
    if (GetFocus() == g_status) SetFocus(g_log);
    SetFocus(g_status);
}

static void updateUi(bool speak) {
    std::wstring status;
    std::wstring primaryLabel = L"&Install";
    bool primaryOn = false;
    bool managed = false;

    if (g_gameDir.empty()) {
        status = L"Game folder not found. Type or paste the folder that contains "
                 L"Darkest.exe into the Game folder field, or use Browse.";
        if (GetFocus() != g_path) SetWindowTextW(g_path, L"");
    } else {
        if (GetFocus() != g_path) SetWindowTextW(g_path, g_gameDir.c_str());
        Manifest m;
        managed = readManifest(g_gameDir, m);
        if (!g_probed) {
            status = L"Checking GitHub for the latest release...";
        } else if (managed) {
            primaryLabel = L"&Update"; primaryOn = true;
            if (!g_latest.empty() && isNewer(g_latest, m.version)) {
                status = L"Update available. Installed: " + m.version + L". Latest: " + g_latest + L".";
            } else if (!g_latest.empty()) {
                status = L"Up to date. Installed: " + m.version + L".";
            } else {
                status = L"Installed: " + m.version + L". Could not check GitHub for updates.";
            }
        } else if (fileExists(g_gameDir + L"\\" + kModDll)) {
            status = L"Manual install detected. Reinstall will convert it to a managed install.";
            primaryLabel = L"&Reinstall"; primaryOn = true;
        } else {
            status = (g_latest.empty() && g_probed)
                ? L"Ready to install, but GitHub is unreachable. Check your internet connection."
                : L"Ready to install.";
            primaryOn = true;
        }
    }

    bool modPresent = !g_gameDir.empty() &&
                      (managed || fileExists(g_gameDir + L"\\" + kModDll));
    bool playable   = !g_gameDir.empty() && fileExists(g_gameDir + L"\\" + kLauncherName);
    if (playable && !g_busy) status += L" Press Play to start the game.";

    SetWindowTextW(g_primary, primaryLabel.c_str());
    EnableWindow(g_primary, primaryOn && !g_busy);
    EnableWindow(g_uninst,  managed && !g_busy);
    EnableWindow(g_play,    playable && !g_busy);
    EnableWindow(g_browse,  !g_busy);
    EnableWindow(g_exit,    !g_busy);
    SendMessageW(g_path, EM_SETREADONLY, g_busy, 0);

    // Existence per state, then pack the visible ones left to right.
    struct { HWND h; bool show; } row[] = {
        { g_primary, true },
        { g_uninst,  managed },
        { g_play,    modPresent },
        { g_exit,    true },
    };
    int x = 12;
    for (auto& b : row) {
        if (b.show) { SetWindowPos(b.h, nullptr, x, 96, 100, 30,
                                   SWP_NOZORDER | SWP_SHOWWINDOW); x += 108; }
        else ShowWindow(b.h, SW_HIDE);
    }

    if (speak) say(status);
    else SetWindowTextW(g_status, status.c_str());
}

// ---- editable Game folder field ----
static bool commitPathField(bool* changed) {
    if (changed) *changed = false;
    if (g_busy) return true;
    wchar_t buf[1024] = L"";
    GetWindowTextW(g_path, buf, 1024);
    std::wstring t(buf);
    if (t.find_first_not_of(L" \t") == std::wstring::npos) return true; // empty = no opinion
    std::wstring r = bdResolveGameDir(t, kExeName);
    if (r.empty()) return false;
    if (_wcsicmp(r.c_str(), g_gameDir.c_str()) != 0) {
        g_gameDir = r;
        if (changed) *changed = true;
    }
    return true;
}

static bool commitPathBeforeAction() {
    bool changed = false;
    if (!commitPathField(&changed)) {
        say(L"That folder does not contain Darkest.exe.");
        announceStatus();
        return false;
    }
    if (changed) updateUi(false);
    return true;
}

#ifdef BDI_TRACE
void traceCmd(const char* what, unsigned lo, unsigned hi);
#endif

static DWORD WINAPI workerProc(LPVOID p) {
    int action = (int)(INT_PTR)p;
#ifdef BDI_TRACE
    traceCmd("worker start, action", (unsigned)action, 0);
#endif
    if (action == ACT_REFRESH) {
        std::string json;
        if (fetchReleaseJson(json)) g_latest = fromUtf8(jsonString(json, "tag_name"));
        g_probed = true;
    } else if (action == ACT_INSTALL) {
        doInstall(g_gameDir);
    } else if (action == ACT_UNINSTALL) {
        doUninstall(g_gameDir);
    }
    PostMessageW(g_hwnd, WM_APP_DONE, 0, 0);
    return 0;
}

static void startWork(int action) {
    if (g_busy) return;
    g_busy = true;
    updateUi(false);           // grey the buttons out
    SetFocus(g_log);           // focus somewhere valid while they are disabled
    HANDLE h = CreateThread(nullptr, 0, workerProc, (LPVOID)(INT_PTR)action, 0, nullptr);
    if (h) CloseHandle(h);
    else { g_busy = false; updateUi(false); say(L"Could not start the operation."); }
}

// ---- Play ----
static HRESULT shellExecInExplorer(const std::wstring& file, const std::wstring& dir) {
    IShellWindows* psw = nullptr;
    HRESULT hr = CoCreateInstance(__uuidof(ShellWindows), nullptr, CLSCTX_LOCAL_SERVER,
                                  IID_PPV_ARGS(&psw));
    if (FAILED(hr)) return hr;

    HWND hwnd = nullptr;
    IDispatch* pdisp = nullptr;
    VARIANT vEmpty; VariantInit(&vEmpty);
    hr = psw->FindWindowSW(&vEmpty, &vEmpty, SWC_DESKTOP, (long*)&hwnd,
                           SWFO_NEEDDISPATCH, &pdisp);
    if (hr == S_OK && pdisp) {
        IShellBrowser* psb = nullptr;
        hr = IUnknown_QueryService(pdisp, SID_STopLevelBrowser, IID_PPV_ARGS(&psb));
        if (SUCCEEDED(hr)) {
            IShellView* psv = nullptr;
            hr = psb->QueryActiveShellView(&psv);
            if (SUCCEEDED(hr)) {
                IDispatch* pdispView = nullptr;
                hr = psv->GetItemObject(SVGIO_BACKGROUND, IID_PPV_ARGS(&pdispView));
                if (SUCCEEDED(hr)) {
                    IShellFolderViewDual* psfvd = nullptr;
                    hr = pdispView->QueryInterface(IID_PPV_ARGS(&psfvd));
                    if (SUCCEEDED(hr)) {
                        IDispatch* pdispShell = nullptr;
                        hr = psfvd->get_Application(&pdispShell);
                        if (SUCCEEDED(hr)) {
                            IShellDispatch2* psd = nullptr;
                            hr = pdispShell->QueryInterface(IID_PPV_ARGS(&psd));
                            if (SUCCEEDED(hr)) {
                                BSTR bstrFile = SysAllocString(file.c_str());
                                VARIANT vDir; VariantInit(&vDir);
                                vDir.vt = VT_BSTR;
                                vDir.bstrVal = SysAllocString(dir.c_str());
                                VARIANT vShow; VariantInit(&vShow);
                                vShow.vt = VT_I4; vShow.lVal = SW_SHOWNORMAL;
                                hr = psd->ShellExecuteW(bstrFile, vEmpty, vDir, vEmpty, vShow);
                                SysFreeString(bstrFile);
                                VariantClear(&vDir);
                                psd->Release();
                            }
                            pdispShell->Release();
                        }
                        psfvd->Release();
                    }
                    pdispView->Release();
                }
                psv->Release();
            }
            psb->Release();
        }
        pdisp->Release();
    } else if (SUCCEEDED(hr)) {
        hr = E_FAIL; // S_FALSE: no desktop window to ask
    }
    psw->Release();
    return hr;
}

static void launchGame() {
    if (gameRunning()) {
        say(L"Darkest Dungeon is already running.");
        announceStatus();
        return;
    }
    std::wstring exe = g_gameDir + L"\\" + kLauncherName;
    if (!fileExists(exe)) {
        say(L"DarkestAccess.exe was not found in the game folder. Install the mod first.");
        announceStatus();
        return;
    }
    HRESULT hr = shellExecInExplorer(exe, g_gameDir);
    if (FAILED(hr)) {
        HINSTANCE h = ShellExecuteW(nullptr, L"open", exe.c_str(), nullptr,
                                    g_gameDir.c_str(), SW_SHOWNORMAL);
        if ((INT_PTR)h <= 32) {
            say(L"Could not start the game launcher.");
            announceStatus();
            return;
        }
    }
    say(L"Launching Darkest Dungeon with the accessibility mod. The mod speaks at the "
        L"main menu. You can exit this installer.");
    announceStatus();
}

// Modern folder picker (keyboard- and screen-reader-friendly).
static std::wstring browseForFolder(HWND owner) {
    std::wstring result;
    IFileOpenDialog* dlg = nullptr;
    if (SUCCEEDED(CoCreateInstance(__uuidof(FileOpenDialog), nullptr, CLSCTX_INPROC_SERVER,
                                   IID_PPV_ARGS(&dlg)))) {
        DWORD opts = 0; dlg->GetOptions(&opts);
        dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
        dlg->SetTitle(L"Select the folder that contains Darkest.exe");
        if (SUCCEEDED(dlg->Show(owner))) {
            IShellItem* item = nullptr;
            if (SUCCEEDED(dlg->GetResult(&item))) {
                PWSTR psz = nullptr;
                if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &psz))) {
                    result = psz; CoTaskMemFree(psz);
                }
                item->Release();
            }
        }
        dlg->Release();
    }
    return result;
}

static LRESULT CALLBACK wndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_APP_SAY: {
        std::wstring* s = (std::wstring*)lp;
        say(*s); delete s;
        return 0;
    }
    case WM_APP_DONE:
        g_busy = false;
        updateUi(true);
        announceStatus();
        return 0;
    case WM_APP_ANNOUNCE:
        announceStatus();
        return 0;
    case WM_COMMAND:
        if (LOWORD(wp) == IDC_PATH && HIWORD(wp) == EN_KILLFOCUS && !g_busy) {
            bool changed = false;
            if (!commitPathField(&changed)) {
                say(L"That folder does not contain Darkest.exe.");
                PostMessageW(h, WM_APP_ANNOUNCE, 0, 0);
            } else if (changed) {
                updateUi(true);
                PostMessageW(h, WM_APP_ANNOUNCE, 0, 0);
            }
            return 0;
        }
#ifdef BDI_TRACE
        traceCmd("WM_COMMAND", LOWORD(wp), HIWORD(wp));
#endif
        if (HIWORD(wp) > 1) break;
        switch (LOWORD(wp)) {
        case IDC_PRIMARY:
            if (!commitPathBeforeAction()) return 0;
            startWork(ACT_INSTALL);
            return 0;
        case IDC_PLAY:
            if (!commitPathBeforeAction()) return 0;
            launchGame();
            return 0;
        case IDC_UNINSTALL:
            if (!commitPathBeforeAction()) return 0;
            if (MessageBoxW(h, L"Remove Blindest Dungeon from this game folder?",
                            L"Confirm Uninstall", MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2)
                == IDYES)
                startWork(ACT_UNINSTALL);
            else { say(L"Cancelled."); announceStatus(); }
            return 0;
        case IDC_BROWSE: {
            std::wstring p = browseForFolder(h);
            if (p.empty()) return 0;
            std::wstring r = bdResolveGameDir(p, kExeName);
            if (!r.empty()) {
                g_gameDir = r;
                updateUi(true);
            } else {
                say(L"That folder does not contain Darkest.exe.");
            }
            announceStatus();
            return 0;
        }
        case IDCANCEL: // Exit button, Escape
            if (!g_busy) DestroyWindow(h);
            else { say(L"Please wait for the current operation to finish."); announceStatus(); }
            return 0;
        }
        break;
    case WM_CLOSE:
        if (!g_busy) DestroyWindow(h);
        else { say(L"Please wait for the current operation to finish."); announceStatus(); }
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

static WNDPROC g_editBaseProc = nullptr;
static LRESULT CALLBACK editSubclassProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    LRESULT r = CallWindowProcW(g_editBaseProc, h, msg, wp, lp);
    if (msg == WM_GETDLGCODE) r &= ~(DLGC_WANTALLKEYS | DLGC_WANTTAB);
    return r;
}

static HWND makeCtl(const wchar_t* cls, const wchar_t* text, DWORD style,
                    int x, int y, int w, int hgt, int id) {
    HWND c = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style,
                             x, y, w, hgt, g_hwnd, (HMENU)(INT_PTR)id,
                             GetModuleHandleW(nullptr), nullptr);
    SendMessageW(c, WM_SETFONT, (WPARAM)g_font, TRUE);
    return c;
}

#ifdef BDI_TRACE
static void trace(const char* what) {
    wchar_t tmp[MAX_PATH]; GetTempPathW(MAX_PATH, tmp);
    FILE* f = _wfopen((std::wstring(tmp) + L"bdi-trace.log").c_str(), L"ab");
    SYSTEMTIME st; GetSystemTime(&st);
    if (f) { fprintf(f, "%02d:%02d:%02d.%03dZ %s\r\n",
                     st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, what); fclose(f); }
}
void traceCmd(const char* what, unsigned lo, unsigned hi) {
    char buf[128];
    sprintf(buf, "%s lo=%u hi=%u", what, lo, hi);
    trace(buf);
}
#else
#define trace(x)
#endif

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int nShow) {
    trace("enter");
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    trace("com ok");

    g_gameDir = bdDetectGameDir(kExeName);
    trace("detect ok");

    // The standard dialog message font.
    NONCLIENTMETRICSW ncm{}; ncm.cbSize = sizeof(ncm);
    SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
    g_font = CreateFontIndirectW(&ncm.lfMessageFont);

    WNDCLASSW wc{};
    wc.lpfnWndProc   = wndProc;
    wc.hInstance     = hInst;
    wc.lpszClassName = L"BlindestDungeonInstaller";
    wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    RegisterClassW(&wc);

    RECT rc = { 0, 0, 560, 400 };
    AdjustWindowRect(&rc, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, FALSE);
    g_hwnd = CreateWindowExW(0, wc.lpszClassName, kTitle,
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT, rc.right - rc.left, rc.bottom - rc.top,
        nullptr, nullptr, hInst, nullptr);
    trace(g_hwnd ? "window ok" : "window FAILED");

    makeCtl(L"STATIC", L"Game folder:", SS_NOPREFIX, 12, 15, 88, 20, 0);
    g_path   = makeCtl(L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL | WS_BORDER,
                       106, 12, 330, 24, IDC_PATH);
    g_browse = makeCtl(L"BUTTON", L"&Browse...", WS_TABSTOP | BS_PUSHBUTTON,
                       446, 11, 100, 26, IDC_BROWSE);
    makeCtl(L"STATIC", L"Status:", SS_NOPREFIX, 12, 51, 60, 20, 0);
    g_status = makeCtl(L"EDIT", L"", WS_TABSTOP | ES_MULTILINE | ES_READONLY | WS_BORDER,
                       76, 48, 472, 40, IDC_STATUS);
    g_primary = makeCtl(L"BUTTON", L"&Install", WS_TABSTOP | BS_DEFPUSHBUTTON,
                        12, 96, 100, 30, IDC_PRIMARY);
    g_uninst  = makeCtl(L"BUTTON", L"U&ninstall", WS_TABSTOP | BS_PUSHBUTTON,
                        120, 96, 100, 30, IDC_UNINSTALL);
    g_play    = makeCtl(L"BUTTON", L"&Play", WS_TABSTOP | BS_PUSHBUTTON,
                        228, 96, 100, 30, IDC_PLAY);
    g_exit    = makeCtl(L"BUTTON", L"E&xit", WS_TABSTOP | BS_PUSHBUTTON,
                        336, 96, 100, 30, IDCANCEL);
    makeCtl(L"STATIC", L"Messages:", SS_NOPREFIX, 12, 140, 200, 18, 0);
    g_log = makeCtl(L"EDIT", L"", WS_TABSTOP | ES_MULTILINE | ES_READONLY |
                    ES_AUTOVSCROLL | WS_VSCROLL | WS_BORDER, 12, 160, 536, 228, IDC_LOG);
    g_editBaseProc = (WNDPROC)SetWindowLongPtrW(g_log, GWLP_WNDPROC, (LONG_PTR)editSubclassProc);
    SetWindowLongPtrW(g_status, GWLP_WNDPROC, (LONG_PTR)editSubclassProc);

    trace("controls ok");
    updateUi(false);
    ShowWindow(g_hwnd, nShow);
    trace("shown");
    say(g_gameDir.empty()
        ? L"Blindest Dungeon installer. Game folder not found; type or paste it into the "
          L"Game folder field, or use Browse."
        : L"Blindest Dungeon installer. Game folder found. Checking GitHub for the latest release...");
    announceStatus();
    startWork(ACT_REFRESH);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        if (!IsDialogMessage(g_hwnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    CoUninitialize();
    return 0;
}

#else
static void say(const std::wstring&); // harness provides its own definition
#endif // BDI_NO_MAIN
