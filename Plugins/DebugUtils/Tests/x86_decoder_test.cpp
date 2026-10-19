// Host unit test for DebugUtils.X86Decoder.h.
//
// The decoder computes how many prologue bytes a 5-byte JMP detour must copy
// into a trampoline. A wrong length silently corrupts the game's code, so this
// is the one part of DebugUtils worth a real test. It has no Windows dependency,
// so it builds and runs on the host:
//
//   g++ -std=c++17 -I Plugins/DebugUtils Plugins/DebugUtils/Tests/x86_decoder_test.cpp -o /tmp/x86_decoder_test
//   /tmp/x86_decoder_test
//
// Exit code 0 = all checks passed, 1 = at least one failed.

#include "DebugUtils.X86Decoder.h"

#include <cstdio>
#include <cstring>

using namespace DebugUtilsX86;

static int g_failures = 0;
static int g_checks = 0;

static void CheckEq(const char* what, long long got, long long want)
{
    ++g_checks;
    if (got != want)
    {
        ++g_failures;
        std::printf("FAIL %-40s got %lld, want %lld\n", what, got, want);
    }
    else
    {
        std::printf("ok   %-40s = %lld\n", what, got);
    }
}

static int Len(const unsigned char* bytes, int count)
{
    // Decode one instruction from a byte array; count is only for clarity.
    (void)count;
    int rel = -1;
    return InstructionLengthEx(bytes, &rel);
}

