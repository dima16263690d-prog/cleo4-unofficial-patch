#pragma once


// Minimal x86 prologue length decoder, for the exact-caller entry hook.
//
// A 5-byte JMP detour must copy WHOLE instructions out of the target's prologue
// into a trampoline, so we need the byte length of the first instructions until
// we have >= 5 bytes. It only accepts instructions that are safe to relocate (no
// relative branch, which would change meaning when moved). Anything it does not
// fully understand returns 0, and the caller then refuses to hook - falling back
// to the labelled stack scan. Being conservative here is the whole safety story:
// a wrong length corrupts the game's code, a refused decode just disables a
// feature.
//
// Kept in its own header, with no DebugUtils state, so it can be unit-tested on
// the host (see Tests/x86_decoder_test.cpp). This is the one piece of the plugin
// where a bug is silent memory corruption rather than a wrong log line.
namespace DebugUtilsX86
{
    // x86 ModRM: returns the number of bytes the modrm(+sib+disp) consumes for
    // the instruction whose modrm byte is at `p` (0 when the byte is not a valid
    // modrm). Reads p[1] when a SIB byte follows, so `p` must point at readable
    // code with at least 2 bytes.
    inline int ModRmLength(const unsigned char* p)
    {
        const unsigned char modrm = p[0];
        const unsigned char mod = (modrm >> 6) & 3;
        const unsigned char rm = modrm & 7;

        if (mod == 3)
            return 1; // register operand, no disp

        if (rm == 4)
        {
            // SIB follows. For mod==0 the displacement depends on the SIB base
            // field: base==5 means [disp32] with no base register, so a disp32
            // still follows the SIB byte. Missing that read 2 instead of 6 and
            // corrupted the trampoline.
            const unsigned char base = p[1] & 7;
            if (mod == 1)
                return 3; // modrm + sib + disp8
            if (mod == 2)
                return 6; // modrm + sib + disp32
            return (base == 5) ? 6 : 2; // mod==0: [disp32] iff base==5, else no disp
        }

        if (rm == 5 && mod == 0)
            return 5; // [disp32]
        if (mod == 1)
            return 2; // modrm + disp8
        return 5;     // mod==2: modrm + disp32
    }

