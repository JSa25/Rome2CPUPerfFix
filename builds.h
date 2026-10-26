// Game builds the fixes know. Each fix keeps its own table of addresses and byte hashes per build (same order as
// here) and uses the entry whose bytes match, so an unknown build is still refused by the byte checks.
//   0: 2026-09-17 Steam release (empire.retail.dll 69CE2041)
//   1: "pantheon" beta, 2026-10-01 (6AB978BD)
#pragma once
#include <windows.h>

#define NBUILDS 2

// for the log: the game version in plain words, by empire.retail.dll's TimeDateStamp (NULL = unknown build)
static inline const char *build_name(DWORD stamp) {
    return stamp == 0x69CE2041u ? "normal Steam version (2026-09-17)"
         : stamp == 0x6AB978BDu ? "\"pantheon\" beta (2026-10-01)"
         : NULL;
}

// rva..rva+n lies inside the mapped image (a table entry for another build may point past a smaller one)
static inline int in_image(const void *m, DWORD rva, DWORD n) {
    const IMAGE_NT_HEADERS *nt = (const IMAGE_NT_HEADERS *)((const char *)m + ((const IMAGE_DOS_HEADER *)m)->e_lfanew);
    return rva < nt->OptionalHeader.SizeOfImage && n <= nt->OptionalHeader.SizeOfImage - rva;
}
