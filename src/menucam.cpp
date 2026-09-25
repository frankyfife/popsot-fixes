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
// new game starts, the camera flies from there to the Prince; see
// MenuCameraTarget for how the moved camera follows that flight.
//
// Free camera: the same hook replaces the main view's camera matrix (display
// *(0x9ec518), camera at +0xcc) with a free-flying one. Back (View) or F9
// toggles it; the game gets no input meanwhile (Start still pauses).
//   left stick / WASD    move       right stick / arrow keys  look
//   LT / RT, Q / E       down / up  LB / RB, Ctrl / Shift     slow / fast (held)
//   Y / P                freeze the world (the main loop's pause flag 0xaf4498,
//                        which also pauses the game behind the pause menu)
//   X / F6, B / F7       store the free camera as [menus] camera_start / camera_end
//                        (Ctrl+F6 / Ctrl+F7 clear them); the pad buzzes once
//                        for the start, twice for the end, three times when cleared
// The sign conventions of the I/J rows are taken from the camera when the free
// camera starts, so the view does not flip whatever handedness the engine uses.

#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "menucam.h"
#include "gamepad.h"

void Log(const char* fmt, ...);
extern char g_iniPath[MAX_PATH];

namespace {

const DWORD kViewFromCamera = 0x00437f70;  // cdecl void(camera*)
const unsigned char kViewFromCameraPrologue[] = { 0x8B, 0x44, 0x24, 0x04, 0x56, 0x57 };
const DWORD kParseWorld = 0x0068bc80;
const DWORD kParseWorldPushes[] = { 0x006780c9, 0x0068c1c1 };  // push 0x68bc80
const DWORD kWorldName = 0x1d8;
const char kMenuWorld[] = "menu3D";
const float kMenuCamPos[3] = { -83.59f, 1.39f, -2.66f };  // Camera02 in menu3D
const float kFlightEnd[3] = { -92.802f, -0.965f, 3.932f };  // end of the new-game camera flight
const float kPrince[3] = { -103.2f, -5.05f, 1.0f };           // the Prince on the balcony during that flight

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
BYTE g_baseRows[0x30];  // camera rows I, J, K before our turn

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
void Axes(float yaw, float pitch, float* f, float* r, float* u)
{
    f[0] = cosf(pitch) * cosf(yaw);
    f[1] = cosf(pitch) * sinf(yaw);
    f[2] = sinf(pitch);
    const float z[3] = { 0.0f, 0.0f, 1.0f };
    Cross(f, z, r);
    Normalize(r);
    Cross(r, f, u);
}

void FreeCamAxes(float* f, float* r, float* u) { Axes(g_free.yaw, g_free.pitch, f, r, u); }

void YawPitch(const float* k, float* yaw, float* pitch)
{
    float f[3] = { k[0], k[1], k[2] };
    Normalize(f);
    *yaw = atan2f(f[1], f[0]);
    *pitch = asinf(f[2] < -1.0f ? -1.0f : f[2] > 1.0f ? 1.0f : f[2]);
}

// Replaces the rows I, J, K of `cam` by a view along yaw/pitch without roll,
// keeping the signs the engine uses for I and J.
void WriteRows(BYTE* cam, float yaw, float pitch)
{
    float f[3], r[3], u[3];
    Axes(yaw, pitch, f, r, u);
    float* I = (float*)(cam + 0x88);
    float* J = (float*)(cam + 0x98);
    float* K = (float*)(cam + 0xa8);
    float signI = Dot(I, r) < 0.0f ? -1.0f : 1.0f;
    float signJ = Dot(J, u) < 0.0f ? -1.0f : 1.0f;
    for (int i = 0; i < 3; i++) {
        I[i] = signI * r[i];
        J[i] = signJ * u[i];
        K[i] = f[i];
    }
}

// A camera placed with the free camera: [menus] camera_start / camera_end.
struct Pose {
    bool set;
    float pos[3];
    float yaw, pitch;
};
Pose g_startPose, g_endPose;

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
        YawPitch((const float*)(cam + 0xa8), &g_free.yaw, &g_free.pitch);
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
    Log("free camera: off at %.2f %.2f %.2f (yaw %.3f pitch %.3f)", g_free.pos[0], g_free.pos[1], g_free.pos[2],
        g_free.yaw, g_free.pitch);
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

// Rotates v around the unit axis a by angle (Rodrigues).
void Rotate(float* v, const float* a, float c, float sn)
{
    float axv[3];
    Cross(a, v, axv);
    float d = Dot(a, v);
    for (int i = 0; i < 3; i++) v[i] = v[i] * c + axv[i] * sn + a[i] * d * (1.0f - c);
}

// Turns the viewing direction k so that the Prince, seen from `from` along k,
// is seen in the same place from `to`.
void AimAtPrince(float* k, const float* from, const float* to)
{
    float a[3], b[3];
    for (int i = 0; i < 3; i++) { a[i] = kPrince[i] - from[i]; b[i] = kPrince[i] - to[i]; }
    Normalize(a);
    Normalize(b);
    float axis[3];
    Cross(a, b, axis);
    float sn = sqrtf(Dot(axis, axis));
    if (sn < 1e-6f) return;
    for (int i = 0; i < 3; i++) axis[i] /= sn;
    float angle = atan2f(sn, Dot(a, b));
    Rotate(k, axis, cosf(angle), sinf(angle));
}

bool Near(const float* a, const float* b, float dist)
{
    float d[3] = { a[0] - b[0], a[1] - b[1], a[2] - b[2] };
    return Dot(d, d) < dist * dist;
}

// Where the main camera should be (false = leave it alone); *look = also turn
// it to yaw/pitch. In the menu the camera rests at kMenuCamPos; we show it from
// the start pose ([menus] camera_start) or moved by the offsets instead. When a
// new game starts, the game flies it forward and up to kFlightEnd (the balcony
// camera) in about eight seconds. We follow that flight's progress s (its
// position projected onto the line menu -> end) but interpolate from our start
// to our end (camera_end, or the game's end), so the camera travels on from
// where the menu showed it, at the game's pace. The view turns from the start
// view to the end view; without an end pose it turns to the game's view,
// corrected so the Prince appears where the game's flight shows him (our path
// runs elsewhere, looking the game's way would lose him). With an end pose the
// camera stays there while the game's camera does (the balcony camera does not
// move while the Prince is on the balcony); when the game's camera moves on we
// blend over to it in 0.75 s, after a cut we leave it alone.
bool MenuCameraTarget(const float* base, const float* k, float* out, bool* look, float* yaw, float* pitch)
{
    static bool flightDone, arrived, handover;
    static float start[3], startYaw, startPitch;
    static LARGE_INTEGER handoverStart;
    *look = false;
    if (!g_menuLoaded) return false;
    if (!g_startPose.set && !g_endPose.set && g_forward == 0.0f && g_up == 0.0f && g_side == 0.0f) return false;
    const float zero[3] = {};
    if (Near(base, zero, 0.01f)) return false;  // the main display also renders passes from the origin
    if (Near(base, kMenuCamPos, 0.05f)) {
        if (g_startPose.set) {
            memcpy(start, g_startPose.pos, sizeof(start));
            startYaw = g_startPose.yaw;
            startPitch = g_startPose.pitch;
            *look = true;
        } else {
            float right[3] = { k[1], -k[0], 0.0f };  // k x Z
            Normalize(right);
            for (int i = 0; i < 3; i++) start[i] = base[i] + g_forward * k[i] + g_side * right[i];
            start[2] += g_up;
            YawPitch(k, &startYaw, &startPitch);
        }
        memcpy(out, start, sizeof(start));
        *yaw = startYaw;
        *pitch = startPitch;
        flightDone = arrived = handover = false;
        return true;
    }
    if (handover) {  // the game's camera moves on from the end: blend over to it
        LARGE_INTEGER now, freq;
        QueryPerformanceCounter(&now);
        QueryPerformanceFrequency(&freq);
        float t = (float)(now.QuadPart - handoverStart.QuadPart) / (float)freq.QuadPart / 0.75f;
        if (t >= 1.0f) handover = false;
        if (!handover) return false;
        float w = t * t * (3.0f - 2.0f * t), gameYaw, gamePitch;
        YawPitch(k, &gameYaw, &gamePitch);
        for (int i = 0; i < 3; i++) out[i] = g_endPose.pos[i] + w * (base[i] - g_endPose.pos[i]);
        float dy = gameYaw - g_endPose.yaw;
        while (dy > 3.14159265f) dy -= 6.28318531f;
        while (dy < -3.14159265f) dy += 6.28318531f;
        *yaw = g_endPose.yaw + w * dy;
        *pitch = g_endPose.pitch + w * (gamePitch - g_endPose.pitch);
        *look = true;
        return true;
    }
    if (flightDone) return false;
    if (arrived) {  // camera_end: hold it while the game's camera stays at the end
        if (!g_endPose.set) {  // cleared meanwhile
            flightDone = true;
            return false;
        }
        if (!Near(base, kFlightEnd, 0.1f)) {
            flightDone = true;
            // A camera moving on is blended over to; after a cut it is shown at once.
            bool blend = Near(base, kFlightEnd, 3.0f);
            Log("menu camera: the game's camera leaves the flight's end (%s)", blend ? "blending over" : "cut");
            if (blend) {
                handover = true;
                QueryPerformanceCounter(&handoverStart);
                return MenuCameraTarget(base, k, out, look, yaw, pitch);
            }
            return false;
        }
        memcpy(out, g_endPose.pos, sizeof(g_endPose.pos));
        *look = true;
        *yaw = g_endPose.yaw;
        *pitch = g_endPose.pitch;
        return true;
    }
    float d[3], e[3];
    for (int i = 0; i < 3; i++) { d[i] = base[i] - kMenuCamPos[i]; e[i] = kFlightEnd[i] - kMenuCamPos[i]; }
    float s = Dot(d, e) / Dot(e, e);
    float off[3];
    for (int i = 0; i < 3; i++) off[i] = d[i] - s * e[i];
    if (Dot(off, off) > 4.0f * 4.0f || s < -0.2f) return false;  // not on the flight path
    if (s >= 0.999f) {  // arrived
        if (g_endPose.set) {
            arrived = true;
            return MenuCameraTarget(base, k, out, look, yaw, pitch);
        }
        flightDone = true;  // the game's camera takes over until the menu returns
        return false;
    }
    if (s < 0.0f) s = 0.0f;
    const float* end = g_endPose.set ? g_endPose.pos : kFlightEnd;
    for (int i = 0; i < 3; i++) out[i] = start[i] + s * (end[i] - start[i]);
    float endYaw, endPitch, w;
    if (g_endPose.set) {
        endYaw = g_endPose.yaw;
        endPitch = g_endPose.pitch;
        w = s * s * (3.0f - 2.0f * s);
    } else {
        float aimed[3] = { k[0], k[1], k[2] };
        AimAtPrince(aimed, base, out);
        YawPitch(aimed, &endYaw, &endPitch);
        w = s / 0.15f;  // the menu picture is kept; the aim comes in during the first part of the flight
        if (w > 1.0f) w = 1.0f;
        w = w * w * (3.0f - 2.0f * w);
    }
    float dy = endYaw - startYaw;
    while (dy > 3.14159265f) dy -= 6.28318531f;
    while (dy < -3.14159265f) dy += 6.28318531f;
    *yaw = startYaw + w * dy;
    *pitch = startPitch + w * (endPitch - startPitch);
    *look = true;
    return true;
}

// Stores the free camera as the start or end pose (index 0 / 1) in popfix.ini,
// or clears it.
void SavePose(int which, bool clear)
{
    Pose& p = which ? g_endPose : g_startPose;
    const char* key = which ? "camera_end" : "camera_start";
    char v[96] = "";
    p.set = !clear;
    if (!clear) {
        memcpy(p.pos, g_free.pos, sizeof(p.pos));
        p.yaw = g_free.yaw;
        p.pitch = g_free.pitch;
        sprintf(v, "%.3f %.3f %.3f %.4f %.4f", p.pos[0], p.pos[1], p.pos[2], p.yaw, p.pitch);
    }
    WritePrivateProfileStringA("menus", key, v, g_iniPath);
    Log("menu camera: %s %s", key, clear ? "cleared" : v);
    Gamepad_Pulse(clear ? 3 : which + 1);  // start: one buzz, end: two, cleared: three
}

void LoadPose(const char* key, Pose* p)
{
    char v[96];
    GetPrivateProfileStringA("menus", key, "", v, sizeof(v), g_iniPath);
    p->set = sscanf(v, "%f %f %f %f %f", &p->pos[0], &p->pos[1], &p->pos[2], &p->yaw, &p->pitch) == 5;
    if (p->set) Log("menu camera: %s %s", key, v);
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
        if (ours) memcpy(cam + 0x88, g_baseRows, sizeof(g_baseRows));  // undo our turn
        float base[3];
        memcpy(base, ours ? g_base : pos, sizeof(base));
        float target[3], yaw, pitch;
        bool look;
        if (cam == MainCamera() && MenuCameraTarget(base, k, target, &look, &yaw, &pitch)) {
            memcpy(g_baseRows, cam + 0x88, sizeof(g_baseRows));
            if (look) WriteRows(cam, yaw, pitch);
            memcpy(pos, target, sizeof(target));
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
    LoadPose("camera_start", &g_startPose);
    LoadPose("camera_end", &g_endPose);
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
    if (g_free.active) {
        // F6 / X and F7 / B store the free camera as the menu picture and as
        // the end of the new-game flight; Ctrl+F6 / Ctrl+F7 clear them.
        static bool xDown, bDown;
        bool read = Gamepad_Read(&pad);
        bool x = read && (pad.wButtons & XINPUT_GAMEPAD_X);
        bool b = read && (pad.wButtons & XINPUT_GAMEPAD_B);
        bool ctrl = GetAsyncKeyState(VK_CONTROL) < 0;
        if ((x && !xDown) || (GetAsyncKeyState(VK_F6) & 1)) SavePose(0, ctrl && !x);
        if ((b && !bDown) || (GetAsyncKeyState(VK_F7) & 1)) SavePose(1, ctrl && !b);
        xDown = x;
        bDown = b;
        return;
    }
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
