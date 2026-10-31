// Points the import table entry of `name` (imported from `dll` by module m) at `to`; returns the old target or NULL.
// Shared by the fixes that hook the game's imports (fileio.c, attrcache.c, bridgeprobe.c).
#pragma once
#include <windows.h>
#include <string.h>

static inline void *iat_hook(HMODULE m, const char *dll, const char *name, void *to) {
    unsigned char *b = (unsigned char *)m;
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(b + ((IMAGE_DOS_HEADER *)m)->e_lfanew);
    IMAGE_DATA_DIRECTORY *dd = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dd->VirtualAddress) return NULL;
    for (IMAGE_IMPORT_DESCRIPTOR *d = (IMAGE_IMPORT_DESCRIPTOR *)(b + dd->VirtualAddress); d->Name; d++) {
        if (_stricmp((char *)(b + d->Name), dll) != 0 || !d->OriginalFirstThunk) continue;
        IMAGE_THUNK_DATA *names = (IMAGE_THUNK_DATA *)(b + d->OriginalFirstThunk), *iat = (IMAGE_THUNK_DATA *)(b + d->FirstThunk);
        for (; names->u1.AddressOfData; names++, iat++) {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
            if (strcmp((char *)((IMAGE_IMPORT_BY_NAME *)(b + names->u1.AddressOfData))->Name, name) != 0) continue;
            void *old = (void *)iat->u1.Function;
            DWORD prot;
            if (!old || !VirtualProtect(&iat->u1.Function, sizeof(void *), PAGE_READWRITE, &prot)) return NULL;
            iat->u1.Function = (DWORD_PTR)to;
            VirtualProtect(&iat->u1.Function, sizeof(void *), prot, &prot);
            return old;
        }
    }
    return NULL;
}
