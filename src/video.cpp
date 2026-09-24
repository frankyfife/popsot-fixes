// High-resolution replacements for the Bink videos.
//
// The videos (Video\*.int) are Bink 1 files of 640x448 (640x346 letterboxed for
// the cutscenes) with one audio track per language. The game plays them with
// binkw32.dll into a texture of the video's size, rounded up to a power of two
// (0x675140: BinkOpen, CreateTexture, UV scale in 0xaf44c0 / 0xaf44bc), and
// every frame copies the decoded picture into it (0x674c50: BinkDoFrame,
// BinkCopyToBuffer, BinkNextFrame). The quad it draws is placed from the Bink
// width and height (0x674d30), so those must stay as they are.
//
// When a file with the same name and the extension .mp4 (or .mov / .mkv) lies
// next to the .int file, Bink still plays the original - sound, timing and
// skipping stay untouched - but the picture comes from that file: the video
// texture is created at the replacement's size, the UV scale is adjusted, and
// BinkCopyToBuffer copies the replacement frame that matches the Bink frame's
// time, decoded with Media Foundation. The replacement may have any size up to
// 4096x4096 and any frame rate (e.g. an AI upscale with frame interpolation);
// it only has to have the same length.

#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <d3d9.h>
#include <stdio.h>
#include <string.h>
#include "video.h"
#include "imports.h"

void Log(const char* fmt, ...);

