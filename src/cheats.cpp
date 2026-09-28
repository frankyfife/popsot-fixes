// Cheats, after the ones of the PS2 prototype (August 2003).
//
// The prototype has a cheat mode written in the game's scripts (Jade AI models
// compiled to C, names from its linker map): bits in a variable of the global
// script object tested by the enemy spawners, and a per-frame function of the
// prince (iCheats) with pad buttons for life up / down and cycling the weapon.
// The PC release keeps only the bit test, so the cheats here are rebuilt on the
// PC version's own data, found with the prototype's symbols as a guide.
//
// Script objects: OBJ_tdst_GameObject +0x18 -> extended data, +4 -> AI
// instance, +0x44 -> the instance's variables. Script references 1 and 2
// stand for the main actors, whose objects are at 0xa99474 / 0xa99478 (the
// prince is main actor 1).
//
// Prince variables (from the PC version of fn_RegenerateLife at 0x4e8772):
// life +0x19c, maximum life +0x11f8 (ints; prototype +0x17c / +0xb48). Sand
// (from the sand cloud pickup at 0x5f24ff and memory dumps taken while
// rewinding): filled tanks +0x7ac, tanks +0xd24 (up to 10), sand in the
// current tank +0xca8, a full tank +0x122c.
//
// One-hit kills: most enemies run the combat model 0x2077, whose
// fn_Combat_ProcessCurrentHurt (PC 0x5ff670, cdecl (object)) applies a hit:
// in hurt state 4 (+0x398) it subtracts the damage (float +0x208) from the
// life (+0x554), and a damage of exactly 1000.0 is the game's own instant
// kill. With the cheat on, we set that damage before it runs (never for the
// main actors, so Farah is not killed by a hit). A few sand creatures (model
// 0x7493) take melee hits in 0x602a60 instead (cdecl (object, message)): their
// life is a hit counter at +0x7c, and a hit that finds it at 0 takes the death
// path, so a hit sent by the prince (message +0) clears it first.
//
// Menu: the game options page (P_GameOptions, also reached from the pause
// menu) still has four rows the PC release hides: labels SetupMenu, Cheats,
// Interface, WorkInProgress, each with On / Off buttons (<name>On, <name>Off).
// When the page is shown (0x4048b0) the labels get the caption " " and the
// buttons are hidden, then 0x404510 refreshes the page. After that refresh we
// caption the first three labels with our cheats, show their buttons and mark
// the active one, all through the page's own helpers (cdecl, by element name):
// 0x403850 caption (a wide string), 0x4032e0 visible, 0x403280 selected. Clicks go to
// 0x404690 (argument: the element, its name at +4); ours are handled there
// with the page's click sound (0x402ab0).
//
// Keys (in the game window): Ctrl+F1 invulnerable, Ctrl+F2 infinite sand,
// Ctrl+F3 one-hit kills, Ctrl+F5 dumps the prince's variables to
// popfix_dump_<n>.bin (for finding further values). One buzz = on, two = off.
// [cheats] in popfix.ini keeps the state.

#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "cheats.h"
#include "gamepad.h"
#include "ui.h"

void Log(const char* fmt, ...);

