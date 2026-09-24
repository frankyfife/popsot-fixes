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
        Log("sound: EAX.DLL creates DirectSound through DSOAL (%s)", path);
    else
        Log("sound: EAX.DLL import of CoCreateInstance not found");
}
