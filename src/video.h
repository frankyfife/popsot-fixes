#pragma once
#include <windows.h>
#include <d3d9.h>

// High-resolution replacement videos (see video.cpp).
void Video_Install();
// Called from CreateTexture: enlarges the video texture for a replacement.
void Video_AdjustTexture(UINT& w, UINT& h, DWORD usage, D3DFORMAT fmt);
void Video_TextureCreated(IDirect3DBaseTexture9* tex);
// True for the texture a replacement video is drawn from (filtered linearly).
bool Video_IsReplacementTexture(IDirect3DBaseTexture9* tex);
