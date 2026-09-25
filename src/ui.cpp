// 2D elements (menus, texts, HUD) in 4:3 proportions on wide screens.
//
// Menus and texts are laid out in a virtual 640x480 space and drawn as
// pre-transformed quads (XYZRHW) in back buffer pixels, the virtual space
// stretched over the whole screen (x * width / 640, y * height / 480). On a
// wide screen everything is stretched horizontally. All of them go through two
// helpers, both cdecl:
//   0x661970  textured quad (x0, y0, x1, y1, u0, v0, u1, v1, colours x4, angle, flags)
//   0x661790  coloured quad (x, y, width / screen width, height / screen height, colour, flags)
// We move their corners towards the screen centre: x by 4:3 / screen aspect
// (and [ui] scale), y by [ui] scale. Quads covering the whole width (fades,
// letterbox bars, full-screen overlays) are left alone.

#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <string.h>
#include "ui.h"

void Log(const char* fmt, ...);

namespace {

const DWORD kTexturedQuad = 0x00661970;
const unsigned char kTexturedQuadPrologue[] = { 0x83, 0xEC, 0x0C, 0x8B, 0x44, 0x24, 0x44 };
const DWORD kColouredQuad = 0x00661790;
const unsigned char kColouredQuadPrologue[] = { 0x83, 0xEC, 0x08, 0x8B, 0x44, 0x24, 0x20 };
const int* const g_screenWidth = (const int*)0x00ae1614;
const int* const g_screenHeight = (const int*)0x00ae1618;

bool g_aspect = true;
float g_scale = 1.0f;

}  // namespace

extern "C" {
void* g_uiTexturedQuad;  // trampolines
void* g_uiColouredQuad;
}

namespace {

// Scale factors for the current screen; false = nothing to do.
bool Factors(float* kx, float* ky, float* cx, float* cy, float* w)
{
    int sw = *g_screenWidth, sh = *g_screenHeight;
    if (sw <= 0 || sh <= 0) return false;
    float aspect = (float)sw / (float)sh;
    *kx = (g_aspect && aspect > 4.0f / 3.0f ? (4.0f / 3.0f) / aspect : 1.0f) * g_scale;
    *ky = g_scale;
    if (*kx == 1.0f && *ky == 1.0f) return false;
    *cx = sw * 0.5f;
    *cy = sh * 0.5f;
    *w = (float)sw;
    return true;
}

}  // namespace

extern "C" void __cdecl UiTexturedQuad(float* a)  // a[0..3] = x0, y0, x1, y1
{
    float kx, ky, cx, cy, w;
    if (!Factors(&kx, &ky, &cx, &cy, &w)) return;
    if (a[0] <= 1.0f && a[2] >= w - 1.0f) return;  // full width
    a[0] = cx + (a[0] - cx) * kx;
    a[2] = cx + (a[2] - cx) * kx;
    a[1] = cy + (a[1] - cy) * ky;
    a[3] = cy + (a[3] - cy) * ky;
}

extern "C" void __cdecl UiColouredQuad(float* a)  // a[0..3] = x, y, width / screen width, height / screen height
{
    float kx, ky, cx, cy, w;
    if (!Factors(&kx, &ky, &cx, &cy, &w)) return;
    if (a[0] <= 1.0f && a[0] + a[2] * w >= w - 1.0f) return;  // full width
    a[0] = cx + (a[0] - cx) * kx;
    a[2] *= kx;
    a[1] = cy + (a[1] - cy) * ky;
    a[3] *= ky;
}

extern "C" __declspec(naked) void UiTexturedQuadHook()
{
    __asm {
        lea eax, [esp + 4]
        push eax
        call UiTexturedQuad
        add esp, 4
        jmp dword ptr [g_uiTexturedQuad]
    }
}

extern "C" __declspec(naked) void UiColouredQuadHook()
{
    __asm {
        lea eax, [esp + 4]
        push eax
        call UiColouredQuad
        add esp, 4
        jmp dword ptr [g_uiColouredQuad]
    }
}

namespace {

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

}  // namespace

void Ui_Install(bool aspect, float scale)
{
    static bool done;
    if (done) return;
    done = true;
    g_aspect = aspect;
    g_scale = scale < 0.25f ? 0.25f : scale > 2.0f ? 2.0f : scale;
    if (!g_aspect && g_scale == 1.0f) return;
    g_uiTexturedQuad = Detour(kTexturedQuad, kTexturedQuadPrologue, sizeof(kTexturedQuadPrologue), (void*)UiTexturedQuadHook);
    g_uiColouredQuad = Detour(kColouredQuad, kColouredQuadPrologue, sizeof(kColouredQuadPrologue), (void*)UiColouredQuadHook);
    Log("ui: 2D elements %s, scale %.2f (%s)", g_aspect ? "in 4:3 proportions" : "stretched", g_scale,
        g_uiTexturedQuad && g_uiColouredQuad ? "installed" : "unknown executable, not installed");
}
