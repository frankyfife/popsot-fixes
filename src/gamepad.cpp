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
//   8 Back, 9 Start, 10 right stick click, 11 left stick click,
//   12 D-pad up, 13 right, 14 down, 15 left,
//   0x25/0x26 move left/right, 0x27/0x28 move forward/back  (left stick)
//   0x29/0x2a, 0x2b/0x2c camera                             (right stick)
//
// This works everywhere (console menus, gameplay, skipping videos) and does not
// depend on the PC control settings or on GOG's DirectInput wrapper.

#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <xinput.h>
#include <string.h>
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

// One direction of a stick axis, 0..1 with a radial-ish dead zone.
float StickDir(SHORT value, int sign, SHORT deadZone)
{
    int v = sign * (int)value;
    if (v <= deadZone) return 0.0f;
    float f = (float)(v - deadZone) / (32767.0f - deadZone);
    return f > 1.0f ? 1.0f : f;
}

float PadValue(unsigned action)
{
    const SHORT dzL = XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE, dzR = XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE;
    switch (action) {
    case 0: return Button(XINPUT_GAMEPAD_A);
    case 1: return Button(XINPUT_GAMEPAD_B);
    case 2: return Button(XINPUT_GAMEPAD_X);
    case 3: return Button(XINPUT_GAMEPAD_Y);
    case 4: return Button(XINPUT_GAMEPAD_RIGHT_SHOULDER);  // Black
    case 5: return Button(XINPUT_GAMEPAD_LEFT_SHOULDER);   // White
    case 6: return Trigger(g_pad.bLeftTrigger);
    case 7: return Trigger(g_pad.bRightTrigger);
    case 8: return Button(XINPUT_GAMEPAD_BACK);
    case 9: return Button(XINPUT_GAMEPAD_START);
    case 10: return Button(XINPUT_GAMEPAD_RIGHT_THUMB);
    case 11: return Button(XINPUT_GAMEPAD_LEFT_THUMB);
    case 12: return Button(XINPUT_GAMEPAD_DPAD_UP);
    case 13: return Button(XINPUT_GAMEPAD_DPAD_RIGHT);
    case 14: return Button(XINPUT_GAMEPAD_DPAD_DOWN);
    case 15: return Button(XINPUT_GAMEPAD_DPAD_LEFT);
    case 0x25: return StickDir(g_pad.sThumbLX, -1, dzL);
    case 0x26: return StickDir(g_pad.sThumbLX, +1, dzL);
    case 0x27: return StickDir(g_pad.sThumbLY, +1, dzL);
    case 0x28: return StickDir(g_pad.sThumbLY, -1, dzL);
    case 0x29: return StickDir(g_pad.sThumbRX, +1, dzR);
    case 0x2a: return StickDir(g_pad.sThumbRX, -1, dzR);
    case 0x2b: return StickDir(g_pad.sThumbRY, +1, dzR);
    case 0x2c: return StickDir(g_pad.sThumbRY, -1, dzR);
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

}  // namespace

void Gamepad_Install()
{
    static bool done;
    if (done) return;
    done = true;

    unsigned char* fn = (unsigned char*)kGetActionValue;
    if (memcmp(fn, kGetActionValuePrologue, sizeof(kGetActionValuePrologue)) != 0) {
        Log("gamepad: unexpected code at %08lx, not installed", kGetActionValue);
        return;
    }
    const char* dlls[] = { "xinput1_4.dll", "xinput1_3.dll", "xinput9_1_0.dll" };
    for (const char* name : dlls) {
        if (HMODULE h = LoadLibraryA(name)) {
            g_xinputGetState = (XInputGetState_t)GetProcAddress(h, "XInputGetState");
            if (g_xinputGetState) break;
        }
    }
    if (!g_xinputGetState) { Log("gamepad: no XInput DLL, not installed"); return; }

    // Trampoline: the relocated prologue, then a jump back behind it.
    const size_t n = sizeof(kGetActionValuePrologue);
    unsigned char* tramp = (unsigned char*)VirtualAlloc(nullptr, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!tramp) return;
    memcpy(tramp, fn, n);
    tramp[n] = 0xE9;
    *(DWORD*)(tramp + n + 1) = (DWORD)(fn + n) - (DWORD)(tramp + n + 5);
    g_original = (GetActionValue_t)tramp;

    DWORD prot;
    VirtualProtect(fn, n, PAGE_EXECUTE_READWRITE, &prot);
    fn[0] = 0xE9;
    *(DWORD*)(fn + 1) = (DWORD)GetActionValueHook - (DWORD)(fn + 5);
    for (size_t i = 5; i < n; i++) fn[i] = 0x90;
    VirtualProtect(fn, n, prot, &prot);
    FlushInstructionCache(GetCurrentProcess(), fn, n);
    Log("gamepad: installed (Xbox controller mapping on all game actions)");
}
