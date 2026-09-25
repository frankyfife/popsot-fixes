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
//   A                  -> Enter (on a list: double click on the current row)
//   B, Y               -> Escape (back)
// Everything runs on the render thread (called from Present), like the game's
// own input processing.

#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <xinput.h>
#include <stdlib.h>
#include <string.h>
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
typedef void(__thiscall* MgrMouseButton_t)(void* mgr, const DWORD* event);  // {buttons, packed position}
const MgrMouseButton_t MgrDoubleClick = (MgrMouseButton_t)0x00712560;
DWORD g_cursorSet = 0xFFFFFFFF;  // last cursor position we set (mouse untouched while equal)

// Shown (+0x24, cleared by the pages to hide an element, see 0x4032e0), enabled
// (+100 bit 1) and of a focusable widget type (types as in 0x711e40).
bool Focusable(char* elem)
{
    char* w = elem ? *(char**)(elem + 0x2c) : nullptr;
    if (!w || !elem[0x24] || !(elem[100] & 2)) return false;
    int type = ((WidgetType_t)(*(DWORD**)w)[3])(w);
    return type == 1 || type == 2 || type == 3 || type == 7 || type == 8 || type == 10;
}

bool Rect(char* elem, short* r)  // x0, x1, y0, y1
{
    char* w = *(char**)(elem + 0x2c);
    r[0] = r[1] = r[2] = r[3] = 0;
    ((WidgetRect_t)(*(DWORD**)w)[4])(w, r);
    return r[1] > r[0] && r[3] > r[2];
}

bool Center(char* elem, int& x, int& y)
{
    short r[4];
    if (!Rect(elem, r)) return false;
    x = (r[0] + r[1]) / 2;
    y = (r[2] + r[3]) / 2;
    return true;
}

