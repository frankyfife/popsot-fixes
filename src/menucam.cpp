// Main-menu camera for wide screens.
//
// The front end renders a 3D scene (world "menu3D", a jungle set) that was built
// for a 4:3 view. With a wider view (GOG's widescreen option keeps the vertical
// field of view and widens the horizontal one) the set runs out at the left and
// right edges. The PS3 HD version solves this by placing the menu camera further
// back; we do the same.
//
// Every frame the main view builds its view matrix from the camera matrix with
// 0x437f70 (called from 0x425db0 at 0x425e48). The camera struct (display +0xcc)
// holds the camera's world matrix at +0x88: rows I (+0x88), J (+0x98),
// K (+0xa8, viewing direction) and the position at +0xb8. We redirect that call
// and, while the menu world is loaded, move the position back along K first.
// The engine rewrites the camera matrix every frame; if it did not, the stored
// base position is reused so the offset never accumulates.
//
// The current world is *(0xaf5650); its name (char[60]) is at +0x1d8, filled by
// the world loader 0x68bc80 from the .wow file.

#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <string.h>
#include "menucam.h"

void Log(const char* fmt, ...);

namespace {

const DWORD kViewCall = 0x00425e48;       // call 0x437f70 inside 0x425db0
const DWORD kViewFromCamera = 0x00437f70;  // cdecl void(camera*)
const DWORD kCurrentWorld = 0x00af5650;
const DWORD kWorldName = 0x1d8;
const char kMenuWorld[] = "menu3D";

typedef void(__cdecl* ViewFromCamera_t)(BYTE* cam);

float g_back = 0.0f;  // [menus] camera_back
bool g_installed;
BYTE* g_lastWorld;
bool g_inMenu;
bool g_haveOut;
float g_base[3], g_out[3];

bool IsMenuWorld()
{
    BYTE* world = *(BYTE**)kCurrentWorld;
    if (world != g_lastWorld) {
        g_lastWorld = world;
        g_inMenu = false;
        if (world) {
            char name[61];
            memcpy(name, world + kWorldName, 60);
            name[60] = 0;
            g_inMenu = _stricmp(name, kMenuWorld) == 0;
            Log("menu camera: world \"%s\"%s", name, g_inMenu ? " (menu)" : "");
        }
    }
    return g_inMenu;
}

void __cdecl ViewFromCameraHook(BYTE* cam)
{
    __try {
        float* pos = (float*)(cam + 0xb8);
        const float* k = (const float*)(cam + 0xa8);
        bool ours = g_haveOut && memcmp(pos, g_out, sizeof(g_out)) == 0;
        if (!ours) memcpy(g_base, pos, sizeof(g_base));
        if (IsMenuWorld() && g_back != 0.0f) {
            for (int i = 0; i < 3; i++) pos[i] = g_base[i] - g_back * k[i];
            memcpy(g_out, pos, sizeof(g_out));
            g_haveOut = true;
        } else {
            if (ours) memcpy(pos, g_base, sizeof(g_base));
            g_haveOut = false;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    ((ViewFromCamera_t)kViewFromCamera)(cam);
}

}  // namespace

void MenuCam_Install(float back)
{
    if (g_installed) return;
    g_back = back;
    unsigned char* p = (unsigned char*)kViewCall;
    if (p[0] != 0xE8 || (DWORD)(p + 5) + *(DWORD*)(p + 1) != kViewFromCamera) {
        Log("menu camera: unknown executable, not enabled");
        return;
    }
    DWORD prot;
    VirtualProtect(p, 5, PAGE_EXECUTE_READWRITE, &prot);
    *(DWORD*)(p + 1) = (DWORD)ViewFromCameraHook - (DWORD)(p + 5);
    VirtualProtect(p, 5, prot, &prot);
    FlushInstructionCache(GetCurrentProcess(), p, 5);
    g_installed = true;
    Log("menu camera: enabled, camera_back %.2f", g_back);
}

void MenuCam_OnPresent(bool keys)
{
    if (!g_installed || !keys) return;
    // F6 / F7 move the menu camera closer / further while tuning.
    float step = 0.0f;
    if (GetAsyncKeyState(VK_F6) & 1) step = -0.25f;
    if (GetAsyncKeyState(VK_F7) & 1) step = 0.25f;
    if (step != 0.0f) {
        g_back += step;
        Log("menu camera: camera_back %.2f", g_back);
    }
}
