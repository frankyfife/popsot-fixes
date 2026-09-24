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
//   action 0 A, 1 B, 2 X, 3 Y, 9 Start, 10 right stick click,
//   11 left stick click, 12 D-pad up, 13 right, 14 down, 15 left.
// Shoulders and triggers follow the PS2 layout instead (see PadValue):
//   LB rewind (Xbox L trigger), RB special action (Xbox R trigger),
//   LT alternate view (Xbox White), RT look (Xbox Black).
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
DWORD g_padSlot;  // XInput slot of g_pad
DWORD g_padTick;

void UpdateMotors();  // vibration, below

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
        g_padSlot = i;
        static int loggedSlot = -1;
        if (loggedSlot != (int)i) { loggedSlot = (int)i; Log("gamepad: XInput controller in slot %lu", i); }
        UpdateMotors();
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
    // The original Xbox pad had no shoulder buttons. The PS2 layout of the same
    // game (L1 rewind, R1 special action, L2 alternate view, R2 look) maps better
    // onto a modern pad, so shoulders and triggers follow it:
    case 4: return Trigger(g_pad.bRightTrigger);           // Black: look (PS2 R2)
    case 5: return Trigger(g_pad.bLeftTrigger);            // White: alternate view (PS2 L2)
    case 6: return Button(XINPUT_GAMEPAD_LEFT_SHOULDER);   // L trigger: rewind (PS2 L1)
    case 7: return Button(XINPUT_GAMEPAD_RIGHT_SHOULDER);  // R trigger: special action (PS2 R1)
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

// ---------------------------------------------------------------- vibration
// The PC port removed force feedback: the script code that rumbles the pad still
// evaluates its arguments, but calls an empty function (0x563530, shared by ~1000
// stripped call sites) instead of the motor driver. We redirect only those calls:
//   small motor: cdecl (pad, frames)
//   large motor: cdecl (pad, strength 0..255, frames)
// Like on Xbox (motors driven in 0xdc6a0 / 0xdc700, counted down in 0xdc160) a motor
// runs for the given number of game frames; pad must be 0. The menu option
// "Vibration" still works on PC and stores its state at 0x7f157c.
//
// Call sites, found by matching the Xbox callers of 0xdc6a0 / 0xdc700 to the PC
// through the AI function table ids:
//   0x558f10 (Xbox 0x18e510)  the Prince's per-frame rumble: script variables +0x30
//                             (small) and +0x74 (large strength) collected during the
//                             frame, applied for 2 frames - hits, landings, etc.
//   0x5e7fb0 (Xbox 0x20c730)  rumble generator objects (quakes, machinery), with
//   0x578420 (Xbox 0x12e000)  their start and stop helpers
//   0x5789a0 (Xbox 0x12e5d0)
//   0x498800 / 0x498890       generic script functions (ids 0x1b63 / 0x1b64)
// Stop calls (frames 0, e.g. entering cutscenes) are not redirected: every rumble
// here ends by itself after a few frames.
const DWORD kEmptyFunction = 0x00563530;
const DWORD kSmallMotorCalls[] = { 0x00558f84, 0x00578489, 0x00578a6a, 0x00498867, 0x0049887f };
const DWORD kLargeMotorCalls[] = { 0x00558fd3, 0x005e8193, 0x00578473, 0x00578a14, 0x0049892e };
const int* const g_vibrationEnabled = (const int*)0x007f157c;

typedef DWORD(WINAPI* XInputSetState_t)(DWORD, XINPUT_VIBRATION*);
XInputSetState_t g_xinputSetState;
// A motor runs until its deadline. Game frames are converted to time (the Xbox
// counts them down per 30 Hz game frame), so the motors do not depend on the
// render loop and always stop on time.
const DWORD kMsPerFrame = 33;
DWORD g_smallUntil, g_largeUntil;
WORD g_largeSpeed;
WORD g_lastLeft, g_lastRight;

int g_loggedMotorCalls;

void UpdateMotors();

