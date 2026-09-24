// Prince of Persia: The Sands of Time (PC, GOG) - graphics fixes and menu pad support.
//
// 1. Post-effect resolution: the game builds its post effects (water
//    refraction, motion blur, glow) from 512x512 render targets and draws them
//    back onto the screen with POINT filtering, which is extremely blocky at
//    modern resolutions. Full-screen downsamples get a full-resolution
//    "shadow" copy that is bound instead; processed buffers get LINEAR filtering.
// 2. Water refraction strength: the PC port lowered the depth scale of the
//    refraction offset (ZOFFSET/ZMAX 1/3 instead of the Xbox's 3/10); the Xbox
//    values are restored.
// 3. Controller navigation for the mouse-only front-end menus (menupad.cpp).
//
// F10 toggles fixes 1 and 2 at runtime; diagnostics go to popfix.log.
//
// Installed as dx.dll (the D3D9 import of GOG's gpp.exe); GOG's own wrapper is
// renamed to dx_gog.dll and everything is forwarded to it. Before the first
// Direct3DCreate9 we patch the vtables of the *system* d3d9 objects, so GOG's
// wrapper keeps working and our hooks sit underneath it on the real device.

#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <d3d9.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include "menupad.h"
#include "trace.h"
#include "consolemenu.h"
#include "gamepad.h"

// ---------------------------------------------------------------- logging
static FILE* g_log;
static HWND g_gameWindow;  // focus window of the game device
static int g_devicesHooked;
void Log(const char* fmt, ...)
{
    if (!g_log) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fputc('\n', g_log);
    fflush(g_log);
}

