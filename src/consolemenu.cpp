// Console (Xbox) front-end menus.
//
// The PC build still contains the console menus: they are Jade AI scripts
// (compiled to C) that build fading text pages, driven by the engine's menu
// manager. The PC port disabled them in four places:
//
// 1. The engine's "open menu" routine (0x467b70) activates the console menu
//    and then unconditionally calls a PC hook (0x402760) that pushes one of the
//    mouse-only PCMenuManager pages on top of it. We turn that hook into a no-op.
//
// 2. The per-frame menu manager tick was cut down. On Xbox (0x17d10) it
//      - lets every menu control (input action, manager +0x14..+0x18) poll its
//        input; the first one that fires consumes the frame,
//      - closes the top page once it has finished fading out (state 4),
//      - ticks the top page and the menu animations.
//    The PC version (0x672310) only kept the last two steps, so the console
//    menus never see any input. We replace it with the Xbox logic, built from
//    the functions that are still present in the PC executable.
//
// 3. The pad state builder drops all pad input while a PC front-end flag is
//    set. We keep the input. (The controller itself is fed in by gamepad.cpp.)
//
// 4. The element show/hide function (0x69a840) was reduced to "hide only", so
//    console menu elements could never appear. We restore the Xbox behaviour.

#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <string.h>
#include "consolemenu.h"

void Log(const char* fmt, ...);

