// EAX through DSOAL.
//
// The game renders its 3D sound with DirectSound3D and EAX 2 reverb. It creates
// DirectSound through Creative's EAX.DLL (EAXDirectSoundCreate8), which creates
// the object with CoCreateInstance(CLSID_DirectSound8) - always the system
// dsound.dll, which has had no hardware 3D or EAX since Windows Vista, so the
// game switches EAX off.
//
// DSOAL (dsound.dll + dsoal-aldrv.dll, a DirectSound implementation on top of
// OpenAL Soft) emulates EAX and outputs stereo, headphones (HRTF) or surround.
// When its dsound.dll is in the game folder, we patch EAX.DLL's import of
// CoCreateInstance so that DirectSound objects come from DSOAL's class factory
// (DllGetClassObject). The object is created uninitialized, exactly like the COM
// path, so EAX.DLL initializes it as before.
//
// The audio options only offer EAX when the device supports it and 3D audio is
// on (config *(0x80c0c4): +0xc 3D audio, +0x10 EAX; setters 0x414020 and
// 0x414070, support check 0x413ff0). With [sound] eax=1 both are switched on
// through those setters once DSOAL reports EAX.

#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <string.h>
#include "sound.h"
#include "imports.h"

void Log(const char* fmt, ...);

namespace {

const GUID kClsidDirectSound = { 0x47d4d946, 0x62e8, 0x11cf, { 0x93, 0xbc, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00 } };
const GUID kClsidDirectSound8 = { 0x3901cc3f, 0x84b5, 0x4fa4, { 0xba, 0x35, 0xaa, 0x81, 0x72, 0xb8, 0xa0, 0x9b } };

typedef HRESULT(WINAPI* CoCreateInstance_t)(REFCLSID, LPUNKNOWN, DWORD, REFIID, LPVOID*);
typedef HRESULT(WINAPI* DllGetClassObject_t)(REFCLSID, REFIID, LPVOID*);

CoCreateInstance_t g_coCreateInstance;
DllGetClassObject_t g_dsoalGetClassObject;

const DWORD kConfig = 0x0080c0c4;
typedef void(__thiscall* SetAudioFlag_t)(void* config, int on);
typedef int(__thiscall* EaxAvailable_t)(void* config);
const SetAudioFlag_t Set3DAudio = (SetAudioFlag_t)0x00414020;
const SetAudioFlag_t SetEax = (SetAudioFlag_t)0x00414070;
const EaxAvailable_t EaxAvailable = (EaxAvailable_t)0x00413ff0;
const unsigned char kSet3DAudioCode[] = { 0x8B, 0x44, 0x24, 0x04, 0x56, 0x8B, 0xF1, 0x89, 0x46, 0x0C };
const unsigned char kSetEaxCode[] = { 0x80, 0x3D, 0x64, 0x43, 0xAF, 0x00, 0x01, 0x56, 0x8B, 0xF1, 0x75, 0x47 };
const unsigned char kEaxAvailableCode[] = { 0x80, 0x3D, 0x64, 0x43, 0xAF, 0x00, 0x01, 0x56, 0x8B, 0xF1, 0x75, 0x20 };
bool g_forceEax;
bool g_routed;  // DirectSound goes through DSOAL

HRESULT WINAPI CoCreateInstanceHook(REFCLSID clsid, LPUNKNOWN outer, DWORD ctx, REFIID iid, LPVOID* out)
{
    if (g_dsoalGetClassObject && !outer &&
        (IsEqualGUID(clsid, kClsidDirectSound8) || IsEqualGUID(clsid, kClsidDirectSound))) {
        IClassFactory* factory = nullptr;
        HRESULT hr = g_dsoalGetClassObject(clsid, IID_IClassFactory, (void**)&factory);
        if (SUCCEEDED(hr) && factory) {
            hr = factory->CreateInstance(nullptr, iid, out);
            factory->Release();
            static int logged;
            if (logged < 4) { logged++; Log("sound: DirectSound from DSOAL (0x%08lx)", hr); }
            if (SUCCEEDED(hr)) return hr;
        }
        Log("sound: DSOAL could not create DirectSound (0x%08lx), using the system one", hr);
    }
    return g_coCreateInstance(clsid, outer, ctx, iid, out);
}

}  // namespace

void Sound_Install(const char* gameDir)
{
    static bool done;
    if (done) return;
    HMODULE eax = GetModuleHandleA("eax.dll");
    if (!eax) return;  // not loaded yet, try again later
    done = true;

    char path[MAX_PATH];
    strcpy(path, gameDir);
    strcat(path, "dsound.dll");
    if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES) {
        Log("sound: no dsound.dll (DSOAL) in the game folder, EAX stays off");
        return;
    }
    HMODULE dsoal = LoadLibraryA(path);
    g_dsoalGetClassObject = dsoal ? (DllGetClassObject_t)GetProcAddress(dsoal, "DllGetClassObject") : nullptr;
    if (!g_dsoalGetClassObject) {
        Log("sound: %s is not usable, EAX stays off", path);
        return;
    }
    if (PatchImport(eax, "ole32.dll", "CoCreateInstance", (void*)CoCreateInstanceHook, (void**)&g_coCreateInstance))
    {
        g_routed = true;
        Log("sound: EAX.DLL creates DirectSound through DSOAL (%s)", path);
    }
    else
        Log("sound: EAX.DLL import of CoCreateInstance not found");
}

void Sound_SetForceEax(bool on) { g_forceEax = on; }

void Sound_OnFrame()
{
    static DWORD last;
    static bool codeOk, checked, done;
    if (!g_forceEax || !g_routed || done) return;
    if (!checked) {
        checked = true;
        codeOk = memcmp((void*)Set3DAudio, kSet3DAudioCode, sizeof(kSet3DAudioCode)) == 0 &&
                 memcmp((void*)SetEax, kSetEaxCode, sizeof(kSetEaxCode)) == 0 &&
                 memcmp((void*)EaxAvailable, kEaxAvailableCode, sizeof(kEaxAvailableCode)) == 0;
        if (!codeOk) Log("sound: unknown audio option code, EAX not switched on");
    }
    if (!codeOk || GetTickCount() - last < 2000) return;
    last = GetTickCount();
    __try {
        BYTE* config = *(BYTE**)kConfig;
        if (!config) return;
        if (*(int*)(config + 0x10)) { done = true; Log("sound: EAX is on"); return; }
        if (!*(int*)(config + 0xc)) Set3DAudio(config, 1);
        if (EaxAvailable(config)) {
            SetEax(config, 1);
            Log("sound: 3D audio and EAX switched on (%d)", *(int*)(config + 0x10));
            done = *(int*)(config + 0x10) != 0;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        done = true;
    }
}
