// Research tracer: logs when selected game functions are called.
//
// Used to find out which of the console (AI script) menu functions still run in
// the PC build and when the PC front-end opens its own mouse pages. Each hook
// is a plain inline detour: the verified prologue is copied into a trampoline
// and replaced by a jump to a small generated stub that reports the call.

#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <string.h>
#include "trace.h"

void Log(const char* fmt, ...);

namespace {

struct Hook {
    const char* name;
    DWORD addr;
    unsigned char prologue[8];
    int len;              // bytes relocated into the trampoline (>= 5, whole instructions)
    bool logPageArg;      // first stack argument is a PC menu page index
    unsigned calls;
    unsigned logged;
};

Hook g_hooks[] = {
    { "ai menu script A",       0x0053f5c0, { 0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF8 }, 6 },
    { "ai menu script B",       0x0059f140, { 0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF8 }, 6 },
    { "ai page add fadable",    0x00528be0, { 0x83, 0xEC, 0x10, 0xA1, 0x34, 0xC9, 0xAB, 0x00 }, 8 },
    { "ai page item 528e00",    0x00528e00, { 0x83, 0xEC, 0x08, 0xA1, 0x28, 0xC9, 0xAB, 0x00 }, 8 },
    { "ai page item 528ef0",    0x00528ef0, { 0x83, 0xEC, 0x08, 0xA1, 0x28, 0xC9, 0xAB, 0x00 }, 8 },
    { "ai page add togglable",  0x0052d660, { 0x83, 0xEC, 0x08, 0xA1, 0x28, 0xC9, 0xAB, 0x00 }, 8 },
    { "ai lib page add fadable", 0x005f92d0, { 0x83, 0xEC, 0x08, 0xA1, 0x28, 0xC9, 0xAB, 0x00 }, 8 },
    { "pc menu GetPage",        0x00409790, { 0x56, 0x8B, 0x74, 0x24, 0x08 }, 5, true },
};
const int kNumHooks = sizeof(g_hooks) / sizeof(g_hooks[0]);
const unsigned kMaxLoggedCalls = 12;

const char* const* const kPcPageNames = (const char* const*)0x007f0250;

// frame = pushad frame; the original ESP (return address) is 32 bytes above it.
void __cdecl OnCall(int idx, DWORD* frame)
{
    Hook& h = g_hooks[idx];
    h.calls++;
    if (h.logged >= kMaxLoggedCalls) return;
    h.logged++;
    DWORD* esp = frame + 8;
    DWORD ret = esp[0], arg0 = esp[1];
    DWORD ecx = frame[6];  // pushad order: edi esi ebp esp ebx edx ecx eax
    if (h.logPageArg && arg0 < 25)
        Log("trace: %s(%lu = %s) from %08lx", h.name, arg0, kPcPageNames[arg0], ret);
    else
        Log("trace: %s from %08lx (ecx %08lx, arg0 %08lx)", h.name, ret, ecx, arg0);
}

void WriteRel32(unsigned char* at, unsigned char opcode, const void* target)
{
    at[0] = opcode;
    *(DWORD*)(at + 1) = (DWORD)target - (DWORD)(at + 5);
}

bool Install(int idx, unsigned char* mem)
{
    Hook& h = g_hooks[idx];
    unsigned char* fn = (unsigned char*)h.addr;
    if (memcmp(fn, h.prologue, h.len) != 0) {
        Log("trace: prologue mismatch for %s at %08lx, not hooked", h.name, h.addr);
        return false;
    }
    // trampoline: original prologue, then jump back behind it
    unsigned char* tramp = mem;
    memcpy(tramp, fn, h.len);
    WriteRel32(tramp + h.len, 0xE9, fn + h.len);

    // stub: pushad; push esp; push idx; call OnCall; add esp, 8; popad; jmp tramp
    unsigned char* stub = mem + 32;
    unsigned char* p = stub;
    *p++ = 0x60;
    *p++ = 0x54;
    *p++ = 0x68; *(DWORD*)p = (DWORD)idx; p += 4;
    WriteRel32(p, 0xE8, (void*)OnCall); p += 5;
    *p++ = 0x83; *p++ = 0xC4; *p++ = 0x08;
    *p++ = 0x61;
    WriteRel32(p, 0xE9, tramp);

    DWORD prot;
    VirtualProtect(fn, h.len, PAGE_EXECUTE_READWRITE, &prot);
    WriteRel32(fn, 0xE9, stub);
    for (int i = 5; i < h.len; i++) fn[i] = 0x90;
    VirtualProtect(fn, h.len, prot, &prot);
    FlushInstructionCache(GetCurrentProcess(), fn, h.len);
    return true;
}

}  // namespace

void Trace_Install()
{
    static bool done;
    if (done) return;
    done = true;
    unsigned char* mem = (unsigned char*)VirtualAlloc(nullptr, kNumHooks * 64, MEM_COMMIT | MEM_RESERVE,
                                                      PAGE_EXECUTE_READWRITE);
    if (!mem) return;
    int n = 0;
    for (int i = 0; i < kNumHooks; i++)
        if (Install(i, mem + i * 64)) n++;
    Log("trace: %d of %d hooks installed", n, kNumHooks);
}

// Console menu manager (PC 0xaf2414, Xbox 0x7584f0): +0x14/+0x18 is the vector of
// menu controls the Xbox menu tick lets process input every frame; the PC tick no
// longer does. Log its contents whenever they change.
void Trace_ProbeMenuManager()
{
    static DWORD lastSig;
    __try {
        char* mgr = *(char**)0x00af2414;
        if (!mgr) return;
        DWORD* begin = *(DWORD**)(mgr + 0x14);
        DWORD* end = *(DWORD**)(mgr + 0x18);
        int open = *(int*)(mgr + 0xf0);
        int count = (begin && end > begin) ? (int)(end - begin) : 0;
        DWORD sig = (DWORD)count * 31 + (DWORD)open * 7 + (DWORD)begin;
        for (int i = 0; i < count && i < 64; i++) sig = sig * 131 + begin[i];
        for (int i = 1; i <= open && i < 32; i++) {
            DWORD page = *(DWORD*)(mgr + 0x6c + i * 4);
            sig = sig * 131 + page + (page ? *(DWORD*)(page + 0x84) : 0);
        }
        if (sig == lastSig) return;
        lastSig = sig;
        Log("probe: menu manager %p, %d controls, %d open pages", mgr, count, open);
        for (int i = 0; i < count && i < 64; i++) {
            DWORD obj = begin[i];
            DWORD vt = obj ? *(DWORD*)obj : 0;
            DWORD* v = (DWORD*)vt;
            if (vt) Log("  [%d] %08lx vt %08lx: %08lx %08lx %08lx %08lx", i, obj, vt, v[0], v[1], v[2], v[3]);
            else Log("  [%d] %08lx", i, obj);
        }
        for (int i = 1; i <= open && i < 32; i++) {
            DWORD page = *(DWORD*)(mgr + 0x6c + i * 4);
            Log("  open page %d: %08lx state %d", i, page, page ? *(int*)(page + 0x84) : -1);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("probe: exception while reading the menu manager");
    }
}

void Trace_Dump()
{
    static unsigned last[sizeof(g_hooks) / sizeof(g_hooks[0])];
    for (int i = 0; i < kNumHooks; i++) {
        if (g_hooks[i].calls != last[i]) {
            Log("trace: %-24s %u calls (+%u)", g_hooks[i].name, g_hooks[i].calls, g_hooks[i].calls - last[i]);
            last[i] = g_hooks[i].calls;
        }
    }
}
