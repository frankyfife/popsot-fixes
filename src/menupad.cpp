// Controller navigation for the PC front-end menus.
//
// The PC menus (PCMenuManager / MNU_* pages) only react to the mouse. Their
// UI manager exposes mouse move / button entry points in a virtual 640x480
// space, so we drive those from an XInput pad: the D-pad / left stick jumps
// between navigation targets on the top page (the target under the cursor gets
// its normal hover highlight), A clicks it, B sends Escape.
//
// Navigation targets are plain interactive elements, the individual rows of
// list widgets (MNU_List, e.g. the main menu entries) and a list's scroll
// arrows. List rows are found by probing the list's own hit test.
//
// Everything here runs on the game's render thread (called from Present).

#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <xinput.h>
#include <math.h>
#include "menupad.h"

void Log(const char* fmt, ...);

namespace {

// ---------------------------------------------------------------- game interface (POP.EXE v181 / gpp.exe)
const DWORD kMenuRootPtr = 0x0080bbd8;  // PCMenuManager*; UI manager lives at +0xc

// UI manager (thiscall, callee cleans one argument)
typedef void(__thiscall* MgrMouseMove_t)(void* mgr, const DWORD* packedPos);
typedef void(__thiscall* MgrMouseButton_t)(void* mgr, const DWORD* event);  // {button, packedPos}
const MgrMouseMove_t MgrMouseMove = (MgrMouseMove_t)0x007125f0;
const MgrMouseButton_t MgrMouseDown = (MgrMouseButton_t)0x00712500;
const MgrMouseButton_t MgrMouseUp = (MgrMouseButton_t)0x00712530;

// Page elements: element->+0x2c is the widget, whose vtable +0x10 returns its rect.
typedef char(__cdecl* ElemIsVisible_t)(void* elem);
typedef void(__thiscall* WidgetGetRect_t)(void* widget, short* rect);  // x0, x1, y0, y1
const ElemIsVisible_t ElemIsVisible = (ElemIsVisible_t)0x00711e40;

// MNU_List widget
const DWORD kListMouseUp = 0x007150c0;  // vtable slot 0x24 of every list widget
typedef int(__thiscall* ListHitTest_t)(void* list, const short* pos);  // row index or -1
const ListHitTest_t ListHitTest = (ListHitTest_t)0x00714e70;
// list fields: +0x14 up-arrow present, +0x64 up-arrow rect, +0x20 down-arrow present,
// +0x74 down-arrow rect, +0x6c rows rect (all rects x0, x1, y0, y1 shorts)

// Prologue bytes checked before anything is called, so an unknown executable
// build simply leaves the feature disabled.
struct Signature { DWORD addr; unsigned char bytes[10]; };
const Signature kSignatures[] = {
    { 0x007125f0, { 0x83, 0xEC, 0x08, 0x8B, 0x44, 0x24, 0x0C, 0x8B, 0x00, 0x89 } },
    { 0x00712500, { 0x8B, 0x54, 0x24, 0x04, 0x8B, 0x42, 0x04, 0x89, 0x41, 0x1A } },
    { 0x00712530, { 0x8B, 0x54, 0x24, 0x04, 0x8B, 0x42, 0x04, 0x89, 0x41, 0x1A } },
    { 0x00711e40, { 0x56, 0x8B, 0x74, 0x24, 0x08, 0x85, 0xF6, 0x75, 0x17, 0x68 } },
    { 0x00716d90, { 0x83, 0xEC, 0x0C, 0x53, 0x55, 0x56, 0x57, 0x8B, 0xF9, 0x8B } },
    { 0x00714e70, { 0x83, 0xEC, 0x0C, 0x53, 0x55, 0x56, 0x8B, 0xF1, 0x8B, 0x4E } },
};

// ---------------------------------------------------------------- XInput
typedef DWORD(WINAPI* XInputGetState_t)(DWORD, XINPUT_STATE*);
XInputGetState_t pXInputGetState;

bool LoadXInput()
{
    const char* dlls[] = { "xinput1_4.dll", "xinput1_3.dll", "xinput9_1_0.dll" };
    for (const char* name : dlls) {
        if (HMODULE h = LoadLibraryA(name)) {
            pXInputGetState = (XInputGetState_t)GetProcAddress(h, "XInputGetState");
            if (pXInputGetState) { Log("menu pad: using %s", name); return true; }
        }
    }
    Log("menu pad: no XInput DLL found");
    return false;
}

bool ReadPad(XINPUT_GAMEPAD& out)
{
    for (DWORD i = 0; i < XUSER_MAX_COUNT; i++) {
        XINPUT_STATE st;
        if (pXInputGetState(i, &st) == ERROR_SUCCESS) { out = st.Gamepad; return true; }
    }
    return false;
}

// ---------------------------------------------------------------- navigation targets
enum { SUB_WHOLE = -100, SUB_ARROW_UP = -2, SUB_ARROW_DOWN = -3 };  // sub >= 0: list row

struct Target {
    void* elem;
    int sub;         // SUB_WHOLE, a list row index or a scroll arrow
    int cx, cy;      // click position (virtual 640x480)
};
const int kMaxTargets = 96;

struct Selection { void* elem; int sub; };

bool g_ready, g_checked;
HWND g_hwnd;
void* g_lastPage;
Selection g_sel;
WORD g_prevButtons;
int g_heldDir;           // 0 none, 1 up, 2 down, 3 left, 4 right
DWORD g_nextRepeat;
bool g_clickPending;     // button-down sent, button-up due next frame
DWORD g_clickPos;

bool CheckSignatures()
{
    for (const Signature& s : kSignatures) {
        __try {
            if (memcmp((const void*)s.addr, s.bytes, sizeof(s.bytes)) != 0) {
                Log("menu pad: signature mismatch at %08lx, feature disabled", s.addr);
                return false;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            Log("menu pad: signature read failed at %08lx, feature disabled", s.addr);
            return false;
        }
    }
    return true;
}

void* UiManager()
{
    DWORD root = *(DWORD*)kMenuRootPtr;
    return root ? (void*)(root + 0xc) : nullptr;
}

// Top page of the UI manager's page stack, or null when no menu is open.
void* TopPage(void* mgr)
{
    int count = *(int*)((char*)mgr + 0x10);
    if (count < 1) return nullptr;
    char* entries = *(char**)((char*)mgr + 4);
    return entries ? *(void**)(entries + (count - 1) * 8) : nullptr;
}

DWORD Pack(int x, int y) { return (DWORD)(WORD)(short)x | ((DWORD)(WORD)(short)y << 16); }

const short* Field(void* obj, int off) { return (const short*)((char*)obj + off); }

bool IsList(void* widget)
{
    DWORD* vt = *(DWORD**)widget;
    return vt && vt[0x24 / 4] == kListMouseUp;
}

void AddTarget(Target* out, int& n, void* elem, int sub, int cx, int cy)
{
    if (n >= kMaxTargets) return;
    out[n].elem = elem;
    out[n].sub = sub;
    out[n].cx = cx;
    out[n].cy = cy;
    n++;
}

// Rows of a list widget: probe its hit test down the middle of the rows rect.
void AddListTargets(Target* out, int& n, void* elem, void* list)
{
    const short* rows = Field(list, 0x6c);
    int x = (rows[0] + rows[1]) / 2;
    int curRow = -1, rowStart = 0;
    for (int y = rows[2]; y <= rows[3] + 1; y++) {
        int row = -1;
        if (y <= rows[3]) {
            short pos[2] = { (short)x, (short)y };
            row = ListHitTest(list, pos);
        }
        if (row != curRow) {
            if (curRow >= 0) AddTarget(out, n, elem, curRow, x, (rowStart + y - 1) / 2);
            curRow = row;
            rowStart = y;
        }
    }
    if (*(int*)((char*)list + 0x14)) {
        const short* r = Field(list, 0x64);
        AddTarget(out, n, elem, SUB_ARROW_UP, (r[0] + r[1]) / 2, (r[2] + r[3]) / 2);
    }
    if (*(int*)((char*)list + 0x20)) {
        const short* r = Field(list, 0x74);
        AddTarget(out, n, elem, SUB_ARROW_DOWN, (r[0] + r[1]) / 2, (r[2] + r[3]) / 2);
    }
}

int CollectTargets(void* page, Target* out)
{
    char* begin = *(char**)((char*)page + 0x28);
    char* end = *(char**)((char*)page + 0x2c);
    if (!begin || end <= begin) return 0;
    int n = 0;
    for (void** it = (void**)begin; it < (void**)end; it++) {
        void* e = *it;
        if (!e || !ElemIsVisible(e)) continue;
        if (!(*((unsigned char*)e + 100) & 8)) continue;  // not an interactive element
        void* widget = *(void**)((char*)e + 0x2c);
        if (!widget) continue;
        if (IsList(widget)) { AddListTargets(out, n, e, widget); continue; }
        short r[4] = { 0, 0, 0, 0 };
        WidgetGetRect_t getRect = *(WidgetGetRect_t*)(*(char**)widget + 0x10);
        getRect(widget, r);
        if (r[1] <= r[0] || r[3] <= r[2]) continue;
        AddTarget(out, n, e, SUB_WHOLE, (r[0] + r[1]) / 2, (r[2] + r[3]) / 2);
    }
    return n;
}

int FindSelected(const Target* t, int n)
{
    for (int i = 0; i < n; i++)
        if (t[i].elem == g_sel.elem && t[i].sub == g_sel.sub) return i;
    return -1;
}

// Spatial navigation: best target in the given direction from `from`.
int Neighbour(const Target* t, int n, int from, int dir)
{
    int best = -1;
    double bestCost = 1e30;
    for (int i = 0; i < n; i++) {
        if (i == from) continue;
        int dx = t[i].cx - t[from].cx, dy = t[i].cy - t[from].cy;
        int along, across;
        switch (dir) {
        case 1: along = -dy; across = dx; break;
        case 2: along = dy; across = dx; break;
        case 3: along = -dx; across = dy; break;
        default: along = dx; across = dy; break;
        }
        if (along <= 0) continue;
        double cost = along + 2.0 * fabs((double)across);
        if (cost < bestCost) { bestCost = cost; best = i; }
    }
    return best;
}

void Select(void* mgr, const Target& t)
{
    g_sel.elem = t.elem;
    g_sel.sub = t.sub;
    DWORD pos = Pack(t.cx, t.cy);
    MgrMouseMove(mgr, &pos);
}

void LogPage(void* page, const Target* t, int n)
{
    Log("menu pad: page %p, %d targets", page, n);
    for (int i = 0; i < n; i++)
        Log("  [%d] elem %p sub %d at %d,%d", i, t[i].elem, t[i].sub, t[i].cx, t[i].cy);
}

void SendKey(WPARAM vk)
{
    if (!g_hwnd) return;
    UINT scan = MapVirtualKeyA((UINT)vk, MAPVK_VK_TO_VSC);
    PostMessageA(g_hwnd, WM_KEYDOWN, vk, 1 | (scan << 16));
    PostMessageA(g_hwnd, WM_KEYUP, vk, 1 | (scan << 16) | (1u << 30) | (1u << 31));
}

int StickDirection(const XINPUT_GAMEPAD& p)
{
    if (p.wButtons & XINPUT_GAMEPAD_DPAD_UP) return 1;
    if (p.wButtons & XINPUT_GAMEPAD_DPAD_DOWN) return 2;
    if (p.wButtons & XINPUT_GAMEPAD_DPAD_LEFT) return 3;
    if (p.wButtons & XINPUT_GAMEPAD_DPAD_RIGHT) return 4;
    const int dz = 16000;
    int x = p.sThumbLX, y = p.sThumbLY;
    if (abs(x) < dz && abs(y) < dz) return 0;
    if (abs(y) >= abs(x)) return y > 0 ? 1 : 2;
    return x < 0 ? 3 : 4;
}

void Update()
{
    void* mgr = UiManager();
    if (!mgr) return;

    // Finish a click started last frame.
    if (g_clickPending) {
        g_clickPending = false;
        DWORD ev[2] = { 1, g_clickPos };
        MgrMouseUp(mgr, ev);
        return;
    }

    XINPUT_GAMEPAD pad;
    bool havePad = ReadPad(pad);
    WORD buttons = havePad ? pad.wButtons : 0;
    WORD pressed = buttons & ~g_prevButtons;
    g_prevButtons = buttons;

    void* page = TopPage(mgr);
    if (!page) { g_lastPage = nullptr; g_sel.elem = nullptr; g_heldDir = 0; return; }
    if (!havePad) return;

    Target t[kMaxTargets];
    int n = CollectTargets(page, t);
    if (page != g_lastPage) {
        g_lastPage = page;
        g_sel.elem = nullptr;
        LogPage(page, t, n);
    }
    if (n == 0) return;

    int cur = FindSelected(t, n);

    // Direction with auto-repeat.
    int dir = StickDirection(pad);
    DWORD now = GetTickCount();
    bool step = false;
    if (dir && dir != g_heldDir) { step = true; g_nextRepeat = now + 400; }
    else if (dir && (int)(now - g_nextRepeat) >= 0) { step = true; g_nextRepeat = now + 140; }
    g_heldDir = dir;

    if (step) {
        int next = (cur < 0) ? 0 : Neighbour(t, n, cur, dir);
        if (next >= 0) { Select(mgr, t[next]); cur = next; }
    }

    if (pressed & XINPUT_GAMEPAD_A) {
        if (cur < 0) {
            Select(mgr, t[0]);  // nothing selected yet: select first, click on next press
        } else {
            Select(mgr, t[cur]);
            g_clickPos = Pack(t[cur].cx, t[cur].cy);
            DWORD ev[2] = { 1, g_clickPos };
            MgrMouseDown(mgr, ev);
            g_clickPending = true;
        }
    }
    if (pressed & XINPUT_GAMEPAD_B) SendKey(VK_ESCAPE);
}

}  // namespace

// ---------------------------------------------------------------- public
void MenuPad_SetWindow(HWND hwnd) { g_hwnd = hwnd; }

void MenuPad_OnPresent()
{
    if (!g_checked) {
        g_checked = true;
        g_ready = CheckSignatures() && LoadXInput();
        Log("menu pad: %s", g_ready ? "enabled" : "disabled");
    }
    if (!g_ready) return;
    __try {
        Update();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_ready = false;
        Log("menu pad: exception 0x%08lx, feature disabled", GetExceptionCode());
    }
}
