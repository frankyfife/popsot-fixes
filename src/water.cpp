// Water wave simulation at the Xbox rate.
//
// The Jade water is a CPU height field. Its driver (PC 0x6cfcb0, Xbox
// 0x525a0, cdecl (water)) runs once per frame for every water object: it
// turns the frame time (us, game speed applied; PC qword 0x80cc48, Xbox
// 0x7654e0) into seconds (x 1e-6, capped at 1/30 s), steps every field
// (PC 0x6cf280, Xbox 0x51c00) and exchanges the field edges. The step is the
// same code on both platforms, but parts of it count per call, not per second:
//
// * the velocity damping (0.97, set by the loader PC 0x6d17d0 / Xbox 0x53a90)
//   and the distance fade are multiplied in once per step;
// * impulses (character movement from the perturber PC 0x6dd1a0 / Xbox
//   0x61f20, rain drops) go in as a force times dt, and the perturber's
//   strength is the distance moved since the last frame, so it shrinks with
//   the frame time too.
//
// The Xbox game logic runs at a fixed 30 Hz (frame clock Xbox 0xdb740: step
// 1,000,000 / (refresh / 2) us), so the console steps its water 30 times a
// second with dt = 1/30 s. The PC version steps once per rendered frame: at
// 60 fps the waves lose 84 % of their speed per second instead of 60 %, at 144
// fps 99 %, and every push is a fraction as strong. The surface stays nearly
// flat, and with it the refraction, which follows the normals.
//
// Fix: the driver is detoured. Every water object adds up its frame times and
// is stepped once for every 1/30 s of game time, with the frame time set to
// 33333 us for that call (the same as rePOP_ff does for the Xbox recompilation). On
// other frames only the driver's done flag (+0x8c) is set, as the driver
// itself would. Impulses that arrive in between stay in the field's list
// (up to 20) until the next step. [water] step_rate in popfix.ini (0 = once per
// frame, the original), Shift+F10 switches it in the game (one buzz = Xbox
// rate, two = original).

#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <string.h>
#include "water.h"
#include "gamepad.h"

void Log(const char* fmt, ...);