// ---------------------------------------------------------------- crash log
// The game only notes "Last execution crashed" in POP.LOG. Log fatal exceptions
// with registers and the game-code return addresses found on the stack.
static LONG CALLBACK CrashLogger(EXCEPTION_POINTERS* ep)
{
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_ILLEGAL_INSTRUCTION &&
        code != EXCEPTION_INT_DIVIDE_BY_ZERO && code != EXCEPTION_STACK_OVERFLOW &&
        code != EXCEPTION_PRIV_INSTRUCTION)
        return EXCEPTION_CONTINUE_SEARCH;
    // Our own diagnostics read game memory inside __try; those faults are expected.
    static HMODULE self;
    if (!self)
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)CrashLogger, &self);
    HMODULE at = nullptr;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)ep->ContextRecord->Eip, &at) && at == self)
        return EXCEPTION_CONTINUE_SEARCH;
    static LONG logged;
    if (InterlockedIncrement(&logged) > 8) return EXCEPTION_CONTINUE_SEARCH;  // first-chance, may be handled
    CONTEXT* c = ep->ContextRecord;
    HMODULE mod = nullptr;
    char name[MAX_PATH] = "?";
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)c->Eip, &mod))
        GetModuleFileNameA(mod, name, MAX_PATH);
    Log("CRASH: exception %08lx at %08lx (%s + %lx), access %lu %08lx", code, c->Eip, name,
        c->Eip - (DWORD)mod, (DWORD)ep->ExceptionRecord->ExceptionInformation[0],
        (DWORD)ep->ExceptionRecord->ExceptionInformation[1]);
    Log("  eax %08lx ebx %08lx ecx %08lx edx %08lx esi %08lx edi %08lx ebp %08lx esp %08lx", c->Eax, c->Ebx,
        c->Ecx, c->Edx, c->Esi, c->Edi, c->Ebp, c->Esp);
    DWORD* sp = (DWORD*)c->Esp;
    int found = 0;
    for (int i = 0; i < 1024 && found < 24; i++) {
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery(sp + i, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT) break;
        DWORD v = sp[i];
        if (v >= 0x401000 && v < 0x7a0000) { Log("  stack+%03x: %08lx", i * 4, v); found++; }
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

// ---------------------------------------------------------------- real d3d9 + export stubs
static HMODULE g_gog;  // GOG's wrapper (dx_gog.dll)
static HMODULE g_sys;  // system d3d9.dll
static FARPROC p_sysCreate9, p_sysCreate9Ex;

#define REAL_EXPORTS(X)                         \
    X(Direct3DCreate9)                          \
    X(Direct3DCreate9Ex)                        \
    X(Direct3DCreate9On12)                      \
    X(Direct3DCreate9On12Ex)                    \
    X(Direct3DShaderValidatorCreate9)           \
    X(Direct3D9EnableMaximizedWindowedModeShim) \
    X(D3DPERF_BeginEvent)                       \
    X(D3DPERF_EndEvent)                         \
    X(D3DPERF_GetStatus)                        \
    X(D3DPERF_QueryRepeatFrame)                 \
    X(D3DPERF_SetMarker)                        \
    X(D3DPERF_SetOptions)                       \
    X(D3DPERF_SetRegion)                        \
    X(DebugSetLevel)                            \
    X(DebugSetMute)                             \
    X(PSGPError)                                \
    X(PSGPSampleTexture)

#define DECLARE_PTR(name) static FARPROC p_##name;
REAL_EXPORTS(DECLARE_PTR)

// Pass-through exports: plain jumps into the real d3d9.dll.
#define DEFINE_STUB(name) extern "C" __declspec(naked) void Stub_##name() { __asm { jmp [p_##name] } }
DEFINE_STUB(Direct3DShaderValidatorCreate9)
DEFINE_STUB(Direct3D9EnableMaximizedWindowedModeShim)
DEFINE_STUB(D3DPERF_BeginEvent)
DEFINE_STUB(D3DPERF_EndEvent)
DEFINE_STUB(D3DPERF_GetStatus)
DEFINE_STUB(D3DPERF_QueryRepeatFrame)
DEFINE_STUB(D3DPERF_SetMarker)
DEFINE_STUB(D3DPERF_SetOptions)
DEFINE_STUB(D3DPERF_SetRegion)
DEFINE_STUB(DebugSetLevel)
DEFINE_STUB(DebugSetMute)

// ---------------------------------------------------------------- vtable hooking
static void* PatchVTable(void* obj, int index, void* hook)
{
    void** vt = *(void***)obj;
    void* old = vt[index];
    if (old == hook) return nullptr;  // already hooked (shared vtable)
    DWORD prot;
    VirtualProtect(&vt[index], sizeof(void*), PAGE_EXECUTE_READWRITE, &prot);
    vt[index] = hook;
    VirtualProtect(&vt[index], sizeof(void*), prot, &prot);
    return old;
}

typedef HRESULT(STDMETHODCALLTYPE* CreateDevice_t)(IDirect3D9*, UINT, D3DDEVTYPE, HWND, DWORD, D3DPRESENT_PARAMETERS*, IDirect3DDevice9**);
typedef HRESULT(STDMETHODCALLTYPE* CreateDeviceEx_t)(IDirect3D9Ex*, UINT, D3DDEVTYPE, HWND, DWORD, D3DPRESENT_PARAMETERS*, D3DDISPLAYMODEEX*, IDirect3DDevice9Ex**);
typedef HRESULT(STDMETHODCALLTYPE* Reset_t)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);
typedef HRESULT(STDMETHODCALLTYPE* Present_t)(IDirect3DDevice9*, const RECT*, const RECT*, HWND, const RGNDATA*);
typedef HRESULT(STDMETHODCALLTYPE* CreateTexture_t)(IDirect3DDevice9*, UINT, UINT, UINT, DWORD, D3DFORMAT, D3DPOOL, IDirect3DTexture9**, HANDLE*);
typedef HRESULT(STDMETHODCALLTYPE* ResetEx_t)(IDirect3DDevice9Ex*, D3DPRESENT_PARAMETERS*, D3DDISPLAYMODEEX*);
typedef HRESULT(STDMETHODCALLTYPE* SetRenderTarget_t)(IDirect3DDevice9*, DWORD, IDirect3DSurface9*);
typedef HRESULT(STDMETHODCALLTYPE* StretchRect_t)(IDirect3DDevice9*, IDirect3DSurface9*, const RECT*, IDirect3DSurface9*, const RECT*, D3DTEXTUREFILTERTYPE);
typedef HRESULT(STDMETHODCALLTYPE* SetTexture_t)(IDirect3DDevice9*, DWORD, IDirect3DBaseTexture9*);
typedef HRESULT(STDMETHODCALLTYPE* SetSamplerState_t)(IDirect3DDevice9*, DWORD, D3DSAMPLERSTATETYPE, DWORD);
typedef HRESULT(STDMETHODCALLTYPE* DrawPrimitive_t)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, UINT);
typedef HRESULT(STDMETHODCALLTYPE* DrawIndexedPrimitive_t)(IDirect3DDevice9*, D3DPRIMITIVETYPE, INT, UINT, UINT, UINT, UINT);
typedef HRESULT(STDMETHODCALLTYPE* DrawPrimitiveUP_t)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, const void*, UINT);
typedef HRESULT(STDMETHODCALLTYPE* DrawIndexedPrimitiveUP_t)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, UINT, UINT, const void*, D3DFORMAT, const void*, UINT);
typedef HRESULT(STDMETHODCALLTYPE* CreatePixelShader_t)(IDirect3DDevice9*, const DWORD*, IDirect3DPixelShader9**);
typedef HRESULT(STDMETHODCALLTYPE* SetPixelShader_t)(IDirect3DDevice9*, IDirect3DPixelShader9*);
typedef HRESULT(STDMETHODCALLTYPE* SetVertexShaderConstantF_t)(IDirect3DDevice9*, UINT, const float*, UINT);

