#pragma once
#include <cstdint>
#include <cstdio>
#include "game/layout_ids.h"

// Runs every plan against an image.

bool axResolverRun(const uint8_t* mem, uint64_t base);

// The plain facts of the last run.
struct AxResolverStats { int anchors, anchorsOk, layouts, layoutsOk; bool criticalOk; };
AxResolverStats axResolverStats();

bool axResolverUsable();

uintptr_t axResolvedRva(uintptr_t steamRva, bool zeroIfRefused);
uintptr_t axResolvedLayout(int id, uintptr_t refVal); // what axLay answers on an unknown build

// Every refused name, one per callback, for the debug log.
void axResolverForEachRefused(void (*fn)(const char* what, const char* name));

void axResolverDump(FILE* f);
