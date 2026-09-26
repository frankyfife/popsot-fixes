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
//
// The pack's textures are darker than the originals (about 0.8 of the
// brightness on screen). [textures] brightness scales their colours when the
// pack uploads them. The pack creates each replacement with the CreateTexture
// it found in the device vtable and fills it with LockRect/UnlockRect of the
// texture vtable, both called through the pointers it saved when it hooked
// them. So we patch these slots before the pack does (CreateDevice of the
// system IDirect3D9 class before its Direct3DCreate9 runs, the device and
// texture slots when the device is created) and sit underneath it. The
// replacements are told from the game's own textures (which pass through the
// pack's CreateTexture hook) by the return address: in the pack, but not in
// its hook. DXT1/DXT5 blocks get their two colour endpoints scaled (with the
// DXT1 mode kept), A8R8G8B8 every pixel.

#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <winternl.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>
#include <stdlib.h>
#include <intrin.h>
#include <d3d9.h>
#include "texpack.h"

void Log(const char* fmt, ...);

namespace {

HMODULE g_pack;
FARPROC g_packCreate9;
float g_brightness = 1.0f;

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

// ---------------------------------------------------------------- brightness

typedef HRESULT(STDMETHODCALLTYPE* CreateDevice_t)(IDirect3D9*, UINT, D3DDEVTYPE, HWND, DWORD, D3DPRESENT_PARAMETERS*, IDirect3DDevice9**);
typedef HRESULT(STDMETHODCALLTYPE* CreateTexture_t)(IDirect3DDevice9*, UINT, UINT, UINT, DWORD, D3DFORMAT, D3DPOOL, IDirect3DTexture9**, HANDLE*);
typedef HRESULT(STDMETHODCALLTYPE* LockRect_t)(IDirect3DTexture9*, UINT, D3DLOCKED_RECT*, const RECT*, DWORD);
typedef HRESULT(STDMETHODCALLTYPE* UnlockRect_t)(IDirect3DTexture9*, UINT);

CreateDevice_t o_createDevice;
CreateTexture_t o_createTexture;
LockRect_t o_lockRect;
UnlockRect_t o_unlockRect;
DWORD g_packStart, g_packEnd;  // the pack's image
DWORD g_packCreateTexHook;     // its CreateTexture hook
BYTE g_lut8[256], g_lut5[32], g_lut6[64];
CRITICAL_SECTION g_lock;
long g_brightened;  // replacements brightened so far

// A replacement texture being filled.
struct Upload {
    IDirect3DTexture9* tex;
    D3DFORMAT fmt;
    DWORD levelsLeft;
    UINT level;  // locked level, or ~0u
    void* bits;
    INT pitch;
};
Upload g_uploads[16];

void* PatchSlot(void* obj, int index, void* hook)
{
    void** vt = *(void***)obj;
    void* old = vt[index];
    DWORD prot;
    if (old == hook || !VirtualProtect(&vt[index], sizeof(void*), PAGE_EXECUTE_READWRITE, &prot)) return nullptr;
    vt[index] = hook;
    VirtualProtect(&vt[index], sizeof(void*), prot, &prot);
    return old;
}

inline WORD Scale565(WORD c)
{
    return (WORD)(g_lut5[c >> 11] << 11 | g_lut6[(c >> 5) & 63] << 5 | g_lut5[c & 31]);
}

// DXT1: c0 > c1 means four colours, else three and transparent black; the
// scaled endpoints keep that mode (swapped with the indices if needed).
void ScaleDxt1Block(BYTE* b)
{
    WORD c0 = *(WORD*)b, c1 = *(WORD*)(b + 2);
    DWORD idx = *(DWORD*)(b + 4);
    WORD n0 = Scale565(c0), n1 = Scale565(c1);
    if (c0 > c1) {
        if (n0 < n1) { WORD t = n0; n0 = n1; n1 = t; idx ^= 0x55555555; }  // 0<->1, 2<->3
        else if (n0 == n1) idx = 0;
    } else if (n0 > n1) {
        WORD t = n0; n0 = n1; n1 = t;
        idx ^= ~(idx >> 1) & 0x55555555;  // 0<->1, 2 and 3 stay
    }
    *(WORD*)b = n0;
    *(WORD*)(b + 2) = n1;
    *(DWORD*)(b + 4) = idx;
}

void Brighten(const Upload& u, UINT w, UINT h)
{
    BYTE* row = (BYTE*)u.bits;
    if (u.fmt == D3DFMT_DXT1 || u.fmt == D3DFMT_DXT5) {
        UINT bw = (w + 3) / 4, bh = (h + 3) / 4;
        for (UINT y = 0; y < bh; y++, row += u.pitch)
            for (UINT x = 0; x < bw; x++) {
                if (u.fmt == D3DFMT_DXT1) ScaleDxt1Block(row + x * 8);
                else {
                    WORD* c = (WORD*)(row + x * 16 + 8);  // always four colours
                    c[0] = Scale565(c[0]);
                    c[1] = Scale565(c[1]);
                }
            }
    } else {
        for (UINT y = 0; y < h; y++, row += u.pitch)
            for (BYTE *p = row, *e = row + w * 4; p < e; p += 4) {
                p[0] = g_lut8[p[0]];
                p[1] = g_lut8[p[1]];
                p[2] = g_lut8[p[2]];
            }
    }
}

bool FromPackItself(DWORD ret)
{
    return ret >= g_packStart && ret < g_packEnd && !(g_packCreateTexHook && ret - g_packCreateTexHook < 0x200);
}

HRESULT STDMETHODCALLTYPE LowLockRect(IDirect3DTexture9* t, UINT level, D3DLOCKED_RECT* lr, const RECT* r, DWORD f)
{
    HRESULT hr = o_lockRect(t, level, lr, r, f);
    if (SUCCEEDED(hr) && lr && !r) {
        EnterCriticalSection(&g_lock);
        for (Upload& u : g_uploads)
            if (u.tex == t) { u.level = level; u.bits = lr->pBits; u.pitch = lr->Pitch; }
        LeaveCriticalSection(&g_lock);
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE LowUnlockRect(IDirect3DTexture9* t, UINT level)
{
    EnterCriticalSection(&g_lock);
    for (Upload& u : g_uploads) {
        if (u.tex != t || u.level != level) continue;
        D3DSURFACE_DESC d;
        if (SUCCEEDED(t->GetLevelDesc(level, &d))) Brighten(u, d.Width, d.Height);
        u.level = ~0u;
        if (--u.levelsLeft == 0) {
            u.tex = nullptr;
            long n = ++g_brightened;
            if (n == 1 || n == 10 || n == 100 || n == 1000) Log("texture pack: %ld replacements brightened", n);
        }
    }
    LeaveCriticalSection(&g_lock);
    return o_unlockRect(t, level);
}

HRESULT STDMETHODCALLTYPE LowCreateTexture(IDirect3DDevice9* dev, UINT w, UINT h, UINT levels, DWORD usage,
                                           D3DFORMAT fmt, D3DPOOL pool, IDirect3DTexture9** out, HANDLE* sh)
{
    DWORD ret = (DWORD)_ReturnAddress();
    HRESULT hr = o_createTexture(dev, w, h, levels, usage, fmt, pool, out, sh);
    if (SUCCEEDED(hr) && out && *out && pool == D3DPOOL_MANAGED && FromPackItself(ret) &&
        (fmt == D3DFMT_DXT1 || fmt == D3DFMT_DXT5 || fmt == D3DFMT_A8R8G8B8)) {
        EnterCriticalSection(&g_lock);
        for (Upload& u : g_uploads)
            if (!u.tex) {
                u.tex = *out;
                u.fmt = fmt;
                u.levelsLeft = (*out)->GetLevelCount();
                u.level = ~0u;
                break;
            }
        LeaveCriticalSection(&g_lock);
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE LowCreateDevice(IDirect3D9* d3d, UINT a, D3DDEVTYPE t, HWND w, DWORD f,
                                          D3DPRESENT_PARAMETERS* pp, IDirect3DDevice9** out)
{
    HRESULT hr = o_createDevice(d3d, a, t, w, f, pp, out);
    if (SUCCEEDED(hr) && out && *out && !o_createTexture) {
        o_createTexture = (CreateTexture_t)PatchSlot(*out, 23, (void*)LowCreateTexture);
        IDirect3DTexture9* probe = nullptr;
        if (o_createTexture &&
            SUCCEEDED(o_createTexture(*out, 4, 4, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &probe, nullptr)) && probe) {
            o_lockRect = (LockRect_t)PatchSlot(probe, 19, (void*)LowLockRect);
            o_unlockRect = (UnlockRect_t)PatchSlot(probe, 20, (void*)LowUnlockRect);
            probe->Release();
        }
        Log("texture pack: brightness %.2f %s", g_brightness,
            o_lockRect && o_unlockRect ? "installed" : "not installed (texture vtable)");
    }
    return hr;
}

void InstallBrightness()
{
    if (g_brightness == 1.0f) return;
    for (int i = 0; i < 256; i++) { int v = (int)(i * g_brightness + 0.5f); g_lut8[i] = (BYTE)(v > 255 ? 255 : v); }
    for (int i = 0; i < 32; i++) { int v = (int)(i * g_brightness + 0.5f); g_lut5[i] = (BYTE)(v > 31 ? 31 : v); }
    for (int i = 0; i < 64; i++) { int v = (int)(i * g_brightness + 0.5f); g_lut6[i] = (BYTE)(v > 63 ? 63 : v); }
    InitializeCriticalSection(&g_lock);
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)((BYTE*)g_pack + ((IMAGE_DOS_HEADER*)g_pack)->e_lfanew);
    g_packStart = (DWORD)g_pack;
    g_packEnd = g_packStart + nt->OptionalHeader.SizeOfImage;
    char sys[MAX_PATH];
    GetSystemDirectoryA(sys, MAX_PATH);
    strncat(sys, "\\d3d9.dll", MAX_PATH - strlen(sys) - 1);
    HMODULE d3d9 = LoadLibraryA(sys);
    FARPROC create = d3d9 ? GetProcAddress(d3d9, "Direct3DCreate9") : nullptr;
    IDirect3D9* d3d = create ? ((IDirect3D9 * (WINAPI*)(UINT))create)(D3D_SDK_VERSION) : nullptr;
    if (!d3d) { Log("texture pack: brightness not installed (no system Direct3D)"); return; }
    o_createDevice = (CreateDevice_t)PatchSlot(d3d, 16, (void*)LowCreateDevice);
    d3d->Release();
}

}  // namespace

void TexPack_Load(const char* gameDir, const char* file, bool keepBloom, const char* skip, float brightness)
{
    if (!file || !*file) return;
    g_brightness = brightness < 0.5f ? 0.5f : brightness > 2.0f ? 2.0f : brightness;
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
    InstallBrightness();  // underneath the pack
    // Its Direct3DCreate9 hooks the system IDirect3D9 class (CreateDevice).
    IUnknown* d3d = ((IUnknown * (WINAPI*)(UINT))g_packCreate9)(32);
    if (d3d) d3d->Release();
    Log("texture pack: Direct3D interception installed (%p)", d3d);
}

void TexPack_DeviceCreated(IDirect3DDevice9* dev)
{
    // Before our own device hooks: the slot now holds the pack's CreateTexture hook.
    DWORD hook = (DWORD)(*(void***)dev)[23];
    if (!g_packCreateTexHook && hook >= g_packStart && hook < g_packEnd) g_packCreateTexHook = hook;
}