    // Length of the instruction at `code`, or 0 when it is not one we are willing
    // to relocate. Covers the one-byte opcode map (the forms MSVC prologues use),
    // including modrm/immediate forms. Relative branches are handled by the caller
    // (rel32 via *relWithin) or refused (rel8, far). Two-byte (0x0F) opcodes and
    // operand/address-size prefixes are refused: a miss only disables the hook,
    // while a wrong length would corrupt the game's code.
    //
    // *relWithin, when non-null, is set to the offset (from the start of this
    // instruction, prefixes included) of a rel32 operand to fix up after the copy,
    // or -1 when there is none.
    inline int InstructionLengthEx(const unsigned char* code, int* relWithin)
    {
        if (relWithin != nullptr)
            *relWithin = -1;

        int i = 0;
        for (;;) // legacy prefixes (no operand/address-size override: refuse those)
        {
            const unsigned char b = code[i];
            if (b == 0x66 || b == 0x67)
                return 0;
            if (b == 0xF0 || b == 0xF2 || b == 0xF3 ||
                b == 0x2E || b == 0x36 || b == 0x3E || b == 0x26 ||
                b == 0x64 || b == 0x65)
            {
                ++i;
                continue;
            }
            break;
        }

        const unsigned char op = code[i];
        int n = 0; // bytes after the prefixes

        if (op == 0x0F)
            return 0; // two-byte opcode map: refused

        // 0x00..0x3F ALU family (add/or/adc/sbb/and/sub/xor/cmp).
        {
            const int d = op & 0x07;
            const int hi = op >> 3;
            if (hi <= 7 && (op & 0xC0) == 0)
            {
                // op is one of 0x00..0x3D with the segment-prefix bytes (0x26,
                // 0x2E, 0x36, 0x3E) already consumed above.
                if (d == 6 || d == 7)
                    return 0; // 0x06/0x0E/... push/pop segment - not a prologue shape
                if (d == 4)
                    n = 2;      // AL, imm8
                else if (d == 5)
                    n = 5;      // EAX, imm32
                else
                {
                    const int m = ModRmLength(code + i + 1);
                    if (m == 0)
                        return 0;
                    n = 1 + m;
                }
                return i + n;
            }
        }

        // 0x40-0x4F inc/dec r32.
        if (op >= 0x40 && op <= 0x4F) return i + 1;
        // 0x50-0x5F push/pop r32.
        if (op >= 0x50 && op <= 0x5F) return i + 1;
        if (op == 0x60 || op == 0x61) return i + 1; // pushad/popad
        if (op == 0x63) // arpl r/m16, r16
        {
            const int m = ModRmLength(code + i + 1);
            return m == 0 ? 0 : i + 1 + m;
        }
        if (op == 0x68) return i + 5; // push imm32
        if (op == 0x69)               // imul r32, r/m32, imm32
        {
            const int m = ModRmLength(code + i + 1);
            return m == 0 ? 0 : i + 1 + m + 4;
        }
        if (op == 0x6A) return i + 2; // push imm8
        if (op == 0x6B)               // imul r32, r/m32, imm8
        {
            const int m = ModRmLength(code + i + 1);
            return m == 0 ? 0 : i + 1 + m + 1;
        }
        if (op >= 0x6C && op <= 0x6F) return i + 1; // ins/outs
        if (op >= 0x70 && op <= 0x7F) return 0;     // jcc rel8: refuse

        if (op == 0x80 || op == 0x83) // group1 r/m, imm8
        {
            const int m = ModRmLength(code + i + 1);
            return m == 0 ? 0 : i + 1 + m + 1;
        }
        if (op == 0x81) // group1 r/m32, imm32
        {
            const int m = ModRmLength(code + i + 1);
            return m == 0 ? 0 : i + 1 + m + 4;
        }
        if (op == 0x84 || op == 0x85 || op == 0x86 || op == 0x87) // test/xchg
        {
            const int m = ModRmLength(code + i + 1);
            return m == 0 ? 0 : i + 1 + m;
        }
        if (op >= 0x88 && op <= 0x8E) // mov family (r/m <-> r, sreg)
        {
            const int m = ModRmLength(code + i + 1);
            return m == 0 ? 0 : i + 1 + m;
        }
        if (op == 0x8F) // pop r/m32
        {
            const int m = ModRmLength(code + i + 1);
            return m == 0 ? 0 : i + 1 + m;
        }
        if (op == 0x90) return i + 1; // nop
        if (op >= 0x91 && op <= 0x97) return i + 1; // xchg eax, r32
        if (op == 0x98 || op == 0x99 || op == 0x9B || op == 0x9C ||
            op == 0x9D || op == 0x9E || op == 0x9F)
            return i + 1;
        if (op == 0x9A) return 0; // far call: refuse

        if (op >= 0xA0 && op <= 0xA3) return i + 5; // mov moffs32
        if (op >= 0xA4 && op <= 0xA7) return i + 1; // movs/cmps
        if (op == 0xA8) return i + 2;               // test al, imm8
        if (op == 0xA9) return i + 5;               // test eax, imm32
        if (op >= 0xAA && op <= 0xAF) return i + 1; // stos/lods/scas

        if (op >= 0xB0 && op <= 0xB7) return i + 2; // mov r8, imm8
        if (op >= 0xB8 && op <= 0xBF) return i + 5; // mov r32, imm32

        if (op == 0xC0 || op == 0xC1) // shift r/m, imm8
        {
            const int m = ModRmLength(code + i + 1);
            return m == 0 ? 0 : i + 1 + m + 1;
        }
        if (op == 0xC2) return i + 3; // ret imm16
        if (op == 0xC3) return i + 1; // ret
        if (op == 0xC4 || op == 0xC5) return 0; // les/lds (or VEX): refuse
        if (op == 0xC6) // mov r/m8, imm8
        {
            const int m = ModRmLength(code + i + 1);
            return m == 0 ? 0 : i + 1 + m + 1;
        }
        if (op == 0xC7) // mov r/m32, imm32
        {
            const int m = ModRmLength(code + i + 1);
            return m == 0 ? 0 : i + 1 + m + 4;
        }
        if (op == 0xC8) return i + 4; // enter imm16, imm8
        if (op == 0xC9) return i + 1; // leave
        if (op == 0xCA) return i + 3; // retf imm16
        if (op == 0xCB || op == 0xCC || op == 0xCE || op == 0xCF) return i + 1;
        if (op == 0xCD) return i + 2; // int imm8

        if (op == 0xD0 || op == 0xD1 || op == 0xD2 || op == 0xD3) // shift
        {
            const int m = ModRmLength(code + i + 1);
            return m == 0 ? 0 : i + 1 + m;
        }
        if (op == 0xD4 || op == 0xD5) return i + 2; // aam/aad
        if (op == 0xD6 || op == 0xD7) return i + 1;
        if (op >= 0xD8 && op <= 0xDF) // x87 FPU
        {
            const int m = ModRmLength(code + i + 1);
            return m == 0 ? 0 : i + 1 + m;
        }

        if (op >= 0xE0 && op <= 0xE3) return 0; // loop/jcxz rel8: refuse
        if (op == 0xE4 || op == 0xE5) return i + 2; // in imm8
        if (op == 0xE6 || op == 0xE7) return i + 2; // out imm8
        if (op == 0xE8 || op == 0xE9) // call/jmp rel32: relocatable with a fixup
        {
            if (relWithin != nullptr)
                *relWithin = i + 1;
            return i + 5;
        }
        if (op == 0xEA) return 0; // far jmp: refuse
        if (op == 0xEB) return 0; // jmp rel8: refuse
        if (op == 0xEC || op == 0xED || op == 0xEE || op == 0xEF) return i + 1;

        if (op == 0xF1 || op == 0xF4 || op == 0xF5) return i + 1;
        if (op == 0xF6) // group3 r/m8 (test/not/neg/...)
        {
            const int m = ModRmLength(code + i + 1);
            if (m == 0)
                return 0;
            const int reg = (code[i + 1] >> 3) & 7;
            return i + 1 + m + ((reg == 0 || reg == 1) ? 1 : 0);
        }
        if (op == 0xF7) // group3 r/m32
        {
            const int m = ModRmLength(code + i + 1);
            if (m == 0)
                return 0;
            const int reg = (code[i + 1] >> 3) & 7;
            return i + 1 + m + ((reg == 0 || reg == 1) ? 4 : 0);
        }
        if (op >= 0xF8 && op <= 0xFD) return i + 1;
        if (op == 0xFE) // group4 (inc/dec r/m8)
        {
            const int m = ModRmLength(code + i + 1);
            return m == 0 ? 0 : i + 1 + m;
        }
        if (op == 0xFF) // group5 (indirect call/jmp/push are position-independent)
        {
            const int m = ModRmLength(code + i + 1);
            return m == 0 ? 0 : i + 1 + m;
        }

        return 0; // unknown opcode: refuse
    }

    // Number of whole bytes to copy so that at least `need` bytes are covered, or
    // 0 when the prologue contains something we cannot relocate. Rel32 operands
    // found on the way are appended to `relOffsets` (offsets from `code`), so the
    // caller can fix them up after moving the bytes into the trampoline.
    inline int PrologueLengthForJump(const unsigned char* code, int need,
                              int* relOffsets, int* relCount, int maxRel)
    {
        int total = 0;
        if (relCount != nullptr)
            *relCount = 0;
        while (total < need)
        {
            int relWithin = -1;
            const int len = InstructionLengthEx(code + total, &relWithin);
            if (len <= 0)
                return 0;
            if (relWithin >= 0 && relCount != nullptr && *relCount < maxRel)
                relOffsets[(*relCount)++] = total + relWithin;
            total += len;
            if (total > 32) // sanity bound
                return 0;
        }
        return total;
    }
}