int main()
{
    // ---- ModRmLength ----
    // mod==3 (register operand): 1 byte.
    CheckEq("modrm mod3", ModRmLength(reinterpret_cast<const unsigned char*>("\xC3")), 1);

    // rm==5, mod==0: [disp32] -> 5 bytes.
    {
        const unsigned char b[] = {0x05, 0, 0, 0, 0};
        CheckEq("modrm [disp32]", ModRmLength(b), 5);
    }
    // mod==1: modrm + disp8 -> 2.
    {
        const unsigned char b[] = {0x41, 0x00};
        CheckEq("modrm mod1 disp8", ModRmLength(b), 2);
    }
    // mod==2: modrm + disp32 -> 5.
    {
        const unsigned char b[] = {0x81, 0, 0, 0, 0};
        CheckEq("modrm mod2 disp32", ModRmLength(b), 5);
    }
    // SIB, mod==0, base==5: [disp32] -> 6 (the historical bug read 2).
    {
        const unsigned char b[] = {0x04, 0x25, 0, 0, 0, 0};
        CheckEq("modrm sib mod0 base5", ModRmLength(b), 6);
    }
    // SIB, mod==0, base!=5: no disp -> 2.
    {
        const unsigned char b[] = {0x04, 0x08};
        CheckEq("modrm sib mod0 base!=5", ModRmLength(b), 2);
    }
    // SIB, mod==1: modrm + sib + disp8 -> 3.
    {
        const unsigned char b[] = {0x44, 0x08, 0x00};
        CheckEq("modrm sib mod1", ModRmLength(b), 3);
    }
    // SIB, mod==2: modrm + sib + disp32 -> 6.
    {
        const unsigned char b[] = {0x84, 0x08, 0, 0, 0, 0};
        CheckEq("modrm sib mod2", ModRmLength(b), 6);
    }

    // ---- InstructionLengthEx: common MSVC prologue shapes ----
    // push ebp
    {
        const unsigned char b[] = {0x55};
        CheckEq("push ebp", Len(b, 1), 1);
    }
    // mov ebp, esp
    {
        const unsigned char b[] = {0x8B, 0xEC};
        CheckEq("mov ebp,esp", Len(b, 2), 2);
    }
    // sub esp, imm8
    {
        const unsigned char b[] = {0x83, 0xEC, 0x10};
        CheckEq("sub esp,imm8", Len(b, 3), 3);
    }
    // sub esp, imm32
    {
        const unsigned char b[] = {0x81, 0xEC, 0x10, 0x00, 0x00, 0x00};
        CheckEq("sub esp,imm32", Len(b, 6), 6);
    }
    // push imm8 (0x6A)
    {
        const unsigned char b[] = {0x6A, 0x08};
        CheckEq("push imm8", Len(b, 2), 2);
    }
    // push imm32 (0x68)
    {
        const unsigned char b[] = {0x68, 0, 0, 0, 0};
        CheckEq("push imm32", Len(b, 5), 5);
    }
    // mov eax,[disp32] (0xA1)
    {
        const unsigned char b[] = {0xA1, 0, 0, 0, 0};
        CheckEq("mov eax,[disp32]", Len(b, 5), 5);
    }
    // call rel32 (0xE8) - relocatable via relWithin, length 5.
    {
        const unsigned char b[] = {0xE8, 0x11, 0x22, 0x33, 0x44};
        int rel = -1;
        const int n = InstructionLengthEx(b, &rel);
        CheckEq("call rel32 length", n, 5);
        CheckEq("call rel32 relWithin", rel, 1);
    }
    // jmp rel32 (0xE9)
    {
        const unsigned char b[] = {0xE9, 0x00, 0x00, 0x00, 0x00};
        int rel = -1;
        const int n = InstructionLengthEx(b, &rel);
        CheckEq("jmp rel32 length", n, 5);
        CheckEq("jmp rel32 relWithin", rel, 1);
    }
    // A prefix followed by a normal opcode: F3 0F is refused because 0x0F.
    {
        const unsigned char b[] = {0xF3, 0x0F, 0x10, 0x00};
        CheckEq("F3 0F.. refused", Len(b, 4), 0);
    }
    // Operand-size prefix is refused.
    {
        const unsigned char b[] = {0x66, 0x8B, 0xEC};
        CheckEq("66 prefix refused", Len(b, 3), 0);
    }
    // Address-size prefix is refused.
    {
        const unsigned char b[] = {0x67, 0x8B, 0xEC};
        CheckEq("67 prefix refused", Len(b, 3), 0);
    }
    // Two-byte opcode map is refused.
    {
        const unsigned char b[] = {0x0F, 0x84, 0, 0, 0, 0};
        CheckEq("0F two-byte refused", Len(b, 6), 0);
    }

    // ---- PrologueLengthForJump ----
    // push ebp; mov ebp,esp; push -1; push imm32... -> first 5 bytes.
    {
        // 55             push ebp              (1)
        // 8B EC          mov ebp, esp          (2) -> total 3
        // 83 EC 10       sub esp, 10h          (3) -> total 6 >= 5
        const unsigned char b[] = {0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x10};
        int relOff[4] = {0};
        int relCount = 0;
        const int n = PrologueLengthForJump(b, 5, relOff, &relCount, 4);
        CheckEq("prologue >=5", n, 6);
        CheckEq("prologue relCount", relCount, 0);
    }
    // A prologue containing a call must record the rel32 offset for re-basing.
    {
        // 55             push ebp              (1)
        // E8 rel32       call rel32            (5) -> total 6
        const unsigned char b[] = {0x55, 0xE8, 0x00, 0x00, 0x00, 0x00};
        int relOff[4] = {0};
        int relCount = 0;
        const int n = PrologueLengthForJump(b, 5, relOff, &relCount, 4);
        CheckEq("prologue with call", n, 6);
        CheckEq("prologue relCount(call)", relCount, 1);
        CheckEq("prologue relOff(call)", relOff[0], 2);
    }
    // An undecodable prologue returns 0 (the caller then refuses to hook).
    {
        const unsigned char b[] = {0x0F, 0x10, 0x00, 0x00, 0x00};
        int relOff[4] = {0};
        int relCount = 0;
        const int n = PrologueLengthForJump(b, 5, relOff, &relCount, 4);
        CheckEq("prologue undecodable", n, 0);
    }

    std::printf("\n%d/%d checks passed\n", g_checks - g_failures, g_checks);
    return g_failures == 0 ? 0 : 1;
}
