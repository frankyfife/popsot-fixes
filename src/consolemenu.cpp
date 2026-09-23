// Console (Xbox) front-end menus.
//
// The PC build still contains the console menus: they are Jade AI scripts
// (compiled to C) that build fading text pages, driven by the engine's menu
// manager. The PC port disabled them in two places:
//
// 1. The engine's "open menu" routine (0x467b70) activates the console menu
//    and then unconditionally calls a PC hook (0x402760) that pushes one of the
//    mouse-only PCMenuManager pages on top of it. We turn that hook into a no-op.
//
// 2. The per-frame menu manager tick was cut down. On Xbox (0x17d10) it
//      - lets every menu control (input action, manager +0x14..+0x18) poll its
//        input; the first one that fires consumes the frame,
//      - closes the top page once it has finished fading out (state 4),
//      - ticks the top page and the menu animations.
//    The PC version (0x672310) only kept the last two steps, so the console
//    menus never see any input. We replace it with the Xbox logic, built from
//    the functions that are still present in the PC executable.

#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <string.h>
#include <xinput.h>
#include "consolemenu.h"

void Log(const char* fmt, ...);

namespace {

const DWORD kPcMenuRedirect = 0x00402760;  // cdecl void(int consoleMenuIndex)
const unsigned char kPcMenuRedirectPrologue[] = { 0x8B, 0x0D, 0x44, 0xBC, 0x80, 0x00, 0x56 };

// The per-frame pad state builder (0x41fa10) clears all pad input while a PC
// front-end flag (*(0x80bc44) + 0x1a) is set, so the console menu controls
// never see a button press. Make that branch unconditional ("je" -> "jmp").
const DWORD kPadClearBranch = 0x0041fcdb;
const unsigned char kPadClearBranchBytes[] = { 0x74, 0x0D };

const DWORD kMenuTick = 0x00672310;  // thiscall bool(MenuManager*)
const unsigned char kMenuTickPrologue[] = { 0x56, 0x8B, 0xF1, 0x8B, 0x86, 0xF0, 0x00, 0x00, 0x00 };

typedef void(__thiscall* PageFn_t)(void* page);
typedef int(__thiscall* ControlProcess_t)(void* control);
typedef void(__cdecl* AnimTick_t)();
const PageFn_t ClosePage = (PageFn_t)0x00697cc0;  // state 4 -> 5 (fade-out finished)
const PageFn_t PageTick = (PageFn_t)0x006979f0;
const AnimTick_t AnimTick = (AnimTick_t)0x006d26c0;

// Menu manager layout (shared by Xbox and PC)
const int kControlsBegin = 0x14, kControlsEnd = 0x18;
const int kOpenPages = 0xf0, kPageStack = 0x6c;  // stack entries 1..open
const int kPageState = 0x84;

bool g_loggedFirstInput;

// ---------------------------------------------------------------- element visuals
// Every menu element owns world objects and texts that are shown/hidden through
// 0x69a840 (thiscall, one argument: show). On Xbox (0x38e20) it stores the new
// state and applies it. The PC port changed the prologue so the argument is
// never read: the function returns unless the element is currently visible and
// then always hides it - the console menus can never become visible. The body
// still contains the full show/hide logic; this is it with the Xbox prologue.
const DWORD kElemSetVisible = 0x0069a840;
const unsigned char kElemSetVisiblePrologue[] = { 0x56, 0x8B, 0xF1, 0x8B, 0x46, 0x34, 0x85, 0xC0, 0x0F, 0x84 };

typedef int(__cdecl* FindObject_t)(DWORD key);
typedef void(__thiscall* ObjectShow_t)(void* obj, void* elem, int show);
typedef void(__cdecl* ObjectHide_t)(int object, DWORD a, DWORD flags);
typedef void(__cdecl* TextPrepare_t)(DWORD id, void* elem);
typedef void(__cdecl* TextSetVisible_t)(int table, DWORD id, int zero, int visible);
const FindObject_t FindObject = (FindObject_t)0x0043c760;
const ObjectShow_t ObjectShow = (ObjectShow_t)0x0069a010;
const ObjectHide_t ObjectHide = (ObjectHide_t)0x0043c7e0;
const TextPrepare_t TextPrepare = (TextPrepare_t)0x00699dd0;
const TextSetVisible_t TextSetVisible = (TextSetVisible_t)0x004363e0;
char** const g_textTables = (char**)0x00af5650;

void __fastcall ElementSetVisible(char* elem, void* /*edx*/, int show)
{
    int& visible = *(int*)(elem + 0x34);
    if ((show != 0) == (visible != 0)) return;
    visible = show ? 1 : 0;

    for (DWORD** it = *(DWORD***)(elem + 0xc); it != *(DWORD***)(elem + 0x10); it++) {
        DWORD* obj = *it;
        int object = FindObject(obj[1]);
        if (!object) continue;
        if (visible) ObjectShow(obj, elem, 1);
        else ObjectHide(object, obj[2], obj[4] | 0x400);
    }
    for (WORD* id = *(WORD**)(elem + 0x1c); id != *(WORD**)(elem + 0x20); id++) {
        char* tables = *g_textTables;
        if (!tables) continue;
        int table = *(int*)(tables + (*id < 0x400 ? 0x464 : *id < 0x800 ? 0x460 : 0x468));
        if (!table) continue;
        if (visible) TextPrepare(*id, elem);
        TextSetVisible(table, *id, 0, visible);
    }
}

// ---------------------------------------------------------------- pad bits
// The engine keeps the Xbox-style digital pad state as a bit mask of logical
// actions (0x80d074 current, 0x80d070 previous frame; built by 0x41fa10). The
// console menus poll A/B/Y/Start and the D-pad (bits 12-15); the PC port never
// binds the D-pad bits to anything. While a console menu is open we OR the
// XInput pad into that mask.
DWORD* const g_padBits = (DWORD*)0x0080d074;

enum : DWORD {
    PAD_A = 1u << 0, PAD_B = 1u << 1, PAD_X = 1u << 2, PAD_Y = 1u << 3,
    PAD_START = 1u << 9, PAD_BACK = 1u << 10,
    // D-pad bits as the menus use them (verified in-game: 13 = up, 14 = down).
    PAD_LEFT = 1u << 12, PAD_UP = 1u << 13, PAD_DOWN = 1u << 14, PAD_RIGHT = 1u << 15,
};

typedef DWORD(WINAPI* XInputGetState_t)(DWORD, XINPUT_STATE*);
XInputGetState_t g_xinputGetState;

DWORD ReadXInputAsPadBits()
{
    static bool loaded;
    if (!loaded) {
        loaded = true;
        HMODULE h = LoadLibraryA("xinput1_4.dll");
        if (!h) h = LoadLibraryA("xinput1_3.dll");
        if (h) g_xinputGetState = (XInputGetState_t)GetProcAddress(h, "XInputGetState");
    }
    if (!g_xinputGetState) {
        static bool loggedMissing;
        if (!loggedMissing) { loggedMissing = true; Log("console menu: XInput not available"); }
        return 0;
    }
    for (DWORD i = 0; i < XUSER_MAX_COUNT; i++) {
        XINPUT_STATE st;
        if (g_xinputGetState(i, &st) != ERROR_SUCCESS) continue;
        const XINPUT_GAMEPAD& p = st.Gamepad;
        static int loggedPad = -1;
        static WORD lastButtons = 0xffff;
        if (loggedPad != (int)i) { loggedPad = (int)i; Log("console menu: XInput controller found in slot %lu", i); }
        if (p.wButtons != lastButtons) { lastButtons = p.wButtons; Log("console menu: XInput buttons %04x", p.wButtons); }
        DWORD bits = 0;
        if (p.wButtons & XINPUT_GAMEPAD_A) bits |= PAD_A;
        if (p.wButtons & XINPUT_GAMEPAD_B) bits |= PAD_B;
        if (p.wButtons & XINPUT_GAMEPAD_X) bits |= PAD_X;
        if (p.wButtons & XINPUT_GAMEPAD_Y) bits |= PAD_Y;
        if (p.wButtons & XINPUT_GAMEPAD_START) bits |= PAD_START;
        if (p.wButtons & XINPUT_GAMEPAD_BACK) bits |= PAD_BACK;
        const int dz = 16000;
        if ((p.wButtons & XINPUT_GAMEPAD_DPAD_UP) || p.sThumbLY > dz) bits |= PAD_UP;
        if ((p.wButtons & XINPUT_GAMEPAD_DPAD_DOWN) || p.sThumbLY < -dz) bits |= PAD_DOWN;
        if ((p.wButtons & XINPUT_GAMEPAD_DPAD_LEFT) || p.sThumbLX < -dz) bits |= PAD_LEFT;
        if ((p.wButtons & XINPUT_GAMEPAD_DPAD_RIGHT) || p.sThumbLX > dz) bits |= PAD_RIGHT;
        return bits;
    }
    return 0;
}

// Xbox menu manager tick, see file comment.
bool __fastcall XboxMenuTick(char* mgr, void* /*edx*/)
{
    DWORD** it = *(DWORD***)(mgr + kControlsBegin);
    DWORD** end = *(DWORD***)(mgr + kControlsEnd);
    int consumed = 0;

    // Diagnostics: which pad bit each control polls, and pad bit changes.
    static DWORD** loggedControls;
    if (it != loggedControls && it && end > it) {
        loggedControls = it;
        for (DWORD** c = it; c < end; c++)
            if (*c) Log("console menu: control %p vt %08lx polls pad %lu bit %lu", *c, (*c)[0], (*c)[4], (*c)[5]);
    }
    if (*(int*)(mgr + kOpenPages) > 0) *g_padBits |= ReadXInputAsPadBits();

    static DWORD lastPad;
    DWORD pad = *g_padBits;
    if (pad != lastPad) { lastPad = pad; Log("console menu: pad bits %08lx", pad); }
    for (; it && it < end; it++) {
        DWORD* control = *it;
        if (!control) continue;
        if (!consumed) {
            ControlProcess_t process = (ControlProcess_t)(*(DWORD**)control)[1];
            consumed = process(control);
            if (consumed && !g_loggedFirstInput) {
                g_loggedFirstInput = true;
                Log("console menu: first input consumed by control %p", control);
            }
        } else {
            control[2] = control[3] = control[4] = 0;
        }
    }

    int open = *(int*)(mgr + kOpenPages);
    if (open) {
        void* top = *(void**)(mgr + kPageStack + open * 4);
        if (*(int*)((char*)top + kPageState) == 4) ClosePage(top);
        PageTick(top);
    }
    AnimTick();
    return *(int*)(mgr + kOpenPages) != 0;
}

bool Patch(DWORD addr, const unsigned char* expect, size_t len, const unsigned char* bytes, size_t n)
{
    unsigned char* fn = (unsigned char*)addr;
    if (memcmp(fn, expect, len) != 0) {
        Log("console menu: unexpected code at %08lx, not patched", addr);
        return false;
    }
    DWORD prot;
    VirtualProtect(fn, n, PAGE_EXECUTE_READWRITE, &prot);
    memcpy(fn, bytes, n);
    VirtualProtect(fn, n, prot, &prot);
    FlushInstructionCache(GetCurrentProcess(), fn, n);
    return true;
}

}  // namespace

