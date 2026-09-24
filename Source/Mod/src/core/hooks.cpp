// core/hooks.cpp — every patch the mod installs, and the bootstrap that installs them

#include <cstdint>
#include <cstdio>
#include <cstring>
#include "internal.h"

// SDL_PollEvent IAT hook.
uintptr_t* g_iatSlot  = nullptr;   // address of the IAT entry we patched
PollFn     g_origPoll = nullptr;   // original SDL_PollEvent

typedef void (*VoidFn)(void);
static uintptr_t* g_startSlot = nullptr;  static VoidFn g_origStartText = nullptr;
static uintptr_t* g_stopSlot  = nullptr;  static VoidFn g_origStopText  = nullptr;

// ---- IAT hook install / remove ----
static uintptr_t* findIatSlot(uintptr_t base, const char* import) {
    auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return nullptr;
    auto nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return nullptr;
    auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return nullptr;
    auto desc = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress);
    for (; desc->Name; ++desc) {
        DWORD namesRva = desc->OriginalFirstThunk ? desc->OriginalFirstThunk : desc->FirstThunk;
        auto orig = reinterpret_cast<IMAGE_THUNK_DATA*>(base + namesRva);
        auto iat  = reinterpret_cast<IMAGE_THUNK_DATA*>(base + desc->FirstThunk);
        for (; orig->u1.AddressOfData; ++orig, ++iat) {
            if (orig->u1.Ordinal & IMAGE_ORDINAL_FLAG64) continue;  // imported by ordinal
            auto ibn = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + orig->u1.AddressOfData);
            if (strcmp(reinterpret_cast<const char*>(ibn->Name), import) == 0)
                return reinterpret_cast<uintptr_t*>(&iat->u1.Function);
        }
    }
    return nullptr;
}

// Patch one IAT entry to `detour`, saving its slot address and original target.
static bool patchIat(uintptr_t base, const char* name, void* detour,
                     uintptr_t** slotOut, void** origOut) {
    uintptr_t* slot = findIatSlot(base, name);
    if (!slot) return false;
    *origOut = reinterpret_cast<void*>(*slot);
    DWORD old = 0;
    if (!VirtualProtect(slot, sizeof(uintptr_t), PAGE_READWRITE, &old)) return false;
    *slot = reinterpret_cast<uintptr_t>(detour);
    VirtualProtect(slot, sizeof(uintptr_t), old, &old);
    *slotOut = slot;
    return true;
}

static bool installPollHook(uintptr_t base) {
    return patchIat(base, "SDL_PollEvent", reinterpret_cast<void*>(&OurPoll),
                    &g_iatSlot, reinterpret_cast<void**>(&g_origPoll));
}

// ---- Verify-before-write for every vtable slot patch ----
static bool vftableIs(uintptr_t base, uintptr_t vftRva, const char* expectDecorated, const char* tag) {
    uintptr_t col = 0; uint32_t tdRva = 0; char name[128] = {0};
    if (!safeReadPtr(base + vftRva - 8, &col) || col < base || col > base + 0x10000000ULL ||
        !safeReadU32(col + 0x0c, &tdRva) || tdRva == 0 ||
        !safeReadCStr(base + tdRva + 0x10, name, sizeof name) || !name[0]) {
        logLine("%s: no readable RTTI behind vftable rva 0x%llx -- NOT patching",
                tag, (unsigned long long)vftRva);
        return false;
    }
    if (strcmp(name, expectDecorated) != 0) {
        logLine("%s: vftable rva 0x%llx is class \"%s\", expected \"%s\" -- NOT patching",
                tag, (unsigned long long)vftRva, name, expectDecorated);
        return false;
    }
    return true;
}

// ---- Options-menu vtable hook ----
typedef void* (__fastcall *MenuDrawFn)(void*, void*, void*, void*);
static MenuDrawFn g_origMenuDraw = nullptr;
static void**     g_menuSlot     = nullptr;