// Distance between the ranges [a0, a1] and [b0, b1], 0 if they overlap.
int Gap(int a0, int a1, int b0, int b1)
{
    if (b0 > a1) return b0 - a1;
    if (a0 > b1) return a0 - b1;
    return 0;
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
// Nearest focusable element in direction vk. "Across" is the gap between the
// element rectangles, not between their centres: menu entries are left-aligned
// texts of different widths, and a long entry's centre lies far to the side.
char* Neighbour(char* page, DWORD vk)
{
    char* from = *(char**)(page + 0x40);
    short fr[4] = { 320, 320, 240, 240 };
    if (from) Rect(from, fr);
    int fx = (fr[0] + fr[1]) / 2, fy = (fr[2] + fr[3]) / 2;
    char* best = nullptr;
    double bestCost = 1e30;
    for (char** it = *(char***)(page + 0x28); it && it < *(char***)(page + 0x2c); it++) {
        char* e = *it;
        short r[4];
        if (e == from || !Focusable(e) || !Rect(e, r)) continue;
        int dx = (r[0] + r[1]) / 2 - fx, dy = (r[2] + r[3]) / 2 - fy, along, across;
        switch (vk) {
        case VK_UP: along = -dy; across = Gap(fr[0], fr[1], r[0], r[1]); break;
        case VK_DOWN: along = dy; across = Gap(fr[0], fr[1], r[0], r[1]); break;
        case VK_LEFT: along = -dx; across = Gap(fr[2], fr[3], r[2], r[3]); break;
        default: along = dx; across = Gap(fr[2], fr[3], r[2], r[3]); break;
        }
        if (along <= 0) continue;
        double cost = along + 2.0 * across;
        if (cost < bestCost) { bestCost = cost; best = e; }
    }
    return best;
}

int ListRow(char* elem)
{
    char* w = elem ? *(char**)(elem + 0x2c) : nullptr;
    return w && *(DWORD*)w == kListVtable ? *(int*)(w + 0x30) : -1;
}

typedef int(__thiscall* ListHitTest_t)(void* list, const short* pos);  // row index or -1
const ListHitTest_t ListHitTest = (ListHitTest_t)0x00714e70;

// Position of a list row, found with the list's own hit test (the one the page
// uses for clicks). Scrolls the row into view first if needed.
bool ListRowPoint(char* w, int row, int& x, int& y)
{
    int visible = *(int*)(w + 0x7c);
    int& first = *(int*)(w + 0x80);
    if (visible > 0) {
        if (row < first) first = row;
        else if (row >= first + visible) first = row - visible + 1;
    }
    const short* rows = (const short*)(w + 0x6c);  // x0, x1, y0, y1
    short pos[2] = { (short)((rows[0] + rows[1]) / 2), 0 };
    int top = -1, bottom = -1;
    for (int yy = rows[2]; yy <= rows[3]; yy++) {
        pos[1] = (short)yy;
        if (ListHitTest(w, pos) == row) { if (top < 0) top = yy; bottom = yy; }
        else if (top >= 0) break;
    }
    if (top < 0) return false;
    x = pos[0];
    y = (top + bottom) / 2;
    return true;
}

typedef char*(__thiscall* ElementAt_t)(void* page, const short* pos);
const ElementAt_t ElementAt = (ElementAt_t)0x00716d90;  // what the mouse would hover

// A point inside the element's rect where the page's own hover test finds this
// element (rects overlap, e.g. text boxes wider than their text).
bool HoverPoint(char* page, char* elem, int& x, int& y)
{
    char* w = *(char**)(elem + 0x2c);
    short r[4] = { 0, 0, 0, 0 };
    ((WidgetRect_t)(*(DWORD**)w)[4])(w, r);
    if (r[1] <= r[0] || r[3] <= r[2]) return false;
    static const int fx[] = { 50, 30, 70, 15, 85, 5, 95 }, fy[] = { 50, 30, 70, 15, 85 };
    for (int j : fy)
        for (int i : fx) {
            short pos[2] = { (short)(r[0] + (r[1] - r[0]) * i / 100), (short)(r[2] + (r[3] - r[2]) * j / 100) };
            if (ElementAt(page, pos) == elem) { x = pos[0]; y = pos[1]; return true; }
        }
    return false;
}

// Put the (virtual) mouse cursor on the focused element - or on the current row
// of a list - so the game shows its normal hover highlight there.
void PointAt(void* mgr, char* page, char* elem)
{
    if (!elem) return;
    int x, y;
    if (!Center(elem, x, y)) return;
    HoverPoint(page, elem, x, y);
    char* w = *(char**)(elem + 0x2c);
    if (*(DWORD*)w == kListVtable) {
        int row = *(int*)(w + 0x30);
        if (!ListRowPoint(w, row, x, y)) {
            static int logged;
            if (logged++ < 5)
                Log("menu pad: list row %d not found (rows %d..%d x %d..%d, visible %d, first %d)", row,
                    ((short*)(w + 0x6c))[2], ((short*)(w + 0x6c))[3], ((short*)(w + 0x6c))[0],
                    ((short*)(w + 0x6c))[1], *(int*)(w + 0x7c), *(int*)(w + 0x80));
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
    { 0x00712560, { 0x8B, 0x54, 0x24, 0x04, 0x8B, 0x42, 0x04, 0x89 } },
    { 0x00714e70, { 0x83, 0xEC, 0x0C, 0x53, 0x55, 0x56, 0x8B, 0xF1 } },
    { 0x00716d90, { 0x83, 0xEC, 0x0C, 0x53, 0x55, 0x56, 0x57, 0x8B } },
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
        Log("  [%d] %p \"%.24s\" elem vt %08lx shown %d flags %02x | widget %p vt %08lx type %d +54 %02x +60 %02x | nb %p %p %p %p",
            i, e, e + 4, *(DWORD*)e, e[0x24], (unsigned char)e[100], w, wvt, type, w ? (unsigned char)w[0x54] : 0,
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
        PointAt(mgr, page, *(char**)(page + 0x40));
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
            PointAt(mgr, page, *(char**)(page + 0x40));
            LogFocus(page, "move");
        }
    }
    if (pressed & XINPUT_GAMEPAD_A) {
        // Lists: Enter only marks the row; the pages act on a double click on
        // it (profile list, save lists), exactly as with the mouse.
        char* focus = *(char**)(page + 0x40);
        if (ListRow(focus) >= 0) {
            PointAt(mgr, page, focus);
            DWORD ev[2] = { 1, g_cursorSet };
            Log("menu pad: double click on %s row %d at %d,%d", ElementName(focus), ListRow(focus),
                (short)(g_cursorSet & 0xffff), (short)(g_cursorSet >> 16));
            MgrDoubleClick(mgr, ev);
        } else {
            Press(mgr, VK_RETURN);
        }
        if ((page = TopPage(mgr))) LogFocus(page, "A");
    }
    if (pressed & (XINPUT_GAMEPAD_B | XINPUT_GAMEPAD_Y)) { Press(mgr, VK_ESCAPE); if ((page = TopPage(mgr))) LogFocus(page, "back"); }
}

// ---------------------------------------------------------------- level select
// The PC build still has the level select of the console versions as a page
// (P_SpecialLoad, index 15: pages of 15 levels, Prev15/Next15, handler 0x40f180),
// and the main menu still has its "SpecialLoad" button - but the main menu hides
// it every time it opens (0x4094fb pushes 0 for "shown") and its click handler
// (0x409180) has no case for it. Show the button and open the page on click.
const DWORD kSpecialLoadShown = 0x004094fb;  // push 0 -> push 1
const unsigned char kSpecialLoadShownBytes[] = { 0x6A, 0x00, 0x68, 0xF8, 0x60, 0x7A, 0x00 };
const DWORD kMainMenuClick = 0x00409180;     // thiscall bool(handler, element)
const unsigned char kMainMenuClickPrologue[] = { 0x53, 0x56, 0x57, 0x8B, 0x7C, 0x24, 0x10 };
typedef int(__thiscall* Click_t)(void* handler, char* elem);
typedef void(__thiscall* OpenPcPage_t)(void* pcMenu, int page, char a, int b);
const OpenPcPage_t OpenPcPage = (OpenPcPage_t)0x00409870;
Click_t g_mainMenuClick;

int __fastcall MainMenuClick(void* handler, void* /*edx*/, char* elem)
{
    if (elem && _stricmp(elem + 4, "SpecialLoad") == 0) {
        Log("menu pad: level select opened");
        OpenPcPage(*(void**)0x0080bc44, 15, 0, 1);  // P_SpecialLoad, as "LoadGame" opens P_LoadSavedGame
        return 1;
    }
    return g_mainMenuClick(handler, elem);
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

}  // namespace

void MenuPad_Install()
{
    static bool done;
    if (done) return;
    done = true;
    if (memcmp((void*)kSpecialLoadShown, kSpecialLoadShownBytes, sizeof(kSpecialLoadShownBytes)) != 0 ||
        memcmp((void*)kMainMenuClick, kMainMenuClickPrologue, sizeof(kMainMenuClickPrologue)) != 0) {
        Log("menu pad: unknown executable, level select not enabled");
        return;
    }
    g_mainMenuClick = (Click_t)Detour(kMainMenuClick, kMainMenuClickPrologue, sizeof(kMainMenuClickPrologue),
                                      (void*)MainMenuClick);
    if (!g_mainMenuClick) return;
    DWORD prot;
    unsigned char* p = (unsigned char*)kSpecialLoadShown;
    VirtualProtect(p, 2, PAGE_EXECUTE_READWRITE, &prot);
    p[1] = 1;
    VirtualProtect(p, 2, prot, &prot);
    FlushInstructionCache(GetCurrentProcess(), p, 2);
    Log("menu pad: level select button enabled in the main menu");
}

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
