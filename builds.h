// Game builds the fixes know. Each fix keeps its own table of addresses and byte hashes per build (same order as
// here) and uses the entry whose bytes match, so an unknown build is still refused by the byte checks.
//   0: 2026-09-17 Steam release (empire.retail.dll 69CE2041)
//   1: "pantheon" beta, 2026-10-01 (6AB978BD)
//   2: GOG version (6A564E92, compiled 2026-07-14)
#pragma once
#include <windows.h>
#include <tlhelp32.h>

#define NBUILDS 3

// for the log: the game version in plain words, by empire.retail.dll's TimeDateStamp (NULL = unknown build)
static inline const char *build_name(DWORD stamp) {
    return stamp == 0x69CE2041u ? "normal Steam version (2026-09-17)"
         : stamp == 0x6AB978BDu ? "\"pantheon\" beta (2026-10-01)"
         : stamp == 0x6A564E92u ? "GOG version"
         : NULL;
}

// FNV-1a of n bytes: the byte checks of the fixes' sites
static inline DWORD fnv1a(const void *p, size_t n) {
    const unsigned char *b = (const unsigned char *)p;
    DWORD h = 0x811C9DC5u;
    while (n--) h = (h ^ *b++) * 0x01000193u;
    return h;
}

// rva..rva+n lies inside the mapped image (a table entry for another build may point past a smaller one)
static inline int in_image(const void *m, DWORD rva, DWORD n) {
    const IMAGE_NT_HEADERS *nt = (const IMAGE_NT_HEADERS *)((const char *)m + ((const IMAGE_DOS_HEADER *)m)->e_lfanew);
    return rva < nt->OptionalHeader.SizeOfImage && n <= nt->OptionalHeader.SizeOfImage - rva;
}

// 1 if the thread was started inside the game's own code (the game module or the exe): only those run the game's code.
// Others set their own state and never run it (XAudio2's thread runs with DAZ/FTZ on: MXCSR 9FE0 seen in game;
// thread pool, Steam, EOS, driver threads).
static inline int game_thread(HANDLE h, HMODULE game) {
    static LONG(NTAPI *q)(HANDLE, int, PVOID, ULONG, PULONG);
    if (!q) q = (LONG(NTAPI *)(HANDLE, int, PVOID, ULONG, PULONG))(void *)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationThread");
    void *start = NULL;
    if (!q || q(h, 9 /* ThreadQuerySetWin32StartAddress */, &start, sizeof start, NULL) != 0) return 0;
    HMODULE m = NULL;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, start, &m)) return 0;
    return m == game || m == GetModuleHandleW(NULL);
}

// Game code changed while other game threads may run it: write() runs while every other game thread is suspended,
// unless one stands where busy(eip) says - then nothing is written and 0 comes back (try again next frame). All threads
// are found before any is suspended: one suspended inside LoadLibrary holds the loader's module lock, which the lookups
// take (x87fix_test: that hung). Nothing that takes a lock (heap, loader, CRT) may run in busy() or write().
// (From the Attila mod's gamerva.h.)
static inline int paused_write(HMODULE game, int (*busy)(UINT_PTR eip), void (*write)(void)) {
    HANDLE th[1024];
    int nth = 0, ns = 0, clear = 1;   // th[0..ns) are suspended
    DWORD pid = GetCurrentProcessId(), me = GetCurrentThreadId();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    THREADENTRY32 te = {.dwSize = sizeof te};
    for (BOOL ok = Thread32First(snap, &te); ok && nth < 1024; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != pid || te.th32ThreadID == me) continue;
        HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID);
        if (!h) continue;
        if (game_thread(h, game)) th[nth++] = h;
        else CloseHandle(h);
    }
    CloseHandle(snap);
    while (ns < nth && clear) {
        if (SuspendThread(th[ns]) == (DWORD)-1) { CloseHandle(th[ns]); th[ns] = th[--nth]; continue; }
        CONTEXT c = {.ContextFlags = CONTEXT_CONTROL};
        if (!GetThreadContext(th[ns++], &c) || busy(c.Eip)) clear = 0;
    }
    if (clear) write();
    for (int i = 0; i < nth; i++) {
        if (i < ns) ResumeThread(th[i]);
        CloseHandle(th[i]);
    }
    return clear;
}
