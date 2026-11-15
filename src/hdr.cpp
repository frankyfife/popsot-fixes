// Picture for Windows' HDR mode.
//
// The game renders and presents an 8-bit SDR picture made for a gamma 2.2
// display. With "Use HDR" on, Windows composes SDR windows into the HDR output
// itself: it decodes them with the piecewise sRGB curve and puts white at the
// "SDR content brightness" of the display settings, which is often far above
// what an SDR monitor shows. Against a gamma 2.2 display the sRGB curve lifts
// the dark tones, and every light source, glow and bright sky shines at that
// white.
//
// Fix: before each Present, the back buffer goes through one more pass
// (copied to a texture, drawn back with a pixel shader): decoded with gamma
// 2.2, scaled from Windows' SDR white to [video] hdr_white nits, encoded with
// the sRGB curve again. Windows' decoding then gives the game's tones at the
// chosen brightness. Brighter than Windows' own SDR white is not possible in
// an SDR picture; that is clamped. The HDR state and the SDR white level of
// the game window's display come from the display configuration
// (DISPLAYCONFIG_DEVICE_INFO_GET_ADVANCED_COLOR_INFO / _GET_SDR_WHITE_LEVEL)
// and are checked again every few seconds, so a change in the display
// settings applies while the game runs.

#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <d3d9.h>
#include <d3dcommon.h>
#include <string.h>
#include "hdr.h"

void Log(const char* fmt, ...);

namespace {

int g_mode = 1;           // [video] hdr
float g_white = 200.0f;   // [video] hdr_white, nits; 0 = Windows' SDR white
HWND g_window;
bool g_hdr;               // the window's display runs in HDR mode
float g_sdrWhite = 80.0f; // Windows' SDR white on that display, nits
DWORD g_checked;          // tick of the last check

IDirect3DTexture9* g_copy;
UINT g_copyW, g_copyH;
D3DFORMAT g_copyFmt;
IDirect3DPixelShader9* g_ps;
IDirect3DStateBlock9* g_state;
bool g_failed;  // no shader compiler: the pass stays off

const char kShader[] =
    "sampler2D s : register(s0);\n"
    "float4 k : register(c0);\n"  // x: hdr_white / Windows' SDR white
    "float4 main(float2 uv : TEXCOORD0) : COLOR0 {\n"
    "    float3 c = tex2D(s, uv).rgb;\n"
    "    float3 lin = pow(max(c, 1e-6), 2.2) * k.x;\n"
    "    float3 hi = 1.055 * pow(max(lin, 1e-6), 1.0 / 2.4) - 0.055;\n"
    "    float3 v = lerp(lin * 12.92, hi, step(0.0031308, lin));\n"
    "    return float4(saturate(v), 1);\n"
    "}\n";

// HDR state and SDR white level of the display showing the window.
void Check()
{
    g_checked = GetTickCount();
    bool hdr = false;
    float white = 80.0f;
    MONITORINFOEXW mi = {};
    mi.cbSize = sizeof(mi);
    if (!g_window || !GetMonitorInfoW(MonitorFromWindow(g_window, MONITOR_DEFAULTTONEAREST), &mi)) return;
    UINT32 nPaths = 0, nModes = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &nPaths, &nModes) != ERROR_SUCCESS || !nPaths) return;
    DISPLAYCONFIG_PATH_INFO* paths = new DISPLAYCONFIG_PATH_INFO[nPaths];
    DISPLAYCONFIG_MODE_INFO* modes = new DISPLAYCONFIG_MODE_INFO[nModes ? nModes : 1];
    if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &nPaths, paths, &nModes, modes, nullptr) == ERROR_SUCCESS) {
        for (UINT32 i = 0; i < nPaths; i++) {
            DISPLAYCONFIG_SOURCE_DEVICE_NAME src = {};
            src.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
            src.header.size = sizeof(src);
            src.header.adapterId = paths[i].sourceInfo.adapterId;
            src.header.id = paths[i].sourceInfo.id;
            if (DisplayConfigGetDeviceInfo(&src.header) != ERROR_SUCCESS || wcscmp(src.viewGdiDeviceName, mi.szDevice))
                continue;
            DISPLAYCONFIG_GET_ADVANCED_COLOR_INFO color = {};
            color.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_ADVANCED_COLOR_INFO;
            color.header.size = sizeof(color);
            color.header.adapterId = paths[i].targetInfo.adapterId;
            color.header.id = paths[i].targetInfo.id;
            // HDR, not the colour management of an SDR display, which also sets advancedColorEnabled
            if (DisplayConfigGetDeviceInfo(&color.header) == ERROR_SUCCESS)
                hdr = color.advancedColorEnabled && !color.wideColorEnforced;
            DISPLAYCONFIG_SDR_WHITE_LEVEL level = {};
            level.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SDR_WHITE_LEVEL;
            level.header.size = sizeof(level);
            level.header.adapterId = paths[i].targetInfo.adapterId;
            level.header.id = paths[i].targetInfo.id;
            if (DisplayConfigGetDeviceInfo(&level.header) == ERROR_SUCCESS && level.SDRWhiteLevel)
                white = level.SDRWhiteLevel / 1000.0f * 80.0f;  // 1000 = 80 nits
            break;
        }
    }
    delete[] paths;
    delete[] modes;
    if (hdr != g_hdr || (hdr && (white < g_sdrWhite - 0.5f || white > g_sdrWhite + 0.5f))) {
        if (hdr)
            Log("hdr: display in HDR mode, SDR white %.0f nits, game white %.0f nits%s", white,
                g_white > 0.0f && g_white < white ? g_white : white,
                g_white > white ? " (hdr_white above Windows' SDR white)" : "");
        else
            Log("hdr: display in SDR mode, no pass");
    }
    g_hdr = hdr;
    g_sdrWhite = white;
}

