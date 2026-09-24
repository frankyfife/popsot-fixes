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
// The menu is detected by the world the game loads: every .wow file is parsed
// by 0x68bc80 (cdecl world*(data)), passed as a load callback at 0x6780ca and
// 0x68c1c2. It returns the new world, whose name (char[60]) is at +0x1d8. The
// current world *(0xaf5650) is not usable for this: loaded worlds are merged
// into a "SuperWorld", and other worlds (Prince, CameraAndGlobal, ...) are
// loaded after it. The menu camera (Camera02 of menu3D) is static, so the offset
// applies while menu3D has been loaded and the camera is at its position.

#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <string.h>
#include "menucam.h"

void Log(const char* fmt, ...);

namespace {

const DWORD kViewCall = 0x00425e48;       // call 0x437f70 inside 0x425db0
const DWORD kViewFromCamera = 0x00437f70;  // cdecl void(camera*)
const DWORD kParseWorld = 0x0068bc80;
const DWORD kParseWorldPushes[] = { 0x006780c9, 0x0068c1c1 };  // push 0x68bc80
const DWORD kWorldName = 0x1d8;
const char kMenuWorld[] = "menu3D";
const float kMenuCamPos[3] = { -83.59f, 1.39f, -2.66f };  // Camera02 in menu3D

typedef void(__cdecl* ViewFromCamera_t)(BYTE* cam);

float g_back = 0.0f;  // [menus] camera_back
bool g_installed;
bool g_menuLoaded;  // menu3D has been loaded
bool g_haveOut;
float g_base[3], g_out[3];

typedef BYTE*(__cdecl* ParseWorld_t)(void* data);

BYTE* __cdecl ParseWorldHook(void* data)
{
    BYTE* world = ((ParseWorld_t)kParseWorld)(data);
    __try {
        if (world) {
            char name[61];
            memcpy(name, world + kWorldName, 60);
            name[60] = 0;
            bool menu = _stricmp(name, kMenuWorld) == 0;
            if (menu) g_menuLoaded = true;
            Log("menu camera: world \"%s\" loaded%s", name, menu ? " (main menu)" : "");
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return world;
}

bool IsMenuCamera(const float* pos)
{
    if (!g_menuLoaded) return false;
    float d2 = 0.0f;
    for (int i = 0; i < 3; i++) d2 += (pos[i] - kMenuCamPos[i]) * (pos[i] - kMenuCamPos[i]);
    return d2 < 1.0f;
}

void __cdecl ViewFromCameraHook(BYTE* cam)
{
    __try {
        float* pos = (float*)(cam + 0xb8);
        const float* k = (const float*)(cam + 0xa8);
        bool ours = g_haveOut && memcmp(pos, g_out, sizeof(g_out)) == 0;
        if (!ours) memcpy(g_base, pos, sizeof(g_base));
        static DWORD lastLog;
        if (GetTickCount() - lastLog > 5000) {
            lastLog = GetTickCount();
            Log("menu camera: cam %p pos %.2f %.2f %.2f dir %.2f %.2f %.2f menu %d", cam, g_base[0], g_base[1],
                g_base[2], k[0], k[1], k[2], IsMenuCamera(g_base));
        }
        if (IsMenuCamera(g_base) && g_back != 0.0f) {
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
    bool known = p[0] == 0xE8 && (DWORD)(p + 5) + *(DWORD*)(p + 1) == kViewFromCamera;
    for (DWORD push : kParseWorldPushes)
        known = known && *(BYTE*)push == 0x68 && *(DWORD*)(push + 1) == kParseWorld;
    if (!known) {
        Log("menu camera: unknown executable, not enabled");
        return;
    }
    for (DWORD push : kParseWorldPushes) {
        DWORD prot;
        VirtualProtect((void*)(push + 1), 4, PAGE_EXECUTE_READWRITE, &prot);
        *(DWORD*)(push + 1) = (DWORD)ParseWorldHook;
        VirtualProtect((void*)(push + 1), 4, prot, &prot);
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
    if (GetAsyncKeyState(VK_F6) & 1) step = -0.5f;
    if (GetAsyncKeyState(VK_F7) & 1) step = 0.5f;
    if (step != 0.0f) {
        g_back += step;
        Log("menu camera: camera_back %.2f", g_back);
    }
}
