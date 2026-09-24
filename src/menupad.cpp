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

// The MNU library came from the consoles and handles key and pad events in
// every widget, but the PC port switched that off per widget in the page data:
//   button instances (vtables 0x7b7f78, 0x7b7fc0): byte +0x54 = keys enabled
//   lists            (vtable 0x7b7ce0):            bit 0 of +0x60 = keys enabled
// Without it they ignore arrows and Enter (sliders and edit boxes have no such
// switch). We turn it on for the elements of the top page.
const DWORD kButtonVtables[] = { 0x007b7f78, 0x007b7fc0 };
const DWORD kListVtable = 0x007b7ce0;

int EnableKeys(char* page)
{
    int changed = 0;
    char** it = *(char***)(page + 0x28);
    char** end = *(char***)(page + 0x2c);
    for (; it && it < end; it++) {
        char* elem = *it;
        char* widget = elem ? *(char**)(elem + 0x2c) : nullptr;
        if (!widget) continue;
        DWORD vt = *(DWORD*)widget;
        if (vt == kListVtable) {
            if (!(widget[0x60] & 1)) { widget[0x60] |= 1; changed++; }
        } else if (vt == kButtonVtables[0] || vt == kButtonVtables[1]) {
            if (!widget[0x54]) { widget[0x54] = 1; changed++; }
        }
    }
    return changed;
}

// Prologue bytes checked before anything is called, so an unknown executable
// build simply leaves the feature disabled.
struct Signature { DWORD addr; unsigned char bytes[8]; };
const Signature kSignatures[] = {
    { 0x007124c0, { 0x8B, 0x41, 0x10, 0x85, 0xC0, 0x7E, 0x0E, 0x8B } },
    { 0x007124e0, { 0x8B, 0x41, 0x10, 0x85, 0xC0, 0x7E, 0x0E, 0x8B } },
    { 0x00716e60, { 0x56, 0x8B, 0xF1, 0x8B, 0x46, 0x40, 0x85, 0xC0 } },
    { 0x007b7ce0, { 0xC0, 0x6A, 0x71, 0x00, 0x90, 0x6A, 0x71, 0x00 } },  // list vtable
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

void LogFocus(char* page, const char* why)
{
    char* focus = *(char**)(page + 0x40);
    char* widget = focus ? *(char**)(focus + 0x2c) : nullptr;
    int item = widget && *(DWORD*)widget == kListVtable ? *(int*)(widget + 0x30) : -1;  // current list row
    if (page == g_lastPage && focus == g_lastFocus && item == g_lastItem) return;
    g_lastItem = item;
    if (page == g_lastPage && focus == g_lastFocus) { Log("menu pad: %s row %d (%s)", ElementName(focus), item, why); return; }
    if (page != g_lastPage) Log("menu pad: page %p", page);
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
    if (int n = EnableKeys(page)) Log("menu pad: key navigation enabled on %d widgets of page %p", n, page);
    LogFocus(page, "page");
    if (!havePad) return;

    // Direction with auto-repeat.
    DWORD key = DirectionKey(pad);
    DWORD now = GetTickCount();
    bool step = false;
    if (key && key != g_heldKey) { step = true; g_nextRepeat = now + 400; }
    else if (key && (int)(now - g_nextRepeat) >= 0) { step = true; g_nextRepeat = now + 120; }
    g_heldKey = key;

    if (step) { Press(mgr, key); if ((page = TopPage(mgr))) LogFocus(page, "move"); }
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
