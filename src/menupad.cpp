// Controller navigation for the PC front-end menus.
//
// The PC menus (PCMenuManager, MNU_* pages) already support keyboard
// navigation: every element has neighbour links for up/down/left/right
// (element +0x54..+0x60, resolved from the page data by 0x7122b0), the page
// keeps a focused element (+0x40), and the focused widget handles key events
// itself (sliders change their value with left/right, lists scroll, buttons
// activate on Enter). The DirectInput keyboard poll feeds these key events to
// the UI manager (0x421ef0 -> 0x7124c0 key down / 0x7124e0 key up).
//
// We feed the same key events from the controller:
//   D-pad / left stick -> arrow keys (with auto-repeat)
//   A                  -> Enter
//   B, Y               -> Escape (back)
// Everything runs on the render thread (called from Present), like the game's
// own input processing.

#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <xinput.h>
#include <stdlib.h>
#include "menupad.h"

void Log(const char* fmt, ...);

namespace {

// ---------------------------------------------------------------- game interface (POP.EXE v181 / gpp.exe)
const DWORD kMenuRootPtr = 0x0080bbd8;  // PCMenuManager*; UI manager lives at +0xc

struct KeyEvent {
    DWORD vk;     // virtual key code
    DWORD ch;     // character (text entry), 0 for control keys
};
typedef void(__thiscall* MgrKey_t)(void* mgr, const KeyEvent* ev);
const MgrKey_t MgrKeyDown = (MgrKey_t)0x007124c0;
const MgrKey_t MgrKeyUp = (MgrKey_t)0x007124e0;

// The MNU library came from the consoles and handles key and pad events in its
// widgets, but the PC page data leaves parts of it unusable:
//   - the neighbour links are empty on most pages, so the focus cannot move
//     between elements with the arrow keys;
//   - after every key event the page gives the focus to the element under the
//     mouse cursor (0x716c50), so a resting cursor steals it.
// So the pad lets the focused widget handle a direction first (list rows,
// slider values) and, if nothing changed, moves the focus itself to the nearest
// element in that direction through the elements' own focus/unfocus methods.
// The hover highlight (gold -> white text) follows the mouse cursor, not the
// focus, so the cursor is then put on the focused element / list row.
const DWORD kListVtable = 0x007b7ce0;    // MNU_List widget
const DWORD kSliderVtable = 0x007b7d90;  // MNU_Slider widget
typedef int(__thiscall* WidgetType_t)(void* widget);
typedef void(__thiscall* WidgetRect_t)(void* widget, short* rect);  // x0, x1, y0, y1
typedef void(__thiscall* ElemFn_t)(void* elem);
typedef void(__thiscall* MgrMouseMove_t)(void* mgr, const DWORD* packedPos);
const MgrMouseMove_t MgrMouseMove = (MgrMouseMove_t)0x007125f0;
DWORD g_cursorSet = 0xFFFFFFFF;  // last cursor position we set (mouse untouched while equal)

// Visible, enabled and of a focusable widget type (types as in 0x711e40).
bool Focusable(char* elem)
{
    char* w = elem ? *(char**)(elem + 0x2c) : nullptr;
    if (!w || !(elem[100] & 2)) return false;
    int type = ((WidgetType_t)(*(DWORD**)w)[3])(w);
    return type == 1 || type == 2 || type == 3 || type == 7 || type == 8 || type == 10;
}

bool Center(char* elem, int& x, int& y)
{
    char* w = *(char**)(elem + 0x2c);
    short r[4] = { 0, 0, 0, 0 };
    ((WidgetRect_t)(*(DWORD**)w)[4])(w, r);
    if (r[1] <= r[0] || r[3] <= r[2]) return false;
    x = (r[0] + r[1]) / 2;
    y = (r[2] + r[3]) / 2;
    return true;
}

void SetFocus(char* page, char* elem)
{
    char* old = *(char**)(page + 0x40);
    if (old == elem) return;
    if (old) ((ElemFn_t)(*(DWORD**)old)[4])(old);  // lose focus
    *(char**)(page + 0x40) = elem;
    ((ElemFn_t)(*(DWORD**)elem)[3])(elem);          // gain focus
}

// Nearest focusable element from the focused one in a direction (VK arrow).
char* Neighbour(char* page, DWORD vk)
{
    char* from = *(char**)(page + 0x40);
    int fx = 320, fy = 240;
    if (from) Center(from, fx, fy);
    char* best = nullptr;
    double bestCost = 1e30;
    for (char** it = *(char***)(page + 0x28); it && it < *(char***)(page + 0x2c); it++) {
        char* e = *it;
        int x, y;
        if (e == from || !Focusable(e) || !Center(e, x, y)) continue;
        int dx = x - fx, dy = y - fy, along, across;
        switch (vk) {
        case VK_UP: along = -dy; across = dx; break;
        case VK_DOWN: along = dy; across = dx; break;
        case VK_LEFT: along = -dx; across = dy; break;
        default: along = dx; across = dy; break;
        }
        if (along <= 0) continue;
        double cost = along + 2.0 * (across < 0 ? -across : across);
        if (cost < bestCost) { bestCost = cost; best = e; }
    }
    return best;
}

int ListRow(char* elem)
{
    char* w = elem ? *(char**)(elem + 0x2c) : nullptr;
    return w && *(DWORD*)w == kListVtable ? *(int*)(w + 0x30) : -1;
}

// Put the (virtual) mouse cursor on the focused element - or on the current row
// of a list - so the game shows its normal hover highlight there.
void PointAt(void* mgr, char* elem)
{
    if (!elem) return;
    int x, y;
    if (!Center(elem, x, y)) return;
    char* w = *(char**)(elem + 0x2c);
    if (*(DWORD*)w == kListVtable) {
        const short* rows = (const short*)(w + 0x6c);  // rows rect x0, x1, y0, y1
        int visible = *(int*)(w + 0x7c), first = *(int*)(w + 0x80), cur = *(int*)(w + 0x30);
        if (visible > 0 && cur >= first && cur < first + visible) {
            int h = (rows[3] - rows[2] + 1) / visible;
            x = (rows[0] + rows[1]) / 2;
            y = rows[2] + (cur - first) * h + h / 2;
        }
    }
    DWORD pos = (DWORD)(WORD)(short)x | ((DWORD)(WORD)(short)y << 16);
    MgrMouseMove(mgr, &pos);
    g_cursorSet = pos;
}

bool IsSlider(char* elem)
{
    char* w = elem ? *(char**)(elem + 0x2c) : nullptr;
    return w && *(DWORD*)w == kSliderVtable;
}

// Prologue bytes checked before anything is called, so an unknown executable
// build simply leaves the feature disabled.
struct Signature { DWORD addr; unsigned char bytes[8]; };
const Signature kSignatures[] = {
    { 0x007124c0, { 0x8B, 0x41, 0x10, 0x85, 0xC0, 0x7E, 0x0E, 0x8B } },
    { 0x007124e0, { 0x8B, 0x41, 0x10, 0x85, 0xC0, 0x7E, 0x0E, 0x8B } },
    { 0x00716e60, { 0x56, 0x8B, 0xF1, 0x8B, 0x46, 0x40, 0x85, 0xC0 } },
    { 0x007b7ce0, { 0xC0, 0x6A, 0x71, 0x00, 0x90, 0x6A, 0x71, 0x00 } },  // list vtable
    { 0x007b7d90, { 0xA0, 0x89, 0x71, 0x00, 0x30, 0x88, 0x71, 0x00 } },  // slider vtable
    { 0x007125f0, { 0x83, 0xEC, 0x08, 0x8B, 0x44, 0x24, 0x0C, 0x8B } },
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
            if (pXInputGetState) return true;
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

// ---------------------------------------------------------------- state
bool g_ready, g_checked;
HWND g_hwnd;
WORD g_prevButtons;
DWORD g_heldKey;      // arrow key currently held (auto-repeat)
DWORD g_nextRepeat;
void* g_lastPage;
void* g_lastFocus;

bool CheckSignatures()
{
    for (const Signature& s : kSignatures) {
        if (memcmp((const void*)s.addr, s.bytes, sizeof(s.bytes)) != 0) {
            Log("menu pad: unexpected code at %08lx, feature disabled", s.addr);
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

// Top page of the UI manager's page stack, or null when no PC menu is open.
char* TopPage(void* mgr)
{
    int count = *(int*)((char*)mgr + 0x10);
    if (count < 1) return nullptr;
    char* entries = *(char**)((char*)mgr + 4);
    return entries ? *(char**)(entries + (count - 1) * 8) : nullptr;
}

const char* ElementName(char* elem) { return elem ? elem + 4 : "(none)"; }

void Press(void* mgr, DWORD vk)
{
    static int logged;
    if (logged < 60) { logged++; Log("menu pad: key %02lx", vk); }
    KeyEvent ev = { vk, 0 };
    MgrKeyDown(mgr, &ev);
    // The page may have closed or changed on key down; the key-up goes to
    // whatever is on top now, exactly as with a real key.
    if (*(int*)((char*)mgr + 0x10) > 0) MgrKeyUp(mgr, &ev);
}

DWORD DirectionKey(const XINPUT_GAMEPAD& p)
{
    if (p.wButtons & XINPUT_GAMEPAD_DPAD_UP) return VK_UP;
    if (p.wButtons & XINPUT_GAMEPAD_DPAD_DOWN) return VK_DOWN;
    if (p.wButtons & XINPUT_GAMEPAD_DPAD_LEFT) return VK_LEFT;
    if (p.wButtons & XINPUT_GAMEPAD_DPAD_RIGHT) return VK_RIGHT;
    const int dz = 16000;
    int x = p.sThumbLX, y = p.sThumbLY;
    if (abs(x) < dz && abs(y) < dz) return 0;
    if (abs(y) >= abs(x)) return y > 0 ? VK_UP : VK_DOWN;
    return x < 0 ? VK_LEFT : VK_RIGHT;
}

int g_lastItem = -2;

// Diagnostics: every element of a page with its widget type and flags.
void DumpPage(char* page)
{
    char** it = *(char***)(page + 0x28);
    char** end = *(char***)(page + 0x2c);
    Log("menu pad: page %p vt %08lx, %d elements, focus %p", page, *(DWORD*)page, it && end > it ? (int)(end - it) : 0,
        *(char**)(page + 0x40));
    for (int i = 0; it && it < end && i < 64; it++, i++) {
        char* e = *it;
        if (!e) continue;
        char* w = *(char**)(e + 0x2c);
        DWORD wvt = w ? *(DWORD*)w : 0;
        typedef int(__thiscall* Type_t)(void*);
        int type = w ? ((Type_t)(*(DWORD**)w)[3])(w) : -1;
        Log("  [%d] %p \"%.24s\" elem vt %08lx flags %02x | widget %p vt %08lx type %d +54 %02x +60 %02x | nb %p %p %p %p",
            i, e, e + 4, *(DWORD*)e, (unsigned char)e[100], w, wvt, type, w ? (unsigned char)w[0x54] : 0,
            w ? (unsigned char)w[0x60] : 0, *(void**)(e + 0x54), *(void**)(e + 0x58), *(void**)(e + 0x5c),
            *(void**)(e + 0x60));
    }
}

void LogFocus(char* page, const char* why)
{
    char* focus = *(char**)(page + 0x40);
    char* widget = focus ? *(char**)(focus + 0x2c) : nullptr;
    int item = widget && *(DWORD*)widget == kListVtable ? *(int*)(widget + 0x30) : -1;  // current list row
    if (page == g_lastPage && focus == g_lastFocus && item == g_lastItem) return;
    g_lastItem = item;
    if (page == g_lastPage && focus == g_lastFocus) { Log("menu pad: %s row %d (%s)", ElementName(focus), item, why); return; }
    if (page != g_lastPage) DumpPage(page);
    Log("menu pad: focus %s (%s)", ElementName(focus), why);
    g_lastPage = page;
    g_lastFocus = focus;
}

void Update()
{
    void* mgr = UiManager();
    if (!mgr) return;

    XINPUT_GAMEPAD pad;
    bool havePad = ReadPad(pad);
    WORD buttons = havePad ? pad.wButtons : 0;
    WORD pressed = buttons & ~g_prevButtons;
    g_prevButtons = buttons;

    char* page = TopPage(mgr);
    if (!page) { g_lastPage = nullptr; g_heldKey = 0; return; }
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    if (pid != GetCurrentProcessId()) return;  // game not in the foreground
    // New page while the pad is in use (the cursor is still where we put it):
    // highlight its focused element right away.
    if (page != g_lastPage && *(DWORD*)((char*)mgr + 0x1a) == g_cursorSet)
        PointAt(mgr, *(char**)(page + 0x40));
    LogFocus(page, "page");
    if (!havePad) return;

    // Direction with auto-repeat.
    DWORD key = DirectionKey(pad);
    DWORD now = GetTickCount();
    bool step = false;
    if (key && key != g_heldKey) { step = true; g_nextRepeat = now + 400; }
    else if (key && (int)(now - g_nextRepeat) >= 0) { step = true; g_nextRepeat = now + 120; }
    g_heldKey = key;

    bool any = step || (pressed & (XINPUT_GAMEPAD_A | XINPUT_GAMEPAD_B | XINPUT_GAMEPAD_Y));
    if (any && !*(char**)(page + 0x40)) {
        char* first = Neighbour(page, VK_DOWN);
        if (first) SetFocus(page, first);
    }

    if (step) {
        char* focus = *(char**)(page + 0x40);
        int row = ListRow(focus);
        Press(mgr, key);
        page = TopPage(mgr);
        bool sliderValue = IsSlider(focus) && (key == VK_LEFT || key == VK_RIGHT);
        if (page && *(char**)(page + 0x40) == focus && ListRow(focus) == row && !sliderValue) {
            if (char* next = Neighbour(page, key)) SetFocus(page, next);
        }
        if (page) {
            PointAt(mgr, *(char**)(page + 0x40));
            LogFocus(page, "move");
        }
    }
    if (pressed & XINPUT_GAMEPAD_A) { Press(mgr, VK_RETURN); if ((page = TopPage(mgr))) LogFocus(page, "A"); }
    if (pressed & (XINPUT_GAMEPAD_B | XINPUT_GAMEPAD_Y)) { Press(mgr, VK_ESCAPE); if ((page = TopPage(mgr))) LogFocus(page, "back"); }
}

}  // namespace

// ---------------------------------------------------------------- public
void MenuPad_SetWindow(HWND hwnd) { g_hwnd = hwnd; }

void MenuPad_OnPresent()
{
    if (!g_checked) {
        g_checked = true;
        g_ready = CheckSignatures() && LoadXInput();
        Log("menu pad: %s", g_ready ? "enabled (controller -> menu key events)" : "disabled");
    }
    if (!g_ready) return;
    __try {
        Update();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_ready = false;
        Log("menu pad: exception 0x%08lx, feature disabled", GetExceptionCode());
    }
}
