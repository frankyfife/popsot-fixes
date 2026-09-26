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
//
// Aspect ratio: 0x674d30 builds the video quad (pre-transformed vertices) over
// the whole back buffer, stretching the 4:3 videos on wide screens. With
// [video] keep_aspect=1 the quad is rebuilt at the largest size with the
// picture's own aspect ratio (4:3 intros pillarboxed, the 1.85:1 cutscenes
// letterboxed).

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
const DWORD kVideoQuad = 0x00af44d0;   // vertex buffer object, vertices at +8
const DWORD kCurrentBink = 0x00af44c8;
const DWORD kBackBufferW = 0x00ae1614, kBackBufferH = 0x00ae1618;
const DWORD kBuildQuad = 0x00674d30;
const DWORD kBuildQuadCalls[] = { 0x00674eeb, 0x00675485 };
bool g_keepAspect = true;

typedef void*(WINAPI* BinkOpen_t)(const char* name, DWORD flags);
typedef int(WINAPI* BinkCopyToBuffer_t)(void* bink, void* dest, int pitch, DWORD height, DWORD x, DWORD y,
                                        DWORD flags);
typedef void(WINAPI* BinkClose_t)(void* bink);
BinkOpen_t g_binkOpen;
BinkCopyToBuffer_t g_binkCopyToBuffer;
BinkClose_t g_binkClose;

// Decoding: a thread per video decodes the replacement ahead into a small
// queue of pictures, so the game thread only copies the one that belongs to
// the current Bink frame (decoding a 2560x1792 frame on the game thread took
// longer than a frame, and Bink's sound stuttered). The decoder's own NV12
// output is converted to BGRA here (BT.601 unless the file says BT.709, as
// ffmpeg encodes RGB input); Media Foundation's RGB32 conversion is the
// fallback for other formats.
const int kQueue = 4;
struct Replacement {
    void* bink;
    char path[MAX_PATH];
    UINT width, height;  // picture size
    double binkFps;
    BYTE* frame;         // current picture, top-down BGRA with alpha 255
    // decoding thread
    HANDLE thread, opened;
    volatile LONG stop, ok;
    BYTE* slot[kQueue];  // decoded pictures, handed over by swapping with frame
    LONGLONG slotTime[kQueue];
    volatile LONG slotFull[kQueue];
    int readPos;
};
const int kMaxReplacements = 4;
Replacement g_rep[kMaxReplacements];
Replacement* g_pending;               // opened, waiting for its texture
IDirect3DBaseTexture9* g_videoTexture;  // texture of the current replacement

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
    if (r.thread) {
        InterlockedExchange(&r.stop, 1);
        WaitForSingleObject(r.thread, INFINITE);
        CloseHandle(r.thread);
    }
    if (r.opened) CloseHandle(r.opened);
    for (BYTE* s : r.slot) delete[] s;
    delete[] r.frame;
    if (g_pending == &r) g_pending = nullptr;
    r = Replacement();
}

inline BYTE Clamp(int v) { return (BYTE)(v < 0 ? 0 : v > 255 ? 255 : v); }

// Decoder output layout.
struct Format {
    bool nv12;
    UINT codedHeight;  // rows of the Y plane (NV12: the UV plane follows)
    LONG stride;       // bytes per row (RGB32: negative = bottom-up)
    int cr, cgu, cgv, cb, cy, y0;  // YUV -> RGB, 8.8 fixed point
};

void ConvertNV12(const BYTE* p, LONG pitch, const Format& f, UINT w, UINT h, BYTE* out)
{
    const BYTE* uvPlane = p + (size_t)pitch * f.codedHeight;
    for (UINT y = 0; y < h; y++) {
        const BYTE* py = p + (size_t)pitch * y;
        const BYTE* puv = uvPlane + (size_t)pitch * (y / 2);
        BYTE* o = out + (size_t)w * 4 * y;
        for (UINT x = 0; x < w; x += 2) {
            int d = puv[x] - 128, e = puv[x + 1] - 128;
            int rr = f.cr * e + 128, gg = -f.cgu * d - f.cgv * e + 128, bb = f.cb * d + 128;
            for (UINT i = 0; i < 2 && x + i < w; i++) {
                int c = f.cy * (py[x + i] - f.y0);
                o[0] = Clamp((c + bb) >> 8);
                o[1] = Clamp((c + gg) >> 8);
                o[2] = Clamp((c + rr) >> 8);
                o[3] = 255;
                o += 4;
            }
        }
    }
}