static void* __fastcall MenuDrawDetour(void* self, void* a2, void* a3, void* a4) {
    g_optMenu = self;
    return g_origMenuDraw ? g_origMenuDraw(self, a2, a3, a4) : nullptr;
}
static void installMenuHook(uintptr_t base) {
    if (!vftableIs(base, MENU_VFTABLE_RVA, ".?AVMenu@Panel@UI@@", "menuhook")) return;
    void** slot = reinterpret_cast<void**>(base + MENU_VFTABLE_RVA + (uintptr_t)MENU_DRAW_SLOT * 8);
    DWORD old = 0;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) {
        logLine("menuhook: VirtualProtect failed"); return;
    }
    g_origMenuDraw = reinterpret_cast<MenuDrawFn>(*slot);
    *slot = reinterpret_cast<void*>(&MenuDrawDetour);
    VirtualProtect(slot, sizeof(void*), old, &old);
    g_menuSlot = slot;
    logLine("menuhook: installed slot=0x%llx orig=0x%llx",
            (unsigned long long)slot, (unsigned long long)g_origMenuDraw);
}
// ---- Tutorial-popup vtable hook (UI::Panel::TutorialPopup, slot 23) ----
typedef void* (__fastcall *TutShowFn)(void*, void*, void*, void*);
static TutShowFn g_origTutShow = nullptr;
static void**    g_tutSlot     = nullptr;

static void* __fastcall TutShowDetour(void* self, void* a2, void* a3, void* a4) {
    __try { announceTutorialPopup(reinterpret_cast<uintptr_t>(self)); }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
    return g_origTutShow ? g_origTutShow(self, a2, a3, a4) : nullptr;
}
static void installTutorialHook(uintptr_t base) {
    if (!vftableIs(base, TUT_VFTABLE_RVA, ".?AVTutorialPopup@Panel@UI@@", "tuthook")) return;
    void** slot = reinterpret_cast<void**>(base + TUT_VFTABLE_RVA + (uintptr_t)TUT_SHOW_SLOT * 8);
    DWORD old = 0;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) {
        logLine("tuthook: VirtualProtect failed"); return;
    }
    g_origTutShow = reinterpret_cast<TutShowFn>(*slot);
    *slot = reinterpret_cast<void*>(&TutShowDetour);
    VirtualProtect(slot, sizeof(void*), old, &old);
    g_tutSlot = slot;
    logLine("tuthook: installed slot=0x%llx orig=0x%llx",
            (unsigned long long)slot, (unsigned long long)g_origTutShow);
}
// ---- Journal-popup vtable hook (UI::Panel::JournalPopup, slot 23) ----
typedef void* (__fastcall *JournalShowFn)(void*, void*, void*, void*);
static JournalShowFn g_origJournalShow = nullptr;
static void**        g_jpSlot          = nullptr;

static void* __fastcall JournalShowDetour(void* self, void* a2, void* a3, void* a4) {
    uintptr_t p = reinterpret_cast<uintptr_t>(self);
    if (p != g_jpObj) {
        uint32_t st = 0, page = 0;
        safeReadU32(p + JP_STATE_OFF, &st);
        safeReadU32(p + JP_PAGE_OFF, &page);
        logLine("journalhook: first draw, obj=%p state=%u page=%u", (void*)p, st, page);
    }
    g_jpObj = p;
    g_jpSeenTick = GetTickCount();
    return g_origJournalShow ? g_origJournalShow(self, a2, a3, a4) : nullptr;
}
static void installJournalHook(uintptr_t base) {
    if (!vftableIs(base, JP_VFT_RVA, ".?AVJournalPopup@Panel@UI@@", "journalhook")) return;
    void** slot = reinterpret_cast<void**>(base + JP_VFT_RVA + (uintptr_t)JP_SHOW_SLOT * 8);
    DWORD old = 0;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) {
        logLine("journalhook: VirtualProtect failed"); return;
    }
    g_origJournalShow = reinterpret_cast<JournalShowFn>(*slot);
    *slot = reinterpret_cast<void*>(&JournalShowDetour);
    VirtualProtect(slot, sizeof(void*), old, &old);
    g_jpSlot = slot;
    logLine("journalhook: installed slot=0x%llx orig=0x%llx",
            (unsigned long long)slot, (unsigned long long)g_origJournalShow);
}
// ---- Light-meter vtable hook (LightTorchOverlay, slot 5 = Render) ----
typedef void* (__fastcall *LightRenderFn)(void*, void*, void*, void*);
static LightRenderFn g_origLightRender = nullptr;
static void**        g_ltoSlot         = nullptr;