void ConsoleMenu_Enable()
{
    static bool done;
    if (done) return;
    done = true;

    // Check all sites first so we never leave the game half-patched.
    if (memcmp((void*)kPcMenuRedirect, kPcMenuRedirectPrologue, sizeof(kPcMenuRedirectPrologue)) != 0 ||
        memcmp((void*)kMenuTick, kMenuTickPrologue, sizeof(kMenuTickPrologue)) != 0 ||
        memcmp((void*)kPadClearBranch, kPadClearBranchBytes, sizeof(kPadClearBranchBytes)) != 0 ||
        memcmp((void*)kElemSetVisible, kElemSetVisiblePrologue, sizeof(kElemSetVisiblePrologue)) != 0) {
        Log("console menu: unknown executable, not enabled");
        return;
    }

    unsigned char jmp[5] = { 0xE9 };
    *(DWORD*)(jmp + 1) = (DWORD)XboxMenuTick - (kMenuTick + 5);
    Patch(kMenuTick, kMenuTickPrologue, sizeof(kMenuTickPrologue), jmp, sizeof(jmp));

    const unsigned char ret = 0xC3;
    Patch(kPcMenuRedirect, kPcMenuRedirectPrologue, sizeof(kPcMenuRedirectPrologue), &ret, 1);

    *(DWORD*)(jmp + 1) = (DWORD)ElementSetVisible - (kElemSetVisible + 5);
    Patch(kElemSetVisible, kElemSetVisiblePrologue, sizeof(kElemSetVisiblePrologue), jmp, sizeof(jmp));

    const unsigned char jmpShort = 0xEB;
    Patch(kPadClearBranch, kPadClearBranchBytes, sizeof(kPadClearBranchBytes), &jmpShort, 1);

    Log("console menu: enabled (PC redirect removed, Xbox menu tick restored, pad input kept, element show restored)");
}