bool MakeShader(IDirect3DDevice9* dev, const HdrDevice& fn)
{
    typedef HRESULT(WINAPI * Compile_t)(const void*, SIZE_T, const char*, const void*, void*, const char*, const char*,
                                        UINT, UINT, ID3DBlob**, ID3DBlob**);
    HMODULE dll = LoadLibraryA("d3dcompiler_47.dll");
    Compile_t compile = dll ? (Compile_t)GetProcAddress(dll, "D3DCompile") : nullptr;
    if (!compile) {
        Log("hdr: d3dcompiler_47.dll not found, pass off");
        return false;
    }
    ID3DBlob* code = nullptr;
    ID3DBlob* errors = nullptr;
    HRESULT hr = compile(kShader, sizeof(kShader) - 1, "hdr", nullptr, nullptr, "main", "ps_2_0", 0, 0, &code, &errors);
    if (FAILED(hr)) {
        Log("hdr: shader error %s", errors ? (const char*)errors->GetBufferPointer() : "");
        if (errors) errors->Release();
        return false;
    }
    hr = fn.CreatePixelShader(dev, (const DWORD*)code->GetBufferPointer(), &g_ps);
    code->Release();
    if (errors) errors->Release();
    return SUCCEEDED(hr);
}

}  // namespace

void Hdr_Configure(int mode, float whiteNits)
{
    g_mode = mode;
    g_white = whiteNits < 0.0f ? 0.0f : whiteNits;
}

void Hdr_SetWindow(HWND w)
{
    g_window = w;
    if (g_mode) Check();
}

void Hdr_OnReset()
{
    if (g_copy) g_copy->Release();
    if (g_state) g_state->Release();
    g_copy = nullptr;
    g_state = nullptr;
}

void Hdr_Release()
{
    Hdr_OnReset();
    if (g_ps) g_ps->Release();
    g_ps = nullptr;
    g_failed = false;
}

