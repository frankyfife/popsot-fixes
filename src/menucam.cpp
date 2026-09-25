// Main-menu camera for wide screens.
//
// The front end renders a 3D scene (world "menu3D", a jungle set) that was built
// for a 4:3 view. With a wider view (GOG's widescreen option keeps the vertical
// field of view and widens the horizontal one) the set runs out at the left and
// right edges. The PS3 HD version moves the menu camera; we move it too, forward
// (past the foreground plants) and up, by an amount tuned by eye.
//
// The view matrix is built from the camera matrix by 0x437f70, several times per
// frame (main view 0x425db0, visibility 0x47b3b0, ...). The camera struct (display +0xcc)
// holds the camera's world matrix at +0x88: rows I (+0x88), J (+0x98),
// K (+0xa8, the viewing direction) and the position at +0xb8. We detour
// 0x437f70 and, in the main menu, move the position along K, sideways (K x Z,
// to the right) and up along the world Z axis first.
// The engine rewrites the camera matrix every frame; if it did not, the stored
// base position is reused so the offset never accumulates.
//
// The menu is detected by the world the game loads: every .wow file is parsed
// by 0x68bc80 (cdecl world*(data)), passed as a load callback at 0x6780ca and
// 0x68c1c2. It returns the new world, whose name (char[60]) is at +0x1d8. The
// current world *(0xaf5650) is not usable for this: loaded worlds are merged
// into a "SuperWorld", and other worlds (Prince, CameraAndGlobal, ...) are
// loaded after it. The menu camera (Camera02 of menu3D) is static, so the offset
// applies while menu3D has been loaded and the camera is at its position. When a
// new game starts, the camera flies from there to the Prince; the offset fades
// out over the first kFadeDistance units of that flight instead of snapping back.
//
// Free camera: the same hook replaces the main view's camera matrix (display
// *(0x9ec518), camera at +0xcc) with a free-flying one. Back (View) or F9
// toggles it; the game gets no input meanwhile (Start still pauses).
//   left stick / WASD    move       right stick / arrow keys  look
//   LT / RT, Q / E       down / up  LB / RB, Ctrl / Shift     slow / fast (held)
//   Y / P                freeze the world (the main loop's pause flag 0xaf4498,
//                        which also pauses the game behind the pause menu)
// The sign conventions of the I/J rows are taken from the camera when the free
// camera starts, so the view does not flip whatever handedness the engine uses.

#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <string.h>
#include <math.h>
#include "menucam.h"
#include "gamepad.h"

void Log(const char* fmt, ...);