static CreateDevice_t o_CreateDevice;
static CreateDeviceEx_t o_CreateDeviceEx;
static Reset_t o_Reset;
static Present_t o_Present;
static CreateTexture_t o_CreateTexture;
static ResetEx_t o_ResetEx;
static SetRenderTarget_t o_SetRenderTarget;
static StretchRect_t o_StretchRect;
static SetTexture_t o_SetTexture;
static SetSamplerState_t o_SetSamplerState;
static DrawPrimitive_t o_DrawPrimitive;
static DrawIndexedPrimitive_t o_DrawIndexedPrimitive;
static DrawPrimitiveUP_t o_DrawPrimitiveUP;
static DrawIndexedPrimitiveUP_t o_DrawIndexedPrimitiveUP;
static CreatePixelShader_t o_CreatePixelShader;
static SetPixelShader_t o_SetPixelShader;
static SetVertexShaderConstantF_t o_SetVertexShaderConstantF;

// ---------------------------------------------------------------- post-effect state
//
// The game builds its post effects (water refraction, motion blur, glow) by
// StretchRect'ing full-screen images into 512x512 textures and later drawing
// those back onto full-screen targets with POINT filtering. At high resolutions
// that is extremely blocky. For every such downsample we keep a full-resolution
// "shadow" copy; when a small texture that still holds exactly that copy is
// sampled while rendering to a large target, the shadow is bound instead. Small
// textures the game has processed further (blur passes) only get LINEAR
// filtering when upscaled, so the effect itself stays as designed.

static IDirect3DPixelShader9* g_waterPS;
static IDirect3DPixelShader9* g_curPS;

static IDirect3DBaseTexture9* g_boundTex[4];
static DWORD g_magFilter[4] = { D3DTEXF_POINT, D3DTEXF_POINT, D3DTEXF_POINT, D3DTEXF_POINT };
static DWORD g_minFilter[4] = { D3DTEXF_POINT, D3DTEXF_POINT, D3DTEXF_POINT, D3DTEXF_POINT };

static UINT g_curRTWidth;

struct Shadow {
    IDirect3DBaseTexture9* smallTex;   // identity key, not ref-counted
    IDirect3DSurface9* smallSurf;   // identity key, not ref-counted
    UINT smallW;
    IDirect3DTexture9* full;
    IDirect3DSurface9* fullSurf;
    UINT fullW, fullH;
    D3DFORMAT fullFmt;
    bool valid;                     // small texture still holds exactly the copied image
};
static const int kMaxShadows = 8;
static Shadow g_shadows[kMaxShadows];
static int g_numShadows;

static void ReleaseShadows()
{
    for (int i = 0; i < g_numShadows; i++) {
        if (g_shadows[i].fullSurf) g_shadows[i].fullSurf->Release();
        if (g_shadows[i].full) g_shadows[i].full->Release();
    }
    memset(g_shadows, 0, sizeof(g_shadows));
    g_numShadows = 0;
    g_curRTWidth = 0;
}

static Shadow* FindShadow(IDirect3DBaseTexture9* key)
{
    for (int i = 0; i < g_numShadows; i++)
        if (g_shadows[i].smallTex == key) return &g_shadows[i];
    return nullptr;
}

static void InvalidateShadowsFor(IDirect3DSurface9* surf)
{
    for (int i = 0; i < g_numShadows; i++)
        if (g_shadows[i].smallSurf == surf) g_shadows[i].valid = false;
}

// Recognise the water pixel shader: ps_1_1 with texcoord t0, three tex fetches
// and a chain of lrp's (see POP.EXE @0x7f8b88 / xemu combiner dump).
static bool IsWaterShaderBytecode(const DWORD* p)
{
    if (!p || p[0] != 0xFFFF0101) return false;
    int nTexcoord = 0, nTex = 0, nLrp = 0;
    for (int i = 1; i < 512;) {
        DWORD tok = p[i];
        if (tok == 0x0000FFFF) break;
        DWORD op = tok & 0xFFFF;
        if (op == 0xFFFE) { i += 1 + ((tok >> 16) & 0x7FFF); continue; }  // comment
        int params;
        switch (op) {
        case 0x00: params = 0; break;             // nop
        case 0x01: params = 2; break;             // mov
        case 0x02: case 0x03: case 0x05:          // add sub mul
        case 0x08: case 0x09: params = 3; break;  // dp3 dp4
        case 0x04: case 0x12: case 0x50: params = 4; break;  // mad lrp cnd
        case 0x40: params = 1; nTexcoord++; break;           // texcoord
        case 0x42: params = 1; nTex++; break;                // tex
        case 0x51: params = 5; break;                        // def
        default: return false;
        }
        if (op == 0x12) nLrp++;
        i += 1 + params;
    }
    return nTexcoord == 1 && nTex == 3 && nLrp >= 3;
}

