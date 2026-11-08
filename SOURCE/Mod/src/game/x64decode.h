// game/x64decode.h -- the small x64 decoder the runtime resolver reads game code with.

#pragma once
#include <cstddef>
#include <cstdint>

enum AxInsClass : uint8_t { AXI_OTHER, AXI_LEA, AXI_MOV, AXI_CALL, AXI_JMP, AXI_CMP, AXI_TEST };
enum { AX_REG_NONE = -1, AX_REG_RCX = 1, AX_REG_RSP = 4, AX_REG_RBP = 5, AX_REG_RIP = 16 };

struct AxIns {
    uint8_t    len;
    AxInsClass cls;
    bool       hasMem;        // a modrm memory operand exists
    bool       op0Mem;
    bool       op1Mem;        // ... and it is the second operand (a load / lea)
    int8_t     base, index;   // AX_REG_*, or 0-15
    int64_t    disp;          // sign-extended
    int8_t     reg;
    bool       rel32;         // E8 / E9 with a 4-byte displacement
    int32_t    rel;
};

// Decodes one instruction at p. False: not decodable, or longer than avail.
bool axDecode(const uint8_t* p, size_t avail, AxIns& out);