namespace {

const DWORD kWaterDriver = 0x006cfcb0;
const unsigned char kWaterDriverPrologue[] = { 0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF8 };  // push ebp; mov ebp,esp; and esp,-8
LONGLONG* const g_frameUs = (LONGLONG*)0x0080cc48;  // frame time in us, game speed applied

const int kOffDone = 0x8c;     // set by the driver, cleared every frame for all water objects (0x6cee30)
const int kOffBlocked = 0x90;  // the driver does nothing while this is set
const int kMaxSteps = 4;       // per frame, after a long frame (loading)

typedef void(__cdecl* WaterDriver_t)(BYTE* water);
WaterDriver_t o_waterDriver;

int g_stepHz = 30;  // [water] step_rate; 0 = once per frame
bool g_original;    // Shift+F10, or step_rate=0

struct Clock {
    BYTE* water;
    LONGLONG acc;  // game time not yet stepped (us)
    LONGLONG diagUs;
    int calls, steps;
};
const int kMaxClocks = 32;
Clock g_clocks[kMaxClocks];
int g_nextClock;

Clock& ClockFor(BYTE* water, LONGLONG stepUs)
{
    for (Clock& c : g_clocks)
        if (c.water == water) return c;
    Clock& c = g_clocks[g_nextClock];
    g_nextClock = (g_nextClock + 1) % kMaxClocks;
    c.water = water;
    c.acc = stepUs;  // a new water object is stepped at once
    c.diagUs = c.calls = c.steps = 0;
    return c;
}

// [debug] verbose: every 5 s of game time per water object, in popfix.log.
// Water object: +0x1c drop height, +0x20 drops per tick, +0x34 patches (+0x38
// columns, +0x3c rows). Patch: +0x124 impulses waiting, +0x128 peak amplitude,
// +0x13c flags (1 culled, 2 at rest).
bool g_diag;

void Diagnose(Clock& c, BYTE* w, int steps)
{
    if (!g_diag) return;
    c.calls++;
    c.steps += steps;
    if (*g_frameUs > 0) c.diagUs += *g_frameUs;
    if (c.diagUs < 5000000) return;
    __try {
        BYTE** patches = *(BYTE***)(w + 0x34);
        int cols = *(int*)(w + 0x38), rows = *(int*)(w + 0x3c);
        int n = 0, culled = 0, rest = 0, waiting = 0;
        float peak = 0;
        for (int i = 0; patches && i < cols * rows && i < 4096; i++) {
            BYTE* p = patches[i];
            if (!p) continue;
            n++;
            DWORD fl = *(DWORD*)(p + 0x13c);
            culled += (fl & 1) != 0;
            rest += (fl & 2) != 0;
            waiting += *(signed char*)(p + 0x124);
            float a = *(float*)(p + 0x128);
            if (a > peak) peak = a;
        }
        Log("water %p: %d patches (%d culled, %d at rest), peak %.5f, %d impulses waiting; drops %d x %.3f;"
            " %d steps in %d calls",
            w, n, culled, rest, peak, waiting, *(int*)(w + 0x20), *(float*)(w + 0x1c), c.steps, c.calls);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("water %p: diagnostics failed", w);
    }
    c.diagUs = c.calls = c.steps = 0;
}

void* Detour(DWORD addr, const unsigned char* prologue, size_t len, void* hook)
{
    unsigned char* fn = (unsigned char*)addr;
    if (memcmp(fn, prologue, len) != 0) return nullptr;
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

void __cdecl WaterDriverHook(BYTE* water)
{
    if (!water || *(int*)(water + kOffDone) || *(int*)(water + kOffBlocked)) {
        o_waterDriver(water);
        return;
    }
    const LONGLONG stepUs = 1000000 / g_stepHz;
    Clock& c = ClockFor(water, stepUs);
    if (g_original) {
        Diagnose(c, water, 1);
        o_waterDriver(water);
        return;
    }
    if (*g_frameUs > 0) c.acc += *g_frameUs;
    int steps = (int)(c.acc / stepUs);
    Diagnose(c, water, steps > kMaxSteps ? kMaxSteps : steps);
    if (steps == 0) {
        *(int*)(water + kOffDone) = 1;
        return;
    }
    c.acc -= steps * stepUs;
    if (steps > kMaxSteps) steps = kMaxSteps;
    LONGLONG frameUs = *g_frameUs;
    *g_frameUs = stepUs;
    for (int i = 0; i < steps; i++) {
        *(int*)(water + kOffDone) = 0;
        o_waterDriver(water);
    }
    *g_frameUs = frameUs;
}

}  // namespace

void Water_Install(int stepHz, bool diag)
{
    g_diag = diag;
    g_original = stepHz <= 0;
    if (stepHz > 0) g_stepHz = stepHz < 10 ? 10 : stepHz > 240 ? 240 : stepHz;
    o_waterDriver = (WaterDriver_t)Detour(kWaterDriver, kWaterDriverPrologue, sizeof(kWaterDriverPrologue),
                                          (void*)WaterDriverHook);
    if (!o_waterDriver) Log("water: unknown executable, wave simulation left once per frame");
    else if (g_original) Log("water: wave simulation once per frame (original), Shift+F10 switches");
    else Log("water: wave simulation at %d Hz (Xbox: 30), Shift+F10 switches", g_stepHz);
}

void Water_Toggle()
{
    if (!o_waterDriver) return;
    g_original = !g_original;
    for (Clock& c : g_clocks) c.water = nullptr;
    Gamepad_Pulse(g_original ? 2 : 1);
    if (g_original) Log("Shift+F10: wave simulation once per frame (original)");
    else Log("Shift+F10: wave simulation at %d Hz", g_stepHz);
}