static bool EnsureShadowTexture(IDirect3DDevice9* dev, Shadow& sh, const D3DSURFACE_DESC& sd)
{
    if (sh.full && sh.fullW == sd.Width && sh.fullH == sd.Height && sh.fullFmt == sd.Format)
        return true;
    if (sh.fullSurf) { sh.fullSurf->Release(); sh.fullSurf = nullptr; }
    if (sh.full) { sh.full->Release(); sh.full = nullptr; }
    HRESULT hr = o_CreateTexture(dev, sd.Width, sd.Height, 1, D3DUSAGE_RENDERTARGET, sd.Format,
                                    D3DPOOL_DEFAULT, &sh.full, nullptr);
    if (FAILED(hr)) {
        Log("CreateTexture %ux%u fmt %d failed: 0x%08lx", sd.Width, sd.Height, sd.Format, hr);
        return false;
    }
    sh.full->GetSurfaceLevel(0, &sh.fullSurf);
    sh.fullW = sd.Width; sh.fullH = sd.Height; sh.fullFmt = sd.Format;
    Log("shadow %d: full-res %ux%u fmt %d for small texture %p (%u wide)",
        (int)(&sh - g_shadows), sd.Width, sd.Height, sd.Format, sh.smallTex, sh.smallW);
    return true;
}

static bool g_loggedSubst, g_loggedWater, g_loggedLinear;
static bool g_enabled = true;  // toggled with F10

// Per-interval statistics of small render-target textures sampled while drawing
// into a larger target (i.e. upscaled post-effect buffers).
enum StatAction { STAT_SHADOW, STAT_LINEAR, STAT_ALREADY_LINEAR, STAT_OFF, STAT_COUNT };
static const char* const kStatNames[STAT_COUNT] = { "shadow", "linear", "already-linear", "fix-off" };
struct Stat { UINT w, h, rtW; int action; unsigned count; };
static const int kMaxStats = 32;
static Stat g_stats[kMaxStats];
static int g_numStats;

static void CountStat(UINT w, UINT h, int action)
{
    for (int i = 0; i < g_numStats; i++) {
        Stat& st = g_stats[i];
        if (st.w == w && st.h == h && st.rtW == g_curRTWidth && st.action == action) { st.count++; return; }
    }
    if (g_numStats < kMaxStats) g_stats[g_numStats++] = { w, h, g_curRTWidth, action, 1 };
}

static void DumpStats(unsigned frames)
{
    Log("--- %u frames, fix %s ---", frames, g_enabled ? "ON" : "OFF");
    for (int i = 0; i < g_numStats; i++) {
        const Stat& st = g_stats[i];
        Log("  %4ux%-4u -> %4u wide target: %-14s x%u", st.w, st.h, st.rtW, kStatNames[st.action], st.count);
    }
    g_numStats = 0;
}

// Size of a plain 2D render-target texture; false for anything else.
static bool RenderTargetTextureSize(IDirect3DBaseTexture9* t, UINT& w, UINT& h)
{
    if (t->GetType() != D3DRTYPE_TEXTURE) return false;
    D3DSURFACE_DESC d;
    if (FAILED(static_cast<IDirect3DTexture9*>(t)->GetLevelDesc(0, &d))) return false;
    if (!(d.Usage & D3DUSAGE_RENDERTARGET)) return false;
    w = d.Width; h = d.Height;
    return true;
}