void __cdecl SmallMotor(int pad, int frames)
{
    if (g_loggedMotorCalls < 20) { g_loggedMotorCalls++; Log("vibration: small motor pad %d frames %d", pad, frames); }
    if (pad != 0) return;
    g_smallUntil = GetTickCount() + (frames > 0 ? frames * kMsPerFrame : 0);
    UpdateMotors();
}

void __cdecl LargeMotor(int pad, int strength, int frames)
{
    if (g_loggedMotorCalls < 20) {
        g_loggedMotorCalls++;
        Log("vibration: large motor pad %d strength %d frames %d", pad, strength, frames);
    }
    if (pad != 0) return;
    g_largeUntil = GetTickCount() + (frames > 0 ? frames * kMsPerFrame : 0);
    g_largeSpeed = strength > 255 ? 65535 : strength <= 0 ? 0 : (WORD)(strength * 65535 / 255);
    UpdateMotors();
}

void SetMotors(WORD left, WORD right)
{
    if (!g_xinputSetState || (left == g_lastLeft && right == g_lastRight)) return;
    XINPUT_VIBRATION v = { left, right };
    DWORD r = g_xinputSetState(g_padSlot, &v);
    if (r == ERROR_SUCCESS) {
        g_lastLeft = left;
        g_lastRight = right;
    }
    static int logged;
    if (logged < 6) { logged++; Log("vibration: motors %u/%u on slot %lu -> %lu", left, right, g_padSlot, r); }
}

bool GameInForeground()
{
    DWORD pid = 0;
    HWND fg = GetForegroundWindow();
    if (fg) GetWindowThreadProcessId(fg, &pid);
    return pid == GetCurrentProcessId();
}

// Called whenever the pad is read (several times per game frame) and on motor calls.
void UpdateMotors()
{
    if (!g_xinputSetState) return;
    DWORD now = GetTickCount();
    bool smallOn = (int)(g_smallUntil - now) > 0, largeOn = (int)(g_largeUntil - now) > 0;
    bool active = (smallOn || largeOn) && *g_vibrationEnabled && g_padConnected && GameInForeground();
    static bool loggedBlocked;
    if ((smallOn || largeOn) && !active && !loggedBlocked) {
        loggedBlocked = true;
        Log("vibration: blocked (option %d, pad %d, foreground %d)", *g_vibrationEnabled, g_padConnected,
            GameInForeground());
    }
    SetMotors(active && largeOn ? g_largeSpeed : 0, active && smallOn ? 0xFFFF : 0);
}

bool IsEmptyCall(DWORD site)
{
    const unsigned char* p = (const unsigned char*)site;
    if (p[0] == 0xE8 && site + 5 + *(const int*)(p + 1) == kEmptyFunction) return true;
    Log("gamepad: unexpected call at %08lx, vibration not installed", site);
    return false;
}

void RedirectCall(DWORD site, void* target)
{
    unsigned char* p = (unsigned char*)site;
    DWORD prot;
    VirtualProtect(p, 5, PAGE_EXECUTE_READWRITE, &prot);
    *(DWORD*)(p + 1) = (DWORD)target - (site + 5);
    VirtualProtect(p, 5, prot, &prot);
    FlushInstructionCache(GetCurrentProcess(), p, 5);
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
            g_xinputSetState = (XInputSetState_t)GetProcAddress(h, "XInputSetState");
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

    // Check every site first so vibration is either fully installed or not at all.
    bool vibration = g_xinputSetState != nullptr;
    for (DWORD site : kSmallMotorCalls) vibration = vibration && IsEmptyCall(site);
    for (DWORD site : kLargeMotorCalls) vibration = vibration && IsEmptyCall(site);
    if (!vibration) return;
    for (DWORD site : kSmallMotorCalls) RedirectCall(site, (void*)SmallMotor);
    for (DWORD site : kLargeMotorCalls) RedirectCall(site, (void*)LargeMotor);
    Log("gamepad: vibration installed");
}

void Gamepad_OnFrame(HWND /*gameWindow*/)
{
    UpdateMotors();
}

void Gamepad_Shutdown()
{
    g_smallUntil = g_largeUntil = GetTickCount();
    SetMotors(0, 0);
}