namespace {

const DWORD kUvScaleU = 0x00af44c0;  // float, video width / texture width
const DWORD kUvScaleV = 0x00af44bc;  // float, video height / texture height

typedef void*(WINAPI* BinkOpen_t)(const char* name, DWORD flags);
typedef int(WINAPI* BinkCopyToBuffer_t)(void* bink, void* dest, int pitch, DWORD height, DWORD x, DWORD y,
                                        DWORD flags);
typedef void(WINAPI* BinkClose_t)(void* bink);
BinkOpen_t g_binkOpen;
BinkCopyToBuffer_t g_binkCopyToBuffer;
BinkClose_t g_binkClose;

struct Replacement {
    void* bink;
    IMFSourceReader* reader;
    UINT width, height;
    LONG stride;         // of the decoded RGB32 frames, negative = bottom-up
    double binkFps;
    BYTE* frame;         // current picture, top-down, width * 4 per row
    bool haveFrame;
    IMFSample* next;     // decoded ahead
    LONGLONG nextTime;   // 100 ns units
    bool eof;
};
const int kMaxReplacements = 4;
Replacement g_rep[kMaxReplacements];
Replacement* g_pending;               // opened, waiting for its texture
IDirect3DBaseTexture9* g_videoTexture;  // texture of the current replacement
bool g_mfStarted;

Replacement* Find(void* bink)
{
    for (Replacement& r : g_rep)
        if (r.bink && r.bink == bink) return &r;
    return nullptr;
}

UINT Pow2(UINT v)
{
    UINT p = 1;
    while (p < v) p <<= 1;
    return p;
}

void Release(Replacement& r)
{
    if (r.next) r.next->Release();
    if (r.reader) r.reader->Release();
    delete[] r.frame;
    if (g_pending == &r) g_pending = nullptr;
    r = Replacement();
}

// Decodes the next sample into r.next (null at the end of the stream).
void ReadAhead(Replacement& r)
{
    if (r.next) { r.next->Release(); r.next = nullptr; }
    while (!r.eof) {
        DWORD stream, flags = 0;
        LONGLONG time = 0;
        IMFSample* sample = nullptr;
        HRESULT hr = r.reader->ReadSample((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, &stream, &flags, &time,
                                          &sample);
        if (FAILED(hr) || (flags & (MF_SOURCE_READERF_ENDOFSTREAM | MF_SOURCE_READERF_ERROR))) {
            if (sample) sample->Release();
            r.eof = true;
            return;
        }
        if (sample) {
            r.next = sample;
            r.nextTime = time;
            return;
        }
    }
}

// Copies r.next into r.frame (top-down).
bool TakeNext(Replacement& r)
{
    IMFMediaBuffer* buf = nullptr;
    if (FAILED(r.next->ConvertToContiguousBuffer(&buf))) return false;
    BYTE* p = nullptr;
    DWORD len = 0;
    bool ok = false;
    if (SUCCEEDED(buf->Lock(&p, nullptr, &len))) {
        UINT rowBytes = r.width * 4;
        UINT absStride = (UINT)(r.stride < 0 ? -r.stride : r.stride);
        if ((DWORD)absStride * r.height <= len) {
            for (UINT y = 0; y < r.height; y++) {
                const BYTE* src = p + (size_t)(r.stride < 0 ? r.height - 1 - y : y) * absStride;
                memcpy(r.frame + (size_t)y * rowBytes, src, rowBytes);
            }
            ok = true;
        }
        buf->Unlock();
    }
    buf->Release();
    return ok;
}

bool OpenReplacement(Replacement& r, const char* binkName)
{
    char path[MAX_PATH];
    const char* exts[] = { ".mp4", ".mov", ".mkv" };
    bool found = false;
    for (const char* ext : exts) {
        strncpy(path, binkName, MAX_PATH - 8);
        path[MAX_PATH - 8] = 0;
        char* dot = strrchr(path, '.');
        char* slash = strrchr(path, '\\');
        if (dot && (!slash || dot > slash)) *dot = 0;
        strcat(path, ext);
        if (GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES) { found = true; break; }
    }
    if (!found) return false;

    // Frame rate of the Bink file (header: rate at 0x1c, divider at 0x20).
    r.binkFps = 29.97;
    if (FILE* f = fopen(binkName, "rb")) {
        DWORD hdr[9] = {};
        if (fread(hdr, 4, 9, f) == 9 && hdr[8]) r.binkFps = (double)hdr[7] / hdr[8];
        fclose(f);
    }

    if (!g_mfStarted) {
        if (FAILED(MFStartup(MF_VERSION, MFSTARTUP_LITE))) { Log("video: Media Foundation not available"); return false; }
        g_mfStarted = true;
    }
    wchar_t wpath[MAX_PATH];
    MultiByteToWideChar(CP_ACP, 0, path, -1, wpath, MAX_PATH);
    IMFAttributes* attr = nullptr;
    MFCreateAttributes(&attr, 1);
    attr->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
    HRESULT hr = MFCreateSourceReaderFromURL(wpath, attr, &r.reader);
    attr->Release();
    if (FAILED(hr)) { Log("video: cannot open %s (0x%08lx)", path, hr); return false; }
    r.reader->SetStreamSelection((DWORD)MF_SOURCE_READER_ALL_STREAMS, FALSE);
    r.reader->SetStreamSelection((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, TRUE);
    IMFMediaType* type = nullptr;
    MFCreateMediaType(&type);
    type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
    hr = r.reader->SetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, type);
    type->Release();
    IMFMediaType* cur = nullptr;
    if (SUCCEEDED(hr)) hr = r.reader->GetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, &cur);
    if (SUCCEEDED(hr)) {
        MFGetAttributeSize(cur, MF_MT_FRAME_SIZE, &r.width, &r.height);
        UINT32 stride = 0;
        r.stride = SUCCEEDED(cur->GetUINT32(MF_MT_DEFAULT_STRIDE, &stride)) ? (LONG)stride : (LONG)(r.width * 4);
        cur->Release();
    }
    if (FAILED(hr) || !r.width || !r.height || r.width > 4096 || r.height > 4096) {
        Log("video: %s not usable (0x%08lx, %ux%u)", path, hr, r.width, r.height);
        return false;
    }
    r.frame = new BYTE[(size_t)r.width * r.height * 4];
    ReadAhead(r);
    if (!r.next || !TakeNext(r)) { Log("video: %s has no decodable frames", path); return false; }
    r.haveFrame = true;
    ReadAhead(r);
    Log("video: %s replaces the picture (%ux%u)", path, r.width, r.height);
    return true;
}

void* WINAPI BinkOpenHook(const char* name, DWORD flags)
{
    void* bink = g_binkOpen(name, flags);
    // Only the open for playback (0x675140, flags 0x8204000); 0x4137b0 opens
    // files without closing them, and a first open (0x4000) only selects the
    // sound track.
    if (!bink || !name || !(flags & 0x08000000)) return bink;
    for (Replacement& r : g_rep) {
        if (r.bink) continue;
        r.bink = bink;
        if (OpenReplacement(r, name)) g_pending = &r;
        else Release(r);
        break;
    }
    return bink;
}

int WINAPI BinkCopyToBufferHook(void* bink, void* dest, int pitch, DWORD height, DWORD x, DWORD y, DWORD flags)
{
    Replacement* r = Find(bink);
    if (!r || g_pending == r) return g_binkCopyToBuffer(bink, dest, pitch, height, x, y, flags);
    // Bink frame numbers are 1-based (BINK +0xc); take the last replacement frame
    // that starts at or before this frame's time.
    DWORD frameNum = ((DWORD*)bink)[3];
    LONGLONG target = (LONGLONG)((frameNum ? frameNum - 1 : 0) / r->binkFps * 1e7) + 50000;
    while (r->next && r->nextTime <= target) {
        TakeNext(*r);
        ReadAhead(*r);
    }
    UINT rowBytes = r->width * 4;
    for (UINT row = 0; row < r->height; row++) {
        DWORD* d = (DWORD*)((BYTE*)dest + (size_t)row * pitch);
        const DWORD* s = (const DWORD*)(r->frame + (size_t)row * rowBytes);
        for (UINT i = 0; i < r->width; i++) d[i] = s[i] | 0xFF000000;
    }
    return 0;
}

void WINAPI BinkCloseHook(void* bink)
{
    if (Replacement* r = Find(bink)) {
        Release(*r);
        g_videoTexture = nullptr;
    }
    g_binkClose(bink);
}

}  // namespace