namespace {

void* const* const g_mainActor1 = (void* const*)0x00a99474;
void* const* const g_mainActor2 = (void* const*)0x00a99478;
const DWORD kCombatHurt = 0x005ff670;
// sub esp, 0xc; mov eax, [esp + 0x10]
const unsigned char kCombatHurtPrologue[] = { 0x83, 0xEC, 0x0C, 0x8B, 0x44, 0x24, 0x10 };
const DWORD kHurtState = 0x398, kHurtDamage = 0x208;
const DWORD kLife = 0x19c, kLifeMax = 0x11f8;
const DWORD kTanks = 0x7ac, kTanksMax = 0xd24, kSand = 0xca8, kSandFull = 0x122c;
const DWORD kEnemyHit = 0x00602a60;
// sub esp, 0x124; mov eax, [0xabc930]
const unsigned char kEnemyHitPrologue[] = { 0x81, 0xEC, 0x24, 0x01, 0x00, 0x00, 0xA1, 0x30, 0xC9, 0xAB, 0x00 };
const DWORD kEnemyHits = 0x7c;

const DWORD kOptionsRefresh = 0x00404510;
// push -1; push 0x7a0e90; mov eax, fs:[0]
const unsigned char kOptionsRefreshPrologue[] = { 0x6A, 0xFF, 0x68, 0x90, 0x0E, 0x7A, 0x00, 0x64, 0xA1, 0x00, 0x00, 0x00, 0x00 };
const DWORD kOptionsClick = 0x00404690;
// push ebx; push esi; push edi; mov edi, [esp + 0x10]
const unsigned char kOptionsClickPrologue[] = { 0x53, 0x56, 0x57, 0x8B, 0x7C, 0x24, 0x10 };
typedef void(__cdecl* ElementSetText_t)(const char* name, const wchar_t* text);
typedef void(__cdecl* ElementSetFlag_t)(const char* name, int on);
typedef int(__cdecl* MenuSound_t)(int sound);
const ElementSetText_t ElementCaption = (ElementSetText_t)0x00403850;
const ElementSetFlag_t ElementVisible = (ElementSetFlag_t)0x004032e0;
const ElementSetFlag_t ElementSelected = (ElementSetFlag_t)0x00403280;
const MenuSound_t MenuSound = (MenuSound_t)0x00402ab0;

char g_ini[MAX_PATH];
char g_dir[MAX_PATH];
bool g_invulnerable, g_infiniteSand, g_oneHitKills;
int g_dumps;

bool Readable(const void* p, size_t n)
{
    MEMORY_BASIC_INFORMATION mi;
    if (!p || !VirtualQuery(p, &mi, sizeof(mi)) || mi.State != MEM_COMMIT) return false;
    if (mi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
    return (BYTE*)p + n <= (BYTE*)mi.BaseAddress + mi.RegionSize;
}

// The AI variables of a script object, or null.
BYTE* Vars(void* obj)
{
    if (!Readable(obj, 0x1c)) return nullptr;
    BYTE* ext = *(BYTE**)((BYTE*)obj + 0x18);
    if (!Readable(ext, 8)) return nullptr;
    BYTE* ai = *(BYTE**)(ext + 4);
    if (!Readable(ai, 0x48)) return nullptr;
    BYTE* vars = *(BYTE**)(ai + 0x44);
    return Readable(vars, 4) ? vars : nullptr;
}

BYTE* PrinceVars()
{
    BYTE* v = Vars(*g_mainActor1);
    return v && Readable(v, kSandFull + 4) ? v : nullptr;
}

struct Keys {
    bool ctrl, f1, f2, f3, f5;
};

Keys ReadKeys(bool keys)
{
    static bool down[4];
    const int vk[4] = { VK_F1, VK_F2, VK_F3, VK_F5 };
    bool p[4];
    for (int i = 0; i < 4; i++) {
        bool now = keys && GetAsyncKeyState(vk[i]) < 0;
        p[i] = now && !down[i];
        down[i] = now;
    }
    return { keys && GetAsyncKeyState(VK_CONTROL) < 0, p[0], p[1], p[2], p[3] };
}

void Set(bool& flag, bool on, const char* key, const char* name)
{
    flag = on;
    WritePrivateProfileStringA("cheats", key, flag ? "1" : "0", g_ini);
    Log("cheats: %s %s", name, flag ? "on" : "off");
}

void Toggle(bool& flag, const char* key, const char* name)
{
    Set(flag, !flag, key, name);
    Gamepad_Pulse(flag ? 1 : 2);
}

// The game options rows we use.
struct Row {
    const char* label;
    const char* on;
    const char* off;
    bool* flag;
    const char* key;
    const char* name;
    const wchar_t* german;
    const wchar_t* english;
};
const Row kRows[] = {
    { "SetupMenu", "SetupMenuOn", "SetupMenuOff", &g_invulnerable, "invulnerable", "invulnerable", L"Unverwundbar",
      L"Invulnerable" },
    { "Cheats", "CheatsOn", "CheatsOff", &g_infiniteSand, "infinite_sand", "infinite sand", L"Unendlich Sand",
      L"Infinite sand" },
    { "Interface", "InterfaceOn", "InterfaceOff", &g_oneHitKills, "one_hit_kills", "one-hit kills",
      L"Ein Treffer reicht", L"One-hit kills" },
};
bool g_menu;  // menu hooks installed

// An element of the current menu page (0x712440 on the menu manager gives the
// page, 0x712240 searches its elements by name, as the page's helpers do).
BYTE* Element(const char* name)
{
    BYTE* mgr = *(BYTE**)0x0080bbd8;
    if (!Readable(mgr, 0x20)) return nullptr;
    mgr += 0xc;
    BYTE* page = nullptr;
    __asm {
        mov ecx, mgr
        push 0
        mov eax, 0x00712440
        call eax
        mov page, eax
    }
    if (!Readable(page, 0x30)) return nullptr;
    return ((BYTE*(__cdecl*)(BYTE*, const char*))0x00712240)(page, name);
}

// The dark panel behind the rows (BlackRectangle, virtual 640x480 lines 130 to
// 286) ends below the tutorials row. Its rect is set again from the layout
// every frame before drawing, so the quad itself is moved while the game
// options page is shown. Its texture has frayed, transparent margins (about
// 13.5 % of the height at the top, 8.5 % at the bottom): lines 109 to 419 keep
// the visible top at line 151 as before and end 25 lines below our last row
// (368), like below the tutorials row.
void StretchPanel()
{
    Ui_MoveQuadRows(Element("SetupMenuOn") != nullptr, 130, 286, 109, 419);
}

void ShowRows()
{
    bool german = PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_GERMAN;
    for (const Row& r : kRows) {
        ElementCaption(r.label, german ? r.german : r.english);
        ElementVisible(r.on, 1);
        ElementVisible(r.off, 1);
        ElementSelected(r.on, *r.flag);
        ElementSelected(r.off, !*r.flag);
    }
}

void Dump()
{
    BYTE* v = PrinceVars();
    if (!v) { Log("cheats: dump - no prince"); return; }
    size_t n = 0x2000;
    while (n > 0x1200 && !Readable(v, n)) n -= 0x100;
    char path[MAX_PATH];
    _snprintf(path, MAX_PATH, "%spopfix_dump_%d.bin", g_dir, ++g_dumps);
    path[MAX_PATH - 1] = 0;
    FILE* f = fopen(path, "wb");
    if (f) {
        fwrite(v, 1, n, f);
        fclose(f);
    }
    Log("cheats: dump %d (%u bytes of the prince's variables at %p), life %d of %d", g_dumps, (unsigned)n, v,
        *(int*)(v + kLife), *(int*)(v + kLifeMax));
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

extern "C" {
void* g_cheatEnemyHit;  // trampolines
void* g_cheatCombatHurt;
void* g_cheatOptionsRefresh;
void* g_cheatOptionsClick;

void __cdecl CheatOptionsRefresh()
{
    ((void(__cdecl*)())g_cheatOptionsRefresh)();
    ShowRows();
}

// True if the clicked element is one of our buttons (then handled here).
int __cdecl CheatOptionsClick(const BYTE* element)
{
    if (!Readable(element, 0x24)) return 0;
    const char* name = (const char*)element + 4;
    for (const Row& r : kRows) {
        bool on = strcmp(name, r.on) == 0;
        if (!on && strcmp(name, r.off) != 0) continue;
        MenuSound(1);
        if (*r.flag != on) Set(*r.flag, on, r.key, r.name);
        ElementSelected(r.on, on);
        ElementSelected(r.off, !on);
        return 1;
    }
    return 0;
}

void __cdecl CheatCombatHurt(void* enemy)
{
    if (!g_oneHitKills || enemy == *g_mainActor1 || enemy == *g_mainActor2) return;
    BYTE* v = Vars(enemy);
    if (v && Readable(v, kHurtState + 4) && *(int*)(v + kHurtState) == 4) {
        static int logged;
        if (logged < 5) {
            logged++;
            Log("cheats: one-hit kill of %p (damage %.2f)", enemy, *(float*)(v + kHurtDamage));
        }
        *(float*)(v + kHurtDamage) = 1000.0f;
    }
}

void __cdecl CheatEnemyHit(void* enemy, void** msg)
{
    if (!g_oneHitKills || !Readable(msg, 4)) return;
    void* sender = msg[0];
    if (sender != (void*)1 && sender != *g_mainActor1) return;
    if (BYTE* v = Vars(enemy)) {
        if (Readable(v, kEnemyHits + 4) && *(int*)(v + kEnemyHits) > 0) *(int*)(v + kEnemyHits) = 0;
    }
}
}

extern "C" __declspec(naked) void CheatEnemyHitHook()
{
    __asm {
        push dword ptr [esp + 8]  // message
        push dword ptr [esp + 8]  // enemy (the first push moved it by 4)
        call CheatEnemyHit
        add esp, 8
        jmp dword ptr [g_cheatEnemyHit]
    }
}

extern "C" __declspec(naked) void CheatOptionsClickHook()
{
    __asm {
        push ecx
        push edx
        push dword ptr [esp + 12]  // element
        call CheatOptionsClick
        add esp, 4
        pop edx
        pop ecx
        test eax, eax
        jz original
        mov al, 1
        ret 4
    original:
        jmp dword ptr [g_cheatOptionsClick]
    }
}

extern "C" __declspec(naked) void CheatCombatHurtHook()
{
    __asm {
        push dword ptr [esp + 4]  // enemy
        call CheatCombatHurt
        add esp, 4
        jmp dword ptr [g_cheatCombatHurt]
    }
}

void Cheats_Install(const char* iniPath)
{
    strncpy(g_ini, iniPath, MAX_PATH - 1);
    strncpy(g_dir, iniPath, MAX_PATH - 1);
    char* slash = strrchr(g_dir, '\\');
    if (slash) slash[1] = 0;
    else g_dir[0] = 0;
    g_invulnerable = GetPrivateProfileIntA("cheats", "invulnerable", 0, g_ini) != 0;
    g_infiniteSand = GetPrivateProfileIntA("cheats", "infinite_sand", 0, g_ini) != 0;
    g_oneHitKills = GetPrivateProfileIntA("cheats", "one_hit_kills", 0, g_ini) != 0;
    g_cheatEnemyHit = Detour(kEnemyHit, kEnemyHitPrologue, sizeof(kEnemyHitPrologue), (void*)CheatEnemyHitHook);
    g_cheatCombatHurt = Detour(kCombatHurt, kCombatHurtPrologue, sizeof(kCombatHurtPrologue), (void*)CheatCombatHurtHook);
    if (memcmp((void*)kOptionsClick, kOptionsClickPrologue, sizeof(kOptionsClickPrologue)) == 0) {
        g_cheatOptionsRefresh = Detour(kOptionsRefresh, kOptionsRefreshPrologue, sizeof(kOptionsRefreshPrologue),
                                       (void*)CheatOptionsRefresh);
        if (g_cheatOptionsRefresh)
            g_cheatOptionsClick = Detour(kOptionsClick, kOptionsClickPrologue, sizeof(kOptionsClickPrologue),
                                         (void*)CheatOptionsClickHook);
    }
    g_menu = g_cheatOptionsRefresh && g_cheatOptionsClick;
    Log("cheats: game options rows %s", g_menu ? "installed" : "not installed (unknown executable)");
    Log("cheats: installed (invulnerable %d, infinite sand %d, one-hit kills %d%s; Ctrl+F1 / F2 / F3)",
        g_invulnerable, g_infiniteSand, g_oneHitKills,
        g_cheatEnemyHit && g_cheatCombatHurt ? "" : " - not available, unknown executable");
}

void Cheats_OnPresent(bool keys)
{
    if (g_menu) StretchPanel();
    Keys k = ReadKeys(keys);
    if (k.ctrl && k.f1) Toggle(g_invulnerable, "invulnerable", "invulnerable");
    if (k.ctrl && k.f2) Toggle(g_infiniteSand, "infinite_sand", "infinite sand");
    if (k.ctrl && k.f3 && g_cheatEnemyHit && g_cheatCombatHurt) Toggle(g_oneHitKills, "one_hit_kills", "one-hit kills");
    if (k.ctrl && k.f5) Dump();

    if (BYTE* p = PrinceVars()) {
        int& life = *(int*)(p + kLife);
        int max = *(int*)(p + kLifeMax);
        if (g_invulnerable && max > 0 && max < 100000 && life > 0 && life < max) life = max;
        int& tanks = *(int*)(p + kTanks);
        int tanksMax = *(int*)(p + kTanksMax);
        if (g_infiniteSand && tanksMax > 0 && tanksMax <= 10 && tanks >= 0 && tanks <= tanksMax) {
            tanks = tanksMax;
            *(int*)(p + kSand) = *(int*)(p + kSandFull);
        }
    }
}
