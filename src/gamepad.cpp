// Native Xbox controller support.
//
// The Jade engine is an Xbox engine: every logical input action is an Xbox pad
// control. On PC, InputManagerPC::GetActionValue (0x41d7b0, thiscall float(int
// action)) returns the highest value of up to three keyboard/mouse/DirectInput
// bindings of an action; everything else - the digital pad bits the menus poll,
// movement and camera sticks - is built from it. We wrap that function and add
// an XInput controller, mapped exactly like the Xbox build maps its pad
// (Xbox 0xdc160):
//
//   action 0 A, 1 B, 2 X, 3 Y, 4 Black (RB), 5 White (LB), 6 LT, 7 RT,
//   9 Start, 10 right stick click, 11 left stick click,
//   12 D-pad up, 13 right, 14 down, 15 left.
// Action 8 (Back/View) is left unmapped: the PC port bound it to "quit game".
//
// The sticks go through the engine's stick query (0x41fd20, cdecl
// void(int pad, float* out, int stick)) instead of the per-action values: the
// PC version builds a stick from four digital actions, clamps each axis on its
// own and scales it for keyboard use, which makes an analog stick stutter on
// diagonals. We fill the vector straight from the pad with a round dead zone.
//
// This works everywhere (console menus, gameplay, skipping videos) and does not
// depend on the PC control settings or on GOG's DirectInput wrapper.

#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <xinput.h>
#include <string.h>
#include <math.h>
#include "gamepad.h"

void Log(const char* fmt, ...);

