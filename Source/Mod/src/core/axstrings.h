// core/axstrings.h -- ids and access for the mod's OWN spoken strings (the ones the game has no
// localization key for).

#pragma once
#include <cstdint>

enum AxStrId {
#define AXS(id, en) AXS_##id,
#define AXH(id, en) AXS_##id,
#include "core/axstrings.inc"
#undef AXS
#undef AXH
    AXS__COUNT
};

const char* axs(AxStrId id);

const char* axsKeyName(AxStrId id);

void axAppendHint(char* buf, int bufsz, AxStrId hint);
void axAppendHintText(char* buf, int bufsz, const char* hint);
bool axEndsSentence(const char* s, size_t n);
void axAppendHintToLast(AxStrId hint);

void axLangService(uintptr_t base);
bool axLangLatched();