// Before a draw: upgrade upscaled post-effect textures. Returns patched stages.
static unsigned BeginDraw(IDirect3DDevice9* dev)
{
    if (!g_curRTWidth) return 0;
    unsigned mask = 0;
    for (int s = 0; s < 4; s++) {
        IDirect3DBaseTexture9* t = g_boundTex[s];
        if (!t) continue;
        UINT w, h;
        if (!RenderTargetTextureSize(t, w, h) || w >= g_curRTWidth) continue;  // not an upscaled buffer
        if (!g_enabled) { CountStat(w, h, STAT_OFF); continue; }

        Shadow* sh = FindShadow(t);
        if (sh && sh->valid && sh->full) {
            o_SetTexture(dev, s, sh->full);
            CountStat(w, h, STAT_SHADOW);
            if (!g_loggedSubst) { g_loggedSubst = true; Log("first full-res shadow substitution (stage %d)", s); }
            if (!g_loggedWater && g_curPS && g_curPS == g_waterPS) {
                g_loggedWater = true;
                Log("water draw uses full-res refraction source");
            }
        } else if (g_magFilter[s] == D3DTEXF_POINT || g_minFilter[s] == D3DTEXF_POINT) {
            CountStat(w, h, STAT_LINEAR);
            if (!g_loggedLinear) { g_loggedLinear = true; Log("first LINEAR upgrade of an upscaled %ux%u buffer", w, h); }
        } else {
            CountStat(w, h, STAT_ALREADY_LINEAR);
            continue;
        }
        o_SetSamplerState(dev, s, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
        o_SetSamplerState(dev, s, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
        mask |= 1u << s;
    }
    return mask;
}

static void EndDraw(IDirect3DDevice9* dev, unsigned mask)
{
    for (int s = 0; s < 4; s++) {
        if (!(mask & (1u << s))) continue;
        o_SetTexture(dev, s, g_boundTex[s]);
        o_SetSamplerState(dev, s, D3DSAMP_MAGFILTER, g_magFilter[s]);
        o_SetSamplerState(dev, s, D3DSAMP_MINFILTER, g_minFilter[s]);
    }
}

// ---------------------------------------------------------------- device hooks
static HRESULT STDMETHODCALLTYPE hk_Reset(IDirect3DDevice9* dev, D3DPRESENT_PARAMETERS* pp)
{
    ReleaseShadows();
    return o_Reset(dev, pp);
}

static HRESULT STDMETHODCALLTYPE hk_ResetEx(IDirect3DDevice9Ex* dev, D3DPRESENT_PARAMETERS* pp, D3DDISPLAYMODEEX* fm)
{
    ReleaseShadows();
    return o_ResetEx(dev, pp, fm);
}

static HRESULT STDMETHODCALLTYPE hk_Present(IDirect3DDevice9* dev, const RECT* sr, const RECT* dr, HWND w,
                                            const RGNDATA* rgn)
{
    static unsigned frames;
    if (GetAsyncKeyState(VK_F10) & 1) {
        g_enabled = !g_enabled;
        Log("F10: fix %s", g_enabled ? "ON" : "OFF");
    }
    if (++frames == 300) {
        DumpStats(frames);
        Trace_Dump();
        frames = 0;
    }
    MenuPad_OnPresent();
    Gamepad_OnFrame(g_gameWindow);
    Trace_ProbeMenuManager();
    return o_Present(dev, sr, dr, w, rgn);
}

static HRESULT STDMETHODCALLTYPE hk_CreateTexture(IDirect3DDevice9* dev, UINT w, UINT h, UINT levels, DWORD usage,
                                                  D3DFORMAT fmt, D3DPOOL pool, IDirect3DTexture9** out, HANDLE* sh)
{
    HRESULT hr = o_CreateTexture(dev, w, h, levels, usage, fmt, pool, out, sh);
    if (usage & D3DUSAGE_RENDERTARGET)
        Log("game render-target texture %ux%u levels %u fmt %d -> %p (0x%08lx)", w, h, levels, fmt,
            out ? *out : nullptr, hr);
    return hr;
}

static HRESULT STDMETHODCALLTYPE hk_SetRenderTarget(IDirect3DDevice9* dev, DWORD idx, IDirect3DSurface9* surf)
{
    if (idx == 0) {
        g_curRTWidth = 0;
        D3DSURFACE_DESC d;
        if (surf && SUCCEEDED(surf->GetDesc(&d))) g_curRTWidth = d.Width;
    }
    if (surf) InvalidateShadowsFor(surf);  // the game is about to render into it
    return o_SetRenderTarget(dev, idx, surf);
}

static HRESULT STDMETHODCALLTYPE hk_StretchRect(IDirect3DDevice9* dev, IDirect3DSurface9* src, const RECT* sr,
                                                IDirect3DSurface9* dst, const RECT* dr, D3DTEXTUREFILTERTYPE f)
{
    HRESULT hr = o_StretchRect(dev, src, sr, dst, dr, f);
    if (!dst) return hr;
    InvalidateShadowsFor(dst);
    if (FAILED(hr) || !src || sr || dr) return hr;

    // Full-screen image downsampled into a texture: keep a full-res copy of it.
    D3DSURFACE_DESC sd, dd;
    if (FAILED(src->GetDesc(&sd)) || FAILED(dst->GetDesc(&dd))) return hr;
    if (!(sd.Usage & D3DUSAGE_RENDERTARGET) || dd.Width >= sd.Width) return hr;
    IDirect3DTexture9* container = nullptr;
    if (FAILED(dst->GetContainer(__uuidof(IDirect3DTexture9), (void**)&container)) || !container) return hr;
    container->Release();  // only used as an identity key

    Shadow* sh = FindShadow(container);
    if (!sh) {
        if (g_numShadows == kMaxShadows) return hr;
        sh = &g_shadows[g_numShadows++];
        sh->smallTex = container;
    }
    sh->smallSurf = dst;
    sh->smallW = dd.Width;
    if (!EnsureShadowTexture(dev, *sh, sd)) return hr;
    if (SUCCEEDED(o_StretchRect(dev, src, nullptr, sh->fullSurf, nullptr, D3DTEXF_NONE)))
        sh->valid = true;
    return hr;
}

static HRESULT STDMETHODCALLTYPE hk_SetTexture(IDirect3DDevice9* dev, DWORD stage, IDirect3DBaseTexture9* tex)
{
    if (stage < 4) g_boundTex[stage] = tex;
    return o_SetTexture(dev, stage, tex);
}

static HRESULT STDMETHODCALLTYPE hk_SetSamplerState(IDirect3DDevice9* dev, DWORD s, D3DSAMPLERSTATETYPE t, DWORD v)
{
    if (s < 4) {
        if (t == D3DSAMP_MAGFILTER) g_magFilter[s] = v;
        else if (t == D3DSAMP_MINFILTER) g_minFilter[s] = v;
    }
    return o_SetSamplerState(dev, s, t, v);
}

static HRESULT STDMETHODCALLTYPE hk_DrawPrimitive(IDirect3DDevice9* dev, D3DPRIMITIVETYPE t, UINT sv, UINT n)
{
    unsigned m = BeginDraw(dev);
    HRESULT hr = o_DrawPrimitive(dev, t, sv, n);
    if (m) EndDraw(dev, m);
    return hr;
}

static HRESULT STDMETHODCALLTYPE hk_DrawIndexedPrimitive(IDirect3DDevice9* dev, D3DPRIMITIVETYPE t, INT bv, UINT mi,
                                                         UINT nv, UINT si, UINT n)
{
    unsigned m = BeginDraw(dev);
    HRESULT hr = o_DrawIndexedPrimitive(dev, t, bv, mi, nv, si, n);
    if (m) EndDraw(dev, m);
    return hr;
}

static HRESULT STDMETHODCALLTYPE hk_DrawPrimitiveUP(IDirect3DDevice9* dev, D3DPRIMITIVETYPE t, UINT n,
                                                    const void* data, UINT stride)
{
    unsigned m = BeginDraw(dev);
    HRESULT hr = o_DrawPrimitiveUP(dev, t, n, data, stride);
    if (m) EndDraw(dev, m);
    return hr;
}

static HRESULT STDMETHODCALLTYPE hk_DrawIndexedPrimitiveUP(IDirect3DDevice9* dev, D3DPRIMITIVETYPE t, UINT mi, UINT nv,
                                                           UINT n, const void* idx, D3DFORMAT fmt,
                                                           const void* data, UINT stride)
{
    unsigned m = BeginDraw(dev);
    HRESULT hr = o_DrawIndexedPrimitiveUP(dev, t, mi, nv, n, idx, fmt, data, stride);
    if (m) EndDraw(dev, m);
    return hr;
}

static HRESULT STDMETHODCALLTYPE hk_CreatePixelShader(IDirect3DDevice9* dev, const DWORD* fn, IDirect3DPixelShader9** out)
{
    HRESULT hr = o_CreatePixelShader(dev, fn, out);
    if (SUCCEEDED(hr) && out && *out && IsWaterShaderBytecode(fn)) {
        g_waterPS = *out;
        Log("water pixel shader detected (%p)", *out);
    }
    return hr;
}

// Water vertex shader constants (POP.EXE @0x7f8a8c, uploaded as c12..c13):
//   c12 = REFRACT (ratio, ratio^2, x/y offset scale), c13 = (HALF, 0, ZOFFSET, ZMAX).
// The refraction offset is scaled by min(eye z, ZMAX) + ZOFFSET. The PC port
// uses ZOFFSET 1 / ZMAX 3, the Xbox (xemu capture, c[-65]) uses 3 / 10, which
// makes the PC refraction roughly 3x weaker. Restore the Xbox values, times an
// optional strength factor ([water] refraction in popfix.ini, default 1.5).
static const float kPcRefract[4] = { 0.6f, 0.36f, 0.04375f, 0.009375f };
static const float kXboxZOffset = 3.0f, kXboxZMax = 10.0f;
static float g_refractScale = 1.5f;
static bool g_loggedRefract;
static char g_iniPath[MAX_PATH];

static HRESULT STDMETHODCALLTYPE hk_SetVertexShaderConstantF(IDirect3DDevice9* dev, UINT start, const float* data,
                                                             UINT count)
{
    if (g_enabled && data && start <= 12 && start + count >= 14) {
        const float* c12 = data + (12 - start) * 4;
        if (memcmp(c12, kPcRefract, sizeof(kPcRefract)) == 0) {
            float buf[4 * 16];
            if (count <= 16) {
                memcpy(buf, data, count * 4 * sizeof(float));
                float* c13 = buf + (13 - start) * 4;
                if (!g_loggedRefract) {
                    g_loggedRefract = true;
                    Log("water refraction depth scale: ZOFFSET %.2f -> %.2f, ZMAX %.2f -> %.2f (Xbox x %.2f)",
                        c13[2], kXboxZOffset * g_refractScale, c13[3], kXboxZMax * g_refractScale,
                        g_refractScale);
                }
                c13[2] = kXboxZOffset * g_refractScale;
                c13[3] = kXboxZMax * g_refractScale;
                return o_SetVertexShaderConstantF(dev, start, buf, count);
            }
        }
    }
    return o_SetVertexShaderConstantF(dev, start, data, count);
}

static HRESULT STDMETHODCALLTYPE hk_SetPixelShader(IDirect3DDevice9* dev, IDirect3DPixelShader9* ps)
{
    g_curPS = ps;
    return o_SetPixelShader(dev, ps);
}

static void HookDevice(IDirect3DDevice9* dev, bool isEx)
{
    // All hooks share one set of originals, so only one device class is hooked.
    static void* hookedVT;
    void* vt = *(void**)dev;
    if (hookedVT && hookedVT != vt) {
        Log("device %p has a different vtable %p, not hooked", dev, vt);
        return;
    }
    hookedVT = vt;
#define HOOK(idx, name) { void* o = PatchVTable(dev, idx, (void*)hk_##name); if (o) o_##name = (name##_t)o; }
    HOOK(16, Reset)
    HOOK(17, Present)
    HOOK(23, CreateTexture)
    HOOK(34, StretchRect)
    HOOK(37, SetRenderTarget)
    HOOK(65, SetTexture)
    HOOK(69, SetSamplerState)
    HOOK(81, DrawPrimitive)
    HOOK(82, DrawIndexedPrimitive)
    HOOK(83, DrawPrimitiveUP)
    HOOK(84, DrawIndexedPrimitiveUP)
    HOOK(106, CreatePixelShader)
    HOOK(94, SetVertexShaderConstantF)
    HOOK(107, SetPixelShader)
    if (isEx) HOOK(132, ResetEx)
#undef HOOK
    g_devicesHooked++;
    Log("device %p hooked (ex=%d)", dev, isEx ? 1 : 0);
}

// ---------------------------------------------------------------- IDirect3D9 hooks
static HRESULT STDMETHODCALLTYPE hk_CreateDevice(IDirect3D9* d3d, UINT a, D3DDEVTYPE t, HWND w, DWORD f,
                                                 D3DPRESENT_PARAMETERS* pp, IDirect3DDevice9** out)
{
    HRESULT hr = o_CreateDevice(d3d, a, t, w, f, pp, out);
    Log("CreateDevice(flags 0x%lx, %ux%u) -> 0x%08lx", f, pp ? pp->BackBufferWidth : 0, pp ? pp->BackBufferHeight : 0, hr);
    if (SUCCEEDED(hr)) {
        g_gameWindow = pp && pp->hDeviceWindow ? pp->hDeviceWindow : w;
        MenuPad_SetWindow(g_gameWindow);
    }
    if (SUCCEEDED(hr) && out && *out) {
        ReleaseShadows();
        g_waterPS = nullptr;
        HookDevice(*out, false);
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE hk_CreateDeviceEx(IDirect3D9Ex* d3d, UINT a, D3DDEVTYPE t, HWND w, DWORD f,
                                                   D3DPRESENT_PARAMETERS* pp, D3DDISPLAYMODEEX* fm,
                                                   IDirect3DDevice9Ex** out)
{
    HRESULT hr = o_CreateDeviceEx(d3d, a, t, w, f, pp, fm, out);
    Log("CreateDeviceEx(flags 0x%lx) -> 0x%08lx", f, hr);
    if (SUCCEEDED(hr) && out && *out) {
        ReleaseShadows();
        g_waterPS = nullptr;
        HookDevice(*out, true);
    }
    return hr;
}

// Fallback: sometimes CreateDevice of the object GOG's wrapper hands to the game
// no longer runs through our system vtable hook (slot 16 overwritten after we
// patched it, or a wrapper object with its own vtable). The object returned to the
// game is hooked as well; its hook only acts if the system hook did not.
static CreateDevice_t o_CreateDeviceOuter;

static HRESULT STDMETHODCALLTYPE hk_CreateDeviceOuter(IDirect3D9* d3d, UINT a, D3DDEVTYPE t, HWND w, DWORD f,
                                                      D3DPRESENT_PARAMETERS* pp, IDirect3DDevice9** out)
{
    int before = g_devicesHooked;
    HRESULT hr = o_CreateDeviceOuter(d3d, a, t, w, f, pp, out);
    if (SUCCEEDED(hr) && out && *out && g_devicesHooked == before) {
        Log("CreateDevice (outer, %ux%u) -> 0x%08lx, system hook was bypassed", pp ? pp->BackBufferWidth : 0,
            pp ? pp->BackBufferHeight : 0, hr);
        g_gameWindow = pp && pp->hDeviceWindow ? pp->hDeviceWindow : w;
        MenuPad_SetWindow(g_gameWindow);
        ReleaseShadows();
        g_waterPS = nullptr;
        HookDevice(*out, false);
    }
    return hr;
}

static void HookOuterD3D(IDirect3D9* d3d)
{
    if (!d3d) return;
    void** vt = *(void***)d3d;
    if (vt[16] == (void*)hk_CreateDevice || vt[16] == (void*)hk_CreateDeviceOuter) return;
    void* o = PatchVTable(d3d, 16, (void*)hk_CreateDeviceOuter);
    if (o) o_CreateDeviceOuter = (CreateDevice_t)o;
    Log("game IDirect3D9 %p: CreateDevice %p is not our hook, hooked outer vtable %p", d3d, o, vt);
}

// IDirect3D9 and IDirect3D9Ex may use different vtables, so each class only gets
// the entry point that is specific to it (one original pointer per hook).
static void HookD3D(IDirect3D9* d3d, bool isEx)
{
    void** vt = *(void***)d3d;
    void* o;
    if (isEx) {
        o = PatchVTable(d3d, 20, (void*)hk_CreateDeviceEx);
        if (o) o_CreateDeviceEx = (CreateDeviceEx_t)o;
    } else {
        o = PatchVTable(d3d, 16, (void*)hk_CreateDevice);
        if (o) o_CreateDevice = (CreateDevice_t)o;
    }
    Log("hooked system IDirect3D9%s vtable %p", isEx ? "Ex" : "", vt);
}

// Patch the shared vtables of the system IDirect3D9 / IDirect3D9Ex classes once.
static void EnsureSystemHooks()
{
    static bool done;
    if (done) return;
    done = true;
    if (p_sysCreate9) {
        IDirect3D9* t = ((IDirect3D9 * (WINAPI*)(UINT))p_sysCreate9)(D3D_SDK_VERSION);
        if (t) { HookD3D(t, false); t->Release(); }
    }
    if (p_sysCreate9Ex) {
        IDirect3D9Ex* t = nullptr;
        if (SUCCEEDED(((HRESULT(WINAPI*)(UINT, IDirect3D9Ex**))p_sysCreate9Ex)(D3D_SDK_VERSION, &t)) && t) {
            HookD3D(t, true);
            t->Release();
        }
    }
}

extern "C" IDirect3D9* WINAPI Proxy_Direct3DCreate9(UINT sdk)
{
    EnsureSystemHooks();
    Trace_Install();
    ConsoleMenu_Enable();
    Gamepad_Install();
    MenuPad_Install();
    IDirect3D9* d3d = ((IDirect3D9 * (WINAPI*)(UINT))p_Direct3DCreate9)(sdk);
    Log("Direct3DCreate9(%u) -> %p", sdk, d3d);
    HookOuterD3D(d3d);
    return d3d;
}

extern "C" HRESULT WINAPI Proxy_Direct3DCreate9Ex(UINT sdk, IDirect3D9Ex** out)
{
    EnsureSystemHooks();
    Trace_Install();
    ConsoleMenu_Enable();
    Gamepad_Install();
    MenuPad_Install();
    HRESULT hr = ((HRESULT(WINAPI*)(UINT, IDirect3D9Ex**))p_Direct3DCreate9Ex)(sdk, out);
    Log("Direct3DCreate9Ex(%u) -> 0x%08lx", sdk, hr);
    return hr;
}

extern "C" IDirect3D9* WINAPI Proxy_Direct3DCreate9On12(UINT sdk, void* args, UINT n)
{
    EnsureSystemHooks();
    Trace_Install();
    ConsoleMenu_Enable();
    Gamepad_Install();
    MenuPad_Install();
    IDirect3D9* d3d = ((IDirect3D9 * (WINAPI*)(UINT, void*, UINT))p_Direct3DCreate9On12)(sdk, args, n);
    Log("Direct3DCreate9On12(%u) -> %p", sdk, d3d);
    return d3d;
}

extern "C" HRESULT WINAPI Proxy_Direct3DCreate9On12Ex(UINT sdk, void* args, UINT n, IDirect3D9Ex** out)
{
    EnsureSystemHooks();
    Trace_Install();
    ConsoleMenu_Enable();
    Gamepad_Install();
    MenuPad_Install();
    HRESULT hr = ((HRESULT(WINAPI*)(UINT, void*, UINT, IDirect3D9Ex**))p_Direct3DCreate9On12Ex)(sdk, args, n, out);
    Log("Direct3DCreate9On12Ex(%u) -> 0x%08lx", sdk, hr);
    return hr;
}

// ---------------------------------------------------------------- entry
BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        char path[MAX_PATH];
        GetModuleFileNameA(inst, path, MAX_PATH);
        char* slash = strrchr(path, '\\');
        if (slash) strcpy(slash + 1, "popfix.log");
        g_log = fopen(path, "a");
        SYSTEMTIME t;
        GetLocalTime(&t);
        Log("\n=== PoP water fix (dx.dll) loaded, pid %lu, %02d:%02d:%02d ===",
            GetCurrentProcessId(), t.wHour, t.wMinute, t.wSecond);
        AddVectoredExceptionHandler(0, CrashLogger);

        if (slash) {
            strcpy(slash + 1, "popfix.ini");
            strcpy(g_iniPath, path);
            char v[32];
            GetPrivateProfileStringA("water", "refraction", "1.5", v, sizeof(v), g_iniPath);
            float f = (float)atof(v);
            if (f > 0.0f && f <= 10.0f) g_refractScale = f;
        }
        if (slash) strcpy(slash + 1, "dx_gog.dll");
        g_gog = LoadLibraryA(path);
        if (!g_gog) { Log("failed to load %s (error %lu)", path, GetLastError()); return FALSE; }
#define RESOLVE(name) p_##name = GetProcAddress(g_gog, #name);
        REAL_EXPORTS(RESOLVE)
#undef RESOLVE

        GetSystemDirectoryA(path, MAX_PATH);  // SysWOW64 for this 32-bit process
        strcat(path, "\\d3d9.dll");
        g_sys = LoadLibraryA(path);
        if (!g_sys) { Log("failed to load %s", path); return FALSE; }
        p_sysCreate9 = GetProcAddress(g_sys, "Direct3DCreate9");
        p_sysCreate9Ex = GetProcAddress(g_sys, "Direct3DCreate9Ex");
    } else if (reason == DLL_PROCESS_DETACH) {
        Gamepad_Shutdown();
        if (g_log) fclose(g_log);
    }
    return TRUE;
}