namespace {

const DWORD kPcMenuRedirect = 0x00402760;  // cdecl void(int consoleMenuIndex)
const unsigned char kPcMenuRedirectPrologue[] = { 0x8B, 0x0D, 0x44, 0xBC, 0x80, 0x00, 0x56 };

// The per-frame pad state builder (0x41fa10) clears all pad input while a PC
// front-end flag (*(0x80bc44) + 0x1a) is set, so the console menu controls
// never see a button press. Make that branch unconditional ("je" -> "jmp").
const DWORD kPadClearBranch = 0x0041fcdb;
const unsigned char kPadClearBranchBytes[] = { 0x74, 0x0D };

const DWORD kMenuTick = 0x00672310;  // thiscall bool(MenuManager*)
const unsigned char kMenuTickPrologue[] = { 0x56, 0x8B, 0xF1, 0x8B, 0x86, 0xF0, 0x00, 0x00, 0x00 };

typedef void(__thiscall* PageFn_t)(void* page);
typedef int(__thiscall* ControlProcess_t)(void* control);
typedef void(__cdecl* AnimTick_t)();
const PageFn_t ClosePage = (PageFn_t)0x00697cc0;  // state 4 -> 5 (fade-out finished)
const PageFn_t PageTick = (PageFn_t)0x006979f0;
const AnimTick_t AnimTick = (AnimTick_t)0x006d26c0;

// Menu manager layout (shared by Xbox and PC)
const int kControlsBegin = 0x14, kControlsEnd = 0x18;
const int kOpenPages = 0xf0, kPageStack = 0x6c;  // stack entries 1..open
const int kPageState = 0x84;

bool g_loggedFirstInput;

// ---------------------------------------------------------------- element visuals
// Every menu element owns world objects and texts that are shown/hidden through
// 0x69a840 (thiscall, one argument: show). On Xbox (0x38e20) it stores the new
// state and applies it. The PC port changed the prologue so the argument is
// never read: the function returns unless the element is currently visible and
// then always hides it - the console menus can never become visible. The body
// still contains the full show/hide logic; this is it with the Xbox prologue.
const DWORD kElemSetVisible = 0x0069a840;
const unsigned char kElemSetVisiblePrologue[] = { 0x56, 0x8B, 0xF1, 0x8B, 0x46, 0x34, 0x85, 0xC0, 0x0F, 0x84 };

typedef int(__cdecl* FindObject_t)(DWORD key);
typedef void(__thiscall* ObjectShow_t)(void* obj, void* elem, int show);
typedef void(__cdecl* ObjectHide_t)(int object, DWORD a, DWORD flags);
typedef void(__cdecl* TextPrepare_t)(DWORD id, void* elem);
typedef void(__cdecl* TextSetVisible_t)(int table, DWORD id, int zero, int visible);
const FindObject_t FindObject = (FindObject_t)0x0043c760;
const ObjectShow_t ObjectShow = (ObjectShow_t)0x0069a010;
const ObjectHide_t ObjectHide = (ObjectHide_t)0x0043c7e0;
const TextPrepare_t TextPrepare = (TextPrepare_t)0x00699dd0;
const TextSetVisible_t TextSetVisible = (TextSetVisible_t)0x004363e0;
char** const g_textTables = (char**)0x00af5650;

void __fastcall ElementSetVisible(char* elem, void* /*edx*/, int show)
{
    int& visible = *(int*)(elem + 0x34);
    if ((show != 0) == (visible != 0)) return;
    visible = show ? 1 : 0;

    for (DWORD** it = *(DWORD***)(elem + 0xc); it != *(DWORD***)(elem + 0x10); it++) {
        DWORD* obj = *it;
        int object = FindObject(obj[1]);
        if (!object) continue;
        if (visible) ObjectShow(obj, elem, 1);
        else ObjectHide(object, obj[2], obj[4] | 0x400);
    }
    for (WORD* id = *(WORD**)(elem + 0x1c); id != *(WORD**)(elem + 0x20); id++) {
        char* tables = *g_textTables;
        if (!tables) continue;
        int table = *(int*)(tables + (*id < 0x400 ? 0x464 : *id < 0x800 ? 0x460 : 0x468));
        if (!table) continue;
        if (visible) TextPrepare(*id, elem);
        TextSetVisible(table, *id, 0, visible);
    }
}

// Xbox menu manager tick, see file comment.
bool __fastcall XboxMenuTick(char* mgr, void* /*edx*/)
{
    DWORD** it = *(DWORD***)(mgr + kControlsBegin);
    DWORD** end = *(DWORD***)(mgr + kControlsEnd);
    int consumed = 0;

    // Diagnostics: which pad bit each control polls, and pad bit changes.
    static DWORD** loggedControls;
    if (it != loggedControls && it && end > it) {
        loggedControls = it;
        for (DWORD** c = it; c < end; c++)
            if (*c) Log("console menu: control %p vt %08lx polls pad %lu bit %lu", *c, (*c)[0], (*c)[4], (*c)[5]);
    }
    static DWORD lastPad;
    DWORD pad = *(DWORD*)0x0080d074;  // engine pad bits (see gamepad.cpp)
    if (pad != lastPad) { lastPad = pad; Log("console menu: pad bits %08lx", pad); }
    for (; it && it < end; it++) {
        DWORD* control = *it;
        if (!control) continue;
        if (!consumed) {
            ControlProcess_t process = (ControlProcess_t)(*(DWORD**)control)[1];
            consumed = process(control);
            if (consumed && !g_loggedFirstInput) {
                g_loggedFirstInput = true;
                Log("console menu: first input consumed by control %p", control);
            }
        } else {
            control[2] = control[3] = control[4] = 0;
        }
    }

    int open = *(int*)(mgr + kOpenPages);
    if (open) {
        void* top = *(void**)(mgr + kPageStack + open * 4);
        if (*(int*)((char*)top + kPageState) == 4) ClosePage(top);
        PageTick(top);
    }
    AnimTick();
    return *(int*)(mgr + kOpenPages) != 0;
}

// ---------------------------------------------------------------- dialogs
// Scripts open a console dialog (0x6725c0: page, text, up to three buttons) and
// poll its answer by id through 0x672420 (cdecl int(int id)); 0 = still open.
// On Xbox (0x18bd0) the answer comes from the menu manager, where the dialog's
// buttons store it (0x672550). The PC port added a hook at the end of 0x6725c0
// (0x402840) that recognises some dialogs by their text and sets a PC mode
// (*(0x80bc44) + 0xa8):
//   1  "save game?"  -> opens the mouse page P_SaveConfirmation on top, which
//                       pauses the game while open; its buttons set the answer
//   2, 3             -> answered automatically (Xbox storage messages)
//   0  anything else -> never answered
// and turned the answer query into a jump to the PC answer (0x4029e0).
// With the console dialogs working again, the save question is answered on the
// console dialog itself: the mouse page is not opened, and the query returns the
// Xbox answer except for the automatically answered PC modes 2 and 3.
const DWORD kSavePageCall = 0x00402900;  // call 0x409870 (thiscall, 3 args): open P_SaveConfirmation
const unsigned char kSavePageCallBytes[] = { 0xE8, 0x6B, 0x6F, 0x00, 0x00 };
const DWORD kDialogAnswer = 0x00672420;
const unsigned char kDialogAnswerBytes[] = { 0xE9, 0xBB, 0x05, 0xD9, 0xFF };  // jmp 0x4029e0
typedef int(__cdecl* PcDialogAnswer_t)();
const PcDialogAnswer_t PcDialogAnswer = (PcDialogAnswer_t)0x004029e0;
char** const g_pcMenu = (char**)0x0080bc44;
char** const g_menuManager = (char**)0x00af2414;

int __cdecl DialogAnswer(int id)
{
    char* pc = *g_pcMenu;
    int mode = pc ? *(int*)(pc + 0xa8) : 0;
    int answer;
    if (mode == 2 || mode == 3) {
        answer = PcDialogAnswer();
    } else if (char* mgr = *g_menuManager) {
        short sid = (short)id;
        if (*(short*)(mgr + 0x1a4) == sid) answer = *(int*)(mgr + 0x1a0);
        else if (*(short*)(mgr + 0x1b4) == sid) answer = *(int*)(mgr + 0x1b0);
        else answer = *(short*)(mgr + 0x1a4) ? 0x20000 : 0x10000;  // unknown id
    } else {
        answer = 0;
    }
    static int lastId, lastAnswer = -1;
    if (id != lastId || answer != lastAnswer) {
        lastId = id;
        lastAnswer = answer;
        Log("console menu: dialog %d (pc mode %d) answer %d", id, mode, answer);
    }
    return answer;
}

// ---------------------------------------------------------------- save files
// The PC port replaced the Xbox storage API with its own save files
// (Profiles\<name>\SaveN.SAV). The script save/load functions call
//   0x41c810 PCHD_SaveGame(slot, ..., data, size)   (from 0x46206c)
//   0x41c620 PCHD_LoadGame(slot, data, size)        (from 0x46209f, 0x4622cd)
// which look the slot up in a list of save files (*(0x80ccb4)) that only the
// PC mouse pages build (0x41d180, before showing their save/load lists). With the
// console menus that list is never built and saving crashes on a null pointer.
// We build it right before these calls; 0x41d180 does nothing if it exists.
typedef void(__cdecl* EnumSaves_t)();
typedef int(__cdecl* SaveGame_t)(int slot, int kind, void* data, int size);
typedef int(__cdecl* LoadGame_t)(int slot, void* data, int size);
const EnumSaves_t EnumSaves = (EnumSaves_t)0x0041d180;
const SaveGame_t PcSaveGame = (SaveGame_t)0x0041c810;
const LoadGame_t PcLoadGame = (LoadGame_t)0x0041c620;
const DWORD kSaveCall = 0x0046206c;
const DWORD kLoadCalls[] = { 0x0046209f, 0x004622cd };

// Save files live in the folder of the current PC profile (*(0x80cc9c), a name
// from the profile list at 0x80ccac, count at 0x80ccb0). Only the PC profile page
// selects one (0x41b080), so with the console menus there is none. Pick the first
// existing profile (the console version has no profiles); create one if there is
// none. The profile's options are not loaded - the game keeps its current settings.
typedef void(__cdecl* RefreshProfiles_t)();
typedef int(__cdecl* SelectProfile_t)(const wchar_t* name);
typedef int(__cdecl* CreateProfile_t)(const wchar_t* name);
const RefreshProfiles_t RefreshProfiles = (RefreshProfiles_t)0x0041b070;
const SelectProfile_t SelectProfile = (SelectProfile_t)0x0041b080;
const CreateProfile_t CreateProfile = (CreateProfile_t)0x0041b150;
const wchar_t* const* const g_currentProfile = (const wchar_t* const*)0x0080cc9c;
DWORD** const g_profileList = (DWORD**)0x0080ccac;  // list head node
const int* const g_profileCount = (const int*)0x0080ccb0;

bool EnsureProfile()
{
    if (*g_currentProfile) return true;
    if (*g_profileCount == 0) RefreshProfiles();
    if (*g_profileCount == 0) {
        int r = CreateProfile(L"Prince");
        Log("console menu: no PC profile, created \"Prince\" -> %d", r);
        RefreshProfiles();
    }
    DWORD* head = *g_profileList;
    if (*g_profileCount == 0 || !head || (DWORD*)head[0] == head) {
        Log("console menu: no PC profile available, saving disabled");
        return false;
    }
    const wchar_t* name = (const wchar_t*)((DWORD*)head[0])[2];  // first node's name
    SelectProfile(name);
    Log("console menu: using PC profile \"%ls\" for save games", name);
    return *g_currentProfile != nullptr;
}

int __cdecl SaveGameHook(int slot, int kind, void* data, int size)
{
    if (!EnsureProfile()) return 0;
    EnumSaves();
    int ok = PcSaveGame(slot, kind, data, size);
    Log("console menu: save game slot %d -> %d", slot, ok);
    return ok;
}

int __cdecl LoadGameHook(int slot, void* data, int size)
{
    if (!EnsureProfile()) return 0;
    EnumSaves();
    int ok = PcLoadGame(slot, data, size);
    Log("console menu: load game slot %d -> %d", slot, ok);
    return ok;
}

bool IsCallTo(DWORD site, DWORD target)
{
    const unsigned char* p = (const unsigned char*)site;
    return p[0] == 0xE8 && site + 5 + *(const int*)(p + 1) == target;
}

void RedirectCall(DWORD site, void* target)
{
    unsigned char* p = (unsigned char*)site;
    DWORD prot;
    VirtualProtect(p, 5, PAGE_EXECUTE_READWRITE, &prot);
    *(DWORD*)(p + 1) = (DWORD)target - (site + 5);
    VirtualProtect(p, 5, prot, &prot);
    FlushInstructionCache(GetCurrentProcess(), p, 5);
}

bool Patch(DWORD addr, const unsigned char* expect, size_t len, const unsigned char* bytes, size_t n)
{
    unsigned char* fn = (unsigned char*)addr;
    if (memcmp(fn, expect, len) != 0) {
        Log("console menu: unexpected code at %08lx, not patched", addr);
        return false;
    }
    DWORD prot;
    VirtualProtect(fn, n, PAGE_EXECUTE_READWRITE, &prot);
    memcpy(fn, bytes, n);
    VirtualProtect(fn, n, prot, &prot);
    FlushInstructionCache(GetCurrentProcess(), fn, n);
    return true;
}

}  // namespace