// Copies a decoded sample into `out` (top-down BGRA).
bool ConvertSample(IMFSample* sample, const Format& f, UINT w, UINT h, BYTE* out)
{
    IMFMediaBuffer* buf = nullptr;
    if (FAILED(sample->ConvertToContiguousBuffer(&buf))) return false;
    bool done = false;
    BYTE* p = nullptr;
    LONG pitch = 0;
    IMF2DBuffer* b2 = nullptr;
    bool locked2d = SUCCEEDED(buf->QueryInterface(IID_PPV_ARGS(&b2))) && SUCCEEDED(b2->Lock2D(&p, &pitch));
    DWORD len = 0;
    if (!locked2d && SUCCEEDED(buf->Lock(&p, nullptr, &len))) pitch = f.stride;
    if (p) {
        if (f.nv12) {
            if (pitch > 0) {
                ConvertNV12(p, pitch, f, w, h, out);
                done = true;
            }
        } else {
            LONG abs = pitch < 0 ? -pitch : pitch;
            for (UINT y = 0; y < h; y++) {
                // Lock2D returns the top row with a signed pitch; Lock the buffer start.
                const BYTE* src = locked2d ? p + (LONG)y * pitch
                                           : p + (size_t)(pitch < 0 ? h - 1 - y : y) * abs;
                DWORD* d = (DWORD*)(out + (size_t)w * 4 * y);
                const DWORD* s = (const DWORD*)src;
                for (UINT i = 0; i < w; i++) d[i] = s[i] | 0xFF000000;
            }
            done = true;
        }
        if (locked2d) b2->Unlock2D();
        else buf->Unlock();
    }
    if (b2) b2->Release();
    buf->Release();
    return done;
}

// Opens the reader for r.path and chooses the output format: NV12, else RGB32.
IMFSourceReader* OpenReader(Replacement& r, Format& f)
{
    wchar_t wpath[MAX_PATH];
    MultiByteToWideChar(CP_ACP, 0, r.path, -1, wpath, MAX_PATH);
    IMFSourceReader* reader = nullptr;
    for (int pass = 0; pass < 2 && !reader; pass++) {
        f = Format();
        f.nv12 = pass == 0;
        IMFAttributes* attr = nullptr;
        MFCreateAttributes(&attr, 1);
        if (!f.nv12) attr->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
        HRESULT hr = MFCreateSourceReaderFromURL(wpath, attr, &reader);
        attr->Release();
        if (FAILED(hr)) { Log("video: cannot open %s (0x%08lx)", r.path, hr); return nullptr; }
        reader->SetStreamSelection((DWORD)MF_SOURCE_READER_ALL_STREAMS, FALSE);
        reader->SetStreamSelection((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, TRUE);
        IMFMediaType* type = nullptr;
        MFCreateMediaType(&type);
        type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        type->SetGUID(MF_MT_SUBTYPE, f.nv12 ? MFVideoFormat_NV12 : MFVideoFormat_RGB32);
        hr = reader->SetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, type);
        type->Release();
        IMFMediaType* cur = nullptr;
        if (SUCCEEDED(hr)) hr = reader->GetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, &cur);
        if (FAILED(hr)) {
            reader->Release();
            reader = nullptr;
            continue;
        }
        UINT cw = 0, ch = 0;
        MFGetAttributeSize(cur, MF_MT_FRAME_SIZE, &cw, &ch);
        r.width = cw;
        r.height = ch;
        MFVideoArea area;
        if (SUCCEEDED(cur->GetBlob(MF_MT_MINIMUM_DISPLAY_APERTURE, (UINT8*)&area, sizeof(area), nullptr)) &&
            area.Area.cx > 0 && area.Area.cy > 0 && (UINT)area.Area.cx <= cw && (UINT)area.Area.cy <= ch) {
            r.width = area.Area.cx;
            r.height = area.Area.cy;
        }
        f.codedHeight = ch;
        UINT32 stride = 0;
        f.stride = SUCCEEDED(cur->GetUINT32(MF_MT_DEFAULT_STRIDE, &stride)) ? (LONG)stride
                                                                            : (LONG)(f.nv12 ? cw : cw * 4);
        UINT32 matrix = 0, range = 0;
        cur->GetUINT32(MF_MT_YUV_MATRIX, &matrix);
        cur->GetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, &range);
        cur->Release();
        bool bt709 = matrix == MFVideoTransferMatrix_BT709, full = range == MFNominalRange_0_255;
        f.y0 = full ? 0 : 16;
        f.cy = full ? 256 : 298;
        if (bt709) { f.cr = full ? 403 : 459; f.cgu = full ? 48 : 55; f.cgv = full ? 120 : 136; f.cb = full ? 475 : 541; }
        else       { f.cr = full ? 359 : 409; f.cgu = full ? 88 : 100; f.cgv = full ? 183 : 208; f.cb = full ? 454 : 516; }
    }
    return reader;
}

