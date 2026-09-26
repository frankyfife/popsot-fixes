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
// The pack's DLL has to be renamed (default poptex_d3d9.dll): as d3d9.dll in
// the game folder it is taken for the system d3d9.dll by modules that look it
// up by name - the GOG Galaxy overlay (proxydx9) then reads past its end and
// crashes the game.
//
// Pack options are baked into the DLL's ".popcfg" section ("PTEX", version,
// flags, "PCFG"). Flag bit 0 replaces the game's bloom pixel shader (ABB07F2E)
// with one without bloom; the HD pack sets it, the 4K pack does not. The flag
// is read whenever a pixel shader is created, so with [textures] bloom=1 we
// clear it in memory after loading and the game keeps its bloom.
//
// Single textures can be left out ([textures] skip, hex keys as in
// poptex_d3d9.log). The HD pack replaces the font atlas 0B0041BB (512x128)
// with a 256x64 one, which blurs every text; it is skipped by default.
// Evgesha.JK starts with a 0x90-byte header (texture count at +0x18) and the
// texture records (0x68 bytes, key at +0). The pack copies them into a heap
// table of larger records (key at +0, and the two SHA-256 hashes from file
// record +0x28 it matches uploads and verifies payloads with); we find that
// table through a pointer in the pack's data section (its first two keys are
// those of the file), and invalidate the hashes of skipped keys, so they never
// match and the game keeps its own texture.

#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <winternl.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>
#include <stdlib.h>
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

bool Readable(const void* p, size_t n)
{
    MEMORY_BASIC_INFORMATION mi;
    if (!VirtualQuery(p, &mi, sizeof(mi)) || mi.State != MEM_COMMIT) return false;
    if (mi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
    return (BYTE*)p + n <= (BYTE*)mi.BaseAddress + mi.RegionSize;
}

// Leaves the textures with the keys in `list` (hex, separated by commas or
// spaces) to the game.
void SkipTextures(HMODULE mod, const char* jkPath, const char* list)
{
    DWORD keys[64];
    int nkeys = 0;
    for (const char* s = list; *s && nkeys < 64;) {
        char* end;
        unsigned long k = strtoul(s, &end, 16);
        if (end == s) { s++; continue; }
        keys[nkeys++] = k;
        s = end;
    }
    if (!nkeys) return;
    // Header and texture records from the file.
    HANDLE f = CreateFileA(jkPath, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    BYTE head[0x90];
    DWORD got = 0;
    BYTE* recs = nullptr;
    DWORD count = 0;
    if (ReadFile(f, head, sizeof(head), &got, nullptr) && got == sizeof(head) && memcmp(head, "EVGJK1", 6) == 0) {
        count = *(DWORD*)(head + 0x18);
        if (count >= 2 && count <= 10000) {
            recs = (BYTE*)HeapAlloc(GetProcessHeap(), 0, count * 0x68);
            if (recs && !(ReadFile(f, recs, count * 0x68, &got, nullptr) && got == count * 0x68)) {
                HeapFree(GetProcessHeap(), 0, recs);
                recs = nullptr;
            }
        }
    }
    CloseHandle(f);
    if (!recs) return;
    DWORD key0 = *(DWORD*)recs, key1 = *(DWORD*)(recs + 0x68);
    // The pack's table: a pointer in its writable sections to key0 ... key1.
    BYTE* base = (BYTE*)mod;
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(base + ((IMAGE_DOS_HEADER*)base)->e_lfanew);
    IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
    BYTE* table = nullptr;
    DWORD stride = 0;
    for (int i = 0; i < nt->FileHeader.NumberOfSections && !table; i++, sec++) {
        if (!(sec->Characteristics & IMAGE_SCN_MEM_WRITE)) continue;
        BYTE** p = (BYTE**)(base + sec->VirtualAddress);
        BYTE** end = (BYTE**)(base + sec->VirtualAddress + sec->Misc.VirtualSize);
        for (; p < end && !table; p++) {
            BYTE* t = *p;
            if ((ULONG_PTR)t < 0x10000 || ((ULONG_PTR)t & 3) || !Readable(t, 4) || *(DWORD*)t != key0) continue;
            for (DWORD st = 0x68; st <= 0x100; st += 4)
                if (Readable(t + st, 4) && *(DWORD*)(t + st) == key1 && Readable(t, (size_t)st * count)) {
                    table = t;
                    stride = st;
                    break;
                }
        }
    }
    if (!table) {
        Log("texture pack: texture table not found, [textures] skip not applied");
    } else {
        for (int k = 0; k < nkeys; k++) {
            const char* result = "not in the pack";
            for (DWORD i = 0; i < count; i++) {
                const BYTE* frec = recs + i * 0x68;
                if (*(const DWORD*)frec != keys[k]) continue;
                // The two hashes (file record +0x28, 64 bytes) inside the pack's copy.
                BYTE* rec = table + i * stride;
                result = "not changed (hashes not found)";
                for (DWORD o = 4; o + 64 <= stride; o += 4)
                    if (memcmp(rec + o, frec + 0x28, 64) == 0) {
                        for (int b = 0; b < 64; b++) rec[o + b] ^= 0xA5;
                        result = "left to the game ([textures] skip)";
                        break;
                    }
                break;
            }
            Log("texture pack: texture %08lX %s", keys[k], result);
        }
    }
    HeapFree(GetProcessHeap(), 0, recs);
}

}  // namespace

void TexPack_Load(const char* gameDir, const char* file, bool keepBloom, const char* skip)
{
    if (!file || !*file) return;
    char path[MAX_PATH], data[MAX_PATH];
    _snprintf(path, MAX_PATH, "%s%s", gameDir, file);
    _snprintf(data, MAX_PATH, "%sEvgesha.JK", gameDir);
    path[MAX_PATH - 1] = data[MAX_PATH - 1] = 0;
    if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES) {
        char old[MAX_PATH];
        _snprintf(old, MAX_PATH, "%sd3d9.dll", gameDir);
        old[MAX_PATH - 1] = 0;
        if (GetFileAttributesA(old) != INVALID_FILE_ATTRIBUTES && GetFileAttributesA(data) != INVALID_FILE_ATTRIBUTES)
            Log("texture pack: rename the pack's d3d9.dll to %s (as d3d9.dll it crashes the GOG Galaxy overlay)", file);
        return;
    }
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
    SkipTextures(g_pack, data, skip);
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
