// shared\gamedir.h -- game-directory detection, shared by the INSTALLER and the LAUNCHER.

#pragma once
#include <windows.h>
#include <string>
#include <vector>
#include <cstdio>
#include <cwchar>

static const wchar_t* kBdGameDirEnv = L"BLINDEST_DUNGEON_DIR"; // skip auto-detect
// The standard Steam location, used only if the registry lookup finds nothing.
static const wchar_t* kBdDefaultWin64 =
    L"C:\\Program Files (x86)\\Steam\\steamapps\\common\\DarkestDungeon\\_windows\\win64";
// Relative tail from a Steam LIBRARY root to the game's win64 folder.
static const wchar_t* kBdSteamTail =
    L"\\steamapps\\common\\DarkestDungeon\\_windows\\win64";

static bool bdFileExists(const std::wstring& p) {
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

static std::wstring bdRegReadString(HKEY root, const wchar_t* subkey, const wchar_t* value) {
    wchar_t buf[MAX_PATH]; DWORD sz = sizeof(buf);
    if (RegGetValueW(root, subkey, value, RRF_RT_REG_SZ, nullptr, buf, &sz) == ERROR_SUCCESS)
        return std::wstring(buf);
    return L"";
}

static std::wstring bdFromUtf8(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}

static std::string bdVdfValue(const std::string& text, size_t afterKey) {
    size_t p = afterKey;
    while (p < text.size() &&
           (text[p] == ' ' || text[p] == '\t' || text[p] == '\r' || text[p] == '\n')) ++p;
    if (p >= text.size() || text[p] != '"') return "";
    ++p;
    std::string o;
    while (p < text.size() && text[p] != '"') {
        if (text[p] == '\\' && p + 1 < text.size()) ++p;
        o += text[p++];
    }
    return o;
}

// Every Steam library root we can find (they may sit on different drives).
static std::vector<std::wstring> bdSteamLibraries() {
    std::vector<std::wstring> libs;
    std::wstring steam = bdRegReadString(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath");
    if (steam.empty())
        steam = bdRegReadString(HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\Valve\\Steam", L"InstallPath");
    if (steam.empty()) steam = L"C:\\Program Files (x86)\\Steam";

    // Steam writes SteamPath with forward slashes; normalise.
    for (auto& c : steam) if (c == L'/') c = L'\\';
    if (!steam.empty()) libs.push_back(steam);

    std::wstring vdf = steam + L"\\steamapps\\libraryfolders.vdf";
    FILE* f = _wfopen(vdf.c_str(), L"rb");
    if (f) {
        std::string text; char b[8192]; size_t r;
        while ((r = fread(b, 1, sizeof(b), f)) > 0) text.append(b, r);
        fclose(f);
        size_t pos = 0;
        for (;;) {
            size_t p = text.find("\"path\"", pos);
            if (p == std::string::npos) break;
            pos = p + 6;
            std::string val = bdVdfValue(text, p + 6);
            if (val.empty()) continue;
            std::wstring w = bdFromUtf8(val);
            for (auto& c : w) if (c == L'/') c = L'\\';
            libs.push_back(w);
        }
    }
    return libs;
}

static std::wstring bdResolveGameDir(std::wstring p, const wchar_t* exeName);

static std::vector<std::wstring> bdGogGames(const wchar_t* exeName) {
    std::vector<std::wstring> found;
    const wchar_t* roots[] = { L"SOFTWARE\\WOW6432Node\\GOG.com\\Games", L"SOFTWARE\\GOG.com\\Games" };
    for (const wchar_t* root : roots) {
        HKEY h;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, root, 0, KEY_READ | KEY_WOW64_64KEY, &h) != ERROR_SUCCESS)
            continue;
        for (DWORD i = 0;; ++i) {
            wchar_t sub[256]; DWORD n = 256;
            if (RegEnumKeyExW(h, i, sub, &n, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
            std::wstring key = std::wstring(root) + L"\\" + sub;
            std::wstring exe  = bdRegReadString(HKEY_LOCAL_MACHINE, key.c_str(), L"exe");
            std::wstring path = bdRegReadString(HKEY_LOCAL_MACHINE, key.c_str(), L"path");
            std::wstring dir = bdResolveGameDir(exe, exeName);
            if (dir.empty()) dir = bdResolveGameDir(path, exeName);
            if (!dir.empty()) found.push_back(dir);
        }
        RegCloseKey(h);
    }
    return found;
}

// Auto-detect the win64 folder, or empty if not found anywhere.
static std::wstring bdDetectGameDir(const wchar_t* exeName) {
    // 1) Explicit override wins.
    wchar_t env[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(kBdGameDirEnv, env, MAX_PATH);
    if (n > 0 && n < MAX_PATH) {
        std::wstring d(env);
        if (bdFileExists(d + L"\\" + exeName)) return d;
    }
    // 2) Steam libraries.
    for (const std::wstring& lib : bdSteamLibraries()) {
        std::wstring cand = lib + kBdSteamTail;
        if (bdFileExists(cand + L"\\" + exeName)) return cand;
    }
    for (const std::wstring& game : bdGogGames(exeName)) return game;
    // 3) Hard default.
    if (bdFileExists(std::wstring(kBdDefaultWin64) + L"\\" + exeName))
        return kBdDefaultWin64;
    return L"";
}

static std::wstring bdResolveGameDir(std::wstring p, const wchar_t* exeName) {
    while (!p.empty() && (p.front() == L' ' || p.front() == L'\t' || p.front() == L'"'))
        p.erase(0, 1);
    while (!p.empty() && (p.back() == L' ' || p.back() == L'\t' || p.back() == L'"'))
        p.pop_back();
    for (auto& c : p) if (c == L'/') c = L'\\';
    while (!p.empty() && p.back() == L'\\') p.pop_back();
    if (p.empty()) return L"";

    // A full path to the exe itself -> its folder.
    size_t len = wcslen(exeName);
    if (p.size() > len + 1 && p[p.size() - len - 1] == L'\\' &&
        _wcsicmp(p.substr(p.size() - len).c_str(), exeName) == 0)
        p = p.substr(0, p.size() - len - 1);

    if (bdFileExists(p + L"\\" + exeName)) return p;
    if (bdFileExists(p + L"\\_windows\\win64\\" + exeName)) return p + L"\\_windows\\win64";
    if (bdFileExists(p + L"\\win64\\" + exeName)) return p + L"\\win64";
    if (bdFileExists(p + L"\\_windowsnosteam\\win64\\" + exeName)) return p + L"\\_windowsnosteam\\win64";
    return L"";
}
