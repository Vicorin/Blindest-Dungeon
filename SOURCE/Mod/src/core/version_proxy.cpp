// ---- THE VERSION.DLL PROXY

#include <windows.h>
#include <winver.h>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <type_traits>

extern "C" IMAGE_DOS_HEADER __ImageBase;
extern "C" BOOL WINAPI GetFileVersionInfoByHandle(DWORD, HANDLE, LPVOID*, PDWORD);

static INIT_ONCE g_versionOnce = INIT_ONCE_STATIC_INIT;

static volatile LONG g_versionInitDone = 0;    // 1 once initializeVersion has run to its end
static HMODULE g_versionBackend = nullptr;     // the real DLL, or null when rejected / failed
static wchar_t g_versionBackendPath[MAX_PATH + 32] = L"";
static char g_versionFailure[160] = "";        // why there is no backend, when there is none
static int g_versionUnavailable = 0;           // exports the backend lacks (ByHandle on Wine)

static void versionDebug(const wchar_t* format, ...) {
    DWORD savedError = GetLastError();
    wchar_t line[1024];
    va_list args;
    va_start(args, format);
    _vsnwprintf_s(line, _countof(line), _TRUNCATE, format, args);
    va_end(args);
    OutputDebugStringW(L"BlindestDungeon version proxy: ");
    OutputDebugStringW(line);
    OutputDebugStringW(L"\n");
    SetLastError(savedError);
}

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
    wchar_t path[MAX_PATH + 32];
    DWORD n = GetSystemDirectoryW(path, MAX_PATH);
    if (!n || n >= MAX_PATH) {
        _snprintf_s(g_versionFailure, _countof(g_versionFailure), _TRUNCATE,
                    "GetSystemDirectoryW failed (error %lu)", GetLastError());
        versionDebug(L"GetSystemDirectoryW failed: %lu", GetLastError());
        InterlockedExchange(&g_versionInitDone, 1);
        return TRUE;
    }
    wcscat_s(path, L"\\version.dll");
    versionDebug(L"Loading system backend: %ls", path);
    HMODULE real = LoadLibraryExW(path, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!real) {
        _snprintf_s(g_versionFailure, _countof(g_versionFailure), _TRUNCATE,
                    "LoadLibraryExW of the System32 version.dll failed (error %lu)", GetLastError());
        versionDebug(L"LoadLibraryExW failed: %lu", GetLastError());
        InterlockedExchange(&g_versionInitDone, 1);
        return TRUE;
    }
    if (real == self) {
        strcpy_s(g_versionFailure, "the loader returned the proxy itself as the backend");
        versionDebug(L"Rejected system backend: loader returned the proxy itself");
        FreeLibrary(real);
        InterlockedExchange(&g_versionInitDone, 1);
        return TRUE;
    }
    // The LoadLibrary reference is kept for the life of the process.
    GetModuleFileNameW(real, g_versionBackendPath, _countof(g_versionBackendPath));
    versionDebug(L"Backend loaded: %ls (%p)", g_versionBackendPath, real);
    for (auto& entry : g_versionExports) {
        FARPROC fn = GetProcAddress(real, entry.name);
        HMODULE owner = nullptr;
        if (fn && (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                       reinterpret_cast<LPCWSTR>(fn), &owner) || owner == self)) {
            versionDebug(L"Rejected recursive/unverifiable export: %hs", entry.name);
            fn = nullptr;
        }
        entry.target = fn;
        if (!fn) {
            ++g_versionUnavailable;
            versionDebug(L"Unavailable export: %hs (error %lu)", entry.name, GetLastError());
        }
    }
    g_versionBackend = real;
    versionDebug(L"Proxy initialization complete");
    InterlockedExchange(&g_versionInitDone, 1);
    return TRUE;
}

static FARPROC versionFunction(const char* name) {
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

// ---- THE REPORT ----
extern "C" void axVersionProxyReport(char* out, size_t cap) {
    if (!InterlockedCompareExchange(&g_versionInitDone, 0, 0)) {
        _snprintf_s(out, cap, _TRUNCATE, "version proxy: backend not initialised yet (no version API call so far)");
        return;
    }
    if (!g_versionBackend) {
        _snprintf_s(out, cap, _TRUNCATE, "version proxy: NO BACKEND -- %s; every version API answers failure",
                    g_versionFailure);
        return;
    }
    _snprintf_s(out, cap, _TRUNCATE, "version proxy: backend %ls, %d of 17 exports unavailable",
                g_versionBackendPath, g_versionUnavailable);
}
