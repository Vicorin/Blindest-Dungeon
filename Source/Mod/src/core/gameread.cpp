// core/gameread.cpp — the game-memory read layer

#include <cstdint>
#include <cstring>
#include <cstdio>
#include "internal.h"

// ---- SEH-guarded raw reads ----
bool safeReadPtr(uintptr_t addr, uintptr_t* out) {
    __try { *out = *reinterpret_cast<volatile uintptr_t*>(addr); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool safeReadI64(uintptr_t addr, int64_t* out) {
    __try { *out = *reinterpret_cast<volatile int64_t*>(addr); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool safeReadU32(uintptr_t addr, uint32_t* out) {
    __try { *out = *reinterpret_cast<volatile uint32_t*>(addr); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool safeReadU8(uintptr_t addr, uint8_t* out) {
    __try { *out = *reinterpret_cast<volatile uint8_t*>(addr); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool safeReadCStr(uintptr_t addr, char* dst, int max) {
    __try {
        const char* s = reinterpret_cast<const char*>(addr);
        int i = 0;
        for (; i < max - 1; i++) { char c = s[i]; dst[i] = c; if (!c) return true; }
        dst[i] = 0;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool safeReadBlock(uintptr_t addr, void* dst, int n) {
    __try { memcpy(dst, reinterpret_cast<const void*>(addr), (size_t)n); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool safeWriteU8(uintptr_t addr, uint8_t v) {
    __try { *reinterpret_cast<volatile uint8_t*>(addr) = v; return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool safeWriteU32(uintptr_t addr, uint32_t v) {
    __try { *reinterpret_cast<volatile uint32_t*>(addr) = v; return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool safeWriteU64(uintptr_t addr, uint64_t v) {
    __try { *reinterpret_cast<volatile uint64_t*>(addr) = v; return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// ---- SEH reporting: WHERE the game code died ----
int sehReport(const char* what, EXCEPTION_POINTERS* xp) {
    if (!xp || !xp->ExceptionRecord) {
        logLine("SEH: %s faulted (no exception record)", what ? what : "?");
        return EXCEPTION_EXECUTE_HANDLER;
    }
    const EXCEPTION_RECORD* er = xp->ExceptionRecord;
    uintptr_t at = (uintptr_t)er->ExceptionAddress;
    char where[64];
    if (g_base && at >= g_base) _snprintf(where, sizeof where, "game+0x%llx",
                                          (unsigned long long)(at - g_base));
    else                        _snprintf(where, sizeof where, "%p", (void*)at);
    where[sizeof where - 1] = 0;
    char line[256];
    if (er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && er->NumberParameters >= 2) {
        const char* how = er->ExceptionInformation[0] == 0 ? "reading"
                        : er->ExceptionInformation[0] == 1 ? "writing"
                                                           : "executing";
        _snprintf(line, sizeof line, "SEH: %s -> access violation at %s, %s %p",
                  what ? what : "?", where, how, (void*)er->ExceptionInformation[1]);
    } else {
        _snprintf(line, sizeof line, "SEH: %s -> exception 0x%08x at %s",
                  what ? what : "?", (unsigned)er->ExceptionCode, where);
    }
    line[sizeof line - 1] = 0;
    logLine("%s", line);
    return EXCEPTION_EXECUTE_HANDLER;
}

// ---- Text extraction ----
bool readTbwText(uintptr_t tbw, char* out, int outsz) {
    if (safeReadCStr(tbw + TBW_TEXT_OFF, out, outsz) && out[0]) return true;
    uintptr_t p = 0;
    if (safeReadPtr(tbw + TBW_TEXT_PTR_OFF, &p) && p > 0x10000) {
        if (safeReadCStr(p, out, outsz) && out[0]) return true;
    }
    return false;
}

bool extractWidgetText(uintptr_t widget, uintptr_t tbwVtbl,
                       char* out, int outsz, char* via, int viasz) {
    uintptr_t vt = 0;
    if (safeReadPtr(widget, &vt) && vt == tbwVtbl) {
        if (readTbwText(widget, out, outsz)) { _snprintf(via, viasz, "self"); return true; }
    }
    for (int off = 0; off <= 0x400; off += 8) {
        uintptr_t p = 0;
        if (!safeReadPtr(widget + off, &p) || p < 0x10000) continue;
        uintptr_t cvt = 0;
        if (safeReadPtr(p, &cvt) && cvt == tbwVtbl) {
            if (readTbwText(p, out, outsz)) { _snprintf(via, viasz, "child+0x%x", off); return true; }
        }
    }
    return false;
}

// ---- Focus lookup ----
uintptr_t findOwnerByFocusId(uintptr_t base, int64_t id) {
    uintptr_t begin = 0, end = 0;
    if (!safeReadPtr(base + VEC_BEGIN_RVA, &begin)) return 0;
    if (!safeReadPtr(base + VEC_END_RVA, &end)) return 0;
    if (begin == 0 || end <= begin) return 0;
    uintptr_t count = (end - begin) / ELEM_STRIDE;
    if (count > 4096) count = 4096;
    for (uintptr_t i = 0; i < count; i++) {
        uintptr_t elem = begin + i * ELEM_STRIDE;
        int64_t eid = 0;
        if (!safeReadI64(elem + ELEM_ID_OFF, &eid)) continue;
        if (eid == id) {
            uintptr_t owner = 0;
            if (safeReadPtr(elem + ELEM_OWNER_OFF, &owner)) return owner;
            return 0;
        }
    }
    return 0;
}

uintptr_t focusElementById(uintptr_t base, int64_t id) {
    uintptr_t begin = 0, end = 0;
    if (!safeReadPtr(base + VEC_BEGIN_RVA, &begin)) return 0;
    if (!safeReadPtr(base + VEC_END_RVA, &end)) return 0;
    if (begin == 0 || end <= begin) return 0;
    uintptr_t count = (end - begin) / ELEM_STRIDE;
    if (count > 4096) count = 4096;
    for (uintptr_t i = 0; i < count; i++) {
        uintptr_t elem = begin + i * ELEM_STRIDE;
        int64_t eid = 0;
        if (!safeReadI64(elem + ELEM_ID_OFF, &eid)) continue;
        if (eid == id) return elem;
    }
    return 0;
}

bool isNoFocus(int64_t id) { return id == 0 || id == (int64_t)-1; }

void idToAscii(int64_t id, char* out) {
    unsigned char* b = reinterpret_cast<unsigned char*>(&id);
    int i = 0;
    for (; i < 8; i++) out[i] = (b[i] >= 32 && b[i] < 127) ? (char)b[i] : '.';
    out[i] = 0;
}

long focusVectorCount(uintptr_t base) {
    uintptr_t begin = 0, end = 0;
    if (!safeReadPtr(base + VEC_BEGIN_RVA, &begin)) return -1;
    if (!safeReadPtr(base + VEC_END_RVA, &end)) return -1;
    if (begin == 0 || end < begin) return 0;
    return (long)((end - begin) / ELEM_STRIDE);
}

// ---- Localization resolve (MAIN THREAD ONLY) ----
bool resolveKey(uintptr_t base, const char* key, char* out, int outsz) {
    if (!out || outsz <= 0) return false;
    out[0] = 0;
    ResolveFn resolve = reinterpret_cast<ResolveFn>(base + RESOLVER_RVA);
    uintptr_t box[8];
    for (int i = 0; i < 8; i++) box[i] = 0;
    __try { resolve(box, key); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    uintptr_t s = box[0];
    if (s < 0x10000) return false;
    if (!safeReadCStr(s, out, outsz) || !out[0]) { out[0] = 0; return false; }
    if (strcmp(out, key) == 0                          // exact key echo
        || strstr(out, key) != nullptr                 // decorated key echo "[<key>]...", incl. coloured
        || strstr(out, "error_not_localized") != nullptr) {
        out[0] = 0;                                    // the contract: false => empty
        return false;
    }
    return true;
}