static void* __fastcall LightRenderDetour(void* self, void* a2, void* a3, void* a4) {
    uintptr_t p = reinterpret_cast<uintptr_t>(self);
    // Log only when the instance changes — once per raid, not once per frame.
    if (p != g_ltoObj) logLine("lighthook: first render, obj=%p", (void*)p);
    g_ltoObj      = p;
    g_ltoSeenTick = GetTickCount();
    return g_origLightRender ? g_origLightRender(self, a2, a3, a4) : nullptr;
}
static void installLightHook(uintptr_t base) {
    if (!vftableIs(base, LTO_VFTABLE_RVA, ".?AVLightTorchOverlay@@", "lighthook")) return;
    void** slot = reinterpret_cast<void**>(base + LTO_VFTABLE_RVA + (uintptr_t)LTO_RENDER_SLOT * 8);
    DWORD old = 0;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) {
        logLine("lighthook: VirtualProtect failed"); return;
    }
    g_origLightRender = reinterpret_cast<LightRenderFn>(*slot);
    *slot = reinterpret_cast<void*>(&LightRenderDetour);
    VirtualProtect(slot, sizeof(void*), old, &old);
    g_ltoSlot = slot;
    logLine("lighthook: installed slot=0x%llx orig=0x%llx",
            (unsigned long long)slot, (unsigned long long)g_origLightRender);
}
// ---- Town-event-popup vtable hook (TownUI::Panel::TownEventDisplay, slot 23) ----
typedef void* (__fastcall *TownEventShowFn)(void*, void*, void*, void*);
static TownEventShowFn g_origTownEventShow = nullptr;
static void**          g_teSlot            = nullptr;

static void* __fastcall TownEventShowDetour(void* self, void* a2, void* a3, void* a4) {
    __try { announceTownEventPopup(g_base, reinterpret_cast<uintptr_t>(self)); }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
    return g_origTownEventShow ? g_origTownEventShow(self, a2, a3, a4) : nullptr;
}
static void installTownEventHook(uintptr_t base) {
    if (!vftableIs(base, TE_VFTABLE_RVA, ".?AVTownEventDisplay@Panel@TownUI@@", "towneventhook")) return;
    void** slot = reinterpret_cast<void**>(base + TE_VFTABLE_RVA + (uintptr_t)TE_SHOW_SLOT * 8);
    DWORD old = 0;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) {
        logLine("towneventhook: VirtualProtect failed"); return;
    }
    g_origTownEventShow = reinterpret_cast<TownEventShowFn>(*slot);
    *slot = reinterpret_cast<void*>(&TownEventShowDetour);
    VirtualProtect(slot, sizeof(void*), old, &old);
    g_teSlot = slot;
    logLine("towneventhook: installed slot=0x%llx orig=0x%llx",
            (unsigned long long)slot, (unsigned long long)g_origTownEventShow);
}
// ---- Panel_Banner hook: capture the live RaidDisplay ----
typedef void* (__fastcall *BannerFn)(void*, void*, void*, void*);
static BannerFn g_origBanner   = nullptr;
static void**   g_bannerSlot   = nullptr;

static void* __fastcall BannerDetour(void* self, void* a2, void* a3, void* a4) {
    g_raidDisplay = a2;
    if (g_enabled && a2) clScanQueue(g_base, (uintptr_t)a2);
    return g_origBanner ? g_origBanner(self, a2, a3, a4) : nullptr;
}
static void installBannerHook(uintptr_t base) {
    if (!vftableIs(base, PANEL_BANNER_VFTABLE_RVA, ".?AVPanel_Banner@@", "bannerhook")) return;
    void** slot = reinterpret_cast<void**>(base + PANEL_BANNER_VFTABLE_RVA +
                                           (uintptr_t)PANEL_UPDATE_SLOT * 8);
    DWORD old = 0;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) {
        logLine("bannerhook: VirtualProtect failed"); return;
    }
    g_origBanner = reinterpret_cast<BannerFn>(*slot);
    *slot = reinterpret_cast<void*>(&BannerDetour);
    VirtualProtect(slot, sizeof(void*), old, &old);
    g_bannerSlot = slot;
    logLine("bannerhook: installed slot=0x%llx orig=0x%llx",
            (unsigned long long)slot, (unsigned long long)g_origBanner);
}
// ---- Popup-text hook: the ONLY way to get a damage number's TYPE ----

