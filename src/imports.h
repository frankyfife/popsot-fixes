#pragma once
#include <windows.h>
#include <string.h>

// Replaces the import `name` from `dll` in module `mod` with `hook` and returns
// the previous target in `*original`. Imports are matched by name, which also
// works for forwarded functions (ole32!CoCreateInstance -> combase).
inline bool PatchImport(HMODULE mod, const char* dll, const char* name, void* hook, void** original)
{
    BYTE* base = (BYTE*)mod;
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(base + ((IMAGE_DOS_HEADER*)base)->e_lfanew);
    IMAGE_DATA_DIRECTORY dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return false;
    for (IMAGE_IMPORT_DESCRIPTOR* imp = (IMAGE_IMPORT_DESCRIPTOR*)(base + dir.VirtualAddress); imp->Name; imp++) {
        if (_stricmp((const char*)(base + imp->Name), dll) != 0 || !imp->OriginalFirstThunk) continue;
        IMAGE_THUNK_DATA* names = (IMAGE_THUNK_DATA*)(base + imp->OriginalFirstThunk);
        IMAGE_THUNK_DATA* funcs = (IMAGE_THUNK_DATA*)(base + imp->FirstThunk);
        for (; names->u1.AddressOfData; names++, funcs++) {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
            IMAGE_IMPORT_BY_NAME* ibn = (IMAGE_IMPORT_BY_NAME*)(base + names->u1.AddressOfData);
            if (strcmp((const char*)ibn->Name, name) != 0) continue;
            DWORD prot;
            VirtualProtect(&funcs->u1.Function, sizeof(void*), PAGE_READWRITE, &prot);
            *original = (void*)funcs->u1.Function;
            funcs->u1.Function = (DWORD)hook;
            VirtualProtect(&funcs->u1.Function, sizeof(void*), prot, &prot);
            return true;
        }
    }
    return false;
}
