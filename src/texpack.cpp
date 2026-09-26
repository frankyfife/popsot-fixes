// Texture packs made with "PoP Texture Studio" (d3d9.dll + Evgesha.JK in the
// game folder, e.g. the HD and 4K packs on Nexus Mods).
//
// The pack's d3d9.dll is a Direct3D 9 proxy for the retail POP.EXE. It works
// with the GOG data (prince.bf is identical: same size, every BF overlay's
// SHA-256 matches), but not with the GOG executable:
//   - gpp.exe imports dx.dll instead of d3d9.dll, and GOG's wrapper loads the
//     system d3d9.dll by its full path, so the pack is never loaded;
//   - its DllMain only activates when the executable is named POP.EXE
//     ("inactive: GPU package belongs to another game profile").
// Its texture replacement patches the vtable of the system IDirect3D9 class
// (CreateDevice, then the device) when its Direct3DCreate9 runs, and its BF
// overlays patch CreateFileA/CreateFileW/ReadFile/CloseHandle in the
// executable's import table. So we load it ourselves (from DllMain, before
// the game opens prince.bf), with the executable's name shown as POP.EXE while
// its DllMain runs, and call its Direct3DCreate9 once before our own vtable
// hooks. The chain is then: our hooks -> pack -> system d3d9, underneath GOG's
// wrapper. Messages of the pack go to poptex_d3d9.log.
//
// Pack options are baked into the DLL's ".popcfg" section ("PTEX", version,
// flags, "PCFG"). Flag bit 0 replaces the game's bloom pixel shader (ABB07F2E)
// with one without bloom; the HD pack sets it, the 4K pack does not. The flag
// is read whenever a pixel shader is created, so with [textures] bloom=1 we
// clear it in memory after loading and the game keeps its bloom.

#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <winternl.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>
#include "texpack.h"

void Log(const char* fmt, ...);

namespace {

HMODULE g_pack;
FARPROC g_packCreate9;

// Replaces the file name at the end of the string in place (same length).
bool SwapName(UNICODE_STRING* s, const wchar_t* from, const wchar_t* to, size_t n)
{
    if (!s || !s->Buffer || s->Length < n * sizeof(wchar_t)) return false;
    size_t len = s->Length / sizeof(wchar_t);
    wchar_t* tail = s->Buffer + len - n;
    if (_wcsnicmp(tail, from, n) != 0) return false;
    if (len > n && tail[-1] != L'\\' && tail[-1] != L'/') return false;
    wmemcpy(tail, to, n);
    return true;
}

// Renames the executable as the loader reports it (GetModuleFileNameW(NULL),
// the loader entry of the main module and the process image path) from `from`
// to `to` (same length). Returns the number of strings changed.
int RenameImage(const wchar_t* from, const wchar_t* to)
{
    size_t n = wcslen(from);
    if (wcslen(to) != n) return 0;
    int changed = 0;
    PEB* peb = (PEB*)NtCurrentTeb()->ProcessEnvironmentBlock;
    HMODULE exe = GetModuleHandleW(nullptr);
    LIST_ENTRY* head = &peb->Ldr->InMemoryOrderModuleList;
    for (LIST_ENTRY* e = head->Flink; e != head; e = e->Flink) {
        LDR_DATA_TABLE_ENTRY* m = CONTAINING_RECORD(e, LDR_DATA_TABLE_ENTRY, InMemoryOrderLinks);
        if (m->DllBase != exe) continue;
        UNICODE_STRING* full = &m->FullDllName;
        UNICODE_STRING* base = (UNICODE_STRING*)m->Reserved4;  // BaseDllName
        wchar_t* fullEnd = full->Buffer + full->Length / sizeof(wchar_t);
        if (SwapName(full, from, to, n)) changed++;
        // BaseDllName usually points into FullDllName's buffer.
        if (!(base->Buffer >= full->Buffer && base->Buffer < fullEnd) && SwapName(base, from, to, n)) changed++;
        break;
    }
    if (peb->ProcessParameters && SwapName(&peb->ProcessParameters->ImagePathName, from, to, n)) changed++;
    return changed;
}

// The pack's option flags (see above), or null.
DWORD* PackFlags(HMODULE mod)
{
    BYTE* base = (BYTE*)mod;
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(base + ((IMAGE_DOS_HEADER*)base)->e_lfanew);
    IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++, sec++) {
        if (memcmp(sec->Name, ".popcfg", 7) != 0 || sec->Misc.VirtualSize < 16) continue;
        DWORD* cfg = (DWORD*)(base + sec->VirtualAddress);
        if (cfg[0] == 0x58455450 && cfg[1] == 1 && cfg[3] == 0x47464350) return &cfg[2];  // "PTEX", 1, flags, "PCFG"
    }
    return nullptr;
}

}  // namespace

void TexPack_Load(const char* gameDir, const char* file, bool keepBloom)
{
    if (!file || !*file) return;
    char path[MAX_PATH], data[MAX_PATH];
    _snprintf(path, MAX_PATH, "%s%s", gameDir, file);
    _snprintf(data, MAX_PATH, "%sEvgesha.JK", gameDir);
    path[MAX_PATH - 1] = data[MAX_PATH - 1] = 0;
    if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES) return;
    if (GetFileAttributesA(data) == INVALID_FILE_ATTRIBUTES) {
        Log("texture pack: %s found but no Evgesha.JK, not loaded", file);
        return;
    }
    // The executable is gpp.exe; the pack only accepts POP.EXE.
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    const wchar_t* name = wcsrchr(exe, L'\\');
    name = name ? name + 1 : exe;
    wchar_t original[MAX_PATH];
    wcscpy(original, name);
    bool renamed = wcslen(original) == 7 && RenameImage(original, L"POP.EXE") > 0;
    g_pack = LoadLibraryA(path);
    DWORD err = GetLastError();
    if (renamed) RenameImage(L"POP.EXE", original);
    if (!g_pack) {
        Log("texture pack: %s could not be loaded (error %lu)", path, err);
        return;
    }
    g_packCreate9 = GetProcAddress(g_pack, "Direct3DCreate9");
    Log("texture pack: %s loaded%s; its messages are in poptex_d3d9.log", file,
        renamed ? " (shown as POP.EXE while it started)" : "");
    DWORD* flags = PackFlags(g_pack);
    if (flags && keepBloom && (*flags & 1)) {
        DWORD prot;
        if (VirtualProtect(flags, 4, PAGE_READWRITE, &prot)) {
            *flags &= ~1u;
            VirtualProtect(flags, 4, prot, &prot);
            Log("texture pack: its no-bloom shader is off, the game keeps its bloom ([textures] bloom=1)");
        }
    }
}

void TexPack_HookSystem()
{
    if (!g_packCreate9) return;
    // Its Direct3DCreate9 hooks the system IDirect3D9 class (CreateDevice).
    IUnknown* d3d = ((IUnknown * (WINAPI*)(UINT))g_packCreate9)(32);
    if (d3d) d3d->Release();
    Log("texture pack: Direct3D interception installed (%p)", d3d);
}
