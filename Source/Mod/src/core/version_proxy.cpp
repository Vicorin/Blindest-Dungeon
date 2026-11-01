// Lazy version.dll proxy. Deliberately independent of the mod bootstrap/logger:
// FMOD can call these exports before the speech worker has initialized either.
#include <windows.h>
#include <winver.h>
#include <cstdio>
#include <cstdarg>
#include <type_traits>

extern "C" IMAGE_DOS_HEADER __ImageBase;
// Not declared in the SDK (see Microsoft's GetFileVersionInfoByHandle docs).
extern "C" BOOL WINAPI GetFileVersionInfoByHandle(DWORD, HANDLE, LPVOID*, PDWORD);

static INIT_ONCE g_versionOnce = INIT_ONCE_STATIC_INIT;
static wchar_t g_versionLog[MAX_PATH * 2];

static void versionLog(const wchar_t* format, ...) {
    DWORD savedError = GetLastError();
    wchar_t line[2048];
    va_list args;
    va_start(args, format);
    _vsnwprintf_s(line, _countof(line), _TRUNCATE, format, args);
    va_end(args);
    OutputDebugStringW(L"BlindestDungeon version proxy: ");
    OutputDebugStringW(line);
    OutputDebugStringW(L"\n");
    // Always available, including before the optional accessibility debug log.
    HANDLE file = CreateFileW(g_versionLog, FILE_APPEND_DATA,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file != INVALID_HANDLE_VALUE) {
        char utf8[8192];
        int n = WideCharToMultiByte(CP_UTF8, 0, line, -1, utf8, sizeof utf8, nullptr, nullptr);
        DWORD written;
        if (n > 1) WriteFile(file, utf8, n - 1, &written, nullptr);
        WriteFile(file, "\r\n", 2, &written, nullptr);
        CloseHandle(file);
    }
    SetLastError(savedError);
}

// Each entry uses the SDK's exact function-pointer type. Only ByHandle needs a
// local declaration. The aliases in version_proxy.def preserve the original ABI.
#define VERSION_APIS(X) \
 X(BOOL, GetFileVersionInfoA, (LPCSTR f, DWORD h, DWORD n, LPVOID p), (f,h,n,p), FALSE) \
 X(BOOL, GetFileVersionInfoByHandle, (DWORD flags, HANDLE h, LPVOID* p, PDWORD n), (flags,h,p,n), FALSE) \
 X(BOOL, GetFileVersionInfoExA, (DWORD flags, LPCSTR f, DWORD h, DWORD n, LPVOID p), (flags,f,h,n,p), FALSE) \
 X(BOOL, GetFileVersionInfoExW, (DWORD flags, LPCWSTR f, DWORD h, DWORD n, LPVOID p), (flags,f,h,n,p), FALSE) \
 X(DWORD, GetFileVersionInfoSizeA, (LPCSTR f, LPDWORD h), (f,h), 0) \
 X(DWORD, GetFileVersionInfoSizeExA, (DWORD flags, LPCSTR f, LPDWORD h), (flags,f,h), 0) \
 X(DWORD, GetFileVersionInfoSizeExW, (DWORD flags, LPCWSTR f, LPDWORD h), (flags,f,h), 0) \
 X(DWORD, GetFileVersionInfoSizeW, (LPCWSTR f, LPDWORD h), (f,h), 0) \
 X(BOOL, GetFileVersionInfoW, (LPCWSTR f, DWORD h, DWORD n, LPVOID p), (f,h,n,p), FALSE) \
 X(DWORD, VerFindFileA, (DWORD flags, LPCSTR f, LPCSTR w, LPCSTR a, LPSTR c, PUINT cn, LPSTR d, PUINT dn), (flags,f,w,a,c,cn,d,dn), VFF_BUFFTOOSMALL) \
 X(DWORD, VerFindFileW, (DWORD flags, LPCWSTR f, LPCWSTR w, LPCWSTR a, LPWSTR c, PUINT cn, LPWSTR d, PUINT dn), (flags,f,w,a,c,cn,d,dn), VFF_BUFFTOOSMALL) \
 X(DWORD, VerInstallFileA, (DWORD flags, LPCSTR s, LPCSTR d, LPCSTR sd, LPCSTR dd, LPCSTR cd, LPSTR t, PUINT tn), (flags,s,d,sd,dd,cd,t,tn), VIF_CANNOTREADSRC) \
 X(DWORD, VerInstallFileW, (DWORD flags, LPCWSTR s, LPCWSTR d, LPCWSTR sd, LPCWSTR dd, LPCWSTR cd, LPWSTR t, PUINT tn), (flags,s,d,sd,dd,cd,t,tn), VIF_CANNOTREADSRC) \
 X(DWORD, VerLanguageNameA, (DWORD lang, LPSTR p, DWORD n), (lang,p,n), 0) \
 X(DWORD, VerLanguageNameW, (DWORD lang, LPWSTR p, DWORD n), (lang,p,n), 0) \
 X(BOOL, VerQueryValueA, (LPCVOID p, LPCSTR s, LPVOID* out, PUINT n), (p,s,out,n), FALSE) \
 X(BOOL, VerQueryValueW, (LPCVOID p, LPCWSTR s, LPVOID* out, PUINT n), (p,s,out,n), FALSE)

