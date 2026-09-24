// core/axstrings.h -- ids and access for the mod's OWN spoken strings (the ones the game has no
// localization key for).

#pragma once
#include <cstdint>

enum AxStrId {
#define AXS(id, en) AXS_##id,
#include "core/axstrings.inc"
#undef AXS
    AXS__COUNT
};

const char* axs(AxStrId id);

const char* axsKeyName(AxStrId id);

void axLangService(uintptr_t base);