void Video_Install()
{
    static bool done;
    if (done) return;
    done = true;
    HMODULE exe = GetModuleHandleA(nullptr);
    bool ok = PatchImport(exe, "binkw32.dll", "_BinkOpen@8", (void*)BinkOpenHook, (void**)&g_binkOpen) &&
              PatchImport(exe, "binkw32.dll", "_BinkCopyToBuffer@28", (void*)BinkCopyToBufferHook,
                          (void**)&g_binkCopyToBuffer) &&
              PatchImport(exe, "binkw32.dll", "_BinkClose@4", (void*)BinkCloseHook, (void**)&g_binkClose);
    Log("video: %s", ok ? "replacement videos enabled (Video\\<name>.mp4)" : "Bink imports not found, disabled");
}

void Video_AdjustTexture(UINT& w, UINT& h, DWORD usage, D3DFORMAT fmt)
{
    Replacement* r = g_pending;
    if (!r || usage != 0 || fmt != D3DFMT_A8R8G8B8) return;
    UINT bw = ((DWORD*)r->bink)[0], bh = ((DWORD*)r->bink)[1];
    if (w == Pow2(bw) && h == Pow2(bh)) {
        w = Pow2(r->width);
        h = Pow2(r->height);
    } else if (w == bw && h == bh) {
        w = r->width;
        h = r->height;
    } else {
        return;
    }
    // 0x675140 computed the UV scale for the Bink size just before; the quad is
    // built from it right after the texture is created.
    *(float*)kUvScaleU = (float)r->width / w;
    *(float*)kUvScaleV = (float)r->height / h;
    g_pending = nullptr;
    g_videoTexture = (IDirect3DBaseTexture9*)1;  // set by Video_TextureCreated
}

void Video_TextureCreated(IDirect3DBaseTexture9* tex)
{
    if (g_videoTexture == (IDirect3DBaseTexture9*)1) g_videoTexture = tex;
}

bool Video_IsReplacementTexture(IDirect3DBaseTexture9* tex) { return tex && tex == g_videoTexture; }
