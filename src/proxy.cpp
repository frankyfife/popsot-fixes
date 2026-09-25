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
#include "menucam.h"
#include "ui.h"
#include "sound.h"
#include "video.h"

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
typedef HRESULT(STDMETHODCALLTYPE* SetViewport_t)(IDirect3DDevice9*, const D3DVIEWPORT9*);
typedef HRESULT(STDMETHODCALLTYPE* SetFVF_t)(IDirect3DDevice9*, DWORD);
typedef HRESULT(STDMETHODCALLTYPE* SetVertexShader_t)(IDirect3DDevice9*, IDirect3DVertexShader9*);
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
static UINT g_bbHeight;  // backbuffer height of the game device
static UINT g_bigRT;     // size of the enlarged blur targets (0 = off), see "post-effect resolution"
static int g_numBig;
static UINT g_bigRTMaxFactor = 4;  // [post] blur_resolution in popfix.ini (1 = off)
static float g_blurRadius;          // [post] blur_radius: tap distance relative to the original, 0 = auto

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
static int g_dumps;  // render targets written by the current F8 capture
extern char g_iniPath[MAX_PATH];
static bool g_verbose;  // [debug] verbose: menu tracing and per-frame statistics in popfix.log

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
        if (Video_IsReplacementTexture(t)) {  // high-resolution video: filter it
            if (g_magFilter[s] != D3DTEXF_LINEAR || g_minFilter[s] != D3DTEXF_LINEAR) {
                o_SetSamplerState(dev, s, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
                o_SetSamplerState(dev, s, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
                mask |= 1u << s;
            }
            continue;
        }
        UINT w, h;
        if (!RenderTargetTextureSize(t, w, h)) continue;
        if (w > g_curRTWidth) {
            // Downsampling a render target (the glow chain 0x670d70: screen -> 256
            // -> 128 ... -> 8). With point sampling every 2:1 step reads exactly on
            // a texel border and picks one side, so each level shifts by half a
            // texel; over five levels the glow ends up far up-left of its source.
            // Linear filtering averages both sides and keeps it centred.
            if (!g_enabled || (g_magFilter[s] != D3DTEXF_POINT && g_minFilter[s] != D3DTEXF_POINT)) continue;
            o_SetSamplerState(dev, s, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
            o_SetSamplerState(dev, s, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
            mask |= 1u << s;
            continue;
        }
        if (w == g_curRTWidth) continue;  // not an upscaled buffer
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
    g_numBig = 0;
    g_bigRT = 0;
    if (pp) g_bbHeight = pp->BackBufferHeight;
    return o_Reset(dev, pp);
}

static HRESULT STDMETHODCALLTYPE hk_ResetEx(IDirect3DDevice9Ex* dev, D3DPRESENT_PARAMETERS* pp, D3DDISPLAYMODEEX* fm)
{
    ReleaseShadows();
    return o_ResetEx(dev, pp, fm);
}

// ---------------------------------------------------------------- post-effect resolution
// Full-screen blur effects (zoom/speed blur) copy the frame into 512x512 render
// targets, blur it there and stretch it back over the screen - blocky at 4K,
// and the shadow copies above do not help because the blur renders into these
// targets. We create them larger (512 * k, k = backbuffer height / 512, max 4).
// The passes find their size from the surface (0x66b540), except the quad helper
// 0x66b300, which takes pixel coordinates: scale those while such a target is set.
static const int kMaxBig = 8;
static IDirect3DTexture9* g_bigTex[kMaxBig];
extern "C" float g_quadScale = 1.0f;       // read by the quad hook
extern "C" void* g_quadOriginal = nullptr;  // trampoline to 0x66b300

static bool IsBigRT(IDirect3DSurface9* surf)
{
    if (!surf || !g_numBig) return false;
    IDirect3DTexture9* tex = nullptr;
    if (FAILED(surf->GetContainer(__uuidof(IDirect3DTexture9), (void**)&tex)) || !tex) return false;
    tex->Release();
    for (int i = 0; i < g_numBig; i++)
        if (g_bigTex[i] == tex) return true;
    return false;
}

static int g_capture;  // > 0 while an F8 frame capture runs (see below)

// quad(this, x0, y0, x1, y1, ...): scale the corners if the current target is
// enlarged and they are still in 512 space.
extern "C" void __cdecl QuadAdjust(float* c)
{
    float s = g_quadScale;
    bool scaled = s != 1.0f && c[2] <= 513.0f && c[3] <= 513.0f;
    if (g_capture) {
        Log("cap: quad %.1f,%.1f - %.1f,%.1f%s", c[0], c[1], c[2], c[3], scaled ? " (scaled)" : "");
        // c[4] colour, then four texture coordinate sets of (u0, v0, u1, v1)
        Log("cap:   uv %.5f %.5f %.5f %.5f | %.5f %.5f %.5f %.5f | %.5f %.5f %.5f %.5f | %.5f %.5f %.5f %.5f",
            c[5], c[6], c[7], c[8], c[9], c[10], c[11], c[12], c[13], c[14], c[15], c[16], c[17], c[18], c[19], c[20]);
    }
    if (scaled)
        for (int i = 0; i < 4; i++) c[i] *= s;
}

extern "C" __declspec(naked) void QuadHook()
{
    __asm {
        push ecx                  // this
        lea eax, [esp + 8]        // first stack argument
        push eax
        call QuadAdjust
        add esp, 4
        pop ecx
        jmp dword ptr [g_quadOriginal]
    }
}

// Blur pass 0x66b540 (thiscall): dst texture, dst w, dst h (float pixels), src
// texture, src w, src h, kernel, taps, ?, u extent, v extent. The quad comes
// from the dst size; the texture coordinates span the u/v extent (a fraction of
// the source, e.g. 0.5), and the tap step is extent / src size. Only the dst size
// is scaled: extents are fractions and stay valid, and keeping the src size keeps
// the tap step - the blur radius - exactly as before.
extern "C" void* g_blurOriginal = nullptr;

static bool IsBigTex(void* tex)
{
    for (int i = 0; i < g_numBig; i++)
        if (g_bigTex[i] == tex) return true;
    return false;
}

extern "C" void __cdecl BlurAdjust(DWORD* a)  // a[0] = first argument
{
    float s = g_bigRT / 512.0f;
    float* f = (float*)a;
    bool dst = g_bigRT && IsBigTex((void*)a[0]) && f[1] <= 513.0f && f[2] <= 513.0f;
    if (g_capture)
        Log("cap: blur %.0fx%.0f <- %.0fx%.0f extents %.3f %.3f%s", f[1], f[2], f[4], f[5], f[9], f[10],
            dst ? " (dst scaled)" : "");
    if (dst) { f[1] *= s; f[2] *= s; }
    // Tap distance: the pass steps extent / srcSize in texture space, i.e. one
    // texel of the original 512 target, however large the target really is. An
    // upscaling emulator steps one texel of its enlarged target instead, which
    // keeps the soft-focus overlay tight (the original's wide halo, slightly
    // up-left of every object, is what reads as ghosting). blur_radius scales
    // the step: 1 = original, auto = one texel of the enlarged target.
    if (dst && IsBigTex((void*)a[3])) {
        float r = g_blurRadius > 0.0f ? g_blurRadius : 1.0f / s;
        f[4] /= r;
        f[5] /= r;
    }
}

extern "C" __declspec(naked) void BlurHook()
{
    __asm {
        push ecx
        lea eax, [esp + 8]
        push eax
        call BlurAdjust
        add esp, 4
        pop ecx
        jmp dword ptr [g_blurOriginal]
    }
}

static void* JmpHook(DWORD addr, const unsigned char* prologue, size_t len, void* hook)
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

// The full-screen soft-focus effect (0x670710) blurs a copy of the frame
// (texture *(effect +4) +0x78) and lays it over the finished frame at 41 %. The
// copy is made before the glow is added, so on glowing surfaces the overlay
// shows the dark, unlit picture - a grey ghost inside every glow. With
// [post] blur_after_glow=1 the copy is refreshed from the current back buffer
// (glow included) right before the effect runs.
static bool g_blurAfterGlow = true;
static bool g_blurOff;  // Ctrl+F10
extern "C" void* g_blurEffectOriginal = nullptr;

extern "C" int __cdecl BlurEffectPre(BYTE* effect)
{
    if (g_blurOff) return 0;
    if (!g_blurAfterGlow || !g_enabled) return 1;
    __try {
        IDirect3DTexture9* tex = *(IDirect3DTexture9**)(*(BYTE**)(effect + 4) + 0x78);
        D3DSURFACE_DESC d;
        if (!tex || FAILED(tex->GetLevelDesc(0, &d)) || !(d.Usage & D3DUSAGE_RENDERTARGET)) return 1;
        IDirect3DDevice9* dev = nullptr;
        if (FAILED(tex->GetDevice(&dev))) return 1;
        IDirect3DSurface9 *rt = nullptr, *dst = nullptr;
        if (SUCCEEDED(dev->GetRenderTarget(0, &rt)) && SUCCEEDED(tex->GetSurfaceLevel(0, &dst))) {
            D3DSURFACE_DESC rd;
            rt->GetDesc(&rd);
            if (rd.Height == g_bbHeight) o_StretchRect(dev, rt, nullptr, dst, nullptr, D3DTEXF_LINEAR);
        }
        if (dst) dst->Release();
        if (rt) rt->Release();
        dev->Release();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return 1;
}

extern "C" __declspec(naked) void BlurEffectHook()
{
    __asm {
        push ecx
        push ecx
        call BlurEffectPre
        add esp, 4
        pop ecx
        test eax, eax
        jz skip
        jmp dword ptr [g_blurEffectOriginal]
    skip:
        ret
    }
}

static void InstallQuadHook()
{
    static bool done;
    if (done) return;
    done = true;
    static const unsigned char quadPrologue[] = { 0x56, 0x8B, 0xF1, 0x80, 0x3E, 0x00 };  // push esi; mov esi,ecx; cmp [esi],0
    static const unsigned char blurPrologue[] = { 0x81, 0xEC, 0x8C, 0x00, 0x00, 0x00 };  // sub esp, 0x8c
    if (memcmp((void*)0x0066b300, quadPrologue, sizeof(quadPrologue)) != 0 ||
        memcmp((void*)0x0066b540, blurPrologue, sizeof(blurPrologue)) != 0) {
        Log("post blur: unknown executable, not enabled");
        g_bigRTMaxFactor = 1;
        return;
    }
    g_quadOriginal = JmpHook(0x0066b300, quadPrologue, sizeof(quadPrologue), (void*)QuadHook);
    g_blurOriginal = JmpHook(0x0066b540, blurPrologue, sizeof(blurPrologue), (void*)BlurHook);
    Log("post blur: blur targets at %u x %u", g_bigRT, g_bigRT);
    static const unsigned char effectPrologue[] = { 0x83, 0xEC, 0x30, 0x53, 0x55 };  // sub esp,0x30; push ebx; push ebp
    g_blurEffectOriginal = JmpHook(0x00670710, effectPrologue, sizeof(effectPrologue), (void*)BlurEffectHook);
    if (g_blurEffectOriginal && g_blurAfterGlow) Log("post blur: soft focus uses the frame with glow");
}

// [post] blur=0: skip the full-screen blur effect (render method 0x670710 of the
// effect object with vtable 0x7b18e4; zoom/speed blur).
static void DisableBlurEffect()
{
    unsigned char* fn = (unsigned char*)0x00670710;
    static const unsigned char prologue[] = { 0x83, 0xEC, 0x30, 0x53, 0x55 };  // sub esp,0x30; push ebx; push ebp
    if (memcmp(fn, prologue, sizeof(prologue)) != 0) { Log("post blur: unknown executable, blur left on"); return; }
    DWORD prot;
    VirtualProtect(fn, 1, PAGE_EXECUTE_READWRITE, &prot);
    fn[0] = 0xC3;  // ret
    VirtualProtect(fn, 1, prot, &prot);
    FlushInstructionCache(GetCurrentProcess(), fn, 1);
    Log("post blur: full-screen blur effect disabled");
}

static void ScaleRect(const RECT* in, RECT& out, float s)
{
    out.left = (LONG)(in->left * s); out.top = (LONG)(in->top * s);
    out.right = (LONG)(in->right * s); out.bottom = (LONG)(in->bottom * s);
}

// ---------------------------------------------------------------- frame capture (F8; F11 is taken by the GOG overlay)
// Logs every render step of one frame, to analyse post effects.
static SetViewport_t o_SetViewport;
static SetFVF_t o_SetFVF;
static SetVertexShader_t o_SetVertexShader;
static DWORD g_curFVF;

static void TexDesc(IDirect3DBaseTexture9* t, char* out, size_t n)
{
    if (!t) { _snprintf(out, n, "-"); return; }
    D3DSURFACE_DESC d;
    if (t->GetType() == D3DRTYPE_TEXTURE && SUCCEEDED(((IDirect3DTexture9*)t)->GetLevelDesc(0, &d)))
        _snprintf(out, n, "%p %ux%u f%d%s", t, d.Width, d.Height, d.Format, (d.Usage & D3DUSAGE_RENDERTARGET) ? " RT" : "");
    else
        _snprintf(out, n, "%p", t);
}

static void CaptureDraw(const char* what, UINT count, const void* up, UINT stride)
{
    char t0[64], t1[64];
    TexDesc(g_boundTex[0], t0, sizeof(t0));
    TexDesc(g_boundTex[1], t1, sizeof(t1));
    Log("cap: %s n %u fvf %lx ps %p | t0 %s | t1 %s", what, count, g_curFVF, g_curPS, t0, t1);
    if (up && stride >= 16) {
        const float* v = (const float*)up;
        Log("cap:   v0 %.1f %.1f %.2f %.2f | %.3f %.3f %.3f %.3f", v[0], v[1], v[2], v[3], v[4], v[5],
            stride >= 32 ? v[6] : 0.0f, stride >= 32 ? v[7] : 0.0f);
    }
}

static HRESULT STDMETHODCALLTYPE hk_SetViewport(IDirect3DDevice9* dev, const D3DVIEWPORT9* vp)
{
    if (g_capture && vp) Log("cap: viewport %lu,%lu %lux%lu", vp->X, vp->Y, vp->Width, vp->Height);
    return o_SetViewport(dev, vp);
}

static HRESULT STDMETHODCALLTYPE hk_SetFVF(IDirect3DDevice9* dev, DWORD fvf)
{
    g_curFVF = fvf;
    return o_SetFVF(dev, fvf);
}

static HRESULT STDMETHODCALLTYPE hk_SetVertexShader(IDirect3DDevice9* dev, IDirect3DVertexShader9* vs)
{
    if (g_capture) Log("cap: vertex shader %p", vs);
    return o_SetVertexShader(dev, vs);
}

static HRESULT STDMETHODCALLTYPE hk_Present(IDirect3DDevice9* dev, const RECT* sr, const RECT* dr, HWND w,
                                            const RGNDATA* rgn)
{
    static unsigned frames;
    if (GetAsyncKeyState(VK_F10) & 1) {
        if (GetAsyncKeyState(VK_CONTROL) < 0) {
            // Ctrl+F10: the full-screen blur effect on / off, for comparison.
            unsigned char* fn = (unsigned char*)0x00670710;
            if (g_blurEffectOriginal) {
                g_blurOff = !g_blurOff;
                Log("Ctrl+F10: blur effect %s", g_blurOff ? "OFF" : "ON");
            } else if (fn[0] == 0x83 || fn[0] == 0xC3) {
                DWORD prot;
                VirtualProtect(fn, 1, PAGE_EXECUTE_READWRITE, &prot);
                fn[0] = fn[0] == 0xC3 ? 0x83 : 0xC3;
                VirtualProtect(fn, 1, prot, &prot);
                FlushInstructionCache(GetCurrentProcess(), fn, 1);
                Log("Ctrl+F10: blur effect %s", fn[0] == 0xC3 ? "OFF" : "ON");
            }
        } else {
            g_enabled = !g_enabled;
            Log("F10: fix %s", g_enabled ? "ON" : "OFF");
        }
    }
    if (g_capture > 0 && --g_capture == 0) Log("cap: ---- end of frame capture");
    if (GetAsyncKeyState(VK_F8) & 1) {
        g_capture = 2;  // the frame after this Present
        g_dumps = 0;
        Log("cap: ---- F8 frame capture");
    }
    if (++frames == 300) {
        if (g_verbose) {
            DumpStats(frames);
            Trace_Dump();
        }
        frames = 0;
    }
    MenuPad_OnPresent();
    MenuCam_OnPresent(!g_gameWindow || GetForegroundWindow() == g_gameWindow);
    Sound_OnFrame();
    Gamepad_OnFrame(g_gameWindow);
    if (g_verbose) Trace_ProbeMenuManager();
    return o_Present(dev, sr, dr, w, rgn);
}

static HRESULT STDMETHODCALLTYPE hk_CreateTexture(IDirect3DDevice9* dev, UINT w, UINT h, UINT levels, DWORD usage,
                                                  D3DFORMAT fmt, D3DPOOL pool, IDirect3DTexture9** out, HANDLE* sh)
{
    bool big = false;
    if ((usage & D3DUSAGE_RENDERTARGET) && w == 512 && h == 512 && fmt == D3DFMT_A8R8G8B8 && g_numBig < kMaxBig) {
        UINT k = g_bbHeight / 512;
        if (k > 4) k = 4;
        if (k > g_bigRTMaxFactor) k = g_bigRTMaxFactor;
        if (k > 1 && g_bigRT == 0) { g_bigRT = 512 * k; InstallQuadHook(); if (g_bigRTMaxFactor < 2) g_bigRT = 0; }
        if (g_bigRT) { w = h = g_bigRT; big = true; }
    }
    if (!big) Video_AdjustTexture(w, h, usage, fmt);
    HRESULT hr = o_CreateTexture(dev, w, h, levels, usage, fmt, pool, out, sh);
    if (big && SUCCEEDED(hr) && out && *out) g_bigTex[g_numBig++] = *out;
    if (SUCCEEDED(hr) && out && *out) Video_TextureCreated(*out);
    if (g_verbose && (usage & D3DUSAGE_RENDERTARGET))
        Log("game render-target texture %ux%u levels %u fmt %d -> %p (0x%08lx)", w, h, levels, fmt,
            out ? *out : nullptr, hr);
    return hr;
}

static HRESULT STDMETHODCALLTYPE hk_SetRenderTarget(IDirect3DDevice9* dev, DWORD idx, IDirect3DSurface9* surf)
{
    if (g_capture) {
        D3DSURFACE_DESC d = {};
        if (surf) surf->GetDesc(&d);
        Log("cap: render target %lu = %p %ux%u f%d", idx, surf, d.Width, d.Height, d.Format);
    }
    if (idx == 0) {
        g_curRTWidth = 0;
        D3DSURFACE_DESC d;
        if (surf && SUCCEEDED(surf->GetDesc(&d))) g_curRTWidth = d.Width;
    }
    if (surf) InvalidateShadowsFor(surf);  // the game is about to render into it
    if (idx == 0) g_quadScale = IsBigRT(surf) ? g_bigRT / 512.0f : 1.0f;
    return o_SetRenderTarget(dev, idx, surf);
}

static HRESULT STDMETHODCALLTYPE hk_StretchRect(IDirect3DDevice9* dev, IDirect3DSurface9* src, const RECT* sr,
                                                IDirect3DSurface9* dst, const RECT* dr, D3DTEXTUREFILTERTYPE f)
{
    if (g_capture) {
        D3DSURFACE_DESC a = {}, b = {};
        if (src) src->GetDesc(&a);
        if (dst) dst->GetDesc(&b);
        Log("cap: StretchRect %p %ux%u %s -> %p %ux%u %s filter %d", src, a.Width, a.Height, sr ? "rect" : "full", dst,
            b.Width, b.Height, dr ? "rect" : "full", f);
    }
    RECT srS, drS;
    if (sr && IsBigRT(src)) { ScaleRect(sr, srS, g_bigRT / 512.0f); sr = &srS; }
    if (dr && IsBigRT(dst)) { ScaleRect(dr, drS, g_bigRT / 512.0f); dr = &drS; }
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
    if (g_capture) CaptureDraw("DrawPrimitive", n, nullptr, 0);
    unsigned m = BeginDraw(dev);
    HRESULT hr = o_DrawPrimitive(dev, t, sv, n);
    if (m) EndDraw(dev, m);
    return hr;
}

// F8 capture with [debug] verbose=1: writes the enlarged blur targets after each
// draw into them as raw BGRA files (popfix_rt_<n>_<w>x<h>.raw) for analysis.
static void DumpRenderTarget(IDirect3DDevice9* dev)
{
    if (!g_verbose || g_dumps >= 40) return;
    IDirect3DSurface9* rt = nullptr;
    if (FAILED(dev->GetRenderTarget(0, &rt)) || !rt) return;
    D3DSURFACE_DESC d;
    rt->GetDesc(&d);
    IDirect3DSurface9* sys = nullptr;
    if (d.Width <= 2048 && d.Format == D3DFMT_A8R8G8B8 &&
        SUCCEEDED(dev->CreateOffscreenPlainSurface(d.Width, d.Height, d.Format, D3DPOOL_SYSTEMMEM, &sys, nullptr)) &&
        SUCCEEDED(dev->GetRenderTargetData(rt, sys))) {
        D3DLOCKED_RECT lr;
        if (SUCCEEDED(sys->LockRect(&lr, nullptr, D3DLOCK_READONLY))) {
            char path[MAX_PATH];
            strcpy(path, g_iniPath);
            char* slash = strrchr(path, '\\');
            if (slash) sprintf(slash + 1, "popfix_rt_%d_%ux%u.raw", g_dumps, d.Width, d.Height);
            if (FILE* f = fopen(path, "wb")) {
                for (UINT y = 0; y < d.Height; y++) fwrite((BYTE*)lr.pBits + y * lr.Pitch, 4, d.Width, f);
                fclose(f);
                Log("cap: render target %p dumped to %s", rt, path);
                g_dumps++;
            }
            sys->UnlockRect();
        }
    }
    if (sys) sys->Release();
    rt->Release();
}

static HRESULT STDMETHODCALLTYPE hk_DrawIndexedPrimitive(IDirect3DDevice9* dev, D3DPRIMITIVETYPE t, INT bv, UINT mi,
                                                         UINT nv, UINT si, UINT n)
{
    if (g_capture) CaptureDraw("DrawIndexedPrimitive", n, nullptr, 0);
    unsigned m = BeginDraw(dev);
    HRESULT hr = o_DrawIndexedPrimitive(dev, t, bv, mi, nv, si, n);
    if (m) EndDraw(dev, m);
    if (g_capture) DumpRenderTarget(dev);
    return hr;
}

static HRESULT STDMETHODCALLTYPE hk_DrawPrimitiveUP(IDirect3DDevice9* dev, D3DPRIMITIVETYPE t, UINT n,
                                                    const void* data, UINT stride)
{
    if (g_capture) CaptureDraw("DrawPrimitiveUP", n, data, stride);
    unsigned m = BeginDraw(dev);
    HRESULT hr = o_DrawPrimitiveUP(dev, t, n, data, stride);
    if (m) EndDraw(dev, m);
    return hr;
}

static HRESULT STDMETHODCALLTYPE hk_DrawIndexedPrimitiveUP(IDirect3DDevice9* dev, D3DPRIMITIVETYPE t, UINT mi, UINT nv,
                                                           UINT n, const void* idx, D3DFORMAT fmt,
                                                           const void* data, UINT stride)
{
    if (g_capture) CaptureDraw("DrawIndexedPrimitiveUP", n, data, stride);
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
static float g_menuCamForward = 3.5f;  // [menus] camera_forward / camera_up, see menucam.cpp
static float g_menuCamUp = 6.25f;
static float g_menuCamSide = 0.0f;
static bool g_uiAspect = true;  // [ui] aspect / scale, see ui.cpp
static float g_uiScale = 1.0f;
static char g_gameDir[MAX_PATH];  // with trailing backslash
static bool g_loggedRefract;
char g_iniPath[MAX_PATH];

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
    HOOK(47, SetViewport)
    HOOK(89, SetFVF)
    HOOK(92, SetVertexShader)
    if (isEx) HOOK(132, ResetEx)
#undef HOOK
    g_devicesHooked++;
    Log("device %p hooked (ex=%d)", dev, isEx ? 1 : 0);
}

// ---------------------------------------------------------------- IDirect3D9 hooks
static HRESULT STDMETHODCALLTYPE hk_CreateDevice(IDirect3D9* d3d, UINT a, D3DDEVTYPE t, HWND w, DWORD f,
                                                 D3DPRESENT_PARAMETERS* pp, IDirect3DDevice9** out)
{
    if (pp) g_bbHeight = pp->BackBufferHeight;
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
    if (pp) g_bbHeight = pp->BackBufferHeight;
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
    if (pp) g_bbHeight = pp->BackBufferHeight;
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
    if (g_verbose) Trace_Install();
    ConsoleMenu_Enable();
    Gamepad_Install();
    MenuPad_Install();
    MenuCam_Install(g_menuCamForward, g_menuCamUp, g_menuCamSide);
    Ui_Install(g_uiAspect, g_uiScale);
    Video_InstallCode();
    Sound_Install(g_gameDir);
    IDirect3D9* d3d = ((IDirect3D9 * (WINAPI*)(UINT))p_Direct3DCreate9)(sdk);
    Log("Direct3DCreate9(%u) -> %p", sdk, d3d);
    HookOuterD3D(d3d);
    return d3d;
}

extern "C" HRESULT WINAPI Proxy_Direct3DCreate9Ex(UINT sdk, IDirect3D9Ex** out)
{
    EnsureSystemHooks();
    if (g_verbose) Trace_Install();
    ConsoleMenu_Enable();
    Gamepad_Install();
    MenuPad_Install();
    MenuCam_Install(g_menuCamForward, g_menuCamUp, g_menuCamSide);
    Ui_Install(g_uiAspect, g_uiScale);
    Video_InstallCode();
    Sound_Install(g_gameDir);
    HRESULT hr = ((HRESULT(WINAPI*)(UINT, IDirect3D9Ex**))p_Direct3DCreate9Ex)(sdk, out);
    Log("Direct3DCreate9Ex(%u) -> 0x%08lx", sdk, hr);
    return hr;
}

extern "C" IDirect3D9* WINAPI Proxy_Direct3DCreate9On12(UINT sdk, void* args, UINT n)
{
    EnsureSystemHooks();
    if (g_verbose) Trace_Install();
    ConsoleMenu_Enable();
    Gamepad_Install();
    MenuPad_Install();
    MenuCam_Install(g_menuCamForward, g_menuCamUp, g_menuCamSide);
    Ui_Install(g_uiAspect, g_uiScale);
    Video_InstallCode();
    Sound_Install(g_gameDir);
    IDirect3D9* d3d = ((IDirect3D9 * (WINAPI*)(UINT, void*, UINT))p_Direct3DCreate9On12)(sdk, args, n);
    Log("Direct3DCreate9On12(%u) -> %p", sdk, d3d);
    return d3d;
}

extern "C" HRESULT WINAPI Proxy_Direct3DCreate9On12Ex(UINT sdk, void* args, UINT n, IDirect3D9Ex** out)
{
    EnsureSystemHooks();
    if (g_verbose) Trace_Install();
    ConsoleMenu_Enable();
    Gamepad_Install();
    MenuPad_Install();
    MenuCam_Install(g_menuCamForward, g_menuCamUp, g_menuCamSide);
    Ui_Install(g_uiAspect, g_uiScale);
    Video_InstallCode();
    Sound_Install(g_gameDir);
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
            g_verbose = GetPrivateProfileIntA("debug", "verbose", 0, g_iniPath) != 0;
            GetPrivateProfileStringA("water", "refraction", "1.5", v, sizeof(v), g_iniPath);
            float f = (float)atof(v);
            if (f > 0.0f && f <= 10.0f) g_refractScale = f;
            if (GetPrivateProfileIntA("post", "blur", 1, g_iniPath) == 0) DisableBlurEffect();
            GetPrivateProfileStringA("post", "blur_radius", "auto", v, sizeof(v), g_iniPath);
            g_blurRadius = _stricmp(v, "auto") == 0 ? 0.0f : (float)atof(v);
            g_blurAfterGlow = GetPrivateProfileIntA("post", "blur_after_glow", 1, g_iniPath) != 0;
            UINT k = GetPrivateProfileIntA("post", "blur_resolution", 4, g_iniPath);
            g_bigRTMaxFactor = k < 1 ? 1 : k > 4 ? 4 : k;
            GetPrivateProfileStringA("menus", "camera_forward", "3.5", v, sizeof(v), g_iniPath);
            g_menuCamForward = (float)atof(v);
            GetPrivateProfileStringA("menus", "camera_up", "6.25", v, sizeof(v), g_iniPath);
            g_menuCamUp = (float)atof(v);
            GetPrivateProfileStringA("menus", "camera_side", "0", v, sizeof(v), g_iniPath);
            g_menuCamSide = (float)atof(v);
            g_uiAspect = GetPrivateProfileIntA("ui", "aspect", 1, g_iniPath) != 0;
            GetPrivateProfileStringA("ui", "scale", "1", v, sizeof(v), g_iniPath);
            g_uiScale = (float)atof(v);
            GetPrivateProfileStringA("controller", "prompts", "auto", v, sizeof(v), g_iniPath);
            Gamepad_SetPromptMode(_stricmp(v, "controller") == 0 ? 1 : _stricmp(v, "keyboard") == 0 ? 2 : 0);
        }
        if (slash) {
            slash[1] = 0;
            strcpy(g_gameDir, path);
            Sound_SetForceEax(GetPrivateProfileIntA("sound", "eax", 1, g_iniPath) != 0);
            Sound_Install(g_gameDir);
            Video_Install(GetPrivateProfileIntA("video", "keep_aspect", 1, g_iniPath) != 0);
            strcpy(slash + 1, "dx_gog.dll");
        }
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