typedef void (__fastcall *PopupProcFn)(void*, unsigned int, float);
static PopupProcFn g_origPopupProc = nullptr;

static void __fastcall PopupProcDetour(void* rd, unsigned int idx, float dt) {
    if (g_enabled && rd) clCaptureEntry((uintptr_t)rd, idx);
    if (g_origPopupProc) g_origPopupProc(rd, idx, dt);
}

static void* allocNear(uintptr_t target) {
    SYSTEM_INFO si; GetSystemInfo(&si);
    uintptr_t gran = si.dwAllocationGranularity ? si.dwAllocationGranularity : 0x10000;
    for (uintptr_t delta = gran; delta < 0x40000000; delta += gran) {
        for (int dir = 0; dir < 2; dir++) {
            uintptr_t addr = dir ? (target + delta) : (target - delta);
            addr &= ~(uintptr_t)(gran - 1);
            if (addr < 0x10000) continue;
            void* p = VirtualAlloc((void*)addr, 64, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
            if (p) return p;
        }
    }
    return nullptr;
}

static void installPopupHook(uintptr_t base) {
    uintptr_t site = base + POPUP_CALLSITE_RVA;
    uintptr_t proc = base + POPUP_PROC_RVA;

    uint8_t op = 0; int32_t rel = 0;
    if (!safeReadU8(site, &op) || op != 0xE8) {
        logLine("popuphook: call site is not E8 (got 0x%02x) — NOT patching", op); return;
    }
    if (!safeReadU32(site + 1, (uint32_t*)&rel)) { logLine("popuphook: rel32 unreadable"); return; }
    if ((uintptr_t)(site + 5 + rel) != proc) {
        logLine("popuphook: call target 0x%llx != expected 0x%llx — NOT patching",
                (unsigned long long)(site + 5 + rel), (unsigned long long)proc);
        return;
    }

    void* stub = allocNear(site);
    if (!stub) { logLine("popuphook: no memory within reach of the call site"); return; }
    // mov rax, <detour> ; jmp rax
    uint8_t code[12] = { 0x48, 0xB8, 0,0,0,0,0,0,0,0, 0xFF, 0xE0 };
    uintptr_t d = (uintptr_t)&PopupProcDetour;
    memcpy(code + 2, &d, 8);
    memcpy(stub, code, sizeof code);

    int64_t newRel = (int64_t)((uintptr_t)stub) - (int64_t)(site + 5);
    if (newRel > 0x7fffffffLL || newRel < -0x80000000LL) {
        logLine("popuphook: stub out of rel32 range — NOT patching"); return;
    }

    g_origPopupProc = reinterpret_cast<PopupProcFn>(proc);
    DWORD old = 0;
    if (!VirtualProtect((void*)site, 5, PAGE_EXECUTE_READWRITE, &old)) {
        logLine("popuphook: VirtualProtect failed"); g_origPopupProc = nullptr; return;
    }
    int32_t r32 = (int32_t)newRel;
    memcpy((void*)(site + 1), &r32, 4);
    VirtualProtect((void*)site, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void*)site, 5);
    logLine("popuphook: installed — call at 0x%llx now -> stub 0x%llx -> detour 0x%llx (orig 0x%llx)",
            (unsigned long long)site, (unsigned long long)stub,
            (unsigned long long)d, (unsigned long long)proc);
}

static void OurStartText(void) {
    if (g_origStartText) g_origStartText();
    g_textInputActive = true;
    uintptr_t d = frontEndDisplay(g_base);
    logLine("SDL_StartTextInput frontEnd=%d sub=%d", d ? 1 : 0, feNamingSub(d));
}
static void OurStopText(void) {
    if (g_origStopText) g_origStopText();
    g_textInputActive = false;
    logLine("SDL_StopTextInput");
}

