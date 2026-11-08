// game/x64decode.cpp -- a table-free x64 length + operand decoder.

#include "game/x64decode.h"
#include <cstring>

namespace {
enum { IMM_NONE, IMM_B, IMM_W, IMM_Z, IMM_V };
}

bool axDecode(const uint8_t* p, size_t avail, AxIns& o) {
    std::memset(&o, 0, sizeof o);
    o.base = o.index = AX_REG_NONE; o.reg = -1;
    if (avail == 0) return false;
    size_t i = 0;
    bool p66 = false, pF3 = false;
    for (;; ++i) {
        if (i > 13) return false;
        uint8_t b = p[i];
        if (b == 0x66) p66 = true;
        else if (b == 0xF3) pF3 = true;
        else if (b == 0xF2) pF3 = false;
        else if (b == 0x67 || b == 0x2E || b == 0x36 || b == 0x3E || b == 0x26 || b == 0x64 || b == 0x65 || b == 0xF0) {}
        else break;
    }
    uint8_t rex = 0;
    if ((p[i] & 0xF0) == 0x40) rex = p[i++];

    bool hasModrm = false, rmFirst = false;
    int imm = IMM_NONE, extra = 0;
    uint8_t reg3 = 0;                    // resolved after the modrm is read, for the groups
    int group = 0;                       // 1-byte group opcodes needing reg3
    uint8_t op = p[i++];
    bool twoByte = false, vex = false;
    uint8_t mapped = 0;                  // 1 = 0F, 2 = 0F38, 3 = 0F3A

    if (op == 0xC4 || op == 0xC5) {      // VEX (always, in 64-bit mode)
        vex = true;
        if (op == 0xC5) { uint8_t b = p[i++]; rex = (b & 0x80) ? 0 : 4; mapped = 1; }
        else {
            uint8_t b = p[i++], b2 = p[i++];
            rex = (uint8_t)(((b & 0x80) ? 0 : 4) | ((b & 0x40) ? 0 : 2) | ((b & 0x20) ? 0 : 1) | ((b2 & 0x80) ? 8 : 0));
            mapped = b & 0x1F;
        }
        op = p[i++];
    } else if (op == 0x0F) {
        twoByte = true; mapped = 1;
        op = p[i++];
        if (op == 0x38) { mapped = 2; op = p[i++]; }
        else if (op == 0x3A) { mapped = 3; op = p[i++]; }
    }

    if (mapped == 2) {
        hasModrm = true;
        if (op == 0xF1) rmFirst = true;
    } else if (mapped == 3) {
        hasModrm = true; imm = IMM_B;
        if (op >= 0x14 && op <= 0x17) rmFirst = true;
    } else if (mapped == 1) {
        if ((op >= 0x05 && op <= 0x09) || op == 0x0B || (op >= 0x30 && op <= 0x37) || op == 0x77 || op == 0xA0 || op == 0xA1 || op == 0xA2 ||
            op == 0xA8 || op == 0xA9 || op == 0xAA || (op >= 0xC8 && op <= 0xCF)) {
            if (vex && op == 0x77) {}
        } else if (op >= 0x80 && op <= 0x8F) {
            imm = IMM_Z;
        } else {
            hasModrm = true;
            switch (op) {
                case 0x00: case 0x01: case 0x0D:
                case 0x11: case 0x13: case 0x17: case 0x29: case 0x2B:
                case 0x71: case 0x72: case 0x73:
                case 0x7F: case 0xA3: case 0xA4: case 0xA5: case 0xAB: case 0xAC: case 0xAD: case 0xAE:
                case 0xB0: case 0xB1: case 0xB3: case 0xBA: case 0xBB: case 0xC0: case 0xC1: case 0xC3: case 0xC7:
                case 0xD6: case 0xE7:
                    rmFirst = true; break;
                case 0x7E: rmFirst = !pF3; break;
                default:
                    if ((op >= 0x18 && op <= 0x1F) || (op >= 0x90 && op <= 0x9F)) rmFirst = true;
                    break;
            }
            if (op == 0x70 || op == 0x71 || op == 0x72 || op == 0x73 || op == 0xA4 || op == 0xAC || op == 0xBA ||
                op == 0xC2 || op == 0xC4 || op == 0xC5 || op == 0xC6) imm = IMM_B;
        }
    } else if (op < 0x40) {
        int l = op & 7;
        if (l >= 6) return false;
        if (l <= 3) { hasModrm = true; rmFirst = l <= 1; }
        else imm = l == 4 ? IMM_B : IMM_Z;
        if (op >= 0x38 && op <= 0x3D) o.cls = AXI_CMP;
    } else {
        switch (op) {
            case 0x63: hasModrm = true; break;
            case 0x68: imm = IMM_Z; break;
            case 0x6A: imm = IMM_B; break;
            case 0x69: hasModrm = true; imm = IMM_Z; break;
            case 0x6B: hasModrm = true; imm = IMM_B; break;
            case 0x80: case 0x82: hasModrm = rmFirst = true; imm = IMM_B; group = 1; break;
            case 0x81: hasModrm = rmFirst = true; imm = IMM_Z; group = 1; break;
            case 0x83: hasModrm = rmFirst = true; imm = IMM_B; group = 1; break;
            case 0x84: case 0x85: hasModrm = rmFirst = true; o.cls = AXI_TEST; break;
            case 0x86: case 0x87: hasModrm = rmFirst = true; break;
            case 0x88: case 0x89: case 0x8C: hasModrm = rmFirst = true; o.cls = AXI_MOV; break;
            case 0x8A: case 0x8B: case 0x8E: hasModrm = true; o.cls = AXI_MOV; break;
            case 0x8D: hasModrm = true; o.cls = AXI_LEA; break;
            case 0x8F: hasModrm = rmFirst = true; break;
            case 0xA0: case 0xA1: case 0xA2: case 0xA3: extra = 8; break;
            case 0xA8: imm = IMM_B; o.cls = AXI_TEST; break;
            case 0xA9: imm = IMM_Z; o.cls = AXI_TEST; break;
            case 0xC0: case 0xC1: hasModrm = rmFirst = true; imm = IMM_B; break;
            case 0xC2: case 0xCA: imm = IMM_W; break;
            case 0xC6: hasModrm = rmFirst = true; imm = IMM_B; group = 2; break;
            case 0xC7: hasModrm = rmFirst = true; imm = IMM_Z; group = 2; break;
            case 0xC8: extra = 3; break;
            case 0xCD: imm = IMM_B; break;
            case 0xD0: case 0xD1: case 0xD2: case 0xD3: hasModrm = rmFirst = true; break;
            case 0xD8: case 0xD9: case 0xDA: case 0xDB: case 0xDC: case 0xDD: case 0xDE: case 0xDF: hasModrm = rmFirst = true; break;
            case 0xE0: case 0xE1: case 0xE2: case 0xE3: case 0xE4: case 0xE5: case 0xE6: case 0xE7: imm = IMM_B; break;
            case 0xE8: imm = IMM_Z; o.cls = AXI_CALL; break;
            case 0xE9: imm = IMM_Z; o.cls = AXI_JMP; break;
            case 0xEB: imm = IMM_B; o.cls = AXI_JMP; break;
            case 0xF6: case 0xF7: hasModrm = rmFirst = true; group = 3; break;
            case 0xFE: hasModrm = rmFirst = true; break;
            case 0xFF: hasModrm = rmFirst = true; group = 4; break;
            default:
                if (op >= 0x70 && op <= 0x7F) imm = IMM_B;
                else if (op >= 0xB0 && op <= 0xB7) { imm = IMM_B; o.cls = AXI_MOV; }
                else if (op >= 0xB8 && op <= 0xBF) { imm = IMM_V; if (!(rex & 8)) o.cls = AXI_MOV; }
                break;
        }
    }

    if (hasModrm) {
        uint8_t m = p[i++];
        int mod = m >> 6, rm = m & 7;
        reg3 = (m >> 3) & 7;
        o.reg = (int8_t)(reg3 | ((rex & 4) << 1));
        if (mod != 3) {
            o.hasMem = true;
            int dispSize = mod == 1 ? 1 : mod == 2 ? 4 : 0;
            if (rm == 4) {
                uint8_t s = p[i++];
                int idx = ((s >> 3) & 7) | ((rex & 2) << 2), bs = s & 7;
                o.index = idx == 4 ? (int8_t)AX_REG_NONE : (int8_t)idx;
                if (bs == 5 && mod == 0) { o.base = AX_REG_NONE; dispSize = 4; }
                else o.base = (int8_t)(bs | ((rex & 1) << 3));
            } else if (rm == 5 && mod == 0) {
                o.base = AX_REG_RIP; dispSize = 4;
            } else {
                o.base = (int8_t)(rm | ((rex & 1) << 3));
            }
            if (dispSize == 1) o.disp = (int8_t)p[i];
            else if (dispSize == 4) { int32_t d; std::memcpy(&d, p + i, 4); o.disp = d; }
            i += dispSize;
            if (rmFirst) o.op0Mem = true; else o.op1Mem = true;
        }
        if (group == 1 && reg3 == 7) o.cls = AXI_CMP;
        if (group == 2 && reg3 == 0) o.cls = AXI_MOV;
        if (group == 3) {
            if (reg3 <= 1) { o.cls = AXI_TEST; imm = op == 0xF6 ? IMM_B : IMM_Z; }
        }
        if (group == 4) {
            if (reg3 == 2) o.cls = AXI_CALL;
            else if (reg3 == 4) o.cls = AXI_JMP;
        }
    }

    size_t immBytes = 0;
    switch (imm) {
        case IMM_B: immBytes = 1; break;
        case IMM_W: immBytes = 2; break;
        case IMM_Z: immBytes = (rex & 8) ? 4 : p66 ? 2 : 4; break;
        case IMM_V: immBytes = (rex & 8) ? 8 : p66 ? 2 : 4; break;
        default: break;
    }
    if ((!twoByte && !vex && (op == 0xE8 || op == 0xE9)) || (twoByte && mapped == 1 && op >= 0x80 && op <= 0x8F)) immBytes = 4;
    if (!twoByte && !vex && (op == 0xE8 || op == 0xE9)) {
        o.rel32 = true;
        std::memcpy(&o.rel, p + i, 4);
    }
    i += immBytes + extra;
    if (i > 15 || i > avail) return false;
    o.len = (uint8_t)i;
    return true;
}