#define VERSION_ENTRY(ret, name, params, args, failure) { #name, nullptr },
static struct { const char* name; FARPROC target; } g_versionExports[] = {
    VERSION_APIS(VERSION_ENTRY)
};
#undef VERSION_ENTRY

static BOOL CALLBACK initializeVersion(PINIT_ONCE, PVOID, PVOID*) {
    HMODULE self = reinterpret_cast<HMODULE>(&__ImageBase);
    DWORD n = GetModuleFileNameW(self, g_versionLog, _countof(g_versionLog));
    if (n && n < _countof(g_versionLog)) {
        wchar_t* slash = wcsrchr(g_versionLog, L'\\');
        const wchar_t logName[] = L"version-proxy.log";
        size_t remaining = slash ? _countof(g_versionLog) - (slash + 1 - g_versionLog) : 0;
        if (remaining >= _countof(logName)) wcscpy_s(slash + 1, remaining, logName);
        else g_versionLog[0] = 0;
    } else g_versionLog[0] = 0;

    wchar_t path[MAX_PATH + 32];
    n = GetSystemDirectoryW(path, MAX_PATH);
    if (!n || n >= MAX_PATH) {
        versionLog(L"GetSystemDirectoryW failed: %lu", GetLastError());
        return TRUE; // Cache failure; never retry or load an unqualified DLL.
    }
    wcscat_s(path, L"\\version.dll");
    versionLog(L"Loading system backend: %ls", path);
    HMODULE real = LoadLibraryExW(path, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!real) {
        versionLog(L"LoadLibraryExW failed: %lu", GetLastError());
        return TRUE;
    }
    // Absolute paths and search flags alone are not a recursion guarantee on
    // Wine. Also validate every resolved function, including PE forwarders.
    if (real == self) {
        versionLog(L"Rejected system backend: loader returned the proxy itself");
        FreeLibrary(real);
        return TRUE;
    }
    // Keep the LoadLibrary reference for the process lifetime.
    GetModuleFileNameW(real, path, _countof(path));
    versionLog(L"Backend loaded: %ls (%p)", path, real);
    for (auto& entry : g_versionExports) {
        FARPROC fn = GetProcAddress(real, entry.name);
        HMODULE owner = nullptr;
        if (fn && (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCWSTR>(fn), &owner) || owner == self)) {
            versionLog(L"Rejected recursive/unverifiable export: %hs", entry.name);
            fn = nullptr;
        }
        entry.target = fn;
        if (!fn) versionLog(L"Unavailable export: %hs (error %lu)", entry.name, GetLastError());
    }
    versionLog(L"Proxy initialization complete");
    return TRUE;
}

static FARPROC versionFunction(const char* name) {
    // No proxy work is added to DllMain. Initialize only on the first API call.
    DWORD savedError = GetLastError();
    if (InitOnceExecuteOnce(&g_versionOnce, initializeVersion, nullptr, nullptr)) {
        for (const auto& entry : g_versionExports) {
            if (strcmp(entry.name, name) == 0 && entry.target) {
                SetLastError(savedError);
                return entry.target;
            }
        }
    }
    SetLastError(ERROR_PROC_NOT_FOUND);
    return nullptr;
}

#define VERSION_WRAPPER(ret, name, params, args, failure) \
 extern "C" ret WINAPI Ax##name params { \
     auto fn = reinterpret_cast<decltype(&::name)>(versionFunction(#name)); \
     return fn ? fn args : failure; \
 } \
 static_assert(std::is_same<decltype(&Ax##name), decltype(&::name)>::value, "version API signature mismatch");
VERSION_APIS(VERSION_WRAPPER)
#undef VERSION_WRAPPER
#undef VERSION_APIS