void ConsoleMenu_Enable()
{
    static bool done;
    if (done) return;
    done = true;

    // Check all sites first so we never leave the game half-patched.
    if (memcmp((void*)kPcMenuRedirect, kPcMenuRedirectPrologue, sizeof(kPcMenuRedirectPrologue)) != 0 ||
        memcmp((void*)kMenuTick, kMenuTickPrologue, sizeof(kMenuTickPrologue)) != 0 ||
        memcmp((void*)kPadClearBranch, kPadClearBranchBytes, sizeof(kPadClearBranchBytes)) != 0 ||
        memcmp((void*)kElemSetVisible, kElemSetVisiblePrologue, sizeof(kElemSetVisiblePrologue)) != 0 ||
        memcmp((void*)kSavePageCall, kSavePageCallBytes, sizeof(kSavePageCallBytes)) != 0 ||
        memcmp((void*)kDialogAnswer, kDialogAnswerBytes, sizeof(kDialogAnswerBytes)) != 0 ||
        !IsCallTo(kSaveCall, (DWORD)PcSaveGame) || !IsCallTo(kLoadCalls[0], (DWORD)PcLoadGame) ||
        !IsCallTo(kLoadCalls[1], (DWORD)PcLoadGame)) {
        Log("console menu: unknown executable, not enabled");
        return;
    }

    unsigned char jmp[5] = { 0xE9 };
    *(DWORD*)(jmp + 1) = (DWORD)XboxMenuTick - (kMenuTick + 5);
    Patch(kMenuTick, kMenuTickPrologue, sizeof(kMenuTickPrologue), jmp, sizeof(jmp));

    const unsigned char ret = 0xC3;
    Patch(kPcMenuRedirect, kPcMenuRedirectPrologue, sizeof(kPcMenuRedirectPrologue), &ret, 1);

    *(DWORD*)(jmp + 1) = (DWORD)ElementSetVisible - (kElemSetVisible + 5);
    Patch(kElemSetVisible, kElemSetVisiblePrologue, sizeof(kElemSetVisiblePrologue), jmp, sizeof(jmp));

    const unsigned char jmpShort = 0xEB;
    Patch(kPadClearBranch, kPadClearBranchBytes, sizeof(kPadClearBranchBytes), &jmpShort, 1);

    const unsigned char dropArgs[] = { 0x83, 0xC4, 0x0C, 0x90, 0x90 };  // add esp, 12 (callee-cleaned args)
    Patch(kSavePageCall, kSavePageCallBytes, sizeof(kSavePageCallBytes), dropArgs, sizeof(dropArgs));

    *(DWORD*)(jmp + 1) = (DWORD)DialogAnswer - (kDialogAnswer + 5);
    Patch(kDialogAnswer, kDialogAnswerBytes, sizeof(kDialogAnswerBytes), jmp, sizeof(jmp));

    RedirectCall(kSaveCall, (void*)SaveGameHook);
    for (DWORD site : kLoadCalls) RedirectCall(site, (void*)LoadGameHook);

    Log("console menu: enabled (PC redirect removed, Xbox menu tick restored, pad input kept, element show restored, "
        "console save dialog)");
}