static void installTextHooks(uintptr_t base) {
    void* o = nullptr;
    if (patchIat(base, "SDL_StartTextInput", reinterpret_cast<void*>(&OurStartText),
                 &g_startSlot, &o)) g_origStartText = reinterpret_cast<VoidFn>(o);
    o = nullptr;
    if (patchIat(base, "SDL_StopTextInput", reinterpret_cast<void*>(&OurStopText),
                 &g_stopSlot, &o))  g_origStopText = reinterpret_cast<VoidFn>(o);
    logLine("text-input hooks: start=%d stop=%d", g_startSlot ? 1 : 0, g_stopSlot ? 1 : 0);
}

static const char* speechJoinSep(const char* buf, size_t len) {
    if (!len) return "";
    char last = buf[len - 1];
    return (last == '.' || last == '!' || last == '?' || last == ':' || last == ';'
         || last == ',' || last == '"') ? " " : ". ";
}

static int takeBarkCarry(char* buf, size_t& len) {
    int took = 0;
    while (took < g_barkCarryN) {
        const char* b = g_barkCarry[took];
        size_t blen = strlen(b);
        const char* sep = speechJoinSep(buf, len);
        size_t slen = strlen(sep);
        if (len + slen + blen >= (size_t)MAILBOX_SZ) break;
        memcpy(buf + len, sep, slen);  len += slen;
        memcpy(buf + len, b, blen);    len += blen;
        buf[len] = 0;
        took++;
    }
    if (took) {
        memmove(g_barkCarry[0], g_barkCarry[took], (size_t)(g_barkCarryN - took) * MAILBOX_SZ);
        g_barkCarryN -= took;
    }
    return took;
}

