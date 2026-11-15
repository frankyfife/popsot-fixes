#pragma once
#include <d3d9.h>

// Picture for Windows' HDR mode (see hdr.cpp).
struct HdrDevice {  // the device functions without our hooks in between
    HRESULT(STDMETHODCALLTYPE* CreateTexture)(IDirect3DDevice9*, UINT, UINT, UINT, DWORD, D3DFORMAT, D3DPOOL,
                                              IDirect3DTexture9**, HANDLE*);
    HRESULT(STDMETHODCALLTYPE* CreatePixelShader)(IDirect3DDevice9*, const DWORD*, IDirect3DPixelShader9**);
    HRESULT(STDMETHODCALLTYPE* StretchRect)(IDirect3DDevice9*, IDirect3DSurface9*, const RECT*, IDirect3DSurface9*,
                                            const RECT*, D3DTEXTUREFILTERTYPE);
    HRESULT(STDMETHODCALLTYPE* SetRenderTarget)(IDirect3DDevice9*, DWORD, IDirect3DSurface9*);
    HRESULT(STDMETHODCALLTYPE* SetViewport)(IDirect3DDevice9*, const D3DVIEWPORT9*);
    HRESULT(STDMETHODCALLTYPE* SetTexture)(IDirect3DDevice9*, DWORD, IDirect3DBaseTexture9*);
    HRESULT(STDMETHODCALLTYPE* SetSamplerState)(IDirect3DDevice9*, DWORD, D3DSAMPLERSTATETYPE, DWORD);
    HRESULT(STDMETHODCALLTYPE* SetFVF)(IDirect3DDevice9*, DWORD);
    HRESULT(STDMETHODCALLTYPE* SetVertexShader)(IDirect3DDevice9*, IDirect3DVertexShader9*);
    HRESULT(STDMETHODCALLTYPE* SetPixelShader)(IDirect3DDevice9*, IDirect3DPixelShader9*);
    HRESULT(STDMETHODCALLTYPE* DrawPrimitiveUP)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, const void*, UINT);
};

void Hdr_Configure(int mode, float whiteNits);  // [video] hdr (0 off, 1 when Windows' HDR is on), hdr_white
void Hdr_SetWindow(HWND w);                     // the game window, checked for HDR at once
void Hdr_OnPresent(IDirect3DDevice9* dev, const HdrDevice& fn);  // before Present: the final pass
void Hdr_OnReset();                             // releases the device's resources (Reset)
void Hdr_Release();                             // releases everything, before a new device is created