namespace {

const DWORD kViewFromCamera = 0x00437f70;  // cdecl void(camera*)
const unsigned char kViewFromCameraPrologue[] = { 0x8B, 0x44, 0x24, 0x04, 0x56, 0x57 };
const DWORD kParseWorld = 0x0068bc80;
const DWORD kParseWorldPushes[] = { 0x006780c9, 0x0068c1c1 };  // push 0x68bc80
const DWORD kWorldName = 0x1d8;
const char kMenuWorld[] = "menu3D";
const float kMenuCamPos[3] = { -83.59f, 1.39f, -2.66f };  // Camera02 in menu3D
const float kFadeDistance = 8.0f;

typedef void(__cdecl* ViewFromCamera_t)(BYTE* cam);
ViewFromCamera_t g_viewFromCamera;  // trampoline

float g_forward = 0.0f;  // [menus] camera_forward
float g_up = 0.0f;    // [menus] camera_up
float g_side = 0.0f;  // [menus] camera_side (positive = right)
bool g_installed;
bool g_menuLoaded;  // menu3D has been loaded
BYTE* g_cam;  // camera we moved last
bool g_haveOut;
float g_base[3], g_out[3];

// Free camera state.
const DWORD kCurrentDisplay = 0x009ec518;  // display being rendered (set by 0x425db0)
DWORD* const g_pauseFlag = (DWORD*)0x00af4498;
struct FreeCam {
    bool active;
    BYTE* cam;       // main camera struct it replaces
    float pos[3];
    float yaw, pitch;  // radians; forward = (cos p cos y, cos p sin y, sin p)
    float signI, signJ;
    LARGE_INTEGER last;
    bool frozen;       // we set the pause flag
    bool freezeDown;   // Y / P held last frame
} g_free;

void Cross(const float* a, const float* b, float* r)
{
    r[0] = a[1] * b[2] - a[2] * b[1];
    r[1] = a[2] * b[0] - a[0] * b[2];
    r[2] = a[0] * b[1] - a[1] * b[0];
}

float Dot(const float* a, const float* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

void Normalize(float* v)
{
    float l = sqrtf(Dot(v, v));
    if (l > 1e-6f) { v[0] /= l; v[1] /= l; v[2] /= l; }
}

// Camera rows from yaw/pitch: forward f, right r = f x Z, up u = r x f.
void FreeCamAxes(float* f, float* r, float* u)
{
    f[0] = cosf(g_free.pitch) * cosf(g_free.yaw);
    f[1] = cosf(g_free.pitch) * sinf(g_free.yaw);
    f[2] = sinf(g_free.pitch);
    const float z[3] = { 0.0f, 0.0f, 1.0f };
    Cross(f, z, r);
    Normalize(r);
    Cross(r, f, u);
}

BYTE* MainCamera()
{
    BYTE* display = *(BYTE**)kCurrentDisplay;
    return display ? display + 0xcc : nullptr;
}

void StartFreeCam()
{
    BYTE* cam = MainCamera();
    if (!cam) return;
    __try {
        const float* I = (const float*)(cam + 0x88);
        const float* J = (const float*)(cam + 0x98);
        const float* K = (const float*)(cam + 0xa8);
        float f[3] = { K[0], K[1], K[2] };
        Normalize(f);
        g_free.yaw = atan2f(f[1], f[0]);
        g_free.pitch = asinf(f[2] < -1.0f ? -1.0f : f[2] > 1.0f ? 1.0f : f[2]);
        float ff[3], r[3], u[3];
        FreeCamAxes(ff, r, u);
        g_free.signI = Dot(I, r) < 0.0f ? -1.0f : 1.0f;
        g_free.signJ = Dot(J, u) < 0.0f ? -1.0f : 1.0f;
        memcpy(g_free.pos, cam + 0xb8, sizeof(g_free.pos));
        g_free.cam = cam;
        g_free.active = true;
        QueryPerformanceCounter(&g_free.last);
        Gamepad_BlockGame(true);
        Log("free camera: on at %.2f %.2f %.2f (I %+.0f J %+.0f)", g_free.pos[0], g_free.pos[1], g_free.pos[2],
            g_free.signI, g_free.signJ);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

void SetFrozen(bool on)
{
    if (on == g_free.frozen) return;
    if (on && *g_pauseFlag) return;  // the game is paused already
    *g_pauseFlag = on ? 1 : 0;
    g_free.frozen = on;
    Log("free camera: world %s", on ? "frozen" : "running");
}

void StopFreeCam()
{
    SetFrozen(false);
    g_free.active = false;
    Gamepad_BlockGame(false);
    Log("free camera: off");
}

float StickAxis(SHORT v, SHORT deadZone)
{
    float f = v / 32767.0f, d = deadZone / 32767.0f;
    if (f > -d && f < d) return 0.0f;
    f = f > 0 ? (f - d) / (1.0f - d) : (f + d) / (1.0f - d);
    return f < -1.0f ? -1.0f : f > 1.0f ? 1.0f : f;
}

void UpdateFreeCam()
{
    LARGE_INTEGER now, freq;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&freq);
    float dt = (float)(now.QuadPart - g_free.last.QuadPart) / (float)freq.QuadPart;
    g_free.last = now;
    if (dt > 0.1f) dt = 0.1f;
    XINPUT_GAMEPAD pad;
    if (!Gamepad_Read(&pad)) memset(&pad, 0, sizeof(pad));
    auto key = [](int vk) { return GetAsyncKeyState(vk) < 0 ? 1.0f : 0.0f; };
    bool freeze = (pad.wButtons & XINPUT_GAMEPAD_Y) || key('P') > 0;
    if (freeze && !g_free.freezeDown) SetFrozen(!g_free.frozen);
    g_free.freezeDown = freeze;
    float speed = 6.0f;  // world units per second
    if ((pad.wButtons & XINPUT_GAMEPAD_RIGHT_SHOULDER) || key(VK_SHIFT)) speed *= 4.0f;
    if ((pad.wButtons & XINPUT_GAMEPAD_LEFT_SHOULDER) || key(VK_CONTROL)) speed *= 0.25f;
    const float turn = 2.0f;  // radians per second at full deflection
    float lookX = StickAxis(pad.sThumbRX, XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE) + key(VK_RIGHT) - key(VK_LEFT);
    float lookY = StickAxis(pad.sThumbRY, XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE) + key(VK_UP) - key(VK_DOWN);
    g_free.yaw -= lookX * turn * dt;
    g_free.pitch += lookY * turn * dt;
    if (g_free.pitch > 1.5f) g_free.pitch = 1.5f;
    if (g_free.pitch < -1.5f) g_free.pitch = -1.5f;
    float f[3], r[3], u[3];
    FreeCamAxes(f, r, u);
    float fwd = StickAxis(pad.sThumbLY, XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE) + key('W') - key('S');
    float side = StickAxis(pad.sThumbLX, XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE) + key('D') - key('A');
    float lift = (pad.bRightTrigger - pad.bLeftTrigger) / 255.0f + key('E') - key('Q');
    for (int i = 0; i < 3; i++) g_free.pos[i] += (f[i] * fwd + r[i] * side) * speed * dt;
    g_free.pos[2] += lift * speed * dt;
}

// Writes the free camera into the camera struct.
void ApplyFreeCam(BYTE* cam)
{
    float f[3], r[3], u[3];
    FreeCamAxes(f, r, u);
    float* I = (float*)(cam + 0x88);
    float* J = (float*)(cam + 0x98);
    float* K = (float*)(cam + 0xa8);
    for (int i = 0; i < 3; i++) {
        I[i] = g_free.signI * r[i];
        J[i] = g_free.signJ * u[i];
        K[i] = f[i];
    }
    memcpy(cam + 0xb8, g_free.pos, sizeof(g_free.pos));
}

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
            if (menu && !g_menuLoaded) Log("menu camera: main menu world \"%s\" loaded", name);
            if (menu) g_menuLoaded = true;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return world;
}

// 1 at the menu camera position, fading to 0 kFadeDistance units away.
float MenuCameraWeight(const float* pos)
{
    if (!g_menuLoaded) return 0.0f;
    float d2 = 0.0f;
    for (int i = 0; i < 3; i++) d2 += (pos[i] - kMenuCamPos[i]) * (pos[i] - kMenuCamPos[i]);
    if (d2 >= kFadeDistance * kFadeDistance) return 0.0f;
    float w = 1.0f - sqrtf(d2) / kFadeDistance;
    return w * w * (3.0f - 2.0f * w);  // smoothstep
}

void* Detour(DWORD addr, const unsigned char* prologue, size_t len, void* hook)
{
    unsigned char* fn = (unsigned char*)addr;
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

void __cdecl ViewFromCameraHook(BYTE* cam)
{
    if (g_free.active && cam == g_free.cam) {
        __try {
            ApplyFreeCam(cam);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
        g_viewFromCamera(cam);
        return;
    }
    __try {
        float* pos = (float*)(cam + 0xb8);
        const float* k = (const float*)(cam + 0xa8);
        // Several cameras pass through here every frame; the offset state belongs
        // to the one we moved last.
        bool ours = cam == g_cam && g_haveOut && memcmp(pos, g_out, sizeof(g_out)) == 0;
        float base[3];
        memcpy(base, ours ? g_base : pos, sizeof(base));
        float w = MenuCameraWeight(base);
        if (w > 0.0f && (g_forward != 0.0f || g_up != 0.0f || g_side != 0.0f)) {
            float right[3] = { k[1], -k[0], 0.0f };  // k x Z
            Normalize(right);
            for (int i = 0; i < 3; i++) pos[i] = base[i] + w * (g_forward * k[i] + g_side * right[i]);
            pos[2] += w * g_up;
            g_cam = cam;
            memcpy(g_base, base, sizeof(g_base));
            memcpy(g_out, pos, sizeof(g_out));
            g_haveOut = true;
        } else if (ours) {
            memcpy(pos, g_base, sizeof(g_base));
            g_haveOut = false;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    g_viewFromCamera(cam);
}

}  // namespace

void MenuCam_Install(float forward, float up, float side)
{
    if (g_installed) return;
    g_forward = forward;
    g_up = up;
    g_side = side;
    bool known = memcmp((void*)kViewFromCamera, kViewFromCameraPrologue, sizeof(kViewFromCameraPrologue)) == 0;
    for (DWORD push : kParseWorldPushes)
        known = known && *(BYTE*)push == 0x68 && *(DWORD*)(push + 1) == kParseWorld;
    if (!known) {
        Log("menu camera: unknown executable, not enabled");
        return;
    }
    g_viewFromCamera = (ViewFromCamera_t)Detour(kViewFromCamera, kViewFromCameraPrologue,
                                                sizeof(kViewFromCameraPrologue), (void*)ViewFromCameraHook);
    if (!g_viewFromCamera) return;
    for (DWORD push : kParseWorldPushes) {
        DWORD prot;
        VirtualProtect((void*)(push + 1), 4, PAGE_EXECUTE_READWRITE, &prot);
        *(DWORD*)(push + 1) = (DWORD)ParseWorldHook;
        VirtualProtect((void*)(push + 1), 4, prot, &prot);
    }
    g_installed = true;
    Log("menu camera: enabled, camera_forward %.2f camera_up %.2f camera_side %.2f", g_forward, g_up, g_side);
}

void MenuCam_OnPresent(bool keys)
{
    if (!g_installed) return;
    // Free camera toggle: F9 or the controller's Back (View) button.
    static bool backDown;
    XINPUT_GAMEPAD pad;
    bool back = keys && Gamepad_Read(&pad) && (pad.wButtons & XINPUT_GAMEPAD_BACK);
    bool toggle = (back && !backDown) || (keys && (GetAsyncKeyState(VK_F9) & 1));
    backDown = back;
    if (toggle) {
        if (g_free.active) StopFreeCam();
        else StartFreeCam();
    }
    if (g_free.active) UpdateFreeCam();
    if (!keys) return;
    // F6 / F7 move the menu camera closer / further, with Shift down / up and
    // with Ctrl left / right.
    int dir = 0;
    if (GetAsyncKeyState(VK_F6) & 1) dir = -1;
    if (GetAsyncKeyState(VK_F7) & 1) dir = 1;
    if (!dir) return;
    if (GetAsyncKeyState(VK_SHIFT) < 0) g_up += dir * 0.25f;
    else if (GetAsyncKeyState(VK_CONTROL) < 0) g_side += dir * 0.25f;
    else g_forward -= dir * 0.5f;
    Log("menu camera: camera_forward %.2f camera_up %.2f camera_side %.2f", g_forward, g_up, g_side);
}