// ---- Speech + control thread (our own; no game state) ----
static DWORD WINAPI SpeechThread(LPVOID) {
    InitializeCriticalSection(&g_cs);
    axSpeechInit();
                      // on without speech rather than dying -- the old Tolk contract, kept.
    Sleep(300);

    g_base    = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    g_tbwVtbl = g_base + TBW_VFTABLE_RVA;

    // ---- The binary identity gate ----
    uint32_t peStamp = 0, peSize = 0;
    {
        auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(g_base);
        if (dos->e_magic == IMAGE_DOS_SIGNATURE) {
            auto nt = reinterpret_cast<IMAGE_NT_HEADERS*>(g_base + dos->e_lfanew);
            if (nt->Signature == IMAGE_NT_SIGNATURE) {
                peStamp = nt->FileHeader.TimeDateStamp;
                peSize  = nt->OptionalHeader.SizeOfImage;
            }
        }
    }
    if (axGameBuild() == AX_BUILD_UNKNOWN) {
        logLine("=== UNKNOWN GAME BINARY: PE stamp 0x%08x size 0x%x, the mod knows Steam "
                "0x%08x / 0x%x and DRM-free 0x%08x / 0x%x -- NO hooks installed, mod inactive ===",
                peStamp, peSize, GAME_PE_TIMESTAMP, GAME_PE_SIZEOFIMAGE,
                GAME_PE_TIMESTAMP_DRMFREE, GAME_PE_SIZEOFIMAGE_DRMFREE);
        char msg[256];
        _snprintf(msg, 256, "Accessibility mod: this version of Darkest Dungeon is not "
                  "supported yet (game build %08x, the mod expects %08x on Steam or %08x on "
                  "GOG). Mod inactive; the game runs without it.",
                  peStamp, GAME_PE_TIMESTAMP, GAME_PE_TIMESTAMP_DRMFREE);
        msg[255] = 0;
        speakUtf8(msg, true);
        Sleep(8000);                        // let the line finish before speech goes away
        axSpeechShutdown();
        DeleteCriticalSection(&g_cs);
        return 0;
    }
    logLine("=== game binary: %s build (PE stamp 0x%08x size 0x%x) ===",
            axGameBuildName(), peStamp, peSize);

    vftableIs(g_base, TBW_VFTABLE_RVA, ".?AVTextBoxWidget@@", "tbw-vftable");

    bool hooked = installPollHook(g_base);
    if (hooked) installTextHooks(g_base);   // best-effort naming open/close signal
    if (hooked) installMenuHook(g_base);    // capture the options-menu object (vtable slot)
    if (hooked) installTutorialHook(g_base);
    if (hooked) installTownEventHook(g_base);
    if (hooked) installJournalHook(g_base);
    if (hooked) installLightHook(g_base);
    if (hooked) installBannerHook(g_base);
    if (hooked) installPopupHook(g_base);
    g_enabled = hooked;
    logLine("=== focus announcer start (build %s %s / main-thread hook); base=0x%llx hooked=%d ===",
            __DATE__, __TIME__, (unsigned long long)g_base, hooked ? 1 : 0);

    if (!hooked) {
        speakUtf8("Accessibility mod could not hook the game. Mod inactive.", true);
        axSpeechShutdown();
        DeleteCriticalSection(&g_cs);
        return 0;
    }
    speakUtf8("Accessibility mod loaded.", true);

    DWORD handoverTick = 0;
    bool  sawSpeaking  = false;

    auto handOver = [&](const char* text, SpeechKind kind, size_t protectLen, DWORD now,
                        bool append = false) {
        size_t len = strlen(text);
        DWORD spoken = (DWORD)protectLen * SPEAK_MS_PER_CHAR;
        if (spoken < SPEAK_MS_FLOOR) spoken = SPEAK_MS_FLOOR;
        DWORD est;
        if (append) {
            est = spoken;
            DWORD until = g_speakUntil + spoken;
            if (until - now > SPEAK_MS_CAP) until = now + SPEAK_MS_CAP;
            g_speakUntil = until;
        } else {
            est = SPEAK_MS_LATENCY + spoken;
            if (est > SPEAK_MS_CAP) est = SPEAK_MS_CAP;
            g_speakUntil      = now + est;
            handoverTick      = now;      // arm the reality check for THIS utterance
            sawSpeaking       = false;
        }
        g_lastSpokenKind  = kind;

        int perr = -1;
        bool ok = speakUtf8(text, !append, &perr);
        const char* sr = axSpeechReaderName();
        logLine("speech->prism kind=%s%s est=%lu ok=%d len=%d err=%s speech=%d backend=%s \"%.60s\"",
                speechKindName(kind), append ? " polite=1 (chained)" : "", (unsigned long)est, ok ? 1 : 0,
                (int)len, axSpeechErrName(perr), axSpeechHasSpeech() ? 1 : 0,
                sr ? sr : "none", text);
    };

    for (;;) {
        Sleep(20);

        DWORD now = GetTickCount();

        if (g_silenceReq) {
            g_silenceReq = false;
            axSpeechSilence();
        }

        axSpeechServiceConfig();

        // ---- THE REALITY CHECK ----
        if (g_speakUntil) {
            int sp = axSpeechIsSpeaking();
            if (sp == 1) {
                sawSpeaking = true;
            } else if (sp == 0 && (sawSpeaking || now - handoverTick >= SPEAK_MS_LATENCY)) {
                if (g_hasPending)
                    logLine("speech: reader idle, hold released %ld ms early (saw_speaking=%d)",
                            (long)(g_speakUntil - now), sawSpeaking ? 1 : 0);
                g_speakUntil = 0;
            }
        }

        // ---- THE BARK CARRY: POLITE HANDOFF ----
        if (g_barkCarryN > 0 && g_speakUntil && (long)(now - g_speakUntil) < 0) {
            char bark[MAILBOX_SZ]; bark[0] = 0;
            size_t blen = 0;
            int took = 0;
            EnterCriticalSection(&g_cs);
            if (g_sqCount == 0) took = takeBarkCarry(bark, blen);
            LeaveCriticalSection(&g_cs);
            if (took) {
                DWORD spoken = (DWORD)blen * SPEAK_MS_PER_CHAR;   // no latency: it follows speech
                if (spoken < SPEAK_MS_FLOOR) spoken = SPEAK_MS_FLOOR;
                if (g_lastSpokenKind != SPK_EVENT) {
                    DWORD until = g_speakUntil + spoken;
                    if (until - now > SPEAK_MS_CAP) until = now + SPEAK_MS_CAP;
                    g_speakUntil     = until;
                    g_lastSpokenKind = SPK_CHATTER;
                }
                int perr = -1;
                bool ok = speakUtf8(bark, false, &perr);
                logLine("speech->prism kind=chatter polite=1 (%d bark(s) queued behind the line in "
                        "flight, %ld ms of its estimate left) ok=%d err=%s \"%.60s\"",
                        took, (long)(g_speakUntil - now), ok ? 1 : 0, axSpeechErrName(perr), bark);
            }
        }

        // ---- THE EVENT CHAIN ----
        bool chain = false;
        if (g_lastSpokenKind == SPK_EVENT && g_speakUntil && (long)(now - g_speakUntil) < 0) {
            EnterCriticalSection(&g_cs);
            chain = g_sqCount > 0 && g_sqKind[g_sqHead] == SPK_EVENT;
            LeaveCriticalSection(&g_cs);
        }
        if (!chain && g_lastSpokenKind == SPK_EVENT && g_speakUntil && (long)(now - g_speakUntil) < 0) {
            if (!g_holdLogged && g_hasPending) {
                g_holdLogged = true;
                logLine("speech: HOLDING %lu ms more for an event, %d line(s) waiting",
                        (unsigned long)(g_speakUntil - now), g_sqCount);
            }
            continue;
        }
        g_holdLogged = false;

        char local[MAILBOX_SZ]; bool has = false;
        SpeechKind kind = SPK_NAV;
        EnterCriticalSection(&g_cs);
        while (g_sqCount > 0) {
            kind = g_sqKind[g_sqHead];
            memcpy(local, g_speechQ[g_sqHead], sizeof local);
            g_sqHead = (g_sqHead + 1) % SPEECH_QUEUE_MAX;
            g_sqCount--;
            g_hasPending = (g_sqCount > 0);
            has = true;

            // ---- THE EVENT MERGE ----
            if (kind == SPK_EVENT) {
                size_t len = strlen(local);
                int merged = 1;
                while (merged < EVENT_MERGE_MAX_LINES && g_sqCount > 0
                       && g_sqKind[g_sqHead] == SPK_EVENT) {
                    const char* next = g_speechQ[g_sqHead];
                    size_t nlen = strlen(next);
                    if (!nlen) {
                        g_sqHead = (g_sqHead + 1) % SPEECH_QUEUE_MAX;
                        g_sqCount--;
                        continue;
                    }
                    const char* sep = speechJoinSep(local, len);
                    size_t slen = strlen(sep);
                    if (len + slen + nlen > (size_t)EVENT_MERGE_MAX_CHARS) break;   // policy bound
                    if (len + slen + nlen >= (size_t)MAILBOX_SZ) break;             // hard bound
                    memcpy(local + len, sep, slen);   len += slen;
                    memcpy(local + len, next, nlen);  len += nlen;
                    local[len] = 0;
                    g_sqHead = (g_sqHead + 1) % SPEECH_QUEUE_MAX;
                    g_sqCount--;
                    merged++;
                }
                g_hasPending = (g_sqCount > 0);
                if (merged > 1)
                    logLine("speech: merged %d event lines into one utterance (%d chars)",
                            merged, (int)len);
            }
            break;
        }
        // ---- THE BARK CARRY ----
        size_t protectLen = 0;
        int rode = 0;
        if (has) {
            size_t len = strlen(local);
            protectLen = len;
            if (g_barkCarryN > 0) {
                rode = takeBarkCarry(local, len);
                if (kind != SPK_EVENT) protectLen = len;   // an event's hold covers its own words only
            }
        } else if (g_barkCarryN > 0 && (!g_speakUntil || (long)(now - g_speakUntil) >= 0)) {
            size_t len = 0;
            local[0] = 0;
            if (takeBarkCarry(local, len)) {
                has = true;
                kind = SPK_CHATTER;
                protectLen = len;
            }
        }
        LeaveCriticalSection(&g_cs);
        if (rode)
            logLine("speech: %d carried bark(s) ride on the %s line", rode, speechKindName(kind));

        if (has) handOver(local, kind, protectLen, now, chain);
    }
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);
        CreateThread(nullptr, 0, SpeechThread, nullptr, 0, nullptr);
    }
    return TRUE;
}