namespace {

const DWORD kGetActionValue = 0x0041d7b0;
const unsigned char kGetActionValuePrologue[] = { 0x51, 0x56, 0x8B, 0xF1, 0x8A, 0x46, 0x08 };  // 3 whole instructions

typedef float(__thiscall* GetActionValue_t)(void* input, unsigned action);
GetActionValue_t g_original;  // trampoline

typedef DWORD(WINAPI* XInputGetState_t)(DWORD, XINPUT_STATE*);
XInputGetState_t g_xinputGetState;

// Controller state, refreshed at most once per millisecond tick.
XINPUT_GAMEPAD g_pad;
bool g_padConnected;
DWORD g_padTick;

void RefreshPad()
{
    DWORD now = GetTickCount();
    if (now == g_padTick) return;
    g_padTick = now;
    g_padConnected = false;
    if (!g_xinputGetState) return;
    for (DWORD i = 0; i < XUSER_MAX_COUNT; i++) {
        XINPUT_STATE st;
        if (g_xinputGetState(i, &st) != ERROR_SUCCESS) continue;
        g_pad = st.Gamepad;
        g_padConnected = true;
        static int loggedSlot = -1;
        if (loggedSlot != (int)i) { loggedSlot = (int)i; Log("gamepad: XInput controller in slot %lu", i); }
        return;
    }
}

float Button(WORD mask) { return (g_pad.wButtons & mask) ? 1.0f : 0.0f; }
float Trigger(BYTE value) { return value > XINPUT_GAMEPAD_TRIGGER_THRESHOLD ? 1.0f : 0.0f; }

// Stick with a round dead zone, rescaled so that the output starts at 0 at the
// dead zone edge and reaches 1 at full deflection. Returns false inside it.
bool Stick(SHORT rawX, SHORT rawY, SHORT deadZone, float& x, float& y)
{
    float fx = rawX, fy = rawY;
    float m = sqrtf(fx * fx + fy * fy);
    if (m <= deadZone) return false;
    float scaled = (m - deadZone) / (32767.0f - deadZone);
    if (scaled > 1.0f) scaled = 1.0f;
    x = fx / m * scaled;
    y = fy / m * scaled;
    return true;
}

float PadValue(unsigned action)
{
    switch (action) {
    case 0: return Button(XINPUT_GAMEPAD_A);
    case 1: return Button(XINPUT_GAMEPAD_B);
    case 2: return Button(XINPUT_GAMEPAD_X);
    case 3: return Button(XINPUT_GAMEPAD_Y);
    case 4: return Button(XINPUT_GAMEPAD_RIGHT_SHOULDER);  // Black
    case 5: return Button(XINPUT_GAMEPAD_LEFT_SHOULDER);   // White
    case 6: return Trigger(g_pad.bLeftTrigger);
    case 7: return Trigger(g_pad.bRightTrigger);
    case 9: return Button(XINPUT_GAMEPAD_START);
    case 10: return Button(XINPUT_GAMEPAD_RIGHT_THUMB);
    case 11: return Button(XINPUT_GAMEPAD_LEFT_THUMB);
    case 12: return Button(XINPUT_GAMEPAD_DPAD_UP);
    case 13: return Button(XINPUT_GAMEPAD_DPAD_RIGHT);
    case 14: return Button(XINPUT_GAMEPAD_DPAD_DOWN);
    case 15: return Button(XINPUT_GAMEPAD_DPAD_LEFT);
    }
    return 0.0f;
}

float __fastcall GetActionValueHook(void* input, void* /*edx*/, unsigned action)
{
    float v = g_original(input, action);
    if (!*((char*)input + 8)) return v;  // input disabled (e.g. window inactive)
    RefreshPad();
    if (!g_padConnected) return v;
    float p = PadValue(action & 0xff);
    return p > v ? p : v;
}

// ---------------------------------------------------------------- sticks
const DWORD kGetStick = 0x0041fd20;
const unsigned char kGetStickPrologue[] = { 0x8B, 0x44, 0x24, 0x04, 0x33, 0xD2 };  // 2 whole instructions
const DWORD* const g_stickEnableMask = (const DWORD*)0x007f1578;

typedef void(__cdecl* GetStick_t)(int pad, float* out, int stick);
GetStick_t g_originalGetStick;

void __cdecl GetStickHook(int pad, float* out, int stick)
{
    g_originalGetStick(pad, out, stick);
    if (pad != 0 || !(*g_stickEnableMask & (1u << (stick + 24)))) return;  // stick disabled by the game
    RefreshPad();
    if (!g_padConnected) return;
    float x, y;
    if (stick == 0) {
        // Movement: the stick replaces the keyboard direction while it is deflected.
        if (Stick(g_pad.sThumbLX, g_pad.sThumbLY, XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE, x, y)) {
            out[0] = x;
            out[1] = y;
        }
    } else {
        // Camera: added on top of mouse and keys.
        if (Stick(g_pad.sThumbRX, g_pad.sThumbRY, XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE, x, y)) {
            out[0] += x;
            out[1] += y;
        }
    }
}

// Inline detour: relocate `len` whole prologue bytes into a trampoline.
void* Detour(DWORD addr, const unsigned char* prologue, size_t len, void* hook)
{
    unsigned char* fn = (unsigned char*)addr;
    if (memcmp(fn, prologue, len) != 0) {
        Log("gamepad: unexpected code at %08lx, not hooked", addr);
        return nullptr;
    }
    unsigned char* tramp = (unsigned char*)VirtualAlloc(nullptr, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!tramp) return nullptr;
    memcpy(tramp, fn, len);
    tramp[len] = 0xE9;
    *(DWORD*)(tramp + len + 1) = (DWORD)(fn + len) - (DWORD)(tramp + len + 5);

    DWORD prot;
    VirtualProtect(fn, len, PAGE_EXECUTE_READWRITE, &prot);
    fn[0] = 0xE9;
    *(DWORD*)(fn + 1) = (DWORD)hook - (DWORD)(fn + 5);
    for (size_t i = 5; i < len; i++) fn[i] = 0x90;
    VirtualProtect(fn, len, prot, &prot);
    FlushInstructionCache(GetCurrentProcess(), fn, len);
    return tramp;
}

}  // namespace

void Gamepad_Install()
{
    static bool done;
    if (done) return;
    done = true;

    const char* dlls[] = { "xinput1_4.dll", "xinput1_3.dll", "xinput9_1_0.dll" };
    for (const char* name : dlls) {
        if (HMODULE h = LoadLibraryA(name)) {
            g_xinputGetState = (XInputGetState_t)GetProcAddress(h, "XInputGetState");
            if (g_xinputGetState) break;
        }
    }
    if (!g_xinputGetState) { Log("gamepad: no XInput DLL, not installed"); return; }

    g_original = (GetActionValue_t)Detour(kGetActionValue, kGetActionValuePrologue,
                                          sizeof(kGetActionValuePrologue), (void*)GetActionValueHook);
    g_originalGetStick = (GetStick_t)Detour(kGetStick, kGetStickPrologue, sizeof(kGetStickPrologue),
                                            (void*)GetStickHook);
    if (!g_original || !g_originalGetStick) return;
    Log("gamepad: installed (Xbox controller mapping on all game actions)");
}