DWORD WINAPI DecodeThread(void* param)
{
    Replacement& r = *(Replacement*)param;
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    Format f;
    IMFSourceReader* reader = OpenReader(r, f);
    bool first = true;
    int writePos = 0;
    if (reader && (!r.width || !r.height || r.width > 4096 || r.height > 4096)) {
        Log("video: %s not usable (%ux%u)", r.path, r.width, r.height);
        reader->Release();
        reader = nullptr;
    }
    while (reader && !r.stop) {
        if (!first && r.slotFull[writePos]) {  // queue full
            Sleep(1);
            continue;
        }
        DWORD stream, flags = 0;
        LONGLONG time = 0;
        IMFSample* sample = nullptr;
        HRESULT hr = reader->ReadSample((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, &stream, &flags, &time, &sample);
        if (FAILED(hr) || (flags & (MF_SOURCE_READERF_ENDOFSTREAM | MF_SOURCE_READERF_ERROR))) {
            if (sample) sample->Release();
            break;
        }
        if (!sample) continue;
        if (first) {
            // The first picture, before the game creates the texture.
            size_t size = (size_t)r.width * r.height * 4;
            r.frame = new BYTE[size];
            for (BYTE*& s : r.slot) s = new BYTE[size];
            bool ok = ConvertSample(sample, f, r.width, r.height, r.frame);
            sample->Release();
            first = false;
            if (!ok) break;
            InterlockedExchange(&r.ok, 1);
            SetEvent(r.opened);
            continue;
        }
        bool ok = ConvertSample(sample, f, r.width, r.height, r.slot[writePos]);
        sample->Release();
        if (!ok) continue;
        r.slotTime[writePos] = time;
        InterlockedExchange(&r.slotFull[writePos], 1);
        writePos = (writePos + 1) % kQueue;
    }
    SetEvent(r.opened);  // also when it failed before the first picture
    if (reader) reader->Release();
    CoUninitialize();
    return 0;
}

bool OpenReplacement(Replacement& r, const char* binkName)
{
    const char* exts[] = { ".mp4", ".mov", ".mkv" };
    bool found = false;
    for (const char* ext : exts) {
        strncpy(r.path, binkName, MAX_PATH - 8);
        r.path[MAX_PATH - 8] = 0;
        char* dot = strrchr(r.path, '.');
        char* slash = strrchr(r.path, '\\');
        if (dot && (!slash || dot > slash)) *dot = 0;
        strcat(r.path, ext);
        if (GetFileAttributesA(r.path) != INVALID_FILE_ATTRIBUTES) { found = true; break; }
    }
    if (!found) return false;

    // Frame rate of the Bink file (header: rate at 0x1c, divider at 0x20).
    r.binkFps = 29.97;
    if (FILE* f = fopen(binkName, "rb")) {
        DWORD hdr[9] = {};
        if (fread(hdr, 4, 9, f) == 9 && hdr[8]) r.binkFps = (double)hdr[7] / hdr[8];
        fclose(f);
    }

    static bool mfStarted;
    if (!mfStarted) {
        if (FAILED(MFStartup(MF_VERSION, MFSTARTUP_LITE))) { Log("video: Media Foundation not available"); return false; }
        mfStarted = true;
    }
    r.opened = CreateEventA(nullptr, TRUE, FALSE, nullptr);
    r.thread = CreateThread(nullptr, 0, DecodeThread, &r, 0, nullptr);
    if (!r.thread) return false;
    WaitForSingleObject(r.opened, 10000);
    if (!r.ok) { Log("video: %s has no decodable frames", r.path); return false; }
    Log("video: %s replaces the picture (%ux%u)", r.path, r.width, r.height);
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
    // Bink frame numbers are 1-based (BINK +0xc); take the last decoded picture
    // that starts at or before this frame's time.
    DWORD frameNum = ((DWORD*)bink)[3];
    LONGLONG target = (LONGLONG)((frameNum ? frameNum - 1 : 0) / r->binkFps * 1e7) + 50000;
    while (r->slotFull[r->readPos] && r->slotTime[r->readPos] <= target) {
        BYTE* t = r->frame;
        r->frame = r->slot[r->readPos];
        r->slot[r->readPos] = t;
        InterlockedExchange(&r->slotFull[r->readPos], 0);
        r->readPos = (r->readPos + 1) % kQueue;
    }
    UINT rowBytes = r->width * 4;
    for (UINT row = 0; row < r->height; row++)
        memcpy((BYTE*)dest + (size_t)row * pitch, r->frame + (size_t)row * rowBytes, rowBytes);
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

// Rebuilds the quad positions with the picture's aspect ratio. Vertices: 4 x 7
// floats (x, y, z, rhw, colour, u, v), order top-left, top-right,
// bottom-right, bottom-left.
void __cdecl BuildQuadHook()
{
    ((void(__cdecl*)())kBuildQuad)();
    if (!g_keepAspect) return;
    __try {
        BYTE* quad = *(BYTE**)kVideoQuad;
        DWORD* bink = *(DWORD**)kCurrentBink;
        float W = (float)*(DWORD*)kBackBufferW, H = (float)*(DWORD*)kBackBufferH;
        if (!quad || !bink || W <= 0 || H <= 0) return;
        float* v = *(float**)(quad + 8);
        // Largest size with square pixels that fits the screen, centred.
        float bw = (float)bink[0], bh = (float)bink[1];
        float s = W / bw < H / bh ? W / bw : H / bh;
        float x0 = (W - bw * s) / 2, y0 = (H - bh * s) / 2;
        float x1 = x0 + bw * s, y1 = y0 + bh * s;
        v[0] = x0;  v[1] = y0;
        v[7] = x1;  v[8] = y0;
        v[14] = x1; v[15] = y1;
        v[21] = x0; v[22] = y1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

}  // namespace

void Video_Install(bool keepAspect)
{
    g_keepAspect = keepAspect;
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

// Redirects both calls of the quad builder (applied once Direct3D is created,
// like the other code patches).
void Video_InstallCode()
{
    static bool done;
    if (done || !g_keepAspect) return;
    done = true;
    bool calls = true;
    for (DWORD site : kBuildQuadCalls)
        calls = calls && *(BYTE*)site == 0xE8 && site + 5 + *(DWORD*)(site + 1) == kBuildQuad;
    if (!calls) { Log("video: unknown quad code, aspect ratio not kept"); return; }
    for (DWORD site : kBuildQuadCalls) {
        DWORD prot;
        VirtualProtect((void*)(site + 1), 4, PAGE_EXECUTE_READWRITE, &prot);
        *(DWORD*)(site + 1) = (DWORD)BuildQuadHook - (site + 5);
        VirtualProtect((void*)(site + 1), 4, prot, &prot);
    }
    Log("video: videos keep their aspect ratio");
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