void Hdr_OnPresent(IDirect3DDevice9* dev, const HdrDevice& fn)
{
    if (!g_mode || g_failed) return;
    if (GetTickCount() - g_checked > 3000) Check();
    if (!g_hdr) return;
    float k = g_white > 0.0f && g_white < g_sdrWhite ? g_white / g_sdrWhite : 1.0f;

    IDirect3DSurface9* bb = nullptr;
    if (FAILED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) || !bb) return;
    D3DSURFACE_DESC d;
    bb->GetDesc(&d);
    if (!g_ps && !MakeShader(dev, fn)) {
        g_failed = true;
        bb->Release();
        return;
    }
    if (g_copy && (g_copyW != d.Width || g_copyH != d.Height || g_copyFmt != d.Format)) {
        g_copy->Release();
        g_copy = nullptr;
    }
    if (!g_copy) {
        if (FAILED(fn.CreateTexture(dev, d.Width, d.Height, 1, D3DUSAGE_RENDERTARGET, d.Format, D3DPOOL_DEFAULT, &g_copy,
                                    nullptr))) {
            Log("hdr: no %ux%u copy texture, pass off", d.Width, d.Height);
            g_failed = true;
            bb->Release();
            return;
        }
        g_copyW = d.Width;
        g_copyH = d.Height;
        g_copyFmt = d.Format;
    }
    if (!g_state && FAILED(dev->CreateStateBlock(D3DSBT_ALL, &g_state))) {
        bb->Release();
        return;
    }
    IDirect3DSurface9* copy = nullptr;
    g_copy->GetSurfaceLevel(0, &copy);
    fn.StretchRect(dev, bb, nullptr, copy, nullptr, D3DTEXF_NONE);
    copy->Release();

    // everything the pass changes is put back: device state by the state block,
    // render target and depth buffer (not part of it) by hand
    g_state->Capture();
    IDirect3DSurface9* rt = nullptr;
    IDirect3DSurface9* ds = nullptr;
    dev->GetRenderTarget(0, &rt);
    dev->GetDepthStencilSurface(&ds);
    fn.SetRenderTarget(dev, 0, bb);
    dev->SetDepthStencilSurface(nullptr);
    D3DVIEWPORT9 vp = { 0, 0, d.Width, d.Height, 0.0f, 1.0f };
    fn.SetViewport(dev, &vp);
    static const D3DRENDERSTATETYPE off[] = { D3DRS_ZENABLE, D3DRS_ALPHABLENDENABLE, D3DRS_ALPHATESTENABLE,
                                               D3DRS_FOGENABLE, D3DRS_STENCILENABLE, D3DRS_SCISSORTESTENABLE,
                                               D3DRS_SRGBWRITEENABLE, D3DRS_LIGHTING, D3DRS_CLIPPLANEENABLE };
    for (D3DRENDERSTATETYPE s : off) dev->SetRenderState(s, FALSE);
    dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
    dev->SetRenderState(D3DRS_FILLMODE, D3DFILL_SOLID);
    fn.SetVertexShader(dev, nullptr);
    fn.SetPixelShader(dev, g_ps);
    fn.SetFVF(dev, D3DFVF_XYZRHW | D3DFVF_TEX1);
    fn.SetTexture(dev, 0, g_copy);
    fn.SetSamplerState(dev, 0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
    fn.SetSamplerState(dev, 0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
    fn.SetSamplerState(dev, 0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
    fn.SetSamplerState(dev, 0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    fn.SetSamplerState(dev, 0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    fn.SetSamplerState(dev, 0, D3DSAMP_SRGBTEXTURE, FALSE);
    float c0[4] = { k, 0.0f, 0.0f, 0.0f };
    dev->SetPixelShaderConstantF(0, c0, 1);
    float w = (float)d.Width - 0.5f, h = (float)d.Height - 0.5f;
    struct V { float x, y, z, rhw, u, v; } quad[4] = {
        { -0.5f, -0.5f, 0.0f, 1.0f, 0.0f, 0.0f }, { w, -0.5f, 0.0f, 1.0f, 1.0f, 0.0f },
        { -0.5f, h, 0.0f, 1.0f, 0.0f, 1.0f },     { w, h, 0.0f, 1.0f, 1.0f, 1.0f },
    };
    if (SUCCEEDED(dev->BeginScene())) {
        fn.DrawPrimitiveUP(dev, D3DPT_TRIANGLESTRIP, 2, quad, sizeof(V));
        dev->EndScene();
    }
    // the targets first: SetRenderTarget resets the viewport, which Apply puts back
    fn.SetRenderTarget(dev, 0, rt);
    dev->SetDepthStencilSurface(ds);
    g_state->Apply();
    if (rt) rt->Release();
    if (ds) ds->Release();
    bb->Release();
}
